// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ui_update_queue.h
 * @brief Thread-safe UI update queue for LVGL
 *
 * This module provides a safe mechanism for scheduling UI updates from any thread.
 * Updates are queued and processed at the START of each lv_timer_handler cycle,
 * BEFORE rendering begins. This guarantees that widget modifications never
 * happen during the render phase.
 *
 * Architecture:
 * 1. Any thread can queue updates via helix::ui::queue_update()
 * 2. Updates accumulate in a thread-safe queue
 * 3. At the start of each frame (via LVGL timer), all pending updates are processed
 * 4. Rendering happens AFTER all updates are applied
 *
 * This is similar to React's batched state updates - changes are queued and
 * applied together at a safe point.
 *
 * Usage:
 * @code
 * // From any thread (WebSocket callback, async operation, etc.):
 * helix::ui::queue_update([](void*) {
 *     lv_subject_set_int(&my_subject, new_value);
 *     lv_label_set_text(label, "Updated!");
 * });
 *
 * // With captured data:
 * auto* data = new MyData{value, text};
 * helix::ui::queue_update([](void* user_data) {
 *     auto* d = static_cast<MyData*>(user_data);
 *     lv_subject_set_int(&my_subject, d->value);
 *     delete d;
 * }, data);
 * @endcode
 */

#pragma once

#include "lvgl/lvgl.h"

#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace helix::ui {

/**
 * @brief Callback type for queued updates
 */
using UpdateCallback = std::function<void()>;

/**
 * @brief Callback entry with optional debug tag
 *
 * The tag identifies which subsystem queued the callback, making crash
 * reports actionable when a crash occurs inside process_pending().
 * Tags must be string literals (pointer stored, not copied).
 */
struct TaggedCallback {
    const char* tag = nullptr;
    UpdateCallback callback;
};

/**
 * @brief Thread-safe UI update queue
 *
 * Singleton that manages pending UI updates. Call init() once at startup
 * to install a high-priority timer that processes updates every lv_timer_handler() cycle.
 *
 * Key insight: Using LV_EVENT_REFR_START doesn't work because it only fires when
 * LVGL decides to render. If nothing invalidates the display, the queue never drains.
 * Instead, we use a highest-priority timer that fires every lv_timer_handler() call,
 * ensuring callbacks execute promptly regardless of render state.
 */
class UpdateQueue {
  public:
    /**
     * @brief Get singleton instance
     */
    static UpdateQueue& instance() {
        static UpdateQueue instance;
        return instance;
    }

    /**
     * @brief Initialize the update queue (call once at startup)
     *
     * Creates a highest-priority timer that processes pending updates
     * every lv_timer_handler() cycle, BEFORE the render timer runs.
     */
    void init() {
        if (initialized_)
            return;

        shut_down_ = false;

        // Create a timer that fires every lv_timer_handler() cycle
        // Period of 1ms ensures it runs frequently (LVGL processes all ready timers)
        // Created early at init, so it's near the head of the timer list
        timer_ = lv_timer_create(timer_cb, 1, this);
        if (!timer_) {
            spdlog::error("[UpdateQueue] Failed to create timer!");
            return;
        }

        // Register the current callback tag pointer with the crash handler
        // so crashes inside process_pending() identify which callback was running
        crash_handler::register_callback_tag_ptr(&current_tag_);
        // Also register the last-N completed tags so post-callback crashes
        // (heap corruption detonating a few callbacks later, or on the next
        // main-thread malloc) can pin which callback planted the corruption.
        // Ring is written on every callback exit; crash handler walks it
        // newest→oldest and emits queue_prev / queue_prev2 / ... lines.
        // High-frequency repeats (e.g. TSM::update_subjects per WebSocket tick)
        // coalesce into a single slot with a count, preserving runway for
        // distinct callbacks above.
        crash_handler::register_previous_tag_ring(previous_tag_ring_,
                                                  previous_tag_count_ring_,
                                                  kPreviousTagRingSize,
                                                  &previous_tag_next_);

        initialized_ = true;
        spdlog::debug("[UpdateQueue] Initialized - timer created for queue drain");
    }

    /**
     * @brief Queue an update for processing
     *
     * Thread-safe. Can be called from any thread.
     * The callback will be executed on the main LVGL thread before rendering.
     *
     * @param callback Function to execute
     */
    void queue(UpdateCallback callback) {
        queue(nullptr, std::move(callback));
    }

    /**
     * @brief Queue an update with a debug tag
     *
     * The tag identifies the caller for crash diagnostics. If a crash occurs
     * inside process_pending(), the crash handler writes the tag to crash.txt
     * so we know which callback was executing.
     *
     * @param tag String literal identifying the caller (e.g., "ToastManager::dismiss")
     * @param callback Function to execute
     */
    // Enqueue work for the main thread.
    //
    // While a scoped_freeze() is held, callbacks are BUFFERED into
    // frozen_buffer_ instead of dropped; ScopedFreeze::~ splices the buffer
    // into pending_ when the last freeze releases, so the work fires on the
    // following process_pending tick. The freeze still serializes BG threads
    // against drain+destroy (anything enqueued during freeze runs after the
    // freeze ends, by which time the drain has flushed and any destroy has
    // completed) — AsyncLifetimeGuard's generation check on the apply side
    // handles UAF if the owner died during the freeze window. shut_down_
    // still drops (post-shutdown enqueues are unrecoverable). Replaces the
    // older defer/defer_critical split: with buffer-not-drop the two were
    // functionally identical, so there is now one path.
    void queue(const char* tag, UpdateCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shut_down_) {
            if (tag) spdlog::warn("[UpdateQueue] DROPPED (shutdown): {}", tag);
            return;
        }
        if (freeze_depth_ > 0) {
            frozen_buffer_.push({tag, std::move(callback)});
            if (tag) spdlog::trace("[UpdateQueue] Buffered (frozen): {} (buffered={})",
                                   tag, frozen_buffer_.size());
            return;
        }
        pending_.push({tag, std::move(callback)});
        if (tag) spdlog::trace("[UpdateQueue] Enqueued: {} (pending={})", tag, pending_.size());
    }

    /**
     * @brief Shutdown and cleanup
     *
     * Nullifies the timer pointer and clears the pending queue to prevent
     * stale callbacks from executing after objects they reference are
     * destroyed. The actual LVGL timer is freed by lv_deinit().
     */
    void shutdown() {
        // Drain pending callbacks while panels are still alive, then gate off
        // new enqueues so background threads (libhv WebSocket) that arrive late
        // silently discard instead of pushing stale panel pointers.
        process_pending();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initialized_ = false;
            shut_down_ = true;
            std::queue<TaggedCallback>().swap(pending_); // Discard any stragglers
        }
        timer_ = nullptr;
    }

    /**
     * @brief Process all pending callbacks immediately
     *
     * Call before destroying objects that may be referenced by queued callbacks.
     * Deferred observer callbacks (from observe_int_sync) capture raw panel
     * pointers; if those callbacks run after the panel is destroyed, they
     * crash with use-after-free. Draining the queue while pointers are still
     * valid ensures those callbacks execute safely.
     *
     * @note Must be called from the main LVGL thread.
     */
    void drain() {
        process_pending();
    }

    /**
     * @brief RAII guard that silently discards queued callbacks
     *
     * Use around drain()+destroy sequences to prevent the WebSocket background
     * thread from queueing new callbacks between drain() and widget destruction.
     * While frozen, queue() buffers callbacks into frozen_buffer_ instead of
     * pending_. On the last ScopedFreeze destruction the buffer is spliced
     * into pending_, so deferred work resumes on the following
     * process_pending tick. shut_down_ still drops (post-shutdown enqueues
     * are unrecoverable).
     *
     * Reference-counted: nested freezes are safe. The queue only unfreezes
     * (and splices the buffer) when the last ScopedFreeze is destroyed.
     */
    class ScopedFreeze {
    public:
        explicit ScopedFreeze(UpdateQueue& q, const char* caller = nullptr) : q_(q), caller_(caller) {
            int depth;
            {
                std::lock_guard<std::mutex> lock(q_.mutex_);
                depth = ++q_.freeze_depth_;
            }
            spdlog::trace("[UpdateQueue] FREEZE depth={} caller={}",
                         depth, caller_ ? caller_ : "unknown");
        }
        ~ScopedFreeze() {
            size_t spliced = 0;
            int depth;
            {
                std::lock_guard<std::mutex> lock(q_.mutex_);
                depth = --q_.freeze_depth_;
                if (depth == 0) {
                    // Splice buffered work into pending so it fires on the next
                    // process_pending tick. AsyncLifetimeGuard's generation
                    // check guards against UAF if the owner died during freeze.
                    spliced = q_.frozen_buffer_.size();
                    while (!q_.frozen_buffer_.empty()) {
                        q_.pending_.push(std::move(q_.frozen_buffer_.front()));
                        q_.frozen_buffer_.pop();
                    }
                }
            }
            spdlog::trace("[UpdateQueue] THAW depth={} caller={} spliced={}",
                         depth, caller_ ? caller_ : "unknown", spliced);
        }
        ScopedFreeze(const ScopedFreeze&) = delete;
        ScopedFreeze& operator=(const ScopedFreeze&) = delete;
    private:
        UpdateQueue& q_;
        const char* caller_;
    };

    /**
     * @brief Create a scoped freeze guard
     *
     * While the returned guard is alive, queue() buffers all callbacks into
     * frozen_buffer_. Use before drain()+destroy to close the race window
     * where background threads can enqueue callbacks targeting widgets about
     * to be destroyed: drain() flushes pre-freeze work, the freeze diverts
     * post-freeze work into the buffer, the destroy completes, and when the
     * guard goes out of scope the buffer is spliced back into pending_.
     * Generation tokens on the apply side handle UAF if the owner died.
     */
    [[nodiscard]] ScopedFreeze scoped_freeze(const char* caller = nullptr) {
        return ScopedFreeze(*this, caller);
    }

    /**
     * @brief Pause the update queue timer
     *
     * Prevents the timer from firing during lv_timer_handler() calls.
     * Used by test infrastructure to break the infinite restart chain
     * where UpdateQueue callbacks trigger subject changes that create
     * new period-0 timers.
     */
    void pause_timer() {
        if (timer_) {
            lv_timer_pause(timer_);
        }
    }

    /**
     * @brief Resume the update queue timer
     *
     * Re-enables the timer after it was paused.
     */
    void resume_timer() {
        if (timer_) {
            lv_timer_resume(timer_);
        }
    }

  private:
    friend class UpdateQueueTestAccess;
    UpdateQueue() = default;
    ~UpdateQueue() {
        shutdown();
    }

    // Non-copyable
    UpdateQueue(const UpdateQueue&) = delete;
    UpdateQueue& operator=(const UpdateQueue&) = delete;

    /**
     * @brief Timer callback - processes all pending updates
     *
     * Called by LVGL on every lv_timer_handler() cycle due to highest priority.
     * Runs BEFORE the render timer, ensuring updates are applied before drawing.
     */
    static void timer_cb(lv_timer_t* timer) {
        auto* self = static_cast<UpdateQueue*>(lv_timer_get_user_data(timer));
        if (self && self->initialized_) {
            self->process_pending();
        }
    }

    void process_pending() {
        // Move pending updates to local queue to minimize lock time
        std::queue<TaggedCallback> to_process;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(to_process, pending_);
        }

        // Execute all pending updates - safe because render hasn't started yet
        while (!to_process.empty()) {
            try {
                auto& entry = to_process.front();
                current_tag_ = entry.tag;
                entry.callback();
            } catch (const std::exception& e) {
                spdlog::error("[UpdateQueue] Exception in queued callback: {}", e.what());
            } catch (...) {
                spdlog::error("[UpdateQueue] Unknown exception in queued callback");
            }
            // Retain the N most-recently-completed tags so a post-callback
            // crash (heap corruption detonating on the next main-thread malloc,
            // or a few callbacks later) still names the prior sequence.
            // current_tag_ signals "inside a callback" vs "between / after".
            //
            // Coalesce consecutive identical tags: if the just-finished tag
            // matches the newest slot, bump that slot's count instead of
            // advancing. Tags are string literals so pointer-equality is
            // sufficient and signal-safe. This keeps high-frequency producers
            // (e.g. TSM::update_subjects per WebSocket tick) from flooding the
            // ring and burying genuinely distinct callbacks.
            if (current_tag_) {
                unsigned int next = previous_tag_next_;
                if (next > 0 &&
                    previous_tag_ring_[(next - 1) % kPreviousTagRingSize] == current_tag_) {
                    previous_tag_count_ring_[(next - 1) % kPreviousTagRingSize] += 1;
                } else {
                    previous_tag_ring_[next % kPreviousTagRingSize] = current_tag_;
                    previous_tag_count_ring_[next % kPreviousTagRingSize] = 1;
                    previous_tag_next_ = next + 1;
                }
            }
            current_tag_ = nullptr;
            to_process.pop();
        }
    }

    std::mutex mutex_;
    std::queue<TaggedCallback> pending_;
    // Callbacks enqueued while freeze_depth_ > 0 land here; ScopedFreeze::~
    // splices them back to pending_ when the depth returns to 0. Protected
    // by mutex_.
    std::queue<TaggedCallback> frozen_buffer_;
    lv_timer_t* timer_ = nullptr;
    bool initialized_ = false;
    bool shut_down_ = false;
    // Protected by mutex_ — accessed from queue() and ScopedFreeze ctor/dtor.
    int freeze_depth_ = 0;

    /// Tag of the currently executing callback (read by crash handler).
    /// Only written from the main LVGL thread; read by the crash signal handler.
    /// Volatile ensures the signal handler sees the current value, not a cached one.
    static inline volatile const char* current_tag_ = nullptr;

    /// Ring of the N most-recently-completed callback tags. The crash handler
    /// walks this newest→oldest to name the prior callback sequence. Useful
    /// when the crashing instruction ran AFTER process_pending() returned but
    /// was corrupted by a prior callback, or when the corruption cascades a
    /// few callbacks later. Slot `(previous_tag_next_ - 1) % N` is newest.
    /// `previous_tag_count_ring_[i]` records how many consecutive callbacks
    /// shared slot `i`'s tag — coalesce keeps the ring representative when
    /// high-frequency producers (TSM, etc.) dominate the queue.
    static constexpr unsigned int kPreviousTagRingSize = 4;
    static inline volatile const char* previous_tag_ring_[kPreviousTagRingSize] = {};
    static inline volatile uint32_t previous_tag_count_ring_[kPreviousTagRingSize] = {};
    static inline volatile unsigned int previous_tag_next_ = 0;

  public:
    /**
     * @brief Get the tag of the currently executing callback
     *
     * Returns nullptr if no callback is running. Strips volatile at the API
     * boundary since callers are normal (non-signal-handler) code.
     */
    static const char* current_callback_tag() {
        return const_cast<const char*>(current_tag_);
    }

    /**
     * @brief Get the tag of the most-recently-completed callback.
     *
     * Non-null once any callback has finished. Read by the crash handler
     * when current_callback_tag() is null — helps pin crashes that detonate
     * on the next main-thread malloc after a corruption-causing callback
     * has already returned.
     */
    static const char* previous_callback_tag() {
        if (previous_tag_next_ == 0) return nullptr;
        unsigned int idx = (previous_tag_next_ - 1) % kPreviousTagRingSize;
        return const_cast<const char*>(previous_tag_ring_[idx]);
    }
};

/**
 * @brief Queue a UI update for safe execution
 *
 * This is the primary API for scheduling UI updates from any thread.
 * Updates are guaranteed to execute BEFORE rendering, avoiding the
 * "Invalidate area is not allowed during rendering" assertion.
 *
 * @param callback Function to execute on the main thread
 */
inline void queue_update(UpdateCallback callback) {
    UpdateQueue::instance().queue(std::move(callback));
}

/**
 * @brief Queue a tagged UI update for safe execution
 *
 * Same as queue_update() but with a debug tag for crash diagnostics.
 *
 * @param tag String literal identifying the caller (e.g., "PrinterState::set_temp")
 * @param callback Function to execute on the main thread
 */
inline void queue_update(const char* tag, UpdateCallback callback) {
    UpdateQueue::instance().queue(tag, std::move(callback));
}

/**
 * @brief Queue a UI update with data
 *
 * Convenience wrapper for updates that need to pass data.
 * The data is captured and passed to the callback.
 *
 * @tparam T Type of data to pass
 * @param data Data to pass to callback (moved into queue)
 * @param callback Function to execute with data
 */
template <typename T> void queue_update(std::unique_ptr<T> data, std::function<void(T*)> callback) {
    // Capture data and callback in a lambda
    T* raw_ptr = data.release(); // Transfer ownership
    queue_update([raw_ptr, callback = std::move(callback)]() {
        std::unique_ptr<T> owned(raw_ptr); // Reclaim ownership for RAII
        callback(owned.get());
    });
}

/**
 * @brief Initialize the UI update queue
 *
 * Call this once during application startup, AFTER lv_init() but BEFORE
 * creating any UI elements. This ensures the processing timer has highest
 * priority and runs before other timers.
 */
inline void update_queue_init() {
    UpdateQueue::instance().init();
}

/**
 * @brief Shutdown the UI update queue
 *
 * Call this during application shutdown, BEFORE lv_deinit().
 */
inline void update_queue_shutdown() {
    UpdateQueue::instance().shutdown();
}

/**
 * @brief Drop-in replacement for lv_async_call
 *
 * Has the EXACT same signature as lv_async_call() but uses the UI update queue
 * to ensure callbacks run BEFORE rendering, not during. Exceptions thrown by
 * callbacks are caught and logged by UpdateQueue::process_pending().
 *
 * Migration: Simply replace `lv_async_call(` with `async_call(`
 *
 * @param async_xcb Callback function (same signature as lv_async_call)
 * @param user_data User data passed to callback
 * @return LV_RESULT_OK always (queue never fails)
 */
inline lv_result_t async_call(lv_async_cb_t async_xcb, void* user_data) {
    queue_update([async_xcb, user_data]() {
        if (async_xcb) {
            async_xcb(user_data);
        }
    });
    return LV_RESULT_OK;
}

// ============================================================================
// Widget-safe overloads
//
// These wrap the base API with an lv_obj_is_valid() guard so async callbacks
// that outlive their widget are silently dropped instead of crashing.
// ============================================================================

/**
 * @brief Queue a UI update with data and widget guard
 *
 * Same as queue_update<T> but validates the widget before invoking the callback.
 * If the widget has been destroyed by the time the callback executes, it is
 * silently skipped and the data is freed via RAII.
 *
 * @tparam T    Type of data to pass
 * @tparam F    Callback type: void(lv_obj_t*, T*)
 * @param widget Widget that must still be valid when callback fires
 * @param data   Data to pass to callback (moved into queue)
 * @param callback Function to execute with validated widget and data
 */
template <typename T, typename F>
void queue_update(lv_obj_t* widget, std::unique_ptr<T> data, F&& callback) {
    T* raw_ptr = data.release();
    queue_update([widget, raw_ptr, cb = std::forward<F>(callback)]() {
        std::unique_ptr<T> owned(raw_ptr); // RAII: always freed
        if (!lv_obj_is_valid(widget)) {
            spdlog::debug("[UpdateQueue] Widget-safe guard: widget destroyed, skipping callback");
            return;
        }
        cb(widget, owned.get());
    });
}

/**
 * @brief Queue a widget update with no extra data
 *
 * Convenience wrapper for updates that only need the widget pointer.
 * The callback is skipped if the widget is no longer valid.
 *
 * @tparam F Callback type: void(lv_obj_t*)
 * @param widget Widget that must still be valid when callback fires
 * @param callback Function to execute with validated widget
 */
template <typename F> void queue_widget_update(lv_obj_t* widget, F&& callback) {
    queue_update([widget, cb = std::forward<F>(callback)]() {
        if (!lv_obj_is_valid(widget)) {
            spdlog::debug("[UpdateQueue] Widget-safe guard: widget destroyed, skipping callback");
            return;
        }
        cb(widget);
    });
}

/**
 * @brief Widget-safe drop-in replacement for lv_async_call
 *
 * Same as async_call(cb, user_data) but validates the widget first.
 * If the widget is destroyed before the callback fires, the callback is skipped.
 *
 * @param widget Widget that must still be valid when callback fires
 * @param async_xcb Callback function (same signature as lv_async_call)
 * @param user_data User data passed to callback
 * @return LV_RESULT_OK always (queue never fails)
 */
inline lv_result_t async_call(lv_obj_t* widget, lv_async_cb_t async_xcb, void* user_data) {
    queue_update([widget, async_xcb, user_data]() {
        if (!lv_obj_is_valid(widget)) {
            spdlog::debug("[UpdateQueue] Widget-safe guard: widget destroyed, skipping async_call");
            return;
        }
        if (async_xcb) {
            async_xcb(user_data);
        }
    });
    return LV_RESULT_OK;
}

} // namespace helix::ui

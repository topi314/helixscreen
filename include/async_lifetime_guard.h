// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file async_lifetime_guard.h
 * @brief Generation-counter-based async callback safety
 *
 * Provides a lightweight mechanism to detect whether an object is still valid
 * when a deferred callback fires. The guard lives in the protected object;
 * lambdas capture a LifetimeToken (which holds a shared_ptr to the generation
 * counter, NOT a pointer to the guard itself). This makes the check safe even
 * if the guard has been destroyed before the callback executes.
 *
 * Usage:
 * @code
 * class MyOverlay {
 *     helix::AsyncLifetimeGuard guard_;
 *
 *     void start_async_work() {
 *         // Callback will silently skip if overlay is dismissed before it fires
 *         guard_.defer("MyOverlay::on_result", [this]() {
 *             update_ui_with_result();
 *         });
 *     }
 *
 *     ~MyOverlay() {
 *         // guard_ destructor calls invalidate(), expiring all outstanding tokens
 *     }
 * };
 * @endcode
 */

#pragma once

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <tuple>
#include <utility>

namespace helix {

class AsyncLifetimeGuard;

namespace internal {

/// Capture the calling thread as the "main" thread (the LVGL/render thread).
/// Call exactly once, as the very first thing in main(), before any thread
/// that uses LifetimeToken can spawn. Subsequent calls are silently ignored.
void set_main_thread_id() noexcept;

/// Returns true if the calling thread is the recorded main thread, OR if
/// set_main_thread_id() has not yet run (returns true conservatively during
/// early init so the bg-thread detector doesn't fire false positives).
bool on_main_thread() noexcept;

/// Report a "tok.expired() called from a bg thread while owner is alive"
/// anti-pattern hit. Per-thread first-fire-only via a small TLS seen-set,
/// so each unique callsite (LR) reports at most once per thread per session.
///
/// Captures `__builtin_return_address(0)` *inside* this function (which is
/// noinline). With `expired()` always inlined into its caller F, the
/// trampoline's caller is F itself, and the captured LR is the position in
/// F where the inlined `tok.expired()` lives — i.e. the original source
/// location. If we captured the LR in the inline `expired()` and passed it
/// in, the compiler would substitute F's caller's LR after inlining, which
/// is the wrong frame for the audit. Resolve the captured LR with addr2line.
///
/// In strict mode (HELIX_STRICT_BG_THREAD_CHECK=1 env or
/// set_strict_bg_check(true)), this aborts after warning so CI / tests
/// fail fast on any new instance of the anti-pattern.
void report_bg_expired_check() noexcept;

/// Enable strict mode programmatically — meant for HelixTestFixture so test
/// runs assert any L081 anti-pattern instead of silently warning. Production
/// code should never call this; only the env var path is safe in user builds.
void set_strict_bg_check(bool enabled) noexcept;

} // namespace internal

/**
 * @brief Lightweight, copyable token captured in lambdas to check validity
 *
 * Holds a shared_ptr to the generation counter (NOT to the guard), so the
 * token remains safe to query even after the guard is destroyed.
 */
class LifetimeToken {
  public:
    /**
     * @brief Check if the generation has advanced past the snapshot
     * @return true if the owning object has been invalidated/destroyed
     *
     * 3XNZQB2R / cluster:pstat-async-delete Mechanism C detector: if the
     * owner is still alive (returning false) AND we're on a non-main
     * thread, flag the callsite as a likely L081 anti-pattern hit
     * (`tok.expired()` check followed by inline LVGL mutation on a bg
     * thread). The correct pattern is `tok.defer([this]() { ... })`,
     * which marshals the body to the main thread before the check.
     * See `feedback_token_defer_required.md` and the audit pending in
     * `project_l081_recurrence_post_840.md`.
     */
    [[gnu::always_inline]] inline bool expired() const {
        bool is_expired = gen_->load(std::memory_order_acquire) != snapshot_;
        if (!is_expired && !internal::on_main_thread()) {
            // LR capture is inside report_bg_expired_check() (noinline) so
            // that the captured address is the position in our caller where
            // this inline expired() lives — see comment on the declaration.
            internal::report_bg_expired_check();
        }
        return is_expired;
    }

    /**
     * @brief Convenience: true if NOT expired (object still alive)
     */
    explicit operator bool() const {
        return !expired();
    }

    /**
     * @brief Queue a guarded callback without accessing the owning object
     *
     * Use this from background thread callbacks instead of lifetime_.defer()
     * to avoid a TOCTOU race where `this` (and thus `lifetime_`) is destroyed
     * between the tok.expired() check and the lifetime_.defer() call (#707).
     *
     * The token holds its own shared_ptr to the generation counter, so it
     * remains safe to use even after the guard (and its owner) are destroyed.
     */
    template <typename F> void defer(F&& fn) const {
        auto gen = gen_;
        auto snapshot = snapshot_;
        helix::ui::queue_update([gen, snapshot, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot)
                return;
            f();
        });
    }

    /// Tagged variant for crash diagnostics
    template <typename F> void defer(const char* tag, F&& fn) const {
        auto gen = gen_;
        auto snapshot = snapshot_;
        helix::ui::queue_update(tag, [gen, snapshot, tag, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                spdlog::trace("[LifetimeToken] Skipped expired callback: {}",
                              tag ? tag : "unknown");
                return;
            }
            f();
        });
    }

  private:
    friend class AsyncLifetimeGuard;

    LifetimeToken(std::shared_ptr<std::atomic<uint64_t>> gen, uint64_t snapshot)
        : gen_(std::move(gen)), snapshot_(snapshot) {}

    std::shared_ptr<std::atomic<uint64_t>> gen_;
    uint64_t snapshot_;
};

/**
 * @brief Owned by the protected object; produces tokens and defers callbacks
 *
 * Non-copyable, non-movable. Destructor calls invalidate() to expire all
 * outstanding tokens. The defer() methods capture a shared_ptr to the
 * generation counter (not `this`), so the lambda is safe even if the guard
 * is destroyed before it fires.
 */
class AsyncLifetimeGuard {
  public:
    AsyncLifetimeGuard() : gen_(std::make_shared<std::atomic<uint64_t>>(0)) {}

    ~AsyncLifetimeGuard() {
        invalidate();
    }

    // Non-copyable, non-movable
    AsyncLifetimeGuard(const AsyncLifetimeGuard&) = delete;
    AsyncLifetimeGuard& operator=(const AsyncLifetimeGuard&) = delete;
    AsyncLifetimeGuard(AsyncLifetimeGuard&&) = delete;
    AsyncLifetimeGuard& operator=(AsyncLifetimeGuard&&) = delete;

    /**
     * @brief Capture the current generation as a token
     *
     * The token can be copied into lambdas and checked later. It will report
     * expired() == true once invalidate() is called (or the guard is destroyed).
     */
    LifetimeToken token() const {
        return LifetimeToken(gen_, gen_->load(std::memory_order_acquire));
    }

    /**
     * @brief Advance the generation counter, expiring all outstanding tokens
     *
     * Safe to call multiple times. Each call expires tokens from every
     * previous generation.
     */
    void invalidate() {
        gen_->fetch_add(1, std::memory_order_release);
    }

    /**
     * @brief Queue a callback that is skipped if the guard has been invalidated
     *
     * Captures a shared_ptr to the generation counter and a snapshot of the
     * current generation. When the callback fires, it compares the snapshot
     * to the current generation; if they differ, the callback is silently
     * skipped.
     *
     * @tparam F Callable with signature void()
     * @param fn The callback to defer
     */
    template <typename F> void defer(F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        helix::ui::queue_update([gen, snapshot, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                return;
            }
            f();
        });
    }

    /**
     * @brief Queue a tagged callback that is skipped if the guard has been invalidated
     *
     * Same as defer(fn), but the tag is passed to the UpdateQueue for crash
     * diagnostics. If the callback is skipped, a trace log is emitted with the tag.
     *
     * @tparam F Callable with signature void()
     * @param tag String literal identifying the caller (for crash diagnostics)
     * @param fn The callback to defer
     */
    template <typename F> void defer(const char* tag, F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        helix::ui::queue_update(tag, [gen, snapshot, tag, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                spdlog::trace("[AsyncLifetimeGuard] Skipped expired callback: {}",
                              tag ? tag : "unknown");
                return;
            }
            f();
        });
    }

    /**
     * @brief Wrap a background-thread callback so its body always fires on the main thread
     *
     * Returns a callable suitable to pass directly to HTTP / WebSocket / HttpExecutor /
     * std::thread APIs. When the underlying API invokes the returned callable on a bg
     * thread, the supplied `fn` is queued via `defer(tag, ...)` — fn always runs on the
     * main thread, after a generation-guard re-check, with `this` guaranteed valid.
     * Arguments are forwarded by-value (decayed) into the deferred lambda so callers
     * never see a dangling reference to a temporary on the bg-thread side.
     *
     * Use this in preference to writing `[this, tok = lifetime_.token()](...) {
     * if (tok.expired()) return; ... }` by hand — that idiom is the L081 anti-pattern
     * the strict-mode detector aborts on.
     *
     * Pattern:
     * @code
     * api_->rest().wled_get_strips(
     *     lifetime_.bg_cb("LedController::wled_get_strips",
     *                     [this](const RestResponse& resp) {
     *                         // runs on main; safe to touch members
     *                         apply_strips(resp);
     *                     }),
     *     [](const MoonrakerError& err) { ... });
     * @endcode
     *
     * If your callback wants to do bg-side work first (parsing JSON before deferring
     * member mutations), keep using the longhand `tok.defer("Tag", [...] { ... })`
     * pattern explicitly — bg_cb defers the *whole* call, so it trades minimum-bg-work
     * for minimum-syntax-overhead.
     *
     * @tparam F  Callable invoked on the main thread when the wrapper fires
     * @param tag  String literal identifying the caller (crash diagnostics)
     * @param fn   The callback body — runs on main thread inside the defer
     */
    template <typename F>
    auto bg_cb(const char* tag, F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        return [gen, snapshot, tag,
                fn = std::forward<F>(fn)](auto&&... args) mutable {
            // Decay args into a value-tuple so the deferred body can't observe
            // references that died with the bg-thread stack frame.
            auto args_tuple =
                std::make_tuple(std::decay_t<decltype(args)>(std::forward<decltype(args)>(args))...);
            helix::ui::queue_update(
                tag, [gen, snapshot, tag, fn, t = std::move(args_tuple)]() mutable {
                    if (gen->load(std::memory_order_acquire) != snapshot) {
                        spdlog::trace("[AsyncLifetimeGuard] Skipped expired bg_cb: {}",
                                      tag ? tag : "unknown");
                        return;
                    }
                    std::apply(
                        [&fn](auto&&... a) {
                            fn(std::forward<decltype(a)>(a)...);
                        },
                        std::move(t));
                });
        };
    }

  private:
    std::shared_ptr<std::atomic<uint64_t>> gen_;
};

} // namespace helix

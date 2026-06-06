// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <atomic>
#include <cstdint>
#include <list>

/**
 * @brief Toast notification severity levels
 */
enum class ToastSeverity {
    INFO,    ///< Informational message (blue)
    SUCCESS, ///< Success message (green)
    WARNING, ///< Warning message (orange)
    ERROR    ///< Error message (red)
};

/**
 * @brief Callback type for toast action button
 */
typedef void (*toast_action_callback_t)(void* user_data);

/**
 * @brief Singleton manager for toast notifications
 *
 * Stacks multiple simultaneous toasts in the top-right of the screen. Each
 * toast owns its own dismiss timer and action callback so the stack can
 * dismiss in any order. The stack height is capped at kMaxVisible to protect
 * small screens — overflow silently drops the oldest toast (no queueing).
 */
class ToastManager {
  public:
    static ToastManager& instance();

    ToastManager(const ToastManager&) = delete;
    ToastManager& operator=(const ToastManager&) = delete;
    ToastManager(ToastManager&&) = delete;
    ToastManager& operator=(ToastManager&&) = delete;

    /** Initialize the toast system. Call once at app startup. */
    void init();

    /** Show a toast; does not replace existing toasts (stacks instead). */
    void show(ToastSeverity severity, const char* message, uint32_t duration_ms = 4000);

    /**
     * Show a toast with an action button. Each toast has its own callback, so
     * multiple action toasts can coexist in the stack.
     */
    void show_with_action(ToastSeverity severity, const char* message, const char* action_text,
                          toast_action_callback_t action_callback, void* user_data,
                          uint32_t duration_ms = 5000);

    /** Dismiss all currently visible toasts (animated). */
    void hide();

    /** Deinit LVGL resources; safe to call before lv_deinit. */
    void deinit_subjects();

    /** True if at least one toast is visible. */
    bool is_visible() const;

    /** Whether init() has completed. Callers before phase 9d should route
     *  through PendingStartupWarnings — see ui_notification.cpp. Atomic
     *  because ui_notification may read this from background threads. */
    bool is_initialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

  private:
    ToastManager() = default;
    ~ToastManager();

    struct ToastInstance {
        lv_obj_t* widget = nullptr;
        lv_timer_t* dismiss_timer = nullptr;
        toast_action_callback_t action_cb = nullptr;
        void* action_user_data = nullptr;
        bool is_exiting = false;
    };
    using ToastList = std::list<ToastInstance>; // std::list → stable pointers

    void create_toast_internal(ToastSeverity severity, const char* message, uint32_t duration_ms,
                               bool with_action, toast_action_callback_t action_cb,
                               void* action_user_data, const char* action_text);
    void ensure_stack_container();
    ToastList::iterator find_by_widget(lv_obj_t* widget);
    void begin_exit(ToastList::iterator it);
    void force_remove(ToastList::iterator it); // no animation
    void finalize_remove(lv_obj_t* widget);    // called from exit-anim completion

    // Detach a toast that is being torn down from any input device that has it
    // (or a child) as a cached press/scroll target, and make its action button
    // unclickable. Prevents a stale release/click from dispatching to the
    // soon-to-be-freed widget — SIGBUS in event_send_core (#850; bundle
    // A5V73UV4).
    static void detach_from_input(lv_obj_t* widget);
    void update_notification_bell();
    size_t visible_count() const; // active_ minus those already exiting

    void animate_entrance(lv_obj_t* toast);
    void animate_exit(lv_obj_t* toast);
    static void exit_animation_complete_cb(lv_anim_t* anim);

    static void dismiss_timer_cb(lv_timer_t* timer);
    static void close_btn_clicked(lv_event_t* e);
    static void action_btn_clicked(lv_event_t* e);

    // Hard cap — further bounded at first show_by screen height.
    static constexpr size_t kMaxVisible = 5;

    lv_obj_t* toast_stack_ = nullptr;
    ToastList active_;
    size_t max_visible_ = kMaxVisible;
    std::atomic<bool> initialized_{false};
};

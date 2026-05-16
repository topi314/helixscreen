// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_init.h"

#include "ui_fatal_error.h"

#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "touch_jitter_filter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <lvgl.h>

namespace helix {

namespace {

/// Jitter filter wrapper context — wraps whatever read callback the backend
/// installed and applies jitter suppression on top.  This ensures the filter
/// works on ALL backends (DRM, FBDEV, SDL) rather than only FBDEV.
struct JitterFilterContext {
    TouchJitterFilter jitter;
    lv_indev_read_cb_t original_read_cb = nullptr;
};

/// File-static — only one pointer indev exists at a time.
JitterFilterContext s_jitter_ctx;

void jitter_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (s_jitter_ctx.original_read_cb) {
        s_jitter_ctx.original_read_cb(indev, data);
    }
    // Guard: On Protocol A touchscreens (e.g., Goodix GT9xx), BTN_TOUCH may
    // arrive before MT position events in a separate SYN frame, leaving
    // coordinates at (0,0). Suppress this premature press so LVGL doesn't
    // anchor scroll detection at the screen corner. Only applies to the
    // first frame of a new touch (jitter filter not yet tracking).
    if (data->state == LV_INDEV_STATE_PRESSED && data->point.x == 0 && data->point.y == 0 &&
        !s_jitter_ctx.jitter.tracking) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    s_jitter_ctx.jitter.apply(data->state, data->point.x, data->point.y);
    s_jitter_ctx.jitter.guard_post_scroll(data->state);
}

} // namespace

bool init_lvgl(int width, int height, LvglContext& ctx) {
    lv_init();
    lv_xml_init();

    // Create display backend (auto-detects: DRM → framebuffer → SDL)
    ctx.backend = DisplayBackend::create_auto();
    if (!ctx.backend) {
        spdlog::error("[LVGL] No display backend available");
        lv_deinit();
        return false;
    }

    spdlog::info("[LVGL] Using display backend: {}", ctx.backend->name());

    // Create display
    ctx.display = ctx.backend->create_display(width, height);
    if (!ctx.display) {
        spdlog::error("[LVGL] Failed to create display");
        ctx.backend.reset();
        lv_deinit();
        return false;
    }

    // Create pointer input device (mouse/touch)
    ctx.pointer = ctx.backend->create_input_pointer();
    if (!ctx.pointer) {
#if defined(HELIX_DISPLAY_DRM) || defined(HELIX_DISPLAY_FBDEV)
        // On embedded platforms (DRM/fbdev), no input device is fatal - show error screen
        spdlog::error("[LVGL] No input device found - cannot operate touchscreen UI");

        static const char* suggestions[] = {
            "Check /dev/input/event* devices exist",
            "Ensure user is in 'input' group: sudo usermod -aG input $USER",
            "Check touchscreen driver is loaded: dmesg | grep -i touch",
            "Set HELIX_TOUCH_DEVICE=/dev/input/eventX to override",
            "Add \"touch_device\": \"/dev/input/event1\" to settings.json",
            nullptr};

        ui_show_fatal_error("No Input Device",
                            "Could not find or open a touch/pointer input device.\n"
                            "The UI requires an input device to function.",
                            suggestions,
                            30000 // Show for 30 seconds then exit
        );

        return false;
#else
        // On desktop (SDL), continue without pointer - mouse is optional
        spdlog::warn("[LVGL] No pointer input device created - touch/mouse disabled");
#endif
    }

    // Configure scroll behavior and jitter filtering
    if (ctx.pointer) {
        Config* cfg = Config::get_instance();

        // Scroll tuning
        // scroll_throw: momentum decay rate (1-99), higher = faster decay (LVGL default: 10)
        // scroll_limit: pixels before scrolling starts (LVGL default: 10)
        int scroll_throw = cfg->get<int>("/input/scroll_throw", 12);
        int scroll_limit = cfg->get<int>("/input/scroll_limit", 10);
        lv_indev_set_scroll_throw(ctx.pointer, static_cast<uint8_t>(scroll_throw));
        lv_indev_set_scroll_limit(ctx.pointer, static_cast<uint8_t>(scroll_limit));
        spdlog::debug("[LVGL] Scroll config: throw={}, limit={}", scroll_throw, scroll_limit);

        // Touch jitter filter — suppresses small coordinate noise during
        // stationary taps to prevent noisy touch controllers (e.g., Goodix
        // GT9xx) from generating enough movement to trigger LVGL's scroll
        // detection.  Applied generically here so it works on ALL backends
        // (DRM, FBDEV, SDL).  Set to 0 to disable.
        int jitter_threshold = cfg->get<int>("/input/jitter_threshold", 5);
        const char* env_jitter = std::getenv("HELIX_TOUCH_JITTER");
        if (env_jitter) {
            jitter_threshold = std::atoi(env_jitter);
        }
        // 30 px matches the Settings → Touch & Input slider max and InputSettingsManager;
        // beyond that the filter starts swallowing intentional short-travel gestures.
        jitter_threshold = std::clamp(jitter_threshold, 0, 30);
        // Post-scroll click guard — suppresses ghost taps from capacitive touch
        // controllers that briefly report release→repress when lifting after a scroll.
        bool scroll_guard = cfg->get<bool>("/input/scroll_guard", false);
        const char* env_guard = std::getenv("HELIX_SCROLL_GUARD");
        if (env_guard) {
            scroll_guard = (std::string(env_guard) == "1" || std::string(env_guard) == "true");
        }

        // Cooldown window for the post-scroll click guard. 80 ms matches most
        // controllers; raise if users still see phantom taps at lift-off.
        int scroll_guard_cooldown = cfg->get<int>(
            "/input/scroll_guard_cooldown_ms",
            static_cast<int>(TouchJitterFilter::SCROLL_GUARD_COOLDOWN_MS_DEFAULT));
        const char* env_cooldown = std::getenv("HELIX_SCROLL_GUARD_COOLDOWN_MS");
        if (env_cooldown) {
            scroll_guard_cooldown = std::atoi(env_cooldown);
        }
        scroll_guard_cooldown = std::clamp(scroll_guard_cooldown, 20, 500);

        if (jitter_threshold > 0) {
            spdlog::info("[LVGL] Touch jitter filter: {}px dead zone", jitter_threshold);
            s_jitter_ctx.jitter.threshold_sq = jitter_threshold * jitter_threshold;
            s_jitter_ctx.jitter.scroll_guard_enabled = scroll_guard;
            s_jitter_ctx.jitter.scroll_guard_cooldown_ms =
                static_cast<uint32_t>(scroll_guard_cooldown);
            if (scroll_guard) {
                spdlog::info("[LVGL] Post-scroll click guard: {}ms cooldown",
                             scroll_guard_cooldown);
            }
            s_jitter_ctx.original_read_cb = lv_indev_get_read_cb(ctx.pointer);
            lv_indev_set_read_cb(ctx.pointer, jitter_read_cb);
        } else {
            spdlog::info("[LVGL] Touch jitter filter disabled");
            s_jitter_ctx.jitter.threshold_sq = 0;
            s_jitter_ctx.original_read_cb = nullptr;
        }
    }

    // Create keyboard input device (optional - enables physical keyboard input)
    lv_indev_t* indev_keyboard = ctx.backend->create_input_keyboard();
    if (indev_keyboard) {
        spdlog::debug("[LVGL] Physical keyboard input enabled");

        // Create input group for keyboard navigation and text input
        lv_group_t* input_group = lv_group_create();
        lv_group_set_default(input_group);
        lv_indev_set_group(indev_keyboard, input_group);
        spdlog::debug("[LVGL] Created default input group for keyboard");
    }

    spdlog::debug("[LVGL] Initialized: {}x{}", width, height);

    return true;
}

void deinit_lvgl(LvglContext& ctx) {
    ctx.backend.reset();
    ctx.display = nullptr;
    ctx.pointer = nullptr;
    lv_xml_deinit();
    lv_deinit();
}

} // namespace helix

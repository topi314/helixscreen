// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_manager.h"

#include "backdrop_blur.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_panel_base.h"
#include "ui_panel_home.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_client.h" // For ConnectionState enum
#include "observer_factory.h"
#include "system/telemetry_manager.h"
#include "overlay_base.h"
#include "printer_state.h" // For KlippyState enum
#include "sound_manager.h"
#include "static_subject_registry.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::ui::observe_int_sync;

#include <algorithm>
#include <chrono>
#include <cstdlib>

// ============================================================================
// SINGLETON INSTANCE
// ============================================================================

// Flag set by NavigationManager destructor to detect static destruction.
// Using namespace-scope static ensures it's initialized before main() and
// outlives all function-local statics, including the singleton itself.
namespace {
bool g_nav_manager_destroyed = false;
}

NavigationManager::~NavigationManager() {
    g_nav_manager_destroyed = true;
}

NavigationManager& NavigationManager::instance() {
    static NavigationManager inst;
    return inst;
}

bool NavigationManager::is_destroyed() {
    // Guard against Static Destruction Order Fiasco.
    // This flag is set by NavigationManager's destructor, so it accurately
    // reflects whether the singleton's internal data structures are valid.
    return g_nav_manager_destroyed;
}

// ============================================================================
// HELPER METHODS
// ============================================================================

const char* NavigationManager::panel_id_to_name(PanelId id) {
    static const char* names[] = {"home_panel",     "print_select_panel", "controls_panel",
                                  "filament_panel", "settings_panel",     "advanced_panel"};
    if (static_cast<int>(id) < UI_PANEL_COUNT) {
        return names[static_cast<int>(id)];
    }
    return "unknown_panel";
}

bool NavigationManager::panel_requires_connection(PanelId panel) {
    return panel == PanelId::Controls || panel == PanelId::Filament;
}

bool NavigationManager::is_printer_connected() const {
    auto* subject = get_printer_state().get_printer_connection_state_subject();
    return lv_subject_get_int(subject) == 2;
}

bool NavigationManager::is_klippy_ready() const {
    auto* subject = get_printer_state().get_klippy_state_subject();
    return lv_subject_get_int(subject) == 0; // KlippyState::READY
}

void NavigationManager::clear_overlay_stack() {
    // Hide all overlay panels immediately (no animation for connection loss)
    while (panel_stack_.size() > 1) {
        lv_obj_t* overlay = panel_stack_.back();
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        // Reset transform and opacity for potential reuse
        lv_obj_set_style_translate_x(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);

        // Deactivate the overlay to stop background work (camera, timers, etc.)
        // and invalidate lifetime tokens. Without this, overlays like QrScanner
        // keep their camera running after connection loss. (#632)
        auto inst_it = overlay_instances_.find(overlay);
        if (inst_it != overlay_instances_.end() && inst_it->second) {
            inst_it->second->on_deactivate();
        }

        // Defer close callback via lv_async_call so any object deletion happens
        // OUTSIDE process_pending(). clear_overlay_stack() is called from subject
        // observers (connection loss, klippy shutdown) which fire inside
        // process_pending() — synchronous lv_obj_delete there corrupts LVGL's
        // event linked list (prestonbrown/helixscreen#637).
        auto close_it = overlay_close_callbacks_.find(overlay);
        if (close_it != overlay_close_callbacks_.end()) {
            auto* deferred = new OverlayCloseCallback(std::move(close_it->second));
            overlay_close_callbacks_.erase(close_it);
            lv_async_call(
                [](void* data) {
                    auto* cb = static_cast<OverlayCloseCallback*>(data);
                    if (!NavigationManager::is_destroyed()) {
                        (*cb)();
                    }
                    delete cb;
                },
                deferred);
        }

        // Clean up dynamic backdrop for this overlay (if one was created).
        // Must use safe_delete_deferred — we may be inside process_pending()
        // and synchronous deletion corrupts LVGL's event list (#637).
        auto backdrop_it = overlay_backdrops_.find(overlay);
        if (backdrop_it != overlay_backdrops_.end()) {
            helix::ui::safe_delete_deferred(backdrop_it->second);
            overlay_backdrops_.erase(backdrop_it);
        }

        panel_stack_.pop_back();
        spdlog::trace("[NavigationManager] Cleared overlay {} from stack", (void*)overlay);
    }

    // Clear zoom source rects for any cleared overlays
    zoom_source_rects_.clear();

    // Destroy primary backdrop snapshot
    if (overlay_backdrop_) {
        helix::ui::safe_delete_deferred(overlay_backdrop_);
        overlay_backdrop_ = nullptr;
    }

    spdlog::trace("[NavigationManager] Overlay stack cleared (connection gating)");
}

// ============================================================================
// ANIMATION HELPERS
// ============================================================================

void NavigationManager::overlay_slide_out_complete_cb(lv_anim_t* anim) {
    if (NavigationManager::instance().is_shutting_down()) {
        return; // Shutdown in progress — widget may be freed
    }
    lv_obj_t* panel = static_cast<lv_obj_t*>(anim->var);
    if (!lv_obj_is_valid(panel)) {
        spdlog::warn("[NavigationManager] Animation completed but panel {} already freed",
                     anim->var);
        return;
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    // Reset all transform and opacity properties for potential reuse
    // (covers both slide and zoom animation properties)
    lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    spdlog::trace("[NavigationManager] Overlay slide+fade-out complete, panel {} hidden",
                  (void*)panel);

    // Defer close callback via lv_async_call so any object deletion happens AFTER the
    // current render cycle completes. Animation callbacks fire from inside
    // lv_timer_handler() → lv_display_refr_timer(), and deleting objects mid-layout
    // causes use-after-free in layout_update_core → lv_obj_scrollbar_invalidate.
    auto& mgr = NavigationManager::instance();
    auto it = mgr.overlay_close_callbacks_.find(panel);
    if (it != mgr.overlay_close_callbacks_.end()) {
        spdlog::trace("[NavigationManager] Deferring close callback for overlay {}", (void*)panel);
        // Move callback to heap — lv_async_call will invoke it on the next LVGL tick
        auto* deferred = new OverlayCloseCallback(std::move(it->second));
        mgr.overlay_close_callbacks_.erase(it);
        lv_async_call(
            [](void* data) {
                auto* cb = static_cast<OverlayCloseCallback*>(data);
                if (!NavigationManager::is_destroyed()) {
                    (*cb)();
                }
                delete cb;
            },
            deferred);
    }

    // Lifecycle: Activate what's now visible after animation completes
    // Stack was already modified in go_back(), so check what's now at top
    if (mgr.panel_stack_.size() == 1) {
        // Back to main panel - activate it
        if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
            spdlog::trace("[NavigationManager] Activating main panel {} after overlay closed",
                          static_cast<int>(mgr.active_panel_));
            mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_activate();
        }
    } else if (mgr.panel_stack_.size() > 1) {
        // Back to previous overlay - activate it
        lv_obj_t* now_visible = mgr.panel_stack_.back();
        auto overlay_it = mgr.overlay_instances_.find(now_visible);
        if (overlay_it != mgr.overlay_instances_.end() && overlay_it->second) {
            spdlog::trace("[NavigationManager] Activating previous overlay {}",
                          overlay_it->second->get_name());
            overlay_it->second->on_activate();
        }
    }
}

void NavigationManager::overlay_animate_slide_in(lv_obj_t* panel) {
    int32_t panel_width = lv_obj_get_width(panel);
    if (panel_width <= 0) {
        panel_width = OVERLAY_SLIDE_OFFSET;
    }

    // Skip animation if disabled - show panel in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - showing overlay instantly");
        return;
    }

    // Set initial state: off-screen and transparent
    lv_obj_set_style_translate_x(panel, panel_width, LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);

    // Slide animation: translate from right to final position
    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, panel_width, 0);
    lv_anim_set_duration(&slide_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from transparent to opaque (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-in animation for panel {} (width={})",
                  (void*)panel, panel_width);
}

void NavigationManager::overlay_animate_slide_out(lv_obj_t* panel) {
    // Disable clicks immediately to prevent interaction during animation
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Skip animation if disabled - hide panel immediately and invoke callback
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        // Reset all transform and opacity properties for potential reuse
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - hiding overlay instantly");

        // Invoke close callback if registered
        auto& mgr = NavigationManager::instance();
        auto it = mgr.overlay_close_callbacks_.find(panel);
        if (it != mgr.overlay_close_callbacks_.end()) {
            spdlog::trace("[NavigationManager] Invoking close callback for overlay {}",
                          (void*)panel);
            auto callback = std::move(it->second);
            mgr.overlay_close_callbacks_.erase(it);
            callback();
        }

        // Lifecycle: Activate what's now visible (same logic as animation callback)
        if (mgr.panel_stack_.size() == 1) {
            if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
                spdlog::trace("[NavigationManager] Activating main panel {} after overlay closed",
                              static_cast<int>(mgr.active_panel_));
                mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_activate();
            }
        } else if (mgr.panel_stack_.size() > 1) {
            lv_obj_t* now_visible = mgr.panel_stack_.back();
            auto overlay_it = mgr.overlay_instances_.find(now_visible);
            if (overlay_it != mgr.overlay_instances_.end() && overlay_it->second) {
                spdlog::trace("[NavigationManager] Activating previous overlay {}",
                              overlay_it->second->get_name());
                overlay_it->second->on_activate();
            }
        }
        return;
    }

    int32_t panel_width = lv_obj_get_width(panel);
    if (panel_width <= 0) {
        panel_width = OVERLAY_SLIDE_OFFSET;
    }

    // Slide animation: translate to off-screen right
    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, 0, panel_width);
    lv_anim_set_duration(&slide_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_set_completed_cb(&slide_anim, overlay_slide_out_complete_cb);
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from opaque to transparent (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-out animation for panel {} (width={})",
                  (void*)panel, panel_width);
}

void NavigationManager::overlay_animate_zoom_in(lv_obj_t* panel, lv_area_t source_rect) {
    // Skip animation if disabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - showing zoom overlay instantly");
        return;
    }

    // Calculate panel dimensions
    lv_obj_update_layout(panel);
    int32_t panel_w = lv_obj_get_width(panel);
    int32_t panel_h = lv_obj_get_height(panel);
    if (panel_w <= 0)
        panel_w = 480;
    if (panel_h <= 0)
        panel_h = 800;

    // Get panel screen position
    lv_area_t panel_coords;
    lv_obj_get_coords(panel, &panel_coords);

    // Calculate source rect center and panel center
    int32_t src_cx = (source_rect.x1 + source_rect.x2) / 2;
    int32_t src_cy = (source_rect.y1 + source_rect.y2) / 2;
    int32_t panel_cx = (panel_coords.x1 + panel_coords.x2) / 2;
    int32_t panel_cy = (panel_coords.y1 + panel_coords.y2) / 2;

    // Calculate starting translation (offset from panel center to source center)
    int32_t start_tx = src_cx - panel_cx;
    int32_t start_ty = src_cy - panel_cy;

    // Calculate starting scale based on card/panel size ratio
    // LVGL scale: 256 = 100%
    int32_t src_w = source_rect.x2 - source_rect.x1;
    int32_t start_scale = (src_w * 256) / panel_w;
    if (start_scale < 64)
        start_scale = 64; // Min 25% scale
    if (start_scale > 200)
        start_scale = 200; // Max ~78% scale

    spdlog::debug("[NavigationManager] zoom-in: panel={}x{} src=({},{}-{},{}) "
                  "start_tx={} start_ty={} start_scale={}",
                  panel_w, panel_h, source_rect.x1, source_rect.y1, source_rect.x2, source_rect.y2,
                  start_tx, start_ty, start_scale);

    // Set pivot to center for symmetric scaling
    lv_obj_set_style_transform_pivot_x(panel, panel_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(panel, panel_h / 2, LV_PART_MAIN);

    // Set initial state
    lv_obj_set_style_translate_x(panel, start_tx, LV_PART_MAIN);
    lv_obj_set_style_translate_y(panel, start_ty, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(panel, static_cast<int16_t>(start_scale), LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);

    // Translate X animation
    lv_anim_t tx_anim;
    lv_anim_init(&tx_anim);
    lv_anim_set_var(&tx_anim, panel);
    lv_anim_set_values(&tx_anim, start_tx, 0);
    lv_anim_set_duration(&tx_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&tx_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&tx_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&tx_anim);

    // Translate Y animation
    lv_anim_t ty_anim;
    lv_anim_init(&ty_anim);
    lv_anim_set_var(&ty_anim, panel);
    lv_anim_set_values(&ty_anim, start_ty, 0);
    lv_anim_set_duration(&ty_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&ty_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ty_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&ty_anim);

    // Scale animation
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, panel);
    lv_anim_set_values(&scale_anim, start_scale, 256);
    lv_anim_set_duration(&scale_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(value),
                                         LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    // Opacity animation
    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, panel);
    lv_anim_set_values(&opa_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&opa_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&opa_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&opa_anim);

    spdlog::trace("[NavigationManager] Started zoom-in animation for panel {} (scale {}->256, "
                  "tx {}->0, ty {}->0)",
                  (void*)panel, start_scale, start_tx, start_ty);
}

void NavigationManager::overlay_animate_zoom_out(lv_obj_t* panel, lv_area_t source_rect) {
    // Disable clicks during animation
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Skip animation if disabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);

        // Invoke close callback
        auto it = overlay_close_callbacks_.find(panel);
        if (it != overlay_close_callbacks_.end()) {
            auto callback = std::move(it->second);
            overlay_close_callbacks_.erase(it);
            callback();
        }

        // Lifecycle: Activate what's now visible
        if (panel_stack_.size() == 1) {
            if (panel_instances_[static_cast<int>(active_panel_)]) {
                panel_instances_[static_cast<int>(active_panel_)]->on_activate();
            }
        } else if (panel_stack_.size() > 1) {
            lv_obj_t* now_visible = panel_stack_.back();
            auto overlay_it = overlay_instances_.find(now_visible);
            if (overlay_it != overlay_instances_.end() && overlay_it->second) {
                overlay_it->second->on_activate();
            }
        }
        return;
    }

    // Calculate animation targets (reverse of zoom-in)
    lv_obj_update_layout(panel);
    int32_t panel_w = lv_obj_get_width(panel);
    int32_t panel_h = lv_obj_get_height(panel);
    if (panel_w <= 0)
        panel_w = 480;
    if (panel_h <= 0)
        panel_h = 800;

    lv_area_t panel_coords;
    lv_obj_get_coords(panel, &panel_coords);

    int32_t src_cx = (source_rect.x1 + source_rect.x2) / 2;
    int32_t src_cy = (source_rect.y1 + source_rect.y2) / 2;
    int32_t panel_cx = (panel_coords.x1 + panel_coords.x2) / 2;
    int32_t panel_cy = (panel_coords.y1 + panel_coords.y2) / 2;

    int32_t end_tx = src_cx - panel_cx;
    int32_t end_ty = src_cy - panel_cy;

    int32_t src_w = source_rect.x2 - source_rect.x1;
    int32_t end_scale = (src_w * 256) / panel_w;
    if (end_scale < 64)
        end_scale = 64;
    if (end_scale > 200)
        end_scale = 200;

    lv_obj_set_style_transform_pivot_x(panel, panel_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(panel, panel_h / 2, LV_PART_MAIN);

    // Translate X
    lv_anim_t tx_anim;
    lv_anim_init(&tx_anim);
    lv_anim_set_var(&tx_anim, panel);
    lv_anim_set_values(&tx_anim, 0, end_tx);
    lv_anim_set_duration(&tx_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&tx_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&tx_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&tx_anim);

    // Translate Y
    lv_anim_t ty_anim;
    lv_anim_init(&ty_anim);
    lv_anim_set_var(&ty_anim, panel);
    lv_anim_set_values(&ty_anim, 0, end_ty);
    lv_anim_set_duration(&ty_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&ty_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&ty_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&ty_anim);

    // Scale
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, panel);
    lv_anim_set_values(&scale_anim, 256, end_scale);
    lv_anim_set_duration(&scale_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(value),
                                         LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    // Opacity — use the completed callback to handle post-animation cleanup
    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, panel);
    lv_anim_set_values(&opa_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&opa_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&opa_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    // Reuse the existing slide-out completion callback for post-animation cleanup
    lv_anim_set_completed_cb(&opa_anim, overlay_slide_out_complete_cb);
    lv_anim_start(&opa_anim);

    spdlog::trace("[NavigationManager] Started zoom-out animation for panel {} (scale 256->{}, "
                  "tx 0->{}, ty 0->{})",
                  (void*)panel, end_scale, end_tx, end_ty);
}

// ============================================================================
// OBSERVER HANDLERS (used by factory-created observers)
// ============================================================================

void NavigationManager::handle_active_panel_change(int32_t new_active_panel) {
    // Show/hide panels if widgets are set
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (panel_widgets_[i]) {
            if (i == new_active_panel) {
                lv_obj_remove_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void NavigationManager::handle_connection_state_change(int state) {
    bool was_connected =
        (previous_connection_state_ == static_cast<int>(ConnectionState::CONNECTED));
    bool is_connected = (state == static_cast<int>(ConnectionState::CONNECTED));

    // Only redirect if we were previously connected and are now disconnected
    if (was_connected && !is_connected && panel_requires_connection(active_panel_)) {
        spdlog::info("[NavigationManager] Connection lost on panel {} - navigating to home",
                     static_cast<int>(active_panel_));

        clear_overlay_stack();
        set_active(PanelId::Home);
    }

    previous_connection_state_ = state;
}

void NavigationManager::handle_klippy_state_change(int state) {
    bool was_ready = (previous_klippy_state_ == static_cast<int>(KlippyState::READY));
    bool is_ready = (state == static_cast<int>(KlippyState::READY));

    // Redirect to home if klippy enters non-READY state (SHUTDOWN/ERROR) while on restricted panel
    if (was_ready && !is_ready && panel_requires_connection(active_panel_)) {
        const char* state_name = (state == static_cast<int>(KlippyState::SHUTDOWN)) ? "SHUTDOWN"
                                 : (state == static_cast<int>(KlippyState::ERROR))  ? "ERROR"
                                                                                    : "non-READY";
        spdlog::info("[NavigationManager] Klippy {} on panel {} - navigating to home", state_name,
                     static_cast<int>(active_panel_));

        clear_overlay_stack();
        set_active(PanelId::Home);
    }

    previous_klippy_state_ = state;
}

// ============================================================================
// EVENT CALLBACKS
// ============================================================================

void NavigationManager::backdrop_click_event_cb(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* current = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only respond if click was directly on backdrop (not bubbled from child)
    if (target != current) {
        return;
    }

    auto& mgr = NavigationManager::instance();

    // Only process if there's an overlay to close (stack > 1 means overlays exist)
    if (mgr.panel_stack_.size() <= 1) {
        return;
    }

    // Get click position to check if it's in the navbar area
    lv_point_t click_point;
    lv_indev_get_point(lv_indev_active(), &click_point);

    // Check if click is in navbar area and find which button was clicked
    if (mgr.navbar_widget_) {
        int32_t navbar_width = lv_obj_get_width(mgr.navbar_widget_);

        if (click_point.x < navbar_width) {
            // Click is in navbar area - find which button and trigger navigation
            const char* button_names[] = {"nav_btn_home",     "nav_btn_print_select",
                                          "nav_btn_controls", "nav_btn_filament",
                                          "nav_btn_settings", "nav_btn_advanced"};

            for (int i = 0; i < UI_PANEL_COUNT; i++) {
                lv_obj_t* btn = lv_obj_find_by_name(mgr.navbar_widget_, button_names[i]);
                if (!btn) {
                    continue;
                }

                // Check if click point is inside this button
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);

                // Simple bounds check (point in rectangle)
                if (click_point.x >= btn_area.x1 && click_point.x <= btn_area.x2 &&
                    click_point.y >= btn_area.y1 && click_point.y <= btn_area.y2) {
                    spdlog::trace(
                        "[NavigationManager] Backdrop click forwarded to navbar button {}", i);
                    // Simulate the navbar button click by sending a clicked event
                    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
                    return;
                }
            }

            // Click was in navbar area but not on a button - just close overlay
            spdlog::trace("[NavigationManager] Backdrop clicked in navbar area (no button hit)");
        }
    }

    // Regular backdrop click - close topmost overlay
    spdlog::trace("[NavigationManager] Backdrop clicked, closing topmost overlay");
    mgr.go_back();
}

void NavigationManager::nav_button_clicked_cb(lv_event_t* event) {
    LVGL_SAFE_EVENT_CB_BEGIN("nav_button_clicked_cb");

    auto& mgr = NavigationManager::instance();
    lv_event_code_t code = lv_event_get_code(event);
    int panel_id = (int)(uintptr_t)lv_event_get_user_data(event);

    spdlog::trace("[NavigationManager] nav_button_clicked_cb fired: code={}, panel_id={}, "
                  "active_panel={}",
                  static_cast<int>(code), panel_id, static_cast<int>(mgr.active_panel_));

    if (code == LV_EVENT_CLICKED) {
        // Already on this panel with no overlays: special handling per panel
        if (panel_id == static_cast<int>(mgr.active_panel_) && !mgr.has_open_overlays()) {
            if (panel_id == static_cast<int>(PanelId::Home)) {
                // Tapping home while on home scrolls carousel to page 0
                spdlog::debug("[NavigationManager] Already on Home - navigating to main page");
                get_global_home_panel().go_to_main_page();
            } else {
                spdlog::debug("[NavigationManager] Skipping - already on panel {} with no overlays",
                              panel_id);
            }
            return;
        }

        // Block navigation to connection-required panels when disconnected or klippy not ready
        if (panel_requires_connection(static_cast<PanelId>(panel_id))) {
            if (!mgr.is_printer_connected()) {
                spdlog::info("[NavigationManager] Navigation to panel {} blocked - not connected",
                             panel_id);
                return;
            }
            if (!mgr.is_klippy_ready()) {
                spdlog::info(
                    "[NavigationManager] Navigation to panel {} blocked - klippy not ready",
                    panel_id);
                return;
            }
        }

        // Queue for REFR_START - guarantees we never modify widgets during render phase
        spdlog::trace("[NavigationManager] Queuing switch to panel {}", panel_id);
        helix::ui::queue_update(
            [panel_id]() { NavigationManager::instance().switch_to_panel_impl(panel_id); });
    }

    LVGL_SAFE_EVENT_CB_END();
}

void NavigationManager::switch_to_panel_impl(int panel_id) {
    auto switch_start = std::chrono::steady_clock::now();
    spdlog::trace("[NavigationManager] switch_to_panel_impl executing for panel {}", panel_id);

    // Hide ALL visible overlay panels
    lv_obj_t* screen = lv_screen_active();
    if (screen) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
            lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }

            if (child == app_layout_widget_) {
                continue;
            }

            bool is_main_panel = false;
            for (int j = 0; j < UI_PANEL_COUNT; j++) {
                if (panel_widgets_[j] == child) {
                    is_main_panel = true;
                    break;
                }
            }

            if (!is_main_panel) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                // Reset transform and opacity for potential reuse
                lv_obj_set_style_translate_x(child, 0, LV_PART_MAIN);
                lv_obj_set_style_opa(child, LV_OPA_COVER, LV_PART_MAIN);
                spdlog::trace("[NavigationManager] Hiding overlay panel {} (nav button clicked)",
                              (void*)child);
            }
        }
    }

    // Hide all main panels
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (panel_widgets_[i]) {
            lv_obj_add_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Deactivate overlays, invoke close callbacks, and clean up backdrops
    // for any overlays being cleared (e.g., settings overlay needs on_deactivate to save).
    // Persistent overlays (e.g., PrintStatusPanel) are SKIPPED — they continue
    // collecting data (temperature history) in the background across navbar switches.
    for (lv_obj_t* panel : panel_stack_) {
        bool is_persistent = persistent_overlay_instances_.count(panel) > 0;

        if (!is_persistent) {
            // Call on_deactivate() if this overlay has a registered instance
            auto inst_it = overlay_instances_.find(panel);
            if (inst_it != overlay_instances_.end() && inst_it->second) {
                spdlog::trace("[NavigationManager] Calling on_deactivate() for overlay {} (navbar)",
                              (void*)panel);
                inst_it->second->on_deactivate();
            }
        } else {
            spdlog::trace(
                "[NavigationManager] Skipping on_deactivate() for persistent overlay {} (navbar)",
                (void*)panel);
        }

        // Defer close callback via lv_async_call — switch_to_panel_impl() can be
        // called from subject observers inside process_pending(), and synchronous
        // lv_obj_delete in close callbacks corrupts LVGL's event list (#637).
        auto it = overlay_close_callbacks_.find(panel);
        if (it != overlay_close_callbacks_.end()) {
            spdlog::trace("[NavigationManager] Deferring close callback for panel {} (navbar)",
                          (void*)panel);
            auto* deferred = new OverlayCloseCallback(std::move(it->second));
            overlay_close_callbacks_.erase(it);
            lv_async_call(
                [](void* data) {
                    auto* cb = static_cast<OverlayCloseCallback*>(data);
                    if (!NavigationManager::is_destroyed()) {
                        (*cb)();
                    }
                    delete cb;
                },
                deferred);
        }

        // Clean up dynamic backdrop for this overlay (if one was created).
        // Must use safe_delete_deferred — we're inside a queue_update() callback
        // and synchronous deletion corrupts LVGL's event list (#620).
        auto backdrop_it = overlay_backdrops_.find(panel);
        if (backdrop_it != overlay_backdrops_.end()) {
            helix::ui::safe_delete_deferred(backdrop_it->second);
            overlay_backdrops_.erase(backdrop_it);
        }
    }

    // Clear panel stack and all overlay tracking maps.
    // overlay_instances_ must be cleared here — not all overlays are deleted by
    // their close callbacks (some are only hidden), leaving stale entries that
    // cause has_open_overlays() to return true and break subsequent home-button
    // "go to page 0" logic. Persistent overlay registrations are preserved.
    panel_stack_.clear();
    overlay_instances_.clear();
    // Restore persistent overlay registrations so they survive the clear
    for (auto& [widget, lifecycle] : persistent_overlay_instances_) {
        overlay_instances_[widget] = lifecycle;
    }
    overlay_close_callbacks_.clear();
    // Delete any remaining dynamic backdrops the loop above didn't reach
    // (orphaned entries not in panel_stack_), then clear the map.
    for (auto& [_, backdrop] : overlay_backdrops_) {
        helix::ui::safe_delete_deferred(backdrop);
    }
    overlay_backdrops_.clear();
    spdlog::trace("[NavigationManager] Panel stack and overlay maps cleared (nav button clicked)");

    // Destroy primary backdrop snapshot since all overlays are being cleared
    if (overlay_backdrop_) {
        helix::ui::safe_delete_deferred(overlay_backdrop_);
        overlay_backdrop_ = nullptr;
    }

    // Show the clicked panel
    lv_obj_t* new_panel = panel_widgets_[static_cast<int>(panel_id)];
    if (new_panel) {
        lv_obj_remove_flag(new_panel, LV_OBJ_FLAG_HIDDEN);
        panel_stack_.push_back(new_panel);
        spdlog::trace("[NavigationManager] Showing panel {} (stack depth: {})", (void*)new_panel,
                      panel_stack_.size());
    }

    set_active((PanelId)panel_id);
    SoundManager::instance().play("nav_forward");

    auto switch_elapsed = std::chrono::steady_clock::now() - switch_start;
    spdlog::info("[NavigationManager] Panel switch to {} took {:.1f}ms", panel_id,
                 std::chrono::duration<double, std::milli>(switch_elapsed).count());
}

// ============================================================================
// NAVIGATION MANAGER IMPLEMENTATION
// ============================================================================

void NavigationManager::init() {
    if (subjects_initialized_) {
        spdlog::warn("[NavigationManager] Subjects already initialized");
        return;
    }

    spdlog::trace("[NavigationManager] Initializing navigation reactive subjects...");

    UI_MANAGED_SUBJECT_INT(active_panel_subject_, static_cast<int>(PanelId::Home), "active_panel",
                           subjects_);

    // Overlay backdrop starts hidden
    UI_MANAGED_SUBJECT_INT(overlay_backdrop_visible_subject_, 0, "overlay_backdrop_visible",
                           subjects_);

    active_panel_observer_ = observe_int_sync<NavigationManager>(
        &active_panel_subject_, this,
        [](NavigationManager* mgr, int value) { mgr->handle_active_panel_change(value); });

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "NavigationManager", []() { NavigationManager::instance().deinit_subjects(); });

    spdlog::trace("[NavigationManager] Navigation subjects initialized successfully");
}

void NavigationManager::init_overlay_backdrop(lv_obj_t* screen) {
    // Backdrop is now created dynamically as a darkened snapshot when the first
    // overlay is pushed.  Nothing to pre-create.
    (void)screen;
    spdlog::trace("[NavigationManager] Overlay backdrop init (dynamic snapshot mode)");
}

void NavigationManager::set_app_layout(lv_obj_t* app_layout) {
    app_layout_widget_ = app_layout;
    spdlog::trace("[NavigationManager] App layout widget registered");
}

void NavigationManager::wire_events(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_events");
        return;
    }

    if (!subjects_initialized_) {
        spdlog::error("[NavigationManager] Subjects not initialized! Call init() first!");
        return;
    }

    // Store navbar reference for z-order management when showing overlays
    navbar_widget_ = navbar;

    lv_obj_remove_flag(navbar, LV_OBJ_FLAG_CLICKABLE);

    const char* button_names[] = {"nav_btn_home",     "nav_btn_print_select", "nav_btn_controls",
                                  "nav_btn_filament", "nav_btn_settings",     "nav_btn_advanced"};

    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);

        if (!btn) {
            spdlog::trace("[NavigationManager] Nav button {} not found (may be intentional)", i);
            continue;
        }

        lv_obj_add_event_cb(btn, nav_button_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        // Remove focus ring — nav buttons use icon color swap for active state
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_group_remove_obj(btn);
    }

    // Register connection state observer for redirect on disconnect
    connection_state_observer_ = observe_int_sync<NavigationManager>(
        get_printer_state().get_printer_connection_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_connection_state_change(value); });

    // Register klippy state observer for redirect on SHUTDOWN/ERROR
    klippy_state_observer_ = observe_int_sync<NavigationManager>(
        get_printer_state().get_klippy_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_klippy_state_change(value); });

    // Printer badge click handler
    lv_obj_t* printer_badge = lv_obj_find_by_name(navbar, "nav_printer_badge");
    if (printer_badge) {
        lv_obj_add_event_cb(
            printer_badge,
            [](lv_event_t*) { NavigationManager::instance().on_printer_badge_clicked(); },
            LV_EVENT_CLICKED, nullptr);
    }

    // Connection status dot — color reflects WebSocket connection state
    printer_dot_widget_ = lv_obj_find_by_name(navbar, "nav_printer_dot");
    if (printer_dot_widget_) {
        printer_dot_observer_ = observe_int_sync<NavigationManager>(
            get_printer_state().get_printer_connection_state_subject(), this,
            [](NavigationManager* mgr, int state) {
                if (!mgr->printer_dot_widget_)
                    return;
                lv_color_t color;
                switch (state) {
                case 2: // connected
                    color = theme_manager_get_color("success");
                    break;
                case 1: // connecting
                case 3: // reconnecting
                    color = theme_manager_get_color("warning");
                    break;
                default: // disconnected, failed
                    color = theme_manager_get_color("danger");
                    break;
                }
                lv_obj_set_style_bg_color(mgr->printer_dot_widget_, color, 0);
            });
    }

    spdlog::trace(
        "[NavigationManager] Navigation button events wired (with connection/klippy gating)");
}

void NavigationManager::wire_status_icons(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_status_icons");
        return;
    }

    const char* button_names[] = {"status_btn_printer", "status_btn_network",
                                  "status_notification_icon"};
    const char* icon_names[] = {"status_printer_icon", "status_network_icon",
                                "status_notification_icon"};
    const int status_icon_count = 3;

    for (int i = 0; i < status_icon_count; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);
        lv_obj_t* icon_widget = lv_obj_find_by_name(navbar, icon_names[i]);

        if (!btn || !icon_widget) {
            spdlog::warn("[NavigationManager] Status icon {}: btn={}, icon={} (may not exist yet)",
                         button_names[i], (void*)btn, (void*)icon_widget);
            continue;
        }

        lv_obj_add_flag(icon_widget, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(icon_widget, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        spdlog::trace("[NavigationManager] Status icon {} wired", button_names[i]);
    }
}

void NavigationManager::set_active(PanelId panel_id) {
    if (static_cast<int>(panel_id) >= UI_PANEL_COUNT) {
        spdlog::error("[NavigationManager] Invalid panel ID: {}", static_cast<int>(panel_id));
        return;
    }

    if (panel_id == active_panel_) {
        return;
    }

    PanelId old_panel = active_panel_;

    // Update panel stack
    // IMPORTANT: Only update the base panel in the stack, preserving any overlays.
    // This fixes the bug where closing an overlay from Controls would return to Home
    // because set_active() was clearing the entire stack unconditionally.
    if (panel_widgets_[static_cast<int>(panel_id)]) {
        if (panel_stack_.empty()) {
            // Stack is empty - just push the new panel
            panel_stack_.push_back(panel_widgets_[static_cast<int>(panel_id)]);
            spdlog::trace("[NavigationManager] Panel stack initialized with panel {}",
                          static_cast<int>(panel_id));
        } else if (panel_stack_.size() == 1) {
            // Only base panel in stack - replace it
            panel_stack_[0] = panel_widgets_[static_cast<int>(panel_id)];
            spdlog::trace("[NavigationManager] Panel stack base updated to panel {}",
                          static_cast<int>(panel_id));
        } else {
            // Overlays are present - update base panel but preserve overlays
            // This handles the case where connection changes while an overlay is open
            panel_stack_[0] = panel_widgets_[static_cast<int>(panel_id)];
            spdlog::trace("[NavigationManager] Panel stack base updated to panel {}, "
                          "preserving {} overlays",
                          static_cast<int>(panel_id), panel_stack_.size() - 1);
        }
    }

    // Call on_deactivate() BEFORE state update
    if (panel_instances_[static_cast<int>(old_panel)]) {
        spdlog::trace("[NavigationManager] Calling on_deactivate() for panel {}",
                      static_cast<int>(old_panel));
        panel_instances_[static_cast<int>(old_panel)]->on_deactivate();
    }

    // Update state
    lv_subject_set_int(&active_panel_subject_, static_cast<int>(panel_id));
    active_panel_ = panel_id;
    // Publish for off-main memory_warning context (relaxed: telemetry only).
    helix::telemetry_context::active_panel_int.store(static_cast<int>(panel_id),
                                                      std::memory_order_relaxed);

    // Crash-diagnostic breadcrumb: records which panel transition was in flight
    // if we crash during on_activate/layout/first-paint.
    {
        const char* name = panel_instances_[static_cast<int>(panel_id)]
                               ? panel_instances_[static_cast<int>(panel_id)]->get_name()
                               : nullptr;
        crash_handler::breadcrumb::note("nav", name ? name : "",
                                        static_cast<long>(panel_id));
    }

    // Call on_activate() AFTER state update
    if (panel_instances_[static_cast<int>(panel_id)]) {
        spdlog::trace("[NavigationManager] Calling on_activate() for panel {}",
                      static_cast<int>(panel_id));
        panel_instances_[static_cast<int>(panel_id)]->on_activate();
    }
}

PanelId NavigationManager::get_active() const {
    return active_panel_;
}

void NavigationManager::set_panels(lv_obj_t** panels) {
    if (!panels) {
        spdlog::error("[NavigationManager] NULL panels array provided");
        return;
    }

    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panel_widgets_[i] = panels[i];
    }

    // Hide all panels except active one
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (panel_widgets_[i]) {
            if (i == static_cast<int>(active_panel_)) {
                lv_obj_remove_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Initialize panel stack
    panel_stack_.clear();
    if (panel_widgets_[static_cast<int>(active_panel_)]) {
        panel_stack_.push_back(panel_widgets_[static_cast<int>(active_panel_)]);
        spdlog::trace("[NavigationManager] Panel stack initialized with active panel {}",
                      (void*)panel_widgets_[static_cast<int>(active_panel_)]);
    }

    spdlog::trace("[NavigationManager] Panel widgets registered for show/hide management");
}

void NavigationManager::register_panel_instance(PanelId id, PanelBase* panel) {
    if (static_cast<int>(id) >= UI_PANEL_COUNT) {
        spdlog::error("[NavigationManager] Invalid panel ID for registration: {}",
                      static_cast<int>(id));
        return;
    }
    panel_instances_[static_cast<int>(id)] = panel;
    spdlog::trace("[NavigationManager] Registered panel instance for ID {}", static_cast<int>(id));
}

helix::PanelId NavigationManager::find_panel_id(const PanelBase* panel) const {
    if (!panel) return helix::PanelId::Count;
    for (int i = 0; i < UI_PANEL_COUNT; ++i) {
        if (panel_instances_[i] == panel) {
            return static_cast<helix::PanelId>(i);
        }
    }
    return helix::PanelId::Count;
}

void NavigationManager::replace_panel_widget(helix::PanelId id, lv_obj_t* new_widget) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= UI_PANEL_COUNT) return;
    panel_widgets_[idx] = new_widget;
    spdlog::debug("[NavigationManager] Panel widget for {} swapped to {}",
                  panel_id_to_name(id), (void*)new_widget);
}

lv_obj_t* NavigationManager::get_panel_widget(helix::PanelId id) const {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= UI_PANEL_COUNT) return nullptr;
    return panel_widgets_[idx];
}

void NavigationManager::rekey_overlay_widget(lv_obj_t* old_widget, lv_obj_t* new_widget) {
    if (!old_widget || !new_widget || old_widget == new_widget) return;

    auto rekey_life = [&](std::unordered_map<lv_obj_t*, IPanelLifecycle*>& m) {
        auto it = m.find(old_widget);
        if (it != m.end()) {
            auto* inst = it->second;
            m.erase(it);
            m[new_widget] = inst;
        }
    };
    rekey_life(overlay_instances_);
    rekey_life(persistent_overlay_instances_);

    auto bd_it = overlay_backdrops_.find(old_widget);
    if (bd_it != overlay_backdrops_.end()) {
        auto* backdrop = bd_it->second;
        overlay_backdrops_.erase(bd_it);
        overlay_backdrops_[new_widget] = backdrop;
    }

    auto cb_it = overlay_close_callbacks_.find(old_widget);
    if (cb_it != overlay_close_callbacks_.end()) {
        auto cb = std::move(cb_it->second);
        overlay_close_callbacks_.erase(cb_it);
        overlay_close_callbacks_[new_widget] = std::move(cb);
    }

    auto zs_it = zoom_source_rects_.find(old_widget);
    if (zs_it != zoom_source_rects_.end()) {
        auto rect = zs_it->second;
        zoom_source_rects_.erase(zs_it);
        zoom_source_rects_[new_widget] = rect;
    }

    std::replace(panel_stack_.begin(), panel_stack_.end(), old_widget, new_widget);

    spdlog::debug("[NavigationManager] Rekeyed overlay widget {} → {}", (void*)old_widget,
                  (void*)new_widget);
}

void NavigationManager::rebuild_active_views() {
    if (shutting_down_) {
        spdlog::debug("[NavigationManager] rebuild_active_views: shutting down, skipping");
        return;
    }
    spdlog::info("[NavigationManager] Rebuilding active views for hot-reload");

    // Active main panel
    int active_idx = static_cast<int>(active_panel_);
    if (active_idx >= 0 && active_idx < UI_PANEL_COUNT) {
        if (auto* p = panel_instances_[active_idx]) {
            p->rebuild();
        }
    }

    // All overlays — snapshot first because rebuild() mutates the maps via rekey.
    std::vector<IPanelLifecycle*> overlays;
    overlays.reserve(overlay_instances_.size() + persistent_overlay_instances_.size());
    for (auto& [w, inst] : overlay_instances_) {
        if (inst) overlays.push_back(inst);
    }
    for (auto& [w, inst] : persistent_overlay_instances_) {
        if (inst) overlays.push_back(inst);
    }
    for (auto* inst : overlays) {
        inst->rebuild();
    }

    // Top modal — rebuild via Modal::rebuild_top()
    if (ModalStack::instance().top_dialog()) {
        Modal::rebuild_top();
    }
}

void NavigationManager::activate_initial_panel() {
    if (panel_instances_[static_cast<int>(active_panel_)]) {
        spdlog::trace("[NavigationManager] Activating initial panel {}",
                      static_cast<int>(active_panel_));
        panel_instances_[static_cast<int>(active_panel_)]->on_activate();
    }
}

void NavigationManager::suspend_active() {
    if (suspended_) {
        return;
    }
    suspended_ = true;

    // Deactivate whatever is currently visible — topmost overlay or main panel
    if (panel_stack_.size() > 1) {
        lv_obj_t* top_overlay = panel_stack_.back();
        auto it = overlay_instances_.find(top_overlay);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::debug("[NavigationManager] Suspending overlay {}", it->second->get_name());
            it->second->on_deactivate();
        }
    } else if (panel_instances_[static_cast<int>(active_panel_)]) {
        spdlog::debug("[NavigationManager] Suspending panel {}", static_cast<int>(active_panel_));
        panel_instances_[static_cast<int>(active_panel_)]->on_deactivate();
    }
}

void NavigationManager::resume_active() {
    if (!suspended_) {
        return;
    }
    suspended_ = false;

    // Re-activate whatever is currently visible
    if (panel_stack_.size() > 1) {
        lv_obj_t* top_overlay = panel_stack_.back();
        auto it = overlay_instances_.find(top_overlay);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::debug("[NavigationManager] Resuming overlay {}", it->second->get_name());
            it->second->on_activate();
        }
    } else if (panel_instances_[static_cast<int>(active_panel_)]) {
        spdlog::debug("[NavigationManager] Resuming panel {}", static_cast<int>(active_panel_));
        panel_instances_[static_cast<int>(active_panel_)]->on_activate();
    }
}

void NavigationManager::register_overlay_instance(lv_obj_t* widget, IPanelLifecycle* overlay,
                                                  bool persistent) {
    if (!widget) {
        spdlog::error("[NavigationManager] Cannot register overlay with NULL widget");
        return;
    }
    overlay_instances_[widget] = overlay;
    if (persistent) {
        persistent_overlay_instances_[widget] = overlay;
    }
    if (overlay) {
        spdlog::trace("[NavigationManager] Registered overlay instance {} for widget {}"
                      " (persistent={})",
                      overlay->get_name(), (void*)widget, persistent);
    } else {
        spdlog::trace("[NavigationManager] Registered overlay widget {} (no lifecycle)",
                      (void*)widget);
    }
}

void NavigationManager::unregister_overlay_instance(lv_obj_t* widget) {
    auto it = overlay_instances_.find(widget);
    if (it != overlay_instances_.end()) {
        spdlog::trace("[NavigationManager] Unregistered overlay instance for widget {}",
                      (void*)widget);
        overlay_instances_.erase(it);
    }
    persistent_overlay_instances_.erase(widget);
}

IPanelLifecycle* NavigationManager::resolve_overlay_lifecycle(lv_obj_t* overlay_panel) {
    auto it = overlay_instances_.find(overlay_panel);
    if (it == overlay_instances_.end()) {
        // Check persistent map — overlay may have survived a panel switch
        auto pit = persistent_overlay_instances_.find(overlay_panel);
        if (pit != persistent_overlay_instances_.end()) {
            overlay_instances_[overlay_panel] = pit->second;
            spdlog::trace("[NavigationManager] Restored persistent overlay {} registration",
                          (void*)overlay_panel);
            return pit->second;
        }
        return nullptr;
    }
    return it->second;
}

void NavigationManager::push_overlay(lv_obj_t* overlay_panel, bool hide_previous) {
    if (!overlay_panel) {
        spdlog::error("[NavigationManager] Cannot push NULL overlay panel");
        return;
    }

    // Always queue - this is the safest pattern for overlay operations
    // which can be triggered from various contexts (events, observers, etc.)
    helix::ui::queue_update([overlay_panel, hide_previous]() {
        auto& mgr = NavigationManager::instance();

        // Check for duplicate push
        if (std::find(mgr.panel_stack_.begin(), mgr.panel_stack_.end(), overlay_panel) !=
            mgr.panel_stack_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} already in stack, ignoring duplicate push",
                         (void*)overlay_panel);
            return;
        }

        bool is_first_overlay = (mgr.panel_stack_.size() == 1);

        // Track overlay opens for telemetry panel_usage event.
        // Breadcrumb distinguishes three cases so crash analysis can tell
        // an intentional callback-based overlay from a missing-registration bug:
        //   - lifecycle present  -> use IPanelLifecycle::get_name()
        //   - registered w/ null -> "anon" (function-based, e.g. keypad,
        //                          notification, theme_explorer, factory_reset)
        //   - not registered     -> "unreg" (caller forgot register_overlay_instance)
        auto inst_it = mgr.overlay_instances_.find(overlay_panel);
        bool registered = inst_it != mgr.overlay_instances_.end() ||
                          mgr.persistent_overlay_instances_.count(overlay_panel) > 0;
        auto* lc = mgr.resolve_overlay_lifecycle(overlay_panel);
        std::string overlay_name = lc ? lc->get_name() : (registered ? "anon" : "unreg");
        if (!registered) {
            spdlog::warn("[NavigationManager] push_overlay({}): no register_overlay_instance "
                         "call before push — overlay invisible to lifecycle machinery",
                         (void*)overlay_panel);
        }
        TelemetryManager::instance().notify_overlay_opened(overlay_name);
        crash_handler::breadcrumb::note("overlay+", overlay_name.c_str());

        // Lifecycle: Deactivate what's currently visible before showing new overlay
        if (is_first_overlay) {
            // Deactivate main panel when first overlay covers it
            if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
                spdlog::trace("[NavigationManager] Deactivating main panel {} for overlay",
                              static_cast<int>(mgr.active_panel_));
                mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_deactivate();
            }
        } else {
            // Deactivate previous overlay if stacking
            lv_obj_t* prev_overlay = mgr.panel_stack_.back();
            auto it = mgr.overlay_instances_.find(prev_overlay);
            if (it != mgr.overlay_instances_.end() && it->second) {
                spdlog::trace("[NavigationManager] Deactivating previous overlay {}",
                              it->second->get_name());
                it->second->on_deactivate();
            }
        }

        // Create snapshot-darkened backdrop BEFORE hiding — snapshot must capture
        // the visible content, not a blank screen.
        lv_obj_t* screen = lv_obj_get_screen(overlay_panel);
        if (screen && is_first_overlay) {
            mgr.overlay_backdrop_ = helix::ui::create_darkened_backdrop(screen, 40);
            if (mgr.overlay_backdrop_) {
                lv_obj_move_foreground(mgr.overlay_backdrop_);
                lv_obj_add_event_cb(mgr.overlay_backdrop_, backdrop_click_event_cb,
                                    LV_EVENT_CLICKED, nullptr);
            }
        }

        // Optionally hide current top panel (after snapshot)
        if (hide_previous && !mgr.panel_stack_.empty()) {
            lv_obj_t* current_top = mgr.panel_stack_.back();
            lv_obj_add_flag(current_top, LV_OBJ_FLAG_HIDDEN);
        }

        // Show overlay
        lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay_panel);
        mgr.panel_stack_.push_back(overlay_panel);
        mgr.overlay_animate_slide_in(overlay_panel);

        // Lifecycle: Activate new overlay
        auto* lifecycle = mgr.resolve_overlay_lifecycle(overlay_panel);
        if (!lifecycle) {
            // Only warn if truly unregistered — overlays registered with nullptr
            // lifecycle (e.g. keypad) are intentionally function-based.
            bool registered = mgr.overlay_instances_.count(overlay_panel) ||
                              mgr.persistent_overlay_instances_.count(overlay_panel);
            if (!registered) {
                spdlog::warn("[NavigationManager] Overlay {} pushed without lifecycle registration",
                             (void*)overlay_panel);
            }
        } else {
            spdlog::trace("[NavigationManager] Activating overlay {}", lifecycle->get_name());
            lifecycle->on_activate();
        }

        SoundManager::instance().play("nav_forward");
        spdlog::trace("[NavigationManager] Pushed overlay {} (stack: {})", (void*)overlay_panel,
                      mgr.panel_stack_.size());
    });
}

void NavigationManager::push_overlay_zoom_from(lv_obj_t* overlay_panel, lv_area_t source_rect) {
    if (!overlay_panel) {
        spdlog::error("[NavigationManager] Cannot push NULL overlay panel");
        return;
    }

    // Queue the push operation (same pattern as push_overlay)
    helix::ui::queue_update([overlay_panel, source_rect]() {
        auto& mgr = NavigationManager::instance();

        // Store source rect for reverse animation on go_back (must be on UI thread)
        mgr.zoom_source_rects_[overlay_panel] = source_rect;

        // Check for duplicate push
        if (std::find(mgr.panel_stack_.begin(), mgr.panel_stack_.end(), overlay_panel) !=
            mgr.panel_stack_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} already in stack, ignoring duplicate push",
                         (void*)overlay_panel);
            return;
        }

        bool is_first_overlay = (mgr.panel_stack_.size() == 1);

        // Track overlay opens for telemetry panel_usage event.
        // See push_overlay() above for the three-way distinction.
        auto inst_it = mgr.overlay_instances_.find(overlay_panel);
        bool registered = inst_it != mgr.overlay_instances_.end() ||
                          mgr.persistent_overlay_instances_.count(overlay_panel) > 0;
        auto* lc = mgr.resolve_overlay_lifecycle(overlay_panel);
        std::string overlay_name = lc ? lc->get_name() : (registered ? "anon" : "unreg");
        TelemetryManager::instance().notify_overlay_opened(overlay_name);
        crash_handler::breadcrumb::note("overlay+", overlay_name.c_str());

        // Lifecycle: Deactivate what's currently visible
        if (is_first_overlay) {
            if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
                mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_deactivate();
            }
        } else {
            lv_obj_t* prev_overlay = mgr.panel_stack_.back();
            auto it = mgr.overlay_instances_.find(prev_overlay);
            if (it != mgr.overlay_instances_.end() && it->second) {
                it->second->on_deactivate();
            }
        }

        // Create backdrop BEFORE hiding previous panel — snapshot must capture
        // the visible content, not a blank screen.
        lv_obj_t* screen = lv_obj_get_screen(overlay_panel);
        if (screen && is_first_overlay) {
            mgr.overlay_backdrop_ = helix::ui::create_darkened_backdrop(screen, 40);
            if (mgr.overlay_backdrop_) {
                lv_obj_move_foreground(mgr.overlay_backdrop_);
                lv_obj_add_event_cb(mgr.overlay_backdrop_, backdrop_click_event_cb,
                                    LV_EVENT_CLICKED, nullptr);
            }
        }

        // Hide current top panel (after snapshot)
        if (!mgr.panel_stack_.empty()) {
            lv_obj_t* current_top = mgr.panel_stack_.back();
            lv_obj_add_flag(current_top, LV_OBJ_FLAG_HIDDEN);
        }

        // Show overlay with zoom animation instead of slide
        lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay_panel);
        mgr.panel_stack_.push_back(overlay_panel);
        mgr.overlay_animate_zoom_in(overlay_panel, source_rect);

        // Lifecycle: Activate new overlay
        auto* lifecycle = mgr.resolve_overlay_lifecycle(overlay_panel);
        if (!lifecycle) {
            bool registered = mgr.overlay_instances_.count(overlay_panel) ||
                              mgr.persistent_overlay_instances_.count(overlay_panel);
            if (!registered) {
                spdlog::warn("[NavigationManager] Overlay {} pushed without lifecycle registration",
                             (void*)overlay_panel);
            }
        } else {
            lifecycle->on_activate();
        }

        SoundManager::instance().play("nav_forward");
        spdlog::trace("[NavigationManager] Pushed overlay {} with zoom (stack: {})",
                      (void*)overlay_panel, mgr.panel_stack_.size());
    });
}

void NavigationManager::register_overlay_close_callback(lv_obj_t* overlay_panel,
                                                        OverlayCloseCallback callback) {
    if (!overlay_panel || !callback) {
        return;
    }
    overlay_close_callbacks_[overlay_panel] = std::move(callback);
    spdlog::trace("[NavigationManager] Registered close callback for overlay {}",
                  (void*)overlay_panel);
}

void NavigationManager::unregister_overlay_close_callback(lv_obj_t* overlay_panel) {
    auto it = overlay_close_callbacks_.find(overlay_panel);
    if (it != overlay_close_callbacks_.end()) {
        overlay_close_callbacks_.erase(it);
        spdlog::trace("[NavigationManager] Unregistered close callback for overlay {}",
                      (void*)overlay_panel);
    }
}

bool NavigationManager::go_back() {
    helix::ui::queue_update([]() {
        auto& mgr = NavigationManager::instance();
        spdlog::trace("[NavigationManager] go_back executing, stack depth: {}",
                      mgr.panel_stack_.size());
        crash_handler::breadcrumb::note("nav",  "go_back",
                                        static_cast<long>(mgr.panel_stack_.size()));

        // Dismiss keyboard before navigation to restore screen position
        if (KeyboardManager::instance().is_visible()) {
            KeyboardManager::instance().hide();
        }

        lv_obj_t* current_top = mgr.panel_stack_.empty() ? nullptr : mgr.panel_stack_.back();

        // Check if current top is an overlay
        bool is_overlay = false;
        if (current_top) {
            is_overlay = true;
            for (int j = 0; j < UI_PANEL_COUNT; j++) {
                if (mgr.panel_widgets_[j] == current_top) {
                    is_overlay = false;
                    break;
                }
            }
        }

        // Lifecycle: Deactivate the closing overlay before animation
        if (is_overlay && current_top) {
            // Remove overlay from focus group BEFORE closing to prevent LVGL from
            // auto-focusing the next element (which triggers scroll-on-focus)
            lv_group_t* group = lv_group_get_default();
            if (group) {
                lv_group_remove_obj(current_top);
            }

            auto it = mgr.overlay_instances_.find(current_top);
            if (it != mgr.overlay_instances_.end() && it->second) {
                spdlog::trace("[NavigationManager] Deactivating closing overlay {}",
                              it->second->get_name());
                it->second->on_deactivate();
            }
        }

        // Pop stack and clean up backdrop BEFORE animation — the no-animation path
        // and the animation completion callback both expect the stack to already
        // reflect the post-pop state when activating the previous panel/overlay.
        if (!mgr.panel_stack_.empty()) {
            lv_obj_t* popped = mgr.panel_stack_.back();
            mgr.panel_stack_.pop_back();
            auto it = mgr.overlay_backdrops_.find(popped);
            if (it != mgr.overlay_backdrops_.end()) {
                helix::ui::safe_delete_deferred(it->second);
                mgr.overlay_backdrops_.erase(it);
            }
        }

        // Determine the previous panel (what will be visible after pop)
        lv_obj_t* previous_panel = mgr.panel_stack_.empty() ? nullptr : mgr.panel_stack_.back();

        // Animate out if overlay (zoom-out for zoomed overlays, slide-out otherwise)
        if (is_overlay && current_top) {
            auto zoom_it = mgr.zoom_source_rects_.find(current_top);
            if (zoom_it != mgr.zoom_source_rects_.end()) {
                lv_area_t source_rect = zoom_it->second;
                mgr.zoom_source_rects_.erase(zoom_it);
                mgr.overlay_animate_zoom_out(current_top, source_rect);
            } else {
                mgr.overlay_animate_slide_out(current_top);
            }
            SoundManager::instance().play("nav_back");
        }

        // Hide stale overlays (but skip current_top, previous panel, and system widgets)
        lv_obj_t* screen = lv_screen_active();
        if (screen) {
            for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
                lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
                if (child == mgr.app_layout_widget_ || child == mgr.overlay_backdrop_ ||
                    child == current_top || child == previous_panel) {
                    continue;
                }
                bool is_main = false;
                for (int j = 0; j < UI_PANEL_COUNT; j++) {
                    if (mgr.panel_widgets_[j] == child) {
                        is_main = true;
                        break;
                    }
                }
                if (!is_main && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                    // Always reset styles unconditionally. The conditional check
                    // (reading transform_scale_x/y before writing) was an ARM perf
                    // optimization but may cause display corruption on Android where
                    // stale transform values interact with SDL's logical scaling.
                    // TODO: Re-enable conditional reset if this doesn't fix Android
                    // corruption (see prestonbrown/helixscreen — v0.13.7 regression).
                    //
                    // Was:
                    //   bool needs_reset =
                    //       lv_obj_get_style_translate_x(child, LV_PART_MAIN) != 0 ||
                    //       ...transform_scale_x != 256 || ...scale_y != 256...
                    //       ...opa != LV_OPA_COVER;
                    //   if (needs_reset) { ... }
                    lv_obj_set_style_translate_x(child, 0, LV_PART_MAIN);
                    lv_obj_set_style_translate_y(child, 0, LV_PART_MAIN);
                    lv_obj_set_style_transform_scale(child, 256, LV_PART_MAIN);
                    lv_obj_set_style_opa(child, LV_OPA_COVER, LV_PART_MAIN);
                }
            }
        }

        // Destroy backdrop if no more overlays
        if (mgr.panel_stack_.size() <= 1 && mgr.overlay_backdrop_) {
            helix::ui::safe_delete_deferred(mgr.overlay_backdrop_);
            mgr.overlay_backdrop_ = nullptr;
        }

        // Fallback to home if empty
        if (mgr.panel_stack_.empty()) {
            spdlog::trace("[NavigationManager] go_back stack empty, falling back to HOME");
            for (int i = 0; i < UI_PANEL_COUNT; i++) {
                if (mgr.panel_widgets_[i])
                    lv_obj_add_flag(mgr.panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (mgr.panel_widgets_[static_cast<int>(PanelId::Home)]) {
                lv_obj_remove_flag(mgr.panel_widgets_[static_cast<int>(PanelId::Home)],
                                   LV_OBJ_FLAG_HIDDEN);
                mgr.panel_stack_.push_back(mgr.panel_widgets_[static_cast<int>(PanelId::Home)]);
                mgr.active_panel_ = PanelId::Home;
                lv_subject_set_int(&mgr.active_panel_subject_, static_cast<int>(PanelId::Home));
            }
            return;
        }

        // Show previous panel
        lv_obj_t* prev = mgr.panel_stack_.back();
        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            if (mgr.panel_widgets_[i] == prev) {
                for (int j = 0; j < UI_PANEL_COUNT; j++) {
                    if (j != i && mgr.panel_widgets_[j])
                        lv_obj_add_flag(mgr.panel_widgets_[j], LV_OBJ_FLAG_HIDDEN);
                }
                mgr.active_panel_ = static_cast<PanelId>(i);
                lv_subject_set_int(&mgr.active_panel_subject_, i);
                break;
            }
        }
        lv_obj_remove_flag(prev, LV_OBJ_FLAG_HIDDEN);

        // Lifecycle: Re-activate the panel being restored
        auto it = mgr.overlay_instances_.find(prev);
        if (it != mgr.overlay_instances_.end() && it->second) {
            spdlog::trace("[NavigationManager] Re-activating restored overlay {}",
                          it->second->get_name());
            it->second->on_activate();
        }
    });
    return true;
}

bool NavigationManager::is_panel_in_stack(lv_obj_t* panel) const {
    if (!panel) {
        return false;
    }
    return std::find(panel_stack_.begin(), panel_stack_.end(), panel) != panel_stack_.end();
}

bool NavigationManager::has_open_overlays() const {
    // Only check the panel stack — it tracks what's actually open/visible.
    // overlay_instances_ is a registration map (persistent overlays survive
    // panel switches) and must NOT be used here, or persistent registrations
    // cause this to return true even when nothing is visibly open.
    return panel_stack_.size() > 1;
}

void NavigationManager::shutdown() {
    spdlog::trace("[NavigationManager] Shutting down...");
    shutting_down_ = true;

    // Hide printer switch menu if open (its widget is a child of the screen)
    printer_switch_menu_.hide();

    // Deactivate any overlays in the stack
    for (lv_obj_t* overlay_widget : panel_stack_) {
        auto it = overlay_instances_.find(overlay_widget);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::trace("[NavigationManager] Deactivating overlay: {}", it->second->get_name());
            it->second->on_deactivate();
        }
    }

    // Clear overlay registry
    // Note: The actual panel objects are destroyed via StaticPanelRegistry,
    // we just clear our tracking references here
    overlay_instances_.clear();
    persistent_overlay_instances_.clear();

    // Clear panel instances
    for (auto& panel : panel_instances_) {
        panel = nullptr;
    }

    // Clear connection status dot observer
    printer_dot_observer_.reset();
    printer_dot_widget_ = nullptr;

    // Clear panel stack and zoom state
    panel_stack_.clear();
    zoom_source_rects_.clear();

    // Clear printer callbacks — they capture Application pointers that become
    // invalid after soft restart tears down and rebuilds printer state
    printer_switch_cb_ = nullptr;
    add_printer_cb_ = nullptr;

    spdlog::trace("[NavigationManager] Shutdown complete");
}

void NavigationManager::set_backdrop_visible(bool visible) {
    if (!subjects_initialized_) {
        spdlog::warn(
            "[NavigationManager] Subjects not initialized, cannot set backdrop visibility");
        return;
    }

    lv_subject_set_int(&overlay_backdrop_visible_subject_, visible ? 1 : 0);
    spdlog::trace("[NavigationManager] Overlay backdrop visibility set to: {}", visible);
}

void NavigationManager::set_printer_callbacks(PrinterSwitchCallback switch_cb,
                                              AddPrinterCallback add_cb) {
    printer_switch_cb_ = std::move(switch_cb);
    add_printer_cb_ = std::move(add_cb);
}

void NavigationManager::trigger_printer_switch(const std::string& printer_id) {
    if (printer_switch_cb_) {
        printer_switch_cb_(printer_id);
    } else {
        spdlog::warn("[NavigationManager] No printer switch callback registered");
    }
}

void NavigationManager::trigger_add_printer() {
    if (add_printer_cb_) {
        add_printer_cb_();
    } else {
        spdlog::warn("[NavigationManager] No add printer callback registered");
    }
}

void NavigationManager::on_printer_badge_clicked() {
    if (printer_switch_menu_.is_visible()) {
        printer_switch_menu_.hide();
        return;
    }

    lv_obj_t* badge = lv_obj_find_by_name(navbar_widget_, "nav_printer_badge");
    if (!badge)
        return;

    lv_obj_t* screen = lv_obj_get_screen(navbar_widget_);

    printer_switch_menu_.set_switch_callback(
        [this](helix::ui::PrinterSwitchMenu::MenuAction action, const std::string& printer_id) {
            switch (action) {
            case helix::ui::PrinterSwitchMenu::MenuAction::SWITCH:
                spdlog::info("[Nav] Switching to printer '{}'", printer_id);
                if (printer_switch_cb_) {
                    printer_switch_cb_(printer_id);
                }
                break;
            case helix::ui::PrinterSwitchMenu::MenuAction::ADD_PRINTER:
                spdlog::info("[Nav] Adding new printer via wizard");
                if (add_printer_cb_) {
                    add_printer_cb_();
                }
                break;
            case helix::ui::PrinterSwitchMenu::MenuAction::CANCELLED:
                break;
            }
        });

    printer_switch_menu_.show(screen, badge);
}

void NavigationManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Reset observer guards BEFORE deiniting subjects - they hold references
    // to subjects that will become invalid. Also handles observers attached
    // to external subjects (PrinterState) that may be reset separately.
    active_panel_observer_.reset();
    connection_state_observer_.reset();
    klippy_state_observer_.reset();

    subjects_.deinit_all();

    // Reset widget pointers - they become invalid when LVGL is reinitialized
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panel_widgets_[i] = nullptr;
        panel_instances_[i] = nullptr;
    }
    overlay_instances_.clear();
    persistent_overlay_instances_.clear();
    overlay_close_callbacks_.clear();
    overlay_backdrops_.clear();
    zoom_source_rects_.clear();
    panel_stack_.clear();
    app_layout_widget_ = nullptr;
    if (overlay_backdrop_) {
        lv_obj_del(overlay_backdrop_);
        overlay_backdrop_ = nullptr;
    }
    navbar_widget_ = nullptr;
    active_panel_ = PanelId::Home;
    previous_connection_state_ = -1;
    previous_klippy_state_ = -1;

    // Allow re-initialization after soft restart (shutdown() sets this to true)
    shutting_down_ = false;

    subjects_initialized_ = false;
    spdlog::trace("[NavigationManager] Subjects deinitialized");
}

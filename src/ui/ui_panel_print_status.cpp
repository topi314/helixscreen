// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_print_status.h"

#include "ui_ams_current_tool.h"
#include "ui_callback_helpers.h"
#include "ui_component_header_bar.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_exclude_object_map_view.h"
#include "ui_exclude_objects_list_overlay.h"
#include "ui_filename_utils.h"
#include "ui_gcode_viewer.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui/ui_widget_helpers.h"

#include "abort_manager.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "injection_point_manager.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "preprint_predictor.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"
#include "tool_state.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::gcode::resolve_gcode_filename;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

// Global instance for legacy API and resize callback
static std::unique_ptr<PrintStatusPanel> g_print_status_panel;

// Cached widget pointer for lazy creation (separate from overlay_root_ which
// is managed by OverlayBase). Declared here so teardown callback can null it.
static lv_obj_t* s_cached_panel = nullptr;

// ID of the MemoryMonitor pressure responder. 0 means "not registered".
// Registered lazily on first push_overlay(); unregistered in the static
// panel-destroy callback to prevent calls into a destroyed singleton.
static helix::MemoryMonitor::PressureResponderId s_memory_responder_id = 0;

using helix::ui::temperature::centi_to_degrees;
using helix::ui::temperature::format_temperature_pair;

// Observer factory pattern
using helix::ui::observe_int_sync;
using helix::ui::observe_print_state;
using helix::ui::observe_string;

// Helper to get or create the global instance
PrintStatusPanel& get_global_print_status_panel() {
    if (!g_print_status_panel) {
        g_print_status_panel = std::make_unique<PrintStatusPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("PrintStatusPanel", []() {
            if (s_memory_responder_id != 0) {
                helix::MemoryMonitor::instance().remove_pressure_responder(s_memory_responder_id);
                s_memory_responder_id = 0;
            }
            if (s_cached_panel && g_print_status_panel) {
                g_print_status_panel->destroy_overlay_ui(s_cached_panel);
            }
            s_cached_panel = nullptr;
            g_print_status_panel.reset();
        });
    }
    return *g_print_status_panel;
}

// Drop the cached widget tree if we can, to reclaim ~400-800KB.
// Safe to call from any thread — hops to UI thread via queue_update and
// bails out if the overlay is currently visible. No-op when there's no
// cached tree (e.g. print status was never opened this session).
static void try_reclaim_cached_print_status() {
    helix::ui::queue_update([]() {
        if (!s_cached_panel) {
            return;
        }
        if (NavigationManager::instance().is_panel_in_stack(s_cached_panel)) {
            spdlog::debug(
                "[PrintStatusPanel] Cached tree is currently visible, skipping memory reclaim");
            return;
        }
        if (!g_print_status_panel) {
            return;
        }
        spdlog::warn("[PrintStatusPanel] Pressure response: destroying cached overlay tree");
        g_print_status_panel->destroy_overlay_ui(s_cached_panel);
        // destroy_overlay_ui() nulls s_cached_panel via its by-ref parameter;
        // next push_overlay() will lazily recreate.
    });
}

PrintStatusPanel::PrintStatusPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {
    // Pre-init local subject used by observer callback below (fires immediately on subscribe)
    lv_subject_init_int(&exclude_objects_available_subject_, 0);

    // Subscribe to temperature subjects using bundle (replaces 4 individual observers)
    temp_observers_.setup_sync(
        this, printer_state_, [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); });

    // Subscribe to active tool changes (refreshes nozzle temp with tool name prefix)
    active_tool_observer_ = observe_int_sync<PrintStatusPanel>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); });

    // Chamber status text: observe chamber temp to compute Heating/Cooling/Holding status
    chamber_temp_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_chamber_temp_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_chamber_status(); });

    // Subscribe to print progress and state
    print_progress_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_progress_subject(), this,
        [](PrintStatusPanel* self, int progress) { self->on_print_progress_changed(progress); });
    print_state_observer_ = observe_print_state<PrintStatusPanel>(
        printer_state_.get_print_state_enum_subject(), this,
        [](PrintStatusPanel* self, PrintJobState state) { self->on_print_state_changed(state); });
    print_filename_observer_ =
        observe_string<PrintStatusPanel>(printer_state_.get_print_filename_subject(), this,
                                         [](PrintStatusPanel* self, const char* filename) {
                                             self->on_print_filename_changed(filename);
                                         });

    // Subscribe to speed/flow factors
    speed_factor_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_speed_factor_subject(), this,
        [](PrintStatusPanel* self, int speed) { self->on_speed_factor_changed(speed); });
    flow_factor_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_flow_factor_subject(), this,
        [](PrintStatusPanel* self, int flow) { self->on_flow_factor_changed(flow); });
    gcode_z_offset_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_gcode_z_offset_subject(), this,
        [](PrintStatusPanel* self, int microns) { self->on_gcode_z_offset_changed(microns); });

    // Subscribe to layer tracking for G-code viewer ghost layer updates
    print_layer_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_layer_current_subject(), this,
        [](PrintStatusPanel* self, int layer) { self->on_print_layer_changed(layer); });

    // Re-render layer text when Z position changes (Z updates more frequently than layer count)
    z_position_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_gcode_position_z_subject(), this, [](PrintStatusPanel* self, int) {
            int layer = lv_subject_get_int(self->printer_state_.get_print_layer_current_subject());
            self->on_print_layer_changed(layer);
        });

    // Subscribe to wall-clock elapsed time (total_duration includes prep time)
    print_duration_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_elapsed_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_print_duration_changed(seconds); });
    print_time_left_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_time_left_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_print_time_left_changed(seconds); });

    // Subscribe to print start preparation phase subjects
    print_start_phase_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_start_phase_subject(), this,
        [](PrintStatusPanel* self, int phase) { self->on_print_start_phase_changed(phase); });
    print_start_message_observer_ =
        observe_string<PrintStatusPanel>(printer_state_.get_print_start_message_subject(), this,
                                         [](PrintStatusPanel* self, const char* message) {
                                             self->on_print_start_message_changed(message);
                                         });
    print_start_progress_observer_ =
        observe_int_sync<PrintStatusPanel>(printer_state_.get_print_start_progress_subject(), this,
                                           [](PrintStatusPanel* self, int progress) {
                                               self->on_print_start_progress_changed(progress);
                                           });
    preprint_remaining_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_preprint_remaining_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_preprint_remaining_changed(seconds); });
    preprint_elapsed_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_preprint_elapsed_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_preprint_elapsed_changed(seconds); });

    // Subscribe to defined objects changes (for objects list button visibility + count)
    exclude_objects_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_defined_objects_version_subject(), this,
        [](PrintStatusPanel* self, int) {
            int available = self->printer_state_.get_defined_objects().size() >= 2 ? 1 : 0;
            lv_subject_set_int(&self->exclude_objects_available_subject_, available);
            self->update_objects_text();
            self->update_view_toggle_position(available != 0);
        });

    // Subscribe to excluded objects changes (for "X of Y obj" count updates)
    excluded_objects_version_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_excluded_objects_version_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_objects_text(); });

    // Subscribe to AMS current filament color for gcode viewer color override
    // When a known filament color is available (from Spoolman spool or AMS lane),
    // use it instead of the gcode metadata color for the 2D/3D render
    ams_color_observer_ = observe_int_sync<PrintStatusPanel>(
        AmsState::instance().get_current_color_subject(), this,
        [](PrintStatusPanel* self, int /*color_rgb*/) { self->build_and_apply_tool_colors(); });

    // Also refresh gcode viewer colors when tool_to_slot_map changes (user remap)
    tool_map_version_observer_ = observe_int_sync<PrintStatusPanel>(
        AmsState::instance().get_tool_map_version_subject(), this,
        [](PrintStatusPanel* self, int /*version*/) { self->build_and_apply_tool_colors(); });

    // Subscribe to shared print thumbnail path set by ActivePrintMediaManager.
    // Use observe_string_immediate: the handler only calls lv_image_set_src
    // (no observer lifecycle changes), and set_print_thumbnail_path is always called
    // from the UI thread via queue_update.
    print_thumbnail_path_observer_ = ui::observe_string_immediate<PrintStatusPanel>(
        printer_state_.get_print_thumbnail_path_subject(), this,
        [](PrintStatusPanel* self, const char* path) {
            if (!path || path[0] == '\0')
                return;
            self->cached_thumbnail_path_ = path;
            if (self->print_thumbnail_) {
                lv_image_set_src(self->print_thumbnail_, path);
                spdlog::debug("[{}] Thumbnail updated from shared subject: {}", self->get_name(),
                              path);
            }
        });

    spdlog::debug("[{}] Subscribed to PrinterState subjects", get_name());

    // LED configuration is read lazily by PrintLightTimelapseControls::handle_light_button()
    // At construction time, hardware discovery may not have completed yet.
    // LED state observer is set up on first on_activate() when strips are available.
    led_state_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_led_state_subject(), this,
        [](PrintStatusPanel* self, int state) { self->on_led_state_changed(state); });
    spdlog::debug("[{}] LED state observer registered (strips read lazily)", get_name());

    // Subscribe to G-code render mode changes from settings panel
    // This allows real-time updates to the viewer when the user changes the setting
    gcode_render_mode_observer_ = observe_int_sync<PrintStatusPanel>(
        DisplaySettingsManager::instance().subject_gcode_render_mode(), this,
        [](PrintStatusPanel* self, int mode) {
            spdlog::info("[{}] G-code render mode changed from settings: {}", self->get_name(),
                         mode);
            if (self->gcode_viewer_ && self->is_active_) {
                // Apply the new render mode (skip "Thumbnail Only" mode = 3)
                if (mode == 3) {
                    // Thumbnail Only - hide the viewer
                    self->show_gcode_viewer(false);
                } else {
                    auto render_mode = static_cast<GcodeViewerRenderMode>(mode);
                    ui_gcode_viewer_set_render_mode(self->gcode_viewer_, render_mode);
                    // Update viewer mode subject to trigger XML visibility bindings
                    if (self->gcode_loaded_) {
                        self->show_gcode_viewer(true);
                    }
                }
            }
        });
    spdlog::debug("[{}] G-code render mode observer registered", get_name());

    // End-overlay visibility: derive three show_* bool subjects from print_outcome
    // and end_overlay_dismissed_. XML binds each overlay's hidden flag to a single
    // subject, avoiding the L042 two-observer race that made the error overlay
    // pop at startup when end_overlay_dismissed==0 unhide-raced the outcome check.
    print_outcome_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_outcome_subject(), this,
        [](PrintStatusPanel* self, int) { self->recompute_end_overlay_visibility(); });
    recompute_end_overlay_visibility();

    // Create filament runout handler (extracted from PrintStatusPanel)
    runout_handler_ = std::make_unique<helix::ui::FilamentRunoutHandler>(api_);
    spdlog::debug("[{}] Created filament runout handler", get_name());
}

PrintStatusPanel::~PrintStatusPanel() {
    deinit_subjects();

    // Expire all outstanding async callback tokens before destroying resources
    lifetime_.invalidate();

    // Cancel pending deferred G-code load timer
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    // ObserverGuard handles observer cleanup automatically
    resize_registered_ = false;

    // Clean up temp G-code file if any
    if (!temp_gcode_path_.empty()) {
        std::remove(temp_gcode_path_.c_str());
        temp_gcode_path_.clear();
    }

    // CRITICAL: Check if LVGL is still initialized before calling LVGL functions.
    // During static destruction, LVGL may already be torn down.
    if (lv_is_initialized()) {
        // Note: lv_anim_delete() is NOT called here for bar widgets because
        // LVGL bar animations use var=&bar->cur_value_anim (internal struct),
        // not the bar object pointer. Passing the bar pointer misses the
        // animation entirely. lv_bar_destructor() handles cancellation
        // correctly using the internal pointers when lv_obj_delete() runs.

        // Deinit exclude manager before LVGL teardown
        if (exclude_manager_) {
            exclude_manager_->deinit();
        }
        // Modal subclasses (runout_modal_, etc.) use RAII cleanup
        // Their destructors will call hide() automatically
    }
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void PrintStatusPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize all subjects with default values
    // Note: Display filename is now handled by ActivePrintMediaManager via print_display_filename
    UI_MANAGED_SUBJECT_STRING(progress_text_subject_, progress_text_buf_, "0%",
                              "print_progress_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(layer_text_subject_, layer_text_buf_, "Layer 0 / 0",
                              "print_layer_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(filament_used_text_subject_, filament_used_text_buf_, "",
                              "print_filament_used_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(elapsed_subject_, elapsed_buf_, "0h 00m", "print_elapsed", subjects_);
    UI_MANAGED_SUBJECT_STRING(remaining_subject_, remaining_buf_, "0h 00m", "print_remaining",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(eta_subject_, eta_buf_, "", "print_eta", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_temp_subject_, nozzle_temp_buf_, "0 / 0°C", "nozzle_temp_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_temp_subject_, bed_temp_buf_, "0 / 0°C", "bed_temp_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_status_subject_, nozzle_status_buf_, "Off",
                              "print_nozzle_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_status_subject_, bed_status_buf_, "Off", "print_bed_status",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(chamber_status_subject_, chamber_status_buf_, "",
                              "print_chamber_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(speed_subject_, speed_buf_, "100%", "print_speed_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(flow_subject_, flow_buf_, "100%", "print_flow_text", subjects_);
    // Pause button icon - MDI icons (pause=F03E4, play=F040A)
    // UTF-8: pause=F3 B0 8F A4, play=F3 B0 90 8A
    UI_MANAGED_SUBJECT_STRING(pause_button_subject_, pause_button_buf_, "\xF3\xB0\x8F\xA4",
                              "pause_button_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(pause_label_subject_, pause_label_buf_, "Pause", "pause_button_label",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(objects_text_subject_, objects_text_buf_, "", "print_objects_text",
                              subjects_);
    // View toggle icon: starts as cube (progress view), flips to layers on complete view.
    // Populated lazily at first update (icon_cube const resolves only after globals load).
    UI_MANAGED_SUBJECT_STRING(view_toggle_icon_subject_, view_toggle_icon_buf_, "",
                              "view_toggle_icon", subjects_);

    // Initialize light/timelapse controls (extracted Phase 2)
    light_timelapse_controls_.init_subjects();
    light_timelapse_controls_.set_api(api_);
    set_global_light_timelapse_controls(&light_timelapse_controls_);

    // Preparing state subjects
    UI_MANAGED_SUBJECT_INT(preparing_visible_subject_, 0, "preparing_visible", subjects_);
    UI_MANAGED_SUBJECT_STRING(preparing_operation_subject_, preparing_operation_buf_,
                              "Preparing...", "preparing_operation", subjects_);
    UI_MANAGED_SUBJECT_INT(preparing_progress_subject_, 0, "preparing_progress", subjects_);

    // Progress bar subject (integer 0-100 for XML bind_value)

    // Viewer mode subject (0=thumbnail, 1=3D gcode viewer, 2=2D gcode viewer)
    UI_MANAGED_SUBJECT_INT(gcode_viewer_mode_subject_, 0, "gcode_viewer_mode", subjects_);
    UI_MANAGED_SUBJECT_INT(exclude_map_active_subject_, 0, "exclude_map_active", subjects_);
    UI_MANAGED_SUBJECT_INT(end_overlay_dismissed_subject_, 0, "end_overlay_dismissed", subjects_);
    end_overlay_dismissed_observer_ = observe_int_sync<PrintStatusPanel>(
        &end_overlay_dismissed_subject_, this,
        [](PrintStatusPanel* self, int) { self->recompute_end_overlay_visibility(); });

    // Derived show flags — computed in recompute_end_overlay_visibility() from
    // print_outcome + end_overlay_dismissed. Replaces the racy pair of XML
    // bind_flag observers per overlay (issue L042).
    UI_MANAGED_SUBJECT_INT(show_complete_overlay_subject_, 0, "show_complete_overlay", subjects_);
    UI_MANAGED_SUBJECT_INT(show_cancelled_overlay_subject_, 0, "show_cancelled_overlay", subjects_);
    UI_MANAGED_SUBJECT_INT(show_error_overlay_subject_, 0, "show_error_overlay", subjects_);

    // Pause overlay subjects + observer on print_stats.message. The state-based
    // visibility (show_paused_overlay) is driven from on_print_state_changed(),
    // which already runs on every PrintJobState transition. The reason text,
    // however, can mutate while the printer remains in PAUSED (Klipper updates
    // print_stats.message), so we also recompute on message change.
    UI_MANAGED_SUBJECT_INT(show_paused_overlay_subject_, 0, "show_paused_overlay", subjects_);
    UI_MANAGED_SUBJECT_STRING(print_pause_reason_subject_, print_pause_reason_buf_, "",
                              "print_pause_reason", subjects_);
    UI_MANAGED_SUBJECT_INT(print_pause_reason_visible_subject_, 0, "print_pause_reason_visible",
                           subjects_);
    print_message_observer_ = observe_string<PrintStatusPanel>(
        printer_state_.get_print_message_subject(), this,
        [](PrintStatusPanel* self, const char*) { self->recompute_paused_overlay_visibility(); });

    // Button enable states driven declaratively from XML (see update_button_states).
    UI_MANAGED_SUBJECT_INT(print_controls_enabled_subject_, 0, "print_controls_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(btn_pause_enabled_subject_, 0, "btn_pause_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(btn_cancel_enabled_subject_, 0, "btn_cancel_enabled", subjects_);

    // Exclude objects availability (0=hidden, 1=visible - shown when >= 2 objects defined)
    // Note: subject already initialized in constructor (needed before observer fires)
    lv_xml_register_subject(nullptr, "exclude_objects_available",
                            &exclude_objects_available_subject_);
    subjects_.register_subject(&exclude_objects_available_subject_);
    SubjectDebugRegistry::instance().register_subject(&exclude_objects_available_subject_,
                                                      "exclude_objects_available",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    // Register XML event callbacks for print status panel buttons
    // (tune overlay subjects/callbacks registered by singleton on first show())
    // (light and timelapse callbacks are registered by light_timelapse_controls_.init_subjects())
    register_xml_callbacks({
        {"on_print_status_pause", on_pause_clicked},
        {"on_print_status_tune", on_tune_clicked},
        {"on_print_status_cancel", on_cancel_clicked},
        {"on_print_status_reprint", on_reprint_clicked},
        {"on_temp_card_clicked", on_temp_card_clicked},
        {"on_print_status_objects", on_objects_clicked},
        {"on_view_toggle", on_view_toggle_clicked},
        {"on_print_status_dismiss_overlay", on_dismiss_overlay_clicked},
    });

    subjects_initialized_ = true;

    // Initial sync of the paused overlay — observers only fire on CHANGE, so
    // a mid-print attach where state is already PAUSED would leave the badge
    // hidden without this explicit recompute.
    recompute_paused_overlay_visibility();

    // Sync initial state from PrinterState (in case app opens while print is in progress)
    // This is necessary because observers only fire on VALUE CHANGE, not on subscribe.
    int initial_progress = lv_subject_get_int(printer_state_.get_print_progress_subject());
    int initial_layer = lv_subject_get_int(printer_state_.get_print_layer_current_subject());
    int initial_total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
    if (initial_progress > 0 || initial_layer > 0 || initial_total_layers > 0) {
        lifecycle_.on_progress_changed(initial_progress);
        lifecycle_.on_layer_changed(initial_layer, initial_total_layers,
                                    printer_state_.has_real_layer_data());
        update_all_displays();
        spdlog::debug("[{}] Synced initial print state: progress={}%, layer={}/{}", get_name(),
                      initial_progress, initial_layer, initial_total_layers);
    }

    // Sync initial preparation state from PrinterState (in case panel opens mid-preparation)
    int initial_phase = lv_subject_get_int(printer_state_.get_print_start_phase_subject());
    if (initial_phase != 0) {
        on_print_start_phase_changed(initial_phase);
        auto* msg = lv_subject_get_string(printer_state_.get_print_start_message_subject());
        on_print_start_message_changed(msg);
        int prog = lv_subject_get_int(printer_state_.get_print_start_progress_subject());
        on_print_start_progress_changed(prog);
        spdlog::debug("[{}] Synced initial preparation state: phase={}, progress={}%", get_name(),
                      initial_phase, prog);
    }

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "PrintStatusPanelSubjects", []() { get_global_print_status_panel().deinit_subjects(); });

    spdlog::debug("[{}] Subjects initialized (20 subjects)", get_name());
}

void PrintStatusPanel::deinit_subjects() {
    if (!subjects_initialized_)
        return;

    // Tune overlay singleton handles its own cleanup via StaticPanelRegistry

    // Clear light/timelapse global accessor
    set_global_light_timelapse_controls(nullptr);
    light_timelapse_controls_.deinit_subjects();

    // Reset observers on local subjects BEFORE deinit frees them.
    // subjects_.deinit_all() calls lv_subject_deinit which frees observer
    // structs — any ObserverGuard still holding a pointer would crash
    // in its destructor trying to lv_observer_remove() on freed memory.
    end_overlay_dismissed_observer_.reset();
    print_message_observer_.reset();

    temp_observers_.clear();
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[PrintStatusPanel] Subjects deinitialized");
}

lv_obj_t* PrintStatusPanel::create(lv_obj_t* parent) {
    parent_screen_ = parent;

    // Create overlay root from XML
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, get_xml_component_name(), nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Setting up panel...", get_name());

    // Panel width is set via XML using #overlay_panel_width_large (same as print_file_detail)
    // Use standard overlay panel setup for header/content/back button
    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");

    // Store header reference for e-stop visibility control
    overlay_header_ = lv_obj_find_by_name(overlay_root_, "overlay_header");

    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return nullptr;
    }

    // Find thumbnail section for nested widgets
    lv_obj_t* thumbnail_section = lv_obj_find_by_name(overlay_content, "thumbnail_section");
    if (!thumbnail_section) {
        spdlog::error("[{}] thumbnail_section not found!", get_name());
        return nullptr;
    }

    // Find G-code viewer, thumbnail, and gradient background widgets
    gcode_viewer_ = lv_obj_find_by_name(thumbnail_section, "print_gcode_viewer");
    print_thumbnail_ = lv_obj_find_by_name(thumbnail_section, "print_thumbnail");
    gradient_background_ = lv_obj_find_by_name(thumbnail_section, "gradient_background");

    if (gcode_viewer_) {
        spdlog::debug("[{}]   ✓ G-code viewer widget found", get_name());

        // Apply render mode - priority: cmdline > env var > settings
        // Note: HELIX_GCODE_MODE env var is handled at widget creation, so we only
        // override if there's an explicit command-line option or if no env var was set
        const auto* config = get_runtime_config();
        const char* env_mode = std::getenv("HELIX_GCODE_MODE");

        if (config->gcode_render_mode >= 0) {
            // Command line takes highest priority
            auto render_mode = static_cast<GcodeViewerRenderMode>(config->gcode_render_mode);
            ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
            spdlog::debug("[{}]   ✓ Set G-code render mode: {} (cmdline)", get_name(),
                          config->gcode_render_mode);
        } else if (env_mode) {
            // Env var already applied at widget creation - just log
            spdlog::debug("[{}]   ✓ G-code render mode: {} (env var)", get_name(),
                          ui_gcode_viewer_is_using_2d_mode(gcode_viewer_) ? "2D" : "3D");
        } else {
            // No cmdline or env var - apply saved settings
            int render_mode_val = DisplaySettingsManager::instance().get_gcode_render_mode();
            if (render_mode_val == 3) {
                // Thumbnail Only mode - skip render mode setup, viewer won't be used
                spdlog::debug("[{}]   ✓ G-code render mode: Thumbnail Only (settings)", get_name());
            } else {
                auto render_mode = static_cast<GcodeViewerRenderMode>(render_mode_val);
                ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
                spdlog::debug("[{}]   ✓ Set G-code render mode: {} (settings)", get_name(),
                              render_mode_val);
            }
        }

        // Create and initialize exclude object manager
        exclude_manager_ = std::make_unique<helix::ui::PrintExcludeObjectManager>(
            api_, printer_state_, gcode_viewer_);
        exclude_manager_->init();
        spdlog::debug("[{}]   ✓ Created and initialized exclude object manager", get_name());

        // Vertical offset to match thumbnail positioning (tuned empirically)
        ui_gcode_viewer_set_content_offset_y(gcode_viewer_, -0.10f);

        // Memory-pressure responder calls ui_gcode_viewer_clear_all_active().
        // Flip our mode subject back to thumbnail (0) so the user sees the
        // slicer preview rather than a transparent rectangle.
        ui_gcode_viewer_set_clear_callback(
            gcode_viewer_,
            [](lv_obj_t*, void* ud) {
                auto* panel = static_cast<PrintStatusPanel*>(ud);
                panel->show_gcode_viewer(false);
                panel->gcode_loaded_ = false;
            },
            this);
    } else {
        spdlog::error("[{}]   ✗ G-code viewer widget NOT FOUND", get_name());
    }
    if (print_thumbnail_) {
        spdlog::debug("[{}]   ✓ Print thumbnail widget found", get_name());
    }
    if (gradient_background_) {
        spdlog::debug("[{}]   ✓ Gradient background widget found", get_name());
    }

    // Force layout calculation
    lv_obj_update_layout(overlay_root_);

    // Register resize callback
    if (auto* dm = DisplayManager::instance()) {
        dm->register_resize_callback(on_resize_static);
    }
    resize_registered_ = true;

    // Store button references for potential state queries (not event wiring - that's in XML)
    btn_timelapse_ = lv_obj_find_by_name(overlay_content, "btn_timelapse");
    btn_pause_ = lv_obj_find_by_name(overlay_content, "btn_pause");
    btn_tune_ = lv_obj_find_by_name(overlay_content, "btn_tune");
    btn_cancel_ = lv_obj_find_by_name(overlay_content, "btn_cancel");

    // Print complete celebration badge (for animation)
    success_badge_ = lv_obj_find_by_name(overlay_content, "success_badge");
    if (success_badge_) {
        spdlog::debug("[{}]   ✓ Success badge", get_name());
    }

    // Print cancelled badge (for animation)
    cancel_badge_ = lv_obj_find_by_name(overlay_content, "cancel_badge");
    if (cancel_badge_) {
        spdlog::debug("[{}]   ✓ Cancel badge", get_name());
    }

    // Print error badge (for animation)
    error_badge_ = lv_obj_find_by_name(overlay_content, "error_badge");
    if (error_badge_) {
        spdlog::debug("[{}]   ✓ Error badge", get_name());
    }

    // Progress bar widget
    progress_bar_ = lv_obj_find_by_name(overlay_content, "print_progress");
    if (progress_bar_) {
        lv_bar_set_range(progress_bar_, 0, 100);
        // WORKAROUND: LVGL bar has a bug where setting value=0 when cur_value=0
        // causes early return without proper layout update, showing full bar.
        // Force update by setting to 1 first, then 0.
        lv_bar_set_value(progress_bar_, 1, LV_ANIM_OFF);
        lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        spdlog::debug("[{}]   ✓ Progress bar", get_name());
    } else {
        spdlog::error("[{}]   ✗ Progress bar NOT FOUND", get_name());
    }

    // Preparing progress bar (shown during pre-print operations)
    preparing_progress_bar_ = lv_obj_find_by_name(overlay_content, "preparing_progress_bar");
    if (preparing_progress_bar_) {
        lv_bar_set_range(preparing_progress_bar_, 0, 100);
        lv_bar_set_value(preparing_progress_bar_, 0, LV_ANIM_OFF);
        spdlog::debug("[{}]   ✓ Preparing progress bar", get_name());
    }

    // AMS current tool indicator (auto-hides when no AMS or no tool active)
    lv_obj_t* ams_indicator = lv_obj_find_by_name(overlay_content, "ams_current_tool_indicator");
    if (ams_indicator) {
        ui_ams_current_tool_setup(ams_indicator);
        spdlog::debug("[{}]   ✓ AMS current tool indicator", get_name());
    }

    // Check if --gcode-file was specified on command line for this panel
    const auto* config = get_runtime_config();
    if (config->gcode_test_file && gcode_viewer_) {
        // Check file size and memory safety before loading
        // Use 2D streaming check since that's the mode used on memory-constrained devices
        std::ifstream file(config->gcode_test_file, std::ios::binary | std::ios::ate);
        if (file) {
            size_t file_size = static_cast<size_t>(file.tellg());
            if (helix::is_gcode_2d_streaming_safe(file_size)) {
                spdlog::info("[{}] Loading G-code file from command line: {}", get_name(),
                             config->gcode_test_file);
                load_gcode_file(config->gcode_test_file);
            } else {
                spdlog::warn("[{}] G-code file too large for 2D streaming: {} ({} bytes) - using "
                             "thumbnail only",
                             get_name(), config->gcode_test_file, file_size);
            }
        }
    }

    // Restore cached thumbnail if a print was already in progress before panel was displayed
    // This handles the case where a print was started from Mainsail while on the Home panel
    if (print_thumbnail_ && !cached_thumbnail_path_.empty()) {
        lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
        spdlog::info("[{}] Restored cached thumbnail: {}", get_name(), cached_thumbnail_path_);
    }

    // Register plugin injection point for print status widgets
    lv_obj_t* extras_container = lv_obj_find_by_name(overlay_root_, "print_status_extras");
    if (extras_container) {
        helix::plugin::InjectionPointManager::instance().register_point("print_status_extras",
                                                                        extras_container);
        spdlog::debug("[{}] Registered injection point: print_status_extras", get_name());
    }

    // Hide initially - NavigationManager will show when pushed
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Seed view toggle icon now that globals.xml has been loaded (init_subjects
    // runs too early to resolve #icon_cube). The subject drives the XML
    // bind_text on btn_view_toggle_icon, so this is the initial render state.
    if (const char* icon = lv_xml_get_const(nullptr, "icon_cube")) {
        lv_subject_copy_string(&view_toggle_icon_subject_, icon);
    }

    spdlog::debug("[{}] Setup complete!", get_name());
    return overlay_root_;
}

void PrintStatusPanel::on_activate() {
    // Cluster:pstat-async-delete (#906) — fine-grained crumbs through every
    // step of on_activate so the next production crash names which step left
    // the corruption rolling. Pair with the larger breadcrumb ring so these
    // survive the pre-crash tick storm.
    crash_handler::breadcrumb::note("pstat_act", "enter");
    OverlayBase::on_activate(); // Sets visible_ = true
    is_active_ = true;

    int state_enum = lv_subject_get_int(printer_state_.get_print_state_enum_subject());
    spdlog::debug("[{}] on_activate() print_state_enum={}", get_name(), state_enum);

    // Load deferred G-code if pending (lazy loading optimization)
    // This avoids downloading large files unless user navigates here
    if (!pending_gcode_filename_.empty()) {
        crash_handler::breadcrumb::note("pstat_act", "defer_gc");
        schedule_deferred_gcode_load();
    }

    // The gcode viewer may be empty after destroy-on-close recreation, or after
    // on_deactivate() cleared it due to memory pressure. Re-feed the current
    // print filename to trigger gcode + thumbnail reload.
    if (lifecycle_.want_viewer() && !gcode_loaded_ && gcode_viewer_ &&
        pending_gcode_filename_.empty()) {
        const char* filename = lv_subject_get_string(printer_state_.get_print_filename_subject());
        if (filename && filename[0] != '\0') {
            crash_handler::breadcrumb::note("pstat_act", "reload_gc");
            spdlog::info("[{}] Re-loading G-code after overlay recreate: {}", get_name(), filename);
            on_print_filename_changed(filename);
        }
    }

    // Re-apply cached thumbnail now that the panel is visible. Breadcrumb the
    // set_src so draw-time UAFs in the blend path (argb8888_image_blend reading
    // source pixels from an unmapped heap page, #851) can be pinned to this
    // reactivation site in post-hoc crash reports.
    if (print_thumbnail_ && !cached_thumbnail_path_.empty()) {
        crash_handler::breadcrumb::note("pstat_thm", "set_src_pre");
        lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
        crash_handler::breadcrumb::note("pstat_thm", "set_src_post");
    }

    // Sync button enabled/visibility state with current print state and outcome.
    // XML bindings may have been lost during overlay lifecycle transitions (#546).
    crash_handler::breadcrumb::note("pstat_act", "btn_states");
    update_button_states();

    // Restore render mode from settings before showing the viewer.
    // The render mode observer only fires when is_active_, so settings
    // changed while the panel was hidden must be applied here.
    int render_mode_val = DisplaySettingsManager::instance().get_gcode_render_mode();
    bool thumbnail_only = (render_mode_val == 3);
    if (gcode_viewer_ && !thumbnail_only) {
        auto render_mode = static_cast<GcodeViewerRenderMode>(render_mode_val);
        ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
    }

    // Restore G-code viewer state based on current print conditions.
    // Thumbnail Only mode forces the viewer off regardless of gcode state.
    bool show_viewer = !thumbnail_only && lifecycle_.want_viewer() && gcode_loaded_;
    crash_handler::breadcrumb::note("pstat_act", show_viewer ? "viewer_on" : "viewer_off");
    show_gcode_viewer(show_viewer);

    // Sync gcode viewer to current print layer (may have advanced while panel was hidden)
    if (gcode_viewer_ && gcode_loaded_ && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN)) {
        int current_layer = lv_subject_get_int(printer_state_.get_print_layer_current_subject());
        int total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
        int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
        int viewer_layer = current_layer;
        if (total_layers > 0 && viewer_max_layer > 0) {
            viewer_layer = (current_layer * viewer_max_layer) / total_layers;
        }
        ui_gcode_viewer_set_print_progress(gcode_viewer_, viewer_layer);
    }
    crash_handler::breadcrumb::note("pstat_act", "exit");
}

void PrintStatusPanel::on_deactivate() {
    OverlayBase::on_deactivate(); // Sets visible_ = false
    is_active_ = false;
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Cancel pending deferred G-code load (panel is no longer visible)
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    // Note: bar animation cancellation is handled by lv_bar_destructor()
    // when widgets are deleted. Manual lv_anim_delete(bar_ptr) uses the wrong
    // var pointer (bar animations use &bar->cur_value_anim internally).

    // Pause G-code viewer rendering when panel is hidden (CPU optimization)
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, true);

        // Release heavy renderer state when leaving the panel after a print has
        // reached a terminal state. Previously gated on system-wide available
        // memory (< 64MB) — that threshold is "kernel about to OOM," not "we're
        // using too much," so devices with abundant free RAM but heavy process
        // RSS would hold the ParsedGCodeFile + GPU geometry indefinitely after
        // a print ended (telemetry: pi32 held 632MB for 1+ hour post-print).
        // The user has navigated away, the print is over — drop the heavy
        // state. Issue #618's smoothness gain only applies while the print is
        // still active (handled by the state guard below).
        auto state = lifecycle_.state();
        if (state != PrintState::Printing && state != PrintState::Paused &&
            state != PrintState::Preparing) {
            ui_gcode_viewer_clear(gcode_viewer_);
            gcode_loaded_ = false;
            // Clear ALL dedup guards so on_activate() can re-download.
            // loaded_thumbnail_filename_ gates the entire set_filename() block
            // (thumbnail + gcode). Without clearing it, on_activate()'s reload
            // path is silently blocked when the filename hasn't changed.
            // The thumbnail re-fetch is a cache hit (file stays on disk).
            loaded_thumbnail_filename_.clear();
            requested_gcode_filename_.clear();
            pending_gcode_filename_.clear();
            spdlog::debug("[{}] Cleared gcode viewer on deactivate (terminal state)", get_name());
        }
    }

    // Hide runout guidance modal if panel is deactivated (e.g., navbar navigation)
    if (runout_handler_) {
        runout_handler_->hide_modal();
    }
}

void PrintStatusPanel::cleanup() {
    // Cancel pending deferred G-code load
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    OverlayBase::cleanup(); // Sets cleanup_called_ = true
}

void PrintStatusPanel::on_ui_destroyed() {
    spdlog::debug("[{}] on_ui_destroyed() - nulling widget pointers", get_name());

    // Cancel pending deferred G-code load
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    // Note: LVGL animations are already cancelled by lv_obj_delete() in the base
    // class destroy_overlay_ui() call, so no need to cancel them here.

    // Clean up map view before the widget tree is gone
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }

    // Deinit exclude manager (holds gcode_viewer_ reference)
    if (exclude_manager_) {
        exclude_manager_->deinit();
        exclude_manager_.reset();
    }

    // Null all child widget pointers (widget tree is already deleted by base class)
    progress_bar_ = nullptr;
    preparing_progress_bar_ = nullptr;
    gcode_viewer_ = nullptr;
    print_thumbnail_ = nullptr;
    gradient_background_ = nullptr;
    btn_timelapse_ = nullptr;
    btn_pause_ = nullptr;
    btn_tune_ = nullptr;
    btn_cancel_ = nullptr;
    success_badge_ = nullptr;
    cancel_badge_ = nullptr;
    error_badge_ = nullptr;
    overlay_header_ = nullptr;

    // Reset widget-dependent state
    resize_registered_ = false;
    is_active_ = false;
    gcode_loaded_ = false;
    complete_view_mode_ = false;
    // Clear dedup guards so thumbnail + gcode reload isn't blocked on next open.
    // loaded_thumbnail_filename_ gates the outer set_filename() idempotency check;
    // requested_gcode_filename_ gates the inner gcode download dedup;
    // pending_gcode_filename_ would cause a redundant load in on_activate() if stale.
    // All must be cleared or reopen after destroy-on-close silently skips all reloads.
    loaded_thumbnail_filename_.clear();
    requested_gcode_filename_.clear();
    pending_gcode_filename_.clear();
}

lv_obj_t* PrintStatusPanel::get_cached_overlay() {
    return s_cached_panel;
}

bool PrintStatusPanel::push_overlay(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        spdlog::error("[PrintStatusPanel] push_overlay: null parent_screen");
        return false;
    }

    // Lazy-create the widget tree if it was destroyed or never created
    if (!s_cached_panel) {
        auto& panel = get_global_print_status_panel();

        if (!panel.are_subjects_initialized()) {
            panel.init_subjects();
        }

        s_cached_panel = panel.create(parent_screen);
        if (!s_cached_panel) {
            spdlog::error("[PrintStatusPanel] Failed to create print status overlay from XML");
            return false;
        }

        // Register with NavigationManager for lifecycle callbacks (persistent
        // so the registration survives navbar panel switches while cached)
        NavigationManager::instance().register_overlay_instance(s_cached_panel, &panel, true);

        // Decide whether to destroy the widget tree when the overlay closes.
        // On memory-constrained devices or when currently under pressure, destroy
        // on close to free ~400-800KB. On devices with plenty of available RAM,
        // keep the widget tree alive so re-opening is instant — no thumbnail→3D
        // rebuild jump (issue #618).
        auto mem = helix::get_system_memory_info();
        bool should_destroy = mem.is_low_memory();
        if (should_destroy) {
            NavigationManager::instance().register_overlay_close_callback(s_cached_panel, []() {
                auto& p = get_global_print_status_panel();
                p.destroy_overlay_ui(s_cached_panel);
            });
            spdlog::info("[PrintStatusPanel] Print status overlay created (destroy-on-close, "
                         "{}MB available, {}MB total)",
                         mem.available_mb(), mem.total_mb());
        } else {
            spdlog::info("[PrintStatusPanel] Print status overlay created (persistent, "
                         "{}MB available, {}MB total)",
                         mem.available_mb(), mem.total_mb());
        }

        // Register pressure responder once. The persistent branch above keeps the
        // widget tree alive across overlay closes to avoid the thumbnail→3D
        // rebuild jump — but that decision assumed plenty of RAM at startup.
        // If pressure builds up later (slow leak, second connection, heavy file
        // selection), drop the cached tree to reclaim memory even in persistent
        // mode. No-op if the overlay is currently visible.
        if (s_memory_responder_id == 0) {
            s_memory_responder_id = helix::MemoryMonitor::instance().add_pressure_responder(
                [](helix::MemoryPressureLevel level) {
                    // Pre-queue bail-out: if there's no cached tree, a
                    // queue_update hop would just land on an empty null check.
                    // Monitor thread reads s_cached_panel without a barrier;
                    // pointer-sized loads are atomic on our targets, and a
                    // false-negative is harmless (queued lambda re-checks).
                    if (level < helix::MemoryPressureLevel::warning || !s_cached_panel) {
                        return;
                    }
                    try_reclaim_cached_print_status();
                });
        }
    }

    NavigationManager::instance().push_overlay(s_cached_panel);
    return true;
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void PrintStatusPanel::format_time(int seconds, char* buf, size_t buf_size) {
    std::string formatted = helix::format::duration_padded(seconds);
    std::snprintf(buf, buf_size, "%s", formatted.c_str());
}

void PrintStatusPanel::cleanup_temp_gcode() {
    if (!temp_gcode_path_.empty()) {
        if (std::remove(temp_gcode_path_.c_str()) == 0) {
            spdlog::debug("[{}] Cleaned up temp G-code file: {}", get_name(), temp_gcode_path_);
        } else {
            spdlog::trace("[{}] Temp G-code file already removed: {}", get_name(),
                          temp_gcode_path_);
        }
        temp_gcode_path_.clear();
    }
}

void PrintStatusPanel::show_gcode_viewer(bool show) {
    // Update viewer mode subject - XML bindings handle visibility reactively
    // Mode 0 = thumbnail (gradient + thumbnail visible, gcode viewer hidden)
    // Mode 1 = 3D gcode viewer (gcode visible, gradient + thumbnail hidden, rotate icon shown)
    // Mode 2 = 2D gcode viewer (gcode visible, gradient shown, thumbnail + rotate icon hidden)
    int mode = 0; // Default: thumbnail
    if (show) {
        // Check if the viewer is using 2D mode
        bool is_2d = gcode_viewer_ && ui_gcode_viewer_is_using_2d_mode(gcode_viewer_);
        mode = is_2d ? 2 : 1;
    }
    lv_subject_set_int(&gcode_viewer_mode_subject_, mode);

    // When falling back to thumbnail mode, ensure the image source is applied.
    // During async gcode reload the gradient covers the area — the user should
    // at least see the cached thumbnail underneath.
    if (mode == 0 && print_thumbnail_ && !cached_thumbnail_path_.empty()) {
        const void* current_src = lv_image_get_src(print_thumbnail_);
        if (!current_src) {
            lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
        }
    }

    // Pause/resume rendering based on visibility mode (CPU optimization)
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, !show);
    }

    spdlog::trace("[{}] G-code viewer mode: {} ({})", get_name(), mode,
                  mode == 0 ? "thumbnail" : (mode == 1 ? "3D" : "2D"));

    // Diagnostic: log visibility state of all viewer components
    if (print_thumbnail_) {
        bool thumb_hidden = lv_obj_has_flag(print_thumbnail_, LV_OBJ_FLAG_HIDDEN);
        const void* img_src = lv_image_get_src(print_thumbnail_);
        spdlog::trace("[{}]   -> thumbnail: hidden={}, has_src={}", get_name(), thumb_hidden,
                      img_src != nullptr);
    }
    if (gcode_viewer_) {
        bool viewer_hidden = lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[{}]   -> gcode_viewer: hidden={}", get_name(), viewer_hidden);
    }
    if (gradient_background_) {
        bool grad_hidden = lv_obj_has_flag(gradient_background_, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[{}]   -> gradient: hidden={}", get_name(), grad_hidden);
    }
}

void PrintStatusPanel::show_exclude_map_view() {
    if (!exclude_manager_)
        return;

    // Get bed dimensions from MoonrakerAPI hardware info
    float bed_w = 235.0f, bed_h = 235.0f; // sensible defaults
    if (api_) {
        const auto& vol = api_->hardware().build_volume();
        float w = vol.x_max - vol.x_min;
        float h = vol.y_max - vol.y_min;
        if (w > 0.0f && h > 0.0f) {
            bed_w = w;
            bed_h = h;
        }
    }

    // Find thumbnail section container
    lv_obj_t* thumbnail_section = lv_obj_find_by_name(overlay_root_, "thumbnail_section");
    if (!thumbnail_section) {
        spdlog::warn("[{}] Cannot show map view: thumbnail_section not found", get_name());
        return;
    }

    // XML bindings on print_thumbnail and gradient_background hide them whenever
    // exclude_map_active == 1 — setting this before creating the map avoids a
    // brief frame with the overlay atop still-visible thumbnail/gradient.
    lv_subject_set_int(&exclude_map_active_subject_, 1);

    map_view_ = std::make_unique<helix::ui::ExcludeObjectMapView>();
    map_view_->set_close_callback([this]() { hide_exclude_map_view(); });

    // Try to get parsed GCode data from the viewer (may be nullptr in thumbnail-only mode)
    std::shared_ptr<helix::gcode::ParsedGCodeFile> parsed;
    if (gcode_viewer_) {
        const auto* raw = ui_gcode_viewer_get_parsed_file(gcode_viewer_);
        if (raw) {
            // Wrap in a no-op shared_ptr (we don't own it, viewer manages lifetime)
            parsed = std::shared_ptr<helix::gcode::ParsedGCodeFile>(
                const_cast<helix::gcode::ParsedGCodeFile*>(raw),
                [](helix::gcode::ParsedGCodeFile*) {});
        }
    }

    map_view_->create(thumbnail_section, printer_state_.get_excluded_objects_state(), bed_w, bed_h,
                      exclude_manager_.get(), parsed);

    spdlog::debug("[{}] Showed exclude object map view (bed: {}x{}mm)", get_name(), bed_w, bed_h);
}

void PrintStatusPanel::hide_exclude_map_view() {
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }
    // Un-hides thumbnail/gradient via the XML bindings on exclude_map_active.
    lv_subject_set_int(&exclude_map_active_subject_, 0);
}

void PrintStatusPanel::load_gcode_file(const char* file_path) {
    if (!gcode_viewer_ || !file_path) {
        spdlog::warn("[{}] Cannot load G-code: viewer={}, path={}", get_name(),
                     gcode_viewer_ != nullptr, file_path != nullptr);
        return;
    }

    spdlog::debug("[{}] Loading G-code file: {}", get_name(), file_path);

    // Register callback to be notified when loading completes
    ui_gcode_viewer_set_load_callback(
        gcode_viewer_,
        [](lv_obj_t* viewer, void* user_data, bool success) {
            auto* self = static_cast<PrintStatusPanel*>(user_data);
            if (!success) {
                spdlog::error("[{}] G-code load failed", self->get_name());
                self->gcode_loaded_ = false;
                self->lifecycle_.set_gcode_loaded(false);
                return;
            }

            // Get layer count from loaded geometry
            int max_layer = ui_gcode_viewer_get_max_layer(viewer);
            if (max_layer >= 0)
                spdlog::debug("[{}] G-code loaded: {} layers", self->get_name(), max_layer);
            else
                spdlog::debug("[{}] G-code loaded (renderer pending)", self->get_name());

            // Mark G-code as successfully loaded (enables viewer mode on state changes)
            self->gcode_loaded_ = true;
            self->lifecycle_.set_gcode_loaded(true);

            // Override extrusion colors with AMS filament colors.
            // For multi-tool prints, applies per-tool AMS slot colors.
            // For single-tool, falls back to current AMS color subject.
            self->build_and_apply_tool_colors();

            // Show viewer if print is active or in terminal state (user can see
            // where print stopped). Only skip in Idle.
            if (self->lifecycle_.want_viewer()) {
                self->show_gcode_viewer(true);
            }

            // Force layout recalculation now that viewer is visible
            lv_obj_update_layout(viewer);
            // Reset camera to fit model to new viewport dimensions
            ui_gcode_viewer_reset_camera(viewer);

            // Set print progress to current layer (not 0!) when joining a print in progress.
            // Read directly from PrinterState subjects to get the latest values.
            int viewer_max_layer = ui_gcode_viewer_get_max_layer(viewer);
            int current_layer =
                lv_subject_get_int(self->printer_state_.get_print_layer_current_subject());
            int total_layers =
                lv_subject_get_int(self->printer_state_.get_print_layer_total_subject());

            // Fallback: if Moonraker metadata didn't provide layer count,
            // use the count from the parsed/indexed gcode file
            if (total_layers == 0 && viewer_max_layer > 0) {
                int layer_count = viewer_max_layer + 1; // max_layer is 0-based
                self->printer_state_.set_print_layer_total(layer_count);
                spdlog::info("[{}] Set total layers from gcode viewer: {}", self->get_name(),
                             layer_count);
            }

            // Update lifecycle state while we're at it
            self->lifecycle_.on_layer_changed(current_layer, total_layers,
                                              self->printer_state_.has_real_layer_data());

            // Map from Moonraker layer count to viewer layer count
            // Note: viewer_max_layer may be -1 if 2D renderer not yet initialized (lazy init)
            int viewer_layer = 0;
            if (viewer_max_layer > 0 && total_layers > 0) {
                viewer_layer = (current_layer * viewer_max_layer) / total_layers;
            } else if (viewer_max_layer <= 0 && current_layer > 0) {
                // 2D renderer not ready yet - use raw current layer, will be corrected later
                // The 2D renderer will use this value when it initializes on first render
                viewer_layer = current_layer;
            }

            // CRITICAL: Defer to avoid lv_obj_invalidate() during render phase
            // This callback runs during lv_timer_handler() which may be mid-render
            struct ViewerProgressCtx {
                lv_obj_t* viewer;
                int layer;
            };
            auto ctx = std::make_unique<ViewerProgressCtx>(ViewerProgressCtx{viewer, viewer_layer});
            helix::ui::queue_update<ViewerProgressCtx>(std::move(ctx), [](ViewerProgressCtx* c) {
                if (c->viewer && lv_obj_is_valid(c->viewer)) {
                    ui_gcode_viewer_set_print_progress(c->viewer, c->layer);
                }
            });

            spdlog::debug("[{}] G-code loaded: initial layer progress set to {} "
                          "(current={}/{}, viewer_max={})",
                          self->get_name(), viewer_layer, current_layer, total_layers,
                          viewer_max_layer);

            // NOTE: PrintStatusPanel does NOT start prints - it only VIEWS them.
            // Prints are started from PrintSelectPanel via the Print button.
            // This callback is for loading G-code into the viewer for visualization only.
            spdlog::debug("[{}] G-code loaded for viewing: {}", self->get_name(),
                          ui_gcode_viewer_get_filename(viewer));
        },
        this);

    // Start loading the file
    ui_gcode_viewer_load_file(gcode_viewer_, file_path);
}

void PrintStatusPanel::update_all_displays() {
    // Guard: don't update if subjects aren't initialized yet
    if (!subjects_initialized_) {
        return;
    }

    // Progress text
    helix::format::format_percent(lifecycle_.progress(), progress_text_buf_,
                                  sizeof(progress_text_buf_));
    lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);

    // Layer text (prefix with ~ when estimated from progress)
    const char* layer_fmt =
        printer_state_.has_real_layer_data() ? "Layer %d / %d" : "Layer ~%d / %d";
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), layer_fmt, lifecycle_.current_layer(),
                  lifecycle_.total_layers());
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);

    // Filament used text
    int filament_mm = lv_subject_get_int(get_printer_state().get_print_filament_used_subject());
    if (filament_mm > 0) {
        std::string fil_str =
            helix::format::format_filament_length(static_cast<double>(filament_mm)) + " " +
            lv_tr("used");
        std::strncpy(filament_used_text_buf_, fil_str.c_str(), sizeof(filament_used_text_buf_) - 1);
        filament_used_text_buf_[sizeof(filament_used_text_buf_) - 1] = '\0';
    } else {
        filament_used_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&filament_used_text_subject_, filament_used_text_buf_);

    // Time displays - Preparing: preprint observers own these.
    // Complete: on_print_state_changed sets frozen final values, don't overwrite.
    if (lifecycle_.state() != PrintState::Preparing && lifecycle_.state() != PrintState::Complete) {
        // elapsed_seconds is wall-clock time from Moonraker total_duration (includes prep)
        format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);

        format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);
    }

    // Use centralized temperature formatting with em dash for heater-off state
    format_temperature_pair(centi_to_degrees(lifecycle_.nozzle_current()),
                            centi_to_degrees(lifecycle_.nozzle_target()), nozzle_temp_buf_,
                            sizeof(nozzle_temp_buf_));
    lv_subject_copy_string(&nozzle_temp_subject_, nozzle_temp_buf_);

    format_temperature_pair(centi_to_degrees(lifecycle_.bed_current()),
                            centi_to_degrees(lifecycle_.bed_target()), bed_temp_buf_,
                            sizeof(bed_temp_buf_));
    lv_subject_copy_string(&bed_temp_subject_, bed_temp_buf_);

    // Heater status text (Off / Heating... / Ready)
    auto nozzle_heater = helix::ui::temperature::heater_display(lifecycle_.nozzle_current(),
                                                                lifecycle_.nozzle_target());
    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s",
                  nozzle_heater.status.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);

    auto bed_heater =
        helix::ui::temperature::heater_display(lifecycle_.bed_current(), lifecycle_.bed_target());
    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", bed_heater.status.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);

    // Speeds
    helix::format::format_percent(lifecycle_.speed_percent(), speed_buf_, sizeof(speed_buf_));
    lv_subject_copy_string(&speed_subject_, speed_buf_);

    helix::format::format_percent(lifecycle_.flow_percent(), flow_buf_, sizeof(flow_buf_));
    lv_subject_copy_string(&flow_subject_, flow_buf_);

    // Update pause button icon and label based on state
    // MDI icons: play=F040A, pause=F03E4 (UTF-8: play=F3 B0 90 8A, pause=F3 B0 8F A4)
    if (lifecycle_.state() == PrintState::Paused) {
        std::snprintf(pause_button_buf_, sizeof(pause_button_buf_), "\xF3\xB0\x90\x8A"); // play
        std::snprintf(pause_label_buf_, sizeof(pause_label_buf_), "Resume");
    } else {
        std::snprintf(pause_button_buf_, sizeof(pause_button_buf_), "\xF3\xB0\x8F\xA4"); // pause
        std::snprintf(pause_label_buf_, sizeof(pause_label_buf_), "Pause");
    }
    lv_subject_copy_string(&pause_button_subject_, pause_button_buf_);
    lv_subject_copy_string(&pause_label_subject_, pause_label_buf_);
}

// ============================================================================
// INSTANCE HANDLERS
// ============================================================================

void PrintStatusPanel::handle_temp_card_click() {
    spdlog::debug("[{}] Temp card clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::GraphOnly, parent_screen_);
}

void PrintStatusPanel::handle_pause_button() {
    if (lifecycle_.state() == PrintState::Printing) {
        spdlog::info("[{}] Pausing print...", get_name());

        // Check if pause slot is available
        const auto& pause_info = StandardMacros::instance().get(StandardMacroSlot::Pause);
        if (pause_info.is_empty()) {
            spdlog::warn("[{}] Pause macro slot is empty", get_name());
            NOTIFY_WARNING(lv_tr("Pause macro not configured"));
            return;
        }

        if (api_) {
            spdlog::info("[{}] Using StandardMacros pause: {}", get_name(), pause_info.get_macro());
            // Stateless callbacks to avoid use-after-free if panel destroyed [L012]
            StandardMacros::instance().execute(
                StandardMacroSlot::Pause, api_,
                []() {
                    spdlog::info("[Print Status] Pause command sent successfully");
                    // State will update via PrinterState observer when Moonraker confirms
                },
                [](const MoonrakerError& err) {
                    spdlog::error("[Print Status] Failed to pause print: {}", err.message);
                    NOTIFY_ERROR(lv_tr("Failed to pause print: {}"), err.user_message());
                });
        } else {
            // Fall back to local state change for mock mode
            spdlog::warn("[{}] API not available - using local state change", get_name());
            set_state(PrintState::Paused);
        }
    } else if (lifecycle_.state() == PrintState::Paused) {
        spdlog::info("[{}] Resuming print...", get_name());

        // Check if resume slot is available
        const auto& resume_info = StandardMacros::instance().get(StandardMacroSlot::Resume);
        if (resume_info.is_empty()) {
            spdlog::warn("[{}] Resume macro slot is empty", get_name());
            NOTIFY_WARNING(lv_tr("Resume macro not configured"));
            return;
        }

        if (api_) {
            spdlog::info("[{}] Using StandardMacros resume: {}", get_name(),
                         resume_info.get_macro());
            // Stateless callbacks to avoid use-after-free if panel destroyed [L012]
            StandardMacros::instance().execute(
                StandardMacroSlot::Resume, api_,
                []() {
                    spdlog::info("[Print Status] Resume command sent successfully");
                    // State will update via PrinterState observer when Moonraker confirms
                },
                [](const MoonrakerError& err) {
                    spdlog::error("[Print Status] Failed to resume print: {}", err.message);
                    NOTIFY_ERROR(lv_tr("Failed to resume print: {}"), err.user_message());
                });
        } else {
            // Fall back to local state change for mock mode
            spdlog::warn("[{}] API not available - using local state change", get_name());
            set_state(PrintState::Printing);
        }
    }
}

void PrintStatusPanel::handle_tune_button() {
    spdlog::info("[{}] Tune button clicked - opening tuning panel", get_name());

    // Use singleton - handles lazy init, subject registration, slider sync, and nav push
    get_print_tune_overlay().show(parent_screen_, api_, printer_state_);
}

void PrintStatusPanel::handle_cancel_button() {
    spdlog::info("[{}] Cancel button clicked - showing confirmation dialog", get_name());

    // Check if AbortManager is idle (not already aborting)
    if (helix::AbortManager::instance().is_aborting()) {
        spdlog::warn("[{}] Abort already in progress", get_name());
        NOTIFY_WARNING(lv_tr("Abort already in progress"));
        return;
    }

    // Set up the confirm callback to start the abort process
    cancel_modal_.set_on_confirm([]() {
        spdlog::info("[PrintStatusPanel] Cancel confirmed - starting AbortManager");

        // AbortManager handles its own UI state (progress modal, button states)
        helix::AbortManager::instance().start_abort();
    });

    // Show the modal (RAII handles cleanup)
    cancel_modal_.show(lv_screen_active());
}

void PrintStatusPanel::handle_reprint_button() {
    // Startup grace period: reject phantom clicks during early boot
    auto elapsed = std::chrono::steady_clock::now() - AppConstants::Startup::PROCESS_START_TIME;
    if (elapsed < AppConstants::Startup::PRINT_START_GRACE_PERIOD) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::warn("[{}] Rejected reprint during startup grace period ({}s < {}s)", get_name(),
                     secs, AppConstants::Startup::PRINT_START_GRACE_PERIOD.count());
        return;
    }

    spdlog::info("[{}] Reprint button clicked - reprinting: {}", get_name(),
                 current_print_filename_);

    if (current_print_filename_.empty()) {
        spdlog::warn("[{}] No filename to reprint", get_name());
        NOTIFY_WARNING(lv_tr("No file to reprint"));
        return;
    }

    if (!api_) {
        spdlog::error("[{}] Cannot reprint: API not available", get_name());
        NOTIFY_ERROR(lv_tr("Cannot reprint: not connected to printer"));
        return;
    }

    // Disable button immediately to prevent double-press
    ui_set_button_enabled(btn_cancel_, false);

    std::string filename = current_print_filename_;

    api_->job().start_print(
        filename,
        [this, filename]() {
            spdlog::info("[{}] Reprint started: {}", get_name(), filename);
            // State will update via PrinterState observer when Moonraker confirms
            // Button will transform back to Cancel mode when state changes to Printing
        },
        [this, token = lifetime_.token()](const MoonrakerError& err) {
            // Runs on libhv WS event loop — marshal LVGL work to main.
            token.defer("PrintStatusPanel::reprint_err", [this, err]() {
                spdlog::error("[{}] Failed to reprint: {}", get_name(), err.message);
                NOTIFY_ERROR(lv_tr("Failed to reprint: {}"), err.user_message());
                ui_set_button_enabled(btn_cancel_, true);
            });
        });
}

void PrintStatusPanel::handle_resize() {
    spdlog::debug("[{}] Handling resize event", get_name());

    // Reset gcode viewer camera to fit new dimensions
    if (gcode_viewer_ && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN)) {
        // Force layout recalculation so viewer gets correct dimensions
        lv_obj_update_layout(gcode_viewer_);
        ui_gcode_viewer_reset_camera(gcode_viewer_);
        spdlog::debug("[{}] Reset gcode viewer camera after resize", get_name());
    }
}

// ============================================================================
// STATIC TRAMPOLINES
// ============================================================================

void PrintStatusPanel::on_temp_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_temp_card_clicked");
    (void)e;
    get_global_print_status_panel().handle_temp_card_click();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_dismiss_overlay_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_dismiss_overlay_clicked");
    (void)e;
    // XML binding on each overlay hides when end_overlay_dismissed == 1.
    lv_subject_set_int(&get_global_print_status_panel().end_overlay_dismissed_subject_, 1);
    spdlog::debug("[PrintStatusPanel] Dismissed print end overlay");
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_pause_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_pause_clicked");
    (void)e;
    get_global_print_status_panel().handle_pause_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_tune_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_tune_clicked");
    (void)e;
    get_global_print_status_panel().handle_tune_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_cancel_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_cancel_clicked");
    (void)e;
    get_global_print_status_panel().handle_cancel_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_reprint_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_reprint_clicked");
    (void)e;
    get_global_print_status_panel().handle_reprint_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_objects_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_objects_clicked");
    (void)e;
    auto& panel = get_global_print_status_panel();

    int mode = lv_subject_get_int(&panel.gcode_viewer_mode_subject_);

    if (mode == 0) {
        // Thumbnail-only mode: toggle the overhead map view
        if (panel.map_view_ && panel.map_view_->is_active()) {
            panel.hide_exclude_map_view();
        } else {
            panel.show_exclude_map_view();
        }
    } else {
        // 3D/2D viewer mode: show the list overlay
        if (panel.exclude_manager_ && panel.parent_screen_) {
            helix::ui::get_exclude_objects_list_overlay().show(
                panel.parent_screen_, panel.api_, panel.printer_state_,
                panel.exclude_manager_.get(), panel.gcode_viewer_);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_view_toggle_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_view_toggle_clicked");
    (void)e;
    auto& panel = get_global_print_status_panel();

    panel.complete_view_mode_ = !panel.complete_view_mode_;

    if (panel.complete_view_mode_) {
        // Complete view: show all layers solid (no ghost)
        if (panel.gcode_viewer_) {
            ui_gcode_viewer_set_print_progress(panel.gcode_viewer_, -1);
        }
    } else {
        // Progress view: restore current layer with ghost
        if (panel.gcode_viewer_) {
            int current_layer =
                lv_subject_get_int(panel.printer_state_.get_print_layer_current_subject());
            int total_layers =
                lv_subject_get_int(panel.printer_state_.get_print_layer_total_subject());
            int viewer_max_layer = ui_gcode_viewer_get_max_layer(panel.gcode_viewer_);
            int viewer_layer = current_layer;
            if (total_layers > 0 && viewer_max_layer > 0) {
                viewer_layer = (current_layer * viewer_max_layer) / total_layers;
            }
            ui_gcode_viewer_set_print_progress(panel.gcode_viewer_, viewer_layer);
        }
    }

    const char* icon_text =
        lv_xml_get_const(nullptr, panel.complete_view_mode_ ? "icon_layers" : "icon_cube");
    if (icon_text) {
        lv_subject_copy_string(&panel.view_toggle_icon_subject_, icon_text);
    }

    spdlog::debug("[PrintStatusPanel] View toggle: {}",
                  panel.complete_view_mode_ ? "complete" : "progress");
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_resize_static() {
    // Use global instance for resize callback (registered without user_data)
    if (g_print_status_panel) {
        g_print_status_panel->handle_resize();
    }
}

// ============================================================================
// OBSERVER INSTANCE METHODS
// ============================================================================

void PrintStatusPanel::on_temperature_changed() {
    // Read all temperature values from PrinterState subjects and delegate to lifecycle
    int nz_cur = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int nz_tgt = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
    int bed_cur = lv_subject_get_int(printer_state_.get_bed_temp_subject());
    int bed_tgt = lv_subject_get_int(printer_state_.get_bed_target_subject());
    lifecycle_.on_temperature_changed(nz_cur, nz_tgt, bed_cur, bed_tgt);

    if (!subjects_initialized_)
        return;

    // Update only temperature-related subjects (not the full display refresh).
    // Temperature observers fire frequently during heating (4 subjects x ~1Hz each),
    // and update_all_displays() re-renders ALL subjects causing visible flickering.
    auto& ts = helix::ToolState::instance();
    if (ts.is_multi_tool() && ts.active_tool()) {
        size_t prefix_len = std::snprintf(nozzle_temp_buf_, sizeof(nozzle_temp_buf_),
                                          "%s: ", ts.active_tool()->name.c_str());
        format_temperature_pair(centi_to_degrees(lifecycle_.nozzle_current()),
                                centi_to_degrees(lifecycle_.nozzle_target()),
                                nozzle_temp_buf_ + prefix_len,
                                sizeof(nozzle_temp_buf_) - prefix_len);
    } else {
        format_temperature_pair(centi_to_degrees(lifecycle_.nozzle_current()),
                                centi_to_degrees(lifecycle_.nozzle_target()), nozzle_temp_buf_,
                                sizeof(nozzle_temp_buf_));
    }
    lv_subject_copy_string(&nozzle_temp_subject_, nozzle_temp_buf_);

    format_temperature_pair(centi_to_degrees(lifecycle_.bed_current()),
                            centi_to_degrees(lifecycle_.bed_target()), bed_temp_buf_,
                            sizeof(bed_temp_buf_));
    lv_subject_copy_string(&bed_temp_subject_, bed_temp_buf_);

    auto nozzle_heater = helix::ui::temperature::heater_display(lifecycle_.nozzle_current(),
                                                                lifecycle_.nozzle_target());
    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s",
                  nozzle_heater.status.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);

    auto bed_heater =
        helix::ui::temperature::heater_display(lifecycle_.bed_current(), lifecycle_.bed_target());
    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", bed_heater.status.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);

    spdlog::trace("[{}] Temperatures updated: nozzle {}/{}°C, bed {}/{}°C", get_name(),
                  lifecycle_.nozzle_current(), lifecycle_.nozzle_target(), lifecycle_.bed_current(),
                  lifecycle_.bed_target());
}

void PrintStatusPanel::recompute_end_overlay_visibility() {
    if (!subjects_initialized_)
        return;
    int outcome = lv_subject_get_int(printer_state_.get_print_outcome_subject());
    bool dismissed = lv_subject_get_int(&end_overlay_dismissed_subject_) != 0;
    int complete = (!dismissed && outcome == static_cast<int>(PrintOutcome::COMPLETE)) ? 1 : 0;
    int cancelled = (!dismissed && outcome == static_cast<int>(PrintOutcome::CANCELLED)) ? 1 : 0;
    int error = (!dismissed && outcome == static_cast<int>(PrintOutcome::ERROR)) ? 1 : 0;
    lv_subject_set_int(&show_complete_overlay_subject_, complete);
    lv_subject_set_int(&show_cancelled_overlay_subject_, cancelled);
    lv_subject_set_int(&show_error_overlay_subject_, error);
}

void PrintStatusPanel::recompute_paused_overlay_visibility() {
    if (!subjects_initialized_)
        return;

    auto state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
    bool paused = (state == PrintJobState::PAUSED);
    lv_subject_set_int(&show_paused_overlay_subject_, paused ? 1 : 0);

    // Reason resolution: prefer Klipper's print_stats.message (firmware-supplied
    // descriptor — runout, error wrap, custom macros). If empty AND any
    // configured runout sensor is currently tripped, surface a generic
    // "Filament Runout" hint. Otherwise leave blank → reason label stays hidden.
    std::string reason;
    if (paused) {
        const char* fw_msg = lv_subject_get_string(printer_state_.get_print_message_subject());
        if (fw_msg && *fw_msg) {
            reason = fw_msg;
        } else if (FilamentSensorManager::instance().has_any_runout()) {
            reason = lv_tr("Filament Runout");
        }
    }
    lv_subject_copy_string(&print_pause_reason_subject_, reason.c_str());
    lv_subject_set_int(&print_pause_reason_visible_subject_, reason.empty() ? 0 : 1);
}

void PrintStatusPanel::update_chamber_status() {
    if (!subjects_initialized_)
        return;

    bool has_heater =
        lv_subject_get_int(printer_state_.get_printer_has_chamber_heater_subject()) != 0;
    int current = lv_subject_get_int(printer_state_.get_chamber_temp_subject());
    int target = lv_subject_get_int(printer_state_.get_chamber_target_subject());

    if (!has_heater || target == 0) {
        // Sensor-only or heater off: no status text
        chamber_status_buf_[0] = '\0';
    } else {
        auto chamber_heater = helix::ui::temperature::heater_display(current, target);
        std::snprintf(chamber_status_buf_, sizeof(chamber_status_buf_), "%s",
                      chamber_heater.status.c_str());
    }
    lv_subject_copy_string(&chamber_status_subject_, chamber_status_buf_);
}

void PrintStatusPanel::on_print_progress_changed(int progress) {
    // Delegate state guard and clamping to lifecycle
    if (!lifecycle_.on_progress_changed(progress)) {
        spdlog::trace("[{}] Ignoring progress update ({}) - guarded by lifecycle", get_name(),
                      progress);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Update progress text
    helix::format::format_percent(lifecycle_.progress(), progress_text_buf_,
                                  sizeof(progress_text_buf_));
    lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);

    // Update progress bar with smooth animation (300ms ease-out) if animations enabled
    // This complements the subject binding with animated transitions
    if (progress_bar_) {
        lv_anim_enable_t anim_enable =
            DisplaySettingsManager::instance().get_animations_enabled() ? LV_ANIM_ON : LV_ANIM_OFF;
        lv_bar_set_value(progress_bar_, lifecycle_.progress(), anim_enable);
    }

    // Update filament used text (evolves during active printing)
    int filament_mm = lv_subject_get_int(get_printer_state().get_print_filament_used_subject());
    if (filament_mm > 0) {
        std::string fil_str =
            helix::format::format_filament_length(static_cast<double>(filament_mm)) + " " +
            lv_tr("used");
        std::strncpy(filament_used_text_buf_, fil_str.c_str(), sizeof(filament_used_text_buf_) - 1);
        filament_used_text_buf_[sizeof(filament_used_text_buf_) - 1] = '\0';
    } else {
        filament_used_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&filament_used_text_subject_, filament_used_text_buf_);

    spdlog::trace("[{}] Progress updated: {}%", get_name(), lifecycle_.progress());
}

void PrintStatusPanel::on_print_state_changed(PrintJobState job_state) {
    spdlog::debug("[{}] on_print_state_changed() job_state={} current_state={}", get_name(),
                  static_cast<int>(job_state), static_cast<int>(lifecycle_.state()));

    // Get outcome from PrinterState for lifecycle decision-making
    auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(printer_state_.get_print_outcome_subject()));

    // Delegate state mapping and transition logic to lifecycle
    auto result = lifecycle_.on_job_state_changed(job_state, outcome);
    if (!result.state_changed) {
        return;
    }

    // Note: Badge/Reprint button visibility is now handled via the print_outcome subject,
    // which persists the terminal state (Complete/Cancelled/Error) until a new print starts.
    // The print_state_enum subject now always reflects the true Moonraker state.

    // Terminal→Idle: Moonraker sends STANDBY after Complete/Cancelled/Error.
    // Clean up tracking data but keep the display frozen — the user should see
    // the final print state until a new print starts.
    bool from_terminal_to_idle = result.print_ended && (result.old_state == PrintState::Complete ||
                                                        result.old_state == PrintState::Cancelled ||
                                                        result.old_state == PrintState::Error);

    // Clear thumbnail and G-code tracking when print ends
    if (result.print_ended) {
        if (!thumbnail_source_filename_.empty() || !loaded_thumbnail_filename_.empty() ||
            gcode_loaded_ || !temp_gcode_path_.empty() || !pending_gcode_filename_.empty()) {
            spdlog::debug("[{}] Clearing thumbnail/gcode tracking (print ended)", get_name());
            // Cancel pending deferred G-code load (print is over)
            if (gcode_load_timer_) {
                lv_timer_delete(gcode_load_timer_);
                gcode_load_timer_ = nullptr;
            }
            thumbnail_source_filename_.clear();
            loaded_thumbnail_filename_.clear();
            cached_thumbnail_path_.clear();
            pending_gcode_filename_.clear();
            requested_gcode_filename_.clear();
            // Panel's local gcode_loaded_ mirrors lifecycle's decision
            if (result.clear_gcode_loaded) {
                gcode_loaded_ = false;
            }
            cleanup_temp_gcode();

            // Note: Shared subjects (print_thumbnail_path, print_display_filename)
            // are cleared by ActivePrintMediaManager when print_filename_ becomes empty
        }
    }

    if (from_terminal_to_idle) {
        // Terminal→Idle: Moonraker sends zeroed subjects (progress=0, layer=0) in the
        // same batch as STANDBY. The XML subject bindings update widgets directly,
        // bypassing lifecycle guards. Re-freeze the display values to counteract this.
        if (subjects_initialized_) {
            // Re-freeze progress bar (XML bind_value="print_progress" set it to 0)
            if (progress_bar_) {
                lv_bar_set_value(progress_bar_, lifecycle_.progress(), LV_ANIM_OFF);
            }

            // Re-freeze gcode viewer layer. The viewer has its own observer on
            // print_layer_current_subject that already zeroed the display before
            // the lifecycle guard kicked in; push the frozen mapped layer back.
            // Deferred via queue_update for the same reason as the live update
            // path below — observer callbacks can fire mid-render.
            if (gcode_viewer_) {
                int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
                int viewer_layer = lifecycle_.map_current_layer_to_viewer(viewer_max_layer);
                struct ViewerProgressCtx {
                    lv_obj_t* viewer;
                    int layer;
                };
                auto ctx = std::make_unique<ViewerProgressCtx>(
                    ViewerProgressCtx{gcode_viewer_, viewer_layer});
                helix::ui::queue_update<ViewerProgressCtx>(
                    std::move(ctx), [](ViewerProgressCtx* c) {
                        if (c->viewer && lv_obj_is_valid(c->viewer)) {
                            ui_gcode_viewer_set_print_progress(c->viewer, c->layer);
                        }
                    });
            }
        }
        // Don't call update_all_displays() or show_gcode_viewer() — keep display frozen
        spdlog::debug("[{}] Print state changed: {} -> {} (display frozen)", get_name(),
                      print_job_state_to_string(job_state), static_cast<int>(result.new_state));
    } else {
        update_all_displays();
        update_button_states();
        show_gcode_viewer(result.should_show_viewer);
        spdlog::debug("[{}] Print state changed: {} -> {}", get_name(),
                      print_job_state_to_string(job_state), static_cast<int>(result.new_state));
    }

    // Delegate runout guidance handling to the handler
    if (runout_handler_) {
        runout_handler_->on_print_state_changed(result.old_state, result.new_state);
    }

    // Update the "Print Paused" overlay any time the job state moves —
    // covers PRINTING→PAUSED, PAUSED→PRINTING, PAUSED→CANCELLED, mid-print attach.
    recompute_paused_overlay_visibility();

    if (result.should_reset_progress_bar) {
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        }
        complete_view_mode_ = false;
        // Reset toggle icon to default (progress view)
        if (const char* icon = lv_xml_get_const(nullptr, "icon_cube")) {
            lv_subject_copy_string(&view_toggle_icon_subject_, icon);
        }
        // Clear any prior end-overlay dismissal so the next outcome surfaces.
        lv_subject_set_int(&end_overlay_dismissed_subject_, 0);
        spdlog::debug("[{}] Reset progress bar and view toggle for new print", get_name());
    }

    if (result.should_clear_excluded_objects && exclude_manager_) {
        exclude_manager_->clear_excluded_objects();
        spdlog::debug("[{}] Cleared excluded objects for new print", get_name());
    }

    // Transition remaining display from preprint observer back to Moonraker's time_left
    if (result.new_state == PrintState::Printing) {
        format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);
    }

    // Freeze display values on Complete (lifecycle already froze the state values)
    if (result.should_freeze_complete) {
        std::snprintf(progress_text_buf_, sizeof(progress_text_buf_), "100%%");
        lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 100, LV_ANIM_OFF);
        }

        if (lifecycle_.total_layers() > 0) {
            std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), "Layer %d / %d",
                          lifecycle_.current_layer(), lifecycle_.total_layers());
            lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
        }

        format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
        format_time(0, remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);

        animate_print_complete();

        spdlog::info("[{}] Print complete! Final progress: {}%, layer: {}/{}, elapsed: {}s",
                     get_name(), lifecycle_.progress(), lifecycle_.current_layer(),
                     lifecycle_.total_layers(), lifecycle_.elapsed_seconds());
    }

    if (result.should_animate_error) {
        animate_print_error();
        spdlog::info("[{}] Print failed at progress: {}%", get_name(), lifecycle_.progress());
    }

    if (result.should_animate_cancelled) {
        animate_print_cancelled();
        spdlog::debug("[{}] Print cancelled at progress: {}%", get_name(), lifecycle_.progress());
    }

    // Update e-stop button visibility: show only during active print
    if (overlay_header_) {
        bool show_estop =
            (result.new_state == PrintState::Preparing ||
             result.new_state == PrintState::Printing || result.new_state == PrintState::Paused);
        if (show_estop) {
            ui_header_bar_show_action_button(overlay_header_);
        } else {
            ui_header_bar_hide_action_button(overlay_header_);
        }
        spdlog::debug("[{}] E-stop button {} (state={})", get_name(),
                      show_estop ? "shown" : "hidden", static_cast<int>(result.new_state));
    }
}

void PrintStatusPanel::on_print_filename_changed(const char* filename) {
    // Check if this is a non-empty filename (new print starting)
    bool has_filename = filename && filename[0] != '\0';

    // Guard: preserve final values when in Complete state and filename is empty
    // Moonraker sends empty filename when transitioning to Standby, but we want
    // to keep showing the completed print's filename. However, if a NEW print
    // starts (non-empty filename), we should accept it even if current_state_
    // hasn't been updated yet (race condition between state and filename observers)
    if (lifecycle_.state() == PrintState::Complete && !has_filename) {
        spdlog::trace("[{}] Ignoring empty filename update in Complete state", get_name());
        return;
    }

    if (has_filename) {
        std::string raw_filename = filename;

        // Auto-resolve temp file patterns to original filename.
        // This handles the race condition where Moonraker reports the temp path
        // (e.g., .helix_temp/modified_*) before set_thumbnail_source() is called.
        // Common when Helix plugin is not installed or during direct Moonraker prints.
        std::string resolved = resolve_gcode_filename(raw_filename);
        if (resolved != raw_filename && thumbnail_source_filename_.empty()) {
            spdlog::debug("[{}] Auto-resolved temp filename: {} -> {}", get_name(), raw_filename,
                          resolved);
            set_thumbnail_source(resolved);
        }

        // Call set_filename() which is idempotent (won't reload if effective filename unchanged)
        // Only log when filename actually changes to avoid log spam
        if (raw_filename != current_print_filename_) {
            spdlog::debug("[{}] Filename changed: {}", get_name(), raw_filename);
        }
        set_filename(filename);
    }
}

void PrintStatusPanel::on_speed_factor_changed(int speed) {
    lifecycle_.on_speed_changed(speed);
    if (subjects_initialized_) {
        helix::format::format_percent(lifecycle_.speed_percent(), speed_buf_, sizeof(speed_buf_));
        lv_subject_copy_string(&speed_subject_, speed_buf_);
    }
    spdlog::trace("[{}] Speed factor updated: {}%", get_name(), speed);
}

void PrintStatusPanel::on_flow_factor_changed(int flow) {
    lifecycle_.on_flow_changed(flow);
    if (subjects_initialized_) {
        helix::format::format_percent(lifecycle_.flow_percent(), flow_buf_, sizeof(flow_buf_));
        lv_subject_copy_string(&flow_subject_, flow_buf_);
    }
    spdlog::trace("[{}] Flow factor updated: {}%", get_name(), flow);
}

void PrintStatusPanel::on_gcode_z_offset_changed(int microns) {
    // Delegate to tune overlay singleton
    get_print_tune_overlay().update_z_offset_display(microns);
}

void PrintStatusPanel::on_led_state_changed(int state) {
    // Delegate to light/timelapse controls (extracted Phase 2)
    light_timelapse_controls_.update_led_state(state != 0);
}

void PrintStatusPanel::on_print_layer_changed(int current_layer) {
    // Read total layers from PrinterState and delegate to lifecycle
    int total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
    bool has_real_data = printer_state_.has_real_layer_data();
    if (!lifecycle_.on_layer_changed(current_layer, total_layers, has_real_data)) {
        spdlog::trace("[{}] Ignoring layer update ({}) - guarded by lifecycle", get_name(),
                      current_layer);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Update the layer text display (prefix with ~ when estimated from progress)
    // Include Z height in centimillimeters when available
    int z_centimm = lv_subject_get_int(printer_state_.get_gcode_position_z_subject());
    if (z_centimm > 0) {
        const char* fmt = has_real_data ? "Layer %d / %d (%.1fmm)" : "Layer ~%d / %d (%.1fmm)";
        std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), fmt, lifecycle_.current_layer(),
                      lifecycle_.total_layers(), z_centimm / 100.0);
    } else {
        const char* fmt = has_real_data ? "Layer %d / %d" : "Layer ~%d / %d";
        std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), fmt, lifecycle_.current_layer(),
                      lifecycle_.total_layers());
    }
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);

    // Update G-code viewer ghost layer if panel is active and viewer is visible
    if (is_active_ && gcode_viewer_ && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN) &&
        !complete_view_mode_) {
        // Map from Moonraker layer count (e.g., 240) to viewer layer count (e.g., 2912)
        // The slicer metadata and parsed G-code often have different layer counts
        int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
        int viewer_layer = lifecycle_.map_current_layer_to_viewer(viewer_max_layer);

        // CRITICAL: Defer to avoid lv_obj_invalidate() during render phase
        // Observer callbacks can fire during lv_timer_handler() which may be mid-render
        struct ViewerProgressCtx {
            lv_obj_t* viewer;
            int layer;
        };
        auto ctx =
            std::make_unique<ViewerProgressCtx>(ViewerProgressCtx{gcode_viewer_, viewer_layer});
        helix::ui::queue_update<ViewerProgressCtx>(std::move(ctx), [](ViewerProgressCtx* c) {
            if (c->viewer && lv_obj_is_valid(c->viewer)) {
                ui_gcode_viewer_set_print_progress(c->viewer, c->layer);
            }
        });

        spdlog::trace("[{}] G-code viewer ghost layer updated to {} (Moonraker: {}/{})", get_name(),
                      viewer_layer, current_layer, lifecycle_.total_layers());
    }
}

void PrintStatusPanel::on_print_duration_changed(int seconds) {
    // Get outcome from PrinterState and delegate guard + state update to lifecycle
    auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(printer_state_.get_print_outcome_subject()));
    if (!lifecycle_.on_duration_changed(seconds, outcome)) {
        spdlog::trace("[{}] Ignoring duration update ({}) - guarded by lifecycle", get_name(),
                      seconds);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // total_duration from Moonraker already includes prep time (wall-clock elapsed)
    format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
    lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
    spdlog::trace("[{}] Elapsed updated: {}s (wall-clock from Moonraker)", get_name(), seconds);
}

void PrintStatusPanel::on_print_time_left_changed(int seconds) {
    // Get outcome from PrinterState and delegate guard + state update to lifecycle
    auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(printer_state_.get_print_outcome_subject()));
    if (!lifecycle_.on_time_left_changed(seconds, outcome)) {
        spdlog::trace("[{}] Ignoring time_left update ({}) - guarded by lifecycle", get_name(),
                      seconds);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
    lv_subject_copy_string(&remaining_subject_, remaining_buf_);

    bool use_24h = DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24;
    auto eta_str = helix::format::eta_clock_time(lifecycle_.remaining_seconds(), 0, use_24h);
    std::snprintf(eta_buf_, sizeof(eta_buf_), "%s", eta_str.c_str());
    lv_subject_copy_string(&eta_subject_, eta_buf_);

    spdlog::trace("[{}] Time remaining updated: {}s, ETA: {}", get_name(), seconds, eta_buf_);
}

void PrintStatusPanel::on_print_start_phase_changed(int phase) {
    // Phase 0 = IDLE (not preparing), non-zero = preparing
    bool preparing = (phase != 0);

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Delegate state transition to lifecycle
    auto current_job_state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
    bool state_changed = lifecycle_.on_start_phase_changed(phase, current_job_state);

    // Update preparing visibility subject
    lv_subject_set_int(&preparing_visible_subject_, preparing ? 1 : 0);

    if (preparing) {
        // Preserve the thumbnail — it was loaded for the current print by the
        // filename observer or ActivePrintMediaManager. The preparing phase
        // fires concurrently with thumbnail loading, so clearing here would
        // race and discard a valid thumbnail. Stale thumbnails from a previous
        // print are cleared by the print_ended path in on_print_state_changed.
        loaded_thumbnail_filename_.clear();
        requested_gcode_filename_.clear();
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        }
        std::snprintf(progress_text_buf_, sizeof(progress_text_buf_), "0%%");
        lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);
        std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), " ");
        lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);

        // Initialize elapsed display to 0m (preprint observer will update it)
        format_time(0, elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);

        // Show predicted total as initial remaining estimate (preprint observer refines it)
        int predicted = helix::PreprintPredictor::predicted_total_from_config();
        if (predicted > 0) {
            int total_remaining = lifecycle_.remaining_seconds() + predicted;
            format_time(total_remaining, remaining_buf_, sizeof(remaining_buf_));
            lv_subject_copy_string(&remaining_subject_, remaining_buf_);
        }
    } else if (state_changed) {
        // Preparation complete - lifecycle restored state from current job state
        update_all_displays();
        update_button_states();

        // Reschedule gcode load — the deferred load scheduled before print start
        // was invalidated when entering Preparing (requested_gcode_filename_ cleared).
        if (!current_print_filename_.empty() && !gcode_loaded_) {
            set_filename(current_print_filename_.c_str());
        }

        spdlog::debug("[{}] Restored state to {} after preparation complete", get_name(),
                      static_cast<int>(lifecycle_.state()));
    }
    spdlog::debug("[{}] Print start phase changed: {} (visible={})", get_name(), phase, preparing);
}

void PrintStatusPanel::on_print_start_message_changed(const char* message) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    if (message) {
        strncpy(preparing_operation_buf_, message, sizeof(preparing_operation_buf_) - 1);
        preparing_operation_buf_[sizeof(preparing_operation_buf_) - 1] = '\0';
        lv_subject_copy_string(&preparing_operation_subject_, preparing_operation_buf_);
        spdlog::trace("[{}] Print start message: {}", get_name(), message);
    }
}

void PrintStatusPanel::on_print_start_progress_changed(int progress) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    lv_subject_set_int(&preparing_progress_subject_, progress);

    // Animate bar for smooth visual feedback
    if (preparing_progress_bar_) {
        lv_anim_enable_t anim_enable =
            DisplaySettingsManager::instance().get_animations_enabled() ? LV_ANIM_ON : LV_ANIM_OFF;
        lv_bar_set_value(preparing_progress_bar_, progress, anim_enable);
    }
    spdlog::trace("[{}] Print start progress: {}%", get_name(), progress);
}

void PrintStatusPanel::on_preprint_remaining_changed(int seconds) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Delegate to lifecycle (handles Preparing guard internally)
    // Fall back to get_estimated_print_time() if remaining_seconds hasn't been seeded yet
    int slicer_time = lifecycle_.remaining_seconds() > 0
                          ? lifecycle_.remaining_seconds()
                          : printer_state_.get_estimated_print_time();
    lifecycle_.on_preprint_remaining_changed(seconds, slicer_time);

    if (lifecycle_.state() != PrintState::Preparing) {
        return;
    }

    // Combine preprint prediction with slicer estimate for total remaining time
    int total_remaining = slicer_time + seconds;
    format_time(total_remaining, remaining_buf_, sizeof(remaining_buf_));
    lv_subject_copy_string(&remaining_subject_, remaining_buf_);
    spdlog::trace("[{}] Preprint remaining: {}s preprint + {}s slicer = {}s", get_name(), seconds,
                  slicer_time, total_remaining);
}

void PrintStatusPanel::on_preprint_elapsed_changed(int seconds) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Delegate to lifecycle (handles Preparing guard internally)
    lifecycle_.on_preprint_elapsed_changed(seconds);

    if (lifecycle_.state() != PrintState::Preparing) {
        return;
    }

    format_time(lifecycle_.preprint_elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
    lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
}

void PrintStatusPanel::update_view_toggle_position(bool objects_visible) {
    if (!gcode_viewer_)
        return;
    lv_obj_t* card = lv_obj_get_parent(gcode_viewer_);
    if (!card)
        return;
    lv_obj_t* btn = lv_obj_find_by_name(card, "btn_view_toggle");
    if (!btn)
        return;

    int32_t space_md = theme_manager_get_spacing("space_md");
    if (objects_visible) {
        lv_obj_t* btn_objects = lv_obj_find_by_name(card, "btn_objects");
        int32_t obj_w = btn_objects ? lv_obj_get_width(btn_objects) : 36;
        lv_obj_set_style_translate_x(btn, space_md + obj_w + space_md, LV_PART_MAIN);
    } else {
        lv_obj_set_style_translate_x(btn, space_md, LV_PART_MAIN);
    }
}

void PrintStatusPanel::update_objects_text() {
    if (!subjects_initialized_)
        return;
    auto& defined = printer_state_.get_defined_objects();
    auto& excluded = printer_state_.get_excluded_objects();
    int total = static_cast<int>(defined.size());
    int active = std::max(0, total - static_cast<int>(excluded.size()));
    if (total >= 2) {
        std::snprintf(objects_text_buf_, sizeof(objects_text_buf_), "%d of %d objects", active,
                      total);
    } else {
        objects_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&objects_text_subject_, objects_text_buf_);
}

void PrintStatusPanel::update_button_states() {
    // Drive button enable via subjects; XML `bind_state_if_eq ... state="disabled"
    // ref_value="0"` toggles LV_STATE_DISABLED, and ui_button dims on disabled.
    auto state = lifecycle_.state();
    bool controls_enabled = PrintLifecycleState::is_active(state);

    auto& macros = StandardMacros::instance();
    bool pause_enabled = controls_enabled &&
                         !macros.get(state == PrintState::Paused ? StandardMacroSlot::Resume
                                                                 : StandardMacroSlot::Pause)
                              .is_empty();
    bool cancel_enabled =
        controls_enabled && !macros.get(StandardMacroSlot::Cancel).is_empty();

    lv_subject_set_int(&print_controls_enabled_subject_, controls_enabled ? 1 : 0);
    lv_subject_set_int(&btn_pause_enabled_subject_, pause_enabled ? 1 : 0);
    lv_subject_set_int(&btn_cancel_enabled_subject_, cancel_enabled ? 1 : 0);

    // Cancel/Reprint visibility is driven entirely by the print_outcome subject
    // via bind_flag_if_eq / bind_flag_if_not_eq on the <ui_button> elements.

    spdlog::debug("[{}] Button states updated: controls={}, pause={}, cancel={} (state={})",
                  get_name(), controls_enabled ? "enabled" : "disabled",
                  pause_enabled ? "enabled" : "disabled",
                  cancel_enabled ? "enabled" : "disabled", static_cast<int>(state));
}

void PrintStatusPanel::animate_badge_pop_in(lv_obj_t* badge, const char* label) {
    if (!badge) {
        return;
    }

    constexpr int32_t SCALE_FINAL = 256; // 100% scale

    // Skip animation if disabled - show badge in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_transform_scale(badge, SCALE_FINAL, LV_PART_MAIN);
        lv_obj_set_style_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::debug("[{}] Animations disabled - showing {} badge instantly", get_name(), label);
        return;
    }

    // Pop-in animation: quick scale-up with overshoot, then settle
    constexpr int32_t POP_DURATION_MS = 300;
    constexpr int32_t SETTLE_DURATION_MS = 150;
    constexpr int32_t SCALE_START = 128;     // 50% scale (128/256)
    constexpr int32_t SCALE_OVERSHOOT = 282; // ~110% scale

    // Start badge small and transparent
    lv_obj_set_style_transform_scale(badge, SCALE_START, LV_PART_MAIN);
    lv_obj_set_style_opa(badge, LV_OPA_TRANSP, LV_PART_MAIN);

    // Stage 1: Scale up with overshoot + fade in
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, badge);
    lv_anim_set_values(&scale_anim, SCALE_START, SCALE_OVERSHOOT);
    lv_anim_set_duration(&scale_anim, POP_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, badge);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, POP_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    // Stage 2: Settle from overshoot to final size (delayed start)
    lv_anim_t settle_anim;
    lv_anim_init(&settle_anim);
    lv_anim_set_var(&settle_anim, badge);
    lv_anim_set_values(&settle_anim, SCALE_OVERSHOOT, SCALE_FINAL);
    lv_anim_set_duration(&settle_anim, SETTLE_DURATION_MS);
    lv_anim_set_delay(&settle_anim, POP_DURATION_MS);
    lv_anim_set_path_cb(&settle_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&settle_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&settle_anim);

    spdlog::debug("[{}] {} badge animation started", get_name(), label);
}

void PrintStatusPanel::animate_print_complete() {
    animate_badge_pop_in(success_badge_, "complete");
}

void PrintStatusPanel::animate_print_cancelled() {
    animate_badge_pop_in(cancel_badge_, "cancelled");
}

void PrintStatusPanel::animate_print_error() {
    animate_badge_pop_in(error_badge_, "error");
}

// Tune panel handlers delegated to PrintTuneOverlay singleton:
// See get_print_tune_overlay() and handle_*() methods in ui_print_tune_overlay.cpp
// XML callbacks are registered in ui_print_tune_overlay.cpp on first show()

// ============================================================================
// THUMBNAIL LOADING
// ============================================================================

void PrintStatusPanel::load_thumbnail_for_file(const std::string& filename) {
    // Increment generation to invalidate any in-flight async operations
    ++thumbnail_load_generation_;
    uint32_t current_gen = thumbnail_load_generation_;

    // If we already have a directly-set thumbnail path, don't overwrite it.
    // This happens when PrintStartController sets the path from a pre-extracted
    // USB thumbnail before the filename observer fires.
    const char* current_thumb =
        lv_subject_get_string(get_printer_state().get_print_thumbnail_path_subject());
    if (current_thumb && current_thumb[0] != '\0') {
        spdlog::debug("[{}] Thumbnail already set ({}), skipping API lookup", get_name(),
                      current_thumb);
        // Update local cache so on_activate() can restore it
        cached_thumbnail_path_ = current_thumb;
        if (print_thumbnail_) {
            lv_image_set_src(print_thumbnail_, current_thumb);
        }
        return;
    }

    // Skip if no API available (e.g., in mock mode)
    if (!api_) {
        spdlog::debug("[{}] No API available - skipping thumbnail load", get_name());
        return;
    }

    // Note: We intentionally do NOT skip if print_thumbnail_ is null.
    // The thumbnail must still be fetched and cached so that:
    // 1. The shared print_thumbnail_path is set for HomePanel to use
    // 2. The thumbnail is ready when PrintStatusPanel is later displayed
    // The lv_image_set_src() call is guarded separately below.

    // Resolve to original filename if this is a modified temp file
    // (Moonraker only has metadata for original files, not modified copies)
    std::string metadata_filename = resolve_gcode_filename(filename);

    // First, get file metadata to find thumbnail path
    auto token = lifetime_.token();
    api_->files().get_file_metadata(
        metadata_filename,
        [this, token, current_gen](const FileMetadata& metadata) {
            // L081 Mechanism C: defer the entire body — it touches member state
            // (thumbnail_load_generation_, get_name(), api_, cached_thumbnail_path_
            // via the inner cb) and dispatches to LVGL-touching code paths. Run
            // it on the main thread.
            token.defer("PrintStatusPanel::metadata_apply",
                        [this, token, current_gen, metadata]() {
                // Check if this callback is still relevant
                if (current_gen != thumbnail_load_generation_) {
                    spdlog::trace("[{}] Stale metadata callback (gen {} != {}), ignoring",
                                  get_name(), current_gen, thumbnail_load_generation_);
                    return;
                }

                // Note: Layer count from metadata is now set by ActivePrintMediaManager

                // Store slicer's estimated print time for remaining time fallback
                if (metadata.estimated_time > 0) {
                    get_printer_state().set_estimated_print_time(
                        static_cast<int>(metadata.estimated_time));
                }

                // Get the largest thumbnail available
                std::string thumbnail_rel_path = metadata.get_largest_thumbnail();
                if (thumbnail_rel_path.empty()) {
                    spdlog::debug("[{}] No thumbnail available in metadata", get_name());
                    return;
                }

                spdlog::debug("[{}] Found thumbnail: {}", get_name(), thumbnail_rel_path);

                // Note: We intentionally do NOT invalidate the cache here.
                // PrintSelectPanel already handles file modification detection and cache
                // invalidation when files are re-uploaded. Aggressive invalidation here
                // causes a race condition where Print Status deletes thumbnails that
                // Print Select just cached, resulting in placeholder thumbnails.

                // Use fetch_for_detail_view() for full-resolution PNG (not pre-scaled .bin)
                // The semantic API ensures we always get the right format for large views.
                // Create context with lifetime token for validity checking.
                ThumbnailLoadContext ctx;
                ctx.lifetime_token = token;
                ctx.generation = nullptr; // Using manual gen check below
                ctx.captured_gen = current_gen;

                get_thumbnail_cache().fetch_for_detail_view(
                    api_, thumbnail_rel_path, ctx,
                    [this, current_gen, token](const std::string& lvgl_path) {
                        // L081 Mechanism C: defer everything. The inner cb
                        // mutates cached_thumbnail_path_ and reads
                        // thumbnail_load_generation_/get_name(); fetch may
                        // invoke us off the main thread depending on cache
                        // state. Marshal the whole body.
                        token.defer("PrintStatusPanel::thumbnail_apply",
                                    [this, current_gen, lvgl_path]() {
                            // Generation check (we passed nullptr for the cache's
                            // own generation tracking).
                            if (current_gen != thumbnail_load_generation_) {
                                spdlog::trace(
                                    "[{}] Stale thumbnail callback (gen {} != {}), ignoring",
                                    get_name(), current_gen, thumbnail_load_generation_);
                                return;
                            }

                            // Store the cached path (without "A:" prefix for internal use)
                            cached_thumbnail_path_ = lvgl_path;

                            get_printer_state().set_print_thumbnail_path(lvgl_path);

                            if (print_thumbnail_) {
                                lv_image_set_src(print_thumbnail_, lvgl_path.c_str());
                                spdlog::info("[{}] Thumbnail loaded and displayed: {}",
                                             get_name(), lvgl_path);
                            } else {
                                spdlog::info(
                                    "[{}] Thumbnail cached (panel not yet displayed): {}",
                                    get_name(), lvgl_path);
                            }
                        });
                    },
                    [this, token](const std::string& error) {
                        // L081 Mechanism C: defer to access get_name() on main.
                        token.defer("PrintStatusPanel::thumbnail_fetch_error",
                                    [this, error]() {
                            spdlog::warn("[{}] Failed to fetch thumbnail: {}", get_name(),
                                         error);
                        });
                    });
            });
        },
        [this, token](const MoonrakerError& err) {
            // L081 Mechanism C: get_name() is virtual on `this`; defer to main.
            token.defer("PrintStatusPanel::metadata_error", [this, err]() {
                spdlog::debug("[{}] Failed to get file metadata: {}", get_name(), err.message);
            });
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

// ============================================================================
// G-CODE VIEWER LOADING
// ============================================================================

void PrintStatusPanel::load_gcode_for_viewing(const std::string& filename) {
    spdlog::debug("[{}] Loading G-code for viewing: {}", get_name(), filename);

    // Skip if no viewer widget
    if (!gcode_viewer_) {
        spdlog::debug("[{}] No gcode_viewer_ widget - skipping G-code load", get_name());
        return;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[{}] No API available - skipping G-code load", get_name());
        return;
    }

    // Check "Thumbnail Only" render mode - skip all gcode downloading/parsing
    if (DisplaySettingsManager::instance().get_gcode_render_mode() == 3) {
        spdlog::info("[{}] G-code render mode is Thumbnail Only - skipping G-code load",
                     get_name());
        show_gcode_viewer(false);
        return;
    }

    // Check config option to disable 3D rendering entirely
    auto* cfg = Config::get_instance();
    bool gcode_3d_enabled = cfg->get<bool>("/display/gcode_3d_enabled", true);
    if (!gcode_3d_enabled) {
        spdlog::info("[{}] G-code 3D rendering disabled via config - using thumbnail only",
                     get_name());
        show_gcode_viewer(false); // Ensure thumbnail is shown, not empty viewer
        return;
    }

    // Generate temp file path - check if we already have a cached copy
    // Use persistent cache directory (not /tmp which may be RAM-backed on embedded)
    std::string cache_dir = get_helix_cache_dir("gcode_temp");
    if (cache_dir.empty()) {
        spdlog::warn("[{}] No writable cache directory - skipping G-code preview", get_name());
        show_gcode_viewer(false);
        return;
    }
    std::string temp_path =
        cache_dir + "/print_view_" + std::to_string(std::hash<std::string>{}(filename)) + ".gcode";

    // Check if file already exists and is non-empty (cached from previous session)
    std::ifstream cached_file(temp_path, std::ios::binary | std::ios::ate);
    if (cached_file && cached_file.tellg() > 0) {
        size_t cached_size = static_cast<size_t>(cached_file.tellg());
        cached_file.close();

        // Check if cached file is safe to render
        if (helix::is_gcode_2d_streaming_safe(cached_size)) {
            spdlog::info("[{}] Using cached G-code file ({} bytes): {}", get_name(), cached_size,
                         temp_path);
            temp_gcode_path_ = temp_path;
            load_gcode_file(temp_path.c_str());
            return;
        } else {
            spdlog::debug("[{}] Cached file too large for 2D streaming, removing", get_name());
            std::remove(temp_path.c_str());
        }
    }

    // Get file metadata to check size before downloading
    // This prevents OOM on memory-constrained devices like AD5M
    std::string metadata_filename = resolve_gcode_filename(filename);

    auto token = lifetime_.token();

    // All four callbacks below fire on background threads — get_file_metadata's
    // success/error cb runs on libhv's WS event loop, download_file_to_path's
    // runs on HttpExecutor::slow(). They MUST marshal to the main thread via
    // tok.defer before touching LVGL widgets, the gcode_viewer state, or the
    // temp_gcode_path_ member. Pre-fix, the inner success cb called
    // load_gcode_file → ui_gcode_viewer_load_file_async → safe_delete on the
    // HTTP worker, racing the main render loop and producing the L081-cluster
    // heap corruption that surfaces as a SIGSEGV in get_prop_core / layout
    // (#906 family, WKC5J9SK on v0.99.56 ad5x).
    api_->files().get_file_metadata(
        metadata_filename,
        [this, token, filename, temp_path](const FileMetadata& metadata) {
            token.defer("PrintStatusPanel::gcode_metadata_ok",
                        [this, filename, temp_path, metadata]() {
                if (!helix::is_gcode_2d_streaming_safe(metadata.size)) {
                    auto mem = helix::get_system_memory_info();
                    spdlog::warn("[{}] G-code too large for 2D streaming: file={} bytes, available "
                                 "RAM={}MB - using thumbnail only",
                                 get_name(), metadata.size, mem.available_mb());
                    show_gcode_viewer(false);
                    return;
                }

                spdlog::debug("[{}] G-code size {} bytes - safe to render, streaming to disk...",
                              get_name(), metadata.size);

                if (!temp_gcode_path_.empty() && temp_gcode_path_ != temp_path) {
                    std::remove(temp_gcode_path_.c_str());
                    temp_gcode_path_.clear();
                }

                auto inner_token = lifetime_.token();
                api_->transfers().download_file_to_path(
                    "gcodes", filename, temp_path,
                    [this, inner_token, temp_path](const std::string& path) {
                        inner_token.defer("PrintStatusPanel::gcode_download_ok",
                                          [this, path]() {
                            temp_gcode_path_ = path;
                            spdlog::debug("[{}] Streamed G-code to disk, loading into viewer: {}",
                                          get_name(), path);
                            load_gcode_file(path.c_str());
                        });
                    },
                    [this, inner_token, filename](const MoonrakerError& err) {
                        inner_token.defer("PrintStatusPanel::gcode_download_err",
                                          [this, filename, err]() {
                            spdlog::warn("[{}] Failed to stream G-code for viewing '{}': {}",
                                         get_name(), filename, err.message);
                            show_gcode_viewer(false);
                        });
                    });
            });
        },
        [this, token, filename](const MoonrakerError& err) {
            token.defer("PrintStatusPanel::gcode_metadata_err",
                        [this, filename, err]() {
                spdlog::debug("[{}] Failed to get G-code metadata for '{}': {} - skipping 3D render",
                              get_name(), filename, err.message);
                show_gcode_viewer(false);
            });
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

// ============================================================================
// FILAMENT COLOR OVERRIDE
// ============================================================================

void PrintStatusPanel::apply_filament_color_override(uint32_t color_rgb) {
    if (!gcode_viewer_ || !gcode_loaded_) {
        return;
    }

    // Try per-tool AMS color mapping first (handles multi-color prints)
    if (build_and_apply_tool_colors()) {
        return; // Per-tool colors applied — don't clobber with single-color
    }

    // Fallback: no per-tool mapping available — use single-color override
    if (color_rgb != 0 && color_rgb != AMS_DEFAULT_SLOT_COLOR) {
        ui_gcode_viewer_set_extrusion_color(gcode_viewer_, lv_color_hex(color_rgb));
    }
}

bool PrintStatusPanel::build_and_apply_tool_colors() {
    if (!gcode_viewer_ || !gcode_loaded_) {
        return false;
    }

    return ui_gcode_viewer_apply_ams_tool_colors(gcode_viewer_);
}

// ============================================================================
// PUBLIC API
// ============================================================================

void PrintStatusPanel::set_temp_control_panel(TemperatureService* temp_panel) {
    temp_control_panel_ = temp_panel;
    spdlog::trace("[{}] TemperatureService reference set", get_name());
}

void PrintStatusPanel::schedule_deferred_gcode_load() {
    // Cancel any existing timer (debounce: if filename changes rapidly, only load the latest)
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    if (pending_gcode_filename_.empty())
        return;

    // Short delay if already printing (user is actively viewing), longer during
    // homing/heating to avoid memory spike while printer is still preparing
    uint32_t delay_ms =
        (lifecycle_.state() == PrintState::Printing || lifecycle_.state() == PrintState::Paused)
            ? 500
            : 5000;

    spdlog::debug("[{}] Scheduling deferred G-code load in {}ms: {}", get_name(), delay_ms,
                  pending_gcode_filename_);

    gcode_load_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<PrintStatusPanel*>(lv_timer_get_user_data(timer));
            self->gcode_load_timer_ = nullptr; // timer is auto-deleted after one-shot
            if (!self->pending_gcode_filename_.empty()) {
                spdlog::info("[{}] Deferred G-code load firing: {}", self->get_name(),
                             self->pending_gcode_filename_);
                self->load_gcode_for_viewing(self->pending_gcode_filename_);
                self->pending_gcode_filename_.clear();
            }
        },
        delay_ms, this);
    lv_timer_set_repeat_count(gcode_load_timer_, 1); // one-shot
}

void PrintStatusPanel::set_filename(const char* filename) {
    // Store the actual filename (may be a temp file path)
    current_print_filename_ = filename ? filename : "";

    // Use thumbnail_source_filename_ if set (for modified temp files)
    // This affects BOTH the display name AND the thumbnail lookup
    std::string effective_filename =
        thumbnail_source_filename_.empty() ? current_print_filename_ : thumbnail_source_filename_;

    // Note: Display filename is now handled by ActivePrintMediaManager
    // PrintStatusPanel only needs to load local resources (gcode viewer, local thumbnail)

    // Load thumbnail ONLY if effective filename changed (makes this function idempotent)
    // This prevents redundant loads when observer fires repeatedly with same filename
    if (!effective_filename.empty() && effective_filename != loaded_thumbnail_filename_) {
        // Clear stale cached thumbnail from previous print
        cached_thumbnail_path_.clear();
        spdlog::debug("[{}] Loading thumbnail for: {}", get_name(), effective_filename);
        load_thumbnail_for_file(effective_filename);

        // G-code loading: deduplicate to avoid redundant expensive downloads.
        // Multiple observers can fire in rapid succession (filename changed,
        // thumbnail source set) causing set_filename() to be called several
        // times with the same effective filename before the async download
        // completes. requested_gcode_filename_ tracks what we've already
        // requested, preventing duplicate loads.
        if (effective_filename != requested_gcode_filename_) {
            requested_gcode_filename_ = effective_filename;
            pending_gcode_filename_ = effective_filename;
            if (is_active_) {
                schedule_deferred_gcode_load();
            }
            // else: on_activate() will schedule the deferred load when panel becomes visible
        } else {
            spdlog::debug("[{}] Skipping duplicate G-code load request for: {}", get_name(),
                          effective_filename);
        }
        loaded_thumbnail_filename_ = effective_filename;
    }
}

void PrintStatusPanel::set_thumbnail_source(const std::string& filename) {
    thumbnail_source_filename_ = filename;
    spdlog::debug("[{}] Thumbnail source set to: {}", get_name(),
                  filename.empty() ? "(cleared)" : filename);

    // If we already have a print filename, refresh everything now.
    // This handles the race condition where Moonraker sends the filename
    // before PrintPreparationManager calls set_thumbnail_source().
    // set_filename() will re-compute the effective filename (now using the
    // thumbnail source) and reload: display name, thumbnail, and G-code viewer.
    if (!current_print_filename_.empty() && !filename.empty()) {
        spdlog::info("[{}] Refreshing display/thumbnail/gcode with source override: {} -> {}",
                     get_name(), current_print_filename_, filename);
        set_filename(current_print_filename_.c_str());
    } else if (!filename.empty()) {
        // WebSocket hasn't updated current_print_filename_ yet (race condition).
        // Clear loaded filename so when on_print_filename_changed() eventually
        // fires and calls set_filename(), the idempotency check will pass and
        // trigger the actual thumbnail/gcode load.
        loaded_thumbnail_filename_.clear();
        spdlog::debug(
            "[{}] Source set before WebSocket, cleared loaded filename for deferred reload",
            get_name());
    }
}

void PrintStatusPanel::set_progress(int percent) {
    lifecycle_.on_progress_changed(percent);
    if (!subjects_initialized_)
        return;
    helix::format::format_percent(lifecycle_.progress(), progress_text_buf_,
                                  sizeof(progress_text_buf_));
    lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);
}

void PrintStatusPanel::set_layer(int current, int total) {
    lifecycle_.on_layer_changed(current, total, printer_state_.has_real_layer_data());
    if (!subjects_initialized_)
        return;
    const char* layer_fmt =
        printer_state_.has_real_layer_data() ? "Layer %d / %d" : "Layer ~%d / %d";
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), layer_fmt, lifecycle_.current_layer(),
                  lifecycle_.total_layers());
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
}

void PrintStatusPanel::set_times(int elapsed_secs, int remaining_secs) {
    // Use NONE outcome so lifecycle doesn't guard against it
    lifecycle_.on_duration_changed(elapsed_secs, PrintOutcome::NONE);
    lifecycle_.on_time_left_changed(remaining_secs, PrintOutcome::NONE);
    if (!subjects_initialized_)
        return;
    if (lifecycle_.state() != PrintState::Preparing && lifecycle_.state() != PrintState::Complete) {
        format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
        format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);
    }
}

void PrintStatusPanel::set_speeds(int speed_pct, int flow_pct) {
    lifecycle_.on_speed_changed(speed_pct);
    lifecycle_.on_flow_changed(flow_pct);
    if (!subjects_initialized_)
        return;
    helix::format::format_percent(lifecycle_.speed_percent(), speed_buf_, sizeof(speed_buf_));
    lv_subject_copy_string(&speed_subject_, speed_buf_);
    helix::format::format_percent(lifecycle_.flow_percent(), flow_buf_, sizeof(flow_buf_));
    lv_subject_copy_string(&flow_subject_, flow_buf_);
}

void PrintStatusPanel::set_state(PrintState state) {
    // Map PrintState back to PrintJobState so lifecycle can process the transition
    PrintJobState mapped = PrintJobState::STANDBY;
    switch (state) {
    case PrintState::Idle:
        mapped = PrintJobState::STANDBY;
        break;
    case PrintState::Printing:
        mapped = PrintJobState::PRINTING;
        break;
    case PrintState::Paused:
        mapped = PrintJobState::PAUSED;
        break;
    case PrintState::Complete:
        mapped = PrintJobState::COMPLETE;
        break;
    case PrintState::Cancelled:
        mapped = PrintJobState::CANCELLED;
        break;
    case PrintState::Error:
        mapped = PrintJobState::ERROR;
        break;
    case PrintState::Preparing:
        // Preparing is handled via on_start_phase_changed, not here
        mapped = PrintJobState::PRINTING;
        break;
    }
    lifecycle_.on_job_state_changed(mapped, PrintOutcome::NONE);
    update_all_displays();
    update_button_states();
    spdlog::debug("[{}] State changed to: {}", get_name(), static_cast<int>(state));
}

// ============================================================================
// PRE-PRINT PREPARATION STATE
// ============================================================================

void PrintStatusPanel::end_preparing(bool success) {
    // Hide preparing UI
    lv_subject_set_int(&preparing_visible_subject_, 0);
    lv_subject_set_int(&preparing_progress_subject_, 0);

    if (success) {
        // Transition to Printing state
        set_state(PrintState::Printing);
        spdlog::debug("[{}] Preparation complete, starting print", get_name());
    } else {
        // Transition back to Idle
        set_state(PrintState::Idle);
        spdlog::warn("[{}] Preparation cancelled or failed", get_name());
    }
}

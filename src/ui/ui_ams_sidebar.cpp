// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_sidebar.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_step_progress.h"
#include "ui_temperature_utils.h"

#include "active_material_provider.h"
#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "filament_database.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "ui/ui_cleanup_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsOperationSidebar::AmsOperationSidebar(PrinterState& ps, MoonrakerAPI* api)
    : printer_state_(ps), api_(api) {
    spdlog::debug("[AmsSidebar] Constructed");
}

AmsOperationSidebar::~AmsOperationSidebar() {
    cleanup();
    spdlog::debug("[AmsSidebar] Destroyed");
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsOperationSidebar::register_callbacks_static() {
    register_xml_callbacks({
        {"ams_sidebar_bypass_toggled", on_bypass_toggled_cb},
        {"ams_sidebar_unload_clicked", on_unload_clicked_cb},
        {"ams_sidebar_reset_clicked", on_reset_clicked_cb},
        {"ams_sidebar_settings_clicked", on_settings_clicked_cb},
    });
}

// ============================================================================
// Static Callback Routing (parent chain traversal)
// ============================================================================

AmsOperationSidebar* AmsOperationSidebar::get_instance_from_event(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    // Find the ams_sidebar component root by name, then get our instance from its user_data.
    // Cannot walk parents checking any user_data — ui_button and other widgets set their own
    // user_data, which would be miscast as AmsOperationSidebar* (L069).
    lv_obj_t* obj = target;
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && strcmp(name, "ams_operation_sidebar") == 0) {
            void* user_data = lv_obj_get_user_data(obj);
            if (user_data) {
                return static_cast<AmsOperationSidebar*>(user_data);
            }
        }
        obj = lv_obj_get_parent(obj);
    }

    spdlog::warn("[AmsSidebar] Could not find instance from event target");
    return nullptr;
}

// ============================================================================
// Static XML Callbacks
// ============================================================================

void AmsOperationSidebar::on_bypass_toggled_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_bypass_toggle();
    }
}

void AmsOperationSidebar::on_unload_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_unload();
    }
}

void AmsOperationSidebar::on_reset_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_reset();
    }
}

void AmsOperationSidebar::on_settings_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsSidebar] on_settings_clicked");

    spdlog::info("[AmsSidebar] Opening AMS Device Operations overlay");

    auto& overlay = helix::ui::get_ams_device_operations_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }

    auto* event_target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_obj_t* parent = lv_obj_get_screen(event_target);
    overlay.show(parent);

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Setup
// ============================================================================

bool AmsOperationSidebar::setup(lv_obj_t* panel) {
    if (!panel) {
        spdlog::error("[AmsSidebar] NULL panel");
        return false;
    }

    sidebar_root_ = lv_obj_find_by_name(panel, "ams_operation_sidebar");
    if (!sidebar_root_) {
        spdlog::error("[AmsSidebar] sidebar component not found in panel");
        return false;
    }

    // Store this pointer for static callback routing
    lv_obj_set_user_data(sidebar_root_, this);

    // Setup step progress
    setup_step_progress();

    // Setup clog detection meter
    clog_meter_ = std::make_unique<UiClogMeter>(sidebar_root_);

    // Hide settings button if no device sections
    update_settings_visibility();

    active_ = true;
    sync_reset_button_label();
    spdlog::debug("[AmsSidebar] Setup complete");
    return true;
}

void AmsOperationSidebar::setup_step_progress() {
    step_progress_container_ = lv_obj_find_by_name(sidebar_root_, "progress_stepper_container");
    if (!step_progress_container_) {
        spdlog::warn("[AmsSidebar] progress_stepper_container not found");
        return;
    }

    // Create initial step progress widget (fresh load by default)
    recreate_step_progress_for_operation(StepOperationType::LOAD_FRESH);

    spdlog::debug("[AmsSidebar] Step progress widget created");
}

// ============================================================================
// Observers
// ============================================================================

void AmsOperationSidebar::init_observers() {
    // Action observer: drives step progress and load completion detection
    action_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_ams_action_subject(), this,
        [](AmsOperationSidebar* self, int action_int) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            auto action = static_cast<AmsAction>(action_int);
            spdlog::debug("[AmsSidebar] Action changed: {} (prev={})", ams_action_to_string(action),
                          ams_action_to_string(self->prev_ams_action_));

            // Detect LOADING -> IDLE or LOADING -> ERROR for post-load cooling
            if (self->prev_ams_action_ == AmsAction::LOADING &&
                (action == AmsAction::IDLE || action == AmsAction::ERROR)) {
                self->handle_load_complete();
            }

            // Detect UNLOADING -> IDLE: if bypass is pending, enable it now
            if (self->pending_bypass_enable_ && action == AmsAction::IDLE &&
                self->prev_ams_action_ == AmsAction::UNLOADING) {
                self->pending_bypass_enable_ = false;
                AmsBackend* backend = AmsState::instance().get_backend();
                if (backend) {
                    spdlog::info("[AmsSidebar] Unload complete — enabling bypass");
                    AmsError err = backend->enable_bypass();
                    if (err.result == AmsResult::SUCCESS) {
                        NOTIFY_INFO(lv_tr("Bypass enabled"));
                    } else {
                        NOTIFY_ERROR(lv_tr("Bypass failed: {}"), err.user_msg);
                    }
                }
            }

            // Update step progress (BEFORE updating prev_ams_action_)
            self->update_action_display(action);

            self->prev_ams_action_ = action;
        });

    // Current slot observer: updates loaded card display and reset button label
    current_slot_observer_ =
        observe_int_sync<AmsOperationSidebar>(AmsState::instance().get_current_slot_subject(), this,
                                              [](AmsOperationSidebar* self, int /*slot_index*/) {
                                                  if (!self->active_ || !self->sidebar_root_)
                                                      return;
                                                  self->update_current_loaded_display();
                                                  self->sync_reset_button_label();
                                              });

    // Active backend observer: re-syncs reset button label when the user switches backend tabs
    active_backend_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_active_backend_subject(), this,
        [](AmsOperationSidebar* self, int /*active_index*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->sync_reset_button_label();
        });

    // Bypass spool color observer: refreshes loaded card when external spool changes
    bypass_spool_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](AmsOperationSidebar* self, int /*color_rgb*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->update_current_loaded_display();
        });

    // Color observer: reactively updates loaded card swatch color
    color_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_current_color_subject(), this,
        [](AmsOperationSidebar* self, int color_int) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            lv_obj_t* swatch = lv_obj_find_by_name(self->sidebar_root_, "loaded_swatch");
            if (swatch) {
                lv_color_t color = lv_color_hex(static_cast<uint32_t>(color_int));
                lv_obj_set_style_bg_color(swatch, color, 0);
                lv_obj_set_style_border_color(swatch, color, 0);
            }
        });

    // Extruder temp observer: checks pending preheat load + refreshes heat step
    extruder_temp_observer_ = observe_int_sync<AmsOperationSidebar>(
        printer_state_.get_active_extruder_temp_subject(), this,
        [](AmsOperationSidebar* self, int /*temp_centi*/) {
            if (!self->active_)
                return;
            self->check_pending_load();
            self->refresh_heat_step_display();
        });

    // Extruder target observer: refreshes heat step when target temp changes
    // (the macro raises the target before any visible action change)
    extruder_target_observer_ = observe_int_sync<AmsOperationSidebar>(
        printer_state_.get_active_extruder_target_subject(), this,
        [](AmsOperationSidebar* self, int /*target_centi*/) {
            if (!self->active_)
                return;
            self->refresh_heat_step_display();
        });
}

// ============================================================================
// Cleanup
// ============================================================================

void AmsOperationSidebar::cleanup() {
    // Clear active flag FIRST to prevent observer callbacks from using freed widgets
    active_ = false;

    // Nullify widget refs BEFORE resetting observers — any cascading observer
    // callbacks that slip through the active_ guard will see null pointers and
    // bail out, preventing use-after-free on deleted LVGL objects.
    if (sidebar_root_) {
        lv_obj_set_user_data(sidebar_root_, nullptr);
    }
    sidebar_root_ = nullptr;
    step_progress_ = nullptr;
    step_progress_container_ = nullptr;

    // Reset ALL observers unconditionally. Keeping extruder_temp_observer_ alive
    // across panel switches is unsafe — the sidebar may be destroyed while the
    // observer still holds a raw pointer to it.
    action_observer_.reset();
    current_slot_observer_.reset();
    active_backend_observer_.reset();
    bypass_spool_observer_.reset();
    color_observer_.reset();
    extruder_temp_observer_.reset();

    // Reset extracted modules AFTER observers — they may have their own observers
    // that reference widget pointers; resetting before our observers could
    // trigger callbacks on already-null widget pointers.
    clog_meter_.reset();

    // Clear all pending state
    pending_bypass_enable_ = false;
    pending_load_slot_ = -1;
    pending_load_target_temp_ = 0;
    ui_initiated_heat_ = false;
    prev_ams_action_ = AmsAction::IDLE;

    spdlog::debug("[AmsSidebar] Cleaned up");
}

// ============================================================================
// Sync from State (call on panel activate)
// ============================================================================

void AmsOperationSidebar::sync_from_state() {
    if (!sidebar_root_) {
        return;
    }

    // Sync step progress with current action
    auto action =
        static_cast<AmsAction>(lv_subject_get_int(AmsState::instance().get_ams_action_subject()));
    update_step_progress(action);

    // If we're in a UI-managed preheat, restore visual feedback
    if (pending_load_slot_ >= 0 && pending_load_target_temp_ > 0) {
        show_preheat_feedback(pending_load_slot_, pending_load_target_temp_);
    }

    // Sync loaded card display
    update_current_loaded_display();

    // Update settings visibility (backend may have changed)
    update_settings_visibility();
}

// ============================================================================
// Settings Visibility
// ============================================================================

void AmsOperationSidebar::update_settings_visibility() {
    if (!sidebar_root_) {
        return;
    }

    auto* backend = AmsState::instance().get_backend(0);
    lv_obj_t* btn_settings = lv_obj_find_by_name(sidebar_root_, "btn_settings");
    if (btn_settings && backend) {
        auto sections = backend->get_device_sections();
        if (sections.empty()) {
            lv_obj_add_flag(btn_settings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(btn_settings, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Reset Button Label
// ============================================================================

void AmsOperationSidebar::sync_reset_button_label() {
    if (!active_ || !sidebar_root_) {
        return;
    }
    lv_obj_t* btn_reset = lv_obj_find_by_name(sidebar_root_, "btn_reset");
    if (!btn_reset) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    std::string label = backend ? backend->reset_button_label() : std::string("Reset");
    ui_button_set_text(btn_reset, lv_tr(label.c_str()));
}

// ============================================================================
// Current Loaded Display
// ============================================================================

void AmsOperationSidebar::update_current_loaded_display() {
    if (!sidebar_root_) {
        return;
    }

    // Subjects updated reactively by sync_from_backend(); color swatch driven by color_observer_
}

// ============================================================================
// Action Display
// ============================================================================

void AmsOperationSidebar::update_action_display(AmsAction action) {
    // Sidebar-only action display: step progress
    // Path canvas heat glow and error modal stay in the host panel
    update_step_progress(action);
}

// ============================================================================
// Step Progress
// ============================================================================

void AmsOperationSidebar::recreate_step_progress_for_operation(StepOperationType op_type) {
    if (!active_ || !step_progress_container_) {
        return;
    }

    // Delete existing step progress widget if any
    safe_delete_obj(step_progress_);
    heat_label_showing_temp_ = false; // fresh widget has plain "Heat nozzle" label

    current_operation_type_ = op_type;

    // Get capabilities from backend for dynamic labels
    TipMethod tip_method = TipMethod::CUT;
    bool supports_purge = false;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo info = backend->get_system_info();
        tip_method = info.tip_method;
        supports_purge = info.supports_purge;
    }
    const char* tip_step_label = tip_method_step_label(tip_method);

    switch (op_type) {
    case StepOperationType::LOAD_FRESH: {
        if (supports_purge) {
            ui_step_t steps[] = {
                {"Heat nozzle", StepState::Pending},
                {"Feed filament", StepState::Pending},
                {"Purge", StepState::Pending},
            };
            current_step_count_ = 3;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 3, false,
                                                     "ams_step_progress");
        } else {
            ui_step_t steps[] = {
                {"Heat nozzle", StepState::Pending},
                {"Feed filament", StepState::Pending},
            };
            current_step_count_ = 2;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 2, false,
                                                     "ams_step_progress");
        }
        break;
    }
    case StepOperationType::LOAD_SWAP: {
        if (supports_purge) {
            ui_step_t steps[] = {
                {"Heat nozzle", StepState::Pending},
                {tip_step_label, StepState::Pending},
                {"Feed filament", StepState::Pending},
                {"Purge", StepState::Pending},
            };
            current_step_count_ = 4;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 4, false,
                                                     "ams_step_progress");
        } else {
            ui_step_t steps[] = {
                {"Heat nozzle", StepState::Pending},
                {tip_step_label, StepState::Pending},
                {"Feed filament", StepState::Pending},
            };
            current_step_count_ = 3;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 3, false,
                                                     "ams_step_progress");
        }
        break;
    }
    case StepOperationType::UNLOAD: {
        ui_step_t steps[] = {
            {"Heat nozzle", StepState::Pending},
            {tip_step_label, StepState::Pending},
            {"Retract", StepState::Pending},
        };
        current_step_count_ = 3;
        step_progress_ =
            ui_step_progress_create(step_progress_container_, steps, 3, false, "ams_step_progress");
        break;
    }
    }

    if (!step_progress_) {
        spdlog::error("[AmsSidebar] Failed to create step progress for op_type={}",
                      static_cast<int>(op_type));
    } else {
        spdlog::debug("[AmsSidebar] Created step progress: {} steps for op_type={}",
                      current_step_count_, static_cast<int>(op_type));
    }
}

int AmsOperationSidebar::get_step_index_for_action(AmsAction action, StepOperationType op_type) {
    switch (op_type) {
    case StepOperationType::LOAD_FRESH:
        switch (action) {
        case AmsAction::HEATING:
            return 0;
        case AmsAction::LOADING:
            return 1;
        case AmsAction::PURGING:
            return 2;
        case AmsAction::IDLE:
            return -1;
        default:
            return -1;
        }

    case StepOperationType::LOAD_SWAP:
        switch (action) {
        case AmsAction::HEATING:
            return 0;
        case AmsAction::CUTTING:
        case AmsAction::FORMING_TIP:
        case AmsAction::UNLOADING:
            return 1;
        case AmsAction::LOADING:
            return 2;
        case AmsAction::PURGING:
            return 3;
        case AmsAction::IDLE:
            return -1;
        default:
            return -1;
        }

    case StepOperationType::UNLOAD:
        switch (action) {
        case AmsAction::HEATING:
            return 0;
        case AmsAction::CUTTING:
        case AmsAction::FORMING_TIP:
            return 1;
        case AmsAction::UNLOADING:
            return 2;
        case AmsAction::IDLE:
            return -1;
        default:
            return -1;
        }
    }
    return -1;
}

void AmsOperationSidebar::start_operation(StepOperationType op_type, int target_slot) {
    spdlog::info("[AmsSidebar] Starting operation: type={}, target_slot={}",
                 static_cast<int>(op_type), target_slot);

    target_load_slot_ = target_slot;

    // Set pending target slot early for pulse animation
    AmsState::instance().set_pending_target_slot(target_slot);

    // Set action to HEATING immediately — triggers XML binding to hide buttons
    AmsState::instance().set_action(AmsAction::HEATING);

    // Create step progress with correct steps
    recreate_step_progress_for_operation(op_type);

    // Show step progress immediately
    if (step_progress_container_) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AmsOperationSidebar::update_step_progress(AmsAction action) {
    if (!active_ || !step_progress_container_) {
        return;
    }

    // Heuristic detection for externally-started operations
    bool is_external = (target_load_slot_ < 0);
    bool filament_loaded = false;
    if (is_external) {
        AmsBackend* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsSystemInfo info = backend->get_system_info();
            filament_loaded = (info.current_slot >= 0);
        }
    }

    auto detection = detect_step_operation(action, prev_ams_action_, current_operation_type_,
                                           is_external, filament_loaded);
    if (detection.should_recreate) {
        if (detection.op_type == StepOperationType::LOAD_SWAP &&
            current_operation_type_ == StepOperationType::UNLOAD) {
            spdlog::debug("[AmsSidebar] Upgrading UNLOAD → LOAD_SWAP");
        }
        recreate_step_progress_for_operation(detection.op_type);
        if (detection.jump_to_step >= 0 && step_progress_) {
            ui_step_progress_set_current(step_progress_, detection.jump_to_step);
        }
    }

    if (!step_progress_) {
        return;
    }

    // Show/hide container based on action
    bool show_progress = (action == AmsAction::HEATING || action == AmsAction::LOADING ||
                          action == AmsAction::PURGING || action == AmsAction::CUTTING ||
                          action == AmsAction::FORMING_TIP || action == AmsAction::UNLOADING);

    if (show_progress) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
        target_load_slot_ = -1;
        if (heat_label_showing_temp_) {
            ui_step_progress_set_label(step_progress_, 0, lv_tr("Heat nozzle"));
            heat_label_showing_temp_ = false;
        }
        return;
    }

    int step_index = get_step_index_for_action(action, current_operation_type_);

    // Physical-state anchor: backends emit LOADING/UNLOADING/etc. at gcode dispatch,
    // before the printer physically leaves the heating phase. Hold the indicator at
    // step 0 with a live "X / Y°C" label until the extruder reaches its target.
    if (is_extruder_below_target()) {
        step_index = 0;
        int current_centi = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        int target_centi = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        char temp_buf[32];
        temperature::format_temperature_pair(current_centi / 10, target_centi / 10, temp_buf,
                                             sizeof(temp_buf));
        char label_buf[64];
        snprintf(label_buf, sizeof(label_buf), "%s %s", lv_tr("Heat nozzle"), temp_buf);
        ui_step_progress_set_label(step_progress_, 0, label_buf);
        heat_label_showing_temp_ = true;
    } else if (heat_label_showing_temp_) {
        ui_step_progress_set_label(step_progress_, 0, lv_tr("Heat nozzle"));
        heat_label_showing_temp_ = false;
    }

    if (step_index >= 0) {
        ui_step_progress_set_current(step_progress_, step_index);
    }
}

bool AmsOperationSidebar::is_extruder_below_target() const {
    int target_centi = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
    if (target_centi <= 0) {
        return false;
    }
    int current_centi = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    // 5°C threshold matches check_pending_load() at line ~795
    constexpr int TEMP_THRESHOLD_CENTI = 50;
    return current_centi < (target_centi - TEMP_THRESHOLD_CENTI);
}

void AmsOperationSidebar::refresh_heat_step_display() {
    if (!active_ || !step_progress_) {
        return;
    }
    int action_int = lv_subject_get_int(AmsState::instance().get_ams_action_subject());
    update_step_progress(static_cast<AmsAction>(action_int));
}

// ============================================================================
// Action Handlers
// ============================================================================

void AmsOperationSidebar::handle_unload() {
    spdlog::info("[AmsSidebar] Unload requested");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("AMS not available"));
        return;
    }

    AmsSystemInfo info = backend->get_system_info();
    if (info.current_slot >= 0) {
        start_operation(StepOperationType::UNLOAD, info.current_slot);
    }

    AmsError error = backend->unload_filament();
    if (error.result != AmsResult::SUCCESS) {
        NOTIFY_ERROR(lv_tr("Unload failed: {}"), error.user_msg);
    }
}

void AmsOperationSidebar::handle_reset() {
    spdlog::info("[AmsSidebar] Reset requested");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("AMS not available"));
        return;
    }

    AmsError error = backend->reset();
    if (error.result != AmsResult::SUCCESS) {
        NOTIFY_ERROR(lv_tr("Reset failed: {}"), error.user_msg);
    }
}

void AmsOperationSidebar::handle_bypass_toggle() {
    spdlog::info("[AmsSidebar] Bypass toggle requested");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("AMS not available"));
        return;
    }

    AmsSystemInfo info = backend->get_system_info();
    if (info.has_hardware_bypass_sensor) {
        NOTIFY_WARNING(lv_tr("Bypass controlled by sensor"));
        spdlog::warn("[AmsSidebar] Bypass toggle blocked — hardware sensor controls bypass");
        return;
    }

    bool currently_bypassed = backend->is_bypass_active();
    AmsError error;

    if (currently_bypassed) {
        error = backend->disable_bypass();
        if (error.result == AmsResult::SUCCESS) {
            NOTIFY_INFO(lv_tr("Bypass disabled"));
        }
    } else {
        // If filament is loaded from an AMS slot, unload first (full animation),
        // then enable bypass after unload completes via action observer
        if (info.current_slot >= 0 && info.filament_loaded) {
            spdlog::info("[AmsSidebar] Unloading slot {} before enabling bypass",
                         info.current_slot);
            pending_bypass_enable_ = true;
            error = backend->unload_filament();
            if (error.result == AmsResult::SUCCESS) {
                NOTIFY_INFO(lv_tr("Unloading before bypass..."));
            } else {
                pending_bypass_enable_ = false;
                NOTIFY_ERROR(lv_tr("Unload failed: {}"), error.user_msg);
            }
            return;
        }

        // No filament loaded — enable bypass directly
        error = backend->enable_bypass();
        if (error.result == AmsResult::SUCCESS) {
            NOTIFY_INFO(lv_tr("Bypass enabled"));
        }
    }

    if (error.result != AmsResult::SUCCESS) {
        NOTIFY_ERROR(lv_tr("Bypass toggle failed: {}"), error.user_msg);
    }
}

// ============================================================================
// Preheat Logic
// ============================================================================

int AmsOperationSidebar::get_load_temp_for_slot(int slot_index) {
    // External spool (bypass/direct)
    if (slot_index == -2) {
        auto info = AmsState::instance().get_external_spool_info();
        if (info.has_value()) {
            auto active = helix::build_active_material(*info);
            return active.material_info.nozzle_min;
        }
        return AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP;
    }

    SlotInfo info = backend->get_slot_info(slot_index);
    auto active = helix::build_active_material(info);
    return active.material_info.nozzle_min;
}

void AmsOperationSidebar::handle_load_with_preheat(int slot_index) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }

    // Tool changers: just send T{n}
    if (backend->get_type() == AmsType::TOOL_CHANGER) {
        AmsSystemInfo info = backend->get_system_info();
        if (info.current_slot >= 0 && info.current_slot == slot_index) {
            spdlog::debug("[AmsSidebar] Tool {} already active, ignoring load", slot_index);
            return;
        }
        backend->load_filament(slot_index);
        return;
    }

    // Determine operation type BEFORE calling backend. The load-vs-swap rule is
    // centralized in needs_unload_before_load() so the UI and backend agree —
    // K1 CFS reports a preloaded cassette slot with an empty nozzle, and a SWAP
    // there would cut nothing and stall at the cut step (#968). The K1 override
    // keys on filament_loaded only; K2 keeps filament_loaded || current_slot.
    AmsSystemInfo info = backend->get_system_info();
    if (backend->needs_unload_before_load(info) && info.current_slot != slot_index) {
        start_operation(StepOperationType::LOAD_SWAP, slot_index);
    } else {
        start_operation(StepOperationType::LOAD_FRESH, slot_index);
    }

    // Helper: initiate load or tool change depending on current state
    auto do_load_or_swap = [&]() {
        if (backend->needs_unload_before_load(info) && info.current_slot != slot_index) {
            const SlotInfo* slot_info = info.get_slot_global(slot_index);
            if (slot_info && slot_info->mapped_tool >= 0) {
                spdlog::info("[AmsSidebar] Preheat path: swapping via tool change T{}",
                             slot_info->mapped_tool);
                backend->change_tool(slot_info->mapped_tool);
            } else {
                spdlog::info("[AmsSidebar] Preheat path: unload first, then load {}", slot_index);
                backend->unload_filament();
            }
        } else {
            backend->load_filament(slot_index);
        }
    };

    // If backend handles heating automatically, just call load directly
    if (backend->supports_auto_heat_on_load()) {
        ui_initiated_heat_ = false;
        do_load_or_swap();
        return;
    }

    // Otherwise, UI handles preheat
    int target = get_load_temp_for_slot(slot_index);

    int current_centi = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int current = current_centi / 10;

    constexpr int TEMP_THRESHOLD = 5;
    if (current >= (target - TEMP_THRESHOLD)) {
        ui_initiated_heat_ = false;
        do_load_or_swap();
        return;
    }

    // Start preheating
    pending_load_slot_ = slot_index;
    pending_load_target_temp_ = target;
    ui_initiated_heat_ = true;

    if (api_) {
        api_->set_temperature(
            printer_state_.active_extruder_name(), target, []() {},
            [](const MoonrakerError& /*err*/) {});
    }

    show_preheat_feedback(slot_index, target);

    spdlog::info("[AmsSidebar] Starting preheat to {}C for slot {} load", target, slot_index);
}

void AmsOperationSidebar::check_pending_load() {
    if (pending_load_slot_ < 0) {
        return;
    }

    int current_centi = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int current = current_centi / 10;

    // Update display with current temperature while waiting
    char temp_buf[32];
    temperature::format_temperature_pair(current, pending_load_target_temp_, temp_buf,
                                         sizeof(temp_buf));
    AmsState::instance().set_action_detail(temp_buf);

    constexpr int TEMP_THRESHOLD = 5;

    if (current >= (pending_load_target_temp_ - TEMP_THRESHOLD)) {
        int slot = pending_load_slot_;
        pending_load_slot_ = -1;
        pending_load_target_temp_ = 0;

        AmsBackend* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsSystemInfo preheat_info = backend->get_system_info();
            // Same centralized load-vs-swap rule as handle_load_with_preheat /
            // on_path_slot_clicked — keeps K1 vs K2 consistent (#968).
            if (backend->needs_unload_before_load(preheat_info) &&
                preheat_info.current_slot != slot) {
                const SlotInfo* slot_info = preheat_info.get_slot_global(slot);
                if (slot_info && slot_info->mapped_tool >= 0) {
                    spdlog::info("[AmsSidebar] Preheat complete, swapping via tool change T{}",
                                 slot_info->mapped_tool);
                    backend->change_tool(slot_info->mapped_tool);
                } else {
                    spdlog::info("[AmsSidebar] Preheat complete, unloading first then load {}",
                                 slot);
                    backend->unload_filament();
                }
            } else {
                spdlog::info("[AmsSidebar] Preheat complete, loading slot {}", slot);
                backend->load_filament(slot);
            }
        }
    }
}

void AmsOperationSidebar::handle_load_complete() {
    if (ui_initiated_heat_) {
        PostOpCooldownManager::instance().schedule();
        spdlog::info("[AmsSidebar] Load complete, scheduling cooldown (UI-initiated heat)");
        ui_initiated_heat_ = false;
    }
}

void AmsOperationSidebar::show_preheat_feedback(int slot_index, int target_temp) {
    LV_UNUSED(slot_index);

    int current_centi = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int current_temp = current_centi / 10;

    char temp_buf[32];
    temperature::format_temperature_pair(current_temp, target_temp, temp_buf, sizeof(temp_buf));
    AmsState::instance().set_action_detail(temp_buf);

    if (step_progress_container_) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (step_progress_) {
        ui_step_progress_set_current(step_progress_, 0);
    }

    spdlog::debug("[AmsSidebar] Showing preheat feedback: {}", temp_buf);
}

} // namespace helix::ui

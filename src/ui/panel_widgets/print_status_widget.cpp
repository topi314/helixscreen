// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_widget.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_constants.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "thumbnail_cache.h"
#include "thumbnail_load_context.h"
#include "thumbnail_processor.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <unordered_set>

namespace helix {
void register_print_status_widget() {
    register_widget_factory(
        "print_status", [](const std::string&) { return std::make_unique<PrintStatusWidget>(); });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "print_card_clicked_cb",
                             PrintStatusWidget::print_card_clicked_cb);
    lv_xml_register_event_cb(nullptr, "library_files_cb", PrintStatusWidget::library_files_cb);
    lv_xml_register_event_cb(nullptr, "library_last_cb", PrintStatusWidget::library_last_cb);
    lv_xml_register_event_cb(nullptr, "library_recent_cb", PrintStatusWidget::library_recent_cb);
    lv_xml_register_event_cb(nullptr, "library_queue_cb", PrintStatusWidget::library_queue_cb);
    lv_xml_register_event_cb(nullptr, "print_status_picker_backdrop_cb",
                             PrintStatusWidget::print_status_picker_backdrop_cb);
    lv_xml_register_event_cb(nullptr, "print_status_nozzle_picker_backdrop_cb",
                             PrintStatusWidget::print_status_nozzle_picker_backdrop_cb);
    lv_xml_register_event_cb(nullptr, "print_status_nozzle_chevron_cb",
                             PrintStatusWidget::print_status_nozzle_chevron_cb);
}
} // namespace helix

using namespace helix;

PrintStatusWidget* PrintStatusWidget::s_active_picker_ = nullptr;
PrintStatusWidget* PrintStatusWidget::s_active_nozzle_picker_ = nullptr;

std::unordered_set<PrintStatusWidget*>& PrintStatusWidget::live_instances() {
    static std::unordered_set<PrintStatusWidget*> instances;
    return instances;
}

PrintStatusWidget::PrintStatusWidget() : printer_state_(get_printer_state()) {
    // Register subjects before XML parsing so bind_flag_if_eq / bind_style can find them
    if (!column_mode_subject_initialized_) {
        lv_subject_init_int(&column_mode_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_column_mode", &column_mode_subject_);
        column_mode_subject_initialized_ = true;
        lv_subject_init_int(&colspan_subject_, 2);
        lv_xml_register_subject(nullptr, "print_status_colspan", &colspan_subject_);
        colspan_subject_initialized_ = true;

        lv_subject_init_int(&title_hidden_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_title_hidden", &title_hidden_subject_);
        lv_subject_init_int(&files_hidden_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_files_hidden", &files_hidden_subject_);
        lv_subject_init_int(&last_hidden_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_last_hidden", &last_hidden_subject_);
        lv_subject_init_int(&recent_hidden_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_recent_hidden", &recent_hidden_subject_);
        lv_subject_init_int(&queue_hidden_subject_, 1); // queue starts hidden until jobs arrive
        lv_xml_register_subject(nullptr, "print_status_queue_hidden", &queue_hidden_subject_);
        lv_subject_init_int(&actions_hidden_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_actions_hidden", &actions_hidden_subject_);
        visibility_subjects_initialized_ = true;

        // Detailed-layout subjects
        lv_subject_init_int(&layout_mode_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_layout_mode", &layout_mode_subject_);
        lv_subject_init_int(&layout_effective_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_layout_effective", &layout_effective_subject_);
        lv_subject_init_int(&temp_under_thumb_subject_, 1);
        lv_xml_register_subject(nullptr, "print_status_temp_under_thumb", &temp_under_thumb_subject_);
        lv_subject_init_int(&show_filament_active_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_show_filament_active",
                                &show_filament_active_subject_);
        lv_subject_init_int(&multi_tool_subject_, 0);
        lv_xml_register_subject(nullptr, "print_status_multi_tool", &multi_tool_subject_);
        detailed_subjects_initialized_ = true;

        StaticSubjectRegistry::instance().register_deinit("PrintStatusWidgetSubjects", []() {
            if (detailed_subjects_initialized_ && lv_is_initialized()) {
                lv_subject_deinit(&layout_mode_subject_);
                lv_subject_deinit(&layout_effective_subject_);
                lv_subject_deinit(&temp_under_thumb_subject_);
                lv_subject_deinit(&show_filament_active_subject_);
                lv_subject_deinit(&multi_tool_subject_);
                detailed_subjects_initialized_ = false;
            }
            if (visibility_subjects_initialized_ && lv_is_initialized()) {
                lv_subject_deinit(&title_hidden_subject_);
                lv_subject_deinit(&files_hidden_subject_);
                lv_subject_deinit(&last_hidden_subject_);
                lv_subject_deinit(&recent_hidden_subject_);
                lv_subject_deinit(&queue_hidden_subject_);
                lv_subject_deinit(&actions_hidden_subject_);
                visibility_subjects_initialized_ = false;
            }
            if (colspan_subject_initialized_ && lv_is_initialized()) {
                lv_subject_deinit(&colspan_subject_);
                colspan_subject_initialized_ = false;
            }
            if (column_mode_subject_initialized_ && lv_is_initialized()) {
                lv_subject_deinit(&column_mode_subject_);
                column_mode_subject_initialized_ = false;
            }
        });
    }
}

PrintStatusWidget::~PrintStatusWidget() {
    detach();
}

void PrintStatusWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    using helix::ui::observe_int_sync;
    using helix::ui::observe_print_state;
    using helix::ui::observe_string_immediate;

    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    live_instances().insert(this);

    if (s_formatter_refcount_++ == 0) {
        s_formatter_ = std::make_unique<DetailedFormatter>();
    }

    if (s_formatter_) {
        s_formatter_->set_nozzle_tool_override(nozzle_tool_override_);
    }

    // Store this pointer for event callback recovery
    lv_obj_set_user_data(widget_obj_, this);

    // Cache widget references from XML
    print_card_thumb_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb");
    print_card_active_thumb_ = lv_obj_find_by_name(widget_obj_, "print_card_active_thumb");
    print_card_layout_ = lv_obj_find_by_name(widget_obj_, "print_card_layout");
    print_card_thumb_wrap_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb_wrap");
    print_card_info_ = lv_obj_find_by_name(widget_obj_, "print_card_info");
    print_card_printing_ = lv_obj_find_by_name(widget_obj_, "print_card_printing");
    print_card_preparing_info_ = lv_obj_find_by_name(widget_obj_, "print_card_preparing_info");

    // Library idle state widgets
    print_card_idle_ = lv_obj_find_by_name(widget_obj_, "print_card_idle");
    print_card_idle_compact_ = lv_obj_find_by_name(widget_obj_, "print_card_idle_compact");
    print_card_thumb_compact_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb_compact");
    library_row_last_ = lv_obj_find_by_name(widget_obj_, "library_row_last");
    compact_row_last_ = lv_obj_find_by_name(widget_obj_, "compact_row_last");

    // Hand the detailed-layout arc widget to the formatter (may be nullptr if not in DOM yet)
    if (s_formatter_) {
        lv_obj_t* arc = lv_obj_find_by_name(widget_obj_, "detailed_progress_arc");
        if (arc)
            s_formatter_->attach_arc(arc);
    }

    // Set up observers (after widget references are cached and widget_obj_ is set)
    print_state_observer_ =
        observe_print_state<PrintStatusWidget>(printer_state_.get_print_state_enum_subject(), this,
                                               [](PrintStatusWidget* self, PrintJobState state) {
                                                   if (!self->widget_obj_)
                                                       return;
                                                   self->on_print_state_changed(state);
                                               });

    // Use observe_string_immediate: the thumbnail handler only calls lv_image_set_src
    // (no observer lifecycle changes), and set_print_thumbnail_path is always called
    // from the UI thread via queue_update. Immediate avoids the double-deferral that
    // caused stale reads when the subject changed between notification and handler.
    print_thumbnail_path_observer_ = observe_string_immediate<PrintStatusWidget>(
        printer_state_.get_print_thumbnail_path_subject(), this,
        [](PrintStatusWidget* self, const char* path) {
            if (!self->widget_obj_)
                return;
            self->on_print_thumbnail_path_changed(path);
        });

    auto& fsm = helix::FilamentSensorManager::instance();
    filament_runout_observer_ = observe_int_sync<PrintStatusWidget>(
        fsm.get_any_runout_subject(), this, [](PrintStatusWidget* self, int any_runout) {
            if (!self->widget_obj_)
                return;
            spdlog::debug("[PrintStatusWidget] Filament runout subject changed: {}", any_runout);
            if (any_runout == 1) {
                self->check_and_show_idle_runout_modal();
            } else {
                self->runout_modal_shown_ = false;
            }
        });

    // Observe job queue count to show/hide queue row
    auto* jq_count_subj = lv_xml_get_subject(nullptr, "job_queue_count");
    if (jq_count_subj) {
        job_queue_count_observer_ = helix::ui::observe_int_sync<PrintStatusWidget>(
            jq_count_subj, this, [](PrintStatusWidget* self, int /*count*/) {
                if (!self->widget_obj_)
                    return;
                self->update_job_queue_row_visibility();
            });
    }

    // Register history observer to update idle thumbnail when history loads
    auto token = lifetime_.token();
    history_changed_cb_ = [this, token]() {
        if (token.expired())
            return;
        if (!widget_obj_ || !print_card_thumb_)
            return;
        auto state = static_cast<PrintJobState>(
            lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
        bool is_idle = (state != PrintJobState::PRINTING && state != PrintJobState::PAUSED);
        if (is_idle) {
            reset_print_card_to_idle();
        }
    };
    if (auto* hm = get_print_history_manager()) {
        hm->add_observer(&history_changed_cb_);
        // Trigger history fetch so idle thumbnail shows last print (not benchy)
        if (!hm->is_loaded()) {
            hm->fetch();
        }
    }

    // Observe connection state to fetch history once connected (widget may
    // attach before the WebSocket connection is established)
    connection_observer_ = helix::ui::observe_int_sync<PrintStatusWidget>(
        printer_state_.get_printer_connection_state_subject(), this,
        [](PrintStatusWidget* /*self*/, int state) {
            if (state == static_cast<int>(ConnectionState::CONNECTED)) {
                if (auto* hm = get_print_history_manager(); hm && !hm->is_loaded()) {
                    hm->fetch();
                }
            }
        });

    spdlog::debug("[PrintStatusWidget] Subscribed to print state/progress/time/thumbnail/runout");

    // Check initial print state
    if (print_card_thumb_ && print_card_active_thumb_) {
        auto state = static_cast<PrintJobState>(
            lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
        if (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED) {
            on_print_state_changed(state);
        } else {
            reset_print_card_to_idle();
        }
        spdlog::debug("[PrintStatusWidget] Found print card widgets for dynamic updates");
    } else {
        spdlog::warn("[PrintStatusWidget] Could not find all print card widgets "
                     "(thumb={}, active_thumb={})",
                     print_card_thumb_ != nullptr, print_card_active_thumb_ != nullptr);
    }

    // Apply section visibility from config (drives all print_status_*_hidden subjects)
    apply_visibility_config();

    // Re-run visibility when the breakpoint changes so the 'Print Library' header
    // hides on shrink-to-micro and returns on grow-past-micro.
    if (auto* bp_subj = theme_manager_get_breakpoint_subject()) {
        breakpoint_observer_ = observe_int_sync<PrintStatusWidget>(
            bp_subj, this, [](PrintStatusWidget* self, int /*bp*/) {
                if (self->widget_obj_)
                    self->apply_visibility_config();
            });
    }

    spdlog::debug("[PrintStatusWidget] Attached");
}

void PrintStatusWidget::detach() {
    // Dismiss any open pickers
    dismiss_configure_picker();
    dismiss_nozzle_tool_picker();

    // Invalidate lifetime guard FIRST to abort in-flight async fetches
    lifetime_.invalidate();
    live_instances().erase(this);

    // Unregister history observer
    if (auto* hm = get_print_history_manager()) {
        hm->remove_observer(&history_changed_cb_);
    }

    // Release observers
    print_state_observer_.reset();
    print_thumbnail_path_observer_.reset();
    filament_runout_observer_.reset();
    job_queue_count_observer_.reset();
    connection_observer_.reset();
    breakpoint_observer_.reset();

    // Clear widget references
    print_card_thumb_ = nullptr;
    print_card_active_thumb_ = nullptr;
    print_card_layout_ = nullptr;
    print_card_thumb_wrap_ = nullptr;
    print_card_info_ = nullptr;
    print_card_printing_ = nullptr;
    print_card_preparing_info_ = nullptr;
    print_card_idle_ = nullptr;
    print_card_idle_compact_ = nullptr;
    print_card_thumb_compact_ = nullptr;
    library_row_last_ = nullptr;
    compact_row_last_ = nullptr;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[PrintStatusWidget] Detached");

    if (s_formatter_refcount_ > 0 && --s_formatter_refcount_ == 0) {
        s_formatter_.reset();
    }
}

// ============================================================================
// Size-Dependent Layout
// ============================================================================

void PrintStatusWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                        int /*height_px*/) {
    lv_subject_set_int(&colspan_subject_, colspan);

    // Derive layout_effective: detailed only when user opted in AND colspan >= 2
    int user_pref = (layout_style_ == "detailed") ? 1 : 0;
    lv_subject_set_int(&layout_mode_subject_, user_pref);
    int effective = (user_pref == 1 && colspan >= 2) ? 1 : 0;
    lv_subject_set_int(&layout_effective_subject_, effective);
    lv_subject_set_int(&temp_under_thumb_subject_, (colspan == 2) ? 1 : 0);
    lv_subject_set_int(&show_filament_active_subject_, (colspan >= 3) ? 1 : 0);

    // Compact mode: 1-column — not enough horizontal space for thumbnail + action rows
    bool compact = (colspan <= 1);
    if (compact != is_compact_) {
        is_compact_ = compact;
        update_idle_compact_mode();
    }

    if (!print_card_layout_ || !print_card_thumb_wrap_) {
        return;
    }

    // 2x2: column layout (thumbnail on top, info below)
    // 1x2, 3x2: row layout (thumbnail left, info right)
    bool use_column = (colspan == 2 && rowspan >= 2);
    if (use_column == is_column_) {
        return;
    }
    is_column_ = use_column;

    // Update subject for declarative icon visibility in XML
    lv_subject_set_int(&column_mode_subject_, use_column ? 1 : 0);

    auto apply_info_layout = [use_column](lv_obj_t* info) {
        if (!info)
            return;
        if (use_column) {
            lv_obj_set_width(info, LV_PCT(100));
            lv_obj_set_height(info, LV_SIZE_CONTENT);
            lv_obj_set_style_flex_grow(info, 0, 0);
        } else {
            lv_obj_set_height(info, LV_PCT(100));
            lv_obj_set_width(info, LV_SIZE_CONTENT);
            lv_obj_set_style_flex_grow(info, 1, 0);
        }
    };

    if (use_column) {
        lv_obj_set_flex_flow(print_card_layout_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(print_card_thumb_wrap_, LV_PCT(100));
        lv_obj_set_style_flex_grow(print_card_thumb_wrap_, 1, 0);
    } else {
        lv_obj_set_flex_flow(print_card_layout_, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(print_card_thumb_wrap_, LV_PCT(40));
        lv_obj_set_height(print_card_thumb_wrap_, LV_PCT(100));
        lv_obj_set_style_flex_grow(print_card_thumb_wrap_, 0, 0);
    }
    apply_info_layout(print_card_preparing_info_);
    apply_info_layout(print_card_info_);

    spdlog::debug("[PrintStatusWidget] on_size_changed {}x{} -> {} (compact={})", colspan, rowspan,
                  use_column ? "column" : "row", is_compact_);
}

void PrintStatusWidget::update_idle_compact_mode() {
    // Toggle between full library card and compact card based on widget size
    if (print_card_idle_ && lv_obj_is_valid(print_card_idle_)) {
        if (is_compact_) {
            lv_obj_add_flag(print_card_idle_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(print_card_idle_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (print_card_idle_compact_ && lv_obj_is_valid(print_card_idle_compact_)) {
        if (is_compact_) {
            lv_obj_remove_flag(print_card_idle_compact_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(print_card_idle_compact_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Print Card Click Handler
// ============================================================================

void PrintStatusWidget::handle_print_card_clicked() {
    // Startup grace period: reject phantom clicks during early boot
    auto elapsed = std::chrono::steady_clock::now() - AppConstants::Startup::PROCESS_START_TIME;
    if (elapsed < AppConstants::Startup::PRINT_START_GRACE_PERIOD) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::warn("[PrintStatusWidget] Rejected print card click during startup grace period "
                     "({}s < {}s)",
                     secs, AppConstants::Startup::PRINT_START_GRACE_PERIOD.count());
        return;
    }

    if (!printer_state_.can_start_new_print()) {
        // Print in progress - show print status overlay
        spdlog::info(
            "[PrintStatusWidget] Print card clicked - showing print status (print in progress)");

        if (!PrintStatusPanel::push_overlay(parent_screen_)) {
            spdlog::error("[PrintStatusWidget] Failed to push print status overlay");
        }
    } else {
        // No print in progress - navigate to print select panel (same as "Print Files")
        handle_library_files();
    }
}

// ============================================================================
// Library Action Handlers
// ============================================================================

void PrintStatusWidget::handle_library_files() {
    spdlog::info("[PrintStatusWidget] Library: Print Files");
    NavigationManager::instance().set_active(PanelId::PrintSelect);
}

void PrintStatusWidget::handle_library_last() {
    if (!last_print_available_) {
        return;
    }

    auto* history = get_print_history_manager();
    if (!history || !history->is_loaded()) {
        spdlog::info("[PrintStatusWidget] Library: Print Last - no history available");
        return;
    }

    const auto& jobs = history->get_jobs();
    if (jobs.empty()) {
        spdlog::info("[PrintStatusWidget] Library: Print Last - no jobs in history");
        return;
    }

    // Find most recent job where the file still exists
    const PrintHistoryJob* last_job = nullptr;
    for (const auto& job : jobs) {
        if (job.exists) {
            last_job = &job;
            break;
        }
    }

    if (!last_job) {
        spdlog::info("[PrintStatusWidget] Library: Print Last - no files exist on disk");
        return;
    }

    spdlog::info("[PrintStatusWidget] Library: Print Last -> {}", last_job->filename);

    // Navigate to PrintSelectPanel, select the file, and return to home on back
    NavigationManager::instance().set_active(PanelId::PrintSelect);

    auto* panel = get_print_select_panel(printer_state_, get_moonraker_api());
    if (panel) {
        panel->set_return_to_home_on_close();
        if (!panel->select_file_by_name(last_job->filename)) {
            panel->set_pending_file_selection(last_job->filename);
        }
    }
}

void PrintStatusWidget::handle_library_recent() {
    spdlog::info("[PrintStatusWidget] Library: Recent");

    NavigationManager::instance().set_active(PanelId::PrintSelect);

    auto* panel = get_print_select_panel(printer_state_, get_moonraker_api());
    if (panel) {
        panel->set_sort_recent();
    }
}

void PrintStatusWidget::handle_library_queue() {
    spdlog::info("[PrintStatusWidget] Library: Job Queue");
    if (parent_screen_) {
        job_queue_modal_.show(parent_screen_);
    }
}

void PrintStatusWidget::update_job_queue_row_visibility() {
    // Queue row: visible when config allows AND there are jobs in queue.
    // Purely subject-driven — XML binds hidden flag to print_status_queue_hidden.
    bool has_jobs = false;
    auto* jq_count_subj = lv_xml_get_subject(nullptr, "job_queue_count");
    if (jq_count_subj) {
        has_jobs = lv_subject_get_int(jq_count_subj) > 0;
    }
    bool queue_visible = show_job_queue_ && has_jobs;
    lv_subject_set_int(&queue_hidden_subject_, queue_visible ? 0 : 1);

    // The library_actions container hides when no action row is visible — re-evaluate
    // here since queue visibility contributes to that combined state.
    recompute_actions_visibility();
}

// ============================================================================
// Observer Callbacks
// ============================================================================

void PrintStatusWidget::on_print_state_changed(PrintJobState state) {
    if (!widget_obj_ || !print_card_thumb_) {
        return;
    }
    if (!lv_obj_is_valid(widget_obj_)) {
        return;
    }

    bool is_active = (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED);

    // Hide both idle cards when printing, show the right one when idle
    if (is_active) {
        if (print_card_idle_ && lv_obj_is_valid(print_card_idle_)) {
            lv_obj_add_flag(print_card_idle_, LV_OBJ_FLAG_HIDDEN);
        }
        if (print_card_idle_compact_ && lv_obj_is_valid(print_card_idle_compact_)) {
            lv_obj_add_flag(print_card_idle_compact_, LV_OBJ_FLAG_HIDDEN);
        }
        // Show the active container (preparing/printing info visibility handled by XML bindings)
        if (print_card_printing_ && lv_obj_is_valid(print_card_printing_)) {
            lv_obj_remove_flag(print_card_printing_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        update_idle_compact_mode();
        // Hide the active container
        if (print_card_printing_ && lv_obj_is_valid(print_card_printing_)) {
            lv_obj_add_flag(print_card_printing_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (is_active) {
        spdlog::debug("[PrintStatusWidget] Print active - state updated via subject bindings");
    } else {
        spdlog::debug("[PrintStatusWidget] Print not active - reverting card to idle state");
        reset_print_card_to_idle();
    }
}

void PrintStatusWidget::on_print_thumbnail_path_changed(const char* path) {
    if (!widget_obj_ || !print_card_active_thumb_) {
        return;
    }

    if (path && path[0] != '\0') {
        lv_image_set_src(print_card_active_thumb_, path);
        spdlog::info("[PrintStatusWidget] Active print thumbnail updated: {}", path);
    } else {
        lv_image_set_src(print_card_active_thumb_, "A:assets/images/benchy_thumbnail_white.png");
        spdlog::debug("[PrintStatusWidget] Active print thumbnail cleared (empty path)");
    }
}

std::string PrintStatusWidget::get_last_print_thumbnail_path() const {
    auto* history = get_print_history_manager();
    if (!history || !history->is_loaded()) {
        return {};
    }

    const auto& jobs = history->get_jobs();
    if (jobs.empty()) {
        return {};
    }

    const auto& job = jobs.front();

    // Select the best thumbnail for the widget's actual rendered size
    if (!job.thumbnails.empty() && print_card_thumb_ && lv_obj_is_valid(print_card_thumb_)) {
        int target_w = lv_obj_get_width(print_card_thumb_);
        int target_h = lv_obj_get_height(print_card_thumb_);

        // Find smallest thumbnail that meets or exceeds the widget dimensions
        const ThumbnailInfo* best_adequate = nullptr;
        const ThumbnailInfo* largest = &job.thumbnails[0];

        for (const auto& t : job.thumbnails) {
            if (t.pixel_count() > largest->pixel_count()) {
                largest = &t;
            }
            if (t.width >= target_w && t.height >= target_h) {
                if (!best_adequate || t.pixel_count() < best_adequate->pixel_count()) {
                    best_adequate = &t;
                }
            }
        }

        const auto* best = best_adequate ? best_adequate : largest;
        spdlog::debug("[PrintStatusWidget] Widget {}x{}, selected thumbnail {}x{} ({})", target_w,
                      target_h, best->width, best->height, best->relative_path);
        return best->relative_path;
    }

    // Fallback: use pre-selected largest thumbnail
    return job.thumbnail_path;
}

void PrintStatusWidget::reset_print_card_to_idle() {
    // Update "Print Last" row availability
    update_last_print_availability();

    if (!print_card_thumb_ || !lv_obj_is_valid(print_card_thumb_)) {
        return;
    }

    // Also update compact thumbnail
    auto set_thumb_on_widgets = [this](const char* src) {
        if (print_card_thumb_ && lv_obj_is_valid(print_card_thumb_)) {
            lv_image_set_src(print_card_thumb_, src);
        }
        if (print_card_thumb_compact_ && lv_obj_is_valid(print_card_thumb_compact_)) {
            lv_image_set_src(print_card_thumb_compact_, src);
        }
    };

    // Try to show the last printed file's thumbnail instead of benchy
    std::string thumb_rel_path = get_last_print_thumbnail_path();
    if (thumb_rel_path.empty()) {
        set_thumb_on_widgets("A:assets/images/benchy_thumbnail_white.png");
        spdlog::debug("[PrintStatusWidget] Idle thumbnail: benchy (no history)");
        return;
    }

    // Compute pre-scale target from actual widget size (not hardcoded breakpoints)
    int widget_w = lv_obj_get_width(print_card_thumb_);
    int widget_h = lv_obj_get_height(print_card_thumb_);
    auto target = helix::ThumbnailProcessor::get_target_for_resolution(
        widget_w, widget_h, helix::ThumbnailSize::Detail);

    // Check if we already have a pre-scaled BIN version
    auto cached = get_thumbnail_cache().get_if_optimized(thumb_rel_path, target);
    if (!cached.empty()) {
        set_thumb_on_widgets(cached.c_str());
        spdlog::debug("[PrintStatusWidget] Idle thumbnail from cache: {}", cached);
        return;
    }

    // Set benchy as placeholder while we fetch
    set_thumb_on_widgets("A:assets/images/benchy_thumbnail_white.png");

    // Fetch async from Moonraker
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::debug("[PrintStatusWidget] Idle thumbnail: benchy (no API)");
        return;
    }

    // Use lifetime token to prevent use-after-free if widget is destroyed during fetch
    lv_obj_t* thumb_widget = print_card_thumb_;
    lv_obj_t* thumb_compact = print_card_thumb_compact_;
    auto token = lifetime_.token();

    get_thumbnail_cache().fetch_optimized(
        api, thumb_rel_path, target,
        [thumb_widget, thumb_compact, token](const std::string& lvgl_path) {
            if (token.expired())
                return;
            helix::ui::queue_update<std::string>(
                std::make_unique<std::string>(lvgl_path),
                [thumb_widget, thumb_compact](std::string* path) {
                    if (lv_obj_is_valid(thumb_widget)) {
                        lv_image_set_src(thumb_widget, path->c_str());
                    }
                    if (thumb_compact && lv_obj_is_valid(thumb_compact)) {
                        lv_image_set_src(thumb_compact, path->c_str());
                    }
                    spdlog::info("[PrintStatusWidget] Idle thumbnail loaded: {}", *path);
                });
        },
        [](const std::string& error) {
            spdlog::debug("[PrintStatusWidget] Idle thumbnail fetch failed: {}", error);
        });
}

void PrintStatusWidget::update_last_print_availability() {
    auto* history = get_print_history_manager();
    last_print_available_ = false;

    if (history && history->is_loaded()) {
        const auto& jobs = history->get_jobs();
        for (const auto& job : jobs) {
            if (job.exists) {
                last_print_available_ = true;
                break;
            }
        }
    }

    // Apply to both full and compact "Print Last" rows
    lv_obj_t* rows[] = {library_row_last_, compact_row_last_};
    for (auto* row : rows) {
        if (!row || !lv_obj_is_valid(row))
            continue;
        if (last_print_available_) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_100, 0);
        } else {
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_40, 0);
        }
    }
}

// ============================================================================
// Filament Runout Modal
// ============================================================================

void PrintStatusWidget::check_and_show_idle_runout_modal() {
    // Grace period - don't show modal during startup
    auto& fsm = helix::FilamentSensorManager::instance();
    if (fsm.is_in_startup_grace_period()) {
        spdlog::debug("[PrintStatusWidget] In startup grace period - skipping runout modal");
        return;
    }

    // Verify actual sensor state
    if (!fsm.has_any_runout()) {
        spdlog::debug("[PrintStatusWidget] No actual runout detected - skipping modal");
        return;
    }

    // Check suppression logic (AMS without bypass, wizard active, etc.)
    if (!get_runtime_config()->should_show_runout_modal()) {
        spdlog::debug("[PrintStatusWidget] Runout modal suppressed by runtime config");
        return;
    }

    // Only show modal if not already shown
    if (runout_modal_shown_) {
        spdlog::debug("[PrintStatusWidget] Runout modal already shown - skipping");
        return;
    }

    // Only show if printer is idle (not printing/paused)
    int print_state = lv_subject_get_int(printer_state_.get_print_state_enum_subject());
    if (print_state != static_cast<int>(PrintJobState::STANDBY) &&
        print_state != static_cast<int>(PrintJobState::COMPLETE) &&
        print_state != static_cast<int>(PrintJobState::CANCELLED)) {
        spdlog::debug("[PrintStatusWidget] Print active (state={}) - skipping idle runout modal",
                      print_state);
        return;
    }

    spdlog::info("[PrintStatusWidget] Showing idle runout modal");
    show_idle_runout_modal();
    runout_modal_shown_ = true;
}

void PrintStatusWidget::trigger_idle_runout_check() {
    spdlog::debug("[PrintStatusWidget] Triggering deferred runout check");
    runout_modal_shown_ = false;
    check_and_show_idle_runout_modal();
}

void PrintStatusWidget::show_idle_runout_modal() {
    if (runout_modal_.is_visible()) {
        return;
    }

    runout_modal_.set_on_load_filament([this]() {
        spdlog::info("[PrintStatusWidget] User chose to load filament (idle)");
        NavigationManager::instance().set_active(PanelId::Filament);
    });

    runout_modal_.set_on_resume([]() {
        // Resume not applicable when idle
    });

    runout_modal_.set_on_cancel_print([]() {
        // Cancel not applicable when idle
    });

    runout_modal_.show(parent_screen_);
}

// ============================================================================
// Configuration
// ============================================================================

void PrintStatusWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    if (config.contains("layout_style") && config["layout_style"].is_string()) {
        std::string ls = config["layout_style"].get<std::string>();
        if (ls == "library" || ls == "detailed") {
            layout_style_ = std::move(ls);
        }
    }
    if (config.contains("nozzle_tool_override") && config["nozzle_tool_override"].is_string()) {
        nozzle_tool_override_ = config["nozzle_tool_override"].get<std::string>();
    }
    if (config.contains("show_title") && config["show_title"].is_boolean()) {
        show_title_ = config["show_title"].get<bool>();
    }
    if (config.contains("show_print_files") && config["show_print_files"].is_boolean()) {
        show_print_files_ = config["show_print_files"].get<bool>();
    }
    if (config.contains("show_reprint_last") && config["show_reprint_last"].is_boolean()) {
        show_reprint_last_ = config["show_reprint_last"].get<bool>();
    }
    if (config.contains("show_recent_prints") && config["show_recent_prints"].is_boolean()) {
        show_recent_prints_ = config["show_recent_prints"].get<bool>();
    }
    if (config.contains("show_job_queue") && config["show_job_queue"].is_boolean()) {
        show_job_queue_ = config["show_job_queue"].get<bool>();
    }
    if (s_formatter_) {
        s_formatter_->set_nozzle_tool_override(nozzle_tool_override_);
    }
}

bool PrintStatusWidget::on_edit_configure() {
    spdlog::info("[PrintStatusWidget] Configure requested - showing section picker");
    show_configure_picker();
    return false; // picker handles save internally, no rebuild needed
}

void PrintStatusWidget::apply_visibility_config() {
    // Per-element hidden flags are driven entirely by subjects; XML binds hidden
    // to print_status_*_hidden via bind_flag_if_eq ref_value="1". Here we just
    // compute each value from config + live breakpoint and push into subjects.
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    bool at_micro = bp_subj && as_breakpoint(lv_subject_get_int(bp_subj)) == UiBreakpoint::Micro;

    lv_subject_set_int(&title_hidden_subject_, (!show_title_ || at_micro) ? 1 : 0);
    lv_subject_set_int(&files_hidden_subject_, show_print_files_ ? 0 : 1);
    lv_subject_set_int(&last_hidden_subject_, show_reprint_last_ ? 0 : 1);
    lv_subject_set_int(&recent_hidden_subject_, show_recent_prints_ ? 0 : 1);

    // Queue row uses a live count — defer to the dedicated helper, which also
    // triggers recompute_actions_visibility() since queue state affects it.
    update_job_queue_row_visibility();
}

void PrintStatusWidget::recompute_actions_visibility() {
    // The action-list container hides when no individual row is visible. That
    // also forces the thumbnail to grow to 100% width and re-centers the body.
    // Width/alignment changes stay imperative (not simple flag toggles); the
    // hidden flag itself rides the actions_hidden subject.
    bool queue_visible = lv_subject_get_int(&queue_hidden_subject_) == 0;
    bool any_button_visible =
        show_print_files_ || show_reprint_last_ || show_recent_prints_ || queue_visible;

    lv_subject_set_int(&actions_hidden_subject_, any_button_visible ? 0 : 1);

    if (!widget_obj_ || !print_card_thumb_)
        return;
    lv_obj_t* library_body = lv_obj_find_by_name(widget_obj_, "library_body");
    if (any_button_visible) {
        lv_obj_set_width(print_card_thumb_, LV_PCT(40));
        if (library_body) {
            lv_obj_set_style_flex_main_place(library_body, LV_FLEX_ALIGN_START, 0);
            lv_obj_set_style_flex_cross_place(library_body, LV_FLEX_ALIGN_START, 0);
        }
    } else {
        lv_obj_set_width(print_card_thumb_, LV_PCT(100));
        if (library_body) {
            lv_obj_set_style_flex_main_place(library_body, LV_FLEX_ALIGN_CENTER, 0);
            lv_obj_set_style_flex_cross_place(library_body, LV_FLEX_ALIGN_CENTER, 0);
        }
    }
}

void PrintStatusWidget::show_configure_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    picker_backdrop_ = static_cast<lv_obj_t*>(
        lv_xml_create(parent_screen_, "print_status_configure_picker", nullptr));
    if (!picker_backdrop_) {
        spdlog::error("[PrintStatusWidget] Failed to create configure picker from XML");
        return;
    }

    lv_obj_t* option_list = lv_obj_find_by_name(picker_backdrop_, "option_list");
    if (!option_list) {
        spdlog::error("[PrintStatusWidget] option_list not found in picker XML");
        helix::ui::safe_delete_deferred(picker_backdrop_);
        return;
    }

    // Helper to resolve space tokens
    auto resolve_space = [](const char* name, int fallback) -> int {
        const char* s = lv_xml_get_const(nullptr, name);
        return s ? std::atoi(s) : fallback;
    };
    int space_sm = resolve_space("space_sm", 6);
    int space_xs = resolve_space("space_xs", 4);
    int space_md = resolve_space("space_md", 10);

    // Create checkbox rows for each toggle option
    struct Option {
        const char* name; // lv_obj name for lookup in apply_picker_state()
        const char* label;
        bool checked;
    };
    Option options[] = {
        {"opt_title", "Title", show_title_},
        {"opt_print_files", "Print Files", show_print_files_},
        {"opt_reprint_last", "Reprint Last", show_reprint_last_},
        {"opt_recent_prints", "Recent Prints", show_recent_prints_},
        {"opt_job_queue", "Job Queue", show_job_queue_},
    };

    for (const auto& opt : options) {
        lv_obj_t* row = lv_obj_create(option_list);
        lv_obj_set_name(row, opt.name);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* cb = lv_checkbox_create(row);
        lv_checkbox_set_text(cb, "");
        lv_obj_set_style_pad_all(cb, 0, 0);
        if (opt.checked) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, opt.label);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);

        // Click row to toggle checkbox
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                uint32_t count = lv_obj_get_child_count(target);
                for (uint32_t i = 0; i < count; ++i) {
                    lv_obj_t* child = lv_obj_get_child(target, static_cast<int32_t>(i));
                    if (lv_obj_check_type(child, &lv_checkbox_class)) {
                        if (lv_obj_has_state(child, LV_STATE_CHECKED)) {
                            lv_obj_remove_state(child, LV_STATE_CHECKED);
                        } else {
                            lv_obj_add_state(child, LV_STATE_CHECKED);
                        }
                        break;
                    }
                }

                // Apply immediately
                if (s_active_picker_ && s_active_picker_->picker_backdrop_) {
                    s_active_picker_->apply_picker_state();
                }
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_active_picker_ = this;

    // Self-clearing delete callback
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<PrintStatusWidget*>(lv_event_get_user_data(e));
            if (self) {
                self->picker_backdrop_ = nullptr;
                if (s_active_picker_ == self) {
                    s_active_picker_ = nullptr;
                }
            }
        },
        LV_EVENT_DELETE, this);

    // Position card near widget
    lv_obj_t* card = lv_obj_find_by_name(picker_backdrop_, "context_menu");
    if (card && widget_obj_) {
        int screen_w = lv_obj_get_width(parent_screen_);
        int screen_h = lv_obj_get_height(parent_screen_);
        int card_w = std::clamp(screen_w * 3 / 10, 160, 240);

        lv_obj_set_width(card, card_w);
        lv_obj_set_style_max_height(card, screen_h * 80 / 100, 0);
        lv_obj_update_layout(card);
        int card_h = lv_obj_get_height(card);

        lv_area_t widget_area;
        lv_obj_get_coords(widget_obj_, &widget_area);

        int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
        int card_y = widget_area.y2 + space_xs;

        if (card_x < space_md)
            card_x = space_md;
        if (card_x + card_w > screen_w - space_md)
            card_x = screen_w - card_w - space_md;
        if (card_y + card_h > screen_h - space_md) {
            card_y = widget_area.y1 - card_h - space_xs;
            if (card_y < space_md)
                card_y = space_md;
        }

        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[PrintStatusWidget] Configure picker shown");
}

void PrintStatusWidget::apply_picker_state() {
    if (!picker_backdrop_)
        return;

    lv_obj_t* option_list = lv_obj_find_by_name(picker_backdrop_, "option_list");
    if (!option_list)
        return;

    // Read checkbox states by named rows (not positional index)
    auto read_checkbox = [&](const char* row_name) -> bool {
        lv_obj_t* row = lv_obj_find_by_name(option_list, row_name);
        if (!row)
            return true;
        uint32_t row_count = lv_obj_get_child_count(row);
        for (uint32_t j = 0; j < row_count; ++j) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(j));
            if (lv_obj_check_type(child, &lv_checkbox_class)) {
                return lv_obj_has_state(child, LV_STATE_CHECKED);
            }
        }
        return true;
    };

    show_title_ = read_checkbox("opt_title");
    show_print_files_ = read_checkbox("opt_print_files");
    show_reprint_last_ = read_checkbox("opt_reprint_last");
    show_recent_prints_ = read_checkbox("opt_recent_prints");
    show_job_queue_ = read_checkbox("opt_job_queue");

    // Persist
    nlohmann::json new_config = config_;
    new_config["show_title"] = show_title_;
    new_config["show_print_files"] = show_print_files_;
    new_config["show_reprint_last"] = show_reprint_last_;
    new_config["show_recent_prints"] = show_recent_prints_;
    new_config["show_job_queue"] = show_job_queue_;
    config_ = new_config;
    save_widget_config(new_config);

    // Apply visibility immediately
    apply_visibility_config();

    spdlog::info("[PrintStatusWidget] Config updated: title={}, print_files={}, reprint_last={}, "
                 "recent_prints={}, job_queue={}",
                 show_title_, show_print_files_, show_reprint_last_, show_recent_prints_,
                 show_job_queue_);
}

void PrintStatusWidget::dismiss_configure_picker() {
    if (!picker_backdrop_) {
        return;
    }

    s_active_picker_ = nullptr;
    helix::ui::safe_delete_deferred(picker_backdrop_);

    spdlog::debug("[PrintStatusWidget] Configure picker dismissed");
}

void PrintStatusWidget::print_status_picker_backdrop_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_status_picker_backdrop_cb");
    (void)e;
    if (s_active_picker_) {
        s_active_picker_->apply_picker_state();
        s_active_picker_->dismiss_configure_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Nozzle Tool Picker
// ============================================================================

void PrintStatusWidget::show_nozzle_tool_picker(lv_obj_t* anchor) {
    if (nozzle_picker_backdrop_ || !parent_screen_)
        return;

    nozzle_picker_backdrop_ = static_cast<lv_obj_t*>(
        lv_xml_create(parent_screen_, "print_status_nozzle_tool_picker", nullptr));
    if (!nozzle_picker_backdrop_) {
        spdlog::error("[PrintStatusWidget] Failed to create nozzle tool picker");
        return;
    }

    lv_obj_t* option_list = lv_obj_find_by_name(nozzle_picker_backdrop_, "option_list");
    if (!option_list) {
        spdlog::error("[PrintStatusWidget] option_list not found in nozzle picker XML");
        helix::ui::safe_delete_deferred(nozzle_picker_backdrop_);
        nozzle_picker_backdrop_ = nullptr;
        return;
    }

    auto add_row = [option_list](const char* label, const std::string& tool_key) {
        const char* attrs[] = {"width",   "100%",            "height", "#button_height_sm",
                               "variant", "ghost",            "text",   label,
                               nullptr};
        lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(option_list, "ui_button", attrs));
        if (!row)
            return;
        auto* key_holder = new std::string(tool_key);
        lv_obj_set_user_data(row, key_holder);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("nozzle_picker_row_cb");
                auto* btn = lv_event_get_current_target_obj(e);
                auto* holder = static_cast<std::string*>(lv_obj_get_user_data(btn));
                if (!s_active_nozzle_picker_ || !holder)
                    return;
                auto* self = s_active_nozzle_picker_;
                self->nozzle_tool_override_ = *holder;
                self->config_["nozzle_tool_override"] = *holder;
                if (s_formatter_)
                    s_formatter_->set_nozzle_tool_override(*holder);
                self->dismiss_nozzle_tool_picker();
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("nozzle_picker_row_delete_cb");
                auto* btn = lv_event_get_current_target_obj(e);
                auto* holder = static_cast<std::string*>(lv_obj_get_user_data(btn));
                delete holder;
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    };

    add_row("Follow active tool", "auto");
    int count = ToolState::instance().tool_count();
    for (int i = 0; i < count; ++i) {
        std::string name = (i == 0) ? "extruder" : ("extruder" + std::to_string(i));
        char label[16];
        snprintf(label, sizeof(label), "T%d", i);
        add_row(label, name);
    }

    s_active_nozzle_picker_ = this;

    lv_obj_add_event_cb(
        nozzle_picker_backdrop_,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("nozzle_picker_backdrop_delete_cb");
            if (s_active_nozzle_picker_ &&
                s_active_nozzle_picker_->nozzle_picker_backdrop_ ==
                    lv_event_get_current_target_obj(e)) {
                s_active_nozzle_picker_->nozzle_picker_backdrop_ = nullptr;
                s_active_nozzle_picker_ = nullptr;
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_DELETE, nullptr);

    // Position context menu near the anchor with overflow clamp
    lv_obj_t* card = lv_obj_find_by_name(nozzle_picker_backdrop_, "context_menu");
    if (card && anchor) {
        lv_obj_update_layout(anchor);
        lv_obj_update_layout(card);
        lv_area_t a;
        lv_obj_get_coords(anchor, &a);
        int screen_h = lv_obj_get_height(parent_screen_);
        int screen_w = lv_obj_get_width(parent_screen_);
        int card_h = lv_obj_get_height(card);
        int card_w = lv_obj_get_width(card);
        int card_y = a.y2 + 4;
        if (card_y + card_h > screen_h - 8) {
            // Flip above the anchor
            card_y = a.y1 - card_h - 4;
            if (card_y < 8) card_y = 8;
        }
        int card_x = a.x1;
        if (card_x + card_w > screen_w - 8) {
            card_x = screen_w - card_w - 8;
        }
        if (card_x < 8) card_x = 8;
        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[PrintStatusWidget] Nozzle tool picker shown");
}

void PrintStatusWidget::dismiss_nozzle_tool_picker() {
    if (!nozzle_picker_backdrop_)
        return;
    s_active_nozzle_picker_ = nullptr;
    helix::ui::safe_delete_deferred(nozzle_picker_backdrop_);
    nozzle_picker_backdrop_ = nullptr;
    spdlog::debug("[PrintStatusWidget] Nozzle tool picker dismissed");
}

void PrintStatusWidget::print_status_nozzle_picker_backdrop_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_status_nozzle_picker_backdrop_cb");
    if (s_active_nozzle_picker_)
        s_active_nozzle_picker_->dismiss_nozzle_tool_picker();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Static Trampolines
// ============================================================================

static PrintStatusWidget* recover_widget_from_event(lv_event_t* e) {
    // Walk up from the clicked element to find the widget root with user_data
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* obj = target;
    while (obj) {
        auto* self = static_cast<PrintStatusWidget*>(lv_obj_get_user_data(obj));
        if (self) {
            // Validate widget is still alive — non-matching pointers may be
            // other user_data types (e.g., UiButtonData on ui_button children),
            // so keep walking up rather than treating as stale
            if (PrintStatusWidget::live_instances().count(self) != 0) {
                return self;
            }
        }
        obj = lv_obj_get_parent(obj);
    }
    return nullptr;
}

void PrintStatusWidget::print_card_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_card_clicked_cb");

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->record_interaction();
        self->handle_print_card_clicked();
    } else {
        spdlog::warn(
            "[PrintStatusWidget] print_card_clicked_cb: could not recover widget instance");
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_files_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_files_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_files();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_last_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_last_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_last();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_recent_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_recent_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_recent();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_queue_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_queue_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_queue();
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// DetailedFormatter — first-instance singleton lifecycle
// ============================================================================

namespace {
// Centidegrees → rounded °C (positive values; negatives round toward zero, fine for temps)
int cd_to_c(int cd) {
    if (cd >= 0) return (cd + 50) / 100;
    return (cd - 50) / 100;
}
} // namespace

void PrintStatusWidget::DetailedFormatter::update_progress_pct() {
    int pct = lv_subject_get_int(get_printer_state().get_print_progress_subject());
    snprintf(progress_pct_buf_, sizeof(progress_pct_buf_), "%d%%", pct);
    lv_subject_copy_string(&progress_pct_subject_, progress_pct_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_layer_text() {
    auto& ps = get_printer_state();
    int cur = lv_subject_get_int(ps.get_print_layer_current_subject());
    int tot = lv_subject_get_int(ps.get_print_layer_total_subject());
    if (tot <= 0) {
        snprintf(layer_text_buf_, sizeof(layer_text_buf_), "Layer %d", cur);
    } else {
        snprintf(layer_text_buf_, sizeof(layer_text_buf_), "Layer %d / %d", cur, tot);
    }
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_time_text() {
    auto& ps = get_printer_state();
    int elapsed = lv_subject_get_int(ps.get_print_elapsed_subject());
    int remain = lv_subject_get_int(ps.get_print_time_left_subject());
    int total = elapsed + remain;
    snprintf(time_text_buf_, sizeof(time_text_buf_), "%dh %02dm / %dh %02dm", elapsed / 3600,
             (elapsed % 3600) / 60, total / 3600, (total % 3600) / 60);
    lv_subject_copy_string(&time_text_subject_, time_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_filament_text() {
    int used_mm = lv_subject_get_int(get_printer_state().get_print_filament_used_subject());
    if (used_mm <= 0) {
        filament_text_buf_[0] = '\0';
    } else {
        snprintf(filament_text_buf_, sizeof(filament_text_buf_), "%.1fm", used_mm / 1000.0);
    }
    lv_subject_copy_string(&filament_text_subject_, filament_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_nozzle_text() {
    auto& ps = get_printer_state();
    lv_subject_t* temp_sub;
    lv_subject_t* tgt_sub;
    if (current_nozzle_override_ == "auto") {
        temp_sub = ps.get_active_extruder_temp_subject();
        tgt_sub  = ps.get_active_extruder_target_subject();
    } else {
        // Read-only access — prefer the no-lifetime overload per [L084] note
        temp_sub = ps.get_extruder_temp_subject(current_nozzle_override_);
        tgt_sub  = ps.get_extruder_target_subject(current_nozzle_override_);
    }
    int t  = cd_to_c(temp_sub ? lv_subject_get_int(temp_sub) : 0);
    int tg = cd_to_c(tgt_sub  ? lv_subject_get_int(tgt_sub)  : 0);
    snprintf(nozzle_text_buf_, sizeof(nozzle_text_buf_), "%d / %d°C", t, tg);
    lv_subject_copy_string(&nozzle_text_subject_, nozzle_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::set_nozzle_tool_override(
    const std::string& override_name) {
    using helix::ui::observe_int_sync;
    auto& ps = get_printer_state();

    // [L084] Clear lifetimes BEFORE observers to expire weak_ptr first.
    nozzle_temp_lifetime_.reset();
    nozzle_target_lifetime_.reset();
    // [L085] reset(), never release()
    nozzle_temp_observer_.reset();
    nozzle_target_observer_.reset();

    auto bind_auto = [&]() {
        current_nozzle_override_ = "auto";
        nozzle_temp_observer_ = observe_int_sync<DetailedFormatter>(
            ps.get_active_extruder_temp_subject(), this,
            [](DetailedFormatter* self, int) { self->update_nozzle_text(); });
        nozzle_target_observer_ = observe_int_sync<DetailedFormatter>(
            ps.get_active_extruder_target_subject(), this,
            [](DetailedFormatter* self, int) { self->update_nozzle_text(); });
        update_nozzle_text();
    };

    if (override_name.empty() || override_name == "auto") {
        bind_auto();
        return;
    }

    // Pinned: resolve dynamic per-tool subjects
    auto* temp_sub = ps.get_extruder_temp_subject(override_name, nozzle_temp_lifetime_);
    auto* tgt_sub  = ps.get_extruder_target_subject(override_name, nozzle_target_lifetime_);
    if (!temp_sub || !tgt_sub) {
        spdlog::info("[DetailedFormatter] nozzle override '{}' not found, falling back to auto",
                     override_name);
        // Clear any half-bound lifetimes before falling back
        nozzle_temp_lifetime_.reset();
        nozzle_target_lifetime_.reset();
        bind_auto();
        return;
    }

    current_nozzle_override_ = override_name;
    nozzle_temp_observer_ = observe_int_sync<DetailedFormatter>(
        temp_sub, this,
        [](DetailedFormatter* self, int) { self->update_nozzle_text(); });
    nozzle_target_observer_ = observe_int_sync<DetailedFormatter>(
        tgt_sub, this,
        [](DetailedFormatter* self, int) { self->update_nozzle_text(); });
    update_nozzle_text();
}

void PrintStatusWidget::DetailedFormatter::update_bed_text() {
    auto& ps = get_printer_state();
    int t = cd_to_c(lv_subject_get_int(ps.get_bed_temp_subject()));
    int tg = cd_to_c(lv_subject_get_int(ps.get_bed_target_subject()));
    snprintf(bed_text_buf_, sizeof(bed_text_buf_), "%d / %d°C", t, tg);
    lv_subject_copy_string(&bed_text_subject_, bed_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_chamber_text() {
    auto& ps = get_printer_state();
    int t = cd_to_c(lv_subject_get_int(ps.get_chamber_temp_subject()));
    int tg = cd_to_c(lv_subject_get_int(ps.get_chamber_target_subject()));
    if (tg <= 0) {
        snprintf(chamber_text_buf_, sizeof(chamber_text_buf_), "%d°C", t);
    } else {
        snprintf(chamber_text_buf_, sizeof(chamber_text_buf_), "%d / %d°C", t, tg);
    }
    lv_subject_copy_string(&chamber_text_subject_, chamber_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_multi_tool() {
    int count = lv_subject_get_int(ToolState::instance().get_tool_count_subject());
    lv_subject_set_int(&PrintStatusWidget::multi_tool_subject_, count > 1 ? 1 : 0);
}

void PrintStatusWidget::DetailedFormatter::update_tool_label() {
    int count = lv_subject_get_int(ToolState::instance().get_tool_count_subject());
    if (count <= 1) {
        nozzle_tool_label_buf_[0] = '\0';
    } else {
        int idx = ToolState::instance().active_tool_index();
        snprintf(nozzle_tool_label_buf_, sizeof(nozzle_tool_label_buf_), "T%d", idx);
    }
    lv_subject_copy_string(&nozzle_tool_label_subject_, nozzle_tool_label_buf_);
}

PrintStatusWidget::DetailedFormatter::DetailedFormatter() {
    UI_MANAGED_SUBJECT_STRING(progress_pct_subject_, progress_pct_buf_, "0%",
                              "print_status_progress_pct", subjects_);
    UI_MANAGED_SUBJECT_STRING(layer_text_subject_, layer_text_buf_, "",
                              "print_status_layer_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(time_text_subject_, time_text_buf_, "0h 00m / 0h 00m",
                              "print_status_time_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(filament_text_subject_, filament_text_buf_, "",
                              "print_status_filament_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_text_subject_, nozzle_text_buf_, "0 / 0°C",
                              "print_status_nozzle_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_text_subject_, bed_text_buf_, "0 / 0°C",
                              "print_status_bed_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(chamber_text_subject_, chamber_text_buf_, "0°C",
                              "print_status_chamber_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_tool_label_subject_, nozzle_tool_label_buf_, "",
                              "print_status_nozzle_tool_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(idle_filename_subject_, idle_filename_buf_, "",
                              "print_status_idle_filename", subjects_);
    UI_MANAGED_SUBJECT_STRING(idle_when_subject_, idle_when_buf_, "Never printed",
                              "print_status_idle_when", subjects_);
    UI_MANAGED_SUBJECT_STRING(idle_meta_subject_, idle_meta_buf_, "",
                              "print_status_idle_meta", subjects_);
    UI_MANAGED_SUBJECT_INT(idle_has_last_subject_, 0,
                           "print_status_idle_has_last", subjects_);

    using helix::ui::observe_int_sync;
    auto& ps = get_printer_state();
    progress_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_progress_subject(), this,
        [](DetailedFormatter* self, int) { self->update_progress_pct(); });
    layer_current_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_layer_current_subject(), this,
        [](DetailedFormatter* self, int) { self->update_layer_text(); });
    layer_total_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_layer_total_subject(), this,
        [](DetailedFormatter* self, int) { self->update_layer_text(); });
    elapsed_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_elapsed_subject(), this,
        [](DetailedFormatter* self, int) { self->update_time_text(); });
    time_left_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_time_left_subject(), this,
        [](DetailedFormatter* self, int) { self->update_time_text(); });
    filament_used_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_filament_used_subject(), this,
        [](DetailedFormatter* self, int) { self->update_filament_text(); });

    // Auto-tool nozzle: observe static active_extruder subjects (no lifetime needed)
    nozzle_temp_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_active_extruder_temp_subject(), this,
        [](DetailedFormatter* self, int) { self->update_nozzle_text(); });
    nozzle_target_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_active_extruder_target_subject(), this,
        [](DetailedFormatter* self, int) { self->update_nozzle_text(); });
    bed_temp_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_bed_temp_subject(), this,
        [](DetailedFormatter* self, int) { self->update_bed_text(); });
    bed_target_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_bed_target_subject(), this,
        [](DetailedFormatter* self, int) { self->update_bed_text(); });
    chamber_temp_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_chamber_temp_subject(), this,
        [](DetailedFormatter* self, int) { self->update_chamber_text(); });
    chamber_target_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_chamber_target_subject(), this,
        [](DetailedFormatter* self, int) { self->update_chamber_text(); });

    // Seed initial values from current subject state
    update_progress_pct();
    update_layer_text();
    update_time_text();
    update_filament_text();
    update_nozzle_text();
    update_bed_text();
    update_chamber_text();

    // Multi-tool: observe tool_count + active_tool to drive gate and T<n> label
    tool_count_observer_ = observe_int_sync<DetailedFormatter>(
        ToolState::instance().get_tool_count_subject(), this,
        [](DetailedFormatter* self, int) {
            self->update_multi_tool();
            self->update_tool_label();
        });
    active_tool_observer_ = observe_int_sync<DetailedFormatter>(
        ToolState::instance().get_active_tool_subject(), this,
        [](DetailedFormatter* self, int) { self->update_tool_label(); });
    update_multi_tool();
    update_tool_label();

    // Arc value observer — keeps lv_arc value in sync with print progress
    arc_value_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_progress_subject(), this, [](DetailedFormatter* self, int pct) {
            if (self->arc_widget_ && lv_obj_is_valid(self->arc_widget_)) {
                lv_arc_set_value(self->arc_widget_, pct);
            }
        });

    spdlog::debug("[DetailedFormatter] subjects initialized");
}

PrintStatusWidget::DetailedFormatter::~DetailedFormatter() {
    spdlog::debug("[DetailedFormatter] tearing down");
    subjects_.deinit_all();
}

void PrintStatusWidget::DetailedFormatter::attach_arc(lv_obj_t* arc) {
    arc_widget_ = arc;
    if (arc) {
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_bg_angles(arc, 135, 45);
        int pct = lv_subject_get_int(get_printer_state().get_print_progress_subject());
        lv_arc_set_value(arc, pct);
    }
}

// ============================================================================
// Nozzle Chevron Callback
// ============================================================================

void PrintStatusWidget::print_status_nozzle_chevron_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("print_status_nozzle_chevron_cb");
    auto* anchor = lv_event_get_current_target_obj(e);
    if (!anchor)
        return;
    // Walk up to find the owning widget instance
    PrintStatusWidget* owner = nullptr;
    for (lv_obj_t* o = anchor; o; o = lv_obj_get_parent(o)) {
        auto* candidate = static_cast<PrintStatusWidget*>(lv_obj_get_user_data(o));
        if (candidate && live_instances().count(candidate)) {
            owner = candidate;
            break;
        }
    }
    if (owner)
        owner->show_nozzle_tool_picker(anchor);
    LVGL_SAFE_EVENT_CB_END();
}

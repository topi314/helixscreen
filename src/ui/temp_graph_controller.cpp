// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_graph_controller.h"

#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "system/crash_handler.h"
#include "temperature_history_manager.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace helix {

using helix::ui::observe_int_sync;
using helix::ui::temperature::centi_to_degrees_f;

// ============================================================================
// Shared color palette
// ============================================================================

const lv_color_t TEMP_GRAPH_SERIES_COLORS[TEMP_GRAPH_PALETTE_SIZE] = {
    lv_color_hex(0xFF4444), // Nozzle (red)
    lv_color_hex(0x88C0D0), // Bed (cyan / nord8)
    lv_color_hex(0xA3BE8C), // Chamber (green / nord14)
    lv_color_hex(0xEBCB8B), // Yellow / nord13
    lv_color_hex(0xB48EAD), // Purple / nord15
    lv_color_hex(0xD08770), // Orange / nord12
    lv_color_hex(0x5E81AC), // Blue / nord10
    lv_color_hex(0xBF616A), // Dark red / nord11
};

// ============================================================================
// Construction / Destruction
// ============================================================================

TempGraphController::TempGraphController(lv_obj_t* container,
                                         const TempGraphControllerConfig& config)
    : config_(config), container_(container) {
    create_graph();
    if (!graph_) {
        spdlog::error("[TempGraphController] Failed to create graph");
        return;
    }

    setup_series();
    setup_observers();
    backfill_history();
    apply_auto_range();

    spdlog::debug("[TempGraphController] Initialized with {} series", series_.size());
}

void TempGraphController::detach() {
    lifetime_.invalidate();

    {
        auto freeze =
            helix::ui::UpdateQueue::instance().scoped_freeze("TempGraphController::detach");
        helix::ui::UpdateQueue::instance().drain();

        connection_observer_.reset();
        for (auto& s : series_) {
            s.temp_obs.reset();
            s.target_obs.reset();
        }
    }

    spdlog::debug("[TempGraphController] Detached observers");
}

TempGraphController::~TempGraphController() {
    // Safe to call multiple times — idempotent (invalidate + reset are no-ops
    // if detach() was already called before deferred deletion)
    detach();

    series_.clear();

    if (graph_) {
        ui_temp_graph_destroy(graph_);
        graph_ = nullptr;
    }

    spdlog::debug("[TempGraphController] Destroyed");
}

// ============================================================================
// Public interface
// ============================================================================

void TempGraphController::set_features(uint32_t features) {
    if (graph_) {
        ui_temp_graph_set_features(graph_, features);
    }
}

void TempGraphController::pause() {
    paused_ = true;
}

void TempGraphController::resume() {
    paused_ = false;
    backfill_history();
}

void TempGraphController::rebuild() {
    // Guard against stale container — if the parent widget was deleted while a
    // deferred reconnect observer callback was queued, container_ is dangling.
    // lv_obj_is_valid() is O(n) but acceptable here (rebuild is debounced to 2s).
    if (!container_ || !lv_obj_is_valid(container_)) {
        spdlog::warn("[TempGraphController] Rebuild skipped — container is null or freed");
        container_ = nullptr;
        return;
    }

    // Debounce rapid rebuilds — reconnect flapping (e.g., Klipper error state)
    // can trigger dozens of rebuilds per second, racing with the LVGL render cycle
    auto now = std::chrono::steady_clock::now();
    if (now - last_rebuild_time_ < REBUILD_DEBOUNCE) {
        spdlog::debug("[TempGraphController] Rebuild debounced (too soon after previous)");
        return;
    }
    last_rebuild_time_ = now;

    crash_handler::breadcrumb::note("tgc", "rebuild", static_cast<long>(generation_ + 1));

    detach();
    series_.clear();

    if (graph_) {
        ui_temp_graph_destroy(graph_);
        graph_ = nullptr;
    }

    ++generation_;
    y_axis_max_ = 100.0f;

    create_graph();
    if (!graph_) {
        spdlog::error("[TempGraphController] Failed to recreate graph on rebuild");
        return;
    }

    setup_series();
    setup_observers();
    backfill_history();
    apply_auto_range();

    spdlog::debug("[TempGraphController] Rebuilt with {} series", series_.size());
}

int TempGraphController::series_id_for(const std::string& klipper_name) const {
    for (const auto& s : series_) {
        if (s.klipper_name == klipper_name) {
            return s.series_id;
        }
    }
    return -1;
}

// ============================================================================
// Graph creation
// ============================================================================

void TempGraphController::create_graph() {
    if (!container_ || !lv_obj_is_valid(container_)) {
        spdlog::error("[TempGraphController] Container is null or freed, cannot create graph");
        container_ = nullptr;
        return;
    }

    graph_ = ui_temp_graph_create(container_);
    if (!graph_)
        return;

    // Size chart to fill container
    lv_obj_t* chart = ui_temp_graph_get_chart(graph_);
    if (chart) {
        lv_obj_set_size(chart, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(chart, theme_manager_get_color("card_bg"), 0);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
        // Let clicks pass through chart to parent container
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    ui_temp_graph_set_axis_size(graph_, config_.axis_size);
    ui_temp_graph_set_features(graph_, config_.initial_features);
    ui_temp_graph_set_point_count(graph_, config_.point_count);
}

// ============================================================================
// Series setup
// ============================================================================

void TempGraphController::setup_series() {
    if (!graph_)
        return;

    auto& ps = get_printer_state();

    for (const auto& spec : config_.series) {
        const char* label =
            spec.display_name.empty() ? spec.klipper_name.c_str() : spec.display_name.c_str();
        int series_id = ui_temp_graph_add_series(graph_, label, spec.color);
        if (series_id < 0) {
            spdlog::warn("[TempGraphController] Failed to add series '{}'", spec.klipper_name);
            continue;
        }

        SeriesState state;
        state.klipper_name = spec.klipper_name;
        state.series_id = series_id;
        state.show_target = spec.show_target;

        // Determine if this uses a dynamic subject
        if (spec.klipper_name.find("extruder") == 0 && ps.extruder_count() > 1) {
            state.is_dynamic = true;
        } else if (spec.klipper_name.find("temperature_sensor") == 0 ||
                   spec.klipper_name.find("temperature_fan") == 0) {
            state.is_dynamic = true;
        }

        series_.push_back(std::move(state));
    }

    spdlog::trace("[TempGraphController] Setup {} series", series_.size());
}

// ============================================================================
// Observer setup
// ============================================================================

void TempGraphController::setup_observers() {
    auto& ps = get_printer_state();
    auto token = lifetime_.token();
    uint32_t gen = generation_;

    for (size_t i = 0; i < series_.size(); ++i) {
        auto& s = series_[i];

        lv_subject_t* temp_subj = nullptr;
        lv_subject_t* target_subj = nullptr;

        if (s.klipper_name == "heater_bed") {
            temp_subj = ps.get_bed_temp_subject();
            target_subj = ps.get_bed_target_subject();
        } else if (s.klipper_name.find("heater_generic") == 0 ||
                   s.klipper_name.find("temperature_fan") == 0) {
            // Chamber (or other heater/fan-based heaters)
            temp_subj = ps.get_chamber_temp_subject();
            target_subj = ps.get_chamber_target_subject();
        } else if (s.klipper_name.find("extruder") == 0) {
            if (ps.extruder_count() <= 1) {
                // Single extruder: use active (static) subjects
                temp_subj = ps.get_active_extruder_temp_subject();
                target_subj = ps.get_active_extruder_target_subject();
            } else {
                // Multi-extruder: use per-extruder (dynamic) subjects
                temp_subj = ps.get_extruder_temp_subject(s.klipper_name, s.lifetime);
                target_subj = ps.get_extruder_target_subject(s.klipper_name, s.lifetime);
            }
        } else {
            // Auxiliary sensor from TemperatureSensorManager
            auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
            temp_subj = sensor_mgr.get_temp_subject(s.klipper_name, s.lifetime);
        }

        if (temp_subj) {
            size_t idx = i;
            s.temp_obs = observe_int_sync<TempGraphController>(
                temp_subj, this,
                [token, gen, idx](TempGraphController* self, int temp_centi) {
                    if (token.expired() || gen != self->generation_)
                        return;
                    if (self->paused_ || !self->graph_)
                        return;

                    auto& si = self->series_[idx];
                    if (si.series_id < 0)
                        return;

                    // Throttle chart updates to 1Hz per series — Klipper pushes
                    // status at ~4Hz which causes excessive LVGL chart invalidations
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
                    if (now_ms - si.last_update_ms < 1000)
                        return;
                    si.last_update_ms = now_ms;

                    float temp_deg = centi_to_degrees_f(temp_centi);
                    ui_temp_graph_update_series_with_time(self->graph_, si.series_id, temp_deg,
                                                          now_ms);
                    self->apply_auto_range();
                },
                s.lifetime);
        }

        if (target_subj && s.show_target) {
            size_t idx = i;
            s.target_obs = observe_int_sync<TempGraphController>(
                target_subj, this,
                [token, gen, idx](TempGraphController* self, int target_centi) {
                    if (token.expired() || gen != self->generation_)
                        return;
                    if (!self->graph_)
                        return;

                    auto& si = self->series_[idx];
                    if (si.series_id < 0)
                        return;

                    float target_deg = centi_to_degrees_f(target_centi);
                    bool show = target_deg > 0.0f;
                    ui_temp_graph_set_series_target(self->graph_, si.series_id, target_deg, show);
                    self->apply_auto_range();
                },
                s.lifetime);
        }
    }

    // Observe printer connection state to clear chart on disconnect and rebuild on
    // REconnect. Only react to actual state changes, and only rebuild after a real
    // disconnect (not the initial connect at startup which would wipe backfilled history).
    auto* conn_subj = ps.get_printer_connection_state_subject();
    if (conn_subj) {
        auto conn_token = lifetime_.token();
        uint32_t conn_gen = generation_;
        auto prev_state = std::make_shared<int>(lv_subject_get_int(conn_subj));
        auto was_disconnected = std::make_shared<bool>(false);
        connection_observer_ = observe_int_sync<TempGraphController>(
            conn_subj, this,
            [conn_token, conn_gen, prev_state, was_disconnected](TempGraphController* self,
                                                                 int state) {
                if (conn_token.expired() || conn_gen != self->generation_)
                    return;
                if (state == *prev_state)
                    return;
                *prev_state = state;
                if (state == 0) { // Disconnected
                    *was_disconnected = true;
                    spdlog::debug("[TempGraphController] Disconnected, clearing chart");
                    if (self->graph_) {
                        ui_temp_graph_clear(self->graph_);
                    }
                } else if (state == 2 && *was_disconnected) { // Reconnected
                    spdlog::debug("[TempGraphController] Reconnected, rebuilding");
                    self->rebuild();
                }
            });
    }
}

// ============================================================================
// History backfill
// ============================================================================

void TempGraphController::backfill_history() {
    auto* history_mgr = get_temperature_history_manager();
    if (!graph_ || !history_mgr)
        return;

    // Only fetch samples that fit in the chart buffer to avoid pushing
    // thousands of points through the circular buffer (wastes CPU and
    // corrupts X-axis timestamp tracking)
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    int64_t cutoff_ms = now_ms - static_cast<int64_t>(config_.point_count) * 1000;

    for (auto& s : series_) {
        if (s.series_id < 0)
            continue;

        auto samples = history_mgr->get_samples_since(s.klipper_name, cutoff_ms);
        if (samples.empty())
            continue;

        for (const auto& sample : samples) {
            float temp_deg = centi_to_degrees_f(sample.temp_centi);
            ui_temp_graph_update_series_with_time(graph_, s.series_id, temp_deg,
                                                  sample.timestamp_ms);
        }

        // Set initial target from most recent sample
        if (s.show_target) {
            float target_deg = centi_to_degrees_f(samples.back().target_centi);
            if (target_deg > 0.0f) {
                ui_temp_graph_set_series_target(graph_, s.series_id, target_deg, true);
            }
        }
    }
}

// ============================================================================
// Auto-range
// ============================================================================

void TempGraphController::apply_auto_range() {
    if (!graph_)
        return;

    // Find max relevant temperature (from data and targets)
    float max_temp = graph_->max_visible_temp;
    for (const auto& s : series_) {
        if (s.show_target && s.series_id >= 0) {
            for (int j = 0; j < graph_->series_count; j++) {
                auto& meta = graph_->series_meta[j];
                if (meta.id == s.series_id && meta.show_target && meta.target_temp > max_temp) {
                    max_temp = meta.target_temp;
                }
            }
        }
    }

    float new_max = calculate_temp_graph_y_max(y_axis_max_, max_temp, graph_->max_visible_temp,
                                               config_.scale_params);

    bool changed = (new_max != y_axis_max_);
    y_axis_max_ = new_max;

    ui_temp_graph_set_temp_range(graph_, 0.0f, y_axis_max_);

    float y_increment = (y_axis_max_ <= 150.0f) ? 25.0f : 50.0f;
    bool show_y = (ui_temp_graph_get_features(graph_) & TEMP_GRAPH_FEATURE_Y_AXIS) != 0;
    ui_temp_graph_set_y_axis(graph_, y_increment, show_y);

    if (changed) {
        spdlog::trace("[TempGraphController] Y-axis range: 0-{}°C (increment={}, show_y={})",
                      y_axis_max_, y_increment, show_y);
    }
}

} // namespace helix

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_happy_hare.h"

#include "config.h"
#include "hh_defaults.h"
#include "moonraker_api.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendHappyHare::AmsBackendHappyHare(MoonrakerAPI* api, MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with Happy Hare defaults
    system_info_.type = AmsType::HAPPY_HARE;
    system_info_.type_name = "Happy Hare";
    system_info_.supports_endless_spool = true;
    system_info_.supports_tool_mapping = true;
    // Bypass support is determined at runtime from mmu.has_bypass status field.
    // Default to true; will be updated when first status arrives.
    system_info_.supports_bypass = true;
    // Happy Hare bypass is always positional (selector moves to bypass position), never a sensor
    system_info_.has_hardware_bypass_sensor = false;
    // Default to TIP_FORM -- Happy Hare's default macro is _MMU_FORM_TIP.
    // Overridden by query_tip_method_from_config() once configfile response arrives.
    system_info_.tip_method = TipMethod::TIP_FORM;

    spdlog::debug("[AMS HappyHare] Backend created");
}

AmsBackendHappyHare::~AmsBackendHappyHare() {
    // lifetime_ destructor calls invalidate() automatically
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void AmsBackendHappyHare::on_started() {
    // Query configfile to determine tip method (cutter vs tip-forming).
    // Happy Hare determines this from form_tip_macro: if it contains "cut",
    // it's a cutter system; otherwise it's tip-forming or none.
    query_tip_method_from_config();

    // Query selector type to determine topology (Type A=LINEAR vs Type B=HUB)
    query_selector_type_from_config();

    // Query configfile.settings.mmu for speed/distance defaults
    query_config_defaults();
}

// stop(), release_subscriptions(), is_running() provided by AmsSubscriptionBackend

// ============================================================================
// Event System
// ============================================================================

// set_event_callback() and emit_event() provided by AmsSubscriptionBackend

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendHappyHare::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_.is_initialized()) {
        return system_info_;
    }

    // Build slot data from registry, then overlay non-slot metadata
    auto info = slots_.build_system_info();

    // Copy system-level fields not managed by registry
    info.type = system_info_.type;
    info.type_name = system_info_.type_name;
    info.version = system_info_.version;
    info.action = system_info_.action;
    info.operation_detail = system_info_.operation_detail;
    info.current_slot = system_info_.current_slot;
    info.current_tool = system_info_.current_tool;
    info.pending_target_slot = system_info_.pending_target_slot;
    info.current_toolchange = system_info_.current_toolchange;
    info.number_of_toolchanges = system_info_.number_of_toolchanges;
    info.filament_loaded = system_info_.filament_loaded;
    info.supports_endless_spool = system_info_.supports_endless_spool;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.supports_bypass = system_info_.supports_bypass;
    info.has_hardware_bypass_sensor = system_info_.has_hardware_bypass_sensor;
    info.tip_method = system_info_.tip_method;
    info.supports_purge = system_info_.supports_purge;

    // Happy Hare v4 extended fields
    info.spoolman_mode = system_info_.spoolman_mode;
    info.pending_spool_id = system_info_.pending_spool_id;
    info.espooler_state = system_info_.espooler_state;
    info.sync_feedback_state = system_info_.sync_feedback_state;
    info.sync_drive = system_info_.sync_drive;
    info.clog_detection = system_info_.clog_detection;
    info.encoder_flow_rate = system_info_.encoder_flow_rate;
    info.encoder_info = system_info_.encoder_info;
    info.flowguard_info = system_info_.flowguard_info;
    info.sync_feedback_flow_rate = system_info_.sync_feedback_flow_rate;
    info.sync_feedback_bias = system_info_.sync_feedback_bias;
    info.sync_feedback_bias_raw = system_info_.sync_feedback_bias_raw;
    info.toolchange_purge_volume = system_info_.toolchange_purge_volume;

    // Copy unit-level metadata not managed by registry
    for (size_t u = 0; u < info.units.size() && u < system_info_.units.size(); ++u) {
        info.units[u].connected = system_info_.units[u].connected;
        info.units[u].has_encoder = system_info_.units[u].has_encoder;
        info.units[u].has_toolhead_sensor = system_info_.units[u].has_toolhead_sensor;
        info.units[u].has_slot_sensors = system_info_.units[u].has_slot_sensors;
        info.units[u].topology = system_info_.units[u].topology;
        info.units[u].has_hub_sensor = system_info_.units[u].has_hub_sensor;
        info.units[u].hub_sensor_triggered = system_info_.units[u].hub_sensor_triggered;
    }

    return info;
}

AmsType AmsBackendHappyHare::get_type() const {
    return AmsType::HAPPY_HARE;
}

bool AmsBackendHappyHare::manages_active_spool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.spoolman_mode != SpoolmanMode::OFF;
}

SlotInfo AmsBackendHappyHare::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto* entry = slots_.get(slot_index);
    if (entry) {
        return entry->info;
    }

    // Return empty slot info for invalid index
    SlotInfo empty;
    empty.slot_index = -1;
    empty.global_index = -1;
    return empty;
}

// get_current_action(), get_current_tool(), get_current_slot(), is_filament_loaded()
// provided by AmsSubscriptionBackend

PathTopology AmsBackendHappyHare::get_topology() const {
    // Type B (VirtualSelector) uses HUB topology (3MS, Box Turtle, Night Owl)
    // Type A (LinearSelector, RotarySelector, ServoSelector) uses LINEAR (ERCF, Tradrack)
    std::lock_guard<std::mutex> lock(mutex_);
    return is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
}

PathTopology AmsBackendHappyHare::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        return system_info_.units[unit_index].topology;
    }
    return is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
}

bool AmsBackendHappyHare::is_type_b() const {
    return selector_type_ == "VirtualSelector";
}

void AmsBackendHappyHare::update_unit_topologies() {
    auto topo = is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
    bool encoder = !is_type_b();
    for (auto& unit : system_info_.units) {
        unit.topology = topo;
        unit.has_encoder = encoder;
    }
}

PathSegment AmsBackendHappyHare::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Convert Happy Hare filament_pos to unified PathSegment
    return path_segment_from_happy_hare_pos(filament_pos_);
}

PathSegment AmsBackendHappyHare::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if this is the active slot - return the current filament segment
    if (slot_index == system_info_.current_slot && system_info_.filament_loaded) {
        return path_segment_from_happy_hare_pos(filament_pos_);
    }

    // For non-active slots, check pre-gate sensor first for better visualization
    const auto* entry = slots_.get(slot_index);
    if (entry) {
        if (entry->sensors.has_pre_gate_sensor && entry->sensors.pre_gate_triggered) {
            return PathSegment::PREP; // Filament detected at pre-gate sensor
        }

        // Fall back to gate_status for slots without pre-gate sensors
        if (entry->info.status == SlotStatus::AVAILABLE ||
            entry->info.status == SlotStatus::FROM_BUFFER) {
            return PathSegment::SPOOL; // Filament at spool ready position
        }
    }

    return PathSegment::NONE;
}

PathSegment AmsBackendHappyHare::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_segment_;
}

bool AmsBackendHappyHare::slot_has_prep_sensor(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return false;
    }
    return entry->sensors.has_pre_gate_sensor;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

void AmsBackendHappyHare::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    if (!notification.contains("params") || !notification["params"].is_array() ||
        notification["params"].empty()) {
        return;
    }

    const auto& params = notification["params"][0];
    if (!params.is_object()) {
        return;
    }

    // Check if this notification contains MMU data
    if (!params.contains("mmu")) {
        return;
    }

    const auto& mmu_data = params["mmu"];
    if (!mmu_data.is_object()) {
        return;
    }

    spdlog::trace("[AMS HappyHare] Received MMU status update");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_mmu_state(mmu_data);
    }

    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendHappyHare::parse_mmu_state(const nlohmann::json& mmu_data) {
    // Parse current gate: printer.mmu.gate
    // -1 = no gate selected, -2 = bypass
    if (mmu_data.contains("gate") && mmu_data["gate"].is_number_integer()) {
        system_info_.current_slot = mmu_data["gate"].get<int>();
        spdlog::trace("[AMS HappyHare] Current slot: {}", system_info_.current_slot);
    }

    // Parse current tool: printer.mmu.tool
    if (mmu_data.contains("tool") && mmu_data["tool"].is_number_integer()) {
        system_info_.current_tool = mmu_data["tool"].get<int>();
        spdlog::trace("[AMS HappyHare] Current tool: {}", system_info_.current_tool);
    }

    // Parse filament loaded state: printer.mmu.filament
    // Values: "Loaded", "Unloaded"
    if (mmu_data.contains("filament") && mmu_data["filament"].is_string()) {
        std::string filament_state = mmu_data["filament"].get<std::string>();
        system_info_.filament_loaded = (filament_state == "Loaded");
        spdlog::trace("[AMS HappyHare] Filament loaded: {}", system_info_.filament_loaded);
    }

    // Parse reason_for_pause: descriptive error message from Happy Hare
    if (mmu_data.contains("reason_for_pause") && mmu_data["reason_for_pause"].is_string()) {
        reason_for_pause_ = mmu_data["reason_for_pause"].get<std::string>();
        spdlog::trace("[AMS HappyHare] Reason for pause: {}", reason_for_pause_);
    }

    // Parse action: printer.mmu.action
    // Values: "Idle", "Loading", "Unloading", "Forming Tip", "Heating", "Checking", etc.
    if (mmu_data.contains("action") && mmu_data["action"].is_string()) {
        std::string action_str = mmu_data["action"].get<std::string>();
        AmsAction prev_action = system_info_.action;
        system_info_.action = ams_action_from_string(action_str);
        system_info_.operation_detail = action_str;
        spdlog::trace("[AMS HappyHare] Action: {} ({})", ams_action_to_string(system_info_.action),
                      action_str);

        // Clear error segment when recovering to idle
        if (prev_action == AmsAction::ERROR && system_info_.action == AmsAction::IDLE) {
            error_segment_ = PathSegment::NONE;
            reason_for_pause_.clear();

            // Clear slot errors on all slots
            for (int i = 0; i < slots_.slot_count(); ++i) {
                auto* entry = slots_.get_mut(i);
                if (entry && entry->info.error.has_value()) {
                    entry->info.error.reset();
                    spdlog::debug("[AMS HappyHare] Cleared error on slot {}", i);
                }
            }
        }

        // Set slot error when entering error state
        if (system_info_.action == AmsAction::ERROR && prev_action != AmsAction::ERROR) {
            error_segment_ = path_segment_from_happy_hare_pos(filament_pos_);

            // Set error on current slot (if valid)
            if (system_info_.current_slot >= 0) {
                auto* entry = slots_.get_mut(system_info_.current_slot);
                if (entry) {
                    SlotError err;
                    // Use reason_for_pause if available; fall back to operation_detail
                    if (!reason_for_pause_.empty()) {
                        err.message = reason_for_pause_;
                    } else {
                        err.message = action_str;
                    }
                    err.severity = SlotError::ERROR;
                    entry->info.error = err;
                    spdlog::debug("[AMS HappyHare] Error on slot {}: {}", system_info_.current_slot,
                                  err.message);
                }
            }
        }
    }

    // Parse filament_pos: printer.mmu.filament_pos
    // Values: 0=unloaded, 1-2=gate area, 3=in bowden, 4=end bowden, 5=homed extruder,
    //         6=extruder entry, 7-8=loaded
    if (mmu_data.contains("filament_pos") && mmu_data["filament_pos"].is_number_integer()) {
        filament_pos_ = mmu_data["filament_pos"].get<int>();
        spdlog::trace("[AMS HappyHare] Filament pos: {} -> {}", filament_pos_,
                      path_segment_to_string(path_segment_from_happy_hare_pos(filament_pos_)));

        // Update hub_sensor_triggered on units based on filament position
        // pos >= 3 means filament is in bowden or further (past the selector/hub)
        bool past_hub = (filament_pos_ >= 3);
        for (auto& unit : system_info_.units) {
            // Active unit: determined by current_slot falling within this unit's range
            int slot = system_info_.current_slot;
            if (slot >= unit.first_slot_global_index &&
                slot < unit.first_slot_global_index + unit.slot_count) {
                unit.hub_sensor_triggered = past_hub;
            } else {
                unit.hub_sensor_triggered = false;
            }
        }
    }

    // Parse bowden_progress: printer.mmu.bowden_progress (v4)
    // 0-100 = loading progress percentage, -1 = not applicable
    if (mmu_data.contains("bowden_progress") && mmu_data["bowden_progress"].is_number_integer()) {
        bowden_progress_ = std::clamp(mmu_data["bowden_progress"].get<int>(), -1, 100);
        spdlog::trace("[AMS HappyHare] Bowden progress: {}%", bowden_progress_);
    }

    // Parse has_bypass: printer.mmu.has_bypass
    // Not all MMU types support bypass (e.g., ERCF/Tradrack do, BoxTurtle does not)
    if (mmu_data.contains("has_bypass") && mmu_data["has_bypass"].is_boolean()) {
        system_info_.supports_bypass = mmu_data["has_bypass"].get<bool>();
        spdlog::trace("[AMS HappyHare] Bypass supported: {}", system_info_.supports_bypass);
    }

    // Parse num_units if available (multi-unit Happy Hare setups)
    if (mmu_data.contains("num_units") && mmu_data["num_units"].is_number_integer()) {
        num_units_ = mmu_data["num_units"].get<int>();
        if (num_units_ < 1)
            num_units_ = 1;
        spdlog::trace("[AMS HappyHare] Number of units: {}", num_units_);
    }

    // Parse num_gates for dissimilar multi-unit (v4)
    // Can be a string like "6,4" for 6-gate ERCF + 4-gate Box Turtle, or plain int
    if (mmu_data.contains("num_gates")) {
        const auto& ng = mmu_data["num_gates"];
        if (ng.is_string()) {
            // Parse comma-separated per-unit gate counts (v4 dissimilar multi-MMU)
            std::string ng_str = ng.get<std::string>();
            std::vector<int> counts;
            std::istringstream iss(ng_str);
            std::string token;
            while (std::getline(iss, token, ',')) {
                try {
                    int count = std::stoi(token);
                    if (count > 0) {
                        counts.push_back(count);
                    } else {
                        spdlog::warn("[AMS HappyHare] Ignoring non-positive gate count {} in "
                                     "num_gates string",
                                     count);
                    }
                } catch (...) {
                    spdlog::warn("[AMS HappyHare] Ignoring invalid token in num_gates string");
                }
            }
            if (!counts.empty()) {
                per_unit_gate_counts_ = counts;
                spdlog::debug("[AMS HappyHare] Per-unit gate counts from num_gates string: {}",
                              ng_str);
            }
        } else if (ng.is_number_integer()) {
            // EMU sends plain integer (single unit)
            int count = ng.get<int>();
            if (count > 0) {
                per_unit_gate_counts_ = {count};
                spdlog::debug("[AMS HappyHare] Single-unit gate count from num_gates int: {}",
                              count);
            }
        } else if (ng.is_array()) {
            // Config format: [8] or [6, 4]
            std::vector<int> counts;
            for (const auto& c : ng) {
                if (c.is_number_integer()) {
                    int count = c.get<int>();
                    if (count > 0)
                        counts.push_back(count);
                }
            }
            if (!counts.empty()) {
                per_unit_gate_counts_ = counts;
                spdlog::debug("[AMS HappyHare] Per-unit gate counts from num_gates array");
            }
        }
    }

    // Parse unit_gate_counts array if present (future-proof for v4)
    if (mmu_data.contains("unit_gate_counts") && mmu_data["unit_gate_counts"].is_array()) {
        std::vector<int> counts;
        for (const auto& c : mmu_data["unit_gate_counts"]) {
            if (c.is_number_integer()) {
                counts.push_back(c.get<int>());
            }
        }
        if (!counts.empty()) {
            per_unit_gate_counts_ = counts;
            spdlog::debug("[AMS HappyHare] Per-unit gate counts from unit_gate_counts array");
        }
    }

    // Parse active unit: printer.mmu.unit (v4)
    if (mmu_data.contains("unit") && mmu_data["unit"].is_number_integer()) {
        active_unit_ = mmu_data["unit"].get<int>();
        spdlog::trace("[AMS HappyHare] Active unit: {}", active_unit_);
    }

    // Parse gate_status array: printer.mmu.gate_status
    // Values: -1 = unknown, 0 = empty, 1 = available, 2 = from_buffer
    if (mmu_data.contains("gate_status") && mmu_data["gate_status"].is_array()) {
        const auto& gate_status = mmu_data["gate_status"];
        int gate_count = static_cast<int>(gate_status.size());

        // Initialize gates if this is the first time we see gate_status
        if (!slots_.is_initialized() && gate_count > 0) {
            initialize_slots(gate_count);
        }

        // Update gate status values via SlotRegistry
        for (size_t i = 0; i < gate_status.size(); ++i) {
            if (gate_status[i].is_number_integer()) {
                int hh_status = gate_status[i].get<int>();
                SlotStatus status = slot_status_from_happy_hare(hh_status);

                // Mark the currently loaded slot as LOADED instead of AVAILABLE
                if (system_info_.filament_loaded &&
                    static_cast<int>(i) == system_info_.current_slot &&
                    status == SlotStatus::AVAILABLE) {
                    status = SlotStatus::LOADED;
                }

                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.status = status;
                }
            }
        }
    }

    // Parse gate_color_rgb: integer array [0xRRGGBB, ...] or float array [[R,G,B], ...]
    bool colors_parsed = false;
    if (mmu_data.contains("gate_color_rgb") && mmu_data["gate_color_rgb"].is_array()) {
        const auto& colors = mmu_data["gate_color_rgb"];
        for (size_t i = 0; i < colors.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry)
                continue;

            if (colors[i].is_number_integer()) {
                // Traditional format: 0xRRGGBB integer
                entry->info.color_rgb = static_cast<uint32_t>(colors[i].get<int>());
                colors_parsed = true;
            } else if (colors[i].is_array() && colors[i].size() >= 3 && colors[i][0].is_number() &&
                       colors[i][1].is_number() && colors[i][2].is_number()) {
                // EMU format: [R, G, B] floats 0.0-1.0
                auto r = static_cast<uint8_t>(
                    std::clamp(colors[i][0].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
                auto g = static_cast<uint8_t>(
                    std::clamp(colors[i][1].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
                auto b = static_cast<uint8_t>(
                    std::clamp(colors[i][2].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
                entry->info.color_rgb = (static_cast<uint32_t>(r) << 16) |
                                        (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                colors_parsed = true;
            }
        }
    }

    // Fallback: parse gate_color hex strings ["ffffff", "000000", ...]
    if (!colors_parsed && mmu_data.contains("gate_color") && mmu_data["gate_color"].is_array()) {
        const auto& colors = mmu_data["gate_color"];
        for (size_t i = 0; i < colors.size(); ++i) {
            if (colors[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    try {
                        entry->info.color_rgb = static_cast<uint32_t>(
                            std::stoul(colors[i].get<std::string>(), nullptr, 16));
                    } catch (...) {
                        // Invalid hex string, leave default color
                    }
                }
            }
        }
    }

    // Parse gate_material array: printer.mmu.gate_material
    // Values are strings like "PLA", "PETG", "ABS"
    if (mmu_data.contains("gate_material") && mmu_data["gate_material"].is_array()) {
        const auto& materials = mmu_data["gate_material"];
        for (size_t i = 0; i < materials.size(); ++i) {
            if (materials[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.material = materials[i].get<std::string>();
                }
            }
        }
    }

    // === Happy Hare v4 extended status fields ===

    // Parse espooler_active: printer.mmu.espooler_active (v4)
    if (mmu_data.contains("espooler_active") && mmu_data["espooler_active"].is_string()) {
        system_info_.espooler_state = mmu_data["espooler_active"].get<std::string>();
        espooler_active_ = system_info_.espooler_state;
        spdlog::trace("[AMS HappyHare] eSpooler state: {}", system_info_.espooler_state);
    }

    // Parse sync_feedback_state: printer.mmu.sync_feedback_state (v4)
    if (mmu_data.contains("sync_feedback_state") && mmu_data["sync_feedback_state"].is_string()) {
        system_info_.sync_feedback_state = mmu_data["sync_feedback_state"].get<std::string>();
        spdlog::trace("[AMS HappyHare] Sync feedback: {}", system_info_.sync_feedback_state);
    }

    // Parse sync_feedback_bias_modelled: printer.mmu.sync_feedback_bias_modelled (v4)
    if (mmu_data.contains("sync_feedback_bias_modelled") &&
        mmu_data["sync_feedback_bias_modelled"].is_number()) {
        system_info_.sync_feedback_bias = mmu_data["sync_feedback_bias_modelled"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback bias (modelled): {:.3f}",
                      system_info_.sync_feedback_bias);
    }

    // Parse sync_feedback_bias_raw: printer.mmu.sync_feedback_bias_raw (v4)
    if (mmu_data.contains("sync_feedback_bias_raw") &&
        mmu_data["sync_feedback_bias_raw"].is_number()) {
        system_info_.sync_feedback_bias_raw = mmu_data["sync_feedback_bias_raw"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback bias (raw): {:.3f}",
                      system_info_.sync_feedback_bias_raw);
    }

    // Parse sync_drive: printer.mmu.sync_drive (v4)
    if (mmu_data.contains("sync_drive") && mmu_data["sync_drive"].is_boolean()) {
        system_info_.sync_drive = mmu_data["sync_drive"].get<bool>();
        spdlog::trace("[AMS HappyHare] Sync drive: {}", system_info_.sync_drive);
    }

    // Parse clog_detection_enabled: printer.mmu.clog_detection_enabled (v4)
    // 0=off, 1=manual, 2=auto
    if (mmu_data.contains("clog_detection_enabled") &&
        mmu_data["clog_detection_enabled"].is_number_integer()) {
        system_info_.clog_detection = mmu_data["clog_detection_enabled"].get<int>();
        system_info_.encoder_info.detection_mode = system_info_.clog_detection;
        system_info_.encoder_info.enabled = (system_info_.clog_detection > 0);
        spdlog::trace("[AMS HappyHare] Clog detection: {}", system_info_.clog_detection);
    }

    // Parse encoder: printer.mmu.encoder (v4, nested)
    if (mmu_data.contains("encoder") && mmu_data["encoder"].is_object()) {
        const auto& encoder = mmu_data["encoder"];
        if (encoder.contains("flow_rate") && encoder["flow_rate"].is_number()) {
            system_info_.encoder_info.flow_rate = encoder["flow_rate"].get<int>();
            // Keep legacy field in sync
            system_info_.encoder_flow_rate = system_info_.encoder_info.flow_rate;
            spdlog::trace("[AMS HappyHare] Encoder flow rate: {}", system_info_.encoder_flow_rate);
        }
        if (encoder.contains("desired_headroom") && encoder["desired_headroom"].is_number()) {
            system_info_.encoder_info.desired_headroom = encoder["desired_headroom"].get<float>();
        }
        if (encoder.contains("detection_length") && encoder["detection_length"].is_number()) {
            system_info_.encoder_info.detection_length = encoder["detection_length"].get<float>();
        }
        if (encoder.contains("headroom") && encoder["headroom"].is_number()) {
            system_info_.encoder_info.headroom = encoder["headroom"].get<float>();
        }
        if (encoder.contains("min_headroom") && encoder["min_headroom"].is_number()) {
            system_info_.encoder_info.min_headroom = encoder["min_headroom"].get<float>();
        }
        spdlog::trace("[AMS HappyHare] Encoder: headroom={:.1f}/{:.1f} min={:.1f}",
                      system_info_.encoder_info.headroom,
                      system_info_.encoder_info.detection_length,
                      system_info_.encoder_info.min_headroom);
    }

    // Parse flowguard: printer.mmu.flowguard (v4, nested)
    if (mmu_data.contains("flowguard") && mmu_data["flowguard"].is_object()) {
        const auto& fg = mmu_data["flowguard"];
        if (fg.contains("enabled") && fg["enabled"].is_boolean()) {
            system_info_.flowguard_info.enabled = fg["enabled"].get<bool>();
        }
        if (fg.contains("active") && fg["active"].is_boolean()) {
            system_info_.flowguard_info.active = fg["active"].get<bool>();
        }
        if (fg.contains("trigger") && fg["trigger"].is_string()) {
            system_info_.flowguard_info.trigger = fg["trigger"].get<std::string>();
        }
        if (fg.contains("level") && fg["level"].is_number()) {
            system_info_.flowguard_info.level = fg["level"].get<float>();
        }
        if (fg.contains("max_clog") && fg["max_clog"].is_number()) {
            system_info_.flowguard_info.max_clog = fg["max_clog"].get<float>();
        }
        if (fg.contains("max_tangle") && fg["max_tangle"].is_number()) {
            system_info_.flowguard_info.max_tangle = fg["max_tangle"].get<float>();
        }
        if (fg.contains("encoder_mode") && fg["encoder_mode"].is_number()) {
            flowguard_encoder_mode_ = fg["encoder_mode"].get<int>();
        }
        spdlog::trace("[AMS HappyHare] Flowguard: enabled={} active={} trigger={} level={:.2f}",
                      system_info_.flowguard_info.enabled, system_info_.flowguard_info.active,
                      system_info_.flowguard_info.trigger, system_info_.flowguard_info.level);
    }

    // Parse LED state: printer.mmu.leds.unit0.exit_effect (v4)
    if (mmu_data.contains("leds") && mmu_data["leds"].is_object()) {
        const auto& leds = mmu_data["leds"];
        if (leds.contains("unit0") && leds["unit0"].is_object()) {
            const auto& u0 = leds["unit0"];
            if (u0.contains("exit_effect") && u0["exit_effect"].is_string()) {
                led_exit_effect_ = u0["exit_effect"].get<std::string>();
                spdlog::trace("[AMS HappyHare] LED exit effect: {}", led_exit_effect_);
            }
        }
    }

    // Parse sync_feedback_flow_rate: printer.mmu.sync_feedback_flow_rate (top-level)
    if (mmu_data.contains("sync_feedback_flow_rate") &&
        mmu_data["sync_feedback_flow_rate"].is_number()) {
        system_info_.sync_feedback_flow_rate = mmu_data["sync_feedback_flow_rate"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback flow rate: {:.1f}",
                      system_info_.sync_feedback_flow_rate);
    }

    // Parse toolchange_purge_volume: printer.mmu.toolchange_purge_volume (v4)
    if (mmu_data.contains("toolchange_purge_volume") &&
        mmu_data["toolchange_purge_volume"].is_number()) {
        system_info_.toolchange_purge_volume = mmu_data["toolchange_purge_volume"].get<float>();
        spdlog::trace("[AMS HappyHare] Toolchange purge volume: {:.1f}",
                      system_info_.toolchange_purge_volume);
    }

    // Parse num_toolchanges: printer.mmu.num_toolchanges
    // Count of completed tool changes (1 = first swap done). Convert to 0-based index.
    if (mmu_data.contains("num_toolchanges") && mmu_data["num_toolchanges"].is_number_integer()) {
        int count = mmu_data["num_toolchanges"].get<int>();
        system_info_.current_toolchange = (count > 0) ? (count - 1) : -1;
        spdlog::trace("[AMS HappyHare] Toolchange count: {} -> index: {}", count,
                      system_info_.current_toolchange);
    }

    // Parse slicer_tool_map.total_toolchanges: printer.mmu.slicer_tool_map
    // Contains total_toolchanges (int or null) from slicer metadata
    if (mmu_data.contains("slicer_tool_map") && mmu_data["slicer_tool_map"].is_object()) {
        const auto& stm = mmu_data["slicer_tool_map"];
        if (stm.contains("total_toolchanges") && stm["total_toolchanges"].is_number_integer()) {
            system_info_.number_of_toolchanges = stm["total_toolchanges"].get<int>();
            spdlog::trace("[AMS HappyHare] Total toolchanges from slicer: {}",
                          system_info_.number_of_toolchanges);
        } else {
            system_info_.number_of_toolchanges = 0;
        }
    }

    // Parse spoolman_support: printer.mmu.spoolman_support (v4)
    if (mmu_data.contains("spoolman_support") && mmu_data["spoolman_support"].is_string()) {
        system_info_.spoolman_mode =
            spoolman_mode_from_string(mmu_data["spoolman_support"].get<std::string>());
        spdlog::trace("[AMS HappyHare] Spoolman mode: {}",
                      spoolman_mode_to_string(system_info_.spoolman_mode));
    }

    // Parse pending_spool_id: printer.mmu.pending_spool_id (v4)
    if (mmu_data.contains("pending_spool_id") && mmu_data["pending_spool_id"].is_number_integer()) {
        system_info_.pending_spool_id = mmu_data["pending_spool_id"].get<int>();
        spdlog::trace("[AMS HappyHare] Pending spool ID: {}", system_info_.pending_spool_id);
    }

    // Parse gate_spool_id array: printer.mmu.gate_spool_id (v4)
    // Per-gate Spoolman spool IDs — enables weight polling and fill gauges
    if (mmu_data.contains("gate_spool_id") && mmu_data["gate_spool_id"].is_array()) {
        const auto& spool_ids = mmu_data["gate_spool_id"];
        for (size_t i = 0; i < spool_ids.size(); ++i) {
            if (spool_ids[i].is_number_integer()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    int id = spool_ids[i].get<int>();
                    entry->info.spoolman_id = (id > 0) ? id : 0;
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_spool_id for {} gates", spool_ids.size());
    }

    // Parse gate_temperature array: printer.mmu.gate_temperature (v4)
    // Per-gate nozzle temperature recommendations
    if (mmu_data.contains("gate_temperature") && mmu_data["gate_temperature"].is_array()) {
        const auto& gate_temps = mmu_data["gate_temperature"];
        for (size_t i = 0; i < gate_temps.size(); ++i) {
            if (gate_temps[i].is_number()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    int temp = gate_temps[i].get<int>();
                    entry->info.nozzle_temp_min = temp;
                    entry->info.nozzle_temp_max = temp;
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_temperature for {} gates", gate_temps.size());
    }

    // Parse gate_name array: printer.mmu.gate_name (v4)
    // Per-gate filament names
    if (mmu_data.contains("gate_name") && mmu_data["gate_name"].is_array()) {
        const auto& gate_names = mmu_data["gate_name"];
        for (size_t i = 0; i < gate_names.size(); ++i) {
            if (gate_names[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.color_name = gate_names[i].get<std::string>();
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_name for {} gates", gate_names.size());
    }

    // Fallback: parse gate_filament_name (EMU uses this instead of gate_name)
    if (mmu_data.contains("gate_filament_name") && mmu_data["gate_filament_name"].is_array()) {
        const auto& names = mmu_data["gate_filament_name"];
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry && entry->info.color_name.empty()) {
                    entry->info.color_name = names[i].get<std::string>();
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_filament_name for {} gates", names.size());
    }

    // Parse ttg_map (tool-to-gate mapping) if available
    if (mmu_data.contains("ttg_map") && mmu_data["ttg_map"].is_array()) {
        const auto& ttg_map = mmu_data["ttg_map"];
        std::vector<int> ttg_vec;
        ttg_vec.reserve(ttg_map.size());

        for (const auto& mapping : ttg_map) {
            if (mapping.is_number_integer()) {
                ttg_vec.push_back(mapping.get<int>());
            }
        }

        // Update both legacy and registry tool maps
        system_info_.tool_to_slot_map = ttg_vec;
        slots_.set_tool_map(ttg_vec);
    }

    // Parse sensors dict: printer.mmu.sensors
    // Keys matching "mmu_pre_gate_X" indicate pre-gate sensors per gate.
    // Values: true (triggered/filament present), false (not triggered), null (error/unknown)
    if (mmu_data.contains("sensors") && mmu_data["sensors"].is_object()) {
        const auto& sensors = mmu_data["sensors"];
        const std::string prefix = "mmu_pre_gate_";
        bool any_sensor = false;

        for (auto it = sensors.begin(); it != sensors.end(); ++it) {
            const std::string& key = it.key();
            if (key.rfind(prefix, 0) != 0) {
                continue; // Not a pre-gate sensor key
            }

            // Extract gate index from key suffix
            std::string index_str = key.substr(prefix.size());
            int gate_idx = -1;
            try {
                gate_idx = std::stoi(index_str);
            } catch (...) {
                continue; // Not a valid integer suffix
            }

            if (gate_idx < 0) {
                continue;
            }

            auto* entry = slots_.get_mut(gate_idx);
            if (!entry) {
                continue;
            }

            entry->sensors.has_pre_gate_sensor = true;
            entry->sensors.pre_gate_triggered = it.value().is_boolean() && it.value().get<bool>();
            any_sensor = true;

            spdlog::trace("[AMS HappyHare] Pre-gate sensor {}: present=true, triggered={}",
                          gate_idx, entry->sensors.pre_gate_triggered);
        }

        // If no per-gate sensors found, check for aggregate format (EMU)
        // EMU reports "mmu_pre_gate" (bool) and "mmu_gear" (bool) for the active gate
        if (!any_sensor && sensors.contains("mmu_pre_gate")) {
            bool pre_gate_val =
                sensors["mmu_pre_gate"].is_boolean() && sensors["mmu_pre_gate"].get<bool>();
            // Note: mmu_gear sensor reading is available but not stored — UI only
            // displays pre-gate sensor status. Add to SlotSensors if needed later.

            // Mark all gates as having sensors, clear stale trigger readings
            // (we only know the current gate's state from aggregate format)
            for (int i = 0; i < slots_.slot_count(); ++i) {
                auto* entry = slots_.get_mut(i);
                if (entry) {
                    entry->sensors.has_pre_gate_sensor = true;
                    entry->sensors.pre_gate_triggered = false;
                }
            }

            // Set the current gate's actual reading
            if (system_info_.current_slot >= 0) {
                auto* entry = slots_.get_mut(system_info_.current_slot);
                if (entry) {
                    entry->sensors.pre_gate_triggered = pre_gate_val;
                }
            }

            any_sensor = true;
            spdlog::trace("[AMS HappyHare] Aggregate sensors: pre_gate={}", pre_gate_val);
        }

        // Update has_slot_sensors flag on units based on actual sensor data
        for (auto& unit : system_info_.units) {
            unit.has_slot_sensors = any_sensor;
        }
    }

    // Parse drying_state: object (KMS/traditional) or array of strings (EMU per-gate)
    if (mmu_data.contains("drying_state")) {
        const auto& drying = mmu_data["drying_state"];
        if (drying.is_object()) {
            // Traditional object format: {active, current_temp, target_temp, ...}
            dryer_info_.supported = true;
            if (drying.contains("active") && drying["active"].is_boolean()) {
                dryer_info_.active = drying["active"].get<bool>();
            }
            if (drying.contains("current_temp") && drying["current_temp"].is_number()) {
                dryer_info_.current_temp_c = drying["current_temp"].get<float>();
            }
            if (drying.contains("target_temp") && drying["target_temp"].is_number()) {
                dryer_info_.target_temp_c = drying["target_temp"].get<float>();
            }
            if (drying.contains("remaining_min") && drying["remaining_min"].is_number_integer()) {
                dryer_info_.remaining_min = drying["remaining_min"].get<int>();
            }
            if (drying.contains("duration_min") && drying["duration_min"].is_number_integer()) {
                dryer_info_.duration_min = drying["duration_min"].get<int>();
            }
            if (drying.contains("fan_pct") && drying["fan_pct"].is_number_integer()) {
                dryer_info_.fan_pct = drying["fan_pct"].get<int>();
            }
            spdlog::trace("[AMS HappyHare] Dryer state (object): active={}, temp={:.1f}°C",
                          dryer_info_.active, dryer_info_.current_temp_c);
        } else if (drying.is_array()) {
            // EMU per-gate array format: ["", "", ...] (empty = inactive)
            dryer_info_.supported = true;
            bool any_active = false;
            for (const auto& entry : drying) {
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    any_active = true;
                    break;
                }
            }
            dryer_info_.active = any_active;
            spdlog::trace("[AMS HappyHare] Dryer state (array): supported=true, active={}",
                          any_active);
        }
    }

    // Parse endless_spool_groups if available
    if (mmu_data.contains("endless_spool_groups") && mmu_data["endless_spool_groups"].is_array()) {
        const auto& es_groups = mmu_data["endless_spool_groups"];
        for (size_t i = 0; i < es_groups.size(); ++i) {
            if (es_groups[i].is_number_integer()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.endless_spool_group = es_groups[i].get<int>();
                }
            }
        }
    }
}

void AmsBackendHappyHare::initialize_slots(int gate_count) {
    spdlog::info("[AMS HappyHare] Initializing {} slots across {} units", gate_count, num_units_);

    system_info_.units.clear();

    // Determine per-unit gate counts:
    // 1. Use per_unit_gate_counts_ if available (v4 dissimilar multi-MMU)
    // 2. Fall back to even split (v3 or identical units)
    std::vector<int> unit_counts;
    if (!per_unit_gate_counts_.empty() &&
        static_cast<int>(per_unit_gate_counts_.size()) == num_units_) {
        // Verify total matches
        int total = 0;
        for (int c : per_unit_gate_counts_)
            total += c;
        if (total == gate_count) {
            unit_counts = per_unit_gate_counts_;
            spdlog::info("[AMS HappyHare] Using dissimilar per-unit gate counts");
        } else {
            spdlog::warn(
                "[AMS HappyHare] Per-unit gate counts sum ({}) != gate_count ({}), falling back",
                total, gate_count);
        }
    }

    // Fallback: even split
    if (unit_counts.empty()) {
        int gates_per_unit = (num_units_ > 1) ? (gate_count / num_units_) : gate_count;
        int remaining = gate_count;
        for (int u = 0; u < num_units_; ++u) {
            int count = (u == num_units_ - 1) ? remaining : gates_per_unit;
            unit_counts.push_back(count);
            remaining -= count;
        }
    }

    int global_offset = 0;
    for (int u = 0; u < num_units_; ++u) {
        int unit_gates = unit_counts[u];

        AmsUnit unit;
        unit.unit_index = u;
        if (num_units_ > 1) {
            unit.name = fmt::format("MMU Unit {}", u + 1);
        } else {
            unit.name = "Happy Hare MMU";
        }
        unit.slot_count = unit_gates;
        unit.first_slot_global_index = global_offset;
        unit.connected = true;
        unit.has_encoder = !is_type_b();
        unit.has_toolhead_sensor = true;
        unit.topology = is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
        // has_slot_sensors starts false; updated when sensor data arrives in parse_mmu_state()
        unit.has_slot_sensors = false;
        unit.has_hub_sensor = true; // HH selector functions as hub equivalent

        for (int i = 0; i < unit_gates; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = global_offset + i;
            slot.status = SlotStatus::UNKNOWN;
            slot.mapped_tool = global_offset + i;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        global_offset += unit_gates;
    }

    system_info_.total_slots = gate_count;

    // Initialize tool-to-gate mapping (1:1 default)
    system_info_.tool_to_slot_map.clear();
    system_info_.tool_to_slot_map.reserve(gate_count);
    for (int i = 0; i < gate_count; ++i) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    // Initialize SlotRegistry alongside legacy state (uses same unit_counts)
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> sr_units;
        int sr_offset = 0;
        for (int u = 0; u < num_units_; ++u) {
            int count = unit_counts[u];
            std::vector<std::string> names;
            for (int g = 0; g < count; ++g) {
                names.push_back(std::to_string(sr_offset + g));
            }
            std::string unit_name = "Unit " + std::to_string(u + 1);
            if (num_units_ == 1) {
                unit_name = "MMU";
            }
            sr_units.push_back({unit_name, names});
            sr_offset += count;
        }
        slots_.initialize_units(sr_units);
    }
}

void AmsBackendHappyHare::query_tip_method_from_config() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.mmu to read form_tip_macro.
    // Happy Hare uses the same logic internally: if the macro name contains "cut",
    // it's a cutter system (e.g., _MMU_CUT_TIP). Otherwise it's tip-forming.
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (system_info_, emit_event)
            // to main thread.
            token.defer("AmsBackendHappyHare::tip_method_apply", [this, response =
                                                                            std::move(response)]() {
                try {
                    const auto& settings = response["result"]["status"]["configfile"]["settings"];

                    if (!settings.contains("mmu") || !settings["mmu"].is_object()) {
                        spdlog::debug("[AMS HappyHare] No mmu section in configfile settings");
                        return;
                    }

                    const auto& mmu_cfg = settings["mmu"];
                    TipMethod method = TipMethod::NONE;

                    if (mmu_cfg.contains("form_tip_macro") &&
                        mmu_cfg["form_tip_macro"].is_string()) {
                        std::string macro = mmu_cfg["form_tip_macro"].get<std::string>();

                        // Convert to lowercase for comparison (same as Happy Hare)
                        std::string lower_macro = macro;
                        for (auto& c : lower_macro) {
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }

                        if (lower_macro.find("cut") != std::string::npos) {
                            method = TipMethod::CUT;
                        } else {
                            method = TipMethod::TIP_FORM;
                        }

                        spdlog::info(
                            "[AMS HappyHare] Tip method from config: {} (form_tip_macro={})",
                            tip_method_to_string(method), macro);
                    } else {
                        // No form_tip_macro configured — default to tip-forming
                        // (Happy Hare default macro is _MMU_FORM_TIP, not a cutter)
                        method = TipMethod::TIP_FORM;
                        spdlog::info("[AMS HappyHare] No form_tip_macro in config, defaulting "
                                     "to TIP_FORM");
                    }

                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        system_info_.tip_method = method;
                    }

                    emit_event(EVENT_STATE_CHANGED);
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for tip method: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for tip method: {}",
                         err.message);
        });
}

void AmsBackendHappyHare::query_selector_type_from_config() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.mmu_machine to read selector_type.
    // VirtualSelector = Type B (hub topology: 3MS, Box Turtle, Night Owl, Angry Beaver)
    // LinearSelector/RotarySelector/ServoSelector = Type A (linear: ERCF, Tradrack)
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (selector_type_,
            // update_unit_topologies, emit_event) to main thread.
            token.defer("AmsBackendHappyHare::selector_type_apply", [this, response = std::move(
                                                                               response)]() {
                try {
                    const auto& settings = response["result"]["status"]["configfile"]["settings"];

                    if (!settings.contains("mmu_machine") || !settings["mmu_machine"].is_object()) {
                        spdlog::debug(
                            "[AMS HappyHare] No mmu_machine section in configfile settings");
                        return;
                    }

                    const auto& mmu_machine = settings["mmu_machine"];
                    if (mmu_machine.contains("selector_type") &&
                        mmu_machine["selector_type"].is_string()) {
                        std::string type = mmu_machine["selector_type"].get<std::string>();
                        spdlog::info("[AMS HappyHare] Selector type from config: {}", type);

                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            selector_type_ = type;
                            update_unit_topologies();
                        }

                        emit_event(EVENT_STATE_CHANGED);
                    }
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for selector type: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for selector type: {}",
                         err.message);
        });
}

// ============================================================================
// Config Defaults Query
// ============================================================================

void AmsBackendHappyHare::query_config_defaults() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.mmu for initial values of speeds and distances.
    // These serve as defaults until the user overrides them via the UI.
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (config_defaults_,
            // load_persisted_overrides, reapply_overrides) to main thread.
            token.defer("AmsBackendHappyHare::config_defaults_apply", [this, response = std::move(
                                                                                 response)]() {
                try {
                    const auto& settings = response["result"]["status"]["configfile"]["settings"];

                    if (!settings.contains("mmu") || !settings["mmu"].is_object()) {
                        spdlog::debug("[AMS HappyHare] No mmu section in configfile for defaults");
                        return;
                    }

                    const auto& mmu = settings["mmu"];

                    // Helper to parse a float from config (values are strings in configfile)
                    auto parse_float = [&](const char* key, float& out) {
                        if (mmu.contains(key) && mmu[key].is_string()) {
                            try {
                                out = std::stof(mmu[key].get<std::string>());
                            } catch (...) {
                                // Keep default
                            }
                        }
                    };

                    // Helper to parse an int from config
                    auto parse_int = [&](const char* key, int& out) {
                        if (mmu.contains(key) && mmu[key].is_string()) {
                            try {
                                out = std::stoi(mmu[key].get<std::string>());
                            } catch (...) {
                                // Keep default
                            }
                        }
                    };

                    {
                        std::lock_guard<std::mutex> lock(mutex_);

                        parse_float("gear_from_buffer_speed",
                                    config_defaults_.gear_from_buffer_speed);
                        parse_float("gear_from_spool_speed",
                                    config_defaults_.gear_from_spool_speed);
                        parse_float("gear_unload_speed", config_defaults_.gear_unload_speed);
                        parse_float("selector_move_speed", config_defaults_.selector_move_speed);
                        parse_float("extruder_load_speed", config_defaults_.extruder_load_speed);
                        parse_float("extruder_unload_speed",
                                    config_defaults_.extruder_unload_speed);
                        parse_float("toolhead_sensor_to_nozzle",
                                    config_defaults_.toolhead_sensor_to_nozzle);
                        parse_float("toolhead_extruder_to_nozzle",
                                    config_defaults_.toolhead_extruder_to_nozzle);
                        parse_float("toolhead_entry_to_extruder",
                                    config_defaults_.toolhead_entry_to_extruder);
                        parse_float("toolhead_ooze_reduction",
                                    config_defaults_.toolhead_ooze_reduction);
                        parse_int("sync_to_extruder", config_defaults_.sync_to_extruder);
                        parse_int("clog_detection", config_defaults_.clog_detection);

                        config_defaults_.loaded = true;

                        spdlog::info("[AMS HappyHare] Config defaults loaded: "
                                     "gear_buf={}, gear_spool={}, gear_unload={}, "
                                     "ext_load={}, ext_unload={}, "
                                     "sensor_to_nozzle={}, extruder_to_nozzle={}",
                                     config_defaults_.gear_from_buffer_speed,
                                     config_defaults_.gear_from_spool_speed,
                                     config_defaults_.gear_unload_speed,
                                     config_defaults_.extruder_load_speed,
                                     config_defaults_.extruder_unload_speed,
                                     config_defaults_.toolhead_sensor_to_nozzle,
                                     config_defaults_.toolhead_extruder_to_nozzle);
                    }

                    load_persisted_overrides();
                    reapply_overrides();
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for defaults: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for defaults: {}",
                         err.message);
        });
}

void AmsBackendHappyHare::load_persisted_overrides() {
    auto* config = helix::Config::get_instance();
    if (!config)
        return;

    // Helper: load a float override if config_default matches current default
    auto load_float = [&](const std::string& key, std::optional<float>& field,
                          float current_default) {
        std::string base = "/hh_overrides/" + key;
        if (!config->exists(base + "/value"))
            return;
        try {
            float saved_default = config->get<float>(base + "/config_default", -999.0f);
            if (std::abs(saved_default - current_default) < 0.01f) {
                field = config->get<float>(base + "/value");
                spdlog::debug("[AMS HappyHare] Loaded override {}: {}", key, *field);
            } else {
                spdlog::info("[AMS HappyHare] Dropping stale override {} "
                             "(config changed: {} -> {})",
                             key, saved_default, current_default);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[AMS HappyHare] Failed to load override {}: {}", key, e.what());
        }
    };

    // Helper: load an int override
    auto load_int = [&](const std::string& key, std::optional<int>& field, int current_default) {
        std::string base = "/hh_overrides/" + key;
        if (!config->exists(base + "/value"))
            return;
        try {
            int saved_default = config->get<int>(base + "/config_default", -999);
            if (saved_default == current_default) {
                field = config->get<int>(base + "/value");
                spdlog::debug("[AMS HappyHare] Loaded override {}: {}", key, *field);
            } else {
                spdlog::info("[AMS HappyHare] Dropping stale override {} "
                             "(config changed: {} -> {})",
                             key, saved_default, current_default);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[AMS HappyHare] Failed to load override {}: {}", key, e.what());
        }
    };

    load_float("gear_from_buffer_speed", user_overrides_.gear_from_buffer_speed,
               config_defaults_.gear_from_buffer_speed);
    load_float("gear_from_spool_speed", user_overrides_.gear_from_spool_speed,
               config_defaults_.gear_from_spool_speed);
    load_float("gear_unload_speed", user_overrides_.gear_unload_speed,
               config_defaults_.gear_unload_speed);
    load_float("selector_move_speed", user_overrides_.selector_move_speed,
               config_defaults_.selector_move_speed);
    load_float("extruder_load_speed", user_overrides_.extruder_load_speed,
               config_defaults_.extruder_load_speed);
    load_float("extruder_unload_speed", user_overrides_.extruder_unload_speed,
               config_defaults_.extruder_unload_speed);
    load_float("toolhead_sensor_to_nozzle", user_overrides_.toolhead_sensor_to_nozzle,
               config_defaults_.toolhead_sensor_to_nozzle);
    load_float("toolhead_extruder_to_nozzle", user_overrides_.toolhead_extruder_to_nozzle,
               config_defaults_.toolhead_extruder_to_nozzle);
    load_float("toolhead_entry_to_extruder", user_overrides_.toolhead_entry_to_extruder,
               config_defaults_.toolhead_entry_to_extruder);
    load_float("toolhead_ooze_reduction", user_overrides_.toolhead_ooze_reduction,
               config_defaults_.toolhead_ooze_reduction);
    load_int("sync_to_extruder", user_overrides_.sync_to_extruder,
             config_defaults_.sync_to_extruder);
    load_int("clog_detection", user_overrides_.clog_detection, config_defaults_.clog_detection);
}

/// Helper to get the config default float for a given action key
float AmsBackendHappyHare::get_config_default_float(const std::string& key) const {
    if (key == "gear_from_buffer_speed")
        return config_defaults_.gear_from_buffer_speed;
    if (key == "gear_from_spool_speed")
        return config_defaults_.gear_from_spool_speed;
    if (key == "gear_unload_speed")
        return config_defaults_.gear_unload_speed;
    if (key == "selector_move_speed" || key == "selector_speed")
        return config_defaults_.selector_move_speed;
    if (key == "extruder_load_speed")
        return config_defaults_.extruder_load_speed;
    if (key == "extruder_unload_speed")
        return config_defaults_.extruder_unload_speed;
    if (key == "toolhead_sensor_to_nozzle")
        return config_defaults_.toolhead_sensor_to_nozzle;
    if (key == "toolhead_extruder_to_nozzle")
        return config_defaults_.toolhead_extruder_to_nozzle;
    if (key == "toolhead_entry_to_extruder")
        return config_defaults_.toolhead_entry_to_extruder;
    if (key == "toolhead_ooze_reduction")
        return config_defaults_.toolhead_ooze_reduction;
    return 0.0f;
}

/// Helper to get the config default int for a given action key
int AmsBackendHappyHare::get_config_default_int(const std::string& key) const {
    if (key == "sync_to_extruder")
        return config_defaults_.sync_to_extruder;
    if (key == "clog_detection")
        return config_defaults_.clog_detection;
    return 0;
}

void AmsBackendHappyHare::save_override(const std::string& key, float value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Update in-memory override
        if (key == "gear_from_buffer_speed")
            user_overrides_.gear_from_buffer_speed = value;
        else if (key == "gear_from_spool_speed")
            user_overrides_.gear_from_spool_speed = value;
        else if (key == "gear_unload_speed")
            user_overrides_.gear_unload_speed = value;
        else if (key == "selector_move_speed" || key == "selector_speed")
            user_overrides_.selector_move_speed = value;
        else if (key == "extruder_load_speed")
            user_overrides_.extruder_load_speed = value;
        else if (key == "extruder_unload_speed")
            user_overrides_.extruder_unload_speed = value;
        else if (key == "toolhead_sensor_to_nozzle")
            user_overrides_.toolhead_sensor_to_nozzle = value;
        else if (key == "toolhead_extruder_to_nozzle")
            user_overrides_.toolhead_extruder_to_nozzle = value;
        else if (key == "toolhead_entry_to_extruder")
            user_overrides_.toolhead_entry_to_extruder = value;
        else if (key == "toolhead_ooze_reduction")
            user_overrides_.toolhead_ooze_reduction = value;
    } // end mutex scope

    // Persist to Config JSON (outside lock — disk I/O)
    auto* config = helix::Config::get_instance();
    if (!config)
        return;
    std::string base = "/hh_overrides/" + key;
    config->set<float>(base + "/value", value);
    config->set<float>(base + "/config_default", get_config_default_float(key));
    config->save();
}

void AmsBackendHappyHare::save_override(const std::string& key, int value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (key == "sync_to_extruder")
            user_overrides_.sync_to_extruder = value;
        else if (key == "clog_detection")
            user_overrides_.clog_detection = value;
    } // end mutex scope

    // Persist to Config JSON (outside lock — disk I/O)
    auto* config = helix::Config::get_instance();
    if (!config)
        return;
    std::string base = "/hh_overrides/" + key;
    config->set<int>(base + "/value", value);
    config->set<int>(base + "/config_default", get_config_default_int(key));
    config->save();
}

void AmsBackendHappyHare::reapply_overrides() {
    // Build a single MMU_TEST_CONFIG command with all active overrides
    std::string cmd = "MMU_TEST_CONFIG";
    bool has_params = false;

    // Helper to append a float parameter
    auto append_float = [&](const char* param, const std::optional<float>& val, bool integer_fmt) {
        if (val.has_value()) {
            if (integer_fmt)
                cmd += fmt::format(" {}={:.0f}", param, *val);
            else
                cmd += fmt::format(" {}={:.1f}", param, *val);
            has_params = true;
        }
    };

    // Helper to append an int parameter
    auto append_int = [&](const char* param, const std::optional<int>& val) {
        if (val.has_value()) {
            cmd += fmt::format(" {}={}", param, *val);
            has_params = true;
        }
    };

    // Speed sliders (integer format)
    append_float("GEAR_FROM_BUFFER_SPEED", user_overrides_.gear_from_buffer_speed, true);
    append_float("GEAR_FROM_SPOOL_SPEED", user_overrides_.gear_from_spool_speed, true);
    append_float("GEAR_UNLOAD_SPEED", user_overrides_.gear_unload_speed, true);
    append_float("SELECTOR_MOVE_SPEED", user_overrides_.selector_move_speed, true);
    append_float("EXTRUDER_LOAD_SPEED", user_overrides_.extruder_load_speed, true);
    append_float("EXTRUDER_UNLOAD_SPEED", user_overrides_.extruder_unload_speed, true);

    // Toolhead distances (one decimal)
    append_float("TOOLHEAD_SENSOR_TO_NOZZLE", user_overrides_.toolhead_sensor_to_nozzle, false);
    append_float("TOOLHEAD_EXTRUDER_TO_NOZZLE", user_overrides_.toolhead_extruder_to_nozzle, false);
    append_float("TOOLHEAD_ENTRY_TO_EXTRUDER", user_overrides_.toolhead_entry_to_extruder, false);
    append_float("TOOLHEAD_OOZE_REDUCTION", user_overrides_.toolhead_ooze_reduction, false);

    // Int toggles
    append_int("SYNC_TO_EXTRUDER", user_overrides_.sync_to_extruder);
    append_int("CLOG_DETECTION", user_overrides_.clog_detection);

    if (has_params) {
        spdlog::info("[AMS HappyHare] Re-applying overrides: {}", cmd);
        execute_gcode(cmd);
    }
}

// ============================================================================
// Filament Operations
// ============================================================================

// check_preconditions() provided by AmsSubscriptionBackend

AmsError AmsBackendHappyHare::validate_slot_index(int gate_index) const {
    if (!slots_.is_valid_index(gate_index)) {
        return AmsErrorHelper::invalid_slot(gate_index,
                                            slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
    }
    return AmsErrorHelper::success();
}

// execute_gcode() provided by AmsSubscriptionBackend

AmsError AmsBackendHappyHare::load_filament(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError gate_valid = validate_slot_index(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }

        // Check if slot has filament available
        const auto* entry = slots_.get(slot_index);
        if (entry && entry->info.status == SlotStatus::EMPTY) {
            return AmsErrorHelper::slot_not_available(slot_index);
        }
    }

    // Send MMU_LOAD GATE={n} command (Happy Hare uses "gate" in its API)
    std::ostringstream cmd;
    cmd << "MMU_LOAD GATE=" << slot_index;

    spdlog::info("[AMS HappyHare] Loading from slot {}", slot_index);
    return ensure_homed_then(cmd.str());
}

AmsError AmsBackendHappyHare::unload_filament(int /*slot_index*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (!system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "No filament loaded", "No filament to unload",
                            "Load filament first");
        }
    }

    spdlog::info("[AMS HappyHare] Unloading filament");
    return ensure_homed_then("MMU_UNLOAD");
}

AmsError AmsBackendHappyHare::select_slot(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError gate_valid = validate_slot_index(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }
    }

    // Send MMU_SELECT GATE={n} command (Happy Hare uses "gate" in its API)
    std::ostringstream cmd;
    cmd << "MMU_SELECT GATE=" << slot_index;

    spdlog::info("[AMS HappyHare] Selecting slot {}", slot_index);
    return execute_gcode(cmd.str());
}

AmsError AmsBackendHappyHare::change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (tool_number < 0 ||
            tool_number >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "Select a valid tool");
        }
    }

    // Send T{n} command for standard tool change
    std::ostringstream cmd;
    cmd << "T" << tool_number;

    spdlog::info("[AMS HappyHare] Tool change to T{}", tool_number);
    return ensure_homed_then(cmd.str());
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendHappyHare::recover() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }
    }

    spdlog::info("[AMS HappyHare] Initiating recovery");
    return execute_gcode("MMU_RECOVER");
}

AmsError AmsBackendHappyHare::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }
    }

    // Happy Hare uses MMU_HOME to reset to a known state
    spdlog::info("[AMS HappyHare] Resetting (homing selector)");
    return execute_gcode("MMU_HOME");
}

AmsError AmsBackendHappyHare::reset_lane(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    // MMU_RECOVER with GATE parameter recovers a specific gate's state
    spdlog::info("[AMS HappyHare] Recovering gate {}", slot_index);
    return execute_gcode("MMU_RECOVER GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::eject_lane(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    // MMU_EJECT fully ejects filament from the gate so the spool can be removed.
    // If filament is loaded it acts like MMU_UNLOAD first, then ejects from gate.
    spdlog::info("[AMS HappyHare] Ejecting gate {}", slot_index);
    return execute_gcode("MMU_EJECT GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::select_gate(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    spdlog::info("[AMS HappyHare] Selecting gate {} (no load)", slot_index);
    return execute_gcode("MMU_SELECT GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::check_gate(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    spdlog::info("[AMS HappyHare] Checking gate {}", slot_index);
    return execute_gcode("MMU_CHECK_GATE GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::check_all_gates() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }
    }

    spdlog::info("[AMS HappyHare] Checking all gates");
    return execute_gcode("MMU_CHECK_GATE");
}

AmsError AmsBackendHappyHare::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        if (system_info_.action == AmsAction::IDLE) {
            return AmsErrorHelper::success(); // Nothing to cancel
        }
    }

    // MMU_PAUSE can be used to stop current operation
    spdlog::info("[AMS HappyHare] Cancelling current operation");
    return execute_gcode("MMU_PAUSE");
}

// ============================================================================
// Configuration Operations
// ============================================================================

AmsError AmsBackendHappyHare::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    int old_spoolman_id = 0;
    int old_mapped_tool = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }

        auto& slot = entry->info;

        // Capture old values BEFORE updating (needed to detect clears / remaps)
        old_spoolman_id = slot.spoolman_id;
        old_mapped_tool = slot.mapped_tool;

        // Detect whether anything actually changed
        bool changed = slot.color_name != info.color_name || slot.color_rgb != info.color_rgb ||
                       slot.material != info.material || slot.brand != info.brand ||
                       slot.spoolman_id != info.spoolman_id || slot.spool_name != info.spool_name ||
                       slot.remaining_weight_g != info.remaining_weight_g ||
                       slot.total_weight_g != info.total_weight_g ||
                       slot.nozzle_temp_min != info.nozzle_temp_min ||
                       slot.nozzle_temp_max != info.nozzle_temp_max ||
                       slot.bed_temp != info.bed_temp || slot.mapped_tool != info.mapped_tool;

        // Update local state
        slot.color_name = info.color_name;
        slot.color_rgb = info.color_rgb;
        slot.material = info.material;
        slot.brand = info.brand;
        slot.spoolman_id = info.spoolman_id;
        slot.spool_name = info.spool_name;
        slot.remaining_weight_g = info.remaining_weight_g;
        slot.total_weight_g = info.total_weight_g;
        slot.nozzle_temp_min = info.nozzle_temp_min;
        slot.nozzle_temp_max = info.nozzle_temp_max;
        slot.bed_temp = info.bed_temp;
        // Tool mapping change goes through registry so reverse maps stay consistent.
        if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
            slots_.set_tool_mapping(slot_index, info.mapped_tool);
        }

        if (changed) {
            spdlog::info("[AMS HappyHare] Updated slot {} info: {} {}", slot_index, info.material,
                         info.color_name);
        }
    }

    // Persist via MMU_GATE_MAP command (Happy Hare stores in mmu_vars.cfg automatically).
    // Skip persistence when persist=false — used by Spoolman weight polling to update
    // in-memory state without sending G-code back to firmware. Without this guard,
    // weight updates would trigger MMU_GATE_MAP → firmware status_update WebSocket
    // event → sync_from_backend → refresh_spoolman_weights → set_slot_info again,
    // creating an infinite feedback loop.
    if (persist) {
        bool has_changes = false;
        std::string cmd = fmt::format("MMU_GATE_MAP GATE={}", slot_index);

        // Color (hex format, no # prefix)
        if (info.color_rgb != 0 && info.color_rgb != AMS_DEFAULT_SLOT_COLOR) {
            cmd += fmt::format(" COLOR={:06X}", info.color_rgb & 0xFFFFFF);
            has_changes = true;
        }

        // Material (validate to prevent command injection)
        if (!info.material.empty() && MoonrakerAPI::is_safe_gcode_param(info.material)) {
            cmd += fmt::format(" MATERIAL={}", info.material);
            has_changes = true;
        } else if (!info.material.empty()) {
            spdlog::warn("[AMS HappyHare] Skipping MATERIAL - unsafe characters in: {}",
                         info.material);
        }

        // Spoolman ID (-1 to clear)
        if (info.spoolman_id > 0) {
            cmd += fmt::format(" SPOOLID={}", info.spoolman_id);
            has_changes = true;
        } else if (info.spoolman_id == 0 && old_spoolman_id > 0) {
            cmd += " SPOOLID=-1"; // Clear existing link
            has_changes = true;
        }

        // Only send command if there are actual changes to persist
        if (has_changes) {
            execute_gcode(cmd);
            spdlog::debug("[AMS HappyHare] Sent: {}", cmd);
        }

        // Tool-to-gate mapping is a separate Happy Hare concern from MMU_GATE_MAP
        // (which is filament metadata). Emit MMU_TTG_MAP whenever the slot edit
        // path changes mapped_tool — mirrors set_tool_mapping() for the modal flow.
        if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
            execute_gcode(fmt::format("MMU_TTG_MAP TOOL={} GATE={}", info.mapped_tool, slot_index));
        }
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));

    return AmsErrorHelper::success();
}

AmsError AmsBackendHappyHare::set_tool_mapping(int tool_number, int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tool_number < 0 ||
            tool_number >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "");
        }

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }

        // Check if another tool already maps to this slot
        for (size_t i = 0; i < system_info_.tool_to_slot_map.size(); ++i) {
            if (i != static_cast<size_t>(tool_number) &&
                system_info_.tool_to_slot_map[i] == slot_index) {
                spdlog::warn("[AMS HappyHare] Tool {} will share slot {} with tool {}", tool_number,
                             slot_index, i);
                break;
            }
        }
    }

    // Send MMU_TTG_MAP command to update tool-to-gate mapping (Happy Hare uses "gate" in its API)
    std::ostringstream cmd;
    cmd << "MMU_TTG_MAP TOOL=" << tool_number << " GATE=" << slot_index;

    spdlog::info("[AMS HappyHare] Mapping T{} to slot {}", tool_number, slot_index);
    return execute_gcode(cmd.str());
}

// ============================================================================
// Bypass Mode Operations
// ============================================================================

AmsError AmsBackendHappyHare::enable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (!system_info_.supports_bypass) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            "This Happy Hare system does not support bypass mode", "");
        }
    }

    // Happy Hare uses MMU_SELECT_BYPASS to select bypass
    spdlog::info("[AMS HappyHare] Enabling bypass mode");
    return execute_gcode("MMU_SELECT_BYPASS");
}

AmsError AmsBackendHappyHare::disable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        if (system_info_.current_slot != -2) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not active",
                            "Bypass mode is not currently active", "");
        }
    }

    // To disable bypass, select a gate or unload
    // MMU_SELECT GATE=0 or MMU_HOME will deselect bypass
    spdlog::info("[AMS HappyHare] Disabling bypass mode (homing selector)");
    return execute_gcode("MMU_HOME");
}

bool AmsBackendHappyHare::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot == -2;
}

// ============================================================================
// Endless Spool Operations (Read-Only)
// ============================================================================

helix::printer::EndlessSpoolCapabilities
AmsBackendHappyHare::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Happy Hare uses group-based endless spool configured in mmu_vars.cfg
    // UI can read but not modify the configuration
    return {true, false, "Happy Hare group-based"};
}

std::vector<helix::printer::EndlessSpoolConfig>
AmsBackendHappyHare::get_endless_spool_config() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<helix::printer::EndlessSpoolConfig> configs;

    if (!slots_.is_initialized()) {
        return configs;
    }

    // Iterate through all slots and find backup slots based on endless_spool_group
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const auto* entry = slots_.get(i);
        if (!entry) {
            continue;
        }

        helix::printer::EndlessSpoolConfig config;
        config.slot_index = entry->info.global_index;
        config.backup_slot = -1; // Default: no backup

        if (entry->info.endless_spool_group >= 0) {
            // Find another slot in the same group
            for (int j = 0; j < slots_.slot_count(); ++j) {
                if (j == i) {
                    continue;
                }
                const auto* other = slots_.get(j);
                if (other && other->info.endless_spool_group == entry->info.endless_spool_group) {
                    config.backup_slot = other->info.global_index;
                    break; // Use first match
                }
            }
        }
        configs.push_back(config);
    }

    return configs;
}

AmsError AmsBackendHappyHare::set_endless_spool_backup(int slot_index, int backup_slot) {
    // Happy Hare endless spool is configured in mmu_vars.cfg, not via runtime G-code
    (void)slot_index;
    (void)backup_slot;
    return AmsErrorHelper::not_supported("Endless spool configuration");
}

AmsError AmsBackendHappyHare::reset_tool_mappings() {
    spdlog::info("[AMS HappyHare] Resetting tool mappings to 1:1");

    int tool_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_count = static_cast<int>(system_info_.tool_to_slot_map.size());
    }

    // Reset to 1:1 mapping (T0→Gate0, T1→Gate1, etc.)
    // Continue on failure to reset as many as possible, return first error
    AmsError first_error = AmsErrorHelper::success();
    for (int tool = 0; tool < tool_count; tool++) {
        AmsError result = set_tool_mapping(tool, tool);
        if (!result.success()) {
            spdlog::error("[AMS HappyHare] Failed to reset tool {} mapping: {}", tool,
                          result.technical_msg);
            if (first_error.success()) {
                first_error = result;
            }
        }
    }

    return first_error;
}

AmsError AmsBackendHappyHare::reset_endless_spool() {
    // Happy Hare endless spool is read-only (configured in mmu_vars.cfg)
    spdlog::warn("[AMS HappyHare] Endless spool reset not supported (read-only)");
    return AmsErrorHelper::not_supported("Happy Hare endless spool is read-only");
}

// ============================================================================
// Tool Mapping Operations
// ============================================================================

helix::printer::ToolMappingCapabilities AmsBackendHappyHare::get_tool_mapping_capabilities() const {
    // Happy Hare supports tool-to-gate mapping via MMU_TTG_MAP G-code
    return {true, true, "Tool-to-gate mapping via MMU_TTG_MAP"};
}

std::vector<int> AmsBackendHappyHare::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

// ============================================================================
// Dryer Control (v4 - KMS/EMU hardware)
// ============================================================================

DryerInfo AmsBackendHappyHare::get_dryer_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dryer_info_;
}

AmsError AmsBackendHappyHare::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)unit;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dryer_info_.supported) {
            return AmsErrorHelper::not_supported("Dryer not available on this hardware");
        }
    }

    std::string cmd = fmt::format("MMU_HEATER DRY=1 TEMP={:.0f} DURATION={}", temp_c, duration_min);
    if (fan_pct >= 0) {
        cmd += fmt::format(" FAN={}", fan_pct);
    }

    spdlog::info("[AMS HappyHare] Starting dryer: {:.0f}°C for {} min", temp_c, duration_min);
    return execute_gcode(cmd);
}

AmsError AmsBackendHappyHare::stop_drying(int unit) {
    (void)unit;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dryer_info_.supported) {
            return AmsErrorHelper::not_supported("Dryer not available on this hardware");
        }
    }

    spdlog::info("[AMS HappyHare] Stopping dryer");
    return execute_gcode("MMU_HEATER DRY=0");
}

// ============================================================================
// Device Management
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendHappyHare::get_device_sections() const {
    return helix::printer::hh_default_sections();
}

std::vector<helix::printer::DeviceAction> AmsBackendHappyHare::get_device_actions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    using namespace helix::printer;
    auto actions = hh_default_actions();

    // If config hasn't loaded yet, disable all non-button actions
    if (!config_defaults_.loaded) {
        for (auto& a : actions) {
            if (a.type != ActionType::BUTTON) {
                a.enabled = false;
                a.disable_reason = "Loading configuration...";
            }
        }
        return actions;
    }

    // Helper: effective float value (user override > config default)
    auto eff_f = [&](const std::optional<float>& ovr, float def) -> double {
        return static_cast<double>(ovr.value_or(def));
    };
    auto eff_i = [&](const std::optional<int>& ovr, int def) -> int { return ovr.value_or(def); };

    // Overlay effective values onto actions
    for (auto& a : actions) {
        // --- Speed sliders ---
        if (a.id == "gear_from_buffer_speed") {
            a.current_value = eff_f(user_overrides_.gear_from_buffer_speed,
                                    config_defaults_.gear_from_buffer_speed);
        } else if (a.id == "gear_from_spool_speed") {
            a.current_value = eff_f(user_overrides_.gear_from_spool_speed,
                                    config_defaults_.gear_from_spool_speed);
        } else if (a.id == "gear_unload_speed") {
            a.current_value =
                eff_f(user_overrides_.gear_unload_speed, config_defaults_.gear_unload_speed);
        } else if (a.id == "selector_speed") {
            a.current_value =
                eff_f(user_overrides_.selector_move_speed, config_defaults_.selector_move_speed);
        } else if (a.id == "extruder_load_speed") {
            a.current_value =
                eff_f(user_overrides_.extruder_load_speed, config_defaults_.extruder_load_speed);
        } else if (a.id == "extruder_unload_speed") {
            a.current_value = eff_f(user_overrides_.extruder_unload_speed,
                                    config_defaults_.extruder_unload_speed);
        }
        // --- Toolhead sliders ---
        else if (a.id == "toolhead_sensor_to_nozzle") {
            a.current_value = eff_f(user_overrides_.toolhead_sensor_to_nozzle,
                                    config_defaults_.toolhead_sensor_to_nozzle);
        } else if (a.id == "toolhead_extruder_to_nozzle") {
            a.current_value = eff_f(user_overrides_.toolhead_extruder_to_nozzle,
                                    config_defaults_.toolhead_extruder_to_nozzle);
        } else if (a.id == "toolhead_entry_to_extruder") {
            a.current_value = eff_f(user_overrides_.toolhead_entry_to_extruder,
                                    config_defaults_.toolhead_entry_to_extruder);
        } else if (a.id == "toolhead_ooze_reduction") {
            a.current_value = eff_f(user_overrides_.toolhead_ooze_reduction,
                                    config_defaults_.toolhead_ooze_reduction);
        }
        // --- Accessories ---
        else if (a.id == "espooler_mode") {
            // Status-backed: use live value if available
            if (!espooler_active_.empty()) {
                a.current_value = espooler_active_;
            }
        } else if (a.id == "clog_detection") {
            // Status-backed flowguard_encoder_mode_ takes priority, else override/config
            int mode = (flowguard_encoder_mode_ >= 0)
                           ? flowguard_encoder_mode_
                           : eff_i(user_overrides_.clog_detection, config_defaults_.clog_detection);
            // Map int to display string
            switch (mode) {
            case 1:
                a.current_value = std::string("Manual");
                break;
            case 2:
                a.current_value = std::string("Auto");
                break;
            default:
                a.current_value = std::string("Off");
                break;
            }
        } else if (a.id == "sync_to_extruder") {
            int val = eff_i(user_overrides_.sync_to_extruder, config_defaults_.sync_to_extruder);
            a.current_value = (val != 0);
        }
        // --- Setup ---
        else if (a.id == "led_mode") {
            // Status-backed: use live LED exit_effect if available
            if (!led_exit_effect_.empty()) {
                a.current_value = led_exit_effect_;
            }
        }
    }

    // --- Topology filtering (Type B = hub-based, no servo/selector/encoder) ---
    if (is_type_b()) {
        for (auto& a : actions) {
            if (a.id == "calibrate_encoder" || a.id == "servo_buzz") {
                a.enabled = false;
                a.disable_reason = "Not available on hub-based (Type B) systems";
            } else if (a.id == "selector_speed") {
                a.enabled = false;
                a.disable_reason = "No selector on hub-based systems";
            } else if (a.id == "clog_detection") {
                a.enabled = false;
                a.disable_reason = "Encoder-based clog detection not available on Type B";
            }
        }
    }

    return actions;
}

AmsError AmsBackendHappyHare::execute_device_action(const std::string& action_id,
                                                    const std::any& value) {
    spdlog::info("[AMS HappyHare] Executing device action: {}", action_id);

    // Helper to extract a typed value from std::any with uniform error handling
    auto require_string = [&](const char* label) -> std::pair<std::string, AmsError> {
        if (!value.has_value()) {
            return {"", AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                                 "Missing value", fmt::format("Select a {}", label))};
        }
        try {
            return {std::any_cast<std::string>(value), AmsErrorHelper::success()};
        } catch (const std::bad_any_cast&) {
            return {"", AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                                 "Invalid value type", fmt::format("Select a valid {}", label))};
        }
    };

    // Helper to look up an action's min/max range from defaults
    auto get_action_range = [](const std::string& id) -> std::pair<float, float> {
        static auto defaults = helix::printer::hh_default_actions();
        for (const auto& a : defaults) {
            if (a.id == id)
                return {a.min_value, a.max_value};
        }
        return {-1e6f, 1e6f};
    };

    // Helper to extract double from std::any (UI sends doubles)
    auto require_double = [&](const char* label) -> std::pair<double, AmsError> {
        if (!value.has_value()) {
            return {0.0, AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                                  "Missing value", fmt::format("Provide a {}", label))};
        }
        try {
            return {std::any_cast<double>(value), AmsErrorHelper::success()};
        } catch (const std::bad_any_cast&) {
            return {0.0,
                    AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                             "Invalid value type", fmt::format("Provide a numeric {}", label))};
        }
    };

    // Helper to extract bool from std::any
    auto require_bool = [&](const char* label) -> std::pair<bool, AmsError> {
        if (!value.has_value()) {
            return {false, AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                                    "Missing value", fmt::format("Provide {}", label))};
        }
        try {
            return {std::any_cast<bool>(value), AmsErrorHelper::success()};
        } catch (const std::bad_any_cast&) {
            return {false,
                    AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                             "Invalid value type", fmt::format("Provide a boolean {}", label))};
        }
    };

    // --- Simple button actions (no value required) ---
    // clang-format off
    static const std::pair<const char*, const char*> button_actions[] = {
        {"calibrate_bowden",    "MMU_CALIBRATE_BOWDEN"},
        {"calibrate_encoder",   "MMU_CALIBRATE_ENCODER"},
        {"calibrate_gear",      "MMU_CALIBRATE_GEAR"},
        {"calibrate_gates",     "MMU_CALIBRATE_GATES"},
        {"test_grip",           "MMU_TEST_GRIP"},
        {"test_load",           "MMU_TEST_LOAD"},
        {"test_move",           "MMU_TEST_MOVE"},
        {"servo_buzz",          "MMU_SERVO"},
        {"reset_servo_counter", "MMU_STATS COUNTER=servo RESET=1"},
        {"reset_blade_counter", "MMU_STATS COUNTER=cutter RESET=1"},
    };
    // clang-format on
    for (const auto& [id, gcode] : button_actions) {
        if (action_id == id) {
            return execute_gcode(gcode);
        }
    }

    // --- LED mode dropdown ---
    if (action_id == "led_mode") {
        auto [mode, err] = require_string("LED mode");
        if (!err)
            return err;
        return execute_gcode("MMU_LED EXIT_EFFECT=" + mode);
    }

    // --- Speed sliders (integer formatting) ---
    // clang-format off
    static const std::pair<const char*, const char*> speed_params[] = {
        {"gear_from_buffer_speed", "GEAR_FROM_BUFFER_SPEED"},
        {"gear_from_spool_speed",  "GEAR_FROM_SPOOL_SPEED"},
        {"gear_unload_speed",      "GEAR_UNLOAD_SPEED"},
        {"selector_speed",         "SELECTOR_MOVE_SPEED"},
        {"extruder_load_speed",    "EXTRUDER_LOAD_SPEED"},
        {"extruder_unload_speed",  "EXTRUDER_UNLOAD_SPEED"},
    };
    // clang-format on
    for (const auto& [id, param] : speed_params) {
        if (action_id == id) {
            auto [speed, err] = require_double("speed");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(id);
            speed = std::clamp(speed, static_cast<double>(lo), static_cast<double>(hi));
            auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG {}={:.0f}", param, speed));
            if (result.success()) {
                save_override(action_id, static_cast<float>(speed));
            }
            return result;
        }
    }

    // --- Toolhead distance sliders (one decimal place) ---
    // clang-format off
    static const std::pair<const char*, const char*> toolhead_params[] = {
        {"toolhead_sensor_to_nozzle",   "TOOLHEAD_SENSOR_TO_NOZZLE"},
        {"toolhead_extruder_to_nozzle", "TOOLHEAD_EXTRUDER_TO_NOZZLE"},
        {"toolhead_entry_to_extruder",  "TOOLHEAD_ENTRY_TO_EXTRUDER"},
        {"toolhead_ooze_reduction",     "TOOLHEAD_OOZE_REDUCTION"},
    };
    // clang-format on
    for (const auto& [id, param] : toolhead_params) {
        if (action_id == id) {
            auto [dist, err] = require_double("distance");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(id);
            dist = std::clamp(dist, static_cast<double>(lo), static_cast<double>(hi));
            auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG {}={:.1f}", param, dist));
            if (result.success()) {
                save_override(action_id, static_cast<float>(dist));
            }
            return result;
        }
    }

    // --- sync_to_extruder toggle ---
    if (action_id == "sync_to_extruder") {
        auto [enable, err] = require_bool("sync state");
        if (!err)
            return err;
        int val = enable ? 1 : 0;
        auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG SYNC_TO_EXTRUDER={}", val));
        if (result.success()) {
            save_override(action_id, val);
        }
        return result;
    }

    // --- eSpooler mode dropdown ---
    if (action_id == "espooler_mode") {
        auto [mode, err] = require_string("eSpooler mode");
        if (!err)
            return err;
        return execute_gcode("MMU_ESPOOLER OPERATION=" + mode);
    }

    // --- Clog detection dropdown ---
    if (action_id == "clog_detection") {
        auto [mode_str, err] = require_string("clog detection mode");
        if (!err)
            return err;
        int mode_int = 0;
        if (mode_str == "Manual")
            mode_int = 1;
        else if (mode_str == "Auto")
            mode_int = 2;
        auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG CLOG_DETECTION={}", mode_int));
        if (result.success()) {
            save_override(action_id, mode_int);
        }
        return result;
    }

    // --- Motors toggle ---
    if (action_id == "motors_toggle") {
        auto [enable, err] = require_bool("motor state");
        if (!err)
            return err;
        return execute_gcode(enable ? "MMU_HOME" : "MMU_MOTORS_OFF");
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_medusahc.h"

#include "ams_error.h"
#include "moonraker_error.h"
#include "moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

using namespace helix;

namespace {
// The [medusahc] Klipper module exposes everything through one status object.
constexpr const char* kMedusaObject = "medusahc";

std::string slot_color_to_macro_hex(uint32_t color_rgb) {
    std::ostringstream os;
    os << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << (color_rgb & 0xFFFFFF);
    return os.str();
}
} // namespace

AmsBackendMedusaHc::AmsBackendMedusaHc(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    system_info_.type = AmsType::MEDUSA_HC;
    system_info_.type_name = "MedusaHC";
    system_info_.supports_tool_mapping = true;
    system_info_.supports_bypass = false;
    system_info_.has_hardware_bypass_sensor = false;
    system_info_.tip_method = TipMethod::NONE;
    system_info_.total_slots = 0;
    system_info_.current_tool = -1;
    system_info_.current_slot = -1;
    system_info_.filament_loaded = false;
    system_info_.units.clear();
    system_info_.tool_to_slot_map.clear();
    spdlog::debug("{} Backend created", backend_log_tag());
}

AmsSystemInfo AmsBackendMedusaHc::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendMedusaHc::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }
    SlotInfo empty;
    empty.slot_index = -1;
    empty.global_index = -1;
    return empty;
}

PathTopology AmsBackendMedusaHc::get_topology() const {
    return PathTopology::PARALLEL;
}

PathSegment AmsBackendMedusaHc::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_tool >= 0 ? PathSegment::NOZZLE : PathSegment::SPOOL;
}

PathSegment AmsBackendMedusaHc::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= static_cast<int>(tools_.size())) {
        return PathSegment::NONE;
    }
    return PathSegment::NOZZLE;
}

PathSegment AmsBackendMedusaHc::infer_error_segment() const {
    return PathSegment::NONE;
}

bool AmsBackendMedusaHc::is_bypass_active() const {
    return false;
}

std::optional<int> AmsBackendMedusaHc::slot_for_extruder(int extruder_idx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (extruder_idx < 0 || extruder_idx >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
        return std::nullopt;
    }
    return system_info_.tool_to_slot_map[static_cast<size_t>(extruder_idx)];
}

helix::printer::ToolMappingCapabilities
AmsBackendMedusaHc::get_tool_mapping_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {.supported = system_info_.total_slots > 0,
            .editable = false,
            .description = "MedusaHC tools"};
}

std::vector<int> AmsBackendMedusaHc::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

AmsError AmsBackendMedusaHc::load_filament(int slot_index) {
    return change_tool(slot_index);
}

AmsError AmsBackendMedusaHc::unload_filament(int slot_index) {
    (void)slot_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
    }
    emit_event(EVENT_STATE_CHANGED);
    return execute_gcode("DROP_TOOL");
}

AmsError AmsBackendMedusaHc::select_slot(int slot_index) {
    return change_tool(slot_index);
}

AmsError AmsBackendMedusaHc::validate_slot_index_locked(int slot_index) const {
    if (system_info_.total_slots == 0) {
        return AmsErrorHelper::not_connected("No tools discovered");
    }
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
    }
    return AmsErrorHelper::success();
}

bool AmsBackendMedusaHc::needs_unload_before_load(const AmsSystemInfo& /*info*/) const {
    // Tool-switch only — never run filament load/swap/cut sequences.
    return false;
}

AmsError AmsBackendMedusaHc::change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }
        AmsError slot_valid = validate_slot_index_locked(tool_number);
        if (!slot_valid) {
            return slot_valid;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::SELECTING;
    }
    emit_event(EVENT_STATE_CHANGED);

    spdlog::info("{} Selecting tool {} via T{}", backend_log_tag(), tool_number, tool_number);
    return execute_gcode("T" + std::to_string(tool_number));
}

AmsError AmsBackendMedusaHc::recover() {
    return AmsErrorHelper::not_supported("MedusaHC recovery");
}

AmsError AmsBackendMedusaHc::reset() {
    return execute_gcode("DROP_TOOL");
}

AmsError AmsBackendMedusaHc::cancel() {
    return execute_gcode("DROP_TOOL");
}

AmsError AmsBackendMedusaHc::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    (void)persist;

    bool should_emit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsError slot_valid = validate_slot_index_locked(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }

        if (system_info_.units.empty() ||
            slot_index >= static_cast<int>(system_info_.units[0].slots.size())) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }

        auto& slot = system_info_.units[0].slots[static_cast<size_t>(slot_index)];
        slot.color_rgb = info.color_rgb;
        slot.color_name = info.color_name;
        slot.material = info.material;
        slot.brand = info.brand;
        slot.spoolman_id = info.spoolman_id;
        slot.spool_name = info.spool_name;
        slot.remaining_weight_g = info.remaining_weight_g;
        slot.total_weight_g = info.total_weight_g;

        if (slot_index < static_cast<int>(tools_.size())) {
            tools_[static_cast<size_t>(slot_index)].color_hex = slot_color_to_macro_hex(info.color_rgb);
        }

        sync_system_info_locked();
        should_emit = true;
    }

    if (should_emit) {
        emit_event(EVENT_STATE_CHANGED);
    }
    return AmsErrorHelper::success();
}

AmsError AmsBackendMedusaHc::set_tool_mapping(int tool_number, int slot_index) {
    (void)tool_number;
    (void)slot_index;
    return AmsErrorHelper::not_supported("Tool mapping edits");
}

AmsError AmsBackendMedusaHc::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

AmsError AmsBackendMedusaHc::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

std::vector<helix::printer::DeviceSection> AmsBackendMedusaHc::get_device_sections() const {
    return {};
}

std::vector<helix::printer::DeviceAction> AmsBackendMedusaHc::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    using AT = helix::printer::ActionType;
    return {
        DA{"open_feeder",
           "Open feeder",
           "",
           "",
           "Open the filament feeder",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
        DA{"close_feeder",
           "Close feeder",
           "",
           "",
           "Close the filament feeder",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
    };
}

AmsError AmsBackendMedusaHc::execute_device_action(const std::string& action_id,
                                                   const std::any& value) {
    (void)value;
    if (action_id == "open_feeder") {
        return execute_gcode("OPEN");
    }
    if (action_id == "close_feeder") {
        return execute_gcode("CLOSE");
    }
    return AmsErrorHelper::not_supported("Device action: " + action_id);
}

void AmsBackendMedusaHc::on_started() {
    if (!client_) {
        spdlog::warn("{} No client available for bootstrap snapshot", backend_log_tag());
        return;
    }

    // One-shot query of the medusahc object so we have full state before the
    // first notify_status_update arrives. nullptr = all fields.
    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", json{{"objects", json{{kMedusaObject, nullptr}}}},
        [this, token](nlohmann::json response) {
            token.defer("AmsBackendMedusaHc::bootstrap_status",
                        [this, response = std::move(response)]() {
                            if (response.contains("result") &&
                                response["result"].contains("status") &&
                                response["result"]["status"].is_object()) {
                                apply_status_snapshot(response["result"]["status"]);
                            }
                        });
        },
        [this](const MoonrakerError& err) {
            spdlog::debug("{} Initial medusahc query failed: {}", backend_log_tag(), err.message);
        });
}

void AmsBackendMedusaHc::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update: {"params": [{...}, timestamp]} — initial query sends bare status.
    const nlohmann::json* status = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status = &notification["params"][0];
    }
    if (!status->is_object()) {
        return;
    }
    apply_status_snapshot(*status);
}

void AmsBackendMedusaHc::apply_status_snapshot(const nlohmann::json& status) {
    if (!status.is_object()) {
        return;
    }

    auto it = status.find(kMedusaObject);
    if (it == status.end() || !it->is_object()) {
        return;
    }

    bool changed = false;
    int previous_current_tool = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous_current_tool = system_info_.current_tool;
        apply_medusahc_object_locked(*it, changed);
        sync_system_info_locked();
        changed = changed || (system_info_.current_tool != previous_current_tool);
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
        emit_event(EVENT_TOOL_CHANGED);
    }
}

void AmsBackendMedusaHc::apply_medusahc_object_locked(const nlohmann::json& obj, bool& changed) {
    // Subscriptions are incremental — only changed fields arrive. tool_count is
    // sent on the first (full) snapshot, so grow capacity before per-tool fields.
    auto count_it = obj.find("tool_count");
    if (count_it != obj.end() && count_it->is_number_integer()) {
        int tool_count = count_it->get<int>();
        if (tool_count > static_cast<int>(tools_.size())) {
            ensure_tool_capacity_locked(tool_count);
            changed = true;
        }
    }

    auto state_it = obj.find("state");
    if (state_it != obj.end() && state_it->is_string()) {
        std::string st = state_it->get<std::string>();
        if (medusahc_state_ != st) {
            medusahc_state_ = st;
            changed = true;
            spdlog::debug("{} state → {}", backend_log_tag(), medusahc_state_);
        }
    }

    auto ct_it = obj.find("current_tool");
    if (ct_it != obj.end() && ct_it->is_number_integer()) {
        int ct = ct_it->get<int>();
        if (current_tool_ != ct) {
            current_tool_ = ct;
            changed = true;
            spdlog::debug("{} current_tool → {}", backend_log_tag(), ct);
        }
    }

    auto tt_it = obj.find("target_tool");
    if (tt_it != obj.end() && tt_it->is_number_integer()) {
        target_tool_ = tt_it->get<int>();
    }

    auto err_it = obj.find("error");
    if (err_it != obj.end()) {
        bool err = parse_boolish(*err_it);
        if (error_state_ != err) {
            error_state_ = err;
            changed = true;
        }
    }

    auto feeder_it = obj.find("feeder_open");
    if (feeder_it != obj.end()) {
        bool open = parse_boolish(*feeder_it);
        if (feeder_open_ != open) {
            feeder_open_ = open;
            changed = true;
        }
    }

    // Per-tool dock sensors: medusahc.tool{N}_docked
    for (const auto& [key, value] : obj.items()) {
        auto idx = parse_tool_docked_index(key);
        if (!idx) {
            continue;
        }
        if (*idx + 1 > static_cast<int>(tools_.size())) {
            ensure_tool_capacity_locked(*idx + 1);
            changed = true;
        }
        bool docked = parse_boolish(value);
        auto& tool = tools_[static_cast<size_t>(*idx)];
        if (tool.docked != docked) {
            tool.docked = docked;
            changed = true;
        }
    }
}

void AmsBackendMedusaHc::ensure_tool_capacity_locked(int tool_count) {
    if (tool_count < 0) {
        tool_count = 0;
    }
    if (static_cast<int>(tools_.size()) < tool_count) {
        tools_.resize(static_cast<size_t>(tool_count));
    }

    if (system_info_.units.empty()) {
        system_info_.units.resize(1);
    }
    auto& unit = system_info_.units[0];
    unit.unit_index = 0;
    unit.name = "MedusaHC";
    unit.display_name = "MedusaHC";
    unit.topology = PathTopology::PARALLEL;
    unit.connected = true;
    unit.first_slot_global_index = 0;

    if (static_cast<int>(unit.slots.size()) < tool_count) {
        unit.slots.resize(static_cast<size_t>(tool_count));
    }
    unit.slot_count = tool_count;
    unit.lane_is_hub_routed.clear();
    unit.hub_tool_label = -1;
    system_info_.total_slots = tool_count;
    system_info_.tool_to_slot_map.assign(static_cast<size_t>(tool_count), -1);
}

void AmsBackendMedusaHc::sync_system_info_locked() {
    if (system_info_.units.empty()) {
        system_info_.units.resize(1);
    }
    auto& unit = system_info_.units[0];
    unit.unit_index = 0;
    unit.name = "MedusaHC";
    unit.display_name = "MedusaHC";
    unit.topology = PathTopology::PARALLEL;
    unit.connected = true;
    unit.first_slot_global_index = 0;

    // medusahc.current_tool is authoritative: -2 = sensor error, -1 = nothing on
    // head, 0..N-1 = mounted tool index.
    int active_tool = current_tool_ >= 0 ? current_tool_ : -1;

    for (size_t i = 0; i < tools_.size(); ++i) {
        auto& tool = tools_[i];
        auto& slot = unit.slots[i];
        const bool is_active = (active_tool >= 0 && static_cast<int>(i) == active_tool);
        slot.slot_index = static_cast<int>(i);
        slot.global_index = static_cast<int>(i);
        slot.mapped_tool = static_cast<int>(i);
        slot.tool_mapping_override = false;
        slot.status = is_active ? SlotStatus::LOADED : SlotStatus::AVAILABLE;
        slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        if (auto color = parse_color(tool.color_hex)) {
            slot.color_rgb = *color;
        }
        system_info_.tool_to_slot_map[static_cast<size_t>(i)] = static_cast<int>(i);
    }

    unit.slot_count = static_cast<int>(tools_.size());
    system_info_.total_slots = static_cast<int>(tools_.size());
    system_info_.supports_tool_mapping = !tools_.empty();
    system_info_.supports_bypass = false;
    system_info_.has_hardware_bypass_sensor = false;
    system_info_.type = AmsType::MEDUSA_HC;
    system_info_.type_name = "MedusaHC";
    system_info_.current_tool = active_tool;
    system_info_.current_slot = active_tool;
    system_info_.filament_loaded = (active_tool >= 0);

    // Drive the in-progress action from medusahc.state. The orchestrator reports
    // "changing" for the whole SET/DROP sequence and "ready"/"error" when it
    // settles, so any terminal state clears the spinner — never get stuck.
    // Fall back to the current_tool heuristic only when state hasn't arrived yet
    // (e.g. an incremental update that carried just current_tool).
    if (!medusahc_state_.empty()) {
        if (medusahc_state_ == "changing") {
            // Keep the optimistic SELECTING/UNLOADING the action setters chose;
            // if none is in flight, treat it as a tool selection.
            if (system_info_.action == AmsAction::IDLE) {
                system_info_.action = AmsAction::SELECTING;
            }
        } else {
            // "ready", "error", "uninitialized" → no longer in progress.
            system_info_.action = AmsAction::IDLE;
        }
    } else if ((system_info_.action == AmsAction::SELECTING && active_tool >= 0) ||
               (system_info_.action == AmsAction::UNLOADING && active_tool < 0)) {
        system_info_.action = AmsAction::IDLE;
    }
}

bool AmsBackendMedusaHc::parse_boolish(const nlohmann::json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text == "1" || text == "true" || text == "on" || text == "yes";
    }
    return false;
}

std::optional<uint32_t> AmsBackendMedusaHc::parse_color(const std::string& color) {
    if (color.empty()) {
        return std::nullopt;
    }
    std::string text = color;
    if (!text.empty() && text.front() == '#') {
        text.erase(text.begin());
    }
    if (!text.empty() && text.front() == '0' && text.size() > 1 &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.erase(0, 2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        return static_cast<uint32_t>(std::stoul(text, nullptr, 16));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> AmsBackendMedusaHc::parse_tool_docked_index(const std::string& key) {
    // Match "tool{N}_docked" → N.
    constexpr const char* kPrefix = "tool";
    constexpr const char* kSuffix = "_docked";
    const size_t prefix_len = std::char_traits<char>::length(kPrefix);
    const size_t suffix_len = std::char_traits<char>::length(kSuffix);
    if (key.size() <= prefix_len + suffix_len || key.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    if (key.compare(key.size() - suffix_len, suffix_len, kSuffix) != 0) {
        return std::nullopt;
    }
    std::string digits = key.substr(prefix_len, key.size() - prefix_len - suffix_len);
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }
    try {
        return std::stoi(digits);
    } catch (...) {
        return std::nullopt;
    }
}

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
constexpr const char* kGlobalStateMacro = "gcode_macro GLOBAL_STATE";
constexpr const char* kMacroPrefix = "gcode_macro T";
constexpr const char* kColorVariable = "variable_color";
constexpr const char* kActiveVariable = "variable_active";
constexpr const char* kMaxToolVariable = "variable_max_tool";
constexpr const char* kPinWatchCurrentTool = "current_tool";
constexpr const char* kPinWatchPrefix = "pin_watch";

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
            .description = "MedusaHC Tn macros"};
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
        if (!pin_watch_object_.empty()) {
            pin_watch_current_tool_ = -1;
            sync_system_info_locked();
        }
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
    if (!api_) {
        spdlog::warn("{} No API available for bootstrap snapshot", backend_log_tag());
        return;
    }

    auto token = lifetime_.token();
    api_->query_configfile(
        [this, token](nlohmann::json config) {
            token.defer("AmsBackendMedusaHc::bootstrap_from_config",
                        [this, config = std::move(config)]() { bootstrap_from_config(config); });
        },
        [this](const MoonrakerError& err) {
            spdlog::warn("{} Config bootstrap failed: {}", backend_log_tag(), err.message);
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

void AmsBackendMedusaHc::bootstrap_from_config(const nlohmann::json& config) {
    if (!config.is_object()) {
        return;
    }

    int max_tool = -1;
    bool saw_global = false;
    nlohmann::json query_objects = nlohmann::json::object();

    for (const auto& [key, value] : config.items()) {
        if (key.rfind(kPinWatchPrefix, 0) == 0) {
            pin_watch_object_ = key;
            query_objects[key] = nlohmann::json::array({kPinWatchCurrentTool});
            continue;
        }

        if (key == kGlobalStateMacro) {
            saw_global = true;
            query_objects[key] = nlohmann::json::array({kMaxToolVariable});
            continue;
        }

        auto idx = parse_macro_index(key);
        if (!idx) {
            continue;
        }

        max_tool = std::max(max_tool, *idx);
        query_objects[key] = nlohmann::json::array({kActiveVariable, kColorVariable});
    }

    int bootstrapped_slots = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        configured_max_tool_ = max_tool;
        saw_global_state_ = saw_global;
        ensure_tool_capacity_locked(max_tool >= 0 ? max_tool + 1 : 0);
        sync_system_info_locked();
        bootstrapped_slots = system_info_.total_slots;
    }

    if (bootstrapped_slots > 0) {
        emit_event(EVENT_STATE_CHANGED);
    }

    if (query_objects.empty() || !client_) {
        return;
    }

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", json{{"objects", std::move(query_objects)}},
        [this, token](nlohmann::json response) {
            token.defer("AmsBackendMedusaHc::bootstrap_status",
                        [this, response = std::move(response)]() {
                            if (response.contains("result") && response["result"].contains("status") &&
                                response["result"]["status"].is_object()) {
                                apply_status_snapshot(response["result"]["status"]);
                            }
                        });
        },
        [this](const MoonrakerError& err) {
            spdlog::debug("{} Initial macro query failed: {}", backend_log_tag(), err.message);
        });
}

void AmsBackendMedusaHc::apply_status_snapshot(const nlohmann::json& status) {
    if (!status.is_object()) {
        return;
    }

    bool changed = false;
    int observed_max = -1;
    int previous_current_tool = -1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous_current_tool = system_info_.current_tool;

        for (const auto& [key, value] : status.items()) {
            if (key.rfind(kPinWatchPrefix, 0) == 0) {
                if (pin_watch_object_.empty()) {
                    pin_watch_object_ = key;
                }
                if (value.is_object()) {
                    auto it = value.find(kPinWatchCurrentTool);
                    if (it != value.end() && it->is_number_integer()) {
                        int ct = it->get<int>();
                        if (pin_watch_current_tool_ != ct) {
                            pin_watch_current_tool_ = ct;
                            changed = true;
                            spdlog::debug("{} pin_watch current_tool → {}", backend_log_tag(),
                                          ct);
                        }
                    }
                }
                continue;
            }

            if (key == kGlobalStateMacro) {
                if (value.is_object()) {
                    auto it = value.find(kMaxToolVariable);
                    if (it != value.end() && it->is_number_integer()) {
                        observed_max = std::max(observed_max, it->get<int>());
                        saw_global_state_ = true;
                    }
                }
                continue;
            }

            auto idx = parse_macro_index(key);
            if (!idx) {
                continue;
            }

            observed_max = std::max(observed_max, *idx);
            if (!value.is_object()) {
                continue;
            }

            ensure_tool_capacity_locked(std::max(static_cast<int>(tools_.size()), *idx + 1));

            auto& tool = tools_[static_cast<size_t>(*idx)];
            bool tool_changed = false;

            auto active_it = value.find(kActiveVariable);
            if (active_it != value.end()) {
                bool active = parse_boolish(*active_it);
                if (tool.active != active) {
                    tool.active = active;
                    tool_changed = true;
                }
            }

            auto color_it = value.find(kColorVariable);
            if (color_it != value.end()) {
                std::string color;
                if (color_it->is_string()) {
                    color = color_it->get<std::string>();
                } else if (color_it->is_number_integer()) {
                    std::ostringstream os;
                    os << std::uppercase << std::hex << color_it->get<int>();
                    color = os.str();
                }
                if (tool.color_hex != color) {
                    tool.color_hex = std::move(color);
                    tool_changed = true;
                }
            }

            changed = changed || tool_changed;
        }

        if (observed_max >= 0 && observed_max + 1 > static_cast<int>(tools_.size())) {
            ensure_tool_capacity_locked(observed_max + 1);
            changed = true;
        }

        if (configured_max_tool_ < observed_max) {
            configured_max_tool_ = observed_max;
        }

        sync_system_info_locked();
        changed = changed || (system_info_.current_tool != previous_current_tool);
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
        emit_event(EVENT_TOOL_CHANGED);
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

    // pin_watch io.current_tool is authoritative for MedusaHC; Tn.variable_active is
    // only a Mainsail/Fluidd UI sync side-effect and may lag or be absent.
    int active_tool = -1;
    if (!pin_watch_object_.empty()) {
        if (pin_watch_current_tool_ >= 0) {
            active_tool = pin_watch_current_tool_;
        }
    } else {
        for (size_t i = 0; i < tools_.size(); ++i) {
            if (tools_[i].active && active_tool < 0) {
                active_tool = static_cast<int>(i);
            }
        }
    }

    for (size_t i = 0; i < tools_.size(); ++i) {
        auto& tool = tools_[i];
        auto& slot = unit.slots[i];
        const bool is_active = (active_tool >= 0 && static_cast<int>(i) == active_tool);
        if (!pin_watch_object_.empty()) {
            tool.active = is_active;
        }
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
    if ((system_info_.action == AmsAction::SELECTING && active_tool >= 0) ||
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

std::optional<int> AmsBackendMedusaHc::parse_macro_index(const std::string& key) {
    if (key.rfind(kMacroPrefix, 0) != 0) {
        return std::nullopt;
    }
    std::string suffix = key.substr(std::char_traits<char>::length(kMacroPrefix));
    if (suffix.empty()) {
        return std::nullopt;
    }
    if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }
    try {
        return std::stoi(suffix);
    } catch (...) {
        return std::nullopt;
    }
}

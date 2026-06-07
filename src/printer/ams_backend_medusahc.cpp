// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_medusahc.h"

#include "ams_error.h"
#include "moonraker_error.h"
#include "moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <sstream>

using namespace helix;

namespace {
constexpr const char* kGlobalStateMacro = "gcode_macro GLOBAL_STATE";
constexpr const char* kMacroPrefix = "gcode_macro T";
constexpr const char* kColorVariable = "variable_color";
constexpr const char* kActiveVariable = "variable_active";
constexpr const char* kMaxToolVariable = "variable_max_tool";
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
    return execute_gcode("UNSELECT_TOOL");
}

AmsError AmsBackendMedusaHc::select_slot(int slot_index) {
    return change_tool(slot_index);
}

AmsError AmsBackendMedusaHc::change_tool(int tool_number) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tool_number < 0 || tool_number >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(tool_number, std::max(0, system_info_.total_slots - 1));
    }
    return execute_gcode("T" + std::to_string(tool_number));
}

AmsError AmsBackendMedusaHc::recover() {
    return AmsErrorHelper::not_supported("MedusaHC recovery");
}

AmsError AmsBackendMedusaHc::reset() {
    return execute_gcode("UNSELECT_TOOL");
}

AmsError AmsBackendMedusaHc::cancel() {
    return execute_gcode("UNSELECT_TOOL");
}

AmsError AmsBackendMedusaHc::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    (void)slot_index;
    (void)info;
    (void)persist;
    return AmsErrorHelper::not_supported("Slot editing");
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
    return {};
}

AmsError AmsBackendMedusaHc::execute_device_action(const std::string& action_id,
                                                   const std::any& value) {
    (void)action_id;
    (void)value;
    return AmsErrorHelper::not_supported("Device actions");
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
    if (!notification.is_object()) {
        return;
    }
    apply_status_snapshot(notification);
}

void AmsBackendMedusaHc::bootstrap_from_config(const nlohmann::json& config) {
    if (!config.is_object()) {
        return;
    }

    int max_tool = -1;
    bool saw_global = false;
    nlohmann::json query_objects = nlohmann::json::object();

    for (const auto& [key, value] : config.items()) {
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        configured_max_tool_ = max_tool;
        saw_global_state_ = saw_global;
        ensure_tool_capacity_locked(max_tool >= 0 ? max_tool + 1 : 0);
        sync_system_info_locked();
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
        if (active_tool >= 0 && active_tool != system_info_.current_tool) {
            changed = true;
        }
    }system_info_.current_tool != previous_

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

    int active_tool = -1;
    for (size_t i = 0; i < tools_.size(); ++i) {
        auto& tool = tools_[i];
        auto& slot = unit.slots[i];
        slot.slot_index = static_cast<int>(i);
        slot.global_index = static_cast<int>(i);
        slot.mapped_tool = static_cast<int>(i);
        slot.tool_mapping_override = false;
        slot.status = tool.active ? SlotStatus::LOADED : SlotStatus::AVAILABLE;
        slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        if (auto color = parse_color(tool.color_hex)) {
            slot.color_rgb = *color;
        }
        if (tool.active && active_tool < 0) {
            active_tool = static_cast<int>(i);
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
    system_info_.action = AmsAction::IDLE;
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

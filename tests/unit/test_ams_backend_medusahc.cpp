// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_medusahc.h"
#include "ams_types.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

#include "hv/json.hpp"

#include <string>
#include <vector>

using json = nlohmann::json;
using namespace helix;

namespace {
class RecordingMedusaHcBackend : public AmsBackendMedusaHc {
  public:
    RecordingMedusaHcBackend() : AmsBackendMedusaHc(nullptr, nullptr) {}

    std::vector<std::string> sent_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        sent_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    using AmsBackendMedusaHc::handle_status_update;
};
} // namespace

TEST_CASE("MedusaHC type identification", "[ams][medusahc]") {
    REQUIRE(AmsType::MEDUSA_HC != AmsType::NONE);
    REQUIRE(static_cast<int>(AmsType::MEDUSA_HC) == 9);
    REQUIRE(is_tool_changer(AmsType::MEDUSA_HC));
    REQUIRE_FALSE(is_filament_system(AmsType::MEDUSA_HC));
    REQUIRE(std::string(ams_type_to_string(AmsType::MEDUSA_HC)) == "MedusaHC");
    REQUIRE(ams_type_from_string("medusa_hc") == AmsType::MEDUSA_HC);
    REQUIRE(ams_type_from_string("medusahc") == AmsType::MEDUSA_HC);
}

TEST_CASE("MedusaHC config discovery", "[ams][medusahc][discovery]") {
    PrinterDiscovery discovery;
    discovery.parse_config_keys(json{{"pin_watch", json::object()}});

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::MEDUSA_HC);

    const auto& systems = discovery.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::MEDUSA_HC);
    REQUIRE(systems[0].name == "MedusaHC");
}

TEST_CASE("MedusaHC parses gcode_macro Tn status", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(json{
        {"gcode_macro GLOBAL_STATE", json{{"variable_max_tool", 1}}},
        {"gcode_macro T0", json{{"variable_active", 1}, {"variable_color", "FF0000"}}},
        {"gcode_macro T1", json{{"variable_active", 0}, {"variable_color", "00FF00"}}},
    });

    auto info = backend.get_system_info();
    REQUIRE(info.type == AmsType::MEDUSA_HC);
    REQUIRE(info.total_slots == 2);
    REQUIRE(info.current_tool == 0);
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.filament_loaded);
    REQUIRE(info.supports_tool_mapping);
    REQUIRE(info.tool_to_slot_map.size() == 2);
    REQUIRE(info.tool_to_slot_map[0] == 0);
    REQUIRE(info.tool_to_slot_map[1] == 1);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 2);
    REQUIRE(info.units[0].slots[0].status == SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFF0000);
    REQUIRE(info.units[0].slots[1].color_rgb == 0x00FF00);
}

TEST_CASE("MedusaHC change_tool emits Tn", "[ams][medusahc][gcode]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(json{
        {"gcode_macro GLOBAL_STATE", json{{"variable_max_tool", 2}}},
        {"gcode_macro T0", json{{"variable_active", 0}, {"variable_color", ""}}},
        {"gcode_macro T1", json{{"variable_active", 0}, {"variable_color", ""}}},
        {"gcode_macro T2", json{{"variable_active", 0}, {"variable_color", ""}}},
    });

    auto result = backend.change_tool(2);
    REQUIRE(result);
    REQUIRE(backend.sent_gcodes.size() == 1);
    REQUIRE(backend.sent_gcodes[0] == "T2");
}

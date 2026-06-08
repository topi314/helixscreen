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

    void set_running_for_test() {
        running_ = true;
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    using AmsBackendMedusaHc::bootstrap_from_config;
    using AmsBackendMedusaHc::handle_status_update;
};

namespace {
json medusahc_test_config(int max_tool) {
    json cfg = json{{"pin_watch io", json::object()},
                    {"gcode_macro GLOBAL_STATE", json{{"variable_max_tool", max_tool}}}};
    for (int i = 0; i <= max_tool; ++i) {
        cfg["gcode_macro T" + std::to_string(i)] = json::object();
    }
    return cfg;
}
} // namespace
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

TEST_CASE("MedusaHC objects.list discovery via Tn macros", "[ams][medusahc][discovery]") {
    PrinterDiscovery discovery;
    discovery.parse_objects(json::array({"gcode_macro GLOBAL_STATE", "gcode_macro T0",
                                         "gcode_macro T1", "extruder", "toolhead"}));

    const auto& systems = discovery.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::MEDUSA_HC);
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

TEST_CASE("MedusaHC unwraps notify_status_update params", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(json{
        {"params",
         json::array({json{{"gcode_macro T0", json{{"variable_active", 1}, {"variable_color", "AABBCC"}}}},
                      0})}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 1);
    REQUIRE(info.current_tool == 0);
    REQUIRE(info.units[0].slots[0].color_rgb == 0xAABBCC);
}

TEST_CASE("MedusaHC is tool-switch only (no load/swap preheat path)", "[ams][medusahc]") {
    RecordingMedusaHcBackend backend;
    backend.handle_status_update(json{
        {"gcode_macro T0", json{{"variable_active", 1}, {"variable_color", ""}}},
    });

    AmsSystemInfo info = backend.get_system_info();
    REQUIRE_FALSE(backend.needs_unload_before_load(info));
}

TEST_CASE("MedusaHC uses pin_watch current_tool for active slot", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;
    backend.bootstrap_from_config(medusahc_test_config(2));

    backend.handle_status_update(json{
        {"gcode_macro GLOBAL_STATE", json{{"variable_max_tool", 2}}},
        {"gcode_macro T0", json{{"variable_active", 0}, {"variable_color", "FF0000"}}},
        {"gcode_macro T1", json{{"variable_active", 0}, {"variable_color", "00FF00"}}},
        {"gcode_macro T2", json{{"variable_active", 0}, {"variable_color", "0000FF"}}},
        {"pin_watch io", json{{"current_tool", 2}}},
    });

    auto info = backend.get_system_info();
    REQUIRE(info.current_tool == 2);
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.filament_loaded);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
}

TEST_CASE("MedusaHC matches pin_watch object by prefix", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;
    backend.bootstrap_from_config(medusahc_test_config(1));

    backend.handle_status_update(json{{"pin_watch io", json{{"current_tool", 1}}}});
    REQUIRE(backend.get_system_info().current_tool == 1);
}

TEST_CASE("MedusaHC pin_watch -1 clears active tool highlight state", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;
    backend.bootstrap_from_config(medusahc_test_config(1));

    backend.handle_status_update(json{
        {"gcode_macro T0", json{{"variable_active", 1}, {"variable_color", ""}}},
        {"pin_watch io", json{{"current_tool", 0}}},
    });
    REQUIRE(backend.get_system_info().current_tool == 0);

    backend.handle_status_update(json{{"pin_watch io", json{{"current_tool", -1}}}});
    auto info = backend.get_system_info();
    REQUIRE(info.current_tool == -1);
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("MedusaHC unload_filament emits DROP_TOOL", "[ams][medusahc][gcode]") {
    RecordingMedusaHcBackend backend;
    backend.set_running_for_test();
    backend.bootstrap_from_config(medusahc_test_config(1));

    auto result = backend.unload_filament();
    REQUIRE(result);
    REQUIRE(backend.sent_gcodes.size() == 1);
    REQUIRE(backend.sent_gcodes[0] == "DROP_TOOL");
}

TEST_CASE("MedusaHC set_slot_info updates local spool metadata", "[ams][medusahc]") {
    RecordingMedusaHcBackend backend;
    backend.bootstrap_from_config(medusahc_test_config(1));

    SlotInfo updated;
    updated.color_rgb = 0x112233;
    updated.color_name = "Blue";
    updated.material = "PLA";
    updated.brand = "Acme";
    updated.spoolman_id = 42;
    updated.spool_name = "Spool A";
    updated.remaining_weight_g = 750;
    updated.total_weight_g = 1000;

    auto result = backend.set_slot_info(0, updated);
    REQUIRE(result);

    auto info = backend.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.color_rgb == 0x112233);
    REQUIRE(slot.color_name == "Blue");
    REQUIRE(slot.material == "PLA");
    REQUIRE(slot.brand == "Acme");
    REQUIRE(slot.spoolman_id == 42);
    REQUIRE(slot.spool_name == "Spool A");
    REQUIRE(slot.remaining_weight_g == 750);
    REQUIRE(slot.total_weight_g == 1000);
}

TEST_CASE("MedusaHC feeder actions emit OPEN and CLOSE", "[ams][medusahc][gcode]") {
    RecordingMedusaHcBackend backend;
    backend.set_running_for_test();

    auto open_result = backend.execute_device_action("open_feeder");
    REQUIRE(open_result);
    REQUIRE(backend.sent_gcodes.size() == 1);
    REQUIRE(backend.sent_gcodes[0] == "OPEN");

    auto close_result = backend.execute_device_action("close_feeder");
    REQUIRE(close_result);
    REQUIRE(backend.sent_gcodes.size() == 2);
    REQUIRE(backend.sent_gcodes[1] == "CLOSE");
}

TEST_CASE("MedusaHC change_tool emits Tn", "[ams][medusahc][gcode]") {
    RecordingMedusaHcBackend backend;
    backend.set_running_for_test();

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

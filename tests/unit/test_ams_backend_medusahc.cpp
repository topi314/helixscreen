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

    using AmsBackendMedusaHc::handle_status_update;
};

// Build a notify_status_update payload for the [medusahc] object. `current_tool`
// uses MedusaHC's convention: -2 sensor error, -1 nothing on head, 0..N-1 mounted.
json medusahc_status(int tool_count, int current_tool, json extra = json::object()) {
    json mh = json{{"tool_count", tool_count}, {"current_tool", current_tool}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        mh[it.key()] = it.value();
    }
    return json{{"medusahc", mh}};
}
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

TEST_CASE("MedusaHC config discovery via [medusahc] section", "[ams][medusahc][discovery]") {
    PrinterDiscovery discovery;
    discovery.parse_config_keys(json{{"medusahc", json::object()}});

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::MEDUSA_HC);

    const auto& systems = discovery.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::MEDUSA_HC);
    REQUIRE(systems[0].name == "MedusaHC");
}

TEST_CASE("MedusaHC objects.list discovery via medusahc object", "[ams][medusahc][discovery]") {
    PrinterDiscovery discovery;
    discovery.parse_objects(json::array({"medusahc", "extruder", "toolhead"}));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::MEDUSA_HC);

    const auto& systems = discovery.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::MEDUSA_HC);
}

TEST_CASE("MedusaHC parses medusahc object status", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(medusahc_status(2, 0));

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
}

TEST_CASE("MedusaHC unwraps notify_status_update params", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(
        json{{"params", json::array({medusahc_status(2, 1), 0})}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 2);
    REQUIRE(info.current_tool == 1);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::LOADED);
}

TEST_CASE("MedusaHC applies incremental field updates", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    // Full snapshot first (tool_count present).
    backend.handle_status_update(medusahc_status(3, 0));
    REQUIRE(backend.get_system_info().current_tool == 0);

    // Subsequent notifications only carry the changed field.
    backend.handle_status_update(json{{"medusahc", json{{"current_tool", 2}}}});
    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 3);
    REQUIRE(info.current_tool == 2);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
}

TEST_CASE("MedusaHC is tool-switch only (no load/swap preheat path)", "[ams][medusahc]") {
    RecordingMedusaHcBackend backend;
    backend.handle_status_update(medusahc_status(1, 0));

    AmsSystemInfo info = backend.get_system_info();
    REQUIRE_FALSE(backend.needs_unload_before_load(info));
}

TEST_CASE("MedusaHC current_tool -1 clears active tool", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(medusahc_status(2, 0));
    REQUIRE(backend.get_system_info().current_tool == 0);

    backend.handle_status_update(json{{"medusahc", json{{"current_tool", -1}}}});
    auto info = backend.get_system_info();
    REQUIRE(info.current_tool == -1);
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("MedusaHC sensor error (-2) reports no mounted tool", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(medusahc_status(2, -2, json{{"error", true}}));
    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 2);
    REQUIRE(info.current_tool == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("MedusaHC tracks per-tool dock sensors", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;

    backend.handle_status_update(medusahc_status(
        2, -1, json{{"tool0_docked", true}, {"tool1_docked", false}}));
    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 2);
    REQUIRE(info.current_tool == -1);
}

TEST_CASE("MedusaHC unload_filament emits DROP_TOOL", "[ams][medusahc][gcode]") {
    RecordingMedusaHcBackend backend;
    backend.set_running_for_test();
    backend.handle_status_update(medusahc_status(1, 0));

    auto result = backend.unload_filament();
    REQUIRE(result);
    REQUIRE(backend.sent_gcodes.size() == 1);
    REQUIRE(backend.sent_gcodes[0] == "DROP_TOOL");
}

TEST_CASE("MedusaHC set_slot_info updates local spool metadata", "[ams][medusahc]") {
    RecordingMedusaHcBackend backend;
    backend.handle_status_update(medusahc_status(1, -1));

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

    backend.handle_status_update(medusahc_status(3, -1));

    auto result = backend.change_tool(2);
    REQUIRE(result);
    REQUIRE(backend.sent_gcodes.size() == 1);
    REQUIRE(backend.sent_gcodes[0] == "T2");
}

TEST_CASE("MedusaHC action follows medusahc.state (never stuck at selecting)",
          "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;
    backend.set_running_for_test();
    backend.handle_status_update(medusahc_status(3, 0, json{{"state", "ready"}}));

    REQUIRE(backend.change_tool(2));
    REQUIRE(backend.get_system_info().action == AmsAction::SELECTING);

    // "changing" keeps the spinner up.
    backend.handle_status_update(json{{"medusahc", json{{"state", "changing"}}}});
    REQUIRE(backend.get_system_info().action == AmsAction::SELECTING);

    // Settling to "ready" clears it, even though the change ended on tool 2.
    backend.handle_status_update(
        json{{"medusahc", json{{"state", "ready"}, {"current_tool", 2}}}});
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("MedusaHC error state clears the in-progress spinner", "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;
    backend.set_running_for_test();
    backend.handle_status_update(medusahc_status(3, -1, json{{"state", "ready"}}));

    REQUIRE(backend.change_tool(1));
    REQUIRE(backend.get_system_info().action == AmsAction::SELECTING);

    // A failed change ends in error (current_tool stays -1) — must not hang.
    backend.handle_status_update(
        json{{"medusahc", json{{"state", "error"}, {"error", true}, {"current_tool", -1}}}});
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("MedusaHC reflects externally-initiated change as SELECTING",
          "[ams][medusahc][status]") {
    RecordingMedusaHcBackend backend;
    backend.handle_status_update(medusahc_status(3, 0, json{{"state", "ready"}}));
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);

    // A change started from Mainsail/console reports state=changing without us
    // setting an optimistic action — surface it as SELECTING.
    backend.handle_status_update(json{{"medusahc", json{{"state", "changing"}}}});
    REQUIRE(backend.get_system_info().action == AmsAction::SELECTING);
}

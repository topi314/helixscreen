// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temperature_utils.h"

#include "../../include/moonraker_client_mock.h"
#include "../lvgl_test_fixture.h"
#include "lvgl.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_temperature_state.h"
#include "settings_manager.h"
#include "temperature_sensor_manager.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterCapabilitiesState;
using helix::PrinterDiscovery;
using helix::PrinterTemperatureState;
using helix::ui::temperature::build_heater_gcode;
using helix::ui::temperature::build_heater_off_gcode;

// 1. PrinterDiscovery stores chamber sensor name
TEST_CASE("PrinterDiscovery stores chamber sensor name", "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_sensor chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_chamber_sensor());
    REQUIRE(discovery.chamber_sensor_name() == "temperature_sensor chamber");
}

// 2. PrinterTemperatureState updates chamber temp from status
TEST_CASE("PrinterTemperatureState updates chamber temp from status", "[temperature][chamber]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false); // No XML registration in tests
    temp_state.set_chamber_sensor_name("temperature_sensor chamber");

    nlohmann::json status = {{"temperature_sensor chamber", {{"temperature", 45.3}}}};
    temp_state.update_from_status(status);

    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 453); // centidegrees
}

// 3. PrinterCapabilitiesState sets chamber sensor capability
TEST_CASE("PrinterCapabilitiesState sets chamber sensor capability", "[capabilities][chamber]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    // Verify initial state before set_hardware()
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);

    PrinterDiscovery hardware;
    nlohmann::json objects = {"temperature_sensor chamber"};
    hardware.parse_objects(objects);

    CapabilityOverrides overrides;
    caps.set_hardware(hardware, overrides);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 1);
}

// 4. No chamber sensor - capability is 0
TEST_CASE("PrinterCapabilitiesState reports no chamber sensor when absent",
          "[capabilities][chamber]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    // Verify initial state before set_hardware()
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);

    PrinterDiscovery hardware;
    nlohmann::json objects = {"extruder", "heater_bed"}; // No chamber
    hardware.parse_objects(objects);

    CapabilityOverrides overrides;
    caps.set_hardware(hardware, overrides);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);
}

// 5. PrinterTemperatureState ignores chamber when sensor not configured
TEST_CASE("PrinterTemperatureState ignores chamber when sensor not configured",
          "[temperature][chamber]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);
    // Note: set_chamber_sensor_name() NOT called

    nlohmann::json status = {{"temperature_sensor chamber", {{"temperature", 45.3}}}};
    temp_state.update_from_status(status);

    // Should remain at initial value (0)
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 0);
}

// 5b. Regression for #947 (QIDI Q2): when both a chamber heater AND a chamber
// sensor are configured, partial Moonraker subscription updates that only
// include the sensor (because the sensor's temperature ticked but the heater's
// didn't) MUST NOT overwrite chamber_temp_ with the sensor's reading. On the
// Q2 the "chamber sensor" auto-discovered is actually a thermal-protection
// thermistor that climbs as the bed heats up — so without this guard, the
// chamber temp display flashes between the real chamber temp (heater) and
// the bed-influenced sensor reading.
TEST_CASE("PrinterTemperatureState does not let sensor pollute chamber when heater configured",
          "[temperature][chamber][issue947]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);
    temp_state.set_chamber_heater_name("heater_generic chamber");
    temp_state.set_chamber_sensor_name("temperature_sensor Chamber_Thermal_Protection_Sensor");

    // First update: heater reports 27.2°C / target 65°C
    nlohmann::json heater_update = {
        {"heater_generic chamber", {{"temperature", 27.2}, {"target", 65.0}}}};
    temp_state.update_from_status(heater_update);
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 272);
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_target_subject()) == 650);

    // Second update: only the thermal-protection sensor ticks (it tracks bed
    // proximity, climbing to ~70°C while the bed heats). The chamber heater
    // object is omitted from this partial update.
    nlohmann::json sensor_only_update = {
        {"temperature_sensor Chamber_Thermal_Protection_Sensor", {{"temperature", 70.0}}}};
    temp_state.update_from_status(sensor_only_update);

    // chamber_temp_ must NOT pick up the 70°C sensor reading — when a chamber
    // heater is configured, it is the only valid source for chamber_temp_.
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 272);
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_target_subject()) == 650);
}

// 6. Chamber assignment settings default to "auto"
TEST_CASE("Chamber assignment settings default to auto", "[settings][chamber]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    REQUIRE(settings.get_chamber_heater_assignment() == "auto");
    REQUIRE(settings.get_chamber_sensor_assignment() == "auto");
}

// 7. Chamber assignment settings persist values
TEST_CASE("Chamber assignment settings persist values", "[settings][chamber]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    settings.set_chamber_heater_assignment("heater_generic my_chamber");
    REQUIRE(settings.get_chamber_heater_assignment() == "heater_generic my_chamber");

    settings.set_chamber_sensor_assignment("temperature_sensor enclosure_bme");
    REQUIRE(settings.get_chamber_sensor_assignment() == "temperature_sensor enclosure_bme");

    settings.set_chamber_heater_assignment("none");
    REQUIRE(settings.get_chamber_heater_assignment() == "none");

    settings.set_chamber_sensor_assignment("auto");
    REQUIRE(settings.get_chamber_sensor_assignment() == "auto");
}

// 8. Manual chamber sensor override takes precedence over auto-detection
TEST_CASE("Manual chamber sensor override", "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);

    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_sensor chamber", "temperature_sensor enclosure_bme",
                              "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    settings.set_chamber_sensor_assignment("temperature_sensor enclosure_bme");

    std::string resolved_sensor = settings.get_chamber_sensor_assignment();
    if (resolved_sensor == "auto") {
        resolved_sensor = discovery.chamber_sensor_name();
    } else if (resolved_sensor == "none") {
        resolved_sensor = "";
    }
    temp_state.set_chamber_sensor_name(resolved_sensor);

    nlohmann::json status = {{"temperature_sensor enclosure_bme", {{"temperature", 33.7}}}};
    temp_state.update_from_status(status);

    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 337);

    settings.set_chamber_sensor_assignment("auto");
}

// 9. "none" disables chamber sensor even when auto would detect
TEST_CASE("Chamber sensor 'none' disables detection", "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);

    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_sensor chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    settings.set_chamber_sensor_assignment("none");

    std::string resolved_sensor = settings.get_chamber_sensor_assignment();
    if (resolved_sensor == "auto") {
        resolved_sensor = discovery.chamber_sensor_name();
    } else if (resolved_sensor == "none") {
        resolved_sensor = "";
    }
    temp_state.set_chamber_sensor_name(resolved_sensor);

    nlohmann::json status = {{"temperature_sensor chamber", {{"temperature", 45.3}}}};
    temp_state.update_from_status(status);

    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 0);

    settings.set_chamber_sensor_assignment("auto");
}

// 10. Manual chamber assignment updates role badge
TEST_CASE("Manual chamber assignment updates sensor role", "[chamber][role]") {
    LVGLTestFixture fixture;

    auto& mgr = helix::sensors::TemperatureSensorManager::instance();
    mgr.init_subjects();

    std::vector<std::string> objects = {"temperature_sensor chamber_temp",
                                        "temperature_sensor enclosure_bme",
                                        "temperature_sensor mcu_temp"};
    mgr.discover(objects);

    auto sensors = mgr.get_sensors_sorted();
    auto it = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor enclosure_bme";
    });
    REQUIRE(it != sensors.end());
    REQUIRE(it->role == helix::sensors::TemperatureSensorRole::AUXILIARY);

    mgr.apply_chamber_sensor_override("temperature_sensor enclosure_bme");

    sensors = mgr.get_sensors_sorted();
    it = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor enclosure_bme";
    });
    REQUIRE(it != sensors.end());
    REQUIRE(it->role == helix::sensors::TemperatureSensorRole::CHAMBER);

    auto old_chamber = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor chamber_temp";
    });
    REQUIRE(old_chamber != sensors.end());
    REQUIRE(old_chamber->role != helix::sensors::TemperatureSensorRole::CHAMBER);
}

// Empty-string override path: existing CHAMBER demoted, nothing re-promoted.
TEST_CASE("apply_chamber_sensor_override(\"\") demotes existing CHAMBER",
          "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& mgr = helix::sensors::TemperatureSensorManager::instance();
    mgr.init_subjects();

    std::vector<std::string> objects = {"temperature_sensor chamber",
                                        "temperature_sensor mcu_temp"};
    mgr.discover(objects);

    // Auto-categorizer should promote the "chamber"-named sensor.
    auto sensors = mgr.get_sensors_sorted();
    auto chamber = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor chamber";
    });
    REQUIRE(chamber != sensors.end());
    REQUIRE(chamber->role == helix::sensors::TemperatureSensorRole::CHAMBER);

    // Clearing the override (auto-detect saw nothing chamber-like) must
    // demote the previous CHAMBER without leaving a stale promotion.
    mgr.apply_chamber_sensor_override("");

    sensors = mgr.get_sensors_sorted();
    chamber = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor chamber";
    });
    REQUIRE(chamber != sensors.end());
    REQUIRE(chamber->role != helix::sensors::TemperatureSensorRole::CHAMBER);
}

// Snapmaker / Elegoo scenario: the chamber sensor's klipper name doesn't
// contain "chamber" (it's "cavity" / "enclosure"). The override must still
// promote it so duplicate Chamber+Cavity entries don't appear in the temp
// graph.
TEST_CASE("apply_chamber_sensor_override promotes non-chamber-named sensor",
          "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& mgr = helix::sensors::TemperatureSensorManager::instance();
    mgr.init_subjects();

    std::vector<std::string> objects = {"temperature_sensor cavity",
                                        "temperature_sensor mcu_temp"};
    mgr.discover(objects);

    // Auto-categorizer doesn't match "cavity" against the "chamber" substring.
    auto sensors = mgr.get_sensors_sorted();
    auto cavity = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor cavity";
    });
    REQUIRE(cavity != sensors.end());
    REQUIRE(cavity->role == helix::sensors::TemperatureSensorRole::AUXILIARY);

    mgr.apply_chamber_sensor_override("temperature_sensor cavity");

    sensors = mgr.get_sensors_sorted();
    cavity = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor cavity";
    });
    REQUIRE(cavity != sensors.end());
    REQUIRE(cavity->role == helix::sensors::TemperatureSensorRole::CHAMBER);
}

// Calling override with the already-CHAMBER sensor is a no-op (preserves role,
// avoids churn / log noise on every reconnect for printers whose chamber name
// matches the auto-categorizer).
TEST_CASE("apply_chamber_sensor_override on already-CHAMBER sensor is a no-op",
          "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& mgr = helix::sensors::TemperatureSensorManager::instance();
    mgr.init_subjects();

    std::vector<std::string> objects = {"temperature_sensor chamber",
                                        "temperature_sensor mcu_temp"};
    mgr.discover(objects);

    auto sensors = mgr.get_sensors_sorted();
    auto chamber = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor chamber";
    });
    REQUIRE(chamber != sensors.end());
    REQUIRE(chamber->role == helix::sensors::TemperatureSensorRole::CHAMBER);
    auto original_priority = chamber->priority;

    // Should remain CHAMBER without demote-then-repromote churn.
    mgr.apply_chamber_sensor_override("temperature_sensor chamber");

    sensors = mgr.get_sensors_sorted();
    chamber = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
        return s.klipper_name == "temperature_sensor chamber";
    });
    REQUIRE(chamber != sensors.end());
    REQUIRE(chamber->role == helix::sensors::TemperatureSensorRole::CHAMBER);
    REQUIRE(chamber->priority == original_priority);
}

// 11. Full round trip: setting → override → temperature update
TEST_CASE("Chamber assignment full round trip", "[chamber][integration]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);

    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_sensor mcu_temp", "temperature_sensor external_bme",
                              "heater_generic custom_heater", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    // Names don't match any chamber keyword (chamber/cavity/enclosure/box) — auto-detect finds nothing
    REQUIRE_FALSE(discovery.has_chamber_sensor());
    REQUIRE_FALSE(discovery.has_chamber_heater());

    // User manually assigns
    settings.set_chamber_sensor_assignment("temperature_sensor external_bme");
    settings.set_chamber_heater_assignment("heater_generic custom_heater");

    // Resolve (same logic as PrinterState::set_hardware)
    std::string sensor = settings.get_chamber_sensor_assignment();
    if (sensor == "auto")
        sensor = discovery.chamber_sensor_name();
    else if (sensor == "none")
        sensor = "";

    std::string heater = settings.get_chamber_heater_assignment();
    if (heater == "auto")
        heater = discovery.chamber_heater_name();
    else if (heater == "none")
        heater = "";

    temp_state.set_chamber_sensor_name(sensor);
    temp_state.set_chamber_heater_name(heater);

    // Verify heater temp + target work
    nlohmann::json status = {
        {"heater_generic custom_heater", {{"temperature", 55.2}, {"target", 60.0}}},
        {"temperature_sensor external_bme", {{"temperature", 48.1}}}};
    temp_state.update_from_status(status);

    // Heater is preferred when both are set
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 552);
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_target_subject()) == 600);

    // Clean up
    settings.set_chamber_sensor_assignment("auto");
    settings.set_chamber_heater_assignment("auto");
}

// 12. Manual assignment updates capability flags for chamber panel visibility
TEST_CASE("Manual chamber assignment enables capability flags", "[chamber][capabilities]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    // Names don't match any chamber keyword (chamber/cavity/enclosure/box) — auto-detect finds nothing
    PrinterDiscovery hardware;
    nlohmann::json objects = {"temperature_sensor external_bme", "heater_generic custom_heater",
                              "extruder", "heater_bed"};
    hardware.parse_objects(objects);

    REQUIRE_FALSE(hardware.has_chamber_sensor());
    REQUIRE_FALSE(hardware.has_chamber_heater());

    CapabilityOverrides overrides;
    caps.set_hardware(hardware, overrides);

    // Capability flags are 0 from auto-detection
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_heater_subject()) == 0);

    // Manual override sets capability flags
    caps.set_has_chamber_sensor(true);
    caps.set_has_chamber_heater(true);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 1);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_heater_subject()) == 1);

    // "none" clears them
    caps.set_has_chamber_sensor(false);
    caps.set_has_chamber_heater(false);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_heater_subject()) == 0);
}

// 13. PrinterDiscovery extracts chamber heater object name from heater_generic
TEST_CASE("PrinterDiscovery extracts chamber heater object name from heater_generic",
          "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"heater_generic chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_chamber_heater());
    REQUIRE(discovery.chamber_heater_name() == "heater_generic chamber");
    REQUIRE(discovery.chamber_heater_object_name() == "chamber");
}

// 14. PrinterDiscovery extracts chamber heater object name from temperature_fan
TEST_CASE("PrinterDiscovery extracts chamber heater object name from temperature_fan",
          "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_fan chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_chamber_heater());
    REQUIRE(discovery.chamber_heater_name() == "temperature_fan chamber");
    REQUIRE(discovery.chamber_heater_object_name() == "chamber");
}

// 15. PrinterDiscovery handles chamber with different naming conventions
TEST_CASE("PrinterDiscovery handles chamber with different naming conventions",
          "[discovery][chamber]") {
    SECTION("heater_generic chamber_heater") {
        PrinterDiscovery discovery;
        nlohmann::json objects = {"heater_generic chamber_heater", "extruder", "heater_bed"};
        discovery.parse_objects(objects);

        REQUIRE(discovery.has_chamber_heater());
        REQUIRE(discovery.chamber_heater_name() == "heater_generic chamber_heater");
        REQUIRE(discovery.chamber_heater_object_name() == "chamber_heater");
    }

    SECTION("temperature_fan chamber_fan") {
        PrinterDiscovery discovery;
        nlohmann::json objects = {"temperature_fan chamber_fan", "extruder", "heater_bed"};
        discovery.parse_objects(objects);

        REQUIRE(discovery.has_chamber_heater());
        REQUIRE(discovery.chamber_heater_name() == "temperature_fan chamber_fan");
        REQUIRE(discovery.chamber_heater_object_name() == "chamber_fan");
    }

    SECTION("heater_generic CHAMBER (uppercase)") {
        PrinterDiscovery discovery;
        nlohmann::json objects = {"heater_generic CHAMBER", "extruder", "heater_bed"};
        discovery.parse_objects(objects);

        REQUIRE(discovery.has_chamber_heater());
        REQUIRE(discovery.chamber_heater_name() == "heater_generic CHAMBER");
        REQUIRE(discovery.chamber_heater_object_name() == "CHAMBER");
    }
}

// 16. PrinterDiscovery chamber_heater_object_name is empty when no chamber heater
TEST_CASE("PrinterDiscovery chamber_heater_object_name is empty when no chamber heater",
          "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"extruder", "heater_bed", "temperature_sensor mcu_temp"};
    discovery.parse_objects(objects);

    REQUIRE_FALSE(discovery.has_chamber_heater());
    REQUIRE(discovery.chamber_heater_name().empty());
    REQUIRE(discovery.chamber_heater_object_name().empty());
}

// 17. PrinterDiscovery clears chamber_heater_object_name on reset
TEST_CASE("PrinterDiscovery clears chamber_heater_object_name on reset", "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"heater_generic chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_chamber_heater());
    REQUIRE(discovery.chamber_heater_object_name() == "chamber");

    // Clear and re-parse with no chamber
    discovery.clear();
    nlohmann::json objects2 = {"extruder", "heater_bed"};
    discovery.parse_objects(objects2);

    REQUIRE_FALSE(discovery.has_chamber_heater());
    REQUIRE(discovery.chamber_heater_name().empty());
    REQUIRE(discovery.chamber_heater_object_name().empty());
}

// 18. PrinterDiscovery prefers heater over sensor for chamber
TEST_CASE("PrinterDiscovery tracks both chamber heater and sensor independently",
          "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"heater_generic chamber", "temperature_sensor chamber_temp",
                              "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_chamber_heater());
    REQUIRE(discovery.has_chamber_sensor());
    REQUIRE(discovery.chamber_heater_name() == "heater_generic chamber");
    REQUIRE(discovery.chamber_heater_object_name() == "chamber");
    REQUIRE(discovery.chamber_sensor_name() == "temperature_sensor chamber_temp");
}

// ============================================================================
// G-code generation tests
// ============================================================================

// 19. build_heater_gcode generates correct gcode for all heater types
TEST_CASE("build_heater_gcode generates correct gcode for all heater types", "[chamber][gcode]") {
    char buf[128];

    SECTION("heater_generic chamber") {
        const char* result = build_heater_gcode("heater_generic chamber", 450, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=chamber TARGET=45");
    }

    SECTION("heater_generic chamber_heater") {
        const char* result =
            build_heater_gcode("heater_generic chamber_heater", 600, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET=60");
    }

    SECTION("temperature_fan chamber") {
        const char* result = build_heater_gcode("temperature_fan chamber", 450, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) ==
                "SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber TARGET=45");
    }

    SECTION("temperature_fan chamber_fan") {
        const char* result =
            build_heater_gcode("temperature_fan chamber_fan", 500, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) ==
                "SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber_fan TARGET=50");
    }

    SECTION("extruder (bare name)") {
        const char* result = build_heater_gcode("extruder", 2100, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210");
    }

    SECTION("heater_bed (bare name)") {
        const char* result = build_heater_gcode("heater_bed", 600, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60");
    }

    SECTION("target=0 (turn off)") {
        const char* result = build_heater_gcode("heater_generic chamber", 0, buf, sizeof(buf));
        REQUIRE(result != nullptr);
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=chamber TARGET=0");
    }

    SECTION("empty heater name returns nullptr") {
        const char* result = build_heater_gcode("", 450, buf, sizeof(buf));
        REQUIRE(result == nullptr);
    }
}

// 20. build_heater_off_gcode convenience wrapper
TEST_CASE("build_heater_off_gcode generates correct off gcode", "[chamber][gcode]") {
    char buf[128];

    SECTION("heater_generic chamber off") {
        const char* result = build_heater_off_gcode("heater_generic chamber", buf, sizeof(buf));
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=chamber TARGET=0");
    }

    SECTION("temperature_fan chamber off") {
        const char* result = build_heater_off_gcode("temperature_fan chamber", buf, sizeof(buf));
        REQUIRE(std::string(result) ==
                "SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber TARGET=0");
    }

    SECTION("extruder off") {
        const char* result = build_heater_off_gcode("extruder", buf, sizeof(buf));
        REQUIRE(std::string(result) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0");
    }
}

// ============================================================================
// Mock client validation tests
// ============================================================================

// 23. Mock client accepts correct chamber gcode format (HEATER=chamber)
TEST_CASE("Mock client accepts correct SET_HEATER_TEMPERATURE HEATER=chamber format",
          "[chamber][gcode][mock]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});

    SECTION("correct format succeeds") {
        int result = mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=45");
        REQUIRE(result == 0);
    }

    SECTION("correct format with target=0 turns off") {
        int result = mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=0");
        REQUIRE(result == 0);
    }

    mock.disconnect();
}

// 24. Mock client rejects invalid format (HEATER=heater_generic chamber)
TEST_CASE("Mock client rejects invalid SET_HEATER_TEMPERATURE HEATER=heater_generic format",
          "[chamber][gcode][mock]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});

    SECTION("invalid prefix format fails") {
        int result =
            mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=heater_generic chamber TARGET=45");
        REQUIRE(result != 0);
    }

    mock.disconnect();
}

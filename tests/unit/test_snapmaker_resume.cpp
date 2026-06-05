// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/snapmaker_resume.h"

#include "../catch_amalgamated.hpp"

using helix::snapmaker_filament_config_gcode;
using helix::snapmaker_resume_noop_detected;
using helix::snapmaker_terminal_matchers;

TEST_CASE("snapmaker_terminal_matchers: empty until #991 device capture", "[pause][snapmaker]") {
    REQUIRE(snapmaker_terminal_matchers().empty());
}

TEST_CASE("snapmaker_filament_config_gcode: builds command for populated slot",
          "[pause][snapmaker]") {
    REQUIRE(
        snapmaker_filament_config_gcode(0, "PLA", "Snapmaker") ==
        "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER='0' FILAMENT_TYPE='PLA' VENDOR='Snapmaker'\n");
}

TEST_CASE("snapmaker_filament_config_gcode: empty material => skip (empty string)",
          "[pause][snapmaker]") {
    REQUIRE(snapmaker_filament_config_gcode(0, "", "Snapmaker").empty());
}

TEST_CASE("snapmaker_filament_config_gcode: empty brand => skip (empty string)",
          "[pause][snapmaker]") {
    REQUIRE(snapmaker_filament_config_gcode(1, "PETG", "").empty());
}

TEST_CASE("snapmaker_filament_config_gcode: uses the given non-zero extruder index",
          "[pause][snapmaker]") {
    REQUIRE(snapmaker_filament_config_gcode(2, "ABS", "eSUN") ==
            "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER='2' FILAMENT_TYPE='ABS' VENDOR='eSUN'\n");
}

TEST_CASE("snapmaker_resume_noop_detected: paused + SD inactive => true", "[pause][snapmaker]") {
    REQUIRE(snapmaker_resume_noop_detected(/*is_paused=*/true, /*sdcard_active=*/false));
}

TEST_CASE("snapmaker_resume_noop_detected: otherwise => false", "[pause][snapmaker]") {
    REQUIRE_FALSE(snapmaker_resume_noop_detected(false, false));
    REQUIRE_FALSE(snapmaker_resume_noop_detected(true, true));
    REQUIRE_FALSE(snapmaker_resume_noop_detected(false, true));
}

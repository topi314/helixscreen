// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_resume.h"

#include <string>

namespace helix {

std::vector<TerminalMatcher> snapmaker_terminal_matchers() {
    // PROVISIONAL(#991): empty until real U1 dirty-bed strings/ids are captured.
    return {};
}

std::string snapmaker_filament_config_gcode(int extruder, const std::string& material,
                                            const std::string& brand) {
    if (material.empty() || brand.empty()) {
        return "";
    }
    return "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER='" + std::to_string(extruder) +
           "' FILAMENT_TYPE='" + material + "' VENDOR='" + brand + "'\n";
}

bool snapmaker_resume_noop_detected(bool is_paused, bool sdcard_active) {
    return is_paused && !sdcard_active;
}

} // namespace helix

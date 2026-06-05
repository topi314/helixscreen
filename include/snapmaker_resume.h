// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pause_cause.h"

#include <string>
#include <vector>

namespace helix {

/// Terminal-cause patterns for the Snapmaker U1.
///
/// PROVISIONAL (prestonbrown/helixscreen#991): returns an EMPTY list for now.
/// We do not yet have confirmed dirty-bed print_stats.message strings or
/// exception ids from a physical U1, and matching on virtual_sdcard.is_active
/// alone would reproduce the original over-blocking bug (it cannot distinguish
/// a terminal dirty-bed abort from a recoverable clog/tangle). With an empty
/// list every pause classifies Recoverable; the post-resume no-op backstop
/// catches any genuinely-terminal cause that no-ops RESUME. Populate from
/// captured device logs under #991, e.g.:
///   return {{"dirty", -1, /*require_sdcard_inactive=*/true}};
std::vector<TerminalMatcher> snapmaker_terminal_matchers();

/// Build the per-extruder filament-config re-assertion gcode line emitted
/// before RESUME (Snapmaker firmware drops FILAMENT_TYPE/VENDOR on sensor
/// loss). Returns "" when material or brand is empty, so we never emit a
/// half-populated SET_PRINT_FILAMENT_CONFIG; the caller logs a warning.
/// Trailing newline included for chain concatenation.
///
/// NOTE (prestonbrown/helixscreen#991): uses the confirmed manual gcode arg
/// names FILAMENT_TYPE / VENDOR (Discord 2026-06). The REST /filament_detect/set
/// path uses MAIN_TYPE — a different interface; verify casing on-device.
std::string snapmaker_filament_config_gcode(int extruder, const std::string& material,
                                            const std::string& brand);

/// Post-resume no-op backstop predicate: true => RESUME silently no-op'd
/// (print still paused with SD playback inactive) => offer restart.
bool snapmaker_resume_noop_detected(bool is_paused, bool sdcard_active);

} // namespace helix

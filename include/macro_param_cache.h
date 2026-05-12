// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "macro_param_modal.h"

#include "hv/json.hpp"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace helix {

enum class MacroParamKnowledge { KNOWN_PARAMS, KNOWN_NO_PARAMS, UNKNOWN };

struct CachedMacroInfo {
    MacroParamKnowledge knowledge = MacroParamKnowledge::UNKNOWN;
    std::vector<MacroParam> params;
    std::string description; ///< From Klipper gcode_macro description field
};

/// Cache for macro parameter information, populated once during printer discovery.
/// Avoids per-click configfile.config queries by pre-parsing all gcode_macro templates.
class MacroParamCache {
  public:
    static MacroParamCache& instance();

    /// Populate cache from configfile.config response.
    /// @param config The configfile.config JSON object (keys like "gcode_macro clean_nozzle")
    /// @param known_macros Set of known macro names (uppercase, from printer's object list)
    void populate_from_configfile(const nlohmann::json& config,
                                  const std::unordered_set<std::string>& known_macros);

    /// Lookup cached info for a macro (case-insensitive).
    [[nodiscard]] CachedMacroInfo get(const std::string& macro_name) const;

    /// True if the macro is registered with Klipper (case-insensitive). Used to
    /// gate features that require a specific gcode_macro to be defined in the
    /// firmware (e.g. K2 AI detect needs LOAD_AI_RUN — not all variants ship it).
    [[nodiscard]] bool has_macro(const std::string& macro_name) const;

    /// Clear all cached state (call on disconnect/reconnect).
    void clear();

  private:
    MacroParamCache() = default;

    mutable std::mutex mutex_;
    // Key: lowercase macro name (e.g., "clean_nozzle")
    std::unordered_map<std::string, CachedMacroInfo> cache_;
};

} // namespace helix

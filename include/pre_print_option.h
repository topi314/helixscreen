// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "hv/json.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

/**
 * @file pre_print_option.h
 * @brief Printer-agnostic abstraction for per-print options shown on the
 *        print-detail panel (bed mesh, QGL, AI detect, timelapse, etc.).
 *
 * Per-printer option definitions live in `assets/config/printer_database.json`
 * under each entry's `pre_print_options` block; the JSON shape is parsed by
 * `parse_pre_print_option_set()` and surfaced via
 * `PrinterDetector::get_pre_print_option_set()`.
 */

/**
 * @brief Logical grouping for UI ordering.
 *
 * Underlying integer values are load-bearing — the option set is sorted
 * primarily by category, so the enum order defines the on-screen group order
 * (Mechanical first, then Quality, then Monitoring).
 */
enum class PrePrintCategory {
    Mechanical = 0, ///< Bed mesh, QGL, Z-tilt, etc.
    Quality = 1,    ///< Nozzle clean, priming, PA cal, flow ratio
    Monitoring = 2, ///< AI detect, waste chute, timelapse
};

/**
 * @brief How the option affects the print when toggled.
 *
 * Each kind has a corresponding payload struct in the variant below.
 */
enum class PrePrintStrategyKind {
    MacroParam,      ///< Append "PARAM=value" to the START_PRINT macro call
    PreStartGcode,   ///< Send a gcode line BEFORE the start macro
    QueueAheadJob,   ///< Run a separate calibration job before the print
    RuntimeCommand,  ///< Issue a runtime command at print start
};

// Strategy payload structs ---------------------------------------------------

/// `MacroParam`: appended to the START_PRINT call as `<param_name>=<value>`.
struct PrePrintStrategyMacroParam {
    std::string param_name;    ///< Native macro param (e.g. "BED_MESH")
    std::string enable_value;  ///< Value when toggle is ON  (e.g. "1")
    std::string skip_value;    ///< Value when toggle is OFF (e.g. "0")
    std::string default_value; ///< Optional default if option is absent from selection
};

/// `PreStartGcode`: a gcode line emitted before the start macro. The literal
/// substring `{value}` is interpolated to `1` when enabled, `0` when disabled.
struct PrePrintStrategyPreStartGcode {
    std::string gcode_template;
};

/// `QueueAheadJob`: queue another job (a calibration print) ahead of this one.
struct PrePrintStrategyQueueAheadJob {
    std::string job_path; ///< Path to the .gcode job to run first
};

/// `RuntimeCommand`: send a gcode command at print start; one form for
/// enabled, another for disabled.
struct PrePrintStrategyRuntimeCommand {
    std::string command_enabled;
    std::string command_disabled;
};

using PrePrintStrategyPayload = std::variant<PrePrintStrategyMacroParam,
                                             PrePrintStrategyPreStartGcode,
                                             PrePrintStrategyQueueAheadJob,
                                             PrePrintStrategyRuntimeCommand>;

/**
 * @brief A single printer-agnostic per-print option.
 *
 * Definitions live in `printer_database.json` per-printer. Labels and
 * descriptions are i18n keys resolved by the UI layer.
 */
struct PrePrintOption {
    std::string id;              ///< Stable identifier, e.g. "bed_mesh", "ai_detect"
    std::string label_key;       ///< i18n key for the toggle label
    std::string description_key; ///< i18n key for the helper description (optional)
    std::string icon;            ///< Material Design icon codepoint string (optional)
    PrePrintCategory category = PrePrintCategory::Mechanical;
    bool default_enabled = false;
    int order = 0; ///< Sort order within category (ascending)

    /// Hide and skip this option when the named macro is not present on the
    /// connected printer. Set on options whose gcode_template/macro_param
    /// targets a feature that ships with some firmware variants but not
    /// others (e.g. K2 Plus AI detect — "LOAD_AI_RUN" exists on Creality OS
    /// variants but not on the user's K2). Empty = no gate (always shown).
    std::string requires_macro;

    PrePrintStrategyKind strategy_kind = PrePrintStrategyKind::MacroParam;
    PrePrintStrategyPayload strategy{PrePrintStrategyMacroParam{}};
};

/**
 * @brief Ordered set of options for one printer.
 *
 * `macro_name` is the START_PRINT macro to use when aggregating
 * `MacroParam` options. Options are sorted by (category, order).
 *
 * `setup_gcode` (optional) is a single gcode command sent BEFORE the start
 * macro when any options are toggled off — used by Creality K1 variants
 * whose `PREPARE` macro variable can't be passed as a START_PRINT argument
 * and must be set via a separate `PRINT_PREPARED` call first.
 *
 * The name `setup_gcode` deliberately avoids overlap with the per-option
 * `PrePrintStrategyKind::PreStartGcode` strategy: this field is the
 * printer-level "fire once before START_PRINT" hook, while the strategy
 * emits per-option gcode lines collected by
 * `PrintPreparationManager::collect_pre_start_gcode_lines()`.
 */
struct PrePrintOptionSet {
    std::string macro_name;
    std::string setup_gcode;
    std::vector<PrePrintOption> options;

    bool empty() const {
        return macro_name.empty() && setup_gcode.empty() && options.empty();
    }

    /// Returns nullptr if no option with the given id is present.
    const PrePrintOption* find(const std::string& id) const;
};

// ----------------------------------------------------------------------------
// Free functions
// ----------------------------------------------------------------------------

/**
 * @brief Parse a single option from JSON. Returns nullopt on malformed input
 *        (missing required field, unknown strategy string, etc.) and logs a
 *        warning via spdlog.
 */
std::optional<PrePrintOption> parse_pre_print_option(const nlohmann::json& j);

/**
 * @brief Parse a top-level option set:
 *
 * @code
 * {
 *   "macro_name": "START_PRINT",
 *   "options": [ { ... }, ... ]
 * }
 * @endcode
 *
 * Malformed individual options are skipped (warning logged); the set still
 * loads. The returned vector is sorted by (category, order).
 */
PrePrintOptionSet parse_pre_print_option_set(const nlohmann::json& j);

/**
 * @brief Render a `MacroParam` option as `KEY=value` token. The `enabled`
 *        argument selects the option's `enable_value` vs `skip_value`.
 *
 * Returns the empty string if the option is not a `MacroParam` strategy.
 */
std::string render_macro_param(const PrePrintOption& opt, bool enabled);

/**
 * @brief Render a `PreStartGcode` option to a single gcode line, with `{value}`
 *        substituted by `1` (enabled) or `0` (disabled).
 *
 * Returns the empty string if the option is not a `PreStartGcode` strategy.
 */
std::string render_pre_start_gcode(const PrePrintOption& opt, bool enabled);

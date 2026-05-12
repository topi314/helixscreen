// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pre_print_option.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace {

// JSON-string -> enum mappings. Keep these centralized so the parser and any
// future serializer agree on the wire format.

std::optional<PrePrintCategory> parse_category(const std::string& s) {
    if (s == "mechanical")
        return PrePrintCategory::Mechanical;
    if (s == "quality")
        return PrePrintCategory::Quality;
    if (s == "monitoring")
        return PrePrintCategory::Monitoring;
    return std::nullopt;
}

std::optional<PrePrintStrategyKind> parse_strategy_kind(const std::string& s) {
    if (s == "macro_param")
        return PrePrintStrategyKind::MacroParam;
    if (s == "pre_start_gcode")
        return PrePrintStrategyKind::PreStartGcode;
    if (s == "queue_ahead_job")
        return PrePrintStrategyKind::QueueAheadJob;
    if (s == "runtime_command")
        return PrePrintStrategyKind::RuntimeCommand;
    return std::nullopt;
}

// Replace every occurrence of `needle` in `haystack` with `replacement`.
std::string replace_all(std::string haystack, const std::string& needle,
                        const std::string& replacement) {
    if (needle.empty())
        return haystack;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        haystack.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return haystack;
}

} // namespace

const PrePrintOption* PrePrintOptionSet::find(const std::string& id) const {
    auto it = std::find_if(options.begin(), options.end(),
                           [&](const PrePrintOption& o) { return o.id == id; });
    return (it != options.end()) ? &(*it) : nullptr;
}

std::optional<PrePrintOption> parse_pre_print_option(const nlohmann::json& j) {
    if (!j.is_object()) {
        spdlog::warn("[PrePrintOption] Skipping option: JSON value is not an object");
        return std::nullopt;
    }

    PrePrintOption opt;

    // --- id (required) ---
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty()) {
        spdlog::warn("[PrePrintOption] Skipping option: missing or empty 'id' field");
        return std::nullopt;
    }
    opt.id = j["id"].get<std::string>();

    // --- strategy (required) ---
    if (!j.contains("strategy") || !j["strategy"].is_string()) {
        spdlog::warn("[PrePrintOption] Skipping option '{}': missing 'strategy' field", opt.id);
        return std::nullopt;
    }
    auto kind = parse_strategy_kind(j["strategy"].get<std::string>());
    if (!kind) {
        spdlog::warn("[PrePrintOption] Skipping option '{}': unknown strategy '{}'", opt.id,
                     j["strategy"].get<std::string>());
        return std::nullopt;
    }
    opt.strategy_kind = *kind;

    // --- optional metadata ---
    opt.label_key = j.value("label_key", "");
    opt.description_key = j.value("description_key", "");
    opt.icon = j.value("icon", "");
    opt.default_enabled = j.value("default_enabled", false);
    opt.order = j.value("order", 0);
    opt.requires_macro = j.value("requires_macro", "");

    // category — defaults to Mechanical if absent or unknown (warned).
    if (j.contains("category") && j["category"].is_string()) {
        auto cat = parse_category(j["category"].get<std::string>());
        if (cat) {
            opt.category = *cat;
        } else {
            spdlog::warn("[PrePrintOption] Option '{}': unknown category '{}', defaulting to "
                         "'mechanical'",
                         opt.id, j["category"].get<std::string>());
        }
    }

    // --- strategy payload ---
    switch (opt.strategy_kind) {
    case PrePrintStrategyKind::MacroParam: {
        PrePrintStrategyMacroParam p;
        p.param_name = j.value("param_name", "");
        p.enable_value = j.value("enable_value", "");
        p.skip_value = j.value("skip_value", "");
        p.default_value = j.value("default_value", "");
        if (p.param_name.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': MacroParam strategy requires "
                         "non-empty 'param_name'",
                         opt.id);
            return std::nullopt;
        }
        // Empty enable/skip values would render as `KEY=` (no value) — silent
        // garbage that the macro will misparse. Reject at parse time.
        if (p.enable_value.empty() || p.skip_value.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': MacroParam strategy requires "
                         "non-empty 'enable_value' and 'skip_value' (got enable='{}' skip='{}')",
                         opt.id, p.enable_value, p.skip_value);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    case PrePrintStrategyKind::PreStartGcode: {
        PrePrintStrategyPreStartGcode p;
        p.gcode_template = j.value("gcode_template", "");
        if (p.gcode_template.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': PreStartGcode strategy requires "
                         "non-empty 'gcode_template'",
                         opt.id);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    case PrePrintStrategyKind::QueueAheadJob: {
        PrePrintStrategyQueueAheadJob p;
        p.job_path = j.value("job_path", "");
        if (p.job_path.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': QueueAheadJob strategy requires "
                         "non-empty 'job_path'",
                         opt.id);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    case PrePrintStrategyKind::RuntimeCommand: {
        PrePrintStrategyRuntimeCommand p;
        p.command_enabled = j.value("command_enabled", "");
        p.command_disabled = j.value("command_disabled", "");
        if (p.command_enabled.empty() && p.command_disabled.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': RuntimeCommand strategy requires "
                         "at least one of 'command_enabled' / 'command_disabled'",
                         opt.id);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    }

    return opt;
}

PrePrintOptionSet parse_pre_print_option_set(const nlohmann::json& j) {
    PrePrintOptionSet set;

    if (!j.is_object()) {
        spdlog::warn("[PrePrintOption] Option set JSON is not an object; returning empty set");
        return set;
    }

    set.macro_name = j.value("macro_name", "");
    set.setup_gcode = j.value("setup_gcode", "");

    if (j.contains("options")) {
        const auto& arr = j["options"];
        if (!arr.is_array()) {
            spdlog::warn("[PrePrintOption] 'options' is not an array; ignoring");
        } else {
            set.options.reserve(arr.size());
            for (const auto& entry : arr) {
                if (auto parsed = parse_pre_print_option(entry); parsed) {
                    set.options.push_back(std::move(*parsed));
                }
            }
        }
    }

    std::sort(set.options.begin(), set.options.end(),
              [](const PrePrintOption& a, const PrePrintOption& b) {
                  if (a.category != b.category) {
                      return static_cast<int>(a.category) < static_cast<int>(b.category);
                  }
                  return a.order < b.order;
              });

    return set;
}

std::string render_macro_param(const PrePrintOption& opt, bool enabled) {
    const auto* p = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
    if (!p) {
        spdlog::warn("[PrePrintOption] render_macro_param called on option '{}' with non-"
                     "MacroParam strategy",
                     opt.id);
        return {};
    }
    const std::string& value = enabled ? p->enable_value : p->skip_value;
    return p->param_name + "=" + value;
}

std::string render_pre_start_gcode(const PrePrintOption& opt, bool enabled) {
    const auto* p = std::get_if<PrePrintStrategyPreStartGcode>(&opt.strategy);
    if (!p) {
        spdlog::warn("[PrePrintOption] render_pre_start_gcode called on option '{}' with non-"
                     "PreStartGcode strategy",
                     opt.id);
        return {};
    }
    return replace_all(p->gcode_template, "{value}", enabled ? "1" : "0");
}

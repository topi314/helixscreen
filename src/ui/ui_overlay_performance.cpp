// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_overlay_performance.h"

#include "observer_factory.h"
#include "ui_utils.h"  // helix::ui::safe_clean_children

#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <vector>

namespace helix {
namespace ui {

UiOverlayPerformance& UiOverlayPerformance::instance() {
    static UiOverlayPerformance s;
    return s;
}

lv_obj_t* UiOverlayPerformance::create(lv_obj_t* parent) {
    if (root_) return root_;

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "performance_overlay", nullptr));
    if (!root_) {
        spdlog::error("[UiOverlayPerformance] Failed to create root from XML");
        return nullptr;
    }
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    mcu_card_ = lv_obj_find_by_name(root_, "mcu_card");

    lv_subject_t* names_subj = lv_xml_get_subject(nullptr, "perf_mcu_names");
    if (names_subj) {
        mcu_names_observer_ = helix::ui::observe_string<UiOverlayPerformance>(
            names_subj, this,
            [](UiOverlayPerformance* self, const char* /*v*/) {
                self->rebuild_mcu_rows();
            });
    } else {
        spdlog::warn("[UiOverlayPerformance] perf_mcu_names subject not found");
    }

    return root_;
}

void UiOverlayPerformance::rebuild_mcu_rows() {
    if (!mcu_card_) return;
    helix::ui::safe_clean_children(mcu_card_);

    lv_subject_t* names_subj = lv_xml_get_subject(nullptr, "perf_mcu_names");
    if (!names_subj) return;
    const char* names_cstr = lv_subject_get_string(names_subj);
    if (!names_cstr || !*names_cstr) return;

    std::vector<std::string> names;
    std::stringstream ss(names_cstr);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            names.push_back(item);
        }
    }

    for (const auto& name : names) {
        // Sanitize name for use as subject key: spaces, slashes, dots → underscores
        std::string safe = name;
        for (auto& c : safe) {
            if (c == ' ' || c == '/' || c == '.') c = '_';
        }

        const std::string bind_value   = "perf_mcu_" + safe + "_load_pct";
        const std::string bind_text    = "perf_mcu_" + safe + "_text";
        const std::string bind_present = "perf_mcu_" + safe + "_present";
        const std::string spark_src    = "mcu_" + safe + "_load_pct";

        const char* attrs[] = {
            "label",            name.c_str(),
            "bind_value",       bind_value.c_str(),
            "bind_text",        bind_text.c_str(),
            "bind_present",     bind_present.c_str(),
            "sparkline_source", spark_src.c_str(),
            nullptr,            nullptr
        };
        lv_obj_t* row = static_cast<lv_obj_t*>(
            lv_xml_create(mcu_card_, "perf_metric_row", attrs));
        if (!row) {
            spdlog::warn("[UiOverlayPerformance] Failed to create perf_metric_row for MCU '{}'",
                         name);
        }
    }
}

} // namespace ui
} // namespace helix

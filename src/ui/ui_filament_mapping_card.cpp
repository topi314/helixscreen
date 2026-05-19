// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_card.h"

#include "ams_state.h"
#include "color_utils.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui_fonts.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Pills are instantiated from ui_xml/components/filament_mapping_pill.xml
// (dynamic count depends on the gcode file). lv_obj_add_event_cb is used on
// the card itself for the modal-open handler as an allowed exception.

// ============================================================================
// Setup
// ============================================================================

void FilamentMappingCard::create(lv_obj_t* card_widget, lv_obj_t* rows_container,
                                  lv_obj_t* warning_container) {
    card_ = card_widget;
    rows_container_ = rows_container;
    warning_container_ = warning_container;

    // Make the entire card tappable to open the mapping modal
    if (card_) {
        lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            card_,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentMappingCard*>(lv_event_get_user_data(e));
                self->open_mapping_modal();
            },
            LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[FilamentMapping] Card created");
}

// ============================================================================
// Update / visibility
// ============================================================================

void FilamentMappingCard::update(const std::vector<std::string>& gcode_colors,
                                  const std::vector<std::string>& gcode_materials) {
    if (!card_ || !rows_container_) {
        should_show_ = false;
        return;
    }

    // Check if AMS is available
    auto& ams = AmsState::instance();
    if (!ams.is_available()) {
        should_show_ = false;
        return;
    }

    // Hide on backends with no editable tool mapping (Snapmaker U1, ACE).
    // Without this, users can open the modal and pick mappings that the
    // print-start path then warns away — dead control. The print-start
    // warning toast stays as a safety net.
    bool any_editable = false;
    for (int i = 0, n = ams.backend_count(); i < n; ++i) {
        auto* backend = ams.get_backend(i);
        if (!backend) {
            continue;
        }
        auto caps = backend->get_tool_mapping_capabilities();
        if (caps.supported && caps.editable) {
            any_editable = true;
            break;
        }
    }
    if (!any_editable) {
        should_show_ = false;
        return;
    }

    // Build tool info from file metadata
    tool_info_ = build_tool_info(gcode_colors, gcode_materials);

    if (tool_info_.empty()) {
        should_show_ = false;
        return;
    }

    // Collect available slots from AMS backends
    available_slots_ = collect_available_slots();

    // Compute mappings based on user preference
    if (SettingsManager::instance().get_auto_color_map()) {
        // Color matching: clear firmware mappings so they don't override color matches
        auto slots_for_matching = available_slots_;
        for (auto& s : slots_for_matching) {
            s.current_tool_mapping = -1;
        }
        mappings_ = helix::FilamentMapper::compute_defaults(tool_info_, slots_for_matching);
    } else {
        // Positional assignment (T0→slot 0, T1→slot 1, etc.)
        mappings_ = helix::FilamentMapper::use_current_assignments(tool_info_, available_slots_);
    }

    // Build the compact UI
    rebuild_compact_view();

    // Visibility is published via the `filament_mapping_visible` subject by the
    // detail view — see PrintSelectDetailView::publish_mapping_visibility().
    should_show_ = true;

    spdlog::debug("[FilamentMapping] Updated: {} tools, {} slots, {} mappings",
                  tool_info_.size(), available_slots_.size(), mappings_.size());
}

bool FilamentMappingCard::has_mismatch() const {
    return has_any_mismatch();
}

void FilamentMappingCard::on_ui_destroyed() {
    card_ = nullptr;
    rows_container_ = nullptr;
    warning_container_ = nullptr;
}

// ============================================================================
// Compact swatch pair view
// ============================================================================

void FilamentMappingCard::rebuild_compact_view() {
    if (!rows_container_) {
        return;
    }

    // [L081] freeze+drain handles UpdateQueue concurrency, but LVGL's own
    // event-dispatch loop (modal on_mappings_updated → here) is the other
    // batch we have to escape. safe_clean_children async-deletes via LVGL.
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::safe_clean_children(rows_container_);

    // Pill layout, sizing, padding, fonts all live in
    // ui_xml/components/filament_mapping_pill.xml — tune visuals without
    // rebuilding. C++ only supplies per-pill dynamic data: colors, Tx label,
    // and the empty-slot warning variant.
    lv_obj_set_flex_flow(rows_container_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_flex_cross_place(rows_container_, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(rows_container_, theme_manager_get_spacing("space_xs"), 0);

    size_t count = std::min(mappings_.size(), tool_info_.size());
    bool multi_tool = count > 1;
    // Cap visible pills so a file with many tools doesn't flood the right
    // column. Beyond the cap, the remaining tools are summarized in a single
    // "+N" overflow pill that fills the final grid cell (tap the card to see
    // and edit the full mapping).
    constexpr size_t kMaxVisiblePills = 6;
    size_t visible = count;
    bool overflow = count > kMaxVisiblePills;
    if (overflow) {
        visible = kMaxVisiblePills - 1; // leave space for the overflow pill
    }
    for (size_t i = 0; i < visible; ++i) {
        const auto& mapping = mappings_[i];
        const auto& tool = tool_info_[i];

        auto* pill = static_cast<lv_obj_t*>(
            lv_xml_create(rows_container_, "filament_mapping_pill", nullptr));
        if (!pill) {
            continue;
        }
        // Target two pills per row (2x2 grid for four-tool prints). Slightly
        // under 50% so the inter-pill gap doesn't force wrapping.
        lv_obj_set_width(pill, lv_pct(48));

        // G-code color dot and Tx label (only shown for multi-tool files).
        lv_color_t gcode_color = lv_color_hex(tool.color_rgb);
        if (auto* gcode_dot = lv_obj_find_by_name(pill, "gcode_dot")) {
            lv_obj_set_style_bg_color(gcode_dot, gcode_color, 0);
        }
        if (auto* tool_lbl = lv_obj_find_by_name(pill, "tool_label")) {
            if (multi_tool) {
                lv_label_set_text_fmt(tool_lbl, "T%d", tool.tool_index);
                lv_obj_set_style_text_color(
                    tool_lbl, theme_manager_get_contrast_color(gcode_color), 0);
                lv_obj_remove_flag(tool_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Slot color dot — resolve mapped slot; empty slots render as a
        // transparent circle with a warning-colored border.
        uint32_t slot_color = 0x808080;
        bool slot_empty = false;
        if (!mapping.is_auto && mapping.mapped_slot >= 0) {
            for (const auto& s : available_slots_) {
                if (s.slot_index == mapping.mapped_slot &&
                    s.backend_index == mapping.mapped_backend) {
                    slot_color = s.color_rgb;
                    slot_empty = s.is_empty;
                    break;
                }
            }
        }
        if (auto* slot_dot = lv_obj_find_by_name(pill, "slot_dot")) {
            if (slot_empty) {
                lv_obj_set_style_bg_opa(slot_dot, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(slot_dot, 2, 0);
                lv_obj_set_style_border_color(
                    slot_dot, theme_manager_get_color("warning"), 0);
                lv_obj_set_style_border_opa(slot_dot, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(slot_dot, lv_color_hex(slot_color), 0);
            }
        }
    }

    if (overflow) {
        if (auto* more = static_cast<lv_obj_t*>(
                lv_xml_create(rows_container_, "filament_mapping_more_pill", nullptr))) {
            lv_obj_set_width(more, lv_pct(48));
            if (auto* lbl = lv_obj_find_by_name(more, "count_label")) {
                lv_label_set_text_fmt(lbl, "+%zu", count - visible);
            }
        }
    }

    // Warning icon visibility is handled by XML bind_flag_if_eq on "filament_mismatch" subject
}

bool FilamentMappingCard::has_any_mismatch() const {
    for (const auto& m : mappings_) {
        if (m.material_mismatch) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Color queries
// ============================================================================

std::vector<uint32_t> FilamentMappingCard::get_mapped_colors() const {
    std::vector<uint32_t> colors;
    colors.reserve(mappings_.size());

    for (size_t i = 0; i < mappings_.size(); ++i) {
        const auto& mapping = mappings_[i];

        if (!mapping.is_auto && mapping.mapped_slot >= 0) {
            uint32_t slot_color = (i < tool_info_.size()) ? tool_info_[i].color_rgb : 0x808080;
            bool found = false;
            for (const auto& s : available_slots_) {
                if (s.slot_index == mapping.mapped_slot &&
                    s.backend_index == mapping.mapped_backend) {
                    slot_color = s.color_rgb;
                    found = true;
                    break;
                }
            }
            spdlog::debug("[FilamentMapping] T{}: mapped slot={} backend={} → "
                          "0x{:06X} (found={}, slots_seen={})",
                          mapping.tool_index, mapping.mapped_slot,
                          mapping.mapped_backend, slot_color, found,
                          available_slots_.size());
            colors.push_back(slot_color);
        } else {
            uint32_t fallback =
                (i < tool_info_.size()) ? tool_info_[i].color_rgb : 0x808080;
            spdlog::debug("[FilamentMapping] T{}: auto={} slot={} → slicer 0x{:06X}",
                          mapping.tool_index, mapping.is_auto, mapping.mapped_slot,
                          fallback);
            colors.push_back(fallback);
        }
    }

    return colors;
}

// ============================================================================
// Modal interaction
// ============================================================================

void FilamentMappingCard::open_mapping_modal() {
    spdlog::debug("[FilamentMapping] Opening mapping modal");

    mapping_modal_.set_tool_info(tool_info_);
    mapping_modal_.set_available_slots(available_slots_);
    mapping_modal_.set_mappings(mappings_);
    mapping_modal_.set_on_mappings_updated([this](auto mappings) {
        mappings_ = std::move(mappings);
        rebuild_compact_view();
        if (on_mappings_changed_) {
            on_mappings_changed_();
        }
    });
    mapping_modal_.show(lv_screen_active());
}

// ============================================================================
// Data collection
// ============================================================================

std::vector<helix::AvailableSlot> FilamentMappingCard::collect_available_slots() {
    std::vector<helix::AvailableSlot> slots;
    auto& ams = AmsState::instance();

    for (int bi = 0; bi < ams.backend_count(); ++bi) {
        auto* backend = ams.get_backend(bi);
        if (!backend) {
            continue;
        }

        auto info = backend->get_system_info();
        bool multi_unit = info.units.size() > 1;

        for (const auto& unit : info.units) {
            for (const auto& slot_info : unit.slots) {
                helix::AvailableSlot as;
                as.slot_index = slot_info.global_index;
                as.local_slot_index = slot_info.slot_index;
                as.backend_index = bi;
                as.color_rgb = slot_info.color_rgb;
                as.material = slot_info.material;
                as.is_empty = (slot_info.status == SlotStatus::EMPTY ||
                               slot_info.status == SlotStatus::UNKNOWN);
                as.current_tool_mapping = slot_info.mapped_tool;
                as.unit_index = unit.unit_index;
                if (multi_unit) {
                    as.unit_display_name = unit.display_name.empty()
                                               ? unit.name
                                               : unit.display_name;
                }
                slots.push_back(std::move(as));
            }
        }
    }

    spdlog::debug("[FilamentMapping] Collected {} available slots from {} backends",
                  slots.size(), ams.backend_count());
    return slots;
}

std::vector<helix::GcodeToolInfo> FilamentMappingCard::build_tool_info(
    const std::vector<std::string>& colors,
    const std::vector<std::string>& materials) {
    std::vector<helix::GcodeToolInfo> tools;

    // Use the larger of colors or materials to determine tool count.
    // If both are empty, return empty — the card will be hidden.
    size_t count = std::max(colors.size(), materials.size());
    if (count == 0) {
        return tools;
    }

    for (size_t i = 0; i < count; ++i) {
        helix::GcodeToolInfo tool;
        tool.tool_index = static_cast<int>(i);

        // Parse color
        if (i < colors.size() && !colors[i].empty()) {
            auto parsed = helix::parse_hex_color(colors[i]);
            tool.color_rgb = parsed.value_or(0x808080);
        } else {
            tool.color_rgb = 0x808080;
        }

        // Material
        if (i < materials.size()) {
            tool.material = materials[i];
        }

        tools.push_back(std::move(tool));
    }

    return tools;
}

} // namespace helix::ui

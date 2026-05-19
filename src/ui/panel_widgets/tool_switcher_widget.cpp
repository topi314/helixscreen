// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_switcher_widget.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include <spdlog/spdlog.h>

namespace helix {

// Static instance for event callback routing
ToolSwitcherWidget* ToolSwitcherWidget::s_active_instance = nullptr;

/// Resolve a responsive spacing token to pixels, with a fallback.
static int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

void register_tool_switcher_widget() {
    register_widget_factory("tool_switcher", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ToolSwitcherWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tool_pill_cb", ToolSwitcherWidget::tool_pill_cb);
    lv_xml_register_event_cb(nullptr, "tool_compact_cb", ToolSwitcherWidget::tool_compact_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : printer_state_(printer_state) {}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_active_instance = this;

    auto& tool_state = ToolState::instance();
    auto token = lifetime_.token();

    // Observe active tool changes
    active_tool_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_active_tool_subject(), this,
        [token](ToolSwitcherWidget* self, int tool) {
            if (token.expired()) return;
            self->on_active_tool_changed(tool);
        });

    // Observe tool count changes to trigger rebuild
    tool_count_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_tool_count_subject(), this,
        [token](ToolSwitcherWidget* self, int /*count*/) {
            if (token.expired()) return;
            if (self->current_colspan_ == 1 && self->current_rowspan_ == 1) {
                self->rebuild_compact();
            } else {
                self->rebuild_pills();
            }
        });

    // Initial build deferred to on_size_changed() which fires after
    // the widget is fully attached to the screen tree.
    // Building here can crash (disp==NULL) if XML tree isn't mounted yet.
}

void ToolSwitcherWidget::detach() {
    lifetime_.invalidate();
    dismiss_tool_picker();
    active_tool_observer_.reset();
    tool_count_observer_.reset();
    pill_buttons_.clear();
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolSwitcherWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                         int /*height_px*/) {
    current_colspan_ = colspan;
    current_rowspan_ = rowspan;

    if (!widget_obj_) return;

    if (colspan == 1 && rowspan == 1) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }
}

// ============================================================================
// Pill buttons (inline mode for 1x2, 2x1, 2x2, etc.)
// ============================================================================

void ToolSwitcherWidget::rebuild_pills() {
    if (!widget_obj_) return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for pill rebuild");
        return;
    }

    pill_buttons_.clear();
    helix::ui::safe_clean_children(container);

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    if (tools.empty()) {
        spdlog::debug("[ToolSwitcher] No tools available for pill rebuild");
        return;
    }

    int space_xs = resolve_space_token("space_xs", 4);

    // Choose flex direction based on widget shape
    // 1x2 (tall) = column, otherwise row
    if (current_colspan_ == 1 && current_rowspan_ >= 2) {
        lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    } else {
        lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    }
    lv_obj_set_style_pad_gap(container, space_xs, 0);

    for (size_t i = 0; i < tools.size(); ++i) {
        bool is_active = (static_cast<int>(i) == active);

        // Create pill button from XML ui_button widget — variant handles base styling
        const char* variant = is_active ? "primary" : "ghost";
        const char* attrs[] = {"variant", variant, "text", tools[i].name.c_str(), nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(container, "ui_button", attrs));
        if (!btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('ui_button') returned NULL for pill '{}'",
                          tools[i].name);
            continue;
        }

        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        int btn_min_h = resolve_space_token("space_xl", 24);
        lv_obj_set_style_min_height(btn, btn_min_h, 0);
        lv_obj_set_style_radius(btn, btn_min_h / 2, 0);
        lv_obj_set_style_pad_ver(btn, resolve_space_token("space_xxs", 4), 0);
        lv_obj_set_style_pad_hor(btn, resolve_space_token("space_sm", 8), 0);

        // Pass tool index via event callback user_data (NOT obj user_data — L069)
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] pill_click");
                if (!s_active_instance) return;
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                s_active_instance->handle_tool_selected(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        pill_buttons_.push_back(btn);
    }

    // Scroll the active pill into view when the container overflows.
    if (active >= 0 && active < static_cast<int>(pill_buttons_.size())) {
        lv_obj_scroll_to_view(pill_buttons_[active], LV_ANIM_OFF);
    }

    spdlog::debug("[ToolSwitcher] Built {} pill buttons, active={}", tools.size(), active);
}

void ToolSwitcherWidget::on_active_tool_changed(int tool_index) {
    if (current_colspan_ == 1 && current_rowspan_ == 1) {
        // Compact mode — rebuild to update the label
        if (widget_obj_) {
            rebuild_compact();
        }
        return;
    }

    // Pill mode — rebuild to apply correct variant styling per button
    if (widget_obj_) {
        rebuild_pills();
    }

    spdlog::debug("[ToolSwitcher] Active tool changed to T{}", tool_index);
}

// ============================================================================
// Compact mode (1x1 — single label + picker popup)
// ============================================================================

void ToolSwitcherWidget::rebuild_compact() {
    if (!widget_obj_) return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for compact rebuild");
        return;
    }

    pill_buttons_.clear();
    helix::ui::safe_clean_children(container);

    auto& tool_state = ToolState::instance();
    int active = tool_state.active_tool_index();
    const auto& tools = tool_state.tools();

    // Set container clickable for compact mode
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(container, resolve_space_token("space_xxs", 2), 0);

    // Swap icon above tool label
    const char* icon_attrs[] = {"src", "arrow_left_right", "size", "sm", "variant",
                                "secondary", nullptr};
    auto* icon = static_cast<lv_obj_t*>(lv_xml_create(container, "icon", icon_attrs));
    if (icon) {
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Current tool label centered with larger font
    lv_obj_t* label = lv_label_create(container);
    std::string tool_name = (active >= 0 && active < static_cast<int>(tools.size()))
                                ? tools[active].name
                                : "T?";
    lv_label_set_text(label, tool_name.c_str());
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
    const lv_font_t* body_font = theme_manager_get_font("font_body");
    if (body_font)
        lv_obj_set_style_text_font(label, body_font, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Click opens picker
    lv_obj_add_event_cb(
        container,
        [](lv_event_t* /*e*/) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] compact_click");
            if (s_active_instance) {
                s_active_instance->show_tool_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    spdlog::debug("[ToolSwitcher] Built compact mode, active=T{}", active);
}

// ============================================================================
// Tool picker popup (for compact mode)
// ============================================================================

void ToolSwitcherWidget::show_tool_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    if (tools.empty()) return;

    int space_xs = resolve_space_token("space_xs", 4);

    int screen_w = lv_obj_get_width(parent_screen_);
    int screen_h = lv_obj_get_height(parent_screen_);

    // Backdrop (full screen, transparent, catches clicks to dismiss)
    picker_backdrop_ = lv_obj_create(parent_screen_);
    lv_obj_set_size(picker_backdrop_, screen_w, screen_h);
    lv_obj_set_pos(picker_backdrop_, 0, 0);
    lv_obj_set_style_bg_color(picker_backdrop_, theme_manager_get_color("screen_bg"), 0);
    lv_obj_set_style_bg_opa(picker_backdrop_, LV_OPA_50, 0);
    lv_obj_set_style_border_width(picker_backdrop_, 0, 0);
    lv_obj_set_style_radius(picker_backdrop_, 0, 0);
    lv_obj_remove_flag(picker_backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(picker_backdrop_, LV_OBJ_FLAG_CLICKABLE);

    // Backdrop click dismisses picker
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* /*e*/) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] backdrop_cb");
            if (s_active_instance) {
                s_active_instance->dismiss_tool_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    // Context menu — compact card positioned at the widget, no title
    lv_obj_t* card = lv_obj_create(picker_backdrop_);
    // Match widget width so buttons fill naturally via 100%
    int card_w = widget_obj_ ? lv_obj_get_width(widget_obj_) : 120;
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, theme_manager_get_color("elevated_bg"), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, resolve_space_token("space_sm", 8), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, theme_manager_get_color("border"), 0);
    lv_obj_set_style_pad_all(card, space_xs, 0);
    lv_obj_set_style_pad_gap(card, space_xs, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Buttons in a simple column — one per tool
    // Context menu = single column of buttons directly in card

    for (size_t i = 0; i < tools.size(); ++i) {
        bool is_active = (static_cast<int>(i) == active);

        // Create picker button from XML template
        const char* btn_attrs[] = {"tool_text", tools[i].name.c_str(), nullptr};
        lv_obj_t* picker_btn =
            static_cast<lv_obj_t*>(lv_xml_create(card, "tool_picker_button", btn_attrs));
        if (!picker_btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('tool_picker_button') returned NULL");
            continue;
        }

        // Find the actual ui_button — context menu buttons are full width
        lv_obj_t* btn = lv_obj_find_by_name(picker_btn, "tool_btn");
        if (btn) {
            lv_obj_set_width(picker_btn, LV_PCT(100));
            lv_obj_set_width(btn, LV_PCT(100));

            // Active tool: use primary variant styling (let ui_button handle colors)
            if (is_active) {
                // ui_button "ghost" doesn't have a bg — set primary bg directly
                lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), 0);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_t* label = lv_obj_find_by_name(picker_btn, "tool_btn_label");
                if (label) {
                    lv_obj_set_style_text_color(label, theme_manager_get_color("screen_bg"), 0);
                }
            }

            // Pass tool index via event callback user_data (NOT obj user_data — L069:
            // ui_button already owns obj user_data for its internal button_data_t)
            lv_obj_add_event_cb(
                btn,
                [](lv_event_t* e) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] picker_tool_click");
                    if (!s_active_instance) return;
                    int idx =
                        static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                    s_active_instance->handle_tool_selected(idx);
                    s_active_instance->dismiss_tool_picker();
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        }
    }

    // Position context menu right above/below the widget
    lv_obj_update_layout(card);
    int card_h = lv_obj_get_height(card);
    int card_actual_w = lv_obj_get_width(card);

    if (widget_obj_) {
        lv_area_t widget_coords;
        lv_obj_get_coords(widget_obj_, &widget_coords);

        // Align left edge of menu with left edge of widget
        int card_x = widget_coords.x1;
        // Place above widget
        int card_y = widget_coords.y1 - card_h - space_xs;

        // Clamp to screen bounds
        card_x = std::clamp(card_x, space_xs, screen_w - card_actual_w - space_xs);
        if (card_y < space_xs) {
            // Not enough room above — place below
            card_y = widget_coords.y2 + space_xs;
        }
        if (card_y + card_h > screen_h - space_xs) {
            lv_obj_center(card);
        } else {
            lv_obj_set_pos(card, card_x, card_y);
        }
    } else {
        lv_obj_center(card);
    }

    spdlog::debug("[ToolSwitcher] Picker shown with {} tools", tools.size());
}

void ToolSwitcherWidget::dismiss_tool_picker() {
    if (!picker_backdrop_) return;

    // Use deferred deletion to avoid destroying the event source during
    // event processing (picker button click calls dismiss then handle_tool_selected)
    helix::ui::safe_delete_deferred(picker_backdrop_);

    spdlog::debug("[ToolSwitcher] Picker dismissed");
}

// ============================================================================
// Tool selection with safety gate
// ============================================================================

void ToolSwitcherWidget::handle_tool_selected(int tool_index) {
    auto& tool_state = ToolState::instance();

    // Already on this tool
    if (tool_index == tool_state.active_tool_index()) {
        spdlog::debug("[ToolSwitcher] Tool T{} already active, ignoring", tool_index);
        return;
    }

    // Check if printing — warn before tool change
    auto job_state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));

    if (job_state == PrintJobState::PRINTING || job_state == PrintJobState::PAUSED) {
        spdlog::info("[ToolSwitcher] Print active, showing confirmation for T{}", tool_index);

        helix::ui::modal_show_confirmation(
            "Tool Change During Print",
            "Changing tools while printing may cause issues. Continue?",
            ::ModalSeverity::Warning, "Change Tool",
            // on_confirm
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] confirm_tool_change");
                int idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                auto* api = get_moonraker_api();
                if (api) {
                    ToolState::instance().request_tool_change(idx, api);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            // on_cancel (nullptr = just dismiss)
            nullptr,
            // user_data = tool_index
            reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));
        return;
    }

    // Not printing — change directly
    auto* api = get_moonraker_api();
    if (api) {
        tool_state.request_tool_change(tool_index, api);
        spdlog::info("[ToolSwitcher] Requesting tool change to T{}", tool_index);
    }
}

// ============================================================================
// Static XML event callbacks (registered at startup, used in XML if needed)
// ============================================================================

void ToolSwitcherWidget::tool_pill_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_pill_cb");
    if (!s_active_instance) return;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    s_active_instance->handle_tool_selected(idx);
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::tool_compact_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_compact_cb");
    if (s_active_instance) {
        s_active_instance->show_tool_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix

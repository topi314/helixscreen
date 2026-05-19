// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_bypass_spool_widget.h"

#include "theme_manager.h"
#include "ui_spool_canvas.h"

namespace helix::ui {

namespace {
constexpr int32_t BYPASS_SPOOL_SIZE = 48;
constexpr int32_t BOX_PAD = 4;
constexpr int32_t BOX_SIZE = BYPASS_SPOOL_SIZE + BOX_PAD * 2;
constexpr int32_t LABEL_GAP = 4;     // px between box edge and bypass/material labels
constexpr int32_t LABEL_HEIGHT = 16; // estimated label line height for spacing
} // namespace

BypassSpoolWidgets bypass_spool_create(lv_obj_t* parent, void (*on_click)(void*),
                                       void* user_data) {
    BypassSpoolWidgets w;
    if (!parent) {
        return w;
    }

    // Card container — fixed size so layout is deterministic. Floating so the
    // panel can absolute-position it on top of the path canvas.
    w.box = lv_obj_create(parent);
    lv_obj_set_size(w.box, BOX_SIZE, BOX_SIZE);
    lv_obj_set_style_pad_all(w.box, 0, 0);
    lv_obj_set_style_bg_opa(w.box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(w.box, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_radius(w.box, theme_manager_get_spacing("border_radius"), 0);
    lv_obj_set_style_border_width(w.box, 0, 0);
    lv_obj_remove_flag(w.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w.box, LV_OBJ_FLAG_FLOATING);

    w.spool_canvas = ui_spool_canvas_create(w.box, BYPASS_SPOOL_SIZE);
    if (!w.spool_canvas) {
        lv_obj_delete(w.box);
        w = {};
        return w;
    }
    lv_obj_set_pos(w.spool_canvas, BOX_PAD, BOX_PAD);

    if (on_click) {
        lv_obj_add_flag(w.box, LV_OBJ_FLAG_CLICKABLE);
        struct ClickCtx {
            void (*cb)(void*);
            void* user_data;
        };
        // Stash a heap-owned ctx via the LVGL user-data slot. Freed in destroy().
        auto* ctx = new ClickCtx{on_click, user_data};
        lv_obj_set_user_data(w.box, ctx);
        lv_obj_add_event_cb(
            w.box,
            [](lv_event_t* e) {
                auto* c = static_cast<ClickCtx*>(lv_event_get_user_data(e));
                if (c && c->cb) {
                    c->cb(c->user_data);
                }
            },
            LV_EVENT_CLICKED, ctx);
    }

    // "Bypass" label below the box (matches the prior ams_panel behavior).
    w.bypass_label = lv_label_create(parent);
    lv_label_set_text(w.bypass_label, "Bypass");
    lv_obj_set_style_text_color(w.bypass_label, theme_manager_get_color("text"), 0);
    lv_obj_add_flag(w.bypass_label, LV_OBJ_FLAG_FLOATING);

    // Material label above the box. Hidden until a non-empty material is set.
    w.material_label = lv_label_create(parent);
    lv_label_set_text(w.material_label, "");
    lv_obj_set_style_text_color(w.material_label, theme_manager_get_color("text"), 0);
    lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);

    return w;
}

void bypass_spool_destroy(BypassSpoolWidgets& w) {
    if (w.box) {
        // Free the click-context we attached in create().
        if (auto* ud = lv_obj_get_user_data(w.box)) {
            struct ClickCtx {
                void (*cb)(void*);
                void* user_data;
            };
            delete static_cast<ClickCtx*>(ud);
            lv_obj_set_user_data(w.box, nullptr);
        }
        lv_obj_delete(w.box);
    }
    if (w.bypass_label) {
        lv_obj_delete(w.bypass_label);
    }
    if (w.material_label) {
        lv_obj_delete(w.material_label);
    }
    w = {};
}

void bypass_spool_set_color(BypassSpoolWidgets& w, uint32_t color_rgb) {
    if (w.spool_canvas) {
        ui_spool_canvas_set_color(w.spool_canvas, lv_color_hex(color_rgb));
        ui_spool_canvas_redraw(w.spool_canvas);
    }
}

void bypass_spool_set_has_spool(BypassSpoolWidgets& w, bool has_spool) {
    if (w.spool_canvas) {
        ui_spool_canvas_set_fill_level(w.spool_canvas, has_spool ? 0.75f : 0.0f);
        ui_spool_canvas_redraw(w.spool_canvas);
    }
}

void bypass_spool_set_material(BypassSpoolWidgets& w, const char* material) {
    if (!w.material_label) {
        return;
    }
    const bool empty = (material == nullptr) || (material[0] == '\0');
    lv_label_set_text(w.material_label, empty ? "" : material);
    if (empty) {
        lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void bypass_spool_set_position(BypassSpoolWidgets& w, int32_t cx, int32_t cy) {
    if (!w.valid()) {
        return;
    }

    int32_t box_left = cx - BOX_SIZE / 2;
    int32_t box_top = cy - BOX_SIZE / 2;
    lv_obj_set_pos(w.box, box_left, box_top);

    if (w.bypass_label) {
        lv_obj_update_layout(w.bypass_label);
        int32_t label_w = lv_obj_get_width(w.bypass_label);
        lv_obj_set_pos(w.bypass_label, cx - label_w / 2, box_top + BOX_SIZE + LABEL_GAP);
    }
    if (w.material_label) {
        lv_obj_update_layout(w.material_label);
        int32_t label_w = lv_obj_get_width(w.material_label);
        lv_obj_set_pos(w.material_label, cx - label_w / 2,
                       box_top - LABEL_HEIGHT - LABEL_GAP);
    }
}

} // namespace helix::ui

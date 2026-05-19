// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_edit_modal.h"
#include "ui_ams_loading_error_modal.h"
#include "ui_ams_sidebar.h"
#include "ui_bypass_spool_widget.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "ams_state.h"
#include "ams_types.h" // For SlotInfo

#include <chrono>
#include <memory>

/**
 * @file ui_panel_ams.h
 * @brief AMS/Multi-filament panel - slot visualization and operations
 *
 * Displays a Bambu-inspired visualization of multi-filament units (Happy Hare, AFC)
 * with colored slots, status indicators, and load/unload operations.
 *
 * ## UI Layout (480x800 primary target):
 * ```
 * ┌─────────────────────────────────────────┐
 * │ header_bar: "Multi-Filament"            │
 * ├─────────────────────────────────────────┤
 * │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐   │
 * │  │ Slot │ │ Slot │ │ Slot │ │ Slot │   │
 * │  │  0   │ │  1   │ │  2   │ │  3   │   │
 * │  └──────┘ └──────┘ └──────┘ └──────┘   │
 * │                                         │
 * │  [Status: Idle / Loading / etc.]        │
 * │                                         │
 * │  [Action buttons: Unload, Home, etc.]   │
 * └─────────────────────────────────────────┘
 * ```
 *
 * ## Reactive Bindings:
 * - Slot colors: `ams_slot_N_color` (int, RGB packed)
 * - Slot status: `ams_slot_N_status` (int, SlotStatus enum)
 * - Current slot: `ams_current_slot` (int, -1 if none)
 * - Action: `ams_action` (int, AmsAction enum)
 * - Action detail: `ams_action_detail` (string)
 *
 * @see AmsState for subject definitions
 * @see AmsBackend for backend operations
 */
class AmsPanel : public PanelBase {
  public:
    /**
     * @brief Construct AMS panel with dependencies
     * @param printer_state Reference to global helix::PrinterState
     * @param api Pointer to MoonrakerAPI (may be nullptr)
     */
    AmsPanel(helix::PrinterState& printer_state, MoonrakerAPI* api);
    ~AmsPanel() override = default;

    // === PanelBase Interface ===

    void init_subjects() override;
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivate() override;

    [[nodiscard]] const char* get_name() const override {
        return "AMS Panel";
    }

    [[nodiscard]] const char* get_xml_component_name() const override {
        return "ams_panel";
    }

    // === Public API ===

    /**
     * @brief Get the root panel object
     * @return Panel widget, or nullptr if not setup
     */
    [[nodiscard]] lv_obj_t* get_panel() const {
        return panel_;
    }

    /**
     * @brief Refresh slot display from backend state
     *
     * Call this after backend operations complete to update UI.
     * Normally handled automatically via AmsState observer callbacks.
     */
    void refresh_slots();

    /**
     * @brief Clear internal panel reference before UI destruction
     *
     * Called by destroy_ams_panel_ui() before deleting the LVGL object.
     * Clears panel_, slot_widgets_, and other widget references to prevent
     * dangling pointers.
     */
    void clear_panel_reference();

    /**
     * @brief Scope detail view to show only one unit's slots
     * @param unit_index Unit index to show (-1 = all units, default)
     */
    void set_unit_scope(int unit_index);

    /**
     * @brief Clear unit scope, showing all slots
     */
    void clear_unit_scope();

  private:
    // === Slot Management ===

    static constexpr int MAX_VISIBLE_SLOTS =
        16; ///< Max slots displayed (increased for 8+ gate systems)
    lv_obj_t* slot_widgets_[MAX_VISIBLE_SLOTS] = {nullptr};
    lv_obj_t* label_widgets_[MAX_VISIBLE_SLOTS] = {nullptr}; ///< Separate label layer for z-order
    AmsDetailWidgets detail_widgets_;                        ///< Shared component widget pointers

    // === Extracted UI Modules ===

    std::unique_ptr<helix::ui::AmsContextMenu> context_menu_;      ///< Slot context menu
    std::unique_ptr<helix::ui::AmsEditModal> edit_modal_;          ///< Edit filament modal
    std::unique_ptr<helix::ui::AmsLoadingErrorModal> error_modal_; ///< Loading error modal
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;      ///< Shared sidebar component

    /// Cooldown after user dismissal to prevent immediate re-trigger from stale AFC state
    std::chrono::steady_clock::time_point error_modal_dismiss_time_{};

    // === Observers (RAII cleanup via ObserverGuard) ===

    ObserverGuard slots_version_observer_;
    ObserverGuard action_observer_;
    ObserverGuard current_slot_observer_;
    ObserverGuard slot_count_observer_;
    ObserverGuard path_segment_observer_;
    ObserverGuard path_topology_observer_;
    ObserverGuard backend_count_observer_;  ///< For backend selector visibility
    ObserverGuard external_spool_observer_; ///< Reactive updates when external spool color changes
    helix::AsyncLifetimeGuard lifetime_;    ///< Guards deferred callbacks from accessing destroyed panel
    bool backend_rebuild_pending_ = false;  ///< Coalesces rapid backend count changes
    bool slot_creation_pending_ = false;    ///< Coalesces rapid slot count changes
    bool path_update_pending_ = false;      ///< Coalesces rapid path state changes

    // === Dynamic Slot State ===

    int scoped_unit_index_ = -1;    ///< Unit scope: -1 = all units, >=0 = specific unit
    int current_slot_count_ = 0;    ///< Number of slots currently created
    lv_obj_t* slot_grid_ = nullptr; ///< Container for dynamically created slots

    // === Filament Path Canvas ===

    lv_obj_t* path_canvas_ = nullptr; ///< Filament path visualization widget

    // === Bypass Spool Holder (overlay on path canvas) ===
    //
    // Owned via the shared BypassSpoolWidgets helper so the Multi-Filament panel
    // and Multi-Filament Overview panel share one source of truth for the spool
    // box + "Bypass" + material labels. Geometry differs per canvas; we just
    // call set_position with the right (cx, cy).
    helix::ui::BypassSpoolWidgets bypass_widgets_{};

    // === Endless Spool Arrows Canvas ===

    lv_obj_t* endless_arrows_ = nullptr; ///< Endless spool backup chain visualization

    // === Backend Selector State ===

    int active_backend_idx_ = 0; ///< Currently selected backend index

    // === Backend Selector Helpers ===

    void rebuild_backend_selector();
    void on_backend_segment_selected(int index);

    // === Setup Helpers ===

    void setup_system_header();
    void setup_slots();
    void setup_path_canvas();
    void update_path_canvas_from_backend();
    void setup_bypass_spool();
    void update_bypass_spool_position();
    void update_bypass_spool_from_state();
    void setup_endless_arrows();
    void update_endless_arrows_from_backend();

    /**
     * @brief Create slot widgets dynamically based on slot count
     * @param count Number of slots to create (0 to max 8)
     *
     * Deletes existing slots and creates new ones. Uses lv_xml_create()
     * to instantiate ams_slot C++ widgets, then sets their slot_index.
     */
    void create_slots(int count);

    // === Slot Count Observer ===
    // on_slot_count_changed migrated to lambda observer factory

    // === UI Update Handlers ===

    void update_slot_colors();
    void update_slot_status(int slot_index);
    void update_current_slot_highlight(int slot_index);

    // === Event Callbacks (static trampolines) ===

    static void on_slot_clicked(lv_event_t* e);

    // === Observer Callbacks ===
    // All observer callbacks migrated to lambda observer factory in init_subjects()

    // === Path Canvas Callbacks ===

    static void on_path_slot_clicked(int slot_index, void* user_data);
    static void on_bypass_spool_clicked(void* user_data);

    /**
     * @brief Handle click on bypass spool box in path canvas
     *
     * Opens the edit modal for the external spool (slot_index -2).
     */
    void handle_bypass_spool_click();

    static void on_buffer_clicked(void* user_data);
    void handle_buffer_click();

    // === Spoolman Integration ===

    void sync_spoolman_active_spool();

    // === UI Module Helpers (internal, show modals with callbacks) ===

    void show_context_menu(int slot_index, lv_obj_t* near_widget, lv_point_t click_pt);
    void show_edit_modal(int slot_index);
    void show_loading_error_modal();

    // === Action Handlers (public for XML event callbacks) ===
  public:
    void handle_slot_tap(int slot_index, lv_point_t click_pt);
};

/**
 * @brief Get global AMS panel singleton
 *
 * Creates the panel on first call, returns cached instance thereafter.
 * Panel is lazily initialized - widgets registered and XML created on first access.
 *
 * @return Reference to global AmsPanel instance
 */
AmsPanel& get_global_ams_panel();

/**
 * @brief Destroy the AMS panel UI to free memory
 *
 * Deletes the LVGL panel object and canvas buffers. The C++ AmsPanel
 * object and widget registrations remain for quick recreation.
 * Call this when the panel is closed to free memory on embedded systems.
 */
void destroy_ams_panel_ui();

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include "macro_param_modal.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @file ui_panel_macros.h
 * @brief Klipper macro execution panel
 *
 * Displays all available Klipper macros and allows single-tap execution.
 * Macros are fetched from PrinterCapabilities after discovery.
 *
 * ## Features
 * - Lists all detected gcode_macro entries from Klipper
 * - Filters system macros (_* prefix) by default
 * - Executes macros via MoonrakerAPI::execute_gcode()
 * - Empty state when no macros available
 *
 * ## Usage
 * Panel is accessed via navigation from controls or settings panel.
 * Uses `macro_card.xml` component for each macro entry.
 */
class MacrosPanel : public OverlayBase {
  public:
    MacrosPanel();
    ~MacrosPanel() override;

    // === OverlayBase interface ===
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Macros";
    }

    // === Lifecycle hooks ===
    void on_activate() override;
    void on_deactivate() override;

  protected:
    /**
     * @brief Null cached widget pointers after the overlay tree is freed.
     *
     * OverlayBase::destroy_overlay_ui() async-deletes overlay_root_ and then
     * calls this hook. Because MacrosPanel is a singleton that outlives its
     * widgets, the cached child pointers (macro_list_container_, etc.) would
     * otherwise dangle — a deferred populate_macro_list() then dereferences a
     * freed container in lv_obj_update_layout()/lv_obj_get_screen() (SIGSEGV).
     */
    void on_ui_destroyed() override;

  public:
    // === Public API ===
    lv_obj_t* get_panel() const {
        return overlay_root_;
    }

    /**
     * @brief Static callback for macro card clicks
     *
     * Registered globally via lv_xml_register_event_cb().
     * Routes to instance method via global accessor.
     */
    static void on_macro_card_clicked(lv_event_t* e);

  private:
    /**
     * @brief Information about a displayed macro
     */
    struct MacroEntry {
        lv_obj_t* card = nullptr;  ///< The macro_card widget
        std::string name;          ///< Macro name (uppercase)
        std::string display_name;  ///< Display name (prettified)
        bool is_system = false;    ///< True if _* prefix
        bool is_dangerous = false; ///< True if potentially destructive
    };

    /**
     * @brief Populate the macro list from capabilities
     */
    void populate_macro_list();

    /**
     * @brief Create a macro card widget
     * @param macro_name The macro name to display
     */
    void create_macro_card(const std::string& macro_name);

    /**
     * @brief Clear all macro cards
     */
    void clear_macro_list();

    /**
     * @brief Execute a macro by name (no parameters)
     * @param macro_name The macro to execute (e.g., "CLEAN_NOZZLE")
     */
    void execute_macro(const std::string& macro_name);

    /**
     * @brief Fetch macro template, parse params, and execute or show param modal.
     * For dangerous macros, shows confirmation first.
     * @param macro_name The macro to query and execute
     */
    void fetch_params_and_execute(const std::string& macro_name);

    /**
     * @brief Internal: fetch params and run (after any confirmation).
     * @param macro_name The macro to query and execute
     */
    void fetch_params_and_run(const std::string& macro_name);

    /**
     * @brief Execute a macro with parameter values
     * @param macro_name The macro name
     * @param params Map of parameter name to value
     */
    void execute_with_params(const std::string& macro_name, const helix::MacroParamResult& result);

    /**
     * @brief Prettify a macro name for display
     *
     * Converts "CLEAN_NOZZLE" to "Clean Nozzle", handles prefixes.
     *
     * @param name Raw macro name
     * @return Prettified display name
     */
    static std::string prettify_macro_name(const std::string& name);

    /**
     * @brief Toggle system macro visibility
     * @param show_system If true, show _* macros
     */
    void set_show_system_macros(bool show_system);

    // Widget references
    lv_obj_t* macro_list_container_ = nullptr;  ///< Scrollable container for macro cards
    lv_obj_t* empty_state_container_ = nullptr; ///< Shown when no macros
    lv_obj_t* status_label_ = nullptr;          ///< Status message label
    lv_obj_t* system_toggle_ = nullptr;         ///< Toggle for showing system macros

    // Flags
    bool callbacks_registered_ = false;

    // Data
    std::vector<MacroEntry> macro_entries_; ///< All displayed macro cards
    bool show_system_macros_ = false;       ///< Whether to show _* macros

    // Macro parameter modal and dangerous macro confirmation
    helix::MacroParamModal param_modal_;
    std::string pending_dangerous_macro_; ///< Macro awaiting danger confirmation
    std::string pending_run_macro_;       ///< Macro awaiting generic run confirmation

    // Subjects
    SubjectManager subjects_;
    char status_buf_[64] = {};
    lv_subject_t status_subject_{};
};

/**
 * @brief Get the global MacrosPanel instance
 *
 * Creates the instance on first call. Used by static callbacks.
 *
 * @return Reference to singleton MacrosPanel
 */
MacrosPanel& get_global_macros_panel();

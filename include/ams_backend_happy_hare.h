// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "slot_registry.h"

#include <optional>
#include <string>

/**
 * @file ams_backend_happy_hare.h
 * @brief Happy Hare MMU backend implementation
 *
 * Implements the AmsBackend interface for Happy Hare MMU systems.
 * Communicates with Moonraker to control the MMU via G-code commands
 * and receives state updates via printer.mmu.* subscriptions.
 *
 * Happy Hare Moonraker Variables:
 * - printer.mmu.gate       (int): Current gate (-1=none, -2=bypass)
 * - printer.mmu.tool       (int): Current tool
 * - printer.mmu.filament   (string): "Loaded" or "Unloaded"
 * - printer.mmu.action     (string): "Idle", "Loading", etc.
 * - printer.mmu.gate_status (array[int]): -1=unknown, 0=empty, 1=available, 2=from_buffer
 * - printer.mmu.gate_color_rgb (array[int]): RGB values like 0xFF0000
 * - printer.mmu.gate_material (array[string]): "PLA", "PETG", etc.
 *
 * G-code Commands:
 * - MMU_LOAD GATE={n}   - Load filament from specified gate
 * - MMU_UNLOAD          - Unload current filament
 * - MMU_SELECT GATE={n} - Select gate without loading
 * - T{n}                - Tool change (unload + load)
 * - MMU_HOME            - Home the selector
 * - MMU_RECOVER         - Attempt error recovery
 */
class AmsBackendHappyHare : public AmsSubscriptionBackend {
  public:
    /**
     * @brief Construct Happy Hare backend
     *
     * @param api Pointer to MoonrakerAPI (for sending G-code commands)
     * @param client Pointer to helix::MoonrakerClient (for subscribing to updates)
     *
     * @note Both pointers must remain valid for the lifetime of this backend.
     */
    AmsBackendHappyHare(MoonrakerAPI* api, helix::MoonrakerClient* client);
    ~AmsBackendHappyHare() override;

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] AmsType get_type() const override;
    [[nodiscard]] bool manages_active_spool() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization
    [[nodiscard]] int get_bowden_progress() const override {
        return bowden_progress_;
    }
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;
    [[nodiscard]] bool slot_has_prep_sensor(int slot_index) const override;

    // Operations
    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError reset_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_reset() const override {
        return true;
    }
    AmsError eject_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_eject() const override {
        return true;
    }
    /**
     * @brief Move the selector to a gate without loading filament (MMU_SELECT).
     */
    AmsError select_gate(int slot_index) override;
    [[nodiscard]] bool supports_gate_select() const override {
        return true;
    }
    [[nodiscard]] bool supports_gate_check() const override {
        return true;
    }
    /**
     * @brief Probe a single gate's sensor (MMU_CHECK_GATE GATE=n).
     */
    AmsError check_gate(int slot_index) override;
    /**
     * @brief Probe all gate sensors (MMU_CHECK_GATE, no params).
     */
    AmsError check_all_gates() override;
    AmsError cancel() override;

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass mode
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // Endless Spool support (read-only - configured in Happy Hare config)
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;
    [[nodiscard]] std::vector<helix::printer::EndlessSpoolConfig>
    get_endless_spool_config() const override;
    AmsError set_endless_spool_backup(int slot_index, int backup_slot) override;

    /**
     * @brief Reset all tool mappings to defaults
     *
     * Resets tool-to-gate mappings to 1:1 (T0->Gate0, T1->Gate1, etc.)
     * by iterating through all tools and calling set_tool_mapping().
     *
     * @return AmsError with result
     */
    AmsError reset_tool_mappings() override;

    /**
     * @brief Reset all endless spool backup mappings
     *
     * Happy Hare endless spool is read-only (configured in mmu_vars.cfg).
     * Returns not_supported error.
     *
     * @return AmsError with not_supported result
     */
    AmsError reset_endless_spool() override;

    // Dryer support (v4 - KMS/EMU hardware with heaters)
    [[nodiscard]] DryerInfo get_dryer_info() const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;

    [[nodiscard]] bool has_firmware_spool_persistence() const override {
        return true; // Happy Hare persists via MMU_GATE_MAP SPOOLID
    }

    // Tool Mapping support
    /**
     * @brief Get tool mapping capabilities for Happy Hare
     *
     * Happy Hare supports tool-to-gate mapping via MMU_TTG_MAP G-code.
     *
     * @return Capabilities with supported=true, editable=true
     */
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;

    /**
     * @brief Get current tool-to-slot mapping
     *
     * Returns the tool_to_slot_map from system_info_ (populated from ttg_map).
     *
     * @return Vector where index=tool, value=slot
     */
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // Device Management
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    // Allow test helper access to private members
    friend class AmsBackendHappyHareTestHelper;
    friend class AmsBackendHappyHareEndlessSpoolHelper;
    friend class AmsBackendHHMultiUnitHelper;
    friend class HappyHareErrorStateHelper;
    friend class HappyHareCharHelper;
    friend class HHToolchangeTestHelper;

    // --- AmsSubscriptionBackend hooks ---
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS HappyHare]";
    }

  private:
    /**
     * @brief Parse MMU state from Moonraker JSON
     *
     * Extracts mmu object from notification and updates system_info_.
     *
     * @param mmu_data JSON object containing printer.mmu data
     */
    void parse_mmu_state(const nlohmann::json& mmu_data);

    /**
     * @brief Initialize slot structures based on gate_status array size
     *
     * Called when we first receive gate_status to create the correct
     * number of SlotInfo entries.
     *
     * @param gate_count Number of gates detected
     */
    void initialize_slots(int gate_count);

    /**
     * @brief Validate gate index is within range
     *
     * @param gate_index Slot index to validate
     * @return AmsError (SUCCESS if valid, INVALID_GATE otherwise)
     */
    AmsError validate_slot_index(int gate_index) const;

    /**
     * @brief Query configfile.settings.mmu to determine tip method
     *
     * Reads form_tip_macro from Happy Hare config via Moonraker.
     * If macro name contains "cut", sets TipMethod::CUT (e.g., _MMU_CUT_TIP).
     * Otherwise sets TipMethod::TIP_FORM (e.g., _MMU_FORM_TIP).
     * Called once during start().
     */
    void query_tip_method_from_config();

    /**
     * @brief Query configfile.settings.mmu_machine to determine selector type
     *
     * Reads selector_type from Happy Hare config via Moonraker.
     * VirtualSelector = Type B (HUB topology), all others = Type A (LINEAR).
     * Called once during on_started().
     */
    void query_selector_type_from_config();

    /**
     * @brief Check if this is a Type B MMU (hub topology)
     * @return true if selector_type is VirtualSelector
     */
    [[nodiscard]] bool is_type_b() const;

    /**
     * @brief Update topology on all existing units after selector_type is known
     */
    void update_unit_topologies();

    std::string selector_type_; ///< Selector type from config (e.g., "VirtualSelector" for Type B)

    // Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;

    // Cached MMU state
    helix::printer::SlotRegistry slots_;    ///< Single source of truth for per-slot state
    int num_units_{1};                      ///< Number of physical units (default 1)
    std::vector<int> per_unit_gate_counts_; ///< Per-unit gate counts for dissimilar multi-MMU (v4)
    int active_unit_{0};                    ///< Currently active MMU unit (v4)

    // Path visualization state
    int filament_pos_{0};     ///< Happy Hare filament_pos value
    int bowden_progress_{-1}; ///< Bowden loading progress 0-100% (-1=unavailable, v4)
    PathSegment error_segment_{PathSegment::NONE}; ///< Inferred error location

    // Dryer state (v4 - KMS/EMU hardware)
    DryerInfo dryer_info_;

    // Error state tracking
    std::string reason_for_pause_; ///< Last reason_for_pause from MMU (descriptive error text)

    // --- Config defaults from configfile.settings.mmu ---

    /// Cached config defaults parsed from configfile.settings.mmu
    struct ConfigDefaults {
        float gear_from_buffer_speed = 150.0f;
        float gear_from_spool_speed = 60.0f;
        float gear_unload_speed = 80.0f;
        float selector_move_speed = 200.0f;
        float extruder_load_speed = 45.0f;
        float extruder_unload_speed = 45.0f;
        float toolhead_sensor_to_nozzle = 62.0f;
        float toolhead_extruder_to_nozzle = 72.0f;
        float toolhead_entry_to_extruder = 0.0f;
        float toolhead_ooze_reduction = 2.0f;
        int sync_to_extruder = 0;
        int clog_detection = 0;
        bool loaded = false;
    };
    ConfigDefaults config_defaults_;

    /// User overrides (set via UI, persisted to Config)
    struct UserOverrides {
        std::optional<float> gear_from_buffer_speed;
        std::optional<float> gear_from_spool_speed;
        std::optional<float> gear_unload_speed;
        std::optional<float> selector_move_speed;
        std::optional<float> extruder_load_speed;
        std::optional<float> extruder_unload_speed;
        std::optional<float> toolhead_sensor_to_nozzle;
        std::optional<float> toolhead_extruder_to_nozzle;
        std::optional<float> toolhead_entry_to_extruder;
        std::optional<float> toolhead_ooze_reduction;
        std::optional<int> sync_to_extruder;
        std::optional<int> clog_detection;
    };
    UserOverrides user_overrides_;

    // Status-backed values (from printer.mmu.* subscriptions)
    std::string led_exit_effect_;
    std::string espooler_active_;
    int flowguard_encoder_mode_ = -1; ///< -1 = not yet received from Moonraker

    void query_config_defaults();
    void load_persisted_overrides();
    void save_override(const std::string& key, float value);
    void save_override(const std::string& key, int value);
    void reapply_overrides();

    /// Get the config default float for a given action key
    [[nodiscard]] float get_config_default_float(const std::string& key) const;
    /// Get the config default int for a given action key
    [[nodiscard]] int get_config_default_int(const std::string& key) const;
};

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_backend.h
 * @brief Abstract platform-independent interface for multi-filament system operations
 *
 * @pattern Pure virtual interface + static create()/create_auto() factory methods
 * @threading Implementation-dependent; see concrete implementations
 *
 * @see ams_backend_happyhare.cpp, ams_backend_afc.cpp
 */

#pragma once

#include "ams_error.h"
#include "ams_types.h"

class MoonrakerAPI;
namespace helix {
class MoonrakerClient;
}

#include <any>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Abstract interface for AMS/MMU backend implementations
 *
 * Provides a platform-agnostic API for multi-filament operations.
 * Concrete implementations handle system-specific details:
 * - AmsBackendHappyHare: Happy Hare MMU via Moonraker
 * - AmsBackendAfc: AFC-Klipper-Add-On via Moonraker
 * - AmsBackendMock: Simulator mode with fake data
 *
 * Design principles:
 * - Hide all backend-specific commands/protocols from AmsManager
 * - Provide async operations with event-based completion
 * - Thread-safe operations where needed
 * - Clean error handling with user-friendly messages
 */
class AmsBackend {
  public:
    virtual ~AmsBackend() = default;

    // ========================================================================
    // Event Types
    // ========================================================================

    /**
     * @brief Standard AMS event types
     *
     * Events are delivered asynchronously via registered callbacks.
     * Event names are strings to allow backend-specific extensions.
     */
    static constexpr const char* EVENT_STATE_CHANGED = "STATE_CHANGED"; ///< System state updated
    static constexpr const char* EVENT_SLOT_CHANGED = "SLOT_CHANGED";   ///< Slot info updated
    static constexpr const char* EVENT_LOAD_COMPLETE = "LOAD_COMPLETE"; ///< Load operation finished
    static constexpr const char* EVENT_UNLOAD_COMPLETE =
        "UNLOAD_COMPLETE";                                            ///< Unload operation finished
    static constexpr const char* EVENT_TOOL_CHANGED = "TOOL_CHANGED"; ///< Tool change completed
    static constexpr const char* EVENT_ERROR = "ERROR";               ///< Error occurred
    static constexpr const char* EVENT_ATTENTION_REQUIRED =
        "ATTENTION"; ///< User intervention needed

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    /**
     * @brief Initialize and start the AMS backend
     *
     * Connects to the underlying AMS system and starts monitoring state.
     * For real backends, this initiates Moonraker subscriptions.
     * For mock backend, this sets up simulated state.
     *
     * @return AmsError with detailed status information
     */
    virtual AmsError start() = 0;

    /**
     * @brief Stop the AMS backend
     *
     * Cleanly shuts down monitoring and releases resources.
     * Safe to call even if not started.
     */
    virtual void stop() = 0;

    /**
     * @brief Release subscriptions without unsubscribing
     *
     * Use during shutdown when the helix::MoonrakerClient may already be destroyed.
     * This abandons the subscription rather than trying to call into the client.
     * Backends that hold SubscriptionGuards should call release() on them.
     */
    virtual void release_subscriptions() {}

    /**
     * @brief Re-fetch authoritative slot/state from the printer.
     *
     * Called from UI sites where the user expects a fresh view of slot state
     * (e.g., entering the filament assignment screen). Lets users self-recover
     * from any drift between cached UI state and printer truth without a
     * full reconnect.
     *
     * Safe to call repeatedly — implementations should debounce/coalesce.
     * Default: no-op. Override in backends with a meaningful resync path.
     */
    virtual void request_resync() {}

    /**
     * @brief Check if backend is currently running/initialized
     * @return true if backend is active and ready for operations
     */
    [[nodiscard]] virtual bool is_running() const = 0;

    // ========================================================================
    // Event System
    // ========================================================================

    /**
     * @brief Callback type for AMS events
     *
     * @param event_name Event identifier (EVENT_* constants)
     * @param data Event-specific payload (JSON string or empty)
     */
    using EventCallback =
        std::function<void(const std::string& event_name, const std::string& data)>;

    /**
     * @brief Register callback for AMS events
     *
     * Events are delivered asynchronously and may arrive from background threads.
     * The callback should be thread-safe or post to main thread.
     *
     * @param callback Handler function for events
     */
    virtual void set_event_callback(EventCallback callback) = 0;

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * @brief Get current AMS system information
     *
     * Returns a snapshot of the current system state including:
     * - System type and version
     * - Current tool/slot selection
     * - All unit and slot information
     * - Capability flags
     *
     * @return Current AmsSystemInfo (copy, safe for caller to hold)
     */
    [[nodiscard]] virtual AmsSystemInfo get_system_info() const = 0;

    /**
     * @brief Get the detected AMS type
     * @return AmsType enum value
     */
    [[nodiscard]] virtual AmsType get_type() const = 0;

    /**
     * @brief Whether this backend manages the Spoolman active spool itself
     *
     * Some backends (e.g., AFC) call spoolman_set_active_spool on tool
     * load/unload natively. When true, HelixScreen must NOT call
     * server.spoolman.post_spool_id to avoid racing with the backend.
     *
     * @return true if the backend manages active spool tracking
     */
    [[nodiscard]] virtual bool manages_active_spool() const {
        return false;
    }

    /**
     * @brief Whether this backend tracks filament weight locally
     *
     * Some backends (e.g., AFC, Happy Hare) track filament consumption via
     * extruder position and update slot weight in real time. When true,
     * HelixScreen must NOT overwrite slot weights from Spoolman polling,
     * because Spoolman's weight is stale (backends don't write back to it).
     *
     * @return true if the backend provides live weight tracking
     */
    [[nodiscard]] virtual bool tracks_weight_locally() const {
        return false;
    }

    /**
     * @brief Get information about a specific slot
     * @param slot_index Slot index (0 to total_slots-1)
     * @return SlotInfo struct (copy, safe for caller to hold)
     */
    [[nodiscard]] virtual SlotInfo get_slot_info(int slot_index) const = 0;

    /**
     * @brief Get current action/operation status
     * @return Current AmsAction enum value
     */
    [[nodiscard]] virtual AmsAction get_current_action() const = 0;

    /**
     * @brief Get currently selected tool number
     * @return Tool number (-1 if none, -2 for bypass on Happy Hare)
     */
    [[nodiscard]] virtual int get_current_tool() const = 0;

    /**
     * @brief Get currently selected slot number
     * @return Slot number (-1 if none, -2 for bypass on Happy Hare)
     */
    [[nodiscard]] virtual int get_current_slot() const = 0;

    /**
     * @brief Slot index currently sourced by the given extruder. Backends that model
     * per-extruder attribution (tool-changers with one spool per tool) override to
     * return the tool->slot mapping. Default returns nullopt; callers fall back to
     * aggregate filament_used_mm + current_slot().
     * @param extruder_idx 0-based extruder index (0 = primary, 1 = extruder1, ...)
     */
    [[nodiscard]] virtual std::optional<int> slot_for_extruder(int extruder_idx) const {
        (void)extruder_idx;
        return std::nullopt;
    }

    /**
     * @brief True when this backend already populates remaining_weight_g from a live
     * printer-side source. FilamentConsumptionTracker skips slots on such backends
     * to avoid double-counting.
     */
    [[nodiscard]] virtual bool tracks_consumption_natively() const {
        return false;
    }

    /**
     * @brief Check if filament is currently loaded in extruder
     * @return true if filament is loaded
     */
    [[nodiscard]] virtual bool is_filament_loaded() const = 0;

    // ========================================================================
    // Filament Path Visualization
    // ========================================================================

    /**
     * @brief Get the path topology for this AMS system
     *
     * Determines how the filament path is rendered:
     * - LINEAR: Selector picks from multiple gates (Happy Hare ERCF)
     * - HUB: Multiple lanes merge through a hub (AFC Box Turtle)
     *
     * @return PathTopology enum value
     */
    [[nodiscard]] virtual PathTopology get_topology() const = 0;

    /**
     * @brief Get the path topology for a specific unit
     *
     * In mixed-topology systems (e.g., Box Turtle + OpenAMS), different units
     * may have different topologies. This method returns the topology for a
     * specific unit by index.
     *
     * Default implementation falls back to get_topology() for backward compat.
     *
     * @param unit_index Index of the unit (0-based)
     * @return PathTopology for this unit, or system-wide topology if unknown
     */
    [[nodiscard]] virtual PathTopology get_unit_topology(int unit_index) const {
        (void)unit_index;
        return get_topology();
    }

    /**
     * @brief Get current filament position in the path
     *
     * Returns which segment the filament is currently at/in.
     * Used for highlighting the active portion of the path visualization.
     *
     * @return PathSegment enum value (NONE if no filament in system)
     */
    [[nodiscard]] virtual PathSegment get_filament_segment() const = 0;

    /**
     * @brief Get filament position for a specific slot
     *
     * Returns how far filament from a specific slot extends into the path.
     * Used for visualizing all installed filaments, not just the active one.
     * For non-active slots, this typically shows filament up to the prep sensor.
     *
     * @param slot_index Slot index (0 to total_slots-1)
     * @return PathSegment enum value (NONE if no filament installed at slot)
     */
    [[nodiscard]] virtual PathSegment get_slot_filament_segment(int slot_index) const = 0;

    /**
     * @brief Infer which segment has an error
     *
     * When an error occurs, this determines which segment of the path
     * is most likely the problem area based on sensor states and
     * current operation. Used for visual error highlighting.
     *
     * @return PathSegment enum value (NONE if no error or can't determine)
     */
    [[nodiscard]] virtual PathSegment infer_error_segment() const = 0;

    /**
     * @brief Get bowden loading progress percentage
     *
     * Returns the firmware-reported bowden loading progress (0-100%).
     * Happy Hare v4 provides this via printer.mmu.bowden_progress.
     * When available (>= 0), AmsState uses it to drive path_anim_progress_subject
     * instead of UI-controlled animation.
     *
     * @return 0-100 for real progress, -1 if not available (v3 or non-HH backends)
     */
    [[nodiscard]] virtual int get_bowden_progress() const {
        return -1;
    }

    /**
     * @brief Check if a specific slot has a prep/pre-gate sensor
     *
     * Returns whether the given slot has a prep sensor that can detect
     * filament presence. Used by the path canvas to decide whether to
     * draw a prep sensor dot for each slot.
     *
     * Default implementation returns false (no prep sensor).
     *
     * @param slot_index Slot index (0 to total_slots-1)
     * @return true if slot has a prep/pre-gate sensor
     */
    [[nodiscard]] virtual bool slot_has_prep_sensor(int slot_index) const {
        (void)slot_index;
        return false;
    }

    // ========================================================================
    // Filament Operations
    // ========================================================================

    /**
     * @brief Whether the UI should redirect to the AMS panel for slot selection
     *        before loading filament.
     *
     * When true, the filament panel navigates to the AMS management UI so the
     * user can pick a specific slot. When false, the UI falls through to the
     * standard LOAD_FILAMENT macro or raw G-code (e.g. bypass mode where the
     * user is feeding filament directly).
     *
     * Default: true (most backends need slot selection). Override in backends
     * where bypass or other modes allow loading without slot selection.
     */
    [[nodiscard]] virtual bool requires_slot_selection_for_load() const {
        return !is_bypass_active();
    }

    /**
     * @brief Whether a load operation must first unload/cut the currently
     *        present filament (load-vs-swap decision).
     *
     * Returns true when filament is physically at the nozzle (or, for most
     * backends, when a slot is otherwise reported as engaged) so the caller
     * picks the cut-before-load (swap) path instead of a fresh load. Centralized
     * here so the UI and backends agree on the rule and per-variant quirks
     * (e.g. K1 CFS preloads current_slot with an empty nozzle) live in one place.
     *
     * Default: filament at nozzle OR a slot engaged. Override where the
     * current_slot signal does not imply filament at the nozzle.
     */
    [[nodiscard]] virtual bool needs_unload_before_load(const AmsSystemInfo& info) const {
        return info.filament_loaded || info.current_slot >= 0;
    }

    /**
     * @brief Load filament from specified slot (async)
     *
     * Initiates filament load from the specified slot to the extruder.
     * Results delivered via EVENT_LOAD_COMPLETE or EVENT_ERROR.
     *
     * Requires:
     * - System not busy with another operation
     * - Slot has filament available
     * - Extruder at appropriate temperature
     *
     * @param slot_index Slot to load from (0-based)
     * @return AmsError indicating if operation was started successfully
     */
    virtual AmsError load_filament(int slot_index) = 0;

    /**
     * @brief Unload filament (async)
     *
     * Initiates filament unload from extruder back to its slot.
     * Results delivered via EVENT_UNLOAD_COMPLETE or EVENT_ERROR.
     *
     * @param slot_index Slot to unload (-1 = unload current/default).
     *        On toolchangers, specifies which tool to unmount.
     *        On single-extruder systems, ignored (only one thing loaded).
     *
     * Requires:
     * - Filament currently loaded
     * - System not busy with another operation
     * - Extruder at appropriate temperature
     *
     * @return AmsError indicating if operation was started successfully
     */
    virtual AmsError unload_filament(int slot_index = -1) = 0;

    /**
     * @brief Select tool/slot without loading (async)
     *
     * Moves the selector to the specified slot without loading filament.
     * Used for preparation or manual operations.
     *
     * @param slot_index Slot to select (0-based)
     * @return AmsError indicating if operation was started successfully
     */
    virtual AmsError select_slot(int slot_index) = 0;

    /**
     * @brief Perform tool change (async)
     *
     * Complete tool change sequence: unload current, load new.
     * Equivalent to sending T{tool_number} command.
     * Results delivered via EVENT_TOOL_CHANGED or EVENT_ERROR.
     *
     * @param tool_number Tool to change to (0-based)
     * @return AmsError indicating if operation was started successfully
     */
    virtual AmsError change_tool(int tool_number) = 0;

    // ========================================================================
    // Recovery Operations
    // ========================================================================

    /**
     * @brief Attempt recovery from error state
     *
     * Initiates system recovery procedure appropriate to current error.
     * For Happy Hare, this typically invokes MMU_RECOVER.
     *
     * @return AmsError indicating if recovery was started
     */
    virtual AmsError recover() = 0;

    /**
     * @brief Reset the AMS system (async)
     *
     * Resets the system to a known good state.
     * - Happy Hare: Calls MMU_HOME to home the selector
     * - AFC: Calls AFC_RESET to reset the system
     *
     * @return AmsError indicating if operation was started
     */
    virtual AmsError reset() = 0;

    /**
     * @brief Reset a specific lane/slot
     *
     * Resets an individual lane to a known good state without affecting others.
     * Default implementation returns NOT_SUPPORTED.
     *
     * @param slot_index Lane to reset (0-based)
     * @return AmsError indicating if operation was started
     */
    virtual AmsError reset_lane(int slot_index) {
        (void)slot_index;
        return AmsErrorHelper::not_supported("Per-lane reset not supported");
    }

    /**
     * @brief Check if per-lane reset is supported
     * @return true if reset_lane() is implemented
     */
    [[nodiscard]] virtual bool supports_lane_reset() const {
        return false;
    }

    /**
     * @brief Eject filament from a specific lane (async)
     *
     * Reverses the lane's extruder motor to release filament so the spool
     * can be physically removed. Different from unload_filament() which
     * retracts filament from the toolhead back through the hub.
     *
     * For AFC: sends LANE_UNLOAD LANE={name}
     * Default implementation returns NOT_SUPPORTED.
     *
     * @param slot_index Lane to eject from (0-based)
     * @return AmsError indicating if operation was started
     */
    virtual AmsError eject_lane(int slot_index) {
        (void)slot_index;
        return AmsErrorHelper::not_supported("Lane eject not supported");
    }

    /**
     * @brief Check if per-lane eject is supported
     * @return true if eject_lane() is implemented
     */
    [[nodiscard]] virtual bool supports_lane_eject() const {
        return false;
    }

    /**
     * @brief Whether the backend can position the selector at a gate without loading.
     * @return true if select_gate() is implemented (selector-based systems only).
     */
    [[nodiscard]] virtual bool supports_gate_select() const {
        return false;
    }

    /**
     * @brief Move the selector to a gate without loading filament.
     *
     * Positions the MMU selector at the given gate without loading filament
     * into the toolhead. This is useful for manual interventions and gate
     * inspection on selector-based systems.
     *
     * @param slot_index Zero-based gate index.
     * @return AmsError indicating success or failure.
     */
    virtual AmsError select_gate(int slot_index) {
        (void)slot_index;
        return AmsErrorHelper::not_supported("Gate select not supported");
    }

    /**
     * @brief Whether the backend can probe gate sensors (MMU_CHECK_GATE).
     * @return true if check_gate()/check_all_gates() are implemented.
     */
    [[nodiscard]] virtual bool supports_gate_check() const {
        return false;
    }

    /**
     * @brief Probe a single gate's filament sensor (MMU_CHECK_GATE GATE=n).
     *
     * Asks the firmware to check whether filament is present at the specified
     * gate's sensor. Useful for diagnosing gate-status discrepancies without
     * a full load/unload cycle.
     *
     * Default implementation returns NOT_SUPPORTED.
     *
     * @param slot_index Zero-based gate index.
     * @return AmsError indicating success or failure.
     */
    virtual AmsError check_gate(int slot_index) {
        (void)slot_index;
        return AmsErrorHelper::not_supported("Gate check not supported");
    }

    /**
     * @brief Probe all gate sensors at once (MMU_CHECK_GATE, no params).
     *
     * Asks the firmware to check filament presence at every gate sensor
     * in one pass. Useful for a full gate-status audit.
     *
     * Default implementation returns NOT_SUPPORTED.
     *
     * @return AmsError indicating success or failure.
     */
    virtual AmsError check_all_gates() {
        return AmsErrorHelper::not_supported("Gate check not supported");
    }

    /**
     * @brief Cancel current operation
     *
     * Attempts to safely abort the current operation.
     * Not all operations can be cancelled.
     *
     * @return AmsError indicating if cancellation was accepted
     */
    virtual AmsError cancel() = 0;

    // ========================================================================
    // Resume Preparation
    // ========================================================================

    /**
     * @brief Completion callback for prepare_for_resume()
     *
     * Always invoked on the main thread. Pass an AmsError with
     * AmsResult::SUCCESS to proceed with RESUME; any other result aborts
     * the resume sequence and surfaces a user-facing error.
     */
    using ResumeReadyCallback = std::function<void(const AmsError&)>;

    /**
     * @brief Backend-side preparation hook fired before a print Resume.
     *
     * Lets a backend run any device-specific recovery gcode (heating,
     * motor-engagement, encoder priming, etc.) before the caller dispatches
     * the RESUME macro. Implementations may detect their own stuck states
     * and either run a recovery sequence or no-op. The default implementation
     * is a no-op that invokes the callback immediately — appropriate when
     * Klipper's stock RESUME path is sufficient.
     *
     * Contract: on_ready is always invoked exactly once. It fires on the
     * main thread regardless of where the backend's gcode execution lands.
     * Pass AmsResult::SUCCESS to indicate the caller may proceed with
     * RESUME; any other result aborts the resume sequence and surfaces a
     * user-facing error.
     *
     * @param slot_index Active slot the caller intends to resume on
     *                   (-1 if no AMS context — backend may fall back to
     *                   whatever it considers "current")
     * @param on_ready   Callback fired when preparation is complete or
     *                   has failed
     */
    virtual void prepare_for_resume(int slot_index, ResumeReadyCallback on_ready) {
        (void)slot_index;
        if (on_ready) {
            on_ready(AmsErrorHelper::success());
        }
    }

    /**
     * @brief True when the current runout signal looks like a stale-sensor
     *        false positive rather than a genuine filament-out.
     *
     * On Snapmaker U1 the encoder-based motion sensor latches
     * filament_detected=false whenever no extrusion has happened recently —
     * so it can fire pause_on_runout at print start, before filament has
     * physically moved. The port/buffer sensor at the spool side still reads
     * filament present in that case. Callers use this to suppress the runout
     * guidance modal and auto-trigger prepare_for_resume + RESUME silently.
     *
     * Default: false (treat every runout signal as real).
     *
     * @param slot_index Slot to check (typically current active slot)
     */
    [[nodiscard]] virtual bool is_stuck_motion_sensor_runout(int slot_index) const {
        (void)slot_index;
        return false;
    }

    // ========================================================================
    // Configuration Operations
    // ========================================================================

    /**
     * @brief Update slot filament information
     *
     * Sets the color, material, and other filament info for a slot.
     *
     * When persist=true (default), changes are written to firmware via G-code
     * commands (e.g., SET_COLOR, SET_MATERIAL, SET_SPOOL_ID for AFC) so they
     * survive reboots. Use this for user-initiated edits.
     *
     * When persist=false, only in-memory state is updated and EVENT_SLOT_CHANGED
     * is emitted for UI refresh. This MUST be used when updating slots from
     * external data sources (e.g., Spoolman weight polling) to prevent a feedback
     * loop: set_slot_info(persist=true) → G-code → firmware status update →
     * sync_from_backend → refresh_spoolman_weights → set_slot_info again → ∞.
     * On AFC with 4 lanes this loop fires 16+ G-code commands per cycle and
     * saturates the CPU.
     *
     * @param slot_index Slot to update (0-based)
     * @param info New slot information (only filament fields used)
     * @param persist If true, persist changes to firmware. If false, update
     *               in-memory state only (for external data sync).
     * @return AmsError indicating if update succeeded
     */
    virtual AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) = 0;

    /**
     * @brief Set tool-to-slot mapping
     *
     * Configures which slot a tool number maps to.
     * Happy Hare specific - may not be supported on all backends.
     *
     * @param tool_number Tool number (0-based)
     * @param slot_index Slot to map to (0-based)
     * @return AmsError indicating if mapping was set
     */
    virtual AmsError set_tool_mapping(int tool_number, int slot_index) = 0;

    /**
     * @brief Erase the user-provided override for a slot.
     *
     * Removes the FilamentSlotOverride for @p slot_index from both the
     * in-memory map and the persisted FilamentSlotOverrideStore, then refreshes
     * override-exclusive fields on the live SlotInfo so the cleared state is
     * visible via get_slot_info() on the very next read.
     *
     * Default implementation is a no-op. Backends without FilamentSlotOverride
     * integration (AFC, Happy Hare, Tool Changer, Mock) ignore the call.
     *
     * Safe to call from the UI thread. Backends lock their own mutex_ for the
     * in-memory mutation and submit the store clear asynchronously.
     *
     * @param slot_index Slot to clear (0-based, global)
     */
    virtual void clear_slot_override(int slot_index) {
        (void)slot_index;
    }

    // ========================================================================
    // Bypass Mode Operations
    // ========================================================================

    /**
     * @brief Enable bypass mode
     *
     * Activates bypass mode where an external spool feeds directly to the
     * toolhead, bypassing the MMU/hub system. Sets current_slot to -2.
     *
     * Not all backends support bypass mode - check supports_bypass flag.
     *
     * @return AmsError indicating if bypass was enabled
     */
    virtual AmsError enable_bypass() = 0;

    /**
     * @brief Disable bypass mode
     *
     * Deactivates bypass mode. Filament should be unloaded from toolhead first.
     *
     * @return AmsError indicating if bypass was disabled
     */
    virtual AmsError disable_bypass() = 0;

    /**
     * @brief Check if bypass mode is currently active
     * @return true if bypass is active (current_slot == -2)
     */
    [[nodiscard]] virtual bool is_bypass_active() const = 0;

    // ========================================================================
    // Dryer Control (Optional - default implementations return "not supported")
    // ========================================================================

    /**
     * @brief Get dryer state and capabilities
     *
     * Returns current dryer state including temperature, duration, and
     * hardware capabilities. Not all AMS systems have dryers - check
     * DryerInfo::supported before showing dryer UI.
     *
     * @return DryerInfo struct (supported=false if no dryer)
     */
    [[nodiscard]] virtual DryerInfo get_dryer_info() const {
        return DryerInfo{.supported = false};
    }

    /**
     * @brief Start drying operation
     *
     * Initiates filament drying at specified temperature and duration.
     * Not all AMS systems support drying - check get_dryer_info().supported.
     *
     * @param temp_c Target temperature in Celsius (within min_temp_c..max_temp_c)
     * @param duration_min Drying duration in minutes (positive, capped at max_duration_min)
     * @param fan_pct Fan speed percentage (0-100, -1 = use backend default)
     * @return AmsError with SUCCESS result on success, or error with reason
     */
    virtual AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) {
        (void)temp_c;
        (void)duration_min;
        (void)fan_pct;
        (void)unit;
        return AmsErrorHelper::not_supported("Dryer");
    }

    /**
     * @brief Stop drying operation
     *
     * Stops any active drying and turns off heater/fan.
     *
     * @return AmsError with SUCCESS result on success, or error with reason
     */
    virtual AmsError stop_drying(int unit = 0) {
        (void)unit;
        return AmsErrorHelper::not_supported("Dryer");
    }

    /**
     * @brief Update drying parameters while running
     *
     * Adjusts temperature, duration, or fan speed during an active dry cycle.
     * Pass -1 to keep current value for any parameter.
     *
     * @param temp_c New target temperature (-1 = no change)
     * @param duration_min New duration (-1 = no change)
     * @param fan_pct New fan speed (-1 = no change)
     * @return AmsError with SUCCESS result on success, or error with reason
     */
    virtual AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1) {
        (void)temp_c;
        (void)duration_min;
        (void)fan_pct;
        return AmsErrorHelper::not_supported("Dryer");
    }

    /**
     * @brief Get available drying presets
     *
     * Returns preset profiles for common filament materials.
     * Backends can override to provide hardware-specific presets.
     * Falls back to get_default_drying_presets() if not overridden.
     *
     * @return Vector of DryingPreset structs
     */
    [[nodiscard]] virtual std::vector<DryingPreset> get_drying_presets() const {
        return get_default_drying_presets();
    }

    // ========================================================================
    // Endless Spool Control
    // ========================================================================

    /**
     * @brief Get endless spool capabilities for this backend
     *
     * Returns information about whether endless spool is supported and
     * whether the configuration can be modified via the UI.
     *
     * @return Capabilities struct with supported/editable flags
     */
    [[nodiscard]] virtual helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const {
        return {false, false, ""}; // Default: not supported
    }

    /**
     * @brief Get endless spool configuration for all slots
     *
     * Returns the backup slot configuration for each slot in the system.
     * For Happy Hare, this translates the group-based configuration to
     * per-slot backup mappings.
     *
     * @return Vector of configs, one per slot
     */
    [[nodiscard]] virtual std::vector<helix::printer::EndlessSpoolConfig>
    get_endless_spool_config() const {
        return {}; // Default: empty
    }

    /**
     * @brief Set backup slot for endless spool
     *
     * Configures which slot will be used as a backup when the specified
     * slot runs out of filament. Pass -1 as backup_slot to disable backup.
     *
     * Not all backends support editing:
     * - AFC: Fully editable via SET_RUNOUT G-code
     * - Happy Hare: Read-only (configured via mmu_vars.cfg)
     *
     * @param slot_index Source slot
     * @param backup_slot Backup slot (-1 to disable)
     * @return AmsError with result
     */
    virtual AmsError set_endless_spool_backup(int slot_index, int backup_slot) {
        (void)slot_index;
        (void)backup_slot;
        return AmsErrorHelper::not_supported("Endless spool");
    }

    /**
     * @brief Reset all tool mappings to defaults
     *
     * Resets tool-to-slot mappings to their original/default configuration.
     * Default behavior is typically 1:1 mapping (T0→Slot0, T1→Slot1, etc.).
     *
     * @return AmsError with result
     */
    virtual AmsError reset_tool_mappings() {
        return AmsErrorHelper::not_supported("Reset tool mappings");
    }

    /**
     * @brief Reset all endless spool backup mappings
     *
     * Clears all endless spool backup slot configurations, setting each
     * slot's backup to -1 (no backup).
     *
     * @return AmsError with result
     */
    virtual AmsError reset_endless_spool() {
        return AmsErrorHelper::not_supported("Reset endless spool");
    }

    // ========================================================================
    // Tool Mapping Control
    // ========================================================================

    /**
     * @brief Get tool mapping capabilities for this backend
     *
     * Returns information about whether tool mapping is supported and
     * whether the configuration can be modified via the UI.
     *
     * @return Capabilities struct with supported/editable flags
     */
    [[nodiscard]] virtual helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const {
        return {false, false, ""}; // Default: not supported
    }

    /**
     * @brief Get current tool-to-slot mapping
     *
     * Returns the mapping from tool number to slot index.
     * The vector index represents the tool number, and the value at that
     * index is the slot that tool maps to.
     *
     * @return Vector where index=tool, value=slot (empty if not supported)
     */
    [[nodiscard]] virtual std::vector<int> get_tool_mapping() const {
        return {}; // Default: empty
    }

    // ========================================================================
    // Device-Specific Actions
    // ========================================================================

    /**
     * @brief Get available device sections for this backend
     *
     * Sections group related actions (e.g., "Calibration", "Speed Settings").
     * UI renders sections in display_order.
     *
     * @return Vector of DeviceSection (empty if no device-specific features)
     */
    [[nodiscard]] virtual std::vector<helix::printer::DeviceSection> get_device_sections() const {
        return {};
    }

    /**
     * @brief Get available device actions
     *
     * Returns all device-specific actions. UI groups them by section ID.
     *
     * @return Vector of DeviceAction (empty if no device-specific features)
     */
    [[nodiscard]] virtual std::vector<helix::printer::DeviceAction> get_device_actions() const {
        return {};
    }

    /**
     * @brief Execute a device action
     *
     * @param action_id The action ID from get_device_actions()
     * @param value Optional value for toggles/sliders/dropdowns
     * @return AmsError indicating success/failure
     */
    virtual AmsError execute_device_action(const std::string& action_id,
                                           const std::any& value = {}) {
        (void)action_id;
        (void)value;
        return AmsErrorHelper::not_supported("Device actions");
    }

    // ========================================================================
    // Capability Queries
    // ========================================================================

    /**
     * @brief Check if backend automatically heats extruder before loading
     *
     * Some backends (like AFC) use material-specific temperatures from their
     * configuration (e.g., default_material_temps in AFC.cfg) to preheat the
     * extruder before loading filament. This eliminates the need for the UI
     * to manage preheating.
     *
     * @return true if backend handles preheat automatically, false if UI should manage it
     */
    [[nodiscard]] virtual bool supports_auto_heat_on_load() const {
        return false;
    }

    /**
     * @brief Whether this backend persists spool assignments via firmware
     *
     * Backends like Happy Hare (MMU_GATE_MAP/SPOOLID) and AFC (SET_SPOOL_ID)
     * store spool-to-slot mappings in firmware gcode. For these, ToolState
     * mirrors firmware state but doesn't need to persist separately.
     *
     * Backends without firmware persistence (tool changers, ACE) rely on
     * ToolState's own persistence (Moonraker DB + local JSON).
     *
     * @return true if firmware handles spool persistence, false if ToolState should
     */
    [[nodiscard]] virtual bool has_firmware_spool_persistence() const {
        return false;
    }

    /**
     * @brief Whether this backend unloads the toolhead automatically after a print
     *
     * Some filament systems (e.g. AD5X IFS) retract filament out of the extruder
     * at end-of-print by default, so an empty toolhead between jobs is the normal
     * resting state. For these backends, a runout-sensor reading "no filament" at
     * print-start is expected, not a warning condition, and the pre-print
     * filament-missing modal should be suppressed.
     *
     * @return true if the backend is expected to leave the toolhead empty between
     *         prints, false otherwise
     */
    [[nodiscard]] virtual bool auto_unloads_after_print() const {
        return false;
    }

    /**
     * @brief Check if backend provides per-unit environment sensors (temp/humidity)
     *
     * CFS units have built-in temperature and humidity sensors. Other backends
     * (Happy Hare, ACE, AFC, Tool Changers) do not.
     *
     * @return true if backend provides environment sensor data per unit
     */
    [[nodiscard]] virtual bool has_environment_sensors() const {
        return false;
    }

    /**
     * @brief Get the list of material strings this backend's firmware will accept.
     *
     * Returns std::nullopt if the backend accepts any material string
     * (default for AFC, Happy Hare, ACE, CFS — firmware treats material as
     * a free-form label).
     *
     * Returns a non-empty vector when firmware validates the string against
     * a fixed list (e.g., AD5X IFS rejects anything outside its 7-item set).
     * Callers should filter dropdowns and normalize outgoing values to this
     * list. An empty vector is treated the same as nullopt (no restriction).
     */
    [[nodiscard]] virtual std::optional<std::vector<std::string>> get_supported_materials() const {
        return std::nullopt;
    }

    /**
     * @brief Firmware-specific name aliases layered between exact match and
     *        compat_group fallback inside normalize_material().
     *
     * Each entry maps an alternate spelling (case-insensitive) to a whitelist
     * entry. Use when the shared filament database groups variants differently
     * than the firmware does. Example: AD5X IFS treats SILK as distinct from
     * PLA, but "Silk PLA" has compat_group "PLA" in the shared DB, so without
     * an alias it would normalize to "PLA" instead of "SILK".
     *
     * Returned pairs: {alias, whitelist_target}. Targets should themselves be
     * present in get_supported_materials(); if not, normalize_material() will
     * still return them (caller beware).
     *
     * @return Vector of (alias -> target) pairs. Empty by default.
     */
    [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>>
    get_material_aliases() const {
        return {};
    }

    /**
     * @brief Normalize an arbitrary material string to one the firmware will accept.
     *
     * Called before sending material to firmware. Pipeline:
     *   1. If get_supported_materials() returns nullopt/empty -> return input unchanged.
     *   2. Case-insensitive exact match against whitelist -> return whitelist entry.
     *   3. Case-insensitive match against get_material_aliases() -> return mapped target.
     *   4. Look up input via filament::find_material() -> get compat_group -> find
     *      first whitelist entry whose compat_group matches -> return it.
     *   5. Fallback: return first whitelist entry (safest default, usually PLA).
     *
     * Backends that need more than alias mapping can override this method.
     *
     * @param material Input material string (may be empty)
     * @return Normalized string safe to send to firmware
     */
    [[nodiscard]] virtual std::string normalize_material(const std::string& material) const;

    // ========================================================================
    // Discovery Configuration (Optional - default implementations are no-ops)
    // ========================================================================

    /**
     * @brief Set discovered lane and hub names from PrinterCapabilities
     *
     * Called before start() to provide lane names discovered from printer.objects.list.
     * Only AFC backend uses this - other backends ignore it.
     *
     * @param lane_names Lane names from PrinterCapabilities::get_afc_lane_names()
     * @param hub_names Hub names from PrinterCapabilities::get_afc_hub_names()
     */
    virtual void set_discovered_lanes(const std::vector<std::string>& lane_names,
                                      const std::vector<std::string>& hub_names) {
        (void)lane_names;
        (void)hub_names;
    }

    /**
     * @brief Set discovered tool names from PrinterCapabilities
     *
     * Called before start() to provide tool names discovered from printer.objects.list.
     * Only tool changer backend uses this - other backends ignore it.
     *
     * @param tool_names Tool names from PrinterCapabilities::get_tool_names()
     */
    virtual void set_discovered_tools(std::vector<std::string> tool_names) {
        (void)tool_names;
    }

    /**
     * @brief Set filament sensor names from PrinterCapabilities
     *
     * Called before start() to provide filament sensor names from printer.objects.list.
     * AFC backend uses this to detect hardware vs virtual bypass sensor.
     *
     * @param sensor_names Sensor names (e.g., "filament_switch_sensor virtual_bypass")
     */
    virtual void set_discovered_sensors(const std::vector<std::string>& sensor_names) {
        (void)sensor_names;
    }

    // ========================================================================
    // Mock Support
    // ========================================================================

    /**
     * @brief Set callback for gcode response injection
     *
     * Used by mock backends to simulate Klipper gcode responses.
     * Default implementation is a no-op for real backends.
     *
     * @param callback Function that receives gcode response lines
     */
    virtual void set_gcode_response_callback(std::function<void(const std::string&)>) {}

    // ========================================================================
    // Factory Method
    // ========================================================================

    /**
     * @brief Create appropriate backend for detected AMS type (mock only)
     *
     * Factory method that creates a mock backend for testing.
     * For real backends, use the overload that accepts MoonrakerAPI and MoonrakerClient.
     *
     * In mock mode (RuntimeConfig::should_mock_ams()), returns AmsBackendMock.
     *
     * @param detected_type The detected AMS type from printer discovery
     * @return Unique pointer to backend instance, or nullptr if type is NONE
     * @deprecated Use create(AmsType, MoonrakerAPI*, helix::MoonrakerClient*) for real backends
     */
    static std::unique_ptr<AmsBackend> create(AmsType detected_type);

    /**
     * @brief Create appropriate backend for detected AMS type with dependencies
     *
     * Factory method that creates the correct backend implementation:
     * - HAPPY_HARE: AmsBackendHappyHare (requires api and client)
     * - AFC: AmsBackendAfc (requires api and client)
     * - NONE: nullptr (no AMS detected)
     *
     * In mock mode (RuntimeConfig::should_mock_ams()), returns AmsBackendMock.
     *
     * @param detected_type The detected AMS type from printer discovery
     * @param api Pointer to MoonrakerAPI for sending commands
     * @param client Pointer to helix::MoonrakerClient for subscriptions
     * @return Unique pointer to backend instance, or nullptr if type is NONE
     */
    static std::unique_ptr<AmsBackend> create(AmsType detected_type, MoonrakerAPI* api,
                                              helix::MoonrakerClient* client);

    /**
     * @brief Create mock backend for testing
     *
     * Creates a mock backend regardless of actual printer state.
     * Used when --test flag is passed or for development.
     *
     * @param slot_count Number of simulated slots (default 4)
     * @return Unique pointer to mock backend instance
     */
    static std::unique_ptr<AmsBackend> create_mock(int slot_count = 4);
};

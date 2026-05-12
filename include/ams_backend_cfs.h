// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_CFS

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CfsTestAccess;

namespace helix::printer {

/// Material info from CFS RFID database
struct CfsMaterialInfo {
    std::string id;
    std::string brand;
    std::string name;
    std::string material_type;
    int min_temp = 0;
    int max_temp = 0;
};

/// Static material database + CFS utility functions
class CfsMaterialDb {
  public:
    static const CfsMaterialDb& instance();

    /// Lookup material by 5-digit ID (e.g., "01001")
    const CfsMaterialInfo* lookup(const std::string& id) const;

    /// Strip CFS material_type code prefix: "101001" -> "01001", "-1" -> ""
    static std::string strip_code(const std::string& code);

    /// Parse CFS color: "0RRGGBB" -> 0xRRGGBB, sentinels -> 0x808080
    static uint32_t parse_color(const std::string& color_str);

    /// Global slot index -> TNN name: 0 -> "T1A", 4 -> "T2A"
    static std::string slot_to_tnn(int global_index);

    /// TNN name -> global slot index: "T1A" -> 0, "T2A" -> 4, invalid -> -1
    static int tnn_to_slot(const std::string& tnn);

    /// Default color for unknown/sentinel slots
    static constexpr uint32_t DEFAULT_COLOR = 0x808080;

  private:
    CfsMaterialDb();
    void load_database();
    std::unordered_map<std::string, CfsMaterialInfo> materials_;
};

/// Decode CFS key8xx error codes into human-readable AmsAlerts
class CfsErrorDecoder {
  public:
    /// Decode a CFS error code. Returns nullopt for unknown codes.
    static std::optional<AmsAlert> decode(const std::string& key_code,
                                          int unit_index, int slot_index);

    /// Look up just the message+hint for a code, without slot/unit context.
    /// Used by the global gcode-error toast handler to translate raw Klipper
    /// `!! {"code":"key***","msg":"..."}` lines into friendly text.
    /// Returns {message, hint} or nullopt for unknown codes.
    static std::optional<std::pair<const char*, const char*>>
        lookup_message(const std::string& key_code);

    /// Variant that splices the `values` array (e.g. `[1,"B"]` from
    /// `!! {"code":"key849","values":[1,"B"]}`) into the user-facing
    /// message when the code's value-format is known. Returns full
    /// `std::string` so the caller doesn't have to mix const-char + string
    /// concatenation. Falls back to the un-augmented message+hint when
    /// the values shape is unknown for that code.
    static std::optional<std::pair<std::string, std::string>>
        lookup_message_with_values(const std::string& key_code,
                                    const nlohmann::json& values);
};

/// CFS (Creality Filament System) backend — K2 series printers with RS-485 CFS units
class AmsBackendCfs : public AmsSubscriptionBackend {
  public:
    AmsBackendCfs(MoonrakerAPI* api, helix::MoonrakerClient* client);

    [[nodiscard]] AmsType get_type() const override { return AmsType::CFS; }

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization
    [[nodiscard]] PathTopology get_topology() const override { return PathTopology::HUB; }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // Operations
    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;
    AmsError reset() override;
    AmsError recover() override;
    AmsError cancel() override;

    // Slot management (user overrides persisted via shared FilamentSlotOverrideStore)
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and fires
    // override_store_->clear_async. CFS firmware populates brand / color_name /
    // total_weight_g from its RFID material database, so those fields are
    // preserved. Only spool_name / spoolman_* / remaining_weight_g are zeroed.
    // The hardware-event detector calls this internally once an RFID
    // fingerprint change confirms a physical swap.
    void clear_slot_override(int slot_index) override;

    // Bypass (not supported)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override { return false; }

    // Capabilities
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;
    [[nodiscard]] bool supports_auto_heat_on_load() const override { return true; }
    [[nodiscard]] bool has_environment_sensors() const override { return true; }
    [[nodiscard]] bool tracks_weight_locally() const override { return false; }
    [[nodiscard]] bool manages_active_spool() const override { return false; }
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

    // Static parser (public for testing)
    static AmsSystemInfo parse_box_status(const nlohmann::json& box_json);

    // GCode helpers (public for testing)
    static std::string load_gcode(int global_slot_index);
    static std::string unload_gcode();
    static std::string swap_gcode(int global_slot_index);
    static std::string reset_gcode();
    static std::string recover_gcode();

  protected:
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[AMS CFS]"; }
    void on_started() override;

    /// Push the user's chosen color back to firmware via the undocumented
    /// `BOX_MODIFY_TN_DATA` gcode (registered by box_wrapper.cpython-39.so,
    /// not in `gcode/help`). Format reverse-engineered from K2's master-server
    /// binary: `BOX_MODIFY_TN_DATA ADDR=<1..4> NUM=<A|B|C|D> PART=color_value
    /// DATA=0RRGGBB`. Writes persist to `/mnt/UDISK/creality/userdata/box/tn_data.json`.
    ///
    /// Color-only for now — material_type uses CFS-internal codes (e.g. "101001"
    /// for PLA) that we lack a complete reverse-map for; round-tripping the
    /// material risks pushing a wrong/empty code and could confuse the K2's
    /// stock LCD or the LOAD_MATERIAL macros. Color is the primary user-edit
    /// anyway and round-trips cleanly because the format is just RGB hex.
    ///
    /// **CRITICAL:** sending invalid args (ADDR=0, malformed payload) triggers
    /// a `TypeError` deep in box_wrapper which Klipper escalates to
    /// `invoke_shutdown` — the entire printer goes offline and needs a full
    /// `RESTART`. This method validates `global_index` in [0, 16) and skips
    /// when `color_rgb == 0` BEFORE formatting the gcode. Non-fatal on dispatch
    /// failure (the override is in lane_data either way).
    ///
    /// Marked virtual + protected so test subclasses can override and capture
    /// the gcode without a live Moonraker connection.
    virtual void push_slot_color_to_firmware(int global_index, uint32_t color_rgb);

  private:
    friend class ::CfsTestAccess;

    std::string current_tnn_;
    bool motor_ready_ = true;

    // Callback lifetime management
    helix::AsyncLifetimeGuard lifetime_;

    /// Dispatch a load/unload/swap CR_BOX_* script with proper completion
    /// semantics: ensures the toolhead is homed, sends the gcode, and flips
    /// `system_info_.action` back to IDLE *only when Klipper finishes the
    /// entire script* (success or error). The previous design relied on the
    /// `filament_switch_sensor` flipping to declare "done" — but that sensor
    /// triggers at the toolhead extruder, which is reached at the *end of
    /// CR_BOX_EXTRUDE* (step 2 of 5). The remaining `CR_BOX_WASTE` and
    /// `CR_BOX_FLUSH` (~3 min of nozzle-at-240 °C extrusion) ran while the
    /// UI told the user the load was idle.
    AmsError dispatch_action_script(std::string gcode);

    /// Layer a configured FilamentSlotOverride for `slot_index` over `slot`,
    /// mutating `slot` in place. Override wins for every non-default field;
    /// default sentinels (empty strings, spoolman_id 0, weights -1, color_rgb 0)
    /// fall through to firmware-reported data. Callers must hold mutex_.
    /// Called from handle_status_update AFTER firmware parse populates the slot
    /// and AFTER check_hardware_event_clear, so the final SlotInfo visible via
    /// get_slot_info reflects the override layer.
    void apply_overrides(SlotInfo& slot, int slot_index);

    /// Hardware-event detection: CFS exposes per-slot RFID material data. The
    /// composite (material_type + color_value) raw RFID strings form a
    /// per-slot fingerprint. When the fingerprint changes between parses, the
    /// physical spool was swapped — clear the stored override so stale
    /// spool_name / spoolman_id / remaining_weight_g from the previous user
    /// don't bleed onto the new spool.
    ///
    /// Empty observed_uid (no tag / sentinel `-1` / `None`) is treated as
    /// "no signal" — never updates the baseline and never clears. First
    /// observation for a slot establishes the baseline and NEVER fires a
    /// clear. Must be called BEFORE apply_overrides so the clear's field
    /// reset isn't masked by a stale override layer.
    ///
    /// CFS-specific field policy on clear: CFS firmware populates
    /// brand/color_name/total_weight_g from its material database via RFID
    /// lookup, so those fields are NOT zeroed here — the parse has already
    /// written firmware-truth for the newly-inserted spool. Only strictly
    /// override-exclusive fields (spool_name / spoolman_id /
    /// spoolman_vendor_id / remaining_weight_g) are reset.
    void check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                    const std::string& observed_uid);

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets strictly override-exclusive fields on
    // the provided SlotInfo (spool_name, spoolman_*, remaining_weight_g), and
    // fires clear_async. Brand / color_name / total_weight_g are preserved —
    // firmware populates them from the RFID material database.
    void clear_override_locked(int slot_index, SlotInfo& slot);

    // Persistent per-slot overrides. Writers (on_started bulk load,
    // set_slot_info persist path, check_hardware_event_clear) all hold
    // mutex_. Reads happen inside apply_overrides, which is also called
    // under mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Per-slot last-observed RFID fingerprint (material_type + "|" +
    // color_value, using the raw pre-strip_code strings). Empty = first
    // observation not yet made (or only sentinel / no-tag values seen so
    // far). All access under mutex_.
    std::unordered_map<int, std::string> last_rfid_uid_;

    // Sub-phase synthesis: CFS sets system_info_.action=LOADING/UNLOADING once
    // at gcode dispatch and leaves it there through cut/retract/feed/purge.
    // The step indicator therefore parks on the wrong sub-step (#Task #2).
    // We synthesize CUTTING / UNLOADING / LOADING / PURGING transitions from
    // physical signals — filament-sensor edges and extruder-target rises —
    // and overwrite system_info_.action so the UI's existing step mapping
    // shows the correct phase. All access under mutex_.
    struct PhaseTracker {
        bool active = false;                  // true between dispatch and on_complete/on_error
        bool started_with_filament = false;   // filament_detected at op start
        bool seen_filament_drop = false;      // true→false transition (cut completed)
        bool seen_filament_rise = false;      // false→true transition after a drop (new filament fed)
        bool reached_target_once = false;     // current_temp ever within 5°C of target this op
        bool pending_purge_target = false;    // target rose >10°C above baseline (waits for rise)
        bool seen_purge_signal = false;       // pending_purge_target gated by seen_filament_rise
        int  baseline_target_centi = 0;       // extruder target when heating first completed
    };
    PhaseTracker phase_tracker_;
    int last_extruder_target_centi_ = 0;
    int last_extruder_temp_centi_ = 0;
    bool last_filament_detected_ = false;

    // Capture op-start state (filament + extruder target). Sets phase_tracker_.active.
    // Caller must hold mutex_.
    void begin_phase_tracking();

    // Reset phase tracker on op completion. Caller must hold mutex_.
    void end_phase_tracking();

    // Drive phase machine on signal changes. Caller must hold mutex_.
    void on_filament_transition_locked(bool new_detected);
    void on_extruder_temp_change_locked(int new_temp_centi, int new_target_centi);

    // Recompute system_info_.action from phase_tracker_ state.
    // Caller must hold mutex_.
    void apply_synthesized_action_locked();
};

} // namespace helix::printer

#endif // HELIX_HAS_CFS

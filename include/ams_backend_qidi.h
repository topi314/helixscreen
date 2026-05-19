// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"

#include <string>
#include <vector>

/// AMS backend stub for the QIDI Box filament changer.
///
/// The QIDI Box is a 4-slot hub-style filament changer (chainable up to 16
/// slots across 4 boxes) sold by QIDI for the PLUS4, Q2, and MAX4 printers.
/// It is NOT supported on Q1 Pro or X-Max 3 — those use a different hub board.
///
/// Architecturally it is closest to the FlashForge AD5X IFS or Bambu AMS
/// (converging hub), NOT a lane-selector MMU. RFID MIFARE Classic tags carry
/// per-spool metadata and the unit has active filament drying. USB-serial
/// connection with udev rule `QIDI_BOX_V1`.
///
/// === Current status: STUB ===
///
/// No protocol work has been done yet — no live hardware is available to
/// validate against. This class registers AmsType::QIDI_BOX and provides
/// empty/no-op implementations for every AmsBackend virtual method so the
/// factory can return a non-null backend and the rest of HelixScreen can
/// treat QIDI Box as "detected but uninitialised" without crashing.
///
/// === Protocol reference (when hardware arrives) ===
///
/// The stock QIDI firmware drives the box through Klipper gcode macros
/// (BOX_CHANGE_FILAMENT, _BOX_START, _BOX_*) plus printer-object polling
/// and save_variables. There are no dedicated Moonraker endpoints.
///
/// The qidi-community Plus4-Wiki ships a "customisable_qidibox_firmware"
/// project with Python replacements for QIDI's obfuscated .so modules —
/// that is the best starting point for reverse-engineering the actual
/// command set when this stub graduates to a real backend:
///
///   https://github.com/qidi-community/Plus4-Wiki
///
/// Until then: every operation logs a "not yet implemented" warning and
/// returns a sensible no-op / NOT_SUPPORTED error.
class AmsBackendQidi : public AmsSubscriptionBackend {
  public:
    AmsBackendQidi(MoonrakerAPI* api, helix::MoonrakerClient* client);
    ~AmsBackendQidi() override;

    /// Single-box default. Chainable up to 16 slots (4 boxes × 4 slots) on
    /// supported hardware, but the stub only advertises a single box.
    static constexpr int NUM_SLOTS = 4;

    // --- AmsBackend interface ---
    [[nodiscard]] AmsType get_type() const override {
        return AmsType::QIDI_BOX;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] bool is_bypass_active() const override;

    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;
    void clear_slot_override(int slot_index) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS QIDI Box]";
    }

  private:
    friend class QidiBoxTestAccess;

    /// Apply a save_variables snapshot onto system_info_ / per-slot state.
    /// Input is the inner `variables` object (already unwrapped from the
    /// `save_variables.variables` envelope).
    void parse_save_variables(const nlohmann::json& variables);

    /// Scan notification for `heater_generic heater_box<N>` and
    /// `aht20_f heater_box<N>` entries; update unit environment with the
    /// max temperature and max humidity observed across all boxes.
    void apply_heater_status(const nlohmann::json& notification);

    /// Raw RFID indices read from save_variables. Per-slot side-table so we
    /// don't pollute SlotInfo with backend-specific fields. Resolution to
    /// material/color/brand happens via the officiall_filas_list.cfg lookup
    /// (separate cycle). 0 = unknown.
    struct QidiSlotRfid {
        int filament_id = 0; ///< 1-99, index into officiall_filas_list.cfg
        int color_id = 0;    ///< 1-24, palette index
        int vendor_id = 0;   ///< Always 1 in the wild so far
    };
    std::vector<QidiSlotRfid> slot_rfid_;
};

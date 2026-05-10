// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

#include "filament_slot_override.h"

class IMoonrakerAPI;
class FilamentSlotOverrideStoreTestAccess;

namespace helix::ams {

class FilamentSlotOverrideStore {
  public:
    FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id);

    // Blocking load from Moonraker database (called only at backend init time).
    // Later tasks will add local-cache fallback.
    std::unordered_map<int, FilamentSlotOverride> load_blocking();

    using SaveCallback = std::function<void(bool success, std::string error)>;
    void save_async(int slot_index, const FilamentSlotOverride& override, SaveCallback cb);
    void clear_async(int slot_index, SaveCallback cb);

    const std::string& backend_id() const {
        return backend_id_;
    }

  private:
    // Test-only access to mutate load_timeout_ without exposing a public
    // setter. Per L065, prefer friend-class over test-only public methods.
    friend class ::FilamentSlotOverrideStoreTestAccess;

    IMoonrakerAPI* api_;
    std::string backend_id_;
    // Adopts the AFC/OrcaSlicer lane_data Moonraker convention. Each slot is
    // stored under key "laneN" where N is the 1-based slot index (lane1, lane2,
    // ...). Slot index 0 in HelixScreen maps to "lane1" on disk.
    std::string namespace_ = "lane_data";
    // Local timeout for load_blocking()'s cv.wait_for. Defaults to 5 seconds;
    // overridable by FilamentSlotOverrideStoreTestAccess for timeout tests.
    // Stored as milliseconds (not seconds) because tests need sub-second
    // resolution — a chrono::seconds member would truncate 50ms to 0s.
    std::chrono::milliseconds load_timeout_{5000};
    // On-disk read-cache directory. Empty = use helix::get_user_config_dir().
    // Overridable by FilamentSlotOverrideStoreTestAccess so tests write to a
    // per-PID tmp dir instead of polluting the user's config. The cache is
    // NEVER authoritative — the Moonraker DB on the printer is the source of
    // truth. The cache exists only so the UI can show last-known metadata
    // when Moonraker is unreachable at backend init.
    std::filesystem::path cache_dir_;
    // Absolute path to the cache JSON file. Computed from cache_dir_ (or
    // get_user_config_dir() if empty). One file serves all backends; each
    // backend's slots live under doc[backend_id]["slots"].
    std::filesystem::path cache_path() const;
    // Absolute path to the directory used for on-disk caches. Same resolution
    // as cache_path(): cache_dir_ if set, otherwise get_user_config_dir().
    // Migration uses this to locate legacy "{backend_id}_slot_overrides.json"
    // files that pre-date the unified filament_slot_overrides.json format.
    std::filesystem::path cache_dir_effective() const;
    // Returns the Moonraker DB key for a given slot.
    //
    // IMPORTANT: the DB key is 1-based (AFC convention: lane1, lane2, ...) but
    // the "lane" field *inside* each record is 0-based (matches Orca's
    // tool-index interpretation, written by to_lane_data_record in the .cpp).
    // Easy to get wrong — changing one without the other silently breaks
    // interop with AFC and Orca.
    static std::string lane_key(int slot_index) {
        return "lane" + std::to_string(slot_index + 1);
    }
};

// =============================================================================
// Shared firmware -> lane_data mirror helper
// =============================================================================
//
// AFC and Happy Hare publish lane_data themselves (their Klipper plugins write
// directly to the Moonraker DB), so OrcaSlicer's MoonrakerPrinterAgent can
// read filament state without HelixScreen's involvement.
//
// CFS, AD5X IFS, and Snapmaker firmware do NOT publish lane_data. HelixScreen
// has to mirror firmware-detected color/material into the lane_data namespace
// so OrcaSlicer's "Sync filaments from Printer" works. This helper centralizes
// that mirror so the three backends share one implementation.
//
// Why a policy enum: backends differ in whether user UI edits propagate back
// to firmware:
//
//   - IFS: set_slot_info writes to Adventurer5M.json — firmware re-reads it
//     and reports the user's chosen color on the next status poll. The mirror
//     can safely overwrite the override unconditionally because firmware-truth
//     and user-truth converge.
//
//   - CFS / Snapmaker: set_slot_info does NOT touch the firmware-side
//     material_type / RFID values. If the mirror unconditionally overwrote
//     ovr.color_rgb with firmware-truth, every status poll would erase the
//     user's color override. So these backends use FillUnsetOnly: only fill
//     fields the user hasn't explicitly set. clear_slot_override resets the
//     entry, after which auto-mirror takes over again.
enum class MirrorPolicy {
    /// Overwrite ovr.color_rgb / ovr.material with firmware values
    /// unconditionally. Use when user edits propagate back to firmware so the
    /// two views stay in sync (AD5X IFS).
    OverwriteAlways,
    /// Only fill ovr.color_rgb / ovr.material when they're currently UNSET
    /// (color_rgb == 0, empty material). Use when user edits don't reach
    /// firmware (CFS, Snapmaker).
    FillUnsetOnly,
};

/// Mirror firmware-detected color/material into `overrides[slot_index]` and
/// fire `store->save_async` to push the resulting record to the lane_data
/// namespace. Caller MUST hold the backend's mutex protecting `overrides`.
///
/// No-op (returns false without writing) when:
///   - !slot_has_filament  (empty / unread slot — no signal)
///   - the chosen policy leaves nothing to change (e.g. FillUnsetOnly when
///     ovr already has both fields set, or OverwriteAlways when ovr already
///     matches firmware)
///
/// Note: `firmware_color == 0` is NOT treated as "no signal" — pure black is
/// a legitimate color the user can load. Backends whose parse path may run
/// before colors are populated (e.g. AD5X IFS) must apply their own
/// color-zero guard upstream of this helper.
///
/// `store` may be null (init-time race / test fixture without MR API) — the
/// in-memory override is still updated, but no save_async is fired.
///
/// `log_tag` is included in the warn log on save failure so multi-backend
/// logs stay attributable.
///
/// Returns true iff `overrides[slot_index]` was actually mutated. Callers
/// (e.g. IFS) use this to drive secondary side-effects like _IFS_VARS sync.
bool mirror_firmware_to_lane_data(FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides,
                                  int slot_index, uint32_t firmware_color,
                                  const std::string& firmware_material, bool slot_has_filament,
                                  MirrorPolicy policy, const std::string& log_tag);

} // namespace helix::ams

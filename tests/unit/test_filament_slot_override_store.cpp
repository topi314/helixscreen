// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "hv/json.hpp"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

using helix::ams::FilamentSlotOverride;
using helix::ams::FilamentSlotOverrideStore;
using nlohmann::json;

// Grants tests access to private tunables on FilamentSlotOverrideStore.
// Declared friend in the header (per L065: prefer friend-class over test-only
// public setters on production classes).
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_load_timeout(helix::ams::FilamentSlotOverrideStore& store,
                                 std::chrono::milliseconds ms) {
        store.load_timeout_ = ms;
    }
    // Redirect the read-cache to a per-test tmp dir so tests never touch the
    // user's real config. Empty path restores the default (get_user_config_dir).
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

namespace {
// Per-test isolation helper: returns a fresh tmp directory that is unique to
// the calling test (different `suffix` per test), creates it, and cleans up
// on scope exit. Any test that triggers a successful save/clear MUST redirect
// the store's cache_dir_ to this — otherwise Task 6's cache write lands in
// the developer's real config dir and pollutes state across runs.
struct TmpCacheDir {
    std::filesystem::path path;
    explicit TmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("filament_slot_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

TEST_CASE("resolved_temps preserves explicit values, falls back to material DB",
          "[filament_slot_override]") {
    using helix::ams::FilamentSlotOverride;
    using helix::ams::resolved_temps;

    SECTION("explicit values pass through unchanged") {
        FilamentSlotOverride o;
        o.material = "PLA";  // PLA defaults differ; explicit must win
        o.bed_temp = 65;
        o.nozzle_temp = 220;
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 65);
        CHECK(r.nozzle_temp == 220);
    }

    SECTION("zero fields fall back to material DB") {
        FilamentSlotOverride o;
        o.material = "PETG";  // PETG defaults: bed 80, nozzle ~245
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 80);
        CHECK(r.nozzle_temp == 245);
    }

    SECTION("partial fields: explicit bed, fallback nozzle") {
        FilamentSlotOverride o;
        o.material = "PETG";
        o.bed_temp = 70;  // explicit
        // nozzle_temp = 0 → fallback
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 70);     // user wins
        CHECK(r.nozzle_temp == 245);  // material DB
    }

    SECTION("no material → no fallback, both stay 0") {
        FilamentSlotOverride o;
        // material empty
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 0);
        CHECK(r.nozzle_temp == 0);
    }

    SECTION("unknown material → no fallback, both stay 0") {
        FilamentSlotOverride o;
        o.material = "ZZZ_NOT_A_REAL_MATERIAL";
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 0);
        CHECK(r.nozzle_temp == 0);
    }

    SECTION("material change picks up new defaults — no stale carry-over") {
        // The point of resolving at emit time, not stamping the override at
        // set time: changing material from PLA → PETG must reflect PETG's
        // defaults on the next resolved_temps() call. Storing fallback values
        // on the struct would freeze PLA's 60°C with the new PETG material.
        FilamentSlotOverride o;
        o.material = "PLA";
        auto pla = resolved_temps(o);
        CHECK(pla.bed_temp == 60);  // PLA default

        o.material = "PETG";
        auto petg = resolved_temps(o);
        CHECK(petg.bed_temp == 80);  // PETG default — fresh, not stale PLA
    }
}

TEST_CASE("populate_temps_from_slot_info wires SlotInfo temps onto the override",
          "[filament_slot_override]") {
    using helix::ams::FilamentSlotOverride;
    using helix::ams::populate_temps_from_slot_info;

    SECTION("nozzle_temp_min < max → midpoint") {
        FilamentSlotOverride o;
        SlotInfo info;
        info.bed_temp = 60;
        info.nozzle_temp_min = 200;
        info.nozzle_temp_max = 220;
        populate_temps_from_slot_info(o, info);
        CHECK(o.bed_temp == 60);
        CHECK(o.nozzle_temp == 210);  // midpoint
    }

    SECTION("nozzle_temp_min == max → just min") {
        // Single-value materials (some firmware-tracked sources don't carry
        // a range) shouldn't fall through to the 0-leaves-fallback branch.
        FilamentSlotOverride o;
        SlotInfo info;
        info.bed_temp = 65;
        info.nozzle_temp_min = 215;
        info.nozzle_temp_max = 215;
        populate_temps_from_slot_info(o, info);
        CHECK(o.nozzle_temp == 215);
    }

    SECTION("nozzle_temp_min set, max unset → just min") {
        FilamentSlotOverride o;
        SlotInfo info;
        info.nozzle_temp_min = 215;
        // nozzle_temp_max = 0
        populate_temps_from_slot_info(o, info);
        CHECK(o.nozzle_temp == 215);
    }

    SECTION("all temps unset → 0 sentinel for both") {
        FilamentSlotOverride o;
        // Pre-existing values must be cleared so resolved_temps()'s
        // material-DB fallback can take over instead of carrying forward
        // a stale value the SlotInfo didn't bring.
        o.bed_temp = 99;
        o.nozzle_temp = 99;
        SlotInfo info;
        populate_temps_from_slot_info(o, info);
        CHECK(o.bed_temp == 0);
        CHECK(o.nozzle_temp == 0);
    }
}

TEST_CASE("FilamentSlotOverride roundtrips through JSON", "[filament_slot_override]") {
    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.spoolman_vendor_id = 7;
    ovr.remaining_weight_g = 850.0f;
    ovr.total_weight_g = 1000.0f;
    ovr.color_rgb = 0xFF5500;
    ovr.color_name = "Orange";
    ovr.material = "PLA";
    ovr.bed_temp = 60;
    ovr.nozzle_temp = 215;
    ovr.updated_at = std::chrono::system_clock::from_time_t(1713441296);

    json j = helix::ams::to_json(ovr);
    FilamentSlotOverride round = helix::ams::from_json(j);

    CHECK(round.brand == ovr.brand);
    CHECK(round.spool_name == ovr.spool_name);
    CHECK(round.spoolman_id == ovr.spoolman_id);
    CHECK(round.spoolman_vendor_id == ovr.spoolman_vendor_id);
    CHECK(round.remaining_weight_g == ovr.remaining_weight_g);
    CHECK(round.total_weight_g == ovr.total_weight_g);
    CHECK(round.color_rgb == ovr.color_rgb);
    CHECK(round.color_name == ovr.color_name);
    CHECK(round.material == ovr.material);
    CHECK(round.bed_temp == ovr.bed_temp);
    CHECK(round.nozzle_temp == ovr.nozzle_temp);
    CHECK(round.updated_at == ovr.updated_at);
}

TEST_CASE("FilamentSlotOverrideStore load returns empty when namespace absent",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    // lane_data namespace has no entries for any slot.
    FilamentSlotOverrideStore store(&api, "ifs");

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore load_blocking parses lane_data entries",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed lane_data namespace with two AFC-shaped entries + our extensions.
    // lane1 also exercises the print-temp fields parse — they round-trip via
    // from_lane_data_record (the load path) into the in-memory override.
    json lane1 = {
        {"lane", "0"},
        {"color", "#FF5500"},
        {"material", "PLA"},
        {"vendor", "Polymaker"},
        {"spool_id", 42},
        {"spool_name", "PolyLite PLA Orange"},
        {"remaining_weight_g", 850.0},
        {"bed_temp", 65},
        {"nozzle_temp", 220},
    };
    json lane2 = {
        {"lane", "1"},
        {"color", "0x00FF00"}, // 0x prefix accepted
        {"material", "PETG"},
        // No bed_temp / nozzle_temp — load must default to 0 (the "use
        // material default" sentinel that resolved_temps() consults at emit).
    };
    api.mock_set_db_value("lane_data", "lane1", lane1);
    api.mock_set_db_value("lane_data", "lane2", lane2);

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();

    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "Polymaker");
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides[0].color_rgb == 0xFF5500u);
    CHECK(overrides[0].spoolman_id == 42);
    CHECK(overrides[0].spool_name == "PolyLite PLA Orange");
    CHECK(overrides[0].remaining_weight_g == 850.0f);
    CHECK(overrides[0].bed_temp == 65);
    CHECK(overrides[0].nozzle_temp == 220);

    REQUIRE(overrides.count(1) == 1);
    CHECK(overrides[1].material == "PETG");
    CHECK(overrides[1].color_rgb == 0x00FF00u);
    // brand / spoolman_id not present in lane2 entry - default values
    CHECK(overrides[1].brand == "");
    CHECK(overrides[1].spoolman_id == 0);
    // Temps default to 0 when absent — the "use material default" sentinel.
    CHECK(overrides[1].bed_temp == 0);
    CHECK(overrides[1].nozzle_temp == 0);
}

TEST_CASE("FilamentSlotOverrideStore load_blocking skips entries missing lane field",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    // Entry without the required "lane" field - should be skipped silently.
    json bad = {{"material", "PLA"}};
    api.mock_set_db_value("lane_data", "lane1", bad);

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore load_blocking rejects negative lane values",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    json bad = {{"lane", "-1"}, {"material", "PLA"}};
    api.mock_set_db_value("lane_data", "lane1", bad);

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore save_async writes AFC-shaped record to lane_data",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("save_afc");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;   // to_lane_data_record emits "color" iff color_set is true
    ovr.spoolman_id = 42;
    ovr.remaining_weight_g = 850.0f;
    ovr.total_weight_g = 1000.0f;

    bool cb_done = false;
    bool cb_ok = false;
    std::string cb_err;

    // slot index 0 → lane1 key, lane="0" field
    store.save_async(0, ovr, [&](bool ok, std::string err) {
        cb_ok = ok;
        cb_err = std::move(err);
        cb_done = true;
    });

    // MoonrakerAPIMock fires callbacks synchronously in-call.
    REQUIRE(cb_done);
    CHECK(cb_ok);
    CHECK(cb_err.empty());

    // Verify the stored record.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["lane"] == "0");
    CHECK(stored["color"] == "#FF5500");
    CHECK(stored["material"] == "PLA");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["remaining_weight_g"] == 850.0f);
    CHECK(stored["total_weight_g"] == 1000.0f);
    CHECK(stored.contains("scan_time"));  // set by save_async
}

TEST_CASE("FilamentSlotOverrideStore save_async emits explicit bed/nozzle temps",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("save_temps_explicit");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // Override carries explicit temps (e.g. user-entered or Spoolman-derived).
    // These must override any material-DB defaults, so we use a material whose
    // DB defaults differ from the values we set.
    FilamentSlotOverride ovr;
    ovr.material = "PLA";   // PLA defaults: bed 60, nozzle ~205
    ovr.bed_temp = 65;
    ovr.nozzle_temp = 220;

    bool cb_done = false;
    store.save_async(0, ovr, [&](bool, std::string) { cb_done = true; });
    REQUIRE(cb_done);

    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["bed_temp"] == 65);
    CHECK(stored["nozzle_temp"] == 220);
}

TEST_CASE("FilamentSlotOverrideStore save_async falls back to material DB when temps unset",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("save_temps_fallback");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // Override has only material set — no explicit temps. The writer must
    // pull recommended values from the internal material DB so OrcaSlicer's
    // preset auto-sync still gets sensible values.
    FilamentSlotOverride ovr;
    ovr.material = "PETG";  // PETG defaults: bed 80, nozzle ~245

    bool cb_done = false;
    store.save_async(1, ovr, [&](bool, std::string) { cb_done = true; });
    REQUIRE(cb_done);

    auto stored = api.mock_get_db_value("lane_data", "lane2");
    REQUIRE(!stored.is_null());
    REQUIRE(stored.contains("bed_temp"));
    REQUIRE(stored.contains("nozzle_temp"));
    CHECK(stored["bed_temp"] == 80);
    CHECK(stored["nozzle_temp"] == 245);
}

TEST_CASE("FilamentSlotOverrideStore save_async omits temps when no material and no override",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("save_temps_none");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // No material → nothing to look up, no explicit values → nothing to emit.
    FilamentSlotOverride ovr;
    ovr.brand = "eSUN";

    bool cb_done = false;
    store.save_async(0, ovr, [&](bool, std::string) { cb_done = true; });
    REQUIRE(cb_done);

    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK_FALSE(stored.contains("bed_temp"));
    CHECK_FALSE(stored.contains("nozzle_temp"));
}

TEST_CASE("FilamentSlotOverrideStore save_async sets updated_at on the stored record",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("save_updated_at");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    FilamentSlotOverride ovr;
    ovr.brand = "eSUN";
    // updated_at deliberately left at default (epoch)

    bool cb_done = false;
    store.save_async(2, ovr, [&](bool, std::string) { cb_done = true; });
    REQUIRE(cb_done);

    // slot 2 → lane3 key
    auto stored = api.mock_get_db_value("lane_data", "lane3");
    REQUIRE(stored.contains("scan_time"));
    REQUIRE(stored["scan_time"].is_string());

    // Spot check the timestamp is ISO-8601-shaped.
    std::string ts = stored["scan_time"].get<std::string>();
    CHECK(ts.size() >= 20);  // "YYYY-MM-DDTHH:MM:SSZ" = 20 chars
    CHECK(ts.back() == 'Z'); // UTC
    CHECK(ts[10] == 'T');    // ISO-8601 separator
    // Confirm the stamp is recent, not leaking the epoch sentinel through the
    // serializer's "only emit when time_since_epoch > 0" guard.
    CHECK(ts != "1970-01-01T00:00:00Z");

    // Caller's override must NOT have been mutated.
    CHECK(ovr.updated_at == std::chrono::system_clock::time_point{});
}

TEST_CASE("FilamentSlotOverrideStore save_async reports error on MR DB failure",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");

    api.mock_reject_next_db_post();

    FilamentSlotOverride ovr;
    ovr.brand = "X";

    bool cb_done = false;
    bool cb_ok = true;
    std::string cb_err;
    store.save_async(0, ovr, [&](bool ok, std::string err) {
        cb_ok = ok;
        cb_err = std::move(err);
        cb_done = true;
    });

    REQUIRE(cb_done);
    CHECK(!cb_ok);
    CHECK(!cb_err.empty());

    // Record must NOT have been written on rejection.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    CHECK(stored.is_null());
}

TEST_CASE("FilamentSlotOverrideStore clear_async removes single slot",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_single");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed two entries; clearing slot 0 should leave slot 1 untouched.
    nlohmann::json lane1 = {{"lane", "0"}, {"material", "PLA"}};
    nlohmann::json lane2 = {{"lane", "1"}, {"material", "PETG"}};
    api.mock_set_db_value("lane_data", "lane1", lane1);
    api.mock_set_db_value("lane_data", "lane2", lane2);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    bool cb_done = false;
    bool cb_ok = false;
    store.clear_async(0, [&](bool ok, std::string) { cb_ok = ok; cb_done = true; });
    REQUIRE(cb_done);
    CHECK(cb_ok);

    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
    auto lane2_after = api.mock_get_db_value("lane_data", "lane2");
    CHECK(lane2_after["material"] == "PETG");
}

TEST_CASE("FilamentSlotOverrideStore clear_async succeeds for absent slot (idempotent)",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_idempotent");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    bool cb_done = false;
    bool cb_ok = false;
    std::string cb_err;
    store.clear_async(3, [&](bool ok, std::string err) {
        cb_ok = ok;
        cb_err = std::move(err);
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(cb_ok);
    CHECK(cb_err.empty());
}

TEST_CASE("FilamentSlotOverrideStore clear_async rejects negative slot_index",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");

    bool cb_done = false;
    bool cb_ok = true;
    store.clear_async(-1, [&](bool ok, std::string) { cb_ok = ok; cb_done = true; });
    REQUIRE(cb_done);
    CHECK(!cb_ok);
}

TEST_CASE("FilamentSlotOverrideStore clear_async handles null callback gracefully",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_null_cb");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    api.mock_set_db_value("lane_data", "lane1", nlohmann::json{{"lane", "0"}});
    // Should not crash with no callback provided.
    store.clear_async(0, {});
    // Verify delete still happened.
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("FilamentSlotOverrideStore clear_async maps 404 error to success",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_404");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    MoonrakerError err;
    err.code = 404;
    err.message = "Key 'lane1' not found";
    api.mock_reject_next_db_delete(err);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    bool cb_done = false;
    bool cb_ok = false;
    store.clear_async(0, [&](bool ok, std::string) {
        cb_ok = ok;
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(cb_ok); // 404 → treated as success
}

TEST_CASE("FilamentSlotOverrideStore clear_async propagates non-missing-key errors",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    MoonrakerError err;
    err.code = 500;
    err.message = "internal server error";
    api.mock_reject_next_db_delete(err);

    FilamentSlotOverrideStore store(&api, "ifs");
    bool cb_done = false;
    bool cb_ok = true;
    std::string cb_err;
    store.clear_async(0, [&](bool ok, std::string e) {
        cb_ok = ok;
        cb_err = std::move(e);
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(!cb_ok);
    CHECK(cb_err.find("internal server error") != std::string::npos);
}

TEST_CASE("FilamentSlotOverrideStore clear_async maps message-based missing-key error to success",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_msg_missing");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    MoonrakerError err;
    err.code = 0; // no code, only message
    err.message = "Key 'lane1' in namespace 'lane_data' not found";
    api.mock_reject_next_db_delete(err);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    bool cb_done = false;
    bool cb_ok = false;
    store.clear_async(0, [&](bool ok, std::string) {
        cb_ok = ok;
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(cb_ok); // "not found" substring → treated as success
}

// ============================================================================
// Lifetime safety: callback fires after store destroyed (no use-after-free).
//
// The store's async paths capture only value-copied strings + the user's
// callback — never `this`. These tests prove that discipline: a deferred
// callback is fired AFTER the store has been destroyed. If a future edit
// accidentally captured `this` (or any ref to a store member), this would
// UAF under ASan. With the correct discipline, it must be harmless.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore save_async callback fires after store destroyed (no UAF)",
          "[filament_slot_override][slow][lifetime]") {
    // TmpCacheDir declared outside the store scope so it outlives the deferred
    // cache-write fired below — cache path is value-captured into the lambda
    // before store destruction, but the dir still needs to exist when the
    // fire happens.
    TmpCacheDir tmp("lifetime_save");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_post();

    bool cb_fired = false;
    {
        FilamentSlotOverrideStore store(&api, "ifs");
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
        FilamentSlotOverride ovr;
        ovr.brand = "Polymaker";
        store.save_async(0, ovr, [&](bool, std::string) { cb_fired = true; });
        // Store goes out of scope here; the deferred callback has NOT yet fired.
    }

    // Now fire the captured success callback. If save_async captured `this` by
    // reference anywhere, this would UAF under ASan. With value-capture
    // discipline, it must be harmless.
    api.fire_deferred_db_post_success();
    CHECK(cb_fired); // user callback still fires — captured by value into the lambda
}

TEST_CASE("FilamentSlotOverrideStore clear_async callback fires after store destroyed (no UAF)",
          "[filament_slot_override][slow][lifetime]") {
    TmpCacheDir tmp("lifetime_clear");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_delete();

    bool cb_fired = false;
    {
        FilamentSlotOverrideStore store(&api, "ifs");
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
        store.clear_async(0, [&](bool, std::string) { cb_fired = true; });
    }

    api.fire_deferred_db_delete_success();
    CHECK(cb_fired);
}

TEST_CASE("FilamentSlotOverrideStore save_async error callback fires after store destroyed (no UAF)",
          "[filament_slot_override][slow][lifetime]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_post();

    bool cb_fired = false;
    bool cb_ok = true;
    {
        FilamentSlotOverrideStore store(&api, "ifs");
        FilamentSlotOverride ovr;
        store.save_async(0, ovr, [&](bool ok, std::string) {
            cb_ok = ok;
            cb_fired = true;
        });
    }

    // Fire the ERROR path after destruction. This is where the spdlog::warn
    // lambda runs — if it captured `this` or accessed a freed member, we'd
    // crash here. The backend_id_copy + key value-capture make this harmless.
    MoonrakerError err;
    err.code = 500;
    err.message = "internal";
    api.fire_deferred_db_post_error(err);

    CHECK(cb_fired);
    CHECK(!cb_ok);
}

// ============================================================================
// load_blocking() cv.wait_for timeout path.
//
// Uses mock_defer_next_db_get() so the namespace GET never completes, forcing
// load_blocking()'s 5s (default) wait to hit its timeout. Overrides the
// timeout to 50ms via the test-access friend class so the test runs fast.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore load_blocking returns empty on timeout",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_get(); // namespace GET never completes

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_load_timeout(store, std::chrono::milliseconds(50));

    auto before = std::chrono::steady_clock::now();
    auto overrides = store.load_blocking();
    auto elapsed = std::chrono::steady_clock::now() - before;

    CHECK(overrides.empty());
    CHECK(elapsed >= std::chrono::milliseconds(50));
    CHECK(elapsed < std::chrono::milliseconds(500)); // didn't hang at 5s default

    // Clean up: fire the deferred callback so the mock's internal state is
    // tidy before the test exits. The shared_ptr-captured state makes this a
    // harmless flip-of-flags on a now-orphaned structure — matching what would
    // happen if a real Moonraker error fired ~55s after we timed out.
    api.fire_deferred_db_get_error(MoonrakerError{});
}

// ============================================================================
// Malformed-data robustness: Moonraker's lane_data namespace could legitimately
// contain non-lane-prefixed keys (AFC metadata) or even corrupt non-object
// values (mis-seeded by another tool). The store must not crash, must skip
// such entries, and must return a clean best-effort result.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore load_blocking handles non-object namespace value",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed the namespace with a non-object (string) value under a lane-prefixed
    // key — malformed. from_lane_data_record guards on !is_object() and returns
    // nullopt, so the entry is silently skipped.
    api.mock_set_db_value("lane_data", "lane1", nlohmann::json("not an object"));

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore load_blocking skips non-lane-prefixed keys",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // An AFC printer might store config metadata at non-lane keys alongside our
    // entries. Verify we ignore them without crashing. The prefix filter in
    // load_blocking() (key.rfind("lane", 0) != 0) should drop "metadata".
    api.mock_set_db_value("lane_data", "metadata",
                          nlohmann::json{{"version", 1}, {"note", "AFC config"}});
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", "0"}, {"material", "PLA"}});

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();

    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides.size() == 1); // metadata key did not leak in
}

// ============================================================================
// Task 6: local JSON read-cache — write side.
//
// The store mirrors every successful save/clear into a local JSON file under
// the user config dir. This cache is a fallback for offline display (Task 7
// wires the load-time fallback); it is NEVER authoritative. These tests
// verify WRITE behavior: save populates, clear erases, other backends'
// entries are preserved, and a corrupt existing cache doesn't kill the save.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore save_async writes local cache on success",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task6_save_writes");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;

    bool cb_ok = false;
    bool cb_done = false;
    store.save_async(0, ovr, [&](bool ok, std::string) {
        cb_ok = ok;
        cb_done = true;
    });
    REQUIRE(cb_done);
    REQUIRE(cb_ok);

    auto cache = tmp.path / "filament_slot_overrides.json";
    REQUIRE(std::filesystem::exists(cache));

    std::ifstream in(cache);
    auto doc = nlohmann::json::parse(in);
    CHECK(doc["version"] == 1);
    REQUIRE(doc.contains("ifs"));
    REQUIRE(doc["ifs"]["slots"].contains("0"));
    CHECK(doc["ifs"]["slots"]["0"]["brand"] == "Polymaker");
    CHECK(doc["ifs"]["slots"]["0"]["material"] == "PLA");
    CHECK(doc["ifs"]["slots"]["0"]["color_rgb"] == 0xFF5500u);
}

TEST_CASE("FilamentSlotOverrideStore clear_async erases from local cache on success",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task6_clear_erases");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    // First save to populate cache.
    bool save_done = false;
    store.save_async(0, ovr, [&](bool, std::string) { save_done = true; });
    REQUIRE(save_done);

    // Now clear.
    bool clear_done = false;
    store.clear_async(0, [&](bool, std::string) { clear_done = true; });
    REQUIRE(clear_done);

    auto cache = tmp.path / "filament_slot_overrides.json";
    std::ifstream in(cache);
    auto doc = nlohmann::json::parse(in);
    CHECK(!doc["ifs"]["slots"].contains("0"));
}

TEST_CASE("FilamentSlotOverrideStore save_async preserves other backends in cache",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task6_preserves_other");

    // Seed the cache with an ACE entry that our IFS save must leave untouched.
    nlohmann::json seeded = {
        {"version", 1},
        {"ace", {{"slots", {{"0", {{"brand", "eSUN"}}}}}}}
    };
    std::ofstream(tmp.path / "filament_slot_overrides.json") << seeded.dump(2);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    bool save_done = false;
    store.save_async(0, ovr, [&](bool, std::string) { save_done = true; });
    REQUIRE(save_done);

    std::ifstream in(tmp.path / "filament_slot_overrides.json");
    auto doc = nlohmann::json::parse(in);
    CHECK(doc["ifs"]["slots"]["0"]["brand"] == "Polymaker");
    CHECK(doc["ace"]["slots"]["0"]["brand"] == "eSUN"); // untouched!
}

TEST_CASE("FilamentSlotOverrideStore save_async survives corrupt existing cache",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task6_corrupt");

    // Seed with unparseable JSON.
    std::ofstream(tmp.path / "filament_slot_overrides.json") << "not json at all {{{ ";

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    bool save_ok = false;
    bool save_done = false;
    store.save_async(0, ovr, [&](bool ok, std::string) {
        save_ok = ok;
        save_done = true;
    });
    REQUIRE(save_done);
    CHECK(save_ok); // The MR DB save succeeded; cache corruption is not fatal.

    // After save, cache should be reset to a well-formed doc containing our entry.
    std::ifstream in(tmp.path / "filament_slot_overrides.json");
    auto doc = nlohmann::json::parse(in);
    CHECK(doc["ifs"]["slots"]["0"]["brand"] == "Polymaker");
}

// ============================================================================
// Task 7: local JSON read-cache — read side.
//
// When load_blocking cannot reach the Moonraker DB (connection error OR
// cv.wait_for timeout), it falls back to the on-disk cache so the UI can show
// last-known metadata offline. A successful-but-empty MR DB response is NOT
// cache-fallback-eligible — it's authoritative "no overrides configured" and
// stale cache data must not leak past it.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore load_blocking falls back to cache when MR DB connection fails",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task7_fallback_error");

    // Seed the cache file directly with a known entry.
    nlohmann::json doc = {
        {"version", 1},
        {"ifs", {{"slots", {
            {"0", {
                {"brand", "Polymaker"},
                {"material", "PLA"},
                {"color_rgb", 0xFF5500},
            }},
        }}}}
    };
    std::ofstream(tmp.path / "filament_slot_overrides.json") << doc.dump(2);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Force MR DB GET to fail — simulates a connection/server failure.
    api.mock_reject_next_db_get();

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "Polymaker");
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides[0].color_rgb == 0xFF5500u);
}

TEST_CASE("FilamentSlotOverrideStore load_blocking falls back to cache on MR DB timeout",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task7_fallback_timeout");

    nlohmann::json doc = {
        {"version", 1},
        {"ifs", {{"slots", {{"0", {{"brand", "eSUN"}}}}}}}
    };
    std::ofstream(tmp.path / "filament_slot_overrides.json") << doc.dump(2);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_get();  // never fires within the wait window

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    FilamentSlotOverrideStoreTestAccess::set_load_timeout(store, std::chrono::milliseconds(50));

    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "eSUN");

    // Clean up the deferred capture so the mock's internal state is tidy —
    // matches the pattern used by the load_blocking-timeout test above.
    api.fire_deferred_db_get_error(MoonrakerError{});
}

TEST_CASE("FilamentSlotOverrideStore load_blocking does NOT use cache when MR DB returns empty namespace",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task7_empty_ns_no_fallback");

    // Seed cache with stale data that we must NOT return — an empty-but-success
    // MR DB response is authoritative.
    nlohmann::json doc = {
        {"version", 1},
        {"ifs", {{"slots", {{"0", {{"brand", "StaleBrand"}}}}}}}
    };
    std::ofstream(tmp.path / "filament_slot_overrides.json") << doc.dump(2);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    // No seed into lane_data — MR DB returns empty (success, not error).

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());  // MR DB said "no overrides" → trust it.
}

TEST_CASE("FilamentSlotOverrideStore load_blocking cache fallback handles missing file",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task7_missing_cache");
    // No cache file created — TmpCacheDir makes the dir but not the file.

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_reject_next_db_get();

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());  // first-run with no cache → empty, not a crash.
}

TEST_CASE("FilamentSlotOverrideStore load_blocking cache fallback handles corrupt cache",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task7_corrupt_cache");
    std::ofstream(tmp.path / "filament_slot_overrides.json") << "not json";

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_reject_next_db_get();

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());  // corrupt cache → treat as empty, don't crash.
}

TEST_CASE("FilamentSlotOverrideStore load_blocking cache returns only this backend's slots",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("task7_backend_isolation");

    // Seed cache with both ifs and ace entries; only the ifs ones are ours.
    nlohmann::json doc = {
        {"version", 1},
        {"ifs", {{"slots", {{"0", {{"brand", "Poly"}}}}}}},
        {"ace", {{"slots", {{"0", {{"brand", "SHOULD_NOT_LEAK"}}}}}}}
    };
    std::ofstream(tmp.path / "filament_slot_overrides.json") << doc.dump(2);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_reject_next_db_get();

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "Poly");
    CHECK(overrides.size() == 1);  // ace entries did not leak.
}

// ============================================================================
// Task 8: one-shot legacy-namespace migration.
//
// Pre-Task-8, ACE and CFS stored per-slot overrides in a single doc at
// helix-screen:{backend_id}_slot_overrides. We now use the AFC-shaped
// lane_data namespace (lane1, lane2, ...). On first startup after upgrade,
// load_blocking migrates the legacy data forward so users don't lose their
// overrides. IFS and Snapmaker never wrote a legacy namespace — they must
// skip migration entirely. Migration runs only when lane_data is empty
// (idempotence guard) and MR DB is reachable (the lane_data round-trip
// already proved that at the call site).
// ============================================================================

TEST_CASE("Migration: ACE backend migrates legacy namespace to lane_data on first load",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_ace_migrate");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed legacy namespace with 2 slots; lane_data is empty → forces migration.
    json legacy = {
        {"0", {
            {"brand", "Polymaker"},
            {"material", "PLA"},
            {"color_rgb", 0xFF5500},
            {"spoolman_id", 42},
            {"remaining_weight_g", 850.0},
            {"total_weight_g", 1000.0},
        }},
        {"2", {
            {"brand", "eSUN"},
            {"material", "PETG"},
            {"color_rgb", 0x00FF00},
        }},
    };
    api.mock_set_db_value("helix-screen", "ace_slot_overrides", legacy);

    FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();

    // Migrated slots returned as if they'd come from lane_data.
    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "Polymaker");
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides[0].color_rgb == 0xFF5500u);
    CHECK(overrides[0].spoolman_id == 42);
    CHECK(overrides[0].remaining_weight_g == 850.0f);

    REQUIRE(overrides.count(2) == 1);
    CHECK(overrides[2].brand == "eSUN");

    // lane_data now has the records in AFC shape (1-based keys, 0-based inner
    // "lane" field — the same invariant enforced by to_lane_data_record).
    auto lane1 = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!lane1.is_null());
    CHECK(lane1["vendor"] == "Polymaker");
    CHECK(lane1["lane"] == "0");

    auto lane3 = api.mock_get_db_value("lane_data", "lane3");
    REQUIRE(!lane3.is_null());
    CHECK(lane3["vendor"] == "eSUN");
    CHECK(lane3["lane"] == "2");

    // Legacy namespace deleted post-migration — second startup sees lane_data
    // populated and skips migration entirely.
    CHECK(api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("Migration: CFS backend migrates its legacy namespace, not ACE's",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_cfs_isolation");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_set_db_value("helix-screen", "cfs_slot_overrides",
                          json{{"0", {{"brand", "CFS-Brand"}}}});
    // ACE legacy also exists — must NOT be touched by a CFS store load. The
    // per-backend key-naming discipline is the only thing keeping the two
    // backends' migrations from colliding.
    api.mock_set_db_value("helix-screen", "ace_slot_overrides",
                          json{{"0", {{"brand", "ACE-Brand"}}}});

    FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "CFS-Brand");

    // CFS legacy deleted; ACE legacy untouched.
    CHECK(api.mock_get_db_value("helix-screen", "cfs_slot_overrides").is_null());
    auto ace_legacy = api.mock_get_db_value("helix-screen", "ace_slot_overrides");
    REQUIRE(!ace_legacy.is_null());
    CHECK(ace_legacy["0"]["brand"] == "ACE-Brand");
}

TEST_CASE("Migration: IFS backend skips migration entirely",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_ifs_skip");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Even if a malformed "helix-screen:ifs_slot_overrides" somehow existed
    // (e.g. hand-seeded during testing, or a misconfigured third-party tool),
    // the IFS store must NOT attempt to migrate it — IFS never used this
    // namespace, and silently consuming it could corrupt unrelated data.
    api.mock_set_db_value("helix-screen", "ifs_slot_overrides",
                          json{{"0", {{"brand", "X"}}}});

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
    // Legacy entry untouched — nothing read it, nothing deleted it.
    CHECK(!api.mock_get_db_value("helix-screen", "ifs_slot_overrides").is_null());
}

TEST_CASE("Migration: no-op when lane_data already populated",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_no_op_with_lane_data");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Both lane_data AND legacy present. lane_data wins; legacy untouched.
    // This is the "second startup after migration already happened" case,
    // OR a manually-seeded lane_data that must NOT be clobbered by stale
    // legacy data. Either way, lane_data is the source of truth.
    api.mock_set_db_value("lane_data", "lane1",
                          json{{"lane", "0"}, {"vendor", "NewData"}});
    api.mock_set_db_value("helix-screen", "ace_slot_overrides",
                          json{{"0", {{"brand", "LegacyData"}}}});

    FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "NewData");
    // Legacy untouched — migration did not run.
    CHECK(!api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("Migration: idempotent (second startup after migration is no-op)",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_idempotent");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_set_db_value("helix-screen", "ace_slot_overrides",
                          json{{"0", {{"brand", "Polymaker"}, {"material", "PLA"}}}});

    FilamentSlotOverrideStore store1(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store1, tmp.path);
    auto first = store1.load_blocking();
    REQUIRE(first.count(0) == 1);
    CHECK(first[0].brand == "Polymaker");

    // Second startup — same printer, MR DB now has lane_data populated. The
    // legacy blob was already deleted by the first startup, so there is
    // literally nothing for a second migration attempt to latch onto.
    FilamentSlotOverrideStore store2(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store2, tmp.path);
    auto second = store2.load_blocking();
    REQUIRE(second.count(0) == 1);
    CHECK(second[0].brand == "Polymaker");
    CHECK(api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("Migration: aborts without deleting legacy if write fails",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_abort_on_write_fail");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_set_db_value("helix-screen", "ace_slot_overrides",
                          json{{"0", {{"brand", "Polymaker"}}}});

    // Reject the lane_data post — migration must abort before deleting legacy.
    api.mock_reject_next_db_post(MoonrakerError{});

    FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    auto overrides = store.load_blocking();
    // Migration failed — empty result (load_blocking falls through to return
    // the empty lane_data map; no cache fallback because MR DB was reachable).
    CHECK(overrides.empty());
    // Legacy PRESERVED so the next startup can retry. Deleting on failure
    // would drop user data irrecoverably.
    CHECK(!api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
    // lane_data remains empty — no partial write leaked through.
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("Migration: non-object slot entries are skipped silently",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_malformed_slot");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // "0" is malformed (string, not object). "1" is valid. The migration
    // must not choke on the bad entry — skip it, migrate "1", and still
    // delete the legacy blob since the good entries transferred cleanly.
    json legacy = {
        {"0", "not an object"},
        {"1", {{"brand", "Good"}, {"material", "PLA"}}},
    };
    api.mock_set_db_value("helix-screen", "ace_slot_overrides", legacy);

    FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(1) == 1);
    CHECK(overrides[1].brand == "Good");
    CHECK(overrides.count(0) == 0);  // malformed entry dropped
}

TEST_CASE("Migration: legacy with only malformed entries is still cleaned up",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_all_malformed");

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Legacy with ONLY malformed entries (all non-object).
    nlohmann::json legacy = {
        {"0", "not an object"},
        {"1", 42},  // not an object either
    };
    api.mock_set_db_value("helix-screen", "ace_slot_overrides", legacy);

    FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());

    // Legacy should be deleted (dropped as unsalvageable) so we don't re-scan
    // every startup.
    CHECK(api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("Migration: deletes legacy per-backend local JSON file after success",
          "[filament_slot_override][migration][slow]") {
    TmpCacheDir tmp("task8_local_file_cleanup");

    // Seed the per-backend legacy local JSON file that pre-dates Task 6's
    // unified filament_slot_overrides.json. Nothing reads it anymore, but
    // the migration should remove it as cleanup so users inspecting their
    // config dir don't see stale files lying around.
    auto legacy_local = tmp.path / "ace_slot_overrides.json";
    std::ofstream(legacy_local) << R"({"0": {"brand": "Old"}})";
    REQUIRE(std::filesystem::exists(legacy_local));

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_set_db_value("helix-screen", "ace_slot_overrides",
                          json{{"0", {{"brand", "Polymaker"}}}});

    FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    auto overrides = store.load_blocking();
    REQUIRE(overrides.count(0) == 1);

    // Post-migration, the per-backend file is gone.
    CHECK(!std::filesystem::exists(legacy_local));
}

// ============================================================================
// mirror_firmware_to_lane_data — shared CFS / Snapmaker / IFS auto-mirror
// ============================================================================
//
// Backend status handlers call this on every parse to publish firmware-detected
// color/material into the lane_data namespace so OrcaSlicer's
// MoonrakerPrinterAgent can sync filaments. Two policies:
//
//   - FillUnsetOnly  (CFS, Snapmaker): user edits don't propagate to firmware,
//     so the mirror must not clobber them. Only fills empty fields.
//   - OverwriteAlways (IFS): user edits round-trip through firmware
//     (Adventurer5M.json), so firmware-truth and user-truth converge after a
//     write. Mirror catches external edits.
//
// The save_async path runs through the same FilamentSlotOverrideStore tested
// above, so these tests focus on the mirror's policy decisions and rate-limit
// behavior. Side-effects on the Moonraker mock confirm that save_async fires
// (or doesn't) as expected.

TEST_CASE("mirror_firmware_to_lane_data FillUnsetOnly: empty override gets firmware values",
          "[mirror_firmware][slow]") {
    TmpCacheDir tmp("mirror_fillunset_empty");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    std::unordered_map<int, FilamentSlotOverride> overrides;
    bool changed = helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, /*slot_index=*/0, /*firmware_color=*/0xFF5500,
        /*firmware_material=*/"PLA", /*slot_has_filament=*/true,
        helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");
    CHECK(changed);
    CHECK(overrides[0].color_rgb == 0xFF5500u);
    CHECK(overrides[0].material == "PLA");

    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["color"] == "#FF5500");
    CHECK(stored["material"] == "PLA");
}

TEST_CASE("mirror_firmware_to_lane_data FillUnsetOnly: user color preserved against firmware",
          "[mirror_firmware][slow]") {
    TmpCacheDir tmp("mirror_fillunset_user_wins");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // Simulate a user who already set blue + brand "Bambu" via set_slot_info.
    // color_set is the explicit "user set this" signal — pure black is a
    // legitimate user choice, so we cannot rely on color_rgb != 0.
    std::unordered_map<int, FilamentSlotOverride> overrides;
    overrides[0].color_rgb = 0x0000FF;
    overrides[0].color_set = true;
    overrides[0].material = "PETG";
    overrides[0].brand = "Bambu";

    // Firmware reports a different color/material on its next status poll —
    // typical CFS RFID read. The user's edit must NOT be overwritten.
    bool changed = helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0xFF0000, "PLA", true,
        helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");
    CHECK_FALSE(changed);
    CHECK(overrides[0].color_rgb == 0x0000FFu); // user's blue preserved
    CHECK(overrides[0].color_set == true);
    CHECK(overrides[0].material == "PETG");
    CHECK(overrides[0].brand == "Bambu");

    // No save fired — lane_data not written.
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("mirror_firmware_to_lane_data: pure black firmware color is mirrored "
          "(NOT treated as no-signal)",
          "[mirror_firmware][slow]") {
    // Regression for the K2 lane1 bug: firmware reports loaded black PLA as
    // RGB 0x000000 (color_value="0000000"). The pre-color_set helper used
    // `firmware_color == 0` as a "no signal" sentinel and silently dropped
    // the slot from lane_data, so OrcaSlicer never saw the loaded black.
    // Now color_set is the explicit signal; firmware_color == 0 IS a real
    // color and must round-trip into lane_data as "color":"#000000".
    TmpCacheDir tmp("mirror_black_filament");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    std::unordered_map<int, FilamentSlotOverride> overrides;

    SECTION("FillUnsetOnly fills empty override with firmware black") {
        bool changed = helix::ams::mirror_firmware_to_lane_data(
            &store, overrides, 0, /*firmware_color=*/0x000000, "PLA",
            /*slot_has_filament=*/true, helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");
        REQUIRE(changed);
        CHECK(overrides[0].color_rgb == 0u);
        CHECK(overrides[0].color_set == true);
        CHECK(overrides[0].material == "PLA");

        auto stored = api.mock_get_db_value("lane_data", "lane1");
        REQUIRE(!stored.is_null());
        CHECK(stored["color"] == "#000000"); // <-- the bug we fixed
        CHECK(stored["material"] == "PLA");
    }

    SECTION("OverwriteAlways propagates firmware black even over a prior color") {
        overrides[0].color_rgb = 0xFFFFFF;
        overrides[0].color_set = true;

        bool changed = helix::ams::mirror_firmware_to_lane_data(
            &store, overrides, 0, /*firmware_color=*/0x000000, "PLA", true,
            helix::ams::MirrorPolicy::OverwriteAlways, "[test]");
        REQUIRE(changed);
        CHECK(overrides[0].color_rgb == 0u);

        auto stored = api.mock_get_db_value("lane_data", "lane1");
        REQUIRE(!stored.is_null());
        CHECK(stored["color"] == "#000000");
    }
}

TEST_CASE("mirror_firmware_to_lane_data FillUnsetOnly: partial override fills only the gap",
          "[mirror_firmware][slow]") {
    TmpCacheDir tmp("mirror_fillunset_partial");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // User has set brand/spool_name but never picked color/material — firmware
    // RFID is the source for those.
    std::unordered_map<int, FilamentSlotOverride> overrides;
    overrides[0].brand = "Polymaker";
    overrides[0].spool_name = "PolyTerra Sage";
    // color_rgb default 0, material default empty

    bool changed = helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0x88AA66, "PLA", true,
        helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");
    CHECK(changed);
    CHECK(overrides[0].color_rgb == 0x88AA66u);
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides[0].brand == "Polymaker");      // user field preserved
    CHECK(overrides[0].spool_name == "PolyTerra Sage");

    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["color"] == "#88AA66");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_name"] == "PolyTerra Sage");
}

TEST_CASE("mirror_firmware_to_lane_data: no-signal cases skip writing",
          "[mirror_firmware][slow]") {
    TmpCacheDir tmp("mirror_no_signal");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    std::unordered_map<int, FilamentSlotOverride> overrides;

    SECTION("empty slot — nothing to publish") {
        bool changed = helix::ams::mirror_firmware_to_lane_data(
            &store, overrides, 0, 0xFF5500, "PLA", /*slot_has_filament=*/false,
            helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");
        CHECK_FALSE(changed);
        CHECK(overrides.empty()); // no phantom entry created
        CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
    }

    // Pure-black case is covered by its own test case "mirror_firmware_to_lane_data:
    // pure black firmware color is mirrored" — the helper deliberately does
    // NOT treat firmware_color == 0 as a no-signal sentinel.
}

TEST_CASE("mirror_firmware_to_lane_data OverwriteAlways: external color edit propagates",
          "[mirror_firmware][slow]") {
    TmpCacheDir tmp("mirror_overwrite_external");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // IFS state: user previously picked blue via Helix; that wrote
    // Adventurer5M.json AND saved override blue. Then someone edited the color
    // to red via Mainsail console / native LCD / CHANGE_ZCOLOR — firmware
    // truth changes, and the next status parse calls the mirror.
    std::unordered_map<int, FilamentSlotOverride> overrides;
    overrides[0].color_rgb = 0x0000FF;
    overrides[0].material = "PLA";
    overrides[0].brand = "Polymaker";

    bool changed = helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0xFF0000, "PETG", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]");
    CHECK(changed);
    CHECK(overrides[0].color_rgb == 0xFF0000u); // firmware-truth wins
    CHECK(overrides[0].material == "PETG");
    CHECK(overrides[0].brand == "Polymaker"); // brand still preserved (mirror only touches color/material)

    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["color"] == "#FF0000");
    CHECK(stored["material"] == "PETG");
    CHECK(stored["vendor"] == "Polymaker");
}

TEST_CASE("mirror_firmware_to_lane_data: steady state does not churn lane_data",
          "[mirror_firmware][slow]") {
    TmpCacheDir tmp("mirror_steady_state");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    std::unordered_map<int, FilamentSlotOverride> overrides;

    // First call seeds the override + writes lane_data.
    CHECK(helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0xABCDEF, "ABS", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]"));
    auto first = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!first.is_null());
    auto first_scan_time = first.value("scan_time", "");
    REQUIRE(!first_scan_time.empty());

    // Subsequent calls with identical firmware values must not write —
    // otherwise we'd hit Moonraker on every status poll just to rewrite the
    // same record (and bump scan_time). Sleep briefly so a hypothetical second
    // save would produce a different scan_time and we'd notice.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK_FALSE(helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0xABCDEF, "ABS", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]"));
    CHECK_FALSE(helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0xABCDEF, "ABS", true,
        helix::ams::MirrorPolicy::FillUnsetOnly, "[test]"));

    auto second = api.mock_get_db_value("lane_data", "lane1");
    CHECK(second.value("scan_time", "") == first_scan_time);
}

TEST_CASE("mirror_firmware_to_lane_data: null store updates in-memory only",
          "[mirror_firmware]") {
    // Real-world case: backend init race or test fixture without a Moonraker
    // API. The in-memory override should still be staged so the next status
    // parse sees consistent state once the store comes online.
    std::unordered_map<int, FilamentSlotOverride> overrides;
    bool changed = helix::ams::mirror_firmware_to_lane_data(
        /*store=*/nullptr, overrides, 0, 0x123456, "PLA", true,
        helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");
    CHECK(changed);
    CHECK(overrides[0].color_rgb == 0x123456u);
    CHECK(overrides[0].material == "PLA");
}

TEST_CASE("mirror_firmware_to_lane_data OverwriteAlways: user-locked material is not overwritten "
          "(#965 regression)",
          "[mirror_firmware]") {
    // #965: AD5X firmware re-emits prior FFMInfo material after print completes,
    // causing parse_adventurer_json to feed mismatched material into the
    // mirror. The OverwriteAlways policy was blindly clobbering the user's
    // material override. With user_locked_material the field is protected.
    std::unordered_map<int, FilamentSlotOverride> overrides;
    auto& ovr = overrides[0];
    ovr.color_rgb = 0xFF0000;
    ovr.color_set = true;
    ovr.material = "TPU";
    ovr.user_locked_color = true;
    ovr.user_locked_material = true;

    // Firmware suddenly reports a different material with same color (post-print
    // FFMInfo revert) — the mirror must NOT overwrite the user's choice.
    bool changed = helix::ams::mirror_firmware_to_lane_data(
        /*store=*/nullptr, overrides, 0, 0xFF0000, "HIPS", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]");
    CHECK_FALSE(changed);
    CHECK(overrides[0].material == "TPU");
    CHECK(overrides[0].color_rgb == 0xFF0000u);

    // Firmware reports a different color too — color is also locked, so still
    // a no-op. Both fields hold against firmware re-emission.
    changed = helix::ams::mirror_firmware_to_lane_data(
        /*store=*/nullptr, overrides, 0, 0x0000FF, "HIPS", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]");
    CHECK_FALSE(changed);
    CHECK(overrides[0].color_rgb == 0xFF0000u);
    CHECK(overrides[0].material == "TPU");
}

TEST_CASE("mirror_firmware_to_lane_data OverwriteAlways: partial lock — color locked, material free",
          "[mirror_firmware]") {
    // User edited color but left material blank ("auto from firmware"); the
    // mirror should still fill material from firmware truth, but never touch
    // the locked color.
    std::unordered_map<int, FilamentSlotOverride> overrides;
    auto& ovr = overrides[0];
    ovr.color_rgb = 0xABCDEF;
    ovr.color_set = true;
    ovr.user_locked_color = true;
    // material empty, user_locked_material false

    bool changed = helix::ams::mirror_firmware_to_lane_data(
        /*store=*/nullptr, overrides, 0, 0x123456, "PLA", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]");
    CHECK(changed);
    CHECK(overrides[0].color_rgb == 0xABCDEFu); // locked — untouched
    CHECK(overrides[0].material == "PLA");      // unlocked — bootstrap filled
}

TEST_CASE("mirror_firmware_to_lane_data OverwriteAlways: auto-mirrored entry still tracks firmware",
          "[mirror_firmware]") {
    // Bootstrap case: empty override → first mirror call fills color/material
    // and leaves locks false. A subsequent external edit should still
    // propagate — otherwise OrcaSlicer's lane_data goes stale.
    std::unordered_map<int, FilamentSlotOverride> overrides;

    // First call: bootstrap from empty override.
    CHECK(helix::ams::mirror_firmware_to_lane_data(
        /*store=*/nullptr, overrides, 0, 0xFF0000, "PLA", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]"));
    CHECK(overrides[0].color_rgb == 0xFF0000u);
    CHECK(overrides[0].material == "PLA");
    CHECK_FALSE(overrides[0].user_locked_color);
    CHECK_FALSE(overrides[0].user_locked_material);

    // Second call: external edit changes color — locks are false, so mirror
    // tracks the new firmware truth.
    CHECK(helix::ams::mirror_firmware_to_lane_data(
        /*store=*/nullptr, overrides, 0, 0x00FF00, "PETG", true,
        helix::ams::MirrorPolicy::OverwriteAlways, "[test]"));
    CHECK(overrides[0].color_rgb == 0x00FF00u);
    CHECK(overrides[0].material == "PETG");
}

TEST_CASE("load_blocking: legacy lane_data record (no helix_locked_*) loads as user-locked "
          "(#965 pessimistic default)",
          "[filament_slot_override][slow]") {
    // Pre-fix records (v0.99.60–.67) don't carry helix_locked_* fields. Many
    // of those entries came from user edits — auto-mirror would silently
    // clobber them on the next firmware change after upgrade. Treat legacy
    // records with values as user-locked so existing data survives.
    TmpCacheDir tmp("legacy_lane_data_lock_default");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", "0"},
                                         {"color", "#AABBCC"},
                                         {"material", "PLA"},
                                         {"vendor", "Polymaker"}});
    api.mock_set_db_value("lane_data", "lane2",
                          nlohmann::json{{"lane", "1"},
                                         {"color", "#112233"},
                                         {"vendor", "eSun"}});

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    auto loaded = store.load_blocking();
    REQUIRE(loaded.size() == 2);

    // Slot 0: both color and material present → both locked.
    const auto& s0 = loaded.at(0);
    CHECK(s0.color_rgb == 0xAABBCCu);
    CHECK(s0.material == "PLA");
    CHECK(s0.user_locked_color);
    CHECK(s0.user_locked_material);

    // Slot 1: only color set, no material → color locked, material unlocked
    // (material is fair game for bootstrap fill).
    const auto& s1 = loaded.at(1);
    CHECK(s1.color_rgb == 0x112233u);
    CHECK(s1.material.empty());
    CHECK(s1.user_locked_color);
    CHECK_FALSE(s1.user_locked_material);
}

TEST_CASE("save + load round-trip preserves explicit lock state",
          "[filament_slot_override][slow]") {
    // Auto-mirror records (locks=false) must reload as locks=false so
    // subsequent firmware changes still propagate. The explicit-emission rule
    // in to_lane_data_record distinguishes "explicit false" from "missing".
    TmpCacheDir tmp("lock_state_roundtrip");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    // Slot 0: simulate auto-mirror — both fields set, both locks false.
    FilamentSlotOverride auto_mirrored;
    auto_mirrored.color_rgb = 0xAABBCC;
    auto_mirrored.color_set = true;
    auto_mirrored.material = "PLA";
    auto_mirrored.user_locked_color = false;
    auto_mirrored.user_locked_material = false;

    bool done0 = false;
    store.save_async(0, auto_mirrored, [&](bool, std::string) { done0 = true; });
    REQUIRE(done0);

    // Slot 1: user edit — both locked.
    FilamentSlotOverride user_edit;
    user_edit.color_rgb = 0x112233;
    user_edit.color_set = true;
    user_edit.material = "PETG";
    user_edit.user_locked_color = true;
    user_edit.user_locked_material = true;

    bool done1 = false;
    store.save_async(1, user_edit, [&](bool, std::string) { done1 = true; });
    REQUIRE(done1);

    auto loaded = store.load_blocking();
    REQUIRE(loaded.size() == 2);
    CHECK_FALSE(loaded.at(0).user_locked_color);
    CHECK_FALSE(loaded.at(0).user_locked_material);
    CHECK(loaded.at(1).user_locked_color);
    CHECK(loaded.at(1).user_locked_material);
}

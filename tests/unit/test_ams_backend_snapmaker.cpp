// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_snapmaker.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

// Friend-class shim for FilamentSlotOverrideStore — same idiom as IFS tests
// and test_filament_slot_override_store.cpp. Lets us redirect the store's
// on-disk read-cache to a per-test tmp dir so save_async doesn't pollute
// the developer's real helixscreen config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

// Friend-class shim for AmsBackendSnapmaker — declared as friend in the
// backend header. Provides narrow, purpose-built accessors for the private
// override and hardware-event-detection state so tests don't have to reach
// into the backend via public APIs (which layer apply_overrides on top and
// obscure what the internal maps actually hold).
class SnapmakerTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const json& n) {
        b.handle_status_update(n);
    }
    static void seed_override(AmsBackendSnapmaker& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendSnapmaker& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    static std::optional<std::string> last_rfid_uid(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.last_rfid_uid_.find(slot_index);
        if (it == b.last_rfid_uid_.end())
            return std::nullopt;
        return it->second;
    }
};

namespace {
// Per-test tmp cache dir — same idiom as test_ams_backend_ad5x_ifs.cpp.
struct SnapmakerTmpCacheDir {
    std::filesystem::path path;
    explicit SnapmakerTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("snapmaker_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~SnapmakerTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Build a filament_detect status notification for a single slot with
// configurable material, color, brand, and CARD_UID. Slot index is 0-based;
// the notification's info array is padded to NUM_TOOLS=4 with "NONE" entries.
json make_filament_detect_status(int slot_index, const std::string& main_type,
                                 uint32_t argb_color, const std::string& manufacturer,
                                 const json& card_uid) {
    json info_arr = json::array();
    for (int i = 0; i < 4; ++i) {
        if (i == slot_index) {
            info_arr.push_back(json{{"MAIN_TYPE", main_type},
                                    {"SUB_TYPE", "Basic"},
                                    {"MANUFACTURER", manufacturer},
                                    {"VENDOR", "Snapmaker"},
                                    {"ARGB_COLOR", argb_color},
                                    {"HOTEND_MIN_TEMP", 190},
                                    {"HOTEND_MAX_TEMP", 220},
                                    {"BED_TEMP", 60},
                                    {"WEIGHT", 1000},
                                    {"CARD_UID", card_uid}});
        } else {
            info_arr.push_back(json{{"MAIN_TYPE", "NONE"}});
        }
    }
    return json{{"filament_detect", json{{"info", info_arr}}}};
}
} // namespace

TEST_CASE("Snapmaker type enum", "[ams][snapmaker]") {
    SECTION("SNAPMAKER is a valid AmsType") {
        auto t = AmsType::SNAPMAKER;
        REQUIRE(t != AmsType::NONE);
        REQUIRE(static_cast<int>(t) == 7);
    }

    SECTION("SNAPMAKER is both a tool changer and filament system") {
        REQUIRE(is_tool_changer(AmsType::SNAPMAKER));
        REQUIRE(is_filament_system(AmsType::SNAPMAKER));
    }

    SECTION("ams_type_to_string returns Snapmaker") {
        REQUIRE(std::string(ams_type_to_string(AmsType::SNAPMAKER)) == "Snapmaker");
    }

    SECTION("ams_type_from_string parses Snapmaker variants") {
        REQUIRE(ams_type_from_string("snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("Snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("snapswap") == AmsType::SNAPMAKER);
    }
}

TEST_CASE("Snapmaker detection via filament_detect", "[ams][snapmaker]") {
    PrinterDiscovery discovery;

    SECTION("filament_detect triggers SNAPMAKER detection") {
        nlohmann::json objects = nlohmann::json::array({
            "extruder", "extruder1", "extruder2", "extruder3",
            "toolchanger", "filament_detect", "toolhead",
            "heater_bed", "print_task_config"
        });
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_snapmaker());
        REQUIRE(discovery.mmu_type() == AmsType::SNAPMAKER);
    }

    SECTION("empty toolchanger without filament_detect is not SNAPMAKER") {
        nlohmann::json objects = nlohmann::json::array({
            "extruder", "toolchanger", "tool T0", "tool T1", "toolhead"
        });
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_snapmaker());
        REQUIRE(discovery.has_tool_changer());
    }
}

// ============================================================================
// Backend Construction Tests
// ============================================================================

#include "ams_backend_snapmaker.h"
#include "hv/json.hpp"

TEST_CASE("AmsBackendSnapmaker construction", "[ams][snapmaker]") {
    SECTION("type returns SNAPMAKER") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == AmsType::SNAPMAKER);
    }

    SECTION("topology is PARALLEL") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_topology() == PathTopology::PARALLEL);
    }

    SECTION("name is Snapmaker SnapSwap") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.type_name == "Snapmaker SnapSwap");
    }

    SECTION("has 4 slots in 1 unit") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.total_slots == 4);
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].slot_count == 4);
    }

    SECTION("tool_to_slot_map is 1:1 identity (gates 2D toolpath colors)") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() == 4);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 1);
        REQUIRE(info.tool_to_slot_map[2] == 2);
        REQUIRE(info.tool_to_slot_map[3] == 3);
    }
}

// ============================================================================
// Extruder State Parser Tests
// ============================================================================

TEST_CASE("Snapmaker extruder state parsing", "[ams][snapmaker]") {
    SECTION("parses parked extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "park_pin": true,
            "active_pin": false,
            "grab_valid_pin": false,
            "activating_move": false,
            "extruder_offset": [0.073, -0.037, 0.0],
            "switch_count": 86,
            "retry_count": 0,
            "error_count": 1
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "PARKED");
        REQUIRE(state.park_pin == true);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.073f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(-0.037f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
        REQUIRE(state.switch_count == 86);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 1);
    }

    SECTION("parses active extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVE",
            "park_pin": false,
            "active_pin": true,
            "activating_move": false,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 12,
            "retry_count": 2,
            "error_count": 0
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "ACTIVE");
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == true);
        REQUIRE(state.switch_count == 12);
        REQUIRE(state.retry_count == 2);
        REQUIRE(state.error_count == 0);
    }

    SECTION("parses activating move in progress") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVATING",
            "park_pin": false,
            "active_pin": false,
            "activating_move": true,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 5,
            "retry_count": 0,
            "error_count": 0
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "ACTIVATING");
        REQUIRE(state.activating_move == true);
    }

    SECTION("handles missing fields gracefully") {
        auto j = nlohmann::json::parse("{}");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state.empty());
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.switch_count == 0);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 0);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }

    SECTION("handles partial extruder_offset array") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "extruder_offset": [1.5]
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(1.5f));
        // Missing indices stay at default
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }
}

// ============================================================================
// RFID Info Parser Tests
// ============================================================================

TEST_CASE("Snapmaker RFID info parsing", "[ams][snapmaker]") {
    SECTION("parses full RFID tag data") {
        // ARGB 0xFF080A0D -> RGB 0x080A0D
        auto j = nlohmann::json::parse(R"({
            "VERSION": 1,
            "VENDOR": "Snapmaker",
            "MANUFACTURER": "Polymaker",
            "MAIN_TYPE": "PLA",
            "SUB_TYPE": "SnapSpeed",
            "ARGB_COLOR": 4278716941,
            "DIAMETER": 175,
            "WEIGHT": 500,
            "HOTEND_MAX_TEMP": 230,
            "HOTEND_MIN_TEMP": 190,
            "BED_TEMP": 60,
            "OFFICIAL": true
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PLA");
        REQUIRE(info.sub_type == "SnapSpeed");
        REQUIRE(info.manufacturer == "Polymaker");
        REQUIRE(info.vendor == "Snapmaker");
        REQUIRE(info.hotend_min_temp == 190);
        REQUIRE(info.hotend_max_temp == 230);
        REQUIRE(info.bed_temp == 60);
        REQUIRE(info.weight_g == 500);
        // ARGB 4278716941 = 0xFF080A0D → mask off alpha → 0x080A0D
        REQUIRE(info.color_rgb == 0x080A0Du);
    }

    SECTION("ARGB alpha byte is masked to produce RGB") {
        // 0xFF0000FF (opaque blue) -> 0x0000FF
        auto j = nlohmann::json::parse(R"({"ARGB_COLOR": 4278190335})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.color_rgb == 0x0000FFu);
    }

    SECTION("stores both MANUFACTURER and VENDOR independently") {
        auto j = nlohmann::json::parse(R"({
            "VENDOR": "Generic",
            "MANUFACTURER": "",
            "MAIN_TYPE": "PETG"
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        // Parser stores fields as-is; brand fallback logic is in handle_status_update
        REQUIRE(info.vendor == "Generic");
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.main_type == "PETG");
    }

    SECTION("handles missing RFID fields with safe defaults") {
        auto j = nlohmann::json::parse("{}");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type.empty());
        REQUIRE(info.sub_type.empty());
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.vendor.empty());
        REQUIRE(info.hotend_min_temp == 0);
        REQUIRE(info.hotend_max_temp == 0);
        REQUIRE(info.bed_temp == 0);
        REQUIRE(info.weight_g == 0);
        // Default color is 0x808080 (grey)
        REQUIRE(info.color_rgb == 0x808080u);
    }

    SECTION("parses PETG with different temperatures") {
        auto j = nlohmann::json::parse(R"({
            "MANUFACTURER": "Generic3D",
            "MAIN_TYPE": "PETG",
            "SUB_TYPE": "Basic",
            "HOTEND_MIN_TEMP": 220,
            "HOTEND_MAX_TEMP": 250,
            "BED_TEMP": 80,
            "WEIGHT": 1000
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PETG");
        REQUIRE(info.sub_type == "Basic");
        REQUIRE(info.manufacturer == "Generic3D");
        REQUIRE(info.hotend_min_temp == 220);
        REQUIRE(info.hotend_max_temp == 250);
        REQUIRE(info.bed_temp == 80);
        REQUIRE(info.weight_g == 1000);
    }

    SECTION("parses CARD_UID array as comma-joined string") {
        auto j = json::parse(R"({"CARD_UID": [144, 32, 196, 2]})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.uid == "144,32,196,2");
    }

    SECTION("missing CARD_UID leaves uid empty") {
        auto j = json::parse(R"({"MAIN_TYPE": "PLA"})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.uid.empty());
    }
}

// ============================================================================
// Task 12: filament slot override integration
// ============================================================================

TEST_CASE("Snapmaker override loaded at init is applied over firmware data",
          "[ams][snapmaker][filament_slot_override][slow]") {
    // Seed lane_data in the mock Moonraker DB with a slot 0 override.
    // Inject the pre-loaded override into the backend directly (skipping
    // on_started() since the backend is built with api=nullptr for simplicity —
    // on_started's load_blocking path is covered by store tests elsewhere).
    // Then push a firmware status update whose values differ from the
    // override and verify the override wins for override-eligible fields
    // while firmware wins for hardware-truth fields (color is present on
    // both, but the override's color_rgb is non-zero and wins per policy).
    SnapmakerTmpCacheDir tmp("task12_override_applied");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Firmware pushes a different color (blue) and a different material.
    // 0xFF0000FF = opaque red in ARGB.
    json status = make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand",
                                              json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, status);

    auto info = backend.get_slot_info(0);
    // Override-eligible fields won.
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);
}

TEST_CASE("Snapmaker set_slot_info(persist=true) writes override and survives status update",
          "[ams][snapmaker][filament_slot_override][slow]") {
    // This is the core behavior that was BROKEN before Task 12: set_slot_info
    // ignored its persist parameter and the next firmware status update
    // wiped user edits. The override must now survive subsequent parses.
    SnapmakerTmpCacheDir tmp("task12_persist_survives");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // Override is staged in-memory AND written to the Moonraker DB.
    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Simulate a subsequent Klipper status update with conflicting firmware
    // data. Pre-Task-12 this wiped the user's edit; the fix is that
    // apply_overrides re-layers the saved override over the parse output.
    json status = make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand",
                                              json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, status);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");              // survived
    CHECK(info.spool_name == "PolyLite PLA Orange"); // survived
    CHECK(info.spoolman_id == 42);                 // survived
    CHECK(info.material == "PLA");                 // override material wins
    CHECK(info.color_rgb == 0xFF5500u);            // override color wins
}

TEST_CASE("Snapmaker set_slot_info(persist=false) preview does NOT write store",
          "[ams][snapmaker][filament_slot_override][slow]") {
    SnapmakerTmpCacheDir tmp("task12_no_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // No override staged, no DB write.
    CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // Preview edit is still visible via get_slot_info (in-memory only).
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE("Snapmaker RFID UID change clears override (hardware swap detected)",
          "[ams][snapmaker][filament_slot_override][slow]") {
    SnapmakerTmpCacheDir tmp("task12_uid_swap_clears");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Seed an override AND the corresponding DB entry so we can verify
    // clear_async deletes it on swap.
    api.mock_set_db_value("lane_data", "lane1",
                          json{{"vendor", "Polymaker"},
                               {"spool_id", 42},
                               {"material", "PLA"},
                               {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: CARD_UID=[1,2,3,4] establishes the baseline. No clear.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker",
                                    json::array({1, 2, 3, 4})));

    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: DIFFERENT CARD_UID — physical swap detected. Override
    // must be cleared in-memory AND the Moonraker DB entry deleted.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic",
                                    json::array({5, 6, 7, 8})));

    CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
    // Baseline advanced to the new UID.
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "5,6,7,8");

    // Override-exclusive fields reset on the live slot. spool_name is NOT
    // override-exclusive for Snapmaker — RFID's SUB_TYPE populates it (the
    // helper hard-codes "Basic"), and the clear preserves firmware writes
    // the same way it preserves brand / total_weight_g.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Generic");      // firmware's new brand flows through
    CHECK(info.spool_name == "Basic");   // firmware's SUB_TYPE flows through
    CHECK(info.spoolman_id == 0);
    CHECK(info.material == "PETG");      // firmware's new material
}

TEST_CASE("Snapmaker first RFID UID observation does NOT clear override",
          "[ams][snapmaker][filament_slot_override][slow]") {
    // Even when the override was saved against a different (now-stale) UID,
    // the very first observation is a BASELINE and must never fire a clear.
    SnapmakerTmpCacheDir tmp("task12_first_uid_baseline");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1",
                          json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a UID on the FIRST observation — no prior baseline,
    // so this must NOT trigger a clear. Override survives.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                    json::array({99, 99, 99, 99})));

    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // A second parse of the SAME UID stays the baseline — no clear, no
    // "weird state" that fires on unchanged polls.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                    json::array({99, 99, 99, 99})));

    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("Snapmaker empty RFID UID does not update baseline or clear",
          "[ams][snapmaker][filament_slot_override][slow]") {
    // Empty UID = no tag / reader disabled / unreadable. Must not update
    // the baseline and must not clear. This is the contract that keeps
    // transient tag-read failures from masking a genuine hardware swap
    // on the next good read.
    SnapmakerTmpCacheDir tmp("task12_empty_uid_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1",
                          json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid UID "1,2,3,4" — baseline established.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker",
                                    json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // Second parse: EMPTY UID (no CARD_UID field). Must NOT update baseline
    // and must NOT clear the override.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker",
                                    json::array()));
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4"); // unchanged
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Third parse: same original UID "1,2,3,4" — still matches baseline,
    // no clear. Proves the empty-UID pass didn't corrupt state.
    SnapmakerTestAccess::handle_status(backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker",
                                    json::array({1, 2, 3, 4})));
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

// ============================================================================
// Firmware writeback (paxx12 Extended Firmware POST /printer/filament_detect/set)
// ============================================================================

TEST_CASE("Snapmaker set_slot_info(persist=true) POSTs to /printer/filament_detect/set",
          "[ams][snapmaker][firmware_writeback]") {
    SnapmakerTmpCacheDir tmp("firmware_writeback_post");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "SnapSpeed"; // known SUB_TYPE — must round-trip
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    edit.nozzle_temp_min = 195;
    edit.nozzle_temp_max = 225;
    edit.bed_temp = 60;

    auto err = backend.set_slot_info(2, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    CHECK(history[0].endpoint == "/printer/filament_detect/set");

    const auto& body = history[0].body;
    REQUIRE(body.is_object());
    CHECK(body["channel"].get<int>() == 2);
    REQUIRE(body.contains("info"));
    const auto& info_obj = body["info"];
    CHECK(info_obj["VENDOR"].get<std::string>() == "Polymaker");
    CHECK(info_obj["MAIN_TYPE"].get<std::string>() == "PLA");
    CHECK(info_obj["SUB_TYPE"].get<std::string>() == "SnapSpeed");
    CHECK(info_obj["RGB_1"].get<uint32_t>() == 0xFF5500u);
    CHECK(info_obj["ALPHA"].get<int>() == 255);
    CHECK(info_obj["HOTEND_MIN_TEMP"].get<int>() == 195);
    CHECK(info_obj["HOTEND_MAX_TEMP"].get<int>() == 225);
    CHECK(info_obj["BED_TEMP"].get<int>() == 60);
    // CARD_UID and SKU intentionally omitted — let firmware preserve them.
    CHECK_FALSE(info_obj.contains("CARD_UID"));
    CHECK_FALSE(info_obj.contains("SKU"));
}

TEST_CASE("Snapmaker firmware POST omits unknown SUB_TYPE strings",
          "[ams][snapmaker][firmware_writeback]") {
    // spool_name is a free-form user field. Only round-trip to firmware when
    // it matches one of the known Snapmaker product lines; otherwise omit it
    // and let firmware keep whatever it had. The free-form string still
    // lives in lane_data via the override store.
    SnapmakerTmpCacheDir tmp("firmware_writeback_unknown_subtype");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Generic";
    edit.spool_name = "My Custom Roll"; // NOT a known SUB_TYPE
    edit.material = "PETG";
    edit.color_rgb = 0x00FF00;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    const auto& info_obj = history[0].body["info"];
    CHECK(info_obj["VENDOR"].get<std::string>() == "Generic");
    CHECK(info_obj["MAIN_TYPE"].get<std::string>() == "PETG");
    CHECK_FALSE(info_obj.contains("SUB_TYPE"));
}

TEST_CASE("Snapmaker firmware POST omits zero temperatures",
          "[ams][snapmaker][firmware_writeback]") {
    SnapmakerTmpCacheDir tmp("firmware_writeback_zero_temps");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    // All temps left at default 0 — must be omitted so firmware keeps prior values.

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    const auto& info_obj = history[0].body["info"];
    CHECK_FALSE(info_obj.contains("HOTEND_MIN_TEMP"));
    CHECK_FALSE(info_obj.contains("HOTEND_MAX_TEMP"));
    CHECK_FALSE(info_obj.contains("BED_TEMP"));
}

TEST_CASE("Snapmaker set_slot_info(persist=false) does NOT POST to firmware",
          "[ams][snapmaker][firmware_writeback]") {
    // Preview edits (persist=false) are in-memory only and must not write to
    // firmware OR the override store. Mirrors the existing "no DB write" test
    // for the override-store path.
    SnapmakerTmpCacheDir tmp("firmware_writeback_no_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    CHECK(api.rest_mock().mock_get_post_history().empty());
}

TEST_CASE("Snapmaker firmware POST 404 on stock firmware does not fail set_slot_info",
          "[ams][snapmaker][firmware_writeback]") {
    // Stock firmware (no Extended Firmware extension) returns 404 for the
    // endpoint. The override is still persisted to lane_data, so the user's
    // edit isn't lost — set_slot_info must report success regardless.
    SnapmakerTmpCacheDir tmp("firmware_writeback_404");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();
    RestResponse not_found;
    not_found.success = false;
    not_found.status_code = 404;
    not_found.error = "Not Found";
    api.rest_mock().mock_queue_post_response("/printer/filament_detect/set", not_found);

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // POST was attempted...
    CHECK(api.rest_mock().mock_get_post_history().size() == 1);
    // ...override still made it to lane_data so the UI's view is preserved.
    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("Snapmaker auto-mirror uses OverwriteAlways policy after firmware writeback",
          "[ams][snapmaker][firmware_writeback]") {
    // With the firmware-writeback path live, user edits round-trip to firmware
    // (paxx12 endpoint) and firmware-truth converges with user-truth on the
    // very next status update. The auto-mirror tail in handle_status_update
    // therefore overwrites lane_data unconditionally — picking up external
    // edits (CHANGE_ZCOLOR from a print, slicer, etc) AND keeping user edits
    // in sync. This test exercises that overwrite by seeding lane_data with
    // a stale color, then firing a status update with a different firmware
    // color, and verifying lane_data was overwritten.
    SnapmakerTmpCacheDir tmp("firmware_writeback_overwrite");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "snapmaker");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Pre-seed lane_data AND the in-memory override map with a stale color
    // (e.g. set externally before this backend started). With FillUnsetOnly
    // the stale color would survive; with OverwriteAlways the next firmware
    // status overwrites it. We seed the in-memory override too because the
    // mirror helper compares against `overrides_[slot]` (creating a default
    // empty entry otherwise) and writes only when the value differs.
    helix::ams::FilamentSlotOverride stale;
    stale.color_rgb = 0xABCDEF;
    stale.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, stale);
    api.mock_set_db_value("lane_data", "lane1",
                          json{{"color", "#ABCDEF"}, {"material", "PLA"}});

    // Build a status update that ALSO sets filament_detect.state[0]=1 so the
    // slot resolves to AVAILABLE — the mirror helper short-circuits when the
    // slot has no filament loaded ("no signal" contract). Stack the existing
    // info builder with an explicit state array.
    json status = make_filament_detect_status(0, "PETG", 0xFF112233u, "Polymaker",
                                              json::array({1, 2, 3, 4}));
    status["filament_detect"]["state"] = json::array({1, 0, 0, 0});
    SnapmakerTestAccess::handle_status(backend, status);

    auto lane = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!lane.is_null());
    REQUIRE(lane.contains("color"));
    // Firmware-truth color (0x112233) wins over stale lane_data (#ABCDEF).
    auto color_str = lane["color"].get<std::string>();
    // Color is stored as "#RRGGBB"; case may vary, so compare lowercase.
    std::string lower = color_str;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    CHECK(lower == "#112233");
}

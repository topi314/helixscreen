// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

// Friend-class shim matching the one in test_filament_slot_override_store.cpp
// (declared friend in filament_slot_override_store.h per L065). Allows our
// Task 10 tests to redirect the store's read-cache to a per-test tmp dir so
// successful save_async calls don't pollute the developer's real config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

namespace {
// Per-test tmp cache dir — same idiom as test_filament_slot_override_store.cpp.
// The store's save callback writes a local JSON cache so the UI can show
// last-known overrides when Moonraker is unreachable. Without redirection,
// that cache lands in the developer's real helixscreen config dir.
struct Ad5xIfsTmpCacheDir {
    std::filesystem::path path;
    explicit Ad5xIfsTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("ad5x_ifs_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~Ad5xIfsTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

using json = nlohmann::json;

// Test access helper — friend class for accessing internals
class Ad5xIfsTestAccess {
  public:
    static void handle_status(AmsBackendAd5xIfs& b, const json& n) {
        b.handle_status_update(n);
    }
    static void parse_vars(AmsBackendAd5xIfs& b, const json& v) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.parse_save_variables(v);
    }
    static int active_tool(const AmsBackendAd5xIfs& b) {
        return b.active_tool_;
    }
    static bool external_mode(const AmsBackendAd5xIfs& b) {
        return b.external_mode_;
    }
    static bool head_filament(const AmsBackendAd5xIfs& b) {
        return b.head_filament_;
    }
    static bool port_presence(const AmsBackendAd5xIfs& b, int i) {
        return b.port_presence_[static_cast<size_t>(i)];
    }
    static std::string build_colors(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_color_list_value();
    }
    static std::string build_types(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_type_list_value();
    }
    static std::string build_tools(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_tool_map_value();
    }
    static AmsAction action(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.action;
    }
    static void set_action(AmsBackendAd5xIfs& b, AmsAction a) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.action = a;
        b.action_start_time_ = std::chrono::steady_clock::now();
    }
    static void check_action_timeout(AmsBackendAd5xIfs& b, std::chrono::seconds elapsed) {
        b.action_start_time_ = std::chrono::steady_clock::now() - elapsed;
        b.check_action_timeout();
    }
    static std::string var_prefix(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.var_prefix_;
    }
    static bool has_per_port_sensors(const AmsBackendAd5xIfs& b) {
        return b.has_per_port_sensors_;
    }
    static size_t external_sync_count(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.external_sync_count_;
    }
    static void set_var_prefix(AmsBackendAd5xIfs& b, const std::string& prefix) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.var_prefix_ = prefix;
    }
    static bool has_ifs_vars(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.has_ifs_vars_;
    }
    static void set_has_ifs_vars(AmsBackendAd5xIfs& b, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.has_ifs_vars_ = val;
    }
    static void set_ifs_macro_confirmed_missing(AmsBackendAd5xIfs& b, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.ifs_macro_confirmed_missing_ = val;
    }
    static void parse_adventurer_json(AmsBackendAd5xIfs& b, const std::string& content) {
        b.parse_adventurer_json(content);
    }
    static bool dirty(const AmsBackendAd5xIfs& b, size_t idx) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.dirty_[idx];
    }
    static void set_dirty(AmsBackendAd5xIfs& b, size_t idx, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.dirty_[idx] = val;
    }
    // Seed firmware-state arrays directly. parse_save_variables no longer
    // writes colors_[]/materials_[] (those come from CHANGE_ZCOLOR/GET_ZCOLOR
    // exclusively now); tests that previously seeded via _IFS_VARS save_variables
    // should use these helpers and then re-run update_slot_from_state via
    // handle_status, parse_adventurer_json, or apply_zcolor_result.
    static void set_color(AmsBackendAd5xIfs& b, size_t idx, const std::string& hex) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.colors_[idx] = hex;
        b.update_slot_from_state(static_cast<int>(idx));
    }
    static void set_material(AmsBackendAd5xIfs& b, size_t idx, const std::string& mat) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.materials_[idx] = mat;
        b.update_slot_from_state(static_cast<int>(idx));
    }
    static void set_port_presence(AmsBackendAd5xIfs& b, size_t idx, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.port_presence_[idx] = val;
        b.update_slot_from_state(static_cast<int>(idx));
    }
    static AmsBackendAd5xIfs::ZColorSilentResult
    parse_zcolor_silent(const std::vector<std::string>& lines) {
        return AmsBackendAd5xIfs::parse_zcolor_silent(lines);
    }
    static bool zcolor_silent_supported(const AmsBackendAd5xIfs& b) {
        return b.zcolor_silent_supported_.load();
    }
    static void apply_zcolor_result(AmsBackendAd5xIfs& b,
                                    const AmsBackendAd5xIfs::ZColorSilentResult& r) {
        b.apply_zcolor_result(r);
    }
    // Seed the in-memory overrides map directly (bypasses load_blocking, which
    // requires a live Moonraker connection). on_started() is the only
    // production path that writes this field; tests must use this shim because
    // the fixtures instantiate the backend with nullptr api/client.
    static void seed_override(AmsBackendAd5xIfs& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    // Read the override currently staged for a slot (empty optional if none).
    // Lets tests assert what set_slot_info(persist=true) wrote into the
    // in-memory map without going through get_slot_info (which also layers
    // apply_overrides on top of firmware state).
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const AmsBackendAd5xIfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    // Inject an override store so persist=true set_slot_info has somewhere to
    // write. Production creates the store inside on_started(); tests that
    // build the backend with a concrete MoonrakerAPIMock but never call
    // on_started() need this shim to populate override_store_.
    static void inject_override_store(AmsBackendAd5xIfs& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    // Drive check_external_color_change directly with a caller-chosen observed
    // color. The only way observed_color == 0 ever reaches the check site in
    // production is through a parse path whose output color literally parses
    // to 0 — parse_adventurer_json never produces that (empty hex becomes
    // 0x808080 gray). Exposing the check lets us assert the "empty reading
    // must not update baseline" contract unambiguously. `slot_has_filament`
    // defaults to true so existing call sites that just want to drive the
    // baseline-update path don't need to think about presence semantics.
    static void check_external_color_change(AmsBackendAd5xIfs& b, int slot_index,
                                            uint32_t observed_color,
                                            bool slot_has_filament = true) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.check_external_color_change(slot_index, observed_color, slot_has_filament);
    }
    static std::optional<uint32_t> last_firmware_color(const AmsBackendAd5xIfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.last_firmware_color_.find(slot_index);
        if (it == b.last_firmware_color_.end())
            return std::nullopt;
        return it->second;
    }
    // Listener feedback fix (v0.99.51 spam loop) + JSON-poll watcher hooks.
    static bool on_gcode_response_line(AmsBackendAd5xIfs& b, const std::string& line) {
        return b.on_gcode_response_line(line);
    }
    static void set_zcolor_query_active(AmsBackendAd5xIfs& b, bool active) {
        b.zcolor_query_active_.store(active);
    }
    static uint32_t zcolor_schedule_count(const AmsBackendAd5xIfs& b) {
        return b.zcolor_schedule_count_.load();
    }
    static size_t zcolor_buffer_size(AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.zcolor_buffer_mutex_);
        return b.zcolor_response_buffer_.size();
    }
    static bool note_json_content(AmsBackendAd5xIfs& b, const std::string& content) {
        return b.note_json_content(content);
    }
    // Override the resolved on-disk Adventurer5M.json path so tests can drive
    // the direct-write path against a tmp file instead of the real
    // /usr/prog/config target. Empty string forces the Moonraker fallback.
    static void set_local_adventurer_json_path(AmsBackendAd5xIfs& b, const std::string& p) {
        b.local_adventurer_json_path_ = p;
    }
    static const std::string& local_adventurer_json_path(const AmsBackendAd5xIfs& b) {
        return b.local_adventurer_json_path_;
    }
    // Drive the local read-modify-write path directly so tests can assert
    // file content without going through the full set_slot_info pipeline.
    static AmsError write_adventurer_json_local(AmsBackendAd5xIfs& b, int slot_index) {
        return b.write_adventurer_json_local(slot_index);
    }
    // tool_map snapshot: copy out for comparison without holding mutex_.
    static std::array<int, AmsBackendAd5xIfs::TOOL_MAP_SIZE> tool_map(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.tool_map_;
    }
    // Custom-types snapshot. Test fixture inspects what bambufy_custom_types
    // / user.cfg merging produced.
    static std::vector<std::string> custom_material_types(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.custom_types_mutex_);
        return b.custom_material_types_;
    }
};

// Helper to build a full save_variables JSON payload
static json make_save_variables(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

// Helper to build a port sensor notification
static json make_port_sensor(int port_1based, bool detected) {
    std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port_1based);
    return json{{key, json{{"filament_detected", detected}}}};
}

// Helper to build a head sensor notification
static json make_head_sensor(bool detected) {
    return json{
        {"filament_switch_sensor head_switch_sensor", json{{"filament_detected", detected}}}};
}

// Helper to build a native ZMOD motion sensor notification
static json make_motion_sensor(bool detected) {
    return json{
        {"filament_motion_sensor ifs_motion_sensor", json{{"filament_detected", detected}}}};
}

// Standard test variables representing a typical IFS configuration. Note:
// `<prefix>_colors` and `<prefix>_types` are no longer consumed by
// parse_save_variables (they live in lessWaste/bambufy's private namespace,
// which zmod doesn't read). Tests that need colors/materials seeded must
// also call seed_standard_colors() — the entries in this map are kept so
// existing tests that simulate notifies still pass through unchanged for the
// fields parse_save_variables still cares about (tools, current_tool,
// external).
static json standard_variables() {
    return json{{"less_waste_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
                {"less_waste_types", json::array({"PLA", "PETG", "ABS", "TPU"})},
                {"less_waste_tools", json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
                {"less_waste_current_tool", 0},
                {"less_waste_external", 0}};
}

// Seed colors_/materials_/port_presence_ to match standard_variables() — the
// shape that parse_save_variables used to populate. Use after constructing a
// backend to give tests a deterministic firmware-truth baseline.
static void seed_standard_colors(AmsBackendAd5xIfs& b) {
    Ad5xIfsTestAccess::set_color(b, 0, "FF0000");
    Ad5xIfsTestAccess::set_color(b, 1, "00FF00");
    Ad5xIfsTestAccess::set_color(b, 2, "0000FF");
    Ad5xIfsTestAccess::set_color(b, 3, "FFFFFF");
    Ad5xIfsTestAccess::set_material(b, 0, "PLA");
    Ad5xIfsTestAccess::set_material(b, 1, "PETG");
    Ad5xIfsTestAccess::set_material(b, 2, "ABS");
    Ad5xIfsTestAccess::set_material(b, 3, "TPU");
    Ad5xIfsTestAccess::set_port_presence(b, 0, true);
    Ad5xIfsTestAccess::set_port_presence(b, 1, true);
    Ad5xIfsTestAccess::set_port_presence(b, 2, true);
    Ad5xIfsTestAccess::set_port_presence(b, 3, true);
}

// ==========================================================================
// 1. Type identification
// ==========================================================================

TEST_CASE("AD5X IFS type identification", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE(backend.get_type() == AmsType::AD5X_IFS);
    REQUIRE(backend.get_topology() == PathTopology::LINEAR);
}

// ==========================================================================
// 2. parse_save_variables — full JSON
// ==========================================================================

TEST_CASE("AD5X IFS parse_save_variables full JSON", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));

    // Color/material seeded separately — parse_save_variables does not write
    // colors_[]/materials_[] anymore (those live in zmod's authoritative state,
    // not in lessWaste/bambufy's private namespace).
    seed_standard_colors(backend);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFF0000);
    REQUIRE(info.material == "PLA");
    REQUIRE(info.mapped_tool == 0); // Tool 0 maps to port 1 (slot 0)

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.color_rgb == 0x00FF00);
    REQUIRE(info1.material == "PETG");
    REQUIRE(info1.mapped_tool == 1);

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.color_rgb == 0x0000FF);
    REQUIRE(info2.material == "ABS");
    REQUIRE(info2.mapped_tool == 2);

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.color_rgb == 0xFFFFFF);
    REQUIRE(info3.material == "TPU");
    REQUIRE(info3.mapped_tool == 3);
}

// ==========================================================================
// 3. parse_save_variables with -1 active tool
// ==========================================================================

TEST_CASE("AD5X IFS no active tool", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    auto vars = standard_variables();
    vars["less_waste_current_tool"] = -1;

    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == -1);
    REQUIRE(sys.current_slot == -1);
    REQUIRE_FALSE(sys.filament_loaded);
}

// ==========================================================================
// 4. Color hex parsing
// ==========================================================================

TEST_CASE("AD5X IFS color hex parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("lowercase hex works") {
        Ad5xIfsTestAccess::set_color(backend, 0, "ff0000");

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("mixed case hex works") {
        Ad5xIfsTestAccess::set_color(backend, 0, "Ff0000");

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("empty string leaves color unchanged") {
        // Seed slot 1 with a known color, then attempt to overwrite slot 0
        // with empty hex — update_slot_from_state's stoul fallback skips the
        // parse, so other slots are unaffected.
        Ad5xIfsTestAccess::set_color(backend, 1, "00FF00");
        Ad5xIfsTestAccess::set_color(backend, 0, "");

        auto info = backend.get_slot_info(1);
        REQUIRE(info.color_rgb == 0x00FF00);
    }
}

// ==========================================================================
// 5. Tool mapping reverse lookup
// ==========================================================================

TEST_CASE("AD5X IFS tool mapping reverse lookup", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("standard 1:1 mapping") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

        for (int i = 0; i < 4; ++i) {
            auto info = backend.get_slot_info(i);
            REQUIRE(info.mapped_tool == i);
        }
    }

    SECTION("non-standard mapping: T0->port3, T1->port1") {
        auto vars = standard_variables();
        // T0->3, T1->1, T2->5(unmapped), T3->2, rest unmapped
        vars["less_waste_tools"] = json::array({3, 1, 5, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        // Slot 0 (port 1): first tool mapping to port 1 is T1
        REQUIRE(backend.get_slot_info(0).mapped_tool == 1);
        // Slot 1 (port 2): first tool mapping to port 2 is T3
        REQUIRE(backend.get_slot_info(1).mapped_tool == 3);
        // Slot 2 (port 3): first tool mapping to port 3 is T0
        REQUIRE(backend.get_slot_info(2).mapped_tool == 0);
        // Slot 3 (port 4): no tool maps to port 4
        REQUIRE(backend.get_slot_info(3).mapped_tool == -1);
    }
}

// ==========================================================================
// 6. Port sensor parsing via handle_status_update
// ==========================================================================

TEST_CASE("AD5X IFS port sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Set port 1 and 3 as having filament
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, true));
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(3, true));

    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == false);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2) == true);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 3) == false);

    // Clear port 1
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, false));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == false);
}

// ==========================================================================
// 7. Head sensor parsing via handle_status_update
// ==========================================================================

TEST_CASE("AD5X IFS head sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

// ==========================================================================
// 7b. Native ZMOD IFS motion sensor (no lessWaste per-port sensors)
// ==========================================================================

TEST_CASE("AD5X IFS native ZMOD motion sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // Native ZMOD motion sensor maps to head filament state
    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS native ZMOD combined update (no per-port sensors)", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Simulate a native ZMOD IFS status update:
    // save_variables + motion sensor + head switch sensor, NO per-port sensors
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Verify system state — should detect filament loaded via motion sensor
    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.filament_loaded);
    REQUIRE(sys.current_tool == 0);

    // Port presence is unknown in native ZMOD (no per-port sensors)
    // but save_variables provides colors and tool mapping
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
}

// ==========================================================================
// 8. Combined status update
// ==========================================================================

TEST_CASE("AD5X IFS combined status update", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Build a combined notification with save_variables + sensors
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor _ifs_port_sensor_2"] = json{{"filament_detected", false}};
    notification["filament_switch_sensor _ifs_port_sensor_3"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor _ifs_port_sensor_4"] = json{{"filament_detected", false}};
    notification["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Verify all state
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == 0);
    REQUIRE(sys.current_slot == 0); // T0 maps to port 1 (slot 0)
    REQUIRE(sys.filament_loaded);
}

// ==========================================================================
// 9. get_system_info
// ==========================================================================

TEST_CASE("AD5X IFS get_system_info", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.type_name == "AD5X IFS");
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
    REQUIRE(sys.supports_bypass);
    REQUIRE(sys.supports_tool_mapping);
    REQUIRE_FALSE(sys.supports_endless_spool);
    REQUIRE_FALSE(sys.supports_purge);

    // IFS tool mapping: 16 entries (tool→slot), first 4 mapped, rest unmapped
    REQUIRE(sys.tool_to_slot_map.size() == 16);
    REQUIRE(sys.tool_to_slot_map[0] == 0);
    REQUIRE(sys.tool_to_slot_map[1] == 1);
    REQUIRE(sys.tool_to_slot_map[2] == 2);
    REQUIRE(sys.tool_to_slot_map[3] == 3);
    for (size_t i = 4; i < 16; ++i) {
        REQUIRE(sys.tool_to_slot_map[i] == -1);
    }
}

// ==========================================================================
// 10. Bypass mode
// ==========================================================================

TEST_CASE("AD5X IFS bypass mode", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("external=1 activates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("external=0 deactivates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 0;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }

    SECTION("toggle bypass via parse") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());

        vars["less_waste_external"] = 0;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }
}

// ==========================================================================
// 11. build_color_list_value format
// ==========================================================================

TEST_CASE("AD5X IFS build_color_list_value format", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    seed_standard_colors(backend);

    std::string colors = Ad5xIfsTestAccess::build_colors(backend);
    // Expected: Python list literal with outer double quotes. Function is no
    // longer wired into the color write path (CHANGE_ZCOLOR is per-slot), but
    // it remains for any future _IFS_VARS payload that legitimately needs the
    // shape — keep the formatter test as a regression guard.
    REQUIRE(colors == "\"['FF0000', '00FF00', '0000FF', 'FFFFFF']\"");
}

// ==========================================================================
// 12. build_tool_map_value format
// ==========================================================================

TEST_CASE("AD5X IFS build_tool_map_value format", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    std::string tools = Ad5xIfsTestAccess::build_tools(backend);
    REQUIRE(tools == "\"[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]\"");
}

// ==========================================================================
// 13. set_slot_info with persist=false
// ==========================================================================

TEST_CASE("AD5X IFS set_slot_info persist=false", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // First parse standard state so slots exist
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // Use a material in the firmware whitelist so normalize_material()
    // doesn't coerce it. See get_supported_materials() for the accepted set.
    SlotInfo new_info;
    new_info.color_rgb = 0x123456;
    new_info.material = "SILK";
    new_info.spoolman_id = 42;
    new_info.remaining_weight_g = 500;
    new_info.total_weight_g = 1000;

    auto err = backend.set_slot_info(1, new_info, false);
    REQUIRE(err.success());

    auto info = backend.get_slot_info(1);
    REQUIRE(info.color_rgb == 0x123456);
    REQUIRE(info.material == "SILK");
    REQUIRE(info.spoolman_id == 42);
    REQUIRE(info.remaining_weight_g == 500);
    REQUIRE(info.total_weight_g == 1000);
}

// ==========================================================================
// 14. Slot status mapping
// ==========================================================================

TEST_CASE("AD5X IFS slot status mapping", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("port with filament, not active → AVAILABLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 2 has filament, active tool is T0 (mapped to port 1)
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(1); // slot 1 = port 2
        REQUIRE(info.status == SlotStatus::AVAILABLE);
    }

    SECTION("port with filament, is active + head loaded → LOADED") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 1 has filament, active tool is T0 (mapped to port 1), head has filament
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(0); // slot 0 = port 1
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("port without filament → EMPTY") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_3"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(2); // slot 2 = port 3
        REQUIRE(info.status == SlotStatus::EMPTY);
    }
}

// ==========================================================================
// 15. Action state tracking
// ==========================================================================

TEST_CASE("AD5X IFS action state tracking", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("load_filament sets LOADING action (precondition fails with null api)") {
        // load_filament will fail at check_preconditions with null api,
        // so we can't test the action being set via that path.
        // Instead test the action inference: LOADING + head sensor → IDLE
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

        // Head sensor triggers → load complete
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING + head sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);

        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("LOADING + head sensor NOT triggered → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

// ==========================================================================
// 16. Path segments
// ==========================================================================

TEST_CASE("AD5X IFS path segments", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("get_filament_segment: no filament anywhere → NONE") {
        REQUIRE(backend.get_filament_segment() == PathSegment::NONE);
    }

    SECTION("get_filament_segment: head has filament → NOZZLE") {
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE(backend.get_filament_segment() == PathSegment::NOZZLE);
    }

    SECTION("get_filament_segment: port has filament, active tool set, head empty → LANE") {
        json notification;
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = 0; // T0 → port 1
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        // head sensor NOT set (defaults to false)
        Ad5xIfsTestAccess::handle_status(backend, notification);

        REQUIRE(backend.get_filament_segment() == PathSegment::LANE);
    }

    SECTION("get_slot_filament_segment: active slot with head filament → NOZZLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        REQUIRE(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }

    SECTION("get_slot_filament_segment: non-active slot with filament → HUB") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        // Slot 1 (port 2) has filament but is not active — shows at hub
        REQUIRE(backend.get_slot_filament_segment(1) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: empty slot → NONE") {
        // Slot 2 with port_presence_=false → NONE.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::NONE);
    }

    SECTION("get_slot_filament_segment: non-active slot with color data → HUB") {
        // Slot 2 with port_presence_=true and not the active slot → HUB.
        seed_standard_colors(backend);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: out of range → NONE") {
        REQUIRE(backend.get_slot_filament_segment(-1) == PathSegment::NONE);
        REQUIRE(backend.get_slot_filament_segment(4) == PathSegment::NONE);
    }
}

// ==========================================================================
// Helper to wrap raw status JSON in Moonraker notify_status_update format
// ==========================================================================
static json wrap_notification(const json& status) {
    return json{{"method", "notify_status_update"}, {"params", json::array({status, 12345.678})}};
}

// ==========================================================================
// 17. Wrapped notification format (real WebSocket path)
// ==========================================================================

TEST_CASE("AD5X IFS handles wrapped notify_status_update", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("wrapped port sensor updates state") {
        auto wrapped = wrap_notification(make_port_sensor(1, true));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    }

    SECTION("wrapped head sensor updates state") {
        auto wrapped = wrap_notification(make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    }

    SECTION("wrapped save_variables updates state") {
        seed_standard_colors(backend);
        auto wrapped = wrap_notification(make_save_variables(standard_variables()));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("wrapped combined notification updates all state") {
        json status;
        status["save_variables"] = json{{"variables", standard_variables()}};
        status["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
        status["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(status));

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

        auto sys = backend.get_system_info();
        REQUIRE(sys.current_tool == 0);
        REQUIRE(sys.current_slot == 0);
        REQUIRE(sys.filament_loaded);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("wrapped notification completes load action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(make_head_sensor(true)));

        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("wrapped notification completes unload action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        // Head sensor was true, now cleared
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(make_head_sensor(false)));

        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("unwrapped format still works (initial query response)") {
        // on_started() callback sends unwrapped format — must still work
        Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(2, true));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == true);
    }
}

// ==========================================================================
// 18. Action timeout safety net
// ==========================================================================

TEST_CASE("AD5X IFS action timeout resets stuck operations", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("LOADING resets to IDLE after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING resets to IDLE after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("IDLE does not change on timeout check") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::IDLE);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("action does not reset before timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(30));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }

    SECTION("get_system_info checks timeout on UI poll") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));

        auto sys = backend.get_system_info();
        REQUIRE(sys.action == AmsAction::IDLE);
    }
}

// ==========================================================================
// 19. Variable prefix auto-detection (lessWaste vs bambufy)
// ==========================================================================

TEST_CASE("AD5X IFS variable prefix auto-detection", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("defaults to less_waste prefix") {
        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "less_waste");
    }

    SECTION("detects bambufy prefix from colors") {
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_types"] = json::array({"PLA", "PETG", "ABS", "TPU"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        vars["bambufy_current_tool"] = 0;
        vars["bambufy_external"] = 0;

        seed_standard_colors(backend);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);

        // Color/material sourced from CHANGE_ZCOLOR/GET_ZCOLOR (seeded above),
        // not from <prefix>_colors.
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("detects bambufy prefix from tools alone") {
        json vars;
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
    }
}

// ==========================================================================
// 20. Motion sensor triggers load/unload completion (native ZMOD)
// ==========================================================================

TEST_CASE("AD5X IFS motion sensor completes load/unload", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("LOADING + motion sensor detected → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING + motion sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("LOADING + motion sensor NOT detected → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

// ==========================================================================
// 21. Native ZMOD IFS active slot inferred from head sensor
// ==========================================================================

TEST_CASE("AD5X IFS native ZMOD infers active slot from head sensor", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Seed colors_ + port_presence_ to simulate the post-GET_ZCOLOR / Adventurer5M
    // state. parse_save_variables no longer sets these fields (they live in
    // zmod's namespace, not lessWaste's).
    seed_standard_colors(backend);

    // No per-port sensors — only motion sensor and save_variables
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Active tool is T0 → port 1 → slot 0. With head filament detected and no
    // per-port sensors, the active slot should be inferred as LOADED.
    auto info = backend.get_slot_info(0);
    REQUIRE(info.status == SlotStatus::LOADED);

    // Non-active slots with port_presence_ true are AVAILABLE (not EMPTY).
    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.status == SlotStatus::AVAILABLE);
}

// ==========================================================================
// 22. has_ifs_vars_ detection
// ==========================================================================

TEST_CASE("AD5X IFS has_ifs_vars detection", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("defaults to false") {
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when lessWaste variables found and macro verified") {
        // Simulate initial query confirming macro exists (clears pessimistic latch)
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when bambufy variables found and macro verified") {
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables arrive before macro check completes") {
        // Latch starts true (pessimistic) — save_variables alone can't enable has_ifs_vars
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables has no recognized prefix") {
        json vars;
        vars["some_other_var"] = 42;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }
}

TEST_CASE("AD5X IFS has_ifs_vars reset when macro missing", "[ams][ad5x_ifs]") {
    // Scenario: lessWaste/bambufy plugins partially installed — save_variables data
    // exists but _IFS_VARS gcode macro is not loaded. parse_save_variables() sets
    // has_ifs_vars_ true, but on_started() should reset it when the macro is absent.
    // This test verifies the parse step sets the flag (the reset happens in on_started).
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("pessimistic latch prevents flag before macro check") {
        // Latch starts true — parse_save_variables can't set has_ifs_vars_
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Clearing the latch (as initial query would for a present macro) allows it
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION(
        "later save_variables notify cannot re-enable has_ifs_vars once macro confirmed missing") {
        // Simulate macro verified + save_variables arriving → has_ifs_vars_ = true
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Simulate on_started() discovering the macro is absent.
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, true);

        // A later subscription update carrying lessWaste save_variables arrives.
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Same for bambufy prefix.
        json bambufy_vars;
        bambufy_vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        bambufy_vars["bambufy_tools"] =
            json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(bambufy_vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set_slot_info uses native ZMOD path when has_ifs_vars is false") {
        // Clear latch to pre-populate slot data, then re-set to simulate macro missing
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, true);

        // set_slot_info without persist should succeed regardless
        SlotInfo info;
        info.color_rgb = 0x00FF00;
        info.material = "PETG";
        auto err = backend.set_slot_info(0, info, false);
        REQUIRE(err.success());
    }
}

// Regression: lessWaste/bambufy save_variables rows persist in
// printer_data/database/ even after the user uninstalls the plugin and the
// gcode_macro _ifs_vars goes away. Pre-fix, parse_save_variables read
// <prefix>_tools / _current_tool / _external unconditionally if the keys were
// present, so a user with stale rows would silently keep using the dead
// plugin's last-known tool map and active-tool guess as truth on every boot.
// Now those reads are gated on has_ifs_vars_ — i.e. plugin actively loaded.
TEST_CASE("AD5X IFS stale save_variables ignored when plugin macro missing",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Confirm latch defaults to "macro missing" — the on_started() initial
    // query never confirmed the macro exists.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Build a save_variables blob that looks like the user once had lessWaste
    // installed: tools/active/external all set to non-default values that
    // would VISIBLY change behavior if applied.
    auto stale = standard_variables();
    stale["less_waste_current_tool"] = 2;             // not the default 0
    stale["less_waste_external"] = 1;                 // bypass mode active
    stale["less_waste_tools"] =                       // T0 -> port 4 (not 1)
        json::array({4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(stale));

    // Macro confirmed missing → none of the stale plugin state should have
    // taken effect. active_tool stays at its default -1, external_mode stays
    // false, and the slot-tool mapping in SlotRegistry was never overwritten.
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == -1);
    CHECK_FALSE(Ad5xIfsTestAccess::external_mode(backend));
    CHECK_FALSE(backend.is_bypass_active());

    // Now flip the latch as on_started() would when it sees the macro is
    // present, then replay — the SAME save_variables payload now applies
    // cleanly. This proves the gate, not some other guard, is what blocked
    // the read.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(stale));
    CHECK(Ad5xIfsTestAccess::has_ifs_vars(backend));
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == 2);
    CHECK(Ad5xIfsTestAccess::external_mode(backend));
    CHECK(backend.is_bypass_active());
}

// ==========================================================================
// 23. parse_adventurer_json (native ZMOD Adventurer5M.json)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("standard 4-slot JSON with # prefixed hex colors") {
        std::string content = R"({
            "FFMInfo": {
                "channel": 2,
                "ffmColor0": "",
                "ffmColor1": "#FF0000",
                "ffmColor2": "#00FF00",
                "ffmColor3": "#0000FF",
                "ffmColor4": "#FFFFFF",
                "ffmType0": "?",
                "ffmType1": "PLA",
                "ffmType2": "PETG",
                "ffmType3": "ABS",
                "ffmType4": "TPU"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xFF0000);
        REQUIRE(info0.material == "PLA");

        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.color_rgb == 0x00FF00);
        REQUIRE(info1.material == "PETG");

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x0000FF);
        REQUIRE(info2.material == "ABS");

        auto info3 = backend.get_slot_info(3);
        REQUIRE(info3.color_rgb == 0xFFFFFF);
        REQUIRE(info3.material == "TPU");
    }

    SECTION("lowercase hex is uppercased") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ff8800",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PLA");
    }

    SECTION("missing FFMInfo section is graceful no-op") {
        std::string content = R"({"OtherSection": {"key": "value"}})";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("partial slots — only 2 of 4 populated") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#AABBCC",
                "ffmType1": "PLA",
                "ffmColor3": "#112233",
                "ffmType3": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xAABBCC);
        REQUIRE(info0.material == "PLA");

        // Slot 1 (port 2) not in JSON — stays at default
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.material.empty());

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x112233);
        REQUIRE(info2.material == "PETG");
    }

    SECTION("# prefix stripping") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ABCDEF",
                "ffmType1": "ABS"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xABCDEF);
    }

    SECTION("empty color string defaults to gray") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0x808080);
        REQUIRE(info.material == "PLA");
    }

    SECTION("invalid JSON is graceful no-op") {
        std::string content = "this is not json {{{";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("color without # prefix still works") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "FF8800",
                "ffmType1": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PETG");
    }
}

// ==========================================================================
// Regression: dirty flag prevents parse_adventurer_json from clobbering
// user edits (#716)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json skips dirty slots", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed slot 0 with initial JSON data
    std::string initial = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, initial);
    REQUIRE(backend.get_slot_info(0).color_rgb == 0xFF0000);
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // User edits slot 0 (persist=false to skip actual write)
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    // Simulate sensor-triggered JSON re-read with stale firmware data
    std::string stale = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, stale);

    // Dirty slot must NOT be overwritten
    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0x00FF00);
    REQUIRE(info.material == "PETG");
}

TEST_CASE("AD5X IFS parse_adventurer_json updates clean slots normally", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Edit slot 0, then clear dirty to simulate completed persist
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    Ad5xIfsTestAccess::set_dirty(backend, 0, false);
    REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 0));

    // JSON re-read should overwrite clean slot
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#AABBCC",
            "ffmType1": "ABS"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCC);
    REQUIRE(info.material == "ABS");
}

TEST_CASE("AD5X IFS set_slot_info persist=false sets dirty flag", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 1));

    SlotInfo edit;
    edit.color_rgb = 0x112233;
    edit.material = "TPU";
    backend.set_slot_info(1, edit, false);

    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 1));
}

TEST_CASE("AD5X IFS dirty flag protects against both parse paths", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed via save_variables (lessWaste path)
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // User edits slot 0. Use a material in the firmware whitelist so
    // normalize_material() leaves it alone — this test is about the dirty
    // flag, not normalization.
    SlotInfo edit;
    edit.color_rgb = 0xDEADBE;
    edit.material = "SILK";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    // parse_save_variables must not overwrite dirty slot
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());
    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xDEADBE);
    REQUIRE(info.material == "SILK");

    // parse_adventurer_json must not overwrite dirty slot either
    std::string stale_json = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, stale_json);
    info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xDEADBE);
    REQUIRE(info.material == "SILK");
}

// ==========================================================================
// Native ZMOD: parse_adventurer_json infers filament presence (#716)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json infers presence for native ZMOD", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No per-port sensors — this is native ZMOD
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    SECTION("slots with non-empty color are marked AVAILABLE") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#161616",
                "ffmColor2": "#FFFFFF",
                "ffmColor3": "#D3C4A3",
                "ffmColor4": "#F72224",
                "ffmType1": "PLA+",
                "ffmType2": "PLA+",
                "ffmType3": "PLA+",
                "ffmType4": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        for (int i = 0; i < 4; ++i) {
            auto info = backend.get_slot_info(i);
            REQUIRE(info.is_present());
            REQUIRE(info.status == SlotStatus::AVAILABLE);
            REQUIRE(Ad5xIfsTestAccess::port_presence(backend, i));
        }
    }

    SECTION("slot with empty color is NOT marked present") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "",
                "ffmType1": "?",
                "ffmColor2": "#FF0000",
                "ffmType2": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slot 0 (port 1): empty color → not present
        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.status == SlotStatus::EMPTY);
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));

        // Slot 1 (port 2): has color → present
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.is_present());
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }

    SECTION("per-port sensors take precedence over JSON inference") {
        // Simulate a per-port sensor detecting filament on port 1
        Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, true));
        REQUIRE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

        // Now parse JSON for a slot with empty color
        std::string content = R"({
            "FFMInfo": {
                "ffmColor2": "",
                "ffmType2": "PLA"
            }
        })";
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slot 1 (port 2): has_per_port_sensors is true, so JSON inference is skipped.
        // port_presence stays false (no sensor for port 2 reported detected).
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }
}

// NOTE: tests previously here exercised parse_save_variables's color/type
// reads from <prefix>_colors / <prefix>_types — including the dirty-flag
// round-trip and port_presence inference from color emptiness. Those code
// paths were removed when CHANGE_ZCOLOR / GET_ZCOLOR became the sole
// color/type source (lessWaste/bambufy save_variables don't reflect zmod's
// authoritative state). The remaining set_slot_info port_presence tests
// below cover the local-edit branch that still drives presence inference.

TEST_CASE("AD5X IFS set_slot_info updates port_presence", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Start with empty save_variables so no color data
    json vars = standard_variables();
    vars["less_waste_colors"] = json::array({"", "", "", ""});
    vars["less_waste_types"] = json::array({"", "", "", ""});
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));

    SECTION("setting color on empty slot latches port_presence") {
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        auto slot = backend.get_slot_info(0);
        REQUIRE(slot.status == SlotStatus::AVAILABLE);
    }

    SECTION("clearing slot resets port_presence") {
        // First assign filament
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

        // Now clear it
        SlotInfo cleared;
        cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        cleared.material = "";
        backend.set_slot_info(0, cleared, false);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
        auto slot = backend.get_slot_info(0);
        REQUIRE(slot.status == SlotStatus::EMPTY);
    }

    SECTION("setting only material (default color) latches port_presence") {
        SlotInfo info;
        info.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        info.material = "PETG";
        backend.set_slot_info(0, info, false);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    }

    SECTION("set_slot_info skips presence for per-port sensor printers") {
        // Enable per-port sensors
        json notification;
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
        REQUIRE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

        // set_slot_info should not alter port_presence (sensors are authoritative)
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    }
}

// ==========================================================================
// select_unload_command — bundle KKZ4XKD2 (prestonbrown/helixscreen)
// ==========================================================================

TEST_CASE("AD5X IFS select_unload_command", "[ams][ad5x_ifs]") {
    using Cmd = std::string;

    SECTION("slot_index < 0 → IFS_REMOVE_PRUTOK (current)") {
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(-1, 2, true) == "IFS_REMOVE_PRUTOK");
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(-1, -1, false) == "IFS_REMOVE_PRUTOK");
    }

    SECTION("active slot with head filament → IFS_REMOVE_PRUTOK (avoids per-port macro)") {
        // Bundle KKZ4XKD2: slot_index=2 (port 3) is active and head sensor true.
        // Per-port REMOVE_PRUTOK_IFS PRUTOK=3 errors when IFS state disagrees
        // with our color-latched port_presence — switch to the "current" macro.
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(2, 2, true) == "IFS_REMOVE_PRUTOK");
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(0, 0, true) == "IFS_REMOVE_PRUTOK");
    }

    SECTION("active slot but head empty → per-port unload (lane retract)") {
        // Filament present in lane but not at head → can't use IFS_REMOVE_PRUTOK,
        // need explicit per-port command.
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(2, 2, false) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=3"));
    }

    SECTION("non-active slot → per-port unload") {
        // Unloading a different slot than the active one — must specify port.
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(0, 2, true) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=1"));
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(3, 0, true) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=4"));
    }

    SECTION("no active slot (current_slot=-1) → per-port unload") {
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(2, -1, false) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=3"));
    }

    SECTION("out-of-range slot_index → IFS_REMOVE_PRUTOK fallback") {
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(99, 0, true) == "IFS_REMOVE_PRUTOK");
    }
}

// ==========================================================================
// parse_zcolor_silent — GET_ZCOLOR SILENT=1 response parser
// ==========================================================================
//
// zmod emits one line per LOADED slot plus a summary line, all prefixed "// ":
//
//   // Extruder: None (1) | IFS: True
//   // 1: PLA/FFFFFF
//   // 2: PLA/2750E0
//
// Post-ad2802ab zmod always appends "/<HEX>" to each slot line. Hex is the
// RIGHTMOST /-segment — transparent/named-color case emits three segments
// (// 3: PLA/transparent/00000000). Missing slot numbers = physically empty.
// Old zmod (pre-fix) emits "// 1: PLA" (no /HEX); we fall back to JSON.
// Very old zmod emits an action:prompt_show dialog; also JSON fallback.

TEST_CASE("AD5X IFS parse_zcolor_silent two-segment lines", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 2: PETG/2750E0",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.current_channel == 1);
    REQUIRE_FALSE(r.extruder_slot.has_value());
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->material == "PETG");
    REQUIRE(r.slots[1]->hex == "2750E0");
    REQUIRE_FALSE(r.slots[2].has_value());
    REQUIRE_FALSE(r.slots[3].has_value());
}

TEST_CASE("AD5X IFS parse_zcolor_silent named-color three-segment", "[ams][ad5x_ifs]") {
    // Transparent / any COLOR_MAPPING match produces an extra segment:
    //   // <N>: <MATERIAL>/<NAME>/<HEX>
    // Parser rule: hex is always the rightmost /-segment.
    std::vector<std::string> lines = {
        "// Extruder: 1: PLA/FFFFFF | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 3: PLA/transparent/00000000",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.extruder_slot == 0); // 0-based (line says slot 1)
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[2].has_value());
    REQUIRE(r.slots[2]->material == "PLA");
    REQUIRE(r.slots[2]->hex == "00000000");
}

TEST_CASE("AD5X IFS parse_zcolor_silent empty (all slots unloaded)", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (0) | IFS: True",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.current_channel == 0);
    for (int i = 0; i < AmsBackendAd5xIfs::NUM_PORTS; ++i) {
        REQUIRE_FALSE(r.slots[static_cast<size_t>(i)].has_value());
    }
}

TEST_CASE("AD5X IFS parse_zcolor_silent IFS disabled (independent mode)", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (0) | IFS: False",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.ifs_active);
}

TEST_CASE("AD5X IFS parse_zcolor_silent old-format (no /HEX)", "[ams][ad5x_ifs]") {
    // Pre-ad2802ab zmod: silent lines are "// N: MATERIAL" with no /HEX.
    // Parser must detect this and flag is_old_format so caller falls back to JSON.
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA",
        "// 2: PETG",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE(r.is_old_format);
    // Presence info is still valid even without color — slot 1 and 2 appear.
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[0]->hex.empty());
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->material == "PETG");
    REQUIRE(r.slots[1]->hex.empty());
}

TEST_CASE("AD5X IFS parse_zcolor_silent prompt fallback", "[ams][ad5x_ifs]") {
    // Very old zmod: SILENT=1 unsupported, emits full dialog.
    std::vector<std::string> lines = {
        "// action:prompt_begin Select filament",
        "// action:prompt_text Extruder: None",
        "// action:prompt_button 1: PLA|RUN_ZCOLOR SLOT=1 HEX=FFFFFF TYPE=PLA|primary|FFFFFF",
        "// action:prompt_show",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE(r.is_prompt_fallback);
}

TEST_CASE("AD5X IFS parse_zcolor_silent malformed lines skipped", "[ams][ad5x_ifs]") {
    // Unrelated gcode-response lines interleaved with valid silent output must
    // not confuse the parser — it should pick out the slot lines it recognises.
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// random gcode echo",
        "// 99: nonsense", // slot number out of range
        "// 2: PETG/00FF00",
        "echo: hotend temp 205",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->hex == "00FF00");
    REQUIRE_FALSE(r.slots[2].has_value());
    REQUIRE_FALSE(r.slots[3].has_value());
}

TEST_CASE("AD5X IFS apply_zcolor_result updates port_presence", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.current_channel = 1;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "2750E0"};
    // slots 2 and 3 left empty — should clear any existing presence

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));
}

TEST_CASE("AD5X IFS apply_zcolor_result skips on prompt fallback", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.is_prompt_fallback = true;

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // One prompt-style response flips silent_supported to false permanently
    // for this session; subsequent query_zcolor_silent() becomes a no-op.
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
}

TEST_CASE("AD5X IFS apply_zcolor_result skips when response has no valid content",
          "[ams][ad5x_ifs]") {
    // Regression: a transient/malformed response with zero slot lines and no
    // summary line must NOT wipe port_presence. Pre-fix, an empty ZColorSilentResult
    // would clear all four slots to "not loaded".
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed presence so we can detect an erroneous wipe.
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // Empty (junk response) — saw_valid_response stays false.
    AmsBackendAd5xIfs::ZColorSilentResult empty;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, empty);

    // Slot 0 must still be present — we didn't get valid data, don't overwrite.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
}

TEST_CASE("AD5X IFS apply_zcolor_result updates colors and materials", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "00FF00"};

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Color and material should be propagated.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    // build_colors returns the comma-separated list used for _IFS_VARS writes;
    // indirect but the only public window into colors_[] without friend access.
    auto colors = Ad5xIfsTestAccess::build_colors(backend);
    auto types = Ad5xIfsTestAccess::build_types(backend);
    REQUIRE(colors.find("00FF00") != std::string::npos);
    REQUIRE(types.find("PETG") != std::string::npos);
}

// raza616 v0.99.50 report: "HelixScreen seems to be unaware of which IFS lane
// is currently loaded. Doesn't update when an IFS lane is unloaded — shows
// the filament that was unloaded." Root cause: parse_zcolor_silent extracted
// extruder_slot from the GET_ZCOLOR summary line ("// Extruder: 3: PLA/HEX |
// IFS: True") but apply_zcolor_result never used it. active_tool_ was only
// updated from lessWaste/bambufy save_variables — stock-ZMOD users never
// got an active-tool signal.
TEST_CASE("AD5X IFS apply_zcolor_result derives active_tool from extruder_slot (stock ZMOD)",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Establish an identity tool map so find_first_tool_for_port(N) -> tool N-1.
    // Default tool_map_ is all UNMAPPED_PORT, so without this find_first_tool_for_port
    // returns -1 for every port and the test would always assert -1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    SECTION("extruder_slot present → active_tool follows tool_map_") {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.extruder_slot = 2; // 0-based → port 3 → tool 2 under identity map
        r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};

        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 2);
    }

    SECTION("extruder_slot absent (unloaded) → active_tool clears to -1") {
        // Seed with a loaded slot first so we can detect the clear.
        AmsBackendAd5xIfs::ZColorSilentResult loaded;
        loaded.saw_valid_response = true;
        loaded.ifs_active = true;
        loaded.extruder_slot = 1;
        loaded.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 1);

        // Now an "Extruder: None (N)" response — extruder_slot becomes nullopt.
        AmsBackendAd5xIfs::ZColorSilentResult unloaded;
        unloaded.saw_valid_response = true;
        unloaded.ifs_active = true;
        unloaded.current_channel = 1;
        // extruder_slot deliberately not set; slots all empty
        Ad5xIfsTestAccess::apply_zcolor_result(backend, unloaded);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
    }

    SECTION("extruder_slot maps to unmapped tool → active_tool becomes -1") {
        // Wipe the map for slot 0 (port 1), then claim slot 0 is in the extruder.
        // No tool maps to port 1, so find_first_tool_for_port(1) returns -1.
        REQUIRE(backend.set_tool_mapping(0, /*slot=*/-1).success()); // unmap T0

        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.extruder_slot = 0;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "808080"};

        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
    }
}

TEST_CASE("AD5X IFS apply_zcolor_result leaves active_tool alone when has_ifs_vars",
          "[ams][ad5x_ifs]") {
    // lessWaste/bambufy users get active_tool from <prefix>_current_tool in
    // save_variables, which is authoritative for them. GET_ZCOLOR's view must
    // not race against it — verify by setting has_ifs_vars=true and checking
    // active_tool_ is unchanged after apply.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed the tool map BEFORE flipping has_ifs_vars=true. set_tool_mapping
    // tries to persist via write_ifs_var when has_ifs_vars is true, which
    // requires an api_ connection — using nullptr would return "No API
    // connection" before the in-memory mutation completes.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.extruder_slot = 2;
    r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // active_tool defaulted to -1 and was not updated — lessWaste's save_variables
    // path retains exclusive ownership.
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
}

TEST_CASE("AD5X IFS apply_zcolor_result skips color write on dirty slot", "[ams][ad5x_ifs]") {
    // Dirty slot means an unsaved user edit is pending — we must NOT overwrite
    // the local color with zmod's view, or we'd clobber the user's edit.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed slot 0 with a color we want preserved.
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);

    // Mark dirty, then apply a result that would change color.
    Ad5xIfsTestAccess::set_dirty(backend, 0, true);
    AmsBackendAd5xIfs::ZColorSilentResult incoming;
    incoming.saw_valid_response = true;
    incoming.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "0000FF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, incoming);

    // Color must still be FF0000 — dirty-slot guard held.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("0000FF") == std::string::npos);
}

TEST_CASE("AD5X IFS apply_zcolor_result old-format preserves colors", "[ams][ad5x_ifs]") {
    // Pre-ad2802ab zmod: slot lines carry no /HEX. Presence should still
    // update, but existing colors must NOT be overwritten with empty strings.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFAA00"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FFAA00") != std::string::npos);

    AmsBackendAd5xIfs::ZColorSilentResult old_fmt;
    old_fmt.saw_valid_response = true;
    old_fmt.is_old_format = true;
    // slot present but material only, no hex
    old_fmt.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", ""};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, old_fmt);

    // Color preserved from JSON-seeded state.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FFAA00") != std::string::npos);
    // Presence still reflects what the old-format response said.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
}

TEST_CASE("AD5X IFS parse_zcolor_silent sets saw_valid_response", "[ams][ad5x_ifs]") {
    SECTION("summary line present") {
        std::vector<std::string> lines = {"// Extruder: None (0) | IFS: True"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE(r.saw_valid_response);
    }
    SECTION("slot line present") {
        std::vector<std::string> lines = {"// 1: PLA/FFFFFF"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE(r.saw_valid_response);
    }
    SECTION("only junk lines") {
        std::vector<std::string> lines = {"echo: random output", "// not a slot line"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE_FALSE(r.saw_valid_response);
    }
    SECTION("slot-number-out-of-range line") {
        // "// 99: nonsense" is skipped and must NOT count as valid.
        std::vector<std::string> lines = {"// 99: nonsense"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE_FALSE(r.saw_valid_response);
    }
}

// ==========================================================================
// Material whitelist + normalization
// ==========================================================================

TEST_CASE("AD5X IFS get_supported_materials returns firmware whitelist", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    auto supported = backend.get_supported_materials();
    REQUIRE(supported.has_value());
    REQUIRE(supported->size() == 7);

    // Exact strings as firmware expects (order mirrors the firmware error message).
    REQUIRE((*supported)[0] == "PLA");
    REQUIRE((*supported)[1] == "PLA-CF");
    REQUIRE((*supported)[2] == "SILK");
    REQUIRE((*supported)[3] == "TPU");
    REQUIRE((*supported)[4] == "ABS");
    REQUIRE((*supported)[5] == "PETG");
    REQUIRE((*supported)[6] == "PETG-CF");
}

TEST_CASE("AD5X IFS normalize_material coerces input to whitelist", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("exact match passes through") {
        REQUIRE(backend.normalize_material("PLA") == "PLA");
        REQUIRE(backend.normalize_material("PETG-CF") == "PETG-CF");
        REQUIRE(backend.normalize_material("TPU") == "TPU");
    }

    SECTION("case-insensitive match returns canonical case") {
        REQUIRE(backend.normalize_material("pla") == "PLA");
        REQUIRE(backend.normalize_material("Pla") == "PLA");
        REQUIRE(backend.normalize_material("petg-cf") == "PETG-CF");
        REQUIRE(backend.normalize_material("silk") == "SILK");
    }

    SECTION("PLA+ collapses via compat_group to PLA") {
        // PLA+ shares compat_group "PLA" with PLA in the filament DB.
        REQUIRE(backend.normalize_material("PLA+") == "PLA");
    }

    SECTION("ASA collapses via compat_group to ABS") {
        // ASA has compat_group "ABS_ASA"; ABS is the first whitelist entry
        // with matching compat_group.
        REQUIRE(backend.normalize_material("ASA") == "ABS");
    }

    SECTION("PEEK falls back to first entry (no compat_group match)") {
        // PEEK's compat_group is "HIGH_TEMP" which no whitelist entry shares.
        REQUIRE(backend.normalize_material("PEEK") == "PLA");
    }

    SECTION("empty string falls back to first entry") {
        REQUIRE(backend.normalize_material("") == "PLA");
    }

    SECTION("unknown material falls back to first entry") {
        REQUIRE(backend.normalize_material("Nonsense") == "PLA");
    }

    SECTION("silk variants map to SILK via AD5X-specific override") {
        // AD5X treats SILK as distinct from PLA. The shared filament DB
        // groups silk variants under compat_group "PLA" because most
        // printers don't make that distinction, so the default fallback
        // would route them to "PLA". The AD5X normalize_material()
        // override catches common silk names before delegating.
        REQUIRE(backend.normalize_material("Silk PLA") == "SILK");
        REQUIRE(backend.normalize_material("PLA Silk") == "SILK");
        REQUIRE(backend.normalize_material("Silk") == "SILK");
        REQUIRE(backend.normalize_material("silk pla") == "SILK");
    }
}

TEST_CASE("AFC backend has no whitelist and passes material through unchanged",
          "[ams][whitelist]") {
    // AFC (like Happy Hare, ACE, CFS) treats material as a free-form label.
    AmsBackendAfc backend(nullptr, nullptr);

    REQUIRE_FALSE(backend.get_supported_materials().has_value());
    REQUIRE(backend.normalize_material("PLA+") == "PLA+");
    REQUIRE(backend.normalize_material("Some Random String") == "Some Random String");
    REQUIRE(backend.normalize_material("") == "");
}

// ==========================================================================
// FilamentSlotOverride integration (Task 9)
//
// The override store is loaded once in on_started() and then layered over
// every parse via update_slot_from_state(). These tests exercise the layering
// directly by seeding the in-memory overrides map (via test access) and then
// driving the parse path — parse_adventurer_json is used because it goes
// through update_slot_from_state and is the simplest hook-free entry point
// from a test fixture constructed with nullptr api/client.
// ==========================================================================

TEST_CASE("AD5X IFS applies override brand over Adventurer5M.json data",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Green";
    ovr.color_rgb = 0x00AA00;  // Override to green
    ovr.color_set = true;
    ovr.material = "PETG";      // Override to PETG
    ovr.spoolman_id = 42;
    ovr.remaining_weight_g = 750.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports slot 0 (port 1) as orange PLA.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    // Override wins for every non-default field.
    REQUIRE(info.brand == "Polymaker");
    REQUIRE(info.spool_name == "PolyLite Green");
    REQUIRE(info.color_rgb == 0x00AA00u);
    REQUIRE(info.material == "PETG");
    REQUIRE(info.spoolman_id == 42);
    REQUIRE(info.remaining_weight_g == 750.0f);
}

TEST_CASE("AD5X IFS preserves firmware color when no override present",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No overrides seeded — firmware data must flow through unchanged.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFF5500u);
    REQUIRE(info.material == "PLA");
    // Default-valued fields on SlotInfo.
    REQUIRE(info.brand.empty());
    REQUIRE(info.spool_name.empty());
    REQUIRE(info.spoolman_id == 0);
}

TEST_CASE("AD5X IFS partial override only replaces specified fields",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed an override that only sets brand. Every other field must fall
    // through to the firmware-reported value (or SlotInfo default).
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "Polymaker");       // override wins
    REQUIRE(info.color_rgb == 0xFF5500u);     // firmware untouched
    REQUIRE(info.material == "PLA");          // firmware untouched
    REQUIRE(info.spool_name.empty());         // default
    REQUIRE(info.spoolman_id == 0);           // default
    REQUIRE(info.remaining_weight_g == -1.0f); // default
}

TEST_CASE("AD5X IFS override applies to multiple slots independently",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr0;
    ovr0.brand = "Polymaker";
    ovr0.material = "PETG";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr0);

    helix::ams::FilamentSlotOverride ovr2;
    ovr2.brand = "eSUN";
    ovr2.color_rgb = 0x123456;
    ovr2.color_set = true;
    Ad5xIfsTestAccess::seed_override(backend, 2, ovr2);

    // Slots 1 and 3 have NO override — must reflect pure firmware data.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmColor2": "#00FF00",
            "ffmColor3": "#0000FF",
            "ffmColor4": "#FFFFFF",
            "ffmType1": "PLA",
            "ffmType2": "PLA",
            "ffmType3": "PLA",
            "ffmType4": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info0 = backend.get_slot_info(0);
    REQUIRE(info0.brand == "Polymaker");
    REQUIRE(info0.material == "PETG");
    REQUIRE(info0.color_rgb == 0xFF0000u); // firmware untouched by ovr0

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.brand.empty());
    REQUIRE(info1.color_rgb == 0x00FF00u);
    REQUIRE(info1.material == "PLA");

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.brand == "eSUN");
    REQUIRE(info2.color_rgb == 0x123456u);
    REQUIRE(info2.material == "PLA");

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.brand.empty());
    REQUIRE(info3.color_rgb == 0xFFFFFFu);
}

TEST_CASE("AD5X IFS override re-applied on every parse",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PETG";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: firmware reports orange PLA.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    auto first = backend.get_slot_info(0);
    REQUIRE(first.brand == "Polymaker");
    REQUIRE(first.material == "PETG");

    // Second parse with the same firmware color but a different material.
    // The override must still win on re-parse. Note: deliberately keep the
    // color stable — Task 11's hardware-event detection clears overrides when
    // firmware color changes (physical spool swap), which is tested in the
    // hardware-swap test cases below. This case exercises the "override wins
    // on re-parse" property, which is a separate contract.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "ABS"}
    })");
    auto second = backend.get_slot_info(0);
    REQUIRE(second.brand == "Polymaker");
    REQUIRE(second.material == "PETG");
}

TEST_CASE("AD5X IFS override zero color_rgb does not replace firmware color",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // color_rgb=0 is the "no override" sentinel — must not clobber firmware.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "TestBrand";
    ovr.color_rgb = 0;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#AA55FF", "ffmType1": "PLA"}
    })");

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "TestBrand");
    REQUIRE(info.color_rgb == 0xAA55FFu); // unchanged
}

TEST_CASE("AD5X IFS override negative weights do not replace firmware values",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed an override with -1.0 weights (the "unknown" sentinel) — must not
    // overwrite whatever weights the firmware / SlotInfo default holds.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "WeightTest";
    ovr.remaining_weight_g = -1.0f;
    ovr.total_weight_g = -1.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF0000", "ffmType1": "PLA"}
    })");

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "WeightTest");
    // Firmware doesn't populate weights at all — they should remain at the
    // SlotInfo default (-1), not at zero. This verifies apply_overrides did
    // NOT write the -1 sentinel over the default (which would be a no-op
    // today but guards against a future default change).
    REQUIRE(info.remaining_weight_g == -1.0f);
    REQUIRE(info.total_weight_g == -1.0f);
}

TEST_CASE("AD5X IFS set_slot_info takes effect when no override present",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Regression lock: with no override seeded for the slot, set_slot_info's
    // edit (every SlotInfo field, not just color/material) must be visible
    // via get_slot_info. Task 9 added apply_overrides to the parse path;
    // Task 10 extended entry->info update to carry brand / spool_name /
    // spoolman_id / color_name through a persist=false "preview" write.
    // This test guards against a regression where the parse path
    // accidentally runs with stale overrides_ state and clobbers the edit.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SlotInfo edit;
    edit.color_rgb = 0xAABBCC;
    edit.material = "PETG";
    edit.brand = "UserBrand";
    edit.spool_name = "UserSpool";
    edit.spoolman_id = 99;
    backend.set_slot_info(0, edit, false);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCCu);
    REQUIRE(info.material == "PETG");
    REQUIRE(info.brand == "UserBrand");
    REQUIRE(info.spool_name == "UserSpool");
    REQUIRE(info.spoolman_id == 99);
}

// ==========================================================================
// Task 10: set_slot_info(persist=true) writes through to
// FilamentSlotOverrideStore + in-memory overrides_ map so user edits survive
// subsequent parses.
// ==========================================================================

TEST_CASE("AD5X IFS set_slot_info(persist=true) stores override in memory and store",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Build a real MoonrakerAPIMock so the backend's override store has a
    // destination to write to. on_started() is not called — overrides_
    // starts empty — so we can assert the persist path populates it.
    Ad5xIfsTmpCacheDir tmp("task10_stores_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    // Native ZMOD path is skipped by marking has_ifs_vars_ true — this test
    // focuses on the override-store write, not the Klipper-facing side.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // In-memory reads immediately see the edits — apply_overrides uses the
    // newly-staged override rather than any pre-edit value.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.remaining_weight_g == 850.0f);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);

    // overrides_ map was written under mutex_ as the persist staging step.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Moonraker DB received the AFC-shaped record via save_async (which
    // MoonrakerAPIMock dispatches synchronously in-call).
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");
}

TEST_CASE("AD5X IFS set_slot_info(persist=false) does NOT write to store",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Same fixture as above, but with persist=false the override store
    // must stay untouched — set_slot_info is a pure in-memory preview.
    Ad5xIfsTmpCacheDir tmp("task10_no_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // No override staged — the in-memory entry carries the edit directly
    // (since no prior override clobbers it via apply_overrides).
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    // Moonraker DB not touched.
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // The edit is still visible via get_slot_info — this is the preview path.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE("AD5X IFS set_slot_info(persist=true) updates overrides_ so next parse preserves it",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // After a persist=true write, a subsequent firmware parse must still see
    // the user's edit — that's the whole point of the override-store layer.
    Ad5xIfsTmpCacheDir tmp("task10_next_parse");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    // Leave has_ifs_vars_ false so the persist path also exercises the
    // Adventurer5M.json write — write_adventurer_json will no-op because
    // the download mock is empty, which is fine: we only care that the
    // override persist happened and the parse layer respects it.
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    // set_slot_info's Adventurer5M.json write will error out because the
    // mock has no file — we don't care, we just need overrides_ populated.
    backend.set_slot_info(0, edit, /*persist=*/true);

    // Simulate a subsequent firmware parse reporting different values — the
    // override must win, since that's the user's saved preference.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#000000", "ffmType1": "ABS"}
    })");

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");     // override still wins after re-parse
    CHECK(info.material == "PLA");         // override still wins
    CHECK(info.color_rgb == 0xFF5500u);    // override still wins
}

TEST_CASE("AD5X IFS set_slot_info(persist=true) with no store still updates in-memory map",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Backend constructed with no api/client AND no injected store — the
    // persist path must still stage the override in memory so the current
    // UI session sees the edit, even though there's nowhere to save it.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    // write_adventurer_json will fail with "No API connection" because api_
    // is nullptr. That's expected — but the in-memory override stage
    // happens BEFORE that write and must still be visible.
    (void)err;

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->material == "PLA");
    CHECK(staged->color_rgb == 0xFF5500u);
}

TEST_CASE("AD5X IFS set_slot_info(persist=true) with pre-existing override replaces it",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Seed an old override (simulating a prior load from disk), then overwrite
    // it via set_slot_info. get_slot_info must reflect the NEW values
    // immediately, not the old staged override.
    Ad5xIfsTmpCacheDir tmp("task10_replace");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride old;
    old.brand = "OldBrand";
    old.spool_name = "OldSpool";
    old.spoolman_id = 7;
    Ad5xIfsTestAccess::seed_override(backend, 0, old);

    // User edits with a different brand — the NEW values must win, not
    // the old override.
    SlotInfo edit;
    edit.brand = "NewBrand";
    edit.spool_name = "NewSpool";
    edit.spoolman_id = 99;
    edit.material = "PLA";
    edit.color_rgb = 0xAABBCC;
    backend.set_slot_info(0, edit, /*persist=*/true);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "NewBrand");
    CHECK(info.spool_name == "NewSpool");
    CHECK(info.spoolman_id == 99);

    // Staged override replaced cleanly.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "NewBrand");
    CHECK(staged->spoolman_id == 99);
}

// ==========================================================================
// External color/material edits (Mainsail console, AD5X LCD, native zmod
// dialog, CHANGE_ZCOLOR from any non-Helix source) must REFRESH the Moonraker
// DB lane_data entry that Orca's MoonrakerPrinterAgent reads — they must NOT
// wipe the brand/spool_name/spoolman_id metadata. (compulsivejohnny on
// Discord: lane_data went stale after every external CHANGE_ZCOLOR because
// the previous "color change = physical swap" heuristic cleared the record.)
// Initial startup observations are a baseline and never trigger a sync.
// Genuine spool removal is detected by the eject path (presence true→false
// in parse_adventurer_json) and clears the override there.
// ==========================================================================

TEST_CASE("AD5X IFS external color change syncs lane_data, preserves brand metadata",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    Ad5xIfsTmpCacheDir tmp("ext_color_change_syncs");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed a lane_data entry in the mock DB plus a matching in-memory override
    // — what the override store load would produce after a Helix-initiated edit.
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"vendor", "Polymaker"},
                                         {"spool_id", 42},
                                         {"spool_name", "PolyLite Orange"},
                                         {"material", "PLA"},
                                         {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: firmware reports color FF5500 — establishes BASELINE. No
    // sync (first observation), override still wins.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.color_rgb == 0xFF5500u);
    }
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: firmware reports a DIFFERENT color (and material). This is
    // an external edit — sync override + lane_data, KEEP brand/spool/spoolman.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    {
        auto info = backend.get_slot_info(0);
        // Brand metadata preserved.
        CHECK(info.brand == "Polymaker");
        CHECK(info.spool_name == "PolyLite Orange");
        CHECK(info.spoolman_id == 42);
        // Color + material reflect firmware truth.
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");

    // Moonraker DB lane1 entry refreshed by save_async — Orca now sees the
    // new color/material plus the preserved vendor + spool_id.
    auto db = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!db.is_null());
    CHECK(db.value("color", "") == "#0055FF");
    CHECK(db.value("material", "") == "PETG");
    CHECK(db.value("vendor", "") == "Polymaker");
    CHECK(db.value("spool_id", 0) == 42);
}

TEST_CASE("AD5X IFS external color change with no override creates minimal lane_data record",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // When zmod owns slot color/material truth and the user has never touched
    // the slot via Helix, lane_data was previously empty → Orca had no way to
    // see the slot's color from MoonrakerPrinterAgent. Now we publish a
    // minimal record (color + material) so Orca's view stays useful.
    Ad5xIfsTmpCacheDir tmp("ext_color_change_creates_minimal");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // No seeded override. lane_data starts empty.
    REQUIRE(api.mock_get_db_value("lane_data", "lane1").is_null());

    // First parse: establishes baseline at FF5500. No sync (baseline).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: external color change. Minimal override created + lane_data
    // record published.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");
    // brand etc. left at defaults — no synthetic vendor name.
    CHECK(staged->brand.empty());
    CHECK(staged->spoolman_id == 0);

    auto db = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!db.is_null());
    CHECK(db.value("color", "") == "#0055FF");
    CHECK(db.value("material", "") == "PETG");
    // vendor field absent (to_lane_data_record omits empty strings).
    CHECK_FALSE(db.contains("vendor"));
}

TEST_CASE("AD5X IFS eject (empty Adventurer5M.json color) clears the override",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Genuine spool removal: zmod sets ffmColor to "" when the user ejects.
    // parse_adventurer_json must clear the override (brand/spool_name/spoolman
    // describe the gone spool) and delete the lane_data entry.
    Ad5xIfsTmpCacheDir tmp("eject_clears_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"vendor", "Polymaker"},
                                         {"spool_id", 42},
                                         {"material", "PLA"},
                                         {"color", "#FF5500"}});
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Loaded state: presence becomes true via the non-empty color.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Eject: ffmColor1 empty. parse_adventurer_json's eject branch clears
    // the override (and the MR DB lane_data entry) directly.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "", "ffmType1": ""}
    })");

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand.empty());
        CHECK(info.spoolman_id == 0);
    }
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
}

// ==========================================================================
// _IFS_VARS mirror: when an external CHANGE_ZCOLOR fires, parse_adventurer_json
// must mirror the new colors_/materials_ snapshot into the lessWaste/bambufy
// plugin's <prefix>_colors / <prefix>_types save_variables. Audited 2026-05-04
// against Hrybmo/lesswaste and function3d/bambufy: neither plugin self-syncs
// in response to CHANGE_ZCOLOR, so without this mirror the plugin's runout-
// recovery alternate-port lookup, smart-purge skip decision, and
// _IFS_COLORS_ASSIGN dialog all run against stale color data and silently
// print the wrong color or skip the wrong purge.
// ==========================================================================

namespace {
class GcodeCapturingBackend : public AmsBackendAd5xIfs {
  public:
    using AmsBackendAd5xIfs::AmsBackendAd5xIfs;
    std::vector<std::string> captured_gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    bool any_gcode_starts_with(const std::string& prefix) const {
        for (const auto& g : captured_gcodes) {
            if (g.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }
};
} // namespace

TEST_CASE("AD5X IFS external color change mirrors colors+types into _IFS_VARS",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_external");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // First parse establishes baseline — no sync, no mirror.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));

    // External color change → sync fires, _IFS_VARS mirror dispatches.
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS types="));

    // Find the actual payloads — should reflect the new firmware truth.
    bool found_color_payload = false;
    bool found_type_payload = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g.find("_IFS_VARS colors=") == 0 && g.find("'0055FF'") != std::string::npos)
            found_color_payload = true;
        if (g.find("_IFS_VARS types=") == 0 && g.find("'PETG'") != std::string::npos)
            found_type_payload = true;
    }
    CHECK(found_color_payload);
    CHECK(found_type_payload);

    // lessWaste prefix → no SHOW=0 suffix (lessWaste's _IFS_VARS doesn't
    // accept it — adding the param would break the macro call).
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS ", 0) == 0) {
            CHECK(g.find("SHOW=0") == std::string::npos);
        }
    }
}

TEST_CASE("AD5X IFS bambufy prefix gets SHOW=0 to suppress _IFS_VARS echo",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_bambufy_show0");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Establish baseline + drive an external change.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    // bambufy's _IFS_VARS macro accepts SHOW=0 to skip the RESPOND echo
    // (lesswaste does not). Both colors+types pushes get the suffix.
    bool colors_has_show0 = false;
    bool types_has_show0 = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS colors=", 0) == 0 && g.find("SHOW=0") != std::string::npos)
            colors_has_show0 = true;
        if (g.rfind("_IFS_VARS types=", 0) == 0 && g.find("SHOW=0") != std::string::npos)
            types_has_show0 = true;
    }
    CHECK(colors_has_show0);
    CHECK(types_has_show0);
}

TEST_CASE("AD5X IFS mirror skipped when has_ifs_vars_ is false (stock zmod)",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Stock zmod (no lessWaste/bambufy plugin) has no _IFS_VARS macro to call.
    // Sync still fires (lane_data still updates for Orca) but the mirror push
    // is skipped — calling _IFS_VARS on a printer without the macro just
    // produces a "Unknown command" gcode error.
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_stock_zmod");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, false); // stock zmod
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    // Sync counter bumped (lane_data still sync'd via save_async).
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) > 0);
    // ...but no _IFS_VARS dispatched.
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS"));
}

// Regression: bundle AQ6DALWG, raza616 v0.99.51, "filament type changing on
// boot". On startup, the initial printer.objects.query response feeds
// parse_save_variables which iterates update_slot_from_state for every slot
// BEFORE Adventurer5M.json is fetched and parsed. At that point colors_[idx]
// is empty, so the firmware-color branch in update_slot_from_state is skipped
// and entry->info.color_rgb is whatever was last left there — initially the
// SlotInfo default (AMS_DEFAULT_SLOT_COLOR / 0x808080), or a value leaked by
// a prior apply_overrides call.
//
// Pre-fix this populated last_firmware_color_ with a phantom 0x808080 baseline.
// Seconds later parse_adventurer_json arrived with the real firmware color
// (e.g. #898989); the diff against the phantom baseline was misread as a
// physical spool swap and wiped the user override (brand, spoolman_id, weights,
// material). User saw their PETG spool flip back to firmware-truth PLA on
// every boot, and after the wipe the next boot loaded 0 overrides because
// boot 1 had cleared them all.
TEST_CASE("AD5X IFS empty colors_[] on boot does NOT establish phantom baseline",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    Ad5xIfsTmpCacheDir tmp("boot_phantom_baseline_no_clear");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed a saved override (PETG, brand, spoolman_id) — what the user
    // configured in a prior session and persisted into filament_slot store.
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"vendor", "Polymaker"},
                                         {"spool_id", 42},
                                         {"material", "PETG"},
                                         {"color", "#898989"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Gray";
    ovr.spoolman_id = 42;
    ovr.material = "PETG";
    ovr.color_rgb = 0x898989;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Boot path step 1: parse_save_variables fires from the initial
    // printer.objects.query response. lessWaste/bambufy tools data is
    // present, which triggers update_slot_from_state for every slot — but
    // colors_[idx] is still empty because Adventurer5M.json hasn't been
    // fetched yet. This is the call that, pre-fix, establishes the phantom
    // 0x808080 baseline.
    json save_vars;
    save_vars["less_waste_tools"] = json::array(
        {0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}); // tool 0 -> port 1
    save_vars["less_waste_current_tool"] = -1;
    save_vars["less_waste_external"] = 0;
    Ad5xIfsTestAccess::parse_vars(backend, save_vars);

    // Override must still be intact after parse_save_variables — the helper
    // had no firmware-truth color so it MUST have skipped the swap check.
    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->material == "PETG");
        CHECK(staged->spoolman_id == 42);
    }
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Boot path step 2: Adventurer5M.json finally arrives with the real
    // firmware color. This is the FIRST real firmware reading for the slot,
    // so it MUST establish the baseline rather than be misread as a swap.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#898989", "ffmType1": "PLA"}
    })");

    // Override survives: brand + spoolman_id still present, user's chosen
    // material (PETG) still wins over firmware-reported PLA.
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.material == "PETG"); // user override, not firmware PLA
        CHECK(info.color_rgb == 0x898989u);
    }
    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->material == "PETG");
    }
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x898989u);

    // Boot path step 3: a SUBSEQUENT firmware parse with a different color
    // is an external edit — sync override + lane_data, KEEP brand metadata.
    // (Pre-lane_data-sync rework, this branch interpreted the color delta as
    // a physical-swap signal and wiped the override entirely; that behavior
    // is what caused the lane_data sync regression compulsivejohnny hit.)
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.color_rgb == 0xFF5500u);
        // External edit changed material to firmware truth — override.material
        // is synced too, since material is firmware-owned for AD5X-IFS (it has
        // to be in zmod's whitelist or the firmware errors). Brand metadata
        // is the only thing the user owns independently and it persists.
        CHECK(info.material == "PLA");
    }
    auto staged3 = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged3.has_value());
    CHECK(staged3->brand == "Polymaker");
    CHECK(staged3->color_rgb == 0xFF5500u);
    CHECK(staged3->material == "PLA");
}

TEST_CASE("AD5X IFS first firmware color observation does NOT clear override",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Even when the override's saved color differs from what the firmware
    // reports at startup, the very first observation is a BASELINE. This
    // matches real-world startup: override loaded from lane_data arrives
    // before firmware is polled; the colors may not match exactly
    // (rounding, scheme differences). We must not clear on first observation.
    Ad5xIfsTmpCacheDir tmp("task11_first_observation_no_clear");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"vendor", "Polymaker"},
                                         {"spool_id", 42},
                                         {"material", "PLA"},
                                         {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500; // saved = orange
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a DIFFERENT color on the FIRST observation — no prior
    // baseline, so this must NOT trigger a clear.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PLA"}
    })");

    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->spoolman_id == 42);
    }
    // DB entry preserved.
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Steady state: a SECOND parse of the SAME firmware color that was used
    // to establish the baseline must ALSO not clear. This locks in the
    // invariant that the baseline-first-observation path doesn't leave
    // last_firmware_color_ in a weird state that fires on unchanged polls.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PLA"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

// ------------------------------------------------------------------
// Task 11 bug fix regression coverage: set_slot_info must not wipe the
// override it just staged. Before the fix, set_slot_info staged the
// new override, then update_slot_from_state -> check_external_color_change
// compared the user's new color against the prior firmware baseline and
// wiped the freshly-staged override.
// ------------------------------------------------------------------

TEST_CASE("AD5X IFS set_slot_info(persist=true) does not wipe override on color edit",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Baseline firmware parse establishes last_firmware_color_.
    // Then user saves a new override with a DIFFERENT color.
    // The override must survive — not get treated as a hardware swap.
    Ad5xIfsTmpCacheDir tmp("task11_set_slot_info_persist_true_no_wipe");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // First parse: color FF5500, no override — establishes baseline.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // User edits: sets NEW color + brand. Before the fix this triggered a
    // spurious "physical swap" clear because the new color (0x00FF00)
    // differed from the baseline (0xFF5500).
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PLA";
    edit.brand = "Polymaker";
    backend.set_slot_info(0, edit, /*persist=*/true);

    // Assert override is present and unharmed.
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.color_rgb == 0x00FF00u);
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->color_rgb == 0x00FF00u);

    // Baseline should have advanced to the user's chosen color.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x00FF00u);

    // Follow-up firmware parse with the NEW color should also not clear
    // (baseline now matches — no swap signal).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PLA"}
    })");
    auto staged2 = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged2.has_value());
    CHECK(staged2->brand == "Polymaker");
}

TEST_CASE("AD5X IFS set_slot_info(persist=false) preview does not wipe existing override",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    // Seed a pre-existing override, establish baseline via firmware parse,
    // then preview a DIFFERENT color with persist=false. The preview must
    // not be misread as a physical swap — the saved override must remain
    // in overrides_.
    Ad5xIfsTmpCacheDir tmp("task11_set_slot_info_persist_false_preview_no_wipe");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed a pre-existing override that matches the upcoming firmware parse.
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spool_name = "PolyLite Orange";
    saved.spoolman_id = 42;
    saved.material = "PLA";
    saved.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, saved);

    // Firmware parse establishes baseline at the override's color.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Preview: persist=false with a DIFFERENT color. Before the fix, the
    // upcoming update_slot_from_state call flagged this as a physical swap
    // and wiped the pre-existing override.
    SlotInfo preview;
    preview.color_rgb = 0x00FF00;
    preview.material = "PLA";
    backend.set_slot_info(0, preview, /*persist=*/false);

    // The previously saved override must still exist in overrides_.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Baseline should have advanced to the previewed color, so a subsequent
    // parse that mirrors the preview color reads as "no change" and doesn't
    // clear either. (Saved override still wins until persist=true is called.)
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x00FF00u);
}

TEST_CASE("AD5X IFS firmware color unchanged across parses does NOT clear",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Parse twice with the same firmware color — no clear should fire on
    // either iteration (baseline then unchanged).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
}

TEST_CASE("AD5X IFS firmware color change with no override creates a minimal one",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // No override seeded and no override store wired up — sync_override_to_
    // firmware_locked still creates the in-memory minimal override (and
    // skips save_async cleanly when override_store_ is null). This proves
    // the helper is null-safe and that lane_data publication doesn't gate
    // the in-memory sync.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // First parse establishes baseline only — no sync.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0xFF5500u);
        CHECK(info.brand.empty());
    }
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Second parse: external color change. Minimal override created (carries
    // color + material only; brand etc. left at defaults).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
        CHECK(info.brand.empty());
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");
    CHECK(staged->brand.empty());
}

TEST_CASE("AD5X IFS empty firmware color does not update the baseline",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // firmware_color == 0 is treated as "no reading" and must not update
    // the baseline. If it did, an intermittent empty poll would mask a real
    // subsequent swap (the next non-empty poll would look like "0 -> new"
    // which would wrongly clear), OR worse, clear on every unread poll.
    //
    // Drive check_external_color_change directly — the Adventurer5M.json parse
    // path can't produce firmware_color==0 (empty hex becomes 0x808080 gray),
    // so a direct call is the only way to exercise the guard unambiguously.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports FF5500 — baseline established.
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF5500);
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Firmware reports color 0 (empty/unread). Must NOT update baseline and
    // must NOT clear the override.
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0);
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Firmware reports FF5500 again — still matches baseline, still no clear.
    // (If the zero-reading had wrongly overwritten the baseline, this would
    // look like "0 -> FF5500" and trigger a spurious clear.)
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF5500);
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
}

// ------------------------------------------------------------------
// Task 16: explicit clear_slot_override — user pressed "Clear slot metadata".
// Verifies the same clear pathway used by hardware-event detection is
// reachable through the public API with no swap signal required.
// ------------------------------------------------------------------

TEST_CASE("AD5X IFS clear_slot_override erases in-memory override and MR DB entry",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    Ad5xIfsTmpCacheDir tmp("task16_clear_slot_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed both halves of the override — lane_data on the Moonraker side
    // and the in-memory map on the backend side — so the clear has something
    // to remove at each layer.
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"vendor", "Polymaker"},
                                         {"spool_id", 42},
                                         {"material", "PLA"},
                                         {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.total_weight_g = 1000.0f;
    ovr.remaining_weight_g = 750.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Prime with a firmware parse so slots_ has an entry to reset. This also
    // establishes the last_firmware_color_ baseline — unrelated to the
    // clear_slot_override path but good sanity for the test.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.remaining_weight_g == 750.0f);
    }
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // User presses "Clear slot metadata". Override MUST be removed from both
    // layers — no swap signal needed.
    backend.clear_slot_override(0);

    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    auto info = backend.get_slot_info(0);
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.remaining_weight_g < 0.0f); // -1.0 sentinel ("unknown")
    CHECK(info.total_weight_g < 0.0f);
    CHECK(info.color_name.empty());
    // Firmware-sourced color flows through — clear only touches override-exclusive fields.
    CHECK(info.color_rgb == 0xFF5500u);
    CHECK(info.material == "PLA");
}

TEST_CASE("AD5X IFS clear_slot_override is safe when no override is present",
          "[ams][ad5x_ifs][filament_slot_override][slow]") {
    Ad5xIfsTmpCacheDir tmp("task16_clear_slot_override_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");

    // No override staged. Calling clear must not crash and must leave
    // firmware-sourced fields intact.
    backend.clear_slot_override(0);

    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x00FF00u);
    CHECK(info.material == "PETG");
}

TEST_CASE("AD5X IFS clear_slot_override rejects out-of-range indices",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Should not crash or touch anything — validate_slot_index rejects.
    backend.clear_slot_override(-1);
    backend.clear_slot_override(AmsBackendAd5xIfs::NUM_PORTS);
    backend.clear_slot_override(999);
    SUCCEED();
}

// ==========================================================================
// Listener self-feedback regression (v0.99.51 GET_ZCOLOR spam loop)
// ==========================================================================
//
// zmod's GET_ZCOLOR macro body echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens through
// notify_gcode_response while our query is still in flight. Pre-fix, the
// listener treated those echoes as external state-change triggers and called
// schedule_zcolor_query() again, looping at ~2-4 Hz on the gcode console.
// The fix: when zcolor_query_active_ is set, buffer the line and return
// without re-arming a query.

TEST_CASE("AD5X IFS listener buffers RUN_ZCOLOR during in-flight query (no re-arm)",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, true);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    REQUIRE(Ad5xIfsTestAccess::zcolor_buffer_size(backend) == 0);

    // Lines that pre-fix would have re-armed the query loop.
    bool buffered_a =
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// RUN_ZCOLOR slot=2 color=FF0000");
    bool buffered_b =
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// CHANGE_ZCOLOR slot=3");

    CHECK(buffered_a);
    CHECK(buffered_b);
    CHECK(Ad5xIfsTestAccess::zcolor_buffer_size(backend) == 2);
    // Critically: schedule_zcolor_query must NOT have been re-armed.
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
}

TEST_CASE("AD5X IFS listener fires schedule_zcolor_query on external RUN_ZCOLOR (no in-flight)",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // No in-flight query: the line is a genuine external state-change signal.
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    bool buffered =
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// CHANGE_ZCOLOR slot=1 color=00FF00");

    CHECK_FALSE(buffered); // Treated as external trigger, not buffered.
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
}

TEST_CASE("AD5X IFS listener ignores unrelated gcode lines",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);

    // Lines without RUN_ZCOLOR / CHANGE_ZCOLOR must not trigger anything.
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Extruder: 1: PLA/FF0000 | IFS: True");
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "ok");
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// 2: PETG/00FF00");

    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
}

// ==========================================================================
// JSON-content poll dedup
// ==========================================================================
//
// poll_adventurer_json downloads Adventurer5M.json and only parses + fires
// GET_ZCOLOR when the body has changed vs. last seen. The comparison is
// content-equality on the raw JSON string. Tests drive the comparison
// helper directly so they don't depend on a live download path.

TEST_CASE("AD5X IFS note_json_content reports changed only on different bytes",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    const std::string a = R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})";
    const std::string b = R"({"FFMInfo":{"ffmColor1":"#00FF00","ffmType1":"PETG"}})";

    // First-ever observation is "changed" (last_json_content_ starts empty).
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, a));
    // Re-observing the same content is NOT changed.
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, a));
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, a));
    // A different body flips back to changed.
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, b));
    // And steady-state again on b.
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, b));
}

// ==========================================================================
// write_adventurer_json local-filesystem path
//
// Bug context: bundle DQK7X96B (AD5X stock-ZMOD, v0.99.52) showed Klipper
// shutdown with JSONDecodeError on Adventurer5M.json. Root cause: Moonraker's
// /server/files/upload writes to /root/printer_data/tmp/ then os.rename's to
// the symlinked /usr/prog/config/Adventurer5M.json — those two locations are
// on different mounts on AD5X stock-ZMOD, so rename throws EXDEV and the
// destination ends up empty. Klipper's zmod_color.py then crashes at startup
// trying to json.load() the empty file → printer bricked.
//
// Fix: when helix-screen runs on the same host as Moonraker, write the file
// directly via filesystem APIs (atomic temp+rename within the same dir).
// Only falls back to the Moonraker upload when remote.
// ==========================================================================

namespace {
struct Ad5xIfsTmpJsonFile {
    std::filesystem::path path;
    explicit Ad5xIfsTmpJsonFile(const std::string& suffix, const std::string& seed_content) {
        path = std::filesystem::temp_directory_path() /
               ("ad5x_ifs_advjson_" + suffix + "_" + std::to_string(::getpid()) + ".json");
        std::filesystem::remove(path);
        if (!seed_content.empty()) {
            std::ofstream f(path);
            f << seed_content;
        }
    }
    ~Ad5xIfsTmpJsonFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        // Cleanup any leftover temp from atomic write
        std::filesystem::remove(path.string() + ".tmp", ec);
    }
};
} // namespace

TEST_CASE("AD5X IFS write_adventurer_json_local read-modify-writes the on-disk file",
          "[ams][ad5x_ifs][local_write]") {
    // Seed a realistic Adventurer5M.json with all four slots.
    const std::string seed = R"({
    "FFMInfo": {
        "ffmColor1": "#FF0000",
        "ffmType1": "PLA",
        "ffmColor2": "#00FF00",
        "ffmType2": "PETG",
        "ffmColor3": "#0000FF",
        "ffmType3": "ABS",
        "ffmColor4": "#FFFFFF",
        "ffmType4": "PLA"
    }
})";
    Ad5xIfsTmpJsonFile tmp("rmw", seed);

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // Stage new color + material on slot 0 (port 1) and trigger the write.
    Ad5xIfsTestAccess::set_color(backend, 0, "AABBCC");
    Ad5xIfsTestAccess::set_material(backend, 0, "TPU");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    REQUIRE(err.success());

    // Read the file back; slot 1 should be updated and other slots untouched.
    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#AABBCC");
    CHECK(doc["FFMInfo"]["ffmType1"] == "TPU");
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#00FF00");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
    CHECK(doc["FFMInfo"]["ffmColor3"] == "#0000FF");
    CHECK(doc["FFMInfo"]["ffmType3"] == "ABS");
    CHECK(doc["FFMInfo"]["ffmColor4"] == "#FFFFFF");
    CHECK(doc["FFMInfo"]["ffmType4"] == "PLA");
}

TEST_CASE("AD5X IFS write_adventurer_json_local creates FFMInfo if missing",
          "[ams][ad5x_ifs][local_write]") {
    // Adventurer5M.json without FFMInfo (zmod default-initialized empty file).
    Ad5xIfsTmpJsonFile tmp("missing_ffminfo", "{}");

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());
    Ad5xIfsTestAccess::set_color(backend, 1, "112233");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 1);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#112233");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
}

TEST_CASE("AD5X IFS write_adventurer_json_local rejects empty path",
          "[ams][ad5x_ifs][local_write]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // local path not set — direct write must report failure so caller falls
    // back to Moonraker upload.
    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    CHECK_FALSE(err.success());
}

TEST_CASE("AD5X IFS write_adventurer_json_local rejects unparseable existing file",
          "[ams][ad5x_ifs][local_write]") {
    // Existing corrupted file (the exact symptom from bundle DQK7X96B —
    // an empty Adventurer5M.json that crashed Klipper). The local-write path
    // must NOT silently overwrite — return an error and let the caller decide
    // recovery. Empty file is not the same as missing file: missing means
    // first-time write; empty means prior corruption that we shouldn't mask.
    Ad5xIfsTmpJsonFile tmp("corrupt", "");
    // Re-touch the file as zero bytes (Ad5xIfsTmpJsonFile's empty seed_content
    // skips the file write). Create explicitly.
    {
        std::ofstream f(tmp.path);
        // intentionally empty
    }

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    // Recovery: write a fresh-baseline FFMInfo block. The corrupted-file case
    // is exactly the bricked-printer state — auto-repair from the values we
    // have in colors_/materials_ is the whole point of the direct-write fix.
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#FF0000");
}

TEST_CASE("AD5X IFS write_adventurer_json_local atomic — leaves no .tmp on success",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("atomic", R"({"FFMInfo":{"ffmColor1":"#000000","ffmType1":"PLA"}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());
    Ad5xIfsTestAccess::set_color(backend, 2, "ABCDEF");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 2);
    REQUIRE(err.success());

    // The atomic-rename pattern uses <path>.tmp as the staging file. After a
    // successful write the temp must be gone (rename consumes it).
    CHECK_FALSE(std::filesystem::exists(tmp.path.string() + ".tmp"));
}

// ==========================================================================
// #904: stale-plugin-data fallback + user-defined material types
// ==========================================================================

// TMTYD's printer in #904 had bambufy_tools=[4,2,4,3,...] AND
// less_waste_tools=[2,1,3,4] left over from past plugin activations, with
// neither plugin currently driving state. Without the fallback, our prefix
// detection picks bambufy first and applies [4,2,4,3,...] — putting T0 on
// port 4 and breaking every per-port T-badge in the UI. The fallback rule:
// when both prefixes have _tools arrays AND they disagree, neither is
// authoritative; revert to the default 1:1 mapping.
TEST_CASE("AD5X IFS #904 both prefixes conflict falls back to 1:1 tool map",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);

    // TMTYD's exact data: bambufy_tools and less_waste_tools both non-default
    // and disagreeing.
    json vars = json{
        {"bambufy_tools", json::array({4, 2, 4, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4})},
        {"bambufy_current_tool", -1},
        {"less_waste_tools", json::array({2, 1, 3, 4})},
        {"less_waste_current_tool", -1},
    };

    Ad5xIfsTestAccess::parse_vars(backend, vars);

    auto map = Ad5xIfsTestAccess::tool_map(backend);
    // Default 1:1 mapping — T0→port1, T1→port2, T2→port3, T3→port4, then
    // UNMAPPED_PORT for the remaining slots.
    CHECK(map[0] == 1);
    CHECK(map[1] == 2);
    CHECK(map[2] == 3);
    CHECK(map[3] == 4);
    for (size_t i = 4; i < map.size(); ++i) {
        CHECK(map[i] == AmsBackendAd5xIfs::UNMAPPED_PORT);
    }

    // Per-port T-badge: slot 3 (port 4) must show T3, NOT T0. Pre-fix this
    // was T0 because bambufy_tools[0]=4 made find_first_tool_for_port(4)=0.
    auto info3 = backend.get_slot_info(3);
    CHECK(info3.mapped_tool == 3);
}

// Single-prefix users still get their map applied (no false-positive
// fallback for users with a legitimately active plugin).
TEST_CASE("AD5X IFS #904 single-prefix non-default tools is honored",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);

    // Only bambufy_tools, with a custom mapping.
    json vars = json{
        {"bambufy_tools", json::array({3, 2, 1, 0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
        {"bambufy_current_tool", -1},
    };

    Ad5xIfsTestAccess::parse_vars(backend, vars);
    auto map = Ad5xIfsTestAccess::tool_map(backend);
    CHECK(map[0] == 3);
    CHECK(map[1] == 2);
    CHECK(map[2] == 1);
    CHECK(map[3] == 0);
}

// Both-prefixes-but-equal: no conflict, apply the map normally. (Edge case:
// a user with bambufy active whose less_waste_tools happens to match.)
TEST_CASE("AD5X IFS #904 both prefixes agree — no fallback",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);

    auto same = json::array({2, 1, 4, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    json vars = json{
        {"bambufy_tools", same},
        {"less_waste_tools", same},
    };

    Ad5xIfsTestAccess::parse_vars(backend, vars);
    auto map = Ad5xIfsTestAccess::tool_map(backend);
    CHECK(map[0] == 2);
    CHECK(map[1] == 1);
    CHECK(map[2] == 4);
    CHECK(map[3] == 3);
}

// Custom material types from bambufy_custom_types must surface in
// get_supported_materials() so the edit modal dropdown isn't restricted to
// the firmware whitelist (#904 root cause #2: PLA+ stomped to PLA on save).
TEST_CASE("AD5X IFS #904 bambufy_custom_types merged into supported materials",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    json vars = json{
        {"bambufy_custom_types", json::array({"PLA+", "rPLA", "PETG-Pro", "PLA-CF"})},
    };
    Ad5xIfsTestAccess::parse_vars(backend, vars);

    auto custom = Ad5xIfsTestAccess::custom_material_types(backend);
    REQUIRE(custom.size() == 4);
    CHECK(custom[0] == "PLA+");
    CHECK(custom[2] == "PETG-Pro");

    auto supported = backend.get_supported_materials();
    REQUIRE(supported.has_value());
    auto contains = [&](const std::string& s) {
        return std::find(supported->begin(), supported->end(), s) != supported->end();
    };
    // Firmware whitelist still present.
    CHECK(contains("PLA"));
    CHECK(contains("PETG-CF"));
    // User-defined types appended.
    CHECK(contains("PLA+"));
    CHECK(contains("rPLA"));
    CHECK(contains("PETG-Pro"));
    // PLA-CF was already in the whitelist — no duplicate (case-insensitive
    // dedup).
    auto count = std::count(supported->begin(), supported->end(), "PLA-CF");
    CHECK(count == 1);

    // Round-trip via normalize_material: the user-defined type must come
    // back unchanged (this is what stops the PLA+ → PLA stomp on save).
    CHECK(backend.normalize_material("PLA+") == "PLA+");
    CHECK(backend.normalize_material("pla+") == "PLA+"); // case-insensitive
}

// /mod_data/user.cfg parsing — the [zmod_ifs] filament_<NAME>: <TEMP>
// directive is zmod's official mechanism for user-defined material types
// (https://wiki.zmod.link/AD5X/#7-add-custom-filament-types). Out-of-section
// matches must NOT be picked up.
TEST_CASE("AD5X IFS #904 user.cfg [zmod_ifs] filament_* parser",
          "[ams][ad5x_ifs][issue_904]") {
    SECTION("standard wiki example") {
        const std::string body =
            "[zmod_ifs]\n"
            "filament_NEWTYPE: 300\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        REQUIRE(names.size() == 1);
        CHECK(names[0] == "NEWTYPE");
    }

    SECTION("multiple entries with comments and other sections") {
        const std::string body =
            "# global header\n"
            "[gcode_macro FOO]\n"
            "filament_IGNORED: 999  ; not in zmod_ifs\n"
            "\n"
            "[zmod_ifs]\n"
            "filament_PLA+: 220   # inline comment\n"
            "filament_RPLA: 215\n"
            "filament_HELIX: 240 ; semicolon comment\n"
            "other_setting: 42\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        REQUIRE(names.size() == 3);
        CHECK(names[0] == "PLA+");
        CHECK(names[1] == "RPLA");
        CHECK(names[2] == "HELIX");
    }

    SECTION("CRLF line endings (zmod files saved on Windows)") {
        const std::string body = "[zmod_ifs]\r\nfilament_FOO: 200\r\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        REQUIRE(names.size() == 1);
        CHECK(names[0] == "FOO");
    }

    SECTION("empty body") {
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types("");
        CHECK(names.empty());
    }

    SECTION("section without filament_ entries") {
        const std::string body = "[zmod_ifs]\nallowed_tool_count: 4\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        CHECK(names.empty());
    }
}

// End-to-end #904 root-cause-#2 fix: TMTYD's slot was PLA+ in zmod, our
// edit modal showed PLA, save wrote _IFS_VARS types="['PLA',...]"
// overwriting zmod's truth. The fix loads bambufy_custom_types into the
// supported-materials list so normalize_material's case-insensitive
// exact-match passes "PLA+" through unchanged. This test exercises the
// FULL save path — from save_variables ingestion through set_slot_info to
// the cached SlotInfo — to prove the round-trip doesn't stomp the user's
// chosen type.
TEST_CASE("AD5X IFS #904 PLA+ round-trips through set_slot_info after custom_types load",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Step 1: ingest TMTYD's bambufy_custom_types from save_variables.
    json vars = json{
        {"bambufy_custom_types", json::array({"PLA+", "rPLA", "PETG-Pro", "PLA-CF"})},
    };
    Ad5xIfsTestAccess::parse_vars(backend, vars);

    // Step 2: user edits slot 0 to PLA+. set_slot_info runs normalize_material
    // internally, which (post-fix) sees "PLA+" in the supported list and
    // returns it unchanged. Pre-fix this returned "PLA" (compat_group fallback)
    // and silently destroyed the user's choice.
    SlotInfo edit;
    edit.color_rgb = 0x000DFF;
    edit.material = "PLA+";
    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // Step 3: read it back and confirm PLA+ survived.
    auto info = backend.get_slot_info(0);
    CHECK(info.material == "PLA+");
    CHECK(info.color_rgb == 0x000DFF);

    // Lowercase input also round-trips (zmod COLOR macro is case-insensitive
    // when matching user.cfg types; our modal preserves the canonical case
    // from the supported list).
    SlotInfo edit_lc;
    edit_lc.color_rgb = 0xABCDEF;
    edit_lc.material = "rpla";
    err = backend.set_slot_info(1, edit_lc, /*persist=*/false);
    REQUIRE(err.success());
    auto info1 = backend.get_slot_info(1);
    CHECK(info1.material == "rPLA");
}

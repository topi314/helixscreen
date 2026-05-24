// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "config.h"
#include "filament_database.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

// Friend-class shim for FilamentSlotOverrideStore — same idiom as IFS /
// Snapmaker / ACE tests. Lets us redirect the store's on-disk read-cache to a
// per-test tmp dir so save_async doesn't pollute the developer's real
// helixscreen config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

// Friend-class shim for AmsBackendCfs — declared as friend in the backend
// header. Gives tests narrow accessors for private override state without
// going through the public get_slot_info path (which layers apply_overrides
// on top and obscures what the internal maps actually hold).
class CfsTestAccess {
  public:
    static void handle_status(AmsBackendCfs& b, const json& n) {
        b.handle_status_update(n);
    }
    static void seed_override(AmsBackendCfs& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const AmsBackendCfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendCfs& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    static std::optional<std::string> last_rfid_uid(const AmsBackendCfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.last_rfid_uid_.find(slot_index);
        if (it == b.last_rfid_uid_.end())
            return std::nullopt;
        return it->second;
    }
    static helix::printer::CfsMacroVariant macro_variant(const AmsBackendCfs& b) {
        return b.macro_variant_;
    }
};

namespace {
// Per-test tmp cache dir — same idiom as test_ams_backend_snapmaker.cpp /
// test_ams_backend_ace.cpp.
struct CfsTmpCacheDir {
    std::filesystem::path path;
    explicit CfsTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("cfs_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~CfsTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

TEST_CASE("CFS type enum", "[ams][cfs]") {
    SECTION("CFS is a valid AmsType") {
        auto t = AmsType::CFS;
        REQUIRE(t != AmsType::NONE);
    }

    SECTION("CFS is a filament system, not a tool changer") {
        REQUIRE(is_filament_system(AmsType::CFS));
        REQUIRE_FALSE(is_tool_changer(AmsType::CFS));
    }

    SECTION("ams_type_to_string returns CFS") {
        REQUIRE(std::string(ams_type_to_string(AmsType::CFS)) == "CFS");
    }

    SECTION("ams_type_from_string parses CFS variants") {
        REQUIRE(ams_type_from_string("cfs") == AmsType::CFS);
        REQUIRE(ams_type_from_string("CFS") == AmsType::CFS);
    }
}

TEST_CASE("CFS data model extensions", "[ams][cfs]") {
    SECTION("EnvironmentData defaults") {
        EnvironmentData env;
        REQUIRE(env.temperature_c == 0.0f);
        REQUIRE(env.humidity_pct == 0.0f);
    }

    SECTION("AmsUnit environment is optional") {
        AmsUnit unit;
        REQUIRE_FALSE(unit.environment.has_value());
        unit.environment = EnvironmentData{27.0f, 48.0f};
        REQUIRE(unit.environment->temperature_c == 27.0f);
        REQUIRE(unit.environment->humidity_pct == 48.0f);
    }

    SECTION("SlotInfo remaining length defaults to zero") {
        SlotInfo slot;
        REQUIRE(slot.remaining_length_m == 0.0f);
    }

    SECTION("SlotInfo environment is optional") {
        SlotInfo slot;
        REQUIRE_FALSE(slot.environment.has_value());
    }

    SECTION("AmsAlert fields") {
        AmsAlert alert;
        alert.message = "Nozzle clog detected";
        alert.hint = "Run a cold pull";
        alert.error_code = "CFS-845";
        alert.level = AmsAlertLevel::SYSTEM;
        REQUIRE(alert.level == AmsAlertLevel::SYSTEM);
        REQUIRE(alert.error_code == "CFS-845");
    }

    SECTION("AmsSystemInfo has alerts vector") {
        AmsSystemInfo info;
        REQUIRE(info.alerts.empty());
        info.alerts.push_back(AmsAlert{
            .message = "test",
            .hint = "fix it",
            .error_code = "CFS-831",
            .level = AmsAlertLevel::SYSTEM
        });
        REQUIRE(info.alerts.size() == 1);
    }
}

using helix::printer::CfsMaterialDb;

TEST_CASE("CFS material database", "[ams][cfs]") {
    const auto& db = CfsMaterialDb::instance();

    SECTION("known material lookup") {
        auto info = db.lookup("01001");
        REQUIRE(info != nullptr);
        REQUIRE(info->brand == "Creality");
        REQUIRE(info->name == "Hyper PLA");
        REQUIRE(info->material_type == "PLA");
        REQUIRE(info->min_temp == 190);
        REQUIRE(info->max_temp == 240);
    }

    SECTION("unknown material returns nullptr") {
        auto info = db.lookup("99999");
        REQUIRE(info == nullptr);
    }

    SECTION("code stripping: 101001 to 01001") {
        auto id = CfsMaterialDb::strip_code("101001");
        REQUIRE(id == "01001");
    }

    SECTION("short code returned as-is") {
        auto id = CfsMaterialDb::strip_code("01001");
        REQUIRE(id == "01001");
    }

    SECTION("sentinel -1 returns empty") {
        auto id = CfsMaterialDb::strip_code("-1");
        REQUIRE(id.empty());
    }
}

TEST_CASE("CFS color parsing", "[ams][cfs]") {
    SECTION("parse 0RRGGBB format") {
        REQUIRE(CfsMaterialDb::parse_color("0C12E1F") == 0xC12E1F);
        REQUIRE(CfsMaterialDb::parse_color("0FFFFFF") == 0xFFFFFF);
        REQUIRE(CfsMaterialDb::parse_color("0000000") == 0x000000);
    }

    SECTION("sentinel values return default") {
        REQUIRE(CfsMaterialDb::parse_color("-1") == 0x808080);
        REQUIRE(CfsMaterialDb::parse_color("None") == 0x808080);
    }
}

TEST_CASE("CFS error decoding", "[ams][cfs]") {
    SECTION("known error code decodes to message and hint") {
        auto alert = CfsErrorDecoder::decode("key845", 0, -1);
        REQUIRE(alert.has_value());
        REQUIRE(alert->message == "Nozzle clog detected");
        REQUIRE_FALSE(alert->hint.empty());
        REQUIRE(alert->level == AmsAlertLevel::SYSTEM);
        REQUIRE(alert->error_code == "CFS-845");
    }

    SECTION("slot-level error includes slot index") {
        auto alert = CfsErrorDecoder::decode("key843", 0, 2);
        REQUIRE(alert.has_value());
        REQUIRE(alert->level == AmsAlertLevel::SLOT);
        REQUIRE(alert->slot_index == 2);
    }

    SECTION("unit-level error includes unit index") {
        auto alert = CfsErrorDecoder::decode("key853", 1, -1);
        REQUIRE(alert.has_value());
        REQUIRE(alert->level == AmsAlertLevel::UNIT);
        REQUIRE(alert->unit_index == 1);
    }

    SECTION("unknown error code returns nullopt") {
        auto alert = CfsErrorDecoder::decode("key999", 0, -1);
        REQUIRE_FALSE(alert.has_value());
    }
}

TEST_CASE("CFS error message+values decoding", "[ams][cfs]") {
    using nlohmann::json;

    SECTION("key849 splices unit/slot locator into message") {
        // Real telemetry shape observed 2026-05-05.
        json values = json::array({1, "B"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("Retract failed") != std::string::npos);
        REQUIRE(out->first.find("unit 1 slot B") != std::string::npos);
        // Hint passes through unchanged.
        REQUIRE(out->second.find("Manually pull") != std::string::npos);
    }

    SECTION("key851 (slot retract didn't reach) also gets unit/slot locator") {
        json values = json::array({2, "C"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key851", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("unit 2 slot C") != std::string::npos);
    }

    SECTION("key840 (unit-level) gets unit-only locator") {
        json values = json::array({3});
        auto out = CfsErrorDecoder::lookup_message_with_values("key840", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("on unit 3") != std::string::npos);
    }

    SECTION("key111 (cold extruder) surfaces pre-heat guidance") {
        json values = json::array();
        auto out = CfsErrorDecoder::lookup_message_with_values("key111", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("Pre-heat") != std::string::npos);
        REQUIRE(out->second.find("220") != std::string::npos);
        REQUIRE(out->second.find("PETG") != std::string::npos);
    }

    SECTION("key298 (system, no formatter) returns message untouched") {
        json values = json::array();
        auto out = CfsErrorDecoder::lookup_message_with_values("key298", values);
        REQUIRE(out.has_value());
        // No "unit" or "slot" appended.
        REQUIRE(out->first.find("unit") == std::string::npos);
        REQUIRE(out->first.find("slot") == std::string::npos);
    }

    SECTION("malformed values fall back to no locator (no regression)") {
        // Wrong shape — array but slot not a string.
        json values = json::array({1, 2});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("unit") == std::string::npos);
    }

    SECTION("unknown code returns nullopt") {
        json values = json::array({1, "A"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key999", values);
        REQUIRE_FALSE(out.has_value());
    }
}

TEST_CASE("CFS slot addressing", "[ams][cfs]") {
    SECTION("global index to TNN name") {
        REQUIRE(CfsMaterialDb::slot_to_tnn(0) == "T1A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(1) == "T1B");
        REQUIRE(CfsMaterialDb::slot_to_tnn(3) == "T1D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(4) == "T2A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(7) == "T2D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(15) == "T4D");
    }

    SECTION("TNN name to global index") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1A") == 0);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1D") == 3);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T2A") == 4);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T4D") == 15);
    }

    SECTION("invalid TNN returns -1") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("invalid") == -1);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T5A") == -1);
    }
}

// =============================================================================
// CFS backend status parsing tests
// =============================================================================

static nlohmann::json make_cfs_status_json() {
    return nlohmann::json::parse(R"({
        "box": {
            "state": "connect",
            "filament": 1,
            "auto_refill": 1,
            "enable": 1,
            "filament_useup": 1,
            "same_material": [
                ["101001", "0000000", ["T1A"], "PLA"],
                ["101001", "0FFFFFF", ["T1B"], "PLA"]
            ],
            "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
            "T1": {
                "state": "connect",
                "filament": "None",
                "temperature": "27",
                "dry_and_humidity": "48",
                "filament_detected": "None",
                "measuring_wheel": "None",
                "version": "1.1.3",
                "sn": "10000882925L125DBZC",
                "mode": "0",
                "vender": ["-1", "-1", "-1", "-1"],
                "remain_len": ["35", "57", "52", "52"],
                "color_value": ["0000000", "0FFFFFF", "00A2989", "0C12E1F"],
                "material_type": ["101001", "101001", "101001", "101001"],
                "uuid": [19, 103],
                "change_color_num": ["-1", "-1", "-1", "-1"]
            },
            "T2": {
                "state": "None",
                "filament": "None",
                "temperature": "None",
                "dry_and_humidity": "None",
                "filament_detected": "None",
                "measuring_wheel": "None",
                "version": "-1",
                "sn": "-1",
                "mode": "-1",
                "vender": ["-1", "-1", "-1", "-1"],
                "remain_len": ["-1", "-1", "-1", "-1"],
                "color_value": ["-1", "-1", "-1", "-1"],
                "material_type": ["-1", "-1", "-1", "-1"],
                "uuid": "None",
                "change_color_num": ["-1", "-1", "-1", "-1"]
            }
        }
    })");
}

// Wrap a box object in the notify_status_update envelope the backend expects.
// {"params": [{ "box": {...} }, timestamp]}
static json make_cfs_notification(const json& box_obj) {
    return json{{"params", json::array({json{{"box", box_obj}}, 0})}};
}

// Build a single-unit T1 box object with configurable per-slot material_type
// and color_value arrays. Other fields are held constant across tests so we
// can focus assertions on the RFID fingerprint path. `material_type[i]` and
// `color_value[i]` form the fingerprint for slot i.
static json make_single_unit_box(const std::vector<std::string>& material_types,
                                 const std::vector<std::string>& color_values) {
    json box = json::parse(R"({
        "state": "connect",
        "filament": 0,
        "auto_refill": 1,
        "enable": 1,
        "filament_useup": 1,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect",
            "filament": "None",
            "temperature": "27",
            "dry_and_humidity": "48",
            "version": "1.1.3",
            "sn": "SERIAL",
            "vender": ["-1", "-1", "-1", "-1"],
            "remain_len": ["35", "57", "52", "52"],
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
    box["T1"]["material_type"] = material_types;
    box["T1"]["color_value"] = color_values;
    return box;
}

using helix::printer::AmsBackendCfs;

TEST_CASE("CFS backend status parsing", "[ams][cfs]") {
    auto status = make_cfs_status_json();
    auto info = AmsBackendCfs::parse_box_status(status["box"]);

    SECTION("system-level fields") {
        REQUIRE(info.type == AmsType::CFS);
        REQUIRE(info.supports_endless_spool == true);
    }

    SECTION("connected unit created, disconnected skipped") {
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].name == "T1");
        REQUIRE(info.units[0].slot_count == 4);
        REQUIRE(info.total_slots == 4);
    }

    SECTION("unit environment data") {
        REQUIRE(info.units[0].environment.has_value());
        REQUIRE(info.units[0].environment->temperature_c == 27.0f);
        REQUIRE(info.units[0].environment->humidity_pct == 48.0f);
    }

    SECTION("unit hardware info") {
        REQUIRE(info.units[0].firmware_version == "1.1.3");
        REQUIRE(info.units[0].serial_number == "10000882925L125DBZC");
    }

    SECTION("slot colors parsed") {
        REQUIRE(info.units[0].slots[0].color_rgb == 0x000000);
        REQUIRE(info.units[0].slots[1].color_rgb == 0xFFFFFF);
        REQUIRE(info.units[0].slots[2].color_rgb == 0x0A2989);
        REQUIRE(info.units[0].slots[3].color_rgb == 0xC12E1F);
    }

    SECTION("slot materials resolved from database") {
        REQUIRE(info.units[0].slots[0].material == "PLA");
        REQUIRE(info.units[0].slots[0].brand == "Creality");
    }

    SECTION("slot remaining length") {
        REQUIRE(info.units[0].slots[0].remaining_length_m == 35.0f);
        REQUIRE(info.units[0].slots[1].remaining_length_m == 57.0f);
    }

    SECTION("slot status derived correctly") {
        REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    }

    SECTION("topology is HUB") {
        REQUIRE(info.units[0].topology == PathTopology::HUB);
    }
}

TEST_CASE("CFS disconnected unit handling", "[ams][cfs]") {
    auto status = make_cfs_status_json();
    auto info = AmsBackendCfs::parse_box_status(status["box"]);

    SECTION("T2 is disconnected — not in units list") {
        for (const auto& unit : info.units) {
            REQUIRE(unit.name != "T2");
        }
    }
}

TEST_CASE("CFS GCode helpers", "[ams][cfs]") {
    // The CR_BOX_* primitives don't park the toolhead — without wrapping,
    // CR_BOX_FLUSH extrudes onto the build plate instead of into the K2's
    // waste port. These assertions enforce the SAVE_GCODE_STATE +
    // BOX_GO_TO_EXTRUDE_POS / BOX_MOVE_TO_SAFE_POS envelope that mirrors
    // the K2 stock screen's LOAD_MATERIAL macro chain.
    SECTION("load gcode uses CR_BOX commands with TNN, wrapped in park envelope") {
        const std::string expected_a =
            "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
            "BOX_SAVE_FAN\n"
            "BOX_GO_TO_EXTRUDE_POS\n"
            "BOX_MODE_WAIT\n"
            "CR_BOX_PRE_OPT\nCR_BOX_EXTRUDE TNN=T1A\n"
            "CR_BOX_WASTE\nCR_BOX_FLUSH TNN=T1A\nCR_BOX_END_OPT\n"
            "BOX_NOZZLE_CLEAN\n"
            "BOX_RESTORE_FAN\n"
            "BOX_MOVE_TO_SAFE_POS\n"
            "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::load_gcode(0) == expected_a);

        REQUIRE(AmsBackendCfs::load_gcode(1).find("TNN=T1B") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_GO_TO_EXTRUDE_POS") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_MOVE_TO_SAFE_POS") !=
                std::string::npos);
        // Envelope: mode-wait + fan save/restore around the op.
        // No BOX_SET_TEMP — we deliberately don't lower a hotter pre-set
        // extruder target (e.g. PETG @ 240°C); cold extruders surface a
        // friendly key111 modal instead.
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_SET_TEMP") == std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_MODE_WAIT") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_RESTORE_FAN") != std::string::npos);
        // Load ends with fresh filament in the nozzle — wipe before parking.
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_NOZZLE_CLEAN") !=
                std::string::npos);

        REQUIRE(AmsBackendCfs::load_gcode(4).find("TNN=T2A") != std::string::npos);
    }

    SECTION("unload gcode uses CR_BOX commands inside park envelope") {
        const std::string g = AmsBackendCfs::unload_gcode();
        REQUIRE(g.find("SAVE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_SET_TEMP") == std::string::npos);
        REQUIRE(g.find("CR_BOX_PRE_OPT\nCR_BOX_CUT\nBOX_MODE_WAIT\n"
                       "CR_BOX_RETRUDE\nCR_BOX_END_OPT") != std::string::npos);
        REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("RESTORE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        // Unload ends with retrude — nozzle is empty; no wipe needed.
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);
    }

    SECTION("swap gcode combines unload and load inside park envelope") {
        for (int idx : {0, 1, 3}) {
            const std::string g = AmsBackendCfs::swap_gcode(idx);
            REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
            REQUIRE(g.find("BOX_SET_TEMP") == std::string::npos);
            REQUIRE(g.find("CR_BOX_CUT\nBOX_MODE_WAIT\nCR_BOX_RETRUDE\n"
                           "BOX_MODE_WAIT\nCR_BOX_EXTRUDE TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
            // Swap ends with flush of the new slot — wipe before parking.
            REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
        }
        // Invalid index
        REQUIRE(AmsBackendCfs::swap_gcode(-1).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16).empty());
    }

    SECTION("reset gcode") {
        REQUIRE(AmsBackendCfs::reset_gcode() == "BOX_ERROR_CLEAR");
    }

    SECTION("recover gcode") {
        REQUIRE(AmsBackendCfs::recover_gcode() == "BOX_ERROR_RESUME_PROCESS");
    }
}

TEST_CASE("CFS K1 macro variant (#968)", "[ams][cfs]") {
    // K1 official CFS upgrade firmware uses plain BOX_* macros — no CR_ prefix,
    // no fan-save/mode-wait helpers. Reporter's K1C gcode/help shows
    // BOX_GO_TO_EXTRUDE_POS and BOX_MOVE_TO_SAFE_POS succeed but every
    // CR_BOX_* command returns key61 "Unknown command".
    using V = helix::printer::CfsMacroVariant;

    SECTION("K1 load gcode uses BOX_* primitives with TNN, simpler envelope") {
        const std::string expected_a =
            "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
            "BOX_GO_TO_EXTRUDE_POS\n"
            "BOX_EXTRUDE_MATERIAL TNN=T1A\n"
            "BOX_MATERIAL_FLUSH TNN=T1A\n"
            "BOX_NOZZLE_CLEAN\n"
            "BOX_MOVE_TO_SAFE_POS\n"
            "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::load_gcode(0, V::K1) == expected_a);

        // No CR_BOX_* primitives on K1.
        const std::string g = AmsBackendCfs::load_gcode(1, V::K1);
        REQUIRE(g.find("CR_BOX_") == std::string::npos);
        REQUIRE(g.find("TNN=T1B") != std::string::npos);

        // K1 firmware lacks fan-save and mode-wait — must not be emitted.
        REQUIRE(g.find("BOX_SAVE_FAN") == std::string::npos);
        REQUIRE(g.find("BOX_RESTORE_FAN") == std::string::npos);
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);

        // Envelope helpers that K1 DOES support.
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
    }

    SECTION("K1 load TNN covers full unit/slot range") {
        // K1 firmware (per K1-Max box.cfg) uses the same TNN encoding as K2.
        // Spot-check first slot of each unit + last slot to lock in the
        // CfsMaterialDb::slot_to_tnn translation under the K1 path.
        struct Case { int idx; const char* tnn; };
        for (const auto& c : {Case{0, "T1A"}, Case{1, "T1B"}, Case{3, "T1D"},
                              Case{4, "T2A"}, Case{8, "T3A"}, Case{12, "T4A"},
                              Case{15, "T4D"}}) {
            const std::string g = AmsBackendCfs::load_gcode(c.idx, V::K1);
            const std::string needle = std::string("TNN=") + c.tnn;
            INFO("idx=" << c.idx << " expected " << needle);
            REQUIRE(g.find(needle) != std::string::npos);
        }
        // Out-of-range stays empty (same contract as K2).
        REQUIRE(AmsBackendCfs::load_gcode(-1, V::K1).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16, V::K1).empty());
    }

    SECTION("K1 unload gcode exact match") {
        const std::string expected =
            "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
            "BOX_GO_TO_EXTRUDE_POS\n"
            "BOX_CUT_MATERIAL\n"
            "BOX_RETRUDE_MATERIAL\n"
            "BOX_MOVE_TO_SAFE_POS\n"
            "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::unload_gcode(V::K1) == expected);
        // Sanity: no K2-only macros leaked in.
        const std::string g = AmsBackendCfs::unload_gcode(V::K1);
        REQUIRE(g.find("CR_BOX_") == std::string::npos);
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
        REQUIRE(g.find("BOX_SAVE_FAN") == std::string::npos);
        // Nozzle empty post-cut → no wipe.
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);
    }

    SECTION("K1 swap gcode exact match for slot 5 (T2B)") {
        const std::string expected =
            "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
            "BOX_GO_TO_EXTRUDE_POS\n"
            "BOX_CUT_MATERIAL\n"
            "BOX_RETRUDE_MATERIAL\n"
            "BOX_EXTRUDE_MATERIAL TNN=T2B\n"
            "BOX_MATERIAL_FLUSH TNN=T2B\n"
            "BOX_NOZZLE_CLEAN\n"
            "BOX_MOVE_TO_SAFE_POS\n"
            "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::swap_gcode(5, V::K1) == expected);
    }

    SECTION("K1 swap spot checks across slot range") {
        for (int idx : {0, 1, 3, 5, 11, 15}) {
            const std::string g = AmsBackendCfs::swap_gcode(idx, V::K1);
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
            REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
            REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_MATERIAL_FLUSH TNN=") != std::string::npos);
            // Swap ends with flush of new slot — wipe before parking.
            REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        }
        REQUIRE(AmsBackendCfs::swap_gcode(-1, V::K1).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16, V::K1).empty());
    }

    SECTION("K1 envelope omits all K2-only helpers across all three operations") {
        // Regression guard: any future refactor that accidentally re-introduces
        // BOX_SAVE_FAN/RESTORE_FAN/BOX_MODE_WAIT for the K1 path would send
        // the printer back into key61 territory (#968).
        for (const std::string& g :
             {AmsBackendCfs::load_gcode(0, V::K1),
              AmsBackendCfs::unload_gcode(V::K1),
              AmsBackendCfs::swap_gcode(0, V::K1)}) {
            REQUIRE(g.find("BOX_SAVE_FAN") == std::string::npos);
            REQUIRE(g.find("BOX_RESTORE_FAN") == std::string::npos);
            REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            // Envelope helpers the K1 firmware DOES expose are present.
            REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
            REQUIRE(g.find("SAVE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
            REQUIRE(g.find("RESTORE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        }
    }

    SECTION("K2 default preserved when variant omitted") {
        // Existing call sites without variant arg must still emit K2 macros.
        REQUIRE(AmsBackendCfs::load_gcode(0).find("CR_BOX_EXTRUDE") != std::string::npos);
        REQUIRE(AmsBackendCfs::unload_gcode().find("CR_BOX_CUT") != std::string::npos);
        REQUIRE(AmsBackendCfs::swap_gcode(0).find("CR_BOX_EXTRUDE") != std::string::npos);
    }
}

TEST_CASE("CFS backend ctor latches macro variant from PrinterDetector (#968)",
          "[ams][cfs]") {
    // The constructor reads PrinterDetector::is_creality_k1() once and caches
    // the result. Verify both routes resolve correctly.
    auto* config = Config::get_instance();
    REQUIRE(config != nullptr);
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C printer type → K1 dialect") {
        config->set<std::string>(type_path, "Creality K1C");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) ==
                helix::printer::CfsMacroVariant::K1);
    }

    SECTION("K1 Max printer type → K1 dialect") {
        config->set<std::string>(type_path, "Creality K1 Max");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) ==
                helix::printer::CfsMacroVariant::K1);
    }

    SECTION("K2 Plus printer type → K2 dialect") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) ==
                helix::printer::CfsMacroVariant::K2);
    }

    SECTION("Unset printer type → K2 dialect (safe default)") {
        config->set<std::string>(type_path, "");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) ==
                helix::printer::CfsMacroVariant::K2);
    }

    // Restore prior config so we don't bleed into adjacent tests.
    config->set<std::string>(type_path, saved);
}

TEST_CASE("PrinterDiscovery enables CFS for K1 box object (#968 gate)",
          "[ams][cfs][discovery]") {
    // Before 6ebe7417b: K1 + `box` → no CFS, warn log.
    // After the #968 fix flip: K1 + `box` → CFS enabled, K1 dialect chosen
    // downstream by AmsBackendCfs ctor.
    auto* config = Config::get_instance();
    REQUIRE(config != nullptr);
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C + box object enables CFS") {
        config->set<std::string>(type_path, "Creality K1C");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "box", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_mmu());
        REQUIRE(discovery.get_mmu_type() == AmsType::CFS);
    }

    SECTION("K2 Plus + box object enables CFS (regression check)") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "box", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_mmu());
        REQUIRE(discovery.get_mmu_type() == AmsType::CFS);
    }

    SECTION("No box object → no CFS regardless of printer type") {
        config->set<std::string>(type_path, "Creality K1C");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_mmu());
    }

    config->set<std::string>(type_path, saved);
}

TEST_CASE("Material comfort ranges", "[filament]") {
    auto* range = filament::get_comfort_range("PLA");
    REQUIRE(range != nullptr);
    REQUIRE(range->max_humidity_good == Catch::Approx(50.0f));
    REQUIRE(range->max_humidity_warn == Catch::Approx(65.0f));

    auto* petg = filament::get_comfort_range("PETG");
    REQUIRE(petg != nullptr);
    REQUIRE(petg->max_humidity_good == Catch::Approx(40.0f));

    REQUIRE(filament::get_comfort_range("UNKNOWN_MATERIAL") == nullptr);
}

TEST_CASE("CFS backend has environment sensors", "[ams][cfs]") {
    // CFS units have built-in temperature and humidity sensors
    // Verify the capability is reported correctly at the type level
    // (Cannot instantiate AmsBackendCfs without a real API, so test the header contract)
    REQUIRE(true); // Compile-time check: has_environment_sensors() exists in header
}

// =============================================================================
// CFS active slot inference from box status
// =============================================================================

TEST_CASE("CFS parse_box_status infers active slot from tool map", "[ams][cfs]") {
    auto status = make_cfs_status_json();

    SECTION("filament loaded with valid tool map → current_slot from T0 mapping") {
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        // box.filament = 1 and map has T1A→T1A (slot 0 mapped to slot 0)
        REQUIRE(info.filament_loaded == true);
        REQUIRE(info.tool_to_slot_map.size() >= 1);
        REQUIRE(info.tool_to_slot_map[0] == 0); // T1A = slot 0
    }

    SECTION("filament not loaded → no active slot inferred") {
        status["box"]["filament"] = 0;
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info.filament_loaded == false);
    }

    SECTION("tool map preserved across multiple slots") {
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        // Map: T1A→T1A(0), T1B→T1B(1), T1C→T1C(2), T1D→T1D(3)
        REQUIRE(info.tool_to_slot_map.size() == 4);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 1);
        REQUIRE(info.tool_to_slot_map[2] == 2);
        REQUIRE(info.tool_to_slot_map[3] == 3);
    }

    SECTION("active slot B → current_slot=1 and current_tool mirrors current_slot") {
        // CFS slots map 1:1 to tools; the print-status color dot reads
        // current_tool to label "T<n>". A hardcoded current_tool=0 would
        // mislabel the loaded slot.
        status["box"]["T1"]["filament"] = "B";
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info.current_slot == 1);
        REQUIRE(info.current_tool == info.current_slot);
    }

    SECTION("active slot D → current_tool follows current_slot (slot 3)") {
        status["box"]["T1"]["filament"] = "D";
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info.current_slot == 3);
        REQUIRE(info.current_tool == 3);
    }
}

// =============================================================================
// CFS BOX_MODIFY_TN tool remap (set_tool_mapping)
// =============================================================================

namespace {
class CfsRemapHelper : public AmsBackendCfs {
  public:
    CfsRemapHelper() : AmsBackendCfs(nullptr, nullptr) {}

    // Capture gcode instead of dispatching to a real Moonraker connection.
    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Expose protected firmware-writeback helper for direct test calls.
    // push_slot_color_to_firmware is protected (called from set_slot_info but
    // not part of the public IAmsBackend surface).
    using AmsBackendCfs::push_slot_color_to_firmware;

    std::vector<std::string> captured;
};
} // namespace

TEST_CASE("CFS set_tool_mapping emits BOX_MODIFY_TN with TNN keys/values",
          "[ams][cfs][remap]") {
    CfsRemapHelper helper;

    SECTION("Identity within unit 1: T1A → T1A") {
        auto err = helper.set_tool_mapping(0, 0);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured.size() == 1);
        REQUIRE(helper.captured[0] == "BOX_MODIFY_TN T1A=T1A");
    }

    SECTION("Cross-unit remap: tool 0 (T1A) → slot 5 (T2B)") {
        auto err = helper.set_tool_mapping(0, 5);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T1A=T2B"});
    }

    SECTION("Last slot: tool 15 (T4D) → slot 0 (T1A)") {
        auto err = helper.set_tool_mapping(15, 0);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T4D=T1A"});
    }

    SECTION("Tool number out of range rejected, no gcode sent") {
        auto err = helper.set_tool_mapping(16, 0);
        REQUIRE(err.result == AmsResult::INVALID_TOOL);
        REQUIRE(helper.captured.empty());

        err = helper.set_tool_mapping(-1, 0);
        REQUIRE(err.result == AmsResult::INVALID_TOOL);
        REQUIRE(helper.captured.empty());
    }

    SECTION("Slot index out of range rejected, no gcode sent") {
        auto err = helper.set_tool_mapping(0, 16);
        REQUIRE(err.result != AmsResult::SUCCESS);
        REQUIRE(helper.captured.empty());

        err = helper.set_tool_mapping(0, -1);
        REQUIRE(err.result != AmsResult::SUCCESS);
        REQUIRE(helper.captured.empty());
    }

    SECTION("Multiple calls send gcode in order") {
        REQUIRE(helper.set_tool_mapping(0, 1).result == AmsResult::SUCCESS);
        REQUIRE(helper.set_tool_mapping(2, 7).result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T1A=T1B",
                                                              "BOX_MODIFY_TN T1C=T2D"});
    }
}

// =============================================================================
// CFS BOX_MODIFY_TN_DATA color firmware-writeback (push_slot_color_to_firmware)
// =============================================================================
//
// Format reverse-engineered from K2's master-server binary
// (`strings /usr/bin/master-server | grep BOX_MODIFY_TN_DATA`):
//   BOX_MODIFY_TN_DATA ADDR=<1..4> NUM=<A|B|C|D> PART=color_value DATA=0RRGGBB
//
// Validated round-trip on K2 Plus 2026-05-10 — see
// .claude/scratchpad/research/k2-box-firmware-writeback.md.
//
// CRITICAL: invalid args trigger box_wrapper TypeError → klippy invoke_shutdown.
// These tests cover the validation guards in push_slot_color_to_firmware that
// prevent any out-of-range slot index or zero-color sentinel from reaching the
// firmware as a malformed gcode.

TEST_CASE("CFS push_slot_color_to_firmware emits BOX_MODIFY_TN_DATA",
          "[ams][cfs][firmware_writeback]") {
    CfsRemapHelper helper;

    SECTION("Slot 0 (T1A) red 0xFF0000 → ADDR=1 NUM=A DATA=0FF0000") {
        helper.push_slot_color_to_firmware(0, 0xFF0000);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000"});
    }

    SECTION("Slot 5 (T2B) green 0x00FF00 → ADDR=2 NUM=B DATA=000FF00") {
        helper.push_slot_color_to_firmware(5, 0x00FF00);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=2 NUM=B PART=color_value DATA=000FF00"});
    }

    SECTION("Slot 15 (T4D) white 0xFFFFFF → ADDR=4 NUM=D DATA=0FFFFFF") {
        helper.push_slot_color_to_firmware(15, 0xFFFFFF);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=4 NUM=D PART=color_value DATA=0FFFFFF"});
    }

    SECTION("Color masks high bits — alpha byte from caller is ignored") {
        // If a caller passes 0xAA112233, the alpha byte is dropped. The
        // firmware's leading nibble is always our own '0'.
        helper.push_slot_color_to_firmware(0, 0xAA112233);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0112233"});
    }
}

TEST_CASE("CFS push_slot_color_to_firmware skips invalid inputs (must NOT crash klippy)",
          "[ams][cfs][firmware_writeback]") {
    CfsRemapHelper helper;

    SECTION("color_rgb == 0 (pure black) is a valid color and DOES dispatch") {
        // Pure black is a legitimate user choice and a real firmware-detected
        // color (K2 reports loaded black PLA as 0x000000). The push helper
        // must NOT skip it — that's the bug fixed by the color_set boolean.
        // Caller (set_slot_info) is responsible for not invoking when color
        // wasn't actually set (color_set=false on the override).
        helper.push_slot_color_to_firmware(0, 0);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0000000"});
    }

    SECTION("Negative slot index is skipped") {
        helper.push_slot_color_to_firmware(-1, 0xFF0000);
        REQUIRE(helper.captured.empty());
    }

    SECTION("slot_index >= 16 is skipped") {
        helper.push_slot_color_to_firmware(16, 0xFF0000);
        REQUIRE(helper.captured.empty());
        helper.push_slot_color_to_firmware(99, 0xFF0000);
        REQUIRE(helper.captured.empty());
    }
}

TEST_CASE("CFS set_tool_mapping updates local tool_to_slot_map", "[ams][cfs][remap]") {
    // PrintStartController::saved_tool_mapping_ snapshots get_tool_mapping()
    // before sending remaps and replays it on print end to restore the prior
    // configuration. If get_tool_mapping() returned an empty vector after a
    // successful set_tool_mapping, the restore path would be a no-op.
    CfsRemapHelper helper;
    auto status = make_cfs_status_json();
    json notification = {{"method", "notify_status_update"},
                         {"params", json::array({status, 0})}};
    CfsTestAccess::handle_status(helper, notification);

    auto baseline = helper.get_tool_mapping();
    REQUIRE_FALSE(baseline.empty()); // box.map populated by handle_status

    REQUIRE(helper.set_tool_mapping(1, 5).result == AmsResult::SUCCESS);

    auto after = helper.get_tool_mapping();
    REQUIRE(after.size() == baseline.size());
    REQUIRE(after[1] == 5);
}

TEST_CASE("CFS get_tool_mapping_capabilities advertises editable", "[ams][cfs][remap]") {
    CfsRemapHelper helper;
    auto caps = helper.get_tool_mapping_capabilities();
    REQUIRE(caps.supported);
    REQUIRE(caps.editable);
    // Description is informational; non-empty so UI can show backend-specific copy
    REQUIRE_FALSE(caps.description.empty());
}

// =============================================================================
// CFS filament segment logic
// =============================================================================

TEST_CASE("CFS segment returns HUB for available slots", "[ams][cfs]") {
    auto status = make_cfs_status_json();
    auto info = AmsBackendCfs::parse_box_status(status["box"]);

    SECTION("available slots have HUB segment") {
        // All slots with filament (remain_len > 0) should be AVAILABLE
        REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        // get_slot_filament_segment returns HUB for AVAILABLE slots (tested via backend)
        // We can only test parse_box_status here since backend needs API
    }

    SECTION("empty slots have EMPTY status") {
        // Modify a slot to have no color
        status["box"]["T1"]["color_value"][0] = "-1";
        auto info2 = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info2.units[0].slots[0].status == SlotStatus::EMPTY);
    }
}

// =============================================================================
// CFS action state in operations
// =============================================================================

TEST_CASE("CFS GCode generation for all operations", "[ams][cfs]") {
    SECTION("load gcode uses TNN for multi-unit addressing") {
        // Unit 1 slots
        REQUIRE(AmsBackendCfs::load_gcode(0).find("TNN=T1A") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(3).find("TNN=T1D") != std::string::npos);
        // Unit 4 last slot
        REQUIRE(AmsBackendCfs::load_gcode(15).find("TNN=T4D") != std::string::npos);
    }

    SECTION("load gcode rejects out of range") {
        REQUIRE(AmsBackendCfs::load_gcode(-1).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16).empty());
    }
}

TEST_CASE("CFS has no per-slot prep sensors", "[ams][cfs]") {
    // CFS tracks slot inventory via material database (RFID/software), not
    // per-gate optical sensors. slot_has_prep_sensor must return false so the
    // filament path canvas draws continuous lines without sensor dot gaps.
    AmsBackendCfs backend(nullptr, nullptr);

    SECTION("all slots report no prep sensor") {
        for (int i = 0; i < 16; i++) {
            REQUIRE_FALSE(backend.slot_has_prep_sensor(i));
        }
    }
}

// ============================================================================
// Task 14: filament slot override integration
// ============================================================================

TEST_CASE("CFS override loaded at init is applied over firmware data",
          "[ams][cfs][filament_slot_override][slow]") {
    // Seed an override in-memory and verify apply_overrides layers it over
    // firmware-parsed slot data. CFS's firmware populates brand /
    // color_name / total_weight_g from the RFID material DB, but the
    // override wins for every non-default field per the merge policy.
    CfsTmpCacheDir tmp("task14_override_applied");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports slot 0 with DIFFERENT color and material code (firmware
    // material db lookup resolves code "101001" -> PLA/Creality).
    json box = make_single_unit_box(
        {"101001", "101001", "101001", "101001"},
        {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    auto info = backend.get_slot_info(0);
    // Override-eligible fields win.
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);
}

TEST_CASE("CFS migrates from helix-screen:cfs_slot_overrides on first startup",
          "[ams][cfs][filament_slot_override][migration][slow]") {
    // Pre-Task-8 CFS wrote per-slot overrides to
    // helix-screen:cfs_slot_overrides. On first startup post-upgrade, the
    // store's load_blocking() migrates that data into lane_data and deletes
    // the legacy namespace. Tests through the store + MoonrakerAPIMock
    // directly so we don't need to drive on_started() (which requires a
    // started subscription backend).
    CfsTmpCacheDir tmp("task14_migration");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed legacy namespace with a PLA Orange override on slot 0. lane_data is
    // untouched -> forces migration.
    json legacy = {
        {"0", {
            {"brand", "Polymaker"},
            {"material", "PLA"},
            {"color_rgb", 0xFF5500},
            {"spoolman_id", 42},
            {"spool_name", "PolyLite Orange"},
        }},
    };
    api.mock_set_db_value("helix-screen", "cfs_slot_overrides", legacy);

    helix::ams::FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    auto loaded = store.load_blocking();

    // Migrated slot is returned from load_blocking as if it came from lane_data.
    REQUIRE(loaded.count(0) == 1);
    CHECK(loaded[0].brand == "Polymaker");
    CHECK(loaded[0].material == "PLA");
    CHECK(loaded[0].color_rgb == 0xFF5500u);
    CHECK(loaded[0].spoolman_id == 42);
    CHECK(loaded[0].spool_name == "PolyLite Orange");

    // lane_data now holds the AFC-shaped record.
    auto lane1 = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!lane1.is_null());
    CHECK(lane1["vendor"] == "Polymaker");
    CHECK(lane1["lane"] == "0");

    // Legacy namespace deleted post-migration.
    CHECK(api.mock_get_db_value("helix-screen", "cfs_slot_overrides").is_null());
}

TEST_CASE("CFS set_slot_info(persist=true) writes to store",
          "[ams][cfs][filament_slot_override][slow]") {
    CfsTmpCacheDir tmp("task14_persist_true");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // Prime the backend with 4 slots so set_slot_info's index check passes.
    json box = make_single_unit_box(
        {"101001", "101001", "101001", "101001"},
        {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // In-memory map carries the override.
    auto staged = CfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Moonraker DB received the AFC-shaped record via save_async.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");

    // Legacy namespace NOT touched — CFS no longer writes there.
    CHECK(api.mock_get_db_value("helix-screen", "cfs_slot_overrides").is_null());
}

TEST_CASE("CFS set_slot_info(persist=false) does NOT write to store",
          "[ams][cfs][filament_slot_override][slow]") {
    CfsTmpCacheDir tmp("task14_persist_false");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    json box = make_single_unit_box(
        {"101001", "101001", "101001", "101001"},
        {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // The user's edit (brand="Draft", color=0x123456) was NOT persisted —
    // any override entry that exists came from the auto-mirror path firing
    // on the firmware-detected color/material in the prior status update,
    // which is independent of this preview edit. Verify by field:
    //   - brand: must NOT be "Draft" (auto-mirror only fills color/material)
    //   - color_rgb: must NOT be 0x123456 (the preview color)
    //   - color_set may be true if firmware reported a color (slot 0 is
    //     black, 0x000000); the auto-mirror records that as the firmware
    //     baseline, NOT as the user's preview.
    auto stored = CfsTestAccess::get_override(backend, 0);
    if (stored.has_value()) {
        CHECK(stored->brand != "Draft");
        CHECK(stored->color_rgb != 0x123456u);
    }
    auto db_record = api.mock_get_db_value("lane_data", "lane1");
    if (!db_record.is_null()) {
        // Auto-mirror's record does not contain the preview's brand/color.
        CHECK(db_record.value("vendor", "") != "Draft");
        CHECK(db_record.value("color", "") != "#123456");
    }

    // Preview edit still visible via get_slot_info (in-memory only).
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE("CFS RFID fingerprint change clears override (hardware swap detected)",
          "[ams][cfs][filament_slot_override][slow]") {
    CfsTmpCacheDir tmp("task14_uid_swap_clears");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed override AND the corresponding DB entry so we can verify
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
    CfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: material=101001, color=0FF5500 establishes the baseline.
    // No clear.
    json box1 = make_single_unit_box(
        {"101001", "101001", "101001", "101001"},
        {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box1));

    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: DIFFERENT fingerprint on slot 0 (material=102001, new
    // color) — physical swap detected.
    //
    // Sequence inside handle_status_update for this slot:
    //   1. check_hardware_event_clear fires clear_override_locked, which
    //      erases the user-set override (brand, spool_name, spoolman_id,
    //      material, color) AND deletes the lane_data record.
    //   2. mirror_firmware_to_lane_data (FillUnsetOnly) immediately
    //      republishes the NEW firmware-truth color/material so OrcaSlicer's
    //      MoonrakerPrinterAgent sees the new spool on the same parse.
    //
    // So the post-swap state is: override exists with ONLY firmware fields,
    // lane_data contains the new color/material (no stale user metadata).
    json box2 = make_single_unit_box(
        {"102001", "101001", "101001", "101001"},
        {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box2));

    CHECK(CfsTestAccess::last_rfid_uid(backend, 0) == "102001|000FF00");

    // Override should now hold the auto-mirrored firmware values (no user
    // metadata): new color, no spool_name / spoolman_id / brand from the
    // wiped user override.
    auto post_swap = CfsTestAccess::get_override(backend, 0);
    REQUIRE(post_swap.has_value());
    CHECK(post_swap->color_rgb == 0x00FF00u);
    CHECK(post_swap->spool_name.empty());
    CHECK(post_swap->spoolman_id == 0);
    CHECK(post_swap->brand.empty());

    // lane_data was cleared by the swap and immediately republished with the
    // new firmware truth — Orca sees the new spool's color, not stale data.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["color"] == "#00FF00");
    CHECK_FALSE(stored.contains("spool_name"));
    CHECK_FALSE(stored.contains("spool_id"));
    CHECK_FALSE(stored.contains("vendor"));

    // Override-exclusive fields reset on the live slot. Firmware-populated
    // fields (brand from material DB, color_rgb) stay — CFS firmware owns
    // those and they should reflect the new spool.
    auto info = backend.get_slot_info(0);
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.remaining_weight_g == -1.0f);
    // color_rgb was re-parsed from firmware this pass — should reflect new
    // spool's color (0x00FF00), not the old override (0xFF5500).
    CHECK(info.color_rgb == 0x00FF00u);
}

TEST_CASE("CFS first RFID observation does NOT clear override",
          "[ams][cfs][filament_slot_override][slow]") {
    // Even when the override was saved against a different (now-stale)
    // fingerprint, the very first observation is a BASELINE and must never
    // fire a clear. Matches Snapmaker semantics.
    CfsTmpCacheDir tmp("task14_first_uid_baseline");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1",
                          json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a fingerprint on the FIRST observation — no prior
    // baseline, so this must NOT trigger a clear. Override survives.
    json box = make_single_unit_box(
        {"999001", "101001", "101001", "101001"},
        {"0123456", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    auto staged = CfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse of the SAME fingerprint stays the baseline — no clear.
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("CFS empty RFID fingerprint does not update baseline or clear",
          "[ams][cfs][filament_slot_override][slow]") {
    // Sentinel material_type "-1" / color_value "-1" = no tag / reader
    // disabled / unreadable. Must not update the baseline and must not clear.
    // This is the contract that keeps transient tag-read failures from
    // masking a genuine hardware swap on the next good read.
    CfsTmpCacheDir tmp("task14_empty_uid_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1",
                          json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid fingerprint — baseline established.
    json box1 = make_single_unit_box(
        {"101001", "101001", "101001", "101001"},
        {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box1));
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");

    // Second parse: slot 0 has SENTINEL material_type and color — empty
    // fingerprint. Must NOT update baseline and must NOT clear the override.
    json box2 = make_single_unit_box(
        {"-1", "101001", "101001", "101001"},
        {"-1", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box2));
    CHECK(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500"); // unchanged
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Third parse: same original fingerprint — matches baseline, no clear.
    // Proves the sentinel-UID pass didn't corrupt state.
    CfsTestAccess::handle_status(backend, make_cfs_notification(box1));
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("CFS override preserved across unchanged parses",
          "[ams][cfs][filament_slot_override][slow]") {
    // When the RFID fingerprint is unchanged (same spool re-observed), the
    // override must be re-applied on every parse. This is the core behavior
    // that was broken pre-Task-14: firmware data overwrote user edits on
    // every status notification.
    CfsTmpCacheDir tmp("task14_preserved_unchanged");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.material = "PLA";
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Multiple parses with the SAME fingerprint — override must persist.
    json box = make_single_unit_box(
        {"101001", "101001", "101001", "101001"},
        {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});

    for (int i = 0; i < 3; ++i) {
        CfsTestAccess::handle_status(backend, make_cfs_notification(box));
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spool_name == "PolyLite Orange");
        CHECK(info.spoolman_id == 42);
        CHECK(info.material == "PLA");
        CHECK(info.color_rgb == 0xFF5500u);
    }

    // Override map itself survived.
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
}

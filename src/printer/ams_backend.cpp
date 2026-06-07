// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#ifdef HELIX_ENABLE_MOCKS
#include "ams_backend_mock.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"
#endif
#if HELIX_HAS_IFS
#include "ams_backend_ad5x_ifs.h"
#endif
#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
#endif
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_backend_ace.h"
#include "ams_backend_medusahc.h"
#include "filament_database.h"
#include "moonraker_api.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

using namespace helix;

std::string AmsBackend::normalize_material(const std::string& material) const {
    auto supported = get_supported_materials();
    if (!supported || supported->empty()) {
        return material;
    }
    const auto& list = *supported;

    // Case-insensitive lowercase helper.
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };

    // (2) Case-insensitive exact match against the whitelist.
    const std::string input_lc = lower(material);
    for (const auto& s : list) {
        if (lower(s) == input_lc) {
            return s;
        }
    }

    // (3) Firmware-specific aliases (backends override get_material_aliases()
    //     to handle names the shared filament DB groups differently than
    //     firmware does — e.g., "Silk PLA" -> "SILK" on AD5X).
    for (const auto& [alias, target] : get_material_aliases()) {
        if (lower(alias) == input_lc) {
            return target;
        }
    }

    // (4) compat_group match via the filament database.
    auto info = filament::find_material(material);
    if (info.has_value() && info->compat_group != nullptr) {
        std::string_view group(info->compat_group);
        for (const auto& s : list) {
            auto s_info = filament::find_material(s);
            if (s_info.has_value() && s_info->compat_group != nullptr &&
                std::string_view(s_info->compat_group) == group) {
                return s;
            }
        }
    }

    // (5) Fallback: first whitelist entry (typically the safest / most common).
    return list.front();
}

#ifdef HELIX_ENABLE_MOCKS
// Helper: lowercase a string for case-insensitive comparison
static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Helper to create mock backend with optional features.
//
// `mock_client` (when non-null) is the MoonrakerClientMock driving the
// simulated printer. The AMS mock subscribes to its active-gcode-tool
// notifications so the active-tool indicator follows the gcode (mock-side
// proxy for production's printer.mmu.tool / toolchanger.tool_number).
static std::unique_ptr<AmsBackendMock>
create_mock_with_features(int gate_count, MoonrakerClient* mock_client = nullptr) {
    auto mock = std::make_unique<AmsBackendMock>(gate_count);

    // Find the moonraker mock to subscribe to. Caller may pass it explicitly;
    // otherwise fall back to the global registered by MoonrakerManager. The
    // AmsState init path calls AmsBackend::create(NONE, null, null) before the
    // factory hooks up specific backends, so the global is the only handle we
    // have at that point.
    MoonrakerClient* mc_raw = mock_client ? mock_client : get_moonraker_client();
    if (auto* mc = dynamic_cast<::MoonrakerClientMock*>(mc_raw)) {
        AmsBackendMock* mock_ptr = mock.get();
        mc->add_active_gcode_tool_observer([mock_ptr](int tool, uint32_t color) {
            mock_ptr->on_simulated_gcode_tool_changed(tool, color);
        });
        spdlog::info("[AMS Backend] Mock backend subscribed to MoonrakerClientMock "
                     "active-gcode-tool notifications");
    } else {
        spdlog::debug("[AMS Backend] No MoonrakerClientMock available; mock backend "
                      "current_tool will stay at default (not simulator-driven)");
    }

    // ========================================================================
    // HELIX_MOCK_AMS — topology/type selection
    // ========================================================================
    const char* mock_ams_env = std::getenv("HELIX_MOCK_AMS");
    std::string ams_type;

    if (mock_ams_env) {
        ams_type = to_lower(mock_ams_env);
    }

    if (!ams_type.empty()) {
        if (ams_type == "afc" || ams_type == "box_turtle" || ams_type == "boxturtle") {
            mock->set_afc_mode(true);
            spdlog::info("[AMS Backend] Mock AFC mode enabled");
        } else if (ams_type == "toolchanger" || ams_type == "tool_changer" || ams_type == "tc") {
            mock->set_tool_changer_mode(true);
            spdlog::info("[AMS Backend] Mock tool changer mode enabled");
        } else if (ams_type == "mixed") {
            mock->set_mixed_topology_mode(true);
            spdlog::info("[AMS Backend] Mock mixed topology mode enabled");
        } else if (ams_type == "multi") {
            mock->set_multi_unit_mode(true);
            spdlog::info("[AMS Backend] Mock multi-unit mode enabled");
        } else if (ams_type == "vivid") {
            mock->set_vivid_mixed_mode(true);
            spdlog::info("[AMS Backend] Mock ViViD mixed mode enabled");
        } else if (ams_type == "ifs" || ams_type == "ad5x" || ams_type == "ad5x_ifs") {
            mock->set_ifs_mode(true);
            spdlog::info("[AMS Backend] Mock AD5X IFS mode enabled");
        } else if (ams_type == "htlf_toolchanger" || ams_type == "htlf_tc" || ams_type == "htlf") {
            mock->set_htlf_toolchanger_mode(true);
            spdlog::info("[AMS Backend] Mock HTLF+Toolchanger mode enabled");
        }
    }

    // ========================================================================
    // HELIX_MOCK_AMS_STATE — visual scenario
    // ========================================================================
    const char* mock_state_env = std::getenv("HELIX_MOCK_AMS_STATE");
    std::string state_scenario;

    if (mock_state_env) {
        state_scenario = to_lower(mock_state_env);
    }

    if (!state_scenario.empty() && state_scenario != "idle") {
        // All non-idle scenarios are applied after start() for consistency.
        // loading/bypass: require running_=true (use interruptible sleep + thread)
        // error: applied directly in start() (no thread needed, but deferred for uniformity)
        mock->set_initial_state_scenario(state_scenario);
        spdlog::info("[AMS Backend] Mock state scenario: {}", state_scenario);
    }

    // ========================================================================
    // Orthogonal features (kept separate)
    // ========================================================================

    // Enable mock dryer by default (disable with HELIX_MOCK_DRYER=0)
    const char* dryer_env = std::getenv("HELIX_MOCK_DRYER");
    bool dryer_enabled = !dryer_env || (std::string(dryer_env) != "0" && std::string(dryer_env) != "false");
    if (dryer_enabled) {
        mock->set_dryer_enabled(true);
        spdlog::info("[AMS Backend] Mock dryer enabled");
    }

    // Environment sensor mode (auto-detects from dryer state if not specified)
    const char* env_mode_env = std::getenv("HELIX_MOCK_AMS_ENV");
    if (env_mode_env) {
        std::string env_mode = to_lower(env_mode_env);
        mock->set_environment_mode(env_mode);
        spdlog::info("[AMS Backend] Mock environment mode: {}", env_mode);
    }

    // Simulate mid-print tool change progress (3rd of 5 swaps) for visual testing
    mock->set_toolchange_progress(2, 5);

    return mock;
}

// Check if mock mode is requested and not explicitly disabled via HELIX_MOCK_AMS=none
static std::unique_ptr<AmsBackend> try_create_mock(MoonrakerClient* mock_client = nullptr) {
    const auto* config = get_runtime_config();
    if (!config->should_mock_ams()) {
        return nullptr;
    }

    const char* mock_ams_env = std::getenv("HELIX_MOCK_AMS");
    if (mock_ams_env && to_lower(mock_ams_env) == "none") {
        spdlog::info("[AMS Backend] Mock AMS disabled via HELIX_MOCK_AMS=none");
        return nullptr;
    }

    spdlog::debug("[AMS Backend] Creating mock backend with {} gates (mock mode enabled)",
                  config->mock_ams_gate_count);
    return create_mock_with_features(config->mock_ams_gate_count, mock_client);
}
#endif

std::unique_ptr<AmsBackend> AmsBackend::create(AmsType detected_type) {
#ifdef HELIX_ENABLE_MOCKS
    const auto* config = get_runtime_config();
    if (auto mock = try_create_mock()) {
        return mock;
    }
#endif

    // Without API/client dependencies, we can only return mock backends
    switch (detected_type) {
    case AmsType::HAPPY_HARE:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] Happy Hare detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] Happy Hare detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::AFC:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] AFC detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] AFC detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::ACE:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] ACE detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] ACE detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::TOOL_CHANGER:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] Tool changer detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] Tool changer detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::AD5X_IFS:
#if HELIX_HAS_IFS && defined(HELIX_ENABLE_MOCKS)
        spdlog::warn("[AMS Backend] AD5X IFS detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] AD5X IFS detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::CFS:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] CFS detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] CFS detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::SNAPMAKER:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] Snapmaker detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] Snapmaker detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::QIDI_BOX:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] QIDI Box detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] QIDI Box detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::MEDUSA_HC:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] MedusaHC detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] MedusaHC detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::NONE:
    default:
        spdlog::debug("[AMS Backend] No AMS detected");
        return nullptr;
    }
}

std::unique_ptr<AmsBackend> AmsBackend::create(AmsType detected_type, MoonrakerAPI* api,
                                               MoonrakerClient* client) {
#ifdef HELIX_ENABLE_MOCKS
    if (auto mock = try_create_mock(client)) {
        return mock;
    }
#endif

    switch (detected_type) {
    case AmsType::HAPPY_HARE:
        if (!api || !client) {
            spdlog::error("[AMS Backend] Happy Hare requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating Happy Hare backend");
        return std::make_unique<AmsBackendHappyHare>(api, client);

    case AmsType::AFC:
        if (!api || !client) {
            spdlog::error("[AMS Backend] AFC requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating AFC backend");
        return std::make_unique<AmsBackendAfc>(api, client);

    case AmsType::ACE:
        if (!api || !client) {
            spdlog::error("[AMS Backend] ACE requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating ACE backend");
        return std::make_unique<AmsBackendAce>(api, client);

    case AmsType::TOOL_CHANGER:
        if (!api || !client) {
            spdlog::error("[AMS Backend] Tool changer requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating Tool Changer backend");
        // Note: Caller must use set_discovered_tools() after creation to set tool names
        return std::make_unique<AmsBackendToolChanger>(api, client);

    case AmsType::AD5X_IFS:
#if HELIX_HAS_IFS
        if (!api || !client) {
            spdlog::error("[AMS Backend] AD5X IFS requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating AD5X IFS backend");
        return std::make_unique<AmsBackendAd5xIfs>(api, client);
#else
        spdlog::info("[AMS Backend] IFS support not compiled in");
        return nullptr;
#endif

    case AmsType::CFS:
#if HELIX_HAS_CFS
        if (!api || !client) {
            spdlog::error("[AMS Backend] CFS requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating CFS backend");
        return std::make_unique<printer::AmsBackendCfs>(api, client);
#else
        spdlog::info("[AMS Backend] CFS support not compiled in");
        return nullptr;
#endif

    case AmsType::SNAPMAKER:
        if (!api || !client) {
            spdlog::error("[AMS Backend] Snapmaker requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating Snapmaker SnapSwap backend");
        return std::make_unique<AmsBackendSnapmaker>(api, client);

    case AmsType::QIDI_BOX:
        if (!api || !client) {
            spdlog::error("[AMS Backend] QIDI Box requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating QIDI Box backend (stub)");
        return std::make_unique<AmsBackendQidi>(api, client);

    case AmsType::MEDUSA_HC:
        if (!api || !client) {
            spdlog::error("[AMS Backend] MedusaHC requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating MedusaHC backend");
        return std::make_unique<AmsBackendMedusaHc>(api, client);

    case AmsType::NONE:
    default:
        spdlog::debug("[AMS Backend] No AMS detected");
        return nullptr;
    }
}

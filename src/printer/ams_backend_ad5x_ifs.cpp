// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_IFS

#include "ams_backend_ad5x_ifs.h"

#include "config.h"
#include "host_identity.h"
#include "http_executor.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "post_op_cooldown_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

using json = nlohmann::json;

AmsBackendAd5xIfs::AmsBackendAd5xIfs(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Fill tool map with UNMAPPED_PORT
    tool_map_.fill(UNMAPPED_PORT);
    port_presence_.fill(false);

    // Initialize SlotRegistry with 4 ports in a single unit
    std::vector<std::string> slot_names;
    for (int i = 1; i <= NUM_PORTS; ++i) {
        slot_names.push_back(std::to_string(i));
    }
    slots_.initialize("IFS", slot_names);

    // Set system info capabilities
    system_info_.type = AmsType::AD5X_IFS;
    system_info_.type_name = "AD5X IFS";
    system_info_.total_slots = NUM_PORTS;
    system_info_.supports_bypass = true;
    system_info_.supports_tool_mapping = true;
    system_info_.supports_endless_spool = false;
    system_info_.supports_purge = false;
}

AmsBackendAd5xIfs::~AmsBackendAd5xIfs() = default;

// --- Lifecycle ---

void AmsBackendAd5xIfs::on_started() {
    // Load persisted per-slot overrides (brand, spool name, spoolman IDs, etc.)
    // BEFORE issuing the initial status query — otherwise the first
    // handle_status_update() callback may fire (on the websocket thread) and
    // update slots before overrides_ is populated, so the first frame of
    // EVENT_STATE_CHANGED would be missing override data. load_blocking runs
    // on this (main) thread; the Moonraker DB callback fires on the libhv
    // event loop, so the two threads don't interfere.
    if (api_) {
        override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api_, "ifs");
        // Do the (potentially 5s) MR DB round-trip OUTSIDE the lock, then swap in
        // under mutex_. AmsSubscriptionBackend::start() registers the WebSocket
        // notify subscription before on_started() is invoked, so a status
        // notification could in principle fire on the libhv thread while we're
        // still inside load_blocking. Holding mutex_ during the swap ensures
        // the parse path (which reads overrides_ under mutex_) sees a coherent
        // map rather than a torn write.
        auto loaded = override_store_->load_blocking();
        const auto loaded_count = loaded.size();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overrides_ = std::move(loaded);
        }
        spdlog::info("{} Loaded {} slot overrides from filament_slot store", backend_log_tag(),
                     loaded_count);
    }

    // Resolve the on-disk Adventurer5M.json path so write_adventurer_json()
    // can bypass the broken Moonraker upload path. Done after override-store
    // setup so failure here doesn't block override loading. Safe no-op when
    // we're remote, the file isn't present, or it isn't writable.
    detect_local_adventurer_json_path();

    // Query initial state from printer
    if (!client_)
        return;

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query",
        json{{"objects",
              json{{"save_variables", nullptr},
                   // Verify _IFS_VARS macro actually exists (not just save_variables data)
                   {"gcode_macro _ifs_vars", nullptr},
                   // lessWaste plugin: per-port filament switch sensors
                   {"filament_switch_sensor _ifs_port_sensor_1", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_2", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_3", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_4", nullptr},
                   // Shared: head switch sensor (both lessWaste and native ZMOD)
                   {"filament_switch_sensor head_switch_sensor", nullptr},
                   // Native ZMOD IFS: single motion sensor (replaces per-port sensors)
                   {"filament_motion_sensor ifs_motion_sensor", nullptr},
                   // Klippy state — GET_ZCOLOR SILENT=1 only works once zmod is
                   // initialised, so we gate the initial query on webhooks.state == "ready".
                   {"webhooks", nullptr}}}},
        [this, token](const json& response) {
            if (token.expired())
                return;

            // Check if the _IFS_VARS gcode macro actually exists on this printer.
            // save_variables may contain lessWaste/bambufy data from a partially installed
            // plugin, but the macro itself might not be loaded in Klipper.
            bool macro_exists = false;
            bool klippy_ready = false;
            if (response.contains("result") && response["result"].contains("status")) {
                const auto& status = response["result"]["status"];
                macro_exists = status.contains("gcode_macro _ifs_vars");
                if (status.contains("webhooks") && status["webhooks"].contains("state") &&
                    status["webhooks"]["state"].is_string()) {
                    klippy_ready = status["webhooks"]["state"].get<std::string>() == "ready";
                }

                // Update latch BEFORE handle_status_update so parse_save_variables
                // sees the correct state. ifs_macro_confirmed_missing_ starts true
                // (pessimistic) and is only cleared here when the macro exists.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (macro_exists) {
                        ifs_macro_confirmed_missing_ = false;
                    }
                    // !macro_exists: latch stays true (initialized true)
                }

                handle_status_update(status);
            }

            // Log initial state after processing query response
            {
                std::lock_guard<std::mutex> lock(mutex_);
                spdlog::debug("{} Initial query: has_ifs_vars={}, macro_exists={}, "
                              "klippy_ready={}, has_per_port_sensors={}, head_filament={}, "
                              "port_presence=[{},{},{},{}], "
                              "colors=[{},{},{},{}]",
                              backend_log_tag(), has_ifs_vars_, macro_exists, klippy_ready,
                              has_per_port_sensors_, head_filament_, port_presence_[0],
                              port_presence_[1], port_presence_[2], port_presence_[3], colors_[0],
                              colors_[1], colors_[2], colors_[3]);
            }

            // Safety net: if parse_save_variables somehow set has_ifs_vars_ despite
            // the latch (shouldn't happen), force it back off for missing macros.
            // has_ifs_vars_ now only gates tool-mapping / current_tool / external
            // reads from save_variables — color/type writes use CHANGE_ZCOLOR for
            // every user regardless.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!macro_exists && has_ifs_vars_) {
                    spdlog::warn("{} save_variables contain {}_ data but _IFS_VARS macro "
                                 "not found — clearing has_ifs_vars_ (tool-mapping "
                                 "from save_variables disabled)",
                                 backend_log_tag(), var_prefix_);
                    has_ifs_vars_ = false;
                }
            }

            // Adventurer5M.json + GET_ZCOLOR SILENT=1 are zmod's authoritative
            // color/type sources for ALL AD5X IFS users. lessWaste/bambufy
            // _IFS_VARS save_variables (less_waste_colors / bambufy_colors)
            // live in a private namespace zmod does not read, so we never
            // trust them for color/type — only for tool-mapping and friends.
            // Register the listeners + fire the initial query unconditionally.
            spdlog::info("{} Reading Adventurer5M.json + GET_ZCOLOR SILENT=1 for color truth",
                         backend_log_tag());
            read_adventurer_json();
            // One-shot fetch of zmod's user-defined material types from
            // /mod_data/user.cfg. Independent of the json/gcode pipelines —
            // best-effort, 404 on non-zmod printers is silent.
            fetch_user_cfg_materials();
            register_zcolor_listener();
            // notify_klippy_ready catches startup and FIRMWARE_RESTART; it's
            // the point at which zmod is initialised and GET_ZCOLOR returns
            // populated results.
            register_klippy_ready_listener();
            if (klippy_ready) {
                schedule_zcolor_query();
            } else {
                spdlog::info("{} Deferring GET_ZCOLOR SILENT=1 until klippy ready",
                             backend_log_tag());
            }
        });
}

void AmsBackendAd5xIfs::on_stopping() {
    unregister_moonraker_listeners();
}

void AmsBackendAd5xIfs::request_resync() {
    spdlog::info("{} request_resync(): re-reading Adventurer5M.json + GET_ZCOLOR",
                 backend_log_tag());
    read_adventurer_json();
    schedule_zcolor_query();
}

// --- Status parsing ---

void AmsBackendAd5xIfs::handle_status_update(const json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    // Initial query response sends unwrapped status directly — handle both formats.
    const json* status = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status = &notification["params"][0];
        if (!status->is_object()) {
            return;
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);

    bool state_changed = false;
    bool sensor_changed = false;

    // Parse save_variables if present
    if (status->contains("save_variables")) {
        const auto& sv = (*status)["save_variables"];
        if (sv.contains("variables") && sv["variables"].is_object()) {
            parse_save_variables(sv["variables"]);
            state_changed = true;
        }
    }

    // Parse per-port filament sensors
    // Leading space in sensor name is intentional — Klipper object naming convention
    for (int port = 1; port <= NUM_PORTS; ++port) {
        std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port);
        if (status->contains(key)) {
            const auto& sensor = (*status)[key];
            if (sensor.contains("filament_detected") && sensor["filament_detected"].is_boolean()) {
                parse_port_sensor(port, sensor["filament_detected"].get<bool>());
                state_changed = true;
                sensor_changed = true;
            }
        }
    }

    // Native ZMOD: when a port sensor changes, the user may have swapped filament.
    // Schedule a re-read of Adventurer5M.json to pick up any color/type changes.
    if (sensor_changed && !has_ifs_vars_) {
        lock.unlock();
        schedule_json_reread();
        lock.lock();
    }

    // Native ZMOD IFS: single motion sensor replaces per-port presence sensors.
    // Maps to head_filament_ since it detects filament at the toolhead.
    if (status->contains("filament_motion_sensor ifs_motion_sensor")) {
        const auto& motion = (*status)["filament_motion_sensor ifs_motion_sensor"];
        if (motion.contains("filament_detected") && motion["filament_detected"].is_boolean()) {
            bool detected = motion["filament_detected"].get<bool>();
            parse_head_sensor(detected);
            detect_load_unload_completion(detected);
            state_changed = true;
        }
    }

    // Parse head sensor
    if (status->contains("filament_switch_sensor head_switch_sensor")) {
        const auto& head = (*status)["filament_switch_sensor head_switch_sensor"];
        if (head.contains("filament_detected") && head["filament_detected"].is_boolean()) {
            bool detected = head["filament_detected"].get<bool>();
            parse_head_sensor(detected);
            detect_load_unload_completion(detected);
            state_changed = true;
        }
    }

    // Update system info from cached state
    if (state_changed) {
        system_info_.current_tool = active_tool_;
        system_info_.filament_loaded = head_filament_;

        // Map current tool to current slot
        if (active_tool_ >= 0 && active_tool_ < TOOL_MAP_SIZE) {
            int port = tool_map_[static_cast<size_t>(active_tool_)];
            if (port >= 1 && port <= NUM_PORTS) {
                system_info_.current_slot = port - 1;
            } else {
                system_info_.current_slot = -1;
            }
        } else {
            system_info_.current_slot = -1;
        }

        // Update all slot states
        for (int i = 0; i < NUM_PORTS; ++i) {
            update_slot_from_state(i);
        }
    }

    // Check for stuck operations on every status update
    check_action_timeout();

    lock.unlock();

    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // Freshness backstop: zmod can mutate Adventurer5M.json via the on-printer
    // "Select print materials" dialog without echoing a CHANGE_ZCOLOR token
    // through notify_gcode_response (zmod's dialog handler is closed-source).
    //
    // We poll the JSON content over HTTP (invisible to the gcode console) and
    // only fire GET_ZCOLOR when the content actually changed. The previous
    // unconditional 15s GET_ZCOLOR backstop was producing 1-2 GET_ZCOLOR/s
    // on connected printers (raza/DIEHARDave report on v0.99.51) and made the
    // Mainsail/Fluidd console unusable. Piggybacking on notify_status_update
    // gives us sub-second cadence; rate-limit via steady_clock so the actual
    // download fires no more often than kJsonPollInterval.
    constexpr auto kJsonPollInterval = std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (now - last_json_poll_kick_ >= kJsonPollInterval) {
        last_json_poll_kick_ = now;
        poll_adventurer_json();
    }
}

void AmsBackendAd5xIfs::parse_save_variables(const json& vars) {
    // Auto-detect variable prefix: lessWaste/zmod uses "less_waste_*", bambufy uses "bambufy_*".
    // Check once per status update — the prefix can't change at runtime, but we may not
    // see both sets of variables in the initial query.
    if (vars.contains("bambufy_colors") || vars.contains("bambufy_tools")) {
        if (var_prefix_ != "bambufy") {
            var_prefix_ = "bambufy";
            spdlog::info("{} Detected bambufy variable prefix", backend_log_tag());
        }
        if (!ifs_macro_confirmed_missing_) {
            has_ifs_vars_ = true;
        }
    } else if (vars.contains("less_waste_colors") || vars.contains("less_waste_tools")) {
        if (var_prefix_ != "less_waste") {
            var_prefix_ = "less_waste";
            spdlog::info("{} Detected lessWaste variable prefix", backend_log_tag());
        }
        if (!ifs_macro_confirmed_missing_) {
            has_ifs_vars_ = true;
        }
    }

    // User-defined material types from bambufy_custom_types. Surfaced via
    // get_supported_materials() so the edit modal's dropdown isn't restricted
    // to the firmware whitelist (PLA, PLA-CF, SILK, TPU, ABS, PETG, PETG-CF) —
    // PLA+, rPLA, PETG-Pro etc. round-trip cleanly through zmod's COLOR macro
    // and don't get silently normalized to PLA on save (#904 root cause #2).
    // Always read regardless of has_ifs_vars_: user-defined types are
    // orthogonal to plugin activation.
    if (vars.contains("bambufy_custom_types") && vars["bambufy_custom_types"].is_array()) {
        std::vector<std::string> staged;
        for (const auto& t : vars["bambufy_custom_types"]) {
            if (t.is_string()) {
                std::string name = t.get<std::string>();
                if (!name.empty()) {
                    staged.push_back(std::move(name));
                }
            }
        }
        size_t count = staged.size();
        {
            std::lock_guard<std::mutex> lock(custom_types_mutex_);
            custom_material_types_ = std::move(staged);
        }
        if (count > 0) {
            spdlog::debug("{} Loaded {} bambufy_custom_types entries", backend_log_tag(), count);
        }
    }

    const std::string p = var_prefix_;

    // NOTE on colors/types: <prefix>_colors and <prefix>_types live in the
    // lessWaste/bambufy plugin's PRIVATE save_variables namespace. zmod does
    // NOT read them — its authoritative color/type store is its own in-memory
    // state, persisted to Adventurer5M.json, mutated only by CHANGE_ZCOLOR.
    // Trusting <prefix>_colors here was poisoning colors_[]/materials_[] with
    // values that diverged silently from zmod's truth (raza's debug bundle
    // ZYYRVVTG showed Adventurer5M.json and less_waste_colors out of sync).
    // Color/type reads now come from GET_ZCOLOR SILENT=1 (live) and
    // Adventurer5M.json (boot snapshot) only. set_slot_info() correspondingly
    // writes via CHANGE_ZCOLOR rather than _IFS_VARS, so the dirty_-clearing
    // round-trip that lived in this branch is no longer needed.
    //
    // The fields below — tools, current_tool, external — DO live only in the
    // plugin's save_variables namespace (no other Moonraker-visible source),
    // so we keep reading them — but ONLY when the plugin is actually active
    // (has_ifs_vars_ requires both the prefix detection above AND the live
    // gcode_macro _ifs_vars existence check from on_started). save_variables
    // rows persist in printer_data/database/ even after a plugin uninstall;
    // pre-fix, a user who removed lessWaste/bambufy but left the rows behind
    // would silently keep using the stale tool map and last active-tool guess
    // forever. The gate makes "macro present" load-bearing for trusting the
    // plugin's data, matching the contract has_ifs_vars_ already advertises.
    if (has_ifs_vars_) {
        // Tool mapping: 16-element array, index=tool, value=port (1-4, 5=unmapped).
        //
        // Both-prefixes-conflict guard (#904): TMTYD's printer had bambufy_tools=
        // [4,2,4,3,...] AND less_waste_tools=[2,1,3,4] left over from past plugin
        // activations, with NEITHER plugin currently driving state (only nopoop
        // active). Our prefix-detect picks bambufy first and applied [4,2,4,3,...]
        // — putting T0 on port 4 and breaking every per-port T-badge in the UI.
        //
        // Rule: if both prefixes have _tools arrays AND they disagree, neither is
        // authoritative. Fall back to the default 1:1 mapping (T0→port1, T1→port2,
        // …) and skip current_tool / external reads too — those came from the same
        // poisoned source.
        //
        // Single-prefix-stale-data is NOT handled here — if only bambufy_* exists
        // (or only less_waste_*) and the plugin has been uninstalled, the
        // ifs_macro_confirmed_missing_ latch from on_started's gcode_macro
        // existence check catches it: has_ifs_vars_ stays false and we never
        // reach this branch. The guard below only matters for the
        // both-installed-then-deactivated scenario.
        const std::string other_p = (p == "bambufy") ? "less_waste" : "bambufy";
        const bool have_self_tools =
            vars.contains(p + "_tools") && vars[p + "_tools"].is_array();
        const bool have_other_tools =
            vars.contains(other_p + "_tools") && vars[other_p + "_tools"].is_array();
        bool conflict = false;
        if (have_self_tools && have_other_tools) {
            conflict = vars[p + "_tools"] != vars[other_p + "_tools"];
        }
        if (conflict) {
            spdlog::warn("{} Both bambufy_tools and less_waste_tools present and disagree "
                         "— stale data from a deactivated plugin; falling back to default "
                         "1:1 tool mapping",
                         backend_log_tag());
            tool_map_.fill(UNMAPPED_PORT);
            for (int t = 0; t < NUM_PORTS; ++t) {
                tool_map_[static_cast<size_t>(t)] = t + 1;
            }
            for (int i = 0; i < NUM_PORTS; ++i) {
                int tool = find_first_tool_for_port(i + 1);
                slots_.set_tool_mapping(i, tool);
            }
            for (int i = 0; i < NUM_PORTS; ++i) {
                update_slot_from_state(i);
            }
            return;
        }

        if (have_self_tools) {
            const auto& tools = vars[p + "_tools"];
            for (size_t i = 0; i < std::min(tools.size(), static_cast<size_t>(TOOL_MAP_SIZE));
                 ++i) {
                if (tools[i].is_number_integer()) {
                    tool_map_[i] = tools[i].get<int>();
                }
            }
        }

        // Current tool (-1 = none, 0-15 = tool number)
        if (vars.contains(p + "_current_tool") &&
            vars[p + "_current_tool"].is_number_integer()) {
            active_tool_ = vars[p + "_current_tool"].get<int>();
        }

        // External/bypass mode (0 or 1)
        if (vars.contains(p + "_external") && vars[p + "_external"].is_number_integer()) {
            external_mode_ = (vars[p + "_external"].get<int>() != 0);
        }

        // Rebuild SlotRegistry tool mapping from IFS tool_map_. Only meaningful
        // when tool_map_ was populated above; without an active plugin the map
        // is whatever default values the registry was constructed with, and
        // running this loop would lock those defaults in over real data that
        // might arrive later via apply_zcolor_result's extruder_slot path.
        for (int i = 0; i < NUM_PORTS; ++i) {
            int tool = find_first_tool_for_port(i + 1); // port is 1-based
            slots_.set_tool_mapping(i, tool);
        }
    }

    // Sync all slots from cached state
    for (int i = 0; i < NUM_PORTS; ++i) {
        update_slot_from_state(i);
    }
}

void AmsBackendAd5xIfs::parse_port_sensor(int port_1based, bool detected) {
    int slot = port_1based - 1;
    if (slot >= 0 && slot < NUM_PORTS) {
        bool was_first = !has_per_port_sensors_;
        bool changed = port_presence_[static_cast<size_t>(slot)] != detected;
        has_per_port_sensors_ = true;
        port_presence_[static_cast<size_t>(slot)] = detected;
        if (was_first || changed) {
            spdlog::debug("{} Port {} sensor: {} (per_port_sensors=true{})", backend_log_tag(),
                          port_1based, detected ? "present" : "empty",
                          was_first ? ", first detection" : "");
        }
    }
}

void AmsBackendAd5xIfs::parse_head_sensor(bool detected) {
    if (head_filament_ != detected) {
        spdlog::debug("{} Head sensor: {}", backend_log_tag(),
                      detected ? "filament detected" : "no filament");
    }
    head_filament_ = detected;
}

void AmsBackendAd5xIfs::update_slot_from_state(int slot_index) {
    if (slot_index < 0 || slot_index >= NUM_PORTS)
        return;

    auto* entry = slots_.get_mut(slot_index);
    if (!entry)
        return;

    auto idx = static_cast<size_t>(slot_index);

    // Color: parse hex string to uint32_t
    if (!colors_[idx].empty()) {
        try {
            entry->info.color_rgb = static_cast<uint32_t>(std::stoul(colors_[idx], nullptr, 16));
        } catch (...) {
            // Invalid hex — leave color unchanged
        }
    }

    // Material
    entry->info.material = materials_[idx];

    // Status based on sensor and active state
    bool is_active_slot = (system_info_.current_slot == slot_index);
    bool has_filament = port_presence_[idx];

    // Native ZMOD IFS has no per-port sensors. For the active slot, infer
    // presence from the head sensor so the UI doesn't show all slots as EMPTY.
    if (!has_per_port_sensors_ && is_active_slot && head_filament_) {
        has_filament = true;
    }

    SlotStatus prev_status = entry->info.status;
    if (has_filament && is_active_slot && head_filament_) {
        entry->info.status = SlotStatus::LOADED;
    } else if (has_filament) {
        entry->info.status = SlotStatus::AVAILABLE;
    } else {
        entry->info.status = SlotStatus::EMPTY;
    }

    if (entry->info.status != prev_status) {
        spdlog::debug("{} Slot {} status: {} → {} (port_presence={}, active={}, head={}, "
                      "per_port_sensors={}, color={}, material={})",
                      backend_log_tag(), slot_index, static_cast<int>(prev_status),
                      static_cast<int>(entry->info.status), port_presence_[idx], is_active_slot,
                      head_filament_, has_per_port_sensors_, colors_[idx], materials_[idx]);
    }

    // Reverse tool mapping: find first tool that maps to this port
    entry->info.mapped_tool = find_first_tool_for_port(slot_index + 1);

    // External-edit detection MUST run BEFORE apply_overrides. entry->info
    // .color_rgb is firmware-truth here IF colors_[idx] was non-empty above;
    // after apply_overrides it would be masked by the (possibly stale)
    // override and we'd miss the delta vs. the prior firmware baseline.
    //
    // When colors_[idx] is empty we have NO firmware reading yet —
    // entry->info.color_rgb is whatever was left there by the SlotInfo
    // default (AMS_DEFAULT_SLOT_COLOR / 0x808080) or a prior apply_overrides
    // leak. Pass 0 (the helper's "no signal" sentinel) so we don't establish
    // a phantom baseline that would later be misread as an external edit.
    // Boot path: parse_save_variables / handle_status_update call
    // update_slot_from_state BEFORE parse_adventurer_json fills in colors_[];
    // pre-fix this populated a 0x808080 baseline, then the first real parse
    // triggered a bogus sync.
    const uint32_t observed_color = colors_[idx].empty() ? 0u : entry->info.color_rgb;
    // Pass slot_has_filament so the helper skips creating a phantom override
    // when a slot read came back as the empty-placeholder #808080 — the eject
    // path in parse_adventurer_json clears the override explicitly.
    check_external_color_change(slot_index, observed_color, port_presence_[idx]);

    // Layer user-configured overrides on top of firmware-reported data. Called
    // last so overrides win for any non-default field. Callers hold mutex_,
    // which also covers overrides_ writes from on_started() and set_slot_info()
    // — see apply_overrides() below for the invariant.
    apply_overrides(entry->info, slot_index);
}

void AmsBackendAd5xIfs::apply_overrides(SlotInfo& slot, int slot_index) {
    // overrides_ is mutated in on_started() (initial load) and set_slot_info()
    // (persisted user edit). Both writers hold mutex_, and every caller of
    // apply_overrides runs inside update_slot_from_state() under mutex_ — so
    // the map is implicitly lock-protected here. If a slot has no override
    // entry, this is a zero-cost hash lookup followed by early return — safe
    // to call inside the hot parse path.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return;
    const auto& o = it->second;
    // Merge policy: override wins when the override field carries a real value.
    // - Strings: non-empty means "user set this", empty means "don't override".
    // - spoolman_id / spoolman_vendor_id: >0 means a real Spoolman record;
    //   0 is the "not linked" sentinel and must fall through.
    // - weights: -1.0 is "unknown" and must fall through; 0 is a legitimate
    //   empty-spool reading and should override.
    // - color_rgb: 0 is treated as "no override" (matches to_lane_data_record's
    //   omission rule and keeps black-but-unset indistinguishable from unset
    //   per the store's on-disk schema; callers who truly mean pure black
    //   #000000 should instead use 0x000001-equivalent color_name).
    if (!o.brand.empty())
        slot.brand = o.brand;
    if (!o.spool_name.empty())
        slot.spool_name = o.spool_name;
    if (o.spoolman_id > 0)
        slot.spoolman_id = o.spoolman_id;
    if (o.spoolman_vendor_id > 0)
        slot.spoolman_vendor_id = o.spoolman_vendor_id;
    if (o.remaining_weight_g >= 0.0f)
        slot.remaining_weight_g = o.remaining_weight_g;
    if (o.total_weight_g >= 0.0f)
        slot.total_weight_g = o.total_weight_g;
    if (o.color_rgb != 0)
        slot.color_rgb = o.color_rgb;
    if (!o.color_name.empty())
        slot.color_name = o.color_name;
    if (!o.material.empty())
        slot.material = o.material;
}

bool AmsBackendAd5xIfs::check_external_color_change(int slot_index, uint32_t observed_color,
                                                    bool slot_has_filament) {
    // observed_color is whatever color this parse (or caller) believes is
    // currently in the slot — typically firmware-truth from the parse path,
    // but set_slot_info() also pre-updates the baseline with the user's
    // chosen color before calling update_slot_from_state(), so this helper
    // can be fed a user-provided color too. Either way, the "did it change
    // from what we last saw?" contract is the same.
    //
    // observed_color == 0 is "no reading" (empty slot, unread, transient JSON
    // parse race). Treat as non-signal: don't update the baseline, don't sync.
    // Otherwise every empty-slot poll would overwrite a real prior color and
    // mask a genuine subsequent edit on the next non-empty poll.
    if (observed_color == 0)
        return false;

    auto it = last_firmware_color_.find(slot_index);
    if (it == last_firmware_color_.end()) {
        // First observation for this slot — establish baseline. Even if the
        // override's color_rgb differs from firmware, the initial startup
        // observation is NEVER an external-edit signal. apply_overrides will
        // still run after us and the override wins.
        last_firmware_color_[slot_index] = observed_color;
        spdlog::debug("{} Slot {} baseline color: #{:06X}", backend_log_tag(), slot_index,
                      observed_color);
        return false;
    }
    if (it->second == observed_color)
        return false; // unchanged — no edit signal

    // Observed color changed. Record the new color as the baseline before
    // doing anything else so a failed save_async doesn't make us re-fire
    // every poll.
    const uint32_t old_color = it->second;
    it->second = observed_color;

    if (!slot_has_filament) {
        // Empty slot reads back as the placeholder #808080 in parse_adventurer_json
        // — that's not an "edit," it's the absence of a reading. Eject is
        // handled separately by parse_adventurer_json calling
        // clear_override_locked when presence flips false. Just update the
        // baseline so we don't repeat this branch every poll.
        spdlog::debug("{} Slot {} firmware color changed #{:06X} -> #{:06X} "
                      "(slot empty — sync skipped, eject handled by parse path)",
                      backend_log_tag(), slot_index, old_color, observed_color);
        return false;
    }

    spdlog::info("{} Slot {} firmware color changed #{:06X} -> #{:06X}, "
                 "syncing override + Moonraker DB lane_data (external edit detected)",
                 backend_log_tag(), slot_index, old_color, observed_color);

    // External edit (Mainsail console, AD5X LCD, native zmod dialog,
    // CHANGE_ZCOLOR from any non-Helix path). Refresh the override's
    // color_rgb + material — preserving brand/spool_name/spoolman_id —
    // and push the result to lane_data so OrcaSlicer's MoonrakerPrinterAgent
    // sees the new state. The caller's next apply_overrides() call lays the
    // refreshed override back over entry->info; since color_rgb + material
    // now match firmware-truth, that's a no-op for those fields.
    sync_override_to_firmware_locked(slot_index, observed_color,
                                     materials_[static_cast<size_t>(slot_index)]);
    return true;
}

bool AmsBackendAd5xIfs::sync_override_to_firmware_locked(int slot_index,
                                                         uint32_t firmware_color,
                                                         const std::string& firmware_material) {
    auto& ovr = overrides_[slot_index]; // creates a default-constructed entry if absent
    const bool color_changed = ovr.color_rgb != firmware_color;
    const bool material_changed = ovr.material != firmware_material;
    if (!color_changed && !material_changed)
        return false; // already in sync; don't churn lane_data

    ovr.color_rgb = firmware_color;
    ovr.material = firmware_material;
    // updated_at left as-is — save_async stamps a fresh value so the on-disk
    // record's scan_time wins over any local clock skew.
    //
    // parse_adventurer_json reads external_sync_count_ before/after its loop
    // to decide whether to also push an _IFS_VARS mirror to the
    // lessWaste/bambufy plugin's save_variables — see the comment block
    // there for why that's required (the plugins don't self-sync).
    ++external_sync_count_;

    if (override_store_) {
        helix::ams::FilamentSlotOverride snapshot = ovr;
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, snapshot,
            [tag, slot_index](bool success, const std::string& err) {
                if (!success) {
                    spdlog::warn("{} lane_data sync failed for slot {}: {}", tag, slot_index, err);
                }
            });
    }
    return true;
}

void AmsBackendAd5xIfs::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Erases the in-memory override, resets
    // override-exclusive fields on the live SlotInfo (so the next
    // get_slot_info sees cleared state — apply_overrides is a no-op for this
    // slot after erase), and fires the async store delete. Firmware-sourced
    // fields (color_rgb, material, mapped_tool, status) are left alone —
    // update_slot_from_state has already refreshed them.
    overrides_.erase(slot_index);

    slot.brand.clear();
    slot.spool_name.clear();
    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
    slot.remaining_weight_g = -1.0f;
    slot.total_weight_g = -1.0f;
    slot.color_name.clear();

    if (override_store_) {
        // Capture by value only — clear_async's Moonraker callback can fire
        // long after this function returns (MR tracker ~60s timeout) and
        // after the backend itself may be gone. Same pattern as the
        // save_async site in set_slot_info().
        const std::string tag = backend_log_tag();
        override_store_->clear_async(
            slot_index, [tag, slot_index](bool ok, std::string err) {
                if (!ok) {
                    spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
                }
            });
    }
}

void AmsBackendAd5xIfs::clear_slot_override(int slot_index) {
    if (slot_index < 0 || slot_index >= NUM_PORTS) {
        spdlog::warn("{} clear_slot_override: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, entry->info);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

// --- State queries ---

AmsSystemInfo AmsBackendAd5xIfs::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for stuck operations on every UI poll, not just status updates.
    // This catches cases where the printer goes silent (network drop, Klipper crash).
    const_cast<AmsBackendAd5xIfs*>(this)->check_action_timeout();

    auto info = slots_.build_system_info();

    // Overlay our cached system info
    info.type = system_info_.type;
    info.type_name = system_info_.type_name;
    info.total_slots = system_info_.total_slots;
    info.current_tool = system_info_.current_tool;
    info.current_slot = system_info_.current_slot;
    info.filament_loaded = system_info_.filament_loaded;
    info.action = system_info_.action;
    info.supports_bypass = system_info_.supports_bypass;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.supports_endless_spool = system_info_.supports_endless_spool;
    info.supports_purge = system_info_.supports_purge;

    // Replace registry's tool map with IFS-specific 16-entry mapping
    info.tool_to_slot_map.clear();
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        int port = tool_map_[static_cast<size_t>(t)];
        if (port >= 1 && port <= NUM_PORTS) {
            info.tool_to_slot_map.push_back(port - 1);
        } else {
            info.tool_to_slot_map.push_back(-1);
        }
    }

    return info;
}

SlotInfo AmsBackendAd5xIfs::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return SlotInfo{};
    }
    return entry->info;
}

bool AmsBackendAd5xIfs::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return external_mode_;
}

// --- Path visualization ---

PathSegment AmsBackendAd5xIfs::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head_filament_) {
        return PathSegment::NOZZLE;
    }
    // Check if active tool's port has filament
    if (active_tool_ >= 0 && active_tool_ < TOOL_MAP_SIZE) {
        int port = tool_map_[static_cast<size_t>(active_tool_)];
        if (port >= 1 && port <= NUM_PORTS && port_presence_[static_cast<size_t>(port - 1)]) {
            return PathSegment::LANE;
        }
    }
    return PathSegment::NONE;
}

PathSegment AmsBackendAd5xIfs::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_PORTS) {
        return PathSegment::NONE;
    }

    auto idx = static_cast<size_t>(slot_index);
    if (!port_presence_[idx]) {
        return PathSegment::NONE;
    }

    bool is_active = (system_info_.current_slot == slot_index);
    if (is_active && head_filament_) {
        return PathSegment::NOZZLE;
    }

    // Active slot in transit — filament is in the lane between gate and head
    if (is_active) {
        return PathSegment::LANE;
    }
    // Non-active slots with filament detected at gate — show at hub
    return PathSegment::HUB;
}

PathSegment AmsBackendAd5xIfs::infer_error_segment() const {
    // IFS doesn't report fine-grained error segments
    return PathSegment::NONE;
}

// --- Filament operations ---

AmsError AmsBackendAd5xIfs::load_filament(int slot_index) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }
    auto err = check_preconditions();
    if (!err.success())
        return err;

    int port = slot_index + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }
    spdlog::info("{} Loading filament from port {}", backend_log_tag(), port);
    return ensure_homed_then("INSERT_PRUTOK_IFS PRUTOK=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::unload_filament(int slot_index) {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }

    int current_slot;
    bool head_filament;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_slot = system_info_.current_slot;
        head_filament = head_filament_;
    }

    std::string unload_cmd = select_unload_command(slot_index, current_slot, head_filament);
    if (unload_cmd == "IFS_REMOVE_PRUTOK") {
        spdlog::info("{} Unloading current filament (slot {})", backend_log_tag(), slot_index);
    } else {
        spdlog::info("{} Unloading filament from port {}", backend_log_tag(), slot_index + 1);
    }

    auto result = ensure_homed_then(std::move(unload_cmd));
    // Backup re-query: for inactive-slot unloads on native ZMOD the head
    // sensor never changes, so detect_load_unload_completion() won't fire.
    // schedule_zcolor_query() coalesces with any trigger from the gcode
    // stream listener, so this is cheap when they overlap.
    schedule_zcolor_query();
    return result;
}

std::string AmsBackendAd5xIfs::select_unload_command(int slot_index, int current_slot,
                                                     bool head_filament) {
    const bool unload_active = (slot_index < 0) || (slot_index == current_slot && head_filament);
    if (unload_active) {
        return "IFS_REMOVE_PRUTOK";
    }
    if (slot_index >= 0 && slot_index < NUM_PORTS) {
        return "REMOVE_PRUTOK_IFS PRUTOK=" + std::to_string(slot_index + 1);
    }
    return "IFS_REMOVE_PRUTOK";
}

AmsError AmsBackendAd5xIfs::select_slot(int slot_index) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }
    auto err = check_preconditions();
    if (!err.success())
        return err;

    int port = slot_index + 1;
    spdlog::info("{} Selecting port {}", backend_log_tag(), port);
    return execute_gcode("SET_EXTRUDER_SLOT SLOT=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::change_tool(int tool_number) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_slot(tool_number, TOOL_MAP_SIZE - 1);
    }
    auto err = check_preconditions();
    if (!err.success())
        return err;

    int port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        port = tool_map_[static_cast<size_t>(tool_number)];
    }

    if (port < 1 || port > NUM_PORTS) {
        return AmsErrorHelper::invalid_parameter("Tool T" + std::to_string(tool_number) +
                                                 " is not mapped to any port");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }
    spdlog::info("{} Changing to tool T{} (port {})", backend_log_tag(), tool_number, port);
    return ensure_homed_then("A_CHANGE_FILAMENT CHANNEL=" + std::to_string(port));
}

// --- Recovery ---

AmsError AmsBackendAd5xIfs::recover() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK resets the IFS driver state machine — safest recovery command
    spdlog::info("{} Recovery: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::reset() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK resets the IFS driver — F15 (hard reset) is not exposed as a safe macro
    spdlog::info("{} Reset: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::cancel() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK to abort current operation
    spdlog::info("{} Cancel: IFS_UNLOCK", backend_log_tag());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::IDLE;
    }
    return execute_gcode("IFS_UNLOCK");
}

// --- Configuration ---

std::optional<std::vector<std::string>> AmsBackendAd5xIfs::get_supported_materials() const {
    // Stock AD5X firmware whitelist — sending anything outside it causes
    // "Invalid material type: X. Valid: PLA, PLA-CF, SILK, TPU, ABS, PETG, PETG-CF".
    std::vector<std::string> result{"PLA", "PLA-CF", "SILK", "TPU", "ABS", "PETG", "PETG-CF"};

    // Append user-defined types from bambufy_custom_types (save_variables) and
    // [zmod_ifs] filament_<NAME> (mod_data/user.cfg). zmod's COLOR macro
    // accepts these alongside the stock list, so they round-trip cleanly
    // through CHANGE_ZCOLOR / Adventurer5M.json without firmware rejection.
    // Including them here makes them appear in the edit modal dropdown AND
    // makes normalize_material()'s case-insensitive exact-match return them
    // unchanged on save (#904).
    auto lower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return out;
    };
    auto already_present = [&](const std::string& name) {
        std::string n_lc = lower(name);
        for (const auto& existing : result) {
            if (lower(existing) == n_lc) {
                return true;
            }
        }
        return false;
    };
    {
        // Use custom_types_mutex_ — NOT mutex_ — so callers that already hold
        // mutex_ (e.g., normalize_material() invoked inside set_slot_info)
        // don't deadlock.
        std::lock_guard<std::mutex> lock(custom_types_mutex_);
        for (const auto& name : custom_material_types_) {
            if (!already_present(name)) {
                result.push_back(name);
            }
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> AmsBackendAd5xIfs::get_material_aliases() const {
    // Names that mean "silk PLA" in the 3D printing world but map to AD5X's
    // distinct SILK slot type. Without these, the compat_group fallback
    // routes them to "PLA" (because silk PLA IS chemically PLA) and users
    // lose the silk distinction on slot edits / Orca imports.
    return {
        {"Silk", "SILK"},
        {"Silk PLA", "SILK"},
        {"PLA Silk", "SILK"},
    };
}

AmsError AmsBackendAd5xIfs::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }

    auto idx = static_cast<size_t>(slot_index);

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Update local state
        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
        }

        // Mark slot dirty to prevent parse_save_variables from overwriting our edit
        dirty_[idx] = true;

        // Convert color to hex string for our cached array
        char hex[7];
        snprintf(hex, sizeof(hex), "%06X", info.color_rgb & 0xFFFFFF);
        colors_[idx] = hex;

        // Normalize material to a value the IFS firmware will accept.
        // Empty input stays empty (an empty slot has no material), but any
        // non-empty input is coerced to the firmware whitelist so we never
        // send "PLA+" or "Silk PLA" and hit "Invalid material type".
        std::string normalized_material =
            info.material.empty() ? std::string{} : normalize_material(info.material);
        materials_[idx] = normalized_material;

        // Without per-port sensors, infer presence from user-provided data.
        // Setting color/material marks the slot occupied; clearing both marks it empty.
        if (!has_per_port_sensors_) {
            bool has_data =
                !normalized_material.empty() || info.color_rgb != AMS_DEFAULT_SLOT_COLOR;
            port_presence_[idx] = has_data;
        }

        spdlog::debug("{} set_slot_info: slot {} dirty=true, color={}, material={} (raw={}), "
                      "presence={}",
                      backend_log_tag(), slot_index, hex, normalized_material, info.material,
                      port_presence_[idx]);

        // Update entry directly. Covers every SlotInfo field the caller may
        // have set, not just the IFS-native color/material — otherwise a
        // persist=false "preview" write would silently drop brand /
        // spool_name / spoolman_* / color_name and the UI would snap back
        // to the previous values on the next get_slot_info().
        entry->info.color_rgb = info.color_rgb;
        entry->info.color_name = info.color_name;
        entry->info.material = normalized_material;
        entry->info.brand = info.brand;
        entry->info.spool_name = info.spool_name;
        entry->info.spoolman_id = info.spoolman_id;
        entry->info.spoolman_vendor_id = info.spoolman_vendor_id;
        entry->info.remaining_weight_g = info.remaining_weight_g;
        entry->info.total_weight_g = info.total_weight_g;

        // If the caller asked for persistence, stage the new override into
        // overrides_ BEFORE update_slot_from_state() — otherwise the call
        // below will re-apply the PRE-EDIT override (if any), snap brand /
        // spool_name / spoolman_id back to their old saved values, and
        // revert the user's edit visually until the next parse. The
        // override store's own save_async fires outside the lock further
        // down, so there's only one place that mutates overrides_ for
        // persist=true set_slot_info.
        if (persist) {
            helix::ams::FilamentSlotOverride ovr;
            ovr.brand = info.brand;
            ovr.spool_name = info.spool_name;
            ovr.spoolman_id = info.spoolman_id;
            ovr.spoolman_vendor_id = info.spoolman_vendor_id;
            ovr.remaining_weight_g = info.remaining_weight_g;
            ovr.total_weight_g = info.total_weight_g;
            ovr.color_rgb = info.color_rgb;
            ovr.color_name = info.color_name;
            // normalize_material() was already applied to the cached
            // materials_ copy; reuse it so the on-disk record carries the
            // firmware-valid value instead of the raw user-typed string.
            ovr.material = normalized_material;
            // SlotInfo carries the user's edit OR the bound Spoolman spool's
            // filament profile; the material-DB fallback for fields left at 0
            // is applied at emit time inside resolved_temps(). Centralized in
            // the helper so the four AMS backends stay in sync.
            helix::ams::populate_temps_from_slot_info(ovr, info);
            // updated_at left default — save_async stamps a fresh value so
            // the on-disk record's scan_time wins over any local clock skew.
            overrides_[slot_index] = ovr;
        }

        // Treat the user's chosen color as the new "firmware truth" baseline
        // so check_external_color_change() doesn't interpret the upcoming
        // update_slot_from_state() call as a foreign edit and fire a
        // redundant lane_data sync. The semantics match user intent: "I'm
        // telling the system this IS the current color." A subsequent
        // genuinely-external CHANGE_ZCOLOR will be detected against the
        // user's chosen color.
        //
        // Applies to both persist=true (override just staged above) and
        // persist=false (preview must not retrigger a sync against
        // last_firmware_color_). Guard on color_rgb != 0 — 0 means "no
        // color change" here and must not overwrite the baseline (same
        // contract as check_external_color_change treats a 0 reading as
        // "no signal").
        if (info.color_rgb != 0) {
            last_firmware_color_[slot_index] = info.color_rgb;
        }

        // Recalculate slot status now that port_presence may have changed.
        // update_slot_from_state() re-applies apply_overrides() from
        // overrides_ — which for persist=true now holds the values we
        // just staged above, so the override wins and matches the edit.
        // For persist=false with NO existing override, apply_overrides is
        // a no-op and the direct entry->info fields survive.
        update_slot_from_state(slot_index);
    }

    if (persist) {
        // Persist user-provided metadata to the slot-override store.
        //
        // Two persistence paths run here, by design:
        //
        //   1. IFS-native fields (color, material) are sent to Klipper via
        //      _IFS_VARS / Adventurer5M.json below — that's the printer-
        //      facing side the firmware and other UIs (Orca, LCD) see.
        //
        //   2. User metadata the firmware can't carry (brand, spool_name,
        //      spoolman_id, weights, color_name) lands in the Moonraker DB
        //      lane_data namespace via the override store. apply_overrides
        //      layers these back over firmware data on every parse.
        //
        // Color + material go into BOTH stores so an external writer
        // (Orca via its own MoonrakerPrinterAgent, another HelixScreen
        // instance) sees the full record in lane_data even when it's not
        // also listening to _IFS_VARS.
        if (override_store_) {
            // Re-read from overrides_ under the lock to get the same object
            // we staged above (including the normalized material). Cheap —
            // FilamentSlotOverride is a small POD-ish struct.
            helix::ams::FilamentSlotOverride ovr_to_save;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = overrides_.find(slot_index);
                if (it != overrides_.end()) {
                    ovr_to_save = it->second;
                }
            }
            // Capture backend_log_tag by value — the save callback may fire
            // well after set_slot_info returns (MR tracker ~60s timeout).
            // Do NOT capture `this`: the backend may outlive its store, but
            // the store will outlive the scheduled save by design.
            const std::string tag = backend_log_tag();
            override_store_->save_async(
                slot_index, ovr_to_save,
                [tag, slot_index](bool success, const std::string& err) {
                    if (!success) {
                        spdlog::warn("{} Override persist failed for slot {}: {}", tag, slot_index,
                                     err);
                    }
                });
        }

        // Write directly to Adventurer5M.json — zmod's authoritative store.
        // CHANGE_ZCOLOR is the macro-level equivalent but always emits the
        // Mainsail "Select print materials" prompt and (on display=True
        // setups) a native AD5X-screen popup, both of which the user must
        // dismiss manually. zmod re-reads Adventurer5M.json on every
        // GET_ZCOLOR call (no in-memory cache), so direct file writes are
        // picked up without ceremony.
        auto err = write_adventurer_json(slot_index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dirty_[idx] = false;
        }
        if (!err.success())
            return err;

        // lessWaste/bambufy users: also persist to the plugin's save_variables
        // store so its purge-skip logic sees consistent colors. zmod does not
        // read these — both writes are required for fully-synchronized state.
        // Best-effort: a failure here doesn't fail the operation because zmod's
        // truth (Adventurer5M.json) is already current.
        if (has_ifs_vars_) {
            std::string colors_val;
            std::string types_val;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                colors_val = build_color_list_value();
                types_val = build_type_list_value();
            }
            auto colors_err = write_ifs_var("colors", colors_val);
            if (!colors_err.success()) {
                spdlog::warn("{} _IFS_VARS colors write failed for slot {}: {}",
                             backend_log_tag(), slot_index, colors_err.technical_msg);
            }
            auto types_err = write_ifs_var("types", types_val);
            if (!types_err.success()) {
                spdlog::warn("{} _IFS_VARS types write failed for slot {}: {}",
                             backend_log_tag(), slot_index, types_err.technical_msg);
            }
        }
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendAd5xIfs::set_tool_mapping(int tool_number, int slot_index) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_parameter("Invalid tool number");
    }

    int port = (slot_index >= 0 && slot_index < NUM_PORTS) ? (slot_index + 1) : UNMAPPED_PORT;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_map_[static_cast<size_t>(tool_number)] = port;

        // Update SlotRegistry reverse mapping
        for (int i = 0; i < NUM_PORTS; ++i) {
            int tool = find_first_tool_for_port(i + 1);
            slots_.set_tool_mapping(i, tool);
        }
    }

    // Persist tool mapping (only for lessWaste/bambufy — native ZMOD manages
    // tool mapping internally via the COLOR/SET_ZCOLOR dialog)
    if (!has_ifs_vars_) {
        return AmsErrorHelper::success();
    }

    std::string tools_val;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_val = build_tool_map_value();
    }

    return write_ifs_var("tools", tools_val);
}

// --- Bypass ---

AmsError AmsBackendAd5xIfs::enable_bypass() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    spdlog::info("{} Enabling bypass (external) mode", backend_log_tag());
    if (!has_ifs_vars_) {
        // Native ZMOD has no external/bypass mode variable — update local state only
        std::lock_guard<std::mutex> lock(mutex_);
        external_mode_ = true;
        return AmsErrorHelper::success();
    }
    return write_ifs_var("external", "1");
}

AmsError AmsBackendAd5xIfs::disable_bypass() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    spdlog::info("{} Disabling bypass (external) mode", backend_log_tag());
    if (!has_ifs_vars_) {
        std::lock_guard<std::mutex> lock(mutex_);
        external_mode_ = false;
        return AmsErrorHelper::success();
    }
    return write_ifs_var("external", "0");
}

// --- Private helpers ---

std::string AmsBackendAd5xIfs::build_color_list_value() const {
    // Build Python list literal for _IFS_VARS macro.
    // Outer double quotes delimit the G-code parameter value (Klipper strips them).
    // _IFS_VARS passes the inner content to SAVE_VARIABLE, adding its own quoting.
    // Single quotes for string elements inside the list.
    // Example: "['FF0000', '00FF00', '0000FF', 'FFFFFF']"
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < NUM_PORTS; ++i) {
        if (i > 0)
            ss << ", ";
        ss << "'" << colors_[static_cast<size_t>(i)] << "'";
    }
    ss << "]\"";
    return ss.str();
}

std::string AmsBackendAd5xIfs::build_type_list_value() const {
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < NUM_PORTS; ++i) {
        if (i > 0)
            ss << ", ";
        ss << "'" << materials_[static_cast<size_t>(i)] << "'";
    }
    ss << "]\"";
    return ss.str();
}

std::string AmsBackendAd5xIfs::build_tool_map_value() const {
    // Integer list — no quotes around elements.
    // Example: "[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]"
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < TOOL_MAP_SIZE; ++i) {
        if (i > 0)
            ss << ", ";
        ss << tool_map_[static_cast<size_t>(i)];
    }
    ss << "]\"";
    return ss.str();
}

AmsError AmsBackendAd5xIfs::write_ifs_var(const std::string& key, const std::string& value) {
    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    // Use _IFS_VARS macro to persist state — works for both lessWaste and bambufy.
    // The macro updates in-memory gcode variables AND writes SAVE_VARIABLE with the
    // correct prefix automatically.
    std::string gcode = "_IFS_VARS " + key + "=" + value;
    spdlog::debug("{} Writing IFS var: {} = {}", backend_log_tag(), key, value);
    return execute_gcode(gcode);
}

AmsError AmsBackendAd5xIfs::write_adventurer_json(int slot_index) {
    // Same-host fast path: write the file via direct filesystem access.
    // Avoids Moonraker's HTTP upload, which on AD5X stock-ZMOD does an
    // os.rename across mount points (/root/printer_data/tmp →
    // /usr/prog/config via symlink) and corrupts the file with EXDEV.
    // Bundle DQK7X96B (v0.99.52) was a bricked Klipper at boot from this
    // exact failure mode.
    if (!local_adventurer_json_path_.empty()) {
        return write_adventurer_json_local(slot_index);
    }

    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    auto idx = static_cast<size_t>(slot_index);
    int port = slot_index + 1; // JSON uses 1-based slot numbering

    std::string hex, material;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hex = colors_[idx];
        material = materials_[idx];
    }

    if (hex.empty())
        hex = "808080";
    if (material.empty())
        material = "PLA";

    // Add # prefix for JSON file format
    std::string hex_with_hash = "#" + hex;

    spdlog::info("{} Writing slot {} to Adventurer5M.json (native ZMOD)", backend_log_tag(), port);

    // Read-modify-write: download current file, update slot, re-upload
    auto result = std::make_shared<AmsError>(AmsErrorHelper::success());
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto token = lifetime_.token();

    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token, port, hex_with_hash, material, result, done](const std::string& content) {
            if (token.expired()) {
                *result =
                    AmsErrorHelper::command_failed("write_adventurer_json", "Connection lost");
                done->store(true);
                return;
            }

            json doc;
            try {
                doc = json::parse(content);
            } catch (const json::parse_error& e) {
                spdlog::warn("{} Failed to parse Adventurer5M.json for write: {}",
                             backend_log_tag(), e.what());
                *result =
                    AmsErrorHelper::command_failed("write_adventurer_json", "JSON parse error");
                done->store(true);
                return;
            }

            // Ensure FFMInfo exists
            if (!doc.contains("FFMInfo")) {
                doc["FFMInfo"] = json::object();
            }

            // Update the slot
            doc["FFMInfo"]["ffmColor" + std::to_string(port)] = hex_with_hash;
            doc["FFMInfo"]["ffmType" + std::to_string(port)] = material;

            // Serialize with indentation to match zmod's format
            std::string updated = doc.dump(4);

            api_->transfers().upload_file(
                "config", "Adventurer5M.json", updated,
                [this, done, port]() {
                    spdlog::info("{} Wrote slot {} to Adventurer5M.json", backend_log_tag(), port);
                    done->store(true);
                },
                [this, result, done, port](const MoonrakerError& err) {
                    spdlog::warn("{} Failed to upload Adventurer5M.json for slot {}: {}",
                                 backend_log_tag(), port, err.message);
                    *result = AmsErrorHelper::command_failed("write_adventurer_json", err.message);
                    done->store(true);
                });
        },
        [this, result, done](const MoonrakerError& err) {
            spdlog::warn("{} Failed to download Adventurer5M.json for write: {}", backend_log_tag(),
                         err.message);
            *result = AmsErrorHelper::command_failed("write_adventurer_json", err.message);
            done->store(true);
        });

    // Wait for async operation to complete (this is called from a sync API)
    // The existing write_ifs_var / execute_gcode also blocks, so this is consistent.
    for (int i = 0; i < 100 && !done->load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!done->load()) {
        return AmsErrorHelper::command_failed("write_adventurer_json",
                                              "Timeout writing Adventurer5M.json");
    }

    return *result;
}

AmsError AmsBackendAd5xIfs::write_adventurer_json_local(int slot_index) {
    if (local_adventurer_json_path_.empty()) {
        return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                              "Local path not resolved");
    }

    auto idx = static_cast<size_t>(slot_index);
    int port = slot_index + 1;

    std::string hex, material;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hex = colors_[idx];
        material = materials_[idx];
    }
    if (hex.empty())
        hex = "808080";
    if (material.empty())
        material = "PLA";
    const std::string hex_with_hash = "#" + hex;

    // Read-modify-write. An empty / unparseable existing file is treated as
    // "fresh start with empty FFMInfo" so we auto-repair the bricked-printer
    // case (corrupted Adventurer5M.json from a prior EXDEV upload — the very
    // failure mode this code path exists to fix).
    json doc;
    {
        std::ifstream in(local_adventurer_json_path_);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string content = buf.str();
        if (content.empty()) {
            doc = json::object();
        } else {
            try {
                doc = json::parse(content);
            } catch (const json::parse_error&) {
                spdlog::warn("{} Adventurer5M.json at {} is unparseable; rewriting from scratch",
                             backend_log_tag(), local_adventurer_json_path_);
                doc = json::object();
            }
        }
    }

    if (!doc.contains("FFMInfo") || !doc["FFMInfo"].is_object()) {
        doc["FFMInfo"] = json::object();
    }
    doc["FFMInfo"]["ffmColor" + std::to_string(port)] = hex_with_hash;
    doc["FFMInfo"]["ffmType" + std::to_string(port)] = material;

    const std::string updated = doc.dump(4);

    // Atomic write: stage to <path>.tmp in the same directory, then rename().
    // POSIX rename() is atomic when src+dst are on the same filesystem — that's
    // guaranteed here because both live in the same directory. Critically, we
    // do NOT cross filesystems the way Moonraker's upload does.
    const std::string tmp_path = local_adventurer_json_path_ + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                                  std::string("open(") + tmp_path +
                                                      ") failed: " + std::strerror(errno));
        }
        out << updated;
        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                                  "Write failed");
        }
    }

    if (std::rename(tmp_path.c_str(), local_adventurer_json_path_.c_str()) != 0) {
        const int saved_errno = errno;
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                              std::string("rename failed: ") +
                                                  std::strerror(saved_errno));
    }

    spdlog::info("{} Wrote slot {} to Adventurer5M.json (direct fs path: {})", backend_log_tag(),
                 port, local_adventurer_json_path_);
    return AmsErrorHelper::success();
}

void AmsBackendAd5xIfs::detect_local_adventurer_json_path() {
    // Only meaningful when Moonraker runs on the same host — otherwise
    // /usr/prog/config/ is on a remote filesystem we can't touch.
    std::string moonraker_host;
    if (helix::Config* cfg = helix::Config::get_instance()) {
        moonraker_host =
            cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    }
    if (!helix::is_moonraker_on_same_host(moonraker_host)) {
        spdlog::debug("{} Moonraker is remote ({}); leaving Adventurer5M.json on upload path",
                      backend_log_tag(), moonraker_host);
        return;
    }

    // Candidate paths in priority order. /usr/prog/config is the AD5X stock-ZMOD
    // canonical install. /opt/config/Adventurer5M.json is where ZMOD-on-ForgeX
    // stages it. Both are the EXDEV-prone destinations on AD5X stock — that's
    // the entire reason we want direct write.
    static constexpr std::array<const char*, 2> candidates = {
        "/usr/prog/config/Adventurer5M.json",
        "/opt/config/Adventurer5M.json",
    };

    for (const auto* candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) {
            continue;
        }
        // Must be a regular file (or a symlink resolving to one) and writable.
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
            continue;
        }
        if (::access(candidate, W_OK) != 0) {
            spdlog::debug("{} {} exists but is not writable (errno={}); skipping",
                          backend_log_tag(), candidate, errno);
            continue;
        }
        local_adventurer_json_path_ = candidate;
        spdlog::info("{} Resolved Adventurer5M.json local path: {} "
                     "(bypasses Moonraker upload to avoid EXDEV on /usr/prog/config symlink)",
                     backend_log_tag(), candidate);
        return;
    }

    spdlog::debug("{} No local Adventurer5M.json candidate found; staying on Moonraker upload path",
                  backend_log_tag());
}

void AmsBackendAd5xIfs::fetch_user_cfg_materials() {
    if (!api_)
        return;

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "mod_data/user.cfg",
        [this, token](const std::string& content) {
            if (token.expired())
                return;
            auto names = parse_user_cfg_filament_types(content);
            if (names.empty()) {
                spdlog::debug("{} user.cfg parsed: no [zmod_ifs] filament_* entries",
                              backend_log_tag());
                return;
            }
            // Append new names, preserving existing bambufy_custom_types order
            // and de-duplicating case-insensitively.
            auto lower = [](const std::string& s) {
                std::string out = s;
                std::transform(out.begin(), out.end(), out.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                return out;
            };
            size_t total;
            {
                std::lock_guard<std::mutex> lock(custom_types_mutex_);
                for (const auto& n : names) {
                    std::string n_lc = lower(n);
                    bool exists = false;
                    for (const auto& existing : custom_material_types_) {
                        if (lower(existing) == n_lc) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        custom_material_types_.push_back(n);
                    }
                }
                total = custom_material_types_.size();
            }
            spdlog::info("{} user.cfg: loaded {} user-defined filament type(s); total custom types {}",
                         backend_log_tag(), names.size(), total);
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                spdlog::debug("{} user.cfg not present (404) — no user-defined types to merge",
                              backend_log_tag());
            } else {
                spdlog::debug("{} user.cfg fetch failed: {}", backend_log_tag(), err.message);
            }
        });
}

std::vector<std::string>
AmsBackendAd5xIfs::parse_user_cfg_filament_types(const std::string& body) {
    // zmod docs: https://wiki.zmod.link/AD5X/#7-add-custom-filament-types
    //
    //   [zmod_ifs]
    //   filament_NEWTYPE: 300
    //
    // Section is Klipper-style INI. Comments start with '#' or ';'. Values
    // are decimal temperatures we don't currently use — we only collect the
    // NAME tokens (uppercased convention, but preserve user case as written).
    // Any whitespace before the section header or key is tolerated; we don't
    // try to fully reimplement Klipper's INI parser, just match the lines we
    // care about within the [zmod_ifs] section.
    std::vector<std::string> out;
    std::istringstream is(body);
    std::string line;
    bool in_section = false;
    static const std::regex section_re(R"(^\s*\[\s*([^\]\s]+)\s*\]\s*$)");
    static const std::regex filament_re(R"(^\s*filament_([A-Za-z0-9_+\-]+)\s*[:=].*$)");
    while (std::getline(is, line)) {
        // Strip trailing CR for files saved with CRLF.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Strip inline comments — Klipper accepts both '#' and ';'.
        for (char ch : {'#', ';'}) {
            auto pos = line.find(ch);
            if (pos != std::string::npos) {
                line.erase(pos);
                break;
            }
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        std::smatch m;
        if (std::regex_match(line, m, section_re)) {
            in_section = (m[1].str() == "zmod_ifs");
            continue;
        }
        if (in_section && std::regex_match(line, m, filament_re)) {
            out.push_back(m[1].str());
        }
    }
    return out;
}

void AmsBackendAd5xIfs::read_adventurer_json() {
    if (!api_)
        return;

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token](const std::string& content) {
            if (token.expired())
                return;
            spdlog::debug("{} Downloaded Adventurer5M.json ({} bytes)", backend_log_tag(),
                          content.size());
            // Baseline the poll cache so the next periodic poll doesn't see
            // this content as "changed" and double-fire GET_ZCOLOR.
            (void)note_json_content(content);
            parse_adventurer_json(content);
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                json_poll_supported_.store(false);
                spdlog::info("{} Adventurer5M.json not found — not a native ZMOD AD5X",
                             backend_log_tag());
            } else {
                spdlog::warn("{} Failed to download Adventurer5M.json: {}", backend_log_tag(),
                             err.message);
            }
        });
}

bool AmsBackendAd5xIfs::note_json_content(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (content == last_json_content_) {
        return false;
    }
    last_json_content_ = content;
    return true;
}

void AmsBackendAd5xIfs::poll_adventurer_json() {
    if (!api_ || !json_poll_supported_.load())
        return;
    if (json_poll_in_flight_.exchange(true)) {
        return; // Previous poll still running — coalesce.
    }

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token](const std::string& content) {
            json_poll_in_flight_.store(false);
            if (token.expired())
                return;

            if (!note_json_content(content)) {
                spdlog::trace("{} Adventurer5M.json unchanged ({} bytes), skipping GET_ZCOLOR",
                              backend_log_tag(), content.size());
                return;
            }

            spdlog::debug("{} Adventurer5M.json changed ({} bytes), parsing + scheduling GET_ZCOLOR",
                          backend_log_tag(), content.size());
            parse_adventurer_json(content);
            schedule_zcolor_query();
        },
        [this, token](const MoonrakerError& err) {
            json_poll_in_flight_.store(false);
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                json_poll_supported_.store(false);
                spdlog::info("{} Adventurer5M.json poll: file not found, disabling poll",
                             backend_log_tag());
            } else {
                spdlog::debug("{} Adventurer5M.json poll failed (will retry): {}",
                              backend_log_tag(), err.message);
            }
        });
}

bool AmsBackendAd5xIfs::on_gcode_response_line(const std::string& line) {
    // Lines arriving while our own GET_ZCOLOR is in flight belong to that
    // response — buffer them and DO NOT treat as external triggers. zmod's
    // GET_ZCOLOR macro body itself echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens;
    // without this guard the listener self-feeds another schedule_zcolor_query
    // on every response, producing a 2-4 Hz spam loop on the gcode console
    // (raza/DIEHARDave report on v0.99.51).
    if (zcolor_query_active_.load()) {
        std::lock_guard<std::mutex> lock(zcolor_buffer_mutex_);
        zcolor_response_buffer_.push_back(line);
        return true;
    }

    if (line.find("RUN_ZCOLOR") != std::string::npos ||
        line.find("CHANGE_ZCOLOR") != std::string::npos) {
        spdlog::debug("{} Detected external color change in gcode stream, "
                      "scheduling re-read + zcolor query",
                      backend_log_tag());
        schedule_json_reread();
        schedule_zcolor_query();
    }
    return false;
}

void AmsBackendAd5xIfs::register_zcolor_listener() {
    if (!client_)
        return;

    static const std::string handler_name = "ifs_zcolor_watcher";
    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_gcode_response", handler_name, [this, token](const json& msg) {
            if (token.expired())
                return;

            std::string line;
            if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
                msg["params"][0].is_string()) {
                line = msg["params"][0].get<std::string>();
            } else if (msg.is_array() && !msg.empty() && msg[0].is_string()) {
                line = msg[0].get<std::string>();
            } else {
                return;
            }

            on_gcode_response_line(line);
        });
}

void AmsBackendAd5xIfs::register_klippy_ready_listener() {
    if (!client_)
        return;

    // notify_klippy_ready fires on every klippy startup->ready transition (cold
    // boot once klippy finishes initialising, and after FIRMWARE_RESTART). This
    // is the point at which zmod has actually come online and GET_ZCOLOR
    // SILENT=1 can return per-slot state.
    static const std::string handler_name = "ifs_klippy_ready_watcher";
    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_klippy_ready", handler_name, [this, token](const json& /*msg*/) {
            if (token.expired())
                return;
            spdlog::info("{} notify_klippy_ready — scheduling GET_ZCOLOR SILENT=1",
                         backend_log_tag());
            // Re-read the JSON cache too — firmware may have persisted new
            // colors during boot, and the stream may have missed RUN_ZCOLOR
            // notifications that happened before we reconnected.
            schedule_json_reread();
            schedule_zcolor_query();
        });
}

void AmsBackendAd5xIfs::unregister_moonraker_listeners() {
    if (!client_)
        return;
    client_->unregister_method_callback("notify_gcode_response", "ifs_zcolor_watcher");
    client_->unregister_method_callback("notify_klippy_ready", "ifs_klippy_ready_watcher");
}

void AmsBackendAd5xIfs::schedule_json_reread() {
    if (reread_pending_.exchange(true))
        return;

    auto token = lifetime_.token();

    // Bounded worker pool — bare std::thread on AD5M can hit EAGAIN under
    // thread exhaustion (feedback_no_bare_threads_arm.md).
    helix::http::HttpExecutor::fast().submit([this, token]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (token.expired())
            return;
        reread_pending_.store(false);
        spdlog::debug("{} Re-reading Adventurer5M.json after external change", backend_log_tag());
        read_adventurer_json();
    });
}

void AmsBackendAd5xIfs::schedule_zcolor_query() {
    if (!zcolor_silent_supported_.load()) {
        return; // Session flagged unsupported; don't retry.
    }
    zcolor_schedule_count_.fetch_add(1, std::memory_order_relaxed);
    // Semantics of zcolor_query_pending_: "a refresh is wanted". Set here and
    // re-set by query_zcolor_silent() when it can't run (active in flight).
    // Cleared on claim by the first worker to wake OR by finalize_zcolor_response.
    zcolor_query_pending_.store(true);

    auto token = lifetime_.token();
    helix::http::HttpExecutor::fast().submit([this, token]() {
        // Short debounce — coalesce bursts from port-sensor changes, the
        // gcode stream, and unload-complete triggers. Multiple workers may
        // wake concurrently; only one claims via exchange.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (token.expired())
            return;
        if (!zcolor_query_pending_.exchange(false)) {
            return; // Another worker (or finalize) already claimed this refresh.
        }
        query_zcolor_silent();
    });
}

void AmsBackendAd5xIfs::query_zcolor_silent() {
    if (!api_ || !zcolor_silent_supported_.load())
        return;
    if (zcolor_query_active_.exchange(true)) {
        // Already in flight — mark pending so finalize will re-schedule.
        zcolor_query_pending_.store(true);
        spdlog::debug("{} GET_ZCOLOR SILENT=1 already in flight, deferring", backend_log_tag());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(zcolor_buffer_mutex_);
        zcolor_response_buffer_.clear();
    }

    spdlog::debug("{} Querying GET_ZCOLOR SILENT=1", backend_log_tag());
    auto token = lifetime_.token();
    api_->execute_gcode(
        "GET_ZCOLOR SILENT=1",
        [this, token]() {
            // Response lines arrive via notify_gcode_response listener;
            // schedule finalization after a short collection window.
            if (token.expired())
                return;
            helix::http::HttpExecutor::fast().submit([this, token]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (token.expired())
                    return;
                finalize_zcolor_response();
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            spdlog::warn("{} GET_ZCOLOR SILENT=1 failed: {}", backend_log_tag(), err.message);
            zcolor_query_active_.store(false);
        });
}

void AmsBackendAd5xIfs::finalize_zcolor_response() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(zcolor_buffer_mutex_);
        lines.swap(zcolor_response_buffer_);
    }
    zcolor_query_active_.store(false);

    if (!lines.empty()) {
        auto result = parse_zcolor_silent(lines);
        apply_zcolor_result(result);
    } else {
        spdlog::debug("{} GET_ZCOLOR SILENT=1 returned no lines", backend_log_tag());
    }

    // If a trigger arrived during the active window, query again directly —
    // we just finished a query, no need to debounce further.
    if (zcolor_query_pending_.exchange(false)) {
        spdlog::debug("{} Re-querying GET_ZCOLOR SILENT=1 (trigger fired during last query)",
                      backend_log_tag());
        query_zcolor_silent();
    }
}

void AmsBackendAd5xIfs::apply_zcolor_result(const ZColorSilentResult& result) {
    if (result.is_prompt_fallback) {
        if (zcolor_silent_supported_.exchange(false)) {
            spdlog::warn("{} zmod returned a prompt dialog for GET_ZCOLOR SILENT=1 — "
                         "old zmod, falling back to Adventurer5M.json polling",
                         backend_log_tag());
        }
        return;
    }

    if (!result.saw_valid_response) {
        // Response arrived but contained no summary or slot lines we recognise
        // (transient timing, malformed response, etc.). Don't wipe presence
        // on what might be incomplete data — wait for the next trigger.
        spdlog::debug("{} GET_ZCOLOR SILENT=1 yielded no recognisable content, "
                      "skipping apply",
                      backend_log_tag());
        return;
    }

    if (result.is_old_format) {
        spdlog::debug("{} GET_ZCOLOR SILENT=1 returned no /HEX segments "
                      "(pre-ad2802ab zmod) — presence only, colors from JSON",
                      backend_log_tag());
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < NUM_PORTS; ++i) {
            const auto idx = static_cast<size_t>(i);
            const auto& parsed = result.slots[idx];
            const bool loaded = parsed.has_value();

            // Live per-port sensors (lessWaste/bambufy) are authoritative for
            // presence — don't let zmod's view race against them. Native ZMOD
            // has no exposed per-port sensor, so we must use zmod's view.
            if (!has_per_port_sensors_ && port_presence_[idx] != loaded) {
                port_presence_[idx] = loaded;
                changed = true;
            }

            // Fill in color/material if we got them and the slot isn't locally
            // dirty (unsaved user edit pending).
            if (!loaded || result.is_old_format || dirty_[idx]) {
                continue;
            }
            if (!parsed->hex.empty() && colors_[idx] != parsed->hex) {
                colors_[idx] = parsed->hex;
                changed = true;
            }
            if (!parsed->material.empty() && materials_[idx] != parsed->material) {
                materials_[idx] = parsed->material;
                changed = true;
            }
        }

        // Active tool: GET_ZCOLOR's "Extruder:" line is zmod's live view of
        // which slot is in the extruder. lessWaste/bambufy users get this
        // from save_variables (<prefix>_current_tool), so leave them alone.
        // Stock-ZMOD users have no other source — without this, active_tool_
        // is permanently stuck at -1 and the UI never shows which lane is
        // loaded (raza616's report against v0.99.50).
        if (!has_ifs_vars_) {
            int new_active_tool = -1;
            if (result.extruder_slot.has_value()) {
                int port = *result.extruder_slot + 1;
                new_active_tool = find_first_tool_for_port(port);
            }
            if (active_tool_ != new_active_tool) {
                active_tool_ = new_active_tool;
                changed = true;
            }
        }

        if (changed) {
            for (int i = 0; i < NUM_PORTS; ++i) {
                update_slot_from_state(i);
            }
        }
    } // release lock before emit_event (which also takes mutex_)

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

AmsBackendAd5xIfs::ZColorSilentResult
AmsBackendAd5xIfs::parse_zcolor_silent(const std::vector<std::string>& lines) {
    ZColorSilentResult result;

    // Regexes compiled once per call; parsing is off the hot path.
    // Summary: "// Extruder: None (N) | IFS: True"
    //   or:    "// Extruder: N: MATERIAL/HEX | IFS: True"
    static const std::regex summary_re(R"(^//\s*Extruder:\s*(.+?)\s*\|\s*IFS:\s*(True|False)\s*$)");
    // Slot: "// N: MATERIAL/HEX" or "// N: MATERIAL/NAME/HEX" or old "// N: MATERIAL"
    static const std::regex slot_re(R"(^//\s*([1-9])\s*:\s*(.+?)\s*$)");
    // Extruder detail inside summary text: "N: MATERIAL/..."
    static const std::regex extruder_slot_re(R"(^([1-9])\s*:)");
    // current channel: "None (N)" or bare "N" form — look for "(N)" paren form
    static const std::regex channel_paren_re(R"(\((\d+)\))");

    for (const auto& raw : lines) {
        if (raw.find("action:prompt_") != std::string::npos) {
            result.is_prompt_fallback = true;
            return result;
        }
    }

    int slot_lines_seen = 0;
    int slot_lines_with_hex = 0;
    std::smatch m;

    for (const auto& line : lines) {
        if (std::regex_match(line, m, summary_re)) {
            result.saw_valid_response = true;
            result.ifs_active = (m[2].str() == "True");
            const std::string extruder_part = m[1].str();

            std::smatch em;
            if (std::regex_search(extruder_part, em, extruder_slot_re)) {
                try {
                    int n = std::stoi(em[1].str());
                    if (n >= 1 && n <= NUM_PORTS) {
                        result.extruder_slot = n - 1; // 0-based
                    }
                } catch (...) {
                }
            }
            std::smatch cm;
            if (std::regex_search(extruder_part, cm, channel_paren_re)) {
                try {
                    result.current_channel = std::stoi(cm[1].str());
                } catch (...) {
                }
            }
            continue;
        }

        if (std::regex_match(line, m, slot_re)) {
            int n;
            try {
                n = std::stoi(m[1].str());
            } catch (...) {
                continue;
            }
            if (n < 1 || n > NUM_PORTS) {
                continue; // slot number out of range — skip (e.g. "// 99: nonsense")
            }

            result.saw_valid_response = true;
            slot_lines_seen++;

            // Slot line body is "MATERIAL" or "MATERIAL/HEX" or "MATERIAL/NAME/HEX".
            // Rule: material is everything before the first '/', hex is everything
            // after the LAST '/'. Anything between is a COLOR_MAPPING alias we ignore.
            const std::string rest = m[2].str();
            const size_t first_slash = rest.find('/');
            const size_t last_slash = rest.rfind('/');

            ZColorSlot slot;
            if (first_slash == std::string::npos) {
                // Old format: just material, no /HEX.
                slot.material = rest;
            } else {
                slot.material = rest.substr(0, first_slash);
                slot.hex = rest.substr(last_slash + 1);
                slot_lines_with_hex++;
            }
            result.slots[static_cast<size_t>(n - 1)] = std::move(slot);
        }
    }

    // Old format detection: we saw slot lines but none had a /HEX segment.
    if (slot_lines_seen > 0 && slot_lines_with_hex == 0) {
        result.is_old_format = true;
    }

    return result;
}

void AmsBackendAd5xIfs::parse_adventurer_json(const std::string& content) {
    json doc;
    try {
        doc = json::parse(content);
    } catch (const json::parse_error& e) {
        spdlog::warn("{} Failed to parse Adventurer5M.json: {}", backend_log_tag(), e.what());
        return;
    }

    auto ffm_it = doc.find("FFMInfo");
    if (ffm_it == doc.end() || !ffm_it->is_object()) {
        spdlog::debug("{} No FFMInfo section in Adventurer5M.json", backend_log_tag());
        return;
    }
    const auto& ffm = *ffm_it;

    int parsed_count = 0;
    bool needs_ifs_vars_push = false;
    std::string ifs_colors_payload;
    std::string ifs_types_payload;
    std::string ifs_var_prefix_snapshot;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t pre_sync_count = external_sync_count_;

        // Ports 1-4 in JSON map to slots 0-3
        for (int port = 1; port <= NUM_PORTS; ++port) {
            std::string color_key = "ffmColor" + std::to_string(port);
            std::string type_key = "ffmType" + std::to_string(port);

            auto color_it = ffm.find(color_key);
            auto type_it = ffm.find(type_key);
            if (color_it == ffm.end() && type_it == ffm.end())
                continue;

            // Extract color — strip '#' prefix, default to gray if empty
            std::string hex;
            if (color_it != ffm.end() && color_it->is_string()) {
                hex = color_it->get<std::string>();
            }
            if (!hex.empty() && hex[0] == '#') {
                hex = hex.substr(1);
            }
            // Non-empty color means filament was loaded into this port
            bool has_filament_data = !hex.empty();
            if (hex.empty()) {
                hex = "808080";
            }
            // Uppercase
            for (auto& c : hex)
                c = static_cast<char>(toupper(c));

            // Extract material type
            std::string type;
            if (type_it != ffm.end() && type_it->is_string()) {
                type = type_it->get<std::string>();
            }

            int idx = port - 1;
            if (dirty_[static_cast<size_t>(idx)])
                continue;
            colors_[static_cast<size_t>(idx)] = hex;
            materials_[static_cast<size_t>(idx)] = type;

            // Native ZMOD has no per-port switch sensors — infer presence from
            // Adventurer5M.json data. A non-empty color means filament is present;
            // an empty color means the spool has been fully ejected. Only act on
            // eject when the system is IDLE so a transient read mid-unload cannot
            // wipe presence (#631 follow-up).
            if (!has_per_port_sensors_) {
                auto& presence = port_presence_[static_cast<size_t>(idx)];
                if (has_filament_data) {
                    presence = true;
                } else if (presence && system_info_.action == AmsAction::IDLE) {
                    spdlog::info("{} Slot {} eject detected (empty color in "
                                 "Adventurer5M.json)",
                                 backend_log_tag(), idx);
                    presence = false;
                    // Spool was physically removed: clear the user override
                    // so brand/spool_name/spoolman_id from the now-gone spool
                    // don't haunt the empty slot or get re-applied to whatever
                    // is loaded next. update_slot_from_state below would
                    // otherwise see the placeholder #808080 read and create a
                    // phantom override — sync_override_to_firmware_locked
                    // skips slots where port_presence is false.
                    if (auto* eject_entry = slots_.get_mut(idx)) {
                        clear_override_locked(idx, eject_entry->info);
                    }
                    needs_ifs_vars_push = true;
                }
            }

            update_slot_from_state(idx);
            ++parsed_count;
        }

        // sync_override_to_firmware_locked (called via update_slot_from_state →
        // check_external_color_change) bumps external_sync_count_ on every
        // accepted external edit. If anything synced or anything ejected,
        // mirror the new colors_/materials_ snapshot into the
        // lessWaste/bambufy plugin's _IFS_VARS save_variables — those don't
        // self-sync against zmod's truth (audited 2026-05-04: neither
        // Hrybmo/lesswaste nor function3d/bambufy listen for CHANGE_ZCOLOR).
        // Without this push, the plugin's runout-recovery alternate-port
        // lookup, smart-purge skip decision, and color-assign dialog all run
        // against stale data and silently print the wrong color or skip the
        // wrong purge.
        needs_ifs_vars_push = needs_ifs_vars_push || (external_sync_count_ > pre_sync_count);
        if (needs_ifs_vars_push && has_ifs_vars_) {
            ifs_colors_payload = build_color_list_value();
            ifs_types_payload = build_type_list_value();
            ifs_var_prefix_snapshot = var_prefix_;
        }
    } // release lock before emit_event + write_ifs_var (both take mutex_)

    if (needs_ifs_vars_push && has_ifs_vars_) {
        // Suppress the noisy `// Colors : [...]` / `// Types : [...]` echo on
        // bambufy (bambufy's _IFS_VARS macro accepts SHOW=0 to skip the
        // RESPOND line; lessWaste does not). Echo on lessWaste is debounced
        // by kJsonPollInterval (5s).
        const std::string suffix = (ifs_var_prefix_snapshot == "bambufy") ? " SHOW=0" : "";
        auto colors_err =
            execute_gcode("_IFS_VARS colors=" + ifs_colors_payload + suffix);
        if (!colors_err.success()) {
            spdlog::warn("{} _IFS_VARS colors mirror failed: {}", backend_log_tag(),
                         colors_err.technical_msg);
        }
        auto types_err = execute_gcode("_IFS_VARS types=" + ifs_types_payload + suffix);
        if (!types_err.success()) {
            spdlog::warn("{} _IFS_VARS types mirror failed: {}", backend_log_tag(),
                         types_err.technical_msg);
        }
    }

    if (parsed_count > 0) {
        spdlog::info("{} Loaded {} slots from Adventurer5M.json (native ZMOD)", backend_log_tag(),
                     parsed_count);
        emit_event(EVENT_STATE_CHANGED);
    } else {
        spdlog::debug("{} No slot data found in Adventurer5M.json", backend_log_tag());
    }
}

void AmsBackendAd5xIfs::detect_load_unload_completion(bool head_detected) {
    if (system_info_.action == AmsAction::LOADING && head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Load complete (head sensor triggered)", backend_log_tag());
        PostOpCooldownManager::instance().schedule();
        schedule_zcolor_query();
    } else if (system_info_.action == AmsAction::UNLOADING && !head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Unload complete (head sensor cleared)", backend_log_tag());
        PostOpCooldownManager::instance().schedule();
        schedule_zcolor_query();
    }
}

int AmsBackendAd5xIfs::find_first_tool_for_port(int port_1based) const {
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        if (tool_map_[static_cast<size_t>(t)] == port_1based) {
            return t;
        }
    }
    return -1; // No tool mapped to this port
}

bool AmsBackendAd5xIfs::validate_slot_index(int slot_index) const {
    return slot_index >= 0 && slot_index < NUM_PORTS;
}

// ensure_homed_then() provided by AmsSubscriptionBackend

void AmsBackendAd5xIfs::check_action_timeout() {
    if (system_info_.action != AmsAction::LOADING && system_info_.action != AmsAction::UNLOADING) {
        return;
    }

    auto elapsed = std::chrono::steady_clock::now() - action_start_time_;
    if (elapsed >= std::chrono::seconds(ACTION_TIMEOUT_SECONDS)) {
        spdlog::warn("{} {} timed out after {}s, resetting to IDLE", backend_log_tag(),
                     ams_action_to_string(system_info_.action),
                     std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        system_info_.action = AmsAction::IDLE;
    }
}

#endif // HELIX_HAS_IFS

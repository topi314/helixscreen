// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_qidi.h"

#include "ams_error.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <string>

// Stub backend for the QIDI Box filament changer. Read-path mirrors
// save_variables onto AmsSystemInfo; write-path (load/unload/change_tool)
// is still not implemented pending field-test access (issue #954 brought
// the protocol reference; hardware validation still gated on Sib6019).
//
// TODO(qidi-box): drop a `qidi_box_64.png` (and matching .svg / `_512.png`
// if other backends carry them) into assets/images/ams/ to match the logo
// convention used by afc_64.png, box_turtle_64.png, happy_hare_64.png, etc.
// The QIDI wordmark / box silhouette is fine — no in-app scaling required.

namespace {
// Parse `"slot<N>"` into N when valid and within [0, slot_count).
// Returns nullopt for the box_extras.py sentinel `"slot-1"` (nothing
// loaded) and for any other malformed input. Used to decode the
// `value_t<T>` and `last_load_slot` save_variables, both of which carry
// slot references in this format.
std::optional<int> parse_slot_name(const std::string& val, int slot_count) {
    if (val.rfind("slot", 0) != 0) {
        return std::nullopt;
    }
    try {
        int idx = std::stoi(val.substr(4));
        if (idx >= 0 && idx < slot_count) {
            return idx;
        }
    } catch (const std::exception&) {
        // Bad slot string — fall through to nullopt
    }
    return std::nullopt;
}
} // namespace

AmsBackendQidi::AmsBackendQidi(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Populate system_info_ so get_system_info() returns a self-consistent
    // empty-but-initialised snapshot even before any status update arrives.
    system_info_.type = AmsType::QIDI_BOX;
    system_info_.type_name = "QIDI Box"; // i18n: do not translate - product name
    system_info_.total_slots = NUM_SLOTS;
    system_info_.supports_bypass = false;
    system_info_.supports_tool_mapping = true;
    system_info_.supports_endless_spool = false;
    system_info_.supports_purge = false;
    system_info_.tip_method = TipMethod::CUT;

    // Single unit with NUM_SLOTS empty slots, PARALLEL-less HUB topology.
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "QIDI Box";
    unit.display_name = "QIDI Box";
    unit.slot_count = NUM_SLOTS;
    unit.first_slot_global_index = 0;
    unit.connected = false; // flip once protocol is implemented
    unit.topology = PathTopology::HUB;

    for (int i = 0; i < NUM_SLOTS; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i;
        unit.slots.push_back(slot);
    }

    system_info_.units.push_back(std::move(unit));
    slot_rfid_.resize(NUM_SLOTS);

    spdlog::debug("{} Stub backend constructed ({} slots, no protocol implemented)",
                  backend_log_tag(), NUM_SLOTS);
}

AmsBackendQidi::~AmsBackendQidi() = default;

// --- Lifecycle hooks ---

void AmsBackendQidi::on_started() {
    spdlog::warn("{} {} not yet implemented — backend is a stub pending live hardware",
                 backend_log_tag(), __func__);
    // Intentionally no subscription work: we have nothing to subscribe to yet.
}

void AmsBackendQidi::handle_status_update(const nlohmann::json& notification) {
    if (!notification.is_object()) {
        return;
    }
    // Moonraker delivers save_variables changes as
    // `{"save_variables": {"variables": {...}}}`. Unwrap and feed the inner
    // variables payload to parse_save_variables.
    auto sv_it = notification.find("save_variables");
    if (sv_it != notification.end() && sv_it->is_object()) {
        auto vars_it = sv_it->find("variables");
        if (vars_it != sv_it->end() && vars_it->is_object()) {
            parse_save_variables(*vars_it);
        }
    }

    // Per-box drying state arrives as separate top-level objects:
    //   "heater_generic heater_box<N>" → {temperature, target, power}
    //   "aht20_f heater_box<N>"        → {temperature, humidity}
    // We surface the maximum temperature and maximum humidity across all
    // boxes onto AmsUnit::environment so the UI can show "drying" when
    // ANY box is active.
    apply_heater_status(notification);
}

void AmsBackendQidi::apply_heater_status(const nlohmann::json& notification) {
    constexpr std::string_view kHeaterPrefix = "heater_generic heater_box";
    constexpr std::string_view kAht20Prefix = "aht20_f heater_box";

    std::optional<float> max_temp;
    std::optional<float> max_humidity;

    for (auto it = notification.begin(); it != notification.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        const std::string& key = it.key();
        const bool is_heater = key.rfind(kHeaterPrefix, 0) == 0;
        const bool is_aht = key.rfind(kAht20Prefix, 0) == 0;
        if (!is_heater && !is_aht) {
            continue;
        }
        if (auto t_it = it->find("temperature");
            t_it != it->end() && t_it->is_number()) {
            const float v = t_it->get<float>();
            if (!max_temp || v > *max_temp) {
                max_temp = v;
            }
        }
        if (is_aht) {
            if (auto h_it = it->find("humidity");
                h_it != it->end() && h_it->is_number()) {
                const float v = h_it->get<float>();
                if (!max_humidity || v > *max_humidity) {
                    max_humidity = v;
                }
            }
        }
    }

    if (!max_temp && !max_humidity) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.units.empty()) {
        return;
    }
    auto& env = system_info_.units[0].environment;
    if (!env) {
        env = EnvironmentData{};
    }
    if (max_temp) {
        env->temperature_c = *max_temp;
    }
    if (max_humidity) {
        env->humidity_pct = *max_humidity;
        env->has_humidity = true;
    }
}

void AmsBackendQidi::parse_save_variables(const nlohmann::json& variables) {
    if (!variables.is_object() || system_info_.units.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    // enable_box: master gate set by box_extras. 1 = active, 0/missing =
    // installed-but-disabled. Treat as the unit's "connected" state.
    auto enable_it = variables.find("enable_box");
    if (enable_it != variables.end() && enable_it->is_number_integer()) {
        system_info_.units[0].connected = (enable_it->get<int>() != 0);
    }

    // box_count: number of physical boxes detected by box_detect.py via USB
    // enumeration. Each box has 4 slots; chainable up to 4 boxes / 16 slots.
    // Resize the unit's slot vector to match, preserving any existing data
    // for slots that remain valid.
    auto box_count_it = variables.find("box_count");
    if (box_count_it != variables.end() && box_count_it->is_number_integer()) {
        int box_count = box_count_it->get<int>();
        if (box_count >= 1 && box_count <= 4) {
            const int desired_slots = box_count * NUM_SLOTS;
            AmsUnit& unit = system_info_.units[0];
            if (static_cast<int>(unit.slots.size()) != desired_slots) {
                unit.slots.resize(static_cast<size_t>(desired_slots));
                slot_rfid_.resize(static_cast<size_t>(desired_slots));
                for (size_t i = 0; i < unit.slots.size(); ++i) {
                    unit.slots[i].slot_index = static_cast<int>(i);
                    unit.slots[i].global_index = static_cast<int>(i);
                    if (unit.slots[i].mapped_tool < 0) {
                        unit.slots[i].mapped_tool = static_cast<int>(i);
                    }
                }
                unit.slot_count = desired_slots;
                system_info_.total_slots = desired_slots;
            }
        }
    }

    AmsUnit& unit_ref = system_info_.units[0];

    // value_t<N> = "slot<M>" — tool N prints from slot M. Apply over the
    // default tool=slot mapping established when the unit was sized.
    const int slot_count = static_cast<int>(unit_ref.slots.size());
    for (size_t t = 0; t < unit_ref.slots.size(); ++t) {
        const std::string key = "value_t" + std::to_string(t);
        auto vt_it = variables.find(key);
        if (vt_it == variables.end() || !vt_it->is_string()) {
            continue;
        }
        if (auto idx = parse_slot_name(vt_it->get<std::string>(), slot_count)) {
            unit_ref.slots[*idx].mapped_tool = static_cast<int>(t);
        }
    }

    // Per-slot state from `slot<N>` save_variables. box_stepper.py state
    // machine values:
    //   0  = empty
    //   1  = available (parked in box)
    //   2  = loaded all the way to extruder
    //   3  = mid-transition (treat as available; action belongs on system_info_.action)
    //   -1 = slot load failed
    //   -2 = extruder load failed
    //   -3 = runout-during-print
    // Negative values all map to BLOCKED so the UI surfaces an error chip.
    for (size_t i = 0; i < unit_ref.slots.size(); ++i) {
        const std::string key = "slot" + std::to_string(i);
        auto slot_it = variables.find(key);
        if (slot_it == variables.end() || !slot_it->is_number_integer()) {
            continue;
        }
        const int state = slot_it->get<int>();
        SlotStatus mapped;
        switch (state) {
        case 0:
            mapped = SlotStatus::EMPTY;
            break;
        case 1:
        case 3:
            mapped = SlotStatus::AVAILABLE;
            break;
        case 2:
            mapped = SlotStatus::LOADED;
            break;
        default:
            // -1, -2, -3 — all error states
            mapped = (state < 0) ? SlotStatus::BLOCKED : SlotStatus::UNKNOWN;
            break;
        }
        unit_ref.slots[i].status = mapped;
    }

    // last_load_slot is box_extras.py's authoritative "which slot is in the
    // extruder right now" signal. Two outcomes:
    //   "slot<N>"  → promote slot N to LOADED (covers the case where the
    //                per-slot signal hasn't caught up, e.g. recovery paths)
    //   "slot-1"   → demote any slot still claiming LOADED to AVAILABLE
    //                (nothing is in the extruder anymore)
    auto load_it = variables.find("last_load_slot");
    if (load_it != variables.end() && load_it->is_string()) {
        const std::string val = load_it->get<std::string>();
        if (val == "slot-1") {
            for (auto& slot : unit_ref.slots) {
                if (slot.status == SlotStatus::LOADED) {
                    slot.status = SlotStatus::AVAILABLE;
                }
            }
        } else if (auto idx = parse_slot_name(val, slot_count)) {
            unit_ref.slots[*idx].status = SlotStatus::LOADED;
        }
    }

    // Per-slot RFID indices written by box_extras.py:
    //   filament_slot<N> = material index 1-99 (officiall_filas_list.cfg)
    //   color_slot<N>    = palette index 1-24
    //   vendor_slot<N>   = vendor index (always 1 observed so far)
    // Captured raw into slot_rfid_; resolved to material/color/brand by a
    // separate cfg-file lookup (not yet implemented).
    if (slot_rfid_.size() < unit_ref.slots.size()) {
        slot_rfid_.resize(unit_ref.slots.size());
    }
    for (size_t i = 0; i < unit_ref.slots.size(); ++i) {
        const std::string suffix = std::to_string(i);
        if (auto it = variables.find("filament_slot" + suffix);
            it != variables.end() && it->is_number_integer()) {
            slot_rfid_[i].filament_id = it->get<int>();
        }
        if (auto it = variables.find("color_slot" + suffix);
            it != variables.end() && it->is_number_integer()) {
            slot_rfid_[i].color_id = it->get<int>();
        }
        if (auto it = variables.find("vendor_slot" + suffix);
            it != variables.end() && it->is_number_integer()) {
            slot_rfid_[i].vendor_id = it->get<int>();
        }
    }
}

// --- State queries ---

AmsSystemInfo AmsBackendQidi::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendQidi::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_SLOTS) {
        return SlotInfo{};
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    return slot ? *slot : SlotInfo{};
}

bool AmsBackendQidi::is_bypass_active() const {
    return false;
}

// --- Path visualisation ---

PathSegment AmsBackendQidi::get_filament_segment() const {
    return PathSegment::NONE;
}

PathSegment AmsBackendQidi::get_slot_filament_segment(int /*slot_index*/) const {
    return PathSegment::NONE;
}

PathSegment AmsBackendQidi::infer_error_segment() const {
    return PathSegment::NONE;
}

// --- Filament operations ---

AmsError AmsBackendQidi::load_filament(int /*slot_index*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box load_filament");
}

AmsError AmsBackendQidi::unload_filament(int /*slot_index*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box unload_filament");
}

AmsError AmsBackendQidi::select_slot(int /*slot_index*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box select_slot");
}

AmsError AmsBackendQidi::change_tool(int /*tool_number*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box change_tool");
}

// --- Recovery ---

AmsError AmsBackendQidi::recover() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box recover");
}

AmsError AmsBackendQidi::reset() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box reset");
}

AmsError AmsBackendQidi::cancel() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box cancel");
}

// --- Configuration ---

AmsError AmsBackendQidi::set_slot_info(int /*slot_index*/, const SlotInfo& /*info*/,
                                       bool /*persist*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box set_slot_info");
}

AmsError AmsBackendQidi::set_tool_mapping(int /*tool_number*/, int /*slot_index*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box set_tool_mapping");
}

void AmsBackendQidi::clear_slot_override(int /*slot_index*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
}

// --- Bypass ---

AmsError AmsBackendQidi::enable_bypass() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box enable_bypass");
}

AmsError AmsBackendQidi::disable_bypass() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box disable_bypass");
}

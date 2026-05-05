// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_phase_tracker.h"

#include "app_globals.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"
#include "ui_update_queue.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>

namespace helix {

namespace {

constexpr const char* kHandlerName = "print_phase_tracker";

/// Default mesh probe count when "Mesh X,Y:" hasn't been seen yet.
/// 25×25 matches K2 stock; over-/undershoot just clamps the progress bar
/// until the first "Mesh X,Y:" line lands.
constexpr int kDefaultMeshProbes = 625;

/// Localized label for a phase. Caller passes the result to
/// `lv_subject_copy_string` — translation packs are loaded once at startup
/// so this is just a hash-map lookup, no allocation.
const char* phase_label(PrintPhase phase) {
    switch (phase) {
        case PrintPhase::IDLE:          return lv_tr("Idle");
        case PrintPhase::PREPARING:     return lv_tr("Preparing");
        case PrintPhase::BED_MESH:      return lv_tr("Bed Mesh");
        case PrintPhase::HEATING:       return lv_tr("Heating");
        case PrintPhase::FILAMENT_LOAD: return lv_tr("Loading Filament");
        case PrintPhase::PURGE:         return lv_tr("Purging");
        case PrintPhase::PRINTING:      return lv_tr("Printing");
        case PrintPhase::PAUSED:        return lv_tr("Paused");
        case PrintPhase::COMPLETE:      return lv_tr("Complete");
        case PrintPhase::ERROR:         return lv_tr("Error");
        case PrintPhase::CANCELLED:     return lv_tr("Cancelled");
    }
    return "";
}

/// Substring match — tolerant of leading "// " from notify_gcode_response.
inline bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

const char* print_phase_to_string(PrintPhase phase) {
    switch (phase) {
        case PrintPhase::IDLE:          return "IDLE";
        case PrintPhase::PREPARING:     return "PREPARING";
        case PrintPhase::BED_MESH:      return "BED_MESH";
        case PrintPhase::HEATING:       return "HEATING";
        case PrintPhase::FILAMENT_LOAD: return "FILAMENT_LOAD";
        case PrintPhase::PURGE:         return "PURGE";
        case PrintPhase::PRINTING:      return "PRINTING";
        case PrintPhase::PAUSED:        return "PAUSED";
        case PrintPhase::COMPLETE:      return "COMPLETE";
        case PrintPhase::ERROR:         return "ERROR";
        case PrintPhase::CANCELLED:     return "CANCELLED";
    }
    return "?";
}

PrintPhaseTracker& PrintPhaseTracker::instance() {
    static PrintPhaseTracker s_instance;
    return s_instance;
}

void PrintPhaseTracker::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrintPhaseTracker] Subjects already initialized, skipping");
        return;
    }

    INIT_SUBJECT_INT(print_phase, static_cast<int>(PrintPhase::IDLE), subjects_, register_xml);
    INIT_SUBJECT_STRING(print_phase_label, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(print_phase_detail, "", subjects_, register_xml);
    INIT_SUBJECT_INT(print_phase_progress, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(print_phase_eta_seconds, -1, subjects_, register_xml);

    // Seed the label so XML bindings have a non-empty starting value.
    lv_subject_copy_string(&print_phase_label_, phase_label(PrintPhase::IDLE));

    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "PrintPhaseTracker", []() { PrintPhaseTracker::instance().deinit_subjects(); });

    spdlog::trace("[PrintPhaseTracker] Subjects initialized");
}

void PrintPhaseTracker::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Reset observers BEFORE subjects so weak_ptr expires first (#705 / [L085]).
    print_state_observer_.reset();
    print_duration_observer_.reset();
    observers_attached_ = false;

    subjects_.deinit_all();
    subjects_initialized_ = false;
    current_phase_ = PrintPhase::IDLE;
    mesh_probes_seen_ = 0;
    mesh_probes_total_ = 0;
    mesh_seconds_per_probe_ = 0.0f;
    mesh_active_ = false;
    purge_percent_ = -1;

    spdlog::trace("[PrintPhaseTracker] Subjects deinitialized");
}

void PrintPhaseTracker::attach_observers() {
    if (observers_attached_ || !subjects_initialized_) {
        return;
    }

    auto& ps = get_printer_state();
    auto* state_subj = ps.get_print_state_enum_subject();
    auto* dur_subj = ps.get_print_duration_subject();
    if (!state_subj || !dur_subj) {
        spdlog::warn("[PrintPhaseTracker] PrinterState subjects not ready, deferring");
        return;
    }

    print_state_observer_ = helix::ui::observe_int_sync(
        state_subj, this,
        [](PrintPhaseTracker* self, int value) { self->on_print_job_state(value); });

    print_duration_observer_ = helix::ui::observe_int_sync(
        dur_subj, this,
        [](PrintPhaseTracker* self, int value) { self->on_print_duration(value); });

    observers_attached_ = true;
    spdlog::debug("[PrintPhaseTracker] Observers attached to PrinterState");
}

void PrintPhaseTracker::attach_to_client(MoonrakerClient* client) {
    if (!client) {
        return;
    }
    // Capture nothing dangerous — singleton lives until process exit, so
    // raw pointer to instance() is safe across the WebSocket thread's lifetime.
    // [L072]: don't capture `this` pointers from short-lived holders; the
    // tracker is process-lifetime so it doesn't apply here, but the rule is
    // the reason we use instance() rather than a member shared_ptr.
    PrintPhaseTracker* self = this;
    client->register_method_callback(
        "notify_gcode_response", kHandlerName, [self](const nlohmann::json& msg) {
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
                return;
            }
            auto handle = [self](const std::string& line) {
                if (!line.empty()) {
                    self->process_gcode_line(line);
                }
            };
            const auto& params = msg["params"];
            if (params[0].is_array()) {
                for (const auto& line : params[0]) {
                    if (line.is_string()) handle(line.get<std::string>());
                }
            } else if (params[0].is_string()) {
                for (const auto& line : params) {
                    if (line.is_string()) handle(line.get<std::string>());
                }
            }
        });
    spdlog::debug("[PrintPhaseTracker] Registered notify_gcode_response listener");
}

void PrintPhaseTracker::detach_from_client(MoonrakerClient* client) {
    if (!client) {
        return;
    }
    client->unregister_method_callback("notify_gcode_response", kHandlerName);
}

void PrintPhaseTracker::reset() {
    helix::ui::queue_update([this]() {
        current_phase_ = PrintPhase::IDLE;
        mesh_probes_seen_ = 0;
        mesh_probes_total_ = 0;
        mesh_seconds_per_probe_ = 0.0f;
        mesh_active_ = false;
        purge_percent_ = -1;
        if (subjects_initialized_) {
            lv_subject_set_int(&print_phase_, static_cast<int>(PrintPhase::IDLE));
            lv_subject_copy_string(&print_phase_label_, phase_label(PrintPhase::IDLE));
            lv_subject_copy_string(&print_phase_detail_, "");
            lv_subject_set_int(&print_phase_progress_, -1);
            lv_subject_set_int(&print_phase_eta_seconds_, -1);
        }
    });
}

void PrintPhaseTracker::set_phase(PrintPhase phase) {
    if (!subjects_initialized_) return;
    if (phase == current_phase_) return;

    spdlog::info("[PrintPhaseTracker] {} -> {}",
                 print_phase_to_string(current_phase_), print_phase_to_string(phase));
    current_phase_ = phase;
    lv_subject_set_int(&print_phase_, static_cast<int>(phase));
    lv_subject_copy_string(&print_phase_label_, phase_label(phase));

    // Most phase transitions clear the previous detail/progress; phase-specific
    // updates (probe count, flush %) re-fill them.
    lv_subject_copy_string(&print_phase_detail_, "");
    lv_subject_set_int(&print_phase_progress_, -1);
    lv_subject_set_int(&print_phase_eta_seconds_, -1);
}

void PrintPhaseTracker::on_print_job_state(int state_enum) {
    if (!subjects_initialized_) return;
    auto state = static_cast<PrintJobState>(state_enum);
    switch (state) {
        case PrintJobState::STANDBY:
            // Don't immediately drop terminal states back to IDLE — let the UI
            // show COMPLETE/CANCELLED/ERROR briefly. The user will start a new
            // print or navigate away; we'll reset on the next standby→printing
            // transition.
            if (current_phase_ != PrintPhase::COMPLETE &&
                current_phase_ != PrintPhase::CANCELLED &&
                current_phase_ != PrintPhase::ERROR) {
                set_phase(PrintPhase::IDLE);
            }
            break;
        case PrintJobState::PRINTING:
            // print_duration drives the PREPARING vs PRINTING distinction —
            // see on_print_duration. Just ensure we're at least PREPARING
            // when entering printing from a non-active state.
            if (current_phase_ == PrintPhase::IDLE ||
                current_phase_ == PrintPhase::COMPLETE ||
                current_phase_ == PrintPhase::CANCELLED ||
                current_phase_ == PrintPhase::ERROR) {
                mesh_probes_seen_ = 0;
                mesh_probes_total_ = 0;
                mesh_seconds_per_probe_ = 0.0f;
                mesh_active_ = false;
                purge_percent_ = -1;
                set_phase(PrintPhase::PREPARING);
            }
            break;
        case PrintJobState::PAUSED:
            set_phase(PrintPhase::PAUSED);
            break;
        case PrintJobState::COMPLETE:
            set_phase(PrintPhase::COMPLETE);
            break;
        case PrintJobState::CANCELLED:
            set_phase(PrintPhase::CANCELLED);
            break;
        case PrintJobState::ERROR:
            set_phase(PrintPhase::ERROR);
            break;
    }
}

void PrintPhaseTracker::on_print_duration(int duration_seconds) {
    if (!subjects_initialized_) return;
    // print_duration > 0 means real print gcode is flowing (Klipper increments
    // it from M75 / first move after START_PRINT.prepare flips). This is the
    // canonical signal that we've left pre-print phases.
    if (duration_seconds > 0 && current_phase_ != PrintPhase::PRINTING &&
        current_phase_ != PrintPhase::PAUSED && current_phase_ != PrintPhase::COMPLETE &&
        current_phase_ != PrintPhase::CANCELLED && current_phase_ != PrintPhase::ERROR) {
        set_phase(PrintPhase::PRINTING);
    }
}

// ============================================================================
// gcode_response pipeline
// ============================================================================
//
// Background-thread entry. We hop to the UI thread immediately so every member
// touched by the matchers (current_phase_, mesh_probes_seen_, …) is read and
// written from a single thread. No mutex required — the UpdateQueue serializes.
//
// Tags currently parsed are K2/CFS-specific. When a new firmware family lands
// (K1 motion sensors, Bambu, RatOS), add a sibling matcher and dispatch to it
// from `dispatch_line_ui` rather than growing this one.
void PrintPhaseTracker::process_gcode_line(const std::string& line_in) {
    helix::ui::queue_update([this, line = line_in]() { dispatch_line_ui(line); });
}

void PrintPhaseTracker::dispatch_line_ui(const std::string& line) {
    if (!subjects_initialized_) return;

    // Order matters only weakly — every matcher returns true on consume,
    // and the matchers don't overlap. The mesh-size scrape runs even when
    // PROBE_STEP_INFO doesn't match because it can fire on a non-mesh line.
    if (try_match_mesh_size(line)) {
        // mesh_size is informational, not a phase trigger — keep scanning
        // in case the same line also matches another tag (it won't today,
        // but cheap to be tolerant).
    }
    if (try_match_probe_step(line))     return;
    if (try_match_mesh_begin(line))     return;
    if (try_match_g29_time(line))       return;
    if (try_match_filament_load(line))  return;
    if (try_match_purge_percent(line))  return;
    if (try_match_heating_hint(line))   return;
}

// ----- [K2/CFS] mesh size ---------------------------------------------------
// "Mesh X,Y: 25,25 / Search Height: 5 / Mesh Average: 0.11"
bool PrintPhaseTracker::try_match_mesh_size(const std::string& line) {
    auto p = line.find("Mesh X,Y:");
    if (p == std::string::npos) return false;

    int mx = 0, my = 0;
    if (std::sscanf(line.c_str() + p, "Mesh X,Y: %d,%d", &mx, &my) == 2 && mx > 0 && my > 0) {
        mesh_probes_total_ = mx * my;
    }
    return true;
}

// ----- [K2/CFS] per-probe progress ------------------------------------------
// "// [PROBE_STEP_INFO]step_bst_indx=0 step_bst_time=12 …"
bool PrintPhaseTracker::try_match_probe_step(const std::string& line) {
    if (!contains(line, "[PROBE_STEP_INFO]")) return false;

    if (current_phase_ != PrintPhase::BED_MESH) {
        set_phase(PrintPhase::BED_MESH);
        mesh_active_ = true;
    }
    mesh_probes_seen_ += 1;
    int total = mesh_probes_total_ > 0 ? mesh_probes_total_ : kDefaultMeshProbes;
    int progress = total > 0 ? std::min(1000, (mesh_probes_seen_ * 1000) / total) : -1;
    lv_subject_set_int(&print_phase_progress_, progress);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d / %d", mesh_probes_seen_, total);
    lv_subject_copy_string(&print_phase_detail_, buf);

    if (mesh_seconds_per_probe_ > 0.0f && total > mesh_probes_seen_) {
        int eta = static_cast<int>((total - mesh_probes_seen_) * mesh_seconds_per_probe_);
        lv_subject_set_int(&print_phase_eta_seconds_, eta);
    }
    return true;
}

// ----- [K2/CFS] mesh begin bookend ------------------------------------------
// "// [DEBUG]multi_probe_begin" — fires once before the probe stream.
bool PrintPhaseTracker::try_match_mesh_begin(const std::string& line) {
    if (!contains(line, "[DEBUG]multi_probe_begin")) return false;

    if (current_phase_ != PrintPhase::BED_MESH) {
        set_phase(PrintPhase::BED_MESH);
        mesh_active_ = true;
        mesh_probes_seen_ = 0;
    }
    return true;
}

// ----- [K2/CFS] mesh end + timing -------------------------------------------
// "// [G29_TIME]Execution time: 266.435 seconds, Time spent at each point: 3.2"
//
// Snap progress to 100% on this line. The K2's "Mesh X,Y: 25,25" announces
// the *interpolated* grid (625 cells), not the probed grid (81 on a K2 Plus).
// The actual probe count is reflected in mesh_probes_seen_ at this point, so
// adopt it as the canonical total — guarantees the bar reads exactly 100%
// at mesh-done regardless of the printer's mesh density.
bool PrintPhaseTracker::try_match_g29_time(const std::string& line) {
    if (!contains(line, "[G29_TIME]")) return false;

    if (auto p = line.find("at each point:"); p != std::string::npos) {
        float per_pt = 0.0f;
        if (std::sscanf(line.c_str() + p, "at each point: %f", &per_pt) == 1 && per_pt > 0.0f) {
            mesh_seconds_per_probe_ = per_pt;
        }
    }

    // Snap to canonical totals so any subscribed UI pins to 100%.
    if (mesh_probes_seen_ > 0) {
        mesh_probes_total_ = mesh_probes_seen_;
        if (current_phase_ == PrintPhase::BED_MESH) {
            lv_subject_set_int(&print_phase_progress_, 1000);
            lv_subject_set_int(&print_phase_eta_seconds_, 0);
        }
    }
    // Don't force a phase here — let the next observed signal (heating, box,
    // print_duration) drive the transition.
    mesh_active_ = false;
    return true;
}

// ----- [K2/CFS + AMS] filament load -----------------------------------------
// "// [box] cut sensor detected", "// [box] cut to return …",
// "BOX_LOAD_MATERIAL", "LOAD_MATERIAL".
bool PrintPhaseTracker::try_match_filament_load(const std::string& line) {
    bool match = contains(line, "[box] cut sensor detected") ||
                 contains(line, "[box] cut to return") ||
                 contains(line, "BOX_LOAD_MATERIAL") ||
                 contains(line, "LOAD_MATERIAL");
    if (!match) return false;

    if (current_phase_ != PrintPhase::FILAMENT_LOAD &&
        current_phase_ != PrintPhase::PURGE &&
        current_phase_ != PrintPhase::PRINTING) {
        set_phase(PrintPhase::FILAMENT_LOAD);
    }
    return true;
}

// ----- [K2/CFS] purge / flush percent ---------------------------------------
// "// num: 0, velocity: 575.000000, percent 1.000000"
//
// Real-world K2 format: "percent" (no colon!) followed by a float in 0..1
// (i.e. fraction, not %). Single-color prints emit one line per flush
// (`num: 0` = first flush). Tolerate both colon and non-colon form, and
// both float-fraction and integer-percent representations, so we don't
// regress if Creality fixes the format on a future firmware.
bool PrintPhaseTracker::try_match_purge_percent(const std::string& line) {
    auto p = line.find("percent");
    if (p == std::string::npos) return false;
    if (!contains(line, "num:") && !contains(line, "velocity:")) return false;

    // Skip "percent" + optional ":" + whitespace, then parse a number.
    const char* cursor = line.c_str() + p + 7; // past "percent"
    while (*cursor == ':' || *cursor == ' ') cursor++;

    float frac = -1.0f;
    if (std::sscanf(cursor, "%f", &frac) != 1 || frac < 0.0f) return false;

    // Accept both 0..1 (K2 firmware fraction form) and 0..100 (legacy/integer).
    int per_mille = (frac <= 1.5f) ? static_cast<int>(frac * 1000.0f + 0.5f)
                                   : static_cast<int>(frac * 10.0f + 0.5f);
    per_mille = std::max(0, std::min(1000, per_mille));

    if (current_phase_ != PrintPhase::PURGE && current_phase_ != PrintPhase::PRINTING) {
        set_phase(PrintPhase::PURGE);
    }
    purge_percent_ = per_mille / 10;
    lv_subject_set_int(&print_phase_progress_, per_mille);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d%%", per_mille / 10);
    lv_subject_copy_string(&print_phase_detail_, buf);
    return true;
}

// ----- [K2/CFS] heating hint ------------------------------------------------
// "// [WHY_DEBUG]…target_temp:220.00 G28_temp:139.94"
// On K2 the line fires both during G28 homing (target ≈ G28_temp) and during
// post-mesh print-temp ramp (target = first-layer temp). Either way the
// printer is past PREPARING/BED_MESH and now actively heating, so we advance
// from any earlier pre-print phase. Phase 3 will split HEATING_BED vs
// HEATING_NOZZLE off this same line.
bool PrintPhaseTracker::try_match_heating_hint(const std::string& line) {
    if (!contains(line, "[WHY_DEBUG]") || !contains(line, "target_temp:")) return false;

    if (current_phase_ == PrintPhase::PREPARING ||
        current_phase_ == PrintPhase::BED_MESH) {
        set_phase(PrintPhase::HEATING);
    }
    return true;
}

} // namespace helix

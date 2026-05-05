// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/print_phase_tracker_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "print_phase_tracker.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void flush() {
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

void feed(PrintPhaseTracker& t, const std::string& line) {
    PrintPhaseTrackerTestAccess::process_line(t, line);
    flush();
}

} // namespace

TEST_CASE("PrintPhaseTracker: init creates subjects with sane defaults",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    REQUIRE(t.get_phase_subject() != nullptr);
    REQUIRE(t.get_phase_label_subject() != nullptr);
    REQUIRE(t.get_phase_detail_subject() != nullptr);
    REQUIRE(t.get_phase_progress_subject() != nullptr);
    REQUIRE(t.get_phase_eta_subject() != nullptr);

    REQUIRE(lv_subject_get_int(t.get_phase_subject()) == static_cast<int>(PrintPhase::IDLE));
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == -1);
    REQUIRE(lv_subject_get_int(t.get_phase_eta_subject()) == -1);

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: probe step lines drive BED_MESH and progress",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    // K2 first announces the mesh size, then begins probing.
    feed(t, "Mesh X,Y: 25,25 / Search Height: 5 / Mesh Average: 0.11");
    REQUIRE(PrintPhaseTrackerTestAccess::mesh_probes_total(t) == 625);

    feed(t, "// [DEBUG]multi_probe_begin");
    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::BED_MESH);

    // Five probes in.
    for (int i = 0; i < 5; ++i) {
        feed(t,
             "// [PROBE_STEP_INFO]step_bst_indx=" + std::to_string(i) +
                 " step_bst_time=12 tri_pose=0 bst_pose=0 bst_zoft=0.1 POS=[1.0,2.0,3.0]");
    }

    REQUIRE(PrintPhaseTrackerTestAccess::mesh_probes_seen(t) == 5);
    REQUIRE(lv_subject_get_int(t.get_phase_subject()) == static_cast<int>(PrintPhase::BED_MESH));
    // 5 / 625 → 8 (out of 1000)
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == 8);

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: G29_TIME captures per-probe seconds",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    feed(t, "// [G29_TIME]Execution time: 266.435 seconds, Time spent at each point: 3.2");
    REQUIRE(PrintPhaseTrackerTestAccess::mesh_seconds_per_probe(t) == Catch::Approx(3.2f));

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: box cut event enters FILAMENT_LOAD",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    feed(t, "// [box] cut sensor detected");
    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::FILAMENT_LOAD);

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: flush percent line enters PURGE and updates progress",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    // Legacy/integer form: "percent:" with int 0..100
    feed(t, "// num: 0, velocity: 23.0, percent: 50");
    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::PURGE);
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == 500);

    feed(t, "// num: 1, velocity: 23.0, percent: 100");
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == 1000);

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: K2 fraction form '/percent <0..1>' is recognized",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    // Real K2 firmware emits: "// num: 0, velocity: 575.000000, percent 1.000000"
    // No colon, value is a 0..1 fraction (not 0..100).
    feed(t, "// num: 0, velocity: 575.000000, percent 0.250000");
    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::PURGE);
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == 250);

    feed(t, "// num: 0, velocity: 575.000000, percent 1.000000");
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == 1000);

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: irrelevant lines do not change phase",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    feed(t, "ok");
    feed(t, "B:25.0 /0.0 T0:24.5 /0.0");
    feed(t, "// some random debug line");

    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::IDLE);
    REQUIRE(lv_subject_get_int(t.get_phase_subject()) == static_cast<int>(PrintPhase::IDLE));

    t.deinit_subjects();
}

TEST_CASE("PrintPhaseTracker: reset clears transient state",
          "[print_phase_tracker]") {
    lv_init_safe();
    auto& t = PrintPhaseTracker::instance();
    t.deinit_subjects();
    t.init_subjects(false);

    feed(t, "Mesh X,Y: 25,25");
    feed(t, "// [PROBE_STEP_INFO]step_bst_indx=0");
    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::BED_MESH);

    t.reset();
    flush();

    REQUIRE(PrintPhaseTrackerTestAccess::current_phase(t) == PrintPhase::IDLE);
    REQUIRE(PrintPhaseTrackerTestAccess::mesh_probes_seen(t) == 0);
    REQUIRE(lv_subject_get_int(t.get_phase_progress_subject()) == -1);

    t.deinit_subjects();
}

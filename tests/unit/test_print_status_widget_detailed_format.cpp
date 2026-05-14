// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "app_globals.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// RAII helper that creates the DetailedFormatter singleton without a real attach()
struct FormatterScope {
    FormatterScope() { PrintStatusWidget::ensure_formatter_for_test(); }
    ~FormatterScope() { PrintStatusWidget::release_formatter_for_test(); }
};

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter writes progress, layer, time",
                 "[print_status][formatter]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_progress_subject(), 47);
    lv_subject_set_int(ps.get_print_layer_current_subject(), 42);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 213);
    lv_subject_set_int(ps.get_print_elapsed_subject(), 42 * 60);               // 0h 42m
    lv_subject_set_int(ps.get_print_time_left_subject(), 2 * 3600 + 14 * 60); // 2h 14m

    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_progress_pct"))) == "47%");
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_layer_text"))) == "Layer 42 / 213");
    // elapsed=42m, total=42m+2h14m=2h56m
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_time_text"))) == "0h 42m / 2h 56m");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter seeds initial values on construction",
                 "[print_status][formatter]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    // Set subjects BEFORE creating formatter — seed calls in constructor pick them up
    lv_subject_set_int(ps.get_print_progress_subject(), 75);
    lv_subject_set_int(ps.get_print_layer_current_subject(), 100);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 200);

    FormatterScope fs;

    // No drain needed — seed calls are synchronous in the constructor
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_progress_pct"))) == "75%");
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_layer_text"))) == "Layer 100 / 200");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter layer text omits total when zero",
                 "[print_status][formatter]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_layer_current_subject(), 7);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 0);

    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_layer_text"))) == "Layer 7");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter filament text empty when zero",
                 "[print_status][formatter]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_filament_used_subject(), 0);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_filament_text"))) == "");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter filament text formatted in meters",
                 "[print_status][formatter]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_filament_used_subject(), 2500); // 2.5m
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_filament_text"))) == "2.5m");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter writes temps (centidegrees rounding)",
                 "[print_status][formatter][temps]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;
    lv_subject_set_int(ps.get_active_extruder_temp_subject(), 21570);    // 215.70 → 216
    lv_subject_set_int(ps.get_active_extruder_target_subject(), 22000);  // 220
    lv_subject_set_int(ps.get_bed_temp_subject(), 6005);                  // 60.05 → 60
    lv_subject_set_int(ps.get_bed_target_subject(), 6000);                // 60
    lv_subject_set_int(ps.get_chamber_temp_subject(), 3800);              // 38, no target
    lv_subject_set_int(ps.get_chamber_target_subject(), 0);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_nozzle_text"))) == "216 / 220°C");
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_bed_text"))) == "60 / 60°C");
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_chamber_text"))) == "38°C");
}

TEST_CASE_METHOD(HelixTestFixture, "Chamber with target shows pair",
                 "[print_status][formatter][temps]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;
    lv_subject_set_int(ps.get_chamber_temp_subject(), 3500);     // 35
    lv_subject_set_int(ps.get_chamber_target_subject(), 4500);   // 45
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "print_status_chamber_text"))) == "35 / 45°C");
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "performance_state.h"
#include "ui_update_queue.h"

#include <lvgl.h>

using helix::perf::PerformanceState;
using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;

namespace {

class PerfStateFixture : public HelixTestFixture {
  public:
    PerfStateFixture() {
        PerformanceState::instance().init_subjects();
    }
    ~PerfStateFixture() override {
        PerformanceState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState registers static subjects",
                 "[performance]") {
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_pct") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_c10") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_free_mb") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_pct_used") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_throttle_state") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_throttle_text") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_names") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_about_summary") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_available") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_history_tick") != nullptr);

    // Defaults: nothing present, summary em-dash, available=0
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_available")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 0);
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState applies sample and updates subjects",
                 "[performance]") {
    using helix::perf::PerfSample;

    PerfSample s;
    s.host_cpu_pct = 37.4f;
    s.host_cpu_temp_c = 61.4f;
    s.host_mem_free_mb = 812;
    s.host_mem_pct_used = 41.0f;
    s.host_throttle_bits = 0;

    PerformanceState::instance().push_sample_for_testing(s);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct")) == 37);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_c10")) == 614);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_free_mb")) == 812);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_pct_used")) == 41);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_present")) == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present")) == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_available")) == 1);
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState ring buffer fills then rolls",
                 "[performance]") {
    using helix::perf::PerfSample;
    auto& ps = PerformanceState::instance();

    for (int i = 0; i < 75; ++i) {
        PerfSample s;
        s.host_cpu_pct = static_cast<float>(i);
        ps.push_sample_for_testing(s);
    }
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto hist = ps.read_history("host_cpu_pct");
    REQUIRE(hist.size() == 60);
    REQUIRE(hist.front() == Catch::Approx(15.0f));   // oldest kept = i=15
    REQUIRE(hist.back()  == Catch::Approx(74.0f));   // newest = i=74
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState absent fields drop _present flags",
                 "[performance]") {
    using helix::perf::PerfSample;
    auto& ps = PerformanceState::instance();

    // First push a fully-populated sample so flags go to 1
    {
        PerfSample s;
        s.host_cpu_pct = 10.0f;
        s.host_cpu_temp_c = 30.0f;
        s.host_mem_free_mb = 500;
        s.host_mem_pct_used = 50.0f;
        ps.push_sample_for_testing(s);
    }
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_present")) == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present")) == 1);

    // Then push an all-absent sample — flags must clear back to 0
    PerfSample empty;
    ps.push_sample_for_testing(empty);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_present")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present")) == 0);
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState about summary reflects host + first MCU",
                 "[performance]") {
    using helix::perf::PerfSample;
    using helix::perf::McuStat;

    PerfSample s;
    s.host_cpu_pct = 37.0f;
    McuStat m; m.name = "mcu"; m.load = 0.14f;
    s.mcus.push_back(m);

    PerformanceState::instance().push_sample_for_testing(s);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    const char* summary =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "perf_about_summary"));
    REQUIRE(std::string(summary) == "37% CPU \xc2\xb7 14% MCU");
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState about summary drops MCU when none",
                 "[performance]") {
    using helix::perf::PerfSample;
    PerfSample s;
    s.host_cpu_pct = 37.0f;

    PerformanceState::instance().push_sample_for_testing(s);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    const char* summary =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "perf_about_summary"));
    REQUIRE(std::string(summary) == "37% CPU");
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState creates per-MCU subjects on first sample",
                 "[performance]") {
    using helix::perf::PerfSample;
    using helix::perf::McuStat;

    PerfSample s;
    s.host_cpu_pct = 10.0f;
    McuStat a; a.name = "mcu";    a.load = 0.10f; a.retransmits = 0;
    McuStat b; b.name = "mcu sb"; b.load = 0.22f; b.retransmits = 14;
    s.mcus = {a, b};

    PerformanceState::instance().push_sample_for_testing(s);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_mcu_load_pct") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_mcu_sb_load_pct") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_mcu_sb_retrans") != nullptr);

    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_mcu_mcu_load_pct")) == 10);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_mcu_mcu_sb_retrans")) == 14);

    const char* names = lv_subject_get_string(lv_xml_get_subject(nullptr, "perf_mcu_names"));
    REQUIRE(std::string(names) == "mcu,mcu sb");
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState updates existing MCU subjects without recreating",
                 "[performance]") {
    using helix::perf::PerfSample;
    using helix::perf::McuStat;
    auto& ps = PerformanceState::instance();

    {
        PerfSample s; McuStat a; a.name = "mcu"; a.load = 0.10f; s.mcus = {a};
        ps.push_sample_for_testing(s);
    }
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    auto* first = lv_xml_get_subject(nullptr, "perf_mcu_mcu_load_pct");
    REQUIRE(first != nullptr);
    {
        PerfSample s; McuStat a; a.name = "mcu"; a.load = 0.30f; s.mcus = {a};
        ps.push_sample_for_testing(s);
    }
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_mcu_load_pct") == first);
    REQUIRE(lv_subject_get_int(first) == 30);
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState formats CPU and Mem text rows",
                 "[performance]") {
    using helix::perf::PerfSample;
    PerfSample s;
    s.host_cpu_pct = 37.0f;
    s.host_cpu_temp_c = 61.4f;
    s.host_mem_free_mb = 812;
    s.host_mem_pct_used = 41.0f;
    PerformanceState::instance().push_sample_for_testing(s);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    const char* cpu_text = lv_subject_get_string(
        lv_xml_get_subject(nullptr, "perf_host_cpu_pct_text"));
    const char* mem_text = lv_subject_get_string(
        lv_xml_get_subject(nullptr, "perf_host_mem_free_text"));
    REQUIRE(std::string(cpu_text) == "37% \xc2\xb7 61.4\xc2\xb0""C");
    REQUIRE(std::string(mem_text) == "812 MB free");
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState formats per-MCU text subject",
                 "[performance]") {
    using helix::perf::PerfSample;
    using helix::perf::McuStat;
    PerfSample s;
    McuStat a; a.name = "mcu sb"; a.load = 0.22f; a.retransmits = 14;
    s.mcus = {a};
    PerformanceState::instance().push_sample_for_testing(s);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    const char* text = lv_subject_get_string(
        lv_xml_get_subject(nullptr, "perf_mcu_mcu_sb_text"));
    REQUIRE(std::string(text) == "22% \xc2\xb7 14 retx");
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState formats per-MCU text with no retransmits",
                 "[performance]") {
    using helix::perf::PerfSample;
    using helix::perf::McuStat;
    PerfSample s;
    McuStat a; a.name = "mcu"; a.load = 0.14f; a.retransmits = 0;
    s.mcus = {a};
    PerformanceState::instance().push_sample_for_testing(s);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    const char* text = lv_subject_get_string(
        lv_xml_get_subject(nullptr, "perf_mcu_mcu_text"));
    REQUIRE(std::string(text) == "14%");
}

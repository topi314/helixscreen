// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "input_settings_manager.h"
#include "runtime_config.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// InputSettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager default values after init",
                 "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("scroll_throw defaults to 25") {
        REQUIRE(InputSettingsManager::instance().get_scroll_throw() == 25);
    }

    SECTION("scroll_limit defaults to 10") {
        REQUIRE(InputSettingsManager::instance().get_scroll_limit() == 10);
    }

    SECTION("restart_pending defaults to false") {
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == false);
    }

    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager scroll_throw set/get", "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("set/get round trip") {
        InputSettingsManager::instance().set_scroll_throw(30);
        REQUIRE(InputSettingsManager::instance().get_scroll_throw() == 30);

        InputSettingsManager::instance().set_scroll_throw(5);
        REQUIRE(InputSettingsManager::instance().get_scroll_throw() == 5);

        InputSettingsManager::instance().set_scroll_throw(50);
        REQUIRE(InputSettingsManager::instance().get_scroll_throw() == 50);
    }

    SECTION("clamping below minimum") {
        InputSettingsManager::instance().set_scroll_throw(1);
        REQUIRE(InputSettingsManager::instance().get_scroll_throw() == 5);
    }

    SECTION("clamping above maximum") {
        InputSettingsManager::instance().set_scroll_throw(100);
        REQUIRE(InputSettingsManager::instance().get_scroll_throw() == 50);
    }

    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager scroll_limit set/get", "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("set/get round trip") {
        InputSettingsManager::instance().set_scroll_limit(15);
        REQUIRE(InputSettingsManager::instance().get_scroll_limit() == 15);

        InputSettingsManager::instance().set_scroll_limit(1);
        REQUIRE(InputSettingsManager::instance().get_scroll_limit() == 1);

        InputSettingsManager::instance().set_scroll_limit(20);
        REQUIRE(InputSettingsManager::instance().get_scroll_limit() == 20);
    }

    SECTION("clamping below minimum") {
        InputSettingsManager::instance().set_scroll_limit(0);
        REQUIRE(InputSettingsManager::instance().get_scroll_limit() == 1);
    }

    SECTION("clamping above maximum") {
        InputSettingsManager::instance().set_scroll_limit(99);
        REQUIRE(InputSettingsManager::instance().get_scroll_limit() == 20);
    }

    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager restart pending flag", "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("restart pending after scroll_throw change") {
        InputSettingsManager::instance().clear_restart_pending();
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == false);

        InputSettingsManager::instance().set_scroll_throw(30);
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == true);
    }

    SECTION("restart pending after scroll_limit change") {
        InputSettingsManager::instance().clear_restart_pending();
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == false);

        InputSettingsManager::instance().set_scroll_limit(15);
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == true);
    }

    SECTION("clear_restart_pending resets flag") {
        InputSettingsManager::instance().set_scroll_throw(30);
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == true);

        InputSettingsManager::instance().clear_restart_pending();
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == false);
    }

    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager subject values match getters",
                 "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("scroll_throw subject reflects setter") {
        InputSettingsManager::instance().set_scroll_throw(35);
        REQUIRE(lv_subject_get_int(InputSettingsManager::instance().subject_scroll_throw()) == 35);
    }

    SECTION("scroll_limit subject reflects setter") {
        InputSettingsManager::instance().set_scroll_limit(8);
        REQUIRE(lv_subject_get_int(InputSettingsManager::instance().subject_scroll_limit()) == 8);
    }

    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager debug_touches live-apply contract",
                 "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();
    InputSettingsManager::instance().clear_restart_pending();

    SECTION("set_debug_touches(true) flips RuntimeConfig immediately") {
        InputSettingsManager::instance().set_debug_touches(true);
        REQUIRE(RuntimeConfig::debug_touches() == true);
        REQUIRE(InputSettingsManager::instance().get_debug_touches() == true);
    }

    SECTION("set_debug_touches does NOT mark restart_pending") {
        // Live-apply is the whole point — restart prompt would be wrong here.
        InputSettingsManager::instance().set_debug_touches(true);
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == false);
    }

    SECTION("subject reflects setter") {
        InputSettingsManager::instance().set_debug_touches(true);
        REQUIRE(lv_subject_get_int(InputSettingsManager::instance().subject_debug_touches()) == 1);
        InputSettingsManager::instance().set_debug_touches(false);
        REQUIRE(lv_subject_get_int(InputSettingsManager::instance().subject_debug_touches()) == 0);
    }

    // Leave RuntimeConfig flag off so other tests don't render ripples.
    InputSettingsManager::instance().set_debug_touches(false);
    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager jitter_threshold clamps and persists",
                 "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("values above 30 clamp to 30") {
        InputSettingsManager::instance().set_jitter_threshold(50);
        REQUIRE(InputSettingsManager::instance().get_jitter_threshold() == 30);
    }

    SECTION("values below 0 clamp to 0") {
        InputSettingsManager::instance().set_jitter_threshold(-5);
        REQUIRE(InputSettingsManager::instance().get_jitter_threshold() == 0);
    }

    SECTION("in-range round trip") {
        InputSettingsManager::instance().set_jitter_threshold(15);
        REQUIRE(InputSettingsManager::instance().get_jitter_threshold() == 15);
    }

    SECTION("marks restart_pending") {
        InputSettingsManager::instance().clear_restart_pending();
        InputSettingsManager::instance().set_jitter_threshold(20);
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == true);
    }

    InputSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "InputSettingsManager scroll_guard persists and is restart-required",
                 "[input_settings]") {
    Config::get_instance();
    InputSettingsManager::instance().init_subjects();

    SECTION("round trip true/false") {
        InputSettingsManager::instance().set_scroll_guard(true);
        REQUIRE(InputSettingsManager::instance().get_scroll_guard() == true);
        InputSettingsManager::instance().set_scroll_guard(false);
        REQUIRE(InputSettingsManager::instance().get_scroll_guard() == false);
    }

    SECTION("subject reflects setter") {
        InputSettingsManager::instance().set_scroll_guard(true);
        REQUIRE(lv_subject_get_int(InputSettingsManager::instance().subject_scroll_guard()) == 1);
    }

    SECTION("marks restart_pending") {
        InputSettingsManager::instance().clear_restart_pending();
        InputSettingsManager::instance().set_scroll_guard(true);
        REQUIRE(InputSettingsManager::instance().is_restart_pending() == true);
    }

    InputSettingsManager::instance().deinit_subjects();
}

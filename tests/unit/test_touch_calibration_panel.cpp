// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_touch_calibration_panel.cpp
 * @brief Unit tests for TouchCalibrationPanel state machine
 *
 * Tests the 3-point touch calibration state machine:
 *
 * States:
 *   IDLE -> POINT_1 -> POINT_2 -> POINT_3 -> VERIFY -> COMPLETE
 *            |          |          |          |
 *            v          v          v          v
 *        (capture)  (capture)  (capture)  (accept/retry)
 *
 * Written TDD-style - tests WILL FAIL until TouchCalibrationPanel is implemented.
 */

#include "../test_helpers/touch_calibration_panel_test_access.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for TouchCalibrationPanel tests
 *
 * Provides panel instance and callback tracking for testing
 * state machine transitions.
 */
class TouchCalibrationPanelTestFixture {
  public:
    TouchCalibrationPanelTestFixture() {
        panel_ = std::make_unique<TouchCalibrationPanel>();
        panel_->set_screen_size(800, 480);

        // Set up callback to capture completion events
        panel_->set_completion_callback([this](const TouchCalibration* cal) {
            callback_called_ = true;
            if (cal != nullptr && cal->valid) {
                callback_calibration_ = *cal;
                callback_received_valid_ = true;
            } else {
                callback_received_valid_ = false;
            }
        });
    }

    ~TouchCalibrationPanelTestFixture() = default;

  protected:
    std::unique_ptr<TouchCalibrationPanel> panel_;
    bool callback_called_ = false;
    bool callback_received_valid_ = false;
    TouchCalibration callback_calibration_;

    /**
     * @brief Simulate capturing a raw touch point at current step
     */
    void capture_raw_point(int x, int y) {
        panel_->capture_point(Point{x, y});
    }

    /**
     * @brief Complete all 3 calibration points with valid data
     *
     * Uses points that form a valid triangle to ensure calibration succeeds.
     */
    void complete_all_points() {
        // POINT_1: target (120, 144) - simulate touch at similar raw position
        capture_raw_point(100, 120);
        // POINT_2: target (400, 408)
        capture_raw_point(380, 390);
        // POINT_3: target (680, 72)
        capture_raw_point(660, 60);
    }

    /**
     * @brief Get state name for debugging
     */
    std::string state_name() const {
        switch (panel_->get_state()) {
        case TouchCalibrationPanel::State::IDLE:
            return "IDLE";
        case TouchCalibrationPanel::State::POINT_1:
            return "POINT_1";
        case TouchCalibrationPanel::State::POINT_2:
            return "POINT_2";
        case TouchCalibrationPanel::State::POINT_3:
            return "POINT_3";
        case TouchCalibrationPanel::State::VERIFY:
            return "VERIFY";
        case TouchCalibrationPanel::State::COMPLETE:
            return "COMPLETE";
        default:
            return "UNKNOWN";
        }
    }
};

// ============================================================================
// Initial State Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture, "TouchCalibrationPanel: Initial state is IDLE",
                 "[touch-calibration][state][init]") {
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

// ============================================================================
// Start Calibration Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: start() transitions to POINT_1",
                 "[touch-calibration][state][start]") {
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);

    panel_->start();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: start() from non-IDLE resets to POINT_1",
                 "[touch-calibration][state][start]") {
    panel_->start();
    capture_raw_point(100, 100); // Move to POINT_2

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);

    panel_->start();

    // Should reset back to POINT_1
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
}

// ============================================================================
// Point Capture Sequence Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: capture_point advances POINT_1 to POINT_2",
                 "[touch-calibration][state][capture]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    capture_raw_point(100, 120);

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: capture_point advances POINT_2 to POINT_3",
                 "[touch-calibration][state][capture]") {
    panel_->start();
    capture_raw_point(100, 120); // POINT_1 -> POINT_2
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);

    capture_raw_point(380, 390);

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_3);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: capture_point advances POINT_3 to VERIFY",
                 "[touch-calibration][state][capture]") {
    panel_->start();
    capture_raw_point(100, 120); // POINT_1 -> POINT_2
    capture_raw_point(380, 390); // POINT_2 -> POINT_3
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_3);

    capture_raw_point(660, 60);

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: VERIFY state has valid calibration data",
                 "[touch-calibration][state][verify]") {
    panel_->start();
    complete_all_points();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    const TouchCalibration* cal = panel_->get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid == true);
}

// ============================================================================
// Verification Accept Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: accept() in VERIFY transitions to COMPLETE",
                 "[touch-calibration][state][accept]") {
    panel_->start();
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    panel_->accept();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::COMPLETE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: accept() invokes callback with valid calibration",
                 "[touch-calibration][callback][accept]") {
    panel_->start();
    complete_all_points();

    panel_->accept();

    REQUIRE(callback_called_ == true);
    REQUIRE(callback_received_valid_ == true);
    REQUIRE(callback_calibration_.valid == true);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: accept() is no-op outside VERIFY state",
                 "[touch-calibration][state][accept]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    panel_->accept();

    // Should still be in POINT_1, accept ignored
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
    REQUIRE(callback_called_ == false);
}

// ============================================================================
// Verification Retry Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: retry() in VERIFY returns to POINT_1",
                 "[touch-calibration][state][retry]") {
    panel_->start();
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    panel_->retry();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: retry() clears previous calibration data",
                 "[touch-calibration][state][retry]") {
    panel_->start();
    complete_all_points();

    const TouchCalibration* cal_before = panel_->get_calibration();
    REQUIRE(cal_before != nullptr);
    REQUIRE(cal_before->valid == true);

    panel_->retry();

    // After retry, calibration should be invalid until new points captured
    const TouchCalibration* cal_after = panel_->get_calibration();
    REQUIRE(cal_after == nullptr);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: retry() is no-op outside VERIFY state",
                 "[touch-calibration][state][retry]") {
    panel_->start();
    capture_raw_point(100, 120); // POINT_2
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);

    panel_->retry();

    // Should still be in POINT_2, retry ignored
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
}

// ============================================================================
// Cancel Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: cancel() from IDLE stays in IDLE",
                 "[touch-calibration][state][cancel]") {
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);

    panel_->cancel();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: cancel() from POINT_1 returns to IDLE",
                 "[touch-calibration][state][cancel]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    panel_->cancel();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: cancel() from POINT_2 returns to IDLE",
                 "[touch-calibration][state][cancel]") {
    panel_->start();
    capture_raw_point(100, 120);
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);

    panel_->cancel();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: cancel() from POINT_3 returns to IDLE",
                 "[touch-calibration][state][cancel]") {
    panel_->start();
    capture_raw_point(100, 120);
    capture_raw_point(380, 390);
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_3);

    panel_->cancel();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: cancel() from VERIFY returns to IDLE",
                 "[touch-calibration][state][cancel]") {
    panel_->start();
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    panel_->cancel();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: cancel() invokes callback with nullptr",
                 "[touch-calibration][callback][cancel]") {
    panel_->start();
    complete_all_points();

    panel_->cancel();

    REQUIRE(callback_called_ == true);
    REQUIRE(callback_received_valid_ == false);
}

// ============================================================================
// Invalid State Transition Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: capture_point in IDLE is no-op",
                 "[touch-calibration][state][invalid]") {
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);

    capture_raw_point(100, 100);

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: capture_point in VERIFY is no-op",
                 "[touch-calibration][state][invalid]") {
    panel_->start();
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    capture_raw_point(500, 500);

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: capture_point in COMPLETE is no-op",
                 "[touch-calibration][state][invalid]") {
    panel_->start();
    complete_all_points();
    panel_->accept();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::COMPLETE);

    capture_raw_point(500, 500);

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::COMPLETE);
}

// ============================================================================
// Target Position Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: get_target_position returns correct inset points",
                 "[touch-calibration][targets]") {
    SECTION("Step 0: 15% from left, 20% from top") {
        // 800 * 0.15 = 120, 480 * 0.20 = 96
        Point target = panel_->get_target_position(0);
        REQUIRE(target.x == 120);
        REQUIRE(target.y == 96);
    }

    SECTION("Step 1: center X, 78% from top") {
        // 800 * 0.50 = 400, 480 * 0.78 = 374
        Point target = panel_->get_target_position(1);
        REQUIRE(target.x == 400);
        REQUIRE(target.y == 374);
    }

    SECTION("Step 2: 85% from left, 20% from top") {
        // 800 * 0.85 = 680, 480 * 0.20 = 96
        Point target = panel_->get_target_position(2);
        REQUIRE(target.x == 680);
        REQUIRE(target.y == 96);
    }
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: get_target_position out of range returns (0,0)",
                 "[touch-calibration][targets]") {
    Point target_neg = panel_->get_target_position(-1);
    REQUIRE(target_neg.x == 0);
    REQUIRE(target_neg.y == 0);

    Point target_over = panel_->get_target_position(3);
    REQUIRE(target_over.x == 0);
    REQUIRE(target_over.y == 0);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: targets scale with screen size",
                 "[touch-calibration][targets]") {
    // Create panel with different screen size
    TouchCalibrationPanel panel_1024;
    panel_1024.set_screen_size(1024, 600);

    // Step 0: 15% from left, 20% from top
    // 1024 * 0.15 = 153.6 -> 153, 600 * 0.20 = 120
    Point target = panel_1024.get_target_position(0);
    REQUIRE(target.x == 153);
    REQUIRE(target.y == 120);
}

// ============================================================================
// Screen Size Configuration Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: set_screen_size updates target positions",
                 "[touch-calibration][config]") {
    Point target_before = panel_->get_target_position(0);

    panel_->set_screen_size(1280, 720);

    Point target_after = panel_->get_target_position(0);

    // 1280 * 0.15 = 192, 720 * 0.20 = 144
    REQUIRE(target_after.x == 192);
    REQUIRE(target_after.y == 144);
    REQUIRE(target_after.x != target_before.x);
    REQUIRE(target_after.y != target_before.y);
}

// ============================================================================
// Full Workflow Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: complete workflow IDLE -> COMPLETE",
                 "[touch-calibration][workflow]") {
    // Start in IDLE
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);

    // Begin calibration
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // Capture 3 points
    capture_raw_point(100, 120);
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);

    capture_raw_point(380, 390);
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_3);

    capture_raw_point(660, 60);
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    // Accept calibration
    panel_->accept();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::COMPLETE);

    // Verify callback was invoked with valid data
    REQUIRE(callback_called_ == true);
    REQUIRE(callback_received_valid_ == true);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: retry workflow loops back correctly",
                 "[touch-calibration][workflow]") {
    // Complete first attempt
    panel_->start();
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    // Retry
    panel_->retry();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // Complete second attempt
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    // Accept this time
    panel_->accept();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::COMPLETE);
    REQUIRE(callback_called_ == true);
    REQUIRE(callback_received_valid_ == true);
}

// ============================================================================
// Screen Size Change Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: retry after screen size change uses new size",
                 "[touch-calibration][state][resize]") {
    // Start calibration at 800x480
    panel_->start();
    complete_all_points();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    // Change screen size while in VERIFY state
    panel_->set_screen_size(1024, 600);

    // Retry should recalculate screen points with new size
    panel_->retry();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // Verify targets use new screen size
    Point target0 = panel_->get_target_position(0);
    Point target1 = panel_->get_target_position(1);
    Point target2 = panel_->get_target_position(2);

    // 1024 * 0.15 = 153.6 -> 153
    // 600 * 0.20 = 120
    REQUIRE(target0.x == 153);
    REQUIRE(target0.y == 120);

    // 1024 * 0.50 = 512
    // 600 * 0.78f = 467 (float truncation)
    REQUIRE(target1.x == 512);
    REQUIRE(target1.y == 467);

    // 1024 * 0.85 = 870.4 -> 870
    // 600 * 0.20 = 120
    REQUIRE(target2.x == 870);
    REQUIRE(target2.y == 120);
}

TEST_CASE_METHOD(TouchCalibrationPanelTestFixture,
                 "TouchCalibrationPanel: get_target_position reflects current screen size",
                 "[touch-calibration][state][resize]") {
    panel_->set_screen_size(800, 480);
    panel_->start();

    // Original targets for 800x480
    Point orig0 = panel_->get_target_position(0);
    REQUIRE(orig0.x == 120); // 800 * 0.15
    REQUIRE(orig0.y == 96);  // 480 * 0.20

    // Change screen size mid-calibration
    panel_->set_screen_size(1920, 1080);

    // get_target_position should now return values for new size
    Point new0 = panel_->get_target_position(0);
    REQUIRE(new0.x == 288); // 1920 * 0.15
    REQUIRE(new0.y == 216); // 1080 * 0.20
}

// ============================================================================
// Failure Callback Tests (Degenerate Points)
// ============================================================================

/**
 * @brief Test fixture with failure callback tracking
 */
class TouchCalibrationPanelFailureFixture {
  public:
    TouchCalibrationPanelFailureFixture() {
        panel_ = std::make_unique<TouchCalibrationPanel>();
        panel_->set_screen_size(800, 480);

        panel_->set_completion_callback([this](const TouchCalibration* cal) {
            completion_called_ = true;
            completion_valid_ = (cal != nullptr && cal->valid);
        });

        panel_->set_failure_callback([this](const char* reason) {
            failure_called_ = true;
            failure_reason_ = reason ? reason : "";
        });
    }

  protected:
    std::unique_ptr<TouchCalibrationPanel> panel_;
    bool completion_called_ = false;
    bool completion_valid_ = false;
    bool failure_called_ = false;
    std::string failure_reason_;
};

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: collinear points trigger failure callback",
                 "[touch-calibration][degenerate][failure]") {
    panel_->start();

    // Capture 3 collinear points (all on a straight line)
    panel_->capture_point(Point{100, 100});
    panel_->capture_point(Point{200, 200});
    panel_->capture_point(Point{300, 300}); // Third point triggers calibration

    // Should restart at POINT_1 due to degenerate points
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // Failure callback should have been invoked
    REQUIRE(failure_called_ == true);
    REQUIRE(failure_reason_.find("close together") != std::string::npos);

    // Completion callback should NOT have been called
    REQUIRE(completion_called_ == false);
}

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: duplicate points trigger failure callback",
                 "[touch-calibration][degenerate][failure]") {
    panel_->start();

    // Capture same point 3 times
    panel_->capture_point(Point{200, 200});
    panel_->capture_point(Point{200, 200});
    panel_->capture_point(Point{200, 200});

    // Should restart at POINT_1 due to degenerate points
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
    REQUIRE(failure_called_ == true);
}

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: valid points do not trigger failure callback",
                 "[touch-calibration][degenerate][failure]") {
    panel_->start();

    // Capture 3 well-distributed points (form a triangle)
    panel_->capture_point(Point{100, 120});
    panel_->capture_point(Point{380, 390});
    panel_->capture_point(Point{660, 60});

    // Should be in VERIFY state
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    // Failure callback should NOT have been invoked
    REQUIRE(failure_called_ == false);
}

// ============================================================================
// Axis Swap Integration Tests (panel-level)
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: swapped axes produce clean calibration via capture_point",
                 "[touch-calibration][axis-swap][panel]") {
    panel_->start();

    // Simulate a touchscreen with X/Y swapped.
    // Screen targets (800x480): (120,96), (400,374), (680,96)
    // Touch reports swapped coords: (96,120), (374,400), (96,680)
    panel_->capture_point(Point{96, 120});
    panel_->capture_point(Point{374, 400});
    panel_->capture_point(Point{96, 680});

    // Should reach VERIFY (not restart due to failure)
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
    REQUIRE(failure_called_ == false);

    const TouchCalibration* cal = panel_->get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid == true);
    REQUIRE(cal->axes_swapped == true);

    // Diagonal should dominate (clean matrix, not cross-coupled)
    float diagonal = std::abs(cal->a) + std::abs(cal->e);
    float off_diagonal = std::abs(cal->b) + std::abs(cal->d);
    REQUIRE(off_diagonal / diagonal < 0.5f);
}

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: non-swapped axes do not set axes_swapped flag",
                 "[touch-calibration][axis-swap][panel]") {
    panel_->start();

    // Normal (non-swapped) touch points close to screen targets
    panel_->capture_point(Point{125, 100});
    panel_->capture_point(Point{405, 378});
    panel_->capture_point(Point{685, 100});

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);

    const TouchCalibration* cal = panel_->get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid == true);
    REQUIRE(cal->axes_swapped == false);
}

// ============================================================================
// Recovery Tests
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: recovery after degenerate points",
                 "[touch-calibration][degenerate][recovery]") {
    panel_->start();

    // First attempt: degenerate points
    panel_->capture_point(Point{100, 100});
    panel_->capture_point(Point{200, 200});
    panel_->capture_point(Point{300, 300});

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
    REQUIRE(failure_called_ == true);

    // Reset failure flag
    failure_called_ = false;

    // Second attempt: valid points (already at POINT_1 from auto-restart)
    panel_->capture_point(Point{100, 120});
    panel_->capture_point(Point{380, 390});
    panel_->capture_point(Point{660, 60});

    // Should now be in VERIFY state
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
    REQUIRE(failure_called_ == false);

    // Accept and verify completion
    panel_->accept();
    REQUIRE(completion_called_ == true);
    REQUIRE(completion_valid_ == true);
}

// ============================================================================
// Multi-Sample Noise Rejection Tests (via add_sample)
// ============================================================================

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: consistent samples produce valid median",
                 "[touch-calibration][sampling]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // 3 consistent samples near (100, 120)
    panel_->add_sample(Point{98, 118});
    panel_->add_sample(Point{100, 121});
    panel_->add_sample(Point{102, 119});

    // Should advance to POINT_2
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
    REQUIRE(failure_called_ == false);
}

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: spread check rejects noisy samples",
                 "[touch-calibration][sampling][noise]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // 3 samples with Y spread > 60px threshold
    panel_->add_sample(Point{395, 67});
    panel_->add_sample(Point{396, 215});
    panel_->add_sample(Point{395, 65});

    // Should still be in POINT_1 (rejected, asked for retry)
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
    REQUIRE(failure_called_ == true);
}

TEST_CASE_METHOD(TouchCalibrationPanelFailureFixture,
                 "TouchCalibrationPanel: saturated sample excluded, remaining 2 valid",
                 "[touch-calibration][sampling][saturation]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // 2 good + 1 saturated (ADC max) — 2 valid meets MIN_VALID_SAMPLES
    panel_->add_sample(Point{100, 120});
    panel_->add_sample(Point{4095, 4095}); // saturated
    panel_->add_sample(Point{102, 119});

    // Saturated sample removed, remaining 2 are consistent
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
    REQUIRE(failure_called_ == false);
}

// ============================================================================
// Press-Debounce Tests (issue #943)
//
// A burst of LV_EVENT_PRESSED from a single physical contact must contribute
// at most one calibration sample. Gate is opt-in (HELIX_TOUCH_CAL_DEBOUNCE);
// tests toggle it deterministically via TouchCalibrationPanelTestAccess rather
// than the process-global env cache.
// ============================================================================

/**
 * @brief Fixture for press-debounce tests with a controllable injected clock.
 */
class TouchCalibrationPanelDebounceFixture {
  public:
    TouchCalibrationPanelDebounceFixture() {
        panel_ = std::make_unique<TouchCalibrationPanel>();
        panel_->set_screen_size(800, 480);

        // Deterministic monotonic-ms clock for the stall-guard. Tests advance
        // `fake_now_ms_` directly instead of spinning a real timer.
        TouchCalibrationPanelTestAccess::set_now_fn(*panel_, [this]() { return fake_now_ms_; });
    }

  protected:
    std::unique_ptr<TouchCalibrationPanel> panel_;
    uint32_t fake_now_ms_ = 0;

    void enable_debounce() {
        TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, true);
    }
    void disable_debounce() {
        TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, false);
    }

    int current_sample_count() const {
        return panel_->get_progress().current_sample;
    }
};

TEST_CASE_METHOD(TouchCalibrationPanelDebounceFixture,
                 "TouchCalibrationPanel: debounce collapses a press burst to one sample",
                 "[touch-calibration][debounce]") {
    enable_debounce();
    panel_->start(); // POINT_1, no auto-start tap involved
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // Five presses from one physical contact (no release between them).
    for (int i = 0; i < 5; ++i) {
        panel_->add_sample(Point{100, 120});
    }

    // Exactly one sample recorded for the current point; the rest were ignored
    // because awaiting_release_ was set after the first.
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
    REQUIRE(current_sample_count() == 1);
}

TEST_CASE_METHOD(TouchCalibrationPanelDebounceFixture,
                 "TouchCalibrationPanel: debounce normal flow with releases completes 3 points",
                 "[touch-calibration][debounce]") {
    enable_debounce();
    panel_->start();

    // Well-distributed triangle, one sample per point, releasing between each.
    const Point taps[3] = {{100, 120}, {380, 390}, {660, 60}};
    for (const auto& tap : taps) {
        for (int s = 0; s < 3; ++s) {
            // Burst within one contact — only the first should record.
            panel_->add_sample(tap);
            panel_->add_sample(tap);
            panel_->notify_release(); // finger lifts, clears the gate
        }
    }

    // Three points captured -> matrix computed -> VERIFY.
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
    const TouchCalibration* cal = panel_->get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid == true);
}

TEST_CASE_METHOD(TouchCalibrationPanelDebounceFixture,
                 "TouchCalibrationPanel: stall-guard clears gate after release timeout",
                 "[touch-calibration][debounce]") {
    enable_debounce();
    panel_->start();

    fake_now_ms_ = 1000;
    panel_->add_sample(Point{100, 120}); // records, arms deadline at 1000+timeout
    REQUIRE(current_sample_count() == 1);
    REQUIRE(TouchCalibrationPanelTestAccess::awaiting_release(*panel_) == true);

    // A second press while still awaiting release is ignored.
    panel_->add_sample(Point{101, 121});
    REQUIRE(current_sample_count() == 1);

    // Advance the clock past the stall-guard timeout WITHOUT a release.
    fake_now_ms_ = 1000 + TouchCalibrationPanelTestAccess::release_timeout_ms() + 1;

    // Next press now records (stall-guard auto-cleared the gate).
    panel_->add_sample(Point{102, 119});
    REQUIRE(current_sample_count() == 2);
}

TEST_CASE_METHOD(TouchCalibrationPanelDebounceFixture,
                 "TouchCalibrationPanel: debounce OFF preserves legacy multi-sample behavior",
                 "[touch-calibration][debounce]") {
    disable_debounce();
    panel_->start();

    // Three presses, NO releases — today's behavior records all three and
    // advances to the next point.
    panel_->add_sample(Point{98, 118});
    panel_->add_sample(Point{100, 121});
    panel_->add_sample(Point{102, 119});

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
}

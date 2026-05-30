// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file touch_calibration_panel.cpp
 * @brief Touch calibration panel state machine implementation
 */

#include "touch_calibration_panel.h"

#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace helix {

// Default screen dimensions used when invalid values are provided
constexpr int DEFAULT_SCREEN_WIDTH = 800;
constexpr int DEFAULT_SCREEN_HEIGHT = 480;

TouchCalibrationPanel::TouchCalibrationPanel()
    : debounce_enabled_(RuntimeConfig::touch_cal_debounce()) {}

TouchCalibrationPanel::~TouchCalibrationPanel() {
    stop_countdown_timer();
    stop_fast_revert_timer();
}

void TouchCalibrationPanel::set_completion_callback(CompletionCallback cb) {
    callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_failure_callback(FailureCallback cb) {
    failure_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_countdown_callback(CountdownCallback cb) {
    countdown_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_timeout_callback(TimeoutCallback cb) {
    timeout_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_fast_revert_callback(FastRevertCallback cb) {
    fast_revert_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_sample_progress_callback(SampleProgressCallback cb) {
    sample_progress_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_verify_timeout_seconds(int seconds) {
    verify_timeout_seconds_ = seconds;
}

void TouchCalibrationPanel::set_screen_size(int width, int height) {
    // Validate screen dimensions - reject zero or negative values
    if (width <= 0 || height <= 0) {
        spdlog::warn("[TouchCalibrationPanel] Invalid screen size {}x{}, using defaults {}x{}",
                     width, height, DEFAULT_SCREEN_WIDTH, DEFAULT_SCREEN_HEIGHT);
        screen_width_ = DEFAULT_SCREEN_WIDTH;
        screen_height_ = DEFAULT_SCREEN_HEIGHT;
        return;
    }
    screen_width_ = width;
    screen_height_ = height;
}

Point TouchCalibrationPanel::compute_target_position(int step) const {
    switch (step) {
    case 0:
        return {static_cast<int>(screen_width_ * TARGET_0_X_RATIO),
                static_cast<int>(screen_height_ * TARGET_0_Y_RATIO)};
    case 1:
        return {static_cast<int>(screen_width_ * TARGET_1_X_RATIO),
                static_cast<int>(screen_height_ * TARGET_1_Y_RATIO)};
    case 2:
        return {static_cast<int>(screen_width_ * TARGET_2_X_RATIO),
                static_cast<int>(screen_height_ * TARGET_2_Y_RATIO)};
    default:
        return Point{0, 0};
    }
}

void TouchCalibrationPanel::start() {
    state_ = State::POINT_1;
    calibration_.valid = false;
    reset_samples();

    // Calculate screen target positions using named constants
    screen_points_[0] = compute_target_position(0);
    screen_points_[1] = compute_target_position(1);
    screen_points_[2] = compute_target_position(2);
}

void TouchCalibrationPanel::capture_point(Point raw) {
    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] capture_point: state={} raw=({},{})", static_cast<int>(state_),
                     raw.x, raw.y);
    }

    switch (state_) {
    case State::POINT_1:
        touch_points_[0] = raw;
        state_ = State::POINT_2;
        break;
    case State::POINT_2:
        touch_points_[1] = raw;
        state_ = State::POINT_3;
        break;
    case State::POINT_3:
        touch_points_[2] = raw;

        if (!compute_calibration(screen_points_, touch_points_, calibration_)) {
            spdlog::warn(
                "[TouchCalibrationPanel] Calibration failed (degenerate points), restarting");
            state_ = State::POINT_1;
            calibration_.valid = false;
            if (failure_callback_) {
                failure_callback_("Touch points too close together. Please try again.");
            }
            break;
        }

        // Detect and correct swapped touch axes (e.g., Ender 5 Max screens)
        // This swaps touch_points_ in-place and recomputes calibration_ if needed
        detect_and_correct_axis_swap(calibration_, screen_points_, touch_points_);

        if (is_touch_debug_enabled()) {
            spdlog::warn("[TouchDebug] calibration computed for all 3 points:");
            for (int i = 0; i < 3; i++) {
                spdlog::warn("[TouchDebug]   point[{}]: screen=({},{}) touch=({},{})", i,
                             screen_points_[i].x, screen_points_[i].y, touch_points_[i].x,
                             touch_points_[i].y);
            }
            if (calibration_.axes_swapped) {
                spdlog::warn("[TouchDebug]   axes were auto-swapped");
            }
        }

        // Validate the matrix produces reasonable results
        if (!validate_calibration_result(calibration_, screen_points_, touch_points_, screen_width_,
                                         screen_height_)) {
            spdlog::warn(
                "[TouchCalibrationPanel] Calibration matrix failed validation, restarting");
            state_ = State::POINT_1;
            calibration_.valid = false;
            if (failure_callback_) {
                failure_callback_("Calibration produced unusual results. Please try again.");
            }
            break;
        }

        state_ = State::VERIFY;
        start_countdown_timer();
        start_fast_revert_timer();
        break;
    default:
        // No-op in IDLE, VERIFY, COMPLETE states
        break;
    }
}

bool TouchCalibrationPanel::is_bad_sample(const Point& sample) const {
    // ADC saturation (12-bit or 16-bit max)
    return sample.x == 4095 || sample.y == 4095 || sample.x == 65535 || sample.y == 65535;
}

void TouchCalibrationPanel::reset_samples() {
    sample_count_ = 0;
}

bool TouchCalibrationPanel::compute_median_point(Point& out) {
    std::vector<int> valid_x, valid_y;
    for (int i = 0; i < sample_count_; i++) {
        Point p{sample_buffer_[i].x, sample_buffer_[i].y};
        if (!is_bad_sample(p)) {
            valid_x.push_back(p.x);
            valid_y.push_back(p.y);
        }
    }

    if (static_cast<int>(valid_x.size()) < MIN_VALID_SAMPLES) {
        spdlog::warn("[TouchCalibrationPanel] Only {}/{} valid samples (need {})", valid_x.size(),
                     sample_count_, MIN_VALID_SAMPLES);
        return false;
    }

    std::sort(valid_x.begin(), valid_x.end());
    std::sort(valid_y.begin(), valid_y.end());

    // Reject if samples have excessive spread (noisy controller)
    int x_spread = valid_x.back() - valid_x.front();
    int y_spread = valid_y.back() - valid_y.front();
    if (x_spread > MAX_SAMPLE_SPREAD || y_spread > MAX_SAMPLE_SPREAD) {
        spdlog::warn("[TouchCalibrationPanel] Sample spread too large "
                     "(x_spread={}, y_spread={}, max={})",
                     x_spread, y_spread, MAX_SAMPLE_SPREAD);
        return false;
    }

    size_t mid_x = valid_x.size() / 2;
    size_t mid_y = valid_y.size() / 2;
    out.x = valid_x[mid_x];
    out.y = valid_y[mid_y];

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] median computation: {}/{} valid samples", valid_x.size(),
                     sample_count_);
        for (size_t i = 0; i < valid_x.size(); i++) {
            spdlog::warn("[TouchDebug]   valid[{}]: ({},{})", i, valid_x[i], valid_y[i]);
        }
        spdlog::warn("[TouchDebug]   spread: x={}, y={} (max={})", x_spread, y_spread,
                     MAX_SAMPLE_SPREAD);
        spdlog::warn("[TouchDebug]   median: ({},{})", out.x, out.y);
    }

    spdlog::debug("[TouchCalibrationPanel] Median from {}/{} valid samples: ({}, {})",
                  valid_x.size(), sample_count_, out.x, out.y);
    return true;
}

TouchCalibrationPanel::Progress TouchCalibrationPanel::get_progress() const {
    Progress p{};
    p.state = state_;
    p.current_sample = sample_count_;
    p.total_samples = SAMPLES_REQUIRED;

    switch (state_) {
    case State::POINT_1:
        p.point_num = 1;
        break;
    case State::POINT_2:
        p.point_num = 2;
        break;
    case State::POINT_3:
        p.point_num = 3;
        break;
    default:
        p.point_num = 0;
        break;
    }
    return p;
}

void TouchCalibrationPanel::add_sample(Point raw) {
    // Press-debounce gate (issue #943): when enabled, a physical contact may
    // contribute at most one sample. A burst of PRESSED events from one tap is
    // ignored until notify_release() (or the stall-guard) clears the gate.
    if (debounce_enabled_ && awaiting_release_) {
        // Stall-guard: some controllers emit PRESSED without a matching RELEASED.
        // If the gate has been held longer than the timeout, clear it and proceed
        // so calibration cannot wedge forever.
        if (now_fn_() >= release_deadline_ms_) {
            awaiting_release_ = false;
        } else {
            if (is_touch_debug_enabled()) {
                spdlog::warn("[TouchDebug] ignored press (awaiting release)");
            }
            return;
        }
    }

    // Auto-start on first tap if in IDLE state (don't count this tap as a sample —
    // the crosshair isn't visible yet, so the user's first tap ON the crosshair is touch 1)
    if (state_ == State::IDLE) {
        start();
        // Arm the gate so the same physical contact's burst doesn't immediately
        // record sample 1 of POINT_1 without a finger-lift first.
        arm_release_gate();
        return;
    }

    if (state_ != State::POINT_1 && state_ != State::POINT_2 && state_ != State::POINT_3) {
        return;
    }

    if (sample_count_ < SAMPLES_REQUIRED) {
        sample_buffer_[sample_count_] = {raw.x, raw.y};
        sample_count_++;

        // Arm the gate: the next press from this same contact is debounced until
        // a finger-lift (RELEASED) or the stall-guard timeout.
        arm_release_gate();

        if (is_touch_debug_enabled()) {
            auto p = get_progress();
            spdlog::warn("[TouchDebug] sample {}/{} for point {}: raw=({},{}){}", sample_count_,
                         SAMPLES_REQUIRED, p.point_num, raw.x, raw.y,
                         is_bad_sample(raw) ? " REJECTED" : "");
        }

        // Only fire progress callback for intermediate samples, not the final
        // one that triggers state transition (avoids showing "touch 6 of 5")
        if (sample_count_ < SAMPLES_REQUIRED && sample_progress_callback_) {
            sample_progress_callback_();
        }
    }

    if (sample_count_ >= SAMPLES_REQUIRED) {
        Point median;
        if (compute_median_point(median)) {
            capture_point(median);
        } else {
            if (failure_callback_) {
                failure_callback_("Too much noise — tap the target again slowly and precisely.");
            }
        }
        reset_samples();
    }
}

void TouchCalibrationPanel::arm_release_gate() {
    if (!debounce_enabled_) {
        return;
    }
    awaiting_release_ = true;
    release_deadline_ms_ = now_fn_() + RELEASE_TIMEOUT_MS;
}

void TouchCalibrationPanel::notify_release() {
    // No-op when debounce is disabled — keeps the default path byte-for-byte
    // identical to pre-#943 behavior.
    if (!debounce_enabled_) {
        return;
    }
    awaiting_release_ = false;
}

void TouchCalibrationPanel::accept() {
    stop_countdown_timer();
    stop_fast_revert_timer();

    if (state_ != State::VERIFY) {
        return;
    }

    state_ = State::COMPLETE;
    if (callback_) {
        callback_(&calibration_);
    }
}

void TouchCalibrationPanel::retry() {
    if (state_ != State::VERIFY) {
        return;
    }

    stop_countdown_timer();
    stop_fast_revert_timer();
    start(); // Resets state to POINT_1 and recalculates target positions
}

void TouchCalibrationPanel::cancel() {
    stop_countdown_timer();
    stop_fast_revert_timer();

    state_ = State::IDLE;
    calibration_.valid = false;
    if (callback_) {
        callback_(nullptr);
    }
}

TouchCalibrationPanel::State TouchCalibrationPanel::get_state() const {
    return state_;
}

Point TouchCalibrationPanel::get_target_position(int step) const {
    if (step < 0 || step > 2) {
        return Point{0, 0};
    }
    // Delegate to private helper that uses named constants
    return compute_target_position(step);
}

const TouchCalibration* TouchCalibrationPanel::get_calibration() const {
    if ((state_ == State::VERIFY || state_ == State::COMPLETE) && calibration_.valid) {
        return &calibration_;
    }
    return nullptr;
}

void TouchCalibrationPanel::start_countdown_timer() {
    countdown_remaining_ = verify_timeout_seconds_;
    countdown_timer_ = lv_timer_create(countdown_timer_cb, 1000, this);
    spdlog::debug("[TouchCalibrationPanel] Started countdown timer: {} seconds",
                  countdown_remaining_);

    // Immediately notify with initial value
    if (countdown_callback_) {
        countdown_callback_(countdown_remaining_);
    }
}

void TouchCalibrationPanel::stop_countdown_timer() {
    if (countdown_timer_ != nullptr) {
        lv_timer_delete(countdown_timer_);
        countdown_timer_ = nullptr;
        spdlog::debug("[TouchCalibrationPanel] Stopped countdown timer");
    }
}

void TouchCalibrationPanel::countdown_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TouchCalibrationPanel*>(lv_timer_get_user_data(timer));
    self->countdown_remaining_--;
    spdlog::debug("[TouchCalibrationPanel] Countdown tick: {} seconds remaining",
                  self->countdown_remaining_);

    if (self->countdown_remaining_ > 0) {
        if (self->countdown_callback_) {
            self->countdown_callback_(self->countdown_remaining_);
        }
    } else {
        spdlog::debug("[TouchCalibrationPanel] Countdown expired, invoking timeout callback");
        if (self->timeout_callback_) {
            self->timeout_callback_();
        }
        self->stop_countdown_timer();
    }
}

void TouchCalibrationPanel::report_verify_touch(bool on_screen) {
    if (state_ != State::VERIFY)
        return;
    verify_raw_touch_count_++;
    if (on_screen) {
        verify_onscreen_touch_count_++;
    }
}

void TouchCalibrationPanel::start_fast_revert_timer() {
    verify_raw_touch_count_ = 0;
    verify_onscreen_touch_count_ = 0;
    fast_revert_timer_ = lv_timer_create(fast_revert_timer_cb, FAST_REVERT_CHECK_MS, this);
    lv_timer_set_repeat_count(fast_revert_timer_, 1);
    spdlog::debug("[TouchCalibrationPanel] Started fast-revert timer ({}ms)", FAST_REVERT_CHECK_MS);
}

void TouchCalibrationPanel::stop_fast_revert_timer() {
    if (fast_revert_timer_) {
        lv_timer_delete(fast_revert_timer_);
        fast_revert_timer_ = nullptr;
    }
}

void TouchCalibrationPanel::fast_revert_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TouchCalibrationPanel*>(lv_timer_get_user_data(timer));
    self->fast_revert_timer_ = nullptr; // Timer auto-deletes (repeat_count=1)

    if (self->state_ != State::VERIFY)
        return;

    if (self->verify_raw_touch_count_ > 0 && self->verify_onscreen_touch_count_ == 0) {
        spdlog::warn("[TouchCalibrationPanel] Fast-revert: {} raw touches, 0 on-screen — "
                     "matrix is broken, reverting",
                     self->verify_raw_touch_count_);
        if (self->fast_revert_callback_) {
            self->fast_revert_callback_();
        }
    } else {
        spdlog::debug("[TouchCalibrationPanel] Fast-revert check passed: {}/{} on-screen",
                      self->verify_onscreen_touch_count_, self->verify_raw_touch_count_);
    }
}

} // namespace helix

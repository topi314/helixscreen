// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "touch_calibration_panel.h"

#include <cstdint>
#include <functional>

namespace helix {

/**
 * @brief Test-only access to TouchCalibrationPanel debounce internals.
 *
 * The press-debounce gate (`debounce_enabled_`) is normally seeded once from
 * the process-global `RuntimeConfig::touch_cal_debounce()` static cache. That
 * cache is order-dependent across a test binary, so tests must NOT rely on the
 * env var to toggle it. This access class provides deterministic seams:
 *
 *  - `set_debounce_enabled()` overrides the gate per-panel-instance.
 *  - `set_now_fn()` injects a monotonic-ms clock so the stall-guard timeout
 *    can be advanced synchronously without spinning a real timer.
 */
class TouchCalibrationPanelTestAccess {
  public:
    static void set_debounce_enabled(TouchCalibrationPanel& p, bool enabled) {
        p.debounce_enabled_ = enabled;
    }

    static bool debounce_enabled(const TouchCalibrationPanel& p) {
        return p.debounce_enabled_;
    }

    static bool awaiting_release(const TouchCalibrationPanel& p) {
        return p.awaiting_release_;
    }

    /// Inject a deterministic monotonic-ms clock used only for the stall-guard.
    static void set_now_fn(TouchCalibrationPanel& p, std::function<uint32_t()> fn) {
        p.now_fn_ = std::move(fn);
    }

    static uint32_t release_timeout_ms() {
        return TouchCalibrationPanel::RELEASE_TIMEOUT_MS;
    }
};

} // namespace helix

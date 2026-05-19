// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_start_collector.h"

class PrintStartCollectorTestAccess {
  public:
    /// Wind back the start time to simulate elapsed seconds
    static void set_elapsed_seconds(PrintStartCollector& c, int seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        c.printing_state_start_ = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
    }

    /// Set predicted_total_seconds_ directly for timeout threshold tests
    static void set_predicted_total(PrintStartCollector& c, float seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        c.predicted_total_seconds_ = seconds;
    }

    /// Read predicted_total_seconds_
    static float get_predicted_total(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.predicted_total_seconds_;
    }

    /// Wind back the silent-progression "temps ready" timestamp to simulate
    /// elapsed seconds since temps became ready. The collector sets this on
    /// the first tick where bed+nozzle reach target — bypassing the natural
    /// path keeps tests deterministic.
    static void set_temps_ready_elapsed_seconds(PrintStartCollector& c, int seconds) {
        c.temps_ready_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
    }

    /// Reset silent_progression_idx_ to N (default 0) so a test can rewind
    /// after a fired entry.
    static void set_silent_progression_idx(PrintStartCollector& c, size_t idx) {
        c.silent_progression_idx_ = idx;
    }
};

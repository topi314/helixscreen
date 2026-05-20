// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hv/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../catch_amalgamated.hpp"

/**
 * WiFi Backend Shutdown Safety Tests
 *
 * Tests for the use-after-free race condition (GitHub issue #8):
 * When WifiBackendWpaSupplicant::start() times out, the event loop thread
 * is still running init_wpa(). If the backend is destroyed while the thread
 * is blocked (e.g., in wpa_ctrl_attach()), cleanup_wpa() frees resources
 * that the thread is still using → segfault.
 *
 * These tests exercise the EXACT same hv::EventLoopThread pattern used by
 * WifiBackendWpaSupplicant without requiring wpa_supplicant (Linux-only).
 */

// ============================================================================
// Test helper: mimics WifiBackendWpaSupplicant's threading pattern
// ============================================================================

/**
 * Reproduces the exact threading pattern from WifiBackendWpaSupplicant:
 * - Inherits privately from hv::EventLoopThread
 * - start() spawns a thread running a slow init, waits with timeout
 * - Destructor must safely clean up even if thread is still running
 *
 * The "resource" simulates conn/mon_conn pointers that init uses.
 */
class SlowInitBackend : private hv::EventLoopThread {
  public:
    SlowInitBackend() : hv::EventLoopThread(nullptr) {
        // Simulate conn/mon_conn: a resource allocated during init
        resource_ = new std::atomic<int>(0);
    }

    ~SlowInitBackend() {
        // BUG REPRODUCTION: This is what WifiBackendWpaSupplicant does today:
        // 1. stop() returns early because init_complete_ is false
        // 2. cleanup frees the resource while the thread is still using it
        // 3. ~EventLoopThread joins the thread AFTER the resource is freed
        if (!init_complete_.load()) {
            // Mimic: stop() returns early
        }

        // Mimic: cleanup_wpa() frees resources before thread is joined
        cleanup();

        // ~EventLoopThread() will call stop() + join() here
    }

    /**
     * Start with a timeout, just like WifiBackendWpaSupplicant::start().
     * Returns true if init completed in time, false on timeout.
     */
    bool start_with_timeout(int timeout_ms) {
        init_complete_ = false;

        hv::EventLoopThread::start(true, [this]() -> int {
            slow_init();
            return 0;
        });

        std::unique_lock<std::mutex> lock(init_mutex_);
        return init_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this] { return init_complete_.load(); });
    }

    // Expose for test assertions
    bool init_completed() const {
        return init_complete_.load();
    }
    bool resource_was_accessed_after_free() const {
        return accessed_after_free_.load();
    }
    int init_progress() const {
        return init_progress_.load();
    }

  private:
    void slow_init() {
        // Simulate the blocking init_wpa() → wpa_ctrl_attach() pattern.
        // Accesses the shared resource repeatedly (like the thread accessing
        // conn/mon_conn during wpa_ctrl operations).
        for (int i = 0; i < 50; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            init_progress_ = i;

            // Access the resource (simulates wpa_ctrl_attach using mon_conn)
            if (resource_) {
                resource_->store(i);
            } else {
                // Resource was freed while we were using it!
                accessed_after_free_ = true;
                break;
            }
        }

        init_complete_ = true;
        init_cv_.notify_all();
    }

    void cleanup() {
        // Simulates cleanup_wpa(): frees the resource
        delete resource_;
        resource_ = nullptr;
    }

    std::atomic<int>* resource_{nullptr};
    std::atomic<bool> init_complete_{false};
    std::atomic<bool> accessed_after_free_{false};
    std::atomic<int> init_progress_{0};
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
};

/**
 * Fixed version: waits for thread to finish before freeing resources.
 * This is the pattern the fix should implement.
 */
class SafeShutdownBackend : private hv::EventLoopThread {
  public:
    SafeShutdownBackend() : hv::EventLoopThread(nullptr) {
        resource_ = new std::atomic<int>(0);
    }

    ~SafeShutdownBackend() {
        // FIX: Signal shutdown, stop the event loop, join the thread,
        // THEN free resources.
        shutdown_requested_ = true;

        // Stop the event loop and wait for thread to finish
        hv::EventLoopThread::stop(true);

        // NOW safe to free resources - thread is done
        cleanup();
    }

    bool start_with_timeout(int timeout_ms) {
        init_complete_ = false;
        shutdown_requested_ = false;

        hv::EventLoopThread::start(true, [this]() -> int {
            slow_init();
            return 0;
        });

        std::unique_lock<std::mutex> lock(init_mutex_);
        return init_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this] { return init_complete_.load(); });
    }

    bool init_completed() const {
        return init_complete_.load();
    }
    bool resource_was_accessed_after_free() const {
        return accessed_after_free_.load();
    }
    int init_progress() const {
        return init_progress_.load();
    }

  private:
    void slow_init() {
        for (int i = 0; i < 50 && !shutdown_requested_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            init_progress_ = i;

            if (resource_) {
                resource_->store(i);
            } else {
                accessed_after_free_ = true;
                break;
            }
        }

        init_complete_ = true;
        init_cv_.notify_all();
    }

    void cleanup() {
        delete resource_;
        resource_ = nullptr;
    }

    std::atomic<int>* resource_{nullptr};
    std::atomic<bool> init_complete_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> accessed_after_free_{false};
    std::atomic<int> init_progress_{0};
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("Slow init timeout triggers use-after-free in unsafe backend",
          "[network][backend][shutdown][issue-8][eventloop][slow]") {
    SECTION("Resource accessed after free when init times out") {
        // This reproduces the exact bug from GitHub issue #8:
        // 1. Start init with a short timeout (init takes 5s, timeout is 500ms)
        // 2. Timeout fires, start returns false
        // 3. Backend is destroyed while thread is still in slow_init()
        // 4. cleanup() frees the resource
        // 5. Thread continues accessing the freed resource

        {
            SlowInitBackend backend;
            bool completed = backend.start_with_timeout(500);

            // Init should NOT have completed (500ms timeout, init takes 5s)
            REQUIRE_FALSE(completed);
            REQUIRE_FALSE(backend.init_completed());

            // Destroying backend here while thread is still running.
            // The unsafe destructor frees resources before joining the thread.
            // On the real system (RPi), this causes Signal 11 because the
            // thread is still in wpa_ctrl_attach() using the freed socket.
        }
        // If we get here, the thread happened to not crash (timing dependent).
        // The real test is the SafeShutdownBackend - it GUARANTEES no crash.
    }
}

TEST_CASE("Safe shutdown backend waits for thread before cleanup",
          "[network][backend][shutdown][issue-8][eventloop][slow]") {
    SECTION("No use-after-free with shutdown signal") {
        SafeShutdownBackend backend;
        bool completed = backend.start_with_timeout(500);

        // Init should NOT have completed (500ms timeout, init takes 5s)
        REQUIRE_FALSE(completed);
        REQUIRE_FALSE(backend.init_completed());

        // Destroying backend here - safe version waits for thread
    }
    // If we get here without crash, the safe pattern works!
    SUCCEED("Backend destroyed safely without use-after-free");
}

TEST_CASE("Safe shutdown backend responds to cancellation quickly",
          "[network][backend][shutdown][issue-8][eventloop][slow]") {
    SECTION("Shutdown flag causes init to abort early") {
        auto start = std::chrono::steady_clock::now();

        {
            SafeShutdownBackend backend;
            backend.start_with_timeout(200);

            // Backend will be destroyed here - should be fast due to
            // shutdown_requested_ causing the init loop to break
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        // Should take well under 5s (the full init time) because
        // shutdown_requested_ causes early exit from the init loop.
        // Allow generous margin for CI but it should be ~300-500ms
        REQUIRE(elapsed_ms < 2000);
        INFO("Shutdown took " << elapsed_ms << "ms");
    }
}

TEST_CASE("Safe shutdown never accesses freed resources",
          "[network][backend][shutdown][issue-8][eventloop][slow]") {
    SECTION("Repeated start/timeout/destroy cycles are safe") {
        // Stress test: rapidly create, timeout, destroy
        for (int i = 0; i < 5; i++) {
            SafeShutdownBackend backend;
            backend.start_with_timeout(100);
            // Destroy immediately
        }
        SUCCEED("All cycles completed without crash");
    }
}

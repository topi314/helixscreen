// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file moonraker_client.cpp
 * @brief WebSocket client for Moonraker printer API communication
 *
 * @pattern libhv WebSocketClient with atomic state machine
 * @threading Callbacks run on libhv event loop thread - use helix::ui::async_call() for LVGL
 * @gotchas is_destroying_ flag blocks callbacks during destruction; skip cleanup during static
 * destruction
 *
 * @see moonraker_manager.cpp, printer_state.cpp
 */

#include "moonraker_client.h"

#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "helix_version.h"
#include "printer_state.h"
#include "system/telemetry_manager.h"

#include <sstream> // For annotate_gcode()

using namespace helix;

using namespace hv;

// Anonymous namespace for file-scoped state
namespace {
// Rate limiting flags for reconnection notifications
std::atomic<bool> g_already_notified_max_attempts{false};
std::atomic<bool> g_already_notified_disconnect{false};

// Reset notification flags on successful connection
void reset_notification_flags() {
    g_already_notified_max_attempts.store(false);
    g_already_notified_disconnect.store(false);
}

// Annotate G-code with source comment for traceability
// Handles multi-line G-code by adding comment to each line
std::string annotate_gcode(const std::string& gcode) {
    constexpr const char* GCODE_SOURCE_COMMENT = " ; from helixscreen";

    std::string result;
    result.reserve(gcode.size() + 20 * std::count(gcode.begin(), gcode.end(), '\n') + 20);

    std::istringstream stream(gcode);
    std::string line;
    bool first = true;

    while (std::getline(stream, line)) {
        if (!first) {
            result += '\n';
        }
        first = false;

        // Only add comment to non-empty lines
        if (!line.empty() && line.find_first_not_of(" \t\r") != std::string::npos) {
            result += line + GCODE_SOURCE_COMMENT;
        } else {
            result += line;
        }
    }

    return result;
}
} // namespace

MoonrakerClient::MoonrakerClient(EventLoopPtr loop)
    : WebSocketClient(loop), discovery_(*this), was_connected_(false),
      connection_state_(ConnectionState::DISCONNECTED),
      connection_timeout_ms_(10000) // Default 10 seconds
      ,
      keepalive_interval_ms_(10000) // Default 10 seconds
      ,
      reconnect_min_delay_ms_(200) // Default 200ms
      ,
      reconnect_max_delay_ms_(2000) { // Default 2 seconds
}

void MoonrakerClient::start_health_timer() {
    if (health_timer_id_ != 0) {
        return; // Already running
    }
    auto l = loop();
    if (!l) {
        return;
    }
    health_timer_id_ = l->setInterval(HEALTH_CHECK_INTERVAL_MS, [this](hv::TimerID) {
        // Acquire shared lock to prevent destructor from proceeding while we execute.
        // try_to_lock ensures we never block if destructor holds the exclusive lock.
        // Without this lock, the destructor can destroy members (logger, tracker_)
        // while this callback is mid-execution — SIGBUS after long uptimes. (#717)
        std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
        if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire)) {
            return;
        }

        // Check pending request timeouts
        process_timeouts();

        // Check for stalled reconnection
        ConnectionState state = connection_state_.load();
        if (state == ConnectionState::RECONNECTING) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - reconnect_started_at_)
                    .count();
            if (elapsed > MAX_RECONNECT_STALL_MS) {
                spdlog::error("[Moonraker Client] Reconnection stalled for {}ms, giving up",
                              elapsed);
                set_connection_state(ConnectionState::FAILED);
                emit_event(MoonrakerEventType::CONNECTION_FAILED,
                           "Unable to reach printer. Check power and network connection.", true);
            }
        }
    });
    spdlog::debug("[Moonraker Client] Health check timer started ({}ms interval)",
                  HEALTH_CHECK_INTERVAL_MS);
}

void MoonrakerClient::stop_health_timer() {
    if (health_timer_id_ == 0) {
        return;
    }
    auto l = loop();
    if (l) {
        l->killTimer(health_timer_id_);
    }
    health_timer_id_ = 0;
    spdlog::debug("[Moonraker Client] Health check timer stopped");
}

MoonrakerClient::~MoonrakerClient() {
    // Set destroying flag FIRST so callbacks see it immediately
    is_destroying_.store(true, std::memory_order_release);

    // Stop health timer before waiting for callbacks (timer fires on event loop thread)
    stop_health_timer();

    // Wait for any in-flight callbacks to finish. Callbacks hold a shared lock;
    // acquiring an exclusive lock here blocks until all shared locks are released.
    // Once acquired, no new callback can start (try_to_lock will fail).
    {
        std::unique_lock<std::shared_mutex> lk(callback_lifecycle_mutex_);
    } // Release immediately — just needed to wait for in-flight callbacks

    // Now safe to reset lifetime guards (no callbacks can be mid-execution).
    // destruction_guard_ signals object destruction to SubscriptionGuard and
    // other external holders; lifetime_guard_ covers WS callbacks.
    lifetime_guard_.reset();
    destruction_guard_.reset();

    // Disable auto-reconnect BEFORE closing - prevents libhv from attempting
    // reconnection after we've started destruction (avoids stderr "No route to host")
    setReconnect(nullptr);

    // Close WebSocket connection - replace callbacks with no-ops to prevent new callbacks
    // from firing during destruction. The base class destructor will handle socket cleanup.
    onopen = []() {};
    onmessage = [](const std::string&) {};
    onclose = []() {};

    // Clear state change callback without locking (destructor context)
    state_change_callback_ = nullptr;

    // Pending requests are dropped without invoking error callbacks.
    // During destruction, callback targets (UI panels, file providers, etc.) may
    // already be destroyed — invoking them would be use-after-free.
    // The tracker_'s default destructor will clear the map automatically.

    // Clear method callbacks safely. Lambdas in this map may capture shared_ptrs
    // to objects (e.g. NoiseCheckCollector) whose destructors call
    // unregister_method_callback(). By moving the map to a local first, the member
    // is empty when those destructors fire, so unregister finds nothing and returns
    // harmlessly. The mutex is safe here since we're on the main thread and not
    // in static destruction (we got here through normal Application::shutdown()).
    if (callbacks_mutex_.try_lock()) {
        decltype(method_callbacks_) doomed_callbacks = std::move(method_callbacks_);
        method_callbacks_.clear();
        callbacks_mutex_.unlock();
        // doomed_callbacks destructs here - lambda destructors may call unregister,
        // but method_callbacks_ is now empty so they'll no-op
    }
}

void MoonrakerClient::set_connection_state(ConnectionState new_state) {
    ConnectionState old_state = connection_state_.exchange(new_state);

    if (old_state != new_state) {
        const char* state_names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED", "RECONNECTING",
                                     "FAILED"};
        spdlog::debug("[Moonraker Client] Connection state: {} -> {}",
                      state_names[static_cast<int>(old_state)],
                      state_names[static_cast<int>(new_state)]);

        // Handle state-specific logic
        if (new_state == ConnectionState::RECONNECTING) {
            if (old_state != ConnectionState::RECONNECTING) {
                reconnect_started_at_ = std::chrono::steady_clock::now();
            }
            reconnect_attempts_++;
            if (max_reconnect_attempts_ > 0 && reconnect_attempts_ >= max_reconnect_attempts_) {
                spdlog::error("[Moonraker Client] Max reconnect attempts ({}) exceeded",
                              max_reconnect_attempts_);
                TelemetryManager::instance().record_error(
                    "websocket", "reconnect_failed",
                    fmt::format("max attempts ({}) exceeded", max_reconnect_attempts_));

                // Emit event only once during reconnect sequence
                if (!g_already_notified_max_attempts.load()) {
                    emit_event(MoonrakerEventType::CONNECTION_FAILED,
                               fmt::format("Unable to reach printer after {} attempts. "
                                           "Check power and network connection.",
                                           max_reconnect_attempts_),
                               true);
                    g_already_notified_max_attempts.store(true);
                }

                set_connection_state(ConnectionState::FAILED);
                return;
            }
        } else if (new_state == ConnectionState::CONNECTED) {
            reconnect_attempts_ = 0; // Reset on successful connection
        }

        // Copy callback under lock to prevent race with destructor clearing it
        // We invoke OUTSIDE the lock so we don't hold mutex during LVGL operations
        std::function<void(ConnectionState, ConnectionState)> callback_copy;
        {
            std::lock_guard<std::mutex> lock(state_callback_mutex_);
            if (state_change_callback_ && !is_destroying_.load()) {
                callback_copy = state_change_callback_;
            }
        }

        // Double-check is_destroying_ AFTER releasing lock but BEFORE invoking callback
        // This catches the race where destructor set the flag between our copy and invocation
        if (callback_copy && !is_destroying_.load()) {
            try {
                callback_copy(old_state, new_state);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("[Moonraker Client] State change callback threw exception: {}",
                                   e.what());
            } catch (...) {
                LOG_ERROR_INTERNAL(
                    "[Moonraker Client] State change callback threw unknown exception");
            }
        }
    }
}

void MoonrakerClient::disconnect() {
    // Serialize with connect() — both modify callbacks and call close()
    std::lock_guard<std::recursive_mutex> connect_lock(connect_mutex_);

    ConnectionState current_state = connection_state_.load();

    // Only log if we're actually connected/connecting
    if (current_state != ConnectionState::DISCONNECTED &&
        current_state != ConnectionState::FAILED) {
        spdlog::debug("[Moonraker Client] Disconnecting from WebSocket server");
    }

    // Disable auto-reconnect BEFORE invalidation to prevent spurious reconnection
    setReconnect(nullptr);

    // Invalidate lifetime guard so any in-flight or future callbacks on the event loop
    // thread will see weak_guard.lock() fail and early-return. Then create a fresh guard
    // for the next connect() call.
    // NOTE: Do NOT replace onopen/onmessage/onclose with no-op lambdas here — that's a
    // data race with the event loop thread which may be mid-call on the std::function.
    // The invalidated weak_ptr is the safe cancellation mechanism.
    lifetime_guard_.reset();
    lifetime_guard_ = std::make_shared<bool>(true);

    // Wait for any in-flight callbacks to finish before we modify shared state.
    // Callbacks hold a shared lock; acquiring exclusive blocks until they complete.
    {
        std::unique_lock<std::shared_mutex> lk(callback_lifecycle_mutex_);
    }

    // Now safe to stop timer and close — no callbacks can restart the timer or
    // access our state because the lifetime guard is invalidated.
    stop_health_timer();
    close();

    // Clean up any pending requests (invokes error callbacks)
    tracker_.cleanup_all();

    // Reset discovery state for next connection
    discovery_.reset_identified();
    discovery_.reset_completion();

    // Reset connection state
    set_connection_state(ConnectionState::DISCONNECTED);
    reconnect_attempts_ = 0;
}

void MoonrakerClient::force_reconnect() {
    spdlog::info("[Moonraker Client] Force reconnect requested - full state reset");

    // Copy stored connection info under lock
    std::string url;
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void()> on_discovery_complete;
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        url = last_url_;
        on_connected = last_on_connected_;
        on_disconnected = last_on_disconnected_;
        on_discovery_complete = last_discovery_complete_;
    }

    // Verify we have stored connection info
    if (url.empty()) {
        spdlog::warn(
            "[Moonraker Client] force_reconnect() called but no previous connection info - "
            "call connect() first");
        return;
    }

    // 1. Disconnect cleanly (clears pending requests, resets state)
    disconnect();

    // 2. Connect using stored URL and callbacks
    int result = connect(url.c_str(), on_connected, on_disconnected);
    if (result != 0) {
        spdlog::error("[Moonraker Client] force_reconnect() connect failed: {}", result);
        return;
    }

    // 3. Re-run discovery if we have a stored callback
    //    Note: discover_printer() is typically called in on_connected callback,
    //    so it will be triggered automatically. But if the caller wants explicit
    //    discovery, we provide the mechanism.
    spdlog::debug("[Moonraker Client] force_reconnect() complete - connection initiated");
}

int MoonrakerClient::connect(const char* url, std::function<void()> on_connected,
                             std::function<void()> on_disconnected) {
    // Serialize concurrent connect() calls — the close/callback-setup/open sequence
    // is not atomic and concurrent callers race on libhv internal state.
    std::lock_guard<std::recursive_mutex> connect_lock(connect_mutex_);

    // Reset WebSocket state from previous connection attempt BEFORE setting new callbacks.
    // This prevents libhv from rejecting the new open() call if we're already connecting/connected.
    // Note: close() is safe to call even if already closed (idempotent).
    close();

    // Apply connection timeout to libhv (must be called before open())
    setConnectTimeout(static_cast<int>(connection_timeout_ms_));

    spdlog::debug("[Moonraker Client] WebSocket connecting to {}", url);
    set_connection_state(ConnectionState::CONNECTING);
    connection_generation_.fetch_add(1);

    // Connection opened callback
    // Wrap entire callback body in try-catch to prevent any exception from escaping to libhv
    // Capture weak_ptr to lifetime_guard_ to safely detect destruction from event loop thread.
    // url is captured BY STRING COPY — capturing the const char* directly would dangle once the
    // caller's stack-local URL string (e.g. ws_url in WizardConnectionStep::handle_test_connection_clicked)
    // is destroyed. Surfaced by ASAN as heap-use-after-free in spdlog format on the libhv worker thread.
    onopen = [this, weak_guard = std::weak_ptr<bool>(lifetime_guard_), on_connected,
              url_owned = std::string(url ? url : "")]() {
        try {
            // Acquire shared lock to prevent destructor from proceeding while we execute.
            // try_to_lock ensures we never block if destructor holds the exclusive lock.
            std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
            if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire)) {
                return; // Destructor is running or waiting, abort callback
            }

            // Check lifetime guard (defense-in-depth)
            auto guard = weak_guard.lock();
            if (!guard) {
                return; // Client is being destroyed, abort callback
            }

            // Note: getHttpResponse() available here if needed for upgrade response inspection
            spdlog::debug("[Moonraker Client] WebSocket connected to {}", url_owned);

            // Check if this is a reconnection (was_connected_ is true from previous session)
            // Emit RECONNECTED event BEFORE updating was_connected_
            if (was_connected_.load()) {
                emit_event(MoonrakerEventType::RECONNECTED, "Connection restored", false);
            }

            was_connected_ = true;
            set_connection_state(ConnectionState::CONNECTED);

            // Start periodic health checks (timeout detection, reconnect staleness)
            start_health_timer();

            // Reset notification flags on successful connection
            reset_notification_flags();

            invoke_connected_callback(on_connected, "WebSocket opened");
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onopen callback threw unexpected exception: {}",
                               e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onopen callback threw unknown exception");
        }
    };

    // Message received callback
    // Wrap entire callback body in try-catch to prevent any exception from escaping to libhv
    // Capture weak_ptr to lifetime_guard_ to safely detect destruction from event loop thread
    onmessage = [this, weak_guard = std::weak_ptr<bool>(lifetime_guard_), on_connected,
                 on_disconnected](const std::string& msg) {
        // DEBUG: Log every raw message received to diagnose AD5M WebSocket issue
        spdlog::trace("[Moonraker Client] onmessage received {} bytes", msg.size());

        try {
            // Acquire shared lock to prevent destructor from proceeding while we execute.
            // try_to_lock ensures we never block if destructor holds the exclusive lock.
            std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
            if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire)) {
                return; // Destructor is running or waiting, abort callback
            }

            // Check lifetime guard (defense-in-depth)
            auto guard = weak_guard.lock();
            if (!guard) {
                return; // Client is being destroyed, abort callback
            }

            // Validate message size to prevent memory exhaustion
            static constexpr size_t MAX_MESSAGE_SIZE = 5 * 1024 * 1024; // 5 MB
            if (msg.size() > MAX_MESSAGE_SIZE) {
                spdlog::error("[Moonraker Client] Message too large: {} bytes (max: {})",
                              msg.size(), MAX_MESSAGE_SIZE);

                // Emit event - this indicates a protocol problem
                emit_event(MoonrakerEventType::MESSAGE_OVERSIZED,
                           fmt::format("Received oversized data from printer ({} bytes). "
                                       "This may indicate a communication error.",
                                       msg.size()),
                           true);

                disconnect();
                return;
            }

            // Check for timed out requests on each message (opportunistic cleanup)
            process_timeouts();

            // DEBUG: Log large messages to help diagnose history issue
            if (msg.size() > 50000) {
                spdlog::debug("[Moonraker Client] Received large message: {} bytes", msg.size());
            }

            // Parse JSON message
            json j;
            try {
                j = json::parse(msg);
            } catch (const json::parse_error& e) {
                LOG_ERROR_INTERNAL("[Moonraker Client] JSON parse error: {}", e.what());
                TelemetryManager::instance().record_error("websocket", "parse_error",
                                                          fmt::format("JSON parse: {}", e.what()));
                return;
            }

            // Route responses with request IDs through the tracker
            if (j.contains("id")) {
                tracker_.route_response(
                    j,
                    [this](MoonrakerEventType type, const std::string& msg_str, bool is_error,
                           const std::string& details) {
                        emit_event(type, msg_str, is_error, details);
                    },
                    []() { return AbortManager::instance().is_handling_shutdown(); });
            }

            // Handle notifications (no request ID)
            if (j.contains("method")) {
                // Validate 'method' field type
                if (!j["method"].is_string()) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Invalid 'method' type in notification: {}",
                        j["method"].type_name());
                    return;
                }

                std::string method = j["method"].get<std::string>();

                // Copy callbacks to invoke (to avoid holding lock during callback execution)
                std::vector<std::function<void(const json&)>> callbacks_to_invoke;

                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex_);

                    // Printer status updates (most common)
                    if (method == "notify_status_update" || method == "notify_filelist_changed") {
                        // Copy all notify callbacks from map
                        callbacks_to_invoke.reserve(notify_callbacks_.size());
                        for (const auto& [id, cb] : notify_callbacks_) {
                            callbacks_to_invoke.push_back(cb);
                        }
                    }

                    // Method-specific persistent callbacks
                    auto method_it = method_callbacks_.find(method);
                    if (method_it != method_callbacks_.end()) {
                        for (auto& [handler_name, cb] : method_it->second) {
                            callbacks_to_invoke.push_back(cb);
                        }
                    }
                } // Release lock

                // Parse bed mesh updates before invoking user callbacks
                if (method == "notify_status_update" && j.contains("params") &&
                    j["params"].is_array() && !j["params"].empty()) {
                    const json& params = j["params"][0];
                    if (params.contains("bed_mesh") && params["bed_mesh"].is_object()) {
                        parse_bed_mesh(params["bed_mesh"]);
                    }
                }

                // Invoke callbacks outside lock to prevent deadlock
                for (auto& cb : callbacks_to_invoke) {
                    // Defense-in-depth: a moved-from or empty std::function in the
                    // registration map would SIGSEGV on invocation (#765 class).
                    if (!cb)
                        continue;
                    try {
                        cb(j);
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL("[Moonraker Client] Callback for {} threw exception: {}",
                                           method, e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Callback for {} threw unknown exception", method);
                    }
                }

                // Klippy disconnected from Moonraker
                if (method == "notify_klippy_disconnected") {
                    spdlog::warn("[Moonraker Client] Klipper disconnected from Moonraker");

                    // Update klippy state in PrinterState (SHUTDOWN = firmware disconnected)
                    get_printer_state().set_klippy_state(KlippyState::SHUTDOWN);

                    // Clear pending requests — Klippy can't process them anymore
                    tracker_.cleanup_all();

                    // Emit event for UI layer to handle
                    emit_event(MoonrakerEventType::KLIPPY_DISCONNECTED,
                               "Klipper has disconnected from Moonraker. Check for errors in your "
                               "printer interface.",
                               true);

                    // Invoke user callback with exception safety
                    if (on_disconnected) {
                        try {
                            on_disconnected();
                        } catch (const std::exception& e) {
                            LOG_ERROR_INTERNAL(
                                "[Moonraker Client] Disconnection callback threw exception: {}",
                                e.what());
                        } catch (...) {
                            LOG_ERROR_INTERNAL("[Moonraker Client] Disconnection callback threw "
                                               "unknown exception");
                        }
                    }
                }
                // Klippy entered shutdown state (M112, thermal runaway, config error)
                // Distinct from disconnect — Klipper is still running but in shutdown mode
                else if (method == "notify_klippy_shutdown") {
                    spdlog::warn("[Moonraker Client] Klipper entered shutdown state");

                    helix::ui::queue_update("MoonrakerClient::notify_klippy_shutdown", []() {
                        get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);
                    });

                    // Emit event for UI layer — recovery dialog will show
                    emit_event(MoonrakerEventType::KLIPPY_SHUTDOWN,
                               "Klipper has entered shutdown state.", true);

                    // Shutdown is a valid gate state for discovery. If we never
                    // completed one on this connection (Klippy was unreachable
                    // at WS-connect time), retry now (#802).
                    if (!discovery_.is_completed()) {
                        spdlog::info("[Moonraker Client] Retrying discovery after Klippy shutdown");
                        invoke_connected_callback(on_connected, "Klippy shutdown");
                    }
                }
                // Klippy reconnected to Moonraker
                else if (method == "notify_klippy_ready") {
                    spdlog::info("[Moonraker Client] Klipper ready");

                    helix::ui::queue_update("MoonrakerClient::notify_klippy_ready", []() {
                        get_printer_state().set_klippy_state_sync(KlippyState::READY);
                    });

                    // Emit event for UI layer to show success toast
                    emit_event(MoonrakerEventType::KLIPPY_READY, "Klipper ready", false);

                    // Unconditional retrigger (unlike notify_klippy_shutdown which
                    // checks !discovery_.is_completed()): a transition INTO ready
                    // may follow a FIRMWARE_RESTART after config edits where the
                    // hardware shape changed, so we always rediscover.
                    invoke_connected_callback(on_connected, "Klippy ready");
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] onmessage callback threw unexpected exception: {}", e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onmessage callback threw unknown exception");
        }
    };

    // Connection closed callback
    // Wrap entire callback body in try-catch to prevent any exception from escaping
    // to libhv (which may not handle exceptions properly or may be noexcept)
    // Capture weak_ptr to lifetime_guard_ to safely detect destruction from event loop thread
    onclose = [this, weak_guard = std::weak_ptr<bool>(lifetime_guard_), on_disconnected]() {
        try {
            spdlog::debug("[Moonraker Client] onclose callback invoked");

            // Acquire shared lock to prevent destructor from proceeding while we execute.
            // try_to_lock ensures we never block if destructor holds the exclusive lock.
            std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
            if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire)) {
                spdlog::debug(
                    "[Moonraker Client] onclose callback early return due to destruction");
                return; // Destructor is running or waiting, abort callback
            }

            // Check lifetime guard (defense-in-depth)
            auto guard = weak_guard.lock();
            if (!guard) {
                spdlog::debug(
                    "[Moonraker Client] onclose callback early return - client destroyed");
                return; // Client is being destroyed, abort callback
            }

            ConnectionState current = connection_state_.load();

            // Cleanup all pending requests (invoke error callbacks)
            tracker_.cleanup_all();

            if (was_connected_) {
                spdlog::warn("[Moonraker Client] WebSocket connection closed");
                TelemetryManager::instance().record_error("websocket", "disconnected",
                                                          "connection closed unexpectedly");
                was_connected_ = false;
                // Reset so re-identification + retry-on-klippy-state happen on reconnect
                discovery_.reset_identified();
                discovery_.reset_completion();

                // Emit event with rate limiting to prevent spam during reconnect loop
                if (!g_already_notified_disconnect.load()) {
                    emit_event(MoonrakerEventType::CONNECTION_LOST,
                               "Connection to printer lost - attempting to reconnect...", false);
                    g_already_notified_disconnect.store(true);
                }

                // Check if this is a reconnection scenario
                if (current != ConnectionState::FAILED) {
                    set_connection_state(ConnectionState::RECONNECTING);
                }

                // Invoke user callback with exception safety
                if (on_disconnected) {
                    try {
                        on_disconnected();
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw exception: {}",
                            e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw unknown exception");
                    }
                }
            } else {
                spdlog::debug(
                    "[Moonraker Client] WebSocket connection failed (printer not available)");

                // Initial connection failed
                if (current == ConnectionState::CONNECTING) {
                    set_connection_state(ConnectionState::DISCONNECTED);
                }

                // Call on_disconnected() to notify about connection failure
                // Callers can use their own state tracking (e.g. connection_testing flag)
                // to distinguish initial connection failures from reconnection scenarios
                if (on_disconnected) {
                    try {
                        on_disconnected();
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw exception: {}",
                            e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw unknown exception");
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onclose callback threw unexpected exception: {}",
                               e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onclose callback threw unknown exception");
        }
    };

    // WebSocket ping (keepalive) - use configured interval
    setPingInterval(static_cast<int>(keepalive_interval_ms_));

    // Automatic reconnection with exponential backoff - use configured values
    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    reconn.min_delay = reconnect_min_delay_ms_;
    reconn.max_delay = reconnect_max_delay_ms_;
    reconn.delay_policy = 2; // Exponential backoff
    setReconnect(&reconn);

    // Store connection info for force_reconnect()
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        last_url_ = url;
        last_on_connected_ = on_connected;
        last_on_disconnected_ = on_disconnected;
    }

    // Connect
    http_headers headers;
    headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
    return open(url, headers);
}

SubscriptionId MoonrakerClient::register_notify_update(std::function<void(const json&)> cb) {
    if (!cb) {
        spdlog::warn("[Moonraker Client] register_notify_update called with null callback");
        return INVALID_SUBSCRIPTION_ID;
    }

    SubscriptionId id = next_subscription_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        notify_callbacks_.emplace(id, cb);
    }
    spdlog::trace("[Moonraker Client] Registered notify callback with ID {}", id);
    return id;
}

bool MoonrakerClient::unsubscribe_notify_update(SubscriptionId id) {
    if (id == INVALID_SUBSCRIPTION_ID) {
        return false;
    }

    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = notify_callbacks_.find(id);
    if (it != notify_callbacks_.end()) {
        notify_callbacks_.erase(it);
        spdlog::debug("[Moonraker Client] Unsubscribed notify callback ID {}", id);
        return true;
    }
    spdlog::debug("[Moonraker Client] Unsubscribe failed: notify callback ID {} not found", id);
    return false;
}

void MoonrakerClient::register_event_handler(MoonrakerEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_handler_mutex_);
    event_handler_ = std::move(cb);
    spdlog::debug("[Moonraker Client] Event handler {}",
                  event_handler_ ? "registered" : "unregistered");
}

void MoonrakerClient::suppress_disconnect_modal(uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(suppress_mutex_);
    suppress_disconnect_modal_until_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    spdlog::info("[Moonraker Client] Suppressing disconnect modal for {}ms", duration_ms);
}

bool MoonrakerClient::is_disconnect_modal_suppressed() const {
    std::lock_guard<std::mutex> lock(suppress_mutex_);
    return std::chrono::steady_clock::now() < suppress_disconnect_modal_until_;
}

void MoonrakerClient::emit_event(MoonrakerEventType type, const std::string& message, bool is_error,
                                 const std::string& details) {
    MoonrakerEventCallback handler;
    {
        std::lock_guard<std::mutex> lock(event_handler_mutex_);
        handler = event_handler_;
    }

    if (handler) {
        MoonrakerEvent evt{type, message, details, is_error};
        try {
            handler(evt);
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] Event handler threw exception: {}", e.what());
        }
    } else {
        // No handler registered - just log the event
        if (is_error) {
            spdlog::error("[Moonraker Event] {}: {}", static_cast<int>(type), message);
        } else {
            spdlog::warn("[Moonraker Event] {}: {}", static_cast<int>(type), message);
        }
    }
}

void MoonrakerClient::dispatch_status_update(const json& status) {
    // Parse bed mesh data before dispatching (mirrors WebSocket handler behavior)
    // This ensures bed mesh is populated on initial subscription response,
    // not just on subsequent notify_status_update messages
    if (status.contains("bed_mesh") && status["bed_mesh"].is_object()) {
        parse_bed_mesh(status["bed_mesh"]);
        // NOTE: Do NOT set build_volume from mesh bounds here!
        // Mesh bounds represent the probe area, not bed dimensions.
        // Actual bed dimensions come from stepper config in moonraker_api_motion.cpp.
    }

    // Extract kinematics type from toolhead data (for printer detection)
    if (status.contains("toolhead") && status["toolhead"].is_object()) {
        const json& toolhead = status["toolhead"];
        if (toolhead.contains("kinematics") && toolhead["kinematics"].is_string()) {
            auto kinematics = toolhead["kinematics"].get<std::string>();
            discovery_.modify_hardware(
                [&](PrinterDiscovery& hw) { hw.set_kinematics(kinematics); });
            spdlog::debug("[Moonraker Client] Kinematics type: {}", kinematics);
        }
    }

    // Wrap raw status into notify_status_update format
    json notification = {
        {"method", "notify_status_update"},
        {"params", json::array({status, 0.0})} // [status, eventtime]
    };

    // Dispatch to all registered callbacks
    // Two-phase: copy under lock, invoke outside to avoid deadlock
    std::vector<std::function<void(const json&)>> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_copy.reserve(notify_callbacks_.size());
        for (const auto& [id, cb] : notify_callbacks_) {
            callbacks_copy.push_back(cb);
        }
    }

    // Wrap each callback in try/catch — this path delivers the initial subscription
    // snapshot synchronously on the main thread (Application::on_discovery_complete →
    // dispatch_status_update). An unhandled exception here unwinds straight through
    // run_main_loop into main()'s top-level catch, exiting 134 and triggering a crash
    // loop the watchdog can't break out of (#filament_motion_sensor null fields,
    // f75b961d8). The onmessage path already wraps each callback (line 533); this
    // mirrors that contract for the initial-state path.
    for (const auto& cb : callbacks_copy) {
        if (!cb)
            continue;
        try {
            cb(notification);
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] dispatch_status_update callback threw exception: {}", e.what());
            TelemetryManager::instance().record_error(
                "websocket", "status_dispatch_exception", e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] dispatch_status_update callback threw unknown exception");
        }
    }

    spdlog::trace(
        "[Moonraker Client] Dispatched status update to {} callbacks (has print_stats: {})",
        callbacks_copy.size(), status.contains("print_stats"));
}

void MoonrakerClient::register_method_callback(const std::string& method,
                                               const std::string& handler_name,
                                               std::function<void(const json&)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = method_callbacks_.find(method);
    if (it == method_callbacks_.end()) {
        spdlog::debug("[Moonraker Client] Registering new method callback: {} (handler: {})",
                      method, handler_name);
        std::map<std::string, std::function<void(const json&)>> handlers;
        handlers.insert({handler_name, cb});
        method_callbacks_.insert({method, handlers});
    } else {
        spdlog::debug("[Moonraker Client] Adding handler to existing method {}: {}", method,
                      handler_name);
        it->second.insert({handler_name, cb});
    }
}

bool MoonrakerClient::unregister_method_callback(const std::string& method,
                                                 const std::string& handler_name) {
    // During destruction, method_callbacks_ may already be cleared or mid-destruction.
    // Skip the erase to avoid use-after-free on the map's internal tree.
    if (is_destroying_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto method_it = method_callbacks_.find(method);
    if (method_it == method_callbacks_.end()) {
        spdlog::debug("[Moonraker Client] Unregister failed: method '{}' not found", method);
        return false;
    }

    auto handler_it = method_it->second.find(handler_name);
    if (handler_it == method_it->second.end()) {
        spdlog::debug(
            "[Moonraker Client] Unregister failed: handler '{}' not found for method '{}'",
            handler_name, method);
        return false;
    }

    method_it->second.erase(handler_it);
    spdlog::debug("[Moonraker Client] Unregistered handler '{}' from method '{}'", handler_name,
                  method);

    // Clean up empty method entries to avoid memory leaks
    if (method_it->second.empty()) {
        method_callbacks_.erase(method_it);
        spdlog::debug("[Moonraker Client] Removed empty method entry for '{}'", method);
    }

    return true;
}

// libhv's WebSocketClient::send only checks `channel != NULL`, NOT the WS
// protocol state. Sending while the channel exists but state ∈ {CONNECTING,
// WS_UPGRADING, WS_CLOSED} writes WS frame bytes into the wrong phase of the
// underlying TCP stream — Moonraker silently drops the malformed bytes and the
// request sits pending until 60 s timeout (or never, if the panel cleared its
// in-flight flag and stopped expecting a response). Surfaced as #909 (K2 Plus
// startup race: get_directory issued before onopen → request lost, panel stuck
// in refresh_in_flight_ for hours, no timeout warning fired because the timer
// was scheduled but the request wasn't where the timer expected).
//
// Fail fast unless our connection_state_ is CONNECTED — that flag flips ONLY
// in the onopen callback after WS_OPENED, and back out on disconnect/close.
bool MoonrakerClient::ready_to_send(const char* method) const {
    auto state = connection_state_.load(std::memory_order_acquire);
    if (state == ConnectionState::CONNECTED) {
        return true;
    }
    spdlog::debug("[Moonraker Client] Refusing send for '{}' — connection_state={}",
                  method ? method : "?", static_cast<int>(state));
    return false;
}

int MoonrakerClient::send_jsonrpc(const std::string& method) {
    if (!ready_to_send(method.c_str())) {
        return -1;
    }
    return tracker_.send_fire_and_forget(*this, method, json());
}

int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    if (!ready_to_send(method.c_str())) {
        return -1;
    }
    return tracker_.send_fire_and_forget(*this, method, params);
}

RequestId MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                        std::function<void(const json&)> cb) {
    // Forward to new overload with null error callback
    return send_jsonrpc(method, params, cb, nullptr, 0);
}

RequestId MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                        std::function<void(const json&)> success_cb,
                                        std::function<void(const MoonrakerError&)> error_cb,
                                        uint32_t timeout_ms, bool silent) {
    if (!ready_to_send(method.c_str())) {
        // Invoke error callback synchronously so callers (panels with
        // in_flight flags, etc.) see immediate failure instead of a stuck
        // request that never times out (#909).
        if (error_cb) {
            try {
                error_cb(MoonrakerError::connection_lost(method));
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker Client] Pre-send error cb for '{}' threw: {}", method,
                              e.what());
            }
        }
        return INVALID_REQUEST_ID;
    }
    return tracker_.send(*this, method, params, success_cb, error_cb, timeout_ms, silent);
}

int MoonrakerClient::gcode_script(const std::string& gcode) {
    std::string annotated = annotate_gcode(gcode);
    json params = {{"script", annotated}};
    int result = send_jsonrpc("printer.gcode.script", params);
    // send() returns bytes sent (positive) on success, negative on error.
    // Normalize to match API contract: 0 = success, negative = error.
    return result < 0 ? result : 0;
}

void MoonrakerClient::get_gcode_store(
    int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
    std::function<void(const MoonrakerError&)> on_error) {
    json params = {{"count", count}};

    send_jsonrpc(
        "server.gcode_store", params,
        [on_success](json response) {
            std::vector<GcodeStoreEntry> entries;

            // Parse response: {"result": {"gcode_store": [...]}}
            if (response.contains("result") && response["result"].contains("gcode_store")) {
                const auto& store = response["result"]["gcode_store"];
                entries.reserve(store.size());

                for (const auto& item : store) {
                    GcodeStoreEntry entry;
                    entry.message = item.value("message", "");
                    entry.time = item.value("time", 0.0);
                    entry.type = item.value("type", "response");
                    entries.push_back(entry);
                }
            }

            if (on_success) {
                on_success(entries);
            }
        },
        on_error);
}

void MoonrakerClient::invoke_connected_callback(const std::function<void()>& cb,
                                                const char* cause) {
    if (!cb) {
        return;
    }
    try {
        cb();
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("[Moonraker Client] {} callback threw exception: {}", cause, e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("[Moonraker Client] {} callback threw unknown exception", cause);
    }
}

void MoonrakerClient::discover_printer(std::function<void()> on_complete,
                                       std::function<void(const std::string& reason)> on_error) {
    // Store callback for force_reconnect()
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        last_discovery_complete_ = on_complete;
    }

    discovery_.start(on_complete, on_error);
}

// Discovery methods (continue_discovery, complete_discovery_subscription, parse_objects,
// parse_bed_mesh) moved to MoonrakerDiscoverySequence
// Request tracking methods (check_request_timeouts, cleanup_pending_requests) moved to
// MoonrakerRequestTracker
// cancel_request() is now an inline delegation in the header

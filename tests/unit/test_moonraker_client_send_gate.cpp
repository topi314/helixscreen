// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_client_send_gate.cpp
 * @brief Regression tests for the connection_state_ send gate (#909).
 *
 * Context: libhv's WebSocketClient::send() only checks `channel != NULL`,
 * not the WS protocol state. A send issued while the client is CONNECTING /
 * RECONNECTING / DISCONNECTED writes WS frame bytes onto a stream that's not
 * in WS-frame phase — Moonraker silently drops the malformed bytes and the
 * request stalls indefinitely. MoonrakerClient::ready_to_send() guards
 * against this by refusing to send unless connection_state_ == CONNECTED;
 * the 5-arg send_jsonrpc invokes the error callback synchronously with
 * CONNECTION_LOST so callers (panels with in_flight flags, etc.) see
 * immediate failure instead of a stuck request.
 */

#include "../../include/moonraker_client.h"
#include "../../include/moonraker_error.h"
#include "hv/EventLoopThread.h"

#include "../catch_amalgamated.hpp"

#include <atomic>

using namespace helix;

namespace {

/// Minimal client wrapper — owns an event loop thread so the WS client base
/// is happy, but never connects. connection_state_ stays DISCONNECTED.
struct UnconnectedClient {
    UnconnectedClient() : loop_(std::make_shared<hv::EventLoopThread>()) {
        loop_->start();
        client_ = std::make_unique<MoonrakerClient>(loop_->loop());
    }
    ~UnconnectedClient() {
        client_.reset();
        loop_->stop();
        loop_->join();
    }

    std::shared_ptr<hv::EventLoopThread> loop_;
    std::unique_ptr<MoonrakerClient> client_;
};

} // namespace

TEST_CASE("send_jsonrpc 5-arg refuses send when not connected and fires error_cb",
          "[moonraker][client][regression][eventloop][slow]") {
    UnconnectedClient u;

    auto error_fired = std::make_shared<std::atomic<bool>>(false);
    auto error_method = std::make_shared<std::string>();
    auto error_type = std::make_shared<MoonrakerErrorType>(MoonrakerErrorType::UNKNOWN);

    auto id = u.client_->send_jsonrpc(
        "server.files.get_directory", json{{"path", "gcodes"}},
        [](const json&) { /* should not fire */ },
        [error_fired, error_method, error_type](const MoonrakerError& err) {
            error_fired->store(true);
            *error_method = err.method;
            *error_type = err.type;
        });

    REQUIRE(id == INVALID_REQUEST_ID);
    REQUIRE(error_fired->load() == true);
    REQUIRE(*error_method == "server.files.get_directory");
    REQUIRE(*error_type == MoonrakerErrorType::CONNECTION_LOST);
}

TEST_CASE("send_jsonrpc 1-arg returns -1 when not connected",
          "[moonraker][client][regression][eventloop][slow]") {
    UnconnectedClient u;
    int result = u.client_->send_jsonrpc("printer.info");
    REQUIRE(result < 0);
}

TEST_CASE("send_jsonrpc 2-arg returns -1 when not connected",
          "[moonraker][client][regression][eventloop][slow]") {
    UnconnectedClient u;
    int result = u.client_->send_jsonrpc("printer.objects.query", json{{"objects", {{"toolhead", nullptr}}}});
    REQUIRE(result < 0);
}

TEST_CASE("send_jsonrpc with null error_cb refuses cleanly when not connected",
          "[moonraker][client][regression][eventloop][slow]") {
    UnconnectedClient u;
    // 4-arg overload — no error callback at all. Must not crash.
    auto id = u.client_->send_jsonrpc(
        "printer.info", json(),
        [](const json&) { /* should not fire */ });
    REQUIRE(id == INVALID_REQUEST_ID);
}

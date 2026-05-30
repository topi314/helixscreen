// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_settings_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("output_device setting round-trips", "[sound][settings]") {
    auto& mgr = helix::AudioSettingsManager::instance();
    mgr.set_output_device("plughw:CARD=vc4hdmi1,DEV=0");
    CHECK(mgr.get_output_device() == "plughw:CARD=vc4hdmi1,DEV=0");
    mgr.set_output_device(""); // reset for other tests
    CHECK(mgr.get_output_device().empty());
}

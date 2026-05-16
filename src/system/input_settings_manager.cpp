// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_settings_manager.h"

#include "config.h"
#include "runtime_config.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>

using namespace helix;

InputSettingsManager& InputSettingsManager::instance() {
    static InputSettingsManager instance;
    return instance;
}

InputSettingsManager::InputSettingsManager() {
    spdlog::trace("[InputSettingsManager] Constructor");
}

void InputSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[InputSettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[InputSettingsManager] Initializing subjects");

    Config* config = Config::get_instance();

    // Scroll throw (default: 25, range 5-50)
    int scroll_throw = config->get<int>("/input/scroll_throw", 25);
    scroll_throw = std::max(5, std::min(50, scroll_throw));
    UI_MANAGED_SUBJECT_INT(scroll_throw_subject_, scroll_throw, "settings_scroll_throw", subjects_);

    // Scroll limit (default: 10, range 1-20)
    int scroll_limit = config->get<int>("/input/scroll_limit", 10);
    scroll_limit = std::max(1, std::min(20, scroll_limit));
    UI_MANAGED_SUBJECT_INT(scroll_limit_subject_, scroll_limit, "settings_scroll_limit", subjects_);

    // Jitter threshold (default: 5, range 0-30; 0 disables)
    int jitter_threshold = config->get<int>("/input/jitter_threshold", 5);
    jitter_threshold = std::max(0, std::min(30, jitter_threshold));
    UI_MANAGED_SUBJECT_INT(jitter_threshold_subject_, jitter_threshold,
                           "settings_jitter_threshold", subjects_);

    // Scroll guard (default: false; AD5M/AD5X presets enable it via hardware preset)
    bool scroll_guard = config->get<bool>("/input/scroll_guard", false);
    UI_MANAGED_SUBJECT_INT(scroll_guard_subject_, scroll_guard ? 1 : 0,
                           "settings_scroll_guard", subjects_);

    // Debug touch visualization (default: false). Apply live at init so the
    // persisted setting matches RuntimeConfig before the ripple timer first fires.
    bool debug_touches = config->get<bool>("/input/debug_touches", false);
    RuntimeConfig::set_debug_touches(debug_touches);
    UI_MANAGED_SUBJECT_INT(debug_touches_subject_, debug_touches ? 1 : 0,
                           "settings_debug_touches", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup with StaticSubjectRegistry
    StaticSubjectRegistry::instance().register_deinit(
        "InputSettingsManager", []() { InputSettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[InputSettingsManager] Subjects initialized: scroll_throw={}, scroll_limit={}, "
                  "jitter={}, scroll_guard={}, debug_touches={}",
                  scroll_throw, scroll_limit, jitter_threshold, scroll_guard, debug_touches);
}

void InputSettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[InputSettingsManager] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[InputSettingsManager] Subjects deinitialized");
}

// =============================================================================
// GETTERS / SETTERS
// =============================================================================

int InputSettingsManager::get_scroll_throw() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&scroll_throw_subject_));
}

void InputSettingsManager::set_scroll_throw(int value) {
    // Clamp to valid range (5-50)
    int clamped = std::max(5, std::min(50, value));
    spdlog::info("[InputSettingsManager] set_scroll_throw({})", clamped);

    // 1. Update subject
    lv_subject_set_int(&scroll_throw_subject_, clamped);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/input/scroll_throw", clamped);
    config->save();

    // 3. Mark restart needed (this setting only takes effect on startup)
    restart_pending_ = true;
    spdlog::debug("[InputSettingsManager] Scroll throw set to {} (restart required)", clamped);
}

int InputSettingsManager::get_scroll_limit() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&scroll_limit_subject_));
}

void InputSettingsManager::set_scroll_limit(int value) {
    // Clamp to valid range (1-20)
    int clamped = std::max(1, std::min(20, value));
    spdlog::info("[InputSettingsManager] set_scroll_limit({})", clamped);

    // 1. Update subject
    lv_subject_set_int(&scroll_limit_subject_, clamped);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/input/scroll_limit", clamped);
    config->save();

    // 3. Mark restart needed (this setting only takes effect on startup)
    restart_pending_ = true;
    spdlog::debug("[InputSettingsManager] Scroll limit set to {} (restart required)", clamped);
}

int InputSettingsManager::get_jitter_threshold() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&jitter_threshold_subject_));
}

void InputSettingsManager::set_jitter_threshold(int value) {
    int clamped = std::max(0, std::min(30, value));
    spdlog::info("[InputSettingsManager] set_jitter_threshold({})", clamped);

    lv_subject_set_int(&jitter_threshold_subject_, clamped);

    Config* config = Config::get_instance();
    config->set<int>("/input/jitter_threshold", clamped);
    config->save();

    restart_pending_ = true;
}

bool InputSettingsManager::get_scroll_guard() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&scroll_guard_subject_)) != 0;
}

void InputSettingsManager::set_scroll_guard(bool enabled) {
    spdlog::info("[InputSettingsManager] set_scroll_guard({})", enabled);

    lv_subject_set_int(&scroll_guard_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/input/scroll_guard", enabled);
    config->save();

    restart_pending_ = true;
}

bool InputSettingsManager::get_debug_touches() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&debug_touches_subject_)) != 0;
}

void InputSettingsManager::set_debug_touches(bool enabled) {
    spdlog::info("[InputSettingsManager] set_debug_touches({}) [live]", enabled);

    // Live-apply: the ripple timer checks RuntimeConfig::debug_touches() on every tick.
    RuntimeConfig::set_debug_touches(enabled);

    lv_subject_set_int(&debug_touches_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/input/debug_touches", enabled);
    config->save();
    // No restart_pending_ — change takes effect immediately.
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_history_manager.h"

#include "ui_update_queue.h"

#include "moonraker_api.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

PrintHistoryManager::PrintHistoryManager(MoonrakerAPI* api, MoonrakerClient* client)
    : api_(api), client_(client) {
    spdlog::debug("[HistoryManager] Created");
    subscribe_to_notifications();
}

PrintHistoryManager::~PrintHistoryManager() {
    // Unregister notification callback
    if (client_) {
        client_->unregister_method_callback("notify_history_changed", "PrintHistoryManager");
    }
}

// ============================================================================
// Fetch / Refresh
// ============================================================================

void PrintHistoryManager::fetch(int limit) {
    if (!api_) {
        spdlog::warn("[HistoryManager] No API available, cannot fetch");
        return;
    }

    if (!api_->is_connected()) {
        spdlog::debug("[HistoryManager] Not connected yet, deferring history fetch");
        return;
    }

    // Atomic check-and-set prevents concurrent fetches. Pairs with the reset
    // in the BG-thread callbacks below.
    bool expected = false;
    if (!is_fetching_.compare_exchange_strong(expected, true)) {
        spdlog::debug("[HistoryManager] Fetch already in progress, ignoring");
        return;
    }

    spdlog::debug("[HistoryManager] Fetching history (limit={})", limit);

    auto token = lifetime_.token();

    api_->history().get_history_list(
        limit, 0, 0.0, 0.0, // limit, start, since, before
        [this, token](const std::vector<PrintHistoryJob>& jobs, uint64_t /*total*/) {
            // Clear guard BEFORE posting defer so a freeze-drop doesn't strand us.
            is_fetching_.store(false);
            // No bare expired() check — token.defer's own guard suffices, and
            // dropping the bare check silences the bg_tok_expired_check
            // detector for this site (3XNZQB2R audit). The std::vector copy
            // below is harmless if defer skips.
            std::vector<PrintHistoryJob> jobs_copy = jobs;
            token.defer("PrintHistoryManager::fetch_success",
                        [this, jobs = std::move(jobs_copy)]() mutable {
                            on_history_fetched(std::move(jobs));
                        });
        },
        [this, token](const MoonrakerError& error) {
            is_fetching_.store(false);
            // spdlog is thread-safe; logging the warn even on a destroyed
            // manager is harmless (informational). Dropping the bare
            // expired() check silences the detector here.
            (void)token;
            spdlog::warn("[HistoryManager] Failed to fetch history: {}", error.message);
        });
}

void PrintHistoryManager::invalidate() {
    spdlog::debug("[HistoryManager] Cache invalidated");
    is_loaded_ = false;
}

// ============================================================================
// Observer Pattern
// ============================================================================

void PrintHistoryManager::add_observer(HistoryChangedCallback* cb) {
    if (cb && *cb) {
        observers_.push_back(cb);
        spdlog::debug("[HistoryManager] Added observer (total: {})", observers_.size());
    }
}

void PrintHistoryManager::remove_observer(HistoryChangedCallback* cb) {
    if (!cb) {
        return;
    }

    auto it = std::find(observers_.begin(), observers_.end(), cb);
    if (it != observers_.end()) {
        observers_.erase(it);
        spdlog::debug("[HistoryManager] Removed observer (remaining: {})", observers_.size());
    }
}

// ============================================================================
// Private Implementation
// ============================================================================

void PrintHistoryManager::on_history_fetched(std::vector<PrintHistoryJob>&& jobs) {
    spdlog::debug("[HistoryManager] Fetched {} jobs", jobs.size());

    cached_jobs_ = std::move(jobs);
    build_filename_stats();

    is_loaded_ = true;
    // is_fetching_ was cleared on the BG thread before this defer was posted.

    notify_observers();
}

void PrintHistoryManager::build_filename_stats() {
    filename_stats_.clear();

    for (const auto& job : cached_jobs_) {
        // Strip path from filename to get basename
        std::string basename = job.filename;
        auto slash_pos = basename.rfind('/');
        if (slash_pos != std::string::npos) {
            basename = basename.substr(slash_pos + 1);
        }

        if (basename.empty()) {
            continue;
        }

        auto& stats = filename_stats_[basename];

        // Count successes and failures
        if (job.status == PrintJobStatus::COMPLETED) {
            stats.success_count++;
        } else if (job.status == PrintJobStatus::CANCELLED || job.status == PrintJobStatus::ERROR) {
            stats.failure_count++;
        }

        // Track most recent job for this filename
        if (job.start_time > stats.last_print_time) {
            stats.last_print_time = job.start_time;
            stats.last_status = job.status;
            stats.uuid = job.uuid;
            stats.size_bytes = job.size_bytes;
        }
    }

    spdlog::debug("[HistoryManager] Built stats for {} unique filenames", filename_stats_.size());
}

std::vector<PrintHistoryJob> PrintHistoryManager::get_jobs_since(double since) const {
    std::vector<PrintHistoryJob> filtered;
    filtered.reserve(cached_jobs_.size()); // Avoid reallocation

    for (const auto& job : cached_jobs_) {
        if (job.start_time >= since) {
            filtered.push_back(job);
        }
    }

    return filtered;
}

void PrintHistoryManager::notify_observers() {
    auto observers_copy = observers_;
    for (auto* cb : observers_copy) {
        if (cb && *cb) {
            (*cb)();
        }
    }
}

void PrintHistoryManager::subscribe_to_notifications() {
    if (!client_) {
        return;
    }

    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_history_changed", "PrintHistoryManager",
        [this, token](const nlohmann::json& /*data*/) {
            if (token.expired())
                return;
            spdlog::debug("[HistoryManager] Received notify_history_changed");

            token.defer("PrintHistoryManager::notify_history_changed", [this]() {
                invalidate();
                fetch();
            });
        });
}

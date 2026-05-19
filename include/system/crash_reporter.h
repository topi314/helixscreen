// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file crash_reporter.h
 * @brief Standalone crash reporter — sends crash data to developer on next launch
 *
 * When HelixScreen crashes, crash_handler.cpp writes config/crash.txt with signal,
 * version, uptime, and backtrace. On next startup, CrashReporter detects this file,
 * collects additional context (platform, logs, hardware info), and offers the user
 * a dialog to send the report.
 *
 * Delivery priority:
 * 1. Auto-send via CF Worker at crash.helixscreen.org → GitHub issue
 * 2. QR code with pre-filled GitHub issue URL (if no network)
 * 3. File fallback to ~/helixscreen/crash_report.txt (always)
 *
 * Independent of TelemetryManager — works without telemetry opt-in.
 */

#include <string>
#include <vector>

#include "hv/json.hpp"

class CrashReporter {
  public:
    static CrashReporter& instance();

    /**
     * @brief Initialize crash reporter with config directory
     * @param config_dir Directory containing crash.txt (e.g., "config" or temp dir for tests)
     */
    void init(const std::string& config_dir);

    /**
     * @brief Reset state for clean re-initialization (used in tests)
     */
    void shutdown();

    /**
     * @brief Check if crash.txt exists from a previous crash
     */
    bool has_crash_report() const;

    /**
     * @brief Structured crash report with all collected context
     */
    struct CrashReport {
        // From crash.txt
        int signal = 0;
        std::string signal_name;
        std::string app_version;
        std::string timestamp;
        int uptime_sec = 0;
        std::vector<std::string> backtrace;

        // Exception message (for signal 0 / EXCEPTION crashes)
        std::string exception_what;

        // glibc abort reason captured via __abort_msg on SIGABRT — e.g.
        // "free(): invalid pointer", "double free or corruption", an assertion
        // string, etc. Empty for non-SIGABRT signals and on non-glibc libcs
        // where __abort_msg doesn't exist.
        std::string abort_msg;

        // Fault info (Phase 2 - from siginfo_t)
        std::string fault_addr;
        int fault_code = 0;
        std::string fault_code_name;

        // Register state (Phase 2 - from ucontext_t)
        std::string reg_pc;
        std::string reg_sp;
        std::string reg_lr; // ARM only
        std::string reg_bp; // x86_64 only

        // ASLR load base (for symbol resolution)
        std::string load_base;

        // UpdateQueue callback tag (identifies which queued callback was executing)
        std::string queue_callback;

        // LVGL event under dispatch at crash time (set by event_send_core hook).
        // event_target is a raw pointer hex string; event_code is an lv_event_code_t.
        // event_original_target differs from event_target only for bubbled events
        // (the originator's pointer); populated only when it differs from target.
        std::string event_target;
        std::string event_original_target;
        int event_code = 0;

        // Cached heap snapshot (refreshed every ~10s from main loop)
        struct HeapSnapshot {
            long age_ms = 0; // monotonic-ms timestamp when captured
            long rss_kb = 0;
            long vsz_kb = 0;
            long arena_kb = 0;    // glibc mallinfo total arena
            long used_kb = 0;     // glibc uordblks
            long free_kb = 0;     // glibc fordblks
            long mmap_kb = 0;     // glibc hblkhd
            long lv_total_kb = 0; // LVGL internal heap total
            int lv_used_pct = 0;
            int lv_frag_pct = 0;
            long lv_free_biggest_kb = 0;
            bool present = false; // true if any heap_* was parsed
        };
        HeapSnapshot heap;

        // Breadcrumbs from the in-process ring buffer.
        // Each entry: "<monotonic_ms> <category> <subject>" (space-separated)
        std::vector<std::string> breadcrumbs;

        // Memory map (/proc/self/maps lines, for mapping addresses to libraries)
        std::vector<std::string> memory_map;

        // Stack-scanned backtrace metadata
        std::string bt_source;  // "stack_scan" if backtrace includes scanned entries
        std::string text_start; // Text segment start address
        std::string text_end;   // Text segment end address

        // Stack dump (ARM32/MIPS: 128 raw stack words for return-address scanning)
        std::string stack_base;
        std::vector<std::string> stack_dump;

        // Extra registers (ARM32: r0-r12, fp, ip; MIPS: ra, etc.)
        // Key = register name (e.g., "r0", "fp"), value = hex string
        std::vector<std::pair<std::string, std::string>> extra_registers;

        // Additional context (collected at startup)
        std::string platform;
        std::string printer_model;
        std::string klipper_version;
        std::string log_tail;
        std::string display_info;
        int ram_total_mb = 0;
        int cpu_cores = 0;

        // Share code of a debug bundle uploaded alongside this report. Empty
        // when no bundle was attached (bundle upload failed, disabled, or the
        // report is being sent via the QR-code fallback path). The worker
        // renders this as a "Debug Bundle: CODE" row in the issue body so the
        // deeper context (sanitized settings, syslog, crash history) is one
        // click away.
        std::string debug_bundle_share_code;
    };

    /**
     * @brief Collect crash data from crash.txt + system context
     * @return Populated CrashReport struct
     */
    CrashReport collect_report();

    /**
     * @brief Attempt to send crash report to CF Worker
     * @return true if report was sent successfully
     */
    bool try_auto_send(const CrashReport& report);

    /**
     * @brief Check if this crash has already been reported (client-side dedup)
     *
     * Computes a fingerprint matching the server-side formula and checks
     * CrashHistory for a previous submission with the same fingerprint.
     */
    bool is_duplicate(const CrashReport& report) const;

    /**
     * @brief Compute the fingerprint for a crash report
     *
     * Format: signal_name/app_version/backtrace[0]
     * Matches the server-side crashFingerprint() in crash-worker.
     */
    static std::string fingerprint(const CrashReport& report);

    /**
     * @brief Generate a pre-filled GitHub issue URL (for QR code)
     *
     * URL is truncated to stay under ~2000 chars for QR code compatibility.
     */
    std::string generate_github_url(const CrashReport& report);

    /**
     * @brief Save human-readable crash report to file
     * @return true if file was written successfully
     */
    bool save_to_file(const CrashReport& report);

    /**
     * @brief Delete crash.txt after handling (prevents re-processing)
     */
    void consume_crash_file();

    /**
     * @brief Convert crash report to JSON (for CF Worker POST)
     */
    nlohmann::json report_to_json(const CrashReport& report);

    /**
     * @brief Convert crash report to human-readable text
     */
    std::string report_to_text(const CrashReport& report);

    /**
     * @brief Read the last N lines from the log file
     * @param num_lines Number of lines to read from end of file
     * @return Last N lines as a string, empty if log not found
     *
     * @note The caller typically requests a generous buffer (e.g., 500) so the
     *       report can filter out post-crash lines written by the reporting
     *       session and still retain pre-crash context (see crash report #827).
     */
    std::string get_log_tail(int num_lines = 500);

    /// Worker endpoint for auto-send
    static constexpr const char* CRASH_WORKER_URL = "https://crash.helixscreen.org/v1/report";

    /// Shared ingest API key (same as telemetry — write-only, not a true secret)
    static constexpr const char* INGEST_API_KEY = "hx-tel-v1-a7f3c9e2d1b84056";

    /// GitHub repo for issue URL generation
    static constexpr const char* GITHUB_REPO = "prestonbrown/helixscreen";

  private:
    CrashReporter() = default;
    ~CrashReporter() = default;

    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;

    std::string config_dir_;
    bool initialized_ = false;
    bool isolated_log_paths_ = false; ///< Test-only: skip system log search

    std::string crash_file_path() const;
    std::string report_file_path() const;

    friend class CrashReporterTestAccess;
};

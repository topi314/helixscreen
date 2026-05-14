// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "subject_managed_panel.h"
#include "ui_observer_guard.h" // SubjectLifetime

#include <atomic>
#include <lvgl.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "hv/json.hpp"

// Forward declaration - enums are defined in printer_state.h
namespace helix {
enum class PrintJobState;
}
namespace helix {
enum class PrintOutcome;
}
namespace helix {
enum class PrintStartPhase;
}

namespace helix {

/**
 * @brief Manages print-related subjects for printer state
 *
 * Tracks print progress, state, timing, layers, and print start phases.
 * Provides 18 subjects for reactive UI updates during printing.
 * Extracted from PrinterState as part of god class decomposition.
 *
 * @note This class manages only the subjects and their values. The enums
 *       (PrintJobState, PrintOutcome, PrintStartPhase) remain in printer_state.h
 *       as they are widely used across the codebase.
 */
class PrinterPrintState {
  public:
    PrinterPrintState();
    ~PrinterPrintState() = default;

    // Non-copyable
    PrinterPrintState(const PrinterPrintState&) = delete;
    PrinterPrintState& operator=(const PrinterPrintState&) = delete;

    /**
     * @brief Initialize print subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update print state from Moonraker status JSON
     * @param status JSON object containing print_stats, virtual_sdcard data
     */
    void update_from_status(const nlohmann::json& status);

    /**
     * @brief Reset UI state when starting a new print
     *
     * Clears progress, layers, and timing but preserves filename.
     */
    void reset_for_new_print();

    // ========================================================================
    // Subject accessors (18 subjects)
    // ========================================================================

    /// Print progress as 0-100 percent
    lv_subject_t* get_print_progress_subject() {
        return &print_progress_;
    }

    /// Raw filename from Moonraker
    lv_subject_t* get_print_filename_subject() {
        return &print_filename_;
    }

    /// String state for UI display ("standby", "printing", etc.)
    lv_subject_t* get_print_state_subject() {
        return &print_state_;
    }

    /// Integer enum value for type-safe logic (PrintJobState)
    lv_subject_t* get_print_state_enum_subject() {
        return &print_state_enum_;
    }

    /**
     * @brief Lifetime token for the "static" print subjects (e.g. print_state_enum).
     *
     * Production-wise these subjects live for the process, but tests call
     * `deinit_subjects()` / `init_subjects()` between cases. Cross-singleton
     * observers (e.g. AmsState's print-state observer) MUST pass this token to
     * `observe_int_sync(...)` — otherwise an ObserverGuard outliving a
     * `deinit_subjects()` cycle will UAF in `lv_observer_remove()` (subject
     * deinit already freed the observer node).
     */
    [[nodiscard]] SubjectLifetime get_static_subjects_lifetime() const {
        return static_subjects_lifetime_;
    }

    /// 1 when PRINTING or PAUSED, 0 otherwise
    lv_subject_t* get_print_active_subject() {
        return &print_active_;
    }

    /// Terminal outcome that persists (PrintOutcome)
    lv_subject_t* get_print_outcome_subject() {
        return &print_outcome_;
    }

    /// Combined: 1 when active AND not in start phase
    lv_subject_t* get_print_show_progress_subject() {
        return &print_show_progress_;
    }

    /// Clean display filename without path/prefix
    lv_subject_t* get_print_display_filename_subject() {
        return &print_display_filename_;
    }

    /// LVGL path to current print thumbnail
    lv_subject_t* get_print_thumbnail_path_subject() {
        return &print_thumbnail_path_;
    }

    /// Current layer number (0-based)
    lv_subject_t* get_print_layer_current_subject() {
        return &print_layer_current_;
    }

    /// Total layers from file metadata
    lv_subject_t* get_print_layer_total_subject() {
        return &print_layer_total_;
    }

    /// Elapsed print time in seconds (extrusion time only, from Moonraker print_duration)
    lv_subject_t* get_print_duration_subject() {
        return &print_duration_;
    }

    /// Wall-clock elapsed time in seconds (from Moonraker total_duration, includes prep)
    lv_subject_t* get_print_elapsed_subject() {
        return &print_elapsed_;
    }

    /// Estimated remaining time in seconds
    lv_subject_t* get_print_time_left_subject() {
        return &print_time_left_;
    }

    /// Filament used during current print (in mm, from Moonraker print_stats.filament_used)
    lv_subject_t* get_print_filament_used_subject() {
        return &print_filament_used_;
    }

    /**
     * @brief Per-extruder filament_used (mm, integer), 0-based.
     *
     * Populated from Klipper's per-object `extruder`/`extruder1`/`extruder2`/...
     * `filament_used` fields during status updates. Callers use the returned
     * subject to observe one tool's consumption independently of the aggregate
     * `print_stats.filament_used` stream.
     *
     * Map entries are **pre-populated** in `init_subjects()` for all indices
     * `0 .. kMaxExtruderScan-1`, so the map structure is frozen after init.
     * This eliminates the WebSocket-BG-thread vs UI-thread rehash race that
     * lazy emplace would expose (only subject values change during status
     * updates, and `lv_subject_set_int` is atomic for the int value).
     *
     * These subjects are still **dynamic** ([L077]): they are re-created on
     * `deinit_subjects()` / `init_subjects()` cycles. Observers MUST pass a
     * SubjectLifetime token and subscribe via `observe_int_sync(..., lifetime)`
     * — otherwise ObserverGuard dangles on reconnect.
     *
     * @param extruder_idx 0-based extruder index (0 = "extruder", 1 = "extruder1", ...)
     * @param[out] lifetime Token whose expiration signals subject death
     * @return Non-null subject pointer for `0 <= idx < kMaxExtruderScan` once
     *         `init_subjects()` has run; `nullptr` otherwise (lifetime untouched).
     */
    lv_subject_t* get_extruder_filament_used_subject(int extruder_idx,
                                                     SubjectLifetime& lifetime);

    /// Maximum number of per-extruder filament subjects pre-populated at init.
    /// Klipper toolchanger setups max out well below this.
    static constexpr int kMaxExtruderScan = 16;

    /// Current PrintStartPhase enum value
    lv_subject_t* get_print_start_phase_subject() {
        return &print_start_phase_;
    }

    /// Human-readable phase message
    lv_subject_t* get_print_start_message_subject() {
        return &print_start_message_;
    }

    /// Print start progress 0-100%
    lv_subject_t* get_print_start_progress_subject() {
        return &print_start_progress_;
    }

    /// 1 while print workflow executing, 0 otherwise
    lv_subject_t* get_print_in_progress_subject() {
        return &print_in_progress_;
    }

    /// Predicted pre-print time remaining (formatted string, e.g. "~2 min left")
    lv_subject_t* get_print_start_time_left_subject() {
        return &print_start_time_left_;
    }

    /// Predicted pre-print time remaining in seconds (for augmenting total remaining)
    lv_subject_t* get_preprint_remaining_subject() {
        return &preprint_remaining_;
    }

    /// Pre-print elapsed seconds (time since preparation started)
    lv_subject_t* get_preprint_elapsed_subject() {
        return &preprint_elapsed_;
    }

    /// Klipper display message (from M117 / display_status.message)
    lv_subject_t* get_display_message_subject() {
        return &display_message_;
    }

    /// 1 when display_message is non-empty, 0 when empty (for XML visibility binding)
    lv_subject_t* get_display_message_visible_subject() {
        return &display_message_visible_;
    }

    /// Klipper print_stats.message — typically populated on pause/error to describe
    /// the reason (e.g. "Filament Sensor filament_sensor: Runout Detected" or error
    /// strings). Empty for normal user-initiated PAUSE without a configured message.
    lv_subject_t* get_print_message_subject() {
        return &print_message_;
    }

    // ========================================================================
    // Setters
    // ========================================================================

    /**
     * @brief Set print outcome for UI badge display
     * @param outcome The print outcome value to set
     */
    void set_print_outcome(PrintOutcome outcome);

    /**
     * @brief Set the current print's thumbnail path
     * @param path LVGL-compatible path (e.g., "A:/tmp/thumbnail_xxx.bin")
     */
    void set_print_thumbnail_path(const std::string& path);

    /**
     * @brief Set display-ready print filename for UI binding
     * @param name Clean display name
     */
    void set_print_display_filename(const std::string& name);

    /**
     * @brief Set total layer count from file metadata
     * @param total Total number of layers
     */
    void set_print_layer_total(int total);

    /**
     * @brief Set current layer number (gcode response fallback)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     * Called from gcode response parser when print_stats.info doesn't fire.
     *
     * @param layer Current layer number
     */
    void set_print_layer_current(int layer);

    /**
     * @brief Check if real layer data has been received from slicer/Moonraker.
     * When false, layer count is estimated from print progress.
     */
    bool has_real_layer_data() const {
        return has_real_layer_data_;
    }

    /**
     * @brief Set print start phase and update message/progress
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param phase Current PrintStartPhase
     * @param message Human-readable message (e.g., "Heating Nozzle...")
     * @param progress Estimated progress 0-100%
     */
    void set_print_start_state(PrintStartPhase phase, const char* message, int progress);

    /**
     * @brief Reset print start to IDLE
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     */
    void reset_print_start_state();

    /**
     * @brief Set the print-in-progress flag (UI workflow state)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     */
    void set_print_in_progress(bool in_progress);

    /**
     * @brief Set predicted pre-print time remaining string
     *
     * Main-thread only (called from LVGL timer).
     * @param text Formatted string (e.g., "~2 min left") or empty to clear
     */
    void set_print_start_time_left(const char* text);

    /**
     * @brief Clear predicted pre-print time remaining
     */
    void clear_print_start_time_left();

    /**
     * @brief Set pre-print remaining seconds (for total remaining augmentation)
     *
     * Main-thread only (called from LVGL timer).
     */
    void set_preprint_remaining_seconds(int seconds);

    /**
     * @brief Set pre-print elapsed seconds (for elapsed display during preparation)
     *
     * Main-thread only (called from LVGL timer).
     */
    void set_preprint_elapsed_seconds(int seconds);

    /**
     * @brief Set slicer's estimated total print time (from file metadata)
     *
     * Used as a fallback for remaining time when print_duration is still 0.
     * @param seconds Slicer's estimated total print time in seconds
     */
    void set_estimated_print_time(int seconds);

    /**
     * @brief Get slicer's estimated total print time
     * @return Estimated print time in seconds, or 0 if not set
     */
    int get_estimated_print_time() const;

    // ========================================================================
    // State queries
    // ========================================================================

    /**
     * @brief Get current print job state as enum
     * @return Current PrintJobState
     */
    PrintJobState get_print_job_state() const;

    /**
     * @brief Check if a new print can be started
     * @return true if printer is in a state that allows starting a new print
     */
    [[nodiscard]] bool can_start_new_print() const;

    /**
     * @brief Check if a print workflow is currently in progress
     * @return true during print preparation
     */
    [[nodiscard]] bool is_print_in_progress() const;

    /**
     * @brief Check if currently in print start phase
     * @return true if phase is not IDLE
     */
    [[nodiscard]] bool is_in_print_start() const;

  private:
    friend class PrinterPrintStateTestAccess;

    /**
     * @brief Update print_show_progress_ combined subject
     *
     * Sets print_show_progress_ to 1 only when print_active==1 AND print_start_phase==IDLE.
     */
    void update_print_show_progress();

    /**
     * @brief Update display_message_visible_ derived subject
     *
     * Visible when display_message is non-empty AND print_start_phase==IDLE. During
     * pre-print, print_start_collector already pipes display_status.message into
     * print_start_message, so showing display_message would duplicate it on the
     * print-status widget.
     */
    void update_display_message_visible();

    /**
     * @brief Internal setter for print-in-progress flag
     *
     * Called via helix::async::invoke from set_print_in_progress().
     */
    void set_print_in_progress_internal(bool in_progress);

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    /// Lifetime for the "static" subjects below. Reset (to false then released)
    /// in `deinit_subjects()` so cross-singleton observers can detect subject
    /// death and skip `lv_observer_remove()` on freed observer nodes.
    SubjectLifetime static_subjects_lifetime_;

    // Print progress subjects
    lv_subject_t print_progress_{};         // Integer 0-100
    lv_subject_t print_filename_{};         // String buffer
    lv_subject_t print_state_{};            // String buffer (for UI display)
    lv_subject_t print_state_enum_{};       // Integer: PrintJobState enum
    lv_subject_t print_active_{};           // Integer: 1 when PRINTING/PAUSED
    lv_subject_t print_outcome_{};          // Integer: PrintOutcome enum
    lv_subject_t print_show_progress_{};    // Integer: 1 when active AND not starting
    lv_subject_t print_display_filename_{}; // String: clean filename
    lv_subject_t print_thumbnail_path_{};   // String: LVGL thumbnail path

    // Layer tracking subjects
    lv_subject_t print_layer_current_{}; // Current layer (0-based)
    lv_subject_t print_layer_total_{};   // Total layers

    // Print time tracking subjects (in seconds)
    lv_subject_t print_duration_{};      // Extrusion-only elapsed time (Moonraker print_duration)
    lv_subject_t print_elapsed_{};       // Wall-clock elapsed time (Moonraker total_duration)
    lv_subject_t print_time_left_{};     // Estimated remaining
    lv_subject_t print_filament_used_{}; // Filament used in mm (from Moonraker print_stats)

    // Per-extruder filament_used (mm) — heap-allocated for stable pointers.
    // Map entries are pre-populated by init_subjects() for indices 0..kMaxExtruderScan-1,
    // freezing the map structure so the WebSocket background thread cannot trigger
    // a rehash while a UI-thread caller is reading. Only the int value inside
    // each subject is mutated during status updates (atomic via lv_subject_set_int).
    // See [L077] for the lifetime-token discipline required when observing
    // these dynamic subjects.
    struct ExtruderFilamentInfo {
        std::unique_ptr<lv_subject_t> subject; ///< int: mm consumed on this extruder
        SubjectLifetime lifetime;              ///< shared_ptr<bool>: true while subject alive
    };
    std::unordered_map<int, ExtruderFilamentInfo> extruder_filament_used_;

    /// Create the per-extruder filament_used entry for idx. Called only from
    /// init_subjects() to pre-populate the map — NEVER from update_from_status
    /// or the accessor, because emplace from the WebSocket background thread
    /// could race with UI-thread reads via rehash invalidation.
    void create_extruder_filament_entry(int extruder_idx);

    // Print start progress subjects
    lv_subject_t print_start_phase_{};    // Integer: PrintStartPhase enum
    lv_subject_t print_start_message_{};  // String: phase message
    lv_subject_t print_start_progress_{}; // Integer: 0-100%

    // Print workflow in-progress subject
    lv_subject_t print_in_progress_{};

    // Pre-print duration prediction subjects
    lv_subject_t print_start_time_left_{};
    lv_subject_t preprint_remaining_{}; // int: seconds remaining for pre-print
    lv_subject_t preprint_elapsed_{};   // int: seconds elapsed since pre-print started

    // Slicer estimated total print time (not a subject - no XML binding needed)
    int estimated_print_time_ = 0;

    // Exponential moving average for time remaining estimate.
    // Smooths out wild jumps at low progress where the extrapolation is noisy.
    double smoothed_remaining_ = 0.0;
    bool has_smoothed_remaining_ = false;

    // Layer tracking: true when real layer data received from print_stats.info or gcode fallback.
    // When false, current_layer is estimated from progress * total_layers.
    // Atomic: written from background thread (gcode fallback), read from main thread (UI).
    std::atomic<bool> has_real_layer_data_{false};

    // Slicer progress from display_status (M73 gcode command)
    // When active, preferred over virtual_sdcard file-position progress
    double slicer_progress_ = 0.0;        // Raw 0.0-1.0 from display_status
    bool slicer_progress_active_ = false; // True once non-zero value seen during print

    // Display message from Klipper (M117 gcode / display_status.message)
    lv_subject_t display_message_{};         // String subject for UI binding
    lv_subject_t display_message_visible_{}; // Integer: 1 when non-empty, 0 when empty
    char display_message_buf_[128]{};        // Buffer for message storage

    // print_stats.message from Klipper (set by firmware on pause/error to describe reason)
    lv_subject_t print_message_{};    // String: e.g. "Filament Sensor: Runout Detected"
    char print_message_buf_[256]{};   // Buffer for print_stats.message storage

    // String buffers for subject storage
    char print_filename_buf_[256]{};
    char print_display_filename_buf_[128]{};
    char print_thumbnail_path_buf_[512]{};
    char print_state_buf_[32]{};
    char print_start_message_buf_[64]{};
    char print_start_time_left_buf_[32]{};
};

} // namespace helix

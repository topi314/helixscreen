// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_job_queue_modal.h"
#include "ui_observer_guard.h"
#include "ui_runout_guidance_modal.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "print_history_manager.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <unordered_set>

namespace helix {

class PrinterState;
enum class PrintJobState;

class PrintStatusWidget : public PanelWidget {
  public:
    PrintStatusWidget();
    ~PrintStatusWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "print_status";
    }

    // Configuration
    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Re-check runout condition after wizard completion
    void trigger_idle_runout_check();

    /// XML event callback — opens print status panel or file browser
    static void print_card_clicked_cb(lv_event_t* e);

    /// Library row callbacks
    static void library_files_cb(lv_event_t* e);
    static void library_last_cb(lv_event_t* e);
    static void library_recent_cb(lv_event_t* e);
    static void library_queue_cb(lv_event_t* e);

    /// Configure picker callback
    static void print_status_picker_backdrop_cb(lv_event_t* e);

    /// Registry of live (attached) widget instances for use-after-free prevention
    static std::unordered_set<PrintStatusWidget*>& live_instances();

    // Test accessors (always compiled — used by unit tests)
    const std::string& layout_style_for_test() const {
        return layout_style_;
    }
    const std::string& nozzle_tool_override_for_test() const {
        return nozzle_tool_override_;
    }
    static lv_subject_t* layout_effective_subject_for_test() {
        return &layout_effective_subject_;
    }
    static lv_subject_t* temp_under_thumb_subject_for_test() {
        return &temp_under_thumb_subject_;
    }
    static lv_subject_t* show_filament_active_subject_for_test() {
        return &show_filament_active_subject_;
    }

    // Test-only — instantiate the formatter without needing a real attach()
    static void ensure_formatter_for_test() {
        if (s_formatter_refcount_++ == 0) {
            s_formatter_ = std::make_unique<DetailedFormatter>();
        }
    }
    static void release_formatter_for_test() {
        if (--s_formatter_refcount_ == 0) {
            s_formatter_.reset();
        }
    }

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Cached widget references (looked up after XML creation)
    lv_obj_t* print_card_thumb_ = nullptr;          // Idle state thumbnail
    lv_obj_t* print_card_active_thumb_ = nullptr;   // Active print thumbnail
    lv_obj_t* print_card_layout_ = nullptr;         // Row/column layout container
    lv_obj_t* print_card_thumb_wrap_ = nullptr;     // Thumbnail wrapper
    lv_obj_t* print_card_info_ = nullptr;           // Info section (filename/progress)
    lv_obj_t* print_card_printing_ = nullptr;       // Active state container (preparing + printing)
    lv_obj_t* print_card_preparing_info_ = nullptr; // Preparing info section

    // Library idle state widgets
    lv_obj_t* print_card_idle_ = nullptr;          // Full library idle card
    lv_obj_t* print_card_idle_compact_ = nullptr;  // Compact idle card (1x2)
    lv_obj_t* print_card_thumb_compact_ = nullptr; // Compact thumbnail
    lv_obj_t* library_row_last_ = nullptr;         // Print Last row (for graying out)
    lv_obj_t* compact_row_last_ = nullptr;         // Compact Print Last row (for graying out)

    // Size-dependent subject for XML bindings (1 = column/2x2 mode, 0 = row/wide)
    static inline lv_subject_t column_mode_subject_;
    static inline bool column_mode_subject_initialized_ = false;

    // Current colspan (widget grid width), exposed for breakpoint-style XML bindings
    static inline lv_subject_t colspan_subject_;
    static inline bool colspan_subject_initialized_ = false;

    // Per-element visibility subjects — 1 = hidden, 0 = visible. XML binds via
    // <bind_flag_if_eq ... ref_value="1"/>. apply_visibility_config() computes
    // each value from show_* config + breakpoint + job queue count and writes
    // these subjects; C++ no longer toggles LV_OBJ_FLAG_HIDDEN directly.
    // Initial values (0 visible, 1 hidden for queue) are safe defaults for the
    // first XML parse; apply_visibility_config() re-derives them on attach().
    static inline lv_subject_t title_hidden_subject_;
    static inline lv_subject_t files_hidden_subject_;
    static inline lv_subject_t last_hidden_subject_;
    static inline lv_subject_t recent_hidden_subject_;
    static inline lv_subject_t queue_hidden_subject_;
    static inline lv_subject_t actions_hidden_subject_;
    static inline bool visibility_subjects_initialized_ = false;

    // Detailed-layout subjects (static inline — shared across all widget instances)
    static inline lv_subject_t layout_mode_subject_{};       // 0=library, 1=detailed (user pref)
    static inline lv_subject_t layout_effective_subject_{};  // after width gating
    static inline lv_subject_t temp_under_thumb_subject_{};  // 1 at colspan==2, 0 at >=3
    static inline lv_subject_t show_filament_active_subject_{};
    static inline lv_subject_t multi_tool_subject_{};        // 1 when tool_count > 1
    static inline bool detailed_subjects_initialized_ = false;

    // Compact mode and state tracking
    bool is_compact_ = false;
    bool is_column_ = false;
    bool last_print_available_ = false;

    // PrinterState reference for subject access
    PrinterState& printer_state_;

    // Observers (RAII cleanup via ObserverGuard)
    ObserverGuard print_state_observer_;
    ObserverGuard print_thumbnail_path_observer_;
    ObserverGuard filament_runout_observer_;
    ObserverGuard job_queue_count_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard breakpoint_observer_;

    // Guards async thumbnail callbacks and history observer from use-after-free
    helix::AsyncLifetimeGuard lifetime_;

    // History observer for updating idle thumbnail when history loads
    helix::HistoryChangedCallback history_changed_cb_;

    // Filament runout modal
    RunoutGuidanceModal runout_modal_;
    bool runout_modal_shown_ = false;

    // Job queue
    helix::JobQueueModal job_queue_modal_;

    // First-instance formatter singleton — owns observers + formatted subjects for Detailed layout.
    // Created on the first widget attach, destroyed on the last detach.
    class DetailedFormatter {
      public:
        DetailedFormatter();
        ~DetailedFormatter();
        DetailedFormatter(const DetailedFormatter&) = delete;
        DetailedFormatter& operator=(const DetailedFormatter&) = delete;
        DetailedFormatter(DetailedFormatter&&) = delete;
        DetailedFormatter& operator=(DetailedFormatter&&) = delete;

      private:
        SubjectManager subjects_;

        // Buffers backing string subjects
        char progress_pct_buf_[8];        // "100%"
        char layer_text_buf_[32];         // "Layer 9999 / 9999"
        char time_text_buf_[40];          // "12h 34m / 99h 99m"
        char filament_text_buf_[32];      // "1234.5m / 9999.9m"
        char nozzle_text_buf_[32];        // "265 / 270°C"
        char bed_text_buf_[32];
        char chamber_text_buf_[32];
        char nozzle_tool_label_buf_[8];   // "T0", "T9"
        char idle_filename_buf_[160];
        char idle_when_buf_[64];          // "Completed 2 hours ago"
        char idle_meta_buf_[64];          // "12.4m filament • 4h 12m"

        // String + int subjects (XML-registered)
        lv_subject_t progress_pct_subject_;
        lv_subject_t layer_text_subject_;
        lv_subject_t time_text_subject_;
        lv_subject_t filament_text_subject_;
        lv_subject_t nozzle_text_subject_;
        lv_subject_t bed_text_subject_;
        lv_subject_t chamber_text_subject_;
        lv_subject_t nozzle_tool_label_subject_;
        lv_subject_t idle_filename_subject_;
        lv_subject_t idle_when_subject_;
        lv_subject_t idle_meta_subject_;
        lv_subject_t idle_has_last_subject_;

        // Print-state observers (wired in constructor, RAII cleanup via ObserverGuard)
        ObserverGuard progress_observer_;
        ObserverGuard layer_current_observer_;
        ObserverGuard layer_total_observer_;
        ObserverGuard elapsed_observer_;
        ObserverGuard time_left_observer_;
        ObserverGuard filament_used_observer_;

        // Nozzle temp observers — paired SubjectLifetimes per [L084] for Task 8 pinning.
        // In auto mode (default) the static active_extruder subjects are observed, lifetimes unused.
        ObserverGuard nozzle_temp_observer_;
        ObserverGuard nozzle_target_observer_;
        SubjectLifetime nozzle_temp_lifetime_;
        SubjectLifetime nozzle_target_lifetime_;

        ObserverGuard bed_temp_observer_;
        ObserverGuard bed_target_observer_;
        ObserverGuard chamber_temp_observer_;
        ObserverGuard chamber_target_observer_;

        void update_progress_pct();
        void update_layer_text();
        void update_time_text();
        void update_filament_text();
        void update_nozzle_text();
        void update_bed_text();
        void update_chamber_text();
    };

    static inline std::unique_ptr<DetailedFormatter> s_formatter_;
    static inline int s_formatter_refcount_ = 0;

    // Print card update methods
    [[nodiscard]] std::string get_last_print_thumbnail_path() const;
    void handle_print_card_clicked();
    void on_print_state_changed(PrintJobState state);
    void on_print_thumbnail_path_changed(const char* path);
    void reset_print_card_to_idle();
    void update_idle_compact_mode();
    void update_last_print_availability();

    // Library action handlers
    void handle_library_files();
    void handle_library_last();
    void handle_library_recent();
    void handle_library_queue();
    void update_job_queue_row_visibility();

    // Filament runout handling
    void check_and_show_idle_runout_modal();
    void show_idle_runout_modal();

    // Configuration state
    nlohmann::json config_;
    std::string layout_style_ = "library";       // "library" | "detailed"
    std::string nozzle_tool_override_ = "auto";  // "auto" | extruder name
    bool show_title_ = true;
    bool show_print_files_ = true;
    bool show_reprint_last_ = true;
    bool show_recent_prints_ = true;
    bool show_job_queue_ = true;

    // Configure picker
    lv_obj_t* picker_backdrop_ = nullptr;
    void show_configure_picker();
    void dismiss_configure_picker();
    void apply_visibility_config();
    void recompute_actions_visibility();
    void apply_picker_state();

    static PrintStatusWidget* s_active_picker_;
};

} // namespace helix

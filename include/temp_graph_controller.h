// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file temp_graph_controller.h
 * @brief Shared temperature graph lifecycle controller
 *
 * Consolidates the duplicated graph lifecycle pattern used by TempGraphWidget,
 * TempGraphOverlay, and TemperatureService's mini graph:
 *   create graph -> add series -> setup observers -> backfill history -> auto-range
 *
 * Consumers provide a TempGraphControllerConfig describing which series to display,
 * then call rebuild() on connect and the controller handles the rest.
 *
 * @pattern Controller owns graph + observers; consumer owns the container.
 * @threading Observer callbacks are deferred via UpdateQueue (observe_int_sync).
 */

#pragma once

#include "ui_observer_guard.h"
#include "ui_temp_graph.h"
#include "ui_temp_graph_scaling.h"

#include "async_lifetime_guard.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix {

// ============================================================================
// Color palette — shared across all temp graph consumers
// ============================================================================

static constexpr int TEMP_GRAPH_PALETTE_SIZE = 8;

/// Nord-inspired palette: nozzle red, bed cyan, chamber green, then 5 more
extern const lv_color_t TEMP_GRAPH_SERIES_COLORS[TEMP_GRAPH_PALETTE_SIZE];

// ============================================================================
// Configuration types
// ============================================================================

/**
 * @brief Describes a single temperature series to display
 *
 * The klipper_name determines how the controller resolves subjects:
 * - "extruder" / "extruder1" etc  -> extruder temp/target subjects
 * - "heater_bed"                  -> bed temp/target subjects
 * - "chamber"                     -> chamber temp/target subjects
 * - anything else                 -> TemperatureSensorManager lookup
 */
struct TempGraphSeriesSpec {
    std::string klipper_name; ///< Klipper object key (e.g., "extruder", "heater_bed")
    lv_color_t color{};       ///< Series line color
    bool show_target = false; ///< Whether this heater has a controllable target
    std::string display_name; ///< Label shown in the graph legend; falls back to klipper_name when empty
};

/**
 * @brief Full configuration for a TempGraphController instance
 */
struct TempGraphControllerConfig {
    int point_count = UI_TEMP_GRAPH_DEFAULT_POINTS; ///< Data points per series
    const char* axis_size = "sm";                   ///< Axis font size ("xs", "sm", "md", "lg")
    uint32_t initial_features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                                TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS |
                                TEMP_GRAPH_FEATURE_GRADIENTS;
    TempGraphScaleParams scale_params{};     ///< Y-axis auto-scaling parameters
    std::vector<TempGraphSeriesSpec> series; ///< Series to display
};

// ============================================================================
// Controller
// ============================================================================

/**
 * @brief Manages a temperature graph's full lifecycle
 *
 * Owns the ui_temp_graph_t, observers, and auto-range state.
 * The consumer provides the LVGL container; the controller creates the graph
 * inside it and handles all observer setup, history backfill, and teardown.
 *
 * Typical usage:
 * @code
 *   TempGraphControllerConfig cfg;
 *   cfg.series = {{"extruder", color_red, true}, {"heater_bed", color_cyan, true}};
 *   controller_ = std::make_unique<TempGraphController>(container, cfg);
 *   // Controller is now live. Call set_features() on resize, pause()/resume() on
 *   // activate/deactivate. Destruction handles full cleanup.
 * @endcode
 */
class TempGraphController {
  public:
    /**
     * @brief Construct and initialize the graph
     *
     * Creates the graph widget inside container, adds series, sets up observers,
     * backfills history, and applies initial auto-range.
     *
     * @param container Parent LVGL object (must outlive the controller)
     * @param config    Series and display configuration
     */
    TempGraphController(lv_obj_t* container, const TempGraphControllerConfig& config);

    ~TempGraphController();

    // Non-copyable, non-movable
    TempGraphController(const TempGraphController&) = delete;
    TempGraphController& operator=(const TempGraphController&) = delete;
    TempGraphController(TempGraphController&&) = delete;
    TempGraphController& operator=(TempGraphController&&) = delete;

    /// Update which graph features are visible (e.g., on resize)
    void set_features(uint32_t features);

    /// Pause observer-driven updates (e.g., when panel is off-screen)
    void pause();

    /// Resume updates and backfill any missed history
    void resume();

    /**
     * @brief Tear down and recreate the graph from scratch
     *
     * Called on reconnect to re-resolve subjects (which may have been
     * recreated by printer state reinitialization).
     */
    void rebuild();

    /**
     * @brief Detach all observers and invalidate lifetime tokens
     *
     * Used before deferred destruction (prevents observer removal on freed
     * objects) and during rebuild (tears down before recreating). Idempotent —
     * safe to call multiple times; the destructor calls it again as a guard.
     */
    void detach();

    /// Access the underlying graph (for chip toggles, custom styling, etc.)
    ui_temp_graph_t* graph() const {
        return graph_;
    }

    /// Check if graph was created successfully
    bool is_valid() const {
        return graph_ != nullptr;
    }

    /**
     * @brief Look up the graph series ID for a given Klipper name
     * @return Series ID (>= 0) or -1 if not found
     */
    int series_id_for(const std::string& klipper_name) const;

  private:
    /// Per-series runtime state (extends the spec with observer handles)
    struct SeriesState {
        std::string klipper_name;
        int series_id = -1;
        bool show_target = false;
        bool is_dynamic = false;
        int64_t last_update_ms = 0; ///< Throttle graph updates to 1Hz per series
        ObserverGuard temp_obs;
        ObserverGuard target_obs;
        SubjectLifetime lifetime;
    };

    void create_graph();
    void setup_series();
    void setup_observers();
    void backfill_history();
    void apply_auto_range();

    TempGraphControllerConfig config_;
    lv_obj_t* container_ = nullptr;
    ui_temp_graph_t* graph_ = nullptr;

    std::vector<SeriesState> series_;
    ObserverGuard connection_observer_;

    AsyncLifetimeGuard lifetime_;
    uint32_t generation_ = 0;
    bool paused_ = false;
    float y_axis_max_ = 100.0f;

    /// Debounce rapid rebuilds (e.g., reconnect flapping in Klipper error state)
    std::chrono::steady_clock::time_point last_rebuild_time_{};
    static constexpr auto REBUILD_DEBOUNCE = std::chrono::seconds(2);
};

} // namespace helix

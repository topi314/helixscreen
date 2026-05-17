# Print Status Fan Row — Design

**Status:** Approved — ready for implementation plan
**Author:** Preston Brown (with brainstorming assistant)
**Date:** 2026-05-17

## Problem

The Print Status panel's right column has unused vertical space between the filament/AMS status row and the action buttons. Users have asked to see fan information here, clickable as a shortcut to the fan control overlay. The space is constrained when AMS/tool-change indicators are visible, so the row must hide itself adaptively when it would push the buttons out of view.

## Goals

- Surface part / hotend / aux fan speeds at-a-glance during a print.
- One-tap entry point to the existing `fan_control_overlay`.
- Coexist with `filament_ams_status_row`; hide only when the column genuinely cannot fit it.
- Reuse existing fan infrastructure (classification, dynamic subjects, spin animation, overlay).

## Non-Goals

- Per-fan tuning inline (the overlay handles it).
- Showing every detected fan (controller_fan, MCU fans). Just the standard 3 — part, hotend, aux.
- New widget-registry / configurability surface. This is a fixed feature of the print status panel.

## Layout

A single horizontal row inserted in `ui_xml/print_status_panel.xml` between `filament_ams_status_row` (line 305) and `button_grid` (line 334):

```
🌀 Part 100% · 🌀 Hotend 82% · 🌀 Aux 0%
```

Sister-row to `speed_flow_row` (line 290-302) in styling and weight — bare `lv_obj` container, not a `ui_card`.

**Per-fan unit (3 across):**
- `icon` (`src="fan"`, `size="xs"`, `variant="secondary"`, `clickable="false"`, `event_bubble="true"`) — spins proportional to speed when animations enabled
- `text_small` name (e.g. "Part", "Hotend", "Aux") with `style_text_color="#primary"`, bubbles
- `text_small` speed value (e.g. "100%"), bubbles

**Container:** `flex_flow="row"`, `style_flex_main_place="center"`, `style_flex_cross_place="center"`, `style_pad_gap="#space_sm"`, `height="content"`, `clickable="true"` so the whole row is the click target.

**Separators:** `· ` text between units, styled as `text_small` with muted color.

**Aux unit (icon + name + value + preceding separator)** is hidden together when no aux fan is detected — same auto-hide pattern as `panel_widget_fan_stack` line 35-43.

## Visibility — Adaptive Fit Calculation

A new `int` subject `print_status_fans_fit` (1 = fits, 0 = doesn't) drives a single XML binding on the fan row:

```xml
<bind_flag_if_eq subject="print_status_fans_fit" flag="hidden" ref_value="0"/>
```

Single binding only (not paired bind_flag_if_eq's) to avoid the [L042] last-write-wins race.

**C++ owner:** `PrintStatusPanel` (`src/ui/ui_panel_print_status.cpp`). The subject is registered via `StaticSubjectRegistry::instance().register_deinit()` from inside the panel's subject-init function, co-located with init per the CLAUDE.md rule.

**`recompute_fans_fit()` algorithm:**

1. `lv_obj_update_layout(controls_section_)` to settle heights.
2. Sum the natural heights of each currently-visible non-fan child:
   - `temp_card_` height
   - `speed_flow_row_` height (0 if `hidden` flag set)
   - `filament_ams_status_row_` height (0 if hidden)
   - `button_grid_` content height — read directly via `lv_obj_get_content_height`, not assumed
   - `print_status_extras_` height
3. Add column gaps: `(visible_children - 1) * space_md`.
4. `available = lv_obj_get_height(controls_section_) - sum_used`.
5. `needed = fan_row_natural_height_`. Measured once at attach, when the row is forced visible (subject = 1) and `lv_obj_update_layout` is called before any consumer can observe the temporary "fits" state. Cached for the panel's lifetime.
6. Hysteresis: if currently hidden, transition to visible only when `available >= needed + 4`. If currently visible, transition to hidden only when `available < needed`. The 4px band absorbs sub-pixel layout jitter at the boundary.
7. Compare to current subject value — set only if changed.

**Triggers (each scheduled through `lifetime_.defer("PrintStatusPanel::recompute_fans_fit", ...)` per [L051]):**
- `ui_breakpoint` observer
- `filament_sensor_count` observer
- `ams_slot_count` observer
- `toolchange_visible` observer
- `LV_EVENT_SIZE_CHANGED` on `controls_section_`
- Once after attach (initial)

**Loop safety:** the fan row's own visibility is excluded from `sum_used` — toggling it doesn't feed back into the equation. The "only-on-change" guard backs that up.

## Fan Data Binding

**Classification helper extraction:** `FanStackWidget::bind_fans()` at `src/ui/panel_widgets/fan_stack_widget.cpp:343-413` contains the part/hotend/aux selection loop. Factor that out:

```cpp
// New in include/printer_fan_state.h:
struct PrimaryFans {
    std::string part;
    std::string hotend;
    std::string aux;
};
PrimaryFans classify_primary_fans() const;
```

Both `FanStackWidget` and the new print-status code call it — single source of truth, no duplication.

**Speed subjects (dynamic — [L084]):** each fan name maps to a `lv_subject_t*` via `PrinterFanState::get_fan_speed_subject(object_name, lifetime)`. In the `PrintStatusPanel` header, every observer guard has a paired lifetime member declared next to it:

```cpp
ObserverGuard part_speed_observer_;
SubjectLifetime part_speed_lifetime_;
ObserverGuard hotend_speed_observer_;
SubjectLifetime hotend_speed_lifetime_;
ObserverGuard aux_speed_observer_;
SubjectLifetime aux_speed_lifetime_;
ObserverGuard fans_version_observer_; // static — no paired lifetime needed
// ... existing members ...
helix::AsyncLifetimeGuard lifetime_; // declared LAST per existing pattern
```

**Rebind on rediscovery:** when `fans_version` ticks, the panel reclassifies, then for each role:
1. `lifetime_.reset()` first (per ordering rule)
2. `observer_.reset()` second (always `reset()`, never `release()` per [L085])
3. Resolve fresh subject pointer via `get_fan_speed_subject(name, lifetime_)`
4. Create new observer via `observe_int_sync<PrintStatusPanel>(...)`

**Aux presence:** a `print_status_aux_fan_present` int subject (0/1) set by the classification step. The aux cluster's `bind_flag_if_eq ref_value="0"` hides icon + name + value + preceding separator together.

## Spin Animation

Reuse `helix::ui::fan_spin_animation` already used by `FanStackWidget` and `FanWidget`. Three small per-icon animations driven by the same speed updates. Honors the `animations_enabled` settings observer — same wiring as the existing widgets.

## Click Handling

XML attribute on the row: `clickable="true"` plus
```xml
<event_cb trigger="clicked" callback="on_print_status_fans_clicked"/>
```

`on_print_status_fans_clicked` is registered at panel init via `lv_xml_register_event_cb` (per [L014]) and pushes a lazily-created `fan_control_overlay_` through `NavigationManager::push_overlay`, mirroring `FanStackWidget::on_fan_stack_clicked` (`src/ui/panel_widgets/fan_stack_widget.cpp:812`). The overlay pointer is owned by `PrintStatusPanel` and created on first click.

All child icons / text use `clickable="false" event_bubble="true"` (per [L071]) so the parent's click event always fires.

## Threading

All observer callbacks come from `lv_subject_set_int` already routed through `UpdateQueue` by `PrinterFanState` from the WebSocket thread. No new background threads, no `std::thread::detach()` per [L083]. No HTTP work.

## Testing

**New file:** `tests/unit/test_print_status_fan_section.cpp` (tags `[print_status][fans]`), extending `XMLTestFixture`:

- 3 fans (part + hotend + aux) → all three units visible, names + speeds rendered correctly
- 2 fans (part + hotend, no aux) → aux cluster (separator + icon + name + value) hidden
- Speed updates propagate to label text (set subject, drain queue, read label)
- Click on row → push of `fan_control_overlay` via NavigationManager spy
- Adaptive fit:
  - Force tall column → `print_status_fans_fit == 1`, row visible
  - Force narrow column → recomputes to 0, row hidden
  - Toggle `ams_slot_count` from 0 to 4 → recompute fires, row may hide depending on column height
  - Hysteresis: oscillating `available` near boundary doesn't flap visibility every frame

**Drift test:** before extracting `classify_primary_fans()`, snapshot the current `bind_fans()` selection behavior in a test, then verify the extracted helper produces the same `PrimaryFans` for the same fan-list inputs. Tag `[fan_state][drift]`.

## Files Touched

| File | Change |
|------|--------|
| `ui_xml/components/print_status_fan_row.xml` | **NEW** — horizontal 3-fan row component |
| `ui_xml/print_status_panel.xml` | Insert `<print_status_fan_row/>` between `filament_ams_status_row` and `button_grid` |
| `src/main.cpp` | Register the new XML component file via `lv_xml_component_register_from_file()` per [L014] |
| `include/printer_fan_state.h` | Add `PrimaryFans` struct + `classify_primary_fans()` |
| `src/printer/printer_fan_state.cpp` | Implement `classify_primary_fans()` |
| `src/ui/panel_widgets/fan_stack_widget.cpp` | Refactor `bind_fans()` to call new helper (same behavior) |
| `include/ui_panel_print_status.h` | New observer + lifetime members; subject decls; fan_control_overlay ptr |
| `src/ui/ui_panel_print_status.cpp` | Subject registration, fan observers, `recompute_fans_fit()`, click handler, lazy overlay create |
| `tests/unit/test_print_status_fan_section.cpp` | **NEW** — coverage above |

## Lessons Cited

- **[L014]** Register new XML components in `main.cpp`
- **[L042]** Single `bind_flag_if_eq` for visibility — no paired observers
- **[L051]** Timer/defer through `AsyncLifetimeGuard::token()`
- **[L071]** Child click passthrough — `clickable="false" event_bubble="true"`
- **[L075]** Validate `lv_obj` before child access
- **[L083]** No bare detached threads
- **[L084]** Paired `SubjectLifetime` member next to every `ObserverGuard` on a dynamic subject
- **[L085]** Always `reset()`, never `release()` for cleanup

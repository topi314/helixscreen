# Environment Variables Reference

This document provides a comprehensive reference for all environment variables used in HelixScreen.

## Quick Reference

| Category | Count | Prefix |
|----------|-------|--------|
| [Display & Backend](#display--backend-configuration) | 14 | `HELIX_` |
| [Touch Calibration](#touch-calibration) | 7 | `HELIX_TOUCH_*` |
| [G-Code Viewer](#g-code-viewer) | 4 | `HELIX_` |
| [Bed Mesh](#bed-mesh) | 1 | `HELIX_` |
| [Mock & Testing](#mock--testing) | 14 | `HELIX_MOCK_*` |
| [UI Automation](#ui-automation) | 3 | `HELIX_AUTO_*` |
| [Calibration](#calibration-auto-start) | 2 | `*_AUTO_START` |
| [Development](#development) | 1 | `HELIX_` |
| [Debugging](#debugging) | 3 | `HELIX_DEBUG_*` |
| [Deployment](#deployment) | 1 | `HELIX_` |
| [Logging & Startup](#logging--startup) | 2 | `HELIX_` |
| [Data Paths](#data-paths) | 3 | `HELIX_` / Standard Unix |

---

## Display & Backend Configuration

These variables control how HelixScreen connects to displays and input devices.

### `HELIX_THEME`

Override the theme loaded from config. Useful for testing themes or taking screenshots without changing settings.

| Property | Value |
|----------|-------|
| **Values** | Theme filename without `.json` extension (e.g., `gruvbox`, `dracula`, `nord`) |
| **Default** | Read from config (`/display/theme`) |
| **File** | `src/ui/theme_manager.cpp` |

```bash
# Test Gruvbox theme
HELIX_THEME=gruvbox ./build/bin/helix-screen --test

# Take screenshot with Dracula theme
HELIX_THEME=dracula ./scripts/screenshot.sh helix-screen dracula-home home
```

**Available themes:** `ayu`, `catppuccin`, `chatgpt`, `chatgpt-classic`, `dracula`, `everforest`, `google-drive`, `google-notebooklm`, `gruvbox`, `kanagawa`, `nord`, `onedark`, `rose-pine`, `solarized`, `tokyonight`

### `HELIX_DISPLAY_BACKEND`

Override the automatic display backend detection.

| Property | Value |
|----------|-------|
| **Values** | `sdl`, `drm`, `fbdev` |
| **Default** | `fbdev` (CPU rendering, maximum compatibility) |
| **File** | `src/display_backend.cpp` |

**Backend comparison:**

| Backend | Rendering | Best for |
|---------|-----------|----------|
| `fbdev` | CPU (software) | Maximum compatibility, all hardware, SPI displays |
| `drm` | GPU-accelerated via DRM+EGL (OpenGL ES) | Pi 3B+, Pi 4, Pi 5, BTT CB1 with HDMI/DSI displays |
| `sdl` | SDL2 (desktop development) | Development on Linux/macOS desktops |

The `drm` backend uses DRM (Direct Rendering Manager) with EGL/OpenGL ES to offload rendering to the GPU. This reduces CPU usage and can improve frame rates, especially on Pi 4 and Pi 5. The `fbdev` backend is the safe default that works everywhere, including SPI displays that lack DRM support.

```bash
# Force SDL backend (useful for debugging on embedded systems)
HELIX_DISPLAY_BACKEND=sdl ./build/bin/helix-screen

# Force DRM backend with GPU acceleration
HELIX_DISPLAY_BACKEND=drm ./build/bin/helix-screen
```

**Enabling in production (systemd service):**
```ini
# /etc/systemd/system/helixscreen.service (or override)
[Service]
Environment="HELIX_DISPLAY_BACKEND=drm"
```

Then reload and restart:
```bash
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

**Supported hardware for `drm`:** Raspberry Pi 3B+, Pi 4, Pi 5, BTT CB1 (and other Allwinner H616 boards). Requires a display connected via HDMI or DSI — SPI displays are not supported with `drm`. If the `drm` backend fails to initialize, HelixScreen falls back to `fbdev` automatically.

**Automatic fbdev fallback for rotation:** When display rotation is configured (e.g., `"rotate": 180`) and the DRM plane does not support hardware rotation (common on Pi DSI displays where the plane rotation mask is `0x0`), HelixScreen automatically switches from `drm` to `fbdev` before applying rotation. The fbdev backend uses LVGL's native software rotation which is flicker-free — unlike the DRM path which must do CPU pixel reversal in the flush callback, risking visible tearing. This fallback is transparent and logged as:
```
DRM lacks hardware rotation for 180°, falling back to fbdev (flicker-free software rotation)
```

**Legacy DRM fallback:** If `drmModeAtomicCommit` fails (e.g., `EACCES` on some Pi 4 configurations), the DRM driver automatically falls back to legacy `drmModeSetCrtc` page flips. If the legacy path also fails, a log message suggests using `HELIX_DISPLAY_BACKEND=fbdev`.

### `HELIX_DRM_DEVICE`

Specify which DRM device to use when the DRM backend is active. Needed when multiple GPU/display controllers are present (common on Pi 5).

| Property | Value |
|----------|-------|
| **Values** | Device path (e.g., `/dev/dri/card0`, `/dev/dri/card1`) |
| **Default** | `/dev/dri/card0` (auto-detect scans for first device with a connected display) |
| **File** | `src/display_backend_drm.cpp` |

```bash
# Use secondary GPU / display controller
HELIX_DRM_DEVICE=/dev/dri/card1 ./build/bin/helix-screen
```

**Pi 5 DRM device mapping:**
| Device | Controller | Use |
|--------|-----------|-----|
| `/dev/dri/card0` | v3d | 3D rendering only (no display output) |
| `/dev/dri/card1` | DSI | Official Pi touchscreen |
| `/dev/dri/card2` | vc4 (HDMI) | HDMI displays |

On Pi 4 and earlier, `/dev/dri/card0` is typically the only device with display output. Auto-detection finds the first device with dumb buffer support and a connected display, so most users do not need to set this variable.

### `HELIX_TOUCH_DEVICE`

Override automatic touch input device detection.

| Property | Value |
|----------|-------|
| **Values** | Device path (e.g., `/dev/input/event0`) |
| **Default** | Auto-detect |
| **Files** | `src/display_backend_fbdev.cpp`, `src/display_backend_drm.cpp` |

```bash
# Specify touch device explicitly
HELIX_TOUCH_DEVICE=/dev/input/event2 ./build/bin/helix-screen
```

### `HELIX_MOUSE_DEVICE`

Override automatic USB mouse device detection. When set, skips sysfs capability scanning and uses the specified device directly.

| Property | Value |
|----------|-------|
| **Values** | Device path (e.g., `/dev/input/event4`) |
| **Default** | Auto-detect (scans for `REL_X` + `REL_Y` + `BTN_LEFT` capabilities) |
| **Files** | `src/api/display_backend_drm.cpp`, `src/api/display_backend_fbdev.cpp` |

```bash
# Specify mouse device explicitly
HELIX_MOUSE_DEVICE=/dev/input/event4 ./build/bin/helix-screen
```

**Auto-detection:** When unset, HelixScreen scans `/sys/class/input/event*/device/capabilities/` for devices with relative axes (`REL_X`, `REL_Y`) and a left mouse button (`BTN_LEFT`). Devices with absolute axes (`ABS_X`, `ABS_Y`) are skipped — those are touchscreens. The first matching device is used.

**Mouse cursor:** When a mouse is detected (manually or via auto-detection), a 12px white circular cursor is displayed. The mouse works alongside the touchscreen — both are active simultaneously.

### `HELIX_ALSA_DEVICE`

Override the ALSA PCM device used for UI sounds. The stock `default` resolves to `hw:0`, which fails on boards whose only audio output enumerates at a higher card index — leaving ALSA unable to open any PCM device.

| Property | Value |
|----------|-------|
| **Values** | ALSA PCM name (e.g., `plughw:CARD=vc4hdmi0,DEV=0`, `default`) |
| **Default** | `default` |
| **Precedence** | `HELIX_ALSA_DEVICE` env > `/sound/output_device` setting > `default` |
| **File** | `src/system/alsa_device_enum.cpp` (`resolve_alsa_device`) |

```bash
# Route UI sounds to an HDMI screen's built-in speaker
HELIX_ALSA_DEVICE=plughw:CARD=vc4hdmi0,DEV=0 ./build/bin/helix-screen
```

**Common case — BTT HDMI5 (and similar HDMI-audio screens):** the screen's built-in speaker rides HDMI audio. On a Pi CM4 the only cards are `vc4hdmi0`/`vc4hdmi1`, which enumerate as cards **1 and 2** — there is no card 0, so `default` fails (`snd_pcm_dmix_open: unable to open slave`). Identify the right card with `aplay -l`, confirm it plays with `aplay -D plughw:CARD=vc4hdmi0,DEV=0 <wav>`, then set this variable (or write an `/etc/asound.conf` that remaps `default`). The service also needs the `audio` supplementary group — see `config/helixscreen.service`.

**Combo devices:** USB devices that combine keyboard and trackpad (e.g., Logitech K400) present as a single evdev node. The same device path can be used by both mouse and keyboard input — LVGL filters events by type internally.

### `HELIX_KEYBOARD_DEVICE`

Override automatic USB keyboard device detection.

| Property | Value |
|----------|-------|
| **Values** | Device path (e.g., `/dev/input/event5`) |
| **Default** | Auto-detect (scans for `KEY_A` capability) |
| **Files** | `src/api/display_backend_drm.cpp`, `src/api/display_backend_fbdev.cpp` |

```bash
# Specify keyboard device explicitly
HELIX_KEYBOARD_DEVICE=/dev/input/event5 ./build/bin/helix-screen
```

**Auto-detection:** When unset, HelixScreen scans `/sys/class/input/event*/device/capabilities/key` for devices with `KEY_A` (bit 30) set. This distinguishes real keyboards from power buttons and other key-like devices.

### `HELIX_BACKLIGHT_DEVICE`

Control backlight device path or disable backlight control entirely.

| Property | Value |
|----------|-------|
| **Values** | Device path, `sysfs`, `allwinner`, `brightness`, or `"none"` to disable |
| **Default** | Auto-detect |
| **File** | `src/api/backlight_backend.cpp` |

```bash
# Disable backlight control (e.g., for external displays)
HELIX_BACKLIGHT_DEVICE=none ./build/bin/helix-screen

# Use specific backlight device
HELIX_BACKLIGHT_DEVICE=/sys/class/backlight/backlight-lvds ./build/bin/helix-screen

# Creality Sonic Pad: drive the `brightness` CLI tool instead of sysfs/ioctl.
# Use this when the screen brightness/blank doesn't respond on a Sonic Pad even
# though auto-detection picked a backend (issue #972).
HELIX_BACKLIGHT_DEVICE=brightness ./build/bin/helix-screen
```

### `HELIX_DISPLAY_ROTATION`

Override the display rotation angle. Takes highest priority over config file (`/display/rotate`) and CLI flags (`--rotate`).

| Property | Value |
|----------|-------|
| **Values** | `0`, `90`, `180`, `270` (degrees) |
| **Default** | `0` (no rotation) |
| **Files** | `src/application/display_manager.cpp` |

```bash
# Rotate display 90° (e.g., portrait display mounted landscape)
HELIX_DISPLAY_ROTATION=90 ./build/bin/helix-screen

# Rotate 180° (upside-down mount)
HELIX_DISPLAY_ROTATION=180 ./build/bin/helix-screen
```

**Priority order:**
1. `HELIX_DISPLAY_ROTATION` environment variable (highest)
2. `--rotate <degrees>` CLI flag
3. `/display/rotate` in `settings.json`
4. Default: `0` (no rotation)

**Note:** Software rotation is only supported on embedded backends (fbdev/DRM). On SDL (desktop dev), rotation is logged as a warning and skipped due to LVGL's DIRECT render mode limitation.

**Automatic backend switching:** When rotation is configured and the DRM backend cannot do hardware rotation, `DisplayManager` automatically switches to the fbdev backend before applying rotation. This means users get flicker-free rotation without needing to manually set `HELIX_DISPLAY_BACKEND=fbdev`. The DRM software rotation path (CPU pixel reversal in `drm_flush()`) still exists as a last resort if fbdev is unavailable.

**Touch auto-rotation:** On fbdev, touch coordinates are automatically rotated to match the display rotation for non-USB-HID devices (e.g., Goodix, sun4i_ts). USB HID touchscreens (e.g., BTT HDMI) report logical coordinates natively and are not transformed. `HELIX_TOUCH_SWAP_AXES` is still available as a manual override for edge cases.

### `HELIX_FORCE_ROTATION_PROBE`

Force the rotation probe to run on next startup, even if it has already run or a rotation is configured. Useful for testing the probe UI on SDL or re-running on a device.

| Property | Value |
|----------|-------|
| **Values** | `1` (force probe) |
| **Default** | Unset (probe only on first boot, fbdev only, no existing rotation) |
| **Files** | `src/application/application.cpp`, `src/application/display_manager.cpp` |

```bash
# Test the rotation probe on SDL (desktop)
HELIX_FORCE_ROTATION_PROBE=1 ./build/bin/helix-screen --test -vv

# Re-run probe on a device that already has rotation configured
HELIX_FORCE_ROTATION_PROBE=1 ./build/bin/helix-screen
```

**Rotation probe behavior:**
- On first boot with no configured rotation (fbdev only), HelixScreen cycles through 0°, 90°, 180°, 270° showing "Tap anywhere if this text is right-side up" for 5 seconds each
- Two-tap confirmation: after initial tap, shows "Tap again to confirm" (10s timeout) to prevent accidental selection
- Loops continuously until user confirms — does not exit after one cycle
- Saves the confirmed rotation to `/display/rotate` in `settings.json`
- Sets `/display/rotation_probed` flag so it doesn't re-run on subsequent boots
- Skips entirely if: rotation is already configured (config, env var, or CLI), or the probe has already run
- Runs after translations are loaded (Phase 8c) so probe strings are translatable via `lv_tr()`

### `HELIX_SCREEN_SIZE`

Override the screen resolution. Alternative to the `-s` / `--size` command-line flag, useful for persistent configuration via `helixscreen.env` or systemd `EnvironmentFile`.

| Property | Value |
|----------|-------|
| **Values** | Named preset: `micro`, `tiny`, `small`, `medium`, `large`, `xlarge` — or custom `WxH` format (e.g., `480x400`, `1920x1080`) |
| **Default** | Auto-detected from display hardware |
| **File** | `src/application/application.cpp` (via `EnvironmentConfig::get_screen_size()`) |
| **Priority** | The `-s` command-line flag takes precedence over this variable |

Named presets and their resolutions:

| Preset | Width | Height |
|--------|-------|--------|
| `micro` | 480 | 272 |
| `tiny` | 480 | 320 |
| `small` | 480 | 400 |
| `medium` | 800 | 480 |
| `large` | 1024 | 600 |
| `xlarge` | 1280 | 720 |

Custom `WxH` values are automatically classified into the nearest breakpoint based on height.

```bash
# Force small screen layout
HELIX_SCREEN_SIZE=small ./build/bin/helix-screen --test

# Custom resolution
HELIX_SCREEN_SIZE=1920x1080 ./build/bin/helix-screen --test

# In helixscreen.env (persistent):
HELIX_SCREEN_SIZE=medium
```

### `HELIX_DPI`

Override the display DPI (dots per inch). Useful for screens where spacing looks too large or too small at the auto-detected DPI.

| Property | Value |
|----------|-------|
| **Values** | `50` – `500` |
| **Default** | Auto-detected (LVGL default: `130`) |
| **File** | `scripts/helix-launcher.sh`, `src/system/cli_args.cpp` |

```bash
# Force 240 DPI (common for high-density small screens)
HELIX_DPI=240 ./build/bin/helix-screen

# In helixscreen.env (persistent):
HELIX_DPI=240
```

The launcher translates this to `--dpi=<value>` on the CLI. Can also be passed directly: `./build/bin/helix-screen --dpi 240`.

### `HELIX_COLOR_SWAP_RB`

Override the automatic R/B channel swap detection for BGR framebuffers. The LVGL fbdev driver auto-detects BGR layout from `fb_var_screeninfo.red.offset` / `blue.offset`, but some kernel drivers report incorrect offsets.

| Property | Value |
|----------|-------|
| **Values** | `1` (force swap ON), `0` (force swap OFF) |
| **Default** | Auto-detect from framebuffer vinfo |
| **File** | `src/api/display_backend_fbdev.cpp` |

```bash
# Force R/B swap for Allwinner R818 with BGR framebuffer
HELIX_COLOR_SWAP_RB=1 ./build/bin/helix-screen

# Disable auto-detected swap (if detection is wrong)
HELIX_COLOR_SWAP_RB=0 ./build/bin/helix-screen
```

**Symptoms of needing this:** Red images appear blue and blue images appear red. The green channel is unaffected. Common on some Allwinner SoCs with RGB parallel (40-pin) display interfaces.

**Fbdev only:** This setting only applies to the framebuffer backend. DRM and SDL backends handle color format natively.

### `HELIX_SDL_DISPLAY`

Select which monitor to use when running with SDL backend on multi-monitor systems.

| Property | Value |
|----------|-------|
| **Values** | Display index (`0`, `1`, `2`, ...) |
| **Default** | `0` (primary display) |
| **File** | `src/main.cpp` |

```bash
# Run on second monitor
HELIX_SDL_DISPLAY=1 ./build/bin/helix-screen
```

### `HELIX_SDL_XPOS` / `HELIX_SDL_YPOS`

Position the SDL window at exact screen coordinates.

| Property | Value |
|----------|-------|
| **Values** | Pixel coordinates (integers) |
| **Default** | Centered on selected display |
| **File** | `src/main.cpp` |

```bash
# Position window at specific coordinates
HELIX_SDL_XPOS=100 HELIX_SDL_YPOS=200 ./build/bin/helix-screen
```

---

## Touch Calibration

### Linear Calibration (env vars)

Simple axis range mapping via LVGL's built-in calibration. Use for devices with known linear offset/scale.

| Variable | Description | Default |
|----------|-------------|---------|
| `HELIX_TOUCH_MIN_X` | Minimum raw X value (maps to screen left) | Auto-detect |
| `HELIX_TOUCH_MAX_X` | Maximum raw X value (maps to screen right) | Auto-detect |
| `HELIX_TOUCH_MIN_Y` | Minimum raw Y value (maps to screen top) | Auto-detect |
| `HELIX_TOUCH_MAX_Y` | Maximum raw Y value (maps to screen bottom) | Auto-detect |
| `HELIX_TOUCH_SWAP_AXES` | Swap X/Y axes (set to "1" to enable). Overrides auto-detection. | Disabled (auto-detected) |

**Usage Notes:**
- All four min/max variables must be set together for calibration to apply
- To invert an axis, swap the min/max values (e.g., `MIN_Y=3200 MAX_Y=900` inverts Y)
- These values override the kernel-reported axis ranges from `EVIOCGABS`
- `HELIX_TOUCH_SWAP_AXES` is now primarily a manual override — the calibration wizard auto-detects swapped axes and saves the flag to config (`/input/calibration/swap_axes`). The env var takes priority over auto-detection if both are set

**Example:**
```bash
# AD5M resistive touchscreen with inverted Y axis
export HELIX_TOUCH_MIN_X=500
export HELIX_TOUCH_MAX_X=3580
export HELIX_TOUCH_MIN_Y=3200  # Higher value = screen top (inverted)
export HELIX_TOUCH_MAX_Y=900
./build/bin/helix-screen
```

### Affine Calibration (config file)

For precise calibration including rotation and skew correction, use the touch calibration wizard. The wizard computes a 6-coefficient affine transform and saves it to the config file at `input.calibration.{a,b,c,d,e,f}`. (The legacy `display.calibration` path is auto-migrated to `input.calibration` on load — see `docs/devel/CONFIG_MIGRATION.md`.)

**Affine transform formula:**
```
screen_x = a * touch_x + b * touch_y + c
screen_y = d * touch_x + e * touch_y + f
```

The calibration wizard is automatically presented during first-run setup on framebuffer devices. It can also be triggered manually from Settings or via the `HELIX_TOUCH_CALIBRATE` environment variable.

### `HELIX_TOUCH_CALIBRATE`

Force the touch calibration wizard to appear on startup. Set to any value to enable.

| Variable | Description | Default |
|----------|-------------|---------|
| `HELIX_TOUCH_CALIBRATE` | Force touch calibration wizard on startup | Disabled |

**Equivalent to:** `--calibrate-touch` CLI flag or `/input/force_calibration` config option.

**Example:**
```bash
# One-shot: force calibration on next launch
HELIX_TOUCH_CALIBRATE=1 ./build/bin/helix-screen

# Or add to helixscreen.env for persistent use
echo "HELIX_TOUCH_CALIBRATE=1" >> ~/helixscreen/config/helixscreen.env
```

**Note:** There are no environment variable overrides for affine calibration coefficients. Edit the config file directly or use the calibration wizard.

### Touch Jitter Filter

Suppresses small coordinate jitter from noisy touch controllers (e.g., Goodix GT9xx) that would otherwise cause stationary taps to be misinterpreted as scroll/swipe gestures.

| Variable | Description | Default |
|----------|-------------|---------|
| `HELIX_TOUCH_JITTER` | Dead-zone threshold in pixels. Coordinate changes within this distance are suppressed. | `5` |
| `HELIX_SCROLL_GUARD` | Enables the post-scroll click guard (default 80 ms cooldown after a scroll ends). | (unset; preset-controlled) |
| `HELIX_SCROLL_GUARD_COOLDOWN_MS` | Cooldown window for `HELIX_SCROLL_GUARD`, in milliseconds. Clamped to 20–500. | `80` |

**How it works:** When a finger is pressed, the filter records the initial position. Subsequent coordinate reports within the dead zone are snapped back to the last stable position. Once movement exceeds the threshold, the new position becomes the anchor. On release, the last stable position is reported.

**Config file equivalents:**
- `/input/jitter_threshold` (integer, default `5`, set to `0` to disable)
- `/input/scroll_guard` (boolean, default `false`; FlashForge AD5M and AD5X presets set it to `true`)
- `/input/scroll_guard_cooldown_ms` (integer, default `80`, range 20–500 — cooldown window when `scroll_guard` is enabled)
- `/input/scroll_limit` (integer, default `10`, range 1–20 — pixels before LVGL commits to scrolling)
- `/input/scroll_throw` (integer, default `25`, range 5–50 — scroll momentum decay; higher = faster stop)

Migration history: `jitter_threshold` was 15 before config v3, then reset to 5 in the v2→v3 migration because the larger dead zone added perceptible drag to intentional gestures. The filter is cheap enough to stay on by default; most users never touch it.

**Example:**
```bash
# Increase threshold for a very noisy touchscreen
HELIX_TOUCH_JITTER=25 ./build/bin/helix-screen

# Disable the jitter filter entirely
HELIX_TOUCH_JITTER=0 ./build/bin/helix-screen

# Force the post-scroll click guard on for a non-FlashForge panel
HELIX_SCROLL_GUARD=1 ./build/bin/helix-screen

# Stretch the post-scroll cooldown if 80 ms isn't enough for your controller
HELIX_SCROLL_GUARD=1 HELIX_SCROLL_GUARD_COOLDOWN_MS=150 ./build/bin/helix-screen
```

---

## G-Code Viewer

### `HELIX_GCODE_MODE`

Force the G-code preview rendering mode.

| Property | Value |
|----------|-------|
| **Values** | `2D`, `3D` |
| **Default** | `2D` |
| **Files** | `src/ui_gcode_viewer.cpp`, `src/ui_panel_print_status.cpp`, `src/ui_panel_gcode_test.cpp` |

```bash
# Force 2D layer view
HELIX_GCODE_MODE=2D ./build/bin/helix-screen
```

### `HELIX_FORCE_GCODE_MEMORY_FAIL`

Force the G-code memory safety check to fail, simulating a memory-constrained device like AD5M. Useful for testing thumbnail fallback behavior without deploying to embedded hardware.

| Property | Value |
|----------|-------|
| **Values** | `1` (force failure), unset (normal behavior) |
| **Default** | Unset (normal memory checking) |
| **File** | `src/memory_utils.cpp` |

```bash
# Force memory check to fail - viewer falls back to thumbnail mode
HELIX_FORCE_GCODE_MEMORY_FAIL=1 ./build/bin/helix-screen --test -p print-status -vv
```

**Use case:** Testing that the thumbnail displays immediately when G-code rendering is unavailable, without needing to deploy to memory-constrained hardware.

### `HELIX_GCODE_STREAMING`

Control G-code streaming mode for memory-efficient loading of large files. Streaming loads layers on-demand instead of the entire file at once, enabling 10MB+ G-code files on memory-constrained devices like AD5M.

| Property | Value |
|----------|-------|
| **Values** | `on` (always stream), `off` (always full load), `auto` (calculate based on RAM) |
| **Default** | `auto` |
| **Config** | `gcode_viewer.streaming_mode` in `settings.json` |
| **File** | `src/gcode_streaming_config.cpp` |

**Priority order:**
1. Environment variable (highest) - for testing/debugging
2. Config file setting - for user preference
3. Auto-detection based on available RAM

```bash
# Force streaming mode (useful for testing streaming behavior)
HELIX_GCODE_STREAMING=on ./build/bin/helix-screen --test -p print-status -vv

# Force full load mode (may crash on large files with low RAM!)
HELIX_GCODE_STREAMING=off ./build/bin/helix-screen --test --gcode-file large.gcode -vv

# Use auto-detection (default)
HELIX_GCODE_STREAMING=auto ./build/bin/helix-screen --test -p print-status -vv
```

**Auto-detection thresholds** (at 40% RAM threshold, 15x expansion factor):
| Available RAM | Streaming kicks in at |
|---------------|----------------------|
| 47 MB (AD5M)  | ~1.25 MB |
| 256 MB        | ~6.8 MB |
| 1 GB          | ~27 MB |
| 4 GB          | ~107 MB |

**Related config options:**
- `gcode_viewer.streaming_mode`: `"auto"`, `"on"`, or `"off"`
- `gcode_viewer.streaming_threshold_percent`: 1-90 (default 40)

### `HELIX_SSAO`

Control enhanced 2D G-code shading. When enabled (default), the 2D layer renderer applies per-segment directional lighting, anti-aliased line drawing (Wu's algorithm), and a silhouette outline post-process for improved depth perception.

| Property | Value |
|----------|-------|
| **Values** | `0` (disable), unset (enabled by default) |
| **Default** | Enabled |
| **File** | `src/ui/ui_gcode_viewer.cpp`, `src/rendering/gcode_layer_renderer.cpp` |

```bash
# Disable enhanced shading (use original flat rendering)
HELIX_SSAO=0 ./build/bin/helix-screen --test -p gcode-test -vv
```

**What it adds:**
- **Normal-based directional shading:** Each extrusion segment is shaded based on its surface normal relative to a fixed upper-left light source, creating visible surface variation
- **Anti-aliased lines:** Wu's line algorithm replaces Bresenham for smoother edges
- **Silhouette outline:** 1px darkened border on the alpha boundary of the model for edge definition

Performance impact is minimal (~2ms post-process pass for the outline, negligible overhead for AA lines and normal shading).

Can also be toggled programmatically via `ui_gcode_viewer_set_ssao_enabled()`.

---

## Bed Mesh

### `HELIX_BED_MESH_2D`

Force the bed mesh visualization to use 2D heatmap mode instead of 3D surface rendering.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Off (3D surface when available) |
| **File** | `src/ui_bed_mesh.cpp` |

```bash
# Force 2D heatmap visualization
HELIX_BED_MESH_2D=1 ./build/bin/helix-screen
```

---

## Mock & Testing

These variables control the mock printer simulation, useful for development and testing without a real printer.

### `HELIX_AMS_GATES`

Set the number of filament gates in the mock AMS (Automatic Material System).

| Property | Value |
|----------|-------|
| **Values** | `1` to `16` |
| **Default** | `4` |
| **File** | `src/main.cpp` |

```bash
# Simulate 8-slot AMS
HELIX_AMS_GATES=8 ./build/bin/helix-screen --test

# Simulate 16-slot MMU
HELIX_AMS_GATES=16 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_AMS`

Select the mock AMS topology/type.

| Property | Value |
|----------|-------|
| **Values** | `afc`, `toolchanger` / `tc`, `mixed`, `multi` |
| **Default** | Happy Hare, LINEAR, 4 slots |
| **File** | `src/printer/ams_backend.cpp` |

| Value | What it simulates |
|-------|-------------------|
| *(unset)* | Happy Hare, LINEAR, 4 slots (default constructor) |
| `afc` | AFC Box Turtle, HUB, 4 slots |
| `toolchanger` / `tc` | Tool Changer, PARALLEL topology |
| `mixed` | Box Turtle + 2x OpenAMS, 6 tools |
| `multi` | Box Turtle (4 slots) + Night Owl (2 slots), single toolhead |
| `htlf_toolchanger` | AFC HTLF + Toolchanger: 4 HTLF lanes (2 direct, 2 hub→shared extruder) + 3 standalone toolheads. Tests MIXED topology. Aliases: `htlf_tc`, `htlf` |

```bash
# Simulate AFC Box Turtle
HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test

# Simulate toolchanger
HELIX_MOCK_AMS=toolchanger ./build/bin/helix-screen --test

# Simulate mixed topology (BT + 2x OpenAMS)
HELIX_MOCK_AMS=mixed ./build/bin/helix-screen --test

# Simulate multi-unit (Box Turtle + Night Owl, 6 slots, single toolhead)
HELIX_MOCK_AMS=multi ./build/bin/helix-screen --test
```

**Multi-extruder and tool testing:** Setting `HELIX_MOCK_AMS=toolchanger` also creates multiple tool definitions and extruders in the mock environment. Multiple extruders (extruder, extruder1, etc.) and tools are auto-discovered from Klipper objects at runtime, so no separate env var is needed to control extruder count. The toolchanger mock provides a complete multi-tool, multi-extruder test environment.

### `HELIX_MOCK_AMS_STATE`

Select the mock AMS visual scenario.

| Property | Value |
|----------|-------|
| **Values** | `idle`, `loading`, `error`, `bypass` |
| **Default** | `idle` (slot 0 loaded, slot 3 empty, others available) |
| **File** | `src/printer/ams_backend.cpp` |

| Value | What it shows |
|-------|---------------|
| *(unset)* / `idle` | Default idle state |
| `loading` | Active load in progress with realistic segment animation |
| `error` | Slot errors visible; buffer fault also shown when combined with `afc` mode |
| `bypass` | Bypass mode active |

```bash
# Show error states (slot errors + buffer fault)
HELIX_MOCK_AMS_STATE=error ./build/bin/helix-screen --test

# Show realistic loading animation
HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test

# Show bypass mode
HELIX_MOCK_AMS_STATE=bypass ./build/bin/helix-screen --test

# Combine with topology selection
HELIX_MOCK_AMS=afc HELIX_MOCK_AMS_STATE=error ./build/bin/helix-screen --test
HELIX_MOCK_AMS=mixed HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test
```


### `HELIX_MOCK_DRYER`

Enable filament dryer simulation in mock mode.

| Property | Value |
|----------|-------|
| **Values** | `1` or `true` |
| **Default** | Disabled |
| **File** | `src/ams_backend.cpp` |

```bash
# Enable mock dryer
HELIX_MOCK_DRYER=1 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DRYER_SPEED`

Speed multiplier for dryer simulation (for faster testing).

| Property | Value |
|----------|-------|
| **Values** | Integer multiplier (e.g., `2` = 2x speed) |
| **Default** | `1` (real-time) |
| **File** | `src/ams_backend_mock.cpp` |

```bash
# Run dryer simulation at 10x speed
HELIX_MOCK_DRYER=1 HELIX_MOCK_DRYER_SPEED=10 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_SPOOLMAN`

Enable or disable mock Spoolman integration. When disabled, `get_spoolman_status()` reports as disconnected.

| Property | Value |
|----------|-------|
| **Values** | `0` or `off` to disable; any other value keeps enabled |
| **Default** | Enabled (mock Spoolman always connected in test mode) |
| **File** | `src/main.cpp` |

```bash
# Disable mock Spoolman to test "no Spoolman" scenarios
HELIX_MOCK_SPOOLMAN=0 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_FILAMENT_SENSORS`

Configure custom filament sensor configurations for testing.

| Property | Value |
|----------|-------|
| **Values** | Comma-separated `type:name` pairs, or `"none"` |
| **Default** | Single runout switch sensor |
| **File** | `src/moonraker_client_mock.cpp` |

**Sensor Types:**
- `switch` - Simple on/off runout switch
- `motion` - Motion-based encoder sensor

```bash
# Multiple sensors
HELIX_MOCK_FILAMENT_SENSORS="switch:fsensor,motion:encoder" ./build/bin/helix-screen --test

# No sensors
HELIX_MOCK_FILAMENT_SENSORS=none ./build/bin/helix-screen --test
```

### `HELIX_MOCK_FILAMENT_STATE`

Set the initial state of filament sensors.

| Property | Value |
|----------|-------|
| **Values** | `sensor_name:state` (e.g., `fsensor:empty`, `fsensor:detected`) |
| **Default** | Detected |
| **File** | `src/moonraker_client_mock.cpp` |

```bash
# Start with empty filament sensor
HELIX_MOCK_FILAMENT_STATE="fsensor:empty" ./build/bin/helix-screen --test
```

### `HELIX_QIDI_BOX_WRITE`

Enable the write-path on the QIDI Box AMS backend. Read-only state mirroring is always available, but `load_filament`, `unload_filament`, `change_tool`, and `set_tool_mapping` are gated behind this flag so unvalidated gcode never reaches live hardware in production builds. Intended for field testing against real hardware (issue [#954](https://github.com/prestonbrown/helixscreen/issues/954)).

| Property | Value |
|----------|-------|
| **Values** | `1` / any non-empty non-`0` string (enable), unset / `0` (default — disabled) |
| **Default** | Unset |
| **File** | `src/printer/ams_backend_qidi.cpp` |

```bash
# Field-testing build with QIDI Box write-path active
HELIX_QIDI_BOX_WRITE=1 ./build/bin/helix-screen

# Or persistent: add to ~/helixscreen/config/helixscreen.env
HELIX_QIDI_BOX_WRITE=1
```

When disabled (the default), all write operations return `not_supported` with a message pointing the caller at this flag. Read-only operations (state queries, slot info, system info) work in either mode.

### `HELIX_FORCE_RUNOUT_MODAL`

Force the filament runout guidance modal to appear even when an AMS/MMU system is present. Normally, runout modals are suppressed for AMS systems because filament runout during swaps is expected behavior.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (normal behavior) |
| **Default** | Unset (modal suppressed with AMS) |
| **File** | `src/system/runtime_config.cpp` |

```bash
# Force runout modal with real AMS system
HELIX_FORCE_RUNOUT_MODAL=1 ./build/bin/helix-screen

# In test mode, use --no-ams instead (simpler)
./build/bin/helix-screen --test --no-ams
```

**Note:** In test mode, a mock AMS is created by default (4 gates). Use `--no-ams` flag to disable the mock AMS, which enables runout modal testing without needing this environment variable.

### `MOCK_EMPTY_POWER`

Return an empty power devices list from mock Moonraker API.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Populated power device list |
| **File** | `src/moonraker_api_mock.cpp` |

```bash
# Simulate printer with no controllable power devices
MOCK_EMPTY_POWER=1 ./build/bin/helix-screen --test
```

---

## UI Automation

These variables enable automated testing and CI/CD workflows.

### `HELIX_AUTO_QUIT_MS`

Automatically quit the application after a specified duration.

| Property | Value |
|----------|-------|
| **Values** | `100` to `3600000` (milliseconds) |
| **Default** | No timeout (run indefinitely) |
| **File** | `src/main.cpp` |

```bash
# Quit after 5 seconds
HELIX_AUTO_QUIT_MS=5000 ./build/bin/helix-screen --test

# CI test run (3 second timeout)
HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p home
```

### `HELIX_AUTO_SCREENSHOT`

Automatically capture a screenshot before quitting (use with `HELIX_AUTO_QUIT_MS`).

| Property | Value |
|----------|-------|
| **Values** | `1` (enable) |
| **Default** | Disabled |
| **File** | `src/main.cpp` |

```bash
# Automated screenshot capture
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p motion
```

### `HELIX_BENCHMARK`

Enable frame counting and FPS reporting for performance testing.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Disabled |
| **File** | `src/main.cpp` |

```bash
# Run performance benchmark
HELIX_BENCHMARK=1 HELIX_AUTO_QUIT_MS=10000 ./build/bin/helix-screen --test
```

---

## Calibration Auto-Start

These variables auto-start calibration procedures for testing purposes.

### `INPUT_SHAPER_AUTO_START`

Auto-start X-axis input shaper calibration when the panel loads.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Disabled |
| **File** | `src/ui_panel_input_shaper.cpp` |

```bash
# Test input shaper panel with auto-start
INPUT_SHAPER_AUTO_START=1 ./build/bin/helix-screen --test -p input-shaper
```

### `SCREWS_AUTO_START`

Auto-start bed screw probing when the screws tilt panel loads.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Disabled |
| **File** | `src/ui_panel_screws_tilt.cpp` |

```bash
# Test screws panel with auto-start
SCREWS_AUTO_START=1 ./build/bin/helix-screen --test -p screws-tilt
```

---

## Development

### `HELIX_HOT_RELOAD`

Enable XML hot reload for live UI editing. When enabled, a background thread polls `ui_xml/` and `ui_xml/components/` every 500ms for file changes. Modified XML components are automatically unregistered and re-registered with LVGL — no restart needed.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Disabled (zero overhead in production) |
| **File** | `src/system/runtime_config.cpp`, `src/application/xml_hot_reloader.cpp` |

```bash
# Enable hot reload during development
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv
```

**How it works:**
1. On startup, scans `ui_xml/` and `ui_xml/components/` and records file modification times
2. Every 500ms, checks all tracked XML files for mtime changes
3. When a change is detected, queues an `lv_xml_component_unregister()` + `lv_xml_register_component_from_file()` on the LVGL main thread
4. Log output confirms the reload: `[HotReload] Reloaded: home_panel (0.4ms)`

**Limitations:**
- **Existing widgets are not rebuilt.** After a reload, navigate away from the current panel and back to see the updated layout. Future versions may add automatic panel refresh.
- **New files are not detected.** Only files present when the app starts are tracked. Adding a new XML file requires a restart.
- **Component re-registration only.** If the XML change requires new subjects, callbacks, or C++ code, a full rebuild + restart is needed.

**Typical workflow:**
```bash
# Terminal 1: Run with hot reload
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv

# Terminal 2: Edit XML, save, watch the log
vim ui_xml/home_panel.xml
# [HotReload] Reloaded: home_panel (0.3ms)

# Switch panels in the UI to see the new layout
```

---

## Debugging

### `HELIX_DEBUG_SUBJECTS`

Enable verbose subject debugging with stack traces. When LVGL subject type mismatches occur (e.g., trying to read an INT from a STRING subject), this enables detailed diagnostics including the subject name, registration location, and a full stack trace.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Disabled |
| **CLI equivalent** | `--debug-subjects` |
| **File** | `src/system/runtime_config.cpp` |

```bash
# Enable via environment variable
HELIX_DEBUG_SUBJECTS=1 ./build/bin/helix-screen -vv

# Or via command-line flag (equivalent)
./build/bin/helix-screen --debug-subjects -vv
```

**Example output when a type mismatch occurs:**
```
[warning] [LVGL] Subject type is not LV_SUBJECT_TYPE_INT (ptr=0x155008f30, type=7)
[warning]   -> Expected type: LV_SUBJECT_TYPE_INT
[warning]   -> Subject: "print_progress" (STRING) registered at printer_state.cpp:226
[warning]   Stack trace:
[warning]     #0 HomePanel::update_status() + 108
[warning]     #1 HomePanel::on_state_changed(int) + 140
```

**Use cases:**
- Debugging XML binding issues (e.g., `bind_text` on an INT subject)
- Finding subject initialization order problems
- Tracing observer callbacks that fire before subjects are ready

### `HELIX_DEBUG_TOUCH`

Unified touch debugging variable that enables both touch point visualization and detailed calibration logging.

**Touch visualization:** A ripple effect is drawn at each touch point, showing exactly where the system registers touches. Useful for diagnosing touch accuracy issues, verifying calibration, and identifying UI elements that absorb click events.

**Calibration logging:** Forces comprehensive logging of the entire calibration pipeline at WARN level, making it visible regardless of the configured log level. Logs include: raw touch samples, median computation, affine matrix calculation, axis swap detection, validation checks with residuals, and runtime coordinate transforms (throttled).

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Disabled |
| **CLI equivalent** | `--debug-touches` |
| **Files** | `src/system/runtime_config.cpp`, `include/touch_calibration.h`, `src/ui/touch_calibration.cpp`, `src/ui/touch_calibration_panel.cpp`, `src/api/display_backend_fbdev.cpp` |
| **Legacy alias** | `HELIX_DEBUG_TOUCHES` (still accepted) |

```bash
# Enable via environment variable
HELIX_DEBUG_TOUCH=1 ./build/bin/helix-screen -vv

# Or via command-line flag (equivalent)
./build/bin/helix-screen --debug-touches -vv

# Combine with test mode for desktop debugging
./build/bin/helix-screen --test --debug-touches -vv

# On device: add to helixscreen.env
HELIX_DEBUG_TOUCH=1
```

**Log prefix:** Calibration messages use `[TouchDebug]` prefix for easy filtering:
```bash
# Filter touch debug messages from log (path varies by platform; see LOGGING.md)
grep '\[TouchDebug\]' /var/log/messages        # AD5M, Snapmaker U1 (syslog → file)
logread | grep '\[TouchDebug\]'                # K1/K1C/K2/CC1/AD5X (BusyBox in-RAM syslog)
journalctl -u helixscreen | grep '\[TouchDebug\]'  # Pi/x86 (systemd journal)
```

**What calibration logging shows:**
- Each raw touch sample with saturation detection
- Median point computation (all valid samples, selected median)
- Affine matrix computation (all input points, determinant, coefficients)
- Axis swap detection (cross-coupling ratios, swap decision)
- Validation checks (coefficient bounds, back-transform residuals, center mapping)
- Runtime coordinate transforms (throttled: first touch, then every 50th)
- Calibration loaded from config at startup
- Device detection details (name, phys, capabilities, calibration decision)

**Use cases:**
- Verifying touch calibration accuracy
- Diagnosing buttons or UI elements that don't respond to taps
- Identifying overlapping UI elements that absorb click events
- Confirming extended click areas are working correctly

**When to use:** Ask users with calibration issues to add `HELIX_DEBUG_TOUCH=1` to their `helixscreen.env` file and reproduce the issue. The `[TouchDebug]` messages in syslog/log will show exactly what the calibration pipeline sees, and the ripple visualization confirms where touches land visually.

---

## Deployment

### `HELIX_DATA_DIR`

Override the runtime asset directory. When set, the application `chdir()`s to this path at startup so that all relative asset paths (`ui_xml/`, `assets/`, `config/`) resolve correctly. Use this when assets are installed to a different location than the default (e.g., due to storage constraints on embedded devices, or a custom filesystem layout).

| Property | Value |
|----------|-------|
| **Values** | Absolute path to directory containing `ui_xml/`, `assets/`, `config/` |
| **Default** | Auto-detect from executable path |
| **File** | `src/application/application.cpp` |

```bash
# Assets on a separate partition
HELIX_DATA_DIR=/mnt/data/helixscreen /usr/bin/helix-screen

# FHS-style installation with split layout
HELIX_DATA_DIR=/usr/share/helixscreen /usr/bin/helix-screen

# Systemd service
Environment="HELIX_DATA_DIR=/opt/helixscreen"
```

**Required directory structure:**
```
$HELIX_DATA_DIR/
  ├── ui_xml/          # XML layout files
  ├── assets/          # Images (runtime-loaded only)
  └── config/          # Default configuration
```

**Note:** Fonts compiled into the binary (e.g., `mdi_icons_64`, `noto_sans_*`) work regardless of this setting. Only runtime-loaded files (XML, images) require the data directory.

---

## Logging & Startup

### `HELIX_LOG_LEVEL`

Set the log verbosity level. Preferred over the legacy `HELIX_DEBUG` variable.

| Property | Value |
|----------|-------|
| **Values** | `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off` |
| **Default** | `warn` |
| **File** | `scripts/helix-launcher.sh`, `src/system/cli_args.cpp` |

```bash
# In helixscreen.env (persistent, recommended for deployed systems):
HELIX_LOG_LEVEL=debug

# Or inline for one-off debugging:
HELIX_LOG_LEVEL=trace ./build/bin/helix-screen
```

The launcher translates this to `--log-level=<value>` on the CLI. Equivalent CLI flags: `-v` (info), `-vv` (debug), `-vvv` (trace), or `--log-level=<level>`.

**Where structured logs land** depends on the platform — `Auto` resolves to systemd-journal on Pi/x86 (with `journalctl -u helixscreen`), to BusyBox syslog on K1/K2/CC1/AD5X (with `logread`), or to persistent syslog file on AD5M and Snapmaker U1 (`/var/log/messages`). The launcher's shell-stdout-redirect file (`launcher.log`) is a separate, smaller stream that captures startup echoes and crash output — see [LOGGING.md](LOGGING.md#log-destinations--retrieval) for the full map.

**Priority order:**
1. CLI `--log-level` / `-v` flags (highest)
2. `HELIX_LOG_LEVEL` env var
3. `HELIX_DEBUG=1` (legacy, maps to debug level)
4. Config `log_level` in `settings.json`
5. Default: `warn`

### `HELIX_SKIP_SPLASH`

Skip the splash screen on startup. Useful for debugging startup issues or faster iteration.

| Property | Value |
|----------|-------|
| **Values** | `1` (skip) or unset (show splash) |
| **Default** | Unset (splash shown) |
| **File** | `scripts/helix-launcher.sh` |

```bash
# In helixscreen.env:
HELIX_SKIP_SPLASH=1
```

The launcher translates this to `--skip-splash` on the CLI.

---

## Data Paths

### `HELIX_CACHE_DIR`

Override the base directory for all HelixScreen cache/temp files (thumbnails, screenshots, gcode temp files).

| Property | Value |
|----------|-------|
| **Values** | Absolute directory path |
| **Default** | Auto-detected per platform |
| **File** | `src/app_globals.cpp` |

When set, all cache subdirectories are created under `$HELIX_CACHE_DIR/<subdir>`. Platform hooks set this automatically:
- **AD5M**: `/data/helixscreen/cache` (5.8GB ext4 partition)
- **K1/K2**: `/usr/data/helixscreen/cache`

```bash
# Custom cache location
HELIX_CACHE_DIR=/mnt/storage/helix-cache ./build/bin/helix-screen

# AD5M (set automatically by platform hooks)
export HELIX_CACHE_DIR="/data/helixscreen/cache"
```

**Resolution chain** (first match wins):
1. `HELIX_CACHE_DIR` env var
2. Config `/cache/base_directory`
3. Platform-specific compile-time default
4. `XDG_CACHE_HOME/helix/`
5. `$HOME/.cache/helix/`
6. `/var/tmp/helix_`
7. `/tmp/helix_` (last resort)

### `XDG_DATA_HOME`

XDG base directory specification for application data storage.

| Property | Value |
|----------|-------|
| **Values** | Directory path |
| **Default** | `~/.local/share` |
| **File** | `src/logging_init.cpp` |

HelixScreen stores logs and data in `$XDG_DATA_HOME/helixscreen/`.

### `HOME`

User home directory (standard Unix variable).

| Property | Value |
|----------|-------|
| **Values** | Directory path |
| **Default** | (from system) |
| **File** | `src/logging_init.cpp` |

Used as fallback when `XDG_DATA_HOME` is not set.

---

## Build-Time Variables

These are set during compilation via the Makefile system.

| Variable | Purpose | Source |
|----------|---------|--------|
| `HELIX_VERSION` | Version string | `VERSION.txt` |
| `HELIX_VERSION_MAJOR` | Major version number | Parsed from version |
| `HELIX_VERSION_MINOR` | Minor version number | Parsed from version |
| `HELIX_VERSION_PATCH` | Patch version number | Parsed from version |
| `HELIX_GIT_HASH` | Git commit hash (short) | `git describe` |

---

## Preprocessor Flags

These are set at compile time to enable/disable features:

| Flag | Purpose |
|------|---------|
| `HELIX_DISPLAY_SDL` | Enable SDL2 display backend |
| `HELIX_DISPLAY_DRM` | Enable DRM display backend |
| `HELIX_DISPLAY_FBDEV` | Enable framebuffer display backend |
| `HELIX_ENABLE_OPENGLES` | Enable OpenGL ES support for 3D rendering |
| `HELIX_HAS_SYSTEMD` | Enable systemd integration |

---

## Shell Script Variables

### Screenshot Script (`scripts/screenshot.sh`)

| Variable | Purpose | Default |
|----------|---------|---------|
| `HELIX_SCREENSHOT_DISPLAY` | Display number for multi-monitor | `1` |
| `HELIX_SCREENSHOT_OPEN` | Auto-open in Preview (macOS) | Disabled |

```bash
# Take screenshot on display 0 and open it
HELIX_SCREENSHOT_DISPLAY=0 HELIX_SCREENSHOT_OPEN=1 ./scripts/screenshot.sh helix-screen test-output
```

---

## Systemd Service Configuration

When running as a systemd service, environment variables are set in the service file:

```ini
# /etc/systemd/system/helixscreen.service
[Service]
Environment="HELIX_DISPLAY_BACKEND=drm"
# Environment="HELIX_DATA_DIR=/usr/share/helixscreen"
```

See `docs/user/CONFIGURATION.md` for systemd deployment details.

---

## Common Usage Patterns

### Development Testing

```bash
# Basic mock testing
./build/bin/helix-screen --test -vv

# Test specific panel with verbose logging
./build/bin/helix-screen --test -p motion -vv

# Multi-slot AMS testing
HELIX_AMS_GATES=8 ./build/bin/helix-screen --test -p filament
```

### CI/CD Screenshots

```bash
# Automated panel screenshots
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p home
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p motion
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p settings
```

### Performance Benchmarking

```bash
# Run 10-second benchmark
HELIX_BENCHMARK=1 HELIX_AUTO_QUIT_MS=10000 ./build/bin/helix-screen --test -p home -v
```

### Hardware Override (Embedded)

```bash
# Override all device paths
HELIX_DISPLAY_BACKEND=drm \
HELIX_DRM_DEVICE=/dev/dri/card1 \
HELIX_TOUCH_DEVICE=/dev/input/event2 \
HELIX_MOUSE_DEVICE=/dev/input/event4 \
HELIX_KEYBOARD_DEVICE=/dev/input/event5 \
HELIX_BACKLIGHT_DEVICE=/sys/class/backlight/lcd \
./build/bin/helix-screen
```

---

## See Also

- [Development Guide](DEVELOPMENT.md) - Daily development workflow
- [Build System](BUILD_SYSTEM.md) - Build configuration
- [Testing Guide](TESTING.md) - Test infrastructure
- [User Configuration](user/CONFIGURATION.md) - End-user setup

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Snapmaker U1 Support

HelixScreen supports the Snapmaker U1 toolchanger as an alternative touchscreen UI. The U1 runs Klipper with Moonraker on a Rockchip ARM64 SoC, and HelixScreen can replace the stock display interface when deployed via SSH.

## Hardware

| Spec | Value |
|------|-------|
| SoC | Rockchip RK3562 — quad Cortex-A53 @ 2GHz (aarch64) |
| GPU | Mali-G52 2EE (OpenGL ES 3.2) |
| RAM | 961MB |
| Display | 3.5" 480x320 32bpp capacitive touch, DRM/KMS (`/dev/dri/card0`, rockchipdrmfb) |
| Touch Controller | TLSC6x capacitive (`tlsc6x_touch` on `/dev/input/event0`) |
| Storage | 28GB eMMC (`/userdata` ext4 persistent, SquashFS rootfs read-only overlay) |
| Recovery | A/B firmware slots + Rockchip MaskRom (unbrickable) |
| Firmware | Klipper + Moonraker |
| OS | Debian Trixie (ARM64) |
| Drivers | TMC2240 steppers |
| Filament | 4-channel RFID reader (FM175xx), OpenSpool NTAG215/216 |
| Camera | MIPI CSI + USB (Rockchip MPP/VPU) |
| Toolheads | 4 independent heads (SnapSwap system) |
| Max Speed | 500mm/s |

### SnapSwap Toolchanger

The U1 is a 4-toolhead color printer. Each head has its own nozzle, extruder, heater, and filament sensor. Tool changes take approximately 5 seconds with no purging required.

The U1 does **not** use the standard [viesturz/klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) module. Instead, it uses native multi-extruder with custom Klipper extensions. Extruders are named `extruder`, `extruder1`, `extruder2`, `extruder3` with custom state fields (`park_pin`, `active_pin`, `activating_move`, `state`). HelixScreen has a dedicated `AmsBackendSnapmaker` that tracks tool state, RFID filament data, and supports tool switching via `T0`–`T3` gcodes.

## Firmware Requirements

HelixScreen targets the community **PAXX Extended Firmware** for the U1 — it provides the SSH access HelixScreen needs to deploy. Stock Snapmaker firmware is not supported.

| Extended Firmware | Status | Notes |
|-------------------|--------|-------|
| 1.3.x | **Tested** — primary development target | Ships `/etc/init.d/S99screen`, which launches the stock UI binary `/usr/bin/gui`. |
| 1.4.x | **Supported, untested** — expected to work | `S99screen` was removed; the stock UI is launched from a relocated path. HelixScreen no longer depends on that script name (see below). |

Source: [paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware).

### How HelixScreen takes over the display

The U1 root filesystem is a read-only SquashFS with a writable OverlayFS upper on `/oem`. `/etc/init.d/S01aoverlayfs` wipes that upper on every boot **unless `/oem/.debug` exists**, so the installer touches `/oem/.debug` to make its changes persist. (This overlay/`.debug` mechanism is byte-identical between firmware 1.3 and 1.4.)

To own the display, the installer:

1. Installs a HelixScreen launcher at `/etc/init.d/S99screen` — patching the stock script on 1.3, creating it on 1.4 — that starts HelixScreen at boot.
2. **Disables the stock UI binary itself** with `chmod -x /usr/bin/gui`. This is launcher-independent: it does not matter whether the stock UI is started by 1.3's `S99screen` or by 1.4's relocated launcher — with the binary non-executable, nothing can start the stock screen, so HelixScreen owns `/dev/fb0` / DRM. The uninstaller restores the exec bit.

Both changes live in the persistent overlay upper and survive reboot via `/oem/.debug`. A **firmware upgrade** re-flashes the rootfs and removes `/oem/.debug`, so HelixScreen must be reinstalled after upgrading the Extended Firmware.

> **Why the binary kill switch?** Firmware 1.3 launched `/usr/bin/gui` only from `/etc/init.d/S99screen`, so HelixScreen originally took over by hijacking that one script. Firmware 1.4 removed `S99screen` and starts the stock UI from a runtime-generated path that is absent from the flashed image and cannot be patched by name. Disabling the binary is the version-independent fix.

## Cross-Compilation

The U1 target uses the same aarch64 cross-compiler as the Raspberry Pi, with fully static linking to avoid glibc version dependencies.

### Build via Docker (Recommended)

```bash
# Build the Docker toolchain (first time only — cached after)
make snapmaker-u1-docker
```

The Docker image (`docker/Dockerfile.snapmaker-u1`) is based on Debian Trixie with `crossbuild-essential-arm64`. It uses Debian's `aarch64-linux-gnu` toolchain with static linking for a self-contained binary.

### Build Directly (Requires Toolchain)

```bash
make PLATFORM_TARGET=snapmaker-u1 -j
```

### Build Configuration

| Setting | Value |
|---------|-------|
| Architecture | aarch64 (ARMv8-A) |
| Toolchain | `aarch64-linux-gnu-gcc` (Debian cross) |
| Linking | Hybrid (static libstdc++/libgcc, dynamic libc/libdrm) |
| Display backend | DRM/KMS (`/dev/dri/card0`, double-buffered page flipping) |
| Input | evdev (auto-detected) |
| SSL | Enabled |
| Optimization | `-Os` (size-optimized) |
| Platform define | `HELIX_PLATFORM_SNAPMAKER_U1` |

### CI/Release Status

The snapmaker-u1 target is included in `release-all`, `package-all`, and the GitHub Actions release workflow. Binaries are built on every tagged release.

Manual packaging is also available:

```bash
make package-snapmaker-u1
```

## Installation

### Prerequisites

1. **Snapmaker U1** on the network (Ethernet or WiFi)
2. **Extended Firmware** installed — provides SSH access. Download from [paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware), flash via USB drive (FAT32, `.bin` file in root)
3. **SSH enabled** — after Extended Firmware is installed, enable SSH via the firmware config web UI:
   ```bash
   # Open http://<printer-ip>/firmware-config/ in a browser, or:
   curl -X POST http://<printer-ip>/firmware-config/api/settings/ssh/true
   ```
4. **SSH access verified** — connect as root:
   ```bash
   ssh root@<printer-ip>   # password: snapmaker
   ```

### One-Line Install (Recommended)

For end users with Extended Firmware already installed, the easiest path is the hosted installer:

```bash
# Install on U1 (requires extended firmware and SSH access)
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

The installer auto-detects the U1 platform, downloads the correct aarch64 binary from the release CDN, deploys platform hooks, and starts HelixScreen. Re-run to upgrade.

### Build

```bash
# Build the Docker toolchain and cross-compile (first time builds the toolchain image)
make snapmaker-u1-docker
```

Output: `build/snapmaker-u1/bin/helix-screen` (~13MB stripped aarch64 binary)

### Deploy

```bash
# Full deploy (binary + assets + platform hooks) — stops stock UI, starts HelixScreen
make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<printer-ip>

# Deploy and run in foreground with debug logging (recommended for first run)
make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=<printer-ip>

# Deploy binary only (fast iteration during development)
make deploy-snapmaker-u1-bin SNAPMAKER_U1_HOST=<printer-ip>

# SSH into the printer
make snapmaker-u1-ssh SNAPMAKER_U1_HOST=<printer-ip>
```

Default SSH user is `root` (override with `SNAPMAKER_U1_USER`). Default deploy directory is `/userdata/helixscreen` (override with `SNAPMAKER_U1_DEPLOY_DIR`).

The deploy target automatically:
- Copies the binary, assets, and platform hooks to `/userdata/helixscreen/`
- Deploys the init script (`helixscreen.init`) and DRM keepalive hooks
- Starts HelixScreen via the init script (which sources the hooks)

### What Happens on Deploy

1. DRM keepalive: a background process opens `/dev/dri/card0` to prevent CRTC teardown
2. The stock UI process (`gui`) is killed, and the installer disables its binary (`chmod -x /usr/bin/gui`) so no launcher can relaunch it. `lmd` (the camera/timelapse supervisor) is left running — killing it would break timelapse.
3. HelixScreen starts as DRM master with double-buffered page flipping
4. The DRM keepalive process exits once HelixScreen has the DRM device open
5. The first-run wizard appears (language selection, printer connection setup)

### Rollback (Restore Stock UI)

To restore the stock Snapmaker touchscreen UI, run the uninstaller — it re-enables the stock UI binary (`/usr/bin/gui`) and restores the stock `S99screen` launcher (firmware 1.3) or removes the HelixScreen-created one (firmware 1.4):

```bash
ssh root@<printer-ip> "curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall; reboot"
```

> **A bare `rm -rf /userdata/helixscreen` is no longer sufficient.** The installer disables the stock UI binary (`chmod -x /usr/bin/gui`) so neither firmware's launcher can start it; removing HelixScreen without re-enabling the binary leaves a black screen. If you can't run the uninstaller, revert manually:
>
> ```bash
> ssh root@<printer-ip> "killall helix-screen helix-watchdog 2>/dev/null; chmod +x /usr/bin/gui; rm -rf /userdata/helixscreen; reboot"
> ```

The stock UI binary lives on the read-only SquashFS rootfs and is only disabled, never deleted — it cannot be damaged by HelixScreen deployment.

## Reversible Deployment Strategy

HelixScreen can be deployed to the U1 without modifying the read-only base firmware — all changes live in the writable overlay and are fully reversible.

### Level 1: Manual SSH Deployment (Current, Fully Reversible)

This is the current deployment method used by `make deploy-snapmaker-u1`:

1. Install [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) for SSH access
2. Enable SSH via firmware config web UI
3. Deploy via `make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<ip>`
4. The installer disables the stock UI binary (`chmod -x /usr/bin/gui`) and installs a HelixScreen launcher at `/etc/init.d/S99screen` (in the writable overlay); HelixScreen starts on `/dev/fb0`. `lmd` (camera/timelapse supervisor) keeps running.

**To revert**: run the uninstaller (re-enables `/usr/bin/gui` and restores the launcher), or manually `chmod +x /usr/bin/gui && rm -rf /userdata/helixscreen`, then reboot. The stock UI binary is on the read-only SquashFS rootfs and is only disabled, never deleted — but note that the init-script changes and the binary's exec bit **are** modified (in the reversible overlay), so a plain reboot alone will not bring the stock UI back.

### Level 2: SysV Init Override (Persistent, Reversible)

The U1 uses SysV init (not systemd). A persistent override would:

1. Create a HelixScreen init script in `/etc/init.d/` (writable overlay)
2. Optionally chmod -x the stock `S99screen` script (reversible since overlay)
3. HelixScreen starts on boot; stock UI stays dormant

**To revert**: Remove the init script override and reboot. Stock UI resumes from the read-only base.

### Level 3: Extended Firmware Overlay (Cleanest, Reversible)

Package HelixScreen as an overlay in paxx12's Extended Firmware build system:

1. Add a HelixScreen overlay that deploys the binary and init script
2. Build a custom firmware .bin with the overlay included
3. Flash via USB like any firmware update

**To revert**: Flash stock firmware (or Extended Firmware without the HelixScreen overlay) via USB. A/B firmware slots ensure the previous firmware is preserved.

### Safety Guarantees

| Risk | Mitigation |
|------|-----------|
| Bricked device | Impossible — Rockchip MaskRom mode provides hardware-level recovery |
| Lost stock UI | Stock UI lives on read-only SquashFS — cannot be accidentally deleted |
| Klipper/Moonraker disrupted | HelixScreen only replaces the display UI; Klipper (S60klipper) and Moonraker (S61moonraker) are independent services |
| Can't revert | Multiple revert paths: reboot, kill process, remove override, reflash firmware |
| Firmware slot corruption | A/B slots — switch with `updateEngine --misc=other --reboot` |

### Display Backend — DRM/KMS with CRTC Keepalive

HelixScreen uses the DRM backend for double-buffered page flipping on `/dev/dri/card0` (rockchipdrmfb). The 480x320 MCU panel runs on a DPI/RGB parallel interface via the Rockchip VOP2 display controller.

**The CRTC keepalive problem**: The stock UI (`/usr/bin/gui`) holds DRM master. When gui exits, the kernel's VOP2 driver calls `vop2_crtc_atomic_disable`, permanently disabling the display until reboot. The MCU panel driver only creates modes during the initial boot sequence — once the CRTC is disabled, there's no way to re-enable it.

**The solution**: The platform hooks (`config/platform/hooks-snapmaker-u1.sh`) spawn a background process that holds `/dev/dri/card0` open *before* killing gui. This prevents the kernel from tearing down the CRTC when gui exits. HelixScreen then opens the DRM device itself and becomes DRM master. The keepalive process detects that HelixScreen has the device open and exits — but the CRTC stays active because HelixScreen now holds the fd.

**Critical implementation notes**:
- The keepalive MUST be a background subshell (`(exec 3>/dev/dri/card0; ...) &`), not a shell fd (`exec 7>`). Shell fds die when the init script exits, but background processes survive.
- The keepalive polls `/proc/*/fd` until it sees `helix-screen` with `/dev/dri/card0` open, then exits.
- `HELIX_DRM_DEVICE=/dev/dri/card0` is set in `platform_pre_start()` to skip auto-detection.
- No libinput is needed — touch input uses evdev directly.

**Filesystem note**: `/opt/` is an overlay filesystem wiped on reboot. All persistent data lives on `/userdata/` (ext4, 28GB). `/home/lava/` is also part of the overlay and is NOT persistent.

### Touch Input

Touch input is provided by a TLSC6x capacitive controller (`tlsc6x_touch`) on `/dev/input/event0`. HelixScreen auto-detects this device and uses multitouch (MT) axis ranges (0-480, 0-320). No touch calibration is required — the capacitive controller is factory-calibrated.

### Backlight

HelixScreen auto-detects the sysfs backlight device (`/sys/class/backlight/backlight`, max brightness 255) for sleep/wake control.

## Auto-Detection

HelixScreen auto-detects the Snapmaker U1 using 17 heuristics from `config/printer_database.json`:

| Heuristic | Confidence | Description |
|-----------|------------|-------------|
| `fm175xx_reader` object | 99 | FM175xx RFID reader -- definitive U1 signature |
| `FILAMENT_DT_UPDATE` macro | 95 | RFID filament detection macro (extended firmware) |
| `FILAMENT_DT_QUERY` macro | 95 | RFID filament query macro (extended firmware) |
| Hostname `u1` | 90 | Hostname contains "u1" |
| Hostname `snapmaker` | 85 | Hostname contains "snapmaker" |
| `tmc2240` object | 60 | TMC2240 stepper driver presence |
| CoreXY kinematics | 40 | CoreXY motion system |
| Cartesian kinematics | 20 | Cartesian motion system |
| _(+9 additional heuristics)_ | various | Tool state, extruder naming, custom macros, motion parameters, etc. |

No manual printer configuration is needed in most cases. The FM175xx RFID reader is the strongest signal -- it is unique to the U1 and provides near-certain identification.

When the U1 is detected, the printer database record provides these metadata fields:

| Field | Value |
|-------|-------|
| `probe_type` | `eddy_current` |
| `toolhead_style` | `snapmaker_u1` |
| `preset` | `snapmaker_u1` |
| `z_offset_calibration_strategy` | `probe_calibrate` |

The `snapmaker_u1` preset causes the first-run wizard to auto-skip hardware steps that do not apply to the U1 (e.g., probe wiring, toolhead identification).

## Print Start Tracking

HelixScreen uses the `snapmaker_u1` print start profile (`config/print_start_profiles/snapmaker_u1.json`) to track progress through the startup sequence. The profile uses weighted progress mode with these phases:

1. Homing (10%)
2. Bed heating (20%)
3. Nozzle heating (20%)
4. Z tilt adjust (15%)
5. Bed mesh calibration (15%)
6. Nozzle cleaning (10%)
7. Purging (10%)

The progress bar updates as each phase completes, so you can see exactly where your printer is in its startup routine.

## 480x320 Display Considerations

The U1's 480x320 display uses the TINY layout preset. This is the smallest resolution HelixScreen supports, and several UI panels have known layout issues at this size. See the [480x320 UI Audit](480x320_UI_AUDIT.md) for a panel-by-panel breakdown. Key issues:

- **Navbar icons clipped** at screen edges
- **Controls panel** labels overlapping, z-offset value wrapping
- **Print select list view** fundamentally broken at this size
- **Numeric keypad overlay** too tall, bottom rows cut off
- **Filament panel** cards pushed off-screen

These are resolution-specific issues, not Snapmaker-specific. Any 480x320 device benefits from the same fixes.

## Known Limitations

- **480x320 UI needs work** -- Multiple panels have layout issues at this resolution (see above).
- **Extended firmware required** -- SSH access (needed for deployment) requires the community [Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware). Stock firmware does not provide SSH.
- **Auto-start requires `/oem/.debug`** -- The overlay filesystem is wiped on boot unless `/oem/.debug` exists. This flag must be created once during installation to persist the S99screen patch.
- **WiFi management** -- Stopping `unisrv` (stock UI) does not affect WiFi — the U1 uses standard `wpa_supplicant` managed by the OS. HelixScreen has its own WiFi manager with `wpa_supplicant` support.

## Future Work

### Auto-Start on Boot

The overlay filesystem is wiped on every reboot by `S01aoverlayfs` (`rm -rf /oem/overlay/*`) **unless** the debug flag `/oem/.debug` exists. With this flag, overlay modifications persist across reboots.

To enable auto-start:

```bash
# 1. Create debug flag to prevent overlay wipe
ssh root@<ip> "touch /oem/.debug"

# 2. Patch S99screen to delegate to HelixScreen when installed
# (The deploy target handles this automatically)
```

The patched `S99screen` checks for `/userdata/helixscreen/helixscreen.init` on boot. If present, it starts HelixScreen instead of the stock GUI. If HelixScreen is removed, S99screen falls back to the stock GUI automatically.

**Important**: The platform hooks must NOT call `/etc/init.d/S99screen stop` — since S99screen delegates to HelixScreen, this causes infinite recursion. The hooks kill `gui` directly instead.

### Extended Firmware Overlay

Package HelixScreen as an Extended Firmware overlay for one-click installation via paxx12's build system.

### RFID Filament UI

The `AmsBackendSnapmaker` backend parses RFID data from `filament_detect.info` when the RFID reader is enabled. With the RFID reader disabled (default on Extended Firmware via `disable-rfid-reader.cfg`), all RFID fields return `"NONE"` and `print_task_config` is the authoritative filament data source.

### Virtual Slot Mapping

The U1 supports an `extruder_map_table` with 32 virtual slots mapped to 4 physical extruders. This could enable more advanced filament management workflows.

## Moonraker Object Reference

The U1's Klipper exposes several custom objects. `AmsBackendSnapmaker` subscribes to and parses these during the Moonraker subscription phase.

### `print_task_config` — Authoritative filament data (HANDLED)

The primary source for filament info. Populated by the stock firmware's task manager regardless of RFID reader state.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `filament_type` | `["PLA","PLA","PLA","PLA"]` | ✅ | Material per slot |
| `filament_sub_type` | `["SnapSpeed",...]` | ✅ | Appended to type (e.g., "PLA SnapSpeed") |
| `filament_vendor` | `["Snapmaker",...]` | ✅ | Brand per slot |
| `filament_color_rgba` | `["080A0DFF",...]` | ✅ | RGBA hex → RGB uint32 |
| `filament_exist` | `[true,true,true,true]` | ✅ | Slot presence |
| `filament_color` | `[4278716941,...]` | — | Redundant with `_rgba`, not parsed |
| `filament_official` | `[true,...]` | ❌ | Could show official/third-party badge |
| `filament_sku` | `[900001,...]` | ❌ | Snapmaker product SKU |
| `filament_soft` | `[false,...]` | ❌ | Soft filament flag (TPU etc.) |
| `filament_edit` | `[false,...]` | ❌ | Whether user has edited filament info |
| `extruder_map_table` | `[0,1,2,3,0,...(x32)]` | ❌ | Virtual→physical slot mapping for multi-material |
| `extruders_used` | `[false,...]` | ❌ | Which extruders are used in current print |
| `extruders_replenished` | `[0,1,2,3]` | ❌ | Auto-replenish mapping |
| `auto_replenish_filament` | `true` | ❌ | Auto-replenish enabled |
| `filament_entangle_detect` | `false` | ❌ | Entangle detection enabled |
| `filament_entangle_sen` | `"medium"` | ❌ | Sensitivity level |
| `flow_calibrate` | `false` | ❌ | Flow calibration pre-print option |
| `shaper_calibrate` | `false` | ❌ | Input shaper calibration pre-print option |
| `time_lapse_camera` | `false` | ❌ | Timelapse pre-print option |
| `auto_bed_leveling` | `false` | ❌ | ABL pre-print option |

### `filament_detect` — RFID tag data (HANDLED)

Per-channel RFID tag reads. All fields return `"NONE"`/`0` when the RFID reader is disabled.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `state` | `[1,1,1,1]` | ✅ | 1=filament present, 0=empty |
| `info[].MAIN_TYPE` | `"NONE"` or `"PLA"` | ✅ | Skipped when `"NONE"` |
| `info[].SUB_TYPE` | `"NONE"` or `"Silk"` | ✅ | Skipped when `"NONE"` |
| `info[].MANUFACTURER` | `"NONE"` or `"Snapmaker"` | ✅ | |
| `info[].VENDOR` | `"NONE"` | ✅ | Fallback for brand |
| `info[].ARGB_COLOR` | `4294967295` | ✅ | ARGB → RGB mask |
| `info[].HOTEND_MIN/MAX_TEMP` | `0` | ✅ | |
| `info[].BED_TEMP` | `0` | ✅ | |
| `info[].WEIGHT` | `0` | ✅ | |
| `info[].DIAMETER` | `0` | ❌ | Filament diameter (1.75mm) |
| `info[].LENGTH` | `0` | ❌ | Spool length |
| `info[].DRYING_TEMP/TIME` | `0` | ❌ | Recommended drying params |
| `info[].OFFICIAL` | `false` | ❌ | Official Snapmaker filament |
| `info[].CARD_UID` | `0` | ❌ | RFID tag unique ID |
| `info[].SKU` | `0` | ❌ | Product SKU |
| `config.startup_stay` | `false` | ❌ | Unknown purpose |

### `filament_feed left` / `filament_feed right` — Feed module state (HANDLED)

Per-extruder filament feed state. Left module serves extruder0/1, right serves extruder2/3.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `extruderN.filament_detected` | `true` | ✅ | Filament presence |
| `extruderN.channel_state` | `"load_finish"` | ✅ | Load/unload progress |
| `extruderN.channel_error` | `"ok"` | ✅ | Error detection |
| `extruderN.channel_error_state` | `"none"` | ❌ | Error sub-state |
| `extruderN.channel_action_state` | `"load_finish"` | ❌ | Redundant with channel_state |
| `extruderN.module_exist` | `true` | ❌ | Feed module physically present |
| `extruderN.disable_auto` | `false` | ❌ | Auto-feed disabled |

Known `channel_state` values: `"idle"`, `"preloading"`, `"loading"`, `"load_finish"`, `"unloading"`

### `machine_state_manager` — Machine state (NOT HANDLED)

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `main_state` | `0` | ❌ | Top-level machine state (0=idle) |
| `action_code` | `0` | ❌ | Current action code |

Likely redundant with `print_stats.state` for most purposes.

### `defect_detection` — Print defect detection (NOT SUBSCRIBED)

AI-based print defect detection system. Not subscribed because it's not directly relevant to AMS/filament management.

| Field | Example | Notes |
|-------|---------|-------|
| `main_enable` | `true` | Master enable |
| `clean_bed.enable` | `true` | Dirty bed detection |
| `noodle.enable` | `true` | Spaghetti/noodle detection |
| `residue.enable` | `false` | Residue detection |
| `nozzle.enable` | `false` | Nozzle clog detection |

### `purifier` — Air purifier (NOT SUBSCRIBED)

Built-in HEPA/carbon air purifier. Not subscribed because we don't have UI for it yet.

| Field | Example | Notes |
|-------|---------|-------|
| `fan_speed` | `0.0` | Purifier fan speed (0-1) |
| `fan_rpm` | `0.0` | Actual RPM |
| `work_time` | `0` | Total runtime (seconds) |
| `power_detected` | `false` | External power connected |
| `delay_time` | `180` | Post-print run time (seconds) |

### `filament_entangle_detect` — Tangle detection (NOT SUBSCRIBED)

Per-extruder filament tangle detection. Not subscribed.

| Field | Example | Notes |
|-------|---------|-------|
| `detect_factor` | `1.0` | Tangle confidence (0-1, lower=tangled) |

## Verified Hardware

HelixScreen has been tested on a Snapmaker U1 with Extended Firmware. Confirmed working:

- DRM display at 480x320 via rockchipdrmfb with double-buffered page flipping
- DRM CRTC keepalive works — gui killed cleanly, no SIGSTOP hack needed
- Touch input via TLSC6x capacitive controller (no calibration needed)
- Backlight control via sysfs
- Stock UI stops and restarts cleanly via init script hooks
- SSH session survives stopping gui (WiFi unaffected)
- First-run wizard displays correctly at TINY breakpoint
- Memory monitor reports 961MB total with appropriate thresholds
- Persistent deployment on `/userdata/` survives reboots

## Community Testing

We welcome additional testers with Snapmaker U1 hardware:

1. Install the [Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) for SSH access
2. Enable SSH: `curl -X POST http://<ip>/firmware-config/api/settings/ssh/true`
3. Install via one-liner (easiest): `curl -sSL https://releases.helixscreen.org/install.sh | sh`
   - Or build from source: `make snapmaker-u1-docker` then `make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=<ip>`
4. Report: Does the wizard appear? Does touch work? Can you connect to Moonraker? Do tool changes work?
5. File issues at the HelixScreen GitHub repository

## Related Resources

- **[Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware)** -- Adds SSH access and community features to the U1
- **[U1 Config Example](https://github.com/JNP-1/Snapmaker-U1-Config)** -- Community reverse-engineered Klipper configuration
- **[Snapmaker Forum](https://forum.snapmaker.com/c/snapmaker-products/87)** -- Official U1 discussion
- **[Toolchanger Research](printer-research/SNAPMAKER_U1_RESEARCH.md)** -- Detailed analysis of U1's toolchanger implementation vs. standard Klipper toolchanger module
- **[Snapmaker/u1-klipper](https://github.com/Snapmaker/u1-klipper)** -- Open source Klipper fork
- **[Snapmaker/u1-moonraker](https://github.com/Snapmaker/u1-moonraker)** -- Open source Moonraker fork
- **[Snapmaker/u1-fluidd](https://github.com/Snapmaker/u1-fluidd)** -- Open source Fluidd fork
- **[paxx12/u1-firmware-tools](https://github.com/paxx12/u1-firmware-tools)** -- Firmware unpack/repack tools
- **[480x320 UI Audit](480x320_UI_AUDIT.md)** -- Panel-by-panel breakdown of layout issues at this resolution

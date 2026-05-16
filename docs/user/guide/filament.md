# Filament Management

---

## External Spool Configuration

If you're not using an AMS or multi-material system, you can tell HelixScreen what filament is loaded by configuring an **external spool**. Tap the spool icon on the Filament panel to set the material, color, and brand. If Spoolman is configured, you can also link to a specific Spoolman spool.

Once configured, the external spool information is used throughout the UI:

- **Spool preset button** — A dynamic preset button appears on the Filament panel with your spool's material name and recommended temperatures. Tap it to pre-heat both the nozzle and bed to the correct temperatures for your loaded filament.
- **Temperature panel presets** — The Nozzle and Bed temperature panels also show a spool preset button for quick one-tap heating.
- **Purge temperature** — When you tap **Purge**, HelixScreen automatically passes the recommended nozzle temperature to the purge macro (as the `PURGE_TEMP` parameter), so macros that support it can heat to the right temperature.

The spool preset button only appears when the loaded material differs from the standard presets (PLA, PETG, ABS, TPU). For standard materials, just use the built-in preset buttons.

> **Tip:** The spool preset updates automatically when you change the external spool configuration — no need to close and reopen panels.

---

## Extrusion Panel

![Extrusion Panel](../../images/user/controls-extrusion.png)

Manual filament control:

| Button | Action |
|--------|--------|
| **Extrude** | Push filament through nozzle |
| **Retract** | Pull filament back |

**Amount selector**: 5mm, 10mm, 25mm, 50mm
**Speed selector**: Slow, Normal, Fast

> **Safety:** Extrusion requires the hotend to be at minimum temperature (usually 180°C for PLA, higher for other materials). If HelixScreen knows what filament is loaded — either from an [external spool](filament.md#external-spool-configuration) or an active AMS slot — it skips the cold-nozzle safety warning and auto-preheats to the correct temperature instead.

---

## Load / Unload / Purge

The Filament panel has dedicated **Load**, **Unload**, and **Purge** buttons. These run Klipper macros — HelixScreen auto-detects common names like `LOAD_FILAMENT`, `UNLOAD_FILAMENT`, and `PURGE` from your printer config.

### Customizing which macro runs

You can override any of these buttons to run a different macro:

1. Go to **Settings > Printer > Macro Buttons**
2. Scroll to the **Standard Macros** section
3. Tap the dropdown for **Load Filament**, **Unload Filament**, or **Purge**
4. Select **(Auto)** to use auto-detection, or pick any macro from your Klipper config

This works whether or not you have an AMS system. If a slot is left empty (no macro detected or configured), the button is disabled.

> **With an AMS system:** The Load and Unload buttons use your AMS backend instead of running a macro — they trigger slot-based load/unload through the AMS panel. The Purge button still uses your configured macro.

### Manual extrude/retract

For manual control without macros, use the **Extrude** and **Retract** buttons on the extrusion widget with selectable amounts (5mm, 10mm, 25mm, 50mm) and speeds.

---

## AMS / Multi-Material Systems

![AMS Panel](../../images/user/ams.png)

For multi-material systems (Happy Hare, AFC-Klipper, ACE, Tool Changer, etc.). The AMS panel has two main areas: the **slot view** on the left and the **sidebar** on the right.

### Slot View (Left)

The left side shows all your filament slots in a visual tray layout:

- **Spool icons** — Each slot displays a 3D spool visualization with its filament color
- **Material labels** — Material type (PLA, PETG, ABS, etc.) shown above each spool
- **Status badge** — Slot number with color-coded background (green = loaded, gray = empty, red = error)
- **Tool badge** — If a slot is assigned to a specific extruder tool (T0, T1, etc.), a badge appears in the corner

Below the slot grid, a **filament path diagram** shows the routing from slots through the hub/selector to the toolhead. This updates in real time during load/unload operations, including eject animations when retracting filament at the slot sensor.

Above the slot view, a **mini temperature graph** shows live nozzle, bed, and chamber temperatures (when a chamber sensor or heater is present) so you can monitor heating during filament operations without switching panels.

### Sidebar (Right)

The right sidebar shows the status of the currently loaded filament and provides quick-access controls.

**Currently loaded section:**

- **"Current: Slot N"** — Header showing which slot is active (or "Current: Bypass" when bypass is enabled)
- **Color swatch** — Large color indicator matching the loaded filament
- **Material name** — e.g., "Red PLA", "Prusament PETG"
- **Remaining weight** — Estimated filament remaining (e.g., "750g"), if available
- **Clog detection meter** — When your system has flow monitoring (encoder, FlowGuard, or AFC buffer), an arc meter shows the current flow rate percentage

**During load/unload operations**, the sidebar switches to a **step progress display** showing each stage of the operation:

- **Load (fresh):** Heat nozzle → Feed filament → Purge
- **Load (swap):** Heat nozzle → Cut/form tip → Feed filament → Purge
- **Unload:** Heat nozzle → Cut/form tip → Retract

Each step updates in real time so you can see exactly where the operation is.

**Action buttons** (hidden while an operation is in progress):

| Button | Action |
|--------|--------|
| **Bypass** (toggle) | Feed filament directly to the extruder, bypassing the AMS. Only shown if your hardware supports bypass. |
| **Unload** | Retract the currently loaded filament back to its slot |
| **Reset** | Reset the AMS system state (useful after jams or errors) |
| **Settings** | Open the AMS Management overlay for advanced controls |

### Slot Context Menu

**Tap any slot** to open a context menu with actions for that specific slot:

| Action | Description |
|--------|-------------|
| **Load** | Feed filament from this slot to the toolhead. Disabled if the slot is empty. |
| **Unload** | Retract filament from this slot. Only available if this slot is currently loaded. |
| **Reset Lane** | Clear an error or jam state on this slot. Only shown if your backend supports per-slot reset. |
| **Spool Info** | Open the filament editor to view or change material, color, vendor, and remaining weight. |
| **Select Spool** | Assign a saved Spoolman spool to this slot. Only shown when Spoolman is configured. |
| **Scan QR Code** | Scan a filament QR code to auto-fill spool data. Only shown when Spoolman is configured. |

For advanced multi-tool setups, the context menu also includes:

- **Tool Mapping** — Assign which extruder tool (T0, T1, etc.) this slot feeds
- **Backup Slot** (Endless Spool) — Choose a backup slot to automatically switch to if this spool runs out mid-print

### Editing Filament Properties

Tap **Spool Info** in the slot context menu to open the filament editor. This lets you tell HelixScreen what's loaded in each slot — important for systems without automatic detection (RFID).

**What you can edit:**

- **Color** — Tap the color swatch to open a color picker
- **Vendor** — Select from a dropdown (e.g., Prusament, eSUN, Hatchbox)
- **Material** — Select the filament type (PLA, PETG, ABS, TPU, Nylon, etc.)
- **Remaining weight** — Tap the pencil icon to enable a slider and set how full the spool is (0–100%)

**Read-only info:**

- **Nozzle temperature range** — Recommended printing temperatures (e.g., 200–230°C)
- **Bed temperature** — Recommended bed temperature (e.g., 60°C)

**Spoolman actions** (when Spoolman is configured):

- **Choose Saved Spool** — Browse your Spoolman database and assign a spool. This auto-fills the vendor, material, color, and temperatures.
- **Scan QR Code** — Scan a filament spool's QR code to look it up in Spoolman
- **More actions button** (▾ dropdown) — Tap the dropdown arrow for additional actions:
  - **Spool Details** — View the full Spoolman spool record
  - **Unlink** — Remove the Spoolman association (appears only when a spool is linked)
  - **Print Label** — Print a physical label for this spool (appears only when a label printer is set up)

Tap **Save** to apply your changes, or **Cancel** to discard them.

### Syncing with OrcaSlicer 2.3.2+

When you edit spool info in HelixScreen — on any supported filament system (AD5X IFS, Snapmaker U1, ACE, CFS) — that information is saved to your printer in the standard location OrcaSlicer 2.3.2 and later reads automatically. Open OrcaSlicer after editing and your slot's vendor, material, color, and Spoolman link show up in the filament panel with no extra setup.

**AFC (Box Turtle) and Happy Hare** work the same way automatically — their Klipper plugins write the same records on their own, so your lane assignments also flow through to OrcaSlicer with nothing to configure.

Either way — whether HelixScreen is writing the metadata (IFS / Snapmaker / ACE / CFS) or your filament system's own plugin is (AFC / Happy Hare) — your printer's filament info and OrcaSlicer stay in sync.

> **Requirements:** OrcaSlicer 2.3.2 or newer, connected to the same printer's Moonraker. Nothing to enable on the HelixScreen side — it's automatic.

### AMS Management (Settings Overlay)

Tap **Settings** in the sidebar to open the AMS Management overlay with advanced controls:

- **Home** — Return the AMS to its home position
- **Recover** — Attempt to recover from an error state
- **Abort** — Cancel the current operation immediately
- **Bypass Mode** — Toggle direct-feed mode (if supported by hardware)
- **System status** — Current system state and firmware version

Additional device-specific settings may appear as expandable sections depending on your hardware.

### Tips

- When an AMS slot is actively loaded, its material information drives spool preset behavior — you'll see a spool preset button on the Filament and Temperature panels, and purge macros receive the correct temperature automatically. See [External Spool Configuration](#external-spool-configuration) for details.
- The filament path diagram at the bottom of the slot view is interactive — you can tap slot entry points to trigger a load.
- During a load or unload, watch the step progress in the sidebar to track exactly where the operation is.

---

## Multiple Filament Systems

HelixScreen supports running multiple filament management backends at the same time. For example, a toolchanger printer might use both a Tool Changer backend and Happy Hare for different parts of the filament path.

When multiple backends are detected:

- A **backend selector** appears at the top of the AMS panel
- Tap to switch between systems (e.g. "Happy Hare" vs "Tool Changer")
- Each backend has its own slots and status display
- Slot assignments and controls are independent per backend

**Supported system types:**

| System | Description |
|--------|-------------|
| **CFS** | Creality Filament System (K2 series printers) |
| **Happy Hare** | MMU2, ERCF, 3MS, Tradrack, EMU |
| **AFC** | Box Turtle, OpenAMS, ViViD |
| **ACE** | Anycubic ACE Pro (via ValgACE/BunnyACE/DuckACE Klipper drivers) |
| **Tool Changer** | Toolchanger-based filament routing |
| **AD5X IFS** | FlashForge Adventurer 5X Intelligent Filament Switching (requires ZMOD firmware v1.7.0 or newer) |

Each system displays its own logo in the AMS panel header. Happy Hare and AFC show their firmware logos; specific hardware variants (ERCF, Box Turtle, ViViD, etc.) show hardware-specific logos when detected.

Single-backend setups are unaffected — the panel works exactly as before with no selector shown.

---

## Creality Filament System (CFS)

The CFS is an enclosed filament storage and delivery system for **Creality K2 series** printers. Each CFS unit holds 4 spools of filament, and up to 4 units can be connected (16 total slots). HelixScreen auto-detects CFS when connected to a K2 printer.

### Slot Layout

CFS uses a **TNN address format** to identify each slot:

| Unit | Slot A | Slot B | Slot C | Slot D |
|------|--------|--------|--------|--------|
| Unit 1 | T1A | T1B | T1C | T1D |
| Unit 2 | T2A | T2B | T2C | T2D |
| Unit 3 | T3A | T3B | T3C | T3D |
| Unit 4 | T4A | T4B | T4C | T4D |

Each slot displays the detected filament color, material type (PLA, PETG, ABS, etc.), brand, and remaining filament length.

### RFID Detection

CFS units have built-in RFID readers that automatically detect Creality filament spools:

- Place a spool in any slot and its material info appears within seconds
- Supported materials include Hyper PLA, Hyper PETG, Hyper ABS, CR-PLA, CR-Silk, and more
- Remaining filament length is tracked automatically
- If a spool isn't recognized, a generic entry is shown — you can identify it manually

> **Tip:** If a slot shows incorrect info, remove and re-seat the spool to trigger a fresh RFID read.

### CFS Device Actions

Tap the menu icon on the CFS panel to access device actions:

| Action | What It Does |
|--------|--------------|
| **Refresh** | Re-read all RFID tags across all units — useful after swapping spools while the printer was off |
| **Auto-Refill** | Toggle automatic backup spool switching — when enabled, if the current spool runs out mid-print, the system loads the next spool of the same material |
| **Nozzle Clean** | Trigger the nozzle cleaning routine using the CFS's built-in silicone strip |

---

## Spoolman Integration

![Spoolman](../../images/user/advanced-spoolman.png)

When Spoolman is configured:

- Spool name and material type displayed per slot
- Remaining filament weight shown
- Tap a slot to open **Spool Picker** and assign a different spool

### Spoolman Panel

Access via the **Filament** nav tab. Browse, search, and manage your entire spool inventory:

- **Search** — Filter spools by vendor, material, or color name
- **Context menu** — Tap a spool to set active, edit, or delete
- **3D spool visualization** — Color-coded fill level at a glance

### New Spool Wizard

Tap **+ Add** in the Spoolman panel to create a new spool in 3 steps:

1. **Select Vendor** — Search existing vendors or tap **+ New** to add one
2. **Select Filament** — Pick an existing filament or tap **+ New** to create one with material, color, temperature ranges, and weight
3. **Spool Details** — Set remaining weight, price, lot number, and notes

The wizard creates all records (vendor, filament, spool) atomically in Spoolman.

> **Tip:** You can print physical spool labels with a QR code linking to Spoolman. See [Label Printing](label-printing.md) for setup instructions.

---

## Dryer Control

If your filament system has an integrated dryer (AMS, CFS, etc.):

- Temperature display and current humidity reading
- Timer settings for drying duration
- Enable/disable drying cycle

**Multi-unit setups:** Each unit has its own dryer with independent controls. The dryer panel shows which unit you're controlling — tap to switch between units. You can run dryers on multiple units simultaneously.

---

---

## See Also

- [Temperature Control](temperature.md) — Preheat presets work with spool material info
- [Bluetooth Setup](bluetooth-setup.md) — Required for Bluetooth-connected AMS and label printers
- [Label Printing](label-printing.md) — Print physical spool labels with Spoolman data
- [Settings: Hardware & Devices](settings/hardware.md) — AMS, Spoolman, and filament sensor configuration

---

**Next:** [Label Printing](label-printing.md) | **Prev:** [Motion & Positioning](motion.md) | [Back to User Guide](../USER_GUIDE.md)

# MonitorSwitcher

A lightweight system tray utility for Windows 11 that lets you instantly switch between monitors, change resolution, refresh rate, and toggle HDR — all from a single menu.

![AutoHotkey v2](https://img.shields.io/badge/AutoHotkey-v2.0%2B-green)
![Windows 11](https://img.shields.io/badge/Windows-11-blue)
![License: GPLv3+](https://img.shields.io/badge/License-GPLv3+-red)

## Features

- **Exclusive monitor mode** — Activate a single monitor and disable all others with one click. Useful for gaming, streaming, or remote desktop setups.
- **Resolution switching** — Browse and apply all available resolutions from a submenu. The current refresh rate is preserved when possible.
- **Refresh rate switching** — Quickly change between available refresh rates (e.g. 60Hz ↔ 144Hz) for the current resolution.
- **HDR toggle** — Enable or disable HDR on the active monitor with a single click or hotkey. Works with both physical monitors and virtual displays.
- **Settings persistence** — Resolution and refresh rate are saved to the Windows display database. When you switch back to a monitor, it restores the last mode you used.
- **Virtual Display Driver support** — Works with physical monitors and virtual displays like [Virtual Display Driver (VDD)](https://github.com/itsmikethetech/Virtual-Display-Driver).
- **Automatic state detection** — The menu refreshes automatically when Windows detects display changes (monitor plugged/unplugged, etc.).
- **One-click restore** — Return to your original multi-monitor layout at any time.
- **Emergency hotkeys** — Global shortcuts work from any screen, even when the taskbar isn't visible.

## Requirements

- **Windows 11**
- **[AutoHotkey v2.0+](https://www.autohotkey.com/)** (Unicode 64-bit)

## Installation

1. Install [AutoHotkey v2](https://www.autohotkey.com/) if you don't have it.
2. Download `MonitorSwitcher.ahk` from this repository.
3. Double-click the file to run it.

The monitor icon will appear in your system tray.

## Usage

### Tray Menu

Right-click the tray icon to access all features:

```
MonitorSwitcher v1.0
────────────────────────────
 *  LG ULTRAGEAR  |  1920x1080 @ 144Hz
    LG Dummy Plug  (off)
    VDD by MTT  (off)
────────────────────────────
Resolution   [1920x1080]  ►
Refresh Rate [144Hz]      ►
HDR          [ON]
────────────────────────────
Restore original config
Exit
```

- **Monitor list** — Click a monitor to activate it exclusively (all others are turned off). The active monitor is marked with `*`, or `>>` when in exclusive mode.
- **Resolution** — Submenu with all available resolutions, sorted largest first. The active one is marked with `>>`.
- **Refresh Rate** — Submenu with available rates for the current resolution.
- **HDR** — Toggle HDR on/off for the active monitor. Shows `[ON]`, `[OFF]`, `[not supported]`, or `[unknown]` depending on monitor capabilities and state.
- **Restore** — Returns to the exact display layout you had before entering exclusive mode.

### Global Hotkeys

| Shortcut | Action |
|---|---|
| `Ctrl+Win+M` | Open the tray menu at the mouse cursor (works from any screen) |
| `Ctrl+Win+R` | Restore the original display configuration |
| `Ctrl+Win+H` | Toggle HDR on/off for the active monitor |

These hotkeys are essential when working with a single monitor — the tray icon may not be visible on a secondary screen, but the hotkeys always work.

You can change the hotkeys by editing the script. For example, to use `Ctrl+Alt+R` instead of `Ctrl+Win+R`:

```ahk
; Change this:
#^r:: {
; To this:
^!r:: {
```

See the [AutoHotkey v2 Hotkey documentation](https://www.autohotkey.com/docs/v2/Hotkeys.htm) for all available modifiers.

## How It Works

MonitorSwitcher uses several Windows APIs, each for what it does best:

- **`SetDisplayConfig`** — Activates and deactivates monitors (topology changes). Uses `SDC_TOPOLOGY_SUPPLIED` to restore modes from Windows' persistence database, avoiding the 60Hz fallback caused by `SDC_ALLOW_CHANGES`. Monitors are identified by their hardware **target ID**, which doesn't change when displays are activated or deactivated (unlike `\\.\DISPLAYn` names which Windows reassigns dynamically).
- **`ChangeDisplaySettingsExW`** — Changes resolution and refresh rate on the active monitor. Finds the exact DEVMODE from the driver's mode list and applies it directly, preserving all signal parameters.
- **`QueryDisplayConfig`** with `QDC_ALL_PATHS` / `QDC_ONLY_ACTIVE_PATHS` — Enumerates all connected monitors and their current state.
- **`DisplayConfigGetDeviceInfo`** — Resolves friendly monitor names (e.g. "LG ULTRAGEAR") and GDI device names.
- **`EnumDisplaySettingsExW`** — Enumerates available resolutions and refresh rates.
- **`WM_DISPLAYCHANGE`** — Listens for system display events to keep the menu in sync.
- **`DisplayConfigGetDeviceInfo` (type 9)** — Checks if a monitor supports HDR (Advanced Color).
- **`DisplayConfigSetDeviceInfo` (type 10)** — Toggles HDR on/off for a monitor.
- **Windows Registry (MonitorDataStore)** — Reads the authoritative HDR state (the CCD API can report incorrect values on some devices).

No external dependencies, no admin privileges required, no PowerShell, no registry hacking.

## Virtual Display Driver (VDD)

MonitorSwitcher detects virtual displays automatically. Empty GPU ports (inactive with no available modes) are filtered out, while virtual displays like VDD that have registered display modes are shown and fully functional.

If VDD appears as `(off)` in the menu, selecting it will activate it using the best available display mode.

## Troubleshooting

**The menu shows `(off)` for my monitor:**
The monitor is detected but not currently active. Click it to activate it.

**Resolution or refresh rate resets after switching monitors:**
The first time you switch to a monitor, Windows uses its default mode. Change the resolution/refresh rate to your preference — it will be saved to the Windows display database and restored automatically on subsequent switches.

**Hotkeys conflict with other software:**
You can change `#^r` and `#^m` in the script to different key combinations. See the [AutoHotkey v2 Hotkey documentation](https://www.autohotkey.com/docs/v2/Hotkeys.htm).

**HDR toggle doesn't work or shows "not supported":**
The monitor may not support HDR, or the display driver doesn't report HDR capabilities correctly. Some dummy plugs and virtual displays don't support HDR.

**HDR state shows "unknown":**
The monitor's HDR state couldn't be read from the Windows registry. This may happen with newly connected monitors or unusual display configurations.

## License

This project is licensed under the [GNU General Public License v3 or later (GPLv3+)](https://choosealicense.com/licenses/gpl-3.0/).

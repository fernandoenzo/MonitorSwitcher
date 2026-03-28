# MonitorSwitcher

![C/Win32](https://img.shields.io/badge/C-Win32-blue)
![Windows 11](https://img.shields.io/badge/Windows-11-blue)
![License: GPLv3+](https://img.shields.io/badge/License-GPLv3+-red)

A lightweight system tray utility for Windows 11 that lets you instantly switch between monitors, change resolution, refresh rate, and toggle HDR — all from a single menu.

## Table of Contents

- [Features](#features)
- [Usage](#usage)
  - [Tray Menu](#tray-menu)
  - [Global Hotkeys](#global-hotkeys)
- [How It Works](#how-it-works)
- [Building from Source](#building-from-source)
- [License](#license)

## Features

- **Exclusive monitor mode** — Activate a single monitor and disable all others with one click. Useful for gaming, streaming, or remote desktop setups.
- **Resolution switching** — Browse and apply all available resolutions from a submenu. The current refresh rate is preserved when possible.
- **Refresh rate switching** — Quickly change between available refresh rates (e.g. 60Hz ↔ 144Hz) for the current resolution.
- **HDR toggle** — Enable or disable HDR per active HDR-capable monitor directly from the menu. A global hotkey toggles HDR on the primary monitor. Works with both physical monitors and virtual displays.
- **Settings persistence** — Resolution and refresh rate are saved to the Windows display database. When you switch back to a monitor, it restores the last mode you used.
- **Virtual Display Driver support** — Works with physical monitors and virtual displays like [Virtual Display Driver (VDD)](https://github.com/itsmikethetech/Virtual-Display-Driver).
- **Automatic state detection** — The menu refreshes automatically when Windows detects display changes (monitor plugged/unplugged, etc.).
- **One-click restore** — Return to your original multi-monitor layout at any time.
- **Start with Windows** — Optional auto-start via the tray menu (writes to `HKCU\...\Run`, no admin required).
- **Emergency hotkeys** — Global shortcuts work from any screen, even when the taskbar isn't visible.

## Usage

### Tray Menu

1. Download `MonitorSwitcher.exe` from [Releases](../../releases).
2. Run it. A monitor icon will appear in the system tray.
3. Right-click the tray icon to access all features:

```
MonitorSwitcher
────────────────────────────
>>  LG ULTRAGEAR  |  1920x1080 @ 144Hz
    LG Dummy Plug  (off)
    VDD by MTT  (off)
────────────────────────────
Resolution   [1920x1080]  ►
Refresh Rate [144Hz]      ►
HDR: LG ULTRAGEAR [ON]
────────────────────────────
Restore original config
────────────────────────────
✓ Start with Windows
Exit
```

- **Monitor list** — Click a monitor to activate it exclusively (all others are turned off). Active monitors are marked with `*` for multiple, or `>>` when only one is active.
- **Resolution** — Submenu with all available resolutions, sorted largest first. The active one is marked with `>>`.
- **Refresh Rate** — Submenu with available rates for the current resolution.
- **HDR** — Individual toggle per active HDR-capable monitor. Shows one line per monitor (e.g., `HDR: LG ULTRAGEAR [ON]`). State is read from Windows registry.
- **Restore** — Returns to the display layout saved at startup, whenever the current layout differs from the original.
- **Start with Windows** — Toggles auto-start at logon. When enabled, writes the executable path to the Windows Run registry key (`HKCU`).

### Global Hotkeys

| Shortcut | Action |
|---|---|
| `Ctrl+Alt+M` | Open the tray menu at the mouse cursor (works from any screen) |
| `Ctrl+Alt+R` | Restore the original display configuration |
| `Ctrl+Alt+H` | Toggle HDR on/off for the primary monitor |
| `Ctrl+Alt+1..9` | Switch directly to monitor (no confirmation dialog) |

These hotkeys are essential when working with a single monitor — the tray icon may not be visible on a secondary screen, but the hotkeys always work.

## How It Works

MonitorSwitcher uses several Windows APIs, each for what it does best:

- **`SetDisplayConfig`** — Activates and deactivates monitors (topology changes). Uses `SDC_TOPOLOGY_SUPPLIED` to restore modes from Windows' persistence database, avoiding the 60Hz fallback caused by `SDC_ALLOW_CHANGES`. When saving the original layout, stores `{targetId, sourceId}` pairs to preserve extend vs duplicate configurations and primary monitor order.
- **`ChangeDisplaySettingsExW`** — Changes resolution and refresh rate on the active monitor. Finds the exact DEVMODE from the driver's mode list and applies it directly, preserving all signal parameters.
- **`QueryDisplayConfig`** with `QDC_ALL_PATHS` / `QDC_ONLY_ACTIVE_PATHS` — Enumerates all connected monitors and their current state.
- **`DisplayConfigGetDeviceInfo`** — Resolves friendly monitor names (e.g. "LG ULTRAGEAR") and GDI device names.
- **`EnumDisplaySettingsExW`** — Enumerates available resolutions and refresh rates.
- **`WM_DISPLAYCHANGE`** — Listens for system display events to keep the menu in sync.
- **`DisplayConfigGetDeviceInfo` (type 9)** — Checks if a monitor supports HDR (Advanced Color).
- **`DisplayConfigSetDeviceInfo` (type 10)** — Toggles HDR on/off for a monitor.
- **Windows Registry (MonitorDataStore)** — Reads the authoritative HDR state (the CCD API can report incorrect values on some devices).

No external dependencies, no admin privileges required, no PowerShell, no registry hacking.

## Building from Source

To cross-compile from Linux (Debian/Ubuntu):

```bash
sudo apt install build-essential mingw-w64
make clean && make
```

This produces `MonitorSwitcher.exe`, a standalone Windows executable with the application icon embedded as a resource.

## License

This project is licensed under the [GNU General Public License v3 or later (GPLv3+)](https://choosealicense.com/licenses/gpl-3.0/).

**Icon Attribution:** Application icon sourced from [icon-icons.com](https://icon-icons.com/icon/rocket-startup-monitor-screen-computer/124621). License: Free for commercial use.

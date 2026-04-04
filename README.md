<p align="center">
  <img src="MonitorSwitcher.svg" alt="MonitorSwitcher" width="128">
</p>
<h1 align="center">MonitorSwitcher</h1>

<p align="center">
  <img src="https://img.shields.io/badge/C-Win32-blue" alt="C/Win32">
  <img src="https://img.shields.io/badge/Windows-11-blue" alt="Windows 11">
  <img src="https://img.shields.io/badge/License-GPLv3+-red" alt="License: GPLv3+">
  <img src="https://img.shields.io/github/v/release/fernandoenzo/MonitorSwitcher" alt="GitHub Release">
</p>

A lightweight system tray utility for Windows 11 that lets you instantly switch between monitors, change resolution, refresh rate, and toggle HDR — all from a single menu.

## Table of Contents

- [Features](#features)
- [Usage](#usage)
  - [Tray Menu](#tray-menu)
  - [Global Hotkeys](#global-hotkeys)
  - [Customizing Hotkeys](#customizing-hotkeys)
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
- **Customizable hotkeys** — Remap all keyboard shortcuts via a built-in dialog. Capture mode with live modifier preview. Individual hotkeys can be cleared (disabled). Monitor prefix modifiers are configurable via checkboxes.
- **Automatic update checker** — Checks GitHub releases on startup for new versions. Silent if up-to-date; shows a balloon notification when an update is available. Manual check available via tray menu ("Check for updates").

## Usage

### Tray Menu

1. Download `MonitorSwitcher.exe` from [Releases](../../releases).
2. Run it. A monitor icon will appear in the system tray.
3. Right-click the tray icon to access all features:

```
MonitorSwitcher 1.8.2
───────────────────────────
★  1. LG ULTRAGEAR  |  1920x1080 @ 144Hz
   2. LG Dummy Plug  (off)
   3. VDD by MTT  (off)
───────────────────────────
Resolution   [1920x1080]  ►
Refresh Rate [144Hz]      ►
✓ HDR: LG ULTRAGEAR [ON]
───────────────────────────
Restore original config
───────────────────────────
✓ Hotkeys enabled
    Customize...
✓ Start with Windows
───────────────────────────
Check for updates (latest)
Exit
```

When multiple monitors are active (extend/clone mode):

```
☆  1. LG ULTRAGEAR  |  1920x1080 @ 144Hz
☆  2. LG Dummy Plug  |  1920x1080 @ 60Hz
   3. VDD by MTT  (off)
```

- **Monitor list** — Monitors are numbered (`1.`, `2.`, `3.`) corresponding to `Ctrl+Alt+1..9` hotkeys. Click a monitor to activate it exclusively (all others are turned off). Active monitors are marked with `★` (exclusive) or `☆` (extend/clone). Inactive monitors show `(off)`.
- **Resolution** — Submenu with all available resolutions, sorted largest first. The active one is marked with a checkmark (✓).
- **Refresh Rate** — Submenu with available rates for the current resolution. The active one is marked with a checkmark (✓).
- **HDR** — Individual toggle per active HDR-capable monitor. Shows one line per monitor (e.g., `HDR: LG ULTRAGEAR [ON]`). When enabled, it is marked with a checkmark (✓). State is read from Windows registry.
- **Restore** — Returns to the display layout saved at startup, whenever the current layout differs from the original.
- **Hotkeys enabled** — Toggle to enable/disable all global hotkeys. When disabled, all registered shortcuts are unregistered. Balloon notification confirms the toggle.
- **Start with Windows** — Toggles auto-start at logon. When enabled, writes the executable path to the Windows Run registry key (`HKCU`).
- **Check for updates** — Manually check for new versions on GitHub. Shows a balloon notification with the result. On startup, checks silently and only notifies if an update is available.

### Global Hotkeys

| Default Shortcut | Action |
|---|---|
| `Ctrl+Alt+M` | Open the tray menu at the mouse cursor |
| `Ctrl+Alt+R` | Restore the original display configuration |
| `Ctrl+Alt+H` | Toggle HDR on/off for the primary monitor |
| `Ctrl+Alt+1..9` | Switch directly to monitor |

> **Note:** If a hotkey doesn't work, another program may have claimed it first (common with NVIDIA/AMD overlays). All hotkeys can be remapped or individually disabled via **Customize** in the tray menu.

These hotkeys work from any screen and bypass confirmation dialogs — they're essential when the tray icon isn't visible, and invaluable when facing a black screen caused by an active virtual monitor or dummy plug.

### Customizing Hotkeys

Open the customization dialog from the tray menu: click **Customize...** under "Hotkeys enabled".

- **Capture mode** — Click the "Capture" button next to any hotkey, then press the desired key combination. Modifiers are shown in real time as you hold them (e.g., "Ctrl+Alt+..."). Press a non-modifier key to complete the capture. At least one modifier (Ctrl, Alt, Shift, or Win) is required.
- **Conflict detection** — If the captured combination conflicts with another hotkey, you'll be prompted to choose a different one.
- **Clear** — Click "Clear" to disable an individual hotkey without affecting the others.
- **Monitor prefix** — The modifier combination for keys 1..9 is configured via checkboxes (Ctrl, Alt, Shift, Win). At least one must be checked to enable the monitor hotkeys.
- **Reset to Defaults** — Restores all hotkeys to their original Ctrl+Alt combinations.
- **Persistence** — Custom hotkey settings are stored in the Windows registry (`HKCU\Software\MonitorSwitcher`) and survive application restarts.
- **Capture failures** — If a key combination doesn't register, another application may have already reserved it. Common culprits include:
  - NVIDIA GeForce Experience (Alt+Z overlay)
  - AMD ReLive
  - Steam overlay
  - Discord push-to-talk or other bindings
  - OBS hotkeys

  If you're unsure which program is using a specific hotkey, try [Hotkey Detective](https://github.com/ITachiLab/hotkey-detective) to identify the culprit.

## How It Works

MonitorSwitcher uses several Windows APIs, each for what it does best:

- **`SetDisplayConfig`** — Activates and deactivates monitors (topology changes). Uses `SDC_TOPOLOGY_SUPPLIED` to restore modes from Windows' persistence database, avoiding the 60Hz fallback caused by `SDC_ALLOW_CHANGES`. When saving the original layout, stores `{targetId, sourceId, adapterLUID}` tuples to preserve extend vs duplicate configurations and primary monitor order.
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

This produces `MonitorSwitcher.exe`, a standalone Windows executable with the icon and manifest embedded as resources.

The version string is automatically extracted from git tags using `git describe --tags --dirty`. To release a new version, simply create a new tag — no code changes needed. The `-dirty` suffix indicates uncommitted changes in the working tree.

## License

This project is licensed under the [GNU General Public License v3 or later (GPLv3+)](https://choosealicense.com/licenses/gpl-3.0/).

**Icon Attribution:** Application icon sourced from [icon-icons.com](https://icon-icons.com/icon/rocket-startup-monitor-screen-computer/124621). License: Free for commercial use.

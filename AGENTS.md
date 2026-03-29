# AGENTS.md

## Project Overview

**MonitorSwitcher** is a Windows 11 system tray utility that allows users to:
- Switch between monitors exclusively (turn off all others)
- Change resolution and refresh rate
- Toggle HDR per active monitor (via menu) or primary monitor (via hotkey)
- Restore original display configuration

## File Structure

```
MonitorSwitcher.c         # Native Win32 C implementation
MonitorSwitcher.rc        # Windows resource script (embeds the .ico into the .exe)
MonitorSwitcher.ico       # Application icon
Makefile                  # Cross-compilation with MinGW-w64 from Linux
README.md                 # Full documentation
AGENTS.md                 # AI agent coding guidelines
LICENSE                   # GPLv3
.gitignore                # Ignores build artifacts (*.exe, *.o, *.res)
```

## Building

Requires `mingw-w64` and `build-essential` for cross-compilation from Linux:

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install build-essential mingw-w64

# Build
make

# Clean
make clean
```

This produces `MonitorSwitcher.exe`, a standalone Windows executable with the icon embedded as a resource. The Makefile links against `-lshell32 -luser32 -lkernel32 -ladvapi32` (advapi32 is needed for registry APIs used by HDR and auto-start).

The Makefile defines `VERSION` using `git describe --tags --dirty` with fallback to `dev` for builds outside a git repository. The version is passed to the compiler via `-DVERSION_STRING`.

## Testing

No automated test framework. Manual testing required:
1. Run `MonitorSwitcher.exe` on Windows 11
2. Right-click (or left-click) tray icon to test menu functionality
3. Test monitor switching with confirmation dialog
4. Test resolution and refresh rate changes via submenus
5. Test hotkeys: Ctrl+Alt+M (menu), Ctrl+Alt+R (restore), Ctrl+Alt+H (HDR toggle), Ctrl+Alt+1..9 (direct switch)
6. Test HDR toggle on HDR-capable and non-HDR monitors
7. Test "Hotkeys enabled" toggle: verify hotkeys can be disabled/enabled and balloon notifications appear
8. Test exit behavior: confirm restore prompt when topology differs from startup
9. Test "Start with Windows" toggle: verify registry entry is created/removed in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`

## Key Win32 APIs Used

### Display Configuration (CCD)
- `GetDisplayConfigBufferSizes` — get required buffer sizes for QueryDisplayConfig
- `QueryDisplayConfig` — enumerate display paths and modes (QDC_ONLY_ACTIVE_PATHS, QDC_ALL_PATHS)
- `SetDisplayConfig` — apply display topology changes (three-attempt strategy)
- `DisplayConfigGetDeviceInfo` — get target names (type 2), source GDI names (type 1), HDR support (type 9)
- `DisplayConfigSetDeviceInfo` — toggle HDR on/off (type 10)

### Display Enumeration
- `EnumDisplaySettingsExW` — enumerate display modes / get current mode
- `EnumDisplayDevicesW` — find primary display by iterating all devices
- `ChangeDisplaySettingsExW` — apply resolution/refresh rate changes

### Registry (HDR)
- `RegOpenKeyExW` / `RegEnumKeyExW` / `RegQueryValueExW` / `RegCloseKey` — read HDR state from `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\MonitorDataStore`

### Registry (Auto-Start)
- `RegOpenKeyExW` / `RegQueryValueExW` / `RegSetValueExW` / `RegDeleteValueW` / `RegCloseKey` — read/write auto-start entry in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`

### System Tray and UI
- `Shell_NotifyIconW` — system tray icon, tooltip, and balloon notifications
- `CreatePopupMenu` / `AppendMenuW` / `TrackPopupMenu` / `DestroyMenu` — context menus with submenus
- `RegisterHotKey` / `UnregisterHotKey` — global hotkeys (Ctrl+Alt+R/M/H + Ctrl+Alt+1..9)
- `CreateMutexW` — enforces single instance
- `MessageBoxW` — confirmation dialogs and error messages

## Architecture

### State Management
- `g_originalTopology[]` — array of `TopologyEntry` structs with `MonitorIdentity identity` and `sourceId`, saved once at startup (primary first), never overwritten
- `IsTopologyChanged()` — queries current active paths and compares against `g_originalTopology` to detect any topology change, regardless of origin (MonitorSwitcher, Windows Settings, etc.)
- `g_selfChanging` — reentrancy guard suppressing WM_DISPLAYCHANGE during our own changes
- Menu lookup tables (`g_menuMonitors`, `g_menuResolutions`, `g_menuFreqs`, `g_menuHdr`) map menu item IDs to action parameters

### Menu Callback Pattern
WM_COMMAND with menu item ID ranges plus parallel static lookup arrays:
- `IDM_MONITOR_BASE` (1000+) — monitor `MonitorIdentity` structs
- `IDM_RES_BASE` (2000+) — resolution entries
- `IDM_FREQ_BASE` (3000+) — frequency values
- `IDM_HDR_BASE` (4000+) — HDR monitor `MonitorIdentity` structs

### Monitor Enumeration (Three-Phase)
1. **Phase 1:** `QDC_ONLY_ACTIVE_PATHS` — get current GDI name, resolution, frequency for active targets
2. **Phase 2:** `QDC_ALL_PATHS` — discover all connected targets (including inactive) with friendly names and device paths. Ghost targets (empty `monitorFriendlyDeviceName`) are filtered out.
3. **Phase 3:** Sort by Adapter LUID + Target ID via `qsort` to mirror Windows Settings numbering (port-centric order).

### Monitor Ordering (Port-Centric)
Monitors are sorted by `{luidHigh, luidLow, targetId}` — this replicates the exact order shown in Windows Settings. The order is tied to the physical GPU port, not the monitor itself: swapping cables between two monitors swaps their numbers (same as Windows). This is deterministic and stable across topology changes.

### Dynamic Hotkeys (Ctrl+Alt+1..9)
`UpdateMonitorHotkeys()` registers `Ctrl+Alt+1` through `Ctrl+Alt+9` based on the number of connected monitors (from `GetAllMonitors`). Called at startup and on every topology change (`TIMER_REBUILD`), as well as after `SetExclusiveMonitor`, `RestoreOriginal`, and before `ShowContextMenu`. Old hotkeys are unregistered before registering new ones. Hotkeys bypass the confirmation dialog and call `SetExclusiveMonitor` directly.

### Hotkey Toggle
`g_hotkeysEnabled` controls whether hotkeys are registered. When toggled via `IDM_TOGGLE_HOTKEYS`, all hotkeys are unregistered first (`UnregisterHotkeys()`), then conditionally re-registered based on the new state. Balloon notifications provide immediate feedback to the user.

### Balloon Notifications
`ShowBalloon()` cancels any pending balloon (sends empty `szInfo`) before displaying a new one, preventing balloon queuing when rapidly toggling features. A one-shot timer (`TIMER_CLOSE_BALLOON`, 3.5 seconds) automatically dismisses the balloon. Uses `NIIF_USER | NIIF_LARGE_ICON` with the application icon.

### Exclusive Mode (Three-Attempt Strategy)
Both `SetExclusiveMonitor` and `RestoreOriginal` use three attempts to apply topology:
1. `SDC_APPLY | SDC_TOPOLOGY_SUPPLIED` — best: reads modes from Windows' persistence database
2. `SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG` — strict, no changes allowed
3. `SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES` — last resort (may cause 60Hz fallback)

### HDR Implementation
- **Reading state:** Registry (`MonitorDataStore\<prefix>\HDREnabled`) is the authoritative source. The CCD API type 9 can lie on dummy plugs.
- **Checking support:** CCD API type 9 (`DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO.advancedColorSupported`)
- **Writing state:** CCD API type 10 (`DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE`)

### Resolution/Refresh Rate
- `ApplyMode` finds the exact driver-enumerated `DEVMODEW` matching (w, h, freq) — it never constructs a DEVMODE from scratch. This preserves driver-specific signal parameters (color depth, timing, etc.).
- `ChangeResolution` tries to preserve the current refresh rate; falls back to highest available.
- Functions always get the GDI name fresh via `GetActiveGdiName()` — never from cached/closure values.

## Conventions

- All code comments and documentation must be written in **English**.
- The C code uses the Win32 Unicode API exclusively (all `W`-suffixed functions, `WCHAR` strings with `L"..."` prefix). The `-municode` compiler flag handles the `UNICODE` / `_UNICODE` defines.
- Comments use `/* C-style */` only (no `//`). Every function has a block comment describing its purpose and any non-obvious behavior.
- Section headers use Unicode box-drawing characters: `/* ─── Section Name ──── */`
- Constants: `UPPER_SNAKE_CASE` via `#define`, grouped by category with prefixes (`TIMER_`, `IDM_`, `HOTKEY_`, `MAX_`).
- Globals: `static g_camelCase`, all initialized to `NULL`/`0`/`FALSE`.
- Functions: `static PascalCase(void)`, all internal linkage.
- Local variables: `camelCase`.
- K&R brace style, 4-space indentation.
- No heap allocation — all stack locals and static globals.
- All git commits must be **signed** (`git commit -S`).
- Git tags must be **lightweight** (`git tag 1.6`), not annotated or signed.

## Important Technical Details

- `IsTopologyChanged()` queries the current active paths and compares them against `g_originalTopology` to detect any topology change. Used to enable/disable "Restore original config" in the menu, in the exit handler, and for tooltip state display.
- `g_pathBuf` and `g_modeBuf` are shared global static arrays for `QueryDisplayConfig` results. They are never used recursively — each call overwrites the previous contents.
- The `Sleep(2000)` in `ExitHandler` is the only blocking sleep in the program. It is acceptable because the app is about to terminate.
- `WM_DISPLAYCHANGE` is debounced with a 2-second one-shot timer (`TIMER_REBUILD`) to let Windows settle after topology changes.
- Balloon notifications via `Shell_NotifyIconW` + `NIF_INFO` integrate automatically with the Windows 10/11 notification center.
- `ShowBalloon()` cancels any pending balloon before showing a new one, preventing queuing when rapidly toggling features (e.g. HDR ON/OFF).
- `TIMER_CLOSE_BALLOON` (3.5 seconds) auto-dismisses balloons. Without this, Windows controls the duration (5-25 seconds depending on accessibility settings).
- The `-ladvapi32` linker flag is required for the registry APIs used by `ReadHdrFromRegistry` and the auto-start feature.
- `WINVER` and `_WIN32_WINNT` must be defined as `0x0601` (Windows 7+) before `#include <windows.h>` for the DISPLAYCONFIG types to be available in MinGW-w64 headers.

## Execution Protocol

**NEVER execute changes without explicit double confirmation:**

1. Always plan first, present to user, wait for approval
2. Ask if the user wants to execute the plan
3. If user confirms, ask once more for final confirmation before proceeding
4. **This applies even in Build Mode** — no exceptions

The user has final say on every action via double confirmation.

## License

GPLv3+. See LICENSE file for full text.

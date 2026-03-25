# AGENTS.md

## Project Overview

**MonitorSwitcher** is a Windows 11 system tray utility that allows users to:
- Switch between monitors exclusively (turn off all others)
- Change resolution and refresh rate
- Toggle HDR per active monitor (via menu) or primary monitor (via hotkey)
- Restore original display configuration

## File Structure

```
MonitorSwitcher.ahk       # Original AutoHotkey v2 implementation (939 lines)
MonitorSwitcher.c         # Native Win32 C implementation (~1640 lines)
MonitorSwitcher.rc        # Windows resource script (embeds the .ico into the .exe)
MonitorSwitcher.ico       # Application icon
Makefile                  # Cross-compilation with MinGW-w64 from Linux
README.md                 # Full documentation
AGENTS.md                 # AI agent coding guidelines
LICENSE                   # GPLv3
.gitignore                # Ignores build artifacts (*.exe, *.o, *.res)
```

Both the `.ahk` and `.c` implementations are functionally equivalent and maintained in parallel.

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

This produces `MonitorSwitcher.exe`, a standalone Windows executable with the icon embedded as a resource. The Makefile links against `-lshell32 -luser32 -lkernel32 -ladvapi32` (advapi32 is needed for registry APIs used by HDR).

## Testing

No automated test framework. Manual testing required:
1. Run `MonitorSwitcher.exe` on Windows 11
2. Right-click (or left-click) tray icon to test menu functionality
3. Test monitor switching with confirmation dialog
4. Test resolution and refresh rate changes via submenus
5. Test hotkeys: Ctrl+Win+M (menu), Ctrl+Win+R (restore), Ctrl+Win+H (HDR toggle)
6. Test HDR toggle on HDR-capable and non-HDR monitors
7. Test exit behavior: confirm restore prompt when in exclusive mode

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

### System Tray and UI
- `Shell_NotifyIconW` — system tray icon, tooltip, and balloon notifications
- `CreatePopupMenu` / `AppendMenuW` / `TrackPopupMenu` / `DestroyMenu` — context menus with submenus
- `RegisterHotKey` / `UnregisterHotKey` — global hotkeys (Ctrl+Win+R/M/H)
- `CreateMutexW` — enforces single instance
- `MessageBoxW` — confirmation dialogs and error messages

## Architecture

### State Management
- `g_originalTopology[]` — array of `{targetId, sourceId}` pairs saved at startup (primary first)
- `g_isExclusive` / `g_activeTargetId` — exclusive mode tracking
- `g_selfChanging` — reentrancy guard suppressing WM_DISPLAYCHANGE during our own changes
- Menu lookup tables (`g_menuMonitorIds`, `g_menuResolutions`, `g_menuFreqs`, `g_menuHdrIds`) map menu item IDs to action parameters

### Menu Callback Pattern (C)
AHK uses closures with `.Bind()`. The C port uses WM_COMMAND with menu item ID ranges plus parallel static lookup arrays:
- `IDM_MONITOR_BASE` (1000+) — monitor targetIds
- `IDM_RES_BASE` (2000+) — resolution entries
- `IDM_FREQ_BASE` (3000+) — frequency values
- `IDM_HDR_BASE` (4000+) — HDR monitor targetIds

### Monitor Enumeration (Two-Phase)
1. **Phase 1:** `QDC_ONLY_ACTIVE_PATHS` — get current GDI name, resolution, frequency for active targets
2. **Phase 2:** `QDC_ALL_PATHS` — discover all connected targets (including inactive) with friendly names and device paths

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
- The project is intentionally single-file (one `.c` file). Do not split it into multiple source files.
- Comments use `/* C-style */` only (no `//`). Every function has a block comment describing its purpose and any non-obvious behavior.
- Section headers use Unicode box-drawing characters: `/* ─── Section Name ──── */`
- Constants: `UPPER_SNAKE_CASE` via `#define`, grouped by category with prefixes (`TIMER_`, `IDM_`, `HOTKEY_`, `MAX_`).
- Globals: `static g_camelCase`, all initialized to `NULL`/`0`/`FALSE`.
- Functions: `static PascalCase(void)`, all internal linkage.
- Local variables: `camelCase`.
- K&R brace style, 4-space indentation.
- No heap allocation — all stack locals and static globals.
- All git commits must be **signed** (`git commit -S`).

## Important Technical Details

- The `ValidateExclusiveState()` function checks whether the exclusive state is still valid (target still active and the only active monitor). Called both on `WM_DISPLAYCHANGE` (debounced) and before building the context menu.
- `g_pathBuf` and `g_modeBuf` are shared global static arrays for `QueryDisplayConfig` results. They are never used recursively — each call overwrites the previous contents.
- The `Sleep(2000)` in `ExitHandler` is the only blocking sleep in the program. It is acceptable because the app is about to terminate.
- `WM_DISPLAYCHANGE` is debounced with a 1-second one-shot timer (`TIMER_REBUILD`) to let Windows settle after topology changes.
- Balloon notifications via `Shell_NotifyIconW` + `NIF_INFO` integrate automatically with the Windows 10/11 notification center.
- The `-ladvapi32` linker flag is required for the registry APIs used by `ReadHdrFromRegistry`.
- `WINVER` and `_WIN32_WINNT` must be defined as `0x0601` (Windows 7+) before `#include <windows.h>` for the DISPLAYCONFIG types to be available in MinGW-w64 headers.

## License

GPLv3+. See LICENSE file for full text.

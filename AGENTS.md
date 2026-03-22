# MonitorSwitcher - AGENTS.md

## Project Overview

MonitorSwitcher is a Windows 11 system tray utility written in AutoHotkey v2. It allows users to:
- Switch between monitors exclusively (turn off all others)
- Change resolution and refresh rate
- Toggle HDR on the active monitor
- Restore original display configuration

Single-file application: `MonitorSwitcher.ahk`

## Commands

### Running
```bash
# Run the script (requires AutoHotkey v2.0+ installed)
AutoHotkey64.exe MonitorSwitcher.ahk
# Or double-click the .ahk file in Windows Explorer
```

### Building (Optional)
AutoHotkey scripts can be compiled to standalone executables:
```bash
Ahk2Exe.exe /in "MonitorSwitcher.ahk" /out "MonitorSwitcher.exe"
```

### Testing
No automated test framework. Manual testing required:
1. Run the script on Windows 11
2. Right-click tray icon to test menu functionality
3. Test monitor switching, resolution/refresh rate changes
4. Test hotkeys: Ctrl+Win+M (menu), Ctrl+Win+R (restore), Ctrl+Win+H (HDR toggle)
5. Test HDR toggle on HDR-capable and non-HDR monitors

### Linting
No formal linter for AHK. Review code manually for:
- Correct DllCall signatures
- Proper Buffer sizes for Win32 structs
- Valid AHK v2 syntax (see: https://www.autohotkey.com/docs/v2/)

## Code Style Guidelines

### File Structure
```
/**
 * MonitorSwitcher vX.Y
 * Brief description
 * License: GPLv3+
 */

#Requires AutoHotkey v2.0
#SingleInstance Force
Persistent

; ==================================================================
;  CONSTANTS
; ==================================================================

; ==================================================================
;  GLOBAL STATE
; ==================================================================

; ==================================================================
;  SECTION NAME
; ==================================================================
```

### Naming Conventions

**Constants** - UPPER_CASE with global prefix:
```ahk
global SIZEOF_DD    := 840
global OFF_DD_NAME  := 4       ; Inline comment explaining purpose
global QDC_ALL_PATHS := 1
```

**Global Variables** - PascalCase with global prefix:
```ahk
global OriginalPaths     := ""
global OriginalPathCount := 0
global IsExclusive       := false
```

**Functions** - PascalCase:
```ahk
GetAllMonitors()
GetTargetInfo(luidLow, luidHigh, targetId)
SetExclusiveMonitor(targetId)
ToggleHdr()
```

**Local Variables** - camelCase:
```ahk
monitors := []
seenTargets := Map()
gdiName := GetActiveGdiName()
```

### Comments

**Block comments** for function documentation (JSDoc-style):
```ahk
/**
 * Brief description of function.
 * More details if needed.
 * @param paramName - description (optional format)
 * @returns description
 */
```

**Section headers** use equal signs:
```ahk
; ==================================================================
;  SECTION NAME
; ==================================================================
```

**Inline comments** explain non-obvious logic:
```ahk
if ret != 0
    MsgBox("Error code " ret, "Title", "Icon!")
; SDC_ALLOW_CHANGES causes 60Hz fallback - use as last resort
```

### Imports

No external dependencies. Uses only:
- AutoHotkey v2 built-in functions
- Windows API via DllCall

### Windows API (DllCall) Patterns

**Buffer allocation for structs:**
```ahk
global SIZEOF_PATH := 72
paths := Buffer(SIZEOF_PATH * count, 0)
```

**NumPut for struct fields:**
```ahk
NumPut("UInt", value, buffer, OFFSET_CONSTANT)
NumPut("UShort", SIZEOF_DM, dm, OFF_DM_SIZE)
```

**NumGet for reading struct fields:**
```ahk
tgtId := NumGet(ap, off + PATH_TGT_ID, "UInt")
```

**DllCall signature format:**
```ahk
DllCall("FunctionName", "Type1", param1, "Type2", param2, "Int")
DllCall("User32\EnumDisplaySettingsExW", "Str", gdiName, "Int", modeIdx, "Ptr", dm, "UInt", 0, "Int")
```

**String handling with Buffer:**
```ahk
return StrGet(buf.Ptr + offset, charCount)
```

### Error Handling

**Try blocks for API calls:**
```ahk
try {
    DllCall("GetDisplayConfigBufferSizes", "UInt", QDC_ACTIVE_PATHS,
            "UInt*", &apc := 0, "UInt*", &amc := 0)
    ; ...
}
```

**User-facing errors via MsgBox:**
```ahk
if ret != 0
    MsgBox("Error description (code " ret ")", "MonitorSwitcher", "Icon!")
```

**Return error codes from functions:**
```ahk
if !DllCall("Function", ...)
    return ""
return result
```

### AHK v2 Specific Patterns

**Global variable declaration required:**
```ahk
global OriginalPaths     := ""
global OriginalPathCount := 0
global IsExclusive       := false  ; must use assignment with :=
```

**Fat arrow functions for hotkeys:**
```ahk
#^r:: {
    ; hotkey code
}
```

**Menu callbacks with closures (bind pattern):**
```ahk
tray.Add(label, ((t, *) => ConfirmSwitch(t)).Bind(tid))
```

**Loop syntax:**
```ahk
Loop count {    ; with count
    ; body
}
Loop {           ; infinite
    if condition
        break
}
```

**Map and Array usage:**
```ahk
map := Map()
map[key] := value
if map.Has(key) { ... }

arr := []
arr.Push(item)
length := arr.Length
```

### Types

AHK v2 is dynamically typed. Common types:
- `Str` - strings
- `Int`, `UInt` - 32-bit integers
- `Ptr`, `UPtr` - pointer-sized
- `UShort` - 16-bit unsigned
- `Float`, `Double` - floating point
- `Buffer` - memory buffer for structs

## Architecture Notes

### Windows APIs Used
- `SetDisplayConfig` - Activate/deactivate monitors (topology)
- `ChangeDisplaySettingsExW` - Change resolution/refresh rate
- `QueryDisplayConfig` - Enumerate displays
- `DisplayConfigGetDeviceInfo` - Get friendly names, device paths, and HDR support info
- `DisplayConfigSetDeviceInfo` - Toggle HDR on/off (type 10)
- `EnumDisplaySettingsExW` - Get available modes
- `EnumDisplayDevicesW` - Get primary display info

### Key Data Structures
- DISPLAYCONFIG_PATH_INFO (72 bytes)
- DISPLAYCONFIG_MODE_INFO (64 bytes)
- DEVMODEW (220+ bytes)
- DISPLAY_DEVICEW (840 bytes)

### State Management
- Global state tracks: original config, exclusive mode, active target
- `SelfChanging` flag prevents WM_DISPLAYCHANGE handler recursion
- Menu rebuilt on every display change event

### HDR Implementation
- `ReadHdrFromRegistry(devPath)` - Reads HDR state from Windows registry (authoritative source)
- `IsHdrSupported(luidLow, luidHigh, targetId)` - Checks if monitor supports HDR via CCD API type 9
- `ToggleHdr()` - Toggles HDR on active monitor via CCD API type 10

The CCD API (`DisplayConfigGetDeviceInfo` type 9) can report incorrect HDR state on some devices (e.g., dummy plugs), so the registry is used as the authoritative source for reading HDR state. Writing HDR state uses `DisplayConfigSetDeviceInfo` type 10.

### Key Constants
- `DC_INFO_GET_ADVANCED_COLOR := 9` - CCD API type for querying HDR support
- `DC_INFO_SET_ADVANCED_COLOR := 10` - CCD API type for setting HDR state
- `MONITOR_DATA_STORE` - Registry path for monitor HDR data
- `CDS_UPDATEREGISTRY := 0x00000001` - Flag for `ChangeDisplaySettingsExW`

## License

GPLv3+. See LICENSE file for full text.
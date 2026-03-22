/**
 * MonitorSwitcher v1.0
 * System tray utility to switch between monitors exclusively,
 * change resolution/refresh rate, and toggle HDR on Windows 11.
 * License: GPLv3+
 */

#Requires AutoHotkey v2.0
#SingleInstance Force
Persistent

; ==================================================================
;  CONSTANTS
; ==================================================================

; --- DISPLAY_DEVICEW (840 bytes) ---
global SIZEOF_DD    := 840
global OFF_DD_NAME  := 4       ; WCHAR[32]
global OFF_DD_FLAGS := 324     ; DWORD

; --- DEVMODEW (220 bytes min) ---
global SIZEOF_DM     := 220
global OFF_DM_SIZE   := 68     ; WORD  dmSize
global OFF_DM_WIDTH  := 172    ; DWORD dmPelsWidth
global OFF_DM_HEIGHT := 176    ; DWORD dmPelsHeight
global OFF_DM_FREQ   := 184    ; DWORD dmDisplayFrequency

global ENUM_CURRENT := -1

; --- DISPLAYCONFIG ---
global SIZEOF_PATH := 72
global SIZEOF_MODE := 64

global PATH_SRC_LUID_LOW   := 0
global PATH_SRC_LUID_HIGH  := 4
global PATH_SRC_ID         := 8
global PATH_SRC_MODEIDX    := 12
global PATH_TGT_LUID_LOW   := 20
global PATH_TGT_LUID_HIGH  := 24
global PATH_TGT_ID         := 28
global PATH_TGT_MODEIDX    := 32
global PATH_FLAGS          := 68

global QDC_ALL_PATHS    := 1
global QDC_ACTIVE_PATHS := 2

global SDC_TOPOLOGY_SUPPLIED  := 0x00000010
global SDC_USE_SUPPLIED       := 0x00000020
global SDC_APPLY              := 0x00000080
global SDC_SAVE_TO_DATABASE   := 0x00000200
global SDC_ALLOW_CHANGES      := 0x00000400

global DC_PATH_ACTIVE       := 0x00000001
global DC_MODE_IDX_INVALID  := 0xFFFFFFFF

global DD_PRIMARY  := 0x00000004

; --- ChangeDisplaySettingsEx flags ---
global CDS_UPDATEREGISTRY := 0x00000001

; --- HDR (Advanced Color) ---
global DC_INFO_GET_ADVANCED_COLOR := 9
global DC_INFO_SET_ADVANCED_COLOR := 10
global MONITOR_DATA_STORE := "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\MonitorDataStore"

; ==================================================================
;  GLOBAL STATE
; ==================================================================

global OriginalPaths     := ""     ; Saved paths buffer before exclusive mode
global OriginalModes     := ""     ; Saved modes buffer before exclusive mode
global OriginalPathCount := 0      ; Number of paths in saved config
global OriginalModeCount := 0      ; Number of modes in saved config
global IsExclusive       := false  ; Whether we are in single-monitor mode
global ActiveTargetId    := 0      ; targetId of the exclusive monitor
global SelfChanging      := false  ; Suppresses WM_DISPLAYCHANGE during our own changes

; ==================================================================
;  DISPLAYCONFIG HELPERS
; ==================================================================

/**
 * Returns an array of monitor objects, one per physically connected display.
 * Each object: {targetId, name, devPath, luidLow, luidHigh, isActive, gdiName, w, h, freq}
 * Uses QDC_ACTIVE_PATHS for active info, QDC_ALL_PATHS for discovery.
 */
GetAllMonitors() {
    monitors := []
    seenTargets := Map()

    ; 1) Get active target info
    activeInfo := Map()
    try {
        DllCall("GetDisplayConfigBufferSizes", "UInt", QDC_ACTIVE_PATHS,
                "UInt*", &apc := 0, "UInt*", &amc := 0)
        if apc > 0 {
            ap := Buffer(SIZEOF_PATH * apc, 0)
            am := Buffer(SIZEOF_MODE * amc, 0)
            if 0 = DllCall("QueryDisplayConfig", "UInt", QDC_ACTIVE_PATHS,
                           "UInt*", &apc, "Ptr", ap,
                           "UInt*", &amc, "Ptr", am, "Ptr", 0)
            {
                off := 0
                Loop apc {
                    tgtId    := NumGet(ap, off + PATH_TGT_ID, "UInt")
                    srcLuidL := NumGet(ap, off + PATH_SRC_LUID_LOW, "UInt")
                    srcLuidH := NumGet(ap, off + PATH_SRC_LUID_HIGH, "UInt")
                    srcId    := NumGet(ap, off + PATH_SRC_ID, "UInt")
                    gdi      := GetSourceGdiName(srcLuidL, srcLuidH, srcId)
                    w := 0, h := 0, freq := 0
                    if gdi != "" {
                        dm := Buffer(SIZEOF_DM, 0)
                        NumPut("UShort", SIZEOF_DM, dm, OFF_DM_SIZE)
                        if DllCall("User32\EnumDisplaySettingsExW",
                                   "Str", gdi, "Int", ENUM_CURRENT, "Ptr", dm, "UInt", 0, "Int")
                        {
                            w    := NumGet(dm, OFF_DM_WIDTH, "UInt")
                            h    := NumGet(dm, OFF_DM_HEIGHT, "UInt")
                            freq := NumGet(dm, OFF_DM_FREQ, "UInt")
                        }
                    }
                    if !activeInfo.Has(tgtId)
                        activeInfo[tgtId] := {gdiName: gdi, w: w, h: h, freq: freq}
                    off += SIZEOF_PATH
                }
            }
        }
    }

    ; 2) Enumerate ALL paths to find every target with a friendly name
    try {
        DllCall("GetDisplayConfigBufferSizes", "UInt", QDC_ALL_PATHS,
                "UInt*", &pc := 0, "UInt*", &mc := 0)
        if pc > 0 {
            paths := Buffer(SIZEOF_PATH * pc, 0)
            modes := Buffer(SIZEOF_MODE * mc, 0)
            if 0 = DllCall("QueryDisplayConfig", "UInt", QDC_ALL_PATHS,
                           "UInt*", &pc, "Ptr", paths,
                           "UInt*", &mc, "Ptr", modes, "Ptr", 0)
            {
                off := 0
                Loop pc {
                    tgtId    := NumGet(paths, off + PATH_TGT_ID, "UInt")
                    tgtLuidL := NumGet(paths, off + PATH_TGT_LUID_LOW, "UInt")
                    tgtLuidH := NumGet(paths, off + PATH_TGT_LUID_HIGH, "UInt")
                    if !seenTargets.Has(tgtId) {
                        info := GetTargetInfo(tgtLuidL, tgtLuidH, tgtId)
                        if info.name != "" {
                            seenTargets[tgtId] := true
                            mon := {}
                            mon.targetId := tgtId
                            mon.name     := info.name
                            mon.devPath  := info.devPath
                            mon.luidLow  := tgtLuidL
                            mon.luidHigh := tgtLuidH
                            mon.isActive := activeInfo.Has(tgtId)
                            if mon.isActive {
                                ai := activeInfo[tgtId]
                                mon.gdiName  := ai.gdiName
                                mon.w        := ai.w
                                mon.h        := ai.h
                                mon.freq     := ai.freq
                            } else {
                                mon.gdiName  := ""
                                mon.w := 0, mon.h := 0, mon.freq := 0
                            }
                            monitors.Push(mon)
                        }
                    }
                    off += SIZEOF_PATH
                }
            }
        }
    }
    return monitors
}

/** Returns {name, devPath} for a target via DisplayConfigGetDeviceInfo type 2. */
GetTargetInfo(luidLow, luidHigh, targetId) {
    buf := Buffer(420, 0)
    NumPut("UInt", 2, buf, 0)
    NumPut("UInt", 420, buf, 4)
    NumPut("UInt", luidLow, buf, 8)
    NumPut("UInt", luidHigh, buf, 12)
    NumPut("UInt", targetId, buf, 16)
    if 0 = DllCall("DisplayConfigGetDeviceInfo", "Ptr", buf, "Int")
        return {name: StrGet(buf.Ptr + 36, 64), devPath: StrGet(buf.Ptr + 164, 128)}
    return {name: "", devPath: ""}
}

/** Returns the GDI device name (e.g. "\\.\DISPLAY1") via DisplayConfigGetDeviceInfo type 1. */
GetSourceGdiName(luidLow, luidHigh, sourceId) {
    buf := Buffer(84, 0)
    NumPut("UInt", 1, buf, 0)
    NumPut("UInt", 84, buf, 4)
    NumPut("UInt", luidLow, buf, 8)
    NumPut("UInt", luidHigh, buf, 12)
    NumPut("UInt", sourceId, buf, 16)
    if 0 = DllCall("DisplayConfigGetDeviceInfo", "Ptr", buf, "Int")
        return StrGet(buf.Ptr + 20, 32)
    return ""
}

/** Returns the GDI name of the current primary display. Always fresh. */
GetActiveGdiName() {
    idx := 0
    Loop {
        dd := Buffer(SIZEOF_DD, 0)
        NumPut("UInt", SIZEOF_DD, dd, 0)
        if !DllCall("User32\EnumDisplayDevicesW", "Ptr", 0, "UInt", idx, "Ptr", dd, "UInt", 0, "Int")
            break
        if (NumGet(dd, OFF_DD_FLAGS, "UInt") & DD_PRIMARY)
            return StrGet(dd.Ptr + OFF_DD_NAME, 32)
        idx++
    }
    return ""
}

/** Returns {w, h, freq} for the active mode of a GDI device. */
GetCurrentMode(gdiName) {
    dm := Buffer(SIZEOF_DM, 0)
    NumPut("UShort", SIZEOF_DM, dm, OFF_DM_SIZE)
    if DllCall("User32\EnumDisplaySettingsExW",
               "Str", gdiName, "Int", ENUM_CURRENT, "Ptr", dm, "UInt", 0, "Int")
        return {w: NumGet(dm, OFF_DM_WIDTH, "UInt"), h: NumGet(dm, OFF_DM_HEIGHT, "UInt"),
                freq: NumGet(dm, OFF_DM_FREQ, "UInt")}
    return ""
}

; ==================================================================
;  EXCLUSIVE MODE — uses SetDisplayConfig
; ==================================================================

/** Saves the current active display config (paths + modes) for later restoration. */
SaveConfig() {
    global OriginalPaths, OriginalModes, OriginalPathCount, OriginalModeCount
    try {
        DllCall("GetDisplayConfigBufferSizes", "UInt", QDC_ACTIVE_PATHS,
                "UInt*", &pc := 0, "UInt*", &mc := 0)
        p := Buffer(SIZEOF_PATH * pc, 0)
        m := Buffer(SIZEOF_MODE * mc, 0)
        if 0 = DllCall("QueryDisplayConfig", "UInt", QDC_ACTIVE_PATHS,
                       "UInt*", &pc, "Ptr", p,
                       "UInt*", &mc, "Ptr", m, "Ptr", 0)
        {
            OriginalPathCount := pc
            OriginalModeCount := mc
            OriginalPaths := Buffer(SIZEOF_PATH * pc, 0)
            OriginalModes := Buffer(SIZEOF_MODE * mc, 0)
            DllCall("msvcrt\memcpy", "Ptr", OriginalPaths, "Ptr", p, "UPtr", SIZEOF_PATH * pc)
            DllCall("msvcrt\memcpy", "Ptr", OriginalModes, "Ptr", m, "UPtr", SIZEOF_MODE * mc)
            return
        }
    }
    OriginalPathCount := 0
}

/**
 * Activates ONLY the specified monitor (by targetId), disabling all others.
 * Three attempts, avoids SDC_ALLOW_CHANGES (causes 60Hz fallback):
 *   1) SDC_TOPOLOGY_SUPPLIED — reads modes from Windows' persistence database.
 *   2) SDC_USE_SUPPLIED — best mode logic, strict.
 *   3) SDC_USE_SUPPLIED | SDC_ALLOW_CHANGES — last resort.
 */
SetExclusiveMonitor(targetId) {
    global IsExclusive, ActiveTargetId, SelfChanging

    if !IsExclusive
        SaveConfig()

    SelfChanging := true

    DllCall("GetDisplayConfigBufferSizes", "UInt", QDC_ALL_PATHS,
            "UInt*", &pc := 0, "UInt*", &mc := 0)
    if pc = 0 {
        SelfChanging := false
        return
    }
    paths := Buffer(SIZEOF_PATH * pc, 0)
    modes := Buffer(SIZEOF_MODE * mc, 0)
    if 0 != DllCall("QueryDisplayConfig", "UInt", QDC_ALL_PATHS,
                    "UInt*", &pc, "Ptr", paths,
                    "UInt*", &mc, "Ptr", modes, "Ptr", 0)
    {
        SelfChanging := false
        return
    }

    found := false
    off := 0
    Loop pc {
        if NumGet(paths, off + PATH_TGT_ID, "UInt") = targetId {
            singlePath := Buffer(SIZEOF_PATH, 0)
            DllCall("msvcrt\memcpy", "Ptr", singlePath, "Ptr", paths.Ptr + off, "UPtr", SIZEOF_PATH)
            NumPut("UInt", DC_PATH_ACTIVE, singlePath, PATH_FLAGS)
            NumPut("UInt", DC_MODE_IDX_INVALID, singlePath, PATH_SRC_MODEIDX)
            NumPut("UInt", DC_MODE_IDX_INVALID, singlePath, PATH_TGT_MODEIDX)

            ret := DllCall("SetDisplayConfig", "UInt", 1, "Ptr", singlePath,
                           "UInt", 0, "Ptr", 0,
                           "UInt", SDC_APPLY | SDC_TOPOLOGY_SUPPLIED, "Int")
            if ret != 0
                ret := DllCall("SetDisplayConfig", "UInt", 1, "Ptr", singlePath,
                               "UInt", 0, "Ptr", 0,
                               "UInt", SDC_APPLY | SDC_USE_SUPPLIED, "Int")
            if ret != 0
                ret := DllCall("SetDisplayConfig", "UInt", 1, "Ptr", singlePath,
                               "UInt", 0, "Ptr", 0,
                               "UInt", SDC_APPLY | SDC_USE_SUPPLIED | SDC_ALLOW_CHANGES, "Int")

            if ret != 0
                MsgBox("SetDisplayConfig failed (error " ret ")", "MonitorSwitcher", "Icon!")
            else
                SaveActiveConfigToDatabase()

            found := true
            break
        }
        off += SIZEOF_PATH
    }

    SelfChanging := false

    if !found {
        MsgBox("Target " targetId " not found.", "MonitorSwitcher", "Icon!")
        return
    }

    IsExclusive := true
    ActiveTargetId := targetId
    BuildMenu()
}

/** Saves current active config to Windows' persistence database. */
SaveActiveConfigToDatabase() {
    try {
        DllCall("GetDisplayConfigBufferSizes", "UInt", QDC_ACTIVE_PATHS,
                "UInt*", &pc := 0, "UInt*", &mc := 0)
        if pc = 0
            return
        p := Buffer(SIZEOF_PATH * pc, 0)
        m := Buffer(SIZEOF_MODE * mc, 0)
        if 0 != DllCall("QueryDisplayConfig", "UInt", QDC_ACTIVE_PATHS,
                        "UInt*", &pc, "Ptr", p,
                        "UInt*", &mc, "Ptr", m, "Ptr", 0)
            return
        DllCall("SetDisplayConfig", "UInt", pc, "Ptr", p,
                "UInt", mc, "Ptr", m,
                "UInt", SDC_APPLY | SDC_USE_SUPPLIED | SDC_SAVE_TO_DATABASE, "Int")
    }
}

/** Restores the display config saved before entering exclusive mode. */
RestoreOriginal() {
    global OriginalPaths, OriginalModes, OriginalPathCount, OriginalModeCount
    global IsExclusive, ActiveTargetId, SelfChanging

    if !IsExclusive || OriginalPathCount = 0 {
        MsgBox("Nothing to restore.", "MonitorSwitcher", "Icon!")
        return
    }

    SelfChanging := true
    ret := DllCall("SetDisplayConfig",
                   "UInt", OriginalPathCount, "Ptr", OriginalPaths,
                   "UInt", OriginalModeCount, "Ptr", OriginalModes,
                   "UInt", SDC_APPLY | SDC_USE_SUPPLIED | SDC_ALLOW_CHANGES, "Int")
    SelfChanging := false

    if ret != 0
        MsgBox("Restore failed (error " ret ")", "MonitorSwitcher", "Icon!")

    IsExclusive := false
    ActiveTargetId := 0
    BuildMenu()
}

; ==================================================================
;  RESOLUTION AND REFRESH RATE
; ==================================================================

/** Returns unique resolutions [{w, h, label}] for a GDI device, sorted by pixel count descending. */
GetAvailableResolutions(gdiName) {
    seen := Map()
    res := []
    modeIdx := 0
    Loop {
        dm := Buffer(SIZEOF_DM, 0)
        NumPut("UShort", SIZEOF_DM, dm, OFF_DM_SIZE)
        if !DllCall("User32\EnumDisplaySettingsExW",
                     "Str", gdiName, "Int", modeIdx, "Ptr", dm, "UInt", 0, "Int")
            break
        w := NumGet(dm, OFF_DM_WIDTH, "UInt")
        h := NumGet(dm, OFF_DM_HEIGHT, "UInt")
        key := w "x" h
        if !seen.Has(key) {
            seen[key] := true
            res.Push({w: w, h: h, label: key})
        }
        modeIdx++
    }
    n := res.Length
    if n >= 2 {
        Loop n - 1 {
            i := A_Index
            Loop n - i {
                j := A_Index
                if (res[j].w * res[j].h) < (res[j+1].w * res[j+1].h) {
                    tmp := res[j], res[j] := res[j+1], res[j+1] := tmp
                }
            }
        }
    }
    return res
}

/** Returns unique refresh rates [Hz] for a given resolution, sorted descending. */
GetAvailableFreqs(gdiName, targetW, targetH) {
    seen := Map()
    freqs := []
    modeIdx := 0
    Loop {
        dm := Buffer(SIZEOF_DM, 0)
        NumPut("UShort", SIZEOF_DM, dm, OFF_DM_SIZE)
        if !DllCall("User32\EnumDisplaySettingsExW",
                     "Str", gdiName, "Int", modeIdx, "Ptr", dm, "UInt", 0, "Int")
            break
        w := NumGet(dm, OFF_DM_WIDTH, "UInt")
        h := NumGet(dm, OFF_DM_HEIGHT, "UInt")
        f := NumGet(dm, OFF_DM_FREQ, "UInt")
        if (w = targetW && h = targetH && !seen.Has(f)) {
            seen[f] := true
            freqs.Push(f)
        }
        modeIdx++
    }
    n := freqs.Length
    if n >= 2 {
        Loop n - 1 {
            i := A_Index
            Loop n - i {
                j := A_Index
                if freqs[j] < freqs[j+1] {
                    tmp := freqs[j], freqs[j] := freqs[j+1], freqs[j+1] := tmp
                }
            }
        }
    }
    return freqs
}

/**
 * Changes resolution. Gets the current GDI name FRESH (not from a closure).
 * Tries to keep the current refresh rate; falls back to highest available.
 */
ChangeResolution(newW, newH) {
    gdiName := GetActiveGdiName()
    if gdiName = ""
        return
    cur := GetCurrentMode(gdiName)
    targetFreq := (cur != "") ? cur.freq : 60
    freqs := GetAvailableFreqs(gdiName, newW, newH)
    bestFreq := (freqs.Length > 0) ? freqs[1] : 60
    for f in freqs {
        if f = targetFreq {
            bestFreq := f
            break
        }
    }
    ApplyMode(gdiName, newW, newH, bestFreq)
}

/** Changes refresh rate. Gets the current GDI name and resolution FRESH. */
ChangeFrequency(newFreq) {
    gdiName := GetActiveGdiName()
    if gdiName = ""
        return
    cur := GetCurrentMode(gdiName)
    if cur = ""
        return
    ApplyMode(gdiName, cur.w, cur.h, newFreq)
}

/**
 * Applies a display mode using ChangeDisplaySettingsExW.
 * This is the correct API for changing resolution/frequency on an already-active monitor.
 * (SetDisplayConfig is for topology changes — activating/deactivating monitors.)
 * Enumerates available modes to find an exact match, then applies it.
 */
ApplyMode(gdiName, w, h, freq) {
    global SelfChanging

    ; Find the exact DEVMODE that matches w, h, freq
    modeIdx := 0
    Loop {
        dm := Buffer(SIZEOF_DM, 0)
        NumPut("UShort", SIZEOF_DM, dm, OFF_DM_SIZE)
        if !DllCall("User32\EnumDisplaySettingsExW",
                     "Str", gdiName, "Int", modeIdx, "Ptr", dm, "UInt", 0, "Int")
            break
        if (NumGet(dm, OFF_DM_WIDTH, "UInt") = w
            && NumGet(dm, OFF_DM_HEIGHT, "UInt") = h
            && NumGet(dm, OFF_DM_FREQ, "UInt") = freq)
        {
            SelfChanging := true
            ret := DllCall("User32\ChangeDisplaySettingsExW",
                           "Str", gdiName, "Ptr", dm, "Ptr", 0,
                           "UInt", CDS_UPDATEREGISTRY, "Ptr", 0, "Int")
            SelfChanging := false

            if ret != 0
                MsgBox("Error applying " w "x" h " @ " freq "Hz (code " ret ")", "MonitorSwitcher", "Icon!")
            return
        }
        modeIdx++
    }
    MsgBox("Mode " w "x" h " @ " freq "Hz not available.", "MonitorSwitcher", "Icon!")
}

; ==================================================================
;  HDR
; ==================================================================

/**
 * Reads the real HDR state from the Windows registry (MonitorDataStore).
 * The CCD API (type 9) can report incorrect values on some devices (e.g. dummy plugs),
 * so the registry is the authoritative source for reading.
 * Returns {found: bool, enabled: bool}.
 */
ReadHdrFromRegistry(devPath) {
    parts := StrSplit(devPath, "#")
    if parts.Length < 2
        return {found: false, enabled: false}
    prefix := parts[2]
    try {
        loop Reg, MONITOR_DATA_STORE, "K" {
            if SubStr(A_LoopRegName, 1, StrLen(prefix)) = prefix {
                try {
                    val := RegRead(MONITOR_DATA_STORE "\" A_LoopRegName, "HDREnabled")
                    return {found: true, enabled: (val = 1)}
                }
            }
        }
    }
    return {found: false, enabled: false}
}

/**
 * Checks if a monitor supports HDR via DisplayConfigGetDeviceInfo type 9.
 * Returns true if supported, false otherwise.
 */
IsHdrSupported(luidLow, luidHigh, targetId) {
    ci := Buffer(32, 0)
    NumPut("UInt", DC_INFO_GET_ADVANCED_COLOR, ci, 0)
    NumPut("UInt", 32, ci, 4)
    NumPut("UInt", luidLow, ci, 8)
    NumPut("UInt", luidHigh, ci, 12)
    NumPut("UInt", targetId, ci, 16)
    if 0 = DllCall("DisplayConfigGetDeviceInfo", "Ptr", ci, "Int")
        return (NumGet(ci, 20, "UInt") & 0x1) != 0
    return false
}

/**
 * Toggles HDR on the active monitor.
 * Reads state from the registry (authoritative), writes via CCD API type 10.
 * All data queried fresh — nothing cached.
 */
ToggleHdr() {
    monitors := GetAllMonitors()
    activeMon := ""
    for mon in monitors {
        if mon.isActive {
            activeMon := mon
            break
        }
    }

    if activeMon = "" {
        ToolTip("No active monitor detected")
        SetTimer(() => ToolTip(), -2500)
        return
    }

    if !IsHdrSupported(activeMon.luidLow, activeMon.luidHigh, activeMon.targetId) {
        ToolTip(activeMon.name ": HDR not supported")
        SetTimer(() => ToolTip(), -2500)
        return
    }

    state := ReadHdrFromRegistry(activeMon.devPath)
    if !state.found {
        ToolTip(activeMon.name ": HDR state not found in registry")
        SetTimer(() => ToolTip(), -2500)
        return
    }

    ; Write the opposite state via CCD API type 10
    newState := !state.enabled
    si := Buffer(24, 0)
    NumPut("UInt", DC_INFO_SET_ADVANCED_COLOR, si, 0)
    NumPut("UInt", 24, si, 4)
    NumPut("UInt", activeMon.luidLow, si, 8)
    NumPut("UInt", activeMon.luidHigh, si, 12)
    NumPut("UInt", activeMon.targetId, si, 16)
    NumPut("UInt", newState ? 1 : 0, si, 20)

    ret := DllCall("DisplayConfigSetDeviceInfo", "Ptr", si, "Int")
    if ret != 0 {
        ToolTip("HDR toggle failed (error " ret ")")
        SetTimer(() => ToolTip(), -2500)
        return
    }

    label := newState ? "ON" : "OFF"
    ToolTip(activeMon.name ": HDR " label)
    SetTimer(() => ToolTip(), -2500)
}

; ==================================================================
;  TRAY MENU
; ==================================================================

/**
 * Builds the tray menu from scratch with fresh data.
 * Called on every menu open, display change, and after every action.
 */
BuildMenu() {
    global IsExclusive, ActiveTargetId

    tray := A_TrayMenu
    tray.Delete()

    tray.Add("MonitorSwitcher v1.0", (*) => "")
    tray.Disable("MonitorSwitcher v1.0")
    tray.Add()

    ; --- Monitors ---
    monitors := GetAllMonitors()

    ; If we think we're in exclusive mode, verify the target is still active.
    ; If the user switched monitors externally (e.g. via Windows Settings),
    ; our state is stale — reset it.
    if IsExclusive {
        exclusiveStillActive := false
        for mon in monitors {
            if (mon.targetId = ActiveTargetId && mon.isActive) {
                exclusiveStillActive := true
                break
            }
        }
        if !exclusiveStillActive {
            IsExclusive := false
            ActiveTargetId := 0
        }
    }

    for mon in monitors {
        label := mon.name
        if mon.isActive && mon.w > 0
            label .= "  |  " mon.w "x" mon.h " @ " mon.freq "Hz"
        if !mon.isActive
            label .= "  (off)"

        if mon.isActive
            label := (IsExclusive ? ">>  " : " *  ") label
        else
            label := "     " label

        tid := mon.targetId
        tray.Add(label, ((t, *) => ConfirmSwitch(t)).Bind(tid))
    }

    ; --- Resolution, Frequency (fresh GDI query) ---
    gdiName := GetActiveGdiName()
    curMode := (gdiName != "") ? GetCurrentMode(gdiName) : ""

    if (gdiName != "" && curMode != "") {
        tray.Add()

        resMenu := Menu()
        for res in GetAvailableResolutions(gdiName) {
            rl := (res.w = curMode.w && res.h = curMode.h) ? ">>  " res.label : res.label
            rw := res.w, rh := res.h
            ; NO gdiName in closure — ChangeResolution gets it fresh
            resMenu.Add(rl, ((w, h, *) => ChangeResolution(w, h)).Bind(rw, rh))
        }
        tray.Add("Resolution  [" curMode.w "x" curMode.h "]", resMenu)

        freqMenu := Menu()
        for f in GetAvailableFreqs(gdiName, curMode.w, curMode.h) {
            fl := (f = curMode.freq) ? ">>  " f "Hz" : f "Hz"
            fc := f
            ; NO gdiName in closure — ChangeFrequency gets it fresh
            freqMenu.Add(fl, ((fr, *) => ChangeFrequency(fr)).Bind(fc))
        }
        tray.Add("Refresh Rate  [" curMode.freq "Hz]", freqMenu)

        ; --- HDR ---
        activeMon := ""
        for mon in monitors {
            if mon.isActive {
                activeMon := mon
                break
            }
        }
        if activeMon != "" {
            if !IsHdrSupported(activeMon.luidLow, activeMon.luidHigh, activeMon.targetId) {
                tray.Add("HDR  [not supported]", (*) => "")
                tray.Disable("HDR  [not supported]")
            } else {
                hdrState := ReadHdrFromRegistry(activeMon.devPath)
                if !hdrState.found {
                    tray.Add("HDR  [unknown]", (*) => "")
                    tray.Disable("HDR  [unknown]")
                } else {
                    hdrLabel := hdrState.enabled ? "HDR  [ON]" : "HDR  [OFF]"
                    tray.Add(hdrLabel, (*) => ToggleHdr())
                }
            }
        }
    }

    ; --- Actions ---
    tray.Add()
    if IsExclusive
        tray.Add("Restore original config", (*) => RestoreOriginal())
    else {
        tray.Add("Restore original config", (*) => "")
        tray.Disable("Restore original config")
    }
    tray.Add()
    tray.Add("Exit", ExitHandler)

    A_IconTip := IsExclusive
        ? "MonitorSwitcher [exclusive]"
        : "MonitorSwitcher"
}

/** Asks for confirmation before switching to exclusive mode on a monitor. */
ConfirmSwitch(targetId) {
    ; Skip if this monitor is already the exclusive target
    if (IsExclusive && targetId = ActiveTargetId)
        return

    ; Find monitor name and check if already active
    monitors := GetAllMonitors()
    name := "target " targetId
    alreadyActive := false
    activeCount := 0
    for mon in monitors {
        if mon.isActive
            activeCount++
        if mon.targetId = targetId {
            name := mon.name
            if mon.isActive
                alreadyActive := true
        }
    }

    ; Skip if this is the only active monitor already (nothing to change)
    if (alreadyActive && activeCount = 1)
        return

    if "Yes" = MsgBox(
        "Activate ONLY " name " and turn off all others?`n`n"
        "Restore anytime with Ctrl+Win+R or the tray menu (Ctrl+Win+M).",
        "MonitorSwitcher", "YesNo Icon!")
        SetExclusiveMonitor(targetId)
}

; ==================================================================
;  EXIT HANDLER
; ==================================================================

ExitHandler(*) {
    if IsExclusive {
        result := MsgBox("Restore display configuration before exiting?",
                         "MonitorSwitcher", "YesNoCancel Icon?")
        if result = "Cancel"
            return
        if result = "Yes" {
            RestoreOriginal()
            Sleep(2000)
        }
    }
    ExitApp()
}

; ==================================================================
;  GLOBAL HOTKEYS
; ==================================================================

#^r:: {
    if IsExclusive {
        RestoreOriginal()
        ToolTip("Configuration restored")
        SetTimer(() => ToolTip(), -3000)
    } else {
        ToolTip("Nothing to restore")
        SetTimer(() => ToolTip(), -2000)
    }
}

#^m:: {
    BuildMenu()
    A_TrayMenu.Show()
}

#^h:: ToggleHdr()

; ==================================================================
;  STARTUP
; ==================================================================

; Rebuild menu before every tray open (right-click on icon)
A_TrayMenu.ClickCount := 1  ; single left-click also shows menu
OnMessage(0x404, TrayClick)  ; AHK_NOTIFYICON
TrayClick(wParam, lParam, msg, hwnd) {
    ; lParam low word: mouse message
    mouseMsg := lParam & 0xFFFF
    if (mouseMsg = 0x0205     ; WM_RBUTTONUP
        || mouseMsg = 0x0202) ; WM_LBUTTONUP
    {
        BuildMenu()
    }
}

TraySetIcon("shell32.dll", 16)
BuildMenu()

OnDisplayChange(*) {
    global SelfChanging
    if !SelfChanging
        SetTimer(BuildMenu, -1000)
}
OnMessage(0x007E, OnDisplayChange)

TrayTip("MonitorSwitcher v1.0",
        "Ctrl+Win+M = menu  |  Ctrl+Win+R = restore  |  Ctrl+Win+H = HDR", 1)

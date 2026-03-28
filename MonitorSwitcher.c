/*
 * MonitorSwitcher.c
 *
 * System tray utility to switch between monitors exclusively,
 * change resolution/refresh rate, and toggle HDR on Windows 11.
 *
 * License: GPLv3+
 */

#define WINVER        0x0601
#define _WIN32_WINNT  0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>

/* ─── Constants ─────────────────────────────────────────────────────── */

/* Array size limits for stack/static buffers */
#define MAX_PATHS         512
#define MAX_MODES         512
#define MAX_MONITORS      16
#define MAX_TOPOLOGY      16
#define MAX_RESOLUTIONS   128
#define MAX_FREQUENCIES   32

/* Timer IDs */
#define TIMER_REBUILD      1    /* One-shot 2000ms debounce for WM_DISPLAYCHANGE */

/* Custom window message for system tray icon events */
#define WM_TRAYICON        (WM_APP + 1)

/* Menu item ID ranges — spaced apart for safe indexing */
#define IDM_MONITOR_BASE   1000   /* 1000 .. 1000+MAX_MONITORS-1  */
#define IDM_RES_BASE       2000   /* 2000 .. 2000+MAX_RESOLUTIONS-1 */
#define IDM_FREQ_BASE      3000   /* 3000 .. 3000+MAX_FREQUENCIES-1 */
#define IDM_HDR_BASE       4000   /* 4000 .. 4000+MAX_MONITORS-1  */
#define IDM_RESTORE        5000
#define IDM_AUTOSTART      5001
#define IDM_EXIT           5002

/* Registry key and value for auto-start with Windows */
#define AUTOSTART_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALUE    L"MonitorSwitcher"

/* Hotkey IDs for RegisterHotKey / WM_HOTKEY */
#define HOTKEY_RESTORE     1     /* Ctrl+Alt+R */
#define HOTKEY_MENU        2     /* Ctrl+Alt+M */
#define HOTKEY_HDR         3     /* Ctrl+Alt+H */
#define HOTKEY_MONITOR_BASE 100  /* 100 .. 108 = Ctrl+Alt+1..9 */

/* Single-instance mutex name */
#define MUTEX_NAME         L"MonitorSwitcher_SingleInstance"

/* Window class name for the hidden message window */
#define CLASS_MAIN         L"MonitorSwitcherMain"

/* Path flags — may be missing from some MinGW-w64 versions */
#ifndef DISPLAYCONFIG_PATH_ACTIVE
#define DISPLAYCONFIG_PATH_ACTIVE          0x00000001
#endif
#ifndef DISPLAYCONFIG_PATH_MODE_IDX_INVALID
#define DISPLAYCONFIG_PATH_MODE_IDX_INVALID 0xFFFFFFFF
#endif

/* ─── Types ─────────────────────────────────────────────────────────── */

typedef struct {
    UINT32 targetId;
    WCHAR  name[64];
    WCHAR  devPath[128];
    UINT32 luidLow;
    UINT32 luidHigh;
    BOOL   isActive;
    WCHAR  gdiName[CCHDEVICENAME];
    UINT32 w;
    UINT32 h;
    UINT32 freq;
} MonitorInfo;

typedef struct {
    UINT32 targetId;
    UINT32 sourceId;
} TopologyEntry;

typedef struct {
    UINT32 w;
    UINT32 h;
} ResolutionEntry;

/* ─── Global State ──────────────────────────────────────────────────── */

static HWND             g_hwndMain             = NULL;
static HICON            g_hIcon                = NULL;
static NOTIFYICONDATAW  g_nid;

/* Saved topology for restore (primary monitor first) */
static TopologyEntry    g_originalTopology[MAX_TOPOLOGY];
static int              g_originalTopologyCount = 0;

/* Reentrancy guard — suppresses WM_DISPLAYCHANGE during our own changes */
static BOOL             g_selfChanging         = FALSE;

/* Dynamic hotkeys */
static int              g_hotkeyMonitorCount   = 0;

/* Menu callback lookup tables — filled by ShowContextMenu, read by WM_COMMAND */
static UINT32           g_menuMonitorIds[MAX_MONITORS];
static int              g_menuMonitorCount     = 0;
static ResolutionEntry  g_menuResolutions[MAX_RESOLUTIONS];
static int              g_menuResCount         = 0;
static UINT32           g_menuFreqs[MAX_FREQUENCIES];
static int              g_menuFreqCount        = 0;
static UINT32           g_menuHdrIds[MAX_MONITORS];
static int              g_menuHdrCount         = 0;

/* Shared buffers for QueryDisplayConfig — never used recursively */
static DISPLAYCONFIG_PATH_INFO g_pathBuf[MAX_PATHS];
static DISPLAYCONFIG_MODE_INFO g_modeBuf[MAX_MODES];

/* ─── Forward Declarations ──────────────────────────────────────────── */

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

/* DisplayConfig helpers */
static int  GetAllMonitors(MonitorInfo *monitors, int maxMonitors);
static BOOL GetActiveGdiName(WCHAR *gdiName, int maxLen);
static BOOL GetCurrentMode(const WCHAR *gdiName,
                           UINT32 *w, UINT32 *h, UINT32 *freq);

/* Exclusive mode */
static void SaveConfig(void);
static BOOL IsTopologyChanged(void);
static void SetExclusiveMonitor(UINT32 targetId);
static void SaveActiveConfigToDatabase(void);
static void RestoreOriginal(void);

/* Resolution / refresh rate */
static int  GetAvailableResolutions(const WCHAR *gdiName,
                                    ResolutionEntry *res, int maxRes);
static int  GetAvailableFreqs(const WCHAR *gdiName,
                              UINT32 targetW, UINT32 targetH,
                              UINT32 *freqs, int maxFreqs);
static void ChangeResolution(UINT32 newW, UINT32 newH);
static void ChangeFrequency(UINT32 newFreq);
static void ApplyMode(const WCHAR *gdiName,
                      UINT32 w, UINT32 h, UINT32 freq);

/* HDR */
static BOOL ReadHdrFromRegistry(const WCHAR *devPath, BOOL *enabled);
static BOOL IsHdrSupported(UINT32 luidLow, UINT32 luidHigh, UINT32 targetId);
static void ToggleHdr(UINT32 targetId);
static void ToggleHdrPrimary(void);

/* Tray menu */
static void ShowContextMenu(void);
static void ConfirmSwitch(UINT32 targetId);

/* Exit */
static void ExitHandler(void);

/* System tray */
static void SetupTrayIcon(void);
static void RemoveTrayIcon(void);
static void UpdateTooltip(void);
static void ShowBalloon(const WCHAR *title, const WCHAR *text);

/* Hotkeys */
static void UpdateMonitorHotkeys(void);

/* Auto-start */
static BOOL IsAutoStartEnabled(void);
static void SetAutoStart(BOOL enable);

/* Cleanup */
static void CleanExit(void);

/* ─── Entry Point ───────────────────────────────────────────────────── */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /*
     * Enforce single instance via a named mutex.
     * If another instance already holds the mutex, exit silently.
     */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    /*
     * Register a window class for the hidden message window.
     * This window never becomes visible — it exists solely to receive
     * WM_TIMER, WM_TRAYICON, WM_HOTKEY, WM_COMMAND, and WM_DISPLAYCHANGE.
     */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = CLASS_MAIN;
    RegisterClassExW(&wc);

    g_hwndMain = CreateWindowExW(
        0, CLASS_MAIN, L"MonitorSwitcher",
        WS_OVERLAPPEDWINDOW,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwndMain) {
        CloseHandle(hMutex);
        return 1;
    }

    SetupTrayIcon();

    /* Snapshot the current topology so we can restore it later */
    SaveConfig();

    /* Register global hotkeys: Ctrl+Alt+R, Ctrl+Alt+M, Ctrl+Alt+H */
    RegisterHotKey(g_hwndMain, HOTKEY_RESTORE, MOD_CONTROL | MOD_ALT, 'R');
    RegisterHotKey(g_hwndMain, HOTKEY_MENU,    MOD_CONTROL | MOD_ALT, 'M');
    RegisterHotKey(g_hwndMain, HOTKEY_HDR,     MOD_CONTROL | MOD_ALT, 'H');

    /* Register dynamic monitor hotkeys (Ctrl+Alt+1..9) */
    UpdateMonitorHotkeys();

    ShowBalloon(L"MonitorSwitcher",
                L"Ctrl+Alt+M = menu  |  Ctrl+Alt+R = restore  |  Ctrl+Alt+H = HDR\n"
                L"Ctrl+Alt+1..9 = switch directly to monitor");

    /* Standard Win32 message loop — runs until PostQuitMessage(0) */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(hMutex);
    return (int)msg.wParam;
}

/* ─── Main Window Procedure ─────────────────────────────────────────── */

/*
 * Handles all messages for the hidden main window:
 *   WM_TIMER:          Debounced display-change rebuild
 *   WM_TRAYICON:       Left/right click on system tray icon
 *   WM_HOTKEY:         Ctrl+Alt+R/M/H + Ctrl+Alt+1..9 global hotkeys
 *   WM_COMMAND:        Context menu item selections
 *   WM_DISPLAYCHANGE:  External display topology changes
 *   WM_DESTROY:        Final cleanup
 */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam)
{
    switch (msg) {

    case WM_TIMER:
        switch (wParam) {
        case TIMER_REBUILD:
            /*
             * One-shot timer: fired 2000ms after WM_DISPLAYCHANGE.
             * Gives Windows time to settle after topology changes.
             */
            KillTimer(hwnd, TIMER_REBUILD);
            UpdateTooltip();
            UpdateMonitorHotkeys();
            break;
        }
        return 0;

    case WM_TRAYICON:
        /* Show context menu on left-click or right-click */
        if (LOWORD(lParam) == WM_LBUTTONUP
            || LOWORD(lParam) == WM_RBUTTONUP) {
            ShowContextMenu();
        }
        return 0;

    case WM_HOTKEY:
        if (wParam >= HOTKEY_MONITOR_BASE
            && wParam < HOTKEY_MONITOR_BASE + (WPARAM)g_hotkeyMonitorCount) {
            int index = (int)(wParam - HOTKEY_MONITOR_BASE);
            MonitorInfo monitors[MAX_MONITORS];
            int monCount = GetAllMonitors(monitors, MAX_MONITORS);
            if (index < monCount) {
                SetExclusiveMonitor(monitors[index].targetId);
            }
            return 0;
        }

        switch (wParam) {
        case HOTKEY_RESTORE:
            if (IsTopologyChanged()) {
                RestoreOriginal();
                ShowBalloon(L"MonitorSwitcher", L"Configuration restored");
            } else {
                ShowBalloon(L"MonitorSwitcher", L"Nothing to restore");
            }
            break;
        case HOTKEY_MENU:
            ShowContextMenu();
            break;
        case HOTKEY_HDR:
            ToggleHdrPrimary();
            break;
        }
        return 0;

    case WM_COMMAND: {
        int id = (int)LOWORD(wParam);
        if (id >= IDM_MONITOR_BASE
            && id < IDM_MONITOR_BASE + g_menuMonitorCount) {
            ConfirmSwitch(g_menuMonitorIds[id - IDM_MONITOR_BASE]);
        } else if (id >= IDM_RES_BASE
                   && id < IDM_RES_BASE + g_menuResCount) {
            int idx = id - IDM_RES_BASE;
            ChangeResolution(g_menuResolutions[idx].w,
                             g_menuResolutions[idx].h);
        } else if (id >= IDM_FREQ_BASE
                   && id < IDM_FREQ_BASE + g_menuFreqCount) {
            ChangeFrequency(g_menuFreqs[id - IDM_FREQ_BASE]);
        } else if (id >= IDM_HDR_BASE
                   && id < IDM_HDR_BASE + g_menuHdrCount) {
            ToggleHdr(g_menuHdrIds[id - IDM_HDR_BASE]);
        } else if (id == IDM_RESTORE) {
            RestoreOriginal();
        } else if (id == IDM_AUTOSTART) {
            SetAutoStart(!IsAutoStartEnabled());
        } else if (id == IDM_EXIT) {
            ExitHandler();
        }
        return 0;
    }

    case WM_DISPLAYCHANGE:
        /*
         * When the display topology changes externally (user plugs in
         * a monitor, changes settings via Windows), schedule a debounced
         * tooltip update.  Menu is always built fresh on click, so no
         * need to rebuild it here.
         */
        if (!g_selfChanging) {
            SetTimer(hwnd, TIMER_REBUILD, 2000, NULL);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ─── DisplayConfig Helpers ─────────────────────────────────────────── */

/*
 * Enumerates all physically connected monitors in a port-centric stable order.
 * Three-phase approach:
 *   Phase 1: QDC_ONLY_ACTIVE_PATHS — get current GDI name, resolution,
 *            and refresh rate for each active target.
 *   Phase 2: QDC_ALL_PATHS — discover all hardware targets, filter ghosts.
 *   Phase 3: Sort by Adapter LUID and Target ID to perfectly mirror
 *            the port-centric numbering of Windows Settings.
 *
 * Returns the number of monitors written to the output array.
 */
static int CompareMonitors(const void *a, const void *b)
{
    const MonitorInfo *m1 = (const MonitorInfo *)a;
    const MonitorInfo *m2 = (const MonitorInfo *)b;

    if (m1->luidHigh != m2->luidHigh) {
        return (m1->luidHigh < m2->luidHigh) ? -1 : 1;
    }
    if (m1->luidLow != m2->luidLow) {
        return (m1->luidLow < m2->luidLow) ? -1 : 1;
    }
    if (m1->targetId != m2->targetId) {
        return (m1->targetId < m2->targetId) ? -1 : 1;
    }
    return 0;
}

static int GetAllMonitors(MonitorInfo *monitors, int maxMonitors)
{
    int count = 0;

    /* Phase 1: Build a lookup table of active targets */
    UINT32 activeTgtIds[MAX_MONITORS];
    WCHAR  activeGdi[MAX_MONITORS][CCHDEVICENAME];
    UINT32 activeW[MAX_MONITORS];
    UINT32 activeH[MAX_MONITORS];
    UINT32 activeFreq[MAX_MONITORS];
    int    activeCnt = 0;

    UINT32 apc = 0, amc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &apc, &amc)
            == ERROR_SUCCESS && apc > 0) {
        if (apc > MAX_PATHS) apc = MAX_PATHS;
        if (amc > MAX_MODES) amc = MAX_MODES;
        ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * apc);
        ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * amc);

        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                &apc, g_pathBuf, &amc, g_modeBuf, NULL) == ERROR_SUCCESS) {
            UINT32 i;
            for (i = 0; i < apc && activeCnt < MAX_MONITORS; i++) {
                UINT32 tgtId = g_pathBuf[i].targetInfo.id;

                /* Skip duplicates */
                BOOL seen = FALSE;
                int j;
                for (j = 0; j < activeCnt; j++) {
                    if (activeTgtIds[j] == tgtId) { seen = TRUE; break; }
                }
                if (seen) continue;

                /* Resolve GDI name via DisplayConfigGetDeviceInfo type 1 */
                DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName;
                ZeroMemory(&srcName, sizeof(srcName));
                srcName.header.type =
                    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                srcName.header.size = sizeof(srcName);
                srcName.header.adapterId = g_pathBuf[i].sourceInfo.adapterId;
                srcName.header.id = g_pathBuf[i].sourceInfo.id;

                activeGdi[activeCnt][0] = L'\0';
                if (DisplayConfigGetDeviceInfo(&srcName.header)
                        == ERROR_SUCCESS) {
                    lstrcpynW(activeGdi[activeCnt],
                              srcName.viewGdiDeviceName, CCHDEVICENAME);
                }

                /* Get current resolution and refresh rate */
                activeW[activeCnt] = 0;
                activeH[activeCnt] = 0;
                activeFreq[activeCnt] = 0;

                if (activeGdi[activeCnt][0] != L'\0') {
                    DEVMODEW dm;
                    ZeroMemory(&dm, sizeof(dm));
                    dm.dmSize = sizeof(dm);
                    if (EnumDisplaySettingsExW(activeGdi[activeCnt],
                            ENUM_CURRENT_SETTINGS, &dm, 0)) {
                        activeW[activeCnt]    = dm.dmPelsWidth;
                        activeH[activeCnt]    = dm.dmPelsHeight;
                        activeFreq[activeCnt] = dm.dmDisplayFrequency;
                    }
                }

                activeTgtIds[activeCnt] = tgtId;
                activeCnt++;
            }
        }
    }

    /* Phase 2: Enumerate all connected targets, filter ghosts */
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pc, &mc)
            == ERROR_SUCCESS && pc > 0) {
        if (pc > MAX_PATHS) pc = MAX_PATHS;
        if (mc > MAX_MODES) mc = MAX_MODES;
        ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * pc);
        ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * mc);

        if (QueryDisplayConfig(QDC_ALL_PATHS,
                &pc, g_pathBuf, &mc, g_modeBuf, NULL) == ERROR_SUCCESS) {
            UINT32 i;
            for (i = 0; i < pc && count < maxMonitors; i++) {
                UINT32 tgtId = g_pathBuf[i].targetInfo.id;

                /* Skip duplicates */
                BOOL seen = FALSE;
                int j;
                for (j = 0; j < count; j++) {
                    if (monitors[j].targetId == tgtId) {
                        seen = TRUE; break;
                    }
                }
                if (seen) continue;

                /* Resolve friendly name and device path via type 2 */
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgtName;
                ZeroMemory(&tgtName, sizeof(tgtName));
                tgtName.header.type =
                    DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgtName.header.size = sizeof(tgtName);
                tgtName.header.adapterId =
                    g_pathBuf[i].targetInfo.adapterId;
                tgtName.header.id = tgtId;

                if (DisplayConfigGetDeviceInfo(&tgtName.header)
                        != ERROR_SUCCESS)
                    continue;
                
                /* Filter ghost targets */
                if (tgtName.monitorFriendlyDeviceName[0] == L'\0')
                    continue;

                MonitorInfo *mon = &monitors[count];
                ZeroMemory(mon, sizeof(*mon));
                mon->targetId = tgtId;
                lstrcpynW(mon->name,
                          tgtName.monitorFriendlyDeviceName, 64);
                lstrcpynW(mon->devPath,
                          tgtName.monitorDevicePath, 128);
                mon->luidLow  =
                    g_pathBuf[i].targetInfo.adapterId.LowPart;
                mon->luidHigh =
                    (UINT32)g_pathBuf[i].targetInfo.adapterId.HighPart;

                /* Cross-reference with active info from Phase 1 */
                mon->isActive = FALSE;
                for (j = 0; j < activeCnt; j++) {
                    if (activeTgtIds[j] == tgtId) {
                        mon->isActive = TRUE;
                        lstrcpynW(mon->gdiName,
                                  activeGdi[j], CCHDEVICENAME);
                        mon->w    = activeW[j];
                        mon->h    = activeH[j];
                        mon->freq = activeFreq[j];
                        break;
                    }
                }

                count++;
            }
        }
    }

    /* Phase 3: Sort by LUID + Target ID to perfectly mirror port-centric order */
    if (count > 1) {
        qsort(monitors, count, sizeof(MonitorInfo), CompareMonitors);
    }

    return count;
}

/*
 * Returns the GDI device name of the current primary display
 * (e.g. "\\\\.\\DISPLAY1").  Always queries fresh data.
 */
static BOOL GetActiveGdiName(WCHAR *gdiName, int maxLen)
{
    DISPLAY_DEVICEW dd;
    DWORD idx = 0;

    while (1) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, idx, &dd, 0))
            break;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            lstrcpynW(gdiName, dd.DeviceName, maxLen);
            return TRUE;
        }
        idx++;
    }
    gdiName[0] = L'\0';
    return FALSE;
}

/*
 * Returns the current resolution and refresh rate for a GDI device.
 */
static BOOL GetCurrentMode(const WCHAR *gdiName,
                           UINT32 *w, UINT32 *h, UINT32 *freq)
{
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsExW(gdiName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        *w    = dm.dmPelsWidth;
        *h    = dm.dmPelsHeight;
        *freq = dm.dmDisplayFrequency;
        return TRUE;
    }
    return FALSE;
}

/* ─── Exclusive Mode ────────────────────────────────────────────────── */

/*
 * Compares the current active topology with the saved original topology.
 * Returns TRUE if they differ (meaning the user or another app changed
 * the display configuration), FALSE if they match.
 */
static BOOL IsTopologyChanged(void)
{
    if (g_originalTopologyCount == 0)
        return FALSE;

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc)
            != ERROR_SUCCESS || pc == 0)
        return FALSE;
    if (pc > MAX_PATHS) pc = MAX_PATHS;
    if (mc > MAX_MODES) mc = MAX_MODES;
    ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * pc);
    ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * mc);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
            &pc, g_pathBuf, &mc, g_modeBuf, NULL) != ERROR_SUCCESS)
        return FALSE;

    /* Different number of active paths = topology changed */
    if ((int)pc != g_originalTopologyCount)
        return TRUE;

    /* Check if all {targetId, sourceId} pairs match */
    UINT32 i;
    for (i = 0; i < pc; i++) {
        BOOL found = FALSE;
        int j;
        for (j = 0; j < g_originalTopologyCount; j++) {
            if (g_pathBuf[i].targetInfo.id
                    == g_originalTopology[j].targetId
                && g_pathBuf[i].sourceInfo.id
                    == g_originalTopology[j].sourceId) {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return TRUE;
    }

    return FALSE;
}

/*
 * Snapshots the current active display topology (primary monitor first).
 * Only stores {targetId, sourceId} pairs — not full paths or modes.
 * Paths and modes are queried fresh on restore; only topology is saved.
 */
static void SaveConfig(void)
{
    g_originalTopologyCount = 0;

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc)
            != ERROR_SUCCESS || pc == 0)
        return;
    if (pc > MAX_PATHS) pc = MAX_PATHS;
    if (mc > MAX_MODES) mc = MAX_MODES;
    ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * pc);
    ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * mc);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
            &pc, g_pathBuf, &mc, g_modeBuf, NULL) != ERROR_SUCCESS)
        return;

    UINT32 i;
    for (i = 0; i < pc && g_originalTopologyCount < MAX_TOPOLOGY; i++) {
        g_originalTopology[g_originalTopologyCount].targetId =
            g_pathBuf[i].targetInfo.id;
        g_originalTopology[g_originalTopologyCount].sourceId =
            g_pathBuf[i].sourceInfo.id;
        g_originalTopologyCount++;
    }
}

/*
 * Activates ONLY the specified monitor (by targetId), disabling all others.
 *
 * Three-attempt strategy (avoids SDC_ALLOW_CHANGES which causes 60Hz fallback):
 *   1) SDC_TOPOLOGY_SUPPLIED — reads modes from Windows' persistence database.
 *   2) SDC_USE_SUPPLIED_DISPLAY_CONFIG — strict, no changes allowed.
 *   3) SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES — last resort.
 */
static void SetExclusiveMonitor(UINT32 targetId)
{
    g_selfChanging = TRUE;

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pc, &mc)
            != ERROR_SUCCESS || pc == 0) {
        g_selfChanging = FALSE;
        return;
    }
    if (pc > MAX_PATHS) pc = MAX_PATHS;
    if (mc > MAX_MODES) mc = MAX_MODES;
    ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * pc);
    ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * mc);

    if (QueryDisplayConfig(QDC_ALL_PATHS,
            &pc, g_pathBuf, &mc, g_modeBuf, NULL) != ERROR_SUCCESS) {
        g_selfChanging = FALSE;
        return;
    }

    BOOL found = FALSE;
    UINT32 i;
    for (i = 0; i < pc; i++) {
        if (g_pathBuf[i].targetInfo.id != targetId)
            continue;

        /* Copy this single path and configure it as the only active one */
        DISPLAYCONFIG_PATH_INFO singlePath = g_pathBuf[i];
        singlePath.flags = DISPLAYCONFIG_PATH_ACTIVE;
        singlePath.sourceInfo.modeInfoIdx =
            DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        singlePath.targetInfo.modeInfoIdx =
            DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

        /* Three-attempt apply */
        LONG ret = SetDisplayConfig(1, &singlePath, 0, NULL,
            SDC_APPLY | SDC_TOPOLOGY_SUPPLIED);
        if (ret != ERROR_SUCCESS)
            ret = SetDisplayConfig(1, &singlePath, 0, NULL,
                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
        if (ret != ERROR_SUCCESS)
            ret = SetDisplayConfig(1, &singlePath, 0, NULL,
                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG
                | SDC_ALLOW_CHANGES);

        if (ret != ERROR_SUCCESS) {
            WCHAR errMsg[128];
            wsprintfW(errMsg,
                L"SetDisplayConfig failed (error %ld)", ret);
            MessageBoxW(NULL, errMsg, L"MonitorSwitcher",
                        MB_OK | MB_ICONWARNING);
        } else {
            SaveActiveConfigToDatabase();
        }

        found = TRUE;
        break;
    }

    g_selfChanging = FALSE;

    if (!found) {
        WCHAR errMsg[128];
        wsprintfW(errMsg, L"Target %u not found.", targetId);
        MessageBoxW(NULL, errMsg, L"MonitorSwitcher",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    UpdateMonitorHotkeys();
    UpdateTooltip();
}

/*
 * Persists the current active display config to Windows' display database.
 * Called after successful exclusive mode activation so the mode is remembered.
 */
static void SaveActiveConfigToDatabase(void)
{
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc)
            != ERROR_SUCCESS || pc == 0)
        return;
    if (pc > MAX_PATHS) pc = MAX_PATHS;
    if (mc > MAX_MODES) mc = MAX_MODES;
    ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * pc);
    ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * mc);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
            &pc, g_pathBuf, &mc, g_modeBuf, NULL) != ERROR_SUCCESS)
        return;

    SetDisplayConfig(pc, g_pathBuf, mc, g_modeBuf,
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
}

/*
 * Restores the display topology saved before entering exclusive mode.
 * Queries fresh paths from QDC_ALL_PATHS and activates those matching
 * the saved {targetId, sourceId} pairs — preserving extend vs duplicate
 * layout and primary monitor order.
 */
static void RestoreOriginal(void)
{
    if (g_originalTopologyCount == 0 || !IsTopologyChanged()) {
        MessageBoxW(NULL, L"Nothing to restore.",
                    L"MonitorSwitcher", MB_OK | MB_ICONWARNING);
        return;
    }

    g_selfChanging = TRUE;

    /* Query fresh paths from the system */
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pc, &mc)
            != ERROR_SUCCESS || pc == 0) {
        g_selfChanging = FALSE;
        MessageBoxW(NULL,
            L"Restore failed: no display paths available.",
            L"MonitorSwitcher", MB_OK | MB_ICONWARNING);
        return;
    }
    if (pc > MAX_PATHS) pc = MAX_PATHS;
    if (mc > MAX_MODES) mc = MAX_MODES;
    ZeroMemory(g_pathBuf, sizeof(g_pathBuf[0]) * pc);
    ZeroMemory(g_modeBuf, sizeof(g_modeBuf[0]) * mc);

    if (QueryDisplayConfig(QDC_ALL_PATHS,
            &pc, g_pathBuf, &mc, g_modeBuf, NULL) != ERROR_SUCCESS) {
        g_selfChanging = FALSE;
        MessageBoxW(NULL,
            L"Restore failed: could not query display config.",
            L"MonitorSwitcher", MB_OK | MB_ICONWARNING);
        return;
    }

    /*
     * For each queried path, check if its {targetId, sourceId} matches
     * a saved topology entry.  Store the matching path buffer index.
     */
    UINT32 foundTgtIds[MAX_TOPOLOGY];
    UINT32 foundSrcIds[MAX_TOPOLOGY];
    UINT32 foundIdx[MAX_TOPOLOGY];       /* index into g_pathBuf */
    int    foundCount = 0;

    UINT32 i;
    for (i = 0; i < pc; i++) {
        UINT32 tgtId = g_pathBuf[i].targetInfo.id;
        UINT32 srcId = g_pathBuf[i].sourceInfo.id;

        /* Skip if this pair was already found */
        BOOL already = FALSE;
        int j;
        for (j = 0; j < foundCount; j++) {
            if (foundTgtIds[j] == tgtId && foundSrcIds[j] == srcId) {
                already = TRUE;
                break;
            }
        }
        if (already) continue;

        /* Check against saved topology */
        for (j = 0; j < g_originalTopologyCount; j++) {
            if (tgtId == g_originalTopology[j].targetId
                && srcId == g_originalTopology[j].sourceId) {
                if (foundCount < MAX_TOPOLOGY) {
                    foundTgtIds[foundCount] = tgtId;
                    foundSrcIds[foundCount] = srcId;
                    foundIdx[foundCount]    = i;
                    foundCount++;
                }
                break;
            }
        }
    }

    if (foundCount == 0) {
        g_selfChanging = FALSE;
        MessageBoxW(NULL,
            L"Restore failed: original monitors not found.",
            L"MonitorSwitcher", MB_OK | MB_ICONWARNING);
        return;
    }

    /*
     * Build restore path array in the original saved order.
     * The first entry is the primary monitor — Windows treats
     * the first path in the array as primary.
     */
    DISPLAYCONFIG_PATH_INFO restorePaths[MAX_TOPOLOGY];
    int restoreCount = 0;

    int s;
    for (s = 0; s < g_originalTopologyCount; s++) {
        int k;
        for (k = 0; k < foundCount; k++) {
            if (foundTgtIds[k] == g_originalTopology[s].targetId
                && foundSrcIds[k] == g_originalTopology[s].sourceId) {
                restorePaths[restoreCount] = g_pathBuf[foundIdx[k]];
                restorePaths[restoreCount].flags =
                    DISPLAYCONFIG_PATH_ACTIVE;
                restorePaths[restoreCount].sourceInfo.modeInfoIdx =
                    DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                restorePaths[restoreCount].targetInfo.modeInfoIdx =
                    DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                restoreCount++;
                break;
            }
        }
    }

    /* Three-attempt apply (same strategy as SetExclusiveMonitor) */
    LONG ret = SetDisplayConfig(
        (UINT32)restoreCount, restorePaths, 0, NULL,
        SDC_APPLY | SDC_TOPOLOGY_SUPPLIED);
    if (ret != ERROR_SUCCESS)
        ret = SetDisplayConfig(
            (UINT32)restoreCount, restorePaths, 0, NULL,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    if (ret != ERROR_SUCCESS)
        ret = SetDisplayConfig(
            (UINT32)restoreCount, restorePaths, 0, NULL,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG
            | SDC_ALLOW_CHANGES);

    g_selfChanging = FALSE;

    if (ret != ERROR_SUCCESS) {
        WCHAR errMsg[128];
        wsprintfW(errMsg, L"Restore failed (error %ld)", ret);
        MessageBoxW(NULL, errMsg, L"MonitorSwitcher",
                    MB_OK | MB_ICONWARNING);
    }

    UpdateMonitorHotkeys();
    UpdateTooltip();
}

/* ─── Resolution and Refresh Rate ───────────────────────────────────── */

/*
 * Enumerates all unique resolutions for a GDI device.
 * Returns them sorted by pixel count descending (highest first).
 */
static int GetAvailableResolutions(const WCHAR *gdiName,
                                   ResolutionEntry *res, int maxRes)
{
    int count = 0;
    DEVMODEW dm;
    DWORD modeIdx = 0;

    while (count < maxRes) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(gdiName, modeIdx, &dm, 0))
            break;

        UINT32 w = dm.dmPelsWidth;
        UINT32 h = dm.dmPelsHeight;

        /* Deduplicate */
        BOOL dup = FALSE;
        int i;
        for (i = 0; i < count; i++) {
            if (res[i].w == w && res[i].h == h) { dup = TRUE; break; }
        }
        if (!dup) {
            res[count].w = w;
            res[count].h = h;
            count++;
        }
        modeIdx++;
    }

    /* Bubble sort by pixel count descending */
    int i, j;
    for (i = 0; i < count - 1; i++) {
        for (j = 0; j < count - 1 - i; j++) {
            UINT64 a = (UINT64)res[j].w * res[j].h;
            UINT64 b = (UINT64)res[j + 1].w * res[j + 1].h;
            if (a < b) {
                ResolutionEntry tmp = res[j];
                res[j]     = res[j + 1];
                res[j + 1] = tmp;
            }
        }
    }

    return count;
}

/*
 * Enumerates all unique refresh rates available at a specific resolution.
 * Returns them sorted descending (highest first).
 */
static int GetAvailableFreqs(const WCHAR *gdiName,
                             UINT32 targetW, UINT32 targetH,
                             UINT32 *freqs, int maxFreqs)
{
    int count = 0;
    DEVMODEW dm;
    DWORD modeIdx = 0;

    while (count < maxFreqs) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(gdiName, modeIdx, &dm, 0))
            break;

        if (dm.dmPelsWidth == targetW && dm.dmPelsHeight == targetH) {
            UINT32 f = dm.dmDisplayFrequency;

            BOOL dup = FALSE;
            int i;
            for (i = 0; i < count; i++) {
                if (freqs[i] == f) { dup = TRUE; break; }
            }
            if (!dup) {
                freqs[count] = f;
                count++;
            }
        }
        modeIdx++;
    }

    /* Bubble sort descending */
    int i, j;
    for (i = 0; i < count - 1; i++) {
        for (j = 0; j < count - 1 - i; j++) {
            if (freqs[j] < freqs[j + 1]) {
                UINT32 tmp = freqs[j];
                freqs[j]     = freqs[j + 1];
                freqs[j + 1] = tmp;
            }
        }
    }

    return count;
}

/*
 * Changes the primary monitor's resolution.
 * Gets the GDI name fresh (not from a closure or cache).
 * Tries to preserve the current refresh rate; falls back to highest available.
 */
static void ChangeResolution(UINT32 newW, UINT32 newH)
{
    WCHAR gdiName[CCHDEVICENAME];
    if (!GetActiveGdiName(gdiName, CCHDEVICENAME))
        return;

    UINT32 curW, curH, curFreq;
    UINT32 targetFreq = 60;
    if (GetCurrentMode(gdiName, &curW, &curH, &curFreq))
        targetFreq = curFreq;

    UINT32 freqs[MAX_FREQUENCIES];
    int freqCount = GetAvailableFreqs(gdiName, newW, newH,
                                      freqs, MAX_FREQUENCIES);

    /* Default to highest available frequency */
    UINT32 bestFreq = (freqCount > 0) ? freqs[0] : 60;

    /* If the current frequency is available at the new resolution, keep it */
    int i;
    for (i = 0; i < freqCount; i++) {
        if (freqs[i] == targetFreq) {
            bestFreq = targetFreq;
            break;
        }
    }

    ApplyMode(gdiName, newW, newH, bestFreq);
}

/*
 * Changes the primary monitor's refresh rate.
 * Gets the GDI name and current resolution fresh.
 */
static void ChangeFrequency(UINT32 newFreq)
{
    WCHAR gdiName[CCHDEVICENAME];
    if (!GetActiveGdiName(gdiName, CCHDEVICENAME))
        return;

    UINT32 curW, curH, curFreq;
    if (!GetCurrentMode(gdiName, &curW, &curH, &curFreq))
        return;

    ApplyMode(gdiName, curW, curH, newFreq);
}

/*
 * Applies a display mode via ChangeDisplaySettingsExW.
 *
 * Enumerates all modes to find the exact DEVMODE matching (w, h, freq).
 * Uses that exact driver-enumerated DEVMODE buffer — never constructs one
 * from scratch — to preserve all driver-specific signal parameters
 * (color depth, timing, etc.).
 */
static void ApplyMode(const WCHAR *gdiName,
                      UINT32 w, UINT32 h, UINT32 freq)
{
    DEVMODEW dm;
    DWORD modeIdx = 0;

    while (1) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(gdiName, modeIdx, &dm, 0))
            break;

        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h
            && dm.dmDisplayFrequency == freq) {
            g_selfChanging = TRUE;
            LONG ret = ChangeDisplaySettingsExW(
                gdiName, &dm, NULL, CDS_UPDATEREGISTRY, NULL);
            g_selfChanging = FALSE;

            if (ret != DISP_CHANGE_SUCCESSFUL) {
                WCHAR errMsg[128];
                wsprintfW(errMsg,
                    L"Error applying %ux%u @ %uHz (code %ld)",
                    w, h, freq, ret);
                MessageBoxW(NULL, errMsg, L"MonitorSwitcher",
                            MB_OK | MB_ICONWARNING);
            }
            return;
        }
        modeIdx++;
    }

    WCHAR errMsg[128];
    wsprintfW(errMsg, L"Mode %ux%u @ %uHz not available.", w, h, freq);
    MessageBoxW(NULL, errMsg, L"MonitorSwitcher", MB_OK | MB_ICONWARNING);
}

/* ─── HDR ───────────────────────────────────────────────────────────── */

/*
 * Reads the real HDR state from the Windows registry (MonitorDataStore).
 *
 * The CCD API (type 9) can report incorrect values on some devices
 * (e.g. dummy HDMI plugs), so the registry is the authoritative source
 * for reading the current HDR on/off state.
 *
 * Algorithm:
 *   1. Extract the device prefix from the devPath (second '#'-delimited segment)
 *   2. Enumerate subkeys under MonitorDataStore
 *   3. Find the subkey whose name starts with the prefix
 *   4. Read the HDREnabled DWORD value (1 = on, 0 = off)
 *
 * Returns TRUE if the value was found; *enabled is set accordingly.
 */
static BOOL ReadHdrFromRegistry(const WCHAR *devPath, BOOL *enabled)
{
    /* Extract second segment: "\\?\DISPLAY#PREFIX#..." → "PREFIX" */
    const WCHAR *p = devPath;
    const WCHAR *segStart = NULL;
    const WCHAR *segEnd   = NULL;
    int hashCount = 0;

    while (*p) {
        if (*p == L'#') {
            hashCount++;
            if (hashCount == 1)
                segStart = p + 1;
            else if (hashCount == 2) {
                segEnd = p;
                break;
            }
        }
        p++;
    }

    if (!segStart || !segEnd || segEnd <= segStart)
        return FALSE;

    int prefixLen = (int)(segEnd - segStart);
    if (prefixLen >= 128) prefixLen = 127;
    WCHAR prefix[128];
    int k;
    for (k = 0; k < prefixLen; k++)
        prefix[k] = segStart[k];
    prefix[prefixLen] = L'\0';

    /* Open the MonitorDataStore registry key */
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\"
            L"GraphicsDrivers\\MonitorDataStore",
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    BOOL found = FALSE;
    WCHAR subKeyName[256];
    DWORD subKeyNameLen;

    for (DWORD i = 0; !found; i++) {
        subKeyNameLen = 256;
        if (RegEnumKeyExW(hKey, i, subKeyName, &subKeyNameLen,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        /* Case-sensitive prefix match */
        BOOL match = TRUE;
        for (k = 0; k < prefixLen; k++) {
            if (k >= (int)subKeyNameLen || subKeyName[k] != prefix[k]) {
                match = FALSE;
                break;
            }
        }
        if (!match) continue;

        HKEY hSubKey;
        if (RegOpenKeyExW(hKey, subKeyName, 0, KEY_READ, &hSubKey)
                == ERROR_SUCCESS) {
            DWORD value = 0;
            DWORD valueSize = sizeof(value);
            DWORD valueType = 0;
            if (RegQueryValueExW(hSubKey, L"HDREnabled", NULL,
                    &valueType, (LPBYTE)&value, &valueSize)
                        == ERROR_SUCCESS
                && valueType == REG_DWORD) {
                *enabled = (value == 1);
                found = TRUE;
            }
            RegCloseKey(hSubKey);
        }
    }

    RegCloseKey(hKey);
    return found;
}

/*
 * Checks if a monitor supports HDR via DisplayConfigGetDeviceInfo type 9
 * (DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO).
 */
static BOOL IsHdrSupported(UINT32 luidLow, UINT32 luidHigh,
                           UINT32 targetId)
{
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci;
    ZeroMemory(&ci, sizeof(ci));
    ci.header.type =
        DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    ci.header.size = sizeof(ci);
    ci.header.adapterId.LowPart  = luidLow;
    ci.header.adapterId.HighPart = (LONG)luidHigh;
    ci.header.id = targetId;

    if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS)
        return ci.advancedColorSupported;
    return FALSE;
}

/*
 * Toggles HDR on a specific monitor identified by targetId.
 *
 * Flow:
 *   1. GetAllMonitors() to find the monitor by targetId
 *   2. IsHdrSupported() check — bail if not
 *   3. ReadHdrFromRegistry() — bail if not found
 *   4. Invert the state
 *   5. DisplayConfigSetDeviceInfo (type 10) to write new state
 *   6. Show balloon notification with result
 */
static void ToggleHdr(UINT32 targetId)
{
    MonitorInfo monitors[MAX_MONITORS];
    int monCount = GetAllMonitors(monitors, MAX_MONITORS);

    MonitorInfo *mon = NULL;
    int i;
    for (i = 0; i < monCount; i++) {
        if (monitors[i].targetId == targetId) {
            mon = &monitors[i];
            break;
        }
    }

    if (!mon) {
        ShowBalloon(L"MonitorSwitcher", L"Monitor not found");
        return;
    }

    if (!IsHdrSupported(mon->luidLow, mon->luidHigh, mon->targetId)) {
        WCHAR msg[128];
        wsprintfW(msg, L"%s: HDR not supported", mon->name);
        ShowBalloon(L"MonitorSwitcher", msg);
        return;
    }

    BOOL currentEnabled;
    if (!ReadHdrFromRegistry(mon->devPath, &currentEnabled)) {
        WCHAR msg[128];
        wsprintfW(msg, L"%s: HDR state not found in registry",
                  mon->name);
        ShowBalloon(L"MonitorSwitcher", msg);
        return;
    }

    BOOL newState = !currentEnabled;

    /* Write new HDR state via DisplayConfigSetDeviceInfo type 10 */
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE si;
    ZeroMemory(&si, sizeof(si));
    si.header.type =
        DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    si.header.size = sizeof(si);
    si.header.adapterId.LowPart  = mon->luidLow;
    si.header.adapterId.HighPart = (LONG)mon->luidHigh;
    si.header.id = mon->targetId;
    si.enableAdvancedColor = newState ? 1 : 0;

    LONG ret = DisplayConfigSetDeviceInfo(&si.header);
    if (ret != ERROR_SUCCESS) {
        WCHAR msg[128];
        wsprintfW(msg, L"HDR toggle failed (error %ld)", ret);
        ShowBalloon(L"MonitorSwitcher", msg);
        return;
    }

    WCHAR msg[128];
    wsprintfW(msg, L"%s: HDR %s", mon->name,
              newState ? L"ON" : L"OFF");
    ShowBalloon(L"MonitorSwitcher", msg);
}

/*
 * Toggles HDR on the primary monitor.
 * Identifies the primary by matching the GDI name from GetActiveGdiName().
 * Used by the Ctrl+Alt+H hotkey.
 */
static void ToggleHdrPrimary(void)
{
    WCHAR gdiName[CCHDEVICENAME];
    if (!GetActiveGdiName(gdiName, CCHDEVICENAME)) {
        ShowBalloon(L"MonitorSwitcher",
                    L"No primary monitor detected");
        return;
    }

    MonitorInfo monitors[MAX_MONITORS];
    int monCount = GetAllMonitors(monitors, MAX_MONITORS);

    int i;
    for (i = 0; i < monCount; i++) {
        if (monitors[i].isActive
            && lstrcmpW(monitors[i].gdiName, gdiName) == 0) {
            ToggleHdr(monitors[i].targetId);
            return;
        }
    }

    ShowBalloon(L"MonitorSwitcher", L"Primary monitor not found");
}

/* ─── Tray Menu ─────────────────────────────────────────────────────── */

/*
 * Builds and shows the tray context menu from scratch with fresh data.
 *
 * Menu layout (top to bottom):
 *   "MonitorSwitcher"               (disabled title)
 *   ────────────────
 *   " *  MonitorName  |  WxH @ FHz"  (active, multiple)
 *   ">>  MonitorName  |  WxH @ FHz"  (only active monitor)
 *   "     MonitorName  (off)"         (inactive)
 *   ────────────────
 *   "Resolution  [WxH]"         ►   submenu
 *   "Refresh Rate  [FHz]"       ►   submenu
 *   "HDR: MonitorName  [ON/OFF]"    per active HDR monitor
 *   ────────────────
 *   "Restore original config"       (enabled when topology changed)
 *   ────────────────
 *   "Start with Windows"            (checked/unchecked toggle)
 *   "Exit"
 *
 * Menu item selections arrive as WM_COMMAND messages.  Lookup tables
 * (g_menuMonitorIds, g_menuResolutions, etc.) map IDs to parameters.
 */
static void ShowContextMenu(void)
{
    UpdateMonitorHotkeys();
    UpdateTooltip();

    MonitorInfo monitors[MAX_MONITORS];
    int monCount = GetAllMonitors(monitors, MAX_MONITORS);

    /* Count active monitors to determine marker style */
    int activeCount = 0;
    UINT32 singleActiveTargetId = 0;
    int i;
    for (i = 0; i < monCount; i++) {
        if (monitors[i].isActive) {
            activeCount++;
            singleActiveTargetId = monitors[i].targetId;
        }
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    /* Title label (disabled, not clickable) */
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"MonitorSwitcher");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* ── Monitor list ── */
    g_menuMonitorCount = 0;
    for (i = 0; i < monCount && g_menuMonitorCount < MAX_MONITORS; i++) {
        WCHAR label[256];

        if (monitors[i].isActive && monitors[i].w > 0) {
            const WCHAR *marker;
            if (activeCount == 1
                && monitors[i].targetId == singleActiveTargetId)
                marker = L">>";
            else
                marker = L" *";
            wsprintfW(label, L"%s  %s  |  %ux%u @ %uHz",
                      marker, monitors[i].name,
                      monitors[i].w, monitors[i].h, monitors[i].freq);
        } else if (!monitors[i].isActive) {
            wsprintfW(label, L"     %s  (off)", monitors[i].name);
        } else {
            const WCHAR *marker;
            if (activeCount == 1
                && monitors[i].targetId == singleActiveTargetId)
                marker = L">>";
            else
                marker = L" *";
            wsprintfW(label, L"%s  %s", marker, monitors[i].name);
        }

        UINT menuId = IDM_MONITOR_BASE + (UINT)g_menuMonitorCount;
        g_menuMonitorIds[g_menuMonitorCount] = monitors[i].targetId;
        g_menuMonitorCount++;

        AppendMenuW(hMenu, MF_STRING, menuId, label);
    }

    /* ── Resolution, Frequency, and HDR ── */
    WCHAR gdiName[CCHDEVICENAME];
    UINT32 curW = 0, curH = 0, curFreq = 0;
    BOOL hasCurMode = FALSE;

    if (GetActiveGdiName(gdiName, CCHDEVICENAME))
        hasCurMode = GetCurrentMode(gdiName, &curW, &curH, &curFreq);

    if (hasCurMode) {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

        /* Resolution submenu */
        HMENU hResMenu = CreatePopupMenu();
        ResolutionEntry resList[MAX_RESOLUTIONS];
        int resCount = GetAvailableResolutions(gdiName, resList,
                                               MAX_RESOLUTIONS);
        g_menuResCount = 0;

        for (i = 0; i < resCount && g_menuResCount < MAX_RESOLUTIONS;
             i++) {
            WCHAR label[64];
            if (resList[i].w == curW && resList[i].h == curH)
                wsprintfW(label, L">>  %ux%u",
                          resList[i].w, resList[i].h);
            else
                wsprintfW(label, L"%ux%u",
                          resList[i].w, resList[i].h);

            UINT menuId = IDM_RES_BASE + (UINT)g_menuResCount;
            g_menuResolutions[g_menuResCount] = resList[i];
            g_menuResCount++;

            AppendMenuW(hResMenu, MF_STRING, menuId, label);
        }

        WCHAR resLabel[64];
        wsprintfW(resLabel, L"Resolution  [%ux%u]", curW, curH);
        AppendMenuW(hMenu, MF_STRING | MF_POPUP,
                    (UINT_PTR)hResMenu, resLabel);

        /* Frequency submenu */
        HMENU hFreqMenu = CreatePopupMenu();
        UINT32 freqList[MAX_FREQUENCIES];
        int freqCount = GetAvailableFreqs(gdiName, curW, curH,
                                          freqList, MAX_FREQUENCIES);
        g_menuFreqCount = 0;

        for (i = 0; i < freqCount && g_menuFreqCount < MAX_FREQUENCIES;
             i++) {
            WCHAR label[32];
            if (freqList[i] == curFreq)
                wsprintfW(label, L">>  %uHz", freqList[i]);
            else
                wsprintfW(label, L"%uHz", freqList[i]);

            UINT menuId = IDM_FREQ_BASE + (UINT)g_menuFreqCount;
            g_menuFreqs[g_menuFreqCount] = freqList[i];
            g_menuFreqCount++;

            AppendMenuW(hFreqMenu, MF_STRING, menuId, label);
        }

        WCHAR freqLabel[32];
        wsprintfW(freqLabel, L"Refresh Rate  [%uHz]", curFreq);
        AppendMenuW(hMenu, MF_STRING | MF_POPUP,
                    (UINT_PTR)hFreqMenu, freqLabel);

        /* HDR items — one per active HDR-capable monitor */
        BOOL anyHdrShown = FALSE;
        g_menuHdrCount = 0;

        for (i = 0; i < monCount; i++) {
            if (!monitors[i].isActive)
                continue;
            if (!IsHdrSupported(monitors[i].luidLow,
                                monitors[i].luidHigh,
                                monitors[i].targetId))
                continue;

            anyHdrShown = TRUE;

            BOOL hdrEnabled;
            BOOL hdrFound = ReadHdrFromRegistry(monitors[i].devPath,
                                                &hdrEnabled);

            WCHAR label[128];
            if (hdrFound) {
                wsprintfW(label, L"HDR: %s  [%s]",
                          monitors[i].name,
                          hdrEnabled ? L"ON" : L"OFF");
                UINT menuId = IDM_HDR_BASE + (UINT)g_menuHdrCount;
                g_menuHdrIds[g_menuHdrCount] = monitors[i].targetId;
                g_menuHdrCount++;
                AppendMenuW(hMenu, MF_STRING, menuId, label);
            } else {
                wsprintfW(label, L"HDR: %s  [unknown]",
                          monitors[i].name);
                AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, label);
            }
        }

        if (!anyHdrShown) {
            AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,
                        L"HDR  [not supported]");
        }
    }

    /* ── Actions ── */
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    if (IsTopologyChanged())
        AppendMenuW(hMenu, MF_STRING, IDM_RESTORE,
                    L"Restore original config");
    else
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,
                    L"Restore original config");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    /*
     * SetForegroundWindow is required before TrackPopupMenu, otherwise
     * the menu won't dismiss when clicking outside (KB135788).
     */
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwndMain);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, g_hwndMain, NULL);

    /* Post WM_NULL for proper menu dismissal (documented Win32 quirk) */
    PostMessage(g_hwndMain, WM_NULL, 0, 0);

    /* DestroyMenu on the root also destroys all submenus */
    DestroyMenu(hMenu);
}

/*
 * Asks for confirmation before switching to exclusive mode.
 * Skips if the monitor is already the only active one.
 */
static void ConfirmSwitch(UINT32 targetId)
{
    MonitorInfo monitors[MAX_MONITORS];
    int monCount = GetAllMonitors(monitors, MAX_MONITORS);

    WCHAR name[64];
    wsprintfW(name, L"target %u", targetId);
    BOOL alreadyActive = FALSE;
    int activeCount = 0;

    int i;
    for (i = 0; i < monCount; i++) {
        if (monitors[i].isActive) activeCount++;
        if (monitors[i].targetId == targetId) {
            lstrcpynW(name, monitors[i].name, 64);
            if (monitors[i].isActive) alreadyActive = TRUE;
        }
    }

    /* Nothing to change if this is already the only active monitor */
    if (alreadyActive && activeCount == 1)
        return;

    WCHAR msg[256];
    wsprintfW(msg,
        L"Activate ONLY %s and turn off all others?\n\n"
        L"Restore anytime with Ctrl+Alt+R or the "
        L"tray menu (Ctrl+Alt+M).", name);

    if (MessageBoxW(NULL, msg, L"MonitorSwitcher",
                    MB_YESNO | MB_ICONWARNING) == IDYES) {
        SetExclusiveMonitor(targetId);
    }
}

/* ─── Exit Handler ──────────────────────────────────────────────────── */

/*
 * Called when the user clicks "Exit" in the tray menu.
 * If topology has changed, prompts the user to restore before exiting.
 * Cancel aborts the exit entirely.
 */
static void ExitHandler(void)
{
    if (IsTopologyChanged()) {
        int result = MessageBoxW(NULL,
            L"Restore display configuration before exiting?",
            L"MonitorSwitcher", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (result == IDCANCEL)
            return;
        if (result == IDYES) {
            RestoreOriginal();
            /*
             * Brief pause to let displays settle before exiting.
             * Acceptable here because the app is about to terminate
             * and the message loop has no further work to process.
             */
            Sleep(2000);
        }
    }
    CleanExit();
}

/* ─── System Tray ───────────────────────────────────────────────────── */

/*
 * Sets up the system tray icon.
 * Loads the icon embedded in the executable as resource ID 1
 * (defined in MonitorSwitcher.rc).  Falls back to the generic
 * Windows application icon if the resource is not found.
 */
static void SetupTrayIcon(void)
{
    g_hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (g_hIcon == NULL)
        g_hIcon = LoadIconW(NULL, IDI_APPLICATION);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwndMain;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_hIcon;
    lstrcpyW(g_nid.szTip, L"MonitorSwitcher");

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

/*
 * Removes the system tray icon and frees the icon resource.
 */
static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hIcon) {
        DestroyIcon(g_hIcon);
        g_hIcon = NULL;
    }
}

/*
 * Updates the tray tooltip text based on whether topology changed.
 */
static void UpdateTooltip(void)
{
    if (IsTopologyChanged())
        lstrcpyW(g_nid.szTip, L"MonitorSwitcher [modified]");
    else
        lstrcpyW(g_nid.szTip, L"MonitorSwitcher");

    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/*
 * Shows a balloon notification from the tray icon.
 * Integrates with the Windows 10/11 notification center automatically.
 */
static void ShowBalloon(const WCHAR *title, const WCHAR *text)
{
    g_nid.uFlags     = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, title,
              sizeof(g_nid.szInfoTitle) / sizeof(WCHAR));
    lstrcpynW(g_nid.szInfo, text,
              sizeof(g_nid.szInfo) / sizeof(WCHAR));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ─── Hotkeys ───────────────────────────────────────────────────────── */

/*
 * Dynamically registers Ctrl+Alt+1..9 hotkeys based on connected monitors.
 * Called at startup and when topology changes.
 */
static void UpdateMonitorHotkeys(void)
{
    MonitorInfo monitors[MAX_MONITORS];
    int monCount = GetAllMonitors(monitors, MAX_MONITORS);

    /* Unregister old hotkeys */
    int i;
    for (i = 0; i < g_hotkeyMonitorCount; i++) {
        UnregisterHotKey(g_hwndMain, HOTKEY_MONITOR_BASE + i);
    }

    g_hotkeyMonitorCount = 0;

    /* Register new hotkeys (up to 9) */
    int toRegister = (monCount > 9) ? 9 : monCount;
    for (i = 0; i < toRegister; i++) {
        RegisterHotKey(g_hwndMain, HOTKEY_MONITOR_BASE + i,
                       MOD_CONTROL | MOD_ALT, '1' + i);
    }

    g_hotkeyMonitorCount = toRegister;
}

/* ─── Auto-Start ────────────────────────────────────────────────────── */

/*
 * Checks whether MonitorSwitcher is registered to start with Windows.
 * Reads HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
 */
static BOOL IsAutoStartEnabled(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY,
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    DWORD valueType = 0;
    DWORD valueSize = 0;
    LONG ret = RegQueryValueExW(hKey, AUTOSTART_VALUE, NULL,
                                &valueType, NULL, &valueSize);
    RegCloseKey(hKey);

    return (ret == ERROR_SUCCESS && valueType == REG_SZ);
}

/*
 * Enables or disables auto-start with Windows.
 * When enabling, writes the full path of the current executable to
 * HKCU\...\Run so Windows launches it at logon.
 * When disabling, deletes the registry value.
 */
static void SetAutoStart(BOOL enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY,
                      0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        RegSetValueExW(hKey, AUTOSTART_VALUE, 0, REG_SZ,
                       (const BYTE *)exePath,
                       (DWORD)((lstrlenW(exePath) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hKey, AUTOSTART_VALUE);
    }

    RegCloseKey(hKey);
}

/* ─── Clean Exit ────────────────────────────────────────────────────── */

/*
 * Performs a clean shutdown:
 *   1. Kill the display-change debounce timer
 *   2. Unregister all global hotkeys
 *   3. Remove the system tray icon
 *   4. Destroy the main window → WM_DESTROY → PostQuitMessage →
 *      message loop exits → program terminates
 */
static void CleanExit(void)
{
    KillTimer(g_hwndMain, TIMER_REBUILD);
    UnregisterHotKey(g_hwndMain, HOTKEY_RESTORE);
    UnregisterHotKey(g_hwndMain, HOTKEY_MENU);
    UnregisterHotKey(g_hwndMain, HOTKEY_HDR);
    
    int i;
    for (i = 0; i < g_hotkeyMonitorCount; i++) {
        UnregisterHotKey(g_hwndMain, HOTKEY_MONITOR_BASE + i);
    }
    
    RemoveTrayIcon();
    DestroyWindow(g_hwndMain);
}

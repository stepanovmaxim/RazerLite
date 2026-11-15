// RazerLite - a tray utility for Razer mice: DPI stages with an on-screen
// indicator, polling rate, sleep timer, battery, and button remapping, without
// Razer Synapse running in the background.
//
// Nothing is model specific. Any Razer device that answers the vendor protocol
// is picked up, the transaction id is probed, and unsupported commands simply
// do not appear in the menu.
//
// Build: see build.cmd (MSVC, no CRT).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include "razer.h"
#include "update.h"

// Bump this with every published release: it is what the update check compares
// against the newest tag on GitHub, and what app.rc reports in the file
// properties.
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 1
#define APP_VERSION_STR   L"1.0.1"

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "cfgmgr32.lib")   // Bluetooth battery lives in a device property

// ---------------------------------------------------------------- constants

#define WM_TRAYICON      (WM_USER + 1)
#define WM_APP_STATE     (WM_USER + 2)   // HID thread: state snapshot updated
#define WM_APP_SHOW_OSD  (WM_USER + 3)   // wParam = dpi, lParam = stage | count<<8
#define WM_APP_SHOW_BATT (WM_USER + 4)   // wParam = percent, lParam = 1 when charging
#define WM_APP_UPDATE    (WM_USER + 5)   // wParam = UPD_*, lParam = check was silent

// messages posted to the HID worker thread
#define HIDM_QUIT        (WM_USER + 20)
#define HIDM_RESCAN      (WM_USER + 21)
#define HIDM_REFRESH     (WM_USER + 22)  // wParam = 1: device hinted a change
#define HIDM_SET_STAGE   (WM_USER + 23)  // wParam = stage, 1-based
#define HIDM_STEP_STAGE  (WM_USER + 24)  // wParam = 1 (next) or 0 (previous)
#define HIDM_SET_POLL    (WM_USER + 25)  // wParam = Hz
#define HIDM_SET_IDLE    (WM_USER + 26)  // wParam = seconds
#define HIDM_SET_SDPI    (WM_USER + 27)  // wParam = stage index 0-based, lParam = dpi
#define HIDM_SET_COUNT   (WM_USER + 28)  // wParam = stage count
#define HIDM_SET_DEVICE  (WM_USER + 29)  // wParam = device index
#define HIDM_SET_MODE    (WM_USER + 30)  // wParam = device mode byte
#define HIDM_SET_DPI     (WM_USER + 31)  // wParam = dpi, for mice without stages
#define HIDM_BLE_DPI     (WM_USER + 32)  // wParam = dpi announced over Bluetooth

#define IDM_DPI_VALUE_BASE 600           // + index into DPI_LADDER

#define IDM_EXIT          100
#define IDM_STARTUP       101
#define IDM_REFRESH       102
#define IDM_OSD           103
#define IDM_BTN_RESET     104
#define IDM_TAKEOVER      105
#define IDM_DPI_NEXT      107
#define IDM_DPI_PREV      108
#define IDM_DUMP          109
#define IDM_DPI_STAGE_BASE 200   // + stage index (0..4)
#define IDM_POLL_BASE      220   // + index into POLL_RATES
#define IDM_IDLE_BASE      240   // + index into IDLE_MINUTES
#define IDM_DEVICE_BASE    260   // + device index
#define IDM_COUNT_BASE     280   // + stage count (1..5)
#define IDM_BATT_BASE      300   // + index into BATTERY_LEVELS
#define IDM_BTN_FORGET     110
#define IDM_TEST_OSD       111
#define IDM_CHECK_UPDATE   112
#define IDM_AUTO_UPDATE    113

#define TIMER_UPDATE        4
#define UPDATE_FIRST_MS   30000              // settle in before the first check
#define UPDATE_EVERY_MS  (6 * 60 * 60 * 1000) // and look again four times a day
#define IDM_STAGEVAL_BASE 1000   // + stage * 100 + value index
#define IDM_ACTION_BASE   2000   // + standard button * 100 + action
#define IDM_VACTION_BASE  3000   // + vendor button * 100 + action

#define IDI_APPICON       101

#define TIMER_OSD           1
#define OSD_HOLD_MS       900
#define OSD_HOLD_LONG_MS 3500        // battery warnings stay up longer
#define OSD_FADE_STEP_MS   25
#define OSD_FADE_STEPS     14

// overlay kinds
#define OSD_DPI       0
#define OSD_BATTERY   1

#define BATTERY_REMIND_MS  (30 * 60 * 1000)   // nag at most twice an hour

static const WCHAR CLASS_NAME[]  = L"RazerLiteTray";
static const WCHAR OSD_CLASS[]   = L"RazerLiteOsd";
static const WCHAR MUTEX_NAME[]  = L"Local\\RazerLiteMutex";
static const WCHAR REG_APP[]     = L"Software\\RazerLite";
static const WCHAR REG_RUN[]     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR RUN_VALUE[]   = L"RazerLite";

static const int POLL_RATES[]     = { 125, 250, 500, 1000 };
static const int BATTERY_LEVELS[] = { 0, 5, 10, 15, 20, 25 };   // 0 = warning off
static const int IDLE_MINUTES[] = { 1, 3, 5, 10, 15 };
static const int DPI_LADDER[]   = { 200, 400, 600, 800, 1000, 1200, 1400, 1600, 1800,
                                    2000, 2400, 2800, 3200, 4000, 4800, 6400, 8000,
                                    10000, 12000, 14000, 16000, 18000, 20000 };

// ------------------------------------------------- freestanding CRT helpers

#ifdef RAZERLITE_NO_CRT
#pragma function(memset)
extern "C" void *memset(void *dst, int value, size_t count) {
    unsigned char *p = (unsigned char *)dst;
    while (count--) *p++ = (unsigned char)value;
    return dst;
}

#pragma function(memcpy)
extern "C" void *memcpy(void *dst, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (count--) *d++ = *s++;
    return dst;
}
#else
#include <string.h>
#endif

// ------------------------------------------------------------------ buttons

// Razer mice split their buttons across two paths, and remapping needs both:
//
//   standard - left, right, wheel click and the two side buttons arrive as
//              ordinary Windows mouse events, so a low-level hook can swallow
//              and replace them;
//   vendor   - DPI, profile, sniper and keypad buttons never reach Windows at
//              all. In driver mode the mouse reports them on a vendor HID
//              collection instead, which is where they get picked up. Nothing
//              needs suppressing there, because nothing else acts on them.
//
// Which vendor codes exist differs per model, so they are not hard coded: any
// code seen is remembered and offered in the menu.

enum {
    BTN_LEFT = 0,
    BTN_RIGHT,
    BTN_MIDDLE,
    BTN_X1,                     // side button 4, "back" by default
    BTN_X2,                     // side button 5, "forward" by default
    BTN_COUNT
};

#define MAX_VENDOR_BUTTONS  12

typedef struct {
    DWORD code;                 // (report id << 16) | (byte index << 8) | value
    int   action;
    BOOL  movedDpi;             // a DPI change once followed this code
    BOOL  ble;                  // seen over Bluetooth rather than the dongle
} VendorButton;

enum {
    ACT_DEFAULT = 0, ACT_DISABLED,
    ACT_DPI_NEXT, ACT_DPI_PREV,
    ACT_LEFT, ACT_RIGHT, ACT_MIDDLE, ACT_BACK, ACT_FORWARD, ACT_DOUBLECLICK,
    ACT_COPY, ACT_PASTE, ACT_UNDO,
    ACT_CLOSETAB, ACT_NEWTAB, ACT_REOPENTAB,
    ACT_ALTTAB, ACT_TASKVIEW, ACT_SHOWDESKTOP,
    ACT_PLAYPAUSE, ACT_NEXTTRACK, ACT_PREVTRACK, ACT_MUTE, ACT_VOLUP, ACT_VOLDOWN,
    ACT_COUNT
};

static const WCHAR *const ACTION_NAMES[ACT_COUNT] = {
    L"Default (do not touch)",
    L"Disabled",
    L"DPI: next stage",
    L"DPI: previous stage",
    L"Left click",
    L"Right click",
    L"Middle click",
    L"Browser back",
    L"Browser forward",
    L"Double click",
    L"Copy\tCtrl+C",
    L"Paste\tCtrl+V",
    L"Undo\tCtrl+Z",
    L"Close tab\tCtrl+W",
    L"New tab\tCtrl+T",
    L"Reopen tab\tCtrl+Shift+T",
    L"Switch window\tAlt+Tab",
    L"Task view\tWin+Tab",
    L"Show desktop\tWin+D",
    L"Media: play / pause",
    L"Media: next track",
    L"Media: previous track",
    L"Volume: mute",
    L"Volume: up",
    L"Volume: down"
};

static const WCHAR *const BUTTON_NAMES[BTN_COUNT] = {
    L"Left",
    L"Right",
    L"Middle (wheel click)",
    L"Side back (button 4)",
    L"Side forward (button 5)"
};

// ------------------------------------------------------------------ globals

typedef struct {
    BOOL  present;
    BOOL  awake;
    BOOL  bluetooth;
    BOOL  vendorProtocol;       // false on a Bluetooth link: read-only, mostly
    WCHAR name[128];
    int   dpi;
    int   stageCount, activeStage;
    int   stageDpi[RAZER_MAX_STAGES];
    int   battery;
    BOOL  charging;
    int   pollRate, idleSeconds, deviceMode, maxDpi;
    BYTE  fwMajor, fwMinor;
    BOOL  capDpi, capStages, capPoll, capBattery, capCharging, capIdle;
    int   deviceCount, activeIndex;
    WCHAR deviceNames[RAZER_MAX_DEVICES][64];
} UiState;

static CRITICAL_SECTION g_lock;
static UiState   g_ui;                  // guarded by g_lock
static HINSTANCE g_hInst;
static HWND      g_hwnd;
static HWND      g_osd;
static NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };
static HICON     g_trayIcon;      // one of the three below, never owned
static HICON     g_iconBase;      // the application icon, as authored
static HICON     g_iconMuted;     // greyed: no mouse found
static HICON     g_iconAlert;     // red: battery nearly flat
static HDEVNOTIFY g_devNotify;
static UINT      g_msgTaskbarCreated;
static DWORD     g_hidThreadId;
static HANDLE    g_hidThread;
static HANDLE    g_watchThread;
static HANDLE    g_watchStop;
static BOOL      g_osdEnabled = TRUE;
static int       g_osdStep;
static int       g_osdKind;
static int       g_osdDpi, g_osdStage, g_osdStageCount;
static int       g_osdBattery;
static int       g_batteryWarnAt = 20;      // percent, 0 disables the warning
static int       g_forceMode = -1;          // device mode to re-apply, -1 = leave alone
static BOOL      g_autoUpdate = TRUE;       // install new releases without asking
static volatile LONG g_updateBusy;          // one update thread at a time
static BOOL      g_updateAnnounce;          // this process is the updated build
static DWORD     g_updatedPid;              // the build we are replacing
static volatile LONG g_action[BTN_COUNT];   // read by the hook thread
static VendorButton  g_vendor[MAX_VENDOR_BUTTONS];   // guarded by g_lock
static int           g_vendorCount;
static DWORD         g_lastVendorCode;      // for the "this moved the DPI" hint
static DWORD         g_lastVendorTick;
static HHOOK     g_hook;
static DWORD     g_hookThreadId;
static HANDLE    g_hookThread;
static UINT      g_pendingCommand;   // a command line request, once the scan is in
static volatile LONG g_diagTicks, g_diagBatteryPolls, g_diagWarns, g_diagIdleMs;
static volatile LONG g_activeWatchDevice = -1;   // link that last delivered a report

// ------------------------------------------------------------ small helpers

// Case-insensitive substring test; `needle` must already be lower case.
static BOOL PathContains(const WCHAR *path, const WCHAR *needle) {
    for (; *path; ++path) {
        int i = 0;
        while (needle[i]) {
            WCHAR c = path[i];
            if (c >= L'A' && c <= L'Z') c = (WCHAR)(c + 32);
            if (c != needle[i]) break;
            ++i;
        }
        if (!needle[i]) return TRUE;
    }
    return FALSE;
}

// ----------------------------------------------------------------- registry

static DWORD RegReadDword(const WCHAR *name, DWORD fallback) {
    HKEY key;
    DWORD value = fallback, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS ||
            type != REG_DWORD)
            value = fallback;
        RegCloseKey(key);
    }
    return value;
}

static void RegWriteDword(const WCHAR *name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
        RegCloseKey(key);
    }
}

static BOOL IsStartupEnabled(void) {
    HKEY key;
    BOOL enabled = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        enabled = (RegQueryValueExW(key, RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        RegCloseKey(key);
    }
    return enabled;
}

static void SetStartup(BOOL enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_WRITE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        WCHAR path[MAX_PATH + 4];
        path[0] = L'"';
        DWORD len = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            path[len + 1] = L'"';
            path[len + 2] = 0;
            RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)path,
                           (len + 3) * sizeof(WCHAR));
        }
    } else {
        RegDeleteValueW(key, RUN_VALUE);
    }
    RegCloseKey(key);
}

// ------------------------------------------------------------- input actions

static void SendKeys(const WORD *vk, int count) {
    INPUT in[8];
    int n = 0;
    for (int i = 0; i < count && i < 4; ++i) {           // press in order
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk[i]; in[n].ki.wScan = 0; in[n].ki.dwFlags = 0;
        in[n].ki.time = 0; in[n].ki.dwExtraInfo = 0;
        ++n;
    }
    for (int i = count - 1; i >= 0 && i < 4; --i) {      // release in reverse
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk[i]; in[n].ki.wScan = 0; in[n].ki.dwFlags = KEYEVENTF_KEYUP;
        in[n].ki.time = 0; in[n].ki.dwExtraInfo = 0;
        ++n;
    }
    SendInput(n, in, sizeof(INPUT));
}

static void SendMouseButton(DWORD downFlag, DWORD upFlag, DWORD data, int times) {
    INPUT in[4];
    int n = 0;
    for (int i = 0; i < times && n < 4; ++i) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = in[n].mi.dy = 0; in[n].mi.mouseData = data;
        in[n].mi.dwFlags = downFlag; in[n].mi.time = 0; in[n].mi.dwExtraInfo = 0;
        ++n;
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = in[n].mi.dy = 0; in[n].mi.mouseData = data;
        in[n].mi.dwFlags = upFlag; in[n].mi.time = 0; in[n].mi.dwExtraInfo = 0;
        ++n;
    }
    SendInput(n, in, sizeof(INPUT));
}

// Runs on the hook thread, so it has to stay quick: anything that talks to the
// mouse is handed to the HID worker instead.
static void PerformAction(int action) {
    WORD keys[4];
    switch (action) {
    case ACT_DPI_NEXT:  PostThreadMessageW(g_hidThreadId, HIDM_STEP_STAGE, 1, 0); break;
    case ACT_DPI_PREV:  PostThreadMessageW(g_hidThreadId, HIDM_STEP_STAGE, 0, 0); break;

    case ACT_LEFT:      SendMouseButton(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0, 1); break;
    case ACT_RIGHT:     SendMouseButton(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0, 1); break;
    case ACT_MIDDLE:    SendMouseButton(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0, 1); break;
    case ACT_DOUBLECLICK: SendMouseButton(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0, 2); break;
    case ACT_BACK:      SendMouseButton(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1, 1); break;
    case ACT_FORWARD:   SendMouseButton(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2, 1); break;

    case ACT_COPY:      keys[0] = VK_CONTROL; keys[1] = 'C'; SendKeys(keys, 2); break;
    case ACT_PASTE:     keys[0] = VK_CONTROL; keys[1] = 'V'; SendKeys(keys, 2); break;
    case ACT_UNDO:      keys[0] = VK_CONTROL; keys[1] = 'Z'; SendKeys(keys, 2); break;
    case ACT_CLOSETAB:  keys[0] = VK_CONTROL; keys[1] = 'W'; SendKeys(keys, 2); break;
    case ACT_NEWTAB:    keys[0] = VK_CONTROL; keys[1] = 'T'; SendKeys(keys, 2); break;
    case ACT_REOPENTAB: keys[0] = VK_CONTROL; keys[1] = VK_SHIFT; keys[2] = 'T';
                        SendKeys(keys, 3); break;
    case ACT_ALTTAB:    keys[0] = VK_MENU; keys[1] = VK_TAB; SendKeys(keys, 2); break;
    case ACT_TASKVIEW:  keys[0] = VK_LWIN; keys[1] = VK_TAB; SendKeys(keys, 2); break;
    case ACT_SHOWDESKTOP: keys[0] = VK_LWIN; keys[1] = 'D'; SendKeys(keys, 2); break;

    case ACT_PLAYPAUSE: keys[0] = VK_MEDIA_PLAY_PAUSE; SendKeys(keys, 1); break;
    case ACT_NEXTTRACK: keys[0] = VK_MEDIA_NEXT_TRACK; SendKeys(keys, 1); break;
    case ACT_PREVTRACK: keys[0] = VK_MEDIA_PREV_TRACK; SendKeys(keys, 1); break;
    case ACT_MUTE:      keys[0] = VK_VOLUME_MUTE; SendKeys(keys, 1); break;
    case ACT_VOLUP:     keys[0] = VK_VOLUME_UP; SendKeys(keys, 1); break;
    case ACT_VOLDOWN:   keys[0] = VK_VOLUME_DOWN; SendKeys(keys, 1); break;

    default: break;    // ACT_DISABLED swallows the click and does nothing
    }
}

static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lParam;
        if (!(m->flags & LLMHF_INJECTED)) {          // never react to our own input
            int button = -1;
            BOOL down = FALSE;
            switch (wParam) {
            case WM_LBUTTONDOWN: button = BTN_LEFT; down = TRUE; break;
            case WM_LBUTTONUP:   button = BTN_LEFT; break;
            case WM_RBUTTONDOWN: button = BTN_RIGHT; down = TRUE; break;
            case WM_RBUTTONUP:   button = BTN_RIGHT; break;
            case WM_MBUTTONDOWN: button = BTN_MIDDLE; down = TRUE; break;
            case WM_MBUTTONUP:   button = BTN_MIDDLE; break;
            case WM_XBUTTONDOWN:
                button = (HIWORD(m->mouseData) == XBUTTON1) ? BTN_X1 : BTN_X2;
                down = TRUE;
                break;
            case WM_XBUTTONUP:
                button = (HIWORD(m->mouseData) == XBUTTON1) ? BTN_X1 : BTN_X2;
                break;
            }
            if (button >= 0) {
                LONG action = g_action[button];
                if (action != ACT_DEFAULT) {
                    if (down) PerformAction((int)action);
                    return 1;                        // swallow press and release
                }
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static BOOL AnyRemapActive(void) {
    for (int i = 0; i < BTN_COUNT; ++i)
        if (g_action[i] != ACT_DEFAULT) return TRUE;
    return FALSE;
}

// The hook lives on its own thread: a low-level hook runs on the thread that
// installed it, and blocking that thread would stall every mouse event in the
// session.
static DWORD WINAPI HookThreadProc(LPVOID) {
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_hInst, 0);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    return 0;
}

static void UpdateHookState(void) {
    BOOL wanted = AnyRemapActive();
    if (wanted && !g_hookThread) {
        g_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &g_hookThreadId);
    } else if (!wanted && g_hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
        g_hookThread = NULL;
        g_hookThreadId = 0;
    }
}

static void SetAction(int button, int action) {
    WCHAR name[8];
    g_action[button] = action;
    wsprintfW(name, L"Std%d", button);
    RegWriteDword(name, (DWORD)action);
    UpdateHookState();
}

// ------------------------------------------------------- vendor button table

static void SaveVendorButtons(void) {
    DWORD blob[MAX_VENDOR_BUTTONS * 3];
    int count;

    EnterCriticalSection(&g_lock);
    count = g_vendorCount;
    for (int i = 0; i < count; ++i) {
        blob[i * 3 + 0] = g_vendor[i].code;
        blob[i * 3 + 1] = (DWORD)g_vendor[i].action;
        blob[i * 3 + 2] = (g_vendor[i].movedDpi ? 1u : 0u) | (g_vendor[i].ble ? 2u : 0u);
    }
    LeaveCriticalSection(&g_lock);

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExW(key, L"VendorButtons", 0, REG_BINARY, (const BYTE *)blob,
                       (DWORD)(count * 3 * sizeof(DWORD)));
        RegCloseKey(key);
    }
}

static void LoadButtons(void) {
    WCHAR name[8];
    for (int i = 0; i < BTN_COUNT; ++i) {
        wsprintfW(name, L"Std%d", i);
        DWORD v = RegReadDword(name, ACT_DEFAULT);
        g_action[i] = (v < ACT_COUNT) ? (LONG)v : ACT_DEFAULT;
    }

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    DWORD blob[MAX_VENDOR_BUTTONS * 3];
    DWORD size = sizeof(blob), type = 0;
    if (RegQueryValueExW(key, L"VendorButtons", NULL, &type, (BYTE *)blob, &size)
        == ERROR_SUCCESS && type == REG_BINARY) {
        int count = (int)(size / (3 * sizeof(DWORD)));
        if (count > MAX_VENDOR_BUTTONS) count = MAX_VENDOR_BUTTONS;
        for (int i = 0; i < count; ++i) {
            g_vendor[i].code     = blob[i * 3 + 0];
            g_vendor[i].action   = (int)blob[i * 3 + 1];
            g_vendor[i].movedDpi = (blob[i * 3 + 2] & 1u) != 0;
            g_vendor[i].ble      = (blob[i * 3 + 2] & 2u) != 0;
            if (g_vendor[i].action < 0 || g_vendor[i].action >= ACT_COUNT)
                g_vendor[i].action = ACT_DEFAULT;
        }
        g_vendorCount = count;
    }
    RegCloseKey(key);
}

// Razer numbers its own DPI signals 0x20 (up) and 0x21 (down). Seeding those
// two makes the DPI button work the moment it is first pressed, the way it did
// under Synapse; anything else starts out doing nothing and waits to be bound.
static int SuggestedAction(DWORD code) {
    BYTE value = (BYTE)(code & 0xFF);
    if (value == 0x20) return ACT_DPI_NEXT;
    if (value == 0x21) return ACT_DPI_PREV;
    return ACT_DEFAULT;
}

// The action bound to a code on this link, or -1 when it has never been seen.
// The link is part of the identity: the same byte can mean different things on
// the dongle and over Bluetooth, and a binding only applies where it was made.
static int LookupVendorAction(DWORD code, BOOL ble) {
    int action = -1;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_vendorCount; ++i)
        if (g_vendor[i].code == code && g_vendor[i].ble == ble) {
            action = g_vendor[i].action;
            break;
        }
    LeaveCriticalSection(&g_lock);
    return action;
}

// Called from the watcher thread. Returns the action bound to this code, and
// records codes that have not been seen before.
static int RegisterVendorPress(DWORD code, BOOL ble, BOOL *isNew) {
    int action = ACT_DEFAULT;
    *isNew = FALSE;

    EnterCriticalSection(&g_lock);
    int index = -1;
    for (int i = 0; i < g_vendorCount; ++i)
        if (g_vendor[i].code == code && g_vendor[i].ble == ble) { index = i; break; }

    if (index < 0 && g_vendorCount < MAX_VENDOR_BUTTONS) {
        index = g_vendorCount++;
        g_vendor[index].code     = code;
        g_vendor[index].action   = SuggestedAction(code);
        g_vendor[index].movedDpi = FALSE;
        g_vendor[index].ble      = ble;
        *isNew = TRUE;
    }
    if (index >= 0) action = g_vendor[index].action;

    g_lastVendorCode = code;
    g_lastVendorTick = GetTickCount();
    LeaveCriticalSection(&g_lock);
    return action;
}

// Called from the HID thread when the DPI moved on its own: if a vendor button
// was pressed a moment ago, that button is the DPI button and can say so.
static void NoteDpiChangeSource(void) {
    EnterCriticalSection(&g_lock);
    if (g_lastVendorCode && GetTickCount() - g_lastVendorTick < 700) {
        for (int i = 0; i < g_vendorCount; ++i)
            if (g_vendor[i].code == g_lastVendorCode) { g_vendor[i].movedDpi = TRUE; break; }
    }
    LeaveCriticalSection(&g_lock);
}

static void SetVendorAction(int index, int action) {
    EnterCriticalSection(&g_lock);
    if (index >= 0 && index < g_vendorCount) g_vendor[index].action = action;
    LeaveCriticalSection(&g_lock);
    SaveVendorButtons();
}

// ------------------------------------------------------- HID worker: helpers

static RazerDevice g_dev[RAZER_MAX_DEVICES];   // owned by the HID thread only
static int  g_devCount;
static int  g_devActive = -1;
static int  g_lastDpi, g_lastStage;
static BOOL g_haveBaseline;

static void PublishState(void) {
    EnterCriticalSection(&g_lock);
    RzZero(&g_ui, sizeof(g_ui));
    g_ui.deviceCount = g_devCount;
    g_ui.activeIndex = g_devActive;
    for (int i = 0; i < g_devCount; ++i)
        lstrcpynW(g_ui.deviceNames[i], g_dev[i].name, 64);

    if (g_devActive >= 0) {
        RazerDevice *d = &g_dev[g_devActive];
        g_ui.present        = TRUE;
        g_ui.awake          = d->awake;
        g_ui.bluetooth      = d->bluetooth;
        g_ui.vendorProtocol = !d->bleOnly;
        lstrcpynW(g_ui.name, d->name, ARRAYSIZE(g_ui.name));
        g_ui.dpi         = d->dpiX;
        g_ui.stageCount  = d->stageCount;
        g_ui.activeStage = d->activeStage;
        for (int i = 0; i < RAZER_MAX_STAGES; ++i) g_ui.stageDpi[i] = d->stageDpi[i];
        g_ui.battery     = d->battery;
        g_ui.charging    = d->charging;
        g_ui.pollRate    = d->pollRate;
        g_ui.idleSeconds = d->idleSeconds;
        g_ui.deviceMode  = d->deviceMode;
        g_ui.maxDpi      = d->maxDpi;
        g_ui.fwMajor     = d->fwMajor;
        g_ui.fwMinor     = d->fwMinor;
        g_ui.capDpi      = d->capDpi;
        g_ui.capStages   = d->capStages;
        g_ui.capPoll     = d->capPollRate;
        g_ui.capBattery  = d->capBattery;
        g_ui.capCharging = d->capCharging;
        g_ui.capIdle     = d->capIdle;
    }
    LeaveCriticalSection(&g_lock);
    PostMessageW(g_hwnd, WM_APP_STATE, 0, 0);
}

// Reads DPI and stages, and raises the overlay when they moved on their own
// (the DPI button on the mouse) or because we just changed them.
static void RefreshDpi(BOOL allowOsd) {
    if (g_devActive < 0) return;
    RazerDevice *d = &g_dev[g_devActive];
    if (d->bleOnly) return;         // no vendor protocol over Bluetooth LE

    // A mouse that was asleep when we found it has no capability list yet.
    // Whatever device mode was chosen has to be re-applied here too: it lives
    // in the mouse only until it is unplugged, re-paired or power cycled.
    if (!d->probed) {
        if (!RazerProbeCaps(d)) return;
        if (g_forceMode >= 0 && d->deviceMode != g_forceMode) {
            RazerSetDeviceMode(d, (BYTE)g_forceMode);
            d->deviceMode = RazerGetDeviceMode(d);
        }
    }

    BOOL ok = FALSE;
    if (d->capStages) ok = RazerGetStages(d);
    if (d->capDpi)    ok = RazerGetDpi(d) || ok;
    d->awake = ok;
    if (!ok) return;                     // asleep again: keep the last values
    if (d->capStages && d->activeStage >= 1 && d->activeStage <= d->stageCount)
        d->dpiX = d->stageDpi[d->activeStage - 1];

    BOOL changed = (d->dpiX != g_lastDpi) || (d->activeStage != g_lastStage);
    g_lastDpi   = d->dpiX;
    g_lastStage = d->activeStage;

    if (changed && allowOsd && g_haveBaseline) {
        NoteDpiChangeSource();
        PostMessageW(g_hwnd, WM_APP_SHOW_OSD, (WPARAM)d->dpiX,
                     (LPARAM)(d->activeStage | (d->stageCount << 8)));
    }
    g_haveBaseline = TRUE;
}

static void StopWatcher(void);
static void StartWatcher(void);

static void HidRescan(void) {
    StopWatcher();
    for (int i = 0; i < g_devCount; ++i) RazerCloseDevice(&g_dev[i]);
    g_devCount   = RazerFindDevices(g_dev, RAZER_MAX_DEVICES);

    // With the dongle still plugged in while the mouse talks over Bluetooth,
    // both show up and only one of them answers. Pick the one that did.
    g_devActive = (g_devCount > 0) ? 0 : -1;
    for (int i = 0; i < g_devCount; ++i)
        if (g_dev[i].probed) { g_devActive = i; break; }
    g_haveBaseline = FALSE;
    if (g_devActive >= 0 && g_forceMode >= 0 && g_dev[g_devActive].probed &&
        g_dev[g_devActive].deviceMode != g_forceMode) {
        RazerSetDeviceMode(&g_dev[g_devActive], (BYTE)g_forceMode);
        g_dev[g_devActive].deviceMode = RazerGetDeviceMode(&g_dev[g_devActive]);
    }
    if (g_devActive >= 0) RefreshDpi(FALSE);
    StartWatcher();
    PublishState();
}

static void HidSetStage(int stage) {
    if (g_devActive < 0) return;
    RazerDevice *d = &g_dev[g_devActive];
    if (!d->capStages || stage < 1 || stage > d->stageCount) return;
    RazerSetStages(d, stage, d->stageCount, d->stageDpi);
    RefreshDpi(TRUE);
    PublishState();
}

static void HidStepStage(int direction) {
    if (g_devActive < 0) return;
    RazerDevice *d = &g_dev[g_devActive];
    if (!d->capStages || d->stageCount < 1) return;
    int stage = d->activeStage + (direction ? 1 : -1);
    if (stage > d->stageCount) stage = 1;
    if (stage < 1) stage = d->stageCount;
    HidSetStage(stage);
}

static void HidSetStageDpi(int index, int dpi) {
    if (g_devActive < 0) return;
    RazerDevice *d = &g_dev[g_devActive];
    if (!d->capStages || index < 0 || index >= d->stageCount) return;
    int table[RAZER_MAX_STAGES];
    for (int i = 0; i < RAZER_MAX_STAGES; ++i) table[i] = d->stageDpi[i];
    table[index] = dpi;
    RazerSetStages(d, d->activeStage, d->stageCount, table);
    RefreshDpi(TRUE);
    PublishState();
}

static void HidSetStageCount(int count) {
    if (g_devActive < 0) return;
    RazerDevice *d = &g_dev[g_devActive];
    if (!d->capStages || count < 1 || count > RAZER_MAX_STAGES) return;

    int table[RAZER_MAX_STAGES];
    for (int i = 0; i < RAZER_MAX_STAGES; ++i)
        table[i] = d->stageDpi[i] ? d->stageDpi[i] : 800 * (i + 1);
    int active = d->activeStage > count ? count : d->activeStage;
    RazerSetStages(d, active, count, table);
    RefreshDpi(TRUE);
    PublishState();
}

// Warn once when the charge crosses the threshold, then only every half hour,
// and again whenever it has dropped another five points since the last warning.
static void CheckBatteryWarning(RazerDevice *d) {
    static DWORD lastWarn;
    static int   warnedAt = -1;

    if (!g_batteryWarnAt || !d->capBattery || d->battery < 0) return;
    if (d->charging || d->battery > g_batteryWarnAt) {
        warnedAt = -1;                       // charged up again: arm the warning
        return;
    }

    DWORD now = GetTickCount();
    BOOL first  = (warnedAt < 0);
    BOOL slipped = (warnedAt >= 0 && d->battery <= warnedAt - 5);
    if (!first && !slipped && now - lastWarn < BATTERY_REMIND_MS) return;

    lastWarn = now;
    warnedAt = d->battery;
    ++g_diagWarns;
    PostMessageW(g_hwnd, WM_APP_SHOW_BATT, (WPARAM)d->battery, 0);
}

static void HidActivate(int index) {
    if (index < 0 || index >= g_devCount || index == g_devActive) return;
    g_devActive = index;
    g_activeWatchDevice = index;
    g_haveBaseline = FALSE;

    RazerDevice *d = &g_dev[index];
    if (!d->bleOnly) {
        if (!d->probed) RazerProbeCaps(d);
        if (g_forceMode >= 0 && d->probed && d->deviceMode != g_forceMode) {
            RazerSetDeviceMode(d, (BYTE)g_forceMode);
            d->deviceMode = RazerGetDeviceMode(d);
        }
        RefreshDpi(FALSE);
    }
    PublishState();
}

// Which link is the mouse using right now? A receiver answers its own commands
// only while the mouse is actually paired to it - with the mouse switched to
// Bluetooth the dongle times out on everything. That makes one command a
// definitive test, so no guessing from device presence is needed.
static void HidCheckLink(void) {
    static DWORD usbSilentSince;
    if (g_devCount < 2) return;

    for (int i = 0; i < g_devCount; ++i) {
        if (g_dev[i].bleOnly) continue;
        RazerReport r;
        if (RazerCommand(&g_dev[i], 0x00, 0x81, 0x02, NULL, 0, &r) != RAZER_STATUS_OK) continue;
        g_dev[i].awake = TRUE;
        usbSilentSince = 0;
        HidActivate(i);
        return;
    }

    // No receiver answered. That is either a sleeping mouse on the dongle or a
    // mouse that moved to Bluetooth, and the two look identical, so give the
    // dongle a few minutes before handing over.
    if (g_devActive < 0 || g_dev[g_devActive].bleOnly) return;
    DWORD now = GetTickCount();
    if (!usbSilentSince) { usbSilentSince = now; return; }
    if (now - usbSilentSince < 180000) return;

    for (int i = 0; i < g_devCount; ++i)
        if (g_dev[i].bleOnly) { HidActivate(i); return; }
}

// A tick is a chance to poll, not an obligation: when the machine has been idle
// the mouse is left alone so it can go to sleep.
static void HidTick(void) {
    static DWORD lastBattery, lastDpiPoll, lastScan, lastLink;

    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    DWORD idleMs = GetLastInputInfo(&lii) ? (GetTickCount() - lii.dwTime) : 0;
    ++g_diagTicks;
    g_diagIdleMs = (LONG)idleMs;
    if (idleMs > 60000) return;

    DWORD now = GetTickCount();

    // Nothing found yet - a receiver can appear late, and a mouse that was
    // asleep during the first scan is worth another look.
    if (g_devActive < 0) {
        if (now - lastScan >= 15000) {
            lastScan = now;
            HidRescan();
        }
        return;
    }
    if (now - lastLink >= 10000) {
        lastLink = now;
        HidCheckLink();
    }

    RazerDevice *active = &g_dev[g_devActive];
    if (!active->bleOnly && now - lastDpiPoll >= 2000) {
        lastDpiPoll = now;
        RefreshDpi(TRUE);
        PublishState();
    }
    if (now - lastBattery >= 60000) {
        lastBattery = now;
        ++g_diagBatteryPolls;
        RazerDevice *d = &g_dev[g_devActive];
        BOOL fresh;
        if (d->bleOnly) {
            // Windows keeps this up to date from the standard GATT battery
            // service, so over Bluetooth nothing has to be asked of the mouse.
            fresh = RzBleReadBattery(d->bleInstance, &d->battery);
            d->capBattery = d->capBattery || fresh;
        } else {
            fresh = d->capBattery && RazerGetBattery(d);
            if (d->capCharging) RazerGetCharging(d);
        }
        PublishState();
        // Only ever warn on a reading that just came back: a sleeping mouse
        // keeps its last known percentage, and it is not draining anyway.
        if (fresh) CheckBatteryWarning(d);
    }
}

static DWORD WINAPI HidThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);   // force a queue
    HidRescan();
    UINT_PTR timer = SetTimer(NULL, 0, 1000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        switch (msg.message) {
        case HIDM_QUIT:
            goto done;
        case HIDM_RESCAN:
            HidRescan();
            break;
        case HIDM_REFRESH:
            RefreshDpi(TRUE);
            PublishState();
            break;
        case HIDM_SET_STAGE:
            HidSetStage((int)msg.wParam);
            break;
        case HIDM_STEP_STAGE:
            HidStepStage((int)msg.wParam);
            break;
        case HIDM_SET_POLL:
            if (g_devActive >= 0) {
                RazerSetPollRate(&g_dev[g_devActive], (int)msg.wParam);
                RazerGetPollRate(&g_dev[g_devActive]);
                PublishState();
            }
            break;
        case HIDM_SET_IDLE:
            if (g_devActive >= 0) {
                RazerSetIdleTime(&g_dev[g_devActive], (int)msg.wParam);
                RazerGetIdleTime(&g_dev[g_devActive]);
                PublishState();
            }
            break;
        case HIDM_SET_SDPI:
            HidSetStageDpi((int)msg.wParam, (int)msg.lParam);
            break;
        case HIDM_SET_COUNT:
            HidSetStageCount((int)msg.wParam);
            break;
        case HIDM_SET_DEVICE:
            // No need to restart the watcher: it already listens to every link.
            HidActivate((int)msg.wParam);
            break;
        case HIDM_BLE_DPI:
            if (g_devActive >= 0 && g_dev[g_devActive].bleOnly) {
                RazerDevice *d = &g_dev[g_devActive];
                int dpi = (int)msg.wParam;
                if (dpi != d->dpiX) {
                    // Readable but not settable: capDpi stays off so the menu
                    // does not offer values the mouse cannot be told over BLE.
                    d->dpiX = d->dpiY = dpi;
                    PostMessageW(g_hwnd, WM_APP_SHOW_OSD, (WPARAM)dpi, 0);
                    PublishState();
                }
            }
            break;
        case HIDM_SET_MODE:
            if (g_devActive >= 0) {
                RazerSetDeviceMode(&g_dev[g_devActive], (BYTE)msg.wParam);
                g_dev[g_devActive].deviceMode = RazerGetDeviceMode(&g_dev[g_devActive]);
                PublishState();
            }
            break;
        case HIDM_SET_DPI:
            if (g_devActive >= 0 && g_dev[g_devActive].capDpi) {
                RazerSetDpi(&g_dev[g_devActive], (int)msg.wParam, (int)msg.wParam, TRUE);
                RefreshDpi(TRUE);
                PublishState();
            }
            break;
        case WM_TIMER:
            HidTick();
            break;
        }
    }
done:
    if (timer) KillTimer(NULL, timer);
    for (int i = 0; i < g_devCount; ++i) RazerCloseDevice(&g_dev[i]);
    return 0;
}

// ----------------------------------------------- vendor input report watcher
//
// Razer mice report side/DPI button activity on a vendor collection. Watching
// it means the overlay appears the moment a button is pressed instead of on the
// next poll. Which collection it is differs per model, so every readable one is
// watched at once.

typedef struct {
    HANDLE   handle;
    OVERLAPPED ov;
    BYTE     buffer[64];
    DWORD    length;
    int      device;            // index into g_dev
    BOOL     ble;
} WatchEntry;

static WatchEntry g_watch[12];
static int        g_watchCount;

// Snapshot of the device list for the watcher thread: which collection belongs
// to which link. Filled by the HID thread before the watcher starts.
typedef struct {
    WCHAR key[24];
    BOOL  ble;
} WatchTarget;

static WatchTarget g_watchTargets[RAZER_MAX_DEVICES];
static int         g_watchTargetCount;

static void CloseWatchEntries(void) {
    for (int i = 0; i < g_watchCount; ++i) {
        if (g_watch[i].handle != INVALID_HANDLE_VALUE) {
            CancelIo(g_watch[i].handle);
            CloseHandle(g_watch[i].handle);
        }
        if (g_watch[i].ov.hEvent) CloseHandle(g_watch[i].ov.hEvent);
    }
    g_watchCount = 0;
}

static DWORD WINAPI WatchThreadProc(LPVOID) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    union {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        BYTE raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 512 * sizeof(WCHAR)];
    } buffer;

    for (DWORD i = 0; g_watchCount < ARRAYSIZE(g_watch) &&
                      SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &iface); ++i) {
        buffer.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &iface, &buffer.detail,
                                              sizeof(buffer), NULL, NULL))
            continue;

        // Both links are watched at once: whichever one delivers reports is the
        // one the mouse is actually using right now.
        int device = -1;
        for (int t = 0; t < g_watchTargetCount; ++t)
            if (PathContains(buffer.detail.DevicePath, g_watchTargets[t].key)) { device = t; break; }
        if (device < 0) continue;

        // Mouse and keyboard collections are held exclusively by the input
        // stack; those simply fail to open, which is the filter we want.
        HANDLE h = CreateFileW(buffer.detail.DevicePath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        USHORT inLen = 0;
        PHIDP_PREPARSED_DATA pp = NULL;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS)
                inLen = caps.InputReportByteLength;
            HidD_FreePreparsedData(pp);
        }
        if (inLen == 0 || inLen > sizeof(g_watch[0].buffer)) { CloseHandle(h); continue; }

        WatchEntry *e = &g_watch[g_watchCount];
        RzZero(e, sizeof(*e));
        e->handle = h;
        e->length = inLen;
        e->device = device;
        e->ble    = g_watchTargets[device].ble;
        e->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        ++g_watchCount;
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (g_watchCount == 0) return 0;

    HANDLE waits[ARRAYSIZE(g_watch) + 1];
    for (int i = 0; i < g_watchCount; ++i) {
        DWORD read = 0;
        ReadFile(g_watch[i].handle, g_watch[i].buffer, g_watch[i].length, &read, &g_watch[i].ov);
        waits[i] = g_watch[i].ov.hEvent;
    }
    waits[g_watchCount] = g_watchStop;

    DWORD lastPost = 0, pendingCode = 0, pendingTick = 0;
    BOOL  pendingBle = FALSE;
    for (;;) {
        DWORD r = WaitForMultipleObjects((DWORD)g_watchCount + 1, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + (DWORD)g_watchCount || r == WAIT_FAILED) break;

        int index = (int)(r - WAIT_OBJECT_0);
        DWORD transferred = 0;
        GetOverlappedResult(g_watch[index].handle, &g_watch[index].ov, &transferred, FALSE);
        ResetEvent(g_watch[index].ov.hEvent);

        // Reports prove which link is live - the other one stays silent.
        if (g_watch[index].device != g_activeWatchDevice) {
            g_activeWatchDevice = g_watch[index].device;
            PostThreadMessageW(g_hidThreadId, HIDM_SET_DEVICE, g_watch[index].device, 0);
        }

        // Over Bluetooth there is no way to ask for the DPI, but the mouse
        // announces it on its own: 05 05 02 <x hi> <x lo> <y hi> <y lo>.
        if (g_watch[index].ble && transferred >= 7 &&
            g_watch[index].buffer[0] == 0x05 && g_watch[index].buffer[1] == 0x05 &&
            g_watch[index].buffer[2] == 0x02) {
            int dpi = ((int)g_watch[index].buffer[3] << 8) | g_watch[index].buffer[4];
            if (dpi >= 100 && dpi <= 32000)
                PostThreadMessageW(g_hidThreadId, HIDM_BLE_DPI, (WPARAM)dpi, 0);
        }

        // A press sets one byte of the report, the matching release clears it
        // again. The first non-zero byte identifies the button.
        DWORD code = 0;
        for (DWORD i = 1; i < transferred && i < sizeof(g_watch[0].buffer); ++i) {
            if (g_watch[index].buffer[i]) {
                code = ((DWORD)g_watch[index].buffer[0] << 16) | (i << 8) |
                       g_watch[index].buffer[i];
                break;
            }
        }

        // Vendor collections also carry status notifications - a receiver
        // reporting that the mouse fell asleep looks exactly like a button
        // press. What tells them apart is the release: a button always clears
        // its byte again a moment later, a notification never does. So an
        // unknown code is only accepted as a button once its release arrives.
        DWORD now = GetTickCount();
        BYTE reportId = g_watch[index].buffer[0];
        BOOL fromBle = g_watch[index].ble;
        if (code) {
            int action = LookupVendorAction(code, fromBle);
            if (action >= 0) {
                if (action != ACT_DEFAULT) PerformAction(action);
            } else {
                pendingCode = code;
                pendingTick = now;
                pendingBle  = fromBle;
            }
        } else if (pendingCode && reportId == (BYTE)(pendingCode >> 16) &&
                   now - pendingTick < 1500) {
            BOOL isNew = FALSE;
            int action = RegisterVendorPress(pendingCode, pendingBle, &isNew);
            pendingCode = 0;
            if (isNew) {
                SaveVendorButtons();
                PostMessageW(g_hwnd, WM_APP_STATE, 0, 0);   // the menu has a new entry
            }
            if (action != ACT_DEFAULT) PerformAction(action);
        }

        // Debounced: a press and its release are one event as far as reading
        // the DPI back is concerned.
        if (now - lastPost > 120) {
            lastPost = now;
            PostThreadMessageW(g_hidThreadId, HIDM_REFRESH, 1, 0);
        }

        DWORD read = 0;
        if (!ReadFile(g_watch[index].handle, g_watch[index].buffer, g_watch[index].length,
                      &read, &g_watch[index].ov) && GetLastError() != ERROR_IO_PENDING)
            break;
    }

    CloseWatchEntries();
    return 0;
}

static void StartWatcher(void) {
    // If a previous watcher outstayed its welcome in StopWatcher, it is still
    // using g_watch[]. Starting a second one would have two threads closing the
    // same handles, so wait it out instead.
    if (g_watchThread && WaitForSingleObject(g_watchThread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_watchThread);
        g_watchThread = NULL;
    }
    if (g_watchThread || g_devCount == 0) return;

    g_watchTargetCount = 0;
    for (int i = 0; i < g_devCount && i < RAZER_MAX_DEVICES; ++i) {
        lstrcpynW(g_watchTargets[i].key, g_dev[i].matchKey, ARRAYSIZE(g_watchTargets[i].key));
        g_watchTargets[i].ble = g_dev[i].bleOnly;
        ++g_watchTargetCount;
    }
    g_activeWatchDevice = g_devActive;

    ResetEvent(g_watchStop);
    g_watchThread = CreateThread(NULL, 0, WatchThreadProc, NULL, 0, NULL);
}

static void StopWatcher(void) {
    if (!g_watchThread) return;
    SetEvent(g_watchStop);
    // Device enumeration inside the watcher can take a while when a Bluetooth
    // device is misbehaving; if it has not finished, keep the handle and let
    // StartWatcher pick the wait up again rather than abandoning the thread.
    if (WaitForSingleObject(g_watchThread, 5000) != WAIT_OBJECT_0) return;
    CloseHandle(g_watchThread);
    g_watchThread = NULL;
}

// --------------------------------------------------------------- tray visuals

enum IconTone {
    TONE_NORMAL,   // the artwork as authored
    TONE_MUTED,    // grey and translucent: no mouse found
    TONE_ALERT     // recoloured red: the battery is nearly flat
};

// Recoloured copy of the application icon, so the tray shows one piece of
// artwork in every state instead of a different picture per state. Returns NULL
// for icons without an alpha channel; callers fall back to the original.
static HICON MakeIconVariant(HICON source, IconTone tone) {
    ICONINFO info;
    if (!source || tone == TONE_NORMAL || !GetIconInfo(source, &info)) return NULL;

    HICON result = NULL;
    BITMAP bm;
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bm), &bm) &&
        bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int width = bm.bmWidth, height = bm.bmHeight;

        BITMAPINFO bi;
        RzZero(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = width;
        bi.bmiHeader.biHeight      = -height;      // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(NULL);
        void *bits = NULL;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dib && bits &&
            GetDIBits(screen, info.hbmColor, 0, (UINT)height, bits, &bi, DIB_RGB_COLORS)) {
            DWORD *pixels = (DWORD *)bits;
            const int count = width * height;

            BOOL hasAlpha = FALSE;
            for (int i = 0; i < count; ++i)
                if (pixels[i] & 0xFF000000u) { hasAlpha = TRUE; break; }

            if (hasAlpha) {
                for (int i = 0; i < count; ++i) {
                    DWORD p = pixels[i];
                    int a = (int)((p >> 24) & 0xFF);
                    if (!a) { pixels[i] = 0; continue; }
                    int r = (int)((p >> 16) & 0xFF);
                    int g = (int)((p >> 8) & 0xFF);
                    int b = (int)(p & 0xFF);
                    int v = (r * 77 + g * 151 + b * 28) >> 8;      // luminance
                    if (tone == TONE_MUTED) {
                        v += (0x96 - v) * 60 / 100;                // towards mid grey
                        r = g = b = v;
                        a = a * 75 / 100;
                    } else {
                        // keep the shading, swap the hue for a warning red
                        r = v * 235 / 255 + 20;
                        g = v * 70 / 255;
                        b = v * 70 / 255;
                        if (r > 255) r = 255;
                    }
                    pixels[i] = ((UINT)a << 24) | ((UINT)r << 16) | ((UINT)g << 8) | (UINT)b;
                }
                ICONINFO variant;
                RzZero(&variant, sizeof(variant));
                variant.fIcon    = TRUE;
                variant.hbmColor = dib;
                variant.hbmMask  = info.hbmMask;
                result = CreateIconIndirect(&variant);   // copies both bitmaps
            }
        }
        if (dib) DeleteObject(dib);
        ReleaseDC(NULL, screen);
    }

    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask)  DeleteObject(info.hbmMask);
    return result;
}

static void LoadTrayIcons(void) {
    if (g_iconBase) return;
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    if (cx < 8) cx = 16;
    if (cy < 8) cy = 16;

    g_iconBase = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   cx, cy, LR_DEFAULTCOLOR);
    if (!g_iconBase) g_iconBase = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);

    HICON muted = MakeIconVariant(g_iconBase, TONE_MUTED);
    HICON alert = MakeIconVariant(g_iconBase, TONE_ALERT);
    g_iconMuted = muted ? muted : g_iconBase;
    g_iconAlert = alert ? alert : g_iconBase;
}

static void UpdateTray(BOOL add) {
    UiState s;
    EnterCriticalSection(&g_lock);
    s = g_ui;
    LeaveCriticalSection(&g_lock);

    // One piece of artwork in three tones: normal, greyed when no mouse is
    // around, red once the battery drops past the warning threshold.
    LoadTrayIcons();
    g_trayIcon = g_iconBase;
    if (!s.present)
        g_trayIcon = g_iconMuted;
    else if (g_batteryWarnAt && s.capBattery && s.battery >= 0 &&
             s.battery <= g_batteryWarnAt && !s.charging)
        g_trayIcon = g_iconAlert;

    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_trayIcon;

    if (!s.present) {
        lstrcpyW(g_nid.szTip, L"RazerLite - no Razer mouse found");
    } else {
        WCHAR battery[40], rate[24], dpi[24], tip[256];
        battery[0] = rate[0] = dpi[0] = 0;
        if (s.capBattery && s.battery >= 0)
            wsprintfW(battery, L"\nBattery %d%%%s", s.battery,
                      s.charging ? L" (charging)" : L"");
        if (s.capPoll && s.pollRate)
            wsprintfW(rate, L" \x00B7 %d Hz", s.pollRate);
        if (s.dpi > 0) wsprintfW(dpi, L"\n%d DPI", s.dpi);
        wsprintfW(tip, L"%.90s%s%s%s%s", s.name, dpi, rate, battery,
                  s.awake ? L"" : L"\nSleeping");
        lstrcpynW(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip));
    }

    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
    if (add) {
        g_nid.uVersion = NOTIFYICON_VERSION;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }
}

// ---------------------------------------------------------------- DPI overlay

static int ScreenDpi(void) {
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return dpi ? dpi : 96;
}

static HFONT OsdFont(int points, int weight) {
    return CreateFontW(MulDiv(points, ScreenDpi(), 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void OsdText(HDC dc, const WCHAR *text, RECT rc, int points, int weight, COLORREF color) {
    HFONT font = OsdFont(points, weight);
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(font);
}

static void PaintDpiOsd(HDC dc, RECT rc, int dpi) {
    int w = rc.right, h = rc.bottom;

    WCHAR text[16];
    wsprintfW(text, L"%d", g_osdDpi);
    RECT tr = rc;
    tr.bottom = h - MulDiv(22, dpi, 96);
    OsdText(dc, text, tr, 40, FW_SEMIBOLD, RGB(245, 245, 245));

    RECT lr = rc;
    lr.top    = MulDiv(8, dpi, 96);
    lr.bottom = lr.top + MulDiv(16, dpi, 96);
    OsdText(dc, L"DPI", lr, 12, FW_NORMAL, RGB(150, 150, 155));

    if (g_osdStageCount > 1) {
        int dot = MulDiv(7, dpi, 96);
        int gap = MulDiv(6, dpi, 96);
        int total = g_osdStageCount * dot + (g_osdStageCount - 1) * gap;
        int x = (w - total) / 2;
        int y = h - MulDiv(16, dpi, 96);
        for (int i = 0; i < g_osdStageCount; ++i) {
            BOOL on = (i + 1 == g_osdStage);
            HBRUSH brush = CreateSolidBrush(on ? RGB(120, 210, 120) : RGB(80, 80, 84));
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, x, y, x + dot + 1, y + dot + 1);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(brush);
            x += dot + gap;
        }
    }
}

static void PaintBatteryOsd(HDC dc, RECT rc, int dpi) {
    int w = rc.right, h = rc.bottom;
    COLORREF accent = (g_osdBattery <= 10) ? RGB(235, 80, 80) : RGB(240, 175, 60);

    RECT lr = rc;
    lr.top    = MulDiv(12, dpi, 96);
    lr.bottom = lr.top + MulDiv(18, dpi, 96);
    OsdText(dc, L"MOUSE BATTERY LOW", lr, 12, FW_SEMIBOLD, accent);

    WCHAR text[16];
    wsprintfW(text, L"%d%%", g_osdBattery);
    RECT tr = rc;
    tr.top    = MulDiv(24, dpi, 96);
    tr.bottom = h - MulDiv(30, dpi, 96);
    OsdText(dc, text, tr, 36, FW_SEMIBOLD, RGB(245, 245, 245));

    // a battery gauge: outline, terminal, and a fill proportional to the charge
    int bw = MulDiv(96, dpi, 96), bh = MulDiv(14, dpi, 96);
    int bx = (w - bw) / 2, by = h - MulDiv(26, dpi, 96);
    HPEN pen = CreatePen(PS_SOLID, MulDiv(1, dpi, 96) + 1, RGB(120, 120, 126));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bx, by, bx + bw, by + bh);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    int tipW = MulDiv(3, dpi, 96) + 1;
    RECT tip;
    tip.left = bx + bw; tip.right = bx + bw + tipW;
    tip.top = by + bh / 4; tip.bottom = by + bh - bh / 4;
    HBRUSH grey = CreateSolidBrush(RGB(120, 120, 126));
    FillRect(dc, &tip, grey);
    DeleteObject(grey);

    int inset = MulDiv(3, dpi, 96);
    int fill = (bw - 2 * inset) * (g_osdBattery < 0 ? 0 : g_osdBattery) / 100;
    if (fill > 0) {
        RECT fr;
        fr.left = bx + inset; fr.right = bx + inset + fill;
        fr.top = by + inset;  fr.bottom = by + bh - inset;
        HBRUSH brush = CreateSolidBrush(accent);
        FillRect(dc, &fr, brush);
        DeleteObject(brush);
    }
}

static void PaintOsd(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int dpi = ScreenDpi();

    HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30));   // always dark, like the volume flyout
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    if (g_osdKind == OSD_BATTERY) PaintBatteryOsd(dc, rc, dpi);
    else                          PaintDpiOsd(dc, rc, dpi);
}

static LRESULT CALLBACK OsdProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintOsd(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == TIMER_OSD) {
            if (g_osdStep < 0) {                    // holding at full opacity
                g_osdStep = 0;
                SetTimer(hwnd, TIMER_OSD, OSD_FADE_STEP_MS, NULL);
            } else if (++g_osdStep >= OSD_FADE_STEPS) {
                KillTimer(hwnd, TIMER_OSD);
                ShowWindow(hwnd, SW_HIDE);
            } else {
                int alpha = 235 - 235 * g_osdStep / OSD_FADE_STEPS;
                SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowOsdWindow(int kind) {
    if (!g_osd) return;
    g_osdKind = kind;

    int dpiScale = ScreenDpi();
    int w = MulDiv(kind == OSD_BATTERY ? 230 : 170, dpiScale, 96);
    int h = MulDiv(kind == OSD_BATTERY ? 130 : 96, dpiScale, 96);

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = (mi.rcWork.left + mi.rcWork.right) / 2 - w / 2;
    int y = mi.rcWork.bottom - h - MulDiv(90, dpiScale, 96);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, MulDiv(14, dpiScale, 96),
                                  MulDiv(14, dpiScale, 96));
    SetWindowRgn(g_osd, rgn, TRUE);          // the window owns the region now

    SetWindowPos(g_osd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetLayeredWindowAttributes(g_osd, 0, 235, LWA_ALPHA);
    InvalidateRect(g_osd, NULL, TRUE);

    g_osdStep = -1;                           // hold, then fade
    SetTimer(g_osd, TIMER_OSD, kind == OSD_BATTERY ? OSD_HOLD_LONG_MS : OSD_HOLD_MS, NULL);
}

static void ShowDpiOsd(int dpi, int stage, int stageCount) {
    if (!g_osdEnabled) return;
    g_osdDpi        = dpi;
    g_osdStage      = stage;
    g_osdStageCount = stageCount;
    ShowOsdWindow(OSD_DPI);
}

// Shown regardless of the DPI overlay setting: a flat battery is worth an
// interruption, and it has its own on/off switch in the menu.
static void ShowBatteryOsd(int percent) {
    g_osdBattery = percent;
    ShowOsdWindow(OSD_BATTERY);
}

// ----------------------------------------------------------------- tray menu

static BOOL IsSynapseRunning(void) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\RazerAppEngineMutex");
    if (mutex) { CloseHandle(mutex); return TRUE; }
    HWND wnd = FindWindowW(NULL, L"Razer Synapse");
    if (wnd) return TRUE;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return FALSE;
    SC_HANDLE svc = OpenServiceW(scm, L"Razer Elevation Service", SERVICE_QUERY_STATUS);
    BOOL running = FALSE;
    if (svc) {
        SERVICE_STATUS status;
        if (QueryServiceStatus(svc, &status))
            running = (status.dwCurrentState == SERVICE_RUNNING);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

static HMENU BuildActionMenu(int current, int idBase) {
    HMENU menu = CreatePopupMenu();
    for (int a = 0; a < ACT_COUNT; ++a) {
        if (a == ACT_DISABLED || a == ACT_DPI_NEXT || a == ACT_COPY ||
            a == ACT_ALTTAB || a == ACT_PLAYPAUSE)
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (current == a ? MF_CHECKED : 0),
                    idBase + a, ACTION_NAMES[a]);
    }
    return menu;
}

static HMENU BuildButtonsMenu(BOOL onBluetooth) {
    HMENU menu = CreatePopupMenu();
    WCHAR line[128];

    for (int b = 0; b < BTN_COUNT; ++b) {
        HMENU sub = BuildActionMenu((int)g_action[b], IDM_ACTION_BASE + b * 100);
        wsprintfW(line, L"%s\t%s", BUTTON_NAMES[b],
                  g_action[b] == ACT_DEFAULT ? L"" : ACTION_NAMES[g_action[b]]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, line);
    }

    // Vendor buttons belong to the link they were learned on: the dongle's DPI
    // codes simply do not exist over Bluetooth, where the mouse cycles DPI on
    // its own and tells nobody in a way that could be intercepted.
    VendorButton local[MAX_VENDOR_BUTTONS];
    int index[MAX_VENDOR_BUTTONS];
    int count = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_vendorCount; ++i)
        if (g_vendor[i].ble == onBluetooth) { local[count] = g_vendor[i]; index[count] = i; ++count; }
    LeaveCriticalSection(&g_lock);

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (count == 0) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    onBluetooth ? L"Over Bluetooth the mouse handles its DPI button itself"
                                : L"Press DPI or profile buttons to detect them");
    } else {
        for (int i = 0; i < count; ++i) {
            HMENU sub = BuildActionMenu(local[i].action, IDM_VACTION_BASE + index[i] * 100);
            wsprintfW(line, L"%s (%02X)\t%s",
                      local[i].movedDpi ? L"DPI button" : L"Mouse button",
                      local[i].code & 0xFF,
                      local[i].action == ACT_DEFAULT ? L"" : ACTION_NAMES[local[i].action]);
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, line);
        }
    }

    // The mouse only forwards its DPI / profile buttons to a driver when it is
    // put in driver mode; left alone it acts on them itself and nothing can be
    // remapped. That is the whole meaning of the old "device mode" setting, so
    // it is worded as what it does rather than as a protocol detail.
    if (!onBluetooth) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (g_forceMode == 0x03 ? MF_CHECKED : 0), IDM_TAKEOVER,
                    L"Let RazerLite handle the DPI button");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_BTN_RESET, L"Reset all to default");
    if (count > 0)
        AppendMenuW(menu, MF_STRING, IDM_BTN_FORGET, L"Forget detected buttons");
    return menu;
}

static void ShowTrayMenu(HWND hwnd) {
    UiState s;
    EnterCriticalSection(&g_lock);
    s = g_ui;
    LeaveCriticalSection(&g_lock);

    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    WCHAR line[160];
    if (s.present) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, s.name);
        WCHAR extra[64], rate[24], dpi[24];
        extra[0] = rate[0] = dpi[0] = 0;
        if (s.capBattery && s.battery >= 0)
            wsprintfW(extra, L"battery %d%%%s", s.battery,
                      s.charging ? L" (charging)" : L"");
        if (s.capPoll && s.pollRate)
            wsprintfW(rate, L"%d Hz \x00B7 ", s.pollRate);
        if (s.dpi > 0) wsprintfW(dpi, L"%d DPI \x00B7 ", s.dpi);
        wsprintfW(line, L"%s%s%s", dpi, rate, extra);
        if (line[0]) AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, line);
    } else {
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"No Razer mouse found");
    }
    if (IsSynapseRunning())
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    L"! Synapse is running - settings may clash");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    if (s.present && s.capStages && s.stageCount > 0) {
        HMENU dpiMenu = CreatePopupMenu();
        for (int i = 0; i < s.stageCount; ++i) {
            wsprintfW(line, L"%d DPI", s.stageDpi[i]);
            AppendMenuW(dpiMenu, MF_STRING | (s.activeStage == i + 1 ? MF_CHECKED : 0),
                        IDM_DPI_STAGE_BASE + i, line);
        }
        AppendMenuW(dpiMenu, MF_SEPARATOR, 0, NULL);

        HMENU editMenu = CreatePopupMenu();
        for (int i = 0; i < s.stageCount; ++i) {
            HMENU valueMenu = CreatePopupMenu();
            for (int v = 0; v < ARRAYSIZE(DPI_LADDER); ++v) {
                if (DPI_LADDER[v] > s.maxDpi) break;
                wsprintfW(line, L"%d", DPI_LADDER[v]);
                AppendMenuW(valueMenu, MF_STRING |
                            (s.stageDpi[i] == DPI_LADDER[v] ? MF_CHECKED : 0),
                            IDM_STAGEVAL_BASE + i * 100 + v, line);
            }
            wsprintfW(line, L"Stage %d (%d DPI)", i + 1, s.stageDpi[i]);
            AppendMenuW(editMenu, MF_POPUP, (UINT_PTR)valueMenu, line);
        }
        AppendMenuW(editMenu, MF_SEPARATOR, 0, NULL);
        HMENU countMenu = CreatePopupMenu();
        for (int c = 1; c <= RAZER_MAX_STAGES; ++c) {
            wsprintfW(line, L"%d", c);
            AppendMenuW(countMenu, MF_STRING | (s.stageCount == c ? MF_CHECKED : 0),
                        IDM_COUNT_BASE + c, line);
        }
        AppendMenuW(editMenu, MF_POPUP, (UINT_PTR)countMenu, L"Number of stages");
        AppendMenuW(dpiMenu, MF_POPUP, (UINT_PTR)editMenu, L"Edit stages");

        AppendMenuW(menu, MF_POPUP, (UINT_PTR)dpiMenu, L"DPI");
    } else if (s.present && s.capDpi) {
        // No onboard stage table on this model: set the sensor directly.
        HMENU dpiMenu = CreatePopupMenu();
        for (int v = 0; v < ARRAYSIZE(DPI_LADDER); ++v) {
            if (DPI_LADDER[v] > s.maxDpi) break;
            wsprintfW(line, L"%d", DPI_LADDER[v]);
            AppendMenuW(dpiMenu, MF_STRING | (s.dpi == DPI_LADDER[v] ? MF_CHECKED : 0),
                        IDM_DPI_VALUE_BASE + v, line);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)dpiMenu, L"DPI");
    }

    if (s.present && s.capPoll) {
        HMENU pollMenu = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(POLL_RATES); ++i) {
            wsprintfW(line, L"%d Hz", POLL_RATES[i]);
            AppendMenuW(pollMenu, MF_STRING | (s.pollRate == POLL_RATES[i] ? MF_CHECKED : 0),
                        IDM_POLL_BASE + i, line);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)pollMenu, L"Polling rate");
    }

    if (s.present && s.capIdle) {
        HMENU idleMenu = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(IDLE_MINUTES); ++i) {
            wsprintfW(line, L"%d min", IDLE_MINUTES[i]);
            AppendMenuW(idleMenu, MF_STRING |
                        (s.idleSeconds == IDLE_MINUTES[i] * 60 ? MF_CHECKED : 0),
                        IDM_IDLE_BASE + i, line);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)idleMenu, L"Sleep after");
    }

    AppendMenuW(menu, MF_POPUP, (UINT_PTR)BuildButtonsMenu(s.present && !s.vendorProtocol),
                L"Buttons");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_osdEnabled ? MF_CHECKED : 0), IDM_OSD,
                L"Show DPI overlay");

    if (s.capBattery) {
        HMENU battMenu = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(BATTERY_LEVELS); ++i) {
            if (BATTERY_LEVELS[i]) wsprintfW(line, L"Below %d%%", BATTERY_LEVELS[i]);
            else                   lstrcpyW(line, L"Off");
            AppendMenuW(battMenu, MF_STRING |
                        (g_batteryWarnAt == BATTERY_LEVELS[i] ? MF_CHECKED : 0),
                        IDM_BATT_BASE + i, line);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)battMenu, L"Low battery warning");
    }

    if (s.deviceCount > 1) {
        HMENU devMenu = CreatePopupMenu();
        for (int i = 0; i < s.deviceCount; ++i)
            AppendMenuW(devMenu, MF_STRING | (s.activeIndex == i ? MF_CHECKED : 0),
                        IDM_DEVICE_BASE + i, s.deviceNames[i]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)devMenu, L"Device");
    }

    if (s.present && !s.vendorProtocol)
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    L"Over Bluetooth: battery and buttons only");

    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), IDM_STARTUP,
                L"Run at startup");

    HMENU updateMenu = CreatePopupMenu();
    AppendMenuW(updateMenu, MF_STRING, IDM_CHECK_UPDATE, L"Check now");
    AppendMenuW(updateMenu, MF_STRING | (g_autoUpdate ? MF_CHECKED : 0), IDM_AUTO_UPDATE,
                L"Install new versions automatically");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)updateMenu, L"Updates\t" APP_VERSION_STR);

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// --------------------------------------------------------------- updating
//
// The check runs on its own thread and reports back with a single message, so
// neither the message loop nor the HID loop ever waits on the network.

#define UPD_CURRENT    0        // already the newest release
#define UPD_INSTALLED  1        // replacement started, this process should go
#define UPD_AVAILABLE  2        // newer build exists, left for the user to start
#define UPD_FAILED     3        // could not ask GitHub
#define UPD_BROKEN     4        // found one, but could not install it

static WCHAR g_updateVersion[32];

static DWORD WINAPI UpdateThreadProc(LPVOID param) {
    BOOL silent = (BOOL)(UINT_PTR)param;
    UpdateInfo info;
    int result;

    if (!UpdateQuery(APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, &info)) {
        result = UPD_FAILED;
    } else {
        lstrcpynW(g_updateVersion, info.version, ARRAYSIZE(g_updateVersion));
        if (!info.available) {
            result = UPD_CURRENT;
        } else if (silent && !g_autoUpdate) {
            result = UPD_AVAILABLE;
        } else {
            WCHAR staged[MAX_PATH];
            DWORD len = GetTempPathW(MAX_PATH, staged);
            if (len && len < MAX_PATH - 24) {
                lstrcatW(staged, L"RazerLite-update.exe");
                result = (UpdateDownload(info.assetUrl, staged) && UpdateApply(staged))
                       ? UPD_INSTALLED : UPD_BROKEN;
            } else {
                result = UPD_BROKEN;
            }
        }
    }

    PostMessageW(g_hwnd, WM_APP_UPDATE, (WPARAM)result, (LPARAM)silent);
    InterlockedExchange(&g_updateBusy, 0);
    return 0;
}

static void StartUpdateCheck(BOOL silent) {
    if (InterlockedCompareExchange(&g_updateBusy, 1, 0) != 0) return;
    HANDLE thread = CreateThread(NULL, 0, UpdateThreadProc,
                                 (LPVOID)(UINT_PTR)silent, 0, NULL);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&g_updateBusy, 0);
}

static void TrayBalloon(const WCHAR *title, const WCHAR *text) {
    NOTIFYICONDATAW balloon;
    RzZero(&balloon, sizeof(balloon));
    balloon.cbSize = sizeof(balloon);
    balloon.hWnd   = g_nid.hWnd;
    balloon.uID    = g_nid.uID;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_NONE;
    lstrcpynW(balloon.szInfoTitle, title, ARRAYSIZE(balloon.szInfoTitle));
    lstrcpynW(balloon.szInfo, text, ARRAYSIZE(balloon.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

// --------------------------------------------------------- crash reporting
//
// A tray utility is meant to run for weeks, and when it does disappear there is
// nothing to look at: no console, and Windows Error Reporting keeps its records
// only briefly. So write our own note - the module offset in it is enough to
// find the faulting line in RazerLite.map.

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *info) {
    static char text[1024];
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;

    #define CRASH_PUT(s) { const char *p = (s); while (*p && n < 1000) text[n++] = *p++; }
    #define CRASH_HEX(v) { unsigned __int64 x = (unsigned __int64)(v); \
                           CRASH_PUT("0x"); \
                           BOOL lead = FALSE; \
                           for (int shift = 60; shift >= 0; shift -= 4) { \
                               int digit = (int)((x >> shift) & 0xF); \
                               if (digit || lead || shift == 0) { lead = TRUE; \
                                   if (n < 1000) text[n++] = hex[digit]; } } }
    #define CRASH_DEC(v) { unsigned int x = (unsigned int)(v); char tmp[12]; int t = 0; \
                           do { tmp[t++] = (char)('0' + x % 10); x /= 10; } while (x); \
                           while (t && n < 1000) text[n++] = tmp[--t]; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    CRASH_PUT("RazerLite crashed\r\ntime   : ");
    CRASH_DEC(st.wYear); CRASH_PUT("-"); CRASH_DEC(st.wMonth); CRASH_PUT("-");
    CRASH_DEC(st.wDay);  CRASH_PUT(" ");  CRASH_DEC(st.wHour); CRASH_PUT(":");
    CRASH_DEC(st.wMinute); CRASH_PUT(":"); CRASH_DEC(st.wSecond);

    ULONG_PTR base = (ULONG_PTR)GetModuleHandleW(NULL);
    ULONG_PTR at   = info && info->ExceptionRecord
                   ? (ULONG_PTR)info->ExceptionRecord->ExceptionAddress : 0;

    CRASH_PUT("\r\ncode   : ");
    CRASH_HEX(info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0);
    CRASH_PUT("\r\naddress: "); CRASH_HEX(at);
    CRASH_PUT("\r\nmodule : "); CRASH_HEX(base);
    CRASH_PUT("\r\noffset : "); CRASH_HEX(at > base ? at - base : 0);
    CRASH_PUT("   (look this up in RazerLite.map)");
    CRASH_PUT("\r\nthread : "); CRASH_DEC(GetCurrentThreadId());
    CRASH_PUT("\r\n");

    WCHAR path[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, path);
    if (len && len < MAX_PATH - 24) {
        lstrcatW(path, L"RazerLite-crash.txt");
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, text, (DWORD)n, &written, NULL);
            CloseHandle(file);
        }
    }

    #undef CRASH_PUT
    #undef CRASH_HEX
    #undef CRASH_DEC
    return EXCEPTION_EXECUTE_HANDLER;      // let the process end, note in hand
}

// ------------------------------------------------------------- status dump
//
// "RazerLite.exe --status" asks the running copy to write what it sees to
// %TEMP%\RazerLite-status.txt - the only way to inspect a tray-only program
// without a console.

static void WriteStatusFile(void) {
    UiState s;
    EnterCriticalSection(&g_lock);
    s = g_ui;
    LeaveCriticalSection(&g_lock);

    // static, not stack: a 4 KB frame would pull __chkstk in from the CRT.
    // Only the UI thread ever gets here.
    static WCHAR text[2048], line[256];
    text[0] = 0;
    wsprintfW(line, L"RazerLite  : %s  (updates %s)\r\n", APP_VERSION_STR,
              g_autoUpdate ? L"automatic" : L"manual");
    lstrcatW(text, line);

    if (!s.present) {
        lstrcatW(text, L"device: none found\r\n");
    } else {
        wsprintfW(line, L"device      : %s\r\n", s.name);            lstrcatW(text, line);
        wsprintfW(line, L"link        : %s\r\n",
                  s.bluetooth ? L"Bluetooth" : L"USB / HyperSpeed dongle");
        lstrcatW(text, line);
        wsprintfW(line, L"state       : %s\r\n",
                  s.awake ? L"awake" : L"asleep (last known values)"); lstrcatW(text, line);
        wsprintfW(line, L"firmware    : %d.%d\r\n", s.fwMajor, s.fwMinor); lstrcatW(text, line);
        wsprintfW(line, L"dpi         : %d\r\n", s.dpi);             lstrcatW(text, line);
        if (s.capStages) {
            wsprintfW(line, L"stages      : %d of %d active\r\n", s.activeStage, s.stageCount);
            lstrcatW(text, line);
            for (int i = 0; i < s.stageCount; ++i) {
                wsprintfW(line, L"  stage %d   : %d dpi%s\r\n", i + 1, s.stageDpi[i],
                          s.activeStage == i + 1 ? L"  <-" : L"");
                lstrcatW(text, line);
            }
        }
        if (s.capPoll)    { wsprintfW(line, L"polling     : %d Hz\r\n", s.pollRate); lstrcatW(text, line); }
        if (s.capBattery) { wsprintfW(line, L"battery     : %d%%%s\r\n", s.battery,
                                      s.charging ? L" charging" : L""); lstrcatW(text, line); }
        if (s.capIdle)    { wsprintfW(line, L"sleep after : %d s\r\n", s.idleSeconds); lstrcatW(text, line); }
        const WCHAR *kept = g_forceMode < 0 ? L"" :
                            g_forceMode == 0x00 ? L", kept onboard" : L", kept driver";
        if (s.deviceMode < 0) wsprintfW(line, L"device mode : unknown%s\r\n", kept);
        else wsprintfW(line, L"device mode : 0x%02X (%s)%s\r\n", s.deviceMode,
                       s.deviceMode == 0x00 ? L"onboard" : L"driver", kept);
        lstrcatW(text, line);
        wsprintfW(line, L"caps        : dpi=%d stages=%d poll=%d battery=%d charging=%d idle=%d\r\n",
                  s.capDpi, s.capStages, s.capPoll, s.capBattery, s.capCharging, s.capIdle);
        lstrcatW(text, line);
    }
    wsprintfW(line, L"devices     : %d\r\n", s.deviceCount);         lstrcatW(text, line);
    wsprintfW(line, L"overlay     : %s\r\n", g_osdEnabled ? L"on" : L"off"); lstrcatW(text, line);
    if (g_batteryWarnAt) wsprintfW(line, L"battery warn: below %d%%\r\n", g_batteryWarnAt);
    else                 lstrcpyW(line, L"battery warn: off\r\n");
    lstrcatW(text, line);

    for (int b = 0; b < BTN_COUNT; ++b) {
        wsprintfW(line, L"%-12s: %s\r\n", BUTTON_NAMES[b], ACTION_NAMES[g_action[b]]);
        lstrcatW(text, line);
    }

    EnterCriticalSection(&g_lock);
    int vendorCount = g_vendorCount;
    VendorButton vendor[MAX_VENDOR_BUTTONS];
    for (int i = 0; i < vendorCount; ++i) vendor[i] = g_vendor[i];
    LeaveCriticalSection(&g_lock);

    for (int i = 0; i < vendorCount; ++i) {
        wsprintfW(line, L"vendor %06X %-9s: %s%s\r\n", vendor[i].code,
                  vendor[i].ble ? L"[bluetooth]" : L"[dongle]",
                  ACTION_NAMES[vendor[i].action], vendor[i].movedDpi ? L"   (DPI button)" : L"");
        lstrcatW(text, line);
    }
    wsprintfW(line, L"hook        : %s\r\n", g_hookThread ? L"installed" : L"not needed");
    lstrcatW(text, line);
    wsprintfW(line, L"polling     : %d ticks, %d battery reads, %d warnings, idle %d ms\r\n",
              g_diagTicks, g_diagBatteryPolls, g_diagWarns, g_diagIdleMs);
    lstrcatW(text, line);

    WCHAR path[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, path);
    if (!len || len > MAX_PATH - 32) return;
    lstrcatW(path, L"RazerLite-status.txt");

    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    static char utf8[4096];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    DWORD written = 0;
    if (bytes > 1) WriteFile(file, utf8, (DWORD)(bytes - 1), &written, NULL);
    CloseHandle(file);
}

// ---------------------------------------------------------------- window proc

static void HandleCommand(HWND hwnd, int id) {
    if (id >= IDM_VACTION_BASE) {
        int index  = (id - IDM_VACTION_BASE) / 100;
        int action = (id - IDM_VACTION_BASE) % 100;
        if (index < MAX_VENDOR_BUTTONS && action < ACT_COUNT) SetVendorAction(index, action);
        return;
    }
    if (id >= IDM_ACTION_BASE) {
        int button = (id - IDM_ACTION_BASE) / 100;
        int action = (id - IDM_ACTION_BASE) % 100;
        if (button < BTN_COUNT && action < ACT_COUNT) SetAction(button, action);
        return;
    }
    if (id >= IDM_STAGEVAL_BASE) {
        int stage = (id - IDM_STAGEVAL_BASE) / 100;
        int value = (id - IDM_STAGEVAL_BASE) % 100;
        if (stage < RAZER_MAX_STAGES && value < ARRAYSIZE(DPI_LADDER))
            PostThreadMessageW(g_hidThreadId, HIDM_SET_SDPI, stage, DPI_LADDER[value]);
        return;
    }
    if (id >= IDM_COUNT_BASE && id < IDM_COUNT_BASE + 16) {
        PostThreadMessageW(g_hidThreadId, HIDM_SET_COUNT, id - IDM_COUNT_BASE, 0);
        return;
    }
    if (id >= IDM_DEVICE_BASE && id < IDM_DEVICE_BASE + RAZER_MAX_DEVICES) {
        PostThreadMessageW(g_hidThreadId, HIDM_SET_DEVICE, id - IDM_DEVICE_BASE, 0);
        return;
    }
    if (id >= IDM_IDLE_BASE && id < IDM_IDLE_BASE + ARRAYSIZE(IDLE_MINUTES)) {
        PostThreadMessageW(g_hidThreadId, HIDM_SET_IDLE,
                           IDLE_MINUTES[id - IDM_IDLE_BASE] * 60, 0);
        return;
    }
    if (id >= IDM_POLL_BASE && id < IDM_POLL_BASE + ARRAYSIZE(POLL_RATES)) {
        PostThreadMessageW(g_hidThreadId, HIDM_SET_POLL, POLL_RATES[id - IDM_POLL_BASE], 0);
        return;
    }
    if (id >= IDM_BATT_BASE && id < IDM_BATT_BASE + ARRAYSIZE(BATTERY_LEVELS)) {
        g_batteryWarnAt = BATTERY_LEVELS[id - IDM_BATT_BASE];
        RegWriteDword(L"BatteryWarn", (DWORD)g_batteryWarnAt);
        return;
    }
    if (id >= IDM_DPI_VALUE_BASE && id < IDM_DPI_VALUE_BASE + ARRAYSIZE(DPI_LADDER)) {
        PostThreadMessageW(g_hidThreadId, HIDM_SET_DPI, DPI_LADDER[id - IDM_DPI_VALUE_BASE], 0);
        return;
    }
    if (id >= IDM_DPI_STAGE_BASE && id < IDM_DPI_STAGE_BASE + RAZER_MAX_STAGES) {
        PostThreadMessageW(g_hidThreadId, HIDM_SET_STAGE, id - IDM_DPI_STAGE_BASE + 1, 0);
        return;
    }

    switch (id) {
    case IDM_EXIT:    DestroyWindow(hwnd); break;
    case IDM_REFRESH: PostThreadMessageW(g_hidThreadId, HIDM_RESCAN, 0, 0); break;
    case IDM_STARTUP: SetStartup(!IsStartupEnabled()); break;
    case IDM_OSD:
        g_osdEnabled = !g_osdEnabled;
        RegWriteDword(L"Osd", g_osdEnabled ? 1u : 0u);
        break;
    case IDM_BTN_RESET:
        for (int b = 0; b < BTN_COUNT; ++b) SetAction(b, ACT_DEFAULT);
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_vendorCount; ++i) g_vendor[i].action = ACT_DEFAULT;
        LeaveCriticalSection(&g_lock);
        SaveVendorButtons();
        break;
    case IDM_BTN_FORGET:
        EnterCriticalSection(&g_lock);
        g_vendorCount = 0;
        LeaveCriticalSection(&g_lock);
        SaveVendorButtons();
        break;
    // Taking over is remembered and re-applied on every reconnect, because the
    // mouse forgets the mode as soon as it loses power. Handing control back
    // puts it in onboard mode once and then leaves it alone.
    case IDM_TAKEOVER: {
        BOOL takeOver = (g_forceMode != 0x03);
        g_forceMode = takeOver ? 0x03 : -1;
        RegWriteDword(L"ForceMode", takeOver ? 0x03u : 0xFFFFFFFFu);
        PostThreadMessageW(g_hidThreadId, HIDM_SET_MODE, takeOver ? 0x03 : 0x00, 0);
        break;
    }
    case IDM_DPI_NEXT:     PostThreadMessageW(g_hidThreadId, HIDM_STEP_STAGE, 1, 0); break;
    case IDM_DPI_PREV:     PostThreadMessageW(g_hidThreadId, HIDM_STEP_STAGE, 0, 0); break;
    case IDM_DUMP:         WriteStatusFile(); break;
    case IDM_CHECK_UPDATE: StartUpdateCheck(FALSE); break;
    case IDM_AUTO_UPDATE:
        g_autoUpdate = !g_autoUpdate;
        RegWriteDword(L"AutoUpdate", g_autoUpdate ? 1u : 0u);
        if (g_autoUpdate) SetTimer(hwnd, TIMER_UPDATE, UPDATE_FIRST_MS, NULL);
        else              KillTimer(hwnd, TIMER_UPDATE);
        break;

    // "--test-osd" draws both overlays without waiting for the mouse to do
    // something, which is the only way to check them on demand.
    case IDM_TEST_OSD: {
        UiState s;
        EnterCriticalSection(&g_lock);
        s = g_ui;
        LeaveCriticalSection(&g_lock);
        ShowBatteryOsd(s.battery >= 0 ? s.battery : 12);
        break;
    }
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UpdateTray(TRUE);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        g_nid.hWnd = hwnd;
        g_nid.uID  = 1;
        UpdateTray(TRUE);

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        DEV_BROADCAST_DEVICEINTERFACE_W filter;
        RzZero(&filter, sizeof(filter));
        filter.dbcc_size       = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid  = hidGuid;
        g_devNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

        if (g_updateAnnounce)
            TrayBalloon(L"RazerLite updated", L"Now running version " APP_VERSION_STR);
        if (g_autoUpdate) SetTimer(hwnd, TIMER_UPDATE, UPDATE_FIRST_MS, NULL);
        return 0;
    }

    case WM_APP_STATE:
        UpdateTray(FALSE);
        return 0;

    case WM_APP_SHOW_OSD:
        ShowDpiOsd((int)wParam, (int)(lParam & 0xFF), (int)((lParam >> 8) & 0xFF));
        return 0;

    case WM_APP_SHOW_BATT:
        ShowBatteryOsd((int)wParam);
        return 0;

    case WM_APP_UPDATE: {
        BOOL silent = (BOOL)lParam;
        WCHAR text[160];
        switch (wParam) {
        case UPD_INSTALLED:
            // The replacement is already starting and will take this path over;
            // the only thing left to do is get out of its way.
            DestroyWindow(hwnd);
            break;
        case UPD_AVAILABLE:
            wsprintfW(text, L"Version %s is available. Updates - Check now to install it.",
                      g_updateVersion);
            TrayBalloon(L"RazerLite update", text);
            break;
        case UPD_CURRENT:
            if (!silent)
                MessageBoxW(NULL, L"RazerLite " APP_VERSION_STR L" is the latest version.",
                            L"RazerLite", MB_OK | MB_ICONINFORMATION);
            break;
        case UPD_BROKEN:
            if (!silent)
                MessageBoxW(NULL, L"The update could not be installed. The previous "
                                  L"version is still in place.",
                            L"RazerLite", MB_OK | MB_ICONWARNING);
            break;
        default:
            if (!silent)
                MessageBoxW(NULL, L"Could not reach GitHub to check for updates.",
                            L"RazerLite", MB_OK | MB_ICONWARNING);
            break;
        }
        return 0;
    }

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            PDEV_BROADCAST_HDR header = (PDEV_BROADCAST_HDR)lParam;
            if (header && header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                PDEV_BROADCAST_DEVICEINTERFACE_W iface =
                    (PDEV_BROADCAST_DEVICEINTERFACE_W)header;
                // USB spells the vendor "VID_1532"; the same mouse paired over
                // Bluetooth arrives as "...VID&00021532...", so both count.
                if (!PathContains(iface->dbcc_name, L"vid_1532") &&
                    !PathContains(iface->dbcc_name, L"vid&00021532")) break;
            }
            SetTimer(hwnd, 2, 1200, NULL);          // let the stack settle first
        }
        break;

    case WM_TIMER:
        if (wParam == 2) {
            KillTimer(hwnd, 2);
            PostThreadMessageW(g_hidThreadId, HIDM_RESCAN, 0, 0);
        } else if (wParam == 3) {
            KillTimer(hwnd, 3);
            if (g_pendingCommand) HandleCommand(hwnd, (int)g_pendingCommand);
            g_pendingCommand = 0;
        } else if (wParam == TIMER_UPDATE) {
            KillTimer(hwnd, TIMER_UPDATE);
            StartUpdateCheck(TRUE);
            SetTimer(hwnd, TIMER_UPDATE, UPDATE_EVERY_MS, NULL);
        }
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
            SetTimer(hwnd, 2, 3000, NULL);
        return TRUE;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            PostThreadMessageW(g_hidThreadId, HIDM_STEP_STAGE, 1, 0);
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        break;

    case WM_COMMAND:
        HandleCommand(hwnd, LOWORD(wParam));
        break;

    case WM_CLOSE:
        return 0;                                   // only Exit shuts us down

    case WM_DESTROY:
        if (g_devNotify) UnregisterDeviceNotification(g_devNotify);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_iconMuted && g_iconMuted != g_iconBase) DestroyIcon(g_iconMuted);
        if (g_iconAlert && g_iconAlert != g_iconBase) DestroyIcon(g_iconAlert);
        if (g_iconBase) DestroyIcon(g_iconBase);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------------------------------------------ start-up

static DWORD ParseUint(const WCHAR *s) {
    DWORD value = 0;
    for (; *s >= L'0' && *s <= L'9'; ++s) value = value * 10 + (DWORD)(*s - L'0');
    return value;
}

static UINT ParseCommandLine(void) {
    UINT command = 0;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    for (int i = 1; i < argc; ++i) {
        const WCHAR *arg = argv[i];
        while (*arg == L'-' || *arg == L'/') ++arg;
        // "--updated=<pid>" is how a freshly installed build is started: it has
        // to wait for the version it replaced to let go of the mutex first.
        if (lstrlenW(arg) > 8 &&
            CompareStringOrdinal(arg, 8, L"updated=", 8, TRUE) == CSTR_EQUAL) {
            g_updatedPid     = ParseUint(arg + 8);
            g_updateAnnounce = TRUE;
            continue;
        }
        if (!lstrcmpiW(arg, L"check-update")) command = IDM_CHECK_UPDATE;
        else if (!lstrcmpiW(arg, L"dpi-next"))      command = IDM_DPI_NEXT;
        else if (!lstrcmpiW(arg, L"dpi-prev")) command = IDM_DPI_PREV;
        else if (!lstrcmpiW(arg, L"status"))   command = IDM_DUMP;
        else if (!lstrcmpiW(arg, L"test-osd")) command = IDM_TEST_OSD;
        // a way back if the left button ever gets mapped to something unusable
        else if (!lstrcmpiW(arg, L"reset-buttons")) command = IDM_BTN_RESET;
        else if (!lstrcmpiW(arg, L"exit") || !lstrcmpiW(arg, L"quit")) command = IDM_EXIT;
    }
    LocalFree(argv);
    return command;
}

static int RunApp(HINSTANCE instance) {
    g_hInst = instance;
    SetUnhandledExceptionFilter(CrashHandler);
    SetProcessDPIAware();

    UINT command = ParseCommandLine();

    // Started as a replacement: give the version being retired its moment to
    // exit, otherwise the mutex below would send this build away again.
    if (g_updatedPid) {
        HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, g_updatedPid);
        if (previous) {
            WaitForSingleObject(previous, 15000);
            CloseHandle(previous);
        }
    }
    UpdateCleanup();              // drop the .old build left by a past update

    // A second launch is a remote control for the running one: RazerLite.exe
    // --dpi-next from a hotkey or a shortcut steps the stage and exits.
    HANDLE mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND running = FindWindowW(CLASS_NAME, NULL);
        if (running && command) PostMessageW(running, WM_COMMAND, command, 0);
        return 0;
    }

    InitializeCriticalSection(&g_lock);
    g_osdEnabled    = RegReadDword(L"Osd", 1) != 0;
    g_batteryWarnAt = (int)RegReadDword(L"BatteryWarn", 20);
    g_autoUpdate    = RegReadDword(L"AutoUpdate", 1) != 0;
    DWORD mode = RegReadDword(L"ForceMode", 0xFFFFFFFFu);
    g_forceMode = (mode == 0x00 || mode == 0x03) ? (int)mode : -1;
    LoadButtons();

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc;
    RzZero(&wc, sizeof(wc));
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = instance;
    wc.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassW(&wc)) return 1;

    RzZero(&wc, sizeof(wc));
    wc.lpfnWndProc   = OsdProc;
    wc.hInstance     = instance;
    wc.hbrBackground = NULL;
    wc.lpszClassName = OSD_CLASS;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, CLASS_NAME, L"RazerLite",
                             WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!g_hwnd) return 1;

    g_osd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            OSD_CLASS, L"", WS_POPUP, 0, 0, 10, 10,
                            NULL, NULL, instance, NULL);

    if (g_msgTaskbarCreated)
        ChangeWindowMessageFilterEx(g_hwnd, g_msgTaskbarCreated, MSGFLT_ALLOW, NULL);

    g_watchStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hidThread = CreateThread(NULL, 0, HidThreadProc, NULL, 0, &g_hidThreadId);
    UpdateHookState();

    // Started by a command line request rather than a running copy: give the
    // first scan a moment, then carry it out.
    if (command && command != IDM_EXIT) {
        g_pendingCommand = command;
        SetTimer(g_hwnd, 3, 2500, NULL);
    }

    MSG msg;
    RzZero(&msg, sizeof(msg));
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hidThreadId) PostThreadMessageW(g_hidThreadId, HIDM_QUIT, 0, 0);
    if (g_hidThread) { WaitForSingleObject(g_hidThread, 3000); CloseHandle(g_hidThread); }
    StopWatcher();
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) { WaitForSingleObject(g_hookThread, 2000); CloseHandle(g_hookThread); }
    return (int)msg.wParam;
}

extern "C" void AppEntryPoint(void) {
    ExitProcess((UINT)RunApp(GetModuleHandleW(NULL)));
}

extern "C" int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    return RunApp(instance);
}

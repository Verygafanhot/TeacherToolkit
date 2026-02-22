// TeacherToolkit.cpp : Defines the entry point for the application.
//
// System-tray app that detects a second monitor, extends the desktop,
// and mirrors the primary screen onto a fullscreen window on the second monitor.

#include "framework.h"
#include "TeacherToolkit.h"

#include <dbt.h>

#define MAX_LOADSTRING 100

// Version
#define APP_VERSION       L"0.1.1"
#define APP_VERSION_A      "0.1.1"

// Timer IDs
#define IDT_MONITOR_POLL    1
#define IDT_MIRROR_REFRESH  2
#define IDT_EXTEND_RETRY    3
#define IDT_DISPLAY_SETTLE  4

// Context menu IDs
#define IDM_TRAY_STATUS    300

// Intervals
#define MONITOR_POLL_MS  2000
#define MIRROR_FPS_MS    33     // ~30 fps (reduzido de 60 para diminuir carga)
#define EXTEND_RETRY_MS  1000   // retry checking after extend

// Registry key for update preferences
static const WCHAR REG_KEY[]   = L"Software\\TeacherToolkit";
static const WCHAR REG_SKIP[]  = L"SkipVersion";

// Single-instance mutex name
static const WCHAR MUTEX_NAME[] = L"Global\\TeacherToolkit_SingleInstance";

// Global Variables
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

static const WCHAR MIRROR_CLASS[] = L"TeacherToolkitMirror";

// State
NOTIFYICONDATA nid = {};
HWND g_hHidden  = nullptr;
HWND g_hMirror  = nullptr;
BOOL g_bProjecting = FALSE;
BOOL g_bExtendPending = FALSE;
int  g_nExtendRetries = 0;
RECT g_rcSecond  = {};
RECT g_rcPrimary = {};
HDEVNOTIFY g_hDevNotify = nullptr;
HANDLE g_hMutex = nullptr;

// Mirroring resources
HDC     g_hdcMem    = nullptr;
HBITMAP g_hBmpMem   = nullptr;
HBITMAP g_hOldBmp   = nullptr;
int     g_memW      = 0;
int     g_memH      = 0;

// Config loaded from embedded resource (config.ini compiled into exe)
WCHAR g_szAuthor[128]      = L"";
WCHAR g_szGitHubRepo[256]  = L"";
WCHAR g_szLatestVer[64]    = L"";
BOOL  g_bUpdateAvailable   = FALSE;

// Forward declarations
ATOM                RegisterHiddenClass(HINSTANCE hInstance);
ATOM                RegisterMirrorClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    HiddenWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    MirrorWndProc(HWND, UINT, WPARAM, LPARAM);

void AddTrayIcon(HWND hWnd);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hWnd);
BOOL HasSecondMonitor(RECT* rcPrimary, RECT* rcSecond);
int  CountPhysicalDisplays();
void StartMirroring();
void StopMirroring();
void FreeMirrorResources();
void TryExtendAndMirror();
BOOL SetExtendMode();
void CheckMonitorState();
void RegisterForDeviceNotifications(HWND hWnd);
void UnregisterDeviceNotifications();
void ShowAboutDialog(HWND hWnd);

// Config / update helpers
void LoadLocalConfig();
BOOL CheckForUpdate();
void PromptUpdate(HWND hWnd);
BOOL IsVersionNewer(const char* remote, const char* local);
BOOL IsVersionSkipped(const WCHAR* version);
void SetVersionSkipped(const WCHAR* version);

// Startup helpers
BOOL IsStartupEnabled();
void EnableStartup();
void DisableStartup();
BOOL GetAppDataExePath(WCHAR* buf, DWORD cch);
BOOL GetStartupShortcutPath(WCHAR* buf, DWORD cch);
BOOL FilesMatchByHash(const WCHAR* path1, const WCHAR* path2);
BOOL ComputeFileHash(const WCHAR* path, BYTE* hashOut, DWORD hashSize);

// — Config loading ————————————————————————————————————————————————————
// TrimWhitespace() removed - now done by the parser

// Parse a value from an INI-style buffer: looks for "key=" and returns
// the text after '=' up to the next newline.
static BOOL ParseIniValue(const char* data, DWORD dataLen,
                          const char* key, WCHAR* out, DWORD outCch)
{
    out[0] = L'\0';
    size_t keyLen = strlen(key);
    const char* p = data;
    const char* end = data + dataLen;

    while (p < end) {
        // Skip whitespace/newlines
        while (p < end && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t'))
            p++;
        if (p >= end) break;

        // Skip comments and section headers
        if (*p == '#' || *p == ';' || *p == '[') {
            while (p < end && *p != '\n') p++;
            continue;
        }

        // Check if this line starts with the key
        if ((size_t)(end - p) > keyLen &&
            _strnicmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            const char* val = p + keyLen + 1;
            const char* eol = val;
            while (eol < end && *eol != '\r' && *eol != '\n') eol++;
            int len = (int)(eol - val);
            if (len > 0 && (DWORD)len < outCch) {
                MultiByteToWideChar(CP_UTF8, 0, val, len, out, outCch);
                out[len] = L'\0';
                return TRUE;
            }
            return FALSE;
        }

        // Skip to next line
        while (p < end && *p != '\n') p++;
    }
    return FALSE;
}

void LoadLocalConfig()
{
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_CONFIG), RT_RCDATA);
    if (!hRes) return;

    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return;

    const char* data = (const char*)LockResource(hData);
    DWORD dataLen = SizeofResource(hInst, hRes);
    if (!data || dataLen == 0) return;

    ParseIniValue(data, dataLen, "author", g_szAuthor, ARRAYSIZE(g_szAuthor));
    ParseIniValue(data, dataLen, "github_repo", g_szGitHubRepo, ARRAYSIZE(g_szGitHubRepo));
}

// — Version comparison ————————————————————————————————————————————————
// Returns TRUE if remote > local (simple major.minor.patch comparison)
BOOL IsVersionNewer(const char* remote, const char* local)
{
    int rMaj = 0, rMin = 0, rPat = 0;
    int lMaj = 0, lMin = 0, lPat = 0;
    sscanf_s(remote, "%d.%d.%d", &rMaj, &rMin, &rPat);
    sscanf_s(local,  "%d.%d.%d", &lMaj, &lMin, &lPat);

    if (rMaj != lMaj) return rMaj > lMaj;
    if (rMin != lMin) return rMin > lMin;
    return rPat > lPat;
}

// — Registry helpers for skipped version ——————————————————————————————
BOOL IsVersionSkipped(const WCHAR* version)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    WCHAR buf[64] = {};
    DWORD cbBuf = sizeof(buf);
    DWORD type = 0;
    BOOL skipped = FALSE;
    if (RegQueryValueExW(hKey, REG_SKIP, nullptr, &type, (BYTE*)buf, &cbBuf) == ERROR_SUCCESS) {
        if (type == REG_SZ && wcscmp(buf, version) == 0)
            skipped = TRUE;
    }
    RegCloseKey(hKey);
    return skipped;
}

void SetVersionSkipped(const WCHAR* version)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_SKIP, 0, REG_SZ,
                       (const BYTE*)version, (DWORD)((wcslen(version) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
}

// — Update check (GitHub API) —————————————————————————————————————————
// Fetches /repos/{owner}/{repo}/releases/latest from the GitHub API.
// Parses "tag_name" from the JSON response to get the latest version.
BOOL CheckForUpdate()
{
    g_bUpdateAvailable = FALSE;
    g_szLatestVer[0] = L'\0';

    if (g_szGitHubRepo[0] == L'\0')
        return FALSE;

    WCHAR url[512];
    StringCchPrintfW(url, ARRAYSIZE(url),
        L"https://api.github.com/repos/%s/releases/latest", g_szGitHubRepo);

    HINTERNET hInet = InternetOpenW(L"TeacherToolkit/" APP_VERSION,
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) return FALSE;

    HINTERNET hUrl = InternetOpenUrlW(hInet, url, nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) {
        InternetCloseHandle(hInet);
        return FALSE;
    }

    char buf[4096] = {};
    DWORD totalRead = 0;
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buf + totalRead,
           (DWORD)(sizeof(buf) - 1 - totalRead), &bytesRead) && bytesRead > 0) {
        totalRead += bytesRead;
        if (totalRead >= sizeof(buf) - 1) break;
    }
    buf[totalRead] = '\0';

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);

    // Simple JSON parse for "tag_name":"vX.Y.Z" or "tag_name":"X.Y.Z"
    const char* tagKey = "\"tag_name\"";
    char* pos = strstr(buf, tagKey);
    if (!pos) return FALSE;

    pos += strlen(tagKey);
    while (*pos == ' ' || *pos == ':' || *pos == '"') pos++;
    if (*pos == 'v' || *pos == 'V') pos++;

    char ver[64] = {};
    int vi = 0;
    while (*pos && *pos != '"' && vi < 63) {
        ver[vi++] = *pos++;
    }
    ver[vi] = '\0';

    if (vi == 0) return FALSE;

    // Convert to wide for storage
    MultiByteToWideChar(CP_UTF8, 0, ver, -1, g_szLatestVer, ARRAYSIZE(g_szLatestVer));

    if (IsVersionNewer(ver, APP_VERSION_A)) {
        g_bUpdateAvailable = TRUE;
        return TRUE;
    }
    return FALSE;
}

void PromptUpdate(HWND hWnd)
{
    if (!g_bUpdateAvailable || g_szLatestVer[0] == L'\0')
        return;

    if (IsVersionSkipped(g_szLatestVer))
        return;

    WCHAR content[512];
    StringCchPrintfW(content, ARRAYSIZE(content),
        L"Versão atual: v%s\nNova versão: v%s",
        APP_VERSION, g_szLatestVer);

    TASKDIALOG_BUTTON buttons[] = {
        { IDB_UPDATE_DOWNLOAD, L"Transferir agora" },
        { IDB_UPDATE_LATER,    L"Lembrar mais tarde" },
        { IDB_UPDATE_SKIP,     L"Não quero!" },
    };

    TASKDIALOGCONFIG tdc = {};
    tdc.cbSize           = sizeof(tdc);
    tdc.hwndParent       = hWnd;
    tdc.hInstance        = hInst;
    tdc.dwFlags          = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.pszWindowTitle   = L"TeacherToolkit — Atualização";
    tdc.pszMainInstruction = L"Atualização disponível!";
    tdc.pszContent       = content;
    tdc.cButtons         = ARRAYSIZE(buttons);
    tdc.pButtons         = buttons;
    tdc.nDefaultButton   = IDB_UPDATE_DOWNLOAD;
    tdc.hMainIcon        = LoadIcon(hInst, MAKEINTRESOURCE(IDI_TEACHERTOOLKIT));

    int pressed = 0;
    HRESULT hr = TaskDialogIndirect(&tdc, &pressed, nullptr, nullptr);
    if (FAILED(hr)) return;

    if (pressed == IDB_UPDATE_DOWNLOAD) {
        WCHAR url[512];
        StringCchPrintfW(url, ARRAYSIZE(url),
            L"https://github.com/%s/releases/latest", g_szGitHubRepo);
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (pressed == IDB_UPDATE_SKIP) {
        SetVersionSkipped(g_szLatestVer);
    }
    // IDB_UPDATE_LATER / dialog closed = do nothing, ask again next launch
}

// — About dialog ——————————————————————————————————————————————————————
void ShowAboutDialog(HWND hWnd)
{
    WCHAR msg[512];
    if (g_szAuthor[0] != L'\0') {
        StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"TeacherToolkit\nVersão %s\n\nDesenvolvido por %s",
            APP_VERSION, g_szAuthor);
    } else {
        StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"TeacherToolkit\nVersão %s",
            APP_VERSION);
    }

    MSGBOXPARAMSW mbp = {};
    mbp.cbSize       = sizeof(mbp);
    mbp.hwndOwner    = hWnd;
    mbp.hInstance    = hInst;
    mbp.lpszText     = msg;
    mbp.lpszCaption  = L"Sobre — TeacherToolkit";
    mbp.dwStyle      = MB_OK | MB_USERICON;
    mbp.lpszIcon     = MAKEINTRESOURCEW(IDI_TEACHERTOOLKIT);
    MessageBoxIndirectW(&mbp);
}

// — Monitor enumeration ———————————————————————————————————————————————
struct MonitorEnumData {
    int   count;
    RECT  rcPrimary;
    RECT  rcSecondary;
    BOOL  foundPrimary;
    BOOL  foundSecondary;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
    auto* data = reinterpret_cast<MonitorEnumData*>(lParam);
    MONITORINFOEX mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);

    if (mi.dwFlags & MONITORINFOF_PRIMARY) {
        data->rcPrimary = mi.rcMonitor;
        data->foundPrimary = TRUE;
    } else {
        data->rcSecondary = mi.rcMonitor;
        data->foundSecondary = TRUE;
    }
    data->count++;
    return TRUE;
}

BOOL HasSecondMonitor(RECT* rcPrimary, RECT* rcSecond)
{
    MonitorEnumData data = {};
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));
    if (data.count >= 2 && data.foundSecondary) {
        if (rcPrimary) *rcPrimary = data.rcPrimary;
        if (rcSecond)  *rcSecond  = data.rcSecondary;
        return TRUE;
    }
    return FALSE;
}

// Count how many physical displays are connected (including inactive ones)
// using the CCD API (QueryDisplayConfig). This correctly detects monitors
// that are physically attached but not part of the desktop (e.g. in
// "PC screen only" or "Duplicate" mode).
int CountPhysicalDisplays()
{
    UINT32 numPaths = 0, numModes = 0;
    LONG ret = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes);
    if (ret != ERROR_SUCCESS || numPaths == 0)
        return 1;

    DISPLAYCONFIG_PATH_INFO* paths = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, numPaths * sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO* modes = (DISPLAYCONFIG_MODE_INFO*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, numModes * sizeof(DISPLAYCONFIG_MODE_INFO));
    if (!paths || !modes) {
        if (paths) HeapFree(GetProcessHeap(), 0, paths);
        if (modes) HeapFree(GetProcessHeap(), 0, modes);
        return 1;
    }

    ret = QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths, &numModes, modes, nullptr);
    if (ret != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, paths);
        HeapFree(GetProcessHeap(), 0, modes);
        return 1;
    }

    struct TargetKey {
        LUID adapterId;
        UINT32 targetId;
    };
    TargetKey seen[32] = {};
    int uniqueCount = 0;

    for (UINT32 i = 0; i < numPaths; i++) {
        if (!paths[i].targetInfo.targetAvailable)
            continue;

        LUID aid = paths[i].targetInfo.adapterId;
        UINT32 tid = paths[i].targetInfo.id;

        BOOL duplicate = FALSE;
        for (int j = 0; j < uniqueCount; j++) {
            if (seen[j].adapterId.LowPart == aid.LowPart &&
                seen[j].adapterId.HighPart == aid.HighPart &&
                seen[j].targetId == tid) {
                duplicate = TRUE;
                break;
            }
        }
        if (!duplicate && uniqueCount < 32) {
            seen[uniqueCount].adapterId = aid;
            seen[uniqueCount].targetId = tid;
            uniqueCount++;
        }
    }

    HeapFree(GetProcessHeap(), 0, paths);
    HeapFree(GetProcessHeap(), 0, modes);
    return uniqueCount > 0 ? uniqueCount : 1;
}

// — Extend display mode ———————————————————————————————————————————————

// Try the simple topology-level extend first.
// If that fails, enumerate all CCD paths, find an inactive target that
// has a monitor physically connected, enable that path, and apply.
BOOL SetExtendMode()
{
    // Attempt 1: simple topology switch
    LONG ret = SetDisplayConfig(0, nullptr, 0, nullptr,
        SDC_APPLY | SDC_TOPOLOGY_EXTEND | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (ret == ERROR_SUCCESS)
        return TRUE;

    // Attempt 2: explicitly activate an inactive path via CCD.
    // Query only active paths first so we preserve them, then find an
    // inactive-but-available target from QDC_ALL_PATHS to add.
    UINT32 numActivePaths = 0, numActiveModes = 0;
    ret = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numActivePaths, &numActiveModes);
    if (ret != ERROR_SUCCESS) return FALSE;

    UINT32 numAllPaths = 0, numAllModes = 0;
    ret = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numAllPaths, &numAllModes);
    if (ret != ERROR_SUCCESS || numAllPaths == 0) return FALSE;

    DISPLAYCONFIG_PATH_INFO* activePaths = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, numActivePaths * sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO* activeModes = (DISPLAYCONFIG_MODE_INFO*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, (numActiveModes + 2) * sizeof(DISPLAYCONFIG_MODE_INFO));
    DISPLAYCONFIG_PATH_INFO* allPaths = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, numAllPaths * sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO* allModes = (DISPLAYCONFIG_MODE_INFO*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, numAllModes * sizeof(DISPLAYCONFIG_MODE_INFO));

    if (!activePaths || !activeModes || !allPaths || !allModes) {
        if (activePaths) HeapFree(GetProcessHeap(), 0, activePaths);
        if (activeModes) HeapFree(GetProcessHeap(), 0, activeModes);
        if (allPaths)    HeapFree(GetProcessHeap(), 0, allPaths);
        if (allModes)    HeapFree(GetProcessHeap(), 0, allModes);
        return FALSE;
    }

    BOOL result = FALSE;

    ret = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numActivePaths, activePaths, &numActiveModes, activeModes, nullptr);
    if (ret == ERROR_SUCCESS) {
        ret = QueryDisplayConfig(QDC_ALL_PATHS, &numAllPaths, allPaths, &numAllModes, allModes, nullptr);
    }

    if (ret == ERROR_SUCCESS) {
        // Find the first inactive path whose target is physically available
        DISPLAYCONFIG_PATH_INFO* newPath = nullptr;
        for (UINT32 i = 0; i < numAllPaths; i++) {
            if (!allPaths[i].targetInfo.targetAvailable) continue;
            if (allPaths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) continue;
            newPath = &allPaths[i];
            break;
        }

        if (newPath) {
            // Build a combined path array: existing active paths + the new one
            UINT32 totalPaths = numActivePaths + 1;
            DISPLAYCONFIG_PATH_INFO* combined = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, totalPaths * sizeof(DISPLAYCONFIG_PATH_INFO));

            if (combined) {
                for (UINT32 i = 0; i < numActivePaths; i++)
                    combined[i] = activePaths[i];

                combined[numActivePaths] = *newPath;
                combined[numActivePaths].flags |= DISPLAYCONFIG_PATH_ACTIVE;
                combined[numActivePaths].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                combined[numActivePaths].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

                ret = SetDisplayConfig(totalPaths, combined, numActiveModes, activeModes,
                    SDC_APPLY | SDC_ALLOW_CHANGES | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
                result = (ret == ERROR_SUCCESS);

                HeapFree(GetProcessHeap(), 0, combined);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, activePaths);
    HeapFree(GetProcessHeap(), 0, activeModes);
    HeapFree(GetProcessHeap(), 0, allPaths);
    HeapFree(GetProcessHeap(), 0, allModes);
    return result;
}

void TryExtendAndMirror()
{
    if (g_bProjecting || g_bExtendPending) return;

    SetExtendMode();
    g_bExtendPending = TRUE;
    g_nExtendRetries = 0;
    SetTimer(g_hHidden, IDT_EXTEND_RETRY, EXTEND_RETRY_MS, nullptr);
}

// — Central monitor check —————————————————————————————————————————————
void CheckMonitorState()
{
    BOOL secondNow = HasSecondMonitor(&g_rcPrimary, &g_rcSecond);
    if (secondNow && !g_bProjecting) {
        // A second monitor appeared — cancel any pending extend and start mirroring
        if (g_bExtendPending) {
            KillTimer(g_hHidden, IDT_EXTEND_RETRY);
            g_bExtendPending = FALSE;
        }
        StartMirroring();
    } else if (!secondNow && !g_bProjecting && !g_bExtendPending) {
        if (CountPhysicalDisplays() >= 2) {
            TryExtendAndMirror();
        }
    } else if (!secondNow && g_bProjecting) {
        StopMirroring();
    }
}

// — Device notification registration —————————————————————————————————
void RegisterForDeviceNotifications(HWND hWnd)
{
    DEV_BROADCAST_DEVICEINTERFACE filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    // GUID_DEVINTERFACE_MONITOR {E6F07B5F-EE97-4A90-B076-33F57BF4EAA7}
    filter.dbcc_classguid = { 0xE6F07B5F, 0xEE97, 0x4A90,
        { 0xB0, 0x76, 0x33, 0xF5, 0x7B, 0xF4, 0xEA, 0xA7 } };

    g_hDevNotify = RegisterDeviceNotification(hWnd, &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE);
}

void UnregisterDeviceNotifications()
{
    if (g_hDevNotify) {
        UnregisterDeviceNotification(g_hDevNotify);
        g_hDevNotify = nullptr;
    }
}

// — Tray helpers ——————————————————————————————————————————————————————
void AddTrayIcon(HWND hWnd)
{
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(NOTIFYICONDATA);
    nid.hWnd             = hWnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TEACHERTOOLKIT),
                               IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON),
                               LR_DEFAULTCOLOR);
    StringCchCopy(nid.szTip, ARRAYSIZE(nid.szTip), L"TeacherToolkit v" APP_VERSION);

    Shell_NotifyIcon(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon()
{
    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (nid.hIcon) {
        DestroyIcon(nid.hIcon);
        nid.hIcon = nullptr;
    }
}

void ShowTrayMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    WCHAR verLabel[64];
    StringCchPrintfW(verLabel, ARRAYSIZE(verLabel), L"TeacherToolkit v%s", APP_VERSION);
    AppendMenu(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, verLabel);

    if (g_bProjecting)
        AppendMenu(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_TRAY_STATUS,
                   L"\x2714  Neste momento a projetar");
    else
        AppendMenu(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_TRAY_STATUS,
                   L"\x2716  Nenhum Projetor (está em modo espandir?)");

    if (g_bUpdateAvailable && g_szLatestVer[0] != L'\0') {
        WCHAR updateLabel[128];
        StringCchPrintfW(updateLabel, ARRAYSIZE(updateLabel),
            L"\x2B06  Atualização disponível: v%s", g_szLatestVer);
        AppendMenu(hMenu, MF_STRING, IDM_TRAY_UPDATE, updateLabel);
    }

    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);

    BOOL startupEnabled = IsStartupEnabled();
    AppendMenu(hMenu, MF_STRING | (startupEnabled ? MF_CHECKED : MF_UNCHECKED),
               IDM_TRAY_STARTUP, L"Iniciar com o Windows");

    AppendMenu(hMenu, MF_STRING, IDM_TRAY_ABOUT, L"Sobre");

    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Fechar a App");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hWnd, nullptr);
    PostMessage(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

// — Startup helpers ———————————————————————————————————————————————————
BOOL GetAppDataExePath(WCHAR* buf, DWORD cch)
{
    WCHAR appData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return FALSE;
    StringCchPrintfW(buf, cch, L"%s\\TeacherToolkit\\TeacherToolkit.exe", appData);
    return TRUE;
}

BOOL GetStartupShortcutPath(WCHAR* buf, DWORD cch)
{
    WCHAR startup[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, startup)))
        return FALSE;
    StringCchPrintfW(buf, cch, L"%s\\TeacherToolkit.lnk", startup);
    return TRUE;
}

BOOL ComputeFileHash(const WCHAR* path, BYTE* hashOut, DWORD hashSize)
{
    BOOL result = FALSE;
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            BYTE buffer[8192];
            DWORD bytesRead;
            BOOL ok = TRUE;
            while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                if (!CryptHashData(hHash, buffer, bytesRead, 0)) { ok = FALSE; break; }
            }
            if (ok) {
                DWORD cbHash = hashSize;
                result = CryptGetHashParam(hHash, HP_HASHVAL, hashOut, &cbHash, 0);
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    CloseHandle(hFile);
    return result;
}

BOOL FilesMatchByHash(const WCHAR* path1, const WCHAR* path2)
{
    BYTE hash1[32], hash2[32];
    if (!ComputeFileHash(path1, hash1, sizeof(hash1))) return FALSE;
    if (!ComputeFileHash(path2, hash2, sizeof(hash2))) return FALSE;
    return (memcmp(hash1, hash2, 32) == 0);
}

static BOOL CreateShortcut(const WCHAR* targetPath, const WCHAR* shortcutPath)
{
    BOOL result = FALSE;
    CoInitialize(nullptr);

    IShellLinkW* pShellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&pShellLink);
    if (SUCCEEDED(hr)) {
        pShellLink->SetPath(targetPath);

        WCHAR dir[MAX_PATH];
        StringCchCopyW(dir, MAX_PATH, targetPath);
        WCHAR* lastSlash = wcsrchr(dir, L'\\');
        if (lastSlash) *lastSlash = L'\0';
        pShellLink->SetWorkingDirectory(dir);

        IPersistFile* pPersist = nullptr;
        hr = pShellLink->QueryInterface(IID_IPersistFile, (void**)&pPersist);
        if (SUCCEEDED(hr)) {
            hr = pPersist->Save(shortcutPath, TRUE);
            result = SUCCEEDED(hr);
            pPersist->Release();
        }
        pShellLink->Release();
    }

    CoUninitialize();
    return result;
}

BOOL IsStartupEnabled()
{
    WCHAR shortcutPath[MAX_PATH];
    if (!GetStartupShortcutPath(shortcutPath, MAX_PATH)) return FALSE;
    return (GetFileAttributesW(shortcutPath) != INVALID_FILE_ATTRIBUTES);
}

void EnableStartup()
{
    WCHAR currentExe[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExe, MAX_PATH);

    WCHAR appDataExe[MAX_PATH];
    if (!GetAppDataExePath(appDataExe, MAX_PATH)) return;

    WCHAR appDataDir[MAX_PATH];
    StringCchCopyW(appDataDir, MAX_PATH, appDataExe);
    WCHAR* lastSlash = wcsrchr(appDataDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    CreateDirectoryW(appDataDir, nullptr);

    BOOL needCopy = TRUE;
    if (GetFileAttributesW(appDataExe) != INVALID_FILE_ATTRIBUTES) {
        if (FilesMatchByHash(currentExe, appDataExe)) {
            needCopy = FALSE;
        }
    }
    if (needCopy) {
        CopyFileW(currentExe, appDataExe, FALSE);
    }

    WCHAR shortcutPath[MAX_PATH];
    if (!GetStartupShortcutPath(shortcutPath, MAX_PATH)) return;
    CreateShortcut(appDataExe, shortcutPath);
}

void DisableStartup()
{
    WCHAR shortcutPath[MAX_PATH];
    if (GetStartupShortcutPath(shortcutPath, MAX_PATH)) {
        DeleteFileW(shortcutPath);
    }

    WCHAR appDataExe[MAX_PATH];
    if (GetAppDataExePath(appDataExe, MAX_PATH)) {
        DeleteFileW(appDataExe);

        WCHAR appDataDir[MAX_PATH];
        StringCchCopyW(appDataDir, MAX_PATH, appDataExe);
        WCHAR* lastSlash = wcsrchr(appDataDir, L'\\');
        if (lastSlash) *lastSlash = L'\0';
        RemoveDirectoryW(appDataDir);
    }
}

static void UpdateStartupExeIfNeeded()
{
    if (!IsStartupEnabled()) return;

    WCHAR currentExe[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExe, MAX_PATH);

    WCHAR appDataExe[MAX_PATH];
    if (!GetAppDataExePath(appDataExe, MAX_PATH)) return;

    if (GetFileAttributesW(appDataExe) != INVALID_FILE_ATTRIBUTES) {
        if (!FilesMatchByHash(currentExe, appDataExe)) {
            CopyFileW(currentExe, appDataExe, FALSE);
        }
    }
}

// — ClipCursor — restrict cursor to primary monitor —----------------------------------------------
void ClipCursorToPrimary(BOOL clip)
{
    if (clip) {
        RECT rc;
        rc.left   = g_rcPrimary.left;
        rc.top    = g_rcPrimary.top;
        rc.right  = g_rcPrimary.right;
        rc.bottom = g_rcPrimary.bottom;
        ClipCursor(&rc);
    } else {
        ClipCursor(nullptr);
    }
}

// — Mirror start / stop ———————————————————————————————————————————————
void StartMirroring()
{
    if (g_bProjecting) return;

    int x = g_rcSecond.left;
    int y = g_rcSecond.top;
    int w = g_rcSecond.right  - g_rcSecond.left;
    int h = g_rcSecond.bottom - g_rcSecond.top;
    if (w <= 0 || h <= 0) return;

    g_hMirror = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        MIRROR_CLASS, L"Mirror",
        WS_POPUP,
        x, y, w, h,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hMirror) return;

    SetWindowPos(g_hMirror, HWND_TOPMOST,
                 x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetTimer(g_hMirror, IDT_MIRROR_REFRESH, MIRROR_FPS_MS, nullptr);

    g_bProjecting = TRUE;
    
    // Confine cursor to primary monitor instead of using hook
    ClipCursor(&g_rcPrimary);
}

void StopMirroring()
{
    if (!g_bProjecting) return;
    
    // Release cursor clipping
    ClipCursor(nullptr);
    
    if (g_hMirror) {
        KillTimer(g_hMirror, IDT_MIRROR_REFRESH);
        DestroyWindow(g_hMirror);
        g_hMirror = nullptr;
    }
    FreeMirrorResources();
    g_bProjecting = FALSE;
}

void FreeMirrorResources()
{
    if (g_hdcMem) {
        if (g_hOldBmp) SelectObject(g_hdcMem, g_hOldBmp);
        DeleteDC(g_hdcMem);
        g_hdcMem = nullptr;
    }
    if (g_hBmpMem) {
        DeleteObject(g_hBmpMem);
        g_hBmpMem = nullptr;
    }
    g_hOldBmp = nullptr;
    g_memW = 0;
    g_memH = 0;
}

// — Entry point ———————————————————————————————————————————————————————
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR    lpCmdLine,
                      _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    SetProcessDPIAware();

    // Single-instance check
    g_hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
            L"O TeacherToolkit já está em execução.\n"
            L"Feche a instância anterior primeiro.",
            L"TeacherToolkit", MB_OK | MB_ICONWARNING);
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_TEACHERTOOLKIT, szWindowClass, MAX_LOADSTRING);

    RegisterHiddenClass(hInstance);
    RegisterMirrorClass(hInstance);

    // Load config and check for updates before creating windows
    LoadLocalConfig();

    if (!InitInstance(hInstance, nCmdShow)) {
        if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
        return FALSE;
    }

    // Check for updates (non-blocking if no network)
    CheckForUpdate();
    if (g_bUpdateAvailable) {
        PromptUpdate(g_hHidden);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ClipCursor(nullptr);
    UnregisterDeviceNotifications();
    RemoveTrayIcon();
    if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
    return (int)msg.wParam;
}

// — Window class registration —————————————————————————————————————————
ATOM RegisterHiddenClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(wcex);
    wcex.lpfnWndProc   = HiddenWndProc;
    wcex.hInstance      = hInstance;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TEACHERTOOLKIT));
    wcex.hIconSm        = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

ATOM RegisterMirrorClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(wcex);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = MirrorWndProc;
    wcex.hInstance      = hInstance;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = MIRROR_CLASS;
    return RegisterClassExW(&wcex);
}

// — InitInstance — create hidden window + tray icon ———————————————————
BOOL InitInstance(HINSTANCE hInstance, int)
{
    hInst = hInstance;

    g_hHidden = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hHidden) return FALSE;

    AddTrayIcon(g_hHidden);
    RegisterForDeviceNotifications(g_hHidden);

    UpdateStartupExeIfNeeded();

    SetTimer(g_hHidden, IDT_MONITOR_POLL, MONITOR_POLL_MS, nullptr);

    if (HasSecondMonitor(&g_rcPrimary, &g_rcSecond)) {
        StartMirroring();
    } else if (CountPhysicalDisplays() >= 2) {
        TryExtendAndMirror();
    }

    return TRUE;
}

// — Hidden window proc (tray + monitor polling) ———————————————————————
LRESULT CALLBACK HiddenWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
        if (wParam == IDT_MONITOR_POLL) {
            CheckMonitorState();
        }
        else if (wParam == IDT_DISPLAY_SETTLE) {
            KillTimer(hWnd, IDT_DISPLAY_SETTLE);
            CheckMonitorState();
        }
        else if (wParam == IDT_EXTEND_RETRY) {
            g_nExtendRetries++;
            if (HasSecondMonitor(&g_rcPrimary, &g_rcSecond)) {
                KillTimer(g_hHidden, IDT_EXTEND_RETRY);
                g_bExtendPending = FALSE;
                StartMirroring();
            } else if (g_nExtendRetries >= 10) {
                KillTimer(g_hHidden, IDT_EXTEND_RETRY);
                g_bExtendPending = FALSE;
            }
        }
        break;

    case WM_CHECKMONITOR:
        CheckMonitorState();
        break;

    case WM_DISPLAYCHANGE:
        // WM_DISPLAYCHANGE fires before the display list is fully updated.
        // Post an immediate check, then arm a settle timer to catch the case
        // where EnumDisplayMonitors hasn't refreshed yet on the first check.
        PostMessage(hWnd, WM_CHECKMONITOR, 0, 0);
        SetTimer(hWnd, IDT_DISPLAY_SETTLE, 500, nullptr);
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVNODES_CHANGED ||
            wParam == DBT_DEVICEARRIVAL ||
            wParam == DBT_DEVICEREMOVECOMPLETE) {
            PostMessage(hWnd, WM_CHECKMONITOR, 0, 0);
        }
        break;

    case WM_TRAYICON:
        switch (LOWORD(lParam))
        {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hWnd);
            break;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_TRAY_EXIT || LOWORD(wParam) == IDM_EXIT) {
            StopMirroring();
            UnregisterDeviceNotifications();
            RemoveTrayIcon();
            PostQuitMessage(0);
        }
        else if (LOWORD(wParam) == IDM_TRAY_STARTUP) {
            if (IsStartupEnabled()) {
                DisableStartup();
            } else {
                EnableStartup();
            }
        }
        else if (LOWORD(wParam) == IDM_TRAY_ABOUT) {
            ShowAboutDialog(hWnd);
        }
        else if (LOWORD(wParam) == IDM_TRAY_UPDATE) {
            PromptUpdate(hWnd);
        }
        break;

    case WM_DESTROY:
        StopMirroring();
        UnregisterDeviceNotifications();
        RemoveTrayIcon();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// — Compute letterbox/pillarbox destination rect ——————————————————————
// Returns a centered rect within (0,0,dstW,dstH) that fits srcW×srcH
// without distortion.
static RECT ComputeLetterboxRect(int srcW, int srcH, int dstW, int dstH)
{
    RECT r = {};
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return r;

    // Scale uniformly to fit the destination
    int scaledW, scaledH;
    if (dstW * srcH <= dstH * srcW) {
        // Width is the limiting dimension
        scaledW = dstW;
        scaledH = (dstW * srcH) / srcW;
    } else {
        // Height is the limiting dimension
        scaledH = dstH;
        scaledW = (dstH * srcW) / srcH;
    }

    r.left   = (dstW - scaledW) / 2;
    r.top    = (dstH - scaledH) / 2;
    r.right  = r.left + scaledW;
    r.bottom = r.top  + scaledH;
    return r;
}

// — Mirror window proc (captures primary screen + draws cursor) ———————
LRESULT CALLBACK MirrorWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
        if (wParam == IDT_MIRROR_REFRESH) {
            // Capture and blit directly — skip InvalidateRect/WM_PAINT overhead
            int srcW = g_rcPrimary.right  - g_rcPrimary.left;
            int srcH = g_rcPrimary.bottom - g_rcPrimary.top;
            int dstW = g_rcSecond.right   - g_rcSecond.left;
            int dstH = g_rcSecond.bottom  - g_rcSecond.top;

            if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
                break;

            HDC hdcWnd = GetDC(hWnd);
            HDC hdcScreen = GetDC(nullptr);
            if (!hdcWnd || !hdcScreen) {
                if (hdcWnd)    ReleaseDC(hWnd, hdcWnd);
                if (hdcScreen) ReleaseDC(nullptr, hdcScreen);
                break;
            }

            // Reinitialize memory DC / bitmap only if size changed
            if (!g_hdcMem || !g_hBmpMem || g_memW != srcW || g_memH != srcH) {
                FreeMirrorResources();
                g_hdcMem  = CreateCompatibleDC(hdcScreen);
                g_hBmpMem = CreateCompatibleBitmap(hdcScreen, srcW, srcH);
                g_memW = srcW;
                g_memH = srcH;
                if (g_hdcMem && g_hBmpMem) {
                    g_hOldBmp = (HBITMAP)SelectObject(g_hdcMem, g_hBmpMem);
                }
            }

            if (g_hdcMem && g_hBmpMem) {
                BitBlt(g_hdcMem, 0, 0, srcW, srcH,
                       hdcScreen, g_rcPrimary.left, g_rcPrimary.top, SRCCOPY);

                // Draw cursor onto the captured image
                CURSORINFO ci = {};
                ci.cbSize = sizeof(ci);
                if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                    ICONINFO ii = {};
                    if (GetIconInfo(ci.hCursor, &ii)) {
                        int cx = ci.ptScreenPos.x - g_rcPrimary.left - (int)ii.xHotspot;
                        int cy = ci.ptScreenPos.y - g_rcPrimary.top  - (int)ii.yHotspot;
                        DrawIconEx(g_hdcMem, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
                        if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    }
                }

                // Letterbox into the destination
                RECT dst = ComputeLetterboxRect(srcW, srcH, dstW, dstH);

                // Black bars
                HBRUSH hBlack = (HBRUSH)GetStockObject(BLACK_BRUSH);
                if (dst.top > 0) {
                    RECT bar = { 0, 0, dstW, dst.top };
                    FillRect(hdcWnd, &bar, hBlack);
                }
                if (dst.bottom < dstH) {
                    RECT bar = { 0, dst.bottom, dstW, dstH };
                    FillRect(hdcWnd, &bar, hBlack);
                }
                if (dst.left > 0) {
                    RECT bar = { 0, dst.top, dst.left, dst.bottom };
                    FillRect(hdcWnd, &bar, hBlack);
                }
                if (dst.right < dstW) {
                    RECT bar = { dst.right, dst.top, dstW, dst.bottom };
                    FillRect(hdcWnd, &bar, hBlack);
                }

                int scaledW = dst.right  - dst.left;
                int scaledH = dst.bottom - dst.top;

                SetStretchBltMode(hdcWnd, COLORONCOLOR);
                StretchBlt(hdcWnd, dst.left, dst.top, scaledW, scaledH,
                           g_hdcMem, 0, 0, srcW, srcH, SRCCOPY);
            }

            ReleaseDC(nullptr, hdcScreen);
            ReleaseDC(hWnd, hdcWnd);
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_MIRROR_REFRESH);
        FreeMirrorResources();
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

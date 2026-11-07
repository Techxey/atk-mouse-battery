/*
 * AtkMouseBattery - 托盘鼠标电量小工具 (ATK U2 PLUS NK / Compx 2.4G 接收器)
 *
 * 纯 Win32 + HID，无 .NET / Python 运行时依赖。
 * 协议要点 (ATK/Compx，与开源 atk-tool 一致):
 *   - 厂商 HID 集合 usagePage=0xFF02，输入/输出报告长 17 字节
 *   - 报告 ID 0x08，其后 16 字节载荷，校验：16 字节之和的低 8 位 == 0x4D
 *   - 查询电量: 载荷 [0]=0x04，其余 0，末字节 0x49
 *   - 响应:    载荷 [0]=0x04，[5]=电量%，[7..8]=电压(mV，大端)
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0700
#endif

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <setupapi.h>
#include <dbt.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

/* ---------- 自带的 HID 声明（MinGW 缺 hidsdi.h / hidpi.h） ---------- */
typedef struct _HIDD_ATTRIBUTES {
    DWORD  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

typedef struct _HIDP_CAPS {
    USHORT Usage;
    USHORT UsagePage;
    USHORT InputReportByteLength;
    USHORT OutputReportByteLength;
    USHORT FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps;
    USHORT NumberInputValueCaps;
    USHORT NumberInputDataIndices;
    USHORT NumberOutputButtonCaps;
    USHORT NumberOutputValueCaps;
    USHORT NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps;
    USHORT NumberFeatureValueCaps;
    USHORT NumberFeatureDataIndices;
} HIDP_CAPS, *PHIDP_CAPS;

typedef void *PHIDP_PREPARSED_DATA;

__declspec(dllimport) BOOLEAN WINAPI HidD_GetHidGuid(LPGUID Guid);
__declspec(dllimport) BOOLEAN WINAPI HidD_GetAttributes(HANDLE h, PHIDD_ATTRIBUTES a);
__declspec(dllimport) BOOLEAN WINAPI HidD_GetPreparsedData(HANDLE h, PHIDP_PREPARSED_DATA *pp);
__declspec(dllimport) BOOLEAN WINAPI HidD_FreePreparsedData(PHIDP_PREPARSED_DATA pp);
__declspec(dllimport) LONG    WINAPI HidP_GetCaps(PHIDP_PREPARSED_DATA pp, PHIDP_CAPS caps);
__declspec(dllimport) BOOL    WINAPI AttachConsole(DWORD pid);
__declspec(dllimport) BOOL    WINAPI CancelIo(HANDLE h);

#ifndef ATTACH_PARENT_PROCESS
#define ATTACH_PARENT_PROCESS ((DWORD)-1)
#endif

/* ---------- 常量 ---------- */
#define APP_NAME        L"AtkMouseBattery"
#define APP_TITLE       L"ATK 鼠标电量"
#define MUTEX_NAME      L"Local\\AtkMouseBattery.SingleInstance"
#define VID_ATK         0x373B
#define PID_DONGLE      0x10C9
#define PID_WIRED       0x126C
#define CMD_PAGE        0xFF02
#define FRAME_LEN       17
#define REPORT_ID       0x08
#define OP_QUERY_BATT   0x04
#define INTERVAL_MS     40000     /* 40 秒查询一次，与接收器心跳同频 */
#define QUERY_TIMEOUT   1500
#define LOW_BATTERY_PCT 20
#define WM_TRAY         (WM_APP + 1)
#define WM_STATE        (WM_APP + 2)
#define TRAY_ID         1
#define IDM_REFRESH     1001
#define IDM_NOTIFY      1002
#define IDM_AUTOSTART   1003
#define IDM_EXIT        1004
#define REG_SETTINGS    L"Software\\AtkMouseBattery"
#define REG_RUN         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* ---------- 全局状态 ---------- */
static HANDLE   g_w = NULL;            /* 命令通道写句柄 */
static HANDLE   g_r = NULL;            /* 读取句柄 */
static HANDLE   g_reader = NULL;
static volatile LONG g_reader_quit = 0;
static HANDLE   g_wake = NULL;         /* 唤醒工作线程 */
static HANDLE   g_resp = NULL;         /* 收到电量响应 */
static volatile LONG g_quit = 0;
static volatile LONG g_rescan = 0;
static volatile LONG g_force = 0;
static volatile LONG g_online = 0;
static volatile LONG g_present = 0;
static volatile LONG g_wired = 0;
static volatile LONG g_pct = -1;
static volatile LONG g_mv = 0;
static volatile LONG g_read_error = 0;
static DWORD    g_last_query = 0;
static BOOL     g_notify_on = TRUE;
static BOOL     g_low_notified = FALSE;
static BOOL     g_first_update = TRUE;
static HWND     g_hwnd = NULL;
static HICON    g_icon = NULL;
static HICON    g_icon_prev = NULL;
static UINT     g_taskbar_created = 0;
static wchar_t  g_exe[MAX_PATH] = {0};


/* ---------- 小工具 ---------- */
#define ARGB(a, r, g, b) ((DWORD)(((DWORD)(a) << 24) | ((DWORD)(r) << 16) | ((DWORD)(g) << 8) | (DWORD)(b)))

static void set_px(DWORD *px, int S, int x, int y, DWORD argb)
{
    if (x < 0 || y < 0 || x >= S || y >= S) return;
    px[y * S + x] = argb;
}

/* 画电池图标：格数按电量比例；未知时只画空壳；有线时蓝色 */
static void draw_battery_pixels(DWORD *px, int S, int pct, BOOL wired)
{
    int m, bx0, by0, bx1, by1, nubW, nubH, nx0, ny0;
    DWORD edge, fill;
    int x, y, ix0, ix1, iy0, iy1, span, fillw;

    ZeroMemory(px, (size_t)S * S * 4);

    m = (S >= 24) ? 2 : 1;
    bx0 = m; by0 = S / 3; bx1 = S - m - 2; by1 = S - S / 3;
    if (by1 <= by0 + 2) by1 = by0 + 3;
    nubW = (S >= 24) ? 3 : 2;
    nubH = (by1 - by0) / 3;
    if (nubH < 2) nubH = 2;
    nx0 = bx1 + 1;
    ny0 = (by0 + by1) / 2 - nubH / 2;

    edge = ARGB(255, 170, 170, 170);
    if (wired)                 fill = ARGB(255, 90, 170, 255);
    else if (pct < 0)          fill = 0;
    else if (pct <= LOW_BATTERY_PCT) fill = ARGB(255, 235, 80, 80);
    else if (pct < 60)         fill = ARGB(255, 240, 190, 60);
    else                       fill = ARGB(255, 80, 200, 90);

    for (y = by0; y <= by1; y++)
        for (x = bx0; x <= bx1; x++)
            if (x == bx0 || x == bx1 || y == by0 || y == by1)
                set_px(px, S, x, y, edge);

    for (y = ny0; y < ny0 + nubH; y++)
        for (x = nx0; x < nx0 + nubW; x++)
            set_px(px, S, x, y, edge);

    ix0 = bx0 + 1; ix1 = bx1 - 1; iy0 = by0 + 1; iy1 = by1 - 1;
    span = ix1 - ix0 + 1;
    fillw = 0;
    if (pct >= 0) fillw = span * pct / 100;
    else if (wired) fillw = span * 60 / 100;
    if (fillw > span) fillw = span;
    for (y = iy0; y <= iy1; y++)
        for (x = ix0; x < ix0 + fillw; x++)
            set_px(px, S, x, y, fill);
}

static HICON make_icon(int pct, BOOL wired)
{
    int S = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER bi;
    HDC hdc;
    void *bits = NULL;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON icon;

    if (S < 16) S = 16;
    if (S > 64) S = 32;

    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    hdc = GetDC(NULL);
    color = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!color || !bits) return NULL;

    draw_battery_pixels((DWORD *)bits, S, pct, wired);

    mask = CreateBitmap(S, S, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

static void tooltip_text(wchar_t *out, int cch)
{
    LONG pct = g_pct;
    LONG mv = g_mv;
    int vi = (int)(mv / 1000);
    int vf = (int)((mv % 1000) / 10);
    if (g_first_update) {
        wcscpy(out, L"ATK 鼠标 · 正在读取…");
        if (cch > 0) out[cch - 1] = 0;
        return;
    }
    if (g_wired) {
        if (pct >= 0) wsprintfW(out, L"ATK 鼠标 · 有线/充电中 · %d%% · %d.%02dV", (int)pct, vi, vf);
        else wsprintfW(out, L"ATK 鼠标 · 有线/充电中");
    } else if (g_online && pct >= 0) {
        wsprintfW(out, L"ATK 鼠标 · 电量 %d%% · %d.%02dV · 已连接(2.4G)", (int)pct, vi, vf);
    } else if (g_present) {
        wsprintfW(out, L"ATK 鼠标 · 未连接（鼠标关机或超出范围）");
    } else {
        wsprintfW(out, L"ATK 鼠标 · 接收器未插入");
    }
    if (cch > 0) out[cch - 1] = 0;
}

static void tray_add_or_update(BOOL modify)
{
    NOTIFYICONDATAW nid;
    HICON fresh = make_icon((g_online || g_wired) ? (int)g_pct : -1, g_wired != 0);

    if (fresh) {
        if (g_icon_prev) DestroyIcon(g_icon_prev);
        g_icon_prev = g_icon;
        g_icon = fresh;
    }

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    tooltip_text(nid.szTip, (int)(sizeof(nid.szTip) / sizeof(nid.szTip[0])));

    Shell_NotifyIconW(modify ? NIM_MODIFY : NIM_ADD, &nid);
}

static void balloon_low_battery(int pct)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 10000;
    wcscpy(nid.szInfoTitle, L"鼠标电量低");
    wsprintfW(nid.szInfo, L"剩余 %d%%，建议尽快充电。", pct);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ---------- HID 连接 ---------- */
/* 读取线程：overlapped + 200ms 超时轮询，退出时不会被挂起的 ReadFile 卡住 */
static DWORD WINAPI reader_thread(LPVOID param)
{
    HANDLE h = (HANDLE)param;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    unsigned char buf[64];

    if (!ev) return 0;
    while (!InterlockedCompareExchange(&g_reader_quit, 1, 1)) {
        OVERLAPPED ov;
        DWORD n = 0, wr, err;
        BOOL ok;

        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);
        ok = ReadFile(h, buf, FRAME_LEN, &n, &ov);
        err = GetLastError();
        if (!ok) {
            if (err != ERROR_IO_PENDING) break;
            wr = WaitForSingleObject(ev, 200);
            if (wr == WAIT_TIMEOUT) {
                CancelIo(h);
                WaitForSingleObject(ev, 500);
                GetOverlappedResult(h, &ov, &n, FALSE);
                continue;
            }
            if (wr != WAIT_OBJECT_0) break;
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) break;
        }
        if (n >= FRAME_LEN && buf[0] == REPORT_ID) {
            int i, sum = 0;
            for (i = 1; i < FRAME_LEN; i++) sum += buf[i];
            if ((sum & 0xFF) == 0x4D && buf[1] == OP_QUERY_BATT) {
                int pct = buf[6];                    /* payload[5] */
                int mv = (buf[8] << 8) | buf[9];     /* payload[7..8] */
                if (pct >= 0 && pct <= 100) {
                    InterlockedExchange(&g_pct, pct);
                    InterlockedExchange(&g_mv, mv);
                    InterlockedExchange(&g_online, 1);
                    if (g_resp) SetEvent(g_resp);
                }
            }
        }
    }
    CloseHandle(ev);
    return 0;
}

static void close_device(void)
{
    InterlockedExchange(&g_reader_quit, 1);
    if (g_reader) {
        WaitForSingleObject(g_reader, 800);
        CloseHandle(g_reader);
        g_reader = NULL;
    }
    if (g_r) { CloseHandle(g_r); g_r = NULL; }
    if (g_w) { CloseHandle(g_w); g_w = NULL; }
    InterlockedExchange(&g_online, 0);
    InterlockedExchange(&g_read_error, 0);
}

static BOOL open_channel(const wchar_t *path, LONG wired, BOOL *opened)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hr;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    hr = CreateFileW(path, GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED, NULL);
    if (hr == INVALID_HANDLE_VALUE) { CloseHandle(h); return FALSE; }
    InterlockedExchange(&g_wired, wired);
    InterlockedExchange(&g_reader_quit, 0);
    g_w = h;
    g_r = hr;
    g_reader = CreateThread(NULL, 0, reader_thread, hr, 0, NULL);
    *opened = TRUE;
    return TRUE;
}

static BOOL try_open_device(void)
{
    GUID hidGuid;
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA did;
    DWORD i;
    wchar_t wiredPath[600] = {0};
    wchar_t donglePath[600] = {0};
    BOOL found = FALSE;
    BOOL opened = FALSE;

    HidD_GetHidGuid(&hidGuid);
    devs = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devs != INVALID_HANDLE_VALUE) {
        did.cbSize = sizeof(did);
        for (i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &hidGuid, i, &did); i++) {
            DWORD need = 0;
            BYTE *raw;
            SP_DEVICE_INTERFACE_DETAIL_DATA_W *det;
            HANDLE h;
            HIDD_ATTRIBUTES attr;

            SetupDiGetDeviceInterfaceDetailW(devs, &did, NULL, 0, &need, NULL);  /* 返回 FALSE+ERROR_INSUFFICIENT_BUFFER 属正常 */
            if (need == 0) {
                continue;
            }
            raw = (BYTE *)HeapAlloc(GetProcessHeap(), 0, need);
            if (!raw) continue;
            det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)raw;
#if defined(_WIN64)
            det->cbSize = 8;
#else
            det->cbSize = 6;
#endif
            if (!SetupDiGetDeviceInterfaceDetailW(devs, &did, det, need, NULL, NULL)) {
                HeapFree(GetProcessHeap(), 0, raw);
                continue;
            }

            h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                h = CreateFileW(det->DevicePath, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

            if (h != INVALID_HANDLE_VALUE) {
                attr.Size = sizeof(attr);
                if (HidD_GetAttributes(h, &attr) &&
                    attr.VendorID == VID_ATK &&
                    (attr.ProductID == PID_DONGLE || attr.ProductID == PID_WIRED)) {
                    PHIDP_PREPARSED_DATA pp = NULL;
                    HIDP_CAPS caps;
                    if (HidD_GetPreparsedData(h, &pp)) {
                        LONG st = HidP_GetCaps(pp, &caps);
                        HidD_FreePreparsedData(pp);
                        if (st == 0x00110000 && caps.UsagePage == CMD_PAGE &&
                            caps.InputReportByteLength == FRAME_LEN &&
                            caps.OutputReportByteLength == FRAME_LEN) {
                            found = TRUE;
                            if (attr.ProductID == PID_WIRED)
                                lstrcpynW(wiredPath, det->DevicePath, 600);
                            else
                                lstrcpynW(donglePath, det->DevicePath, 600);
                        }
                    }
                }
                CloseHandle(h);
            }
            HeapFree(GetProcessHeap(), 0, raw);
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    InterlockedExchange(&g_present, found ? 1 : 0);
    if (wiredPath[0] && open_channel(wiredPath, 1, &opened)) return TRUE;
    if (donglePath[0] && open_channel(donglePath, 0, &opened)) return TRUE;
    return FALSE;
}

static BOOL query_battery(void)
{
    unsigned char f[FRAME_LEN];
    DWORD wr = 0;

    if (!g_w) return FALSE;
    ZeroMemory(f, sizeof(f));
    f[0] = REPORT_ID;
    f[1] = OP_QUERY_BATT;
    f[FRAME_LEN - 1] = 0x49;   /* 0x04 + 0x49 = 0x4D */

    if (g_resp) ResetEvent(g_resp);
    if (!WriteFile(g_w, f, FRAME_LEN, &wr, NULL) || wr != FRAME_LEN) return FALSE;
    if (!g_resp) return FALSE;
    return WaitForSingleObject(g_resp, QUERY_TIMEOUT) == WAIT_OBJECT_0;
}

/* 仅在状态变化时刷新托盘，避免无谓的 GDI 重绘 */
static void post_state(BOOL force)
{
    static LONG last_sig = -1;
    LONG sig = (g_present ? 1 : 0) | (g_wired ? 2 : 0) | (g_online ? 4 : 0) |
               (((g_pct + 1) & 0x1FF) << 4) | (((g_mv / 10) & 0xFFFF) << 13);
    if (force || sig != last_sig) {
        last_sig = sig;
        PostMessage(g_hwnd, WM_STATE, 0, 0);
    }
}

/* ---------- 工作线程 ---------- */
static DWORD WINAPI worker_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        DWORD now;
        LONG force;

        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

        if (InterlockedCompareExchange(&g_rescan, 0, 0)) {
            InterlockedExchange(&g_rescan, 0);
            close_device();
            g_last_query = 0;
            InterlockedExchange(&g_force, 1);
        }
        if (InterlockedCompareExchange(&g_read_error, 0, 0)) {
            close_device();
            g_last_query = 0;
            InterlockedExchange(&g_force, 1);
        }

        if (!g_w) {
            if (!try_open_device()) {
                post_state(FALSE);
                WaitForSingleObject(g_wake, 2000);
                continue;
            }
            g_last_query = 0;
            post_state(TRUE);
        }

        now = GetTickCount();
        force = InterlockedExchange(&g_force, 0);
        if (force || (now - g_last_query) >= INTERVAL_MS) {
            g_last_query = GetTickCount();
            if (!query_battery()) InterlockedExchange(&g_online, 0);
            post_state(FALSE);
        }
        WaitForSingleObject(g_wake, 400);
    }
    return 0;
}

/* ---------- 开机自启 / 设置 ---------- */
static BOOL autostart_is_on(void)
{
    HKEY k;
    wchar_t v[600] = {0};
    DWORD cb = sizeof(v), type = 0;
    BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, APP_NAME, NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS && type == REG_SZ)
            on = TRUE;
        RegCloseKey(k);
    }
    return on;
}

static void autostart_set(BOOL on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        if (on) {
            wchar_t cmd[MAX_PATH + 8];
            wsprintfW(cmd, L"\"%ls\"", g_exe);
            RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(k, APP_NAME);
        }
        RegCloseKey(k);
    }
}

static void load_settings(void)
{
    HKEY k;
    DWORD v = 1, cb = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"Notify", NULL, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS && type == REG_DWORD)
            g_notify_on = (v != 0);
        RegCloseKey(k);
    }
}

static void save_notify(void)
{
    HKEY k;
    DWORD v = g_notify_on ? 1 : 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SETTINGS, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"Notify", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

/* ---------- 托盘菜单 ---------- */
static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    if (!m) return;

    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"立即刷新");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_notify_on ? MF_CHECKED : 0), IDM_NOTIFY, L"低电量提醒（20%）");
    AppendMenuW(m, MF_STRING | (autostart_is_on() ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        tray_add_or_update(FALSE);
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            InterlockedExchange(&g_force, 1);
            if (g_wake) SetEvent(g_wake);
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            show_menu();
        }
        return 0;

    case WM_STATE:
        g_first_update = FALSE;
        tray_add_or_update(TRUE);
        if (g_notify_on && !g_wired && g_online && g_pct >= 0 &&
            g_pct <= LOW_BATTERY_PCT && !g_low_notified) {
            balloon_low_battery((int)g_pct);
            g_low_notified = TRUE;
        }
        if (!g_wired && g_online && g_pct > LOW_BATTERY_PCT + 5)
            g_low_notified = FALSE;
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_REFRESH:
            InterlockedExchange(&g_force, 1);
            if (g_wake) SetEvent(g_wake);
            break;
        case IDM_NOTIFY:
            g_notify_on = !g_notify_on;
            save_notify();
            break;
        case IDM_AUTOSTART:
            autostart_set(!autostart_is_on());
            break;
        case IDM_EXIT:
        default:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DEVICECHANGE:
        InterlockedExchange(&g_rescan, 1);
        if (g_wake) SetEvent(g_wake);
        return 0;

    case WM_DESTROY: {
        NOTIFYICONDATAW nid;
        InterlockedExchange(&g_quit, 1);
        if (g_wake) SetEvent(g_wake);
        close_device();
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = TRAY_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (g_icon) { DestroyIcon(g_icon); g_icon = NULL; }
        if (g_icon_prev) { DestroyIcon(g_icon_prev); g_icon_prev = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    default:
        if (msg == g_taskbar_created) {
            tray_add_or_update(FALSE);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- 图标渲染自检：把 16/32px 图标合到三种背景上导出 BMP ---------- */
static void write_bmp32(const wchar_t *path, int w, int h, const DWORD *px)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE f;
    DWORD bw = 0;

    ZeroMemory(&fh, sizeof(fh));
    ZeroMemory(&ih, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;      /* 负数 = 自上而下 */
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)(w * h * 4);

    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, &fh, sizeof(fh), &bw, NULL);
    WriteFile(f, &ih, sizeof(ih), &bw, NULL);
    WriteFile(f, px, (DWORD)(w * h * 4), &bw, NULL);
    CloseHandle(f);
}

static void dump_icons(void)
{
    static const int sizes[2] = {16, 32};
    static const int pcts[5] = {45, 15, 80, -1, 45};
    static const BOOL wireds[5] = {FALSE, FALSE, FALSE, FALSE, TRUE};
    DWORD bgs[3] = {ARGB(255, 32, 32, 32), ARGB(255, 240, 240, 240), ARGB(255, 0, 120, 215)};
    int si, pi, b;

    for (si = 0; si < 2; si++) {
        int S = sizes[si];
        DWORD *px = (DWORD *)malloc((size_t)S * S * 4);
        DWORD *canvas = (DWORD *)malloc((size_t)S * 3 * S * 4);
        if (!px || !canvas) { free(px); free(canvas); return; }
        for (pi = 0; pi < 5; pi++) {
            int x, y;
            wchar_t name[64];
            draw_battery_pixels(px, S, pcts[pi], wireds[pi]);
            for (b = 0; b < 3; b++) {
                for (y = 0; y < S; y++) {
                    for (x = 0; x < S; x++) {
                        DWORD src = px[y * S + x];
                        DWORD dst = ((src >> 24) != 0)
                            ? ARGB(255, (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF)
                            : bgs[b];
                        canvas[y * (S * 3) + b * S + x] = dst;
                    }
                }
            }
            wsprintfW(name, L"icon_%d_%d.bmp", S, pi);
            write_bmp32(name, S * 3, S, canvas);
        }
        free(px);
        free(canvas);
    }
}

/* ---------- 命令行探测模式（用于验证协议） ---------- */
static int probe_mode(void)
{
    int ok = 0;
    char narrow[300];
    wchar_t tmp[MAX_PATH], path[MAX_PATH + 40];
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_resp = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (try_open_device() && query_battery()) ok = 1;

    g_first_update = FALSE;
    sprintf(narrow, "present=%d wired=%d ok=%d pct=%d mv=%d\r\n",
            (int)g_present, (int)g_wired, ok, (int)g_pct, (int)g_mv);

    GetTempPathW(MAX_PATH, tmp);
    wsprintfW(path, L"%lsatk-probe.txt", tmp);
    {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD bw = 0;
            WriteFile(f, narrow, (DWORD)strlen(narrow), &bw, NULL);
            CloseHandle(f);
        }
    }
    {
        wchar_t tip[260];
        char tip8[900];
        int len;
        tooltip_text(tip, 260);
        len = WideCharToMultiByte(CP_UTF8, 0, tip, -1, tip8, (int)sizeof(tip8), NULL, NULL);
        if (len > 1) {
            HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD bw = 0;
                WriteFile(f, "tip=", 4, &bw, NULL);
                WriteFile(f, tip8, (DWORD)(len - 1), &bw, NULL);
                WriteFile(f, "\r\n", 2, &bw, NULL);
                CloseHandle(f);
            }
        }
    }
    close_device();
    ExitProcess(ok ? 0 : 2);
}
/* ---------- 入口 ---------- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
    MSG msg;
    HANDLE mutex;
    (void)hPrev; (void)cmdLine; (void)show;

    {   /* 老 MinGW 导入库没有 SetProcessDPIAware，动态取 */ 
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            typedef BOOL (WINAPI *PFN_DPIAWARE)(void);
            PFN_DPIAWARE pfn = (PFN_DPIAWARE)GetProcAddress(u32, "SetProcessDPIAware");
            if (pfn) pfn();
        }
    }
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);

    if (wcsstr(GetCommandLineW(), L"--dump-icons")) { dump_icons(); return 0; }
    if (wcsstr(GetCommandLineW(), L"--probe")) return probe_mode();

    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    load_settings();
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_resp = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"AtkMouseBatteryWnd";
        RegisterClassExW(&wc);
        CreateWindowExW(0, L"AtkMouseBatteryWnd", APP_TITLE, 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    }

    CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}

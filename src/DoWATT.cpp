// SPDX-License-Identifier: GPL-3.0-only
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <initguid.h>
#include <wbemidl.h>
#include "estimate.h"
#include <version.h>

// No CRT or third-party runtime. These routines are compiler intrinsics' fallback.
extern "C" void *memset(void *p, int c, size_t n) {
    volatile unsigned char *b = (volatile unsigned char *)p;
    while (n--) *b++ = (unsigned char)c;
    return p;
}
extern "C" void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    while (n--) *a++ = *b++;
    return d;
}

static const wchar_t CLASS_NAME[] = L"DoWATT.Overlay.1";
static const UINT TRAY_MESSAGE = WM_APP + 1;
static const UINT EXIT_ID = 1, RAM_ID = 2, STARTUP_ID = 3, ABOUT_ID = 4, OPACITY_ID = 10;
static BOOL showRam = TRUE;
static DWORD opacity = 92;
static const wchar_t SETTINGS_KEY[] = L"Software\\DoWATT";
static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static void loadSettings(void) {
    DWORD size = sizeof(DWORD), value;
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"ShowRAM", RRF_RT_REG_DWORD, 0, &value, &size) == ERROR_SUCCESS) showRam = value != 0;
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Opacity", RRF_RT_REG_DWORD, 0, &value, &size) == ERROR_SUCCESS && value >= 25 && value <= 100) opacity = value;
}
static void saveSettings(void) {
    HKEY key; DWORD value = showRam ? 1 : 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, 0, 0, KEY_SET_VALUE, 0, &key, 0) != ERROR_SUCCESS) {
        MessageBoxW(0, L"Your settings could not be saved.", L"DoWATT", MB_OK | MB_ICONERROR); return;
    }
    LONG ramResult = RegSetValueExW(key, L"ShowRAM", 0, REG_DWORD, (BYTE *)&value, sizeof(value));
    LONG opacityResult = RegSetValueExW(key, L"Opacity", 0, REG_DWORD, (BYTE *)&opacity, sizeof(opacity)); RegCloseKey(key);
    if (ramResult != ERROR_SUCCESS || opacityResult != ERROR_SUCCESS)
        MessageBoxW(0, L"Your settings could not be saved.", L"DoWATT", MB_OK | MB_ICONERROR);
}
static BOOL startupEnabled(void) {
    wchar_t *buffer = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, 3 * 32768 * sizeof(wchar_t));
    if (!buffer) return FALSE;
    wchar_t *saved = buffer, *path = buffer + 32768, *expected = buffer + 65536;
    DWORD size = 32768 * sizeof(wchar_t);
    DWORD n = GetModuleFileNameW(0, path, 32768);
    if (!n || n >= 32765) { HeapFree(GetProcessHeap(), 0, buffer); return FALSE; }
    expected[0] = L'"'; lstrcpyW(expected + 1, path); expected[n + 1] = L'"'; expected[n + 2] = 0;
    BOOL enabled = RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"DoWATT", RRF_RT_REG_SZ, 0, saved, &size) == ERROR_SUCCESS && !lstrcmpiW(saved, expected);
    HeapFree(GetProcessHeap(), 0, buffer); return enabled;
}
static void toggleStartup(void) {
    HKEY key; LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, 0, 0, KEY_SET_VALUE, 0, &key, 0);
    if (result == ERROR_SUCCESS) {
        if (startupEnabled()) result = RegDeleteValueW(key, L"DoWATT");
        else {
            wchar_t *buffer = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, 2 * 32768 * sizeof(wchar_t));
            if (!buffer) { RegCloseKey(key); MessageBoxW(0, L"Not enough memory to update the sign-in setting.", L"DoWATT", MB_OK | MB_ICONERROR); return; }
            wchar_t *path = buffer, *command = buffer + 32768; DWORD n = GetModuleFileNameW(0, path, 32768);
            if (!n || n >= 32765) result = ERROR_INSUFFICIENT_BUFFER;
            else { command[0] = L'"'; lstrcpyW(command + 1, path); command[n + 1] = L'"'; command[n + 2] = 0;
                result = RegSetValueExW(key, L"DoWATT", 0, REG_SZ, (BYTE *)command, (n + 3) * sizeof(wchar_t)); }
            HeapFree(GetProcessHeap(), 0, buffer);
        }
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS) MessageBoxW(0, L"The sign-in setting could not be changed.", L"DoWATT", MB_OK | MB_ICONERROR);
}
static HWND window;
static HANDLE stopEvent;
static HFONT font;
static HBRUSH background;
static HICON icon;
static UINT taskbarCreated;
static UINT dpi = 96;
static BOOL trayPresent;
static wchar_t ramText[80] = L"RAM N/A";
static wchar_t powerText[80] = L"W N/A";
static int widgetWidth = 230, widgetHeight = 22;
static int powerWidth, dividerWidth;
static SRWLOCK powerLock = SRWLOCK_INIT;
static struct { LONG value; BOOL estimated; ULONGLONG timestamp; } powerState = {-1, TRUE, 0};
static void publishPower(LONG value, BOOL estimated) {
    AcquireSRWLockExclusive(&powerLock);
    powerState.value = value; powerState.estimated = estimated; powerState.timestamp = GetTickCount64();
    ReleaseSRWLockExclusive(&powerLock);
}

static wchar_t *number(wchar_t *p, ULONGLONG value) {
    wchar_t digits[24];
    UINT n = 0;
    do { digits[n++] = (wchar_t)(L'0' + value % 10); value /= 10; } while (value);
    while (n) *p++ = digits[--n];
    *p = 0;
    return p;
}
static wchar_t *append(wchar_t *p, const wchar_t *s) {
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}
static wchar_t *decimal(wchar_t *p, ULONGLONG tenths) {
    p = number(p, tenths / 10); *p++ = L'.'; *p++ = (wchar_t)(L'0' + tenths % 10); *p = 0;
    return p;
}
static BOOL memoryText(wchar_t *s) {
    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m) || !m.ullTotalPhys || m.ullAvailPhys > m.ullTotalPhys) {
        lstrcpyW(s, L"RAM N/A"); return FALSE;
    }
    wchar_t *p = append(s, L"RAM ");
    p = decimal(p, ((m.ullTotalPhys - m.ullAvailPhys) * 10 + (1ULL << 29)) >> 30);
    p = append(p, L"/");
    p = decimal(p, (m.ullTotalPhys * 10 + (1ULL << 29)) >> 30);
    append(p, L" GiB");
    return TRUE;
}
static void wattsText(wchar_t *s) {
    AcquireSRWLockShared(&powerLock);
    LONG value = powerState.value; BOOL estimated = powerState.estimated; ULONGLONG stamp = powerState.timestamp;
    ReleaseSRWLockShared(&powerLock);
    if (value < 0 || !stamp || GetTickCount64() - stamp > 10000) { lstrcpyW(s, L"W N/A"); return; }
    if (estimated) {
        // Five-watt rounding avoids implying sensor precision for a model.
        append(number(append(s, L"\x2248"), ((ULONGLONG)value + 25) / 50 * 5), L" W");
    } else append(decimal(s, (ULONGLONG)value), L" W");
}

static HRESULT query(IWbemServices *svc, const wchar_t *text, IEnumWbemClassObject **out) {
    BSTR language = SysAllocString(L"WQL"), q = SysAllocString(text);
    HRESULT hr = E_OUTOFMEMORY;
    if (language && q) hr = svc->ExecQuery(language, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, 0, out);
    SysFreeString(language); SysFreeString(q); return hr;
}
static BOOL integer(IWbemClassObject *object, const wchar_t *key, LONGLONG *result) {
    VARIANT v; VariantInit(&v);
    BOOL ok = FALSE;
    if (SUCCEEDED(object->Get(key, 0, &v, 0, 0))) {
        switch (v.vt) {
        case VT_I4: *result = v.lVal; ok = TRUE; break;
        case VT_UI4: *result = v.ulVal; ok = TRUE; break;
        case VT_I2: *result = v.iVal; ok = TRUE; break;
        case VT_UI2: *result = v.uiVal; ok = TRUE; break;
        case VT_I8: *result = v.llVal; ok = TRUE; break;
        case VT_UI8: if (v.ullVal <= 0x7fffffffffffffffULL) { *result = (LONGLONG)v.ullVal; ok = TRUE; } break;
        case VT_BSTR: {
            // WMI encodes uint64 values as decimal BSTRs. Reject malformed/overflow values.
            const wchar_t *p = v.bstrVal; ULONGLONG value = 0;
            if (p && *p) {
                ok = TRUE;
                while (*p) {
                    if (*p < L'0' || *p > L'9' || value > (0x7fffffffffffffffULL - (*p - L'0')) / 10) { ok = FALSE; break; }
                    value = value * 10 + (*p++ - L'0');
                }
                if (ok) *result = (LONGLONG)value;
            }
            break;
        }
        }
    }
    VariantClear(&v); return ok;
}
static BOOL validWatts(IWbemClassObject *meter, LONG *tenths) {
    LONGLONG type, units, modifier, reading;
    // CIM BaseUnits 7 = watts. Only input meters, never output/component estimates.
    if (!integer(meter, L"MeterType", &type) || type != 0 ||
        !integer(meter, L"BaseUnits", &units) || units != 7 ||
        !integer(meter, L"UnitModifier", &modifier) || modifier < -9 || modifier > 6 ||
        !integer(meter, L"CurrentReading", &reading) || reading < 0) return FALSE;
    double scaled = (double)reading * 10.0;
    while (modifier > 0) { scaled *= 10.0; --modifier; }
    while (modifier < 0) { scaled /= 10.0; ++modifier; }
    if (scaled < 0 || scaled > 10000000.0) return FALSE;
    *tenths = (LONG)(scaled + 0.5); return TRUE;
}
static BSTR findSystemMeter(IWbemServices *svc) {
    IEnumWbemClassObject *meters = 0;
    if (FAILED(query(svc, L"SELECT * FROM Win32_PowerMeter", &meters))) return 0;
    BSTR chosen = 0; UINT count = 0; BOOL complete = FALSE;
    for (UINT i = 0; i < 32 && WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT; ++i) {
        IWbemClassObject *meter = 0; ULONG fetched = 0;
        HRESULT hr = meters->Next(1500, 1, &meter, &fetched);
        if (!fetched) { complete = (hr == WBEM_S_FALSE); break; }
        if (FAILED(hr)) { if (meter) meter->Release(); break; }
        VARIANT path; VariantInit(&path);
        LONGLONG type;
        if (integer(meter, L"MeterType", &type) && type == 0 &&
            SUCCEEDED(meter->Get(L"__PATH", 0, &path, 0, 0)) && path.vt == VT_BSTR && SysStringLen(path.bstrVal) < 700) {
            wchar_t q[900];
            wchar_t *p = append(q, L"ASSOCIATORS OF {");
            p = append(p, path.bstrVal); append(p, L"} WHERE ResultClass=Win32_ComputerSystem");
            IEnumWbemClassObject *systems = 0;
            if (SUCCEEDED(query(svc, q, &systems))) {
                IWbemClassObject *system = 0; ULONG found = 0;
                hr = systems->Next(1500, 1, &system, &found);
                if (SUCCEEDED(hr) && found) { ++count; if (!chosen) chosen = SysAllocString(path.bstrVal); }
                if (system) system->Release();
                systems->Release();
            }
        }
        VariantClear(&path); meter->Release();
    }
    meters->Release();
    // Multiple whole-system meters are ambiguous; never add them together.
    if (!complete || count != 1) { SysFreeString(chosen); chosen = 0; }
    return chosen;
}
static DWORD WINAPI powerWorker(void *) {
    HRESULT init = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(init)) return 0;
    CoInitializeSecurity(0, -1, 0, 0, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, 0, EOAC_NONE, 0);
    IWbemLocator *locator = 0; IWbemServices *svc = 0; BSTR path = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void **)&locator))) {
        BSTR ns = SysAllocString(L"ROOT\\CIMV2\\power");
        HRESULT hr = ns ? locator->ConnectServer(ns, 0, 0, 0, WBEM_FLAG_CONNECT_USE_MAX_WAIT, 0, 0, &svc) : E_OUTOFMEMORY;
        SysFreeString(ns);
        if (SUCCEEDED(hr) && SUCCEEDED(CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, 0,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, 0, EOAC_NONE))) path = findSystemMeter(svc);
    }
    EstimateInitialize();
    // Inventory and meter discovery happen once. Each tick samples lightweight APIs.
    do {
        IWbemClassObject *meter = 0; LONG watts = -1;
        HRESULT hr = path ? svc->GetObject(path, 0, 0, &meter, 0) : E_FAIL;
        if (SUCCEEDED(hr) && validWatts(meter, &watts)) {
            publishPower(watts, FALSE);
        } else {
            EstimateSample sample = EstimateRead();
            publishPower(sample.ready ? sample.wattsTenths : -1, TRUE);
        }
        if (meter) meter->Release();
    } while (WaitForSingleObject(stopEvent, 2000) == WAIT_TIMEOUT);
    EstimateShutdown();
    SysFreeString(path);
    if (svc) svc->Release(); if (locator) locator->Release();
    CoUninitialize(); return 0;
}

static int px(int n) { return MulDiv(n, (int)dpi, 96); }
static void place(void) {
    POINT origin = {0, 0};
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    int width = widgetWidth;
    if (width > mi.rcMonitor.right - mi.rcMonitor.left) width = mi.rcMonitor.right - mi.rcMonitor.left;
    SetWindowPos(window, HWND_TOPMOST, mi.rcMonitor.right - width, mi.rcMonitor.top,
        width, widgetHeight, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
static void layout(void) {
    HDC dc = GetDC(window); if (!dc) return;
    HFONT old = (HFONT)SelectObject(dc, font);
    SIZE power = {}, ram = {}, separator = {};
    GetTextExtentPoint32W(dc, powerText, lstrlenW(powerText), &power);
    GetTextExtentPoint32W(dc, ramText, lstrlenW(ramText), &ram);
    GetTextExtentPoint32W(dc, L"  |  ", 5, &separator);
    SelectObject(dc, old); ReleaseDC(window, dc);
    powerWidth = power.cx; dividerWidth = separator.cx;
    int width = px(10) + power.cx + (showRam ? separator.cx + ram.cx : 0);
    int height = power.cy > ram.cy ? power.cy : ram.cy;
    height += px(4);
    if (width != widgetWidth || height != widgetHeight) {
        widgetWidth = width; widgetHeight = height;
        // The panel touches both screen edges. No rounded corner creates a gap.
        SetWindowRgn(window, 0, FALSE); place();
    }
}
static void updateFont(void) {
    UINT current = GetDpiForWindow(window); if (current) dpi = current;
    HFONT replacement = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (replacement) { if (font) DeleteObject(font); font = replacement; }
    layout(); place();
}
static HICON makeIcon(void) {
    UINT systemDpi = GetDpiForSystem();
    HICON embedded = (HICON)LoadImageW(GetModuleHandleW(0), MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, systemDpi), GetSystemMetricsForDpi(SM_CYSMICON, systemDpi), 0);
    if (embedded) return embedded;
    HDC screen = GetDC(0), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 32, 32);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(dc, bitmap);
    RECT r = {0, 0, 32, 32}; FillRect(dc, &r, background);
    // Original pulse monogram, drawn from project-owned geometry; no font or stock logo.
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(128, 223, 182));
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    POINT pulse[] = {{4,17},{9,17},{12,7},{17,25},{21,12},{24,17},{28,17}};
    Polyline(dc, pulse, 7);
    SelectObject(dc, oldPen); DeleteObject(pen); SelectObject(dc, oldBitmap);
    BYTE mask[128] = {};
    HBITMAP maskBitmap = CreateBitmap(32, 32, 1, 1, mask);
    ICONINFO info = {}; info.fIcon = TRUE; info.hbmColor = bitmap; info.hbmMask = maskBitmap;
    HICON result = CreateIconIndirect(&info);
    DeleteObject(maskBitmap); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(0, screen); return result;
}
static void addTray(void) {
    NOTIFYICONDATAW data = {}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data.uCallbackMessage = TRAY_MESSAGE; data.hIcon = icon;
    lstrcpyW(data.szTip, L"DoWATT " DoWATT_VERSION_W L" | RAM + system power\nApproximate (~): hardware model. Right-click: Settings.");
    trayPresent = Shell_NotifyIconW(NIM_ADD, &data);
    if (trayPresent) { data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data); }
}
static void removeTray(void) {
    NOTIFYICONDATAW data = {}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data); trayPresent = FALSE;
}
static void menu(void) {
    HMENU m = CreatePopupMenu(), transparency = CreatePopupMenu(); if (!m || !transparency) { if(m) DestroyMenu(m); if(transparency) DestroyMenu(transparency); return; }
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"DoWATT v1.0");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING | (showRam ? MF_CHECKED : 0), RAM_ID, L"Show RAM usage");
    AppendMenuW(m, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), STARTUP_ID, L"Start at sign-in");
    const DWORD levels[] = {100, 92, 75, 50, 25};
    const wchar_t *labels[] = {L"Opaque", L"Subtle (8%)", L"25%", L"50%", L"75%"};
    for (UINT i = 0; i < 5; ++i) AppendMenuW(transparency, MF_STRING | (opacity == levels[i] ? MF_CHECKED : 0), OPACITY_ID + i, labels[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)transparency, L"Background transparency");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING, ABOUT_ID, L"About DoWATT");
    AppendMenuW(m, MF_STRING, EXIT_ID, L"Exit");
    POINT p; GetCursorPos(&p); SetForegroundWindow(window);
    UINT selected = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, window, 0);
    PostMessageW(window, WM_NULL, 0, 0); DestroyMenu(m);
    if (selected) SendMessageW(window, WM_COMMAND, selected, 0);
}
static void render(HDC dc, HWND h, BOOL mask = FALSE) {
    RECT r; GetClientRect(h, &r); FillRect(dc, &r, mask ? (HBRUSH)GetStockObject(BLACK_BRUSH) : background);
    HFONT old = (HFONT)SelectObject(dc, font); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, mask ? RGB(255,255,255) : RGB(128, 223, 182));
    RECT line = {px(5), 0, px(5) + powerWidth, r.bottom};
    DrawTextW(dc, powerText, -1, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (showRam) {
    line.left = line.right; line.right += dividerWidth; SetTextColor(dc, mask ? RGB(255,255,255) : RGB(128, 139, 145));
    DrawTextW(dc, L"  |  ", 5, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    line.left = line.right; line.right = r.right - px(5); SetTextColor(dc, mask ? RGB(255,255,255) : RGB(244, 246, 248));
    DrawTextW(dc, ramText, -1, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}
static BOOL present(void) {
    RECT rect; GetWindowRect(window, &rect);
    SIZE size = {rect.right - rect.left, rect.bottom - rect.top};
    if (size.cx <= 0 || size.cy <= 0) return FALSE;
    HDC screen = GetDC(0), colorDC = CreateCompatibleDC(screen), maskDC = CreateCompatibleDC(screen);
    BITMAPINFO info = {}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    DWORD *colors = 0, *coverage = 0;
    HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void **)&colors, 0, 0);
    HBITMAP mask = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void **)&coverage, 0, 0);
    BOOL ok = FALSE;
    if (colorDC && maskDC && color && mask) {
        HBITMAP oldColor = (HBITMAP)SelectObject(colorDC, color), oldMask = (HBITMAP)SelectObject(maskDC, mask);
        render(colorDC, window); render(maskDC, window, TRUE); GdiFlush();
        UINT backgroundAlpha = opacity * 255 / 100;
        for (LONG i = 0; i < size.cx * size.cy; ++i) {
            UINT ink = coverage[i] & 255;
            UINT lost = (255 - ink) * (255 - backgroundAlpha) / 255;
            UINT alpha = 255 - lost;
            UINT blue = colors[i] & 255, green = (colors[i] >> 8) & 255, red = (colors[i] >> 16) & 255;
            blue -= blue >= 30 * lost / 255 ? 30 * lost / 255 : blue;
            green -= green >= 26 * lost / 255 ? 26 * lost / 255 : green;
            red -= red >= 22 * lost / 255 ? 22 * lost / 255 : red;
            colors[i] = (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
        POINT target = {rect.left, rect.top}, source = {0,0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ok = UpdateLayeredWindow(window, screen, &target, &size, colorDC, &source, 0, &blend, ULW_ALPHA);
        SelectObject(colorDC, oldColor); SelectObject(maskDC, oldMask);
    }
    if(color) DeleteObject(color); if(mask) DeleteObject(mask);
    if(colorDC) DeleteDC(colorDC); if(maskDC) DeleteDC(maskDC); if(screen) ReleaseDC(0, screen);
    return ok;
}
static LRESULT CALLBACK procedure(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message == taskbarCreated && taskbarCreated) { trayPresent = FALSE; addTray(); return 0; }
    switch (message) {
    case WM_TIMER: {
        wchar_t ram[80], watts[80]; memoryText(ram); wattsText(watts);
        if (lstrcmpW(ram, ramText) || lstrcmpW(watts, powerText)) {
            lstrcpyW(ramText, ram); lstrcpyW(powerText, watts); layout(); InvalidateRect(h, 0, FALSE);
        }
        place(); if (!trayPresent) addTray(); return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc = BeginPaint(h, &paint);
        (void)dc; EndPaint(h, &paint); present(); return 0;
    }
    case WM_PRINT: if (l & PRF_CLIENT) render((HDC)w, h); return 0;
    case WM_PRINTCLIENT: render((HDC)w, h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DPICHANGED: updateFont(); InvalidateRect(h, 0, FALSE); return 0;
    case WM_DISPLAYCHANGE: updateFont(); return 0;
    case WM_SETTINGCHANGE: if (w == SPI_SETWORKAREA) place(); return 0;
    case WM_DEVICECHANGE: EstimateRequestRefresh(); return 0;
    case WM_POWERBROADCAST: if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) EstimateRequestRefresh(); return TRUE;
    case TRAY_MESSAGE:
        if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) menu();
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == EXIT_ID) DestroyWindow(h);
        else if (LOWORD(w) == ABOUT_ID) MessageBoxW(h,
            L"DoWATT v1.0\nCopyright (c) 2026 Dogukan Sahil\n\nFree software under GNU GPL v3.0 with NVML linking permission.\nYou may modify and redistribute under that license.\nABSOLUTELY NO WARRANTY. See LICENSE and NVML-EXCEPTION.txt in the source package.\n\nPower values marked ~ are estimates, not wall-meter readings.",
            L"About DoWATT", MB_OK | MB_ICONINFORMATION);
        else if (LOWORD(w) == STARTUP_ID) toggleStartup();
        else if (LOWORD(w) == RAM_ID) { showRam = !showRam; saveSettings(); layout(); InvalidateRect(h, 0, FALSE); }
        else if (LOWORD(w) >= OPACITY_ID && LOWORD(w) < OPACITY_ID + 5) {
            const DWORD levels[] = {100, 92, 75, 50, 25}; opacity = levels[LOWORD(w) - OPACITY_ID]; saveSettings();
            present();
        }
        return 0;
    case WM_DESTROY: KillTimer(h, 1); removeTray(); SetEvent(stopEvent); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, message, w, l);
}

static const wchar_t *reportPath;
static DWORD WINAPI reportWorker(void *) {
    if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED))) return 1;
    CoInitializeSecurity(0, -1, 0, 0, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, 0, EOAC_NONE, 0);
    EstimateInitialize(); Sleep(150); EstimateRead(); Sleep(2200); EstimateRead();
    BOOL written = EstimateWriteReport(reportPath);
    EstimateShutdown(); CoUninitialize(); return written ? 0 : 1;
}

extern "C" void WINAPI entry(void) {
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 3 && !lstrcmpW(argv[1], L"--report")) {
        reportPath = argv[2];
        HANDLE worker = CreateThread(0, 0, reportWorker, 0, 0, 0);
        DWORD code = 1;
        if (worker && WaitForSingleObject(worker, 15000) == WAIT_OBJECT_0) GetExitCodeThread(worker, &code);
        if (worker) CloseHandle(worker);
        // Keep argv alive if a provider timed out: ExitProcess stops that thread.
        ExitProcess(code);
    }
    if (argv && argc == 2 && !lstrcmpW(argv[1], L"--exit")) {
        HWND existing = FindWindowW(CLASS_NAME, 0); if (existing) PostMessageW(existing, WM_CLOSE, 0, 0);
        LocalFree(argv); ExitProcess(0);
    }
    if (argv) LocalFree(argv);
    HANDLE single = CreateMutexW(0, FALSE, L"Local\\DoWATT.Overlay.1");
    if (!single) ExitProcess(1);
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(single); ExitProcess(0); }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    stopEvent = CreateEventW(0, TRUE, FALSE, 0); if (!stopEvent) ExitProcess(1);
    background = CreateSolidBrush(RGB(22, 26, 30)); if (!background) ExitProcess(1);
    HINSTANCE instance = GetModuleHandleW(0);
    WNDCLASSEXW cls = {}; cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = procedure; cls.lpszClassName = CLASS_NAME; cls.hCursor = LoadCursorW(0, IDC_ARROW);
    if (!RegisterClassExW(&cls)) ExitProcess(1);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"DoWATT " DoWATT_VERSION_W, WS_POPUP, 0, 0, widgetWidth, widgetHeight, 0, 0, instance, 0);
    if (!window) ExitProcess(1);
    loadSettings();

    memoryText(ramText); updateFont(); icon = makeIcon(); addTray();
    HANDLE worker = CreateThread(0, 0, powerWorker, 0, 0, 0);
    if (worker) CloseHandle(worker);
    if (!SetTimer(window, 1, 2000, 0)) { DestroyWindow(window); ExitProcess(1); }
    present(); ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    MSG msg; int status;
    while ((status = GetMessageW(&msg, 0, 0, 0)) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    // ExitProcess ends a stalled provider thread too, so tray Exit always closes promptly.
    SetEvent(stopEvent);
    if (font) DeleteObject(font); if (icon) DestroyIcon(icon); DeleteObject(background);
    CloseHandle(single);
    ExitProcess(status < 0 ? 1 : 0);
}

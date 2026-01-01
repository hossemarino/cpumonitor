#include "help_window.h"

#include <windows.h>

#include <wchar.h>
#include <stdbool.h>
#include <stdint.h>

static const wchar_t *kHelpClass = L"ccm_help";
static HWND g_hwnd = NULL;
static HWND g_edit = NULL;

static const wchar_t *kHelpText =
L"CCM (Const CPU Monitor) – Metrics Help\r\n"
L"\r\n"
L"CPU static info:\r\n"
L"- Vendor/Brand/Family/Model/Stepping: CPUID identification fields.\r\n"
L"- Topology: packages/cores/logical processors from GetLogicalProcessorInformationEx.\r\n"
L"- Cache: L1/L2/L3 sizes (KB) reported by Windows topology APIs.\r\n"
L"- TSC GHz: best-effort calibration using RDTSC vs QPC over ~200ms.\r\n"
L"\r\n"
L"Live counters (PDH):\r\n"
L"- Usage: Total CPU utilization (% Processor Time).\r\n"
L"- Context Switches/sec: \"\\System\\Context Switches/sec\".\r\n"
L"- Interrupts/sec: \"\\Processor(_Total)\\Interrupts/sec\".\r\n"
L"- DPCs/sec: best-effort counter; name varies by OS.\r\n"
L"- Processor Queue Length: \"\\System\\Processor Queue Length\".\r\n"
L"- Power (W): optional \"\\Power Meter(_Total)\\Power\" if a power meter is exposed; often N/A.\r\n"
L"\r\n"
L"Live counters (Power API):\r\n"
L"- Per-core MHz: CallNtPowerInformation(ProcessorInformation).\r\n"
L"- Fchg/s: frequency-change rate (counts per-core MHz changes between samples).\r\n"
L"- Thr%: throttling estimate = percent of cores below 95% of their Max MHz.\r\n"
L"\r\n"
L"ETW (Kernel session, may require Admin):\r\n"
L"- cs: context-switch events observed via ETW.\r\n"
L"- isr: interrupt service routine events via ETW.\r\n"
L"- dpc: deferred procedure call events via ETW.\r\n"
L"Additional scheduler events depend on OS support and provider fields.\r\n"
L"\r\n"
L"Sensors (WMI, best-effort):\r\n"
L"- Temperature: MSAcpi_ThermalZoneTemperature (often not CPU package temp).\r\n"
L"- Fan RPM: Win32_Fan (rarely present on consumer systems).\r\n"
L"\r\n"
L"ISA flags (what they mean):\r\n"
L"- SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2: Streaming SIMD Extensions generations.\r\n"
L"- AVX/AVX2: Advanced Vector Extensions (256-bit SIMD; OS support required for AVX).\r\n"
L"- FMA: fused multiply-add (often used with AVX).\r\n"
L"- BMI1/BMI2: bit manipulation instruction sets.\r\n";

static bool file_exists_w(const wchar_t *path)
{
    const DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static void dir_from_path_w(wchar_t *path)
{
    size_t len = wcslen(path);
    while (len > 0) {
        const wchar_t c = path[len - 1];
        if (c == L'\\' || c == L'/') {
            path[len - 1] = 0;
            return;
        }
        len--;
    }
}

static bool try_get_chm_path(HWND owner, wchar_t *outChm, uint32_t outCch)
{
    if (!outChm || outCch == 0) return false;
    outChm[0] = 0;

    wchar_t exePath[MAX_PATH];
    exePath[0] = 0;
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    wchar_t exeDir[MAX_PATH];
    wcscpy_s(exeDir, MAX_PATH, exePath);
    dir_from_path_w(exeDir);

    // Look in: <exeDir>\help\... and <exeDir>\..\help\... (common when exe is in build\)
    wchar_t chm1[MAX_PATH], chm2[MAX_PATH];

    swprintf(chm1, MAX_PATH, L"%s\\help\\CCM.chm", exeDir);
    swprintf(chm2, MAX_PATH, L"%s\\..\\help\\CCM.chm", exeDir);

    const wchar_t *chm = NULL;
    if (file_exists_w(chm1)) chm = chm1;
    else if (file_exists_w(chm2)) chm = chm2;

    if (!chm) return false;
    wcscpy_s(outChm, outCch, chm);
    return true;
}

// Open CHM using the HTML Help viewer (hhctrl.ocx) rather than spawning a browser.
static bool try_open_chm_help(HWND owner)
{
    wchar_t chm[MAX_PATH];
    if (!try_get_chm_path(owner, chm, (uint32_t)_countof(chm))) {
        return false;
    }

    // Dynamically load HtmlHelpW so we don't need extra link flags.
    HMODULE hh = LoadLibraryW(L"hhctrl.ocx");
    if (!hh) {
        return false;
    }

    typedef HWND (WINAPI *HtmlHelpWFn)(HWND, LPCWSTR, UINT, DWORD_PTR);
    HtmlHelpWFn HtmlHelpW_ = (HtmlHelpWFn)(void *)GetProcAddress(hh, "HtmlHelpW");
    if (!HtmlHelpW_) {
        FreeLibrary(hh);
        return false;
    }

    // Commands: https://learn.microsoft.com/windows/win32/htmlhelp/htmlhelp-api-reference
    // Keep local defines to avoid needing htmlhelp.h.
    const UINT HH_DISPLAY_TOC = 0x0001;
    const UINT HH_DISPLAY_TOPIC = 0x0000;

    HWND hHelp = HtmlHelpW_(owner, chm, HH_DISPLAY_TOC, 0);
    if (!hHelp) {
        hHelp = HtmlHelpW_(owner, chm, HH_DISPLAY_TOPIC, 0);
    }

    // Intentionally do not FreeLibrary(hh) here; HtmlHelp may call back into the module.
    return hHelp != NULL;
}

void HelpWindow_ShowTopic(HWND owner, const wchar_t *topicHtml)
{
    if (!topicHtml || topicHtml[0] == 0) {
        HelpWindow_Show(owner);
        return;
    }

    wchar_t chm[MAX_PATH];
    if (!try_get_chm_path(owner, chm, (uint32_t)_countof(chm))) {
        HelpWindow_Show(owner);
        return;
    }

    HMODULE hh = LoadLibraryW(L"hhctrl.ocx");
    if (!hh) {
        HelpWindow_Show(owner);
        return;
    }

    typedef HWND (WINAPI *HtmlHelpWFn)(HWND, LPCWSTR, UINT, DWORD_PTR);
    HtmlHelpWFn HtmlHelpW_ = (HtmlHelpWFn)(void *)GetProcAddress(hh, "HtmlHelpW");
    if (!HtmlHelpW_) {
        FreeLibrary(hh);
        HelpWindow_Show(owner);
        return;
    }

    const UINT HH_DISPLAY_TOPIC = 0x0000;

    // Format: <path>\CCM.chm::/topic.html
    wchar_t target[MAX_PATH + 96];
    swprintf(target, (uint32_t)_countof(target), L"%s::/%s", chm, topicHtml);

    HWND hHelp = HtmlHelpW_(owner, target, HH_DISPLAY_TOPIC, 0);
    if (!hHelp) {
        // Fall back to TOC if direct navigation fails.
        HelpWindow_Show(owner);
    }

    // Intentionally do not FreeLibrary(hh) here; HtmlHelp may call back into the module.
}

static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kHelpText,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY,
                                 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                                 hwnd, NULL, (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), NULL);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return 0;
    }
    case WM_SIZE:
        if (g_edit) {
            MoveWindow(g_edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hwnd = NULL;
        g_edit = NULL;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void HelpWindow_Show(HWND owner)
{
    if (try_open_chm_help(owner)) {
        return;
    }

    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        return;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HelpProc;
    wc.hInstance = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    wc.lpszClassName = kHelpClass;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kHelpClass, L"CCM – Help",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 820, 640,
                             owner, NULL, wc.hInstance, NULL);
}

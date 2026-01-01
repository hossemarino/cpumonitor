#include <windows.h>
#include <pdh.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "../src/wmi_sensors.h"

#pragma comment(lib, "pdh.lib")

static const wchar_t *kPipeName = L"\\\\.\\pipe\\ccm_sensors";

typedef struct PdhPower {
    bool ok;
    bool hasPower;
    PDH_HQUERY q;
    PDH_HCOUNTER power;
} PdhPower;

static void PdhPower_Shutdown(PdhPower *p)
{
    if (!p) return;
    if (p->q) {
        PdhCloseQuery(p->q);
    }
    ZeroMemory(p, sizeof(*p));
}

static bool PdhPower_Init(PdhPower *p)
{
    if (!p) return false;
    ZeroMemory(p, sizeof(*p));

    if (PdhOpenQueryW(NULL, 0, &p->q) != ERROR_SUCCESS) {
        p->ok = false;
        return false;
    }

    // Optional. Only exists if the system exposes a power meter provider.
    if (PdhAddEnglishCounterW(p->q, L"\\Power Meter(_Total)\\Power", 0, &p->power) == ERROR_SUCCESS) {
        p->hasPower = true;
    }

    // Need two collects for rate counters, and it doesn't hurt here.
    PdhCollectQueryData(p->q);
    PdhCollectQueryData(p->q);

    p->ok = true;
    return true;
}

static bool PdhPower_TryReadWatts(PdhPower *p, double *outW)
{
    if (!p || !p->ok || !p->q || !p->hasPower || !p->power || !outW) return false;

    if (PdhCollectQueryData(p->q) != ERROR_SUCCESS) return false;

    PDH_FMT_COUNTERVALUE v;
    PDH_STATUS st = PdhGetFormattedCounterValue(p->power, PDH_FMT_DOUBLE, NULL, &v);
    if (st != ERROR_SUCCESS) return false;
    if (v.CStatus != ERROR_SUCCESS) return false;

    *outW = v.doubleValue;
    return true;
}

static bool try_parse_u32(const wchar_t *s, uint32_t *out)
{
    if (!s || !out) return false;
    wchar_t *end = NULL;
    unsigned long v = wcstoul(s, &end, 10);
    if (!end || end == s) return false;
    if (v > 0xFFFFFFFFu) return false;
    *out = (uint32_t)v;
    return true;
}

static bool overlapped_connect(HANDLE pipe, HANDLE parentHandle)
{
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;

    BOOL ok = ConnectNamedPipe(pipe, &ov);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) {
            CloseHandle(ov.hEvent);
            return true;
        }
        if (e != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            return false;
        }

        HANDLE waits[2];
        DWORD n = 0;
        waits[n++] = ov.hEvent;
        if (parentHandle) waits[n++] = parentHandle;

        DWORD w = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            DWORD dummy = 0;
            ok = GetOverlappedResult(pipe, &ov, &dummy, FALSE);
            CloseHandle(ov.hEvent);
            return ok ? true : false;
        }

        // Parent exited or unexpected wait result.
        CancelIoEx(pipe, &ov);
        CloseHandle(ov.hEvent);
        return false;
    }

    // Shouldn't happen with overlapped, but handle anyway.
    CloseHandle(ov.hEvent);
    return true;
}

static bool overlapped_read_some(HANDLE h, void *buf, DWORD cap, DWORD timeoutMs, DWORD *outGot)
{
    if (outGot) *outGot = 0;
    if (!h || h == INVALID_HANDLE_VALUE || !buf || cap == 0) return false;

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;

    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, cap, &got, &ov);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (w == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(h, &ov, &got, FALSE);
            } else {
                CancelIoEx(h, &ov);
                ok = FALSE;
            }
        }
    }

    CloseHandle(ov.hEvent);
    if (outGot) *outGot = got;
    return ok ? true : false;
}

static bool overlapped_write_all(HANDLE h, const void *buf, DWORD len, DWORD timeoutMs)
{
    if (!h || h == INVALID_HANDLE_VALUE || !buf) return false;

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;

    DWORD wrote = 0;
    BOOL ok = WriteFile(h, buf, len, &wrote, &ov);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (w == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(h, &ov, &wrote, FALSE);
            } else {
                CancelIoEx(h, &ov);
                ok = FALSE;
            }
        }
    }

    CloseHandle(ov.hEvent);
    return ok ? true : false;
}

static uint32_t parse_parent_pid_from_cmdline(void)
{
    const wchar_t *cmd = GetCommandLineW();
    if (!cmd) return 0;
    const wchar_t *p = wcsstr(cmd, L"--parent");
    if (!p) return 0;
    p += wcslen(L"--parent");
    while (*p == L' ' || *p == L'\t') p++;

    uint32_t pid = 0;
    const wchar_t *start = p;
    while (*p >= L'0' && *p <= L'9') {
        uint32_t next = pid * 10u + (uint32_t)(*p - L'0');
        pid = next;
        p++;
    }
    if (p == start) return 0;
    return pid;
}

static int provider_main(uint32_t parentPid)
{

    HANDLE parentHandle = NULL;
    if (parentPid != 0) {
        parentHandle = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)parentPid);
    }

    WmiSensors wmi;
    WmiSensors_Init(&wmi);

    PdhPower pdh;
    PdhPower_Init(&pdh);

    for (;;) {
        if (parentHandle) {
            DWORD st = WaitForSingleObject(parentHandle, 0);
            if (st == WAIT_OBJECT_0) break;
        }

        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            1024,
            1024,
            0,
            NULL);

        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(250);
            continue;
        }

        if (!overlapped_connect(pipe, parentHandle)) {
            CloseHandle(pipe);
            break;
        }

        // Read request (best-effort). CCM sends "GET\n".
        char req[64];
        ZeroMemory(req, sizeof(req));
        DWORD got = 0;
        (void)overlapped_read_some(pipe, req, (DWORD)sizeof(req) - 1, 2000, &got);

        // Always respond with a status line so CCM won't show "provider parse".
        char resp[512];
        int len = 0;
        len += snprintf(resp + len, (int)sizeof(resp) - len, "status=OK\n");

        float tempC = 0.0f;
        if (WmiSensors_TryReadCpuTempC(&wmi, &tempC)) {
            len += snprintf(resp + len, (int)sizeof(resp) - len, "tempC=%.1f\n", tempC);
        }

        double powerW = 0.0;
        if (PdhPower_TryReadWatts(&pdh, &powerW)) {
            len += snprintf(resp + len, (int)sizeof(resp) - len, "powerW=%.1f\n", powerW);
        }

        float fanRpm = 0.0f;
        if (WmiSensors_TryReadFanRpm(&wmi, &fanRpm)) {
            len += snprintf(resp + len, (int)sizeof(resp) - len, "fanRpm=%.0f\n", fanRpm);
        }

        if (len < 0) len = 0;
        if (len > (int)sizeof(resp)) len = (int)sizeof(resp);

        (void)overlapped_write_all(pipe, resp, (DWORD)len, 1500);

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    PdhPower_Shutdown(&pdh);
    WmiSensors_Shutdown(&wmi);

    if (parentHandle) CloseHandle(parentHandle);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    const uint32_t parentPid = parse_parent_pid_from_cmdline();
    return provider_main(parentPid);
}

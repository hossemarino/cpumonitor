#include "external_sensors.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static const wchar_t *kPipeName = L"\\\\.\\pipe\\ccm_sensors";

static bool overlapped_write_all(HANDLE h, const void *buf, DWORD len, DWORD timeoutMs, DWORD *outWrote)
{
    if (outWrote) *outWrote = 0;
    if (!h || h == INVALID_HANDLE_VALUE || !buf) return false;

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;

    DWORD wrote = 0;
    BOOL ok = WriteFile(h, buf, len, &wrote, &ov);
    if (!ok) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            const DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (w == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(h, &ov, &wrote, FALSE);
            } else {
                CancelIoEx(h, &ov);
                ok = FALSE;
            }
        }
    }

    CloseHandle(ov.hEvent);
    if (outWrote) *outWrote = wrote;
    return ok ? true : false;
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
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            const DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
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

static bool parse_kv_lines(const char *buf, uint32_t len, ExternalSensorsSample *out)
{
    if (!buf || !out) return false;

    // Make a local, NUL-terminated copy for simple parsing.
    char tmp[1024];
    if (len >= sizeof(tmp)) len = (uint32_t)(sizeof(tmp) - 1);
    memcpy(tmp, buf, len);
    tmp[len] = 0;

    bool any = false;

    char *ctx = NULL;
    for (char *line = strtok_s(tmp, "\n\r", &ctx); line; line = strtok_s(NULL, "\n\r", &ctx)) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == 0) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;

        if (_stricmp(key, "tempC") == 0) {
            out->cpuTempC = (float)atof(val);
            out->hasCpuTempC = true;
            any = true;
        } else if (_stricmp(key, "powerW") == 0) {
            out->powerW = (double)atof(val);
            out->hasPowerW = true;
            any = true;
        } else if (_stricmp(key, "fanRpm") == 0) {
            out->fanRpm = (float)atof(val);
            out->hasFanRpm = true;
            any = true;
        } else if (_stricmp(key, "status") == 0) {
            // Allows a provider to respond without any sensor keys.
            // CCM will treat this as a successful provider round-trip.
            any = true;
        }
    }

    return any;
}

bool ExternalSensors_TrySample(ExternalSensorsSample *out)
{
    if (!out) return false;
    ZeroMemory(out, sizeof(*out));
    wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider)");

    // Fast path: if no provider is listening, don't block the UI.
    // WaitNamedPipeW returns immediately if timeout is 0.
    if (!WaitNamedPipeW(kPipeName, 0)) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider not running)");
        } else {
            swprintf(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider wait err %lu)", (unsigned long)e);
        }
        return false;
    }

    HANDLE h = CreateFileW(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        swprintf(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider open err %lu)", (unsigned long)e);
        return false;
    }

    // Keep the protocol dead simple.
    // Use timeouts so a broken provider can't stall the UI.
    const char *req = "GET\n";
    DWORD wrote = 0;
    if (!overlapped_write_all(h, req, (DWORD)strlen(req), 80, &wrote)) {
        CloseHandle(h);
        wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider write timeout)");
        return false;
    }

    char resp[1024];
    DWORD got = 0;
    if (!overlapped_read_some(h, resp, (DWORD)sizeof(resp), 120, &got)) {
        CloseHandle(h);
        wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider read timeout)");
        return false;
    }

    CloseHandle(h);

    if (got == 0) {
        wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider empty)");
        return false;
    }

    if (!parse_kv_lines(resp, (uint32_t)got, out)) {
        wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"N/A (provider parse)");
        return false;
    }

    wcscpy_s(out->status, (uint32_t)(sizeof(out->status) / sizeof(out->status[0])), L"OK (provider)");
    return true;
}

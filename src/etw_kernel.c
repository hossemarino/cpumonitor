#include "etw_kernel.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef KERNEL_LOGGER_NAMEW
#define KERNEL_LOGGER_NAMEW L"NT Kernel Logger"
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

// Some MinGW SDKs omit these classic opcodes.
#ifndef EVENT_TRACE_TYPE_CSWITCH
#define EVENT_TRACE_TYPE_CSWITCH 36
#endif
#ifndef EVENT_TRACE_TYPE_DPC
#define EVENT_TRACE_TYPE_DPC 50
#endif
#ifndef EVENT_TRACE_TYPE_ISR
#define EVENT_TRACE_TYPE_ISR 51
#endif

#ifndef ERROR_PRIVILEGE_NOT_HELD
#define ERROR_PRIVILEGE_NOT_HELD 1314
#endif

// Some MinGW SDKs omit some kernel enable flags; keep these best-effort.
#ifndef EVENT_TRACE_FLAG_PROCESS
#define EVENT_TRACE_FLAG_PROCESS 0x00000001
#endif
#ifndef EVENT_TRACE_FLAG_THREAD
#define EVENT_TRACE_FLAG_THREAD 0x00000002
#endif
#ifndef EVENT_TRACE_FLAG_IMAGE_LOAD
#define EVENT_TRACE_FLAG_IMAGE_LOAD 0x00000004
#endif
#ifndef EVENT_TRACE_FLAG_DISPATCHER
#define EVENT_TRACE_FLAG_DISPATCHER 0x00000800
#endif
#ifndef EVENT_TRACE_FLAG_SYSTEMCALL
#define EVENT_TRACE_FLAG_SYSTEMCALL 0x00000080
#endif
#ifndef EVENT_TRACE_FLAG_PROFILE
#define EVENT_TRACE_FLAG_PROFILE 0x01000000
#endif
#ifndef EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS
#define EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS 0x00001000
#endif
#ifndef EVENT_TRACE_FLAG_DISK_IO
#define EVENT_TRACE_FLAG_DISK_IO 0x00000100
#endif
#ifndef EVENT_TRACE_FLAG_DISK_FILE_IO
#define EVENT_TRACE_FLAG_DISK_FILE_IO 0x00000200
#endif
#ifndef EVENT_TRACE_FLAG_FILE_IO
#define EVENT_TRACE_FLAG_FILE_IO 0x02000000
#endif
#ifndef EVENT_TRACE_FLAG_FILE_IO_INIT
#define EVENT_TRACE_FLAG_FILE_IO_INIT 0x04000000
#endif
#ifndef EVENT_TRACE_FLAG_NETWORK_TCPIP
#define EVENT_TRACE_FLAG_NETWORK_TCPIP 0x00010000
#endif
#ifndef EVENT_TRACE_FLAG_REGISTRY
#define EVENT_TRACE_FLAG_REGISTRY 0x00020000
#endif

typedef struct EtwThreadCtx {
    EtwKernel *k;
} EtwThreadCtx;

static void on_event_record(EVENT_RECORD *rec)
{
    EtwKernel *k = (EtwKernel *)rec->UserContext; // UserContext is expected to be a pointer to EtwKernel
    if (!k) return;

    // Kernel provider
    if (memcmp(&rec->EventHeader.ProviderId, &SystemTraceControlGuid, sizeof(GUID)) != 0) {
        return;
    }

    const UCHAR op = rec->EventHeader.EventDescriptor.Opcode; // opcode

    // Category classification (best-effort): kernel keywords generally mirror EnableFlags.
    const ULONGLONG kw = rec->EventHeader.EventDescriptor.Keyword;
    bool classified = false;

    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_THREAD) {
        InterlockedIncrement64((volatile LONG64 *)&k->threadCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_PROCESS) {
        InterlockedIncrement64((volatile LONG64 *)&k->processCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_IMAGE_LOAD) {
        InterlockedIncrement64((volatile LONG64 *)&k->imageLoadCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_DISPATCHER) {
        InterlockedIncrement64((volatile LONG64 *)&k->dispatcherCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_SYSTEMCALL) {
        InterlockedIncrement64((volatile LONG64 *)&k->syscallCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_PROFILE) {
        InterlockedIncrement64((volatile LONG64 *)&k->profileCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS) {
        InterlockedIncrement64((volatile LONG64 *)&k->pageFaultCount);
        classified = true;
    }
    if (kw & ((ULONGLONG)EVENT_TRACE_FLAG_FILE_IO | (ULONGLONG)EVENT_TRACE_FLAG_FILE_IO_INIT)) {
        InterlockedIncrement64((volatile LONG64 *)&k->fileIoCount);
        classified = true;
    }
    if (kw & ((ULONGLONG)EVENT_TRACE_FLAG_DISK_IO | (ULONGLONG)EVENT_TRACE_FLAG_DISK_FILE_IO)) {
        InterlockedIncrement64((volatile LONG64 *)&k->diskIoCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_NETWORK_TCPIP) {
        InterlockedIncrement64((volatile LONG64 *)&k->tcpipCount);
        classified = true;
    }
    if (kw & (ULONGLONG)EVENT_TRACE_FLAG_REGISTRY) {
        InterlockedIncrement64((volatile LONG64 *)&k->registryCount);
        classified = true;
    }

    if (op == EVENT_TRACE_TYPE_CSWITCH) {
        InterlockedIncrement64((volatile LONG64 *)&k->cswitchCount);
    } else if (op == EVENT_TRACE_TYPE_ISR) {
        InterlockedIncrement64((volatile LONG64 *)&k->isrCount);
    } else if (op == EVENT_TRACE_TYPE_DPC) {
        InterlockedIncrement64((volatile LONG64 *)&k->dpcCount);
    }

    if (!classified) {
        InterlockedIncrement64((volatile LONG64 *)&k->otherCount);
    }
}

static DWORD WINAPI etw_thread_main(LPVOID param)
{
    EtwThreadCtx *ctx = (EtwThreadCtx *)param;
    EtwKernel *k = ctx ? ctx->k : NULL;

    if (!k) {
        free(ctx);
        return 0;
    }

    // Allocate trace properties
    const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024;
    EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)calloc(1, propsSize);
    if (!props) {
        free(ctx);
        return 0;
    }

    props->Wnode.BufferSize = propsSize;
    props->Wnode.Guid = SystemTraceControlGuid;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // QPC

    // Kernel tracing uses the "system logger" mode.
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;

    // Provide sane defaults; leaving these at 0 can lead to ERROR_INVALID_PARAMETER on some systems.
    props->BufferSize = 64;       // KB
    props->MinimumBuffers = 8;
    props->MaximumBuffers = 128;

    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    // Write session name into the trailing buffer
    wchar_t *nameBuf = (wchar_t *)((uint8_t *)props + props->LoggerNameOffset);
    wcscpy_s(nameBuf, 512, k->sessionName);

    TRACEHANDLE session = 0;
    // Kernel flags: start broad, but fall back to a conservative set if a system rejects some flags with INVALID_PARAMETER.
    const ULONG fullFlags =
        (ULONG)EVENT_TRACE_FLAG_PROCESS |
        (ULONG)EVENT_TRACE_FLAG_THREAD |
        (ULONG)EVENT_TRACE_FLAG_IMAGE_LOAD |
        (ULONG)EVENT_TRACE_FLAG_CSWITCH |
        (ULONG)EVENT_TRACE_FLAG_DISPATCHER |
        (ULONG)EVENT_TRACE_FLAG_INTERRUPT |
        (ULONG)EVENT_TRACE_FLAG_DPC |
        (ULONG)EVENT_TRACE_FLAG_SYSTEMCALL |
        (ULONG)EVENT_TRACE_FLAG_PROFILE |
        (ULONG)EVENT_TRACE_FLAG_MEMORY_PAGE_FAULTS |
        (ULONG)EVENT_TRACE_FLAG_DISK_IO |
        (ULONG)EVENT_TRACE_FLAG_DISK_FILE_IO |
        (ULONG)EVENT_TRACE_FLAG_FILE_IO |
        (ULONG)EVENT_TRACE_FLAG_FILE_IO_INIT |
        (ULONG)EVENT_TRACE_FLAG_NETWORK_TCPIP |
        (ULONG)EVENT_TRACE_FLAG_REGISTRY;

    const ULONG minimalFlags =
        (ULONG)EVENT_TRACE_FLAG_PROCESS |
        (ULONG)EVENT_TRACE_FLAG_THREAD |
        (ULONG)EVENT_TRACE_FLAG_IMAGE_LOAD |
        (ULONG)EVENT_TRACE_FLAG_CSWITCH |
        (ULONG)EVENT_TRACE_FLAG_INTERRUPT |
        (ULONG)EVENT_TRACE_FLAG_DPC |
        (ULONG)EVENT_TRACE_FLAG_DISPATCHER;

    props->EnableFlags = fullFlags;

    ULONG st = StartTraceW(&session, k->sessionName, props);
    if (st == ERROR_ALREADY_EXISTS) {
        // Try stopping and restarting.
        ControlTraceW(0, k->sessionName, props, EVENT_TRACE_CONTROL_STOP);
        st = StartTraceW(&session, k->sessionName, props);
    }
    if (st == ERROR_INVALID_PARAMETER) {
        props->EnableFlags = minimalFlags;
        st = StartTraceW(&session, k->sessionName, props);
        if (st == ERROR_ALREADY_EXISTS) {
            ControlTraceW(0, k->sessionName, props, EVENT_TRACE_CONTROL_STOP);
            st = StartTraceW(&session, k->sessionName, props);
        }
    }

    if (st != ERROR_SUCCESS) {
        k->lastStage = 1;
        k->lastStatus = (uint32_t)st;
        k->ok = false;
        free(props);
        free(ctx);
        return 0;
    }

    EVENT_TRACE_LOGFILEW log;
    ZeroMemory(&log, sizeof(log));
    log.LoggerName = k->sessionName;
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = on_event_record;
    log.Context = k;

    TRACEHANDLE h = OpenTraceW(&log);
    if (h == (TRACEHANDLE)INVALID_PROCESSTRACE_HANDLE) {
        k->lastStage = 2;
        k->lastStatus = (uint32_t)GetLastError();
        k->ok = false;
        ControlTraceW(session, k->sessionName, props, EVENT_TRACE_CONTROL_STOP);
        free(props);
        free(ctx);
        return 0;
    }

    // Running.
    k->lastStage = 0;
    k->lastStatus = 0;
    k->ok = true;

    // Process events until stopped.
    // We'll poll stopRequested by calling CloseTrace from the stop function.
    ProcessTrace(&h, 1, NULL, NULL);

    CloseTrace(h);
    ControlTraceW(session, k->sessionName, props, EVENT_TRACE_CONTROL_STOP);

    free(props);
    free(ctx);
    return 0;
}

bool EtwKernel_Start(EtwKernel *k)
{
    if (!k) return false;
    ZeroMemory(k, sizeof(*k));

    // Use the canonical kernel logger session name.
    // Some systems reject custom names for the classic kernel provider with ERROR_INVALID_PARAMETER.
    wcscpy_s(k->sessionName, 64, KERNEL_LOGGER_NAMEW);

    k->cswitchCount = 0;
    k->isrCount = 0;
    k->dpcCount = 0;

    k->threadCount = 0;
    k->processCount = 0;
    k->imageLoadCount = 0;
    k->dispatcherCount = 0;
    k->syscallCount = 0;
    k->profileCount = 0;
    k->pageFaultCount = 0;
    k->fileIoCount = 0;
    k->diskIoCount = 0;
    k->tcpipCount = 0;
    k->registryCount = 0;
    k->otherCount = 0;

    k->stopRequested = 0;
    k->lastStage = 0;
    k->lastStatus = 0;
    k->startAttempted = true;

    EtwThreadCtx *ctx = (EtwThreadCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    ctx->k = k;

    HANDLE th = CreateThread(NULL, 0, etw_thread_main, ctx, 0, NULL);
    if (!th) {
        k->lastStage = 3; // CreateThread
        k->lastStatus = (uint32_t)GetLastError();
        k->ok = false;
        free(ctx);
        return false;
    }

    k->thread = th;
    // ok will become true once the thread successfully starts the session.
    k->ok = false;
    return true;
}

void EtwKernel_Stop(EtwKernel *k)
{
    if (!k || !k->thread) {
        return;
    }

    // Stop the ETW session; ProcessTrace should return.
    EVENT_TRACE_PROPERTIES props;
    ZeroMemory(&props, sizeof(props));
    props.Wnode.BufferSize = sizeof(props);

    ControlTraceW(0, k->sessionName, &props, EVENT_TRACE_CONTROL_STOP);

    WaitForSingleObject((HANDLE)k->thread, 2000);
    CloseHandle((HANDLE)k->thread);

    k->thread = NULL;
    k->ok = false;
}

static const wchar_t *etw_stage_name(uint32_t stage)
{
    switch (stage) {
    case 1:
        return L"StartTrace";
    case 2:
        return L"OpenTrace";
    case 3:
        return L"CreateThread";
    default:
        return L"ETW";
    }
}

static const wchar_t *etw_status_name(uint32_t st)
{
    switch (st) {
    case 0:
        return L"OK";
    case ERROR_ACCESS_DENIED:
        return L"ACCESS_DENIED";
    case ERROR_PRIVILEGE_NOT_HELD:
        return L"PRIVILEGE_NOT_HELD";
    case ERROR_ALREADY_EXISTS:
        return L"ALREADY_EXISTS";
    case ERROR_NOT_SUPPORTED:
        return L"NOT_SUPPORTED";
    case ERROR_INVALID_PARAMETER:
        return L"INVALID_PARAMETER";
    default:
        return L"ERR";
    }
}

static void trim_trailing_ws(wchar_t *s)
{
    if (!s) return;
    size_t n = wcslen(s);
    while (n > 0) {
        wchar_t c = s[n - 1];
        if (c == L'\r' || c == L'\n' || c == L' ' || c == L'\t') {
            s[n - 1] = 0;
            n--;
            continue;
        }
        break;
    }
}

static void format_win32_message(uint32_t code, wchar_t *out, uint32_t outCount)
{
    if (!out || outCount == 0) return;
    out[0] = 0;
    if (code == 0) return;

    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags,
                            NULL,
                            (DWORD)code,
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                            out,
                            (DWORD)outCount,
                            NULL);
    if (n == 0) {
        out[0] = 0;
        return;
    }

    trim_trailing_ws(out);
}

void EtwKernel_GetStatusText(const EtwKernel *k, wchar_t *out, uint32_t outCount)
{
    if (!out || outCount == 0) return;
    out[0] = 0;
    if (!k) return;

    // If ETW is running but there simply aren't events, callers should still see a positive status.
    if (k->ok) {
        wcscpy_s(out, outCount, L"ETW: OK");
        return;
    }

    if (!k->startAttempted) {
        wcscpy_s(out, outCount, L"ETW: not started");
        return;
    }

    if (k->startAttempted && k->lastStage == 0 && k->lastStatus == 0) {
        // Startup still in progress or failed before reporting a stage.
        wcscpy_s(out, outCount, L"ETW: starting...");
        return;
    }

    // If we failed before reporting a stage/code, still be explicit.
    if (k->lastStage == 0 || k->lastStatus == 0) {
        wcscpy_s(out, outCount, L"ETW: failed (no error details)");
        return;
    }

    const wchar_t *stage = etw_stage_name(k->lastStage);
    const wchar_t *name = etw_status_name(k->lastStatus);

    wchar_t msg[128];
    format_win32_message(k->lastStatus, msg, (uint32_t)(sizeof(msg) / sizeof(msg[0])));

    const wchar_t *hint = L"";
    if (k->lastStatus == ERROR_ACCESS_DENIED) {
        hint = L" (run as Admin)";
    } else if (k->lastStatus == ERROR_PRIVILEGE_NOT_HELD) {
        hint = L" (run as Admin; enable SeSystemProfilePrivilege)";
    } else if (k->lastStatus == ERROR_ALREADY_EXISTS) {
        hint = L" (kernel logger already running)";
    } else if (k->lastStatus == ERROR_NOT_SUPPORTED) {
        hint = L" (blocked by OS/policy)";
    } else if (k->lastStatus == ERROR_INVALID_PARAMETER) {
        hint = L" (flags rejected)";
    }

    if (name && name[0] != 0 && wcscmp(name, L"ERR") != 0) {
        if (msg[0] != 0) {
            swprintf(out, outCount,
                     L"%s: %s (%u/0x%08X)%s - %s",
                     stage, name, (unsigned)k->lastStatus, (unsigned)k->lastStatus, hint, msg);
        } else {
            swprintf(out, outCount,
                     L"%s: %s (%u/0x%08X)%s",
                     stage, name, (unsigned)k->lastStatus, (unsigned)k->lastStatus, hint);
        }
    } else {
        if (msg[0] != 0) {
            swprintf(out, outCount,
                     L"%s: 0x%08X (%u)%s - %s",
                     stage, (unsigned)k->lastStatus, (unsigned)k->lastStatus, hint, msg);
        } else {
            swprintf(out, outCount,
                     L"%s: 0x%08X (%u)%s",
                     stage, (unsigned)k->lastStatus, (unsigned)k->lastStatus, hint);
        }
    }
}

void EtwKernel_ComputeRates(EtwKernel *k, double dt, EtwRates *out)
{
    if (out) {
        out->cswitchPerSec = 0.0;
        out->isrPerSec = 0.0;
        out->dpcPerSec = 0.0;

        out->threadPerSec = 0.0;
        out->processPerSec = 0.0;
        out->imageLoadPerSec = 0.0;
        out->dispatcherPerSec = 0.0;
        out->syscallPerSec = 0.0;
        out->profilePerSec = 0.0;
        out->pageFaultPerSec = 0.0;
        out->fileIoPerSec = 0.0;
        out->diskIoPerSec = 0.0;
        out->tcpipPerSec = 0.0;
        out->registryPerSec = 0.0;
        out->otherPerSec = 0.0;
    }

    if (!k || !k->ok || dt <= 0.0) {
        return;
    }

    const long long cs = InterlockedAdd64((volatile LONG64 *)&k->cswitchCount, 0);
    const long long isr = InterlockedAdd64((volatile LONG64 *)&k->isrCount, 0);
    const long long dpc = InterlockedAdd64((volatile LONG64 *)&k->dpcCount, 0);

    const long long thr = InterlockedAdd64((volatile LONG64 *)&k->threadCount, 0);
    const long long proc = InterlockedAdd64((volatile LONG64 *)&k->processCount, 0);
    const long long img = InterlockedAdd64((volatile LONG64 *)&k->imageLoadCount, 0);
    const long long disp = InterlockedAdd64((volatile LONG64 *)&k->dispatcherCount, 0);
    const long long sysc = InterlockedAdd64((volatile LONG64 *)&k->syscallCount, 0);
    const long long prof = InterlockedAdd64((volatile LONG64 *)&k->profileCount, 0);
    const long long pf = InterlockedAdd64((volatile LONG64 *)&k->pageFaultCount, 0);
    const long long fio = InterlockedAdd64((volatile LONG64 *)&k->fileIoCount, 0);
    const long long dio = InterlockedAdd64((volatile LONG64 *)&k->diskIoCount, 0);
    const long long tcp = InterlockedAdd64((volatile LONG64 *)&k->tcpipCount, 0);
    const long long reg = InterlockedAdd64((volatile LONG64 *)&k->registryCount, 0);
    const long long oth = InterlockedAdd64((volatile LONG64 *)&k->otherCount, 0);

    const long long dCS = cs - k->prevCS;
    const long long dISR = isr - k->prevISR;
    const long long dDPC = dpc - k->prevDPC;

    const long long dThr = thr - k->prevThread;
    const long long dProc = proc - k->prevProcess;
    const long long dImg = img - k->prevImageLoad;
    const long long dDisp = disp - k->prevDispatcher;
    const long long dSysc = sysc - k->prevSyscall;
    const long long dProf = prof - k->prevProfile;
    const long long dPf = pf - k->prevPageFault;
    const long long dFio = fio - k->prevFileIo;
    const long long dDio = dio - k->prevDiskIo;
    const long long dTcp = tcp - k->prevTcpip;
    const long long dReg = reg - k->prevRegistry;
    const long long dOth = oth - k->prevOther;

    k->prevCS = cs;
    k->prevISR = isr;
    k->prevDPC = dpc;

    k->prevThread = thr;
    k->prevProcess = proc;
    k->prevImageLoad = img;
    k->prevDispatcher = disp;
    k->prevSyscall = sysc;
    k->prevProfile = prof;
    k->prevPageFault = pf;
    k->prevFileIo = fio;
    k->prevDiskIo = dio;
    k->prevTcpip = tcp;
    k->prevRegistry = reg;
    k->prevOther = oth;

    if (out) {
        out->cswitchPerSec = (double)dCS / dt;
        out->isrPerSec = (double)dISR / dt;
        out->dpcPerSec = (double)dDPC / dt;

        out->threadPerSec = (double)dThr / dt;
        out->processPerSec = (double)dProc / dt;
        out->imageLoadPerSec = (double)dImg / dt;
        out->dispatcherPerSec = (double)dDisp / dt;
        out->syscallPerSec = (double)dSysc / dt;
        out->profilePerSec = (double)dProf / dt;
        out->pageFaultPerSec = (double)dPf / dt;
        out->fileIoPerSec = (double)dFio / dt;
        out->diskIoPerSec = (double)dDio / dt;
        out->tcpipPerSec = (double)dTcp / dt;
        out->registryPerSec = (double)dReg / dt;
        out->otherPerSec = (double)dOth / dt;
    }
}

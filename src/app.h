#pragma once

#include <windows.h>
#include <stdint.h>

#include "render_d2d.h"
#include "cpu_static.h"
#include "pdh_counters.h"
#include "ringbuf.h"
#include "wmi_sensors.h"
#include "power_cpu.h"
#include "etw_kernel.h"
#include "proc_table.h"
#include "external_sensors.h"
#include "gpu_perf.h"

typedef enum AppTab {
    APP_TAB_CPU = 0,
    APP_TAB_MEMORY = 1,
    APP_TAB_GPU = 2,
} AppTab;

typedef struct DiskSeries {
    wchar_t name[128];
    RingBufF readMBpsHistory;
    RingBufF writeMBpsHistory;
    double readMBps;
    double writeMBps;
} DiskSeries;


typedef struct App {
    HINSTANCE hInstance;
    HWND hwnd;

    RenderD2D render;

    CpuStaticInfo cpuStatic;

    PdhState pdh;
    uint32_t logicalCount;

    // History: total usage + per-core usage
    RingBufF totalUsageHistory;
    RingBufF *coreUsageHistory;

    // Memory + storage history
    RingBufF memUsedPctHistory;
    RingBufF commitUsedPctHistory;

    // GPU history (best-effort)
    RingBufF gpuDedicatedUsedMBHistory;
    RingBufF gpuSharedUsedMBHistory;
    RingBufF *gpuEnginePctHistory; // length = gpuEngineTypeCount
    uint32_t gpuEngineTypeCount;

    DiskSeries *disks;
    uint32_t diskCount;
    RenderDiskSeries *renderDisks;

    // Fallback (if per-disk counters aren't available)
    RingBufF diskReadMBpsHistory;
    RingBufF diskWriteMBpsHistory;

    // Latest sampled values
    float totalUsage;
    float totalUsageMin;
    float totalUsageMax;
    float *coreUsage;
    float *coreMHz;
    float *coreMaxMHz;

    float *prevCoreMHz;
    uint32_t freqChangeCount;
    double freqChangesPerSec;

    // Memory + storage (latest)
    uint64_t memTotalPhysBytes;
    uint64_t memAvailPhysBytes;
    uint64_t commitTotalBytes;
    uint64_t commitLimitBytes;
    float memUsedPct;
    float commitUsedPct;
    double diskReadMBps;
    double diskWriteMBps;

    // GPU (latest, best-effort)
    GpuPerfState *gpu;
    GpuPerfSample gpuLast;
    double gpuDedicatedUsedMB;
    double gpuDedicatedLimitMB;
    double gpuSharedUsedMB;
    double gpuSharedLimitMB;

    // System rates
    double qpcFreq;
    int64_t lastSampleQpc;
    int64_t lastRenderQpc;

    // Optional sensors
    WmiSensors wmi;
    float cpuTempC;
    float fanRpm;
    float throttlePct;

    // Optional external sensor provider (named pipe).
    ExternalSensorsSample extSensors;

    // Optional: auto-start a bundled provider executable (sample).
    bool providerAutostartAttempted;
    HANDLE providerProcess;
    DWORD providerPid;

    // Optional ETW kernel session
    EtwKernel etw;
    EtwRates etwRates;

    // Process table (top by CPU)
    ProcTable procTable;

    // Process table view (optional stacked/groups view)
    bool procStacked;
    ProcRow *procViewRows;
    uint32_t procViewCount;
    uint32_t procViewCap;

    struct ProcGroupIndex {
        wchar_t baseName[64];
        uint32_t leaderPid;
        uint32_t memberStart;
        uint32_t memberCount;
    } *procGroups;
    uint32_t procGroupCount;
    uint32_t procGroupCap;
    uint32_t *procGroupMembers;
    uint32_t procGroupMemberCount;
    uint32_t procGroupMemberCap;

    // Stacked view: one expanded group at a time (accordion).
    bool procHasExpanded;
    wchar_t procExpandedBaseName[64];

    // Process table UI state
    ProcSortKey procSortKey;
    bool procSortAsc;
    uint32_t procScrollRow;
    uint32_t procSelectedPid;

    // Config
    double sampleIntervalSec; // e.g. 0.25

    // UI toggles
    bool showCpu0to15;

    // UI state
    AppTab tab;
} App;

bool App_Init(App *app, HINSTANCE hInstance);
void App_Show(App *app, int nCmdShow);
int App_Run(App *app);
void App_Shutdown(App *app);

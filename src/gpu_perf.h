#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Best-effort GPU telemetry via OS-exposed APIs:
// - DXGI: adapter identity + memory sizes + LUID
// - PDH: GPU Engine utilization + GPU Adapter Memory usage/limits
// This intentionally does NOT attempt vendor-specific temps/power/fan telemetry.

typedef struct GpuEngineTypeSample {
    const wchar_t *name; // stable pointer owned by GpuPerfState
    float utilizationPct; // 0..100
} GpuEngineTypeSample;

typedef struct GpuPerfSample {
    // Adapter identity
    bool hasAdapter;
    wchar_t adapterName[128];
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t subsystemId;
    uint32_t revision;
    LUID adapterLuid;

    // Memory sizes from DXGI (bytes)
    uint64_t dedicatedVideoMemoryBytes;
    uint64_t dedicatedSystemMemoryBytes;
    uint64_t sharedSystemMemoryBytes;

    // GPU Adapter Memory counters (bytes)
    bool hasMemoryCounters;
    uint64_t dedicatedUsageBytes;
    uint64_t dedicatedLimitBytes;
    uint64_t sharedUsageBytes;
    uint64_t sharedLimitBytes;

    // GPU Engine utilization counters (percent)
    bool hasEngineCounters;
    uint32_t engineTypeCount;
    const GpuEngineTypeSample *engineTypes; // length = engineTypeCount
} GpuPerfSample;

typedef struct GpuPerfState GpuPerfState;

bool GpuPerf_Init(GpuPerfState **outState);
void GpuPerf_Shutdown(GpuPerfState *s);

// Samples current values. Returns false if nothing usable could be collected.
bool GpuPerf_TrySample(GpuPerfState *s, GpuPerfSample *out);

#ifdef __cplusplus
}
#endif

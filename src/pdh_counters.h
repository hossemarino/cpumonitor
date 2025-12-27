#pragma once

#include <windows.h>
#include <pdh.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct PdhRates {
    double contextSwitchesPerSec;
    double interruptsPerSec;
    double dpcsPerSec;

    double processorQueueLength;

    // Best-effort disk throughput (legacy: _Total)
    double diskReadBytesPerSec;
    double diskWriteBytesPerSec;
    bool hasDisk;

    // Optional, requires a power meter provider.
    double powerWatts;
    bool hasPowerWatts;
} PdhRates;

typedef struct PdhSample {
    float totalCpu;
    float *coreCpu;   // length = logicalCount
    float *coreMHz;   // length = logicalCount (may be NULL)
} PdhSample;

typedef struct PdhState {
    uint32_t logicalCount;

    PDH_HQUERY query;

    PDH_HCOUNTER totalCpu;
    PDH_HCOUNTER ctxSwitches;
    PDH_HCOUNTER processorQueueLength;
    PDH_HCOUNTER interrupts;
    PDH_HCOUNTER dpcs;

    // Disk (best-effort): _Total
    PDH_HCOUNTER diskReadBytes;
    PDH_HCOUNTER diskWriteBytes;

    // Disk (best-effort): per-disk instances
    wchar_t *diskInstanceBuf;      // MULTI_SZ buffer owned by PDH state
    const wchar_t **diskInstances; // pointers into diskInstanceBuf (excluding _Total)
    uint32_t diskCount;
    PDH_HCOUNTER *diskReadBytesByDisk;
    PDH_HCOUNTER *diskWriteBytesByDisk;
    double *lastDiskReadBytesPerSec;
    double *lastDiskWriteBytesPerSec;
    bool hasPerDisk;

    PDH_HCOUNTER powerWatts;

    PDH_HCOUNTER *coreCpu;
    PDH_HCOUNTER *coreMHz;

    bool hasCoreMHz;
    bool ok;

    // storage for returning samples
    PdhSample scratch;
    float *scratchCoreCpu;
    float *scratchCoreMHz;

    PdhRates lastRates;
} PdhState;

bool Pdh_Init(PdhState *s, uint32_t logicalCount);
void Pdh_Shutdown(PdhState *s);

// Returns true if PDH is initialized and updated. Values are 0..100.
bool Pdh_TrySample(PdhState *s, PdhSample *out);

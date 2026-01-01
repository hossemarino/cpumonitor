#include "power_cpu.h"

#include <windows.h>
#include <powrprof.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "PowrProf.lib")

// MinGW headers sometimes omit these.
#ifndef PROCESSOR_POWER_INFORMATION
typedef struct _PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION, *PPROCESSOR_POWER_INFORMATION;
#endif

bool PowerCpu_TrySample(uint32_t logicalCount, PowerCpuSample *out)
{
    if (!out || logicalCount == 0 || !out->currentMHz || !out->maxMHz) {
        return false;
    }

    const size_t bytes = (size_t)logicalCount * sizeof(PROCESSOR_POWER_INFORMATION);
    PROCESSOR_POWER_INFORMATION *ppi = (PROCESSOR_POWER_INFORMATION *)malloc(bytes);
    if (!ppi) {
        return false;
    }

    // ProcessorInformation = 11 in POWER_INFORMATION_LEVEL
    DWORD st = CallNtPowerInformation((POWER_INFORMATION_LEVEL)11, NULL, 0, ppi, (ULONG)bytes);
    if (st != 0) {
        free(ppi);
        return false;
    }

    for (uint32_t i = 0; i < logicalCount; i++) {
        out->currentMHz[i] = (float)ppi[i].CurrentMhz;
        out->maxMHz[i] = (float)ppi[i].MaxMhz;
    }

    free(ppi);
    return true;
}

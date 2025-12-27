#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct PowerCpuSample {
    float *currentMHz; // length = logicalCount
    float *maxMHz;     // length = logicalCount
} PowerCpuSample;

// Best-effort per-core frequency via CallNtPowerInformation(ProcessorInformation).
bool PowerCpu_TrySample(uint32_t logicalCount, PowerCpuSample *out);

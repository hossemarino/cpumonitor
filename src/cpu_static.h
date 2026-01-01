#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct CpuCacheInfo {
    uint32_t level;      // 1/2/3
    uint32_t lineSize;
    uint32_t sizeKB;
    uint32_t type;       // 1=data 2=instruction 3=unified (maps to PROCESSOR_CACHE_TYPE)
} CpuCacheInfo;

typedef struct CpuStaticInfo {
    wchar_t vendor[16];
    wchar_t brand[64];

    uint32_t family;
    uint32_t model;
    uint32_t stepping;

    bool sse;
    bool sse2;
    bool sse3;
    bool ssse3;
    bool sse41;
    bool sse42;
    bool avx;
    bool avx2;
    bool bmi1;
    bool bmi2;
    bool fma;

    uint32_t logicalProcessorCount;
    uint32_t coreCount;
    uint32_t numaNodeCount;
    uint32_t packageCount;

    CpuCacheInfo caches[16];
    uint32_t cacheCount;

    double tscGHz;
} CpuStaticInfo;

void CpuStatic_Init(CpuStaticInfo *out);
void CpuStatic_Shutdown(CpuStaticInfo *cpu);

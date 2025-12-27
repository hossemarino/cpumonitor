#include "cpu_static.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
  #include <intrin.h>
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
  #include <cpuid.h>
  #include <x86intrin.h>
#endif

static unsigned long long read_tsc(void)
{
#if defined(_MSC_VER)
    return __rdtsc();
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    return __builtin_ia32_rdtsc();
#else
    return 0ULL;
#endif
}

static unsigned long long xgetbv0(void)
{
#if defined(_MSC_VER)
    return _xgetbv(0);
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    unsigned int eax = 0, edx = 0;
    __asm__ volatile (".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | (unsigned long long)eax;
#else
    return 0ULL;
#endif
}

static void cpuid(int regs[4], int leaf, int subleaf)
{
#if defined(_MSC_VER)
    __cpuidex(regs, leaf, subleaf);
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    unsigned int a = 0, b = 0, c = 0, d = 0;
    __cpuid_count((unsigned int)leaf, (unsigned int)subleaf, a, b, c, d);
    regs[0] = (int)a;
    regs[1] = (int)b;
    regs[2] = (int)c;
    regs[3] = (int)d;
#else
    regs[0] = regs[1] = regs[2] = regs[3] = 0;
    (void)leaf;
    (void)subleaf;
#endif
}

static void write_vendor(wchar_t out[16])
{
    int r[4] = {0};
    cpuid(r, 0, 0);
    char vendor[13];
    memcpy(&vendor[0], &r[1], 4); // EBX
    memcpy(&vendor[4], &r[3], 4); // EDX
    memcpy(&vendor[8], &r[2], 4); // ECX
    vendor[12] = 0;
    MultiByteToWideChar(CP_ACP, 0, vendor, -1, out, 16);
    out[15] = 0;
}

static void write_brand(wchar_t out[64])
{
    int r[4] = {0};
    cpuid(r, 0x80000000, 0);
    const unsigned maxExt = (unsigned)r[0];
    if (maxExt < 0x80000004) {
        wcscpy_s(out, 64, L"(unknown)");
        return;
    }

    char brand[49];
    memset(brand, 0, sizeof(brand));

    int *dst = (int *)brand;
    cpuid(r, 0x80000002, 0); memcpy(dst + 0, r, sizeof(r));
    cpuid(r, 0x80000003, 0); memcpy(dst + 4, r, sizeof(r));
    cpuid(r, 0x80000004, 0); memcpy(dst + 8, r, sizeof(r));
    brand[48] = 0;

    // trim leading spaces
    char *p = brand;
    while (*p == ' ') p++;

    MultiByteToWideChar(CP_ACP, 0, p, -1, out, 64);
}

static void detect_features(CpuStaticInfo *cpu)
{
    int r[4] = {0};
    cpuid(r, 1, 0);

    const int ecx = r[2];
    const int edx = r[3];

    cpu->sse = (edx & (1 << 25)) != 0;
    cpu->sse2 = (edx & (1 << 26)) != 0;
    cpu->sse3 = (ecx & (1 << 0)) != 0;
    cpu->ssse3 = (ecx & (1 << 9)) != 0;
    cpu->sse41 = (ecx & (1 << 19)) != 0;
    cpu->sse42 = (ecx & (1 << 20)) != 0;
    cpu->fma = (ecx & (1 << 12)) != 0;

    const bool osxsave = (ecx & (1 << 27)) != 0;
    const bool avxCpu = (ecx & (1 << 28)) != 0;
    cpu->avx = false;

    if (osxsave && avxCpu) {
        // Check OS support for AVX state (XMM/YMM enabled)
        unsigned long long xcr0 = xgetbv0();
        cpu->avx = ((xcr0 & 0x6) == 0x6);
    }

    cpuid(r, 7, 0);
    const int ebx = r[1];
    cpu->avx2 = cpu->avx && ((ebx & (1 << 5)) != 0);
    cpu->bmi1 = (ebx & (1 << 3)) != 0;
    cpu->bmi2 = (ebx & (1 << 8)) != 0;
}

static void detect_model(CpuStaticInfo *cpu)
{
    int r[4] = {0};
    cpuid(r, 1, 0);
    const unsigned eax = (unsigned)r[0];

    unsigned stepping = eax & 0xF;
    unsigned model = (eax >> 4) & 0xF;
    unsigned family = (eax >> 8) & 0xF;
    unsigned extModel = (eax >> 16) & 0xF;
    unsigned extFamily = (eax >> 20) & 0xFF;

    if (family == 0xF) {
        family += extFamily;
    }
    if (family == 0x6 || family == 0xF) {
        model += (extModel << 4);
    }

    cpu->family = family;
    cpu->model = model;
    cpu->stepping = stepping;
}

static uint32_t count_set_bits(ULONG_PTR mask)
{
    uint32_t count = 0;
    while (mask) {
        mask &= (mask - 1);
        count++;
    }
    return count;
}

static void detect_topology_and_caches(CpuStaticInfo *cpu)
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        return;
    }

    if (!GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf);
        return;
    }

    uint32_t cores = 0;
    uint32_t packages = 0;
    uint32_t nodes = 0;
    uint32_t logical = 0;

    uint32_t cacheCount = 0;

    uint8_t *p = buf;
    uint8_t *end = buf + len;
    while (p < end) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;

        switch (info->Relationship) {
        case RelationProcessorCore: {
            cores++;
            // GROUP_AFFINITY is per-core affinity
            for (WORD g = 0; g < info->Processor.GroupCount; g++) {
                logical += count_set_bits(info->Processor.GroupMask[g].Mask);
            }
            break;
        }
        case RelationProcessorPackage:
            packages++;
            break;
        case RelationNumaNode:
            nodes++;
            break;
        case RelationCache:
            if (cacheCount < (uint32_t)(sizeof(cpu->caches) / sizeof(cpu->caches[0]))) {
                CACHE_RELATIONSHIP *c = &info->Cache;
                cpu->caches[cacheCount].level = c->Level;
                cpu->caches[cacheCount].lineSize = c->LineSize;
                cpu->caches[cacheCount].sizeKB = (uint32_t)(c->CacheSize / 1024);
                cpu->caches[cacheCount].type = (uint32_t)c->Type;
                cacheCount++;
            }
            break;
        default:
            break;
        }

        p += info->Size;
    }

    cpu->coreCount = cores;
    cpu->packageCount = packages;
    cpu->numaNodeCount = nodes;
    cpu->logicalProcessorCount = logical;
    cpu->cacheCount = cacheCount;

    free(buf);
}

static double qpc_freq(void)
{
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return (double)li.QuadPart;
}

static int64_t qpc_now(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (int64_t)li.QuadPart;
}

static double calibrate_tsc_ghz(void)
{
    // Best-effort: measure TSC delta over ~200ms using QPC.
    // On systems where TSC is not invariant, this will be approximate.
    const double qpf = qpc_freq();

    HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (!timer) {
        return 0.0;
    }

    LARGE_INTEGER due;
    due.QuadPart = -200LL * 10000LL; // 200ms relative

    const int64_t q0 = qpc_now();
    const unsigned long long t0 = read_tsc();

    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(timer, INFINITE);

    const unsigned long long t1 = read_tsc();
    const int64_t q1 = qpc_now();

    CloseHandle(timer);

    const double secs = (double)(q1 - q0) / qpf;
    if (secs <= 0.0) {
        return 0.0;
    }

    const double hz = (double)(t1 - t0) / secs;
    return hz / 1e9;
}

void CpuStatic_Init(CpuStaticInfo *out)
{
    memset(out, 0, sizeof(*out));

    write_vendor(out->vendor);
    write_brand(out->brand);

    detect_model(out);
    detect_features(out);
    detect_topology_and_caches(out);

    if (out->logicalProcessorCount == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        out->logicalProcessorCount = si.dwNumberOfProcessors;
    }

    out->tscGHz = calibrate_tsc_ghz();
}

void CpuStatic_Shutdown(CpuStaticInfo *cpu)
{
    (void)cpu;
}

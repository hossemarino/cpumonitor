#include "pdh_counters.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "pdh.lib")

static bool add_counter(PDH_HQUERY q, const wchar_t *path, PDH_HCOUNTER *out)
{
    PDH_STATUS st = PdhAddEnglishCounterW(q, path, 0, out);
    return st == ERROR_SUCCESS;
}

static bool get_fmt_double(PDH_HCOUNTER c, double *out)
{
    PDH_FMT_COUNTERVALUE v;
    PDH_STATUS st = PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, NULL, &v);
    if (st != ERROR_SUCCESS) return false;
    if (v.CStatus != ERROR_SUCCESS) return false;
    *out = v.doubleValue;
    return true;
}

static bool get_fmt_float(PDH_HCOUNTER c, float *out)
{
    double d = 0.0;
    if (!get_fmt_double(c, &d)) return false;
    *out = (float)d;
    return true;
}

static uint32_t multistring_count(const wchar_t *ms)
{
    if (!ms) return 0;
    uint32_t n = 0;
    const wchar_t *p = ms;
    while (*p) {
        n++;
        p += wcslen(p) + 1;
    }
    return n;
}

static bool is_total_instance(const wchar_t *s)
{
    return (s && wcscmp(s, L"_Total") == 0);
}

static bool init_per_disk(PdhState *s)
{
    if (!s || !s->query) return false;

    // Enumerate instances of the PhysicalDisk object.
    DWORD counterLen = 0;
    DWORD instLen = 0;
    PDH_STATUS st = PdhEnumObjectItemsW(NULL, NULL, L"PhysicalDisk",
                                       NULL, &counterLen,
                                       NULL, &instLen,
                                       PERF_DETAIL_WIZARD, 0);
    if (st != ERROR_MORE_DATA || instLen == 0) {
        return false;
    }

    wchar_t *counterBuf = (wchar_t *)calloc(counterLen + 2, sizeof(wchar_t));
    wchar_t *instBuf = (wchar_t *)calloc(instLen + 2, sizeof(wchar_t));
    if (!counterBuf || !instBuf) {
        free(counterBuf);
        free(instBuf);
        return false;
    }

    st = PdhEnumObjectItemsW(NULL, NULL, L"PhysicalDisk",
                             counterBuf, &counterLen,
                             instBuf, &instLen,
                             PERF_DETAIL_WIZARD, 0);
    free(counterBuf);
    if (st != ERROR_SUCCESS) {
        free(instBuf);
        return false;
    }

    const uint32_t rawCount = multistring_count(instBuf);
    if (rawCount == 0) {
        free(instBuf);
        return false;
    }

    // Count non-_Total instances.
    uint32_t count = 0;
    for (const wchar_t *p = instBuf; *p; p += wcslen(p) + 1) {
        if (is_total_instance(p)) continue;
        count++;
    }
    if (count == 0) {
        free(instBuf);
        return false;
    }

    const wchar_t **instances = (const wchar_t **)calloc(count, sizeof(const wchar_t *));
    PDH_HCOUNTER *readC = (PDH_HCOUNTER *)calloc(count, sizeof(PDH_HCOUNTER));
    PDH_HCOUNTER *writeC = (PDH_HCOUNTER *)calloc(count, sizeof(PDH_HCOUNTER));
    double *lastR = (double *)calloc(count, sizeof(double));
    double *lastW = (double *)calloc(count, sizeof(double));
    if (!instances || !readC || !writeC || !lastR || !lastW) {
        free(instBuf);
        free(instances);
        free(readC);
        free(writeC);
        free(lastR);
        free(lastW);
        return false;
    }

    uint32_t idx = 0;
    for (wchar_t *p = instBuf; *p; p += wcslen(p) + 1) {
        if (is_total_instance(p)) continue;
        if (idx >= count) break;
        instances[idx++] = p;
    }

    // Add counters for each instance. If any fail, we keep what we can.
    uint32_t okCount = 0;
    for (uint32_t i = 0; i < count; i++) {
        wchar_t pathR[512];
        wchar_t pathW[512];
        swprintf(pathR, 512, L"\\PhysicalDisk(%s)\\Disk Read Bytes/sec", instances[i]);
        swprintf(pathW, 512, L"\\PhysicalDisk(%s)\\Disk Write Bytes/sec", instances[i]);
        if (add_counter(s->query, pathR, &readC[i]) && add_counter(s->query, pathW, &writeC[i])) {
            okCount++;
        } else {
            readC[i] = NULL;
            writeC[i] = NULL;
        }
    }

    if (okCount == 0) {
        free(instBuf);
        free(instances);
        free(readC);
        free(writeC);
        free(lastR);
        free(lastW);
        return false;
    }

    s->diskInstanceBuf = instBuf;
    s->diskInstances = instances;
    s->diskCount = count;
    s->diskReadBytesByDisk = readC;
    s->diskWriteBytesByDisk = writeC;
    s->lastDiskReadBytesPerSec = lastR;
    s->lastDiskWriteBytesPerSec = lastW;
    s->hasPerDisk = true;
    return true;
}

bool Pdh_Init(PdhState *s, uint32_t logicalCount)
{
    memset(s, 0, sizeof(*s));
    s->logicalCount = logicalCount;

    if (PdhOpenQueryW(NULL, 0, &s->query) != ERROR_SUCCESS) {
        s->ok = false;
        return false;
    }

    // CPU usage:
    // Prefer Processor Information(*)\% Processor Utility if present; fallback to % Processor Time.
    // We'll use % Processor Time for broad compatibility.
    if (!add_counter(s->query, L"\\Processor(_Total)\\% Processor Time", &s->totalCpu)) {
        s->ok = false;
        return false;
    }

    // Per-logical core: on Windows, instances for \Processor() are 0..N-1.
    s->coreCpu = (PDH_HCOUNTER *)calloc(logicalCount, sizeof(PDH_HCOUNTER));
    s->coreMHz = (PDH_HCOUNTER *)calloc(logicalCount, sizeof(PDH_HCOUNTER));
    s->scratchCoreCpu = (float *)calloc(logicalCount, sizeof(float));
    s->scratchCoreMHz = (float *)calloc(logicalCount, sizeof(float));

    if (!s->coreCpu || !s->coreMHz || !s->scratchCoreCpu || !s->scratchCoreMHz) {
        s->ok = false;
        return false;
    }

    for (uint32_t i = 0; i < logicalCount; i++) {
        wchar_t path[256];
        swprintf(path, 256, L"\\Processor(%u)\\%% Processor Time", i);
        add_counter(s->query, path, &s->coreCpu[i]);

        // Frequency (may not exist depending on OS/counters)
        // Processor Information has "Processor Frequency" in MHz.
        wchar_t pathMHz[256];
        swprintf(pathMHz, 256, L"\\Processor Information(%u,0)\\Processor Frequency", i);
        if (add_counter(s->query, pathMHz, &s->coreMHz[i])) {
            s->hasCoreMHz = true;
        }
    }

    add_counter(s->query, L"\\System\\Context Switches/sec", &s->ctxSwitches);
    // Queue length is a level (not a rate), but useful for overload indicators.
    add_counter(s->query, L"\\System\\Processor Queue Length", &s->processorQueueLength);
    add_counter(s->query, L"\\Processor(_Total)\\Interrupts/sec", &s->interrupts);

    // Optional system power draw if a power meter is exposed via performance counters.
    // Common instance names vary; try _Total first.
    if (add_counter(s->query, L"\\Power Meter(_Total)\\Power", &s->powerWatts)) {
        s->lastRates.hasPowerWatts = true;
    }

    // Disk throughput (best-effort). Instance naming varies; _Total is commonly present.
    if (add_counter(s->query, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &s->diskReadBytes) &&
        add_counter(s->query, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &s->diskWriteBytes)) {
        s->lastRates.hasDisk = true;
    }

    // Prefer per-disk counters if we can enumerate them.
    init_per_disk(s);

    // DPC: no universal counter name. Try common ones.
    if (!add_counter(s->query, L"\\Processor(_Total)\\DPCs Queued/sec", &s->dpcs)) {
        add_counter(s->query, L"\\Processor Information(_Total)\\DPC Rate", &s->dpcs);
    }

    // PDH needs two collects for rate counters.
    PdhCollectQueryData(s->query);

    s->scratch.totalCpu = 0.0f;
    s->scratch.coreCpu = s->scratchCoreCpu;
    s->scratch.coreMHz = s->hasCoreMHz ? s->scratchCoreMHz : NULL;

    s->ok = true;
    return true;
}

void Pdh_Shutdown(PdhState *s)
{
    if (!s) return;

    if (s->query) {
        PdhCloseQuery(s->query);
    }

    free(s->coreCpu);
    free(s->coreMHz);
    free(s->scratchCoreCpu);
    free(s->scratchCoreMHz);

    free(s->diskReadBytesByDisk);
    free(s->diskWriteBytesByDisk);
    free(s->lastDiskReadBytesPerSec);
    free(s->lastDiskWriteBytesPerSec);
    free((void *)s->diskInstances);
    free(s->diskInstanceBuf);

    memset(s, 0, sizeof(*s));
}

bool Pdh_TrySample(PdhState *s, PdhSample *out)
{
    if (!s || !s->ok || !s->query) return false;

    if (PdhCollectQueryData(s->query) != ERROR_SUCCESS) {
        return false;
    }

    get_fmt_float(s->totalCpu, &s->scratch.totalCpu);

    for (uint32_t i = 0; i < s->logicalCount; i++) {
        float v = 0.0f;
        if (s->coreCpu[i]) {
            get_fmt_float(s->coreCpu[i], &v);
        }
        s->scratchCoreCpu[i] = v;

        if (s->hasCoreMHz && s->coreMHz[i]) {
            get_fmt_float(s->coreMHz[i], &s->scratchCoreMHz[i]);
        }
    }

    double d = 0.0;
    if (s->ctxSwitches && get_fmt_double(s->ctxSwitches, &d)) s->lastRates.contextSwitchesPerSec = d;
    if (s->processorQueueLength && get_fmt_double(s->processorQueueLength, &d)) s->lastRates.processorQueueLength = d;
    if (s->interrupts && get_fmt_double(s->interrupts, &d)) s->lastRates.interruptsPerSec = d;
    if (s->dpcs && get_fmt_double(s->dpcs, &d)) s->lastRates.dpcsPerSec = d;
    if (s->powerWatts && s->lastRates.hasPowerWatts && get_fmt_double(s->powerWatts, &d)) s->lastRates.powerWatts = d;

    if (s->lastRates.hasDisk) {
        if (s->diskReadBytes && get_fmt_double(s->diskReadBytes, &d)) s->lastRates.diskReadBytesPerSec = d;
        if (s->diskWriteBytes && get_fmt_double(s->diskWriteBytes, &d)) s->lastRates.diskWriteBytesPerSec = d;
    }

    if (s->hasPerDisk && s->diskCount > 0 && s->lastDiskReadBytesPerSec && s->lastDiskWriteBytesPerSec) {
        for (uint32_t i = 0; i < s->diskCount; i++) {
            double rr = 0.0;
            double ww = 0.0;
            if (s->diskReadBytesByDisk && s->diskReadBytesByDisk[i]) {
                get_fmt_double(s->diskReadBytesByDisk[i], &rr);
            }
            if (s->diskWriteBytesByDisk && s->diskWriteBytesByDisk[i]) {
                get_fmt_double(s->diskWriteBytesByDisk[i], &ww);
            }
            s->lastDiskReadBytesPerSec[i] = rr;
            s->lastDiskWriteBytesPerSec[i] = ww;
        }
    }

    if (out) {
        *out = s->scratch;
    }
    return true;
}

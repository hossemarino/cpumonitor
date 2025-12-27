#include "gpu_perf.h"

#include <pdh.h>

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <dxgi.h>
#include <dxgi1_6.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

typedef struct EngineCounter {
    PDH_HCOUNTER counter;
    uint32_t typeIndex;
} EngineCounter;

struct GpuPerfState {
    bool ok;

    // Selected adapter
    bool hasAdapter;
    wchar_t adapterName[128];
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t subsystemId;
    uint32_t revision;
    LUID adapterLuid;
    uint64_t dedicatedVideoMemoryBytes;
    uint64_t dedicatedSystemMemoryBytes;
    uint64_t sharedSystemMemoryBytes;

    // PDH query
    PDH_HQUERY query;

    // Memory counters (GPU Adapter Memory)
    bool hasMemoryCounters;
    PDH_HCOUNTER dedicatedUsage;
    PDH_HCOUNTER dedicatedLimit;
    PDH_HCOUNTER sharedUsage;
    PDH_HCOUNTER sharedLimit;

    // Engine counters (GPU Engine)
    bool hasEngineCounters;
    EngineCounter *engineCounters;
    uint32_t engineCounterCount;

    wchar_t *engineTypeBuf;      // MULTI_SZ-ish storage for names (packed)
    const wchar_t **engineTypes; // pointers into engineTypeBuf
    uint32_t engineTypeCount;

    // scratch output array
    GpuEngineTypeSample *scratchTypes;

    // cached instance token for filtering
    wchar_t luidToken[64];
};

static void safe_wcsncpy0(wchar_t *dst, uint32_t dstCch, const wchar_t *src)
{
    if (!dst || dstCch == 0) return;
    dst[0] = 0;
    if (!src) return;
    wcsncpy(dst, src, dstCch - 1);
    dst[dstCch - 1] = 0;
}

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

static void luid_to_token(LUID luid, wchar_t *out, uint32_t outCch)
{
    // Common PDH instance format uses: luid_0x%08X_0x%08X (high, low)
    if (!out || outCch == 0) return;
    swprintf(out, outCch, L"luid_0x%08X_0x%08X", (uint32_t)luid.HighPart, (uint32_t)luid.LowPart);
    out[outCch - 1] = 0;
}

static bool instance_contains_luid(const wchar_t *instance, const wchar_t *luidToken)
{
    if (!instance || !luidToken || luidToken[0] == 0) return false;
    return wcsstr(instance, luidToken) != NULL;
}

static const wchar_t *find_engtype_suffix(const wchar_t *instance)
{
    if (!instance) return NULL;
    const wchar_t *p = wcsstr(instance, L"engtype_");
    if (!p) return NULL;
    return p + wcslen(L"engtype_");
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

static bool pick_primary_adapter(GpuPerfState *s)
{
    if (!s) return false;

    IDXGIFactory1 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr) || !factory) {
        return false;
    }

    uint64_t bestMem = 0;
    IDXGIAdapter1 *best = NULL;

    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *ad = NULL;
        hr = IDXGIFactory1_EnumAdapters1(factory, i, &ad);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !ad) break;

        DXGI_ADAPTER_DESC1 d1;
        ZeroMemory(&d1, sizeof(d1));
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d1))) {
            if ((d1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                if ((uint64_t)d1.DedicatedVideoMemory >= bestMem) {
                    if (best) IDXGIAdapter1_Release(best);
                    best = ad;
                    bestMem = (uint64_t)d1.DedicatedVideoMemory;
                    continue;
                }
            }
        }

        IDXGIAdapter1_Release(ad);
    }

    if (!best) {
        IDXGIFactory1_Release(factory);
        return false;
    }

    DXGI_ADAPTER_DESC1 d;
    ZeroMemory(&d, sizeof(d));
    if (FAILED(IDXGIAdapter1_GetDesc1(best, &d))) {
        IDXGIAdapter1_Release(best);
        IDXGIFactory1_Release(factory);
        return false;
    }

    safe_wcsncpy0(s->adapterName, (uint32_t)_countof(s->adapterName), d.Description);
    s->vendorId = d.VendorId;
    s->deviceId = d.DeviceId;
    s->subsystemId = d.SubSysId;
    s->revision = d.Revision;
    s->adapterLuid = d.AdapterLuid;
    s->dedicatedVideoMemoryBytes = (uint64_t)d.DedicatedVideoMemory;
    s->dedicatedSystemMemoryBytes = (uint64_t)d.DedicatedSystemMemory;
    s->sharedSystemMemoryBytes = (uint64_t)d.SharedSystemMemory;
    s->hasAdapter = true;

    luid_to_token(s->adapterLuid, s->luidToken, (uint32_t)_countof(s->luidToken));

    IDXGIAdapter1_Release(best);
    IDXGIFactory1_Release(factory);
    return true;
}

static bool init_memory_counters(GpuPerfState *s)
{
    if (!s || !s->query || !s->hasAdapter) return false;

    // Enumerate instances of GPU Adapter Memory to find the one matching our adapter LUID.
    DWORD counterLen = 0;
    DWORD instLen = 0;
    PDH_STATUS st = PdhEnumObjectItemsW(NULL, NULL, L"GPU Adapter Memory",
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

    st = PdhEnumObjectItemsW(NULL, NULL, L"GPU Adapter Memory",
                             counterBuf, &counterLen,
                             instBuf, &instLen,
                             PERF_DETAIL_WIZARD, 0);
    free(counterBuf);
    if (st != ERROR_SUCCESS) {
        free(instBuf);
        return false;
    }

    const wchar_t *matchInst = NULL;
    for (const wchar_t *p = instBuf; *p; p += wcslen(p) + 1) {
        if (instance_contains_luid(p, s->luidToken)) {
            matchInst = p;
            break;
        }
    }

    if (!matchInst) {
        free(instBuf);
        return false;
    }

    wchar_t path[512];

    swprintf(path, (uint32_t)_countof(path), L"\\GPU Adapter Memory(%s)\\Dedicated Usage", matchInst);
    bool ok = add_counter(s->query, path, &s->dedicatedUsage);

    swprintf(path, (uint32_t)_countof(path), L"\\GPU Adapter Memory(%s)\\Dedicated Limit", matchInst);
    ok = ok && add_counter(s->query, path, &s->dedicatedLimit);

    swprintf(path, (uint32_t)_countof(path), L"\\GPU Adapter Memory(%s)\\Shared Usage", matchInst);
    ok = ok && add_counter(s->query, path, &s->sharedUsage);

    swprintf(path, (uint32_t)_countof(path), L"\\GPU Adapter Memory(%s)\\Shared Limit", matchInst);
    ok = ok && add_counter(s->query, path, &s->sharedLimit);

    free(instBuf);

    if (!ok) {
        s->dedicatedUsage = NULL;
        s->dedicatedLimit = NULL;
        s->sharedUsage = NULL;
        s->sharedLimit = NULL;
        return false;
    }

    s->hasMemoryCounters = true;
    return true;
}

static uint32_t engine_type_index(GpuPerfState *s, const wchar_t *typeName)
{
    if (!s || !typeName || typeName[0] == 0) return UINT32_MAX;

    for (uint32_t i = 0; i < s->engineTypeCount; i++) {
        if (s->engineTypes[i] && wcscmp(s->engineTypes[i], typeName) == 0) {
            return i;
        }
    }

    // Append new type name
    const uint32_t newCount = s->engineTypeCount + 1;
    const wchar_t **newPtrs = (const wchar_t **)realloc((void *)s->engineTypes, newCount * sizeof(const wchar_t *));
    if (!newPtrs) return UINT32_MAX;
    s->engineTypes = newPtrs;

    // Store name into engineTypeBuf (simple packed heap)
    const size_t nameLen = wcslen(typeName);
    const size_t oldBytes = s->engineTypeBuf ? (wcslen(s->engineTypeBuf) + 1) * sizeof(wchar_t) : 0;

    // We don't have a real allocator for packed strings; do the simple thing:
    // re-pack by appending to a growable flat buffer with NUL separators.
    // Keep it easy and safe: just allocate a new buffer and copy all existing strings.

    size_t totalChars = 0;
    for (uint32_t i = 0; i < s->engineTypeCount; i++) {
        totalChars += wcslen(s->engineTypes[i]) + 1;
    }
    totalChars += nameLen + 1;
    totalChars += 1; // final NUL

    wchar_t *nb = (wchar_t *)calloc(totalChars, sizeof(wchar_t));
    if (!nb) return UINT32_MAX;

    // Copy old strings
    wchar_t *dst = nb;
    for (uint32_t i = 0; i < s->engineTypeCount; i++) {
        const wchar_t *src = s->engineTypes[i];
        const size_t n = wcslen(src);
        memcpy(dst, src, n * sizeof(wchar_t));
        dst[n] = 0;
        dst += n + 1;
    }

    // Append new string
    memcpy(dst, typeName, nameLen * sizeof(wchar_t));
    dst[nameLen] = 0;

    // Rebuild pointers into nb
    wchar_t *p = nb;
    for (uint32_t i = 0; i < s->engineTypeCount; i++) {
        const size_t n = wcslen(p);
        newPtrs[i] = p;
        p += n + 1;
    }
    newPtrs[s->engineTypeCount] = p;

    free(s->engineTypeBuf);
    s->engineTypeBuf = nb;
    s->engineTypes = newPtrs;
    s->engineTypeCount = newCount;

    // Resize scratch output
    GpuEngineTypeSample *ns = (GpuEngineTypeSample *)realloc(s->scratchTypes, newCount * sizeof(GpuEngineTypeSample));
    if (!ns) return UINT32_MAX;
    s->scratchTypes = ns;

    (void)oldBytes;
    return newCount - 1;
}

static bool init_engine_counters(GpuPerfState *s)
{
    if (!s || !s->query || !s->hasAdapter) return false;

    DWORD counterLen = 0;
    DWORD instLen = 0;
    PDH_STATUS st = PdhEnumObjectItemsW(NULL, NULL, L"GPU Engine",
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

    st = PdhEnumObjectItemsW(NULL, NULL, L"GPU Engine",
                             counterBuf, &counterLen,
                             instBuf, &instLen,
                             PERF_DETAIL_WIZARD, 0);
    free(counterBuf);
    if (st != ERROR_SUCCESS) {
        free(instBuf);
        return false;
    }

    const uint32_t instCount = multistring_count(instBuf);
    if (instCount == 0) {
        free(instBuf);
        return false;
    }

    // Add counters for instances matching this adapter LUID.
    EngineCounter *counters = NULL;
    uint32_t cap = 0;
    uint32_t count = 0;

    for (const wchar_t *p = instBuf; *p; p += wcslen(p) + 1) {
        if (!instance_contains_luid(p, s->luidToken)) continue;
        const wchar_t *type = find_engtype_suffix(p);
        if (!type || type[0] == 0) continue;

        uint32_t typeIdx = engine_type_index(s, type);
        if (typeIdx == UINT32_MAX) continue;

        if (count + 1 > cap) {
            const uint32_t newCap = cap ? (cap * 2) : 128;
            EngineCounter *nc = (EngineCounter *)realloc(counters, newCap * sizeof(EngineCounter));
            if (!nc) break;
            counters = nc;
            cap = newCap;
        }

        wchar_t path[1024];
        swprintf(path, (uint32_t)_countof(path), L"\\GPU Engine(%s)\\Utilization Percentage", p);

        PDH_HCOUNTER hc = NULL;
        if (!add_counter(s->query, path, &hc)) {
            continue;
        }

        counters[count].counter = hc;
        counters[count].typeIndex = typeIdx;
        count++;
    }

    free(instBuf);

    if (count == 0) {
        free(counters);
        return false;
    }

    s->engineCounters = counters;
    s->engineCounterCount = count;
    s->hasEngineCounters = true;
    return true;
}

bool GpuPerf_Init(GpuPerfState **outState)
{
    if (!outState) return false;
    *outState = NULL;

    GpuPerfState *s = (GpuPerfState *)calloc(1, sizeof(GpuPerfState));
    if (!s) return false;

    pick_primary_adapter(s);

    if (PdhOpenQueryW(NULL, 0, &s->query) != ERROR_SUCCESS) {
        GpuPerf_Shutdown(s);
        return false;
    }

    init_memory_counters(s);
    init_engine_counters(s);

    // PDH needs two collects for rate counters.
    PdhCollectQueryData(s->query);

    s->ok = s->hasAdapter && (s->hasEngineCounters || s->hasMemoryCounters);

    *outState = s;
    return s->ok;
}

void GpuPerf_Shutdown(GpuPerfState *s)
{
    if (!s) return;

    if (s->query) {
        PdhCloseQuery(s->query);
        s->query = NULL;
    }

    free(s->engineCounters);
    s->engineCounters = NULL;
    s->engineCounterCount = 0;

    free((void *)s->engineTypes);
    s->engineTypes = NULL;
    s->engineTypeCount = 0;

    free(s->engineTypeBuf);
    s->engineTypeBuf = NULL;

    free(s->scratchTypes);
    s->scratchTypes = NULL;

    free(s);
}

bool GpuPerf_TrySample(GpuPerfState *s, GpuPerfSample *out)
{
    if (!s || !s->ok || !s->query || !out) return false;

    if (PdhCollectQueryData(s->query) != ERROR_SUCCESS) {
        return false;
    }

    ZeroMemory(out, sizeof(*out));

    out->hasAdapter = s->hasAdapter;
    safe_wcsncpy0(out->adapterName, (uint32_t)_countof(out->adapterName), s->adapterName);
    out->vendorId = s->vendorId;
    out->deviceId = s->deviceId;
    out->subsystemId = s->subsystemId;
    out->revision = s->revision;
    out->adapterLuid = s->adapterLuid;
    out->dedicatedVideoMemoryBytes = s->dedicatedVideoMemoryBytes;
    out->dedicatedSystemMemoryBytes = s->dedicatedSystemMemoryBytes;
    out->sharedSystemMemoryBytes = s->sharedSystemMemoryBytes;

    if (s->hasMemoryCounters) {
        double d = 0.0;
        uint64_t du = 0, dl = 0, su = 0, sl = 0;
        if (s->dedicatedUsage && get_fmt_double(s->dedicatedUsage, &d) && d >= 0.0) du = (uint64_t)d;
        if (s->dedicatedLimit && get_fmt_double(s->dedicatedLimit, &d) && d >= 0.0) dl = (uint64_t)d;
        if (s->sharedUsage && get_fmt_double(s->sharedUsage, &d) && d >= 0.0) su = (uint64_t)d;
        if (s->sharedLimit && get_fmt_double(s->sharedLimit, &d) && d >= 0.0) sl = (uint64_t)d;

        out->hasMemoryCounters = true;
        out->dedicatedUsageBytes = du;
        out->dedicatedLimitBytes = dl;
        out->sharedUsageBytes = su;
        out->sharedLimitBytes = sl;
    }

    if (s->hasEngineCounters && s->engineTypeCount > 0) {
        // zero scratch
        for (uint32_t i = 0; i < s->engineTypeCount; i++) {
            s->scratchTypes[i].name = s->engineTypes[i];
            s->scratchTypes[i].utilizationPct = 0.0f;
        }

        for (uint32_t i = 0; i < s->engineCounterCount; i++) {
            double d = 0.0;
            if (!s->engineCounters[i].counter) continue;
            if (!get_fmt_double(s->engineCounters[i].counter, &d)) continue;
            if (d < 0.0) d = 0.0;
            if (d > 100.0) d = 100.0;
            const uint32_t idx = s->engineCounters[i].typeIndex;
            if (idx >= s->engineTypeCount) continue;
            s->scratchTypes[idx].utilizationPct += (float)d;
            if (s->scratchTypes[idx].utilizationPct > 100.0f) {
                s->scratchTypes[idx].utilizationPct = 100.0f;
            }
        }

        out->hasEngineCounters = true;
        out->engineTypeCount = s->engineTypeCount;
        out->engineTypes = s->scratchTypes;
    }

    return out->hasAdapter && (out->hasEngineCounters || out->hasMemoryCounters);
}

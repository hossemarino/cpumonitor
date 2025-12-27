#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <d2d1.h>
#include <dwrite.h>

#include "cpu_static.h"
#include "ringbuf.h"
#include "pdh_counters.h"
#include "etw_kernel.h"
#include "proc_table.h"

typedef struct RenderD2D {
    HWND hwnd;

    ID2D1Factory *factory;
    ID2D1HwndRenderTarget *rt;

    IDWriteFactory *dwFactory;
    IDWriteTextFormat *text;
    IDWriteTextFormat *textSmall;

    ID2D1SolidColorBrush *brushText;
    ID2D1SolidColorBrush *brushDim;
    ID2D1SolidColorBrush *brushGreen;
    ID2D1SolidColorBrush *brushYellow;
    ID2D1SolidColorBrush *brushRed;
    ID2D1SolidColorBrush *brushGrid;

    float dpiScale;
    uint32_t width;
    uint32_t height;

    // Layout
    float tabsBottomY;
    float headerBottomY;
    float graphBottomY;

    // Tabs layout (from last draw) for input hit-testing
    float tabCpuX;
    float tabCpuY;
    float tabCpuW;
    float tabCpuH;
    float tabMemX;
    float tabMemY;
    float tabMemW;
    float tabMemH;
    float tabGpuX;
    float tabGpuY;
    float tabGpuW;
    float tabGpuH;

    // Process table layout (from last draw) for input hit-testing
    float procTableX;
    float procTableY;
    float procTableW;
    float procTableH;
    float procHeaderY;
    float procRowH;
    float procTitleY;
    float procTitleH;
    float procHelpX;
    float procHelpY;
    float procHelpW;
    float procHelpH;
    float procColX[8];
    uint32_t procRowCount;
    uint32_t procScrollRow;
    uint32_t procVisibleRows;
} RenderD2D;

bool Render_Init(RenderD2D *r, HWND hwnd);
void Render_Shutdown(RenderD2D *r);
void Render_Resize(RenderD2D *r, uint32_t width, uint32_t height);

void Render_Begin(RenderD2D *r);
void Render_Clear(RenderD2D *r);
void Render_End(RenderD2D *r);

// UI tabs
void Render_DrawTabs(RenderD2D *r, int activeTab);

void Render_DrawHeader(RenderD2D *r,
                       const CpuStaticInfo *cpu,
                       float totalUsage,
                       float usageMin,
                       float usageMax,
                       float cpuTempC,
                       const PdhRates *rates,
                       double cpuFreqChangesPerSec,
                       const EtwRates *etw,
                       const wchar_t *etwStatusText,
                       const wchar_t *sensorStatusText,
                       uint64_t uptimeMs,
                       float throttlePct,
                       float fanRpm);

void Render_DrawUsageGraph(RenderD2D *r, const RingBufF *history);

// Memory tab
void Render_DrawMemoryHeader(RenderD2D *r,
                             uint64_t totalPhysBytes,
                             uint64_t availPhysBytes,
                             uint64_t commitTotalBytes,
                             uint64_t commitLimitBytes,
                             double diskReadMBps,
                             double diskWriteMBps);

void Render_DrawPercentGraph(RenderD2D *r, const RingBufF *history, const wchar_t *title);
void Render_DrawDiskGraph(RenderD2D *r, const RingBufF *readMBpsHistory, const RingBufF *writeMBpsHistory);

// GPU tab
void Render_DrawGpuHeader(RenderD2D *r,
                          const wchar_t *adapterName,
                          uint32_t vendorId,
                          uint64_t dedicatedTotalBytes,
                          uint64_t sharedTotalBytes,
                          double dedicatedUsedMB,
                          double dedicatedLimitMB,
                          double sharedUsedMB,
                          double sharedLimitMB);

// Generic single-series graph (best-effort) with a fixed max scale.
// Used for GPU memory usage (MB).
void Render_DrawValueGraph(RenderD2D *r, const RingBufF *history, const wchar_t *title, float maxValue);

typedef struct RenderDiskSeries {
    const wchar_t *name;
    const RingBufF *readMBpsHistory;
    const RingBufF *writeMBpsHistory;
    double readMBps;
    double writeMBps;
} RenderDiskSeries;

void Render_DrawDisksGraph(RenderD2D *r, const RenderDiskSeries *disks, uint32_t diskCount);

void Render_DrawPerCore(RenderD2D *r,
                    uint32_t logicalCount,
                    const float *coreUsage,
                    const float *coreMHz,
                    const RingBufF *coreHistory);

void Render_DrawProcessTable(RenderD2D *r,
                        const ProcRow *rows,
                        uint32_t rowCount,
                        uint32_t totalRunningCount,
                        bool stacked,
                        uint32_t scrollRow,
                        uint32_t selectedPid);

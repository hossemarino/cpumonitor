#include "render_d2d.h"

#include <math.h>
#include <stdio.h>
#include <wchar.h>

#include <dxgi.h>
#include <dxgiformat.h>

#ifndef SAFE_RELEASE
#define SAFE_RELEASE_IFACE(type, x) do { if ((x)) { type##_Release((x)); (x) = NULL; } } while (0)
#endif

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

static void ensure_rt(RenderD2D *r)
{
    if (r->rt) {
        return;
    }

    RECT rc;
    GetClientRect(r->hwnd, &rc);
    r->width = (uint32_t)(rc.right - rc.left);
    r->height = (uint32_t)(rc.bottom - rc.top);

    D2D1_SIZE_U size;
    size.width = r->width;
    size.height = r->height;

    D2D1_RENDER_TARGET_PROPERTIES props;
    ZeroMemory(&props, sizeof(props));
    props.type = D2D1_RENDER_TARGET_TYPE_HARDWARE;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 0.0f;
    props.dpiY = 0.0f;
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps;
    ZeroMemory(&hwndProps, sizeof(hwndProps));
    hwndProps.hwnd = r->hwnd;
    hwndProps.pixelSize = size;
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    if (FAILED(ID2D1Factory_CreateHwndRenderTarget(r->factory, &props, &hwndProps, &r->rt))) {
        r->rt = NULL;
        return;
    }

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;

    D2D1_COLOR_F c;
    c.r = 1.0f; c.g = 1.0f; c.b = 1.0f; c.a = 1.0f;
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &c, NULL, &r->brushText);
    c.r = 0.7f; c.g = 0.7f; c.b = 0.7f; c.a = 1.0f;
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &c, NULL, &r->brushDim);
    c.r = 0.2f; c.g = 0.85f; c.b = 0.35f; c.a = 1.0f;
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &c, NULL, &r->brushGreen);
    c.r = 0.95f; c.g = 0.75f; c.b = 0.2f; c.a = 1.0f;
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &c, NULL, &r->brushYellow);
    c.r = 0.95f; c.g = 0.25f; c.b = 0.25f; c.a = 1.0f;
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &c, NULL, &r->brushRed);
    c.r = 0.18f; c.g = 0.18f; c.b = 0.18f; c.a = 1.0f;
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &c, NULL, &r->brushGrid);
}

static void drop_rt(RenderD2D *r)
{
    SAFE_RELEASE_IFACE(ID2D1SolidColorBrush, r->brushText);
    SAFE_RELEASE_IFACE(ID2D1SolidColorBrush, r->brushDim);
    SAFE_RELEASE_IFACE(ID2D1SolidColorBrush, r->brushGreen);
    SAFE_RELEASE_IFACE(ID2D1SolidColorBrush, r->brushYellow);
    SAFE_RELEASE_IFACE(ID2D1SolidColorBrush, r->brushRed);
    SAFE_RELEASE_IFACE(ID2D1SolidColorBrush, r->brushGrid);
    SAFE_RELEASE_IFACE(ID2D1HwndRenderTarget, r->rt);
}

static void draw_text(RenderD2D *r, float x, float y, float w, float h,
                      IDWriteTextFormat *fmt, ID2D1Brush *brush, const wchar_t *text)
{
    D2D1_RECT_F rect;
    rect.left = x;
    rect.top = y;
    rect.right = x + w;
    rect.bottom = y + h;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;
    ID2D1RenderTarget_DrawText(rt, text, (UINT32)wcslen(text), fmt, &rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

static ID2D1Brush* usage_brush(RenderD2D *r, float pct)
{
    if (pct < 50.0f) return (ID2D1Brush*)r->brushGreen;
    if (pct < 80.0f) return (ID2D1Brush*)r->brushYellow;
    return (ID2D1Brush*)r->brushRed;
}

bool Render_Init(RenderD2D *r, HWND hwnd)
{
    ZeroMemory(r, sizeof(*r));
    r->hwnd = hwnd;

    float dpiX = 96.0f, dpiY = 96.0f;
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        dpiX = (float)GetDeviceCaps(hdc, LOGPIXELSX);
        dpiY = (float)GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hwnd, hdc);
    }
    r->dpiScale = dpiX / 96.0f;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&r->factory))) {
        return false;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&r->dwFactory))) {
        return false;
    }

    if (FAILED(IDWriteFactory_CreateTextFormat(r->dwFactory,
                                                      L"Consolas", NULL,
                                                      DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL,
                                                      16.0f * r->dpiScale,
                                                      L"en-us",
                                                      &r->text))) {
        return false;
    }

    if (FAILED(IDWriteFactory_CreateTextFormat(r->dwFactory,
                                                      L"Consolas", NULL,
                                                      DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL,
                                                      13.0f * r->dpiScale,
                                                      L"en-us",
                                                      &r->textSmall))) {
        return false;
    }

    IDWriteTextFormat_SetWordWrapping(r->text, DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteTextFormat_SetWordWrapping(r->textSmall, DWRITE_WORD_WRAPPING_NO_WRAP);

    ensure_rt(r);
    return r->rt != NULL;
}

void Render_Shutdown(RenderD2D *r)
{
    drop_rt(r);
    SAFE_RELEASE_IFACE(IDWriteTextFormat, r->textSmall);
    SAFE_RELEASE_IFACE(IDWriteTextFormat, r->text);
    SAFE_RELEASE_IFACE(IDWriteFactory, r->dwFactory);
    SAFE_RELEASE_IFACE(ID2D1Factory, r->factory);
    ZeroMemory(r, sizeof(*r));
}

void Render_Resize(RenderD2D *r, uint32_t width, uint32_t height)
{
    r->width = width;
    r->height = height;
    if (r->rt) {
        D2D1_SIZE_U size;
        size.width = width;
        size.height = height;
        ID2D1HwndRenderTarget_Resize(r->rt, &size);
    }
}

void Render_Begin(RenderD2D *r)
{
    ensure_rt(r);
    if (!r->rt) return;
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)r->rt);
}

void Render_Clear(RenderD2D *r)
{
    if (!r->rt) return;

    // Reset layout each frame so tab changes don't reuse stale positions.
    r->tabsBottomY = 0.0f;
    r->headerBottomY = 0.0f;
    r->graphBottomY = 0.0f;

    D2D1_COLOR_F c;
    c.r = 0.06f; c.g = 0.06f; c.b = 0.06f; c.a = 1.0f;
    ID2D1RenderTarget_Clear((ID2D1RenderTarget *)r->rt, &c);
}

void Render_DrawTabs(RenderD2D *r, int activeTab)
{
    if (!r->rt) return;

    const float pad = 12.0f * r->dpiScale;
    const float tabH = 20.0f * r->dpiScale;
    const float tabW = 84.0f * r->dpiScale;
    const float gap = 8.0f * r->dpiScale;
    float x = pad;
    float y = pad;

    r->tabCpuX = x;
    r->tabCpuY = y;
    r->tabCpuW = tabW;
    r->tabCpuH = tabH;
    r->tabMemX = x + tabW + gap;
    r->tabMemY = y;
    r->tabMemW = tabW;
    r->tabMemH = tabH;
    r->tabGpuX = x + (tabW + gap) * 2.0f;
    r->tabGpuY = y;
    r->tabGpuW = tabW;
    r->tabGpuH = tabH;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;

    // Subtle background boxes using existing grid brush.
    D2D1_RECT_F cpuR = { r->tabCpuX, r->tabCpuY, r->tabCpuX + r->tabCpuW, r->tabCpuY + r->tabCpuH };
    D2D1_RECT_F memR = { r->tabMemX, r->tabMemY, r->tabMemX + r->tabMemW, r->tabMemY + r->tabMemH };
    D2D1_RECT_F gpuR = { r->tabGpuX, r->tabGpuY, r->tabGpuX + r->tabGpuW, r->tabGpuY + r->tabGpuH };
    ID2D1RenderTarget_FillRectangle(rt, &cpuR, (ID2D1Brush*)r->brushGrid);
    ID2D1RenderTarget_FillRectangle(rt, &memR, (ID2D1Brush*)r->brushGrid);
    ID2D1RenderTarget_FillRectangle(rt, &gpuR, (ID2D1Brush*)r->brushGrid);

    const ID2D1Brush *cpuBrush = (activeTab == 0) ? (ID2D1Brush*)r->brushText : (ID2D1Brush*)r->brushDim;
    const ID2D1Brush *memBrush = (activeTab == 1) ? (ID2D1Brush*)r->brushText : (ID2D1Brush*)r->brushDim;
    const ID2D1Brush *gpuBrush = (activeTab == 2) ? (ID2D1Brush*)r->brushText : (ID2D1Brush*)r->brushDim;
    draw_text(r, r->tabCpuX + 10.0f * r->dpiScale, r->tabCpuY + 1.0f * r->dpiScale,
              r->tabCpuW - 10.0f * r->dpiScale, r->tabCpuH, r->textSmall, (ID2D1Brush*)cpuBrush, L"CPU");
    draw_text(r, r->tabMemX + 10.0f * r->dpiScale, r->tabMemY + 1.0f * r->dpiScale,
              r->tabMemW - 10.0f * r->dpiScale, r->tabMemH, r->textSmall, (ID2D1Brush*)memBrush, L"Memory");
    draw_text(r, r->tabGpuX + 10.0f * r->dpiScale, r->tabGpuY + 1.0f * r->dpiScale,
              r->tabGpuW - 10.0f * r->dpiScale, r->tabGpuH, r->textSmall, (ID2D1Brush*)gpuBrush, L"GPU");

    // Advance layout start.
    r->tabsBottomY = y + tabH + (10.0f * r->dpiScale);
}

static void fmt_bytes_gb(uint64_t bytes, wchar_t *out, uint32_t outCch)
{
    if (!out || outCch == 0) return;
    const double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    swprintf(out, outCch, L"%.1f", gb);
}

void Render_DrawMemoryHeader(RenderD2D *r,
                             uint64_t totalPhysBytes,
                             uint64_t availPhysBytes,
                             uint64_t commitTotalBytes,
                             uint64_t commitLimitBytes,
                             double diskReadMBps,
                             double diskWriteMBps)
{
    if (!r->rt) return;

    const float pad = 12.0f * r->dpiScale;
    float y = (r->tabsBottomY > 0.0f) ? r->tabsBottomY : pad;

    wchar_t totGB[32], availGB[32], usedGB[32];
    fmt_bytes_gb(totalPhysBytes, totGB, 32);
    fmt_bytes_gb(availPhysBytes, availGB, 32);
    const uint64_t usedPhys = (totalPhysBytes > availPhysBytes) ? (totalPhysBytes - availPhysBytes) : 0;
    fmt_bytes_gb(usedPhys, usedGB, 32);
    const double usedPct = (totalPhysBytes > 0) ? (100.0 * (double)usedPhys / (double)totalPhysBytes) : 0.0;

    const uint64_t usedCommit = (commitLimitBytes > commitTotalBytes) ? commitTotalBytes : commitTotalBytes;
    wchar_t commitGB[32], limitGB[32];
    fmt_bytes_gb(usedCommit, commitGB, 32);
    fmt_bytes_gb(commitLimitBytes, limitGB, 32);
    const double commitPct = (commitLimitBytes > 0) ? (100.0 * (double)commitTotalBytes / (double)commitLimitBytes) : 0.0;

    wchar_t line1[256];
    wchar_t line2[256];
    swprintf(line1, 256, L"Memory: %s/%s GB used (%.0f%%) | Avail %s GB", usedGB, totGB, usedPct, availGB);
    swprintf(line2, 256, L"Commit: %s/%s GB (%.0f%%) | Disk R %.1f MB/s  W %.1f MB/s",
             commitGB, limitGB, commitPct, diskReadMBps, diskWriteMBps);

    draw_text(r, pad, y, (float)r->width - 2 * pad, 26.0f * r->dpiScale, r->text, (ID2D1Brush*)r->brushText, L"Memory");
    y += 22.0f * r->dpiScale;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 22.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line1);
    y += 18.0f * r->dpiScale;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 22.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line2);
    y += 18.0f * r->dpiScale;

    r->headerBottomY = y + (10.0f * r->dpiScale);
}

static void fmt_mb(double mb, wchar_t *out, uint32_t outCch)
{
    if (!out || outCch == 0) return;
    swprintf(out, outCch, L"%.0f", mb);
}

void Render_DrawGpuHeader(RenderD2D *r,
                          const wchar_t *adapterName,
                          uint32_t vendorId,
                          uint64_t dedicatedTotalBytes,
                          uint64_t sharedTotalBytes,
                          double dedicatedUsedMB,
                          double dedicatedLimitMB,
                          double sharedUsedMB,
                          double sharedLimitMB)
{
    if (!r->rt) return;

    const float pad = 12.0f * r->dpiScale;
    float y = (r->tabsBottomY > 0.0f) ? r->tabsBottomY : pad;

    wchar_t dedGB[32], shGB[32];
    fmt_bytes_gb(dedicatedTotalBytes, dedGB, 32);
    fmt_bytes_gb(sharedTotalBytes, shGB, 32);

    wchar_t du[32], dl[32], su[32], sl[32];
    fmt_mb(dedicatedUsedMB, du, 32);
    fmt_mb(dedicatedLimitMB, dl, 32);
    fmt_mb(sharedUsedMB, su, 32);
    fmt_mb(sharedLimitMB, sl, 32);

    wchar_t line1[320];
    if (adapterName && adapterName[0]) {
        swprintf(line1, (uint32_t)_countof(line1), L"%ls  (VEN_%04X)", adapterName, vendorId & 0xFFFF);
    } else {
        swprintf(line1, (uint32_t)_countof(line1), L"GPU adapter: N/A");
    }

    wchar_t line2[320];
    if (dedicatedLimitMB > 0.0 || sharedLimitMB > 0.0) {
        swprintf(line2, (uint32_t)_countof(line2),
                 L"Dedicated: %ls/%ls MB | Shared: %ls/%ls MB | Totals: %ls GB + %ls GB shared",
                 du, dl, su, sl, dedGB, shGB);
    } else {
        swprintf(line2, (uint32_t)_countof(line2),
                 L"Totals: %ls GB dedicated + %ls GB shared | Usage: N/A",
                 dedGB, shGB);
    }

    draw_text(r, pad, y, (float)r->width - 2 * pad, 26.0f * r->dpiScale, r->text, (ID2D1Brush*)r->brushText, L"GPU");
    y += 22.0f * r->dpiScale;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 22.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line1);
    y += 18.0f * r->dpiScale;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 22.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line2);
    y += 18.0f * r->dpiScale;

    r->headerBottomY = y + (10.0f * r->dpiScale);
}

void Render_DrawValueGraph(RenderD2D *r, const RingBufF *history, const wchar_t *title, float maxValue)
{
    if (!r->rt || !history || !title) return;
    if (history->count < 2) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;
    const float pad = 12.0f * r->dpiScale;
    const float baseTop = (r->graphBottomY > 0.0f) ? r->graphBottomY :
                          ((r->headerBottomY > 0.0f) ? r->headerBottomY :
                           ((r->tabsBottomY > 0.0f) ? r->tabsBottomY : pad));
    const float titleH = 18.0f * r->dpiScale;
    const float titlePad = 6.0f * r->dpiScale;
    const float top = baseTop + titleH + titlePad;
    const float graphH = 110.0f * r->dpiScale;
    const float axisW = 80.0f * r->dpiScale;
    const float left = pad + axisW;
    const float right = (float)r->width - pad;

    float maxV = maxValue;
    if (maxV <= 0.0f) {
        for (uint32_t i = 0; i < history->count; i++) {
            const float v = RingBuf_GetOldest(history, i);
            if (v > maxV) maxV = v;
        }
        if (maxV < 1.0f) maxV = 1.0f;
    }

    D2D1_RECT_F rect = { left, top, right, top + graphH };

    for (int i = 0; i <= 4; i++) {
        const float yy = top + (graphH * (float)i / 4.0f);
        D2D1_POINT_2F a = { left, yy };
        D2D1_POINT_2F b = { right, yy };
        ID2D1RenderTarget_DrawLine(rt, a, b, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        const float v = maxV * (float)(4 - i) / 4.0f;
        wchar_t lbl[40];
        swprintf(lbl, (uint32_t)_countof(lbl), L"%.0f MB", v);
        draw_text(r, pad, yy - 8.0f * r->dpiScale, axisW - 6.0f * r->dpiScale, 18.0f * r->dpiScale,
                  r->textSmall, (ID2D1Brush*)r->brushDim, lbl);
    }

    draw_text(r, left, baseTop + 2.0f * r->dpiScale, 520.0f * r->dpiScale, titleH,
              r->textSmall, (ID2D1Brush*)r->brushDim, title);

    const float w = right - left;
    D2D1_POINT_2F prev = { left, top + graphH };
    for (uint32_t i = 0; i < history->count; i++) {
        float v = RingBuf_GetOldest(history, i);
        if (v < 0.0f) v = 0.0f;
        if (v > maxV) v = maxV;
        const float x = left + (w * (float)i / (float)(history->cap - 1));
        const float yy = top + graphH - (graphH * (v / maxV));
        D2D1_POINT_2F p = { x, yy };
        if (i > 0) {
            ID2D1RenderTarget_DrawLine(rt, prev, p, (ID2D1Brush*)r->brushGreen, 2.0f, NULL);
        }
        prev = p;
    }

    ID2D1RenderTarget_DrawRectangle(rt, &rect, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);
    r->graphBottomY = top + graphH + (10.0f * r->dpiScale);
}

static void draw_percent_graph_internal(RenderD2D *r, const RingBufF *history, const wchar_t *title)
{
    if (!r->rt || !history || !title) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;
    const float pad = 12.0f * r->dpiScale;
    const float baseTop = (r->graphBottomY > 0.0f) ? r->graphBottomY :
                          ((r->headerBottomY > 0.0f) ? r->headerBottomY :
                           ((r->tabsBottomY > 0.0f) ? r->tabsBottomY : (76.0f * r->dpiScale)));
    const float titleH = 18.0f * r->dpiScale;
    const float titlePad = 6.0f * r->dpiScale;
    const float top = baseTop + titleH + titlePad;
    const float graphH = 110.0f * r->dpiScale;
    const float axisW = 52.0f * r->dpiScale;
    const float left = pad + axisW;
    const float right = (float)r->width - pad;

    D2D1_RECT_F rect = { left, top, right, top + graphH };

    for (int i = 0; i <= 4; i++) {
        const float yy = top + (graphH * (float)i / 4.0f);
        D2D1_POINT_2F a = { left, yy };
        D2D1_POINT_2F b = { right, yy };
        ID2D1RenderTarget_DrawLine(rt, a, b, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        const int pct = 100 - (i * 25);
        wchar_t lbl[16];
        swprintf(lbl, 16, L"%d%%", pct);
        draw_text(r, pad, yy - 8.0f * r->dpiScale, axisW - 6.0f * r->dpiScale, 18.0f * r->dpiScale,
                  r->textSmall, (ID2D1Brush*)r->brushDim, lbl);
    }

    draw_text(r, left, baseTop + 2.0f * r->dpiScale, 320.0f * r->dpiScale, titleH,
              r->textSmall, (ID2D1Brush*)r->brushDim, title);

    if (history->count >= 2) {
        const uint32_t n = history->count;
        const float w = right - left;

        D2D1_POINT_2F prev = { left, top + graphH };
        for (uint32_t i = 0; i < n; i++) {
            const float pct = clamp01(RingBuf_GetOldest(history, i));
            const float x = left + (w * (float)i / (float)(history->cap - 1));
            const float yy = top + graphH - (graphH * pct / 100.0f);
            D2D1_POINT_2F p = { x, yy };
            if (i > 0) {
                ID2D1RenderTarget_DrawLine(rt, prev, p, usage_brush(r, pct), 2.0f, NULL);
            }
            prev = p;
        }
    }

    ID2D1RenderTarget_DrawRectangle(rt, &rect, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);
    r->graphBottomY = top + graphH + (10.0f * r->dpiScale);
}

void Render_DrawPercentGraph(RenderD2D *r, const RingBufF *history, const wchar_t *title)
{
    draw_percent_graph_internal(r, history, title);
}

void Render_DrawDiskGraph(RenderD2D *r, const RingBufF *readMBpsHistory, const RingBufF *writeMBpsHistory)
{
    if (!r->rt || !readMBpsHistory || !writeMBpsHistory) return;
    if (readMBpsHistory->count < 2 && writeMBpsHistory->count < 2) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;
    const float pad = 12.0f * r->dpiScale;
    const float baseTop = (r->graphBottomY > 0.0f) ? r->graphBottomY : ((r->headerBottomY > 0.0f) ? r->headerBottomY : ((r->tabsBottomY > 0.0f) ? r->tabsBottomY : pad));
    const float titleH = 18.0f * r->dpiScale;
    const float titlePad = 6.0f * r->dpiScale;
    const float top = baseTop + titleH + titlePad;
    const float graphH = 110.0f * r->dpiScale;
    const float axisW = 70.0f * r->dpiScale;
    const float left = pad + axisW;
    const float right = (float)r->width - pad;

    // Find max for scaling.
    float maxV = 0.0f;
    for (uint32_t i = 0; i < readMBpsHistory->count; i++) {
        const float v = RingBuf_GetOldest(readMBpsHistory, i);
        if (v > maxV) maxV = v;
    }
    for (uint32_t i = 0; i < writeMBpsHistory->count; i++) {
        const float v = RingBuf_GetOldest(writeMBpsHistory, i);
        if (v > maxV) maxV = v;
    }
    if (maxV < 1.0f) maxV = 1.0f;

    D2D1_RECT_F rect = { left, top, right, top + graphH };

    // Grid + labels (0..max)
    for (int i = 0; i <= 4; i++) {
        const float yy = top + (graphH * (float)i / 4.0f);
        D2D1_POINT_2F a = { left, yy };
        D2D1_POINT_2F b = { right, yy };
        ID2D1RenderTarget_DrawLine(rt, a, b, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        const float v = maxV * (float)(4 - i) / 4.0f;
        wchar_t lbl[32];
        swprintf(lbl, 32, L"%.0f MB/s", v);
        draw_text(r, pad, yy - 8.0f * r->dpiScale, axisW - 6.0f * r->dpiScale, 18.0f * r->dpiScale,
                  r->textSmall, (ID2D1Brush*)r->brushDim, lbl);
    }

    draw_text(r, left, baseTop + 2.0f * r->dpiScale, 360.0f * r->dpiScale, titleH,
              r->textSmall, (ID2D1Brush*)r->brushDim, L"Disk throughput (history)  Read/Write");

    // Plot both series.
    const float w = right - left;

    if (readMBpsHistory->count >= 2) {
        D2D1_POINT_2F prev = { left, top + graphH };
        for (uint32_t i = 0; i < readMBpsHistory->count; i++) {
            const float v = RingBuf_GetOldest(readMBpsHistory, i);
            const float x = left + (w * (float)i / (float)(readMBpsHistory->cap - 1));
            const float yy = top + graphH - (graphH * (v / maxV));
            D2D1_POINT_2F p = { x, yy };
            if (i > 0) {
                ID2D1RenderTarget_DrawLine(rt, prev, p, (ID2D1Brush*)r->brushGreen, 2.0f, NULL);
            }
            prev = p;
        }
    }

    if (writeMBpsHistory->count >= 2) {
        D2D1_POINT_2F prev = { left, top + graphH };
        for (uint32_t i = 0; i < writeMBpsHistory->count; i++) {
            const float v = RingBuf_GetOldest(writeMBpsHistory, i);
            const float x = left + (w * (float)i / (float)(writeMBpsHistory->cap - 1));
            const float yy = top + graphH - (graphH * (v / maxV));
            D2D1_POINT_2F p = { x, yy };
            if (i > 0) {
                ID2D1RenderTarget_DrawLine(rt, prev, p, (ID2D1Brush*)r->brushYellow, 2.0f, NULL);
            }
            prev = p;
        }
    }

    ID2D1RenderTarget_DrawRectangle(rt, &rect, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);
    r->graphBottomY = top + graphH + (10.0f * r->dpiScale);
}

void Render_DrawDisksGraph(RenderD2D *r, const RenderDiskSeries *disks, uint32_t diskCount)
{
    if (!r->rt || !disks || diskCount == 0) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;
    const float pad = 12.0f * r->dpiScale;
    const float baseTop = (r->graphBottomY > 0.0f) ? r->graphBottomY :
                          ((r->headerBottomY > 0.0f) ? r->headerBottomY :
                           ((r->tabsBottomY > 0.0f) ? r->tabsBottomY : pad));

    const float titleH = 18.0f * r->dpiScale;
    const float titlePad = 6.0f * r->dpiScale;
    float y = baseTop;

    draw_text(r, pad, y, (float)r->width - 2 * pad, titleH,
              r->textSmall, (ID2D1Brush*)r->brushDim, L"Disks (history)  Read/Write");
    y += titleH + titlePad;

    const float labelW = 220.0f * r->dpiScale;
    const float right = (float)r->width - pad;
    const float graphW = right - (pad + labelW);

    // Reserve some space for the process table so "all disks" stays visible.
    const float reserveForProc = 180.0f * r->dpiScale;
    float avail = (float)r->height - pad - reserveForProc - y;
    if (avail < 60.0f * r->dpiScale) {
        avail = 60.0f * r->dpiScale;
    }

    float rowH = avail / (float)diskCount;
    const float minRowH = 14.0f * r->dpiScale;
    const float maxRowH = 22.0f * r->dpiScale;
    if (rowH < minRowH) rowH = minRowH;
    if (rowH > maxRowH) rowH = maxRowH;

    // Determine global max for scaling (MB/s).
    float maxV = 0.0f;
    for (uint32_t d = 0; d < diskCount; d++) {
        const RingBufF *rh = disks[d].readMBpsHistory;
        const RingBufF *wh = disks[d].writeMBpsHistory;
        if (rh) {
            for (uint32_t i = 0; i < rh->count; i++) {
                const float v = RingBuf_GetOldest(rh, i);
                if (v > maxV) maxV = v;
            }
        }
        if (wh) {
            for (uint32_t i = 0; i < wh->count; i++) {
                const float v = RingBuf_GetOldest(wh, i);
                if (v > maxV) maxV = v;
            }
        }
    }
    if (maxV < 1.0f) maxV = 1.0f;

    // Draw each disk as a small sparkline pair.
    for (uint32_t d = 0; d < diskCount; d++) {
        if (y + rowH > (float)r->height - pad) break;

        const float top = y;
        const float bottom = y + rowH;
        const float left = pad + labelW;
        const float gTop = top + 2.0f * r->dpiScale;
        const float gH = rowH - 4.0f * r->dpiScale;
        const float halfH = gH * 0.5f;
        const float gTopRead = gTop;
        const float gTopWrite = gTop + halfH;

        wchar_t lbl[256];
        const wchar_t *nm = disks[d].name ? disks[d].name : L"Disk";
        swprintf(lbl, 256, L"%s  R %.1f  W %.1f", nm, disks[d].readMBps, disks[d].writeMBps);
        draw_text(r, pad, top - 1.0f * r->dpiScale, labelW, rowH,
                  r->textSmall, (ID2D1Brush*)r->brushDim, lbl);

        D2D1_RECT_F rr = { left, top, right, bottom };
        ID2D1RenderTarget_DrawRectangle(rt, &rr, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        // Divider between Read (top) and Write (bottom)
        D2D1_POINT_2F divA = { left, gTopWrite };
        D2D1_POINT_2F divB = { right, gTopWrite };
        ID2D1RenderTarget_DrawLine(rt, divA, divB, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        const RingBufF *rh = disks[d].readMBpsHistory;
        const RingBufF *wh = disks[d].writeMBpsHistory;

        // Read (green) - top half
        if (rh && rh->count >= 2) {
            D2D1_POINT_2F prev = { left, gTopRead + halfH };
            for (uint32_t i = 0; i < rh->count; i++) {
                const float v = RingBuf_GetOldest(rh, i);
                const float x = left + graphW * (float)i / (float)(rh->cap - 1);
                const float yy = gTopRead + halfH - (halfH * (v / maxV));
                D2D1_POINT_2F p = { x, yy };
                if (i > 0) {
                    ID2D1RenderTarget_DrawLine(rt, prev, p, (ID2D1Brush*)r->brushGreen, 2.0f, NULL);
                }
                prev = p;
            }
        }

        // Write (yellow) - bottom half
        if (wh && wh->count >= 2) {
            D2D1_POINT_2F prev = { left, gTopWrite + halfH };
            for (uint32_t i = 0; i < wh->count; i++) {
                const float v = RingBuf_GetOldest(wh, i);
                const float x = left + graphW * (float)i / (float)(wh->cap - 1);
                const float yy = gTopWrite + halfH - (halfH * (v / maxV));
                D2D1_POINT_2F p = { x, yy };
                if (i > 0) {
                    ID2D1RenderTarget_DrawLine(rt, prev, p, (ID2D1Brush*)r->brushYellow, 2.0f, NULL);
                }
                prev = p;
            }
        }

        y += rowH;
    }

    r->graphBottomY = y + (8.0f * r->dpiScale);
}

static void format_uptime(uint64_t uptimeMs, wchar_t *out, uint32_t outCch)
{
    if (!out || outCch == 0) return;
    if (uptimeMs == 0) {
        wcscpy_s(out, outCch, L"0:00:00");
        return;
    }

    const uint64_t totalSec = uptimeMs / 1000ULL;
    const uint64_t days = totalSec / 86400ULL;
    const uint64_t rem = totalSec % 86400ULL;
    const uint64_t hours = rem / 3600ULL;
    const uint64_t mins = (rem % 3600ULL) / 60ULL;
    const uint64_t secs = rem % 60ULL;

    if (days > 0) {
        swprintf(out, outCch, L"%llud %02llu:%02llu:%02llu",
                 (unsigned long long)days,
                 (unsigned long long)hours,
                 (unsigned long long)mins,
                 (unsigned long long)secs);
    } else {
        swprintf(out, outCch, L"%llu:%02llu:%02llu",
                 (unsigned long long)hours,
                 (unsigned long long)mins,
                 (unsigned long long)secs);
    }
}

void Render_End(RenderD2D *r)
{
    if (!r->rt) return;
    HRESULT hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)r->rt, NULL, NULL);
    if (hr == D2DERR_RECREATE_TARGET) {
        drop_rt(r);
    }
}

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
                       float fanRpm)
{
    if (!r->rt) return;

    wchar_t line1[512];
    wchar_t line2[512];
    wchar_t line3[512];
    wchar_t line4[512];
    wchar_t line5[512];

    swprintf(line1, 512, L"CPU: %s | %s", cpu->vendor, cpu->brand);

    wchar_t upPart[64];
    format_uptime(uptimeMs, upPart, (uint32_t)(sizeof(upPart) / sizeof(upPart[0])));

    swprintf(line2, 512,
             L"Model: family %u model %u stepping %u | TSC %.3f GHz | packages %u cores %u logical %u | Up %s",
             cpu->family, cpu->model, cpu->stepping, cpu->tscGHz,
             cpu->packageCount, cpu->coreCount, cpu->logicalProcessorCount, upPart);

    // Fixed-width “table” segments to avoid column shifting.
    wchar_t tempVal[16];
    if (cpuTempC > 0.0f) swprintf(tempVal, 16, L"%5.1fC", cpuTempC);
    else wcscpy_s(tempVal, 16, L"  N/A ");

    wchar_t fanVal[16];
    if (fanRpm > 0.0f) swprintf(fanVal, 16, L"%5.0f", fanRpm);
    else wcscpy_s(fanVal, 16, L"  N/A ");

    wchar_t pwrVal[16];
    if (rates && rates->hasPowerWatts) swprintf(pwrVal, 16, L"%6.1f", rates->powerWatts);
    else wcscpy_s(pwrVal, 16, L"  N/A ");

    wchar_t thrVal[16];
    if (throttlePct > 0.0f) swprintf(thrVal, 16, L"%5.1f", throttlePct);
    else wcscpy_s(thrVal, 16, L" 0.0 ");

    wchar_t etwShort[160];
    if (etw && (etw->cswitchPerSec > 0.0 || etw->isrPerSec > 0.0 || etw->dpcPerSec > 0.0)) {
        swprintf(etwShort, (uint32_t)(sizeof(etwShort) / sizeof(etwShort[0])),
                 L"ETW cs%6.0f isr%6.0f dpc%6.0f",
                 etw->cswitchPerSec, etw->isrPerSec, etw->dpcPerSec);
    } else if (etwStatusText && etwStatusText[0] != 0) {
        wcsncpy(etwShort, etwStatusText, (uint32_t)(sizeof(etwShort) / sizeof(etwShort[0])) - 1);
        etwShort[(uint32_t)(sizeof(etwShort) / sizeof(etwShort[0])) - 1] = 0;
    } else {
        wcscpy_s(etwShort, (uint32_t)(sizeof(etwShort) / sizeof(etwShort[0])), L"ETW: no data");
    }

    // Line 3: utilization + PDH rates (fixed widths)
    swprintf(line3, 512,
             L"Usage %6.1f%%  Min %6.1f%%  Max %6.1f%%   Ctx/s %7.0f  IRQ/s %7.0f  DPC/s %7.0f  QLen %4.0f  Fchg/s %6.0f",
             totalUsage, usageMin, usageMax,
             rates ? rates->contextSwitchesPerSec : 0.0,
             rates ? rates->interruptsPerSec : 0.0,
             rates ? rates->dpcsPerSec : 0.0,
             rates ? rates->processorQueueLength : 0.0,
             cpuFreqChangesPerSec);

    // Line 4: “sensors” row (fixed widths) + short ETW summary/status
    if (sensorStatusText && sensorStatusText[0] != 0) {
        swprintf(line4, 512,
                 L"Pwr %ls W  Temp %ls  Fan %ls RPM  Thr %ls%%   %ls   %ls",
                 pwrVal, tempVal, fanVal, thrVal, etwShort, sensorStatusText);
    } else {
        swprintf(line4, 512,
                 L"Pwr %ls W  Temp %ls  Fan %ls RPM  Thr %ls%%   %ls",
                 pwrVal, tempVal, fanVal, thrVal, etwShort);
    }

    // Features + cache summary
    wchar_t feats[256];
    feats[0] = 0;
    if (cpu->sse) wcscat_s(feats, 256, L"SSE ");
    if (cpu->sse2) wcscat_s(feats, 256, L"SSE2 ");
    if (cpu->sse3) wcscat_s(feats, 256, L"SSE3 ");
    if (cpu->ssse3) wcscat_s(feats, 256, L"SSSE3 ");
    if (cpu->sse41) wcscat_s(feats, 256, L"SSE4.1 ");
    if (cpu->sse42) wcscat_s(feats, 256, L"SSE4.2 ");
    if (cpu->avx) wcscat_s(feats, 256, L"AVX ");
    if (cpu->avx2) wcscat_s(feats, 256, L"AVX2 ");
    if (cpu->fma) wcscat_s(feats, 256, L"FMA ");
    if (cpu->bmi1) wcscat_s(feats, 256, L"BMI1 ");
    if (cpu->bmi2) wcscat_s(feats, 256, L"BMI2 ");

    uint32_t l1d = 0, l1i = 0, l2 = 0, l3 = 0;
    for (uint32_t i = 0; i < cpu->cacheCount; i++) {
        const CpuCacheInfo *c = &cpu->caches[i];
        if (c->level == 1) {
            if (c->type == 1) l1d += c->sizeKB;
            else if (c->type == 2) l1i += c->sizeKB;
            else l1d += c->sizeKB;
        } else if (c->level == 2) {
            l2 += c->sizeKB;
        } else if (c->level == 3) {
            l3 += c->sizeKB;
        }
    }

    if (l1i > 0) {
        swprintf(line5, 512, L"Cache: L1d %uKB L1i %uKB | L2 %uKB | L3 %uKB | ISA: %s", l1d, l1i, l2, l3, feats);
    } else {
        swprintf(line5, 512, L"Cache: L1 %uKB | L2 %uKB | L3 %uKB | ISA: %s", l1d, l2, l3, feats);
    }

    const float pad = 12.0f * r->dpiScale;
    float y = (r->tabsBottomY > 0.0f) ? r->tabsBottomY : pad;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 30.0f * r->dpiScale, r->text, (ID2D1Brush*)r->brushText, line1);
    y += 22.0f * r->dpiScale;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 26.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line2);
    y += 18.0f * r->dpiScale;

    draw_text(r, pad, y, (float)r->width - 2 * pad, 26.0f * r->dpiScale, r->textSmall,
              usage_brush(r, clamp01(totalUsage)), line3);

    y += 18.0f * r->dpiScale;
    draw_text(r, pad, y, (float)r->width - 2 * pad, 26.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line4);

    y += 18.0f * r->dpiScale;
    draw_text(r, pad, y, (float)r->width - 2 * pad, 26.0f * r->dpiScale, r->textSmall, (ID2D1Brush*)r->brushDim, line5);

    // record header bottom for downstream layout
    // Extra padding so the graph caption never collides with the header.
    r->headerBottomY = y + (18.0f * r->dpiScale) + (18.0f * r->dpiScale);
}

void Render_DrawUsageGraph(RenderD2D *r, const RingBufF *history)
{
    if (!r->rt || !history) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;

    const float pad = 12.0f * r->dpiScale;
    const float baseTop = (r->headerBottomY > 0.0f) ? r->headerBottomY : (76.0f * r->dpiScale);
    const float titleH = 18.0f * r->dpiScale;
    const float titlePad = 6.0f * r->dpiScale;
    const float top = baseTop + titleH + titlePad;
    const float graphH = 130.0f * r->dpiScale;
    const float axisW = 52.0f * r->dpiScale;
    const float left = pad + axisW;
    const float right = (float)r->width - pad;

    D2D1_RECT_F rect;
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = top + graphH;

    // legend + grid (0/25/50/75/100)
    for (int i = 0; i <= 4; i++) {
        const float y = top + (graphH * (float)i / 4.0f);
        D2D1_POINT_2F a = { left, y };
        D2D1_POINT_2F b = { right, y };
        ID2D1RenderTarget_DrawLine(rt, a, b, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        // label: top line is 0%? actually graph maps top=100. Keep labels consistent.
        const int pct = 100 - (i * 25);
        wchar_t lbl[16];
        swprintf(lbl, 16, L"%d%%", pct);
        draw_text(r, pad, y - 8.0f * r->dpiScale, axisW - 6.0f * r->dpiScale, 18.0f * r->dpiScale,
                  r->textSmall, (ID2D1Brush*)r->brushDim, lbl);
    }

    // graph title / legend (sits between header and graph)
    draw_text(r, left, baseTop + 2.0f * r->dpiScale, 260.0f * r->dpiScale, titleH,
              r->textSmall, (ID2D1Brush*)r->brushDim, L"Total CPU (history)");

    if (history->count < 2) {
        return;
    }

    const uint32_t n = history->count;
    const float w = right - left;

    D2D1_POINT_2F prev;
    prev.x = left;
    prev.y = top + graphH;

    for (uint32_t i = 0; i < n; i++) {
        const float pct = clamp01(RingBuf_GetOldest(history, i));
        const float x = left + (w * (float)i / (float)(history->cap - 1));
        const float y = top + graphH - (graphH * pct / 100.0f);
        D2D1_POINT_2F p;
        p.x = x;
        p.y = y;
        if (i > 0) {
            ID2D1RenderTarget_DrawLine(rt, prev, p, usage_brush(r, pct), 2.0f, NULL);
        }
        prev = p;
    }

    // border
    ID2D1RenderTarget_DrawRectangle(rt, &rect, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

    r->graphBottomY = top + graphH + (10.0f * r->dpiScale);
}

void Render_DrawPerCore(RenderD2D *r,
                        uint32_t logicalCount,
                        const float *coreUsage,
                        const float *coreMHz,
                        const RingBufF *coreHistory)
{
    if (!r->rt || !coreUsage || logicalCount == 0) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;

    const float pad = 12.0f * r->dpiScale;
    float y = (r->graphBottomY > 0.0f) ? r->graphBottomY : ((76.0f + 140.0f) * r->dpiScale);

    const float barH = 14.0f * r->dpiScale;
    const float gap = 6.0f * r->dpiScale;

    const float labelW = 80.0f * r->dpiScale;
    const float right = (float)r->width - pad;
    const float barW = right - (pad + labelW);

    for (uint32_t i = 0; i < logicalCount; i++) {
        if (y + barH + gap > (float)r->height - pad) {
            break;
        }

        wchar_t label[64];
        if (coreMHz && coreMHz[i] > 0.0f) {
            swprintf(label, 64, L"CPU%u %4.0f", i, coreMHz[i]);
        } else {
            swprintf(label, 64, L"CPU%u", i);
        }

        draw_text(r, pad, y - 3.0f * r->dpiScale, labelW, barH + 6.0f * r->dpiScale,
                  r->textSmall, (ID2D1Brush*)r->brushDim, label);

        const float pct = clamp01(coreUsage[i]);
        const float fillW = barW * pct / 100.0f;

        D2D1_RECT_F back;
        back.left = pad + labelW;
        back.top = y;
        back.right = pad + labelW + barW;
        back.bottom = y + barH;

        D2D1_RECT_F fill;
        fill.left = pad + labelW;
        fill.top = y;
        fill.right = pad + labelW + fillW;
        fill.bottom = y + barH;

        ID2D1RenderTarget_FillRectangle(rt, &back, (ID2D1Brush*)r->brushGrid);
        ID2D1RenderTarget_FillRectangle(rt, &fill, usage_brush(r, pct));
        ID2D1RenderTarget_DrawRectangle(rt, &back, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        // optional mini history sparkline
        if (coreHistory) {
            const RingBufF *h = &coreHistory[i];
            if (h->count > 1) {
                const float sparkH = barH;
                const float sparkTop = y;
                const float sparkLeft = pad + labelW + barW - 140.0f * r->dpiScale;
                const float sparkRight = pad + labelW + barW;

                D2D1_POINT_2F prev;
                prev.x = sparkLeft;
                prev.y = sparkTop + sparkH;
                for (uint32_t s = 0; s < h->count; s++) {
                    float sp = clamp01(RingBuf_GetOldest(h, s));
                    float x = sparkLeft + (sparkRight - sparkLeft) * (float)s / (float)(h->cap - 1);
                    float yy = sparkTop + sparkH - (sparkH * sp / 100.0f);
                    D2D1_POINT_2F p;
                    p.x = x;
                    p.y = yy;
                    if (s > 0) {
                        ID2D1RenderTarget_DrawLine(rt, prev, p, (ID2D1Brush*)r->brushDim, 1.0f, NULL);
                    }
                    prev = p;
                }
            }
        }

        y += barH + gap;
    }

    // Advance layout cursor for downstream sections.
    r->graphBottomY = y + (8.0f * r->dpiScale);
}

void Render_DrawProcessTable(RenderD2D *r,
                             const ProcRow *rows,
                             uint32_t rowCount,
                             uint32_t totalRunningCount,
                             bool stacked,
                             uint32_t scrollRow,
                             uint32_t selectedPid)
{
    if (!r->rt || !rows || rowCount == 0) return;

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)r->rt;

    const float pad = 12.0f * r->dpiScale;
    float y = (r->graphBottomY > 0.0f) ? r->graphBottomY : ((76.0f + 140.0f) * r->dpiScale);

    const float titleH = 18.0f * r->dpiScale;
    r->procTitleY = y;
    r->procTitleH = titleH;
    r->procHelpX = 0.0f;
    r->procHelpY = 0.0f;
    r->procHelpW = 0.0f;
    r->procHelpH = 0.0f;
    wchar_t title[96];
    if (stacked) {
        swprintf(title, (uint32_t)(sizeof(title) / sizeof(title[0])),
                 L"Processes (stacked)  [%u groups, %u running]",
                 (unsigned)rowCount,
                 (unsigned)totalRunningCount);
    } else {
        swprintf(title, (uint32_t)(sizeof(title) / sizeof(title[0])), L"Processes (top by CPU)  [%u running]", (unsigned)rowCount);
    }
    draw_text(r, pad, y, (float)r->width - 2 * pad, titleH,
              r->textSmall, (ID2D1Brush*)r->brushDim, title);

    // Right-aligned help link (hit-tested in the app)
    const wchar_t *helpText = L"Process table detailed view";
    const float helpW = 260.0f * r->dpiScale;
    const float helpX = (float)r->width - pad - helpW;
    draw_text(r, helpX, y, helpW, titleH,
              r->textSmall, (ID2D1Brush*)r->brushText, helpText);
    r->procHelpX = helpX;
    r->procHelpY = y;
    r->procHelpW = helpW;
    r->procHelpH = titleH;
    y += titleH + 6.0f * r->dpiScale;

    const float rowH = 18.0f * r->dpiScale;
    r->procRowH = rowH;
    const float left = pad;
    const float right = (float)r->width - pad;
    const float tableW = right - left;

    // Column widths (keep stable; last column gets remaining)
    const float colPid = 64.0f * r->dpiScale;
    const float colCpu = 68.0f * r->dpiScale;
    const float colMem = 84.0f * r->dpiScale;
    const float colOwner = 180.0f * r->dpiScale;
    const float colNet = 220.0f * r->dpiScale;
    const float colName = 140.0f * r->dpiScale;
    const float colPath = (right - left) - (colPid + colCpu + colMem + colOwner + colNet + colName);
    if (colPath < 120.0f * r->dpiScale) {
        // If window is too narrow, drop path width but keep it non-negative.
        // (Still draws; just heavily clipped.)
    }

    // Header background
    D2D1_RECT_F hdr;
    hdr.left = left;
    hdr.top = y;
    hdr.right = right;
    hdr.bottom = y + rowH;
    r->procHeaderY = y;

    // Store column boundaries for hit-testing.
    r->procColX[0] = left;
    r->procColX[1] = left + colPid;
    r->procColX[2] = r->procColX[1] + colCpu;
    r->procColX[3] = r->procColX[2] + colMem;
    r->procColX[4] = r->procColX[3] + colOwner;
    r->procColX[5] = r->procColX[4] + colNet;
    r->procColX[6] = r->procColX[5] + colName;
    r->procColX[7] = right;
    ID2D1RenderTarget_FillRectangle(rt, &hdr, (ID2D1Brush*)r->brushGrid);
    ID2D1RenderTarget_DrawRectangle(rt, &hdr, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

    float x = left + 6.0f * r->dpiScale;
    draw_text(r, x, y, colPid, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"PID");
    x += colPid;
    draw_text(r, x, y, colCpu, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"CPU% ");
    x += colCpu;
    draw_text(r, x, y, colMem, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"Mem(MB)");
    x += colMem;
    draw_text(r, x, y, colOwner, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"Owner");
    x += colOwner;
    draw_text(r, x, y, colNet, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"Net(remote)");
    x += colNet;
    draw_text(r, x, y, colName, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"Name");
    x += colName;
    draw_text(r, x, y, colPath, rowH, r->textSmall, (ID2D1Brush*)r->brushText, L"Path");

    y += rowH;

    const float tableTop = r->procHeaderY;
    const float tableBottomLimit = (float)r->height - pad;
    const float tableH = tableBottomLimit - tableTop;

    // Visible rows inside the table area (excluding header row)
    const float availForRows = tableH - rowH;
    const uint32_t visibleRows = (availForRows > rowH) ? (uint32_t)(availForRows / rowH) : 0;
    r->procVisibleRows = visibleRows;
    r->procRowCount = rowCount;

    if (scrollRow >= rowCount) scrollRow = (rowCount > 0) ? (rowCount - 1) : 0;
    r->procScrollRow = scrollRow;

    const uint32_t start = scrollRow;
    const uint32_t end = (visibleRows == 0) ? start : (start + visibleRows);
    const uint32_t maxEnd = (end > rowCount) ? rowCount : end;

    for (uint32_t i = start; i < maxEnd; i++) {
        if (y + rowH > (float)r->height - pad) break;

        // Row border
        D2D1_RECT_F rr;
        rr.left = left;
        rr.top = y;
        rr.right = right;
        rr.bottom = y + rowH;
        ID2D1RenderTarget_DrawRectangle(rt, &rr, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);

        const ProcRow *pr = &rows[i];

        if (selectedPid != 0 && pr->pid == selectedPid) {
            // Highlight selection
            ID2D1RenderTarget_DrawRectangle(rt, &rr, (ID2D1Brush*)r->brushYellow, 2.0f, NULL);
        }

        wchar_t pid[16];
        wchar_t cpu[24];
        wchar_t mem[24];

        swprintf(pid, 16, L"%u", (unsigned)pr->pid);
        swprintf(cpu, 24, L"%.1f", pr->cpuPct);
        swprintf(mem, 24, L"%.1f", (double)pr->workingSetBytes / (1024.0 * 1024.0));

        const wchar_t *owner = pr->owner[0] ? pr->owner : L"";
        const wchar_t *net = pr->hasNet ? pr->netRemote : L"";
        const wchar_t *name = pr->name[0] ? pr->name : L"";
        const wchar_t *path = pr->path[0] ? pr->path : L"";

        x = left + 6.0f * r->dpiScale;
        draw_text(r, x, y, colPid, rowH, r->textSmall, (ID2D1Brush*)r->brushDim, pid);
        x += colPid;
        draw_text(r, x, y, colCpu, rowH, r->textSmall, usage_brush(r, clamp01(pr->cpuPct)), cpu);
        x += colCpu;
        draw_text(r, x, y, colMem, rowH, r->textSmall, (ID2D1Brush*)r->brushDim, mem);
        x += colMem;
        draw_text(r, x, y, colOwner, rowH, r->textSmall, (ID2D1Brush*)r->brushDim, owner);
        x += colOwner;
        draw_text(r, x, y, colNet, rowH, r->textSmall, (ID2D1Brush*)r->brushDim, net);
        x += colNet;
        draw_text(r, x, y, colName, rowH, r->textSmall, (ID2D1Brush*)r->brushText, name);
        x += colName;
        draw_text(r, x, y, colPath, rowH, r->textSmall, (ID2D1Brush*)r->brushDim, path);

        y += rowH;
    }

    // Record table rect for input hit-testing (includes header).
    r->procTableX = left;
    r->procTableY = tableTop;
    r->procTableW = tableW;
    r->procTableH = tableH;

    // Scrollbar thumb (simple)
    if (rowCount > 0 && visibleRows > 0 && rowCount > visibleRows) {
        const float sbW = 8.0f * r->dpiScale;
        const float sbX = right - sbW;
        const float sbY0 = tableTop + rowH;
        const float sbH = tableH - rowH;
        const float thumbH = (sbH * (float)visibleRows) / (float)rowCount;
        const float maxScroll = (float)(rowCount - visibleRows);
        const float t = (maxScroll > 0.0f) ? ((float)scrollRow / maxScroll) : 0.0f;
        const float thumbY = sbY0 + (sbH - thumbH) * t;

        D2D1_RECT_F sb;
        sb.left = sbX;
        sb.top = sbY0;
        sb.right = right;
        sb.bottom = sbY0 + sbH;
        ID2D1RenderTarget_FillRectangle(rt, &sb, (ID2D1Brush*)r->brushGrid);

        D2D1_RECT_F th;
        th.left = sbX;
        th.top = thumbY;
        th.right = right;
        th.bottom = thumbY + thumbH;
        ID2D1RenderTarget_FillRectangle(rt, &th, (ID2D1Brush*)r->brushDim);
        ID2D1RenderTarget_DrawRectangle(rt, &sb, (ID2D1Brush*)r->brushGrid, 1.0f, NULL);
    }

    r->graphBottomY = y + (8.0f * r->dpiScale);
}

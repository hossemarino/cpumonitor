#include "app.h"

#include "help_window.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <windowsx.h>
#include <psapi.h>
#include <shellapi.h>

static bool file_exists_w(const wchar_t *path)
{
    if (!path || !*path) return false;
    const DWORD a = GetFileAttributesW(path);
    return (a != INVALID_FILE_ATTRIBUTES) && ((a & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static bool path_dirname_w(wchar_t *path)
{
    if (!path) return false;
    size_t n = wcslen(path);
    while (n > 0) {
        wchar_t c = path[n - 1];
        if (c == L'\\' || c == L'/') {
            path[n - 1] = 0;
            return true;
        }
        n--;
    }
    return false;
}

static bool try_start_bundled_provider(App *app)
{
    if (!app) return false;
    if (app->providerProcess) return true;

    wchar_t exePath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exePath, (DWORD)(sizeof(exePath) / sizeof(exePath[0])));
    if (n == 0 || n >= (DWORD)(sizeof(exePath) / sizeof(exePath[0]))) return false;
    if (!path_dirname_w(exePath)) return false;

    wchar_t provPath[MAX_PATH];
    swprintf(provPath, (uint32_t)(sizeof(provPath) / sizeof(provPath[0])), L"%ls\\CCM_sensor_provider.exe", exePath);
    if (!file_exists_w(provPath)) {
        // Alternate name (older/dev naming).
        swprintf(provPath, (uint32_t)(sizeof(provPath) / sizeof(provPath[0])), L"%ls\\ccm_sensor_provider.exe", exePath);
        if (!file_exists_w(provPath)) {
            return false;
        }
    }

    wchar_t cmd[512];
    swprintf(cmd, (uint32_t)(sizeof(cmd) / sizeof(cmd[0])), L"\"%ls\" --parent %lu", provPath, (unsigned long)GetCurrentProcessId());

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(provPath, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS,
                        NULL, exePath, &si, &pi)) {
        return false;
    }

    // Keep only the process handle; provider exits when CCM exits (parent pid watch).
    CloseHandle(pi.hThread);
    app->providerProcess = pi.hProcess;
    app->providerPid = pi.dwProcessId;
    return true;
}

static const wchar_t *kWndClass = L"ccm_wnd";

enum {
    IDM_VIEW_CPU0_15 = 1001,
    IDM_VIEW_STACK_PROCS = 1002,
    IDM_PROC_END_TASK = 1501,
    IDM_PROC_KILL = 1502,
    IDM_PROC_COPY = 1503,
    IDM_PROC_OPEN_LOCATION = 1504,
    IDM_PROC_COPY_ALL_VISIBLE = 1505,
    IDM_PROC_COPY_ALL = 1506,
    IDM_PROC_COPY_PID = 1507,
    IDM_PROC_COPY_PATH = 1508,
    IDM_HELP_METRICS = 2001,
    IDM_HELP_ABOUT = 2002,
    IDM_HELP_MEMORY_DISKS = 2003,
    IDM_HELP_GPU = 2004,
};

typedef struct TextBufW {
    wchar_t *p;
    size_t len;
    size_t cap;
} TextBufW;

static const ProcRow *find_row_by_pid(const ProcTable *pt, uint32_t pid);

static bool textbuf_ensure_w(TextBufW *tb, size_t need)
{
    if (!tb) return false;
    if (tb->cap >= need) return true;

    size_t newCap = tb->cap ? tb->cap : 4096;
    while (newCap < need) {
        newCap *= 2;
        if (newCap < tb->cap) return false;
    }

    wchar_t *np = (wchar_t *)realloc(tb->p, newCap * sizeof(wchar_t));
    if (!np) return false;
    tb->p = np;
    tb->cap = newCap;
    return true;
}

static bool textbuf_append_w(TextBufW *tb, const wchar_t *s)
{
    if (!tb || !s) return false;
    const size_t n = wcslen(s);
    if (!textbuf_ensure_w(tb, tb->len + n + 1)) return false;
    memcpy(tb->p + tb->len, s, n * sizeof(wchar_t));
    tb->len += n;
    tb->p[tb->len] = 0;
    return true;
}

static bool textbuf_appendf_w(TextBufW *tb, const wchar_t *fmt, ...)
{
    if (!tb || !fmt) return false;

    wchar_t tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vswprintf(tmp, (uint32_t)(sizeof(tmp) / sizeof(tmp[0])), fmt, ap);
    va_end(ap);
    if (n <= 0) return false;
    return textbuf_append_w(tb, tmp);
}

static void textbuf_free_w(TextBufW *tb)
{
    if (!tb) return;
    free(tb->p);
    tb->p = NULL;
    tb->len = 0;
    tb->cap = 0;
}

static bool clipboard_set_text(HWND hwnd, const wchar_t *text)
{
    if (!hwnd || !text) return false;

    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();

    const size_t cch = wcslen(text) + 1;
    const size_t cb = cch * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cb);
    if (!h) {
        CloseClipboard();
        return false;
    }

    void *p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        CloseClipboard();
        return false;
    }
    memcpy(p, text, cb);
    GlobalUnlock(h);

    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

static int wcmp_insensitive_local(const wchar_t *a, const wchar_t *b)
{
    if (!a) a = L"";
    if (!b) b = L"";
#ifdef _MSC_VER
    return _wcsicmp(a, b);
#else
    return wcsicmp(a, b);
#endif
}

static const ProcRow *app_proc_view_rows(const App *app, uint32_t *outCount)
{
    if (outCount) *outCount = 0;
    if (!app) return NULL;

    if (app->procStacked) {
        if (outCount) *outCount = app->procViewCount;
        return app->procViewRows;
    }

    if (outCount) *outCount = app->procTable.rowCount;
    return app->procTable.rows;
}

static bool ensure_proc_view_rows(App *app, uint32_t want)
{
    if (!app) return false;
    if (want <= app->procViewCap) return true;
    uint32_t newCap = app->procViewCap ? (app->procViewCap * 2u) : 256u;
    while (newCap < want) {
        if (newCap > (UINT32_MAX / 2u)) {
            newCap = want;
            break;
        }
        newCap *= 2u;
    }
    void *p = realloc(app->procViewRows, (size_t)newCap * sizeof(*app->procViewRows));
    if (!p) return false;
    app->procViewRows = (ProcRow *)p;
    app->procViewCap = newCap;
    return true;
}

static bool ensure_proc_groups(App *app, uint32_t want)
{
    if (!app) return false;
    if (want <= app->procGroupCap) return true;
    uint32_t newCap = app->procGroupCap ? (app->procGroupCap * 2u) : 64u;
    while (newCap < want) {
        if (newCap > (UINT32_MAX / 2u)) {
            newCap = want;
            break;
        }
        newCap *= 2u;
    }
    void *p = realloc(app->procGroups, (size_t)newCap * sizeof(*app->procGroups));
    if (!p) return false;
    app->procGroups = (struct ProcGroupIndex *)p;
    app->procGroupCap = newCap;
    return true;
}

static bool ensure_proc_group_members(App *app, uint32_t want)
{
    if (!app) return false;
    if (want <= app->procGroupMemberCap) return true;
    uint32_t newCap = app->procGroupMemberCap ? (app->procGroupMemberCap * 2u) : 256u;
    while (newCap < want) {
        if (newCap > (UINT32_MAX / 2u)) {
            newCap = want;
            break;
        }
        newCap *= 2u;
    }
    void *p = realloc(app->procGroupMembers, (size_t)newCap * sizeof(*app->procGroupMembers));
    if (!p) return false;
    app->procGroupMembers = (uint32_t *)p;
    app->procGroupMemberCap = newCap;
    return true;
}

static int find_group_by_name(App *app, const wchar_t *baseName)
{
    if (!app || !baseName) return -1;
    for (uint32_t i = 0; i < app->procGroupCount; i++) {
        if (wcmp_insensitive_local(app->procGroups[i].baseName, baseName) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const struct ProcGroupIndex *find_group_by_leader_pid(const App *app, uint32_t leaderPid)
{
    if (!app || leaderPid == 0) return NULL;
    for (uint32_t i = 0; i < app->procGroupCount; i++) {
        if (app->procGroups[i].leaderPid == leaderPid) {
            return &app->procGroups[i];
        }
    }
    return NULL;
}

static const struct ProcGroupIndex *find_group_by_name_const(const App *app, const wchar_t *baseName)
{
    if (!app || !baseName) return NULL;
    for (uint32_t i = 0; i < app->procGroupCount; i++) {
        if (wcmp_insensitive_local(app->procGroups[i].baseName, baseName) == 0) {
            return &app->procGroups[i];
        }
    }
    return NULL;
}

static int row_cmp_cpu_desc_local(const void *a, const void *b)
{
    const ProcRow *ra = (const ProcRow *)a;
    const ProcRow *rb = (const ProcRow *)b;
    if (ra->cpuPct < rb->cpuPct) return 1;
    if (ra->cpuPct > rb->cpuPct) return -1;
    if (ra->workingSetBytes < rb->workingSetBytes) return 1;
    if (ra->workingSetBytes > rb->workingSetBytes) return -1;
    if (ra->pid < rb->pid) return -1;
    if (ra->pid > rb->pid) return 1;
    return 0;
}

typedef struct GroupAgg {
    float cpuSum;
    uint64_t memSum;
    bool ownerSame;
    bool pathSame;
    wchar_t owner[96];
    wchar_t path[MAX_PATH];
    bool hasAnyNet;
    wchar_t netRemote[64];
} GroupAgg;

static bool ensure_group_aggs(GroupAgg **pAggs, uint32_t *pCap, uint32_t want)
{
    if (!pAggs || !pCap) return false;
    if (want <= *pCap) return true;
    uint32_t newCap = (*pCap) ? (*pCap * 2u) : 64u;
    while (newCap < want) {
        if (newCap > (UINT32_MAX / 2u)) {
            newCap = want;
            break;
        }
        newCap *= 2u;
    }
    void *p = realloc(*pAggs, (size_t)newCap * sizeof(**pAggs));
    if (!p) return false;
    *pAggs = (GroupAgg *)p;
    *pCap = newCap;
    return true;
}

static void App_RebuildProcView(App *app)
{
    if (!app) return;

    if (!app->procStacked) {
        app->procViewCount = 0;
        app->procGroupCount = 0;
        app->procGroupMemberCount = 0;
        return;
    }

    const ProcRow *raw = app->procTable.rows;
    const uint32_t rawCount = app->procTable.rowCount;
    if (!raw || rawCount == 0) {
        app->procViewCount = 0;
        app->procGroupCount = 0;
        app->procGroupMemberCount = 0;
        return;
    }

    app->procGroupCount = 0;
    app->procGroupMemberCount = 0;

    // Temporary per-group aggregation.
    static GroupAgg *aggs = NULL;
    static uint32_t aggsCap = 0;

    for (uint32_t i = 0; i < rawCount; i++) {
        const ProcRow *pr = &raw[i];
        if (pr->pid == 0) continue;

        const int gi = find_group_by_name(app, pr->name);
        int useGi = gi;
        if (useGi < 0) {
            if (!ensure_proc_groups(app, app->procGroupCount + 1)) break;
            if (!ensure_group_aggs(&aggs, &aggsCap, app->procGroupCount + 1)) break;

            const uint32_t idx = app->procGroupCount++;
            memset(&app->procGroups[idx], 0, sizeof(app->procGroups[idx]));
            wcsncpy(app->procGroups[idx].baseName, pr->name, (uint32_t)(sizeof(app->procGroups[idx].baseName) / sizeof(app->procGroups[idx].baseName[0])) - 1);
            app->procGroups[idx].baseName[(uint32_t)(sizeof(app->procGroups[idx].baseName) / sizeof(app->procGroups[idx].baseName[0])) - 1] = 0;
            app->procGroups[idx].leaderPid = pr->pid;
            app->procGroups[idx].memberStart = app->procGroupMemberCount;
            app->procGroups[idx].memberCount = 0;

            memset(&aggs[idx], 0, sizeof(aggs[idx]));
            aggs[idx].cpuSum = 0.0f;
            aggs[idx].memSum = 0;
            aggs[idx].ownerSame = true;
            aggs[idx].pathSame = true;
            wcsncpy(aggs[idx].owner, pr->owner, (uint32_t)(sizeof(aggs[idx].owner) / sizeof(aggs[idx].owner[0])) - 1);
            aggs[idx].owner[(uint32_t)(sizeof(aggs[idx].owner) / sizeof(aggs[idx].owner[0])) - 1] = 0;
            wcsncpy(aggs[idx].path, pr->path, (uint32_t)(sizeof(aggs[idx].path) / sizeof(aggs[idx].path[0])) - 1);
            aggs[idx].path[(uint32_t)(sizeof(aggs[idx].path) / sizeof(aggs[idx].path[0])) - 1] = 0;
            aggs[idx].hasAnyNet = pr->hasNet;
            if (pr->hasNet) {
                wcsncpy(aggs[idx].netRemote, pr->netRemote, (uint32_t)(sizeof(aggs[idx].netRemote) / sizeof(aggs[idx].netRemote[0])) - 1);
                aggs[idx].netRemote[(uint32_t)(sizeof(aggs[idx].netRemote) / sizeof(aggs[idx].netRemote[0])) - 1] = 0;
            } else {
                aggs[idx].netRemote[0] = 0;
            }

            useGi = (int)idx;
        }

        if (useGi < 0) continue;

        if (!ensure_proc_group_members(app, app->procGroupMemberCount + 1)) break;
        app->procGroupMembers[app->procGroupMemberCount++] = pr->pid;
        app->procGroups[useGi].memberCount++;

        if (pr->pid < app->procGroups[useGi].leaderPid) {
            app->procGroups[useGi].leaderPid = pr->pid;
        }

        aggs[useGi].cpuSum += pr->cpuPct;
        aggs[useGi].memSum += pr->workingSetBytes;

        if (aggs[useGi].ownerSame) {
            if (wcmp_insensitive_local(aggs[useGi].owner, pr->owner) != 0) {
                aggs[useGi].ownerSame = false;
            }
        }
        if (aggs[useGi].pathSame) {
            if (wcmp_insensitive_local(aggs[useGi].path, pr->path) != 0) {
                aggs[useGi].pathSame = false;
            }
        }
        if (!aggs[useGi].hasAnyNet && pr->hasNet) {
            aggs[useGi].hasAnyNet = true;
            wcsncpy(aggs[useGi].netRemote, pr->netRemote, (uint32_t)(sizeof(aggs[useGi].netRemote) / sizeof(aggs[useGi].netRemote[0])) - 1);
            aggs[useGi].netRemote[(uint32_t)(sizeof(aggs[useGi].netRemote) / sizeof(aggs[useGi].netRemote[0])) - 1] = 0;
        }
    }

    // Build view rows from groups (+ optional expanded group members).
    uint32_t extra = 0;
    if (app->procHasExpanded && app->procExpandedBaseName[0]) {
        const struct ProcGroupIndex *eg = find_group_by_name_const(app, app->procExpandedBaseName);
        if (eg && eg->memberCount > 1) {
            // We'll show members except leader PID to avoid PID collisions with group header.
            extra = eg->memberCount - 1;
        }
    }

    if (!ensure_proc_view_rows(app, app->procGroupCount + extra)) {
        app->procViewCount = 0;
        return;
    }
    app->procViewCount = 0;

    for (uint32_t i = 0; i < app->procGroupCount; i++) {
        ProcRow vr;
        memset(&vr, 0, sizeof(vr));

        vr.pid = app->procGroups[i].leaderPid;
        vr.cpuPct = aggs[i].cpuSum;
        if (vr.cpuPct < 0.0f) vr.cpuPct = 0.0f;
        if (vr.cpuPct > 100.0f) vr.cpuPct = 100.0f;
        vr.workingSetBytes = aggs[i].memSum;

        // Network endpoints are per-process; aggregated view doesn't try to summarize.
        vr.hasNet = false;
        vr.netRemote[0] = 0;

        if (app->procGroups[i].memberCount > 1) {
            swprintf(vr.name, (uint32_t)(sizeof(vr.name) / sizeof(vr.name[0])),
                     L"%ls (%u)",
                     app->procGroups[i].baseName,
                     (unsigned)app->procGroups[i].memberCount);
        } else {
            wcsncpy(vr.name, app->procGroups[i].baseName, (uint32_t)(sizeof(vr.name) / sizeof(vr.name[0])) - 1);
            vr.name[(uint32_t)(sizeof(vr.name) / sizeof(vr.name[0])) - 1] = 0;
        }

        if (aggs[i].ownerSame) {
            wcsncpy(vr.owner, aggs[i].owner, (uint32_t)(sizeof(vr.owner) / sizeof(vr.owner[0])) - 1);
            vr.owner[(uint32_t)(sizeof(vr.owner) / sizeof(vr.owner[0])) - 1] = 0;
        } else {
            vr.owner[0] = 0;
        }

        if (aggs[i].pathSame) {
            wcsncpy(vr.path, aggs[i].path, (uint32_t)(sizeof(vr.path) / sizeof(vr.path[0])) - 1);
            vr.path[(uint32_t)(sizeof(vr.path) / sizeof(vr.path[0])) - 1] = 0;
        } else {
            vr.path[0] = 0;
        }

        app->procViewRows[app->procViewCount++] = vr;

        // Insert expanded member rows directly under the expanded group header.
        if (app->procHasExpanded && app->procExpandedBaseName[0] &&
            wcmp_insensitive_local(app->procExpandedBaseName, app->procGroups[i].baseName) == 0 &&
            app->procGroups[i].memberCount > 1) {

            // Collect member rows from the raw table.
            const uint32_t m = app->procGroups[i].memberCount;
            ProcRow *tmp = (ProcRow *)calloc(m, sizeof(ProcRow));
            uint32_t tmpCount = 0;
            if (tmp) {
                for (uint32_t mi = 0; mi < m; mi++) {
                    const uint32_t mpid = app->procGroupMembers[app->procGroups[i].memberStart + mi];
                    if (mpid == app->procGroups[i].leaderPid) continue;
                    const ProcRow *rawRow = find_row_by_pid(&app->procTable, mpid);
                    if (!rawRow) continue;
                    tmp[tmpCount++] = *rawRow;

                    // Visual hint: indent member rows.
                    wchar_t nn[64];
                    wcsncpy(nn, tmp[tmpCount - 1].name, (uint32_t)(sizeof(nn) / sizeof(nn[0])) - 1);
                    nn[(uint32_t)(sizeof(nn) / sizeof(nn[0])) - 1] = 0;
                    swprintf(tmp[tmpCount - 1].name,
                             (uint32_t)(sizeof(tmp[tmpCount - 1].name) / sizeof(tmp[tmpCount - 1].name[0])),
                             L"  %ls",
                             nn);
                }

                if (tmpCount > 1) {
                    qsort(tmp, tmpCount, sizeof(tmp[0]), row_cmp_cpu_desc_local);
                }

                for (uint32_t k = 0; k < tmpCount; k++) {
                    if (app->procViewCount < app->procViewCap) {
                        app->procViewRows[app->procViewCount++] = tmp[k];
                    }
                }
                free(tmp);
            }
        }
    }

    // Sort group rows only; keep expanded member rows directly beneath their group.
    // Implementation approach: if nothing is expanded, sort the whole view.
    if (!app->procHasExpanded || !app->procExpandedBaseName[0]) {
        ProcTable st;
        memset(&st, 0, sizeof(st));
        st.rows = app->procViewRows;
        st.rowCount = app->procViewCount;
        ProcTable_Sort(&st, app->procSortKey, app->procSortAsc);
    } else {
        // Build a temporary group-only array, sort it, then re-insert members for the expanded group.
        const struct ProcGroupIndex *eg = find_group_by_name_const(app, app->procExpandedBaseName);
        const uint32_t groupCount = app->procGroupCount;
        ProcRow *sortedGroups = (ProcRow *)calloc(groupCount, sizeof(ProcRow));
        if (sortedGroups) {
            uint32_t gcount = 0;
            for (uint32_t i = 0; i < app->procGroupCount; i++) {
                // Group headers are the first occurrence for each group in procViewRows at build time.
                // We can reconstruct them by building again from procGroups/aggs would be expensive; instead
                // take the already-built group headers from procViewRows by matching leaderPid and baseName.
                // We know the group header was appended before any members for that group.
                // So locate it in procViewRows by scanning from 0.
                for (uint32_t j = 0; j < app->procViewCount; j++) {
                    if (app->procViewRows[j].pid == app->procGroups[i].leaderPid) {
                        // Also ensure this row looks like a group header for this group.
                        if (wcsstr(app->procViewRows[j].name, app->procGroups[i].baseName) == app->procViewRows[j].name ||
                            wcsstr(app->procViewRows[j].name, app->procGroups[i].baseName) != NULL) {
                            sortedGroups[gcount++] = app->procViewRows[j];
                            break;
                        }
                    }
                }
            }

            ProcTable st;
            memset(&st, 0, sizeof(st));
            st.rows = sortedGroups;
            st.rowCount = gcount;
            ProcTable_Sort(&st, app->procSortKey, app->procSortAsc);

            // Rewrite procViewRows: sorted group headers, and after the expanded one insert members.
            uint32_t out = 0;
            for (uint32_t gi = 0; gi < gcount; gi++) {
                app->procViewRows[out++] = sortedGroups[gi];
                if (eg && sortedGroups[gi].pid == eg->leaderPid) {
                    // Rebuild members for expanded group (same logic as above, but without indenting twice).
                    const uint32_t m = eg->memberCount;
                    ProcRow *tmp = (ProcRow *)calloc(m, sizeof(ProcRow));
                    uint32_t tmpCount = 0;
                    if (tmp) {
                        for (uint32_t mi = 0; mi < m; mi++) {
                            const uint32_t mpid = app->procGroupMembers[eg->memberStart + mi];
                            if (mpid == eg->leaderPid) continue;
                            const ProcRow *rawRow = find_row_by_pid(&app->procTable, mpid);
                            if (!rawRow) continue;
                            tmp[tmpCount++] = *rawRow;
                            wchar_t nn[64];
                            wcsncpy(nn, tmp[tmpCount - 1].name, (uint32_t)(sizeof(nn) / sizeof(nn[0])) - 1);
                            nn[(uint32_t)(sizeof(nn) / sizeof(nn[0])) - 1] = 0;
                            swprintf(tmp[tmpCount - 1].name,
                                     (uint32_t)(sizeof(tmp[tmpCount - 1].name) / sizeof(tmp[tmpCount - 1].name[0])),
                                     L"  %ls",
                                     nn);
                        }
                        if (tmpCount > 1) {
                            qsort(tmp, tmpCount, sizeof(tmp[0]), row_cmp_cpu_desc_local);
                        }
                        for (uint32_t k = 0; k < tmpCount; k++) {
                            app->procViewRows[out++] = tmp[k];
                        }
                        free(tmp);
                    }
                }
            }
            app->procViewCount = out;
            free(sortedGroups);
        }
    }

    // If selection doesn't exist in the new view, clear it.
    if (app->procSelectedPid != 0) {
        bool found = false;
        for (uint32_t i = 0; i < app->procViewCount; i++) {
            if (app->procViewRows[i].pid == app->procSelectedPid) {
                found = true;
                break;
            }
        }
        if (!found) {
            app->procSelectedPid = 0;
        }
    }
}

static const ProcRow *find_row_by_pid(const ProcTable *pt, uint32_t pid)
{
    if (!pt || !pt->rows || pid == 0) return NULL;
    for (uint32_t i = 0; i < pt->rowCount; i++) {
        if (pt->rows[i].pid == pid) return &pt->rows[i];
    }
    return NULL;
}

static const wchar_t *proc_copy_header_line(void)
{
    // Match the on-screen column headers from Render_DrawProcessTable.
    return L"PID\tCPU%\tMem(MB)\tOwner\tNet(remote)\tName\tPath\r\n";
}

static bool proc_append_row_tab_line(TextBufW *tb, const ProcRow *pr)
{
    if (!tb || !pr) return false;
    const double memMB = (double)pr->workingSetBytes / (1024.0 * 1024.0);
    const wchar_t *owner = pr->owner[0] ? pr->owner : L"";
    const wchar_t *net = pr->hasNet ? pr->netRemote : L"";
    const wchar_t *name = pr->name[0] ? pr->name : L"";
    const wchar_t *path = pr->path[0] ? pr->path : L"";

    return textbuf_appendf_w(tb,
                             L"%u\t%.1f\t%.1f\t%ls\t%ls\t%ls\t%ls\r\n",
                             (unsigned)pr->pid,
                             pr->cpuPct,
                             memMB,
                             owner,
                             net,
                             name,
                             path);
}

static bool copy_process_rows(App *app, uint32_t startRow, uint32_t count)
{
    if (!app || !app->hwnd) return false;
    uint32_t rowCount = 0;
    const ProcRow *rows = app_proc_view_rows(app, &rowCount);
    if (!rows || rowCount == 0) return false;
    if (count == 0) return false;
    if (startRow >= rowCount) return false;

    const uint32_t maxCount = rowCount - startRow;
    if (count > maxCount) count = maxCount;

    TextBufW tb = {0};
    bool ok = textbuf_append_w(&tb, proc_copy_header_line());
    for (uint32_t i = 0; ok && i < count; i++) {
        ok = proc_append_row_tab_line(&tb, &rows[startRow + i]);
    }

    if (ok) {
        ok = clipboard_set_text(app->hwnd, tb.p);
    }
    textbuf_free_w(&tb);
    return ok;
}

static bool copy_all_process_rows(App *app)
{
    if (!app) return false;
    uint32_t rowCount = 0;
    (void)app_proc_view_rows(app, &rowCount);
    return copy_process_rows(app, 0, rowCount);
}

static bool copy_all_visible_process_rows(App *app)
{
    if (!app) return false;
    const uint32_t start = app->procScrollRow;
    const uint32_t vis = app->render.procVisibleRows;
    return copy_process_rows(app, start, vis);
}

static bool copy_selected_process_pid(App *app)
{
    if (!app || !app->hwnd) return false;
    if (app->procSelectedPid == 0) return false;
    wchar_t buf[64];
    swprintf(buf, (uint32_t)(sizeof(buf) / sizeof(buf[0])), L"%u\r\n", (unsigned)app->procSelectedPid);
    return clipboard_set_text(app->hwnd, buf);
}

static bool copy_selected_process_path(App *app)
{
    if (!app || !app->hwnd) return false;
    uint32_t rowCount = 0;
    const ProcRow *rows = app_proc_view_rows(app, &rowCount);
    if (!rows || rowCount == 0) return false;
    if (app->procSelectedPid == 0) return false;

    // In stacked mode, Path is only shown if common across group.
    ProcTable tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.rows = (ProcRow *)rows;
    tmp.rowCount = rowCount;
    const ProcRow *pr = find_row_by_pid(&tmp, app->procSelectedPid);
    if (!pr) return false;
    if (pr->path[0] == 0) return false;

    wchar_t buf[MAX_PATH + 8];
    swprintf(buf, (uint32_t)(sizeof(buf) / sizeof(buf[0])), L"%ls\r\n", pr->path);
    return clipboard_set_text(app->hwnd, buf);
}

static bool copy_selected_process_row(App *app)
{
    if (!app || !app->hwnd) return false;
    uint32_t rowCount = 0;
    const ProcRow *rows = app_proc_view_rows(app, &rowCount);
    if (!rows || rowCount == 0) return false;
    if (app->procSelectedPid == 0) return false;

    ProcTable tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.rows = (ProcRow *)rows;
    tmp.rowCount = rowCount;
    const ProcRow *pr = find_row_by_pid(&tmp, app->procSelectedPid);
    if (!pr) return false;

    TextBufW tb = {0};
    bool ok = textbuf_append_w(&tb, proc_copy_header_line());
    ok = ok && proc_append_row_tab_line(&tb, pr);
    if (ok) {
        ok = clipboard_set_text(app->hwnd, tb.p);
    }
    textbuf_free_w(&tb);
    return ok;
}

static bool open_selected_process_location(App *app)
{
    if (!app || !app->hwnd) return false;
    uint32_t rowCount = 0;
    const ProcRow *rows = app_proc_view_rows(app, &rowCount);
    if (!rows || rowCount == 0) return false;
    if (app->procSelectedPid == 0) return false;

    ProcTable tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.rows = (ProcRow *)rows;
    tmp.rowCount = rowCount;
    const ProcRow *pr = find_row_by_pid(&tmp, app->procSelectedPid);
    if (!pr) return false;

    if (pr->path[0] == 0) {
        MessageBoxW(app->hwnd,
                    L"No executable path available for this process (access denied or protected process).",
                    L"Open File Location",
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // Open Explorer with the file selected.
    wchar_t args[MAX_PATH + 64];
    swprintf(args, (uint32_t)(sizeof(args) / sizeof(args[0])), L"/select,\"%ls\"", pr->path);

    HINSTANCE res = ShellExecuteW(app->hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)res <= 32) {
        MessageBoxW(app->hwnd,
                    L"Failed to open file location.",
                    L"Open File Location",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t proc_max_scroll_rows(const App *app)
{
    if (!app) return 0;
    uint32_t rowCount = 0;
    (void)app_proc_view_rows(app, &rowCount);
    const uint32_t vis = app->render.procVisibleRows;
    if (rowCount == 0 || vis == 0 || rowCount <= vis) return 0;
    return rowCount - vis;
}

static bool proc_hit_test_header(const App *app, int x, int y, ProcSortKey *outKey)
{
    if (!app || !outKey) return false;
    const RenderD2D *r = &app->render;

    const float fx = (float)x;
    const float fy = (float)y;
    if (fy < r->procHeaderY || fy > (r->procHeaderY + r->procRowH)) return false;
    if (fx < r->procTableX || fx > (r->procTableX + r->procTableW)) return false;

    // Columns: PID, CPU, Mem, Owner, Net, Name, Path
    if (fx < r->procColX[1]) {
        *outKey = PROC_SORT_PID;
        return true;
    }
    if (fx < r->procColX[2]) {
        *outKey = PROC_SORT_CPU;
        return true;
    }
    if (fx < r->procColX[3]) {
        *outKey = PROC_SORT_MEM;
        return true;
    }
    if (fx < r->procColX[4]) {
        *outKey = PROC_SORT_OWNER;
        return true;
    }
    if (fx < r->procColX[5]) {
        *outKey = PROC_SORT_NET;
        return true;
    }
    if (fx < r->procColX[6]) {
        *outKey = PROC_SORT_NAME;
        return true;
    }
    if (fx < r->procColX[7]) {
        *outKey = PROC_SORT_PATH;
        return true;
    }

    return false;
}

static bool proc_hit_test_row_pid(const App *app, int x, int y, uint32_t *outPid)
{
    if (!app || !outPid) return false;
    const RenderD2D *r = &app->render;
    const float fx = (float)x;
    const float fy = (float)y;

    // Must be inside table rows area (below header).
    const float rowsTop = r->procHeaderY + r->procRowH;
    const float rowsBottom = r->procTableY + r->procTableH;
    if (fy < rowsTop || fy > rowsBottom) return false;
    if (fx < r->procTableX || fx > (r->procTableX + r->procTableW)) return false;
    if (r->procRowH <= 0.0f) return false;

    const int rel = (int)((fy - rowsTop) / r->procRowH);
    if (rel < 0) return false;

    uint32_t rowCount = 0;
    const ProcRow *rows = app_proc_view_rows(app, &rowCount);
    if (!rows || rowCount == 0) return false;

    const uint32_t idx = app->procScrollRow + (uint32_t)rel;
    if (idx >= rowCount) return false;

    *outPid = rows[idx].pid;
    return true;
}

static bool proc_hit_test_help_link(const App *app, int x, int y)
{
    if (!app) return false;
    const RenderD2D *r = &app->render;
    if (r->procHelpW <= 0.0f || r->procHelpH <= 0.0f) return false;

    const float fx = (float)x;
    const float fy = (float)y;
    if (fy < r->procHelpY || fy > (r->procHelpY + r->procHelpH)) return false;
    if (fx < r->procHelpX || fx > (r->procHelpX + r->procHelpW)) return false;
    return true;
}

static bool tab_hit_test(const App *app, int x, int y, AppTab *outTab)
{
    if (!app || !outTab) return false;
    const RenderD2D *r = &app->render;

    const float fx = (float)x;
    const float fy = (float)y;

    if (r->tabCpuW > 0.0f && r->tabCpuH > 0.0f) {
        if (fx >= r->tabCpuX && fx <= (r->tabCpuX + r->tabCpuW) &&
            fy >= r->tabCpuY && fy <= (r->tabCpuY + r->tabCpuH)) {
            *outTab = APP_TAB_CPU;
            return true;
        }
    }

    if (r->tabMemW > 0.0f && r->tabMemH > 0.0f) {
        if (fx >= r->tabMemX && fx <= (r->tabMemX + r->tabMemW) &&
            fy >= r->tabMemY && fy <= (r->tabMemY + r->tabMemH)) {
            *outTab = APP_TAB_MEMORY;
            return true;
        }
    }

    if (r->tabGpuW > 0.0f && r->tabGpuH > 0.0f) {
        if (fx >= r->tabGpuX && fx <= (r->tabGpuX + r->tabGpuW) &&
            fy >= r->tabGpuY && fy <= (r->tabGpuY + r->tabGpuH)) {
            *outTab = APP_TAB_GPU;
            return true;
        }
    }

    return false;
}

typedef struct EndTaskCtx {
    uint32_t pid;
    uint32_t count;
} EndTaskCtx;

static BOOL CALLBACK enum_windows_endtask(HWND hwnd, LPARAM lp)
{
    EndTaskCtx *ctx = (EndTaskCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if ((uint32_t)pid != ctx->pid) return TRUE;

    // Only top-level windows (avoid child controls)
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ctx->count++;
    return TRUE;
}

static bool try_end_task(uint32_t pid)
{
    EndTaskCtx ctx;
    ctx.pid = pid;
    ctx.count = 0;
    EnumWindows(enum_windows_endtask, (LPARAM)&ctx);
    return ctx.count > 0;
}

static bool try_kill_process(uint32_t pid)
{
    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!hp) return false;
    const BOOL ok = TerminateProcess(hp, 1);
    CloseHandle(hp);
    return ok ? true : false;
}

static double qpc_seconds(int64_t delta, double freq)
{
    return (double)delta / freq;
}

static int64_t qpc_now(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (int64_t)li.QuadPart;
}

static double qpc_freq(void)
{
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return (double)li.QuadPart;
}

static void App_Sample(App *app)
{
    const int64_t now = qpc_now();
    const double dt = qpc_seconds(now - app->lastSampleQpc, app->qpcFreq);
    if (dt < app->sampleIntervalSec) {
        return;
    }
    app->lastSampleQpc = now;

    PdhSample sample = {0};
    if (Pdh_TrySample(&app->pdh, &sample)) {
        app->totalUsage = sample.totalCpu;
        if (app->totalUsage < app->totalUsageMin) app->totalUsageMin = app->totalUsage;
        if (app->totalUsage > app->totalUsageMax) app->totalUsageMax = app->totalUsage;
        for (uint32_t i = 0; i < app->logicalCount; i++) {
            app->coreUsage[i] = sample.coreCpu[i];
            app->coreMHz[i] = sample.coreMHz ? sample.coreMHz[i] : 0.0f;
        }
    }

    // Prefer powrprof per-core frequency when available.
    PowerCpuSample ps;
    ps.currentMHz = app->coreMHz;
    ps.maxMHz = app->coreMaxMHz;
    PowerCpu_TrySample(app->logicalCount, &ps);

    // Frequency change events (best-effort): count core MHz changes between samples.
    uint32_t changes = 0;
    for (uint32_t i = 0; i < app->logicalCount; i++) {
        const float prev = app->prevCoreMHz[i];
        const float cur = app->coreMHz[i];
        if (prev > 0.0f && cur > 0.0f && prev != cur) {
            changes++;
        }
        app->prevCoreMHz[i] = cur;
    }
    app->freqChangeCount = changes;
    app->freqChangesPerSec = (dt > 0.0) ? ((double)changes / dt) : 0.0;

    // Throttling estimate: cores running significantly below their max.
    uint32_t thr = 0;
    uint32_t denom = 0;
    for (uint32_t i = 0; i < app->logicalCount; i++) {
        const float maxM = app->coreMaxMHz[i];
        const float curM = app->coreMHz[i];
        if (maxM > 0.0f && curM > 0.0f) {
            denom++;
            const float ratio = curM / maxM;
            if (ratio < 0.95f) {
                thr++;
            }
        }
    }
    app->throttlePct = (denom > 0) ? (100.0f * (float)thr / (float)denom) : 0.0f;

    // ETW-derived scheduler/ISR/DPC rates
    EtwKernel_ComputeRates(&app->etw, dt, &app->etwRates);

    RingBuf_Push(&app->totalUsageHistory, app->totalUsage);
    for (uint32_t i = 0; i < app->logicalCount; i++) {
        RingBuf_Push(&app->coreUsageHistory[i], app->coreUsage[i]);
    }

    // Optional sensors via external provider.
    // If no provider is running, try to auto-start a bundled provider executable.
    ExternalSensors_TrySample(&app->extSensors);
    if (!app->providerAutostartAttempted) {
        app->providerAutostartAttempted = true;
        if (wcsstr(app->extSensors.status, L"provider not running") != NULL) {
            (void)try_start_bundled_provider(app);
        }
    }

    float tempC = 0.0f;
    if (app->extSensors.hasCpuTempC) {
        app->cpuTempC = app->extSensors.cpuTempC;
    } else if (WmiSensors_TryReadCpuTempC(&app->wmi, &tempC)) {
        app->cpuTempC = tempC;
    } else {
        app->cpuTempC = -1.0f;
    }

    float rpm = 0.0f;
    if (app->extSensors.hasFanRpm) {
        app->fanRpm = app->extSensors.fanRpm;
    } else if (WmiSensors_TryReadFanRpm(&app->wmi, &rpm)) {
        app->fanRpm = rpm;
    } else {
        app->fanRpm = -1.0f;
    }

    // Process table (best-effort)
    ProcTable_Sample(&app->procTable);

    // Keep display sorted according to UI state.
    if (!app->procStacked) {
        ProcTable_Sort(&app->procTable, app->procSortKey, app->procSortAsc);
    }

    // Build stacked/grouped view if enabled (and sort the view rows).
    App_RebuildProcView(app);

    // Clamp scroll after any change in row count.
    const uint32_t maxScroll = proc_max_scroll_rows(app);
    app->procScrollRow = clamp_u32(app->procScrollRow, 0, maxScroll);

    // Memory + storage (best-effort)
    MEMORYSTATUSEX ms;
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        app->memTotalPhysBytes = (uint64_t)ms.ullTotalPhys;
        app->memAvailPhysBytes = (uint64_t)ms.ullAvailPhys;
        const uint64_t used = (app->memTotalPhysBytes > app->memAvailPhysBytes) ? (app->memTotalPhysBytes - app->memAvailPhysBytes) : 0;
        app->memUsedPct = (app->memTotalPhysBytes > 0) ? (100.0f * (float)((double)used / (double)app->memTotalPhysBytes)) : 0.0f;
        RingBuf_Push(&app->memUsedPctHistory, app->memUsedPct);
    }

    PERFORMANCE_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        const uint64_t pageSize = (uint64_t)pi.PageSize;
        app->commitTotalBytes = (uint64_t)pi.CommitTotal * pageSize;
        app->commitLimitBytes = (uint64_t)pi.CommitLimit * pageSize;
        app->commitUsedPct = (app->commitLimitBytes > 0) ? (100.0f * (float)((double)app->commitTotalBytes / (double)app->commitLimitBytes)) : 0.0f;
        RingBuf_Push(&app->commitUsedPctHistory, app->commitUsedPct);
    }

    // Disk throughput
    if (app->pdh.hasPerDisk && app->pdh.diskCount > 0 && app->disks && app->diskCount == app->pdh.diskCount) {
        for (uint32_t i = 0; i < app->diskCount; i++) {
            const double rMBps = app->pdh.lastDiskReadBytesPerSec[i] / (1024.0 * 1024.0);
            const double wMBps = app->pdh.lastDiskWriteBytesPerSec[i] / (1024.0 * 1024.0);
            app->disks[i].readMBps = rMBps;
            app->disks[i].writeMBps = wMBps;
            if (app->renderDisks) {
                app->renderDisks[i].readMBps = rMBps;
                app->renderDisks[i].writeMBps = wMBps;
            }
            RingBuf_Push(&app->disks[i].readMBpsHistory, (float)rMBps);
            RingBuf_Push(&app->disks[i].writeMBpsHistory, (float)wMBps);
        }

        // Also keep a legacy _Total fallback series updated (useful on layouts)
        app->diskReadMBps = app->pdh.lastRates.hasDisk ? (app->pdh.lastRates.diskReadBytesPerSec / (1024.0 * 1024.0)) : 0.0;
        app->diskWriteMBps = app->pdh.lastRates.hasDisk ? (app->pdh.lastRates.diskWriteBytesPerSec / (1024.0 * 1024.0)) : 0.0;
        RingBuf_Push(&app->diskReadMBpsHistory, (float)app->diskReadMBps);
        RingBuf_Push(&app->diskWriteMBpsHistory, (float)app->diskWriteMBps);
    } else if (app->pdh.lastRates.hasDisk) {
        app->diskReadMBps = app->pdh.lastRates.diskReadBytesPerSec / (1024.0 * 1024.0);
        app->diskWriteMBps = app->pdh.lastRates.diskWriteBytesPerSec / (1024.0 * 1024.0);
        RingBuf_Push(&app->diskReadMBpsHistory, (float)app->diskReadMBps);
        RingBuf_Push(&app->diskWriteMBpsHistory, (float)app->diskWriteMBps);
    } else {
        app->diskReadMBps = 0.0;
        app->diskWriteMBps = 0.0;
        RingBuf_Push(&app->diskReadMBpsHistory, 0.0f);
        RingBuf_Push(&app->diskWriteMBpsHistory, 0.0f);
    }

    // GPU (best-effort)
    if (app->gpu) {
        GpuPerfSample gs;
        if (GpuPerf_TrySample(app->gpu, &gs)) {
            app->gpuLast = gs;

            if (gs.hasMemoryCounters) {
                app->gpuDedicatedUsedMB = (double)gs.dedicatedUsageBytes / (1024.0 * 1024.0);
                app->gpuDedicatedLimitMB = (double)gs.dedicatedLimitBytes / (1024.0 * 1024.0);
                app->gpuSharedUsedMB = (double)gs.sharedUsageBytes / (1024.0 * 1024.0);
                app->gpuSharedLimitMB = (double)gs.sharedLimitBytes / (1024.0 * 1024.0);
            } else {
                app->gpuDedicatedUsedMB = 0.0;
                app->gpuDedicatedLimitMB = 0.0;
                app->gpuSharedUsedMB = 0.0;
                app->gpuSharedLimitMB = 0.0;
            }

            RingBuf_Push(&app->gpuDedicatedUsedMBHistory, (float)app->gpuDedicatedUsedMB);
            RingBuf_Push(&app->gpuSharedUsedMBHistory, (float)app->gpuSharedUsedMB);

            if (gs.hasEngineCounters && app->gpuEnginePctHistory && app->gpuEngineTypeCount == gs.engineTypeCount) {
                for (uint32_t i = 0; i < app->gpuEngineTypeCount; i++) {
                    RingBuf_Push(&app->gpuEnginePctHistory[i], gs.engineTypes[i].utilizationPct);
                }
            }
        } else {
            RingBuf_Push(&app->gpuDedicatedUsedMBHistory, 0.0f);
            RingBuf_Push(&app->gpuSharedUsedMBHistory, 0.0f);
            if (app->gpuEnginePctHistory) {
                for (uint32_t i = 0; i < app->gpuEngineTypeCount; i++) {
                    RingBuf_Push(&app->gpuEnginePctHistory[i], 0.0f);
                }
            }
        }
    }
}

static void App_Render(App *app)
{
    Render_Begin(&app->render);

    Render_Clear(&app->render);

    int activeTab = 0;
    if (app->tab == APP_TAB_MEMORY) activeTab = 1;
    else if (app->tab == APP_TAB_GPU) activeTab = 2;
    Render_DrawTabs(&app->render, activeTab);

    wchar_t etwStatus[196];
    EtwKernel_GetStatusText(&app->etw, etwStatus, (uint32_t)(sizeof(etwStatus) / sizeof(etwStatus[0])));

    const uint64_t uptimeMs = (uint64_t)GetTickCount64();

    PdhRates rates = app->pdh.lastRates;
    if (app->extSensors.hasPowerW) {
        rates.hasPowerWatts = true;
        rates.powerWatts = app->extSensors.powerW;
    }

    wchar_t sensShort[96];
    sensShort[0] = 0;
    const bool anyProv = app->extSensors.hasCpuTempC || app->extSensors.hasPowerW || app->extSensors.hasFanRpm;
    if (anyProv) {
        wchar_t flags[8];
        uint32_t n = 0;
        if (app->extSensors.hasPowerW) flags[n++] = L'P';
        if (app->extSensors.hasCpuTempC) flags[n++] = L'T';
        if (app->extSensors.hasFanRpm) flags[n++] = L'F';
        flags[n] = 0;
        swprintf(sensShort, (uint32_t)(sizeof(sensShort) / sizeof(sensShort[0])), L"Sens: provider(%ls)", flags);
    } else {
        // Show a short, human-readable status so it's obvious whether a provider is running.
        wcsncpy(sensShort, app->extSensors.status, (uint32_t)(sizeof(sensShort) / sizeof(sensShort[0])) - 1);
        sensShort[(uint32_t)(sizeof(sensShort) / sizeof(sensShort[0])) - 1] = 0;
    }

    if (app->tab == APP_TAB_CPU) {
        Render_DrawHeader(&app->render, &app->cpuStatic,
                          app->totalUsage, app->totalUsageMin, app->totalUsageMax,
                          app->cpuTempC,
                          &rates, app->freqChangesPerSec, &app->etwRates,
                          etwStatus,
                          sensShort,
                          uptimeMs,
                          app->throttlePct, app->fanRpm);

        Render_DrawUsageGraph(&app->render, &app->totalUsageHistory);

        if (app->showCpu0to15) {
            uint32_t count = app->logicalCount;
            if (count > 16) count = 16;
            Render_DrawPerCore(&app->render, count, app->coreUsage, app->coreMHz,
                               app->coreUsageHistory);
        }
    } else if (app->tab == APP_TAB_MEMORY) {
        Render_DrawMemoryHeader(&app->render,
                                app->memTotalPhysBytes,
                                app->memAvailPhysBytes,
                                app->commitTotalBytes,
                                app->commitLimitBytes,
                                app->diskReadMBps,
                                app->diskWriteMBps);

        Render_DrawPercentGraph(&app->render, &app->memUsedPctHistory, L"RAM used (history)");
        Render_DrawPercentGraph(&app->render, &app->commitUsedPctHistory, L"Commit used (history)");
        if (app->renderDisks && app->diskCount > 0) {
            Render_DrawDisksGraph(&app->render, app->renderDisks, app->diskCount);
        } else {
            Render_DrawDiskGraph(&app->render, &app->diskReadMBpsHistory, &app->diskWriteMBpsHistory);
        }
    } else {
        // GPU tab (best-effort)
        const wchar_t *nm = (app->gpuLast.hasAdapter) ? app->gpuLast.adapterName : L"";
        Render_DrawGpuHeader(&app->render,
                             nm,
                             app->gpuLast.vendorId,
                             app->gpuLast.dedicatedVideoMemoryBytes,
                             app->gpuLast.sharedSystemMemoryBytes,
                             app->gpuDedicatedUsedMB,
                             app->gpuDedicatedLimitMB,
                             app->gpuSharedUsedMB,
                             app->gpuSharedLimitMB);

        Render_DrawValueGraph(&app->render, &app->gpuDedicatedUsedMBHistory,
                              L"Dedicated GPU memory used (history)",
                              (float)app->gpuDedicatedLimitMB);
        Render_DrawValueGraph(&app->render, &app->gpuSharedUsedMBHistory,
                              L"Shared GPU memory used (history)",
                              (float)app->gpuSharedLimitMB);

        // Per-engine utilization graphs. Stop before we starve the process table.
        const float reserveForProc = 240.0f * app->render.dpiScale;
        if (app->gpuLast.hasEngineCounters && app->gpuEnginePctHistory && app->gpuEngineTypeCount == app->gpuLast.engineTypeCount) {
            for (uint32_t i = 0; i < app->gpuEngineTypeCount; i++) {
                if (app->render.graphBottomY > 0.0f && (app->render.graphBottomY + reserveForProc) > (float)app->render.height) {
                    break;
                }

                const wchar_t *ename = app->gpuLast.engineTypes[i].name ? app->gpuLast.engineTypes[i].name : L"Engine";
                wchar_t title[128];
                swprintf(title, (uint32_t)_countof(title), L"GPU %ls (history)", ename);
                Render_DrawPercentGraph(&app->render, &app->gpuEnginePctHistory[i], title);
            }
        }
    }

    uint32_t viewCount = 0;
    const ProcRow *viewRows = app_proc_view_rows(app, &viewCount);
    Render_DrawProcessTable(&app->render,
                            viewRows,
                            viewCount,
                            app->procTable.rowCount,
                            app->procStacked,
                            app->procScrollRow,
                            app->procSelectedPid);

    Render_End(&app->render);
}

// Window procedure for main application window.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    App *app = (App *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_SIZE:
        if (app) {
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            Render_Resize(&app->render, w, h);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (app) {
            App_Render(app);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (!app) {
            return 0;
        }
        if (id == IDM_VIEW_CPU0_15) {
            app->showCpu0to15 = !app->showCpu0to15;
            HMENU menu = GetMenu(hwnd);
            if (menu) {
                CheckMenuItem(menu, IDM_VIEW_CPU0_15, MF_BYCOMMAND | (app->showCpu0to15 ? MF_CHECKED : MF_UNCHECKED));
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (id == IDM_VIEW_STACK_PROCS) {
            app->procStacked = !app->procStacked;
            HMENU menu = GetMenu(hwnd);
            if (menu) {
                CheckMenuItem(menu, IDM_VIEW_STACK_PROCS, MF_BYCOMMAND | (app->procStacked ? MF_CHECKED : MF_UNCHECKED));
            }
            App_RebuildProcView(app);
            app->procScrollRow = clamp_u32(app->procScrollRow, 0, proc_max_scroll_rows(app));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (id == IDM_HELP_METRICS) {
            HelpWindow_Show(hwnd);
            return 0;
        }
        if (id == IDM_HELP_MEMORY_DISKS) {
            HelpWindow_ShowTopic(hwnd, L"memory_and_disks.html");
            return 0;
        }
        if (id == IDM_HELP_GPU) {
            HelpWindow_ShowTopic(hwnd, L"gpu_and_motherboard.html");
            return 0;
        }
        if (id == IDM_HELP_ABOUT) {
            MessageBoxW(hwnd,
                        L"CCM - Const CPU Monitor\n\n"
                        L"Win32 C + Direct2D/DirectWrite\n"
                        L"Telemetry: PDH, ETW (kernel), PowrProf, WMI, Toolhelp, PSAPI, IP Helper\n",
                        L"About", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (id == IDM_PROC_END_TASK) {
            const uint32_t pid = app->procSelectedPid;
            if (pid == 0) return 0;
            if (app->procStacked) {
                const struct ProcGroupIndex *g = find_group_by_leader_pid(app, pid);
                if (g && g->memberCount > 1) {
                    uint32_t closed = 0;
                    for (uint32_t i = 0; i < g->memberCount; i++) {
                        const uint32_t mpid = app->procGroupMembers[g->memberStart + i];
                        if (try_end_task(mpid)) closed++;
                    }
                    if (closed == 0) {
                        MessageBoxW(hwnd, L"No top-level windows to close for this group (End Task is best-effort).", L"End Task", MB_OK | MB_ICONINFORMATION);
                    }
                    return 0;
                }
            }
            if (!try_end_task(pid)) {
                MessageBoxW(hwnd, L"No top-level window to close (End Task is best-effort).", L"End Task", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        if (id == IDM_PROC_KILL) {
            const uint32_t pid = app->procSelectedPid;
            if (pid == 0) return 0;

            uint32_t viewCount = 0;
            const ProcRow *viewRows = app_proc_view_rows(app, &viewCount);
            ProcTable tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.rows = (ProcRow *)viewRows;
            tmp.rowCount = viewCount;
            const ProcRow *pr = find_row_by_pid(&tmp, pid);
            const wchar_t *name = (pr && pr->name[0]) ? pr->name : L"(unknown)";

            if (app->procStacked) {
                const struct ProcGroupIndex *g = find_group_by_leader_pid(app, pid);
                if (g && g->memberCount > 1) {
                    wchar_t msg[512];
                    swprintf(msg, (uint32_t)(sizeof(msg) / sizeof(msg[0])),
                             L"Kill %u processes for \"%ls\"?\n\nThis uses TerminateProcess and may cause data loss.",
                             (unsigned)g->memberCount,
                             name);
                    const int res = MessageBoxW(hwnd, msg, L"Kill Process", MB_OKCANCEL | MB_ICONWARNING);
                    if (res == IDOK) {
                        uint32_t failed = 0;
                        for (uint32_t i = 0; i < g->memberCount; i++) {
                            const uint32_t mpid = app->procGroupMembers[g->memberStart + i];
                            if (!try_kill_process(mpid)) failed++;
                        }
                        if (failed > 0) {
                            wchar_t emsg[256];
                            swprintf(emsg, (uint32_t)(sizeof(emsg) / sizeof(emsg[0])),
                                     L"Failed to terminate %u of %u processes (access denied?).",
                                     (unsigned)failed,
                                     (unsigned)g->memberCount);
                            MessageBoxW(hwnd, emsg, L"Kill Process", MB_OK | MB_ICONERROR);
                        }
                    }
                    return 0;
                }
            }

            wchar_t msg[512];
            swprintf(msg, (uint32_t)(sizeof(msg) / sizeof(msg[0])),
                     L"Kill process \"%ls\" (PID %u)?\n\nThis uses TerminateProcess and may cause data loss.",
                     name,
                     (unsigned)pid);
            const int res = MessageBoxW(hwnd, msg, L"Kill Process", MB_OKCANCEL | MB_ICONWARNING);
            if (res == IDOK) {
                if (!try_kill_process(pid)) {
                    MessageBoxW(hwnd, L"Failed to terminate process (access denied?).", L"Kill Process", MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        if (id == IDM_PROC_COPY) {
            (void)copy_selected_process_row(app);
            return 0;
        }
        if (id == IDM_PROC_COPY_ALL_VISIBLE) {
            (void)copy_all_visible_process_rows(app);
            return 0;
        }
        if (id == IDM_PROC_COPY_ALL) {
            (void)copy_all_process_rows(app);
            return 0;
        }
        if (id == IDM_PROC_COPY_PID) {
            (void)copy_selected_process_pid(app);
            return 0;
        }
        if (id == IDM_PROC_COPY_PATH) {
            (void)copy_selected_process_path(app);
            return 0;
        }
        if (id == IDM_PROC_OPEN_LOCATION) {
            (void)open_selected_process_location(app);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (app) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && (wParam == 'C' || wParam == 'c')) {
                (void)copy_selected_process_row(app);
                return 0;
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (app) {
            const int z = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            const int step = 3;
            int scroll = (int)app->procScrollRow;
            scroll -= z * step;
            const uint32_t maxScroll = proc_max_scroll_rows(app);
            scroll = clamp_int(scroll, 0, (int)maxScroll);
            app->procScrollRow = (uint32_t)scroll;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (app) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);

            AppTab tab;
            if (tab_hit_test(app, x, y, &tab)) {
                if (app->tab != tab) {
                    app->tab = tab;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }

            if (proc_hit_test_help_link(app, x, y)) {
                HelpWindow_ShowTopic(hwnd, L"processes.html");
                return 0;
            }

            ProcSortKey key;
            if (proc_hit_test_header(app, x, y, &key)) {
                if (app->procSortKey == key) {
                    app->procSortAsc = !app->procSortAsc;
                } else {
                    app->procSortKey = key;
                    switch (key) {
                    case PROC_SORT_CPU:
                    case PROC_SORT_MEM:
                        app->procSortAsc = false;
                        break;
                    default:
                        app->procSortAsc = true;
                        break;
                    }
                }
                ProcTable_Sort(&app->procTable, app->procSortKey, app->procSortAsc);
                app->procScrollRow = clamp_u32(app->procScrollRow, 0, proc_max_scroll_rows(app));
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            uint32_t pid = 0;
            if (proc_hit_test_row_pid(app, x, y, &pid)) {
                app->procSelectedPid = pid;

                // In stacked mode, clicking a multi-process group header toggles expansion.
                if (app->procStacked) {
                    const struct ProcGroupIndex *g = find_group_by_leader_pid(app, pid);
                    if (g && g->memberCount > 1) {
                        if (app->procHasExpanded &&
                            wcmp_insensitive_local(app->procExpandedBaseName, g->baseName) == 0) {
                            app->procHasExpanded = false;
                            app->procExpandedBaseName[0] = 0;
                        } else {
                            app->procHasExpanded = true;
                            wcsncpy(app->procExpandedBaseName, g->baseName,
                                    (uint32_t)(sizeof(app->procExpandedBaseName) / sizeof(app->procExpandedBaseName[0])) - 1);
                            app->procExpandedBaseName[(uint32_t)(sizeof(app->procExpandedBaseName) / sizeof(app->procExpandedBaseName[0])) - 1] = 0;
                        }
                        App_RebuildProcView(app);
                        app->procScrollRow = clamp_u32(app->procScrollRow, 0, proc_max_scroll_rows(app));
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        if (app) {
            // lParam is screen coords.
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            POINT client = pt;
            ScreenToClient(hwnd, &client);

            uint32_t pid = 0;
            if (proc_hit_test_row_pid(app, client.x, client.y, &pid)) {
                app->procSelectedPid = pid;
                InvalidateRect(hwnd, NULL, FALSE);

                uint32_t viewCount = 0;
                const ProcRow *viewRows = app_proc_view_rows(app, &viewCount);
                ProcTable tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.rows = (ProcRow *)viewRows;
                tmp.rowCount = viewCount;
                const ProcRow *pr = find_row_by_pid(&tmp, pid);
                const bool hasPid = (pid != 0) && (pr != NULL);
                const bool hasPath = hasPid && pr->path[0];

                const UINT en = MF_STRING | (hasPid ? MF_ENABLED : MF_GRAYED);
                const UINT enPath = MF_STRING | (hasPath ? MF_ENABLED : MF_GRAYED);

                HMENU popup = CreatePopupMenu();
                AppendMenuW(popup, en, IDM_PROC_COPY, L"Copy");
                AppendMenuW(popup, en, IDM_PROC_COPY_ALL_VISIBLE, L"Copy all visible rows");
                AppendMenuW(popup, en, IDM_PROC_COPY_ALL, L"Copy all rows");
                AppendMenuW(popup, en, IDM_PROC_COPY_PID, L"Copy PID only");
                AppendMenuW(popup, enPath, IDM_PROC_COPY_PATH, L"Copy Path only");
                AppendMenuW(popup, enPath, IDM_PROC_OPEN_LOCATION, L"Open file location");
                AppendMenuW(popup, MF_SEPARATOR, 0, NULL);
                AppendMenuW(popup, en, IDM_PROC_END_TASK, L"End Task (close window)");
                AppendMenuW(popup, en, IDM_PROC_KILL, L"Kill Process");
                TrackPopupMenu(popup, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(popup);
                return 0;
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static HMENU build_menu(bool showCpu0to15)
{
    HMENU main = CreateMenu();
    HMENU view = CreateMenu();
    HMENU help = CreateMenu();

    AppendMenuW(view, MF_STRING | (showCpu0to15 ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_CPU0_15, L"Show CPU0-CPU15");
    AppendMenuW(view, MF_STRING | MF_CHECKED, IDM_VIEW_STACK_PROCS, L"Stack multi-process apps");
    AppendMenuW(help, MF_STRING, IDM_HELP_METRICS, L"Metrics Help");
    AppendMenuW(help, MF_STRING, IDM_HELP_MEMORY_DISKS, L"Memory && Disks overview");
    AppendMenuW(help, MF_STRING, IDM_HELP_GPU, L"GPU && Motherboard overview");
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"About");

    AppendMenuW(main, MF_POPUP, (UINT_PTR)view, L"View");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)help, L"Help");
    return main;
}

bool App_Init(App *app, HINSTANCE hInstance)
{
    memset(app, 0, sizeof(*app));
    app->hInstance = hInstance;

    app->qpcFreq = qpc_freq();
    app->lastSampleQpc = qpc_now();
    app->lastRenderQpc = app->lastSampleQpc;
    app->sampleIntervalSec = 0.25;

    app->showCpu0to15 = true;
    app->tab = APP_TAB_CPU;
    app->totalUsageMin = FLT_MAX;
    app->totalUsageMax = -FLT_MAX;

    app->procSortKey = PROC_SORT_CPU;
    app->procSortAsc = false;
    app->procScrollRow = 0;
    app->procSelectedPid = 0;

    app->procStacked = true;
    app->procViewRows = NULL;
    app->procViewCount = 0;
    app->procViewCap = 0;
    app->procGroups = NULL;
    app->procGroupCount = 0;
    app->procGroupCap = 0;
    app->procGroupMembers = NULL;
    app->procGroupMemberCount = 0;
    app->procGroupMemberCap = 0;

    app->procHasExpanded = false;
    app->procExpandedBaseName[0] = 0;

    app->providerAutostartAttempted = false;
    app->providerProcess = NULL;
    app->providerPid = 0;

    ProcTable_Init(&app->procTable);

    CpuStatic_Init(&app->cpuStatic);

    app->logicalCount = app->cpuStatic.logicalProcessorCount;
    if (app->logicalCount == 0) {
        app->logicalCount = 1;
    }

    app->coreUsage = (float *)calloc(app->logicalCount, sizeof(float));
    app->coreMHz = (float *)calloc(app->logicalCount, sizeof(float));
    app->coreMaxMHz = (float *)calloc(app->logicalCount, sizeof(float));
    app->prevCoreMHz = (float *)calloc(app->logicalCount, sizeof(float));
    app->coreUsageHistory = (RingBufF *)calloc(app->logicalCount, sizeof(RingBufF));
    if (!app->coreUsage || !app->coreMHz || !app->coreMaxMHz || !app->prevCoreMHz || !app->coreUsageHistory) {
        return false;
    }

    // Keep ~60 seconds of history at 4 Hz sampling
    const uint32_t histCap = 240;
    if (!RingBuf_Init(&app->totalUsageHistory, histCap)) {
        return false;
    }

    if (!RingBuf_Init(&app->memUsedPctHistory, histCap)) {
        return false;
    }
    if (!RingBuf_Init(&app->commitUsedPctHistory, histCap)) {
        return false;
    }
    if (!RingBuf_Init(&app->diskReadMBpsHistory, histCap)) {
        return false;
    }
    if (!RingBuf_Init(&app->diskWriteMBpsHistory, histCap)) {
        return false;
    }

    if (!RingBuf_Init(&app->gpuDedicatedUsedMBHistory, histCap)) {
        return false;
    }
    if (!RingBuf_Init(&app->gpuSharedUsedMBHistory, histCap)) {
        return false;
    }

    for (uint32_t i = 0; i < app->logicalCount; i++) {
        if (!RingBuf_Init(&app->coreUsageHistory[i], histCap)) {
            return false;
        }
    }

    if (!Pdh_Init(&app->pdh, app->logicalCount)) {
        // Keep running; PDH might fail on some systems.
    }

    // GPU (best-effort). If unavailable, CCM still runs.
    app->gpu = NULL;
    app->gpuEnginePctHistory = NULL;
    app->gpuEngineTypeCount = 0;
    app->gpuDedicatedUsedMB = 0.0;
    app->gpuDedicatedLimitMB = 0.0;
    app->gpuSharedUsedMB = 0.0;
    app->gpuSharedLimitMB = 0.0;
    ZeroMemory(&app->gpuLast, sizeof(app->gpuLast));

    if (GpuPerf_Init(&app->gpu)) {
        // Prime once to discover engine type count.
        GpuPerfSample gs;
        if (GpuPerf_TrySample(app->gpu, &gs) && gs.hasEngineCounters && gs.engineTypeCount > 0) {
            app->gpuEngineTypeCount = gs.engineTypeCount;
            app->gpuEnginePctHistory = (RingBufF *)calloc(app->gpuEngineTypeCount, sizeof(RingBufF));
            if (app->gpuEnginePctHistory) {
                bool ok = true;
                for (uint32_t i = 0; i < app->gpuEngineTypeCount; i++) {
                    ok = ok && RingBuf_Init(&app->gpuEnginePctHistory[i], histCap);
                }
                if (!ok) {
                    for (uint32_t i = 0; i < app->gpuEngineTypeCount; i++) {
                        RingBuf_Shutdown(&app->gpuEnginePctHistory[i]);
                    }
                    free(app->gpuEnginePctHistory);
                    app->gpuEnginePctHistory = NULL;
                    app->gpuEngineTypeCount = 0;
                }
            } else {
                app->gpuEngineTypeCount = 0;
            }
        }
    }

    // Per-disk series (best-effort). Requires PDH init.
    if (app->pdh.hasPerDisk && app->pdh.diskCount > 0 && app->pdh.diskInstances) {
        app->diskCount = app->pdh.diskCount;
        app->disks = (DiskSeries *)calloc(app->diskCount, sizeof(*app->disks));
        app->renderDisks = (RenderDiskSeries *)calloc(app->diskCount, sizeof(*app->renderDisks));

        bool ok = (app->disks != NULL) && (app->renderDisks != NULL);
        if (ok) {
            for (uint32_t i = 0; i < app->diskCount; i++) {
                const wchar_t *nm = app->pdh.diskInstances[i] ? app->pdh.diskInstances[i] : L"Disk";
                wcsncpy(app->disks[i].name, nm, (sizeof(app->disks[i].name) / sizeof(app->disks[i].name[0])) - 1);
                app->disks[i].name[(sizeof(app->disks[i].name) / sizeof(app->disks[i].name[0])) - 1] = 0;

                ok = ok && RingBuf_Init(&app->disks[i].readMBpsHistory, histCap);
                ok = ok && RingBuf_Init(&app->disks[i].writeMBpsHistory, histCap);

                app->renderDisks[i].name = app->disks[i].name;
                app->renderDisks[i].readMBpsHistory = &app->disks[i].readMBpsHistory;
                app->renderDisks[i].writeMBpsHistory = &app->disks[i].writeMBpsHistory;
                app->renderDisks[i].readMBps = 0.0;
                app->renderDisks[i].writeMBps = 0.0;
            }
        }

        if (!ok) {
            if (app->disks) {
                for (uint32_t i = 0; i < app->diskCount; i++) {
                    RingBuf_Shutdown(&app->disks[i].readMBpsHistory);
                    RingBuf_Shutdown(&app->disks[i].writeMBpsHistory);
                }
            }
            free(app->disks);
            free(app->renderDisks);
            app->disks = NULL;
            app->renderDisks = NULL;
            app->diskCount = 0;
        }
    }

    // Best-effort ETW kernel session (scheduler/ISR/DPC)
    EtwKernel_Start(&app->etw);

    WmiSensors_Init(&app->wmi);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    app->hwnd = CreateWindowExW(
        0, kWndClass,
        L"CCM - Const CPU Monitor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1600, 900,
        NULL, NULL, hInstance, app);

    if (!app->hwnd) {
        return false;
    }

    SetMenu(app->hwnd, build_menu(app->showCpu0to15));

    if (!Render_Init(&app->render, app->hwnd)) {
        return false;
    }

    return true;
}

void App_Show(App *app, int nCmdShow)
{
    ShowWindow(app->hwnd, nCmdShow);
    UpdateWindow(app->hwnd);
}

int App_Run(App *app)
{
    MSG msg;
    memset(&msg, 0, sizeof(msg));

    // Simple fixed render loop: pump messages, sample at ~4Hz, render at ~60fps.
    const double targetFrameSec = 1.0 / 60.0;

    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        App_Sample(app);

        const int64_t now = qpc_now();
        const double dt = qpc_seconds(now - app->lastRenderQpc, app->qpcFreq);
        if (dt >= targetFrameSec) {
            app->lastRenderQpc = now;
            InvalidateRect(app->hwnd, NULL, FALSE);
        } else {
            DWORD sleepMs = (DWORD)((targetFrameSec - dt) * 1000.0);
            if (sleepMs > 1) {
                Sleep(sleepMs);
            } else {
                Sleep(0);
            }
        }
    }
}

void App_Shutdown(App *app)
{
    if (app->providerProcess) {
        CloseHandle(app->providerProcess);
        app->providerProcess = NULL;
    }

    free(app->procViewRows);
    app->procViewRows = NULL;
    app->procViewCount = 0;
    app->procViewCap = 0;
    free(app->procGroups);
    app->procGroups = NULL;
    app->procGroupCount = 0;
    app->procGroupCap = 0;
    free(app->procGroupMembers);
    app->procGroupMembers = NULL;
    app->procGroupMemberCount = 0;
    app->procGroupMemberCap = 0;

    Render_Shutdown(&app->render);

    Pdh_Shutdown(&app->pdh);
    WmiSensors_Shutdown(&app->wmi);
    EtwKernel_Stop(&app->etw);

    ProcTable_Shutdown(&app->procTable);

    CpuStatic_Shutdown(&app->cpuStatic);

    if (app->coreUsageHistory) {
        for (uint32_t i = 0; i < app->logicalCount; i++) {
            RingBuf_Shutdown(&app->coreUsageHistory[i]);
        }
    }
    RingBuf_Shutdown(&app->totalUsageHistory);
    RingBuf_Shutdown(&app->memUsedPctHistory);
    RingBuf_Shutdown(&app->commitUsedPctHistory);
    RingBuf_Shutdown(&app->diskReadMBpsHistory);
    RingBuf_Shutdown(&app->diskWriteMBpsHistory);

    RingBuf_Shutdown(&app->gpuDedicatedUsedMBHistory);
    RingBuf_Shutdown(&app->gpuSharedUsedMBHistory);
    if (app->gpuEnginePctHistory) {
        for (uint32_t i = 0; i < app->gpuEngineTypeCount; i++) {
            RingBuf_Shutdown(&app->gpuEnginePctHistory[i]);
        }
    }
    free(app->gpuEnginePctHistory);
    app->gpuEnginePctHistory = NULL;
    app->gpuEngineTypeCount = 0;

    if (app->gpu) {
        GpuPerf_Shutdown(app->gpu);
        app->gpu = NULL;
    }

    if (app->disks) {
        for (uint32_t i = 0; i < app->diskCount; i++) {
            RingBuf_Shutdown(&app->disks[i].readMBpsHistory);
            RingBuf_Shutdown(&app->disks[i].writeMBpsHistory);
        }
    }
    free(app->disks);
    free(app->renderDisks);

    free(app->coreUsageHistory);
    free(app->coreUsage);
    free(app->coreMHz);
    free(app->coreMaxMHz);
    free(app->prevCoreMHz);

    memset(app, 0, sizeof(*app));
}

#include "proc_table.h"

// Winsock must be included before windows.h to avoid older winsock.h conflicts.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <tlhelp32.h>
#include <psapi.h>
#include <iphlpapi.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

static uint64_t ft_to_u64(FILETIME ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (uint64_t)u.QuadPart;
}

static void safe_wcpy(wchar_t *dst, size_t dstCount, const wchar_t *src)
{
    if (!dst || dstCount == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
#ifdef _MSC_VER
    wcscpy_s(dst, dstCount, src);
#else
    wcsncpy(dst, src, dstCount - 1);
    dst[dstCount - 1] = 0;
#endif
}

static void get_process_owner(HANDLE hProcess, wchar_t *out, size_t outCount)
{
    if (!out || outCount == 0) return;
    out[0] = 0;

    HANDLE hToken = NULL;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
        return;
    }

    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, NULL, 0, &len);
    if (len == 0) {
        CloseHandle(hToken);
        return;
    }

    TOKEN_USER *tu = (TOKEN_USER *)malloc(len);
    if (!tu) {
        CloseHandle(hToken);
        return;
    }

    if (!GetTokenInformation(hToken, TokenUser, tu, len, &len)) {
        free(tu);
        CloseHandle(hToken);
        return;
    }

    wchar_t name[64];
    wchar_t domain[64];
    DWORD cchName = (DWORD)_countof(name);
    DWORD cchDomain = (DWORD)_countof(domain);
    SID_NAME_USE use;
    if (LookupAccountSidW(NULL, tu->User.Sid, name, &cchName, domain, &cchDomain, &use)) {
        wchar_t tmp[96];
#ifdef _MSC_VER
        swprintf(tmp, _countof(tmp), L"%s\\%s", domain, name);
#else
    swprintf(tmp, _countof(tmp), L"%ls\\%ls", domain, name);
#endif
        safe_wcpy(out, outCount, tmp);
    }

    free(tu);
    CloseHandle(hToken);
}

static void get_process_path(HANDLE hProcess, wchar_t *out, size_t outCount)
{
    if (!out || outCount == 0) return;
    out[0] = 0;

    DWORD sz = (DWORD)outCount;
    // QueryFullProcessImageNameW is in kernel32.
    if (!QueryFullProcessImageNameW(hProcess, 0, out, &sz)) {
        out[0] = 0;
    }
}

typedef struct PidNet {
    uint32_t pid;
    wchar_t remote[64];
    bool has;
} PidNet;

static bool is_internet_remote_ipv4(uint32_t addr_be)
{
    // addr_be is network byte order.
    const uint32_t addr = ntohl(addr_be);
    if (addr == 0) return false;
    // 127.0.0.0/8 loopback
    if ((addr & 0xFF000000u) == 0x7F000000u) return false;
    return true;
}

static int pidnet_find(PidNet *arr, uint32_t n, uint32_t pid)
{
    for (uint32_t i = 0; i < n; i++) {
        if (arr[i].pid == pid) return (int)i;
    }
    return -1;
}

static uint32_t build_pid_net_map(PidNet *out, uint32_t outCap)
{
    if (!out || outCap == 0) return 0;

    uint32_t count = 0;

    // TCP IPv4
    DWORD size = 0;
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size > 0) {
        void *buf = malloc(size);
        if (buf) {
            if (GetExtendedTcpTable(buf, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
                MIB_TCPTABLE_OWNER_PID *tab = (MIB_TCPTABLE_OWNER_PID *)buf;
                for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                    const MIB_TCPROW_OWNER_PID *r = &tab->table[i];
                    if (r->dwState != MIB_TCP_STATE_ESTAB) continue;
                    if (!is_internet_remote_ipv4(r->dwRemoteAddr)) continue;

                    const uint32_t pid = r->dwOwningPid;
                    if (pid == 0) continue;

                    if (pidnet_find(out, count, pid) >= 0) continue;
                    if (count >= outCap) break;

                    IN_ADDR a;
                    a.S_un.S_addr = r->dwRemoteAddr;
                    wchar_t ip[64];
                    ip[0] = 0;
                    InetNtopW(AF_INET, &a, ip, (DWORD)_countof(ip));
                    const uint16_t port = ntohs((uint16_t)r->dwRemotePort);

                    out[count].pid = pid;
                    out[count].has = true;
#ifdef _MSC_VER
                    swprintf(out[count].remote, _countof(out[count].remote), L"%s:%u", ip, (unsigned)port);
#else
                    swprintf(out[count].remote, _countof(out[count].remote), L"%ls:%u", ip, (unsigned)port);
#endif
                    count++;
                }
            }
            free(buf);
        }
    }

    return count;
}

static uint64_t get_process_total_time_100ns(HANDLE hProcess)
{
    FILETIME ct, et, kt, ut;
    if (!GetProcessTimes(hProcess, &ct, &et, &kt, &ut)) {
        return 0;
    }
    return ft_to_u64(kt) + ft_to_u64(ut);
}

static uint64_t get_system_total_time_100ns(void)
{
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return 0;
    }
    return ft_to_u64(kernel) + ft_to_u64(user);
}

static int row_cmp_cpu_desc(const void *a, const void *b)
{
    const ProcRow *ra = (const ProcRow *)a;
    const ProcRow *rb = (const ProcRow *)b;
    if (ra->cpuPct < rb->cpuPct) return 1;
    if (ra->cpuPct > rb->cpuPct) return -1;
    // tie-breaker: memory
    if (ra->workingSetBytes < rb->workingSetBytes) return 1;
    if (ra->workingSetBytes > rb->workingSetBytes) return -1;
    return 0;
}

static int wcmp_insensitive(const wchar_t *a, const wchar_t *b)
{
    if (!a) a = L"";
    if (!b) b = L"";
#ifdef _MSC_VER
    return _wcsicmp(a, b);
#else
    return wcsicmp(a, b);
#endif
}

static int row_cmp_string(const wchar_t *a, const wchar_t *b)
{
    // Empty strings sort last.
    const bool aEmpty = (!a || a[0] == 0);
    const bool bEmpty = (!b || b[0] == 0);
    if (aEmpty && bEmpty) return 0;
    if (aEmpty) return 1;
    if (bEmpty) return -1;
    return wcmp_insensitive(a, b);
}

typedef struct SortCtx {
    ProcSortKey key;
    bool asc;
} SortCtx;

static SortCtx g_sortCtx;

static int row_cmp_ctx(const void *a, const void *b)
{
    const ProcRow *ra = (const ProcRow *)a;
    const ProcRow *rb = (const ProcRow *)b;

    int c = 0;
    switch (g_sortCtx.key) {
    case PROC_SORT_CPU:
        if (ra->cpuPct < rb->cpuPct) c = -1;
        else if (ra->cpuPct > rb->cpuPct) c = 1;
        else c = 0;
        break;
    case PROC_SORT_PID:
        if (ra->pid < rb->pid) c = -1;
        else if (ra->pid > rb->pid) c = 1;
        else c = 0;
        break;
    case PROC_SORT_MEM:
        if (ra->workingSetBytes < rb->workingSetBytes) c = -1;
        else if (ra->workingSetBytes > rb->workingSetBytes) c = 1;
        else c = 0;
        break;
    case PROC_SORT_OWNER:
        c = row_cmp_string(ra->owner, rb->owner);
        break;
    case PROC_SORT_NET:
        c = row_cmp_string(ra->hasNet ? ra->netRemote : L"", rb->hasNet ? rb->netRemote : L"");
        break;
    case PROC_SORT_NAME:
        c = row_cmp_string(ra->name, rb->name);
        break;
    case PROC_SORT_PATH:
        c = row_cmp_string(ra->path, rb->path);
        break;
    default:
        c = 0;
        break;
    }

    // Tie-breakers: CPU desc then PID asc.
    if (c == 0) {
        if (ra->cpuPct < rb->cpuPct) c = 1;
        else if (ra->cpuPct > rb->cpuPct) c = -1;
        else {
            if (ra->pid < rb->pid) c = -1;
            else if (ra->pid > rb->pid) c = 1;
            else c = 0;
        }
    }

    if (!g_sortCtx.asc) {
        c = -c;
    }
    return c;
}

static void prev_mark_all_unseen(ProcTable *pt)
{
    for (uint32_t i = 0; i < pt->prevCount; i++) {
        pt->prev[i].seen = false;
    }
}

static uint32_t prev_find_or_add(ProcTable *pt, uint32_t pid)
{
    for (uint32_t i = 0; i < pt->prevCount; i++) {
        if (pt->prev[i].pid == pid) {
            pt->prev[i].seen = true;
            return i;
        }
    }

    if (pt->prevCount == pt->prevCap) {
        uint32_t newCap = pt->prevCap ? (pt->prevCap * 2) : 256;
        void *p = realloc(pt->prev, newCap * sizeof(*pt->prev));
        if (!p) {
            // can't grow; reuse slot 0
            pt->prev[0].pid = pid;
            pt->prev[0].procTotal100ns = 0;
            pt->prev[0].seen = true;
            return 0;
        }
        pt->prev = (struct PrevPidTime *)p;
        pt->prevCap = newCap;
    }

    const uint32_t idx = pt->prevCount++;
    pt->prev[idx].pid = pid;
    pt->prev[idx].procTotal100ns = 0;
    pt->prev[idx].seen = true;
    return idx;
}

static void prev_compact_unseen(ProcTable *pt)
{
    uint32_t w = 0;
    for (uint32_t r = 0; r < pt->prevCount; r++) {
        if (pt->prev[r].seen) {
            if (w != r) {
                pt->prev[w] = pt->prev[r];
            }
            w++;
        }
    }
    pt->prevCount = w;
}

void ProcTable_Init(ProcTable *pt)
{
    if (!pt) return;
    memset(pt, 0, sizeof(*pt));
}

void ProcTable_Shutdown(ProcTable *pt)
{
    if (!pt) return;
    free(pt->prev);
    free(pt->rows);
    memset(pt, 0, sizeof(*pt));
}

static bool proc_table_ensure_rows(ProcTable *pt, uint32_t want)
{
    if (!pt) return false;
    if (want <= pt->rowCap) return true;

    uint32_t newCap = pt->rowCap ? pt->rowCap : (uint32_t)PROC_TABLE_INITIAL_CAP;
    while (newCap < want) {
        // Avoid overflow; process counts are typically small.
        if (newCap > (UINT32_MAX / 2u)) {
            newCap = want;
            break;
        }
        newCap *= 2u;
    }

    void *p = realloc(pt->rows, (size_t)newCap * sizeof(*pt->rows));
    if (!p) return false;
    pt->rows = (ProcRow *)p;
    pt->rowCap = newCap;
    return true;
}

void ProcTable_Sample(ProcTable *pt)
{
    if (!pt) return;

    const uint64_t sysTotal = get_system_total_time_100ns();
    const uint64_t sysDelta = pt->prevInit ? (sysTotal - pt->prevSysTotal100ns) : 0;
    pt->prevSysTotal100ns = sysTotal;

    PidNet nets[1024];
    memset(nets, 0, sizeof(nets));
    const uint32_t netCount = build_pid_net_map(nets, (uint32_t)_countof(nets));

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        pt->rowCount = 0;
        pt->prevInit = true;
        return;
    }

    prev_mark_all_unseen(pt);

    PROCESSENTRY32W pe;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);

    pt->rowCount = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            const uint32_t pid = (uint32_t)pe.th32ProcessID;
            if (pid == 0) continue;

            if (!proc_table_ensure_rows(pt, pt->rowCount + 1)) {
                // Out of memory; keep partial list.
                break;
            }

            ProcRow r;
            memset(&r, 0, sizeof(r));
            r.pid = pid;
            safe_wcpy(r.name, _countof(r.name), pe.szExeFile);

            // Network
            const int ni = pidnet_find(nets, netCount, pid);
            if (ni >= 0) {
                r.hasNet = true;
                safe_wcpy(r.netRemote, _countof(r.netRemote), nets[ni].remote);
            } else {
                r.hasNet = false;
                r.netRemote[0] = 0;
            }

            // Open process (best-effort)
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hp) {
                get_process_path(hp, r.path, _countof(r.path));
                get_process_owner(hp, r.owner, _countof(r.owner));

                // CPU% (requires GetProcessTimes)
                const uint64_t procTotal = get_process_total_time_100ns(hp);
                const uint32_t pidx = prev_find_or_add(pt, pid);
                const uint64_t prevTotal = pt->prev[pidx].procTotal100ns;
                pt->prev[pidx].procTotal100ns = procTotal;

                if (pt->prevInit && sysDelta > 0 && procTotal >= prevTotal) {
                    const uint64_t d = procTotal - prevTotal;
                    r.cpuPct = (float)((double)d * 100.0 / (double)sysDelta);
                    if (r.cpuPct < 0.0f) r.cpuPct = 0.0f;
                    if (r.cpuPct > 100.0f) r.cpuPct = 100.0f;
                } else {
                    r.cpuPct = 0.0f;
                }

                // Memory (needs PROCESS_QUERY_INFORMATION on some systems; try broader if needed)
                PROCESS_MEMORY_COUNTERS_EX pmc;
                memset(&pmc, 0, sizeof(pmc));
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
                    r.workingSetBytes = (uint64_t)pmc.WorkingSetSize;
                }

                CloseHandle(hp);
            } else {
                // Still mark in prev map to allow later samples if we get permission.
                (void)prev_find_or_add(pt, pid);
            }

            pt->rows[pt->rowCount++] = r;
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    prev_compact_unseen(pt);
    pt->prevInit = true;
}

void ProcTable_Sort(ProcTable *pt, ProcSortKey key, bool ascending)
{
    if (!pt || pt->rowCount == 0) return;
    g_sortCtx.key = key;
    g_sortCtx.asc = ascending;
    qsort(pt->rows, pt->rowCount, sizeof(pt->rows[0]), row_cmp_ctx);
}

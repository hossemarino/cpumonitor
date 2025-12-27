#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef PROC_TABLE_INITIAL_CAP
#define PROC_TABLE_INITIAL_CAP 256
#endif

typedef enum ProcSortKey {
    PROC_SORT_CPU = 0,
    PROC_SORT_PID,
    PROC_SORT_MEM,
    PROC_SORT_OWNER,
    PROC_SORT_NET,
    PROC_SORT_NAME,
    PROC_SORT_PATH,
} ProcSortKey;

typedef struct ProcRow {
    uint32_t pid;
    float cpuPct;
    uint64_t workingSetBytes;

    bool hasNet;
    wchar_t netRemote[64];

    wchar_t name[64];
    wchar_t path[MAX_PATH];
    wchar_t owner[96];
} ProcRow;

typedef struct ProcTable {
    ProcRow *rows;
    uint32_t rowCount;
    uint32_t rowCap;

    // Previous sample state for CPU% (100ns units)
    uint64_t prevSysTotal100ns;
    bool prevInit;

    struct PrevPidTime {
        uint32_t pid;
        uint64_t procTotal100ns;
        bool seen;
    } *prev;
    uint32_t prevCount;
    uint32_t prevCap;
} ProcTable;

void ProcTable_Init(ProcTable *pt);
void ProcTable_Shutdown(ProcTable *pt);

// Samples process list and fills pt->rows with top processes by CPU%.
// Best-effort fields: path/owner/net may be empty if access is denied.
void ProcTable_Sample(ProcTable *pt);

// Sorts pt->rows in-place.
void ProcTable_Sort(ProcTable *pt, ProcSortKey key, bool ascending);

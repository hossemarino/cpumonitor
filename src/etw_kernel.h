#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct EtwRates {
    double cswitchPerSec;
    double isrPerSec;
    double dpcPerSec;

    // Best-effort kernel category rates (classified via EventDescriptor.Keyword)
    double threadPerSec;
    double processPerSec;
    double imageLoadPerSec;
    double dispatcherPerSec;
    double syscallPerSec;
    double profilePerSec;
    double pageFaultPerSec;
    double fileIoPerSec;
    double diskIoPerSec;
    double tcpipPerSec;
    double registryPerSec;
    double otherPerSec;
} EtwRates;

typedef struct EtwKernel {
    bool ok;

    // Failure diagnostics (best-effort): if ok==false, these may explain why.
    // lastStage: 0=none, 1=StartTrace, 2=OpenTrace
    uint32_t lastStage;
    uint32_t lastStatus;

    // cumulative counters (written by ETW thread)
    volatile long long cswitchCount;
    volatile long long isrCount;
    volatile long long dpcCount;

    volatile long long threadCount;
    volatile long long processCount;
    volatile long long imageLoadCount;
    volatile long long dispatcherCount;
    volatile long long syscallCount;
    volatile long long profileCount;
    volatile long long pageFaultCount;
    volatile long long fileIoCount;
    volatile long long diskIoCount;
    volatile long long tcpipCount;
    volatile long long registryCount;
    volatile long long otherCount;

    // baselines for rate calculation
    long long prevCS;
    long long prevISR;
    long long prevDPC;

    long long prevThread;
    long long prevProcess;
    long long prevImageLoad;
    long long prevDispatcher;
    long long prevSyscall;
    long long prevProfile;
    long long prevPageFault;
    long long prevFileIo;
    long long prevDiskIo;
    long long prevTcpip;
    long long prevRegistry;
    long long prevOther;

    // internal
    void *thread;
    volatile long stopRequested;
    wchar_t sessionName[64];

    // Best-effort: set true once startup has been attempted.
    bool startAttempted;
} EtwKernel;

bool EtwKernel_Start(EtwKernel *k);
void EtwKernel_Stop(EtwKernel *k);

// Returns a short human-oriented status string.
// If ETW is running, this returns an empty string.
void EtwKernel_GetStatusText(const EtwKernel *k, wchar_t *out, uint32_t outCount);

// Computes rates from counter deltas over dt, and updates internal baselines.
void EtwKernel_ComputeRates(EtwKernel *k, double dt, EtwRates *out);

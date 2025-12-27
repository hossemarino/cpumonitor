# CCM (Const CPU Monitor)

A small Win32 C application inspired by `htop`, with a GPU-accelerated Direct2D UI.

## Features

- Direct2D/DirectWrite UI with a rolling total-CPU history graph
- CPU identification via CPUID (vendor/brand/family/model/stepping) + ISA flags
- CPU topology/cache info via `GetLogicalProcessorInformationEx`
- Best-effort TSC frequency estimate (RDTSC vs QPC)
- Live metrics via PDH (total/per-core CPU usage, context switches/sec, interrupts/sec, queue length, optional power meter)
- Per-core frequency via `CallNtPowerInformation(ProcessorInformation)`
- Best-effort sensors via WMI (thermal zone temperature, fan RPM when exposed)
- ETW kernel tracing (best-effort): context switches / ISR / DPC and additional kernel categories (thread/process/dispatcher/syscall/…)
- View menu: toggle CPU0–CPU15 bar graphs
- Help menu: opens HTML/CHM help if present; otherwise uses built-in help window

## Build (MSYS2 / MinGW-w64)

This repo includes a PowerShell build script that uses MSYS2’s GCC.

1. Install MSYS2 and a toolchain (e.g. UCRT64):
   - `pacman -S --needed mingw-w64-ucrt-x86_64-gcc`
2. Build:
   - `pwsh -NoProfile -ExecutionPolicy Bypass -File .\build_msys2.ps1`
3. Run:
   - `./build/CCM_all.exe` (preferred “single-file-ish” build)
   - `./build/CCM.exe` (normal build)

If the EXE is currently running, Windows will lock it and rebuild will fail—close the app before rebuilding.

To avoid overwriting a running binary, either set a single-output name:

- ` $env:CCM_EXE='CCM_dev.exe'; pwsh -NoProfile -ExecutionPolicy Bypass -File .\build_msys2.ps1`

Build modes:

- Default: builds both `CCM.exe` and `CCM_all.exe`
- Only normal: `pwsh -NoProfile -ExecutionPolicy Bypass -File .\build_msys2.ps1 -Build normal`
- Only all-in-one: `pwsh -NoProfile -ExecutionPolicy Bypass -File .\build_msys2.ps1 -Build all`
- Clean outputs: `pwsh -NoProfile -ExecutionPolicy Bypass -File .\build_msys2.ps1 -Clean`

Note: `CCM_all.exe` still depends on Windows system DLLs (Direct2D/DirectWrite), but it should not require MSYS2 runtime DLLs.

## Build (CMake)

A `CMakeLists.txt` is provided, but your environment must have CMake installed and configured.

## Help (HTML / CHM)

HTML help lives in the `help/` folder:

- `help/index.html`
- `help/metrics.html`
- `help/isa.html`
- `help/etw.html`

This repo includes the HTML Help Workshop project files:

- `help/CCM.hhp`
- `help/CCM.hhc`
- `help/CCM.hhk`

To build a CHM you need **Microsoft HTML Help Workshop** (it provides `hhc.exe`).

- Build (from repo root): `pwsh -NoProfile -ExecutionPolicy Bypass -File .\help\make_chm.ps1`
- Output: `help/CCM.chm`

When you click **Help → Metrics Help** the app tries (in order):

1. `help/CCM.chm` next to the EXE
2. `..\help\CCM.chm` (useful when running from `build\`)
3. Built-in help window fallback

## Notes on ETW

- ETW kernel tracing is best-effort.
- Some kernel flags/events may require elevated privileges or be blocked by policy.
- If ETW can’t start, the app still runs; ETW fields show as N/A/0.

## Driver mode (future expansion)

CCM is intentionally **driver-free** and designed to run safely in user mode.

Driver-mode sensors are a possible future expansion, but are currently treated as out-of-scope for this small project.
The help system retains documentation about feasibility and safety rules (user-mode vs kernel-mode, threat model, AMD-first roadmap).


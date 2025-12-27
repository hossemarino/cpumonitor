cmake_minimum_required(VERSION 3.20) # Minimum CMake version
project(CCM C) # Project name and language


set(CMAKE_C_STANDARD 17) # Use C17 standard
set(CMAKE_C_STANDARD_REQUIRED ON) # Enforce C standard

# Shared sources
set(CCM_SOURCES
  src/main.c
  src/app.c
  src/app.h
  src/help_window.c
  src/help_window.h
  src/external_sensors.c
  src/external_sensors.h
  src/proc_table.c
  src/proc_table.h
  src/render_d2d.c
  src/render_d2d.h
  src/cpu_static.c
  src/cpu_static.h
  src/gpu_perf.c
  src/gpu_perf.h
  src/pdh_counters.c
  src/pdh_counters.h
  src/ringbuf.c
  src/ringbuf.h
  src/wmi_sensors.c
  src/wmi_sensors.h
  src/power_cpu.c
  src/power_cpu.h
  src/guids.c
  src/etw_kernel.c
  src/etw_kernel.h
)

add_executable(CCM WIN32 ${CCM_SOURCES})

# Single-file-ish build: statically link the compiler runtime where possible.
# Note: Windows system DLLs (e.g., d2d1.dll) are still required.
add_executable(CCM_all WIN32 ${CCM_SOURCES})
# Define UNICODE for Windows API
target_compile_definitions(CCM PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX CCM_SAFE_MODE=1)
target_compile_definitions(CCM_all PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX CCM_SAFE_MODE=1)

# MSVC warnings
if (MSVC)
  target_compile_options(CCM PRIVATE /W4 /permissive-) # Enable high warning level and standard conformance
  target_compile_options(CCM_all PRIVATE /W4 /permissive-) # Enable high warning level and standard conformance

  # Prefer static CRT for the all-in-one EXE.
  set_property(TARGET CCM_all PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_compile_options(CCM PRIVATE -Wall -Wextra -Wpedantic) # Enable high warning levels for other compilers
  target_compile_options(CCM_all PRIVATE -Wall -Wextra -Wpedantic) # Enable high warning levels for other compilers

  # Prefer static libgcc/libwinpthread where available.
  target_link_options(CCM_all PRIVATE -static -static-libgcc)
endif()

# Link necessary Windows libraries
target_link_libraries(CCM PRIVATE
  user32 gdi32 ole32 oleaut32 wbemuuid
  d2d1 dwrite dxgi
  pdh
  powrprof
  advapi32
  psapi
  iphlpapi
  ws2_32
)

target_link_libraries(CCM_all PRIVATE
  user32 gdi32 ole32 oleaut32 wbemuuid
  d2d1 dwrite dxgi
  pdh
  powrprof
  advapi32
  psapi
  iphlpapi
  ws2_32
)

# Optional sample external sensor provider (auto-started by CCM when present).
add_executable(CCM_sensor_provider WIN32
  tools/ccm_sensor_provider.c
  src/wmi_sensors.c
  src/wmi_sensors.h
)
target_compile_definitions(CCM_sensor_provider PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(CCM_sensor_provider PRIVATE
  ole32 oleaut32 wbemuuid
  pdh
)
set_target_properties(CCM_sensor_provider PROPERTIES OUTPUT_NAME "CCM_sensor_provider")

set_target_properties(CCM_all PROPERTIES OUTPUT_NAME "CCM_all")

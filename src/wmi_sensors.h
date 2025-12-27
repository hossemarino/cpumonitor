#pragma once

#include <stdbool.h>

typedef struct WmiSensors {
    bool ok;
    void *servicesWmi;   // IWbemServices* (ROOT\\WMI)
    void *servicesCimv2; // IWbemServices* (ROOT\\CIMV2)
    void *locator;       // IWbemLocator*
} WmiSensors;

bool WmiSensors_Init(WmiSensors *w);
void WmiSensors_Shutdown(WmiSensors *w);

// Best-effort: tries MSAcpi_ThermalZoneTemperature (often unavailable / not CPU package temp).
bool WmiSensors_TryReadCpuTempC(WmiSensors *w, float *outTempC);

// Best-effort: tries Win32_Fan (rarely exposed). Returns average RPM if any.
bool WmiSensors_TryReadFanRpm(WmiSensors *w, float *outRpm);

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Optional external sensor provider.
//
// CCM will attempt to query a local named pipe provider for better sensor readings.
// This allows a separate service/driver stack to provide temperatures/power/etc.
// without baking any driver code into CCM itself.
//
// Wire protocol (request/response, UTF-8 text):
//   Client sends: "GET\n"
//   Server replies with lines:
//     tempC=<float>\n
//     powerW=<float>\n
//     fanRpm=<float>\n
//   Unknown keys are ignored. Missing keys mean "not available".
//
// Named pipe: \\.\pipe\ccm_sensors

typedef struct ExternalSensorsSample {
    bool hasCpuTempC;
    float cpuTempC;

    bool hasPowerW;
    double powerW;

    bool hasFanRpm;
    float fanRpm;

    // Human-readable status for troubleshooting/logging.
    // (Currently not shown in the UI.)
    wchar_t status[160];
} ExternalSensorsSample;

bool ExternalSensors_TrySample(ExternalSensorsSample *out);

#include "wmi_sensors.h"

#include <windows.h>

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <wbemidl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool ensure_init(WmiSensors *w)
{
    return w && w->ok && w->servicesWmi;
}

bool WmiSensors_Init(WmiSensors *w)
{
    memset(w, 0, sizeof(*w));

    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        w->ok = false;
        return false;
    }

    // Security can fail if already initialized elsewhere; treat as best-effort.
    CoInitializeSecurity(NULL, -1, NULL, NULL,
                         RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);

    IWbemLocator *loc = NULL;
    hr = CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (LPVOID *)&loc);
    if (FAILED(hr)) {
        w->ok = false;
        return false;
    }

    IWbemServices *svc = NULL;
    hr = IWbemLocator_ConnectServer(loc,
                                    L"ROOT\\WMI",
                                    NULL, NULL, NULL, 0, NULL, NULL, &svc);
    if (FAILED(hr)) {
        IWbemLocator_Release(loc);
        w->ok = false;
        return false;
    }

    IWbemServices *svc2 = NULL;
    hr = IWbemLocator_ConnectServer(loc,
                                    L"ROOT\\CIMV2",
                                    NULL, NULL, NULL, 0, NULL, NULL, &svc2);
    if (FAILED(hr)) {
        // Still OK; fans/etc are optional.
        svc2 = NULL;
    }

    // Set proxy blanket
    CoSetProxyBlanket((IUnknown *)svc,
                      RPC_C_AUTHN_WINNT,
                      RPC_C_AUTHZ_NONE,
                      NULL,
                      RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE,
                      NULL,
                      EOAC_NONE);

    if (svc2) {
        CoSetProxyBlanket((IUnknown *)svc2,
                          RPC_C_AUTHN_WINNT,
                          RPC_C_AUTHZ_NONE,
                          NULL,
                          RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE,
                          NULL,
                          EOAC_NONE);
    }

    w->locator = loc;
    w->servicesWmi = svc;
    w->servicesCimv2 = svc2;
    w->ok = true;
    return true;
}

void WmiSensors_Shutdown(WmiSensors *w)
{
    if (!w) return;
    if (w->servicesWmi) {
        IWbemServices *svc = (IWbemServices *)w->servicesWmi;
        IWbemServices_Release(svc);
        w->servicesWmi = NULL;
    }
    if (w->servicesCimv2) {
        IWbemServices *svc = (IWbemServices *)w->servicesCimv2;
        IWbemServices_Release(svc);
        w->servicesCimv2 = NULL;
    }
    if (w->locator) {
        IWbemLocator *loc = (IWbemLocator *)w->locator;
        IWbemLocator_Release(loc);
        w->locator = NULL;
    }
    memset(w, 0, sizeof(*w));
    // Intentionally not calling CoUninitialize here: app may share COM state.
}

bool WmiSensors_TryReadCpuTempC(WmiSensors *w, float *outTempC)
{
    if (!ensure_init(w) || !outTempC) return false;

    IWbemServices *svc = (IWbemServices *)w->servicesWmi;

    IEnumWbemClassObject *en = NULL;
    HRESULT hr = IWbemServices_ExecQuery(
        svc,
        L"WQL",
        L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL,
        &en);

    if (FAILED(hr) || !en) {
        return false;
    }

    IWbemClassObject *obj = NULL;
    ULONG ret = 0;
    hr = IEnumWbemClassObject_Next(en, 0, 1, &obj, &ret);
    if (FAILED(hr) || ret == 0 || !obj) {
        IEnumWbemClassObject_Release(en);
        return false;
    }

    VARIANT v;
    VariantInit(&v);
    hr = IWbemClassObject_Get(obj, L"CurrentTemperature", 0, &v, NULL, NULL);

    bool ok = false;
    if (SUCCEEDED(hr) && (V_VT(&v) == VT_I4 || V_VT(&v) == VT_UI4)) {
        // Value is tenths of Kelvin.
        const long raw = (V_VT(&v) == VT_I4) ? V_I4(&v) : (long)V_UI4(&v);
        const float c = (raw / 10.0f) - 273.15f;
        if (c > -50.0f && c < 150.0f) {
            *outTempC = c;
            ok = true;
        }
    }

    VariantClear(&v);
    IWbemClassObject_Release(obj);
    IEnumWbemClassObject_Release(en);
    return ok;
}

bool WmiSensors_TryReadFanRpm(WmiSensors *w, float *outRpm)
{
    if (!w || !w->ok || !w->servicesCimv2 || !outRpm) return false;

    IWbemServices *svc = (IWbemServices *)w->servicesCimv2;

    IEnumWbemClassObject *en = NULL;
    HRESULT hr = IWbemServices_ExecQuery(
        svc,
        L"WQL",
        L"SELECT DesiredSpeed, CurrentSpeed FROM Win32_Fan",
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL,
        &en);

    if (FAILED(hr) || !en) {
        return false;
    }

    float sum = 0.0f;
    uint32_t count = 0;

    for (;;) {
        IWbemClassObject *obj = NULL;
        ULONG ret = 0;
        hr = IEnumWbemClassObject_Next(en, 0, 1, &obj, &ret);
        if (FAILED(hr) || ret == 0 || !obj) {
            break;
        }

        VARIANT v;
        VariantInit(&v);
        // Prefer CurrentSpeed; fallback to DesiredSpeed
        hr = IWbemClassObject_Get(obj, L"CurrentSpeed", 0, &v, NULL, NULL);
        if (SUCCEEDED(hr) && (V_VT(&v) == VT_I4 || V_VT(&v) == VT_UI4)) {
            long raw = (V_VT(&v) == VT_I4) ? V_I4(&v) : (long)V_UI4(&v);
            if (raw > 0 && raw < 100000) {
                sum += (float)raw;
                count++;
            }
        }
        VariantClear(&v);

        if (count == 0) {
            VariantInit(&v);
            hr = IWbemClassObject_Get(obj, L"DesiredSpeed", 0, &v, NULL, NULL);
            if (SUCCEEDED(hr) && (V_VT(&v) == VT_I4 || V_VT(&v) == VT_UI4)) {
                long raw = (V_VT(&v) == VT_I4) ? V_I4(&v) : (long)V_UI4(&v);
                if (raw > 0 && raw < 100000) {
                    sum += (float)raw;
                    count++;
                }
            }
            VariantClear(&v);
        }

        IWbemClassObject_Release(obj);
    }

    IEnumWbemClassObject_Release(en);

    if (count == 0) {
        return false;
    }

    *outRpm = sum / (float)count;
    return true;
}

#include <windows.h>

#include <shellapi.h>

#include "app.h"

static void try_enable_privilege(const wchar_t *privName)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return;
    }

    LUID luid;
    if (!LookupPrivilegeValueW(NULL, privName, &luid)) {
        CloseHandle(token);
        return;
    }

    TOKEN_PRIVILEGES tp;
    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(token);
}

static void try_enable_common_privileges(void)
{
    // Best-effort: many ETW kernel sessions require SeSystemProfilePrivilege.
    try_enable_privilege(L"SeSystemProfilePrivilege");

    // Best-effort: helps with process inspection/termination on some targets.
    try_enable_privilege(L"SeDebugPrivilege");
}

static bool is_running_as_admin(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elev;
    DWORD cb = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &cb);
    CloseHandle(token);
    if (!ok) {
        return false;
    }
    return elev.TokenIsElevated ? true : false;
}

static void maybe_prompt_elevation(HINSTANCE hInstance, PWSTR lpCmdLine)
{
    (void)hInstance;

    if (is_running_as_admin()) {
        return;
    }

    const int res = MessageBoxW(NULL,
                                L"Run as Administrator?\n\n"
                                L"This enables ETW kernel tracing on most systems.\n"
                                L"\n"
                                L"Yes: restart elevated\n"
                                L"No: continue with best-effort monitorable metrics",
                                L"CCM - Const CPU Monitor", MB_YESNO | MB_ICONQUESTION);

    if (res != IDYES) {
        return;
    }

    wchar_t exePath[MAX_PATH];
    exePath[0] = 0;
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    // Use lpCmdLine so the restarted process keeps the same arguments.
    HINSTANCE r = ShellExecuteW(NULL, L"runas", exePath, lpCmdLine, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) {
        ExitProcess(0);
    }
    // If the user cancels UAC, just continue non-elevated.
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;

    maybe_prompt_elevation(hInstance, lpCmdLine);

    // If we are elevated, try enabling privileges needed by ETW/process control.
    // (If not elevated, this is harmless and just fails.)
    try_enable_common_privileges();

    App app;
    if (!App_Init(&app, hInstance)) {
        return 1;
    }

    App_Show(&app, nCmdShow);
    int exitCode = App_Run(&app);
    App_Shutdown(&app);
    return exitCode;
}

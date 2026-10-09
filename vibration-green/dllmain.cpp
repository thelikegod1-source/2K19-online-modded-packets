/**
 * autogreen.dll v6 — Vibration-based auto-green for NBA 2K19
 *
 * Uses Microsoft Detours to hook XInputSetState and detect vibration.
 * 2K19 vibrates at the green release window — when vibration is
 * detected while X is held, it releases the shoot button.
 *
 * No memory scanning needed. Just inject, press F8, and shoot.
 *
 * Hotkeys:
 *   F8  = toggle auto-green on/off
 *   F9  = dump state to log
 *   F10 = toggle lag switch
 *   F12 = uninject DLL
 */

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <atomic>

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

#include <detours.h>
#pragma comment(lib, "detours.lib")

// ─── Logging ────────────────────────────────────────────────────────────────

static FILE* g_Log = nullptr;
static CRITICAL_SECTION g_LogLock;

static void LogInit()
{
    InitializeCriticalSection(&g_LogLock);
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        char dir[MAX_PATH];
        snprintf(dir, MAX_PATH, "%s\\NBA2K-AutoGreen", appdata);
        CreateDirectoryA(dir, NULL);
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\autogreen.log", dir);
        g_Log = fopen(path, "w");
    }
}

static void Log(const char* fmt, ...)
{
    if (!g_Log) return;
    EnterCriticalSection(&g_LogLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_Log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_Log, fmt, args);
    va_end(args);
    fprintf(g_Log, "\n");
    fflush(g_Log);
    LeaveCriticalSection(&g_LogLock);
}

// ─── Globals ────────────────────────────────────────────────────────────────

static std::atomic<bool> g_Running{true};
static HMODULE g_Module = nullptr;
static std::atomic<bool> g_AutoGreen{false};
static std::atomic<int> g_ShotsTaken{0};
static std::atomic<bool> g_LagActive{false};
static HANDLE g_Thread = nullptr;

// ─── Vibration hook via Detours ─────────────────────────────────────────────

typedef DWORD (WINAPI *fn_XInputSetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD (WINAPI *fn_XInputGetState)(DWORD, XINPUT_STATE*);

static fn_XInputSetState g_RealSetState = nullptr;
static fn_XInputGetState g_RealGetState = nullptr;
static std::atomic<bool> g_VibrationDetected{false};
static std::atomic<WORD> g_LastLeftMotor{0};
static std::atomic<WORD> g_LastRightMotor{0};
static bool g_Hooked = false;

static DWORD WINAPI MyXInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
{
    if (pVibration) {
        g_LastLeftMotor = pVibration->wLeftMotorSpeed;
        g_LastRightMotor = pVibration->wRightMotorSpeed;
        if ((pVibration->wLeftMotorSpeed > 0 || pVibration->wRightMotorSpeed > 0)
            && g_AutoGreen) {
            g_VibrationDetected = true;
        }
    }
    return g_RealSetState(dwUserIndex, pVibration);
}

static bool InstallHook()
{
    const char* xinputDlls[] = {"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll"};
    HMODULE hXInput = NULL;
    const char* loadedXInput = nullptr;

    for (auto dllName : xinputDlls) {
        hXInput = GetModuleHandleA(dllName);
        if (hXInput) { loadedXInput = dllName; break; }
    }
    if (!hXInput) {
        Log("WARNING: No XInput DLL loaded");
        return false;
    }
    Log("Found XInput: %s", loadedXInput);

    g_RealSetState = (fn_XInputSetState)GetProcAddress(hXInput, "XInputSetState");
    g_RealGetState = (fn_XInputGetState)GetProcAddress(hXInput, "XInputGetState");

    if (!g_RealSetState) {
        Log("WARNING: XInputSetState not found");
        return false;
    }
    Log("XInputSetState at 0x%llX", (unsigned long long)g_RealSetState);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach((PVOID*)&g_RealSetState, (PVOID)MyXInputSetState);
    LONG err = DetourTransactionCommit();

    if (err != NO_ERROR) {
        Log("WARNING: Detours attach failed (error %ld)", err);
        return false;
    }

    Log("Detours hook installed on XInputSetState");
    return true;
}

static void RemoveHook()
{
    if (!g_Hooked) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach((PVOID*)&g_RealSetState, (PVOID)MyXInputSetState);
    DetourTransactionCommit();
    g_Hooked = false;
    Log("Detours hook removed");
}

// ─── Auto-green thread ─────────────────────────────────────────────────────

static void AutoGreenThread()
{
    Log("=== AUTO-GREEN v6 (DETOURS) ACTIVE ===");

    if (!g_Hooked) {
        g_Hooked = InstallHook();
    }
    if (!g_Hooked) {
        Log("Cannot run without hook. Auto-green stopped.");
        g_AutoGreen = false;
        return;
    }

    while (g_AutoGreen && g_Running) {
        if (g_VibrationDetected.exchange(false)) {
            XINPUT_STATE state;
            bool xHeld = false;
            if (g_RealGetState && g_RealGetState(0, &state) == ERROR_SUCCESS) {
                xHeld = (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;
            }

            if (xHeld) {
                g_ShotsTaken++;
                Log(">>> VIBRATION RELEASE #%d (motors L=%u R=%u) <<<",
                    g_ShotsTaken.load(), g_LastLeftMotor.load(), g_LastRightMotor.load());

                HMODULE hXInput = nullptr;
                const char* xinputDlls[] = {"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll"};
                for (auto dll : xinputDlls) {
                    hXInput = GetModuleHandleA(dll);
                    if (hXInput) break;
                }

                typedef void (WINAPI *fn_XInputEnable)(BOOL);
                fn_XInputEnable pEnable = nullptr;
                if (hXInput) pEnable = (fn_XInputEnable)GetProcAddress(hXInput, "XInputEnable");

                if (pEnable) {
                    pEnable(FALSE);
                    pEnable(TRUE);
                    Log("Released via XInputEnable");
                }

                Sleep(200);
            }
        }
        Sleep(1);
    }
    Log("Auto-green stopped");
}

// ─── Lag switch ─────────────────────────────────────────────────────────────

static bool RunNetsh(const char* args)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "netsh %s", args);
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

static void ToggleLag()
{
    if (!g_LagActive) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char a1[1024], a2[1024];
        snprintf(a1, sizeof(a1),
            "advfirewall firewall add rule name=\"2K19Secret_Lag\" dir=out action=block program=\"%s\" enable=yes", exePath);
        snprintf(a2, sizeof(a2),
            "advfirewall firewall add rule name=\"2K19Secret_Lag_In\" dir=in action=block program=\"%s\" enable=yes", exePath);
        bool ok = RunNetsh(a1) && RunNetsh(a2);
        g_LagActive = true;
        Log("LAG ON %s", ok ? "OK" : "FAILED (need admin)");
    } else {
        RunNetsh("advfirewall firewall delete rule name=\"2K19Secret_Lag\"");
        RunNetsh("advfirewall firewall delete rule name=\"2K19Secret_Lag_In\"");
        g_LagActive = false;
        Log("LAG OFF");
    }
}

// ─── Hotkey thread ──────────────────────────────────────────────────────────

static void HotkeyThread()
{
    Log("Hotkeys: F8=toggle auto-green, F9=dump, F10=lag, F12=uninject");

    while (g_Running) {
        if (GetAsyncKeyState(VK_F8) & 1) {
            if (g_AutoGreen) {
                g_AutoGreen = false;
                Log("F8: Auto-green DISABLED");
            } else {
                g_AutoGreen = true;
                CreateThread(NULL, 0,
                    [](LPVOID) -> DWORD { AutoGreenThread(); return 0; },
                    NULL, 0, NULL);
                Log("F8: Auto-green ENABLED (Detours vibration mode)");
            }
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            Log("=== F9: STATE DUMP ===");
            Log("  Auto-green: %s", g_AutoGreen.load() ? "ON" : "OFF");
            Log("  Shots: %d", g_ShotsTaken.load());
            Log("  Hook: %s", g_Hooked ? "YES" : "NO");
            Log("  Last motors: L=%u R=%u", g_LastLeftMotor.load(), g_LastRightMotor.load());
            Log("  Lag switch: %s", g_LagActive.load() ? "ON" : "OFF");
        }
        if (GetAsyncKeyState(VK_F10) & 1) {
            ToggleLag();
        }
        if (GetAsyncKeyState(VK_F12) & 1) {
            Log("F12: UNINJECTING...");
            RemoveHook();
            if (g_LagActive) ToggleLag();
            g_Running = false;
            g_AutoGreen = false;
            Sleep(200);
            Log("autogreen.dll unloaded (uninject)");
            if (g_Log) { fclose(g_Log); g_Log = nullptr; }
            CloseHandle(g_Thread);
            g_Thread = nullptr;
            FreeLibraryAndExitThread(g_Module, 0);
        }
        Sleep(50);
    }
}

// ─── DllMain ────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_Module = hModule;
        LogInit();
        Log("autogreen.dll v6 loaded (DETOURS MODE) - pid %u", GetCurrentProcessId());
        g_Thread = CreateThread(NULL, 0,
            [](LPVOID) -> DWORD { HotkeyThread(); return 0; },
            NULL, 0, NULL);
        Log("Ready! Press F8 to enable auto-green.");
        break;

    case DLL_PROCESS_DETACH:
        RemoveHook();
        if (g_LagActive) ToggleLag();
        g_Running = false;
        g_AutoGreen = false;
        if (g_Thread) {
            WaitForSingleObject(g_Thread, 3000);
            CloseHandle(g_Thread);
        }
        Log("autogreen.dll unloaded");
        if (g_Log) fclose(g_Log);
        break;
    }
    return TRUE;
}

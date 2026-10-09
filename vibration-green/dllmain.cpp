/**
 * autogreen.dll v8 — Hybrid Timer + Vibration Auto-Calibration
 *
 * Uses timer as the base release mechanism (default 670ms).
 * Monitors vibration feedback to auto-adjust the delay:
 *   - If vibration arrives BEFORE timer fires → delay too long, decrease
 *   - If vibration arrives AFTER timer fires → delay too short, increase
 *   - Converges to optimal timing for current lag conditions
 *
 * Hotkeys:
 *   F5  = decrease timer by 5ms (manual adjust)
 *   F6  = increase timer by 5ms (manual adjust)
 *   F7  = toggle auto-calibration on/off
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
#pragma comment(lib, "winmm.lib")

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

// ─── Hybrid Timer + Vibration system ────────────────────────────────────────

typedef DWORD (WINAPI *fn_XInputSetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD (WINAPI *fn_XInputGetState)(DWORD, XINPUT_STATE*);

static fn_XInputSetState g_RealSetState = nullptr;
static fn_XInputGetState g_RealGetState = nullptr;
static bool g_Hooked = false;

// Shared state
static std::atomic<bool> g_BlockX{false};
static std::atomic<DWORD> g_BlockXUntil{0};
static std::atomic<bool> g_ShotActive{false};

// Timer state
static std::atomic<int> g_ReleaseDelayMs{670};
static LARGE_INTEGER g_XPressTime = {};
static LARGE_INTEGER g_PerfFreq = {};
static std::atomic<bool> g_TimerFired{false};
static LARGE_INTEGER g_TimerFireTime = {};

// Auto-calibration state
static std::atomic<bool> g_AutoCalibrate{true};
static std::atomic<bool> g_VibrationSeen{false};
static LARGE_INTEGER g_VibrationTime = {};
static std::atomic<int> g_CalibrationCount{0};
static std::atomic<int> g_TotalAdjustment{0};

// Vibration monitoring
static std::atomic<WORD> g_LastLeftMotor{0};
static std::atomic<WORD> g_LastRightMotor{0};

static DWORD WINAPI MyXInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
{
    if (pVibration) {
        WORD prevLeft = g_LastLeftMotor.load();
        WORD prevRight = g_LastRightMotor.load();
        g_LastLeftMotor = pVibration->wLeftMotorSpeed;
        g_LastRightMotor = pVibration->wRightMotorSpeed;

        // Detect vibration onset (transition from zero to non-zero) during an active shot
        if (g_AutoGreen && g_ShotActive
            && (prevLeft == 0 && prevRight == 0)
            && (pVibration->wLeftMotorSpeed > 0 || pVibration->wRightMotorSpeed > 0)) {
            if (!g_VibrationSeen) {
                QueryPerformanceCounter(&g_VibrationTime);
                g_VibrationSeen = true;
            }
        }
    }
    return g_RealSetState(dwUserIndex, pVibration);
}

static DWORD WINAPI MyXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD result = g_RealGetState(dwUserIndex, pState);
    if (result != ERROR_SUCCESS || !g_AutoGreen) return result;

    bool xHeld = (pState->Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;

    // Detect X press start
    if (xHeld && !g_ShotActive && !g_BlockX) {
        QueryPerformanceCounter(&g_XPressTime);
        g_ShotActive = true;
        g_TimerFired = false;
        g_VibrationSeen = false;
    }

    // Timer fires after delay
    if (g_ShotActive && xHeld && !g_BlockX) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsedMs = (double)(now.QuadPart - g_XPressTime.QuadPart)
                         / (double)g_PerfFreq.QuadPart * 1000.0;
        if (elapsedMs >= (double)g_ReleaseDelayMs.load()) {
            g_ShotsTaken++;
            g_BlockXUntil = GetTickCount() + 200;
            g_BlockX = true;
            g_TimerFired = true;
            QueryPerformanceCounter(&g_TimerFireTime);

            // Auto-calibration: compare vibration timing to timer
            if (g_AutoCalibrate && g_VibrationSeen) {
                double vibMs = (double)(g_VibrationTime.QuadPart - g_XPressTime.QuadPart)
                             / (double)g_PerfFreq.QuadPart * 1000.0;
                double diff = vibMs - elapsedMs;
                // diff < 0 means vibration came BEFORE timer → we're late → decrease delay
                // diff > 0 means vibration came AFTER timer → we're early → increase delay
                int adjust = 0;
                if (diff < -10.0) adjust = -3;
                else if (diff < -3.0) adjust = -1;
                else if (diff > 10.0) adjust = 3;
                else if (diff > 3.0) adjust = 1;

                if (adjust != 0) {
                    int newDelay = g_ReleaseDelayMs.load() + adjust;
                    if (newDelay < 100) newDelay = 100;
                    if (newDelay > 1500) newDelay = 1500;
                    g_ReleaseDelayMs = newDelay;
                    g_CalibrationCount++;
                    g_TotalAdjustment += adjust;
                    Log("AUTO-CAL: vib=%.1fms timer=%.1fms diff=%.1fms adj=%+dms → delay=%dms",
                        vibMs, elapsedMs, diff, adjust, newDelay);
                }
            }
        }
    }

    // Reset shot when X released and not blocking
    if (!xHeld && !g_BlockX) {
        // Late vibration check: if timer hasn't fired yet but vibration arrived, we missed it
        if (g_ShotActive && !g_TimerFired && g_VibrationSeen && g_AutoCalibrate) {
            double vibMs = (double)(g_VibrationTime.QuadPart - g_XPressTime.QuadPart)
                         / (double)g_PerfFreq.QuadPart * 1000.0;
            int currentDelay = g_ReleaseDelayMs.load();
            // Vibration came but timer never fired — delay is way too long
            int adjust = -5;
            int newDelay = currentDelay + adjust;
            if (newDelay < 100) newDelay = 100;
            g_ReleaseDelayMs = newDelay;
            g_CalibrationCount++;
            g_TotalAdjustment += adjust;
            Log("AUTO-CAL (missed): vib=%.1fms delay=%dms → reduced to %dms",
                vibMs, currentDelay, newDelay);
        }
        g_ShotActive = false;
    }

    // Block X when flag is set
    if (g_BlockX) {
        DWORD now = GetTickCount();
        if (now < g_BlockXUntil) {
            pState->Gamepad.wButtons &= ~XINPUT_GAMEPAD_X;
        } else {
            g_BlockX = false;
        }
    }

    return result;
}

static bool InstallHook()
{
    QueryPerformanceFrequency(&g_PerfFreq);

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

    if (!g_RealSetState || !g_RealGetState) {
        Log("WARNING: XInput functions not found");
        return false;
    }
    Log("XInputSetState at 0x%llX", (unsigned long long)g_RealSetState);
    Log("XInputGetState at 0x%llX", (unsigned long long)g_RealGetState);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach((PVOID*)&g_RealSetState, (PVOID)MyXInputSetState);
    DetourAttach((PVOID*)&g_RealGetState, (PVOID)MyXInputGetState);
    LONG err = DetourTransactionCommit();

    if (err != NO_ERROR) {
        Log("WARNING: Detours attach failed (error %ld)", err);
        return false;
    }

    Log("Detours hooks installed");
    return true;
}

static void RemoveHook()
{
    if (!g_Hooked) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach((PVOID*)&g_RealSetState, (PVOID)MyXInputSetState);
    DetourDetach((PVOID*)&g_RealGetState, (PVOID)MyXInputGetState);
    DetourTransactionCommit();
    g_Hooked = false;
    Log("Detours hooks removed");
}

// ─── Auto-green thread ─────────────────────────────────────────────────────

static void AutoGreenThread()
{
    Log("=== AUTO-GREEN v8 ACTIVE (HYBRID AUTO-CAL) ===");
    Log("Timer delay: %d ms", g_ReleaseDelayMs.load());
    Log("Auto-calibration: %s", g_AutoCalibrate.load() ? "ON" : "OFF");

    if (!g_Hooked) {
        g_Hooked = InstallHook();
    }
    if (!g_Hooked) {
        Log("Cannot run without hooks. Auto-green stopped.");
        g_AutoGreen = false;
        return;
    }

    int lastShots = 0;
    while (g_AutoGreen && g_Running) {
        int shots = g_ShotsTaken.load();
        if (shots != lastShots) {
            Log(">>> RELEASE #%d (delay=%dms, cal=%s, adjustments=%d, total=%+dms) <<<",
                shots, g_ReleaseDelayMs.load(),
                g_AutoCalibrate.load() ? "ON" : "OFF",
                g_CalibrationCount.load(), g_TotalAdjustment.load());
            lastShots = shots;
        }
        Sleep(10);
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
    Log("Hotkeys: F5/F6=adjust timer, F7=toggle auto-cal, F8=toggle, F9=dump, F10=lag, F12=uninject");

    while (g_Running) {
        if (GetAsyncKeyState(VK_F5) & 1) {
            int d = g_ReleaseDelayMs.load();
            if (d > 5) {
                g_ReleaseDelayMs = d - 5;
                Log("F5: Timer delay = %d ms (earlier)", g_ReleaseDelayMs.load());
            }
        }
        if (GetAsyncKeyState(VK_F6) & 1) {
            int d = g_ReleaseDelayMs.load();
            g_ReleaseDelayMs = d + 5;
            Log("F6: Timer delay = %d ms (later)", g_ReleaseDelayMs.load());
        }
        if (GetAsyncKeyState(VK_F7) & 1) {
            bool was = g_AutoCalibrate.load();
            g_AutoCalibrate = !was;
            Log("F7: Auto-calibration %s", g_AutoCalibrate.load() ? "ON" : "OFF");
        }
        if (GetAsyncKeyState(VK_F8) & 1) {
            if (g_AutoGreen) {
                g_AutoGreen = false;
                Log("F8: Auto-green DISABLED");
            } else {
                g_AutoGreen = true;
                CreateThread(NULL, 0,
                    [](LPVOID) -> DWORD { AutoGreenThread(); return 0; },
                    NULL, 0, NULL);
                Log("F8: Auto-green ENABLED");
            }
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            Log("=== F9: STATE DUMP ===");
            Log("  Auto-green: %s", g_AutoGreen.load() ? "ON" : "OFF");
            Log("  Auto-calibration: %s", g_AutoCalibrate.load() ? "ON" : "OFF");
            Log("  Timer delay: %d ms", g_ReleaseDelayMs.load());
            Log("  Shots: %d", g_ShotsTaken.load());
            Log("  Calibrations: %d (total adj: %+dms)", g_CalibrationCount.load(), g_TotalAdjustment.load());
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
        Log("autogreen.dll v8 loaded (HYBRID AUTO-CAL) - pid %u", GetCurrentProcessId());
        g_Thread = CreateThread(NULL, 0,
            [](LPVOID) -> DWORD { HotkeyThread(); return 0; },
            NULL, 0, NULL);
        Log("Ready! Press F8 to enable. F5/F6 to adjust timer. F7 to toggle auto-cal.");
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

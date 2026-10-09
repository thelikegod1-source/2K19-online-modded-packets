/**
 * autogreen.dll v5 — Vibration-based auto-green for NBA 2K19
 *
 * Detects controller vibration to auto-release the shoot button.
 * 2K19 vibrates at the green release window — this catches it.
 *
 * Uses EAT hook on XInputSetState + IAT hook as backup.
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
#include <psapi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <atomic>

#pragma comment(lib, "psapi.lib")

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

// ─── Logging ────────────────────────────────────────────────────────────────

static FILE* g_Log = nullptr;
static CRITICAL_SECTION g_LogLock;
static char g_LogDir[MAX_PATH] = {};

static void LogInit()
{
    InitializeCriticalSection(&g_LogLock);
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        snprintf(g_LogDir, MAX_PATH, "%s\\NBA2K-AutoGreen", appdata);
        CreateDirectoryA(g_LogDir, NULL);
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\autogreen.log", g_LogDir);
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

// ─── Vibration detection ────────────────────────────────────────────────────

typedef DWORD (WINAPI *fn_XInputSetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD (WINAPI *fn_XInputGetState)(DWORD, XINPUT_STATE*);
static fn_XInputSetState g_OrigSetState = nullptr;
static fn_XInputGetState g_OrigGetState = nullptr;
static std::atomic<bool> g_VibrationDetected{false};
static std::atomic<WORD> g_LastLeftMotor{0};
static std::atomic<WORD> g_LastRightMotor{0};
static bool g_Hooked = false;

static DWORD WINAPI HookedXInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
{
    if (pVibration) {
        g_LastLeftMotor = pVibration->wLeftMotorSpeed;
        g_LastRightMotor = pVibration->wRightMotorSpeed;
        if ((pVibration->wLeftMotorSpeed > 0 || pVibration->wRightMotorSpeed > 0)
            && g_AutoGreen) {
            g_VibrationDetected = true;
        }
    }
    return g_OrigSetState(dwUserIndex, pVibration);
}

// ─── IAT patcher ────────────────────────────────────────────────────────────

static bool PatchIATFunc(HMODULE hModule, const char* targetDll,
                         const char* funcName, void* hookFunc, void** origFunc)
{
    __try {
        BYTE* base = (BYTE*)hModule;
        auto dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return false;

        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
            if (_stricmp((const char*)(base + imp->Name), targetDll) != 0) continue;

            IMAGE_THUNK_DATA* origThunk = imp->OriginalFirstThunk
                ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
            IMAGE_THUNK_DATA* iatThunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

            while (iatThunk->u1.Function) {
                bool match = false;
                if (origThunk && !IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) {
                    auto hint = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                    match = (strcmp((const char*)hint->Name, funcName) == 0);
                }
                if (!match && !origThunk) {
                    match = ((void*)iatThunk->u1.Function == *origFunc);
                }

                if (match) {
                    if (!*origFunc) *origFunc = (void*)iatThunk->u1.Function;
                    DWORD oldProtect;
                    VirtualProtect(&iatThunk->u1.Function, sizeof(uintptr_t),
                                   PAGE_READWRITE, &oldProtect);
                    iatThunk->u1.Function = (uintptr_t)hookFunc;
                    VirtualProtect(&iatThunk->u1.Function, sizeof(uintptr_t),
                                   oldProtect, &oldProtect);
                    return true;
                }

                iatThunk++;
                if (origThunk) origThunk++;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ─── EAT (Export Address Table) hook ────────────────────────────────────────

static bool PatchEAT(HMODULE hModule, const char* funcName, void* hookFunc, void** origFunc)
{
    __try {
        BYTE* base = (BYTE*)hModule;
        auto dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return false;

        auto exports = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
        DWORD* names = (DWORD*)(base + exports->AddressOfNames);
        WORD* ordinals = (WORD*)(base + exports->AddressOfNameOrdinals);
        DWORD* functions = (DWORD*)(base + exports->AddressOfFunctions);

        for (DWORD i = 0; i < exports->NumberOfNames; i++) {
            if (strcmp((const char*)(base + names[i]), funcName) == 0) {
                DWORD funcRVA = functions[ordinals[i]];
                *origFunc = (void*)(base + funcRVA);

                DWORD newRVA = (DWORD)((BYTE*)hookFunc - base);
                DWORD oldProtect;
                VirtualProtect(&functions[ordinals[i]], sizeof(DWORD),
                               PAGE_READWRITE, &oldProtect);
                functions[ordinals[i]] = newRVA;
                VirtualProtect(&functions[ordinals[i]], sizeof(DWORD),
                               oldProtect, &oldProtect);
                return true;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void InstallVibrationHook()
{
    if (g_Hooked) return;

    const char* xinputDlls[] = {"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll"};
    const char* loadedXInput = nullptr;
    HMODULE hXInput = NULL;

    for (auto dllName : xinputDlls) {
        hXInput = GetModuleHandleA(dllName);
        if (hXInput) { loadedXInput = dllName; break; }
    }
    if (!loadedXInput) {
        Log("WARNING: No XInput DLL loaded in game");
        return;
    }
    Log("Found XInput: %s", loadedXInput);

    g_OrigGetState = (fn_XInputGetState)GetProcAddress(hXInput, "XInputGetState");
    g_OrigSetState = (fn_XInputSetState)GetProcAddress(hXInput, "XInputSetState");
    Log("XInputSetState at 0x%llX", (unsigned long long)g_OrigSetState);

    // Method 1: Try IAT hook on all modules
    HMODULE mods[1024];
    DWORD needed = 0;
    int iatPatched = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        for (int i = 0; i < count; i++) {
            if (mods[i] == g_Module || mods[i] == hXInput) continue;
            void* orig = nullptr;
            if (PatchIATFunc(mods[i], loadedXInput, "XInputSetState",
                             (void*)HookedXInputSetState, &orig)) {
                iatPatched++;
            }
        }
    }
    if (iatPatched > 0) {
        g_Hooked = true;
        Log("IAT hook installed (%d modules)", iatPatched);
    }

    // Method 2: Also try EAT hook so GetProcAddress returns our hook
    void* eatOrig = nullptr;
    if (PatchEAT(hXInput, "XInputSetState", (void*)HookedXInputSetState, &eatOrig)) {
        g_Hooked = true;
        Log("EAT hook installed on %s", loadedXInput);
    }

    if (!g_Hooked) {
        Log("WARNING: No hook method worked");
    }
}

// ─── Auto-green thread ─────────────────────────────────────────────────────

static void AutoGreenThread()
{
    Log("=== AUTO-GREEN v5 (VIBRATION) ACTIVE ===");
    InstallVibrationHook();

    if (!g_Hooked) {
        Log("Cannot run without vibration hook. Auto-green stopped.");
        g_AutoGreen = false;
        return;
    }

    while (g_AutoGreen && g_Running) {
        if (g_VibrationDetected.exchange(false)) {
            XINPUT_STATE state;
            bool xHeld = false;
            if (g_OrigGetState && g_OrigGetState(0, &state) == ERROR_SUCCESS) {
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
                    Sleep(80);
                    pEnable(TRUE);
                    Log("Released via XInputEnable");
                }

                Sleep(300);
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
                Log("F8: Auto-green ENABLED (vibration mode)");
            }
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            Log("=== F9: STATE DUMP ===");
            Log("  Auto-green: %s", g_AutoGreen.load() ? "ON" : "OFF");
            Log("  Shots: %d", g_ShotsTaken.load());
            Log("  Vibration hook: %s", g_Hooked ? "YES" : "NO");
            Log("  Last motors: L=%u R=%u", g_LastLeftMotor.load(), g_LastRightMotor.load());
            Log("  Lag switch: %s", g_LagActive.load() ? "ON" : "OFF");
        }
        if (GetAsyncKeyState(VK_F10) & 1) {
            ToggleLag();
        }
        if (GetAsyncKeyState(VK_F12) & 1) {
            Log("F12: UNINJECTING...");
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
        Log("autogreen.dll v5 loaded (VIBRATION MODE) - pid %u", GetCurrentProcessId());
        g_Thread = CreateThread(NULL, 0,
            [](LPVOID) -> DWORD { HotkeyThread(); return 0; },
            NULL, 0, NULL);
        Log("Ready! Press F8 to enable auto-green. No meter scan needed!");
        break;

    case DLL_PROCESS_DETACH:
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

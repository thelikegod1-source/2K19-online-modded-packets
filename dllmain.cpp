/**
 * autogreen.dll v4 — Auto-green shot release for NBA 2K19
 *
 * v4: Simplified meter finder — no baseline needed. Just press F5 mid-shot
 * and it scans ALL readable memory for floats that are rising steadily.
 * Works with MEM_IMAGE + MEM_PRIVATE + any protection that allows reads.
 *
 * Hotkeys:
 *   F5 = find meter (press while shot meter is filling, ~1/3 full)
 *   F8 = toggle auto-green on/off
 *   F9 = dump current state to log
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
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>

#pragma comment(lib, "psapi.lib")

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
static std::atomic<uintptr_t> g_MeterAddr{0};
static std::atomic<float> g_GreenTarget{0.50f};
static std::atomic<float> g_FadeTarget{0.40f};
static std::atomic<bool> g_FindRequest{false};
static std::atomic<int> g_ShotsTaken{0};
static std::atomic<float> g_LastReleaseVal{0};
static std::atomic<bool> g_LagActive{false};
static HANDLE g_Thread = nullptr;

// ─── Safe memory read ───────────────────────────────────────────────────────

static bool SafeReadFloat(uintptr_t addr, float* out)
{
    __try {
        *out = *(volatile float*)addr;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ─── Memory regions — scan everything readable ──────────────────────────────

struct MemRegion { uintptr_t base; size_t size; };

static std::vector<MemRegion> GetScanRegions()
{
    std::vector<MemRegion> regions;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t maxAddr = (uintptr_t)si.lpMaximumApplicationAddress;

    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) break;

        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & PAGE_GUARD) &&
            !(mbi.Protect & PAGE_NOACCESS) &&
            mbi.RegionSize <= 256 * 1024 * 1024) {
            regions.push_back({(uintptr_t)mbi.BaseAddress, mbi.RegionSize});
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return regions;
}

// ─── Region scanner (SEH-safe, no C++ objects) ─────────────────────────────

static int ScanRegionForRange(uintptr_t base, size_t size,
                              float lo, float hi,
                              uintptr_t* outAddrs, float* outVals, int maxOut)
{
    int count = 0;
    __try {
        float* ptr = (float*)base;
        size_t n = size / sizeof(float);
        for (size_t i = 0; i < n && count < maxOut; i++) {
            float v = ptr[i];
            if (v >= lo && v <= hi) {
                outAddrs[count] = base + i * sizeof(float);
                outVals[count] = v;
                count++;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// ─── Simple meter finder ────────────────────────────────────────────────────

static uintptr_t FindMeterSimple()
{
    Log("=== METER SCAN (F5) ===");
    auto t0 = std::chrono::steady_clock::now();

    auto regions = GetScanRegions();
    size_t totalBytes = 0;
    for (auto& r : regions) totalBytes += r.size;
    Log("Scanning %zu regions (%.0f MB)...", regions.size(), totalBytes / (1024.0 * 1024.0));

    struct Addr { uintptr_t a; float v; };
    std::vector<Addr> pass1;
    pass1.reserve(2000000);

    const int BATCH = 65536;
    uintptr_t batchAddrs[65536];
    float batchVals[65536];

    for (auto& region : regions) {
        if (pass1.size() >= 10000000) break;
        size_t off = 0;
        while (off < region.size && pass1.size() < 10000000) {
            size_t chunk = region.size - off;
            if (chunk > (size_t)BATCH * 4) chunk = (size_t)BATCH * 4;
            int found = ScanRegionForRange(region.base + off, chunk,
                                           0.05f, 0.90f, batchAddrs, batchVals, BATCH);
            for (int j = 0; j < found; j++)
                pass1.push_back({batchAddrs[j], batchVals[j]});
            off += chunk;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    Log("Pass 1: %zu floats in [0.05, 0.90] (%.0f ms)",
        pass1.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());

    if (pass1.empty()) {
        Log("Nothing found. Are you mid-shot?");
        return 0;
    }

    // Pass 2: wait 50ms, keep only addresses where value went UP
    Sleep(50);
    std::vector<Addr> pass2;
    pass2.reserve(pass1.size() / 10);

    for (auto& p : pass1) {
        float v2;
        if (!SafeReadFloat(p.a, &v2)) continue;
        if (v2 > p.v + 0.001f && v2 >= 0.05f && v2 <= 0.98f) {
            pass2.push_back({p.a, v2});
        }
    }

    Log("Pass 2: %zu still rising after 50ms", pass2.size());
    if (pass2.empty()) {
        Log("No rising floats. Press F5 earlier in the shot.");
        return 0;
    }

    // Pass 3: wait another 50ms, keep ones still rising
    Sleep(50);
    struct Candidate { uintptr_t a; float v1, v2, v3; };
    std::vector<Candidate> pass3;

    for (auto& p : pass2) {
        float v3;
        if (!SafeReadFloat(p.a, &v3)) continue;
        if (v3 > p.v + 0.001f && v3 >= 0.05f && v3 <= 0.99f) {
            pass3.push_back({p.a, 0, p.v, v3});
        }
    }

    Log("Pass 3: %zu still rising after another 50ms", pass3.size());
    if (pass3.empty()) {
        Log("Lost all candidates. Shot may have ended. Try again!");
        return 0;
    }

    // Pass 4: one more check 50ms later for confidence
    Sleep(50);
    struct Final { uintptr_t a; float v2, v3, v4; float rate; };
    std::vector<Final> finals;

    for (auto& c : pass3) {
        float v4;
        if (!SafeReadFloat(c.a, &v4)) continue;
        if (v4 > c.v3 + 0.001f && v4 <= 1.05f) {
            float rate = (v4 - c.v2) / 0.100f;  // rise per 100ms
            finals.push_back({c.a, c.v2, c.v3, v4, rate});
        }
    }

    Log("Pass 4: %zu candidates confirmed rising over 150ms", finals.size());

    if (finals.empty()) {
        Log("No steady risers found. Press F5 when meter is ~1/3 full.");
        return 0;
    }

    // Pick the best: prefer rate closest to what a shot meter looks like (~0.5-1.5/sec)
    // Shot meter goes 0→1 in about 1-2 seconds, so rate ~0.05-0.15 per 100ms
    std::sort(finals.begin(), finals.end(), [](const Final& a, const Final& b) {
        float aFit = fabsf(a.rate - 0.10f);
        float bFit = fabsf(b.rate - 0.10f);
        return aFit < bFit;
    });

    int show = finals.size() < 10 ? (int)finals.size() : 10;
    Log("Top candidates:");
    for (int i = 0; i < show; i++) {
        Log("  [%d] addr=0x%llX  vals: %.3f -> %.3f -> %.3f  rate=%.3f/100ms",
            i, (unsigned long long)finals[i].a,
            finals[i].v2, finals[i].v3, finals[i].v4, finals[i].rate);
    }

    auto& best = finals[0];
    auto tEnd = std::chrono::steady_clock::now();
    Log("*** METER FOUND: addr=0x%llX (rate=%.3f/100ms, total %.0f ms) ***",
        (unsigned long long)best.a, best.rate,
        std::chrono::duration<double, std::milli>(tEnd - t0).count());
    return best.a;
}

// ─── XInput hook ────────────────────────────────────────────────────────────

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

typedef DWORD (WINAPI *fn_XInputGetState)(DWORD, XINPUT_STATE*);
static fn_XInputGetState g_OrigXInputGetState = nullptr;
typedef void (WINAPI *fn_XInputEnable)(BOOL);
static fn_XInputEnable g_XInputEnable = nullptr;
static std::atomic<bool> g_MaskShootButton{false};
static bool g_XInputHooked = false;

static DWORD WINAPI HookedXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD ret = g_OrigXInputGetState(dwUserIndex, pState);
    if (ret == ERROR_SUCCESS && g_MaskShootButton) {
        pState->Gamepad.wButtons &= ~XINPUT_GAMEPAD_X;
    }
    return ret;
}

static bool PatchModuleIAT(HMODULE hModule, const char* targetDll, void* hookFunc)
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
                    match = (strcmp((const char*)hint->Name, "XInputGetState") == 0);
                }

                if (match) {
                    g_OrigXInputGetState = (fn_XInputGetState)iatThunk->u1.Function;
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

static void InstallXInputHook()
{
    if (g_XInputHooked) return;

    const char* xinputDlls[] = {"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll"};
    const char* loadedXInput = nullptr;
    HMODULE hXInput = NULL;

    for (auto dllName : xinputDlls) {
        hXInput = GetModuleHandleA(dllName);
        if (hXInput) { loadedXInput = dllName; break; }
    }
    if (!loadedXInput) {
        Log("WARNING: No XInput DLL loaded");
        g_XInputEnable = nullptr;
        return;
    }
    Log("Found XInput: %s", loadedXInput);

    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        int patched = 0;
        for (int i = 0; i < count; i++) {
            if (mods[i] == g_Module || mods[i] == hXInput) continue;
            if (PatchModuleIAT(mods[i], loadedXInput, (void*)HookedXInputGetState)) {
                patched++;
            }
        }
        if (patched > 0) {
            g_XInputHooked = true;
            Log("XInput IAT hook installed (%d modules)", patched);
        }
    }

    if (!g_XInputHooked) {
        g_XInputEnable = (fn_XInputEnable)GetProcAddress(hXInput, "XInputEnable");
        if (g_XInputEnable) {
            Log("Fallback: using XInputEnable for shot release");
        } else {
            Log("WARNING: No hook method available");
        }
    }
}

// ─── Release helper ─────────────────────────────────────────────────────────

static void SetReleaseMask(bool on)
{
    if (g_XInputHooked) {
        g_MaskShootButton = on;
    } else if (g_XInputEnable) {
        g_XInputEnable(on ? FALSE : TRUE);
    }
}

// ─── Read config files ──────────────────────────────────────────────────────

static void ReadTargetConfigs()
{
    char path[MAX_PATH];

    snprintf(path, MAX_PATH, "%s\\target.txt", g_LogDir);
    FILE* f = fopen(path, "r");
    if (f) {
        float t = 0;
        if (fscanf(f, "%f", &t) == 1 && t >= 0.15f && t <= 0.95f) {
            if (fabsf(t - g_GreenTarget.load()) > 0.001f) {
                g_GreenTarget = t;
                Log("Standing target updated to %.2f", t);
            }
        }
        fclose(f);
    }

    snprintf(path, MAX_PATH, "%s\\fade_target.txt", g_LogDir);
    f = fopen(path, "r");
    if (f) {
        float t = 0;
        if (fscanf(f, "%f", &t) == 1 && t >= 0.15f && t <= 0.95f) {
            if (fabsf(t - g_FadeTarget.load()) > 0.001f) {
                g_FadeTarget = t;
                Log("Fade target updated to %.2f", t);
            }
        }
        fclose(f);
    }
}

// ─── Auto-green thread ─────────────────────────────────────────────────────

static void AutoGreenThread()
{
    Log("=== AUTO-GREEN ACTIVE === target=%.2f", g_GreenTarget.load());
    InstallXInputHook();
    ReadTargetConfigs();

    while (g_AutoGreen && g_Running) {
        uintptr_t meterAddr = g_MeterAddr.load();

        if (meterAddr == 0) {
            if (g_FindRequest.exchange(false)) {
                uintptr_t found = FindMeterSimple();
                if (found) g_MeterAddr = found;
            }
            Sleep(16);
            continue;
        }

        // === Main auto-green loop ===
        float lastVal = 0;
        bool released = false;
        int staleCount = 0;
        int zeroCount = 0;
        int configCounter = 0;

        Log("Monitoring meter at 0x%llX (standing=%.2f, fade=%.2f)...",
            (unsigned long long)meterAddr, g_GreenTarget.load(), g_FadeTarget.load());

        while (g_AutoGreen && g_Running) {
            if (g_FindRequest.exchange(false)) {
                uintptr_t found = FindMeterSimple();
                if (found) {
                    g_MeterAddr = found;
                    meterAddr = found;
                    Log("Meter re-found at 0x%llX", (unsigned long long)found);
                }
            }
            if (++configCounter > 2000) {
                configCounter = 0;
                ReadTargetConfigs();
            }

            float val;
            if (!SafeReadFloat(meterAddr, &val)) { Sleep(1); continue; }

            if (val >= -0.001f && val <= 0.001f) {
                zeroCount++;
                if (zeroCount > 50 && released) released = false;
                continue;
            }
            zeroCount = 0;

            if (val < 0.0f || val > 1.05f) {
                staleCount++;
                if (staleCount > 10000) {
                    Log("Meter stale (val=%.4f). Press F5 during a shot to re-find.", val);
                    g_MeterAddr = 0;
                    break;
                }
                continue;
            }
            staleCount = 0;

            if (released && lastVal > 0.01f && val < lastVal - 0.05f) {
                released = false;
                lastVal = 0;
            }

            float delta = (lastVal > 0.01f) ? (val - lastVal) : 0.0f;
            bool isFade = (delta > 0.025f);
            float target = isFade ? g_FadeTarget.load() : g_GreenTarget.load();

            if (val >= target && val <= 0.95f && !released) {
                Log(">>> %s RELEASE at %.4f (target %.2f) <<<",
                    isFade ? "FADE" : "GREEN", val, target);

                SetReleaseMask(true);
                Sleep(100);
                SetReleaseMask(false);

                g_LastReleaseVal = val;
                g_ShotsTaken++;
                released = true;
                Log("Shot #%d released at %.4f", g_ShotsTaken.load(), val);
                Sleep(300);
            }

            lastVal = val;
        }
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
    Log("Hotkeys: F5=find meter, F8=toggle auto-green, F9=dump, F10=lag, F12=uninject");

    while (g_Running) {
        if (GetAsyncKeyState(VK_F5) & 1) {
            if (g_AutoGreen) {
                g_FindRequest = true;
                Log("F5: meter scan requested");
            } else {
                uintptr_t found = FindMeterSimple();
                if (found) {
                    g_MeterAddr = found;
                    Log("F5: Meter locked at 0x%llX", (unsigned long long)found);
                }
            }
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
                Log("F8: Auto-green ENABLED (target=%.2f)", g_GreenTarget.load());
            }
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            Log("=== F9: STATE DUMP ===");
            Log("  Meter addr: 0x%llX", (unsigned long long)g_MeterAddr.load());
            Log("  Auto-green: %s", g_AutoGreen.load() ? "ON" : "OFF");
            Log("  Target: standing=%.2f fade=%.2f", g_GreenTarget.load(), g_FadeTarget.load());
            Log("  Shots: %d", g_ShotsTaken.load());
            Log("  Last release: %.4f", g_LastReleaseVal.load());
            Log("  Lag switch: %s", g_LagActive.load() ? "ON" : "OFF");
            Log("  XInput hook: %s", g_XInputHooked ? "IAT" : (g_XInputEnable ? "XInputEnable" : "NONE"));
            uintptr_t ma = g_MeterAddr.load();
            if (ma) {
                float v;
                if (SafeReadFloat(ma, &v)) Log("  Meter current value: %.4f", v);
            }
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
        Log("autogreen.dll v4 loaded - pid %u", GetCurrentProcessId());
        g_Thread = CreateThread(NULL, 0,
            [](LPVOID) -> DWORD { HotkeyThread(); return 0; },
            NULL, 0, NULL);
        Log("Ready! Press F8 to enable auto-green, then F5 during a shot to find the meter.");
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

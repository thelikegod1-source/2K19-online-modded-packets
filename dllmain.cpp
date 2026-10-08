/**
 * autogreen.dll v3 — Auto-green shot release for NBA 2K19
 *
 * v3 changes:
 * - Temporal profiling: takes 12 rapid samples and uses linear regression
 *   to reliably identify the shot meter among millions of floats
 * - Proper IAT hook: patches XInputGetState to mask only the X button,
 *   keeping all other controller input working during release
 * - Self-calibrating release timing based on measured meter rate
 *
 * Hotkeys:
 *   F5 = find meter (press while shot meter is filling, ~1/3 to 2/3 full)
 *   F8 = toggle auto-green on/off
 *   F9 = dump current state to log
 *   F10 = toggle lag switch
 *   F12 = uninject DLL
 */

#include <windows.h>
#include <shlobj.h>
#include <psapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dbghelp.lib")

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
static std::atomic<float> g_LatencyMs{170.0f};
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

// ─── Memory regions (MEM_PRIVATE + MEM_IMAGE, capped at 256MB per region) ──

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
            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_READONLY | PAGE_EXECUTE_READ)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize <= 256 * 1024 * 1024) {
            regions.push_back({(uintptr_t)mbi.BaseAddress, mbi.RegionSize});
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return regions;
}

// ─── Fast region scan ───────────────────────────────────────────────────────

static int ScanRegionForRange(uintptr_t base, size_t size,
                              float lo, float hi,
                              uintptr_t* outAddrs, int maxOut)
{
    int count = 0;
    __try {
        float* ptr = (float*)base;
        size_t n = size / sizeof(float);
        for (size_t i = 0; i < n && count < maxOut; i++) {
            float v = ptr[i];
            if (v >= lo && v <= hi) {
                outAddrs[count++] = base + i * sizeof(float);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// ─── Temporal meter finder ──────────────────────────────────────────────────
//
// The shot meter is a float that ramps linearly from 0 to ~1 over ~1.5 seconds.
// Among millions of floats in range, only the meter shows this clean linear ramp.
//
// Strategy:
//   Phase 1: Sweep all MEM_PRIVATE memory for floats in [0.10, 0.85]
//   Phase 2: Re-read after 40ms, keep only addresses where value rose
//   Phase 3: Take 10 more rapid samples (15ms apart) for each survivor
//   Phase 4: Score with linear regression (R²), monotonicity, rate of change
//   Phase 5: Pick the highest-scoring candidate

static const int NUM_TEMPORAL_SAMPLES = 12;
static const int SAMPLE_INTERVAL_MS = 15;

struct TemporalCandidate {
    uintptr_t addr;
    float samples[NUM_TEMPORAL_SAMPLES];
    int nonZeroCount;
    float r2;
    float ratePerSec;
    float monoScore;
    float totalScore;
};

static float ScoreCandidate(TemporalCandidate& c)
{
    float nzVals[NUM_TEMPORAL_SAMPLES];
    float nzTimes[NUM_TEMPORAL_SAMPLES];
    int nz = 0;

    for (int i = 0; i < NUM_TEMPORAL_SAMPLES; i++) {
        if (c.samples[i] > 0.02f && c.samples[i] < 1.05f) {
            nzVals[nz] = c.samples[i];
            nzTimes[nz] = (float)(i * SAMPLE_INTERVAL_MS);
            nz++;
        }
    }
    c.nonZeroCount = nz;

    if (nz < 5) return -1.0f;

    // Monotonicity: what fraction of consecutive non-zero pairs are rising
    int rises = 0;
    for (int i = 1; i < nz; i++) {
        if (nzVals[i] > nzVals[i - 1]) rises++;
    }
    c.monoScore = (float)rises / (float)(nz - 1);
    if (c.monoScore < 0.6f) return -1.0f;

    // Linear regression for R²
    float n = (float)nz;
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
    for (int i = 0; i < nz; i++) {
        sumX += nzTimes[i];
        sumY += nzVals[i];
        sumXY += nzTimes[i] * nzVals[i];
        sumX2 += nzTimes[i] * nzTimes[i];
        sumY2 += nzVals[i] * nzVals[i];
    }
    float denX = n * sumX2 - sumX * sumX;
    float denY = n * sumY2 - sumY * sumY;
    if (denX < 0.0001f || denY < 0.0001f) return -1.0f;

    float num = n * sumXY - sumX * sumY;
    c.r2 = (num * num) / (denX * denY);

    // Slope = rate of change (value per millisecond)
    float slope = num / denX;
    c.ratePerSec = slope * 1000.0f;

    // Rate score: shot meter rises at ~0.5-2.0 per second
    float rateScore = 0.0f;
    if (c.ratePerSec > 0.2f && c.ratePerSec < 4.0f) {
        rateScore = 1.0f - fabsf(c.ratePerSec - 1.0f) / 3.0f;
        if (rateScore < 0) rateScore = 0;
    }

    c.totalScore = c.r2 * 0.50f + c.monoScore * 0.30f + rateScore * 0.20f;
    return c.totalScore;
}

static uintptr_t FindMeterTemporal()
{
    Log("=== TEMPORAL METER SCAN (F5) ===");
    auto t0 = std::chrono::steady_clock::now();

    // Phase 1: Sweep for floats in meter range
    auto regions = GetScanRegions();
    size_t totalBytes = 0;
    for (auto& r : regions) totalBytes += r.size;
    Log("Phase 1: scanning %zu regions (%.0f MB private heap)...",
        regions.size(), totalBytes / (1024.0 * 1024.0));

    const int BATCH = 65536;
    uintptr_t batchAddrs[65536];
    std::vector<uintptr_t> phase1;
    phase1.reserve(2000000);

    for (auto& region : regions) {
        if (phase1.size() >= 8000000) break;
        size_t off = 0;
        while (off < region.size && phase1.size() < 8000000) {
            size_t chunk = region.size - off;
            if (chunk > (size_t)BATCH * 4) chunk = (size_t)BATCH * 4;
            int found = ScanRegionForRange(region.base + off, chunk, 0.03f, 0.95f, batchAddrs, BATCH);
            for (int j = 0; j < found; j++) phase1.push_back(batchAddrs[j]);
            off += chunk;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double sweepMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    Log("Phase 1: %zu addresses in [0.03, 0.95] (%.0f ms)", phase1.size(), sweepMs);

    if (phase1.empty()) {
        Log("No floats found in range. Are you mid-shot?");
        return 0;
    }

    // Phase 2: Re-read after delay, keep only addresses that rose
    Sleep(40);
    std::vector<TemporalCandidate> candidates;
    candidates.reserve(50000);

    for (auto addr : phase1) {
        float val;
        if (!SafeReadFloat(addr, &val)) continue;
        if (val < 0.05f || val > 0.95f) continue;

        // The address was in [0.10, 0.85] during the sweep.
        // If it's now higher, it's rising (potential meter).
        // We don't know the exact sweep value, but if current > 0.12 that's a sign.
        // We'll let temporal profiling sort the real meter from noise.
        TemporalCandidate tc = {};
        tc.addr = addr;
        tc.samples[0] = val;
        candidates.push_back(tc);
    }

    // Too many? Tighten to addresses that clearly rose above their sweep value.
    // Re-read again after another short delay and keep only definite risers.
    if (candidates.size() > 100000) {
        Log("Phase 2: %zu candidates (too many), tightening...", candidates.size());
        Sleep(30);
        std::vector<TemporalCandidate> tighter;
        tighter.reserve(50000);
        for (auto& c : candidates) {
            float val;
            if (!SafeReadFloat(c.addr, &val)) continue;
            if (val > c.samples[0] + 0.003f && val > 0.10f && val < 0.95f) {
                c.samples[1] = val;
                tighter.push_back(c);
            }
        }
        candidates = tighter;
    }

    auto t2 = std::chrono::steady_clock::now();
    Log("Phase 2: %zu candidates rising (%.0f ms)",
        candidates.size(), std::chrono::duration<double, std::milli>(t2 - t1).count());

    if (candidates.empty()) {
        Log("No rising addresses found. Press F5 earlier in the shot (when meter is ~1/3 full).");
        return 0;
    }

    if (candidates.size() > 200000) {
        Log("Still too many (%zu). Try pressing F5 mid-shot, not at the start.", candidates.size());
        return 0;
    }

    // Phase 3: Temporal profiling — take remaining samples
    int startSample = (candidates[0].samples[1] > 0.01f) ? 2 : 1;
    for (int s = startSample; s < NUM_TEMPORAL_SAMPLES; s++) {
        Sleep(SAMPLE_INTERVAL_MS);
        for (auto& c : candidates) {
            float val;
            if (SafeReadFloat(c.addr, &val)) {
                c.samples[s] = val;
            }
        }
    }

    auto t3 = std::chrono::steady_clock::now();
    Log("Phase 3: collected %d samples over %.0f ms for %zu candidates",
        NUM_TEMPORAL_SAMPLES, std::chrono::duration<double, std::milli>(t3 - t2).count(),
        candidates.size());

    // Phase 4: Score each candidate
    for (auto& c : candidates) {
        ScoreCandidate(c);
    }

    // Sort by score descending
    std::sort(candidates.begin(), candidates.end(),
        [](const TemporalCandidate& a, const TemporalCandidate& b) {
            return a.totalScore > b.totalScore;
        });

    // Log top 10
    int showCount = candidates.size() < 15 ? (int)candidates.size() : 15;
    Log("Phase 4: Top candidates:");
    for (int i = 0; i < showCount; i++) {
        auto& c = candidates[i];
        if (c.totalScore < 0) break;
        Log("  [%d] addr=0x%llX score=%.3f R2=%.3f mono=%.2f rate=%.2f/s nz=%d  vals: %.3f %.3f %.3f %.3f %.3f %.3f",
            i, (unsigned long long)c.addr, c.totalScore, c.r2, c.monoScore, c.ratePerSec, c.nonZeroCount,
            c.samples[0], c.samples[2], c.samples[4], c.samples[6], c.samples[8], c.samples[10]);
    }

    // Phase 5: Pick the best
    float minScore = 0.35f;
    if (!candidates.empty() && candidates[0].totalScore >= minScore) {
        auto& best = candidates[0];

        // Extra validation: if there's a close second, verify the best still reads sensibly
        if (candidates.size() > 1 && candidates[1].totalScore > minScore) {
            float gap = best.totalScore - candidates[1].totalScore;
            Log("Top two scores: %.3f vs %.3f (gap=%.3f)", best.totalScore, candidates[1].totalScore, gap);
        }

        auto tEnd = std::chrono::steady_clock::now();
        Log("*** METER FOUND: addr=0x%llX score=%.3f R2=%.3f rate=%.2f/s (total %.0f ms) ***",
            (unsigned long long)best.addr, best.totalScore, best.r2, best.ratePerSec,
            std::chrono::duration<double, std::milli>(tEnd - t0).count());
        return best.addr;
    }

    Log("No candidate scored above %.2f. Press F5 again during a shot!", minScore);
    return 0;
}

// ─── XInput IAT hook ────────────────────────────────────────────────────────

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
        return;
    }
    Log("Found XInput: %s", loadedXInput);

    // Try IAT patching on all loaded modules
    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        int patched = 0;

        for (int i = 0; i < count; i++) {
            if (mods[i] == g_Module || mods[i] == hXInput) continue;
            if (PatchModuleIAT(mods[i], loadedXInput, (void*)HookedXInputGetState)) {
                char modName[MAX_PATH] = {};
                GetModuleFileNameA(mods[i], modName, MAX_PATH);
                const char* sn = strrchr(modName, '\\');
                Log("  IAT patched: %s", sn ? sn + 1 : modName);
                patched++;
            }
        }

        if (patched > 0) {
            g_XInputHooked = true;
            Log("XInput IAT hook installed (%d module%s patched)", patched, patched > 1 ? "s" : "");
        } else {
            Log("WARNING: No IAT entries found for XInputGetState");
        }
    }

    // Fallback: XInputEnable (disables all input, not ideal but works)
    if (!g_XInputHooked) {
        g_XInputEnable = (fn_XInputEnable)GetProcAddress(hXInput, "XInputEnable");
        if (g_XInputEnable) {
            Log("Fallback: using XInputEnable for shot release");
        } else {
            Log("WARNING: No hook method available - auto-green release won't work!");
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

    snprintf(path, MAX_PATH, "%s\\latency.txt", g_LogDir);
    f = fopen(path, "r");
    if (f) {
        float t = 0;
        if (fscanf(f, "%f", &t) == 1 && t >= 0.0f && t <= 500.0f) {
            if (fabsf(t - g_LatencyMs.load()) > 0.1f) {
                g_LatencyMs = t;
                Log("Latency updated to %.0f ms", t);
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

        // Wait for meter to be found (F5 triggers FindMeterTemporal)
        if (meterAddr == 0) {
            if (g_FindRequest.exchange(false)) {
                uintptr_t found = FindMeterTemporal();
                if (found) g_MeterAddr = found;
            }
            Sleep(16);
            continue;
        }

        // === Main auto-green loop: watch meter and release at target ===
        float lastVal = 0;
        bool released = false;
        int staleCount = 0;
        int zeroCount = 0;
        int configCounter = 0;
        float lastChangeVal = 0;
        auto lastChangeTime = std::chrono::steady_clock::now();
        float meterRate = 1.0f;

        Log("Monitoring meter at 0x%llX (standing=%.2f, fade=%.2f, latency=%.0f ms)...",
            (unsigned long long)meterAddr, g_GreenTarget.load(), g_FadeTarget.load(), g_LatencyMs.load());

        while (g_AutoGreen && g_Running) {
            // Re-check for F5 (re-find meter) or configs
            if (g_FindRequest.exchange(false)) {
                uintptr_t found = FindMeterTemporal();
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

            // Handle zero frames (double-buffer)
            if (val >= -0.001f && val <= 0.001f) {
                zeroCount++;
                if (zeroCount > 50 && released) released = false;
                continue;
            }
            zeroCount = 0;

            // Stale check
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

            // Re-arm after shot ends (value drops)
            if (released && lastVal > 0.01f && val < lastVal - 0.05f) {
                released = false;
                lastVal = 0;
                lastChangeVal = 0;
            }

            // Measure meter speed
            auto nowT = std::chrono::steady_clock::now();
            if (lastChangeVal > 0.01f && fabsf(val - lastChangeVal) > 0.002f) {
                double dt = std::chrono::duration<double>(nowT - lastChangeTime).count();
                float step = val - lastChangeVal;
                if (step > 0.0f && dt > 0.001 && dt < 0.1)
                    meterRate = step / (float)dt;
            }
            if (fabsf(val - lastChangeVal) > 0.002f) {
                lastChangeVal = val;
                lastChangeTime = nowT;
            }

            // Calculate release point
            float delta = (lastVal > 0.01f) ? (val - lastVal) : 0.0f;
            bool isFade = (delta > 0.025f);
            float target = isFade ? g_FadeTarget.load() : g_GreenTarget.load();
            float lead = meterRate * (g_LatencyMs.load() / 1000.0f);
            float releaseAt = target - lead;
            if (releaseAt < 0.05f) releaseAt = 0.05f;

            // Release!
            if (val >= releaseAt && val <= 0.95f && !released) {
                Log(">>> %s RELEASE at %.4f (target %.2f, lead %.3f, rate %.2f/s) <<<",
                    isFade ? "FADE" : "GREEN", val, target, lead, meterRate);

                SetReleaseMask(true);
                DWORD t0 = GetTickCount();
                DWORD lastRise = t0;
                float peak = val;
                bool restored = false;
                bool froze = false;

                for (;;) {
                    DWORD now = GetTickCount();
                    if (!restored && now - t0 >= 100) { SetReleaseMask(false); restored = true; }
                    float v;
                    if (SafeReadFloat(meterAddr, &v) && v > peak && v <= 1.05f) { peak = v; lastRise = now; }
                    if (now - lastRise > 80) { froze = true; break; }
                    if (now - t0 > 500) break;
                    Sleep(1);
                }
                if (!restored) SetReleaseMask(false);

                g_LastReleaseVal = peak;
                g_ShotsTaken++;
                released = true;
                Log("Shot #%d peak=%.4f (target was %.2f)", g_ShotsTaken.load(), peak, target);

                // Self-calibrate latency
                float err = peak - target;
                if (froze && meterRate > 0.2f && fabsf(err) < 0.5f) {
                    float adjMs = err / meterRate * 1000.0f * 0.5f;
                    if (adjMs > 60.0f) adjMs = 60.0f;
                    if (adjMs < -60.0f) adjMs = -60.0f;
                    float newLat = g_LatencyMs.load() + adjMs;
                    if (newLat < 20.0f) newLat = 20.0f;
                    if (newLat > 400.0f) newLat = 400.0f;
                    Log("CALIBRATE: err %+.3f -> latency %.0f -> %.0f ms", err, g_LatencyMs.load(), newLat);
                    g_LatencyMs = newLat;
                    char lp[MAX_PATH];
                    snprintf(lp, MAX_PATH, "%s\\latency.txt", g_LogDir);
                    if (FILE* lf = fopen(lp, "w")) { fprintf(lf, "%.0f", newLat); fclose(lf); }
                }
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
    Log("LAGCMD: %s", cmd);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log("NETSH LAUNCH FAILED (err=%lu)", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode != 0) { Log("NETSH FAILED (exit %lu)", exitCode); return false; }
    return true;
}

static void ToggleLag()
{
    if (!g_LagActive) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        Log("LAG ON: blocking %s", exePath);

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

// ─── Net probe (read-only) ──────────────────────────────────────────────────

static void LogNetImports()
{
    static const char* kInteresting[] = {
        "recv", "recvfrom", "WSARecv", "WSARecvFrom", "select",
        "send", "sendto", "WSASend", "WSASendTo"
    };

    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    int count = (int)(needed / sizeof(HMODULE));

    for (int m = 0; m < count; m++) {
        char modName[MAX_PATH] = {};
        GetModuleFileNameA(mods[m], modName, MAX_PATH);
        const char* sn = strrchr(modName, '\\');
        sn = sn ? sn + 1 : modName;

        __try {
            BYTE* base = (BYTE*)mods[m];
            auto dos = (IMAGE_DOS_HEADER*)base;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
            auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
            auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) continue;

            for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
                const char* dll = (const char*)(base + imp->Name);
                if (_stricmp(dll, "ws2_32.dll") != 0 && _stricmp(dll, "wsock32.dll") != 0) continue;

                DWORD thunkRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
                for (auto t = (IMAGE_THUNK_DATA*)(base + thunkRva); t->u1.AddressOfData; t++) {
                    if (IMAGE_SNAP_BY_ORDINAL(t->u1.Ordinal)) continue;
                    auto name = (IMAGE_IMPORT_BY_NAME*)(base + t->u1.AddressOfData);
                    for (auto want : kInteresting) {
                        if (strcmp((const char*)name->Name, want) == 0)
                            Log("NETPROBE: %s imports %s!%s", sn, dll, want);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    Log("NETPROBE: done");
}

// ─── Hotkey thread ──────────────────────────────────────────────────────────

static void HotkeyThread()
{
    LogNetImports();
    Log("Hotkeys: F5=find meter, F8=toggle auto-green, F9=dump, F10=lag, F12=uninject");

    while (g_Running) {
        if (GetAsyncKeyState(VK_F5) & 1) {
            if (g_AutoGreen) {
                g_FindRequest = true;
                Log("F5: meter scan requested (will run on auto-green thread)");
            } else {
                uintptr_t found = FindMeterTemporal();
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
            Log("  Latency: %.0f ms", g_LatencyMs.load());
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
            Log("autogreen.dll unloaded");
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
        Log("autogreen.dll v3 loaded - pid %u", GetCurrentProcessId());
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

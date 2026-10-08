#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdio>
#include <cmath>
#include <psapi.h>
#include <tlhelp32.h>
#include "WebView2.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

using namespace Microsoft::WRL;

static HWND g_hWnd = nullptr;
static ComPtr<ICoreWebView2> g_webView;
static ComPtr<ICoreWebView2Controller> g_controller;
static std::wstring g_baseDir;
static std::string g_logPath;
static long g_logOffset = 0;
static bool g_injected = false;

// ── Paths ──
static std::wstring GetBaseDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring s(buf);
    return s.substr(0, s.find_last_of(L"\\/"));
}

static std::string WtoA(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}

static std::wstring AtoW(const std::string& a) {
    if (a.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, NULL, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, &w[0], n);
    return w;
}

// ── Inject ──
static DWORD FindProcess(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof(pe) };
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                CloseHandle(snap);
                return pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
}

static std::string DoInject() {
    std::string dllPath = WtoA(g_baseDir) + "\\autogreen.dll";
    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return "{\"ok\":false,\"msg\":\"DLL not found at: " + dllPath + "\"}";

    DWORD pid = FindProcess("NBA2K19.exe");
    if (!pid) return "{\"ok\":false,\"msg\":\"NBA2K19.exe not running — launch the game first\"}";

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        DWORD err = GetLastError();
        char buf[128];
        snprintf(buf, 128, "{\"ok\":false,\"msg\":\"Cannot open process (err %lu) — run as Administrator\"}", err);
        return buf;
    }

    void* mem = VirtualAllocEx(hProc, NULL, dllPath.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { CloseHandle(hProc); return "{\"ok\":false,\"msg\":\"VirtualAllocEx failed\"}"; }
    if (!WriteProcessMemory(hProc, mem, dllPath.c_str(), dllPath.size() + 1, NULL)) {
        VirtualFreeEx(hProc, mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return "{\"ok\":false,\"msg\":\"WriteProcessMemory failed\"}";
    }

    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        mem, 0, NULL);
    if (!hThread) {
        DWORD err = GetLastError();
        VirtualFreeEx(hProc, mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        char buf[128];
        snprintf(buf, 128, "{\"ok\":false,\"msg\":\"CreateRemoteThread failed (err %lu)\"}", err);
        return buf;
    }

    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, mem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (exitCode == 0)
        return "{\"ok\":false,\"msg\":\"LoadLibraryA returned 0 — DLL failed to load\"}";

    g_injected = true;
    return "{\"ok\":true}";
}

// ── Send hotkey ──
static void SendHotkey(int vk) {
    keybd_event(vk, 0, 0, 0);
    Sleep(50);
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

// ── Log polling ──
static std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 20);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); out += b; }
            else out += c;
        }
    }
    return out;
}

static std::string PollLog() {
    std::string logText;
    std::vector<std::string> events;

    FILE* f = fopen(g_logPath.c_str(), "r");
    if (f) {
        // DLL truncates the log on (re)inject; restart from the top if it shrank
        fseek(f, 0, SEEK_END);
        if (ftell(f) < g_logOffset) g_logOffset = 0;
        fseek(f, g_logOffset, SEEK_SET);
        char buf[4096];
        std::string newData;
        while (fgets(buf, sizeof(buf), f))
            newData += buf;
        if (!newData.empty()) {
            g_logOffset = ftell(f);
            logText = newData;
            if (newData.find("METER CONFIRMED") != std::string::npos ||
                newData.find("METER FOUND") != std::string::npos ||
                newData.find("METER AUTO-FOUND") != std::string::npos)
                events.push_back("meter_found");
            if (newData.find("Auto-green ENABLED") != std::string::npos ||
                newData.find("AUTO-GREEN ACTIVE") != std::string::npos)
                events.push_back("ag_on");
            if (newData.find("Auto-green stopped") != std::string::npos ||
                newData.find("Auto-green DISABLED") != std::string::npos)
                events.push_back("ag_off");
            if (newData.find("autogreen.dll loaded") != std::string::npos) {
                events.push_back("injected");
                g_injected = true;
            }
            if (newData.find("unloaded (uninject)") != std::string::npos) {
                events.push_back("uninjected");
                g_injected = false;
            }
            if (newData.find("LAG ON") != std::string::npos)
                events.push_back("lag_on");
            if (newData.find("LAG OFF") != std::string::npos)
                events.push_back("lag_off");

            // find shot info
            auto pos = newData.find("Shot #");
            if (pos != std::string::npos) {
                auto rpos = newData.find("released at ", pos);
                if (rpos != std::string::npos) {
                    int shotNum = 0;
                    sscanf(newData.c_str() + pos + 6, "%d", &shotNum);
                    float relVal = 0;
                    sscanf(newData.c_str() + rpos + 12, "%f", &relVal);
                    char sb[64];
                    snprintf(sb, 64, "shot:%d:%.4f", shotNum, relVal);
                    events.push_back(sb);
                }
            }
        }
        fclose(f);
    }

    std::string json = "{\"log\":\"" + EscapeJson(logText) + "\",\"events\":[";
    for (size_t i = 0; i < events.size(); i++) {
        if (i) json += ",";
        json += "\"" + events[i] + "\"";
    }
    json += "]}";
    return json;
}

// ── HTML ──
static const char* HTML_PAGE = R"RAWHTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{
    font-family:'Segoe UI',sans-serif;
    background:#0c1018;
    color:#d0d8e0;
    height:100vh;overflow:hidden;
    user-select:none;-webkit-user-select:none;
}
.app{display:flex;flex-direction:column;height:100vh}

.header{
    display:flex;align-items:center;justify-content:space-between;
    padding:14px 20px 10px;
    background:linear-gradient(180deg,#111822 0%,#0c1018 100%);
    border-bottom:1px solid rgba(0,230,180,0.06);
}
.logo{display:flex;align-items:center;gap:10px}
.logo-icon{
    width:32px;height:32px;border-radius:8px;
    background:linear-gradient(135deg,#00e6b4,#00b4d8);
    display:flex;align-items:center;justify-content:center;
    font-size:16px;font-weight:900;color:#0c1018;
}
.logo h1{font-size:16px;font-weight:700;letter-spacing:1.5px;color:#e8f0f8}
.logo .ver{font-size:10px;color:#4a6a7a;margin-left:4px}
.badge{
    font-size:10px;font-weight:700;letter-spacing:1px;
    padding:4px 10px;border-radius:12px;
    border:1px solid rgba(0,230,180,0.15);
    background:rgba(0,230,180,0.06);
    color:#00e6b4;
}
.badge.off{border-color:rgba(255,60,60,0.2);background:rgba(255,60,60,0.06);color:#ff4444}

.tabs{
    display:flex;padding:0 16px;gap:0;
    background:#0e1420;
    border-bottom:1px solid rgba(255,255,255,0.04);
}
.tab{
    padding:10px 20px;font-size:11px;font-weight:600;letter-spacing:0.6px;
    color:#4a6a7a;cursor:pointer;border:none;background:none;
    border-bottom:2px solid transparent;transition:all 0.2s;
}
.tab:hover{color:#8ab4c8}
.tab.active{color:#00e6b4;border-bottom-color:#00e6b4}

.content{flex:1;overflow-y:auto;padding:14px 16px}
.page{display:none}.page.active{display:block}

.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.card{
    background:linear-gradient(160deg,rgba(18,26,38,0.95),rgba(14,20,30,0.98));
    border:1px solid rgba(255,255,255,0.04);
    border-radius:10px;padding:14px 16px;
    margin-bottom:10px;
}
.card.span2{grid-column:span 2}
.card-title{
    font-size:10px;font-weight:700;letter-spacing:2px;
    color:#00e6b4;margin-bottom:10px;text-transform:uppercase;
}
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0}
.row-label{font-size:12px;color:#4a6a7a}
.row-value{font-size:12px;font-weight:600;font-family:'Consolas',monospace}
.v-off{color:#ff4444}.v-on{color:#00e6b4}.v-dim{color:#4a6a7a}
.v-cyan{color:#00b4d8}.v-gold{color:#f0c040}.v-white{color:#d0d8e0}

.btn-row{display:flex;gap:6px;margin-top:8px}
.btn{
    flex:1;padding:8px 0;border:none;border-radius:6px;
    font-size:11px;font-weight:700;letter-spacing:0.8px;
    cursor:pointer;transition:all 0.12s;color:white;text-transform:uppercase;
}
.btn:hover{filter:brightness(1.2);transform:translateY(-1px)}
.btn:active{transform:translateY(0)}
.btn-pri{background:linear-gradient(135deg,#00b4d8,#00e6b4)}
.btn-red{background:linear-gradient(135deg,#c02020,#e03030)}
.btn-grn{background:linear-gradient(135deg,#1a8a40,#20c050)}
.btn-org{background:linear-gradient(135deg,#c06000,#e08020)}
.btn-dark{background:rgba(255,255,255,0.06);border:1px solid rgba(255,255,255,0.08)}

.slider-row{display:flex;align-items:center;gap:10px;margin:6px 0}
.slider-row input[type=range]{
    flex:1;-webkit-appearance:none;height:3px;border-radius:2px;
    background:linear-gradient(90deg,#00b4d8,#00e6b4);outline:none;
}
.slider-row input[type=range]::-webkit-slider-thumb{
    -webkit-appearance:none;width:14px;height:14px;border-radius:50%;
    background:#fff;box-shadow:0 0 6px rgba(0,230,180,0.4);cursor:pointer;
}
.slider-val{
    font-size:20px;font-weight:800;font-family:'Consolas',monospace;
    color:#00e6b4;min-width:55px;text-align:right;
}

.hotkey-grid{display:grid;grid-template-columns:1fr 1fr;gap:2px 16px}
.hotkey{font-size:10px;color:#4a6a7a;padding:2px 0}
.hotkey kbd{
    display:inline-block;padding:1px 5px;border-radius:3px;
    background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.08);
    font-family:'Consolas',monospace;font-size:9px;color:#00e6b4;margin-right:3px;
}

.toggle{display:flex;align-items:center;gap:10px;padding:6px 0}
.toggle-track{
    width:40px;height:22px;border-radius:11px;
    background:rgba(255,255,255,0.06);border:1px solid rgba(255,255,255,0.08);
    position:relative;cursor:pointer;transition:all 0.3s;
}
.toggle-track.on{background:rgba(255,60,60,0.2);border-color:rgba(255,60,60,0.4)}
.toggle-knob{
    width:16px;height:16px;border-radius:50%;background:#4a6a7a;
    position:absolute;top:2px;left:2px;transition:all 0.3s;
}
.toggle-track.on .toggle-knob{left:20px;background:#ff4444;box-shadow:0 0 6px #ff4444}

.log-box{
    background:#080c12;border:1px solid rgba(255,255,255,0.04);
    border-radius:8px;padding:10px;
    height:calc(100vh - 140px);overflow-y:auto;
    font-family:'Consolas',monospace;font-size:10px;line-height:1.5;color:#4a6a7a;
}
.log-box::-webkit-scrollbar{width:5px}
.log-box::-webkit-scrollbar-track{background:transparent}
.log-box::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:3px}
.log-line-green{color:#00e6b4}.log-line-red{color:#ff4444}.log-line-orange{color:#e08020}

.ping-bar{height:2px;border-radius:1px;background:rgba(255,255,255,0.04);margin-top:4px;overflow:hidden}
.ping-bar-fill{height:100%;border-radius:1px;transition:width 0.5s;background:#00e6b4}

.select{
    background:rgba(255,255,255,0.06);border:1px solid rgba(255,255,255,0.08);
    border-radius:4px;color:#d0d8e0;font-size:11px;padding:4px 8px;
    font-family:'Consolas',monospace;width:100%;
}
</style>
</head>
<body>
<div class="app">
    <div class="header">
        <div class="logo">
            <div class="logo-icon">S</div>
            <div><h1>2K19 SECRET</h1></div>
            <span class="ver">v2.0</span>
        </div>
        <div class="badge off" id="badge">NOT INJECTED</div>
    </div>
    <div class="tabs">
        <button class="tab active" onclick="switchTab('general')">General</button>
        <button class="tab" onclick="switchTab('network')">Network</button>
        <button class="tab" onclick="switchTab('secret')">Secret v2.0</button>
        <button class="tab" onclick="switchTab('debug')">Debug</button>
    </div>
    <div class="content">

        <!-- GENERAL -->
        <div class="page active" id="page-general">
            <div class="grid">
                <div class="card">
                    <div class="card-title">Status</div>
                    <div class="row"><span class="row-label">Injection</span><span class="row-value v-off" id="gSt">Not Injected</span></div>
                    <div class="row"><span class="row-label">Meter</span><span class="row-value v-dim" id="gMe">Not Found</span></div>
                    <div class="row"><span class="row-label">Auto-Green</span><span class="row-value v-off" id="gAG">Off</span></div>
                </div>
                <div class="card">
                    <div class="card-title">Stats</div>
                    <div class="row"><span class="row-label">Shots</span><span class="row-value v-dim" id="gShots">0</span></div>
                    <div class="row"><span class="row-label">Last Release</span><span class="row-value v-dim" id="gLast">--</span></div>
                    <div class="row"><span class="row-label">Ping</span><span class="row-value v-dim" id="gPing">--</span></div>
                </div>
                <div class="card span2">
                    <div class="card-title">Controls</div>
                    <div class="btn-row">
                        <button class="btn btn-pri" onclick="doAction('inject')">Inject</button>
                        <button class="btn btn-red" onclick="doAction('f12')">Uninject</button>
                    </div>
                </div>
                <div class="card span2">
                    <div class="card-title">Hotkeys</div>
                    <div class="hotkey-grid">
                        <div class="hotkey"><kbd>F5</kbd> Scan Shot</div>
                        <div class="hotkey"><kbd>F6</kbd> Reset Baseline</div>
                        <div class="hotkey"><kbd>F8</kbd> Auto-Green</div>
                        <div class="hotkey"><kbd>F10</kbd> Lag Switch</div>
                        <div class="hotkey"><kbd>F9</kbd> Dump State</div>
                        <div class="hotkey"><kbd>F12</kbd> Uninject</div>
                    </div>
                </div>
            </div>
        </div>

        <!-- NETWORK -->
        <div class="page" id="page-network">
            <div class="grid">
                <div class="card">
                    <div class="card-title">Connection</div>
                    <div class="row"><span class="row-label">Server</span><span class="row-value v-dim" id="nIP">Searching...</span></div>
                    <div class="row"><span class="row-label">Ping</span><span class="row-value v-dim" id="nPing">--</span></div>
                    <div class="ping-bar"><div class="ping-bar-fill" id="pingBar" style="width:0%"></div></div>
                    <div class="row" style="margin-top:6px"><span class="row-label">Jitter</span><span class="row-value v-dim" id="nJit">--</span></div>
                </div>
                <div class="card">
                    <div class="card-title">Stats</div>
                    <div class="row"><span class="row-label">Quality</span><span class="row-value v-dim" id="nQual">--</span></div>
                    <div class="row"><span class="row-label">Playing</span><span class="row-value v-dim" id="nPlay">--</span></div>
                </div>
                <div class="card">
                    <div class="card-title">Sync</div>
                    <div class="row"><span class="row-label">Mode</span><span class="row-value v-white">Automatic</span></div>
                    <div class="row"><span class="row-label">Status</span><span class="row-value v-dim" id="nSync">---</span></div>
                    <div class="row"><span class="row-label">Delay</span><span class="row-value v-dim" id="nDelay">0 ms</span></div>
                </div>
                <div class="card">
                    <div class="card-title">Lag Switch</div>
                    <div class="toggle">
                        <div class="toggle-track" id="lagTog" onclick="doAction('f10')"><div class="toggle-knob"></div></div>
                        <span class="row-value v-dim" id="lagSt">Off</span>
                    </div>
                    <div class="btn-row"><button class="btn btn-org" onclick="doAction('f10')" style="font-size:10px">Toggle (F10)</button></div>
                </div>
            </div>
        </div>

        <!-- SECRET -->
        <div class="page" id="page-secret">
            <div class="card">
                <div class="card-title">Auto-Green</div>
                <div class="row"><span class="row-label">Enabled</span><span class="row-value v-off" id="sEn">Off</span></div>
                <div style="margin-top:10px"><span style="font-size:9px;font-weight:700;letter-spacing:1.5px;color:#4a6a7a">SHOT CUE (STANDING)</span></div>
                <div class="slider-row">
                    <input type="range" min="20" max="95" value="50" id="cueSlider" oninput="updateCue()">
                    <div class="slider-val" id="cueVal">0.50</div>
                </div>
                <div style="margin-top:6px"><span style="font-size:9px;font-weight:700;letter-spacing:1.5px;color:#4a6a7a">SHOT CUE (FADE)</span></div>
                <div class="slider-row">
                    <input type="range" min="20" max="95" value="40" id="fadeCueSlider" oninput="updateFadeCue()">
                    <div class="slider-val" id="fadeCueVal">0.40</div>
                </div>
                <div class="btn-row">
                    <button class="btn btn-dark" onclick="doAction('f5')">Scan (F5)</button>
                    <button class="btn btn-dark" onclick="doAction('f6')">Baseline (F6)</button>
                    <button class="btn btn-grn" onclick="doAction('f8')">Toggle (F8)</button>
                </div>
            </div>
            <div class="grid">
                <div class="card">
                    <div class="card-title">Shot Stats</div>
                    <div class="row"><span class="row-label">Last Release</span><span class="row-value v-cyan" id="sLast">--</span></div>
                    <div class="row"><span class="row-label">Total Shots</span><span class="row-value v-dim" id="sTotal">0</span></div>
                </div>
                <div class="card">
                    <div class="card-title">Settings</div>
                    <div class="row"><span class="row-label">Button</span><span class="row-value v-white">X / Square</span></div>
                    <div class="row"><span class="row-label">Tempo</span><span class="row-value v-white" id="sTempo">--</span></div>
                </div>
            </div>
        </div>

        <!-- DEBUG -->
        <div class="page" id="page-debug">
            <div class="log-box" id="logBox"></div>
        </div>
    </div>
</div>

<script>
function switchTab(name){
    const pages=['general','network','secret','debug'];
    document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',pages[i]===name));
    document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
    document.getElementById('page-'+name).classList.add('active');
}
function updateCue(){
    var v=(document.getElementById('cueSlider').value/100).toFixed(2);
    document.getElementById('cueVal').textContent=v;
    window.chrome.webview.postMessage(JSON.stringify({action:'cue',value:parseFloat(v)}));
}
function updateFadeCue(){
    var v=(document.getElementById('fadeCueSlider').value/100).toFixed(2);
    document.getElementById('fadeCueVal').textContent=v;
    window.chrome.webview.postMessage(JSON.stringify({action:'fadecue',value:parseFloat(v)}));
}
function doAction(a){
    window.chrome.webview.postMessage(JSON.stringify({action:a}));
}
function handleEvents(evts){
    for(const e of evts){
        if(e==='injected'){
            document.getElementById('badge').className='badge';
            document.getElementById('badge').textContent='INJECTED';
            document.getElementById('gSt').textContent='Injected';
            document.getElementById('gSt').className='row-value v-on';
            document.getElementById('nPlay').textContent='Yes';
            document.getElementById('nPlay').className='row-value v-on';
        }
        if(e==='uninjected'){
            document.getElementById('badge').className='badge off';
            document.getElementById('badge').textContent='NOT INJECTED';
            document.getElementById('gSt').textContent='Not Injected';
            document.getElementById('gSt').className='row-value v-off';
            document.getElementById('gMe').textContent='Not Found';
            document.getElementById('gMe').className='row-value v-dim';
            document.getElementById('gAG').textContent='Off';
            document.getElementById('gAG').className='row-value v-off';
            document.getElementById('sEn').textContent='Off';
            document.getElementById('sEn').className='row-value v-off';
        }
        if(e==='meter_found'){
            document.getElementById('gMe').textContent='Locked';
            document.getElementById('gMe').className='row-value v-on';
        }
        if(e==='ag_on'){
            document.getElementById('gAG').textContent='Active';
            document.getElementById('gAG').className='row-value v-on';
            document.getElementById('sEn').textContent='Active';
            document.getElementById('sEn').className='row-value v-on';
        }
        if(e==='ag_off'){
            document.getElementById('gAG').textContent='Off';
            document.getElementById('gAG').className='row-value v-off';
            document.getElementById('sEn').textContent='Off';
            document.getElementById('sEn').className='row-value v-off';
        }
        if(e==='lag_on'){
            document.getElementById('lagTog').classList.add('on');
            document.getElementById('lagSt').textContent='Active';
            document.getElementById('lagSt').className='row-value v-off';
        }
        if(e==='lag_off'){
            document.getElementById('lagTog').classList.remove('on');
            document.getElementById('lagSt').textContent='Off';
            document.getElementById('lagSt').className='row-value v-dim';
        }
        if(e.startsWith('shot:')){
            const[,num,val]=e.split(':');
            document.getElementById('gShots').textContent=num;
            document.getElementById('gShots').className='row-value v-white';
            document.getElementById('gLast').textContent=val;
            document.getElementById('gLast').className='row-value v-cyan';
            document.getElementById('sTotal').textContent=num;
            document.getElementById('sLast').textContent=val;
        }
    }
}
function handleNet(n){
    if(!n)return;
    if(n.ip){
        document.getElementById('nIP').textContent=n.ip;
        document.getElementById('nIP').className='row-value v-cyan';
    }
    if(n.ping!==undefined){
        const p=n.ping;
        document.getElementById('nPing').textContent=p+'ms';
        document.getElementById('gPing').textContent=p+'ms';
        const pc=p<60?'v-on':p<100?'v-gold':'v-off';
        document.getElementById('nPing').className='row-value '+pc;
        document.getElementById('gPing').className='row-value '+pc;
        const pct=Math.min(100,Math.max(5,(200-p)/200*100));
        const bar=document.getElementById('pingBar');
        bar.style.width=pct+'%';
        bar.style.background=p<60?'#00e6b4':p<100?'#f0c040':'#ff4444';
        document.getElementById('nJit').textContent=n.jitter+'ms';
        document.getElementById('nJit').className='row-value '+(n.jitter<10?'v-on':n.jitter<25?'v-gold':'v-off');
        const q=p<50?'Excellent':p<80?'Good':p<120?'Fair':'Poor';
        document.getElementById('nQual').textContent=q;
        document.getElementById('nQual').className='row-value '+(p<80?'v-on':p<120?'v-gold':'v-off');
        document.getElementById('nSync').textContent='Active';
        document.getElementById('nSync').className='row-value v-on';
        document.getElementById('nDelay').textContent=p+'ms';
    }
}
window.chrome.webview.addEventListener('message',e=>{
    try{
        const d=typeof e.data==='string'?JSON.parse(e.data):e.data;
        if(d.log){
            const box=document.getElementById('logBox');
            const lines=d.log.trim().split('\n');
            for(const line of lines){
                const el=document.createElement('div');
                el.textContent=line;
                if(line.includes('GREEN')||line.includes('METER'))el.className='log-line-green';
                else if(line.includes('FAIL')||line.includes('ERROR'))el.className='log-line-red';
                else if(line.includes('LAG'))el.className='log-line-orange';
                box.appendChild(el);
            }
            box.scrollTop=box.scrollHeight;
        }
        if(d.events)handleEvents(d.events);
        if(d.net)handleNet(d.net);
    }catch(ex){}
});
async function pollLoop(){
    while(true){
        window.chrome.webview.postMessage(JSON.stringify({action:'poll'}));
        await new Promise(r=>setTimeout(r,800));
    }
}
pollLoop();
</script>
</body>
</html>)RAWHTML";

// ── Window procedure ──
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_controller) {
            RECT r;
            GetClientRect(hWnd, &r);
            g_controller->put_Bounds(r);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── Network (background thread) ──
static std::string g_netJson = "{}";
static CRITICAL_SECTION g_netLock;
static float g_pingHistory[32];
static int g_pingCount = 0;

static std::string FindCourtIP() {
    DWORD size = 0;
    GetTcpTable2(NULL, &size, FALSE);
    if (size == 0) return "";

    std::vector<BYTE> buf(size);
    PMIB_TCPTABLE2 table = (PMIB_TCPTABLE2)buf.data();
    if (GetTcpTable2(table, &size, FALSE) != NO_ERROR) return "";

    std::string bestIP;
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        auto& row = table->table[i];
        if (row.dwState != MIB_TCP_STATE_ESTAB) continue;

        IN_ADDR addr;
        addr.S_un.S_addr = row.dwRemoteAddr;
        char ipStr[32];
        inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr));
        int port = ntohs((u_short)row.dwRemotePort);

        std::string ip(ipStr);
        if (ip.substr(0, 4) == "127.") continue;

        if (port == 20054 || ip.substr(0, 8) == "155.103.") return ip;
        if (port == 21000 && bestIP.empty()) bestIP = ip;
    }
    return bestIP;
}

static float TcpPing(const std::string& ip) {
    int ports[] = { 21000, 3478, 3479, 3480, 443, 80 };
    for (int port : ports) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        DWORD timeout = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        int ret = connect(s, (sockaddr*)&addr, sizeof(addr));
        QueryPerformanceCounter(&t1);
        closesocket(s);

        if (ret == 0) {
            return (float)(t1.QuadPart - t0.QuadPart) * 1000.0f / freq.QuadPart;
        }
    }
    return -1;
}

static DWORD WINAPI NetThread(LPVOID) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    while (true) {
        std::string ip = FindCourtIP();
        if (!ip.empty()) {
            float ms = TcpPing(ip);
            if (ms >= 0) {
                if (g_pingCount < 32) g_pingHistory[g_pingCount++] = ms;
                else { memmove(g_pingHistory, g_pingHistory + 1, 31 * sizeof(float)); g_pingHistory[31] = ms; }

                float avg = 0;
                for (int i = 0; i < g_pingCount; i++) avg += g_pingHistory[i];
                avg /= g_pingCount;
                float jitter = 0;
                if (g_pingCount > 1) {
                    for (int i = 1; i < g_pingCount; i++)
                        jitter += fabsf(g_pingHistory[i] - g_pingHistory[i - 1]);
                    jitter /= (g_pingCount - 1);
                }

                char buf[256];
                snprintf(buf, 256, "{\"ip\":\"%s\",\"ping\":%d,\"jitter\":%.1f}", ip.c_str(), (int)avg, jitter);
                EnterCriticalSection(&g_netLock);
                g_netJson = buf;
                LeaveCriticalSection(&g_netLock);
            }
        }
        Sleep(3000);
    }
    return 0;
}

// ── Entry ──
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_baseDir = GetBaseDir();

    // Log path
    char appdata[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata);
    CreateDirectoryA((std::string(appdata) + "\\NBA2K-AutoGreen").c_str(), NULL);
    g_logPath = std::string(appdata) + "\\NBA2K-AutoGreen\\autogreen.log";

    // Delete old log
    DeleteFileA(g_logPath.c_str());

    // WebView2 requires COM on the UI thread
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    InitializeCriticalSection(&g_netLock);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"2K19SecretWnd";
    wc.hbrBackground = CreateSolidBrush(RGB(12, 16, 24));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"2K19SecretWnd", L"2K19 Secret v2.0",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 520,
        NULL, NULL, hInstance, NULL);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    // Start network thread
    CreateThread(NULL, 0, NetThread, NULL, 0, NULL);

    // Create WebView2
    auto envCallback = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr)) return hr;
            env->CreateCoreWebView2Controller(g_hWnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                        if (FAILED(hr)) return hr;
                        g_controller = ctrl;
                        g_controller->get_CoreWebView2(&g_webView);

                        RECT r;
                        GetClientRect(g_hWnd, &r);
                        g_controller->put_Bounds(r);

                        // Disable context menu and dev tools
                        ComPtr<ICoreWebView2Settings> settings;
                        g_webView->get_Settings(&settings);
                        settings->put_AreDevToolsEnabled(FALSE);
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);

                        // Handle messages from JS
                        g_webView->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    LPWSTR msgW = nullptr;
                                    HRESULT hr2 = args->TryGetWebMessageAsString(&msgW);
                                    if (FAILED(hr2) || !msgW) {
                                        args->get_WebMessageAsJson(&msgW);
                                    }
                                    if (!msgW) return S_OK;
                                    std::string msg = WtoA(msgW);
                                    CoTaskMemFree(msgW);

                                    // Parse action
                                    auto pos = msg.find("\"action\":\"");
                                    if (pos != std::string::npos) {
                                        std::string action = msg.substr(pos + 10);
                                        action = action.substr(0, action.find('"'));

                                        if (action == "inject") {
                                            CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                                                std::string result = DoInject();
                                                std::string js = "handleEvents(["
                                                    + std::string(result.find("\"ok\":true") != std::string::npos
                                                        ? "'injected'" : "'inject_fail'")
                                                    + "]);";
                                                if (result.find("\"ok\":true") == std::string::npos) {
                                                    auto mp = result.find("\"msg\":\"");
                                                    std::string errMsg = "Inject failed";
                                                    if (mp != std::string::npos) {
                                                        errMsg = result.substr(mp + 7);
                                                        errMsg = errMsg.substr(0, errMsg.find('"'));
                                                    }
                                                    js = "alert('" + errMsg + "');";
                                                }
                                                if (g_webView) {
                                                    g_webView->ExecuteScript(AtoW(js).c_str(), nullptr);
                                                }
                                                return 0;
                                            }, NULL, 0, NULL);
                                        }
                                        else if (action == "f5") SendHotkey(VK_F5);
                                        else if (action == "f6") SendHotkey(VK_F6);
                                        else if (action == "f8") SendHotkey(VK_F8);
                                        else if (action == "f10") SendHotkey(VK_F10);
                                        else if (action == "f12") SendHotkey(VK_F12);
                                        else if (action == "cue") {
                                            double val = 0.50;
                                            auto vp = msg.find("\"value\":");
                                            if (vp != std::string::npos) val = atof(msg.c_str() + vp + 8);
                                            char path[MAX_PATH];
                                            char* appdata = getenv("APPDATA");
                                            snprintf(path, MAX_PATH, "%s\\NBA2K-AutoGreen\\target.txt", appdata ? appdata : ".");
                                            FILE* f = fopen(path, "w");
                                            if (f) { fprintf(f, "%.2f", val); fclose(f); }
                                        }
                                        else if (action == "fadecue") {
                                            double val = 0.40;
                                            auto vp = msg.find("\"value\":");
                                            if (vp != std::string::npos) val = atof(msg.c_str() + vp + 8);
                                            char path[MAX_PATH];
                                            char* appdata = getenv("APPDATA");
                                            snprintf(path, MAX_PATH, "%s\\NBA2K-AutoGreen\\fade_target.txt", appdata ? appdata : ".");
                                            FILE* f = fopen(path, "w");
                                            if (f) { fprintf(f, "%.2f", val); fclose(f); }
                                        }
                                        else if (action == "poll") {
                                            std::string logData = PollLog();
                                            EnterCriticalSection(&g_netLock);
                                            std::string net = g_netJson;
                                            LeaveCriticalSection(&g_netLock);
                                            std::string merged = logData.substr(0, logData.size() - 1) + ",\"net\":" + net + "}";
                                            g_webView->PostWebMessageAsJson(AtoW(merged).c_str());
                                        }
                                    }
                                    return S_OK;
                                }).Get(), nullptr);

                        // Navigate to HTML
                        g_webView->NavigateToString(AtoW(HTML_PAGE).c_str());

                        // JS polls via postMessage({action:'poll'})

                        return S_OK;
                    }).Get());
            return S_OK;
        });

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, envCallback.Get());

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(&g_netLock);
    WSACleanup();
    return 0;
}

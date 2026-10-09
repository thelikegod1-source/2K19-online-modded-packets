/**
 * Auto-Green Injector for NBA 2K19
 * Injects autogreen.dll into NBA2K19.exe
 */

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

static const char* TARGET_PROCESS = "NBA2K19.exe";

DWORD FindProcess(const char* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool InjectDLL(DWORD pid, const char* dllPath)
{
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) { printf("[-] OpenProcess failed: %lu\n", GetLastError()); return false; }

    size_t pathLen = strlen(dllPath) + 1;
    void* remoteMem = VirtualAllocEx(proc, NULL, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) { printf("[-] VirtualAllocEx failed\n"); CloseHandle(proc); return false; }

    WriteProcessMemory(proc, remoteMem, dllPath, pathLen, NULL);

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remoteMem, 0, NULL);
    if (!thread) { printf("[-] CreateRemoteThread failed: %lu\n", GetLastError()); CloseHandle(proc); return false; }

    printf("[+] Injection thread created, waiting...\n");
    WaitForSingleObject(thread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exitCode == 0) { printf("[-] LoadLibrary returned NULL\n"); return false; }
    printf("[+] DLL injected (module at 0x%08X)\n", exitCode);
    return true;
}

int main(int argc, char* argv[])
{
    printf("=== NBA 2K19 Auto-Green Injector ===\n\n");

    char dllPath[MAX_PATH];
    GetModuleFileNameA(NULL, dllPath, MAX_PATH);
    char* slash = strrchr(dllPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(dllPath, "autogreen.dll");

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("[-] autogreen.dll not found: %s\n", dllPath);
        return 1;
    }
    printf("[*] DLL: %s\n", dllPath);

    DWORD pid = FindProcess(TARGET_PROCESS);
    if (pid) {
        printf("[+] Found %s PID %lu\n", TARGET_PROCESS, pid);
    } else {
        printf("[*] Waiting for %s...\n", TARGET_PROCESS);
        ULONGLONG deadline = GetTickCount64() + 120000;
        while (!(pid = FindProcess(TARGET_PROCESS))) {
            if (GetTickCount64() > deadline) { printf("[-] Timeout\n"); return 2; }
            Sleep(100);
        }
        printf("[+] Found %s PID %lu\n", TARGET_PROCESS, pid);
        Sleep(3000);
    }

    if (InjectDLL(pid, dllPath)) {
        printf("\n[+] Auto-green scanner running!\n");
        printf("[+] Log: %%APPDATA%%\\NBA2K-AutoGreen\\autogreen.log\n");
        printf("[+] CSV: %%APPDATA%%\\NBA2K-AutoGreen\\candidates.csv\n");
        printf("[+] Go take some shots — the scanner will find the shot meter address.\n");
        return 0;
    }

    printf("\n[-] Injection failed. Run as admin.\n");
    return 1;
}

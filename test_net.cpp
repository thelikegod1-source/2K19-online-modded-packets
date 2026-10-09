#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    DWORD size = 0;
    GetTcpTable2(NULL, &size, FALSE);
    printf("Table size: %lu\n", size);
    if (size == 0) { printf("FAILED\n"); return 1; }

    std::vector<BYTE> buf(size);
    PMIB_TCPTABLE2 table = (PMIB_TCPTABLE2)buf.data();
    DWORD ret = GetTcpTable2(table, &size, FALSE);
    printf("GetTcpTable2 ret: %lu, entries: %lu\n", ret, table->dwNumEntries);

    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        auto& row = table->table[i];
        if (row.dwState != MIB_TCP_STATE_ESTAB) continue;
        IN_ADDR addr;
        addr.S_un.S_addr = row.dwRemoteAddr;
        char ipStr[32];
        inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr));
        int port = ntohs((u_short)row.dwRemotePort);
        std::string ip(ipStr);
        if (ip.substr(0,4) == "127.") continue;
        printf("  %s:%d", ipStr, port);
        if (port == 20054 || ip.substr(0,8) == "155.103.") printf(" <-- COURT");
        if (port == 21000) printf(" <-- 2K");
        printf("\n");
    }
    WSACleanup();
    return 0;
}

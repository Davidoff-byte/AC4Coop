// Host-IP peer-to-peer UDP link (Winsock).
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include "net.h"

#pragma comment(lib, "ws2_32.lib")

namespace {
    bool               g_enabled = false;
    SOCKET             g_sock = INVALID_SOCKET;
    sockaddr_in        g_peer{};
    CRITICAL_SECTION   g_lock;
    PlayerState        g_remote{};
    bool               g_haveRemote = false;

    void dbg(const char* s) { printf("%s", s); OutputDebugStringA(s); }

    DWORD WINAPI RecvThread(LPVOID) {
        PlayerState pkt;
        for (;;) {
            sockaddr_in from{}; int flen = sizeof(from);
            int n = recvfrom(g_sock, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&from, &flen);
            if (n == sizeof(PlayerState) && pkt.magic == NET_MAGIC) {
                EnterCriticalSection(&g_lock);
                g_remote = pkt;
                g_haveRemote = true;
                LeaveCriticalSection(&g_lock);
            } else if (n == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEINTR || e == WSAENOTSOCK) break;   // socket closed
                Sleep(1);
            }
        }
        return 0;
    }
}

namespace Net {

bool Init() {
    if (GetPrivateProfileIntA("Net", "Enabled", 0, ".\\AC4Coop.ini") == 0) return false;

    int localPort = GetPrivateProfileIntA("Net", "LocalPort", 47801, ".\\AC4Coop.ini");
    int peerPort  = GetPrivateProfileIntA("Net", "PeerPort",  47801, ".\\AC4Coop.ini");
    char peerIp[64] = {0};
    GetPrivateProfileStringA("Net", "PeerIP", "127.0.0.1", peerIp, sizeof(peerIp), ".\\AC4Coop.ini");

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { dbg("[Net] WSAStartup failed\n"); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { dbg("[Net] socket failed\n"); return false; }

    // Bind LocalPort; if it's already taken (2nd instance on the same PC sharing this ini), bind
    // PeerPort instead and send to LocalPort. Two-PC setups use LocalPort==PeerPort and just bind.
    int bindPort = localPort, sendPort = peerPort;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons((u_short)bindPort);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        bindPort = peerPort; sendPort = localPort;
        local.sin_port = htons((u_short)bindPort);
        if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
            char m[96]; wsprintfA(m, "[Net] bind(%d/%d) failed err=%d\n", localPort, peerPort, WSAGetLastError()); dbg(m);
            closesocket(g_sock); g_sock = INVALID_SOCKET; return false;
        }
    }

    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons((u_short)sendPort);
    inet_pton(AF_INET, peerIp, &g_peer.sin_addr);

    InitializeCriticalSection(&g_lock);
    CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);

    g_enabled = true;
    char m[128]; wsprintfA(m, "[Net] up: bound:%d -> %s:%d\n", bindPort, peerIp, sendPort); dbg(m);
    return true;
}

bool Enabled() { return g_enabled; }

void Send(const PlayerState& s) {
    if (!g_enabled) return;
    sendto(g_sock, (const char*)&s, sizeof(s), 0, (sockaddr*)&g_peer, sizeof(g_peer));
}

bool GetRemote(PlayerState& out) {
    if (!g_enabled) return false;
    bool have;
    EnterCriticalSection(&g_lock);
    have = g_haveRemote;
    if (have) out = g_remote;
    LeaveCriticalSection(&g_lock);
    return have;
}

} // namespace Net

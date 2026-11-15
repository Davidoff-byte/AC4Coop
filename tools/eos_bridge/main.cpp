// eos_bridge - 64-bit EOS bridge for the 32-bit AC4 co-op mod (docs/design/eos-transport.md, "The bridge", slice 1).
// The EOS SDK v1.19 ships no Win32 DLL, so EOS runs here and the mod keeps its UDP transport, pointed at this process.
//
//   eos_bridge host            [--bridge-port 47900] [--game-port 47801] [--relay allow|force|none] [--ini local\eos.ini] [--tag A] [--echo]
//   eos_bridge join <CODE>     (same options; default --tag B)
//
// Data path: UDP 127.0.0.1:<bridge-port> (loopback only)  <->  EOS P2P socket AC4COOP, channel 1, unreliable-unordered.
//   game -> bridge : every datagram from a local sender goes to the remote bridge; the last local sender is remembered.
//   remote -> game : every P2P datagram goes to the last local sender (default 127.0.0.1:<game-port>).
//   --echo         : P2P datagrams are sent back to the remote instead of to the game (test oracle with udp_probe.py).
// Datagrams over EOS_P2P_MAX_PACKET_SIZE (1170 B) are dropped and counted. The host accepts the first peer that asks.
// Credentials come from the ini at run time and are never printed.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>            // before windows.h (pulled in by eos_core.h)
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "../eos_spike/eos_core.h"

static const uint8_t kDataChannel = 1;
static EOS_ProductUserId g_remote = nullptr;            // the peer bridge (host: first accepted; guest: lobby owner)
static SOCKET g_udp = INVALID_SOCKET;
static sockaddr_in g_game = {};                          // where the local game is (last sender, or the default)
static bool g_echo = false;
struct Stats { uint64_t udpIn, udpOut, p2pIn, p2pOut, tooBig, noRemote, sendFail; } g_st = {};

static bool OpenUdp(int port, int gamePort)
{
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return false;
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET) return false;
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(g_udp, (sockaddr*)&a, sizeof(a))) { printf("bind 127.0.0.1:%d failed (%d) - port in use?\n", port, WSAGetLastError()); return false; }
    u_long nb = 1; ioctlsocket(g_udp, FIONBIO, &nb);
    g_game.sin_family = AF_INET; g_game.sin_port = htons((u_short)gamePort); inet_pton(AF_INET, "127.0.0.1", &g_game.sin_addr);
    printf("UDP bridge on 127.0.0.1:%d, game expected at 127.0.0.1:%d\n", port, gamePort);
    return true;
}

static void PumpData()
{
    uint8_t buf[2048];
    for (;;) {   // game -> remote
        sockaddr_in from = {}; int fl = sizeof(from);
        const int n = recvfrom(g_udp, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) break;
        ++g_st.udpIn; g_game = from;
        if (n > EOS_P2P_MAX_PACKET_SIZE) { ++g_st.tooBig; continue; }
        if (!g_remote) { ++g_st.noRemote; continue; }
        if (Send(g_remote, kDataChannel, buf, (uint32_t)n, EOS_EPacketReliability::EOS_PR_UnreliableUnordered)) ++g_st.p2pOut; else ++g_st.sendFail;
    }
    for (;;) {   // remote -> game (or back, with --echo)
        EOS_ProductUserId from = nullptr; uint8_t ch = 0;
        const int n = Recv(&from, &ch, buf, sizeof(buf));
        if (n < 0) break;
        if (!g_remote) g_remote = from;   // host: the first peer that sends anything
        if (ch != kDataChannel) continue; // channel 2 = bridge control (the guest's hello), never game data
        ++g_st.p2pIn;
        if (g_echo) { if (Send(from, ch, buf, (uint32_t)n, EOS_EPacketReliability::EOS_PR_UnreliableUnordered)) ++g_st.p2pOut; else ++g_st.sendFail; continue; }
        if (sendto(g_udp, (const char*)buf, n, 0, (const sockaddr*)&g_game, sizeof(g_game)) == n) ++g_st.udpOut;
    }
}

static volatile bool g_quit = false;
static BOOL WINAPI OnCtrl(DWORD) { g_quit = true; return TRUE; }

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "join"))) {
        printf("usage: eos_bridge host|join <CODE> [--bridge-port 47900] [--game-port 47801] [--relay allow|force|none] [--ini local\\eos.ini] [--tag X] [--echo]\n");
        return 2;
    }
    const bool host = !strcmp(argv[1], "host");
    std::string code = (!host && argc > 2) ? argv[2] : "", tag = host ? "A" : "B", relay = "allow", ini = "local\\eos.ini";
    int bridgePort = 47900, gamePort = 47801;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--relay") && i + 1 < argc) relay = argv[++i];
        else if (!strcmp(argv[i], "--ini") && i + 1 < argc) ini = argv[++i];
        else if (!strcmp(argv[i], "--bridge-port") && i + 1 < argc) bridgePort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--game-port") && i + 1 < argc) gamePort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--echo")) g_echo = true;
    }
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    printf("eos_bridge %s tag=%s relay=%s%s\n", argv[1], tag.c_str(), relay.c_str(), g_echo ? " ECHO" : "");
    if (!OpenUdp(bridgePort, gamePort)) return 1;
    if (!StartPlatform(ini, tag, "AC4Coop EOS bridge")) return 1;

    int rc = 1;
    if (Login(("ac4coop-bridge-" + tag).c_str())) {
        SetRelay(relay.c_str());
        WatchEstablished();
        if (host) {
            EOS_P2P_SocketId s = Sock();
            EOS_P2P_AddNotifyPeerConnectionRequestOptions no = {}; no.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST; no.LocalUserId = g_me; no.SocketId = &s;
            EOS_P2P_AddNotifyPeerConnectionRequest(EOS_Platform_GetP2PInterface(g_plat), &no, nullptr, [](const EOS_P2P_OnIncomingConnectionRequestInfo* i) {
                if (g_remote && Puid(g_remote) != Puid(i->RemoteUserId)) { printf("refusing second peer %s\n", Puid(i->RemoteUserId).c_str()); return; }
                EOS_P2P_AcceptConnectionOptions ao = {}; ao.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST; ao.LocalUserId = i->LocalUserId; ao.RemoteUserId = i->RemoteUserId; ao.SocketId = i->SocketId;
                printf("peer %s asks -> accept: %s\n", Puid(i->RemoteUserId).c_str(), EOS_EResult_ToString(EOS_P2P_AcceptConnection(EOS_Platform_GetP2PInterface(g_plat), &ao)));
                g_remote = i->RemoteUserId;
            });
            code = MakeCode();
            if (!HostLobby(code)) goto done;
            printf("\n*** LOBBY CODE: %s ***  (Ctrl+C to stop)\n\n", code.c_str());
        } else {
            g_remote = JoinByCode(code);
            if (!g_remote) { printf("join failed\n"); goto done; }
            if (Puid(g_remote) == Puid(g_me)) { printf("INVALID: the lobby owner is this same EOS user (run the other side on another Windows user or PC)\n"); goto done; }
            // Open the link now (EOS connects on first send) so it is up before game traffic starts.
            static const char hello[] = "AC4B-HELLO";
            Send(g_remote, 2, hello, sizeof(hello), EOS_EPacketReliability::EOS_PR_ReliableOrdered);
        }
        {
            double nextStat = NowMs() + 5000.0;
            while (!g_quit) {
                EOS_Platform_Tick(g_plat);
                PumpData();
                if (NowMs() >= nextStat) {
                    nextStat += 5000.0;
                    printf("[stats] link=%s udpIn=%llu udpOut=%llu p2pIn=%llu p2pOut=%llu tooBig=%llu noRemote=%llu sendFail=%llu\n", NetName(g_established),
                           g_st.udpIn, g_st.udpOut, g_st.p2pIn, g_st.p2pOut, g_st.tooBig, g_st.noRemote, g_st.sendFail);
                }
                Sleep(1);
            }
            rc = 0;
        }
    done:
        LeaveLobby(host);
    }
    EOS_Platform_Release(g_plat); EOS_Shutdown();
    closesocket(g_udp); WSACleanup();
    printf("shutdown done\n");
    return rc;
}

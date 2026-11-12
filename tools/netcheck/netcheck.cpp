// ac4coop_netcheck.exe - "can this PC host a lobby code?" (docs/design/lobby-code.md, slices 4-5).
// Built from the SAME src/upnp.cpp and src/stun.cpp the plugin uses, so a run on another network is
// also a test of that code against another router. No game needed.
//
// Build (x86 Developer prompt, from the repo root):
//   cl /nologo /O2 /EHsc /std:c++17 /Isrc tools\netcheck\netcheck.cpp src\upnp.cpp src\stun.cpp ^
//      /Fe:bin\ac4coop_netcheck.exe ws2_32.lib winhttp.lib
//
// Usage: ac4coop_netcheck.exe [--try-map] [--show-ips] [--port 47801]
//   default    read-only: UPnP discovery + GetExternalIPAddress + GetSpecificPortMappingEntry, and two STUN
//              Binding requests (stun.l.google.com, stun1.l.google.com; those servers see this PC's IP).
//   --try-map  also ADD a UDP mapping for --port with a 120 s lease, read it back, and DELETE it.
//   --show-ips print addresses; by default only their class is printed, so the output is safe to share.
// Writes the same report to netcheck.txt beside the exe. Exit 0 = can host, 1 = cannot, 2 = unclear.
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "upnp.h"
#include "stun.h"

static FILE* g_rep = nullptr;
static bool g_showIps = false;

static void Out(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt); _vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    b[sizeof(b) - 1] = 0;
    fputs(b, stdout);
    if (g_rep) fputs(b, g_rep);
}
static const char* ClassName(int c) {
    return c == 0 ? "public" : c == 1 ? "private (RFC 1918)" : c == 2 ? "carrier-grade NAT (100.64/10)" : "non-routable";
}
static const char* Ip(uint32_t ip, char* b) {
    if (g_showIps) sprintf(b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    else strcpy(b, "<hidden>");
    return b;
}

int main(int argc, char** argv) {
    bool tryMap = false; unsigned port = 47801;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--try-map")) tryMap = true;
        else if (!strcmp(argv[i], "--show-ips")) g_showIps = true;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned)atoi(argv[++i]);
        else { printf("usage: ac4coop_netcheck [--try-map] [--show-ips] [--port 47801]\n"); return 2; }
    }
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) strcpy(sl + 1, "netcheck.txt");
    g_rep = fopen(exe, "w");
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    char t1[32], err[256];
    SYSTEMTIME st; GetLocalTime(&st);
    Out("[netcheck] %04u-%02u-%02u %02u:%02u port=%u try-map=%d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        port, tryMap ? 1 : 0);
    { char sm[160]; Out("%s", (Stun::SelfTest(sm, sizeof(sm)), sm)); }

    uint32_t lan = 0;
    if (!Upnp::DefaultRouteIPv4(&lan)) { Out("[netcheck] RESULT no IPv4 default route - offline?\n"); return 2; }
    Out("[netcheck] LAN address %s class=%s\n", Ip(lan, t1), ClassName(Upnp::AddressClass(lan)));

    // ---- UPnP --------------------------------------------------------------------------------
    Upnp::Gateway gw{};
    bool upnp = Upnp::Discover(lan, &gw, err, sizeof(err));
    uint32_t ext = 0; bool haveExt = false;
    if (upnp) {
        Out("[netcheck] UPnP gateway found: %s\n", gw.service);
        haveExt = Upnp::GetExternalIp(gw, &ext, err, sizeof(err));
        if (haveExt) Out("[netcheck] UPnP external address %s class=%s\n", Ip(ext, t1), ClassName(Upnp::AddressClass(ext)));
        else Out("[netcheck] UPnP GetExternalIPAddress failed: %s\n", err);
        uint32_t cl = 0; uint16_t ip2 = 0; char desc[128] = {0};
        if (Upnp::GetUdpMapping(gw, (uint16_t)port, &cl, &ip2, desc, sizeof(desc), err, sizeof(err)))
            Out("[netcheck] existing UDP %u mapping -> %s:%u desc=\"%s\" (%s)\n", port, Ip(cl, t1), ip2, desc,
                cl == lan ? "this PC" : "ANOTHER device - a host here would conflict");
        else Out("[netcheck] no existing UDP %u mapping (%s)\n", port, err);
    } else {
        Out("[netcheck] UPnP: %s\n", err);
    }

    // ---- STUN from the game port (if free) ------------------------------------------------------
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(lan); me.sin_port = htons((u_short)port);
    bool onGamePort = bind(s, (sockaddr*)&me, sizeof(me)) == 0;
    if (!onGamePort) { me.sin_port = 0; bind(s, (sockaddr*)&me, sizeof(me)); }
    sockaddr_in got{}; int gl = sizeof(got); getsockname(s, (sockaddr*)&got, &gl);
    const unsigned localPort = ntohs(got.sin_port);
    uint32_t sa = 0, sb = 0; uint16_t pa = 0, pb = 0;
    sockaddr_in srvA{}, srvB{};
    const bool ra = Stun::Resolve("stun.l.google.com:19302", &srvA) && Stun::Query(s, srvA, 600, &sa, &pa);
    const bool rb = Stun::Resolve("stun1.l.google.com:19302", &srvB) && Stun::Query(s, srvB, 600, &sb, &pb);
    closesocket(s);
    Out("[netcheck] STUN from local port %u%s: A=%s B=%s\n", localPort, onGamePort ? "" : " (game port busy)",
        ra ? "answered" : "no answer", rb ? "answered" : "no answer");
    if (ra) Out("[netcheck] internet sees %s:%u class=%s\n", Ip(sa, t1), pa, ClassName(Upnp::AddressClass(sa)));
    const bool sameMap = ra && rb && sa == sb && pa == pb;
    if (ra && rb) Out("[netcheck] NAT mapping: %s; port %s\n",
                      sameMap ? "same for both servers (endpoint-independent: hole-punch friendly)" : "DIFFERS per server (symmetric NAT)",
                      pa == localPort ? "preserved" : "rewritten");

    // ---- optional live mapping test ---------------------------------------------------------
    bool mapOk = false;
    if (tryMap && upnp) {
        int ue = 0; uint32_t lease = 0; uint16_t extUsed = 0;
        if (Upnp::AddUdpMappingAny(gw, (uint16_t)port, 120, "AC4Coop netcheck", &extUsed, &ue, &lease, err, sizeof(err))) {
            uint32_t cl = 0; uint16_t ip2 = 0; char desc[128] = {0};
            const bool back = Upnp::GetUdpMapping(gw, extUsed, &cl, &ip2, desc, sizeof(desc), err, sizeof(err)) &&
                              cl == lan && ip2 == (uint16_t)port;
            const bool del = Upnp::DeleteUdpMapping(gw, extUsed, err, sizeof(err));
            uint32_t c2 = 0; uint16_t p2 = 0;
            const bool gone = !Upnp::GetUdpMapping(gw, extUsed, &c2, &p2, nullptr, 0, err, sizeof(err));
            mapOk = back;
            Out("[netcheck] try-map: add OK ext %u -> local %u (lease %u s%s), read back %s, delete %s, gone %s\n",
                extUsed, port, lease, extUsed != port ? ", port conflict skipped" : "",
                back ? "OK" : "MISMATCH", del ? "OK" : "FAILED", gone ? "yes" : "NO - remove it in the router UI");
        } else {
            Out("[netcheck] try-map: AddPortMapping refused: %s\n", err);
        }
    }

    // ---- verdict ------------------------------------------------------------------------------
    int rc = 2;
    const char* why = "";
    if (!ra && !rb) { rc = 2; why = "no STUN answer: outgoing UDP is blocked here, or no internet"; }
    else if (ra && lan == sa) { rc = 0; why = "this PC has a public address directly (allow the game through the firewall)"; }
    else if (upnp && haveExt && ra && ext == sa && (!tryMap || mapOk)) {
        rc = 0; why = tryMap ? "single router with working UPnP: the game can open the port itself"
                             : "single router with UPnP (run with --try-map to confirm the router accepts a mapping)";
    }
    else if (upnp && haveExt && Upnp::AddressClass(ext) == 2) { rc = 1; why = "carrier-grade NAT at the ISP: nobody can reach this PC unsolicited"; }
    else if (upnp && haveExt && Upnp::AddressClass(ext) == 1) {
        rc = 1; why = "double NAT: this router sits behind another one; UPnP here only opens the inner one "
                      "(outer router would need a UDP forward to this router, or bridge mode)";
    }
    else if (upnp && tryMap && !mapOk) { rc = 1; why = "UPnP present but it refused the mapping"; }
    else if (!upnp) { rc = 1; why = "no UPnP: hosting needs a manual UDP port forward on the router"; }
    Out("[netcheck] VERDICT %s - %s\n", rc == 0 ? "CAN HOST" : rc == 1 ? "CANNOT HOST by code" : "UNCLEAR", why);
    if (ra && rb) Out("[netcheck] hole-punch fallback: %s\n", sameMap ? "likely possible" : "unlikely (symmetric NAT)");
    if (g_rep) fclose(g_rep);
    return rc;
}

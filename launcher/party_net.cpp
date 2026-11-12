// The launcher's party networking. See party_net.h and docs/design/launcher.md.
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include "party_net.h"
#include "lobby_code.h"
#include "relay_proto.h"
#include "upnp.h"
#include "stun.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

using namespace PartyNet;

Config            g_cfg;
SOCKET            g_sock = INVALID_SOCKET;
CRITICAL_SECTION  g_lock;
// Initialised at program start: the UI calls Snapshot() every frame, before any Init().
struct LockInit { LockInit() { InitializeCriticalSection(&g_lock); WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); } } g_lockInit;
State             g_st;                 // guarded by g_lock
volatile LONG     g_run = 0;            // socket alive
volatile LONG     g_gen = 0;            // bumps on Host/Join/Leave: old threads retire

// ---- relay ----
sockaddr_in       g_relay{};
bool              g_relayOk = false;
volatile LONG64   g_room = 0;
uint32_t          g_relayNonce = 0;
volatile LONG     g_registered = 0;
volatile LONG     g_peerInfo = 0;       // guest: PEER received

// ---- host: members ----
struct Mem { bool used; char name[Party::kNameLen]; sockaddr_in ep; bool viaRelay; uint16_t slot; uint32_t nonce; ULONGLONG seen; };
Mem               g_mem[Party::kMaxMembers];   // [0] = host itself (guarded by g_lock)
volatile LONG     g_started = 0;
Party::Start      g_start{};

// ---- STUN through the receive thread (one reader per socket) ----
uint8_t           g_stunTid[12];
volatile LONG     g_stunPending = 0, g_stunDone = 0;
uint32_t          g_stunIp = 0;
uint16_t          g_stunPort = 0;

// ---- guest ----
sockaddr_in       g_hostEp{};           // direct host endpoint (when not via relay)
bool              g_guestViaRelay = false;
uint32_t          g_myNonce = 0;
volatile LONG     g_joined = 0;
ULONGLONG         g_lastFromHost = 0;

void SetStatus(const char* fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); _vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); b[sizeof(b) - 1] = 0;
    EnterCriticalSection(&g_lock); lstrcpynA(g_st.status, b, sizeof(g_st.status)); LeaveCriticalSection(&g_lock);
}
void SetPhase(Phase p) { EnterCriticalSection(&g_lock); g_st.phase = p; LeaveCriticalSection(&g_lock); }
uint32_t Nonce() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (uint32_t)q.LowPart ^ (GetCurrentProcessId() << 12) ^ (uint32_t)GetTickCount64(); }
bool Same(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }
sockaddr_in Sa(uint32_t ipHost, uint16_t port) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ipHost); a.sin_port = htons(port); return a; }

void RelaySend(uint8_t type, uint16_t slot, uint64_t room, const void* p, int n) {
    uint8_t b[Relay::kMaxDatagram];
    Relay::Hdr h{ Relay::kMagic, Relay::kVersion, type, slot, room };
    memcpy(b, &h, sizeof(h)); if (n) memcpy(b + sizeof(h), p, (size_t)n);
    sendto(g_sock, (const char*)b, (int)sizeof(h) + n, 0, (const sockaddr*)&g_relay, sizeof(g_relay));
}
void RelayHello(uint8_t type, uint64_t room) {
    uint32_t lan = 0; Upnp::DefaultRouteIPv4(&lan);
    Relay::Hello h{ g_cfg.gameProto, lan, (uint16_t)g_st.boundPort, 0, g_relayNonce, 0, 0 };
    RelaySend(type, 0, room, &h, (int)sizeof(h));
}
// A party datagram to a member (direct, or as relay DATA to its slot).
void PartySend(const Mem& m, uint8_t type, uint32_t nonce, const void* p, int n) {
    uint8_t b[512];
    Party::Hdr h{ Party::kMagic, Party::kVersion, type, 0, nonce };
    memcpy(b, &h, sizeof(h)); if (n) memcpy(b + sizeof(h), p, (size_t)n);
    if (m.viaRelay) RelaySend(Relay::T_DATA, m.slot, (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0), b, (int)sizeof(h) + n);
    else sendto(g_sock, (const char*)b, (int)sizeof(h) + n, 0, (const sockaddr*)&m.ep, sizeof(m.ep));
}
void ToHost(uint8_t type, const void* p, int n) {
    Mem host{}; host.viaRelay = g_guestViaRelay; host.slot = Relay::kHostSlot; host.ep = g_hostEp;
    PartySend(host, type, g_myNonce, p, n);
}

Party::Roster BuildRoster() {   // caller holds g_lock
    Party::Roster r{}; r.maxPlay = 2;
    for (int i = 0; i < Party::kMaxMembers; ++i) if (g_mem[i].used) {
        Party::Member& m = r.m[r.count++];
        lstrcpynA(m.name, g_mem[i].name, Party::kNameLen); m.slot = (uint8_t)i;
    }
    return r;
}
void PublishRoster(const Party::Roster& r) {   // caller holds g_lock
    g_st.count = r.count; g_st.maxPlay = r.maxPlay;
    memcpy(g_st.members, r.m, sizeof(r.m));
}
void BroadcastRoster() {
    EnterCriticalSection(&g_lock);
    Party::Roster r = BuildRoster(); PublishRoster(r);
    Mem copy[Party::kMaxMembers]; memcpy(copy, g_mem, sizeof(copy));
    LeaveCriticalSection(&g_lock);
    for (int i = 1; i < Party::kMaxMembers; ++i) if (copy[i].used) PartySend(copy[i], Party::T_ROSTER, 0, &r, (int)sizeof(r));
}

// ---- receive ----
void OnHostParty(const Party::Hdr& h, const uint8_t* p, int n, const sockaddr_in& from, bool viaRelay, uint16_t slot) {
    Mem src{}; src.ep = from; src.viaRelay = viaRelay; src.slot = slot;
    EnterCriticalSection(&g_lock);
    int idx = -1;
    for (int i = 1; i < Party::kMaxMembers; ++i)
        if (g_mem[i].used && g_mem[i].nonce == h.nonce && g_mem[i].viaRelay == viaRelay && (viaRelay ? g_mem[i].slot == slot : Same(g_mem[i].ep, from))) idx = i;
    LeaveCriticalSection(&g_lock);
    if (h.type == Party::T_JOIN && n >= (int)sizeof(Party::Join)) {
        Party::Join j; memcpy(&j, p, sizeof(j)); j.name[Party::kNameLen - 1] = 0;
        Party::Refuse rf{ 0, 0 };
        if (j.gameProto != g_cfg.gameProto) rf.code = Party::R_PROTO;
        else if (InterlockedCompareExchange(&g_started, 0, 0) && idx < 0) rf.code = Party::R_STARTED;
        if (!rf.code && idx < 0) {
            EnterCriticalSection(&g_lock);
            for (int i = 1; i < Party::kMaxMembers && idx < 0; ++i) if (!g_mem[i].used) {
                idx = i; g_mem[i] = Mem{ true, {0}, from, viaRelay, slot, h.nonce, GetTickCount64() };
                lstrcpynA(g_mem[i].name, j.name[0] ? j.name : "Player", Party::kNameLen);
            }
            LeaveCriticalSection(&g_lock);
            if (idx < 0) rf.code = Party::R_FULL;
        }
        if (rf.code) { PartySend(src, Party::T_REFUSE, h.nonce, &rf, (int)sizeof(rf)); return; }
        EnterCriticalSection(&g_lock); g_mem[idx].seen = GetTickCount64(); LeaveCriticalSection(&g_lock);
        BroadcastRoster();
        if (InterlockedCompareExchange(&g_started, 0, 0)) PartySend(src, Party::T_START, 0, &g_start, (int)sizeof(g_start));
        SetStatus("%s joined", j.name);
    } else if (h.type == Party::T_PING && idx >= 0) {
        EnterCriticalSection(&g_lock); g_mem[idx].seen = GetTickCount64(); Party::Roster r = BuildRoster(); LeaveCriticalSection(&g_lock);
        PartySend(src, Party::T_ROSTER, 0, &r, (int)sizeof(r));
        if (InterlockedCompareExchange(&g_started, 0, 0)) PartySend(src, Party::T_START, 0, &g_start, (int)sizeof(g_start));
    } else if (h.type == Party::T_LEAVE && idx >= 0) {
        EnterCriticalSection(&g_lock); char nm[Party::kNameLen]; lstrcpynA(nm, g_mem[idx].name, sizeof(nm)); g_mem[idx].used = false; LeaveCriticalSection(&g_lock);
        BroadcastRoster(); SetStatus("%s left", nm);
    }
}

void OnGuestParty(const Party::Hdr& h, const uint8_t* p, int n) {
    g_lastFromHost = GetTickCount64();
    if (h.type == Party::T_ROSTER && n >= (int)sizeof(Party::Roster)) {
        Party::Roster r; memcpy(&r, p, sizeof(r)); if (r.count > Party::kMaxMembers) r.count = Party::kMaxMembers;
        EnterCriticalSection(&g_lock); PublishRoster(r);
        if (g_st.phase == JOINING) { g_st.phase = JOINED; lstrcpynA(g_st.status, "In the party - waiting for the host to start", sizeof(g_st.status)); }
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_joined, 1);
    } else if (h.type == Party::T_REFUSE && n >= (int)sizeof(Party::Refuse) && h.nonce == g_myNonce) {
        Party::Refuse rf; memcpy(&rf, p, sizeof(rf));
        SetPhase(FAILED);
        SetStatus("The host refused: %s", rf.code == Party::R_FULL ? "the party is full" : rf.code == Party::R_PROTO ?
                  "you run a different mod build (update both)" : rf.code == Party::R_STARTED ? "the game has already started" : "?");
    } else if (h.type == Party::T_START && n >= (int)sizeof(Party::Start)) {
        Party::Start s; memcpy(&s, p, sizeof(s)); s.joinCode[15] = 0; s.relayServer[63] = 0;
        EnterCriticalSection(&g_lock);
        if (g_st.phase != STARTED) { g_st.start = s; g_st.phase = STARTED; lstrcpynA(g_st.status, "The host started the game - launching", sizeof(g_st.status)); }
        LeaveCriticalSection(&g_lock);
    } else if (h.type == Party::T_LEAVE) {
        SetPhase(FAILED); SetStatus("The host closed the party");
    }
}

void OnRelayControl(const Relay::Hdr& h, const uint8_t* p, int n) {
    if (h.type == Relay::T_REGISTERED && n >= (int)sizeof(Relay::Registered)) {
        Relay::Registered r; memcpy(&r, p, sizeof(r));
        if (r.nonce != g_relayNonce) return;
        InterlockedExchange64(&g_room, (LONG64)(h.room & 0xFFFFFFFFFFull));
        InterlockedExchange(&g_registered, 1);
    } else if (h.type == Relay::T_PEER && n >= (int)sizeof(Relay::Peer)) {
        Relay::Peer pe; memcpy(&pe, p, sizeof(pe));
        if (!g_st.isHost && pe.nonce == g_relayNonce) InterlockedExchange(&g_peerInfo, 1);
    } else if (h.type == Relay::T_ERR && n >= (int)sizeof(Relay::Error)) {
        Relay::Error e; memcpy(&e, p, sizeof(e));
        if (e.nonce != g_relayNonce) return;
        if (!g_st.isHost && e.code == Relay::E_UNKNOWN_ROOM) { SetPhase(FAILED); SetStatus("No such room on the relay server (mistyped, expired, or a different server)"); }
        else if (e.code == Relay::E_PROTO) { SetPhase(FAILED); SetStatus("The host runs a different mod build"); }
        else if (e.code == Relay::E_ROOM_FULL) { SetPhase(FAILED); SetStatus("The room is full"); }
    }
}

unsigned __stdcall RecvThread(void*) {
    uint8_t buf[2048];
    while (InterlockedCompareExchange(&g_run, 0, 0)) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) { if (WSAGetLastError() == WSAENOTSOCK || WSAGetLastError() == WSAEINTR) break; Sleep(1); continue; }
        if (n < 12) continue;
        if (Stun::LooksLikeStun(buf, n)) {   // no party/relay datagram can match (first byte 'A')
            uint32_t ip = 0; uint16_t pt = 0;
            if (InterlockedCompareExchange(&g_stunPending, 0, 0) && Stun::ParseResponse(buf, n, g_stunTid, &ip, &pt)) {
                g_stunIp = ip; g_stunPort = pt; InterlockedExchange(&g_stunPending, 0); InterlockedExchange(&g_stunDone, 1);
            }
            continue;
        }
        uint32_t magic; memcpy(&magic, buf, 4);
        const uint8_t* p = buf; bool viaRelay = false; uint16_t slot = 0;
        if (magic == Relay::kMagic && g_relayOk && Same(from, g_relay) && n >= (int)sizeof(Relay::Hdr)) {
            Relay::Hdr rh; memcpy(&rh, buf, sizeof(rh));
            if (rh.type != Relay::T_DATA) { OnRelayControl(rh, buf + sizeof(rh), n - (int)sizeof(rh)); continue; }
            if ((rh.room & 0xFFFFFFFFFFull) != (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0)) continue;
            viaRelay = true; slot = rh.slot; p = buf + sizeof(rh); n -= (int)sizeof(rh);
            if (n < (int)sizeof(Party::Hdr)) continue;
            memcpy(&magic, p, 4);
        }
        if (magic != Party::kMagic || n < (int)sizeof(Party::Hdr)) continue;
        Party::Hdr h; memcpy(&h, p, sizeof(h));
        if (h.version != Party::kVersion) continue;
        if (g_st.isHost) OnHostParty(h, p + sizeof(h), n - (int)sizeof(h), from, viaRelay, slot);
        else if (viaRelay ? slot == Relay::kHostSlot : Same(from, g_hostEp)) OnGuestParty(h, p + sizeof(h), n - (int)sizeof(h));
    }
    return 0;
}

bool ResolveRelay() {
    if (!g_cfg.relay[0]) return false;
    char hp[160]; lstrcpynA(hp, g_cfg.relay, sizeof(hp));
    if (!strchr(hp, ':')) lstrcatA(hp, ":47800");
    g_relayOk = Stun::Resolve(hp, &g_relay);
    return g_relayOk;
}

// ---- host setup + upkeep ----
unsigned __stdcall HostThread(void* genArg) {
    const LONG gen = (LONG)(intptr_t)genArg;
    auto alive = [&] { return InterlockedCompareExchange(&g_gen, 0, 0) == gen && InterlockedCompareExchange(&g_run, 0, 0); };
    const uint16_t port = (uint16_t)g_st.boundPort;
    uint32_t lan = 0; const bool haveLan = Upnp::DefaultRouteIPv4(&lan);
    char err[256];
    // Relay room first: if a relay server is set, its room code is the one to share (works through any NAT).
    if (g_cfg.relay[0]) {
        SetStatus("Contacting the relay server...");
        if (ResolveRelay()) {
            g_relayNonce = Nonce();
            for (int i = 0; i < 8 && alive() && !g_registered; ++i) { RelayHello(Relay::T_REGISTER, (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0)); Sleep(600); }
        }
    }
    // UPnP + STUN for the direct code (and the verdict shown to the host).
    bool mapped = false, haveExt = false; uint32_t ext = 0; uint16_t extPort = 0;
    if (g_cfg.upnp && haveLan && alive()) {
        SetStatus("Opening the port on your router (UPnP)...");
        Upnp::Gateway gw{};
        if (Upnp::Discover(lan, &gw, err, sizeof(err))) {
            for (int k = 0; k < 10; ++k) {   // stale mapping of this PC (crashed launcher/game)
                uint32_t cl = 0; uint16_t ip2 = 0; char d[64] = {0};
                if (Upnp::GetUdpMapping(gw, (uint16_t)(port + k), &cl, &ip2, d, sizeof(d), err, sizeof(err)) && cl == lan && !strcmp(d, "AC4Coop"))
                    Upnp::DeleteUdpMapping(gw, (uint16_t)(port + k), err, sizeof(err));
            }
            int ue = 0; uint32_t lease = 0;
            mapped = Upnp::AddUdpMappingAny(gw, port, 7200, "AC4Coop", &extPort, &ue, &lease, err, sizeof(err));
            haveExt = Upnp::GetExternalIp(gw, &ext, err, sizeof(err));
        }
    }
    uint32_t sIp = 0; uint16_t sPort = 0; bool stun = false;
    // STUN from the party socket (the mapping the game will reuse); the receive thread completes it.
    if (g_cfg.stunServer[0] && alive()) {
        SetStatus("Checking your internet address (STUN)...");
        sockaddr_in srv{};
        if (Stun::Resolve(g_cfg.stunServer, &srv)) {
            for (int attempt = 0; attempt < 3 && !stun && alive(); ++attempt) {
                InterlockedExchange(&g_stunPending, 0);
                uint32_t seed = Nonce(); for (int i = 0; i < 12; ++i) { seed = seed * 1103515245u + 12345u; g_stunTid[i] = (uint8_t)(seed >> 16); }
                InterlockedExchange(&g_stunDone, 0); InterlockedExchange(&g_stunPending, 1);
                uint8_t req[20]; Stun::BuildRequest(req, g_stunTid);
                sendto(g_sock, (const char*)req, sizeof(req), 0, (const sockaddr*)&srv, sizeof(srv));
                for (int w = 0; w < 50 && !InterlockedCompareExchange(&g_stunDone, 0, 0); ++w) Sleep(20);
                if (InterlockedCompareExchange(&g_stunDone, 0, 0)) { stun = true; sIp = g_stunIp; sPort = g_stunPort; }
            }
            InterlockedExchange(&g_stunPending, 0);
        }
    }
    if (!alive()) return 0;
    const char* verdict; const char* advice; uint32_t cIp = 0; uint16_t cPort = 0;
    if (!stun) { verdict = "NO_STUN"; advice = "Internet address unknown - only players on your network can join with this code."; }
    else if (haveLan && lan == sIp) { verdict = "PUBLIC"; advice = "Your PC is directly on the internet - friends can join."; cIp = sIp; cPort = port; }
    else if (mapped && haveExt && ext == sIp) { verdict = "OK"; advice = "Your router opened the port - friends can join."; cIp = sIp; cPort = extPort; }
    else if (haveExt && Upnp::AddressClass(ext) == 2) { verdict = "CGNAT"; advice = "Your internet provider shares your address (carrier NAT) - friends outside cannot reach you. Use a relay server or let a friend host."; cIp = sIp; cPort = sPort; }
    else if (haveExt && Upnp::AddressClass(ext) == 1) { verdict = "DOUBLE_NAT"; advice = "Your router is behind another router - friends outside probably cannot reach you. Use a relay server or let a friend host."; cIp = sIp; cPort = sPort; }
    else { verdict = "NO_UPNP"; advice = "Your router did not open the port - forward UDP to this PC, use a relay server, or let a friend host."; cIp = sIp; cPort = sPort; }
    char code[15] = {0};
    const bool room = InterlockedCompareExchange(&g_registered, 0, 0) != 0;
    if (room) {
        uint32_t ip = 0; uint16_t pt = 0; Relay::AddressFromRoom((uint64_t)InterlockedCompareExchange64(&g_room, 0, 0), &ip, &pt);
        LobbyCode::Encode(ip, pt, code); verdict = "RELAY"; advice = "Room code through your relay server - works from any network.";
    } else if (cIp) LobbyCode::Encode(cIp, cPort, code);
    else if (haveLan) LobbyCode::Encode(lan, port, code);
    char lanCode[15] = {0}; if (haveLan) LobbyCode::Encode(lan, port, lanCode);
    EnterCriticalSection(&g_lock);
    lstrcpynA(g_st.code, code, sizeof(g_st.code)); lstrcpynA(g_st.verdict, verdict, sizeof(g_st.verdict));
    lstrcpynA(g_st.lanCode, strcmp(lanCode, code) ? lanCode : "", sizeof(g_st.lanCode));
    lstrcpynA(g_st.advice, (g_cfg.relay[0] && !room) ? "The relay server did not answer - using a direct code instead." : advice, sizeof(g_st.advice));
    g_st.phase = code[0] ? HOSTING : FAILED;
    lstrcpynA(g_st.status, code[0] ? "Hosting - send your code to your friends" : "No code: no network address found", sizeof(g_st.status));
    LeaveCriticalSection(&g_lock);
    BroadcastRoster();
    // Upkeep: relay re-REGISTER every 15 s; drop guests silent for 10 s.
    ULONGLONG lastRelay = GetTickCount64();
    while (alive()) {
        Sleep(500);
        const ULONGLONG now = GetTickCount64();
        if (g_relayOk && now - lastRelay > 15000) { lastRelay = now; RelayHello(Relay::T_REGISTER, (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0)); }
        bool changed = false;
        EnterCriticalSection(&g_lock);
        for (int i = 1; i < Party::kMaxMembers; ++i) if (g_mem[i].used && now - g_mem[i].seen > 10000) { g_mem[i].used = false; changed = true; }
        LeaveCriticalSection(&g_lock);
        if (changed && !InterlockedCompareExchange(&g_started, 0, 0)) BroadcastRoster();
    }
    return 0;
}

// ---- guest ----
unsigned __stdcall GuestThread(void* genArg) {
    const LONG gen = (LONG)(intptr_t)genArg;
    auto alive = [&] { return InterlockedCompareExchange(&g_gen, 0, 0) == gen && InterlockedCompareExchange(&g_run, 0, 0); };
    if (g_guestViaRelay) {
        SetStatus("Contacting the relay server...");
        const uint64_t room = (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0);
        for (int i = 0; i < 30 && alive() && !g_peerInfo; ++i) {
            RelayHello(Relay::T_JOIN, room); Sleep(500);
            EnterCriticalSection(&g_lock); const bool failed = g_st.phase == FAILED; LeaveCriticalSection(&g_lock);
            if (failed) return 0;
        }
        if (!g_peerInfo) { if (alive()) { SetPhase(FAILED); SetStatus("The relay server did not answer"); } return 0; }
    }
    SetStatus("Joining the party...");
    Party::Join j{}; j.gameProto = g_cfg.gameProto; lstrcpynA(j.name, g_cfg.name, Party::kNameLen);
    const ULONGLONG t0 = GetTickCount64();
    while (alive() && !g_joined) {
        EnterCriticalSection(&g_lock); const Phase ph = g_st.phase; LeaveCriticalSection(&g_lock);
        if (ph == FAILED) return 0;
        if (GetTickCount64() - t0 > 20000) { SetPhase(FAILED); SetStatus("No answer from the host (wrong code, host not hosting, or its router blocks it)"); return 0; }
        ToHost(Party::T_JOIN, &j, (int)sizeof(j)); Sleep(500);
    }
    ULONGLONG lastKeep = 0;
    while (alive()) {
        const ULONGLONG now = GetTickCount64();
        EnterCriticalSection(&g_lock); const Phase ph = g_st.phase; LeaveCriticalSection(&g_lock);
        if (ph == FAILED) return 0;
        if (ph != STARTED) ToHost(Party::T_PING, nullptr, 0);
        if (g_guestViaRelay && now - lastKeep > 15000) { lastKeep = now; RelaySend(Relay::T_KEEPALIVE, 0, (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0), nullptr, 0); }
        if (ph == JOINED && now - g_lastFromHost > 15000) { SetPhase(FAILED); SetStatus("Lost the host (no answer for 15 s)"); return 0; }
        Sleep(2000);
    }
    return 0;
}

unsigned __stdcall StartThread(void*) {
    for (int k = 0; k < 8; ++k) {   // repeated: UDP may drop one; guests act on the first and ignore the rest
        EnterCriticalSection(&g_lock); Mem copy[Party::kMaxMembers]; memcpy(copy, g_mem, sizeof(copy)); LeaveCriticalSection(&g_lock);
        for (int i = 1; i < Party::kMaxMembers; ++i) if (copy[i].used) PartySend(copy[i], Party::T_START, 0, &g_start, (int)sizeof(g_start));
        Sleep(250);
    }
    return 0;
}

}  // namespace

namespace PartyNet {

bool Init(const Config& c, char* err, size_t errN) {
    g_cfg = c;
    if (g_sock != INVALID_SOCKET) return true;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_port = htons((u_short)c.port);
    bool gamePort = bind(g_sock, (sockaddr*)&me, sizeof(me)) == 0;   // no SO_REUSEADDR: a running game must not lose its port
    if (!gamePort) { me.sin_port = 0; if (bind(g_sock, (sockaddr*)&me, sizeof(me)) != 0) { _snprintf(err, errN, "cannot bind a UDP port (%d)", WSAGetLastError()); return false; } }
    sockaddr_in got{}; int gl = sizeof(got); getsockname(g_sock, (sockaddr*)&got, &gl);
    // Ignore ICMP "port unreachable" resets on this UDP socket (else recvfrom fails after a send to a closed port).
    BOOL off = FALSE; DWORD br = 0; WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), nullptr, 0, &br, nullptr, nullptr);
    EnterCriticalSection(&g_lock); g_st = State{}; g_st.boundPort = ntohs(got.sin_port); g_st.portIsGamePort = gamePort; LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_run, 1);
    _beginthreadex(nullptr, 0, RecvThread, nullptr, 0, nullptr);   // the only reader of this socket
    return true;
}

bool Host() {
    const LONG gen = InterlockedIncrement(&g_gen);
    InterlockedExchange(&g_started, 0); InterlockedExchange(&g_registered, 0); InterlockedExchange64(&g_room, 0);
    EnterCriticalSection(&g_lock);
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0].used = true; lstrcpynA(g_mem[0].name, g_cfg.name, Party::kNameLen); g_mem[0].seen = GetTickCount64();
    g_st.isHost = true; g_st.phase = HOST_SETUP; g_st.code[0] = 0; g_st.verdict[0] = 0; g_st.advice[0] = 0;
    LeaveCriticalSection(&g_lock);
    _beginthreadex(nullptr, 0, HostThread, (void*)(intptr_t)gen, 0, nullptr);
    return true;
}

bool Join(const char* code, char* err, size_t errN) {
    uint32_t ip = 0; uint16_t port = 0;
    if (!code || !LobbyCode::Decode(code, &ip, &port)) { _snprintf(err, errN, "That is not a valid code (12 characters, XXXX-XXXX-XXXX)"); return false; }
    if (Relay::IsRoomAddress(ip) && !g_cfg.relay[0]) { _snprintf(err, errN, "A Z... room code needs the host's relay server in Settings"); return false; }
    const LONG gen = InterlockedIncrement(&g_gen);
    InterlockedExchange(&g_joined, 0); InterlockedExchange(&g_peerInfo, 0);
    g_myNonce = Nonce(); g_lastFromHost = GetTickCount64();
    g_guestViaRelay = Relay::IsRoomAddress(ip);
    if (g_guestViaRelay) {
        if (!ResolveRelay()) { _snprintf(err, errN, "The relay server name did not resolve"); return false; }
        g_relayNonce = Nonce();
        InterlockedExchange64(&g_room, (LONG64)Relay::RoomFromAddress(ip, port));
    } else {
        g_hostEp = Sa(ip, port);
    }
    char canon[15] = {0}; LobbyCode::Encode(ip, port, canon);
    EnterCriticalSection(&g_lock);
    g_st.isHost = false; g_st.phase = JOINING; lstrcpynA(g_st.code, canon, sizeof(g_st.code)); g_st.count = 0;
    LeaveCriticalSection(&g_lock);
    _beginthreadex(nullptr, 0, GuestThread, (void*)(intptr_t)gen, 0, nullptr);
    return true;
}

bool StartGame() {
    EnterCriticalSection(&g_lock);
    if (!g_st.isHost || g_st.phase != HOSTING) { LeaveCriticalSection(&g_lock); return false; }
    g_start = Party::Start{};
    g_start.gameProto = g_cfg.gameProto;
    lstrcpynA(g_start.joinCode, g_st.code, sizeof(g_start.joinCode));
    g_start.relayRoom = InterlockedCompareExchange(&g_registered, 0, 0) ? (uint64_t)InterlockedCompareExchange64(&g_room, 0, 0) : 0;
    lstrcpynA(g_start.relayServer, g_start.relayRoom ? g_cfg.relay : "", sizeof(g_start.relayServer));
    g_start.startSeq = Nonce();
    g_st.start = g_start; g_st.phase = STARTED;
    lstrcpynA(g_st.status, "Starting the game for everyone...", sizeof(g_st.status));
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_started, 1);
    _beginthreadex(nullptr, 0, StartThread, nullptr, 0, nullptr);
    return true;
}

void Leave() {
    EnterCriticalSection(&g_lock); const bool host = g_st.isHost; const Phase ph = g_st.phase; Mem copy[Party::kMaxMembers]; memcpy(copy, g_mem, sizeof(copy)); LeaveCriticalSection(&g_lock);
    if (host && (ph == HOSTING || ph == STARTED)) { for (int i = 1; i < Party::kMaxMembers; ++i) if (copy[i].used) PartySend(copy[i], Party::T_LEAVE, 0, nullptr, 0); }
    else if (!host && (ph == JOINED || ph == JOINING)) ToHost(Party::T_LEAVE, nullptr, 0);
    InterlockedIncrement(&g_gen);
    EnterCriticalSection(&g_lock);
    const int bp = g_st.boundPort; const bool gp = g_st.portIsGamePort;
    g_st = State{}; g_st.boundPort = bp; g_st.portIsGamePort = gp; memset(g_mem, 0, sizeof(g_mem));
    LeaveCriticalSection(&g_lock);
}

State Snapshot() { EnterCriticalSection(&g_lock); State s = g_st; LeaveCriticalSection(&g_lock); return s; }

void Shutdown() {
    InterlockedExchange(&g_run, 0); InterlockedIncrement(&g_gen);
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
}

}  // namespace PartyNet

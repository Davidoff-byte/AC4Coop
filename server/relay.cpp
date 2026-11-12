// ac4coop-relay - rendezvous + relay server for AC4Coop lobby codes (docs/design/relay.md).
// Needs neither the game nor any store client. One UDP port. Single file, portable:
//   Windows (x86/x64 Developer prompt):  cl /nologo /O2 /EHsc /std:c++17 /I src server\relay.cpp /Fe:bin\ac4coop-relay.exe ws2_32.lib bcrypt.lib
//   Linux:                               g++ -O2 -std=c++17 -I src server/relay.cpp -o ac4coop-relay
// Usage: ac4coop-relay [--port 47800] [--max-rooms 256] [--quiet]
//
// What it does: a host REGISTERs and gets a room id (shown to players as a 'Z...' lobby code). A guest
// JOINs the room; the server sends each side the other's public and LAN endpoints (T_PEER) so they can
// connect directly (hole punch). If that fails, members send T_DATA to the server and it forwards to the
// addressed member. It only forwards between members of the same room, never to arbitrary addresses,
// and an unauthenticated request never gets a reply larger than itself.
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <bcrypt.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#    pragma comment(lib, "bcrypt.lib")
#  endif
   typedef int socklen_t;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <ctime>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define closesocket close
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <map>
#include "relay_proto.h"

using namespace Relay;

namespace {

bool g_quiet = false;

uint64_t NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

void Log(const char* fmt, ...) {
    char ts[32];
    time_t t = time(nullptr); struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
    printf("%s ", ts);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}

bool RandomBytes(void* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const bool ok = fread(out, 1, n, f) == n;
    fclose(f);
    return ok;
#endif
}

struct Endpoint { uint32_t ip; uint16_t port; };   // host order
uint64_t Key(const Endpoint& e) { return ((uint64_t)e.ip << 16) | e.port; }
const char* Str(const Endpoint& e, char* b) {
    sprintf(b, "%u.%u.%u.%u:%u", e.ip >> 24, (e.ip >> 16) & 0xFF, (e.ip >> 8) & 0xFF, e.ip & 0xFF, e.port);
    return b;
}

struct Member {
    bool     used = false;
    Endpoint pub{};
    uint32_t lanIp = 0;
    uint16_t lanPort = 0;
    uint32_t nonce = 0;
    uint64_t lastSeen = 0;
};
struct Room {
    uint32_t gameProto = 0;
    Member   m[kMaxMembers];
    uint64_t created = 0;
};
struct Bucket { double tokens = 0; uint64_t last = 0; };

std::map<uint64_t, Room>  g_rooms;
std::map<uint64_t, std::pair<uint64_t, uint16_t>> g_byEndpoint;   // endpoint -> (room, slot)
std::map<uint64_t, Bucket> g_buckets;
SOCKET  g_sock = INVALID_SOCKET;
int     g_maxRooms = 256;
uint64_t g_fwdPkts = 0, g_fwdBytes = 0, g_dropRate = 0, g_dropBad = 0;

const double kRatePerSec = 400.0;   // per source endpoint; a player sends ~20-60 datagrams/s
const double kBurst      = 800.0;
const uint64_t kGuestTimeoutMs = 30000;
const uint64_t kHostTimeoutMs  = 60000;

bool RateOk(const Endpoint& e, uint64_t now) {
    Bucket& b = g_buckets[Key(e)];
    if (b.last == 0) { b.tokens = kBurst; b.last = now; }
    b.tokens += (double)(now - b.last) * kRatePerSec / 1000.0;
    if (b.tokens > kBurst) b.tokens = kBurst;
    b.last = now;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

void SendTo(const Endpoint& to, const void* buf, size_t n) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(to.ip); a.sin_port = htons(to.port);
    sendto(g_sock, (const char*)buf, (int)n, 0, (const sockaddr*)&a, sizeof(a));
}

void Reply(const Endpoint& to, uint8_t type, uint16_t slot, uint64_t room, const void* payload, size_t pn) {
    uint8_t buf[64];
    Hdr h{ kMagic, kVersion, type, slot, room };
    memcpy(buf, &h, sizeof(h));
    if (pn) memcpy(buf + sizeof(h), payload, pn);
    SendTo(to, buf, sizeof(h) + pn);
}

void SendErr(const Endpoint& to, uint64_t room, uint16_t code, uint32_t nonce) {
    Error e{ code, 0, nonce };
    Reply(to, T_ERR, 0, room, &e, sizeof(e));
}

void SendPeer(const Endpoint& to, uint64_t room, uint16_t slotOfOther, const Member& other, uint32_t nonce) {
    Peer p{ other.pub.ip, other.pub.port, 0, other.lanIp, other.lanPort, 0, nonce };
    Reply(to, T_PEER, slotOfOther, room, &p, sizeof(p));
}

void RemoveMember(uint64_t roomId, Room& r, int slot, const char* why) {
    char b[32];
    if (!r.m[slot].used) return;
    g_byEndpoint.erase(Key(r.m[slot].pub));
    if (!g_quiet) Log("room %010llx slot %d %s left (%s)\n", (unsigned long long)roomId, slot, Str(r.m[slot].pub, b), why);
    r.m[slot] = Member{};
}

uint64_t NewRoomId() {
    for (int tries = 0; tries < 64; ++tries) {
        uint64_t v = 0;
        if (!RandomBytes(&v, sizeof(v))) v = NowMs() * 6364136223846793005ull + 1442695040888963407ull;
        v &= 0xFFFFFFFFFFull;   // 40 bits
        if (v != 0 && g_rooms.find(v) == g_rooms.end()) return v;
    }
    return 0;
}

void OnRegister(const Endpoint& src, const Hdr& h, const Hello& hi, uint64_t now) {
    char b[32];
    uint64_t roomId = h.room & 0xFFFFFFFFFFull;
    auto it = g_byEndpoint.find(Key(src));
    if (roomId == 0 && it != g_byEndpoint.end() && it->second.second == kHostSlot) roomId = it->second.first;   // retry
    auto r = roomId ? g_rooms.find(roomId) : g_rooms.end();
    if (r != g_rooms.end()) {
        Member& host = r->second.m[kHostSlot];
        if (!host.used || Key(host.pub) != Key(src)) {
            // Same room id, different host endpoint: the host's NAT mapping changed (or someone guesses ids).
            // Accept only if the old host has been silent past half the timeout.
            if (host.used && now - host.lastSeen < kHostTimeoutMs / 2) { SendErr(src, roomId, E_UNKNOWN_ROOM, hi.nonce); return; }
            if (host.used) g_byEndpoint.erase(Key(host.pub));
            host.used = true; host.pub = src;
            g_byEndpoint[Key(src)] = { roomId, kHostSlot };
            if (!g_quiet) Log("room %010llx host moved to %s\n", (unsigned long long)roomId, Str(src, b));
        }
        host.lanIp = hi.lanIp; host.lanPort = hi.lanPort; host.lastSeen = now; host.nonce = hi.nonce;
        r->second.gameProto = hi.gameProto;
    } else {
        if ((int)g_rooms.size() >= g_maxRooms) { SendErr(src, 0, E_ROOM_FULL, hi.nonce); return; }
        if (it != g_byEndpoint.end()) { SendErr(src, roomId, E_UNKNOWN_ROOM, hi.nonce); return; }   // is a guest elsewhere
        // A non-zero unknown id is re-created as is: the server restarted and the host's code still stands.
        if (roomId == 0) roomId = NewRoomId();
        if (roomId == 0) { SendErr(src, 0, E_ROOM_FULL, hi.nonce); return; }
        Room room; room.gameProto = hi.gameProto; room.created = now;
        Member& host = room.m[kHostSlot];
        host.used = true; host.pub = src; host.lanIp = hi.lanIp; host.lanPort = hi.lanPort; host.lastSeen = now; host.nonce = hi.nonce;
        g_rooms[roomId] = room;
        g_byEndpoint[Key(src)] = { roomId, kHostSlot };
        if (!g_quiet) Log("room %010llx created by %s (game proto %u)%s\n", (unsigned long long)roomId, Str(src, b), hi.gameProto,
                          h.room ? " [re-created after restart]" : "");
    }
    Registered rg{ hi.nonce, 0 };
    Reply(src, T_REGISTERED, kHostSlot, roomId, &rg, sizeof(rg));
}

void OnJoin(const Endpoint& src, const Hdr& h, const Hello& hi, uint64_t now) {
    char b[32];
    const uint64_t roomId = h.room & 0xFFFFFFFFFFull;
    auto r = g_rooms.find(roomId);
    if (r == g_rooms.end() || !r->second.m[kHostSlot].used) { SendErr(src, roomId, E_UNKNOWN_ROOM, hi.nonce); return; }
    Room& room = r->second;
    if (hi.gameProto != room.gameProto) { SendErr(src, roomId, E_PROTO, hi.nonce); return; }
    int slot = -1;
    auto it = g_byEndpoint.find(Key(src));
    if (it != g_byEndpoint.end()) {
        if (it->second.first != roomId || it->second.second == kHostSlot) { SendErr(src, roomId, E_NOT_MEMBER, hi.nonce); return; }
        slot = it->second.second;   // a retry: answer again with the same slot
    } else {
        for (int s = 1; s < kMaxMembers; ++s) if (!room.m[s].used) { slot = s; break; }
        if (slot < 0) { SendErr(src, roomId, E_ROOM_FULL, hi.nonce); return; }
        g_byEndpoint[Key(src)] = { roomId, (uint16_t)slot };
        if (!g_quiet) Log("room %010llx slot %d joined by %s\n", (unsigned long long)roomId, slot, Str(src, b));
    }
    Member& g = room.m[slot];
    g.used = true; g.pub = src; g.lanIp = hi.lanIp; g.lanPort = hi.lanPort; g.lastSeen = now; g.nonce = hi.nonce;
    SendPeer(src, roomId, kHostSlot, room.m[kHostSlot], hi.nonce);                 // guest learns the host
    SendPeer(room.m[kHostSlot].pub, roomId, (uint16_t)slot, g, hi.nonce);           // host learns the guest
}

void OnKeepalive(const Endpoint& src, const Hdr& h, uint64_t now) {
    auto it = g_byEndpoint.find(Key(src));
    if (it == g_byEndpoint.end() || it->second.first != (h.room & 0xFFFFFFFFFFull)) { ++g_dropBad; return; }   // no reply: no amplification
    g_rooms[it->second.first].m[it->second.second].lastSeen = now;
}

void OnData(const Endpoint& src, const Hdr& h, const uint8_t* buf, int n, uint64_t now) {
    auto it = g_byEndpoint.find(Key(src));
    if (it == g_byEndpoint.end() || it->second.first != (h.room & 0xFFFFFFFFFFull)) { ++g_dropBad; return; }
    Room& room = g_rooms[it->second.first];
    const uint16_t srcSlot = it->second.second;
    room.m[srcSlot].lastSeen = now;
    uint8_t out[kMaxDatagram];
    memcpy(out, buf, n);
    Hdr fh = h; fh.slot = srcSlot;
    memcpy(out, &fh, sizeof(fh));
    for (int s = 0; s < kMaxMembers; ++s) {
        if (s == srcSlot || !room.m[s].used) continue;
        if (h.slot != kAllSlots && h.slot != s) continue;
        SendTo(room.m[s].pub, out, (size_t)n);
        ++g_fwdPkts; g_fwdBytes += (uint64_t)n;
    }
}

void Expire(uint64_t now) {
    for (auto r = g_rooms.begin(); r != g_rooms.end(); ) {
        Room& room = r->second;
        for (int s = 1; s < kMaxMembers; ++s)
            if (room.m[s].used && now - room.m[s].lastSeen > kGuestTimeoutMs) RemoveMember(r->first, room, s, "silent 30 s");
        if (now - room.m[kHostSlot].lastSeen > kHostTimeoutMs) {
            for (int s = 1; s < kMaxMembers; ++s) RemoveMember(r->first, room, s, "room closed");
            g_byEndpoint.erase(Key(room.m[kHostSlot].pub));
            if (!g_quiet) Log("room %010llx closed (host silent 60 s)\n", (unsigned long long)r->first);
            r = g_rooms.erase(r);
        } else ++r;
    }
    for (auto b = g_buckets.begin(); b != g_buckets.end(); ) b = (now - b->second.last > 60000) ? g_buckets.erase(b) : std::next(b);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 47800;
    uint64_t statsMs = 60000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats-sec") && i + 1 < argc) statsMs = (uint64_t)atoi(argv[++i]) * 1000u;
        else if (!strcmp(argv[i], "--max-rooms") && i + 1 < argc) g_maxRooms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) g_quiet = true;
        else { printf("usage: ac4coop-relay [--port 47800] [--max-rooms 256] [--stats-sec 60] [--quiet]\n"); return 2; }
    }
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY); me.sin_port = htons((uint16_t)port);
    if (g_sock == INVALID_SOCKET || bind(g_sock, (sockaddr*)&me, sizeof(me)) != 0) { Log("cannot bind UDP %d\n", port); return 1; }
    Log("ac4coop-relay listening on UDP %d (relay protocol %u, max %d rooms, %d members each)\n", port, kVersion, g_maxRooms, kMaxMembers);
    uint64_t lastExpire = NowMs(), lastStats = NowMs();
    uint8_t buf[kMaxDatagram + 1];
    for (;;) {
        fd_set rs; FD_ZERO(&rs); FD_SET(g_sock, &rs);
        timeval tv{ 1, 0 };
        const int sel = select((int)g_sock + 1, &rs, nullptr, nullptr, &tv);
        const uint64_t now = NowMs();
        if (sel > 0) {
            sockaddr_in from{}; socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n >= (int)sizeof(Hdr) && n <= kMaxDatagram) {
                const Endpoint src{ ntohl(from.sin_addr.s_addr), ntohs(from.sin_port) };
                Hdr h; memcpy(&h, buf, sizeof(h));
                if (h.magic != kMagic) ++g_dropBad;
                else if (!RateOk(src, now)) ++g_dropRate;
                else if (h.version != kVersion) {
                    if (n >= (int)(sizeof(Hdr) + sizeof(Hello)) && (h.type == T_REGISTER || h.type == T_JOIN)) {
                        Hello hi; memcpy(&hi, buf + sizeof(Hdr), sizeof(hi)); SendErr(src, h.room, E_VERSION, hi.nonce);
                    } else ++g_dropBad;
                } else if ((h.type == T_REGISTER || h.type == T_JOIN) && n >= (int)(sizeof(Hdr) + sizeof(Hello))) {
                    Hello hi; memcpy(&hi, buf + sizeof(Hdr), sizeof(hi));
                    if (h.type == T_REGISTER) OnRegister(src, h, hi, now); else OnJoin(src, h, hi, now);
                } else if (h.type == T_KEEPALIVE) OnKeepalive(src, h, now);
                else if (h.type == T_DATA) OnData(src, h, buf, n, now);
                else ++g_dropBad;
            } else if (n > 0) ++g_dropBad;
        }
        if (now - lastExpire >= 1000) { lastExpire = now; Expire(now); }
        if (now - lastStats >= statsMs) {
            lastStats = now;
            size_t members = 0; for (auto& r : g_rooms) for (auto& m : r.second.m) members += m.used ? 1 : 0;
            Log("stats rooms=%zu members=%zu forwarded=%llu pkts %llu B dropped rate=%llu bad=%llu\n", g_rooms.size(), members,
                (unsigned long long)g_fwdPkts, (unsigned long long)g_fwdBytes, (unsigned long long)g_dropRate, (unsigned long long)g_dropBad);
        }
    }
}

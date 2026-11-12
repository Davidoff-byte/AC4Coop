// Minimal STUN Binding client (lobby slice 5). See stun.h.
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "stun.h"

#pragma comment(lib, "ws2_32.lib")

namespace {
uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
}

namespace Stun {

void BuildRequest(uint8_t out[20], const uint8_t tid[12]) {
    out[0] = 0x00; out[1] = 0x01;          // Binding Request
    out[2] = 0x00; out[3] = 0x00;          // no attributes
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;
    std::memcpy(out + 8, tid, 12);
}

bool LooksLikeStun(const uint8_t* buf, int n) {
    return n >= 20 && (buf[0] & 0xC0) == 0 && Be32(buf + 4) == kMagicCookie && (Be16(buf + 2) & 3) == 0 &&
           20 + (int)Be16(buf + 2) <= n;
}

bool ParseResponse(const uint8_t* buf, int n, const uint8_t tid[12], uint32_t* ipHost, uint16_t* port) {
    if (!LooksLikeStun(buf, n)) return false;
    if (Be16(buf) != 0x0101) return false;                       // Binding Success Response
    if (std::memcmp(buf + 8, tid, 12) != 0) return false;        // not our transaction
    const int end = 20 + Be16(buf + 2);
    bool haveMapped = false; uint32_t mIp = 0; uint16_t mPort = 0;
    for (int i = 20; i + 4 <= end; ) {
        const uint16_t type = Be16(buf + i), len = Be16(buf + i + 2);
        const uint8_t* v = buf + i + 4;
        if (i + 4 + len > end) return false;
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {   // IPv4 family
            uint16_t p = Be16(v + 2); uint32_t a = Be32(v + 4);
            if (type == 0x0020) {                                   // XOR-MAPPED-ADDRESS wins
                *port = (uint16_t)(p ^ (kMagicCookie >> 16));
                *ipHost = a ^ kMagicCookie;
                return true;
            }
            haveMapped = true; mIp = a; mPort = p;                   // MAPPED-ADDRESS (RFC 3489 servers)
        }
        i += 4 + ((len + 3) & ~3);
    }
    if (haveMapped) { *ipHost = mIp; *port = mPort; return true; }
    return false;
}

bool Resolve(const char* hostPort, sockaddr_in* out) {
    char host[256]; lstrcpynA(host, hostPort, sizeof(host));
    unsigned port = 3478;
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (unsigned)atoi(colon + 1); }
    if (!host[0] || port == 0 || port > 65535) return false;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    *out = *(sockaddr_in*)res->ai_addr;
    out->sin_port = htons((u_short)port);
    freeaddrinfo(res);
    return true;
}

bool Query(SOCKET s, const sockaddr_in& server, int timeoutMs, uint32_t* ipHost, uint16_t* port) {
    uint8_t tid[12];
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint32_t seed = q.LowPart ^ GetCurrentProcessId();
    for (int i = 0; i < 12; ++i) { seed = seed * 1103515245u + 12345u; tid[i] = (uint8_t)(seed >> 16); }
    uint8_t req[20]; BuildRequest(req, tid);
    DWORD rto = 200; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rto, sizeof(rto));
    for (int attempt = 0; attempt < 3; ++attempt) {
        sendto(s, (const char*)req, sizeof(req), 0, (const sockaddr*)&server, sizeof(server));
        const ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < (ULONGLONG)timeoutMs) {
            uint8_t buf[1024]; sockaddr_in from{}; int fl = sizeof(from);
            int n = recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n > 0 && ParseResponse(buf, n, tid, ipHost, port)) return true;
        }
    }
    return false;
}

bool SelfTest(char* msg, size_t msgN) {
    // RFC 5769 section 2.2 "Sample IPv4 Response": header, SOFTWARE "test vector ", XOR-MAPPED-ADDRESS
    // 192.0.2.1:32853. The MESSAGE-INTEGRITY/FINGERPRINT attributes of the RFC sample are replaced by
    // zero filler of the same lengths: this parser does not verify them (no credentials in Binding use).
    static const uint8_t kResp[] = {
        0x01,0x01,0x00,0x3c, 0x21,0x12,0xa4,0x42, 0xb7,0xe7,0xa7,0x01,0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae,
        0x80,0x22,0x00,0x0b, 0x74,0x65,0x73,0x74,0x20,0x76,0x65,0x63,0x74,0x6f,0x72,0x20,
        0x00,0x20,0x00,0x08, 0x00,0x01,0xa1,0x47, 0xe1,0x12,0xa6,0x43,
        0x00,0x08,0x00,0x14, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x80,0x28,0x00,0x04, 0,0,0,0,
    };
    static const uint8_t kTid[12] = {0xb7,0xe7,0xa7,0x01,0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae};
    uint32_t ip = 0; uint16_t port = 0;
    const bool pos = ParseResponse(kResp, sizeof(kResp), kTid, &ip, &port) && ip == 0xC0000201u && port == 32853;
    uint8_t wrongTid[12]; std::memcpy(wrongTid, kTid, 12); wrongTid[11] ^= 1;
    const bool negTid = !ParseResponse(kResp, sizeof(kResp), wrongTid, &ip, &port);
    const bool negShort = !ParseResponse(kResp, 40, kTid, &ip, &port);          // truncated body
    uint8_t game[20] = {'A','C','4','C'};                                         // a game datagram
    const bool negGame = !LooksLikeStun(game, sizeof(game));
    uint8_t req[20]; BuildRequest(req, kTid);
    const bool reqOk = LooksLikeStun(req, 20) && req[1] == 0x01;
    const bool ok = pos && negTid && negShort && negGame && reqOk;
    _snprintf(msg, msgN, "[Lobby] stun selftest: %s (rfc5769=%d tid=%d short=%d game=%d req=%d)\n",
              ok ? "PASS" : "FAIL", pos, negTid, negShort, negGame, reqOk);
    msg[msgN - 1] = 0;
    return ok;
}

}  // namespace Stun

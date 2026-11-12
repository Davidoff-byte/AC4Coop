// Lobby-code format (slice 2 of docs/design/lobby-code.md). See lobby_code.h.
#define _CRT_SECURE_NO_WARNINGS
#include "lobby_code.h"
#include "lobby_code_vectors.h"
#include <cstdio>
#include <cstring>

namespace {
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford base32, 32 symbols

int SymbolValue(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int k = 0; k < 32; ++k) if (kAlphabet[k] == c) return k;
    return -1;
}

bool ParseIpv4(const char* s, uint32_t* out) {
    unsigned a, b, c, d; char tail;
    if (std::sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}
}  // namespace

namespace LobbyCode {

uint16_t Crc16CcittFalse(const uint8_t* data, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

bool Encode(uint32_t ip, uint16_t port, char out[15]) {
    if (!out) return false;
    const uint8_t raw[6] = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip,
                             (uint8_t)(port >> 8), (uint8_t)port };
    uint64_t value = 0;
    for (int i = 0; i < 6; ++i) value = (value << 8) | raw[i];
    value = (value << 12) | (Crc16CcittFalse(raw, 6) & 0x0FFFu);
    int o = 0;
    for (int i = 0; i < 12; ++i) {
        if (i == 4 || i == 8) out[o++] = '-';
        out[o++] = kAlphabet[(value >> (5 * (11 - i))) & 0x1F];
    }
    out[o] = '\0';
    return true;
}

bool Decode(const char* text, uint32_t* ip, uint16_t* port) {
    if (!text || !ip || !port) return false;
    uint64_t value = 0;
    int n = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '-' || *p == ' ') continue;
        const int k = SymbolValue(*p);
        if (k < 0 || n == 12) return false;      // foreign symbol, or more than 12 symbols
        value = (value << 5) | (uint64_t)k;
        ++n;
    }
    if (n != 12) return false;
    const uint64_t addr = value >> 12;
    const uint8_t raw[6] = { (uint8_t)(addr >> 40), (uint8_t)(addr >> 32), (uint8_t)(addr >> 24),
                             (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    if ((Crc16CcittFalse(raw, 6) & 0x0FFFu) != (uint32_t)(value & 0x0FFFu)) return false;
    *ip = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
    *port = (uint16_t)((raw[4] << 8) | raw[5]);
    return true;
}

bool SelfTest(char* msg, size_t msgSize) {
    using namespace LobbyCodeVectors;
    char why[160] = { 0 };
    int n = 0;
    {
        const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
        if (Crc16CcittFalse(check, sizeof(check)) != 0x29B1) std::snprintf(why, sizeof(why), "crc check value");
    }
    for (size_t i = 0; !why[0] && i < sizeof(kValid) / sizeof(kValid[0]); ++i, ++n) {
        uint32_t ip = 0, dip = 0; uint16_t dport = 0; char code[15];
        if (!ParseIpv4(kValid[i].ip, &ip) || !Encode(ip, kValid[i].port, code) || std::strcmp(code, kValid[i].code) != 0)
            std::snprintf(why, sizeof(why), "encode %s:%u", kValid[i].ip, (unsigned)kValid[i].port);
        else if (!Decode(kValid[i].code, &dip, &dport) || dip != ip || dport != kValid[i].port)
            std::snprintf(why, sizeof(why), "decode %s", kValid[i].code);
    }
    for (size_t i = 0; !why[0] && i < sizeof(kAccept) / sizeof(kAccept[0]); ++i, ++n) {
        uint32_t ip = 0, dip = 0; uint16_t dport = 0;
        if (!ParseIpv4(kAccept[i].ip, &ip) || !Decode(kAccept[i].text, &dip, &dport) || dip != ip || dport != kAccept[i].port)
            std::snprintf(why, sizeof(why), "accept '%s'", kAccept[i].text);
    }
    for (size_t i = 0; !why[0] && i < sizeof(kReject) / sizeof(kReject[0]); ++i, ++n) {
        uint32_t dip = 0; uint16_t dport = 0;
        if (Decode(kReject[i], &dip, &dport)) std::snprintf(why, sizeof(why), "reject '%s'", kReject[i]);
    }
    // Negative control on the first valid code: every single-symbol substitution must be rejected.
    if (!why[0]) {
        char code[15]; std::strcpy(code, kValid[0].code);
        for (int pos = 0; pos < 14 && !why[0]; ++pos) {
            if (code[pos] == '-') continue;
            const char orig = code[pos];
            for (int k = 0; k < 32 && !why[0]; ++k) {
                if (kAlphabet[k] == orig) continue;
                code[pos] = kAlphabet[k];
                uint32_t dip; uint16_t dport;
                if (Decode(code, &dip, &dport)) std::snprintf(why, sizeof(why), "substitution '%s' decoded", code);
            }
            code[pos] = orig;
        }
        ++n;
    }
    if (msg && msgSize) {
        if (why[0]) std::snprintf(msg, msgSize, "[Lobby] code selftest: FAIL %s\n", why);
        else        std::snprintf(msg, msgSize, "[Lobby] code selftest: PASS n=%d\n", n);
    }
    return why[0] == 0;
}

}  // namespace LobbyCode

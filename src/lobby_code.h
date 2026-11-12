#pragma once
// Lobby codes: an IPv4 address + UDP port as 12 Crockford-base32 characters, shown XXXX-XXXX-XXXX.
// Spec: docs/LEDGER.md slice 2, docs/design/lobby-code.md. Python twin: tools/lobby_code.py (same vectors,
// tools/testdata/lobby_codes.txt -> generated src/lobby_code_vectors.h). Pure functions: no networking, no ini.
#include <cstdint>
#include <cstddef>

namespace LobbyCode {

// out receives "XXXX-XXXX-XXXX" + NUL (15 bytes). ipv4_hostorder: a.b.c.d == (a<<24)|(b<<16)|(c<<8)|d.
bool Encode(uint32_t ipv4_hostorder, uint16_t port, char out[15]);

// Accepts either case, ignores '-' and spaces, maps O->0 and I/L->1. Exactly 12 symbols and a matching
// 12-bit check, else false (outputs untouched).
bool Decode(const char* text, uint32_t* ipv4_hostorder, uint16_t* port);

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final xor). Exposed for the selftest.
uint16_t Crc16CcittFalse(const uint8_t* data, size_t n);

// Runs every vector of the generated table plus the negative controls. Writes one line
// "[Lobby] code selftest: PASS n=<count>" or "[Lobby] code selftest: FAIL <which>" into msg.
bool SelfTest(char* msg, size_t msgSize);

}  // namespace LobbyCode

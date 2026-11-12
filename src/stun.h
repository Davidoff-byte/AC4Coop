#pragma once
// Minimal STUN (RFC 5389) Binding client for lobby host mode (docs/design/lobby-code.md, slice 5):
// learns the address the internet sees for a UDP socket. No engine calls.
#include <winsock2.h>
#include <cstdint>
#include <cstddef>

namespace Stun {

static const uint32_t kMagicCookie = 0x2112A442;   // RFC 5389 section 6

// 20-byte Binding Request with the given 12-byte transaction id.
void BuildRequest(uint8_t out[20], const uint8_t tid[12]);

// True when `buf` looks like a STUN message at all (top bits 00, cookie at bytes 4..8). Lets a socket
// that also carries game traffic route the datagram here.
bool LooksLikeStun(const uint8_t* buf, int n);

// Binding Success Response for `tid` -> the XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS) IPv4, host order.
bool ParseResponse(const uint8_t* buf, int n, const uint8_t tid[12], uint32_t* ipHost, uint16_t* port);

// "host:port" (default port 3478 if none) -> IPv4 sockaddr. Blocking DNS.
bool Resolve(const char* hostPort, sockaddr_in* out);

// Standalone blocking query on a socket nobody else reads (tools/netcheck). Up to 3 tries.
bool Query(SOCKET s, const sockaddr_in& server, int timeoutMs, uint32_t* ipHost, uint16_t* port);

// Parses the RFC 5769 section 2.2 sample IPv4 response (192.0.2.1:32853) plus negatives.
bool SelfTest(char* msg, size_t msgN);

}  // namespace Stun

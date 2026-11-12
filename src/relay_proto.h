#pragma once
// ac4coop-relay wire protocol (docs/design/relay.md). Shared by the plugin (src/net.cpp) and the server
// (server/relay.cpp), so the two cannot drift; tools/relay_peer.py pins the same numbers and its
// --selftest fails if this file changes under it. No engine types: this header must compile on Linux.
//
// Every relay datagram starts with RelayHdr (16 bytes, little-endian, packed). It travels on the game's
// own UDP socket, so the endpoint the server observes is the NAT mapping a peer must reach.
#include <cstdint>

namespace Relay {

static const uint32_t kMagic   = 0x52344341;   // 'AC4R' (bytes 'A','C','4','R' on the wire)
static const uint8_t  kVersion = 1;            // relay protocol version (independent of NET_PROTO_VER)
static const uint16_t kHostSlot = 0;           // the room's host is always slot 0
static const uint16_t kAllSlots = 0xFFFF;      // DATA destination: every other member
static const int      kMaxMembers = 8;         // host + 7 guests (the plugin itself is 2-player for now)
static const int      kMaxDatagram = 1500;     // header + payload; larger datagrams are dropped

enum Type : uint8_t {
    T_REGISTER   = 1,   // host -> server. room = 0: new room; room = id: keepalive / re-register
    T_REGISTERED = 2,   // server -> host. room = id
    T_JOIN       = 3,   // guest -> server. room = id (repeat until PEER/ERROR)
    T_PEER       = 4,   // server -> member: another member's endpoints (sent to both sides of a join)
    T_DATA       = 5,   // member -> server: slot = destination; server -> member: slot = source
    T_ERR        = 6,   // server -> sender: RelayError payload
    T_KEEPALIVE  = 7,   // guest -> server: room = id, keeps the membership and the NAT mapping alive
};

enum ErrorCode : uint16_t {
    E_UNKNOWN_ROOM = 1,   // no such room (expired, mistyped, or the server restarted)
    E_ROOM_FULL    = 2,
    E_PROTO        = 3,   // the joiner's game protocol differs from the host's
    E_VERSION      = 4,   // relay protocol version differs
    E_NOT_MEMBER   = 5,   // DATA/KEEPALIVE from an endpoint that is not in the room
};

#pragma pack(push, 1)
struct Hdr {             // 16 B
    uint32_t magic;      // kMagic
    uint8_t  version;    // kVersion
    uint8_t  type;       // Type
    uint16_t slot;       // DATA: destination (client->server) / source (server->client); PEER: that peer's slot
    uint64_t room;       // room id (low 40 bits used); 0 in a first REGISTER
};
struct Hello {           // REGISTER / JOIN payload, 24 B (JOIN padded so a reply is never larger than it)
    uint32_t gameProto;  // NET_PROTO_VER of the sender's build
    uint32_t lanIp;      // sender's LAN IPv4, host order (0 = unknown)
    uint16_t lanPort;    // sender's bound port
    uint16_t reserved0;
    uint32_t nonce;      // echoed by REGISTERED / PEER / ERR so a reply is matched to its request
    uint32_t reserved1;
    uint32_t reserved2;
};
struct Registered {      // REGISTERED payload, 8 B
    uint32_t nonce;
    uint32_t reserved;
};
struct Peer {            // PEER payload, 20 B
    uint32_t pubIp;      // that member's endpoint as the server sees it (host order)
    uint16_t pubPort;
    uint16_t reserved0;
    uint32_t lanIp;      // what that member reported (same-network shortcut)
    uint16_t lanPort;
    uint16_t reserved1;
    uint32_t nonce;      // the JOIN nonce of this join
};
struct Error {           // ERR payload, 8 B
    uint16_t code;       // ErrorCode
    uint16_t reserved;
    uint32_t nonce;
};
#pragma pack(pop)

static_assert(sizeof(Hdr) == 16, "relay header is a wire layout");
static_assert(sizeof(Hello) == 24, "relay hello is a wire layout");
static_assert(sizeof(Registered) == 8, "relay registered is a wire layout");
static_assert(sizeof(Peer) == 20, "relay peer is a wire layout");
static_assert(sizeof(Error) == 8, "relay error is a wire layout");

// A room code is a lobby code (src/lobby_code.h) whose IPv4 first octet is 255 - 255.0.0.0/8 is reserved,
// so no direct code ever uses it - carrying the 40-bit room id in the other 24 address bits + the 16 port
// bits. Room codes therefore always start with 'Z'.
inline bool IsRoomAddress(uint32_t ipHost) { return (ipHost >> 24) == 0xFF; }
inline uint64_t RoomFromAddress(uint32_t ipHost, uint16_t port) {
    return ((uint64_t)(ipHost & 0x00FFFFFFu) << 16) | port;
}
inline void AddressFromRoom(uint64_t room, uint32_t* ipHost, uint16_t* port) {
    *ipHost = 0xFF000000u | (uint32_t)((room >> 16) & 0x00FFFFFFu);
    *port = (uint16_t)(room & 0xFFFFu);
}

}  // namespace Relay

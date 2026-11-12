#pragma once
// Party protocol: the launcher's pre-game lobby (launcher/, docs/design/launcher.md). Shared by the launcher and
// tools/party_peer.py (whose --selftest pins every number here). No engine types; builds anywhere.
//
// A host launcher hosts a party on the same UDP port its game will use; guests JOIN with the host's lobby code
// (direct) or room code (Z..., through the relay as relay DATA). The host keeps the roster and broadcasts it;
// START tells every guest launcher to configure and launch its game.
#include <cstdint>

namespace Party {

static const uint32_t kMagic      = 0x50344341;   // 'AC4P' ('A','C','4','P' on the wire)
static const uint8_t  kVersion    = 1;
static const int      kMaxMembers = 8;            // host + 7 (the game itself is 2-player for now)
static const int      kNameLen    = 24;           // UTF-8, zero-padded

enum Type : uint8_t {
    T_JOIN   = 1,   // guest -> host, repeated until ROSTER/REFUSE (payload Join, padded: no reply is larger)
    T_ROSTER = 2,   // host -> every member when the roster changes, and as the answer to JOIN / PING
    T_START  = 3,   // host -> every guest, repeated a few times (payload Start)
    T_LEAVE  = 4,   // either way: a member leaves / the host closes the party
    T_PING   = 5,   // guest -> host every 2 s (keeps the slot and NAT mappings); host answers with ROSTER
    T_REFUSE = 6,   // host -> guest (payload Refuse)
};
enum RefuseCode : uint16_t { R_FULL = 1, R_PROTO = 2, R_STARTED = 3 };

#pragma pack(push, 1)
struct Hdr {             // 12 B
    uint32_t magic;      // kMagic
    uint8_t  version;    // kVersion
    uint8_t  type;       // Type
    uint16_t reserved;
    uint32_t nonce;      // the guest's per-launch id (JOIN/PING/LEAVE); echoed in REFUSE; 0 from the host otherwise
};
struct Join {            // 260 B: larger than the largest reply (ROSTER, 228 B), so a JOIN never amplifies
    uint32_t gameProto;  // NET_PROTO_VER of the build the launcher ships
    char     name[kNameLen];
    uint8_t  pad[232];
};
struct Member {          // 28 B
    char     name[kNameLen];
    uint8_t  slot;       // 0 = host
    uint8_t  inGame;     // reserved for later (1 = this member's game reported in-world)
    uint16_t reserved;
};
struct Roster {          // 228 B
    uint8_t  count;
    uint8_t  maxPlay;    // how many of them the game supports right now (2)
    uint16_t reserved;
    Member   m[kMaxMembers];
};
struct Start {           // 100 B
    uint32_t gameProto;
    char     joinCode[16];     // the code guests' games join (the host's lobby/room code)
    uint64_t relayRoom;        // 0 = no relay
    char     relayServer[64];  // as typed in the host's launcher ("" = none)
    uint32_t startSeq;         // same value in every repeat
    uint32_t reserved;
};
struct Refuse {          // 4 B
    uint16_t code;       // RefuseCode
    uint16_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Hdr) == 12, "party header is a wire layout");
static_assert(sizeof(Join) == 260, "party join is a wire layout");
static_assert(sizeof(Member) == 28, "party member is a wire layout");
static_assert(sizeof(Roster) == 228, "party roster is a wire layout");
static_assert(sizeof(Start) == 100, "party start is a wire layout");
static_assert(sizeof(Refuse) == 4, "party refuse is a wire layout");
static_assert(sizeof(Hdr) + sizeof(Join) >= sizeof(Hdr) + sizeof(Roster), "JOIN must not be smaller than its ROSTER reply");

}  // namespace Party

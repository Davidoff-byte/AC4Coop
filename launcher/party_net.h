#pragma once
// The launcher's party (pre-game lobby) networking. One UDP socket on the port the game will use; a host role
// (UPnP + STUN + optional relay room -> code, roster, START) and a guest role (direct or relay join, PING, START).
// All blocking work runs on its own threads; the UI reads Snapshot().
#include <cstdint>
#include "party_proto.h"

namespace PartyNet {

enum Phase { IDLE, HOST_SETUP, HOSTING, JOINING, JOINED, STARTED, FAILED };

struct State {
    Phase    phase = IDLE;
    bool     isHost = false;
    char     code[16] = {0};          // host: the code to share; guest: the code joined
    char     lanCode[16] = {0};       // host: code for players on the same network (LAN address)
    char     verdict[24] = {0};       // host: OK / PUBLIC / DOUBLE_NAT / CGNAT / NO_UPNP / NO_STUN / RELAY / LAN
    char     advice[200] = {0};       // host: plain-words advice for the verdict
    char     status[256] = {0};       // one line for the UI
    int      count = 0;               // roster size (incl. host)
    int      maxPlay = 2;
    Party::Member members[Party::kMaxMembers] = {};
    Party::Start  start = {};         // valid when phase == STARTED
    int      boundPort = 0;
    bool     portIsGamePort = true;   // false: the game port was busy and an ephemeral port is used (test / 2nd launcher)
};

struct Config {
    int      port = 47801;            // [Net] LocalPort of the game
    char     name[Party::kNameLen] = "Player";
    char     relay[128] = {0};        // "host[:port]" or ""
    uint32_t gameProto = 12;          // NET_PROTO_VER of the shipped build
    bool     upnp = true;
    char     stunServer[128] = "stun.l.google.com:19302";
};

bool  Init(const Config& c, char* err, size_t errN);   // bind the socket (falls back to an ephemeral port)
bool  Host();                                          // start hosting (async setup -> HOSTING)
bool  Join(const char* code, char* err, size_t errN);  // join a party by code (async -> JOINED)
bool  StartGame();                                     // host: send START to everyone (async) -> STARTED
void  Leave();                                         // leave / close the party, back to IDLE
State Snapshot();
void  Shutdown();                                      // release the port (call before launching the game)

}  // namespace PartyNet

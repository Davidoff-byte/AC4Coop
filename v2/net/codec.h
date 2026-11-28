#pragma once
// Protocol v2 wire format (docs/v2/ARCHITECTURE.md s5). Pure code: no sockets, no engine, no logging; unit-tested in
// v2/tests. Every datagram is a 16-byte header followed by one body of the header's type. All integers little-endian.
#include "wire.h"

namespace v2::net {

constexpr uint32_t kMagic       = 0x32344341u;   // the bytes 'A' 'C' '4' '2'
constexpr uint16_t kProto       = 1;
constexpr size_t   kHeaderBytes = 16;
constexpr size_t   kMaxDatagram = 1200;          // under every common path MTU: no IP fragmentation
constexpr uint8_t  kNoPeer      = 0xFF;          // sender id before WELCOME assigned one
constexpr size_t   kMaxPayload  = 64;            // per state record / per event

enum class Type : uint8_t { Hello = 1, Welcome = 2, Bye = 3, Clock = 4, State = 5, Events = 6 };

// magic u32 | proto u16 | sender u8 | type u8 | seq u32 | evAck u32
struct Header {
    uint16_t proto  = kProto;
    uint8_t  sender = kNoPeer;
    Type     type   = Type::Hello;
    uint32_t seq    = 0;   // this sender's datagram counter toward this receiver, from 1 (stale-drop, loss count)
    uint32_t evAck  = 0;   // cumulative: the receiver's events up to this seq were all delivered here
};
void WriteHeader(Writer& w, const Header& h);
bool ReadHeader(Reader& r, Header& h);   // false: short, not our magic, or another proto (h.proto says which)

// HELLO (joiner -> host), WELCOME (host -> joiner), BYE (either way).
struct Hello   { uint32_t nonce = 0; uint32_t build = 0; char name[16] = {}; };
struct Welcome { uint32_t nonce = 0; uint8_t peerId = 0; uint8_t players = 0; uint32_t session = 0; };
struct Bye     { uint8_t reason = 0; };
enum ByeReason : uint8_t { kByeQuit = 1, kByeFull = 2, kByeProto = 3, kByeTimeout = 4 };

// CLOCK: NTP-style ping/pong in each side's local microseconds. t0 = ping sent (pinger), t1 = ping received and
// t2 = pong sent (ponger); the pinger adds t3 = pong received.
struct Clock { uint8_t pong = 0; uint32_t id = 0; uint64_t t0 = 0, t1 = 0, t2 = 0; };

void WriteHello(Writer& w, const Hello& b);
bool ReadHello(Reader& r, Hello& b);
void WriteWelcome(Writer& w, const Welcome& b);
bool ReadWelcome(Reader& r, Welcome& b);
void WriteBye(Writer& w, const Bye& b);
bool ReadBye(Reader& r, Bye& b);
void WriteClock(Writer& w, const Clock& b);
bool ReadClock(Reader& r, Clock& b);

// STATE (unreliable, newest wins per object): stamp u64 (host-time us) | count u8 | records.
// A record is self-delimiting, so a receiver skips kinds it does not know: netId u32 | kind u8 | mask u16 | len u8 | data.
struct Record { uint32_t netId = 0; uint8_t kind = 0; uint16_t mask = 0; uint8_t len = 0; uint8_t data[kMaxPayload] = {}; };
constexpr size_t kStateHead      = 9;
constexpr size_t kRecordOverhead = 8;

// EVENTS (reliable, ordered): firstSeq u32 | count u8 | events, the contiguous run firstSeq .. firstSeq+count-1.
// An event: type u16 | netId u32 | len u8 | data. count 0 = a pure ack (the header's evAck).
struct Event { uint16_t type = 0; uint32_t netId = 0; uint8_t len = 0; uint8_t data[kMaxPayload] = {}; };
constexpr size_t kEventsHead    = 5;
constexpr size_t kEventOverhead = 7;

// Builders: Begin* writes the body head and returns the offset of its count; Add* appends one item only if it fits
// (false = full, nothing written); EndCount patches the count.
size_t BeginState(Writer& w, uint64_t stampUs);
size_t BeginEvents(Writer& w, uint32_t firstSeq);
bool   AddRecord(Writer& w, const Record& rec);
bool   AddEvent(Writer& w, const Event& ev);
inline void EndCount(Writer& w, size_t countOff, uint8_t count) { w.Patch8(countOff, count); }

bool ReadStateHead(Reader& r, uint64_t& stampUs, uint8_t& count);
bool ReadEventsHead(Reader& r, uint32_t& firstSeq, uint8_t& count);
bool ReadRecord(Reader& r, Record& rec);   // false: short or len > kMaxPayload
bool ReadEvent(Reader& r, Event& ev);      // false: short or len > kMaxPayload

}  // namespace v2::net

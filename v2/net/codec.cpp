#include "codec.h"

namespace v2::net {

void WriteHeader(Writer& w, const Header& h) {
    w.U32(kMagic); w.U16(h.proto); w.U8(h.sender); w.U8((uint8_t)h.type); w.U32(h.seq); w.U32(h.evAck);
}

bool ReadHeader(Reader& r, Header& h) {
    const uint32_t magic = r.U32();
    h.proto = r.U16(); h.sender = r.U8(); h.type = (Type)r.U8(); h.seq = r.U32(); h.evAck = r.U32();
    return r.ok() && magic == kMagic && h.proto == kProto;
}

void WriteHello(Writer& w, const Hello& b) { w.U32(b.nonce); w.U32(b.build); w.Bytes(b.name, sizeof(b.name)); }
bool ReadHello(Reader& r, Hello& b) {
    b.nonce = r.U32(); b.build = r.U32(); r.Bytes(b.name, sizeof(b.name));
    b.name[sizeof(b.name) - 1] = 0;
    return r.ok();
}

void WriteWelcome(Writer& w, const Welcome& b) { w.U32(b.nonce); w.U8(b.peerId); w.U8(b.players); w.U32(b.session); }
bool ReadWelcome(Reader& r, Welcome& b) {
    b.nonce = r.U32(); b.peerId = r.U8(); b.players = r.U8(); b.session = r.U32();
    return r.ok();
}

void WriteBye(Writer& w, const Bye& b) { w.U8(b.reason); }
bool ReadBye(Reader& r, Bye& b) { b.reason = r.U8(); return r.ok(); }

void WriteClock(Writer& w, const Clock& b) { w.U8(b.pong); w.U32(b.id); w.U64(b.t0); w.U64(b.t1); w.U64(b.t2); }
bool ReadClock(Reader& r, Clock& b) {
    b.pong = r.U8(); b.id = r.U32(); b.t0 = r.U64(); b.t1 = r.U64(); b.t2 = r.U64();
    return r.ok();
}

size_t BeginState(Writer& w, uint64_t stampUs) { w.U64(stampUs); const size_t off = w.size(); w.U8(0); return off; }
size_t BeginEvents(Writer& w, uint32_t firstSeq) { w.U32(firstSeq); const size_t off = w.size(); w.U8(0); return off; }

bool AddRecord(Writer& w, const Record& rec) {
    if (rec.len > kMaxPayload || w.room() < kRecordOverhead + rec.len) return false;
    w.U32(rec.netId); w.U8(rec.kind); w.U16(rec.mask); w.U8(rec.len); w.Bytes(rec.data, rec.len);
    return w.ok();
}

bool AddEvent(Writer& w, const Event& ev) {
    if (ev.len > kMaxPayload || w.room() < kEventOverhead + ev.len) return false;
    w.U16(ev.type); w.U32(ev.netId); w.U8(ev.len); w.Bytes(ev.data, ev.len);
    return w.ok();
}

bool ReadStateHead(Reader& r, uint64_t& stampUs, uint8_t& count) { stampUs = r.U64(); count = r.U8(); return r.ok(); }
bool ReadEventsHead(Reader& r, uint32_t& firstSeq, uint8_t& count) { firstSeq = r.U32(); count = r.U8(); return r.ok(); }

bool ReadRecord(Reader& r, Record& rec) {
    rec.netId = r.U32(); rec.kind = r.U8(); rec.mask = r.U16(); rec.len = r.U8();
    if (!r.ok() || rec.len > kMaxPayload) return false;
    r.Bytes(rec.data, rec.len);
    return r.ok();
}

bool ReadEvent(Reader& r, Event& ev) {
    ev.type = r.U16(); ev.netId = r.U32(); ev.len = r.U8();
    if (!r.ok() || ev.len > kMaxPayload) return false;
    r.Bytes(ev.data, ev.len);
    return r.ok();
}

}  // namespace v2::net

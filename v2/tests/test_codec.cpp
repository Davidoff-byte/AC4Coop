#include "test.h"
#include "../net/codec.h"

using namespace v2::net;

TEST(codec_header_roundtrip) {
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    Header h; h.sender = 3; h.type = Type::State; h.seq = 0xA1B2C3D4u; h.evAck = 77;
    WriteHeader(w, h);
    CHECK(w.ok() && w.size() == kHeaderBytes);
    CHECK(buf[0] == 'A' && buf[1] == 'C' && buf[2] == '4' && buf[3] == '2');
    Reader r(buf, w.size());
    Header g;
    CHECK(ReadHeader(r, g));
    CHECK(g.proto == kProto && g.sender == 3 && g.type == Type::State && g.seq == 0xA1B2C3D4u && g.evAck == 77);
}

TEST(codec_header_rejects) {
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    Header h; WriteHeader(w, h);
    for (size_t n = 0; n < kHeaderBytes; ++n) { Reader r(buf, n); Header g; CHECK(!ReadHeader(r, g)); }   // every short read
    buf[0] = 'X';
    { Reader r(buf, kHeaderBytes); Header g; CHECK(!ReadHeader(r, g)); }                                      // magic
    buf[0] = 'A'; buf[4] = 9;
    { Reader r(buf, kHeaderBytes); Header g; CHECK(!ReadHeader(r, g)); CHECK(g.proto == 9); }               // proto
}

TEST(codec_bodies_roundtrip) {
    uint8_t buf[256];
    {
        Writer w(buf, sizeof(buf));
        Hello a; a.nonce = 0xDEADBEEF; a.build = 0x581619DC; memcpy(a.name, "host-a", 7);
        WriteHello(w, a);
        Reader r(buf, w.size()); Hello b; CHECK(ReadHello(r, b));
        CHECK(b.nonce == a.nonce && b.build == a.build && strcmp(b.name, "host-a") == 0 && r.left() == 0);
    }
    {
        Writer w(buf, sizeof(buf));
        Hello a; memset(a.name, 'z', sizeof(a.name));                 // unterminated name on the wire
        WriteHello(w, a);
        Reader r(buf, w.size()); Hello b; CHECK(ReadHello(r, b)); CHECK(strlen(b.name) == sizeof(b.name) - 1);
    }
    {
        Writer w(buf, sizeof(buf));
        Welcome a; a.nonce = 5; a.peerId = 1; a.players = 2; a.session = 0x01020304;
        WriteWelcome(w, a);
        Reader r(buf, w.size()); Welcome b; CHECK(ReadWelcome(r, b));
        CHECK(b.nonce == 5 && b.peerId == 1 && b.players == 2 && b.session == 0x01020304 && r.left() == 0);
    }
    {
        Writer w(buf, sizeof(buf));
        Clock a; a.pong = 1; a.id = 9; a.t0 = 1ull << 40; a.t1 = 123456789012345ull; a.t2 = ~0ull;
        WriteClock(w, a);
        CHECK(w.size() == 29);
        Reader r(buf, w.size()); Clock b; CHECK(ReadClock(r, b));
        CHECK(b.pong == 1 && b.id == 9 && b.t0 == a.t0 && b.t1 == a.t1 && b.t2 == a.t2);
        for (size_t n = 0; n < w.size(); ++n) { Reader s(buf, n); Clock c; CHECK(!ReadClock(s, c)); }
    }
    {
        Writer w(buf, sizeof(buf));
        WriteBye(w, Bye{ kByeTimeout });
        Reader r(buf, w.size()); Bye b; CHECK(ReadBye(r, b) && b.reason == kByeTimeout);
    }
}

TEST(codec_state_fill_and_read) {
    uint8_t buf[kMaxDatagram];
    Writer w(buf, sizeof(buf));
    const size_t off = BeginState(w, 0x1122334455667788ull);
    Record rec; rec.kind = 2; rec.mask = 0x8001; rec.len = kMaxPayload;
    uint8_t n = 0;
    while (true) {
        rec.netId = 1000 + n;
        for (int i = 0; i < kMaxPayload; ++i) rec.data[i] = (uint8_t)(n + i);
        if (!AddRecord(w, rec)) break;
        ++n;
    }
    EndCount(w, off, n);
    CHECK(w.ok() && w.size() <= kMaxDatagram);
    CHECK(n == (kMaxDatagram - kStateHead) / (kRecordOverhead + kMaxPayload));   // 16 full records
    Reader r(buf, w.size());
    uint64_t stamp = 0; uint8_t count = 0;
    CHECK(ReadStateHead(r, stamp, count) && stamp == 0x1122334455667788ull && count == n);
    bool all = true;
    for (uint8_t i = 0; i < count; ++i) {
        Record g;
        if (!ReadRecord(r, g) || g.netId != 1000u + i || g.kind != 2 || g.mask != 0x8001 || g.len != kMaxPayload ||
            g.data[5] != (uint8_t)(i + 5)) all = false;
    }
    CHECK(all && r.left() == 0);
}

TEST(codec_record_bad_len) {
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    w.U32(1); w.U8(1); w.U16(0); w.U8(kMaxPayload + 1);   // len 65
    Reader r(buf, w.size()); Record g;
    CHECK(!ReadRecord(r, g));
    Record big; big.len = kMaxPayload + 1;
    Writer w2(buf, sizeof(buf));
    CHECK(!AddRecord(w2, big) && w2.size() == 0);
    Event ev; ev.len = kMaxPayload + 1;
    CHECK(!AddEvent(w2, ev) && w2.size() == 0);
}

TEST(codec_truncated_events_never_overrun) {
    uint8_t buf[kMaxDatagram];
    Writer w(buf, sizeof(buf));
    const size_t off = BeginEvents(w, 41);
    Event ev; ev.type = 7; ev.netId = 0x01000005; ev.len = 10;
    for (int i = 0; i < 5; ++i) { ev.data[0] = (uint8_t)i; CHECK(AddEvent(w, ev)); }
    EndCount(w, off, 5);
    // Every prefix: the reader reports failure instead of reading past the end.
    for (size_t len = 0; len < w.size(); ++len) {
        Reader r(buf, len);
        uint32_t first = 0; uint8_t count = 0;
        bool ok = ReadEventsHead(r, first, count);
        int got = 0;
        for (uint8_t i = 0; ok && i < count; ++i) { Event g; ok = ReadEvent(r, g); if (ok) ++got; }
        CHECK(!ok || got == 5);
        CHECK(got < 5);
    }
}

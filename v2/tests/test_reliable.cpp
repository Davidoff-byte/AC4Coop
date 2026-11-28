#include "test.h"
#include "../net/reliable.h"

#include <cstring>
#include <deque>
#include <vector>

using namespace v2::net;

namespace {

Event MakeEvent(uint32_t i) {
    Event e; e.type = (uint16_t)(i & 0xFFFF); e.netId = i * 7; e.len = (uint8_t)(i % (kMaxPayload + 1));
    for (int k = 0; k < e.len; ++k) e.data[k] = (uint8_t)(i + k);
    return e;
}
bool SameEvent(const Event& a, const Event& b) {
    return a.type == b.type && a.netId == b.netId && a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
}

// One direction of a simulated link: latency + jitter, loss, duplication (and so reordering).
struct Pipe {
    struct Dgram { uint64_t at; std::vector<uint8_t> bytes; };
    std::deque<Dgram> q;
    Rng* rng; unsigned lossPct, dupPct; uint64_t latUs, jitterUs;
    void Send(const uint8_t* p, size_t n, uint64_t now) {
        if (rng->Chance(lossPct)) return;
        const int copies = rng->Chance(dupPct) ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            Dgram d; d.at = now + latUs + rng->Below((unsigned)jitterUs + 1); d.bytes.assign(p, p + n);
            auto it = q.begin();
            while (it != q.end() && it->at <= d.at) ++it;   // ordered by arrival time: jitter reorders
            q.insert(it, d);
        }
    }
    bool Recv(uint64_t now, std::vector<uint8_t>& out) {
        if (q.empty() || q.front().at > now) return false;
        out = q.front().bytes; q.pop_front(); return true;
    }
};

struct Result { uint32_t delivered; bool inOrder; bool exact; uint64_t resent, sentFirst, dups, gaps, endUs; };

// A sends `total` events to B over lossy pipes; B acks every 5 ms (pure ack: EVENTS count 0) and on every receipt.
Result Run(uint32_t total, unsigned lossPct, unsigned dupPct, uint64_t latUs, uint64_t jitterUs, unsigned seed,
           uint32_t burst, uint32_t everyMs = 1) {
    Rng rng(seed);
    Pipe ab{ {}, &rng, lossPct, dupPct, latUs, jitterUs }, ba{ {}, &rng, lossPct, dupPct, latUs, jitterUs };
    ReliableTx tx; ReliableRx rx;
    uint32_t pushed = 0, delivered = 0;
    bool inOrder = true, exact = true;
    uint64_t lastAck = 0;
    const uint64_t rto = 2 * (2 * latUs + jitterUs) > 50000 ? 2 * (2 * latUs + jitterUs) : 50000;
    uint64_t now = 0;
    for (; now < 120ull * 1000000 && delivered < total; now += 1000) {   // 1 ms steps, 120 s cap
        if ((now / 1000) % everyMs == 0)
            for (uint32_t k = 0; k < burst && pushed < total; ++k) { if (tx.Push(MakeEvent(pushed + 1))) ++pushed; else break; }
        uint8_t buf[kMaxDatagram];
        Writer w(buf, sizeof(buf));
        if (tx.Fill(w, now, rto)) ab.Send(buf, w.size(), now);   // one datagram per ms at most
        std::vector<uint8_t> d;
        while (ab.Recv(now, d)) {
            Reader r(d.data(), d.size());
            uint32_t first = 0; uint8_t count = 0;
            if (!ReadEventsHead(r, first, count)) { exact = false; continue; }
            for (uint8_t i = 0; i < count; ++i) {
                Event e; if (!ReadEvent(r, e)) { exact = false; break; }
                if (rx.Accept(first + i)) {
                    ++delivered;
                    if (first + i != delivered) inOrder = false;
                    if (!SameEvent(e, MakeEvent(first + i))) exact = false;
                }
            }
        }
        if (rx.TakeAckDirty() || now - lastAck >= 5000) {
            lastAck = now;
            uint8_t ab2[8]; Writer aw(ab2, sizeof(ab2)); aw.U32(rx.CumAck());
            ba.Send(ab2, aw.size(), now);
        }
        while (ba.Recv(now, d)) { Reader r(d.data(), d.size()); const uint32_t ack = r.U32(); if (r.ok()) tx.OnAck(ack); }
    }
    return { delivered, inOrder, exact, tx.Resent(), tx.SentFirst(), rx.Dups(), rx.Gaps(), now };
}

}  // namespace

TEST(reliable_clean_link) {
    const Result r = Run(5000, 0, 0, 2000, 0, 1, 4);
    CHECK(r.delivered == 5000 && r.inOrder && r.exact);
    CHECK(r.resent == 0 && r.dups == 0 && r.gaps == 0);
}

TEST(reliable_lossy_link) {
    // 10 % loss each way, 5 % duplicates, 20 ms +- 15 ms: every event arrives once, in order, intact.
    const Result r = Run(20000, 10, 5, 20000, 15000, 7, 3);
    CHECK(r.delivered == 20000 && r.inOrder && r.exact);
    std::printf("  lossy: resent %llu / first %llu, dups %llu gaps %llu, %.1f s\n", (unsigned long long)r.resent,
                (unsigned long long)r.sentFirst, (unsigned long long)r.dups, (unsigned long long)r.gaps, r.endUs / 1e6);
}

TEST(reliable_internet_link) {
    // 2 % loss, 30 ms +- 2 ms, 1 event per ms: the expected two-PC case. Go-back-N overhead stays small without reordering.
    const Result r = Run(20000, 2, 0, 30000, 2000, 3, 1);
    CHECK(r.delivered == 20000 && r.inOrder && r.exact);
    std::printf("  internet: resent %llu / first %llu (%.1f %%), %.1f s\n", (unsigned long long)r.resent,
                (unsigned long long)r.sentFirst, 100.0 * r.resent / (r.sentFirst ? r.sentFirst : 1), r.endUs / 1e6);
}

TEST(reliable_internet_game_rate) {
    // The same link at a game's event rate (50 events/s): each loss resends the few events in flight.
    const Result r = Run(3000, 2, 0, 30000, 2000, 5, 1, 20);
    CHECK(r.delivered == 3000 && r.inOrder && r.exact);
    std::printf("  game rate: resent %llu / first %llu (%.1f %%)\n", (unsigned long long)r.resent,
                (unsigned long long)r.sentFirst, 100.0 * r.resent / (r.sentFirst ? r.sentFirst : 1));
}

TEST(reliable_heavy_loss) {
    const Result r = Run(3000, 40, 0, 5000, 2000, 99, 2);
    CHECK(r.delivered == 3000 && r.inOrder && r.exact);
}

TEST(reliable_window_full) {
    ReliableTx tx;
    uint32_t ok = 0;
    for (uint32_t i = 0; i < ReliableTx::kWindow + 10; ++i) ok += tx.Push(MakeEvent(i + 1)) ? 1 : 0;
    CHECK(ok == ReliableTx::kWindow && tx.Pending() == ReliableTx::kWindow);
    uint8_t buf[kMaxDatagram]; Writer w(buf, sizeof(buf));
    const uint8_t n = tx.Fill(w, 1000, 50000);
    CHECK(n > 0);
    tx.OnAck(n);                                 // n delivered: room for n more
    CHECK(tx.Pending() == ReliableTx::kWindow - n);
    CHECK(tx.Push(MakeEvent(9999)));
    tx.OnAck(100000);                            // acks events never sent: ignored
    CHECK(tx.Acked() == n);
}

TEST(reliable_resend_after_rto_only) {
    ReliableTx tx;
    tx.Push(MakeEvent(1)); tx.Push(MakeEvent(2));
    uint8_t buf[kMaxDatagram];
    { Writer w(buf, sizeof(buf)); CHECK(tx.Fill(w, 1000, 50000) == 2); }
    { Writer w(buf, sizeof(buf)); CHECK(tx.Fill(w, 30000, 50000) == 0 && w.size() == 0); }   // not due yet
    { Writer w(buf, sizeof(buf)); CHECK(tx.Fill(w, 51000, 50000) == 2); }                   // timed out: both again
    CHECK(tx.Resent() == 2);
    tx.OnAck(1);
    { Writer w(buf, sizeof(buf)); CHECK(tx.Fill(w, 102000, 50000) == 1); }
    Reader r(buf, kEventsHead); uint32_t first = 0; uint8_t c = 0;
    CHECK(ReadEventsHead(r, first, c) && first == 2 && c == 1);
}

TEST(reliable_rx_order) {
    ReliableRx rx;
    CHECK(rx.Accept(1));
    CHECK(!rx.Accept(3));          // gap: dropped (go-back-N)
    CHECK(rx.Accept(2));
    CHECK(!rx.Accept(2));          // duplicate
    CHECK(rx.Accept(3));
    CHECK(rx.CumAck() == 3 && rx.Gaps() == 1 && rx.Dups() == 1);
}

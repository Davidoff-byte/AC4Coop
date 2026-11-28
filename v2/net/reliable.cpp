#include "reliable.h"

namespace v2::net {

bool ReliableTx::Push(const Event& e) {
    if (Pending() >= kWindow || e.len > kMaxPayload) return false;
    Slot& s = ring_[next_ % kWindow];
    s.e = e; s.sentUs = 0;
    ++next_;
    return true;
}

void ReliableTx::OnAck(uint32_t cumAck) {
    if (cumAck <= acked_ || cumAck >= sentEnd_) return;   // old, or acks events never sent (bogus): ignore
    acked_ = cumAck;
    if (cursor_ <= acked_) cursor_ = acked_ + 1;           // a rewound cursor skips what the peer already has
}

uint8_t ReliableTx::Fill(Writer& w, uint64_t nowUs, uint64_t rtoUs) {
    if (!Pending()) return 0;
    const uint32_t oldest = acked_ + 1;
    const Slot& o = ring_[oldest % kWindow];
    if (o.sentUs && nowUs - o.sentUs >= rtoUs && cursor_ > oldest) { cursor_ = oldest; ++rewinds_; }   // go back N
    if (cursor_ >= next_) return 0;
    const uint32_t start = cursor_;
    if (w.room() < kEventsHead + kEventOverhead + ring_[start % kWindow].e.len) return 0;
    const size_t countOff = BeginEvents(w, start);
    uint8_t n = 0;
    for (uint32_t s = start; s < next_ && n < 255; ++s) {
        Slot& sl = ring_[s % kWindow];
        if (!AddEvent(w, sl.e)) break;
        if (sl.sentUs) ++resent_; else ++sentFirst_;
        sl.sentUs = nowUs ? nowUs : 1;   // 0 means "not sent"
        ++n;
    }
    EndCount(w, countOff, n);
    cursor_ = start + n;
    if (cursor_ > sentEnd_) sentEnd_ = cursor_;
    return n;
}

}  // namespace v2::net

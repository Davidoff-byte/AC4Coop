#pragma once
// The reliable, ordered event channel to one peer (docs/v2/ARCHITECTURE.md s5): go-back-N with a cumulative ack.
// The receiver delivers only the next expected seq and drops anything after a gap; when the oldest unacked event has
// waited `rto` without an ack, the sender rewinds its cursor to it and sends the whole run again, one datagram per Fill.
// Pure code (explicit time), unit-tested in v2/tests.
// Seqs start at 1 and do not wrap within a session (2^32 events).
#include "codec.h"

namespace v2::net {

class ReliableTx {
public:
    static constexpr uint32_t kWindow = 256;   // unacked events; Push fails beyond it

    bool Push(const Event& e);          // false: window full (nothing queued; the caller counts the drop)
    void OnAck(uint32_t cumAck);        // the peer delivered every event up to cumAck (later or bogus acks: ignored)
    // Writes an EVENTS body (head + run) from the cursor: the events not sent yet, or, after rtoUs without an ack for the
    // oldest unacked event, everything from that one again. Returns the count (0 = nothing due, nothing written).
    uint8_t Fill(Writer& w, uint64_t nowUs, uint64_t rtoUs);

    uint32_t Pending() const { return next_ - 1 - acked_; }
    uint32_t Acked() const { return acked_; }
    uint32_t NextSeq() const { return next_; }
    uint64_t Resent() const { return resent_; }
    uint64_t SentFirst() const { return sentFirst_; }
    uint64_t Rewinds() const { return rewinds_; }

private:
    struct Slot { Event e; uint64_t sentUs; };   // sentUs 0 = not sent yet
    Slot     ring_[kWindow] = {};
    uint32_t next_ = 1;        // seq of the next pushed event
    uint32_t acked_ = 0;       // every seq <= acked_ is delivered
    uint32_t cursor_ = 1;      // the next seq to send
    uint32_t sentEnd_ = 1;     // one past the highest seq ever sent (an ack beyond it is bogus)
    uint64_t resent_ = 0, sentFirst_ = 0, rewinds_ = 0;
};

class ReliableRx {
public:
    // One event of an EVENTS body at `seq`: true = it is the next in order, deliver it now.
    bool Accept(uint32_t seq) {
        ackDirty_ = true;
        if (seq == expected_) { ++expected_; return true; }
        if (seq < expected_) ++dups_; else ++gaps_;
        return false;
    }
    uint32_t CumAck() const { return expected_ - 1; }
    bool     TakeAckDirty() { const bool d = ackDirty_; ackDirty_ = false; return d; }   // an ack should go out soon
    uint64_t Dups() const { return dups_; }
    uint64_t Gaps() const { return gaps_; }

private:
    uint32_t expected_ = 1;
    bool     ackDirty_ = false;
    uint64_t dups_ = 0, gaps_ = 0;
};

}  // namespace v2::net

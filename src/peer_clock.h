#pragma once
#include <cstdint>

// [Coop] PeerClock (default 0): the shared clock of the shared-brain design (brain stage S0,
// ops/evidence/guard-ai/brain-design-2026-11-22.md section 5). Each peer pings the other at 10 Hz with its own
// datagram type ('AC4K', not the reliable intention lane: a resend would corrupt the round-trip time) and keeps an
// NTP-style estimate of the peer's clock: offset = ((t2 - t1) + (t3 - t4)) / 2, taken from the sample with the
// smallest round trip among the last 16. The published offset snaps on the first estimate and on a step > 50 ms,
// otherwise it moves at most 1 ms per sample; HostUs() never goes backwards.
//
// Clocks are QueryPerformanceCounter in microseconds. [Coop] PeerClockSkewMs (default 0, test only) adds a fixed
// amount to the HOST's clock, so on one PC the true offset is known exactly (the S0 oracle: estimate - skew,
// p95 < 5 ms).
namespace PeerClock {
    static const uint32_t kMagic = 0x4B344341;   // 'AC4K'
    enum : uint8_t { T_PING = 1, T_PONG = 2 };
#pragma pack(push, 1)
    struct Wire {                                // 40 B
        uint32_t magic;      // kMagic
        uint32_t protoVer;   // NET_PROTO_VER
        uint8_t  type;       // T_PING / T_PONG
        uint8_t  rsv[3];     // 0
        uint32_t seq;        // the pinger's sequence (echoed in the pong)
        uint64_t t1;         // pinger's clock at send (us)
        uint64_t t2;         // ponger's clock at receive (us), 0 in a ping
        uint64_t t3;         // ponger's clock at send (us), 0 in a ping
    };
#pragma pack(pop)
    static_assert(sizeof(Wire) == 40, "PeerClock::Wire is a 40-byte wire layout");

    // Pure estimator (no I/O; the offline selftest drives it with synthetic timestamps).
    struct Estimator {
        struct S { int64_t off; int64_t rtt; };
        static const int kWin = 16;   // 1.6 s at 10 Hz
        S        ring[kWin];
        int      n = 0, head = 0;
        int64_t  pub = 0;          // published offset (peer - local, us)
        bool     have = false;
        uint32_t samples = 0, snaps = 0;
        int64_t  lastRtt = 0, bestRtt = 0, target = 0;
        // One completed exchange. Returns false for an impossible sample (negative round trip), which is dropped.
        bool Add(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4);
    };
    bool SelfTest();             // offline: fixed delays, jitter, asymmetry, a step; prints "[selftest] peerclock ..."

    // Runtime (Net calls these; all no-ops when the flag is off).
    void     Init();             // read [Coop] PeerClock (Net::Init)
    // The role, once phase1 has decided it (g_entHost). Applies [Coop] PeerClockSkewMs on the host. Until it is set
    // nothing is sent or answered, so no sample mixes a clock with and without the skew.
    void     SetRole(bool isHost);
    bool     On();
    uint64_t LocalUs();          // this process's clock (QPC us, plus the skew on the host)
    // The host's clock as this process knows it: the host returns LocalUs(); the guest returns LocalUs() + the
    // estimated offset, never smaller than its previous result. False = no estimate yet (guest).
    bool     HostUs(uint64_t* out);
    int64_t  OffsetUs(bool* valid);  // peer - local, as published
    // A ping when one is due (100 ms), else 0. Fills `out` and returns its length.
    int      BuildPing(Wire* out);
    // A received 'AC4K' datagram at local time rxUs. Returns the pong length to send back (into `reply`), or 0.
    int      OnDatagram(const void* buf, int n, uint64_t rxUs, Wire* reply);
    // One "[Clock]" line every 2 s through the caller's logger (game thread).
    void     Report(void (*log)(const char*));
}

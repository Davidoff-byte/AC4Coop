// [Coop] PeerClock - see peer_clock.h.
#include "peer_clock.h"
#include "netstate.h"     // NET_PROTO_VER
#include "paths.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace PeerClock {

bool Estimator::Add(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    const int64_t rtt = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
    if (rtt < 0 || (int64_t)(t4 - t1) < 0) return false;
    const int64_t off = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;
    ring[head] = { off, rtt };
    head = (head + 1) % kWin;
    if (n < kWin) ++n;
    ++samples; lastRtt = rtt;
    int best = 0;
    for (int i = 1; i < n; ++i) if (ring[i].rtt < ring[best].rtt) best = i;
    bestRtt = ring[best].rtt; target = ring[best].off;
    const int64_t d = target - pub;
    if (!have || d > 50000 || d < -50000) { pub = target; have = true; ++snaps; }
    else if (d > 1000) pub += 1000;
    else if (d < -1000) pub -= 1000;
    else pub = target;
    return true;
}

// ---- offline selftest ----------------------------------------------------------------------------
namespace {
    struct Lcg { uint32_t s; uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; } };
    // One simulated exchange: peer clock = local + trueOff; one-way delays d1 (to peer), d2 (back), peer hold h.
    void Sim(Estimator& e, uint64_t& local, int64_t trueOff, int64_t d1, int64_t d2, int64_t h) {
        const uint64_t t1 = local;
        const uint64_t t2 = (uint64_t)((int64_t)local + d1 + trueOff);
        const uint64_t t3 = t2 + (uint64_t)h;
        const uint64_t t4 = local + (uint64_t)(d1 + h + d2);
        e.Add(t1, t2, t3, t4);
        local += 100000;
    }
    int64_t Abs(int64_t v) { return v < 0 ? -v : v; }
}

bool SelfTest() {
    bool all = true;
    auto report = [&](const char* name, bool ok, int64_t err) {
        printf("[selftest] peerclock %-10s %s (err=%lld us)\n", name, ok ? "PASS" : "FAIL", (long long)err);
        all = all && ok;
    };
    {   // symmetric fixed delay: exact
        Estimator e; uint64_t t = 1000000;
        for (int i = 0; i < 20; ++i) Sim(e, t, 250000, 3000, 3000, 200);
        report("fixed", e.pub == 250000, e.pub - 250000);
    }
    {   // jitter 0..20 ms each way, independently, base 2 ms. The registered S0 oracle is p95 < 5 ms (design section 5);
        // the worst case is printed too. A first version of this test used worst < 5 ms with an 8-sample window and
        // failed at 5.5 ms; the window is now 16 samples (1.6 s).
        Estimator e; uint64_t t = 1000000; Lcg r{ 7 };
        static int64_t errs[400]; int ne = 0; int64_t worst = 0;
        for (int i = 0; i < 400; ++i) {
            Sim(e, t, -123456, 2000 + (int64_t)(r.next() % 20000), 2000 + (int64_t)(r.next() % 20000), 300);
            if (i >= 16) { const int64_t err = Abs(e.pub - (-123456)); errs[ne++] = err; if (err > worst) worst = err; }
        }
        for (int i = 1; i < ne; ++i) for (int j = i; j > 0 && errs[j - 1] > errs[j]; --j) { const int64_t x = errs[j]; errs[j] = errs[j - 1]; errs[j - 1] = x; }
        const int64_t p95 = errs[(ne * 95) / 100];
        printf("[selftest] peerclock jitter     worst=%lld us\n", (long long)worst);
        report("jitter", p95 < 5000, p95);
    }
    {   // asymmetric path (5 ms out, 1 ms back): error is half the asymmetry, the known NTP limit (2 ms)
        Estimator e; uint64_t t = 1000000;
        for (int i = 0; i < 20; ++i) Sim(e, t, 0, 5000, 1000, 100);
        report("asym", Abs(e.pub - 2000) <= 1, e.pub);
    }
    {   // a 20 ms step: slews 1 ms per sample (never jumps), reaches it within 8 samples of the window turning over
        Estimator e; uint64_t t = 1000000;
        for (int i = 0; i < 20; ++i) Sim(e, t, 0, 2000, 2000, 100);
        int64_t prev = e.pub; bool mono = true;
        for (int i = 0; i < 40; ++i) { Sim(e, t, 20000, 2000, 2000, 100); if (e.pub - prev > 1000) mono = false; prev = e.pub; }
        report("slew", mono && e.pub == 20000, e.pub - 20000);
    }
    {   // a 2 s step (session reset, or the first estimate): snaps
        Estimator e; uint64_t t = 1000000;
        for (int i = 0; i < 20; ++i) Sim(e, t, 0, 2000, 2000, 100);
        for (int i = 0; i < 17; ++i) Sim(e, t, 2000000, 2000, 2000, 100);
        report("snap", e.pub == 2000000 && e.snaps == 2, e.pub - 2000000);
    }
    {   // an impossible sample (peer hold longer than the round trip) is dropped
        Estimator e;
        const bool took = e.Add(1000, 5000, 9000, 2000);
        report("reject", !took && !e.have, 0);
    }
    return all;
}

// ---- runtime ------------------------------------------------------------------------------------
namespace {
    int       g_on = 0;
    volatile LONG g_roleSet = 0;
    bool      g_isHost = false;
    int64_t   g_skewUs = 0;
    double    g_qpcToUs = 0.0;
    SRWLOCK   g_lock = SRWLOCK_INIT;
    Estimator g_est;
    uint32_t  g_seq = 0, g_txPing = 0, g_rxPing = 0, g_rxPong = 0, g_bad = 0, g_lostOld = 0;
    uint64_t  g_nextPing = 0, g_lastHost = 0, g_nextReport = 0;
    struct Out { uint32_t seq; uint64_t t1; };
    Out       g_out[16];
}

void Init() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_qpcToUs = 1e6 / (double)f.QuadPart;
    g_on = GetPrivateProfileIntA("Coop", "PeerClock", 0, Paths::Ini()) != 0 ? 1 : 0;
}

void SetRole(bool isHost) {
    if (!On() || g_roleSet) return;
    g_isHost = isHost;
    g_skewUs = isHost ? (int64_t)GetPrivateProfileIntA("Coop", "PeerClockSkewMs", 0, Paths::Ini()) * 1000 : 0;
    InterlockedExchange(&g_roleSet, 1);
}

bool On() { return g_on == 1; }

uint64_t LocalUs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * g_qpcToUs) + (uint64_t)g_skewUs;
}

int64_t OffsetUs(bool* valid) {
    AcquireSRWLockShared(&g_lock);
    const int64_t o = g_est.pub; const bool v = g_est.have;
    ReleaseSRWLockShared(&g_lock);
    if (valid) *valid = v;
    return o;
}

bool HostUs(uint64_t* out) {
    if (!On() || !g_roleSet) return false;
    const uint64_t local = LocalUs();
    if (g_isHost) { *out = local; return true; }
    AcquireSRWLockExclusive(&g_lock);
    const bool v = g_est.have;
    uint64_t h = (uint64_t)((int64_t)local + g_est.pub);
    if (v) { if (h < g_lastHost) h = g_lastHost; g_lastHost = h; }
    ReleaseSRWLockExclusive(&g_lock);
    if (v) *out = h;
    return v;
}

int BuildPing(Wire* w) {
    if (!On() || !g_roleSet) return 0;
    const uint64_t now = LocalUs();
    AcquireSRWLockExclusive(&g_lock);
    if (now < g_nextPing) { ReleaseSRWLockExclusive(&g_lock); return 0; }
    g_nextPing = now + 100000;
    const uint32_t seq = ++g_seq;
    Out& o = g_out[seq & 15];
    if (o.seq && o.seq + 16 == seq) ++g_lostOld;   // the slot's previous ping never got a pong
    o.seq = seq; o.t1 = LocalUs();
    ++g_txPing;
    std::memset(w, 0, sizeof(*w));
    w->magic = kMagic; w->protoVer = NET_PROTO_VER; w->type = T_PING; w->seq = seq; w->t1 = o.t1;
    ReleaseSRWLockExclusive(&g_lock);
    return (int)sizeof(*w);
}

int OnDatagram(const void* buf, int n, uint64_t rxUs, Wire* reply) {
    if (!On() || !g_roleSet) return 0;
    if (n != (int)sizeof(Wire)) { InterlockedIncrement((volatile LONG*)&g_bad); return 0; }
    Wire w; std::memcpy(&w, buf, sizeof(w));
    if (w.magic != kMagic || w.protoVer != NET_PROTO_VER || (w.type != T_PING && w.type != T_PONG)) {
        InterlockedIncrement((volatile LONG*)&g_bad); return 0;
    }
    if (w.type == T_PING) {
        InterlockedIncrement((volatile LONG*)&g_rxPing);
        *reply = w;
        reply->type = T_PONG; reply->t2 = rxUs; reply->t3 = LocalUs();
        return (int)sizeof(*reply);
    }
    AcquireSRWLockExclusive(&g_lock);
    Out& o = g_out[w.seq & 15];
    if (o.seq == w.seq && o.t1 == w.t1) {      // our ping, not yet answered
        ++g_rxPong;
        g_est.Add(w.t1, w.t2, w.t3, rxUs);
        o.seq = 0;
    } else ++g_bad;
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

void Report(void (*log)(const char*)) {
    if (!On() || !g_roleSet || !log) return;
    const uint64_t now = LocalUs();
    if (now < g_nextReport) return;
    g_nextReport = now + 2000000;
    AcquireSRWLockShared(&g_lock);
    const Estimator e = g_est;
    const uint32_t tx = g_txPing, rp = g_rxPing, rq = g_rxPong, bad = g_bad, lost = g_lostOld;
    ReleaseSRWLockShared(&g_lock);
    uint64_t hu = 0; const bool hv = HostUs(&hu);
    char m[320];
    _snprintf_s(m, sizeof(m), _TRUNCATE,
        "[Clock] role=%s skewUs=%lld have=%d offUs=%lld targetUs=%lld bestRttUs=%lld lastRttUs=%lld samples=%u snaps=%u "
        "txPing=%u rxPing=%u rxPong=%u lost=%u bad=%u localUs=%llu hostUs=%llu hostValid=%d\n",
        g_isHost ? "host" : "guest", (long long)g_skewUs, e.have ? 1 : 0, (long long)e.pub, (long long)e.target,
        (long long)e.bestRtt, (long long)e.lastRtt, e.samples, e.snaps, tx, rp, rq, lost, bad,
        (unsigned long long)now, (unsigned long long)hu, hv ? 1 : 0);
    log(m);
}

} // namespace PeerClock

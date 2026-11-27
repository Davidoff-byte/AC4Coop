// [Coop] ShipSpawnReplay (default 0; hooks installed when non-zero at load, then re-read once a second): the guest asks
// its OWN SpawningManager for an AI ship of the class a host ship record needs, instead of leaving that record unbound.
// Design + RE: ops/evidence/ship-spawn-replay-design-2026-11-27.md (s1-s4, s7, addendum "risk closure"); summary in
// docs/design/ship-spawn-replay.md. Facts used here (all static, AC4BFSP.exe, from that note):
//   submit  sub_569560(spec, owner, ctx, lo, hi), cdecl, returns char (sub_79B8D0's result), NOT the request. The request
//           is the factory sub_F72ED0's return value, seen by spawn_census.cpp DetFac on the calling thread (tagged here).
//   request +0x08 ctx key, +0x0C owner, +0x10 spec, +0x18/+0x1C id. No transform field.
//   spec    u16 +0x32 params count, +0x2C params array, first entry = AI SpawnShipParams (vt 0x0270A538); +0x3C = the
//           spec's current request, overwritten by each submit (0x5B06C8) and read by spec vt slot 21 (sub_4F70A0) ->
//           NOT restored (2026-11-28, both restore variants crashed): we submit only on an IDLE spec (+0x3C == 0), so our
//           request holds the slot exactly as an engine submit would (the request dtor clears it when it holds itself, 0x79C65B).
//   built   sub_5BC470(client, req, ent) on the manager job thread (spawn_census.cpp DetSet) -> captured lock-free here.
//   class   entity +0x9C (ReadShipClass, phase1.cpp). The class is NOT fixed per spec (addendum: template chosen at spawn
//           time from world state + position), so want == got is a hard gate.
// Threads: capture = job thread, plain reads only, no engine calls, no locks. Everything else = player-update thread.
// sub_569560 is called from OnFrame OUTSIDE every __try (0x5BCAE0 takes EnterCriticalSection(client+4) on the caller's
// thread; a swallowed fault would leave it held). Pointers are validated with VirtualQuery before the call instead.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "paths.h"

void Phase1_LogLine(const char* s);
bool Phase1_InSave();
uint64_t Phase1_ScbHostOf(uintptr_t guestShip);
bool SpawnCensusCtxOf(uintptr_t req, uintptr_t* ctx);   // spawn_census.cpp: ctx argument DetFac saw for this request
void SpawnShareReplayTag(int v);                        // spawn_census.cpp: SpawnShareDecide passes requests we submit
bool SpawnLibSpecUsable(uintptr_t spec);   // spawn_lib.cpp
uintptr_t SpawnLibSubmit(uintptr_t spec, uintptr_t owner, uintptr_t ctx, int* priv, char* submitted);   // spawn_lib.cpp
bool SpawnLibPickShip(uint32_t cls, uintptr_t* spec, uintptr_t* specVt, uintptr_t* owner, uintptr_t* ownerVt, uintptr_t* ctx, uint32_t* gotCls);   // spawn_lib.cpp

namespace {
constexpr uint32_t kShipVt = 0x01E4A128u;         // ship entity vtable (phase1.cpp ShipClassBind)
constexpr uint32_t kShipParamsVt = 0x0270A538u;   // AI SpawnShipParams
constexpr int kPoolCap = 6, kRing = 256, kPend = 32, kQ = 32, kRes = 16;
constexpr ULONGLONG kFireGapMs = 2000, kBuildTimeoutMs = 10000, kPendMs = 10000, kQueueIdleMs = 3000, kResIdleMs = 10000;

bool g_hooked = false;                 // DetFac + DetSrc + DetSet installed
volatile LONG g_rtOn = 0;              // runtime flag (player thread writes, job thread reads)
volatile LONG g_clear = 0;             // world change requested a table clear (any thread)
__declspec(thread) int t_tag = 0;      // set around our own sub_569560 call
volatile uintptr_t g_ourReq = 0;       // the request DetFac saw on the tagged thread

struct CapE { volatile uintptr_t req; uintptr_t ent, spec, specVt, owner, ownerVt, ctxReq, params, p4C; uint32_t lo, hi, clsSet; };
CapE g_ring[kRing]; volatile LONG g_ringW = 0; LONG g_ringR = 0;

struct Pend { CapE e; ULONGLONG t; };              Pend g_pend[kPend]; int g_pendN = 0;
struct Spec { uintptr_t spec, specVt, owner, ownerVt, ctx; ULONGLONG t; int valid, logged; };   Spec g_spec[64];
struct Want { uint64_t host; uint32_t cls; ULONGLONG t; int noSpecCounted; };                  Want g_q[kQ]; int g_qN = 0;
struct Res { uint64_t host; uintptr_t ent; uint32_t cls; ULONGLONG tBuilt, lastAsk; int bound; }; Res g_res[kRes]; int g_resN = 0;
struct PoolE { uintptr_t ent; ULONGLONG t; };      PoolE g_pool[16]; int g_poolN = 0;
struct Fly { uintptr_t req, spec; uint64_t host; uint32_t cls; ULONGLONG t; int active; } g_fly = {};
ULONGLONG g_lastFire = 0, g_lastFrame = 0;
struct Fail { ULONGLONG t0; int n; } g_fail[64];
LONG c_captured = 0, c_want = 0, c_fired = 0, c_submit0 = 0, c_built = 0, c_classOk = 0, c_classMiss = 0, c_bound = 0,
     c_noSpec = 0, c_failed = 0, c_gone = 0, c_badPtr = 0, c_busy = 0;

bool Readable(uintptr_t p, size_t n)   // VirtualQuery only: no fault, no __try
{
    if (p < 0x10000) return false;
    const uintptr_t end = p + n;
    for (uintptr_t a = p; a < end; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}
uint32_t ClassOfWord(uint32_t f) { return ((f & 7u) == 3u && (f & 0x1F8u) == 0xE8u) ? (f >> 9) & 0x3Fu : 0u; }   // = ReadShipClass
bool Replayable(uint32_t cls) { return cls != 0 && cls != 7 && !(cls >= 8 && cls <= 16) && cls < 64; }       // design s7 step 2
int LiveIdx(void* const* live, int n, uintptr_t e) { for (int k = 0; k < n; ++k) if ((uintptr_t)live[k] == e) return k; return -1; }
int ResOfEnt(uintptr_t e) { for (int i = 0; i < g_resN; ++i) if (g_res[i].ent == e) return i; return -1; }
int ResOfHost(uint64_t h) { for (int i = 0; i < g_resN; ++i) if (g_res[i].host == h) return i; return -1; }
int QOf(uint64_t h) { for (int i = 0; i < g_qN; ++i) if (g_q[i].host == h) return i; return -1; }
void QDel(int i) { g_q[i] = g_q[--g_qN]; }
void ClearAll(const char* why)
{
    for (int c = 0; c < 64; ++c) { g_spec[c].valid = 0; g_spec[c].logged = 0; }
    g_pendN = g_qN = g_resN = g_poolN = 0; g_fly.active = 0; g_ringR = g_ringW;
    char line[96]; _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] clear (%s)\n", why); Phase1_LogLine(line);
}
void NoteFail(uint32_t cls, ULONGLONG now)
{
    ++c_failed;
    if (cls >= 64) return;
    Fail& f = g_fail[cls];
    if (now - f.t0 > 60000) { f.t0 = now; f.n = 0; }
    ++f.n;
}
bool GaveUp(uint32_t cls, ULONGLONG now) { return cls < 64 && g_fail[cls].n >= 3 && now - g_fail[cls].t0 <= 60000; }
}  // namespace

int ShipSpawnReplayInitFlag() { return GetPrivateProfileIntA("Coop", "ShipSpawnReplay", 0, Paths::Ini()); }
void ShipSpawnReplaySetHooked(bool ok)
{
    g_hooked = ok;
    char line[96]; _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] hooks=%d (DetFac+DetSrc+DetSet)\n", ok ? 1 : 0);
    Phase1_LogLine(line);
}
bool ShipSpawnReplayOn()
{
    if (!g_hooked) return false;
    static ULONGLONG s_next = 0; const ULONGLONG now = GetTickCount64();
    if (now >= s_next) { s_next = now + 1000; InterlockedExchange(&g_rtOn, GetPrivateProfileIntA("Coop", "ShipSpawnReplay", 0, Paths::Ini()) != 0 ? 1 : 0); }
    return g_rtOn != 0;
}
void ShipSpawnReplayWorldChange() { InterlockedExchange(&g_clear, 1); }

// spawn_census.cpp DetFac (any thread, after the factory returned): our own submit's request.
void ShipReplayNoteFac(uintptr_t req) { if (t_tag && req) g_ourReq = req; }

// spawn_census.cpp DetSet (manager job thread, entry of sub_5BC470): plain reads of live objects, no engine calls.
extern "C" void ShipReplayNoteEnt(uintptr_t req, uintptr_t ent)   // C linkage: declared inside extern "C" SpawnKeyNoteEnt
{
    if (!g_hooked || !g_rtOn || !req || !ent) return;
    __try {   // guards our own reads only; nothing here calls into the engine or holds a lock
        if (*(const uint32_t*)ent != kShipVt) return;
        const uintptr_t spec = *(const uintptr_t*)(req + 0x10);
        if (!spec || *(const uint16_t*)(spec + 0x32) == 0) return;
        const uintptr_t arr = *(const uintptr_t*)(spec + 0x2C);
        const uintptr_t params = arr ? *(const uintptr_t*)arr : 0;
        if (!params || *(const uint32_t*)params != kShipParamsVt) return;   // AI ships only (not SpawnPlayerShipParams)
        const uintptr_t owner = *(const uintptr_t*)(req + 0x0C);
        CapE& c = g_ring[(InterlockedIncrement(&g_ringW) - 1) & (kRing - 1)];
        c.req = 0;
        c.ent = ent; c.spec = spec; c.specVt = *(const uint32_t*)spec; c.owner = owner; c.ownerVt = owner ? *(const uint32_t*)owner : 0;
        c.ctxReq = *(const uintptr_t*)(req + 0x08); c.params = params; c.p4C = *(const uintptr_t*)(params + 0x4C);   // p4C: instrument only
        c.lo = *(const uint32_t*)(req + 0x18); c.hi = *(const uint32_t*)(req + 0x1C);
        c.clsSet = ClassOfWord(*(const uint32_t*)(ent + 0x9C));
        c.req = req;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// phase1.cpp ShipClassBindFor (player thread): no free hull of class cls for this host record. Queue only.
void ShipSpawnReplayWant(uint64_t host, uint32_t cls)
{
    if (!host || !Replayable(cls)) return;
    if (ResOfHost(host) >= 0 || (g_fly.active && g_fly.host == host)) return;
    const ULONGLONG now = GetTickCount64();
    const int i = QOf(host);
    if (i >= 0) { g_q[i].t = now; g_q[i].cls = cls; return; }
    if (g_qN >= kQ) return;
    g_q[g_qN++] = { host, cls, now, 0 };
    ++c_want;
}
// phase1.cpp ShipClassBindFor: the hull built for this host record (class cls), 0 = none.
uintptr_t ShipSpawnReplayReservedFor(uint64_t host, uint32_t cls)
{
    const int i = ResOfHost(host);
    if (i < 0 || g_res[i].cls != cls) return 0;
    g_res[i].lastAsk = GetTickCount64();
    return g_res[i].ent;
}
// phase1.cpp: true when ent is a replay hull reserved for a DIFFERENT host record (ordinary binding must skip it).
bool ShipSpawnReplayReservedOther(uintptr_t ent, uint64_t host) { const int i = ResOfEnt(ent); return i >= 0 && g_res[i].host != host; }
void ShipSpawnReplayNoteBound(uint64_t host) { const int i = ResOfHost(host); if (i >= 0) g_res[i].bound = 1; ++c_bound; }

// phase1.cpp ApplyShips (player thread, once per applied snapshot): drain captures, accept builds, upkeep, stats.
// live[] / liveCls[] = ShipClassBind's candidate local AI ships of this frame (= the naval list, filtered).
void ShipSpawnReplayFrame(void* const* live, const uint32_t* liveCls, int liveN, int scbN)
{
    if (!ShipSpawnReplayOn()) return;
    const ULONGLONG now = GetTickCount64();
    if (InterlockedExchange(&g_clear, 0)) ClearAll("world-change");
    char line[320];
    // 1. ring -> pending (wait for the entity to appear in the naval list)
    LONG w = g_ringW;
    if (w - g_ringR > kRing) g_ringR = w - kRing;
    for (; g_ringR < w; ++g_ringR) {
        CapE c = g_ring[g_ringR & (kRing - 1)];
        if (!c.req) continue;
        if (g_pendN >= kPend) { for (int i = 1; i < kPend; ++i) g_pend[i - 1] = g_pend[i]; --g_pendN; }
        g_pend[g_pendN++] = { c, now };
    }
    // 2. pending entries present in the naval list: a capture, or the build of our request
    for (int i = 0; i < g_pendN; ) {
        const CapE& c = g_pend[i].e;
        const int k = LiveIdx(live, liveN, c.ent);
        if (k < 0) { if (now - g_pend[i].t > kPendMs) g_pend[i] = g_pend[--g_pendN]; else ++i; continue; }
        const uint32_t got = liveCls[k];
        if (g_fly.active && c.req == g_fly.req) {
            ++c_built;
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] built host=%016llX want=%u got=%u ent=%08X req=%08X clsAtSet=%u dt=%llums\n",
                        (unsigned long long)g_fly.host, g_fly.cls, got, (unsigned)c.ent, (unsigned)c.req, c.clsSet, now - g_fly.t);
            Phase1_LogLine(line);
            uint64_t host = 0;
            if (got == g_fly.cls) { ++c_classOk; host = g_fly.host; }
            else {   // class not fixed per spec (addendum): give it to a pending host record of the class built, if any
                ++c_classMiss;
                for (int q = 0; q < g_qN; ++q) if (g_q[q].cls == got && ResOfHost(g_q[q].host) < 0) { host = g_q[q].host; break; }
                _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] classMiss want=%u got=%u ent=%08X -> host=%016llX\n", g_fly.cls, got, (unsigned)c.ent, (unsigned long long)host);
                Phase1_LogLine(line);
            }
            if (host && g_resN < kRes) { g_res[g_resN++] = { host, c.ent, got, now, now, 0 }; const int q = QOf(host); if (q >= 0) QDel(q); }
            if (g_poolN < 16) g_pool[g_poolN++] = { c.ent, now };
            g_fly.active = 0;
        } else if (Replayable(got) && LiveIdx(live, liveN, c.ent) >= 0 && ResOfEnt(c.ent) < 0) {
            uintptr_t ctx = c.ctxReq, ctxFac = 0;
            const bool haveFac = SpawnCensusCtxOf(c.req, &ctxFac) && ctxFac;
            if (haveFac) ctx = ctxFac;   // design s7 step 1: prefer DetFac's ctx argument; both are logged
            Spec& s = g_spec[got];
            const bool first = !s.logged;
            s = { c.spec, c.specVt, c.owner, c.ownerVt, ctx, now, 1, 1 };
            ++c_captured;
            if (first) {
                _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] cap cls=%u item=%08X ivt=%08X owner=%08X ovt=%08X ctx=%08X/%08X ent=%08X pB=%08X clsAtSet=%u\n",
                            got, (unsigned)c.spec, (unsigned)c.specVt, (unsigned)c.owner, (unsigned)c.ownerVt, (unsigned)ctxFac, (unsigned)c.ctxReq, (unsigned)c.ent, (unsigned)c.p4C, c.clsSet);
                Phase1_LogLine(line);
            }
        }
        g_pend[i] = g_pend[--g_pendN];
    }
    // 3. upkeep: in-flight timeout, queue idle, reservations, pool liveness (O4 = gone)
    if (g_fly.active && now - g_fly.t > kBuildTimeoutMs) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] failed host=%016llX cls=%u req=%08X (no built entity in the naval list within %llums)\n",
                    (unsigned long long)g_fly.host, g_fly.cls, (unsigned)g_fly.req, kBuildTimeoutMs);
        Phase1_LogLine(line);
        NoteFail(g_fly.cls, now); g_fly.active = 0;
    }
    for (int i = 0; i < g_qN; ) if (now - g_q[i].t > kQueueIdleMs) QDel(i); else ++i;
    for (int i = 0; i < g_resN; ) {
        Res& r = g_res[i];
        if (r.bound && Phase1_ScbHostOf(r.ent) == r.host) r.lastAsk = now;
        if (LiveIdx(live, liveN, r.ent) < 0 || now - r.lastAsk > kResIdleMs) g_res[i] = g_res[--g_resN]; else ++i;   // hull becomes a normal free AI hull
    }
    for (int i = 0; i < g_poolN; ) if (LiveIdx(live, liveN, g_pool[i].ent) < 0) { ++c_gone; g_pool[i] = g_pool[--g_poolN]; } else ++i;
    // 4. stats every 5 s (cumulative counters)
    static ULONGLONG s_next = 0;
    if (now >= s_next) {
        s_next = now + 5000;
        int specs = 0; for (int c = 0; c < 64; ++c) specs += g_spec[c].valid;
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[ShipSpawnReplay] stats want=%ld captured=%ld fired=%ld submit0=%ld built=%ld classOk=%ld classMiss=%ld bound=%ld pool=%d noSpec=%ld failed=%ld gone=%ld badPtr=%ld busySkip=%ld specs=%d queue=%d res=%d | localAI=%d boundTotal=%d\n",
                    c_want, c_captured, c_fired, c_submit0, c_built, c_classOk, c_classMiss, c_bound, g_poolN, c_noSpec, c_failed, c_gone, c_badPtr, c_busy,
                    specs, g_qN, g_resN, liveN, scbN);
        Phase1_LogLine(line);
    }
    g_lastFrame = now;
}

// phase1.cpp OnFrame, AFTER the OnFrameInner __try (player-update thread only). At most one engine call per frame.
// No __try here on purpose (see the file header).
void ShipSpawnReplayFire()
{
    if (!g_hooked || !g_rtOn || !Phase1_InSave()) return;
    const ULONGLONG now = GetTickCount64();
    if (g_lastFrame && now - g_lastFrame > 30000) return;   // ApplyShips not running (no snapshots): nothing is current
    if (g_fly.active || now - g_lastFire < kFireGapMs || g_poolN >= kPoolCap || g_qN == 0) return;
    int pick = -1;
    for (int i = 0; i < g_qN; ++i) {
        const uint32_t cls = g_q[i].cls;
        if (cls < 64 && !g_spec[cls].valid) {   // [Coop] SpawnLib: newest VALID spec of this class, else of any ship class
            uintptr_t sp = 0, spv = 0, ow = 0, owv = 0, cx = 0; uint32_t gc = 0;
            if (SpawnLibPickShip(cls, &sp, &spv, &ow, &owv, &cx, &gc)) {
                g_spec[cls] = { sp, spv, ow, owv, cx, now, 1, 1 };
                char ln[200]; _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[ShipSpawnReplay] spec from SpawnLib want=%u libClass=%u spec=%08X owner=%08X\n", cls, gc, (unsigned)sp, (unsigned)ow);
                Phase1_LogLine(ln);
            }
        }
        if (cls >= 64 || !g_spec[cls].valid) { if (!g_q[i].noSpecCounted) { g_q[i].noSpecCounted = 1; ++c_noSpec; } continue; }
        if (GaveUp(cls, now)) continue;
        if (pick < 0 || g_q[i].t < g_q[pick].t) pick = i;
    }
    if (pick < 0) return;
    const Want wq = g_q[pick];
    Spec& s = g_spec[wq.cls];
    g_lastFire = now;
    // Lock-free validation (design s3): spec / owner vtables as captured, params count, first params = AI SpawnShipParams.
    bool ok = Readable(s.spec, 0x40) && *(const uint32_t*)s.spec == s.specVt && Readable(s.owner, 4) && *(const uint32_t*)s.owner == s.ownerVt
              && *(const uint16_t*)(s.spec + 0x32) != 0;
    uintptr_t arr = 0, params = 0;
    if (ok) { arr = *(const uintptr_t*)(s.spec + 0x2C); ok = Readable(arr, 4); }
    if (ok) { params = *(const uintptr_t*)arr; ok = Readable(params, 0x50) && *(const uint32_t*)params == kShipParamsVt; }
    char line[300];
    if (!ok) {
        ++c_badPtr; s.valid = 0;   // requester gone (travel / unload): never call the engine with it
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] req host=%016llX cls=%u item=%08X -> skipped (spec/owner no longer valid)\n", (unsigned long long)wq.host, wq.cls, (unsigned)s.spec);
        Phase1_LogLine(line);
        return;
    }
    const uintptr_t old = *(const uintptr_t*)(s.spec + 0x3C);
    if (!SpawnLibSpecUsable(s.spec)) {   // busy spec not usable per [Coop] SpawnLibBusy (spawn_lib.cpp SpecUsableSt). Run 2026-11-28
        // host 17860: submit on a busy spec + restore left an owner ship record {old request, our ship}; the old request was
        // freed -> AV 0x1C27B15 (sub_1C27A90 <- sub_1C4BDC0 <- sub_1972150). Swap in an idle SpawnLib spec for the next fire.
        ++c_busy;
        uintptr_t sp = 0, spv = 0, ow = 0, owv = 0, cx = 0; uint32_t gc = 0;
        if (SpawnLibPickShip(wq.cls, &sp, &spv, &ow, &owv, &cx, &gc)) s = { sp, spv, ow, owv, cx, now, 1, 1 };
        return;
    }
    // Submit through spawn_lib.cpp SubmitPrivate: private owner for the naval owner class (no engine adoption; T3 crash
    // 0x1C27B25) + slot borrow restored at build. Same tagging (SpawnShareReplayTag) as before.
    int priv = 0; char submitted = 0;
    const uintptr_t req = SpawnLibSubmit(s.spec, s.owner, s.ctx, &priv, &submitted);
    ++c_fired;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipSpawnReplay] req host=%016llX cls=%u item=%08X owner=%08X ctx=%08X -> submitted=%d req=%08X old3C=%08X privOwner=%d\n",
                (unsigned long long)wq.host, wq.cls, (unsigned)s.spec, (unsigned)s.owner, (unsigned)s.ctx, (int)submitted, (unsigned)req, (unsigned)old, priv);
    Phase1_LogLine(line);
    if (!submitted || !req) { ++c_submit0; NoteFail(wq.cls, now); return; }
    g_fly = { req, s.spec, wq.host, wq.cls, now, 1 };
    QDel(pick);
}

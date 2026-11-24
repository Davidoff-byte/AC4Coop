// [Coop] Brain - see guard_brain.h.
#include "guard_brain.h"
#include "peer_clock.h"
#include "net.h"
#include "netstate.h"
#include "paths.h"
#include "attack_forward.h"   // [Coop] BrainReact: AtkFields, AttackForward_Capture / AttackForward_Build
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace {

// ---- wire ----------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct Cmd {                 // IC_BRAIN payload, 32 B
    uint16_t slot;           // IHumanGround slot the host's AI called (kMark = test command, no engine call)
    uint8_t  tgt;            // TargetClass of arg0 (slots that take a target)
    uint8_t  nargs;          // stack args the slot takes
    uint32_t execHostMs;     // host clock (PeerClock, ms, wraps) at which both games run it
    uint32_t a[4];           // the call's args as captured; a[0] is replaced by the mapped target on replay
    uint64_t tgtObj;         // tgt == TG_GUARD: the target guard's host objId
};
#pragma pack(pop)
static_assert(sizeof(Cmd) == 32, "IC_BRAIN payload is 32 bytes (INTENT_PAYLOAD_MAX)");
const uint16_t kMark = 0xFFFF;
enum TargetClass : uint8_t { TG_NONE = 0, TG_HOST_PLAYER = 1, TG_GUEST_PLAYER = 2, TG_GUARD = 3, TG_OTHER = 4 };

const uint32_t kVtIg = 0x026E7DC0u;   // IHumanGround interface vtable (hg+0x10)
const uint32_t kVtHg = 0x026E8AE8u;   // HumanGround context vtable (*(Human+0x720))
// argKind 1 = Handle&, 2 = entity, 0 = none. vt 0 = IHumanGround; adj = bytes added to `this` to reach the IHumanGround
// interface (the body key): HumanGround's constructor sub_16D8690 installs 15 interface vtables, IHumanGround at hg+0x10
// (0x016D8767) and 0x026E7C9C at hg+0x34 (0x016D87A6), so that interface is IHumanGround + 0x24.
struct SlotSpec { uint16_t slot; uint32_t fn; uint8_t nargs; uint8_t replay; uint8_t argKind; uint32_t vt; int32_t adj; };
const SlotSpec kSlots[] = {
    { 76,  0x01D79A50u, 1, 1, 1 },    // DoEnterFight(const Handle&)
    { 113, 0x01D79C30u, 1, 1, 0 },    // DoExitFight(type)
    { 155, 0x01D50310u, 3, 1, 2 },    // (target, 0, 34)  sub_14BE2A0 case 11
    { 160, 0x01D505E0u, 4, 1, 2 },    // PerformCombo(target, n, end, 0)
    { 546, 0x01D4F450u, 2, 1, 2 },    // (target, 7)      sub_14BE2A0 case 27
    { 158, 0, 0, 0, 0 }, { 159, 0, 0, 0, 0 }, { 241, 0, 0, 0, 0 }, { 242, 0, 0, 0, 0 }, { 533, 0, 0, 0, 0 }, { 534, 0, 0, 0, 0 },
    // [Coop] BrainHitSync (IDA cc/g22-g27): the attack outcome. In attack substate 180 the host guard gets
    // slot 209 (sub_1D28280 -> sub_1CF5A90: token 512 when sub_17FE210(event) == 2, i.e. *(*(event+12)+32) != 9 and
    // helper+0x88 == 0; else 513) then slot 210 (sub_1D7A9F0 -> sub_1D674C0: with 512, 180 -> 182 via sub_1851BB0, which
    // reads *(event+12)+52 = the hit target's HandleContainer). Neighbourhood name in the 360 PDB (slots 194/195):
    // Can/DoAttackHit(FightDamageResponseEvent&) - UNVERIFIED on PC. argKind 3 = that event. Replay is gated by HitSyncOn().
    { 210, 0x01D7A9F0u, 1, 1, 3 },
    { 209, 0x01D28280u, 0, 0, 0 },                        // count only (the Can half; the guest calls it itself)
    // [Coop] BrainReact (IDA cc/g31-g36, census run 22): reactions forced through the +0x1C interface (vtable 0x026E7D40 at
    // hg+0x1C, installed at 0x016D877C; this - 0x0C = the IHumanGround key) with a FightDamageEvent argument, Can = slot - 1:
    //   1 (0x1D79010; Can 0 0x1D49D20)  guards and player 317 / 334 / 336 / 343 / 344
    //   6 (0x1D7A3C0; Can 5 0x1D275D0)  312
    //  12 (0x1D7A600; Can 11 0x1D5E480) 268 (sub_1D66BA0); called by sub_185DA80 (vt+44/+48)
    //  14 (0x1D7A660; Can 13 0x1D27940) 280 (sub_1D68C50); called by sub_18644D0 (hit resolution)
    // argKind 4 = FightDamageEvent (vtable 0x02700720). And IHumanGround 208 (0x1D7A990; Can 207 0x1D28210): guard reaction
    // 217 / 225 / 232 / 233, called from sub_1809C10 next to 209/210, so the same response-event argument (argKind 3).
    { 1,  0x01D79010u, 1, 1, 4, 0x026E7D40u, -0x0C }, { 6,  0x01D7A3C0u, 1, 1, 4, 0x026E7D40u, -0x0C },
    { 12, 0x01D7A600u, 1, 1, 4, 0x026E7D40u, -0x0C }, { 14, 0x01D7A660u, 1, 1, 4, 0x026E7D40u, -0x0C },
    { 208, 0x01D7A990u, 1, 1, 3 },
    // State-115 probe (count + log only, IDA cc/g4-g15): the only actions the Ground chart handles in dispatch 115.
    { 375, 0x01D2AC10u, 0, 0, 0 },                        // -> sub_1D03C20: in 115 pushes token 301
    { 0,   0x01D30600u, 0, 0, 0, 0x026E7C9Cu, -0x24 },    // -> sub_1D04D30: in 115 pushes token 305
    { 2,   0x01D30650u, 0, 0, 0, 0x026E7C9Cu, -0x24 },    // -> sub_1D04E30: in 115 pushes token 306
};
uint32_t VtOf(int i) { return kSlots[i].vt ? kSlots[i].vt : 0x026E7DC0u; }
const int kNSlots = sizeof(kSlots) / sizeof(kSlots[0]);

int  g_on = -1, g_delayMs = 0, g_test = 0, g_testDelayMs = 150, g_hitSync = 0, g_chartSync = 0, g_playoutMs = 0, g_puppet = 0, g_react = 0;   // g_react: [Coop] BrainReact (default 0)   // g_puppet: [Coop] BrainPuppet (default 0)   // g_playoutMs: [Coop] BrainPlayoutMs (default 0)   // [Coop] BrainHitSync, [Coop] BrainChartSync (default 0)
const uint16_t kGate = 0xB5B5;   // command: a host native took attack-substate transition a[1] from state a[0] (permit for the copy)
const uint16_t kChart = 0xC4A7;   // command: a host native took Ground transition a[1] from node a[0], handler a[2] (BrainChartSync)
// [Coop] BrainPuppet: the host player (host) and the guest's puppet of it (guest) are one brain body under this fixed id, so capture,
// the chart mirror and the hit replay apply to them unchanged. Not a guard objId (guard ids are 0x00000000Fxxxxxxx).
const uint64_t kPuppetWire = 0x5055505045540001ull;
const uint16_t kReact = 0xD0D1, kReactData = 0xD0D0;   // BrainReact: header {slot, seq, attacker} and 5 data chunks of 20 bytes (AtkFields)
volatile LONG g_role = -1;   // 1 host, 0 guest

void Log(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap); va_end(ap);
    Phase1_LogLine(b);
}
bool Rd(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const char*)p + n <= (const char*)mi.BaseAddress + mi.RegionSize;
}

// ---- bodies --------------------------------------------------------------------------------------
struct Body {
    uint32_t human, ent, ig; uint64_t objId; uint64_t seenMs;
    uint64_t watchUntilUs; uint32_t watchExec; uint8_t in173; uint16_t lastPath[12]; uint8_t lastDepth; int pathLines; uint16_t lastGid; struct { uint16_t st; int32_t code; uint64_t untilMs; } permit[4];
    Cmd pendAtk; uint8_t pendValid; uint8_t hostIn173;   // BrainHitSync host: last attack call, sent when the guard enters 173
    uint32_t fsm;   // cached Ground helper *(ig+0x90), refreshed by NoteBody
    uint32_t pathHash;   // BrainPuppet: last logged path of the player / puppet body
    uint32_t rseq; uint8_t rmask; uint8_t rbuf[100]; uint32_t rFde; uint32_t rFdeSeq;   // BrainReact guest: reassembly, built event
    struct { uint16_t node; int32_t code; uint32_t handler; uint64_t atMs; uint8_t sched; } cq[8]; int cqn;   // BrainChartSync guest queue
    struct { uint16_t node; int32_t code; uint32_t handler; uint32_t dueHostMs; uint64_t setMs; } hold;   // BrainPlayoutMs host: a deferred attack start
};
const int kBodies = 256;
Body    g_b[kBodies]; int g_nb = 0;
SRWLOCK g_bLock = SRWLOCK_INIT;

uint32_t IgOf(uint32_t human) {
    __try {
        if (!Rd((void*)(human + 0x720), 4)) return 0;
        const uint32_t hg = *(uint32_t*)(human + 0x720);
        if (!Rd((void*)hg, 0x20) || *(uint32_t*)hg != kVtHg || *(uint32_t*)(hg + 0x10) != kVtIg) return 0;
        return hg + 0x10;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int FindBy(uint32_t Body::* f, uint32_t v) { for (int i = 0; i < g_nb; ++i) if (g_b[i].*f == v) return i; return -1; }
int FindObj(uint64_t o) { for (int i = 0; i < g_nb; ++i) if (g_b[i].objId == o) return i; return -1; }

// ---- counters ------------------------------------------------------------------------------------
volatile LONG c_cap[kNSlots] = {}, c_capUnbound = 0, c_tx = 0, c_txFail = 0, c_rx = 0, c_rxStaleBase = 0, c_exec = 0, c_execFail = 0,
              c_stale = 0, c_noTarget = 0, c_noBody = 0, c_noClock = 0, c_s173 = 0, c_queueFull = 0, c_notReady = 0, c_enterRefused = 0, c_dupDropped = 0, c_alreadyFight = 0, c_gatePermits = 0, c_hitLines = 0, c_hitHeld = 0, c_hitApplied = 0, c_hitRefused = 0, c_gate181Tx = 0, c_gate181Blocked = 0, c_gate181Allowed = 0, c_gateSubst = 0, c_attackHeld = 0, c_atkBuffered = 0, c_atkSent = 0, c_atkNoCall = 0, c_chartDropped = 0, c_chartRx = 0, c_puppetTx = 0, c_ppathLines = 0, c_pcapLines = 0, c_evtLines = 0, c_capLines = 0, c_execLines = 0;
LONG    g_late[64]; volatile LONG g_lateN = 0;   // last 64 lateness samples (ms) for the report

// Host: the guest is in a world and answering. Its PeerClock role is set only from a player frame (OnFrameInner), so the
// host's estimate of the guest clock becomes valid only once the guest is in the world. Run 1 (brain-s0) sent its first
// 24 markers while the guest was still loading; none ran there.
// Run 2: gating on the clock alone still lost the first 24 markers: the intention lane's handshake (peerInc known) came
// later, and the guest dropped the delayed first messages as more than 1 s late. Both must be up.
bool PeerReady() {
    bool v = false; PeerClock::OffsetUs(&v);
    Net::IntentStats st; Net::GetIntentStats(st);
    const bool lane = st.peerInc != 0;
    static int s_logged = 0;
    if ((v || lane) && s_logged < 2) {
        const int want = (v && lane) ? 2 : 1;
        if (want > s_logged) { s_logged = want; Log("[Brain] peer clock=%d lane=%d (peerInc=%08X) t=%llu ms\n", v ? 1 : 0, lane ? 1 : 0, st.peerInc, GetTickCount64()); }
    }
    return v && lane;
}
uint32_t HostMs(bool* ok) {
    uint64_t us = 0;
    *ok = PeerClock::HostUs(&us);
    return (uint32_t)(us / 1000);
}

TargetClass ClassOf(uint32_t ent, uint64_t* obj) {
    *obj = 0;
    if (!ent) return TG_NONE;
    if ((void*)ent == Phase1_LocalPlayerEntity()) return TG_HOST_PLAYER;   // host side: the local player is the host's
    if ((void*)ent == Phase1_PuppetEntity()) return TG_GUEST_PLAYER;
    AcquireSRWLockShared(&g_bLock);
    const int k = FindBy(&Body::ent, ent);
    if (k >= 0) *obj = g_b[k].objId;
    ReleaseSRWLockShared(&g_bLock);
    return k >= 0 ? TG_GUARD : TG_OTHER;
}

void Watch(uint32_t human, uint32_t execMs) {
    AcquireSRWLockExclusive(&g_bLock);
    const int k = FindBy(&Body::human, human);
    if (k >= 0) {
        LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
        g_b[k].watchUntilUs = (uint64_t)(c.QuadPart * 1000000.0 / f.QuadPart) + 3000000; g_b[k].watchExec = execMs; g_b[k].pathLines = 0;
    }
    ReleaseSRWLockExclusive(&g_bLock);
}

// ---- host capture --------------------------------------------------------------------------------
void Publish(uint64_t objId, const Cmd& c) {
    const bool ok = Net::PublishIntent(0, objId, IC_BRAIN, c.execHostMs, &c, (uint8_t)sizeof(c));
    InterlockedIncrement(ok ? &c_tx : &c_txFail);
    if (ok) Net::PumpIntents();   // Havana run 1: commands waited up to a frame for the next player-lane send (late p50 22 ms)
}

// The Ground active path of the body whose IHumanGround interface is `ig`: fsm = [ig+0x90] (= [hg+0xA0]), ids u16
// *(fsm+0x118), depth u16 fsm+0x11E. Returns the dispatch id path[3] (the value sub_1D02DF0 tests), 0 if unreadable.
unsigned PathStr(uint32_t ig, char* out, size_t cap) {
    out[0] = 0; unsigned gid = 0;
    __try {
        const uint32_t fsm = Rd((void*)(ig + 0x90), 4) ? *(uint32_t*)(ig + 0x90) : 0;
        if (!Rd((void*)fsm, 0x120)) return 0;
        unsigned d = *(const uint16_t*)(fsm + 0x11E); if (d > 12) d = 12;
        const uint32_t ss = *(uint32_t*)(fsm + 0x118);
        if (!Rd((void*)ss, 2 * (d > 4 ? d : 4))) return 0;
        if (d > 3) gid = *(const uint16_t*)(ss + 6);
        int o = 0;
        for (unsigned i = 0; i < d && (size_t)o + 8 < cap; ++i) o += _snprintf_s(out + o, cap - o, _TRUNCATE, i ? "/%u" : "%u", *(const uint16_t*)(ss + 2 * i));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return gid;
}
volatile LONG c_probeLines = 0, c_gidLines = 0;

// ---- [Coop] BrainReact host side --------------------------------------------------------------------------------------
static_assert(sizeof(AtkFields) <= 100, "AtkFields must fit the 5 x 20-byte BrainReact chunks");
volatile LONG g_reactSeq = 0, c_reactCap = 0, c_reactNonFde = 0, c_reactLines = 0, c_reactApplied = 0, c_reactRefused = 0,
              c_reactIncomplete = 0, c_reactBuildFail = 0, c_reactVeto = 0, c_reactHeld = 0;
void ReactCapture(uint32_t id, uint32_t ecx, const uint32_t* frame) {
    if (!g_react || g_role != 1) return;
    __try {
        const uint32_t ig = ecx + (uint32_t)kSlots[id].adj;
        uint64_t objId = 0;
        AcquireSRWLockShared(&g_bLock);
        const int k = FindBy(&Body::ig, ig);
        if (k >= 0) objId = g_b[k].objId;
        ReleaseSRWLockShared(&g_bLock);
        if (!objId) { InterlockedIncrement(&c_capUnbound); return; }
        InterlockedIncrement(&c_cap[id]);
        const uint32_t fde = frame[1];
        if (!Rd((void*)fde, 0x170) || *(const uint32_t*)fde != 0x02700720u) {   // FightDamageEvent vtable (player_damage_probe.cpp VT_FDE)
            if (InterlockedIncrement(&c_reactNonFde) <= 20)
                Log("[Brain] react slot=%u id=%016llX arg %08X is not a FightDamageEvent (vt %08X): not sent\n", kSlots[id].slot, (unsigned long long)objId, fde,
                    Rd((void*)fde, 4) ? *(const uint32_t*)fde : 0);
            return;
        }
        AtkFields af; memset(&af, 0, sizeof(af));
        AttackForward_Capture((const void*)fde, af);
        const uint32_t ah = *(const uint32_t*)(fde + 0x58);   // attacker handle -> entity: [h+8] < 0 ? [h] : 0
        const uint32_t att = Rd((void*)ah, 12) && *(const int32_t*)(ah + 8) < 0 ? *(const uint32_t*)ah : 0;
        uint64_t aObj = 0; const uint8_t aCls = (uint8_t)ClassOf(att, &aObj);
        bool clk = false; const uint32_t now = HostMs(&clk);
        if (!clk || !PeerReady()) return;
        const uint32_t seq = (uint32_t)InterlockedIncrement(&g_reactSeq);
        uint8_t raw[100] = {}; memcpy(raw, &af, sizeof(af));
        for (int i = 0; i < 5; ++i) {   // data first: the lane delivers one guard's messages in order, so the header finds them all
            Cmd d = {}; d.slot = kReactData; d.nargs = (uint8_t)i; d.a[0] = seq; d.execHostMs = now;
            memcpy(&d.a[1], raw + i * 20, 12); memcpy(&d.tgtObj, raw + i * 20 + 12, 8);
            Publish(objId, d);
        }
        Cmd h = {}; h.slot = kReact; h.nargs = (uint8_t)kSlots[id].slot; h.a[0] = seq; h.tgt = aCls; h.tgtObj = aObj; h.execHostMs = now;
        Publish(objId, h);
        InterlockedIncrement(&c_reactCap);
        if (InterlockedIncrement(&c_reactLines) <= 200) {
            char ps[96]; PathStr(ig, ps, sizeof(ps));
            Log("[Brain] react cap slot=%u id=%016llX seq=%u phase=%u type=%u sub=%u attacker=%08X cls=%u aobj=%016llX path=%s hostMs=%u\n",
                kSlots[id].slot, (unsigned long long)objId, seq, af.phase, af.type, af.sub, att, aCls, (unsigned long long)aObj, ps, now);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void __cdecl Capture(uint32_t id, uint32_t ecx, const uint32_t* frame) {
    if (id >= (uint32_t)kNSlots) return;
    if (kSlots[id].argKind == 4) { ReactCapture(id, ecx, frame); return; }   // [Coop] BrainReact
    if (kSlots[id].vt || kSlots[id].slot == 375) {   // state-115 probe: who calls the actions dispatch 115 accepts, and from which path
        __try {
            const uint32_t ig = ecx + (uint32_t)kSlots[id].adj;
            uint64_t objId = 0;
            AcquireSRWLockShared(&g_bLock);
            const int k = FindBy(&Body::ig, ig);
            if (k >= 0) objId = g_b[k].objId;
            ReleaseSRWLockShared(&g_bLock);
            if (!objId) { InterlockedIncrement(&c_capUnbound); return; }
            InterlockedIncrement(&c_cap[id]);
            char ps[96]; const unsigned gid = PathStr(ig, ps, sizeof(ps));
            if (InterlockedIncrement(&c_probeLines) <= 200)
                Log("[Brain] probe vt=%08X slot=%u id=%016llX gid=%u path=%s ret=%08X\n", VtOf(id), kSlots[id].slot, (unsigned long long)objId, gid, ps, frame[0]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    __try {
        uint64_t objId = 0; uint32_t human = 0;
        AcquireSRWLockShared(&g_bLock);
        const int k = FindBy(&Body::ig, ecx + (uint32_t)kSlots[id].adj);
        if (k >= 0) { objId = g_b[k].objId; human = g_b[k].human; }
        ReleaseSRWLockShared(&g_bLock);
        if (!objId) { InterlockedIncrement(&c_capUnbound); return; }   // player, puppet, or an unscoped body
        InterlockedIncrement(&c_cap[id]);
        if (objId == kPuppetWire && InterlockedIncrement(&c_pcapLines) <= 200)   // BrainPuppet: which slots the player's own moves use
            Log("[Brain] pcap slot=%u a=%08X,%08X,%08X ret=%08X\n", kSlots[id].slot, frame[1], frame[2], frame[3], frame[0]);
        const SlotSpec& s = kSlots[id];
        bool clk = false;
        const uint32_t now = HostMs(&clk);
        Cmd c = {};
        c.slot = s.slot; c.nargs = s.nargs; c.execHostMs = now + (uint32_t)g_delayMs;
        for (int i = 0; i < 4; ++i) c.a[i] = frame[1 + i];
        uint32_t tent = 0;
        if (s.argKind == 1 && Rd((void*)frame[1], 4)) {                 // const Handle&: *arg = HandleContainer*
            const int32_t* hc = *(const int32_t* const*)frame[1];
            if (Rd(hc, 12)) tent = (uint32_t)(hc[0] & (hc[2] >> 31));  // object = hc[0] & (hc[2] >> 31) (GuardTargetClass)
        } else if (s.argKind == 2) tent = frame[1];
        else if (s.argKind == 3) {                                       // hit-response event: R = *(E+12); type R+32; target hc R+52
            const uint32_t e = frame[1];
            const uint32_t r = Rd((void*)(e + 12), 4) ? *(uint32_t*)(e + 12) : 0;
            if (Rd((void*)r, 56)) {
                c.a[0] = *(uint32_t*)(r + 32);
                const int32_t* hc = *(const int32_t* const*)(r + 52);
                if (Rd(hc, 12)) tent = (uint32_t)(hc[0] & (hc[2] >> 31));
            } else c.a[0] = 0xFFFFFFFFu;
            c.a[1] = c.a[2] = c.a[3] = 0;
        }
        if (s.argKind) c.tgt = (uint8_t)ClassOf(tent, &c.tgtObj);
        if (InterlockedIncrement(&c_capLines) <= 80)
            Log("[Brain] cap slot=%u id=%016llX tgt=%u tobj=%016llX a=%08X,%08X,%08X,%08X ret=%08X hostMs=%u clk=%d\n", s.slot,
                (unsigned long long)objId, c.tgt, (unsigned long long)c.tgtObj, c.a[0], c.a[1], c.a[2], c.a[3], frame[0], now, clk ? 1 : 0);
        if (!s.replay || !clk) return;
        if (s.argKind == 3 && !g_hitSync) return;
        if (!PeerReady()) { InterlockedIncrement(&c_notReady); return; }
        const bool atk = s.slot == 155 || s.slot == 160 || s.slot == 546;
        if (atk && g_hitSync) {
            // Havana run 14: only 5 of 9 attack calls on the host started an attack (the AI also calls while the guard cannot
            // attack yet). The call is kept per guard and sent by OwnUpdate when the guard's path first shows 173, i.e. when
            // the engine really starts it.
            AcquireSRWLockExclusive(&g_bLock);
            const int kb = FindBy(&Body::human, human);
            if (kb >= 0) { g_b[kb].pendAtk = c; g_b[kb].pendValid = 1; }
            ReleaseSRWLockExclusive(&g_bLock);
            InterlockedIncrement(&c_atkBuffered);
        } else Publish(objId, c);
        if (atk) Watch(human, c.execHostMs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

uint8_t* g_stubs = nullptr;           // allocated once and never freed: a thread may still be inside a stub after a restore
uint32_t g_orig[kNSlots] = {};        // the slot's target before the swap (the verified VA)
bool     g_hooked[kNSlots] = {};
void Emit(uint8_t* p, uint32_t id, uint32_t orig) {
    int k = 0;
    auto b = [&](uint8_t v) { p[k++] = v; };
    auto d = [&](uint32_t v) { memcpy(p + k, &v, 4); k += 4; };
    b(0x60); b(0x9C);                               // pushad; pushfd
    b(0x8D); b(0x44); b(0x24); b(0x24);             // lea eax, [esp+24h]  -> the original frame: [0] ret, [1..] args
    b(0x50);                                        // push eax
    b(0x51);                                        // push ecx (this = the IHumanGround interface)
    b(0x68); d(id);                                 // push id
    b(0xB8); d((uint32_t)(uintptr_t)&Capture);      // mov eax, Capture
    b(0xFF); b(0xD0);                               // call eax
    b(0x83); b(0xC4); b(0x0C);                      // add esp, 12
    b(0x9D); b(0x61);                               // popfd; popad
    b(0x68); d(orig);                               // push orig
    b(0xC3);                                        // ret  (stack and registers exactly as the caller left them)
}
bool SwapSlot(uint32_t* p, uint32_t from, uint32_t to) {   // exchange one vtable dword only if it still holds `from`
    DWORD old = 0;
    if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return false;
    const bool ok = (uint32_t)InterlockedCompareExchange((volatile LONG*)p, (LONG)to, (LONG)from) == from;
    VirtualProtect(p, 4, old, &old);
    return ok;
}
void InstallCapture() {
    (void)0;
    if (!g_stubs) g_stubs = (uint8_t*)VirtualAlloc(nullptr, kNSlots * 40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs) { Log("[Brain] VirtualAlloc failed: no capture\n"); return; }
    int ok = 0;
    for (int i = 0; i < kNSlots; ++i) {
        if (g_hooked[i]) { ++ok; continue; }
        const uint32_t cur = ((uint32_t*)(uintptr_t)VtOf(i))[kSlots[i].slot];
        // Every slot dword must still hold the target read from the exe (tools/pe.py, 2026-11-22); anything else (another
        // hook, a different build) is refused with the value printed.
        if ((kSlots[i].argKind == 4 || kSlots[i].slot == 208 && !kSlots[i].vt) && !g_react) continue;   // BrainReact slots only when on
        if (kSlots[i].fn && cur != kSlots[i].fn) { Log("[Brain] slot %u is %08X, expected %08X: not hooked\n", kSlots[i].slot, cur, kSlots[i].fn); continue; }
        if (cur < 0x00401000u || cur >= 0x02400000u) { Log("[Brain] slot %u target %08X outside code: not hooked\n", kSlots[i].slot, cur); continue; }
        uint8_t* stub = g_stubs + i * 40;
        Emit(stub, (uint32_t)i, cur);
        FlushInstructionCache(GetCurrentProcess(), stub, 40);
        if (SwapSlot(&((uint32_t*)(uintptr_t)VtOf(i))[kSlots[i].slot], cur, (uint32_t)(uintptr_t)stub)) { g_orig[i] = cur; g_hooked[i] = true; ++ok; }
    }
    Log("[Brain] host capture: %d of %d IHumanGround slots hooked (vtable %08X)\n", ok, kNSlots, kVtIg);
}
// Audit I-103 (ops/AUDIT-CLAUDE-WORK-2026-11-23.md): put every swapped dword back when [Coop] Brain goes to 0 at runtime. A
// slot is restored only if it still points at our stub; the stub memory stays allocated.
void RemoveCapture() {
    (void)0;
    int restored = 0, foreign = 0;
    for (int i = 0; i < kNSlots; ++i) {
        if (!g_hooked[i]) continue;
        if (SwapSlot(&((uint32_t*)(uintptr_t)VtOf(i))[kSlots[i].slot], (uint32_t)(uintptr_t)(g_stubs + i * 40), g_orig[i])) { g_hooked[i] = false; ++restored; }
        else ++foreign;
    }
    Log("[Brain] host capture removed: restored=%d foreign=%d\n", restored, foreign);
}

// ---- guest duplicate filter (audit I-102) ---------------------------------------------------------
struct SeqRec { uint64_t objId; uint32_t seq; };
const int kSeq = 256;
SeqRec   g_seq[kSeq]; int g_nSeq = 0; uint32_t g_seqInc = 0;
SRWLOCK  g_seqLock = SRWLOCK_INIT;

// ---- guest queue ---------------------------------------------------------------------------------
struct Pending { uint64_t objId; Cmd c; uint8_t used; };
const int kQ = 512;
Pending g_q[kQ];
SRWLOCK g_qLock = SRWLOCK_INIT;

uint32_t CallSlot(uint32_t ig, int slot, int n, const uint32_t* a) {
    void* fn = (*(void***)ig)[slot];
    __try {
        switch (n) {
        case 1: return ((uint32_t (__thiscall*)(uint32_t, uint32_t))fn)(ig, a[0]);
        case 2: return ((uint32_t (__thiscall*)(uint32_t, uint32_t, uint32_t))fn)(ig, a[0], a[1]);
        case 3: return ((uint32_t (__thiscall*)(uint32_t, uint32_t, uint32_t, uint32_t))fn)(ig, a[0], a[1], a[2]);
        case 4: return ((uint32_t (__thiscall*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))fn)(ig, a[0], a[1], a[2], a[3]);
        default: return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&c_execFail); return 0xFFFFFFFFu; }
}

uint32_t ResolveTarget(const Cmd& c, bool* ok) {   // guest side
    *ok = true;
    switch (c.tgt) {
    case TG_NONE: return 0;
    case TG_HOST_PLAYER: return (uint32_t)(uintptr_t)Phase1_PuppetEntity();        // the host's body here is the puppet
    case TG_GUEST_PLAYER: return (uint32_t)(uintptr_t)Phase1_LocalPlayerEntity();
    case TG_GUARD: {
        AcquireSRWLockShared(&g_bLock);
        const int k = FindObj(c.tgtObj);
        const uint32_t e = k >= 0 ? g_b[k].ent : 0;
        ReleaseSRWLockShared(&g_bLock);
        *ok = e != 0; return e;
    }
    default: *ok = false; return 0;
    }
}

bool Execute(uint32_t human, uint32_t ig, uint64_t objId, const Cmd& c, int32_t lateMs) {
    if (c.slot == kMark) {
        InterlockedIncrement(&c_exec);
        Log("[Brain] exec role=%s slot=mark id=%016llX execMs=%u lateMs=%d\n", g_role == 1 ? "host" : "guest", (unsigned long long)objId, c.execHostMs, lateMs);
        return true;
    }
    if (c.slot == kReactData) {   // BrainReact: one 20-byte chunk of the host's FightDamageEvent fields
        AcquireSRWLockExclusive(&g_bLock);
        const int k = FindBy(&Body::human, human);
        if (k >= 0 && c.nargs < 5) {
            Body& b = g_b[k];
            if (b.rseq != c.a[0]) { b.rseq = c.a[0]; b.rmask = 0; }
            memcpy(b.rbuf + c.nargs * 20, &c.a[1], 12); memcpy(b.rbuf + c.nargs * 20 + 12, &c.tgtObj, 8);
            b.rmask |= (uint8_t)(1u << c.nargs);
        }
        ReleaseSRWLockExclusive(&g_bLock);
        InterlockedIncrement(&c_exec);
        return true;
    }
    if (c.slot == kReact) {   // BrainReact: rebuild the event and play the host's reaction (+0x1C Can = slot - 1, then Do)
        uint8_t raw[100]; bool complete = false; uint32_t victim = 0, fde = 0;
        AcquireSRWLockExclusive(&g_bLock);
        const int k = FindBy(&Body::human, human);
        if (k >= 0) {
            Body& b = g_b[k];
            complete = b.rseq == c.a[0] && b.rmask == 0x1F;
            if (complete) memcpy(raw, b.rbuf, sizeof(raw));
            victim = b.ent;
            if (b.rFdeSeq == c.a[0]) fde = b.rFde;   // a retry reuses the event built for this seq
        }
        ReleaseSRWLockExclusive(&g_bLock);
        // Commands with equal execMs run in queue-slot order, not arrival order, so the header can come before its chunks: wait.
        if (!complete) { if (lateMs < 400) return false; InterlockedIncrement(&c_reactIncomplete); return true; }
        bool tok = true;
        const uint32_t att = ResolveTarget(c, &tok);
        const uint32_t iface = ig + 0x0C;   // the +0x1C interface: hg + 0x1C = (ig = hg + 0x10) + 0x0C
        char ps[96]; PathStr(ig, ps, sizeof(ps));
        if (!fde) {
            AtkFields af; memcpy(&af, raw, sizeof(af));
            fde = (uint32_t)(uintptr_t)AttackForward_Build(tok ? (void*)(uintptr_t)att : nullptr, af);
            if (!fde) { InterlockedIncrement(&c_reactBuildFail); return true; }
            // FDE+0xA4 = the victim's HandleContainer with one reference, as sub_185DA80 sets it before the reaction calls
            // (`v4 = *(entity+200)`, two increments, exchange into a2+164, one decrement).
            __try {
                const uint32_t hc = *(const uint32_t*)(victim + 0xC8);
                if (hc) { InterlockedIncrement((volatile LONG*)(hc + 4)); *(uint32_t*)(fde + 0xA4) = hc; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            AcquireSRWLockExclusive(&g_bLock);
            const int kk = FindBy(&Body::human, human);
            if (kk >= 0) { g_b[kk].rFde = fde; g_b[kk].rFdeSeq = c.a[0]; }
            ReleaseSRWLockExclusive(&g_bLock);
        }
        uint32_t ea[4] = { fde, 0, 0, 0 };
        const uint32_t can = CallSlot(iface, c.nargs - 1, 1, ea);
        if (!(can & 0xFF) && lateMs < 400) { InterlockedIncrement(&c_reactHeld); return false; }   // not in the right state yet: retry
        uint32_t dr = 0;
        if (can & 0xFF) dr = CallSlot(iface, c.nargs, 1, ea);
        char ps2[96]; PathStr(ig, ps2, sizeof(ps2));
        InterlockedIncrement(&c_exec); InterlockedIncrement((can & 0xFF) ? &c_reactApplied : &c_reactRefused);
        if (InterlockedIncrement(&c_reactLines) <= 400)
            Log("[Brain] react guest slot=%u id=%016llX seq=%u attacker=%08X cls=%u can=%08X do=%08X path %s -> %s lateMs=%d\n", c.nargs,
                (unsigned long long)objId, c.a[0], att, c.tgt, can, dr, ps, ps2, lateMs);
        return true;
    }
    if (c.slot == kChart) {   // BrainChartSync: queue the host guard's transition for the copy's node wrapper (NodeW)
        AcquireSRWLockExclusive(&g_bLock);
        const int k = FindBy(&Body::human, human);
        if (k >= 0) {
            Body& b = g_b[k];
            if (b.cqn == 8) { for (int i = 1; i < 8; ++i) b.cq[i - 1] = b.cq[i]; b.cqn = 7; InterlockedIncrement(&c_chartDropped); }
            b.cq[b.cqn].node = (uint16_t)c.a[0]; b.cq[b.cqn].code = (int32_t)c.a[1]; b.cq[b.cqn].handler = c.a[2]; b.cq[b.cqn].atMs = GetTickCount64(); b.cq[b.cqn].sched = c.a[3] == 1;
            ++b.cqn;
        }
        ReleaseSRWLockExclusive(&g_bLock);
        InterlockedIncrement(&c_exec); InterlockedIncrement(&c_chartRx);
        return true;
    }
    if (c.slot == kGate) {   // BrainHitSync: the host guard took transition a[1] from attack substate a[0]; store it for the copy
        AcquireSRWLockExclusive(&g_bLock);
        const int k = FindBy(&Body::human, human);
        if (k >= 0) {
            int j = 0;   // same state replaces, else the oldest slot
            for (int i = 0; i < 4; ++i) { if (g_b[k].permit[i].st == (uint16_t)c.a[0]) { j = i; break; } if (g_b[k].permit[i].untilMs < g_b[k].permit[j].untilMs) j = i; }
            g_b[k].permit[j].st = (uint16_t)c.a[0]; g_b[k].permit[j].code = (int32_t)c.a[1]; g_b[k].permit[j].untilMs = GetTickCount64() + 1500;
        }
        ReleaseSRWLockExclusive(&g_bLock);
        InterlockedIncrement(&c_exec); InterlockedIncrement(&c_gatePermits);
        return true;
    }
    bool tok = true;
    const uint32_t t = ResolveTarget(c, &tok);
    if (c.slot == 210 || c.slot == 208) {   // BrainHitSync / BrainReact: replay the host guard's hit (210) or blocked (208) outcome, in substate 180
        char ps[96]; PathStr(ig, ps, sizeof(ps));
        unsigned p5 = 0;
        __try {
            const uint32_t fsm = *(uint32_t*)(ig + 0x90);
            const unsigned d = *(const uint16_t*)(fsm + 0x11E);
            if (d > 5) p5 = *(const uint16_t*)(*(uint32_t*)(fsm + 0x118) + 10);
        } __except (EXCEPTION_EXECUTE_HANDLER) { p5 = 0; }
        if (p5 != 180) {   // not there yet: hold (retried every frame up to 1 s late); a copy past it is reported, not forced
            if (InterlockedIncrement(&c_hitLines) <= 120)
                Log("[Brain] hit held id=%016llX path=%s lateMs=%d\n", (unsigned long long)objId, ps, lateMs);
            InterlockedIncrement(&c_hitHeld);
            return false;
        }
        if (!tok || !Rd((void*)t, 0xCC)) { InterlockedIncrement(&c_noTarget); return true; }
        // The response event as sub_17FE210 / sub_1868F80 / sub_1851BB0 read it: E+12 -> R; R+32 type; R+52 the hit target's
        // HandleContainer (the engine takes a reference on it, +4). Everything else zero. UNVERIFIED that nothing else in E/R
        // is read on these paths beyond what the three decompiles show.
        __declspec(align(16)) uint8_t r[128] = {};
        __declspec(align(16)) uint8_t e[64] = {};
        *(uint32_t*)(r + 32) = c.a[0];
        *(uint32_t*)(r + 52) = *(uint32_t*)(t + 0xC8);
        *(uint32_t*)(e + 12) = (uint32_t)(uintptr_t)r;
        uint32_t ea[4] = { (uint32_t)(uintptr_t)e, 0, 0, 0 };
        const uint32_t can = CallSlot(ig, c.slot - 1, 1, ea);
        uint32_t dr = 0;
        if (can & 0xFF) dr = CallSlot(ig, c.slot, 1, ea);
        char ps2[96]; PathStr(ig, ps2, sizeof(ps2));
        InterlockedIncrement(&c_exec); InterlockedIncrement((can & 0xFF) ? &c_hitApplied : &c_hitRefused);
        if (InterlockedIncrement(&c_hitLines) <= 120)
            Log("[Brain] hit id=%016llX type=%u target=%08X can=%08X do=%08X path %s -> %s lateMs=%d\n", (unsigned long long)objId, c.a[0], t, can, dr, ps, ps2, lateMs);
        return true;
    }
    if (!tok || ((c.slot == 76 || c.slot == 155 || c.slot == 160 || c.slot == 546) && !Rd((void*)t, 0xCC))) {
        InterlockedIncrement(&c_noTarget);
        if (InterlockedIncrement(&c_execLines) <= 120)
            Log("[Brain] exec slot=%u id=%016llX NO TARGET tgt=%u tobj=%016llX\n", c.slot, (unsigned long long)objId, c.tgt, (unsigned long long)c.tgtObj);
        return true;
    }
    uint32_t a[4] = { c.a[0], c.a[1], c.a[2], c.a[3] };
    uint32_t r1 = 0, r2 = 0;
    const bool attack = c.slot == 155 || c.slot == 160 || c.slot == 546;
    // Havana run 1 (2026-11-23): an attack sent while the copy was still in Navigation (path 1/2/3/4/..) did not attack,
    // or attacked 390 ms late; and the first enter commands were refused by CanEnterFight. So: an attack on a copy that is
    // not in Fight enters first with the attack's target; a refused enter is held and retried (OwnUpdate requeues).
    const int st1 = Phase1_HumanStage1((void*)human);   // HumanGroundStage + 1: 5 = Fight
    // Havana run 13: all 3 missed attacks arrived while the copy was still in the previous attack (path .../173/180 or
    // .../173/182), because the copy runs each attack 25-45 ms behind the host guard and the host starts the next one as it
    // reaches fight idle. The slot call then started nothing. With BrainHitSync on, an attack is held (retried each frame,
    // up to 1 s late) while attack state 173 is on the copy's path, so attacks run in the host's order.
    if (attack && g_hitSync) {
        bool in173 = false;
        __try {
            const uint32_t fsm = *(uint32_t*)(ig + 0x90);
            unsigned d = *(const uint16_t*)(fsm + 0x11E); if (d > 12) d = 12;
            const uint16_t* p = *(const uint16_t* const*)(fsm + 0x118);
            for (unsigned i = 0; i < d; ++i) if (p[i] == 173) in173 = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (in173) { InterlockedIncrement(&c_attackHeld); return false; }
    }
    float dz = -1.0f;
    __try {
        AcquireSRWLockShared(&g_bLock);
        const int k = FindBy(&Body::human, human);
        const uint32_t self = k >= 0 ? g_b[k].ent : 0;
        ReleaseSRWLockShared(&g_bLock);
        if (Rd((void*)self, 0x50) && Rd((void*)t, 0x50)) { dz = *(float*)(self + 0x48) - *(float*)(t + 0x48); if (dz < 0) dz = -dz; }   // M43 (entity+0x40 = M41)
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Havana run 2 (guest 14956): of the 187 refusal lines logged (265 counted; the log is capped), 100 were an enter on a
    // copy already in Fight (stage1 = 5). That is the engine refusing a second enter, not a failure: the command is done.
    if (c.slot == 76 && st1 == 5) {
        InterlockedIncrement(&c_alreadyFight); InterlockedIncrement(&c_exec);
        return true;
    }
    if (c.slot == 76 || (attack && st1 != 5)) {
        uint32_t hnd = *(uint32_t*)(t + 0xC8);          // the entity's HandleContainer* (as CopyCmdTest / GuardStageApply)
        uint32_t ha[4] = { (uint32_t)(uintptr_t)&hnd, 0, 0, 0 };
        r1 = CallSlot(ig, 75, 1, ha);                   // CanEnterFight pushes node 107 ...
        if (r1 & 0xFF) r2 = CallSlot(ig, 76, 1, ha);    // ... DoEnterFight consumes it
        if (!(r1 & 0xFF)) {
            // sub_1D02DF0 (IDA cc/g3) pushes 107 only when the dispatch id u16 *(*(fsm+0x118)+6) == 4 and sub_1CEDC90 returns 2.
            // sub_1CEDC90 returns 3 when (a) the self body's kind (u32 +156 & 7) == 1 and |dz| >= 3.0, or (b) bit 0x20 of
            // *(sub_667230(0,0)+0x14) is clear. dz and the dispatch id are logged here; the global bit is not read (UNVERIFIED
            // which condition refuses at small dz).
            // Runs 3-4: every refusal had gid = 115, with and without GuardActionLane. The whole active path is logged so the
            // state that holds 115 can be named.
            unsigned gid = 0; char ps[96]; ps[0] = 0;
            __try {
                const uint32_t fsm = Rd((void*)(ig + 0x90), 4) ? *(uint32_t*)(ig + 0x90) : 0;
                const uint32_t ss = Rd((void*)(fsm + 0x118), 4) ? *(uint32_t*)(fsm + 0x118) : 0;
                if (Rd((void*)ss, 8)) gid = *(const uint16_t*)(ss + 6);
                unsigned d = Rd((void*)(fsm + 0x11E), 2) ? *(const uint16_t*)(fsm + 0x11E) : 0; if (d > 12) d = 12;
                int o = 0;
                for (unsigned i = 0; i < d && Rd((void*)(ss + 2 * i), 2) && o < 80; ++i)
                    o += _snprintf_s(ps + o, sizeof(ps) - o, _TRUNCATE, i ? "/%u" : "%u", *(const uint16_t*)(ss + 2 * i));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            InterlockedIncrement(&c_enterRefused);
            if (InterlockedIncrement(&c_execLines) <= 200)
                Log("[Brain] enter refused slot=%u id=%016llX stage1=%d gid=%u path=%s dz=%.2f can=%08X lateMs=%d (held)\n", c.slot, (unsigned long long)objId,
                    st1, gid, ps, dz, r1, lateMs);
            return false;
        }
        if (c.slot == 76) a[0] = ha[0];
    }
    if (c.slot == 113) {
        r1 = CallSlot(ig, 112, 1, a);                   // CanExitFight(type)
        if (r1 & 0xFF) r2 = CallSlot(ig, 113, 1, a);    // DoExitFight(type)
    } else if (attack) {
        a[0] = t;
        r2 = CallSlot(ig, c.slot, c.nargs, a);
    }
    InterlockedIncrement(&c_exec);
    if (attack) Watch(human, c.execHostMs);
    if (InterlockedIncrement(&c_execLines) <= 200)
        Log("[Brain] exec role=guest slot=%u id=%016llX tgt=%u target=%08X a=%08X,%08X,%08X stage1=%d dz=%.2f can=%08X do=%08X execMs=%u lateMs=%d\n", c.slot,
            (unsigned long long)objId, c.tgt, t, a[1], a[2], a[3], st1, dz, r1, r2, c.execHostMs, lateMs);
    return true;
}

void WatchPath(int k, uint32_t ig) {   // body k locked by the caller (exclusive)
    Body& b = g_b[k];
    LARGE_INTEGER cq, fq; QueryPerformanceCounter(&cq); QueryPerformanceFrequency(&fq);
    const uint64_t nowUs = (uint64_t)(cq.QuadPart * 1000000.0 / fq.QuadPart);
    if (!b.watchUntilUs || nowUs > b.watchUntilUs) { b.watchUntilUs = 0; return; }
    __try {
        const uint32_t fsm = Rd((void*)(ig + 0x90), 4) ? *(uint32_t*)(ig + 0x90) : 0;   // = [hg+0xA0] (FiPath)
        if (!Rd((void*)fsm, 0x120)) return;
        unsigned d = *(const uint16_t*)(fsm + 0x11E); if (d > 12) d = 12;
        const uint16_t* p = *(const uint16_t* const*)(fsm + 0x118);
        if (!Rd(p, 2 * d)) return;
        bool has = false, changed = d != b.lastDepth;
        for (unsigned i = 0; i < d; ++i) { if (p[i] == 173) has = true; if (p[i] != b.lastPath[i]) changed = true; }
        bool clk = false; const uint32_t now = HostMs(&clk);
        if (changed && b.pathLines < 24) {
            ++b.pathLines;
            char s[96]; int o = 0; s[0] = 0;
            for (unsigned i = 0; i < d && o < 80; ++i) o += _snprintf_s(s + o, sizeof(s) - o, _TRUNCATE, i ? "/%u" : "%u", p[i]);
            Log("[Brain] path role=%s id=%016llX +%dms %s\n", g_role == 1 ? "host" : "guest", (unsigned long long)b.objId, (int)(now - b.watchExec), s);
        }
        for (unsigned i = 0; i < d; ++i) b.lastPath[i] = p[i];
        b.lastDepth = (uint8_t)d;
        if (has && !b.in173) {
            InterlockedIncrement(&c_s173);
            Log("[Brain] s173 role=%s id=%016llX execMs=%u hostMs=%u dt=%dms\n", g_role == 1 ? "host" : "guest", (unsigned long long)b.objId, b.watchExec, now,
                (int)(now - b.watchExec));
        }
        b.in173 = has ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Ground dispatch 115 (docs/evidence/brain-s0-2026-11-22.md, runs 3-9) -------------------------------------------
// Both games enter 115 on their own and leave it after ~1.1 s; a copy in 115 refuses enter fight. Run 9's write watchpoint
// put the write of 115 in the chart's own dispatch: sub_1D80950 -> sub_1D65BB0 (0x1D65CA9) -> sub_1D63440 (enter state)
// -> sub_1CFFFE0 (0x1D0003F, path push). sub_1D65BB0 asks sub_1D634D0(stateId) for each active node; the first nonzero
// code wins (target = low 10 bits, level = bits 10-19). Codes with target 115 from state 4 (0x1073 = 4211), IDA cc/g19-g21:
//   case 4  -> sub_1D0EDE0 (E8 at 0x1D64CDC): 4211 when sub_1CE1E30(ctx) is true (handler sub_1CDCC20)
//   case 58 -> sub_1D23570 (E8 at 0x1D64D03): 4211 on an interface test / float / mode (handler sub_1D1E9D0)
// (sub_1D6EB00, the third 4211, is a forced transition on token 114 and does not run through 0x1D65CA9.) The evaluator's
// `this` is the chart helper (path at +0x118), the same object as *(ig+0x90) of a body: that is how a body is matched.
// [Coop] Brain115Probe (default 0): wrappers on both E8s log every 4211 with the body. [Coop] Brain115Suppress (default 0):
// on the guest, a 4211 for a registered copy becomes 0 ("no transition from this node"), and the handler slot (+8) the
// evaluator wrote is put back, so the copy stays in dispatch 4.
int g_probe115 = 0, g_supp115 = 0;
volatile LONG c115[2] = {}, c115Copy[2] = {}, c115Native[2] = {}, c115Unmatched = 0, c115Lines = 0, c115Supp = 0, c115SelCalls = 0;

int BodyOfFsm(uint32_t fsm, uint64_t* objId) {   // registered body whose Ground helper is fsm, else -1
    AcquireSRWLockShared(&g_bLock);
    int k = fsm ? FindBy(&Body::fsm, fsm) : -1;   // cached by NoteBody; the live read below covers a helper allocated since
    for (int i = 0; i < g_nb && k < 0; ++i) {
        __try { if (*(uint32_t*)(g_b[i].ig + 0x90) == fsm) k = i; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (k >= 0 && objId) *objId = g_b[k].objId;
    ReleaseSRWLockShared(&g_bLock);
    return k;
}
typedef int(__thiscall* EvalFn)(uint32_t self, int a2);
int Eval115(int which, uint32_t self, int a2, EvalFn orig) {
    InterlockedIncrement(&c115SelCalls);
    uint32_t savedHandler = 0;
    __try { savedHandler = *(uint32_t*)(self + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int r = orig(self, a2);
    if (r != 4211) return r;
    uint64_t objId = 0;
    const int k = BodyOfFsm(self, &objId);
    InterlockedIncrement(&c115[which]);
    if (k >= 0) InterlockedIncrement(g_role == 0 ? &c115Copy[which] : &c115Native[which]); else InterlockedIncrement(&c115Unmatched);
    const bool supp = g_supp115 && g_role == 0 && g_on == 1 && k >= 0;
    if (g_probe115 && InterlockedIncrement(&c115Lines) <= 300) {
        bool clk = false; const uint32_t now = HostMs(&clk);
        Log("[Brain] enter115 eval=%s role=%s fsm=%08X id=%016llX matched=%d suppressed=%d hostMs=%u\n", which ? "58" : "4",
            g_role == 1 ? "host" : g_role == 0 ? "guest" : "?", self, (unsigned long long)objId, k >= 0 ? 1 : 0, supp ? 1 : 0, now);
    }
    if (!supp) return r;
    __try { *(uint32_t*)(self + 8) = savedHandler; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    InterlockedIncrement(&c115Supp);
    return 0;
}
int __fastcall Eval4W(uint32_t self, uint32_t, int a2) { return Eval115(0, self, a2, (EvalFn)0x01D0EDE0); }    // thiscall-compatible, ret 4
int __fastcall Eval58W(uint32_t self, uint32_t, int a2) { return Eval115(1, self, a2, (EvalFn)0x01D23570); }

// [Coop] BrainHitSync, attack-substate gate. The exits of attack substates 180/181/182 are each chosen by that state's own
// evaluator (sub_1D634D0 cases 180/181/182, E8s at 0x1D64756 / 0x1D64749 / 0x1D6473C -> sub_1CD8D70 / sub_1CD8DB0 /
// sub_1CD8E20; IDA cc/g22). Havana runs 11-12: copies chose differently from the host guards (180 -> 181 -> re-attack;
// after 182: re-attack or 186 instead of fight idle). Host: a native's nonzero answer is the transition the dispatch takes
// (first nonzero wins), and it is sent as kGate {state, code}. Guest: when a registered copy's evaluator answers nonzero, it
// takes the host's code for that state if a permit (1.5 s) exists - the same code, or the host's code with that code's
// handler written to the handler slot (+8), from the table below (the handler each evaluator writes for each code, cc/g22).
// Without a permit the answer becomes 0 (handler slot restored): the copy waits in that substate.
struct GateCode { int32_t code; uint32_t handler; };
const GateCode kGate180[] = { { 184501, 0x01CCC840u } };
const GateCode kGate181[] = { { 166915, 0x01CCC7C0u }, { 177325, 0x01CCD1C0u }, { 177319, 0x01CCC880u } };
const GateCode kGate182[] = { { 186551, 0x01CCCA00u }, { 177361, 0x01CCCE80u }, { 186554, 0x01CCC8C0u }, { 177325, 0x01CCC900u },
                              { 177319, 0x01CD3200u }, { 166915, 0x01CCDC40u } };
int EvalGate(uint16_t st, uint32_t self, int a2, EvalFn orig, const GateCode* tab, int ntab) {
    uint32_t saved = 0;
    __try { saved = *(uint32_t*)(self + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int r = orig(self, a2);
    if (!r || !g_hitSync || g_on != 1) return r;
    uint64_t objId = 0;
    const int k = BodyOfFsm(self, &objId);
    if (k < 0) return r;
    if (g_role == 1) {
        bool clk = false; const uint32_t now = HostMs(&clk);
        if (clk && PeerReady()) { Cmd c = {}; c.slot = kGate; c.a[0] = st; c.a[1] = (uint32_t)r; c.execHostMs = now; Publish(objId, c); InterlockedIncrement(&c_gate181Tx); }
        return r;
    }
    if (g_role != 0) return r;
    int32_t want = 0;
    AcquireSRWLockExclusive(&g_bLock);
    if (k < g_nb && g_b[k].objId == objId)
        for (int i = 0; i < 4; ++i)
            if (g_b[k].permit[i].st == st && g_b[k].permit[i].untilMs > GetTickCount64()) { want = g_b[k].permit[i].code; g_b[k].permit[i].untilMs = 0; break; }
    ReleaseSRWLockExclusive(&g_bLock);
    if (want == r) { InterlockedIncrement(&c_gate181Allowed); return r; }
    if (want) {
        for (int i = 0; i < ntab; ++i)
            if (tab[i].code == want) {
                __try { *(uint32_t*)(self + 8) = tab[i].handler; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                InterlockedIncrement(&c_gateSubst);
                if (InterlockedIncrement(&c_hitLines) <= 120) Log("[Brain] gate st=%u id=%016llX copy=%d host=%d -> host\n", st, (unsigned long long)objId, r, want);
                return want;
            }
        if (InterlockedIncrement(&c_hitLines) <= 120) Log("[Brain] gate st=%u id=%016llX host code %d not in table (copy %d): blocked\n", st, (unsigned long long)objId, want, r);
    }
    __try { *(uint32_t*)(self + 8) = saved; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    InterlockedIncrement(&c_gate181Blocked);
    return 0;
}

// [Coop] BrainChartSync (default 0; needs BrainHitSync). Havana run 15: after the attack-substate gate, copies still lagged
// because every other fight substate exit is the copy's own decision (sub-step 178 lasted ~700 ms longer on a copy than on its
// host guard). One call carries all of them: the dispatch sub_1D65BB0 asks sub_1D634D0(node, a2) for each active node
// (E8 at 0x1D65BE1: push a2; push node; ecx = fsm; IDA cc/g28), and the first nonzero code is taken; the evaluator writes
// the transition's handler at fsm+8. Scope: nodes strictly below 163 (the fight root) on the path, and only calls with
// a2 == 0 (event calls carry pointer arguments and stay native).
//   Host: a registered native's nonzero answer is sent as kChart {node, code, handler}.
//   Guest: a registered copy makes no such decision itself. For the node being asked, if the host's queue has an entry for
//   it: take the host's code and handler when the copy's own evaluator is ready (nonzero) or the entry is >= 100 ms old;
//   drop it and every older entry. Otherwise the answer is 0 and fsm+8 is restored (the copy waits). Entries expire at
//   1.5 s. The handler address is the host's, valid here because both run the same exe (build gate).
typedef int(__thiscall* NodeFn)(uint32_t fsm, int node, int a2);
volatile LONG c_holdSet = 0, c_holdApplied = 0, c_holdExpired = 0;
volatile LONG c_chartTx = 0, c_chartApplied = 0, c_chartOwn = 0, c_chartForced = 0, c_chartWait = 0, c_chartExpired = 0, c_chartLines = 0;
int __fastcall NodeW(uint32_t fsm, uint32_t, int node, int a2) {
    uint32_t saved = 0;
    __try { saved = *(uint32_t*)(fsm + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int r = ((NodeFn)0x01D634D0)(fsm, node, a2);
    if (g_chartSync && g_on == 1 && g_role == 1 && a2 != 0 && r) {   // probe: event-driven transitions on registered host bodies
        uint64_t eo = 0;
        if (BodyOfFsm(fsm, &eo) >= 0 && InterlockedIncrement(&c_evtLines) <= 400) {
            uint32_t ev[6] = {};
            __try { for (int i = 0; i < 6; ++i) ev[i] = ((const uint32_t*)(uintptr_t)a2)[i]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            uint32_t handler = 0;
            __try { handler = *(uint32_t*)(fsm + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            bool clk = false; const uint32_t now = HostMs(&clk);
            Log("[Brain] evt id=%016llX node=%d code=%d (-> %d, level %d) handler=%08X a2=%08X [%08X %08X %08X %08X %08X %08X] hostMs=%u\n",
                (unsigned long long)eo, node, r, r & 0x3FF, (r >> 10) & 0x3FF, handler, (uint32_t)a2, ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], now);
        }
    }
    if (!g_chartSync || g_on != 1 || a2 != 0 || g_role < 0) return r;
    if (g_role == 1 && !r && !g_playoutMs) return r;   // host: only answers are interesting (and held attack starts)
    bool below163 = false;
    __try {
        unsigned d = *(const uint16_t*)(fsm + 0x11E); if (d > 12) d = 12;
        const uint16_t* p = *(const uint16_t* const*)(fsm + 0x118);
        int i163 = -1, in = -1;
        for (unsigned i = 0; i < d; ++i) { if (p[i] == 163 && i163 < 0) i163 = (int)i; if (p[i] == (uint16_t)node && in < 0) in = (int)i; }
        // Run 21 smoke: node 163 itself chooses the fight branch (166 / 268 / 317) and leaving the fight; with it excluded the
        // puppet left the fight on its own while the host player went 163/317 and 163/268. So 163 is inside the region.
        below163 = i163 >= 0 && in >= i163;
    } __except (EXCEPTION_EXECUTE_HANDLER) { below163 = false; }
    if (!below163) return r;
    uint64_t objId = 0;
    const int k = BodyOfFsm(fsm, &objId);
    if (k < 0) return r;
    if (g_role == 1) {
        bool clk = false; const uint32_t now = HostMs(&clk);
        uint32_t handler = 0;
        __try { handler = *(uint32_t*)(fsm + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // [Coop] BrainPlayoutMs (design section 3, "the host delays its own execution by the same amount"): only attack starts
        // (a transition whose target, the low 10 bits, is 173) are held, so animations keep their native length. The host
        // sends the kept attack call at once (the copy only fills its request record with it) and the transition stamped
        // now + D; it holds its own transition for D, freezing this guard's other fight transitions meanwhile (at most 500 ms).
        if (objId == kPuppetWire) {   // [Coop] BrainPuppet: the host player is never held (no input delay). Each transition goes at
            // once and is applied by the puppet as soon as it arrives (a[3] = 1); an attack start carries the kept attack call.
            if (!r) return r;
            if (clk && PeerReady()) {
                if (!(r & 0x80000000) && (r & 0x3FF) == 173) {
                    Cmd atk; bool haveAtk = false;
                    AcquireSRWLockExclusive(&g_bLock);
                    if (k < g_nb && g_b[k].objId == objId && g_b[k].pendValid) { atk = g_b[k].pendAtk; g_b[k].pendValid = 0; haveAtk = true; }
                    ReleaseSRWLockExclusive(&g_bLock);
                    if (haveAtk) { atk.execHostMs = now; Publish(objId, atk); InterlockedIncrement(&c_atkSent); }
                    else InterlockedIncrement(&c_atkNoCall);
                }
                Cmd c = {}; c.slot = kChart; c.a[0] = (uint32_t)node; c.a[1] = (uint32_t)r; c.a[2] = handler; c.a[3] = 1; c.execHostMs = now;
                Publish(objId, c); InterlockedIncrement(&c_chartTx); InterlockedIncrement(&c_puppetTx);
            }
            return r;
        }
        if (g_playoutMs && clk) {
            const uint64_t tick = GetTickCount64();
            bool holding = false, due = false; uint16_t hn = 0; int32_t hc = 0; uint32_t hh = 0;
            AcquireSRWLockExclusive(&g_bLock);
            if (k < g_nb && g_b[k].objId == objId && g_b[k].hold.setMs) {
                if (tick - g_b[k].hold.setMs > 500) { g_b[k].hold.setMs = 0; InterlockedIncrement(&c_holdExpired); }
                else {
                    holding = true; hn = g_b[k].hold.node; hc = g_b[k].hold.code; hh = g_b[k].hold.handler;
                    due = (int32_t)(now - g_b[k].hold.dueHostMs) >= 0;
                    if (due && hn == (uint16_t)node) g_b[k].hold.setMs = 0;
                }
            }
            ReleaseSRWLockExclusive(&g_bLock);
            if (holding) {
                if (due && hn == (uint16_t)node) {
                    __try { *(uint32_t*)(fsm + 8) = hh; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    InterlockedIncrement(&c_holdApplied);
                    return hc;
                }
                __try { *(uint32_t*)(fsm + 8) = saved; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
            if (r && !(r & 0x80000000) && (r & 0x3FF) == 173 && PeerReady()) {
                Cmd atk; bool haveAtk = false; uint32_t human = 0;
                AcquireSRWLockExclusive(&g_bLock);
                if (k < g_nb && g_b[k].objId == objId) {
                    Body& b = g_b[k];
                    b.hold.node = (uint16_t)node; b.hold.code = r; b.hold.handler = handler; b.hold.dueHostMs = now + (uint32_t)g_playoutMs; b.hold.setMs = tick;
                    if (b.pendValid) { atk = b.pendAtk; b.pendValid = 0; haveAtk = true; }
                    human = b.human;
                }
                ReleaseSRWLockExclusive(&g_bLock);
                if (haveAtk) {
                    atk.execHostMs = now; Publish(objId, atk); InterlockedIncrement(&c_atkSent);
                    Watch(human, now);   // both sides' s173 lines key on this execMs (tools/brain_check.py)
                } else InterlockedIncrement(&c_atkNoCall);
                Cmd c = {}; c.slot = kChart; c.a[0] = (uint32_t)node; c.a[1] = (uint32_t)r; c.a[2] = handler; c.a[3] = 1;
                c.execHostMs = now + (uint32_t)g_playoutMs;
                Publish(objId, c); InterlockedIncrement(&c_chartTx); InterlockedIncrement(&c_holdSet);
                if (InterlockedIncrement(&c_chartLines) <= 150)
                    Log("[Brain] hold role=host id=%016llX node=%d code=%d (-> 173) dueMs=%u atk=%d\n", (unsigned long long)objId, node, r, now + g_playoutMs, haveAtk ? 1 : 0);
                __try { *(uint32_t*)(fsm + 8) = saved; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
            if (!r) return r;
        }
        if (clk && PeerReady()) {
            Cmd c = {}; c.slot = kChart; c.a[0] = (uint32_t)node; c.a[1] = (uint32_t)r; c.a[2] = handler; c.execHostMs = now;
            Publish(objId, c); InterlockedIncrement(&c_chartTx);
            if (InterlockedIncrement(&c_chartLines) <= 150)
                Log("[Brain] chart role=host id=%016llX node=%d code=%d (-> %d) handler=%08X hostMs=%u\n", (unsigned long long)objId, node, r, r & 0x3FF, handler, now);
        }
        return r;
    }
    // guest copy
    const uint64_t ms = GetTickCount64();
    int32_t code = 0; uint32_t handler = 0; bool apply = false; uint64_t age = 0;
    AcquireSRWLockExclusive(&g_bLock);
    if (k < g_nb && g_b[k].objId == objId) {
        Body& b = g_b[k];
        int j = 0;   // expire
        for (int i = 0; i < b.cqn; ++i) { if (ms - b.cq[i].atMs <= 1500) b.cq[j++] = b.cq[i]; else InterlockedIncrement(&c_chartExpired); }
        b.cqn = j;
        // Newest matching entry first (run 16: fight idle flips 168 <-> 169 on the host, and taking the oldest entry replayed
        // stale idle steps while the host had already attacked; 32 of 47 applications were such substitutions).
        for (int i = b.cqn - 1; i >= 0; --i)
            if (b.cq[i].node == (uint16_t)node) {
                age = ms - b.cq[i].atMs;
                if (r || age >= 100 || b.cq[i].sched) {   // sched: a playout-delayed entry, released by Execute at its execMs
                    code = b.cq[i].code; handler = b.cq[i].handler; apply = true;
                    int m = 0; for (int q = i + 1; q < b.cqn; ++q) b.cq[m++] = b.cq[q];   // drop it and every older entry
                    b.cqn = m;
                }
                break;
            }
    }
    ReleaseSRWLockExclusive(&g_bLock);
    if (apply) {
        __try { *(uint32_t*)(fsm + 8) = handler; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        InterlockedIncrement(&c_chartApplied); InterlockedIncrement(r == code ? &c_chartOwn : &c_chartForced);
        if (InterlockedIncrement(&c_chartLines) <= 150)
            Log("[Brain] chart role=guest id=%016llX node=%d own=%d host=%d (-> %d) ageMs=%llu\n", (unsigned long long)objId, node, r, code, code & 0x3FF,
                (unsigned long long)age);
        return code;
    }
    __try { *(uint32_t*)(fsm + 8) = saved; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (r) InterlockedIncrement(&c_chartWait);
    return 0;
}
int __fastcall Eval180W(uint32_t self, uint32_t, int a2) { return EvalGate(180, self, a2, (EvalFn)0x01CD8D70, kGate180, 1); }
int __fastcall Eval181W(uint32_t self, uint32_t, int a2) { return EvalGate(181, self, a2, (EvalFn)0x01CD8DB0, kGate181, 3); }
int __fastcall Eval182W(uint32_t self, uint32_t, int a2) { return EvalGate(182, self, a2, (EvalFn)0x01CD8E20, kGate182, 6); }


// [Coop] Brain115Watch (default 0): run 7 showed routes A/B explain 7 of 71 entries into 115, so the writer is found with a
// hardware write watchpoint: DR0 = the u16 path[3] of one registered body in dispatch 4 (LEN 2, R/W 01 = write), set on
// every thread of the process (suspend, Set/GetThreadContext, resume; re-applied every 5 s for new threads). The vectored
// handler logs, for writes of 115 (and the first few other values), the eip and every dword on the 64 stack slots above
// esp that points into the exe's code (0x401000..0x2400000). Up to 12 hits of 115, then DR7 is cleared everywhere.
volatile LONG g_wAddr = 0, g_wHits115 = 0, g_wHitsOther = 0, g_wDone = 0;
uint64_t g_wObj = 0; PVOID g_wVeh = nullptr;
LONG WINAPI WatchVeh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;   // not our DR0
    c->Dr6 = 0;
    const uint32_t a = (uint32_t)g_wAddr;
    uint16_t v = 0;
    __try { v = *(volatile uint16_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return EXCEPTION_CONTINUE_EXECUTION; }
    const bool is115 = v == 115;
    const LONG n = is115 ? InterlockedIncrement(&g_wHits115) : InterlockedIncrement(&g_wHitsOther);
    if ((is115 && n <= 12) || (!is115 && n <= 6)) {
        char s[400]; int o = _snprintf_s(s, sizeof(s), _TRUNCATE, "[Brain] watch id=%016llX path[3]=%u eip=%08X tid=%u stack:", (unsigned long long)g_wObj, v,
                                         (unsigned)c->Eip, GetCurrentThreadId());
        __try {
            const uint32_t* sp = (const uint32_t*)c->Esp;
            for (int i = 0; i < 64 && o < 380; ++i) { const uint32_t w = sp[i]; if (w >= 0x00401000u && w < 0x02400000u) o += _snprintf_s(s + o, sizeof(s) - o, _TRUNCATE, " %08X", w); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("%s\n", s);
    }
    if (is115 && n >= 12) InterlockedExchange(&g_wDone, 1);   // Tick clears DR7 on all threads
    return EXCEPTION_CONTINUE_EXECUTION;
}
void WatchSetAll(uint32_t addr) {   // addr 0 clears
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        const bool me = te.th32ThreadID == self;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (!me && SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
        CONTEXT cx; memset(&cx, 0, sizeof(cx)); cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (me) {   // the current thread: its context cannot be set while it runs; DR0 for it is set on the next pass
        } else if (GetThreadContext(th, &cx)) {
            cx.Dr0 = addr;
            cx.Dr7 = addr ? ((cx.Dr7 & ~0x000F0003u) | 0x1u | (0x1u << 16) | (0x1u << 18)) : (cx.Dr7 & ~0x000F0003u);   // L0, R/W0 = 01, LEN0 = 01 (2 B)
            if (SetThreadContext(th, &cx)) ++n;
        }
        if (!me) ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    static int s_lines = 0;
    if (s_lines++ < 6) Log("[Brain] watch %s %08X on %d threads\n", addr ? "set" : "cleared", addr, n);
}

// Rewrite the rel32 of one E8 call at `site` from `expect` to `to`. Runs at load (Brain::Init from dllmain), before any world.
bool PatchCall(uint32_t site, uint32_t expect, uint32_t to) {
    uint8_t* p = (uint8_t*)(uintptr_t)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != expect) {
        Log("[Brain] call %08X is %02X %08X, expected E8 -> %08X: not patched\n", site, p[0], site + 5 + *(int32_t*)(p + 1), expect);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(p + 1) = (int32_t)(to - (site + 5));
    VirtualProtect(p + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
// [Coop] BrainEnterProbe (default 0): log every Ground state entry on a registered body, host and guest. Run 21 smoke 3: no
// dispatch call on these bodies carries an event (a2 != 0), so hit reactions / blocks are forced transitions: a caller runs a
// handler with its event, then sub_1D659D0(fsm, code) (280 callers, IDA cc/g30) exits to the level and enters the target
// through sub_1D63440. sub_1D63440 has 3 callers, each one E8: 0x1D65A36 (sub_1D659D0, forced), 0x1D65ADD (sub_1D65A50),
// 0x1D65CA4 (sub_1D65BB0, the dispatch). The wrapper logs the route and the code addresses above it on the stack, which
// name the function that forced the transition.
typedef int(__thiscall* EnterFn)(uint32_t fsm, int target);
volatile LONG c_enterLines = 0;
int EnterLog(int route, uint32_t fsm, int target, void** retSlot) {
    uint64_t objId = 0;
    if (BodyOfFsm(fsm, &objId) >= 0 && InterlockedIncrement(&c_enterLines) <= 3000) {
        char s[220]; int o = 0; s[0] = 0;
        __try {
            const uint32_t* sp = (const uint32_t*)retSlot;
            for (int i = 1, n = 0; i < 40 && n < 6 && o < 200; ++i) {
                const uint32_t w = sp[i];
                if (w >= 0x00401000u && w < 0x02400000u) { o += _snprintf_s(s + o, sizeof(s) - o, _TRUNCATE, " %08X", w); ++n; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        bool clk = false; const uint32_t now = HostMs(&clk);
        Log("[Brain] enter role=%s id=%016llX state=%d route=%c hostMs=%u stack:%s\n", g_role == 1 ? "host" : "guest", (unsigned long long)objId, target,
            route == 0 ? 'F' : route == 1 ? 'A' : 'D', now, s);
    }
    return 0;
}
int __fastcall EnterF(uint32_t fsm, uint32_t, int target) { EnterLog(0, fsm, target, (void**)_AddressOfReturnAddress()); return ((EnterFn)0x01D63440)(fsm, target); }
int __fastcall EnterA(uint32_t fsm, uint32_t, int target) { EnterLog(1, fsm, target, (void**)_AddressOfReturnAddress()); return ((EnterFn)0x01D63440)(fsm, target); }
int __fastcall EnterD(uint32_t fsm, uint32_t, int target) { EnterLog(2, fsm, target, (void**)_AddressOfReturnAddress()); return ((EnterFn)0x01D63440)(fsm, target); }

void Install115() {
    g_probe115 = GetPrivateProfileIntA("Coop", "Brain115Probe", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_supp115 = GetPrivateProfileIntA("Coop", "Brain115Suppress", 0, Paths::Ini()) != 0 ? 1 : 0;
    if (GetPrivateProfileIntA("Coop", "BrainEnterProbe", 0, Paths::Ini()) != 0) {
        const bool f = PatchCall(0x01D65A36u, 0x01D63440u, (uint32_t)(uintptr_t)&EnterF);
        const bool a = PatchCall(0x01D65ADDu, 0x01D63440u, (uint32_t)(uintptr_t)&EnterA);
        const bool d = PatchCall(0x01D65CA4u, 0x01D63440u, (uint32_t)(uintptr_t)&EnterD);
        Log("[Brain] enter probe: forced %s, 1D65A50 %s, dispatch %s\n", f ? "wrapped" : "NOT", a ? "wrapped" : "NOT", d ? "wrapped" : "NOT");
    }
    if (g_hitSync && g_chartSync) {   // the node mirror covers 180/181/182 too, so the per-state gate is not installed
        const bool n = PatchCall(0x01D65BE1u, 0x01D634D0u, (uint32_t)(uintptr_t)&NodeW);
        Log("[Brain] chart sync: dispatch node call %s\n", n ? "wrapped" : "NOT wrapped");
    } else if (g_hitSync) {
        const bool h0 = PatchCall(0x01D64756u, 0x01CD8D70u, (uint32_t)(uintptr_t)&Eval180W);
        const bool h1 = PatchCall(0x01D64749u, 0x01CD8DB0u, (uint32_t)(uintptr_t)&Eval181W);
        const bool h2 = PatchCall(0x01D6473Cu, 0x01CD8E20u, (uint32_t)(uintptr_t)&Eval182W);
        Log("[Brain] hit sync: evaluators 180 %s, 181 %s, 182 %s\n", h0 ? "wrapped" : "NOT wrapped", h1 ? "wrapped" : "NOT wrapped", h2 ? "wrapped" : "NOT wrapped");
    }
    if (!g_probe115 && !g_supp115) return;
    const bool a = PatchCall(0x01D64CDCu, 0x01D0EDE0u, (uint32_t)(uintptr_t)&Eval4W);
    const bool b = PatchCall(0x01D64D03u, 0x01D23570u, (uint32_t)(uintptr_t)&Eval58W);
    Log("[Brain] 115 evaluators: case 4 %s, case 58 %s (probe=%d suppress=%d)\n", a ? "wrapped" : "NOT wrapped", b ? "wrapped" : "NOT wrapped",
        g_probe115, g_supp115);
}

}  // namespace

namespace Brain {

void Init() {
    if (g_on >= 0) return;
    g_on = GetPrivateProfileIntA("Coop", "Brain", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_delayMs = GetPrivateProfileIntA("Coop", "BrainDelayMs", 0, Paths::Ini());
    g_test = GetPrivateProfileIntA("Coop", "BrainTest", 0, Paths::Ini());
    g_testDelayMs = GetPrivateProfileIntA("Coop", "BrainTestDelayMs", 150, Paths::Ini());
    g_hitSync = GetPrivateProfileIntA("Coop", "BrainHitSync", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_chartSync = GetPrivateProfileIntA("Coop", "BrainChartSync", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_playoutMs = GetPrivateProfileIntA("Coop", "BrainPlayoutMs", 0, Paths::Ini());
    g_puppet = GetPrivateProfileIntA("Coop", "BrainPuppet", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_react = GetPrivateProfileIntA("Coop", "BrainReact", 0, Paths::Ini()) != 0 ? 1 : 0;
    if (g_playoutMs < 0 || g_playoutMs > 500) g_playoutMs = 0;
    if (g_on) Log("[Brain] Brain=1 delayMs=%d test=%d testDelayMs=%d (needs IntentLane=1, PeerClock=1)\n", g_delayMs, g_test, g_testDelayMs);
    if (g_on) Install115();
}

bool On() { return g_on == 1; }
bool PuppetMirrorGuest() { return g_on == 1 && g_puppet && g_chartSync && g_role == 0; }
bool VetoLocalFde(const void* ev, const void* recv) {
    if (!g_react || g_on != 1 || g_role != 0 || !ev || !recv) return false;
    __try {
        if (*(const uint32_t*)ev != 0x02700720u) return false;   // FightDamageEvent only
        const uint32_t ah = *(const uint32_t*)((const char*)ev + 0x58);
        const uint32_t att = ah && *(const int32_t*)(ah + 8) < 0 ? *(const uint32_t*)ah : 0;
        const uint32_t pup = (uint32_t)(uintptr_t)Phase1_PuppetEntity(), r = (uint32_t)(uintptr_t)recv;
        AcquireSRWLockShared(&g_bLock);
        const bool recvBody = r == pup || FindBy(&Body::ent, r) >= 0;
        const bool attBody = att && (att == pup || FindBy(&Body::ent, att) >= 0);
        ReleaseSRWLockShared(&g_bLock);
        if (recvBody && attBody) { InterlockedIncrement(&c_reactVeto); return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

}  // namespace Brain

// ---- [Coop] BrainCensus (default 0): every interface call made on a registered body ---------------------------------------
// Owner fight 2026-11-24 (run 20) and the run-21 smokes: the puppet / copies miss whatever the host body gets through
// interface calls (hit reactions are forced by 0x026E7D40 slots 12/14 with a damage event, the hit outcome by IHumanGround
// 209/210, ...). Instead of finding them one at a time, this hooks every slot of all 15 interface vtables HumanGround's
// constructor sub_16D8690 installs (0x016D8761..0x016D87C9; offsets +0x10..+0x48 in the object; 1006 slots, read from the exe
// 2026-11-24) and records, per slot and per body class (player/puppet, guard), the count and the first 4 calls with return
// address, 4 stack dwords and the body's Ground path. A call on any other object costs one compare against a 64-entry snapshot.
namespace {
struct CVt { uint32_t off, vt; };
// Slot counts per vtable, read from the exe 2026-11-24 (tools/pe.py: code dwords up to the next vtable). At runtime other mod
// features hook some IHumanGround entries, so the walk must not stop at a non-exe value (first census build stopped at 165).
const uint16_t kCVtSlots[] = { 841, 2, 1, 29, 2, 2, 2, 3, 20, 10, 6, 17, 63, 4, 4 };
const CVt kCVt[] = { { 0x10, 0x026E7DC0u }, { 0x14, 0x026E7DB8u }, { 0x18, 0x026E7DB4u }, { 0x1C, 0x026E7D40u }, { 0x20, 0x026E7D34u },
                     { 0x24, 0x026E7D2Cu }, { 0x28, 0x026E7D24u }, { 0x2C, 0x026E7D18u }, { 0x30, 0x026E7CC8u }, { 0x34, 0x026E7C9Cu },
                     { 0x38, 0x026E7C84u }, { 0x3C, 0x026E7C40u }, { 0x40, 0x026E7B40u }, { 0x44, 0x026E7B2Cu }, { 0x48, 0x026E7B1Cu } };
const uint32_t kCVtEnd = 0x026E8AE8u;   // HumanGround's own vtable follows the interface block
struct CSlot { uint16_t vti, slot; uint32_t target; volatile LONG n[2]; };
const int kCMax = 1100;
CSlot g_cs[kCMax]; int g_csN = 0;
uint8_t* g_cstubs = nullptr;
volatile uint32_t g_cIg[64]; volatile LONG g_cIgN = 0;   // registered bodies' IHumanGround pointers, refreshed by Tick
volatile uint32_t g_cPlayerIg = 0;                         // the kPuppetWire body's
volatile LONG c_censusLines = 0;
int g_census = 0;

void __cdecl CensusCall(uint32_t id, uint32_t ecx, const uint32_t* frame) {
    if (id >= (uint32_t)g_csN) return;
    const uint32_t ig = ecx - kCVt[g_cs[id].vti].off + 0x10;
    const LONG n = g_cIgN;
    bool hit = false;
    for (LONG i = 0; i < n && i < 64; ++i) if (g_cIg[i] == ig) { hit = true; break; }
    if (!hit) return;
    const int cls = ig == g_cPlayerIg ? 0 : 1;
    const LONG c = InterlockedIncrement(&g_cs[id].n[cls]);
    if (c > 4 || InterlockedIncrement(&c_censusLines) > 4000) return;
    __try {
        char ps[96]; PathStr(ig, ps, sizeof(ps));
        bool clk = false; const uint32_t now = HostMs(&clk);
        Log("[Brain] census role=%s cls=%s vt+%02X slot=%u fn=%08X ret=%08X a=%08X,%08X,%08X,%08X path=%s hostMs=%u\n", g_role == 1 ? "host" : "guest",
            cls ? "guard" : "player", kCVt[g_cs[id].vti].off, g_cs[id].slot, g_cs[id].target, frame[0], frame[1], frame[2], frame[3], frame[4], ps, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void EmitCensus(uint8_t* p, uint32_t id, uint32_t orig) {
    int k = 0;
    auto b = [&](uint8_t v) { p[k++] = v; };
    auto d = [&](uint32_t v) { memcpy(p + k, &v, 4); k += 4; };
    b(0x60); b(0x9C); b(0x8D); b(0x44); b(0x24); b(0x24); b(0x50); b(0x51);   // pushad; pushfd; lea eax,[esp+24h]; push eax; push ecx
    b(0x68); d(id); b(0xB8); d((uint32_t)(uintptr_t)&CensusCall); b(0xFF); b(0xD0); b(0x83); b(0xC4); b(0x0C);
    b(0x9D); b(0x61); b(0x68); d(orig); b(0xC3);
}
void InstallCensus() {
    uint32_t starts[16]; int ns = 0;
    for (const CVt& v : kCVt) starts[ns++] = v.vt;
    g_cstubs = (uint8_t*)VirtualAlloc(nullptr, kCMax * 40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_cstubs) { Log("[Brain] census: VirtualAlloc failed\n"); return; }
    int hooked = 0, skipped = 0;
    for (int vi = 0; vi < (int)(sizeof(kCVt) / sizeof(kCVt[0])); ++vi) {
        uint32_t next = kCVtEnd;
        for (int j = 0; j < ns; ++j) if (starts[j] > kCVt[vi].vt && starts[j] < next) next = starts[j];
        uint32_t* tab = (uint32_t*)(uintptr_t)kCVt[vi].vt;
        for (uint32_t s = 0; s < kCVtSlots[vi] && kCVt[vi].vt + 4 * s < next && g_csN < kCMax; ++s) {
            const uint32_t cur = tab[s];
            // An exe code address, or one of the brain's own capture stubs (installed first; wrapped, so both run).
            const bool ours = g_stubs && cur >= (uint32_t)(uintptr_t)g_stubs && cur < (uint32_t)(uintptr_t)g_stubs + kNSlots * 40;
            if (!ours && (cur < 0x00401000u || cur >= 0x02400000u)) ++skipped;   // another feature's hook: wrapped as it is (target logged = that hook)
            const int id = g_csN++;
            g_cs[id].vti = (uint16_t)vi; g_cs[id].slot = (uint16_t)s; g_cs[id].target = ours ? g_orig[(cur - (uint32_t)(uintptr_t)g_stubs) / 40] : cur;
            uint8_t* stub = g_cstubs + id * 40;
            EmitCensus(stub, (uint32_t)id, cur);
            if (SwapSlot(&tab[s], cur, (uint32_t)(uintptr_t)stub)) ++hooked;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), g_cstubs, kCMax * 40);
    Log("[Brain] census: %d slots hooked over %d vtables (skipped %d)\n", hooked, (int)(sizeof(kCVt) / sizeof(kCVt[0])), skipped);
}
void CensusTick(uint64_t ms) {
    if (!g_census || !g_cstubs) return;
    AcquireSRWLockShared(&g_bLock);   // snapshot of the registered bodies' interface pointers
    LONG n = 0;
    for (int i = 0; i < g_nb && n < 64; ++i) { g_cIg[n++] = g_b[i].ig; if (g_b[i].objId == kPuppetWire) g_cPlayerIg = g_b[i].ig; }
    ReleaseSRWLockShared(&g_bLock);
    InterlockedExchange(&g_cIgN, n);
    static uint64_t s_next = 0; static LONG s_last[kCMax][2];
    if (ms < s_next) return;
    s_next = ms + 15000;
    char line[400]; int o = 0; line[0] = 0;
    for (int i = 0; i < g_csN; ++i)
        for (int c = 0; c < 2; ++c) {
            const LONG v = g_cs[i].n[c];
            if (v == s_last[i][c]) continue;
            s_last[i][c] = v;
            o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " %s+%02X:%u=%ld", c ? "g" : "p", kCVt[g_cs[i].vti].off, g_cs[i].slot, v);
            if (o > 330) { Log("[Brain] census sum%s\n", line); o = 0; line[0] = 0; }
        }
    if (o) Log("[Brain] census sum%s\n", line);
}
}  // namespace

namespace Brain {

void SetRole(bool isHost) {
    if (!On() || InterlockedCompareExchange(&g_role, isHost ? 1 : 0, -1) != -1) return;
    Log("[Brain] role=%s\n", isHost ? "host" : "guest");
    if (isHost) InstallCapture();
    g_census = GetPrivateProfileIntA("Coop", "BrainCensus", 0, Paths::Ini()) != 0 ? 1 : 0;
    if (g_census) InstallCensus();   // after the capture hooks: wraps them, so both run
}

void NoteBody(uint64_t objId, void* human, void* entity) {
    if (!On() || !objId || !human) return;
    const uint32_t h = (uint32_t)(uintptr_t)human, ig = IgOf(h);
    if (!ig) {   // run 3: one guest copy (bound, slot 23) never reached the table; say why
        static uint64_t s_seen[32]; static int s_n = 0;
        bool seen = false; for (int i = 0; i < s_n; ++i) if (s_seen[i] == objId) seen = true;
        if (!seen && s_n < 32) {
            s_seen[s_n++] = objId;
            uint32_t hg = 0, v0 = 0, v1 = 0;
            __try { if (Rd((void*)(h + 0x720), 4)) hg = *(uint32_t*)(h + 0x720); if (Rd((void*)hg, 0x20)) { v0 = *(uint32_t*)hg; v1 = *(uint32_t*)(hg + 0x10); } }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            Log("[Brain] body refused id=%016llX human=%08X hg=%08X vt=%08X igvt=%08X\n", (unsigned long long)objId, h, hg, v0, v1);
        }
        return;
    }
    AcquireSRWLockExclusive(&g_bLock);
    int k = FindObj(objId);
    if (k < 0) k = FindBy(&Body::human, h);
    if (k < 0) {
        if (g_nb < kBodies) { k = g_nb++; memset(&g_b[k], 0, sizeof(g_b[k])); }
        else {   // full: reuse the stalest
            k = 0; for (int i = 1; i < g_nb; ++i) if (g_b[i].seenMs < g_b[k].seenMs) k = i;
            memset(&g_b[k], 0, sizeof(g_b[k]));
        }
    }
    g_b[k].human = h; g_b[k].ent = (uint32_t)(uintptr_t)entity; g_b[k].ig = ig; g_b[k].objId = objId; g_b[k].seenMs = GetTickCount64();
    __try { g_b[k].fsm = *(uint32_t*)(ig + 0x90); } __except (EXCEPTION_EXECUTE_HANDLER) { g_b[k].fsm = 0; }
    ReleaseSRWLockExclusive(&g_bLock);
}

void ForgetBody(void* human) {
    if (!On()) return;
    AcquireSRWLockExclusive(&g_bLock);
    const int k = FindBy(&Body::human, (uint32_t)(uintptr_t)human);
    if (k >= 0) g_b[k] = g_b[--g_nb];
    ReleaseSRWLockExclusive(&g_bLock);
}

void OnIntent(uint64_t wireId, uint32_t seq, uint8_t flags, const void* payload, uint8_t len) {
    if (!On() || g_role != 0 || len != sizeof(Cmd)) return;
    Cmd c; memcpy(&c, payload, sizeof(c));
    InterlockedIncrement(&c_rx);
    bool clk = false; const uint32_t now = HostMs(&clk);
    if ((flags & IF_BASE) && clk && (int32_t)(now - c.execHostMs) > 1000) { InterlockedIncrement(&c_rxStaleBase); return; }   // a replayed old command
    // Audit I-102: each command runs at most once. The lane's chanSeq is strictly increasing per (guard, channel) within one
    // host incarnation, and a rejoin replays the last command as a base with its old seq. So: drop seq <= the last one
    // accepted for this guard; forget everything when the host's lane incarnation changes (its seqs restart).
    {
        Net::IntentStats st; Net::GetIntentStats(st);
        AcquireSRWLockExclusive(&g_seqLock);
        if (st.peerInc != g_seqInc) { g_seqInc = st.peerInc; g_nSeq = 0; }
        int k = -1;
        for (int i = 0; i < g_nSeq; ++i) if (g_seq[i].objId == wireId) { k = i; break; }
        const bool dup = k >= 0 && (int32_t)(seq - g_seq[k].seq) <= 0;
        if (!dup) {
            if (k < 0) { k = g_nSeq < kSeq ? g_nSeq++ : (int)(wireId % kSeq); g_seq[k].objId = wireId; }
            g_seq[k].seq = seq;
        }
        ReleaseSRWLockExclusive(&g_seqLock);
        if (dup) { InterlockedIncrement(&c_dupDropped); return; }
    }
    AcquireSRWLockExclusive(&g_qLock);
    int k = -1;
    for (int i = 0; i < kQ; ++i) if (!g_q[i].used) { k = i; break; }
    if (k >= 0) { g_q[k].objId = wireId; g_q[k].c = c; g_q[k].used = 1; }
    ReleaseSRWLockExclusive(&g_qLock);
    if (k < 0) InterlockedIncrement(&c_queueFull);
}

void OwnUpdate(void* human) {
    if (!On() || g_role < 0) return;
    const uint32_t h = (uint32_t)(uintptr_t)human;
    uint64_t objId = 0; uint32_t ig = 0; bool watching = false;
    AcquireSRWLockShared(&g_bLock);
    const int k = FindBy(&Body::human, h);
    uint16_t lastGid = 0;
    if (k >= 0) { objId = g_b[k].objId; ig = g_b[k].ig; watching = g_b[k].watchUntilUs != 0; lastGid = g_b[k].lastGid; }
    ReleaseSRWLockShared(&g_bLock);
    if (!objId) return;
    if (g_role == 1 && g_hitSync && !(g_playoutMs && g_chartSync)) {   // BrainHitSync host: send the kept attack call when this native guard enters 173 (with BrainPlayoutMs it goes at the decision, NodeW)
        bool in173 = false;
        __try {
            const uint32_t fsm = *(uint32_t*)(ig + 0x90);
            unsigned d = *(const uint16_t*)(fsm + 0x11E); if (d > 12) d = 12;
            const uint16_t* p = *(const uint16_t* const*)(fsm + 0x118);
            for (unsigned i = 0; i < d; ++i) if (p[i] == 173) in173 = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Cmd c; bool send = false;
        AcquireSRWLockExclusive(&g_bLock);
        const int kk = FindBy(&Body::human, h);
        if (kk >= 0) {
            if (in173 && !g_b[kk].hostIn173) {
                if (g_b[kk].pendValid) { c = g_b[kk].pendAtk; g_b[kk].pendValid = 0; send = true; }
                else InterlockedIncrement(&c_atkNoCall);   // entered 173 with no kept call (UNVERIFIED how)
            }
            g_b[kk].hostIn173 = in173 ? 1 : 0;
        }
        ReleaseSRWLockExclusive(&g_bLock);
        if (send) {
            bool clk = false; const uint32_t now = HostMs(&clk);
            if (clk && PeerReady()) {
                c.execHostMs = now + (uint32_t)g_delayMs; Publish(objId, c); InterlockedIncrement(&c_atkSent);
                Watch(h, c.execHostMs);   // the host's s173 line carries the sent command's execMs (tools/brain_check.py pairs on it)
                watching = true;
            }
        }
    }
    if (objId == kPuppetWire) {   // [Coop] BrainPuppet: every change of the player's (host) / puppet's (guest) whole path, timed
        char ps[96]; PathStr(ig, ps, sizeof(ps));
        uint32_t hsh = 2166136261u; for (const char* q = ps; *q; ++q) hsh = (hsh ^ (uint8_t)*q) * 16777619u;
        bool changed = false;
        AcquireSRWLockExclusive(&g_bLock);
        const int kk = FindBy(&Body::human, h);
        if (kk >= 0 && g_b[kk].pathHash != hsh) { g_b[kk].pathHash = hsh; changed = true; }
        ReleaseSRWLockExclusive(&g_bLock);
        if (changed && ps[0] && InterlockedIncrement(&c_ppathLines) <= 3000) {
            bool clk = false; const uint32_t now = HostMs(&clk);
            Log("[Brain] ppath role=%s hostMs=%u path=%s\n", g_role == 1 ? "host" : "guest", now, ps);
        }
    }
    {   // State-115 probe: every change of the dispatch id (path[3]) on a registered body, host and guest, with the whole path
        char ps[96]; const unsigned gid = PathStr(ig, ps, sizeof(ps));
        if (gid && gid != lastGid) {
            bool clk = false; const uint32_t now = HostMs(&clk);
            if (InterlockedIncrement(&c_gidLines) <= 600)
                Log("[Brain] gid role=%s id=%016llX %u->%u path=%s hostMs=%u stage1=%d\n", g_role == 1 ? "host" : "guest", (unsigned long long)objId,
                    (unsigned)lastGid, gid, ps, now, Phase1_HumanStage1(human));
            AcquireSRWLockExclusive(&g_bLock);
            const int kk = FindBy(&Body::human, h);
            if (kk >= 0) g_b[kk].lastGid = (uint16_t)gid;
            ReleaseSRWLockExclusive(&g_bLock);
        }
    }
    if (g_role == 0) {
        bool clk = false; const uint32_t now = HostMs(&clk);
        Cmd held[8]; int nh = 0;   // refused this frame (enter not accepted yet): retried next frame, up to 1 s late
        for (;;) {   // due commands for this body, oldest execution time first
            Cmd c; int best = -1;
            AcquireSRWLockExclusive(&g_qLock);
            for (int i = 0; i < kQ; ++i)
                if (g_q[i].used && g_q[i].objId == objId && (!clk || (int32_t)(now - g_q[i].c.execHostMs) >= 0)
                    && (best < 0 || (int32_t)(g_q[i].c.execHostMs - g_q[best].c.execHostMs) < 0)) best = i;
            if (best >= 0) { c = g_q[best].c; g_q[best].used = 0; }
            ReleaseSRWLockExclusive(&g_qLock);
            if (best < 0) break;
            if (!clk) InterlockedIncrement(&c_noClock);
            const int32_t late = clk ? (int32_t)(now - c.execHostMs) : 0;
            if (late > 1000) { InterlockedIncrement(&c_stale); continue; }
            if (!Execute(h, ig, objId, c, late)) { if (nh < 8) held[nh++] = c; else InterlockedIncrement(&c_stale); continue; }
            const LONG n = InterlockedIncrement(&g_lateN); g_late[(n - 1) & 63] = late;
        }
        if (nh) {
            AcquireSRWLockExclusive(&g_qLock);
            for (int j = 0, i = 0; j < nh && i < kQ; ++i) if (!g_q[i].used) { g_q[i].objId = objId; g_q[i].c = held[j++]; g_q[i].used = 1; }
            ReleaseSRWLockExclusive(&g_qLock);
        }
    }
    if (watching) {
        AcquireSRWLockExclusive(&g_bLock);
        const int kk = FindBy(&Body::human, h);
        if (kk >= 0) WatchPath(kk, ig);
        ReleaseSRWLockExclusive(&g_bLock);
    }
}

void Tick() {
    const uint64_t ms = GetTickCount64();
    // Live kill switch (audit I-103): [Coop] BrainOff (default 0) is re-read every 2 s; 1 turns a running brain off and the
    // host restores its vtable, 0 again turns it back on. Not [Coop] Brain itself: Havana run 3 showed tools/two_copy.ps1
    // restoring the ini after boot, so a live re-read of Brain switched the brain off mid-run (restored=11 foreign=0).
    {
        static uint64_t s_nextFlag = 0;
        static int s_killed = 0;
        if (g_on >= 0 && (g_on == 1 || s_killed) && ms >= s_nextFlag) {
            s_nextFlag = ms + 2000;
            const int off = GetPrivateProfileIntA("Coop", "BrainOff", 0, Paths::Ini()) != 0 ? 1 : 0;
            if (off != s_killed) {
                Log("[Brain] BrainOff=%d live\n", off);
                s_killed = off; g_on = off ? 0 : 1;
                if (g_role == 1) { if (off) RemoveCapture(); else InstallCapture(); }
            }
        }
    }
    if (!On() || g_role < 0) return;
    CensusTick(ms);   // [Coop] BrainCensus: refresh the body snapshot, 15 s summary
    if (g_puppet && g_chartSync) {   // [Coop] BrainPuppet: register the host player (host) / the puppet (guest) as kPuppetWire
        void* ent = g_role == 1 ? Phase1_LocalPlayerEntity() : Phase1_PuppetEntity();
        void* hum = ent ? Phase1_HumanOfEntity(ent) : nullptr;
        if (hum) NoteBody(kPuppetWire, hum, ent);
    }
    {   // [Coop] Brain115Watch: arm once a registered body is in dispatch 4, re-apply every 5 s, clear when done or after 90 s
        static int s_w = -1; static uint64_t s_wNext = 0, s_wStart = 0;
        if (s_w < 0) s_w = GetPrivateProfileIntA("Coop", "Brain115Watch", 0, Paths::Ini()) != 0 ? 1 : 0;
        if (s_w == 1 && ms >= s_wNext) {
            s_wNext = ms + 5000;
            if (g_wDone || (s_wStart && ms - s_wStart > 90000)) {
                WatchSetAll(0); s_w = 2;
                Log("[Brain] watch done hits115=%ld other=%ld\n", g_wHits115, g_wHitsOther);
            } else if (!g_wAddr) {
                uint32_t addr = 0; uint64_t obj = 0;
                AcquireSRWLockShared(&g_bLock);
                for (int i = 0; i < g_nb && !addr; ++i) {
                    char ps[96];
                    if (PathStr(g_b[i].ig, ps, sizeof(ps)) != 4) continue;
                    __try {
                        const uint32_t fsm = *(uint32_t*)(g_b[i].ig + 0x90);
                        addr = *(uint32_t*)(fsm + 0x118) + 6; obj = g_b[i].objId;
                    } __except (EXCEPTION_EXECUTE_HANDLER) { addr = 0; }
                }
                ReleaseSRWLockShared(&g_bLock);
                if (addr) {
                    g_wObj = obj; InterlockedExchange(&g_wAddr, (LONG)addr); s_wStart = ms;
                    if (!g_wVeh) g_wVeh = AddVectoredExceptionHandler(1, WatchVeh);
                    WatchSetAll(addr);
                    Log("[Brain] watch armed id=%016llX addr=%08X (path[3] of a body in dispatch 4)\n", (unsigned long long)obj, addr);
                }
            } else WatchSetAll((uint32_t)g_wAddr);   // threads created since the last pass
        }
    }
    bool clk = false; const uint32_t now = HostMs(&clk);
    static uint64_t s_nextTest = 0, s_nextSweep = 0, s_nextReport = 0;
    static uint32_t s_lcg = 0x1234567u;
    if (g_role == 1 && g_test && clk && PeerReady() && ms >= s_nextTest) {   // S0 lane oracle: a marker to one random bound guard per second
        s_nextTest = ms + 1000;
        uint64_t obj = 0; uint32_t human = 0;
        AcquireSRWLockShared(&g_bLock);
        if (g_nb) { s_lcg = s_lcg * 1664525u + 1013904223u; const int k = (int)((s_lcg >> 8) % (uint32_t)g_nb); obj = g_b[k].objId; human = g_b[k].human; }
        ReleaseSRWLockShared(&g_bLock);
        if (obj) {
            Cmd c = {}; c.slot = kMark; c.execHostMs = now + (uint32_t)g_testDelayMs;
            Publish(obj, c);
            AcquireSRWLockExclusive(&g_qLock);   // the host plays its own marker at the same host time
            for (int i = 0; i < kQ; ++i) if (!g_q[i].used) { g_q[i].objId = obj; g_q[i].c = c; g_q[i].used = 1; break; }
            ReleaseSRWLockExclusive(&g_qLock);
            Log("[Brain] issue slot=mark id=%016llX execMs=%u nowMs=%u\n", (unsigned long long)obj, c.execHostMs, now);
            (void)human;
        }
    }
    if (g_role == 1 && g_test && clk) {   // host markers execute here (no slot call, so the player frame is fine)
        for (int i = 0; i < kQ; ++i) {
            AcquireSRWLockExclusive(&g_qLock);
            const bool due = g_q[i].used && (int32_t)(now - g_q[i].c.execHostMs) >= 0;
            Cmd c = g_q[i].c; const uint64_t obj = g_q[i].objId;
            if (due) g_q[i].used = 0;
            ReleaseSRWLockExclusive(&g_qLock);
            if (due) Execute(0, 0, obj, c, (int32_t)(now - c.execHostMs));
        }
    }
    if (ms >= s_nextSweep) {   // drop commands for bodies that never ticked, and bodies not seen for 10 s
        s_nextSweep = ms + 1000;
        if (clk && g_role == 0) {
            AcquireSRWLockExclusive(&g_qLock);
            for (int i = 0; i < kQ; ++i)
                if (g_q[i].used && (int32_t)(now - g_q[i].c.execHostMs) > 1000) {
                    g_q[i].used = 0;
                    AcquireSRWLockShared(&g_bLock);
                    const bool known = FindObj(g_q[i].objId) >= 0;
                    ReleaseSRWLockShared(&g_bLock);
                    InterlockedIncrement(known ? &c_stale : &c_noBody);
                }
            ReleaseSRWLockExclusive(&g_qLock);
        }
        AcquireSRWLockExclusive(&g_bLock);
        for (int i = 0; i < g_nb;) { if (ms - g_b[i].seenMs > 10000) g_b[i] = g_b[--g_nb]; else ++i; }
        ReleaseSRWLockExclusive(&g_bLock);
    }
    if (ms >= s_nextReport) {
        s_nextReport = ms + 5000;
        if (g_hitSync)
            Log("[Brain] hitsync role=%s gateTx=%ld permits=%ld allowed=%ld substituted=%ld blocked=%ld hitApplied=%ld hitRefused=%ld hitHeld=%ld attackHeld=%ld atkCalls=%ld atkSent=%ld atkNoCall=%ld\n",
                g_role == 1 ? "host" : "guest", c_gate181Tx, c_gatePermits, c_gate181Allowed, c_gateSubst, c_gate181Blocked, c_hitApplied, c_hitRefused, c_hitHeld,
                c_attackHeld, c_atkBuffered, c_atkSent, c_atkNoCall);
        if (g_react)
            Log("[Brain] react role=%s cap=%ld nonFde=%ld applied=%ld refused=%ld held=%ld incomplete=%ld buildFail=%ld veto=%ld\n",
                g_role == 1 ? "host" : "guest", c_reactCap, c_reactNonFde, c_reactApplied, c_reactRefused, c_reactHeld, c_reactIncomplete,
                c_reactBuildFail, c_reactVeto);
        if (g_chartSync)
            Log("[Brain] chartsync role=%s tx=%ld rx=%ld applied=%ld own=%ld forced=%ld waitFrames=%ld expired=%ld dropped=%ld playoutMs=%d holdSet=%ld holdApplied=%ld holdExpired=%ld\n",
                g_role == 1 ? "host" : "guest", c_chartTx, c_chartRx, c_chartApplied, c_chartOwn, c_chartForced, c_chartWait, c_chartExpired, c_chartDropped,
                g_playoutMs, c_holdSet, c_holdApplied, c_holdExpired);
        if (g_probe115 || g_supp115)
            Log("[Brain] 115 role=%s eval4=%ld eval58=%ld copy4=%ld copy58=%ld native4=%ld native58=%ld unmatched=%ld suppressed=%ld evalCalls=%ld\n",
                g_role == 1 ? "host" : "guest", c115[0], c115[1], c115Copy[0], c115Copy[1], c115Native[0], c115Native[1], c115Unmatched, c115Supp, c115SelCalls);
        LONG s[64]; const LONG n = g_lateN < 64 ? g_lateN : 64;
        for (int i = 0; i < n; ++i) s[i] = g_late[i];
        for (int i = 1; i < n; ++i) for (int j = i; j > 0 && s[j - 1] > s[j]; --j) { const LONG x = s[j]; s[j] = s[j - 1]; s[j - 1] = x; }
        char caps[160]; int o = 0; caps[0] = 0;
        for (int i = 0; i < kNSlots && o < 150; ++i) if (c_cap[i]) o += _snprintf_s(caps + o, sizeof(caps) - o, _TRUNCATE, kSlots[i].vt ? "b%u:%ld " : "%u:%ld ", kSlots[i].slot, c_cap[i]);
        Log("[Brain] stats role=%s bodies=%d clk=%d cap={%s} unbound=%ld tx=%ld txFail=%ld rx=%ld staleBase=%ld exec=%ld execFail=%ld stale=%ld "
            "noTarget=%ld noBody=%ld noClock=%ld queueFull=%ld notReady=%ld enterRefused=%ld alreadyFight=%ld dupDropped=%ld s173=%ld late(ms) n=%ld p50=%ld p95=%ld max=%ld\n",
            g_role == 1 ? "host" : "guest", g_nb, clk ? 1 : 0, caps, c_capUnbound, c_tx, c_txFail, c_rx, c_rxStaleBase, c_exec, c_execFail, c_stale,
            c_noTarget, c_noBody, c_noClock, c_queueFull, c_notReady, c_enterRefused, c_alreadyFight, c_dupDropped, c_s173, n, n ? s[n / 2] : 0, n ? s[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] : 0, n ? s[n - 1] : 0);
    }
}

}  // namespace Brain

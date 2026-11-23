// [Coop] Brain - see guard_brain.h.
#include "guard_brain.h"
#include "peer_clock.h"
#include "net.h"
#include "netstate.h"
#include "paths.h"
#include <windows.h>
#include <tlhelp32.h>
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
    // State-115 probe (count + log only, IDA cc/g4-g15): the only actions the Ground chart handles in dispatch 115.
    { 375, 0x01D2AC10u, 0, 0, 0 },                        // -> sub_1D03C20: in 115 pushes token 301
    { 0,   0x01D30600u, 0, 0, 0, 0x026E7C9Cu, -0x24 },    // -> sub_1D04D30: in 115 pushes token 305
    { 2,   0x01D30650u, 0, 0, 0, 0x026E7C9Cu, -0x24 },    // -> sub_1D04E30: in 115 pushes token 306
};
uint32_t VtOf(int i) { return kSlots[i].vt ? kSlots[i].vt : 0x026E7DC0u; }
const int kNSlots = sizeof(kSlots) / sizeof(kSlots[0]);

int  g_on = -1, g_delayMs = 0, g_test = 0, g_testDelayMs = 150;
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
    uint64_t watchUntilUs; uint32_t watchExec; uint8_t in173; uint16_t lastPath[12]; uint8_t lastDepth; int pathLines; uint16_t lastGid;
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
              c_stale = 0, c_noTarget = 0, c_noBody = 0, c_noClock = 0, c_s173 = 0, c_queueFull = 0, c_notReady = 0, c_enterRefused = 0, c_dupDropped = 0, c_alreadyFight = 0, c_capLines = 0, c_execLines = 0;
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

void __cdecl Capture(uint32_t id, uint32_t ecx, const uint32_t* frame) {
    if (id >= (uint32_t)kNSlots) return;
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
        if (s.argKind) c.tgt = (uint8_t)ClassOf(tent, &c.tgtObj);
        if (InterlockedIncrement(&c_capLines) <= 80)
            Log("[Brain] cap slot=%u id=%016llX tgt=%u tobj=%016llX a=%08X,%08X,%08X,%08X ret=%08X hostMs=%u clk=%d\n", s.slot,
                (unsigned long long)objId, c.tgt, (unsigned long long)c.tgtObj, c.a[0], c.a[1], c.a[2], c.a[3], frame[0], now, clk ? 1 : 0);
        if (!s.replay || !clk) return;
        if (!PeerReady()) { InterlockedIncrement(&c_notReady); return; }
        Publish(objId, c);
        if (s.slot == 155 || s.slot == 160 || s.slot == 546) Watch(human, c.execHostMs);
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
    bool tok = true;
    const uint32_t t = ResolveTarget(c, &tok);
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

// ---- Ground dispatch 115 (docs/evidence/brain-s0-2026-11-22.md, runs 3-6) -------------------------------------------
// Both games enter 115 on their own from dispatch 4 and leave it after ~1.1 s; a copy in 115 refuses enter fight. Two static
// entry routes (IDA cc/g10-g16), each reached through one E8 call (bytes checked against the exe, tools/pe.py):
//   A: sub_1D9BAE0 0x1D9BAF4 -> selector sub_1D994F0 -> stub sub_1D89480 0x1D894AA -> sub_1D87230 (sets 115)
//   B: sub_1D98770 0x1D98B23 -> selector sub_1D91D60 -> stub sub_1D90D10 0x1D90D36 -> sub_1D8F3D0 (sets 115)
// Both stubs pass `ecx = [ctx+0xC]`; the selectors read the same object as **(this+0xC). Called X here; X+264 is what the
// chart passes to sub_5FC110. Whether X is the HumanGround (ig - 0x10) is what the probe checks (UNVERIFIED).
// [Coop] Brain115Probe (default 0): logging stubs on the two transition calls. [Coop] Brain115Suppress (default 0): on the
// guest only, selector B returns 0 ("no transition") for a registered copy, so the copy does not enter 115 by route B.
int g_probe115 = 0, g_supp115 = 0;
volatile LONG c115[2] = {}, c115Copy[2] = {}, c115Native[2] = {}, c115Unmatched = 0, c115Lines = 0, c115Supp = 0, c115SelCalls = 0;

int BodyOfX(uint32_t x, uint64_t* objId) {   // index of the registered body whose HumanGround is x, else -1 (lock held by caller)
    const int k = FindBy(&Body::ig, x + 0x10);
    if (k >= 0 && objId) *objId = g_b[k].objId;
    return k;
}

void __cdecl Entry115(uint32_t route, uint32_t ecx) {
    __try {
        const uint32_t x = Rd((void*)ecx, 4) ? *(uint32_t*)ecx : 0;
        uint64_t objId = 0;
        AcquireSRWLockShared(&g_bLock);
        const int k = BodyOfX(x, &objId);
        ReleaseSRWLockShared(&g_bLock);
        InterlockedIncrement(&c115[route & 1]);
        if (k >= 0) InterlockedIncrement(g_role == 0 ? &c115Copy[route & 1] : &c115Native[route & 1]); else InterlockedIncrement(&c115Unmatched);
        bool clk = false; const uint32_t now = HostMs(&clk);
        if (InterlockedIncrement(&c115Lines) <= 300)
            Log("[Brain] enter115 route=%c role=%s x=%08X id=%016llX matched=%d hostMs=%u\n", route ? 'B' : 'A', g_role == 1 ? "host" : g_role == 0 ? "guest" : "?",
                x, (unsigned long long)objId, k >= 0 ? 1 : 0, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
uint8_t* g_stubs115 = nullptr;
void Emit115(uint8_t* p, uint32_t route, uint32_t orig) {
    int k = 0;
    auto b = [&](uint8_t v) { p[k++] = v; };
    auto d = [&](uint32_t v) { memcpy(p + k, &v, 4); k += 4; };
    b(0x60); b(0x9C);                               // pushad; pushfd
    b(0x51);                                        // push ecx (the transition's this)
    b(0x68); d(route);                              // push route
    b(0xB8); d((uint32_t)(uintptr_t)&Entry115);     // mov eax, Entry115
    b(0xFF); b(0xD0);                               // call eax
    b(0x83); b(0xC4); b(0x08);                      // add esp, 8
    b(0x9D); b(0x61);                               // popfd; popad
    b(0x68); d(orig);                               // push orig
    b(0xC3);                                        // ret: as if the original E8 had called orig (return address untouched)
}

typedef int(__thiscall* SelBFn)(uint32_t self, int a2);
int __fastcall SelB(uint32_t self, uint32_t /*edx*/, int a2) {   // replaces the E8 at 0x1D98B23; thiscall-compatible (ret 4)
    InterlockedIncrement(&c115SelCalls);
    if (g_supp115 && g_role == 0 && g_on == 1) {
        bool copy = false;
        __try {
            const uint32_t p = Rd((void*)(self + 0xC), 4) ? *(uint32_t*)(self + 0xC) : 0;
            const uint32_t x = Rd((void*)p, 4) ? *(uint32_t*)p : 0;
            AcquireSRWLockShared(&g_bLock);
            copy = BodyOfX(x, nullptr) >= 0;
            ReleaseSRWLockShared(&g_bLock);
        } __except (EXCEPTION_EXECUTE_HANDLER) { copy = false; }
        if (copy) { InterlockedIncrement(&c115Supp); return 0; }   // sub_1D91D60's own "no transition" value
    }
    return ((SelBFn)0x01D91D60)(self, a2);
}

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
void Install115() {
    g_probe115 = GetPrivateProfileIntA("Coop", "Brain115Probe", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_supp115 = GetPrivateProfileIntA("Coop", "Brain115Suppress", 0, Paths::Ini()) != 0 ? 1 : 0;
    if (g_probe115) {
        g_stubs115 = (uint8_t*)VirtualAlloc(nullptr, 2 * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_stubs115) {
            Emit115(g_stubs115, 0, 0x01D87230u); Emit115(g_stubs115 + 32, 1, 0x01D8F3D0u);
            FlushInstructionCache(GetCurrentProcess(), g_stubs115, 64);
            const bool a = PatchCall(0x01D894AAu, 0x01D87230u, (uint32_t)(uintptr_t)g_stubs115);
            const bool b = PatchCall(0x01D90D36u, 0x01D8F3D0u, (uint32_t)(uintptr_t)(g_stubs115 + 32));
            Log("[Brain] 115 probe: route A %s, route B %s\n", a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked");
        }
    }
    if (g_supp115) {
        const bool s = PatchCall(0x01D98B23u, 0x01D91D60u, (uint32_t)(uintptr_t)&SelB);
        Log("[Brain] 115 suppress (guest copies, route B): %s\n", s ? "installed" : "NOT installed");
    }
}

}  // namespace

namespace Brain {

void Init() {
    if (g_on >= 0) return;
    g_on = GetPrivateProfileIntA("Coop", "Brain", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_delayMs = GetPrivateProfileIntA("Coop", "BrainDelayMs", 0, Paths::Ini());
    g_test = GetPrivateProfileIntA("Coop", "BrainTest", 0, Paths::Ini());
    g_testDelayMs = GetPrivateProfileIntA("Coop", "BrainTestDelayMs", 150, Paths::Ini());
    if (g_on) Log("[Brain] Brain=1 delayMs=%d test=%d testDelayMs=%d (needs IntentLane=1, PeerClock=1)\n", g_delayMs, g_test, g_testDelayMs);
    if (g_on) Install115();
}

bool On() { return g_on == 1; }

void SetRole(bool isHost) {
    if (!On() || InterlockedCompareExchange(&g_role, isHost ? 1 : 0, -1) != -1) return;
    Log("[Brain] role=%s\n", isHost ? "host" : "guest");
    if (isHost) InstallCapture();
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
        if (g_probe115 || g_supp115)
            Log("[Brain] 115 role=%s A=%ld B=%ld copyA=%ld copyB=%ld nativeA=%ld nativeB=%ld unmatched=%ld suppressed=%ld selBCalls=%ld\n",
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

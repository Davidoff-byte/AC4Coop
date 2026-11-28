// [Coop] SpawnLib (default 0; hooks when non-zero at load, then re-read once a second): spawn ANY entity the engine itself
// spawns (AI ships, NPCs incl. guards), on demand, through the engine's own SpawningManager - never by cloning (clones share
// sub-objects with their source and crash the world unload: ops/evidence/fast-travel-crash-2026-11-26.md,
// two-instance-crash-2026-11-27.md).
//
// Facts used (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35; receipts in docs/evidence/spawn-manager-2026-11-25.md s1-s7 and
// ops/evidence/ship-spawn-replay-design-2026-11-27.md):
//   submit   sub_569560(spec, owner, ctx, lo, hi), cdecl: factory sub_F72ED0 -> sub_79C6C0 -> sub_79B8D0. Our own request is
//            the factory's return value, seen by spawn_census.cpp DetFac on the calling thread (tagged here).
//   request  +0x0C owner, +0x10 spec (sub_5B06B0 writes it), +0x20 state.
//   spec     SpawningSpecification vt 0x01E4A278: u16 +0x32 params count, +0x2C params array (first = the entity class:
//            AI SpawnShipParams 0x0270A538, SpawnNPCParams 0x026FD610, SpawnCharacterParams 0x026FD4F0), +0x3C current
//            request (restored after our submit, as ship_spawn_replay.cpp does).
//   specs    persistent objects owned by their spec set: sub_1B44EA0 copies pointers out of the set's own array (this+4,
//            count this+10) into a temporary list (2026-11-28 decompile) -> a spec stays valid while its owner lives;
//            validity is checked lock-free (VirtualQuery + vtables) right before every submit.
//   built    sub_5BC470(client, req, ent) (spawn_census.cpp DetSet, manager job thread) -> our request's entity.
//   NPC key  spawn_census.cpp SpawnKeyNoteReq/SpawnKeyNoteEnt: static-anchored specs have a key equal on two machines
//            (68/69, s5); a replay of a keyed spec produces an entity with that key (RK/EK tables).
// Uses:
//   1. [Coop] SpawnCmd + SpawnCmdSeq (any role): a change of SpawnCmdSeq spawns one "ship[:class]" / "npc[:keyhex]" and
//      places it in front of the player (ship 80 m, NPC 4 m) 1.5 s after it is built. The co-op lanes then carry it.
//   2. Guest follow ([Coop] SpawnLibFollow, default 1 when SpawnLib is on): a host guard whose spawn key has no free local
//      body (phase1.cpp WideBindPrepass key round) -> replay our own spec for that key; the key round binds the new body.
//   3. Ship spec supply for ship_spawn_replay.cpp (SpawnLibPickShip): newest VALID spec of the class, else of any class.
// Threads: notes = game/job threads, plain reads + a spinlock (no engine calls). Submit/place = player-update thread only,
// outside every __try (lock-taking engine calls), after VirtualQuery validation.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <intrin.h>
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

void Phase1_LogLine(const char* s);
bool Phase1_InSave();
void SpawnShareReplayTag(int v);   // spawn_census.cpp: SpawnShareDecide passes our own submit
int SpawnCrewOfShip(uintptr_t ship, uintptr_t* out, int* kinds, float* loc3, int cap);   // spawn_census.cpp
int SpawnCrewAnchors(uintptr_t* out, int cap);                                              // spawn_census.cpp

namespace {
constexpr uint32_t kSpecVt = 0x01E4A278u, kShipParamsVt = 0x0270A538u, kNpcParamsVt = 0x026FD610u, kCharParamsVt = 0x026FD4F0u;
constexpr uint32_t kShipVt = 0x01E4A128u, kNpcVt = 0x01E4CE90u;
enum { K_NONE = 0, K_SHIP = 1, K_NPC = 2 };
enum { P_CMD = 1, P_FOLLOW = 2 };
constexpr int kLib = 512, kQ = 32, kTried = 256;
constexpr ULONGLONG kGapMs = 1500, kBuildTimeoutMs = 10000, kPlaceDelayMs = 1500;

struct LibE { uintptr_t spec, specVt, owner, ownerVt, ctx; uint64_t key; int kind; uint32_t cls; ULONGLONG t; int bad; };
LibE g_lib[kLib]; int g_libN = 0; volatile LONG g_libLock = 0;
struct Spin { Spin() { while (InterlockedExchange(&g_libLock, 1)) YieldProcessor(); } ~Spin() { InterlockedExchange(&g_libLock, 0); } };

bool g_installed = false;
volatile LONG g_on = 0, g_follow = 1, g_busyPol = 1;   // [Coop] SpawnLibBusy (see SpecUsable)
volatile LONG g_lateRestore = 0;   // [Coop] SpawnLibLateRestore (default 0): give the borrowed spec slot back at completion, not at DetSet
volatile ULONGLONG g_lastOurBuildMs = 0;   // GetTickCount64 of our last build (DetSet), for [ShipDisp] in phase1.cpp
__declspec(thread) int t_tag = 0;
volatile uintptr_t g_ourReq = 0;

struct Want { int kind, purpose; uint64_t key, host; uint32_t cls; int hasPlace; float place[3]; ULONGLONG t; };
Want g_q[kQ]; int g_qN = 0;
struct Fly { volatile uintptr_t req; volatile uintptr_t ent; volatile ULONGLONG tBuilt; Want w; uintptr_t spec; ULONGLONG t; int active; };
Fly g_fly = {};
struct Tried { uint64_t id; ULONGLONG t0; int n; } g_tried[kTried]; int g_triedN = 0;
ULONGLONG g_lastFire = 0;
LONG c_noted = 0, c_learned = 0, c_want = 0, c_fired = 0, c_submit0 = 0, c_built = 0, c_placed = 0, c_noSpec = 0, c_failed = 0,
     c_badSpec = 0, c_follow = 0, c_cmd = 0, c_busy = 0, c_unarmed = 0;

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
bool ShipClassOk(uint32_t cls) { return cls != 0 && cls != 7 && !(cls >= 8 && cls <= 16) && cls < 64; }   // as ship_spawn_replay.cpp
int KindOfParamsVt(uint32_t v) { return v == kShipParamsVt ? K_SHIP : (v == kNpcParamsVt || v == kCharParamsVt) ? K_NPC : K_NONE; }

// Lock-free kind of a spec (plain reads; the caller guards with __try where it runs).
int SpecKindRaw(uintptr_t spec)
{
    if (*(const uint32_t*)spec != kSpecVt || *(const uint16_t*)(spec + 0x32) == 0) return K_NONE;
    const uintptr_t arr = *(const uintptr_t*)(spec + 0x2C);
    const uintptr_t p0 = arr ? *(const uintptr_t*)arr : 0;
    return p0 ? KindOfParamsVt(*(const uint32_t*)p0) : K_NONE;
}
// Full validation right before a submit (player thread, no __try): VirtualQuery + vtables as noted + params class.
bool SpecValid(const LibE& e)
{
    if (!Readable(e.spec, 0x40) || *(const uint32_t*)e.spec != e.specVt || *(const uint16_t*)(e.spec + 0x32) == 0) return false;
    if (!Readable(e.owner, 4) || *(const uint32_t*)e.owner != e.ownerVt) return false;
    const uintptr_t arr = *(const uintptr_t*)(e.spec + 0x2C);
    if (!Readable(arr, 4)) return false;
    const uintptr_t p0 = *(const uintptr_t*)arr;
    return Readable(p0, 0x50) && KindOfParamsVt(*(const uint32_t*)p0) == e.kind;
}
// Busy-spec policy. +0x3C is the spec's single current-request slot (submit sets it, the request dtor 0x79C640 clears it when
// it still holds itself). Measured 2026-11-28 host 28620: idle specs (+0x3C == 0) were 3/27 ship specs and none built (4/4
// timed out at 10 s); busy ones built in 1.5 s earlier. Both crashes (guest 17552, host 17860) came from RESTORING the old
// request after our submit (owner ship record ended up {old request (freed), our ship} -> AV 0x1C27B13/0x1C27B15), so there
// is no restore any more. [Coop] SpawnLibBusy: 0 = idle specs only; 1 (default) = also busy specs whose current request is
// not in flight (state not 1 submitted / 2 building; UNVERIFIED that this avoids the owner-record clash); 2 = any.
// Request +0x20 state as read here: 1 set by sub_79C6C0, 4 seen on our built requests, 6 on the freed old one (dump 17860, FC46DF00).
LONG c_st[8] = {};   // busy specs met by Pick, by current-request state: [0]=1 [1]=2 [2]=4 [3]=6 [4]=9 [5]=other [6]=bad ptr
int SpecState(uintptr_t spec)   // -1 idle, -2 slot not a live request, else +0x20
{
    const uintptr_t cur = *(const uintptr_t*)(spec + 0x3C);
    if (!cur) return -1;
    if (!Readable(cur, 0x24) || *(const uint32_t*)cur != 0x01E6D77Cu || *(const uintptr_t*)(cur + 0x10) != spec) return -2;
    return (int)*(const uint32_t*)(cur + 0x20);
}
// Armed: spec+0x1C must hold an object with a readable vtable at build time. Every params class builds through
// sub_46D6F0 (ship/playership vt[32]; npc vt[32] sub_18A03A0 and char vt[32] sub_189FDE0 call it), which does
// sub_6B3CE0(sub_E05060(params) = params+4 = spec) = spec+0x1C ->vt[2](). Runs 2026-11-28 host 25716 + guest 24228:
// the 2nd submit on one spec, after our 1st request had ended (request dtor 0x79C65B -> sub_4EDF60(0) -> spec vt[16]),
// found spec+0x1C == 0 -> AV 0x6B3CE3 on the manager job thread on both copies.
bool SpecArmed(uintptr_t spec)
{
    const uintptr_t a = *(const uintptr_t*)(spec + 0x1C);
    if (!a || !Readable(a, 4)) return false;
    const uintptr_t v = *(const uintptr_t*)a;
    // vtable inside .rdata (0x01E3A000..0x0274274A) and vt[2] inside .text (0x00401000..0x01E399F2), AC4BFSP.exe section table
    return v >= 0x01E3A000u && v < 0x0274274Au && Readable(v, 0x0C) && *(const uintptr_t*)(v + 8) >= 0x00401000u && *(const uintptr_t*)(v + 8) < 0x01E399F2u;
}
bool SpecUsableSt(int st)
{
    if (st == -1) return true;
    InterlockedIncrement(&c_st[st == 1 ? 0 : st == 2 ? 1 : st == 4 ? 2 : st == 6 ? 3 : st == 9 ? 4 : st == -2 ? 6 : 5]);
    const LONG pol = g_busyPol;
    return pol >= 2 ? st != -2 : pol == 1 ? (st != 1 && st != 2 && st != -2) : false;
}
// ---- private owner + slot borrow (2026-11-28) ------------------------------------------------------------------------
// The engine's naval owner adopts each built ship 1:1 with its spec (owner vt[10] sub_196E910 via sub_5C5FC0). Extra
// ships of ours on one spec broke that: run T3 (asi 129C6403, 7deff59) host 16180 + guest 6104 both died at 0x1C27B25 -
// owner record {our request FC0B3840, our ship}, its spec FC64CB40 already freed (params count 0 -> sub_4F33C0 = 0).
// So our submits carry a PRIVATE owner, never the engine's. Owner interface class 0x02708E80 (at +8 of its object; +0
// 0x02708F04, +4 0x02708EE0; dumps 17860/16180). Its slots (IDA): [0][3][9] nullsub_1 0x54DEB0 retn 8; [1][2][12][13][14]
// [22][23] nullsub_25 0x1243A70 retn 4; [4] sub_196E610, [5] sub_19709F0, [6] sub_1970A70, [7] sub_1970AB0, [8]
// sub_1970AF0, [11] sub_1970B30 = entity removed/lost notices (thiscall, 2 args, retn 8, use this-8); [10] sub_196E910
// adoption (retn 8); [15] sub_1944E90 stdcall(a1) retn 4 (uses a1 only); [16] [19..21] return 0; [17] sub_19644B0 =
// deleting-dtor thunk (this-8, char); [18] sub_4E0490 retn 4 (static null-id handle 0x27F3F90, no this). The built
// entity keeps the owner pointer (sub_5C5FC0: sub_6F5FD0(ent) = ent+0xE8, sub_734600: +0x60 = owner), so the private
// owner lives forever and has no fields: a zeroed object whose three vtable slots all point at our copy of 0x02708E80
// with [4]-[8] [10] [11] [17] replaced by counting no-ops; every slot kept from the engine ignores this.
// Slot borrow: the submit makes our request the spec's current one (+0x3C, single slot). At build (DetSet, manager job
// thread, before the owner callbacks and before our request ends) the previous current request goes back into the slot
// if it is still a live built request of this spec (vt 0x01E6D77C, +0x10 == spec, +0x20 == 4), so our request's end does
// not un-arm the spec (dtor 0x79C65B acts only when the slot holds itself) and the engine sees its own state unchanged.
constexpr uint32_t kOwnerNavalVt = 0x02708E80u;
volatile LONG c_fo[24] = {}; LONG c_foUsed = 0, c_realOwner = 0, c_bwRestored = 0, c_bwKept = 0;
volatile uintptr_t g_fo4Caller = 0;   // first return address seen in FoS4 (which engine code calls owner vt[4])
#define FO_STUB2(i) uintptr_t __fastcall FoS##i(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { InterlockedIncrement(&c_fo[i]); return 0; }
// [4] (sub_5C58E0 0x5C59BE) and [10] (sub_5C5FC0 0x5C60DA) are the adoption calls at the end of completion, both
// push (request, entity). With SpawnLibLateRestore the borrowed slot goes back here: completion first runs the
// spec's placement for "the spec's current request's entity" (spec vt[21] sub_4F70A0 = spec+0x3C -> req+0x34 handle;
// position object vt[22] sub_19DD110 -> sub_56AD00), and a slot already given back at DetSet made that the OLD
// request's live ship: run 2026-11-28 07:40 (asi D8C38B53), the displaced hull 3E6F60F0 was exactly the old request's
// handle object, moved ~775 m to the spawn point 31 ms after each of our builds.
void BorrowRestore(uintptr_t req);
volatile LONG c_lateRestore = 0;
uintptr_t __fastcall FoS4(uintptr_t, uintptr_t, uintptr_t req, uintptr_t) { InterlockedIncrement(&c_fo[4]); if (!g_fo4Caller) g_fo4Caller = (uintptr_t)_ReturnAddress(); if (g_lateRestore) { BorrowRestore(req); InterlockedIncrement(&c_lateRestore); } return 0; }
uintptr_t __fastcall FoS10(uintptr_t, uintptr_t, uintptr_t req, uintptr_t) { InterlockedIncrement(&c_fo[10]); if (g_lateRestore) { BorrowRestore(req); InterlockedIncrement(&c_lateRestore); } return 0; }
FO_STUB2(5) FO_STUB2(6) FO_STUB2(7) FO_STUB2(8) FO_STUB2(11)
#undef FO_STUB2
uintptr_t __fastcall FoS17(uintptr_t self, uintptr_t, uint32_t) { InterlockedIncrement(&c_fo[17]); return self; }   // retn 4
// [19]: "release the request to the client after completion?" The naval client (vtable 0x0270B310) overrides the two
// completion methods - slot 5 sub_1982900 (after sub_5C58E0) and slot 37 sub_1982940 (after sub_5C5FC0) - and calls
// sub_5BCBE0(req) (+0x24 = 3, +0x2C = 4, queued on the client's type-3 list +0x4C) when req+0x30 != 0 && owner vt[19]().
// The naval owner's own [19] is sub_169EB60 (return 0): it keeps its requests (type 1) and disposes them itself. We do
// not, so our requests go back to the client. Run 2026-11-28 guest 29212 (crash H) died with [19] = 0 in the world unload
// (sub_718650 -> listeners -> sub_48F5D0 -> sub_48BAE0) on a freed replay request still listed; our requests then also
// sat in the real owner's container (fixed later in SubmitPrivate), so which of the two freed it is not separated.
// No arguments, plain retn (like sub_169EB60).
uint8_t __fastcall FoS19(uintptr_t, uintptr_t) { InterlockedIncrement(&c_fo[19]); return 1; }
uintptr_t g_foVt[24] = {};
alignas(16) uint8_t g_foObj[0x400] = {};   // the private owner object (zeroed); interface pointer = g_foObj + 8
uintptr_t PrivateOwnerFor(uintptr_t real)   // player thread; real validated by the caller
{
    if (*(const uint32_t*)real != kOwnerNavalVt) { InterlockedIncrement(&c_realOwner); return real; }   // other owner classes: unread
    if (!g_foVt[0]) {
        if (!Readable(kOwnerNavalVt, sizeof(g_foVt))) return 0;
        memcpy(g_foVt, (const void*)kOwnerNavalVt, sizeof(g_foVt));
        g_foVt[4] = (uintptr_t)&FoS4; g_foVt[5] = (uintptr_t)&FoS5; g_foVt[6] = (uintptr_t)&FoS6; g_foVt[7] = (uintptr_t)&FoS7;
        g_foVt[8] = (uintptr_t)&FoS8; g_foVt[10] = (uintptr_t)&FoS10; g_foVt[11] = (uintptr_t)&FoS11; g_foVt[17] = (uintptr_t)&FoS17;
        // [19] = 1: our requests go back to the client (its own disposal path; T17 + run 2026-11-28 04:32 survived fast
        // travels with it). Released requests sit in the client's type-3 list +0x4C, which sub_198F080 (client vt[23]
        // sub_1991F70) walks every tick: req vt[2] -> spec, sub_4EFD00(spec) = spec+0x24 -> [+0x14] (0x198F274). A
        // borrowed spec freed by its engine owner while our released request still pointed at it -> AV 0x198F274 (guest
        // 24636, at sea, before any travel). So built requests stay in g_inf and SpawnLibSpecDying re-points them too.
        g_foVt[19] = (uintptr_t)&FoS19;
        for (int i = 0; i < 3; ++i) *(uintptr_t*)(g_foObj + 4 * i) = (uintptr_t)g_foVt;
    }
    ++c_foUsed;
    return (uintptr_t)(g_foObj + 8);
}
bool IsPrivateOwner(uintptr_t o) { return o == (uintptr_t)(g_foObj + 8); }
uintptr_t g_pendSpec = 0, g_pendOld = 0;   // set around our submit (player thread), consumed in SpawnLibNoteFac (same thread)
struct Borrow { volatile uintptr_t req; uintptr_t spec, old; ULONGLONG t; };
Borrow g_bw[32] = {}; volatile LONG g_bwNext = 0;
// Our submitted, not yet built requests (any owner). A spec can be freed while one of them is queued (run T5 guest 29392:
// spec FC63CA20 freed with our request FC124C40 still queued -> sub_199D1F0 read its position params, AV 0x69D0A5).
// The spec dtor sub_501DF0 (detoured in spawn_census.cpp) never touches the request in +0x3C, so we redirect.
struct Inf { volatile uintptr_t req; volatile uintptr_t spec; int kind; uint32_t cls; ULONGLONG t; uintptr_t realOwner, privOwner; int built; };
constexpr int kInf = 1024;   // no expiry: run T7 host 28300 - our request FC21EF40 stayed queued > 12 s, its entry had expired, the
                            // spec was freed later and the manager built it -> AV 0x69D0A5. Entries leave at build or when dead.
Inf g_inf[kInf] = {}; volatile LONG g_infNext = 0;
uintptr_t g_ourShips[64] = {}; int g_ourShipsN = 0;   // our built ships (player thread), crew diagnostics
bool IsOurShip(uintptr_t s) { for (int i = 0; i < g_ourShipsN; ++i) if (g_ourShips[i] == s) return true; return false; }
uintptr_t g_pendAny = 0; int g_pendKind = 0; uint32_t g_pendCls = 0; uintptr_t g_pendReal = 0, g_pendPriv = 0;   // set around our submit (player thread)
volatile LONG c_dying = 0, c_redir = 0, c_redirFail = 0, c_ownerSwap = 0, c_ownerSwapMiss = 0; LONG c_bwEarly = 0, c_infDead = 0;
constexpr ULONGLONG kBorrowEarlyMs = 3000;   // unbuilt after 3 s: give the slot back now (the private owner cannot pair it)
int InfCount() { int n = 0; for (int i = 0; i < kInf; ++i) n += g_inf[i].req != 0; return n; }
int InfBuilt() { int n = 0; for (int i = 0; i < kInf; ++i) n += g_inf[i].req != 0 && g_inf[i].built; return n; }
volatile LONG g_infLock = 0; LONG c_reqDtor = 0, c_redirOwned = 0;
struct InfGuard { InfGuard() { while (InterlockedCompareExchange(&g_infLock, 1, 0) != 0) YieldProcessor(); } ~InfGuard() { InterlockedExchange(&g_infLock, 0); } };
int InfSlot() {   // a free entry if there is one, else the next one round-robin
    for (int k = 0; k < kInf; ++k) { const int i = (int)((InterlockedIncrement(&g_infNext) - 1) & (kInf - 1)); if (!g_inf[i].req) return i; }
    return (int)((InterlockedIncrement(&g_infNext) - 1) & (kInf - 1));
}
// DetSet, our builds (first 24): the spec's spawn objects and the borrowed slot's previous request + its entity handle.
// Run 07:40: spec+0x34 is 0 on naval specs (the job's 0x5C57E3 test skips it); +0x28 = position object vt 0x0270FC58.
void Log(const char* fmt, ...);   // below
// Old-ship watch: the borrowed slot's previous request's entity and where it was at our build; SpawnLibFire (player
// thread) reports it if it jumps > 30 m within 1 s (both copies, any build of ours).
struct OldWatch { volatile uintptr_t ent; float p[3]; ULONGLONG t; };
OldWatch g_ow[16] = {}; volatile LONG g_owNext = 0; LONG c_owMoved = 0, c_owStill = 0;
void BuildProbe(uintptr_t req, uintptr_t sp, uintptr_t cb)   // DetSet, our builds: classes of the spec's spawn objects (first 24)
{
    {   // every build: watch the old request's live ship
        uintptr_t old = 0; for (int j = 0; j < 32; ++j) if (g_bw[j].req == req) { old = g_bw[j].old; break; }
        const uintptr_t oh = (old && Readable(old, 0x38)) ? *(const uintptr_t*)(old + 0x34) : 0;
        const uintptr_t oe = (oh && Readable(oh, 0x0C) && (*(const int32_t*)(oh + 8) < 0)) ? *(const uintptr_t*)oh : 0;
        if (oe && Readable(oe, 0x50) && *(const uint32_t*)oe == kShipVt) {
            OldWatch& w = g_ow[(InterlockedIncrement(&g_owNext) - 1) & 15];
            w.ent = 0; const float* m = (const float*)(oe + 0x10); w.p[0] = m[12]; w.p[1] = m[13]; w.p[2] = m[14]; w.t = GetTickCount64(); MemoryBarrier(); w.ent = oe;
        }
    }
    static volatile LONG s_n = 0;
    if (InterlockedIncrement(&s_n) > 24) return;
    auto vt = [](uintptr_t o, int slot) -> uintptr_t { if (!o || !Readable(o, 4)) return 0; const uintptr_t v = *(const uintptr_t*)o; return (slot >= 0 && Readable(v + 4 * slot, 4)) ? *(const uintptr_t*)(v + 4 * slot) : v; };
    const uintptr_t pp = *(const uintptr_t*)(sp + 0x28);
    const uintptr_t h = Readable(req, 0x38) ? *(const uintptr_t*)(req + 0x34) : 0;
    const uintptr_t hObj = (h && Readable(h, 0x0C)) ? *(const uintptr_t*)h : 0, hFl = (h && Readable(h, 0x0C)) ? *(const uintptr_t*)(h + 8) : 0;
    uintptr_t old = 0; for (int j = 0; j < 32; ++j) if (g_bw[j].req == req) { old = g_bw[j].old; break; }
    const uintptr_t oh = (old && Readable(old, 0x38)) ? *(const uintptr_t*)(old + 0x34) : 0;
    const uintptr_t ohObj = (oh && Readable(oh, 0x0C)) ? *(const uintptr_t*)oh : 0, ohFl = (oh && Readable(oh, 0x0C)) ? *(const uintptr_t*)(oh + 8) : 0;
    Log("[SpawnLib] build probe req=%08X spec=%08X +28=%08X vt=%08X vt22=%08X | +34=%08X vt=%08X vt20=%08X | reqHandle=%08X obj=%08X fl=%08X | old=%08X handle=%08X obj=%08X fl=%08X\n",
        (unsigned)req, (unsigned)sp, (unsigned)pp, (unsigned)vt(pp, -1), (unsigned)vt(pp, 22), (unsigned)cb, (unsigned)vt(cb, -1), (unsigned)vt(cb, 20),
        (unsigned)h, (unsigned)hObj, (unsigned)hFl, (unsigned)old, (unsigned)oh, (unsigned)ohObj, (unsigned)ohFl);
}
void BorrowRestore(uintptr_t req)   // DetSet (manager job thread) at build, or SpawnLibFire (player thread) 3 s after submit
{
    for (int i = 0; i < 32; ++i) {
        Borrow& b = g_bw[i];
        if (b.req != req) continue;
        b.req = 0;
        const uintptr_t o = b.old, s = b.spec;
        if (o && Readable(o, 0x24) && *(const uint32_t*)o == 0x01E6D77Cu && *(const uintptr_t*)(o + 0x10) == s && *(const uint32_t*)(o + 0x20) == 4
            && Readable(s, 0x40) && (uintptr_t)InterlockedCompareExchange((volatile LONG*)(s + 0x3C), (LONG)o, (LONG)req) == req) InterlockedIncrement(&c_bwRestored);
        else InterlockedIncrement(&c_bwKept);
        return;
    }
}

// Submit through the SpawningManager (player thread, outside __try; spec/owner validated by the caller). The private owner
// is used for the naval owner class; any other owner class is passed as it is and gets no slot borrow (its callbacks are
// unread, and restoring the slot before an engine adoption is what crashed host 17860).
uintptr_t SubmitPrivate(uintptr_t spec, uintptr_t owner, uintptr_t ctx, int* priv, char* submitted)
{
    *submitted = 0;
    // Run at sea 2026-11-28 (host 25224): 4/4 private-owner submits never built. The naval builder sub_199D1F0 builds a request
    // at once only when request+0x0C == sub_194AFB0(player ctx)+8 (the naval owner interface); any other owner waits until
    // the spawn point is out of view / far (sub_718A30, sub_1C46A30 tests). So the REAL owner goes into the request for the
    // build, and DetSet (SpawnLibNoteBuilt, before every owner callback) swaps request+0x0C to the private owner.
    const uintptr_t po = PrivateOwnerFor(owner);
    *priv = (po && po != owner) ? 1 : 0;
    if (!po) return 0;
    g_pendSpec = *priv ? spec : 0; g_pendOld = *(const uintptr_t*)(spec + 0x3C);
    g_pendAny = spec; g_pendKind = SpecKindRaw(spec); g_pendCls = 0; g_pendReal = *priv ? owner : 0; g_pendPriv = *priv ? po : 0;
    { Spin s; for (int i = 0; i < g_libN; ++i) if (g_lib[i].spec == spec && g_lib[i].cls) { g_pendCls = g_lib[i].cls; break; } }
    g_ourReq = 0; t_tag = 1; SpawnShareReplayTag(1);
    // Submit with the PRIVATE owner: sub_79B8D0 appends the request to [req+0Ch]+4 (the owner's container, sub_48A390 at
    // 0x79B8F7), and a request of ours left in the real naval owner's container crashed both copies (runs 2026-11-28
    // guest 28276 / host 16964: sub_1978B50 walked owner+0x0C and called a freed request's vt[2], AV 0x1425954; host dump:
    // the list held exactly our 3 requests). Only after the submit does req+0x0C become the real owner, so the naval
    // builder (sub_199D1F0) takes its build-now path; DetSet swaps it back before any owner callback.
    *submitted = ((char(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t))0x00569560)(spec, po, ctx, 0, 0);
    SpawnShareReplayTag(0); t_tag = 0; g_pendSpec = 0; g_pendAny = 0;
    const uintptr_t req = g_ourReq; g_ourReq = 0;
    if (*priv && req && Readable(req, 0x14)) InterlockedCompareExchange((volatile LONG*)(req + 0x0C), (LONG)owner, (LONG)po);
    return req;
}

// Newest valid entry of a kind; key != 0 -> that key only; cls != 0 -> that learned ship class only. Invalid entries met on
// the way are marked bad (never submitted again).
bool Pick(int kind, uint64_t key, uint32_t cls, LibE* out, bool keyedOnly = false, uintptr_t exclude = 0)
{
    LibE cand[kLib]; int n = 0;
    { Spin s; for (int i = 0; i < g_libN; ++i) { const LibE& e = g_lib[i]; if (!e.bad && e.kind == kind && (!key || e.key == key) && (!cls || e.cls == cls) && (!keyedOnly || e.key) && e.spec != exclude
        && (kind != K_SHIP || e.ownerVt == kOwnerNavalVt)) cand[n++] = e; } }   // ships: naval owner only (T5: 5/5 other-owner ship submits never built)
    for (int pass = 0; pass < n; ++pass) {
        int best = -1;
        for (int i = 0; i < n; ++i) if (cand[i].spec && (best < 0 || cand[i].t > cand[best].t)) best = i;
        if (best < 0) break;
        if (SpecValid(cand[best])) {
            if (!SpecArmed(cand[best].spec)) { ++c_unarmed; cand[best].spec = 0; continue; }   // see SpecArmed (not marked bad: may re-arm)
            if (SpecUsableSt(SpecState(cand[best].spec))) { *out = cand[best]; return true; }   // see SpecUsableSt
            ++c_busy; cand[best].spec = 0; continue;
        }
        ++c_badSpec;
        { Spin s; for (int i = 0; i < g_libN; ++i) if (g_lib[i].spec == cand[best].spec && g_lib[i].owner == cand[best].owner) g_lib[i].bad = 1; }
        cand[best].spec = 0;
    }
    return false;
}
int TriedIdx(uint64_t id) { for (int i = 0; i < g_triedN; ++i) if (g_tried[i].id == id) return i; return -1; }
bool TryAllowed(uint64_t id, ULONGLONG now)   // at most 2 replays per id per 60 s
{
    int i = TriedIdx(id);
    if (i < 0) { if (g_triedN >= kTried) { for (int k = 1; k < kTried; ++k) g_tried[k - 1] = g_tried[k]; --g_triedN; } g_tried[g_triedN++] = { id, now, 0 }; i = g_triedN - 1; }
    if (now - g_tried[i].t0 > 60000) { g_tried[i].t0 = now; g_tried[i].n = 0; }
    return g_tried[i].n < 2;
}
void TryCount(uint64_t id) { const int i = TriedIdx(id); if (i >= 0) ++g_tried[i].n; }
bool PlayerFront(float dist, float* out)
{
    World* w = World::GetCurrentWorld();
    Entity* p = w ? w->GetPlayer() : nullptr;
    if (!p || !Readable((uintptr_t)p + 0x10, 0x40)) return false;
    const HookMath::Transform& m = p->m_GlobalMatrix;
    float fx = m.M21, fy = m.M22;   // UNVERIFIED: row 1 = forward for a Human (a wrong guess only rotates the drop point)
    const float l = sqrtf(fx * fx + fy * fy);
    if (l < 0.01f) { fx = 1.f; fy = 0.f; } else { fx /= l; fy /= l; }
    out[0] = m.M41 + fx * dist; out[1] = m.M42 + fy * dist; out[2] = m.M43;
    return true;
}
void Log(const char* fmt, ...)
{
    char line[1024]; va_list ap; va_start(ap, fmt); const int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap); va_end(ap);
    if (n < 0) { line[sizeof(line) - 2] = '\n'; line[sizeof(line) - 1] = 0; }   // truncated: keep the line ending
    Phase1_LogLine(line);
}
// __try helpers kept apart from the spinlock users (MSVC: no __try in a function with an object that has a destructor).
bool ReadSpecInfo(uintptr_t item, uintptr_t owner, int* kind, uintptr_t* specVt, uintptr_t* ownerVt)
{
    __try { *kind = SpecKindRaw(item); *specVt = *(const uint32_t*)item; *ownerVt = *(const uint32_t*)owner; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadBuiltShip(uintptr_t req, uintptr_t ent, uint32_t* cls, uintptr_t* spec)
{
    __try {
        if (*(const uint32_t*)ent != kShipVt) return false;
        *cls = ClassOfWord(*(const uint32_t*)(ent + 0x9C)); *spec = *(const uintptr_t*)(req + 0x10); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}  // namespace

// ---- notes (spawn_census.cpp hooks) ---------------------------------------------------------------------------------
int SpawnLibInitFlag() { return GetPrivateProfileIntA("Coop", "SpawnLib", 0, Paths::Ini()); }
void SpawnLibSetHooked(bool ok) { g_installed = ok; Log("[SpawnLib] hooks=%d\n", ok ? 1 : 0); }
bool SpawnLibOn()
{
    if (!g_installed) return false;
    static ULONGLONG s_next = 0; const ULONGLONG now = GetTickCount64();
    if (now >= s_next) {
        s_next = now + 1000;
        constexpr int kAbsent = -0x7F00;   // a transient empty read (ini being rewritten) keeps the previous values
        const int on = GetPrivateProfileIntA("Coop", "SpawnLib", kAbsent, Paths::Ini());
        if (on != kAbsent) {
            InterlockedExchange(&g_on, on != 0 ? 1 : 0);
            InterlockedExchange(&g_follow, GetPrivateProfileIntA("Coop", "SpawnLibFollow", 1, Paths::Ini()) != 0 ? 1 : 0);
            InterlockedExchange(&g_busyPol, GetPrivateProfileIntA("Coop", "SpawnLibBusy", 1, Paths::Ini()));
            InterlockedExchange(&g_lateRestore, GetPrivateProfileIntA("Coop", "SpawnLibLateRestore", 0, Paths::Ini()) != 0 ? 1 : 0);
        }
    }
    return g_on != 0;
}
// DetFac (any thread): our own submit's request.
void SpawnLibNoteFac(uintptr_t req)
{
    if (!t_tag || !req) return;
    g_ourReq = req;
    if (g_pendSpec) { Borrow& b = g_bw[(InterlockedIncrement(&g_bwNext) - 1) & 31]; b.spec = g_pendSpec; b.old = g_pendOld; b.t = GetTickCount64(); MemoryBarrier(); b.req = req; g_pendSpec = 0; }
    if (g_pendAny) { InfGuard gd; Inf& f = g_inf[InfSlot()]; f.spec = g_pendAny; f.kind = g_pendKind; f.cls = g_pendCls; f.t = GetTickCount64(); f.realOwner = g_pendReal; f.privOwner = g_pendPriv; f.built = 0; MemoryBarrier(); f.req = req; g_pendAny = 0; }
}
// SpawnKeyNoteReq (submit, game threads): one submitted request with its factory args; key = static spawn key or 0.
void SpawnLibNoteSubmit(uintptr_t item, uintptr_t owner, uintptr_t ctx, uint64_t key)
{
    if (!g_installed || !item || !owner || IsPrivateOwner(owner)) return;   // our own submits are not library entries
    int kind = K_NONE; uintptr_t specVt = 0, ownerVt = 0;
    if (!ReadSpecInfo(item, owner, &kind, &specVt, &ownerVt) || kind == K_NONE) return;
    const ULONGLONG now = GetTickCount64();
    Spin s;
    for (int i = 0; i < g_libN; ++i) {
        LibE& e = g_lib[i];
        if (e.spec == item && e.owner == owner) { e.ctx = ctx; e.t = now; e.specVt = specVt; e.ownerVt = ownerVt; e.bad = 0; if (key) e.key = key; return; }
    }
    {   // owner class census: one line per new (kind, owner vtable) - the private owner covers 0x02708E80 only
        static struct { int kind; uintptr_t vt; } s_ov[48]; static int s_ovN = 0; bool seen = false;
        for (int i = 0; i < s_ovN; ++i) if (s_ov[i].kind == kind && s_ov[i].vt == ownerVt) { seen = true; break; }
        if (!seen && s_ovN < 48) { s_ov[s_ovN++] = { kind, ownerVt }; Log("[SpawnLib] owner class kind=%d ownerVt=%08X specVt=%08X (spec %08X owner %08X) - %s\n", kind, (unsigned)ownerVt, (unsigned)specVt, (unsigned)item, (unsigned)owner, ownerVt == kOwnerNavalVt ? "private owner" : "real owner"); }
    }
    int slot = g_libN < kLib ? g_libN++ : 0;
    if (slot == 0 && g_libN == kLib) { for (int i = 1; i < kLib; ++i) if (g_lib[i].t < g_lib[slot].t) slot = i; }   // replace the oldest
    g_lib[slot] = { item, specVt, owner, ownerVt, ctx, key, kind, 0, now, 0 };
    InterlockedIncrement(&c_noted);
}
// DetSet (manager job thread): the entity built for req. Learns a ship spec's class; completes our own submit.
extern "C" void SpawnLibNoteBuilt(uintptr_t req, uintptr_t ent)   // C linkage: declared inside extern "C" SpawnKeyNoteEnt
{
    if (!g_installed || !req || !ent) return;
    for (int i = 0; i < kInf; ++i) {
        Inf& f = g_inf[i];
        if (f.req != req) continue;
        if (f.privOwner) {   // the build used the real owner; every callback from here on must reach the private one
            if ((uintptr_t)InterlockedCompareExchange((volatile LONG*)(req + 0x0C), (LONG)f.privOwner, (LONG)f.realOwner) == f.realOwner) InterlockedIncrement(&c_ownerSwap);
            else InterlockedIncrement(&c_ownerSwapMiss);
        }
        f.built = 1;   // kept: a built request still holds its spec (+0x10) until the client disposes it
        if (f.privOwner && Readable(f.spec, 0x40)) BuildProbe(req, f.spec, *(const uintptr_t*)(f.spec + 0x34));
        g_lastOurBuildMs = GetTickCount64();
    }
    if (!g_lateRestore) BorrowRestore(req);   // SpawnLibLateRestore: at completion (FoS4 / FoS10); 3 s fallback in SpawnLibFire
    if (g_fly.active && req == g_fly.req) { g_fly.ent = ent; g_fly.tBuilt = GetTickCount64(); }
    uint32_t cls = 0; uintptr_t spec = 0;
    if (!ReadBuiltShip(req, ent, &cls, &spec) || !cls || !spec) return;
    Spin s;
    for (int i = 0; i < g_libN; ++i) if (g_lib[i].spec == spec) { g_lib[i].cls = cls; InterlockedIncrement(&c_learned); }
}

uintptr_t SpawnLibSubmit(uintptr_t spec, uintptr_t owner, uintptr_t ctx, int* priv, char* submitted) { return SubmitPrivate(spec, owner, ctx, priv, submitted); }   // ship_spawn_replay.cpp
bool SpawnLibShipOwnerOk(uintptr_t owner) { return Readable(owner, 4) && *(const uint32_t*)owner == kOwnerNavalVt; }   // ship_spawn_replay.cpp
bool SpawnLibSpecUsable(uintptr_t spec) { return SpecArmed(spec) && SpecUsableSt(SpecState(spec)); }   // ship_spawn_replay.cpp (spec Readable 0x40 checked there)

// spawn_census.cpp DetSpecDtor: sub_501DF0 (SpawningSpecification dtor body) entry, any thread. Our queued requests on this
// spec are moved to a live armed substitute of the same kind (same learned class first); their slot borrow is dropped so
// nothing is ever written into the freed spec. Plain reads, spinlock, VirtualQuery only (no engine call).
extern "C" void SpawnLibSpecDying(uintptr_t spec)
{
    if (!g_installed || !spec) return;
    { Spin s; for (int i = 0; i < g_libN; ++i) if (g_lib[i].spec == spec) g_lib[i].bad = 1; }
    InfGuard gd;   // vs SpawnLibReqDying (request dtor, any thread): never write into a request being destroyed
    for (int i = 0; i < kInf; ++i) {
        Inf& f = g_inf[i];
        const uintptr_t req = f.req;
        if (!req || f.spec != spec) continue;
        InterlockedIncrement(&c_dying);
        for (int j = 0; j < 32; ++j) if (g_bw[j].req == req) g_bw[j].req = 0;
        // +0x31 = the request owns its spec (request dtor sub_79C640: sub_A1E800(spec)); such a request must keep it
        if (Readable(req, 0x34) && *(const uint8_t*)(req + 0x31)) { InterlockedIncrement(&c_redirOwned); continue; }
        LibE e;
        const bool have = (f.cls && Pick(f.kind, 0, f.cls, &e, false, spec)) || Pick(f.kind, 0, 0, &e, false, spec);
        if (have && Readable(req, 0x14) && *(const uint32_t*)req == 0x01E6D77Cu && *(const uintptr_t*)(req + 0x10) == spec) {
            InterlockedExchange((volatile LONG*)(req + 0x10), (LONG)e.spec); f.spec = e.spec; InterlockedIncrement(&c_redir);
        } else InterlockedIncrement(&c_redirFail);
    }
}

// spawn_census.cpp DetReqDtor: sub_79C640 (SpawningRequest dtor body, thiscall ecx = request; from the deleting dtor
// vt[1] sub_79DA90 and from sub_549F10) entry, any thread. Our request is leaving: drop its entry before the body runs
// (the body reads +0x10's +0x3C and may end the spec, which calls SpawnLibSpecDying on this thread).
extern "C" void SpawnLibReqDying(uintptr_t req)
{
    if (!g_installed || !req) return;
    InfGuard gd;
    for (int i = 0; i < kInf; ++i) if (g_inf[i].req == req) { g_inf[i].req = 0; ++c_reqDtor; }
}

ULONGLONG SpawnLibLastBuildMs() { return g_lastOurBuildMs; }   // phase1.cpp [ShipDisp]
void SpawnLibNoteOurShip(uintptr_t ent) { if (ent && g_ourShipsN < 64) g_ourShips[g_ourShipsN++] = ent; }   // ship_spawn_replay.cpp: crew diagnostics

// ---- ship spec supply for ship_spawn_replay.cpp (player thread) ------------------------------------------------------
bool SpawnLibPickShip(uint32_t cls, uintptr_t* spec, uintptr_t* specVt, uintptr_t* owner, uintptr_t* ownerVt, uintptr_t* ctx, uint32_t* gotCls)
{
    if (!g_installed) return false;
    LibE e;
    if (!(cls && Pick(K_SHIP, 0, cls, &e)) && !Pick(K_SHIP, 0, 0, &e)) return false;
    *spec = e.spec; *specVt = e.specVt; *owner = e.owner; *ownerVt = e.ownerVt; *ctx = e.ctx; *gotCls = e.cls;
    return true;
}

// ---- guest follow (phase1.cpp WideBindPrepass key round, player thread) ----------------------------------------------
void SpawnLibWantKey(uint64_t key, uint64_t hostObjId)
{
    if (!SpawnLibOn() || !g_follow || !key || (key >> 60) != 0x5) return;   // static keys only (crew keys follow their ship)
    for (int i = 0; i < g_qN; ++i) if (g_q[i].host == hostObjId) return;
    if (g_fly.active && g_fly.w.host == hostObjId) return;
    const ULONGLONG now = GetTickCount64();
    if (!TryAllowed(hostObjId, now) || g_qN >= kQ) return;
    g_q[g_qN++] = { K_NPC, P_FOLLOW, key, hostObjId, 0, 0, { 0, 0, 0 }, now };
    ++c_want; ++c_follow;
}

// phase1.cpp WideBindPrepass distance rounds: a follow spawn for this host guard is queued, in flight or was built
// < 4 s ago -> do not bind it by distance meanwhile (run 2026-11-28: the distance round took another guest body 6.9 m
// away first and the follow-built body was left unbound).
bool SpawnLibPendingFor(uint64_t hostObjId)
{
    if (!hostObjId || !g_installed || !g_follow) return false;
    for (int i = 0; i < g_qN; ++i) if (g_q[i].purpose == P_FOLLOW && g_q[i].host == hostObjId) return true;
    if (g_fly.w.purpose == P_FOLLOW && g_fly.w.host == hostObjId && (g_fly.active || (g_fly.tBuilt && GetTickCount64() - g_fly.tBuilt < 4000))) return true;
    return false;
}

// ---- player thread, OnFrame, outside every __try ---------------------------------------------------------------------
void SpawnLibFire()
{
    if (!SpawnLibOn() || !Phase1_InSave()) return;
    const ULONGLONG now = GetTickCount64();
    // 1. command: [Coop] SpawnCmd "ship[:cls]" / "npc[:keyhex]", fired when [Coop] SpawnCmdSeq changes
    static int s_seq = -1; static ULONGLONG s_nextCmd = 0;
    if (now >= s_nextCmd) {
        s_nextCmd = now + 1000;
        // Sentinel defaults: an ini being rewritten reads as empty for a moment (run T5: 39 phantom commands from 6 changes).
        constexpr int kAbsent = -0x7F00;
        const int seq = GetPrivateProfileIntA("Coop", "SpawnCmdSeq", kAbsent, Paths::Ini());
        const int pid = GetPrivateProfileIntA("Coop", "SpawnCmdPid", kAbsent, Paths::Ini());
        // + debounce: a new value must be read twice in a row (1 s apart); a key caught mid-write reads as an empty value = 0
        // (run T6: 19 commands from 6 changes even with the sentinel).
        static int s_cand = -1;
        const bool stable = (seq == s_cand); s_cand = seq;
        if (seq == kAbsent || pid == kAbsent || !stable) {}   // transient or not yet confirmed: no decision this second
        else if (s_seq < 0) s_seq = seq;   // the value at start is not a command
        else if (seq != s_seq && (pid == 0 || (DWORD)pid == GetCurrentProcessId())) {   // SpawnCmdPid: one copy only
            s_seq = seq;
            char cmd[64] = {}; GetPrivateProfileStringA("Coop", "SpawnCmd", "", cmd, sizeof(cmd), Paths::Ini());
            Want w = {}; w.purpose = P_CMD; w.t = now;
            if (!_strnicmp(cmd, "ship", 4)) { w.kind = K_SHIP; if (cmd[4] == ':') w.cls = (uint32_t)atoi(cmd + 5); }
            else if (!_strnicmp(cmd, "npc", 3)) { w.kind = K_NPC; if (cmd[3] == ':') w.key = _strtoui64(cmd + 4, nullptr, 16); }
            if (w.kind && g_qN < kQ) {
                w.hasPlace = PlayerFront(w.kind == K_SHIP ? 80.f : 4.f, w.place) ? 1 : 0;
                g_q[g_qN++] = w; ++c_want; ++c_cmd;
                Log("[SpawnLib] cmd seq=%d '%s' -> kind=%d cls=%u key=%016llX place=%d (%.1f, %.1f, %.1f)\n", seq, cmd, w.kind, w.cls,
                    (unsigned long long)w.key, w.hasPlace, w.place[0], w.place[1], w.place[2]);
            }
        }
    }
    // 2. our request in flight: built -> place (command) -> done; or timeout
    if (g_fly.active) {
        const uintptr_t ent = g_fly.ent;
        if (ent) {
            if (g_fly.tBuilt && now - g_fly.tBuilt >= (g_fly.w.hasPlace ? kPlaceDelayMs : 0)) {
                const uint32_t evt = Readable(ent, 0xA0) ? *(const uint32_t*)ent : 0;
                const uint32_t cls = (evt == kShipVt) ? ClassOfWord(*(const uint32_t*)(ent + 0x9C)) : 0;
                ++c_built;
                Log("[SpawnLib] built kind=%d purpose=%d ent=%08X evt=%08X cls=%u key=%016llX host=%016llX dt=%llums\n", g_fly.w.kind, g_fly.w.purpose,
                    (unsigned)ent, evt, cls, (unsigned long long)g_fly.w.key, (unsigned long long)g_fly.w.host, now - g_fly.t);
                if (evt == kShipVt && g_ourShipsN < 64) g_ourShips[g_ourShipsN++] = ent;   // crew diagnostics
                if (g_fly.w.hasPlace && (evt == kShipVt || evt == kNpcVt)) {
                    HookMath::Transform m = ((Entity*)ent)->m_GlobalMatrix;
                    m.M41 = g_fly.w.place[0]; m.M42 = g_fly.w.place[1]; m.M43 = g_fly.w.place[2];
                    ((Entity*)ent)->SetGlobalMatrixInternal(&m, true);   // validated above; post-add write (pre-add writes did not hold)
                    ++c_placed;
                    const HookMath::Vector4 p = ((Entity*)ent)->GetPosition();
                    Log("[SpawnLib] placed ent=%08X at (%.1f, %.1f, %.1f) now (%.1f, %.1f, %.1f)\n", (unsigned)ent, g_fly.w.place[0], g_fly.w.place[1], g_fly.w.place[2], p.x, p.y, p.z);
                }
                g_fly.active = 0;
            }
        } else if (now - g_fly.t > kBuildTimeoutMs) {
            ++c_failed;
            Log("[SpawnLib] failed kind=%d purpose=%d req=%08X (no build in %llums)\n", g_fly.w.kind, g_fly.w.purpose, (unsigned)g_fly.req, kBuildTimeoutMs);
            g_fly.active = 0;
        }
    }
    // 2a. old-ship watch (see BuildProbe)
    for (int i = 0; i < 16; ++i) {
        OldWatch& w = g_ow[i]; const uintptr_t e = w.ent;
        if (!e) continue;
        if (now - w.t > 1000) { w.ent = 0; ++c_owStill; continue; }
        if (!Readable(e, 0x50) || *(const uint32_t*)e != kShipVt) { w.ent = 0; continue; }
        const float* m = (const float*)(e + 0x10); const float dx = m[12] - w.p[0], dy = m[13] - w.p[1], dz = m[14] - w.p[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 30.0f) { ++c_owMoved; Log("[SpawnLib] old ship moved ent=%08X d=%.1f to=(%.1f, %.1f, %.1f) msAfterBuild=%llu late=%ld\n", (unsigned)e, d, m[12], m[13], m[14], now - w.t, g_lateRestore); w.ent = 0; }
    }
    // 2b. our queued requests: give the borrowed slot back after 3 s; drop table entries whose request is dead (vt gone)
    static ULONGLONG s_maint = 0;
    if (now >= s_maint) {
        s_maint = now + 1000;
        for (int i = 0; i < 32; ++i) { Borrow& b = g_bw[i]; const uintptr_t r = b.req; if (r && now - b.t >= kBorrowEarlyMs) { BorrowRestore(r); ++c_bwEarly; } }
        InfGuard gd;   // fallback only: the request dtor hook (SpawnLibReqDying) drops entries exactly
        for (int i = 0; i < kInf; ++i) {
            Inf& f = g_inf[i]; const uintptr_t r = f.req;
            if (r && now - f.t > 15000 && !(Readable(r, 0x14) && *(const uint32_t*)r == 0x01E6D77Cu && *(const uintptr_t*)(r + 0x10) == f.spec)) { f.req = 0; ++c_infDead; }
        }
    }
    // 2c. crew diagnostics (every 10 s): component vtables of every built entity anchored on one of our ships, once each.
    // Run T10/T11: crews of our ships never became guard-lane candidates (skeleton + BhvGenericNPC 0x026E34D8 + AI/health).
    static ULONGLONG s_crewDiag = 0;
    if (now >= s_crewDiag && g_ourShipsN) {
        s_crewDiag = now + 10000;
        static uintptr_t s_logged[512]; static int s_loggedN = 0;
        for (int si = 0; si < g_ourShipsN; ++si) {
            const uintptr_t ship = g_ourShips[si];
            if (!Readable(ship, 0xA0) || *(const uint32_t*)ship != kShipVt) continue;
            uintptr_t crew[48]; int kinds[48]; float loc[48 * 3]; const int nc = SpawnCrewOfShip(ship, crew, kinds, loc, 48);
            int nNew = 0;
            for (int c = 0; c < nc; ++c) {
                bool seen = false; for (int j = 0; j < s_loggedN; ++j) if (s_logged[j] == crew[c]) { seen = true; break; }
                if (seen || s_loggedN >= 512) continue;
                s_logged[s_loggedN++] = crew[c]; ++nNew;
                const uintptr_t e = crew[c];
                char comps[400]; int k = 0; comps[0] = 0; int ncomp = 0; uint32_t evt = 0; float pos[3] = {};
                if (Readable(e, 0x68)) {
                    evt = *(const uint32_t*)e;
                    ncomp = *(const uint16_t*)(e + 0x66);
                    const uintptr_t data = *(const uintptr_t*)(e + 0x60);
                    if (ncomp > 0 && ncomp <= 96 && Readable(data, (size_t)ncomp * 4))
                        for (int q = 0; q < ncomp && k < 380; ++q) {
                            const uintptr_t cp = ((const uintptr_t*)data)[q];
                            k += _snprintf_s(comps + k, sizeof(comps) - k, _TRUNCATE, " %08X", (cp && Readable(cp, 4)) ? *(const uint32_t*)cp : 0u);
                        }
                    if (evt == kNpcVt || evt == kShipVt) { const HookMath::Vector4 p = ((Entity*)e)->GetPosition(); pos[0] = p.x; pos[1] = p.y; pos[2] = p.z; }
                }
                Log("[CrewDiag] ship=%08X crew=%08X kind=%d local=(%.2f, %.2f, %.2f) evt=%08X comps=%d pos=(%.1f, %.1f, %.1f):%s\n", (unsigned)ship, (unsigned)e, kinds[c],
                    loc[3 * c], loc[3 * c + 1], loc[3 * c + 2], evt, ncomp, pos[0], pos[1], pos[2], comps);
            }
            if (nNew || nc == 0) Log("[CrewDiag] ship=%08X built entities anchored on it: %d (new %d)\n", (unsigned)ship, nc, nNew);
        }
    }
    // 2d. crew census (every 10 s, any ship): built entities anchored on each ship, and how many are AI NPCs (component
    // BhvGenericNPC 0x026E34D8, the guard lane's gate, phase1.cpp:7444) - only those can carry crew keys. One line per ship
    // whenever its numbers change. Ship id = HandleContainer id (*(ship+0xC8) +0x0C), class = entity+0x9C decode.
    static ULONGLONG s_crewCensus = 0;
    if (now >= s_crewCensus) {
        s_crewCensus = now + 10000;
        static struct { uintptr_t ship; int n, nAi; } s_last[64]; static int s_lastN = 0;
        uintptr_t anc[48]; const int na = SpawnCrewAnchors(anc, 48);
        for (int a = 0; a < na; ++a) {
            const uintptr_t ship = anc[a];
            if (!Readable(ship, 0xD0) || *(const uint32_t*)ship != kShipVt) continue;
            uintptr_t crew[48]; int kinds[48]; float loc[48 * 3]; const int nc = SpawnCrewOfShip(ship, crew, kinds, loc, 48);
            int alive = 0, nAi = 0;
            for (int c = 0; c < nc; ++c) {
                const uintptr_t e = crew[c];
                if (!Readable(e, 0x68) || *(const uint32_t*)e != kNpcVt) continue;
                ++alive;
                const int ncomp = *(const uint16_t*)(e + 0x66); const uintptr_t data = *(const uintptr_t*)(e + 0x60);
                if (ncomp > 0 && ncomp <= 96 && Readable(data, (size_t)ncomp * 4))
                    for (int q = 0; q < ncomp; ++q) { const uintptr_t cp = ((const uintptr_t*)data)[q]; if (cp && Readable(cp, 4) && *(const uint32_t*)cp == 0x026E34D8u) { ++nAi; break; } }
            }
            int li = -1; for (int j = 0; j < s_lastN; ++j) if (s_last[j].ship == ship) { li = j; break; }
            if (li >= 0 && s_last[li].n == alive && s_last[li].nAi == nAi) continue;
            if (li < 0 && s_lastN < 64) { li = s_lastN++; s_last[li].ship = ship; }
            if (li >= 0) { s_last[li].n = alive; s_last[li].nAi = nAi; }
            const uintptr_t hc = *(const uintptr_t*)(ship + 0xC8);
            const uint32_t sid = (hc && Readable(hc, 0x14)) ? *(const uint32_t*)(hc + 0x0C) : 0;
            const HookMath::Vector4 p = ((Entity*)ship)->GetPosition();
            Log("[CrewCensus] ship=%08X id=%08X cls=%u crew=%d aiNpc=%d pos=(%.0f, %.0f, %.0f)%s\n", (unsigned)ship, sid, ClassOfWord(*(const uint32_t*)(ship + 0x9C)),
                alive, nAi, p.x, p.y, p.z, IsOurShip(ship) ? " ours" : "");
        }
    }
    // 3. stats every 10 s
    static ULONGLONG s_stats = 0;
    if (now >= s_stats) {
        s_stats = now + 10000;
        int ships = 0, npcs = 0, keyed = 0, learned = 0, idleS = 0, idleN = 0, armS = 0, armN = 0;
        { Spin s; for (int i = 0; i < g_libN; ++i) { if (g_lib[i].bad) continue; const bool rd = Readable(g_lib[i].spec, 0x40); const int idle = rd && *(const uintptr_t*)(g_lib[i].spec + 0x3C) == 0; const int arm = rd && SpecArmed(g_lib[i].spec);
            if (g_lib[i].kind == K_SHIP) { ++ships; learned += g_lib[i].cls != 0; idleS += idle; armS += arm; } else { ++npcs; keyed += g_lib[i].key != 0; idleN += idle; armN += arm; } } }
        Log("[SpawnLib] stats lib ships=%d (classKnown %d idle %d armed %d) npcs=%d (keyed %d idle %d armed %d) | want=%ld cmd=%ld follow=%ld fired=%ld submit0=%ld built=%ld placed=%ld noSpec=%ld failed=%ld badSpec=%ld busySkip=%ld unarmedSkip=%ld busyState(1/2/4/6/9/other/bad)=%ld/%ld/%ld/%ld/%ld/%ld/%ld pol=%ld queue=%d | owner priv=%ld real=%ld slot restored=%ld kept=%ld | ownerSwap=%ld miss=%ld vt4caller=%08X | early=%ld | tracked=%d built=%d reqDtor=%ld dead=%ld specDying=%ld redirected=%ld redirectFail=%ld owned=%ld | lateRestore=%ld oldShip moved=%ld still=%ld | privCalls(4/5/6/7/8/10/11/17/19)=%ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld\n",
            ships, learned, idleS, armS, npcs, keyed, idleN, armN, c_want, c_cmd, c_follow, c_fired, c_submit0, c_built, c_placed, c_noSpec, c_failed, c_badSpec, c_busy, c_unarmed, c_st[0], c_st[1], c_st[2], c_st[3], c_st[4], c_st[5], c_st[6], g_busyPol, g_qN, c_foUsed, c_realOwner, c_bwRestored, c_bwKept, c_ownerSwap, c_ownerSwapMiss, (unsigned)g_fo4Caller, c_bwEarly, InfCount(), InfBuilt(), c_reqDtor, c_infDead, c_dying, c_redir, c_redirFail, c_redirOwned, c_lateRestore, c_owMoved, c_owStill, c_fo[4], c_fo[5], c_fo[6], c_fo[7], c_fo[8], c_fo[10], c_fo[11], c_fo[17], c_fo[19]);
    }
    // 4. fire at most one request
    if (g_fly.active || g_qN == 0 || now - g_lastFire < kGapMs) return;
    const Want w = g_q[0];
    for (int i = 1; i < g_qN; ++i) g_q[i - 1] = g_q[i];
    --g_qN;
    g_lastFire = now;
    LibE e;
    bool have = false;
    if (w.kind == K_SHIP) have = (w.cls && Pick(K_SHIP, 0, w.cls, &e)) || Pick(K_SHIP, 0, 0, &e);
    else if (w.key) have = Pick(K_NPC, w.key, 0, &e);
    else { have = Pick(K_NPC, 0, 0, &e, true) || Pick(K_NPC, 0, 0, &e); }   // prefer a keyed spec: the guest can follow it by key
    static unsigned s_noSpecLog = 0;
    if (!have) { ++c_noSpec; if (s_noSpecLog++ < 20) Log("[SpawnLib] no valid idle spec kind=%d cls=%u key=%016llX purpose=%d\n", w.kind, w.cls, (unsigned long long)w.key, w.purpose); return; }
    if (w.purpose == P_FOLLOW) TryCount(w.host);
    const uintptr_t old = *(const uintptr_t*)(e.spec + 0x3C);   // spec Readable 0x40 (Pick); usable per SpecUsableSt
    const int oldSt = SpecState(e.spec);
    int priv = 0; char submitted = 0;
    const uintptr_t req = SubmitPrivate(e.spec, e.owner, e.ctx, &priv, &submitted);
    // No restore (2026-11-28: both restore variants crashed - guest 17552 blind, host 17860 validated - see SpecUsableSt).
    ++c_fired;
    Log("[SpawnLib] req kind=%d purpose=%d spec=%08X owner=%08X ctx=%08X key=%016llX cls=%u(lib %u) host=%016llX -> submitted=%d req=%08X old3C=%08X oldState=%d privOwner=%d\n",
        w.kind, w.purpose, (unsigned)e.spec, (unsigned)e.owner, (unsigned)e.ctx, (unsigned long long)e.key, w.cls, e.cls, (unsigned long long)w.host,
        (int)submitted, (unsigned)req, (unsigned)old, oldSt, priv);
    if (!submitted || !req) { ++c_submit0; return; }
    g_fly.req = req; g_fly.ent = 0; g_fly.tBuilt = 0; g_fly.w = w; g_fly.spec = e.spec; g_fly.t = now; g_fly.active = 1;
}

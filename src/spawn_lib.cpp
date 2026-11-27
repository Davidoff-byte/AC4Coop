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
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

void Phase1_LogLine(const char* s);
bool Phase1_InSave();
void SpawnShareReplayTag(int v);   // spawn_census.cpp: SpawnShareDecide passes our own submit

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
// Newest valid entry of a kind; key != 0 -> that key only; cls != 0 -> that learned ship class only. Invalid entries met on
// the way are marked bad (never submitted again).
bool Pick(int kind, uint64_t key, uint32_t cls, LibE* out, bool keyedOnly = false)
{
    LibE cand[kLib]; int n = 0;
    { Spin s; for (int i = 0; i < g_libN; ++i) { const LibE& e = g_lib[i]; if (!e.bad && e.kind == kind && (!key || e.key == key) && (!cls || e.cls == cls) && (!keyedOnly || e.key)) cand[n++] = e; } }
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
    char line[400]; va_list ap; va_start(ap, fmt); _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap); va_end(ap);
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
        InterlockedExchange(&g_on, GetPrivateProfileIntA("Coop", "SpawnLib", 0, Paths::Ini()) != 0 ? 1 : 0);
        InterlockedExchange(&g_follow, GetPrivateProfileIntA("Coop", "SpawnLibFollow", 1, Paths::Ini()) != 0 ? 1 : 0);
        InterlockedExchange(&g_busyPol, GetPrivateProfileIntA("Coop", "SpawnLibBusy", 1, Paths::Ini()));
    }
    return g_on != 0;
}
// DetFac (any thread): our own submit's request.
void SpawnLibNoteFac(uintptr_t req) { if (t_tag && req) g_ourReq = req; }
// SpawnKeyNoteReq (submit, game threads): one submitted request with its factory args; key = static spawn key or 0.
void SpawnLibNoteSubmit(uintptr_t item, uintptr_t owner, uintptr_t ctx, uint64_t key)
{
    if (!g_installed || !item || !owner) return;
    int kind = K_NONE; uintptr_t specVt = 0, ownerVt = 0;
    if (!ReadSpecInfo(item, owner, &kind, &specVt, &ownerVt) || kind == K_NONE) return;
    const ULONGLONG now = GetTickCount64();
    Spin s;
    for (int i = 0; i < g_libN; ++i) {
        LibE& e = g_lib[i];
        if (e.spec == item && e.owner == owner) { e.ctx = ctx; e.t = now; e.specVt = specVt; e.ownerVt = ownerVt; e.bad = 0; if (key) e.key = key; return; }
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
    if (g_fly.active && req == g_fly.req) { g_fly.ent = ent; g_fly.tBuilt = GetTickCount64(); }
    uint32_t cls = 0; uintptr_t spec = 0;
    if (!ReadBuiltShip(req, ent, &cls, &spec) || !cls || !spec) return;
    Spin s;
    for (int i = 0; i < g_libN; ++i) if (g_lib[i].spec == spec) { g_lib[i].cls = cls; InterlockedIncrement(&c_learned); }
}

bool SpawnLibSpecUsable(uintptr_t spec) { return SpecArmed(spec) && SpecUsableSt(SpecState(spec)); }   // ship_spawn_replay.cpp (spec Readable 0x40 checked there)

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
        const int seq = GetPrivateProfileIntA("Coop", "SpawnCmdSeq", 0, Paths::Ini());
        if (s_seq < 0) s_seq = seq;   // the value at start is not a command
        else if (seq != s_seq && (GetPrivateProfileIntA("Coop", "SpawnCmdPid", 0, Paths::Ini()) == 0 || (DWORD)GetPrivateProfileIntA("Coop", "SpawnCmdPid", 0, Paths::Ini()) == GetCurrentProcessId())) {   // SpawnCmdPid: one copy only
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
    // 3. stats every 10 s
    static ULONGLONG s_stats = 0;
    if (now >= s_stats) {
        s_stats = now + 10000;
        int ships = 0, npcs = 0, keyed = 0, learned = 0, idleS = 0, idleN = 0, armS = 0, armN = 0;
        { Spin s; for (int i = 0; i < g_libN; ++i) { if (g_lib[i].bad) continue; const bool rd = Readable(g_lib[i].spec, 0x40); const int idle = rd && *(const uintptr_t*)(g_lib[i].spec + 0x3C) == 0; const int arm = rd && SpecArmed(g_lib[i].spec);
            if (g_lib[i].kind == K_SHIP) { ++ships; learned += g_lib[i].cls != 0; idleS += idle; armS += arm; } else { ++npcs; keyed += g_lib[i].key != 0; idleN += idle; armN += arm; } } }
        Log("[SpawnLib] stats lib ships=%d (classKnown %d idle %d armed %d) npcs=%d (keyed %d idle %d armed %d) | want=%ld cmd=%ld follow=%ld fired=%ld submit0=%ld built=%ld placed=%ld noSpec=%ld failed=%ld badSpec=%ld busySkip=%ld unarmedSkip=%ld busyState(1/2/4/6/9/other/bad)=%ld/%ld/%ld/%ld/%ld/%ld/%ld pol=%ld queue=%d\n",
            ships, learned, idleS, armS, npcs, keyed, idleN, armN, c_want, c_cmd, c_follow, c_fired, c_submit0, c_built, c_placed, c_noSpec, c_failed, c_badSpec, c_busy, c_unarmed, c_st[0], c_st[1], c_st[2], c_st[3], c_st[4], c_st[5], c_st[6], g_busyPol, g_qN);
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
    g_ourReq = 0; t_tag = 1; SpawnShareReplayTag(1);
    const char submitted = ((char(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t))0x00569560)(e.spec, e.owner, e.ctx, 0, 0);
    SpawnShareReplayTag(0); t_tag = 0;
    const uintptr_t req = g_ourReq; g_ourReq = 0;
    // No restore (2026-11-28: both restore variants crashed - guest 17552 blind, host 17860 validated - see SpecUsableSt).
    ++c_fired;
    Log("[SpawnLib] req kind=%d purpose=%d spec=%08X owner=%08X ctx=%08X key=%016llX cls=%u(lib %u) host=%016llX -> submitted=%d req=%08X old3C=%08X oldState=%d\n",
        w.kind, w.purpose, (unsigned)e.spec, (unsigned)e.owner, (unsigned)e.ctx, (unsigned long long)e.key, w.cls, e.cls, (unsigned long long)w.host,
        (int)submitted, (unsigned)req, (unsigned)old, oldSt);
    if (!submitted || !req) { ++c_submit0; return; }
    g_fly.req = req; g_fly.ent = 0; g_fly.tBuilt = 0; g_fly.w = w; g_fly.spec = e.spec; g_fly.t = now; g_fly.active = 1;
}

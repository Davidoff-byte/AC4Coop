// Coordinator sync ([Coop] CoordSync, default 0) - the schema table and the lane that drives it.
//
// The chain that finds a GcL* object is coord_enum.h/coord_enum.cpp (read-only, receipted): entity+0x60 ->
// GameplayCoordinatorComponent -> +0x60 Coordinator -> +0x64 GameplayLogic. This file adds the writable
// half: a per-class table of {identity, extract, apply} and one intent channel (IC_COORD) that carries a
// single (field, float) pair per message.
//
// Deliberate split. Every engine route - the hull getter slot 0x1B4, the raw hull write at +0xD0, the sail
// setter 0x01220960 with its clone guard, GetLocalShip, the ShipMirror pool - already exists exactly once
// in phase1.cpp. This file owns none of it: it calls six one-line accessors defined beside those routes
// (CoordSync* glue, right after the [Coop] SailSync block) so a change to a route cannot drift apart from
// the coordinator lane. The only constants here are the GcLShip vtable id and the two field ids.
//
// Read-only where not applying: the host side reads (CoordCollect, extract) and never writes game memory;
// the guest side applies through the schema, every call Readable()-guarded and SEH-wrapped, and the apply
// is counted rather than fatal. With the flag 0 nothing here runs and no game memory is read.
//
// Why the host side is rate-limited: CoordCollect walks the whole entity manager (up to 4096 slots) and the
// census probe runs it every 10 s. At 10 Hz (kHostWalkMs) it is still cheap and well inside the oracle's
// 1 s bound; collecting every frame would be the only per-frame cost this lane adds and it is avoidable.
#include "coord_sync.h"
#include "coord_enum.h"
#include "paths.h"
#include "net.h"
#include "netstate.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

// ---- the six routes into phase1.cpp (defined right after the [Coop] SailSync block) ------------------
// Each is a one-line wrapper over a static that already exists there; none adds an engine address. They
// are at global scope, like coord_enum.cpp's Phase1_LogLine, because they have external linkage.
void*    CoordSyncLocalShipEntity();                 // GetLocalShip()
void*    CoordSyncEntityLogic(void* ent);            // ShipGcLObj(ent)
bool     CoordSyncHullRead(void* gcl, float* out);   // hull HP on the GcL itself (slot 0x1B4 getter)
bool     CoordSyncHullWriteRaw(void* ent, float hp); // WriteShipHullHPRaw
int      CoordSyncSailApply(void* ent, unsigned st, uint64_t key);  // ShipGcLObj + SailSetViaEngine
bool     CoordSyncHelmApply(void* gcl, unsigned on); // sub_11FE840(gcl, on) - the helm lever, [Coop] HelmSync
uint64_t CoordSyncProxyHostId(const void* ent);      // live ShipMirror proxy -> its hostId, else 0
void*    CoordSyncProxyEntity(uint64_t hostId);      // live ShipMirror slot for a hostId, else nullptr
uint64_t CoordSyncObjectId(const void* obj);         // GetObjectID 0x9F0B50, read-only, all 64 bits back

// Already exported from phase1.cpp (coord_enum.cpp declares Phase1_LogLine the same way).
bool Phase1_IsEntityHost();
void Phase1_LogLine(const char* s);

namespace {

// ---- the six routes into phase1.cpp (defined right after the [Coop] SailSync block) ------------------
const int kHostWalkMs   = 100;   // host: one CoordCollect + change diff each 100 ms (oracle bound is 1 s)
const int kMaxCollect   = 128;   // same shot size CoordEnumTick asks for
const int kMaxTxSlots   = 8;     // (key, field) pairs the host remembers last-published values for
const int kMaxRxSlots   = 16;    // guest freshness slots; evict the least recently used
const DWORD kReportMs   = 5000;  // guest counter line

// Readable(): the same guard coord_enum.cpp uses (this module must not fault on a reclaimed object).
bool Readable(const void* p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (!a || a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t end = a + n;
    return end >= a && end <= (uintptr_t)m.BaseAddress + m.RegionSize;
}

// ---- [Coop] HelmSync: GcLShip+0x39A, the helm flag (ops/evidence/helm-state-2026-11-21.md) -----------
// The state is the byte itself: 12 inline readers, no getter, and no "who holds it" field (Q2). Writers are
// exactly three - the lever 0x011FE840, the reset 0x01204124, the object copy 0x0130A7D7 - and none of them
// is per-tick, so unlike the hull at +0xD0 a value written here is not undone by regeneration.
const int      kHelmOff    = 0x39A;
const uint32_t kGcLShipVt  = 0x026A34E8u;     // the vtable guard every write in this lane already takes

bool HelmSyncOn() {
    static int s_ini = -1;   // the s_x = -1 idiom: read once
    if (s_ini < 0) s_ini = GetPrivateProfileIntA("Coop", "HelmSync", 0, Paths::Ini()) != 0 ? 1 : 0;
    return s_ini == 1;
}

// Read the helm byte off a GcL object: vtable-gated (a foreign object's +0x39A means nothing) and refused
// when the byte is not 0/1, the same refusal the sail field makes before it can reach the wire.
bool HelmRead(void* logic, uint8_t* out) {
    if (!logic || !out) return false;
    if (!Readable((const char*)logic + kHelmOff, 1)) return false;
    __try {
        if (*(uint32_t*)logic != kGcLShipVt) return false;
        const uint8_t v = *(uint8_t*)((char*)logic + kHelmOff);
        if (v > 1) return false;
        *out = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- schema callbacks for GcLShip ---------------------------------------------------------------
// Identity, exactly as the table contract states: the local ship is key 0 on both machines; a ShipMirror
// proxy is identified by the host's deep-copied entity id; everything else is not synced. Both lookups are
// pointer comparisons inside phase1.cpp (no dereference of the candidate), so no SEH is needed here.
uint64_t ShipIdentity(void* ent, void* logic) {
    (void)logic;                                          // a GcLShip is identified by its entity
    if (!ent) return kCoordKeyNone;
    if (ent == CoordSyncLocalShipEntity()) return kCoordKeyLocal;
    const uint64_t hostId = CoordSyncProxyHostId(ent);
    return hostId ? hostId : kCoordKeyNone;
}

// `logic` is the GcL object off the CoordRef, never guessed: field 1 is the hull getter route, field 2 the
// dword at +0xCC refused when it is not a SailState (the same refusal SailPublishTick makes before it
// publishes, so a garbage read can never reach the wire).
bool ShipExtract(void* logic, uint8_t field, float* out) {
    if (!logic || !out) return false;
    if (field == CF_HULL) return CoordSyncHullRead(logic, out);
    if (field == CF_SAIL) {
        if (!Readable((const char*)logic + 0xCC, 4)) return false;
        __try {
            const uint32_t st = *(uint32_t*)((char*)logic + 0xCC);
            if (st >= 4u) return false;      // SailState::Count: not a sail state
            *out = (float)st;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    if (field == CF_HELM) {
        uint8_t b = 0;
        if (!HelmRead(logic, &b)) return false;
        *out = (float)b;
        return true;
    }
    return false;
}

// The wire key of the message being applied, for the sail setter's log label only (it logs the identity it
// was handed; IC_SHIP passes msg.wireId the same way). Set by the consumer immediately before the call.
uint64_t s_applyKey = 0;

// Hull apply goes through the engine setter, never a raw +0xD0 write. A raw write is undone within a frame
// or two: regeneration sub_1211EE0 does `+0xD0 = max*rate*dt + +0xD0` clamped to `+0xD4`, and at high hull
// `+0xD4` is -1.0 ("no cap, regenerate to max"), so the write arms a full regen. 0x01225A10 is GcLShip
// vtable slot +0x12C, `void __thiscall f(GcLShip*, float newHull)`: it sets the pool, HealthState, `+0xD4`
// and the bar fraction at *(ship+0x570)+0x32C. It also rewrites `+0xD4` itself (via sub_1220B60), so the cap
// that holds the value is written back AFTER the call. Evidence:
// ops/evidence/ship-hull-live-write-2026-11-21.md (sections 1 and 2).
LONG s_hullExc = 0;   // faults caught here; a bad GcL costs one message, never the frame

bool ShipApplyHull(void* ent, float v) {
    void* gcl = CoordSyncEntityLogic(ent);                   // ShipGcLObj glue (phase1.cpp)
    if (!gcl || !Readable(gcl, 0xB70)) return false;
    // Vtable gate (guest-crash-2026-11-21 analysis, item 2): 0x01225A10 is GcLShip vtable slot +0x12C and is
    // only valid on a GcLShip. coord_sync.h:7/37 pins that vtable at 0x026A34E8 (coord_enum.cpp census, n=9).
    // Calling it on anything else walks a foreign vtable slot into an arbitrary function.
    if (*(uint32_t*)gcl != 0x026A34E8u) return false;
    float after = 0.0f, cap = 0.0f;
    int ok = 0;
    __try {
        float maxHull = *(float*)((char*)gcl + 0xB4);        // +0xB4 = max hull, +0xD0 = current HP
        if (!(maxHull > 0.0f)) maxHull = 0.0f;
        if (!(v > 0.0f)) v = 0.0f;                           // also turns NaN into 0
        if (v > maxHull) v = maxHull;
        ((void (__thiscall*)(void*, float))0x01225A10)(gcl, v);
        *(float*)((char*)gcl + 0xD4) = v;                    // hold: cap regen at the applied value
        after = *(float*)((char*)gcl + 0xD0);
        cap = *(float*)((char*)gcl + 0xD4);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++s_hullExc; }
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] hull apply ship=%p gcl=%p %.1f->%.1f cap=%.1f ok=%d\n",
                ent, gcl, v, after, cap, ok);
    Phase1_LogLine(line);
    return ok == 1;
}

bool ShipApplyHelm(void* ent, float v) {
    if (!(v == 0.0f || v == 1.0f)) return false;          // never hand the lever a non-helm value
    void* gcl = CoordSyncEntityLogic(ent);
    uint8_t cur = 0;
    if (!HelmRead(gcl, &cur)) return false;               // vtable + readable gate, before the call
    // The lever, not a raw store at +0x39A: sub_11FE840 ALSO resolves ship+0xA4 to the controller object and
    // dispatches the two interface calls, notifies +0x570 and fires the enter/leave events (evidence Q1). A
    // raw store would set the byte and leave the controller unaware - the state would look right and the seat
    // would not be handed over. The lever itself early-returns when the byte already equals `on`.
    return CoordSyncHelmApply(gcl, v >= 0.5f ? 1u : 0u);
}

bool ShipApply(void* ent, uint8_t field, float v) {
    if (!ent) return false;
    if (field == CF_HULL) return ShipApplyHull(ent, v);
    if (field == CF_HELM) return ShipApplyHelm(ent, v);
    if (field == CF_SAIL) {
        if (!(v >= 0.0f) || !(v < 4.0f)) return false;   // never hand the setter a non-SailState
        return CoordSyncSailApply(ent, (unsigned)v, s_applyKey) == 1;
    }
    return false;
}

// ---- schema callbacks for GcLBoardingBattle (battle objectives; [Coop] BoardObjectiveSync) ----------
// Six BattleObjective* at +0x60..+0x74 of one GcLBoardingBattle (class-info 0x02917FC0, vtable 0x026AEFD8,
// size 0x3E8). The ctor sub_12ECE30 zeroes all six and the dtor frees all six independently, and the
// class-info property names fix the slot order - evidence: ops/evidence/battle-objective-state-2026-11-21.md
// sections 3 and 5. Each objective carries BattleObjectiveState as a uint32 at +0x04 (section 2).
const int      kObjSlotBase  = 0x60;    // first Objective* (four-byte stride, six slots)
const int      kObjSlotCount = 6;
const int      kObjStateOff  = 0x04;    // BattleObjective+0x04 = State
const uint32_t kBoardBattleVt = 0x026AEFD8u;   // NEVER 0x026AA294 - that address is the name string

struct ObjSlotRow { uint8_t id; const char* name; };
const ObjSlotRow kObjRows[kObjSlotCount] = {
    { CF_OBJ_CREW,     "ObjectiveKillEnemyCrew" },
    { CF_OBJ_CAPTAIN,  "ObjectiveKillCaptain" },
    { CF_OBJ_SCOUTS,   "ObjectiveKillScouts" },
    { CF_OBJ_OFFICERS, "ObjectiveKillOfficers" },
    { CF_OBJ_FLAG,     "ObjectiveDestroyFlag" },
    { CF_OBJ_POWDER,   "ObjectiveDestroyPowderReserve" },
};

bool IsObjField(uint8_t field) { return field >= CF_OBJ_CREW && field <= CF_OBJ_POWDER; }

int ObjSlotIndex(uint8_t field) {
    for (int i = 0; i < kObjSlotCount; ++i) if (kObjRows[i].id == field) return i;
    return -1;
}

const char* ObjSlotName(uint8_t field) {
    const int i = ObjSlotIndex(field);
    return i >= 0 ? kObjRows[i].name : "?";
}

// A slot that has not been filled yet is normal (the ctor nulls all six); log it once per (parent, field)
// rather than once per 100 ms host walk.
struct NullSlotSeen { void* parent; uint8_t field; bool used; };
NullSlotSeen s_nullSlot[16];

void NullSlotOnce(void* parent, uint8_t field) {
    for (int i = 0; i < 16; ++i)
        if (s_nullSlot[i].used && s_nullSlot[i].parent == parent && s_nullSlot[i].field == field) return;
    for (int i = 0; i < 16; ++i)
        if (!s_nullSlot[i].used) {
            s_nullSlot[i].used = true; s_nullSlot[i].parent = parent; s_nullSlot[i].field = field;
            char line[192];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordS] obj slot=%s NULL parent=%p\n",
                        ObjSlotName(field), parent);
            Phase1_LogLine(line);
            return;
        }
    // Table full: stay quiet rather than spam the log.
}

// The wire key is the parent object's own 64-bit registry id, read through GetObjectID (0x9F0B50), never a
// pointer. Two clients only agree on it if both spawned the same authored boarding battle; if they did not,
// the guest's walk finds nothing and it logs `obj unresolved` once instead of guessing a state.
uint64_t BoardIdentity(void* ent, void* logic) {
    (void)ent;                                            // identified by the parent, not the entity
    if (!logic || !Readable(logic, kObjSlotBase + 4 * kObjSlotCount)) return kCoordKeyNone;
    uint32_t vt = 0;
    uint64_t id = 0;
    __try {
        vt = *(uint32_t*)logic;
        if (vt == kBoardBattleVt) id = CoordSyncObjectId(logic);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return kCoordKeyNone; }
    return vt == kBoardBattleVt ? id : kCoordKeyNone;
}

// `logic` is the GcLBoardingBattle. Slot i -> the BattleObjective*, whose +0x04 dword is the state; a value
// above 2 is not a BattleObjectiveState and is refused (same refusal the ship sail field makes).
bool BoardExtract(void* logic, uint8_t field, float* out) {
    if (!logic || !out) return false;
    const int i = ObjSlotIndex(field);
    if (i < 0) return false;
    if (!Readable((const char*)logic + kObjSlotBase, 4 * kObjSlotCount)) return false;
    bool nullSlot = false;
    __try {
        void* obj = *(void**)((char*)logic + kObjSlotBase + 4 * i);
        if (!obj) { nullSlot = true; }
        else if (Readable((const char*)obj + kObjStateOff, 4)) {
            const uint32_t st = *(uint32_t*)((char*)obj + kObjStateOff);
            if (st <= 2u) { *out = (float)st; return true; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (nullSlot) NullSlotOnce(logic, field);
    return false;
}

// The parent for the message being applied, set by the consumer immediately before sc->apply - the same
// one-shot channel s_applyKey already is for the sail setter.
void* s_applyObj = nullptr;

// Apply is a raw uint32 store at BattleObjective+0x04. There is no setter: the class writes +0x04 itself at
// 0x14AA033 (`= 1`) and 0x14AA0F6 (`= 2`), both hard-coded and guarded (evidence section 4). Whether such a
// store is later overwritten by the monitors is UNPROVEN - that is what the 2 s probe below measures.
// [Coop] BoardObjectiveReplicate (default 0, read once; needs BoardObjectiveSync): evidence naval-boarding-noclass-sink
// Q1d. A slot the guest never activated is not in the battle's active list (+0x174, count u16 +0x17A) and does not
// count toward completion, and a raw 1 skips Start's side effects. So: host >= 1 and the slot missing from the list ->
// sub_12DBDB0(battle, obj) (IDA: __thiscall, retn 4; sub_14A9FE0(battle+0x2E8), sub_14AA010 Start, append to +0x174 -
// no duplicate check, the append grows through the allocator) from OnFrame outside every __try (CoordSyncDeferredFire);
// host 2 with the slot active and at 1 -> the raw 2 (the engine's own final store, 0x14AA0F6). Never a raw 1, never back.
int BoardObjectiveReplicateOn() {
    static int s = -1;
    if (s < 0) s = GetPrivateProfileIntA("Coop", "BoardObjectiveReplicate", 0, Paths::Ini()) != 0 ? 1 : 0;
    return s;
}
struct ObjActQ { void* battle; void* obj; uint8_t field; };
ObjActQ s_objActQ[8]; int s_objActN = 0;
long s_objActQueued = 0, s_objActFired = 0, s_objActSkip = 0, s_objDone = 0;
bool ObjInActiveList(void* battle, void* obj) {   // plain reads; false when unreadable
    if (!Readable((const char*)battle + 0x174, 8)) return false;
    void** arr = *(void***)((char*)battle + 0x174);
    const unsigned n = *(uint16_t*)((char*)battle + 0x17A);
    if (!n) return false;
    if (n > 64 || !Readable(arr, 4 * n)) return false;
    for (unsigned k = 0; k < n; ++k) if (arr[k] == obj) return true;
    return false;
}

bool BoardApply(void* ent, uint8_t field, float v) {
    (void)ent;
    void* parent = s_applyObj;
    if (!parent || !Readable(parent, kObjSlotBase + 4 * kObjSlotCount)) return false;
    const int i = ObjSlotIndex(field);
    if (i < 0 || !(v >= 0.0f) || !(v <= 2.0f)) return false;   // never store a non-BattleObjectiveState
    const uint32_t st = (uint32_t)(v + 0.5f);
    if (BoardObjectiveReplicateOn()) {
        int r = 0;   // 1 = applied, 2 = queued an activation
        __try {
            if (*(uint32_t*)parent != kBoardBattleVt) return false;
            void* obj = *(void**)((char*)parent + kObjSlotBase + 4 * i);
            if (!obj || !Readable((const char*)obj + kObjStateOff, 4)) return false;
            const uint32_t g = *(uint32_t*)((char*)obj + kObjStateOff);
            const bool active = ObjInActiveList(parent, obj);
            if (st >= 1 && !active) {
                bool dup = false; for (int k = 0; k < s_objActN; ++k) if (s_objActQ[k].obj == obj) dup = true;
                if (!dup && s_objActN < 8) { s_objActQ[s_objActN++] = { parent, obj, field }; ++s_objActQueued; }
                r = 2;
            } else if (st == 2 && active && g == 1) {
                *(uint32_t*)((char*)obj + kObjStateOff) = 2; ++s_objDone; r = 1;
            } else r = (g == st) ? 1 : 0;   // nothing to do (never a raw 1, never back to 0)
        } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
        return r != 0;
    }
    int ok = 0;
    __try {
        if (*(uint32_t*)parent != kBoardBattleVt) return false;     // vtable gate, as for the hull
        void* obj = *(void**)((char*)parent + kObjSlotBase + 4 * i);  // re-read the slot now
        if (obj && Readable((const char*)obj + kObjStateOff, 4)) {
            *(uint32_t*)((char*)obj + kObjStateOff) = st;
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok == 1;
}

const uint8_t kShipFields[]  = { CF_HULL, CF_SAIL, CF_HELM, 0 };
const uint8_t kBoardFields[] = { CF_OBJ_CREW, CF_OBJ_CAPTAIN, CF_OBJ_SCOUTS, CF_OBJ_OFFICERS,
                                 CF_OBJ_FLAG, CF_OBJ_POWDER, 0 };

const CoordSchema kSchemas[] = {
    { 0x026A34E8u, "GcLShip", ShipIdentity, ShipExtract, ShipApply, kShipFields },   // census: n = 9
    // Not yet seen in a coordinator census: if GcLBoardingBattle is not a coordinator's GameplayLogic the
    // existing walk will not yield it. The row is registered so it resolves if/when a ref appears; no new
    // enumeration is invented here.
    { kBoardBattleVt, "GcLBoardingBattle", BoardIdentity, BoardExtract, BoardApply, kBoardFields },
};

} // namespace

const CoordSchema* CoordSchemaFor(uint32_t vt) {
    for (const CoordSchema& s : kSchemas) if (s.vt == vt) return &s;
    return nullptr;
}

// [Coop] BoardObjectiveReplicate: the queued activations, from phase1.cpp OnFrame's player-work block (after the
// OnFrameInner __try): validated with plain reads first, then sub_12DBDB0 outside any __try.
void CoordSyncDeferredFire() {
    while (s_objActN > 0) {
        const ObjActQ q = s_objActQ[--s_objActN];
        bool ok = false;
        __try {
            ok = Readable(q.battle, kObjSlotBase + 4 * kObjSlotCount) && *(uint32_t*)q.battle == kBoardBattleVt;
            if (ok) { const int i = ObjSlotIndex(q.field); ok = i >= 0 && *(void**)((char*)q.battle + kObjSlotBase + 4 * i) == q.obj; }
            if (ok) ok = Readable((const char*)q.obj + kObjStateOff, 4) && !ObjInActiveList(q.battle, q.obj);
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        char line[160];
        if (!ok) { ++s_objActSkip; _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordS] obj activate slot=%s skipped (gone / already active)\n", ObjSlotName(q.field)); Phase1_LogLine(line); continue; }
        ((void(__thiscall*)(void*, void*))0x012DBDB0)(q.battle, q.obj);   // activate: bind, Start, append to +0x174
        ++s_objActFired;
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordS] obj activate slot=%s battle=%p obj=%p (queued %ld fired %ld done %ld)\n",
                    ObjSlotName(q.field), q.battle, q.obj, s_objActQueued, s_objActFired, s_objDone);
        Phase1_LogLine(line);
    }
}

// ---------------------------------------------------------------------------------------------
// Telemetry. Counters are reported on the guest's 5 s line, never per message.
static LONG     s_txN = 0;                                   // IC_COORD publishes accepted by the lane
static LONG     s_rxOk = 0, s_rxStale = 0, s_rxUnres = 0, s_rxBad = 0, s_rxExc = 0;
// Battle-objective persistence probe ([Coop] BoardObjectiveSync): classified 2 s after each apply.
// ok = the applied value is still there; stale = changed to something else; revert = back to the old
// value; bad = unreadable / not a BattleObjectiveState; gone = the parent is no longer a live
// GcLBoardingBattle (boarding ended, object released) - reported separately so `bad` keeps its meaning.
static LONG     s_objOk = 0, s_objStale = 0, s_objRevert = 0, s_objBad = 0, s_objGone = 0;
// [Coop] HelmSync: ok = the lever ran and the byte read back as sent; noop = the value was already there so
// the lever was not called (this is what stops a two-sided publish from turning into a notification storm);
// stale = an older sequence than the one already applied for that ship; refused = helm=1 declined by the
// owner check; bad = unreadable object/byte, a non-0/1 value, or a fault.
static LONG     s_helmOk = 0, s_helmNoop = 0, s_helmStale = 0, s_helmRefused = 0, s_helmBad = 0;

// Host change cache: one row per (key, field), last value published. Publishing on change only is what
// keeps the lane quiet - the hull moves in damage-sized steps and the sail state changes rarely.
namespace {
struct TxSlot { uint64_t key; uint8_t field; float last; bool used; };
TxSlot  s_tx[kMaxTxSlots];
struct RxSlot { uint64_t key; uint8_t field; uint32_t lastSeq; uint32_t seenMs; bool used; };
RxSlot  s_rx[kMaxRxSlots];

// [Coop] HelmSync, one row per ship key: the epoch (last sequence applied), the last value this machine
// published for that key (change detection), and whether anything has been published/applied yet.
struct HelmShip {
    uint64_t key;
    uint32_t lastAppliedSeq;
    uint8_t  lastPubValue;
    bool     haveApplied;
    bool     havePub;
    bool     used;
};
const int kMaxHelmShips = 16;
HelmShip  s_helm[kMaxHelmShips];

// 2 s persistence probe, one row per in-flight apply. Eight is plenty: objectives change on boarding
// events, not per frame.
const int kMaxProbes = 8;
struct ObjProbe {
    uint64_t key; uint8_t field; uint32_t before; uint32_t applied; uint32_t after;
    void* parent; DWORD dueMs; bool used;
};
ObjProbe s_probe[kMaxProbes];
int      s_probeN = 0;
}

bool BoardObjectiveSyncOn() {
    static int s_ini = -1;   // the s_x = -1 idiom: read once
    if (s_ini < 0) s_ini = GetPrivateProfileIntA("Coop", "BoardObjectiveSync", 0, Paths::Ini()) != 0 ? 1 : 0;
    return s_ini == 1;
}

bool CoordSyncOn() {
    static int s_ini = -1;   // the s_x = -1 idiom: read once, never re-stat the ini per frame
    if (s_ini < 0) s_ini = GetPrivateProfileIntA("Coop", "CoordSync", 0, Paths::Ini()) != 0 ? 1 : 0;
    return s_ini == 1 && Net::IntentLaneOn();
}

namespace {

// [Coop] HelmSync, defined further down: declared here because the host walk (HostTick) also publishes the
// helm byte of ships it sees as proxies.
void HelmPublishChanged(uint64_t key, uint8_t v, bool holderIsLocal);
void HelmSampleLocal();
void HelmReportTick();
void HelmApplyMessage(uint64_t key, uint32_t seq, uint8_t holderIsLocal, float v);

// Publish one field if it differs from the last value published for (key, field).
void PublishField(const CoordSchema* sc, uint64_t key, uint8_t field, float v) {
    TxSlot* slot = nullptr;
    for (int i = 0; i < kMaxTxSlots; ++i)
        if (s_tx[i].used && s_tx[i].key == key && s_tx[i].field == field) { slot = &s_tx[i]; break; }
    if (!slot) for (int i = 0; i < kMaxTxSlots; ++i) if (!s_tx[i].used) { slot = &s_tx[i]; break; }
    if (!slot) return;
    if (slot->used && slot->last == v) return;               // edge only
    const IntentCoordValue p = { field, { 0, 0, 0 }, v };
    const bool ok = Net::PublishIntent(0, key, IC_COORD, (uint32_t)GetTickCount(), &p, (uint8_t)sizeof(p));
    slot->used = true; slot->key = key; slot->field = field; slot->last = v;
    if (ok) ++s_txN;
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordS] tx vt=%08X key=%016llX f=%u v=%.2f\n",
                sc ? sc->vt : 0u, (unsigned long long)key, (unsigned)field, v);
    Phase1_LogLine(line);
}

// Host: walk the coordinators, and for every entry whose class has a schema and whose identity is known
// (i.e. not kCoordKeyNone - the local ship IS key 0), publish fields 1 and 2 on change.
void HostTick() {
    static DWORD s_next = 0;
    const DWORD now = GetTickCount();
    if (s_next && now - s_next < (DWORD)kHostWalkMs) return;
    s_next = now;
    static CoordRef refs[kMaxCollect];
    int n = 0;
    __try { n = CoordCollect(refs, kMaxCollect); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordS] EXCEPTION 0x%08X (collect aborted)\n", GetExceptionCode());
        Phase1_LogLine(line);
        return;
    }
    for (int i = 0; i < n; ++i) {
        const CoordRef& c = refs[i];
        const CoordSchema* sc = CoordSchemaFor(c.vt);
        if (!sc) continue;
        const uint64_t key = sc->identity(c.ent, c.logic);
        if (key == kCoordKeyNone) continue;
        for (const uint8_t* f = sc->fields; f && *f; ++f) {
            if (*f == CF_HELM) continue;   // [Coop] HelmSync has its own path (below): per-ship epoch + noop
            if (IsObjField(*f) && !BoardObjectiveSyncOn()) continue;   // flag 0: not a single extra read
            float v = 0.0f;
            if (sc->extract(c.logic, *f, &v)) PublishField(sc, key, *f, v);
        }
        // [Coop] HelmSync: sample the byte for every ship in the walk that is not this machine's own (key 0 is
        // sampled every frame by HelmSampleLocal). State-driven, so a change the engine made on its own path is
        // published too - not only one caused by our key press.
        if (HelmSyncOn() && key != kCoordKeyLocal) {
            uint8_t hv = 0;
            if (HelmRead(c.logic, &hv)) HelmPublishChanged(key, hv, false);
        }
    }
}

// Guest: find the object a wire key names, using the same read-only walk the host uses (CoordCollect is
// the existing receipted enumerator - no new mechanism). The schema's identity decides the match, so a
// class keyed by its parent's registry id resolves through the parent, not the entity.
void* ResolveByKey(uint64_t key, void** logicOut) {
    if (logicOut) *logicOut = nullptr;
    if (!key || key == kCoordKeyLocal) return nullptr;
    static CoordRef refs[kMaxCollect];
    int n = 0;
    __try { n = CoordCollect(refs, kMaxCollect); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    for (int i = 0; i < n; ++i) {
        const CoordSchema* sc = CoordSchemaFor(refs[i].vt);
        if (!sc) continue;
        if (sc->identity(refs[i].ent, refs[i].logic) == key) {
            if (logicOut) *logicOut = refs[i].logic;
            return refs[i].ent;
        }
    }
    return nullptr;
}

// `obj unresolved` once per key, not once per message: the host republishes on change only, but a guest
// that never spawned the same boarding battle must not fill the log with the same line.
struct UnresKey { uint64_t key; bool used; };
UnresKey s_unres[8];

void LogUnresolvedOnce(uint64_t key, uint8_t field) {
    for (int i = 0; i < 8; ++i) if (s_unres[i].used && s_unres[i].key == key) return;
    bool have = false;
    for (int i = 0; i < 8; ++i) if (!s_unres[i].used) { s_unres[i].used = true; s_unres[i].key = key; have = true; break; }
    if (!have) return;
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordS] obj unresolved key=%016llX slot=%s\n",
                (unsigned long long)key, ObjSlotName(field));
    Phase1_LogLine(line);
}

// ---- [Coop] HelmSync: publish, epoch, apply ---------------------------------------------------------
// Why this one field carries an epoch when hull and sail do not: the engine arbitrates helm changes by
// IDENTITY - 0x01228352 loads ship+0xA4, compares it with the message sender, and 0x01228354 jnz parks a
// foreign request at +0x963 instead of applying it (ops/evidence/helm-state-2026-11-21.md Q4). Calling the
// lever directly BYPASSES that guard, so nothing on the engine side orders two calls and nothing on the
// engine side has any notion of time. The rule is therefore ours, and it is stated here once:
//
//     LAST WRITER WINS BY SEQUENCE; ON AN EQUAL SEQUENCE THE LOWER PEER ID WINS.
//
// The sequence is the lane's own per-(guard, channel) seq (net.h: "strictly increasing in delivery order"),
// not a counter of ours, so a re-delivered message is recognisable. "Lower peer id" is evaluated as
// Phase1_IsEntityHost() (the host is the lower slot) and only on the equal-sequence tie, which a correct lane
// never produces - it is there so the rule is total rather than undefined.
//
// holderIsLocal (IntentCoordValue::rsv[0], unused by every other field): 1 = the publishing machine natively
// owns the object this value describes (the local ship, key 0); 0 = the ship is the peer's, seen through a
// ShipMirror proxy. It is NOT a "which player is at the wheel" flag - the engine has no such field (Q2) - and
// it is carried for the log and for the record, not trusted as proof of authority.
HelmShip* HelmSlot(uint64_t key, bool create) {
    for (int i = 0; i < kMaxHelmShips; ++i)
        if (s_helm[i].used && s_helm[i].key == key) return &s_helm[i];
    if (!create) return nullptr;
    for (int i = 0; i < kMaxHelmShips; ++i)
        if (!s_helm[i].used) {
            s_helm[i].used = true;
            s_helm[i].key = key;
            s_helm[i].lastAppliedSeq = 0;
            s_helm[i].lastPubValue = 0;
            s_helm[i].haveApplied = false;
            s_helm[i].havePub = false;
            return &s_helm[i];
        }
    return nullptr;
}

void HelmLog(uint64_t key, bool holderIsLocal, uint8_t v, uint32_t seq,
             uint8_t before, uint8_t after, const char* via) {
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] helm key=%016llX holder=%s v=%u seq=%u before=%u after=%u via=%s\n",
                (unsigned long long)key, holderIsLocal ? "local" : "remote",
                (unsigned)v, seq, (unsigned)before, (unsigned)after, via);
    Phase1_LogLine(line);
}

// Publish only on a change of the byte as this machine sees it. This is deliberately state-driven and not
// event-driven: a helm change made by the engine's own key path (or by the lever firing from a script) must
// reach the peer too, so the byte is sampled rather than hooked.
void HelmPublishChanged(uint64_t key, uint8_t v, bool holderIsLocal) {
    HelmShip* s = HelmSlot(key, true);
    if (!s) return;
    if (s->havePub && s->lastPubValue == v) return;          // edge only
    IntentCoordValue p;
    memset(&p, 0, sizeof(p));
    p.field    = CF_HELM;
    p.rsv[0]   = holderIsLocal ? 1 : 0;
    p.value    = (float)v;
    // hostMs is carried through untouched by the lane (net.h), so GetTickCount is a stamp, not the ordering.
    Net::PublishIntent(0, key, IC_COORD, (uint32_t)GetTickCount(), &p, (uint8_t)sizeof(p));
    const uint8_t before = s->havePub ? s->lastPubValue : v;
    s->havePub = true;
    s->lastPubValue = v;
    // seq 0 / via=tx: the lane assigns the real sequence at publish time and does not report it back.
    HelmLog(key, holderIsLocal, v, 0, before, v, "tx");
}

// Every ship this machine natively drives, each frame: one vtable-gated byte read. Two pointer lookups and a
// byte, so this is safe to run at frame rate on both host and guest (the coord walk is the only expensive
// part of the lane and it stays at 10 Hz).
void HelmSampleLocal() {
    void* ent = CoordSyncLocalShipEntity();
    if (!ent) return;
    void* gcl = CoordSyncEntityLogic(ent);
    uint8_t v = 0;
    if (!HelmRead(gcl, &v)) return;
    HelmPublishChanged(kCoordKeyLocal, v, true);
}

// The two evaluable halves of the engine's identity guard, since the guard itself is bypassed here:
//   * a mirror proxy is ours to drive only if the key IS that proxy's host id (no guessing a stranger's ship);
//   * the local ship is granted only while ship+0xA4 - the controller entity the engine hands helm changes to -
//     still names something. With +0xA4 null the engine would have parked even its own command, so a grant
//     here would be inventing an owner. The peer's own identity is not on the wire (IC_COORD is 8 bytes and
//     carries no peer id), so no stronger check is claimed.
bool HelmOwnerKnown(void* ent, void* logic, uint64_t key) {
    if (key != kCoordKeyLocal) return CoordSyncProxyHostId(ent) == key;
    if (!logic || !Readable((const char*)logic + 0xA4, 4)) return false;
    __try { return *(void**)((char*)logic + 0xA4) != nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// `helm refused` once per key: a guest permanently without a controller would otherwise log it on every
// republish. Same shape as UnresKey above.
struct HelmRefusedKey { uint64_t key; bool used; };
HelmRefusedKey s_helmRefusedKey[8];

void HelmRefusedOnce(uint64_t key) {
    for (int i = 0; i < 8; ++i) if (s_helmRefusedKey[i].used && s_helmRefusedKey[i].key == key) return;
    for (int i = 0; i < 8; ++i)
        if (!s_helmRefusedKey[i].used) {
            s_helmRefusedKey[i].used = true;
            s_helmRefusedKey[i].key = key;
            char line[128];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[CoordS] helm refused key=%016llX reason=no-owner\n", (unsigned long long)key);
            Phase1_LogLine(line);
            return;
        }
}

// One IC_COORD message carrying CF_HELM, on either side. The generic apply path is not used: this field needs
// the per-ship epoch, the noop rule and the owner check, none of which belong to a (field, float) store.
void HelmApplyMessage(uint64_t key, uint32_t seq, uint8_t holderIsLocal, float v) {
    if (!(v == 0.0f || v == 1.0f)) {                        // not a helm value
        ++s_helmBad;
        HelmLog(key, holderIsLocal != 0, 0, seq, 0, 0, "bad");
        return;
    }
    const uint8_t want = (v >= 0.5f) ? 1 : 0;
    void* ent = (key == kCoordKeyLocal) ? CoordSyncLocalShipEntity() : CoordSyncProxyEntity(key);
    if (!ent) { ++s_helmBad; HelmLog(key, holderIsLocal != 0, want, seq, 0, 0, "bad"); return; }
    void* logic = CoordSyncEntityLogic(ent);
    uint8_t before = 0;
    if (!HelmRead(logic, &before)) { ++s_helmBad; HelmLog(key, holderIsLocal != 0, want, seq, 0, 0, "bad"); return; }
    HelmShip* s = HelmSlot(key, true);
    if (!s) { ++s_helmBad; return; }

    const bool newer  = !s->haveApplied || (int32_t)(seq - s->lastAppliedSeq) > 0;
    const bool tieHost = s->haveApplied && seq == s->lastAppliedSeq && Phase1_IsEntityHost();
    if (!newer && !tieHost) {                               // older (or equal, and we are not the host)
        ++s_helmStale;
        HelmLog(key, holderIsLocal != 0, want, seq, before, before, "stale");
        return;
    }
    if (before == want) {                                   // already there: never fire the notifications twice
        s->haveApplied = true;
        s->lastAppliedSeq = seq;
        s->havePub = true;
        s->lastPubValue = want;                             // and do not echo it back out as a new change
        ++s_helmNoop;
        HelmLog(key, holderIsLocal != 0, want, seq, before, before, "noop");
        return;
    }
    // Granting: only for a ship we can prove is ours to drive. Releasing is always allowed - a revoke can
    // never hand control to anyone.
    if (want == 1 && !HelmOwnerKnown(ent, logic, key)) {
        ++s_helmRefused;
        HelmRefusedOnce(key);
        HelmLog(key, holderIsLocal != 0, want, seq, before, before, "refused");
        return;
    }

    int ok = 0;
    __try { ok = ShipApplyHelm(ent, (float)want) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; ++s_helmBad; }
    uint8_t after = before;
    if (!HelmRead(logic, &after)) after = before;
    if (ok) {
        s->haveApplied = true;
        s->lastAppliedSeq = seq;
        s->havePub = true;
        s->lastPubValue = after;
        ++s_helmOk;
    } else if (after != want) {
        ++s_helmBad;
    }
    HelmLog(key, holderIsLocal != 0, want, seq, before, after, "lever");
}

// The helm 5 s line, on BOTH sides: the guest's ReportTick already runs only on the guest, and the host
// applies helm too, so this owns its own timer.
void HelmReportTick() {
    static DWORD s_next = 0;
    const DWORD now = GetTickCount();
    if (s_next && now - s_next < (DWORD)kReportMs) return;
    s_next = now;
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] helm stats ok=%ld noop=%ld stale=%ld refused=%ld bad=%ld\n",
                s_helmOk, s_helmNoop, s_helmStale, s_helmRefused, s_helmBad);
    Phase1_LogLine(line);
}

// Arm the 2 s re-read for one applied objective. `before`/`applied`/`after` are the values the log needs.
void ProbeRecord(uint64_t key, uint8_t field, void* parent,
                 uint32_t before, uint32_t applied, uint32_t after) {
    ObjProbe* slot = nullptr;
    for (int i = 0; i < kMaxProbes; ++i)
        if (s_probe[i].used && s_probe[i].key == key && s_probe[i].field == field) { slot = &s_probe[i]; break; }
    if (!slot) for (int i = 0; i < kMaxProbes; ++i) if (!s_probe[i].used) { slot = &s_probe[i]; break; }
    if (!slot) {                                     // full: drop the row that is due soonest
        slot = &s_probe[0];
        for (int i = 1; i < kMaxProbes; ++i)
            if ((int32_t)(s_probe[i].dueMs - slot->dueMs) < 0) slot = &s_probe[i];
    }
    if (!slot->used) ++s_probeN;
    slot->used = true;
    slot->key = key; slot->field = field;
    slot->before = before; slot->applied = applied; slot->after = after;
    slot->parent = parent;
    slot->dueMs = GetTickCount() + 2000;             // the hull lane taught us one frame proves nothing
}

// Guest, per frame: the due 2 s re-reads. With no rows this is one loop over eight flags.
void ObjProbeTick() {
    if (!s_probeN) return;
    const DWORD now = GetTickCount();
    for (int i = 0; i < kMaxProbes; ++i) {
        ObjProbe& p = s_probe[i];
        if (!p.used || (int32_t)(now - p.dueMs) < 0) continue;
        p.used = false;
        --s_probeN;
        uint32_t after2s = 0;
        bool have = false, gone = false, bad = false;
        if (!p.parent || !Readable(p.parent, kObjSlotBase + 4 * kObjSlotCount)) gone = true;
        else __try {
            if (*(uint32_t*)p.parent != kBoardBattleVt) { gone = true; }
            else {
                void* obj = *(void**)((char*)p.parent + kObjSlotBase + 4 * ObjSlotIndex(p.field));
                if (!obj || !Readable((const char*)obj + kObjStateOff, 4)) gone = true;
                else {
                    const uint32_t st = *(uint32_t*)((char*)obj + kObjStateOff);
                    if (st <= 2u) { after2s = st; have = true; } else { bad = true; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { gone = true; have = false; }

        if (have) {
            if (after2s == p.applied)                                 ++s_objOk;
            else if (after2s == p.before && p.before != p.applied)    ++s_objRevert;
            else                                                      ++s_objStale;
        } else if (bad)  ++s_objBad;
        else             ++s_objGone;

        char line[224];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CoordS] obj slot=%s key=%016llX before=%u applied=%u after=%u after2s=%u\n",
                    ObjSlotName(p.field), (unsigned long long)p.key,
                    p.before, p.applied, p.after, have ? after2s : 0xFFFFFFFFu);
        Phase1_LogLine(line);
    }
}

// Guest: the 5 s counter line. The applies themselves are CoordSyncApplyMsg (the lane's single consumer).
void ReportTick() {
    static DWORD s_next = 0;
    const DWORD now = GetTickCount();
    if (s_next && now - s_next < kReportMs) return;
    s_next = now;
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] rx stats ok=%ld stale=%ld unres=%ld bad=%ld exc=%ld hexc=%ld\n",
                s_rxOk, s_rxStale, s_rxUnres, s_rxBad, s_rxExc, s_hullExc);
    Phase1_LogLine(line);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] obj stats ok=%ld stale=%ld revert=%ld bad=%ld gone=%ld\n",
                s_objOk, s_objStale, s_objRevert, s_objBad, s_objGone);
    Phase1_LogLine(line);
}

} // namespace

void CoordSyncTick() {
    const bool helm = HelmSyncOn();   // read once, before any branch: flag 0 = no sampling, no publish, no apply
    if (CoordSyncOn()) {
        if (Phase1_IsEntityHost()) HostTick();        // 10 Hz; also publishes proxy helm state it walks past
        else { ObjProbeTick(); ReportTick(); }
    }
    if (helm) {
        HelmSampleLocal();            // key 0, every frame, on both sides - the state IS the byte
        HelmReportTick();
    }
}

// One IC_COORD message: resolve the object by wire key, check per-(key, field) freshness, apply through
// the schema, report the transition. A refused or faulting apply costs this message, never the frame.
void CoordSyncApplyMsg(uint64_t wireId, uint32_t seq, const uint8_t* payload, uint8_t len) {
    const bool helmOn = HelmSyncOn();
    if (!CoordSyncOn() && !helmOn) return;
    if (len != sizeof(IntentCoordValue) || !payload) { ++s_rxBad; return; }
    IntentCoordValue p;
    memcpy(&p, payload, sizeof(p));
    if (p.field == CF_HELM) {      // [Coop] HelmSync: the one field with its own per-ship epoch (see above)
        if (!helmOn) { ++s_rxBad; return; }
        HelmApplyMessage(wireId, seq, p.rsv[0], p.value);
        return;
    }
    if (!CoordSyncOn()) return;    // every field below belongs to [Coop] CoordSync / BoardObjectiveSync
    const bool objField = IsObjField(p.field);
    if (!objField && p.field != CF_HULL && p.field != CF_SAIL) { ++s_rxBad; return; }
    if (objField && !BoardObjectiveSyncOn()) { ++s_rxBad; return; }

    // Key 0 is the local ship on both sides; anything else is a ship this machine renders as a proxy.
    // An objective message names the parent GcLBoardingBattle by its registry id, so when no ship proxy
    // owns the key the read-only coordinator walk is asked instead (ResolveByKey); if that finds nothing
    // the guest reports `unresolved` once rather than inventing a state.
    s_applyObj = nullptr;
    void* ent = nullptr;
    void* logic = nullptr;
    if (wireId == kCoordKeyLocal) {
        ent = CoordSyncLocalShipEntity();
        if (ent) logic = CoordSyncEntityLogic(ent);
    } else {
        ent = CoordSyncProxyEntity(wireId);
        if (ent) logic = CoordSyncEntityLogic(ent);
        else if (objField) ent = ResolveByKey(wireId, &logic);
    }
    if (!ent) { ++s_rxUnres; if (objField) LogUnresolvedOnce(wireId, p.field); return; }
    if (!logic || !Readable(logic, 4)) { ++s_rxUnres; if (objField) LogUnresolvedOnce(wireId, p.field); return; }
    if (objField) s_applyObj = logic;

    uint32_t vt = 0;
    __try { vt = *(uint32_t*)logic; } __except (EXCEPTION_EXECUTE_HANDLER) { ++s_rxExc; return; }
    const CoordSchema* sc = CoordSchemaFor(vt);        // the resolved object must be a class we know
    if (!sc) { ++s_rxBad; return; }

    // Freshness per (key, field): the lane orders per (guard, channel), and a field is its own key here.
    const DWORD now = GetTickCount();
    int slot = -1;
    for (int i = 0; i < kMaxRxSlots; ++i)
        if (s_rx[i].used && s_rx[i].key == wireId && s_rx[i].field == p.field) { slot = i; break; }
    const bool sameKey = slot >= 0;
    if (slot < 0) {
        slot = -1;
        for (int i = 0; i < kMaxRxSlots; ++i) if (!s_rx[i].used) { slot = i; break; }
        if (slot < 0) {                                   // full: evict the least recently seen pair
            slot = 0;
            for (int i = 1; i < kMaxRxSlots; ++i) if (s_rx[i].seenMs < s_rx[slot].seenMs) slot = i;
        }
    }
    if (sameKey && seq <= s_rx[slot].lastSeq) { ++s_rxStale; return; }

    float before = 0.0f, after = 0.0f;
    const bool haveBefore = sc->extract(logic, p.field, &before);
    s_applyKey = wireId;
    int ok = 0;
    __try { ok = sc->apply(ent, p.field, p.value) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ++s_rxExc;
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CoordS] EXCEPTION 0x%08X key=%016llX f=%u (apply)\n",
                    GetExceptionCode(), (unsigned long long)wireId, (unsigned)p.field);
        Phase1_LogLine(line);
        return;
    }
    if (ok) {
        s_rx[slot].used = true; s_rx[slot].key = wireId; s_rx[slot].field = p.field;
        s_rx[slot].lastSeq = seq; s_rx[slot].seenMs = now;
        ++s_rxOk;
    }
    const bool haveAfter = sc->extract(logic, p.field, &after);   // read back; a failed read leaves 0
    // Arm the 2 s persistence re-read. `applied` is the value this message actually stored, `before` and
    // `after` the values around it - the log line the oracle reads.
    if (ok && objField && s_applyObj)
        ProbeRecord(wireId, p.field, s_applyObj, haveBefore ? (uint32_t)before : 0xFFFFFFFFu,
                    (uint32_t)(p.value + 0.5f), haveAfter ? (uint32_t)after : 0xFFFFFFFFu);
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] rx vt=%08X key=%016llX f=%u %.2f->%.2f ok=%d\n",
                vt, (unsigned long long)wireId, (unsigned)p.field,
                haveBefore ? before : 0.0f, after, ok);
    Phase1_LogLine(line);
}

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
uint64_t CoordSyncProxyHostId(const void* ent);      // live ShipMirror proxy -> its hostId, else 0
void*    CoordSyncProxyEntity(uint64_t hostId);      // live ShipMirror slot for a hostId, else nullptr

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

// ---- schema callbacks for GcLShip ---------------------------------------------------------------
// Identity, exactly as the table contract states: the local ship is key 0 on both machines; a ShipMirror
// proxy is identified by the host's deep-copied entity id; everything else is not synced. Both lookups are
// pointer comparisons inside phase1.cpp (no dereference of the candidate), so no SEH is needed here.
uint64_t ShipIdentity(void* ent) {
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

bool ShipApply(void* ent, uint8_t field, float v) {
    if (!ent) return false;
    if (field == CF_HULL) return ShipApplyHull(ent, v);
    if (field == CF_SAIL) {
        if (!(v >= 0.0f) || !(v < 4.0f)) return false;   // never hand the setter a non-SailState
        return CoordSyncSailApply(ent, (unsigned)v, s_applyKey) == 1;
    }
    return false;
}

const CoordSchema kSchemas[] = {
    { 0x026A34E8u, "GcLShip", ShipIdentity, ShipExtract, ShipApply },   // coord_enum.cpp census: n = 9
};

} // namespace

const CoordSchema* CoordSchemaFor(uint32_t vt) {
    for (const CoordSchema& s : kSchemas) if (s.vt == vt) return &s;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Telemetry. Counters are reported on the guest's 5 s line, never per message.
static LONG     s_txN = 0;                                   // IC_COORD publishes accepted by the lane
static LONG     s_rxOk = 0, s_rxStale = 0, s_rxUnres = 0, s_rxBad = 0, s_rxExc = 0;

// Host change cache: one row per (key, field), last value published. Publishing on change only is what
// keeps the lane quiet - the hull moves in damage-sized steps and the sail state changes rarely.
namespace {
struct TxSlot { uint64_t key; uint8_t field; float last; bool used; };
TxSlot  s_tx[kMaxTxSlots];
struct RxSlot { uint64_t key; uint8_t field; uint32_t lastSeq; uint32_t seenMs; bool used; };
RxSlot  s_rx[kMaxRxSlots];
}

bool CoordSyncOn() {
    static int s_ini = -1;   // the s_x = -1 idiom: read once, never re-stat the ini per frame
    if (s_ini < 0) s_ini = GetPrivateProfileIntA("Coop", "CoordSync", 0, Paths::Ini()) != 0 ? 1 : 0;
    return s_ini == 1 && Net::IntentLaneOn();
}

namespace {

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
        const uint64_t key = sc->identity(c.ent);
        if (key == kCoordKeyNone) continue;
        float v = 0.0f;
        if (sc->extract(c.logic, CF_HULL, &v)) PublishField(sc, key, CF_HULL, v);
        if (sc->extract(c.logic, CF_SAIL, &v)) PublishField(sc, key, CF_SAIL, v);
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
}

} // namespace

void CoordSyncTick() {
    if (!CoordSyncOn()) return;
    if (Phase1_IsEntityHost()) { HostTick(); return; }
    ReportTick();
}

// One IC_COORD message: resolve the object by wire key, check per-(key, field) freshness, apply through
// the schema, report the transition. A refused or faulting apply costs this message, never the frame.
void CoordSyncApplyMsg(uint64_t wireId, uint32_t seq, const uint8_t* payload, uint8_t len) {
    if (!CoordSyncOn()) return;
    if (len != sizeof(IntentCoordValue) || !payload) { ++s_rxBad; return; }
    IntentCoordValue p;
    memcpy(&p, payload, sizeof(p));
    if (p.field != CF_HULL && p.field != CF_SAIL) { ++s_rxBad; return; }

    // Key 0 is the local ship on both sides; anything else is a ship this machine renders as a proxy.
    void* ent = (wireId == kCoordKeyLocal) ? CoordSyncLocalShipEntity() : CoordSyncProxyEntity(wireId);
    if (!ent) { ++s_rxUnres; return; }
    void* logic = CoordSyncEntityLogic(ent);
    if (!logic || !Readable(logic, 4)) { ++s_rxUnres; return; }

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
    sc->extract(logic, p.field, &after);                 // read back for the log; a failed read leaves 0
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordS] rx vt=%08X key=%016llX f=%u %.2f->%.2f ok=%d\n",
                vt, (unsigned long long)wireId, (unsigned)p.field,
                haveBefore ? before : 0.0f, after, ok);
    Phase1_LogLine(line);
}

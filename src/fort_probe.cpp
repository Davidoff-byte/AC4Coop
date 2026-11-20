// [Coop] FortProbe (default 0, read at first tick; observation only, no writes of any kind). Fort sync needs to know
// which forts and towers exist on each machine and what state they are in.
//
// A fort is NOT a component - it is reached through a GameplayCoordinator component - which is why the earlier
// GetComponentByType(0x02A18520) test measured zero forts on both machines while the walk examined ~4k entities.
// Static RE (ops/evidence/fort-enumeration-2-2026-11-20.md) gives the engine's own predicate, and this probe now
// calls it instead of guessing at a component type:
//
//   0x011D9380  (Entity* entity, char flag)  cdecl  -> fort or null            units.json "011D9380"
//     flag==0 path: sub_525590(entity, key = *(void**)0x027AF364 /* GameplayCoordinatorComponent */, &arr)
//                   equal_range over entity+0x60 / count +0x66
//                   per candidate: v9 = (comp+0xC)->vtbl[3](comp+0xC); fort = *(v9+0x64); sub_11C15B0(fort)
//
// Several entities can resolve to the same fort (the coordinator's candidate list is per entity), so results are
// de-duplicated by pointer before anything is counted or logged.
//
// Field map (given by reflection; NOT re-derived here, and every read below is guarded so a wrong offset can only
// produce a rejected line, never a write or an aborted frame). These are offsets on the GcLNavalFort object the
// predicate returns:
//   GcLNavalFort        classinfo 0x02A18520  size 0x3D8
//     +0x118 SmallArray<Handle> TowersAlive        +0xC0 SmallArray<Handle> DestroyedFortEntities
//     +0x74  U32 FortPhaseCounter                  +0x2F8 Bool HasEnteredCombat
//     +0x120 BaseObject CurrentFortPhase           +0x64 SmallArray<Handle> TowerSetup
//   GcLNavalTowerACGA   classinfo 0x02A020C8  size 0x4F0   <- the RUNTIME tower class
//     +0x16C Float CurrentHealth                   +0x2E3 Bool IsFortTowerDisabled   (ACGA offsets)
//     +0xB8  Handle Fort (back-pointer; documented on GcLNavalTower - UNVERIFIED whether ACGA carries it, not read)
//   FortTowerManager    classinfo 0x02A911C0  size 0xF0 base Component - a real Component, so that test stays
//     +0x2C SmallArray<ObjectPtr> RuntimeTowers (ACGA)   +0x81 Bool PlayerShipInFortRange
//
// TowersAlive is a SmallArray<Handle>. A Handle is a 4-byte wrapper around a HandleContainer*
// (src/Scimitar/BaseTypes.h:48), and HandleContainer { m_Object, softCount, hardCount, id } has no vtable
// (BaseTypes.h:26) - so a slot resolves in two hops, exactly as the entity walk resolves a Reference:
// slot (HandleContainer*) -> m_Object (the tower). Read as the object itself (the old shape) a slot pointed at
// the container, not the tower; both shapes are accepted below, the container hop first.
//
// Enumeration: the engine's own entity manager, world->m_EntityManager.m_Entities (World.h:228), walked exactly as
// src/phase1.cpp:3489-3503 does. The earlier world+0x80 / +0x84 handle array came from an external digest, was not
// verified in this binary, and measured zero entities on both machines (2026-11-20 live run, three probed strides),
// so it is gone along with the stride probe - this array has no stride to guess. Per-entity order is
// Reference -> HandleContainer -> m_Object (Entity), with phase1's break (reference slot unreadable: stop) vs
// continue (handle or object slot bad: skip) semantics. An entity is examined once its Entity pointer is readable;
// the fort count is the number of distinct fort objects 0x011D9380 returns over that walk, and the towerMgrs count
// is the number of entities carrying a FortTowerManager, i.e. still a GetComponentByType result.
//
// Rates: one shot per second whatever the frame rate. Detail lines (fort/tower) and the towerMgrs count are printed
// once every 10 s, capped at 8 forts x 8 towers, so a naval map cannot flood the log; the summary line carries the
// true counts and repeats every shot.
//
// Census: once per run (after the first walk with a usable entity manager) every component's vtable is counted and
// the top 40 are logged as [FortC] lines, plus an explicit n for the three fort vtables (0x026FD400 GcLNavalFort,
// 0x026F7AC0 GcLNavalTowerACGA, 0x02722048 FortTowerManager). It is now a CONTROL: with forts reached through
// 0x011D9380 the fort vtable is expected to be absent from the component list, which explains the earlier zero and
// is worth seeing in the same log. A nonzero fort vtable would mean a fort is also a live component.
#include "fort_probe.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>

void Phase1_LogLine(const char* s);

namespace {

const uintptr_t VA_TOWERMGR_CLASSINFO = 0x02A911C0;   // FortTowerManager class descriptor (rebased at use)
const uintptr_t VA_TOWER_ACGA_VTABLE  = 0x026F7AC0;   // GcLNavalTowerACGA vtable (compared only, never called)

const uint32_t kMaxEntries   = 4096;   // clamp on the entity-manager count per shot
const int      kMaxFortsLog  = 8;      // forts detailed per shot
const int      kMaxTowersLog = 8;      // tower lines per fort
const int      kMaxUniqueForts = 128;  // distinct fort pointers remembered for de-duplication
const size_t   kFortBytes    = 0x3D8;  // GcLNavalFort size: the readable floor before any field is trusted
const size_t   kTowerBytes   = 0x2E8;  // covers +0x16C (float) and +0x2E3 (bool)

bool Readable(const void* p, size_t n)
{
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

// A candidate object: plausible heap range, aligned, and its vtable dword readable too. A soft/hard ref count read
// as a pointer fails this, which is what keeps the towersAlive slots honest.
bool LooksLikeObject(uintptr_t p)
{
    if (p < 0x10000 || p >= 0x7F000000 || (p & 3)) return false;
    if (!Readable((const void*)p, 4)) return false;
    uintptr_t vt = 0;
    __try { vt = *(const uintptr_t*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return vt >= 0x10000 && vt < 0x7F000000 && Readable((const void*)vt, 4);
}

// One guarded pointer read: a slot that is freed between the guard and the read abandons the slot, not the shot.
bool ReadPtr(uintptr_t at, uintptr_t* out)
{
    *out = 0;
    __try { *out = *(const uintptr_t*)at; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SmallArray<Handle> slot -> the tower object it refers to. A Handle/Reference is a 4-byte wrapper around a
// HandleContainer* (BaseTypes.h:48) and HandleContainer { m_Object, softCount, hardCount, id } has no vtable
// (BaseTypes.h:26), so the slot IS a HandleContainer and the tower is its m_Object. A slot that is the object
// itself (bare ObjectPtr array) is accepted too: for it the "hop" reads the vtable pointer, which is not a tower
// vtable, so the container branch never claims it. Returns 0 when nothing behind the slot is a tower object.
uintptr_t ResolveTowerSlot(uintptr_t slot, uintptr_t towerVt)
{
    if (!LooksLikeObject(slot)) return 0;

    uintptr_t slotVt = 0;
    __try { slotVt = *(const uintptr_t*)slot; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    uintptr_t obj = 0, objVt = 0;
    __try { obj = (uintptr_t)((const HandleContainer*)slot)->m_Object; }
    __except (EXCEPTION_EXECUTE_HANDLER) { obj = 0; }
    if (obj && obj != slotVt && LooksLikeObject(obj)) {
        __try { objVt = *(const uintptr_t*)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { objVt = 0; }
        if (objVt == towerVt) return obj;                  // container hop: the slot is a HandleContainer
    }
    if (towerVt && slotVt == towerVt) return slot;         // the slot is the ACGA object itself
    return 0;
}

// The engine's predicate maps several entities onto one fort object, so a fort is counted and logged once.
bool FortSeen(uintptr_t* seen, int* n, bool* full, uintptr_t fort)
{
    for (int i = 0; i < *n; ++i)
        if (seen[i] == fort) return true;
    if (*n < kMaxUniqueForts) { seen[(*n)++] = fort; return false; }
    *full = true;                                          // table exhausted: counted, de-dup not guaranteed
    return false;
}

// SmallArray = {begin, end, ...}. The element size of SmallArray<Handle> is not established in this binary, so the
// raw byte span is returned; the caller divides by 4 (the size this probe was specified with) and the span is
// logged whenever (end-begin)%4 != 0, so a real element size of 8 / 0xC / 0x10 shows up in the log.
bool SmallArraySpan(uintptr_t arr, uintptr_t* begin, uint32_t* span)
{
    *begin = 0;
    *span = 0;
    if (!Readable((const void*)arr, 8)) return false;
    uintptr_t b = 0, e = 0;
    __try { b = *(const uintptr_t*)arr; e = *(const uintptr_t*)(arr + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!b && !e) return true;
    if (!b || e <= b) return false;
    const uintptr_t d = e - b;
    if (d > 0x4000) return false;                      // 4096 x 4 B: a larger span is not this array
    if (!Readable((const void*)b, (size_t)d)) return false;
    *begin = b;
    *span = (uint32_t)d;
    return true;
}

bool FortPosition(uintptr_t obj, float* xyz)      // compiler-owned layout: BaseEntity::GetPosition (M41..M43)
{
    __try {
        HookMath::Vector4 p = ((Entity*)obj)->GetPosition();
        xyz[0] = p.x; xyz[1] = p.y; xyz[2] = p.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Shot
{
    int         examined;
    int         forts;
    int         towerMgrs;      // entities carrying a FortTowerManager component (this shot, when counted)
    bool        towerMgrsOk;    // false: the FortTowerManager descriptor itself was unreadable
    const char* source;
    int         loggedForts;
};

void LogTowers(uintptr_t fort, uintptr_t towerVt, Shot* sh, char* line, size_t cap)
{
    uintptr_t base = 0;
    uint32_t  span = 0;
    if (!SmallArraySpan(fort + 0x118, &base, &span)) {
        _snprintf_s(line, cap, _TRUNCATE, "[Fort]   towersAlive unreadable at +0x118\n");
        Phase1_LogLine(line);
        return;
    }
    if (span % 4) {
        _snprintf_s(line, cap, _TRUNCATE,
                    "[Fort]   towersAlive span=%u bytes: (end-begin)%%4=%u, element size is not 4 - counts below are /4\n",
                    span, span % 4);
        Phase1_LogLine(line);
    }

    const uint32_t slots = span / 4;
    int shown = 0, shortOnes = 0;
    for (uint32_t i = 0; i < slots; i++) {
        uintptr_t slot = 0;
        if (!base || !ReadPtr(base + 4 * i, &slot)) continue;
        if (!LooksLikeObject(slot)) continue;           // not a handle slot: skipped, never printed as a tower
        if (shown >= kMaxTowersLog) break;
        const uintptr_t tower = ResolveTowerSlot(slot, towerVt);
        if (!tower) {
            _snprintf_s(line, cap, _TRUNCATE,
                        "[Fort]   tower[%d]=<unresolved> slot=%p (no tower object behind this slot)\n",
                        (int)i, (void*)slot);
            Phase1_LogLine(line);
            shown++;
            continue;
        }
        if (!Readable((const void*)tower, kTowerBytes)) {
            _snprintf_s(line, cap, _TRUNCATE,
                        "[Fort]   tower[%d]=%p unreadable to +0x2E8 (hp/disabled not read)\n", (int)i, (void*)tower);
            Phase1_LogLine(line);
            shown++; shortOnes++;
            continue;
        }
        const float hp  = *(const float*)(tower + 0x16C);   // ACGA offsets: +0x16C CurrentHealth, +0x2E3 disabled
        const int   dis = *(const uint8_t*)(tower + 0x2E3) ? 1 : 0;
        _snprintf_s(line, cap, _TRUNCATE, "[Fort]   tower[%d]=%p hp=%.1f disabled=%d\n", (int)i, (void*)tower, hp, dis);
        Phase1_LogLine(line);
        shown++;
    }
    if (shortOnes) {
        _snprintf_s(line, cap, _TRUNCATE, "[Fort]   towersAlive: %d object slot(s) too short to read\n", shortOnes);
        Phase1_LogLine(line);
    }
}

// One pass over the engine's entity manager (src/phase1.cpp:3489-3503). Returns the fort count, or -1 when the
// manager itself is unusable (count unreadable, non-positive, or absurd). The order and the break/continue split are
// phase1's: a dead reference slot ends the walk, a bad handle or object slot is skipped.
int ScanEntities(World* w, uintptr_t towerDesc, uintptr_t towerVt, bool logDetail, bool countTowerMgrs,
                 Shot* sh, char* line, size_t cap)
{
    BaseBigArray<Reference>& ents = w->m_EntityManager.m_Entities;   // World.h:228
    int nobj = 0;
    __try { nobj = ents.GetSize(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (nobj <= 0) return -1;

    if ((unsigned)nobj > kMaxEntries) {
        _snprintf_s(line, cap, _TRUNCATE,
                    "[Fort] entity manager count=%d exceeds the %u cap - walking the first %u\n",
                    nobj, (unsigned)kMaxEntries, (unsigned)kMaxEntries);
        Phase1_LogLine(line);
    }
    const int n = ((unsigned)nobj > kMaxEntries) ? (int)kMaxEntries : nobj;

    int forts = 0;
    uintptr_t seen[kMaxUniqueForts];
    int  nseen = 0;
    bool seenFull = false;
    for (int i = 0; i < n; ++i) {
        Reference* ref = nullptr;
        __try { ref = ents.GetElement(i); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!ref || !Readable(ref, 4)) break;                    // reference slot gone: end the walk, as phase1 does

        HandleContainer* hc = nullptr;
        Entity* e = nullptr;
        __try {
            hc = ref->m_HandleContainer;
            if (hc) e = (Entity*)hc->m_Object;
        } __except (EXCEPTION_EXECUTE_HANDLER) { hc = nullptr; e = nullptr; }
        if (!hc || !Readable(hc, 4)) continue;
        if (!e || !Readable(e, 4)) continue;
        sh->examined++;

        // The engine's own predicate (0x011D9380, cdecl, evidence ops/evidence/fort-enumeration-2-2026-11-20.md):
        // fort or null. Forts are reached through a GameplayCoordinator component, so no component-type test can
        // see one - that is why the earlier census measured zero. Guarded on +0x68 because the predicate reads the
        // component array data @ +0x60 and count @ +0x66.
        uintptr_t fort = 0;
        if (Readable(e, 0x68)) {
            __try { fort = (uintptr_t)((void* (__cdecl*)(void*, char))0x011D9380)((void*)e, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { fort = 0; }
        }

        if (countTowerMgrs) {
            uintptr_t tm = 0;
            __try { tm = (uintptr_t)e->GetComponentByType((ClassDescriptor*)towerDesc); }
            __except (EXCEPTION_EXECUTE_HANDLER) { tm = 0; }
            if (tm) sh->towerMgrs++;
        }

        if (!fort) continue;
        if (FortSeen(seen, &nseen, &seenFull, fort)) continue;   // same fort object reached from another entity
        if (!Readable((const void*)fort, kFortBytes)) {
            _snprintf_s(line, cap, _TRUNCATE, "[Fort] comp=%p unreadable to +0x3D8 - skipped\n", (void*)fort);
            Phase1_LogLine(line);
            continue;
        }
        forts++;
        if (!logDetail || sh->loggedForts >= kMaxFortsLog) continue;

        uint32_t phase = 0, aliveSpan = 0, deadSpan = 0;
        int combat = 0, fieldsOk = 0;
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        uintptr_t aliveBegin = 0, deadBegin = 0;
        __try {
            phase  = *(const uint32_t*)(fort + 0x74);
            combat = *(const uint8_t*)(fort + 0x2F8) ? 1 : 0;
            fieldsOk = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { fieldsOk = 0; }

        const bool posOk = FortPosition((uintptr_t)e, pos);
        const unsigned alive = SmallArraySpan(fort + 0x118, &aliveBegin, &aliveSpan) ? aliveSpan / 4 : 0;
        const unsigned dead  = SmallArraySpan(fort + 0xC0, &deadBegin, &deadSpan) ? deadSpan / 4 : 0;
        if (!fieldsOk) {
            _snprintf_s(line, cap, _TRUNCATE, "[Fort] fort=%p fields unreadable (+0x74/+0x2F8) - skipped\n", (void*)fort);
            Phase1_LogLine(line);
            continue;
        }
        _snprintf_s(line, cap, _TRUNCATE,
                    "[Fort] fort=%p pos=(%.1f,%.1f,%.1f) comp=%p towersAlive=%u destroyed=%u phase=%u combat=%d%s\n",
                    (void*)fort, posOk ? pos[0] : 0.0f, posOk ? pos[1] : 0.0f, posOk ? pos[2] : 0.0f,
                    (void*)fort, alive, dead, phase, combat, posOk ? "" : " pos=unreadable");
        Phase1_LogLine(line);
        LogTowers(fort, towerVt, sh, line, cap);
        sh->loggedForts++;
    }
    if (seenFull) {
        _snprintf_s(line, cap, _TRUNCATE,
                    "[Fort] unique-fort table (%d) full: further forts counted without de-duplication\n",
                    (int)kMaxUniqueForts);
        Phase1_LogLine(line);
    }
    return forts;
}

// ---------------------------------------------------------------------------------------------
// One-shot vtable census (read-only). Forts measured zero on both machines while the manager walk
// examined ~4k entities, so the probe needs to say which components this list actually holds:
// either the fort vtables are here (and the class-info lookup is what fails) or they are not (and
// forts live in another container). Runs once per run: at the first walk that examined >= 1000
// entities, or (loudly) after 120 s if the world never gets that large.
//
// Count source is the entity's component array: data @ entity+0x60, authoritative count (u16) @
// entity+0x66, clamped to 0..96 - the same read phase1.cpp:3494-3535 uses and documents
// (+0x64 is cap|flags, never the count). No engine address is introduced: the vtables are read
// from live objects, and the three watch constants below are only compared, never dereferenced.
// ---------------------------------------------------------------------------------------------
const int      kMaxDistinctVt = 256;   // distinct vtables tracked; the rest go to overflow
const int      kMaxTopVt      = 40;    // top lines logged
const int      kMaxComponents = 96;    // same safety clamp phase1 uses
// The census is only meaningful on a loaded world: the first shot of a run can fire while the world is
// still streaming (18 entities were observed), which measures an empty world and proves nothing. Arm it
// only once a walk has examined kCensusArmExamined entities; if the world never grows that large, run it
// anyway after kCensusForceMs and say so, so an under-loaded world is a stated fact rather than silence.
const int      kCensusArmExamined = 1000;
const ULONGLONG kCensusForceMs    = 120000;   // 120 s of ticks with a usable walk and examined < 1000

struct VtCount
{
    uint32_t vt;
    uint32_t n;
};

void CensusBump(VtCount* map, int* distinct, uint32_t* overflow, uint32_t vt)
{
    for (int i = 0; i < *distinct; ++i) {
        if (map[i].vt == vt) { map[i].n++; return; }
    }
    if (*distinct < kMaxDistinctVt) {
        map[*distinct].vt = vt;
        map[*distinct].n = 1;
        (*distinct)++;
        return;
    }
    (*overflow)++;
}

void CensusOnce(World* w, int examined)
{
    static bool s_done = false;
    if (s_done) return;                // one shot per process, whatever the tick rate
    if (examined < kCensusArmExamined) {
        static ULONGLONG s_first = 0;  // first tick at which a shot had a usable walk
        const ULONGLONG now = GetTickCount64();
        if (!s_first) s_first = now;
        if (now - s_first < kCensusForceMs) return;   // world still streaming: wait, and re-check next shot
        char forced[128];
        _snprintf_s(forced, sizeof(forced), _TRUNCATE,
                    "[FortC] forced at examined=%d (<1000 after 120s)\n", examined);
        Phase1_LogLine(forced);
    }
    s_done = true;

    char line[192];

    _snprintf_s(line, sizeof(line), _TRUNCATE, "[FortC] census at examined=%d\n", examined);
    Phase1_LogLine(line);

    BaseBigArray<Reference>& ents = w->m_EntityManager.m_Entities;   // World.h:228
    int nobj = 0;
    __try { nobj = ents.GetSize(); } __except (EXCEPTION_EXECUTE_HANDLER) { nobj = 0; }
    if (nobj <= 0) {
        Phase1_LogLine("[FortC] entities=0 components=0 distinct=0 overflow=0 (entity manager unusable)\n");
        return;
    }
    const int n = ((unsigned)nobj > kMaxEntries) ? (int)kMaxEntries : nobj;

    VtCount  map[kMaxDistinctVt];
    int      distinct = 0;
    uint32_t overflow = 0;
    int      entities = 0;
    unsigned long long components = 0;

    for (int i = 0; i < n; ++i) {
        Reference* ref = nullptr;
        __try { ref = ents.GetElement(i); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!ref || !Readable(ref, 4)) break;                    // same break/continue split as phase1

        HandleContainer* hc = nullptr;
        Entity* e = nullptr;
        __try {
            hc = ref->m_HandleContainer;
            if (hc) e = (Entity*)hc->m_Object;
        } __except (EXCEPTION_EXECUTE_HANDLER) { hc = nullptr; e = nullptr; }
        if (!hc || !Readable(hc, 4)) continue;
        if (!e || !Readable(e, 4)) continue;
        entities++;

        uint16_t    count = 0;
        Component** data  = nullptr;
        __try {
            count = *(const uint16_t*)((const uint8_t*)e + 0x66);
            data  = *(Component***)((const uint8_t*)e + 0x60);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (count < 1 || count > kMaxComponents) continue;
        if (!data || !Readable(data, (size_t)count * 4)) continue;

        for (int k = 0; k < count; ++k) {
            uintptr_t c = 0;
            __try { c = (uintptr_t)data[k]; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!c || !Readable((const void*)c, 4)) continue;
            uint32_t vt = 0;
            __try { vt = *(const uint32_t*)c; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!vt) continue;
            components++;
            CensusBump(map, &distinct, &overflow, vt);
        }
    }

    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[FortC] entities=%d components=%llu distinct=%d overflow=%u\n",
                entities, components, distinct, overflow);
    Phase1_LogLine(line);

    for (int i = 1; i < distinct; ++i) {                 // insertion sort, descending by count
        const VtCount cur = map[i];
        int j = i - 1;
        while (j >= 0 && map[j].n < cur.n) { map[j + 1] = map[j]; --j; }
        map[j + 1] = cur;
    }
    const int top = (distinct < kMaxTopVt) ? distinct : kMaxTopVt;
    for (int i = 0; i < top; ++i) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[FortC] vt=%08X n=%u\n", map[i].vt, map[i].n);
        Phase1_LogLine(line);
    }

    // The three vtables the fort question turns on: present (n>0) means the class-info lookup is
    // what misses; all zero while thousands of components were counted means another container.
    for (int pass = 0; pass < 3; ++pass) {
        static const uint32_t watchVt[3]   = { 0x026FD400, 0x026F7AC0, 0x02722048 };
        static const char*    watchName[3] = { "GcLNavalFort", "GcLNavalTowerACGA", "FortTowerManager" };
        uint32_t wn = 0;
        for (int i = 0; i < distinct; ++i)
            if (map[i].vt == watchVt[pass]) { wn = map[i].n; break; }
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[FortC] watch vt=%08X (%-16s) n=%u\n", watchVt[pass], watchName[pass], wn);
        Phase1_LogLine(line);
    }
}

void ShotOnce(bool logDetail)
{
    char line[256];
    World* w = nullptr;
    __try { w = World::GetCurrentWorld(); } __except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
    if (!w) {
        Phase1_LogLine("[Fort] examined=0 forts=0 source=- (no current world) (0x011D9380 not called)\n");
        return;
    }

    const uintptr_t towerDesc = GameModuleBase + (VA_TOWERMGR_CLASSINFO - 0x00400000);
    const uintptr_t towerVt   = GameModuleBase + (VA_TOWER_ACGA_VTABLE - 0x00400000);   // compared, never called

    Shot sh = { 0, 0, 0, false, "mgr.m_Entities", 0 };
    sh.towerMgrsOk = Readable((const void*)towerDesc, 4);
    const int forts = ScanEntities(w, towerDesc, towerVt, logDetail, logDetail, &sh, line, sizeof(line));

    if (forts < 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Fort] examined=0 forts=0 source=- (entity manager unusable: count unreadable or empty) "
                    "(0x011D9380 not called)\n");
        Phase1_LogLine(line);
        return;
    }

    if (!logDetail) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Fort] examined=%d forts=%d source=%s\n", sh.examined, forts, sh.source);
    } else if (!sh.towerMgrsOk) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Fort] examined=%d forts=%d towerMgrs=? source=%s (FortTowerManager classinfo %08X unreadable)\n",
                    sh.examined, forts, sh.source, (unsigned)VA_TOWERMGR_CLASSINFO);
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Fort] examined=%d forts=%d towerMgrs=%d source=%s\n",
                    sh.examined, forts, sh.towerMgrs, sh.source);
    }
    Phase1_LogLine(line);
    if (logDetail && sh.loggedForts >= kMaxFortsLog)
        Phase1_LogLine("[Fort] detail capped at 8 forts x 8 towers per 10 s; the counts above are complete\n");

    CensusOnce(w, sh.examined);   // one shot, at the first walk that examined >= 1000 entities
}

}  // namespace

void FortProbeTick()
{
    static int s_on = -1;                 // [Coop] FortProbe, read once: with it 0 not a single byte is read
    if (s_on < 0) s_on = GetPrivateProfileIntA("Coop", "FortProbe", 0, Paths::Ini()) ? 1 : 0;
    if (!s_on) return;

    static ULONGLONG s_next = 0;
    static ULONGLONG s_nextDetail = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) return;             // at most one shot each second, whatever the frame rate
    s_next = now + 1000;

    const bool detail = now >= s_nextDetail;   // fort/tower lines and the towerMgrs count: every 10 s
    if (detail) s_nextDetail = now + 10000;

    __try { ShotOnce(detail); } __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Fort] EXCEPTION 0x%08X (shot aborted)\n", GetExceptionCode());
        Phase1_LogLine(line);
    }
}

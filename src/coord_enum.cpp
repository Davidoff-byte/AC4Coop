// Coordinator enumerator (read-only).
//
// The chain is reflection-derived and documented once, here - it is not re-derived in this file:
//
//   entity +0x60[ ]  component array (data @ +0x60, authoritative count u16 @ +0x66; +0x64 is cap|flags, never
//                    the count)
//     GameplayCoordinatorComponent  classinfo 0x027AFEF0  size 0x80
//       +0x60 ObjectPtr Coordinator -> AbstractGameplayCoordinator  classinfo 0x0276FE08
//         +0x64 ObjectPtr GameplayLogic -> GcL*  (vtable names the logic class)
//
// The component is found with Entity::GetComponentByType - the receipted 0x005200C0 wrapper this project already
// uses everywhere (phase1.cpp:3486, fort_probe.cpp ScanEntities) - rather than a raw call, so this module
// introduces no engine address at all beyond the classinfo constant that is passed as a descriptor.
//
// Enumeration is exactly the idiom at phase1.cpp:3489-3503 (Reference -> m_HandleContainer -> m_Object), and the
// identity is the same one the ship lane calls CaptureShips uses: HandleContainer::GetObjectID()
// (phase1.cpp:4524, and FindLocalShipByObjId at 4544 resolves a peer record by comparing exactly it).
//
// Everything is read-only and every dereference is Readable()-guarded *and* SEH-wrapped: a slot freed between the
// guard and the read abandons that entry, never the walk.
#include "coord_enum.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"
#include "Hook.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>

void Phase1_LogLine(const char* s);

namespace {

const uintptr_t VA_COORD_CLASSINFO = 0x027AFEF0;   // GameplayCoordinatorComponent class descriptor (rebased at use)

const int      kMaxWalkEntities = 4096;   // clamp on the entity-manager count per walk (fort_probe's kMaxEntries)
const int      kMaxCollect      = 128;    // entries CoordEnumTick asks for in one shot
const int      kMaxDistinctVt   = 256;    // distinct logic vtables tracked; the rest go to overflow
const int      kMaxTopVt        = 20;     // histogram lines logged per shot
const int      kMaxObjLines     = 40;     // obj lines logged per shot
const ULONGLONG kIntervalMs     = 10000;  // one census each 10 s

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

// One guarded pointer read: a slot freed between the guard and the read abandons the slot, not the walk.
bool ReadPtr(uintptr_t at, uintptr_t* out)
{
    *out = 0;
    __try { *out = *(const uintptr_t*)at; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct VtCount
{
    uint32_t vt;
    uint32_t n;
};

void Bump(VtCount* map, int* distinct, uint32_t* overflow, uint32_t vt)
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

// The single walk behind both public entry points.
//
//   wantId == 0  collect mode: fill out[0..max), return the count written.
//   wantId != 0  match mode : return 1 with out[0] = the first entry matching (objId == wantId && vt == wantVt),
//                or 0. max is ignored in this mode.
int WalkCoords(CoordRef* out, int max, uint64_t wantId, uint32_t wantVt)
{
    if (!out || max <= 0) return 0;

    const uintptr_t coordDesc = GameModuleBase + (VA_COORD_CLASSINFO - 0x00400000);
    if (!Readable((const void*)coordDesc, 4)) return 0;      // descriptor unreadable: no component test possible

    World* w = nullptr;
    __try { w = World::GetCurrentWorld(); } __except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
    if (!w || !Readable(w, 4)) return 0;

    BaseBigArray<Reference>& ents = w->m_EntityManager.m_Entities;   // World.h:228
    int nobj = 0;
    __try { nobj = ents.GetSize(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (nobj <= 0) return 0;
    const int n = ((unsigned)nobj > (unsigned)kMaxWalkEntities) ? kMaxWalkEntities : nobj;

    const bool matchMode = (wantId != 0);
    int written = 0;

    for (int i = 0; i < n; ++i) {
        if (!matchMode && written >= max) break;

        // phase1.cpp:3489-3503: a dead reference slot ends the walk, a bad handle or object slot is skipped.
        Reference* ref = nullptr;
        __try { ref = ents.GetElement(i); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!ref || !Readable(ref, 4)) break;

        HandleContainer* hc = nullptr;
        Entity* e = nullptr;
        __try {
            hc = ref->m_HandleContainer;
            if (hc) e = (Entity*)hc->m_Object;
        } __except (EXCEPTION_EXECUTE_HANDLER) { hc = nullptr; e = nullptr; }
        if (!hc || !Readable(hc, 4)) continue;
        if (!e || !Readable(e, 4)) continue;

        // The receipted wrapper, not a raw call. A component array that is mid-grow can fault here, so the whole
        // lookup is under SEH and simply yields "no coordinator" for this entity.
        void* comp = nullptr;
        __try { comp = e->GetComponentByType((ClassDescriptor*)coordDesc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { comp = nullptr; }
        if (!comp || !Readable(comp, 0x64)) continue;            // +0x60 ObjectPtr needs 4 readable bytes

        uintptr_t coord = 0, logic = 0, vtRaw = 0;
        if (!ReadPtr((uintptr_t)comp + 0x60, &coord) || !coord) continue;
        if (!Readable((const void*)coord, 0x68)) continue;       // covers +0x64 GameplayLogic
        if (!ReadPtr(coord + 0x64, &logic) || !logic) continue;
        if (!Readable((const void*)logic, 4)) continue;
        if (!ReadPtr(logic, &vtRaw) || !vtRaw) continue;

        CoordRef r;
        r.ent   = e;
        r.objId = 0;
        __try { r.objId = hc->GetObjectID(); } __except (EXCEPTION_EXECUTE_HANDLER) { r.objId = 0; }
        r.comp  = comp;
        r.coord = (void*)coord;
        r.logic = (void*)logic;
        r.vt    = (uint32_t)vtRaw;

        if (matchMode) {
            if (r.objId == wantId && r.vt == wantVt) { out[0] = r; return 1; }
            continue;
        }
        out[written++] = r;
    }
    return matchMode ? 0 : written;
}

void ShotOnce()
{
    char line[192];

    CoordRef refs[kMaxCollect];
    const int n = CoordCollect(refs, kMaxCollect);

    VtCount  map[kMaxDistinctVt];
    int      distinct = 0;
    uint32_t overflow = 0;
    for (int i = 0; i < n; ++i) Bump(map, &distinct, &overflow, refs[i].vt);

    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[CoordE] n=%d distinct=%d%s\n", n, distinct, (n == kMaxCollect) ? " (capped)" : "");
    Phase1_LogLine(line);
    if (overflow) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CoordE] vt-overflow=%u (more than %d distinct logic vtables)\n", overflow, kMaxDistinctVt);
        Phase1_LogLine(line);
    }

    for (int i = 1; i < distinct; ++i) {                 // insertion sort, descending by count
        const VtCount cur = map[i];
        int j = i - 1;
        while (j >= 0 && map[j].n < cur.n) { map[j + 1] = map[j]; --j; }
        map[j + 1] = cur;
    }
    const int top = (distinct < kMaxTopVt) ? distinct : kMaxTopVt;
    for (int i = 0; i < top; ++i) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordE] vt=%08X n=%u\n", map[i].vt, map[i].n);
        Phase1_LogLine(line);
    }

    const int objs = (n < kMaxObjLines) ? n : kMaxObjLines;
    for (int i = 0; i < objs; ++i) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CoordE] obj=%016llX vt=%08X ent=%p logic=%p\n",
                    (unsigned long long)refs[i].objId, refs[i].vt, refs[i].ent, refs[i].logic);
        Phase1_LogLine(line);
    }
    if (n > objs) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CoordE] per-object lines capped at %d; the counts above are complete\n", kMaxObjLines);
        Phase1_LogLine(line);
    }
}

}  // namespace

int CoordCollect(CoordRef* out, int max)
{
    __try {
        return WalkCoords(out, max, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;                                        // the walk never faults the caller's frame
    }
}

bool CoordFindByObjId(uint64_t objId, uint32_t vt, CoordRef* out)
{
    if (!objId || !out) return false;                    // objId 0 is "unknown", never a match key
    __try {
        return WalkCoords(out, 1, objId, vt) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CoordEnumTick()
{
    static int s_on = -1;                 // [Coop] CoordEnum, read once: with it 0 not a single byte is read
    if (s_on < 0) s_on = GetPrivateProfileIntA("Coop", "CoordEnum", 0, Paths::Ini()) ? 1 : 0;
    if (!s_on) return;

    static bool s_header = false;
    if (!s_header) {
        s_header = true;
        Phase1_LogLine("[CoordE] coordinator enumerator (read-only): entity+0x60 -> GameplayCoordinatorComponent "
                       "027AFEF0 -> +0x60 Coordinator -> +0x64 GameplayLogic; 10 s census, identity = entity objId\n");
    }

    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) return;             // one census each 10 s, whatever the frame rate
    s_next = now + kIntervalMs;

    __try { ShotOnce(); } __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[CoordE] EXCEPTION 0x%08X (shot aborted)\n", GetExceptionCode());
        Phase1_LogLine(line);
    }
}

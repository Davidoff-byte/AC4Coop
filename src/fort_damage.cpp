// [Coop] FortDamageTest (default 0): the one-shot F1 oracle of docs/design/fort-sync.md 4.
//
// The question it settles: forts were unenumerable until the coordinator walk, and for a GcLNavalFort
// (logic vtable 0x026FD400) static work found the engine path
//
//   0x017E5DF0  __thiscall(ecx = GcLNavalTowerACGA*, float damage)            = ApplyDamage
//     -> 0x17DEA8E  health -= damage, clamped to [settings+0xD0, settings+0xCC]
//     -> sub_17E97D0 @0x17E987A  *(u8*)(tower+0x2E3) = 1, posts "FIGHT_FORT_STOP"   (the only setter)
//     -> sub_18A2D00 case 8      TowersAlive RemoveAt, --Fort+0x10C, CurrentFortPhase reload, ++*(Fort+0x74)
//
// but it was never established that the engine walks that chain by itself - the disable is reachable through a
// vtable slot (0x026F6348) whose caller was never found. This harness damages one tower once and reads the
// result, which turns the hop into an observation.
//
// Offsets are the reflection field map already used by fort_probe.cpp (not re-derived here):
//   GcLNavalFort        classinfo 0x02A18520
//     +0x118 SmallArray<Handle> TowersAlive   +0xC0 SmallArray<Handle> DestroyedFortEntities
//     +0x74  U32 FortPhaseCounter             +0x2F8 Bool HasEnteredCombat
//   GcLNavalTowerACGA   classinfo 0x02A020C8
//     +0x16C Float CurrentHealth              +0x2E3 Bool IsFortTowerDisabled
//
// Safety shape: the fort and the tower both come from the coordinator walk (they are never constants), every
// dereference is Readable()-guarded and SEH-wrapped, and the single engine call happens once, only when the flag
// is on, only on that resolved tower. A tower that becomes unreadable between shots (freed by the cascade) is
// logged with the sentinels hp=-1.0 dis=-1 and never counts as a PASS.
#include "fort_damage.h"
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

const uintptr_t VA_FORT_VTABLE       = 0x026FD400;   // GcLNavalFort (compared only, never called)
const uintptr_t VA_TOWER_ACGA_VTABLE = 0x026F7AC0;   // GcLNavalTowerACGA (compared only, never called)
const uintptr_t VA_APPLY_DAMAGE      = 0x017E5DF0;  // __thiscall(GcLNavalTowerACGA*, float) - the one call

const int       kMaxCollect = 256;      // coordinators asked of CoordCollect per search
const ULONGLONG kSearchMs   = 2000;     // search cadence until the shot fires
const ULONGLONG kWaitLogMs  = 10000;    // "waiting" line cadence
const ULONGLONG kS1Ms       = 500;      // first re-read
const ULONGLONG kS2Ms       = 2000;     // verdict re-read

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

// One guarded pointer read: a slot freed between the guard and the read abandons the slot, not the shot.
bool ReadPtr(uintptr_t at, uintptr_t* out)
{
    *out = 0;
    __try { *out = *(const uintptr_t*)at; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool LooksLikeObject(uintptr_t p)
{
    if (p < 0x10000 || p >= 0x7F000000 || (p & 3)) return false;
    if (!Readable((const void*)p, 4)) return false;
    uintptr_t vt = 0;
    __try { vt = *(const uintptr_t*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return vt >= 0x10000 && vt < 0x7F000000 && Readable((const void*)vt, 4);
}

// SmallArray = {begin, end, ...}. Element size is not established for SmallArray<Handle> in this binary, so the
// span is returned in bytes and divided by 4 - the same reading fort_probe.cpp uses (it logs a non-multiple of 4
// instead of guessing). No engine address is introduced.
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

// SmallArray<Handle> slot -> the tower object behind it (fort_probe.cpp ResolveTowerSlot, same two hops):
// a Handle is a 4-byte wrapper around a HandleContainer* (BaseTypes.h:48) whose m_Object is the tower
// (BaseTypes.h:29); a slot that IS the object is accepted too, and the vtable check keeps the two apart.
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
    if (slotVt == towerVt) return slot;                    // the slot is the ACGA object itself
    return 0;
}

uintptr_t ResolveTower0(uintptr_t fort, uintptr_t towerVt)
{
    uintptr_t base = 0;
    uint32_t  span = 0;
    if (!SmallArraySpan(fort + 0x118, &base, &span)) return 0;
    const uint32_t slots = span / 4;
    for (uint32_t i = 0; i < slots; ++i) {
        uintptr_t slot = 0;
        if (!base || !ReadPtr(base + 4 * i, &slot)) continue;
        const uintptr_t tower = ResolveTowerSlot(slot, towerVt);
        if (tower) return tower;                           // first slot that resolves to an ACGA object
    }
    return 0;
}

// One observation of the fort + tower pair. Sentinels (-1.0 hp, -1 disabled) mean "not readable", never a value.
struct Obs
{
    float    hp;
    int      dis;
    unsigned alive;
    unsigned dest;
    unsigned phase;
    bool     towerOk;
    bool     fortOk;
};

void ReadObs(uintptr_t fort, uintptr_t tower, Obs* o)
{
    o->hp = -1.0f;
    o->dis = -1;
    o->alive = 0;
    o->dest = 0;
    o->phase = 0;
    o->towerOk = false;
    o->fortOk = false;

    if (fort && Readable((const void*)fort, 0x124)) {      // covers +0x74, +0xC0 and +0x118
        uintptr_t ab = 0, db = 0;
        uint32_t  as = 0, ds = 0;
        __try {
            o->phase = *(const uint32_t*)(fort + 0x74);
            o->alive = SmallArraySpan(fort + 0x118, &ab, &as) ? as / 4 : 0;
            o->dest  = SmallArraySpan(fort + 0xC0, &db, &ds) ? ds / 4 : 0;
            o->fortOk = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            o->fortOk = false;
        }
    }

    if (tower && Readable((const void*)tower, 0x2E8)) {     // covers +0x16C (float) and +0x2E3 (bool)
        __try {
            o->hp  = *(const float*)(tower + 0x16C);
            o->dis = *(const uint8_t*)(tower + 0x2E3) ? 1 : 0;
            o->towerOk = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            o->hp = -1.0f;
            o->dis = -1;
            o->towerOk = false;
        }
    }
}

// One-shot state, per process: nothing here is reset, so the harness fires at most once whatever the frame rate.
int       s_state = 0;          // 0 = searching, 1 = waiting for S1, 2 = waiting for S2, 3 = finished
ULONGLONG s_nextSearch = 0;
ULONGLONG s_nextWaitLog = 0;
ULONGLONG s_tFire = 0;
uintptr_t s_fort = 0;
uintptr_t s_tower = 0;
float     s_hp0 = 0.0f;
unsigned  s_alive0 = 0;
unsigned  s_phase0 = 0;

void LogException(unsigned code, int stage)
{
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[FortD] EXCEPTION 0x%08X at S%d\n", code, stage);
    Phase1_LogLine(line);
    s_state = 3;
}

// One search pass. Fires the call at most once; a fort whose tower cannot be resolved yet is retried, since the
// towers stream in with the world.
void SearchAndFire()
{
    const uint32_t fortVt  = (uint32_t)(GameModuleBase + (VA_FORT_VTABLE       - 0x00400000));
    const uint32_t towerVt = (uint32_t)(GameModuleBase + (VA_TOWER_ACGA_VTABLE - 0x00400000));

    CoordRef refs[kMaxCollect];
    const int n = CoordCollect(refs, kMaxCollect);

    uintptr_t fort = 0;
    uint32_t  vts[kMaxCollect];
    int       classes = 0;
    for (int i = 0; i < n; ++i) {
        bool seen = false;
        for (int k = 0; k < classes; ++k) if (vts[k] == refs[i].vt) { seen = true; break; }
        if (!seen) vts[classes++] = refs[i].vt;
        if (!fort && refs[i].vt == fortVt) fort = (uintptr_t)refs[i].logic;
    }

    char line[192];
    const ULONGLONG now = GetTickCount64();

    if (!fort) {
        if (now >= s_nextWaitLog) {
            s_nextWaitLog = now + kWaitLogMs;
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[FortD] waiting: no GcLNavalFort coordinator (n=%d classes=%d)\n", n, classes);
            Phase1_LogLine(line);
        }
        return;
    }

    const uintptr_t tower = ResolveTower0(fort, towerVt);
    if (!tower) {
        if (now >= s_nextWaitLog) {
            s_nextWaitLog = now + kWaitLogMs;
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[FortD] waiting: fort=%p but towersAlive has no resolvable ACGA tower yet (n=%d classes=%d)\n",
                        (void*)fort, n, classes);
            Phase1_LogLine(line);
        }
        return;
    }

    Obs s0;
    ReadObs(fort, tower, &s0);

    uint32_t towerVtSeen = 0;
    int      combat = 0;
    __try { towerVtSeen = *(const uint32_t*)tower; } __except (EXCEPTION_EXECUTE_HANDLER) { towerVtSeen = 0; }
    __try { combat = *(const uint8_t*)(fort + 0x2F8) ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { combat = -1; }

    _snprintf_s(line, sizeof(line), _TRUNCATE, "[FortD] ctx combat=%d\n", combat);
    Phase1_LogLine(line);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[FortD] S0 fort=%p tower=%p vt=%08X hp=%.1f dis=%d alive=%u dest=%u phase=%u\n",
                (void*)fort, (void*)tower, towerVtSeen, s0.hp, s0.dis, s0.alive, s0.dest, s0.phase);
    Phase1_LogLine(line);

    s_fort = fort;
    s_tower = tower;
    s_hp0 = s0.hp;
    s_alive0 = s0.alive;
    s_phase0 = s0.phase;

    // The one engine call: ApplyDamage(tower, hp0 + 1.0f). Rebased from the VA in decomp/units.json "017E5DF0".
    const uintptr_t apply = GameModuleBase + (VA_APPLY_DAMAGE - 0x00400000);
    const float damage = s_hp0 + 1.0f;
    int exc = 0;
    __try {
        ((void (__thiscall*)(void*, float))apply)((void*)tower, damage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = (int)GetExceptionCode();
    }
    if (exc) {
        LogException((unsigned)exc, 0);
        return;
    }

    s_tFire = now;
    s_state = 1;
}

void LogShot(int stage)
{
    Obs o;
    ReadObs(s_fort, s_tower, &o);
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[FortD] S%d hp=%.1f dis=%d alive=%u dest=%u phase=%u\n",
                stage, o.hp, o.dis, o.alive, o.dest, o.phase);
    Phase1_LogLine(line);

    if (stage != 2) return;

    // The oracle of docs/design/fort-sync.md 4: hp <= 0, disabled set, TowersAlive one shorter (or the fort was
    // already empty), and the phase counter never going backwards. Both objects must still be readable, else the
    // -1 sentinels would read as "hp dropped".
    const bool aliveOk = (s_alive0 > 0) && (o.alive == s_alive0 - 1);
    const bool ok = o.towerOk && o.fortOk && o.hp <= 0.0f && o.dis == 1 && aliveOk && o.phase >= s_phase0;

    const char* reason;
    if (!o.towerOk)                          reason = "no-change";   // tower unreadable/freed: nothing observed
    else if (o.hp <= 0.0f && o.dis == 1)     reason = "cascaded";
    else if (o.hp < s_hp0)                   reason = "hp-only";     // damage landed, the disable hop did not run
    else                                     reason = "no-change";

    _snprintf_s(line, sizeof(line), _TRUNCATE, "[FortD] CASCADE ok=%d reason=%s\n", ok ? 1 : 0, reason);
    Phase1_LogLine(line);
    s_state = 3;
}

}  // namespace

void FortDamageTestTick()
{
    static int s_on = -1;                 // [Coop] FortDamageTest, read once: with it 0 not a single byte is read
    if (s_on < 0) s_on = GetPrivateProfileIntA("Coop", "FortDamageTest", 0, Paths::Ini()) ? 1 : 0;
    if (!s_on) return;
    if (s_state >= 3) return;             // one shot per process

    static bool s_header = false;
    if (!s_header) {
        s_header = true;
        Phase1_LogLine("[FortD] one-shot F1 oracle: first GcLNavalFort coordinator, tower[0], ApplyDamage 0x017E5DF0 "
                       "(hp0+1), reads at +0.5 s / +2 s; +0x16C and +0x2E3 are never written\n");
    }

    const ULONGLONG now = GetTickCount64();

    if (s_state == 0) {
        if (now < s_nextSearch) return;   // at most one search each 2 s, whatever the frame rate
        s_nextSearch = now + kSearchMs;
        __try { SearchAndFire(); } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogException((unsigned)GetExceptionCode(), 0);
        }
        return;
    }

    // Non-blocking re-reads: the frame is never held, the counters are compared against the fire time.
    const ULONGLONG elapsed = now - s_tFire;
    if (s_state == 1 && elapsed >= kS1Ms) {
        __try { LogShot(1); } __except (EXCEPTION_EXECUTE_HANDLER) { LogException((unsigned)GetExceptionCode(), 1); }
        if (s_state == 1) s_state = 2;
    } else if (s_state == 2 && elapsed >= kS2Ms) {
        __try { LogShot(2); } __except (EXCEPTION_EXECUTE_HANDLER) { LogException((unsigned)GetExceptionCode(), 2); }
    }
}

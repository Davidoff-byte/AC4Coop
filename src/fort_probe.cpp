// [Coop] FortProbe (default 0, read at first tick; observation only, no writes of any kind). Fort sync needs to know
// which forts and towers exist on each machine and what state they are in. Nothing in the mod enumerated forts
// before this probe; it answers that question and nothing else.
//
// Field map (given by reflection; NOT re-derived here, and every read below is guarded so a wrong offset can only
// produce a rejected line, never a write or an aborted frame):
//   GcLNavalFort        classinfo 0x02A18520  size 0x3D8
//     +0x118 SmallArray<Handle> TowersAlive        +0xC0 SmallArray<Handle> DestroyedFortEntities
//     +0x74  U32 FortPhaseCounter                  +0x2F8 Bool HasEnteredCombat
//     +0x120 BaseObject CurrentFortPhase           +0x64 SmallArray<Handle> TowerSetup
//   GcLNavalTowerACGA   classinfo 0x02A020C8  size 0x4F0   <- the RUNTIME tower class
//     +0x16C Float CurrentHealth                   +0x2E3 Bool IsFortTowerDisabled
//     +0xB8  Handle Fort (back-pointer; documented on GcLNavalTower - UNVERIFIED whether ACGA carries it, not read)
//   FortTowerManager    classinfo 0x02A911C0  size 0xF0 base Component
//     +0x2C SmallArray<ObjectPtr> RuntimeTowers (ACGA)   +0x81 Bool PlayerShipInFortRange
//
// Entity component getter: 0x005200C0. Reached through the already-receipted Entity::GetComponentByType
// (src/Scimitar/Entity.cpp:19 -> Hook::CallClassMethod<Component*, Entity*, ClassDescriptor*>(0x005200C0, ...);
// decomp/units.json "005200C0"), so this unit introduces NO new engine address and no new calling convention.
//
// Enumeration. The world handle array (world+0x80 = array, +0x84 = count; handle = {obj, soft rc, hard rc, u64 id})
// is a LEAD from the AC4Tweaker digest and is NOT verified in this binary. Everything about it is guarded, and the
// entry stride is probed empirically (0x18 / 0x14 / 0x10): the stride that actually yields fort components is the
// one named in the `source=` field, so the log states what was used instead of assuming a layout. If the array is
// unusable the probe says so (examined=0, source=-) rather than staying silent.
//
// Rates: one shot per second whatever the frame rate. Detail lines are capped (8 forts x 8 towers per shot) so a
// naval map cannot flood the log; the summary line carries the true counts and repeats every shot.
#include "fort_probe.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>

void Phase1_LogLine(const char* s);

namespace {

const uintptr_t VA_FORT_CLASSINFO = 0x02A18520;   // GcLNavalFort class descriptor (rebased at use)

const uint32_t kMaxEntries   = 4096;   // clamp on the world handle count
const int      kMaxFortsLog  = 8;      // forts detailed per shot
const int      kMaxTowersLog = 8;      // tower lines per fort
const uint32_t kStrides[3]   = { 0x18, 0x14, 0x10 };
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
// as a pointer fails this, which is what makes the stride probe meaningful.
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
    const char* source;
    int         loggedForts;
};

void LogTowers(uintptr_t fort, Shot* sh, char* line, size_t cap)
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
        uintptr_t tower = 0;
        if (!base || !ReadPtr(base + 4 * i, &tower)) continue;
        if (!LooksLikeObject(tower)) continue;          // not an object slot: skipped, never printed as a tower
        if (shown >= kMaxTowersLog) break;
        if (!Readable((const void*)tower, kTowerBytes)) {
            _snprintf_s(line, cap, _TRUNCATE,
                        "[Fort]   tower[%d]=%p unreadable to +0x2E8 (hp/disabled not read)\n", (int)i, (void*)tower);
            Phase1_LogLine(line);
            shown++; shortOnes++;
            continue;
        }
        const float hp  = *(const float*)(tower + 0x16C);
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

// One pass over the world handle array with one stride. Returns the fort count, or -1 when the array itself is
// unusable (pointer/count unreadable, zero, or out of range).
int ScanStride(World* w, uintptr_t desc, uint32_t stride, bool logDetail, Shot* sh, char* line, size_t cap)
{
    const char* wb = (const char*)w;
    if (!Readable(wb + 0x80, 8)) return -1;
    const uintptr_t arr = *(const uintptr_t*)(wb + 0x80);
    const uint32_t  cnt = *(const uint32_t*)(wb + 0x84);
    if (!arr || !cnt || cnt > kMaxEntries) return -1;
    if (!Readable((const void*)arr, (size_t)cnt * stride)) return -1;

    int forts = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uintptr_t entry = arr + (uintptr_t)i * stride;
        if (!Readable((const void*)entry, 4)) continue;          // unreadable entry: skipped, frame unaffected
        uintptr_t obj = 0;
        if (!ReadPtr(entry, &obj)) continue;
        if (!LooksLikeObject(obj)) continue;
        sh->examined++;

        Component* comp = nullptr;
        __try { comp = ((Entity*)obj)->GetComponentByType((ClassDescriptor*)desc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { comp = nullptr; }
        if (!comp) continue;

        const uintptr_t fort = (uintptr_t)comp;
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

        const bool posOk = FortPosition(obj, pos);
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
                    (void*)comp, alive, dead, phase, combat, posOk ? "" : " pos=unreadable");
        Phase1_LogLine(line);
        LogTowers(fort, sh, line, cap);
        sh->loggedForts++;
    }
    return forts;
}

void ShotOnce()
{
    char line[256];
    World* w = nullptr;
    __try { w = World::GetCurrentWorld(); } __except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
    if (!w) {
        Phase1_LogLine("[Fort] examined=0 forts=0 source=- (no current world) (no fort component matched)\n");
        return;
    }

    const uintptr_t desc = GameModuleBase + (VA_FORT_CLASSINFO - 0x00400000);
    if (!Readable((const void*)desc, 4)) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Fort] examined=0 forts=0 source=- (fort classinfo %08X unreadable) (no fort component matched)\n",
                    (unsigned)VA_FORT_CLASSINFO);
        Phase1_LogLine(line);
        return;
    }

    // Stride probe: the first stride that yields a fort is the one the details and the source string name. If none
    // does, the largest examined count is still reported - an empty result must be a stated fact.
    int best = -1, bestExamined = 0;
    bool arrayUnusable = false;
    for (int s = 0; s < 3; s++) {
        Shot probe = { 0, 0, "-", 0 };
        const int n = ScanStride(w, desc, kStrides[s], false, &probe, line, sizeof(line));
        if (n < 0) { arrayUnusable = true; break; }              // array unusable: other strides cannot help
        if (probe.examined > bestExamined) bestExamined = probe.examined;
        if (n > 0) { best = s; break; }
    }

    if (best < 0) {
        if (arrayUnusable) {
            Phase1_LogLine("[Fort] examined=0 forts=0 source=- (world+0x80 handle array unusable: pointer or count "
                           "unreadable, zero, or count > 4096) (no fort component matched)\n");
        } else {
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[Fort] examined=%d forts=0 source=world+0x80 (no fort component matched)\n", bestExamined);
            Phase1_LogLine(line);
        }
        return;
    }

    static char src[40];
    _snprintf_s(src, sizeof(src), _TRUNCATE, "world+0x80/stride=0x%X", (unsigned)kStrides[best]);
    Shot detail = { 0, 0, src, 0 };
    const int forts = ScanStride(w, desc, kStrides[best], true, &detail, line, sizeof(line));
    if (forts <= 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Fort] examined=%d forts=0 source=%s (no fort component matched)\n", detail.examined, src);
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Fort] examined=%d forts=%d source=%s\n", detail.examined, forts, src);
    }
    Phase1_LogLine(line);
    if (detail.loggedForts >= kMaxFortsLog)
        Phase1_LogLine("[Fort] detail capped at 8 forts x 8 towers per shot; the counts above are complete\n");
}

}  // namespace

void FortProbeTick()
{
    static int s_on = -1;                 // [Coop] FortProbe, read once: with it 0 not a single byte is read
    if (s_on < 0) s_on = GetPrivateProfileIntA("Coop", "FortProbe", 0, Paths::Ini()) ? 1 : 0;
    if (!s_on) return;

    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) return;             // at most one shot each second, whatever the frame rate
    s_next = now + 1000;

    __try { ShotOnce(); } __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Fort] EXCEPTION 0x%08X (shot aborted)\n", GetExceptionCode());
        Phase1_LogLine(line);
    }
}

// Guard-shaped spawn probe. See guard_spawn_probe.h for the question and the route, and
// docs/evidence/guard-spawn-route-2026-11-12.md for every address below (IDA 9.3 on AC4BFSP.exe,
// sha256 bc11ad79...ab5150, image base 0x400000). Nothing in this file runs unless [Coop] GuardSpawnProbe=1.
#include "guard_spawn_probe.h"
#include "paths.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include "Scimitar/World.h"
#include "Scimitar/EntityBuilder.h"
#include "Scimitar/RowSelector.h"
#include "Scimitar/DuplicationManager.h"
#include "Scimitar/AI/AIEntityHelper.h"

extern "C" int AC4Coop_FormatBuildLine(char* out, int cap);   // src/dllmain.cpp

namespace {

// ---- detour sites (bytes read from the image; IDA disasm quoted in the evidence doc) -----------------------
// EntityBuilder::BuildEntityWithSelector, EntityBuilder vtable 0x1E57790 slot 17 (+0x44). __thiscall
// (this=builder, Entity** out, RowSelector* sel), `retn 8`. Prologue: push ebp / mov ebp,esp / push esi /
// mov esi,[ebp+0Ch] - 7 bytes, no relative operand.
const uintptr_t VA_BUILD = 0x005FAC00;
const BYTE kBuildPrologue[7] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x0C };
// sub_16FA7D0: the per-Human update (vtable slot at 0x26EBBE8). ecx = Human* (`mov esi, ecx` @0x16FA7EF).
// Prologue: push ebx / mov ebx,esp / sub esp,8 - 6 bytes, no relative operand. phase1's frame hook patches
// 0x16FAACD inside the same function; these bytes do not overlap it.
const uintptr_t VA_HUMAN_UPDATE = 0x016FA7D0;
const BYTE kHumanPrologue[6] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 };

// ---- layout constants ---------------------------------------------------------------------------------------
const uint32_t VT_CHARACTER_ENTITY = 0x01E4CE90;  // guard / player entity vtable (AMBIENT-NPC-SYNC-PLAN L269)
const uint32_t VT_SKELETON         = 0x01E41E58;  // SkeletonComponent (class-info-map.tsv)
const uint32_t VT_BHV_GENERIC_NPC  = 0x026E34D8;  // BhvGenericNPC (phase1.cpp:4646)
const uint32_t VT_WEAPON           = 0x027106A8;  // WeaponComponent -> soldier (phase1.cpp:4649)
const uint32_t VT_GUARD_ICON       = 0x0269E9D0;  // MapIconBehaviorGuard -> soldier (phase1.cpp:4650)
const uint32_t VT_CSRV_NPC_HEALTH  = 0x02712F60;  // CSrvNPCHealth service (phase1.cpp:7676 FindHealthInAI)
const size_t   ENT_COMP_DATA   = 0x60;   // Entity component array (phase1.cpp:4635)
const size_t   ENT_COMP_COUNT  = 0x66;   // u16 count (phase1.cpp:4633, "never +0x64")
const size_t   BHV_ARCH        = 0x14;   // BhvGenericNPC id the wire calls `arch` (netstate.h:230)
const size_t   AI_SVC_ARRAY    = 0x70;   // AI service array / u16 count @+0x76 (phase1.cpp:7669)
const size_t   AI_SVC_COUNT    = 0x76;
// RowSelector (class-info 0x27B0BD0, name "RowSelector", size 0x4C). +0x10 = refcounted EntityBuilder handle
// container: Duplicate (0x7A3BD0) copies it with sub_A2B760 (soft ref +4), 0x7A3C90 releases this+16, and
// sub_46D8E0 dereferences params+0x4C (= embedded selector at +0x3C, +0x10) as the builder it calls, and the
// builder's own per-phase init 0x5FD560 tests handleDeref(*(sel+0x10)) == this.
// UNVERIFIED live until a log shows `selHcIsBuilder=1` on the builds this probe records.
const size_t   SEL_BUILDER_HC  = 0x10;
const size_t   HC_OBJECT_ID    = 0x0C;   // HandleContainer::m_ObjectID (BaseTypes.h; 0x12F9C90 reads *(u64*)(hc+12))

// ---- config ------------------------------------------------------------------------------------------------
int      g_cycles   = 20;     // [Coop] GuardSpawnProbeCycles (0 = observe + census only)
int      g_delayMs  = 30000;  // [Coop] GuardSpawnProbeDelayS  - time in world before the first cycle
int      g_holdMs   = 3000;   // [Coop] GuardSpawnProbeHoldMs  - AddEntity -> oracle check -> RemoveEntity
int      g_gapMs    = 1500;   // [Coop] GuardSpawnProbeGapMs   - RemoveEntity -> next build
float    g_distM    = 6.0f;   // [Coop] GuardSpawnProbeDistM   - placement offset on +X from the player
uint64_t g_wantId   = 0;      // [Coop] GuardSpawnProbeBuilder - builder object id (hex) to clone; 0 = auto

// ---- log ---------------------------------------------------------------------------------------------------
FILE*            g_log = nullptr;
CRITICAL_SECTION g_logCs;
CRITICAL_SECTION g_capCs;

void Log(const char* fmt, ...) {
    char buf[768];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logCs);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    LeaveCriticalSection(&g_logCs);
    OutputDebugStringA(buf);
}

bool Readable(const void* p, size_t n) {
    if (!p || !n) return false;
    __try {
        volatile char a = ((const volatile char*)p)[0];
        volatile char b = ((const volatile char*)p)[n - 1];
        (void)a; (void)b;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Scimitar handle container -> object: obj = hc[0] & (hc[2] >> 31) (0x46D8E9..0x46D8FA; phase1 handleDeref).
void* HandleDeref(const void* hc) {
    if (!Readable(hc, 12)) return nullptr;
    int32_t obj  = ((const int32_t*)hc)[0];
    int32_t vbit = ((const int32_t*)hc)[2] >> 31;
    return (void*)(obj & vbit);
}

void* SelBuilderHc(const void* sel) {
    if (!Readable(sel, SEL_BUILDER_HC + 4)) return nullptr;
    return *(void* const*)((const char*)sel + SEL_BUILDER_HC);
}

uint64_t HcObjectId(const void* hc) {
    if (!Readable(hc, HC_OBJECT_ID + 8)) return 0;
    return *(const uint64_t*)((const char*)hc + HC_OBJECT_ID);
}

// ---- body classification (one walk of the component array) -------------------------------------------------
struct Body {
    bool     ok;          // entity readable, component array sane
    uint32_t vt;          // entity vtable
    int      comps;
    bool     skel, bhv, soldier;
    uint32_t arch;        // BhvGenericNPC+0x14 & 0xFFFF (0 when no BhvGenericNPC)
    uint32_t vts[96];     // component vtables in array order
};

void Classify(const void* e, Body& b) {
    memset(&b, 0, sizeof(b));
    if (!Readable(e, 0x70)) return;
    b.vt = *(const uint32_t*)e;
    int n = *(const uint16_t*)((const char*)e + ENT_COMP_COUNT);
    const uint32_t* data = *(const uint32_t* const*)((const char*)e + ENT_COMP_DATA);
    if (n < 0 || n > 96) return;
    if (n > 0 && !Readable(data, (size_t)n * 4)) return;
    b.comps = n;
    for (int k = 0; k < n; ++k) {
        const void* c = (const void*)data[k];
        uint32_t cvt = Readable(c, 4) ? *(const uint32_t*)c : 0;
        b.vts[k] = cvt;
        if (cvt == VT_SKELETON) b.skel = true;
        if (cvt == VT_WEAPON || cvt == VT_GUARD_ICON) b.soldier = true;
        if (cvt == VT_BHV_GENERIC_NPC) {
            b.bhv = true;
            if (Readable(c, BHV_ARCH + 4)) b.arch = *(const uint32_t*)((const char*)c + BHV_ARCH) & 0xFFFF;
        }
    }
    b.ok = true;
}

bool SameComponents(const Body& a, const Body& b) {
    if (!a.ok || !b.ok || a.comps != b.comps) return false;
    for (int k = 0; k < a.comps; ++k) if (a.vts[k] != b.vts[k]) return false;
    return true;
}

// CSrvNPCHealth lookup: AIEntityHelper::GetEntityAI (0x6F5F70: entity+0xE8) then the AI's service array,
// gated like phase1's HostHpSvcValid (vtable + plausible Max@+0x5E).
uint32_t HealthService(void* e, uint32_t* life, uint32_t* maxLife) {
    *life = *maxLife = 0;
    void* ai = nullptr;
    __try { ai = (void*)AIEntityHelper::GetEntityAI((Entity*)e); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!Readable(ai, AI_SVC_COUNT + 2)) return 0;
    uint32_t arr = *(uint32_t*)((char*)ai + AI_SVC_ARRAY);
    uint32_t n   = *(uint16_t*)((char*)ai + AI_SVC_COUNT);
    if (!arr || n == 0 || n > 64 || !Readable((void*)arr, n * 4)) return 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t svc = ((uint32_t*)arr)[k];
        if (!Readable((void*)svc, 0x70) || *(uint32_t*)svc != VT_CSRV_NPC_HEALTH) continue;
        *life    = *(uint16_t*)(svc + 0x5C);
        *maxLife = *(uint16_t*)(svc + 0x5E);
        if (*maxLife < 1 || *maxLife > 2000) return 0;
        return svc;
    }
    return 0;
}

// ---- captured guard types ----------------------------------------------------------------------------------
struct Capture {
    uint64_t id;          // builder object id (selector+0x10 handle container, m_ObjectID)
    void*    dupSel;      // our own RowSelector copy; it holds a soft ref on the builder container
    uint32_t arch;
    bool     soldier;
    Body     body;        // the source body as it came out of the build
    volatile LONG state;  // 0 free, 1 reserved (duplicating), 2 ready, 3 failed
};
const int CAP_MAX = 32;
Capture g_cap[CAP_MAX];
int     g_capN = 0;

// distinct builder ids seen (observation log, any body kind)
const int SEEN_MAX = 96;
uint64_t g_seenId[SEEN_MAX];
int      g_seenN = 0;

volatile LONG g_ourTid = 0;          // thread currently inside one of OUR builds
volatile LONG g_builds = 0, g_npcBuilds = 0, g_soldierBuilds = 0, g_ourBuilds = 0, g_buildExc = 0;

typedef int (__fastcall* Build_t)(void* builder, void* edx, void** out, void* sel);
Build_t g_origBuild = nullptr;

bool DuplicateSelector(void* sel, void** out) {
    *out = nullptr;
    bool ok = false;
    DuplicationManager::Begin(1, 0);              // 0xA2D5A0, same arguments as the puppet and 0x12F9F8D
    __try {
        __try { *out = ((RowSelector*)sel)->Duplicate(0, 0); ok = Readable(*out, 0x4C); }  // 0x7A3BD0
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    } __finally {
        DuplicationManager::End();                // 0xA2F320 - must run even on a fault
    }
    return ok;
}

void OnBuilt(void* builder, void** out, void* sel) {
    if ((LONG)GetCurrentThreadId() == g_ourTid) { InterlockedIncrement(&g_ourBuilds); return; }
    InterlockedIncrement(&g_builds);
    void* e = Readable(out, 4) ? *out : nullptr;
    Body b; Classify(e, b);
    const bool npc = b.ok && b.skel && b.bhv;
    if (npc) InterlockedIncrement(&g_npcBuilds);
    if (npc && b.soldier) InterlockedIncrement(&g_soldierBuilds);

    void* hc = SelBuilderHc(sel);
    const uint64_t id = HcObjectId(hc);
    const bool hcIsBuilder = hc && HandleDeref(hc) == builder;

    bool firstSeen = false;
    int  slot = -1;
    EnterCriticalSection(&g_capCs);
    {
        bool seen = false;
        for (int i = 0; i < g_seenN; ++i) if (g_seenId[i] == id) { seen = true; break; }
        if (!seen && g_seenN < SEEN_MAX) { g_seenId[g_seenN++] = id; firstSeen = true; }
        if (npc && id) {
            bool have = false;
            for (int i = 0; i < g_capN; ++i) if (g_cap[i].id == id) { have = true; break; }
            if (!have && g_capN < CAP_MAX) {
                slot = g_capN++;
                g_cap[slot].id = id; g_cap[slot].arch = b.arch; g_cap[slot].soldier = b.soldier;
                g_cap[slot].body = b; g_cap[slot].dupSel = nullptr;
                InterlockedExchange(&g_cap[slot].state, 1);
            }
        }
    }
    LeaveCriticalSection(&g_capCs);

    if (firstSeen)
        Log("[GSP] build builderId=%08X`%08X builder=%p bvt=%08X sel=%p selHc=%p selHcIsBuilder=%d ent=%p evt=%08X "
            "comps=%d skel=%d bhv=%d soldier=%d arch=%04X tid=%lu\n",
            (uint32_t)(id >> 32), (uint32_t)id, builder, Readable(builder, 4) ? *(uint32_t*)builder : 0, sel, hc,
            hcIsBuilder ? 1 : 0, e, b.vt, b.comps, b.skel, b.bhv, b.soldier, b.arch, GetCurrentThreadId());

    if (slot >= 0) {
        // Duplicate outside the lock: an engine call must never run while another thread waits on our lock.
        void* dup = nullptr;
        const bool ok = DuplicateSelector(sel, &dup);
        g_cap[slot].dupSel = dup;
        InterlockedExchange(&g_cap[slot].state, ok ? 2 : 3);
        Log("[GSP] capture #%d builderId=%08X`%08X arch=%04X soldier=%d comps=%d dupSel=%p dupHc=%p ok=%d\n",
            slot, (uint32_t)(id >> 32), (uint32_t)id, b.arch, b.soldier, b.comps, dup, SelBuilderHc(dup), ok ? 1 : 0);
    }
}

int __fastcall MyBuild(void* builder, void* edx, void** out, void* sel) {
    const int rc = g_origBuild(builder, edx, out, sel);
    __try { OnBuilt(builder, out, sel); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_buildExc); }
    return rc;
}

// ---- player-update tick ------------------------------------------------------------------------------------
struct Cycle {
    int       n;             // cycles started
    int       built, added, skel, bhv, hp, sameArch, sameComps, removed, exc, notLoaded;
    int       phase;         // 0 waiting, 1 holding, 2 gap, 3 done
    ULONGLONG t;             // phase deadline
    Entity*   e;
    int       capIdx;
    float     want[3];
};
Cycle     g_cy = {};
ULONGLONG g_inWorldSince = 0, g_nextCensus = 0, g_cycleReady = 0;
bool      g_headerDone = false;

// census: distinct entity handle ids ever seen in the world list
const int ID_SET = 16384;   // power of two
uint64_t  g_ids[ID_SET];
int       g_idsUsed = 0;
int       g_censusN = 0;
LONG      g_lastBuilds = 0, g_lastNpc = 0, g_lastSoldier = 0;

bool IdInsert(uint64_t id) {   // true when new
    if (!id || g_idsUsed >= ID_SET / 2) return false;   // half full: stop counting (reported as idsFull)
    uint32_t h = (uint32_t)(id ^ (id >> 29)) * 2654435761u;
    for (int p = 0; p < ID_SET; ++p) {
        uint64_t& s = g_ids[(h + p) & (ID_SET - 1)];
        if (s == id) return false;
        if (s == 0) { s = id; ++g_idsUsed; return true; }
    }
    return false;
}

void Census(World* w, ULONGLONG now) {
    int ents = 0, npc = 0, soldier = 0, nNew = 0, nNewNpc = 0, nNewSoldier = 0;
    BaseBigArray<Reference>& list = w->m_EntityManager.m_Entities;
    const int size = list.GetSize();
    for (int i = 0; i < size; ++i) {
        Reference* ref = list.GetElement(i);
        if (!Readable(ref, 4)) break;
        HandleContainer* hc = ref->m_HandleContainer;
        if (!Readable(hc, 0x14)) continue;
        void* e = (void*)hc->m_Object;
        if (!Readable(e, 0x70)) continue;
        ++ents;
        Body b; Classify(e, b);
        const bool isNpc = b.ok && b.skel && b.bhv;
        if (isNpc) ++npc;
        if (isNpc && b.soldier) ++soldier;
        if (IdInsert(hc->m_ObjectID)) { ++nNew; if (isNpc) ++nNewNpc; if (isNpc && b.soldier) ++nNewSoldier; }
    }
    const LONG bl = g_builds, np = g_npcBuilds, so = g_soldierBuilds;
    Log("[GSP] census #%d t=%llus %s ents=%d npc=%d soldier=%d new=%d newNpc=%d newSoldier=%d "
        "builds+%ld npcBuilds+%ld soldierBuilds+%ld ourBuilds=%ld buildExc=%ld idsFull=%d\n",
        g_censusN, (now - g_inWorldSince) / 1000, g_censusN == 0 ? "BASELINE" : "window",
        ents, npc, soldier, nNew, nNewNpc, nNewSoldier,
        bl - g_lastBuilds, np - g_lastNpc, so - g_lastSoldier, (long)g_ourBuilds, (long)g_buildExc,
        g_idsUsed >= ID_SET / 2 ? 1 : 0);
    g_lastBuilds = bl; g_lastNpc = np; g_lastSoldier = so;
    ++g_censusN;
}

int PickCapture() {
    int best = -1;
    EnterCriticalSection(&g_capCs);
    for (int i = 0; i < g_capN; ++i) {
        if (g_cap[i].state != 2) continue;
        if (g_wantId) { if (g_cap[i].id == g_wantId) { best = i; break; } continue; }
        if (best < 0 || (g_cap[i].soldier && !g_cap[best].soldier)) best = i;   // first soldier, else first NPC
    }
    LeaveCriticalSection(&g_capCs);
    return best;
}

void SpawnOne(World* w, Entity* player, int ci) {
    Capture& c = g_cap[ci];
    ++g_cy.n;
    void* builder = HandleDeref(SelBuilderHc(c.dupSel));
    if (!Readable(builder, 4)) {
        ++g_cy.notLoaded;
        Log("[GSP] cycle %d: builder %08X`%08X not loaded (hc=%p) - skipped\n",
            g_cy.n, (uint32_t)(c.id >> 32), (uint32_t)c.id, SelBuilderHc(c.dupSel));
        return;
    }
    void* sel = nullptr;
    Entity* e = nullptr;
    int rc = -1;
    InterlockedExchange(&g_ourTid, (LONG)GetCurrentThreadId());
    if (DuplicateSelector(c.dupSel, &sel))
        rc = ((EntityBuilder*)builder)->BuildEntityWithSelector(&e, sel);   // 0x5FAC00 (through our detour)
    InterlockedExchange(&g_ourTid, 0);
    Body b; Classify(e, b);
    const bool same = SameComponents(b, c.body);
    Log("[GSP] cycle %d: build rc=%d sel=%p ent=%p evt=%08X comps=%d (src %d) skel=%d bhv=%d arch=%04X (src %04X) "
        "sameCompsAtBuild=%d\n", g_cy.n, rc, sel, e, b.vt, b.comps, c.body.comps, b.skel, b.bhv, b.arch, c.arch,
        same ? 1 : 0);
    if (!b.ok || b.vt != VT_CHARACTER_ENTITY) return;
    ++g_cy.built;

    HookMath::Vector4 p = player->GetPosition();
    p.x += g_distM;
    w->AddEntity(e);              // 0x70E860, deferred to the world's next update (as the puppet)
    e->SetPosition(p);            // raw m_GlobalMatrix write, the puppet's own placement (phase1.cpp:619)
    ++g_cy.added;
    g_cy.e = e; g_cy.capIdx = ci;
    g_cy.want[0] = p.x; g_cy.want[1] = p.y; g_cy.want[2] = p.z;
}

void CheckAndRemove(World* w) {
    Entity* e = g_cy.e;
    g_cy.e = nullptr;
    const Capture& c = g_cap[g_cy.capIdx];
    Body b; Classify(e, b);
    uint32_t life = 0, maxLife = 0;
    const uint32_t svc = (b.ok && b.vt == VT_CHARACTER_ENTITY) ? HealthService(e, &life, &maxLife) : 0;
    const bool same = SameComponents(b, c.body);
    float pos[3] = { 0, 0, 0 };
    if (b.ok) { pos[0] = e->m_GlobalMatrix.M41; pos[1] = e->m_GlobalMatrix.M42; pos[2] = e->m_GlobalMatrix.M43; }
    if (b.skel) ++g_cy.skel;
    if (b.bhv)  ++g_cy.bhv;
    if (svc)    ++g_cy.hp;
    if (b.bhv && b.arch == c.arch) ++g_cy.sameArch;
    if (same)   ++g_cy.sameComps;
    Log("[GSP] cycle %d: hold ent=%p evt=%08X comps=%d skel=%d bhv=%d soldier=%d arch=%04X hp=%08X life=%u/%u "
        "sameComps=%d pos=(%.1f,%.1f,%.1f) want=(%.1f,%.1f,%.1f)\n", g_cy.n, e, b.vt, b.comps, b.skel, b.bhv,
        b.soldier, b.arch, svc, life, maxLife, same ? 1 : 0, pos[0], pos[1], pos[2],
        g_cy.want[0], g_cy.want[1], g_cy.want[2]);
    if (!b.ok || b.vt != VT_CHARACTER_ENTITY) { Log("[GSP] cycle %d: entity gone before removal\n", g_cy.n); return; }
    w->RemoveEntity(e);           // 0x70E880 -> sub_5A8AC0, the call DespawnPuppet makes
    ++g_cy.removed;
}

void Result() {
    const int N = g_cycles;
    const bool pass = g_cy.n == N && g_cy.built == N && g_cy.added == N && g_cy.skel == N && g_cy.bhv == N &&
                      g_cy.hp == N && g_cy.sameArch == N && g_cy.sameComps == N && g_cy.removed == N && g_cy.exc == 0;
    Log("[GSP] RESULT cycles=%d/%d built=%d added=%d skel=%d bhv=%d hp=%d sameArch=%d sameComps=%d removed=%d "
        "notLoaded=%d exc=%d verdict=%s\n", g_cy.n, N, g_cy.built, g_cy.added, g_cy.skel, g_cy.bhv, g_cy.hp,
        g_cy.sameArch, g_cy.sameComps, g_cy.removed, g_cy.notLoaded, g_cy.exc, pass ? "PASS" : "FAIL");
}

void Tick(World* w, Entity* player) {
    const ULONGLONG now = GetTickCount64();
    if (!g_headerDone) {
        g_headerDone = true;
        char bl[512] = {0};
        if (AC4Coop_FormatBuildLine(bl, (int)sizeof(bl)) > 0) Log("%s", bl);
        Log("[GSP] in world: cycles=%d delay=%dms hold=%dms gap=%dms dist=%.1fm want=%08X`%08X tid=%lu\n",
            g_cycles, g_delayMs, g_holdMs, g_gapMs, g_distM, (uint32_t)(g_wantId >> 32), (uint32_t)g_wantId,
            GetCurrentThreadId());
        g_inWorldSince = now; g_nextCensus = now; g_cycleReady = now + (ULONGLONG)g_delayMs;
    }
    if (now >= g_nextCensus) { g_nextCensus = now + 10000; Census(w, now); }

    if (g_cycles <= 0 || g_cy.phase == 3) return;
    if (g_cy.phase == 0) {
        if (now < g_cycleReady) return;
        const int ci = PickCapture();
        if (ci < 0) {
            static ULONGLONG s_nag = 0;
            if (now >= s_nag) { s_nag = now + 10000; Log("[GSP] waiting: no guard type captured yet\n"); }
            return;
        }
        if (g_cy.n == 0)
            Log("[GSP] cloning capture #%d builderId=%08X`%08X arch=%04X soldier=%d comps=%d\n", ci,
                (uint32_t)(g_cap[ci].id >> 32), (uint32_t)g_cap[ci].id, g_cap[ci].arch, g_cap[ci].soldier,
                g_cap[ci].body.comps);
        __try { SpawnOne(w, player, ci); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_cy.exc; InterlockedExchange(&g_ourTid, 0); g_cy.e = nullptr;
            Log("[GSP] cycle %d: EXCEPTION 0x%08X in build/add\n", g_cy.n, GetExceptionCode());
        }
        if (g_cy.e) { g_cy.phase = 1; g_cy.t = now + g_holdMs; }
        else        { g_cy.phase = 2; g_cy.t = now + g_gapMs; }
    } else if (g_cy.phase == 1) {
        if (now < g_cy.t) return;
        __try { CheckAndRemove(w); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_cy.exc; g_cy.e = nullptr;
            Log("[GSP] cycle %d: EXCEPTION 0x%08X in check/remove\n", g_cy.n, GetExceptionCode());
        }
        g_cy.phase = 2; g_cy.t = now + g_gapMs;
    } else if (g_cy.phase == 2) {
        if (now < g_cy.t) return;
        if (g_cy.n >= g_cycles) { g_cy.phase = 3; Result(); return; }
        g_cy.phase = 0;
    }
}

// Called at the entry of every Human update; acts only on the player's (phase1.cpp isPlayerUpdate shape).
void __cdecl OnHumanUpdate(void* human) {
    __try {
        World* w = World::GetCurrentWorld();          // 0x54D4D0
        if (!w) return;
        Entity* player = w->GetPlayer();              // 0x70E810
        if (!player) return;
        if ((void*)AIEntityHelper::GetHuman(player) != human) return;   // 0x13905A0
        Tick(w, player);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static LONG s_n = 0;
        if (InterlockedIncrement(&s_n) <= 20) Log("[GSP] EXCEPTION 0x%08X in tick (skipped)\n", GetExceptionCode());
    }
}

DWORD g_humanTramp = 0;

__declspec(naked) void HumanUpdateDetour() {
    __asm {
        pushad
        pushfd
        push ecx                  // Human* (thiscall this)
        call OnHumanUpdate
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_humanTramp]
    }
}

// Copy the verified prologue to RWX memory, append `jmp target+len`.
BYTE* MakeTrampoline(uintptr_t target, const BYTE* prologue, size_t len) {
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, prologue, len);
    t[len] = 0xE9;
    *(int32_t*)(t + len + 1) = (int32_t)((target + len) - ((uintptr_t)t + len + 5));
    FlushInstructionCache(GetCurrentProcess(), t, len + 5);
    return t;
}

bool Patch(uintptr_t at, size_t len, void* to, const char* what) {
    DWORD old = 0;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[GSP] %s @%08X: VirtualProtect failed - NOT patched\n", what, (unsigned)at);
        return false;
    }
    BYTE* p = (BYTE*)at;
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)to - (at + 5));
    for (size_t i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    Log("[GSP] %s @%08X: detoured\n", what, (unsigned)at);
    return true;
}

} // namespace

void GuardSpawnProbe::Init() {
    if (!GetPrivateProfileIntA("Coop", "GuardSpawnProbe", 0, Paths::Ini())) return;
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&g_capCs);
    char leaf[64];
    _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "AC4Coop_gsp_%lu.txt", GetCurrentProcessId());
    fopen_s(&g_log, Paths::LogPath(leaf), "w");

    g_cycles  = GetPrivateProfileIntA("Coop", "GuardSpawnProbeCycles", 20, Paths::Ini());
    g_delayMs = GetPrivateProfileIntA("Coop", "GuardSpawnProbeDelayS", 30, Paths::Ini()) * 1000;
    g_holdMs  = GetPrivateProfileIntA("Coop", "GuardSpawnProbeHoldMs", 3000, Paths::Ini());
    g_gapMs   = GetPrivateProfileIntA("Coop", "GuardSpawnProbeGapMs", 1500, Paths::Ini());
    g_distM   = (float)GetPrivateProfileIntA("Coop", "GuardSpawnProbeDistM", 6, Paths::Ini());
    char idStr[32] = {0};
    GetPrivateProfileStringA("Coop", "GuardSpawnProbeBuilder", "", idStr, sizeof(idStr), Paths::Ini());
    if (idStr[0]) g_wantId = _strtoui64(idStr, nullptr, 16);
    if (g_cycles < 0) g_cycles = 0;
    if (g_cycles > 200) g_cycles = 200;

    Log("[GSP] init GuardSpawnProbe=1 cycles=%d delay=%dms hold=%dms gap=%dms dist=%.1f\n",
        g_cycles, g_delayMs, g_holdMs, g_gapMs, g_distM);

    // Both prologues are checked before either is patched, so a mismatch leaves the image untouched.
    if (memcmp((const void*)VA_BUILD, kBuildPrologue, sizeof(kBuildPrologue)) != 0 ||
        memcmp((const void*)VA_HUMAN_UPDATE, kHumanPrologue, sizeof(kHumanPrologue)) != 0) {
        Log("[GSP] a prologue differs from the verified bytes - nothing patched\n");
        return;
    }
    BYTE* tb = MakeTrampoline(VA_BUILD, kBuildPrologue, sizeof(kBuildPrologue));
    BYTE* th = MakeTrampoline(VA_HUMAN_UPDATE, kHumanPrologue, sizeof(kHumanPrologue));
    if (!tb || !th) { Log("[GSP] trampoline alloc failed - nothing patched\n"); return; }
    g_origBuild  = (Build_t)tb;
    g_humanTramp = (DWORD)(uintptr_t)th;
    Patch(VA_BUILD, sizeof(kBuildPrologue), (void*)&MyBuild, "BuildEntityWithSelector");
    Patch(VA_HUMAN_UPDATE, sizeof(kHumanPrologue), (void*)&HumanUpdateDetour, "HumanUpdate");
}

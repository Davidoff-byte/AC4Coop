// [Coop] SpawnCensus (default 0, read at load, read-only hooks): where does EVERY entity that enters the world come from?
// Question (owner 2026-11-25): is there one spawn manager for everything, so the guest can stop spawning on its own and
// build only what the host spawned? Static map (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35,
// docs/evidence/spawn-manager-2026-11-25.md):
//   F  0x0046D8E0 SpawnEntityParams::Spawn base (vt 0x01E43BB0 slot 31): thiscall(params, a2), retn 4, returns the entity.
//      Every entity-producing SpawnParams class reaches it: xrefs = 0x1128778 (SpawnObjectParams 0x1128740),
//      0x1898E2E (SpawnNPCParams / SpawnCharacterParams 0x1898E20), 0x1996ABB (SpawnShipParams 0x1996AB0, and
//      SpawnPlayerShipParams 0x122BD30 through it), data 0x1E43C2C (the base vtable). Bytes 55 8B EC 83 EC 08.
//   R  0x0079B8D0 SpawningRequest::Spawn: thiscall(req); the request entry (11 callers). Bytes 56 8B F1 8B 06.
//      Not hooked when [Coop] DisableSpawns / DisableCrowd is on (phase1 owns that site then).
//   A  0x005A8A80 world add core: (world, entity, u16 slot, flags) - World::AddEntity 0x70E860 and 25 other callers.
//      Bytes 55 8B EC 8B 45 10.
//   D  0x005A8AC0 world remove core: (world, entity) - 27 callers incl. the SpawningManager removal queue sub_44FA20.
//      Bytes 55 8B EC 8B 45 0C.
// Each add is classified: produced by F (entity seen leaving F) or not, character (AIEntityHelper::GetHuman != 0) or not,
// and keyed by its caller (first raw-stack return address outside the World::AddEntity wrapper 0x70E860..0x70E880).
// Aggregates are printed by SpawnCensusReport ([Census] lines); the first 120 adds/removes are also printed one by one.
#include "Hook.h"
#include "paths.h"
#include "net.h"
#include "Scimitar/Entity.h"
#include "Scimitar/AI/AIEntityHelper.h"
#include "Scimitar/ManagedObject.h"

#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <intrin.h>
#include <cmath>
#include <cstdlib>

void Phase1_LogLine(const char* s);

namespace {
uintptr_t g_lo = 0, g_hi = 0;
bool g_on = false;
DWORD g_tF = 0, g_tR = 0, g_tA = 0, g_tD = 0;
volatile LONG g_lock = 0, g_lines = 0, g_watch = 0;
bool g_keyBind = false;    // [Coop] SpawnKeyBind (see the end of this file)
DWORD g_tSet = 0;          // trampoline of sub_5BC470 (request <- built entity)
bool g_libFlag = false;    // [Coop] SpawnLib hooks requested and installed (spawn_lib.cpp)
bool g_share = false;      // [Coop] SpawnShare (see the SpawnShare section at the end of this file)
float g_shareR = 150.f;    // [Coop] SpawnShareRadiusM

struct Spin { Spin() { while (InterlockedExchange(&g_lock, 1)) YieldProcessor(); } ~Spin() { InterlockedExchange(&g_lock, 0); } };

bool IsRet(uint32_t v)
{
    if (v < g_lo + 6 || v >= g_hi) return false;
    const BYTE* p = (const BYTE*)(uintptr_t)v;
    if (p[-5] == 0xE8) { const uint32_t t = v + *(const int32_t*)(p - 4); return t >= g_lo && t < g_hi; }
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && (p[-1] & 0xC0) == 0xC0) return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10 && ((p[-5] & 0xC0) == 0x80 || p[-5] == 0x15)) return true;
    return false;
}
int Scan(uintptr_t esp, uint32_t* out, int cap)
{
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    uintptr_t end = esp + 0x800;
    if (end > (uintptr_t)tib->StackBase) end = (uintptr_t)tib->StackBase;
    int n = 0;
    for (uintptr_t a = esp; a + 4 <= end && n < cap; a += 4) { const uint32_t v = *(const uint32_t*)a; if (IsRet(v)) out[n++] = v; }
    return n;
}

// Entities seen leaving F (ring), with the params class that made them.
constexpr int kRing = 4096;
struct Made { volatile uintptr_t e; uintptr_t pvt; };
Made g_made[kRing];
volatile LONG g_madePos = 0;
uintptr_t MadeBy(uintptr_t e)
{
    const LONG pos = g_madePos;
    for (int i = 0; i < kRing; ++i) { const Made& m = g_made[(pos - 1 - i) & (kRing - 1)]; if (m.e == e) return m.pvt ? m.pvt : 1; }
    return 0;
}

// Aggregate tables: key -> count (+ split counters). Linear, small.
struct Row { uint32_t key, key2; LONG n, human, funnel; };
template <int N> struct Table {
    Row r[N]; int used = 0; LONG overflow = 0;
    Row* Get(uint32_t k, uint32_t k2) {
        for (int i = 0; i < used; ++i) if (r[i].key == k && r[i].key2 == k2) return &r[i];
        if (used < N) { r[used] = { k, k2, 0, 0, 0 }; return &r[used++]; }
        ++overflow; return nullptr;
    }
};
Table<96> g_adds;      // key = caller ret, key2 = params vt if produced by F, else 0
Table<48> g_funnel;    // key = params vt, key2 = caller of F
Table<48> g_reqs;      // key = request vt, key2 = caller of R
Table<64> g_dels;      // key = caller ret
LONG g_nAdd = 0, g_nDel = 0, g_nF = 0, g_nR = 0;

void NoteAdd(uint32_t caller, uintptr_t pvt, bool h)
{ Spin s; ++g_nAdd; if (Row* r = g_adds.Get(caller, pvt > 1 ? (uint32_t)pvt : (pvt ? 1u : 0u))) { ++r->n; r->human += h; r->funnel += pvt != 0; } }
void NoteDel(uint32_t caller, uintptr_t pvt, bool h)
{ Spin s; ++g_nDel; if (Row* r = g_dels.Get(caller, 0)) { ++r->n; r->human += h; r->funnel += pvt != 0; } }
void NoteReq(uint32_t vt, uint32_t caller) { Spin s; ++g_nR; if (Row* r = g_reqs.Get(vt, caller)) ++r->n; }
void NoteF(uint32_t pvt, uint32_t caller, bool made, bool h)
{ Spin s; ++g_nF; if (Row* r = g_funnel.Get(pvt, caller)) { ++r->n; r->human += h; r->funnel += made; } }
uint32_t CallerOf(uintptr_t esp)
{
    uint32_t f[4]; const int n = Scan(esp, f, 4);
    for (int i = 0; i < n; ++i) if (!(f[i] >= 0x0070E860 && f[i] < 0x0070E880)) return f[i];
    return n ? f[0] : 0;
}
bool IsHuman(uintptr_t e) { __try { return e && AIEntityHelper::GetHuman((Entity*)e) != nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
void Pos(uintptr_t e, float* p) { p[0] = p[1] = p[2] = 0; __try { const HookMath::Vector4 v = ((Entity*)e)->GetPosition(); p[0] = v.x; p[1] = v.y; p[2] = v.z; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}  // namespace

extern "C" void __cdecl CensusAdd(const uint32_t* esp)   // esp[0] = ret, esp[1] = world, esp[2] = entity
{
    __try {
        const uintptr_t e = esp[2];
        const uint32_t caller = CallerOf((uintptr_t)esp);
        const uintptr_t pvt = MadeBy(e);
        const bool h = IsHuman(e);
        NoteAdd(caller, pvt, h);
        if (InterlockedIncrement(&g_lines) <= 120) {
            float p[3]; Pos(e, p); char line[200];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] add ent=%08X evt=%08X human=%d funnel=%08X caller=%08X pos=(%.0f, %.0f, %.0f)\n",
                        (unsigned)e, e ? *(const unsigned*)e : 0u, h ? 1 : 0, (unsigned)pvt, caller, p[0], p[1], p[2]);
            Phase1_LogLine(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
extern "C" void __cdecl CensusDel(const uint32_t* esp)   // esp[1] = world, esp[2] = entity
{
    __try {
        const uintptr_t e = esp[2];
        const uint32_t caller = CallerOf((uintptr_t)esp);
        const bool h = IsHuman(e);
        const uintptr_t pvt = MadeBy(e);
        NoteDel(caller, pvt, h);
        if (InterlockedIncrement(&g_lines) <= 120) {
            char line[160];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] del ent=%08X human=%d funnel=%08X caller=%08X\n", (unsigned)e, h ? 1 : 0, (unsigned)pvt, caller);
            Phase1_LogLine(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int DumpWords(char* out, size_t cap, uintptr_t base, int n);   // defined below
int SpawnShareDecide(uintptr_t req);   // defined below ([Coop] SpawnShare)
void SpawnKeyNoteReq(uintptr_t req);   // defined below ([Coop] SpawnKeyBind)
extern "C" void __cdecl SpawnKeyNoteEnt(const uint32_t* esp);
extern "C" void __cdecl CensusReq(uintptr_t req, const uint32_t* esp)
{
    __try {
        const uint32_t vt = req ? *(const uint32_t*)req : 0;
        NoteReq(vt, esp[0]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

namespace {
typedef uintptr_t(__thiscall* SpawnFn)(void*, int);
uintptr_t __fastcall DetF(void* params, void*, int a2)
{
    void* fr = _AddressOfReturnAddress();
    const uintptr_t e = ((SpawnFn)g_tF)(params, a2);
    __try {
        const uint32_t pvt = *(const uint32_t*)params;
        if (e) { Made& m = g_made[(InterlockedIncrement(&g_madePos) - 1) & (kRing - 1)]; m.e = 0; m.pvt = pvt; m.e = e; }
        NoteF(pvt, *(const uint32_t*)fr, e != 0, e && IsHuman(e));
        if (e && (pvt == 0x0270A538u || pvt == 0x026A20C8u)) {   // ship built: its template builder's ObjectID (HandleContainer at params+0x4C, id at hc+0x0C)
            static LONG s_sk = 0;
            if (InterlockedIncrement(&s_sk) <= 80) {
                const uintptr_t hc = *(const uintptr_t*)((uintptr_t)params + 0x4C);
                const uint32_t lo = hc ? *(const uint32_t*)(hc + 0x0C) : 0, hi = hc ? *(const uint32_t*)(hc + 0x10) : 0;
                char line[180]; _snprintf_s(line, sizeof(line), _TRUNCATE, "[ShipKey] ship ent=%08X pvt=%08X params=%08X builder=%08X:%08X\n",
                                            (unsigned)e, pvt, (unsigned)(uintptr_t)params, hi, lo);
                Phase1_LogLine(line);
                // AI ships: builder id 0 on both copies (run E99E075E). Dump the params (0x98 B, class map size) and the builder
                // object for a cross-copy content key. Lock-free reads.
                static char big[3000]; int k = _snprintf_s(big, sizeof(big), _TRUNCATE, "[ShipKD] n=%ld P:", s_sk);
                if (k > 0) k += DumpWords(big + k, sizeof(big) - k, (uintptr_t)params, 38);
                const uintptr_t bo = hc ? *(const uintptr_t*)hc : 0;
                if (k > 0) k += _snprintf_s(big + k, sizeof(big) - k, _TRUNCATE, " | B:");
                if (k > 0 && bo) k += DumpWords(big + k, sizeof(big) - k, bo, 24);
                for (int off = 0, part = 0; off < k; off += 900, ++part) { char seg[1000]; _snprintf_s(seg, sizeof(seg), _TRUNCATE, "[ShipKD+] n=%ld p=%d %.900s\n", s_sk, part, big + off); Phase1_LogLine(seg); }
            }
        }
        if (g_watch > 0 && InterlockedDecrement(&g_watch) >= 0) { char line[160]; float p[3]; Pos(e, p); _snprintf_s(line, sizeof(line), _TRUNCATE, "[Replay] build after fire: params=%08X pvt=%08X -> ent=%08X human=%d\n", (unsigned)(uintptr_t)params, pvt, (unsigned)e, e && IsHuman(e) ? 1 : 0); Phase1_LogLine(line); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return e;
}
// SpawningRequest::Spawn 0x79B8D0 is thiscall(req), no stack args, plain retn (disasm 0x79B8D0..0x79B91A). Returning 0
// without calling it = declined, the same answer the request's own vt[0] gate gives ([Coop] DisableSpawns does this).
typedef char(__thiscall* SubmitFn)(void*);
char __fastcall DetR(void* req, void*)
{
    const uint32_t ret[1] = { *(const uint32_t*)_AddressOfReturnAddress() };
    CensusReq((uintptr_t)req, ret);
    SpawnKeyNoteReq((uintptr_t)req);                          // [Coop] SpawnKeyBind: req -> spawn key
    if (SpawnShareDecide((uintptr_t)req) == 1) return 0;   // [Coop] SpawnShare: guest holds this request
    return ((SubmitFn)g_tR)(req);
}
__declspec(naked) void DetA()
{
    __asm { pushad
            lea eax, [esp + 32]
            push eax
            call CensusAdd
            add esp, 4
            popad
            jmp dword ptr [g_tA] }
}
__declspec(naked) void DetD()
{
    __asm { pushad
            lea eax, [esp + 32]
            push eax
            call CensusDel
            add esp, 4
            popad
            jmp dword ptr [g_tD] }
}

// ---- Request capture + replay (also under SpawnCensus) -------------------------------------------------------------
// The whole submit is one cdecl function, sub_569560(item, owner, ctx, id_lo, id_hi) (disasm 0x569560..0x5695A0):
//   req = singleton(dword_2AC1E88)->vt[0x84/4](ctx, item)   = sub_F72ED0, thiscall, retn 8 (singleton unused)
//   sub_79C6C0(req, owner, id_lo, id_hi)                    thiscall, retn 0Ch: req+0xC = owner, req+0x18/+0x1C = id
//                                                           (id 0 -> taken from owner->vt[0x48])
//   sub_79B8D0(req)                                         submit
// Other submitters (sub_56F8E0, CrowdHerder) inline the same three calls. We hook the factory (55 8B EC 8B 45 08) and
// the source setter (55 8B EC 53 8B 5D 10, 7 bytes) and join them by req.
DWORD g_tFac = 0, g_tSrc = 0;
struct Cap { volatile uintptr_t req; uintptr_t ctx, item, itemVt, owner; uint32_t idLo, idHi; ULONGLONG t; volatile LONG printed; };
constexpr int kCap = 1024;
Cap g_cap[kCap];
volatile LONG g_capPos = 0, g_facN = 0, g_srcN = 0;
typedef uintptr_t(__thiscall* FacFn)(void*, uintptr_t, uintptr_t);
typedef void(__thiscall* SrcFn)(void*, uintptr_t, uint32_t, uint32_t);
uintptr_t __fastcall DetFac(void* self, void*, uintptr_t ctx, uintptr_t item)
{
    const uintptr_t req = ((FacFn)g_tFac)(self, ctx, item);
    __try {
        InterlockedIncrement(&g_facN);
        if (req) {
            Cap& c = g_cap[(InterlockedIncrement(&g_capPos) - 1) & (kCap - 1)];
            c.req = 0; c.ctx = ctx; c.item = item; c.itemVt = item ? *(const uintptr_t*)item : 0; c.owner = 0;
            c.idLo = c.idHi = 0; c.t = GetTickCount64(); c.printed = 0; c.req = req;
        }
        { void ShipReplayNoteFac(uintptr_t); ShipReplayNoteFac(req); void SpawnLibNoteFac(uintptr_t); SpawnLibNoteFac(req); }   // [Coop] ShipSpawnReplay: our own submit's request (tagged thread)
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return req;
}
void __fastcall DetSrc(void* req, void*, uintptr_t owner, uint32_t lo, uint32_t hi)
{
    ((SrcFn)g_tSrc)(req, owner, lo, hi);
    __try {
        InterlockedIncrement(&g_srcN);
        const LONG pos = g_capPos;
        for (int i = 0; i < 64; ++i) {
            Cap& c = g_cap[(pos - 1 - i) & (kCap - 1)];
            if (c.req == (uintptr_t)req) { c.owner = owner; c.idLo = lo; c.idHi = hi; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void DetSet()   // sub_5BC470 entry: [esp+4] = request, [esp+8] = built entity
{
    __asm { pushad
            lea eax, [esp + 32]
            push eax
            call SpawnKeyNoteEnt
            add esp, 4
            popad
            jmp dword ptr [g_tSet] }
}

// [Coop] SpawnLib: sub_501DF0 = SpawningSpecification dtor body (thiscall, ecx = spec; called by the deleting dtor vt[0]
// sub_504560 and by owners of embedded specs). It releases +0x24/+0x28/+0x2C/+0x34/+0x38 and never touches the request
// in +0x3C, so a queued request of ours would build from freed memory (run T5 guest 29392, AV 0x69D0A5).
DWORD g_tSpecDt = 0;
extern "C" void SpawnLibSpecDying(uintptr_t spec);   // spawn_lib.cpp
__declspec(naked) void DetSpecDtor()
{
    __asm { pushad
            push ecx
            call SpawnLibSpecDying
            add esp, 4
            popad
            jmp dword ptr [g_tSpecDt] }
}

// [Coop] SpawnLib: sub_79C640 = SpawningRequest dtor body (thiscall, ecx = request; IDA: called by the deleting dtor
// vt[1] sub_79DA90 at 0x79DA96 and by sub_549F10 at 0x549F56). Our request table drops the entry before the body runs.
DWORD g_tReqDt = 0;
extern "C" void SpawnLibReqDying(uintptr_t req);   // spawn_lib.cpp
__declspec(naked) void DetReqDtor()
{
    __asm { pushad
            push ecx
            call SpawnLibReqDying
            add esp, 4
            popad
            jmp dword ptr [g_tReqDt] }
}

bool Install(uintptr_t va, const BYTE* pro, int n, void* det, DWORD* slot, const char* name)
{
    BYTE* site = (BYTE*)(GameModuleBase + (va - 0x00400000));
    char line[128];
    if (memcmp(site, pro, n) != 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] %s %08X prologue mismatch - not installed\n", name, (unsigned)va);
        Phase1_LogLine(line); return false;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, pro, n); t[n] = 0x68; *(DWORD*)(t + n + 1) = (DWORD)(uintptr_t)(site + n); t[n + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, n + 6);
    *slot = (DWORD)(uintptr_t)t;
    const bool ok = Hook::DetourFunc(site, (DWORD)(uintptr_t)det, (DWORD)n);
    if (!ok) *slot = 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] %s %08X hooked=%d\n", name, (unsigned)va, ok ? 1 : 0);
    Phase1_LogLine(line);
    return ok;
}

template <int N> void Dump(const char* tag, Table<N>& t, const char* k2name)
{
    Row copy[N]; int used;
    { Spin s; used = t.used; memcpy(copy, t.r, sizeof(Row) * used); }
    char line[200];
    for (int i = 0; i < used; ++i) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] %s key=%08X %s=%08X n=%ld human=%ld funnel=%ld\n", tag, copy[i].key, k2name,
                    copy[i].key2, copy[i].n, copy[i].human, copy[i].funnel);
        Phase1_LogLine(line);
    }
}
}  // namespace

void SpawnCensusInit()
{
    const bool census = GetPrivateProfileIntA("Coop", "SpawnCensus", 0, Paths::Ini()) != 0;
    g_keyBind = GetPrivateProfileIntA("Coop", "SpawnKeyBind", 0, Paths::Ini()) != 0;
    const bool shareFlag = GetPrivateProfileIntA("Coop", "SpawnShare", 0, Paths::Ini()) != 0;
    int ShipSpawnReplayInitFlag(); void ShipSpawnReplaySetHooked(bool);
    const bool ssr = ShipSpawnReplayInitFlag() != 0;   // [Coop] ShipSpawnReplay needs DetFac + DetSrc + DetSet
    int SpawnLibInitFlag(); void SpawnLibSetHooked(bool);
    g_libFlag = SpawnLibInitFlag() != 0;               // [Coop] SpawnLib (spawn_lib.cpp) needs DetFac + DetSrc + DetR + DetSet
    if (!census && !g_keyBind && !shareFlag && !ssr && !g_libFlag) return;   // SpawnKeyBind / SpawnShare need the request hooks below
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)GameModuleBase;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(GameModuleBase + dos->e_lfanew);
    g_lo = GameModuleBase + nt->OptionalHeader.BaseOfCode;
    g_hi = g_lo + nt->OptionalHeader.SizeOfCode;
    static const BYTE pF[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
    static const BYTE pR[5] = { 0x56, 0x8B, 0xF1, 0x8B, 0x06 };
    static const BYTE pA[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10 };
    static const BYTE pD[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C };
    const bool spawnsOwned = GetPrivateProfileIntA("Coop", "DisableSpawns", 0, Paths::Ini()) || GetPrivateProfileIntA("Coop", "DisableCrowd", 0, Paths::Ini());
    const bool f = census && Install(0x0046D8E0, pF, 6, (void*)&DetF, &g_tF, "SpawnEntityParamsSpawn");
    const bool r = spawnsOwned ? false : Install(0x0079B8D0, pR, 5, (void*)&DetR, &g_tR, "SpawningRequestSpawn");
    const bool a = census && Install(0x005A8A80, pA, 6, (void*)&DetA, &g_tA, "WorldAddCore");
    const bool d = census && Install(0x005A8AC0, pD, 6, (void*)&DetD, &g_tD, "WorldRemoveCore");
    static const BYTE pFac[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };
    static const BYTE pSrc[7] = { 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x10 };
    const bool fac = Install(0x00F72ED0, pFac, 6, (void*)&DetFac, &g_tFac, "RequestFactory");
    const bool src = Install(0x0079C6C0, pSrc, 7, (void*)&DetSrc, &g_tSrc, "RequestSetSource");
    // sub_5BC470(client, req, entity), thiscall retn 8: the manager job stores the built entity into its request
    // (0x005C57A9 in sub_5C5770; the job object IS the request: 0x79B8D0 sets req+0x20 = 1, sub_5C5770 sets +0x20 = 2).
    static const BYTE pSet[7] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1 };
    const bool set = (g_keyBind || shareFlag || ssr || g_libFlag) && Install(0x005BC470, pSet, 7, (void*)&DetSet, &g_tSet, "RequestSetEntity");
    if (ssr) ShipSpawnReplaySetHooked(fac && src && set);
    // sub_501DF0 prologue (IDA): 55 push ebp / 8B EC mov ebp,esp / 51 push ecx / 53 push ebx / 56 push esi (6 bytes)
    static const BYTE pSpecDt[6] = { 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56 };
    const bool specDt = g_libFlag && Install(0x00501DF0, pSpecDt, 6, (void*)&DetSpecDtor, &g_tSpecDt, "SpecDtor");
    // sub_79C640 prologue (IDA): 55 push ebp / 8B EC mov ebp,esp / 51 push ecx / 56 push esi / 8B F1 mov esi,ecx (7 bytes)
    static const BYTE pReqDt[7] = { 0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1 };
    const bool reqDt = g_libFlag && Install(0x0079C640, pReqDt, 7, (void*)&DetReqDtor, &g_tReqDt, "RequestDtor");
    if (g_libFlag) { g_libFlag = fac && src && set && r && specDt && reqDt; SpawnLibSetHooked(g_libFlag); }
    g_keyBind = g_keyBind && r && fac && src && set;
    g_share = shareFlag && r && fac && src;   // the lane is checked at use: Net::Init runs after this init (run 2026-11-25 13:5x)
    { char rb[32]; GetPrivateProfileStringA("Coop", "SpawnShareRadiusM", "150", rb, sizeof(rb), Paths::Ini()); g_shareR = (float)atof(rb); }
    g_on = f || r || a || d || fac || src;
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] SpawnCensus on: F=%d R=%d A=%d D=%d fac=%d src=%d | SpawnShare=%d radius=%.0f SpawnKeyBind=%d\n", f, r, a, d, fac, src, g_share ? 1 : 0, g_shareR, g_keyBind ? 1 : 0);
    Phase1_LogLine(line);
}

// Called from the periodic report block; prints the full tables every 30 s.
int SpawnCensusReport(char* out, int cap)
{
    if (!g_on) return 0;
    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= s_next) {
        s_next = now + 30000;
        Dump("adds", g_adds, "pvt");
        Dump("funnel", g_funnel, "caller");
        Dump("reqs", g_reqs, "caller");
        Dump("dels", g_dels, "-");
    }
    return _snprintf_s(out, cap, _TRUNCATE, "[Census] totals adds=%ld dels=%ld funnel=%ld reqs=%ld | rows adds=%d(+%ld) funnel=%d reqs=%d dels=%d",
                       g_nAdd, g_nDel, g_nF, g_nR, g_adds.used, g_adds.overflow, g_funnel.used, g_reqs.used, g_dels.used);
}

static unsigned ReadVt(uintptr_t p) { __try { return p ? *(const unsigned*)p : 0u; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; } }
// vtable of a spec's FIRST SpawnParams (sub_4F33C0: u16 spec+0x32 count, params = **(spec+0x2C)); 0 if none / unreadable.
static unsigned FirstParamsVt(uintptr_t spec)
{
    __try { if (!spec || !*(const uint16_t*)(spec + 0x32)) return 0u; const uintptr_t a = *(const uintptr_t*)(spec + 0x2C); const uintptr_t p = a ? *(const uintptr_t*)a : 0; return p ? *(const unsigned*)p : 0u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
}

// One dword per word; a value that is a readable address whose first dword lies in the image is printed as value@vt.
static int DumpWords(char* out, size_t cap, uintptr_t base, int n)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t v = 0, vt = 0; bool okv = false, okt = false;
        __try { v = *(const uint32_t*)(base + 4 * i); okv = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!okv) { k += _snprintf_s(out + k, cap - k, _TRUNCATE, " ?"); continue; }
        if (v >= 0x10000 && (v & 3) == 0) { __try { vt = *(const uint32_t*)(uintptr_t)v; okt = vt >= 0x00400000 && vt < 0x05300000; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        const int w = okt ? _snprintf_s(out + k, cap - k, _TRUNCATE, " %X@%X", v, vt) : _snprintf_s(out + k, cap - k, _TRUNCATE, " %X", v);
        if (w <= 0) break;
        k += w;
    }
    return k;
}
// Predicted world spawn point of a request (static: sub_69D090, the position params' local->world, 2026-11-25):
//   pos = spec+0x28 (SpawnPositionStaticParams, vt 0x01E5F428); local xyz at pos+0x10; anchor handle at pos+0x30;
//   anchor entity e = *handle; if *(e+8) == 0 the frame is e's matrix at e+0x10 (rows at +0x10/+0x20/+0x30, translation
//   +0x40), else sub_6BC3A0(*(e+8)) (not read here: reported as frame=2). World = local.x*r0 + local.y*r1 + local.z*r2 + t.
// Lock-free reads only. Returns 0 no position params, 1 anchored world point, 2 frame not readable, 3 no anchor (local
// treated as world).
static int SpawnPoint(uintptr_t item, float* w, float* loc, uintptr_t* anc)
{
    w[0] = w[1] = w[2] = loc[0] = loc[1] = loc[2] = 0.f; *anc = 0;
    __try {
        const uintptr_t pos = *(const uintptr_t*)(item + 0x28);
        if (!pos || *(const uint32_t*)pos != 0x01E5F428u) return 0;
        const float* l = (const float*)(pos + 0x10); loc[0] = l[0]; loc[1] = l[1]; loc[2] = l[2];
        const uintptr_t hc = *(const uintptr_t*)(pos + 0x30);
        const uintptr_t e = hc ? *(const uintptr_t*)hc : 0;
        *anc = e;
        if (!e) { w[0] = loc[0]; w[1] = loc[1]; w[2] = loc[2]; return 3; }
        if (*(const uintptr_t*)(e + 8)) return 2;
        const float* m = (const float*)(e + 0x10);
        for (int j = 0; j < 3; ++j) w[j] = loc[0] * m[0 + j] + loc[1] * m[4 + j] + loc[2] * m[8 + j] + m[12 + j];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// Player-update thread (phase1.cpp OnFrameInner). Prints each newly captured request's key once (cap 400 lines):
// the item's own ObjectID (authored ids have hi != 0) and whether HandleContainer_GetObjectByID 0xA1F160 (stdcall
// (lo, hi) -> object) resolves it back to the same item here. [Coop] SpawnReplayFire (once a second; acts when the value
// changes to non-zero; [Coop] SpawnReplayFirePid limits it to one process): v % 10 = 1 newest ship item (vt 0x0270A538),
// 2 newest NPC item (vt 0x026FD610), 3 newest any; re-submits it through sub_569560(item, owner, ctx, id_lo, id_hi).
void SpawnReplayTick()
{
    if (!g_on || !g_tFac) return;
    { void SpawnShareTick(); SpawnShareTick(); }
    static ULONGLONG s_nextKey = 0, s_nextFire = 0;
    static LONG s_keyLines = 0, s_seen = 0;
    static uintptr_t s_items[512]; static int s_nItems = 0;
    static int s_last = 0;
    const ULONGLONG now = GetTickCount64();
    char line[300];
    if (now >= s_nextKey && s_keyLines < 400) {
        s_nextKey = now + 2000;
        const LONG pos = g_capPos;
        for (LONG i = s_seen; i < pos && s_keyLines < 400; ++i) {
            Cap& c = g_cap[i & (kCap - 1)];
            if (!c.req || c.printed) continue;
            c.printed = 1;
            bool dup = false; for (int k = 0; k < s_nItems; ++k) if (s_items[k] == c.item) { dup = true; break; }
            if (dup) continue;
            if (s_nItems < 512) s_items[s_nItems++] = c.item;
            // Differential key dump (2026-11-25): no engine calls here (lock-free reads only; see the deadlock note in
            // docs/evidence/spawn-manager-2026-11-25.md section 4). Both copies boot the same save; offline, find fields that
            // are equal across copies and unique per request: tools/oneoff/spawn_key_diff.py.
            const uintptr_t rq = (c.owner && ReadVt(c.owner - 0x38) == 0x026BDEE0u) ? c.owner - 0x38 : 0;
            static char big[6000];
            float w[3], lc[3]; uintptr_t anc = 0; const int fr = SpawnPoint(c.item, w, lc, &anc);
            { char kl[220]; _snprintf_s(kl, sizeof(kl), _TRUNCATE, "[K3] n=%ld ovt=%08X frame=%d w=(%.2f, %.2f, %.2f) local=(%.2f, %.2f, %.2f) anc=%08X avt=%08X apar=%08X\n",
                                        s_keyLines, ReadVt(c.owner), fr, w[0], w[1], w[2], lc[0], lc[1], lc[2], (unsigned)anc, ReadVt(anc), anc ? ReadVt(anc + 8) : 0u); Phase1_LogLine(kl); }
            int k = _snprintf_s(big, sizeof(big), _TRUNCATE, "[KD] n=%ld t=%llu item=%08X ivt=%08X owner=%08X ovt=%08X rq=%08X ctx=%08X | I:",
                                s_keyLines, now, (unsigned)c.item, (unsigned)c.itemVt, (unsigned)c.owner, ReadVt(c.owner), (unsigned)rq, (unsigned)c.ctx);
            if (k > 0) k += DumpWords(big + k, sizeof(big) - k, c.item, 32);
            if (k > 0) k += _snprintf_s(big + k, sizeof(big) - k, _TRUNCATE, " | R:");
            if (k > 0) k += DumpWords(big + k, sizeof(big) - k, rq ? rq : c.owner, rq ? 96 : 48);
            // One level deeper (run 2026-11-25 13:2x): spec+0x1C -> vt 0x01E56940, +0x24 -> SpawnStrategyParams 0x01E581A8,
            // +0x28 -> SpawnPositionStaticParams 0x01E5F428 (the authored spawn position is expected there: UNVERIFIED).
            static const int kSub[3] = { 0x1C, 0x24, 0x28 }; static const char* kTag[3] = { " | X:", " | S:", " | P:" };
            for (int s = 0; s < 3 && k > 0; ++s) {
                const uintptr_t sub = c.item ? (uintptr_t)ReadVt(c.item + kSub[s]) : 0;
                k += _snprintf_s(big + k, sizeof(big) - k, _TRUNCATE, "%s", kTag[s]);
                if (sub) k += DumpWords(big + k, sizeof(big) - k, sub, 28);
            }
            // L() truncates at 1024 chars (phase1.cpp:196): emit numbered parts "[KD+] n=<n> p=<i> <text>", joined offline.
            Phase1_LogLine("[KD] begin\n");
            for (int off = 0, part = 0; off < k; off += 900, ++part) {
                char seg[1000]; _snprintf_s(seg, sizeof(seg), _TRUNCATE, "[KD+] n=%ld p=%d %.900s\n", s_keyLines, part, big + off);
                Phase1_LogLine(seg);
            }
            ++s_keyLines;
            (void)line;        }
        s_seen = pos;
    }
    if (now < s_nextFire) return;
    s_nextFire = now + 1000;
    const int v = GetPrivateProfileIntA("Coop", "SpawnReplayFire", 0, Paths::Ini());
    if (v == s_last) return;
    s_last = v;
    if (!v) return;
    const int pid = GetPrivateProfileIntA("Coop", "SpawnReplayFirePid", 0, Paths::Ini());
    if (pid && (DWORD)pid != GetCurrentProcessId()) return;
    const int mode = v % 10;
    const uintptr_t want = mode == 1 ? 0x0270A538u : mode == 2 ? 0x026FD610u : 0u;
    const LONG pos = g_capPos;
    Cap pick = {}; bool have = false;
    for (int i = 0; i < kCap && !have; ++i) {
        const Cap& c = g_cap[(pos - 1 - i) & (kCap - 1)];
        // mode 1: the factory's item is the SPEC (vt 0x01E4A278), so a ship request is recognised by its first params'
        // vtable (design 2026-11-27 s2.1), not by the item's own vtable (the old test never matched).
        if (c.req && c.owner && (!want || (mode == 1 ? FirstParamsVt(c.item) == want : c.itemVt == want))) { pick.ctx = c.ctx; pick.item = c.item; pick.itemVt = c.itemVt; pick.owner = c.owner; pick.idLo = c.idLo; pick.idHi = c.idHi; pick.t = c.t; have = true; }
    }
    if (!have) { Phase1_LogLine("[Replay] fire: no captured request of that kind\n"); return; }
    char ok = 0; DWORD code = 0;
    InterlockedExchange(&g_watch, 3);
    __try {
        if (*(const uintptr_t*)pick.item != pick.itemVt) { Phase1_LogLine("[Replay] fire: item vtable changed - skipped\n"); return; }
        ok = ((char(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t))0x00569560)(pick.item, pick.owner, pick.ctx, pick.idLo, pick.idHi);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[Replay] fire v=%d item=%08X ivt=%08X owner=%08X ctx=%08X id=%08X:%08X captured %llums ago -> submitted=%d exc=%08X\n",
                v, (unsigned)pick.item, (unsigned)pick.itemVt, (unsigned)pick.owner, (unsigned)pick.ctx, pick.idHi, pick.idLo, now - pick.t, (int)ok, (unsigned)code);
    Phase1_LogLine(line);
}
// ==== [Coop] SpawnShare (default 0): Storm-style spawner-keyed spawning, slice 1 (static anchors) ====================
// Design: docs/design/host-spawn-authority.md; key evidence: docs/evidence/spawn-manager-2026-11-25.md section 5.
// Key = the request's predicted world spawn point (SpawnPoint frame 1, anchor class Entity 0x01E4CE90), on a 0.5 m grid:
// measured equal on two copies of the same save for 68/69 such requests and unique per request (max reuse 1).
// Host (EntityHost=1): every such request it submits is published on IC_SPAWN, wireId = key (the lane persists the last
//   value per key and replays it to a (re)joining guest = the connect snapshot).
// Guest: while the host's player state is fresh (< 3 s) and the spawn point is within [Coop] SpawnShareRadiusM (150) of
//   the host's player, its own matching request is HELD (0x79B8D0 declined) until the host's key arrives; then it is
//   re-submitted through sub_569560(item, owner, ctx, id) - the replay measured working 2026-11-25 (NPC, n=1). A request
//   whose key the host already published passes at once. Ship-anchored and position-less (random sea traffic) requests
//   are NOT handled in this slice: they pass unchanged and are counted.
// UNVERIFIED: that the requester re-requests after a decline (only matters for keys the host never sends: expiry count).
void Phase1_LogLine(const char* s);
bool Phase1_EntityHost();                 // phase1.cpp: [Coop] EntityHost == 1
bool Phase1_HostPos(float* out);          // phase1.cpp: the peer's player position, fresh (< 3 s)
bool Phase1_InSave();                     // phase1.cpp: this player's update ran in world < 1 s ago (lock-free)
namespace {
volatile LONG g_shLock = 0;
struct ShSpin { ShSpin() { while (InterlockedExchange(&g_shLock, 1)) YieldProcessor(); } ~ShSpin() { InterlockedExchange(&g_shLock, 0); } };
struct Held { uint64_t key; uintptr_t item, owner, ctx; uint32_t lo, hi; ULONGLONG t; uint32_t itemVt, ownerVt; float w[3]; int pending; };
constexpr int kHeld = 512, kRel = 2048, kPub = 256;
Held g_held[kHeld]; int g_nHeld = 0;
uint64_t g_rel[kRel]; ULONGLONG g_relT[kRel]; int g_nRel = 0;           // keys the host spawned (guest side)
struct Pub { uint64_t key; float w[3]; uint32_t ivt; };
Pub g_pub[kPub]; volatile LONG g_pubW = 0, g_pubR = 0;                  // host side: ring, published on the player thread
volatile LONG c_pub = 0, c_held = 0, c_relArr = 0, c_relReq = 0, c_passShip = 0, c_passNone = 0, c_passOut = 0,
              c_passNoHost = 0, c_expired = 0, c_rx = 0, c_replayFail = 0, c_pubDrop = 0, c_pending = 0, c_lateLocal = 0,
              c_fallback = 0, c_stale = 0;
__declspec(thread) int t_replay = 0;

uint64_t KeyOf(const float* w)
{
    const int32_t q[3] = { (int32_t)floorf(w[0] * 2.f), (int32_t)floorf(w[1] * 2.f), (int32_t)floorf(w[2] * 2.f) };
    return 0x5300000000000000ull | ((uint64_t)(q[0] & 0xFFFFF) << 40) | ((uint64_t)(q[1] & 0xFFFFF) << 20) | (uint64_t)(q[2] & 0xFFFFF);
}
bool RelHas(uint64_t k) { for (int i = 0; i < g_nRel; ++i) if (g_rel[i] == k) return true; return false; }
void RelAdd(uint64_t k, ULONGLONG now)
{
    for (int i = 0; i < g_nRel; ++i) if (g_rel[i] == k) { g_relT[i] = now; return; }
    if (g_nRel < kRel) { g_rel[g_nRel] = k; g_relT[g_nRel++] = now; }
}
bool CapFor(uintptr_t req, Cap& out)
{
    const LONG pos = g_capPos;
    for (int i = 0; i < 64; ++i) {
        const Cap& c = g_cap[(pos - 1 - i) & (kCap - 1)];
        if (c.req == req) { out.req = c.req; out.ctx = c.ctx; out.item = c.item; out.itemVt = c.itemVt; out.owner = c.owner;
                            out.idLo = c.idLo; out.idHi = c.idHi; out.t = c.t; return true; }
    }
    return false;
}
// 0 = not static-keyed, 1 = static key computed.
int StaticKey(uintptr_t item, uint64_t* key, float* w)
{
    float loc[3]; uintptr_t anc = 0;
    if (SpawnPoint(item, w, loc, &anc) != 1) return 0;
    if (ReadVt(anc) != 0x01E4CE90u) return 0;
    *key = KeyOf(w);
    return 1;
}
}  // namespace

void SpawnShareReplayTag(int v) { t_replay = v; }   // [Coop] ShipSpawnReplay: our submit passes SpawnShareDecide
// [Coop] ShipSpawnReplay: the ctx argument DetFac recorded for this request (whole 1024 ring, newest first).
bool SpawnCensusCtxOf(uintptr_t req, uintptr_t* ctx)
{
    const LONG pos = g_capPos;
    for (int i = 0; i < kCap; ++i) { const Cap& c = g_cap[(pos - 1 - i) & (kCap - 1)]; if (c.req == req) { *ctx = c.ctx; return true; } }
    return false;
}
// Called from DetR (any game thread) for every request submitted. 1 = hold (decline), 0 = let it through.
int SpawnShareDecide(uintptr_t req)
{
    if (!g_share || t_replay || !Net::IntentLaneOn()) return 0;
    Cap c = {}; if (!CapFor(req, c) || !c.item || !c.owner) return 0;
    uint64_t key = 0; float w[3] = {};
    if (!StaticKey(c.item, &key, w)) {
        float loc[3]; uintptr_t anc = 0;
        const int fr = SpawnPoint(c.item, w, loc, &anc);
        InterlockedIncrement(fr == 0 ? &c_passNone : &c_passShip);
        return 0;
    }
    if (Phase1_EntityHost()) {   // host: publish, never hold
        const LONG wi = InterlockedIncrement(&g_pubW) - 1;
        if (wi - g_pubR >= kPub) { InterlockedIncrement(&c_pubDrop); return 0; }
        Pub& p = g_pub[wi & (kPub - 1)]; p.key = key; p.w[0] = w[0]; p.w[1] = w[1]; p.w[2] = w[2]; p.ivt = (uint32_t)c.itemVt;
        return 0;
    }
    // Run 2026-11-25 14:0x: boot and fast-travel spawns all happen while neither side sends player state (loading), so
    // "host position fresh" missed every one of them (noHost 93/93). Now: unknown host position -> hold as PENDING and
    // decide in SpawnShareTick once the host's position is fresh (outside the radius -> replay locally).
    // Run 2026-11-25 14:1x: a request held during the guest's BOOT froze the load (frame 10 for 6 min, reqs=1): the loader
    // waits for it and the tick that would release it does not run while loading. Hold only while the guest's player
    // update runs in world (Phase1_InSave: lock-free, < 1 s old). Load-time spawns pass; reconciling them is the next slice.
    if (!Phase1_InSave()) { InterlockedIncrement(&c_passNoHost); return 0; }
    float hp[3];
    const bool fresh = Phase1_HostPos(hp);
    if (fresh) {
        const float dx = w[0] - hp[0], dy = w[1] - hp[1], dz = w[2] - hp[2];
        if (dx * dx + dy * dy + dz * dz > g_shareR * g_shareR) { InterlockedIncrement(&c_passOut); return 0; }
    }
    ShSpin s;
    if (RelHas(key)) { InterlockedIncrement(&c_relReq); return 0; }
    int slot = -1;
    for (int i = 0; i < g_nHeld; ++i) if (g_held[i].key == key) { slot = i; break; }
    if (slot < 0 && g_nHeld < kHeld) slot = g_nHeld++;
    if (slot < 0) return 0;   // table full: let it through (fail open)
    g_held[slot] = { key, c.item, c.owner, c.ctx, c.idLo, c.idHi, GetTickCount64(), (uint32_t)c.itemVt, ReadVt(c.owner), { w[0], w[1], w[2] }, fresh ? 0 : 1 };
    InterlockedIncrement(fresh ? &c_held : &c_pending);
    return 1;
}

// Re-submit a held request through sub_569560. Refuses (returns false, nothing called) when the spec or the owner no
// longer has the vtable it had when held: a travel unloads the requesters and the pointers may be freed - calling into
// the engine with a freed object could fault inside an engine lock (see the 0xA1F160 deadlock note).
static bool ReplayHeld(const Held& h)
{
    if (ReadVt(h.item) != h.itemVt || ReadVt(h.owner) != h.ownerVt) return false;
    char ok = 0;
    t_replay = 1;
    __try { ok = ((char(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t))0x00569560)(h.item, h.owner, h.ctx, h.lo, h.hi); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    t_replay = 0;
    return ok != 0;
}

static bool TakeHeld(uint64_t key, Held* h)   // marks the key released; takes its held request if any
{
    ShSpin s;
    RelAdd(key, GetTickCount64());
    for (int i = 0; i < g_nHeld; ++i) if (g_held[i].key == key) { *h = g_held[i]; g_held[i] = g_held[--g_nHeld]; return true; }
    return false;
}

// Guest: one IC_SPAWN message (player thread, from FightIntentDrain).
void SpawnKeyRxBind(uint64_t hostObjId, uint64_t key);   // below
void SpawnShareRx(uint64_t key, const uint8_t* payload, uint8_t len)
{
    if (len >= 16 && payload[0] == 2 && payload[1] == 2) { uint64_t k; memcpy(&k, payload + 8, 8); SpawnKeyRxBind(key, k); return; }
    if (!g_share) return;
    InterlockedIncrement(&c_rx);
    Held h = {};
    if (!TakeHeld(key, &h)) return;
    InterlockedIncrement(ReplayHeld(h) ? &c_relArr : &c_replayFail);
}
bool SpawnShareOn() { return g_share || g_keyBind; }   // keeps phase1's lane drain running for IC_SPAWN

// Player thread: host publishes queued keys; guest expires held requests (60 s); stats every 10 s.
void SpawnShareTick()
{
    if (!g_share || !Net::IntentLaneOn()) return;
    const ULONGLONG now = GetTickCount64();
    while (g_pubR < g_pubW) {
        const Pub p = g_pub[g_pubR & (kPub - 1)];
        struct { uint8_t ver, kind; uint16_t pad; float w[3]; uint32_t ivt; } pl = { 1, 1, 0, { p.w[0], p.w[1], p.w[2] }, p.ivt };
        if (Net::PublishIntent(0, p.key, 15 /* IC_SPAWN */, 0, &pl, (uint8_t)sizeof(pl))) InterlockedIncrement(&c_pub);
        else InterlockedIncrement(&c_pubDrop);
        InterlockedIncrement(&g_pubR);
    }
    // Guest: decide pending holds once the host's position is fresh; replay locally what lies outside the radius, and
    // as a fallback what waited 60 s for a key that never came (no permanently missing NPCs). Replays run outside the
    // lock (a replay re-enters DetR; t_replay makes it pass without locking).
    Held todo[64]; int nTodo = 0; int why[64];
    {
        float hp[3]; const bool fresh = Phase1_HostPos(hp);
        ShSpin s;
        for (int i = 0; i < g_nHeld && nTodo < 64; ) {
            Held& h = g_held[i];
            int act = 0;
            if (now - h.t > 60000) act = 2;
            else if (h.pending && fresh) {
                const float dx = h.w[0] - hp[0], dy = h.w[1] - hp[1], dz = h.w[2] - hp[2];
                if (dx * dx + dy * dy + dz * dz > g_shareR * g_shareR) act = 1; else h.pending = 0;
            }
            if (act) { why[nTodo] = act; todo[nTodo++] = h; g_held[i] = g_held[--g_nHeld]; } else ++i;
        }
    }
    for (int i = 0; i < nTodo; ++i) InterlockedIncrement(ReplayHeld(todo[i]) ? (why[i] == 1 ? &c_lateLocal : &c_fallback) : &c_stale);
    static ULONGLONG s_next = 0;
    if (now < s_next) return;
    s_next = now + 10000;
    char line[320];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[Share] role=%s pub=%ld drop=%ld | rx=%ld held=%ld pending=%ld heldNow=%d relOnArrival=%ld relOnRequest=%ld lateLocal=%ld fallback=%ld stale=%ld replayFail=%ld | pass ship=%ld none=%ld outside=%ld loading=%ld rel=%d\n",
                Phase1_EntityHost() ? "host" : "guest", c_pub, c_pubDrop, c_rx, c_held, c_pending, g_nHeld, c_relArr, c_relReq, c_lateLocal,
                c_fallback, c_stale, c_replayFail, c_passShip, c_passNone, c_passOut, c_passNoHost, g_nRel);
    Phase1_LogLine(line);
}
// ==== [Coop] SpawnKeyBind (default 0): bind host guards to guest bodies by spawn key ==================================
// Evidence: docs/evidence/spawn-manager-2026-11-25.md sections 5-6. Both games spawn the same static-anchored NPC at the
// same world point W (68/69) during loads; their ids are minted per process. Each side records, for every entity the
// manager builds, the key of the request that built it (DetR: req -> key, DetSet/sub_5BC470: entity <- req). The host
// publishes its guard's objId -> key once on IC_SPAWN (payload ver 2, kind 2, key at +8); the guest binds that objId to
// its own entity built for the same key (phase1.cpp WideBindPrepass, key round before the distance rounds).
// Static anchors only (ship-anchored crews and random traffic have no shared key yet). Lock-free reads only.
namespace {
struct RK { volatile uintptr_t req; uint64_t key; int kind; uintptr_t anc; uint32_t avt; float loc[3]; };   // kind: SpawnPoint frame (1 static, 0 none, 2 parent, 3 no anchor), 4 = frame 1 with a non-Entity anchor
struct EK { volatile uintptr_t ent; uint64_t key; int kind; uintptr_t anc; uint32_t avt; float loc[3]; };
constexpr int kRK = 2048, kEK = 2048, kHK = 1024, kHP = 2048;
RK g_rk[kRK]; volatile LONG g_rkPos = 0;
EK g_ek[kEK]; volatile LONG g_ekPos = 0;
struct HK { uint64_t obj, key; }; HK g_hk[kHK]; volatile LONG g_hkPos = 0;      // guest: host objId -> key
uint64_t g_hostPub[kHP]; int g_hostPubN = 0;                                     // host: objIds already published
volatile LONG k_req = 0, k_ent = 0, k_entNoKey = 0, k_pub = 0, k_rx = 0, k_hostNoKey = 0, k_hostKeyed = 0, k_hostCrew = 0, k_crewResolved = 0, k_crewNoShip = 0, k_hostJackdawCrew = 0;
}
void SpawnKeyNoteReq(uintptr_t req)
{
    if (!g_keyBind && !g_share && !g_libFlag) return;
    Cap c = {}; if (!CapFor(req, c) || !c.item) return;
    uint64_t key = 0; float w[3], loc[3]; uintptr_t anc = 0;
    int kind = SpawnPoint(c.item, w, loc, &anc);
    const uint32_t avt = ReadVt(anc);
    if (kind == 1 && avt == 0x01E4CE90u) key = KeyOf(w); else if (kind == 1) kind = 4;
    RK& r = g_rk[(InterlockedIncrement(&g_rkPos) - 1) & (kRK - 1)]; r.req = 0; r.key = key; r.kind = kind; r.anc = anc; r.avt = avt; r.loc[0] = loc[0]; r.loc[1] = loc[1]; r.loc[2] = loc[2]; r.req = req;
    InterlockedIncrement(&k_req);
    if (g_libFlag) { void SpawnLibNoteSubmit(uintptr_t, uintptr_t, uintptr_t, uint64_t); SpawnLibNoteSubmit(c.item, c.owner, c.ctx, key); }   // [Coop] SpawnLib
}
extern "C" void __cdecl SpawnKeyNoteEnt(const uint32_t* esp)   // esp[1] = request, esp[2] = entity
{
    { void ShipReplayNoteEnt(uintptr_t, uintptr_t); ShipReplayNoteEnt(esp[1], esp[2]); }   // [Coop] ShipSpawnReplay capture (lock-free)
    if (g_libFlag) { void SpawnLibNoteBuilt(uintptr_t, uintptr_t); SpawnLibNoteBuilt(esp[1], esp[2]); }   // [Coop] SpawnLib (lock-free reads)
    if (!g_keyBind) return;
    const uintptr_t req = esp[1], ent = esp[2];
    if (!req || !ent) return;
    const LONG pos = g_rkPos;
    for (int i = 0; i < kRK; ++i) {
        const RK& r = g_rk[(pos - 1 - i) & (kRK - 1)];
        if (r.req == req) {
            EK& e = g_ek[(InterlockedIncrement(&g_ekPos) - 1) & (kEK - 1)]; e.ent = 0; e.key = r.key; e.kind = r.kind; e.anc = r.anc; e.avt = r.avt; e.loc[0] = r.loc[0]; e.loc[1] = r.loc[1]; e.loc[2] = r.loc[2]; e.ent = ent;
            InterlockedIncrement(r.key ? &k_ent : &k_entNoKey); return;
        }
    }
    InterlockedIncrement(&k_entNoKey);
}
bool SpawnKeyBindOn() { return g_keyBind; }
uintptr_t Phase1_ScbGuestOfLo(uint32_t hostLo);    // phase1.cpp
uint64_t Phase1_ScbHostOf(uintptr_t guestShip);   // phase1.cpp [Coop] ShipClassBind: host ship objId bound to this guest ship, 0 = none
// Crew keys (2026-11-25): a crew member's spawn spec is anchored to its ship (kind 4, anchor vt EntityGroup 0x01E4A128)
// with a local offset that is equal on two copies (s5: 71/71). Key = (ship objId lo32 << 32) | local offset on a 0.1 m
// grid (11/11/10 bits). Ship objId = the ship's HandleContainer id: hc = *(entity+0xC8) (sub_109B500 reads the same
// +200), id at hc+0x0C (BaseTypes.h). Minted ship ids are 0xF0000000..0xF7FFFFFF, so a crew key's top nibble is F;
// static keys' is 5. On the guest the ship part is translated through the ShipClassBind table.
static uint32_t CrewQ(const float* loc)
{
    const int32_t qx = (int32_t)floorf(loc[0] * 10.f + 0.5f), qy = (int32_t)floorf(loc[1] * 10.f + 0.5f), qz = (int32_t)floorf(loc[2] * 10.f + 0.5f);
    return ((uint32_t)(qx & 0x7FF) << 21) | ((uint32_t)(qy & 0x7FF) << 10) | (uint32_t)(qz & 0x3FF);
}
// Ship class as ShipClassBind/ReadShipClass decode entity+0x9C (class 7 = Jackdaw); 0 = not a ship class word.
static uint32_t ShipClassOf(uintptr_t ship)
{
    __try { const uint32_t f = *(const uint32_t*)(ship + 0x9C); return ((f & 7u) == 3u && (f & 0x1F8u) == 0xE8u) ? (f >> 9) & 0x3Fu : 0u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uint64_t ShipIdOf(uintptr_t ship)
{
    __try { const uintptr_t hc = *(const uintptr_t*)(ship + 0xC8); return hc ? *(const uint64_t*)(hc + 0x0C) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint64_t SpawnKeyOfEntity(uintptr_t ent)
{
    const LONG pos = g_ekPos;
    for (int i = 0; i < kEK; ++i) { const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)]; if (e.ent == ent) return e.key; }
    return 0;
}
uintptr_t SpawnEntityOfKey(uint64_t key)
{
    const LONG pos = g_ekPos;
    if ((key >> 60) == 0xF) {   // crew key: this guest's crew member on the ship bound to that host ship, same offset
        const uint32_t shipLo = (uint32_t)(key >> 32), q = (uint32_t)key;
        for (int i = 0; i < kEK; ++i) {
            const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)];
            if (!e.ent || (e.kind != 4 && e.kind != 2) || e.avt != 0x01E4A128u || CrewQ(e.loc) != q) continue;
            if ((uint32_t)Phase1_ScbHostOf(e.anc) == shipLo) { InterlockedIncrement(&k_crewResolved); return e.ent; }
        }
        InterlockedIncrement(&k_crewNoShip);
        {   // diagnostics: is the host ship bound, and which crew offsets does our bound ship have? Up to 4 lines per host
            // ship, 24 ships (run T8: the old global cap of 40 lines was spent on one docked ship that was never bound).
            static struct { uint32_t ship; LONG n; } s_cs[24]; static LONG s_csN = 0;
            int ci = -1;
            for (int i = 0; i < s_csN && i < 24; ++i) if (s_cs[i].ship == shipLo) { ci = i; break; }
            if (ci < 0 && s_csN < 24) { ci = InterlockedIncrement(&s_csN) - 1; if (ci < 24) { s_cs[ci].ship = shipLo; s_cs[ci].n = 0; } else ci = -1; }
            if (ci >= 0 && InterlockedIncrement(&s_cs[ci].n) <= 4) {
                const uintptr_t gs = Phase1_ScbGuestOfLo(shipLo);
                int nOn = 0; char qs[160]; int k = 0; qs[0] = 0;
                if (gs) for (int i = 0; i < kEK; ++i) { const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)]; if (e.ent && e.anc == gs) { ++nOn; if (k < 140) k += _snprintf_s(qs + k, sizeof(qs) - k, _TRUNCATE, " %08X/k%d", CrewQ(e.loc), e.kind); } }
                char line[320]; _snprintf_s(line, sizeof(line), _TRUNCATE, "[KeyBind] crew miss hostShip=%08X q=%08X boundGuestShip=%08X guestSpawnsOnIt=%d:%s\n", shipLo, q, (unsigned)gs, nOn, qs);
                Phase1_LogLine(line);
            }
        }
        return 0;
    }
    for (int i = 0; i < kEK; ++i) { const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)]; if (e.ent && e.key == key) return e.ent; }
    return 0;
}
// spawn_lib.cpp crew diagnostics: built entities whose spawn anchor is this ship (EK ring, newest first, de-duplicated).
int SpawnCrewOfShip(uintptr_t ship, uintptr_t* out, int* kinds, float* loc3, int cap)
{
    const LONG pos = g_ekPos; int n = 0;
    for (int i = 0; i < kEK && n < cap; ++i) {
        const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)];
        if (!e.ent || e.anc != ship) continue;
        bool dup = false; for (int j = 0; j < n; ++j) if (out[j] == e.ent) { dup = true; break; }
        if (!dup) { out[n] = e.ent; kinds[n] = e.kind; loc3[3 * n] = e.loc[0]; loc3[3 * n + 1] = e.loc[1]; loc3[3 * n + 2] = e.loc[2]; ++n; }
    }
    return n;
}
// spawn_lib.cpp crew census: distinct ship anchors (EntityGroup vt 0x01E4A128) of built entities in the EK ring.
int SpawnCrewAnchors(uintptr_t* out, int cap)
{
    const LONG pos = g_ekPos; int n = 0;
    for (int i = 0; i < kEK && n < cap; ++i) {
        const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)];
        if (!e.ent || !e.anc || e.avt != 0x01E4A128u) continue;
        bool dup = false; for (int j = 0; j < n; ++j) if (out[j] == e.anc) { dup = true; break; }
        if (!dup) out[n++] = e.anc;
    }
    return n;
}
uint64_t SpawnKeyForHostObj(uint64_t obj)
{
    const LONG pos = g_hkPos;
    for (int i = 0; i < kHK; ++i) { const HK& h = g_hk[(pos - 1 - i) & (kHK - 1)]; if (h.obj == obj) return h.key; }
    return 0;
}
void SpawnKeyRxBind(uint64_t hostObjId, uint64_t key)   // guest, player thread (lane drain)
{
    InterlockedIncrement(&k_rx);
    for (int i = 0; i < kHK; ++i) if (g_hk[i].obj == hostObjId) { g_hk[i].key = key; return; }
    HK& h = g_hk[(InterlockedIncrement(&g_hkPos) - 1) & (kHK - 1)]; h.obj = hostObjId; h.key = key;
}
// Host, from EmitGuardEntity (player thread): publish objId -> key once per objId.
void SpawnKeyHostNote(uint64_t objId, uintptr_t ent)
{
    if (!g_keyBind || !objId || !Phase1_EntityHost() || !Net::IntentLaneOn()) return;
    for (int i = 0; i < g_hostPubN; ++i) if (g_hostPub[i] == objId) return;
    const uint64_t key = SpawnKeyOfEntity(ent);
    if (!key) {   // no static key: a crew member gets a crew key (ship id + local offset); counted once per objId
        int kind = -1; uintptr_t anc = 0; uint32_t avt = 0; float loc[3] = {};
        { const LONG pos = g_ekPos; for (int i = 0; i < kEK; ++i) { const EK& e = g_ek[(pos - 1 - i) & (kEK - 1)]; if (e.ent == ent) { kind = e.kind; anc = e.anc; avt = e.avt; loc[0] = e.loc[0]; loc[1] = e.loc[1]; loc[2] = e.loc[2]; break; } } }
        // kind 2 = the anchor ship has a parent (*(e+8) != 0; run 2026-11-25: the other 20/20 unkeyed host guards): the local
        // offset and the ship are still known, so they get a crew key too.
        const uint64_t sid = ((kind == 4 || kind == 2) && avt == 0x01E4A128u) ? ShipIdOf(anc) : 0;
        // A Jackdaw's crew (ship class 7, entity+0x9C as ShipClassBind reads it) can never resolve on the guest: Jackdaws
        // are excluded from ShipClassBind (each player keeps their own). Runs T9-T12 2026-11-28: the only published crew
        // keys were the host Jackdaw's 20 (ship[0], 15 m from the player), all crewNoShip on the guest.
        if (sid && ShipClassOf(anc) == 7) { InterlockedIncrement(&k_hostJackdawCrew); if (g_hostPubN < kHP) g_hostPub[g_hostPubN++] = objId; return; }
        if (sid &&(sid >> 32) == 0 && ((uint32_t)sid >> 28) == 0xF) {   // minted ship id (the player's own ship has an authored id: skipped)
            const uint64_t ck = ((uint64_t)(uint32_t)sid << 32) | CrewQ(loc);
            uint8_t pl[16] = { 2, 2 }; memcpy(pl + 8, &ck, 8);
            if (Net::PublishIntent(0, objId, 15 /* IC_SPAWN */, 0, pl, (uint8_t)sizeof(pl))) {
                InterlockedIncrement(&k_pub); InterlockedIncrement(&k_hostCrew);
                if (g_hostPubN < kHP) g_hostPub[g_hostPubN++] = objId;
                static uint32_t s_ships[32]; static int s_shipsN = 0; bool seen = false;   // one line per crew-carrying host ship
                for (int i = 0; i < s_shipsN; ++i) if (s_ships[i] == (uint32_t)sid) { seen = true; break; }
                if (!seen && s_shipsN < 32) {
                    s_ships[s_shipsN++] = (uint32_t)sid; char line[160];
                    _snprintf_s(line, sizeof(line), _TRUNCATE, "[KeyBind] host crew ship=%08X shipEnt=%08X first crew obj=%08X q=%08X\n", (unsigned)sid, (unsigned)anc, (unsigned)objId, CrewQ(loc));
                    Phase1_LogLine(line);
                }
            }
            return;
        }
        InterlockedIncrement(&k_hostNoKey); if (g_hostPubN < kHP) g_hostPub[g_hostPubN++] = objId;
        static LONG s_un = 0;
        if (InterlockedIncrement(&s_un) <= 40) {
            float p[3]; Pos(ent, p); char line[200];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[KeyBind] host unkeyed guard obj=%08X ent=%08X kind=%d anchor=%08X avt=%08X pos=(%.0f, %.0f, %.0f)\n",
                        (unsigned)objId, (unsigned)ent, kind, (unsigned)anc, avt, p[0], p[1], p[2]);
            Phase1_LogLine(line);
        }
        return;
    }
    InterlockedIncrement(&k_hostKeyed);
    uint8_t pl[16] = { 2, 2 }; memcpy(pl + 8, &key, 8);
    if (Net::PublishIntent(0, objId, 15 /* IC_SPAWN */, 0, pl, (uint8_t)sizeof(pl))) {
        InterlockedIncrement(&k_pub);
        if (g_hostPubN < kHP) g_hostPub[g_hostPubN++] = objId;
    }
}
int SpawnKeyReport(char* out, int cap)
{
    if (!g_keyBind) return 0;
    return _snprintf_s(out, cap, _TRUNCATE, "[KeyBind] reqKeys=%ld entKeys=%ld entNoKey=%ld | host guards keyed=%ld crew=%ld jackdawCrewSkipped=%ld unkeyed=%ld published=%ld | guestReceived=%ld crewResolved=%ld crewNoShip=%ld",
                       k_req, k_ent, k_entNoKey, k_hostKeyed, k_hostCrew, k_hostJackdawCrew, k_hostNoKey, k_pub, k_rx, k_crewResolved, k_crewNoShip);
}
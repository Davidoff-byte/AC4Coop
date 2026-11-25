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
#include "Scimitar/Entity.h"
#include "Scimitar/AI/AIEntityHelper.h"

#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <intrin.h>

void Phase1_LogLine(const char* s);

namespace {
uintptr_t g_lo = 0, g_hi = 0;
bool g_on = false;
DWORD g_tF = 0, g_tR = 0, g_tA = 0, g_tD = 0;
volatile LONG g_lock = 0, g_lines = 0;

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
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return e;
}
__declspec(naked) void DetR()
{
    __asm { pushad
            lea eax, [esp + 32]
            push eax
            push ecx
            call CensusReq
            add esp, 8
            popad
            jmp dword ptr [g_tR] }
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
    if (GetPrivateProfileIntA("Coop", "SpawnCensus", 0, Paths::Ini()) == 0) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)GameModuleBase;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(GameModuleBase + dos->e_lfanew);
    g_lo = GameModuleBase + nt->OptionalHeader.BaseOfCode;
    g_hi = g_lo + nt->OptionalHeader.SizeOfCode;
    static const BYTE pF[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
    static const BYTE pR[5] = { 0x56, 0x8B, 0xF1, 0x8B, 0x06 };
    static const BYTE pA[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10 };
    static const BYTE pD[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C };
    const bool spawnsOwned = GetPrivateProfileIntA("Coop", "DisableSpawns", 0, Paths::Ini()) || GetPrivateProfileIntA("Coop", "DisableCrowd", 0, Paths::Ini());
    const bool f = Install(0x0046D8E0, pF, 6, (void*)&DetF, &g_tF, "SpawnEntityParamsSpawn");
    const bool r = spawnsOwned ? false : Install(0x0079B8D0, pR, 5, (void*)&DetR, &g_tR, "SpawningRequestSpawn");
    const bool a = Install(0x005A8A80, pA, 6, (void*)&DetA, &g_tA, "WorldAddCore");
    const bool d = Install(0x005A8AC0, pD, 6, (void*)&DetD, &g_tD, "WorldRemoveCore");
    g_on = f || r || a || d;
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[Census] SpawnCensus on: F=%d R=%d A=%d D=%d\n", f, r, a, d);
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

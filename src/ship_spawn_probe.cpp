// [Coop] ShipSpawnProbe (default 0, read at load, read-only): who CREATES the ships that enter the naval list.
// Static trail (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, docs/evidence/naval-s0-2026-11-25.md "spawner"):
//   RegisterShip 0x0109B500  thiscall(NavalManager* mgr, Entity* ship), retn 4 - the ONLY increment of the naval list count
//                            [mgr+0x3EE] (0x0109B607). Single caller 0x012350A0, reached only from the thunk 0x01236130 in
//                            vtable slot 8 of the state class vt 0x026A15F0 (ctor 0x011FCF70, no code xrefs). So the
//                            register runs on a state entry, after the ship exists: its own stack does not name the creator.
//   Ship entity = EntityGroup (vt 0x01E4A128) built by ctor 0x00503330 (called from 0x006DEFA0 = WorldEntityGroup
//                            0x01E64680, and jumped to from 0x00504880). Neither wrapper has a code xref: they are class
//                            descriptor create functions (reflection), so the caller of the factory is runtime-only.
// The probe: a ring of creation stacks from the EntityGroup ctor (hooked at 0x00503338, after the base call, bytes
// B8 01 00 00 00 = mov eax,1; esi = this). On every RegisterShip it prints the registered entity's creation stack from
// the ring, plus the register's own stack. Stacks are raw scans: dwords in the code range whose preceding bytes are a
// call (E8 rel32 into the image, FF /2 reg, [reg+d8], [reg+d32] / [abs]). Raw scans include stale frames; read them
// against the decompile, not as an exact chain.
//
// Live result 2026-11-25 (build F77EC8AA, pid 29356): 21/21 registrations matched; every natural ship was created by
//   SpawningManager job sub_455310 -> ... sub_5B06E0 -> sub_45E330 (SpawnParams::Spawn wrapper) -> params vt[31]
//   (SpawnShipParams vt 0x0270A538 slot 31 = sub_1996AB0; a subclass slot at 0x026A2144 = sub_122BD30 wraps it)
//   -> sub_46D8E0 -> builder vt[0x44] (sub_6060C0, builder vt 0x01E577E0) -> sub_5FD730 -> template vt[3](0,0)
//   (Duplicate) -> sub_717C30 -> sub_A5B4F0 -> sub_503600 -> sub_A38120 -> EntityGroup ctor.
// Spawn capture + test (also under ShipSpawnProbe):
//   SpawnShipParams::Spawn 0x01996AB0 thiscall(params, int a2), retn 4 (callers: sub_45E330 at 0x0045E34C `call edx`
//   with ecx = params; sub_122BD30). Hooked at entry (55 8B EC 83 EC 18, 6 bytes, no relative operand) to log every
//   ship spawn with its params object and keep the newest params.
//   [Coop] ShipSpawnFire (read once a second on the player-update thread; acts when the value CHANGES to non-zero):
//     value % 10 == 1: e = sub_45E330(newest params, params->vt[0x50](params)) - the engine's own call (0x005B0700..0x005B070A)
//     value % 10 == 2: the same, then World::AddEntity(e) (0x0070E860; the proven NPC route adds the built body this way)
//   UNVERIFIED: that re-spawning from a params object that already spawned a live ship is safe (sub_469D10 resets the
//   params' selector state after each spawn unless flag +0x94 & 1). This is the test.
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void Phase1_LogLine(const char* s);

namespace {
constexpr int kFrames = 18;
constexpr int kRing = 2048;
struct Rec { volatile uintptr_t obj; ULONGLONG t; int n; uint32_t f[kFrames]; };
Rec g_ring[kRing];
volatile LONG g_ringPos = 0, g_ctors = 0, g_regs = 0, g_matched = 0, g_lines = 0;
DWORD g_tCtor = 0, g_tReg = 0;
uintptr_t g_codeLo = 0, g_codeHi = 0;
bool g_on = false;

bool IsRet(uint32_t v)
{
    if (v < g_codeLo + 6 || v >= g_codeHi) return false;
    const BYTE* p = (const BYTE*)(uintptr_t)v;
    if (p[-5] == 0xE8) {
        const uint32_t tgt = v + *(const int32_t*)(p - 4);
        return tgt >= g_codeLo && tgt < g_codeHi;
    }
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && (p[-1] & 0xC0) == 0xC0) return true;            // call reg
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return true;            // call [reg+d8]
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10 && ((p[-5] & 0xC0) == 0x80 || p[-5] == 0x15)) return true;  // [reg+d32] / [abs]
    return false;
}

int Scan(uintptr_t esp, uint32_t* out, int cap)
{
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    uintptr_t end = esp + 0x1800;
    if (end > (uintptr_t)tib->StackBase) end = (uintptr_t)tib->StackBase;
    int n = 0;
    for (uintptr_t a = esp; a + 4 <= end && n < cap; a += 4) {
        const uint32_t v = *(const uint32_t*)a;
        if (IsRet(v)) out[n++] = v;
    }
    return n;
}

void Fmt(char* dst, size_t cap, const uint32_t* f, int n)
{
    size_t k = 0; dst[0] = 0;
    for (int i = 0; i < n && k + 10 < cap; ++i) k += _snprintf_s(dst + k, cap - k, _TRUNCATE, " %08X", f[i]);
}
}  // namespace

extern "C" void __cdecl SspCtor(uintptr_t obj, uintptr_t esp)
{
    __try {
        InterlockedIncrement(&g_ctors);
        Rec& r = g_ring[(InterlockedIncrement(&g_ringPos) - 1) & (kRing - 1)];
        r.obj = 0;
        r.t = GetTickCount64();
        r.n = Scan(esp, r.f, kFrames);
        r.obj = obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

extern "C" void __cdecl SspReg(uintptr_t mgr, const uint32_t* esp)
{
    __try {
        InterlockedIncrement(&g_regs);
        if (InterlockedIncrement(&g_lines) > 300) return;
        const uintptr_t ent = esp[1];
        const uint32_t vt = ent ? *(const uint32_t*)ent : 0;
        const unsigned cnt = *(const unsigned short*)(mgr + 0x3EE);
        uint32_t own[12]; const int on = Scan((uintptr_t)esp, own, 12);
        char a[200], b[220], line[600];
        Fmt(a, sizeof(a), own, on);
        const Rec* hit = nullptr;
        const LONG pos = g_ringPos;
        for (int i = 0; i < kRing; ++i) {
            const Rec& r = g_ring[(pos - 1 - i) & (kRing - 1)];
            if (r.obj == ent) { hit = &r; break; }
        }
        if (hit) { InterlockedIncrement(&g_matched); Fmt(b, sizeof(b), hit->f, hit->n); }
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[SSP] reg mgr=%08X ent=%08X vt=%08X listN=%u ret=%08X | own:%s | created %s%llums ago:%s\n",
                    (unsigned)mgr, (unsigned)ent, vt, cnt, esp[0], a, hit ? "" : "(not in ring) ",
                    hit ? GetTickCount64() - hit->t : 0ull, hit ? b : "");
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

namespace {
// ---- SpawnShipParams::Spawn capture --------------------------------------------------------------------------------
typedef uintptr_t(__thiscall* SpawnFn)(void* params, int a2);
DWORD g_tSpawn = 0;
volatile uintptr_t g_lastParams = 0, g_lastParamsVt = 0, g_lastSpawned = 0, g_aiParams = 0;   // g_aiParams: newest pvt 0x0270A538
volatile LONG g_spawns = 0, g_fires = 0;

void PosOf(uintptr_t e, float* p)
{
    p[0] = p[1] = p[2] = 0.f;
    __try { const HookMath::Vector4 v = ((Entity*)e)->GetPosition(); p[0] = v.x; p[1] = v.y; p[2] = v.z; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

uintptr_t __fastcall DetSpawn(void* params, void* /*edx*/, int a2)
{
    const uintptr_t e = ((SpawnFn)g_tSpawn)(params, a2);
    __try {
        InterlockedIncrement(&g_spawns);
        const uintptr_t vt = *(const uintptr_t*)params;
        if (e) { g_lastParams = (uintptr_t)params; g_lastParamsVt = vt; g_lastSpawned = e; if (vt == 0x0270A538) g_aiParams = (uintptr_t)params; }
        if (InterlockedIncrement(&g_lines) > 300) return e;
        float p[3]; if (e) PosOf(e, p); else p[0] = p[1] = p[2] = 0.f;
        char line[240];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[SSP] spawn params=%08X pvt=%08X a2=%08X flags94=%02X -> ent=%08X evt=%08X pos=(%.1f, %.1f, %.1f) tid=%lu\n",
                    (unsigned)(uintptr_t)params, (unsigned)vt, (unsigned)a2, *((const BYTE*)params + 0x94), (unsigned)e,
                    e ? *(const unsigned*)e : 0u, p[0], p[1], p[2], GetCurrentThreadId());
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return e;
}

__declspec(naked) void DetCtor()   // at 0x00503338: esi = this
{
    __asm {
        pushad
        lea  eax, [esp + 32]
        push eax
        push esi
        call SspCtor
        add  esp, 8
        popad
        jmp  dword ptr [g_tCtor]
    }
}
__declspec(naked) void DetReg()    // at 0x0109B500 entry: ecx = mgr, [esp] = ret, [esp+4] = ship
{
    __asm {
        pushad
        lea  eax, [esp + 32]
        push eax
        push ecx
        call SspReg
        add  esp, 8
        popad
        jmp  dword ptr [g_tReg]
    }
}

bool Install(uintptr_t va, const BYTE* pro, int n, void* det, DWORD* slot, const char* name)
{
    BYTE* site = (BYTE*)(GameModuleBase + (va - 0x00400000));
    char line[128];
    if (memcmp(site, pro, n) != 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[SSP] %s %08X prologue mismatch - not installed\n", name, (unsigned)va);
        Phase1_LogLine(line); return false;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, pro, n); t[n] = 0x68; *(DWORD*)(t + n + 1) = (DWORD)(uintptr_t)(site + n); t[n + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, n + 6);
    *slot = (DWORD)(uintptr_t)t;
    const bool ok = Hook::DetourFunc(site, (DWORD)(uintptr_t)det, (DWORD)n);
    if (!ok) *slot = 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[SSP] %s %08X hooked=%d\n", name, (unsigned)va, ok ? 1 : 0);
    Phase1_LogLine(line);
    return ok;
}
}  // namespace

void ShipSpawnProbeInit()
{
    if (GetPrivateProfileIntA("Coop", "ShipSpawnProbe", 0, Paths::Ini()) == 0) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)GameModuleBase;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(GameModuleBase + dos->e_lfanew);
    g_codeLo = GameModuleBase + nt->OptionalHeader.BaseOfCode;
    g_codeHi = g_codeLo + nt->OptionalHeader.SizeOfCode;
    static const BYTE pCtor[5] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };          // 0x00503338 mov eax, 1
    static const BYTE pReg[6] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 };      // 0x0109B500 push ebx; mov ebx,esp; sub esp,8
    const bool c = Install(0x00503338, pCtor, 5, (void*)&DetCtor, &g_tCtor, "EntityGroupCtor");
    const bool r = Install(0x0109B500, pReg, 6, (void*)&DetReg, &g_tReg, "RegisterShip");
    static const BYTE pSpawn[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };    // 0x01996AB0 push ebp; mov ebp,esp; sub esp,18h
    const bool s = Install(0x01996AB0, pSpawn, 6, (void*)&DetSpawn, &g_tSpawn, "SpawnShipParamsSpawn");
    g_on = c || r || s;
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[SSP] ShipSpawnProbe on: code %08X..%08X ctor=%d reg=%d spawn=%d\n",
                (unsigned)g_codeLo, (unsigned)g_codeHi, c ? 1 : 0, r ? 1 : 0, s ? 1 : 0);
    Phase1_LogLine(line);
}

// Player-update thread, every frame (phase1.cpp OnFrameInner). [Coop] ShipSpawnFire is re-read once a second.
//   v % 10: 1 build only, 2 build + World::AddEntity, 3 = 2 + place the ship 80 m ahead of the player 1.5 s after the add
//   (a pre-add position write did not hold for NPC bodies, brain-identity-spawn-rng.md: so the write comes after).
//   (v / 10) % 10: 0 = newest AI SpawnShipParams (pvt 0x0270A538), 1 = newest params of any class (incl. the player-ship
//   subclass pvt 0x026A20C8 - run 2026-11-25 10:53: build + AddEntity of that one crashed BOTH copies, read NULL at
//   0x01A64ADA from naval code 0x0122DD8D).
//   [Coop] ShipSpawnFirePid: fire only in the process with this pid (0 = every process reading the ini).
void ShipSpawnProbeTick()
{
    if (!g_on || !g_tSpawn) return;
    static ULONGLONG s_next = 0, s_placeAt = 0;
    static int s_last = 0;
    static uintptr_t s_place = 0;
    const ULONGLONG now = GetTickCount64();
    char line[300];
    if (s_place && now >= s_placeAt) {
        const uintptr_t e = s_place; s_place = 0;
        DWORD code = 0; float b[3] = {}, a[3] = {};
        __try {
            World* w = World::GetCurrentWorld();
            Entity* pl = w ? w->GetPlayer() : nullptr;
            if (pl) {
                PosOf(e, b);
                HookMath::Transform m = ((Entity*)e)->m_GlobalMatrix;
                const HookMath::Transform& pm = pl->m_GlobalMatrix;
                m.M41 = pm.M41 + 80.f * pm.M21; m.M42 = pm.M42 + 80.f * pm.M22; m.M43 = 0.f;
                ((Entity*)e)->SetGlobalMatrixInternal(&m, true);
                PosOf(e, a);
            }
        } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[SSP] place ent=%08X before=(%.1f, %.1f, %.1f) after=(%.1f, %.1f, %.1f) exc=%08X\n",
                    (unsigned)e, b[0], b[1], b[2], a[0], a[1], a[2], (unsigned)code);
        Phase1_LogLine(line);
    }
    if (now < s_next) return;
    s_next = now + 1000;
    const int v = GetPrivateProfileIntA("Coop", "ShipSpawnFire", 0, Paths::Ini());
    if (v == s_last) return;
    s_last = v;
    if (v == 0) return;
    const int pid = GetPrivateProfileIntA("Coop", "ShipSpawnFirePid", 0, Paths::Ini());
    if (pid && (DWORD)pid != GetCurrentProcessId()) return;
    const int mode = v % 10, any = (v / 10) % 10;
    const uintptr_t params = any ? g_lastParams : g_aiParams;
    const uintptr_t wantVt = any ? g_lastParamsVt : 0x0270A538;
    if (!params) { Phase1_LogLine("[SSP] fire: no captured SpawnShipParams of that kind yet\n"); return; }
    InterlockedIncrement(&g_fires);
    uintptr_t e = 0, a2 = 0; DWORD code = 0; int added = 0; const uintptr_t prev = g_lastSpawned;
    __try {
        if (*(const uintptr_t*)params != wantVt) { Phase1_LogLine("[SSP] fire: params vtable changed (freed?) - skipped\n"); return; }
        typedef uintptr_t(__thiscall* VFn)(void*);
        a2 = ((VFn)(*(uintptr_t**)params)[0x50 / 4])((void*)params);           // the engine's own a2 (0x005B0700)
        typedef uintptr_t(__thiscall* SpawnWrap)(void*, uintptr_t);
        e = ((SpawnWrap)0x0045E330)((void*)params, a2);                          // SpawnParams::Spawn wrapper (lock + vt[31])
        if (e && mode >= 2) {
            World* w = World::GetCurrentWorld();
            if (w) { w->AddEntity((Entity*)e); added = 1; }
            if (added && mode == 3) { s_place = e; s_placeAt = now + 1500; }
        }
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
    float p[3]; if (e) PosOf(e, p); else p[0] = p[1] = p[2] = 0.f;
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[SSP] fire v=%d mode=%d any=%d params=%08X pvt=%08X a2=%08X -> ent=%08X evt=%08X pos=(%.1f, %.1f, %.1f) added=%d exc=%08X prevSpawned=%08X\n",
                v, mode, any, (unsigned)params, (unsigned)wantVt, (unsigned)a2, (unsigned)e, e ? *(const unsigned*)e : 0u,
                p[0], p[1], p[2], added, (unsigned)code, (unsigned)prev);
    Phase1_LogLine(line);
}
int ShipSpawnProbeReport(char* out, int cap)
{
    if (!g_on) return 0;
    return _snprintf_s(out, cap, _TRUNCATE, "[SSP] counts ctors=%ld regs=%ld matched=%ld spawns=%ld fires=%ld", g_ctors, g_regs,
                       g_matched, g_spawns, g_fires);
}

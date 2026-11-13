// Guards perceive the co-op puppet (host only). Evidence: ops/evidence/guard-perception-of-puppet-2026-11-13.md
// (S3, sections 0, 3.2, 7.4, 7.5), AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35.
//
// Guard sight and detection never query "who is near": they call World::GetPlayer (0x70E810), which is always the
// local player, so the puppet is never a candidate. The three call sites that CHOOSE the target are redirected here;
// GetPlayer itself is untouched (603 code xrefs).
//
//   site       original bytes   what                                  observer at the call
//   0x103D61D  E8 EE 11 6D FF   CSLineOfSight player-request create   [edi+78h] (owner guard)
//   0x103D7A1  E8 6A 10 6D FF   CSLineOfSight retarget, every update  [esi+78h] (owner guard)
//   0x131EFD5  E8 36 F8 3E FF   CRLDetect start (target -> +0x74)     edx = guard Entity* (resolved inline above)
//
// GetPlayer is __thiscall (ecx = World*), no stack arguments, plain retn, returns Entity* in eax, clobbers eax ecx edx
// and flags, preserves ebx esi edi ebp (0x70E810: mov eax,[ecx+7F8h] / mov ecx,[eax+4] / jmp 0x736540). Each stub
// keeps that contract: it calls the real GetPlayer, then returns either the player or the puppet.
//
// Choice per guard (one table, so sensor, detection start and the escalation that carries CRLDetect+0x74 agree):
// decided at the per-update retarget site - the nearer of player / puppet, switching only when the other is 3 m
// nearer and the current choice is at least 2 s old; read by the other two sites (create: decide once if unknown).
// Not covered (S3 3.2 / 7.2 / 7.3): the target gate 0x101CAA0 for the puppet, the escalation receiver, and the
// helpers 0x6F6010 / 0x65A660 / 0x1B06D30 (not patched: they still read the local player).
#include "guard_sight.h"
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

void* Phase1_PuppetEntity();
bool  Phase1_IsEntityHost();

namespace {

struct Site { uintptr_t va; BYTE orig[5]; };
const Site kSites[3] = {
    { 0x0103D61D, { 0xE8, 0xEE, 0x11, 0x6D, 0xFF } },
    { 0x0103D7A1, { 0xE8, 0x6A, 0x10, 0x6D, 0xFF } },
    { 0x0131EFD5, { 0xE8, 0x36, 0xF8, 0x3E, 0xFF } },
};
bool g_installed = false;
int  g_on = 0; ULONGLONG g_nextMs = 0;

const int SLOTS = 256;
struct Choice { uintptr_t obs; int puppet; ULONGLONG sinceMs; ULONGLONG seenMs; };   // seenMs: last Choose for obs
Choice  g_tab[SLOTS];
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_calls[3] = {}, g_puppetPicks = 0, g_switches = 0;

bool On(ULONGLONG nowMs)
{
    if (nowMs >= g_nextMs) {
        g_nextMs = nowMs + 2000;
        g_on = GetPrivateProfileIntA("Coop", "GuardSeesPuppet", 0, Paths::Ini()) != 0;
    }
    return g_on != 0;
}

bool Pos(void* e, float* p)
{
    __try {
        const Entity* en = (const Entity*)e;
        p[0] = en->m_GlobalMatrix.M41; p[1] = en->m_GlobalMatrix.M42; p[2] = en->m_GlobalMatrix.M43;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float D2(const float* a, const float* b)
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}
void* World(void* e)
{
    __try { return *(void**)((char*)e + 0x5C); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }   // m_AssociatedWorld
}

// [Coop] SightProbe (default 0; live; read-only except the two faction getters, which S3 section 8.3 read as loads
// and getter calls only). S3 section 8: the reaction is created only after CRLDetectParams CanReact (0x1076110), so
// a puppet chosen as sight target but never detected stops at L (geometry / gate), S (stimulus) or R (faction /
// alert state). At the retarget site, when this sensor's previous target was the puppet, the request's flags are the
// result of that evaluation: req = *(los+0xF8), flags = u16 req+0x10: 0x001 cone, 0x002 VISIBLE (stimulus 8 sent),
// 0x004 blocked, 0x020 evaluated, 0x040 async result arrived, 0x100 in range, 0x200 player request.
// Puppet: kind/role = *(P+0x9C) &7 / >>3&0x3F; h = *(P+0xE8): byte h+0x28 (L2 gate), ai = *(h+0x1C) (L4).
// Faction (R3), only on visible frames: sub_133A4C0(e) entity -> faction (cdecl, 1 arg), sub_17114C0(G, T) "T is an
// enemy of G" (cdecl, 2 args) - both already have decomp units and are used in phase1.cpp.
int g_spOn = 0; ULONGLONG g_spNext = 0;
volatile LONG g_spN = 0, g_spCone = 0, g_spVis = 0, g_spBlk = 0, g_spEval = 0, g_spAsync = 0, g_spRange = 0, g_spPReq = 0,
              g_spL2 = 0, g_spNoAi = 0, g_spHostP = 0, g_spHostPl = 0, g_spSameFac = 0, g_spAsOff = 0;
struct SpSample { unsigned flags, kind, role, l2, ai, facP, facPl, as20; float dist; };
SpSample g_spLast = {};
void SightProbe(void* los, void* G, void* P, void* player)
{
    const ULONGLONG nowMs = GetTickCount64();
    if (nowMs >= g_spNext) { g_spNext = nowMs + 2000; g_spOn = GetPrivateProfileIntA("Coop", "SightProbe", 0, Paths::Ini()) != 0; }
    if (!g_spOn || !los) return;
    __try {
        const char* req = *(const char* const*)((const char*)los + 0xF8);
        if (!req) return;
        SpSample s = {};
        s.flags = *(const uint16_t*)(req + 0x10);
        InterlockedIncrement(&g_spN);
        if (s.flags & 0x001) InterlockedIncrement(&g_spCone);
        if (s.flags & 0x002) InterlockedIncrement(&g_spVis);
        if (s.flags & 0x004) InterlockedIncrement(&g_spBlk);
        if (s.flags & 0x020) InterlockedIncrement(&g_spEval);
        if (s.flags & 0x040) InterlockedIncrement(&g_spAsync);
        if (s.flags & 0x100) InterlockedIncrement(&g_spRange);
        if (s.flags & 0x200) InterlockedIncrement(&g_spPReq);
        const uint32_t k = *(const uint32_t*)((const char*)P + 0x9C);
        s.kind = k & 7; s.role = (k >> 3) & 0x3F;
        const char* h = *(const char* const*)((const char*)P + 0xE8);
        s.l2 = h ? *(const uint8_t*)(h + 0x28) : 0xFF;
        s.ai = h ? (unsigned)(uintptr_t)*(void* const*)(h + 0x1C) : 0;
        if (s.l2 && s.l2 != 0xFF) InterlockedIncrement(&g_spL2);
        if (!s.ai) InterlockedIncrement(&g_spNoAi);
        const char* w = *(const char* const*)((const char*)G + 0x5C);
        const char* as = w ? *(const char* const*)(w + 0x8E0) : nullptr;
        s.as20 = as ? *(const uint8_t*)(as + 0x20) : 0xFF;
        if (!as || !s.as20) InterlockedIncrement(&g_spAsOff);
        float pg[3], pp[3];
        if (Pos(G, pg) && Pos(P, pp)) s.dist = sqrtf(D2(pg, pp));
        if (s.flags & 0x002) {
            typedef void* (__cdecl* FacFn)(void*);
            typedef BOOL (__cdecl* HostFn)(void*, void*);
            s.facP  = (unsigned)(uintptr_t)((FacFn)0x0133A4C0)(P);
            s.facPl = (unsigned)(uintptr_t)((FacFn)0x0133A4C0)(player);
            if (s.facP && s.facP == s.facPl) InterlockedIncrement(&g_spSameFac);
            if (((HostFn)0x017114C0)(G, P)) InterlockedIncrement(&g_spHostP);
            if (((HostFn)0x017114C0)(G, player)) InterlockedIncrement(&g_spHostPl);
        }
        g_spLast = s;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int g_dktOn = 0; ULONGLONG g_dktNext = 0; volatile LONG g_dktKept = 0;
// mode 0 = request create, 1 = per-update retarget (decides), 2 = detection start (reads)
void* __cdecl Choose(void* obs, void* player, int mode, void* los)
{
    InterlockedIncrement(&g_calls[mode]);
    const ULONGLONG nowMs = GetTickCount64();
    if (!On(nowMs) || !Phase1_IsEntityHost() || !obs || !player) return player;
    void* pup = Phase1_PuppetEntity();
    if (!pup || pup == obs || World(pup) != World(player)) return player;
    // [Coop] DetectKeepTarget (live, default 0). At 0x131EFD5 `los` is the CRLDetect (esi). CreateReaction
    // (0x1055CB0) set CRLDetect+0x74 to the stimulus's handle (S3 evidence section 8); the start function then
    // overwrites +0x74 with this call's result. When the reaction was created FOR the puppet, keep the puppet, so
    // the guard detects the body that triggered it. Handle -> entity: (*(h+8) >> 31) & *h (engine pattern, e.g.
    // sub_13F22E0, sub_1340E60). Run 13532 (15ee79a): 7 CanReact passes for the puppet (stimulus 95), each matched
    // by a detection start, but the guards stayed on alert only.
    if (mode == 2 && los) {
        if (nowMs >= g_dktNext) { g_dktNext = nowMs + 2000; g_dktOn = GetPrivateProfileIntA("Coop", "DetectKeepTarget", 0, Paths::Ini()) != 0; }
        if (g_dktOn) {
            __try {
                const int* h = *(const int* const*)((const char*)los + 0x74);
                const void* e = h ? (const void*)(uintptr_t)((h[2] >> 31) & h[0]) : nullptr;
                if (e == pup) { InterlockedIncrement(&g_dktKept); InterlockedIncrement(&g_puppetPicks); return pup; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    float po[3], pp[3], pq[3];
    if (!Pos(obs, po) || !Pos(player, pp) || !Pos(pup, pq)) return player;

    AcquireSRWLockExclusive(&g_lock);
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (g_tab[i].obs == (uintptr_t)obs) { slot = i; break; }
        if (freeSlot < 0 && (g_tab[i].obs == 0 || nowMs - g_tab[i].sinceMs > 60000)) freeSlot = i;
    }
    int pick = 0;
    if (slot >= 0) pick = g_tab[slot].puppet;
    if (mode == 1 && pick) {                  // previous target was the puppet: its evaluation result is in req
        ReleaseSRWLockExclusive(&g_lock);
        SightProbe(los, obs, pup, player);
        AcquireSRWLockExclusive(&g_lock);
    }
    if (mode != 2) {
        const float dp = D2(po, pp), dq = D2(po, pq);
        const bool puppetNearer = dq < dp;
        if (slot < 0) {
            pick = puppetNearer ? 1 : 0;
            slot = freeSlot;
            if (slot >= 0) { g_tab[slot].obs = (uintptr_t)obs; g_tab[slot].puppet = pick; g_tab[slot].sinceMs = nowMs; }
        } else if (mode == 1 && nowMs - g_tab[slot].sinceMs >= 2000) {
            // switch only when the other body is at least 3 m nearer (compare distances, not squares)
            const float sp = sqrtf(dp), sq = sqrtf(dq);
            const int want = pick ? (sp + 3.0f < sq ? 0 : 1) : (sq + 3.0f < sp ? 1 : 0);
            if (want != pick) { pick = want; g_tab[slot].puppet = pick; g_tab[slot].sinceMs = nowMs; InterlockedIncrement(&g_switches); }
        }
    }
    if (slot >= 0 && g_tab[slot].obs == (uintptr_t)obs) g_tab[slot].seenMs = nowMs;
    ReleaseSRWLockExclusive(&g_lock);
    if (pick) { InterlockedIncrement(&g_puppetPicks); return pup; }
    return player;
}

// 0x103D61D: ecx = World*, observer [edi+78h]. [esp+4] belongs to the caller (its pushed ebx): not touched.
__declspec(naked) void StubCreate()
{
    __asm {
        mov  eax, 0x0070E810
        call eax                    // eax = local player (real GetPlayer, ecx = World*)
        push 0                      // los (not probed here)
        push 0
        push eax
        push dword ptr [edi + 78h]
        call Choose
        add  esp, 16
        ret
    }
}
// 0x103D7A1: ecx = World*, observer [esi+78h]
__declspec(naked) void StubRetarget()
{
    __asm {
        mov  eax, 0x0070E810
        call eax
        push esi                    // los = CSLineOfSight (SightProbe)
        push 1
        push eax
        push dword ptr [esi + 78h]
        call Choose
        add  esp, 16
        ret
    }
}
// 0x131EFD5: ecx = World*, edx = guard Entity* (GetPlayer clobbers edx: saved first)
__declspec(naked) void StubDetect()
{
    __asm {
        push edx
        mov  eax, 0x0070E810
        call eax
        pop  edx
        push esi                    // the CRLDetect (DetectKeepTarget reads its +0x74)
        push 2
        push eax
        push edx
        call Choose
        add  esp, 16
        ret
    }
}

bool PatchCall(uintptr_t site, const BYTE* orig, void* stub)
{
    BYTE* p = (BYTE*)(GameModuleBase + (site - 0x00400000));
    if (memcmp(p, orig, 5) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = (int32_t)((uintptr_t)stub - ((uintptr_t)p + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void GuardSight::Init()
{
    if (GetPrivateProfileIntA("Coop", "GuardSeesPuppet", -1, Paths::Ini()) < 0) return;
    // all three or none: verify every site before patching any
    for (const Site& s : kSites)
        if (memcmp((const void*)(GameModuleBase + (s.va - 0x00400000)), s.orig, 5) != 0) {
            OutputDebugStringA("[Sight] call-site bytes differ - not installed\n");
            return;
        }
    void* stubs[3] = { (void*)&StubCreate, (void*)&StubRetarget, (void*)&StubDetect };
    for (int i = 0; i < 3; ++i) PatchCall(kSites[i].va, kSites[i].orig, stubs[i]);
    g_installed = true;
    OutputDebugStringA("[Sight] installed at 0x103D61D, 0x103D7A1, 0x131EFD5\n");
}

// For guard_poi.cpp ([Coop] GuardPlayerOfInterest): true when this guard's sight sensor currently tracks the
// puppet (its entry picked the puppet and was refreshed by a Choose call within the last 3 s).
bool GuardSight::TracksPuppet(const void* guard)
{
    if (!g_installed || !guard) return false;
    const ULONGLONG nowMs = GetTickCount64();
    bool r = false;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < SLOTS; ++i)
        if (g_tab[i].obs == (uintptr_t)guard) { r = g_tab[i].puppet && nowMs - g_tab[i].seenMs < 3000; break; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

int GuardSight::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    int n = _snprintf_s(out, cap, _TRUNCATE, "[Sight] on=%d host=%d puppet=%p | calls create=%ld retarget=%ld detect=%ld keptPuppet=%ld | puppetPicks=%ld switches=%ld",
                       g_on, Phase1_IsEntityHost() ? 1 : 0, Phase1_PuppetEntity(),
                       InterlockedExchange(&g_calls[0], 0), InterlockedExchange(&g_calls[1], 0),
                       InterlockedExchange(&g_calls[2], 0), InterlockedExchange(&g_dktKept, 0),
                       InterlockedExchange(&g_puppetPicks, 0), InterlockedExchange(&g_switches, 0));
    if (g_spOn && n > 0 && n < cap - 200) {
        const SpSample s = g_spLast;
        n += _snprintf_s(out + n, cap - n, _TRUNCATE,
            " | probe n=%ld cone=%ld VIS=%ld blocked=%ld eval=%ld async=%ld range=%ld playerReq=%ld L2=%ld noAi=%ld asOff=%ld"
            " | onVis: sameFac=%ld hostile(G,P)=%ld hostile(G,player)=%ld | last flags=%03X kind=%u role=%u l2=%u ai=%08X as20=%u d=%.1fm facP=%08X facPl=%08X",
            InterlockedExchange(&g_spN, 0), InterlockedExchange(&g_spCone, 0), InterlockedExchange(&g_spVis, 0),
            InterlockedExchange(&g_spBlk, 0), InterlockedExchange(&g_spEval, 0), InterlockedExchange(&g_spAsync, 0),
            InterlockedExchange(&g_spRange, 0), InterlockedExchange(&g_spPReq, 0), InterlockedExchange(&g_spL2, 0),
            InterlockedExchange(&g_spNoAi, 0), InterlockedExchange(&g_spAsOff, 0), InterlockedExchange(&g_spSameFac, 0),
            InterlockedExchange(&g_spHostP, 0), InterlockedExchange(&g_spHostPl, 0),
            s.flags, s.kind, s.role, s.l2, s.ai, s.as20, s.dist, s.facP, s.facPl);
    }
    return n > 0 ? n : 0;
}

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
struct Choice { uintptr_t obs; int puppet; ULONGLONG sinceMs; };
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

// mode 0 = request create, 1 = per-update retarget (decides), 2 = detection start (reads)
void* __cdecl Choose(void* obs, void* player, int mode)
{
    InterlockedIncrement(&g_calls[mode]);
    const ULONGLONG nowMs = GetTickCount64();
    if (!On(nowMs) || !Phase1_IsEntityHost() || !obs || !player) return player;
    void* pup = Phase1_PuppetEntity();
    if (!pup || pup == obs || World(pup) != World(player)) return player;
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
        push 0
        push eax
        push dword ptr [edi + 78h]
        call Choose
        add  esp, 12
        ret
    }
}
// 0x103D7A1: ecx = World*, observer [esi+78h]
__declspec(naked) void StubRetarget()
{
    __asm {
        mov  eax, 0x0070E810
        call eax
        push 1
        push eax
        push dword ptr [esi + 78h]
        call Choose
        add  esp, 12
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
        push 2
        push eax
        push edx
        call Choose
        add  esp, 12
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

int GuardSight::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    return _snprintf_s(out, cap, _TRUNCATE, "[Sight] on=%d host=%d puppet=%p | calls create=%ld retarget=%ld detect=%ld | puppetPicks=%ld switches=%ld",
                       g_on, Phase1_IsEntityHost() ? 1 : 0, Phase1_PuppetEntity(),
                       InterlockedExchange(&g_calls[0], 0), InterlockedExchange(&g_calls[1], 0),
                       InterlockedExchange(&g_calls[2], 0), InterlockedExchange(&g_puppetPicks, 0),
                       InterlockedExchange(&g_switches, 0));
}

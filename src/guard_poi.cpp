// Per-guard player of interest. AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA 2026-11-13 (o19-o21) and the S3
// evidence (guard-perception-of-puppet-2026-11-13.md section 7, G4):
//   sub_6F6010(Entity* e) = GetPlayer(e->m_AssociatedWorld): cdecl, 1 arg, 121 code xrefs; the detection
//     statechart's distance-to-player / speed modifier / vanish / investigate logic reaches "the player" through it.
//     0x006F6010  55 / 8B EC / 8B 45 08    push ebp / mov ebp,esp / mov eax,[ebp+8]   <- 6 bytes stolen
//   sub_65A660(Entity* e, float r) "e within r of the player": calls GetPlayer itself at
//     0x0065A688  E8 83 41 0B 00   call sub_70E810 (ecx = e->world, esi = e)            <- call-site redirect
// User observation (run 19236, build 91255aa): guards spot the puppet and the meter fills while it is in sight, but
// they never chase it once it is out of sight - those follow-up states ask for "the player" (the host's own).
// With the flag on, for a guard whose sight sensor currently tracks the puppet (guard_sight's per-guard choice:
// the nearer body, 3 m / 2 s hysteresis), both lookups return the puppet. Every other caller is unchanged.
//
// ConflictNullGuard - crash in run 26408 (build d34ca54, AC4Coop_crashctx_26408_1.txt): sub_15DB170 (thiscall, on
// the world conflict object at world+0x8DC, vtable 0x026DB6E0; caller sub_1626AB0 at 0x01626CAA) resolves its target
// from +0x304, then reads target->(+0xE8 AIComponent)->(+0x94) and calls its vt+0x5C. For the puppet +0x94 is null:
//   0x015DB196  8B 89 94 00 00 00   mov ecx,[ecx+94h]
//   0x015DB19C  8B 11 / 8B 42 5C    mov edx,[ecx] / mov eax,[edx+5Ch]   <- fault (ecx = 0); 5 bytes stolen
//   0x015DB1A1  FF D0               call eax
//   0x015DB25B  5E / C3             pop esi / retn   (the function's own exit when there is no target; same frame
//                                                     depth: only esi is pushed at 0x015DB170)
// The guard takes that exit when ecx is 0.
#include "guard_poi.h"
#include "guard_sight.h"
#include "Hook.h"
#include "paths.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void* Phase1_PuppetEntity();
bool  Phase1_IsEntityHost();

namespace {

const uintptr_t VA_POI = 0x006F6010;
const BYTE kPoiPrologue[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };
const uintptr_t VA_NEAR_CALL = 0x0065A688;
const BYTE kNearCall[5] = { 0xE8, 0x83, 0x41, 0x0B, 0x00 };
const uintptr_t VA_GETPLAYER = 0x0070E810;
const uintptr_t VA_CONFLICT = 0x015DB19C;
const BYTE kConflict[5] = { 0x8B, 0x11, 0x8B, 0x42, 0x5C };
const uintptr_t VA_CONFLICT_RESUME = 0x015DB1A1;
const uintptr_t VA_CONFLICT_EXIT = 0x015DB25B;

DWORD g_poiTramp = 0, g_getPlayer = 0, g_cfResume = 0, g_cfExit = 0;
bool  g_poiInstalled = false, g_cfInstalled = false;
int   g_on = 0; ULONGLONG g_next = 0;
volatile LONG g_calls = 0, g_lent = 0, g_cfSkipped = 0;

void* __cdecl PoiFor(void* e)
{
    __try {
        InterlockedIncrement(&g_calls);
        const ULONGLONG now = GetTickCount64();
        if (now >= g_next) { g_next = now + 2000; g_on = GetPrivateProfileIntA("Coop", "GuardPlayerOfInterest", 0, Paths::Ini()) != 0; }
        if (!g_on || !e || !Phase1_IsEntityHost()) return nullptr;
        void* pup = Phase1_PuppetEntity();
        if (!pup || e == pup) return nullptr;
        if (!GuardSight::TracksPuppet(e)) return nullptr;
        InterlockedIncrement(&g_lent);
        return pup;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// Entry of sub_6F6010: [esp] = return, [esp+4] = e. cdecl, result in eax.
__declspec(naked) void PoiDetour()
{
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 12]   // e
        call PoiFor
        add  esp, 4
        pop  edx
        pop  ecx
        test eax, eax
        jz   orig
        ret                         // the puppet
    orig:
        jmp  dword ptr [g_poiTramp]
    }
}

// Replaces `call sub_70E810` at 0x0065A688: ecx = world, esi = the entity being tested.
__declspec(naked) void NearStub()
{
    __asm {
        push ecx
        push edx
        push esi
        call PoiFor
        add  esp, 4
        pop  edx
        pop  ecx
        test eax, eax
        jnz  done
        jmp  dword ptr [g_getPlayer] // real GetPlayer (thiscall ecx = world); returns to 0x0065A68D
    done:
        ret
    }
}

__declspec(naked) void ConflictDetour()
{
    __asm {
        test ecx, ecx
        jz   bad
        mov  edx, [ecx]
        mov  eax, [edx + 5Ch]
        jmp  dword ptr [g_cfResume]
    bad:
        lock inc dword ptr [g_cfSkipped]
        jmp  dword ptr [g_cfExit]
    }
}

uintptr_t Va(uintptr_t va) { return GameModuleBase + (va - 0x00400000); }

bool PatchCall(uintptr_t site, void* stub)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(BYTE*)site = 0xE8;
    *(int32_t*)(site + 1) = (int32_t)((uintptr_t)stub - (site + 5));
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    return true;
}

}  // namespace

void GuardPoi::Init()
{
    if (GetPrivateProfileIntA("Coop", "ConflictNullGuard", 0, Paths::Ini()) == 1) {
        const uintptr_t s = Va(VA_CONFLICT);
        if (memcmp((const void*)s, kConflict, 5) == 0) {
            g_cfResume = (DWORD)Va(VA_CONFLICT_RESUME);
            g_cfExit = (DWORD)Va(VA_CONFLICT_EXIT);
            g_cfInstalled = Hook::DetourFunc((BYTE*)s, (DWORD)(uintptr_t)&ConflictDetour, 5) != 0;
        } else OutputDebugStringA("[Poi] bytes mismatch at 0x15DB19C\n");
    }
    if (GetPrivateProfileIntA("Coop", "GuardPlayerOfInterest", -1, Paths::Ini()) < 0) return;   // key absent: no hooks
    const uintptr_t p = Va(VA_POI), n = Va(VA_NEAR_CALL);
    if (memcmp((const void*)p, kPoiPrologue, 6) != 0 || memcmp((const void*)n, kNearCall, 5) != 0) {
        OutputDebugStringA("[Poi] bytes mismatch at 0x6F6010 / 0x65A688 - not installed\n");
        return;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kPoiPrologue, 6);
    t[6] = 0x68;
    *reinterpret_cast<DWORD*>(t + 7) = (DWORD)(p + 6);
    t[11] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, 12);
    g_poiTramp = (DWORD)(uintptr_t)t;
    g_getPlayer = (DWORD)Va(VA_GETPLAYER);
    if (!Hook::DetourFunc((BYTE*)p, (DWORD)(uintptr_t)&PoiDetour, 6)) return;
    PatchCall(n, (void*)&NearStub);
    g_poiInstalled = true;
}

int GuardPoi::FormatReport(char* out, int cap)
{
    if ((!g_poiInstalled && !g_cfInstalled) || cap < 64) return 0;
    const int n = _snprintf_s(out, cap, _TRUNCATE, "[Poi] on=%d lookups=%ld lentPuppet=%ld | conflictNullSkips=%ld",
                              g_on, InterlockedExchange(&g_calls, 0), InterlockedExchange(&g_lent, 0),
                              InterlockedExchange(&g_cfSkipped, 0));
    return n > 0 ? n : 0;
}

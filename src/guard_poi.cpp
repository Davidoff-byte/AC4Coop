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
//
// ConflictKeepPuppet - why the FightManager dropped the puppet 1-2 s after taking it as its target (run 476, build
// c0bb544, FightProbe: every removal `from=0161F870 <00A00EB0 <00A0105A`). IDA 2026-11-14 (o71-o81), static chain:
//   0x0161F860  mov eax,[ecx+304h] / test / jz / push eax / call sub_161F420 / retn   (undefined in the idb; "drop the
//               current target"; its only references are tail jumps at 0x01789609 / 0x01789613)
//   sub_17895F0 (cdecl, 1 arg): fm = ((a1+0xC)->+0x70 world)->+0x8DC; if (fm+0x304) RemoveFighter(fm, fm+0x304).
//               Only reference: the statechart transition descriptor 0x029ED630 (+0xC action = 0x017895F0, +8 guard = 0),
//               wired at 0x01788275 in sub_1786BE0 as sub_9F1610(0, state v97, dword_4E18E50, dword_4E18E50, state v95).
//   0x00A00E30 drains the deferred statechart event queue (16-byte records) into sub_A00470, which runs the action
//               at descriptor +0xC (matches the live return chain above).
//   dword_4E18E50 (registered from hash 0xF58E6E97) is posted by sub_17AF5F0, sub_17BFD10 and sub_17BFF50; the
//   per-tick one is sub_17BFF50 (owner esi = v3, v3+0x70 = world):
//     0x017C012A  call sub_16090C0(player)        every fighter of the player's fight group has a target
//     0x017C013B  E8 30 8E E4 FF  call sub_1608F70 (ecx = fm, push player)   some fighter targets THE PLAYER  <- here
//     0x017C0142  jnz 0x017C021E                  yes: skip the "player left the fight" test
//     0x017C0148  call sub_17A9C30(v3); 0x017C0180..01D7: is any entity of the list v3+0x250 within
//                 sqrt(tuning(0x4DE6D48)+0x2AC) of the player? none -> 0x017C01FB: post E50 + E20 -> target dropped.
//   With every guard fighting the puppet, "some fighter targets the player" is false and the guards are near the
//   puppet, not the player, so the engine ends the fight once per tick. sub_1608F70 cannot simply be asked about the
//   puppet: it takes the fight group from its argument (+0xE8 -> +0x94), which is null for the puppet (see above).
//   With the flag on, the call answers "yes" also when the FightManager's current target record (+0x304, entity via
//   handle +0x14) is the puppet - the same branch the engine takes while the guards fight the player.
//   sub_1608F70 has one code reference (IDA, 2026-11-14 o80); thiscall, retn 4, result in al (0x017C0140 test al,al).
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
const uintptr_t VA_ANYTGT_CALL = 0x017C013B;
const BYTE kAnyTgtCall[5] = { 0xE8, 0x30, 0x8E, 0xE4, 0xFF };
const uintptr_t VA_ANYTGT = 0x01608F70;     // sub_1608F70: thiscall(fm, entity) -> al

DWORD g_poiTramp = 0, g_getPlayer = 0, g_cfResume = 0, g_cfExit = 0;
bool  g_poiInstalled = false, g_cfInstalled = false;
int   g_on = 0; ULONGLONG g_next = 0;
volatile LONG g_calls = 0, g_lent = 0, g_cfSkipped = 0;
bool  g_ckInstalled = false; int g_ckOn = 0; ULONGLONG g_ckNext = 0;
volatile LONG g_ckCalls = 0, g_ckKept = 0;

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

// Replaces `call sub_1608F70` at 0x017C013B. __fastcall with an unused edx = the callee's thiscall frame: ecx = fm,
// one stack argument (the player), callee pops 4 bytes; the caller only tests al.
char __fastcall KeepPuppetStub(void* fm, void* /*edx*/, void* ent)
{
    const char r = Hook::CallClassMethod<char, void*, void*>((unsigned long)Va(VA_ANYTGT), fm, ent);
    if (r) return r;
    __try {
        InterlockedIncrement(&g_ckCalls);
        const ULONGLONG now = GetTickCount64();
        if (now >= g_ckNext) { g_ckNext = now + 2000; g_ckOn = GetPrivateProfileIntA("Coop", "ConflictKeepPuppet", 0, Paths::Ini()) != 0; }
        if (!g_ckOn || !fm || !Phase1_IsEntityHost()) return r;
        void* pup = Phase1_PuppetEntity();
        if (!pup) return r;
        const char* rec = *(const char* const*)((const char*)fm + 0x304);        // FightManager target record
        if (!rec) return r;
        const int* h = *(const int* const*)(rec + 0x14);                         // entity handle
        const void* e = h ? (const void*)(uintptr_t)((h[2] >> 31) & h[0]) : nullptr;
        if (e != pup) return r;
        InterlockedIncrement(&g_ckKept);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

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
    if (GetPrivateProfileIntA("Coop", "ConflictKeepPuppet", -1, Paths::Ini()) >= 0) {   // key present: install, read live
        const uintptr_t c = Va(VA_ANYTGT_CALL);
        if (memcmp((const void*)c, kAnyTgtCall, 5) == 0) g_ckInstalled = PatchCall(c, (void*)&KeepPuppetStub);
        else OutputDebugStringA("[Poi] bytes mismatch at 0x17C013B\n");
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
    if ((!g_poiInstalled && !g_cfInstalled && !g_ckInstalled) || cap < 64) return 0;
    const int n = _snprintf_s(out, cap, _TRUNCATE,
                              "[Poi] on=%d lookups=%ld lentPuppet=%ld | conflictNullSkips=%ld | keepPuppet on=%d tests=%ld kept=%ld",
                              g_on, InterlockedExchange(&g_calls, 0), InterlockedExchange(&g_lent, 0),
                              InterlockedExchange(&g_cfSkipped, 0), g_ckOn, InterlockedExchange(&g_ckCalls, 0),
                              InterlockedExchange(&g_ckKept, 0));
    return n > 0 ? n : 0;
}

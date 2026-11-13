// [Coop] FightProbe (default 0, read at load; observation only). Which path removes the co-op puppet's fighter record.
// AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA 2026-11-13 (o57, o62):
//   sub_161F420 = FightManager RemoveFighter(this = FightManager (world+0x8DC), record): clears fightMgr+0x304 when the
//   record is the target. Callers: sub_16200F0 (20 xrefs), sub_1620220 (10), sub_1620750 (the update), 0x0161F86B.
//     0x0161F420  55 / 8B EC / 83 EC 28     push ebp / mov ebp,esp / sub esp,28h   <- 6 bytes stolen
//   record: role at +0x18 (sub_15FCC60, 7 = player character), entity via handle +0x14: (*(h+8) >> 31) & *h.
// Run 22628 (fightmgr.py): the FightManager took the puppet as its role-7 target and dropped it every 1-2 s, while the
// player's target held 24 s. Logs every RemoveFighter for the puppet's or the player's record with the return address
// and two EBP-chain return addresses above it (<= 300 lines), so the removing path is named by the binary.
#include "fight_probe.h"
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void Phase1_LogLine(const char* s);
void* Phase1_PuppetEntity();

namespace {

const uintptr_t VA_REMOVE = 0x0161F420;
const BYTE kPro[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28 };
DWORD g_tramp = 0;
volatile LONG g_lines = 0;

bool InStack(uintptr_t p)
{
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return p >= (uintptr_t)tib->StackLimit && p + 8 <= (uintptr_t)tib->StackBase;
}

void __cdecl OnRemove(void* fm, const char* rec, uintptr_t ret, uintptr_t ebp)
{
    __try {
        if (!rec || g_lines >= 300) return;
        const int* h = *(const int* const*)(rec + 0x14);
        const void* e = h ? (const void*)(uintptr_t)((h[2] >> 31) & h[0]) : nullptr;
        if (!e) return;
        void* pup = Phase1_PuppetEntity();
        void* pl = nullptr;
        World* w = World::GetCurrentWorld();
        if (w) pl = w->GetPlayer();
        const char* who = (pup && e == pup) ? "PUPPET" : (pl && e == pl) ? "PLAYER" : nullptr;
        if (!who) return;
        uintptr_t r1 = 0, r2 = 0, eb = ebp;
        if (InStack(eb)) { r1 = *(const uintptr_t*)(eb + 4); const uintptr_t n = *(const uintptr_t*)eb;
            if (n > eb && InStack(n)) r2 = *(const uintptr_t*)(n + 4); }
        LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
        char line[200];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[FightRm] t=%llu %s rec=%p role=%d fm=%p from=%08X <%08X <%08X\n",
                    (unsigned long long)((c.QuadPart / f.QuadPart) * 1000ull + (c.QuadPart % f.QuadPart) * 1000ull / f.QuadPart),
                    who, rec, *(const int*)(rec + 0x18), fm, (unsigned)ret, (unsigned)r1, (unsigned)r2);
        InterlockedIncrement(&g_lines);
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Entry: ECX = FightManager, [esp] = return, [esp+4] = record. Prologue not run yet: EBP is the caller's frame.
__declspec(naked) void Detour()
{
    __asm {
        pushad
        push ebp                    // caller's frame (arg 4)
        mov  eax, [esp + 36]        // ret (original esp+0)
        push eax
        mov  eax, [esp + 44]        // record (original esp+4)
        push eax
        push ecx                    // FightManager
        call OnRemove
        add  esp, 16
        popad
        jmp  dword ptr [g_tramp]
    }
}

}  // namespace

void FightProbe::Init()
{
    if (!GetPrivateProfileIntA("Coop", "FightProbe", 0, Paths::Ini())) return;
    const uintptr_t s = GameModuleBase + (VA_REMOVE - 0x00400000);
    if (memcmp((const void*)s, kPro, 6) != 0) { OutputDebugStringA("[FightProbe] bytes mismatch at 0x161F420\n"); return; }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kPro, 6);
    t[6] = 0x68; *reinterpret_cast<DWORD*>(t + 7) = (DWORD)(s + 6); t[11] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, 12);
    g_tramp = (DWORD)(uintptr_t)t;
    Hook::DetourFunc((BYTE*)s, (DWORD)(uintptr_t)&Detour, 6);
}

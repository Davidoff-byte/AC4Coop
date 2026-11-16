// [Coop] SenseOwnerGuard (default 1 since 2026-11-16, read at load; =0 turns it off): the perception sight check
// dereferences its owner entity unchecked. Live: run 32076 (asi ec00f440) fired twice (owner 0xBBD00000) and survived.
//
// Crash family seen 3 times on 2026-11-16 (WER dumps AC4BFSP.exe.30380 / .20292 / .22744, host and guest, cdb .ecxr):
//   30380  eip 0x0188F054  mov ecx,[esi+9Ch]   esi = 0            (sub_188F030 reads the entity kind)
//   20292  eip 0x0133BD03  inside sub_133F010, called from 0x0188F04F with that entity
//   22744  eip 0x00520FC0  inside sub_5220C0, called from 0x0188F07A with esi = 0x00937563 (a code address: garbage)
// All three come through 0x010390D8 in sub_1039070 (ida_batch cr1/cr2): `sub_1895290(&v26, target, *(this+120));
// sub_188F030(.., *(this+120))`. this = the perception object of sub_103D250 (its v5[30] = this+0x78 is the owner
// entity). The only gate before those calls is sub_101CAA0(this, target) (thiscall, retn 4, al = bool; ONE caller,
// 0x010390B0, ida_batch cr3); when it returns false sub_1039070 takes its own "not applicable" path (LABEL_16/17).
//
// The guard: at sub_101CAA0's entry, when *(this+0x78) is not a plausible object (outside [0x10000, 0x7FFF0000),
// unreadable, or its first dword not inside .rdata VA [0x01E3A000, 0x01E3A000+0x0090874A) - the bound deref_guard
// uses), return false. Valid owners run the original unchanged. Why the owner is null / stale is NOT established
// (UNVERIFIED: a perception object outliving its entity).
//
// Site: 0x0101CAA0  55 8B EC 53 57 | 8B 7D 08   push ebp; mov ebp,esp; push ebx; push edi  (5 bytes stolen whole)
#include "Hook.h"
#include "paths.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void Phase1_LogLine(const char* s);

namespace SenseGuard { void Init(); int FormatReport(char* out, int cap); }

namespace {
const uintptr_t VA_GATE = 0x0101CAA0;
const BYTE kPro[5] = { 0x55, 0x8B, 0xEC, 0x53, 0x57 };
const uintptr_t RDATA_LO = 0x01E3A000, RDATA_HI = 0x01E3A000 + 0x0090874A;
DWORD g_tramp = 0;
bool g_on = false;
volatile LONG g_hits = 0, g_total = 0;
volatile uintptr_t g_lastBad = 0, g_lastSelf = 0;
}

extern "C" int __cdecl SenseOwnerBad(uintptr_t self)
{
    __try {
        const uintptr_t e = *(const uintptr_t*)(self + 0x78);
        bool bad = e < 0x10000 || e >= 0x7FFF0000;
        if (!bad) { const uintptr_t vt = *(const uintptr_t*)e; bad = vt < RDATA_LO || vt >= RDATA_HI; }
        if (bad) { g_lastBad = e; g_lastSelf = self; InterlockedIncrement(&g_hits); InterlockedIncrement(&g_total); }
        return bad ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastSelf = self; InterlockedIncrement(&g_hits); InterlockedIncrement(&g_total);
        return 1;
    }
}

namespace {
__declspec(naked) void Detour()
{
    __asm {
        pushad
        push ecx
        call SenseOwnerBad
        add  esp, 4
        test eax, eax
        popad
        jnz  sg_skip
        jmp  dword ptr [g_tramp]
    sg_skip:
        xor  al, al
        ret  4
    }
}
}  // namespace

void SenseGuard::Init()
{
    if (!GetPrivateProfileIntA("Coop", "SenseOwnerGuard", 1, Paths::Ini())) return;
    BYTE* site = (BYTE*)(GameModuleBase + (VA_GATE - 0x00400000));
    char line[160];
    if (memcmp(site, kPro, 5) != 0) {
        Phase1_LogLine("[SG] sub_101CAA0 prologue mismatch - SenseOwnerGuard not installed\n");
        return;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kPro, 5);
    t[5] = 0x68; *(DWORD*)(t + 6) = (DWORD)(uintptr_t)(site + 5); t[10] = 0xC3;   // push resume; ret
    FlushInstructionCache(GetCurrentProcess(), t, 11);
    g_tramp = (DWORD)(uintptr_t)t;
    g_on = Hook::DetourFunc(site, (DWORD)(uintptr_t)&Detour, 5);
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[SG] SenseOwnerGuard=1: sub_101CAA0 0x0101CAA0 hooked=%d\n", g_on ? 1 : 0);
    Phase1_LogLine(line);
}

void SenseGuardInit() { SenseGuard::Init(); }
int SenseGuardReport(char* out, int cap) { return SenseGuard::FormatReport(out, cap); }

int SenseGuard::FormatReport(char* out, int cap)
{
    if (!g_on) return 0;
    const LONG h = InterlockedExchange(&g_hits, 0);
    if (!h) return 0;   // quiet unless it fired in this window
    return _snprintf_s(out, cap, _TRUNCATE, "[SG] sight checks skipped (owner not an object): %ld this window, %ld total; last owner=%08X self=%08X",
                       h, g_total, (unsigned)g_lastBad, (unsigned)g_lastSelf);
}

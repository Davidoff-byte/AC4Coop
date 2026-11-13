// Action-writer probe. Hook site (AC4BFSP.exe, md5 2058342866688f780c8b34526a65bc35, IDA batch 2026-11-13):
//
//   0x00651C20  55        push ebp            <- 6 bytes stolen whole
//   0x00651C21  8B EC     mov  ebp, esp
//   0x00651C23  8B 45 08  mov  eax, [ebp+8]
//   0x00651C26  ...                           <- resume
//
// int __thiscall sub_651C20(ActionComponent* this, Action* a2, int, int, int, int, int, char) - the evaluator
// switch. xrefs_to 0x651C20 = exactly 4 callers, so every action switch passes here:
//   0x00658CF8 in sub_658CA0  (from sub_65DCB0 <- SetTargetAction 0x6625D0, sub_6B0090, sub_662210, sub_6623B0,
//                              sub_666B30; and from sub_65DC80)
//   0x00658E5C in sub_658D80  (default-action fallback, frame step registered at 0x5870BF in sub_5863D0)
//   0x006705B5 in sub_670150, 0x0067117B in sub_670D80
// sub_669220 (ActionPostUpdate auto-transition, reached from frame step sub_6695F0, registered at 0x587DA3 /
// 0x587E35) switches via sub_6B0090 / sub_648BD0 -> sub_65DCB0. `this` is the ActionComponent (same +0x4C
// controller lists sub_658D80 and sub_669220 use); the alias is Action+0x08.
//
// The detour records, for switches that land on a body we drive (NoteMirrored) and differ from the host's alias,
// the return address and the EBP-chain return addresses above it (functions without an EBP frame end the chain),
// and counts them by that 4-address key. It calls no engine function and changes nothing.
#include "action_writer.h"
#include "Hook.h"
#include "paths.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {

const uintptr_t VA_SWITCH = 0x00651C20;
const uintptr_t STEAL_LEN = 6;
const BYTE kPrologue[STEAL_LEN] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };

DWORD g_tramp = 0;
bool  g_installed = false;

// Mirrored bodies: ActionComponent* -> host alias, refreshed by the body's own update.
const int MIR_MAX = 128;
volatile uintptr_t g_mirAc[MIR_MAX];
volatile uint32_t  g_mirAlias[MIR_MAX];
volatile ULONGLONG g_mirMs[MIR_MAX];
SRWLOCK g_mirLock = SRWLOCK_INIT;

__declspec(thread) int t_own = 0;

volatile LONG g_switch = 0, g_toHost = 0, g_other = 0, g_own = 0;

struct Site { uintptr_t r[4]; LONG n; uint32_t lastAlias, lastHost; };
const int SITE_MAX = 64;
Site    g_site[SITE_MAX];
int     g_nSite = 0;
LONG    g_siteDrop = 0;
SRWLOCK g_siteLock = SRWLOCK_INIT;

bool InStack(uintptr_t p)
{
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return p >= (uintptr_t)tib->StackLimit && p + 8 <= (uintptr_t)tib->StackBase;
}

void Record(uintptr_t ret, uintptr_t ebp, uint32_t alias, uint32_t host)
{
    uintptr_t r[4] = { ret, 0, 0, 0 };
    uintptr_t e = ebp;
    for (int k = 1; k < 4; ++k) {
        if (!InStack(e)) break;
        const uintptr_t next = *(const uintptr_t*)e;
        const uintptr_t ra   = *(const uintptr_t*)(e + 4);
        if (ra < 0x00400000 || ra >= 0x05300000) break;   // not a return into the exe image
        r[k] = ra;
        if (next <= e) break;                             // the chain must grow toward the stack base
        e = next;
    }
    AcquireSRWLockExclusive(&g_siteLock);
    int i = 0;
    for (; i < g_nSite; ++i)
        if (!memcmp(g_site[i].r, r, sizeof(r))) break;
    if (i == g_nSite) {
        if (g_nSite < SITE_MAX) { memcpy(g_site[i].r, r, sizeof(r)); g_site[i].n = 0; ++g_nSite; }
        else { ++g_siteDrop; i = -1; }
    }
    if (i >= 0) { ++g_site[i].n; g_site[i].lastAlias = alias; g_site[i].lastHost = host; }
    ReleaseSRWLockExclusive(&g_siteLock);
}

void __cdecl OnSwitch(void* ac, void* action, uintptr_t ret, uintptr_t ebp)
{
    __try {
        if (t_own) { InterlockedIncrement(&g_own); return; }
        const uintptr_t a = (uintptr_t)ac;
        const ULONGLONG nowMs = GetTickCount64();
        int slot = -1;
        for (int i = 0; i < MIR_MAX; ++i)
            if (g_mirAc[i] == a) { slot = i; break; }
        if (slot < 0 || nowMs - g_mirMs[slot] > 1000) return;
        const uint32_t host = g_mirAlias[slot];
        const uint32_t alias = action ? *(const uint32_t*)((const char*)action + 8) : 0;
        InterlockedIncrement(&g_switch);
        if (alias == host) { InterlockedIncrement(&g_toHost); return; }
        InterlockedIncrement(&g_other);
        Record(ret, ebp, alias, host);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Entry: ECX = this, [esp] = return address, [esp+4] = a2 (Action*). All registers preserved; then the
// stolen prologue runs in the trampoline and execution continues at 0x00651C26.
__declspec(naked) void Detour()
{
    __asm {
        pushad                      // 32 bytes; original esp+0 (ret) is now at esp+32
        push ebp                    // arg 4: the caller's frame pointer (prologue not run yet)
        mov  eax, [esp + 36]        // ret
        push eax                    // arg 3
        mov  eax, [esp + 44]        // a2 (original esp+4)
        push eax                    // arg 2
        push ecx                    // arg 1: this
        call OnSwitch
        add  esp, 16
        popad
        jmp  dword ptr [g_tramp]
    }
}

BYTE* BuildTrampoline(uintptr_t resume)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, kPrologue, STEAL_LEN);
    t[STEAL_LEN] = 0x68;                                       // push imm32; ret = absolute jump
    *reinterpret_cast<DWORD*>(t + STEAL_LEN + 1) = (DWORD)resume;
    t[STEAL_LEN + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, STEAL_LEN + 6);
    return t;
}

}  // namespace

void ActionWriter::Init()
{
    if (!GetPrivateProfileIntA("Coop", "ActionWriterProbe", 0, Paths::Ini())) return;
    const uintptr_t site = GameModuleBase + (VA_SWITCH - 0x00400000);
    if (memcmp((const void*)site, kPrologue, STEAL_LEN) != 0) {
        OutputDebugStringA("[ActWriter] prologue mismatch at 0x651C20 - not installed\n");
        return;
    }
    BYTE* t = BuildTrampoline(site + STEAL_LEN);
    if (!t) return;
    g_tramp = (DWORD)(uintptr_t)t;
    if (!Hook::DetourFunc((BYTE*)site, (DWORD)(uintptr_t)&Detour, (DWORD)STEAL_LEN)) return;
    g_installed = true;
    OutputDebugStringA("[ActWriter] installed at 0x651C20\n");
}

bool ActionWriter::Installed() { return g_installed; }

void ActionWriter::NoteMirrored(void* ac, uint32_t hostAlias)
{
    if (!g_installed || !ac) return;
    const uintptr_t a = (uintptr_t)ac;
    const ULONGLONG nowMs = GetTickCount64();
    AcquireSRWLockExclusive(&g_mirLock);
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < MIR_MAX; ++i) {
        if (g_mirAc[i] == a) { slot = i; break; }
        if (freeSlot < 0 && (g_mirAc[i] == 0 || nowMs - g_mirMs[i] > 5000)) freeSlot = i;
    }
    if (slot < 0) slot = freeSlot;
    if (slot >= 0) { g_mirAlias[slot] = hostAlias; g_mirMs[slot] = nowMs; g_mirAc[slot] = a; }
    ReleaseSRWLockExclusive(&g_mirLock);
}

void ActionWriter::SetOwn(bool on) { t_own = on ? 1 : 0; }

int ActionWriter::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    Site top[6] = {}; int nTop = 0; LONG drop = 0;
    AcquireSRWLockExclusive(&g_siteLock);
    for (int i = 0; i < g_nSite; ++i) {
        int k;
        if (nTop < 6) k = nTop++;
        else if (g_site[i].n > top[5].n) k = 5;
        else continue;
        top[k] = g_site[i];
        for (; k > 0 && top[k - 1].n < top[k].n; --k) { const Site s = top[k]; top[k] = top[k - 1]; top[k - 1] = s; }
    }
    g_nSite = 0; drop = g_siteDrop; g_siteDrop = 0;
    ReleaseSRWLockExclusive(&g_siteLock);
    int n = _snprintf_s(out, cap, _TRUNCATE, "[ActWriter] mirrored switches=%ld toHost=%ld other=%ld own=%ld sitesDropped=%ld |",
                        InterlockedExchange(&g_switch, 0), InterlockedExchange(&g_toHost, 0),
                        InterlockedExchange(&g_other, 0), InterlockedExchange(&g_own, 0), drop);
    for (int i = 0; i < nTop && n > 0 && n < cap - 80; ++i)
        n += _snprintf_s(out + n, cap - n, _TRUNCATE, " %ld:%08X<%08X<%08X<%08X(a=%u h=%u)", top[i].n,
                         (unsigned)top[i].r[0], (unsigned)top[i].r[1], (unsigned)top[i].r[2], (unsigned)top[i].r[3],
                         top[i].lastAlias, top[i].lastHost);
    return n > 0 ? n : 0;
}

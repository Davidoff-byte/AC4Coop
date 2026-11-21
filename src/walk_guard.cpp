#include "walk_guard.h"

#include <windows.h>
#include <process.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <share.h>
#include <cstring>

#include "Hook.h"
#include "paths.h"
#include "build_gate.h"

namespace {

// ---- the two leaves, as verified against AC4BFSP.exe.i64 ---------------------------------------
//
// Both are __thiscall with a single stack dword (IDA: "retn 4"), and both open with the SAME 8 bytes:
//
//   55               push ebp
//   8B EC            mov  ebp,esp
//   8B 45 08         mov  eax,[ebp+8]      ; the caller's transform pointer
//   50               push eax
//   51               push ecx             ; the L being walked
//
// then the unchecked dereference we are guarding. Both sites have the SAME instruction at +0x08:
//
//   75A1B8  8B 89 A0 00 00 00   mov ecx,[ecx+0A0h]   (ModRM 0x89 = mod 10 / reg 001 = ECX)
//   75A1F8  8B 89 A0 00 00 00   mov ecx,[ecx+0A0h]
//
// 8 bytes is a whole-instruction prefix, so nothing has to be split for the 5-byte E9.
const BYTE PROLOGUE[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x50, 0x51 };
const BYTE RESUME[6]   = { 0x8B, 0x89, 0xA0, 0x00, 0x00, 0x00 };   // mov ecx,[ecx+0A0h]
const BYTE EPILOGUE[4] = { 0x5D, 0xC2, 0x04, 0x00 };               // pop ebp; retn 4

volatile LONG g_hitsF = 0;   // sub_75A1F0 null hits (written by the cave)
volatile LONG g_hitsB = 0;   // sub_75A1B0 null hits

struct Target {
    const char*    name;
    uintptr_t      va;
    DWORD          epilogueOff;    // offset of `pop ebp; retn 4` from the entry
    volatile LONG* hits;
};

const Target kTargets[2] = {
    { "sub_75A1F0", 0x0075A1F0, 0x19, &g_hitsF },   // 5D C2 04 00 at 0x0075A209 (size 0x1D)
    { "sub_75A1B0", 0x0075A1B0, 0x16, &g_hitsB },   // 5D C2 04 00 at 0x0075A1C6 (size 0x1A)
};

// ---- cave layout --------------------------------------------------------------------------------
//
// One 0x20-byte guard per target, so 0x40 of code in a 0x100 cave:
//
//   00  85 C9                 test ecx,ecx
//   02  75 0C                 jnz  +0x0C          ; non-null -> original prologue at 0x10
//   04  F0 FF 05 <imm32>      lock inc dword ptr [hits]
//   0B  33 C0                 xor  eax,eax
//   0D  C2 04 00              ret  4              ; the original `pop ebp; retn 4`, minus the EBP push
//   10  55                    push ebp            \  the 8 stolen bytes, re-executed verbatim
//   11  8B EC                 mov  ebp,esp        |
//   13  8B 45 08              mov  eax,[ebp+8]    |
//   16  50                    push eax            |
//   17  51                    push ecx            /
//   18  E9 <rel32>            jmp  site+8         ; the guarded `mov ecx,[ecx+0A0h]`
//
// 0x1D bytes used, padded to 0x20 by the stride. The null path never touches EBP, so `ret 4` leaves the
// stack exactly as the original epilogue does (it also discards the one stack argument). On the non-null
// path EAX/ECX/EBP are left for the stolen bytes to write, so the detour is invisible; the only visible
// difference is the flags word from `test`, and no instruction before the next flag write reads it.
const size_t GUARD_SIZE = 0x20;
const size_t CAVE_SIZE  = 0x100;

// A rel32 patch reaches +-2 GB; require a margin so the boundary itself is never used.
const uintptr_t MAX_CAVE_DELTA = 0x7FFF0000;
const uintptr_t CAVE_HINTS[] = { 0x20000000, 0x24000000, 0x30000000, 0x50000000, 0x10000000 };

bool     g_entry     = false;          // Init() runs at most once
bool     g_installed = false;
FILE*    g_log       = nullptr;
LONG     g_lastLogged = 0;
uint64_t g_firstHitTick = 0;

void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

int32_t Rel32(const void* fromNext, const void* target)
{
    return (int32_t)((intptr_t)target - (intptr_t)fromNext);
}

bool InRange(const BYTE* cave, uintptr_t site)
{
    const uintptr_t d = (uintptr_t)cave > site ? (uintptr_t)cave - site : site - (uintptr_t)cave;
    return d < MAX_CAVE_DELTA;
}

// VirtualAlloc, then reject (and retry with hints) any block the E9 from either site cannot reach.
BYTE* AllocateCave()
{
    BYTE* cave = (BYTE*)VirtualAlloc(nullptr, CAVE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (cave && InRange(cave, kTargets[0].va) && InRange(cave, kTargets[1].va)) {
        Log("[WalkGuard] cave (unhinted) = %p\n", cave);
        return cave;
    }
    if (cave) {
        Log("[WalkGuard] cave %p out of rel32 range, retrying with hints\n", cave);
        VirtualFree(cave, 0, MEM_RELEASE);
    }

    for (size_t h = 0; h < sizeof(CAVE_HINTS) / sizeof(CAVE_HINTS[0]); ++h) {
        cave = (BYTE*)VirtualAlloc((LPVOID)CAVE_HINTS[h], CAVE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
        if (!cave) continue;
        if (InRange(cave, kTargets[0].va) && InRange(cave, kTargets[1].va)) {
            Log("[WalkGuard] cave (hint 0x%08X) = %p\n", (unsigned)CAVE_HINTS[h], cave);
            return cave;
        }
        Log("[WalkGuard] cave %p out of rel32 range, next hint\n", cave);
        VirtualFree(cave, 0, MEM_RELEASE);
    }
    return nullptr;
}

void BuildGuard(BYTE* g, uintptr_t site, volatile LONG* hits)
{
    g[0x00] = 0x85; g[0x01] = 0xC9;                             // test ecx,ecx
    g[0x02] = 0x75; g[0x03] = 0x0C;                             // jnz +0x0C (index 4 -> 0x10)
    g[0x04] = 0xF0; g[0x05] = 0xFF; g[0x06] = 0x05;             // lock inc dword ptr [hits]
    *reinterpret_cast<uint32_t*>(g + 0x07) = (uint32_t)(uintptr_t)hits;
    g[0x0B] = 0x33; g[0x0C] = 0xC0;                             // xor eax,eax
    g[0x0D] = 0xC2; g[0x0E] = 0x04; g[0x0F] = 0x00;             // ret 4
    memcpy(g + 0x10, PROLOGUE, sizeof(PROLOGUE));               // the 8 stolen bytes
    g[0x18] = 0xE9;                                             // jmp site+8
    *reinterpret_cast<int32_t*>(g + 0x19) = Rel32(g + 0x1D, (const void*)(site + 8));
}

unsigned __stdcall ReporterThread(void*)
{
    for (;;) {
        Sleep(1000);                                            // at most one line per second
        if (!g_installed) continue;
        __try {
            const LONG n = g_hitsF + g_hitsB;
            if (n <= g_lastLogged) continue;
            if (!g_firstHitTick) g_firstHitTick = GetTickCount64();
            g_lastLogged = n;
            Log("[WalkGuard] null L hit n=%ld (first at t=%llu)\n",
                (long)n, (unsigned long long)g_firstHitTick);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Never let the reporter take the process down.
        }
    }
    return 0;
}

bool Install()
{
    // Byte-verify BOTH functions -- prologue, the guarded dereference, and the epilogue the guard
    // mirrors -- before touching either. A mismatch means the layout is not the one this guard was
    // written for, so neither site is patched.
    for (int i = 0; i < 2; ++i) {
        const Target& t = kTargets[i];
        const BYTE* p = (const BYTE*)t.va;
        const BYTE* r = (const BYTE*)(t.va + 8);
        const BYTE* e = (const BYTE*)(t.va + t.epilogueOff);

        if (memcmp(p, PROLOGUE, sizeof(PROLOGUE)) != 0 ||
            memcmp(r, RESUME, sizeof(RESUME)) != 0 ||
            memcmp(e, EPILOGUE, sizeof(EPILOGUE)) != 0) {
            Log("[WalkGuard] install FAILED: %s @ 0x%08X: prologue %02X %02X %02X %02X %02X %02X %02X %02X, "
                "resume %02X %02X %02X %02X %02X %02X, epilogue(+0x%X) %02X %02X %02X %02X -- "
                "nothing patched\n",
                t.name, (unsigned)t.va, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                r[0], r[1], r[2], r[3], r[4], r[5],
                (unsigned)t.epilogueOff, e[0], e[1], e[2], e[3]);
            return false;
        }
    }

    BYTE* cave = AllocateCave();
    if (!cave) {
        Log("[WalkGuard] install FAILED: no cave within rel32 range\n");
        return false;
    }

    for (int i = 0; i < 2; ++i)
        BuildGuard(cave + i * GUARD_SIZE, kTargets[i].va, kTargets[i].hits);
    FlushInstructionCache(GetCurrentProcess(), cave, 2 * GUARD_SIZE);

    for (int i = 0; i < 2; ++i) {
        // E9 rel32 over the 8-byte prologue (Hook::DetourFunc NOP-fills the 3 bytes behind the jump).
        if (!Hook::DetourFunc((BYTE*)kTargets[i].va, (DWORD)(uintptr_t)(cave + i * GUARD_SIZE), 8)) {
            Log("[WalkGuard] install FAILED: DetourFunc %s @ 0x%08X\n",
                kTargets[i].name, (unsigned)kTargets[i].va);
            // The first site may already be patched; leave it -- it is the same guard, and unpatching
            // half of a pair is worse than leaving a working detour in place.
            return false;
        }
    }

    Log("[WalkGuard] installed 75A1F0=%p 75A1B0=%p (null L -> return)\n",
        (void*)kTargets[0].va, (void*)kTargets[1].va);
    g_installed = true;
    return true;
}

} // namespace

void WalkGuard::Init()
{
    if (g_entry) return;
    g_entry = true;

    // [Coop] WalkNullGuard, default 1. Read before anything else: with the flag at 0 this unit does
    // exactly one GetPrivateProfileIntA and returns -- no log file, no allocation, no patch, no thread.
    if (GetPrivateProfileIntA("Coop", "WalkNullGuard", 1, Paths::Ini()) == 0) {
        OutputDebugStringA("[WalkGuard] disabled by [Coop] WalkNullGuard=0\n");
        return;
    }

    // Fail closed on patching, not on loading: an unverified build keeps this unit silent.
    if (!BuildGate::Trusted()) {
        OutputDebugStringA("[WalkGuard] skipped: build not trusted\n");
        return;
    }

    // Several processes in AC4's launch chain load this ASI; only the real game has the module.
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) {
        OutputDebugStringA("[WalkGuard] install skipped: ac4bfsp.exe not loaded\n");
        return;
    }

    if (!g_log) {
        char leaf[64];
        _snprintf_s(leaf, sizeof(leaf), _TRUNCATE,
                    "AC4Coop_walk_guard_%u.log", (unsigned)GetCurrentProcessId());
        g_log = _fsopen(Paths::LogPath(leaf), "a", _SH_DENYWR);
        if (!g_log) Paths::NoteOpenFailure(Paths::LogPath(leaf));
    }

    if (!Install()) return;

    // _beginthreadex, not CreateThread: the reporter calls CRT functions (fputs/_vsnprintf_s) and
    // CreateThread does not initialise the CRT's per-thread state.
    _beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, nullptr);
}

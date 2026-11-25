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

// ---- [Coop] WalkNullGuardExt (default 0): the three sibling leaves that return a 16-byte vector ------------------
// 2026-11-22 soak (asi db928886, guest pid 32568, dump AC4BFSP.exe.32568.dmp): the guest died at 0x0075A1D3
// `mov eax,[ecx+0A0h]` with ecx = 0, on the world-update worker (caller 0x0042610A in sub_4260A0, ecx = [esi+0x134]),
// although both guards above were installed and had fired twice. A byte scan of 0x759800-0x75B000 for
// `8B 81|89 A0 00 00 00` finds five leaves; three are unguarded (IDA jobs p6/p7/p8):
//   sub_75A1D0  55 8B EC 8B 81 A0 00 00 00  ...  8B C6 5E 5D C2 04 00   thiscall(L, out), *out = vec4, returns out
//   sub_75A210  53 8B DC 83 EC 08 ...       ...  8B C1 ... 5B C2 04 00   thiscall(L, out), *out = 2 qwords, returns out
//   sub_75A2E0  53 8B DC 83 EC 08 ...       ...  5F 5E ... 5B C2 08 00   (L, idx, out), *out = vec4; retn 8
// Null L: write (0,0,0,1) to the output (identity as a rotation, a harmless point as a position) and return it.
// UNVERIFIED: which of the two the caller expects; both are bounded values, unlike the stale stack garbage the
// caller would otherwise read.
struct VecTarget {
    const char*    name;
    uintptr_t      va;
    BYTE           stolen[9];
    BYTE           stolenLen;      // whole instructions, no relative operands
    BYTE           outOff;         // [esp+outOff] at entry = the output pointer
    BYTE           retPop;         // ret N
    DWORD          epilogueOff;    // offset of the verified epilogue bytes
    BYTE           epilogue[3];    // C2 nn 00
    volatile LONG* hits;
};
volatile LONG g_hitsD0 = 0, g_hits210 = 0, g_hits2E0 = 0;
const VecTarget kVecTargets[3] = {
    { "sub_75A1D0", 0x0075A1D0, { 0x55, 0x8B, 0xEC, 0x8B, 0x81, 0xA0, 0x00, 0x00, 0x00 }, 9, 4, 4, 0x1B, { 0xC2, 0x04, 0x00 }, &g_hitsD0 },
    { "sub_75A210", 0x0075A210, { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 },                   6, 4, 4, 0x4C, { 0xC2, 0x04, 0x00 }, &g_hits210 },   // C2 at +0x4C (byte count of the job p6 dump; 0x4E refused live)
    { "sub_75A2E0", 0x0075A2E0, { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 },                   6, 8, 8, 0xE7, { 0xC2, 0x08, 0x00 }, &g_hits2E0 },
};
const size_t VEC_GUARD_SIZE = 0x40;
const size_t VEC_CAVE_SIZE  = 0x100;   // 3 x 0x40

// 00 test ecx,ecx / 02 jnz tramp / 04 mov eax,[esp+out] / 08 test eax,eax / 0A jz +0x1B /
// 0C..26 four dword stores (0,0,0,1.0f) / 27 lock inc [hits] / 2E ret N / 31 stolen bytes / jmp site+len
void BuildVecGuard(BYTE* g, const VecTarget& t)
{
    size_t i = 0;
    g[i++] = 0x85; g[i++] = 0xC9;                                       // test ecx,ecx
    g[i++] = 0x75; g[i++] = 0x2D;                                       // jnz 0x31
    g[i++] = 0x8B; g[i++] = 0x44; g[i++] = 0x24; g[i++] = t.outOff;     // mov eax,[esp+out]
    g[i++] = 0x85; g[i++] = 0xC0;                                       // test eax,eax
    g[i++] = 0x74; g[i++] = 0x1B;                                       // jz 0x27
    g[i++] = 0xC7; g[i++] = 0x00; *reinterpret_cast<uint32_t*>(g + i) = 0; i += 4;                     // [eax]    = 0
    g[i++] = 0xC7; g[i++] = 0x40; g[i++] = 0x04; *reinterpret_cast<uint32_t*>(g + i) = 0; i += 4;      // [eax+4]  = 0
    g[i++] = 0xC7; g[i++] = 0x40; g[i++] = 0x08; *reinterpret_cast<uint32_t*>(g + i) = 0; i += 4;      // [eax+8]  = 0
    g[i++] = 0xC7; g[i++] = 0x40; g[i++] = 0x0C; *reinterpret_cast<uint32_t*>(g + i) = 0x3F800000; i += 4; // [eax+C] = 1.0f
    g[i++] = 0xF0; g[i++] = 0xFF; g[i++] = 0x05;                       // lock inc dword ptr [hits]
    *reinterpret_cast<uint32_t*>(g + i) = (uint32_t)(uintptr_t)t.hits; i += 4;
    g[i++] = 0xC2; g[i++] = t.retPop; g[i++] = 0x00;                   // ret N   (i == 0x31 here)
    memcpy(g + i, t.stolen, t.stolenLen); i += t.stolenLen;            // stolen prologue
    g[i] = 0xE9;                                                        // jmp site+len
    *reinterpret_cast<int32_t*>(g + i + 1) = Rel32(g + i + 5, (const void*)(t.va + t.stolenLen));
}

bool g_extInstalled = false;
void InstallVecGuards()
{
    bool ok[3] = {};
    for (int i = 0; i < 3; ++i) {
        const VecTarget& t = kVecTargets[i];
        const BYTE* p = (const BYTE*)t.va;
        const BYTE* e = (const BYTE*)(t.va + t.epilogueOff);
        ok[i] = memcmp(p, t.stolen, t.stolenLen) == 0 && memcmp(e, t.epilogue, 3) == 0;
        if (!ok[i])
            Log("[WalkGuard] ext %s @ 0x%08X bytes mismatch (head %02X %02X %02X %02X %02X %02X, epi %02X %02X %02X) - skipped\n",
                t.name, (unsigned)t.va, p[0], p[1], p[2], p[3], p[4], p[5], e[0], e[1], e[2]);
    }
    BYTE* cave = (BYTE*)VirtualAlloc(nullptr, VEC_CAVE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (size_t h = 0; cave && !(InRange(cave, kVecTargets[0].va)) && h < sizeof(CAVE_HINTS) / sizeof(CAVE_HINTS[0]); ++h) {
        VirtualFree(cave, 0, MEM_RELEASE);
        cave = (BYTE*)VirtualAlloc((LPVOID)CAVE_HINTS[h], VEC_CAVE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!cave || !InRange(cave, kVecTargets[0].va)) { Log("[WalkGuard] ext install FAILED: no cave in range\n"); return; }
    for (int i = 0; i < 3; ++i) if (ok[i]) BuildVecGuard(cave + i * VEC_GUARD_SIZE, kVecTargets[i]);
    FlushInstructionCache(GetCurrentProcess(), cave, VEC_CAVE_SIZE);
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        if (!ok[i]) continue;
        if (Hook::DetourFunc((BYTE*)kVecTargets[i].va, (DWORD)(uintptr_t)(cave + i * VEC_GUARD_SIZE), kVecTargets[i].stolenLen)) ++n;
        else Log("[WalkGuard] ext install FAILED: DetourFunc %s\n", kVecTargets[i].name);
    }
    g_extInstalled = n > 0;
    Log("[WalkGuard] ext installed %d/3 (75A1D0 75A210 75A2E0: null L -> out=(0,0,0,1)) cave=%p\n", n, cave);
}


// ---- [Coop] ReleaseStaleGuard (default 0): the fast-travel crash in the shared-resource registry ------------------
// Guest dumps 31660 (2026-11-22) and 24860 (2026-11-25, full memory): leaving the sea world 73CA8CEE by fast travel, the
// world-update worker calls sub_773B60(registry dword_2AC08A8, component) (from sub_50A240, slot 7 of class vt 0x01E4B160);
// the record's count reaches 0 and sub_76E0F0 destroys the record's object through its vtable - but that object had been
// destroyed already (its vtable word held a heap address, 0xFCD37C50 -> call 0xFCD38190). Registration is the class's clone
// (sub_517270 -> sub_773A40); who releases once too often is not proven. sub_76E0F0 has ONE caller (0x773C16), so this guard
// only touches that registry: before the destroy, the object's vtable must lie inside the game image; if not, the destroy and
// the free of the dead object are skipped and the function continues at its own tail (0x76E124: clear holder, release the
// record's other members), exactly the null-object path. A live object is untouched.
//   76E0F0  56 57 8B F9 8B 37   push esi; push edi; mov edi,ecx; mov esi,[edi]   (stolen, whole instructions)
//   76E0F6  85 F6 74 2A         test esi,esi; jz 76E124                          (resume point)
const uintptr_t REL_SITE   = 0x0076E0F0;
const uintptr_t REL_RESUME = 0x0076E0F6;
const uintptr_t REL_TAIL   = 0x0076E124;
const BYTE REL_STOLEN[6]   = { 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x37 };
const BYTE REL_RESUMEB[4]  = { 0x85, 0xF6, 0x74, 0x2A };
volatile LONG g_hitsRel = 0;
volatile LONG g_relLastVt = 0, g_relLastObj = 0;
uintptr_t g_imgLo = 0, g_imgHi = 0;
bool g_relInstalled = false;

int __cdecl RelObjAlive(const void* obj)
{
    uint32_t vt = 0;
    __try { vt = *(const uint32_t*)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; }
    if (vt >= g_imgLo && vt < g_imgHi) return 1;
    InterlockedExchange(&g_relLastVt, (LONG)vt);
    InterlockedExchange(&g_relLastObj, (LONG)(uintptr_t)obj);
    return 0;
}

void InstallReleaseGuard(HMODULE game)
{
    const BYTE* p = (const BYTE*)REL_SITE;
    if (memcmp(p, REL_STOLEN, sizeof(REL_STOLEN)) != 0 || memcmp((const BYTE*)REL_RESUME, REL_RESUMEB, sizeof(REL_RESUMEB)) != 0) {
        Log("[WalkGuard] release guard bytes mismatch at 0x%08X (%02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X) - skipped\n",
            (unsigned)REL_SITE, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
        return;
    }
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)game;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)game + dos->e_lfanew);
    g_imgLo = (uintptr_t)game; g_imgHi = g_imgLo + nt->OptionalHeader.SizeOfImage;
    BYTE* g = (BYTE*)VirtualAlloc(nullptr, 0x40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (size_t h = 0; g && !InRange(g, REL_SITE) && h < sizeof(CAVE_HINTS) / sizeof(CAVE_HINTS[0]); ++h) {
        VirtualFree(g, 0, MEM_RELEASE);
        g = (BYTE*)VirtualAlloc((LPVOID)CAVE_HINTS[h], 0x40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!g || !InRange(g, REL_SITE)) { Log("[WalkGuard] release guard FAILED: no cave in range\n"); return; }
    size_t i = 0;
    memcpy(g, REL_STOLEN, sizeof(REL_STOLEN)); i += sizeof(REL_STOLEN);              // 00 stolen prologue
    g[i++] = 0x85; g[i++] = 0xF6;                                                   // 06 test esi,esi
    g[i++] = 0x0F; g[i++] = 0x84; *reinterpret_cast<int32_t*>(g + i) = Rel32(g + i + 4, (const void*)REL_TAIL); i += 4;   // jz tail
    g[i++] = 0x60;                                                                  // pushad
    g[i++] = 0x56;                                                                  // push esi
    g[i++] = 0xB8; *reinterpret_cast<uint32_t*>(g + i) = (uint32_t)(uintptr_t)&RelObjAlive; i += 4;   // mov eax,RelObjAlive
    g[i++] = 0xFF; g[i++] = 0xD0;                                                   // call eax
    g[i++] = 0x83; g[i++] = 0xC4; g[i++] = 0x04;                                   // add esp,4
    g[i++] = 0x89; g[i++] = 0x44; g[i++] = 0x24; g[i++] = 0x1C;                    // mov [esp+1Ch],eax (saved EAX)
    g[i++] = 0x61;                                                                  // popad
    g[i++] = 0x85; g[i++] = 0xC0;                                                   // test eax,eax
    g[i++] = 0x0F; g[i++] = 0x85; *reinterpret_cast<int32_t*>(g + i) = Rel32(g + i + 4, (const void*)REL_RESUME); i += 4; // jnz resume
    g[i++] = 0xF0; g[i++] = 0xFF; g[i++] = 0x05; *reinterpret_cast<uint32_t*>(g + i) = (uint32_t)(uintptr_t)&g_hitsRel; i += 4; // lock inc [hits]
    g[i++] = 0xE9; *reinterpret_cast<int32_t*>(g + i) = Rel32(g + i + 4, (const void*)REL_TAIL); i += 4;               // jmp tail
    FlushInstructionCache(GetCurrentProcess(), g, 0x40);
    if (!Hook::DetourFunc((BYTE*)REL_SITE, (DWORD)(uintptr_t)g, 6)) { Log("[WalkGuard] release guard FAILED: DetourFunc\n"); return; }
    g_relInstalled = true;
    Log("[WalkGuard] release guard installed 76E0F0 (dead object in the registry release -> skip destroy) cave=%p image=%08X..%08X\n",
        g, (unsigned)g_imgLo, (unsigned)g_imgHi);
}

unsigned __stdcall ReporterThread(void*)
{
    for (;;) {
        Sleep(1000);                                            // at most one line per second
        if (!g_installed && !g_relInstalled) continue;
        __try {
            static LONG s_relLogged = 0;
            const LONG r = g_hitsRel;
            if (r > s_relLogged) {
                s_relLogged = r;
                Log("[WalkGuard] release guard hit n=%ld: dead object %08X (vtable word %08X) - destroy skipped\n",
                    (long)r, (unsigned)g_relLastObj, (unsigned)g_relLastVt);
            }
            const LONG n = g_hitsF + g_hitsB + g_hitsD0 + g_hits210 + g_hits2E0;
            if (n <= g_lastLogged) continue;
            if (!g_firstHitTick) g_firstHitTick = GetTickCount64();
            g_lastLogged = n;
            Log("[WalkGuard] null L hit n=%ld (first at t=%llu) [1F0=%ld 1B0=%ld 1D0=%ld 210=%ld 2E0=%ld]\n",
                (long)n, (unsigned long long)g_firstHitTick, (long)g_hitsF, (long)g_hitsB,
                (long)g_hitsD0, (long)g_hits210, (long)g_hits2E0);
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

    if (GetPrivateProfileIntA("Coop", "ReleaseStaleGuard", 0, Paths::Ini()) != 0) InstallReleaseGuard(game);   // [Coop] ReleaseStaleGuard
    if (!Install()) { if (g_relInstalled) _beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, nullptr); return; }
    if (GetPrivateProfileIntA("Coop", "WalkNullGuardExt", 0, Paths::Ini()) != 0) InstallVecGuards();

    // _beginthreadex, not CreateThread: the reporter calls CRT functions (fputs/_vsnprintf_s) and
    // CreateThread does not initialise the CRT's per-thread state.
    _beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, nullptr);
}

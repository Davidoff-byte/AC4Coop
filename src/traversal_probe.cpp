// [Coop] TraversalProbe (default 0, read at load): every Human context transition of the local player and the co-op
// puppet, with the transition function that made it. Read-only (every hook runs the original unchanged). Purpose: decode
// the traversal moves (ground / air / wall-run / beam / climb ...) as (old context, new context, transition function) so
// they can be replicated as transitions, and find what lifts the hanging native replica onto the beam
// (docs/evidence/climb-moves-2026-11-15.md, rounds 2-3).
//
// Sites (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, ida_batch tr1/tr2; S1 evidence for the MP counterparts):
//   E 0x0052C330  Exit current context:  thiscall(Human*); layer = byte this+0x100; slot = this+8+48*layer; calls the
//                 current context's vt+0x38, stores its id at slot+0x10. 367 callers (the transition functions).
//                 Bytes 0F B6 81 00 01 00 00 (movzx eax, byte [ecx+100h]) - 7 stolen, no ret-relative code.
//   N 0x0052DFF0  Enter context(id):     thiscall(Human*, int id), retn 4; new = factory vt+0x20(slot+0x2C, id),
//                 slot+0x14 = new, new->vt+0x30. 84 callers. Bytes 55 8B EC 0F B6 81 00 01 00 00 - 10 stolen.
//   T 0x0052DF90  TransitToContext(id):  exit + enter in one (1 caller). Same 10 prologue bytes.
// `this` is the Human (AIActor): GetCurrentContextID 0x0052C2E0 is thiscall(Human*, track) on the same slots.
// Logged: [TR] lines with who (player / puppet), kind (E/N/T), layer, current context id on that layer (before the
// call), the new id (N/T), the return address into the transition function, and 5 backtrace frames.
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"
#include "Scimitar/AI/AIEntityHelper.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void Phase1_LogLine(const char* s);
void* Phase1_PuppetEntity();
int Phase1_PeerContextId();   // phase1.cpp: the peer's newest contextId

namespace {
DWORD g_tE = 0, g_tN = 0, g_tT = 0;
bool g_on = false, g_log = false;   // g_log: [Coop] TraversalProbe (the [TR] lines); g_on: any hook installed
volatile uintptr_t g_plH = 0, g_pupH = 0;
volatile ULONGLONG g_next = 0;
volatile LONG g_lines = 0, g_cnt[2][3] = {};

void Refresh()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_next) return;
    g_next = now + 250;
    __try {
        World* w = World::GetCurrentWorld();
        Entity* p = w ? w->GetPlayer() : nullptr;
        g_plH = p ? (uintptr_t)AIEntityHelper::GetHuman(p) : 0;
        Entity* q = (Entity*)Phase1_PuppetEntity();
        g_pupH = q ? (uintptr_t)AIEntityHelper::GetHuman(q) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int CurCtx(uintptr_t human, int layer)   // AIActor::GetCurrentContextID body (0x0052C2E0): slot ptr at this+4*(12*layer+7)
{
    const uintptr_t c = *(const uintptr_t*)(human + 4 * (12 * layer + 7));
    if (!c) return 2;
    typedef uintptr_t(__thiscall* GetInfo)(uintptr_t);
    const uintptr_t info = ((GetInfo)(*(uintptr_t**)c)[9])(c);   // vt+0x24
    return info ? *(const int*)(info + 4) : -1;
}

ULONGLONG NowMs()
{
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (ULONGLONG)((c.QuadPart / f.QuadPart) * 1000ull + (c.QuadPart % f.QuadPart) * 1000ull / f.QuadPart);
}
}  // namespace

// kind 0 Exit, 1 Enter, 2 Transit. args[0] = return address into the transition function, args[1] = id (N/T).
extern "C" void __cdecl TrNote(uintptr_t human, const uint32_t* args, int kind)
{
    __try {
        Refresh();
        const int who = human == g_plH ? 0 : human == g_pupH ? 1 : -1;
        if (who < 0) return;
        InterlockedIncrement(&g_cnt[who][kind]);
        if (InterlockedIncrement(&g_lines) > 6000) return;
        const int layer = *(const uint8_t*)(human + 0x100);
        const int cur = CurCtx(human, layer);
        void* fr[7] = {}; RtlCaptureStackBackTrace(0, 7, fr, nullptr);
        char line[240];
        static const char K[3] = { 'E', 'N', 'T' };
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[TR] t=%llu %s %c layer=%d cur=%d new=%d ret=%08X bt=%08X %08X %08X %08X %08X\n",
                    NowMs(), who ? "puppet" : "player", K[kind], layer, cur, kind ? (int)args[1] : -1, args[0],
                    (unsigned)(uintptr_t)fr[2], (unsigned)(uintptr_t)fr[3], (unsigned)(uintptr_t)fr[4], (unsigned)(uintptr_t)fr[5],
                    (unsigned)(uintptr_t)fr[6]);
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

namespace {
__declspec(naked) void DetE()
{
    __asm {
        pushad
        push 0
        lea  eax, [esp + 36]
        push eax
        push ecx
        call TrNote
        add  esp, 12
        popad
        jmp  dword ptr [g_tE]
    }
}
__declspec(naked) void DetN()
{
    __asm {
        pushad
        push 1
        lea  eax, [esp + 36]
        push eax
        push ecx
        call TrNote
        add  esp, 12
        popad
        jmp  dword ptr [g_tN]
    }
}
__declspec(naked) void DetT()
{
    __asm {
        pushad
        push 2
        lea  eax, [esp + 36]
        push eax
        push ecx
        call TrNote
        add  esp, 12
        popad
        jmp  dword ptr [g_tT]
    }
}

// C 0x01581EE0  climb transition-condition dispatcher (ida_batch tr4/tr5): thiscall(list, u16 id, int arg), retn 8,
//               switch on id 1..22 to one condition function each. Called by the climb context's evaluator sub_1583780
//               (`mov ecx, edi` at 0x015837AC) for each id in the current state's list; the first non-zero result is the
//               move: low 10 bits < 23 = climb sub-state, >= 23 = leave the climb for context (low10 - 23); bits 10..19
//               and bit 30 are passed on (sub_15759E0 / sub_1576B40). Human = *(*list + 4) (the evaluator's Exit arg).
//               Bytes 55 8B EC 0F B7 45 08 - 7 stolen. Logged only when the result is non-zero (a move fires).
DWORD g_tC = 0;
volatile LONG g_cond[2] = {};
typedef int(__thiscall* CondFn)(void*, uint32_t, int);
// [Coop] NativeReplicaClimbExitGate (default 0, read at load): the native replica leaves Climbing only the way its peer
// did. Run 4220 (asi 723b1157): the replica's Climbing -> NarrowObject came from condition 17 (sub_156FF80, the
// transfer-to-narrow-object test) with zero climbing input on the replica, while the peer kept hanging. The player fires the
// same condition in a free-run (run 4220: 8 -> 30 -> 9 at 191846083), so it is native behaviour, not a fault. Gate: for the
// puppet only, condition 17 is not evaluated (returns 0, as when it does not apply) while the peer's context is still
// Climbing (30); once the peer has left Climbing it runs as usual. Skipped BEFORE the call, so none of its side effects run.
bool g_exitGate = false;
volatile LONG g_gated = 0;
int __fastcall DetC(void* list, void* /*edx*/, uint32_t id, int arg)
{
    if (g_exitGate && (id & 0xFFFF) == 17) {
        __try {
            Refresh();
            const uintptr_t hu = *(const uintptr_t*)(*(const uintptr_t*)list + 4);
            if (hu && hu == g_pupH && Phase1_PeerContextId() == 30) {
                if (InterlockedIncrement(&g_gated) <= 20) {
                    char line[160];
                    _snprintf_s(line, sizeof(line), _TRUNCATE, "[TR] t=%llu puppet climb-cond 17 held: peer still climbing (gated %ld)\n", NowMs(), g_gated);
                    Phase1_LogLine(line);
                }
                return 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    const int r = ((CondFn)g_tC)(list, id, arg);
    if (!g_log) return r;   // installed for the gate only: no logging
    if (!r) return r;
    __try {
        Refresh();
        const uintptr_t human = *(const uintptr_t*)(*(const uintptr_t*)list + 4);
        const int who = human == g_plH ? 0 : human == g_pupH ? 1 : -1;
        if (who < 0) return r;
        InterlockedIncrement(&g_cond[who]);
        if (InterlockedIncrement(&g_lines) > 6000) return r;
        const int tgt = r & 0x3FF;
        char line[200];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[TR] t=%llu %s climb-cond id=%u -> %s %d (raw %08X: bits10-19=%u bit30=%d) ctx=%d\n",
                    NowMs(), who ? "puppet" : "player", id & 0xFFFF, tgt < 23 ? "substate" : "context", tgt < 23 ? tgt : tgt - 23,
                    (unsigned)r, ((unsigned)r >> 10) & 0x3FF, (r & 0x40000000) ? 1 : 0, CurCtx(human, *(const uint8_t*)(human + 0x100)));
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

bool Install(uintptr_t va, const BYTE* pro, int n, void* det, DWORD* slot, const char* name)
{
    BYTE* site = (BYTE*)(GameModuleBase + (va - 0x00400000));
    char line[128];
    if (memcmp(site, pro, n) != 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[TR] %s %08X prologue mismatch - not installed\n", name, (unsigned)va);
        Phase1_LogLine(line); return false;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, pro, n); t[n] = 0x68; *(DWORD*)(t + n + 1) = (DWORD)(uintptr_t)(site + n); t[n + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, n + 6);
    *slot = (DWORD)(uintptr_t)t;
    const bool ok = Hook::DetourFunc(site, (DWORD)(uintptr_t)det, (DWORD)n);
    if (!ok) *slot = 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[TR] %s %08X hooked=%d\n", name, (unsigned)va, ok ? 1 : 0);
    Phase1_LogLine(line);
    return ok;
}
}  // namespace

void TraversalProbeInit()
{
    g_log = GetPrivateProfileIntA("Coop", "TraversalProbe", 0, Paths::Ini()) != 0;
    g_exitGate = GetPrivateProfileIntA("Coop", "NativeReplicaClimbExitGate", 0, Paths::Ini()) != 0;
    if (!g_log && !g_exitGate) return;
    if (g_log) {
        static const BYTE pE[7] = { 0x0F, 0xB6, 0x81, 0x00, 0x01, 0x00, 0x00 };
        static const BYTE pN[10] = { 0x55, 0x8B, 0xEC, 0x0F, 0xB6, 0x81, 0x00, 0x01, 0x00, 0x00 };
        g_on |= Install(0x0052C330, pE, 7, (void*)&DetE, &g_tE, "Exit");
        g_on |= Install(0x0052DFF0, pN, 10, (void*)&DetN, &g_tN, "Enter");
        g_on |= Install(0x0052DF90, pN, 10, (void*)&DetT, &g_tT, "TransitToContext");
    }
    static const BYTE pC[7] = { 0x55, 0x8B, 0xEC, 0x0F, 0xB7, 0x45, 0x08 };
    const bool c = Install(0x01581EE0, pC, 7, (void*)&DetC, &g_tC, "ClimbCondition");
    g_on |= c;
    if (!c) g_exitGate = false;
    char line[96];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[TR] TraversalProbe=%d NativeReplicaClimbExitGate=%d\n", g_log ? 1 : 0, g_exitGate ? 1 : 0);
    Phase1_LogLine(line);
}

int TraversalProbeReport(char* out, int cap)
{
    if (!g_on) return 0;
    return _snprintf_s(out, cap, _TRUNCATE, "[TR] counts player E/N/T=%ld/%ld/%ld puppet E/N/T=%ld/%ld/%ld lines=%ld | exit gate held=%ld",
                       g_cnt[0][0], g_cnt[0][1], g_cnt[0][2], g_cnt[1][0], g_cnt[1][1], g_cnt[1][2], g_lines, g_gated);
}

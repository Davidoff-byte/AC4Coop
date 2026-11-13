// Event probe. Hook site (AC4BFSP.exe, md5 2058342866688f780c8b34526a65bc35, IDA batch 2026-11-13, o3):
//
//   0x0064A020  55        push ebp            <- 6 bytes stolen whole
//   0x0064A021  8B EC     mov  ebp, esp
//   0x0064A023  56        push esi
//   0x0064A024  8B F1     mov  esi, ecx
//   0x0064A026  ...                           <- resume
//
// int __thiscall sub_64A020(Event* this, Entity* receiver): calls this->vt[+0x24](receiver) and, when that returns
// non-zero, queues the event (sub_454460); retn 4. 577 call sites post through it.
// The perception stimulus event class has vtable 0x026E47C4 (24 bytes, allocated by sub_FA3260): +0x08 = source
// handle (entity+0xC8), +0x0C = receiver handle, +0x10 = stimulus id (sub_102DD90 writes all three; sight = 8,
// docs: guard-perception-of-puppet-2026-11-13.md S1). The user's report: guards aggro when the player attacks or
// fires a pistol. Which event that posts is NOT known; this probe finds it.
//
// Logged (first sight only, <= 400 lines): every (event vtable, call site) pair, with the time, and for stimulus
// events every (stimulus id, call site, source-is-local-player) triple. A run where the host shoots / swings near
// guards, with the times noted, names the events those actions post. Per 5 s window on [EvtHB]: stimulus counts by
// id for the local player as source.
#include "event_probe.h"
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

void Phase1_LogLine(const char* s);

namespace {

const uintptr_t VA_POST = 0x0064A020;
const uintptr_t STEAL_LEN = 6;
const BYTE kPrologue[STEAL_LEN] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1 };
const uintptr_t VT_STIMULUS = 0x026E47C4;

DWORD g_tramp = 0;
bool  g_installed = false;

volatile uintptr_t g_playerHandle = 0;
volatile ULONGLONG g_playerNextMs = 0;

struct Seen { uintptr_t vt, ret; uint32_t id; uint8_t pl; };
const int SEEN_MAX = 1024;
Seen    g_seen[SEEN_MAX];
int     g_nSeen = 0;
LONG    g_lines = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

const int ID_MAX = 256;
volatile LONG g_plStim[ID_MAX];       // stimulus id -> count with the local player as source (window)
volatile LONG g_stimAll = 0, g_stimPl = 0, g_posts = 0;
ULONGLONG g_occLast[ID_MAX];          // per id: when the occurrence log last wrote (GetTickCount64)
int       g_occN[ID_MAX];

ULONGLONG NowMs()
{
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (ULONGLONG)((c.QuadPart / f.QuadPart) * 1000ull + (c.QuadPart % f.QuadPart) * 1000ull / f.QuadPart);
}

uintptr_t PlayerHandle()
{
    const ULONGLONG now = GetTickCount64();
    if (now >= g_playerNextMs) {
        g_playerNextMs = now + 500;
        uintptr_t h = 0;
        __try {
            World* w = World::GetCurrentWorld();
            Entity* p = w ? w->GetPlayer() : nullptr;
            if (p) h = *(const uintptr_t*)((const char*)p + 0xC8);   // the handle sub_102DD90 stores at event+8
        } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
        g_playerHandle = h;
    }
    return g_playerHandle;
}

void FirstSight(uintptr_t vt, uintptr_t ret, uint32_t id, uint8_t pl, uintptr_t src, void* recv)
{
    AcquireSRWLockExclusive(&g_lock);
    bool fresh = true;
    for (int i = 0; i < g_nSeen; ++i)
        if (g_seen[i].vt == vt && g_seen[i].ret == ret && g_seen[i].id == id && g_seen[i].pl == pl) { fresh = false; break; }
    if (fresh && g_nSeen < SEEN_MAX) { g_seen[g_nSeen].vt = vt; g_seen[g_nSeen].ret = ret; g_seen[g_nSeen].id = id; g_seen[g_nSeen].pl = pl; ++g_nSeen; }
    const bool log = fresh && g_lines < 400;
    if (log) ++g_lines;
    ReleaseSRWLockExclusive(&g_lock);
    if (!log) return;
    char line[160];
    if (vt == VT_STIMULUS)
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Evt] t=%llu STIM id=%u src=%08X player=%d recv=%p from=%08X\n",
                    NowMs(), id, (unsigned)src, (int)pl, recv, (unsigned)ret);
    else
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[Evt] t=%llu event vt=%08X recv=%p from=%08X\n",
                    NowMs(), (unsigned)vt, recv, (unsigned)ret);
    Phase1_LogLine(line);
}

void __cdecl OnPost(void* ev, void* recv, uintptr_t ret)
{
    __try {
        InterlockedIncrement(&g_posts);
        if (!ev) return;
        const uintptr_t vt = *(const uintptr_t*)ev;
        if (vt != VT_STIMULUS) { FirstSight(vt, ret, 0, 0, 0, recv); return; }
        const uintptr_t src = *(const uintptr_t*)((const char*)ev + 0x08);
        const uint32_t id = *(const uint32_t*)((const char*)ev + 0x10);
        const uintptr_t ph = PlayerHandle();
        const uint8_t pl = (ph && src == ph) ? 1 : 0;
        InterlockedIncrement(&g_stimAll);
        if (pl) { InterlockedIncrement(&g_stimPl); if (id < ID_MAX) InterlockedIncrement(&g_plStim[id]); }
        // Occurrence log (v2): a player-source stimulus other than sight (8) is logged with all its fields when the
        // same id was last logged > 2 s ago (one broadcast reaches many receivers within the same frame), <= 12
        // lines per id. Separates repeated actions (3 shots 15 s apart = 3 lines of the shot's ids).
        if (pl && id != 8 && id < ID_MAX) {
            const ULONGLONG now = GetTickCount64();
            if (now - g_occLast[id] > 2000 && g_occN[id] < 12) {
                g_occLast[id] = now; ++g_occN[id];
                const uint32_t* d = (const uint32_t*)ev;
                char line[200];
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[EvtOcc] t=%llu id=%u n=%d src=%08X dst=%08X flags=%08X recv=%p from=%08X\n",
                            NowMs(), id, g_occN[id], d[2], d[3], d[5], recv, (unsigned)ret);
                Phase1_LogLine(line);
            }
        }
        FirstSight(vt, ret, id, pl, src, recv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Entry: ECX = event, [esp] = return address, [esp+4] = receiver. All registers preserved; the stolen prologue
// then runs in the trampoline and execution continues at 0x0064A026.
__declspec(naked) void Detour()
{
    __asm {
        pushad                      // original esp+0 (ret) is now at esp+32
        mov  eax, [esp + 32]
        push eax                    // arg 3: ret
        mov  eax, [esp + 40]        // receiver (original esp+4)
        push eax                    // arg 2
        push ecx                    // arg 1: event
        call OnPost
        add  esp, 12
        popad
        jmp  dword ptr [g_tramp]
    }
}

BYTE* BuildTrampoline(const BYTE* stolen, uintptr_t resume)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, stolen, STEAL_LEN);
    t[STEAL_LEN] = 0x68;                                       // push imm32; ret = absolute jump
    *reinterpret_cast<DWORD*>(t + STEAL_LEN + 1) = (DWORD)resume;
    t[STEAL_LEN + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, STEAL_LEN + 6);
    return t;
}

}  // namespace

void EventProbe::Init()
{
    if (!GetPrivateProfileIntA("Coop", "EventProbe", 0, Paths::Ini())) return;
    const uintptr_t site = GameModuleBase + (VA_POST - 0x00400000);
    if (memcmp((const void*)site, kPrologue, STEAL_LEN) != 0) {
        OutputDebugStringA("[EventProbe] prologue mismatch at 0x64A020 - not installed\n");
        return;
    }
    BYTE* t = BuildTrampoline(kPrologue, site + STEAL_LEN);
    if (!t) return;
    g_tramp = (DWORD)(uintptr_t)t;
    if (!Hook::DetourFunc((BYTE*)site, (DWORD)(uintptr_t)&Detour, (DWORD)STEAL_LEN)) return;
    g_installed = true;
    OutputDebugStringA("[EventProbe] installed at 0x64A020\n");
}

int EventProbe::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    int n = _snprintf_s(out, cap, _TRUNCATE, "[EvtHB] posts=%ld stim=%ld fromPlayer=%ld kinds=%d lines=%ld | player ids:",
                        InterlockedExchange(&g_posts, 0), InterlockedExchange(&g_stimAll, 0),
                        InterlockedExchange(&g_stimPl, 0), g_nSeen, g_lines);
    for (int i = 0; i < ID_MAX && n > 0 && n < cap - 16; ++i) {
        const LONG c = InterlockedExchange(&g_plStim[i], 0);
        if (c) n += _snprintf_s(out + n, cap - n, _TRUNCATE, " %d:%ld", i, c);
    }
    return n > 0 ? n : 0;
}

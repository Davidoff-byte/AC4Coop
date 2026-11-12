// AutoContinue - boot straight into the world (part B). See auto_continue.h.
//
// EVIDENCE (AC4BFSP.exe sha256 bc11ad79..., image base 0x00400000; IDA 9.3 + two MenuProbe captures, 2026-11-12)
//
// Press start.  TitleMenuPressStartComponent (vtable 0x026F4148) slot 18 = sub_17B4840, __thiscall(this, a2), retn 4:
//     017B4843  mov eax,[esi+8]            ; component state
//     017B4846  cmp eax,2 / cmp eax,3      ; only in state 2 or 3 ...
//     017B4865  mov eax,[edx+40h] ; push 45h ; call eax   ; ... and when the input object says action 0x45 fired:
//     017B4871  mov edi,[esi+28h]          ; entity
//     017B487A  call sub_F91B20            ; __thiscall(entity) -> al (type check)
//     017B4885  call sub_14ACD10           ; __thiscall(entity): if [entity+0D8h]==2 -> sub_1345390(3), vfunc +70h
//   Live (probe v2, pid 34548): entity+0xD8 went 0 -> 1 -> 2, stayed 2 while the component was in state 3, then 5.
//   What this unit does: in a detour of sub_17B4840, when state is 2/3, the entity passes sub_F91B20 and
//   [entity+0xD8]==2, it calls sub_14ACD10(entity) - the same two calls with the input test replaced.
//
// Pick the save.  TitleMenuSavegame (vtable 0x027004C8) registers UI callbacks in slot 30 (sub_18E4010):
//     sub_18E2390  __thiscall(this, slot), retn 4   "slot highlighted" (shows the save's info)
//     sub_18DAC60  __thiscall(this, slot), retn 4   "slot chosen"
//   Live, both captures: choosing the save = sub_18DAC60(this, 0) -> sub_13237E0(0) -> sub_18D3470(this) ->
//   sub_12DECD0("SAVE_GAME", 0, "CONTINUE") -> sub_18609A0(0), all at one tick, called through the functor thunk
//   0x0162C0E1 on a worker thread. The UI itself calls sub_18E2390(this, 0) through the same thunk ~1.2 s after the
//   menu opens, before any input (capture 2: 5 s without input followed). Every update/callback ran on a different
//   worker thread per frame (10+ thread ids), so there is no "main thread" to marshal to.
//   What this unit does: in a detour of sub_18E2390, AFTER the original has run, it calls sub_18DAC60(this, slot)
//   once - the same UI-callback context a real click uses, in the natural order (highlight, then choose).
//
// Never a new game.  sub_18DAC60 takes its NEW_GAME branch when the slot record has bytes +64, +65, +66 all zero
//   ("if (!v8 && !*(v7+66) && !*(v7+64)) { ... "SAVE_GAME", slot, "NEW_GAME" }"). The record of slot i is read with
//   the engine's own handle idiom (sub_132A6D0): h = (*(*(0x04E38128)+4484))[i]; obj = ((int)h[2] >> 31) & h[0].
//   Live: slot 0 = bytes 00/01/00 (has a save), slots 1 and 2 = 00/00/00 (empty); obj+36 = the slot index.
//   This unit selects a slot only when its record is readable, its index matches and those bytes are not all zero.
//
// UNVERIFIED: that selecting on the first highlight callback is never too early on other machines; what the DLC
//   resource-pack branch of sub_18DAC60 shows (it can open a prompt instead of continuing - then the player answers it).
#include "auto_continue.h"
#include "Hook.h"
#include "build_gate.h"
#include "pattern_scan.h"
#include "paths.h"

#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <cstdint>
#include <cstdarg>
#include <cstring>

namespace {

const uintptr_t VA_IMAGE_BASE   = 0x00400000;
const uintptr_t VA_START_UPDATE = 0x017B4840;   // sub_17B4840
const uintptr_t VA_SLOT_HOVER   = 0x018E2390;   // sub_18E2390
const char* PAT_START_UPDATE = "56 8B F1 8B 46 08 83 F8 02 74 05 83 F8 03 75 3B";   // unique in .text; steal 6
const char* PAT_SLOT_HOVER   = "55 8B EC 83 EC 28 A1 24 E5 AB 02 8A 80 8A 00 00";   // unique in .text; steal 6
const size_t STEAL = 6;   // both: complete instructions, no relative operand (capstone 5.0.7)

typedef int  (__thiscall* StartUpdate_t)(void* self, int a2);
typedef char (__thiscall* SlotHover_t)(void* self, uint32_t slot);
StartUpdate_t g_origStartUpdate = nullptr;
SlotHover_t   g_origSlotHover = nullptr;

bool          g_enabled = false;
int           g_wantSlot = -1;
volatile LONG g_startCalls = 0;      // sub_14ACD10 issued this many times (max 3)
volatile LONG g_lastStartTick = 0;
volatile LONG g_continueDone = 0;    // the slot decision was made (once per process)
volatile LONG g_logLock = 0;
FILE*         g_log = nullptr;

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
    while (InterlockedExchange(&g_logLock, 1)) Sleep(0);
    if (!g_log) g_log = _fsopen(Paths::LogPath("auto_continue_log.txt"), "a", _SH_DENYWR);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    InterlockedExchange(&g_logLock, 0);
}

bool Rd32(uint32_t a, uint32_t* out) { __try { if (a < 0x10000) return false; *out = *(volatile uint32_t*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool Rd8(uint32_t a, uint32_t* out) { __try { if (a < 0x10000) return false; *out = *(volatile uint8_t*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

// Slot record i: 1 = has a save, 0 = empty, -1 = not readable / index mismatch.
int SlotHasSave(int i) {
    uint32_t mgr = 0, arr = 0, h = 0, p = 0, f = 0, idx = 0, b64 = 0, b65 = 0, b66 = 0;
    if (!Rd32(0x04E38128u, &mgr) || !Rd32(mgr + 4484, &arr) || !Rd32(arr + 4 * (uint32_t)i, &h) || !Rd32(h, &p) || !Rd32(h + 8, &f)) return -1;
    const uint32_t obj = (uint32_t)(((int32_t)f >> 31) & (int32_t)p);
    if (!obj || !Rd32(obj + 36, &idx) || idx != (uint32_t)i) return -1;
    if (!Rd8(obj + 64, &b64) || !Rd8(obj + 65, &b65) || !Rd8(obj + 66, &b66)) return -1;
    return (b64 | b65 | b66) ? 1 : 0;
}

// ---- press start --------------------------------------------------------------------------------------------
void TryPressStart(void* self) {
    if (InterlockedCompareExchange(&g_startCalls, 0, 0) >= 3) return;
    uint32_t state = 0, ent = 0, d8 = 0;
    if (!Rd32((uint32_t)(uintptr_t)self + 8, &state) || (state != 2 && state != 3)) return;
    if (!Rd32((uint32_t)(uintptr_t)self + 0x28, &ent) || !ent || !Rd32(ent + 0xD8, &d8) || d8 != 2) return;
    const LONG now = (LONG)GetTickCount();
    const LONG last = InterlockedCompareExchange(&g_lastStartTick, 0, 0);
    if (last && now - last < 700) return;                       // one attempt, then give the state time to move
    if (InterlockedCompareExchange(&g_lastStartTick, now, last) != last) return;   // another worker took this tick
    if (!Hook::CallClassMethod<unsigned char, void*>(0x00F91B20, (void*)(uintptr_t)ent)) return;   // sub_F91B20
    const LONG n = InterlockedIncrement(&g_startCalls);
    Log("[AC] t=%lu tid=%lu press-start: component=%p state=%u entity=%08X +D8=2 -> sub_14ACD10 (attempt %ld)\n",
        GetTickCount(), GetCurrentThreadId(), self, state, ent, n);
    Hook::CallClassMethod<int, void*>(0x014ACD10, (void*)(uintptr_t)ent);                          // sub_14ACD10
}

int __fastcall MyStartUpdate(void* self, void* /*edx*/, int a2) {   // __thiscall(this, a2), retn 4
    TryPressStart(self);
    return g_origStartUpdate(self, a2);
}

// ---- pick the save --------------------------------------------------------------------------------------------
void TryContinue(void* self, uint32_t hovered) {
    uint32_t g = 0, g138 = 0;
    if (!Rd32(0x02ABE524u, &g) || !Rd8(g + 138, &g138) || !g138) return;    // sub_18DAC60 would do nothing yet
    if (InterlockedCompareExchange(&g_continueDone, 1, 0) != 0) return;     // once per process
    int slot = -1;
    const int s[3] = { SlotHasSave(0), SlotHasSave(1), SlotHasSave(2) };
    if (g_wantSlot >= 0 && g_wantSlot <= 2) { if (s[g_wantSlot] == 1) slot = g_wantSlot; }
    else if (hovered <= 2 && s[hovered] == 1) slot = (int)hovered;
    else for (int i = 0; i < 3 && slot < 0; ++i) if (s[i] == 1) slot = i;
    if (slot < 0) {
        Log("[AC] t=%lu continue: NOT selecting - no slot with a save (records %d %d %d, wanted %d, highlighted %u); staying in the menu\n",
            GetTickCount(), s[0], s[1], s[2], g_wantSlot, hovered);
        return;
    }
    Log("[AC] t=%lu tid=%lu continue: menu=%p highlighted=%u records=%d %d %d -> sub_18DAC60(menu, %d)\n",
        GetTickCount(), GetCurrentThreadId(), self, hovered, s[0], s[1], s[2], slot);
    Hook::CallClassMethod<int, void*, uint32_t>(0x018DAC60, self, (uint32_t)slot);                 // sub_18DAC60
    Log("[AC] t=%lu continue: sub_18DAC60 returned\n", GetTickCount());
}

char __fastcall MySlotHover(void* self, void* /*edx*/, uint32_t slot) {   // __thiscall(this, slot), retn 4
    const char r = g_origSlotHover(self, slot);
    TryContinue(self, slot);
    return r;
}

// ---- install ----------------------------------------------------------------------------------------------------
int HexNibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
size_t ParsePattern(const char* s, BYTE* out, size_t cap) {
    size_t n = 0;
    while (*s && n < cap) { while (*s == ' ') ++s; if (!*s) break; const int hi = HexNibble(*s++), lo = *s ? HexNibble(*s++) : -1; if (hi < 0 || lo < 0) return 0; out[n++] = (BYTE)((hi << 4) | lo); }
    return n;
}
bool Readable(const void* p, size_t n) {
    __try { volatile char a = ((const volatile char*)p)[0], b = ((const volatile char*)p)[n - 1]; (void)a; (void)b; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Detours `va` to `hook`; returns a trampoline (stolen bytes + jmp back) that calls the original, or nullptr.
// *origOut is set BEFORE the site is patched, so a hook that fires at once already has its original.
bool Detour(HMODULE game, const char* name, uintptr_t docVa, const char* pattern, void* hook, void** origOut) {
    BYTE want[32]; const size_t n = ParsePattern(pattern, want, sizeof(want));
    int hits = 0;
    uintptr_t va = PatternScan::Find(game, pattern, &hits);
    if (hits != 1) {
        if (hits == 0 && BuildGate::Trusted()) va = (uintptr_t)game + (docVa - VA_IMAGE_BASE);
        else { Log("[AC] %s: pattern matches=%d -> not installed\n", name, hits); return false; }
    }
    if (n < STEAL || !Readable((const void*)va, n) || memcmp((const void*)va, want, n) != 0) { Log("[AC] %s @ %08X: bytes differ -> not installed\n", name, (unsigned)va); return false; }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, (const void*)va, STEAL);
    *(DWORD*)(t + 24) = (DWORD)(va + STEAL);                       // data slot: resume address
    t[STEAL] = 0xFF; t[STEAL + 1] = 0x25; *(DWORD*)(t + STEAL + 2) = (DWORD)(uintptr_t)(t + 24);   // jmp [slot]
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    *origOut = t;
    if (!Hook::DetourFunc((BYTE*)va, (DWORD)(uintptr_t)hook, (DWORD)STEAL)) { *origOut = nullptr; Log("[AC] %s: DetourFunc failed\n", name); return false; }
    Log("[AC] %s detoured at %08X\n", name, (unsigned)va);
    return true;
}

}  // namespace

namespace AutoContinue {

bool Enabled() { return g_enabled; }

void Init() {
    if (GetPrivateProfileIntA("Coop", "AutoContinue", 0, Paths::Ini()) == 0) return;
    if (!BuildGate::Trusted()) return;
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) return;
    g_wantSlot = (int)GetPrivateProfileIntA("Coop", "AutoContinueSlot", -1, Paths::Ini());
    Log("[AC] pid=%lu AutoContinue=1 slot=%d\n", GetCurrentProcessId(), g_wantSlot);
    const bool a = Detour(game, "press-start update 0x17B4840", VA_START_UPDATE, PAT_START_UPDATE, (void*)&MyStartUpdate, (void**)&g_origStartUpdate);
    const bool b = Detour(game, "slot highlight 0x18E2390", VA_SLOT_HOVER, PAT_SLOT_HOVER, (void*)&MySlotHover, (void**)&g_origSlotHover);
    g_enabled = a || b;
}

}  // namespace AutoContinue

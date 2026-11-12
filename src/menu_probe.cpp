// Menu probe ([Coop] MenuProbe=1, default 0) - boot straight into the world, part B step 5.
// OBSERVATION ONLY. Logs every call to five title-menu / save functions found in IDA (2026-11-12) while the player
// picks Continue once, so the automatic Continue can be built from what the game actually does, not from a guess.
//
//   site 0  0x018DAC60  __thiscall(this, wchar_t* slot)     TitleMenuSavegame callback (vtable 0x027004C8 slot 30
//                       registers it): picks a save slot; empty slot -> "NEW_GAME" branch, else ... sub_18D3470
//   site 1  0x018D3470  __thiscall(this)                    calls sub_12DECD0("SAVE_GAME", *(this+232), "CONTINUE")
//   site 2  0x012DECD0  __thiscall(this, name, a3, a4)      named-command dispatcher (lock, sub_9CBB00(name) lookup,
//                       handler call): telemetry or the real load? - UNVERIFIED, this probe decides
//   site 3  0x013237E0  __stdcall(slotIndex)                swaps the current-slot pointer at [0x2ABEAE4]+324
//   site 4  0x018609A0  __cdecl(a1)                         sub_14ACA00(sub_18496D0(), a1): menu state transition
//
// Same machinery as combat_observe.cpp (whole-instruction prologue steal into a private stub that logs, then
// re-executes the stolen bytes and jumps back). Stolen ranges decoded with capstone 5.0.7: complete instructions,
// no relative branch (sites 3 and 4 steal `mov eax, [abs32]`, position-independent). Each 16-byte pattern is
// unique in .text of the pinned exe. String arguments are read under SEH; nothing is written to engine memory.
#include "menu_probe.h"
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
#include <cwchar>

namespace {

const uintptr_t VA_IMAGE_BASE = 0x00400000;
const size_t    SITE_COUNT    = 5;
const size_t    PATTERN_MAX   = 32;
const LONG      MAX_PER_SITE  = 200;
const size_t    STUB_SIZE     = 64;
const size_t    STUB_SLOT_OFF = 48;

struct SiteDesc { const char* name; uintptr_t va; const char* pattern; size_t steal; size_t verify; };
const SiteDesc kSites[SITE_COUNT] = {
    { "slot-select 0x18DAC60", 0x018DAC60u, "55 8B EC 83 EC 30 53 56 33 DB 53 8B F1 8B 0D E4", 6, 16 },
    { "continue    0x18D3470", 0x018D3470u, "56 57 8B F9 8B 77 28 85 F6 74 0B 8B CE E8 9E E6", 7, 16 },
    { "named-cmd   0x12DECD0", 0x012DECD0u, "55 8B EC 53 56 8B F1 8D 5E 1C 57 8B CB E8 0E 9B", 5, 16 },
    { "set-slot    0x13237E0", 0x013237E0u, "55 8B EC A1 28 81 E3 04 8B 90 84 11 00 00 8B 45", 8, 16 },
    { "transition  0x18609A0", 0x018609A0u, "55 8B EC A1 74 B1 DF 04 85 C0 74 1E 80 B8 A8 00", 8, 16 },
};

FILE*         g_log = nullptr;
bool          g_installed = false;
uintptr_t     g_va[SITE_COUNT];
BYTE          g_orig[SITE_COUNT][PATTERN_MAX];
volatile LONG g_hits[SITE_COUNT] = {};
volatile LONG g_inLog = 0;

struct SavedRegs {   // PUSHAD memory order, then EFLAGS, return address and the caller's stack arguments
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;   // +0..+28  (ECX = this for __thiscall)
    uint32_t eflags;                                   // +32
    uint32_t ret;                                      // +36
    uint32_t a1, a2, a3;                               // +40 +44 +48: first three stack arguments
};

void Log(const char* fmt, ...) {
    if (InterlockedExchange(&g_inLog, 1)) return;
    char buf[640];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    InterlockedExchange(&g_inLog, 0);
}

bool Readable(const void* p, size_t n) {
    if (!p || !n) return false;
    __try { volatile char a = ((const volatile char*)p)[0]; volatile char b = ((const volatile char*)p)[n - 1]; (void)a; (void)b; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A pointer shown as text if it looks like a short ASCII (or UTF-16 when wide) string, else "".
void Text(uint32_t p, bool wide, char* out, size_t n) {
    out[0] = 0;
    if (p < 0x10000) return;
    __try {
        size_t i = 0;
        for (; i + 1 < n && i < 64; ++i) {
            const unsigned c = wide ? ((const volatile wchar_t*)p)[i] : ((const volatile unsigned char*)p)[i];
            if (!c) break;
            if (c < 0x20 || c > 0x7E) { out[0] = 0; return; }
            out[i] = (char)c;
        }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

int HexNibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
size_t ParsePattern(const char* s, BYTE* out, size_t cap) {
    size_t n = 0;
    while (s && *s && n < cap) { while (*s == ' ') ++s; if (!*s) break; const int hi = HexNibble(*s++); const int lo = *s ? HexNibble(*s++) : -1;
        if (hi < 0 || lo < 0) return 0; out[n++] = (BYTE)((hi << 4) | lo); }
    return n;
}

}  // namespace

extern "C" void __cdecl MenuProbe_LogHit(int site, const void* saved) {
    if (site < 0 || site >= (int)SITE_COUNT || !saved) return;
    if (InterlockedIncrement(&g_hits[site]) > MAX_PER_SITE) return;
    const SavedRegs* r = (const SavedRegs*)saved;
    const DWORD t = GetTickCount();
    char s1[72], s3[72];
    switch (site) {
        case 0: Text(r->a1, true, s1, sizeof(s1));
            Log("[MP] t=%lu slot-select this=%08X slot=%08X \"%s\" ret=%08X\n", t, r->ecx, r->a1, s1, r->ret); break;
        case 1: Log("[MP] t=%lu continue    this=%08X ret=%08X\n", t, r->ecx, r->ret); break;
        case 2: Text(r->a1, false, s1, sizeof(s1)); Text(r->a3, false, s3, sizeof(s3));
            Log("[MP] t=%lu named-cmd   this=%08X name=%08X \"%s\" a3=%08X a4=%08X \"%s\" ret=%08X\n", t, r->ecx, r->a1, s1, r->a2, r->a3, s3, r->ret); break;
        case 3: Log("[MP] t=%lu set-slot    index=%u ret=%08X\n", t, r->a1, r->ret); break;
        case 4: Log("[MP] t=%lu transition  a1=%08X ret=%08X\n", t, r->a1, r->ret); break;
    }
}

namespace {

BYTE* BuildStub(int site, const BYTE* stolen, size_t steal, uintptr_t resume) {   // as combat_observe.cpp
    BYTE* s = (BYTE*)VirtualAlloc(nullptr, STUB_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return nullptr;
    size_t o = 0;
    s[o++] = 0x9C; s[o++] = 0x60; s[o++] = 0x54; s[o++] = 0x6A; s[o++] = (BYTE)site;   // pushfd; pushad; push esp; push site
    s[o++] = 0xE8; const DWORD from = (DWORD)(uintptr_t)(s + o + 4);
    *(DWORD*)(s + o) = (DWORD)(uintptr_t)&MenuProbe_LogHit - from; o += 4;               // call MenuProbe_LogHit
    s[o++] = 0x83; s[o++] = 0xC4; s[o++] = 0x08; s[o++] = 0x61; s[o++] = 0x9D;           // add esp,8; popad; popfd
    memcpy(s + o, stolen, steal); o += steal;                                             // stolen bytes
    *(DWORD*)(s + STUB_SLOT_OFF) = (DWORD)resume;
    s[o++] = 0xFF; s[o++] = 0x25; *(DWORD*)(s + o) = (DWORD)(uintptr_t)(s + STUB_SLOT_OFF); o += 4;   // jmp [slot]
    FlushInstructionCache(GetCurrentProcess(), s, o);
    return s;
}

bool InstallOne(size_t i, HMODULE game) {
    BYTE want[PATTERN_MAX];
    const size_t n = ParsePattern(kSites[i].pattern, want, sizeof(want));
    if (n != kSites[i].verify) { Log("[MP] %s: bad pattern\n", kSites[i].name); return false; }
    int hits = 0;
    uintptr_t va = PatternScan::Find(game, kSites[i].pattern, &hits);
    if (hits != 1) {
        if (hits == 0 && BuildGate::Trusted()) va = (uintptr_t)game + (kSites[i].va - VA_IMAGE_BASE);
        else { Log("[MP] %s: pattern matches=%d -> skipped\n", kSites[i].name, hits); return false; }
    }
    if (!Readable((const void*)va, n) || memcmp((const void*)va, want, n) != 0) { Log("[MP] %s @ %08X: pattern mismatch -> skipped\n", kSites[i].name, (unsigned)va); return false; }
    memcpy(g_orig[i], (const void*)va, kSites[i].steal);
    BYTE* stub = BuildStub((int)i, g_orig[i], kSites[i].steal, va + kSites[i].steal);
    if (!stub || !Hook::DetourFunc((BYTE*)va, (DWORD)(uintptr_t)stub, (DWORD)kSites[i].steal)) { Log("[MP] %s: install failed\n", kSites[i].name); return false; }
    g_va[i] = va;
    Log("[MP] %s armed va=%08X steal=%u\n", kSites[i].name, (unsigned)va, (unsigned)kSites[i].steal);
    return true;
}

}  // namespace

void MenuProbe::Init() {
    if (g_installed) return;
    if (GetPrivateProfileIntA("Coop", "MenuProbe", 0, Paths::Ini()) == 0) return;
    if (!BuildGate::Trusted()) return;
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) return;
    g_installed = true;
    char leaf[64]; _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "AC4Coop_menu_probe_%u.log", (unsigned)GetCurrentProcessId());
    g_log = _fsopen(Paths::LogPath(leaf), "a", _SH_DENYWR);
    Log("[MP] menu probe starting (observe only)\n");
    int armed = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) armed += InstallOne(i, game) ? 1 : 0;
    Log("[MP] armed %d/%d sites\n", armed, (int)SITE_COUNT);
}

// Detection-reaction probe. AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA batch 2026-11-13 (o17) and the S3
// evidence (guard-perception-of-puppet-2026-11-13.md section 8):
//   sub_1076110 = CRLDetectParams "CanReact", bool __thiscall(params, Entity* guard, Stimulus* stim), retn 8.
//     0x01076110  53 / 8B DC / 83 EC 08     push ebx / mov ebx,esp / sub esp,8   <- 6 bytes stolen (entry count)
//     R1 filters (sub_14C3C90), R2 range, R3 faction (call 0x0107623E), R4 AS = *(*(guard+0x5C)+0x8E0) != 0, then
//     0x0107625F  56                 push esi            (esi = stim + 8, the target handle slot)
//     0x01076260  E8 2B 6C FA FF     call sub_101CE90    (thiscall ecx = AS, 1 arg, ret 4)  <- R5, call-site redirect
//     0x01076265  ...                its al is CanReact's result
// Run 22340 (build a436718, SightProbe): the puppet is visible to guards in cone and range, same faction as the
// player and hostile(G,P) as often as hostile(G,player), as20 = 1, yet the detection start (0x131EFD5) never runs.
// This probe counts, per target (puppet / local player): CanReact entries, arrivals at R5 (= R1..R4 passed), and
// R5 results - so a failing R1/R2 (entered, never at R5), R5 (at R5, false) or R0 (never entered) separates.
// Seam (S3 8.4), [Coop] AlertQueryAsPlayer: when the target handle is the puppet's, R5 is asked about a slot that
// holds the local player's handle instead - the engine's own alert-state rule, with the player's identity lent to
// the puppet for this one query.
#include "react_probe.h"
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void* Phase1_PuppetEntity();

namespace {

const uintptr_t VA_CANREACT = 0x01076110;
const uintptr_t CR_STEAL = 6;
const BYTE kCrPrologue[CR_STEAL] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 };
const uintptr_t VA_R5CALL = 0x01076260;
const BYTE kR5Call[5] = { 0xE8, 0x2B, 0x6C, 0xFA, 0xFF };
const uintptr_t VA_R5 = 0x0101CE90;

DWORD g_crTramp = 0, g_r5Target = 0;
bool  g_installed = false;

volatile LONG g_enterPup = 0, g_enterPl = 0, g_atR5Pup = 0, g_atR5Pl = 0, g_r5TruePup = 0, g_r5TruePl = 0, g_subst = 0;
volatile LONG g_idPup[8];             // stimulus ids seen at entry for the puppet (first 8 distinct)
volatile LONG g_idPupN[8];
__declspec(thread) int t_who = 0;     // 1 = puppet, 2 = player (set at R5, read after it)

uintptr_t g_plHandle = 0; ULONGLONG g_plNext = 0;
uintptr_t g_plSlot = 0;               // a slot holding the player's handle (R5 reads the handle through a pointer)
int g_substOn = 0; ULONGLONG g_substNext = 0;

uintptr_t PlayerHandle()
{
    const ULONGLONG now = GetTickCount64();
    if (now >= g_plNext) {
        g_plNext = now + 500;
        uintptr_t h = 0;
        __try {
            World* w = World::GetCurrentWorld();
            Entity* p = w ? w->GetPlayer() : nullptr;
            if (p) h = *(const uintptr_t*)((const char*)p + 0xC8);
        } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
        g_plHandle = h;
    }
    return g_plHandle;
}

uintptr_t PuppetHandle()
{
    void* p = Phase1_PuppetEntity();
    return p ? *(const uintptr_t*)((const char*)p + 0xC8) : 0;
}

void NoteId(uint32_t id)
{
    for (int i = 0; i < 8; ++i) {
        if (g_idPupN[i] && (uint32_t)g_idPup[i] == id) { InterlockedIncrement(&g_idPupN[i]); return; }
        if (!g_idPupN[i]) { g_idPup[i] = (LONG)id; InterlockedIncrement(&g_idPupN[i]); return; }
    }
}

void __cdecl OnEnter(const char* stim)
{
    __try {
        if (!stim) return;
        const uintptr_t t = *(const uintptr_t*)(stim + 8);
        if (!t) return;
        const uintptr_t pu = PuppetHandle();
        if (pu && t == pu) { InterlockedIncrement(&g_enterPup); NoteId(*(const uint32_t*)(stim + 0x10)); }
        else if (t == PlayerHandle()) InterlockedIncrement(&g_enterPl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void CrDetour()
{
    __asm {
        pushad
        mov  eax, [esp + 40]        // stim (original esp+8)
        push eax
        call OnEnter
        add  esp, 4
        popad
        jmp  dword ptr [g_crTramp]
    }
}

// Before R5: count, and maybe substitute the handle slot. Returns the slot pointer to pass.
const uintptr_t* __cdecl R5Pre(const uintptr_t* slot)
{
    t_who = 0;
    __try {
        const uintptr_t t = slot ? *slot : 0;
        const uintptr_t pu = PuppetHandle();
        if (pu && t == pu) {
            t_who = 1; InterlockedIncrement(&g_atR5Pup);
            const ULONGLONG now = GetTickCount64();
            if (now >= g_substNext) { g_substNext = now + 2000; g_substOn = GetPrivateProfileIntA("Coop", "AlertQueryAsPlayer", 0, Paths::Ini()) != 0; }
            const uintptr_t ph = PlayerHandle();
            if (g_substOn && ph) { g_plSlot = ph; InterlockedIncrement(&g_subst); return &g_plSlot; }
        } else if (t && t == PlayerHandle()) { t_who = 2; InterlockedIncrement(&g_atR5Pl); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return slot;
}

void __cdecl R5Post(int al)
{
    if (!(al & 0xFF)) return;
    if (t_who == 1) InterlockedIncrement(&g_r5TruePup);
    else if (t_who == 2) InterlockedIncrement(&g_r5TruePl);
}

// Replaces `call sub_101CE90` at 0x01076260: ecx = AS, [esp] = return to 0x01076265, [esp+4] = slot pointer.
__declspec(naked) void R5Stub()
{
    __asm {
        push ecx                    // AS
        mov  eax, [esp + 8]         // slot (original esp+4)
        push eax
        call R5Pre
        add  esp, 4
        pop  ecx
        push eax                    // the slot to query
        call dword ptr [g_r5Target] // thiscall, ret 4 pops it
        push eax
        push eax
        call R5Post
        add  esp, 4
        pop  eax
        ret  4                      // pops the caller's argument, as sub_101CE90 would
    }
}

}  // namespace

void ReactProbe::Init()
{
    if (!GetPrivateProfileIntA("Coop", "ReactProbe", 0, Paths::Ini())) return;
    const uintptr_t cr = GameModuleBase + (VA_CANREACT - 0x00400000);
    const uintptr_t r5 = GameModuleBase + (VA_R5CALL - 0x00400000);
    if (memcmp((const void*)cr, kCrPrologue, CR_STEAL) != 0 || memcmp((const void*)r5, kR5Call, 5) != 0) {
        OutputDebugStringA("[React] byte mismatch at 0x1076110 / 0x1076260 - not installed\n");
        return;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kCrPrologue, CR_STEAL);
    t[CR_STEAL] = 0x68;
    *reinterpret_cast<DWORD*>(t + CR_STEAL + 1) = (DWORD)(cr + CR_STEAL);
    t[CR_STEAL + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, CR_STEAL + 6);
    g_crTramp = (DWORD)(uintptr_t)t;
    g_r5Target = (DWORD)(GameModuleBase + (VA_R5 - 0x00400000));
    if (!Hook::DetourFunc((BYTE*)cr, (DWORD)(uintptr_t)&CrDetour, (DWORD)CR_STEAL)) return;
    DWORD old = 0;
    if (VirtualProtect((void*)r5, 5, PAGE_EXECUTE_READWRITE, &old)) {
        *(BYTE*)r5 = 0xE8;
        *(int32_t*)(r5 + 1) = (int32_t)((uintptr_t)&R5Stub - (r5 + 5));
        VirtualProtect((void*)r5, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)r5, 5);
    }
    g_installed = true;
}

int ReactProbe::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    int n = _snprintf_s(out, cap, _TRUNCATE,
        "[React] CanReact enter pup=%ld pl=%ld | atR5 pup=%ld pl=%ld | R5 true pup=%ld pl=%ld | subst=%ld | pup ids:",
        InterlockedExchange(&g_enterPup, 0), InterlockedExchange(&g_enterPl, 0),
        InterlockedExchange(&g_atR5Pup, 0), InterlockedExchange(&g_atR5Pl, 0),
        InterlockedExchange(&g_r5TruePup, 0), InterlockedExchange(&g_r5TruePl, 0), InterlockedExchange(&g_subst, 0));
    for (int i = 0; i < 8 && n > 0 && n < cap - 16; ++i) {
        const LONG c = InterlockedExchange(&g_idPupN[i], 0);
        if (c) n += _snprintf_s(out + n, cap - n, _TRUNCATE, " %ld:%ld", g_idPup[i], c);
    }
    return n > 0 ? n : 0;
}

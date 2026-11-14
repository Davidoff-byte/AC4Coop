// Puppet stimulus test (host). Every engine fact below is from IDA on AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35
// (docs/evidence/player-stimuli-2026-11-13.md):
//   sub_64A020  int __thiscall Post(Event* this, Entity* receiver), retn 4 - the generic event post.
//   sub_FA3260  int __cdecl ()  -> new 24-byte stimulus event, vtable 0x026E47C4 (ctor sub_16AAD10).
//   sub_FA3300  int __cdecl ()  -> new 24-byte template event, vtable 0x026E4798 (ctor sub_16AAC80).
//     Both ctors: +0x08 = +0x0C = &dword_4DD5F8C (the null handle), u16 +0x14 &= 0xFC00, +0x10 = 0.
//   sub_A094D0  __thiscall (this = byte_4DD5FA0, handle) - releases a handle whose count reached 0.
//   Handle fields are set the way sub_184EEA0 / sub_102DD90 do it: count +1 (handle+4), exchange into the field,
//   then the old handle's count -1 and release when it is 0 and (old[2] & 0xFFFFFF) == 0.
//   An entity's handle is *(entity + 0xC8).
// Mode 1 copies sub_184EEA0: templates 48 then 49 posted to the emitter's OWN entity, +8 = +0xC = its handle (the
//   player's first shot in run 21692 had src == dst). The engine's listener (sub_13442D0 -> sub_1340E60) then
//   broadcasts it - IF the puppet entity carries that listener (UNVERIFIED; EventProbe shows it: from=013410BA with
//   src = the puppet's handle).
// Mode 2 copies sub_1340E60's loop: mgr = *(0x4DE6D4C) (sub_10A9F80), list = *(mgr+464) with u16 count at mgr+470,
//   entity = *(*(elem) + 112); skip kind 1 ((*(e+156) & 7) == 1) and the source; radius *(entry+40), vertical
//   *(entry+52) when the receiver is above, *(entry+56) below, entry = *(*(mgr+500) + 4*id) or mgr+60/72/76 when 0.
//   The engine also skips receivers for which sub_133FA50(e+200) is true (not read; not applied here) and spreads
//   later receivers over frames with sub_64A050 (here: all posted at once).
#include "puppet_stim.h"
#include "paths.h"
#include "Hook.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

void Phase1_LogLine(const char* s);
bool Phase1_IsEntityHost();
void* Phase1_PuppetEntity();

namespace {

typedef void* (__cdecl* NewEvent_t)();
typedef int   (__thiscall* Post_t)(void* ev, void* receiver);
typedef int   (__thiscall* Release_t)(void* pool, void* handle);

ULONGLONG g_nextMs = 0;
int  g_seq = 0, g_haveSeq = 0, g_pending = 0, g_mode = 1;
char g_ids[64] = "48,49";

void Log(const char* fmt, ...)
{
    char b[256];
    va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
    Phase1_LogLine(b);
}

void SetHandle(char* ev, int off, char* h)
{
    InterlockedIncrement((volatile LONG*)(h + 4));
    char* old = (char*)(uintptr_t)InterlockedExchange((volatile LONG*)(ev + off), (LONG)(uintptr_t)h);
    if (old && !InterlockedDecrement((volatile LONG*)(old + 4)) && (*(const uint32_t*)(old + 8) & 0xFFFFFF) == 0)
        ((Release_t)0x00A094D0)((void*)0x04DD5FA0, old);
}

bool Pos(const void* e, float* p)
{
    // Entity::m_GlobalMatrix translation M41..M43 (Scimitar/Entity.h); the layout comes from the compiler.
    const Entity* en = (const Entity*)e;
    p[0] = en->m_GlobalMatrix.M41; p[1] = en->m_GlobalMatrix.M42; p[2] = en->m_GlobalMatrix.M43;
    return true;
}

int Fire(void* pup)
{
    char* h = *(char**)((char*)pup + 0xC8);
    if (!h) { Log("[PupStim] puppet has no handle\n"); return 0; }
    int ids[8], n = 0;
    for (char* s = g_ids; *s && n < 8; ) { ids[n++] = atoi(s); while (*s && *s != ',') ++s; if (*s) ++s; }
    int posted = 0;
    for (int k = 0; k < n; ++k) {
        const int id = ids[k];
        if (g_mode == 1) {
            char* ev = (char*)((NewEvent_t)0x00FA3300)();
            if (!ev) continue;
            *(uint32_t*)(ev + 0x10) = (uint32_t)id;
            SetHandle(ev, 0x0C, h);
            SetHandle(ev, 0x08, h);
            ((Post_t)0x0064A020)(ev, pup);
            ++posted;
            continue;
        }
        const char* mgr = *(const char* const*)0x04DE6D4C;
        if (!mgr) continue;
        const char* entry = (*(const char* const* const*)(mgr + 500))[id];
        const float r  = entry ? *(const float*)(entry + 40) : *(const float*)(mgr + 60);
        const float up = entry ? *(const float*)(entry + 52) : *(const float*)(mgr + 72);
        const float dn = entry ? *(const float*)(entry + 56) : *(const float*)(mgr + 76);
        float ps[3]; Pos(pup, ps);
        const char* const* arr = *(const char* const* const*)(mgr + 464);
        const unsigned cnt = *(const uint16_t*)(mgr + 470);
        int got = 0;
        for (unsigned i = 0; i < cnt; ++i) {
            if (!arr[i]) continue;
            char* e = *(char* const*)(arr[i] + 112);
            if (!e || e == (char*)pup || (*(const uint32_t*)(e + 156) & 7) == 1) continue;
            float pe[3]; Pos(e, pe);
            const float dx = pe[0] - ps[0], dy = pe[1] - ps[1], dz = pe[2] - ps[2];
            if (fabsf(dz) > (dz > 0 ? up : dn)) continue;
            if (dx * dx + dy * dy + dz * dz > r * r) continue;
            char* ev = (char*)((NewEvent_t)0x00FA3260)();
            if (!ev) continue;
            *(uint32_t*)(ev + 0x10) = (uint32_t)id;
            SetHandle(ev, 0x08, h);
            SetHandle(ev, 0x0C, h);
            ((Post_t)0x0064A020)(ev, e);
            ++got; ++posted;
        }
        Log("[PupStim] id=%d r=%.1f up=%.1f dn=%.1f receivers=%d (of %u listed)\n", id, r, up, dn, got, cnt);
    }
    return posted;
}

// ---- [Coop] FightSkipPuppet -------------------------------------------------------------------------------------
// Run 7164 (build b2ad941, mode 2): guards reacted to the puppet's gunshot and the host's fight system enlisted the
// puppet as a fighter. sub_161BD30 (fight member loop) -> sub_13F22E0(entity) -> nav service vt+64 -> sub_1780610
// (CSrvNavigation, vtable 0x026F4B70) read *(*(nav+0x84)+0x14)+0x0C with the middle pointer null: read fault
// eip=0x01780657 (AC4Coop_crashctx_7164_1.txt; the puppet entity is the argument on the stack).
// sub_13F22E0 is void __cdecl(Entity*), 6 callers (0xFE19BB, 0x14B6F7C, 0x156CD7C, 0x161BDEE, 0x1B03775,
// 0x1BE170B); it already returns without doing anything for an entity with no AI data (*(e+0x5C) == 0 or
// *(*(e+0x5C)+0x8DC) == 0). With the flag on, the puppet takes that same exit: the host never gives the guest's body
// fight orders (the guest drives it). Prologue (7 bytes, whole instructions):
//   0x013F22E0  55 / 8B EC / 51 / 8B 55 08   push ebp / mov ebp,esp / push ecx / mov edx,[ebp+8]
const uintptr_t VA_FIGHTMEMBER = 0x013F22E0;
const uintptr_t FM_STEAL = 7;
const BYTE kFmPrologue[FM_STEAL] = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x55, 0x08 };
DWORD g_fmTramp = 0;
volatile LONG g_fmSkipped = 0;
int g_fmOn = 0; ULONGLONG g_fmNext = 0;

int __cdecl FmSkip(void* e)
{
    __try {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_fmNext) { g_fmNext = now + 2000; g_fmOn = GetPrivateProfileIntA("Coop", "FightSkipPuppet", 0, Paths::Ini()) != 0; }
        if (!g_fmOn || !e) return 0;
        if (e != Phase1_PuppetEntity()) return 0;
        if (InterlockedIncrement(&g_fmSkipped) <= 20) Log("[PupStim] fight order for the puppet skipped (sub_13F22E0)\n");
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

__declspec(naked) void FmDetour()
{
    __asm {
        pushad
        mov  eax, [esp + 36]        // arg (original esp+4)
        push eax
        call FmSkip
        add  esp, 4
        test eax, eax
        popad
        jnz  skip
        jmp  dword ptr [g_fmTramp]
    skip:
        ret                         // cdecl: the caller pops the argument
    }
}

// ---- [Coop] NavNullGuard --------------------------------------------------------------------------------------
// The narrow alternative to FightSkipPuppet: let the fight order reach the puppet, and only stop the read that
// faulted. In sub_1780610 (CSrvNavigation, thiscall, 4 args, retn 10h):
//   0x01780647  8B 8E 84 00 00 00   mov ecx,[esi+84h]     <- 6 bytes stolen
//   0x0178064D  53 / 57 / 8B 79 14  push ebx / push edi / mov edi,[ecx+14h]
//   0x01780657  39 57 0C            cmp [edi+0Ch],edx     <- run 7164 fault (edi = 0)
// At 0x01780647 the frame holds only the pushed ebp and esi - the same depth as the function's own early exit at
// 0x01780640 (xor al,al / pop esi / pop ebp / retn 10h), which this guard takes when [esi+84h] or its +0x14 is null.
const uintptr_t VA_NAV = 0x01780647;
const uintptr_t NAV_STEAL = 6;
const BYTE kNavBytes[NAV_STEAL] = { 0x8B, 0x8E, 0x84, 0x00, 0x00, 0x00 };
DWORD g_navResume = 0;
volatile LONG g_navGuarded = 0;
LONG g_navLogged = 0;

__declspec(naked) void NavDetour()
{
    __asm {
        mov  ecx, [esi + 84h]
        test ecx, ecx
        jz   bad
        cmp  dword ptr [ecx + 14h], 0
        jz   bad
        jmp  dword ptr [g_navResume]
    bad:
        lock inc dword ptr [g_navGuarded]
        xor  al, al
        pop  esi
        pop  ebp
        ret  10h
    }
}

// ---- [Coop] PuppetNavService (default 0, read live) ---------------------------------------------------------------
// F7 root cause (ops/evidence/guard-ai/combat-nav.md "ROOT CAUSE", run 26404): the puppet's CSrvNavigation (vt
// 0x026F4B70, *(*(e+0xE8)+0x94)) was never started because the mod disables the puppet's behaviour component:
// +0x88 = 0, +0x100 = -1, +0x10C = garbage, u16 +0xA = 0, order -2 (player: valid). Every guard reachability test on
// the target reads these (C1 0x019285B8 / C2 0x005121B7 need +0xA != 0; CLConflict rule sub_129B510 path-queries the
// target's nav location), so guards pick status 2 / order 7 (intercept, no goal) instead of status 1 / order 2.
// Engine calls (IDA o109/o110, all thiscall ecx = svc, no args, plain retn):
//   sub_A011D0  statechart start: svc slot 8 sub_1792F10 (owner +0x88, entity +0x7C, combat obj +0x80, child services),
//               then pushes the root state (+0x4/+0xA). Called once per service.
//   sub_179CDF0 svc slot 45 UpdateNavLocation: +0x10C..+0x114 position from +0x7C, +0x100/+0x104 via sub_1793290;
//               called each puppet update, as the engine's teleport helper sub_196ED20 does after a transform write.
// UNVERIFIED: that the started service's idle order-0 state never moves the puppet on its own.
const unsigned  NAVSVC_VT = 0x026F4B70;
const uintptr_t VA_NAVSVC_START = 0x00A011D0;
const uintptr_t VA_NAVSVC_LOCATE = 0x0179CDF0;
int g_nsOn = 0; ULONGLONG g_nsNext = 0; void* g_nsStarted = nullptr; LONG g_nsTicks = 0, g_nsFaults = 0;

void NavServiceTick(void* pup)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nsNext) { g_nsNext = now + 2000; g_nsOn = GetPrivateProfileIntA("Coop", "PuppetNavService", 0, Paths::Ini()) != 0; }
    if (!g_nsOn) return;
    __try {
        char* aic = *(char**)((char*)pup + 0xE8);
        char* svc = aic ? *(char**)(aic + 0x94) : nullptr;
        if (!svc || *(const unsigned*)svc != NAVSVC_VT) return;
        if (*(const uintptr_t*)(*(const char* const*)svc + 45 * 4) != VA_NAVSVC_LOCATE) return;
        if (g_nsStarted != svc) {
            g_nsStarted = svc;
            if (*(const uint16_t*)(svc + 0xA) == 0 && *(void* const*)(svc + 0x88) == nullptr) {
                Hook::CallClassMethod<int, char*>(VA_NAVSVC_START, svc);
                const char* ctx = *(const char* const*)(svc + 0x84);
                Log("[PupNav] started svc=%p: +0xA=%u +0x88=%p +0x7C=%p +0x80=%p order=%d\n", svc,
                    (unsigned)*(const uint16_t*)(svc + 0xA), *(void* const*)(svc + 0x88), *(void* const*)(svc + 0x7C),
                    *(void* const*)(svc + 0x80), ctx ? *(const int*)(ctx + 0x20) : -9);
            } else {
                Log("[PupNav] svc=%p already started (+0xA=%u)\n", svc, (unsigned)*(const uint16_t*)(svc + 0xA));
            }
        }
        Hook::CallClassMethod<void, char*>(VA_NAVSVC_LOCATE, svc);
        if ((++g_nsTicks % 600) == 1) {
            const float* p = (const float*)(svc + 0x10C);
            Log("[PupNav] tick=%ld +0xA=%u +0x100=%08X +0x104=%08X pos=(%.1f,%.1f,%.1f)\n", g_nsTicks,
                (unsigned)*(const uint16_t*)(svc + 0xA), *(const unsigned*)(svc + 0x100), *(const unsigned*)(svc + 0x104), p[0], p[1], p[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (++g_nsFaults <= 10) Log("[PupNav] EXCEPTION 0x%08X\n", GetExceptionCode());
    }
}

}  // namespace

void PuppetStim::Init()
{
    if (GetPrivateProfileIntA("Coop", "NavNullGuard", 0, Paths::Ini()) == 1) {
        const uintptr_t nav = GameModuleBase + (VA_NAV - 0x00400000);
        if (memcmp((const void*)nav, kNavBytes, NAV_STEAL) == 0) {
            g_navResume = (DWORD)(nav + NAV_STEAL);
            Hook::DetourFunc((BYTE*)nav, (DWORD)(uintptr_t)&NavDetour, (DWORD)NAV_STEAL);
        } else {
            OutputDebugStringA("[PupStim] bytes mismatch at 0x1780647 - NavNullGuard not installed\n");
        }
    }
    if (GetPrivateProfileIntA("Coop", "FightSkipPuppet", -1, Paths::Ini()) < 0) return;   // key absent: no hook
    const uintptr_t site = GameModuleBase + (VA_FIGHTMEMBER - 0x00400000);
    if (memcmp((const void*)site, kFmPrologue, FM_STEAL) != 0) { OutputDebugStringA("[PupStim] prologue mismatch at 0x13F22E0\n"); return; }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kFmPrologue, FM_STEAL);
    t[FM_STEAL] = 0x68;                                        // push imm32; ret = absolute jump to the resume
    *reinterpret_cast<DWORD*>(t + FM_STEAL + 1) = (DWORD)(site + FM_STEAL);
    t[FM_STEAL + 5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), t, FM_STEAL + 6);
    g_fmTramp = (DWORD)(uintptr_t)t;
    Hook::DetourFunc((BYTE*)site, (DWORD)(uintptr_t)&FmDetour, (DWORD)FM_STEAL);
}

void PuppetStim::OnPuppetUpdate(void* pup)
{
    if (!pup || !Phase1_IsEntityHost()) return;
    NavServiceTick(pup);   // [Coop] PuppetNavService (default 0)
    const LONG ng = g_navGuarded;
    if (ng != g_navLogged && g_navLogged < 50) { g_navLogged = ng; Log("[PupStim] NavNullGuard took the null exit %ld time(s)\n", ng); }
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextMs) {
        g_nextMs = now + 1000;
        const int seq = GetPrivateProfileIntA("Coop", "PuppetStimSeq", 0, Paths::Ini());
        g_mode = GetPrivateProfileIntA("Coop", "PuppetStimMode", 1, Paths::Ini());
        GetPrivateProfileStringA("Coop", "PuppetStimIds", "48,49", g_ids, sizeof(g_ids), Paths::Ini());
        if (!g_haveSeq) { g_haveSeq = 1; g_seq = seq; }
        else if (seq != g_seq) { g_seq = seq; g_pending = 1; }
    }
    if (!g_pending) return;
    g_pending = 0;
    // [Coop] PuppetStimSource (default 0, read per fire): 1 = emit from the LOCAL PLAYER instead of the puppet, as the
    // positive control for F7 (guards fighting the real player vs. the puppet, same stimulus ids and receiver rule).
    // The player is World::GetCurrentWorld()->GetPlayer() (as fight_probe.cpp reads it); handle *(e+0xC8) as above.
    void* src = pup;
    const int source = GetPrivateProfileIntA("Coop", "PuppetStimSource", 0, Paths::Ini());
    if (source == 1) {
        World* w = World::GetCurrentWorld();
        void* pl = w ? (void*)w->GetPlayer() : nullptr;
        if (!pl) { Log("[PupStim] source=player but no player\n"); return; }
        src = pl;
    }
    int posted = -1;
    __try { posted = Fire(src); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[PupStim] EXCEPTION 0x%08X (nothing more posted)\n", GetExceptionCode()); }
    Log("[PupStim] fired seq=%d mode=%d ids=%s posted=%d source=%s src=%p handle=%08X\n", g_seq, g_mode, g_ids, posted,
        source == 1 ? "PLAYER" : "puppet", src, (unsigned)*(uintptr_t*)((char*)src + 0xC8));
}

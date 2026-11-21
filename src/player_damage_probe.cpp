// [Coop] PlayerDamageProbe (default 0, read at load): read-only hit counters along the damage chain, for the puppet
// (the peer's Edward on this machine) and, as the positive control, the local player. The first counter that stays 0
// for the puppet while the player's counters move is where a guard's hit on the puppet is lost
// (docs/evidence/player-damage-path-2026-11-16.md, Q2). Observation only: every hook calls the original unchanged.
//
// Sites (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, ida_batch pd1 2026-11-16; bytes quoted):
//   H1 0x0064A080  55 8B EC 8B 4D 08 | 85 C9 ...   event vt[9] "listener for receiver": __stdcall(recv) reached as
//                  thiscall(ev, recv), retn 4; returns sub_5220E0(recv) (0 = nobody listens, the post is dropped at
//                  0x64A035 / 0x64D8D8). Shared by FightDamageEvent (vt 0x02700720) and DamageEvent (vt 0x027214B8).
//                  Steal 6 (3 whole insns), call-through wrapper to read the return value.
//   H2 0x0186B370  55 8B EC 51 56 8B 75 08 | 8B 46 54   CSHumanFightInteractions slot 18, thiscall(this, FDE), retn 4.
//                  Victim entity = *(*(this+0x74)+8) (0x186B3A3 `mov edi,[edi+74h]`, 0x186B3CA `mov ebx,[edi+8]`).
//                  FDE+0x54 selects the path (0x186B378/0x186B390: 1 = sub_18644D0 hit resolution). Steal 8.
//   H3 0x01998D40  53 8B DC 83 EC 08 83 E4 F0 83 C4 04 | 55 8B 6B 04   builds the DamageEvent; at entry [esp+4] = FDE,
//                  [esp+8] = final damage, [esp+0xC] = skip flag (args at [ebx+8..] after `push ebx; mov ebx,esp`).
//                  Counted only inside an H2 call whose victim is the puppet / player (TLS). Steal 12, count-only.
//   H4 0x0173E190  55 8B EC 83 EC 24 | 53 56 57   CSDamage listener body, thiscall(this, ev); victim entity
//                  = *(*(this+4)+8) (doc step 5: `esi = [[ebx+4]+8]`, ebx = ecx). Steal 6, count-only.
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>

void Phase1_LogLine(const char* s);
void* Phase1_PuppetEntity();
// phase1.cpp, [Coop] PlayerDamageRelay: a guard's hit on the puppet, as the engine would have scored it on a player.
void Phase1_PuppetHitPosted(uintptr_t attacker, int damage, uint32_t contact, uint32_t subtype, uint32_t f18, uint8_t flags);

#include "player_damage_probe.h"
#include "attack_forward.h"
#include "combat_timing.h"

namespace {

const uintptr_t VT_FDE = 0x02700720, VT_DE = 0x027214B8;

bool g_installed = false;   // probe counters / report
bool g_probe = false, g_relay = false, g_relayIni = false, g_atkFwd = false;   // g_relay: ini AND the H1 hook is in (the sending side)
DWORD g_t1 = 0, g_t2 = 0, g_t3 = 0, g_t4 = 0;

volatile uintptr_t g_pup = 0, g_pl = 0;
volatile ULONGLONG g_nextMs = 0;
__declspec(thread) int t_victim = 0;   // 1 puppet, 2 player: inside an H2 call for that victim

// [who][counter]: who 0 = puppet, 1 = player
enum { C_FDE_POST, C_FDE_NOL, C_FDE_HIT, C_DE_POST, C_DE_NOL, C_H2, C_H2_HIT, C_H3, C_H3_SKIP, C_H4, C_N };
volatile LONG g_c[3][C_N];      // window   [who]: 0 puppet, 1 player, 2 the player's target (FDE attacker == player)
volatile LONG g_tot[3][C_N];    // session
LONG g_lines = 0;
LONG g_kindLogged = 0;

ULONGLONG NowMs()
{
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (ULONGLONG)((c.QuadPart / f.QuadPart) * 1000ull + (c.QuadPart % f.QuadPart) * 1000ull / f.QuadPart);
}

void Refresh()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextMs) return;
    g_nextMs = now + 250;
    uintptr_t pl = 0;
    __try { World* w = World::GetCurrentWorld(); pl = (uintptr_t)(w ? w->GetPlayer() : nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { pl = 0; }
    g_pl = pl;
    g_pup = (uintptr_t)Phase1_PuppetEntity();
    if (g_pup && !g_kindLogged) {
        g_kindLogged = 1;
        __try {
            const uint32_t k = *(const uint32_t*)(g_pup + 0x9C);
            const uint32_t kp = g_pl ? *(const uint32_t*)(g_pl + 0x9C) : 0;
            char line[160];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[PDP] puppet=%08X +9C=%08X kind=%u | player=%08X +9C=%08X kind=%u\n",
                        (unsigned)g_pup, k, k & 7, (unsigned)g_pl, kp, kp & 7);
            Phase1_LogLine(line);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

int Who(uintptr_t e)
{
    if (!e) return -1;
    Refresh();
    if (e == g_pup) return 0;
    if (e == g_pl) return 1;
    return -1;
}

void Count(int who, int c) { InterlockedIncrement(&g_c[who][c]); InterlockedIncrement(&g_tot[who][c]); }

void Line(const char* fmt, ...)
{
    if (!g_probe || InterlockedIncrement(&g_lines) > 1500) return;
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = _snprintf_s(b, sizeof(b), _TRUNCATE, "[PDP] t=%llu ", NowMs());
    if (n > 0) _vsnprintf_s(b + n, sizeof(b) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    strcat_s(b, "\n");
    Phase1_LogLine(b);
}

const char* WhoName(int w) { return w == 0 ? "puppet" : w == 1 ? "player" : "player-target"; }
// FDE+0x58 attacker handle -> entity ([h+8] < 0 ? [h] : 0, 0x186B3C6..0x186B3D3); 2 when it is the local player.
int WhoByAttacker(const void* fde)
{
    const uintptr_t h = *(const uintptr_t*)((const char*)fde + 0x58);
    if (!h) return -1;
    const uintptr_t att = (*(const int*)(h + 8) < 0) ? *(const uintptr_t*)h : 0;
    Refresh();
    return (att && att == g_pl) ? 2 : -1;
}

// H1: call-through wrapper (fastcall = ecx event, edx unused, stack recv; callee pops 4 = the original's retn 4).
typedef int(__thiscall* Vt9Fn)(void* ev, void* recv);
int __fastcall H1(void* ev, void* /*edx*/, void* recv)
{
    const int r = ((Vt9Fn)g_t1)(ev, recv);
    __try {
        int w = Who((uintptr_t)recv);
        if (w < 0 && ev && recv && *(const uintptr_t*)ev == VT_FDE) w = WhoByAttacker(ev);   // the player's own strikes
        if (w >= 0 && ev) {
            const uintptr_t vt = *(const uintptr_t*)ev;
            if (vt == VT_FDE || vt == VT_DE) {
                const bool fde = vt == VT_FDE;
                Count(w, fde ? C_FDE_POST : C_DE_POST);
                if (!r) Count(w, fde ? C_FDE_NOL : C_DE_NOL);
                // Attack-forwarding RE (2026-11-16): who posts a FightDamageEvent to the local player, per phase (FDE+0x54),
                // and the whole event once per (phase, caller). Frames from RtlCaptureStackBackTrace (the game's code keeps
                // ebp frames); [0] is this wrapper's caller inside 0x64A020 / 0x64D8C0.
                if (fde && w == 1 && g_probe) {
                    static LONG s_bt = 0;
                    if (InterlockedIncrement(&s_bt) <= 120) {
                        void* fr[8] = {}; const USHORT n = RtlCaptureStackBackTrace(0, 8, fr, nullptr);
                        const uint32_t ph = *(const uint32_t*)((const char*)ev + 0x54);
                        Line("bt FDE->player ph=%u ev=%p n=%u %08X %08X %08X %08X %08X %08X", ph, ev, n, (unsigned)(uintptr_t)fr[1],
                             (unsigned)(uintptr_t)fr[2], (unsigned)(uintptr_t)fr[3], (unsigned)(uintptr_t)fr[4], (unsigned)(uintptr_t)fr[5],
                             (unsigned)(uintptr_t)fr[6]);
                        static uintptr_t s_dumped[16]; static int s_nd = 0;
                        const uintptr_t key = (uintptr_t)fr[2] ^ (ph << 28);
                        bool seen = false; for (int i = 0; i < s_nd; ++i) if (s_dumped[i] == key) seen = true;
                        if (!seen && s_nd < 16) {
                            s_dumped[s_nd++] = key;
                            const uint32_t* d = (const uint32_t*)ev;
                            for (int row = 0; row < 0x170 / 4; row += 8)
                                Line("dump ph=%u +%03X: %08X %08X %08X %08X %08X %08X %08X %08X", ph, row * 4, d[row], d[row + 1], d[row + 2],
                                     d[row + 3], d[row + 4], d[row + 5], d[row + 6], d[row + 7]);
                        }
                    }
                }
                if (fde && w == 0 && CombatTiming::On()) {   // [Coop] CombatTimingProbe: every FDE a guard posts to the puppet
                    const uintptr_t th = *(const uintptr_t*)((const char*)ev + 0x58);
                    const uintptr_t tat = th && *(const int*)(th + 8) < 0 ? *(const uintptr_t*)th : 0;
                    CombatTiming::HostFde(tat, (uintptr_t)recv, (int)*(const uint32_t*)((const char*)ev + 0x54),
                                          *(const uint32_t*)((const char*)ev + 8), *(const uint32_t*)((const char*)ev + 0x10));
                }
                if (fde && w == 0 && g_atkFwd && *(const uint32_t*)((const char*)ev + 8) != 35) {
                    const uintptr_t ah = *(const uintptr_t*)((const char*)ev + 0x58);
                    const uintptr_t aat = ah && *(const int*)(ah + 8) < 0 ? *(const uintptr_t*)ah : 0;
                    if (aat && *(const uintptr_t*)(aat + 0xC8) == ah) {
                        AtkFields af; AttackForward_Capture(ev, af);
                        af.dmg = 0;
                        if (af.phase) {   // same gate and call as the hit-phase block below (0x18644D0 lines 343-360)
                            const char* ff = (const char*)ev;
                            const uint32_t ty = *(const uint32_t*)(ff + 8), su = *(const uint32_t*)(ff + 0x10);
                            if (*(const uint8_t*)(ff + 333) && ty != 26 && *(const uint32_t*)(aat + 0x5C) && !*(const uint8_t*)(ff + 323)) {
                                typedef int(__thiscall* DmgFn)(void*, uint32_t, void*, void*, uint32_t, void*, void*);
                                af.dmg = ((DmgFn)0x0199A160)(*(void* const*)0x04E25974, ty, recv, (void*)aat, su, (void*)(ff + 304), (void*)ff);
                            }
                        }
                        Phase1_PuppetAttackPosted(aat, af);
                    }
                }
                if (fde && *(const uint32_t*)((const char*)ev + 0x54) == 1) {
                    // hit phase (0x186B390): every one is logged. FDE+0x58 = attacker handle; entity = [h+8] < 0 ? [h] : 0
                    // (0x186B3C6..0x186B3D3 `mov edi,[esi+8]; sar edi,1Fh; and edi,[esi]`).
                    Count(w, C_FDE_HIT);
                    const char* f = (const char*)ev;
                    const uintptr_t h = *(const uintptr_t*)(f + 0x58);
                    uintptr_t att = 0, attHc = 0;
                    if (h) { att = (*(const int*)(h + 8) < 0) ? *(const uintptr_t*)h : 0; if (att) attHc = *(const uintptr_t*)(att + 0xC8); }
                    // The engine's base damage, as 0x18644D0 computes it (decompile lines 343-360): only when FDE+333 != 0,
                    // type FDE+8 not 35/26, attacker with entity+0x5C, FDE+323 == 0:
                    //   sub_199A160(this = dword_4E25974 (sub_197B680: `mov eax, dword_4E25974; retn`), FDE+8, victim,
                    //               attacker, FDE+0x10, FDE+304, FDE)   thiscall, retn 18h (6 args), ida_batch pd3.
                    const uint32_t type = *(const uint32_t*)(f + 8), subt = *(const uint32_t*)(f + 0x10);
                    const uint8_t b333 = *(const uint8_t*)(f + 333), b323 = *(const uint8_t*)(f + 323);
                    int calc = -1;
                    if (b333 && type != 35 && type != 26 && att && *(const uint32_t*)(att + 0x5C) && !b323) {
                        typedef int(__thiscall* DmgFn)(void*, uint32_t, void*, void*, uint32_t, void*, void*);
                        calc = ((DmgFn)0x0199A160)(*(void* const*)0x04E25974, type, recv, (void*)att, subt, (void*)(f + 304), (void*)f);
                    }
                    Line("hit-post %s recv=%p ev=%p type=%u sub=%u b333=%u b323=%u calc=%d +C=%d +1C=%08X +20=%u attacker=%08X (hc %s) listener=%08X",
                         WhoName(w), recv, ev, type, subt, b333, b323, calc, *(const int*)(f + 0xC), *(const uint32_t*)(f + 0x1C),
                         *(const uint32_t*)(f + 0x20), (unsigned)att, attHc == h ? "==+58" : "!=+58", (unsigned)r);
                    // [Coop] PlayerDamageRelay: the puppet has no fight / health services, so this hit stops here (probe
                    // runs 25140 / 19900: 0 fight-interactions, 0 DamageEvents for the puppet vs 1:1 for the player). Hand it
                    // to the puppet's owner. calc matched the engine's DamageEvent damage on 6/6 player hits (run 25140).
                    // With [Coop] AttackForward the owner resolves non-35 hits natively (the forwarded phase-1 event); only
                    // type 35 (the contact poster sub_1D1A4A0, which this forward does not rebuild) stays on the relay.
                    if (g_relay && w == 0 && calc > 0 && attHc == h && !(g_atkFwd && type != 35))
                        Phase1_PuppetHitPosted(att, calc, *(const uint32_t*)(f + 0x20), subt, *(const uint32_t*)(f + 0x18),
                                               *(const uint8_t*)(f + 0x1C));
                }
                // one line per (who, class, listener found) per 2 s at most
                static ULONGLONG last[3][2][2];
                const ULONGLONG now = GetTickCount64();
                ULONGLONG& l = last[w][fde ? 0 : 1][r ? 1 : 0];
                if (now - l > 2000) {
                    l = now;
                    Line("post %s -> %s ev=%p listener=%08X%s", fde ? "FightDamageEvent" : "DamageEvent", WhoName(w), ev,
                         (unsigned)r, r ? "" : " (NONE: dropped)");
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// H2: call-through wrapper, sets the TLS victim for H3.
typedef void(__thiscall* H2Fn)(void* self, void* fde);
void __fastcall H2(void* self, void* /*edx*/, void* fde)
{
    int w = -1;
    __try {
        const uintptr_t fi = *(const uintptr_t*)((const char*)self + 0x74);
        const uintptr_t victim = fi ? *(const uintptr_t*)(fi + 8) : 0;
        w = Who(victim);
        if (w < 0 && victim) w = WhoByAttacker(fde);
        if (w >= 0) {
            Count(w, C_H2);
            const uint32_t ph = *(const uint32_t*)((const char*)fde + 0x54);
            if (ph == 1) Count(w, C_H2_HIT);
            static ULONGLONG last[3];
            const ULONGLONG now = GetTickCount64();
            if (ph == 1 || now - last[w] > 1000) {
                last[w] = now;
                Line("fight-interactions %s victim=%08X fde=%p +54=%u +C=%d +20=%u +1C=%08X", WhoName(w), (unsigned)victim, fde, ph,
                     *(const int*)((const char*)fde + 0xC), *(const uint32_t*)((const char*)fde + 0x20),
                     *(const uint32_t*)((const char*)fde + 0x1C));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { w = -1; }
    const int prev = t_victim;
    t_victim = w + 1;
    ((H2Fn)g_t2)(self, fde);
    t_victim = prev;
}

void __cdecl H3Note(const uint32_t* args)   // args[0] = ret, [1] = FDE, [2] = damage, [3] = skip
{
    const int w = t_victim - 1;
    if (w < 0) return;
    __try {
        Count(w, C_H3);
        if (args[3] & 0xFF) Count(w, C_H3_SKIP);   // a bool: upper bytes are stack garbage (seen 0x3A7F0000)
        Line("damage-event-build %s fde=%08X damage=%d skip(low byte)=%u from=%08X", WhoName(w), args[1], (int)args[2], args[3] & 0xFF, args[0]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void H3()
{
    __asm {
        pushad
        lea  eax, [esp + 32]
        push eax
        call H3Note
        add  esp, 4
        popad
        jmp  dword ptr [g_t3]
    }
}

void __cdecl H4Note(uintptr_t self, const uint32_t* args)   // args[0] = ret, [1] = event
{
    __try {
        const uintptr_t sub = *(const uintptr_t*)(self + 4);
        const uintptr_t victim = sub ? *(const uintptr_t*)(sub + 8) : 0;
        const int w = Who(victim);
        if (w < 0) return;
        Count(w, C_H4);
        const uintptr_t vt = args[1] ? *(const uintptr_t*)args[1] : 0;
        static ULONGLONG last[2];
        const ULONGLONG now = GetTickCount64();
        if (vt == VT_DE || now - last[w] > 2000) {
            last[w] = now;
            Line("csdamage-listener %s ev=%08X vt=%08X%s", WhoName(w), args[1], (unsigned)vt, vt == VT_DE ? " (DamageEvent)" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void H4()
{
    __asm {
        pushad
        lea  eax, [esp + 32]
        push eax
        push ecx
        call H4Note
        add  esp, 8
        popad
        jmp  dword ptr [g_t4]
    }
}

bool Install(uintptr_t va, const BYTE* pro, int n, void* detour, const char* name, DWORD* slot)
{
    BYTE* site = (BYTE*)(GameModuleBase + (va - 0x00400000));
    char line[160];
    if (memcmp(site, pro, n) != 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[PDP] %s %08X prologue mismatch (another hook?) - not installed\n", name, (unsigned)va);
        Phase1_LogLine(line);
        return false;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, pro, n);
    t[n] = 0x68; *(DWORD*)(t + n + 1) = (DWORD)(uintptr_t)(site + n); t[n + 5] = 0xC3;   // push resume; ret
    FlushInstructionCache(GetCurrentProcess(), t, n + 6);
    *slot = (DWORD)(uintptr_t)t;                     // the trampoline is published before the jump is written
    if (!Hook::DetourFunc(site, (DWORD)(uintptr_t)detour, (DWORD)n)) { *slot = 0; return false; }
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[PDP] %s %08X hooked\n", name, (unsigned)va);
    Phase1_LogLine(line);
    return true;
}

}  // namespace

void PlayerDamageProbe::Init()
{
    g_probe = GetPrivateProfileIntA("Coop", "PlayerDamageProbe", 0, Paths::Ini()) != 0;
    // Default follows [Coop] FightIntents (verified live 2026-11-16, run 29952/13796: 13 of 13 relayed hits applied natively,
    // docs/evidence/player-damage-relay-2026-11-16.md); an explicit PlayerDamageRelay=0 turns it off. Read once, here.
    const int fi = GetPrivateProfileIntA("Coop", "FightIntents", 0, Paths::Ini()) != 0 ? 1 : 0;
    g_relayIni = GetPrivateProfileIntA("Coop", "PlayerDamageRelay", fi, Paths::Ini()) != 0;
    g_relay = g_relayIni;
    g_atkFwd = GetPrivateProfileIntA("Coop", "AttackForward", 0, Paths::Ini()) != 0;   // default 0 until verified live
    if (!g_probe && !g_relay && !g_atkFwd) return;
    static const BYTE p1[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08 };
    static const BYTE p2[8] = { 0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0x75, 0x08 };
    static const BYTE p3[12] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04 };
    static const BYTE p4[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 };
    bool any = false;
    const bool h1 = Install(0x0064A080, p1, 6, (void*)&H1, "H1 event-listener", &g_t1);   // probe and relay
    if (!h1) g_relay = false;
    if (g_probe) {
        any |= h1;
        any |= Install(0x0186B370, p2, 8, (void*)&H2, "H2 fight-interactions", &g_t2);
        any |= Install(0x01998D40, p3, 12, (void*)&H3, "H3 damage-event-build", &g_t3);
        any |= Install(0x0173E190, p4, 6, (void*)&H4, "H4 csdamage-listener", &g_t4);
    }
    g_installed = any;
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[PDR] PlayerDamageRelay=%d PlayerDamageProbe=%d (read once)\n", g_relay ? 1 : 0, g_probe ? 1 : 0);
    Phase1_LogLine(line);
}

bool PlayerDamageProbe::RelayOn() { return g_relayIni; }
bool AttackForward_On() { return g_atkFwd; }

void AttackForward_Capture(const void* ev, AtkFields& a)
{
    const char* f = (const char*)ev;
    auto B = [&](int o) { return *(const uint8_t*)(f + o); };
    auto D = [&](int o) { return *(const uint32_t*)(f + o); };
    a.phase = (uint8_t)D(0x54); a.type = (uint8_t)D(0x08); a.sub = (uint8_t)D(0x10);
    a.b331 = B(0x14B); a.b328 = B(0x148); a.b322 = B(0x142); a.b323 = B(0x143); a.b324 = B(0x144); a.b325 = B(0x145);
    a.b326 = B(0x146); a.b327 = B(0x147); a.b329 = B(0x149); a.b330 = B(0x14A); a.b361 = B(0x169); a.b308 = B(0x134); a.b1C = B(0x1C);
    a.f336 = D(0x150); a.f60 = D(0x60); a.f5C = D(0x5C); a.f50 = D(0x50); a.f110 = D(0x110); a.f124 = D(0x124); a.f128 = D(0x128);
    a.f120 = D(0x120); a.f11C = D(0x11C); a.f114 = D(0x114); a.f130 = D(0x130);
    memcpy(a.dir, f + 0x30, 16); memcpy(a.pos, f + 0x40, 16);
}

// The owner-side rebuild, as sub_16551A0 does it (ida_batch fa2): new FDE (sub_10EFC30: cdecl, 0x170 bytes, ctor 0x18C9470),
// sub_18BD690 (thiscall, 8 args, retn 20h, fa3), the copied fields, phase at +0x54, then sub_64D8C0(fde, victim) as
// sub_18DA720 posts it (the second event sub_18DA720 sends to the attacker side when fde+0x90 != 0 is not rebuilt).
static int AfFaultNote(EXCEPTION_POINTERS* ep)
{
    static LONG s_n = 0;
    if (InterlockedIncrement(&s_n) <= 12) {
        char line[200];
        const EXCEPTION_RECORD* r = ep->ExceptionRecord;
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[AF] post fault code=%08X at=%08X rw=%u addr=%08X\n", (unsigned)r->ExceptionCode,
                    (unsigned)(uintptr_t)r->ExceptionAddress, r->NumberParameters > 0 ? (unsigned)r->ExceptionInformation[0] : 0u,
                    r->NumberParameters > 1 ? (unsigned)r->ExceptionInformation[1] : 0u);
        Phase1_LogLine(line);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
bool AttackForward_Post(void* victim, void* attacker, const AtkFields& a)
{
    __try {
        char* f = ((char* (__cdecl*)())0x010EFC30)();
        if (!f) return false;
        float dir[4], pos[4]; memcpy(dir, a.dir, 16); memcpy(pos, a.pos, 16);
        ((void* (__thiscall*)(void*, int, int, int, void*, void*, char, char, void*))0x018BD690)(
            f, a.type, a.sub, (int)a.f336, dir, pos, (char)a.b331, (char)a.b328, attacker);
        auto SB = [&](int o, uint8_t v) { *(uint8_t*)(f + o) = v; };
        auto SD = [&](int o, uint32_t v) { *(uint32_t*)(f + o) = v; };
        SD(0x60, a.f60); SB(0x146, a.b326); SB(0x147, a.b327); SD(0x110, a.f110); SB(0x14A, a.b330); SB(0x142, a.b322);
        SB(0x145, a.b325); SB(0x149, a.b329); SD(0x124, a.f124); SD(0x128, a.f128); SD(0x120, a.f120); SD(0x50, a.f50);
        SB(0x169, a.b361); SB(0x143, a.b323); SB(0x144, a.b324); SD(0x11C, a.f11C); SD(0x114, a.f114);
        SD(0x130, a.f130); SB(0x134, a.b308); SD(0x5C, a.f5C); SD(0x54, a.phase ? 1u : 0u); SB(0x1C, a.b1C);   // +0x1C as seen at post time on the host
        ((void(__thiscall*)(void*, void*))0x0064D8C0)(f, victim);
        return true;
    } __except (AfFaultNote(GetExceptionInformation())) { return false; }
}

int PlayerDamageProbe::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    int n = 0;
    for (int w = 0; w < 3 && n >= 0 && n < cap - 8; ++w) {
        LONG c[C_N];
        for (int i = 0; i < C_N; ++i) c[i] = InterlockedExchange(&g_c[w][i], 0);
        const int k = _snprintf_s(out + n, cap - n, _TRUNCATE,
            "%s[PDP] %s: FDEpost=%ld (noListener %ld, hitPhase %ld) DEpost=%ld (noListener %ld) fightInter=%ld (hit %ld) deBuild=%ld (skip %ld) csDamage=%ld | total FDE=%ld DE=%ld fi=%ld build=%ld csd=%ld",
            w ? "\n" : "", WhoName(w), c[C_FDE_POST], c[C_FDE_NOL], c[C_FDE_HIT], c[C_DE_POST], c[C_DE_NOL], c[C_H2], c[C_H2_HIT], c[C_H3],
            c[C_H3_SKIP], c[C_H4], g_tot[w][C_FDE_POST], g_tot[w][C_DE_POST], g_tot[w][C_H2], g_tot[w][C_H3], g_tot[w][C_H4]);
        if (k < 0) break;
        n += k;
    }
    return n;
}

// Owner side of [Coop] PlayerDamageRelay: post a DamageEvent to `victim` (the local player) exactly as 0x1998D40 does at
// 0x1999040..0x19990BE (bytes read in ida_batch pd5), so the native CSDamage chain decides armour, death and the HUD:
//   ev = sub_1029240()                                      cdecl, no args, `retn` (0x10292D7): new 0x28-byte DamageEvent
//   sub_1B86E40(ev, contact, damage, attacker, flags&1, (flags&2)!=0, 35, 0, 0)   thiscall, 8 args, `retn 20h`
//   ev+0x1C bits 0x20 / 0x10 from flags, ev+0x10 = subtype, ev+0x18 = f18   (after the Init call, as the original)
//   EnterCriticalSection(**(victim+0xD4)) if *(victim+0xD4) (`mov edi,[edx+0D4h]` ; sub_D587F0 = `mov eax,[ecx]; push
//   eax; call EnterCriticalSection`), sub_64D8C0(ecx = ev, victim) thiscall `retn 4`, then LeaveCriticalSection.
// With type 35 at Init and ev+0x10 still the ctor's value, 0x173E7C0 skips the sub_199A160 recompute (needs ev+8 != 35),
// so `damage` is applied as given. UNVERIFIED: the ctor value of ev+0x10 (0x1B70940 not read); the live run checks it.
// Returns false on a fault. Game thread only (the guest's player frame).
bool PlayerDamage_PostDamageEvent(void* victim, void* attacker, int damage, uint32_t contact, uint32_t subtype, uint32_t f18, uint8_t flags)
{
    __try {
        char* ev = ((char* (__cdecl*)())0x01029240)();
        if (!ev) return false;
        ((int(__thiscall*)(void*, uint32_t, int, void*, char, char, int, char, int))0x01B86E40)(
            ev, contact, damage, attacker, (char)(flags & 1), (char)((flags & 2) != 0), 35, 0, 0);
        uint8_t b = *(uint8_t*)(ev + 0x1C);
        b ^= (flags ^ b) & 0x20;
        *(uint32_t*)(ev + 0x10) = subtype;
        b ^= (flags ^ b) & 0x10;
        *(uint8_t*)(ev + 0x1C) = b;
        *(uint32_t*)(ev + 0x18) = f18;
        CRITICAL_SECTION** cs = *(CRITICAL_SECTION***)((char*)victim + 0xD4);
        if (cs) EnterCriticalSection(*cs);
        __try { ((void(__thiscall*)(void*, void*))0x0064D8C0)(ev, victim); }
        __finally { if (cs) LeaveCriticalSection(*cs); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [Coop] ConflictProbe (default 0, read at load; observation only). Who asks the FightManager to start a fight, for the
// player vs the co-op puppet. docs/evidence/conflict-gate-2026-11-17.md: in a live run the puppet never gets a round and
// no team exists while guards face it, so the conflict starter is never reached for it. This names the caller.
// AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA 2026-11-17 (D:\ac4ida\cc jobs v3/v4):
//   sub_1625330 StartConflict  thiscall(fm, a2 attacker, a3 target, u8 a4), retn 0Ch (0x0162558C), 16 callers.
//     0x01625330  55 / 8B EC / 83 EC 14        push ebp / mov ebp,esp / sub esp,14h        <- 6 bytes stolen
//   sub_1620140 AttackApi      thiscall(fm, a2, a3, u8 a4),          retn 0Ch (0x0162021B), 9 callers.
//     0x01620140  55 / 8B EC / 51 / 53         push ebp / mov ebp,esp / push ecx / push ebx <- 5 bytes stolen
//   The only caller of sub_1622990 (team/round creation) is sub_1625330 (0x016253C5).
// Logs every call whose a2 or a3 is the local player or the puppet, with the return address and two EBP-chain return
// addresses above it (<= 400 lines), plus a per-10 s count of all calls.
#include "conflict_probe.h"
#include "Hook.h"
#include "paths.h"
#include "Scimitar/World.h"
#include "Scimitar/Entity.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>

void Phase1_LogLine(const char* s);
void* Phase1_PuppetEntity();

namespace {

const uintptr_t VA_START  = 0x01625330;
const uintptr_t VA_ATTACK = 0x01620140;
const BYTE kProStart[6]  = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 };
const BYTE kProAttack[5] = { 0x55, 0x8B, 0xEC, 0x51, 0x53 };
DWORD g_trStart = 0, g_trAttack = 0;
volatile LONG g_lines = 0;
volatile LONG g_nStart = 0, g_nAttack = 0, g_nBuild = 0;
volatile LONG g_nDrive = 0, g_nDrivePup = 0, g_nDrivePl = 0;
volatile LONG g_nSelPl = 0, g_nSelPup = 0;

bool InStack(uintptr_t p)
{
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return p >= (uintptr_t)tib->StackLimit && p + 8 <= (uintptr_t)tib->StackBase;
}

const char* Who(const void* e, const void* pup, const void* pl)
{
    if (!e) return "null";
    if (pup && e == pup) return "PUPPET";
    if (pl && e == pl) return "PLAYER";
    return "npc";
}

void __cdecl OnCall(int which, void* fm, const void* a2, const void* a3, unsigned a4, uintptr_t ret, uintptr_t ebp)
{
    __try {
        InterlockedIncrement(which ? &g_nAttack : &g_nStart);
        static ULONGLONG s_next = 0;
        const ULONGLONG now = GetTickCount64();
        if (now >= s_next) {
            s_next = now + 10000;
            char s[96];
            _snprintf_s(s, sizeof(s), _TRUNCATE, "[CF] counts start=%ld attack=%ld build=%ld drive=%ld drivePlayer=%ld drivePuppet=%ld selPlayer=%ld selPuppet=%ld\n",
                        g_nStart, g_nAttack, g_nBuild, g_nDrive, g_nDrivePl, g_nDrivePup, g_nSelPl, g_nSelPup);
            Phase1_LogLine(s);
        }
        if (g_lines >= 400) return;
        void* pup = Phase1_PuppetEntity();
        void* pl = nullptr;
        World* w = World::GetCurrentWorld();
        if (w) pl = w->GetPlayer();
        const bool hit = (pup && (a2 == pup || a3 == pup)) || (pl && (a2 == pl || a3 == pl));
        if (!hit) return;
        uintptr_t r1 = 0, r2 = 0;
        if (InStack(ebp)) { r1 = *(const uintptr_t*)(ebp + 4); const uintptr_t n = *(const uintptr_t*)ebp;
            if (n > ebp && InStack(n)) r2 = *(const uintptr_t*)(n + 4); }
        // the attacker's round, as sub_1625330 tests it: *(a2+0xE8)+0x98
        const void* aic = a2 ? *(const void* const*)((const char*)a2 + 0xE8) : nullptr;
        const void* rnd = aic ? *(const void* const*)((const char*)aic + 0x98) : nullptr;
        // attacker -> target distance (Entity::m_GlobalMatrix translation, Scimitar/Entity.h)
        float d = -1.f;
        if (a2 && a3) {
            const Entity* ea = (const Entity*)a2; const Entity* eb = (const Entity*)a3;
            const float dx = ea->m_GlobalMatrix.M41 - eb->m_GlobalMatrix.M41, dy = ea->m_GlobalMatrix.M42 - eb->m_GlobalMatrix.M42,
                        dz = ea->m_GlobalMatrix.M43 - eb->m_GlobalMatrix.M43;
            d = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        char line[240];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CF] t=%llu %s a2=%s %p a3=%s %p a4=%u a2round=%p dist=%.2f fm=%p from=%08X <%08X <%08X\n",
                    now, which ? "attack" : "start", Who(a2, pup, pl), a2, Who(a3, pup, pl), a3, a4 & 0xFF, rnd, d, fm,
                    (unsigned)ret, (unsigned)r1, (unsigned)r2);
        InterlockedIncrement(&g_lines);
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// sub_1342F70 BuildFightBehaviour, cdecl(a1 = &attacker handle holder, a2 = &target handle holder, a3, char a4, a5)
// (decompiled 2026-11-17, job v5). Entity from a holder: h = *a; e = (h[2] >> 31) & h[0]. It returns null without
// starting anything if either entity or its +0x5C is null, if sub_133FAA0(target) (target AI vt+0x34 returns true), or
// if a4 && sub_1711500 (same / allied faction). With a4 it calls sub_1625330 (0x01343003), else sub_1620140.
// sub_133FAA0 (job v6): ai = *(*(e+0xE8)+0x1C); refuse when ai->vt[0x34](ai). Slot 0x34 of AI vtable 0x026D3CD8
// = sub_753330 (job v7/v8): (*(ai+0x98) && **(byte**)(ai+0x98)) || *(byte*)(ai+0x8C). Read here without calling it.
const uintptr_t VA_BUILD = 0x01342F70;
const BYTE kProBuild[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };   // push ebp / mov ebp,esp / mov eax,[ebp+8]
const uintptr_t VA_AI_REFUSE_SLOT = 0x00753330;
DWORD g_trBuild = 0;
volatile LONG g_bLines = 0;

const void* EntFromHolder(const void* holder)
{
    if (!holder) return nullptr;
    const int* h = *(const int* const*)holder;
    return h ? (const void*)(uintptr_t)((h[2] >> 31) & h[0]) : nullptr;
}

// Writes "vt=.. slot=.. b8C=.. p98=.. b98=.. refuse=.." for an entity's AI into buf.
void AiRefuse(const void* e, char* buf, size_t n)
{
    const char* aic = e ? *(const char* const*)((const char*)e + 0xE8) : nullptr;
    const char* ai = aic ? *(const char* const*)(aic + 0x1C) : nullptr;
    if (!ai) { _snprintf_s(buf, n, _TRUNCATE, "ai=0"); return; }
    const uintptr_t vt = *(const uintptr_t*)ai;
    const uintptr_t slot = vt ? *(const uintptr_t*)(vt + 0x34) : 0;
    const unsigned b8c = *(const unsigned char*)(ai + 0x8C);
    const unsigned char* p98 = *(const unsigned char* const*)(ai + 0x98);
    const unsigned b98 = p98 ? *p98 : 0;
    const char* rf = (slot == VA_AI_REFUSE_SLOT) ? (((p98 && b98) || b8c) ? "1" : "0") : "?";
    _snprintf_s(buf, n, _TRUNCATE, "ai=%p vt=%08X slot=%08X b8C=%u p98=%p b98=%u refuse=%s",
                ai, (unsigned)vt, (unsigned)slot, b8c, p98, b98, rf);
}

void __cdecl OnBuild(const void* h1, const void* h2, unsigned a4, uintptr_t ret)
{
    __try {
        InterlockedIncrement(&g_nBuild);
        if (g_bLines >= 300) return;
        const void* att = EntFromHolder(h1);
        const void* tgt = EntFromHolder(h2);
        void* pup = Phase1_PuppetEntity();
        void* pl = nullptr;
        World* w = World::GetCurrentWorld();
        if (w) pl = w->GetPlayer();
        const bool hit = (pup && (att == pup || tgt == pup)) || (pl && (att == pl || tgt == pl));
        if (!hit) return;
        char ar[160];
        AiRefuse(tgt, ar, sizeof(ar));
        const void* tw = tgt ? *(const void* const*)((const char*)tgt + 0x5C) : nullptr;
        char line[300];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[CF] build t=%llu att=%s tgt=%s a4=%u tgtWorld=%p tgt %s from=%08X\n",
                    GetTickCount64(), Who(att, pup, pl), Who(tgt, pup, pl), a4 & 0xFF, tw, ar, (unsigned)ret);
        InterlockedIncrement(&g_bLines);
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// cdecl entry, prologue not run: [esp] = ret, [esp+4] = a1, [esp+8] = a2, [esp+0x10] = a4.
__declspec(naked) void DetourBuild()
{
    __asm {
        pushad
        mov  eax, [esp + 32]        // ret  (orig esp+0)
        push eax
        mov  eax, [esp + 52]        // a4   (orig esp+0x10)
        push eax
        mov  eax, [esp + 48]        // a2   (orig esp+8)
        push eax
        mov  eax, [esp + 48]        // a1   (orig esp+4)
        push eax
        call OnBuild
        add  esp, 16
        popad
        jmp  dword ptr [g_trBuild]
    }
}

// sub_15B37E0 ReactionDrive, thiscall(this), retn (job v11/v12). Reaction object *(this+0x88); target holder *(this+0x80),
// entity = decode(*(holder+8)); vt[0x20](react, *(this+0x70), holder) gates, sub_1587230 then calls vt[0x24] (on the
// player's path: vtable 0x026B1BF0 slot 9 = sub_1343D30 -> sub_1342F70 -> sub_1625330).
//   0x015B37E0  56 / 8B F1 / 8B 8E 88 00 00 00   push esi / mov esi,ecx / mov ecx,[esi+88h]   <- 9 bytes stolen
const uintptr_t VA_DRIVE = 0x015B37E0;
const BYTE kProDrive[9] = { 0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x88, 0x00, 0x00, 0x00 };
DWORD g_trDrive = 0;
struct DKey { uintptr_t vt; int who; unsigned sid; };
DKey g_seen[128]; volatile LONG g_nSeen = 0;

void __cdecl OnDrive(const char* self)
{
    __try {
        InterlockedIncrement(&g_nDrive);
        const char* react = *(const char* const*)(self + 0x88);
        if (!react) return;
        const char* holder = *(const char* const*)(self + 0x80);
        const void* tgt = holder ? EntFromHolder(holder + 8) : nullptr;
        void* pup = Phase1_PuppetEntity();
        void* pl = nullptr;
        World* w = World::GetCurrentWorld();
        if (w) pl = w->GetPlayer();
        const int who = (pup && tgt == pup) ? 2 : (pl && tgt == pl) ? 1 : tgt ? 0 : -1;
        if (who == 2) InterlockedIncrement(&g_nDrivePup);
        if (who == 1) InterlockedIncrement(&g_nDrivePl);
        const uintptr_t vt = *(const uintptr_t*)react;
        // holder = the stimulus clone installed by sub_15AE100 (mgr+0x80 = stim->vt[0x20]()); its id at +0x10.
        // UNVERIFIED that the clone keeps the id at +0x10: the clone's vtable is logged so a mismatch shows.
        const unsigned sid = holder ? *(const unsigned*)(holder + 0x10) : 0xFFFFFFFF;
        const unsigned hvt = holder ? *(const unsigned*)holder : 0;
        const LONG n = g_nSeen;
        for (LONG i = 0; i < n; ++i) if (g_seen[i].vt == vt && g_seen[i].who == who && g_seen[i].sid == sid) return;
        if (n >= 128) return;
        g_seen[n].vt = vt; g_seen[n].who = who; g_seen[n].sid = sid; InterlockedIncrement(&g_nSeen);
        char line[200];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[CF] drive new t=%llu react vt=%08X stim id=%u (holderVt=%08X) tgt=%s %p self=%p\n",
                    GetTickCount64(), (unsigned)vt, sid, hvt, who == 2 ? "PUPPET" : who == 1 ? "PLAYER" : who == 0 ? "npc" : "null",
                    tgt, self);
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void DetourDrive()
{
    __asm {
        pushad
        push ecx
        call OnDrive
        add  esp, 4
        popad
        jmp  dword ptr [g_trDrive]
    }
}

// sub_15AE100 ReactionSelect, thiscall(mgr, stim, float), retn 8 (job v16/v17). Stimulus: source holder at stim+8,
// id at stim+0x10. Picks a reaction set (sub_1A61820, else the default *(sub_10A9F80()+92)) and installs
// mgr+0x80 = stim->vt[0x20]() (a stimulus clone: the reaction's target is the stimulus source), mgr+0x88 = reaction.
// The reacting body is the entity at mgr+0x70 (sub_15A1650 reads its +0x9C kind word).
//   0x015AE100  55 / 8B EC / 83 EC 08   push ebp / mov ebp,esp / sub esp,8   <- 6 bytes stolen
const uintptr_t VA_SELECT = 0x015AE100;
const BYTE kProSelect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
DWORD g_trSelect = 0;
struct SKey { unsigned id; int who; };
SKey g_sSeen[128]; volatile LONG g_nsSeen = 0;

void __cdecl OnSelect(const char* mgr, const char* stim)
{
    __try {
        if (!stim) return;
        const void* src = EntFromHolder(stim + 8);
        const unsigned id = *(const unsigned*)(stim + 0x10);
        void* pup = Phase1_PuppetEntity();
        void* pl = nullptr;
        World* w = World::GetCurrentWorld();
        if (w) pl = w->GetPlayer();
        const int who = (pup && src == pup) ? 2 : (pl && src == pl) ? 1 : src ? 0 : -1;
        if (who == 2) InterlockedIncrement(&g_nSelPup);
        if (who == 1) InterlockedIncrement(&g_nSelPl);
        {
            static ULONGLONG s_nextSel = 0;
            const ULONGLONG now = GetTickCount64();
            if (now >= s_nextSel) {
                s_nextSel = now + 10000;
                char s[96];
                _snprintf_s(s, sizeof(s), _TRUNCATE, "[CF] selcounts player=%ld puppet=%ld drivePuppet=%ld\n",
                            g_nSelPl, g_nSelPup, g_nDrivePup);
                Phase1_LogLine(s);
            }
        }
        if (who < 1) return;                         // only stimuli whose source is the player or the puppet
        const LONG n = g_nsSeen;
        for (LONG i = 0; i < n; ++i) if (g_sSeen[i].id == id && g_sSeen[i].who == who) return;
        if (n >= 128) return;
        g_sSeen[n].id = id; g_sSeen[n].who = who; InterlockedIncrement(&g_nsSeen);
        const void* guard = mgr ? *(const void* const*)(mgr + 0x70) : nullptr;
        const unsigned srcKind = *(const unsigned*)((const char*)src + 0x9C);
        const unsigned gKind = guard ? *(const unsigned*)((const char*)guard + 0x9C) : 0;
        char line[200];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CF] select new t=%llu stim id=%u src=%s kind9C=%08X (&7=%u >>3&3F=%u) guard=%p gKind=%08X stimVt=%08X\n",
                    GetTickCount64(), id, who == 2 ? "PUPPET" : "PLAYER", srcKind, srcKind & 7, (srcKind >> 3) & 0x3F,
                    guard, gKind, *(const unsigned*)stim);
        Phase1_LogLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void DetourSelect()
{
    __asm {
        pushad
        mov  eax, [esp + 36]        // stim (orig esp+4)
        push eax
        push ecx                    // mgr
        call OnSelect
        add  esp, 8
        popad
        jmp  dword ptr [g_trSelect]
    }
}

// sub_131EF30 DetectStart (job v11/v21): thiscall(this), no stack args, plain `retn` (0x0131F2B2). After it runs,
//   +0x64 = handle of the entity at *(*(this+0x20)+0x88) (decompile: v23 = this+32, *(v23+136)+200);
//   +0x74 = handle of sub_70E810(world of that entity), i.e. the world's player.
// Wrapped (original first, then read both handles): which body the detection was raised about, and which it targets.
//   0x0131EF30  55 / 8B EC / 83 EC 1C   push ebp / mov ebp,esp / sub esp,1Ch   <- 6 bytes stolen
const uintptr_t VA_DETECT = 0x0131EF30;
const BYTE kProDetect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
DWORD g_trDetect = 0;
volatile LONG g_dLines = 0;
volatile LONG g_dCount[3][3] = {};   // [about: npc/player/puppet][target: npc/player/puppet]

int __fastcall DetectWrap(char* self, void*)
{
    const int r = ((int(__thiscall*)(void*))g_trDetect)(self);
    __try {
        const void* about = EntFromHolder(self + 0x64);
        const void* tgt = EntFromHolder(self + 0x74);
        void* pup = Phase1_PuppetEntity();
        void* pl = nullptr;
        World* w = World::GetCurrentWorld();
        if (w) pl = w->GetPlayer();
        const int wa = (pup && about == pup) ? 2 : (pl && about == pl) ? 1 : 0;
        const int wt = (pup && tgt == pup) ? 2 : (pl && tgt == pl) ? 1 : 0;
        InterlockedIncrement(&g_dCount[wa][wt]);
        if (InterlockedIncrement(&g_dLines) <= 60) {
            const char* nm[3] = { "npc", "PLAYER", "PUPPET" };
            char line[200];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[CF] detect t=%llu about=%s %p target=%s %p self=%p | counts pl->pl=%ld pup->pl=%ld pup->pup=%ld\n",
                        GetTickCount64(), nm[wa], about, nm[wt], tgt, self, g_dCount[1][1], g_dCount[2][1], g_dCount[2][2]);
            Phase1_LogLine(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// Entry, prologue not run: ECX = fm, [esp] = ret, [esp+4] = a2, [esp+8] = a3, [esp+0xC] = a4. EBP = caller's frame.
// After pushad (32 bytes) the original [esp+k] is at [esp+32+k]; each push below moves it by 4 more.
__declspec(naked) void DetourStart()
{
    __asm {
        pushad
        push ebp                    // ebp
        mov  eax, [esp + 36]        // ret   (orig esp+0)
        push eax
        mov  eax, [esp + 52]        // a4    (orig esp+0xC)
        push eax
        mov  eax, [esp + 52]        // a3    (orig esp+8)
        push eax
        mov  eax, [esp + 52]        // a2    (orig esp+4)
        push eax
        push ecx                    // fm
        push 0                      // which = start
        call OnCall
        add  esp, 28
        popad
        jmp  dword ptr [g_trStart]
    }
}

__declspec(naked) void DetourAttack()
{
    __asm {
        pushad
        push ebp
        mov  eax, [esp + 36]
        push eax
        mov  eax, [esp + 52]
        push eax
        mov  eax, [esp + 52]
        push eax
        mov  eax, [esp + 52]
        push eax
        push ecx
        push 1                      // which = attack
        call OnCall
        add  esp, 28
        popad
        jmp  dword ptr [g_trAttack]
    }
}

DWORD MakeTramp(uintptr_t s, const BYTE* pro, int n)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, pro, n);
    t[n] = 0x68; *reinterpret_cast<DWORD*>(t + n + 1) = (DWORD)(s + n); t[n + 5] = 0xC3;   // push s+n; ret
    FlushInstructionCache(GetCurrentProcess(), t, n + 6);
    return (DWORD)(uintptr_t)t;
}

}  // namespace

void ConflictProbe::Init()
{
    if (!GetPrivateProfileIntA("Coop", "ConflictProbe", 0, Paths::Ini())) return;
    const uintptr_t s1 = GameModuleBase + (VA_START - 0x00400000);
    const uintptr_t s2 = GameModuleBase + (VA_ATTACK - 0x00400000);
    if (memcmp((const void*)s1, kProStart, 6) != 0 || memcmp((const void*)s2, kProAttack, 5) != 0) {
        Phase1_LogLine("[CF] prologue bytes mismatch - not installed\n");
        return;
    }
    g_trStart = MakeTramp(s1, kProStart, 6);
    g_trAttack = MakeTramp(s2, kProAttack, 5);
    if (!g_trStart || !g_trAttack) return;
    Hook::DetourFunc((BYTE*)s1, (DWORD)(uintptr_t)&DetourStart, 6);
    Hook::DetourFunc((BYTE*)s2, (DWORD)(uintptr_t)&DetourAttack, 5);
    const uintptr_t s3 = GameModuleBase + (VA_BUILD - 0x00400000);
    if (memcmp((const void*)s3, kProBuild, 6) == 0 && (g_trBuild = MakeTramp(s3, kProBuild, 6)) != 0)
        Hook::DetourFunc((BYTE*)s3, (DWORD)(uintptr_t)&DetourBuild, 6);
    else
        Phase1_LogLine("[CF] sub_1342F70 prologue mismatch - build probe not installed\n");
    const uintptr_t s4 = GameModuleBase + (VA_DRIVE - 0x00400000);
    if (memcmp((const void*)s4, kProDrive, 9) == 0 && (g_trDrive = MakeTramp(s4, kProDrive, 9)) != 0)
        Hook::DetourFunc((BYTE*)s4, (DWORD)(uintptr_t)&DetourDrive, 9);
    else
        Phase1_LogLine("[CF] sub_15B37E0 prologue mismatch - drive probe not installed\n");
    const uintptr_t s5 = GameModuleBase + (VA_SELECT - 0x00400000);
    if (memcmp((const void*)s5, kProSelect, 6) == 0 && (g_trSelect = MakeTramp(s5, kProSelect, 6)) != 0)
        Hook::DetourFunc((BYTE*)s5, (DWORD)(uintptr_t)&DetourSelect, 6);
    else
        Phase1_LogLine("[CF] sub_15AE100 prologue mismatch - select probe not installed\n");
    const uintptr_t s6 = GameModuleBase + (VA_DETECT - 0x00400000);
    if (memcmp((const void*)s6, kProDetect, 6) == 0 && (g_trDetect = MakeTramp(s6, kProDetect, 6)) != 0)
        Hook::DetourFunc((BYTE*)s6, (DWORD)(uintptr_t)&DetectWrap, 6);
    else
        Phase1_LogLine("[CF] sub_131EF30 prologue mismatch - detect probe not installed\n");
    Phase1_LogLine("[CF] on: sub_1625330 + sub_1620140 + sub_1342F70 + sub_15B37E0 + sub_15AE100 + sub_131EF30 probes\n");
}

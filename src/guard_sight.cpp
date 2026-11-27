// Guards perceive the co-op puppet (host only). Evidence: ops/evidence/guard-perception-of-puppet-2026-11-13.md
// (S3, sections 0, 3.2, 7.4, 7.5), AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35.
//
// Guard sight and detection never query "who is near": they call World::GetPlayer (0x70E810), which is always the
// local player, so the puppet is never a candidate. The three call sites that CHOOSE the target are redirected here;
// GetPlayer itself is untouched (603 code xrefs).
//
//   site       original bytes   what                                  observer at the call
//   0x103D61D  E8 EE 11 6D FF   CSLineOfSight player-request create   [edi+78h] (owner guard)
//   0x103D7A1  E8 6A 10 6D FF   CSLineOfSight retarget, every update  [esi+78h] (owner guard)
//   0x131EFD5  E8 36 F8 3E FF   CRLDetect start (target -> +0x74)     edx = guard Entity* (resolved inline above)
//
// GetPlayer is __thiscall (ecx = World*), no stack arguments, plain retn, returns Entity* in eax, clobbers eax ecx edx
// and flags, preserves ebx esi edi ebp (0x70E810: mov eax,[ecx+7F8h] / mov ecx,[eax+4] / jmp 0x736540). Each stub
// keeps that contract: it calls the real GetPlayer, then returns either the player or the puppet.
//
// Choice per guard (one table, so sensor, detection start and the escalation that carries CRLDetect+0x74 agree):
// decided at the per-update retarget site - the nearer of player / puppet, switching only when the other is 3 m
// nearer and the current choice is at least 2 s old; read by the other two sites (create: decide once if unknown).
// Not covered (S3 3.2 / 7.2 / 7.3): the target gate 0x101CAA0 for the puppet, the escalation receiver, and the
// helpers 0x6F6010 / 0x65A660 / 0x1B06D30 (not patched: they still read the local player).
// [Coop] GuardPerceivePuppet=1 replaces this choice rule (visibility-sticky G1, G3 keeps the stimulus target): see
// ChoosePerceive. With the flag at 0 the rule above runs unchanged.
#include "guard_sight.h"
#include "Hook.h"
#include "paths.h"
#include "Scimitar/Entity.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

void* Phase1_PuppetEntity();
bool  Phase1_IsEntityHost();
void  Phase1_LogLine(const char* s);

namespace {

struct Site { uintptr_t va; BYTE orig[5]; };
const Site kSites[3] = {
    { 0x0103D61D, { 0xE8, 0xEE, 0x11, 0x6D, 0xFF } },
    { 0x0103D7A1, { 0xE8, 0x6A, 0x10, 0x6D, 0xFF } },
    { 0x0131EFD5, { 0xE8, 0x36, 0xF8, 0x3E, 0xFF } },
};
bool g_installed = false;
int  g_on = 0; ULONGLONG g_nextMs = 0;

const int SLOTS = 256;
// seenMs: last Choose for obs. commit .. detMs are used only with [Coop] GuardPerceivePuppet=1 (ChoosePerceive):
// commit = body last seen (0 player, 1 puppet), visMs[body] = last tick its request had VISIBLE (bit 2),
// det = detection target at G3 (0 none, 1 player, 2 puppet), detMs = when G3 set it.
struct Choice { uintptr_t obs; int puppet; ULONGLONG sinceMs; ULONGLONG seenMs; int commit; ULONGLONG visMs[2]; int det; ULONGLONG detMs; };
Choice  g_tab[SLOTS];
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_calls[3] = {}, g_puppetPicks = 0, g_switches = 0;
int g_perc = 0;   // [Coop] GuardPerceivePuppet (default 0), read with GuardSeesPuppet

bool On(ULONGLONG nowMs)
{
    if (nowMs >= g_nextMs) {
        g_nextMs = nowMs + 2000;
        g_on = GetPrivateProfileIntA("Coop", "GuardSeesPuppet", 0, Paths::Ini()) != 0;
        g_perc = GetPrivateProfileIntA("Coop", "GuardPerceivePuppet", 0, Paths::Ini()) != 0;
    }
    return g_on != 0;
}

bool Pos(void* e, float* p)
{
    __try {
        const Entity* en = (const Entity*)e;
        p[0] = en->m_GlobalMatrix.M41; p[1] = en->m_GlobalMatrix.M42; p[2] = en->m_GlobalMatrix.M43;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float D2(const float* a, const float* b)
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}
void* World(void* e)
{
    __try { return *(void**)((char*)e + 0x5C); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }   // m_AssociatedWorld
}

// [Coop] SightProbe (default 0; live; read-only except the two faction getters, which S3 section 8.3 read as loads
// and getter calls only). S3 section 8: the reaction is created only after CRLDetectParams CanReact (0x1076110), so
// a puppet chosen as sight target but never detected stops at L (geometry / gate), S (stimulus) or R (faction /
// alert state). At the retarget site, when this sensor's previous target was the puppet, the request's flags are the
// result of that evaluation: req = *(los+0xF8), flags = u16 req+0x10: 0x001 cone, 0x002 VISIBLE (stimulus 8 sent),
// 0x004 blocked, 0x020 evaluated, 0x040 async result arrived, 0x100 in range, 0x200 player request.
// Puppet: kind/role = *(P+0x9C) &7 / >>3&0x3F; h = *(P+0xE8): byte h+0x28 (L2 gate), ai = *(h+0x1C) (L4).
// Faction (R3), only on visible frames: sub_133A4C0(e) entity -> faction (cdecl, 1 arg), sub_17114C0(G, T) "T is an
// enemy of G" (cdecl, 2 args) - both already have decomp units and are used in phase1.cpp.
int g_spOn = 0; ULONGLONG g_spNext = 0;
volatile LONG g_spN = 0, g_spCone = 0, g_spVis = 0, g_spBlk = 0, g_spEval = 0, g_spAsync = 0, g_spRange = 0, g_spPReq = 0,
              g_spL2 = 0, g_spNoAi = 0, g_spHostP = 0, g_spHostPl = 0, g_spSameFac = 0, g_spAsOff = 0;
struct SpSample { unsigned flags, kind, role, l2, ai, facP, facPl, as20; float dist; };
SpSample g_spLast = {};
void SightProbe(void* los, void* G, void* P, void* player)
{
    const ULONGLONG nowMs = GetTickCount64();
    if (nowMs >= g_spNext) { g_spNext = nowMs + 2000; g_spOn = GetPrivateProfileIntA("Coop", "SightProbe", 0, Paths::Ini()) != 0; }
    if (!g_spOn || !los) return;
    // Lock rule (ops/evidence/guard-perceive-puppet-design-2026-11-27.md, Addendum A3): 0x17114C0 always enters the
    // global handle-table CS [0x4DD5FA0+0x7C] (0xA0BE15), 0x133A4C0 enters it on the refcount-zero path (0x133A530).
    // A fault swallowed by __except while that CS is owned deadlocks every other thread, so those calls run below,
    // OUTSIDE any __try, only after every input was read here. Choose (the caller) holds no __try around this call.
    SpSample s = {};
    bool callFac = false;
    __try {
        const char* req = *(const char* const*)((const char*)los + 0xF8);
        if (!req) return;
        s.flags = *(const uint16_t*)(req + 0x10);
        InterlockedIncrement(&g_spN);
        if (s.flags & 0x001) InterlockedIncrement(&g_spCone);
        if (s.flags & 0x002) InterlockedIncrement(&g_spVis);
        if (s.flags & 0x004) InterlockedIncrement(&g_spBlk);
        if (s.flags & 0x020) InterlockedIncrement(&g_spEval);
        if (s.flags & 0x040) InterlockedIncrement(&g_spAsync);
        if (s.flags & 0x100) InterlockedIncrement(&g_spRange);
        if (s.flags & 0x200) InterlockedIncrement(&g_spPReq);
        const uint32_t k = *(const uint32_t*)((const char*)P + 0x9C);
        s.kind = k & 7; s.role = (k >> 3) & 0x3F;
        const char* h = *(const char* const*)((const char*)P + 0xE8);
        s.l2 = h ? *(const uint8_t*)(h + 0x28) : 0xFF;
        s.ai = h ? (unsigned)(uintptr_t)*(void* const*)(h + 0x1C) : 0;
        if (s.l2 && s.l2 != 0xFF) InterlockedIncrement(&g_spL2);
        if (!s.ai) InterlockedIncrement(&g_spNoAi);
        const char* w = *(const char* const*)((const char*)G + 0x5C);
        const char* as = w ? *(const char* const*)(w + 0x8E0) : nullptr;
        s.as20 = as ? *(const uint8_t*)(as + 0x20) : 0xFF;
        if (!as || !s.as20) InterlockedIncrement(&g_spAsOff);
        float pg[3], pp[3];
        if (Pos(G, pg) && Pos(P, pp)) s.dist = sqrtf(D2(pg, pp));
        if (s.flags & 0x002) {
            // Validate the getter inputs here (inside __try): the same fields already read from P, read from the
            // player too; G was read above (+0x5C, Pos). A fault here leaves the getters uncalled.
            if (!player) return;
            volatile uint32_t k2 = *(const uint32_t*)((const char*)player + 0x9C); (void)k2;
            volatile const void* h2 = *(const void* const*)((const char*)player + 0xE8); (void)h2;
            volatile const void* w2 = *(const void* const*)((const char*)player + 0x5C); (void)w2;
            callFac = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (callFac) {   // engine calls that may take the global CS: no __try on this path (see above)
        typedef void* (__cdecl* FacFn)(void*);
        typedef BOOL (__cdecl* HostFn)(void*, void*);
        s.facP  = (unsigned)(uintptr_t)((FacFn)0x0133A4C0)(P);
        s.facPl = (unsigned)(uintptr_t)((FacFn)0x0133A4C0)(player);
        if (s.facP && s.facP == s.facPl) InterlockedIncrement(&g_spSameFac);
        if (((HostFn)0x017114C0)(G, P)) InterlockedIncrement(&g_spHostP);
        if (((HostFn)0x017114C0)(G, player)) InterlockedIncrement(&g_spHostPl);
    }
    g_spLast = s;
}

// ---- [Coop] GuardPerceivePuppet (default 0; needs GuardSeesPuppet; read live with it every 2 s) --------------------
// Design: ops/evidence/guard-perceive-puppet-design-2026-11-27.md section 3.1 (faults F1, F3, F4 of the rules above).
// Only the decision changes; sites, stubs and byte checks are the ones above. No engine call on this path (SightProbe's
// faction getters are not called with the flag on): GetTickCount64, g_lock and field reads under __try only.
//   G1 retarget (mode 1): req = *(los+0xF8); u16 req+0x10 bit 2 = VISIBLE, the result for the body the previous call
//      returned (S3 A8.3). Visible -> keep it. Not visible -> hold the body last seen until T_LOSE without seeing it,
//      then alternate the two bodies, each staying in the slot at least T_PROBE.
//   G1 create (mode 0): unchanged rule (the nearer body, decided once).
//   G3 detection start (mode 2): esi = CRLDetect; +0x74 already holds the stimulus target (CreateReaction 0x1055D12).
//      Return that body when it is the player or the puppet; otherwise the real GetPlayer (SP). Recorded as det.
//   G4 (guard_poi.cpp PoiFor): DetectsPuppet() - lend the puppet only while this guard's detection target is the puppet.
// Not implemented: design 3.1 (b) "the other body is within the guard's sight range" needs sub_18779F0 (an engine call,
// lock behaviour unread) - the alternation probes the other body instead.
const ULONGLONG T_LOSE = 1500, T_PROBE = 500, T_DET = 3000;
volatile LONG g_pcG1 = 0, g_pcPickPup = 0, g_pcPickPl = 0, g_pcKeepVis = 0, g_pcHold = 0, g_pcProbe = 0,
              g_pcG3 = 0, g_pcTgtPup = 0, g_pcTgtPl = 0, g_pcTgtOther = 0, g_pcLend = 0, g_pcEv1 = 0, g_pcEv3 = 0;

// caller holds g_lock exclusive. A slot is free when unused or not seen by any gate for 60 s.
int FindSlotP(uintptr_t obs, ULONGLONG nowMs, bool create)
{
    int freeSlot = -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (g_tab[i].obs == obs) return i;
        if (freeSlot < 0 && (g_tab[i].obs == 0 || nowMs - g_tab[i].seenMs > 60000)) freeSlot = i;
    }
    if (!create || freeSlot < 0) return -1;
    g_tab[freeSlot] = Choice{};
    g_tab[freeSlot].obs = obs; g_tab[freeSlot].sinceMs = nowMs; g_tab[freeSlot].seenMs = nowMs;
    return freeSlot;
}

void* ChoosePerceive(void* obs, void* player, void* pup, int mode, void* los, ULONGLONG nowMs)
{
    char line[160]; line[0] = 0;
    if (mode == 2) {
        InterlockedIncrement(&g_pcG3);
        const void* e = nullptr;
        if (los) {
            __try {
                const int* h = *(const int* const*)((const char*)los + 0x74);
                e = h ? (const void*)(uintptr_t)((h[2] >> 31) & h[0]) : nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) { e = nullptr; }
        }
        // +0x74 = puppet -> puppet; = player -> player; anything else -> the real GetPlayer, so the detection then
        // targets the player and is recorded as such (no lend).
        const int det = e == pup ? 2 : 1;
        InterlockedIncrement(e == pup ? &g_pcTgtPup : e == player ? &g_pcTgtPl : &g_pcTgtOther);
        AcquireSRWLockExclusive(&g_lock);
        const int s = FindSlotP((uintptr_t)obs, nowMs, true);
        if (s >= 0) { g_tab[s].det = det; g_tab[s].detMs = nowMs; g_tab[s].seenMs = nowMs; }
        ReleaseSRWLockExclusive(&g_lock);
        if (InterlockedIncrement(&g_pcEv3) <= 20)
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[Perceive] gate=3 guard=%p target=%s src=%s\n", obs,
                        det == 2 ? "puppet" : "player", (e == pup || e == player) ? "stim74" : "getplayer");
        if (line[0]) Phase1_LogLine(line);
        if (det == 2) { InterlockedIncrement(&g_puppetPicks); return pup; }
        return player;
    }

    InterlockedIncrement(&g_pcG1);
    unsigned flags = 0;
    if (mode == 1 && los) {
        __try {
            const char* req = *(const char* const*)((const char*)los + 0xF8);
            flags = req ? *(const uint16_t*)(req + 0x10) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { flags = 0; }
    }
    float po[3], pp[3], pq[3];
    const bool havePos = Pos(obs, po) && Pos(player, pp) && Pos(pup, pq);
    AcquireSRWLockExclusive(&g_lock);
    int s = FindSlotP((uintptr_t)obs, nowMs, false);
    if (s < 0) {   // first sight of this guard: the create rule (nearer body)
        if (!havePos) { ReleaseSRWLockExclusive(&g_lock); InterlockedIncrement(&g_pcPickPl); return player; }
        s = FindSlotP((uintptr_t)obs, nowMs, true);
        const int first = D2(po, pq) < D2(po, pp) ? 1 : 0;
        if (s >= 0) { g_tab[s].puppet = first; g_tab[s].commit = first; }
        else {
            ReleaseSRWLockExclusive(&g_lock);
            InterlockedIncrement(first ? &g_pcPickPup : &g_pcPickPl);
            if (first) { InterlockedIncrement(&g_puppetPicks); return pup; }
            return player;
        }
    }
    Choice& c = g_tab[s];
    const int prev = c.puppet ? 1 : 0;
    int pick = prev;
    if (mode == 1) {
        if (flags & 0x002) {   // the body in the slot is visible: keep it
            const bool reacquired = c.visMs[prev] == 0 || nowMs - c.visMs[prev] >= T_LOSE;
            c.visMs[prev] = nowMs;
            c.commit = prev;
            InterlockedIncrement(&g_pcKeepVis);
            if (reacquired && InterlockedIncrement(&g_pcEv1) <= 40)
                _snprintf_s(line, sizeof(line), _TRUNCATE, "[Perceive] gate=1 guard=%p vis=%s after=%llums\n", obs,
                            prev ? "pup" : "pl", (unsigned long long)(nowMs - c.sinceMs));
        } else if (c.visMs[c.commit ? 1 : 0] && nowMs - c.visMs[c.commit ? 1 : 0] < T_LOSE) {
            pick = c.commit ? 1 : 0;   // seen less than T_LOSE ago: hold it
            InterlockedIncrement(&g_pcHold);
        } else if (nowMs - c.sinceMs >= T_PROBE) {
            pick = prev ^ 1;           // lost: alternate, each body at least T_PROBE in the slot
            InterlockedIncrement(&g_pcProbe);
        }
        if (pick != prev) { c.puppet = pick; c.sinceMs = nowMs; InterlockedIncrement(&g_switches); }
    }
    c.seenMs = nowMs;
    ReleaseSRWLockExclusive(&g_lock);
    if (line[0]) Phase1_LogLine(line);
    InterlockedIncrement(pick ? &g_pcPickPup : &g_pcPickPl);
    if (pick) { InterlockedIncrement(&g_puppetPicks); return pup; }
    return player;
}

int g_dktOn = 0; ULONGLONG g_dktNext = 0; volatile LONG g_dktKept = 0;
// mode 0 = request create, 1 = per-update retarget (decides), 2 = detection start (reads)
void* __cdecl Choose(void* obs, void* player, int mode, void* los)
{
    InterlockedIncrement(&g_calls[mode]);
    const ULONGLONG nowMs = GetTickCount64();
    if (!On(nowMs) || !Phase1_IsEntityHost() || !obs || !player) return player;
    void* pup = Phase1_PuppetEntity();
    if (!pup || pup == obs || World(pup) != World(player)) return player;
    if (g_perc) return ChoosePerceive(obs, player, pup, mode, los, nowMs);   // [Coop] GuardPerceivePuppet
    // [Coop] DetectKeepTarget (live, default 0). At 0x131EFD5 `los` is the CRLDetect (esi). CreateReaction
    // (0x1055CB0) set CRLDetect+0x74 to the stimulus's handle (S3 evidence section 8); the start function then
    // overwrites +0x74 with this call's result. When the reaction was created FOR the puppet, keep the puppet, so
    // the guard detects the body that triggered it. Handle -> entity: (*(h+8) >> 31) & *h (engine pattern, e.g.
    // sub_13F22E0, sub_1340E60). Run 13532 (15ee79a): 7 CanReact passes for the puppet (stimulus 95), each matched
    // by a detection start, but the guards stayed on alert only.
    if (mode == 2 && los) {
        if (nowMs >= g_dktNext) { g_dktNext = nowMs + 2000; g_dktOn = GetPrivateProfileIntA("Coop", "DetectKeepTarget", 0, Paths::Ini()) != 0; }
        if (g_dktOn) {
            __try {
                const int* h = *(const int* const*)((const char*)los + 0x74);
                const void* e = h ? (const void*)(uintptr_t)((h[2] >> 31) & h[0]) : nullptr;
                if (e == pup) { InterlockedIncrement(&g_dktKept); InterlockedIncrement(&g_puppetPicks); return pup; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    float po[3], pp[3], pq[3];
    if (!Pos(obs, po) || !Pos(player, pp) || !Pos(pup, pq)) return player;

    AcquireSRWLockExclusive(&g_lock);
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (g_tab[i].obs == (uintptr_t)obs) { slot = i; break; }
        if (freeSlot < 0 && (g_tab[i].obs == 0 || nowMs - g_tab[i].sinceMs > 60000)) freeSlot = i;
    }
    int pick = 0;
    if (slot >= 0) pick = g_tab[slot].puppet;
    if (mode == 1 && pick) {                  // previous target was the puppet: its evaluation result is in req
        ReleaseSRWLockExclusive(&g_lock);
        SightProbe(los, obs, pup, player);
        AcquireSRWLockExclusive(&g_lock);
    }
    if (mode != 2) {
        const float dp = D2(po, pp), dq = D2(po, pq);
        const bool puppetNearer = dq < dp;
        if (slot < 0) {
            pick = puppetNearer ? 1 : 0;
            slot = freeSlot;
            if (slot >= 0) { g_tab[slot].obs = (uintptr_t)obs; g_tab[slot].puppet = pick; g_tab[slot].sinceMs = nowMs; }
        } else if (mode == 1 && nowMs - g_tab[slot].sinceMs >= 2000) {
            // switch only when the other body is at least 3 m nearer (compare distances, not squares)
            const float sp = sqrtf(dp), sq = sqrtf(dq);
            const int want = pick ? (sp + 3.0f < sq ? 0 : 1) : (sq + 3.0f < sp ? 1 : 0);
            if (want != pick) { pick = want; g_tab[slot].puppet = pick; g_tab[slot].sinceMs = nowMs; InterlockedIncrement(&g_switches); }
        }
    }
    if (slot >= 0 && g_tab[slot].obs == (uintptr_t)obs) g_tab[slot].seenMs = nowMs;
    ReleaseSRWLockExclusive(&g_lock);
    if (pick) { InterlockedIncrement(&g_puppetPicks); return pup; }
    return player;
}

// 0x103D61D: ecx = World*, observer [edi+78h]. [esp+4] belongs to the caller (its pushed ebx): not touched.
__declspec(naked) void StubCreate()
{
    __asm {
        mov  eax, 0x0070E810
        call eax                    // eax = local player (real GetPlayer, ecx = World*)
        push 0                      // los (not probed here)
        push 0
        push eax
        push dword ptr [edi + 78h]
        call Choose
        add  esp, 16
        ret
    }
}
// 0x103D7A1: ecx = World*, observer [esi+78h]
__declspec(naked) void StubRetarget()
{
    __asm {
        mov  eax, 0x0070E810
        call eax
        push esi                    // los = CSLineOfSight (SightProbe)
        push 1
        push eax
        push dword ptr [esi + 78h]
        call Choose
        add  esp, 16
        ret
    }
}
// 0x131EFD5: ecx = World*, edx = guard Entity* (GetPlayer clobbers edx: saved first)
__declspec(naked) void StubDetect()
{
    __asm {
        push edx
        mov  eax, 0x0070E810
        call eax
        pop  edx
        push esi                    // the CRLDetect (DetectKeepTarget reads its +0x74)
        push 2
        push eax
        push edx
        call Choose
        add  esp, 16
        ret
    }
}

bool PatchCall(uintptr_t site, const BYTE* orig, void* stub)
{
    BYTE* p = (BYTE*)(GameModuleBase + (site - 0x00400000));
    if (memcmp(p, orig, 5) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = (int32_t)((uintptr_t)stub - ((uintptr_t)p + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void GuardSight::Init()
{
    if (GetPrivateProfileIntA("Coop", "GuardSeesPuppet", -1, Paths::Ini()) < 0) return;
    // all three or none: verify every site before patching any
    for (const Site& s : kSites)
        if (memcmp((const void*)(GameModuleBase + (s.va - 0x00400000)), s.orig, 5) != 0) {
            OutputDebugStringA("[Sight] call-site bytes differ - not installed\n");
            return;
        }
    void* stubs[3] = { (void*)&StubCreate, (void*)&StubRetarget, (void*)&StubDetect };
    for (int i = 0; i < 3; ++i) PatchCall(kSites[i].va, kSites[i].orig, stubs[i]);
    g_installed = true;
    OutputDebugStringA("[Sight] installed at 0x103D61D, 0x103D7A1, 0x131EFD5\n");
}

// For guard_poi.cpp ([Coop] GuardPlayerOfInterest): true when this guard's sight sensor currently tracks the
// puppet (its entry picked the puppet and was refreshed by a Choose call within the last 3 s).
bool GuardSight::TracksPuppet(const void* guard)
{
    if (!g_installed || !guard) return false;
    const ULONGLONG nowMs = GetTickCount64();
    bool r = false;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < SLOTS; ++i)
        if (g_tab[i].obs == (uintptr_t)guard) { r = g_tab[i].puppet && nowMs - g_tab[i].seenMs < 3000; break; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

bool GuardSight::PerceiveOn() { return g_installed && g_on && g_perc; }

// G4 with GuardPerceivePuppet=1: this guard's detection target (G3) is the puppet and was set less than T_DET ago.
bool GuardSight::DetectsPuppet(const void* guard)
{
    if (!PerceiveOn() || !guard) return false;
    const ULONGLONG nowMs = GetTickCount64();
    bool r = false;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < SLOTS; ++i)
        if (g_tab[i].obs == (uintptr_t)guard) { r = g_tab[i].det == 2 && nowMs - g_tab[i].detMs < T_DET; break; }
    ReleaseSRWLockShared(&g_lock);
    if (r) InterlockedIncrement(&g_pcLend);
    return r;
}

// Guest lane ([Coop] GuardActionLane): false with the flag off (nothing appended). stage 2 = detection start less than
// T_DET ago (target = det), 1 = the committed sight body was visible less than T_LOSE ago, 0 = none.
bool GuardSight::PerceiveFor(const void* guard, unsigned char* stage, unsigned char* targetIsPuppet)
{
    if (!PerceiveOn() || !guard || !stage || !targetIsPuppet) return false;
    *stage = 0; *targetIsPuppet = 0;
    const ULONGLONG nowMs = GetTickCount64();
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < SLOTS; ++i) {
        if (g_tab[i].obs != (uintptr_t)guard) continue;
        const Choice& c = g_tab[i];
        const int b = c.commit ? 1 : 0;
        if (c.det && nowMs - c.detMs < T_DET) { *stage = 2; *targetIsPuppet = c.det == 2 ? 1 : 0; }
        else if (c.visMs[b] && nowMs - c.visMs[b] < T_LOSE) { *stage = 1; *targetIsPuppet = (unsigned char)b; }
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    return true;
}

int GuardSight::FormatPerceiveReport(char* out, int cap)
{
    if (!g_installed || !g_perc || cap < 64) return 0;
    const int n = _snprintf_s(out, cap, _TRUNCATE,
        "[Perceive] on=%d g1 calls=%ld pickPup=%ld pickPl=%ld keepVis=%ld hold=%ld probeSwitch=%ld | g3 calls=%ld tgt pup=%ld pl=%ld other=%ld | g4 lend=%ld",
        g_on && g_perc ? 1 : 0, InterlockedExchange(&g_pcG1, 0), InterlockedExchange(&g_pcPickPup, 0),
        InterlockedExchange(&g_pcPickPl, 0), InterlockedExchange(&g_pcKeepVis, 0), InterlockedExchange(&g_pcHold, 0),
        InterlockedExchange(&g_pcProbe, 0), InterlockedExchange(&g_pcG3, 0), InterlockedExchange(&g_pcTgtPup, 0),
        InterlockedExchange(&g_pcTgtPl, 0), InterlockedExchange(&g_pcTgtOther, 0), InterlockedExchange(&g_pcLend, 0));
    return n > 0 ? n : 0;
}

int GuardSight::FormatReport(char* out, int cap)
{
    if (!g_installed || cap < 64) return 0;
    int n = _snprintf_s(out, cap, _TRUNCATE, "[Sight] on=%d host=%d puppet=%p | calls create=%ld retarget=%ld detect=%ld keptPuppet=%ld | puppetPicks=%ld switches=%ld",
                       g_on, Phase1_IsEntityHost() ? 1 : 0, Phase1_PuppetEntity(),
                       InterlockedExchange(&g_calls[0], 0), InterlockedExchange(&g_calls[1], 0),
                       InterlockedExchange(&g_calls[2], 0), InterlockedExchange(&g_dktKept, 0),
                       InterlockedExchange(&g_puppetPicks, 0), InterlockedExchange(&g_switches, 0));
    if (g_spOn && n > 0 && n < cap - 200) {
        const SpSample s = g_spLast;
        n += _snprintf_s(out + n, cap - n, _TRUNCATE,
            " | probe n=%ld cone=%ld VIS=%ld blocked=%ld eval=%ld async=%ld range=%ld playerReq=%ld L2=%ld noAi=%ld asOff=%ld"
            " | onVis: sameFac=%ld hostile(G,P)=%ld hostile(G,player)=%ld | last flags=%03X kind=%u role=%u l2=%u ai=%08X as20=%u d=%.1fm facP=%08X facPl=%08X",
            InterlockedExchange(&g_spN, 0), InterlockedExchange(&g_spCone, 0), InterlockedExchange(&g_spVis, 0),
            InterlockedExchange(&g_spBlk, 0), InterlockedExchange(&g_spEval, 0), InterlockedExchange(&g_spAsync, 0),
            InterlockedExchange(&g_spRange, 0), InterlockedExchange(&g_spPReq, 0), InterlockedExchange(&g_spL2, 0),
            InterlockedExchange(&g_spNoAi, 0), InterlockedExchange(&g_spAsOff, 0), InterlockedExchange(&g_spSameFac, 0),
            InterlockedExchange(&g_spHostP, 0), InterlockedExchange(&g_spHostPl, 0),
            s.flags, s.kind, s.role, s.l2, s.ai, s.as20, s.dist, s.facP, s.facPl);
    }
    return n > 0 ? n : 0;
}

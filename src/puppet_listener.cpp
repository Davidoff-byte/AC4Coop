// [Coop] PuppetListener (default 0): is the replica reachable by the damage path?
//
// Static basis: docs/design/f7-puppet-damage-mechanism.md and docs/evidence/player-damage-path-2026-11-16.md.
// The event post resolves a listener component; the player damage branch resolves a *different* component and then
// its health object:
//    0x005220E0 -> 0x005210A0 (type off_279BF68 "EventListenerComponent") -> listener + 0xC   (or 0 = dropped)
//    0x005220D0 -> 0x00521010 (type off_2752DE0 "CharacterBehavior")      -> comp + 0x1C     (the health object)
// On the health object: +0x5C is int16 life (0x019FB4C0 writes *(this+46)), +0xD4 is its flags word
// (0x115ACC0 clear/early-out bits 0 and 3). Reading life off the ENTITY is wrong and yields garbage - the first
// version of this probe did that.
//
// [Coop] PuppetListenerAttach (default 0, needs PuppetListener): attach the listener component (0x6F7180, a2=0
// allocates). Cosmetic only until R6 (docs/design/guard-fight-sync.md).
//
// Poll thread: every engine call is inside __try. Nothing writes unless Attach=1 and the listener is missing.
#include "puppet_listener.h"
#include "paths.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdio>

void Phase1_LogLine(const char* s);
void* Phase1_PuppetEntity();

namespace {

const uintptr_t VA_LISTENER = 0x005220E0;   // -> listener+0xC or 0
const uintptr_t VA_HEALTH   = 0x005220D0;   // -> CharacterBehavior component or 0
const uintptr_t VA_ATTACH   = 0x006F7180;   // add the listener component
const uintptr_t VA_FACTION  = 0x0133A4C0;   // ResolveFaction
const uintptr_t VA_AITYPE   = 0x00F916A0;   // gate: does this AI carry a faction at +0xA4

typedef int  (__thiscall *ResolveFn)(void* self);
typedef int  (__thiscall *AttachFn)(void* self, int existing, int flags);
typedef int  (__cdecl *FacFn)(void* ent);
typedef int  (__thiscall *AiTypeFn)(void* ai);

// The mod's own gate (phase1.cpp HostHpSvcValid): a real service carries the CSrvNPCHealth vtable and a plausible
// Max at +0x5E. DECOMP-AUDIT puts the health services in the AI's +0x70 array (20 entries, "AI-side only").
const uint32_t CSRV_NPCHEALTH_VT = 0x02712F60;
const uint32_t CSRV_PLAYERHEALTH_VT = 0x0269BBF8;   // ops/evidence/player-damage-path: players carry their own class
const int      AI_HP_SLOTS = 20;

void* FindHealth(void* ai)
{
    if (!ai) return 0;
    void* arr = 0;
    void* rec98 = 0;
    unsigned int n = 0;
    __try {
        arr = *(void**)((char*)ai + 0x70);
        n = *(const unsigned short*)((char*)ai + 0x76);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!arr || n == 0 || n > 64) return 0;
    for (unsigned int k = 0; k < n; ++k) {
        void* svc = *(void**)((char*)arr + 4 * k);
        if (!svc) continue;
        __try {
            { const uint32_t vt = *(const uint32_t*)svc; if (vt != CSRV_NPCHEALTH_VT && vt != CSRV_PLAYERHEALTH_VT) continue; }
            const unsigned short mx = *(const unsigned short*)((const char*)svc + 0x5E);
            if (mx < 1 || mx > 2000) continue;
            return svc;
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    }
    return 0;
}

// Team lookup, re-implemented read-only (no engine call): sub_15E50F0(fm, e) (0x015E50F0, decompiled 2026-11-17) scans
// the u32 array at fm+0x30C (count u16 fm+0x312) for the first entry whose +0x18 equals ResolveFaction(e). The conflict
// starter sub_1625330 (0x01625330) proceeds only when both sides' team is non-null AND team+0x18 != 0; teams are created
// by sub_1622990 with +0x18 = ResolveFaction of the first member (sub_156D040, this = team). fm = *(world+0x8DC).
// Returns the matching team (or 0) and writes the list count to *n.
const void* TeamFor(const void* world, unsigned fac, unsigned* n)
{
    *n = 0;
    if (!world) return 0;
    const char* fm = *(const char* const*)((const char*)world + 0x8DC);
    if (!fm) return 0;
    const unsigned cnt = *(const unsigned short*)(fm + 0x312);
    const char* const* arr = *(const char* const* const*)(fm + 0x30C);
    *n = cnt;
    if (!arr || cnt == 0 || cnt > 256) return 0;
    for (unsigned k = 0; k < cnt; ++k) {
        const char* t = arr[k];
        if (t && *(const unsigned*)(t + 0x18) == fac) return t;
    }
    return 0;
}

volatile LONG g_run = 0;
volatile LONG g_attached = 0;
volatile LONG g_lines = 0;
bool g_attach = false;
bool g_maxlife = false;

void Poll()
{
    void* pup = Phase1_PuppetEntity();
    if (!pup) return;

    // entity+0x5C is the WORLD pointer (status reports the same value), not health. The health service is reached
    // through the AI: e+0xE8 -> AIComponent, +0x1C -> AbstractEntityAI (STATE.json human_chain). Log that chain and
    // the AI's own health-ish words so player and replica can be compared field by field.
    int listener = -1;
    void* hptr = 0;
    void* ai = 0;
    void* svc = 0;
    void* arr = 0;
    void* rec98 = 0;
    unsigned cnt = 0;
    unsigned team = 0, fac = 0, tgate = 0;
    int life = -1, flags = -1;
    __try {
        listener = ((ResolveFn)VA_LISTENER)(pup);
        hptr = *(void**)((char*)pup + 0x5C);              // the world, per status
        void* aic = *(void**)((char*)pup + 0xE8);
        ai = aic ? *(void**)((char*)aic + 0x1C) : 0;
        arr = ai ? *(void**)((char*)ai + 0x70) : 0;
        cnt = ai ? *(const unsigned short*)((char*)ai + 0x76) : 0;
        rec98 = ai ? *(void**)((char*)ai + 0x98) : 0;   // the entity fight record; null = no fight data at all
        team = *(unsigned*)((char*)pup + 0x10);            // entity+0x10 = the word sub_15DABC0 matches against fm+0x30C
        tgate = ai ? ((AiTypeFn)VA_AITYPE)(ai) : 0;        // 1 = the faction field is used, 0 = handle fallback
        svc = FindHealth(ai);
        if (svc) {
            life  = *(const short*)((const char*)svc + 0x5C);
            flags = *(const unsigned short*)((const char*)svc + 0x5E);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // Lock rule (ops/evidence/guard-perceive-puppet-design-2026-11-27.md, Addendum A3): ResolveFaction 0x133A4C0 can
    // enter the global handle CS [0x4DD5FA0+0x7C] (refcount-zero path, 0x133A51A / 0x133A530). It runs OUTSIDE any
    // __try, after the pup fields (+0x5C, +0xE8, +0x10) were read above. Poll runs on its own thread (no outer __try).
    fac = ((FacFn)VA_FACTION)(pup);                        // ResolveFaction (0x133A4C0) - the hostility input, NOT the team

    // Differential control: the local player through the identical chain.
    int plLife = -1, plFlags = -1;
    void* plAI = 0;
    void* plSvc = 0;
    void* plRec98 = 0;
    unsigned pteam = 0, pfac = 0, ptgate = 0;
    void* plForFac = 0;   // set once the player's +0xE8/+0x10 fields read cleanly; ResolveFaction runs after the __try
    __try {
        World* w = World::GetCurrentWorld();
        void* pl = w ? w->GetPlayer() : 0;
        if (pl) {
            void* aic = *(void**)((char*)pl + 0xE8);
            plAI = aic ? *(void**)((char*)aic + 0x1C) : 0;
            pteam = *(unsigned*)((char*)pl + 0x10);
            plRec98 = plAI ? *(void**)((char*)plAI + 0x98) : 0;
            plForFac = pl;
            ptgate = plAI ? ((AiTypeFn)VA_AITYPE)(plAI) : 0;
            plSvc = FindHealth(plAI);
            if (plSvc) {
                plLife  = *(const short*)((const char*)plSvc + 0x5C);
                plFlags = *(const unsigned short*)((const char*)plSvc + 0x5E);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (plForFac) pfac = ((FacFn)VA_FACTION)(plForFac);   // 0x133A4C0 outside any __try (lock rule above)

    // [Coop] PuppetMaxLife: the replica spawns with its own stat block (max 10 vs the player's 125), so a single
    // guard hit kills it. Copy the player's max once and refill. Cosmetic; the guest still owns the outcome.
    if (g_maxlife && svc && plSvc) {
        __try {
            const unsigned pm = *(const unsigned short*)((const char*)plSvc + 0x5E);
            const unsigned mm = *(const unsigned short*)((const char*)svc + 0x5E);
            if (pm >= 1 && pm <= 2000 && mm != pm) {
                *(unsigned short*)((char*)svc + 0x5E) = (unsigned short)pm;
                *(short*)((char*)svc + 0x5C) = (short)pm;
                life = (int)pm;
                flags = pm;
                char l2[96];
                _snprintf_s(l2, sizeof(l2), _TRUNCATE, "[PL] maxlife was %u -> %u\n", mm, pm);
                Phase1_LogLine(l2);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (g_attach && listener == 0 && InterlockedCompareExchange(&g_attached, 1, 0) == 0) {
        __try { ((AttachFn)VA_ATTACH)(pup, 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        int after = -1;
        __try { after = ((ResolveFn)VA_LISTENER)(pup); } __except (EXCEPTION_EXECUTE_HANDLER) { after = -1; }
        char line[192];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[PL] attach pup=%p before=0 after=%d\n", pup, after);
        Phase1_LogLine(line);
        return;
    }

    // Conflict-gate read (docs/evidence/conflict-gate-2026-11-17.md): the team each body resolves to, and that team's key.
    unsigned nTeams = 0, nT2 = 0;
    const void* pupTeam = 0;
    const void* plTeam = 0;
    unsigned pupKey = 0, plKey = 0;
    void* pupRound = 0; void* pupRoundTeam = 0; void* plRound = 0; void* plRoundTeam = 0; void* fmPtr = 0;
    __try {
        pupTeam = TeamFor(hptr, fac, &nTeams);
        pupKey = pupTeam ? *(const unsigned*)((const char*)pupTeam + 0x18) : 0;
        World* w2 = World::GetCurrentWorld();
        plTeam = TeamFor(w2, pfac, &nT2);
        plKey = plTeam ? *(const unsigned*)((const char*)plTeam + 0x18) : 0;
        // The round as the engine reads it: *(e+0xE8)+0x98 = AIComponent+0x98 (sub_1625330 entry, sub_161D810
        // 0x0161D82B-0x0161D835, sub_156D040). NOT ai(+0x1C)+0x98, which rec98 above reads.
        const char* aic = *(const char* const*)((const char*)pup + 0xE8);
        pupRound = aic ? *(void* const*)(aic + 0x98) : 0;
        pupRoundTeam = pupRound ? *(void* const*)((const char*)pupRound + 0x10) : 0;
        void* pl2 = w2 ? w2->GetPlayer() : 0;
        const char* paic = pl2 ? *(const char* const*)((const char*)pl2 + 0xE8) : 0;
        plRound = paic ? *(void* const*)(paic + 0x98) : 0;
        plRoundTeam = plRound ? *(void* const*)((const char*)plRound + 0x10) : 0;
        fmPtr = hptr ? *(void* const*)((const char*)hptr + 0x8DC) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Log the first sight of each puppet, then every 10 s, always (life moves are what we are watching for).
    if (g_lines < 200) {
        ++g_lines;
        char cg[256];
        _snprintf_s(cg, sizeof(cg), _TRUNCATE,
                    "[PL] gate fm=%p teams=%u/%u pupFac=%08X pupTeam=%p key=%08X pupRound=%p rt=%p | plFac=%08X plTeam=%p key=%08X plRound=%p rt=%p\n",
                    fmPtr, nTeams, nT2, fac, pupTeam, pupKey, pupRound, pupRoundTeam, pfac, plTeam, plKey, plRound, plRoundTeam);
        Phase1_LogLine(cg);
        char line[224];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[PL] pup=%p listener=%s world=%p ai=%p arr=%p n=%u svc=%p life=%d max=%u rec98=%p team=%08X fac=%08X tg=%u || player ai=%p svc=%p life=%d max=%u playerRec98=%p playerTeam=%08X playerFac=%08X playerTg=%u\n",
                    pup, listener ? "has" : "MISSING", hptr, ai, arr, cnt, svc, life, (unsigned)flags, rec98, team, fac, tgate,
                    plAI, plSvc, plLife, (unsigned)plFlags, plRec98, pteam, pfac, ptgate);
        Phase1_LogLine(line);
    }
}

DWORD WINAPI Thread(LPVOID)
{
    while (InterlockedCompareExchange(&g_run, 1, 1)) {
        Poll();
        Sleep(1000);
    }
    return 0;
}

}  // namespace

void PuppetListener::Init()
{
    if (!GetPrivateProfileIntA("Coop", "PuppetListener", 0, Paths::Ini())) return;
    g_attach = GetPrivateProfileIntA("Coop", "PuppetListenerAttach", 0, Paths::Ini()) != 0;
    g_maxlife = GetPrivateProfileIntA("Coop", "PuppetMaxLife", 0, Paths::Ini()) != 0;
    InterlockedExchange(&g_run, 1);
    if (!CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr)) {
        OutputDebugStringA("[PL] thread failed\n");
        return;
    }
    Phase1_LogLine(g_attach ? "[PL] on (attach, CharacterBehavior-aware)\n" : "[PL] on (report only, CharacterBehavior-aware)\n");
}

#pragma once
#include <cstdint>

// [Coop] Brain (default 0): the shared brain's command lane and command player, stages S0 + S2 of
// ops/evidence/guard-ai/brain-design-2026-11-22.md section 5. Policy A: the host's native AI decides; every decision it
// executes through IHumanGround (vtable 0x026E7DC0) on a scoped guard is captured, stamped with a host-clock time
// ([Coop] PeerClock) and sent on the intention lane (channel IC_BRAIN). The guest plays the same slot call on its copy of
// that guard at that host time, from the copy's own update.
//
// Captured slots (vtable 0x026E7DC0, targets read from AC4BFSP.exe with tools/pe.py 2026-11-22):
//   76  0x1D79A50 DoEnterFight(const Handle&)   replayed as 75 CanEnterFight then 76 (node 107 is a Can->Do token,
//                                                ref/AC4Coop-sail/docs/evidence/ground-tick-r1-2026-11-22.md part 3)
//   113 0x1D79C30 DoExitFight(int)              replayed as 112 CanExitFight then 113 (as GuardStageApply does)
//   155 0x1D50310 (target entity, int, int)     the AI executor sub_14BE2A0 case 11 calls it (target, 0, 34) (IDA job cc/g1)
//   160 0x1D505E0 PerformCombo(target, n, end, 0)  sub_14BE080 (brain-design section 1, VERIFIED)
//   546 0x1D4F450 (target entity, int)          sub_14BE2A0 case 27 calls it (target, 7) (IDA job cc/g1)
// Counted and logged but not replayed (argument counts not yet verified): 158/159, 241/242, 533/534.
//
// [Coop] BrainDelayMs (default 0): playout delay added to the capture time. The host itself is not delayed in this slice
// (the native call proceeds at once), so with 0 the guest plays each move one network trip late.
// [Coop] BrainTest (default 0, host): every second, one GC_MARK command (no engine call) to a random bound guard with
// execution BrainTestDelayMs (default 150) ahead: the S0 lane oracle (both sides log the same cseq at the same host time).
namespace Brain {
    void Init();             // dllmain: read flags; the capture stubs are installed when the role is known
    bool On();
    // Role (phase1 decides it): the host installs the capture stubs.
    void SetRole(bool isHost);
    // A scoped guard body, keyed by the host's objId: host capture site (native guard) and guest bind site (copy).
    void NoteBody(uint64_t objId, void* human, void* entity);
    void ForgetBody(void* human);
    // The single intention-lane consumer hands IC_BRAIN messages here (game thread).
    void OnIntent(uint64_t wireId, uint32_t seq, uint8_t flags, const void* payload, uint8_t len);
    // Every Human's own update (OnFrame): executes due commands for this body and watches its Ground path.
    void OwnUpdate(void* human);
    // Once per player frame: test driver, sweep, report.
    void Tick();
}

// phase1.cpp exports used here.
void* Phase1_PuppetEntity();
void* Phase1_LocalPlayerEntity();
void  Phase1_LogLine(const char* s);

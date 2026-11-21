#pragma once
// [Coop] CombatTimingProbe (default 0, read once at load): read-only timing of guard attacks on the other player, on one
// clock (QueryPerformanceCounter, microseconds: system-wide, so the two copies of a same-PC session share it).
// Lines are tagged [CTP]; tools/combat_timing.py joins the host's and the guest's log. Nothing here writes game state or
// the wire. Spec and PASS bar: docs/evidence/combat-timing-2026-11-17.md.
//
//   host   H fde   a guard posted a FightDamageEvent to the puppet (first warning of a series, every hit)
//          H tx    AttackForward published that event (seq)         H pdr   PlayerDamageRelay published a hit (#n)
//          H anim  a guard in sync scope changed its action (sampled on its own update)
//          H snap  a snapshot left (interval histogram in the 5 s line)
//   guest  G rx    the host's action alias for a guard changed in an applied snapshot (with the host's capture time)
//          G act   the alias-only mirror issued SetTargetAction on the copy
//          G body  the copy's own current action changed (sampled on its own update)
//          G atk   a forwarded attack event arrived / was posted to the local player (seq)
//          G pdr   a relayed hit was applied (#n)
//          G life  the local player's life changed            G pact  the local player's action changed (fight only)
//   both   frame   5 s: player-update interval percentiles, foreground share, display Hz, remote session
//          lane    5 s: snapshot send / apply intervals, host capture -> guest apply latency
#include <cstdint>

namespace CombatTiming {
void Init();                 // dllmain: reads [Coop] CombatTimingProbe
bool On();
uint64_t NowUs();

// --- both sides -----------------------------------------------------------------------------------------------
void PlayerFrame(bool host, uint32_t playerAlias, float playerRatio, int playerLife);   // the local player's update
// A body's own update: `human` is the key given to ScopeBody. Returns true when the caller should sample and call
// BodySample (the body is tracked).
bool Tracked(const void* human);
void BodySample(const void* human, uint32_t alias, float ratio);
// Register / refresh a body: host = a guard emitted into a snapshot; guest = a mirrored copy with a host record.
void ScopeBody(const void* human, const void* ent, uint64_t objId);
void Report(bool host);      // the 5 s line(s)

// --- host -----------------------------------------------------------------------------------------------------
void HostFde(uintptr_t attackerEnt, uintptr_t puppetEnt, int phase, uint32_t type, uint32_t sub);   // any thread (H1)
void HostAtkTx(uint64_t guardId, uint32_t seq, int phase, bool ok);
void HostPdrTx(uint64_t guardId, uint32_t count, int dmg);
void HostSnapshotSent(uint32_t hostMs, uint32_t records);

// --- guest ----------------------------------------------------------------------------------------------------
void GuestSnapshotApplied(uint32_t seq, uint32_t hostMs, bool newSeq);
void GuestGuardRx(uint64_t objId, const void* ent, uint32_t hostAlias, uint16_t ratioQ, uint32_t hostMs);
void GuestMirrorAct(const void* ent, uint64_t objId, uint32_t alias);
void GuestAtk(uint64_t guardId, uint32_t seq, int phase, uint32_t msgHostMs, const void* copyEnt, const void* playerEnt,
              bool posted, int lifeBefore);
void GuestAtkRepost();
void GuestNativeFde(uintptr_t attackerEnt, int phase);   // a local NPC posted an FDE to this player (not forwarded)
void GuestPdr(uint64_t guardId, uint32_t count, int dmg, const void* copyEnt, const void* playerEnt, bool posted, int lifeBefore);
}

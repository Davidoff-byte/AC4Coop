# Native replica (R3): the puppet moves itself on the ground

Status: **slice 1 built, tuned and solo-tested** behind `[Coop] NativeReplica` (default 0), 2026-11-15. Slice R3 of
`docs/design/coop-reevaluation-2026-11-15.md`. Evidence: `docs/evidence/native-replica-2026-11-15.md`.

## Goal
Stop replaying the puppet as a teleported animation and let it run as a real NPC body that the engine moves itself,
the way AC Unity drives replicas (intentions plus corrections) and AC4 MP drives duplicas (`SteeringData` = target and
speed, extrapolated on the copy; ops/evidence/x360-mp-2014-pdb.md). Native movement brings the engine's own nav
location, ground stage and statechart back on the puppet, which guards, perception and fights read.

## What the puppet was before (mapped 2026-11-15)
- `ApplyRemoteState` (phase1.cpp, player update thread, once per frame, interpolated 100 ms behind) writes the peer
  matrix with `SetGlobalMatrixInternal` every frame and sets `m_DriveEntity = false`.
- `HybridLoco=1` is meant to give the ground to the puppet's own statechart (behaviour enabled, HumanGroundData fed),
  and other contexts to action replication (behaviour disabled). **It tests context id 2, but a real peer on the ground
  sends 4** (PS3 enum `ActorContextID`, cu 277 DIE 0x4A6766B: 2 = NOT_DEFINED, 4 = Ground; taps: PlayerState+134 = 4 with
  ground stage 1; logs: `[Net] puppet behavior disabled (ctx=4)`). So with real packets the puppet always ran with its
  behaviour disabled and action replication, including on the ground. Not changed by this slice (default behaviour);
  listed as a follow-up.

## Design (slice 1)
On the ground (peer `contextId == 4`) with the flag on:
1. Behaviour ENABLED, no action replication writes, no HumanGroundData writes, `m_DriveEntity = true`, collision filter
   left alone.
2. No per-frame matrix write. A snap (`SetGlobalMatrixInternal(.., true)`) only on the first frame or when the planar
   error exceeds `NativeReplicaSnapCm` (default 300) or |dz| > 2 m.
3. Steering through the engine's NPC locomotion entry: `CSrvNavigation::NavigateToPosition` (slot 49, 0x01793650) on
   the puppet's nav service, issued from the puppet's own update (OwnDrain) when the target moved > 0.5 m or the speed
   class changed (2 s keepalive). Tuned version (v7, evidence "Tuning"):
   - moving / stopped and the base class follow the **newest packet's** intention (`destSpeed`, `sprint`), 100 ms ahead
     of the interpolated position;
   - target = interpolated peer position + peer velocity x 0.45 s (at least 2 m ahead while the replica is starting);
   - class: Regular 4 / VeryFast 7 / Fastest 8 from the intention, +1 when > 0.8 m behind along the track, +2 when
     > 2 m, -1 when 0.6 m ahead, Fastest while the replica is below 1 m/s; at rest Regular toward the peer, Stop within
     0.4 m.
4. Blended correction (AC4 MP's `STEERING_BLENDED`): between 0.5 m and the snap limit the body is moved toward the peer
   by at most `NativeReplicaBlendCmS` (default 150 cm/s; 0 = off), own rotation kept. Needed because the replica's top
   speed (Fastest) equals the peer's run speed, so a gap lost while accelerating never closes by steering alone.
   Start boost: while the replica is > 1 m/s slower and > 0.5 m behind a moving peer, the cap is
   `NativeReplicaStartBoostCmS` (default 400; 0 = off) - a short slide at run starts instead of a snap.
5. Replica-mode hysteresis: the replica stays on through off-ground spells < 400 ms and takes over again only after 300 ms
   of continuous ground, so quick context flips do not toggle the puppet's behaviour every frame.
Off the ground the old path runs unchanged.

Rejected on the way (measured, not assumed):
- **Player intention seam** (HumanGroundData +0x20/+0x98, then IHumanGround slots 1/0 on the puppet): 0.0 m of own
  movement in both runs. A probe on `SetSpeedRatio` saw only the player calling it; NPCs never do.

## Oracle (tap = host-p14-state-2.tap, 5 passes)
Honest note: these thresholds were written down while the v3 run was going, after its first few 2 s windows had been
seen, so they are not blind. They are kept loose on purpose and must be re-run, unchanged, on a second tap.
PASS when, over ground frames:
1. own movement while the peer runs is within 20 % of the peer's path length per pass;
2. no snap during a ground walking segment (snaps allowed: spawn, loop reset, return from another context);
3. p95 planar error <= 1.0 m;
4. native ground stage equals the peer's on every logged window.
The old mode is the control: its error is 0 by construction (teleport), its own movement is 0, and its behaviour is
disabled on the ground.

## Not covered yet (next slices)
- Facing at rest (the nav call takes a direction vector, a7; its effect is unmeasured).
- What NavigateToPosition's return value means (eax = 0 on every call while the puppet moved).
- Off-ground contexts (climb, ledge, swim) still use the replay path; transitions back to the ground re-enable
  behaviour after the off-ground branch has disabled the collision filter (one-shot, PuppetPhysics=0).
- Fight stance, damage, guards' reactions to a native puppet (R4-R6). Two-machine run.

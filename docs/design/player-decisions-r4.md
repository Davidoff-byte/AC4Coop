# R4: player decisions on the native replica (plan)

Status: **plan** (2026-11-15), slice R4.1 in progress. Roadmap: `docs/design/coop-reevaluation-2026-11-15.md` (R4).
Builds on R3 (`docs/design/native-replica.md`): on the ground the puppet is a native NPC body moved by its own nav
service. Everything else still uses the old replay path (behaviour disabled, peer animation copied with
SetTargetAction), and the target-needing parkour contexts (Ladder 5, Ledge 7, Walling 8, NarrowObject 9, Climbing 30,
Hanging 31) cannot be entered on a teleported puppet: `DriveContextMask` keeps them off because the engine's later
update crashes without the context's attach target (AC4Coop.ini note).

## What decisions are (leads, not AC4 facts)
- **ACU** (user's AU-RW, ACU-RE-0042/0043): parkour is a candidate generator plus a selection function returning an index
  into a candidate array; each candidate is an action object of one of ~112 types with its target geometry. A replica
  that receives (type, target) can run the same move. ACU's netcode carries these as `NetBehaviorAI_*` parkour decisions.
- **AC4 MP 2014 PDB** (ops/evidence/x360-mp-2014-pdb.md): duplicas get state as *transitions* between ActorContextIDs
  (`TransitionManager`, `NetProcessReceivedTransitions`, `M2R_ReplicateEnterCustomActionState`); in-air position and yaw
  are sent directly; ground is steering.
- **AC4 SP names** (PS3 DWARF index): `ClimbingSequencer::CandidateMove` (240 B), `TransitionCandidateMove` (160 B),
  `CandidateCoreMovement`; `fsm_humannarrowobject` FreeRun data; `FreeRunTargetingMagnetComponent`. Climbing has its
  own candidate selection, the AC4 analog to ACU-RE-0042. Nothing of this is mapped on PC yet.

## Slices (each behind a default-0 flag, oracle first)
| # | slice | oracle |
|---|---|---|
| R4.1 | **Off-ground follow by native navigation.** Keep the replica (behaviour on, nav service) when the peer climbs, aim the nav at the peer's 3D position, teleport only when stuck. Tests whether the NPC body climbs by itself the way chasing guards do. Cheap; a negative result is useful | parkour segments of host-p13-1 / host-p14-state-3 / state-10: replica enters a climb context natively, 3D error at the end of each segment, snaps per segment, no crash |
| R4.2 | **RE the player's climb decision** on PC: ClimbingSequencer candidate generation and selection, the chosen move's type and target (handle / positions). Static first, then a [Probe] trace like ACU-RE-0042 | a recorded trace where every climb step of a scripted run has a chosen candidate |
| R4.3 | **Replicate the decision**: the sender publishes the chosen move (intention lane, proto 15) with a stable target reference; the replica runs the same candidate, which supplies the context's target and avoids the crash class | climb segments: replica context sequence matches the peer's within one step, no crash with the mask open |
| R4.4 | **Actions with targets**: attack, assassinate, custom actions (IHumanGround `Do*` family, MP slots 163/313/...); damage authority stays with R6 | action sequence parity on a combat tap |
| R4.5 | Swim and boat contexts on the replica | swim tap divergence |

## R4.1 design
- Flag `[Coop] NativeReplicaParkour` (default 0; needs `NativeReplica=1`).
- Replica mode also covers peer contexts 5/6/7/8/9/30/31 (with the R3 hysteresis); swim, boat and the rest keep the old
  path.
- Nav target = peer's 3D position; speed `ChaseHigh` (10) while the peer is off the ground or > 1.5 m above/below,
  otherwise the R3 classes. Blended correction and start boost only on the ground with |dz| < 1 m (pushing a climbing
  body into a wall would fight the climb).
- Teleport: 3D error > `NativeReplicaParkourSnapCm` (default 800), or 3D error > 2.5 m for more than 4 s (stuck).
- Measurements per 2 s window: 3D error (mean/max) on off-ground frames, the replica's own context ids seen, snaps.

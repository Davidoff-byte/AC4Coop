# Status

Last updated 2026-11-11, at `main` after PR #1 (the live-tests stack). This file replaces every plan,
journal and handoff in `docs/archive/`. Update it in the same PR as any change that alters what is true here.

## Build
- Target: `AC4BFSP.exe` PC 1.05, 45,056,040 B, sha256 `bc11ad79…ab5150`. Any other build is refused by the
  build gate (`known_builds.txt`), which logs `UNKNOWN_BUILD` and patches nothing.
- Last pinned build: `cddbc41`, ASI 342,528 B. `main` is now three commits ahead of it (skel-attach `1eb652d`,
  F8 BoardableTest `19788a1`/`9f6fe53`) plus README updates.

## Verified, and how
| area | state | verified by |
|---|---|---|
| transport | UDP peer-to-peer both directions, ~200 ms over a VPN | two-machine run |
| player puppet | the peer's body mirrors position and animates; collision off; ~33 kB/s | live run 2026-11-10 (`glide-live-2026-11-10`) |
| kills | host→guest | live; guest→host is built but unverified |
| enemy scope | classified by `entity+0x9C` kind/role from the game's own enums | `[Fight]` census (`d4b5088`) |
| ship mirror | a duplicated local ship carries the peer's hull HP (`GcLShip+0xD0`) and transform | live |
| body spawn | engine `EntityBuilder` + duplicated `RowSelector` spawns a second Edward | in game |
| event layer | exactly-once delivery under 30 % loss | `tools/event_peer.py` selftest (`05eb0c6`), Python only |
| two copies on one PC | two `AC4BFSP.exe` running at once, both started directly from the exe (no Steam), with `proxy/version` `version.dll` | live 2026-11-15, pids 25788 + 26200 (`docs/evidence/single-instance-guard-2026-11-15.md`); render and co-op between them not yet checked |
| fight decisions (S7 slice 1) | `[Coop] FightIntents` (default 0): fight enter / attack / exit with target on the intention lane drive the puppet's own FSM | same-PC two copies 2026-11-15: enter/exit 3/3, one real attack (`docs/evidence/s7-fight-intents-2026-11-15.md`); needs the target bound by the entity lane |
| guards hitting the other player | `[Coop] PlayerDamageRelay` (default follows FightIntents): a guard's hit on the puppet (lost there: the puppet has no fight/health services) is scored with the engine's `sub_199A160` and posted as a native DamageEvent on the owner's player | same-PC two copies 2026-11-16: 13/13 hits applied, native death and respawn, no double hits (`docs/evidence/player-damage-relay-2026-11-16.md`); no hurt reaction, the owner's block/counter not consulted |

## Measured negatives
- Guards do not perceive the other player's body, with or without collision. `docs/ops/P6-RESULT.md`, `RUN-2026-11-10-X2b`.
- A guard-shaped spawn through the crowd route (`EntityBuilder` slot 39) access-violated 3/3.
- The 2026-11-08 launch crash was not gated by any ini flag; its root cause was the action-guard trampoline
  continuation jump, fixed in `c718e92`. The investigation is in `docs/archive/ops/HAUL-2026-11-08.md`.

## Built but not verified live
- Everything after `cddbc41`: skel-attach (`[Coop] SkelAttach`, default 0, tested solo only) and BoardableTest
  (`[Coop] BoardableTest`, default 0).
- Guest→host kill relay.
- Native replica on the ground (`[Coop] NativeReplica`, default 0; `docs/design/native-replica.md`): solo replay
  tested and tuned, the puppet moves itself through its nav service plus a capped correction; final build passed the
  oracle in one run (p95 0.60 m); an action-system crash during rapid context flips was seen twice before the switch
  hysteresis (link unverified)
- R4.1 off-ground follow by navigation (`[Coop] NativeReplicaParkour`, default 0): solo-tested negative, the replica
  does not climb after the peer (`docs/evidence/native-replica-parkour-2026-11-15.md`).
- R4 climbing input (`[Coop] NativeReplicaClimbIntent`, default 0): the peer's real climbing input rides in
  PlayerState's context block (ClimbBlock); solo replay: on-wall error 1.02 m vs 1.26 m error-derived, route still
  diverges (`docs/evidence/native-replica-climb-input-2026-11-15.md`).
- R4 climbing moves (`[Coop] NativeReplicaClimbMoves`, default 0): each peer move's hand holds ride in the ClimbBlock and
  the replica follows the peer's hold path; solo replay, 3 runs each: on-wall error 0.92 m vs 1.01 m with real input
  only; route still diverges (`docs/evidence/climb-moves-2026-11-15.md`).
  (`docs/evidence/native-replica-2026-11-15.md`). Same evidence: `HybridLoco`'s ground branch tests context 2, but real
  packets send 4, so it never runs.

## Open
- Guard perception of the other player: needs the Gamer/avatar route (`docs/ops/ANGLE-GAMER-AVATAR.md`,
  `docs/ops/GAMER-AVATAR-API.md`); two PC-mapping anchors for the Gamer global came back negative (`b5fe59b`).
- Sail/billow sync: spec only, needs protocol 14.
- Second game build (1.08 etc.): 22 of 101 sites relocated (`docs/archive/ops/JOURNAL-2026-11-04.md` §11).
- Connection setup is manual: both machines edit `[Net] PeerIP/LocalPort/PeerPort` in `AC4Coop.ini`, and
  internet play has needed a VPN.
- The spine's byte arm needs the pinned compiler `cl 16.00.40219.01`; until then no unit can reach `matched`.

## Plan of record
Replication layer: `docs/design/storm-replication.md` (2026-11-15, Storm/HackNet-style net objects, decisions on the
intention lane, slices S0–S10). It refines the R1–R7 roadmap of `docs/design/coop-reevaluation-2026-11-15.md`.
`docs/ops/PLAN-COOP-V2.md` (outcome-sync architecture, gates P0–P8), with `docs/ops/SOLO-PLAN.md` for what one
machine can prove.

Next slice: lobby codes (host shows a code, guest enters it), serverless, one guest. Design and PR stack in
`docs/design/lobby-code.md`.

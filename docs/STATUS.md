# Status

Last updated 2026-11-28 (night) on `feat/integration-1121`: smooth synced ships — `ShipInterp` (AI ships) and `ShipGhostInterp` (the peer's ship) (row "ship smoothness"; `docs/design/ship-interp.md`); before that the same day SpawnLib — spawn engine AI ships / NPCs on demand and sync them host→guest (row "spawn: SpawnLib"; `docs/design/spawn-lib.md`). Before that 2026-11-25 (afternoon): the spawn manager, spawner keys and key binding (rows marked "spawn"; day trail in the parent `ops/JOURNAL-2026-11-25.md`); earlier the same day puppet animation carry, sinking fix, crash-class CR-2/3/4. Before that 2026-11-21 on `feat/launcher` (guest combat timing, plan step 1 of `ops/HANDOVER-MP-BLUEPRINT-2026-11-21.md`); the
rest of the file dates from 2026-11-11, at `main` after PR #1 (the live-tests stack). This file replaces every plan,
journal and handoff in `docs/archive/`. Update it in the same PR as any change that alters what is true here.

`feat/integration-1121` is the merge of `feat/launcher` (combat) and `feat/shipmirror-port` (coord/sail/fort) on
common base `b95b7a5`; it is the branch that carries every feature described below, together, for the first time.

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
| puppet animation carry | `[Coop] PuppetFightCarry=2` (default 0): the peer's primary action copied onto the puppet everywhere, not only in fights; the sword drawn in a fight follows it | same-PC two copies 2026-11-24/25: alias match 92-97 % ground / air / climbing; guest footage shows the puppet swinging a cutlass (`docs/evidence/brain-s0-2026-11-22.md`). Overlay channels (`PuppetChannels`) freeze poses and stay 0 |
| puppet standing on the ground | `[Coop] PuppetUpdatePhysics=1` (default 0): the matrix write also moves the puppet's CharacterController, on the puppet's own update | 2026-11-25 A/B on the guest screen: without it the puppet sinks to the knees (controller left at the spawn point; IDA 0x528200 -> sub_42D570), with it the boots stand (arms E/F); bodies separate ~0.5 m on contact once (control: none) |
| host kills on the guest | fixed: flag-only deaths get the engine kill, OwnerDamage no longer blocks the relay, the lane resolves by the snapshot table | 2026-11-25 two copies: 17/17 (`13ac6e0`) |
| mirrored guard motion | per-frame writer with a bounded approach (no takeover pops) | 2026-11-25: stall 83 % -> ~2 %, pops 64 -> 1 (`20b7ab0`) |
| spawn: the spawn manager | every AI ship (21/21) and 132/141 characters come out of one funnel (`0x46D8E0`, SpawnEntityParams::Spawn) behind one request factory and `0x79B8D0`; FX/props are local | 2026-11-25 census, 1785 world adds (`docs/evidence/spawn-manager-2026-11-25.md` s1-2) |
| spawn: request replay | re-submitting a captured request through `sub_569560` makes the manager build it | 2026-11-25, NPC n=1 (s4) |
| spawn: shared spawner key | the world spawn point of a static-anchored spec, computed without engine calls, is equal on two copies | 2026-11-25: 68/69, unique (s5) |
| spawn: key binding | `[Coop] SpawnKeyBind` (default 0): host guard objId -> spawn key on IC_SPAWN; the guest binds its own body built for the same key | 2026-11-25 two copies: 12/12 keyed guards at 0.0 m; coverage 12/32 (the rest are ship crews) (s7-8) || ship class binding | `[Coop] ShipClassBind=2` (default 0): each host ship bound to a guest AI ship of the same class and driven to the host transform with physics, 20 Hz | 2026-11-25 two copies at sea: bound 168/168, error mean 1.3-2.5 m, 0 exceptions (`naval-s0-2026-11-25.md` s9); crews/AI/cannons/hull not yet synced |
| crew keys | `[Coop] SpawnKeyBind` also keys ship crews by (ship objId, local offset) through the ShipClassBind table | 2026-11-25: 37 crew bound at median 1.4 m, but on the players' docked Jackdaws (now excluded); AI-ship crews 0 bound, cause not measured (`naval-s0-2026-11-25.md` s10) |
| spawn: SpawnLib | `[Coop] SpawnLib` (default 0): spawns engine AI ships / NPCs through the SpawningManager on command (`SpawnCmd` + `SpawnCmdSeq`); naval submits carry a private owner (no engine adoption) and borrow the spec slot; queued requests on a dying spec are re-pointed (detour `0x501DF0`); guest: ShipSpawnReplay builds a hull per host ship, NPC follow builds a body per host key | 2026-11-28 two copies T8 (asi 4F47E385, `3e73a97`, docked): 411 s both alive; T15 at sea (asi E3351439, `116496b`): host 4/4 ships built in 1.5 s, guest 6/6 replays bound, err mean 0.60 m, 3 fast travels without a crash; host 37 spawns (9 ships, 28 NPCs); guest 7/7 ship replays bound (err mean 0.36 m), 27/27 follow NPCs bound by key. Crews of spawned ships are deck actors (no AI) that the guest replay hull spawns with the same layout (7/7 offsets, n = 1) and carries; host Jackdaw crew keys no longer published. Open: AI crews on AI ships untested (at sea near Havana only Jackdaws carry AI crew); one guest AV `0xF1813C` after a ShipClassBind steal (T6, unexplained). 2026-11-28 night: two more crash classes fixed (our requests in the real owner's container, AV `0x1425954`; released requests on a freed borrowed spec, AV `0x198F274`): fast travel with 3 spawned ships alive, both copies alive (asi 85159A69 and 117D9734), redirectFail 0 (`docs/evidence/spawn-lib-2026-11-28.md` s14-15) |
| ship smoothness | `[Coop] ShipInterp` (AI ships bound by ShipClassBind=2, guest) and `[Coop] ShipGhostInterp` (the peer's ship drawn by ShipGhost, both copies); code default 0, template 2 / 2: per-frame drive on the sender's timeline, cubic Hermite, prompt ring feed, own slewed clock, `m_DriveEntity` off on driven hulls (`docs/design/ship-interp.md`) | 2026-11-28 two copies at sea (asi 51B92A56 / 117D9734): AI ships 0.25 % frames > 15 m/s² vs 50.8 % before, jerky 3.6 vs 33 %, pops 0 vs 1313 per ~1 min; ghost 0.0 % vs 19.2 %, jerky 2.6 vs 14.7 %, pops 0. Open: one-frame engine displacement of a live ship whose spec a guest replay borrows (cause unread); two machines untested (`docs/evidence/spawn-lib-2026-11-28.md` s15) |
| SgmDetour thread safety | CR-4: per-call state on the stack, body under `__try` (`HookSgm` ships as 1) | 2026-11-25 run HookSgm=1: both alive, 0 exceptions |
| guest combat timing | `[Coop] CombatTimingProbe` (read-only) and `[Coop] GuardActionLane` (default 0): a guard's action switch rides the intention lane with the hits | owner fight 2026-11-21 (asi `9da8d40d`): lane off, hits landed 251-378 ms before the swing shown on the guest (action p50 329 ms, hit p50 10 ms, n = 18); lane on, linked copies \|skew\| p95 45 ms (n = 10). Frame time is not the cause (guest p95 21-22 ms, unfocused host 18 ms). **AWAITING OWNER:** whether counters can be timed (`docs/evidence/combat-timing-2026-11-17.md` run 3) |

## Measured negatives
- Guards do not perceive the other player's body, with or without collision. `docs/ops/P6-RESULT.md`, `RUN-2026-11-10-X2b`.
- A guard-shaped spawn through the crowd route (`EntityBuilder` slot 39) access-violated 3/3.
- The 2026-11-08 launch crash was not gated by any ini flag; its root cause was the action-guard trampoline
  continuation jump, fixed in `c718e92`. The investigation is in `docs/archive/ops/HAUL-2026-11-08.md`.

- spawn: calling SpawnShipParams::Spawn and then World::AddEntity ourselves crashed 3/3 (SSrvNavigation+0xEC NULL in the
  GcLShip activation): the manager's post-spawn step is required (`naval-s0-2026-11-25.md`).
- spawn: holding the guest's own spawn requests (`[Coop] SpawnShare`) has nothing to hold: 90/90 static spawns happen
  during loads, and a hold during a load froze the guest's boot (fixed by holding only in world).
- spawn: AI ship spawn data has no per-ship identity (builder id 0, identical SpawnShipParams on both copies).
- The repo's shipped `AC4Coop.ini` has `IntentLane=0` and `DisableSpawns=1`: every lane feature (Brain set, OwnerDamage, SailSync, SpawnKeyBind) is inert in a kit built from it, and SpawnKeyBind is also off under DisableSpawns. Owner sessions ran on the install ini (IntentLane=1, DisableSpawns=0). The 2026-11-25 test kit ships its own ini.
- Fast-travel guest crash (`sub_76E0F0` release during the sea -> Havana unload) needs ship clones alive: 5/5;
  `ReleaseStaleGuard` only moves it. INTERIM FIX in place: `MirrorRetireAll()` retires all proxies before
  world change (gated by `[Coop] FastTravelCrashFix`, default 1). Complete fix needs sub-entity independence
  (audit sub_52A980). Mitigation ShipMirror/ShipGhost=0 still safest. UNVERIFIED.
## Built but not verified live
- 2026-11-25 crash-class: CR-2 `reloadBuilder` gives the async loader a static slot (path not reached in any run); CR-3 `WbTrap` tolerates a guard re-applied after a disarm (debug trap, default 0). CR-6/CR-7 closed by disassembly with no change; CR-5 (guards written from the player thread) PARTIALLY ADDRESSED: legacy direct-write fallback on starved guards now skipped when OwnGuardsOn() is true (hypothesized cause of worse score). Full fix needs game-run verification.
- Guest combat, built after the owner fight (default 0, NOT yet run in the game): the 16-byte alignment fix for the forwarded
  attack event (968 of 1086 warning re-posts faulted at `0x018BD72D`); `[Coop] RespawnHitGraceMs` (hits forwarded while the
  guest reloads were applied 2.6-4.5 s late to the respawned player); `[Coop] GuardStageApply` (the guard's Fight stage
  through Can/DoEnterFight and Can/DoExitFight on the guest's copy, `docs/evidence/guard-stage-apply-2026-11-21.md`).
- GuardPlace (`[Coop] GuardPlace`, default 0) is now on `feat/launcher` (cherry-picked `89da594`, `844586f` from
  `feat/enemy-placement`; evidence `docs/evidence/enemy-placement-2026-11-16.md`).
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

Next slice (owner, 2026-11-25): ship sync - each guest AI ship bound to a host ship of the same class and driven to the
host's transform (AI off), then ship-crew keys = (bound ship, local offset). Design: `docs/design/host-spawn-authority.md`.
Lobby codes: `docs/design/lobby-code.md` (their state was not re-checked on 2026-11-25).

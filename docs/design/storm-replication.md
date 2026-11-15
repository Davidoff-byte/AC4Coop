# A Storm-like replication layer for bf-coop (plan, 2026-11-15)

Status: plan. Nothing in it is built beyond what section 2 lists as existing. It refines the R1-R7 roadmap in
`docs/design/coop-reevaluation-2026-11-15.md` with what came in since then:
- the X360 MP PDB names (`ops/evidence/x360-mp-2014-pdb.md` in the parent repo);
- the AU-RW reading of ACU's Storm co-op (AU-RW `origin/acu-85-coop-re-plan`: `docs/coop/AVATAR_SPEC.md`,
  `docs/findings/ACU-RE-0075..0085`);
- today's same-PC A/B (`docs/evidence/act-match-ab-2026-11-15.md`);
- two-copy testing on one PC (`docs/evidence/same-pc-coop-2026-11-15.md`).

Rule for this document: an AC4 fact carries an address, a `file:line` or an evidence file. ACU and AC4-MP facts are
leads for AC4BFSP.exe. Anything not verified in the SP binary is marked UNVERIFIED.

## 1. The model: what "Storm-like" means here

What ACU's Storm (ACU findings, read statically in IDA) and AC4's own MP exe (HackNet, PDB names) agree on:

| Pillar | ACU (Storm) | AC4 MP (HackNet) | Source |
|---|---|---|---|
| Per-object roles | Only the master of an object pushes its state; replicas apply it | `NetObject` +0x10 NetRole, +0x18 MasterStationID, +0x28 IsMigrating, +0x2A Authority | ACU-RE-0084; PDB part 2 |
| Replica runs natively | Replica restarts the same behaviour through the normal entry point | Duplica executes replicated transitions (`NetProcessReceivedTransitions`; HumanGround context slots 18-29 `ExecRemoteFunc*`) | ACU-RE-0080/0081; PDB part 2, chunk 3 |
| Decisions, not animation | 94 typed `NetHumanIntention` messages (navigate, assassinate, fire, custom action...), targets as u64 handles, one receive handler `sub_1407269B0` | Transitions with additional info (`S2C_Transition`, `S2C_TransitionToLedgeClimb(uint64,float,uchar,RMCHumanLedgeOrClimbAddData)`, `M2R_ReplicateEnterCustomActionState(Entity*,...)`) | ACU-RE-0075/0079; PDB part 2b |
| Movement apart from decisions | Dead-reckoned proxy pool: resend when error² > 0.25 (0.0025 precise), after 4 m (2.5 m), on stop, or heading change > 0.175 rad; transform set directly | Ground: steering target + speed, extrapolated on the duplica (`SteeringData`, `GetExtrapolated`); in air: position + yaw direct; `MoveReplicationMode` {FULL_MOVE, STEERING_ALONE, STEERING_BLENDED} | ACU-RE-0076/0077; PDB part 2 |
| Continuous state as mementos | Per-field, applied only if newer by timestamp (HumanAvatar, health, AI blobs) | Datasets with `m_Dirty` + `NetSerializer`; NPC character-logic blobs, detection, targets | ACU-RE-0084; PDB part 2 |
| Replica AI off | A gate refuses intentions from a replica's own sources (`sub_141C45720`, the meaning of source 2 is inferred) | "AI only on master" for `NetGenericNPC` | ACU-RE-0080; PDB part 2 |
| Damage goes to the master | `SendFightDamageEventToMaster` (name only) | `D2M_SendDamageKillEvent(Handle<Entity>)`, `R2M_SendApplyShockEffects` | ACU names; PDB part 2b |
| Join in progress | Master re-sends its stored messages and unicasts active intentions | `S2C_GettingRelevant(...)`, `C2S_NeedsResynchro` | ACU-RE-0083; PDB part 2b |
| Identity | Pre-placed NPCs and props matched by entity id on each peer (`sub_141C5B2D0`), not network-spawned | `NetObject` +0x14 InstanceID | ACU-RE-0076; PDB part 2 |

The SP exe has no such layer. AU-RW's `net_title_survey.py` found no replication framework in AC4BFSP.exe
(ACU-RE-0074). The HumanGround context vtable on PC (0x026E8AE8) ends after about 18 slots, so the MP replica
executors are compiled out (PDB "PC check": strong lead from the vtable shape, not a full proof). **So we build the
layer ourselves, around the same SP entry points the MP executors would have called.**

## 2. What bf-coop already has (reused, not rebuilt)

- **Transport:** UDP peer link with lobby codes, an EOS option, host time stamps and stale-sequence rejection
  (`src/net.cpp`); same-PC two-copy runs (`docs/evidence/same-pc-coop-2026-11-15.md`).
- **Reliable lane:** intention lane, proto 15 (`docs/design/intention-lane.md`). It has 16 channels, 32-B payloads, a u64 id per
  message, acks, resends and last-message-per-channel replay for late join. Selftests pass. It ran idle in the game
  (`docs/evidence/intent-lane-ingame-2026-11-15.md`). Nothing publishes on it yet.
- **State lanes:** `PlayerState` snapshots (576 B, `src/netstate.h`) and the entity lane for guards.
- **Player animation layer:** action-alias copy. Measured with replica OFF: 95-100% action match on ground, 86-96% in
  the main off-ground contexts, 57-82% in transitions (`docs/evidence/act-match-ab-2026-11-15.md`).
- **Native replica experiment:** the puppet moves itself through its nav service. With it on, the peer's actions never
  reach the puppet (`phase1.cpp:2789`: `if (replica) { aiOff = false; actNow = false; locoNow = false; }`), giving 0% match off
  the ground. Parkour follow by navigation is a measured negative (`docs/evidence/native-replica-parkour-2026-11-15.md`).
- **Verified SP seams:**
  - `IHumanGround` SetDirection 0x01CB91B0 / SetSpeedRatio 0x01D51690 drive any Human, verified live on Edward
    (`docs/evidence/pilot-intent-2026-11-15.md`).
  - `AIActor::GetCurrentContextID`, plus TransitToContext / ExitCurrentContext 0x0052C330 (`phase1.cpp:50-57`, `:1892`).
  - `SetTargetAction` 0x6625D0.
- **Known wall:** entering a climb or ledge context externally crashes `Human::Update`, because the context dereferences
  target data (wall normal, ledge point) that a real climb computes. `DriveContextMask` therefore allows only InAir and
  Swim (`phase1.cpp:114-121`). The MP build's answer is to send that data with the transition (pillar 3 above).
- **Oracles and tools:** `[ActMatch]` (`a29b8ab`), `pilot.py` (scripted master), `[Probe]` pipe,
  `tools/arrange_windows.ps1`.

## 3. Architecture

### 3.1 Net objects
- A registry `NetObj { u64 netId; u8 cls (Player, Npc, Ship, Prop); u8 role (Master, Replica); u8 masterSlot;
  u32 state; Entity* local; }`.
- `netId` is the lane's existing u64 `wireId`, so the intention lane needs no format change.
- **Roles, phase 1:** each player masters its own avatar (as in ACU and AC4 MP). The host masters every NPC.
  Per-object migration (`IsMigrating`, `MigrateToPlayer`) is designed for in the ids, not built.
- **Identity:**
  - Players: fixed ids (slot 1 and 2).
  - NPCs: first **measure** whether a placed NPC has the same engine id in both copies on the same save. This is now
    testable on one PC (the worldId already matches: `[Id] worldId=0000000032E21E58` in both copies). The `[MirrorAct]`
    objIds `F000xxxx` look runtime-minted: UNVERIFIED.
  - If ids do not match: key by the placed spawner (open item 2 of `unity-netcode-mapping.md`).

### 3.2 Three kinds of traffic, by lane
| Kind | Content | Lane | Rule |
|---|---|---|---|
| Movement | Ground: steering target + speed ratio + heading. Air/climb: position + yaw. | Unreliable snapshot (`PlayerState` / entity lane) | Dead-reckoned. Resend thresholds start at ACU's values (error² 0.25, 4 m, stop, 0.175 rad). Replica applies through `IHumanGround` SetDirection/SetSpeedRatio on ground; direct transform in air; teleport past a threshold. |
| Decisions | Context transitions with their additional info; targeted actions (enter fight, attack, assassinate, grab, custom action, interact) with target netIds | Reliable intention lane, one channel per concern (locomotion-context, action, weapon, look-at, interaction) | One message per change; last per channel replayed on join; applying the current state again must be a no-op |
| Mementos | Health, death, weapon drawn, fight stage, alert state, mission flags | Reliable lane (low rate) or snapshot fields with a timestamp | Apply only if newer |
| Events | Damage request, kill, loot, cutscene start | Reliable lane | Non-master requests; master applies and broadcasts the outcome (`D2M_*` pattern) |

### 3.3 The replica Human (player puppet first, guards later)
1. **AI logic off, native state machine and animation on.** We need the seam that skips the AI decision layer of one
   Human while its HumanGround FSM, locomotion and action system keep running. UNVERIFIED that AC4 separates them
   (`unity-netcode-mapping.md` item 1, lead `sub_52DF80`). This is also the ACU replica gate.
2. **Decisions drive the FSM through SP entry points.**
   - The transition executor, recorded on the master and replayed on the replica. It must carry what the MP build sends
     as additional info: ledge/climb attach data, landing intention, jump target. Without it the climb contexts crash
     (section 2).
   - Targeted actions through `IHumanGround` methods. MP names: Attack(Entity*,bool,WeaponType) slot 163,
     Can/DoEnterFight(Handle) 95/96, Can/DoAssassinate 40/41, Can/DoEnterCustomAction 378/379. The PC slots differ:
     the SP interface has extra methods (PDB chunk 1: GetFightStage MP 282 -> PC +18; GetHumanGroundStage MP 629 -> PC +60).
     Each PC slot must be found in IDA. UNVERIFIED.
3. **Animation copy stays as a per-context fallback.** Until a context's transitions replay correctly, keep the action-alias
   copy (measured 86-100%) for that context. Switch a context to "decision-driven" only when its oracle beats the copy.
   This replaces the global NativeReplica switch, which today blocks all action writes in every replica context.
4. **Correction:** native steering for small errors; snap past a threshold; never in the middle of a decision-driven
   attach (climb, ledge).

### 3.4 NPCs
- AI only on the master (host). Guest guards are replicas, with the same seam as 3.3.1 and the same four channels
  (action, navigation, weapon, look-at; `unity-netcode-mapping.md` item 3).
- Detection runs on each machine against its own player. Outcomes go to the host, which merges alert and mission state
  (item 7 there).
- Damage to a host-mastered guard from the guest: a request event; the host applies it and broadcasts health or death.

## 4. Implementation stack

Every slice sits behind a default-0 `[Coop]` flag, ships with its oracle, and is verified on two copies on one PC before
any two-machine run.

| # | Slice | Work | Oracle (PASS/FAIL without opinion) |
|---|---|---|---|
| S0 | Test rig | Two-copy runner (`ab_run.ps1` pattern) into `tools/`; scripted master moves; fix or bypass the pilot frame-cap freeze (`boot --fps 0` worked; the root cause of the Present-patch freeze is open) | 3 consecutive two-copy runs boot and finish with no hang |
| S1 | RE, MP exe (static) | In AC4BFMP.exe (PC, no PDB; X360 PDB names as the map): `AddTransitFromContext/Substate/WithFn`, `NetProcessReceivedTransitions`, `Net{Replicate,Read}AdditionalInfoForTransitions`, `S2C_TransitionToLedgeClimb`, `M2R_ReplicateEnterCustomActionState`, steering extrapolation. Output: for each replicated transition, the SP function it ends in and the data it carries | Each mapping names a PC MP VA plus the SP VA it corresponds to (byte or behaviour match); RULES-RE negatives carry METHOD/COVERED/NAMES |
| S2 | RE, SP exe | PC `IHumanGround` slots for the targeted actions; the AI-off seam; the transition entry that accepts attach data | Decompile plus a live probe on Edward (a call through the slot does the action) |
| S3 | Net objects + identity | Registry and netIds; the NPC same-id measurement on two copies | Fraction of nearby NPCs whose id matches in both copies (measured, recorded) |
| S4 | Wire the intention lane | Publish and consume, with a loopback replica (host drives a local copy from its own outgoing messages) | Lane delivers, acks and replays on rejoin in game, with two copies |
| S5 | Player movement v2 | Steering-based ground movement, direct air movement, dead-reckoning thresholds, teleport threshold | Position error p95 on the scripted route, against today's baseline |
| S6 | Player transitions | Replay context transitions with additional info, starting with jump/InAir, then ledge, then climb | Per-context transition sequence parity and ActMatch at least the copy's level; zero crashes over N climbs |
| S7 | Player targeted actions | Enter fight, attack, assassinate, custom action, with targets resolved by netId | Host action on a guard gives the same action and the same target on the guest puppet; no null-target crash |
| S8 | Guard replicas | Guest AI off on guards, four channels, identity from S3 | Guard divergence-onset rate; GuardActionGate no longer needed |
| S9 | Fight authority | Damage requests to the master; health and death mementos; kills both ways | Hit applied exactly once per landed strike, on both machines |
| S10 | Shared state | Mission channel with history, relevancy, join in progress; later per-object migration | Late join reproduces mission and alert state |

Order: S0 → S1 + S2 (static, can run in parallel) → S3 → S4 → S5 → S6 → S7 → S8 → S9 → S10.
S5 and S6 can start on S2's results without waiting for all of S1.

## 5. What stops now
- **New NativeReplica parkour work** (`NativeReplicaParkour`, `ClimbIntent`, `ClimbMoves`, `WallEntry`). With the replica
  on, the peer's actions never reach the puppet; S6 replaces the follow-by-navigation approach. Keep the flags, default 0.
- **The test install's ini** has those flags on (`NativeReplica=1` and the five R4 flags). For eye tests of today's best
  movement, run with `NativeReplica=0`.
- **New host-side workarounds** for an AI-less puppet, and new animation-layer fixes for guards (as in the re-evaluation
  §4). Keep the verified ones until their replacement passes its oracle.

## 6. Open decisions for the owner
1. NPC authority: host masters all NPCs for now (recommended, simpler), or per-object masters from the start?
2. S1 depth: decompile the MP transition replication fully (slower, engine-exact), or only enough to learn the SP
   entry points and the attach data, then build our own (faster)? Recommended: the second, with S6 as the check.
3. Merge order with the other sessions' branches (`re/guard-*`, the F7 work on `feat/launcher`).

## 7. Risks
- The AI-off seam may not exist as a clean split. Fallback: a per-Human action gate, as `GuardActionGate` does, which
  is known to work but fights the AI instead of switching it off.
- Attach data may hold pointers into the master's world (ledge objects). Those need netIds or a geometric re-query on
  the replica. UNVERIFIED what the MP `RMCHumanLedgeOrClimbAddData` holds; S1 answers it.
- Two copies on one PC share a CPU; a frame time far below a real machine's may hide timing bugs. Two-machine runs stay
  the final gate for every slice.

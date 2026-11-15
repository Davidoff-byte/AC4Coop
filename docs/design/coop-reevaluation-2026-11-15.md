# Re-evaluation: make bf-coop feel like true multiplayer, the AC Unity way (2026-11-15)

Inputs: the ACU binary analysis (`C:\Users\Administrator\acu-unity-re\COOP-NETCODE-ACU-vs-AC4.md`, tagged [D]/[S]),
the GDC 2015 talk mapping (`docs/design/unity-netcode-mapping.md`), the plan of record (`docs/ops/PLAN-COOP-V2.md`),
`docs/STATUS.md`, and this week's live results (F7, Pilot, Probe). Engine claims about ACU are leads for AC4, not AC4 facts.

## 1. What ACU does (the model to copy)
1. **Per-object master/replica** (Storm/Echo). One peer simulates an object; the others hold replicas. Roles can move.
2. **Replicas run natively.** AI logic is off on a replica, but its human state machine and animation run, driven by
   **intentions** (NetHumanIntention_*, NetBehaviorAI_* parkour decisions) plus periodic **movement corrections**
   (SyncMovement / ReceiveInSynMovement), teleport only as a last resort.
3. **Each player owns their own avatar.** Local response never waits on the network; remote peers replay decisions.
4. **NPC AI only on the master**; NPCs are networked explicitly (GcL*NetData: guard, chase, health, fight...).
5. **Identity by creation**: replicas are created/deleted once, keyed by a stable id (spawner-keyed in the talk).
6. **Events for discrete changes, deltas for state** (per-field dirty flags); reliable, persisted intention channels.
7. **Shared net time**, **zone relevance**, **mission state with history** (late join, migration).

## 2. Where bf-coop stands against it
| ACU pillar | bf-coop today | consequence seen this week |
|---|---|---|
| replica runs natively from intentions | the player puppet's AI and behaviour are **off** and it is driven by replaying position + animation (`DisablePuppetAI`) | every engine system that touches the puppet fails one by one: nav service never initialised (PuppetNavService fix), conflict end, despawn, and now **damage events arrive but are never applied** (f7-puppet-damage) |
| guards: AI off on replica | guest guards are animation mirrors and the guest's own AI keeps writing actions on them (GuardActionGate) | phase lock / blend fixes fight symptoms |
| player owns avatar | yes for the local player | the puppet is not a real avatar on the host, so guards fighting it is a stack of host-side workarounds |
| reliable intention channels | `feat/intent-lane` (proto 15) exists, **unmerged, never run in game** | nothing can carry decisions yet |
| identity by creation | guest *borrows* the nearest NPC (`BindRadiusM`) | binding drift, StuckProbe ceiling |
| per-object authority | one global host | guards near a far-away guest have no master |
| events vs state | kill events yes; no player health/death; no hit events | guest death cannot reach the host |

**Verdict.** The design notes already point the right way; the code has drifted the other way: we keep adding
host-side workarounds so engine systems tolerate an AI-less, animation-driven puppet. That cost will keep repeating for
every new system (stealth, chase, assassination, parkour, ships). The F7 work stays useful (it found the seams), but the
next step is to stop extending it.

## 3. New capability this week that unlocks the ACU model
- **World-space locomotion intent on any Human**: `IHumanGround` vtable 0x026E7DC0 slots 0/1 (SetDirection /
  SetSpeedRatio), verified live on Edward: compass pushes within 6 deg, in-process goto within 0.8 m, a 6-leg route
  (docs/evidence/pilot-intent-2026-11-15.md). intention-seam.md notes the same slots work for the puppet if called per frame.
- **Runtime probes without rebuilds** ([Probe], AU-RW tap port) and **an automated master** ([Pilot]: boot, teleport,
  goto, world queries). Together they make the talk's **loopback replica** oracle buildable on one machine: drive the
  master Edward by script, feed its outgoing intentions to a local replica with adjustable lag, measure divergence.
- **AC4's own multiplayer exe** (`AC4BFMP.exe`, same engine) replicates player Humans: `netproxy.cpp`, `NetProxyClass`,
  `S2C_SetMoveReplicationMode`, `M2R_ReplicateEnterCustomActionState` [S]. That is Ubisoft's AC4-era answer to "drive a
  remote Human from decisions". The owner has an **Xbox MP build with PDBs** (Aug 22 2013), which would name it.

## 4. Re-sequenced roadmap (each slice behind a default-0 flag, oracle first)
| # | slice | why first / what it replaces | oracle |
|---|---|---|---|
| R1 | **Verify and merge the intention lane** (proto 15) | everything below sends decisions | net_selftest (exists) + one in-game loopback run |
| R2 | **RE AC4BFMP's Human replication** (move replication mode, custom-action state, NetProxy) | gives the engine-native way to drive a remote Human; may replace our own replay | static (IDA on the MP exe; PDB names if provided) |
| R3 | **Native player replica**: puppet with AI logic off but Human state machine on, locomotion from intents (IHumanGround), position correction with a teleport threshold | replaces animation replay for the body; nav service, ground stage, fight stance and damage handling come back natively | loopback replica: master vs replica position error, ground stage and FightStage sequences |
| R4 | **Player decisions**: actions with targets (attack, assassinate, custom action), parkour/attach targets | the remaining body behaviour | loopback divergence onsets |
| R5 | **Guard replicas native** (guest AI logic off) + spawner-keyed identity | replaces GuardActionGate and nearest-body binding | guard divergence rate, binding stability |
| R6 | **Fights**: defender-authoritative melee carried as intentions (docs/design/guard-fight-sync.md becomes this slice) | with native replicas the guest sees the guard aim at them, indicators and counters natively | hit applied once per landed strike on the guest; counter parity |
| R7 | events and shared state: player health/death, kills both ways, mission channel with history, net time, zone relevance | ACU pillars 6-7 | late join keeps mission state; death parity |

What we stop: new host-side workarounds for the AI-less puppet (keep the verified ones as flags until R3 replaces
them); new animation-layer fixes for guards (keep the crash/float fixes).

## 5. Open questions for the owner
1. Is R2 worth doing before R3, using the Xbox MP PDB (only if you can provide it from a source you own), or start R3 on
   our own seams and use the MP exe only as a reference?
2. Per-object authority now (each player masters NPCs near them), or keep one host for NPCs until R7?
3. Merge order with the other sessions' branches (`feat/intent-lane`, `re/guard-*`).

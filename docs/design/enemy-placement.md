# Enemy placement on the guest (2026-11-16)

Goal: every host guard near either player has exactly one body on the guest. That body stands where the host's guard
stands and moves smoothly. It is never frozen, missing, doubled, teleporting or popping.

Status: slice A is built behind `[Coop] GuardPlace` (default 0). The live result is in
`docs/evidence/enemy-placement-2026-11-16.md`. Stages B-D are designs only.

## 1. How guards reach the guest today (code at 856e912, `src/phase1.cpp`)

**Host**
- `CaptureGuards` sends every BhvGenericNPC human with a health service: soldiers only with `SyncEnemiesOnly`,
  nearest first, within `SyncRadiusM`=60 of the peer, at most `SyncMaxEntities`=32.
- The record holds pos, quat, action alias, anim block, hp, arch and objId, keyed by a stable slot.
- The identity is the HandleContainer objId, a runtime-minted `F000xxxx` value that the guest does not share
  (`s3-guard-identity-2026-11-16.md`).

**Entity lane**
- The lane is delta-encoded. An unchanged entity is omitted from the wire, so a guard standing still goes quiet.
- The snapshot is stamped with the host capture time (`hostMs`, proto 14).

**Guest: `ApplyGuards`**
- Finds a body for each record, in this order:
  1. the sticky slot;
  2. the resolver;
  3. a 1.5 m position scan;
  4. `WideBindPrepass`: the nearest free local NPC of the same archetype within 12 m, assigned one-to-one with
     `BindLock`.
- It takes the body over: behaviour off and drive flag clear, every packet.
- It writes the transform when the record moved, from the thread of the player's update: `SetGlobalMatrixInternal`,
  with approach v3 for gaps of 1.5-30 m.
- The default cadence (`EntityFrameCadence=0`) runs `EntitySyncTick` work every 6th frame. The host sends at most
  every 6th frame, and the guest applies at most every 6th frame. At the rig's frame rate that is the "about 4 Hz"
  per-guard write rate in `seamless-scorecard-2026-11-16.md`.

**Own mode** (`OwnUpdateGuards`, default 0): the writes happen on the body's own update instead (hook in the per-Human
update, `OnFrame` -> `OwnDrain`).

## 2. Failure modes, measured (guest logs of the 2026-11-16 scorecard runs)

| mode | measurement | source |
|---|---|---|
| **missing** | 10-18% of guard records have no body. **Both causes are about equally common:** no free body of that archetype on the guest at all (e.g. `noArchetype=46` of 131 needs), and the nearest one beyond 12 m (`beyondRadius=71`, nearest 12.1-17.7 m) | guest 5020 `[WideBind]` lines |
| **hopping** | 0.4-1.5 m per write | `[Bind] takeover maxStep` |
| between writes the body moves by itself | in 46-51% of frames, mean 0.2-0.5 m, no write involved: own probe `guard frames=6813 moved=3276`, `legacyWrites` ~190 per 5 s | guest 5020 `[Own]` |
| **popping** (own mode) | 15.24 m step; "moved by another mover" 8 times, up to 69 m | `seamless-scorecard` per-frame section |
| **frozen** | a body that ignores writes, e.g. at (109.4,-73.4,2.0); now released after 6 writes | `seamless-scorecard` |
| **doubled** | 0 double links since `BindLock`. Guest-only fighters (local AI, no host guard) were not counted before this work | `[Bind] doubleNow` |

**A code finding on the own-mode pops** (read, not yet proven live):
- `OwnPublish` keeps one `OwnBody` per Human. It never resets that record when the body is unlinked and later linked
  again, possibly to another host guard: the ring and the from/to segment of the earlier link survive.
- When the host guard of the new link is standing still, `changed` is false. The segment is then never re-seeded,
  `OwnSample` returns the old `toPos`, and the next write moves the body there in one frame.
- The "moved" probe compares against the last write of the *earlier* link. A re-link after the body walked under its
  own AI therefore reads as "something moved it 10-69 m".
- This fits the 8 moved events and the 15 m step. It does not exclude an engine mover. Slice A's moved-by-other probe
  logs the context of each event, so that question gets a measured answer.

## 3. AU-RW findings (`C:\Users\Administrator\AU-RW`, all heads fetched 2026-11-16)

**Sources**
- Co-op branches `origin/acu-85-coop-re-plan` (head 1ebe9f5) and `origin/acu-85-coop-core` (head 35fd78a, which was
  not in the local clone).
- `.claude/worktrees` does not exist, and `git worktree list` shows only the main checkout.
- All ACU addresses are ACU.exe and do not transfer.
- The findings below are those documents' own claims, each marked live or static there. I did not re-verify them in
  ACU.exe.

1. **ACU-RE-0075, "universal wire-ID rule" (d879ffc).** `id = CRC32("zenitar_" + Type + "_ID")`, 963 of 963 types.
   - It names **message types**, not entities, so it does not solve guard identity.
2. **ACU-RE-0076/0077/0078, dead-reckoned proxy pool (e20b520, f71f8e8, d113162).** The pool holds up to 300 proxies.
   - **Sender:** it re-sends position and velocity when the error between its prediction and the actual position
     exceeds 0.5 m (5 cm in precise mode), after 4 m of travel, on a stop, or on a heading change of more than 10°.
     It predicts from the quantised values it sent, so both sides extrapolate identically.
   - **Receiver:** `p + v * (now - ts)`, using a host-origin clock that is slewed rather than stepped. The step is
     capped at 4 m (precise) or 6.9 m.
   - **The consumer sets the transform every query, straight from the prediction.** The replica has no other mover.
   - Live check (one PC): median error 0.24 m, 90th percentile 1.57 m.
3. **ACU-RE-0080/0081, intentions (486947d, 382a201).**
   - The owner runs the AI. Every high-level action (94 `NetHumanIntention` types: navigate to a patrol point,
     navigate in a fight, look-at, ...) is broadcast.
   - The replica re-enacts it through the same `StartIntention`, so locomotion and animation run locally.
   - A gate refuses intentions from the replica's own AI.
4. **Ownership hand-over (ACU-RE-0092 obs 18-20 live; 0097/0100/0103 static).**
   - **The numbers in the brief are not 0088-0090:** 0088-0090 are the error code, Storm session and LAN flag.
   - Claim at about 50 m (R-45), release at 55.5-88.8 m (R-40, planar distance, R being a world load distance of
     about 96 m).
   - A nearer peer never takes an entity from an owner that is also inside the radius.
   - When the owner unloads the entity, a peer that still has it loaded takes over within 3 s.
   - **Causer requests, owner decides:** an acquisition request goes to the owner, and the owner answers with a grant
     or a refusal.
   - No state is sent at hand-over: the new owner starts from its replica.
   - Portable implementation: `coop/core/include/acoop/session.hpp` (35fd78a), with Announce/Claim/Grant messages,
     epoch plus lower-peer-id tie-break, and 75 synthetic checks including 20% packet loss.
5. **Identity.**
   - ACU does **not** replicate crowd: each game simulates its own (live census, 289 masters against 9 replicas).
   - Placed entities bind by their data id. Spawned ones are **created from a received record with the replica role**.
   - Runtime ids differ per machine, as ours do.
   - The co-op core's rule: "NetId must be the same value on both machines and never a pointer: placed objects use the
     data id, spawned soldiers something both sides compute (spawner data id + spawn index)."
6. **What a replica runs.**
   - It does not run local AI decisions, damage or life. A hit on a replica becomes a request to the owner.
   - It does run re-enacted intentions, behaviour handlers, regeneration and the replication driver.
   - Fights are separate: `NetFight` is deterministic input lockstep (0102).
   - The replica is its own class (`HumanReplica`, 14 overridden slots).

### What transfers to AC4, and what does not

| AU-RW idea | AC4 today | transfers? |
|---|---|---|
| The transform is set every frame from a prediction, by **one** writer | written per packet (about 4 Hz) from the player's thread, with the body's own update running on another thread at the same time (`[Own] overlap`) | **Yes, slice A.** One writer, on the body's own update, every frame, from a host-timestamped ring |
| No competing local mover | behaviour off and drive flag clear, yet the body moves in about 50% of frames between writes | **Partly.** Slice A measures the mover. Silencing it properly needs AC4's replica gate (stage C, RE) |
| Identity from shared data (spawner id + index); never "nearest body" | nearest same-archetype body within 12 m, `BindLock` | **Yes, but needs RE (stage B).** `SceneSpawner+0xCC` -> entity handle exists (`guard-spawner-id-2026-11-13.md`), but `SpawnerIdProbe` found 0 SceneSpawningComponents in `m_Entities` |
| Spawn replicas from a record instead of conscripting | `GuardSpawnProbe`: 20 of 20 generic NPC clones built and removed; soldiers not tried; `sameComps` 14/20 | **Yes, stage B.** Removes the no-archetype share; needs a soldier spawn route verified in IDA and live |
| Do not replicate crowd; hide or suppress the guest's own crowd | `DisableSpawns` exists (default 0) | **Partly.** Civilians stay local. Guest-only *fighters* are a doubling risk, now measured as `orphansNear` |
| Ownership hand-over by distance with hysteresis (50 / 57 m), owner decides | not built (stage 2 of storm-replication) | **Yes, stage D,** on the reliable lane. `session.hpp` can be ported; it builds for MSVC x86 according to the AU-RW notes (unverified here) |
| Causer requests, owner decides (damage) | already done (owner-decides damage, S9) | already in place |
| Fight lockstep | not applicable: snapshots + forwarded attacks | no |
| Wire-type CRC rule | our own protocol | no |

## 4. Design

Stages, ordered by what is measurable and buildable now:
- **A. Placement (built):** one writer per linked body, every frame, from the ring, with a clean link lifecycle.
  Targets hopping, popping and frozen bodies.
- **A2. Far bind (built, flag):** more bodies for host guards whose nearest same-archetype body is beyond 12 m, only
  when the move happens away from the guest player.
- **B. Identity and proxies (design):** for host guards the guest cannot back with a local body of the same archetype,
  spawn a proxy from a creation record. Bind authored guards by spawner identity.
- **C. Replica gate (RE):** find what moves a taken-over body between our writes, and switch it off for linked bodies.
- **D. Ownership hand-over (design):** the guest owns guards in its own area, with the AU-RW hysteresis.

### A. `[Coop] GuardPlace` (default 0, read once)

**Single writer**
- `GuardPlaceWrite` runs from `OwnDrain` on each linked body's **own** update, every frame.
- `ApplyGuards` calls `OwnPublish` (samples into the ring) and nothing else for that body. It falls back to the legacy
  write only when the body's own update has not run for 300 ms (`OwnStarved`).
- This removes both cross-thread writes and the per-packet hop.

**Target**
- `OwnRingSample` interpolates the host-timestamped ring at `now - offset - GuardInterpDelayMs` (120 ms).
- It extrapolates at most `GuardExtrapMs` (100 ms) past the newest sample, then holds.
- A step of more than 5 m between two samples is a jump: it is held, never glided.

**Link lifecycle**
- An `OwnBody` record carries the host guard id (`objId`).
- **Reset** (empty ring, approach from the body, write clock now) when the id changes or the link was dropped.
  `GuestObjForget` calls `OwnUnlinkEnt`, and the writer leaves an unlinked body alone at once.
- **Re-seed** when the same link was quiet for more than 300 ms.

**Never pop**
- A gap of 30 m or more snaps, with physics, and is logged.
- A gap of more than 1.5 m (or a fresh link) is closed **from where the body is now**, at most
  max(9 m/s, 1.5 × the guard's speed) × frame time, with the frame time capped at 100 ms.
- Otherwise the body is placed on the target.

**Probes**
- *moved-by-other:* at frame start the body is more than 0.5 m from our last write. It is logged with the time since
  our write, the link age, the context id and the drive flag (first 30 lines).
- *stuck:* 30 frames unmoved while more than 0.3 m from our last write and more than 1.5 m from the target. It is
  released through the existing BindStuck path.

**Recommended with it:** `EntityFrameCadence=1` (existing flag, default 0, not verified live before this work). With it
the host sends at a true 20 Hz and the guest applies each snapshot on the frame it arrives. Without it, the ring is
fed at about 4-10 Hz and the interpolation runs 120 ms behind sparse samples, so extrapolation and hold are hit more
often.

### A2. `[Coop] BindFarRadiusM` (default 0) + `BindHideM` (default 35)

- This is a third round of the wide bind, up to `BindFarRadiusM`, for pairs whose body **and** host guard are both at
  least `BindHideM` from the guest player.
- In place mode such a body then snaps (gap of 30 m or more) or walks in (under 30 m) away from the player.
- **It does nothing for guards near the guest.** That is stage B.
- UNVERIFIED: that 35 m hides the move. The game has no camera RE yet. A frustum test would be the right gate.

### B. Proxies from a creation record (not built)

**Why:** the no-archetype share is a guest population difference. No amount of binding radius fixes it. AU-RW and ACU
itself create replicas from a record for spawned NPCs.

**Shape**
- When a host guard has no same-archetype body within R on the guest, the host's record is enough to make a creation
  request: arch, position, heading, and a builder id if stage B's RE finds one that both machines share.
- Request id: a new intent channel (12, reliable lane; 1-10 are taken, 11 is reserved by the brief).
- The guest spawns a proxy at the host position. Its AI is off from birth, and it is pooled: kept and re-used when the
  host guard leaves scope, despawned after 30 s unused.
- A local body that stands at the host position (1.5 m, same archetype) is still preferred, because it has the right
  outfit.

**RE needed before any code**
- (1) A soldier spawn on the guest: `SpawnEntityParams` slot 31 `sub_46D8E0` -> EntityBuilder vt+0x44 `0x5FAC00`
  (`guard-spawn-route-2026-11-12.md`). Only generic NPCs were built live.
- (2) Which builder makes archetype X. Is the builder object loaded on the guest when no instance of X exists?
- (3) The proxy must not register with group AI or fight managers. `AC4BFMP.exe` has
  `CREATE_DUPLICA`/`NetGenericNPC`, which is the place to look (X360 PDB leads, `x360-pdb-results.md` part 2).

**Identity**
- For authored guards, `SceneSpawner+0xCC` -> spawned entity gives (layout owner id, spawner id) as a key both
  machines load from data.
- `SpawnerIdProbe` found no SceneSpawningComponents in `m_Entities`. METHOD: m_Entities walk, vtable 0x026900B8.
  COVERED: m_Entities only. So the registry has to be found statically (callers of slot 36, `0x10462B0`) before the
  key can be used.
- Until then, the binding stays nearest-same-archetype with `BindLock`.

### C. Replica gate (RE, not built)

- In legacy mode a taken-over body moves by itself between writes (about 50% of frames). Slice A's probe names when
  and how far.
- The candidates are the animation's root motion through the character controller, and physics. **UNVERIFIED.**
- Next: in IDA, read what `CharacterController::m_DriveEntity` gates, and which update writes `m_GlobalMatrix` for an
  NPC whose BhvGenericNPC is disabled (xrefs of `SetGlobalMatrixInternal` reached from the per-Human update
  `0x16FA7D0`).
- Then decide: either silence that mover for linked bodies, or keep it (natural feet) and only correct its error,
  ACU's other half.

### D. Ownership hand-over (not built)

- AU-RW numbers: claim under 50 m, release over 57 m, planar distance, the owner arbitrates, epoch plus lower peer id,
  and a hand-off on unload to a peer that already holds a replica.
- On AC4 this means the guest runs the AI of guards near itself and far from the host. The host links them as
  replicas.
- This needs: a guard identity both sides agree on (B), the replica gate (C) on the host side too, and state
  replication in both directions.
- It is the larger change and comes after B and C.

## 5. Challenge (what could make A wrong)

| objection | answer |
|---|---|
| Writing from the body's own update was tried (`OwnUpdateGuards`) and was worse | Partly a lifecycle bug: section 2 shows stale link state is enough to produce those steps. A keeps the hook site and rebuilds the state rules; the moved-by-other probe tells whether an engine mover remains. If it does, A still bounds every write (approach from the body), so the worst case becomes a slide, not a pop |
| The body's update may run before the engine integrates its motion, so our write is overwritten in the same frame | Then the moved-by-other probe reports about 100% of frames with small distances, and the settled error grows. Measured in the run |
| A 120 ms render delay adds error | Yes: about 0.7 m behind a sprinting guard. The bar `place_err_mean_m` (0.35 m) is set over all settled links, and the max bar is 1.5 m. Lower the delay (`GuardInterpDelayMs`, live) if the ring is fed at 20 Hz |
| Static guards go quiet on the wire, so the writer stops after 1 s without samples | The body then stays where it was placed, which is the target. Its behaviour stays off (statue), the same as today |
| Far bind may move bodies the player can see | Gated at 35 m from the guest player, and off by default. UNVERIFIED without a camera test |
| A is not the real fix for missing guards | Correct. The no-body share is unchanged by A. A2 covers only the far band. B is the fix and is RE-bound |

## 6. Test plan and bars

Bars in `tools/scorecard.py` THRESH, committed in 89da594 before the first run:

| metric | bar |
|---|---|
| largest write step of a linked guard (`[Bind] takeover maxStep`, every writer) | ≤ 0.50 m |
| no-body share, alive guards < 30 m from the guest player | ≤ 5% (expected to FAIL until B) |
| no-body share, all alive guards | ≤ 12% |
| settled position error (links ≥ 3 s old, not approaching; body vs host's newest position, at apply) | mean ≤ 0.35 m, max ≤ 1.5 m |
| frozen links (host guard moved > 1 m in ≥ 2 s, body < 0.1 m) | 0 |
| double links | 0 |
| both copies alive | yes |

**Informational:** orphan fighters within 40 m, moved-by-other count and max, stuck releases, snaps.

**Runs:** the same build and the same move script (`two_copy.ps1 -Moves tools/two_copy_moves.py`), with base settings
`NativeReplica=0 FightIntents=1 IntentLane=1 ActMatchProbe=1`:
- baseline: `GuardPlace=0` (today's defaults);
- after: `GuardPlace=1 EntityFrameCadence=1`;
- optional: `+ BindFarRadiusM=40`.

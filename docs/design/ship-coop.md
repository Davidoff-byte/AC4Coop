# Ship co-op: both players aboard one moving ship, either at the helm

Status: design + first implementation, 2026-11-16, branch `feat/ship-coop`. Everything is behind `[Coop] ShipCoop`
(default 0). Live results are in `docs/evidence/ship-coop-2026-11-16.md`; this doc says what is verified, and how.
Supersedes the direction note `shared-ship.md` (which stays as the record of the idea).

## Goal

Host and guest stand on the same ship while it sails. Either can take the helm. Neither screen shows the
following:
- the puppet sliding or popping on deck;
- ship jitter, or a snap when the helm changes hands;
- rubber-banding.

## What already existed, and what it does not solve

| piece | where | why it is not the answer |
|---|---|---|
| ShipMirror / ghost ship | `phase1.cpp` `SpawnGhostShip`, `DuplicateShip`, `[Coop] ShipGhost` (default 0) | A render-only duplicate of the local Jackdaw with a keyframed body, driven by `SetGlobalMatrixInternal`. It carries the peer's hull HP. Nobody can stand on it or steer it, and its physics is a fresh body we built, not the ship's own. |
| `[Ship] passenger` | `phase1.cpp`, `[Coop] ShipPassenger` (default 0) | Places the puppet at one invented point (`kShipPassengerDeckOffset = {0,2,-3}`, "a first guess") with the ship's heading. The peer's real deck position is never sent (`ops/evidence/on-ship-transforms.md` §2). |
| ship identity / naval insert / class index | `ops/evidence/ship-identity-*`, `ship-class-index.md`, `ship-type-entity-field.md` | Reused here for the ship class only (`entity+0x9C` gate). |
| ownership handover (ACU) | `storm-replication.md` | Distance-based take/release for AI objects. Here the handover is triggered by the helm, not by distance. The "causer requests, owner decides" rule is kept. |
| intent lane | `net.h` `PublishIntent` / `PollIntent` | Used as is: channel 11 `IC_HELM`. |

## The idea in one paragraph

**Each PC keeps its own player ship** (both players are Edward, and each has a Jackdaw). The two Jackdaws are kept
at the same world pose:
- **The helm holder's ship is sailed natively.**
- **On the other PC, the mod rewrites that PC's own ship's keyframe target, once per physics step.**
- The engine then hard-keyframes its own Havok hull bodies to the holder's pose.
- So the ship moves with real body velocities, and the local player walking on deck is carried by the engine's
  own character physics, exactly as when Edward walks the deck of a ship he is sailing.

Nothing is teleported, and no body we built is involved. The only thing replaced is one pose argument of one
engine call. Players on deck are sent in the ship's frame. The helm decides who owns the ship transform, and the
host arbitrates.

## RE ground truth (AC4BFSP.exe PC 1.05; IDA 9.3 jobs `D:/ac4ida/ship/jobs/s01..s11`)

All addresses are VAs, imagebase 0x400000. Quoted from the decompile or disassembly of the pinned exe.

1. **Ship physics class chain** (class-info `+0x10` = base id, job s02):
   `ShipPhysicComponent` (vtable `0x272D378`) → `AbstractWatercraftPhysicComponent` (`0x26C6EE0`) →
   `RigidBodyComponent`. The physics driver is `SailManager`, an `AbstractShipPhysicDriver`.
2. **The step.** `sub_1CA3C30` (ShipPhysicComponent vtable slot 127, `+0x1FC`) calls `sub_1C51AD0` first.
   - `sub_1C51AD0` refreshes the component's pose matrix `this+0x540..0x57F` (rows 0-2 basis, row 3 translation)
     from the hull body's own transform: `sub_5B7310(this+0xA0)`, job s10, `movaps [esi+540h..570h]` at
     `0x1C51B19..0x1C51B3A`.
   - The integrator `sub_1C8F6B0` then does the following:
     - reads the hull body's linear velocity (`sub_1C80110`) and angular velocity (`sub_1C801A0`);
     - damps and clamps them (`sub_1C74090`, speed caps at `this+0x120/0x124`);
     - integrates `pos = this+0x570 + dt*v` and `quat = integrate(this+0x540 basis, w, dt)` (`sub_1C744F0`);
     - writes the velocities back to the body (`motion+0xE0` vtable `+64`/`+68`);
     - calls the keyframe function `sub_1C74650`, described in item 3.
   - The alternate step `sub_1C97DD0` (vtable slot 57) reaches the same call (`0x1C984D7`).
3. **The keyframe call**:
   `sub_1C74650(this, __m128* nextPos, __m128* nextQuat, float invDt, __m128* curMatrix)`.
   - Calling convention (disasm job s08): `__thiscall`, 4 stack args, `retn 10h` at `0x1C751FA`.
   - Call sites: `0x1C8FA9A` (args `[ebp-0C0h]` pos, `[ebp-0B0h]` quat, `1/dt`, `esi+540h`) and `0x1C984D7`.
   - For every hull element in `this+0x370` (u16 count at `+0x376`, stride 96), it computes the element's pose
     relative to `curMatrix`, moves it rigidly to `(nextPos, nextQuat)`, and calls
     `sub_C982D0(&pos, &quat, invDt, hkpRigidBody*)`.
   - `sub_C982D0` has the shape of Havok's `applyHardKeyFrame`: body velocity becomes
     `(target - current) * invDt`. **UNVERIFIED:** that this is the Havok function itself and not a look-alike.
     The argument list and the per-body use match; the body was not decompiled.
4. **Consequence.** Rewriting `*nextPos` and `*nextQuat` before the original runs makes the engine itself do two
   things:
   - put the hull and masts where we say, with physically consistent velocities;
   - start the next step from there, because of item 2's refresh from the body.

   Releasing the override is therefore continuous by construction: the native integrator resumes from the pose
   and velocity it finds on the body.
5. **Component → entity.** `*(component+8)` is the owning entity. `sub_1C9DFE0` and `sub_1C97B30` read
   `*(this+8)+0x5C` as the entity's world.
6. **Prologue** at `0x1C74650`:
   - bytes: `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC A8 01 00 00 56 57 89 4D D4 8B 43 0C`;
   - the 36 bytes occur once in `.text` (job s11);
   - 6 bytes are stolen: `push ebx; mov ebx,esp; sub esp,8`. These are complete instructions with no relative
     operand.
7. **Helm context.** PS3 DWARF `ContextID` enum (`ops/evidence/ps3_enums_focus.txt:747`): `ContextID_ShipWheel = 25`,
   `ContextID_ShipDeck = 47`, `ContextID_Ground = 4`. The PC sends 4 on the ground (STATUS), which matches the PS3
   enum. **UNVERIFIED on PC** until the live run logs `ctx -> 25` at the wheel; hence `[Coop] ShipCoopHelmCtx`.
8. **Native leave-the-wheel.** The PS3 enum `NavalOrderType_ForceExitShipWheel = 41` exists. Its PC handler is
   **not found yet** (see Failure cases and "What I would do differently").
9. `ActionPlayerTakeShipWheel` (vtable `0x26EFBC8`; Execute `sub_17591C0`, job s07) only swaps a byte at
   `(entity+0x8F4)+0x402` and restores it on exit. It is a permission flag for scripts, not the take or leave
   mechanism.

### Leads from BF-RW (github.com/rdev-rna/BF-RW `docs/research/SHIP-PHYSICS.md`), received 2026-11-16

- **Same binary.** They state md5 `2058342866688f780c8b34526a65bc35`, the md5 our IDA jobs print. Their label "1.08"
  disagrees with our 1.05 build identity. The hash decides; the label is theirs.
- **Corroborated by our decompile (job s05 of `sub_1CA3C30`).** The force order inside slot 127 is:
  1. buoyancy `0x1C98EE0`;
  2. wind `0x1C9E330`;
  3. drag `0x1C8FE90`;
  4. rudder `0x1C80A80` (called only when `this+0x284 == 0`);
  5. keel roll damper `0x1C806E0`.

  It matches the call order we read. Their step slot 57 = `0x1C97DD0` matches our vtable dump (s02).
- **Verified here (job s13).**
  - `sub_1220960(this, state, instant)` returns for `state > 3` and compares against the current value at
    `this+0xCC`. This is their GcLShip SailState setter (Up 0 / Half 1 / Full 2 / AnimusTravel 3).
  - `0x1CA3BA0` (`retn 4`) copies a driver command block into ShipPhysicComponent `+0x300..+0x326`. This includes
    the dword at `+0x314` and the float at `+0x318`, which they name SailState and steer input.
- **Not yet checked by us (leads, UNVERIFIED):**
  - forward speed at `+0x2C4` (vslot 91);
  - rudder angle at `+0x298` (degrees, + = starboard);
  - sail scale at `+0x320`;
  - NavalCamera update `0x1108C30`;
  - broadside fire `0x11D4A50(ship, side, force)`;
  - naval controller `0x13905A0(player)`.
- **Use.** This is the input lane of §7.1. Replicate the holder's SailState (`sub_1220960` on the follower's
  GcLShip, so the follower's sails show the same state and the new owner inherits it) and rudder. That is the
  minimal state their research names: pos/rot/lin/ang vel + SailState + rudder.

- **Deck collision** (BF-RW `AC4-MODELS.md` §11, UNVERIFIED by us): walkable deck = `Hull_Lower` mesh. Model-space
  heights: main deck 2.6-2.8, quarterdeck 5.1-5.4, poop 6.0. Layer 9 is the wheel; 0x23 are non-walkable damage boxes.
  The design does not need any of it: each player walks on their own ship's real collision, and the puppet takes the
  peer's measured deck position. It is a **sanity check** for the live CSV: a player standing on the main deck should
  log `deckZ` near 2.6-2.8 plus the character-origin offset. That assumes the entity frame's Z equals model-space up,
  which is itself unverified.

## 1. Ship authority

- **Owner of the ship transform = the helm holder.** At start the host owns it (holder = host, epoch 1).
- **Ownership is a number, not a place.** The holder keeps ownership after leaving the wheel. The ship then coasts
  natively on the holder's PC, and the other PC keeps following it. Ownership moves only when the other player
  takes the wheel.
- **The host is the only writer of `(holder, epoch)`.** Every change bumps `epoch`, and every receiver ignores a
  grant with a lower epoch.
- Each PC applies one rule in the keyframe detour, which runs on every physics step:

| local role | what the detour does |
|---|---|
| owner | Passes the native target through unchanged, and records it as the outgoing sample `(pos, quat, QPC ms)`. |
| not owner, peer aboard, owner stream fresh (< 1.5 s), ships within `ShipCoopEngageM` (40 m) | Replaces `nextPos` / `nextQuat` with the owner's pose predicted for now, plus a decaying error offset (§2). |
| anything else | Native. |

**Handover protocol (what each side sends):**

1. The guest's player enters the wheel context (edge `ctx -> ShipCoopHelmCtx`). The guest sends
   `IC_HELM REQUEST {epochSeen, reqSeq}`. It stays a follower until the grant: its wheel input is ignored because
   its keyframes are overridden, so the two ships do not diverge while the request is in flight.
2. The host grants on arrival, last taker wins: `holder = guest, ++epoch`, and sends `IC_HELM GRANT {holder, epoch}`.
   - The intent lane persists the grant and replays it with `IF_BASE` to a rejoining guest.
   - Every ship-lane datagram also carries `(holder, epoch)` as a backup.
3. The host taking the wheel grants itself at once, without a request round trip. Host priority bounds the double
   ownership window to one-way latency.
4. On the grant:
   - **The new owner stops overriding.** Its ship is exactly where the last override put it, with that override's
     body velocity, so native integration continues with no step.
   - **The old owner starts following.** At engagement the error offset is set to `nativePose - target`, so the
     first overridden step equals the native step, and the offset decays over `ShipCoopBlendMs` (400 ms).
   - Neither side snaps, at either end of the handover.

## 2. Smooth ship on the non-helm side

- **Sample stream.**
  - The owner sends at `ShipCoopHz` (30 Hz) on the unreliable `AC4V` datagram (`ShipCoopWire`, 104 B).
  - Each datagram carries the latest native keyframe target (the exact `sub_1C74650` arguments), its QPC
    timestamp, and a velocity differenced from successive steps (EMA 0.3).
- **Clock.**
  - `offMin = min(rxMs - senderSampleMs)` over the stream, with a slow upward drift so it can follow a moving clock.
    It equals the clock offset plus the minimum transit.
  - "Now" on the sender's clock is `localNow - offMin + ShipCoopLeadMs`. On one PC both copies share the QPC, so
    `offMin` is just the transit.
- **Prediction.**
  - Position: `p + v·dt`.
  - Orientation: the last inter-sample rotation raised to `dt/span`. All quaternion algebra is order-consistent and
    never assumes the engine's axis convention; the engine's 4 floats are carried opaquely.
  - Extrapolation is capped at `ShipCoopMaxExtrapMs` (300 ms).
- **No steps from new data.**
  - When a new sample changes the model, the difference between the old and new prediction for the same instant
    goes into the error offset. The output stays continuous.
  - The offset decays as `exp(-dt / ShipCoopBlendMs)`. This is projective-velocity blending: ships are inertial, so
    extrapolation error is small and smooth.
- **Full pose, including heave, roll and pitch, from the owner.** Waves on the two PCs are not proven identical, so
  the follower's hull may sit a few centimetres differently in its local water. That is visual only, and the deck
  frame used for players is the ship's frame on each PC.

## 3. Players on deck, in the ship's frame

- **Sender.** Every send carries the player's position in its own ship's entity frame. In row-vector form:
  `l = (p - t) · Rᵀ`, from `entity+0x10` of both entities. It also carries the player's X axis projected into the
  ship frame (`deckFwd`, x/y).
- **Receiver.** It rebuilds the puppet's world matrix on its own ship, every frame:
  - `world = l · R_local + t_local`;
  - basis = yaw-on-deck × `R_local`.

  It replaces the matrix only, in the existing puppet apply call, before the old `ShipPassenger` branch. Animation
  and context are unchanged.
- **Timing.** The deck pose is interpolated at the newest sample minus `ShipCoopDeckDelayMs` (60 ms, two samples at
  30 Hz), on the sender's send clock.
- **Why this glues the puppet.** The receiver composes it with its own ship's current matrix, so the puppet moves
  with the deck the local player stands on. A ship correction moves both together, and network delay shows up
  only as walking latency, never as sliding. A standing peer stays at a constant deck point regardless of ship
  motion.
- **Assumption, UNVERIFIED.** The deck is about level and up is world Z, as `phase1.cpp` `g_localShipZ = M43` uses
  it. A player on rigging or a ladder gets the right position, but only a yaw facing.

## 4. Helm request and grant

| op | direction | payload `ShipHelmMsg` (16 B) | when |
|---|---|---|---|
| 1 REQUEST | guest → host | `epochSeen, reqSeq, ms` | the guest's player enters the wheel context |
| 2 GRANT | host → guest | `holder, epoch, reqSeq` | the host takes the wheel, or a REQUEST arrives |

- Channel `IC_HELM = 11`, wire id `'SHIPHELM'`. Channels are independent on the intent lane, so helm traffic never
  waits on combat traffic.
- There is no explicit release: leaving the wheel does not change ownership (§1).

## 5. Helm state on the side that lost it

- The loser stays in the wheel context and animation for as long as the engine keeps them there. Their steering
  input is ignored by construction, because their ship's keyframes are overridden. The log line is
  `at the wheel WITHOUT the helm`.
- The ship itself is consistent on both screens. The cost is visual: on the loser's screen, the puppet (the new
  holder) also stands at the wheel.
- **Wanted, not yet built:** issue the engine's own exit, `NavalOrderType_ForceExitShipWheel` (41). Its PC dispatch
  site is not reverse-engineered yet. Until it is, no input is forged on the user's pad.

## 6. Failure cases

| case | behaviour |
|---|---|
| Owner stream lost (> 1.5 s) | The follower releases. Native integration continues from the last keyframed pose and velocity: no snap, and the ship coasts. On recovery the follower re-engages with offset = native - target and blends again. |
| Ships far apart (> `ShipCoopEngageM`) | Follow refuses and logs once every 120 steps. It never drags a ship across the sea; there is no join teleport (see §7). |
| Peer not aboard / we are not aboard | No follow. The deck placement falls back to the normal or ShipPassenger path. |
| Both take the wheel within one RTT | The host serializes: the later arrival wins with a higher epoch. The guest ignores any grant below its epoch. At most RTT/2 of double ownership, absorbed by the blend. |
| Lost grant datagram | The intent lane is reliable, and the grant also rides every ship datagram (`holder`, `helmEpoch`). |
| Guest rejoins | The persisted GRANT is replayed with `IF_BASE`, and the guest adopts holder and epoch. |
| Prologue differs (another build, another hook) | Not installed; logged. `Enabled()` stays false, so nothing else runs. |
| Hook fires for another ship | Pass-through. Only `*(this+8) == our player ship` is touched. |
| Ship asleep (step not running) | Nothing to override. Handling is UNVERIFIED (§7): the live run measures it. |
| Puppet absent | Metrics log `pupOk=0`; nothing else is affected. |

## 7. Challenge: what is weak, and what I would do differently

1. **Two ships pretending to be one** is the main weakness. Each PC's crew, damage, sails, cargo and swivel guns are
   still its own.
   - Hull HP already has a lane (`ShipHPSync`).
   - Sails do not (`ops/evidence/sail-sync-spec.md`). After a handover the new owner's sail state is its own: the
     ship's speed changes physically, without a snap, but it changes.
   - **Better:** replicate the holder's *driver inputs* (sail level, rudder) as well, so the follower's own
     `SailManager` shows the same sails and the new owner starts from the same throttle. The pose override then
     only corrects drift.
2. **Full-pose following** makes the follower's hull ignore its local waves. A "planar" mode (owner x, y and yaw;
   local heave, roll and pitch from the native proposal) would look better in rough sea. The cost is that the deck
   frames differ by the wave difference.
3. **The join is manual.** Both PCs must start co-located: same save at sea, or both sail to the same place. A
   proper "board the host's ship" should instead do three things in the same frame:
   - park the guest's own Jackdaw at the host's ship;
   - move the guest's player by the same rigid transform;
   - engage.

   That needs the ship-teleport and boarding RE (`ActivityTeleportToShipControlPoint`, vtable `0x26BA568`;
   `NavalBoardingGameplayPosition`). I chose not to fake it with a large keyframe jump: one step at 1000 m/s would
   throw the deck crew.
4. **Losing the helm should exit the wheel natively.** Find the `ForceExitShipWheel` (41) order dispatch, from the
   naval order consumer in `GcLShipNavigation` / `SailManager`, and call it on the loser. Until then two Edwards can
   stand at one wheel on the loser's screen.
5. **A sleeping ship is not stepped.** `sub_1C9DFE0` deactivates when `this+0x27C < 0.0005` and clears
   `this+0x364`. If the follower's ship is asleep the detour is never called, and a moving owner would leave it
   behind until something wakes it. The fix is to keep the follower's physics component active (its own wake call)
   while following. That is RE still to do; the live run shows whether it is needed.
6. **Clock.** `offMin` drift handling is crude. For two PCs at 400 ms RTT, the existing `PuppetHostTime` clock
   should be shared with this lane instead.
7. **Authority by distance (ACU) is not used.** It is right for AI ships, wrong for the player ship, where intent is
   explicit: the hand on the wheel.

## Flags (`[Coop]`, all read once at start)

| key | default | meaning |
|---|---|---|
| `ShipCoop` | 0 | master switch: installs the keyframe detour and runs the lane |
| `ShipCoopFollow` | 1 | (with ShipCoop) override the keyframe target when not the owner |
| `ShipCoopDeck` | 1 | (with ShipCoop) place the puppet by the peer's deck pose |
| `ShipCoopHelmCtx` | 25 | context id of the wheel (PS3 `ContextID_ShipWheel`; verify on PC) |
| `ShipCoopBlendMs` | 400 | error-offset decay constant |
| `ShipCoopLeadMs` | 0 | added to `offMin` when predicting now (use about RTT/2 across PCs) |
| `ShipCoopMaxExtrapMs` | 300 | extrapolation cap |
| `ShipCoopDeckDelayMs` | 60 | deck pose interpolation delay |
| `ShipCoopEngageM` | 40 | follow engages only within this distance |
| `ShipCoopHz` | 30 | ship-lane send rate |
| `ShipCoopMetrics` | 0 | `ship_coop_<pid>.csv`: one `K` row per keyframe call, one `F` row per player frame, one `H` row per grant |
| `ShipCoopProbe` | 0 | detour installed but never overrides (observation run) |

Logs: `ship_coop_<pid>.log` next to the ini.

## Code

- `src/ship_coop.{h,cpp}`: detour, lane, filter, helm state machine, deck placement, metrics.
- `src/netstate.h`: `NET_SHIP_MAGIC`, `ShipCoopWire`, `IC_HELM`, `ShipHelmMsg`.
- `src/net.{h,cpp}`: `SendShipCoop` / `DrainShipCoop`.
- `src/phase1.cpp`: frame call, `IC_HELM` routing in the single intent consumer, deck placement before
  ShipPassenger, CFG lines.
- `tools/ship_coop_metrics.py`: the oracle (pass/fail from two CSVs).

# R4.2 climb decision: static leads (in progress), 2026-11-15

Goal (docs/design/player-decisions-r4.md R4.2): find, on PC, where the player's free-climb step is chosen and what the
chosen step's target is, so R4.3 can send it to the replica. Status: **the shimmy decision is identified live on PC** (candidate generator, selection, move build and push; section
"Decision chain"). Climb-up and other move types are not traced yet. Binary: AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA db `D:\ac4ida\a3` (jobs r42a-r42h).

## PS3 DWARF (names and layouts are leads, not PC facts; `ops/tools/dwarf_die.py`, cu 297)
- `scimitar::ClimbingSequencer::CandidateMove` (DIE 0x6FE894A, 240 B): movingLimb / staticLimb (IKLimbEffector),
  moveIntention, **targetedGuidance** (+0x10, TargetedGuidance), initialHandPosition, otherHandPosition,
  distanceBetweenHand, movementGlobalAxis, m_HighPriority, m_Value, localDirection; methods Compare / ctor / operator=.
  (Offsets after +0x10 did not decode with this tool: DW_OP location lists; to be fixed if the offsets are needed.)
- `ClimbingSequencer::TargetedGuidance` (DIE 0x6FE8801, 128 B): +0x00 m_GuidanceType, +0x10 m_Guidance (0x50 B payload),
  +0x60 m_Weight, +0x70 m_BrutePosition (Vector4). The climb target is a guidance item plus a position.
- `ClimbingSequencer::TransitionCandidateMove` (160 B: targetedGuidance, idealPosition, m_HighPriority, m_Value),
  `CandidateCoreMovement` (32 B: CoreMovementDefinition*, m_Value, m_PositionOnGuidance).
- `ClimbingDetectionState` (96 B): cycleType, actor matrix, left/right hand positions, currentDetectedType, HangType.
- Free functions `FetchCandidateMoveForOpen/Close(..., Array<CandidateMove>&, IKLimbEffector, IKLimbEffector,
  const Vector4&, const Vector2&, const ClimbingDetectionState&, bool, bool)` (DIE 0x71C1AC0 / 0x71C1B03): candidate
  generation per hand move ("open" / "close").
- False lead recorded: `GenerateCandidateDeferredRayCastRequests` is `FightManager::RangedWeaponBulletManager` (bullets).

## PC (verified statically)
- Climbing context (ActorContextID 30) is created by `sub_16B7080(Human+0x720)` (lazy at +0x70), case 30 of the context
  factory `sub_16F1670`; ctor `sub_1581770`, 0x30 bytes: main vtable 0x026D4638, interface vtables at +0x10
  (0x026D4588), +0x14 (0x026D4580), +0x18 (0x026D4578), +0x1C (0x026D4564), +0x20 (0x026D455C), +0x24 (0x026D453C),
  +0x28 (0x026D4504); +0x2C = 0.
- Every slot of interface 0x026D4588 starts with `call sub_156ADD0` (getter, role UNVERIFIED); climbing code sits in
  0x0150xxxx-0x015Fxxxx.
- `sub_151CDE0` (object with vtable 0x026D1784, not in the class map) embeds at +0xA0 an object built by `sub_1515260`
  that holds a fixed array of **20 elements of 240 bytes** (loop `v4 += 240`, 20 iterations, then fields at +5012/+5020/
  +5024); its destructor walks the same array backwards (`sub_151A110`). 240 B = PS3 CandidateMove size; that this is the
  candidate array is UNVERIFIED.
- Element-stride scan (0xF0 / 0xA0) over 0x01500000-0x01600000: `sub_1560190` indexes 240-byte elements (`imul edi,0F0h`,
  3.5 KB), `sub_1516910` loops 160-byte elements (13 x `add esi,0A0h`); both are candidates for the generator /
  selection. Not decompiled in full.

## Live climb trace (run 26944, asi bd2d195c, Pilot-driven, `[Probe]` hooks + write watch)
Pilot drove Edward from (37.4,-79.6) into the wall at (36.35,-81.71) with RT + A held (scratch `climbpush.py`, stick angle
from the learned camera offset 57 deg): wall run to z 6.06, then hanging at z 4.86 - the same spot and height as the peer in
host-p14-state-2.tap (contexts 8 -> 30). Stick left/right then shimmied him 1.5 m along the ledge.
- Hooks during the climb: `sub_156ADD0` (climbing getter) 1,252 calls in the climb, 158,145 by the end; `sub_1560190` and
  `sub_1516910` **0** - both static candidates above are not on this climb's path.
- `sub_156ADD0` is a lazy allocator: `*this` = a 0x530-byte block (ctor fields +0x24/+0x4C self-lists); its `this` is
  climbing context + 0x2C (context 0xFD1FFC30, block 0x407F7810 in this run). The block held no world-space vectors.
- **Hand grip points**: Human+0xC20 (36.87,-81.31,7.26) and Human+0xC70 (35.87,-81.30,7.20) - Edward's x +/- 0.5 m at ledge
  height (Human 0x35AA4E10); 0x50-byte records from Human+0xC10 (limb index i at +0x10 + 0x50*i).
- **Ledge guidance chain**: `[Human+0x564]` (0x451786A0) holds points along the ledge at z ~7.25, ~0.82 m apart
  (x 38.10 / 37.29 / 36.46 / 35.65 / 34.83, y -81.31); a second list at `[Human+0x570]`. Role (guidance vertices) UNVERIFIED.
- Write watch on Human+0xC20 (32 B) during a 0.8 s shimmy: 144 writes, all from `sub_48BE60` (36-byte copy: two movaps +
  a float) called by **`sub_1831280(this, limb, target, value)`**: record = this + limb*0x50; copies the target to +0x10,
  stores value at +0x50, sets +0x54 = 1 - the IK limb-target setter. 9 callers; during climbing only **`sub_13CF890`**
  fired (1,752 calls; `sub_169B200` / `sub_170F660` 0), always from 0x013D04CE.
- `sub_13CF890(this, target)`: builds a limb target from its argument (`sub_13B5E70`), ray-casts to the surface
  (`sub_643490`) and on a hit calls `sub_1831280` - per-frame IK contact placement, not the decision.
- Its caller `sub_13CFA60` (0x13CFA60-0x13D1741) is slot 11 of vtable **0x026BCD70** (ctor `sub_13D9690`, dtor
  `sub_13D96E0`; class not in the class map) - a per-frame task update that hands each limb its target. The climb step
  choice feeds it from above.
- Probe fix: `Plausible()` rejected addresses >= 0x7FFF0000, but the exe is large-address-aware and live objects sit above
  2 GB; bound raised to 0xFFFF0000 (built, not yet deployed).
## Decision chain (run 8744, asi 0454741f = 0795f21 + nothing else, probe reads above 2 GB; earlier steps run 26944)
Climb reproduced with the Pilot (stick offset re-learned per attempt: `goto ... --stick` prints it, then RT + A push for
1.6 s), Edward hanging at (35.24,-81.31,4.85); shimmies with stick left/right.
1. **Contact manager** = the 0x026BCD70 object at **Human+0x818** (`sub_169AE70` returns `*(Human+0x818)`). It keeps a ring
   of 4 move records (0x350 B each; base +0xE40, write index +0xE50, read index +0xE54). Slot 18 `sub_13BEC30(limb, out)`
   copies the newest record's guidance target for a hand (record+0x314 -> move object, per-limb table +0x2A8 / count
   +0x2AE, 48-byte targets at +0x2A0); slot 16 **`sub_13D9010`** pops finished records and pushes a new move (write watch on
   +0xE50 during a shimmy: writes only from 0x013D91FA / 0x013D9100, a few per shimmy, not per frame).
2. **Guidance target** (48 B, vtable 0x026BA8A4; ctor `sub_1C11C10`, copy `sub_1C11CC0`): +4 type (11 = none), +0xC index
   (-1 = none), +0x14 ref-counted guidance pointer, +0x20 Vector4 position.
3. **Move object** (vtable 0x0271F7E8; base ctor **`sub_13D5640`**, derived ctor **`sub_1B59600`** adding +0x330/+0x334/+0x338):
   the base ctor fills the per-limb guidance targets (`sub_13CF650(this+0x2A0, this+0x2A8, ...)`) and pushes itself into the
   contact manager (`mgr->vt[16]`). During shimmies every move was created via `sub_1B59600` from **0x01B65599**, i.e. by
   **`sub_1B64320`** (one of 12 move-type creators in 0x1B5xxxx-0x1B6xxxx that call `sub_1B59600`).
4. **`sub_1B64320`** (shimmy move type): `sub_1B509A0(&cands, ...)` = **candidate generator** (verified live: called from
   0x01B64BD6, returns al=1 and 3 candidates per step); a loop over the candidates takes the first one passing a distance
   test (threshold `dword_2A8902C`); `sub_1B633E0` resolves it into per-limb guidance targets; `sub_1B59140` /
   `sub_1B4D650` validate; then the move is allocated (0x340 B) and built with `sub_1B59600`.
5. **Candidates** (32 B each, = PS3 `ClimbingSequencer::CandidateCoreMovement`): +0 `CoreMovementDefinition*`, +4 score,
   +8 = 3, +0x10 position on the guidance. Live, per shimmy step: 3 candidates sorted by score 1.000 / 0.796 / 0.775 (moving
   one way) or 0.998 / 0.795 / 0.774 (the other way), position = the ledge point at Edward's x (z 7.25-7.26).
6. **Movement definitions**: 0x20-byte records in a table (0x2B106D70, +0x20 ...), each starting with an **index**
   (D70 = 1, D90 = 2, DB0 = 3, DD0 = 4, DF0 = 5, E10 = 6, E30 = 7), then floats (+0x0C, +0x10) and a pointer (+0x14).
   One direction used definitions 2 / 4 / 6, the other 1 / 5 / 3.

7. **Names (PS3 DWARF, cu 297, `ClimbingSequencer::CoreMovementDefinition`, DIE 0x6FE8E73, 32 B)** - same layout as the PC
   records: +0 `m_CycleType`, +4/+8 left/right `HandRestriction`, +0x0C idealStepDistance, +0x10 maxStepDistance, +0x14
   `m_CycleDefinitionList` (array ptr + count/cap), +0x1C `m_SwingMode`. `CoreMovementDefinition::Type`: 1 HorizontalLeft,
   2 HorizontalRight, 3 DiagonalUpLeft, 4 DiagonalUpRight, 5 DiagonalDownLeft, 6 DiagonalDownRight, 7/8 VerticalUp/Down_60,
   9/10 VerticalUp/Down_90, 11-16 VerticalUp/Down_VCrack{Center,LeftSide,RightSide}, 17-20 diagonal VCrack variants,
   21/22 VerticalUp/Down_120. The live candidates match: one shimmy direction offered 2/4/6 (HorizontalRight, DiagonalUpRight,
   DiagonalDownRight), the other 1/5/3 (HorizontalLeft, DiagonalDownLeft, DiagonalUpLeft), best first. Guidance types
   (`TargetedGuidance::Type`): 1 HorizontalLedge, 2 HorizontalPole, 3/4 VerticalCrackLeft/Right, 5 HangFork.
   `HandsConfiguration::Type`: 1 OneHandLeft ... 3 Closed, 4 OpenedHorizontal, 5-8 opened diagonal/vertical.

8. **Initial grab** (wall run -> hang, run 8744): moves were built by the base ctor from **0x015686E8** (`sub_15680C0`, climbing
   context band) and **0x01B620F9** (`sub_1B61C40`), not by the shimmy creator. `sub_15680C0` loops over 44-byte entries of an
   array at `a3+0xA4` (+0 id passed to `sub_652220` / `sub_1C34200`, +0x10 type 1-7 mapped to the move kind, +0x18 / +0x20
   per-limb data) - a data list of contacts carried by the playing action (source of `a3` not traced). The mantle onto the
   ledge top (stick up) created no climb move and no new candidates: a different mechanism (context transition, UNVERIFIED).
9. **Guidance identity** (Edward hanging at (34.87,-81.31,4.87)): newest record -> move 0x45393620 -> per-limb items (limb 0 ->
   target index 1, limb 1 -> index 2; sub-item type 2) -> 48-byte targets: type 0, `+8 = 0x07831600`, index -1, and a
   ref-counted object at +0x14 (0x34326980, vtable 0x01E5C448, 0xE0 bytes, ctor `sub_6577A0`, built by `sub_524800` /
   `sub_524980` / `sub_524B00`). That object holds the **grab point** at +0x70 (34.87,-81.31,7.27 = the ledge at Edward's x),
   `+0x60 = 0x53B5`, and at +0x10 the **handle of the climbed entity** (ctor copies `a4[50]` = Entity+0xC8). Live: handle
   container 0x3074FA74 = {object 0x34A54C10 (Entity, vt 0x01E4CE90, at (37.0,-84.0,3.6)), refs, **ObjectID
   0x0000000A0D0158CD**}. A world-data ObjectID is the same in both games, so a climb target can be named as (climbed entity
   ObjectID, guidance element, position). Over three shimmy steps (`grabrec.py`, scratch): `+0x60` rose 0x7115 -> 0x7172 -> 0x71CE (a per-move counter/time, not an
   element id); `+8` changed 0x07831600 -> 0x01124100 when the hands moved to new targets (packed element id? UNVERIFIED);
   the entity ObjectID stayed 0x0000000A0D0158CD and the grab point followed Edward along the ledge (34.87 -> 34.20).

10. **Climbing context chart** (run 8744, hooks on vtable 0x026D4638 slots 11-16 and the state-entry dispatcher):
    slot 12 `sub_1583760` = **Enter** (called from 0x0052E025, the engine's context transition, with context id 0x1E):
    allocates the 0x530 data block (`sub_156ADD0(this+0x2C)`), pushes state 1 and runs state entries (`sub_1581910(state)`,
    22 states) through default transitions until a stable state (bitmask `byte_2992A90`); Enter takes no target. Slot 14
    `sub_1577B20` = **Exit** (B drop; called from 0x0052C34B). Slot 15 = per-frame dispatch (`sub_1583780` ->
    `sub_1581EE0` transition evaluation). Recorded sequences: drop = EXIT, (brief re-grab ENTER 12 13 EXIT); wall-run grab =
    ENTER, states 1, 11 (state 11 sets data+0x98 / +0x91 = 1). States 20 / 21 (`sub_1574EB0` / `sub_1570EE0`) are callers of
    the move base ctor.
11. **Where the shimmy decision runs** (static xref chain, every level has one caller): `sub_1581EE0` (chart transitions)
    -> `sub_156FE40` -> `sub_156B840` (per-frame climbing update: capsule radius/height adjustments, then the sequencer) ->
    **`sub_17E8520`** (method of the 0x1C0-byte object built by `sub_17E8500` off the climbing block, = ClimbingSequencer,
    UNVERIFIED name) -> `sub_1B66C10` -> `sub_1B66310` -> `sub_1B64320` (shimmy) / `sub_1B65920` -> `sub_1B61C40`. So the
    decision runs inside the Climbing context itself, not in the player's behaviour task (an earlier hypothesis, refuted).
    The sequencer runs only when byte `+0x90` of the climbing scope is set; it is told `+0x91` && `[scope+8]+0x14 != 0`.
12. **Commands**: interface 0x026D4588 slots are requests that post chart events: `sub_59C660(id)` = PostEvent (appends to
    the queue at +0x20 unless queued). Slot 0 `sub_156F1E0(a2, a3)`: in state 11 and if the sequencer's +0xD0 is set,
    `sub_1567A60` computes a target (`sub_16EC080`) and posts event 15; slot 2 `sub_156F260`: in state 11, posts event 10.
    These are the climbing analog of IHumanGround - a native command seam usable on a replica (UNVERIFIED in game).
13. **Input seam** (write watch on Human+0x1A4C while hanging and holding the stick, run 8744): all writes come from
    `sub_1537F30` = **climbing interface (vtable 0x026D4588) slot 37** (`data = [this-4]; data+0x0C = v.x; data+0x10 = v.y`:
    **SetDirection2D(Vector2*)**); slot 38 `sub_152B0C0` = **SetSpeedRatio(float)**, clamped 0..1 -> data+0x14 (the field the
    sequencer gate tests). data = Human+0x1A40 in this run. Callers: `sub_113DAC0` every update - first slot 37 (0,0) and
    slot 38 1.0 (0x0113E1B5, 61 hits), then the real input (0x0113F96B, 36 hits): v = (in.x * (+/-1 mirror), in.y) from
    locals [ebp-0xBC] / [ebp-0xB8]. `sub_113DAC0` is registered by `sub_113FA00` via `sub_112D810(2000, ...)` on the object
    returned by `sub_1AD9510` (the player behaviour task accessor family, as the ground handlers registered by
    `sub_1AF8390`) - the player's climbing handler (role consistent, not proven). So climbing mirrors the ground exactly:
    player handlers feed IHumanGround slots 0/1 on the ground and the climbing interface slots 37/38 while climbing, and the
    Climbing context's own sequencer turns that intention into moves.

14. **Seam verified live** (run 32012, asi a3decf69, `[Pilot] ClimbIntent` hooks on vtable 0x026D4588 slots 37/38 for the
    player entity, `tools/pilot.py climb x y --sec 1`): Edward hanging at (34.66,-81.31,4.87), pad neutral throughout.
    Control (no intent, 1 s): no movement. Intent x = +1: 34.66 -> 34.07 m along the ledge; x = -1: 34.01 -> 34.49; x = -1:
    34.62 -> 36.19. Substitutions ~66/s on each slot (every update). So the 2D climbing intention alone makes the native
    sequencer choose and play shimmy moves - the replica-side mechanism R4.3 needs (on the player here; replica untested).
15. **Entry**: transitions requested for Edward (hook on `sub_52DFF0(ctxId)`, the enter half of TransitToContext: main track
    `this+0x100`, factory `[track+0x28]->vt[8]`, then context Enter at +0x30): drop = 6 from 0x01583840 (climbing dispatcher),
    4 from 0x01DC6EF0; wall-run grab = **8 from 0x01D65A11** (ground statechart band, near the event dispatcher
    `sub_1D65BB0`) then **30 from 0x01756D5A** (Walling code). The ground -> wall-run choice is made by the native ground
    chart; whether it can be driven by intention on a replica is open.

16. **Entry by intention, on Edward** (run 32012, scratch `intentclimb.py`: Pilot ground intent cmd 9 toward the wall
    plus pad state): (b) intent only, stick neutral, no buttons -> walked into the wall, no climb; (a) intent + RT + A,
    stick neutral -> did not move (with free-run held and no stick the handler does not call the ground slots, so the
    intent hook never fires); (c) intent + RT + A + stick held (direction replaced by the intent) -> wall run and climb onto
    the top (max dz +2.05, end z 7.02). Player-filtered hooks on IHumanGround (vtable 0x026E7DC0) slot 3 `0x01D0DA00`
    (SetMovementType) and slot 32 `0x016D8960` (SetSprint), entity snapshot `[[[ecx-0xC]+0x108]+4]`: walking -> slot 32
    arg low byte 0, slot 3 arg 0; RT + A running -> slot 32 low byte 1, slot 3 arg **1**; both from the player handler
    (0x01AEF9C7 / 0x01AEF9EF). So the free-run state reaches the ground context as movement type 1 + sprint, both
    callable on a replica's IHumanGround; the sender already transmits `destMvt` (HumanGroundData+0x125, written by
    slot 3) and `sprint`. Whether an NPC replica's ground chart then wall-runs is untested.

17. **Free-run intention on the NPC replica - negative** (run 22432, asi 63aea3a7, `[Coop] NativeReplicaFreeRun=1` with
    NativeReplica=1 / NativeReplicaParkour=1; replica's IHumanGround slots 3 (peer destMvt), 32 (peer sprint), 1 (speed),
    0 (direction to target) called on every replica frame from the puppet's own update; host-p14-state-2 x3): the replica's
    own context stayed 4 on every window (repCtx 0x10) while the peer went 6 / 9 / 8 / 30; 8 snaps (3 parkour), no crash.
    Caveat from the tap: the recorded peer only carries sprint = 1 / destMvt = 1 from t = 23.0 s (its jump) onward - during
    the run-up both are 0 - so the replica had the free-run state on the ground for ~0.4 s before the peer went to Walling.
    Consistent with R3 (NPC locomotion ignores IHumanGround). Next lead: the ground chart's transitions go through
    `sub_1D659D0(state)` (280 callers; the argument is a state of the 644-state ground chart, not a context id); naming
    those states (PS3 DWARF) and filtering to the player's chart would give the wall-run transition and its condition.
    Checked: PS3 enum `HumanGroundEntryPointID` (cu 289, DIE 0x62200C1, 811 values; e.g. 276 GrabbedFromClimb_Standard,
    724 WallingImpulsion_Standard) does not match the sampled `sub_1D659D0` arguments (648 / 652 / 657 = DropHidingSpot /
    CarryObject..., and values up to 53252) - either a different id space or NPC calls (no player filter yet).

18. **Wall-run entry seam** (static + live):
    - `sub_1D659D0(code)` decodes a transition: low 10 bits = target; < 0x284 (644) a ground-chart state, >= 644 a context
      transition to (target - 644) via `sub_52DFF0` (so 652 = Walling 8, 648 = Ground 4).
    - The Walling transition is taken in `sub_1D6B3E0` (= **IHumanGround slot 22**, via `sub_1D7BDE0`) when a Walling event
      is queued (`sub_15DFE50`): event 127 in ground sub-state 4 (223 / 241 / 237 in sub-states 67 / 76 / 75); it installs
      the Walling entry function at chart+0x120, calls it with (a2, a3, a4) and requests 652.
    - All five events are posted only by `sub_1D5D920` (= **IHumanGround slot 21** via `sub_1D5E5C0`): in sub-state 4,
      if `sub_1CE0A40` (flags bits 6-11 of the current action, selected by scope +0x424 / +0x42C) then `sub_1CED250`; a
      result of 2 posts 127.
    - Player: the free-run handler `sub_1AEE4F0` (11.5 KB, prologue found by scan; no boundary in the idb before) calls
      slot 21 twice per try (0x01AF121B mode 0, 0x01AF1255 mode 1) with (out 28 B, out 16 B, &handle, &mode, &direction),
      and on success slot 22 (out 28 B, out 16 B, byte task-scope+0x2205). Live (run 22432): 98 slot-21 calls for the
      player only, 11 returned 1.
19. **Replica enters Walling and Climbing natively** (run 27100, asi 6bdb586c, NativeReplica / Parkour / FreeRun = 1,
    host-p14-state-2 x4): while the peer's newest packet says context 8 or 30 and the replica is on the ground, the
    replica's own IHumanGround slot 21 (mode 0 and 1, direction to the peer) and on success slot 22 (buffers from slot 21,
    flag 0). Result: six applies logged `slot 22 -> replica ctx now 8`; windows show the replica's own contexts {4, 8, 30}
    (3D error mean 0.61 m, max 1.43 in the first pass) and {9, 30} - the replica wall-ran and grabbed with a target produced
    by its own engine (the crash class of entering 30 without a target did not occur; no crash). Defect: after it climbed,
    the pass-reset teleport did not hold and repeated every frame (132 snaps).
20. **Exits are the open problem** (same flags, host-p14-state-2):
    | run (pid) | asi | snap handling | result |
    |---|---|---|---|
    | 32368 | 3bff907d | ExitCurrentContext (0x0052C330, exit half only) on the player thread, 250 ms limit | replica stayed in context 9 (beam) and was pulled back after each teleport; 64 snaps, no crash |
    | 32668 | 9601f2d5 | TransitToContext(4) (0x0052DF90) on the player thread | first exit worked (30 -> 4, teleport held); then crash: write fault 0x0059AB4F (state-stack push) under 0x0052DFE6 - cross-thread chart change (UNVERIFIED cause) |
    | 33576 | c9db19e7 | TransitToContext(4) deferred to the puppet's own update | crash in the nav location update: 0x0179CDF0 -> 0x01793290 -> 0x00624440 -> 0x00411EE0 swaps a null handle at [[[svc+0x84]+4]+0x40] (asi frames = the PuppetNavService tick) |
    | 8812 | d091acb3 | + guard on [[svc+0x84]+0x40] (wrong field, null normally) | navigation blocked from the start; no applies, no crash |
    | 33432 | d86705b2 | guard corrected to [[[svc+0x84]+4]+0x40] | two applies -> context 8, then after a deferred exit from context 9: crash, read fault 0x009113BF (null+8) on an engine thread, no asi frame |
    | 16212 | 40dc5daa | no forced exit: snap deferred while the replica is off the ground; in Climbing (30) ask for a native drop (climbing slot 2 / event 10), otherwise wait | **5 passes, no crash, no EXCEPTION**: 9 native entries (`slot 22 -> replica ctx now 8`); per pass the replica wall-ran (8), climbed (30), went onto the beam (9) like the peer and later dropped back to the ground by itself (6 -> 4), after which the deferred pass-reset snap held; 3D error during the climbs 0.6-2.2 m (window means) |
    Reading: entering Walling / Climbing through slots 21/22 is native and works; forcing the replica OUT of a beam / climb
    context (TransitToContext(4)) is not safe even on its own thread; leaving it to exit natively was stable in one 5-pass
    run. The drop request never ran in run 16212 (the replica was on the beam, context 9, every time a snap was due), so
    climbing slot 2 as "drop" is untested. Still open: following the peer's moves while climbing (climbing intention seam on
    the replica), beam exits, more taps, two-machine.

So a shimmy step is fully described by **(definition index, position on the guidance)** plus the guidance it lies on; that
is what R4.3 would send. UNVERIFIED: that the definition table is identical in both games (same assets, so expected), how
the replica side reaches the table (the generator's inputs), and the other 11 move types (climb up, jump, ...).

## Next
1. How the generator reaches the definition table (its arguments; owner object of the table) - needed to resolve an index
   on the replica side.
2. Trace the other move-type creators (climb up, drop, jump-across) the same way (hook `sub_1B59600`, read the return
   address, hook that creator's generator).
3. R4.3 design: publish (move type, definition index, guidance reference + position) on the intention lane; on the replica,
   build the same move through the same creator path.

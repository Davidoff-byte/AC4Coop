# Native replica: the peer's climbing moves, hold by hold (2026-11-15)

Flags: `[Coop] NativeReplicaClimbIntent` (sender + receiver, default 0) and `[Coop] NativeReplicaClimbMoves` (receiver,
default 0, re-read every 2 s). Solo replay only; not two-machine tested.

## What a move is on the wire (RE + live)
- Live trace on Edward (run 33660, asi 3b175a43, `[Probe]` hooks on the move ctors `sub_13D5640` (base) and
  `sub_1B59600` (derived), Pilot climb script right/up/left/up/right/left/down): 31 moves, creators by return address -
  initial grab `0x015686E8` and `0x01B620F9`; shimmy `0x01B65599` (`sub_1B64320`, most moves); corner turn `0x01B5DDBA`
  (the move's orientation quaternion, base-ctor arg 1, turns from (0,0,1,0) to (0,0,-0.71,0.71)); `0x01B61B4D` (vertical
  step: the up step 4.85 -> 6.75 m and the down step). Every move was pushed into the player's own contact manager
  (base-ctor arg 9 = `*(Human+0x818)`).
- Content: `ReadClimbMove` (src/phase1.cpp) reads the newest move of a Human's contact manager - limb 0 / 1 (hands) grab
  points and the climbed entity's ObjectID (offsets and sources in the code comment; chain verified live with scratch
  grabrec.py on Edward: 4 limb targets, hands at ledge height 0.8 m apart, feet 1.3 m lower, ObjectID
  0x0000000A0D0158CD).

## What changed
- `ClimbBlock` (PlayerState context block, contextId 30) gains `moveSeq`, `hand[2][3]`, `objectId` (still inside the
  128-byte union; `sizeof(PlayerState)` 576, no protocol bump). Sender: `moveSeq` increments when the newest move
  (pointer or ring index) changes; holds re-read every frame.
- Receiver: each new `moveSeq` appends the hand midpoint to a 32-entry hold queue (the peer's hold path). On the replica's
  own update while both climb, with `NativeReplicaClimbMoves=1`: replica position = hand midpoint of its own newest move;
  the queued hold nearest to it (oldest of near-equal ones) marks the replica's place on the peer's path, older holds are
  dropped; target = next hold if the replica is on the nearest one (< 0.4 m), else the nearest hold; direction (along row
  0 / vertical) through climbing slots 37/38. Otherwise the previous behaviour (peer's real input, then error-derived).

## Test (solo replay)
- Build tested: asi sha256 `78aa82ba51b815fc3dde6288a3ea16961a332e050be5b815121fac5a1db4e8cc` (commit 5290bc0 + this diff,
  tree dirty), pid 18096, AC4BFSP.exe 1.05; ini NativeReplica / Parkour / FreeRun / ClimbIntent = 1.
- Recording `host-climbmoves-1.tap` (85 s, recorded with asi 9a0cc8b4, the first version of this diff; the sender part is
  unchanged since): the Pilot climb script on Edward. `tools/oneoff/tapmoves.py`: 33 moves (plus 8 garbage rows, see the sender defect below), hold midpoints along the
  ledge (0.25-0.5 m steps), round the corner and back, all on ObjectID 0x0000000A0D0158CD.
- Six replays in one game session, alternating the flag (`tools/oneoff/abmoves.sh 18096 <tap> 165 1 0 1 0 1 0`,
  2 passes each, Edward parked at (80, -76)); metrics by `tools/oneoff/climb_intent_summary.py`:

| run | moves | on-wall frames | on-wall mean error | hold-following frames | holds reached | parkour err3d | snaps |
|---|---|---|---|---|---|---|---|
| 1 | on | 1360 | 0.94 m | 831 | 5 | 2.90 m | 12 |
| 2 | off | 1459 | 0.96 m | - | - | 2.90 m | 11 |
| 3 | on | 1572 | 0.91 m | 964 | 3 | 2.58 m | 12 |
| 4 | off | 1564 | 1.07 m | - | - | 2.65 m | 13 |
| 5 | on | 1572 | 0.90 m | 946 | 4 | 2.54 m | 12 |
| 6 | off | 1498 | 0.99 m | - | - | 2.88 m | 12 |

  On: 0.92 m mean (0.90-0.94); off (peer's real input): 1.01 m (0.96-1.07). Every on run is below every off run, by
  0.02-0.17 m. Parkour 3D error 2.67 vs 2.81 m (overlapping ranges). Snaps unchanged. No crash, no EXCEPTION in 6 runs.
- An earlier version (asi 9a0cc8b4: 8-entry queue, "reached within 0.4 m" only) barely engaged - 125 hold frames in a run,
  130 of 163 holds dropped by overflow (queue filled while the replica was still on the ground); replaced by the path
  following above. Its one usable on run gave 1.48 m vs 1.71 / 1.61 m off.

- Sender defect found in `host-climbmoves-1.tap`: the context block is not cleared per packet, so the first 8 climbing
  packets (before the first move was read) carried leftover bytes as `moveSeq` / holds (huge coordinates; the receiver's
  6 m gate kept them from steering, but they entered the queue). Fixed in the committed build
  `048a2d96898b11e225867e2e9f3809503326e7562f75345503ab2a744a513a2b`: the sender zeroes the move fields, the receiver
  ignores holds with implausible coordinates. Verified (pid 29656, `host-climbmoves-2.tap`, 40 s): 18 packets with
  `moveSeq` 0 before the first move, then 14 clean moves, no implausible values. The A/B table above was run before this
  fix; the receiver's plausibility check is build-only (no garbage reached it after the sender fix).

## Result
- The peer's moves cross the wire and the replica follows the peer's hold path through its own sequencer: on the wall it
  tracks about **9 % closer** than with the real input (0.92 vs 1.01 m, 3 runs each, consistent ordering).
- The route still diverges: the replica lands on a peer hold (< 0.4 m) only 3-5 times per run, and the overall parkour
  error and snap count are unchanged. Open, not root-caused: `ReadClimbMove` failed on the replica in ~35 % of its
  climbing frames (`noread` 510-588 per on run - not both hand targets readable, or another move layout); the replica
  starts its climb from its own wall-run contact, not the peer's.
- Next: (1) find why `ReadClimbMove` fails on the replica (dump the failing step); (2) make the replica pick the peer's
  candidate inside the shimmy creator (`sub_1B64320` candidate loop: choose the candidate whose position on the guidance is
  nearest the peer's hold) instead of steering by direction; (3) align the climb entry (same first hold as the peer).

## Candidate choice in the shimmy creator (`[Coop] NativeReplicaClimbChoice`, default 0) - negative
- Seam (static, IDA): generator `sub_1B509A0` is `__thiscall` (ecx = the shimmy creator's `this`, 7 stack args, `retn 1Ch`
  at 0x01B50CC2 / 0x01B50CCF; only caller 0x01B64BD1 in `sub_1B64320`); it reads the movement definitions from a global
  table (`dword_4E38B10`, count `word_4E38B16`: same data in both games) and returns a sorted array (+0 pointer, +6 u16
  count) of 32-byte candidates (+0x10 position). The creator takes the first candidate passing its direction test
  (threshold `dword_2A8902C`, loop at 0x01B64BF0); its Human is `*(this+0xC)` (0x01B6436C). The hook moves the candidate
  nearest either hand of the peer's target move to the front, on the replica's calls only.
- Run (asi `c7eaefe130e3f62e45e86037b9752da441b90a2e767eba2f4eebb4f0da5185b0`, pid 30680, `host-climbmoves-1.tap`,
  `abmoves.sh ... 1:1 1:0 1:1 1:0 1:1 1:0` = moves+choice vs moves only): the hook installed and ran (467 generator calls),
  but only 19 were the replica's, and in all 19 the nearest candidate was already first (`moved=0`). On-wall error
  0.97 / 0.92 / 1.06 m (choice) vs 0.93 / 0.95 m (moves only; run 6 never reached the wall - the replica got one wall-run
  entry - as in run 4 of the first A/B; a harness/state problem seen in both modes, excluded). No crash.
- Reading: the replica's own top candidate already matches the peer's hold; the divergence is not in the shimmy choice.
- Same build, why `ReadClimbMove` fails on the replica (failing step, 3 runs with moves on): step 5 (newest move's vtable
  is not 0x0271F7E8) ~80 %, step 9 (one hand target only) ~15 %, step 6 (limb count) ~4 %.

## Replica between moves; hold-following re-measured
- The step-5 "other vtables" were heap addresses (run 25716: 0x40547818, 0x41655C08, ...): the newest record's move had
  finished and its memory was reused - the replica was between moves. `ReadClimbMoveStep` now returns -1 with the
  Human's last complete hand pair in that case (and fills a hand a one-hand move does not target from that pair;
  UNVERIFIED that the other hand stays put). Failed reads fell from ~500 to ~45 per run.
- Re-run (asi `77cbc2aef4f569281e9660326331d3b99291d8c5309ba1cb827ce4c35635d04e`, pid 27916, `abmoves.sh ... 1 0 1 0 1 0`):
  on-wall error moves 0.95 / 1.15 / 1.14 m vs real input 0.98 / 1.10 / 1.01 m. With the replica's position known more
  often, hold-following is not better than the real input; the earlier 0.92 vs 1.01 m edge is within run-to-run noise
  (~0.1 m). Climbing steering is at its limit; the divergence is upstream.

## Beam steering (`[Coop] NativeReplicaBeam`, default 0) - negative
- NarrowObject context (9) = `*(Human+0x720+0x3C)` (getter `sub_16E8EF0`, factory case 9; ctor `sub_16D8EC0`: main vtable
  0x026E9BD8, interface 0x026E9A40 at +0x10). Interface slot 0 `sub_1578490` stores a Vector4 at [fsm data]+0xC0, slot 1
  `sub_1578510` a value at +0x19C - the same pattern as IHumanGround slot 1 `sub_1D51690` (lazy FSM, then store).
- Test (asi `5284b637262caa628b2579bae588d0905325de639e51fb0a4aa2c170f434e10c`, pid 22336, `abmoves.sh ... 0:0:1 0:0:0` x3):
  the steer ran ~3100 frames per run. Windows with the replica on a beam the peer is not on: 49 / 48 / 50 % (steer) vs
  51 / 55 / 27 % (off); distance moved there 35 / 30 / 34 m vs 42 / 46 / 23 m; parkour error 2.79 / 2.78 / 2.81 vs
  2.88 / 2.85 / 2.61 m. No effect: like the ground context with IHumanGround (R3), the NPC's beam context does not move on
  these slots.
- Main finding: in about half of all 2 s windows the replica is on a beam the peer never used. The route splits at the
  wall-run entry, not in the climbing moves.

## Why the replica ends on the beam: the stuck snap (`[Coop] NativeReplicaClimbNoSnap`, default 0)
- Wall-entry alignment first (`[Coop] NativeReplicaWallEntry`, default 0): the peer's wall-run start = its last ground
  position and travel direction when its context went ground / InAir -> Walling (8); while the start is < 5 s old the
  replica (on the ground) navigates there at ChaseHigh and requests its wall run (slots 21/22) only within 1 m of it, in
  the peer's direction. Probe run (asi 36d73806, pid 17976, 2 passes): 2 starts recorded, 391 tries from the start - and
  still 54 % of windows with the replica on a beam alone.
- The context sequence of that run showed why: every `[parkour, stuck 4 s]` snap (3D error > 2.5 m for 4 s) teleported the
  replica from the ground to the peer's hanging spot in mid-air (snaps #2-#10 -> (33.6,-82.9,5.3) / (34.7,-85.3,6.6)); it
  fell (6) and landed on the beam below (9). The native drop requests in that run all came while the replica was already
  on the beam and did nothing (slot 2 returned -1 outside Climbing).
- `NativeReplicaClimbNoSnap`: no stuck snap while the peer is on a wall (contexts 7 / 8 / 30 / 31); the far snap
  (`NativeReplicaParkourSnapCm`, 8 m) stays. Test (asi `80d372ee8dd8f88c9e917913095bfd4905054e0a519ca2616032b3d5ac2bd117`
  = the committed build, pid 22476, `abmoves.sh ... 0:0:0:1:1 0:0:0:0:0` x3, `tools/oneoff/beamalone.py`):

| run | entry + no stuck snap | beam alone | both climbing | wall-run entries | parkour err3d | snaps |
|---|---|---|---|---|---|---|
| 1 | on | 5 % | 7 % | 4 | 2.97 m | 2 |
| 2 | off | 51 % | 29 % | 14 | 2.85 m | 12 |
| 3 | on | 7 % | 10 % | 4 | 3.36 m | 2 |
| 4 | off | 51 % | 27 % | 15 | 2.83 m | 12 |
| 5 | on | 5 % | 6 % | 3 | 2.85 m | 2 |
| 6 | off | 51 % | 33 % | 13 | 2.64 m | 13 |

- Reading: the stuck snaps cause the beam landings (51 % -> 5-7 %), but they are also how the replica got onto the wall
  at all: without them it climbs with the peer in 6-10 % of windows instead of 27-33 %, and the overall 3D error is not
  better. In a no-snap pass the replica reaches the peer's start, but its tries from there fail for ~6 s; the entry that
  succeeds is the 5 s fallback from elsewhere, its wall run misses the ledge, it falls and then stands at the wall foot.
- Run-up attempt (asi 6bc536d6, pid 31524, reverted, not committed): go 3 m behind the start, then run through it, try
  only while moving toward the wall > 2 m/s. Zero tries: the run-through target lies in the wall, navigation routes around
  it and the replica moved at 1.3-1.5 m/s (diagnostic log). An NPC cannot build a run-up into a wall by navigation.
- Open: why the replica's wall-run request fails at the peer's start (precondition `sub_1CE0A40` on the current action's
  flags is the lead - a running/sprinting action may be required), and a safe way to put a replica onto a wall (an
  attach/grab, not a teleport into the air).
- Precondition, static (IDA, `sub_1D5D920` = IHumanGround slot 21 body, ground sub-state `*(*(chart+0x118)+6)` == 4):
  `sub_1CE0A40(scope = *(chart+0xC))` reads the flag word of the current action (`*(sub_667230(0,0)+0x14)`, after
  `sub_5FC110(*(*scope+0x108))`); bit 5 refuses; otherwise one bit of 6-11 decides, chosen by scope+0x424 (0 / 1 / 2) and
  scope+0x42C (0 / 1): (0,0) bit 6, (0,1) bit 7, (1,0) bit 8, (1,1) bit 10, (2,0) bit 9, (2,1) bit 11. Only then the
  wall/geometry test `sub_1CED250` runs (result 2 posts event 127). So the request needs the action being played to
  carry a wall-run flag. UNVERIFIED: that only run/sprint actions carry these bits, and what +0x424 / +0x42C select
  (gait / leading foot are guesses) - consistent with the replica's tries failing at 1.3-1.5 m/s.

## Wall-run request on the replica: facing and the ground sub-state
- Contact-point target (asi `eb620a23...`, pid 13188): the replica navigates to the peer's position on its first Walling
  frame (the wall's foot) and requests within 2 m of it; the log carries the replica's speed. Tries at 4-5.8 m/s near the
  contact point all failed; the one success came at 0.01 m/s facing (0.93, 0.35) - another wall face than the peer's
  (-0.41, -0.91). Speed is not what decides.
- Static (IDA): for ground sub-state 4 the request (`sub_1D5D920`) calls `sub_1CED250`, which ignores the direction the
  caller passes: it queries `sub_1DD27F0` from the entity's own position and facing (`*(matrix+0x10)` = row 1) with a reach
  of 1.5 m, or ~1.97 m when the mode argument is 1. Slot 21's wrapper `0x01D5E5C0` returns `sub_1D5D920(...) == 2` with
  ecx = [interface+0x90] (0x01D5E60C / 0x01D5E620).
- Matrix convention verified live on Edward (pid 32684): walking -x gave row 1 = (-1, 0, 0), row 0 = (0, 1, 0); walking +y
  gave row 1 = (0, 1, 0), row 0 = (1, 0, 0). Forward = row 1, right = (hy, -hx).
- Facing (asi `8b1b4416...`, pid 32684): within 1.5 m of the contact point the replica is turned to the peer's heading
  (physics-warp matrix write). 16 frames faced; tries while facing still failed; pass 2 had one success in the peer's
  direction (-0.43, -0.89).
- Diagnostic (asi `4d761f4c...`, pid 17948; reads the sub-state and calls the precondition `sub_1CE0A40` directly, unit
  added): during the whole approach the replica's ground chart was in **sub-state 115**, not 4 - 300 of 300 tries were
  refused before the precondition. Sub-state 4 appeared only after the replica stopped; then 73 precondition-false and
  ~1500 wall-test-failed (replica standing at the wall foot while the peer climbed). UNVERIFIED: that 115 is the NPC's
  navigation-follow state and that IHumanGround drives the replica in sub-state 4 (would also explain R3's "NPC ignores
  IHumanGround": navigation was always active).
- Final-approach experiment (in the committed code, **built but not run**, asi `cf23de28...`): within 4 m of the contact
  point, one NavigateToPosition Stop at the replica's own position, then no navigation; the FreeRun block drives
  IHumanGround with free-run (slot 3 = 1), sprint, speed 1.0 and the direction to the wall. Not run: the game exited
  ~2 s after start on three launches (store-layer restart, docs/evidence/single-instance-guard-2026-11-15.md; Ubisoft
  Connect had not been running), and the owner was using the PC.
- Final approach run (asi `ea083c34...` = feat/launcher f3d4898 incl. this code, pid 22736, replays converted with
  `--as-proto 16`, `0:0:0:1:1`, 2 passes): the Stop came once; afterwards the chart still reported a sub-state other than 4
  for 38 tries while the replica coasted 3.9 -> 3.2 m/s (IHumanGround did not drive it), then sub-state 4 near standstill
  (0.33 m/s, turned to the peer's wall): the first request there succeeded. That entry followed the peer's route - contexts
  {4,8} with the peer {4,8,30}, then {8,30} with an on-wall error of 0.27 m (first time the replica hung on the peer's ledge
  after its own entry). Afterwards it climbed onto the beam (the peer's input said up), dropped, and stood at the wall foot
  (start older than 5 s, no re-entry).
- Re-entry variant (asi `9dab1b5e...`, pid 29204, reverted, not committed): start kept valid while the peer is off the
  ground, Stop radius 1.5 m, hold-following on; `abmoves.sh ... 1:0:0:1:1 0:0:0:0:0` x3:

| run | mode | parkour err3d | both climbing | beam alone | wall-run entries |
|---|---|---|---|---|---|
| 1 | entry + no snap + moves | 3.74 m | 5 % | 11 % | 3 |
| 2 | baseline | 2.26 m | 18 % | 24 % | 18 |
| 3 | entry + no snap + moves | 3.68 m | 10 % | 11 % | 4 |
| 4 | baseline | 9.04 m (replica parked on a beam all run) | 0 % | 93 % | 1 |
| 5 | entry + no snap + moves | 3.53 m | 13 % | 20 % | 6 |
| 6 | baseline | 2.82 m | 24 % | 48 % | 11 |

  Worse: the replica stayed pinned at the wall foot (~7000 requests and as many facing writes per run, wall test failing)
  and without the stuck snap it never caught up. Negative; reverted.
- Status of this line: a native entry can follow the peer's route (once, 0.27 m), but the NPC replica cannot reliably
  wall-run on demand - navigation holds the ground chart in sub-state 115, IHumanGround does not move it, and a standing
  request succeeds only sometimes. The committed default-0 flags keep the tested behaviour; nothing here is on by default.

## Native entry, round 2 (2026-11-15 evening; builds on feat/launcher 610cfe7, replays `--as-proto 16`)
- Static (IDA): sub-state 115 is entered from 4 by event 114 (`sub_1D6EB00`: consumes 114, entry function `sub_1CDCC20`,
  `sub_1D659D0(4211)`); event 114 is posted only by `sub_1D2AD40` = **IHumanGround slot 632** (in sub-state 4), applied by
  slot 631 (`sub_1D7C120` -> `sub_1D6EB00`). IHumanGround has 841 slots (0x026E7DC0..0x026E8AE4). The only reader of slot
  632 is the navigation service: `sub_179DCB0` (called from `sub_17A7F00`) gets IHumanGround by component id 17, asks
  slot 630 (`vt+0x9D8`, meaning UNVERIFIED), drives with slot 638 (`vt+0x9F8`), slot 0 SetDirection and slot 24
  (`vt+0x60`), or requests 115 through slot 632. So 115 is navigation's own locomotion state, and its drive is slots
  0 + 24 + 638, not slot 1.
- Runs (pids 2468, 19596, 26868, 11324, 29936, 26456; asi cc444230 / 27982581 / 63e2348f / 1f558228 / 49bf639b / f8532f86;
  one 90 s replay of host-climbmoves-1 each):
  - Pushing slot 0 along the peer's heading after the navigation Stop moved the replica (2.5 -> 3.2 m/s) while the chart
    stayed in 115: slot 0 steers navigation's state too.
  - Sub-state during navigation varies by run: 115 for the whole approach in runs 17948 / 22736 / 2468, but 4 at 4 m/s in
    run 19596 (wall test failed, then a mode-1 success at 2.8 m/s).
  - In sub-state 4 the NPC did not move on slots 0/1/3/32 (back-away phase: 0.00 m/s; one sample before the next entry).
  - **Every run's first entry now follows the peer's route**: replica contexts {4,8,30} with the peer's {4,8,30}, 3D
    error 0.53-1.31 m in that window (0.24-0.60 m on the wall). Then, within ~2 s, the replica goes 30 -> 9 (onto the beam
    above) while the peer keeps hanging - with navigation stopped on the wall (63e2348f), with a neutral ground intention
    on the wall (1f558228), with no climbing input (hold queue empty / input idle, 49bf639b), and with the wall run applied
    only after a mode-0 success like the player (f8532f86).
- Reading: entering the wall natively on the peer's route works; what moves the hanging replica onto the beam is inside
  the replica (not navigation, not our ground or climbing input). Lead, UNVERIFIED: the NPC's own behaviour, enabled in
  replica mode.
- Committed under `NativeReplicaWallEntry` (default 0): final-approach drive phases, no navigation while the replica is on
  a wall, neutral ground intention on a wall, hold-idle (no stick fallback when hold-following has no target), mode-0-only
  apply. None of them changes the 30 -> 9 outcome; kept as the measured configuration.

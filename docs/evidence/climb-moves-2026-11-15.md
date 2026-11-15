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

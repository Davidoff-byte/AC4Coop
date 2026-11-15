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

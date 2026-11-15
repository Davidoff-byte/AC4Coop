# Native replica: the peer's real climbing input (2026-11-15)

Flag: `[Coop] NativeReplicaClimbIntent` (default 0). Solo replay only; not two-machine tested.

## What changed
- `src/netstate.h`: `ClimbBlock { magic 'CLMB', dir[2], speed }` added to `PlayerState`'s 128-byte `ContextBlock`
  union (selected by `contextId` 30). `sizeof(PlayerState)` stays 576 (static_assert), no protocol bump.
- Sender (`src/phase1.cpp`, capture): while `contextId == 30` and the flag is on, reads the climbing context's data block
  — context = `*(Human+0x720+0x70)`, checked against main vtable `0x026D4638` and interface vtable `0x026D4588` at
  context+0x10; data = `*(context+0xC)`; input at data+0x0C/+0x10 (SetDirection2D, slot 37, `0x01537F30`) and data+0x14
  (SetSpeedRatio, slot 38, `0x0152B0C0`); evidence `climb-decision-leads-2026-11-15.md` items 13-14.
- Receiver: the newest packet's block is kept at drain (`g_nrClimbValid`/`g_nrClimbIn`). In the climb-intent block the
  replica uses the peer's input when the block is present and non-idle (|dir| > 0.05); otherwise the error-derived
  direction (error > 0.6 m) as before. Log line gains `real=/derived=/peerBlock=` counters.

## Test (solo replay)
- Tested build: asi sha256 `3b175a43e8de84f9a1e63f29a829dc7032f829f286047b7191ffe2aaac4c383f`, commit 38181d7 + this diff
  (tree dirty), game pid 33660, AC4BFSP.exe 1.05. Test ini: NativeReplica, NativeReplicaParkour, NativeReplicaFreeRun,
  NativeReplicaClimbIntent = 1.
- Committed build differs only by caching the sender's ini read in a static (build-only, not re-run):
  `8ed0f2ef4b6427317f347389c2024cdf01f4f1c6fa1d2bfeffe881a563b329c7`.
- Recording `host-climbin-1.tap` (80 s, 4800 PlayerState datagrams): Pilot drove Edward up the wall at (35.9, -82.6)
  (climbpush, RT+A) then `pilot.py climb` right 4 s / up 4 s / left 4 s / up 4 s / right 3 s / left 4 s / down 4 s.
  `tools/oneoff/tapclimb.py`: **3708 datagrams carry the block, 1701 with non-zero input**, and the input timeline
  matches the Pilot script (e.g. 25.46 s (+1,0), 30.19 s (0,+1), 34.94 s (-1,0) ... 62.27 s (0,-1)).
- Negative control: same tap with the 3708 `CLMB` magics zeroed (`host-climbin-1-noblock.tap`) — the replica must fall
  back to error-derived input. Confirmed: in that run the `real` counter stayed at 777 and `peerBlock=0`.
- Both replays: `wire_tap.py replay <tap> join <code> --player-only --adopt-world --seconds 165` (3 passes, 9902 sent),
  Edward parked at (78.6, -76.3). Summary by `tools/oneoff/climb_intent_summary.py` over the two log windows
  (phase1_log_33660.txt lines 2211-5204 real, 5204-end control):

| run | on-wall frames | on-wall mean error | input frames real / derived | parkour err3d (frames) | snaps (parkour) | wall-run accepts |
|---|---|---|---|---|---|---|
| real input | 1289 | **1.02 m** | 777 / 253 | 2.76 m (7478) | 15 (14) | 15 |
| control (no block) | 1254 | 1.26 m | 0 / 881 | 2.52 m (7460) | 14 (14) | 9 |

## Result
- The real input crosses the wire and drives the replica's climbing sequencer (verified: counters, control).
- On the wall, the replica tracks the peer **19 % closer** (1.02 vs 1.26 m, one run each; run-to-run noise not measured).
- Overall it does **not** fix the route: parkour 3D error is no better (2.76 vs 2.52 m), stuck snaps are unchanged.
  Observed causes in the log (not root-caused): windows where the peer is 1.5-1.6 m above the replica (`y=1.57`) while
  the replica is fed "up" and does not move, and the replica ending on a beam (context 9, 20 deferred snaps per run)
  where the peer is not. Real stick input only says *which way*; where the replica's sequencer can go from its own
  hand-hold decides the rest, so a divergent entry point stays divergent.
- Next (R4.3 proper): send the decision, not only the stick — the chosen move (definition index + position on the
  guidance, `climb-decision-leads` items 19-21) and climb-up/ledge transitions; and a matching entry point (same wall
  contact as the peer) before input is applied.

# Native replica (R3 slice 1): the puppet moves itself on the ground - solo replay runs, 2026-11-15

Design: `docs/design/native-replica.md`. Flag `[Coop] NativeReplica` (default 0), `NativeReplicaSnapCm` (default 300).
All runs: AC4BFSP.exe sha256 bc11ad79..., Havana save, Edward parked at (78.7,-75.4) with `tools/pilot.py goto`,
`tools/wire_tap.py replay <tap> join <LAN code> --as-proto 15 --player-only --adopt-world`, game ini as the F7 runs plus
`NativeReplica=1` (PuppetNavService=1, PuppetPhysics=0, HybridLoco=1, IntentLane=1, [Pilot]/[Probe] Enabled).
Builds are from the uncommitted tree on 5384554 (`tree=dirty`); the final one is this commit's content.
Oracle: `py -3.14 tools/oneoff/replica_oracle.py <phase1_log> <tap>` (checks in the design note).
Verdict: **solo-tested: native movement works; oracle 2 of 4 checks pass (movement, ground stage); p95 error and the
run-start snaps fail.** Not two-machine tested.

## Ground-context finding (affects the default path)
The sender's `GetCurrentContextID(1)` is **4** on the ground (PS3 `ActorContextID`: 2 NOT_DEFINED, 4 Ground; tap
PlayerState+134 = 4 with ground stage 1 in 36530 of 36573 packets of hold-guard-near.tap). `HybridLoco` compares with
`CTX_GROUND = 2`, so with real packets the puppet's ground branch never runs: earlier logs show
`[Net] puppet behavior disabled (ctx=4)` (local/runs/phase1_log_19852.txt, 20588). Not changed here.

## Runs (tap host-p14-state-2.tap: 45.3 m of ground running per 33 s pass, then a climb; 5 passes)
| run (pid) | asi sha256 | steering | own movement / peer path, per full pass | snaps inside passes | p95 | stage |
|---|---|---|---|---|---|---|
| 17580 | b7d1519c... | v1: HumanGroundData +0x20/+0x98 writes | 21-32 %* | 13-14 (every 3 m) | 2.65 m | match |
| 19364 | 23e4d6d7... | v2: puppet's IHumanGround slots 1/0 | 23-28 %* | 12-14 | 2.65 m | match |
| 26108 | ae5f983a... | v3: CSrvNavigation::NavigateToPosition | 114-129 %* | 2, 0, 0 | 1.85 m | match |
| 7432 | 53938f99... | v3 + movement counter fix | crashed in pass 2 (below) | | | |
| 25508 | 5ea32a0c... | v3 + crash guards (final) | 103 %, 86 %, 118 % | 2, 2, 0 | 1.85 m | match |
\* runs 17580/19364/26108 counted the old path's off-ground teleports as own movement; v1/v2 windows on the ground show
`moved=0.0m` (49 of 63 windows in 17580), i.e. no own movement at all; the counter was fixed for 7432/25508.

- v1/v2: the replica never moved by itself. v2 logged `HumanGround=411DBEF0 vt=026E8AE8 ivt=026E7DC0 fsm(+0xA0)=4166A040
  data(+0xC)=4094A520 bundle+0x10=4094A520` (so `HumanGround+0xC == bundle+0x10`, which intention-seam.md had as
  UNVERIFIED) and the ground FSM already existed. A `[Probe]` hook on `SetSpeedRatio` 0x01D51690 with an entity snapshot
  ([[[ecx-0xC]+0x108]+4]) recorded 256 of 256 calls from the player (via the Pilot hook), none from ~50 NPCs nearby.
- v3 (final, 25508): in steady running the error is 0.3-0.9 m (2 s window means); the snaps inside passes are all in the
  first ~2 s of the run (x = 73 and 68: the replica accelerates from rest behind a 100 ms-delayed peer and falls 3 m
  back); the end of each pass (stairs, peer flickering through contexts 6/8, then climbing 30 on the old path) raises the
  error to 1.4-1.7 m. `NavigateToPosition` returned eax = 0 on every call while the puppet moved (meaning unknown).
- Second tap, 25508, host-p14-state-5.tap (35 m ground walk then rest, all context 4; 40 s): puppet spawned mid-run
  (snap 11.8 m), one more snap at the run start (3.02 m), then ~37 m of own movement trailing the peer by 0.9-1.3 m;
  at rest it settled 0.34 m from the peer (inside the 0.4 m deadband) and stayed there. No crash.

## Crash and fix (run 7432)
`AC4Coop_crashctx_7432_1.txt`: read fault eip 0x0123959C, data 0xE8, inside our NavigateToPosition call (return address
asi+0x7DDE is the instruction after `call eax` in NativeReplicaNavTick, dumpbin). Chain: slot 46 0x017B8220
(`ecx = [svc+0x84]`, call 0x0061F1F0) -> 0x0061F1F0 (`ecx = [ecx+0xC]`, jmp vt[0x30]) -> 0x01239590, which reads
`[[this]+0xC8]+0xE8` with this = [[svc+0x84]+0xC] = 0x4123FF30 and its +0xC8 null. It happened while the peer was
switching between ground and air/ledge/climb contexts (the old path disables the puppet's behaviour there). Fix: no nav
call unless the last ApplyRemoteState took the replica path with behaviour enabled and settled (`s_nrNavOk`), and skip
while `[[[svc+0x84]+0xC]+0xC8]` is null. Run 25508 (5 passes + the second tap): no crash, guard skip count 0, so the
null guard itself was not exercised.

## Tuning (same day, same two taps, oracle script unchanged)
Each version is one run (game restarted, build installed with the game closed). "state-5 run" = 2 s window means of the
error during the walk after spawn; "state-5 rest" = settled error with the peer at rest.
| run | asi sha256 | change | state-2 p95 | snaps inside passes | own movement | state-5 run / rest | crash |
|---|---|---|---|---|---|---|---|
| 25508 | 5ea32a0c... | v3 (baseline, committed db8f1f0) | 1.85 m | 2, 2, 0 | 86-118 % | 0.9-1.3 / 0.34 m | no |
| 22720 | 613be28d... | v4: velocity lead 0.45 s, class by along-track error, re-issue 0.5 m / 250 ms | 1.25 m | 2, 1, 0 | 93-108 % | 0.7-1.3 / **1.21 m stuck** | no |
| 31300 | 63ae42c7... | v5: lead only while the intention moves, 2 s keepalive, Fastest + 2 m aim at starts | 1.60 m | 3, 0, 2 | 86-91 % | 1.2-1.5 / 0.16 m | no |
| 17912 | cd70e0c0... | v6: + blended correction 150 cm/s | 1.05 m | 1, 0, 1, 1 | 79-111 %* | 0.44-0.6 / 0.15 m | no |
| 25720 | bb5340b3... | v7: + start/stop from the newest packet's intention (final) | 1.05 m | 1, 1, 1, 0 | 78-96 %* | 0.42-0.59 / 0.17 m | no |
\* own movement excludes the blended correction by design (about 0.6-0.8 m/s of correction while catching up), so
check 1 now fails narrowly although own + correction covers the peer path.

What the logs show (v4+ windows carry peer speed, replica speed and along-track error):
- Steady running: replica speed equals the peer's (~6.1 m/s at Fastest) and the error is 0.2-0.6 m.
- v4's stuck replica: commanded Regular toward a resting peer 1.2-1.8 m away, 0.0 m moved for 6 s while the same request
  was re-issued every 250 ms; with a 2 s keepalive (v5) it settles within 0.16 m. Cause of the stall UNVERIFIED.
- Run starts remain the failure: in the first 2 s window the replica averages 3.8-3.9 m/s against the peer's 5.6-5.7,
  falls 3 m behind and snaps (about once per pass). Starting from the newest intention (v7) did not change this, so the
  limit is the NPC body's acceleration, not the 100 ms delay.
Final verdict (v7): checks 4 (stage) PASS; 3 (p95 <= 1.0) FAIL at 1.05 m; 2 FAIL (one run-start snap per pass);
1 FAIL narrowly by construction (correction excluded). No crash in 4 tuning runs.

## Not established
- Run-start snaps. Options not tried: a stronger correction only while the replica is accelerating (risks visible
  sliding), a larger snap limit (would only hide it), a faster start through another nav argument (a5/a9/a11/a12 are
  unknown).
- Facing at rest, the meaning of NavigateToPosition's a5/a7/a9/a11/a12 and of its return value.
- Whether guards, perception and damage now treat the native puppet differently (R4-R6).

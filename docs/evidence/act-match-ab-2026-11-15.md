# Puppet action match, native replica on vs off: same-PC A/B (2026-11-15 20:21 / 20:37)

Build: AC4BFSP.exe pinned (bc11ad79...); ASI 66d3520444d5ecec156daa0ec3a502bf6a1dccb7d1163b513bbc4f3698807eb2 from commit
`a29b8ab`, tree clean (`[Build]` line of every run log) (`[Coop] ActMatchProbe`); `proxy/version` version.dll 3f357d02.... D: Steam install. Verdict: **same-PC two-instance
tested**, one valid run per arm.

## Oracle
`[ActMatch]` (src/phase1.cpp `ActMatchNote`). Each puppet apply compares the puppet's current ACTLAYER_PRIMARY action alias with
the peer's `PlayerState.actionAlias` (the same reads as the `[Net] PUP` line). It is bucketed by the peer's context id and by mode
(`replica` = `NativeReplicaOn() && NativeReplicaMode(ctx)`, otherwise `actions`) and logged every 5 s. Alias match is necessary
for correct animation but not sufficient: it does not check pose, timing, position or targets.

## Setup (identical in both arms)
- Host A and guest B joined through lobby code `R2M4-8S5T-Q74V` (`docs/evidence/same-pc-coop-2026-11-15.md`). The only ini
  differences between arms were `[Coop] NativeReplica=1/0` (the master switch for `replica`) and `ActMatchProbe=1` in both. The other
  NativeReplica* flags stayed as in the install (Parkour, FreeRun, ClimbIntent, ClimbMoves, WallEntry, ClimbNoSnap = 1).
  `HybridLoco=1`.
- Both copies booted into the world with `pilot.py boot`, then the fixed script `ab_moves.py` (virtual pad, host only) ran: run 3 s,
  3 run-jumps, free-run 5 s, turn and free-run 5 s, idle, 6 taps of X, idle. Measured on the guest (the puppet of the mover).
- Code fact (phase1.cpp, per-frame mode block): `if (replica) { aiOff = false; actNow = false; locoNow = false; }`. In replica mode
  we never write the peer's action; the puppet's own FSM chooses its animation.

## Results: guest's puppet of the moving host, match = puppet alias == peer alias
| peer ctx | replica ON (20:21) | replica OFF (20:37) |
|---|---|---|
| 4 (ground) | 5-s windows: 9%, 0%, 30%, 97%, 5%, then 0% x4 while the host stood idle | 95-100% in every window (296-299 frames) |
| 6 | 0/31 (0%) | 22/23, 91/95, 12/13, 19/22, 27/29 (86-96%) |
| 7 | not seen | 19/19 (100%) |
| 8 | 0/11 (0%) | 9/11, 2/3 (67-82%) |
| 9 | 0/1 | 1/2, 10/11, 11/12, 16/17, 13/14 (50-94%) |
| 10 | not seen | 92/162, 180/180 (57-100%) |
| 30 | 0/30 (0%) | 15/19, 147/207 (71-79%) |
- Replica ON: every frame in every context ran in `replica` mode, so the peer's jump and parkour actions never reached the
  puppet, and even idle on the ground did not match.
- Replica OFF: every frame ran in `actions` mode. Ground and off-ground contexts mostly match. The lowest buckets (ctx 8, 10, 30)
  are transitions, where the puppet lags the peer's switch.
- The attack taps produced no fight context (no target nearby), so **combat was not exercised** in either arm.
- Host's puppet of the idle guest, OFF arm: 100% until the last two windows, which read 0/295 and 0/298 after the host had moved
  about 45 m away. Not explained.

## Incidents (pilot frame cap), not a replica effect
- Two earlier OFF attempts (20:22, 20:31) froze one copy right after `[Pilot] frame cap: Present slot ... patched` (the first
  `pilot.py boot` command, default `--fps 60`). 20:22: the guest stopped logging and responding (0.12 s CPU in 3 s). 20:31: the
  host's frame counter stayed at 2 through 4 pulses, then the process exited with no Application-log crash entry. In both ON runs the
  host's first boot attempt also stalled ("not in world yet") and a retry worked.
- With `boot --fps 0` (no Present patch) the OFF run and an interactive pair both booted cleanly. A probable cause is the pilot
  Present patch with two copies; that is unverified (no dump was captured: the frozen copies were closed before the dump ran).
- One more ON attempt (20:17) is invalid: the host was not in the world while the moves ran (pos 0,0,0). It was re-run.

## Not checked
Visual pose and transitions on screen, combat or kills, NPC sync, runs longer than about 1 minute, repeatability (n = 1 per arm).

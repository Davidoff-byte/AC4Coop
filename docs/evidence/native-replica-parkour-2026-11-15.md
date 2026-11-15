# R4.1 off-ground native follow (NativeReplicaParkour): solo replay, 2026-11-15 - NEGATIVE

Design: `docs/design/player-decisions-r4.md` (R4.1). Code: 3903b0e. Build: asi sha256
bd2d195c8c0bd60f000164432712f8dff14494691d63a8ec09d8956cb4933781 (`[Build]` line: commit=3f6a171 tree=dirty, i.e. 3903b0e's
source before its commit). Run pid **26944**, AC4BFSP.exe sha256 bc11ad79..., Havana save, Edward parked at (78.7,-75.4).
Game ini: as the R3 runs plus `NativeReplica=1`, `NativeReplicaParkour=1` (`NativeReplicaParkourSnapCm` default 800,
blend 150, start boost 400). Replays, one after another (`wire_tap.py replay <tap> join <LAN code> --as-proto 15
--player-only --adopt-world`): host-p13-1 90 s (climb to a 12 m roof at 46-55 s), host-p14-state-3 45 s (climb to
7.3 m at 18-33 s), host-p14-state-10 80 s (beam/climb at 5-7 m, 31-73 s). Report:
`py -3.14 tools/oneoff/r41_parkour_report.py local/runs/phase1_log_26944.txt host-p13-1 host-p14-state-3 host-p14-state-10`.

Verdict: **solo-tested, negative.** Navigating the replica to the peer's 3D position at ChaseHigh (NavigationSpeed 10)
does not make the NPC body climb after the peer. No crash, no `EXCEPTION` line, no crash context file.

| tap | replica context on off-ground windows | off-ground 3D error (frame-weighted mean / max) | snaps (parkour / stuck) | longest consecutive snap run |
|---|---|---|---|---|
| host-p13-1 | 4, then 5 and 6 only after the parkour teleport onto the roof | 4.46 / 7.96 m | 43 (1 / 0) | **38** |
| host-p14-state-3 | 4 only | 2.05 / 7.93 m | 3 (2 / 1) | 1 |
| host-p14-state-10 | 4 only | 1.77 / 4.63 m | 6 (1 / 1) | 2 |

Peer contexts on those windows: 4, 6, 7, 8, 9, 30 (all three taps; state-10 without 7).

## Details
- host-p13-1: while the peer climbed (peerCtx 0x40000150 = contexts 4, 6, 8, 30) the replica stayed in context 4 (repCtx
  0x10) and its 3D error grew to 5-8 m; snap #5 (`err=2.96 dz=7.44 -> (21.5,-83.6,11.0) [parkour]`, 3D 8.0 m > 8 m limit)
  put it on the roof. Only then did windows show replica contexts 4+6 (0x50) and 4+5 (0x30).
- The same tap then logged 38 consecutive snaps (#6-#29 and on) on the roof at (19.7..19.8,-77.5..-76.4,12.0): the peer was
  in context 4 on the roof, so the ground rule (planar error > 3 m) applied, and the error grew 3.04 -> 4.2 m between
  frames although every frame teleported the body to the peer. The teleport did not hold. A reading consistent with
  replica context 5 (Ladder) placing the body itself, overriding the matrix write, is UNVERIFIED.
- host-p14-state-3 / state-10: no native climb context at all; the error stayed bounded by the stuck rule (one stuck
  snap each) and the 8 m rule.

## What this means for R4
NPC navigation does not reproduce player parkour, so the native route for climbing is the decision route: R4.2 (RE the
player's ClimbingSequencer candidate selection on PC) and R4.3 (replicate the chosen move so the replica has the context's
attach target). `NativeReplicaParkour` stays 0.

## Not established
- Why the roof teleports did not hold (snap loop). A snap-rate limit would bound it but was not added: the flag stays off.
- Whether another NavigationSpeed or nav argument (a5/a9/a11/a12 unknown) enables climbing links.

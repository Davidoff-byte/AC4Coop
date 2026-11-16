# Seamless scorecard: baseline and the first fixes (2026-11-16)

`tools/scorecard.py` (commit dd82a91, bars fixed in its THRESH before the first run). Every run: two copies via
`two_copy.ps1 -Moves tools/two_copy_moves.py`, settings `NativeReplica=0 FightIntents=1 IntentLane=1 ActMatchProbe=1`,
D: Steam install.

| run / build | guest puppet ground | off-ground | puppet largest frame move | guard max error | **guard largest write step** | guards with no body |
|---|---|---|---|---|---|---|
| baseline 19720/27356 (asi 7e7078f7, 605f382) | 93.9% FAIL | 80.8% | 0.530 m FAIL | 11.99 m FAIL | (metric added later) | 12.6% |
| approach v1 27840/22656 (66f3d23) | 94.9% FAIL | 81.6% | 0.516 m FAIL | 11.55 m | **10.37 m** FAIL | 13.6% |
| approach v2 29364/18436 (447549c) | 98.9% | 83.5% | 0.505 m FAIL | 11.86 m | **2.02 m** FAIL | 11.0% |
| per-frame guards 27448/26876 (a5a7bb4, OwnUpdateGuards=1) | 96.6% | 87.7% | **1.178 m** FAIL | 11.95 m | **11.78 m** FAIL | 16.2% |
| per-frame + re-seed 17916/15912 (asi 785cec66, OwnUpdateGuards=1) | 99.7% | 86.1% | 0.621 m FAIL | 15.23 m | **15.24 m** FAIL | 7.8% |

Host's puppet of the idle guest: 100% ground, max frame move 0.013 m, in every run. Double links 0 and both copies alive in
every run.

## What changed
- **Approach (66f3d23 + 447549c):** a body linked 1.5–30 m from its host guard eases to it (smoothstep, duration
  max(0.8 s, gap / 5 m/s)) instead of snapping. A stale approach entry from an earlier link of the same body is dropped
  (that caused the 10.37 m step in v1). The largest guard write step fell from 10.37 m to 2.02 m.
- **The remaining ~2 m steps come from write rate,** not snaps:
  - windows without new approaches show 0.4–0.6 m;
  - windows with approaches show 1.6–2.1 m;
  - legacy writes happen per received packet per guard, about 4 Hz here, so a guard moves in hops.
- **Per-frame writes (`OwnUpdateGuards=1`, a5a7bb4): worse in this run.** Probable cause: the legacy write (when
  `OwnStarved`) and the per-frame blend both write the body. UNVERIFIED. The default went back to 0 (read once);
  the approach code stays on both paths.
- `guard max error` stays about 12 m by design: during an approach the body is still on its way. The step metric is the
  visible one.

## Per-frame mode: a second mover (run 17916/15912)
- Re-seed fix: an own-mode record that is stale (not published for 300 ms) or > 1.5 m from the body starts from the
  body's real position. 28 re-seeds. It did not help: the largest step was 15.24 m.
- The own-mode probe gives the reason. Between two of our writes, something else moved a linked body **8 times, mean
  10.3 m, max 69.1 m**, with `legacyWrites=0` (`[Own] ... guard frames=1622 moved=8 mean=10.338 max=69.083`). Our next
  write puts it back, and that is the "step".
- Not ours: `DriveCachedMirrors` has no caller, and the legacy write is off in own mode. Candidates: the engine moving
  or recycling the body (despawn and respawn of the same Human far away), or physics. **UNVERIFIED**.
  Next: log the body's position delta with the Human's spawn or recycle state on a `moved` event.
- `OwnUpdateGuards` stays 0.

## Next for smoothness
- Make the per-frame path the only writer for a linked body, and re-score.
- Puppet pop of about 0.5 m during free-running (guest's puppet of the moving host).

## Guard pops, found and fixed (2026-11-16 afternoon)
`[Bind] step` lines (≤ 40 per run) now log every write step > 3 m with the body's position and the target.

| run (asi) | change | puppet maxD (3D / frame / peak speed) | guard largest write step | records with no body | stuck released |
|---|---|---|---|---|---|
| 31172/30600 (ccece73b) | metrics only | 0.345 (0.260 m / 18.8 ms / 15.9 m/s) | 12.71 m | 8.6% | - |
| 21764/20240 (9335ced9) | step log | 0.326 | 8.17 m: **one body never moved**, (109.4,-73.4,2.0) for 25 writes while the targets went 3 → 8 m away | 10.4% | - |
| 11048/23292 (2e343fb6) | stuck release v1 | 0.549 (0.429 m / 35.1 ms) | 7.17 m: one approach write (`approaching=1`, v2 approach eased from its start point after the body had moved) | 11.4% | 0 |
| 14092/33460, 1600/32420 (5b44a204) | approach v3 (move toward the target from the body's current position, ≤ 9 m/s × time since its last write, capped at 0.1 s) | 0.336, 0.344 | **1.45, 1.47 m** | **52.5, 55.3%**: v1 released pirates idling about 1 m from their guard | 5, 5 |
| 27780/29824, 21152/5020 (c8c3dd80) | stuck release v2 (only while the body is > 1.5 m off) | 0.631 (0.558 m / 17.3 ms / 32.2 m/s), 0.596 (0.518 / 16.9 / 31.5) | **1.48, 1.47 m** | 11.9, 18.3% | 0, 1 (the same spot, 2.3 m off) |

- **Stuck bodies:** some bodies ignore position writes. One spot, (109.4,-73.4,2.0), recurred in 4 runs. What holds
  the body there is **UNVERIFIED**; a scripted station is a guess. Before this change, such a body stood frozen while its
  host guard walked away. Now, once a body is > 1.5 m off and has not moved for 6 writes, it is released to its own AI
  and kept out of linking for 120 s (`[Bind] stuck body ... released`).
- **Approach v3** cannot pop by construction: each write moves at most 0.9 m.
- **What remains, about 1.5 m:** the hop of a running guard between legacy writes. A host guard at about 6 m/s, written
  about every 0.25 s, moves 1.5 m per write. A per-frame writer would remove it, but own mode currently fights an
  unknown external mover (section above).
- **The puppet's maxD varies from run to run** (0.33 – 0.63 m on the same move script). The new 3D / frame-time numbers
  show the two latest runs had a real pop: 0.52-0.56 m in one 17 ms frame (32 m/s, about 4× a sprint). Earlier "pops"
  of 0.5 m were partly the metric (XZ + |dY| overstates diagonal moves by up to 41%).

## Where the puppet's largest frames happen (runs 28388/22128 and 33500/26780, asi 82d491fe)
`[Smooth]` now names the largest frame's context (and the previous frame's), and which interpolation path produced it.

| run | window maxima (3D m / frame ms / m/s / context / path) |
|---|---|
| 28388/22128 | 0.12/17/7 ctx4; **0.37/34/14 ctx9**; 0.16/19/9 ctx30; 0.33/35/14 ctx6→4; **0.29/17/17 ctx6** |
| 33500/26780 | 0.12/18/7 ctx4; 0.19/17/11 ctx6; 0.06/18/4 ctx10; 0.15/17/9 ctx30; **0.26/16/16 ctx6** |

- All are path `i` (interpolated between two samples); none is extrapolation, hold or clamp.
- The largest frames are in the air (ctx 6) and in free-run (ctx 9), at 14-17 m/s, or come from a 34 ms frame.
  Air at 16-17 m/s is plausible for a fall, but whether these frames are pops is **not settled**. The interpolation is
  linear between 20 Hz samples, so a curved jump path is cut into chords, which a ballistic or curve-aware
  interpolation would fix. UNVERIFIED that this is the cause.
- Both runs pass the 0.5 m bar (0.40 and 0.28 m). Over the 8 scorecard runs today the bar passed 5 times and failed
  3 times (0.549-0.631). In the two failures with the 3D metric, the frame was 0.52-0.56 m in 17 ms; their context was
  not recorded yet.

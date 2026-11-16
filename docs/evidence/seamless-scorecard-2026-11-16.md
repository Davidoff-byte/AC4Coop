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

## Next for smoothness
- Make the per-frame path the only writer for a linked body, and re-score.
- Puppet pop of about 0.5 m during free-running (guest's puppet of the moving host).

# Enemy placement: baseline and GuardPlace (2026-11-16)

Design: `docs/design/enemy-placement.md`. The bars were committed in 89da594, before the first run
(`tools/scorecard.py` THRESH: `place_*`).

## Setup

- **Rig:** two copies on one PC via `tools/two_copy.ps1 -Moves tools/placement_moves.py`. That is about 110 s of host
  movement around the start point: idle, walk loop, sprint loop, run back. The guest player stays idle.
- **Install:** D: Steam install. The rig lock was taken for the runs, and the installed ASI and ini were restored
  afterwards: ASI 723b1157... back, ini hash identical.
- **Base settings for every run:** `NativeReplica=0 FightIntents=1 IntentLane=1 ActMatchProbe=1`, so the wide bind
  is 12 m and `BindLock` is on by default.
- **Scoring:** `py -3.14 tools/scorecard.py <game> <host> <guest>`. JSON was saved to `%TEMP%`; it is not committed.

| run | host / guest pid | asi (sha256, first 16) | commit | flags |
|---|---|---|---|---|
| 1 baseline | 31700 / 19724 | 2192db504fd4c5bf | 89da594 | `GuardPlace=0` (today's defaults) |
| 2 | 20712 / 32224 | 2192db504fd4c5bf | 89da594 | `GuardPlace=1 EntityFrameCadence=1` |
| 3 | 10960 / 20472 | a5c498d2c6f0699a | 89da594 + step split | run 2 + `BindFarRadiusM=40` |
| 4 | 8600 / 28048 | 6a4b0a8c1f95c9a8 | this commit (recycle release, 0.4 m approach cap) | as run 3 |

## Result

| metric (guest) | bar | 1 baseline | 2 | 3 | 4 |
|---|---|---|---|---|---|
| largest write step of a linked guard | ≤ 0.50 m | 1.49 FAIL | 0.96 FAIL | 1.22 FAIL | **0.65 FAIL** (≤ 1.0 old bar: PASS) |
| ... at frame rate (update gap ≤ 100 ms) | - | n/a | n/a | 0.729 | 0.655 |
| ... writes after an update gap > 100 ms / of them > 0.5 m | - | n/a | n/a | 50 / 41 | 3 / 0 |
| settled error mean (links ≥ 3 s, not approaching) | ≤ 0.35 m | 0.11 PASS | 0.02 PASS | 0.03 PASS | **0.02 PASS** |
| settled error max | ≤ 1.5 m | 5.15 FAIL | 0.56 PASS | 0.43 PASS | **0.53 PASS** |
| no body, alive guards < 30 m from the guest | ≤ 5% | 0.0% (0/403) PASS | 16.8% (203/1210) FAIL | 0.0% (0/1051) PASS | **0.0% (0/1279) PASS** |
| no body, all alive guards | ≤ 12% | 5.5% PASS | 5.1% PASS | 6.7% PASS | **3.9% PASS** |
| frozen links | 0 | 0 PASS | 0 PASS | 0 PASS | **0 PASS** |
| double links | 0 | 0 PASS | 0 PASS | 0 PASS | **0 PASS** |
| both copies alive | yes | PASS | PASS | PASS | **PASS** |
| moved-by-other events / largest | - | (legacy probe: 43-50% of frames) | 79 / 4.40 m | 41 / 68.59 m | 22 / 68.48 m |
| slot releases "body-gone" | - | 29 | 39 | **209** | 24 |
| recycled-body releases / place snaps | - | - | 0 / 0 | 0 / 15 | 3 (+2 logged as stuck lines) / 1 |
| guest-only fighters < 40 m (max per window) | - | 0 | 1 | 9 | 1 |

**Bars passed in the final run:** 7 of 8. The one that failed is the largest write step: 0.65 m against 0.50 m. It is
better than the baseline's 1.49 m and passes the older 1.0 m bar.

**The guards in this scene mostly stand.** The settled error is very low in every run, and these runs have no combat.
Running guards were not measured under combat in this work.

## What the runs showed

### 1. The 69 m "second mover" is the guest engine recycling bodies

Run 3 logged a linked body that was moved straight to the world origin, then put somewhere else 36 ms later with its
drive flag set again:

```
[Place] moved ent=3C111570 ... by=68.59 m since our write 438 ms ago (link age 760 ms) ctx=4 drive=1 body=(0.0,0.0,0.0)
[Place] moved ent=3C111570 ... by=11.07 m since our write 36 ms ago ... drive=1 body=(30.6,-60.8,4.5)
```

- The drive flag stays 1 even though ApplyGuards clears it every packet (`m_DriveEntity`).
- Three different bodies linked to host guard F0007633 were each put back on one spot, (26.2,-61.7,6.9), 34-37 m from
  our writes.
- The earlier "stuck spot" (109.4,-73.4,2.0) fits the same pattern. UNVERIFIED.
- **Reading:** the guest's population system despawns and re-spawns these NPCs, sometimes at the same entity address
  (pooling). That is a hypothesis from live data only. The despawn/recycle path is **not** found in IDA. That is the
  next RE item (design, stage B0).
- Chasing a recycled body is what produced the 209 "body-gone" releases and 15 snaps in run 3. With run 4's rule (a
  jump of more than 5 m away from our write, or a body at the origin, ends the link at once and keeps the body out of
  linking for 120 s), the count went back to 24 releases, with 3 recycled-body releases.

### 2. The run-2 near-band gap

The run-2 near-band gap is the same phenomenon. Its body was despawned (`[Slot] release reason=body-gone`). The next
free same-archetype body was 16 m away, beyond the 12 m radius (`[WideBind] left(beyondRadius=15 nearest=16.0)`), so
that guard had no body for about 13 s. Runs 3 and 4 did not repeat it. The scene's guards are random, so one 110 s run
per setting is a small sample.

### 3. The hop

- Baseline writes happen per applied packet. The legacy probe sees the body move between our writes in 43-50% of
  frames, the "hop".
- With `GuardPlace` the body is written every frame it updates, and the settled error drops from 0.11 / 5.15 m to
  0.02 / 0.53 m.
- The remaining steps of 0.5-0.65 m happen at frame rate. They are not approach steps, which run 4 caps at 0.4 m.
  The source is not identified: candidates are target jumps in the ring (samples more than 5 m apart are held, then
  stepped) and extrapolation corrections. UNVERIFIED.

### 4. Bodies the engine updates rarely

`[Place] moved ... since our write 616 / 1158 / 3703 ms ago`: some bodies 30-60 m away are not updated every frame.
Any writer on their own update steps by speed × gap. In run 4 that happened 3 times, all under 0.5 m.

### 5. Far bind

`BindFarRadiusM=40` linked 5 bodies at 14-24 m in run 3, all at least 35 m from the guest player. Whether a player
would see that is UNVERIFIED (no camera test).

### 6. Hook placement (IDA, read-only, copy `D:\ac4ida\enemy`)

- `OnFrame` (and so `OwnDrain` / `GuardPlaceWrite`) is called at 0x016FAACD.
- That is after the per-Human update's last call `call sub_16CBB80` (0x016FAAC8) and before the tail
  `sub_14238E0` / `sub_169A170` (0x016FAAD7 / 0x016FAAE5).
- `SetGlobalMatrixInternal` 0x00528200 has only data xrefs: vtable slot entries at 0x01E4A160, 0x01E4CEC8,
  0x01E646B8 and 0x01E809D8. The engine reaches it virtually. **NEGATIVE for "static callers":**
  - METHOD: `xrefs_to` 0x528200.
  - COVERED: direct code xrefs.
  - NAMES: none found.
  - Virtual call sites were not enumerated.

## Not verified / not done

- Combat with running and chasing guards, and the guest attacking linked guards under `GuardPlace`.
- Visibility of far-bind moves.
- The despawn/recycle path in IDA (named above as a hypothesis).
- Two-machine runs.
- `EntityFrameCadence=1` alone, without `GuardPlace`: the guest puppet's apply rate fell to 24.1/s in run 2 against
  the 25/s bar. That is one run, and the bar is a frame-rate proxy.

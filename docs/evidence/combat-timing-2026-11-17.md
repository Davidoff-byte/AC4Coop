# Guard combat timing on the guest (2026-11-17)

Owner's live verdict (guest, two copies on one PC, both relay modes): "very delayed and laggy", "I can't time counters,
hits are inconsistent with the enemy animations". This file measures that, names the cause from the numbers, and scores
the fix against a bar written before the first run.

- Probe: `[Coop] CombatTimingProbe` (default 0, read once at load), `src/combat_timing.cpp`, lines tagged `[CTP]`.
  Read-only: no game memory and no wire byte is written. One clock on both copies: QueryPerformanceCounter microseconds
  (system-wide, so a same-PC session needs no offset estimate).
- Scorer: `py -3.14 tools/combat_timing.py <host log> <guest log> [--attacks]`. The bar constants live in the scorer.

## 1. PASS bar (written before any probe run)

| id | measure | bar |
|---|---|---|
| B1 | per guard hit on the guest's player: `skew = (guest hit post - host hit post) - (guest copy starts the attack action - host guard starts it)` | `\|skew\|` p95 <= 100 ms, and the copy is in the host's attack action at the hit in >= 95 % of hits |
| B2 | warning lead (first warning post -> hit post), guest vs host, AttackForward runs only | `\|guest lead - host lead\|` p95 <= 100 ms, no hit without a warning on the guest when the host had one |
| B3 | guest player-update interval in 5 s windows that contain an attack | median of the windows' p95 < 25 ms |
| B4 | entity lane: host capture -> guest apply | median of the windows' p95 <= 100 ms |
| B5 | owner, playing the guest | counters can be timed, comparable to a solo fight |

- At least 10 joined hits per run, otherwise INCONCLUSIVE.
- Baselines: one owner-driven run with `AttackForward=1`, one with `AttackForward=0` (PlayerDamageRelay), live ini
  otherwise unchanged.
- Negative control for the probe: an idle pair (nobody fights) must show 0 attack lines and still print frame and lane
  lines.

## 2. Static findings before the first run (AC4Coop source at `b95b7a5`; leads to confirm or kill with numbers)

| # | finding | where | status |
|---|---|---|---|
| S1 | The whole entity lane runs on every 6th player update on **both** copies: `EntitySyncTick` returns unless `slowTick` when `[Coop] EntityFrameCadence` is 0. The live ini does not set the key (default 0). The host therefore captures at most every 6th frame, and the guest polls and applies at most every 6th frame. | `src/phase1.cpp` `EntitySyncTick` (`slowTick = ++tick >= 6`, `if (!s_frameCadence && !slowTick) return;`) | read in source; cadence UNVERIFIED live |
| S2 | With `OwnUpdateGuards=0` and `GuardInterpBuffer=0` (both absent from the live ini) a mirrored guard's transform is written once per applied snapshot, with no interpolation between snapshots. | `ApplyGuards` -> `SetGlobalMatrixInternal` | read in source |
| S3 | The guard's action is mirrored alias-only: `SetTargetAction(alias, 0, 0, 0xFFFF, PRIMARY)` on the copy's next own update, no seek to the host's phase (`GuardAnimFull=0`). | `MirrorActQueue` / `MirrorActDrain` | read in source |
| S7 | Alias debounce: a new alias is issued on the copy only on the **third** consecutive apply that names it (first sight: `count = 1`, return false; second: `count = 2`, return false; third: issue). Re-applies of the retained snapshot on the slow tick count as applies. With S1 that is two more 6-frame ticks after the alias first arrives. An alias the host holds for fewer than three applies is never played. | `MirrorAliasShouldApply` (`src/phase1.cpp`) | read in source |
| S4 | Attack events do not use that lane: `IC_ATK_*` / `IC_PDAMAGE` are published on the host's player update and the lane is pumped with every player-state send (every frame); the guest drains it every player update. | `AttackForwardHostPump`, `Net::Send` -> `PumpIntents`, `FightIntentDrain` | read in source |
| S5 | Warnings are sent at most every 150 ms per guard; the guest re-posts the last one every frame until it is 250 ms old. | `Phase1_PuppetAttackPosted`, `AttackForwardOwnerTick` | read in source |
| S6 | Frame rate in today's logs, from the `[Own] ... puppet frames=` count per 5 s window: both copies ran at about 60 fps or at about 26.6 fps, always together. Owner session 37776/23888 (asi `62c7e5c7`): 60 fps for the first ~16 min, 26.6 fps for the next ~23 min, then 60 fps. All six pairs between 09:36 and 13:43 ran at 26.6 fps; the pairs at 17:00-17:36 ran at 60 fps. | `phase1_log_*.txt` in the test install, 28 newest logs | measured from existing logs. **Not the owner's problem:** every 5 s window with a fight line (`[FI]`, `[AF]`, `[PDR]`) in the three owner sessions of 18:30-18:42 (pairs 37776/23888, 25884/34488, 28784/2460) has 295-301 frames, i.e. 60 fps, on both copies; the 26.6 fps stretch of 37776/23888 contains no fight line at all. Cause of the 26.6 fps state UNKNOWN and not pursued (it did not occur in run 1 below under any focus state) |

If S1 and S7 hold live, a guard's animation reaches the guest's copy several hundred ms after a hit does, which is
candidate (i) of the task.

## 3. Runs

(newest last; every run names the ASI sha256 and commit)

### Run 1: idle pair, probe validation and negative control (no fight)
- ASI `c296adc5130302d5752a1d1fd9b7cc81895d374237396072c61d6ab02c0037bb` (commit `b95b7a5` + the probe, tree dirty), host
  pid 14760, guest pid 21752, `tools\two_copy.ps1 -Interactive -Set CombatTimingProbe=1`, live ini otherwise unchanged
  (`AttackForward=1`, no `EntityFrameCadence`, `GuardInterpBuffer`, `OwnUpdateGuards`, `GuardAnimFull` keys).
- Negative control: 0 attack lines (`H fde`, `G atk`, `G pdr`) on both copies; frame and lane lines every 5 s. PASS.

| measure | host | guest |
|---|---|---|
| player-update interval, 16 consecutive 5 s windows | p50 16.6-16.8 ms, p95 17.8-18.8 ms, max 24.4 ms | p50 16.6-16.8 ms, p95 18.0-19.0 ms; two single frames of 73 ms and 880 ms at the two forced focus switches |
| same, by focus (22 s each: guest window in front, host window in front, neither) | no change | no change |
| display | 144 Hz, not a remote session | same |
| snapshot send interval (`SnapshotHz=20`) | **p50 100 ms** (p95 103 ms) | - |
| snapshot apply interval | - | p50 100 ms |
| host capture -> guest apply | - | p50 8-76 ms by window (drifts with the phase of the two 6-frame ticks), p95 up to 96 ms |

- **S1 confirmed live:** the configured 20 Hz lane runs at 10 Hz at 60 fps.
- **Candidate (iii) killed for the frame rate:** the unfocused copy does not run slower (16.7 ms in all three focus
  states, n = 16 windows per copy). METHOD: forced foreground with `SetForegroundWindow`, `[CTP] frame` lines. COVERED: an
  idle pair in the world, 66 s. NOT COVERED: a fight (run 2 measures it).
- **Guard action latency, all action switches of mirrored guards (n = 915 over 174 s, ambient, no fight):**

| stage on the guest | reached | latency after the host guard switched |
|---|---|---|
| an applied snapshot names the new action | 757 / 915 (83 %) | p50 104 ms, p95 160 ms |
| `SetTargetAction` issued on the copy | 719 / 915 (79 %) | p50 321 ms, p95 379 ms |
| the copy plays it (its own update sees it current) | 666 / 915 (73 %) | **p50 331 ms, p95 398 ms** |

  By how long the host held the action: under 150 ms, 12 of 82 played (15 %); 300-600 ms, 57 of 92 (62 %); over 1.5 s,
  455 of 536 (85 %). The ~220 ms between "named" and "issued" is S7 (two more 100 ms applies); the first ~100 ms is S1.
  Script: `tools/oneoff/action_latency.py` (joins `H anim` with `G rx` / `G act` / `G body` by guard id and alias).

### Run 2: idle pair, `GuardActionLane=1` (the fix candidate for the animation half)
- ASI `21bcae0d2e778f622c52ea8f3cfd78322c5dbd8e8fa94f1fdde7f99ba7754702` (commit `b95b7a5` + probe + lane, tree dirty),
  host pid 32760, guest pid 30264, `-Set CombatTimingProbe=1,GuardActionLane=1`. Same place, same script as run 1.
- Roles checked (the same-PC rig left `EntityHost=1` in the shared ini for both copies, `ops/SESSION-2026-11-20.md`
  section 5): on this branch the join copy runs as guest. Receipt: its log has only `[CTP] G ...` lines and
  `[GAL] ... host: scoped=0 tx=0 | guest: rx=1305`; the host copy has only `H` lines and `guest: rx=0`.

| stage on the guest | run 1 (lane off, n = 915) | run 2 (lane on, n = 493 over 93 s) |
|---|---|---|
| `SetTargetAction` issued on the copy | 79 %, p50 321 ms, p95 379 ms | 94 %, **p50 26 ms, p95 42 ms** |
| the copy plays it | 73 %, p50 331 ms, p95 398 ms | 89 %, **p50 42 ms, p95 60 ms** |
| played when the host held the action under 150 ms | 12 / 82 (15 %) | 41 / 46 (89 %) |
| guest player-update interval | p50 16.7 ms, p95 18-19 ms | p50 16.6-16.8 ms, p95 17.7-17.9 ms |
| exceptions logged | 0 | 0 |

- Lane counters at the end: host `tx=851 txFail=0`; guest `rx=1305 queued=1025 noBody=335 rebind=62`. `noBody` = the
  guard had no linked copy when its switch arrived; those are the 11 % not played.
- **Cause named for the animation half (candidate i, mechanism S1 + S7): CONFIRMED.** A guard's action reached the copy
  a median 331 ms late because the entity lane ticks every 6th frame on both copies and the alias is issued on the third
  apply; hits and warnings ride the intention lane every frame. Moving the action switch onto the same lane removes
  about 290 ms at the median.
- Candidate (iv), logging hitches: KILLED for the idle pair (0 frames over 25 ms in 30 consecutive 5 s windows on both
  copies with `[MirrorAct]` logging on). NOT COVERED: a fight with many guards.

### Run 2b: scripted fight and one owner fight on the same pair: no attack data
- `tools\two_copy_fight.py 30264 --sec 20`, then `--no-goto --mode none --sec 60`: 0 `[FI] tx`, 0 guard attacks. The
  script walked the guest to (31, -75), 47 m from the host player at (76, -74).
- The owner then fought there (about 40 s, guest window in front). Guest: `[FI] tx enter/attack/attack/exit`, 13 damage
  requests. Host: `[FI] apply ... target 00000000F000CAD0 unresolved` (both targets), `[OD] ... kills=1 unresolved=12`,
  `[AF] seen warn/hit=0/0`. The host log has no other line naming either guard id.
- So at 47 m from the host player the guards the guest fights are not the host's guards (no host identity), the puppet
  fight never starts on the host, and no host guard attacks the puppet. That is candidate (v), guard placement /
  identity, seen once (n = 1 fight, 2 targets). It gave no hit-timing numbers.
- The owner stopped the session here (2026-11-17 19:13). B1, B2, B3 (in a fight) and B5 are **not measured**.

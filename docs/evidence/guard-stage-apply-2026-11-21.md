# Guard stage on the guest (plan step 2, 2026-11-21)

Handover 3.2: the guard alert `stage` (HumanGroundStage) was captured, sent (`EF_STAGE`) and decoded, and nothing on the
guest applied it. Each game ran its own guard state.

## Scope (static, with receipts)
- The per-guard value is `HumanGroundStage` (`Human` data bundle, `m_GroundData.m_HumanGroundStage`; 1 Navigation, 4 Fight,
  12 RangeCombat, 19 GetTackled; PS3 DWARF enum matched on PC, `ops/evidence/guard-ai/dwarf-enums.md`).
- The world-level detection statechart (Unaware / Investigating / Alerted / InConflict at holder+0x8E0, 46 of 46 predicate
  callers) is **not per guard** and is not touched (`docs/evidence/guard-alert-apply-S4.md` on `re/guard-alert-apply`).
  The per-guard HUD icon (ConflictIcon module 0x12, `sub_FABC00`) is not handled here.
- The engine's own transition into and out of Fight is the `IHumanGround` Can/DoEnterFight (slots 75 / 76, `const
  Handle&`) and Can/DoExitFight (112 / 113, `ExitFightType_Normal`) pair. It is VERIFIED live on the puppet
  (`s7-fight-intents-2026-11-15.md`, `FightIntentApply`). HumanGround = `*(Human+0x720)` with vtables `0x026E8AE8` /
  `0x026E7DC0`.
- The guard's fight target is read on the host with the `sub_15DABC0` layout (`guard_poi.cpp` `FightTargetIs`).

## Build: `[Coop] GuardStageApply` (default 0, re-read every 2 s; needs `[Coop] GuardActionLane`)
- Wire: the two spare bytes of the `IC_ACTION` payload carry `stage + 1` and the target class (1 = the puppet = the
  guest's own player, 2 = the host player, 3 = another body). The host publishes when the alias, the stage or the target
  class changes.
- Guest, on the copy's own update: host Fight and copy not Fight -> CanEnterFight / DoEnterFight with the mapped target;
  host not Fight and copy Fight -> CanExitFight / DoExitFight. At most one try per copy per 500 ms. No field is written.
- Probe (runs whenever `GuardActionLane=1`, so the flag is an A/B): per copy frame, host stage vs copy stage
  (`[GSA] ... frames agree hostFight bothFight`).
- Double-attack probe (`CombatTimingProbe`): every FightDamageEvent a local NPC posts to the guest's player that was not
  forwarded by `AttackForward` (`[CTP] G native ... linked=`).
- UNVERIFIED before the run: whether a copy whose behaviour component is disabled accepts Can/DoEnterFight.

## PASS bar (written before the run)
| id | measure | bar |
|---|---|---|
| G1 | frames in which the host guard is in Fight and its linked copy is in Fight too (`bothFight / hostFight`) | flag on >= 80 %, and higher than flag off in the same session |
| G2 | `[CTP] G native ... linked=1` with `ph=1` (a linked copy hitting the guest's player by itself) | 0 with the flag on |
| G3 | stability | 0 `EXCEPTION`, 0 `[GSA]` slot faults, guest frame p95 median < 25 ms, both players alive or a death explained |
| G4 | owner: the guards on the guest look and react like they are in the same fight as on the host | AWAITING OWNER |

## Runs
- 2026-11-21: run not started (usage limit); flag built, gated (spine OK, signatures PASS), never run. Next: the scripted A/B per the bar above, then owner G4.

## Run 2026-11-21 (owner-driven, `GuardActionLane=1` `GuardStageApply=1` `CombatTimingProbe=1`)

Build `48CFB068` (2,756,608 B — debug-sized; note for G3 frame numbers), host pid 21692, guest 17372, ini as above, one fight at Havana.

| id | result |
|---|---|
| G1 | **66 %** — `hostFight=1940 bothFight=1280` → **FAIL** against the 80 % bar |
| G2 | **PASS** — 10 `[CTP] G native` lines, 4 with `linked=1`, **all `ph=0`**; none with `ph=1` |
| G3 | 0 exceptions on both logs; guest frame p50 16.6 ms, **p95 18.2 ms**, p99 21.8 ms, max 30.0 ms (>25 ms: 1 of 299); host p95 18.0 ms |
| G4 | awaiting owner |

```
[GSA] apply=1 copies=34 frames=171833 agree=170414 hostFight=1940 bothFight=1280 |
      enter try=23 ok=3   exit try=14 ok=7   noTarget=0
```

**The informative number is `enter try=23 ok=3`.** `noTarget=0`, so target mapping is not the failure: the engine's `CanEnterFight` refused the copy in 20 of 23 attempts. That is the exact risk this file listed as unverified before the run — *"whether a copy whose behaviour component is disabled accepts Can/DoEnterFight"*. Answer: mostly **no**.

Caveats recorded rather than smoothed over: the bar's ON-vs-OFF comparison was not run in the same session, so 66 % is an ON-only measurement; and both processes had exited by the time the logs were read, so G3's "both alive" could not be confirmed after the fact (0 exceptions were logged).

**Next:** `ops/evidence/enter-fight-refusal-2026-11-21.md` — decompile `CanEnterFight` and identify which condition refuses a behaviour-disabled copy.

## Run 2026-11-21 #2 — `GuardStageForceEnter=1` (commit `b0eecc9`, ASI `C815ABC6`)

The gate was identified and bypassed: `CanEnterFight`'s failing condition is `|self.z − target.z| < 3.0f`. Slot 76 is now called directly.

```
[GSA] apply=1/1 copies=43 frames=179367 agree=176289 hostFight=6584 bothFight=4880 |
      enter try=55 ok=0 exit try=26 ok=13 noTarget=7 forceSkip=24
[GSA] force enter gid=4 dz=0.00 target=333F56A0 ok=0      (x55, all zero)
[GSA] skip gid=378 copy=3110FA90 …                        (x24)
```

| id | result |
|---|---|
| G1 | **74.1 %** (4880/6584) — up from 66 %, but **not from this path**: `enter ok=0` |
| G2 | no `linked=1 ph=1` seen |
| G3 | **2 exceptions** — one `resolve/build 0xC0000005` (known intermittent puppet-spawn fault), one `[FI] EXCEPTION 0xC0000005 in slot 160` (`PerformCombo`, separate bug) |
| G4 | awaiting owner |

**Two distinct populations, not one:**
- **55 attempts with `gid=4` and `dz≈0`** — the dispatch id is right and the distance gate is satisfied, yet `DoEnterFight` returns 0. So `Do` has its own internal refusal, not the `Can` gate.
- **24 copies with `gid=378`** — not in the Ground context at all. No enter is possible for them regardless of either call.

**Next:** `ops/evidence/do-enter-fight-2026-11-21.md` — the return-0 branches inside `0x1D79A50`, and what `gid=378` means.

## VERDICT on this feature (2026-11-21, after the static pass)

**`GuardStageApply` cannot work while the copy's behaviour component is disabled.** `DoEnterFight` returns 0 at `0x1D65D72` because state word **107** is absent from the state-path vector at `helper+0xE0`, which is a per-tick snapshot produced by the copy's own state machine. The mod disables the component, the machine never ticks, the vector is empty, and no `Can`/`Do` variant can get past it. Bypassing `Can` did not help because `Do` checks the same data one level down.

`gid=378` copies are also permanently ineligible: that value is the state-path **leaf** (`path[2] == 377`), not a movement context, so such a copy can never accept an enter fight.

**The real prerequisite** is Unity's arrangement: the replica keeps its **native state machine and animation**, and only the **AI decision layer** is off. `ops/evidence/ai-off-seam-2026-11-21.md` is the job that looks for that seam. Until it is found and used, the flag should stay **0**, and this feature is recorded as blocked-by-architecture rather than blocked-by-gate.

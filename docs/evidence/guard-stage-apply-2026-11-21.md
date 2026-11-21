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

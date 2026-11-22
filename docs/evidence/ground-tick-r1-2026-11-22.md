# R1: the Ground statechart ticks on bound, disabled guard copies on the guest (2026-11-22)

Question (R1 of `ops/evidence/guard-ai/brain-design-2026-11-22.md` §4, bf-coop repo): does the Human Ground statechart
tick on a body the mod has bound and disabled on the guest? The shared-brain design needs it, because IHumanGround
commands only fill a request record that this statechart consumes on its tick (state 168).

## Build and probe
- `feat/integration-1121` @ `87e6cb9`, asi `8433cf798be94acf…`, `tree=clean` in both `[Build]` lines.
- `[Coop] GroundTickProbe=1` (new, default 0). It swaps the Ground context update slot `0x026E8B24` (vtable
  `0x026E8AE8` slot 15; holds `sub_1D80950`, its only reference: IDA cc/r1) for a counting thunk that calls the
  original unchanged, keyed by context. It logs every 2 s, per role: the call delta, and the active-state count
  (`u16 helper+0x11E`) and first id (`*(helper+0x118)`), with helper = `ctx+0xA0`.
- Roles: **copy** = a guest body at the bind site right after `SetEnabled(false)` on its behaviour and
  `m_DriveEntity = false` (`phase1.cpp`, guest mirror tick; noted only when not locally dead); **native** = a host body in
  `CaptureGuards` (AI on); **puppet**; **player**.

## Run
- Two copies started from the exe (host 35792 top-right, guest 21008 bottom-left), save at sea (-4035, 4087), 11:33.
  Saves backed up first (`ref/AC4Coop-lobby/backups/saves-437-20261122-113024-r1`). Rig lock held; ini bytes and the
  deployed asi (`d2a3b0cd…`) restored afterwards. No dump; both alive throughout.
- After about 60 s at sea, both players were teleported to Havana (72/75, -74, 3) with `tools/pilot.py`. **The Havana
  population did not stream in** (`chars=2/2` on both, `scan` finds only the two players), and both players went into
  Swim (`puppet behavior disabled (ctx=10)`). The street-guard half of the plan therefore measured nothing.

## Result (the at-sea part, before the teleport)

| role | side | bodies noted | bodies whose Ground update ran in a 2 s window | per-body calls per 2 s |
|---|---|---|---|---|
| copy (bound, behaviour disabled, drive off) | guest | 32 | **23-30** (14 windows at 23, one at 30/30) | ~111-118 |
| native (AI on) | host | 32 | 14-16 (one window at 30) | ~120 |
| puppet | guest | 1 | 1 (82 of 82 windows) | ~111 |

- Three copies tracked individually: 16 of 16 consecutive windows ticking (about 32 s each), active-state count 4-6.
- Disabled copies tick at the same per-body rate as native bodies, and a larger share of them tick.
- **Verdict on the tick criterion: PASS.** On the guest, the Ground statechart keeps running on bodies the mod has bound
  and disabled. n = 1 run, about 30 bodies, ship-crew humans at sea, not Havana street guards; same bind code path.

## Seen, not settled
- When a body leaves the Ground context, its update stops: the local player's Ground calls went to +0 and its active
  count to 0 exactly when it entered Swim (ctx 10) after the teleport. Consistent with "the Ground update runs only while
  Ground is the active context". UNVERIFIED as a rule; it means a +0 window is meaningful only for a body in Ground.
- Bodies that never ticked (2-9 of 32 copies, ~16 of 32 natives) were not identified (other context, streamed out, or
  dead). Not investigated.
- Not run: the second half of the pre-registered check (a slot-160 call on a copy followed by state 173). The tick
  exists; whether a command on a copy plays the move is the next test.
- Teleporting from sea into Havana does not load the town's population. A street-guard run needs a save that starts in
  Havana (11-15 era saves did) or a sail-in.

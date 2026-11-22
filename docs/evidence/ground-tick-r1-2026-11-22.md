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

---

## Part 2: does a command on a copy play the move? (`[Coop] CopyCmdTest`, same day)

Pre-registered (in `src/phase1.cpp` above `CctOwnUpdate`, before any run): from the body's own update, DoEnterFight
(IHumanGround slot 76) on the local player, then 1.5 s later PerformCombo (slot 160: target, n = 3, end = 1, 0);
**PASS** = state 173 in the active path within 2 s of the slot-160 call.

| run | build (all `tree=clean`) | bodies | DoEnterFight (low byte) | node list (`fsm+0xE0`, count `+0x106`) | 173 after slot 160 |
|---|---|---|---|---|---|
| guest 24272 | `31b4225`, asi `fbc9d94c…` | 2 distinct copies (pirate crew at sea) | 0, 0 | not logged | no |
| guest 11328 | `bcfb761`, asi `c9de4213…` | 2 copies | 0, 0 | **empty** (`nodes=0`, `n107=0`) | no |
| host 5980 (control, AI on) | `ae2a46a`, asi `66e709b0…` | 1 native crew | **0** | **empty** | no |
| guest 24004 | same | 1 copy | 0 | empty | no |

- **Verdict against the bar: FAIL** (0 of 5 copies), and **not attributable to the copy being disabled**: the AI-on native
  control refused the same way, with the same empty node list. Every body here was the player's own pirate crew at sea,
  asked to fight its own captain; no hostile human was available on this save.
- The copies' state machines were visibly running: the active path changed every ~0.35 s (`1/2/3/4/5/6/7` ↔
  `1/2/3/4/55/56`), matching part 1.
- PerformCombo returns 5 in every case; it only fills the request record, which nothing consumes outside fight idle
  (state 168).
- **Correction to `guard-stage-apply-2026-11-21.md`:** its verdict "the machine never ticks, the vector is empty"
  has the first half refuted (part 1: it ticks). The empty node list is real, but it is empty on an AI-on body as well,
  so ticking alone does not put node 107 there. Also, that run's `enter ok=3` counted CanEnterFight successes
  (`ok` is incremented whenever `rc & 0xFF`, before DoEnterFight's result is read), not successful enters.
- **Lead (UNVERIFIED):** the one place slot 76/160 is known to work live is the puppet, and the puppet's behaviour
  component is **enabled** whenever it is in Ground (`HybridLoco`, `phase1.cpp` "GROUND context -> puppet's own FSM owns
  actions: behavior ENABLED"). Guard copies run with it disabled. If the producer of node 107 sits behind the behaviour
  component, the design's second branch applies: copies keep behaviour enabled and the decision hooks suppress their
  own choices. Not proven: the at-sea control could not separate "behaviour off" from "friendly target".
- A save swap to the 11-15 01:03 backup did not change where the game loads (still at sea); saves were restored and
  hash-verified. A hostile-target run still needs a Havana start or a boarding.
- Static next (rule 9): find what writes node 107 into `fsm+0xE0` (`+0x106` count); the earlier byte sweeps for direct
  `disp32` stores found no writer (`ai-off-seam-2026-11-21.md` §8), so it goes through a register-held base.

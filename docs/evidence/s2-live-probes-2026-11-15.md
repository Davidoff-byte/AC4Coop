# S2 live probes on Edward (2026-11-15 21:30-21:50)

Probes for `docs/evidence/s2-sp-intent-seams-2026-11-15.md`. Build: AC4BFSP.exe pinned (bc11ad79...); ASI
7ba0f7f6be0610d427cb280a70ea0e3a585b9c6ec011b95db8129b6cbaf69885 (`856d712`, tree clean, `[Build]` line of pid 25532). One copy,
started directly from the exe, booted into the world with `pilot.py boot`. Saves were backed up first
(`backups/saves-437-20261115-213852`).

## Method
- New test command `kCmdHgCall` (`src/pilot.cpp` HgCall, client `tools/pilot.py hg`). It runs on Edward's own Human update
  (`Pilot::OnPlayerFrame`).
  - It finds HumanGround = `*(Human+0x720)` and checks the vtables 0x026E8AE8 / 0x026E7DC0.
  - It calls an interface slot through the interface's own vtable with the exact argument count, inside SEH.
  - A Do slot runs only if its Can returned non-zero (or when no Can is given, for the negative control).
- It logs the ground FSM path (`*(u16**)(fsm+0x118)`, fsm = `[IHumanGround+0x90]`) and `GetCurrentContextID(1)` before the
  call, then on every change for 120 frames (`[HG] watch` lines in `phase1_log_25532.txt`).
- Every value below is quoted from those log lines or the `pilot.py hg` output.

## Results
| # | Call | Result | Verdict |
|---|---|---|---|
| 1 | GetFightStage, slot 300 (no args) | returns 0x27 (39); path 1/2/3/4/5/6, ctx 4 | **small enum, as predicted**. Meaning of 39 not pinned (MP: NotInFightStance; SP numbering may be shifted by one) |
| 2 | GetHumanGroundStage, slot 689 | returns 1 | **small enum, as predicted** (MP: 1 = Navigation) |
| 3 | CanAttackNow 163 → DoAttackNow 164 | Can returns 0, so Do is skipped; Do alone returns 0; path unchanged | **consistent with stubs** |
| 4 | DoEnterFight 76 alone (negative control), target = Pirate 2EB9FE80 | returns garbage eax (0x3B626A00), path unchanged | **Do is gated on its Can**, as decompiled |
| 5 | CanEnterFight 75 → DoEnterFight 76, Pirate 2EB9FE80 | Can 1, Do 1; next frame path **1/2/163/166/167/168** (state 163 = Fight at path[2]); +2 frames back to 1/2/3/4/5/6 | **PASS** (the predicted `path[2]==0xA3`) |
| 6 | Same pair, Templar-role NPC 3D46C1A0 | Can 1, Do 1; same one-frame Fight, then back to Move | **PASS**, reproduced (2/2) |
| 7 | Same pair while in the teleport landing state (path 1/597/601/0/0/0) | Can returns 0 | **source-state check confirmed** (Can needs Move) |
| 8 | CanEnterClimb 382 → DoEnterClimb 383, standing 0.8 m from a 3 m wall | Can returns 0 | see below |
| 9 | Same, every 100 ms while walking into the wall (world intent +y, no free-run); path 1/2/3/4/26/27 at the wall | Can 0 in 29 of 29 calls; walking alone (control) did not climb either | see below |

### Why CanEnterClimb refuses (decompile of the scratch db copy, `ida_batch` md5 2058342866688f780c8b34526a65bc35)
- `sub_1D5A500` (slot 382): lazily creates the FSM, requires `path[3]==4`, calls `sub_1D4B070(scope = fsm+0xC)`, and on a
  result of 2 adds transition id 131 (`sub_59C660(131)`).
- `sub_1D4B070` first tests `sub_16FEAE0(scope+0x270, scope+0x260, 0)` and returns 1 (fail) if it is false. It then reads 4
  entries of 96 B at `[scope+0x260]` and a vec4 at `scope+0x280` before returning 2.
- So the Can needs a **climb candidate already written into the scope**, by the player's climb search during a real
  free-run or climb. Walking into a wall does not produce one.
- That fits S1 section 4 (four limb items in the MP ledge/climb attach data) and the plan's S6 point that the attach data
  must travel with the transition. UNVERIFIED that the 4x96-B array is the same data as MP's limb items. Who writes
  `scope+0x260` was not traced.

### Why the fight lasts one frame
- On Edward the Fight state (163) is left on the next frame. A hostile was absent: the save area has 75 loaded characters
  (Peasant 44, Pirate 20, Animal 5, Courtesan 4, Templar-role 1) and no soldiers.
- A likely cause is that the player's own decision or input layer re-asserts Move each frame. UNVERIFIED.
- So Attack (154), PerformCombo (160) and Can/DoExitFight (112/113) could not be tested on Edward. They move to the
  puppet, with its AI off, on two copies (S7).

## Not done
- Probes 3-5 of the S2 list (Attack, ExitFight, PerformCombo): need a lasting fight.
- Probe 8 (the AI-off seam, a detour of 0x0055DEF0): needs a hook and a guard. Not run.
- Climb with a real candidate: run CanEnterClimb while the player free-runs into the wall, or write the candidate. Open.

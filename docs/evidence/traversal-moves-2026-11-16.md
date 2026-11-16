# Traversal moves decoded: contexts, transitions, the climbing condition table (2026-11-16)

Goal (user): decode the whole set of traversal moves for clean sync, and find what lifts the hanging native replica onto
the "beam" (`climb-moves-2026-11-15.md`, rounds 2-3).
- Static RE: AC4BFSP.exe md5 `2058342866688f780c8b34526a65bc35`, ida_batch tr1-tr7.
- Live: `[Coop] TraversalProbe=1` (`src/traversal_probe.cpp`, read-only hooks), on solo-replay runs 26516 and 568
  (asi 2a55e516 / 723b1157).

## 1. Context ids are ActorContextID (names from the PS3 SP DWARF, `ops/evidence/ps3_enums_focus.txt`)
4 Ground, 5 Ladder, **6 InAir**, **7 Ledge**, **8 Walling**, **9 NarrowObject**, **10 Swim**, 11 Boat, 13 RagdollGround,
15 CustomAction, 19 Dead, 20 HayStack, 25 ShipWheel, **30 Climbing**, **31 Hanging**, 41 Rope, 43 HangRope, 47 ShipDeck (48 in all).
- PC numbering matches on every observation so far: 4 while walking; 6 between take-off and landing; 10 after falling into
  the sea; 30 when grabbing a wall.
- **"Beam" (ctx 9) is NarrowObject.** It covers beams and poles, and the player also enters it on ordinary landings
  (6 -> 9 -> 4).

## 2. How a context changes (verified)
- The context manager is the Human itself (AIActor; `GetCurrentContextID` 0x0052C2E0 reads its per-track slots).
- **Exit** `sub_52C330(Human*)` (367 callers) → the transition function's own setup → **Enter** `sub_52DFF0(Human*, id)`
  (thiscall, `retn 4`, 84 callers).
  - Enter makes the new context with factory vt+0x20 (the factory is at slot+0x2C) and calls the new context's vt+0x30.
  - The slot is `Human + 8 + 48*layer`; layer is byte +0x100.
- `sub_52DF90` = TransitToContext (exit + enter, 1 caller).
- Context updates run from `sub_52C220`, which calls each track's current context vt+0x3C every frame
  (0x52C249 ← 0x52DF87 ← 0x5500DC ← 0xA327A6).

## 3. Observed moves (old → new context, transition function = return address of the Enter call)
| move | from → to | transition (Enter return) | Exit return | seen on |
|---|---|---|---|---|
| start a wall run | Ground → Walling (4 → 8) | 0x01D65A11 (Ground's generic enter) | 0x01D6B519 | replica |
| take-off (run / jump) | Ground → InAir (4 → 6) | 0x01D65A11 | 0x01D6B0DC / 0x01D6B2FA | player |
| step up onto a narrow object | Ground → NarrowObject (4 → 9) | 0x01D65A11 | 0x01D6D61D | player |
| wall run → grab | Walling → Climbing (8 → 30) | 0x01756D5A | 0x01757BAF | replica, player |
| **climb → onto the narrow object above** | **Climbing → NarrowObject (30 → 9)** | **0x01583840** (from the evaluator 0x01585379) | 0x015837E8 | replica |
| climb → back into the water | Climbing → Swim (30 → 10) | 0x01583840 | 0x015837E8 | player |
| land | InAir → Ground / NarrowObject / Ledge / Swim (6 → 4 / 9 / 7 / 10) | 0x01DC6EF0 | 0x01DC6E98 | both |
| ledge → narrow object | Ledge → NarrowObject (7 → 9) | 0x01DA156B | 0x01DA1523 | player |
| narrow object → ground | NarrowObject → Ground (9 → 4) | 0x015854D0 | 0x01585478 | player |
| narrow object → jump | NarrowObject → InAir (9 → 6) | 0x01581CAA | 0x01582704 / 0x015829A1 | both |
| swim → climb out | Swim → Climbing (10 → 30) | 0x01112010 | 0x01111FB8 | player |

## 4. The Climbing context decides its own exits from a condition table (verified)
- The context's update (`sub_1585360`) calls the evaluator `sub_1583780`. The evaluator walks the current state's list of
  condition ids (`u16[]` at this+0x54, count u16 at this+0x5A) and calls `sub_1581EE0(id, arg)` for each. The **first
  non-zero result is the move**:
  - low 10 bits < 23: a climb sub-state (`sub_15759E0` / `sub_1576B40`, then `sub_1581E30`);
  - low 10 bits ≥ 23: leave Climbing for context (value − 23), via Exit and then Enter(value − 23).
  - Bits 10-19 and bit 30 are passed on.
- `sub_1581EE0` is thiscall (list, u16 id, arg) with `retn 8`: a switch over ids 1-22, one condition function each.
  The context targets that can be returned:

| id | function | possible exits (context) |
|---|---|---|
| 1 | sub_156FE40 | 33 → **Swim** |
| 13 | sub_1577BC0 | 29 → **InAir** |
| 14 | sub_1540C40 | 29 → InAir |
| 15 | sub_155C5C0 | 29 → InAir |
| **17** | **sub_156FF80** | **32 → NarrowObject**, 31 → Hanging (asks `sub_156BD20` with the Walling and NarrowObject contexts; code < 14 → NarrowObject, 14-16 → Hanging) |
| **19** | **sub_1565810** | climb sub-state 20 (19476 = 0x4C14), **27 → Ground**, **32 → NarrowObject** (the "over the top" chain: `sub_1538950`, then `sub_1552CD0`, then `sub_1553380`) |
| 21 | sub_1575A30 | 33 → Swim |
| 22 | sub_1570070 | 29 → InAir |
| 2-5, 7, 8, 10, 11, 20 | sub_1581A80 / 1540C00 / 156FF60 / 15381B0 / 15381E0 / 1538210 / 1538250 / 156B510 / 15658E0 | climb sub-states only (no context exit in the decompile) |

- Climbing → NarrowObject is therefore one of **conditions 17 or 19**. Live, which one fires for the replica is
  **not yet captured**: the probe on `sub_1581EE0` is in, but runs 568 and the 3-replay run had no wall entry by the
  replica, and the rig was busy.
- Gate of the over-the-top move (`sub_1553380`): refused when `a1[58] == 3` or when `sub_666DC0(0,0)` (a query on the
  current action, via `sub_5FC110` = the action component) is false. Otherwise it sets a mantle action
  (`SetTargetAction` 0x6625D0 with 1010825457, or `unk_4BDB58D` when `a1[58] == 2`).
  - So the move is gated by the **current climb animation** and a mode field `a1[58]`. **UNVERIFIED** what the mode
    field means and what sets it (a climb "intent" written from input is the lead).

## 5. The replica's hang → narrow object, live: condition 17, and the exit gate
- **Run 4220** (asi 723b1157, 3 replays, probe on):
  - The replica entered at 191726542 (8 → 30) and left at 191728441 through
    `[TR] puppet climb-cond id=17 -> context 9` (30 → 9), about 1.9 s later.
  - During the hang its climbing input was **zero**: `climb intent ... v=(0.00,0.00) speed 0.00 | real=0 derived=0`, with an
    on-wall error of 0.20 m.
  - **The player fires the same condition**: 8 → 30 at 191843287, then `player climb-cond id=17 -> context 9` at 191846083,
    during the move script's free-run (RT + A + stick). So the transfer is native free-run behaviour, not a fault.
- Condition 17 (`sub_156FF80`) gets the Walling and NarrowObject contexts (`sub_52C360(8/9)`) and runs `sub_156BD20` on a
  helper it caches (`sub_156B480(1)`: 0x70 bytes, `this+0x1C`). In the disasm the helper is `ecx` (0x0156FFE3).
  - `sub_156BD20` first requires `sub_155C9B0(helper)`, then switches on helper+0x58 (cases 2 and 5-9) and checks
    helper+0x24 == 10.
  - Why the replica's helper qualifies while the hanging recorded peer did not is **not established** (the helper's
    state, not our input).
- **`[Coop] NativeReplicaClimbExitGate`** (default 0, `src/traversal_probe.cpp`):
  - for the puppet only, condition 17 is not evaluated while the peer's newest context is still Climbing (30). The hook
    returns 0 before calling it, as when it does not apply, so none of its side effects run;
  - once the peer has left Climbing, it runs as usual.
- **Run 32644** (asi 976ec99f, 3 replays, gate on, `0:0:0:1:1`):

| run | wall entry | after the entry | both climbing (all windows) | replica on a narrow object alone |
|---|---|---|---|---|
| 1 | 8 → 30 at 197353081 | condition 17 held (7,220 times over the run); **30 for ~68 s**, then 30 → 9 → 6 → 4 at 197420961, after the peer left Climbing | **78 %** | 4 % |
| 2 | none | - | 0 % | 0 % |
| 3 | 8 → 30 at 197535138 | **30 for ~73 s**, then 30 → 9 → 6 → 4 | **84 %** | 4 % |

  Before the gate (runs 30324, 26516, 4220): both-climbing 0-5 %, and the replica on the narrow object within ~2 s of
  every entry. **PASS** on the stated bar: no Climbing → NarrowObject while the peer climbs.
- **New problem, now visible:** on the wall the replica keeps up badly. Its mean on-wall error is 1.1-2.5 m, often frozen
  (the same error over many 2 s windows), while the peer shimmies. Hold-by-hold following (`NativeReplicaClimbMoves`) was
  off in this configuration. The next A/B is the gate plus moves.

## Not done yet
- The live condition id for the replica's Climbing → NarrowObject, and the gate values (`a1[58]`, `sub_666DC0`) for the
  replica vs the hanging player.
- The Ground context's own moves: the HumanGround statechart, 644 states, `ops/evidence/guard-ai/statechart.md`.
- Ledge, NarrowObject, Hanging and Swim condition tables (each context has its own evaluator; not yet decompiled).

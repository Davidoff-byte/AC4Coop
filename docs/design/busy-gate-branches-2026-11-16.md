# The busy gate, branch by branch: why a guard refuses the replica (static, 2026-11-16)

`sub_1608960` is the function that decides whether a candidate is worth fighting. Callers: `sub_14AFA00`,
`sub_14BCB10` (the FightTask context builder), `sub_16103E0`. At the `sub_14BCB10` call site the arguments are
`ecx = guard`, `a3 = *(esi+0xC)` (candidate), `a4 = *(esi+0x278)` (context; `a4+0x366` must be 0 to even call), and a
`1` return sends the guard into the busy branch (`[esi+0x80] |= 0x10`). All tags `[DECOMP]`, single binary
`AC4BFSP.exe`, idb `D:\ac4ida\enemy`.

## Branches, in order

| # | test | returns |
|---|---|---|
| 0 | `!a4` or `!a3` or `!*(a4+64)` | 0 (not busy) |
| 1 | `*(guard+952)` set AND `v5 = decode(*(guard+956))` non-zero AND `a3 != v5` | **1** |
| 2 | `v6 = sub_6F5FF0(decode(*(v4+0x14)))` is null | skip to 6 |
| 3 | `v7 = v6->vt[18](17)` is null | `sub_52C2E0(v6,1) != 16` -> 6, else **1** |
| 4 | `v8 = v7->vt[300]()` in {9,33,74} while a global switch is set; or in {10,77,11,38,13,12,25}; or `v8==52 && v7->vt[288]()==7`; or `v8==67` | **1** |
| 5 | `v10 = v7->vt[689]()`: `==19` -> `v7->vt[44](&tmp) != 0`; else **not** in {1,4,5,10,12,13,14,15,18} | **1** |
| 6 | `*(a3+92)` and `*(that+2344)` and decoded handle non-zero, then `sub_176D4F0` / `sub_1770A40` (weapon reach) | **1** |
| - | otherwise | 0 |

(Index arithmetic: `vt[300]` = 1200/4, `vt[689]` = 2756/4, `vt[288]`, `vt[44]`.)

## Why this closes the loop for a replica

Two independent branches both fire, and neither needs a guard to be "wrong" about anything:

- **Branch 1** is the one pass 3 exposed: while a guard's current target is Edward, every other candidate differs from
  `v5` and is declared busy before any capability read happens.
- **Branch 5** is the structural one: `vt+2756` is `HumanGroundStage` (**not** a behaviour order - see corrections doc). The allow-list is
  {1,4,5,10,12,13,14,15,18}; a replica that runs no ground behaviour has no reason to hold one of those, so even a
  guard with no target would call it busy.

The puppet therefore loses twice: it must beat the current-target test *and* present an acceptable ground stage. This
is also why `FightOrderPuppet` alone cannot clear this gate: it writes behaviour order 2/17 (`sub_13F22E0` -> nav
service vt+64) while the gate reads a ground stage from a different object. Corrected in `corrections-2026-11-16.md` C1.

## Status: branch 1 is a SYMPTOM, not the cause

`ops/evidence/guard-ai/decision-tree.md:106-111` refuted the busy lead live (run 22156: `TgtBusy` in 0 of 115 samples)
- but in a setup where guards were **already fighting the held puppet**, so `a3 == v5` and branch 1 is false there.
Our pass 3 is the complementary setup (guards fight the player), where branch 1 fires for every other candidate.
Both are true; neither makes busy the root.

The root is **fight-group membership**, which pass 3 measured directly (`pupInGroup=0`):

- `sub_161BD30` only hands out order 2 to members whose record `+0x2C == 1`; `fightmanager.md:149-153` states the FM
  itself never issues order 2 to a fighter whose `+0x2C != 1`.
- Membership arrives as `sub_1620350(fm, e, 1)` ("added to the target's attacker list", `fightmanager.md:158-172`),
  whose status-1 callers are the guard's own statechart action `sub_1397450` and the FM's start-fight
  `sub_1625590`.
- `sub_15DABC0(fm, rec)` *keeps* a target record only while the target's **`entity+0x10`** word is present in the dword array at `fm+0x30C` (count `u16` at `fm+0x312`; read at instruction level, 0x015DABDB-0x015DABFF); otherwise it clears
  `rec+0x40`, calls `sub_156CB00(rec,0,1)` and marks the record `+0x180 = 1`.
- `sub_15E5B70(fm, rec)` is the only writer of the target slot `fm+0x304` (`fm+772`), and per `fight-target-drop:89-91`
  nothing restores it.

So patch A (never-busy) can at best let a guard continue a fight it has not joined. The sequence that matters is
**attacker-list add -> team validity -> order**. A detached `fm+0x304` write would be undone by `sub_15DABC0` on the
very next tick if the Team fails.

`facP=00000000` in every sample (passes 1-3) raises the team question first: if the replica's team word is 0 and 0 is
not in `fm+0x30C`, no target write can survive, and that is the thing to prove before writing anything.

## The previous patch table stands, with A re-graded

| # | change | grade now |
|---|---|---|
| A | never-busy detour | symptom relief; cannot create membership |
| C | give the replica a valid order | still needed for post-membership behaviour |
| D | **new**: make the replica a valid *target* (attacker-list add + Team in `fm+0x30C`) | the actual gate |



| # | change | cost | risk |
|---|---|---|---|
| A | one detour on `sub_1608960`: if `a3 == Phase1_PuppetEntity()` return 0, else call through | 12 lines, read-only logic | a guard may then fight a body with no attack chain, so it circles and whiffs |
| B | A, but only when the replica is flagged (own ini key), so the ladder can A/B | +flag | same |
| C | give the replica a real order (write `vt+2756`'s source) so branch 5 passes naturally | needs that source named | larger, but fixes it for every caller |

A is the smallest thing that turns pass 4 into a real measurement; C is the honest fix.

## What pass 4 must show

With A active and a guard next to the replica, `[PoiFO] pupInGroup` / `enlisted` going non-zero and `pupInGroup=1`
precedes any damage. If the guard fights it but nothing lands, the busy gate was necessary and not sufficient - then
branch 5 (C) is confirmed as the real one.

## UNVERIFIED

- That `vt+2756` (branch 5) is the order the ledger calls "C17 vt+2756" - same index arithmetic, not yet proven to be
  the same read.
- That a replica holds a value outside the allow-list at all; it has not been sampled. If it happens to sit inside the
  list, branch 1 alone is the stop and C is unnecessary.

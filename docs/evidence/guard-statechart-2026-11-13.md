# Who drives a mirrored guard's actions: the Human context statechart (static, 2026-11-13)

Binary `AC4BFSP.exe` md5 2058342866688f780c8b34526a65bc35, IDA 9.3 headless (`ida_batch.py`, outputs o21–o26 in
the session scratch). Static only: nothing here has been probed live yet. Purpose: Phase R of
the "guard sync by intentions" plan, which asks whether the second writer on mirrored guards is the AI decision
layer (outcome a) or the human state machine (outcome b).

## Verdict

**(b), refined.** The second writers are state entry actions and transition actions of a generated hierarchical
statechart that runs inside a Human *context*. The Human updates its contexts every frame. The AI behaviour
layer (BhvGenericNPC) is already disabled on mirrored guards (phase1.cpp:7014), and the writes continue anyway.

What this opens up:
- The statechart's active state can be **read** (a u16 stack on the context object).
- It can be **entered natively** through the context's own enter function, and
- its own transitions can be **suppressed** at one point.

So the Unity-style puppet maps onto AC4 as follows: send the host's active state per context, and have the
guest enter it with the statechart's own code, so the entry actions pick and play the animation natively.

## Chain (receipts)

| Step | Address | Evidence |
|---|---|---|
| Human update calls the context update | `call sub_52DF80` @0x016FA857 in `sub_16FA7D0` (data ref 0x026EBBE8) | o16/o18; o22 xrefs |
| `sub_52DF80` = `sub_52C220(this,0)` | 0x0052DF82 is its only caller | o21 |
| `sub_52C220`: 5 tracks at `Human+8`, stride 0x30; a track whose byte is 1 is reset and skipped; otherwise `ctx->vt[+0x3C](a2)`, with ctx = `track+0x14` | decompile | o21:33-56 |
| Track byte +0 = "context switched this frame". Set to 1 by the context switchers `sub_52DFF0` (enter on main track `Human+0x100`) and `sub_52DF90` (exit old via `vt+0x34`, save old type id at `track+0x10`, enter new). New context = `factory(track+0x28)->vt[+0x20](track[+0x2C], index)`; enter = `vt+0x30` | decompile | o22 |
| Context type id = `*(ctx->vt[+0x24]() + 4)` (`sub_52C2E0(human, track)`, default 2). For the class below, `vt+0x24` = `sub_169EB80` returns `ctx[3]` (descriptor at ctx+0xC) | decompile | o22, o26 |
| Context class (name unknown) vtable **0x026E8AE8** (ctor `sub_16D8690`, 7 call sites). Slots: +0x30 `sub_1D65A50` (enter: `sub_1D63440(1)`, the root state), +0x34 `sub_1D65AF0`, +0x3C `sub_1D80950` (update) | bytes + xrefs | o25, o26 |
| Update `sub_1D80950` = lazily create helper `ctx+0xA0` (0xA80 bytes, `sub_1D22EC0`), then `sub_1D65BB0(a2)` | decompile | o26 |
| `sub_1D65BB0` = event dispatch: for each active state (u16 stack at `*(ctx+0x118)`, count u16 at `ctx+0x11E`, root first) call `sub_1D634D0(state, event)`. A non-zero result encodes target = bits 0-9, LCA = bits 10-19, flag = bit 30. Exit to LCA with `sub_1D65960`, then enter the target with `sub_1D63440`. A target ≥ 0x284 switches the Human context instead: `sub_52DFF0(human, target-644)` | decompile | o24 |
| `sub_1D63440(ctx, s)` = enter state `s`: push it (`sub_1CFFFE0`), run its entry action (`sub_1D5E6E0`), then descend through default children while `s` is not a leaf (bitmap `byte_2ABC488`) | decompile | o23 |
| `sub_1D65960(ctx, lca, flag)` = pop states and run each exit (`sub_1D611E0`) down to `lca`; exits `lca` too when flag is 0 | decompile | o24 |
| `sub_1D659D0(ctx, code)` = the same "exit to LCA + enter" from inside a state action. It has 280 callers, the generated transition bodies | o24 xrefs |
| Second writers: `sub_1D15690` ← `sub_1D15910` / `sub_1D15D40` ← entry dispatcher `sub_1D5E6E0`; `sub_1D178B0` ← `sub_1D5E6E0`; `sub_1D138C0` ← `sub_1D174D0` ← `sub_1D1E8B0`, and ← `sub_1D1E6B0` / `sub_1D1E930`, which are referenced as data from `sub_1D22FD0` / `sub_1D23450` ← `sub_1D634D0` (transition evaluation). Each reads `*this` = Human (`Human+0x108` → `sub_5FC110` → the entity's action owner; `Human+0x10C` IHuman `vt+560`) and calls `SetTargetAction` 0x6625D0 with a choice computed from its own fields | decompile + xrefs | o21-o23 |
| `sub_194FC90` (4th writer): called from `sub_18B6520` ← `sub_18BEBB0` / `sub_18CA650`. Plays an action from a data record (`*(*this)+8` alias, flags 0x20001). Owner class not identified | o21 | |

Perception contexts use the same pattern: `sub_1038E00` (21 states, then `sub_52DFF0(human, s-21)`) and
`sub_103B420` (67 states). These are different context classes.

## Not established (open)

- The class names of context vtable 0x026E8AE8 and of the other context classes. The class map
  (`ops/evidence/class-info-map.tsv`) has no Human contexts.
- Entity → Human path. Lead: `*(AIComponent+0x24)` (sub_54DFA0 calls `vt+0x24/+0x28` on it, phase1 notes
  `AIComponent = *(e+0xE8)`). Needs a read-only live check: vptr's vtable holds `sub_16FA7D0` at the slot of
  0x026EBBE8, and `track+0x14` of the main track has vptr 0x026E8AE8 or another context vtable.
- Whether forcing `sub_1D63440` into a state whose entry action reads fields the guest's context never filled
  (targets, timers at ctx+0x80..0xDC, +0x1C70..) faults or misbehaves. Needs a guarded probe.
- What `sub_1D634D0`'s internal reactions do every frame besides transitions. Suppressing dispatch also removes
  in-state reactions (`sub_1D138C0` is one).
- R2 (same-alias `SetTargetAction` idempotence, `sub_658CA0`) was not read. It matters less if states are
  entered instead of actions.

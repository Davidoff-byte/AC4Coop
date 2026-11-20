# Helm entry point — live (2026-11-20)

**Build:** `feat/sail-memento` `8998081`, ASI 753,152 B sha256 `102be3c6…e387bd`,
stamp `commit=8998081 tree=clean` on both copies.
**Rig:** two copies, one machine, `EntityHost` per copy (`b006387`). `HelmSeat=1`.
**Probe:** every 5 s, per instance — read `captain`; if 1 call `0x011FE840(gcl,0)`; then call
`0x011FE840(gcl,1)`; re-read after each.

---

## Result

```
host  pid 32076: [Helm] cycle ship=3C0B2840 gcl=3C5FCA00 captain 1 -> 0 -> 1 ok=1   (x17, exc=0)
guest pid 22836: [Helm] cycle ship=3D0172A0 gcl=3C76DC60 captain 1 -> 0 -> 1 ok=1   (x12, exc=0)
both, last:      [Helm] state … captain=1 drivable=1 solo=0 ship=1
```

`sub_11FE840(gcl, a2)` **works in both directions from an injected mod**, on a live ship, with no
exceptions, and the value persists between samples (post-cycle `captain=1`).

## What this settles

- The helm bool is engine-owned and the engine path is the only correct route. A raw write to `+0x39A`
  would set the field but skip the enter/exit events (`sub_A014A0` / `sub_11F7260`) that the rest of the
  ship state depends on — that is the reason the static job said "call, never poke", and the call is now
  proven callable.
- "Either player can helm" is implementable with one call per transition: `(gcl,1)` to seat a player,
  `(gcl,0)` to unseat. The guard inside the function (`a2 == *(this+0x39A)` → return) also makes it
  idempotent, so a per-frame call would be harmless.

## What this does NOT settle

- **Cross-machine authority.** This probe seats the *local* player on the *local* ship on each instance.
  Who is *allowed* to helm in a shared ship is a separate question (ShipCoop's holder/epoch, or an
  equivalent rule) and is not tested here.
- Whether the enter/exit events fire correctly for a body that is not the local player — untested, and
  the obvious next probe if the feature is pursued.
- The guest and host values are independent per machine; nothing here replicates a helm decision.

## Evidence of a prior inconclusive attempt

Run of the same harness before the cycle change (`353fb2f`, pids 18136/16828): `captain` was already `1`
when the ship resolved, so the enter branch never executed — 0 transitions, 0 exceptions. The cycle form
is what made the path testable. Same lesson as the sail setter: an idempotent engine path needs a
deliberate transition to test, not a "call it when needed" condition.

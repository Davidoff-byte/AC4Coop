# Sail memento — first live run: transport PASS, apply blocked on identity (2026-11-20)

**Build:** `feat/sail-memento` `c60c766`, ASI 752,640 B sha256 `75cbf70c…59733f`, stamp
`[Build] asi=75cbf70c… commit=c60c766 tree=clean` on both copies.
**Rig:** two copies, one machine, `two_copy.ps1`; host pid 20884, guest 21952.
**Flags:** `SailSync=1`, `SailTestCycle=1`, `SailApplyRaw=0`, `IntentLane=1`.

---

## 1. What passed

The lane and the publisher work end to end:

```
host:  [Sail] tx ship=39D80AF0 state=1 wire=0000004026573992 seq=2985362786 ok=1
host:  [Sail] tx ship=39D80AF0 state=2 wire=0000004026573992 seq=2985362786 ok=1
host:  [Sail] testcycle 2 -> 0 ok=1
host:  [Sail] tx ship=39D80AF0 state=0 wire=0000004026573992 seq=2985362786 ok=1
guest: [Sail] rx stats ok=0 stale=0 unresolved=17 bad=0 exc=0 tx=0
```

- The host resolves its ship and **calls the engine setter successfully** (`testcycle ok=1`), so
  `sub_1220960` is callable on a live `GcLShip` — the static reading holds.
- The guest **receives** the messages: `unresolved` climbs 11 → 17 over ~20 s. Transport, channel and
  payload are all working.
- `bad=0`, `exc=0`: no malformed payload, no exception in the consumer.

## 2. What failed, and why it is not a code bug

`unresolved` is the correct behaviour here. Two independent reasons, both measured:

1. **The copies were in different scene cells** in the same world:
   ```
   host : world=73CA8CEE  localKey=3FF55DC9
   guest: world=73CA8CEE  localKey=00058DD2 / 000591D2
   ```
   The guest booted at `(-4370, 2320)` with 21 characters; the host at Havana `(76, -74)` with 102.
2. **The ship mirror is not in this base.** `git merge-base --is-ancestor c82a696 HEAD` → **NO**.
   `feat/launcher` does not contain `ShipMirror` (`c82a696`), so the guest has no object that
   corresponds to the host's ship at all — regardless of cell.

So the memento arrived and had nothing to apply to. That is the design's own §7 dependency: *"The ship
must be resolvable by `wireId` on the guest."*

## 3. The decision this forces

The co-op model on this line is **one ship per player** (`ShipCoop`: each PC keeps its own Jackdaw, the
follower keyframe-follows). Under that model the two Jackdaws are *the same gameplay ship*, so identity
does not need a wireId match — the guest should apply the host's sail state to **its own local ship**.

Proposed change, one place in the consumer:

```
resolve(wireId)            -> apply via sub_1220960
else GetLocalShip() != null -> apply via sub_1220960, log via=local
else                        -> unresolved
```

With a logged route (`via=wire` / `via=local`) so the log states which identity was used, and a note that
the fallback is correct **only** while both players share one ship; with two independent ships it would
apply the host's state to the wrong hull.

The alternative — porting `c82a696` (game-mirror) onto this base — is the right route if and when the
players have separate ships, but it is a much larger port and is not needed for the shared-ship model.

## 4. Still to prove

- The guest applying to its own ship and the **visual** changing (control B still decides that: raw
  `+0xCC` write ⇒ value yes, sails furled ⇒ the setter is what matters).
- Both copies in the same cell, so the world is comparable (teleport the guest to the host's position
  before the test).
- Whether `sub_1220960` is safe on a duplicate `GcLShip` — untested; the guest's own ship is a real
  `GcLShip`, not a duplicate, so this run would not exercise it.

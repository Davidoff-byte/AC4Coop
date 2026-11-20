# Sail memento — three-arm oracle (2026-11-20)

**Build:** `feat/sail-memento` `4a11368` (identity fallback), ASI 752,640 B sha256 `6a482a88…74823e`,
stamp `[Build] asi=6a482a88… commit=4a11368 tree=clean` on both copies.
**Rig:** two copies, one machine, `two_copy.ps1` with `EntityHost` per copy (`b006387`).
**Common flags:** `SailTestCycle=1`, `IntentLane=1`; host cycles 0→1→2→0 every 3 s through the engine
setter.

---

## Arm 1 — PASS (`SailSync=1`, `SailApplyRaw=0`)

```
host  pid 31440 : tx=33  testcycle=31  applied=0   exc=0
guest pid 26472 : tx=0   testcycle=0   applied=23  exc=0
guest final     : ok=22 stale=0 unresolved=0 bad=0 exc=0 tx=0 local=22
guest lines     : [Sail] rx ship=37A0F990 wire=0000004026549356 1->2 seq=30 via=local ok=1
                  [Sail] rx ship=37A0F990 wire=0000004026549356 2->0 seq=31 via=local ok=1
```
The guest resolved the ship by the **local fallback** (no wireId match on this base), applied 22 states
through `sub_1220960`, **zero exceptions**. `unresolved=0`.

## Arm 2 — control A (`SailSync=0`), the change must not come from shared state

```
host  pid 21900 : tx=0  testcycle=28   (the host's sails still changed 28 times)
guest pid 32372 : ANY sail line = 0    (no rx, no apply, nothing)
```
`tx=0` on the publisher and **zero** `[Sail]` lines on the guest. So Arm 1's applies came from the lane,
not from any shared state on the same machine.

## Arm 3 — control B (`SailSync=1`, `SailApplyRaw=1`), the setter must be what moves the sails

```
host  pid 29048 : tx=29  applied=0
guest pid 9608  : raw=19  applied=0  exc=0
guest final     : ok=18 stale=0 unresolved=0 bad=0 exc=0 tx=0 local=18
guest lines     : [Sail] rx RAW ship=3C9DDB10 1->2 via=local (setter skipped - control B)
```
The value is delivered and written to `+0xCC`, with the setter skipped — 18 times, no exception.

## What is proven, and what is not

**Proven (logged):** the lane delivers; exactly one publisher; the host's sail setter is callable
(`testcycle ok=1`); the guest applies via the identity fallback; the flag-off control produces nothing;
the raw arm reaches the field without the setter.

**Not proven — needs eyes, not logs:** that Arm 1's sails are *visibly* deployed and Arm 3's are furled.
The numeric arms cannot distinguish "the visual followed" from "the field changed", which is precisely
why control B exists. **The visual comparison is the outstanding item and requires the owner at the
screen.**

**Also unproven:** whether `sub_1220960` is safe on a *duplicate* `GcLShip`. Neither arm exercised that;
the guest applied to its own ship, which is a real object.

## Note for the two-machine path

`via=local` is correct only while both players share one hull (`ShipCoop` model). On two machines, or
with two independent ships, the fallback would apply the host's state to the wrong ship; that is when
`c82a696` (ship mirror) must be ported. `docs/design/sail-memento.md` §7.1 records this.

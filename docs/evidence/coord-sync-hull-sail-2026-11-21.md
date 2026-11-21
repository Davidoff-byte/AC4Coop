# Coordinator sync — hull + sail proven live (2026-11-21)

**Build:** `feat/shipmirror-port` `e6a0a5c`, ASI 788,992 B sha256 `9ff85b93…75dc16e`,
stamp `commit=e6a0a5c tree=clean`. Host pid 1332, guest pid 23744, `[Coop] CoordSync=1`.

This is the first slice of the generic coordinator sync: one mechanism, keyed by the engine's own
coordinator chain, replacing per-class lanes.

---

## 1. What was proven

**Sail** (`field 2`, `GcLShip+0xCC`), earlier run:

```
[CoordS] tx vt=026A34E8 key=0 f=2 v=1.00
[CoordS] rx vt=026A34E8 key=0 f=2 2.00->0.00 ok=1
[CoordS] rx stats ok=8 stale=0 unres=0 bad=0 exc=0
```

**Hull** (`field 1`, `GcLShip+0xD0`), this run, after the apply path was corrected:

```
[CoordS] rx vt=026A34E8 key=0 f=1 9625.00->9500.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 9500.00->9375.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 9375.00->9250.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 9250.00->9125.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 9125.00->9000.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 9000.00->8875.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 8875.00->8750.00 ok=1
[CoordS] rx vt=026A34E8 key=0 f=1 8750.00->8625.00 ok=1
[CoordS] hull apply ship=3B2B4C00 gcl=3BCBABB0 8625.0->8625.0 cap=8625.0 ok=1
[CoordS] rx stats ok=28 stale=0 unres=3 bad=0 exc=0 hexc=0
```

The guest's `before` value is the previous applied value — **no snap back to 12000**. That is the
oracle from `ship-hull-live-write-2026-11-21.md` met.

## 2. Why the first hull attempt failed, and the fix

The raw write to `+0xD0` was correct in value and undone within a frame. Static verdict (C):

```
sub_1211EE0 (regen):  +0xD0 = max * rate * dt + +0xD0     clamped to +0xD4
at high hull          +0xD4 == -1.0   -> "no cap, regen to max"
```
So the write armed regeneration. The fix:

1. call the engine setter `0x1225A10(GcLShip*, float)` (`__thiscall`, vtable `+0x12C`), which sets the
   pool, `HealthState`, `+0xD4`, the bar fraction at `*(ship+0x570)+0x32C`, damage info and the zero-HP
   event;
2. **then** write `+0xD4 = value`, because the setter rewrites `+0xD4` via `sub_1220B60`.

`+0xD4` is the lever the owner asked about: the regeneration cap. Never leave it at `-1.0`.

This also explains the old contradiction: a raw write held on a `DuplicateShip` ghost because the clone
has no regen driving it (`sub_12129A0` copies the field), while a live ship runs `sub_1211EE0`.

## 3. The mechanism, for the next classes

```
entity +0x60[ ] GameplayCoordinatorComponent (0x027AFEF0, size 0x80)
  +0x60 Coordinator -> AbstractGameplayCoordinator (0x0276FE08)
    +0x64 GameplayLogic -> GcL*                                   (class = its vtable)
```
`CoordCollect` enumerates these with a stable `objId`; `CoordSchemaFor(vt)` decides identity, extract and
apply per class. Live census: 79–83 coordinators, 12 classes, `GcLShip` n=9.

Identity is per class because object ids are **local only** — `object-id-writability-2026-11-21.md` proves
the id is a map key (`0xA1F160`, 58 xrefs; no rekey API), so ids cannot be rewritten to match. `GcLShip`
uses `0` (the local ship) or a mirror `hostId`.

## 4. Field kinds — the rule the next classes follow

| kind | example | rule |
|---|---|---|
| write-through | sail `+0xCC` | the field *is* the state; set it, or call its setter |
| call-through | hull `+0xD0` | the field is a projection owned by a service; call the owner's setter/damage path |
| composite | fort towers | N sub-objects; apply per tower through the engine's damage call |

A schema entry that guesses the kind will appear to work and be reverted a frame later — exactly what
happened to hull on the first attempt.

## 5. Open

- **`IC_SHIP` is now redundant** with `IC_COORD` for sail; retire it so one mechanism remains.
- Next classes: `GcLHealth` (2 live), `GcLBoardingBattle` (1), `GcLCollectable` (8) — each needs its own
  extract/apply and its kind identified before any code.
- **Forts**: `GcLNavalFort` has no live instance at the tested positions; the F1 harness
  (`[Coop] FortDamageTest`) is armed and waiting, and will report `CASCADE ok=…` the moment one streams in.

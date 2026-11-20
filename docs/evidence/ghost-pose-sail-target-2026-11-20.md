# Ghost pose and sail target — two fixes from the owner's play test (2026-11-20)

Owner report after a live co-op session:

> "ship doesn't face the right direction, rotation is disabled/doesnt work, and sails are stowed even
> when full speed"

Both symptoms pointed at the **ghost ship** (the clone of the peer's ship), and both had a concrete
cause. Neither fix is live-verified yet.

---

## 1. Sails were applied to the wrong ship — our design error

Session logs showed the guest applying the host's sail states to its **own** ship:

```
[Sail] rx ship=391AEEF0 wire=0000004026561312 2->3 via=local ok=1
[Sail] rx stats ok=6 … local=6 …
```

`SailResolveShip` was `wire → local`, with the ghost explicitly refused. That refusal was inherited from
the concern that `sub_1220960` faults on a duplicated `GcLShip`. But in this build **the ghost *is* the
peer's ship** — the local ship is the player's own Jackdaw. So the memento changed the wrong hull, and
the ship the owner was looking at (the ghost) never moved its sails.

Fixed in `83db8db`: route is now `wire → ghost → local`, `g_ghostShip` is a valid target, mirror proxies
are still refused, the setter stays SEH-guarded, and `exc=` is now the measurement of whether the setter
faults on a clone. New counters: `ghost=` and `refused=` on the 5 s line, `via=wire|ghost|local` on each
apply.

## 2. Every rotation that crossed the wire was mirrored — root cause

`HookMath::ToMatrix` builds the **row-vector** (transposed) form of the quaternion rotation.
`HookMath::FromMatrix` used the **column-vector** antisymmetric signs, so it returned the **conjugate** of
whatever `ToMatrix` produced.

Round trip on a +45° yaw:

```
q       = [0.0,  0.3827, 0.0, 0.9239]
current = [0.0, -0.3827, 0.0, 0.9239]   <- conjugate, sign of y flipped
fixed   = [0.0,  0.3827, 0.0, 0.9239]
```

Verified over 4000 random unit quaternions, all four extraction branches exercised
(`trace>0 / M11 / M22 / M33` = 1569 / 786 / 839 / 806):

```
worst round-trip error (fixed): 0.000003 deg
worst round-trip error (old trace>0 path): 179.98 deg   <- 2x the rotation angle, i.e. mirrored
```

Fixed in `dc7d1f1` in all four branches. **This is a root fix, not a ship fix**: the three `FromMatrix`
call sites are all "read a matrix, put the quaternion on the wire" — `fillShipState` (ships, kind 1),
the guard capture (kind 2), and `BaseEntity::GetRotation` (used by the puppet path). So ships, guards and
the player puppet were all mirrored by the same defect. A consumer audit is running separately to confirm
nothing was compensating for the old behaviour.

## 3. Measurement added, so the next run decides

`[GhostPose]` at 1 Hz, inside the ghost drive block, after `SetGlobalMatrixInternal`:

```
[GhostPose] target=%.1f actual=%.1f d=%.1f pos=(…) tgt=(…)
```

`d` is the signed shortest yaw difference between the commanded quaternion and the ghost's own matrix.
A value near 0 means the write takes; a persistent offset means the engine is overwriting the transform;
a mirror-symmetric value would have shown the old conjugate bug.

## 4. Unverified until the next run

- whether the ghost accepts `sub_1220960` without faulting (`ghost=` rising, `exc=` staying 0) — the SEH
  counter is the answer, not an argument;
- whether `[GhostPose] d` is near 0;
- whether the visible heading is now correct (a person has to look);
- the axis convention used for `target`/`actual` is marked UNVERIFIED in-code; `d` is convention-free.

## 5. Build

`feat/shipmirror-port` @ `dc7d1f1`, ASI 763,392 B sha256 `b50f6b91…d53c7d`, gate exit 0. Deployed, flags
`SailSync=1`, `ShipMirror=0`, probes 0. **Not launched** — the owner is on another game.

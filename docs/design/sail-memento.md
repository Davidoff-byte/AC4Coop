# Sail state as a memento (Storm slice S4-tiny)

Status: **design, no code.** Base `b95b7a5` (`feat/launcher`, the deployed build), branch
`feat/sail-memento`. Follows `docs/design/storm-replication.md` §3.2 — sails are a **memento**, not a
per-frame field write, and the apply goes through a native setter so the visual follows.

---

## 1. What this slice is

One replicated value: the ship's `SailState`. Host is the master of the ship object; it publishes the
value when it changes. The guest applies it only if it is newer than what it last applied, and applies
it the way the engine does — not by poking `GcLShip+0xCC` and hoping the sails follow.

Reflection (verified 2026-11-16, `ops/evidence/feature-classes-2026-11-16.md` in the ops tree):

```
GcLShip+0xCC   SailState   Enum: 0 Up, 1 Half, 2 Full, 3 AnimusTravel, 4 Count
WatercraftSail+0x74 HalfSailState, +0x78 FullSailState   (both Enum SailState)
GcLShip+0x154  RegisteredWatercraftSail  SmallArray<ObjectPtr>
```

`GcLShip+0xCC` was read live on the Jackdaw (`sail=0 (Up)`) on 2026-11-16, so the field and the object
path are measured, not assumed. **Whether `+0xCC` is upstream of the visible sails is the open question
the static job `sail-sync-static-2026-11-16` answers.** If it is not, the payload must land on the sail
objects instead — the slice is designed so that only the *apply* function changes.

## 2. Wire

No change to `PlayerState` (576 B) or `NET_PROTO_VER` (16). The intention lane already carries what a
memento needs: `{ u32 msgId, u32 slot, u64 wireId, u8 channel, u8 flags, u8 len, u32 chanSeq,
u32 hostMs, u8 payload[len] }`, with `IF_BASE` for a full state and per-(key, channel) delivery order.

| item | value |
|---|---|
| channel | **`IC_SHIP = 11`** (5–10 are taken; ids 1–15 are legal on the wire by design) |
| payload | `u8 sailState` (+ `u8 reserved` for alignment) — 2 bytes, far under `INTENT_PAYLOAD_MAX` 32 |
| identity | the ship's `wireId` (the same id the entity lane uses); `slot` is the sender's |
| flags | `IF_BASE` on the first message per key and on replay to a rejoined peer |
| freshness | the lane's `chanSeq` for ordering, `hostMs` for the cross-channel timestamp; apply only if newer than `lastApplied[wireId]` |
| who sends | **host only** (phase-1 roles: the host masters NPC/ship objects). A guest-originated sail message is ignored and counted |

## 3. Apply

1. Resolve the local ship by `wireId`. Unresolved → drop the message and count it (a memento for an
   object we do not have is not an error, it is a race with the entity lane).
2. If `chanSeq <= lastApplied[wireId]`, drop as stale. Mementos are idempotent by construction.
3. Apply through **the engine's own sail setter** — the function the `Action*Sail*` path or the input
   handler calls. `// UNVERIFIED:` which VA that is; it is the deliverable of
   `sail-sync-static-2026-11-16`. A raw write to `+0xCC` is used **only** if that job proves no setter
   exists, and then it is recorded as a raw-field route with its own risk note.
4. Log one line per change:
   `[Sail] ship=%p wire=%llu %s -> %s src=host seq=%u ok=%d`

## 4. Oracle (written before the code)

**PASS:** with `[Coop] SailSync=1`, the host toggles Up→Half→Full→Up. For each change the guest logs
`sail=<new>` within 1 s **and** the guest's on-screen sails are visibly open at Full — three consecutive
toggles, no crash, no dropped apply.

**CONTROL:** identical run with `SailSync=0`. The guest's `[ShpF] sail=` must stay at the value it had
before the host changed (it must **not** follow), proving the change came from the lane and not from
shared state on one machine.

**Negative control for the apply path:** with the flag on but the setter deliberately not called
(compile-time `#if 0`), the guest must log `ok=0` and the sails must not move — otherwise the visual is
following something other than our apply.

n ≥ 3 toggles per arm. One machine, two copies (`tools/two_copy.ps1`), same build both sides.

## 5. Failure modes, and what aborts the slice

| failure | detection | action |
|---|---|---|
| setter faults | SEH catch, `[Sail] EXCEPTION` | stop, do not ship; the VA is wrong |
| memento applied to the wrong ship | `[Sail] ship=` pointer differs from `[ShpF] ent=` | stop; identity is wrong |
| visual does not follow `+0xCC` | sails static while `sail=` changes | re-target the apply to `WatercraftSail.HalfSailState/FullSailState`; the wire does not change |
| guest overrides a local change | host-only sender, so a guest value can only come from the lane | if seen, the role model is wrong — escalate before patching |

## 6. Non-goals

Sail *rate*, billow, boost sails, per-sail animation parameters, authority migration of the ship.
Those are later slices; this one carries one enum.

## 7. Dependencies

- `sail-sync-static-2026-11-16` (ops tree): is `+0xCC` upstream of the visual, and what is the setter VA.
- `[Coop] IntentLane` must be on; the lane is default 0. In the run's ini it must be 1 on both copies or
  the memento has no transport (and the failure looks like "sails don't sync").
- The ship must be resolvable by `wireId` on the guest. If the ship's `wireId` is minted per machine
  (the `F000xxxx` concern in storm §3.1), identity falls back to `slot`+class and must be stated.

### 7.1 Identity route: `wireId`, then the local ship

- The consumer resolves the ship **by `wireId` first** and, if that returns no entity, by
  **`GetLocalShip()`**. The log records which route applied: `via=wire` or `via=local` on both the
  apply line and the RAW line, and a `local=` count on the 5 s stats line.
- This is correct **only while both players share one ship** (`ShipCoop`: each PC keeps its own
  Jackdaw, the follower keyframe-follows). The two Jackdaws are the same gameplay ship, so identity
  does not require a `wireId` match.
- **With two independent hulls the fallback would apply the host's state to the wrong hull.** That is
  the point at which `c82a696` (the ship mirror) must be ported and the local fallback retired.
- Measured basis: run 2 (2026-11-20, host pid 27920 / guest pid 21908, ASI `75cbf70c`) — the guest
  received 22 `IC_SHIP` messages and resolved **zero**: `rx ship=0`, `unresolved=22`, `bad=0`,
  `exc=0`. `FiResolve(wireId)` has no entity because the ship mirror (`c82a696`) is absent from this
  base (`git merge-base --is-ancestor` against `feat/launcher` = NO).

## 8. Build order

1. Add `IC_SHIP`, the 2-byte payload struct, and a `static_assert` on its size.
2. Host publisher: called when `GcLShip+0xCC` changes (edge, not per-frame).
3. Guest consumer: resolve, freshness, apply via setter, log.
4. Flag `[Coop] SailSync` default **0**; a counter for dropped/stale/unresolved.
5. Gate: `py tools/spine_gate.py` exit 0. PR against `feat/launcher`'s line with the oracle above.

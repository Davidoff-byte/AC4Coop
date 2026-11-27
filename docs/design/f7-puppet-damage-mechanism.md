# Where a guard's hit on the puppet actually stops (static, 2026-11-16)

Status: **design note + probe spec**. No code. Static only, one IDB (`D:\ac4ida\enemy`, AC4BFSP.exe).
Extends `f7-puppet-damage-2026-11-15.md`; read with `guard-fight-sync.md`, whose decision (intention replication,
R6) this does not replace.

## Why this note exists

The 2026-11-15 receipt ends at "the chain stops at the receiver" with the per-hit count on Edward never completed.
Before spending another live run, this answers statically *what a receiver must be* for the chain to run, so the run
tests a named hypothesis instead of fishing.

## What is now proven [DECOMP]

- **The handler is table-registered, not called.** `sub_11488D0` (`0x011488D0`) has **zero cross-references**,
  code or data, in the `enemy` IDB. It is reached only through a dispatch table.
- **The table is a static array of `(handler, eventId-slot)` pairs at `0x0028CE2A0`.** Read from bytes, because this
  IDB populates no data xrefs at all (the same is true of the table itself and of `0x0028CF260`, so xref presence is
  not evidence here - RULES-RE rule 1 and rule 5 apply to this whole measurement):

  | pair | handler | event-id slot |
  |---|---|---|
  | 0 | `0x011488D0` | `dword_4DE7FC0` |
  | 1 | `0x011489D0` | `dword_4DE7FC4` |
  | 2 | `0x01148930` | `dword_4DE7FCC` |
  | 3 | `0x011489B0` | `dword_4DE7FD4` |
  | ... | ... | ... |

  Tables of this family are themselves pointed to by an array at `0x0028CF260` (`0x28CE260`, `0x28CE268`, ...).
- **The event id slots are runtime-filled copies.** `dword_4DE7FC0 = dword_4E20518` at `0x0114FD28`
  (`sub_114FAB0`). At rest both read 0, so the id *value* cannot be resolved statically - only the pairing.
- **The victim rides the event at `+0x0C`.** Both the thin handler (`sub_11488D0`: `sub_1C08EE0(*(this+3), a2)`) and
  the damage-math sibling (`sub_1148C10`: `v1 = *(a1+12)`) read the receiver from there.
- **Application goes through the victim's own health vtable.** `sub_1C08EE0` calls `victim->vt[+0x40]`, then clears
  bit 3 of `victim+0xD4` when the new value exceeds the old. On a player that slot is `sub_115ACC0`, which reads
  `CSrvNPCHealth_GetLife`, `victim+208`, writes `victim+92`, and toggles `victim+0xD4` bits 0/1/2 by thresholds.

## Trace log (static, in order)

- `[DECOMP]` `0x0064A080` — the slot the post calls. Not a dispatcher: `if (a1) return sub_5220E0(a1); else 0`.
  A null guard and a forward. Dead end.
- `[DECOMP]` `0x005220E0` — `v1 = sub_5210A0(this); return v1 ? v1 + 12 : 0`. Resolves the receiver, then hands the
  handlers `resolved+0xC` (the victim field). **Returns 0 for a receiver that cannot resolve.**
- `[DECOMP]` `0x005210A0` — the resolver. Caches a type byte from `receiver+0xF0`, and on change finds the
  component whose type matches in the receiver's array (`+0x60` data, `u16 +0x66` count, index = `(ptr-base)>>2`).
  No match -> index `0xFF` -> **returns 0**. So consume is skipped silently, not failed loudly.
- `[DECOMP]` `0x006F5A20` — `return off_279BF68`, the component-type descriptor it looks up.
- `[BINARY]` `off_279BF68 -> 0x279DDA0`; that descriptor's ctor/name slot `+0x14` points at `0x01E65090`, which is
  the string **`EventListenerComponent`**; descriptor carries size `0x1C`.
- `[DECOMP]` `0x006F7180` — the installer for that component (`sub_A38120(off_279BF68, a2)` then init). Exactly
  **one** cross-reference: a *data* ref at `0x1E6593C`, i.e. the class method table. Nothing calls it directly, so
  a body only has one if something asked for that component by type.
- `[BINARY]` the same descriptor is referenced from exactly three places, all in code (`0x58C9B5`, `0x6F5A21`,
  `0x6F718D`); the last two are the accessor/installer pair. There is no fourth add-path.

**Conclusion [DECOMP]:** the dispatch is gated on a per-entity `EventListenerComponent`. A receiver without it posts
fine and drops silently. This is M1, and it is now a static result rather than a hypothesis.

**Correction to the earlier receipt:** it recorded that "the puppet's own health object was not located
(`entity+0x70` is not the component array)". The component array is `+0x60`/`+0x66` (this trace, and `STATE.json`
`entity_components_offset`). The earlier search was in the wrong structure.

## Mechanisms, after the trace
- **M1 - PROVEN statically; one runtime fact left.** Dispatch requires the EventListenerComponent (trace above), so
  a receiver without it has its posts dropped by the design of that path. What remains is only "does this body have
  the component" - a scan, not a hook.
- **M2 - still live, but downstream only.** The `+0xD4` early-outs matter once dispatch happens, so keep them as the
  second question.

## Probe (read-only, one run, no new code)

1. Snapshot `puppet+0x60` / `u16 +0x66`, scan for a component whose type == `off_279BF68`
   (`EventListenerComponent`), and log `puppet+0xF0` and `puppet+0x5C`.
2. Same scan on Edward, as the control. If Edward shows no such component the scanner is wrong and nothing is
   concluded.
3. `[Coop] EventProbe=1` (already built: `EXP-f6e67e4-eventprobe`) keeps the `[Evt] recv=` posts as the outer control
   that hits reach the puppet at all.

PASS/FAIL, fixed before the run: **FAIL = the puppet scans clean of EventListenerComponent while Edward has one**
(M1 confirmed live; fix = attach the component to the replica). **PASS = the puppet has one**, retiring M1 and moving
to M2's `+0xD4` bits.

## Implementation (2026-11-16, built - NOT run)

- `src/puppet_listener.{h,cpp}`, flags `[Coop] PuppetListener` (default 0) and `[Coop] PuppetListenerAttach`
  (default 0, needs the first). Wired in `dllmain.cpp` next to the other probes, added to `AC4Coop.vcxproj`.
- Once a second it asks the engine's own predicate (`0x005220E0`) for the replica's listener and logs it with the
  replica's life `+0x5C` and flags `+0xD4`. With Attach=1 and a missing listener it calls `0x006F7180(pup, 0, 0)`
  **once**, then re-asks and logs the result - the re-check is what decides whether the inferred convention is right.
- Build: MSBuild Release x86 v143, exit 0. ASI 755,200 B, sha256 `7AD22290...CB32F0`, from `027f4bd` + this diff
  (tree dirty). `spine_gate.py` VERDICT: OK, `verify_signatures.py` ORACLE: PASS.
- **UNVERIFIED until the first run:** that `this` is the entity and `a3 == 0` is the engine's default for the add
  path; that a listener attached post-spawn actually queues the event. Nothing here is two-machine verified.

## Bearing on the fix

Per `guard-fight-sync.md` the authoritative outcome belongs to the guest (intention lane, R6), so host-side
application does **not** decide who wins. It still matters for two things: the puppet's on-host hurt/death
presentation, and the fact that native replicas (R3-R5) present a body the host AI can connect with. Until R6,
applying the hit on the host is cosmetic only and must be labelled as such.

## UNVERIFIED

- That `dword_4DE7FC0` is the `FightDamageEvent` id and not a sibling damage id. Test: resolve
  `dword_4E20518` at runtime through `sub_9F17B0`'s token and compare against the string-hash 0x13F3BE59.
- That the puppet lacks the EventListenerComponent. The gate is proven; the puppet's own component set is not read yet.
- The puppet's health object was never located (f7-puppet-damage, "Not established"); `entity+0x70` is not a
  component array for an Entity. If the puppet has no health object, `vt[+0x40]` is a read of garbage and M2 reduces
  to "no health object exists" - which is the first thing the probe's `victim+0x5C` snapshot distinguishes.

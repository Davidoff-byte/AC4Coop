# PLAN — co-op for AC4 Black Flag, written from scratch

Supersedes every earlier plan. Nothing here is inherited by default; where an earlier conclusion is
reused it is because we measured it, and the measurement is named.

---

## 1. What the game gives us, and what it does not

AnvilNext, 2013, PC 1.05, single-player by design. There is no netcode to hook, no server, no
replication layer. Every client spawns its own world: its own NPCs, its own ships, its own mission
state. Two clients are **two separate simulations**, not two views of one.

That single fact decides the architecture. Any plan that starts with "sync the world" is fighting
the engine; the engine will win.

## 2. Architecture decision

> **Replicate outcomes and identities, not worlds. One authority per subsystem.**

Two candidate architectures, and why we take the second:

| | A. Mirror | B. Outcome-sync |
|---|---|---|
| idea | host simulates, guest renders host state | each client simulates; they agree on *events* |
| fidelity | pixel-close NPCs and ships | guards may stand in different places |
| difficulty | must solve NPC identity, spawning, perception, and suppression of the guest's own simulation | must solve event delivery and semantic identity |
| risk | one unsolved piece (perception) kills the whole feature | each subsystem ships independently |
| verdict | an upgrade path, not the plan | **the plan** |

Under B, what must agree is: **who died, who took damage, what mission state changed, where the
other player is, and what they are doing.** What may differ: the exact positions of unengaged
civilians, ambient crowd, distant ships.

B is chosen because it is the only version where every phase can be tested in isolation, and because
our own evidence says the mirror approach's blocker — the guest's body having no perception
registration — is a research question, not a schedule item.

## 3. Invariants

1. **One authority per subsystem.** Player A owns A's body; player B owns B's. Host owns mission
   state. Nobody mirror-writes another's authority.
2. **Events are edges, not absolutes.** Never send "hp=30" — send "damage 6, max 36" or "died". An
   absolute value from a world with different stats is meaningless (this cost us a day).
3. **Every event has an id and is acknowledged.** UDP loses; the layer above cannot.
4. **Identity is semantic.** Never a pointer, an `objId` or a manager index from the other machine.
   An entity is identified by what it *is* (archetype) and where it was *spawned*, not by address.
5. **Anything not observable in both worlds is not synced.** No guessing at bodies that do not exist
   on the peer.
6. **Every gate has an oracle that does not involve a human opinion.**

## 4. Phases and gates

Each phase ends in a machine-readable pass/fail. A phase does not start until the previous gate is
green on a two-machine run.

### P0 — Link and ghost *(built; needs re-verification)*

Player bodies mirrored as **render-only transforms**: no physics, no collision, interpolated, ~20 Hz
snapshots over UDP.

**Oracle** `[Net] stats rx>0` both ways for 10 min; `[Smooth] maxD` < 0.35 m; no crash marker.
**Status** works; last verified before five unshipped commits, so re-run it first.

### P1 — Same-world anchor

Both players must be provably in the same world and place before any event sync means anything.

**Oracle** a `[Scene]` line carrying `localWorldId`, `peerWorldId`, `distance`. Gate: same world id,
peer within 2 km, for 60 consecutive samples.
**Why first** every later symptom (deaths not landing, guards "missing") is indistinguishable from a
desync if this is not proven.

### P2 — Reliable event layer

A thin layer over UDP: monotonically numbered events, ack + retry, dedupe by id, bounded queue.
Kinds: `damage`, `death`, `mission`, `pickup`.

**Oracle** a scripted run that emits 200 synthetic events with 30 % artificial loss: 200 delivered
exactly once, median latency < 300 ms, zero duplicates.
**Why** the current one-shot death packet was silently overwritten by the next snapshot; that class
of bug must be structurally impossible, not patched.

### P3 — Combat parity (the minimum viable co-op)

Damage and deaths flow over P2. A guard killed on either machine dies on both.
The victim is resolved by semantic identity (P4), with a bounded position fallback.

**Oracle** 20 kills alternating killer, in view of both: 20/20 on both screens, 0 duplicates,
corpse settles (no revival), no crash.
**Status** host→guest proven in the old design; guest→host built but never run.

### P4 — Semantic identity

Give every synced entity an identity that survives two independent simulations:

```
id = hash(archetype, spawnCell, spawnIndexWithinCell)
```

Host publishes `id → state`; guest resolves `id → its own entity` by the same recipe, and **drops**
what it cannot resolve, with a counted reason. No proximity guessing as the primary mechanism.

**Oracle** `[Id] bind sent/bound/dropped` line: ≥ 95 % bound of entities the host marks relevant,
every drop carries a reason code, and the same guard never binds to two ids.
**Why** pointer/manager identity is why 40 % of NPCs were "not there".

### P5 — Enemy scope

Only what can fight is synced: `kind == 3` with a fighter role, plus naval NPCs. Civilians are local
by decision.

**Oracle** `[Sync] scope candidates/inRadius/emitted` + `kindrole` census within 5 % of a
hand-counted street scene; zero civilians emitted.

### P6 — Stealth: measure, then decide

Does the host's AI perceive the other player's body? This is a research question, and the answer
decides whether co-op stealth exists.

**Experiment** (3 min, one machine + synthetic peer): place the ghost 5 m in front of a guard, freeze
it, log the guard's `HumanGroundStage` and `AlertLevelTag`; repeat at 30 m. Pass = the guard's stage
changes with the ghost's presence and not with a same-distance control.

**If red**, the choice is explicit and cheap: ship combat-only co-op, or invest in one of
(a) a perception-capable proxy body, (b) registering the ghost with the guard's awareness, (c) host
simulating guards for the guest. Do not start any of these before the measurement.

### P7 — Naval

Host-authoritative ship: transform, hull, sails, crew count. Guest rides as a passenger.
**Oracle** sail 200 m, one broadside, one boarding: hull HP identical on both, guest's deck position
stable, no drowning, no flicker.

### P8 — Progression

Objective state as a grow-only set per mission, host-authoritative, reconciled on reconnect.
**Oracle** start a mission on the host: guest's objective list equals the host's within 1 s; complete
a step: both advance; disconnect/reconnect: sets converge with no regression.

## 5. What we stop doing

- **Porting to a second game build.** It was a workaround for Steam's per-app lock. The honest answer
  is two machines; a build port is a separate project with its own gate (byte-identity, §6).
- **Mirroring NPC bodies as the primary feature.** It can be added later; it is not required for
  combat, naval or mission co-op.
- **The five-seat hive with message passing.** Keep `tools/bus.py` (reports are files); drop the rest.
  One implementer, one verifier, one run per gate.
- **Any claim without an oracle.** The claims ledger stays; `MATCH` is reserved for byte-identical
  proof with the pinned compiler.

## 6. Toolchain fact that shapes long-term work

Measured from the PE Rich header: **AC4 PC 1.05 = MSVC 2010 SP1, `16.00.40219.01`, x86**, with
VS2008 SP1 objects from bundled libraries. Byte-identical reimplementation is therefore *possible*,
and is the strongest oracle available — but only with that exact compiler. Until it is installed, the
ceiling is `VERIFIED-BINARY`.

## 7. Schedule shape

| phase | work | blocking risk |
|---|---|---|
| P0 | 1 run | none |
| P1 | 1 day | world ids may not be comparable |
| P2 | 3–4 days | none, it is ordinary netcode |
| P3 | 2 days | depends on P2 |
| P4 | 1 week | spawn determinism across clients |
| P5 | 2 days | none |
| P6 | 3 days measurement, then a fork | unknown |
| P7 | 1 week | ship authority |
| P8 | 1 week | mission graph |

Roughly 5–6 weeks of focused work to a co-op that plays the campaign together without stealth
parity, plus a decision at P6 about whether stealth is in scope.

## 8. First three actions

1. Run P0 on the two machines and re-establish the baseline (nothing has been verified since the
   last five commits).
2. Build the P1 `[Scene]` line — it is a few lines of logging and it protects every later diagnosis.
3. Start P2, because reliable events are what everything after it stands on.

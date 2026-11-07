# P6 result — guards do not perceive the other player's body

**Verdict: FAIL. Co-op stealth is not possible with the ghost as it is built today.**

## Method

`[G10]` logs, per emitted guard, its `HumanGroundStage` (`stage`, where 4 = Fight, 0xC = RangeCombat,
0x11 = FleeFight) and its distance to the peer's body (`dPeer`) and to the local player (`dPlayer`).

Two conditions on the same guards, same session, same metric:

| condition | ghost distance to guard | player distance | guard stage |
|---|---|---|---|
| **positive control** — player walks up to guards | 11–35 m | **1.0–3.5 m** | **4 (Fight), aggro=1** |
| **test** — ghost parked on a guard, player 65 m away | **0.1 m** | 64.7 m | **1 (idle), aggro=0** for 50 s |

Second guard in the same group: `dPeer = 2.2 m`, `dPlayer = 66.4 m`, `stage = 1` throughout.

So the metric is sound — it fires for the player at 1–4 m — and it does not fire for the ghost at
**0.1 m** for fifty seconds. A guard standing inside the other player's body does not notice them.

## Why

The puppet is a `BhvAssassin` copy of the host's own player, created through the player's
`RowSelector`, added to the world with `g_blockReg` so it never becomes a player, with collision
forced to `Layer::Disabled`. Nothing anywhere registers it with the AI's awareness: no perception
component, no faction membership, no player-controller grant. The guards' sight test simply never
considers it.

This matches the static finding from `ops/AI-T5-G10.md` — the body has no perception registration —
and now has a live measurement behind it.

## What this costs the plan

`PLAN-COOP-V2.md` put P6 as "measure, then decide". The measurement says stealth is not free.
Three options, cheapest first:

1. **Cut stealth from co-op.** Ship combat, naval, missions. Cost: zero. What is lost: guards react
   only to their own player, so a co-op stealth approach is one player sneaking and the other being
   shot at.
2. **Proxy body.** On the host, place an entity the AI *can* perceive at the peer's transform — a
   guard-faction NPC driven by the network. Reuses the entity factory / clone work already scoped in
   P4's neighbourhood. Cost: days, and unproven — a guard-clone may be perceived as an ally, not a
   target, because hostility is faction-driven.
3. **Register the puppet with the AI's perception.** The clean fix, and the one with no known
   route: the component that would have to be attached has never been located. Cost: unknown,
   potentially unbounded.

**Recommendation: 1 now, 2 as an experiment of its own** with a gate — "does a guard-clone placed
at 1 m produce stage 4 on the host, and can it be made hostile". If that experiment is red, option 2
dies and stealth is out for good.

## Evidence

* `phase1_log_1656.txt` — `[G10]` rows quoted above.
* `docs/evidence/solo-*.json` — the P0/P5 run in the same session.
* Positive control rows: `key=21 dPeer=1.0 dPlayer=1.9 stage=4 aggro=1`,
  `key=32 dPeer=1.8 dPlayer=2.3 stage=4 aggro=1`.
* Test rows: `key=23 dPeer=0.1 dPlayer=64.7 stage=1 aggro=0` × 6 consecutive windows.

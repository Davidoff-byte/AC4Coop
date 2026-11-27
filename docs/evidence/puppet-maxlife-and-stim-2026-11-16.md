# Max-life mirror, and perception events reach the replica (2026-11-16)

Pair run after the resolver fix. Flags `PuppetMaxLife=1`, `PuppetListener=1`, `PlayerDamageProbe=1`, `FightIntents=1`,
`PuppetNavService=1`. Verdict: **mirror works; stimuli are posted to the replica; no guard was in range to fight.**

## The mirror

`[Coop] PuppetMaxLife` (default 0) copies the player's max life onto the replica once and refills it. Measured:

```
before: life=10  max=10
after : life=125 max=125      (player 125/125 - identical)
```

Without it one guard hit (20) killed the replica outright, which would have made every damage test meaningless.

## Perception reaches the replica

31 `[Evt]` stimuli this run, and the receivers include the puppet:

```
[Evt] t=217834537 STIM id=241 src=35DCE8FC player=1 recv=3B97AF20 from=017EDD01
[Evt] t=217834538 STIM id=299 src=35DCE8FC player=1 recv=3B97AF20 from=017EDD01
```

So the event system delivers to the puppet (`recv=` is the puppet, `player=1` is the local player as source). Together
with `listener=has` this closes the "does anything reach the replica" question: yes, both the listener and the posts.

## Staging notes (for whoever repeats this)

- `pilot teleport` **holds** on the guest copy but **snaps back** on the host copy (the host owns collision), so move
  the host with `pilot goto --route` instead. It arrived 1.5 m from the target in 1.4 s.
- At `(54.3,-74.3,2.9)` - the f7 stage point - there is currently **no guard within sight**: `[Sight] ... n=0 ... d=48.4m`,
  although `[Sync] scope` still counts 650 hostiles in 60 m. X taps land on air.
- The `hold-guard-near.tap` the f7 receipt used is **not in the repo**. `tools/wire_tap.py` can record a new one
  (`record host.tap join <code> --second`) - that is the missing piece for a repeatable guard-fight scenario.

## Still open

`[Sync] bind sent=0` in every run, including this one, so nothing is position-bound even with `FightIntents=1` and a
fight lane active (`[FI] stats` all zero). Guard damage therefore remains unmeasured; the local write path is proven
(`lifehit` 10 -> -10) but the engine's own contact still has to be staged and observed.

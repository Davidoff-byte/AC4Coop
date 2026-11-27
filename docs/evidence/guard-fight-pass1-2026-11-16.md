# Guard-fight pass 1: the instrument works, the guards fought the player (2026-11-16)

Owner-driven run, host copy pid 33256, puppet 3B97AF20 at 125/125, flags `PlayerDamageProbe=1`, `EventProbe=1`,
`PuppetMaxLife=1`, `FightIntents=1`, `ConflictKeepPuppet=1`, `FightOrderPuppet=0`. Verdict against the pre-registered
criterion: **FAIL - the replica's life never moved.** Not because the fight was fake: it was real, and it went the
wrong way.

## The instrument is proven

```
[PDP] player: FDEpost=23 (noListener 0, hitPhase 1) ... fightInter=23 (hit 1) ... csDamage=23 | total FDE=325 ...
[PDP] player-target: FDEpost=8 (noListener 0, hitPhase 2) ... deBuild=1 | total FDE=162 ...
```

Edward was hit, the hit phase ran, damage events were built and CSDamage ran. So `PlayerDamageProbe`'s counters do
move for a real guard fight - every subsequent zero means something.

## The replica

- `[PDP] puppet`: every stage 0, `total FDE=1` for the whole run.
- `[PL] pup=... life=125 max=125` unchanged across every one-second sample.
- `[FI] stats ... puppetFight=0`: no fight intent ever named the puppet.
- `[Evt]` with `recv=` the puppet: 4 in the whole log, and the last four `[Evt]` lines are other receivers.

So the guards engaged Edward and ignored the replica, exactly as in the 11-15 receipt, and the single FDE that did
reach the puppet still produced no apply.

## Next lever

`[Coop] FightOrderPuppet` - the order layer that puts guards onto the puppet - was **0**. It is read live every 2 s
(`guard_poi.cpp:206`), so it can be flipped mid-session with no rebuild and no restart. Re-run the same pass with it
at 1 before drawing any conclusion about the damage lane.

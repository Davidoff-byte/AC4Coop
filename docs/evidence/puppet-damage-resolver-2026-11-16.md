# Damage reaches the replica: the blocker was a vtable whitelist (2026-11-16)

Pair run, build `027f4bd` + resolver fix, asi `684B983A...` chain, host A pid 21916 / guest B 21248 then a repeat pair.
Flags `[PL] PuppetListener=1`, `[Coop] PlayerDamageProbe=1`. Verdict: **the replica's life can be written; the earlier
"no damage" was a resolver whitelist, not the engine.** Solo-tested on one PC.

## The two-line defect

Both resolvers whitelisted only `CSrvNPCHealth` (vt `0x02712F60`). A mirrored puppet is a player-type body
(`+0x9C & 7 == 1`) and carries `CSrvPlayerHealth` (vt `0x0269BBF8`), so neither resolver could ever find its service:

- `FindHealthInAI` (phase1.cpp:~10584) - NPC-only vtable test.
- `HostHpSvcValid` (phase1.cpp:~4478) - the D-016 gate in front of every health read, same NPC-only test.

Both now accept `0x0269BBF8` as well. Measured effect of that alone, no other change:

```
[Pilot] lifehit <puppet> 20      ->  status 0 | life before 10 after -10
[PL] pup=2C7F6A50 ... svc=3ADFFFE0 life=-10 max=10 || player svc=38DA3FB0 life=125 max=125
```

The write went through `CSrvNPCHealth::SetLife` with the engine's own path and stuck. Before the fix, the identical
command returned `status -1` (service unresolved) *and the player also failed*, which is what proved it was the
resolver rather than the replica.

## Second defect, found on the way: the replica's stats are wrong

`max=10` on the replica, `max=125` on the player, both from the same class of object. So a guard hit of 20 will kill
the puppet in a single blow even though the real Edward survives one. Any damage test that "looks brutal" is this,
not the sync. Max and Life both need to be part of what the replica receives.

## Health object, correctly addressed

`entity+0x5C` is the **world**, `+0xE8 -> AIComponent -> +0x1C -> AI`, `ai+0x70` is a **pointer** to a service array
with a `u16` count at `+0x76` (15 here), and the service is the entry carrying a health vtable. Four earlier
candidate chains are listed as dead in `puppet-health-chain-2026-11-16.md`.

## Next

1. Mirror `MaxHullHealth`-style stat onto the replica (max life) before judging any guard damage.
2. With a fight: does a guard's hit now lower the replica, and does `PlayerDamageProbe` fire its stage counters
   (they were 0 in every run so far because nothing was ever applied)?
3. Kills on the replica still need the relay-to-guest lane (R6); this note only proves the local write works.

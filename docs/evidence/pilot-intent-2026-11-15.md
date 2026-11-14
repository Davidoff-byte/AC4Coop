# [Pilot] world-space intent (part A): live check, 2026-11-15

Build 351378b (tree clean), asi 2e33f3c96d971abeff8d8976f93d5a0eeacb85552c6fd5826c34e48ce48f01ae, run pid 22156, Havana.
Seam: bf-coop ops/evidence/pilot/intention-seam.md seam 1 (IHumanGround vtable 0x026E7DC0 slots 0/1), re-verified in
a second idb before use (bytes `B0 91 CB 01` / `90 16 D5 01`, slot 1 decompiled: writes HumanGroundData+0x98 and
zeroes scope+0x5F8). Load line: `[Pilot] intent hooks installed on IHumanGround vtable 0x026E7DC0 slots 0/1`.
Verdict: **solo-tested**.

| check | command | result |
|---|---|---|
| world direction, 4 compass pushes | `intent 1 0`, `0 1`, `-1 0`, `0 -1` (1.2 s each) | moved at 0°, 85°, 175°, -96° for 0/90/180/-90 asked (0.78-1.21 m; first push from standing); substitutions counted on both slots, about 60 per second |
| in-process goto | `goto 70.9 -70.3 --tol 0.8`, `goto 78 -76 --tol 0.8` | arrived d=0.79 m in 4.1 s and 5.7 s |
| route + in-process goto | `goto 90.5 -52.3 --route` from (76.3,-74.3) | 6 legs, every leg arrived (5.2/3.1/3.8/5.2/2.1/2.6 s), final d=1.20 m at (91.49,-52.95). The same route failed on leg 4 with the camera-relative stick controller (run 26452: Edward sideways at a pier edge) |
| planner retry | teleport to (77.44,-75.55), `route 90.5 -52.3` | first lattice walled in (was NO ROUTE before the retry); second lattice: 4 legs in 7.0 s |

Also verified in this run (fixes from earlier commits):
- navmesh line byte return: `navline 70.9 -70.3` CLEAR, `navline 76 -60` BLOCKED code=1 err=2 (clean values).
- probe re-HOOK after UNHOOK: `HOOK 5` on 0x70E810 reused id 0's MinHook entry (`"reused":0`), entries 10697 =
  exits 10697, game alive.

Not established: the in-frame order of the ground handlers vs our player-frame tick (one frame of latency is assumed,
not measured); what scope+0x5F8 means (zeroed by the original slot 1, which we still call); no negative control with
`[Pilot] Intent=0` (expected: goto status -4). The puppet is not driven through these slots yet (it has no controller
that calls them; intention-seam.md describes calling them per frame).

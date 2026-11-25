# Host spawn authority - design (2026-11-25, not built)

Owner's spec: the guest's own spawning is off; it is on only while connected to a host; the guest spawns from the host's
data and drives what it spawned from the host's data. Evidence: `docs/evidence/spawn-manager-2026-11-25.md`.

## What the evidence supports
- One gate covers the actors: requests at `0x79B8D0`, builds at `0x46D8E0` (every ship, ~94 % of characters).
- Everything else that enters the world (FX, props, crew props, ~90 % of all adds) is local cosmetics: leave it alone.
- The guest can make the engine build a given ship class from our code, but only the request path is survivable
  (Spawn + AddEntity crashed 3/3).

## Changes to the spec (proposed)
1. **Gate by kind, not all spawns.** Blocking every request hung boot before (`phase1.cpp:144`): the player and
   essential entities use the same entry. Decline only requests whose params class is an AI actor (SpawnNPCParams /
   SpawnCharacterParams / SpawnShipParams / SpawnObjectParams when it is a character), keep player, mission and
   cinematic spawns local.
2. **"On only when connected" needs a hand-over, not only a gate.** Before connecting, the guest plays solo and has
   already spawned its own population. At connect: the guest removes its manager actors through the manager's own
   remove (`sub_423010` path / removal queue), then the host sends a snapshot of every live manager actor, then the
   stream of spawns and removes. On disconnect: lift the gate, the guest repopulates itself.
3. **Proximity is the real problem.** The host's engine spawns around the host's player. If the guest sails or walks
   away from the host, a pure host-spawn world is empty around the guest. Options:
   - (a) shared bubble only: sync actors within R of the host; outside it the guest spawns locally (unsynced). Cheapest;
     the two worlds differ when apart.
   - (b) area ownership: each machine spawns around its own player and owns those actors; the other machine builds them
     from the owner's data. Symmetric version of the same lanes (spawn, drive, remove, damage requests). Both players
     always see a populated, synced world. More work: every lane becomes two-way.
   - (c) the host spawns around both players: needs the host's spawner to think the guest's position is a player -
     no lever found for the spawn trigger (prior: UNPROVEN, virtual client methods not located).
   Recommendation: (a) first, built so it extends to (b).
4. **Drive from host data = the guard lane.** Spawned actors get their AI off and are driven by the existing ring /
   interpolation (`OwnUpdateGuards`, `GuardInterpBuffer`, Brain for intents), ships by the same shape. Deaths, hull,
   sinking and boarding go host -> guest as now; guest damage goes to the host as requests.

## Open, in the order they block
1. The request factory's arguments: `ctx` (`sub_FCDB70`: `[a1+0x1E8]->vt[1]()`) and what `sub_79C6C0` stores (source,
   position). Static.
2. A portable key for params: is the builder ObjectID the same on two machines, and can the guest find its local params
   object for it? Live: log builder ids on both copies at the same place; PASS = the same set.
3. Our own request passes our gate (tag it), the manager builds it, and the actor registers and survives 60 s. Live.
4. Placement: write the host's transform after the add (prior: a pre-add write did not hold).
5. Snapshot at connect and the remove lane.

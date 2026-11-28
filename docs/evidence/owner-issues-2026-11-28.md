# Owner issues 2026-11-28 (afternoon): ship damage lever, guest-started combat, tied Jackdaws, guest-only ships

Two copies on one PC. Builds: F681F906 (ShipHurt, run demo5), 80C2D38D (the three fixes, run demo6). All flags default 0.

## ShipHurtCmd - the naval test lever (run demo5, host 26760 / guest 19512)
- H1 PASS: `[ShipHurt] seq=2 key=00000000F000CD89 ... d=736 hull 900.0 -> 45.0 (max 900, thr 90) state 91 -> 95 a540=1`,
  still 95 after 2 s (400 m found no ship; the 2000 m retry did).
- H2 PASS: guest `[ShipSt] key=...F000CD89 host=95 localBefore=91 posted=4DEAD5C ... localAfter=95`, then
  `[ShipSt] PASS ... dtMs=15`. H3 PASS: guest `[ShipSt] hull ... target=45.0 before=900.0 after=45.0`.
- H4 partial: `fail=0 unsanctioned=0` for 30 s. H5 (boarding) NOT REACHED: the session ended (agent process exit) during it.

## The owner's three reports, causes read from session f681f906 (12:47-12:56)
1. **Guest kills in Havana, "npcs will not run to attack".** The guest's 4 kills reached the host (`[OD] tx` ->
   `[OD] host kill ... killed=1`), but they are life writes with no attacker: no stimulus, no alarm. Host
   `[Fight] combatBodies=0` all session; `[FI] apply` 0 (kills/assassinations never enter the fight FSM, so
   PuppetFightStim, which rides FightIntentApply, never fired). Host perception hooks did run early
   (`[Perceive] g1 calls=200 pickPup~50%`).
2. **"Jackdaw speeds tied".** CoordSync's key 0 = "this machine's own ship" on both sides (coord_sync.h: one shared ship),
   so the host's sail / hull / helm were applied to the guest's own Jackdaw (`[CoordS] tx ... key=0 f=2`).
3. **Schooners on the guest only.** The guest's own naval traffic (DisableSpawns=0, needed by ShipSpawnReplay), plus
   replay hulls that lapse to free hulls (ship-spawn-replay.md item 6: no mod despawn).
- Link flapping (`[E] ev=link-down/up`, ~2300 per 10 min) is present in every build since 11-24 (scan of 300+ logs):
  not a regression.

## Fixes (asi 80C2D38D; gates: spine OK, signatures PASS, ini keys PASS)
- `[Coop] PuppetAlertOnHit` (host): each applied guest hit / kill broadcasts `PuppetAlertIds` (default 8,257) from the puppet
  to every NPC within each id's radius (conflict-gate-2026-11-17 s6 run 8: 7 conflicts, 13 guard hits), at most once per
  `PuppetAlertMs`; 257 to a surviving victim.
- `[Coop] SeparateShips`: key-0 (own ship) CoordSync / HelmSync messages are neither published nor applied.
- `[Coop] GuestNoShipTraffic` (guest): declines requests whose owner vtable is the naval owner 0x02708E80 while in world;
  our replays pass (t_replay / SpawnLib's private owner).

## Run demo6 (host 14792 / guest 30436) - pre-registered C1-C3, S1, T1, A
- T1 **PASS**: 20+ `[Census] GuestNoShipTraffic ... declined`; at sea `[Nav] ships` host 9 / guest 10 (the ghost) in 10/10.
- S1 **INCONCLUSIVE**: the host's sail stayed 0 (103 samples) - nothing to tie to; `sepDropped=0` is expected because the host
  no longer publishes key 0 (my pre-registration missed that).
- C1-C3 **NOT RUN**: the save starts at Dry Tortuga; the harness' soldier filter used the role field, unreadable for every
  nearby character (role -1).
- A **PASS**: both alive, no crash context.

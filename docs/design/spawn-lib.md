# SpawnLib — spawn any engine entity on demand, and keep it synced (`src/spawn_lib.cpp`)

Flag `[Coop] SpawnLib` (default 0; hooks installed only when non-zero at load, then re-read every second).
Evidence: `docs/evidence/spawn-lib-2026-11-28.md` (runs, four crash root causes, IDA receipts).

## What it does
1. **Library.** Every engine submit seen by `spawn_census.cpp` (`SpawnKeyNoteReq`) is recorded as
   (spec, owner, ctx, static key, kind ship/NPC, learned ship class). Our own submits are not recorded.
2. **On demand.** `SpawnCmd = ship[:class] | npc[:keyhex]`, fired by a change of `SpawnCmdSeq`, only in the copy whose
   pid is `SpawnCmdPid` (0 = any). Built entity is placed in front of the player (ship 80 m, NPC 4 m) 1.5 s after build.
   `npc` without a key prefers a keyed spec, so the guest can follow it by key.
3. **Guest follow** (`SpawnLibFollow`, default 1). WideBind's key round finds a host guard whose static key has no free
   local body → the guest replays its own spec for that key; while that is queued/in flight/just built (< 4 s) the
   distance rounds skip that guard (`pendingFollow=`), so the key round binds the new body.
4. **Ship supply** for ShipSpawnReplay (`SpawnLibPickShip`): newest usable spec of the wanted class, else any class.

## Rules every submit obeys (each one is a crash we had)
| rule | why (crash) |
|---|---|
| never restore `spec+0x3C` right after the submit | A (guest 17552) and B (host 17860): the owner paired the old request with our ship; it was freed |
| spec must be **armed**: `spec+0x1C` object with a vtable in .rdata and vt[2] in .text | C (host 25716 + guest 24228): `sub_46D6F0 → sub_6B3CE0(spec)` on an un-armed spec, AV 0x6B3CE3 |
| busy spec only if its current request is not in flight (`SpawnLibBusy=1`; 0 idle only, 2 any) | idle specs did not build (4/4 timeouts); in-flight is where B's pairing came from |
| naval submits use the **private owner** (no adoption) + slot borrow restored at build | D (T3, both copies): owner adoption is 1:1 per spec; it freed a spec another record still used, AV 0x1C27B25 |
| a spec dying under one of our queued requests → request re-pointed to a live substitute (detour `sub_501DF0`) | E (T5 guest): encounter freed the spec while our request was queued, AV 0x69D0A5 |
| ship specs only from the naval owner class 0x02708E80 | ship specs of class 0x026BDE50 never built (11/11 in T5+T6) |
| `SpawnCmdSeq` acts on a value read twice in a row (1 s apart), sentinel defaults on every re-read | a key caught mid-write reads as 0: 39 / 19 phantom commands in T5 / T6 |

Private owner: one static zeroed object; its vtables point at a copy of the owner interface 0x02708E80 with the
adoption / entity-removed / dtor slots ([4]–[8], [10], [11], [17]) replaced by counting no-ops; the other slots are the
engine's own and ignore `this`. Owner classes other than 0x02708E80 are passed through unchanged (`[SpawnLib] owner class`
lines list them; their callbacks are unread — NPC spawns still use the engine owner).

## Sync path
- Ships: host ShipSync carries every host AI ship; guest ShipClassBind=2 binds a free local hull of that class, else
  ShipSpawnReplay builds one through the same submit (private owner) and binds it; driven by
  `Entity::SetGlobalMatrixInternal` per snapshot.
- NPCs: host publishes each guard's static key (IC_SPAWN kind 2) or crew key; guest key round binds its own body for the
  key, else SpawnLib follow builds one; guard lanes mirror it.

## Not done / unverified
- Ships under the private owner skip the owner's adoption (vt[4] `sub_196E610` per build); they still get their crew
  spawns (7 per hull). Whether they sail/fight like engine ships: not measured.
- NPC owner class 0x026BDE50: callbacks forward to global functions (read), real owner kept; 28 host NPC spawns + 27
  guest follow spawns in T8 without a crash. No 1:1 adoption study beyond that.
- Crews of spawned ships are deck actors (NPC vt 0x01E4CE90, no BhvGenericNPC/AI/health): never in the guard lane;
  the guest's replay hull spawns the same layout (7/7 offsets, n = 1) and carries them (evidence s12). AI crews on AI
  ships (crew-key path) untested; the host's Jackdaw crew is no longer published (class 7, never bindable).
- Requests that never build stay queued (their slot borrow is given back after 3 s); such a ship can appear later.
- Nothing despawns our private-owner ships before world unload; world-unload behaviour with them alive: untested.
- `ShipClassBindSteal=1` + SpawnLib: one guest AV 0xF1813C right after a steal (T6), cause unknown; tests since run
  with steal off.

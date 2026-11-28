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
| …and under one of our **built** requests too, until the request's own dtor (detour `sub_79C640`) | 0x198F274 (guest 24636): the client's released-list walk read a freed borrowed spec |
| naval submits go into the private owner's container, never the real owner's | 0x1425954 (guest 28276, host 16964): `sub_1978B50` walked freed requests of ours in the real owner's list |
| the borrowed slot goes back at completion (`SpawnLibLateRestore=1`: private owner vt[4]/vt[10]), not at DetSet | not a crash: completion places `spec+0x3C`'s entity at the spawn point; given back early, that was the old live ship (4/4 one-frame jumps of 65–775 m, both copies; 0/4 with the flag; evidence s16) |
| ship specs only from the naval owner class 0x02708E80 | ship specs of class 0x026BDE50 never built (11/11 in T5+T6) |
| `SpawnCmdSeq` acts on a value read twice in a row (1 s apart), sentinel defaults on every re-read | a key caught mid-write reads as 0: 39 / 19 phantom commands in T5 / T6 |

Naval submits are made with the private owner (so the request lands in the private owner's container), then carry
the **real** owner while queued (the naval builder `sub_199D1F0` builds at once only for its own owner interface,
others wait until out of view), and DetSet swaps `request+0x0C` back to the private owner before the first owner
callback (vt[4] adoption, called from `sub_5C58E0` 0x5C59BE).
Private owner: one static zeroed object; its vtables point at a copy of the owner interface 0x02708E80 with the
adoption / entity-removed / dtor slots ([4]–[8], [10], [11], [17]) replaced by counting no-ops; the other slots are the
engine's own and ignore `this`. Owner classes other than 0x02708E80 are passed through unchanged (`[SpawnLib] owner class`
lines list them; their callbacks are unread — NPC spawns still use the engine owner).

## Request lifetime (IDA, 2026-11-28) — vt[19] = true, private container, tracked until the dtor
- `sub_79B8D0` (submit) sets `req+0x24 = 1` (owner-kept), state 1, appends the request to its owner's container
  (`sub_48A390([req+0Ch])`: owner interface + 4 = naval owner object + 0x0C, count u16 at +0x12) and registers it with
  the client found from its ctx key (request vt[3] `sub_79BA80` → client `vt[34]`).
- The naval owner's own spawn routine `sub_1978B50` walks that container (object+0x0C) and calls each request's vt[2].
  Requests of ours left there crashed both copies once they were freed (runs 2026-11-28 guest 28276 / host 16964,
  AV `0x1425954`; host dump: the list held exactly our 3 requests). So the submit uses the **private** owner (its
  container is our static object), and only afterwards is `req+0x0C` set to the real owner for the build-now path
  (`sub_199D1F0`); DetSet swaps it back to the private owner before the first owner callback.
- Completion (`sub_5C58E0` / `sub_5C5FC0`): state 4/5, owner vt[13], vt[4] (adoption), `sub_480F40` (flags 2/3 →
  client vt[15]). The naval client (vtable 0x0270B310) overrides both with `sub_1982900`/`sub_1982940`: after the base
  call, `if (req+0x30 && owner->vt[19]()) sub_5BCBE0(req)` — release to the client's type-3 list `+0x4C`.
- The naval owner's vt[19] is `sub_169EB60` (return 0): it keeps its requests. The private owner's returns 1, so ours go
  back to the client (T17 and run 2026-11-28 04:32 survived fast travels with it; crash H at the first travel was with
  0 and our requests in the real owner's container).
- Released requests are walked every tick by `sub_198F080` (client vt[23] `sub_1991F70`): request vt[2] → spec,
  `sub_4EFD00(spec)` = `spec+0x24` → `[+0x14]` (0x198F274). A borrowed spec freed by its engine owner while our released
  request still pointed at it → AV 0x198F274 (guest 24636, at sea). So **built** requests stay in the request table and
  `SpawnLibSpecDying` re-points them to a live spec of the same kind/class too (never a request with `+0x31` set: the
  request dtor `sub_79C640` frees an owned spec through `sub_A1E800`). Entries leave the table in a detour of
  `sub_79C640` (request dtor body; callers 0x79DA96 deleting dtor, 0x549F56), before its body runs.
- Entity gone (`sub_5C5E40`): owner vt[11]/vt[12]/vt[14], flag 1 → client vt[17] (`sub_19969C0`: removes the request
  from the client's lists +0x6C/+0x74 only); flags 2/3 → vt[15] (`sub_9ED490`, returns 0).

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
- World unload with our ships alive: fast travel with 3 host spawns + guest replays alive, both copies alive after it
  (run 2026-11-28 05:00, asi 85159A69; evidence s15). `sub_198F080` returns one released request to the caller of
  client vt[23] after its distance/visibility tests; that this picks it for despawn is UNVERIFIED. A replay hull that
  goes is replayed again (2 per host ship per 60 s).
- `ShipClassBindSteal=1` + SpawnLib: one guest AV 0xF1813C right after a steal (T6), cause unknown; tests since run
  with steal off.

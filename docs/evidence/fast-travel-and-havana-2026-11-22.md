# Fast travel driven unattended; Havana is entered through its gate (2026-11-22)

Goal: get both copies into Havana among hostile guards without the owner, and capture the engine call behind fast
travel so it can be issued directly. Build `feat/integration-1121` @ `3452a53` (asi `7930f6e2…`), host pid 22196.
Saves backed up before and restored after (`ref/AC4Coop-lobby/backups/saves-437-20261122-135134-session`).

## Tools added (all default off)
- `pilot.py frame`: saves the next presented back buffer (Present hook, installed with `fps 0`) as BMP/PNG; served
  from the poll thread too, so it works in menus. This is how the map was read, with no desktop access.
- `pilot.py teleport --ship`: moves the player's ship (`Phase1_PlayerShipEntity`, same lookup as SpawnGhostShip).
- `[Coop] CallTraceVt`: per-slot stubs over chosen vtables that log caller, `this` and first argument.

## What the boot save is
Edward starts **on land at Dry Tortuga** (a naval fort, "infiltrate fort" side activity), about (-4035, 4087), among
58 friendly pirates. Not at sea; there is no ship under him (ship lookup returns null there).

## Fast travel, unattended (worked)
Map (Back) → zoom out (RS click) → pan (left stick) and zoom (right stick), reading frames each step → hover the ⏻
icon north of Havana (map coords ~(235, 649)): prompt "X fast travel" → X → "Do you want to fast travel to this
location?" → A. About 25 s later Edward stood at the Jackdaw's helm at sea, (-4495, 2490).
- Legend (read from frames): city letters are **gates** (H = Havana, N = Nassau, K = Kingston, …); ⏻ = fast travel;
  the small ⏻ badge marks fast-travel-capable icons. Havana's viewpoints on this save are **not synched**, so they
  offer no travel. Hovering the H gate at world zoom gives no prompt.

## What the tracer saw during the travel
Over FastTravelManager `0x0271A5F8`, MapIconBehaviorFastTravel `0x026F6E18`, WorldTransitionPortal `0x01E49D18`,
WhiteRoomManager `0x01E58B98`, FastTravelEntry `0x02719DA8`, MapIconFastTravel `0x026F9F28` (82 slots):
- FastTravelManager slot 16 `sub_1AC5300` (refresh: walks the entries, then `+0x12 = 0`), this = `0xFFB6517C`,
  from `0x00657C35`; WhiteRoomManager slot 17 `0x609660` from `0x00657C65` (same engine loop over universe
  components); FastTravelEntry slot 14 `0x1A92340` ×3 from `0x007E0097`.
- **The confirm itself did not pass through any traced slot.** The request goes UI → travel code by another route.
  Next capture (not built): log the return-address chain when the player entity moves more than 100 m in one
  SetGlobalMatrixInternal call.
- The manager instance lives above 2 GB (`0xFFB6517C`), so a memory scan capped at `0x7FFE0000` misses it.

## Havana
- Reflection: `FastTravelEntry` = { EntryName, TargetWorld, ReferenceEntity, Portal (WorldTransitionPortal), Profile
  (WhiteRoomTransitionProfile), Locked }; `FastTravelManager` +0x8 entries, +0x12 FastTravelDisabled.
- Havana's bay is at about (-4300, 1700) in this world (map scale ≈ 17 world units per map unit, map y follows world
  y). The old runs' (72, -74) is a different coordinate frame, which is why teleporting there loaded nothing.
- With the ship parked at the H gate's shore: only the crew loaded, and engine ground rays at four points south of the
  gate hit nothing. So the city is not streamed in by proximity; it is entered through the gate. UNVERIFIED which
  action triggers the gate (docking prompt, or the portal), and whether the city is a separate World object.

## Not done
- Entering Havana; a hostile-guard run (needs the city loaded).
- Capturing the fast-travel call (route above).

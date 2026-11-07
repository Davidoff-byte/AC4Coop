# The angle we got wrong: we built a body, the engine wants a Gamer

## What we have been doing

Every attempt at the other player has been to **construct a body**: build a `BhvAssassin` from the
player's `RowSelector`, `World::AddEntity` it, drive its transform, and then try to make things
notice it (`+0x660` "targetable" hack, collision layer, behaviour toggles).

P6 measured the end of that road: a guard standing 0.1 m from the ghost, for 50 s, at `stage=1`
while the same guards fire `stage=4` at the real player 1–4 m away. The body exists, renders, moves —
and is not a participant.

## What the engine actually models

Strings in **`AC4BFSP.exe`** (single-player, not the MP build):

```
Gamer
GamerWorldInstance
GamerCollection
AvatarAnimationParams
AvatarAnimationDuration
MultiplayerMenuComponent
```

and in the PS3 symbols:

```
scimitar::Gamer::RegisterAvatarEntity      @ 0x1dbc45
scimitar::Gamer::UnRegisterAvatarEntity    @ 0x1d2b50
```

So the unit the engine knows how to reason about is not "an entity with a skeleton". It is a
**Gamer**, which owns an **avatar entity**, and gamers are collected per world
(`GamerWorldInstance`, `GamerCollection`). Perception, animation registration and "is this a
player" all hang off that relationship — which is exactly the set of things the puppet lacks.

That reframes the whole problem:

> **Stop manufacturing a body. Register a second Gamer and give it the body.**

## Why this is plausible rather than hopeful

- The machinery is present in the **SP** executable, not only in MP. The SP game already has one
  Gamer (the local player) and a collection to put them in.
- Avatars carry animation registration, which is the second thing we have been fighting by hand
  (`SetTargetAction`, blend factors, time scale, seek-on-switch).
- If perception hangs off gamer-ness, P6 stops being a wall: the ghost becomes a participant
  instead of a decorated prop.

## The experiment that tests it (and cheaply fails)

1. Locate `Gamer::RegisterAvatarEntity` in the PC build. Start from the PS3 twin at `0x1dbc45`,
   match by shape (it takes a Gamer and an entity, and touches the collection) — the same
   structure-matching that already produced 22 confirmed PS3→PC pairs.
2. Read what it requires: which Gamer fields must exist, whether a Gamer can be created outside the
   network path (`GamerCollection::Add`? a constructor?), and whether an avatar needs a pad.
3. Instrument the mod to log the local player's Gamer and its avatar entity — the *positive* case.
   If we can read the player's Gamer, we can see what a second one would need.
4. Only then attempt a second Gamer.

**Fail condition, stated up front:** if a Gamer can only be created by the multiplayer session
layer, the path is closed and the answer is the cut in `P6-RESULT.md`. That is knowable in a day of
reading, not a month of trying.

## What this does not fix

- **Two instances on one PC** — unchanged. That is Steam's per-app lock, and only a second licence,
  a second machine, or a Steam-free build gets around it.
- **NPC identity between independent simulations** — unchanged, still P4.
- **Stealth** — unproven until the gamer experiment lands. If it works, guards perceive a Gamer's
  avatar; if not, cut it.

## Order of work

1. Read the Gamer/avatar path in the PC binary (structure match from the PS3 names).
2. Log the local player's Gamer — cheap, safe, and it proves we can see the object at all.
3. Decide: attempt a second Gamer, or cut stealth and finish combat/naval/missions.

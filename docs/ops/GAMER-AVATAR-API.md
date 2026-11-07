# The Gamer/avatar API, read from the PS3 build

Target: make the other player a **registered avatar of a Gamer**, instead of a body the AI ignores
(`P6-RESULT.md`). This is what the engine's registration path looks like, disassembled from
`scimitar_final.elf` with the addresses from `scimitar_final.map`.

## The functions

| symbol | PS3 address | size |
|---|---|---|
| `Gamer::RegisterAvatarEntity(Handle<World>, Handle<Entity>)` | `0x7504c0` | 0x120 |
| `Gamer::UnRegisterAvatarEntity(Handle<World>, Handle<Entity>)` | `0x74cac8` | 0xe8 |
| `Gamer::GetGamerWorldInstanceFromWorld(Handle<World>)` | `0x714030` | ~8 |
| `GamerCollection::GetGamerFromID(uint64)` | `0x71404c` | 88 |
| `GamerCollection::GetNumGamers()` | `0x7141d8` | 16 |
| `GamerCollection::CalculateLocalGamerID(uint8, bool)` | `0x71403c` | 16 |
| `GamerCollection::GamerCollection()` | `0x714ed0` | 68 |

## What the code says

`GetGamerWorldInstanceFromWorld` is three instructions — it ignores the world handle entirely and
returns a **global**:

```
0x00714030  lis  r9, 0x258
0x00714034  lwz  r3, 0x41a8(r9)      ; r3 = *(0x25841a8)
```

So there is **one** `GamerWorldInstance` for the process, at `0x25841a8` on PS3. Not per-world.
That is good news for us: one pointer to find, not a container to walk.

`GamerCollection::GetGamerFromID` gives the collection layout:

```
lwzu r11, 4(r3)        ; array base at +0x04  (and r3 becomes +0x04)
lhz  r0,  4(r3)        ; count  u16 at +0x08
...  lwz r3, 0(r9)     ; element = pointer to a Gamer
     ld  r0, 0(r3)     ; Gamer's id is a uint64 at +0x00
     cmpd r0, r4       ; compare with the requested id
```

`Gamer::RegisterAvatarEntity` walks a container starting at `this+0x10`:

```
lwzu r10, 0x10(r3)     ; avatar array base at +0x10
lhz  r0,  4(r3)        ; count u16 at +0x14
...
```

So a **Gamer** is: id at `+0x00`, avatar-entity array at `+0x10` (count at `+0x14`).
A **GamerCollection** is: array at `+0x04` (count `+0x08`) of pointers to Gamers.

## What this buys

The registration we need is one call:

```
Gamer::RegisterAvatarEntity(Handle<World>, Handle<Entity>)
```

with the entity being our puppet. If that call makes the engine treat the entity as a player
avatar — perception, animation registration, pad ownership — then P6's answer changes from "guards
cannot see the other player" to "they can".

## What is still missing, stated plainly

**No PC addresses yet.** The PS3 layout is not the PC layout (`JOURNAL-2026-11-04.md` §11), and
offsets must be re-confirmed. The mapping job is four symbols, not a hundred:

1. the `GamerWorldInstance` global (PS3 `0x25841a8`) — a data address, findable by xrefs from
   whatever reads it
2. `Gamer::RegisterAvatarEntity`
3. `GamerCollection::GetGamerFromID` (or `GetNumGamers`, which is a thinner accessor)
4. `GamerCollection::GamerCollection()` — to know how a Gamer is made at all

Until 1 and 3 are mapped we cannot even *see* the local player's Gamer, which is the safe first
instrument (log the local Gamer and its avatar; prove the object is reachable; only then consider a
second one).

## The cheap experiment, in order

1. Map the global. If the PC build keeps a `GamerWorldInstance` singleton, it will be a data
   pointer read by a tiny accessor — the same shape as the PS3 three-instruction function.
2. From it, read the GamerCollection, log `GetNumGamers()` and each Gamer's id + avatar array.
   Expected: exactly one Gamer (the player) and one avatar.
3. Only if step 2 works: try `RegisterAvatarEntity` on the puppet.

**Fail condition:** if the SP build's `GamerCollection` is empty outside a multiplayer session, or
`RegisterAvatarEntity` asserts on a non-gamer entity, the path closes and `P6-RESULT.md`'s cut
stands.

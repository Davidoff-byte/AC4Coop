# Where the replica's health actually lives, chased by differential (2026-11-16)

Three pair runs, build chain `027f4bd` + probe edits, latest asi `EDD7A36B10AD66ED...`. Two-instance rig (host + join,
pilot-booted, ~0.1 m apart). Verdict: **the health object is not where three candidate chains said; four offsets are
now eliminated by measurement.** No fix yet.

## What the listener question settled

`listener=has` on every replica, both directions, every run. M1 is dead. The dispatch gate is not the stop
(`puppet-listener-live-2026-11-16.md`, `two-instance-listener-2026-11-16.md`).

## Eliminated by measurement (do not re-walk)

| candidate | result |
|---|---|
| `entity+0x5C` = int16 life | wrong. It is the **world pointer** - `status` prints the identical value (`world=2C970AB0`), and both entities read the same one. "life" 9052/12592 was its low word. |
| `entity+0x5C` = health object pointer | same value as the world, so also wrong (it is the world). |
| CharacterBehavior `0x5220D0` -> comp `+0x1C` | resolves (non-null) on player *and* replica, but the object behind it reads implausible life for both, so it is not the life carrier either. |
| `[Pilot] lifehit <ent>` | **fails on the local player too** (`status -1`), so it is a tool fault, not a replica fault. `Phase1_TestLifeHit` returns -1 when `HostHpSvcValid()` rejects both `CachedHealthForEntity()` and `FindHealthInAI(GetEntityAI(ent))` (phase1.cpp:7936-7950). |

## What is real, measured

The AI chain works on replicas and is per-side distinct: `e+0xE8 -> AIComponent -> +0x1C -> AI` is non-null on both
copies and on both players. Latest pair:

```
A pup=3A8770B0 ai=3A7E1B70 ai70=FC957F44 ai5c=0008 || A player ai=39054E00 ai70=FC952D00 ai5c=005C
B pup=3A3E1920 ai=3A074AC0 ai70=FC8C51CC ai5c=0060 || B player ai=38A38760 ai70=FC8C2D00 ai5c=005C
```

- `ai+0x70` holds a pointer-like value (heap 0xFCxxxxxx) on every AI, so it is the service-array base DECOMP-AUDIT
  describes ("+0x70 health (20 entries)"), not a scalar.
- `ai+0x5C` (u16) is 0x005C for **both players** and 0x0008 / 0x0060 for the replicas, i.e. it is per-AI and the
  replica differs from the player. DECOMP-AUDIT calls this the is-alive word; that reading is unverified here.

## Next, in order

1. Read `FindHealthInAI` and `HostHpSvcValid` in phase1.cpp and make `lifehit` work on the player first - with the
   tool proven, every later result means something.
2. Then dump `ai+0x70` **as an array** (count/index source from the same audit) for player vs replica instead of
   comparing one word, and only then call the service absent.
3. Leave `CharacterBehavior`/`+0x1C` out of it until something shows it carries life.

## Corrections this run forced

- My probe read `entity+0x5C` three times over before the `status` output showed it was the world. The differential
  (player vs replica through the identical chain) is what exposed it - keep that column.
- Toolchain gotchas: `void* a = 0, b = 0;` declares `b` as `void` (C star-binding) - split the lines. Copying the ASI
  while a copy still runs fails silently on the lock; kill first, verify the hash, then start.

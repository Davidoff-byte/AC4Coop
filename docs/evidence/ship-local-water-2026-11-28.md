# Synced ships on the local water: the engine's water-height query and `[Coop] ShipLocalWater` (2026-11-28)

## Defect (owner, clip of run 11:50, asi 2CEE3701)
On the guest's screen a bound AI schooner rode with its keel out of the water at a tilt (`C:\caps\pass2_guest.mkv`,
4.3-5.9 s). The guest draws a bound hull with the host's whole transform from `ShipInterpDelayMs` (400 ms) ago: Z, pitch and
roll belong to the host's water at that time. The same run's `[ShipSmooth]` reported 2.4-12.8 % frames > 15 m/s² (pooled
6.68 %, 31 windows) vs 1.16-1.82 % in the other runs of the day at the same 400 ms delay (`shipsmooth_scan.py` over the
day's logs). That run differed in one more way: both copies ran at ~60 fps (display awake: ~300 writes per ring per 5 s,
host ship samples 48 per ring per 5 s) instead of ~30 fps (display asleep overnight: 23 per ring per 5 s), so the hull
reproduced more of the host's wave motion. `[ShipSmooth]` had no vertical/horizontal split; added below.

## Static (IDA, AC4BFSP.exe; MP binary not checked - presence claim, SP only)
- **Water height = `sub_9556B0`** (`__thiscall`, ecx = the ocean object, one arg = pointer to a position x, y, z, w; `retn 4`,
  result in st0). 35 callers. It returns 0.0 unless the bytes at ocean+0x12C6 and +0x12C7 are set and (x, y) is within the
  radius at ocean+0xAB8 of the centre at ocean+0xAC4/0xAC8; the height comes from `sub_951EC0` and `sub_951BB0` (two wave
  layers) and `sub_94ECC0`. The z of the input is zeroed (a 2-D query).
- **The ship buoyancy uses it.** `sub_1C98EE0` (the buoyancy force; vt `0x0272D378` ShipPhysicComponent slot 127
  `sub_1CA3C30` calls it first in its force order) loops over the hull's float points: array `comp+0x2E0`, u16 count
  `comp+0x2E6`, stride 0x60, local point at +0, radius at +0x30; world point = x·row0 + y·row1 + z·row2 + t with the body rows
  at `comp+0x540/0x550/0x560` and translation `comp+0x570`. For each point it samples `sub_9556B0` (ecx = `comp+0x360`) at the
  centre and at ±0.71·r, takes `max(comp+0x28C, h)`, and builds a local water plane from the three.
- **Helper `sub_1C4FDB0`** (bytes `55 8B EC 51 8B C1 8B 88 60 03 00 00`, 92 bytes): `ecx = comp`; `comp+0x360 == 0` → 0.0;
  else `max(*(float*)(comp+0x28C), sub_9556B0(comp+0x360, pos))`; `retn 4`, st0. Called by the watercraft code
  (`sub_1C75950` ×3, `sub_1C9EC70`). Other callers reach the same ocean through `[[obj+0x8F4]+0x1668]`.
- Earlier searches missed it because the function is unnamed and its callers are reached only through the physics vtable
  (`ops/evidence/water-height.md`, 11-08: "NOT FOUND"; `naval-s0-2026-11-25.md` S0.3).

## Change (asi 640A9181, built, gates: spine OK, signatures PASS, ini keys PASS)
`[Coop] ShipLocalWater` (default 0, re-read every second; `src/phase1.cpp` block before ShipInterp): X, Y and heading stay
the sender's; Z, pitch and roll come from five local samples (centre, ±ex, ±ey over the float points' extent) through
`sub_1C4FDB0`; Z = plane + draft, draft = EMA (τ 0.5 → 4 s) of (sender Z − local plane) at the drawn position. Bits:
1 = AI hulls driven by ShipInterp (guest), 2 = the peer's ship (ghost; its commanded pose, so the PuppetOnGhost deck placement
follows), 4 = measure only. `[ShipWater]` every 5 s: the own ship as the natural reference, the AI / ghost mismatch
(`dev` = sender Z − local plane − draft), transforms rewritten; `[ShipSmooth] split` = horizontal / vertical accel jitter;
`[ShipNear]` = nearest driven hull (test harness).

## Pre-registered (before run demo4, asi 640A9181)
- W1 the query is the water the own ship floats on: own-ship windows with plane range ≥ 0.5 m: corr ≥ 0.8 and rel sd ≤ 0.5 m,
  both copies.
- W2 the defect (ShipLocalWater=4): AI `dev` p95 ≥ 2 × own-ship rel sd (same copy, same phase).
- W3 the fix (ShipLocalWater=7): `[ShipSmooth] split` zJit % ON ≤ OFF and accel jitter % ON ≤ OFF; frames: keels in the water
  (owner's look).
- W4 both copies alive, no crash context.

## Run demo4 (asi 640A9181, commit 1830cd9 + working tree; host 30200, guest 29816; sea west of Dry Tortuga, 12:26-12:45)
Phase A only (`ShipLocalWater=4`, measure, nothing rewritten): the owner closed both copies before phase B. Rough sea: the
own ship's local plane moved up to 8.4 m within a 5 s window.

| check | host 30200 | guest 29816 | verdict |
|---|---|---|---|
| W1 own ship vs the query (windows with range ≥ 0.5 m) | 18/20 pass; corr 0.79-0.98, rel sd 0.12-1.07 m | 20/21 pass; corr 0.34-0.99, rel sd 0.12-0.43 m | 38/41 windows: **the query is the water the ship floats on**; 3 windows miss the bar (corr 0.79 at range 0.89 m; sd 1.07 m at range 8.0 m; corr 0.34 at range 1.75 m - not inspected, UNVERIFIED cause) |
| W2 AI hulls drawn with the host's transform vs the local water | (host has no driven hulls) | dev p95 **1.28-3.86 m** in 28/28 windows vs own rel sd ≤ 0.43 m (2× = 0.86) | **PASS: the defect is the host's Z on the guest's water** |
| ghost (the peer's ship, same measure) | dev p95 0.08-3.93 m | dev p95 0-3.87 m | the peer's ship has the same defect in this sea |
| W3 fix applied | - | - | **NOT RUN** (session closed) |
| W4 | no crash context | no crash context | closed by the owner, not a crash |

Verdict: cause confirmed and the engine query validated live; `ShipLocalWater` is **built, not yet verified live** (W3).
Template value stays 0.

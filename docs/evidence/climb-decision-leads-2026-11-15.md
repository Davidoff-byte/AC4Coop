# R4.2 climb decision: static leads (in progress), 2026-11-15

Goal (docs/design/player-decisions-r4.md R4.2): find, on PC, where the player's free-climb step is chosen and what the
chosen step's target is, so R4.3 can send it to the replica. Status: **the selection function is not identified on PC yet; the live hand-placement chain below it is.** Binary: AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA db `D:\ac4ida\a3` (jobs r42a-r42h).

## PS3 DWARF (names and layouts are leads, not PC facts; `ops/tools/dwarf_die.py`, cu 297)
- `scimitar::ClimbingSequencer::CandidateMove` (DIE 0x6FE894A, 240 B): movingLimb / staticLimb (IKLimbEffector),
  moveIntention, **targetedGuidance** (+0x10, TargetedGuidance), initialHandPosition, otherHandPosition,
  distanceBetweenHand, movementGlobalAxis, m_HighPriority, m_Value, localDirection; methods Compare / ctor / operator=.
  (Offsets after +0x10 did not decode with this tool: DW_OP location lists; to be fixed if the offsets are needed.)
- `ClimbingSequencer::TargetedGuidance` (DIE 0x6FE8801, 128 B): +0x00 m_GuidanceType, +0x10 m_Guidance (0x50 B payload),
  +0x60 m_Weight, +0x70 m_BrutePosition (Vector4). The climb target is a guidance item plus a position.
- `ClimbingSequencer::TransitionCandidateMove` (160 B: targetedGuidance, idealPosition, m_HighPriority, m_Value),
  `CandidateCoreMovement` (32 B: CoreMovementDefinition*, m_Value, m_PositionOnGuidance).
- `ClimbingDetectionState` (96 B): cycleType, actor matrix, left/right hand positions, currentDetectedType, HangType.
- Free functions `FetchCandidateMoveForOpen/Close(..., Array<CandidateMove>&, IKLimbEffector, IKLimbEffector,
  const Vector4&, const Vector2&, const ClimbingDetectionState&, bool, bool)` (DIE 0x71C1AC0 / 0x71C1B03): candidate
  generation per hand move ("open" / "close").
- False lead recorded: `GenerateCandidateDeferredRayCastRequests` is `FightManager::RangedWeaponBulletManager` (bullets).

## PC (verified statically)
- Climbing context (ActorContextID 30) is created by `sub_16B7080(Human+0x720)` (lazy at +0x70), case 30 of the context
  factory `sub_16F1670`; ctor `sub_1581770`, 0x30 bytes: main vtable 0x026D4638, interface vtables at +0x10
  (0x026D4588), +0x14 (0x026D4580), +0x18 (0x026D4578), +0x1C (0x026D4564), +0x20 (0x026D455C), +0x24 (0x026D453C),
  +0x28 (0x026D4504); +0x2C = 0.
- Every slot of interface 0x026D4588 starts with `call sub_156ADD0` (getter, role UNVERIFIED); climbing code sits in
  0x0150xxxx-0x015Fxxxx.
- `sub_151CDE0` (object with vtable 0x026D1784, not in the class map) embeds at +0xA0 an object built by `sub_1515260`
  that holds a fixed array of **20 elements of 240 bytes** (loop `v4 += 240`, 20 iterations, then fields at +5012/+5020/
  +5024); its destructor walks the same array backwards (`sub_151A110`). 240 B = PS3 CandidateMove size; that this is the
  candidate array is UNVERIFIED.
- Element-stride scan (0xF0 / 0xA0) over 0x01500000-0x01600000: `sub_1560190` indexes 240-byte elements (`imul edi,0F0h`,
  3.5 KB), `sub_1516910` loops 160-byte elements (13 x `add esi,0A0h`); both are candidates for the generator /
  selection. Not decompiled in full.

## Live climb trace (run 26944, asi bd2d195c, Pilot-driven, `[Probe]` hooks + write watch)
Pilot drove Edward from (37.4,-79.6) into the wall at (36.35,-81.71) with RT + A held (scratch `climbpush.py`, stick angle
from the learned camera offset 57 deg): wall run to z 6.06, then hanging at z 4.86 - the same spot and height as the peer in
host-p14-state-2.tap (contexts 8 -> 30). Stick left/right then shimmied him 1.5 m along the ledge.
- Hooks during the climb: `sub_156ADD0` (climbing getter) 1,252 calls in the climb, 158,145 by the end; `sub_1560190` and
  `sub_1516910` **0** - both static candidates above are not on this climb's path.
- `sub_156ADD0` is a lazy allocator: `*this` = a 0x530-byte block (ctor fields +0x24/+0x4C self-lists); its `this` is
  climbing context + 0x2C (context 0xFD1FFC30, block 0x407F7810 in this run). The block held no world-space vectors.
- **Hand grip points**: Human+0xC20 (36.87,-81.31,7.26) and Human+0xC70 (35.87,-81.30,7.20) - Edward's x +/- 0.5 m at ledge
  height (Human 0x35AA4E10); 0x50-byte records from Human+0xC10 (limb index i at +0x10 + 0x50*i).
- **Ledge guidance chain**: `[Human+0x564]` (0x451786A0) holds points along the ledge at z ~7.25, ~0.82 m apart
  (x 38.10 / 37.29 / 36.46 / 35.65 / 34.83, y -81.31); a second list at `[Human+0x570]`. Role (guidance vertices) UNVERIFIED.
- Write watch on Human+0xC20 (32 B) during a 0.8 s shimmy: 144 writes, all from `sub_48BE60` (36-byte copy: two movaps +
  a float) called by **`sub_1831280(this, limb, target, value)`**: record = this + limb*0x50; copies the target to +0x10,
  stores value at +0x50, sets +0x54 = 1 - the IK limb-target setter. 9 callers; during climbing only **`sub_13CF890`**
  fired (1,752 calls; `sub_169B200` / `sub_170F660` 0), always from 0x013D04CE.
- `sub_13CF890(this, target)`: builds a limb target from its argument (`sub_13B5E70`), ray-casts to the surface
  (`sub_643490`) and on a hit calls `sub_1831280` - per-frame IK contact placement, not the decision.
- Its caller `sub_13CFA60` (0x13CFA60-0x13D1741) is slot 11 of vtable **0x026BCD70** (ctor `sub_13D9690`, dtor
  `sub_13D96E0`; class not in the class map) - a per-frame task update that hands each limb its target. The climb step
  choice feeds it from above.
- Probe fix: `Plausible()` rejected addresses >= 0x7FFF0000, but the exe is large-address-aware and live objects sit above
  2 GB; bound raised to 0xFFFF0000 (built, not yet deployed).
## Next
1. Follow the limb target into `sub_13CFA60`: which member of the 0x026BCD70 task holds each hand's intended target, and
   who writes it (write watch on that member during a shimmy step - it should change once per hand move, not per frame).
2. That writer, or its caller, is where a climb step is chosen; snapshot its inputs (guidance pointer, position) for R4.3.

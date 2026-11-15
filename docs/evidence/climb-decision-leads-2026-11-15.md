# R4.2 climb decision: static leads (in progress), 2026-11-15

Goal (docs/design/player-decisions-r4.md R4.2): find, on PC, where the player's free-climb step is chosen and what the
chosen step's target is, so R4.3 can send it to the replica. Status: **leads only; the selection function is not
identified on PC.** Binary: AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA db `D:\ac4ida\a3` (jobs r42a-r42h).

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

## Next
1. Decompile `sub_1560190` and `sub_1516910`; look for a Compare-style sort over the 240-byte array (m_HighPriority byte,
   m_Value float) and the write of the chosen element.
2. Live: `[Probe]` HOOK on the chosen function while Pilot drives Edward up a climbable wall (Havana; Pilot `goto` +
   world queries), snapshot the chosen element's TargetedGuidance (+0x10, 128 B) - the ACU-RE-0042 method.

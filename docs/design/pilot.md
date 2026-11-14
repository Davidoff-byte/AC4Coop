# Pilot: drive Edward and read his surroundings without touching the desktop

Status: **builds** (2026-11-15). Live verification pending; see the end.

## Why
Solo tests need the game in the Havana save, Edward moved to a spot, and a read of who is around him. Until now that
took a person at the keyboard (title → Enter, login, Enter twice; never Space in the save menu). Sending OS-level
keystrokes from an agent is refused by policy and also hits whatever window has focus. AU-RW solved the same problem
for ACU with an in-process virtual controller, a focus keeper and a shared-memory status block; this is the AC4 x86
equivalent, written independently (AU-RW has no license).

## What (src/pilot.h, src/pilot.cpp, tools/pilot.py)
- `[Pilot] Enabled=1` (default 0). `KeepFocus` (default 0), `PadIndex` (default 0). Refused when `[Coop] VPadEnable=1`.
- Virtual pad: IAT entries for xinput1_3 ordinals 2/3/4 (AC4BFSP.exe imports XInput by ordinal) → pad `PadIndex`
  is always connected; its state is `Local\ac4pilot-pad-<pid>` while `enabled`, else neutral. No code bytes patched.
- Focus keeper: IAT GetForegroundWindow / GetActiveWindow return the game window; the window procedure drops
  WM_ACTIVATEAPP(FALSE), WM_ACTIVATE(WA_INACTIVE), WM_KILLFOCUS. User input is never blocked or forged.
- State: every player Human frame (phase1 OnFrameInner, game thread) writes `Local\ac4pilot-state-<pid>`: player
  entity, world matrix, and each `World::m_CharacterList` entity with position, +0x9C kind/role, AI component, fighter
  record status/role (offsets cited in pilot.cpp). Sequence lock: odd while writing. Layout fixed by `static_assert`.
- Client: `status`, `scan`, `tap`, `stick`, `goto` (closed loop; learns the camera yaw from Edward's motion),
  `boot` (A presses only, never moves a cursor; pair with `[Coop] AutoContinue=1`), `mem` (read-only dump), `watch`.

## Not done (next slices)
- Parkour/traversal decisions (the ACU work decoded the candidate selection); AC4 needs its own RE.
- Camera yaw read from the camera object instead of learned from motion.
- Keyboard injection (DirectInput) for prompts that have no pad binding.

## Verification
- Builds: Release x86 v143; `spine_gate.py` VERDICT OK; `verify_signatures.py` ORACLE PASS. No new engine call.
- Live: not yet.

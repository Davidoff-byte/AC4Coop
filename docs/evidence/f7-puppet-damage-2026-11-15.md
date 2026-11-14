# F7 follow-up: do the guards' strikes damage the puppet? (live, 2026-11-15)

Build 351378b (tree clean), asi 2e33f3c9..., run pid 22156; same setup as f7-strikes-2026-11-15.md (PuppetNavService=1,
held puppet from hold-guard-near.tap, gunshots PuppetStimSeq 2 and 3). Instruments: `[Probe]` entry hooks (read-only
recording, removed afterwards), `ops/tools/ftask.py` for the guards' FightStage. Verdict: **solo-tested: NO damage.**

## Health write paths hooked (static sources: bf-coop ops/evidence/character-health-setter.md)
| hook | VA | role |
|---|---|---|
| 10 | 0x0115ACC0 | player-side life apply (int16 +0x5C, dead flag +0xD4) |
| 11 | 0x019FB4C0 | CSrvNPCHealth SetLife (the only NPC int16 life setter) |
| 12 | 0x01C08EE0 | 36-byte wrapper that sets life through the health vtable (15 callers in 0x1148xxx-0x1160xxx) |
Snapshot: 8 bytes at ecx+0x5C (life, max) on hooks 10/11.

## Results
| window | guard attacks on the puppet (FightStage 7) | health writes that lowered a value |
|---|---|---|
| gunshot 2, 25 s | 14 (3 guards) | **none**: 50 SetLife calls, every one setting life = max (spawn initialisation, callers 0x1C13E4C / 0x1C08EF2); 0 player applies |
| gunshot 3, 20 s | 9 | **none**: 21 wrapper calls, all life = max (callers 0x19FB46F / 0x1372BF9); 0 player applies |
| positive control: guards hit Edward | (Edward provoked one guard) | player apply on Edward's health 0x35BA3990: 125 -> 105 -> 85 -> 65 -> 45 -> 25 -> 5 -> -1 (dead), 20 per hit; also Edward's hits on guards (SetLife 24 -> 5, 36 -> 35 -> 34) |
| control 2 | | Edward's health 0x41048600: wrapper called from 0x011488DF (sub_11488D0) with the new life (105, 85), then the player apply |

So a hit on a player-type Human is applied by `sub_11488D0` (no direct xrefs; it is an event handler in the table at
0x028CE2A0, paired with event id `dword_4DE7FC0` = copy of the global event id `dword_4E20518`, written by registration
`sub_114FAB0` at 0x0114FD28) -> wrapper 0x1C08EE0 -> 0x115ACC0. None of that runs for the puppet while guards strike it.

## Not established
- Where the chain stops for the puppet (attack contact, hit event post, or the puppet's health handler). The generic
  event post `sub_64A020` could not be probed: it already starts with a `jmp` (the mod's `[Coop] EventProbe` detour), so
  the byte guard refused the hook. Next: run with `[Coop] EventProbe=1` and compare events posted to the puppet vs the
  player during strikes.
- The puppet's own health object was not located (the AI-side container chain from phase1's guard probe led to a static
  object with 0 components for these bodies; `entity+0x70` is not the component array for an Entity).
- Design note (not a finding): the co-op rule "replicate intentions, no logic on replicas" means a hit on the puppet should
  reach the guest's real body over the wire, not damage the replica; that lane does not exist yet.

## Trace: where the chain stops (same run 22156, gunshot 4)
- `FightDamageEvent` class: name string 0x026FDD9C, descriptor 0x02A23C2C (base id 0x67F2E868 = DamageEvent, own id
  0x13F3BE59, size 0x170, ctor slot -> 0x018D2E70 -> 0x018C9470 which writes **vtable 0x02700720**; both functions had no
  boundary in the idb and were defined with the prologue scan). DamageEvent: descriptor 0x02A920CC, size 0x28.
- The event post `sub_64A020` calls `event->vt[+0x24](receiver)`; for both damage classes that slot is the shared
  `0x0064A080`. A `[Probe]` hook there with an 16-byte snapshot of the event (vtable) recorded, in a window with 19 guard
  attacks on the puppet (ftask: NormalAttack 13 + 5 + 1): **102 posts of FightDamageEvent (vt 0x02700720) with receiver =
  the puppet (0x31A8FF50)**, all with the same event object (0x0A960010); and no health write (section above).
- So the guards' hits **land and post damage events to the puppet**; the chain stops at the receiver. The repeated post of
  one event object is consistent with an event the receiver never consumes (UNVERIFIED reading).
- Not completed: the per-hit event count on Edward (two control attempts: the first had no hit on Edward in the window,
  the second lost the fight reset after Edward died). The handler that applies a FightDamageEvent to a player (receiver
  side) is not traced; the known tail is event id dword_4E20518 -> sub_11488D0 -> 0x1C08EE0 -> 0x115ACC0.

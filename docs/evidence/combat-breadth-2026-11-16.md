# Combat breadth: a real pad fight against a soldier group, and a crash family (2026-11-16)

Two copies on one PC (`two_copy.ps1 -Interactive`, D: install, AC4BFSP.exe md5 `2058342866688f780c8b34526a65bc35`), settings
`NativeReplica=0 FightIntents=1 IntentLane=1`. The guest runs `tools/two_copy_fight.py <pid> --sec 60 --counter`:
1. walk to the soldiers at (30,-75);
2. slot EnterFight on the nearest soldier;
3. X every 0.6 s, with B (counter) on every 4th beat.

No OS input: everything goes through the virtual pad. With `PlayerDamageProbe=1`, the probe also follows the local
player's own strikes (`player-target`: hits whose attacker, FDE+0x58, is the local player).

## Results
| run (asi) | guest strikes: hit-phase FDEs / DamageEvents built | guest requests (`[OD] tx`) | host applied / kills / unresolved | guest deaths from the host | guards' hits on the guest (PDR applied) | both alive |
|---|---|---|---|---|---|---|
| 30380/26416 (b3ab07bd) | 65 / 5 | 8 | 1 / 1 / - | 1 | - | **host crashed** (below) |
| 4240/12996 (8d4607ee, SenseOwnerGuard=1) | 448 / 27 | 33 | 18 / 6 / **7** | 6 | 9 of 9 | yes |
| 32076/21224 (ec00f440, + corpse skip) | (probe off) | 5, **corpse=3 skipped** | 3 / 2 / **0** | 3 | 10 | yes |

- **Kills on several opponents work.** Over 60 s the guest engaged 3-5 soldiers in turn. The host decided every death,
  and each death reached the guest (`[Kill] peer DEATH`, then `[Mirror] snapshot death applied=1`).
- **Unresolved requests were strikes on corpses.** Guard F000B3EE: the host's death arrived at guest log line 2776. The
  guest still sent lethal requests afterwards (lines 2855 and 2877). The host could not resolve them, because the guard
  was gone. Fix: `OwnerDamageGuestPump` sends nothing for a copy that is already dead here (`corpse=` on the `[OD]` line).
  Unresolved went from 7 to 0.

## Crash family 0x188F054: a sight check with a non-object observer
Three WER dumps today. Each was read with cdb `.ecxr; kb`:

| dump | role | eip | faulting value |
|---|---|---|---|
| 30380 | host, during this fight | `0x0188F054 mov ecx,[esi+9Ch]` | esi = 0 |
| 20292 | host, PDR default-on run | `0x0133BD03` in `sub_133F010`, called from `0x0188F04F` | (the same entity) |
| 22744 | guest, probe run 3 | `0x00520FC0` in `sub_5220C0`, called from `0x0188F07A` | esi = `0x00937563`, a code address |

All three share the caller chain `0x10390D8 ← 0x103D558 ← 0x103EBF2 ← 0x159BF80 ← 0x156CC8E ← 0x15DA86F`.
- In the decompile (ida_batch cr1/cr2), `sub_1039070` calls `sub_1895290(.., target, *(this+120))` and then
  `sub_188F030(.., *(this+120))` with no check.
- `this` is the perception object of `sub_103D250`, and `this+0x78` (its `v5[30]`) is its owner entity.
- The only gate before those calls is `sub_101CAA0(this, target)`: thiscall, `retn 4`, returns a bool in al, and has
  **one** caller, `0x010390B0` (cr3). When it returns false, `sub_1039070` takes its own "not applicable" path.

**`[Coop] SenseOwnerGuard`** (`src/sense_guard.cpp`):
- At `sub_101CAA0`'s entry (prologue `55 8B EC 53 57`, 5 bytes stolen whole) it returns false when `*(this+0x78)` is not
  a plausible object: outside [0x10000, 0x7FFF0000), unreadable, or with a vtable outside .rdata (the bound
  `deref_guard` uses).
- Valid owners run the original unchanged.
- **Live:** run 32076 (asi ec00f440) fired twice, with owner `0xBBD00000` and self `39BCC820`, and the host kept running.
  The default is now **1** (`=0` turns it off), as for the earlier null guards (fired and survived).
- Why the owner is null or garbage is **not established**. UNVERIFIED reading: a perception object that outlived its
  entity.

## Not done: counters
- The guest's B taps cannot counter a host guard. The guest's copy of that guard does not attack the guest's Edward: its
  AI is off (stage-1 take-over), and the attack happens on the host, against the puppet. The guest's fight system
  therefore never sees an incoming attack.
- The hit reaches the guest as a PlayerDamageRelay DamageEvent, so blocking and countering are not consulted
  (`player-damage-relay-2026-11-16.md`).
- In the engine, counters are fight actions (CounterKill/CounterHurt/CounterThrow/CounterTool classes, class map). The
  guard decision tree `sub_13E2230` lists the Counter* stages (`ops/evidence/guard-ai/decision-tree.md`).
- Native counters need the attack to exist on the owner's machine. That is a design choice still to make.

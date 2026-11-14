# F7: why the FightManager dropped the puppet, and the fix (2026-11-14)

AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35. IDA headless (scratch o71-o86). Solo host + `wire_tap.py replay`
puppet. Verdicts: **static chain VERIFIED in IDA**; **fix solo-tested live** (one run); two-machine **not tested**.

## Run 476 (build c0bb544, asi 15a50551): the drop
- FightProbe on every removal of the puppet's role-7 record: `from=0161F870 <00A00EB0 <00A0105A` (3 of 3, two fires).
- `fightmgr.py`: target = PUPPET, then none after 1-2 s (fire 2); one hold of 37 s (fire 1).

## Static chain (IDA)
| step | address | receipt |
|---|---|---|
| "drop current target": `mov eax,[ecx+304h] / test / jz / push eax / call sub_161F420 / retn` | `0x0161F860` (undefined in the idb) | o72 disasm + bytes `8B 81 04 03 00 00 85 C0 74 06 50 E8 B0 FB FF FF C3` |
| its only references: tail jumps | `0x01789609`, `0x01789613` | o72 xrefs + full-image E8/E9 scan (refscan.py) |
| statechart action `sub_17895F0` (cdecl, 1 arg): fm = ((a1+0xC)->+0x70)->+0x8DC; drop fm+0x304 | `0x017895F0` (defined in o73) | o73 decompile |
| only reference to `0x017895F0`: transition descriptor (+8 guard 0, +0xC action) | `.data 0x029ED630` | full-image dword scan; o75 bytes |
| wired as `sub_9F1610(0, state v97, E50, E50, state v95)` | `0x01788275` in `sub_1786BE0` | o76 disasm + decompile |
| E50 = `dword_4E18E50 = sub_9F17B0(0xF58E6E97)` | `sub_1786BE0` | o76 |
| `sub_A00E30` drains the deferred event queue into `sub_A00470`, which calls descriptor +0xC | `0x00A00EAB` (ret `0x00A00EB0`) | o71, o72 (matches the live return chain) |
| E50 posters: `sub_17AF5F0`, `sub_17BFD10`, `sub_17BFF50` (per tick) | xrefs to `0x4E18E50` | o77, o78 |

`sub_17BFF50` (owner v3 = ecx+0xC; v3+0x70 = world), per tick:
1. `0x017C012A call sub_16090C0(player)`: every fighter in the player's fight group has a target.
2. `0x017C013B call sub_1608F70(fm, player)`: some fighter targets **the player**. Yes → `jnz 0x017C021E`, skipping step 3.
3. `sub_17A9C30(v3)`, then `0x017C0180..01D7`: is any entity of list v3+0x250 within sqrt(tuning(0x4DE6D48)+0x2AC)
   of the player? None → `0x017C01FB`: post E50 + E20 → the transition action drops the target.

With every guard fighting the puppet, step 2 is false and the guards are near the puppet, not the player.

## Fix: `[Coop] ConflictKeepPuppet` (commit 9592773; default 0; installed when the key is present; read live)
- The call at `0x017C013B` (bytes `E8 30 8E E4 FF` checked before patching) now also answers yes when the
  FightManager's target record (+0x304 → handle +0x14) is the puppet.
- `sub_1608F70` has one code reference (o80). It cannot be asked about the puppet directly: it takes the group
  from its argument's AIComponent+0x98.

## Run 21832 (build 9592773, asi b62e36e4), `ConflictKeepPuppet=1`, one gunshot (seq 1, 39 receivers)
- `fightmgr.py`: target = PUPPET from 18:06:12. **No change for over 5 minutes**; there was no `[FightRm]` for the puppet.
- `[Poi] keepPuppet on=1 tests=58 kept=58`, then 278/278, then 289/289; afterwards tests=0.
- Negative control: the same steps without the flag (run 476) dropped the target every 1-2 s.
- HUD: the red eye and a red minimap appear (the player's conflict HUD stays on while the fight lasts).
  This is a side effect to review.

## Next blocker (measured, not yet explained)
Read-only `fgroup.py` (layout from `sub_16090C0` / `sub_15F0390` / `sub_15DABC0`, o79/o84):
- The puppet's fight data is the role-7 record. The player has none.
- **5 guards** (role 2) are in the puppet's group, all with target = PUPPET, and were 1.4-2.4 m from it at the first
  sample.
- Over 5 minutes their positions did not change by 0.1 m while the replayed puppet walked 40 m away. **The guards
  never chase or attack.**
- NavNullGuard took its null exit 0 times. The puppet's AIComponent+0x94 is non-null (aiitems.py).
  `fm+0x6D4..` holds a unit direction, not a position (an earlier hypothesis "stale target position" is withdrawn).

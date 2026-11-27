# `fm+0x30C`: identity still open - sound descriptors and a vtable-only add site (static, 2026-11-16)

Follow-up to the team-gate identification. `[DECOMP]`, idb `D:\ac4ida\enemy`.

## Established

- The vector is `fm+0x30C` (base) with `u16` count at `fm+0x312` - both used consistently by `sub_15DABC0`,
  `sub_161BD30` and the removal path.
- **Removal** is inline in `sub_1621670` (0x01621670): scans the array for a value, `dec dword ptr [edi+314h]`,
  compacts. (0x314 is a separate entity counter, not the count.)
- **One add site**: `sub_13FE5E0` at 0x013FEB86 does
  `push off_295A35C; lea ecx,[edi+30C]; call sub_6F5CF0; push eax; mov ecx,esi; call sub_9FBBE0`.
  So the shape is: a type/descriptor from `sub_6F5CF0()` (`_UNKNOWN**` returning `off_279C3D4`), the *value* from a
  global (`off_295A35C`), dispatched through `sub_9FBBE0` with the variant holder in `esi`. Not a bare push_back.

## Consequence

The map holds what `off_295A35C` names, appended by the fight-setup path around `sub_13FE5E0`. That means team
membership is *written by the engine during a fight setup*, which is consistent with `sub_1625590` being the way in and
with `sub_15DABC0` clearing any target whose `entity+0x10` is absent from it.

## Still unread (UNVERIFIED)

- What `off_295A35C` / `off_279C3D4` actually are (descriptor names not resolved; `known.py` is for crc32 ids, these are
  static pointers, so rule 13 does not apply).
- Whether `sub_13FE5E0` is reachable for a replica at all, or only from the player-side fight setup.
- Whether a forced append is needed at all: `[PoiFO]`'s `teams=` is still the cheap way to learn whether the map matters
  before writing to it.

## Next

Resolve the two globals by their readers (`off_295A35C` first - if it is a faction/team descriptor, the comparison in
`sub_15DABC0` and this append are the same currency and the gate is genuinely about the map).

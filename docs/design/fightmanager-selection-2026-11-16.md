# The FightManager does select candidates - `sub_1614EE0` (static, 2026-11-16)

Corrects a working belief: the guard decision *tree* has no candidate node, and that was read as "there is no selection".
There is. `sub_1614EE0` is a candidate->target selector called from `sub_1626AB0` (**FightManagerUpdate**, the function
`[PoiFO]` already wraps), at 0x01626E71.

## Shape

```
if (this+2160 == 0) return;                       // gate
... only when a timer/progress test passes (v4 >= this+2148 etc.) ...
v7 = *(*(this+772)+16) if non-zero                 // the current target, +0x10
iterate candidates v9 over an array from sub_15F04D0(...)
    iterate records v11 from sub_15F05B0(v9, ...)
        if ( v11
             && !sub_1561B50(v9)                    // filter on the candidate
             && *(v11+68) != v9                     // record+0x44 already this candidate
             && (!isCurrent || *(v9+64) != v11) )   // exclusion vs the current target
                sub_1529830(1);                     // set / switch target
```

`sub_1561B50(ent)` is `sub_15574E0(ent+0x14)` - a test on the entity's **handle** field (`+0x14`, the same refcounted
handle `sub_15DABC0` decodes). So an entity whose handle is "in use" is skipped as a candidate.

## Why this matters for the replica

`sub_1529830(1)` is the set-target step, and it is *inside* the manager. So a replica that never appears in the candidate
list, or whose handle fails `sub_15574E0`, will never be chosen no matter what the busy gate or the order layer say.
This reframes the three layers again, and it is now a candidate-list question, not a membership one alone.

## Not read yet (next)

- `sub_15F04D0` - where the candidate array comes from (entity enumeration vs team list).
- `sub_15574E0` - what "handle in use" means, and whether the replica's handle differs from a real player's.
- `sub_1529830` - the actual set (argument 1 = ?), so a forced call can mirror the engine exactly instead of using
  `sub_1625590`.

## Receipts

`sub_1614EE0` 0x01614EE0 (len 3781 decompiled, 0x01614EE0-0x0161504E); caller xref 0x01626E71 in `sub_1626AB0`;
`sub_1561B50` 0x01561B50 -> `sub_15574E0(a1+20)`. Tags `[DECOMP]`, idb `D:\ac4ida\enemy`.

## The three functions, read (same night)

- sub_15F04D0(a1, a2) (0x015F04D0): **builds** the candidate vector. Iterates the array at 1+16 (count u16 at
  +22), then each entry's array (+8, count u16 at +14), appending every entity into 2 (data [a2], cap
  [a2+4] & 0x3FFF, count u16 at +6, growth factor 3/1 via sub_A09AB0). The selector passes the current target
  record's +0x10, so **the candidate pool is the fight group** - no membership, no candidates.
- sub_15574E0(this, a2) (0x015574E0): takes a pointer to a handle. Returns 1 if *a2 is present in the array at
  	his+936 (count u16 at +942), else compares sub_1311FB0(x+156) on both sides and, when they differ, consults
  sub_11D0EA0 / +7. Practical meaning: **already engaged**.
- sub_1529830(rec, a2) (0x01529830): if (rec+0x18 != 7) { rec+0x180 = a2; }. So the "is target" state is a single
  byte at ec+0x180 - the same byte sub_15DABC0 writes when it invalidates a target. This is the honest setter and
  [PFF] now calls it instead of relying on the create path alone.

## Field confirmation and one gap

sub_1529850(rec, v) is the status setter: if (v != 8) *(rec+11) = v - i.e. ec+0x2C is a status, 8 is the
"no change" marker, and PFF writing 1 is status 1, not an invented flag.

**Gap, UNVERIFIED:** sub_1529830 refuses to set +0x180 when ec+0x18 == 7, and +0x18 is not identified. No
immediate write of 7 to +0x18 exists (C6 ?? 18 07 sweep over 8 ModRM forms: 0 hits), so it is set computed. Until
it is read, "the set silently no-ops on a status-7 record" is a real possibility and the [PFF] log alone cannot tell
that apart from "set worked and the guard ignored it" - 	argetIsPup in [PoiFO] is the discriminator.

## ec+0x18 identified - faction pointer, 7 is a sentinel

Instruction level: sub_1529830 is cmp dword ptr [ecx+18h], 7 / jz - a **dword** compare, so it is not a small enum.
The same field is dereferenced as a faction in sub_1625330: 18 = *(rec+0x18) and then sub_19017C0(v21, v18+20),
the identical relation test EnemyOf runs on ResolveFaction results. So ec+0x18 holds a faction pointer and 7 is a
sentinel meaning "no faction / unresolved".

Consequence for the replica: if its record's +0x18 is ever the sentinel, sub_1529830 silently refuses to set
+0x180. The [PoiFO] targetIsPup counter is the discriminator; an [PFF] enlisted=n line alone cannot show this.

Still unread: who writes +0x18 on a created record, and whether a replica record gets a real faction there.

## The engagement filter, base resolved

sub_15574E0 reads an array at 	his+0x3A8 with count u16 at 	his+0x3AE (disasm 0x015574E3/0x015574EA) and returns 1
when the handle *a2 is present. Its only caller is sub_1561B50, which passes entity+0x14, so the list is at
**entity+0x3BC / count entity+0x3C2** and the compared value is the entity's handle at +0x14.

[PFF] now logs engaged=<0|1> by calling sub_1561B50(pup) directly before it sets anything, because if the filter
already calls the replica engaged, selection skips it before any record state can matter and the hand-set is cosmetic.

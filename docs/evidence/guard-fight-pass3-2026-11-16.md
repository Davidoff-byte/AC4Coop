# Pass 3: perception works, enlistment never happens (2026-11-16)

Owner-driven, host 29096. New diagnostic `[PoiFO]`. Verdict: **FAIL**, and the failure is now localised to fight-group
enlistment, one layer below perception.

## Perception is fine

```
[Sight] ... probe n=65 cone=9 VIS=9 blocked=13 ... puppetPicks=65 switches=1 | onVis: hostile(G,P)=1 hostile(G,player)=1
```

The guards see the replica (`VIS=9`), classify it hostile (`hostile(G,P)=1`) and it is picked 65 times. So "they walk
right past the puppet" is not blindness.

## Enlistment never happens

```
[PoiFO] grp total=2 enlisted=0 pupInGroup=0 targetIsPup=0
```

Every sample. The fight group holds 2 records (`sub_161BD30`'s `this+780`/`+786`), none carries the enlist flag
`+0x2C == 1`, and the replica is not among them. With `pupInGroup=0` the order layer cannot ever help, which is why
`FightOrderPuppet` did nothing in pass 2.

## Mechanism, from the IDB

`sub_1608960` (`__userpurge`, a1 = guard, a3 = candidate, a4 = context) is the busy gate:

```
if (*(BYTE*)(a1+952) && v5 && a3 != v5) return 1;      // v5 = handle at a1+956, decoded
```

i.e. **if the guard already has a current target and the candidate is not it, the candidate reads "busy"** and the
caller does nothing. While a guard is fighting Edward, every other candidate - the replica included - is busy by
construction. Only after the target is dropped does anything else get considered, and a replica that never emits an
attack chain does not win that either.

This is the ledger's old "Do nothing" lead (`ops/evidence/guard-ai/decision-tree.md`), now confirmed in the function
itself rather than inferred from the tree.

## Next instrument

Detour `0x01608960`, log `(candidate=a3, current=v5, result)` per call with the puppet flagged. That gives, per
candidate, whether the replica is refused and by which branch - and is a read-only hook, so behaviour is unchanged
until a patch follows it.

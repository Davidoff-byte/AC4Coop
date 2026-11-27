# Fight records are keyed by FACTION, not entity (static, 2026-11-16)

`[DECOMP]`, idb `D:\ac4ida\enemy`.

## The lookup

```
sub_15E50F0(fm, entity):
    v3 = ResolveFaction(entity)          // 0x133A4C0 -> ai+0xA4
    for each record in fm+780 (count u16 fm+786):
        if (*(record+0x18) == v3) return record
    return 0
```

So a fight **record represents a faction inside this fight**, keyed by `rec+0x18`, and `sub_1625330` takes two of these
(`v13 = sub_15E50F0(a2)`, `v14 = sub_15E50F0(a3)`) before doing anything else. This finally explains why `rec+0x18` is a
faction pointer and why `sub_1529830`'s `!= 7` guard exists at all.

`GetEntityAI(e)` confirmed as `*(*(e+0xE8)+0x1C)` (0x006F5F70), so `ResolveFaction` -> `ai+0xA4` is the same field the
probe logs as `fac=`.

## Consequence for the replica

Our passes logged `facP=00000000` for the puppet on every sample. With that:

- `sub_15E50F0` compares `rec+0x18` (non-null pointers) against 0 for every record, so it returns 0;
- `sub_1625330` therefore enters with `v13 == 0`, and the whole player-branch body is guarded by `if (v13) { if (v14) {`
  - it never runs;
- so no record is created for the replica, `pupInGroup` stays 0, and every downstream layer (order, stage, damage) is
  unreachable.

This is the first candidate root that explains **all** observed numbers at once, including `[FI] puppetFight=0` and the
single FDE that arrives but is never consumed.

## What would falsify it

If a pass shows `fac=` non-zero on the replica and `pupInGroup` still 0, this is wrong and the stop is elsewhere.
`[PoiFO] rec18/rec2C/rec180` plus the probe's `fac=` give that in one sample.

## Next

Who writes `ai+0xA4`. Plan: enumerate `mov [reg+0A4h], src` over the eight ModRM families (only the `0x80-0x8F` band was
swept, 96 hits), then keep only functions that also touch `+0xE8` / AI creation - that intersection is small, unlike the
raw sweep.

## Note on C2

This supersedes my earlier hedge: `facP=0` is no longer merely "does not prove non-enemy", it is now the leading
explanation, and the `hostile=1` observation becomes the anomaly to explain (the Sight probe's `hostile` is probably not
`EnemyOf`).

## Writer sweep, first results (no conclusion)

mov [reg+0A4h], src over all disp32 ModRM forms: **459** raw sites - the field is common. Two small owners read:

- sub_1B89950(this, a2) (0x01B89950): when *(a2+0x50)==2 && !*(a2+0x6E) && !this[20..], copies *(a2+0x64)'s
  +0x0C/+0x10 into this+0xA0/this+0xA4. A pair write from another object.
- sub_1C60760(this) (0x01C60760): zeroes a block, then this+0xA4 = *(*(v3+92)+0x888) - world-derived.

So at least two different things land at ai+0xA4 depending on the writer, and the "faction" reading rests on
ResolveFaction alone. **UNVERIFIED which writer produces the value ResolveFaction is meant to return.** The 459-site
space is not sweepable by reading; the discriminator is the caller of sub_1B89950 (is it the AI ctor?) versus
sub_1C60760 (is it a nav init?).

METHOD: full ModRM enumeration of 89 /r A4 00 00 00, function lookup on the band hits, two bodies read.
COVERED: 2 of 459, chosen by address band and size. NAMES: sub_1B89950, sub_1C60760.

## Writer callers walked (second pass)

- sub_1C7A560 (0x01C7A560) is a **constructor**: allocates a 16-byte sub-object via sub_A085B0(0x10,4,...), stores
  it at 2+0x10, clears the block, zeroes this+0xA4, then calls sub_1C60760. So +0xA4 is populated on the
  **creation** path.
- sub_1B93B30 (0x01B93B30) is a predicate returning a 4-dword result; it calls sub_1B89950 in one branch. Not a ctor,
  so its write is a re-assignment.

## The guard in front of the read - cause splits in two

ResolveFaction only takes ai+0xA4 when sub_F916A0(ai) is true. That function (0x00F916A0) is a **type gate**: it
calls t+20 for an id and compares two u16 fields (+0x2C/+0x2E of the returned record) against a global object
from sub_15295E0, returning a range test. When it fails, ResolveFaction falls through to the handle path
(sub_11DDD60) instead.

**So facP=0 has two possible causes:** (a) +0xA4 really is 0, or (b) the replica's AI class fails the type gate and
the fallback returned null. The consequence is unchanged - no faction, no record, no fight - but the cause is now
two-branch and must be separated by probe values, not by reading.

[PL] fac= already prints the value; what is missing is *which* branch produced it. Adding the type-gate result to the
probe is the next cheap step, and it is a one-line read - not a live-only fact.

## The type gate is not selective - cause (b) mostly dies

sub_15295E0() (0x015295E0) is eturn off_2994EE8, and off_2994EE8 -> 0x299C260 has +0x2C/+0x2E = **32767 / 32767**
(0x7FFF, the u16 max, i.e. a sentinel ceiling). The gate subtracts the candidate's words from those, so with 0x7FFF as
the reference the comparison is satisfied for effectively any sane id.

**Therefore:** sub_F916A0 fails only for pathological AIs, and acP=0 should be read as **i+0xA4 genuinely 0**, not
as the fallback firing. Cause (b) is downgraded to "unlikely, not impossible" and the probe's new tg= will confirm 1
on the replica in one sample.

**METHOD:** decompiled the gate, the reference accessor, and dereferenced the static record to read the compared words.
**COVERED:** the only reference record, both compared fields. **NAMES:** sub_F916A0, sub_15295E0, off_2994EE8,
x299C260.

## Who creates the object at all - and how +0xA4 gets its value

sub_1C60760 derives the value from 2 = *this, 3 = *(*(v2+0x108)+4), 1 = *(v3+0x5C)+0x888, and stores 1 at
this+0xA4 (then copies *v1 / 1[1] into +0x88 / +0x8C). So +0xA4 is a **borrowed context pointer**, not a scalar
tag, and the copy of two dwords beside it says the ctor expects a populated struct.

Creation is **lazy on demand**: both ctor callers allocate 176 bytes (sub_A085B0(0xB0,16,...) + sub_4061F0) and only
then initialise:

- sub_1C84870 (2 callers): returns the existing object if this[3] is set, else allocates and constructs.
- sub_1C848D0 (1 caller): same, keyed on this+0x0C, then does ***(this+104) = -11 and posts through sub_1C60A80.

**So a null +0xA4 on a replica has three candidate origins**, and they are distinguishable:

1. the object exists but the 3 chain was null at ctor time, so 1 stayed null (value 0 stored);
2. the object never existed until our probe forced a read (then the copy is fresh, not spawn-time);
3. nothing in the replica path ever calls sub_1C84870/sub_1C848D0, so no AI-side context is built for it.

tg=1 plus fac=0 narrows this to (1) or (2); a null ai would be (3). All three are visible in one probe sample,
which is why no new code is needed for the next step.

## The +0x888 context is world-scoped, and there is a second writer

Chain decoded: `[this] = A`, `A+0x108 = B`, `*(B+4) = v3`, `v3+0x5C = world`, and the ctor stores the **address**
`world+0x888` at `+0xA4` (`v1 = world + 0x888; *(this+41) = v1`, 0x01C6079A). `sub_1610E40` dereferences the same field
(`*(*(this+8)+0x888)`) and, before that, gates on `sub_5220D0(entity)+0xF0` - the CharacterBehavior cache - through
`sub_120E230`.

So the ctor value is **world-scoped** (identical for every entity in that world), while the record lookup needs a
per-entity key. The two writers therefore have different semantics:

- ctor `sub_1C60760` -> `+0xA4 = world+0x888` (an address inside the world, not a pointer to a per-entity thing);
- `sub_1B89950` later overwrites `+0xA4` from a borrowed object's `+0x14` pair.

Since `+0xA4` is not `0` after a successful ctor, `fac=0` means the ctor either never produced a value (null chain) or
never ran - i.e. the probe forced the lazy creation (scenario 2), or the object existed but `*(B+4)`/`*(v3+0x5C)` was
null at the time (scenario 1).

**Falsifiable consequence:** if a pass shows `ai` non-null and `fac=0`, and `[PoiFO]` shows no record, then "a world
address is enough" is wrong and the per-entity overwrite (`sub_1B89950`) is the one that matters. If `fac` comes back
equal to a world address, the ctor ran and the per-entity stage is what never happened.

METHOD: chained five derefs from the ctor, confirmed `this+8 = world` via `sub_1610E40`. COVERED: both writers, one
consumer. NAMES: `sub_1C60760`, `sub_1B89950`, `sub_1610E40`, `sub_120E230`, `sub_5220D0`.

## world+0x888 writers (mechanical read)

Five writers of `mov [reg+888h], src`. One is small enough to read: `sub_F79E50` (0x00F79E50). It resolves
`result = ***(this+0xC)`, then `result = result[567]` (`+0x8DC`), and writes `v3[546]` (`+0x888`) from `result[549]`,
clearing `[550]`/`[551]` and copying `result[549][0..1]` into `[542..545]` first.

So `+0x888` is slot 546 of a struct whose adjacent field is slot 567 (`+0x8DC`) - the same field the status path
`sub_1397450` tests (`*(v3+2268)`). The two are the same structure family, and `+0x888` is copied from a slot 549
pointer, not composed fresh.

**Dead end for now:** the other four writers are inside `sub_8F0BD0` (0x7080 bytes), `sub_8EA050`, `sub_18A6ED0`, and one
site at `0x16261DA` with no function boundary in this idb (band `0x1626xxx` is FightManagerUpdate - RULES-RE rule 2 says
scan back for a prologue before concluding anything there). Not swept this batch.

METHOD: full ModRM enumeration of `89 /r 88 08 00 00` (5 sites), one body read. COVERED: 1 of 5. NAMES: `sub_F79E50`,
`sub_1397450`.

## Rule-2 scan at 0x16261DA: it is a block FILL, not a setter

Scanning back 0x1E0 bytes found no prologue (`55 8B EC` with a frame, or `53 8B DC`), and the site sits inside a run of
consecutive `mov [esi+disp32], ebx` stores:

```
0x16261D0: mov [esi+880h],ebx ; [esi+884h] ; [esi+888h] ; [esi+88Ch] ; [esi+890h] ; [esi+894h] ; [esi+8A8h] ; [esi+8ACh]
```

No `xor ebx,ebx` exists in the 0x2E0 bytes scanned, so the fill value arrives from the caller. Either way this is a
**fill of one value across the 0x880..0x8AC block** - an init/clear idiom, not a targeted assignment - so it is a negative
for "setter of +0x888", not a lead.

Secondary value: the block tells us which fields belong together. `+0x888` sits between `+0x884` and `+0x88C` and the whole
`0x880..0x8AC` span (plus `+0x8DC` from the earlier read) is one manager-shaped record, cleared as a unit.

METHOD: backwards prologue scan + byte-window scan for the zeroing idiom. COVERED: 0x2E0 bytes, one window. NAMES: none
(this band has no function boundaries in the idb).

## world+0x888 is an EMBEDDED MANAGER, and its ctor writes 4

`sub_8EA050` (0x008EA050) is a constructor: it installs vtable `off_1E7E0DC` then `off_1E83A14`, initialises arrays at
`+0x830`/`+0x840`, and at 0x008EA1E4 does `mov ecx,4; mov [esi+888h],ecx` while clearing the surrounding fields with `edi`.
So:

- `world+0x888` is **not a pointer slot** - it is the first dword of an **embedded object** occupying roughly
  `0x880..0x8B0` (plus `+0x8DC` seen earlier), and its initial value is **4**, not 0.
- The deref in `sub_1610E40` (`*(*(this+8)+0x888)`) therefore reads that first dword, and the accompanying `v26 = v6+2184`
  keeps the address - consistent with the AI ctor storing the **address** `world+0x888` in `+0xA4`.

Class: vtable `0x01E83A14`, exactly two references - the ctor and `sub_8E88B0` (0x008E88B0, likely the accessor/creator).

**Negative:** no name is obtainable - there is no name string on the vtable, and `known.py` cannot help (a vtable is an
address, not a crc32). Any label for this manager is inference until a string or caller proves it.

METHOD: byte-level read of the store (not decompiler text, which printed neither `888h` nor 2184), vtable dump, xref on the
vtable. COVERED: full ctor body, both vtable refs. NAMES: `sub_8EA050`, `sub_8E88B0`, vtable `0x1E83A14`.

## What is left of the +0x888 writer list

Of the five, two are now characterised: one is an init **fill** (0x16261DA, block 0x880-0x8AC) and one is this **ctor**.
Remaining: `sub_8F0BD0` (0x7080 bytes) and `sub_18A6ED0`. Neither is read; both are large.

## C4 (self-correction): there is NO per-entity writer - the field is world-scoped, full stop

All five writers of `fm/world+0x888` are now accounted for:

| site | kind |
|---|---|
| 0x16261DA | block **fill** across 0x880-0x8AC, value from caller |
| `sub_8EA050` (0x008EA1E4) | **ctor**, writes 4 as the object's first dword |
| `sub_18A6ED0` | **constant init**: writes 709035168 across `+0x850..+0x89C` and 709035169 across `+0x898..`, incl. `[this+546]` |
| `sub_F79E50` | copies `[+0x8DC][549]` into slot 546 |
| `sub_8F0BD0` | large (0x7080), **not read** |

The two constants resolve to **nothing** in the AC4Tweaker dictionary (tried decimal, then `0x2A4304A0`/`0x2A4304A1`) - they
are plain numbers, not class/property crcs.

**So my earlier statement "a second writer (`sub_1B89950`) is what would make it per-entity" is wrong** - `sub_1B89950`
writes `+0xA4` on the **AI** object, whereas these five write the **manager's** first dword. Different fields, different
objects, and they never interact. `AI+0xA4` is simply `&(world+0x888)`: constant per world.

### What that implies for the record key

If the key is a constant address for the world, records are **not** per-faction at all - each fight manager has its own
record, and the per-member structures hang off `record+0x10` (the group/subs arrays). `sub_15E50F0` is then a
"find my own record" helper, not a faction lookup.

`facP=0` therefore means the AI never got `&(world+0x888)` - the ctor chain `*(A+0x108)->+4`, `v3+0x5C` was null at the
time - rather than "this body has no faction".

METHOD: all five writer sites enumerated by byte pattern and classified; both constants looked up (rule 13) and found
unnameable. COVERED: 5/5 writers, 4 read. NAMES: `sub_8EA050`, `sub_18A6ED0`, `sub_F79E50`, `sub_1B89950`.

## Method negative: the ModRM sweep conflates classes

The unread writer `sub_8F0BD0` turns out **not to be a fight class at all**: at 0x008F557E it does `mov [ebx+888h], eax`
with `eax` from `call sub_8EFBE0` and the argument string `"GaussianBloom7_OPT_Linear"` (plus `push 0F78h`), i.e. a
**render/bloom** pass manager.

So `+0x888` is an offset that several unrelated classes share, and "5 writers of +0x888" is only meaningful **within one
class**. The fight identification stands for the object constructed by `sub_8EA050` (vtable `0x1E83A14`); it does not
extend to the render one, and the other three writers (`0x16261DA`, `sub_18A6ED0`, `sub_F79E50`) have not been confirmed
to belong to the same class as the ctor either.

**Corrected scope for the claim:** "`AI+0xA4` = `&(world+0x888)`" needs one more check - that the object at `[this]+0x108`
is the world and not merely another object that happens to be at the same offsets.

## Open ambiguity in the chain itself

`sub_1C60760` does `v2 = *this`, then `*(v2 + 0x108)`. If `*this` is a vptr then `+0x108` is vtable slot 66, which yields
a **function address**, and taking `+4` of that would read code bytes, not data - unlikely. If `*this` is a data pointer,
`+0x108` is a field and the chain is data all the way, which is what the rest of the chain implies. The ctor
`sub_1C7A560` writes `*this = *v5` from a 16-byte allocation, which suggests the latter but does not prove it.

That is the next read and it is cheap: dump the 16 bytes at the allocation and see whether `*this` lands in `.data`
(field) or `.rdata` (vtable).

METHOD: disasm of the writer site, string identification, chain re-read. COVERED: 1 writer, 1 chain. NAMES: `sub_8F0BD0`,
`sub_8EFBE0`, `GaussianBloom7_OPT_Linear`.

### Allocator does not zero - first hop still unproven

`sub_A085B0` (0x00A085B0) is a size-classed allocator (bucket shift, no memset). The 16-byte block in `sub_1C7A560` is
therefore only initialised by the following `sub_4061F0(16, 4, block)`, and `*this` is copied from that block's first
dword. Until `sub_4061F0` is read, "`*this` is a data pointer to the world" is **UNVERIFIED** - it could equally be a
vtable-installing constructor, which would make `+0x108` a vtable slot after all.

Consequence for the record key: if it is a vtable slot, the "key" is a function address (still constant per class, so the
per-manager reading survives), but the `+4` offset in the chain would need explaining.

METHOD: decompiled the allocator, traced the block's only writer to the next call. COVERED: 1 allocator, 1 call.
NAMES: `sub_A085B0`, `sub_4061F0`.

### sub_4061F0 is a second allocator, not a constructor

`sub_4061F0(a1, a2, a3)` (0x004061F0) delegates to the global allocator object at `dword_2AC40F0` (`vt+4` for
sizes <= 0x10, `vt+8` above), retrying via `sub_8197A0(1, a1, 0,0,0)`. No memset, no vtable store.

So neither `sub_A085B0` nor `sub_4061F0` initialises the 16-byte block, and `sub_1C7A560` copies `block[0..3]` straight
into its own object before calling `sub_1C60760`, which immediately dereferences `*this`. The block content is therefore
either pool-zeroed or written by something outside this path - **not established**.

**Deduction (tagged INFERENCE):** because the ctor dereferences `*this` in the same breath, whatever fills that block must
have run before the ctor, or the allocator must hand back zeroed memory. A null `*this` here would fault on
`*(*this+0x108)`, so a *zeroed* block would be a bug - which argues the block is populated elsewhere.

That is the next read: the callers of `sub_1C84870`/`sub_1C848D0` (2 and 1 respectively) and whether any of them write
`*(a2+0x10)` before calling.

METHOD: decompiled the allocator chain to its global. COVERED: 2 allocators. NAMES: `sub_4061F0`, `sub_A085B0`,
`sub_8197A0`, `dword_2AC40F0`.

### The block is created inside this path, so nothing external pre-fills it

Callers: `sub_1CAF300` and `sub_1CAF340` both do `if (!holder[0xC]) sub_1C84870(0);` then use `holder[0xC]`, and
`sub_1CA99E0` is a plain `switch` dispatch that calls `sub_1C848D0()` for case 1. `sub_1CAF300` then writes a 16-byte
OWORD at `+0x20` of the result - not the block.

So the 16-byte block (`holder+0x10`) is allocated *inside* `sub_1C7A560` and reused thereafter; there is no external
pre-fill. That leaves only two possibilities, and they disagree about the first deref:

- the allocator hands back zeroed memory -> `*this == 0` -> `*(*this+0x108)` would fault, so something else must be going
  on; or
- the block retains **stale** content from a previous holder (the block is per-holder, so on first use it is fresh).

The decisive read is `sub_8197A0` / the global allocator's `vt+4` method: if it memsets, the first hop is data-from-zero and
the deref implies an ordering effect we have not seen.

METHOD: callers walked (3), ctor re-read for who writes `holder+0x10`. COVERED: 3 callers. NAMES: `sub_1CAF300`,
`sub_1CAF340`, `sub_1CA99E0`, `sub_8197A0`.

### The allocator global is a dispatcher, not a zeroer - dead end recorded

Correct address first: the global is `0x02AC40F0` (my earlier sweeps used `2C` in the byte pattern instead of `AC` and
returned nothing - a typo, not an absence).

- Installed by `sub_80DFB0` (0x0080DFB0) with `mov dword_2AC40F0, esi`; the stored object's `[4]` is vtable
  `off_1E74B20`, and `[esi+0x10] = esi`.
- `vt+4` of that vtable is `sub_80A0C0`: `v2 = *(*(a2)+4); ++a2[1]; return v2(a1);` - a **forwarding thunk** that
  redispatches into a per-arena object and bumps a counter.

So `sub_4061F0`'s path ends in an arena dispatcher; the zeroing question is a property of the **arena's** `vt+4`, reached
via `*a2`, and the arena objects are built through `sub_80B0C0` from the list at `[esi]`. Not resolvable from the global
alone.

**Verdict for this thread:** the data-vs-vtable question about `*this` needs a live read of the first dword of a freshly
allocated 176-byte object, or the arena creation site. Static work has taken it as far as the dispatcher; further reads are
chasing allocator plumbing rather than the fight system.

METHOD: byte-pattern sweep for the correct address (7 encodings), writer disassembly, vtable slot read, thunk decompile.
COVERED: the only writer, vtable slots 0/1, the thunk. NAMES: `sub_80DFB0`, `sub_80A0C0`, `off_1E74B20`, `dword_2AC40F0`,
`sub_80B0C0`.

## Two negatives: neither sub_1625330 nor sub_161BE20 creates a record

Presumption being tested: "the record fill is in `sub_1625330`" (the doc's own wording). It is false.

- **`sub_1625330`** (0x01625330, 4410 chars decompiled): no allocator call (`sub_A085B0`/`sub_4061F0` absent), no store to
  `record+0x18`, `+0x2C` or `+0x44`. It only *reads* records through `sub_15E50F0` and relates them. So it is a
  relationship step, not a constructor.
- **`sub_161BE20`** (0x0161BE20): allocates via `sub_1602240()` and writes only `+8 = 0` and `+0x0C = handle(a3+0x200)`; its
  one caller is `sub_161D810`. A ~12-byte object with a handle at `+0x0C` - an *event*, not the fight record (which is
  much larger and carries `+0x18`). No store to `+0x18` either.
- **`sub_1626AB0`** (FightManagerUpdate, 8942 chars): calls the selector `sub_1614EE0` but contains no allocator call and no
  `sub_1529850`/`sub_1529830`/`sub_13F22E0`.

So record creation is still unfound: four plausible hosts checked, four negatives. The remaining leads are the record's
size class and `sub_1602240`'s pool, or `sub_1620350`'s status-1 path (which the manager doc ties to the attacker list).

METHOD: presence/absence of allocator names and of `+24`/`+44` stores in three decompiled bodies, plus xref on
`sub_161BE20`. COVERED: 3 functions. NAMES: `sub_A085B0`, `sub_4061F0`, `sub_1602240`, `sub_161D810`, `sub_1620350`.

**Note on my own error:** the earlier `+ 24` counts matched the substring inside `+ 245`. Counts now use `+ 24) =`.

## Record located: it is per-entity at `AI+0x98`

`sub_1620350(this=fm, a2=entity, a3=status)` resolves the record from the **entity**:

```
v4 = *(entity+0xE8); v5 = *(v4+0x98)        // AI component -> the entity's fight data = THE record
v23 = *(v5+0x2C)                            // current status
sub_1529850(a3)                             // thiscall on v5: the status setter, a3==8 means "no change"
switch (a3) { case 1: ... attacker-list work on v5+0x40 ... }
```

So a record is **owned by the entity** at `AI+0x98`, and the manager's `fm+0x780` array holds pointers to such records.
That reconciles the earlier pieces: `sub_15E50F0` scans the array by `rec+0x18`, `sub_1529830` sets `rec+0x180`, and the
record identity is per-entity even though the array is manager-wide.

**This also explains the replica failure cleanly:** if the puppet's `AI+0x98` is null there is no record for the manager to
list, `sub_15E50F0` cannot match, and every status/order path no-ops. `[PoiFO] pupInGroup=0` is the same fact seen from the
manager side.

### Next (precise)

Writers of `[ai+0x98]`. An entity without one has no fight data at all, so this is now the root candidate above faction:
probe `ai+0x98` and the answer is one deref away.

METHOD: decompile of the status path, followed the entity->component->field chain. COVERED: 1 function, full body head.
NAMES: `sub_1620350`, `sub_1529850`, `sub_1625330`.

## +0x98 writer sweep (negative)

Only **3** disp8 stores to `+98` in the image (`89 /r 98`): `0x29041A5`, `0x94F23B`, `0x29A26FD`. Only the middle one has a
function boundary (`sub_94F140`) and it is a **bulk copy of 160-byte elements** - a vector copy, not the record
assignment. The other two sites have no function in this idb.

So the record pointer is not set by an obvious single setter; it is most likely attached where the fight data itself is
built (component init), which is still unfound. Probe now logs `rec98` (and `playerRec98`) so the null/non-null fact is
readable without further sweep.

METHOD: full disp8 ModRM enumeration of `89 /r 98` (3 hits), one body read. COVERED: 3/3 sites, 1 read. NAMES:
`sub_94F140`.

## The status switch, case by case (companion to records-keyed-by-faction)

`sub_1620350` after setting the status switches on it. `[DECOMP]`, full body read (6217 chars):

| case | body | meaning |
|---|---|---|
| 1 | `if (*(rec+0x40)) sub_1557730(rec)` | record has a target -> **attacker-list add** |
| 2 | if `sub_15C65A0(entity) \|\| fm+973` -> mark `+200` and `sub_15FD330`; else build event type **4** (via `sub_1602380`), post on the record's target handle, `sub_1620220`, clear, `sub_15FD330` | leave-fight path |
| 5 | if `sub_15C3E60(fm)` and `sub_15DAD50(fm+772, rec)` -> event type **104** (`sub_FA3590`), handle = fm target's, `v18[6] = entity`, then `sub_5B9CF0`/`sub_649FE0` | repost to a new target |
| 6 | event type **3**, post, `sub_1620220` | drop notice |
| 7 | `sub_1546600(rec)` | per-status action |
| 9 | if `!sub_153D8F0(entity)`: if `rec == fm+772` call `sub_1432710(fm+900)`; then event | target-side reactivation |
| 8 | intercepted earlier: `a3 = *(rec+0x2C)` (re-post, no change) | no-op repost |
| others (3,4,10) | fall through the default arm | not enumerated yet |

Two mechanics worth keeping: the record's `+0x14` handle is what events are posted on (consistent with it being a refcounted
handle), and `fm+772` is the current target **record**, so case 9's comparison is record-to-record.

**Relevance to the replica:** every one of these arms assumes a record exists. With `AI+0x98 == null` there is nothing to
switch on, which is why status transitions would have to precede any damage work.

METHOD: full-body decompile of `sub_1620350`, arms transcribed. COVERED: 7 of 10 arms (1,2,5,6,7,8,9). NAMES: `sub_1557730`,
`sub_1602380`, `sub_64A020`, `sub_1620220`, `sub_15FD330`, `sub_15C3E60`, `sub_15DAD50`, `sub_FA3590`, `sub_1546600`,
`sub_153D8F0`, `sub_1432710`.

### Remaining arms and the attacker list

- **Cases 3, 4, 10:** the `default:` arm is empty - they do no per-case work. The **shared tail** still runs for them:
  `if (a3 != 1) { if (*(rec+0x40)) sub_154D900(rec); }`, then `*(sub_16022E0()+8) = a3` (queue the status) and
  `sub_15E0690(entity)`. Status 1 is deliberately excluded from the `sub_154D900` call - the attacker-list path must not
  re-notify its own target.
- **Case 9 detail (corrects my earlier line):** after the reactivation check it **broadcasts** - walks every group in
  `rec+0x10` and, for each record in each sub-list, posts the event on that record's `+0x14` handle. So case 9 is a
  group-wide notify, not a single post.

`sub_1557730(rec, ent)` is the attacker-list add and is entirely mundane:

```
scan rec+0x4C entries for entry+8 == ent            // dedupe
if absent: v = alloc(12); *(v+8) = ent; sub_154D970(v); grow; *(arr + 4*++count - 4) = v
```

So the attacker list is a vector at `rec+0x4C` (count `u16` at `rec+0x52`) of **12-byte entries carrying the entity at
`+8`**, grown with the same idiom as `fm+0x30C` (cap `&0x3FFF`, x4/3 or +4 via `sub_A09AB0`). That shared idiom is useful
corroboration that both are plain vectors and not something bespoke.

METHOD: default arm read in place, case 9 loop read, full `sub_1557730` decompile. COVERED: 3 arms + 1 function. NAMES:
`sub_154D900`, `sub_16022E0`, `sub_15E0690`, `sub_1557730`, `sub_154D970`, `sub_A09AB0`.

## Negative: the pool is for 16-byte event items, not records

`sub_1602240()` (0x01602240) takes the global lock `dword_2AC1FCC`, allocates **16 bytes** (`sub_418A80(16,4)`),
initialises with `sub_139DEC0`, appends to the global vector `dword_2AC1FBC` (cap `&0x3FFF`, count `word_2AC1FC2`), releases
the lock and returns. Its only callers are `sub_161BE20` (event builder) and `sub_161F420` (FightRemoveFighter).

**So the pool lead is dead for record creation.** Records are much larger than 16 bytes and carry `+0x10`, `+0x14`, `+0x18`.

## Reconciliation: `fm+780` IS `fm+0x30C` - the record list, not a team map

`780` decimal is `0x30C`. The array `sub_15E50F0` scans for records **is** the array `sub_15DABC0` validates against, and
the array `sub_1621670` removes from. So the thing I called the "team vector" is the **fight record list**.

That also explains the earlier `SoundInstance` puzzle properly: the writer I attributed to it (`sub_13FE5E0`) is a vtable
method of a *different* class that happens to share the offset - which is exactly the offset-coincidence already logged as
a method negative. The record list's own fill is still unfound.

**Open, sharpened:** `sub_15DABC0` compares each array entry against `target_entity+0x10`. If array entries are record
pointers, that only works if `entity+0x10` holds the entity's record pointer - a second location beside `AI+0x98`. That is
now the specific thing to test, and it is one deref pair.

METHOD: full body read of the pool, caller list, offset arithmetic on 780/0x30C. COVERED: 1 function, 2 callers, 3 array
consumers. NAMES: `sub_1602240`, `sub_418A80`, `sub_139DEC0`, `sub_161BE20`, `sub_161F420`.

## Record layout confirmed from the remove path, and one pair left ambiguous

`sub_161F420(fm, rec)` (0x0161F420) takes the manager lock at `fm+1644`, reads `v42 = *(rec+0x10)` and guards on it before
any group work. So **`rec+0x10` is the group pointer**, consistent with `sub_161BD30` walking `rec+0x10 -> +0x10` arrays.

What that does **not** settle: `sub_15DABC0` compares each entry of the record array to `*(target_entity+0x10)`. With entries
being record pointers and `rec+0x10` being a group, an *entity's* `+0x10` must be something specific for that compare to be
meaningful. Static reads so far give no writer of `entity+0x10` in this band, and the sweep for `mov [reg+10h], ...` is the
noisiest field in the image.

**Decision: this one is handed to the pair, not to more decompile.** `team=` in the probe already logs `entity+0x10` and
`rec98=` logs the record, so one sample says whether `entity+0x10` equals the record pointer (compare is identity) or is
unrelated (compare is something else and C3 needs a further pass).

METHOD: full body head of the remove path, offset cross-check against `sub_161BD30`. COVERED: 1 function, 1 offset.
NAMES: `sub_161F420`, `sub_15DABC0`.

## `AI+0x98` is a ROUND, not a generic record - named by its setter

`sub_1B996D0` (0x01B996D0) is a setter: `result = *(a2+8); if (result != *(this+0x98)) { *(this+0x98) = result; return sub_FB2530(L"setRound", result); }` -
and it logs the literal **`setRound`**. So the field is the entity's current **fight round**, and every `rec+...` access in
`sub_1620350` / `sub_15E50F0` / `sub_1529830` is a round access.

That is a better word than "record" and it explains the shape: rounds carry a status (`+0x2C`), a target (`+0x40`/`+0x180`), an
attacker list (`+0x4C`), a group (`+0x10`) and a faction key (`+0x18`). A body without a round has no fight at all.

Second writer, different class: `sub_1B6FCB0` copies `v1[0]` into `this+0x98` among `+0x90..+0xA8`, so the same offset is used
by another structure - the offset-coincidence caveat still applies and "round" is scoped to the AI class that `sub_1B996D0`
serves.

**Consequence:** `[PL] rec98=` should be read as "round", and `rec98=0` on the replica means no round was ever created for
it. `setRound` also gives a search hook - any other site calling `sub_FB2530(L"setRound", ...)` is a round change.

METHOD: disp32 `89 /r 98 00 00 00` sweep (399 hits), AI-band filter (18), smallest three decompiled, string in the winner.
COVERED: 3 of 18 band hits, 1 named. NAMES: `sub_1B996D0`, `sub_FB2530`, `sub_1B6FCB0`.

## `setRound` is telemetry, not a creation marker (negative)

The string `setRound` (0x02722650) has exactly **two** references: `sub_1B996D0` (the setter) and `sub_1B994A0`. The second
is a **publish** routine, not a creator:

```
sub_FB2530(L"setRound", *(this+0x98))                  // log the CURRENT value
for i in * (u16*)(this+0x92):                          // 16-byte stride array at this+0x8C
    sub_1B982C0(L"setPlayer", i++, name_or_Default, ...) // log each slot
sub_7FA4A0(this)
```

So neither `setRound` site creates a round - the creator is elsewhere and the string is only a log label. Structure gained
instead: a 16-byte-stride player array at `this+0x8C` (count `u16` at `this+0x92`) beside the round value at `this+0x98`.

**Negative for the approach, not just the site:** a debug-string anchor finds *logging*, and this codebase logs state changes
far more often than it creates things. Round creation stays open.

METHOD: string-byte search (`setRound` UTF-16, 1 hit), xref on the string (2 refs), full read of both. COVERED: 2/2 refs.
NAMES: `sub_1B996D0`, `sub_1B994A0`, `sub_FB2530`, `sub_1B982C0`, `sub_7FA4A0`.

## `sub_160FAD0` is a selector, not a creator (negative)

Body: `sub_A062A0(0)`, then over `*(u16*)(this+786)` entries of `*(this+780)` (the round array) it performs a **binary-search
insert into a sorted 8-byte-pair set** keyed by `sub_153D990(0)`, and returns a chosen element. `sub_161BD30` stores that into
`fm+2248` and only re-orders when it *changes*.

So `fm+2248` is the current selection value and `sub_160FAD0` is a ranking pass over rounds that already exist. No
`A085B0`/`4061F0`/`418A80`, no `sub_15E50F0`, no status/order call.

That is the fifth function checked for round creation and the fifth negative - creation is not in the setter, the two
`setRound` sites, `sub_1625330`, `sub_161BE20`, `sub_1626AB0` or here. It is also not the 16-byte pool.

**Where that points:** if no health/status/selection path allocates a round, the allocator call must sit in whatever spawns
the *combat state* for an entity - i.e. the same place that gives a body its AI fight data. The remaining unread writer of
`+0x888` (`sub_8F0BD0` was render) and `sub_1621670`'s counterpart add are better leads than more status plumbing.

METHOD: full head read, presence checks for six allocator/consumer names. COVERED: 1 function. NAMES: `sub_160FAD0`,
`sub_153D990`, `sub_161BD30`, `fm+2248`.

## The add counterpart is not separable from creation (negative)

Two searches failed to isolate an "add round" function:

1. `inc [x+312h]` in either the `66 FF /0` and `FF /0` (disp32) forms: **none** in the image. The count at `fm+0x312` is
   therefore not incremented through an object-relative store; the vector is passed by pointer and the count written via
   `[ptr+6]`, exactly as `sub_1557730` does for the attacker list.
2. The `lea reg,[x+30Ch]` candidates (`0x660017`, `0x123D426`, `0x1999CA7`, plus `sub_1621670`'s remove) contain the offset
   but none of the sampled windows calls the grow helper `sub_A09AB0`.

**Conclusion:** the add is not a standalone sibling of the remove. It is inside whichever function *creates* the round and
appends the first pointer - i.e. the same unknown creator. So "find the add" collapses into "find creation", and the two
should be tracked as one gap rather than two.

METHOD: count-increment sweep in two encodings (0 hits), presence checks on four `lea` sites. COVERED: 4 sites, 2 encodings.
NAMES: `sub_A09AB0`, `sub_1557730`, `sub_1621670`, `fm+0x312`.

## The round setter has ONE caller, and it sits in an undefined hole (negative)

`sub_1B996D0` (the `setRound` setter) has exactly one xref: `0x1B9C254`. Attempts to find its enclosing function:

- prologue scan over `0x1B9BF00..0x1B9C300`: prologues exist at `0x1B9BF20/70/E0/F0`, `0x1B9C090`, and **after** the site at
  `0x1B9C280`, `0x1B9C2B0` - nothing between `0x1B9C090` and `0x1B9C280` except the site itself;
- the two functions that could span it (`sub_1B9C090`, `sub_1B9C130`) were decompiled and neither contains the call
  (`sub_1B9C090` is a start-loading-message setter, `sub_1B9C130` its per-tick updater, both `showLoadingMessage`);
- `disasm` at `0x1B9C230` returns **no lines** - IDA has no items there at all, i.e. a hole, not a wrong boundary.

So the one caller of the round setter cannot be attributed without defining code in the hole first (rule 2's remedy applied
to a stretch with no prologue at its head). That is the concrete blocker for "who changes a round", separate from "who
creates one".

**Consequence:** `setRound` is called from the *loading-message* subsystem, not obviously from fight code. If that holds once
the hole is defined, the round value is driven from a presentation path and `AI+0x98` may be written only there - which would
make "no round on the replica" a UI/state-publication gap rather than a combat one. Hypothesis, UNVERIFIED.

METHOD: xref, two prologue scans (0x400 + 0x160 windows), three decompiles, one failed disasm. COVERED: 1 caller, 5 candidate
functions. NAMES: `sub_1B996D0`, `sub_1B9C090`, `sub_1B9C130`, `sub_1275300`, `L"showLoadingMessage"`.

## The hole holds a callback table, and `setRound` is registered not called (major)

Disassembling at the failure address `0x1B9C228` (the real head, in a stretch with **no prologue at all**) gives the truth:

```
call sub_A062A0
mov [esi+94h], ebx
mov dword ptr [esi+98h], 1
mov [esi+5Ch], esi ; mov [esi+60h], offset sub_1B99520
mov [esi+68h], esi ; mov [esi+6Ch], offset sub_1B99650
mov [esi+74h], esi ; mov [esi+78h], offset sub_1B996D0
mov [esi+80h], esi ; mov [esi+84h], offset off_1B70F20
mov dword ptr [esi+34h], 1Ch
```

So the xref to `sub_1B996D0` was never a call site - it is a **store of its address into a table**. The structure being built
pairs a self-pointer with a function pointer at `+0x5C/+0x60`, `+0x68/+0x6C`, `+0x74/+0x78`, `+0x80/+0x84`: four callback
slots of a small dispatcher object (the last one is a vtable `off_1B70F20`, not code).

**Corrections this forces:**

1. `sub_1B996D0` is a table entry, invoked later through `+0x78`. Nothing calls it directly - which is why its xref list
   looked like a dead end.
2. In *this* object, `+0x98` is initialised to **1**, not a pointer. So `AI+0x98` being a round pointer is now suspect again:
   either the AI class's `+0x98` is a different field, or this object is not the AI.
3. Decompilation fails at `0x1B9C228` because the head has no prologue (`call` as the first instruction); bound attempts at
   `0x1B9C1D0` and `0x1B9C228` both failed, while `disasm` from `0x1B9C228` succeeds and `define_func` refuses it. Nearby
   `disasm` at `0x1B9C230` had returned nothing earlier only because nothing was defined.

METHOD: `disasm` at the failure address after two failed defines, full 12-instruction window read. COVERED: 1 site, 12
instructions. NAMES: `sub_1B99520`, `sub_1B99650`, `sub_1B996D0`, `sub_A062A0`, `off_1B70F20`.

## Correction: the object is MULTI-BASE, and the function head is `0x1B9C1D6`

Reading back from the table site corrects my previous entry:

```
0x1B9C1C7 retn 4                      <- end of the previous function
0x1B9C1CA..1D5  CC CC ...             <- 12 bytes of padding
0x1B9C1D6 push ebx; push esi; mov esi, ecx
0x1B9C1DC call <init>
0x1B9C1E1 mov dword ptr [esi], off_2722588
0x1B9C1EC mov [esi+58h], off_2721DB0 ; [esi+60h] = sub_1B99520
0x1B9C1F8 mov [esi+64h], off_2721DBC ; [esi+6Ch] = sub_1B99650
0x1B9C204 mov [esi+70h], off_2721DC8 ; [esi+78h] = sub_1B996D0
0x1B9C210 mov [esi+7Ch], off_2721DD4 ; [esi+84h] = ebx
```

So it is a **multi-base object with four sub-vtables** (`0x2722588` at `+0`, then `0x2721DB0/1DBC/1DC8/1DD4` at
`+0x58/0x64/0x70/0x7C`), each paired with a callback. `setRound` sits in the third pair. `[esi+84h] = ebx` and `+0x88 = bl`
follow, then `+0x94`/`+0x98` (=1), then `+0x34 = 0x1C`.

**Corrects my last note in two ways:** the `+0x84` "vtable" was actually `ebx` (the real one is `+0x7C`), and the head is
`0x1B9C1D6` behind 0xC bytes of `CC` padding - which is exactly why a prologue scan and `define_func` both fail there.

**Boundary negative:** `define_func` refuses `0x1B9C1D6` three times (with and without explicit bounds, and after two
attempts at neighbouring addresses), while `disasm` from it works. So the idb has a chunk-level disagreement at this site and
the constructor is readable by disassembly only until that is resolved.

METHOD: byte dump of the gap (found the `CC` pad and the `53 56 8B F1` head), 12-instruction disasm. COVERED: full head,
three define attempts. NAMES: `off_2722588`, `off_2721DB0`, `off_2721DBC`, `off_2721DC8`, `off_2721DD4`, `sub_1B99520`,
`sub_1B99650`, `sub_1B996D0`.

## The four tables are bound-call interface vtables (structure read)

Contents of the four tables the constructor installs:

- entries are tiny accessors in one band (`0x1B71280` size 6, `0x1B71330` size 0x22, `0x1B713C0` size 0x22, ...);
- every pair is followed by the **same trampoline** `sub_17DBEC0` (0x017DBEC0, size 0xE):
  `return (*(this+8))(*(this+4));` - it calls a stored function with a stored context, i.e. a **bound call**.

So the layout is: `[self][fn][table]` repeating, four times (`+0x58/60/6C`, `+0x68/6C/70`, `+0x74/78/7C`, `+0x80/84/8C`-ish),
each triple wiring one interface whose table routes through the shared trampoline. `sub_1B996D0` (`setRound`) is a **direct
override at `+0x78`**, outside the tables - the object is an observable/behaviour base with four delegated interfaces, not a
plain value struct.

**Consequence for `+0x98`:** it lies *past* all four interfaces, in the concrete tail (which also holds `+0x94`, `+0xA0`... and
the round value is written by the `setRound` override against `this`). Whether the AI's `+0x98` is this same concrete field is
still open, but the multi-base shape now makes "different base, same offset" a plausible explanation rather than a coincidence,
and the concrete tail is where the AI-side write would have to land.

METHOD: dumped all four tables, identified the repeated trampoline and its semantics, sizes on three entries. COVERED: 4 tables,
4 entries sized, 1 trampoline decompiled. NAMES: `sub_17DBEC0`, `sub_1B71280`, `sub_1B71330`, `sub_1B713C0`.

## The class is an EVENT type - id getter resolves to `HudBowlsStartGameEvent` (rule 13)

Table entries decompiled: three are **scalar-deleting destructors** (`*this = &off_...; if (a2 & 1) operator delete(this)`,
against `off_27217D8/17E4/1808`), and `sub_1B71280` is a **class-id getter** returning the bare constant `433336762`
(`0x19D431BA`). `known.py` resolves that unambiguously to **`HudBowlsStartGameEvent`**.

So this object is an **event class with four interface bases**, not a value struct - and the `setRound`/`setPlayer` overrides
belong to an event-typed publisher.

### Consequence, stated carefully

If `AI+0x98` were this event type, the field would be "current bowls/minigame round", which sits oddly beside
`sub_1620350` using `*(entity+0xE8)+0x98` as the **fight** record for status/target/attacker-list work. Two readings:

1. `+0x98` on the AI really is a fight round and this event class merely *shares the offset* (the offset-collision reading,
   now the simpler one);
2. the fight code operates on a "round" concept that the event publisher also speaks about, and the AI's field is a different
   object of similar shape.

Reading 1 is favoured by the destructor/id evidence being specific to this class; reading 2 is not excluded. **Open, but the
collision reading has gained real weight** - enough that `rec98` should be described as "AI+0x98, unconfirmed identity" until
one sample compares it to a known record.

METHOD: full table dump, 10 distinct entries, four decompiled, constant converted and resolved with `known.py`. COVERED: 4/10
entries, 1 id. NAMES: `sub_1B71280`, `0x19D431BA`, `HudBowlsStartGameEvent`, `off_27217D8`, `off_27217E4`, `off_2721808`.

## All six remaining entries resolve to the HUD-bowls event family (rule 13)

The six unread entries: four are more class-id getters, two more scalar-deleting destructors. Every constant resolves
cleanly in the dictionary:

| getter | constant | name |
|---|---|---|
| `sub_1B71280` | 0x19D431BA | `HudBowlsStartGameEvent` |
| `sub_1B71290` | 0xE16FDA93 | `HudBowlsUpdatePlayerEvent` |
| `sub_1B712A0` | 0xABBF9D50 | `HudBowlsSetRoundEvent` |
| `sub_1B712B0` | 0x55826CC8 | `HudBowlsEndGameEvent` |
| `sub_1B712C0` | 0xC3F39CF6 | `ShowTutorialMenuEvent` |

Destructors pair with `off_27217D8/17E4/17F0/17FC/1808`.

So the object is the **HUD-bowls (minigame) event publisher**: five event ids plus a tutorial-menu id, each behind a bound-call
interface, with `setRound`/`setPlayer` overrides. Not a fight structure at all.

### Verdict on `+0x98`

This settles the *style* of the answer even though it doesn't name the AI field: the hole object is a transient
event/publisher class, so it is **not** the thing `sub_1620350` walks for status, target and attacker list. The offset
collision reading is now strongly favoured - `+0x98` on the AI is some other object with a round-like role, and "HudBowls"
labelling should not be carried onto it.

`rec98` stays "AI+0x98, unconfirmed identity"; the five-name list above is about the *class in the hole*, not about the AI.

METHOD: six entries decompiled, four constants converted from signed and resolved with `known.py`, five vtable refs recorded.
COVERED: 6/6 remaining entries, 4 ids. NAMES: as tabled, plus `off_27217D8/17E4/17F0/17FC/1808`.

## Caller map of `sub_1620350` (anchoring attempt)

Seven callers, so this is a widely-used notifier, not a private helper:

| caller | context |
|---|---|
| `sub_1396F60` | statechart status-2 source (per `fightmanager.md:158-172`) |
| `sub_1397450` | statechart action that adds to the attacker list (status 1) |
| `sub_1620630` | inside the manager (no-op re-post, status 8) |
| `sub_1625590` | start-fight-between (status 1 cross-call) |
| `sub_173E190` | CSDamage listener (status 9) |
| `sub_18644D0` | hit resolution on the victim (status 9) |
| `sub_1B080A0` | AI-side helper (status 9) |

`sub_1B080A0(this, a2)` was read: it decodes a handle at `*(this+3)+0x5C`, then reads `*(entity+0x5C)` -> world and
`[world+0x8DC]` before `sub_15C56A0(entity)` / `sub_15C6330(entity, 9)`.

**Projected onto the "who owns +0x98" question: no caller pins a class.** Every one hands over a bare entity (or an entity
decoded from a handle), so the field's owner type is still not established by the call graph. The anchor failed for the same
reason the offset did - the API is entity-level.

**One recurring detail worth its own note:** `world+0x8DC` (2268) has now appeared in `sub_1397450`, `sub_1B080A0` and
`sub_F79E50`. It is a world-level slot used repeatedly by status/AI code, and it is a better structural landmark than any
offset we started with.

METHOD: full caller enumeration, one caller decompiled. COVERED: 7 callers listed, 1 read. NAMES: as tabled, `world+0x8DC`.

## `+0x8DC` is a shared offset too - the landmark idea weakens (negative)

15 writers of `89 /r DC 08 00 00`. The fight-band one (`0x186825B`, inside `sub_18680F0`) writes `[esi+8DCh]` where `esi` is
**its own object**, in a cluster `+0x8B0/8D8/8DC/8E0/8E4/8E8/8EC/8F0/8F8` - not the world.

So `+0x8DC` joins `+0x10`, `+0x30C`, `+0x888` and `+0x98` as an offset that several unrelated classes use. Three functions
*reading* it did not make it a landmark; only a per-class base does, and we do not have one for the world.

**Correction to last batch:** "`world+0x8DC` is a better structural landmark" holds only as "a slot the world also has". It is
not a discriminator and must not be used as one.

What survives: in `sub_1B080A0` the base really is `*(entity+0x5C)` before the read, so that function's `+0x8DC` is world-side.
The rendering/writer side is a different object. Both true, neither transferable.

METHOD: full disp32 ModRM sweep (15 sites), one site disassembled and its base register checked against the read site.
COVERED: 1 writer vs 1 reader base. NAMES: `sub_18680F0`, `sub_1B080A0`.

## AIComponent descriptor located by content (rule 13), but it is not addressed by immediate

`crc32("AIComponent")` = `0x33E86CF3`, confirmed in the dictionary. The immediate appears three times, all in `.data`
(`0x27541A8`, `0x27686DC`, `0x2778108`) - descriptor rows, not code. The first is the real one:

```
0x27541A8: f3 6c e8 33 | 5c 91 93 f1 | 10 01 00 00 | 04 00 00 00 | ... | ff 7f ff 7f
           crc=crc32("AIComponent")  hash2=0xF193915C  size=0x110  flags=4
```

Two things worth having:

- **`AIComponent` is `0x110` (272) bytes** - a large component, consistent with `e+0xE8` carrying AI state rather than a
  marker.
- It shares the second hash `0xF193915C` with the `CharacterBehavior` descriptor (`0x2754198`, 16 bytes earlier). The two
  descriptors are adjacent and pair-linked, so "AI is reached through CharacterBehavior" (the `sub_5220D0` path seen earlier)
  is structurally expected rather than accidental.

**Negative:** no immediate anywhere in the image references `0x27541A8` (`A8 41 75 02` and `A8 41 75` both 0 hits), so the
component is not obtained through a constant-addressed getter like `sub_4959E0`/`sub_5220D0`. It must be reached by index or
through the adjacent descriptor - which is the next read.

METHOD: crc computed and resolved with `known.py`, immediate sweep for the crc (3 data hits), full descriptor dump, immediate
sweep for the descriptor address (0 hits). COVERED: 1 descriptor, 4 sweeps. NAMES: `0x33E86CF3`, `0x27541A8`, `0x2754198`,
`0xF193915C`.

## Descriptor rows dumped - irregular cluster, not a uniform table

```
0x2754180: 05 000000 | 09 b1 e8 f9 | 58 41 75 02 | 1c 15 93 02
0x2754190: 06 000000 | 00000000 | 98 2c 75 02 | 00000000
0x27541A0: 00000000 | 10 60 e4 01 | f3 6c e8 33 | 5c 91 93 f1
0x27541B0: 10 01 00 00 | 04 000000 | ... | ff 7f ff 7f
```

Reading it precisely: `0x27541A4` holds `0x01E46010`, the **`"CharacterBehavior"` string**; the `AIComponent` row begins at
`0x27541A8` with its crc first (`0x33E86CF3`), then `0xF193915C`, size `0x110`, flags 4, terminator `0x7FFF`.

**So the rows are not a uniform 0x10 array** - the preceding entries have a different shape (`05`/`06` + a hash + a pointer),
and `AIComponent`'s row has no name pointer of its own, sitting directly on `CharacterBehavior`'s name slot. That is why the
"index by row size" assumption would have been wrong: this is a hand-authored cluster, not a generated table.

**Negative:** with no uniform stride, there is no single index multiply to look for, so "who indexes the descriptor table" must
be answered by finding the base pointer of the whole cluster instead. Not done this batch.

METHOD: 0x80-byte dump parsed into rows, string pointer identified by deref of a known good address. COVERED: 8 rows.
NAMES: `0x27541A8`, `0x27541A4`, `0x1E46010`, `0x2754198`.

## Verdict: the descriptor cluster is per-class static data, not a lookup table

Dumping 0xA0 bytes before the pair shows a **run of 4-byte pointers** into `0x27562xx..0x27564xx` (`0x27540E8..0x2754138`),
then zeros, then a fresh crc-first row at `0x275415C`. So the layout is: pointer arrays, padding, descriptor rows - no header,
no count, no stride.

Reference counts for four sampled rows: `0x2754160` **0**, `0x2754180` **0**, `0x2754198` **1**, `0x27541B0` **0**.

**So descriptors are mostly not addressed by immediate at all.** Combined with the earlier zero for `AIComponent`'s row, this
means the descriptor route cannot answer "who obtains AIComponent" - it only tells us what the class *is* (name, size 0x110,
flags, hash link to CharacterBehavior).

**Thread closed as a dead end**, deliberately, rather than another three batches of pointer archaeology. What the descriptor
work bought is real and stays: `AIComponent` identity, its size, and the structural fact that it pairs with
CharacterBehavior.

### Practical route from here

`e+0xE8` is written by something with a *typed* base; 254 writers is too many, but the component's own **vtable** read from a
live `ai` (one pointer deref of the value the probe already has) would name the class instantly - that is a live read, not
static. Static alternative: pick writers that pass the entity as a parameter and cross-check their callers for a class-typed
argument, which is what failed for `sub_1620350` and will likely fail again.

METHOD: 0xA0 dump parsed, four xref counts taken. COVERED: 4 rows, 0xA0 region. NAMES: `0x275415C`, `0x27540E8`.

## Creation hunt, next pass - still negative, plus a process error found

Re-examined the six `lea [x+30Ch]` candidates by decompiling their **whole** functions instead of 10-instruction windows
(previous pass only sampled windows and had also read the wrong response field, `decompilation` instead of `code`, which
silently produced empty bodies for all six).

| function | len | calls `sub_A09AB0` | touches `+0x30C` | touches count |
|---|---|---|---|---|
| `sub_65FED0` | 3674 | no | yes | yes |
| `sub_123C410` | 14143 | no | yes | no |
| `sub_1999B80` | 1396 | no | yes | no |
| `sub_13E8D90` | 176 | no | no | +0x314 read only |
| `sub_11F8B10` | 316 | no | no | no |
| `sub_64CF70` | 5511 | no | yes | yes |

`sub_65FED0` read in full: it is a **copy-construction helper** - `sub_A38120(off_2786100, a2)` to allocate from a class
descriptor, then a block of float field copies. Not a creator.

**So touching `+0x30C` is not the same as growing it**, and none of the six is the creation site. That is the sixth creation
negative and it narrows the shape usefully: the creator is probably *not* keyed on the array at all, but on the entity (an
attach/add-component path), with the array append as a side effect.

**Process error to keep:** `ida_decompile` returns the body in `code`; reading `decompilation` yields silent empty strings, and
I lost a pass to it.

METHOD: six decompiles at function granularity, one read in full, column checks for the grow call and both offsets.
COVERED: 6/6 candidate functions. NAMES: as tabled, `sub_A38120`, `off_2786100`.

## The two boundary-less "+0x98 writers" are DATA, not code (negative)

Both sites disassemble as `dd offset` tables:

- `0x29A26F0`: a handler table of `(function, eventId)` pairs (`sub_15AA1D0`/`dword_4E06AB0`, `sub_15B06F0`/`dword_4E06AB0`,
  ... ) in the same shape as the table at `0x28CE2A0`;
- `0x29041A0`: a descriptor-ish table (`unk_2000001`, `unk_3832932`, ...).

So the disp32 pattern `89 /r 98 00 00 00` produced **two data false positives**, and `+0x98` in code has exactly **one writer**:
`sub_1B996D0` (the `setRound` setter). That closes "who writes `+0x98`" - there is no other writer to find, and earlier batches
chasing "writer sites" were partly chasing table bytes.

**What `setRound` actually does with it, restated exactly:** `*(this+0x98) = *(a2+8)` - it copies one dword from a *source struct*
`a2`, not a pointer it creates. So `+0x98` holds a copied value/id, and whatever owns `a2` owns the real object.

**Consequence for the replica:** a null `+0x98` means `setRound` was never called *and* nothing else can set it. It is a
one-writer field, so the fix is "call the setter", not "find the missing writer" - and it can be done from a probe with a value
borrowed from the player's own `+0x98` if the provenance question is ever settled.

METHOD: dis7assembly of both boundary-less sites, table shapes identified. COVERED: 2/2 remaining writer sites. NAMES:
`0x29A26F0`, `0x29041A0`, `sub_15AA1D0`, `sub_15B06F0`, `dword_4E06AB0`.

## The setter's caller is virtual-only (boundary, not a lead)

Two searches to find who invokes `setRound` (`sub_1B996D0`) through its slot:

1. `call [reg+78h]` in seven register forms (`FF 5?78`): **0 hits** - nothing calls the slot directly;
2. `mov ecx/edx/..., [reg+78h]` in thirteen ModRM forms: **266 hits** - `+0x78` is yet another shared offset.

So the invocation goes through the bound-call trampoline (`sub_17DBEC0`: `(*(this+8))(*(this+4))`) from a base computed at
run time, and no pattern isolates it statically.

**This is a genuine boundary, stated as one:** "who calls `setRound`" is not answerable by pattern in this binary. It is
answerable by reading a live `+0x78` value - one deref of an object the probe can already reach - and then finding that
address's callers, which *is* static once the address is known.

**Where that leaves `+0x98`:** one writer, virtual-only caller, value copied from `a2+8`. Three facts, no static route to the
fourth. The probe logs `rec98`; the missing datum is the object behind `a2` and the slot's live value.

METHOD: two pattern families, 20 encodings total, hit counts taken. COVERED: `call` form 0/7, `mov` form 266/13. NAMES: none
new; `sub_17DBEC0` and `sub_1B996D0` already recorded.

## A SECOND AIComponent descriptor exists, with a different size (0xE0 vs 0x110)

`0x27686CC` is a descriptor row carrying the same crc at `+0x10`, a **name pointer chain** at `+0` (`0x2766EF8` -> `0x2766EC0`
-> ... , three hops, not yet resolved), ctor `0x01E4EF84`, hash pair `0xCB0F23F4`/`0x33E86CF3`, size **`0xE0`** (224),
terminator `0x7FFF`.

So there are two rows naming `AIComponent` with **different sizes**: `0x110` at `0x27541A8` and `0xE0` at `0x27686CC`. Layouts
differ too (crc-first vs name-first), and the earlier batch's "irregular cluster" note understates it - this is at least two
different row formats for the same class.

**Negative:** `xrefs_to 0x27686CC` is **empty**, so this copy is not referenced either. Two descriptors, two formats, two sizes,
zero references: the descriptors do not identify a single canonical layout, and the size to trust is unsettled.

**What this corrects:** "AIComponent is 0x110 bytes" was stated as a fact from one row; with a 0xE0 row for the same crc, it
should read "0x110 or 0xE0 depending on which row, unresolved". Neither the size nor the layout is safe to code against yet.

METHOD: dump of both rows, name pointer chased two hops, xref on the second row. COVERED: 2 rows. NAMES: `0x27686CC`,
`0x2766EF8`, `0x2766EC0`, `0x27541A8`.

## Name chain resolved by hash - the class hierarchy is explicit (rule 13)

The `0x2766EF8` pointer chain leads to a struct (`0x2766EC0`) whose `+4` is a hash, not a string. Resolving the hashes in
both descriptor rows:

| hash | name | where |
|---|---|---|
| `0x9C197177` | **`AI_Execution_Priority_0`** | name-chain struct `+4` |
| `0xCB0F23F4` | **`Component`** | second descriptor `+0x0C` |
| `0xF193915C` | **`CharacterBehavior`** | paired hash in **both** rows |

So the picture is now explicit and consistent:

- `AIComponent` **derives from `Component`** (base hash present at `+0x0C` of the second row);
- it carries **execution priority 0** (`AI_Execution_Priority_0`);
- and both descriptors pair it with `CharacterBehavior`, so the AI?CharacterBehavior link is confirmed from two independent
  rows rather than one.

That retroactively settles the `sub_5220D0` -> CharacterBehavior -> `+0x1C` path used all night: AI is reached through
CharacterBehavior by design.

**Still unresolved:** the size discrepancy (`0x110` vs `0xE0`) and the zero references on both rows. A base/derived split would
explain different sizes, and `Component` being the base now makes that plausible - but nothing yet proves which row is which.

METHOD: pointer chain chased, three hashes converted and resolved with `known.py`. COVERED: 2 rows, 3 hashes. NAMES:
`AI_Execution_Priority_0`, `Component`, `CharacterBehavior`, `0x2766EC0`.

## No static reference to the AIComponent class-info either (negative)

`0x2766EC0` (the class-info struct) is pointed at only by its own descriptor's `+0` field (`0x2766EF8`); a search for the
literal `C0 6E 76 02` returns exactly that one self-reference and nothing in code. `0x2766F04` inside the struct points at
`0x27686C8` (descriptor `-4`), so the struct is internally coherent and externally unreferenced.

**Conclusion: neither descriptor nor class-info is addressed from code at all.** The class is therefore resolved at runtime,
not by literal - and the unit already in the spine for exactly that is `ComponentClassId` (`0x00A1D0D0`), which maps a class
pointer to its id.

So the remaining static routes are: read `ComponentClassId`'s callers for the AI case, or accept it as runtime and take the
component vtable from a live `ai`. Both are ordinary; neither is a sweep.

**This closes the descriptor/class-info thread a second time, now with the reason stated** - the earlier closure said "no
references" without explaining that the class-info struct itself is unreferenced too, which is the actual, structural reason
literals do not exist here.

METHOD: four immediate searches (descriptor addresses and class-info address), internal pointer check. COVERED: 4 addresses.
NAMES: `0x2766EC0`, `0x2766EF8`, `0x2766F04`, `0x00A1D0D0`.

## Negative and a consolidation: `ComponentClassId` is generic (100 callers)

`0x00A1D0D0` has **100 code callers** - it is a general helper, not an AI-specific accessor, so it cannot type `e+0xE8` on its
own. Refuted as a shortcut before anyone spends a batch on it.

## State of the chain (for whoever picks this up)

End-to-end, with status:

```
event post -> receiver vt[+0x24] (0x64A080) -> resolve listener (0x5220E0)  [VERIFIED: listener present on replicas]
  -> drop if no listener                                                   [NOT the stop]
entity -> e+0xE8 AIComponent -> +0x1C AI -> ai+0x98 ROUND                 [ai reachable; +0x98 null on replicas]
  -> round+0x2C STATUS -> switch arm -> +0x180 target, +0x4C attacker list [switch fully mapped]
  -> +0x180 != 7 guard, sub_1529830 sets                                 [mapped]
fresh record -> sub_161BD30 order -> ground stage -> damage                [mapped; needs a round to exist]
```

Open items and the cheapest resolution for each:

1. **`ai+0x98` is null on the replica** (round absent). Read `[PL] rec98=`. Non-null kills this root.
2. **One writer only** (`sub_1B996D0`, copies `a2+8`); caller virtual-only. Read `+0x78` on a live object, then find that
   address's callers (static again once known).
3. **`entity+0x10`** compared against the round array by `sub_15DABC0`. Read `[PL] team=` and compare with `rec98`; equal means
   identity of the same object, unrelated means the compare is something else.
4. **Descriptor size** `0x110` vs `0xE0` unresolved; base/derived guesses are unproven.
5. **Round creation** still unfound after six function-level negatives and one pool negative. Creator is probably entity-side
   attach, not array code.

**Pre-registered pass criterion for all of the above:** PASS = `rec98` non-null on the replica AND `[PoiFO] pupInGroup` > 0;
FAIL = null `rec98` with `pupInGroup` 0 (round absent confirmed, creation is the whole bug).

### Two warnings that cost batches tonight

- **Offsets collide.** `+0x10`, `+0x78`, `+0x30C`, `+0x888`, `+0x98`, `+0x8DC` are all shared across unrelated classes. Always
  check the base register; never label from an offset alone.
- **`ida_decompile` returns the body in `code`.** Reading `decompilation` gives silent empty strings.

## ROUND CREATION FOUND - full chain, with every gate (closes six negatives)

Chasing the falsification of "`+0x180` is the target byte" led into the creator. The whole path, in order:

```
sub_1620140(entity a2, other a3, flag a4)          <- attack API, 9 sites in 8 functions
  v9 = *(*(a3+0xE8)+0x98)+0x10                     <- the OTHER party's round's GROUP
  if (v9 && !sub_1529D00(a2))                       <- not already in this group
    sub_161D810(this_group, v9, a2)
      if (!a3 || !a3[0xE8] || !round || !*(round+0x10))                     <- absent OR groupless
        sub_161BE20(this, group, entity)
          v10 = sub_156D040(group, entity)          <- CREATOR
          ... then sub_161BD30(this)                <- the order/stage function
```

`sub_156D040(this=group, a2=entity)`:

1. if `a2[58]` (`entity+0xE8`, the AIComponent) is null -> return null;
2. `result = *(AIComponent+152)` (`+0x98`); if `result[4] == this` (the round already belongs to THIS group) it falls through to
   `return result` - an existing round of this group short-circuits;
3. `v9 = *(*(a2+0x5C) + 2268)` (`+0x8DC`); **if null, return null** - so a missing controller at `entity+0x5C -> +0x8DC`
   kills creation outright;
4. `sub_156CDC0(round, controller)` is called later regardless.

Allocation and construction: `sub_A085B0(0x3B0, 16, dword_4DD5EE0)` -> `sub_4061F0(944, 16, ...)` -> `sub_1561050(v11, group,
entity)`. **Record size is 0x3B0 (944) - a hard allocation constant.**

`sub_1561050` installs `off_26D3E70` at `+0` and writes `*(round+0x10) = group` (single site, `0x1561385`), then zeroes a
large field block: `+0x94`, `+0x98`, `+0xA0`-`+0xB0`, `+0x180 = 1`, and if `faction (+0x18) == 7` it runs `sub_15E5B70` /
`sub_15DA7B0` and clears `+0x180` to 0.

**Append - the helper hunted for six batches - is `sub_154DB60`:** capacity `*(+0x0C) & 0x3FFF`, count `u16 at +0x0E`, grow
via `sub_A09AB0((int*)(this+8), ...)`, then `*(ptr + 4*count - 4) = element`. It also recomputes a max over `element+652`.

### Three corrections this forces

1. **`+0x180` is a FLAG (0/1), not a target handle.** All 17 callers of the setter `sub_1529830` pass `1`; the two clear sites
   in `sub_1561050` pass `0`; both read sites in the band are `cmp byte ptr [reg+180h], 0` (`0x15C57BE`, `0x1562EB2`).
2. **The `!= 7` sentinel belongs to `+0x18` (faction), not `+0x180`.** `sub_1529830` is a guarded store:
   `if (*(this+24) != 7) *(this+384) = a2;`. Fourteen sites in the band compare `[reg+18h]` to 7. So faction gates the flag -
   a faction-7 (neutral, unordered) record can never be armed.
3. **"`+0x98` has exactly one writer" was family-limited.** `0x98` fits in a disp8, so the disp32-only sweep caught the rare
   form. Full sweep (disp8 `0x40`-`0x7F` + disp32 `0x80`-`0xBF`, byte/dword ops, verified by disassembling each hit and keeping
   only real `[reg+98h]` operands) gives **57 writers in the 0x15-0x165 band alone**. And `rec+0x98` is a *member* zeroed by the
   ctor - a different field from `ai+0x98` (the round pointer).

**Process error to keep:** a 3-byte disp8 pattern also matches stack slots (`89 45 98` = `mov [ebp-68h], eax`); 47 of 47 raw
hits were stack. Always disassemble the hit and filter on the base register, and always cover the disp8 family.

### Consequence for the puppet bug

Creation happens **on the attacker**, when it engages a target whose round already has a group. It is not a global tick. So
"guard ignores the puppet" and "puppet has no round" are the same fact viewed twice, and the cheapest discriminator is
`entity+0x5C -> +0x8DC` (controller) plus faction at `+0x18` - both readable in one probe pass.

METHOD: guard-by-guard decompile from the falsified `+0x180` claim through 7 functions to the allocator; two full-modrm
writer sweeps with per-hit disassembly. COVERED: 7 functions, 3 offsets, both encoding families. NAMES: `sub_1620140`,
`sub_161D810`, `sub_161BE20`, `sub_156D040`, `sub_1561050`, `sub_154DB60`, `sub_156CDC0`, `sub_1529D00`, `sub_A085B0`,
`sub_4061F0`, `off_26D3E70`, `dword_4DD5EE0`.

**Arg-convention note (decompiler drops `this` on calls):** read from disasm, not the decompile -
`mov ecx,[ebp+var_4]` / `push edi` / `call sub_154DB60` at `0x156D0FE` means `sub_154DB60(group, round)`, and
`push ebx` / `mov ecx,edi` / `call sub_156CDC0` at `0x156D107` means `sub_156CDC0(round, controller)`. `var_4` is the saved ecx
(`mov [ebp+var_4], ebx` at `0x156D04B`), and `ebx` is `*(*(entity+0x5C)+0x8DC)` (`0x156D0A5`-`0x156D0B4`).

## Conflict layer: teams are keyed by a faction object, and both keys must be non-null

Following the creator upward. `sub_1625330(fm, attacker, other, flags)` is the **start-conflict** API (16 call sites, one of
them `sub_1625590`, the enlist path that also calls `sub_1620350(attacker, 1)`).

Its body is nested four deep, and the whole thing is a no-op unless **both** sides resolve to a Team **with a non-null
`team+0x18`**:

```
if (a2) { v5 = *(a2+0xE8); if (!v5 || !*(v5+0x98)) {          <- attacker must have NO round
  ...
  v25 = sub_1622990(a2, a3);                                  <- team create/merge
  v13 = sub_15E50F0(a2); v14 = sub_15E50F0(a3);
  if (v13) if (v14) { v21 = *(v13+0x18); if (v21) if (*(v14+0x18)) {   <- BOTH faction keys non-null
      for each team in fm+0x30C: if hostile(sub_19017C0(v21, t+0x14)) -> sub_15C5700(v13, t)
      ...
```

`sub_15E50F0(entity)` is a plain lookup: `key = sub_133A4C0(entity)`, then linear scan of `fm+0x30C` returning the team whose
`+0x18` equals `key`. So **`fm+0x30C` is a Team list keyed by faction**, which settles the earlier naming - and `team+0x18` is
the faction object, not a "side".

Teams: `sub_1622990` allocates 0x148 bytes (`sub_A085B0(0x148, 8, dword_4DD5EE0)` + `sub_4061F0(328, 8, ...)`), constructs with
`sub_1561DC0(*(fm+2)+2184, fm)`, and **merges** when the two faction keys are equal (`sub_1621670(team, 0)`, return 0). So all
guards of one faction collapse into one team; the puppet gets its own only if its key differs.

`sub_133A4C0(entity)` returns the key either as `ai+0xA4` (only when the ordering test `sub_F916A0` on the AI holds) or as
the resolved owner handle from `sub_11DDD60`. **Correction:** the object from `sub_6F5F70` is the AI (`AIC+0x1C`), not a nav agent -
the field is `ai+0xA4`.

**Consequence for the puppet - the sharpest gate found so far.** An entity that cannot produce a non-null faction key fails
silently at the fourth nesting level: no hostile enlist, no conflict, no round, no fight. Nothing logs it. Two values decide
it - `sub_133A4C0(puppet)` and `sub_133A4C0(playerGuard)` - and both are pure local reads over the replica's own components.

### Dead end recorded: the request-queue poster

`fm+0x170` (pointer) / `fm+0x176` (u16 count) is a **deferred request queue** consumed by `sub_1626AB0` case 2 (type 2 ->
`sub_1620140`, type 1 -> `sub_1625330`, 3/4 -> remove). Tried to find the poster by: writers of `+0x176` (0 hits), the vector
append helper `sub_154DB60` (only ONE caller - the round creator), type stores `C7 4? 14 02` (38 candidates, none touching
`+0x374`/`+0x368`), and state stores `C7 4? 08 01` intersected with type stores (**0 intersection**). Reading `[reg+0x170]`
gives **226 functions**, because `+0x170` is a common offset. Conclusion: the entry objects are not filled by any one obvious
helper, and the poster cannot be isolated by offset alone. It is a legitimate boundary, not a wall - the API is entered
directly from 8 gameplay functions anyway, so the queue is an optimisation, not the only door.

METHOD: 6 decompiles (both APIs, the team maker, the key getter) plus 4 offset sweeps with hit counts. COVERED: 16 + 9 call
sites enumerated; poster not identified. NAMES: `sub_1625330`, `sub_1625590`, `sub_1622990`, `sub_15E50F0`, `sub_133A4C0`,
`sub_F916A0`, `sub_11DDD60`, `sub_1621670`, `sub_1561DC0`, `sub_1620350`.

## The field behind fm+0x30C is a faction key, set once, from the first record

`fm+0x30C` is a vector of **Team\***: pointer `+0x30C`, capacity `+0x310` (`& 0x3FFF`), count `u16 +0x312`. It is appended by
`sub_59A8D0(fm+0x30C, team, 0)` inside `sub_1622990` (dis `0x1622A5F`: `lea ecx,[esi+30Ch]`). Layout confirmed a second time
from the *generic* vector helpers: `sub_16D9D80(team)` appends to the team's OWN record vector (`+0x10` ptr, `+0x14` cap,
`+0x16` count), `sub_59A8D0`/`sub_154DB60`/`sub_15C5700` are all the same grow-and-append with the head passed in.

**`team+0x18` is a pointer to a faction object, and it is written exactly once:**

```
0x156D08B  cmp  word ptr [ebx+0Eh], 0     ; team's record count
0x156D090  jnz  skip
0x156D092  push esi                        ; entity
0x156D093  call sub_133A4C0                ; -> its faction object
0x156D09B  mov  [ebx+18h], eax             ; captured from the FIRST record
0x156D09E  or   byte ptr [edi+104h], 1     ; AI gets +0x104 |= 1
```

So a Team *is* one faction's aggregate: the first member stamps it, everyone else joins by matching `sub_15E50F0` (linear scan of
`fm+0x30C` for `team+0x18 == key`).

Hostility is **directional and stored on the faction object**: `sub_17114C0(a, b)` = `key(a) && key(b) && sub_19017C0(key(b), key(a)+0x14)`,
where `sub_19017C0(key, listHead)` resolves `key` and asks `sub_15DFF10(listHead, &resolved)` to scan that faction's list at `+0x14`
for it. So `faction+0x14` = the factions it considers hostile.

### Three independent silent gates on the same value

`sub_133A4C0(entity)` returning 0 is not one bug, it is three, and every one of them fails **without a log or an error**:

1. `sub_17114C0(a1, a2)` -> `v2 && v3` false, so the guard's target filter (`sub_13B8010` : `v20 && v20 != v45 &&
   !sub_133A6B0(v20,1) && sub_17114C0(a2, v20)`) never lets the puppet through;
2. `sub_1625330` -> the fourth-nesting `if (*(v14+0x18))` fails, so no round is ever created even if the call is made;
3. `sub_15E50F0` -> returns 0, so the entity never joins a Team, so `fm+0x30C` never contains it.

**This supersedes "the guard ignores the puppet" as a single-cause story.** One null - `sub_133A4C0` - produces all three symptoms,
and `sub_133A4C0` is a purely local read (`navAgent+0xA4` or the owner handle from `sub_11DDD60`), so it is fully testable
offline against the replica's own components.

METHOD: writer hunt by intersecting the key getter's 48 call sites with team+0x18 stores, then disassembly of the winning site;
vector heads proved from three independent generic helpers. COVERED: 3 gates, 1 writer, 3 vector appenders. NAMES: `sub_133A4C0`,
`sub_17114C0`, `sub_19017C0`, `sub_15DFF10`, `sub_15E50F0`, `sub_59A8D0`, `sub_16D9D80`, `sub_13B8010`, `sub_133A6B0`,
`sub_11DDD60`.

## Tail of `sub_1625330` read: the `+0xF0` target-flag family (completes the function)

All five remaining helpers key off `fm+0x304` (`this+193`) - the current target **record** - and a per-record byte at `+0xF0`:

| helper | behaviour |
|---|---|
| `sub_15C5D20(fm)` | `fm+0x304 && sub_1529CE0(rec)` - is there a live target record |
| `sub_15D0830(team)` | true if `team+0xF0` or ANY record of `team+0x10` has `+0xF0` - "is this team flagged as a target" |
| `sub_15DAA00(fm)` | clears `+0xF0` on the target record and on every record of its team (bulk clear) |
| `sub_1607F20(fm)` | snapshots `sub_15F0390(rec+0x10)` - every record handle of the target's LINKED teams - then re-derives the fm target |
| `sub_15DB9F0(target, other)` | walks the target's team records to seed the reverse linkage |

So `rec+0xF0` is a "part of the current fight" marker, set/cleared in bulk by team, and `sub_1625330` uses it to decide whether the
incoming attacker changes the target at all. Combined with the earlier reading, the whole function is now covered end to end.

METHOD: 5 decompiles. COVERED: 5/5. NAMES: `sub_15C5D20`, `sub_15D0830`, `sub_15DAA00`, `sub_1607F20`, `sub_15DB9F0`,
`sub_1529CE0`, `sub_15F0390`.

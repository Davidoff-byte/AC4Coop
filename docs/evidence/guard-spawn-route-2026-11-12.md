# Guard-shaped spawn: the guard's own builder + selector (O12) - 2026-11-12

```
BINARY : AC4BFSP.exe 45,056,040 B, sha256 bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150, image base 0x400000
         (sha256 re-read from the installed exe and from the idb's stored input hash: identical)
TOOL   : IDA 9.3 idalib, on a scratch COPY of bf-coop/ref/ida/AC4BFSP.exe.i64 (131,645 functions), opened with
         run_auto_analysis=False and closed with save=False. The tracked .i64 was not opened or modified.
BUILD  : branch re/guard-spawn-probe (worktree ref/AC4Coop-guardspawn), base cadc0a5. MSBuild Release x86 v143: 0 warnings,
         bin/AC4Coop.asi 486,912 B, sha256 E012AC0C...C475 from the uncommitted tree (stamp tree=dirty). Rebuilding
         at the commit changes the embedded stamp and so the hash; record the [Build] line of the run, not this one.
         py tools\spine_gate.py -> VERDICT: OK. py tools\verify_signatures.py -> ORACLE: PASS.
LIVE   : one run, 2026-11-13 pid 7792, ASI a3de755 (= e9c5e75 + probe) sha256 CD4E6808...D352 - see section 8.
```

## 0. Answer

**Primary lead: yes, statically.** A guard spawned from a `SpawnEntityParams`-family record is built by the
same `EntityBuilder::BuildEntityWithSelector` the puppet uses, with a `RowSelector` that **carries its own
EntityBuilder handle**. A guard type is therefore a (builder, selector) pair that the engine keeps in the spawn
record. The puppet route (Begin / Duplicate / End / Build / AddEntity) works on any such pair, not only the
player's. Two pieces are verified only statically:

- **Not yet established:** whether a *live guard entity* links back to that pair. No back-link was found (section 4, U1).
- **Unverified live:** that ambient guards actually come through this path, and that a clone built this way
  survives AddEntity/RemoveEntity.

The probe in section 5 exists to settle both. It captures the pair at build time instead of reading it back
from the entity.

## 1. The build call inside the spawn record (verified in IDA)

`SpawnEntityParams` vtable `0x01E43BB0` (class-info-map.tsv: desc `0x0274DDE0`, id 1111260158). Slot 31 (`+0x7C`) is
`sub_46D8E0` (size 120). Its disassembly, quoted:

```
0046D8E7  mov     esi, ecx                 ; esi = params
0046D8E9  mov     edx, [esi+4Ch]           ; handle container
0046D8F1  mov     ecx, [edx+8]
0046D8F5  mov     edi, [edx]
0046D8F7  sar     ecx, 1Fh
0046D8FA  and     edi, ecx                 ; builder = hc[0] & (hc[2] >> 31)
0046D917  mov     eax, [edi]
0046D919  mov     eax, [eax+44h]           ; builder vtable slot 17 = BuildEntityWithSelector
0046D91C  lea     ecx, [esi+3Ch]           ; selector = params + 0x3C (embedded, by value)
0046D91F  push    ecx
0046D920  lea     edx, [ebp+var_4]         ; &entity
0046D923  push    edx
0046D924  mov     ecx, edi
0046D926  call    eax
0046D94D  mov     eax, [ebp+var_4]         ; returns the entity
0046D955  retn    4
```

`EntityBuilder` vtable `0x01E57790` slot 17 is `0x005FAC00`. `xrefs_to(0x5FAC00)` returns exactly one reference,
the data reference at `0x1E577D4` = `0x1E57790 + 0x44`. So the vtable is the only way in, and `sub_46D8E0`
calls it.

Who reaches `sub_46D8E0`. Verified with `xrefs_to`, and each vtable resolved against `class-info-map.tsv`:

| caller | reached as | class |
|---|---|---|
| data `0x1E43C2C` | slot 31 of `0x1E43BB0` | `SpawnEntityParams` |
| `sub_1898E20` | slot 31 of `0x26FD4F0` **and** of `0x26FD610` (vtable dump, slot 31 = `01898E20` in both) | `SpawnCharacterParams`, `SpawnNPCParams` |
| `sub_1128740` | slot 31 of `0x26976F0` (data ref `0x269776C`) | `SpawnObjectParams` |
| `sub_1996AB0` | slot 31 of `0x270A538` (data ref `0x270A5B4`) | `SpawnShipParams` |

So `SpawnNPCParams` and `SpawnCharacterParams` build through `sub_1898E20 -> sub_46D8E0 -> builder->vt[0x44]`.
The earlier note `spawn-params-lead.md` says "params are data only". That holds for the *serialized* fields.
Its slot table (§10 UNPROVEN 3) left slot 31 / `0x1898E20` unread, and that slot is the build call.

## 2. RowSelector carries the builder handle (verified in IDA)

- `off_27AF834 -> 0x27B0BD0`: class-info whose name pointer reads `"RowSelector"`, size `+0x18 = 0x4C`.
- `RowSelector::Duplicate` `0x7A3BD0` (decompiled): allocates from that class-info (`sub_A38120(off_27AF834, a2)`), copies
  `+0x0/+0x4`, the array at `+0x8` (u16 count `+0xE`), **`sub_A2B760(v4+4, this+4, 0)` = the dword at `+0x10`**,
  the array at `+0x14` (u16 count `+0x1A`) and the member at `+0x1C`.
- `sub_A2B760` with `a3 = 0` (decompiled): `_InterlockedExchangeAdd(*a2 + 4, 1)` (adds a soft ref to the source
  container), exchanges it into `*a1`, and releases the old one. That is a handle-container copy.
- `0x7A3C90` (selector member teardown) releases `*(this+16)`. It decrements `+4`, and when the count reaches 0
  and `(+8 & 0xFFFFFF) == 0` it calls `sub_A094D0(byte_4DD5FA0, hc)`. That is the same release pattern the
  engine uses for the builder container in `0x12F9C90`.
- `sub_46D8E0` dereferences `params+0x4C` as the builder. With the selector embedded at `params+0x3C`, that is
  `selector+0x10`.
- PS3 DWARF, a lead and not proof: `AnyMemberFunctionCast<void (scimitar::RowSelector::*)(const scimitar::Handle<scimitar::EntityBuilder>&)>`
  and `AnyMemberFunctionCast<void (scimitar::SpawnEntityParams::*)(scimitar::RowSelector*)>`
  (`bf-coop/logs/dwarf/entity_core_funcs.txt`, `stream_save_funcs.txt`).

- **The engine itself checks it.** `0x5FD560` (EntityBuilder vtable `+0x38`, the per-phase init that `0x79BC50` calls
  for phases 0/2/4/5) opens with `if ((**(sel+16) & (*(*(sel+16)+8) >> 31)) == this && ...)`. In other words: "is the
  builder behind the selector's `+0x10` handle me?". It then walks the selector's `+0x14` handle array (count `+0x1A`)
  and calls each object's `+0x38`.

So `RowSelector+0x10` is a refcounted `HandleContainer*` for the `EntityBuilder`, and a duplicate holds its own
soft reference. This is **UNVERIFIED live**. The probe logs `selHcIsBuilder=` on every first-seen build: whether
`HandleDeref(sel+0x10)` equals the builder `this` the engine called.

`SpawnEntityParams` layout, as far as it is used here: embedded `RowSelector` at `+0x3C..+0x88`, source selector
pointer `+0x14`, a handle container at `+0x88` (filled from `+0x10` or resolved in slot 32; its type is not
established), and a flags byte at `+0x94`. The "prepare" slot 32 `sub_46D6F0` duplicates
`*(params+0x14)` into `params+0x3C` (`0x46D89C mov esi,[esi+14h] ... 0x46D8B2 push 0 / push edx(=params+0x3C) / mov ecx,esi /
call sub_7A3BD0`) inside `Begin(1,0)` / `End()`. That is the same scope the puppet uses. `0x4C` (the class-info
size) fits exactly between `+0x3C` and `+0x88`.

## 3. The surrounding calls (verified in IDA)

| VA | what | evidence |
|---|---|---|
| `0x5FAC00` | `BuildEntityWithSelector(this, Entity** out, sel)`, `retn 8` | calls `sub_7A22F0(sel)`, `sub_79DD50(sel)`, `Begin(0,1)`, `vt[0x4C](this, sel)` -> `*out`, `vt[0x40](this, out, sel)`, `End()`, then `sub_7A2380(sel)`. `sub_7A2380` clears `sel+0x2C..+0x3E` and does **not** free the selector |
| `0x5FD520` | builder slot 19: `sub_5FAC60(sel)` picks a template handle (builder `+0x94` entries, count `+0x9A`, else `+0x80`); `template->vt[0xC](0,0)` clones it; `sub_526590` sets flags; `sub_A2E820` flushes | decompiled |
| `0xA2D5A0` / `0xA2F320` | `DuplicationManager::Begin(a,b)` / `End()`: a per-thread **stack** (TLS `+24`, depth at `+2016`, 252-byte frames), so scopes nest | decompiled |
| `0x12F9C90` | the only writer of the puppet globals `0x4DEEF90` (selector) and `0x4DEEF94` (builder HC). Verified with `xrefs_to` (4 + 4 refs, all inside it). It runs `Begin(1,0)` / `Duplicate` / `End`, the pattern the puppet copies | decompiled |
| `0x70E880` | `World::RemoveEntity` = `sub_5A8AC0(this, e)` | disasm |
| `0x16FA7D0` | per-Human update (vtable data at `0x26EBBE8`): `mov esi, ecx` at `0x16FA7EF`, so `ecx` = Human. phase1's frame hook patches `0x16FAACD` in the same function | disasm |

`RowSelector::Duplicate` has 7 code call sites, all from `xrefs_to(0x7A3BD0)`: `sub_46D6F0` x2,
`sub_12F37D0`, `sub_12F4B50`, `sub_12F9C90`, `sub_13094F0`, `sub_1888C40` (`SpawnPlayerParams` slot 32).

**Lead, not verified:** `0x7A8860` seeds its row choice with `*(sel+4) ^ ResolveObjectID(builder)` whenever
`*(sel+4) != 0`. A duplicated selector copies `+4` (`0x7A3BD0`: `v4[1] = this[1]`), so a clone *may* get the same
appearance row as its source. UNVERIFIED. The visual check in section 6 is the only test of it.

## 4. Negatives and open points (each with METHOD / COVERED / NAMES)

**U1 - a live guard entity links back to its builder or selector.**
```
CLAIM:   the built entity stores no pointer to its EntityBuilder / RowSelector / SpawnEntityParams.
METHOD:  decompiled the build path 0x5FAC00, 0x5FD520 (slot 19), 0x5FAC60, 0x526590, 0x79BC50 (slot 16), 0x5FD560
         (slot 14, +0x38), 0x5F7F40 (slot 18, +0x48) and 0x7A8860 (row picker: weighted choice, seed = sel+4 XOR the
         builder's object id): none writes the builder, the selector or the params into the new entity.
COVERED: those eight functions only. NOT covered: the row-apply callback builder vt[+0x3C] called by 0x7A8860, the
         +0x38 of the selector's +0x14 children, the template's clone vt[0xC], the AddEntity side, the
         SpawningRequest subclasses. AC4BFMP.exe not searched.
NAMES:   n/a (no name filter used).
VERDICT: UNPROVEN. Not needed for the route: the probe captures the pair at build time instead.
```

**U2 - ambient guards come through sub_46D8E0 / 0x5FAC00.**
```
CLAIM:   the guards the player meets are built by BuildEntityWithSelector (so the capture sees them).
METHOD:  static: SpawnNPCParams/SpawnCharacterParams slot 31 -> 0x1898E20 -> 0x46D8E0 -> vt[0x44] (section 1).
COVERED: the four params classes whose slot 31 reaches 0x46D8E0. NOT covered: the callers of params slot 31 (+0x7C,
         indirect, not enumerated), other indirect callers of builder vt[0x44], the crowd path (slot 39 0x5FD730 clones a
         template without a selector and is NOT seen by this hook), mission/domino spawns (0x106ABF0).
NAMES:   n/a.
VERDICT: UNPROVEN. The probe measures it: npcBuilds/soldierBuilds counters and `capture #` lines.
```

**U3 - the guest can get a guard's selector without spawning that guard locally.** Not attempted. The capture needs a
local build of the type. With `[Coop] DisableSpawns=1` from the start, the guest builds no guards, so it captures
nothing (section 6, test B, measures this). The puppet's builder reload by id (`HandleManager::CreateReference` RVA 0x60BE50 +
`LoadingThread::AddLoadTask_Callback` RVA 0x62A6C0) returns a builder but not a selector. **OPEN.**

**U4 - a live guard's builder id is readable on the host.** Not attempted. The host-side equivalent of this probe (record
entity -> builder id in the same hook) would give it. A wire field for the id is a protocol change in `netstate.h`,
which another session owns. **OPEN.**

**Reversed earlier verdict (static only).** `AMBIENT-NPC-SYNC-PLAN.md` §6 closed "spawn a SPECIFIC guard" as **NO**.
Its method searched for an enumerable template table by name or hash (forge TOC, raw ids, addVisual callers). This
route needs no table: the type key is the builder handle that travels inside the selector, and the engine's own
spawn record hands both to the builder. That verdict is therefore UNPROVEN. It stays open until the live run below
passes.

## 5. The probe (`src/guard_spawn_probe.{h,cpp}`, `[Coop] GuardSpawnProbe`, default 0)

With the flag at 0 it does one `GetPrivateProfileIntA` and patches nothing. With 1:

1. **Detour `0x5FAC00`** (7 bytes `55 8B EC 56 8B 75 0C`). The original runs first. Then, for each build not made by
   the probe: classify the body (skeleton `0x01E41E58` + BhvGenericNPC `0x026E34D8` = NPC; WeaponComponent `0x027106A8`
   or MapIconBehaviorGuard `0x0269E9D0` = soldier), log the first build per builder id, and, for the first NPC of a new
   builder id, `Begin(1,0)` / `Duplicate(sel)` / `End()` and keep the copy.
2. **Detour `0x16FA7D0` entry** (6 bytes `53 8B DC 83 EC 08`). This is not phase1's `0x16FAACD`. It acts on the
   player's Human only, which is the context the puppet spawn already runs in:
   - every 10 s: census of `world->m_EntityManager.m_Entities`, counting entities, NPCs, soldiers and handle ids
     never seen before, plus the build counters for the window;
   - after `GuardSpawnProbeDelayS`: `GuardSpawnProbeCycles` cycles of: duplicate the kept selector ->
     `BuildEntityWithSelector` -> `World::AddEntity` + raw position write (player + `DistM` on X, the puppet's own
     placement) -> wait `HoldMs` -> oracle -> `World::RemoveEntity`.
3. **Oracle per cycle.** Entity vtable still `0x01E4CE90`, skeleton, BhvGenericNPC, a `CSrvNPCHealth` service
   (vtable `0x02712F60` in the AI service array, Max 1..2000), **same `arch` (bhv+0x14) as the source**, and **the same
   component-vtable list in the same order as the source body had at its build**.
   The run ends with `[GSP] RESULT cycles=N/N built added skel bhv hp sameArch sameComps removed notLoaded exc
   verdict=PASS|FAIL`. PASS requires every counter == N and exc == 0. A crash shows up as a missing RESULT line, plus
   WER / CrashContext.

Both prologues were checked against the installed exe's bytes. The code compares them again at runtime and patches
neither if either differs. Every engine call goes through the existing `Scimitar/` wrappers. New units in
`decomp/units.json`: `0x54D4D0`, `0x13905A0`, `0x7A3BD0`, `0xA2D5A0`, `0xA2F320`, `0x70E880`, `0x16FA7D0`, `0x46D8E0`.
`0x5FAC00`, `0x70E860`, `0x70E810` and `0x6F5F70` already had units.

Known costs, accepted for a probe: each captured type leaks one selector, and so does each cycle (the puppet path
leaks the same way). The census walks the whole entity list every 10 s on the player's update thread. The cloned
guard has live AI for `HoldMs`, so it can engage the player.

## 6. Live run plan (the user runs the game; nothing is installed without asking)

Before installing: back up the saves (`...\savegames\21c1f16e-...\437`) and `AC4Coop.ini`. Never copy while
`AC4BFSP.exe` is running. The install is shared with the other session's tests.

**Test C - clone oracle.** Single machine, `[Net] Enabled=0`. Start somewhere guards spawn (a fort approach or a
Havana street):
```
[Coop]
GuardSpawnProbe=1
GuardSpawnProbeCycles=20
GuardSpawnProbeDelayS=30
```
Stand still for the whole run, about 30 s + 20 x 4.5 s ≈ 2 min. PASS = the `RESULT ... verdict=PASS` line. The user
also confirms by eye that the clone looks like the guard type named in the `cloning capture #` line. That visual
check is the weakest oracle and is reported as such.

**Test A/B - does DisableSpawns stop guard/crowd spawns (counted).** `g_disableSpawns` sits in phase1's anonymous
namespace, so it cannot be toggled from this file. The test therefore uses two sessions, same save, same spot, ≥ 3 min
each, standing still:
```
A: [Coop] GuardSpawnProbe=1  GuardSpawnProbeCycles=0  DisableSpawns=0
B: [Coop] GuardSpawnProbe=1  GuardSpawnProbeCycles=0  DisableSpawns=1
```
Measure: per 10 s window after the BASELINE window, `newNpc`, `newSoldier` and `npcBuilds`. PASS = A > 0 (positive
control) and B == 0 in every window after the player is in world. Limits: the census does not see crowd bodies that
lack a BhvGenericNPC. Two sessions are a weaker control than an in-session toggle (the naval test toggled in-session
on `feat/live-tests`).

## 7. Reproduction (IDA, this pass)

```
idapro.open_database(<copy of AC4BFSP.exe.i64>, False)
xrefs_to 0x5FAC00                 -> 1 data ref 0x1E577D4
xrefs_to 0x79B8D0                 -> 11 callers (SpawningRequest::Spawn issuers)
xrefs_to 0x4DEEF90 / 0x4DEEF94    -> all in sub_12F9C90 (+ atexit thunk 0x1E39194)
decompile 0x12F9C90 0x46D6F0 0x46D8E0 0x18A03A0 0x189FDE0 0x1888C40 0x1898E20 0x46D6C0 0x462970
decompile 0x7A3BD0 0x7A3C90 0x5FD520 0x79BC50 0x469D10 0x5FAC60 0x526590 0x5F7F40 0xA2D5A0 0xA2F320
decompile 0x7A22F0 0x79DD50 0x7A2380 0xA2B760 0x79BC00 0x1128740 0x1996AB0 0x70E880 0x54D4D0 0x5FD560 0x7A8860
xrefs_to 0x7A3BD0 (7 code) 0x46D6F0 0x46D8E0 0x1128740 0x1996AB0
vtable dump 36 slots: 0x1E43BB0 0x26FD4F0 0x26FD580 0x26FD610 0x1E57790
get_wide_dword 0x27AF834 -> 0x27B0BD0; +0x18 = 0x4C; name "RowSelector"
disasm 0x16FA7D0 (22), 0x16FAACD context, 0x46D8E0 (50), 0x46D6F0 (150)
idapro.close_database(False)
```

## 8. Live run 1 - 2026-11-13, pid 7792 (log `AC4Coop_gsp_7792.txt`, 22,877 B, 149 lines)

```
BUILD  : [Build] asi=cd4e680849511f7477198443165a3bb3d7b4263b75dbefa11feabd5ae19cd352 commit=a3de755 tree=clean
GAME   : [BG] sha256=bc11ad79...ab5150 verdict=KNOWN. Session was a LINKED co-op run as guest ([Net] Mode=join, peer link up,
         puppet shown, [Coop] GuardAnimFull switched 0->1 by the other session at 10:43), not the solo run section 6 planned.
INI    : GuardSpawnProbe=1 GuardSpawnProbeCycles=20 GuardSpawnProbeDelayS=30 (hold 3000 ms, gap 1500 ms, dist 6 m)
```

**Oracle line, verbatim:**
`[GSP] RESULT cycles=20/20 built=20 added=20 skel=20 bhv=20 hp=20 sameArch=20 sameComps=14 removed=20 notLoaded=0 exc=0 verdict=FAIL`

| check | result |
|---|---|
| `selHcIsBuilder` (RowSelector+0x10 is the builder) | **1 on all 25 first-seen builds with a real handle.** The one 0 is `builderId=0`, `selHc=04DD5F8C` (the engine's invalid-handle sentinel), on the player's 32-component builder |
| every logged build via 0x5FAC00 has builder vtable `0x01E57790` | yes, 26/26 |
| NPC types captured (skeleton + BhvGenericNPC at build time) | 17 builder ids, arch `35F7`/`318C`/`31EC`/`3594`, 16-20 components, every `Duplicate` ok=1 |
| clone of capture #0 (builder `00000006'12CECF74`, arch 35F7, 19 comps) | 20/20 built (rc=0), added, held 3 s, removed. Every one had a skeleton, BhvGenericNPC, arch 35F7 and a CSrvNPCHealth service reading 36/36 |
| same component list as the source | **14/20.** Cycles 5, 9, 10, 14, 15 and 19 built with **18** components instead of 19, both at build and at hold. This is the only reason the verdict is FAIL |
| exceptions in the probe | 0 (`exc=0`, `buildExc=0` in all 61 census windows) |
| game survived | yes. It ran about 8 minutes after the last cycle and was closed by the user |

**Reading (measured, not interpreted further):**
- U2 is answered for this area. Ambient NPCs are built through 0x5FAC00. After the 20 s load window the census shows
  `npcBuilds` == `newNpc` or close to it in most windows (e.g. #7: 11 / 9, #21: 14 / 14), and 150 NPC builds in the
  first loaded window.
- **No soldier was seen.** `soldier=0` in every census window (about 97 NPCs live) and in every capture, so the cloned type
  is a generic NPC, **not a guard**. This run cannot say whether there were no soldiers nearby or the soldier test
  (WeaponComponent `0x027106A8` / MapIconBehaviorGuard `0x0269E9D0`) is wrong: the independent census in phase1
  (`[Sync] kindrole`) runs only on the host and read n=0 here. **The guard-type claim is still UNVERIFIED.**
- The 18-vs-19 components: one builder produces two component sets across builds. That fits the row picker `0x7A8860`
  choosing a different row, which means the `sel+4` seed lead (section 2) does **not** pin the variant. Which component
  differs is not known, because the log does not print the vtable list.
- **Placement does not hold.** In 14/20 cycles the position at hold was not the position written before AddEntity, up
  to 50 m away. Cycles 3 and 4, two different entities, both sat at exactly (27.4,-120.9,3.6). A raw `m_GlobalMatrix`
  write before AddEntity is not enough to place the clone (UNVERIFIED cause). Placement needs a write after the add
  takes effect, e.g. SetGlobalMatrixInternal as the puppet drive does.
- Visual check: not done (the user did not report on looks).

**Crash-context record in the same run** (`AC4Coop_crashctx_7792_1.txt`, 10:49:46, about t=483 s in world, about 6 min after the
last clone was removed). A read fault the game survived: `eip=006855D1` in `sub_685360` (`mov ecx,[esi+104h]` -> ecx=0 ->
`cmp [ecx+90h], eax`), esi=`2FEE5AE0`. `edi=4644B860` is the **puppet's CharacterActionSet** (`phase1_log_7792.txt:434`:
`CAS set from local player (pac=4644B860 ...)`). None of the probe's 58 pointers (20 clone entities, 20 cycle selectors,
17 captured selectors, the source's) appears anywhere in the record. Not attributed to the probe; not attributed to
anything else either. The stack scan lists ASI-range values, but this Release build has no map file to resolve them.

**Probe defect found by this run:** the log was opened with `fopen_s`, which denies all sharing, so it could not be read
while the game ran. Fixed in `5347330` (`_fsopen(..., _SH_DENYWR)`), not yet installed.

**Next run, to close O12:** at a fort or guarded area (soldiers present), with the vtable list printed for differing cycles
and placement done after the add. Then repeat for a soldier capture.

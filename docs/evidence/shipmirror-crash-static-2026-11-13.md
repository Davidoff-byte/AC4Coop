# ShipMirror v1 crash at sub_75A1F0 + the engine's own entity-instantiate path (O13), static, 2026-11-13

Binaries: **AC4BFSP.exe** md5 `2058342866688f780c8b34526a65bc35`; **AC4BFMP.exe** md5 `a82983ac8ddb00de30268e5c37e4ba51`
(IDA 9.3 headless `ida_batch.py`, private copies of `ref/ida/*.i64`, DBFL_KILL, nothing saved). Static only: the game
was not launched and the install was not touched. Inputs: `ops/evidence/RUN-2026-11-12-shipmirror-crash.md`,
ShipMirror code at `c82a696:src/phase1.cpp` (`DuplicateShip` L2303, `MirrorApply` L3545).
Rule 11 run first: `known.py SkeletonComponent / ActionComponent / DuplicationManager / 503600 / ShipSpawnedActor`.
Every address below comes from an IDA read in this pass. Anything I could not read is marked **UNVERIFIED**.

## Verdicts

**Q1, the root cause (static, strong; live confirmation still needed).** `DuplicateShip` clones a live AI ship inside
`DuplicationManager::Begin(1,0)`. That scope turns off the engine's pointer remap: frame byte `+0xED`, Begin's second
argument, gates three things. It gates registering each source-to-copy pair, recording each pointer that needs fixing,
and the rewrite that End performs. So a cloned component keeps the source's pointer values unchanged. That includes
`ActionComponent+0x24` (its SkeletonComponent) and every component's `+0x08` (its owning entity). Each tick, the
displacement job of the clone's ActionComponent reads `[[AC+0x24]+0x134]` without a guard and passes it as `this` to
`sub_75A1F0`. That `this` is the SOURCE ship's skeleton or pose object. The clone does not own it, and it dies when the
source despawns. The engine's own instantiate (`sub_527BA0`) uses `Begin(0,1)`, which is the opposite flag pair.

**Q2, O13 (MP `sub_B3B57B` as a model for an SP ship proxy): NO.** In MP it allocates a fixed 132-byte named node
for the online-services facades. It creates no entity, takes no class or template, and has no components (re-read in
MP, matches `mp-spawn-internals.md`). The SP analogue for "make an engine-built entity" is
`sub_527BA0`, which clones a template entity, followed by `World::AddEntity 0x70E860`. Addresses are in §4.

## 1. What sub_75A1F0 walks, and which field was invalid (VERIFIED, SP)

```
sub_75A1F0 [0x75A1F0..0x75A20D]   bytes 55 8B EC 8B 45 08 50 51 8B 89 A0 00 00 00 8B 49 04 83 C1 0C E8 ..
  0075A1F7 push ecx                 ; a2 of sub_5D55C0 = this (the skeleton/pose object)
  0075A1F8 mov  ecx,[ecx+0A0h]      ; <- FAULT (first memory access through ecx)
  0075A1FE mov  ecx,[ecx+4] / add ecx,0Ch / call sub_5D55C0
sub_5D55C0(desc, skel, quat): writes the quaternion into *(skel+0xA4) + desc offset, in the format given by *(skel+0x90)
```
`this` is a skeleton/pose object (`+0x90` mode, `+0xA0` hierarchy, `+0xA4` pose buffer, the same layout as MODLOG
2026-10-09 #4b). The fault is at the first dereference of `ecx`. So **the `this` pointer itself was unreadable**. The
fields inside it were not the bad value.

Every caller passes SkeletonComponent `+0x134` as that `this`. `xrefs_to 0x75A1F0` returns 3, complete:

| path | site | where `this` comes from |
|---|---|---|
| P1 | `0x4ADB0A` in sub_4AD990 `mov ecx,[edi+4]` | record+4. sub_65DDC0 writes it at `0x65DDF7` from `0x65DDD7 mov edx,[esi+24h]` / `0x65DDDA mov eax,[edx+134h]`. So the value is `[[ActionComponent+0x24]+0x134]`. The record is `AC+0x200`. sub_4AD990 writes `[edi+4]` only at `0x4ADB2B`, where it restores the saved value. |
| P2 | `0x6CDF47` in sub_6CDEC0 | `*(*(AnimComponent+0x7C)+0x134)`. sub_6CDEC0 is reached from sub_6D99E0, which is called by sub_6D9C70 = AnimComponent (vt `0x01E63CB8`) slot 44. That slot takes the sub_6D99E0 path only when `+0x7C != 0`. |
| P3 | `0x42350A` jmp in sub_423500 `sub_75A1F0(this[77])` = SkeletonComponent `+0x134` | sub_423500 is called once, at `0x659359` in sub_658F50 (`mov ecx,edi`). sub_658F50 is called from sub_65DDC0 at `0x65DE6B` with `ecx = AC+0x170`. Where `edi` comes from was **not traced**. |

- sub_65DDC0 is the **"Anim::UpdateDisplacement / ActionUpdateDisplacement"** frame step. sub_5863D0 stores it at
  `a1+4764` next to that string pair, so it is a per-ActionComponent job.
- `this` in sub_65DDC0 is an ActionComponent (vt `0x01E5CD10`, class map). The AC binds `+0x24` in its slot 19
  OnAttach, `sub_671330` (`0x67134F mov [esi+24h],eax`). The value is `sub_5200C0(entity, sub_423310())`, and getter
  `sub_423310` returns `off_274787C` → `0x02749420` = **SkeletonComponent** descriptor. So AC+0x24 is the owning
  entity's SkeletonComponent.
- The SkeletonComponent's own tick (slot 44, `sub_453740`) guards `if (*(this+308))` and locks `this+260`. The AC
  displacement job (P1) has **no** guard on `+0x134` and takes no lock on the skeleton component.
- The note's "reached from sub_4AD990" is **UNVERIFIED**: no stack survives. WER kept 6 AC4 dumps
  (`%LOCALAPPDATA%\CrashDumps`). None is pid 19552, and none faults at `0x75A1F8`. The candidates are P1, P2 and P3.
  All three take `this` from a SkeletonComponent's `+0x134`.

## 2. What DuplicateShip skipped that the engine does (VERIFIED, SP)

### 2.1 Begin's second argument switches pointer remapping on and off
`sub_A2D5A0` (Begin) stores `a1` into frame `+0xEC` (`0xA2D648`) and `a2` into frame `+0xED` (`0xA2D64E`). The frame is
`TLS[+0x18] + 0xFC*depth`. `+0xED` gates all three of these:

| step | function | gate |
|---|---|---|
| register src→copy pair into map `frame+0xF0` | `sub_A2B630`, called from `sub_9F0B10`, the base copy every component copy reaches through `sub_717D40` | `if (*(frame+237))` |
| record "fix this pointer later" `{src, &dst}` into `frame+0` | `sub_A2B6C0` (`0xA2B701 cmp byte [ecx+0EDh],0`) and `sub_A2D870` | same byte |
| rewrite at End: `sub_A2F320` → `sub_A2E820` → sort map (`sub_A2DE00`) → `sub_A2C250` binary search, `*pdst = copy` only if found | `sub_A2E820` | `if (*(v4+237))` |

`sub_A2B6C0(dst, src, a3)` first does `*dst = a3 ? 0 : src`. That is a raw copy, and it is all the function does when
the gate is off.

- **Mod:** `DuplicationManager::Begin(1,0)` (`src/Scimitar/DuplicationManager.cpp` → `0xA2D5A0`) gives `+0xEC=1,
  +0xED=0`. The mod then calls `vt[3](shipEnt, nullptr, 1)` (phase1.cpp L2312-2316).
- **Engine instantiate `sub_527BA0`:** `0x527BA4 push 1 / 0x527BA6 push 0 / call sub_A2D5A0` is Begin(0,1), so `+0xED=1`.
  It calls `vt[3](0,0)` at `0x527BBD`, then End at `0x527BC1`, then sets `entity+0x50 |= 4|8|0x800000` and calls
  `sub_522610`. The builder path `sub_5FAC00` uses Begin(0,1) as well (`spawned-body-animation.md` §2.1).
- Engine call-site census of `sub_A2D5A0`, 230 sites read 4 instructions back (heuristic push parse): (0,1) 137,
  (1,0) 46, (1,1) 33, register or other 14. So Begin(1,0) is a real engine mode. I have **not** classified what those
  46 sites copy (UNVERIFIED). The engine's entity instantiate is (0,1).

### 2.2 The two fields that therefore point at the SOURCE
- `sub_717D40` (Component base copy) calls `sub_A2B6C0(v4+8, *(this+2), 0)`. So **component `+0x08` (owner entity)
  is copied raw**.
- `sub_65DAE0` = ActionComponent slot 3 (copy). It calls `0x65DB52 mov eax,[esi+24h]` … `0x65DB5C call sub_A2B6C0`
  with `(v4+36, src+36, 0)`. So **AC `+0x24` is copied raw**. Under Begin(1,0) no fixup is recorded and End rewrites
  nothing. Result: **the clone's AC+0x24 is the source entity's SkeletonComponent** (sections 2.1 and 2.2 together).
- AnimComponent copy `sub_6D5B90` does **not** copy `+0x7C`. Its re-bind `sub_6CBF50` (from slot 19 `sub_6CDE80` and
  slot 21 `sub_6CDE50`) sets `+0x7C = sub_5220C0(*(this+8))`, which is the owner's SkeletonComponent. With the raw
  `+0x08`, that owner is the source entity too. Whether that bind runs on a clone is **UNVERIFIED**.
- Consequences for the source: P1 writes the clone's blended quaternion/vec into the **source's** pose buffer
  (`sub_5D55C0` writes `*(skel+0xA4)`). The clone keeps a pointer it does not own. Naval traffic does despawn: in
  `RUN-2026-11-12-naval-spawn.md` phase B the count went 5→1 as "ships leave range, none replace them".
  `sub_107D1D0` / `sub_12353B0` do the teardown (`ship-despawn-and-damage-event.md` §3).

### 2.3 Writers of AC+0x24 that were checked (rule 3, scope stated)
`vtscan 0x1E5CD10` (44 slots, regex `\+ 36\)`) hits only slot 3 `sub_65DAE0` (copy) and slot 19 `sub_671330`
(OnAttach). Slot 24 `sub_66BA50` reads it. `sub_670D80` (called by slots 19 and 21) reads `+36` and stores it into
action-node records (`+100`). It does not write it.
**Not covered:** a whole-image `[reg+24h]` writer sweep (the offset is too common to filter by class), virtual
dispatch, and the deferred AddEntity consumer (`sub_70E860` → `sub_5A8A80` → `sub_5A7F20` only queues a record through
`sub_5965E0`). So "nothing re-binds AC+0x24 on a clone after AddEntity" is **UNPROVEN**.

### 2.4 Other differences from the engine path (each read, effect UNVERIFIED)
1. **Source is a live ship, not a template.** The engine resolves a template entity from a handle before `sub_527BA0`:
   `0x11227B6 mov ecx,[esi+0B8h]` + unwrap, then AddEntity `0x112283F`. The mod clones a ship that is already in the
   world, with a built rig.
2. **Duplicate flag:** the engine calls `vt[3](0,0)` and the mod calls `vt[3](nullptr,1)`. The flag reaches every
   component copy through `sub_A27550` / `sub_A275A0`. Its meaning was not read.
3. **Activation:** the engine sets `+0x50 |= 4|8|0x800000` and calls `sub_522610` (LOD/cull level from the
   SkeletonComponent). The mod does AddEntity and SetPosition only.
4. **Init phases:** `sub_A2DAE0` (Entity/EntityGroup Init registration in `sub_52A980` / `sub_503600`) behaves
   differently by mode. With `+0xEC`=1, or with an empty frame job stack, it queues to `sub_A39360`
   ("::Engine::Initializer"). Otherwise it queues to the frame's job list (`sub_A40BD0`). End calls `sub_A2D660`
   (→ `sub_A35CF0(2,…)`); that this drains the list is **UNVERIFIED**. Init (`sub_527270`) calls every component's
   slot 21 Initialize(phase 0/2). When the Begin(1,0) queue drains
   relative to the component copy (`sub_A27550` runs after `sub_A2DAE0` inside `sub_52A980`) is **UNVERIFIED**.
   The SkeletonComponent copy `sub_441FE0` does not copy `+0x134` (re-read; matches `skeleton-attach-model.md` §2.3).
   So the clone's own skeleton has no rig until slot 21 runs.

## 3. Candidates (UNVERIFIED until the differential below) and the read-only plan that decides them

- **C1 (best supported): dangling source skeleton.** The clone's AC+0x24 is the source's SkeletonComponent (§2.2,
  static). The source despawns or streams out, and `[stale+0x134]` is garbage, so the fault is at `0x75A1F8`. This fits
  three observations: the own-ship ghost is stable (its source never despawns), the crash came about 20 s into a
  traffic run, and source `4368ECA0` was cloned twice.
- **C2: source alive, but its rig was torn down or rebuilt** (model or LOD change through `sub_45ABB0` → `sub_45A830`),
  read without the `+260` lock. `+0x134` is 0 or stale.
- **C3: re-bound to the clone's own SkeletonComponent** (some path not covered in §2.3), whose `+0x134` was never
  built, so the fault address is `0xA0`.

**Differential (read-only, no behaviour change; the live session runs it):**
1. At each `[ShipMirror] created`, walk source and clone the same way: component array `+0x60` / u16 `+0x66`, plus
   sub-entities `+0x140` / u16 `+0x146` one level down. Log per component:
   - AC (vt `0x01E5CD10`): `+0x08`, `+0x24`
   - AnimComponent (vt `0x01E63CB8`): `+0x08`, `+0x7C`
   - SkeletonComponent (vt `0x01E41E58`): its address, `+0x08`, `+0x134`
2. Every 5 s stat window: is the source's id still in the naval list (`mgr+0x3E8`, count u16 `+0x3EE`)? Also log
   `clone AC+0x24 → +0x134`, and stop dereferencing once the source is absent.
3. A VEH logger (the existing DrTrap/GpTrap pattern) that does nothing except log `ExceptionInformation[1]` and
   registers when EIP == `0x75A1F8`. Also copy `%LOCALAPPDATA%\CrashDumps\AC4BFSP.exe.<pid>.dmp` right after any
   crash, because WER rotates them.
4. Control run: `ShipMirrorForceProxy=1` with the source forced to the player's own ship (never despawns).

**Decision:**
- **C1:** clone AC+0x24 ∈ {source SkeletonComponents} and clone `comp+0x08` == source entity, and the crash follows
  the source leaving the naval list.
- **C2:** same pointer identity, but `+0x134` changes while the source is still listed.
- **C3:** clone AC+0x24 == the clone's own SkeletonComponent, with `+0x134`=0 and fault address `0xA0`.

The control run should show shared pointers and no crash under C1/C2.

**Fix direction (UNVERIFIED, do not ship before the differential):** instantiate through the engine's path. Use
`sub_527BA0` with `ecx` = a template or source entity, which gives Begin(0,1), remap, End and activation, then
`World::AddEntity`. Do not clone inside Begin(1,0). Note: the own-ship ghost has the same raw pointers, so its
AC also writes into the Jackdaw's pose (UNVERIFIED live).

## 4. Q2 (O13): MP sub_B3B57B vs an SP engine-built ship proxy

**MP, re-read this pass:** `xrefs_to 0xB3B57B` = 71 (21 in `sub_76B3AC`). The body takes a lock at `this+40`, then
`sub_401000(alloc,4,132)` + `sub_75C411(name,a3)` (name ptr / hash / zeroed fields). It sets id/alive when
`this+8` is set, evicts duplicate ids from 20-byte lists `+0x74` / `+0x7C`, sets `+16` = owner, and appends to the
owner's vector. **This is not a model for a ship proxy.** It has no class, no template, no components, no world or
AddEntity, and no skeleton. The SP analogues, all SP and read this pass:

| step | SP address | receipt |
|---|---|---|
| instantiate a template | `sub_527BA0` (ecx = template entity, out ptr) | Begin(0,1) `0x527BA4`, `vt[3](0,0)` `0x527BBD`, End `0x527BC1`, activate `+0x50`, `sub_522610`; **42 callers** |
| template-instantiate + AddEntity pattern | `sub_11227A0` / `sub_1122850` | template = handle `[this+0xB8]`; `sub_70E860(world=[this+0xC8], e)` at `0x112283F` |
| naval-band uses of the same primitive | `sub_106ABF0` (**NavalCollectableComponent** `0x029B4658`), `sub_106AC80` (**NavalMineComponent** `0x029F80B8`) | template handle `[this+0xC8]`; `sub_5A8A80(world=[this+8], e, 0xFFFF, 1)` (`0x106AC40`); callers `sub_10914B0` / `sub_108A7F0` |
| add to world | `World::AddEntity 0x70E860` → `sub_5A8A80(this,e,0xFFFF,0)` → `sub_5A7F20` (queued) | 37 callers |
| traffic spawn gate (live-proven) | `SpawningRequest::Spawn sub_79B8D0` [0x79B8D0..0x79B91A] | 11 callers; `RUN-2026-11-12-naval-spawn.md` (declining it stops new ships) |
| naval-list insert | `0x109B500` via data `0x26A1610` → `0x1236130` → `0x12350A0` | `naval-insert-path.md` §3 |

`0x01082810` ("ShipSpawnReq") is **not** a ship spawner. It allocates a generic 0xDD0-byte request, pushes it to the
global list `dword_2AC1FBC` under a lock, and returns it (decompiled). It has **54 callers**, which set different types
at `+0xC` (46, 44, 6, 0x13; see `damage-formula.md`, `gc-lship-construction.md`).

**Open for a usable proxy (UNVERIFIED):** which template handle holds an AI-ship archetype, meaning which object's
`+0xB8`/`+0xC8` handle `SpawningRequest::Spawn` → `vt[+0xC]` → `vt[+0x88]` (`0x79B8FC`–`0x79B910`) leads to.
The ship's per-class template was not located in this pass.

## 5. Negatives (rule 6)
```
CLAIM: the 0x75A1F8 crash left no dump or stack on this machine
METHOD: minidump exception-stream parse (own script) of all 6 %LOCALAPPDATA%\CrashDumps\AC4BFSP.exe.*.dmp; ls WER dirs
COVERED: pids 10204,12060,19236,20096,26408,7164 -> EIPs 0, 0x6855D1, 0x15DB19C, 0, 0x15DB19C, 0x1780657; no 19552
NAMES: n/a (not name-based)        VERDICT: NEGATIVE for this machine (the game-dir log was not read, by instruction)

CLAIM: no writer re-binds a clone's AC+0x24 after Duplicate
METHOD: vtscan 0x1E5CD10 regex '\+ 36\)'; grepdecomp sub_670D80; decompile sub_52A980/sub_A27550/sub_527270
COVERED: AC vtable slot bodies + sub_670D80; NOT a whole-image disp sweep, NOT callees of AddEntity's deferred consumer
NAMES: 0 named functions relied on   VERDICT: UNPROVEN
```
MP: only `sub_B3B57B` / `sub_75C411` were read. Every DuplicationManager claim here covers **SP only**.

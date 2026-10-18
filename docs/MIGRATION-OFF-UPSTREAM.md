# Migration off the upstream skeleton

Goal: the repository contains **nothing** written by the upstream author — only our own code, with the
engine facts regenerated from our decompilation (`C:\Users\Administrator\ac4-decomp`).

Rule for every file: re-derive the layout/behaviour from the decompilation (and confirm live where the
code path matters), then write our own version. Never copy the upstream text; keep only what the engine
requires (offsets, sizes, virtual slots), which are facts about the binary.

## Status

| area | upstream lines | status |
|---|---|---|
| project files (.sln/.vcxproj/.filters/.gitignore) | ~430 | **to replace** (trivial, regenerate from our source list) |
| `src/Hook.{h,cpp}` | ~97 | **DONE** — rewritten from scratch, build verified |
| `src/HookMath.{h,cpp}` | ~645 | **header DONE** (rewritten; build verified). `.cpp` pending — needs a live positional check |
| `src/Scimitar/**` (class declarations + small .cpp) | ~5,100 | **to replace incrementally** — only the headers a change actually touches |
| `src/phase1.cpp` | 1,225 of 4,941 | ~75% ours; remaining early scaffolding rewritten opportunistically |
| `src/net.*`, `netstate.h`, `ship_sync.cpp`, docs | ours | keep |

## Order of work

1. **Project files** — regenerate `.sln/.vcxproj` from our own file list. Removes ~430 upstream lines and
   makes the build independent of their project layout.
2. **Hook** — reimplement the three call helpers we actually use (`CallClassMethod`, `CallCdeclMethod`,
   `CallClassMethod<void,...>` variants) as direct function-pointer casts; no framework needed.
3. **HookMath** — reimplement only the operations used (matrix from/to quaternion, vector ops, transform
   composition). Verify by comparing outputs against the live engine on a known entity.
4. **Scimitar headers** — replace one at a time, starting with the smallest and hottest:
   `ObjectShape`, `CollisionFilterInfo`, `Components/*`, then `Entity`/`World`, then `Human`/`AIActor`.
   For each: dump the vtable + field offsets from the decomp (or live), keep our own names, and compile.
   Do not attempt all at once; a header is only replaced when a change touches it.
5. **phase1.cpp** — as areas are touched, rewrite the early scaffolding rather than editing it.

## Verification for every replacement

- Build clean.
- The specific feature that uses the file still works live (e.g. after replacing `CollisionFilterInfo`,
  re-run the corpse-collision test).
- No upstream text remains: `git grep` for distinctive upstream identifiers/comments.

## Definition of done

`git log` upstream commits remain only as history; every file in `src/` is regenerated from our
decompilation; `CREDITS.md` reflects that the initial skeleton has been fully replaced.

## Live verification (2026-10-18)

`Hook.{h,cpp}` and `HookMath.h` were exercised end-to-end by the real kill path on the rewritten files:

````
[Kill] SELF-TEST peer event objId=4026541036 -> ent=358A9440
[KF] SetLife(-1) rc=0  life 36 -> 65535
[KF] is-alive service=FCB0B828 +4 00000000 -> 00001000
[KF] after : alive=0
[KF] death clip + collision layer -> Disabled(0x29)
````

User-confirmed: guard died, stayed down, no invisible wall. Position read from the matrix was correct
(`35.4,-73.7,3.4`), so the HookMath conversions are behaviourally equivalent.

Status: Hook done + verified; HookMath.h done + verified; HookMath.cpp still pending (next).

## Include-graph audit (2026-10-18)

Every Scimitar header was checked against every `.h`/`.cpp` include in the project:
- 35 headers, **34 referenced**, **1 dead** (`BipedComponent.h`, 21 lines) — deleted.
- Consequence: pruning is not a shortcut. The `Scimitar` block (~5,100 lines) is all load-bearing and has
  to be **replaced**, not removed. Replacement stays incremental, in dependency order:
  `ObjectShape` -> `CollisionFilterInfo` -> `Components/*` -> `Entity` -> `World` -> `Human`/`AIActor`.

### HookMath.cpp — replaced and verified (2026-10-18)

`HookMath.cpp` rewritten from scratch (own implementations of all declared functions; row-major, row
vectors, translation in row 4). Live oracle: `EntitySync=1 EntitySelfTest=1`, which runs
`FromMatrix`/`ToMatrix` on every mirrored guard each tick.

Result: guards **frozen, correct facing, no spinning** -> conversions correct. The "walking animation in
place" is the pre-existing self-test mirror behaviour (action replication without transform drive), not a
regression.

Tooling layer (Hook + HookMath) is now entirely ours.

## Build protection protocol (2026-10-18)

The working build must never be put at risk by migration work.

- **Known-good build**: tag `build-verified-2026-10-18` on `fix/live-game-udp-bind`. That is the binary on
  the PC and the tester package base (Hook + HookMath ours, live-verified).
- **All remaining migration work happens on `migration/off-upstream`.** Nothing is merged to
  `fix/live-game-udp-bind` until it has passed a build **and** the live check for the feature it touches.
- **Never deploy from the migration branch.** Deploying is a separate, explicit step after a live check.
- If a header replacement breaks something, revert only that header on the branch (the tag is untouched).
- Each header replacement is its own commit: build-clean is not enough, the using feature must be re-tested
  (corpse collision for `CollisionFilterInfo`, ship for ship types, puppet/mirrors for `Entity`/`Human`).

### Header replacement — first batch (2026-10-18, branch `migration/off-upstream`)

Replaced with our own text, declarations preserved (call sites depend on the names; layouts are engine
facts): `ManagedObject.h`, `Hierarchy.h`, `Memory.h`. Build clean. **Not deployed** — the verified build
(tag `build-verified-2026-10-18`) remains the one on the PC.

Remaining Scimitar headers: 31 (including the large `Entity`/`World`/`Human`/`AIActor`). Continue in
dependency order, one commit per header, re-testing the using feature after each.

### Row 6 — `HandleManager.h` (2026-10-18)

Rewritten with our own text; declarations unchanged. Re-derived from the binary:

- the table is one static object at `0x04DD5FA0`; callers load that address straight into `ecx`
  (`0x00407993`), so `GetInstance()` returns the object itself, not a pointer to it, and the class has
  no vtable;
- entries are 20 bytes: object `+0`, counters `+4` and `+8`, id `+12`. `CreateReference` (`0xA0BE50`)
  bumps `+8`, `CreateHandle` (`0xA0BDE0`) bumps `+4`;
- `FindContainerNoLock` (`0xA0BBF0`) takes no lock and creates on a miss; `FindContainer` is the
  locking lookup `0xA0BB40` (`retn 10h` = id + one pointer, lock at `+0x7C`). The `.cpp` pointed
  `FindContainer` at the no-lock core; corrected.

Verification. `.text` diff against the pre-rewrite build of this branch: **0 bytes**. The new
`bin\AC4Coop.asi` is byte-identical to the build running on the PC (`build-verified-2026-10-18`,
2026-10-18 08:55) in `.text`, `.data`, `.fptable`, `.rsrc` and `.reloc`; the only difference is 9 bytes
of build timestamp inside `.rdata`. No new machine code exists, so the row's "build + world loads"
check is inherited from the live-verified build instead of re-run. Not deployed.

Both `FindContainer` thunks are absent from the shipped `.asi` — the linker drops them as unreferenced,
so this row cannot alter runtime behaviour.

### Row 7 — `WhiteRoomManager.h` (2026-10-18)

Rewritten with our own text; declarations unchanged. Re-derived from the binary:

- it is a heap object behind a global pointer slot, not a static: the engine's getter `0x609670` is a
  single `return dword_2AC1E7C`, other code null-tests the slot (`0x550D80`), and the destructor
  `0x61A020` clears it — so `*(WhiteRoomManager**)0x02AC1E7C` is the right form;
- `+24` is a `HandleContainer` owned by the manager: the destructor releases `*(this+6)` (byte `+24`)
  through the container decrement sequence, and `0x44E830` dereferences it as `[0]` object / `[8]` count;
- the class does have a vtable (`0x1E58B98`, deleting destructor `0x61C710`); our header never
  constructs or virtual-calls it, so the layout is deliberately left opaque.

Verification. `.text` diff against this branch before the row: **0 bytes**; all sections identical to the
build on the PC except 5 timestamp bytes in `.rdata`. No new machine code, so the live check is
inherited from the verified build. Not deployed.

## Exhaustive remaining worklist (generated 2026-10-18)

Ordered smallest-first. Replace one, build clean, live-test, commit. Do not batch the big four.

| # | lines | header | live check after replacing |
|---|-------|--------|----------------------------|
| 1 | 9 | `LoadingManager.h` | startup + save load |
| 2 | 13 | `LoadingThread.h` | startup + save load |
| 3 | 13 | `DuplicationManager.h` | build + world loads |
| 4 | 15 | `PlayerPadManager.h` | build + world loads |
| 5 | 17 | `Components\AIComponent.h` | build + world loads |
| 6 | 18 | `HandleManager.h` | build + world loads |
| 7 | 18 | `WhiteRoomManager.h` | build + world loads |
| 8 | 18 | `PlayerProgression.h` | build + world loads |
| 9 | 23 | `AI\AbstractEntityAI.h` | build + world loads |
| 10 | 24 | `InventoryContainer.h` | build + world loads |
| 11 | 28 | `Components\Component.h` | build + world loads |
| 12 | 28 | `BoneHandle.h` | mirror pose + action replication |
| 13 | 29 | `Object.h` | build + world loads |
| 14 | 32 | `Statechart.h` | build + world loads |
| 15 | 54 | `AI\HumanSwimFSM.h` | mirror pose + action replication |
| 16 | 70 | `Components\ActionComponent.h` | mirror pose + action replication |
| 17 | 97 | `Components\CharacterController.h` | corpse collision + ship contact |
| 18 | 116 | `Components\PhysicComponent.h` | corpse collision + ship contact |
| 19 | 160 | `ClassDescriptor.h` | build + world loads |
| 20 | 160 | `CollisionFilterInfo.h` | corpse collision + ship contact |
| 21 | 161 | `Entity.h` | spawn puppet + kill a guard |
| 22 | 174 | `ObjectShape.h` | corpse collision + ship contact |
| 23 | 245 | `World.h` | world loads, mirrors drive |
| 24 | 247 | `GraphicTypes.h` | build + world loads |
| 25 | 261 | `Enums.h` | build + world loads |
| 26 | 369 | `BaseTypes.h` | build + world loads |
| 27 | 548 | `AI\AIActor.h` | mirror pose + action replication |
| 28 | 766 | `AI\Human.h` | mirror pose + action replication |

# Guest crash in the world-update walk: three unguarded sibling leaves (2026-11-22)

Follows `ops/evidence/guest-crash-2026-11-21.md` and `guest-crash-dump-2026-11-21.md` (DeepSeek) and the runbook
`ops/VERIFY-SESSION-2026-11-21.md`. AC4BFSP.exe md5 `2058342866688f780c8b34526a65bc35`; IDA jobs `D:\ac4ida\cc\jobs\p5`-`p8`.

## 1. Runbook pass on the deployed build (unattended, no owner)
Build `feat/integration-1121` @ `a8a1165`, asi `db928886…`, `tree=clean` in both `[Build]` lines. Two copies started
from the exe at sea (save at (-4035, 4087)), idle, all runbook flags from the live ini. Host pid 29640, guest pid 32568,
started 07:20:04. Saves backed up first (`ref/AC4Coop-lobby/backups/saves-437-20261122-071712-verifysoak`).

| # | item | result | evidence line |
|---|---|---|---|
| 1 | provenance + guard installed | **PASS** | `[Build] asi=db928886… commit=a8a1165`; `[WalkGuard] installed 75A1F0 75A1B0` on both; guest `null L hit n=2` |
| 2 | `GcLBoardingBattle` reachable | **PASS** | `[CoordE] vt=026AEFD8 n=1` on both copies |
| 2b | detection block per-instance or shared | **INCONCLUSIVE** | no `[Tune] ship=` line in either log (no same-archetype pair in view at that spot, or the probe did not fire: not distinguished) |
| 3 | AI-off verdict | **NOT RUN** | `[AiOff] skip reason=helper-unreadable` (no guard at sea) |
| 4 | helm presence | **NOT RUN** | needs a person at the helm |
| 5 | sail/hull no regression | **INCONCLUSIVE** | 0 `[CoordS] rx vt=` lines on either side in 4 min; `SailTestCycle=1` produced no sail traffic here. Not investigated |
| 6 | boarding objective sticks | **NOT RUN** | needs a boarding |
| 7 | 10-minute soak | **FAIL** | the guest died at minute 4 (07:24); dump `AC4BFSP.exe.32568.dmp`; the host survived; `[ShipMirror] stat live=1 … meanErr=0.00` |

Both puppets built cleanly (`BuildEntityWithSelector rc=0` on both copies): no spawn fault in this sample.

## 2. The crash (cdb on the dump)
```
eip=0075a1d3  8b81a0000000  mov eax,dword ptr [ecx+0A0h]   ecx=00000000
0acffe34 0042610a  <- 0065de5a <- 0057e0de <- 00a327a6 <- 00a1db19 <- 008165ce ... (world-update worker thread)
```
- `0x0075A1D3` is **not** in either guarded leaf: `sub_75A1B0` is `[0x75A1B0, 0x75A1CA)` and `sub_75A1F0` starts at
  `0x75A1F0`. It is `sub_75A1D0`, a third leaf with the same unchecked `[L+0xA0]` dereference.
- Caller: `sub_4260A0` at `0x00426105`, `call sub_75A1D0` with `ecx = [esi+0x134]` (null here).
- This is the "second dump" shape DeepSeek recorded for dump 33104 (NULL_CLASS_PTR_READ at `[ecx+0A0h]`). The
  garbage-lock crash (`0x194d2`) did **not** occur in this run.

## 3. Every leaf of the family (byte scan + decompile)
Scan of the exe file for `8B 81 A0 00 00 00` / `8B 89 A0 00 00 00` in `0x759800`-`0x75B000`:

| leaf | head | contract (decompiled) | guarded before |
|---|---|---|---|
| `sub_75A1B0` | `55 8B EC 8B 45 08 50 51 8B 89 A0…` | thiscall(L, a), `retn 4` | yes |
| **`sub_75A1D0`** | `55 8B EC 8B 81 A0 00 00 00` | thiscall(L, out): `sub_5D2B30(L->A0->4, out, L)`, returns out (vec4); ends `8B C6 5E 5D C2 04 00` | **no: crashed** |
| `sub_75A1F0` | `55 8B EC 8B 45 08 50 51 8B 89 A0…` | thiscall(L, a), `retn 4` | yes |
| **`sub_75A210`** | `53 8B DC 83 EC 08 …`, `+0x16 8B 81 A0…` | thiscall(L, out): two qwords from `sub_5D2BD0` into out, returns out; ends `… 5B C2 04 00` | **no** |
| **`sub_75A2E0`** | `53 8B DC 83 EC 08 …`, `+0x16 8B 81 A0…` | (L, idx, out): `*out` = vec4; ends `5F 5E 8B E5 5D 8B E3 5B C2 08 00` (`retn 8`) | **no** |

A whole-`.text` scan finds 699 such instructions in total. This document claims only the cluster above, the one the
crashing call chain uses.

## 4. Change: `[Coop] WalkNullGuardExt` (default 0), `src/walk_guard.cpp`
- One 0x40-byte cave per unguarded leaf:
  - `test ecx,ecx`; non-null runs the stolen prologue (9 bytes for `75A1D0`, 6 for the aligned-frame two) and jumps
    back;
  - null writes `(0, 0, 0, 1.0f)` to the output pointer (`[esp+4]`, or `[esp+8]` for `75A2E0`), counts the hit, and
    returns with the leaf's own `ret N` and the output pointer in eax.
- Each leaf's head and epilogue bytes are verified before patching; a mismatch skips only that leaf.
- The reporter line now splits hits per leaf (`[1F0=… 1B0=… 1D0=… 210=… 2E0=…]`).
- Installed only after the original two guards, and only with `WalkNullGuardExt=1` (read at load).
- UNVERIFIED: whether the caller expects a position or a rotation in the output. `(0,0,0,1)` is the identity rotation
  and a bounded point, where the caller would otherwise read stale stack contents.

## 5. Pre-registered check (written before the run)
Same rig, local build (asi `ceff8a5b…`, `feat/integration-1121` + this change + the puppet-build diagnostics), with
`WalkNullGuardExt=1`, 11 minutes idle at sea.
- **PASS:** both copies alive for 11 minutes, no new dump, `[WalkGuard] ext installed 3/3`.
- **FAIL:** any dump. Read it: `0x75A1xx` again means a leaf is still unguarded; anything else is a different bug.
- Hit counts per leaf are reported, not judged.

**Result (soak 2, asi `ceff8a5b…`, host 28880, guest 31136): FAIL.**
- `[WalkGuard] ext installed 2/3`: `sub_75A210` was refused (epilogue offset typed as `0x4E`; the byte count puts
  `C2 04 00` at `+0x4C`, and the log's "epi 00 CC 53" read at `+0x4E` confirms it). Fixed for the next build.
- The guest died at minute 5 with dump `AC4BFSP.exe.31136.dmp`. cdb:
  - `RtlEnterCriticalSection` `lock btr [ecx],0` with `ecx = 0x01560155`;
  - stack `0x00D587F9` <- `0x0066943E` <- `0x0057E276` <- `0x00A327A6`;
  - this is DeepSeek's **garbage-lock** shape (`X+0x104`; dump 30168 had `0x194d2`), not a `0x75A1xx` leaf. The
    extended guard cannot cover it.
- `[ShipMirror] stat live=0 pool=1 created=1` on the guest at the end: one proxy had been created.
- Both puppets built cleanly (`rc=0`).

## 6. Both crashes have one trigger: the proxy's world object
- DeepSeek's `ops/evidence/proxy-update-crash-2026-11-21.md` §7 verdict (A): a `DuplicateShip` clone reaches the
  world-update walk with no render instance of its own (`X+0x134 == 0`); `X+0x104` is a neighbouring field of the
  same 64-byte block.
- Today's two dumps hit the same object `X` (`this` of `sub_425C40`, and `[esi+0x134]` passed by `sub_4260A0`), one
  per field.
- The engine's own instantiate is `sub_527BA0(src, &out)` (job p9, 42 callers):
  1. `sub_A2D5A0(0, 1)`;
  2. `src->vt[0xC](src, 0, 0)`;
  3. `sub_A2F320`;
  4. `out+0x50 |= 4 | 8 | 0x800000`;
  5. `sub_522610`.
- `DuplicateShip` calls the same `vt[0xC]`, but with `(0, 1)` under `Begin(1, 0)`.
- **`[Coop] ShipMirrorInstantiate` (default 0)** makes step 1 of `DuplicateShip` call `sub_527BA0` instead. The rest of
  the clone setup is unchanged. UNVERIFIED that this gives the clone its own render instance.

**Pre-registered (soak 3), written before the run:** asi `a3a0a12d…`, `WalkNullGuardExt=1`, `ShipMirrorInstantiate=1`,
11 minutes at sea.
- **PASS:** both copies alive for 11 minutes, no dump, and `[ShipMirror] instantiate route: dup=<non-null>`.
- **Supporting:** zero `[WalkGuard] null L` hits (a proxy with its own `L` never reaches the guards with null).
- **Report:** `ShipMirror` live and meanErr. A proxy that no longer renders or follows its source is a regression even
  if nothing crashes.
- **FAIL:** a dump. n = 1: a pass is a lead to repeat, not a fix.

**Result (soak 3, asi `a3a0a12d…`, host 21996, guest 33480, 07:44-07:55): PASS (n = 1).**
- Both copies were alive at 11 minutes; no new dump; 0 `EXCEPTION` lines on either side.
- `[WalkGuard] ext installed 3/3` on both, and **zero** `null L hit` lines.
- Guest: `[ShipMirror] instantiate route: dup=3D9A0CE0` and `dup=3A319A50`, two proxies, both built through
  `sub_527BA0`.
  - `stat … live=2 … samples=102 meanErr=0.00` at peak, then each retired in place after 5 s without a record.
  - The host took the instantiate route 0 times: it mirrors nothing.
- Against soaks 1-2 (default clone route), whose guests died at minutes 4 and 5. Visual correctness of the proxies was
  not checked (nobody looked).
- **Pre-registered (soak 4), written before the run:** same build, `ShipMirrorInstantiate=1`, `WalkNullGuardExt=0`.
  If the instantiate route gives the proxy its own render instance, the guest survives 11 minutes without the
  extended guard, with zero null hits on the two original guards. A dump here means soak 3's survival needed the guard
  (or was luck).

**Result (soak 4, asi `a3a0a12d…`, host 13748, guest 27692, 07:58-08:09): PASS.**
- Both copies were alive at 11 minutes; no new dump; 0 `EXCEPTION` lines.
- Only the two original guards were installed, and they took zero null hits.
- The guest built **4** proxies through `sub_527BA0` (`instantiate route` x4); `live=1 … meanErr=0.00` while
  tracking.

## 7. Verdict
- **The guest crashes of 11-21/22 are the default clone route.**
  - Two soaks with `ShipMirrorInstantiate=1` (6 proxies, 22 minutes): no crash, and **no** null render instance reached
    any guarded leaf.
  - Two soaks on the old route (1-2 proxies): both guests died within 5 minutes, one per symptom (`X+0x134` null,
    `X+0x104` garbage).
- **Static reading, matching DeepSeek's §7 verdict (A):** the engine's instantiate (`Begin(0,1)`, `vt[0xC](0,0)`)
  produces a clone whose world object gets its own render instance; the `(0,1)` / `Begin(1,0)` clone does not.
  UNVERIFIED which step publishes `X+0x134`; the outcome is measured, the mechanism is not named.
- **`ShipMirrorInstantiate` now defaults to 1.** `WalkNullGuardExt` stays 0 (not needed on the new route; kept as a
  fallback). n = 2 soaks, same spot, idle.
- Not checked:
  - visual correctness of the proxies;
  - a soak while sailing past many ships;
  - cross-machine behaviour.

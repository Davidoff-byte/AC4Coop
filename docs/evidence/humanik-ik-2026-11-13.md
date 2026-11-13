# S2 — run-time IK / ground adaptation on Human bodies (`AC4BFSP.exe`), 2026-11-13

Scope: static RE only. No plugin code, no live run. Binary: `AC4BFSP.exe` md5 `2058342866688f780c8b34526a65bc35`,
imagebase 0x400000, read from a scratch copy of `ref/ida/AC4BFSP.exe.i64` with `DBFL_KILL` (nothing saved).
`AC4BFMP.exe` was **not** examined. Every statement below is about the SP binary only.

Question: does the game run HumanIK / foot IK on NPC guard bodies and on the co-op puppet? If so, what
are its inputs and gates, and can it make a borrowed guard look like it floats when host Z is clean?

## 0. Answer in five lines

1. **Yes, there is a per-frame skeleton IK pass on every `SkeletonComponent`.** It is the job
   "Anim::SkeletonIKUpdate::Update/Update2", and both job entries run one body at `0x453780` (§2). Autodesk
   HumanIK is statically linked (§1). No AI/behaviour check is on this path (§5).
2. Each body takes **one of two branches**, chosen by whether the `SkeletonComponent` owns a `BipedFootIK2`
   (`comp+0x4C`):
   * **A. `BipedFootIK2` present** → foot IK `sub_4CF4A0`. Its ground reference is the
     **CharacterController contact list**, not a raycast. With no contact it falls back to *entity position +
     the last stored contact offset* (§3.2).
   * **B. no `BipedFootIK2`** → `sub_442460`, a **ground-plane root adaptation**. It adds
     `planeZ − entityZ` (clamped ±1 m) to the **pose root translation**. The plane is a critically damped
     spring that chases the entity's *own* position and up axis. Visual only; the entity transform is not written (§3.1).
3. Both branches change only what is drawn. The entity position that we drive and that the host sends is not
   touched. That matches "host Z clean, body looks off".
4. Gates (§4): `EngineOptions` bytes `+0x63` / `+0x64`, entity `+0x9C` kind == 1,
   `SkeletonComponent` bits `+0x336` (1, 0x40) and `+0x337` (0x10), LOD bits `comp+0x88 & 0x38`, an active
   `RagdollComponent`, and CharacterController state `+0x90 & 0x3F == 0x15`.
5. The cause of the float is **not proven**. Two candidate mechanisms are below (H1, H2), with the live
   probe that decides between them and the seam for each (§6, §7).

## 1. HumanIK is in the binary

| Evidence | VA |
|---|---|
| HIK data-block tags `HIKCHARACTER000`, `HIKSTATE0000000`, `HIKEFFECTOR0000`, `HIKPROPERTY0000`, `HIKDATABLOCK000` | `0x1E5A384..0x1E5A3D4` (no code xref; a dword scan of all segments found no pointer to `0x1E5A384`/`0x1E5A398`) |
| Full HumanIK effector name table, `HipsEffector` … `RightFootExtraFingerEffector` | `0x25DE12C..0x25DE500` |
| HIK reach / contact property names (`FootContactType`, `ParamFootContactStiffness`, `FootBottomToAnkle`, `CtrlPullLeftFoot`, …) | `0x25E16D8..0x25E22D8` |
| Reflection classes `BipedFootIK2` (desc `0x2754A58`, vt `0x1E46BB4`), `QuadrupedFootIK` (vt `0x1E49D90`), `FootSocket` (vt `0x1E49810`), plus name-only `IKGround` (desc `0x275C328`), `FootIK` (`0x275C2C8`), `LegIK` (`0x27576E8`), `IKData` (`0x27BAFD8`) | descriptor name ptr at `desc+0xC`; those four descriptors are referenced only from registration tables `0x275A9C0/0x275A950/0x275678C/0x27B7A04` (dword scan, all segments, 1 hit each) |
| Bone identity is `crc32(HumanIK name)` (already documented) | `ops/evidence/ac4-bone-name-map.md` |

Method: case-insensitive regex over all C and UTF-16 strings (152,486 strings scanned, 205 matched),
`(humanik|hik|footik|legik|iksolver|footplant|groundadapt|\bik|ik[A-Z_]|ik$|foot|effector)`. Then a raw
dword scan of all segments for each class descriptor and name string. `FootPlant`, `GroundAdapt` and
`IKSolver` have **0 string hits**. That proves only that those *names* are absent (rule 5).

## 2. Update path (per frame, per `SkeletonComponent`)

* Job-graph builder `sub_587F70` (only caller `sub_588CA0` @`0x588CB9`, only caller `sub_729130` @`0x72939C`)
  registers the skeleton jobs. Each job node is 0xD8 bytes, with its callback at `node+0xD4`:
  * `Anim::SkeletonUpdate` → `sub_423390`
  * `Anim::SkeletonIKUpdate::Update` → **`0x455590`** (only xref: data `0x5882A5`)
  * `Anim::SkeletonIKUpdate::Update2` → **`0x4555A0`** (only xref: data `0x588367`)
  * `Physic::LiteRagdollComponent::Update` → `sub_1A1DBD0`
* `0x455590`: `test byte [ecx+336h],40h / jnz ret / jmp 0x453780`.
  `0x4555A0`: `test byte [ecx+336h],40h / jz ret / jmp 0x453780` (raw bytes `F6 81 36 03 00 00 40 74 05 E9 D2 E1 FF FF C3`).
  So **one body, `0x453780`**, runs in exactly one of the two phases, selected by `comp+0x336` bit 0x40.
* That bit is written by `sub_425C40` (`0x425C87`/`0x425C8F`, a SkeletonComponent function: it reads
  `+0x4C`, `+0x134`, `+0x88`). Bit 0x40 = **not**(`footIK != 0 && footIK+0x14 > 5e-4 && footIK+0x20 > 5e-4`).
  Active foot IK runs in the early phase; otherwise the body runs in Update2.
* The component is the `SkeletonComponent`: `+8` is the owning entity, `+0x20` the model, `+0x124/+0x128` are
  written by M3 `sub_45A830` (`ops/evidence/skeleton-attach-recipe-2026-11-10.md`), and `+0x4C` is allocated by
  `sub_431B70`, which is called only from `sub_45A830` (@`0x45A9FB`).
* Body `0x453780`, in order (disasm `0x4537F0..0x45391F`):
  1. **Vertical offset blend.** `+0xF4` steps toward `+0xF0` at rate `+0xF8 × dt`, where dt is
     `*(entity+0x5C)+0x8A0`. Then the pose root translation is set to `root + up × (+0xF4)`
     (`sub_426230` read @`0x45381E` → `sub_43AE00` write @`0x45383B`). This runs only if `+0xF0` or `+0xF4` is nonzero.
     *Writers of `+0xF0` were not searched* (UNPROVEN what sets it).
  2. `sub_450560` slope/bone alignment, only if `comp+0x336 & 0x10` and the model check passes (bone crc
     `0x530EC1CB`, effector request `sub_643280(1, …)` on the IK request block at `comp+0x1D4`).
  3. `sub_431F30` hand/arm effectors (`sub_643280(10|19|6, …)`, weight `comp+0x310`). It is committed by
     `sub_63A880(comp+0x1D4)` at `0x45388F`.
  4. **Foot IK** `sub_4CF4A0(comp+0x4C)` under the gates in §4 (`0x45389A..0x4538F8`).
  5. `comp+0x2C0 = 0` (`0x453904`); **if `comp+0x4C == 0` → `sub_442460`** ground plane (`0x45390C..0x453910`).
     A body that has `BipedFootIK2` never runs `sub_442460`, even when its foot IK is gated off.
* `BipedFootIK2` is allocated (0xAE0 bytes, ctor `sub_4BB6E0` called @`0x431D6A`) in `sub_431B70`, the pose
  build reached from M3. So **the puppet build (M1→M4→M2⇒M3) takes the same decision as a native body.**

## 3. Inputs

### 3.1 Branch B — `sub_442460` ground plane (no `BipedFootIK2`)

Disasm `0x442460..0x44279B`; the only caller is `0x453910`.

* `cc = sub_4375D0(comp+0x174, comp+0x124)`, a cached index into the entity component list (`entity+0x60`,
  version byte `entity+0xF0`) of type `off_27B2B6C` → descriptor `0x27B4E40` "**CharacterController**". If
  there is none, the function returns (`0x44249A`).
* **Reset** (`sub_42E6C0(comp+0x260, entityPos, up)` @`0x442525`, which puts the plane at the entity with
  zero root offset) when any of these holds:
  `!(comp+0x336 & 1)` (`0x4424A0`) · `(comp+0x88 & 0x38) < 8` (`0x4424A9`) · a `RagdollComponent`
  (`off_2753E64` → desc `0x2755D78`) on the entity with (`+0x1C0 > 0` or `+0x1DD`) and `+0x1C4 > 0`
  (`0x4424B5..0x4424E7`) · `(cc+0x90 & 0x3F) == 0x15` (`0x4424EC..0x4424F8`).
* Otherwise the plane at `comp+0x260` (normal at `+0x260`, point at `+0x2B0`) is driven toward
  **point = entity world position** (`sub_413460` with `ecx=entity`: matrix translation) and
  **normal = entity matrix row +0x20** (`sub_425BA0`). That is the entity's own up axis. **No raycast and
  no physics query.**
  * Spring rate = `1 + 2·clamp((|cc.vel.xy| − 1)/4, 0, 1)`, using `cc+0xA0`. It is **20** if
    `cc+0x18` has no contact entry with flag 1 or −1 (`sub_625DE0` @`0x44255B/0x442569`), ×3 if
    `cc+0x7C < 0.5`, and ×1.5 if `|entityZ − planeZ| > 0.4`.
  * Hard reset if the plane point is more than √3 m (squared 3.0) from the entity (`0x442640`).
  * The Z channel is a critically damped spring (`sub_43E190` @`0x4426AD` → `sub_4287E0`, coefficients 0.48/0.235).
* `dz = planeZ(at entity XY) − entityZ` (`sub_436430` @`0x4426BC`), stored at **`comp+0x2C0`** (`0x4426CC`).
  If `|dz| > 1` it is clamped to ±1 and the plane is shifted (`sub_42E650` @`0x44274E`). If `|dz| > 0.003`
  the pose root translation gets `z += dz` (`sub_426230` @`0x442771` → `sub_43AE00` @`0x44278E`).
* `sub_43AE00` writes the pose (`comp+0x138`, `comp+0x134`, LOD `(comp+0x88>>3)&7`). It does **not** write
  the entity matrix. **Visual only.**

Net effect: the drawn root trails the entity's Z by a spring lag of at most 1 m. When the entity moves down
in steps, the body is drawn above it until the spring settles.

### 3.2 Branch A — `BipedFootIK2` foot IK (`sub_4CF4A0`, `this = comp+0x4C`, only caller `0x4538F8`)

* Early out unless `footIK+4 & 0x40` (the enable flag; *the writer was not searched*).
* `footIK+8` = SkeletonComponent and `footIK+8 → +8` = entity (read in `sub_4CF4A0`/`sub_4CEF30`).
  `footIK+0xC` is a component that has the same fields the CharacterController has in §3.1
  (`+0x18` contact list, `+0x90` state with `0x15`, `+0xA0` velocity). *That it **is** the CharacterController
  is inferred from identical field use and is UNVERIFIED (its writer was not found).*
* **Reset** (`footIK+4 |= 0xA00`) only when the entity moved > 30 m/s since last frame
  (`|Δxy/dt|² > 900`) or `|Δz| > 2 m`, against `footIK+0x40/+0x48`.
* Ground reference, `sub_4BBA30` (only caller `sub_4CF4A0`): it searches the contact list
  `footIK+0xC → +0x18` for a contact with normal z > 0.1 (`sub_62C4C0`, fallback `sub_625E20`).
  * Contact found: `footIK+6 |= 1`, ground point `footIK+0x740 = contact point`,
    **offset `footIK+0x750 = contact − entityPos`**.
  * **No contact**: `footIK+0x740 = entityPos + footIK+0x750`. The **old offset is kept** unless the byte
    `footIK+0x19` is set, in which case it is zeroed. `footIK+6 &= ~1`.
* The solve itself, `sub_4CEF30` (only if weights `footIK+0x14` and `footIK+0x20` > 5e-4), runs 20 sub-steps
  (`sub_4AEE80, sub_4C92A0, sub_4C9870, sub_4BBC40, sub_4AFEC0, sub_49E110, sub_4BC310, sub_4B0C50,
  sub_4A3840, sub_4A4810, sub_4A57F0, sub_4CCF40, sub_4BF370, sub_4B14A0, sub_4B23A0, sub_4B27F0,
  sub_4B2F10, sub_4B30B0, sub_4B36A0, sub_4B4180, sub_4B4EE0, sub_4B6000`). Three of them call the
  plane-height helper `sub_436430` (`sub_4CCF40`, `sub_4B27F0`, `sub_4B2F10`). **These sub-steps were not
  decompiled.** Whether they move the pelvis/root, and whether any of them issues its own raycast, is **UNPROVEN**.
* Branch A does not reach `sub_442460`, and `comp+0x2C0` stays 0 for these bodies.

## 4. Gates (all VAs from the disasm of `0x453880..0x453910` and the decompile of `sub_431B70`)

| Gate | Where | Effect |
|---|---|---|
| `BipedFootIK2` allocated | `sub_431B70` | needs `!(comp+0x88 & 0x1000)`, resolved model `+0x30 <= 1`, **and** (`EngineOptions+0x64 == 0` **or** entity `+0x9C & 7 == 1`) |
| `EngineOptions+0x63 != 0` | `0x4538A5` | skips foot IK (and still no `sub_442460` if `+0x4C` exists) |
| `EngineOptions+0x64 != 0` and kind != 1 | `0x4538D9` | skips foot IK for that body |
| `comp+0x337 & 0x10` | `0x4538AB` | **skips foot IK for that body**, for every kind |
| kind == 1 | `0x4538ED` | `footIK+4 |= 1` before the update |
| `comp+0x336 & 0x40` | `0x455590`/`0x4555A0` | phase choice only (§2) |
| `comp+0x336 & 1` clear | `0x4424A0` | **Branch B plane reset → no root offset** |
| `(comp+0x88 & 0x38) < 8` | `0x4424A9` | Branch B reset (bits 3..5 are also the pose LOD index in `sub_43AE00`/`sub_4427A0`) |
| RagdollComponent active / CC state 0x15 | `0x4424B5..0x4424F8` | Branch B reset |

* `EngineOptions` is the global `dword_2ABEC08`. Its getter is `sub_45BE80` (61 callers) and its only writer
  is `sub_45BE90` (xrefs to `0x2ABEC08`: 1 read, 1 write). `sub_45BE90` is called once, at `0x77E304`, at the
  end of the ctor of the class with vt `0x1E6C440` = "EngineOptions" (class map). That ctor code sits in an
  undefined band ending `0x77E311` (rule 2), read by range disasm `0x77E180..0x77E312`. The ctor zeroes dwords
  `+0x60` and `+0x64` (`0x77E2DC`/`0x77E2DF`), so **both bytes start at 0**. EngineOptions is a reflection
  class, so data may overwrite them. **Their run-time values are UNPROVEN.**
* Entity `+0x9C & 7` is the "kind" field (`ops/evidence/faction-and-roles.md` §7: kind 3 Templar guard,
  kind 2 Peasant). *What kind 1 is (player / Edward-built puppet?) is UNVERIFIED.*
* Writers of `+0x336` (dispsweep: 131,647 functions, 7,878,952 instructions, 22 hits, all with displacement
  0x336 on any base): bit 1 set **only** at `0x153AD6F` (`sub_153ACF0`, on `sub_5220C0(entity)`), cleared at
  `0x155E4D4`. There are 13 more `xor`/`mov`/`or 4|8` hits whose base class was not attributed.
  **Families covered:** `mov/or/and/xor/add/sub/adc/sbb/inc/dec/not/neg/xchg/cmpxchg/xadd/setcc/movss/movd [r+0x336]`
  and `lea r,[r+0x336]`. **Skipped:** word/dword writes at `+0x334`/`+0x335` that overlap `+0x336`
  (M3 does `+0x335 |= 4`), `rep movs` copies (the SkeletonComponent copy ctor), and code outside defined
  functions. So "which bodies have bit 1" is **UNPROVEN**. `+0x337` writers: 3 hits (`0x877C7C` word
  `0x101`, `0x1C8F1A2`, `0x1C913BD`), class not attributed.

## 5. AI / behaviour, distance and LOD

* AI: `0x453780`, `sub_442460`, `sub_4CF4A0`, `sub_4BBA30` and `sub_431B70` read only the SkeletonComponent,
  the entity transform and `+0x9C`, the CharacterController, the RagdollComponent and EngineOptions.
  **None of them references BhvGenericNPC or the behaviour component.** Disabling the behaviour does not
  disable this pass.
  METHOD: full decompile + disasm of those five bodies. COVERED: those bodies and their direct helpers
  listed in §3. NAMES: none matched. *Not covered:* the 20 `sub_4CEF30` sub-steps, `sub_4A63E0`,
  `sub_4CB450`, `sub_450560`'s callees. So "the IK pass is not gated by AI" is proven only for the gates
  listed in §4, not for the whole foot-IK subtree.
* Distance/LOD: the only LOD-like input seen is `comp+0x88` bits 3..5 (`< 8` resets Branch B). No camera
  distance read was seen in the bodies above (same coverage caveat).

## 6. Hypotheses for "borrowed guard floats" (UNVERIFIED) and the probe that decides

* **H1 (Branch B lag).** Guard bodies without `BipedFootIK2` and with `comp+0x336 & 1` set draw the root at
  `entityZ + comp+0x2C0`. Our position drive moves the entity in network-rate steps, which feeds the spring
  a staircase. Going down, the drawn body sits up to 1 m above the entity until it settles. A guard that
  stands still converges to 0, so H1 predicts the float **only while moving or just after a Z step**.
* **H2 (Branch A stale contact offset).** On a borrowed body the CharacterController may have no ground
  contact (or stale ones) because we place the entity directly. `sub_4BBA30` then uses
  `entityPos + footIK+0x750`, where `+0x750` is the offset left from the body's **last** real contact
  (possibly from where it stood before we took it over). It is reset only by a > 2 m Z jump or a > 30 m/s
  move. H2 predicts a **persistent** float (or sink) of a fixed size per guard.

**Probe (read-only, one floating guard and one normal guard, same frame):**
`comp = SkeletonComponent of the body`; log `comp+0x4C` (≠0 ⇒ Branch A), `comp+0x336`, `comp+0x337`,
`comp+0x2C0` (Branch B offset actually applied), `comp+0xF0/+0xF4` (§2 step 1 offset). If `comp+0x4C != 0`,
also log `footIK+4`, `+6` (bit 0 = contact this frame), `+0x14`, `+0x20` (weights), `+0x740` (ground point),
`+0x750` (stored offset), and `EngineOptions(*0x2ABEC08)+0x63/+0x64`.

* Branch B with `|comp+0x2C0|` ≈ the visible float → H1.
* Branch A with `footIK+6 & 1 == 0` and `footIK+0x750.z` ≈ the float → H2.
* `comp+0xF4 != 0` → a third source (the §2 step 1 vertical offset), not covered by H1/H2.
* All zero → the float is not in this pass; look at the 20 foot-IK sub-steps or elsewhere.

## 7. Recommended seams (each needs the probe first; none is implemented)

| For | Seam | Why it is expected to work | Status |
|---|---|---|---|
| H1 | Clear `comp+0x336` bit 1 on mirrored bodies (after M3 / after borrowing) | `0x4424A0` then takes the reset path every frame and `0x453904` has already zeroed `comp+0x2C0`, so no root offset is applied | static only; something may set bit 1 again (writers §4 not attributed) — re-clear per frame in the existing per-Human hook, or verify with the probe |
| H1 alt | After each drive step, do what the engine's reset does: `sub_42E6C0(comp+0x260, &entityPos, &up)` (Hex-Rays shows `__thiscall(this, point*, normal*)`; *the exact convention and stack cleanup are UNVERIFIED — read the epilogue before calling it*) | same state as the engine's own reset | static only |
| H2 | When we take over a body or teleport it: zero `footIK+0x750` and set `footIK+4 |= 0xA00` | `0xA00` is the engine's own "moved too far" reset (`sub_4CF4A0`); the zero offset makes the no-contact fallback equal to the entity position | static only; the full meaning of 0xA00 inside the sub-steps is not decoded |
| H2 strong | Set `comp+0x337 |= 0x10` on mirrored bodies | `0x4538AB` then skips `sub_4CF4A0` for every kind | **risk:** the last IK pose may stay frozen in the pose buffer (not checked) |
| either | Not recommended: patch `EngineOptions+0x63/+0x64` | global; it changes every NPC and the local player | — |

Do not drive these from AI state: nothing in this path reads AI (§5).

## 8. Not done / open

* `AC4BFMP.exe` not checked (rule 4). No engine-wide absence claim is made.
* The 20 `sub_4CEF30` sub-steps, `sub_4A63E0` and `sub_4CB450` (1,119-line decompile) are not read. Pelvis
  lowering and raycasts in the foot solve are UNPROVEN either way.
* The writers of `footIK+4 & 0x40` (enable), `footIK+0xC`, `comp+0xF0` and `comp+0x336` bit 1 on guards are not found.
* Which body gets which branch at run time: needs the §6 probe. Statically it depends on EngineOptions bytes
  whose run-time values are unknown.

## Reproduce

Run `idat -A -S"<script> <jobs.json> <out>" <copy of ref/ida/AC4BFSP.exe.i64>` with the shared
`ida_batch.py` (job kinds decompile/disasm/xrefs/bytes/dispsweep/grepdecomp/vtscan) plus four small helpers
written for this pass: `strsearch.py` (regex over strings + xrefs), `dwscan.py` (raw dword scan of all
segments), `typename.py` (type handle → descriptor → name at `desc+0xC`) and `rdis.py` (range disasm for
undefined bands). Every script sets `DBFL_KILL`. Copies are in the shared side-session notes folder under
`humanik-s2-scripts/`.

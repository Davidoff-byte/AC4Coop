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
| H2 | Zero `footIK+0x750` while `footIK+6 & 1 == 0` (see §9.5) | the no-contact branch `0x4BBBFA..0x4BBC1C` then computes ground = entity position | static only. *Correction (§9.5):* `footIK+4 |= 0xA00`, suggested here in the first version, does **not** reset `+0x750`; it only sets 0x800 and zeroes `+0x28` that frame |
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

## 9. Probe spec for `src/phase1.cpp` (read-only; added 2026-11-13 at the plan session's request)

Target: a borrowed guard body `Entity* e` whose matrix the plugin writes in `OwnDrain` (hook `0x016FAACD`).
Take the reads in `OwnDrain`, after the plugin's own `SetGlobalMatrixInternal`, and log one line per guard per
N frames. Also log the same line for one **non-borrowed** guard as the control. Use `readable()` on every pointer.

### 9.1 Reaching the objects (no engine calls needed)

| Object | Chain | Evidence |
|---|---|---|
| `comp` (SkeletonComponent) | `FindComponentByVtable(e, 0x01E41E58)`, the existing helper in phase1.cpp (walks `e+0x60`, count `u16 e+0x66`) | vt slot 5 `0x423310`; M1 already checks this vtable live; class map row `0x01E41E58 → desc 0x02749420 SkeletonComponent` |
| `cc` (CharacterController) | `FindComponentByVtable(e, 0x01E6EE10)` | vt `0x1E6EE10` slot 5 = `0x7AA8E0` = the class getter used by `sub_4375D0` in Branch B (`mov eax, off_27B2B6C`; `*0x27B2B6C` = desc `0x27B4E40` "CharacterController") |
| `fik` (BipedFootIK2) | `*(void**)(comp+0x4C)`; valid if non-null **and** `*(u32*)fik == 0x01E46BB4` | ctor `sub_4BB6E0` writes vt `0x1E46BB4` (`0x4BB6F4`); stored at `comp+0x4C` by `sub_431B70`; body reads it at `0x45389A`/`0x4538F5` |
| `opt` (EngineOptions) | `*(u8**)0x02ABEC08` | getter `sub_45BE80`; read at `0x4538A5`/`0x4538D9` |
| kind | `*(u32*)(e+0x9C) & 7` | `0x4538B4..0x4538C4` |

Cross-check worth logging once: `*(void**)(fik+0xC) == cc`. That tests the §3.2 UNVERIFIED claim that
`fik+0xC` is the CharacterController.

### 9.2 Fields

All offsets are from the object named. f32 = 4-byte float; v4 = 16-byte-aligned float4 (x,y,z,w).

| # | Field | Type | Meaning | Writer / reader (VA) |
|---|---|---|---|---|
| S1 | `comp+0x4C` | ptr | ≠0 ⇒ **Branch A** (foot IK); 0 ⇒ **Branch B** | `0x45389A`, `0x4538FD` |
| S2 | `comp+0x336` | u8 | bit 0x01 = Branch B adaptation enabled; 0x40 = phase (foot IK inactive); 0x10 = slope align | `0x4424A0`, `0x455590`, `sub_425C40` |
| S3 | `comp+0x337` | u8 | bit 0x10 = foot IK skipped for this body | `0x4538AB` |
| S4 | `comp+0x88` | u16 | `(v & 0x38)` < 8 ⇒ Branch B reset (LOD); `v & 0x1000` blocks foot-IK allocation | `0x4424A9`, `sub_431B70` |
| S5 | `comp+0x2C0` | f32 | **Branch B root Z offset applied this frame** (zeroed at `0x453904`, set at `0x4426CC`/`0x442720`; applied when \|v\| > 0.003, `0x442769`) | — |
| S6 | `comp+0x2A0` | f32 | Branch B spring value (plane Z) | reset `0x42E6FF`; spring step `sub_4287E0` with `ecx = plane+0x40` (`lea ecx,[esi+40h]` @`0x43E268`, call @`0x43E29F`) |
| S7 | `comp+0x2A4` | f32 | Branch B spring velocity (`this[1]` of `sub_4287E0`) | `0x42E6EC` zeroes it |
| S8 | `comp+0x2B0` | v4 | Branch B plane point (z at `+0x2B8`) | `0x42E6E2`; `sub_43E190` end |
| S9 | `comp+0xF0`, `+0xF4`, `+0xF8` | f32 ×3 | §2 step 1: target, current, rate. Root Z += `+0xF4` when either of the first two ≠ 0 | `0x45380D` writes `+0xF4` |
| A1 | `fik+0x4` | u16 | bit 0x40 = foot IK enabled (else `sub_4CF4A0` returns at once); 0x200/0x800 = per-frame reset bits | `0x4CF4BC`, `0x4CF4D7` |
| A2 | `fik+0x6` | u8 | bit 0x01 = **a ground contact was found this frame** | set `0x4BBB1B`, cleared `0x4BBC23` |
| A3 | `fik+0x14`, `fik+0x20` | f32 ×2 | solve weights; `sub_4CEF30` runs only if both > 5e-4 | `sub_4CF4A0` tail |
| A4 | `fik+0x740` | v4 | foot-IK ground reference point | `0x4BBBA6` (contact) / `0x4BBC1C` (no contact) |
| A5 | `fik+0x750` | v4 | **stored offset contact − entityPos**; kept on no-contact frames | `0x4BBBB9` (contact), `0x4BBC00` (zeroed only if `fik+0x19`) |
| A6 | `fik+0x760` | u32 | contact body id; 0 on no-contact frames | `0x4BBBEB`, `0x4BBC27` |
| A7 | `fik+0x19` | u8 | when set: offset zeroed on no-contact frames, and `sub_4CF4A0` re-inits leg state (`0x4CF882`) | ctor zeroes it |
| C1 | `cc+0x90` | u32 | `& 0x3F`; `== 0x15` ⇒ Branch B reset (state value name unknown) | `0x4424EC..0x4424F8` |
| C2 | `cc+0x18` (ptr), `cc+0x1E` (u16 count) | contact array, stride 64 B, flags u32 at `entry+0x28` | Branch B looks for an entry with flag & 1, else any nonzero flag (`sub_625DE0`, `ecx=cc+0x18` @`0x442558`) | — |
| C3 | `cc+0xA0` | v4 | velocity (xy magnitude sets the Branch B spring rate) | `sub_442460` |
| E1 | `opt+0x63`, `opt+0x64` | u8 ×2 | foot IK global off / foot IK only for kind 1 | `0x4538A5`, `0x4538D9` |
| E2 | entity world Z | f32 | `e` matrix translation z (what we wrote) | `sub_413460` |

### 9.3 Decision rule (evaluate on the floating guard; the control should show "none")

1. `S1 == 0` (Branch B):
   * `S2 & 1` set **and** `|S5| ≥ 0.05` **and** `S5 ≈` the visible float (sign: S5 > 0 ⇒ drawn above) ⇒ **H1**.
     Expected signature: S5 is nonzero right after our Z steps and decays toward 0 when the guard stands still.
     `S6 − E2 ≈ S5`.
   * `S2 & 1` clear, or `S4 & 0x38 < 8`, or `C1&0x3F == 0x15` ⇒ the engine resets every frame, S5 must be 0 ⇒ H1 ruled out.
2. `S1 != 0` (Branch A; S5 is always 0 here):
   * `A1 & 0x40` clear, or `E1[0x63] != 0`, or `S3 & 0x10`, or (`E1[0x64] != 0` and kind ≠ 1) ⇒ foot IK not running ⇒ H2 ruled out.
   * otherwise `A2 & 1 == 0` (no contact) for most frames **and** `|A5.z| ≥ 0.05` **and** `A5.z ≈` the visible
     float, steady while standing ⇒ **H2**. Also expect `A4.z − E2 ≈ A5.z` and `A6 == 0`.
   * `A2 & 1 == 1` with `A5.z ≈ 0` ⇒ contacts are live ⇒ H2 ruled out.
3. `S9`: `|+0xF4| ≥ 0.05` ⇒ a third offset source, not H1 or H2.
4. All of the above zero but the guard still floats ⇒ not this pass. Next suspects: the 20 `sub_4CEF30` sub-steps
   (UNREAD), or the animation itself.

The 0.05 m threshold is a suggestion to sit above noise. It is not derived from the binary.

### 9.4 When it is safe to read

* **UNVERIFIED:** whether the skeleton job graph (`sub_587F70` jobs, scheduled from `sub_729130`) runs on the
  same thread as, and strictly after or before, the Human update `sub_16FA7D0` (reached only through the data
  xref `0x26EBBE8`). Not established statically.
* Consequences:
  * 4-byte aligned scalars (S1–S7, S9, A1–A3, A6, C1, E1) are single `mov`/`movss` writes in the code above.
    A read cannot tear, but may be one frame old.
  * v4 fields (S8, A4, A5, C3) are written with `movaps`. Read them as four separate floats and treat a single
    sample as approximate. **Decide on several consecutive frames, never on one.**
  * Pointers (S1, `cc`, `fik`) change only on build/attach (`sub_431B70`, M4). They are stable while the body exists.
* Read-only reads from `OwnDrain` therefore cannot corrupt engine state. The worst case is a stale or mixed vector sample.

### 9.5 Fix seams (none implemented; each must be checked with the probe first)

**H1: yes, two seams.**
* (a) Reset the spring right after we place the body:
  `((void(__thiscall*)(void* plane, const float* point16, const float* normal16))0x0042E6C0)(comp+0x260, pos, up)`.
  Evidence: the engine does exactly this in its own reset path (`0x442519` get position → `0x442525 call`, `ecx = comp+0x260`).
  The callee ends `retn 8` (`0x42E727`), so it is callee-clean. Arg 1 is the point (it goes to `+0x50`; z to
  `+0x40/+0x48`). Arg 2 is the normal (goes to `+0x00/+0x20`). It also zeroes the velocities `+0x10/+0x30/+0x44/+0x4C`.
  **Both pointers must be 16-byte aligned** (`movaps` at `0x42E6DC`/`0x42E709`). `pos` = the matrix translation we
  just wrote, w = 0. `up` = `(0,0,1,0)`, the same value the engine passes (`xmmword_1E40AE0`, bytes dumped:
  `00000000 00000000 3F800000 00000000`).
* (b) Clear `comp+0x336 & 1` every frame. `0x4424A0` then takes the engine's own reset, and `0x453904` has
  already zeroed `+0x2C0`. One byte write. *Writers that set bit 1 again are not attributed (§4)*, so it must
  be re-applied each frame.
* Thread caveat (§9.4): if the IK job runs concurrently with `OwnDrain`, (a) can race the spring update. (b) is a
  single-byte write and the safer choice.

**H2: yes, one seam with evidence and one that is riskier.**
* (a) While `fik+6 & 1 == 0`, write `fik+0x750 = (0,0,0,0)`. The no-contact branch then sets
  `fik+0x740 = entityPos + 0` (`0x4BBC07..0x4BBC1C`). Evidence: the engine's own zeroing of this field under
  `fik+0x19` (`0x4BBBFA..0x4BBC00`). **UNVERIFIED:** that `fik+0x740` is what the foot solve uses as ground
  (its consumers in the 20 sub-steps are unread).
* (b) Set `fik+0x19 = 1`. The engine then zeroes the offset itself and `sub_4CF4A0` re-inits the leg state every
  frame (`0x4CF882`, decompile `if (v36 || fik+0x25) {… reset block …}`). Nothing seen clears it. **Risk:** foot IK
  is effectively reset every frame. Use (a) first.
* Correction to §7: `fik+4 |= 0xA00` does **not** reset `fik+0x750`. 0x200 forces 0x800 and `fik+0x28 = 0`,
  and 0x200 is cleared at the end of `sub_4CF4A0`.
* Blunt fallback for either branch on mirrored bodies: `comp+0x337 |= 0x10` skips foot IK (`0x4538AB`). It does not
  stop Branch B. Risk: a frozen last IK pose (unchecked).

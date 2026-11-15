# S1: how AC4BFMP.exe replicates Human transitions and targeted actions (static RE, 2026-11-15)

Slice S1 of `docs/design/storm-replication.md`. Read-only static RE. No game run, no live probe.

## Inputs and method
- MP: `D:\AC4-RE\ida\AC4BFMP.exe.i64` (PC, no PDB), input md5 `a82983ac8ddb00de30268e5c37e4ba51`, imagebase 0x400000.
  Opened headless (`idat -A`) with `DBFL_KILL`. The `.i64` timestamp is unchanged (2026-11-10 13:44).
- SP: a scratch copy of `D:\AC4-RE\ida\AC4BFSP.exe.i64` (2026-11-10 packed state; input md5 `2058342866688f780c8b34526a65bc35`).
  The live SP database had unpacked side files dated today, so another session holds it. It was never opened in place.
- Names map: `ops/evidence/x360-mp-2014-pdb.md` (X360 build p). X360 names are applied **only** where the PC MP
  structure matches: the same vtable slot, the same arguments, the same field use.
- Tooling: a custom IDAPython runner modelled on `ops/tools/ida_batch.py` (string xrefs, decompile, disasm, vtable dumps,
  masked-byte and token signatures). The scripts sit in the session scratchpad, not in the repo.
- Every VA below is PC MP unless marked SP. Each claim rests on the decompile or disassembly cited next to it.

## 1. RMC plumbing (verified, MP)
- **Registration:** `sub_4CE584` (NetPlayer RMCs) and `sub_4BF90E` (NetCharacter RMCs) do
  `id = sub_122D3A0(); sub_C3348C(mgr, &desc, name)`.
- **Descriptor:** static initialisers fill a 0x18-byte block:

  | Offset | Content |
  |---|---|
  | +0x00 | descriptor vtable |
  | +0x04/+0x08 | **member-function pointer of the receive handler** (this-adjust 0) |
  | +0x0C | id |
  | +0x10 | name |
  | +0x14 | unmarshal thunk |

  Example: `sub_1371AC7` writes `[ebp-8]=offset sub_4ED80B; movsd; movsd` into `dword_1A1C3EC`.
  This corrects `RE-NETCODE-ORACLE.md`, which read +0x04 as "serialize fn" and +0x14 as the name. There, `FUN_004d9140`
  is the receive handler, not a sender.
- **The thunk** (for example `sub_4F7C68`) copies the arguments out of the call object and calls the handler on the NetPlayer.

| RMC | desc | id global | handler (+4) | unmarshal (+0x14) |
|---|---|---|---|---|
| S2C_Transition(float,hnzBuffer) | 0x1A1C3E8 | 0x1A1C3F4 | `sub_4ED80B` | `sub_4CA7F9`→`sub_4C5165` |
| S2C_TransitionToLedgeClimb(u64,float,u8,AddData) | 0x1A1C400 | 0x1A1C40C | `sub_4FB91B` | `sub_4FC1CD`→`sub_4F7C68` |
| S2C_TransitFromLedgeOrClimbToDieRagdoll() | 0x1A1C418 | 0x1A1C424 | `sub_4D95FC` (not decompiled) | `sub_4CA815` |
| M2R_ReplicateEnterCustomActionState(...) | 0x1A1C838 | 0x1A1C844 | `sub_4D9140` | `sub_4D1BED`→`sub_4CAA4A` |
| M2R_ReplicateExitCustomActionState() | 0x1A1C850 | 0x1A1C85C | `sub_4D92E9` | `sub_4CA815` |
| S2C_SetMoveReplicationMode(u8) | 0x1A1B780 | 0x1A1B78C | `sub_4BFCD3` | `sub_4CA68D` |

**Sender:** `sub_4FDEF2`, reached from NetPlayer vt[10] `sub_4FF41B`. It sends S2C_Transition through `sub_4DBC27` + `sub_C31951`
(line 416) and S2C_TransitionToLedgeClimb through `sub_4FDA69` + `sub_C31951` (line 160) to each station in the NetPlayer+0x374 list.

## 2. Mapping table

Confidence:
- **verified** = decompile and disasm agree, MP-internal.
- **strong lead** = the SP function has the same operation sequence and field use, with offsets shifted consistently.
- **lead** = name or shape only.

| MP name (X360) | PC MP VA | How identified | What it does | Data carried | SP counterpart | Match method | Confidence |
|---|---|---|---|---|---|---|---|
| S2C_Transition handler | 0x4ED80B | descriptor +4 | If the entity is valid and `!vt(this+0x30)` (role), copies the buffer and calls `sub_4EA454(time, buf, 0)` | float time + hnzBuffer (§3) | none (net-only) | - | verified |
| AddTransitionBuffer / parse | 0x4EA454 | structure | Raises the max time at NetPlayer+0x324; sequence byte +0x6C (gap ≤6 → resync `sub_4DF8BD`); builds a 0x28 record; enqueues it in **TransitionManager at NetPlayer+0x2C0** (`lea ecx,[edi+2C0h]` before each `sub_4CD0xx`) | §3 | none | - | verified (X360 has the manager at +0x2F0) |
| AddTransitFromContext / …Interf / Substate / SubstateInterf / WithFn / InterfWithFn (master) | 0x72FDE3 / 72FE48 / 72FEB0 / 72FF18 / 72FF83 / 72FFEE | each calls one of `sub_4D50AC..4D5286` → `sub_4CCFC0/4CCFE6/4CD03D/4CD067/4CD0C4/4CD0FB` (kinds 0-5) | Records (ctx, funcIdx[, substate][, param][, fn]) with engine time, gated by `sub_B2B4F8` and actor+184 != 0, actor+188 == 0 | ctx id, 1-based funcIdx | none (SP transitions do not record) | - | verified for kind and fields; names are leads |
| per-context record wrappers (Ground = ctx 4) | 0x98FCFE / 98FD1E / 98FD41 / 98FD67 | `push 4` + call to the recorders | Called first thing in each Ground transition function, e.g. `sub_996B1F: push 1; call sub_98FCFE` | - | - | - | verified |
| **NetProcessReceivedTransitions** | NetPlayer vt[18] **0x4FAEE8** (vt 0x139BD08) | X360 slot 18; walks the records | For each due record (time ≥ NetPlayer+0x300 window, ≥ +0x70×1000): `ctx = AI->vt[+0x1C](layer, rec.ctx)` (layer 4 if ctx 25, else 1; 17/21 skipped). If the current ctx id on the layer > 2, calls the context slot for the kind (next row) | record | none | - | verified |
| HumanGround ctx vtable | **0x13FAC00** (ctor `sub_9DD955`; slot 17 `sub_9A4A27` = "HumanGround") | name getter | slots 18-29 present (next row) | - | SP **0x026E8AE8** (ctor `sub_16D8690`): slots 18+ are other code; slot 25 = 0 | slot dump of both | verified (MP); SP has no executors (dump) |
| Exec slots 18-23 | 18 `0x996959` ExecRemoteFunc(i); 21 `0x996975` …Interf(i,FSMParam*); 19 `0x9D050D` ExecSubstateRemoteFunc(s,i); 22 `0x9D0845` …SubstateInterf; 20 `0x996994` …SubstateWithFn; 23 `0x9969B3` …InterfWithFn | dispatch in 0x4FAEE8: kind0→+0x48(idx-1), 1→+0x54(idx-1,param), 2→+0x4C(sub,idx-1), 3→+0x58(sub,idx-1,param), 4→+0x50(a,b), 5→+0x5C(a,param,b) | 18/20/21/23 call `table[i]` (method-pointer pairs, `off_18B2560/2580/2570/2590`). 19/22 switch on substate to a sub-object table (e.g. case 0x5F: this+0x354, `off_18B2734`) | - | none | - | verified |
| transition table builder | HumanGround slot 15 **0x9E81EF** | 358 `sub_D3053D(&fn,1,0)` appends | Fills the tables with the context's transition functions. **For every entry whose first call is the Ground recorder, the pushed funcIdx = position+1 (kind-0 block, entries 0-99) or position-99 (kind-1 block)**: e.g. [0] 0x996B1F→1, [98] 0x9C3A32→99, [100] 0x9CF4C9→1, [311] 0x9B385A→212. **So the duplica runs the same transition function the master ran.** | - | SP slot 15 `0x1D80950` is not a builder (it allocates 0xA80, ctor `sub_1D22EC0`, then calls `sub_1D65BB0`) | dump | verified (MP); SP index map UNPROVEN |
| UpdateNetContext / UseNetContext / Fill/UseNewRelevancyBuffer / Net{Replicate,Read}AdditionalInfoForTransitions | slots 24 `0x99A51A`, 25 `0x9C1FD6`, 26 `0x9B16A2`, 27 `0x9AA48B`, 28/29 **nullsub_7** | slot order | **HumanGround sends no additional transition info** (28/29 empty). Slot 26/27 pack and unpack Ground flags (+0xA60 bit 2, +0xB34 bit 5, …) for join-in-progress | relevancy u16 flags | none | - | verified |
| TransitToContext(id) | **0xBA323D** | structure | `layer=this[0xFC]`; `new=fac->vt[+0x1C](…,id)`; `old->vt[+0x34]` (exit); prev id; `new->vt[+0x30]` (enter) | ctx id | SP **0x52DF90** (`layer=this[0x100]`, factory vt[+0x20], exit +0x34, enter +0x30) | same sequence, offsets +4 | strong lead |
| TransitToContext(id, functor[, arg]) | 0xBA328E / 0xBAC787 | as above, plus `functor->+0xC()` between exit and enter | Used by every Ground transition (e.g. `sub_996B1F` → (6, nullsub)) | - | SP has no single function. SP transitions inline **Exit 0x52C330 → functor → Enter 0x52DFF0(id)** (e.g. SP `sub_1D9FEB0`→Enter(6), `sub_1DA0A40`→Enter(4), `sub_119F830`→Enter(6)) | SP decompiles | lead (composite) |
| GetCurrentContextID | 0xBA313D | returns 2 if none, else `ctx->vt[+0x24]()->+4` | | | SP **0x52C2E0** (same, returns 2) | same body | strong lead |
| GetContext(id) on the current track | factory vt[+0x1C] | - | | | SP **0x52C360** (factory vt[+0x20]) | same use | strong lead |
| S2C_TransitionToLedgeClimb handler | **0x4FB91B** | descriptor +4 | Valid entity → time max (+0x324); sequence check (+0x6C); if `+0x324+0.5 ≥ +0x68`: `sub_4F7CA2(wire→0x1A0 runtime)` then `sub_4F8F39` pushes `{rt*, u64}` into a 32-slot ring at **NetPlayer+0xBBD0**. If byte +0x371 == 0, it first flushes (`sub_4F54ED`) | §4 | none | - | verified |
| ledge-queue consumer (duplica) | **0xAE4881**, called by 0xAE4D55 / 0xAE4F42, which are reached from vt **0x14014D0** slots 14 / 12 (slot 17 name "DistantLedgeClimb") | xrefs | Pops entries due at `now - [dword_1AE1C3C+0x44]` (`sub_4CD1EE`). Applies the attach (§4): `sub_C6E0E3(rt+0x30, rt+0x38, …, 0xFFFF)`, `sub_77E5A0(…, rt, &pos, …, rt+0x10, rt+0x20, rt+0x158, …, rt+0x40)`, IK per limb `sub_7F15DB/7F177D(i, rt+0x50+64*i)` | rt struct | none (net-only). The engine calls inside it are SP targets | - | verified (MP). That the context is "DistantLedgeClimb" rests on the vtable name only |
| (action setter used by the consumer) | 0xC6E0E3 (1008 xrefs) | structure | `if this[2] != sentinel` (master net hook) → `sub_C6D810(a2..a5, 0)` | action ids | SP **SetTargetAction 0x6625D0** (`this[2] != dword_2AC1408` → `sub_65DCB0(a2..a6, 0)`) | same shape; argument count differs by 1 | strong lead |
| other ledge-queue users | 0xAC50FC, 0xAC7CFF (`lea …,[+0BBD0h]`); master producers to NetPlayer+0xBF00: 0xAAFE4C, 0xA82E9A | displacement sweep `+0BBD0h`/`+0BF00h`, 0x400000-0x1300000 | not decompiled | - | - | - | lead |
| M2R_ReplicateEnterCustomActionState handler | **0x4D9140** | descriptor +4 | Ground ctx = `AI->vt[+0x1C](1,4)`; CustomAction = `sub_E862E2(&unk_1B04040, u32 arg)`; normalises the 3rd vec; `sub_4C7566` → HumanGround **+0x180** pending block, word **+0x1D0 = 0x0101** (pending=1, duplica=1) | Entity*, vec4, u32 id → CustomAction*, bool, vec4, vec4 | none | - | verified |
| Exit handler | 0x4D92E9 | descriptor +4 | Ground ctx word +0x1D0 = 0x0001 | none | none | - | verified |
| custom-action consumer (duplica) | **0x9F30EE** (called from 0x9F4F2C) | reads +0x1D0/+0x1D1/+0x180.. | IHumanGround(+0x14) vt+0x5A0 (slot 360) false; vt+0x5E8 (slot 378, X360 CanEnterCustomAction)(CA) true. Builds an FSMParam {CA* t0x26, Entity* t0x24, vec t0x14, bool t0, vec, vec}; +0xF08 = bool; +0xF04 = Handle(CA); **`sub_9EE517(this, 0x49, &param)`**. Exit: vt+0x5F0 (slot 380), then `sub_9EE517(this, 0x4A, 0)` | pending block | SP not found | masked-byte search failed | verified (MP); SP UNPROVEN |
| HumanGround event dispatch | 0x9EE517 | dispatch on current state byte +0x1D4 against the state ids written by slot 15 (+0x220, stride 3) | Statechart event (0x49 enter custom action, 0x4A exit) | event id, FSMParam* | SP not found | masked bytes; SP state-id regex found 0 hits | UNPROVEN |
| S2C_SetMoveReplicationMode handler | 0x4BFCD3 | descriptor +4 | `this[230] = mode` (NetCharacter+0x398). NetProcess (0x4FAEE8, NetCharacter vt[18] 0x4D506D) calls vt[+0xA4] (slot 41) when +0x394 != +0x398 | u8 mode | none | - | verified (X360 +0x3D4/+0x3D8; the PC offsets are 0x40 lower) |

## 3. S2C_Transition hnzBuffer layout (`sub_4EA454`, verified)
`u8 kind | u8 seq | f32 time_s (→ u64 ms) | u8 ctx | per kind:`
- kind 0: `u8 funcIdx`.
- kind 1: `u8 funcIdx` + FSMParam.
- kind 2: `u8 substate, u8 funcIdx`.
- kind 3: `u8 substate, u8 funcIdx` + FSMParam.
- kind 4: `u8 a, u8 b`.
- kind 5: `u8 a` + FSMParam + `u8 b`.

FSMParam (`sub_7381F1`): `u8 present`; if present, `u16 len` and `len` raw bytes. It goes into a 36-byte param object (`sub_4E8581`, vtable `off_139A684`). On execution `sub_737B8D(param+32)` makes a typed clone.

Record (0x28 bytes, ctors `sub_72DBFC/72DC50/72DC23/72DC7D`):

| Offset | Field |
|---|---|
| +0 | param* |
| +4 | funcIdx (1-based) |
| +8 | b |
| +0xC | kind |
| +0x10 | ctx |
| +0x14 | next |
| +0x18 | substate |
| +0x20 | u64 ms |

The TransitionManager list header is at mgr+0x1C (head), +0x20 (tail), +0x24 (count), under a lock (`sub_D8D46B/474`), in `sub_4C7693`.

## 4. RMCHumanLedgeOrClimbAddData (wire 0x148 B) → runtime attach struct (0x1A0 B)
**Source:** wire copy-ctor `sub_4F7570` and converter `sub_4F7CA2`. The runtime ctor is `sub_4DDB95`, allocated with 16-byte alignment and size 416 (`sub_744AF8`). The fields are used in `0xAE4881`.

RMC arguments:
- `u64`: the master's engine time (`sub_C1EE5F`) at send (sender line 150). It is the ring key; the duplica plays the entry back after the delay `[dword_1AE1C3C+0x44]`.
- `float`: NetPlayer+0x68.
- `u8`: sequence byte NetPlayer+0x6C.

The sender fires only while the master's context on track 1 is **7, 8 or 9** (`sub_4FDEF2` lines 135-139).

| wire | runtime | type | use on duplica (0xAE4881) |
|---|---|---|---|
| +0x000 / +0x010 / +0x020 | +0x00 / +0x10 / +0x20 | 3 × vec4 (geometry). The runtime default for +0x00 is `xmmword_1397D00`; for +0x10/+0x20 it is `xmmword_1397CD0` | args of `sub_77E5A0` (with rt+0x10, rt+0x20) |
| +0x030..+0x040 | +0x30..+0x40 | 5 × u32 | +0x30/+0x38 → `sub_C6E0E3` (action ids, lead); +0x3C == -1 or == +0x30 picks the branch; +0x34 → anim param bits; +0x40 → `sub_77E5A0` |
| +0x044 + 48·i, i = 0..3 | +0x50 + 64·i | **limb item**: vec4 +0, vec4 +16, **+32 ref-counted handle to a world object** (`sub_4C9051` does `lock xadd [p+8]`); u32 +36; f32 +40; u8 +44/+45 | `sub_4C55F6` resolves +32 with `sub_B0C047`: a pointer is kept only if the object's +0x6C != 0. Per-limb IK: `sub_7F15DB/7F177D(i, item)` with `sub_78CD28(i)` |
| +0x104..+0x10B | +0x150..+0x157 | 8 × u8 flags | +0x150 sets ctx+0x1C; +0x151 copies +0x153..+0x157 into obj+0x80 bits; +0x152 turns on limb IK |
| +0x10C / +0x110 / +0x114 | +0x158 / +0x15C / +0x160 | u32, u32, f32 | +0x158 → `sub_77E5A0`; +0x15C → `sub_C5EE57`; +0x160 > 0 = blend time |
| +0x118 / +0x119 | +0x164 / +0x165 | u8 ×2 | anim param bits (&7, &3) |
| +0x11C (8 B) / +0x124 | +0x168 / +0x170 | array of 4-byte elements + u8 | `sub_7F1545(100, rt+0x168, rt[0x170])` |
| +0x128 (8 B) | +0x174 | array of 4-byte elements | `sub_C5F015` |
| +0x130 / +0x131 | +0x17C / +0x17D | u8 / bool | +0x17C → ctx+0x2A0; +0x17D enables +0x180/+0x190 |
| +0x134 | +0x180 | vec4 | `sub_409D07(rt+0x180)` |
| +0x144 | +0x190 | u8 → f32 = byte × 0.001 | `sub_CD79F5` time |

**Pointers vs geometry:** the only object references are the four limb `+32` handles. The duplica resolves them to *its own* object pointers (`sub_B0C047`). How they cross the wire (object id or re-query) is UNVERIFIED: the call-object serializer behind descriptor vt `0x139BF8C` → `sub_4FED9B`/`sub_4FED78` was not decoded. Everything else is plain geometry, ids and flags. The two 4-byte arrays have unverified element meaning.

## 5. Ground steering on the duplica
NetPlayer vtable slots (match X360 slots 19/20/24):
- vt[19] `0x4C8F38` getSteeringData: target from `sub_72DEA0`, speed from NetPlayer+0x1108.
- vt[20] `0x4C8F66` setSteeringData: dataset at **+0x1F8** (X360 +0x220).
  - Target X and Y are stored as `short ×100` at +0x226 / +0x228.
  - Speed is set through `sub_7300FB`.
  - The dataset is marked dirty at +0x1FC only on the master (`vt(this+0x30)` true and !+0x48).
- vt[24] `0x4D949D` UpdatePositionFromNet, on the duplica (+0x371), dispatches on the current context:
  - ctx 4: the HumanGround context's **slot 25 UseNetContext `0x9C1FD6`** applies the steering.
  - ctx 10/16: that context's slot 25.
  - ctx 11: `sub_B1EAF9(entity, NetPlayer+0x1100)` sets the position directly.

`GetExtrapolated` was not located on PC: UNPROVEN.

S2C_SetMoveReplicationMode stores the mode only (§2). The FULL_MOVE / STEERING_ALONE / STEERING_BLENDED switch lives in NetPlayer vt[41] `0x4E3C7C`, which was not decompiled.

## 6. Negatives and gaps
- **SP byte match of MP functions: NEGATIVE.**
  - METHOD: masked-byte regex (rel32, absolute addresses and image-range immediates wildcarded) of 31 MP functions over SP `.text` 0x401000+0x1A39000, plus a prefix binary search.
  - COVERED: TransitTo*, GetCurrentContextID, the ledge consumer and its callees, the custom-action consumer, HumanGround dispatch and ctor, 4 Ground transitions.
  - NAMES: n/a.
  - Result: 0 full hits. The best prefixes are ≤24 B (noise). The SP build is laid out differently, so behaviour/structure matching is needed.
- **SP HumanGround net slots absent: NEGATIVE for SP vtable 0x026E8AE8.**
  - METHOD: slot dump 0-44 of both binaries.
  - COVERED: SP vtable 0x026E8AE8 and MP vtable 0x13FAC00, slots 0-44.
  - NAMES: none needed.
- **SP strings "HumanGround" and "DistantLedgeClimb": absent; "HumanLedgeData" present (0x26DBE84).**
  - METHOD: exact NUL-terminated byte search, case-sensitive, all segments.
  - COVERED: the whole SP image.
  - Consequence: SP context vtables must be found through structure, not names.
- **UNPROVEN:**
  - SP state-id builder: the regex `C6 8x disp32 01/02/03` gave 0 hits in SP `.text`. Other encodings (`mov word`, register sources) were not covered.
  - SP counterpart of `0x9EE517` and of the transition tables, so the MP funcIdx → SP function map.
  - The kinds 4/5 "fn" field meaning.
  - The 0x1A0 attach-struct producer on the master (`0xAAFE4C`, `0xA82E9A` not decompiled).
  - D2M_SendDamageKillEvent, M2All_WantsKill / WantsInteract, S2C_TransitFromLedgeOrClimbToDieRagdoll (`0x4D95FC`): only registration was read.
- **Leads:** MP context-id 7/8/9 = Ledge/Climb/? (X360 enum: 7 Ledge, 8 Climb). PC-SP runtime context 4 = Ground matches MP 4 (`phase1.cpp:1708`).

## 7. Next steps (what a probe should confirm)
1. On SP: find the SP Ledge/Climb context whose Update/Enter consumes a struct with the §4 runtime layout. Hint: ctx+0x2A0 written from a byte, and a 4-limb loop calling IK setters. Then dump that struct live on Edward during a real climb, as a differential (two ledges), to confirm +0x00/+0x10/+0x20 geometry and the 4 limb handles.
2. Decode the call-object serializer (descriptor vt `0x139BF8C` slot 0 → `sub_4FED78` vtable) to learn how the limb handles travel.
3. Match SP SetTargetAction 0x6625D0 ↔ MP `0xC6E0E3` live: log the (+0x30, +0x38) values of a climb against the puppet's SetTargetAction arguments.
4. S2 owns the IHumanGround slots. On MP PC the slot numbers equal X360 (360, 378, 380 used by `0x9F30EE`). On SP they are expected to shift (+18 per PDB chunk 1): UNVERIFIED.

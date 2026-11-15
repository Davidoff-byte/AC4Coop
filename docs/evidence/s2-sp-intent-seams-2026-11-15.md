# S2: SP intent seams — IHumanGround slots, AI-off, transition attach data (static, 2026-11-15)

**Inputs.**
- Binary: `AC4BFSP.exe` (PC 1.05), md5 `2058342866688f780c8b34526a65bc35` (IDA header of every job), image base 0x400000.
- Database: `D:\AC4-RE\ida\AC4BFSP.exe.i64`. I copied it byte for byte to a private path (`D:\ac4ida\s2\`) so that it
  could not collide with the other agent's IDA session. Every job opened it with `DBFL_KILL`, so nothing was saved.
- PS3 SP debug ELF: `D:\Games\acbf_ps3_gold\scimitar_final.elf`. I used its `.symtab` vtable symbols, demangled with c++filt.
  It gives **names and slot order only**.
- Jobs and raw output: session scratch `s2\v01..v15.{py,txt}`, `hg_vt.txt` (the PS3 HumanGround vtable, 2413 entries),
  `ihg_slots.txt`, `align.txt`.

**Method.**
1. Read the PS3 vtable `_ZTVN8scimitar11HumanGroundE`, every secondary group (offset-to-top markers). The IHumanGround
   group is at +0x1248, offset-to-top -0x10. That matches the PC vptr at HumanGround+0x10: the ctor `sub_16D8690`
   stores 0x026E7DC0 there (0x016D8767).
2. Dumped the 841 PC slots of 0x026E7DC0 with each target's `retn N`.
3. Aligned the PS3 order to the PC order with Needleman-Wunsch. The score is the expected argument bytes from the
   demangled signature against the PC `retn` (`align.txt`).
4. Decompiled every target slot and the FSM function it forwards to.

**No live probe was run** (the game is in use). The S2 oracle needs decompile plus a live probe, so every new slot below
is a *strong lead*. Only slots 0 and 1 are verified (`pilot-intent-2026-11-15.md`).

## Q1. IHumanGround and sibling interfaces: PC slots

**Alignment result.**
- PC slot = PS3 SP slot for slots 0..275. Checks:
  - The 7 slots that `ops/evidence/pilot/intention-seam.md` already matched.
  - The `retn` byte count matches the PS3 signature for every slot from 139 to 172.
- From about slot 276 on, PC slot = PS3 slot − 1:
  - GetFightStage: PS3 301, PC 300 = byte 1200.
  - GetHumanGroundStage: PS3 690, PC 689 = byte 2756.
  - Both PC byte offsets equal the ones in `x360-mp-2014-pdb.md` part 3.
- The exact one-slot gap sits among the 0-argument slots 271..276 and is not pinned. No target slot is in that range.

**Wrapper pattern (every Can/Do below).**
- `ecx` = IHumanGround* = HumanGround+0x10.
- The FSM is created lazily at `[ecx+0x90]` (= HumanGround+0xA0, `sub_1D22EC0`, 0xA80 bytes).
- Can: `return FSM::TestInterfaceTransitions_X(args) == 2`.
- Do: `FSM::ExecuteInterfaceTransition_X(args)`. The names are the PS3 `fsm_humanground::HumanGroundFSM::` symbols.

**Two rules the FSM code enforces.**
- **The Do needs its Can first.** A Can that succeeds appends its transition id to the list at FSM+0xE0 (`sub_59C660`).
  The Execute function calls `sub_15DFE50(&id)` and returns 0 unless that id is in the list. Examples: DoEnterClimb
  checks id 131; CanEnterClimb `0x1D5A500` adds 131 with `sub_59C660(fsm+0xE0, 131)`.
- **The source state is checked.** Execute compares the active-state path at `*(FSM+0x118)` (u16 array) against fixed
  source states, for example `path[3] == 4`.

| method (PS3 SP signature) | MP X360 slot | PS3 SP slot | PC slot | PC VA | evidence | confidence |
|---|---|---|---|---|---|---|
| DoJumpToTarget2(FootHold&,LandingIntention,FootHold&,LandingIntention,bool) | 4-7 family | 4 | 4 | 0x01D7CF60 | retn 14h; forwards 5 args to Execute `0x1D6AFD0`, which has 4 transitions as PS3 `_On_JumpToTarget` has (Move→InAir target 650 = context 6) | strong lead |
| CanJumpToTarget() (iface HumanGround+0x14, vt 0x026E7DB8) | 4 | +0x14 iface slot 0 | +0x14 slot 0 | 0x01D2BE70 | **Not a function in the idb**: defined it (add_func True, size 0x58, plain retn) | strong lead |
| DoJumpToTarget(FootHold&,LandingIntention,bool) (iface +0x14) | 5 | +0x14 slot 1 | +0x14 slot 1 | 0x01D7CE30 | retn 0Ch; calls `0x1D6AFD0(fh, li, <empty FootHold>, 0x80000000, b)` | strong lead |
| CanEnterLadder(Entity*,Vec4&,Vec4&,float,float,bool*) | 20 | 17 | 17 | 0x01D2AB80 | retn 18h (6 args); Test `0x1CF6350`==2 | strong lead |
| DoEnterLadder(Entity*) | 21 | 18 | 18 | 0x01D7BF10 | retn 4; Execute `0x1D6A980`: path[3]==4 (Move), transition 132, action `0x1CD0020`, target 649 = context 5 | strong lead |
| CanEnterWallingVertical(Vec4&,Vec4&,Handle&,int&,const Vec4&) | 24 | 21 | 21 | 0x01D5E5C0 | retn 14h; Test `0x1D5D920`==2 | strong lead |
| DoEnterWallingVertical(const Vec4&,const Vec4&,bool) | 25 | 22 | 22 | 0x01D7BDE0 | retn 0Ch; Execute `0x1D6B3E0`: 5 transitions (PS3 has 5), target 652 = context 8 | strong lead |
| CanEnterFight(const Handle<Entity>&) | 95 | 75 | 75 | 0x01D269F0 | `call sub_1D02DF0` @0x01D26A40, `cmp eax,2` @0x01D26A47 | strong lead |
| DoEnterFight(const Handle<Entity>&) | 96 | 76 | 76 | 0x01D79A50 | `mov ecx,[esi+90h]; call sub_1D65D40` @0x01D79B14/1B. Execute: source state 4 / 88 / 135, each target state 0xA3 (163); same count as PS3's 3 `_To_Fight_On_EnterFight`. Sets, then clears, bit 4 of `[scope+0x3A8]` around the call | strong lead |
| CanEnterFightAttackOnGround(WeaponType) | 127 | 104 | 104 | 0x01D2DB60 | retn 4; Test `0x1CF8D10`==2 | strong lead |
| DoEnterFightAttackOnGround(WeaponType,const Handle&) | 128 | 105 | 105 | 0x01D7E4C0 | retn 8; Execute `0x1D76120` | strong lead |
| SetFightFocusTarget(const Handle&,bool,bool) | 129 | 106 | 106 | 0x01D4D0F0 | retn 0Ch; `0x18674C0` on fight obj `[[[fsm+0xC]+0xC]+0x74]` (handle, 2 bools) | strong lead |
| GetFightFocusTarget() | 130 | 107 | 107 | 0x01D4D180 | returns an AddRef'd copy of the handle at fight+0x1C8 through a hidden return pointer (retn 4) | strong lead |
| ReleaseFightFocusTarget() | 131 | 108 | 108 | 0x01D4D210 | releases fight+0x1C8, stores the null handle `dword_4DD5F8C` | strong lead |
| CanExitFight(ExitFightType) | 132 | 112 | 112 | 0x01D26B10 | Test `0x1CF5530`: path[4]==167, transitions 450/451/452 (PS3 has 3 `FightMovement_To_*_On_ExitFight`) | strong lead |
| DoExitFight(ExitFightType) | 133 | 113 | 113 | 0x01D79C30 | Execute `0x1D65F30` | strong lead |
| Attack(Entity*,bool,WeaponType) | 163 | 154 | 154 | 0x01D503A0 | `mov ecx,[esi+74h]; call sub_1836EC0` @0x01D5041C/1F, retn 0Ch. `0x1836BB0` writes a fight request: target handle from `Entity+0xC8` → req+0xB30, weapon → +0xB3C, gated on req+0xB7C==34 | strong lead |
| Attack(Vec4,bool,WeaponType) | — | 155 | 155 | 0x01D50310 | → `0x1836E80` | strong lead |
| PerformCombo(Entity*,uint,ComboEndCondition,bool) | 168 | 160 | 160 | 0x01D505E0 | retn 10h; `0x1861540` writes target handle +0xB30, count +0xB70, end condition +0xB74 | strong lead |
| CanAttackNow() | 171 | 163 | 163 | 0x01D26EF0 | **A stub on PC: always false.** Disasm: lazy FSM create, then `xor al,al; retn` (0x01D26F2F / 0x01D26F3B). The same function sits in slot 274 | strong lead |
| DoAttackNow() | 172 | 164 | 164 | 0x01D2DF30 | **A no-op on PC.** Lazy FSM create only (0x01D2DF6F retn). Shared with slot 275 | strong lead |
| CanAssassinate(AssassinationSituation&,bool,Vec4&,Vec4&,uint&,bool,bool,bool) | 40 (AC2MPConflict&) | assassination iface (HG+0x74) slot 0 | HG+0x74 vt 0x026E7998 slot 0 | 0x01D5E370 | retn 20h (8 args); Test `0x1D5DAD0`==2 | strong lead |
| DoAssassinate(AssassinationSituation&,uint,bool,bool) | 41 | HG+0x74 slot 1 | same vt, slot 1 | 0x01D790F0 | retn 10h; Execute `0x1D6BB80` | strong lead |
| CanEnterCustomAction(CustomAction*,CustomActionPriority) | 378 | ICharacterCustomAction (HG+0x70) slot 1 | HG+0x70 vt 0x026E79B8 slot 1 | 0x01D2C410 | retn 8; Test `0x1D1D4F0`==2 (another branch: `0x1D93170`) | strong lead |
| DoEnterCustomAction(CustomAction*,CustomActionPack*,const DoEnterCustomActionParams&) | 379 | HG+0x70 slot 0 | HG+0x70 slot 0 | 0x01D7D6A0 | retn 0Ch; stores params+0x30 byte → scope+0x1BD8 and the action handle → scope+0x1BD4, then Execute `0x1D6B670` or `0x1D9BD30` | strong lead |
| CanEnterClimb() / DoEnterClimb() | — | 383 / 384 | 382 / 383 | 0x01D5A500 / 0x01D7BEB0 | Can: path[3]==4, Test `0x1D4B070`, adds id 131. Do: tail-jumps to `0x1D6B5F0` (target 674 = context 30) | strong lead |
| CanFallOffEdge / DoFallOffEdge(GapInfo&,…) | — | 357 / 358 | 356 / 357 | 0x01D4BAC0 / 0x01D7CC10 | retn 0Ch each; Do → `0x1D6CC70` | strong lead (by alignment only) |

**SP and MP signatures differ.**
- SP has no `Can/DoAssassinate(AC2MPConflict&)` in IHumanGround. The SP form is the separate assassination interface
  at HumanGround+0x74, with different arguments.
- SP has no `Can/DoEnterCustomAction(CustomAction*,Entity*,Vec4&,bool)` either. The SP form is ICharacterCustomAction at
  HumanGround+0x70.
- How the code reaches these interfaces from an IHumanGround* `p`:
  - The +0x14 interface is `p+4`.
  - The +0x70 interface is `p+0x60`.
  - The +0x74 interface is `p+0x64`.
  - The PC vptr offsets equal the PS3 thunk offsets (ctor stores at 0x016D8775 / 0x016D880F / 0x016D8816, final values).
- State ids (lead, by intersecting the source-state sets of the PC Execute functions with the PS3 transition names):
  4 = Move, 135 = Acting, 88 = RangedWeaponsReloading, 67 = JumpImpulsion, 163 = Fight, 167 = FightMovement.

## Q2. AI-off seam

**Chain (static).**
1. `0x0054CB90` is AIComponent::UpdateActor.
   - It is slot 48 (+0xC0) of BhvGenericNPC vt 0x026E34D8. The same pointer is in 5 AIComponent vtables
     (data refs 0x01E46838, 0x01E4FB70, 0x02687228, 0x026E3598, 0x026FA958).
   - PS3 order: CharacterBehavior → AIComponent. Enter / Reset / Exit / UpdateActor / UpdateSingleThreaded /
     UpdateMultiThreaded / UpdateAIInteractionMultiThreaded / Shutdown(World*) / OnUserDelete / SwitchTo…(World*) ×2.
   - The PC slots 45..55 match those by their `retn` and their bodies:
     - slot 47 calls actor vt+0x3C (Exit);
     - slot 52 retn 4 (Shutdown);
     - slots 54 and 55 retn 4.
   - Body: `if (actor = *(this+0x24)) actor->vt[+0x38]()`.
2. Human vt 0x026EBBB0 (ctor `sub_1701C20`, store at 0x01701C2F) slot 14 (+0x38) = **0x016FA7D0 = Human::Update**.
   - PS3 `Human::Update()` is at PS3 slot 15. GCC emits 2 destructors where MSVC emits 1, so PS3 15 = PC 14.
   - Its only xref is that vtable entry, 0x026EBBE8.
   - Human::Update calls `sub_52DF80` at 0x016FA857. `sub_52DF80` → `sub_52C220(0)`: 5 tracks at Human+8, stride 0x30,
     each `ctx->vt[+0x3C](0)`. The HumanGround context's vt+0x3C → FSM dispatch.
   - **So `sub_52DF80` is the native context FSM, not the AI decision layer.** The `unity-netcode-mapping.md` item 1 lead
     points at the layer that must keep running.
3. The other AIComponent update slots:
   - UpdateMultiThreaded `0x00550090` (slot 50) calls its own vt+0xC0 (UpdateActor) when `*(this+0x34)==1`.
   - UpdateAIInteractionMultiThreaded `0x018EA300` (slot 51) calls its own vt+0xC8.
   - UpdateSingleThreaded `0x0055DEF0` (slot 49; vtable 0x026FA898 reaches it through thunk `0x017FFD60`):
     - runs the component's statechart object at this+0x68 (vt+8; first-time init with callback `sub_556520`);
     - then `sub_5564C0` (process list at this+0x8C), `sub_556420`, `sub_556170` (visibility / LOD flags at this+0xBC).
4. What disabling the behaviour component does:
   - Component::SetEnabled `0x007130C0` only toggles bit 0x4000000 of comp+4 (an interlocked CAS loop). It has no other
     effect.
   - CharacterBehavior::GetEntityComponentCategories `0x004AE410` pushes categories {3, 4, 6} without reading that bit.
   - **Where the AI scheduler reads that bit: UNPROVEN** (see Negatives).
   - Live evidence that already exists (crash stacks quoted at `phase1.cpp:6666-6677`, `guard-statechart-2026-11-13.md`):
     Human::Update → `sub_52DF80` ran on guards whose BhvGenericNPC was disabled. So disabling does not stop UpdateActor
     on guards. Not shown for the puppet.

**Recommended seam (hypothesis, UNVERIFIED).**
- What: per-component skip of **AIComponent::UpdateSingleThreaded `0x0055DEF0`**. Hook it, compare `ecx` with the
  replica's behaviour component, and return. Leave UpdateActor and UpdateMultiThreaded alone, so that Human::Update
  (contexts, HumanGround FSM, locomotion, actions) keeps running.
- Why: in the static chain, slot 49 is the only AIComponent update that does work besides driving the actor (statechart
  at +0x68, process list). UpdateActor is the only path into Human::Update found so far.
- Fallback: the `SetEnabled(false)` the plugin already uses. Guards show that their FSM continues under it.
- Risks:
  - (a) The decision layer could also run from UpdateAIInteractionMultiThreaded, or from outside the component (AI
    manager tasks). Not shown either way.
  - (b) The +0x68 statechart might own state the FSM needs (it is initialised with a callback).
  - (c) `0x0055DEF0` is shared by every NPC and by the player-type vtable, so the filter must compare the component
    pointer.
  - (d) `UpdateActor` runs only when `*(this+0x34)==1` in the multi-threaded path. The other caller set of UpdateActor
    is not enumerated.

## Q3. Transition entry and attach data

**The context switch API** (all thiscall on the Human / AIActor):
- `TransitToContext` **0x0052DF90**: `int __thiscall(AIActor*, int contextId)`, retn 4. **It takes no attach argument.**
  - track = Human+8+0x30·`[Human+0x100]`.
  - `new = factory(track+0x28)->vt[+0x20](*(track+0x2C), id)`, then `old->vt[+0x34]()`.
  - track+0x10 = the old type id, track+0x14 = new, track+0 = 1; returns `new->vt[+0x30]()`.
  - Only caller: 0x0169AB28 (`sub_169AB20` = TransitToContext(3)).
- `EnterContext` **0x0052DFF0**: the same without the exit. 84 callers, including the FSM `sub_1D659D0` @0x01D65A0C.
- `ExitCurrentContext` **0x0052C330**: `old->vt[+0x38]()`, then track+0x10 = the type id.
- `sub_52C360` **0x0052C360** = `factory->vt[+0x20](*(track+0x2C), id)`: it gets the context object for an id **without
  entering it**.

**How a native entry passes the data.**
1. The IHumanGround `Do*` calls `FSM::ExecuteInterfaceTransition_*`. When the target is ≥ 0x284 (a target outside the
   FSM), it calls ExitCurrentContext and then the generated transition action.
2. The action fetches the target context with `sub_52C360(id)`.
   - It resets that context's **pending-transition record** to type -11.
   - It fills the record with type 0, an entry-function pointer and the payload.
3. Then `sub_1D659D0(target)` → `EnterContext(human, target-644)`.

| context (id) | IHumanGround Do → Execute → action | record location | payload written (writer) |
|---|---|---|---|
| Ladder (5) | slot 18 → `0x1D6A980` → `0x1CD0020` | `*(*(ctx+0x2C)+0x68)` | +0 = 0, +8 = fn `0x1CE86A0`, +4 = ladder handle `Entity+0xC8` (`0x1643670`); reset by `0x1783DA0` |
| InAir (6) | slot 4 / +0x14 slot 1 → `0x1D6AFD0` → `0x1CEB790` (Move→InAir) | `*(*(ctx+0x2C)+0x6C)` | reset by `0x1DA2250`. FootHold and LandingIntention go to `0x1CE2260` (ecx = scope) → `0x170E560`, then `0x1DA2220`. The fill itself was not decompiled |
| Walling (8) | slot 22 → `0x1D6B3E0` → `0x1CCFF90` (Move→Walling) | `*(*(ctx+0x20)+0xF0)` | `0x1712770`: +0 = 0, +0x40 = fn `nullsub_26`, +0x10 = Vec4 arg1, +0x20 = Vec4 arg2, +0x30 = 1, +0x34 = 0, +0x38 = bool |
| Climbing (30) | slot 383 → `0x1D6B5F0` (id 131) → `0x1D437B0` | `*(*(ctx+0x2C)+0x520)` | `0x157E610`: +0 = 0, +0x250 = fn `0x1577AE0`, +0x10 = copy (`0x157C1C0`) of a **default-built** local object (vptr 0x026CC104, 164 B) |

**Consequences for the crash at `phase1.cpp:114-121`.**
- Entering 5, 8 or 30 with TransitToContext / EnterContext skips the record write. The entering context then reads an
  unset (-11) or stale record. Strong lead; the consuming enter code was not decompiled.
- Climbing gets no geometry through its record (the payload is default-constructed). CanEnterClimb's test `0x1D4B070`
  runs a detection on the ground FSM scope's climb data:
  - `sub_16FEAE0(scope+0x270, scope+0x260, 0)`;
  - 4 entries of 96 B at `*(scope+0x260)`;
  - Vec4 at scope+0x280.

  So the climb attach data is computed on the ground FSM (Human-local), not passed to the context. UNVERIFIED that the
  Climbing enter reads it.
- **Path to use for S6:** drive entries through the IHumanGround Can/Do pairs on the replica. They write the records
  natively. Raw TransitToContext does not. For a jump, the data is FootHold + LandingIntention (the slot 4 args). For
  walling, it is two Vec4s and a bool.

## Live probes to run (Edward, offline, each from Edward's own Human update: hook site 0x016FAACD)

Common setup:
- `p` = IHumanGround* = `QueryInterface(17)` (Human vt+0x48).
- `fsm = *(p+0x90)`, `path = *(u16**)(fsm+0x118)`.
- Log `path[0..5]` and `GetCurrentContextID(1)` (0x0052C2E0) every frame.

1. **Slot sanity, no calls:**
   - read `*(p+4)` = 0x026E7DB8, `*(p+0x60)` = 0x026E79B8, `*(p+0x64)` = 0x026E7998;
   - call slot 300 (GetFightStage) and slot 689 (GetHumanGroundStage); they must return small enums.
2. **DoEnterFight (75/76):**
   - walking (`path[3]==4`) near a guard G, `h = *(G+0xC8)`;
   - `r = slot75(p, &h)` → expect 1;
   - then at once `slot76(p, &h)` → next frame `path[2]==0xA3`;
   - negative control: slot76 without slot75 → path unchanged (the transition-id gate).
3. **Attack (154)** while in fight: `slot154(p, G, false, weaponType)` → read the request: +0xB30 = G's handle, +0xB7C == 34.
4. **Exit fight (112/113):** `slot112(p, 0)` → 1; `slot113(p, 0)` → `path[4]` leaves 167.
5. **PerformCombo (160)** in fight: `slot160(p, G, 0, 0, false)` → request +0xB70 / +0xB74 written.
6. **Climb (382/383)** facing a climbable wall: `slot382(p)` → 1, then `slot383(p)` → context 30 the next frame, no crash.
   Then the same on the puppet, as the A/B against TransitToContext(30).
7. **AttackNow (163/164):** expect 0 and no effect. This confirms they are stubs.
8. **AI-off (Q2):**
   - detour `0x0055DEF0` and count calls per component `ecx` for Edward's and a guard's behaviour component;
   - skip it for one guard → expect its FSM path to keep changing (Human::Update hook still fires), and no new
     self-chosen decisions.

## Negatives and gaps

- **No SP `Can/DoAssassinate(AC2MPConflict&)` or `Can/DoEnterCustomAction(CustomAction*,Entity*,Vec4&,bool)` in
  IHumanGround.**
  - METHOD: full demangled dump of all secondary groups of PS3 `_ZTVN8scimitar11HumanGroundE`, grep `Assassin` and
    `CustomAction`.
  - COVERED: the HumanGround vtable only (PS3 SP).
  - NAMES: CanAssassinate/DoAssassinate (group -0x74), Can/DoEnterCustomAction (group -0x70), Can/DoForceAssassinate
    (slots 66/67).
  - The PC slot mapping of those groups rests on the `retn` match, not on behaviour.
- **CanAttackNow / DoAttackNow are stubs on PC.**
  - METHOD: disasm of 0x01D26EF0 / 0x01D2DF30.
  - COVERED: these slots only.
  - The PS3 implementations were not compared, so whether another PC path implements them is UNPROVEN.
- **One-slot gap position (PS3 vs PC) between slots 168 and 300.**
  - METHOD: retn-signature alignment.
  - COVERED: all 841 PC slots.
  - Not pinned: candidates PS3 271..276, all 0-argument. UNPROVEN.
- **AI scheduler gate on the enabled bit: UNPROVEN.**
  - METHOD: imm 0x4000000 test/and sweep, 258 sites. Plus a search for functions that both test the bit and call vt+0xC0..0xD0.
  - Result: 0 functions; 4 decompiled, none of them the scheduler.
  - Families not covered: byte form `test byte [r+7],4`; bit tests through a register; list removal on toggle.
  - NAMES: n/a.
- **Who calls UpdateSingleThreaded / UpdateActor besides vtables.**
  - METHOD: xrefs. Only data refs (vtables) and the 0x017FFD60 thunk. The indirect call sites are not enumerated.
- **Readers of the transition records** (the context enter, vt+0x30): not decompiled. UNPROVEN that they consume type 0
  and fall back on -11.
- **PS3 DWARF layouts** of TransitionToClimbing, ClimbingEntryTransitionData, TransitionSetupDataToInAir, GapInfo and
  HumanGroundScope: not obtained.
  - METHOD: name scan of all CUs for definitions with byte_size.
  - Found only `Human::HumanContextBundle` (cu 276, 0x48793E9, 156 B).
  - The other types carry their name only on a declaration (DW_AT_specification not resolved).
- **The player decision chain** (AssassinTask handlers `0x1AF7C80`… ← `sub_1AF9460` ← `sub_1AF9D10`): not traced up to
  the component update.
- **AC4BFMP.exe was not checked** (RULES-RE rule 4; another agent holds the MP db). Every claim here covers the SP binary only.

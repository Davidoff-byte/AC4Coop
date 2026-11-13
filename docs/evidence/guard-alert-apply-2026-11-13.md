# Guard alert state on the guest: what the engine has, what reads it, where to apply it (S4), 2026-11-13

Static RE only. No plugin code changed, game not launched.

```
BINARY : AC4BFSP.exe (idb input path D:\SteamLibrary\...\AC4BFSP.exe, 131,645 functions), base 0x400000.
         Private copy of ref/ida/AC4BFSP.exe.i64 opened headless (idat 9.3, DBFL_KILL: nothing saved).
SOURCE : ref/AC4Coop-lobby @ facb9aa (feat/launcher) for every phase1.cpp / netstate.h line cited.
METHOD : decompile + xrefs + disasm in IDA; CRC32 of every idb string against state-descriptor ids;
         decompile of every caller of the three named alert predicates (46).
NOT RUN: no live probe. Everything marked [LIVE-TODO] is a static reading that still needs one.
```

## TL;DR

1. There are **two different "alert" things**. Keep them apart:
   - **World-level detection state** (`Unaware / Investigating / Alerted / In Conflict`) is held **once per
     world**, not per guard: a pointer at `holder+0x8E0`, where the holder is reached from the singleton that
     `0x54D4D0` returns (the plugin calls it `World::GetCurrentWorld`, `src/Scimitar/World.h:199`). It is a
     statechart, and the predicates test which states are active. It is the *local player's* conflict status:
     the debug "DETECTION" stat, HUD conflict flags and the conflict-music stop all read it.
     **Do not write it on the guest.** On the guest it is the guest's own world and player status.
   - **Per-guard visible state** is the **ConflictIcon HUD element** the engine keeps for each guard (HUD
     manager `dword_4DE3788`, module index `0x12`). Its mode field selects the icon:
     `investigation / chase / search / scold / neutral / atkMelee / atkRange / atkGrenade / atkFort /
     enterchase / atkNaval / atkNavalAttack / detection_blink`. The engine's AI drives it through a small
     API that takes a guard handle: `sub_FABC00` shows it or sets the mode, `sub_FA4820` sets the fill,
     `sub_FA4760` sets the sub-mode and `sub_FA4860` hides it. **That API is the recommended guest-side
     seam for the icon.** Static reading says it does not need the guard's AI to run.
2. **Fight and search can be told apart, but not from `EF_STAGE` and not from `CharacterAI+0xA0`.**
   `CharacterAI+0xA0` reads 14 in both fight and search (`ops/evidence/guard-state-7C-2026-11-10.md`). The
   difference is in the icon element. Mode `1` (chase) or `2` (search) is chosen by `sub_1046A30` from the
   sub-mode at `+0xE0`: sub-modes 1/2/3/10 give chase, anything else gives search. While the guard is
   chasing, the attack type at `+0xF0` (values 1..4) changes the label to `atkMelee/atkRange/atkGrenade/atkFort`.
3. **`EF_STAGE` (`HumanGroundStage`) is not enough.** It is the body's ground-locomotion stage and has no
   investigate or search value. The SDK enum names value 3 `Unbalance`, not "suspicious" (section 5). Add
   the icon's mode, sub-mode, attack type and fill per guard (section 7, 4 bytes).
4. **Stance (weapon out, fight idle) is already reproduced by the action-alias mirror**
   (`phase1.cpp:6952`, `SetTargetAction`). This pass did not find a separate stance switch to drive (see
   NOT COVERED).

---

## 1. World-level detection state (Unaware / Investigating / Alerted / In Conflict)

### 1.1 The name function (verbatim)

`AI_Detection_GetStateName` `0x13DD550` (size 0x8E), only caller `sub_15DEF90` @ `0x15DF126`:

```c
v3 = a2 ? *(_DWORD *)(a2 + 2272) : 0;                 // +0x8E0
if ( v3 ) {
  if      ( AI_AlertState_IsInConflict(v3) )    sub_986600("DETECTION", "In Conflict");
  else if ( AI_AlertState_IsAlerted(v3) )       sub_986600("DETECTION", "Alerted");
  else if ( AI_AlertState_IsInvestigating(v3) ) sub_986600("DETECTION", "Investigating");
  else                                          sub_986600("DETECTION", "Unaware");
}
```

`sub_15DEF90` is a stat/telemetry dump. In the same body it also emits `"STAT" "Leap of faith"`,
`"WANTED"`, `"COMBAT"`, `"GROUPMANIP"`, `"STAT" "Ship Elevator"` and `"TERRAIN" "Ship"`. `a2 = *(a3+92)`.

### 1.2 The predicates (verbatim): they test statechart active-state bits

```c
// 0x1013BA0 AI_AlertState_IsInConflict (0x2F B)
return (*(DWORD*)(*(DWORD*)(*(this+7) + 64) + 4*(desc->idx >> 5)) & (1 << (desc->idx & 0x1F))) != 0;
//   desc = *off_29EDE38 = 0x29EDDE8, idx = word desc+0x42 = 0x52
// 0x1013D20 AI_AlertState_IsInvestigating - same shape, desc = *off_29EEB9C = 0x29EEBA0, idx 0x73
// 0x1002E20 AI_AlertState_IsAlerted (0x10 B)
return *(_WORD *)(*(this + 7) + 326) != 0;            // u16 at statechart+0x146
```

`this+0x1C` is a statechart. The plugin's own `Statechart` layout (`src/Scimitar/Statechart.h`: Object
vptr, then the members in order) puts `m_InternalActiveStatesBitArray` at exactly `+0x40`. `idx` is the
state descriptor's index word (`+0x42`). Disassembly shows five more predicates of the same shape:

| fn | desc | idx | parent (desc+0x28) | name |
|---|---|---|---|---|
| `0x1013BA0` IsInConflict | `0x29EDDE8` | 0x52 | `0x29EDCC0` | id `F4B56DF5`, name not recovered |
| `0x1013D20` IsInvestigating | `0x29EEBA0` | 0x73 | `0x29EE928` | id `C904A2A1`, name not recovered |
| `0x1013C30` | `0x29EDCC0` | 0x50 | none | **parent of the In-Conflict state** |
| `0x1013BD0` | `0x29EDD20` | 0x51 | none | - |
| `0x1013C00` | `0x29EDD78` | 0x4F | none | - |
| `0x1013DB0` | `0x29EE6E8` | 0x20 | none | - |
| `0x1013C90` | `0x29EFC40` | 0x56 | none | - |

All 121 state descriptors are initialised in one function, `sub_1786BE0`: 121 pairs of
`mov ecx, offset desc; call sub_9FDF50`. `sub_9FDF50` stores the parent at `+0x24` and the depth at `+0x34`.
**Name recovery:** I computed CRC32 of every idb string (as-is and lower-case) and compared it with the 121
ids at `desc+0x2C`. Only **one** matched: `0x280740E3` = `"ConflictLoopLogic"` (desc `0x29F0950`, idx 0x78).
The other 120 states are UNNAMED: their hash input is not a plain idb string.

`sub_14ACAA0` returns "any alert": `IsInConflict || 0x51 || 0x50 || IsInvestigating || 0x20 || 0x4F || IsAlerted`.

### 1.3 Ownership: one per world, not per guard

```
METHOD : every caller of the three named predicates (46 unique functions), decompiled; the line that
         produces the predicate's `this` printed for each.
COVERED: 46/46 callers. 45 read it as `*(holder + 2272)` (+0x8E0) or `holder[568]`; sub_16C7DC0 reads
         `*(... + 0x8E0)` (same field, different decompiler form). 0 callers take it from a guard component.
NAMES  : no conclusion rests on a name; World is the plugin's name for 0x54D4D0's result.
```

Where the holder's origin can be traced inside the same function, it comes from the world-level singleton:

- `sub_1802140` takes `slot = sub_550B70(sub_5576A0())` from the player-slot list `dword_2AC16A0`, then
  reads `*(slot+0x5C)+0x8E0`. `sub_5576A0` returns the index of the slot whose `+0x5C == *(dword_2AC1630+0x50)`,
  which is the value `0x54D4D0()` returns.
- `sub_54D4E0` returns `0x54D4D0() + 0x888`. `sub_17DA860` (only caller: thunk `sub_17E0760`) and
  `sub_17D5500` copy `*(that + 0)` into `owner+0x8E0` and reset `+0x8E4..+0x8F8`.
- `sub_1325020` and `sub_1325060` go from the entity's `+0x64` handle to its owner, then `+0x5C`, then `+0x8E0`.

So `Unaware/Investigating/Alerted/InConflict` is the **global conflict status of the local player's world**.
[UNPROVEN that all 46 holders are the same object: 8 can be traced within their own function, the rest
arrive through getters.] On the guest it describes the guest's own world, so it must not be used to show a
host guard's state.

### 1.4 Readers of the world state (what it drives)

| reader | what it does | VA |
|---|---|---|
| debug stats | "DETECTION" name, see 1.1 | `0x15DEF90` |
| statechart conditions | About 25 small functions call `HandleTrigger` (`0xA014A0`) on an entity statechart depending on whether the world is in conflict or investigating. Examples: `0x1325020` fires `dword_4DEF040` if in conflict and `0x1325060` fires `dword_4DEF03C` if not; `0x124B840/850` do the same for investigating; `0x115B1B0` fires `dword_4DE83B0` when the world is neither in conflict nor alerted | see 1.3 list |
| local-player-in-conflict | `sub_1802140` | `0x1802140` |
| HUD flags | `sub_1247220` caches IsInConflict at `this+188` and IsAlerted at `this+192`, and calls `sub_123A220` when either changes | `0x1247220` |
| conflict music | `sub_1600F30` posts `FIGHT_STOP`, `CONFLICT_STOP` (via `sub_FE8B70`), `CONFLICT_FORT_STOP` and `HARPOONING_STOP`. Its only caller is `sub_17F02F0`, which looks like world/session teardown | `0x1600F30` |

Conflict music is **not** fully traced. The `CONFLICT_STOP` string is referenced by 6 functions
(`0x1600F30, 0x1601060, 0x1601260, 0x1601390, 0x160D6E0, 0x1622D70`); only `0x1600F30` was read.

---

## 2. Per-guard visible state: the ConflictIcon HUD element

### 2.1 Creation / lookup

```c
// 0xFABC00 (cdecl, 10 callers in AI code)
mod = sub_1311470(dword_4DE3788, 0x12);        // disasm: mov ecx, dword_4DE3788 ; push 12h ; call
if (mod) sub_FAACF0(ref, mode, los, a4, a5);   // thiscall on mod
// sub_1311470: return a2 >= 64 ? 0 : *(this + a2)   -> module = *(hudMgr + 0x48)

// 0xFAACF0 (show / set)
el = sub_FA0BC0(mod, ref);                     // find existing element for this guard
if (el) { sub_13DD8D0(el, 0); los = el[+0xF4]; }   // clear the dismiss byte +0x21
else    el = sub_F88200(new(0x40..), "ConflictIcon");
el[+0x120] = a4;  if (a4) el[+0x64] = 2;  el[+0x124] = a5;
sub_13DD930(el, mode);   // -> sub_1046920: set mode
sub_13DD910(el, los);    // -> sub_1046A00: el[+0xF4] = los, dirty +0x26
```

`sub_FA0BC0(mod, ref)` (read from disassembly) walks the element array at `mod+0x58` (u16 count at
`mod+0x5E`). It keeps elements whose class id `sub_A1D0D0(el)` is `0xE0E9D3BC`, and returns the one whose
ref list at `el+0x78` (u16 count `el+0x7E`) contains `ref` (comparison via `sub_194EEF0` -> `sub_131CBD0`).

`ref` is the address of a refcounted smart handle to the guard. Some callers pass `&local`, where
`local = *(X+0xC8)` and `InterlockedIncrement(local+4)` is done first (`0x17A04A0`). Others pass `X+0xC8`
directly (`0x11E9A20`: `*(this+164)+200`). Resolving that handle with `sub_6F9EC0` gives an object `O`.
`O` is then used exactly like `GetHuman`'s argument (`O+0xE8 -> +0x1C`; compare `0x13905A0`/`sub_6F5F70`),
so `O` is the guard **entity**. [LIVE-TODO: confirm `X` and `O` on a real guard. The plugin already reads
smart handles of this shape at `phase1.cpp:3276`.]

### 2.2 Element fields (all from the setters and the updater above)

| offset | meaning | evidence |
|---|---|---|
| `+0x21` u8 | dismissed (1 = going away) | `sub_13DD8D0`, `sub_FAADC0` |
| `+0x26` u8 | dirty | `sub_1046920/1046A00/1046A30` |
| `+0x78` / `+0x7E` | guard handle list / count | `sub_194EEF0` |
| `+0xE0` u32 | **sub-mode** (selects chase vs search) | `sub_1046A30` |
| `+0xE4` u32 | **mode** = index into label table `off_28B2BB0` | `sub_1046920`, `sub_105C9D0` |
| `+0xE8` u32 | deferred mode while mode == 9 (enterchase) | `sub_1046920` |
| `+0xEC` f32 | fill / detection progress; mode becomes 13 when `>= *(dword_4DE6D48+192)` | `sub_13DD970`, `sub_105C9D0` |
| `+0xF0` u32 | attack type: 1..4 map to labels 5..8; set to 5 (none) when sub-mode != 1 | `sub_105C9D0`, `sub_1046A30` |
| `+0xF4` u8 | line of sight (label prefix `"los_%s"` vs `"nolos_%s"`) | `sub_105C9D0`, `sub_1046A00` |
| `+0x120` u8 | a4 flag (when set, `+0x64 = 2`) | `sub_FAACF0` |

`sub_105C9D0` is a virtual method of the element (its only xref is vtable slot `0x2690468`). It builds the
Flash label `los_/nolos_ + table[mode]`; modes 1 and 2 are remapped through `+0xF0`.

### 2.3 State table (label table `off_28B2BB0`, read from the idb)

| mode | label | set by (static) |
|---|---|---|
| 0 | investigation | `sub_1419FE0` (event handler; also writes fill) |
| 1 | chase | `sub_FABC00` from `sub_1324460`, `sub_1498980`, `sub_15E6600`, `sub_17B9810`, `sub_19B8370`; `sub_FAD3D0`; `sub_1419ED0`; also from `sub_1046A30` for sub-mode 1/2/3/10 |
| 2 | search | `sub_1046A30` for any other sub-mode |
| 3 | scold | `sub_FABC40` (from `sub_1A44430`) |
| 4 | neutral | `sub_141A090` (event handler) |
| 5..8 | atkMelee / atkRange / atkGrenade / atkFort | label remap of modes 1/2 by `+0xF0` = 1..4; mode 7 is also passed directly by `sub_1A32240` |
| 9 | enterchase | `sub_1046920`: mode 4 + request 1 while `dword_4DE4D34` is set |
| 10 | atkNaval | `sub_122EFC0` |
| 11 | atkNavalAttack | label only, no setter found |
| 12 | chase | `sub_17A04A0` (reached from `sub_17A0570`; `0x17A0A50` references `"ConflictLoop_IndividualEnterChase"`), `sub_11E9A20` |
| 13 | detection_blink | `sub_105C9D0` when the fill crosses the threshold |
| - | hidden | `sub_FA4860` posts an event; `sub_FAADC0` sets `+0x21` |

Mode passed by each of the 10 `sub_FABC00` callers (from their decompiles):

| caller | mode |
|---|---|
| `11E9A20` | 12 |
| `11E9B50` | computed `v3`; it then calls `sub_FA4760(ref, 2)` and `sub_FA4470(ref,1,1)` |
| `122EFC0` | 10 |
| `1324460` | 1 |
| `1498980` | 1 |
| `15E6600` | 1 |
| `17A04A0` | 12 |
| `17B9810` | 1 |
| `19B8370` | 1 |
| `1A32240` | 7 |

### 2.4 Two call paths into the element

- **Direct.** `sub_FABC00/FABC40/FA4820/FA4760/FABCA0` load `dword_4DE3788` and change module 0x12 in place.
- **Event.** `sub_FA4860` and `sub_FA4470` allocate a message from a pool under a lock (`sub_FA06C0`:
  `sub_D587F0(dword_2AC1FCC)` ... `sub_AC6580(dword_2AC1FCC)`, list `dword_2AC1FBC`). They post it through
  `sub_6E5470()` -> `sub_649FE0` -> `sub_4545A0`. The handlers are registered in `sub_141FEB0`
  (`sub_1419ED0`, `sub_1419F10`, `sub_1419FE0`, `sub_141A090`, ...).
  - Which message goes to which handler is matched by field layout only: `FA4860` writes `+12 = 1`, and
    handler `1419ED0` treats op 1 as hide. `FA4470` writes bytes `+12/+13`, which handler `141A090` reads.
    [UNPROVEN mapping.]
  - The lock suggests this path is meant for callers outside the HUD's own context. [UNPROVEN: I did not
    read whether `sub_4545A0` queues or dispatches inline.]

---

## 3. Per-guard internal fields (already known, re-stated for the table)

| field | values | fight vs search | source |
|---|---|---|---|
| `CharacterAI+0xA0` | 0 calm, 14 conflict (reaction-stack top, push `sub_154D2E0` / pop `sub_154D370`) | **no** (14 both) | `ops/evidence/guard-state-7C-2026-11-10.md` |
| behaviour `+0x7C` | 0/4/8/12, **latches** | no | same |
| `HumanGroundStage` (`EF_STAGE`) | locomotion stage; 1 Navigation, 4 Fight, 0xC RangeCombat, 0x11 FleeFight | partly: combat stages only while actually fighting | `phase1.cpp:4503`, enum `src/Scimitar/AI/Human.h:294` |
| ConflictIcon mode/sub-mode | section 2 | **yes** (1 chase vs 2 search) | this doc |

## 4. Answer to (2): fight vs search

- **From the icon element: yes.** Mode 1 vs 2 comes from the sub-mode at `+0xE0`, refined by the attack
  type at `+0xF0`.
- **From the fields the host already sends: only partly.** `stage` in {4, 0xC, 0x11} means "fighting now",
  but a searching guard and a calm guard both sit in non-combat stages. `CharacterAI+0xA0 != 0` together
  with a non-combat stage would mean "searching", but `+0xA0` is not on the wire.

[LIVE-TODO: one host run that reads `+0xE0/+0xE4/+0xF0` of every guard's element through calm, investigate,
chase, fight, search and calm.]

## 5. `EF_STAGE` is not the alert state

- It is `HumanDataBundle->m_GroundData.m_HumanGroundStage` (`phase1.cpp:4503`), the body's ground-FSM stage.
- The enum in `src/Scimitar/AI/Human.h:294` has no investigate, suspicious or search value. Its names come
  from the SDK header, and a string scan finds **only `"Navigation"` as an exact string in the idb**, so the
  names are unverified against the binary.
- `ops/evidence/alert-mirror-design-note-2026-11-12.md` reads value 3 as "suspicious" from a live run; the
  header says 3 = `Unbalance`. This is unresolved: do not build on either reading.

## 6. Recommended seam (3): drive the icon through the engine's own API on the guest

On the guest, for each borrowed body that mirrors a host guard, act whenever the host's icon values change:

1. **Build a smart handle to the borrowed body** the way the engine does for icons, then release it after
   the call (`InterlockedDecrement(h+4)`, and `sub_A094D0` when the count reaches 0). Two candidates:
   - `sub_A0BDE0(byte_4DD5FA0, 0, obj)`, as in `sub_1419F10`/`sub_FABCA0`. There `obj` comes from
     `sub_13905E0`, whose return value was not read.
   - `*(X+0xC8)` with the refcount bump that `sub_17A04A0` does.
2. **Show or change the mode:** `sub_FABC00(&h, mode, los, 0, 0)` (cdecl, 5 args).
3. **Set the sub-mode:** `sub_FA4760(&h, sub)` (cdecl). This is what turns modes 1/2 into chase or search.
4. **Set the fill:** `sub_FA4820(&h, fill)` (cdecl, float).
5. **Hide:** `sub_FA4860(&h)` (event path), when the host's element is gone or the guard is released.

Why this seam:
- It is the exact call the engine's AI makes.
- It lives in the HUD module and does not read the guard's AI state. `sub_FAACF0`, `sub_1046920`,
  `sub_1046A00` and `sub_1046A30` read only the element and the tuning globals `dword_4DE4D34`/`dword_4DE6D48`.
- So a body with BhvGenericNPC disabled is not a problem for the call itself.

Risks and constraints (all [LIVE-TODO] unless stated):

- **Thread.** The engine's direct callers are AI functions (`sub_11E9A20`, `sub_15E6600`, ...). The plugin's
  safe place is the per-Human frame hook `0x16FAACD` (`phase1.cpp:5738`), which deferred action mirroring
  already uses. This pass did not show whether AI update and that hook run on the same thread. If unsure,
  use only the event-path calls (`sub_FA4470`, `sub_FA4860`), which take the pool lock; but those cannot
  set the mode or the fill.
- **Handle target.** It is not yet shown whether the icon handle must point at the Entity or at another
  object (section 2.1). Probe on the host first: read `el+0x78[0]` -> handle -> object for one guard and
  compare it with that guard's Entity*.
- **Lifetime.** The element holds a refcount on the guard handle. Hide it before the borrowed body is
  released or recycled (the guard stale-return path), or the HUD keeps a stale icon.
- **Competing writers.** If the guest's own AI still calls `sub_FABC00` for that body, it overwrites our
  mode every frame. Static reading says those callers are AI/conflict code, which disabling BhvGenericNPC
  should silence. [UNPROVEN: those callers were not traced back to BhvGenericNPC.]
- **Perspective.** The host's element shows how a guard reacts to the *host's* player, so mirroring it
  shows the host's view on the guest. If the guest should see its own perspective, that is G1 (guards
  perceive the guest), not this task.
- **Do not** write the world alert statechart (`holder+0x8E0`) or fire its triggers on the guest. It drives
  the guest's own conflict status, music and HUD flags (section 1.4).

## 7. What must go on the wire (EF_STAGE is not enough)

Per guard, captured on the host from that guard's ConflictIcon element (same walk as `sub_FA0BC0`):

| field | bits | source |
|---|---|---|
| `iconMode` | u8 (0..13, 0xFF = no element or `+0x21` set) | `+0xE4` |
| `iconSub` | u4 | `+0xE0` (only "in {1,2,3,10}" matters: chase vs search) |
| `iconAtk` | u3 | `+0xF0` (1..5) |
| `iconLos` | u1 | `+0xF4` |
| `iconFill` | u8 | `+0xEC` scaled to 0..255 [the float's range was not shown statically] |

- That is four bytes per guard. An optional fifth bit could carry `CharacterAI+0xA0 != 0` (in conflict).
- Keep `stage` for what it already does.
- This is a protocol change in `netstate.h`/`net.cpp` (Track A's files), so it is listed here, not done.
- The design note's advice still holds: build the wire field together with its consumer (this seam), not before.

## 8. Proposed live probes (in order; none run)

1. **Host, read-only.** Walk module 0x12's elements each second. Log the guard Entity*, the handle target
   and `+0xE0/E4/E8/EC/F0/F4/21` through calm -> investigate -> chase -> fight -> search -> calm.
   PASS = the mode sequence matches section 2.3 and the handle target == the guard entity.
2. **Host, read-only.** Log `GetCurrentThreadId()` in the frame hook and on entry of `sub_FABC00`
   (one-shot). The answer is either same thread or not.
3. **Guest.** On one borrowed body, call `sub_FABC00(&h, 1, 1, 0, 0)`, then `sub_FA4860(&h)` 5 s later.
   PASS = the icon appears over that body and disappears, 0 faults, and the element count returns to its
   starting value.

## NOT COVERED

- Readers of `HumanGroundStage` and of the weapon-out flag (stance): no sweep was run. The stance claim
  rests on the existing alias mirror only.
- Combat **music start**: only one of the six `CONFLICT_STOP` users was read, and no `*_START` string
  search was done.
- Names of 120 of the 121 world-statechart states.
- `AC4BFMP.exe` was not opened for any of this (RULES-RE 4). Every absence above covers AC4BFSP only.
- `sub_4545A0` (inline vs queued event dispatch).

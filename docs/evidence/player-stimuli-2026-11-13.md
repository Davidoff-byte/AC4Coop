# What the player's combat posts to guards (EventProbe runs + IDA, 2026-11-13)

Purpose: the user reports that guards aggro when the player attacks or fires a pistol, not when the player walks
past. To make guards react to the co-op puppet, the host must raise the same events with the puppet as source.
Binary md5 2058342866688f780c8b34526a65bc35.

## Runs (host, one PC)

| pid | build | what the user did | log |
|---|---|---|---|
| 19852 | f6e67e4 (asi fa7ad187) | stood near guards, fired the pistol, then sword attacks | `local/runs/phase1_log_19852.txt` |
| 21692 | f3b4b42 (asi abaa249b) | pistol shots and sword swings, spaced (count and timing not logged) | `local/runs/phase1_log_21692.txt` |

0 faults, 0 crash records in either run.

## Verified mechanism (IDA)

- **Post.** Every event goes through `sub_64A020(Event* this, Entity* receiver)`, thiscall, `retn 4`. It calls
  `this->vt[+0x24](receiver)` and queues the event (`sub_454460`).
- **Stimulus event.** Class vtable `0x026E47C4`, 24 bytes, allocated by `sub_FA3260` (ctor `sub_16AAD10`):
  - `+0x08` source handle (`entity+0xC8`);
  - `+0x0C` second handle;
  - `+0x10` stimulus id;
  - `+0x14` u16 flags. The upper 16 bits of that dword are not initialised; the probe printed them as garbage.
- **Broadcast template.** Class vtable `0x026E4798`, allocated by `sub_FA3300` (ctor `sub_16AAC80`), with the same
  field layout. It is posted to the **emitter's own entity**. A listener descriptor at `0x0291EEB0`
  (handler `sub_13442D0`, event type hash `0x7D537629`) runs `sub_1340E60`, which:
  - copies the template;
  - posts the copy to every entity in the list at `mgr+464` that is not kind 1 and is not the source;
  - includes only entities within the per-id radius;
  - is the call site seen in the logs as `from=013410BA`.
- **Per-id radius table.**
  - The getter `sub_10A9F80` returns `mgr = *(0x4DE6D4C)`. Entry = `*(*(mgr+500) + 4*id)`; `sub_10C56E0` reads
    the radius at `+40`, `sub_10C5780` the vertical limits at `+52` / `+56`. A missing entry falls back to
    `mgr+60` (15 m).
  - Entry vtable `0x026953BC`.
  - Read live from pid 21692 (ReadProcessMemory, read-only).
- **`sub_184EEA0`.** A combat-context function (callers `sub_1854390`, `sub_18573E0`, `sub_185A800`,
  `sub_185CAE0`) posts templates **48 then 49** to its own Human's entity, with:
  - `+8` = that entity's handle;
  - `+0xC` = the handle of the entity at `ctx+0x1B8`.
  - **Not confirmed as the poster in our runs.** A template posted at `0x0184EFC2` would log `from=0184EFC7`,
    and that never appears, because the probe deduplicated by call site and logged no template ids.
- **`sub_16188C0`.** Also writes 48 and 49 as local values and posts to a victim `a3`, depending on a damage type
  `a6`. It looks like a damage handler; this is a reading of the code, not a verified role.

## What the player emitted (player = source)

Ids in order of first appearance, with radius (m) from the live table and the call site:

| id | radius r40 (up/down) | site | seen |
|---|---|---|---|
| 56 | 6 (6/6) | broadcast | before combat, repeatedly |
| 48 | 15 (6/6) | broadcast | **together with 49**, twice in run 21692 (21904.032, 21925.343), once in run 19852 |
| 49 | 31 (45/45) | broadcast | always with 48 |
| 116 | 8 | `sub_1562640` (direct, entities from `sub_17FF690`) | many (cap 12) |
| 135 / 136 | 30 / 12 | `sub_10608A0` (direct, distance test) | many |
| 22 | 3 | 0x0189BB38 (no function boundary in the idb) | several |
| 23 / 252 | 25 / 25 | `sub_1412D00` (direct to `*(this+3)+140`) | several |
| 74–78 | 25 | `sub_184E4F0` (one id, randomly chosen, to one target) | 75 once, 78 twice |
| 153 | 30 (45/10) | broadcast | several |
| 124 / 125 | 30 / 30 | `0x01D8ECB2` / broadcast | once, just before the second 48+49 |

## Not established

- Which id is "gunshot" and which is "melee attack". The run had no per-action markers. 48+49 appeared twice;
  the user was asked for 3 shots.
- Which function posted the 48/49 templates in these runs.
- Whether the co-op puppet entity carries the listener that turns a template into a broadcast.

## Next probe (v3)

- Log every template (`0x026E4798`) occurrence with its id, `+8`, `+0xC` and the poster's return address.
- Add the player's current action alias to each occurrence line, so each id is tied to the action being played.

## Result: the puppet's gunshot alerts the host's guards (2026-11-13)

| pid | build | test | result |
|---|---|---|---|
| 7164 | b2ad941 (asi 16a4ba46) | `PuppetStimMode=1`: templates 48, 49 posted to the puppet's own entity | posted 2; **no broadcast** (no 48/49 copy with the puppet as source reached anyone): the puppet entity does not turn templates into broadcasts |
| 7164 | b2ad941 | `PuppetStimMode=2`: one stimulus per NPC in radius (48: 3 receivers, 49: 50 of 102) | the engine generated its own follow-up stimuli with the puppet as source (49 relayed by `sub_1654F40`, 116 by `sub_1562640`), then **crashed**. Fault at `0x01780657`: the fight-member loop `sub_161BD30` → `sub_13F22E0(puppet)` → CSrvNavigation `sub_1780610` read `*(*(nav+0x84)+0x14)` = null (`AC4Coop_crashctx_7164_1.txt`) |
| 23688 | 7936a75 (asi 219507ce), `FightSkipPuppet=1` | mode 2 (48: 3, 49: 55 of 103) | **no crash**; 1 fight order for the puppet skipped; two guards then sent stimuli 8 + 95 about the puppet continuously (~30 each in 10 s) |

User's observation in run 23688, with the puppet on a roof: the guards spotted the puppet and tracked it. They went to full alert / combat state, facing the puppet's direction, **without moving or attacking**.

- Oracle level: 4 (the user looked at it) plus the log counts above.
- Not established: why they neither moved nor attacked. Candidates, all UNVERIFIED:
  - the puppet was on a roof the guards cannot path to;
  - the fight system needs target data the puppet lacks;
  - `FightSkipPuppet` also removes something the guards' attack depends on.

## Session 1 (2026-11-13 17:30, build f352057, asi fbd05f19): chase still absent; the puppet has no FightDataItem

| item | oracle | result |
|---|---|---|
| M1 AutoContinue | `[AC] ... press-start` + `continue` lines, in world with no input | **FAIL**: both detours installed (pids 33068, 1108), neither fired; the user had to load manually. Cause not yet established |
| F7 chase | guards chase/search the puppet out of sight; `[Poi] lentPuppet>0` | **FAIL** (user: "they just stand"). `[Poi]` lent the puppet 0-4 times per 5 s window out of about 1,600 lookups |
| crash | none | **crash** at 17:37: `0x010D4BAD` read `[eax+6Ch]`, eax = 0 (`AC4Coop_crashctx_20588_1.txt`) |

### Crash root cause (static)

- **Chain:** `sub_11DA9F0` → `sub_10D4AA0(e)` → `sub_133EFB0(e)` returned 0, then `cmp byte [eax+6Ch],0` faulted.
- **The lookup:** `sub_133EFB0(e)` = `sub_133BBF0(e)` searches the entity's AI object for an item of class id `0x9BEE81DB`:
  - AI object = `*(*(e+0xE8)+0x1C)`, via `sub_6F5F70`;
  - item array at `ai+0x5C`, u16 count at `ai+0x62`;
  - item class tested with `sub_A1D0D0`;
  - class id `0x9BEE81DB` = **FightDataItem** (class map: vtable `0x02695368`, class info `0x028C0678`, size `0x70`).
- **Scope:** `sub_133EFB0` has **165** callers. The player's stack in run 7164 shows a FightDataItem (`0x42A21460`); the puppet's lookup returns null. So the puppet lacks a FightDataItem, and every fight-code caller that assumes one can fault.
- **Same pattern, earlier crashes** (puppet lacking player-side data):
  - `0x01780657`: CSrvNavigation `+0x84→+0x14`;
  - `0x015DB19C`: AIComponent `+0x94`.
  
  Guarding sites one at a time is not a fix (165 callers).
- **Construction:**
  - The puppet is built from the player's own EntityBuilder + selector (`phase1.cpp:550-609`).
  - FightDataItem's ctor is `sub_10DF7B0`. It is reached only through `0x010EA380`, a jump with no code xrefs, i.e. the reflection create function.
  - The vtable's slot 0 is `sub_10EA390`, a clone, which calls `sub_10DFB00`.
  - **Not yet found:** who attaches a FightDataItem to the player's AI item array, and why the build path leaves the puppet without it.

Next (static): writers of `ai+0x5C/+0x62` that store a FightDataItem (reflection instance `0x028C0678`, clone `sub_10EA390`).

### CORRECTION (session 2, 2026-11-13 17:51): the FightDataItem lookup in crash 20588 was on the PLAYER, not the puppet

- **Who `esi` was.** In `AC4Coop_crashctx_20588_1.txt`, `esi = 49636A70`. The same log names that pointer as the
  player at the time: `[Phase1] AddEntity issued; GetPlayer now = 49636A70 (orig=49636A70 new=44D99AF0)`. So
  `sub_133EFB0` returned null for the **player** entity. The claim above that "the puppet lacks a FightDataItem" was
  wrong: it attributed the entity from the stack layout instead of reading the pointer against the log.
- **Differential** (`ops/tools/aiitems.py`, read-only ReadProcessMemory, pid 5452, player `40295B70` vs puppet
  `3402EE20`):
  - Both AI objects are CharacterAI (vtable `0x026D3CD8`) with exactly **one** item, the same class (vtable
    `0x0272D18C`, not in the class map).
  - Neither has a FightDataItem.
  - The puppet's `AIComponent+0x94` is non-null (`419D7F80`).
  - A 1 s sampler through the user's fight in step 3 recorded **no change** to the player's list.
- **So FightDataItem is not on the player's AI item list even in a fight.** Which entity normally carries one, and
  why `sub_10D4AA0` reached the player with the path `14098E0 = true, 14B7FE0 = false`, are both open.
- **Not established.** Why the conflict object's +0x304 handle resolved to the player at that moment, after the
  puppet's handle was recycled across a scene change (the fired puppet handle `34582EFC` later pointed at `49636A70`).

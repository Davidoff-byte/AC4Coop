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

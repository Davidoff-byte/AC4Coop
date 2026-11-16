# Host forwards attacks: native defence and counters on the guest (2026-11-16)

User's choice (over "copies fight locally"): the host stays the only brain. A host guard's attack on the puppet is re-posted
on the guest as a native FightDamageEvent from the guest's linked copy of that guard to the guest's own Edward. The guest's
engine then resolves the warning (defence / counter side) and the hit (block, parry, damage, hurt reaction) itself.
Flag: `[Coop] AttackForward`, default **0**, read once at load. Code: `src/attack_forward.h`, `src/player_damage_probe.cpp`
(H1 capture, `AttackForward_Post`), `src/phase1.cpp` (AttackForward block).

## RE (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, ida_batch fa1-fa3; probe backtraces, run 29284)
- **Warning phase** (FDE+0x54 = 0). Posted every frame during a guard's attack (113 posts in one host fight) through
  `0x64D8C0`. Stack: `0x18DA72F` (sub_18DA720) ← `0x16553CE` (sub_16551A0) ← `0x168D103` ← `0xFF1CA1` ← `0x57E212`.
  - `sub_16551A0` is the melee attack poster:
    1. `fde = sub_10EFC30()` (cdecl; 0x170 bytes; ctor `0x18C9470`);
    2. `sub_18BD690(fde, type, subtype, f336, &dir, &pos, b331, b328, attacker)` (thiscall, 8 args, `retn 20h`; sets the
       attacker handle +0x58 from `attacker+0xC8` with its refcount);
    3. it copies about 20 fields from the attack descriptor (listed in `attack_forward.h`);
    4. `fde+0x54 = (a6 == 0)`;
    5. `sub_18DA720`, which posts with `sub_64D8C0`.
- **Victim side, warning:** `sub_185DA80`. It hands the event to the victim AI's component 78 (`vt+44/+48`, or `vt+20/+24`).
  What that component reads is **not traced**.
- **Hit phase:** the same poster with a6 = 0. There is also the contact poster `sub_1D1A4A0` (type 35, `sub_18B5AC0`),
  which is not forwarded and stays on PlayerDamageRelay.
- **Not copied:** handles (+0x4, +0x58, +0x118, +0xA4) and embedded objects with vtables (+0x70, +0xD0, +0xD8). The guest's
  own constructor and `sub_18BD690` make those for the guest's entities. Raw copying was rejected for this reason.

## Wire
- Three intent messages per event, IC_ATK_A/B/C (8/9/10), 32 bytes each. `wireId` is the guard's canonical id, and the
  messages are joined by seq.
- Rate:
  - Hits: always, deduplicated to one per guard per 150 ms (each hit is posted twice).
  - Warnings: at most every 150 ms per guard. The guest re-posts the latest warning every frame while it is < 250 ms old.
- A guard idle for 2 s is dropped from the lane (`ForgetIntents`), so a rejoin does not replay an old hit.
- A hit from a guard with no linked copy on the guest falls back to a DamageEvent with the host's `sub_199A160` damage.

## Live runs (two copies on one PC, `FightIntents=1 IntentLane=1 PlayerDamageProbe=1 AttackForward=1`)
`tools/two_copy_fight.py`: 20 s of normal fighting to start the fight, then 50-70 s of the mode under test. Only that
second phase is counted (guest log lines after the provoke phase).

| run (asi) | phase 2 mode | forwarded hits | hits that built damage on the guest | guest strikes on copies | guards killed (host decided) |
|---|---|---|---|---|---|
| 33620/22896 (aafdb257) | mixed X/B, whole run | 17 | 8 | 5 | 1 |
| 24936/33648 (d47f5649) | **counter only** | 21 | 5 | 2 | 1 |
| 33756/13424 (d47f5649) | no input (control) | 6 | 4 | 0 | 0 |
| 23860/32972 (d47f5649) | **counter only** | 8 | 3 | 0 | 0 |
| 29212/29964 (87a5e743) | no input (control), 70 s | 44 | 11 | 0 | 0 |
| 5668/18300 (87a5e743) | **counter only**, 70 s | 29 | 3 | **8** | **3** |

- **Counters work natively on the guest.** With only the counter button pressed, the guest hurt and killed host guards:
  8 strikes and 3 deaths in run 5668/18300, 2 and 1 in 24936/33648. The no-input controls had 0 and 0. The counter's
  damage reached the host as OwnerDamage requests, and the host decided the deaths.
- **Every forwarded event ran the guest's own fight handler.** The probe shows `fight-interactions player` for each
  forwarded hit, and the DamageEvents were built by the guest's engine (20 per hit, as on the host). The guest's life fell
  through the native chain (e.g. 125 → 105 → 85 → 65 → 45 → 26 → 6).
- **Hits that built damage:** 10% with counter only (6/58 over the three counter-only runs), 25-67% with no input.
  Pressing the counter button cuts the damage taken.
- **UNVERIFIED:** why only 11 of 44 hits built damage with no input at all. Candidates: the guest was already low or
  downed, or an engine rule (for example a damage cooldown), or a field the forward does not carry. With
  PlayerDamageRelay every hit applied. This is why the flag stays default 0.
- Both copies stayed up in all six runs. `failed=0` on the builds from d47f5649 on. In the first run 590 per-frame
  re-posts failed; the cause was not captured, and it did not recur once fault logging was added.

## Not done
- The guard's second event (`sub_18DA720` posts one more to the attacker side when fde+0x90 != 0) is not rebuilt.
- Type-35 contact hits stay on PlayerDamageRelay.
- The guest's counter animation and the host guard's reaction (does the host guard get interrupted?) were not checked
  visually. On the host, the guard keeps its own AI until the death request arrives.

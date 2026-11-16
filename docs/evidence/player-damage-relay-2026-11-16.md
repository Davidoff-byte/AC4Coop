# Guards hitting the other player: where the hit is lost, and the owner-decides relay (live, 2026-11-16)

Follows `player-damage-path-2026-11-16.md` (static). Two copies on one PC via `two_copy.ps1`, D: install (AC4BFSP.exe md5
`2058342866688f780c8b34526a65bc35`), settings `NativeReplica=0 FightIntents=1 IntentLane=1` plus the install ini's
`PuppetNavService=1 FightKeepOnConflictEnd=1 DespawnLeaveFight=1 ConflictKeepPuppet=1 DisablePuppetAI=1 GuardSeesPuppet=1`.
The guest walked to the soldiers at (26,-75) and swung (pad X); the host's puppet fired the gunshot stimulus
(`PuppetStimSeq` bumped). Guards then attacked the puppet on the host. Positive control: the host walked over and fought.
Builds: the working tree on f7a1520 (tree dirty = this change), asi hashes per run below.

## 1. Q2 answered: the hit is posted to the puppet and then dropped (probe `[Coop] PlayerDamageProbe=1`)
Read-only hooks, all calling the original (`src/player_damage_probe.cpp`, prologue bytes quoted there):
H1 `0x64A080` (event vt[9] listener lookup, return value read), H2 `0x186B370` (CSHumanFightInteractions slot 18),
H3 `0x1998D40` (DamageEvent builder), H4 `0x173E190` (CSDamage listener).

| run (asi) | victim | FDE posts | hit-phase posts (FDE+0x54 = 1) | listener found | fight-interactions (H2) | DamageEvents built (H3) | CSDamage (H4) |
|---|---|---|---|---|---|---|---|
| 17624 (9d18ead2) | puppet | 1118 | (not counted yet) | all (0 dropped) | **0** | **0** | **0** |
| 17624 (9d18ead2) | host's Edward (control) | 181 | | all | 181 (1:1) | 7 | 189 |
| 25140 (687115e4) | puppet | | 44 lines | all | **0** | **0** | **0** |
| 25140 (687115e4) | host's Edward (control) | | 6 | all | 6 | 6, damage 20 each | |

- The listener lookup (`sub_5220E0`) succeeds for the puppet, so the event is queued. But nothing that handles it runs
  for the puppet: no fight-interactions call, no DamageEvent, no CSDamage. The puppet has no fight or health services.
  The f7 "no damage" result is explained.
- **Each hit on the puppet is posted twice**, 16-17 ms apart, with the same attacker. Distinct hits from one attacker
  are ≥ 1.7 s apart. A hit on the host's Edward is posted once.
- FDE fields at post time:
  - +0xC (damage) is **0** in both cases. The damage is computed later, in `0x18644D0`.
  - +0x58 is the attacker's own `[E+0xC8]` handle in 50/50 lines.
  - +0x20 = 1, +0x10 = 1 or 2, low byte of +0x1C = 0x10.
- The puppet's entity kind (+0x9C & 7) is **1**, the same as the player's: `[PDP] puppet=3C315260 +9C=00000001 kind=1`.

## 2. Scoring the hit with the engine's own function
`0x18644D0` (decompile lines 343-360) calls
`sub_199A160(dword_4E25974, FDE+8, victim, attacker, FDE+0x10, FDE+304, FDE)`, but only when:
- FDE+333 != 0,
- FDE+8 is not 35 or 26,
- the attacker has entity+0x5C,
- FDE+323 == 0.

`sub_197B680` is `mov eax, dword_4E25974; retn`. `sub_199A160` is a thiscall with `retn 18h` (6 args; ida_batch pd3).
The probe calls it at post time:

| run 25140 | computed | engine's DamageEvent damage |
|---|---|---|
| host's Edward, 6 hits | 20 ×6 | 20 ×6 (H3) |
| puppet, 44 posts | 20 ×44 | (none: the chain never runs) |

The computed value matched on 6 of 6 hits. Every value was 20 (one soldier type, no armour), so this checks the call,
not every input. The later multipliers (`dword_4DE6D48+1896/1900` under the `sub_133EFB0` 0x200 flags) and
block/parry/counter handling are **not** modelled.

## 3. The relay: `[Coop] PlayerDamageRelay`
Default follows `FightIntents`, read once at load.
- **Machine holding the puppet** (stage 1: host only, since it owns the guards):
  - H1 sees a hit-phase FDE posted to the puppet and scores it with `sub_199A160`.
  - It removes the duplicate (one hit per attacker per 150 ms).
  - It sends `IC_PDAMAGE` (7) on the avatar wire id `'AVATARO2'`: running total, hit count, attacker canonical id,
    damage, contact, subtype, flags.
- **Owner (guest):** posts a **DamageEvent to its own player**, in the same way as `0x1998D40` at `0x1999040..0x19990BE`
  (bytes read, ida_batch pd5):
  1. `sub_1029240()` (new DamageEvent);
  2. `sub_1B86E40(ev, contact, damage, attacker, flags&1, flags&2, 35, 0, 0)` (`retn 20h`);
  3. set +0x1C bits 0x20/0x10, +0x10 = subtype, +0x18;
  4. `EnterCriticalSection(**(player+0xD4))`, then `sub_64D8C0(ev, player)`, then Leave.

  The native CSDamage chain then decides armour, lethal and the HUD. The attacker is the guest's linked copy of the host
  guard when one resolves, otherwise null.
- **Replay safety:** the lane replays the last value (IF_BASE) to a rejoining peer. An owner with no state adopts a
  replayed total with count > 1 as its baseline and applies nothing.

### Live results
**Run 29952 / 13796 (asi bc84dafe, relay 1, probe 1):**
- Host: `[PDR] stats seen=17 dup=4 tx=13`.
- Guest: `rx=13 applied=13 failed=0`.
- Guest's life before each relayed hit:
  `125, 107, 89, 71, 53, 35, 17, 1, 1, 5, 0, 0, 125`.
  That is 20 per hit, minus regeneration in between. The player went down natively and the engine respawned it at 125.
- The probe on the guest shows each relayed hit as `post DamageEvent -> player` followed by
  `csdamage-listener player ... (DamageEvent)`: the native chain ran.
- Double damage: the guest's local guard bodies landed **0** native hits on the guest's Edward (`hit-post player` = 0).
- The attacker resolved to a local body for most hits (`-> local 3ABB01A0`), null for some (`F000B32D`).

**Run 20292 / 26200 (asi f7efdce9, only `FightIntents=1` set, probe 0):** `PlayerDamageRelay=1` by default.
Host `seen=2 tx=2`; guest `rx=2 applied=2`, life 125 → 112.

**PASS** on the oracle stated before the run: every non-duplicate hit-phase post on the puppet becomes exactly one
native DamageEvent on the owner's player, with the engine's damage value, and there is no second native hit on the owner.

## Not established / caveats
- **No hurt reaction (flinch) on the owner's Edward.** A DamageEvent changes health, death and HUD. The hit reaction
  comes from `0x18644D0` (`sub_185FAC0`), which a DamageEvent does not run. Not checked visually.
- **The owner's defence is not consulted.** The host decides that a hit landed from the puppet's state. If the guest
  blocks or counters at that moment, the hit still lands. A causer-requests version would have the owner run the
  resolution itself.
- The ctor value of DamageEvent+0x10 (`0x1B70940`) was not read. The applied damage equalled the sent value (20 → −20
  less regeneration), which is consistent with the type-35 path skipping the recompute.
- The life clamp at 1 (17 → 1, then 1 → 1) is engine behaviour that was not traced.
- Guest-side guards (unlinked bodies running AI) hitting the host's puppet on the guest are not relayed (stage 1).
- n = 2 runs, one soldier group, all hits 20.

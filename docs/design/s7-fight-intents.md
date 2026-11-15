# S7 slice 1: fight decisions on the intention lane (design, 2026-11-15)

Part of `docs/design/storm-replication.md` (S7). Flag: `[Coop] FightIntents`, default 0; it also needs `[Coop] IntentLane=1`.
Both directions are symmetric: each machine publishes its own player's fight decisions and applies the peer's to its puppet.

## Ground truth this slice rests on (all live-verified)
- On a replica (behaviour component off), CanEnterFight 75 → DoEnterFight 76 (handle of the target), PerformCombo 160
  (Entity*, count, ComboEndCondition, bool) and CanExitFight 112 → DoExitFight 113 (ExitFightType) drive the puppet's own
  FSM. The fight state holds, a combo plays, and exit returns to Move (`docs/evidence/s2-puppet-probes-2026-11-15.md`).
- A Do only works right after its Can succeeded (both probes). Calls are made on the puppet's own update (OwnDrain).
- Fight state: ground FSM path (`*(u16**)(fsm+0x118)`, fsm = `[HumanGround+0x10+0x90]`, HumanGround = `*(Human+0x720)`):
  - `path[2] == 163` means in fight;
  - `path[4] == 173` is the attack sub-state seen during PerformCombo;
  - `1/2/3/4/5/6` is Move.
- Fight target: `fight = [fsm+0x74]` (0 outside a fight), focus handle = `[fight+0x1C8]`. After EnterFight(G) this
  equals G's own handle `[G+0xC8]`; a different NPC does not match. Measured on the puppet (2026-11-15, focus_chain check).
- Entity identity across machines: the host's objId is canonical. The guest maps it with `GuestObjFind` /
  `GuestObjFindResolve`, the host with `ResolveEntityById` (phase1.cpp).

## Wire
- New intention channel `IC_FIGHT = 5`. The lane delivers channel ids 1..15; ids not named by a build are passed
  through as they are. Subject: slot 0, wireId `kAvatarWire` (the player avatar, not a guard).
- Payload, 12 B: `u8 op` (1 enter, 2 attack, 3 exit), `u8 comboCount`, `u8 endCondition`, `u8 reserved`,
  `u64 target` (canonical objId; 0 = none).
- The lane persists the last value per (subject, channel), so a rejoining peer receives the current fight state.

## Sender (player's own frame)
- Each frame: read the player's path and focus handle. Find the target entity: the character whose `[E+0xC8]` equals the
  focus handle (m_CharacterList).
  - Not in fight → in fight: publish `enter(target)`.
  - Entering `path[4]==173`: publish `attack(target, 1, Damage)`.
  - In fight → not in fight: publish `exit`.
- Canonical target id: on the host, `GetObjectID(E)`. On the guest, the host objId bound to E in the guest objId
  table; otherwise `GetObjectID(E)`.

## Receiver
- The player's frame drains `Net::PollIntent` and queues `IC_FIGHT` messages for the avatar into a small single-producer
  single-consumer ring.
- The puppet's own update (OwnDrain, puppet branch) pops each op and applies it:
  - **enter:** resolve the target, then Can 75 → Do 76 with `&[E+0xC8]`;
  - **attack:** while the puppet is in fight, PerformCombo 160 (E, 1, Damage, false);
  - **exit:** Can 112 → Do 113 (Normal).
- **Action copy is suspended** while the puppet's own path is in fight (`path[2]==163`). Otherwise the copied alias
  and the FSM both write the puppet's actions. Position copy continues.

## Oracle (two copies, `tools/two_copy.ps1`)
- The host player fights soldiers. With the flag on, the guest log must show, for each host fight:
  - one `[FI] rx enter`, then puppet `path[2]==163` within 30 frames;
  - `[FI] rx attack` count equal to the host's `[FI] tx attack` count, each followed by puppet path[4]==173;
  - `[FI] rx exit`, then the puppet back to Move.
- No crash over the run.
- **Negative control:** the flag off gives no `[FI]` lines and today's behaviour.

## Out of scope (later slices)
Damage and hit outcomes (S9), Attack 154 (its request gate is not understood), counters, parries, assassinations, guard
replicas (S8).

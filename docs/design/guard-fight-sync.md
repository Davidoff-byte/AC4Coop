# Guard fight sync: a guard fighting the guest looks and plays right on the guest's screen

Status: **plan** (2026-11-15). Nothing below is built. Owner's requirement: when a host guard targets the puppet, the
guest sees that guard aim at *them*, with the attack indicators and everything, and a hit lands on the guest's real body;
when the guest dies, the puppet dies.

## What is measured (receipts)
- Host guards reach, circle and attack the puppet with `[Coop] PuppetNavService=1` (f7-strikes-2026-11-15.md).
- Their hits post `FightDamageEvent` (vtable 0x02700720) to the puppet; nothing applies them (f7-puppet-damage-2026-11-15.md).
- The protocol has no player health or death (netstate.h: only ships and guards carry hp/dead).
- Guest-side guards are animation mirrors; the guest's own AI keeps writing actions on them (unity-netcode-mapping.md).

## Decision: defender-authoritative melee, carried as intentions
Options considered:
1. **Relay the host's damage event** to the guest. The host resolves contact against a puppet that replays poses 100+ ms
   late, so a guest who blocks or counters on time can still be hit, and a counter-kill on the guest would not undo the
   host's hit. Divergence by construction.
2. **Relay a damage number** to the guest's health setter. No hurt reaction, ignores blocks. Rejected by the owner.
3. **Replicate the guard's fight intention** (target = the guest, attack phase, weapon) and let the guest's copy of the guard
   perform the attack against the guest's real Edward. The guest's engine shows the aim and the indicators, opens the
   counter window, and resolves hit / block / counter itself. Outcomes go back to the host. **Chosen** (matches
   unity-netcode-mapping.md items 3, 5, 6: intentions on a reliable lane, fights get their own precise path).

Authority after this change:
| fact | decided by | travels |
|---|---|---|
| which guard attacks whom, when (FightManager, pacing, decision tree) | host | host -> guest, intention lane |
| whether the strike hits, is blocked or countered (defender) | guest | guest -> host, outcome event |
| the guest's health and death | guest | guest -> host, player lane field |
| a guard's death | whoever kills it (existing kill relay, host merges) | existing lane |

## Slices (each its own PR, each behind a flag that defaults to 0)
1. **RE, no code**: on the guest, what makes a guard's attack *against the local player* happen natively:
   - the Human action an attack runs (SetTargetAction 0x6625D0 is known as an action writer) and its target argument;
   - what drives the counter indicator and the "about to attack" markers (FightManager attacker list, PS3 names
     `FightManager::SetNPCPerformingAttackAnticipation` / `GetNPCPerformingAttackAnticipation`, FightStage 4
     AttackAnticipation), and whether the guest's FightManager must enlist the mirrored guard against the player;
   - the per-attack data a guard needs (attack type / pattern id, weapon), from the decision tree's action leaves
     (`DecisionTree_Action`, `sub_5470D0(id)`).
2. **Host capture**: per guard fighting the puppet, publish `fight{target=puppet, stage, attack id}` on change (FightTask
   +608 stage is already readable, ftask.py). Uses the intention lane (feat/intent-lane, proto 15, unmerged) channel
   IC_ACTION, target remapped puppet -> "the receiving peer's player".
3. **Guest apply**: drive the mirrored guard's attack against the local player through the seam found in slice 1; enlist it
   in the guest's fight so the indicators come from the guest's own HUD.
4. **Outcomes back**: guest player life/dead on the player lane (proto bump) -> host kills / despawns the puppet with
   the death; guest counter / kill of a guard -> existing kill relay.
5. **Host suppression**: host stops treating its own FightDamageEvents on the puppet as anything (today they are already
   inert; keep it explicit and logged).

## Verification plan
- Slice 1: static (IDA) plus a solo probe: make the local guard attack Edward through the seam and read FightStage and the
  HUD indicator state (oracle: FightStage 4 -> 5/7 on the guard and the indicator flag set, numbers only).
- Slices 2-3 solo: a loopback replica (unity-netcode-mapping.md item 11) or the game running as guest against a scripted
  host peer; oracle = the guest's FightStage sequence matches the host's within one phase, and a hit on Edward goes through
  the player health apply (0x115ACC0) exactly once per landed strike.
- Two-machine run before any flag defaults to 1.

## Open questions
- Whether the intention lane (proto 15) is merged first, or slice 2 rides the existing entity lane's actionAlias.
- Ranged attacks (guards' pistols) follow the same model but need the aim direction on the weapon channel.

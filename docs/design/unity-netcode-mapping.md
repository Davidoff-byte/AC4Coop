# Design input: the AC Unity co-op netcode talk, mapped onto bf-coop

Source: C. Lefebvre, "Networking Gameplay and AI in Assassin's Creed Unity", GDC 2015 (gdcvault 1022168).
Input was the full transcript, without the slides. The talk describes Ubisoft's Unity engine, not AC4.
Everything said here about *their* engine is a design lead. Our own facts carry a `file:line` or an address.
Every AC4 engine claim that depends on this note is `// UNVERIFIED:` until IDA and a live probe back it up.

## 1. The main point, and how far we are from it

The talk is one argument: **convert the single-player game without rewriting it, by replicating at the right
layer.** Every system they show is a variation on that.

| Layer | Their decision for NPCs | What bf-coop does today |
|---|---|---|
| Logic / AI | Off on the replica. The replica is a puppet. | **On.** The guest's own AI context states keep calling `SetTargetAction` 0x6625D0 on mirrored guards (`docs/evidence/action-writers-2026-11-13.md`). `GuardActionGate` blocks the writes one at a time. |
| Intentions | The only thing replicated: a few concurrent channels (animation, navigation, weapon, look-at). One reliable message per change; the last message per channel is persisted. | One `actionAlias` per guard, re-sent on change (`netstate.h` EntityState), with `RESEND_AFTER_CHANGE=3`. Not reliable, no channels, no navigation/weapon/look-at. |
| Human state machine + animation | Runs natively on the replica, driven by the intentions. Animation quality is the same as on the master by construction. | We write the animation layer from outside: phase ratio, time scale, blend-in parameters, per-slot blend factors (`netstate.h:206` GuardAnim), plus the phase lock, interpolation buffer, BlendTypeSafe and FootOffsetFix. |
| Identity | The replica is spawned by the same placed spawner on every peer and keyed by that spawner (net key + seed = spawner id). Master and replica are the same archetype by construction. | Host guard ids are runtime-minted and not shared (`phase1.cpp:5584`). The guest *borrows* the nearest free local NPC of the same archetype within `BindRadiusM` (`phase1.cpp:5588`). |
| Position | Shared in one stream ("proxies"). About 1 m of error was acceptable outside fights. | Per-guard pos/quat at about 10 Hz, 2 mm deadband, interpolation buffer. Target O3 < 5 cm. |

The player puppet is the same picture. Their master sends **final decisions** ("landing on this beam",
"entering this window"). A replica-only state machine turns them into triggers for the native state machine,
adds catch-up transitions and teleports as a last resort. Ours sends ground state, the action alias and the
full blend-factor array (`netstate.h` PlayerState) and drives the animation from outside.

**What follows.** Much of our recent work sits a layer too low:
- Phase lock, blend fixes, interpolation of animation parameters.
- The action gate, which fights the guest's AI instead of switching it off.

It made things measurably better (0 faults; phase 54 → 85%; jitter 64 → 13.5%), but it is fighting symptoms
of two root causes the talk avoids:
- the guest's AI still runs;
- master and replica are not the same entity.

Stated plainly: our O1 ceiling (the StuckProbe result: the switch does not take on a borrowed body) and the
phase targets are the cost of replicating animation instead of intentions.

## 2. What it changes in the plan

Ordered by dependency. Each item needs IDA, then a probe behind a default-0 flag, before it is built.

1. **Puppet mode for mirrored guards (logic off).** Find the seam that skips the AI logic update for one
   body while keeping its human state machine, locomotion and animation running.
   - Lead: `sub_52DF80` = `52C220(0)`, the AIActor context tracks.
   - This replaces `GuardActionGate` as the fix for the second writer.
   - UNVERIFIED: that AC4 separates these two layers. The action evaluator and `SetTargetAction` sit below the
     AI contexts, which suggests it does.
2. **Same-entity identity (spawner-keyed).** Key each guard by the placed spawner or spawn point that created
   it on both machines, instead of minted id + nearest-body binding.
   - Overlaps Track B (`re/guard-spawn-probe`) and the guard-identity session.
   - UNVERIFIED: that AC4 guards come from placed spawners with an id both processes share.
3. **Intention channels on a reliable, persisted lane.** This is S3 piece 1, which should be designed as
   this from the start (proto 15).
   - Channels: action (alias, channel, layer); navigation (destination / move target, speed); weapon
     (drawn/sheathed, which); look-at (target entity).
   - Re-applying the current state must be a no-op. To check in `65DCB0`: does `SetTargetAction` with the
     alias already playing restart it?
   - Each channel keeps its last message for join-in-progress.
4. **Animation stays native.** Once 1–3 hold, drop the per-frame animation writes (GuardAnim, phase lock,
   blend factors) for guards, or keep them only as a fallback flag.
   - Position: correct through the native navigation or locomotion where possible. Snap only past a
     threshold, as their "teleport as last resort".
5. **Player puppet on decisions.** Send the decisions the human state machine makes (action + target, ground
   stage, which parkour/attach target), not the blend array. Add catch-up and a teleport threshold.
   - Larger change. After 1–4, because the guards are where the user sees divergence.
6. **Fights get their own precise path.** Damage, hit reactions, health, death (G3). Ambient mirroring is
   not built for it.
7. **Detection and mission UI run locally; shared state is merged by the host.**
   - Each machine detects against its *own* player and reports outcomes (`detect{guard, level}`): S3 piece 2.
   - The host merges them into shared alert and mission state and replicates that. Only the host's master
     guards receive actions, for example to chase the guest.
   - Consequence for S4 (`re/guard-alert-apply` d47a015): its own doc says the conflict status is the *local
     player's*. So sending the host's per-guard icon fill would show the guest the host's detection meter.
     The guest's icon must come from the guest's own detection. Only alarm and combat come from the host.
   - The sight redirect (`guard_sight.cpp`) stays only to let a host guard target and fight the puppet.
   - Known cost: a round trip of reaction delay.
8. **Contested interactions get one arbiter.** Their tokens exist because peer-to-peer has no authority. We
   have a host, so the host decides: loot, chests, the same assassination target, cutscene start (their
   "wait all"). Nothing in bf-coop arbitrates these today. UNVERIFIED which of them collide in AC4.
9. **Players far apart.** Their world is streamed per peer, so an entity is mastered by whoever has it loaded.
   We are host-authoritative: guards near a guest who is far from the host have no host master. UNVERIFIED
   what our mod does with them now. Per-entity ownership (guest owns its own bubble) is the talk's answer.
   Defer it, but design ids and the reliable lane so ownership can be per entity.
10. **Only important NPCs.** They replicated about 60 (guards, mission characters, unique NPCs); the crowd went
    through a separate cheap system. Restrict guard sync to combat-relevant NPCs; civilians at most get a
    descriptor.
11. **Oracle and tooling.**
    - Their bar: testers cannot tell master from replica, with about 1 m tolerated for ambient NPCs.
    - Add a **loopback replica**: the host spawns a local replica of a guard next to the master and drives it
      from its own outgoing packets with adjustable lag, so master and replica can be compared side by side
      in one process.
    - Log **divergence onsets** (first frame the replica's channel state differs, and why), not only rates.
      Their hardest problem was finding where a divergence began.

## 3. What does not change

- Host authority, the proto 14 timing work (host time stamps, stale-sequence rejection) and the replay rig.
  They are transport and stay useful.
- The BlendTypeSafe and FootOffsetFix crash and float fixes. They are needed for as long as we write
  animation, and they are harmless after that.
- The tested live flags stay as they are until a replacement is verified live.

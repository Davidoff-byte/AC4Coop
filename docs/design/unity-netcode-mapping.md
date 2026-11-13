# Design input: the AC Unity co-op netcode talk, mapped onto bf-coop

Source: C. Lefebvre, "Networking Gameplay and AI in Assassin's Creed Unity", GDC 2015 (gdcvault 1022168).
Input was the talk transcript only, without the slides. The talk is about the Unity engine, not AC4. Each item
below is a design lead: it says what *their* engine did. Nothing here is verified against `AC4BFSP.exe` unless
an address with a receipt is given. Mark every engine claim taken from this note `// UNVERIFIED:` until IDA
and a live probe back it up.

## What the talk describes (paraphrased)

- **Identity.**
  - Every replicated entity gets a 64-bit network key: creating peer + type + generated id.
  - Placed ("deterministic") spawners run on every peer. A token decides which peer spawns the master; the
    others build a replica.
  - The seed is the spawner's own id, so only the key has to travel. Ambient ("non-deterministic") NPCs instead
    send a small descriptor: key, archetype tag and seed.
- **NPC replica = puppet.** On the replica the NPC's logic does *not* run. Between logic and the human state
  machine they added an "intentions" layer:
  - Intentions are a few concurrent channels: animation, navigation, weapon, look-at. Each channel has
    exclusive states and an idle state.
  - Each state change is a reliable RPC. The last RPC per channel is persisted, so a late joiner replays one
    message per channel.
  - Re-executing an intention that is already active is a no-op, so it does not restart. This makes
    replays and migrations seamless.
  - A replica that isn't ready yet (asset not loaded) re-executes once it can.
- **Quality bar.**
  - For ambient NPCs, a 1 m offset was acceptable. Their own verdict on quality was "good, not awesome".
  - Fights were replicated by a separate, more precise path.
  - Their stop criterion: testers could not tell master from replica.
- **Player replica.** The master sends *final decisions*, not inputs or animation (for example "landing on
  this beam"). A replica-only state machine turns those into triggers and adds catch-up transitions when it
  diverges; the last resort is a teleport. It cost more bandwidth than expected, and it needed transitions that
  exist only for the replica.
- **Missions ("coordinators").**
  - Each peer runs the mission's sensors and UI against its *own local player*. They explicitly did not use
    the replica's position, so detection feels right to the player who is being detected.
  - Each peer sends minimal outcomes ("I am detected", "I am in range") to the master. The master merges them
    into shared state and replicates it.
  - Only the master NPCs receive actions.
  - Known cost: noticeable reaction delay, because detection takes a round trip before the guard reacts.
- **Debugging.**
  - Tool: a loopback replica (the local packet stream drives a local replica, with adjustable lag).
  - The hard part was finding where divergence started. That took heavy logging.

## Consequences for bf-coop (our engine facts cited; their engine facts are leads)

1. **Action-match ceiling (O1).**
   - Their replica runs no logic. Ours does: the second writer on mirrored guards is the guest's AI context
     states calling `SetTargetAction` 0x6625D0 (`sub_1D178B0`, `1D138C0`, `1D15690`, `194FC90`;
     `docs/evidence/action-writers-2026-11-13.md`).
   - `GuardActionGate` suppresses those writes one at a time. The talk points to the root fix instead: stop
     the AI logic update on mirrored bodies (lead: `sub_52DF80` = `52C220(0)`, the AIActor context tracks).
   - UNVERIFIED: whether skipping that update leaves the body's locomotion and animation working. Needs IDA,
     then a probe behind a default-0 flag.
2. **Re-execute = no-op.**
   - The talk's replays only work because re-applying the current state does not restart it.
   - To check: does our reassert path (`PuppetActionReassert`, `MirrorActions`) restart an action that is
     already playing? The only no-op exit we know of in 0x6625D0 is `this[2]==dword_2AC1408`, and that is not
     a same-alias test.
   - Needs IDA on `65DCB0`, plus a live counter.
3. **Identity.**
   - Their deterministic path keys the *spawner*, and both peers spawn the same archetype. That is our
     "semantic id" plus Track B (guard-type spawn, branch `re/guard-spawn-probe`).
   - It also explains our measured ceiling: a borrowed body of a different type cannot resolve the host's
     alias. See the StuckProbe result (2026-11-13).
4. **Reliable intentions.**
   - Today `EF_ACTION` is sent on change, with `RESEND_AFTER_CHANGE=3`. The talk's layer is reliable,
     ordered per channel, deduplicated, with the last value persisted per channel.
   - Assigned: piece 1 of the S3 session, proto 15.
   - Channels to add after action: weapon drawn/sheathed, and look-at target. Neither is on the wire today.
     UNVERIFIED: whether the guest diverges on them.
5. **Detection belongs to the detected player's machine.**
   - Implies S3 piece 2: the guest runs its own detection against its real player and reports `detect{guard,
     level}`. The host merges the reports into the shared alert or mission state and drives its master guard.
   - The host sight redirect (`guard_sight.cpp`, `GuardSeesPuppet`) remains the way to make a host guard
     *target and fight* the puppet. It should not be the source of the guest's detection meter.
   - Known cost: a round trip of reaction delay.
6. **Per-guard alert icon: change to the S4 plan.**
   - S4 (`re/guard-alert-apply` d47a015) proposes sending the host's ConflictIcon state (mode, sub-mode,
     attack, fill) per guard. Its own doc (§2) says the world conflict status is the *local player's*.
   - UNVERIFIED but likely: the icon fill is detection progress against the host's player. Copying it would
     show the guest the host's detection meter.
   - Following the talk: the guest's icon should come from the guest's own detection, which needs a
     guest-side probe that CRLDetect runs on mirrored bodies. Only shared states (alarm raised, in combat)
     come from the host.
   - Resolve before building the proto 15 icon field.
7. **Fights need a separate, more precise path.** That is G3 (damage relay, health, death). It should not
   be served by the ambient guard mirror.
8. **Targets.**
   - Our targets (O1 ≥ 98%, O3 < 5 cm) are already stricter than the quality the talk accepted for ambient
     NPCs.
   - Spend precision where players interact: the guard you fight, the guard detecting you, the other player.
     Elsewhere, accept the "can't tell master from replica" bar.
9. **Debugging.** Our `wire_tap.py replay` rig is the counterpart of their loopback replica. Their lesson:
   log divergence onsets, not just steady-state rates. Our `[O5]` and StuckProbe partly do this.

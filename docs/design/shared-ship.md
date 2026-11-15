# Shared ship: both players on one ship (design direction, not started)

Status: **idea recorded 2026-11-15 at the owner's request. Nothing reverse-engineered or built for it yet; every engine claim
below is UNVERIFIED.** Latency budget comes from the EOS two-PC test (docs/design/eos-transport.md): ~400 ms RTT between the
owner's two PCs (different hemispheres), zero loss, low jitter.

## Goal
One player at the helm, the other on deck (or at the swivel guns / rigging), on the same ship, playable at ~400 ms RTT.

## Model
- **One ship, one owner = whoever is at the helm.** The owner's PC simulates the ship and sends its state at about 20 Hz:
  position, rotation, linear velocity, turn rate, sail state. The other PC holds a replica **projected forward to the
  present** by the measured one-way delay (ships are inertial, so prediction is accurate), with errors blended over about
  0.25 s, never snapped.
- **The passenger owns their own body in the ship's frame.** They send a ship-relative state: deck offset, facing, "on ship
  <id>", and on-deck / at-helm / rigging / swivel state. They do not send world coordinates. Each PC places the passenger on
  its own copy of the ship, so the passenger stays glued to the deck even while the replica ship is being corrected. This
  removes the classic "sliding off a moving ship" problem of high-latency shared vehicles.
- **Helm handover** moves ship ownership with one reliable event.
- **Cannons:** the firing player sends one broadside event (side, pitch, charge); the ship owner applies it. Never
  individual projectiles.
- **What the latency costs:** the passenger sees each turn about 200 ms late; the helmsman feels nothing. For a heavy ship
  that is playable.

## Engine questions to answer first (static RE first, IDA on AC4BFSP.exe; PS3 DWARF for names)
1. How a character is attached to a moving ship: ship-local position, a parent entity link, or a ground-context state? Can
   a player walk the deck while the ship moves?
2. Can the guest's **own** Jackdaw be driven from the owner's stream instead of its own sailing physics? This is the cleanest
   replica: the guest stays "Edward on his ship" natively. Start from the ShipMirror work: branches `fix/shipmirror-v2`
   (never run in game) and `re/shipmirror-crash`, plus `ops/evidence/RUN-2026-11-12-F8-boardable.md` and STATUS-TRUTH
   rows 5 and 12.
3. The minimal ship-relative player state, and what helm / deck / rigging / swivel are in the engine.

## Fit with the rest of the plan
- Transport: the EOS bridge. Ship state on the unreliable channel; helm handover, broadsides and boarding on the reliable
  channel.
- Authority: each player owns their own body; the helmsman owns the ship; the host keeps AI ships (roadmap R5 / R7 for
  per-object authority).
- On foot on deck: the native replica approach (intent-driven, not animation replay), placed in ship space.

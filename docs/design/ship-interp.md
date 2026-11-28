# Smooth synced ships: ShipInterp (AI ships) and ShipGhostInterp (the peer's ship) — `src/phase1.cpp`

Two flags, both `[Coop]`, both re-read every second, both default 0:

| flag | ships | keys |
|---|---|---|
| `ShipInterp` | host AI ships bound to a guest AI hull (`ShipClassBind=2`), guest side | `ShipInterpDelayMs` (200), `ShipExtrapMs` (150), `ShipInterpPhysEvery` (1) |
| `ShipGhostInterp` | the peer's own ship as drawn by `ShipGhost=1`, both sides | `ShipGhostDelayMs` (150), `ShipGhostExtrapMs` (150) |

## Why the old drives jittered (read + measured, 2026-11-28)
1. **AI ships: snapshot-rate warps.** `ApplyShips` warped the bound hull to the raw host transform once per applied
   snapshot (`SetGlobalMatrixInternal(&m, true)`); ship snapshots arrive at ~10 Hz (`[Sync] scope … captures=50` per
   5 s), so the hull jumped every 100 ms, and the slow tick also re-applied the previous snapshot.
2. **AI ships: the hull's own simulation drives the entity.** A ship hull's components (dump 28300, class names from
   each vtable's slot-5 descriptor getter): `ShipPhysicComponent` (vt 0x0272D378) → `AbstractWatercraftPhysicComponent`
   → `RigidBodyComponent` → `InteractivePhysicComponent` → `PhysicComponent`; `AIComponent` (vt 0x01E4FAB0); plus
   `SailManager`, `WatercraftAttributes`, `WatercraftElement`, `CustomNavigationComponent`, `NavalTargetingComponent`, …
   With `PhysicComponent::m_DriveEntity = true` the rigid body (buoyancy, AI sail/rudder forces) writes the entity
   after our warp. Measured with per-frame warps and drive left on: hull pulled 0.7–0.9 m per frame from our write
   (p95 2.0 m), 5–11 pops > 1 m per 5 s.
3. **AI ships: the per-frame writer was not per frame.** `ScbInterpFrame` was first called from `EntitySyncTick`, whose
   body runs on every 6th frame unless `EntityFrameCadence=1` (phase1.cpp `slowTick`). Run 2026-11-28 04:32 (asi
   build of `b270c87`+): every judged frame of the acceleration metric was a hitch → the writer ran at ~10–20 Hz.
   It is now called from `OnFrameInner` on the guest, every frame, before `EntitySyncTick`.
4. **The peer's ship: arrival-time stamps.** The ghost (`ShipGhost=1`, a duplicated local ship moved by
   `SetGlobalMatrixInternal` every frame, render override in `LogSetGlobalMatrixBody`) took its transform from the
   puppet buffer (`ComputeInterpolated`). With `PuppetHostTime=0` (template default) each packet is stamped with the
   frame that drained it, so the sender's frame-time jitter and the receive batching show up as uneven speed on a hull
   sailing at 5–15 m/s; and `lerpState` keeps the newer sample's Z (a step per packet when `ShipPeerZ=1`).

## What the flags do
- **ShipInterp.** `ApplyShips` (the binding authority) creates one ring per (hull, host ship) and records
  host-timestamped samples (`OwnHostUs(pkt.hostMs)`). `ScbFeed` (every frame) pushes the newest snapshot's records
  into those rings on the frame it arrives — `ApplyShips` only sees it on the slow tick, and at 200 ms delay 17 % of
  frames were extrapolated (run 05:00). `ScbInterpFrame` (guest, every frame, player thread, `OnFrameInner`) samples
  the ring at `now − offset − ShipInterpDelayMs`: mode 1 position lerp, mode 2 cubic Hermite (tangents by finite
  differences over the real stamps; linear when a neighbour is missing or a step); rotation nlerp; > 30 m between
  samples = step; past the newest sample extrapolated for at most `ShipExtrapMs`, then held. The offset is the ships'
  own minimum (arrival frame − capture) over the current and previous 2 s window, **slewed by at most 5 %** of the
  elapsed time: the shared `OwnOffset` (noted at the slow tick and by the guard lane) moved its window minimum by up to
  ~100 ms, an out-and-back step of 0.5–1 m on an 8 m/s hull (10 pops in pairs, engine drift 0, run 06:10). It warps
  the hull with physics (every `ShipInterpPhysEvery`-th frame; render-only between). While driven, the hull's
  `ShipPhysicComponent.m_DriveEntity` is cleared (vtable checked), the remedy the driven guard mirrors use on their
  character controller; restored when the binding ends (no sample for 500 ms) or the flag goes off.
- **ShipGhostInterp.** Every drained player packet's ship transform is stamped with the sender's capture time
  (`PlayerState::hostTimeMs`, proto 14, QPC ms — the clock of `OwnNowUs`) and mapped onto this clock by the minimum
  (receive − capture) over the current and previous 2 s window (the `OwnOffsetNote` rule, own state: on the host no
  entity snapshots feed `OwnOffset`). Mode 1 interpolates through every packet; mode 2 first averages the packets of
  each `ShipGhostMinGapMs` bucket (mean time, mean position, sign-aligned mean rotation) and runs the same Hermite
  through those samples — the linear path through every ~28 Hz packet kept the sender's capture-time noise as a kink
  per packet (18 % of frames > 15 m/s², run 05:50). The drive samples it every frame at `now − ShipGhostDelayMs`
  and stores the result in `g_ghostCmdPos/Quat`; the render override and `[GhostPose]` use that commanded transform
  (a new ghost uses `g_ghostSync*` until its first drive). Z still goes through `MirroredShipZ` (`ShipPeerZ`).
- **Template values** (measured, two copies on one PC, evidence s15): `ShipInterp=2`, `ShipInterpDelayMs=400`,
  `ShipGhostInterp=2`, `ShipGhostDelayMs=250`, `ShipGhostMinGapMs=90`. The spline needs the sample after the bracket,
  so its delay must exceed two sample periods plus jitter. AI ships are sampled at ~5 Hz (host snapshots on its slow
  tick at ~30 fps): 300 ms used the spline on 48 % of writes (5.0 % frames > 15 m/s²), 400 ms on 97 % (0.25 %). The
  ghost gets a packet every sender frame: 90 ms buckets at 250 ms gave 0.0 %. Changing a delay live steps every hull
  by v × Δdelay once (two 1 m pops at the 400→300 switch).

## Measurement (both modes, for A/B)
- `[ShipSmooth]` (guest, every 5 s) and `[GhostSmooth]` (both copies, every 5 s): the hull as the engine left it
  (AI) / as drawn after our write (ghost), per frame. `jerky` = |step − previous step| > max(25 %, 5 cm); `jitter` =
  apparent acceleration from three consecutive frames > 15 m/s² (a sailing ship changes velocity by < 2 m/s²; frames
  with dt outside 5–40 ms are `hitch`, not judged); `zJit` = the same test on the vertical axis alone; `pops` = steps
  > 1 m (AI: settled rings only). `[ShipSmooth]` also logs `drift` = how far the engine moved the hull since our last
  write (one frame), `underrun`, `spline`/`linear` sample counts, `fed`, the offset in use and the largest
  frame-to-frame change of its target; `[ShipPop]` lines name the hull of each pop. `accP95` = 95th percentile of the
  apparent acceleration per window.
- `[ShipSmooth]` reads the hull before its own write, i.e. what the previous frame left; until asi 85159A69 it stamped
  that with the current frame's time. Synthetic control (linear 8 m/s, frame dt ±2/4/8 ms): 87/94/97 % false jitter
  with that stamp, 0 % with the producing frame's stamp (fixed since asi 117D9734).

## Not covered
- The AI hull's AI still runs (it steers its own body, re-warped every frame); its cannons still fire locally.
- Two machines: the delays are tuned on loopback; on a real link the guard lane's `InterpAdaptive`-style sizing may be
  needed.
- The puppet itself still uses the arrival-time buffer (`PuppetHostTime=0`); only the ship transform moved.

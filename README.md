# AC4Coop

Custom peer-to-peer co-op mod for Assassin's Creed IV: Black Flag (PC, `AC4BFSP.exe`, x86).
Built as an **ASI plugin** (`bin\AC4Coop.asi`), so it needs an ASI loader such as Ultimate ASI Loader in the
game folder.

Two game instances connect over UDP. Each one spawns a **puppet** `Human` that mirrors the other player's
position, orientation and animation.

## Status

Working:

- **Networking.** Host-IP UDP link (`[Net]`), with a snapshot buffer and interpolation (`InterpDelayMs`).
  Works with two instances on one PC (auto port-swap) or across two PCs.
- **Puppet lifecycle.** The puppet spawns automatically when the peer is in the same world. It despawns
  when the peer leaves or disconnects, and is dropped and respawned across world travel.
- **Empty shared world.** All ambient spawning is suppressed (`DisableSpawns=1`), because ambient NPCs are
  seeded independently on each client.
- **Position and orientation.** The puppet is placed every frame with `SetGlobalMatrixInternal`, and
  `CharacterController::m_DriveEntity = false` stops its physics from fighting the placement. This is
  jitter-free in every context.
- **Animation by action replication.** The puppet's behavior component is disabled, so it is a playback
  dummy. The peer's current action is replayed with `SetTargetAction` using the peer's real alias,
  channel and layer, which plays the same clip regardless of context. The local player's
  `CharacterActionSet` is handed to the puppet so Edward's clip overrides resolve.
- **Locomotion gait (walk/jog/run).** The ground move cycle is a 46-slot blend tree (`SimpleAnimBlender<46>`
  → `SetTargetBlendFactors(arr, 0, PRIMARY)`). The whole channel-0 blend array is synced, with
  `ACTPERSIST_ALWAYS` persistence. Syncing only 4 slots was why the puppet always walked.
- **Smooth action switches.**
  - **Blend-in:** the peer's real blend-in descriptor (length, start times, `m_Flags`/`m_BlendType`) is
    copied onto the puppet's new evaluator. A transition-less `SetTargetAction` otherwise gets the
    engine's default blend `{0.2, flags 0x600}`, which is `ACTBLENDTYPE_NONE`, a hard cut. That covers
    state-machine-generated transitions such as crouch→stand.
  - **Phase seek:** the new clip is seeked to the peer's playback ratio (`SeekOnSwitch`).
  - **Minimum blend:** when the peer switched with no blend at all, the puppet uses a short blend
    (`MinBlendInMs`) to hide network timing, and never replaces a blend the puppet found on its own.

The animation side is "good enough for now". See **Known issues / next** below.

## Modes (`AC4Coop.ini` `[Coop]`)

| Setting | Meaning |
|---|---|
| `DisablePuppetAI=1`, `DriveActions=1` | Action replication: the working mode. Keep `DriveContext=0`. |
| `DisablePuppetAI=1`, `DriveActions=0` | Braindead baseline: position only, and the ActionComponent update is killed. Used to isolate position bugs. |
| `DisablePuppetAI=0`, `DriveLocomotion=1` | Legacy: feeds `HumanGroundData` to the puppet's own state machine (MVP path). |
| `HybridLoco=1` | Per-context ownership: on ground the puppet's own state machine animates; everywhere else we replicate actions. With this on, the action/blend sync above runs **only off-ground**. Set `0` to use action replication everywhere. |
| `SeekOnSwitch` | Seek a newly targeted action to the peer's phase (default 1). |
| `MinBlendInMs` | Blend used when the peer's switch had none (default 100, 0 = copy the cut exactly). |
| `DisableSpawns` | Suppress ambient spawning (default 1). |

`[Net]`: `Enabled`, `PeerIP`, `LocalPort`/`PeerPort`, `Interpolation`, `InterpDelayMs`.
`[Display]`: `Fullscreen`, `Borderless`, `ForceWindowedMs` (windowed mode is recommended for two-instance
testing).

## Wire format

`src/netstate.h` holds `PlayerState`, one packed struct per packet. The receiver drops any packet whose
size differs, so **both instances must run the same build**. Main fields:

- the global matrix
- legacy ground data
- `contextId`
- action alias, channel and layer
- `blendCount` + `blendFactors[46]`
- `actionRatio`
- blend-in descriptor (`blendInLen`, `blendInStartFromEnd`, `blendInTargetStart`, `blendInFlags`)
- `timeScale`
- a 128-byte context union (swim, now mostly legacy)

## Build

The user builds it with VS2022/MSBuild (`AC4Coop.sln`, Release|Win32, toolset **v145**). Output is
`bin\AC4Coop.asi`. If v145 isn't installed, pass `/p:PlatformToolset=v143`; it compiles the same.

## Deploy / test

Copy `bin\AC4Coop.asi` and `AC4Coop.ini` next to `AC4BFSP.exe` (with an ASI loader). For a same-PC test,
run two instances with `PeerIP=127.0.0.1` and the default ports.

- **Hotkeys:**
  - **F6** requests a manual puppet spawn (used when `[Net] Enabled=0`).
  - **F7** dumps the player `Human*` and the swim chain to the log.
- **Logs:** written next to `AC4Coop.asi` (the game folder): `phase0_log.txt` and
  `phase1_log_<pid>.txt`. Useful lines:
  - `[Net] cap ...` / `cap SWITCH ...`: the sender's action, blend array and blend-in.
  - `[Net] action -> ...` / `blendIn fix ...`: what the puppet was told to do.
  - `[Net] PUP ...`: read-back of the puppet's actual action, blend array and time scale.

## Engine hooks (raw RVAs, base 0x400000)

| RVA | Purpose |
|---|---|
| `0x12FAACD` | Per-frame main-thread hook in the Human update (6 stolen bytes). All engine calls happen here. |
| `0x54D0` | Focus deactivate: no-pause-on-focus-loss, for running two instances. |
| `0x39B8D0` | `SpawningRequest::Spawn`: declines ambient spawns while the player is in the world. |
| `0x1578B50` | `CrowdHerder::UpdateSpawningNeeds`: no-op. |
| `0x344CF0` | `PlayerPadManager::GetUnusedPadID`: gives the puppet its own empty pad slot, so it registers as a new gamer instead of reusing the player's. |

Every other engine call goes through the Scimitar class wrappers in `src/Scimitar/`. These hardcode
absolute addresses, assuming base 0x400000.

### Key ActionComponent addresses (PC)

| Address | Function |
|---|---|
| `0x6625D0` | `SetTargetAction(alias, flags, ActionTransition*, channel, layer)` |
| `0x666C80` | `GetCurrentAction` |
| `0x666BB0` | channel-controller lookup (controller `+0x28` = current evaluator, `+0x2C/+0x32` = blend factors data/count) |
| `0x652B10` / `0x667590` | `SetBlendFactor` / `GetBlendFactor` |
| `0x652D60` | `SetBlendFactorPersistence` |
| `0x667170` / `0x652640` | `GetCurrentActionRatio` / `SetCurrentActionRatio(ratio, ch, processDisplacement, processEvents, layer)` |
| `0x667540` / `0x6528B0` / `0x652A40` | time scale get / set / persistence |
| `0x651840` | `ActivateCharacterActionSet(Reference<CAS>*)` |
| `0x4C6980` | `ActionController::SetTargetAction` |

Evaluator layout (same on PC and PS3):

| Offset | Field |
|---|---|
| `+0xD4` | type (4 = ActionEvaluator) |
| `+0xE0` / `+0xE4` | blend-in runtime `m_Time` / `m_Length` |
| `+0x1C8` | `m_ActionBlend` (length, startFromEnd, targetStart; `m_Flags` u16 at `+0x1DC`) |
| `+0x258` | `m_IsTransitionAction` |
| `+0x2A8` | `m_Action` |

## Source layout

- `src/dllmain.cpp`: ASI entry, windowed mode, hotkey poll thread, log.
- `src/phase1.{h,cpp}`: frame hook, spawn/despawn, `CaptureLocalState` / `ApplyRemoteState`, interpolation,
  spawn suppression.
- `src/net.{h,cpp}`: UDP transport.
- `src/netstate.h`: wire struct.
- `src/windowed.{h,cpp}`, `src/scimitarCommandLineOptions.h`: windowed mode.
- `src/Scimitar/`: reversed engine classes (`Entity`, `World`, `Human`, `AIActor`, `ActionComponent`,
  `CharacterController`, `HumanSwimFSM`, …).
- `HANDOFF.md`: long-form research and decision log (the older sections describe the superseded
  context-forcing approach).

## Known issues / next

- **Climbing IK:** hand and foot targets aren't replicated, so climb poses are approximate.
- **Phase drift:** the phase is synced only on action switch. Long loops can drift; no continuous
  correction yet.
- **Blend-tree transitions:** while the puppet is mid-transition, `SetTargetBlendFactors` writes to the
  incoming evaluator's `m_InitialBlendFactors` (`+0x2BC`), which isn't mirrored yet.
- **Character overrides:** the CAS is borrowed from the local player, which assumes both players are the
  same character.
- **Leftover code:** the swim context block and the ground-data drive are legacy under action replication
  and can be removed.
- **Pause menu:** the puppet disappears in the pause menu.

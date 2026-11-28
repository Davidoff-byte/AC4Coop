# AC4Coop v2 - architecture

Status: plan of record from 2026-11-28 (owner-approved rewrite). v1 is frozen at tag `v1-final`; its audit is
`docs/v2/AUDIT-2026-11-28.md`. v2 lives in `v2/` (own project `v2/AC4Coop2.vcxproj`, output `bin/v2/AC4Coop.asi`). Only one
of v1 / v2 is installed at a time. Branch `rewrite/v2`, pushed to `fork`.

Scope of the first v2 build: on foot + guards, sailing together (two separate Jackdaws), naval combat + boarding, world +
connection. Two players, written for N. Later: walking on the partner's ship.

## 1. Rules (each one answers a v1 audit finding)
| # | rule | why (audit) |
|---|---|---|
| R1 | Engine writes and engine calls run only in the **game tick**: the local player's Human update (hook 0x016FAACD) on its own thread, from one command queue drained at a fixed point. | 16 worker + 19 job threads touched shared state |
| R2 | **No SEH around engine calls.** SEH only inside `mem::SafeRead*` (pure reads of memory we do not own). A fault in an engine call crashes the game and `crash_ctx` records it. | ~30 statechart/lock calls ran under `__try`; a swallowed fault leaves locks held |
| R3 | Detours on engine/job threads only read immutable double-buffered snapshots (atomic index swap) and push to lock-free queues: no ini read, no file I/O, no engine call, no allocation. | SGM detour tore quaternions, read the ini, wrote past an array |
| R4 | Our own threads (net, logger, init) never call engine functions. | puppet_listener did |
| R5 | **One typed config**, read once at init, printed in full; code defaults = the verified values; no presence gating, no sentinels, no "default follows another flag". | 358 keys, 75 core flags 0 in code, presence-gated hooks |
| R6 | **One HookManager**: every patch registered with expected bytes, owner and purpose; a second patch on the same address is refused; the manifest is printed at boot and checked by `tools/verify_signatures.py`. | ~90 patches, collisions, missing byte checks |
| R7 | **One engine layer** (`v2/engine/`): every engine address and offset used lives there once, with its `decomp/units.json` receipt. Feature code never casts an address. | 7 ship pickers, 6 hull writers, 15 `readable` variants |
| R8 | **One writer per object.** Each replicated object has exactly one driver on each machine; the driver turns the engine's own driver off (AI, `m_DriveEntity`) for as long as it drives, and restores it. | up to 3 writers per ship/guard |
| R9 | **One identity**: `NetId` assigned by the owner, carried on every message. | 7 identity schemes |
| R10 | **One clock**: the host timeline from PeerClock; every stamp on the wire is host-time microseconds. | 5 clock estimators |
| R11 | **One interpolation buffer** (template): cubic Hermite through stamped samples, bounded extrapolation, per-consumer delay. | 4 buffers, 2 Hermites |
| R12 | Probes, test commands and hotkeys compile only in the `DevTools` configuration. The shipping build has none. | 57 probe + 50 harness keys |
| R13 | Every feature has a scenario oracle (section 9) before it ships, and the evidence file names the build. | "it worked last week" regressions |

## 2. Modules
```
v2/
  core/      dllmain (minimal) · init thread · config · log (async ring) · hooks (HookManager) · mem (SafeRead)
             · tick (game tick + command queue) · crash_ctx · build_gate · clock (QPC) · stats (5 s lines)
  engine/    receipts-only wrappers: world/entity/human, character (health, kill, stimulus), ship (naval list,
             hull, sail, water height, state events), spawn (SpawnLib core, census detours), boarding
  net/       transport (UDP socket, lobby, relay) · peers (N-ready table, roles) · peerclock · codec · state stream
             · event channel (reliable, ordered) · registry (NetId <-> local entity)
  sync/      avatar (local capture, remote puppet) · npc (guard binding, drive, actions) · combat (damage/kill
             authority, stimuli, perception) · ships (AI ships: bind, replay, drive) · ghost (peer's ship, helmsman,
             local water) · spawn (policy: host spawns, guest traffic) · naval (damage requests, sinking, boarding,
             objectives) · world (time of day)
  dev/       (DevTools build only) pilot bridge · probes · ShipHurt and other test commands
  tests/     offline unit tests (codec, clock, interpolation buffer, registry, config)
```
Files stay under ~1200 lines. Headers declare; no block-scope `extern`.

## 3. Threads
| thread | runs | may touch the engine |
|---|---|---|
| game tick (player's Human update, hook 0x016FAACD, esi = Human) | `tick::Run()`: drain the command queue, run every sync module's `Tick()` in a fixed order | yes (R1, R2) |
| other Human updates (same hook, other Humans) | per-Human "own update" writes for bodies v2 drives (puppet, bound guards), from the module's published plan | only that Human (R8) |
| engine job threads (detours) | snapshot reads, lock-free queue pushes | no (R3) |
| net receive | decode, push into the per-channel inbox queues | no (R4) |
| logger | drain the log ring to the file | no |
| init | read config, install hooks, start net | no (hooks only) |
Fallback: if the player's update stops reaching the hook (seen at the helm, b52ce83), the tick runs from the next Human update on that thread after 100 ms; the switch is logged (`[Tick] fallback on/off`).

## 4. Config (`AC4Coop.ini`, v2 schema; anything else is ignored and reported once)
```
[Net]   Mode=manual|host|join, PeerIP, PeerPort=47801, LocalPort=47801, BindIP, JoinCode, Relay
[Coop]  Players=2
        Avatar=1, Guards=1, Combat=1, AiShips=1, PeerShip=1, Naval=1, Boarding=1, WorldTime=1, GuestShipTraffic=0
        PuppetDelayMs=100, GuardDelayMs=150, AiShipDelayMs=400, PeerShipDelayMs=250
        SyncRadiusM=60, ShipSyncRadiusM=1000, AlertIds=8,257
[Log]   Level=info
[Dev]   (DevTools build only) Pilot=0, Probes=, ShipHurt=0
```
Every key, its default and its evidence line are listed in `v2/core/config.cpp`; `tools/ini_keys.py` checks the template
against it.

## 5. Net protocol v2 (`net/codec`, `net/reliable`; unit tests in `v2/tests`)
- One UDP socket, owned by the net thread: it alone sends, receives, runs the handshake, the clock and the reliable
  channel. The game tick hands it state bodies and events through SPSC rings (`core/spsc.h`) and drains decoded records
  and events from SPSC rings the other way; nothing is shared under a lock.
- Datagram header (16 B): magic `AC42` (u32) · proto (u16) · sender peer id (u8) · type (u8) · seq (u32, per sender
  toward one receiver, from 1) · evAck (u32, cumulative: every event of the receiver's stream up to it was delivered).
  Datagrams stay ≤ 1200 B (no IP fragmentation).
- Types:
  - `HELLO/WELCOME/BYE`: handshake, peer id assignment, role.
  - `CLOCK`: PeerClock ping/pong (t0, t1, t2 in each side's local µs).
  - `STATE`: unreliable, newest wins per object; a datagram older than the newest seen from that peer is dropped. Body:
    host-time stamp (u64 µs) + count (u8) + records {NetId u32, kind u8, field mask u16, len u8, data ≤ 64 B}; `len`
    lets a receiver skip kinds it does not know.
  - `EVENTS`: reliable, ordered per sender. Body: firstSeq (u32) + count (u8) + the contiguous run of events {type u16,
    NetId u32, len u8, data ≤ 64 B}; count 0 = a pure ack. Go-back-N: the receiver delivers only the next seq; when the
    oldest unacked event has waited max(2 × RTT, 50 ms), the sender rewinds to it. Window 256 events. Measured offline
    (`reliable_*` tests): every event once, in order, intact at 0/2/10/40 % loss with duplicates and reordering; resend
    overhead at 2 % loss, 30 ± 2 ms: 143 % at 1000 events/s (each loss resends the in-flight run), 10.7 % at 50 events/s.
- **Link state**: a peer is live while any datagram arrived in the last 500 ms; `[Net] link down/up` only on that edge, with
  the gap in ms. (Fixes W-P3-1.)
- **NetId**: u32 = owner peer id (8 bits) << 24 | owner counter. Events `Spawned{NetId, kind, binding}` and
  `Removed{NetId}` frame every object's life; a receiver never guesses removal.
- Rates: avatar state every frame (≤ 60 Hz); guards and AI ships 20 Hz (time-based, not frame-based); events as they
  happen.

## 6. Authority and identity
| object | owner | state stream | events | receiver binds by | receiver drives |
|---|---|---|---|---|---|
| player avatar | itself | pose, velocity, action alias, context, ship pose if aboard | fight enter/exit, attack, death, respawn | NetId of the peer's avatar | puppet (`sync/avatar`) |
| player's ship | its player | ship pose, sail state | hull changes, state 91..95 | NetId | ghost (`sync/ghost`) |
| guards/NPCs | host | pose, action alias, fight stage | damage applied, kill, alert | spawn key (static spawn), else host-command spawn | bound body, AI off, interpolated |
| ship crews | host | via the ship | kill | (ship NetId, deck offset) | carried by the ship |
| AI ships | host | pose | hull, state, sink, boarding start/end | host-command replay (SpawnLib) or class + position | bound hull, `m_DriveEntity` off, local water |
| spawns (on demand) | host | - | `Spawned` | owner command | built by key / replay |
| boarding battle, objectives | host | - | start, objective state, outcome | battle NetId | engine activation path |
| world (time of day) | host | - | value every 1 s | - | clock set past 0.05 h |
Damage: the attacker's machine sends `DamageRequest{target NetId, amount, attacker NetId}` to the target's owner. The owner
applies it through the engine path that carries the attacker, and posts the fight/sight stimuli from the attacker's body,
so guards react. The resulting life/death is replicated by the owner.
Proximity: the host syncs what its game has loaded; outside the host's area the guest plays locally and unsynced
(`[Sync] area local` / `area shared` on change). The guest declines its own naval traffic only inside the shared area.

## 7. Clock and interpolation
`net/peerclock`: NTP-style ping/pong at 10 Hz, min-RTT filter over 2 s windows, offset slewed at ≤ 5 % of elapsed time.
Every received sample is stamped in host time. `core/interp` `Buffer<T>`: ring of 16, cubic Hermite when both neighbours
exist and are < 30 m apart, linear otherwise, extrapolation ≤ `ExtrapMs`, step on a gap > 30 m. Consumers set their
delay (two sample periods + jitter).

## 8. Logging and stats
`core/log`: a bounded lock-free queue (8192 lines of up to 480 bytes, Vyukov cells), one writer thread, one file per
process. The file keeps v1's name, `phase1_log_<pid>.txt`, because the tools read it (`pilot.py` picks the newest,
`Lines`/`Len` helpers, `two_copy.ps1`). The first line `[Build] asi=<sha256> commit=<c> tree=<t> version=v2[ devtools]`
names the build; then the config dump and the hook manifest. Each module prints one `[Stats] <module> ...` line every 5 s
with its oracle numbers.

## 8a. Engine layer receipts
`v2/engine/scimitar/` is v1's `src/Scimitar/` reused as is. v1's spine gate never scanned that folder; v2's does
(`tools/spine_gate.py` walks `v2/` recursively). Its 87 engine calls were registered on 2026-11-28 as units named after
their methods, with sizes from the IDB (status `unknown`, no receipts yet). Files no v2 module uses get dropped as the
phases show what is needed.

## 9. Scenario suite (`tools/scenarios/`)
Two copies on one PC (host top-right, guest bottom-left, started from the exe, saves backed up), display kept awake,
driven by `pilot.py`. Each scenario prints PASS/FAIL against oracles fixed in its header, and saves frames.
- **S-boot**: both copies in world, 10 min, no crash, the hook manifest matches.
- **S-link**: 10 min, 0 false link drops, clock error p95 ≤ 2 ms.
- **S-foot**: puppet error p95 ≤ the v1 baseline, animation alias match ≥ 92 %, feet on the ground.
- **S-combat** (Havana): the guest attacks → host guards engage ≤ 5 s, ≥ 3 guard hits on the guest; kills applied 100 % both
  ways.
- **S-sea**: ghost and AI jitter ≤ 1 % at 60 fps, keel in the water, 0 guest-only ships, sails independent both ways,
  30 min.
- **S-naval**: cripple → guest copy crippled ≤ 1 s → board → objectives match → outcome on both.
- **S-world**: time of day within 0.05 h.
- **S-loop**: 30 min, on foot → sea → naval → boarding. Then the two-machine run.

## 10. Porting rule
A v1 piece moves to v2 only with a proven status, the evidence line that proves it, and the scenario that keeps it proven.
It is rewritten into the module, not copied: engine addresses move into `engine/` with their receipts, state into the
module, threading per section 3.

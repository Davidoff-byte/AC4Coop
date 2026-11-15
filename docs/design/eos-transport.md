# EOS transport: connections that work through any NAT, connecting from inside the game

Status: **design + spike plan. Nothing built.** Proposed by the owner 2026-11-15; the owner registered the EOS
product the same day (credentials in `local/eos.ini`, git-ignored — never commit, log or print them).
Extends `lobby-code.md` and supersedes the default path of `relay.md` (see "Relation to existing designs").

## Problem (measured)
- Every host run logs `[Lobby] host internet code ... verdict=DOUBLE_NAT - ... friends outside probably cannot join`
  (`net_log.txt`, runs 2026-11-12..15). The owner's line is double NAT (relay.md: outer router; next hop in
  100.64.0.0/10, carrier NAT likely, not proven). A lobby code is the host's public address, so this player can never host.
- `relay.md` fixed that with a server we ship, but someone must run it on a reachable port. We run no public instance.

## Decision
Use **Epic Online Services** (EOS) for the session layer, not for gameplay:
- **Identity: EOS Connect with a Device ID.** No Epic account, no login UI: `EOS_Connect_CreateDeviceId`, then
  `EOS_Connect_Login` with `EOS_ECT_DEVICEID_ACCESS_TOKEN`; on `EOS_InvalidUser`, `EOS_Connect_CreateUser`. The result is a
  ProductUserId (PUID) per install.
- **Rendezvous: EOS Lobbies.** The host creates a lobby (max 2 members today, see below) with a short random code as a
  public attribute. The guest searches by that attribute and joins. No IP address is shown or typed.
- **Transport: EOS P2P.** NAT punch-through first, Epic relay as the fallback (`EOS_P2P_SetRelayControl`). Unreliable and
  reliable channels on one socket name.
- **Not used:** Epic accounts, friends, presence, the Epic overlay (create the platform with the overlay disabled), stats,
  achievements, anti-cheat. Nothing touches Ubisoft online services (AGENTS.md).
- **EOS is store-neutral.** relay.md rejected a store's lobby/relay service because players own AC4 on Steam *and* Ubisoft
  Connect; EOS Connect + Device ID needs neither store, so that objection does not apply.

**Checked against the SDK headers** (EOS SDK v1.19.2.1, `EOS-SDK-58105819-Release-v1.19.2.1.zip`, sha256 56A3BD80…B582FB,
extracted to the git-ignored `local/eos-sdk/`). Every call above exists. One name was wrong: forced relays is
`EOS_RC_ForceRelays` (enum `EOS_ERelayControl`, `eos_p2p_types.h`), not `EOS_RNP_ForceRelays`.

## SDK finding: no 32-bit Windows binary (changes where EOS runs)
v1.19.2.1 ships `EOSSDK-Win64-Shipping.dll/.lib` and arm64 only. There is **no `EOSSDK-Win32-Shipping`**, even though
`eos_base.h` still defines `EOS_CALL __stdcall` for `_M_IX86`. AC4 is a 32-bit process, so EOS cannot load inside it.
**Decision (proposed):** a small **64-bit bridge process** shipped with the mod runs EOS.
- The mod keeps its UDP transport and talks to the bridge on `127.0.0.1`.
- The bridge relays each datagram over EOS P2P and serves the in-game panel's host/join requests over the same local link.
- This keeps the current SDK. The alternative (an older SDK that still shipped Win32) builds on a version Epic may retire.

## Where it plugs in (our code, verified)
- `src/net.h` / `src/net.cpp` own one UDP socket and a receive thread. The payload formats do not change:
  `PlayerState` (576 B, `static_assert` in `src/netstate.h:138`), the entity lane (chunked, `ENTITY_CHUNK_MAX` = 1400 B,
  at most 12 chunks, `receivedMask` u16, `src/net.cpp:192-217`), kill events, and the proto-15 intention lane (merged
  into `feat/launcher` as fb1bc59).
- New: a transport seam under `Net` — `Send(bytes, channel, reliable)` / `Poll()` — with two implementations:
  `UdpTransport` (today's socket: LAN, manual PeerIP, lobby codes) and `EosTransport`. `[Net] Transport=udp|eos`, default
  `udp` until verified live.
- **EOS packet limit:** EOS P2P caps a packet at 1170 B (`EOS_P2P_MAX_PACKET_SIZE`, UNVERIFIED). `PlayerState` fits.
  Entity chunks must shrink to <= 1170: `ENTITY_CHUNK_DATA` becomes 1170 - header (36 B, proto >= 14), giving at most
  ceil((16384-32)/1134) = 15 chunks, which still fits the u16 `receivedMask`. The offline wire harness (SelfTestWire) is
  the oracle for that change.
- **Threading:** EOS is not thread-safe. Every EOS call, including `EOS_Platform_Tick`, runs on our net thread, which
  already exists. The game thread never calls EOS.
- **Binary:** AC4 is x86, so we need the Win32 SDK (`EOSSDK-Win32-Shipping.dll/.lib`). The DLL ships next to the ASI. The
  launcher installs it with the other files.
- **Credentials at build time:** a build step generates `local/gen/eos_creds.h` from `local/eos.ini` (git-ignored). A
  release binary contains the client id and secret; that is normal for EOS clients. The **client policy** on the Epic portal
  limits that client to Connect (Device ID), Lobbies and P2P.

## In-game flow (connect after loading into the world)
1. The mod starts EOS on the net thread at load (platform + Device ID login, about 1 s, no UI).
2. In the world, the overlay panel (today's lobby panel; it moves to ImGui over the game's present hook,
   `UiProbe` already measures that path) offers **Host** and **Join**.
3. Host: create a lobby and show the code (copy button). Guest: type the code; search, join, then the P2P connection
   request is accepted on the host. The existing HELLO/WELCOME handshake runs over EOS P2P unchanged.
4. Leave: leave or destroy the lobby, then close P2P.
5. The launcher becomes install/update + build check + options + launch. Its party networking (`launcher/party_net.cpp`)
   is retired once the in-game flow is verified.

## Relation to existing designs
- `lobby-code.md` (direct codes, UPnP/STUN): kept for LAN and for players who want no third-party service.
- `relay.md` (our relay server): kept on the shelf. EOS relays replace it as the default internet path, so nobody has to run
  a server.
- Multi-guest: EOS lobbies hold more members, but the plugin is still one `g_peer` / one puppet. That is a separate project.

## Spike (standalone x86 console app, outside the game) — the oracle before any mod change
`tools/eos_spike/` (source committed; SDK and credentials never committed). Run two instances on one PC:
1. Platform create + Device ID login on both (separate Device IDs: per-instance model names).
2. Instance A creates a lobby with code attribute; B searches by code and joins.
3. P2P: B requests a connection; A accepts. 1000 unreliable packets of 576 B and 200 of 1170 B, plus 200 reliable ordered
   packets. Measure loss, ordering and RTT.
4. **Repeat with relays forced** (`EOS_RNP_ForceRelays`): this exercises Epic's relay path on one machine, which is the path a
   double-NAT host needs. A real two-network test still needs a second machine on another network (machine B).
- PASS = both phases deliver all reliable packets in order, unreliable loss under 1 % locally, relay RTT reported, and a clean
  shutdown (lobby destroyed, no leaked handles).

## Spike results (2026-11-15, `tools/eos_spike`, x64, run on the owner's PC)
- **Host (verified against Epic's live backend):** EOS_Initialize OK, platform created (overlay disabled), Device-ID login OK
  (ProductUserId issued), relay control set, **NAT type Moderate**, lobby created with a public `CODE` attribute (`EHGR7E`).
- **Join by code (verified):** lobby search on the `CODE` attribute returned 1 result, and JoinLobby succeeded.
- **P2P: NOT tested yet.** Both copies on one Windows user got the **same ProductUserId**. Device-ID credentials are per OS
  user profile ("already exist for the current user profile"), not per `CacheDirectory`. The guest's packets to "the host"
  were delivered locally (RTT 0.0 ms) and the host echoed nothing, so that run's PASS is void. The spike now refuses this case
  (`VERDICT: INVALID`, exit 4).
- **Two PCs in different hemispheres (owner's PCs, 2026-11-15): P2P PASS.**
  - Runs 1-2 (build 9a1a125) were invalid as measurements. The first phase was sent before the link was up (0 echoes back),
    and 200 packets/s queued behind a ~400 ms link, giving 0.4-4.4 s RTT. Fixed in 0f1c55c: wait for the
    connection-established event, report DIRECT/RELAYED, send at 60/30 Hz.
  - **Run 3 (0f1c55c):** host `000252c6…`, guest `000209b6…`, both NAT Moderate. Lobby by code, then connection established.
    The guest reported **DIRECT** and the host reported **RELAYED** (both lines quoted from the two logs; asymmetric, meaning
    UNVERIFIED).

    | phase | lost | RTT median / p95 |
    |---|---|---|
    | unreliable 576 B @60 Hz | 0 / 600 | 399.3 / 411.8 ms |
    | unreliable 1170 B @30 Hz | 0 / 300 | 406.9 / 423.8 ms |
    | reliable-ordered 576 B @30 Hz | 0 / 300 | 400.3 / 414.0 ms |

    The host echoed 1201 packets (hello + 600 + 300 + 300 = everything). `VERDICT: PASS`.
  - **Reading:** EOS P2P works across the world with no loss and low jitter. The ~400 ms RTT is the intercontinental path,
    which co-op must be designed for (intent-based replicas, no round-trip waits); EOS cannot remove it.
- **Still open:** a `--relay force` run (both sides) to measure the relay-only path. Then the 64-bit bridge.

## The bridge (`tools/eos_bridge`, x64)
**Data path.** The bridge binds UDP `127.0.0.1:<BridgePort>` (default 47900, loopback only, never exposed). Every datagram from
the local game goes out over EOS P2P (socket `AC4COOP`, channel 1, unreliable-unordered: the mod already has its own
reliability and staleness rules, and the intention lane carries its own sequencing). Every P2P datagram from the remote
bridge goes to the local game, at the last local sender's endpoint (default `127.0.0.1:47801`).
- Datagrams larger than `EOS_P2P_MAX_PACKET_SIZE` (1170) are dropped and counted. Entity chunks (1400 B today) need the
  `ENTITY_CHUNK_MAX` change in `net.cpp` before the mod runs over the bridge.

**Roles.** `host` creates the lobby and prints the code; `join <CODE>` finds it, joins, and opens P2P to the lobby owner.
The host accepts the first peer that requests a connection. Two players today, matching the mod's single `g_peer`.

**Slices.**
1. Bridge exe with the console roles above, stats every 5 s, and `--echo` (send P2P datagrams back instead of to the
   game) for tests. Oracle: `udp_probe.py` here → bridge (host) → EOS → remote bridge (`--echo`) → back; loss, order and RTT.
   **No mod change** in this slice.
2. The mod runs over it. `[Net] Mode=manual`, `PeerIP=127.0.0.1`, `PeerPort=47900`, `BindIP=127.0.0.1`, entity chunks of
   1170 B or less. The game's HELLO/WELCOME runs end to end through two bridges (needs a game on both PCs).
3. In-game panel (ImGui) drives the bridge over a local control protocol (HOST / JOIN code / STATUS / LEAVE); the mod
   starts and stops the bridge process; the launcher installs it.

## Open items
- The owner sets the client policy on the Epic portal (Connect / Lobbies / P2P only) before a public build.
- EOS terms and the SDK licence for redistributing `EOSSDK-Win32-Shipping.dll` with a game mod: read before release.
- Code format: 6 characters from an unambiguous alphabet. The search is by lobby attribute, so collisions are handled by
  retrying.

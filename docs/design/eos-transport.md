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

API names above are from the EOS SDK documentation as known before the SDK was installed. **UNVERIFIED against the
headers** until the SDK is in `local/eos-sdk/`; the spike's first step is to confirm every call and constant.

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

## Open items
- The owner sets the client policy on the Epic portal (Connect / Lobbies / P2P only) before a public build.
- EOS terms and the SDK licence for redistributing `EOSSDK-Win32-Shipping.dll` with a game mod: read before release.
- Code format: 6 characters from an unambiguous alphabet. The search is by lobby attribute, so collisions are handled by
  retrying.

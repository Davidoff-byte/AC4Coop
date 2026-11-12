# ac4coop-relay: room codes that work through any NAT

Status: **server built and checked offline (slice 7); plugin side not built yet (slices 8–9).**
Decided 2026-11-12. Extends `docs/design/lobby-code.md`; does not replace it.

## Problem
A lobby code (lobby-code.md) is the host's public address, so it only works when the host can receive an
unsolicited packet. Measured on the owner's line 2026-11-12: double NAT (UPnP external address private,
tracert hop 2 = the outer router), and the next hop is in 100.64.0.0/10 (carrier NAT: likely, not proven).
That player can never host by code. Players own the game on Steam and on Ubisoft Connect, so a store's
lobby/relay service cannot be the answer (and Ubisoft online services are out of scope, AGENTS.md).

## Decision
A small server we ship, `ac4coop-relay`, that anyone with one reachable UDP port runs: a player whose
`ac4coop_netcheck` says CAN HOST, or a cheap/free VPS. We run no public instance; `[Net] RelayServer=host:port`
is empty by default and then nothing below happens. It needs neither the game nor any store client.
(Inspiration: ReSkate's dedicated server. ReSkate is GPL-3.0; no code was taken.)

## How it works
- **Rooms.** The host REGISTERs from its game socket and gets a random 40-bit room id. The player-facing code
  is a normal 12-character lobby code whose IPv4 first octet is 255 (reserved, never a real host); the other
  40 bits are the room id. Room codes therefore always start with `Z`. Direct codes are unchanged.
- **Rendezvous first.** A guest JOINs; the server sends each side the other's public endpoint (as the server
  sees it, i.e. the NAT mapping of the game socket) and its LAN endpoint (PEER). The guest sends HELLO to the
  host's endpoints; the host sends `LOBBY_PUNCH` to the guest's endpoints, which opens the host's NAT. The
  existing HELLO/WELCOME handshake then locks the peer, and game traffic flows directly.
- **Relay when that fails.** No WELCOME 3 s after PEER (or `[Net] RelayForce=1`): both sides wrap datagrams
  in DATA to the server, which forwards them to the addressed member (or all members, slot 0xFFFF).
- **Multiplayer-ready.** A room holds a host (slot 0) and up to 7 guests. The plugin is still 2-player
  (one `g_peer`, one puppet); multi-guest in the plugin is its own project.

## Wire (src/relay_proto.h; tools/relay_peer.py pins it)
16-byte header `{u32 'AC4R', u8 version=1, u8 type, u16 slot, u64 room}` + payload:
REGISTER/JOIN `{gameProto, lanIp, lanPort, nonce}` (24 B) · REGISTERED `{nonce}` · PEER `{pubIp, pubPort,
lanIp, lanPort, nonce}` · DATA (payload = one game datagram) · ERR `{code, nonce}` · KEEPALIVE (guest, every
15 s; the host re-REGISTERs every 15 s). Errors: unknown room, room full, game protocol differs, relay
version differs, not a member.

## Abuse limits
Forwarding only between members of one room, never to an address a client names. No reply to a non-member's
DATA/KEEPALIVE; every reply to an unauthenticated request is no larger than the request (checked).
400 datagrams/s per source endpoint (burst 800). Guests leave after 30 s of silence, rooms close 60 s after the
host goes silent. A REGISTER with a known room id from a new endpoint is accepted only if the old host has been
silent 30 s; an unknown non-zero id is re-created, so a host's code survives a server restart.

## Privacy
The server sees every member's IP address and port (it must, to introduce them) and logs room events with
them. Its operator is whoever runs it; players should only use a server run by someone they trust.

## Verified (2026-11-12, offline, one PC, server built with MSVC /W4 and g++ MinGW)
`tools/relay_check.py --slow` against the server: 13/13 PASS (register, retry, unknown room, protocol, relay
version, PEER both ways, room full at 8, DATA routing incl. broadcast 7/7, stranger gets nothing,
amplification, restart id, flood rate-limited (1194 of 2000 dropped), expiry at 60 s).
`tools/relay_peer.py` host + join: DIRECT path both ways (server forwarded nothing), and `--force-relay`:
RELAY path both ways (server forwarded 224 datagrams, 131 kB).
Not verified: the Linux build (no Linux toolchain here: MinGW compiles the Windows branch), any real NAT.

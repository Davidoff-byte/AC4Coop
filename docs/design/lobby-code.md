# Lobby codes: host shows a code, guest types it to join

Status: **design, not implemented.** Decided 2026-11-11: serverless, one host plus one guest.

## Problem
Today both players edit `[Net] PeerIP / LocalPort / PeerPort` in `AC4Coop.ini` with each other's addresses,
and internet play has needed a VPN. Every test session starts with that setup. The goal is that the host
starts the game and gets a code, the guest enters the code, and they are connected, with no IP addresses
typed and no VPN.

## Decisions
- **Serverless.** No rendezvous server of our own. The code itself carries the host's public address.
- **One host, one guest.** The mod has one `g_peer`, one puppet and one `PlayerState` stream
  (`src/net.cpp`, `Init()` ~line 756). Multi-guest is a separate project.
- **Manual mode stays.** The current `PeerIP` setup remains the default and keeps working. Lobby mode is opt-in.

## How it works

### Why only the host needs to be reachable
The guest sends first. Its own router then creates a mapping, and the host's replies come back through it.
So only the host's router must accept an unsolicited packet. That is what the port mapping is for.

### Host
1. Bind `LocalPort` as today.
2. **UPnP-IGD**: SSDP `M-SEARCH` to `239.255.255.250:1900`, fetch the gateway's description, then
   `AddPortMapping(UDP, ext=LocalPort → this PC's LAN IP:LocalPort, lease 7200 s)` and `GetExternalIPAddress`.
3. **STUN check**: send one Binding Request from the game socket to a public STUN server
   (`[Net] StunServer`, default `stun.l.google.com:19302`). This gives the address the internet actually sees.
   - The STUN address equals the UPnP external IP → the code will work.
   - The UPnP external IP is private or `100.64.0.0/10`, or it differs from the STUN address → **CGNAT**.
     The code will not work from outside. Log it plainly and still show the LAN code.
   - No UPnP gateway → show the STUN address with a warning: it works only if the router forwards the
     port (manual port forward, or a NAT that accepts it).
4. Show the code: **window title** (`AC4Coop | host code K7M2-9QXD-4A`, via `windowed.cpp`), the log, and
   the clipboard. A **LAN code** (LAN IP) is shown too, for same-network and same-PC testing.
5. Wait for `HELLO`. Lock `g_peer` to the first guest whose `HELLO` carries the right protocol version and
   code check. Reply `WELCOME`. Drop everything from other sources (the existing strict-peer gate, applied to
   the learned address instead of `PeerIP`).
6. On exit, call `DeletePortMapping`. The lease expires on its own if the game crashes.

### Guest
1. The code comes from `[Net] JoinCode=` in the ini, or from a **paste hotkey** that reads the clipboard.
   The mod has no in-game text input.
2. Decode it to `IP:port`, set `g_peer`, then send `HELLO` every 250 ms until `WELCOME` arrives or 15 s pass.
   The window title and the log say what happened.

### The code
- 32-bit IPv4 + 16-bit port + 2-bit check = 50 bits → **10 Crockford base32 characters**, shown as
  `XXXX-XXXX-XX`. Crockford base32 has no I/L/O/U, is case-insensitive and ignores dashes when typed.
- No secret is in it: anyone who knows the code and runs the same build can take the guest slot. That is
  acceptable for testing between friends. An optional 20-bit session token (14 characters) can be added
  later without changing anything else.

### Wire
New packet `NET_LOBBY_MAGIC` with `HELLO {protoVer, nonce, codeCheck}`, `WELCOME {protoVer, nonce}`, and
`BYE`. This bumps `NET_PROTO_VER` 11 → 12. The `PlayerState` and entity layouts do not change.

## Where it can fail (and what the user sees)
| case | result |
|---|---|
| UPnP disabled on the host router | STUN code plus a "forward UDP <port> or enable UPnP" message |
| host behind CGNAT (some mobile and ISP lines) | LAN code only, "CGNAT: internet join impossible without a VPN or relay" |
| guest typed a wrong code | the check fails locally ("code invalid"), nothing is sent |
| different builds | `HELLO` protoVer mismatch → both sides log it, no connection |

UPnP and STUN run on a background thread, never on the game thread. `Net::Init` is called from the first
in-world frame (`src/phase1.cpp:9575`).

## External contact
In lobby mode the host contacts a public STUN server once per session; it sees the host's IP. The server is
configurable and `StunServer=` (empty) disables the check. Manual mode contacts nothing.

## PR stack
1. **This design** (docs only).
2. **Code format**: `src/lobby_code.{h,cpp}` with encode/decode and a startup selftest, plus `tools/lobby_code.py`
   giving the same encoding in Python. No behaviour change.
3. **Handshake + learned peer**: `[Net] Mode=manual|host|join` (default `manual`), HELLO/WELCOME, `g_peer` learned
   on the host, proto 12. LAN codes work at this point. `tools/peer9.py --join CODE` for solo testing.
4. **UPnP** port mapping, external IP, cleanup on exit.
5. **STUN** check and CGNAT detection, with the messages in the table above.
6. **Display and input**: window title, clipboard copy, paste-to-join hotkey.

## Verification
- Slices 2–3: Python/C++ encode round trip (the same vectors on both sides); peer9 joins a running host with a
  LAN code on one PC; a wrong code and a wrong protoVer are both refused (negative controls).
- Slices 4–5: on the host PC, the router's UPnP table shows the mapping and it is gone after exit. Ideally a
  phone hotspot as the guest network gives a true outside join.
- Done when: two machines on different networks connect by code alone, with no ini IP edits and no VPN.

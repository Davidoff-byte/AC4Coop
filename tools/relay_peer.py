#!/usr/bin/env python3
"""relay_peer.py - Python stand-in player for the ac4coop-relay path (docs/design/relay.md).

Speaks the relay protocol (src/relay_proto.h) plus the lobby handshake (LobbyPacket, src/netstate.h) and then
streams minimal 576-byte PlayerStates (valid=0, so a game applies nothing) to prove the link both ways.

  py tools\\relay_peer.py host --server HOST:PORT [--seconds 60] [--force-relay] [--proto N]
      REGISTER a room, print its Z... code, keep it alive; when a guest arrives, punch towards it and accept
      its HELLO directly, or through the server (DATA) when --force-relay or after 3 s without a direct HELLO.
  py tools\\relay_peer.py join CODE --server HOST:PORT [--seconds 20] [--force-relay] [--proto N]
      JOIN the room, then HELLO the host directly (public + LAN endpoints); fall back to the server after 3 s
      (or at once with --force-relay). Exit 0 only if WELCOMEd AND game datagrams came back.
  py tools\\relay_peer.py --selftest
      Layouts, the room-code mapping, and every constant checked against src/relay_proto.h + src/netstate.h.
Exit: 0 pass, 1 fail, 2 usage. Standard library only.
"""
from __future__ import annotations

import argparse
import random
import re
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lobby_code  # noqa: E402
import lobby_peer  # noqa: E402  (LobbyPacket + PlayerState helpers, constants pinned to netstate.h)

ROOT = Path(__file__).resolve().parents[1]
RELAY_H = ROOT / "src" / "relay_proto.h"

MAGIC, VERSION, HOST_SLOT, ALL_SLOTS, MAX_MEMBERS, MAX_DGRAM = 0x52344341, 1, 0, 0xFFFF, 8, 1500
REGISTER, REGISTERED, JOIN, PEER, DATA, ERR, KEEPALIVE = 1, 2, 3, 4, 5, 6, 7
ERRS = {1: "unknown room", 2: "room full", 3: "game protocol differs", 4: "relay version differs", 5: "not a member"}
HDR = "<IBBHQ"          # 16 B
HELLO = "<IIHHIII"      # 24 B
REGD = "<II"            # 8 B
PEERF = "<IHHIHHI"      # 20 B
ERRF = "<HHI"           # 8 B
LOBBY_PUNCH = 4         # LobbyPacket type, opens the sender's NAT towards a guest; receivers ignore it


def hdr(ptype, slot, room):
    return struct.pack(HDR, MAGIC, VERSION, ptype, slot, room)


def parse(data):
    if len(data) < 16:
        return None
    magic, ver, ptype, slot, room = struct.unpack_from(HDR, data)
    return None if magic != MAGIC else (ver, ptype, slot, room, data[16:])


def ip_int(s):
    return struct.unpack(">I", socket.inet_aton(s))[0]


def ip_str(v):
    return socket.inet_ntoa(struct.pack(">I", v))


def room_code(room):
    ip = 0xFF000000 | ((room >> 16) & 0xFFFFFF)
    return lobby_code.encode(ip_str(ip), room & 0xFFFF)


def code_room(code):
    d = lobby_code.decode(code)
    if d is None:
        return None
    ip = ip_int(d[0])
    return None if (ip >> 24) != 0xFF else ((ip & 0xFFFFFF) << 16) | d[1]


def resolve(hp):
    h, p = hp.rsplit(":", 1)
    return socket.gethostbyname(h), int(p)


class Link:
    """The game-side view: one socket, a peer that is either direct (ip, port) or relayed (server + slot)."""

    def __init__(self, sock, server, room):
        self.sock, self.server, self.room = sock, server, room
        self.direct = None      # (ip, port) once the direct path is chosen
        self.slot = None        # peer slot on the server when relayed
        self.c = lobby_peer.Counter()
        self.tx = 0

    def send(self, payload):
        if self.direct:
            self.sock.sendto(payload, self.direct)
        else:
            self.sock.sendto(hdr(DATA, self.slot, self.room) + payload, self.server)

    def unwrap(self, data, src):
        """-> (payload, via) for a game/lobby datagram from our peer, else None."""
        r = parse(data)
        if r:
            ver, ptype, slot, room, body = r
            if src == self.server and ptype == DATA and room == self.room:
                return body, f"relay(slot {slot})"
            return None
        if self.direct and src == self.direct:
            return data, "direct"
        return None


def stream(link, seconds, proto, sock, server, room, keepalive_type):
    sock.settimeout(0.01)
    seq = 0
    t_end = time.monotonic() + seconds
    nxt = ka = 0.0
    while time.monotonic() < t_end:
        now = time.monotonic()
        if now >= nxt:
            link.send(lobby_peer.player_state(seq, proto)); seq += 1; link.tx += 1; nxt = now + 0.05
        if now >= ka:
            sock.sendto(keepalive_type(), server); ka = now + 15
        try:
            data, src = sock.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        u = link.unwrap(data, src)
        if u and lobby_peer.unpack_lobby(u[0]) is None:
            link.c.add(u[0])


def lan_hello(sock, proto, nonce):
    lan = lobby_peer.lan_ip() or "0.0.0.0"
    return struct.pack(HELLO, proto, ip_int(lan), sock.getsockname()[1], 0, nonce, 0, 0)


def cmd_host(server_hp, seconds, force, proto, port):
    server = resolve(server_hp)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    nonce = random.getrandbits(32)
    sock.settimeout(1.0)
    room = None
    for _ in range(10):
        sock.sendto(hdr(REGISTER, 0, 0) + lan_hello(sock, proto, nonce), server)
        try:
            data, src = sock.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        r = parse(data)
        if r and src == server and r[1] == REGISTERED:
            room = r[3]; break
        if r and r[1] == ERR:
            code, _, _ = struct.unpack_from(ERRF, r[4]); print(f"[relay_peer] RESULT host=ERR {ERRS.get(code, code)}"); return 1
    if room is None:
        print("[relay_peer] RESULT host=NO-SERVER (no REGISTERED in 10 s)"); return 1
    print(f"[relay_peer] host room {room:010x} code {room_code(room)} (server {server[0]}:{server[1]}, local :{sock.getsockname()[1]})", flush=True)
    link = Link(sock, server, room)
    t0 = time.monotonic(); ka = 0.0
    guest = None        # (slot, pub, lan, nonce)
    punch_until = 0.0
    sock.settimeout(0.05)
    while time.monotonic() - t0 < seconds:
        now = time.monotonic()
        if now >= ka:
            sock.sendto(hdr(REGISTER, 0, room) + lan_hello(sock, proto, nonce), server); ka = now + 15
        if guest and not force and now < punch_until:
            for ep in (guest[1], guest[2]):
                if ep[1]:
                    sock.sendto(lobby_peer.pack_lobby(LOBBY_PUNCH, guest[3], 0, proto), ep)
        try:
            data, src = sock.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        r = parse(data)
        if r and src == server and r[1] == PEER:
            pub_ip, pub_port, _, lan_ip, lan_port, _, gnonce = struct.unpack_from(PEERF, r[4])
            guest = (r[2], (ip_str(pub_ip), pub_port), (ip_str(lan_ip), lan_port), gnonce)
            punch_until = now + 3
            print(f"[relay_peer] PEER guest slot {r[2]} pub {guest[1][0]}:{guest[1][1]} lan {guest[2][0]}:{guest[2][1]}", flush=True)
            continue
        payload, via = None, None
        if r and src == server and r[1] == DATA and r[3] == room:
            payload, via = r[4], "relay"
        elif not r and guest and src in (guest[1], guest[2]) and not force:
            payload, via = data, "direct"
        lp = lobby_peer.unpack_lobby(payload) if payload else None
        if lp and lp["type"] == lobby_peer.HELLO:
            if via == "direct":
                link.direct = src
            else:
                link.slot = r[2]
            link.send(lobby_peer.pack_lobby(lobby_peer.WELCOME, lp["nonce"], 0, proto))
            print(f"[relay_peer] guest HELLO via {via}: WELCOME sent; streaming", flush=True)
            left = max(5.0, seconds - (time.monotonic() - t0))
            stream(link, left, proto, sock, server, room, lambda: hdr(REGISTER, 0, room) + lan_hello(sock, proto, nonce))
            ok = link.c.total() > 0
            print(f"[relay_peer] RESULT host=OK path={'DIRECT' if link.direct else 'RELAY'} tx_player={link.tx} {link.c} "
                  f"link={'BOTH-WAYS' if ok else 'ONE-WAY(no rx)'}")
            return 0 if ok else 1
    print(f"[relay_peer] RESULT host=NO-GUEST in {seconds:.0f} s")
    return 1


def cmd_join(code, server_hp, seconds, force, proto):
    room = code_room(code)
    if room is None:
        print(f"[relay_peer] {code!r} is not a room code (invalid, or a direct code)"); return 1
    server = resolve(server_hp)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    nonce = random.getrandbits(32)
    sock.settimeout(0.5)
    peer = None
    for _ in range(20):
        sock.sendto(hdr(JOIN, 0, room) + lan_hello(sock, proto, nonce), server)
        try:
            data, src = sock.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        r = parse(data)
        if not r or src != server:
            continue
        if r[1] == ERR:
            ecode, _, _ = struct.unpack_from(ERRF, r[4]); print(f"[relay_peer] RESULT join=ERR {ERRS.get(ecode, ecode)}"); return 1
        if r[1] == PEER:
            pub_ip, pub_port, _, lan_ip, lan_port, _, _ = struct.unpack_from(PEERF, r[4])
            peer = ((ip_str(pub_ip), pub_port), (ip_str(lan_ip), lan_port)); break
    if peer is None:
        print("[relay_peer] RESULT join=NO-PEER (server silent)"); return 1
    print(f"[relay_peer] PEER host pub {peer[0][0]}:{peer[0][1]} lan {peer[1][0]}:{peer[1][1]}", flush=True)
    link = Link(sock, server, room); link.slot = HOST_SLOT
    hello = lobby_peer.pack_lobby(lobby_peer.HELLO, nonce, 0, proto)
    sock.settimeout(0.05)
    t0 = time.monotonic(); nxt = 0.0; welcomed = None
    while time.monotonic() - t0 < 15 and not welcomed:
        now = time.monotonic()
        relayed = force or now - t0 > 3
        if now >= nxt:
            if relayed:
                sock.sendto(hdr(DATA, HOST_SLOT, room) + hello, server)
            else:
                for ep in peer:
                    if ep[1]:
                        sock.sendto(hello, ep)
            nxt = now + 0.25
        try:
            data, src = sock.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        r = parse(data)
        body, via = (r[4], "relay") if (r and src == server and r[1] == DATA) else ((data, "direct") if not r else (None, None))
        lp = lobby_peer.unpack_lobby(body) if body else None
        if lp and lp["type"] == lobby_peer.WELCOME and lp["nonce"] == nonce:
            welcomed = via
            if via == "direct":
                link.direct = src
        elif lp and lp["type"] == lobby_peer.REJECT and lp["nonce"] == nonce:
            print(f"[relay_peer] RESULT join=REJECTED host proto {lp['proto']}"); return 1
    if not welcomed:
        print("[relay_peer] RESULT join=TIMEOUT (no WELCOME direct or relayed in 15 s)"); return 1
    print(f"[relay_peer] WELCOME via {welcomed}; streaming {seconds:.0f} s", flush=True)
    stream(link, seconds, proto, sock, server, room, lambda: hdr(KEEPALIVE, 0, room))
    ok = link.c.total() > 0
    print(f"[relay_peer] RESULT join=OK path={'DIRECT' if link.direct else 'RELAY'} tx_player={link.tx} {link.c} "
          f"link={'BOTH-WAYS' if ok else 'ONE-WAY(no rx)'}")
    return 0 if ok else 1


def selftest():
    fails = []
    sizes = {"Hdr": (HDR, 16), "Hello": (HELLO, 24), "Registered": (REGD, 8), "Peer": (PEERF, 20), "Error": (ERRF, 8)}
    for name, (fmt, n) in sizes.items():
        if struct.calcsize(fmt) != n:
            fails.append(f"{name} size {struct.calcsize(fmt)} != {n}")
    h = RELAY_H.read_text(encoding="utf-8")
    pins = {"kMagic": MAGIC, "kVersion": VERSION, "kHostSlot": HOST_SLOT, "kAllSlots": ALL_SLOTS,
            "kMaxMembers": MAX_MEMBERS, "kMaxDatagram": MAX_DGRAM,
            "T_REGISTER": REGISTER, "T_REGISTERED": REGISTERED, "T_JOIN": JOIN, "T_PEER": PEER, "T_DATA": DATA, "T_ERR": ERR,
            "T_KEEPALIVE": KEEPALIVE, "E_UNKNOWN_ROOM": 1, "E_ROOM_FULL": 2, "E_PROTO": 3, "E_VERSION": 4, "E_NOT_MEMBER": 5}
    for name, val in pins.items():
        m = re.search(rf"\b{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", h)
        if not m or int(m.group(1), 0) != val:
            fails.append(f"{name} differs from src/relay_proto.h ({m.group(1) if m else 'missing'} vs {val})")
    for name, n in (("Hdr", 16), ("Hello", 24), ("Registered", 8), ("Peer", 20), ("Error", 8)):
        if not re.search(rf"sizeof\({name}\)\s*==\s*{n}\b", h):
            fails.append(f"sizeof({name}) static_assert differs from {n}")
    ns = (ROOT / "src" / "netstate.h").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"LOBBY_PUNCH\s*=\s*(\d+)", ns)
    if not m or int(m.group(1)) != LOBBY_PUNCH:
        fails.append("LOBBY_PUNCH differs from src/netstate.h (or missing)")
    if hdr(DATA, 3, 0x1122334455)[:4] != b"AC4R":
        fails.append("magic bytes are not 'AC4R'")
    for room in (1, 0xFFFFFFFFFF, 0x0123456789, random.getrandbits(40) or 1):
        c = room_code(room)
        if not c.startswith("Z") or code_room(c) != room:
            fails.append(f"room code round trip {room:#x} -> {c} -> {code_room(c)}")
    if code_room(lobby_code.encode("203.0.113.7", 47801)) is not None:
        fails.append("a direct code was taken for a room code")
    if lobby_peer.selftest() != 0:
        fails.append("lobby_peer selftest")
    if fails:
        print("[relay_peer] selftest: FAIL " + "; ".join(fails)); return 1
    print("[relay_peer] selftest: PASS (layouts, constants vs relay_proto.h/netstate.h, room codes, direct-code negative)")
    return 0


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    ap = argparse.ArgumentParser(description="relay-path stand-in player")
    sub = ap.add_subparsers(dest="cmd")
    h = sub.add_parser("host"); h.add_argument("--server", required=True); h.add_argument("--seconds", type=float, default=60)
    h.add_argument("--force-relay", action="store_true"); h.add_argument("--proto", type=int, default=lobby_peer.NET_PROTO_VER)
    h.add_argument("--port", type=int, default=0)
    j = sub.add_parser("join"); j.add_argument("code"); j.add_argument("--server", required=True)
    j.add_argument("--seconds", type=float, default=20); j.add_argument("--force-relay", action="store_true")
    j.add_argument("--proto", type=int, default=lobby_peer.NET_PROTO_VER)
    a = ap.parse_args(argv)
    if a.cmd == "host":
        return cmd_host(a.server, a.seconds, a.force_relay, a.proto, a.port)
    if a.cmd == "join":
        return cmd_join(a.code, a.server, a.seconds, a.force_relay, a.proto)
    ap.print_help(); return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""relay_check.py - black-box checks of a running ac4coop-relay (docs/design/relay.md).

  py tools\\relay_check.py --server 127.0.0.1:47800 [--slow]

Each check talks raw protocol (tools/relay_peer.py constants, pinned to src/relay_proto.h) and prints PASS/FAIL:
  register        REGISTER -> REGISTERED with a 40-bit room id, echoing the nonce
  register-retry  a second REGISTER (room 0) from the same endpoint returns the same room
  unknown-room    JOIN to a room that does not exist -> ERR unknown room
  proto           JOIN with a different game protocol -> ERR game protocol differs
  version         REGISTER with relay version 2 -> ERR relay version differs
  peer            JOIN -> PEER to the guest (host endpoints) AND PEER to the host (guest endpoints)
  full            7 guests fill the room; the 8th -> ERR room full
  data            guest DATA(slot 0) reaches the host with slot = guest; host DATA(all) reaches every guest
  stranger        DATA/KEEPALIVE from a non-member: nothing forwarded, nothing answered
  amplification   for every request above, reply size <= request size (PEER to the host excepted: a member)
  restart-id      REGISTER with a non-zero unknown id re-creates that id (code survives a server restart)
  flood           2000 datagrams in a burst from one endpoint: the server keeps answering others (rate limit)
  --slow: expiry  a room whose host goes silent is closed after 60 s: JOIN -> ERR unknown room
Exit 0 = all PASS.
"""
from __future__ import annotations

import argparse
import random
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import relay_peer as rp  # noqa: E402

RESULTS: list[tuple[str, bool, str]] = []
AMP: list[tuple[str, int, int]] = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)


class Client:
    def __init__(self, server):
        self.server = server
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(0.5)

    @property
    def ep(self):
        return self.s.getsockname()

    def send(self, data):
        self.s.sendto(data, self.server)
        return len(data)

    def recv(self, timeout=0.5):
        self.s.settimeout(timeout)
        try:
            data, src = self.s.recvfrom(4096)
            return data, src
        except (socket.timeout, ConnectionResetError):
            return None, None

    def hello(self, ptype, room, proto=12, version=rp.VERSION, nonce=None):
        nonce = random.getrandbits(32) if nonce is None else nonce
        body = struct.pack(rp.HELLO, proto, 0x7F000001, self.ep[1], 0, nonce, 0, 0)
        h = struct.pack(rp.HDR, rp.MAGIC, version, ptype, 0, room)
        return self.send(h + body), nonce


def expect(c, want_type, what, req_len=None, timeout=0.5):
    data, src = c.recv(timeout)
    r = rp.parse(data) if data else None
    if r and req_len is not None:
        AMP.append((what, req_len, len(data)))
    if not r or r[1] != want_type:
        return None
    return r


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="127.0.0.1:47800")
    ap.add_argument("--slow", action="store_true")
    a = ap.parse_args(argv)
    server = rp.resolve(a.server)

    host = Client(server)
    n, nonce = host.hello(rp.REGISTER, 0)
    r = expect(host, rp.REGISTERED, "register", n)
    room = r[3] if r else 0
    nonce_ok = bool(r) and struct.unpack_from(rp.REGD, r[4])[0] == nonce
    check("register", bool(r) and 0 < room <= 0xFFFFFFFFFF and nonce_ok, f"room={room:010x} code={rp.room_code(room) if room else '-'}")

    n, _ = host.hello(rp.REGISTER, 0)
    r = expect(host, rp.REGISTERED, "register-retry", n)
    check("register-retry", bool(r) and r[3] == room)

    g = Client(server)
    n, _ = g.hello(rp.JOIN, room ^ 0x5A5A5A)
    r = expect(g, rp.ERR, "unknown-room", n)
    check("unknown-room", bool(r) and struct.unpack_from(rp.ERRF, r[4])[0] == 1)

    n, _ = g.hello(rp.JOIN, room, proto=11)
    r = expect(g, rp.ERR, "proto", n)
    check("proto", bool(r) and struct.unpack_from(rp.ERRF, r[4])[0] == 3)

    v = Client(server)
    n, _ = v.hello(rp.REGISTER, 0, version=2)
    r = expect(v, rp.ERR, "version", n)
    check("version", bool(r) and struct.unpack_from(rp.ERRF, r[4])[0] == 4)

    n, gnonce = g.hello(rp.JOIN, room)
    rg = expect(g, rp.PEER, "peer(guest)", n)
    rh = expect(host, rp.PEER, None)
    ok = False
    if rg and rh:
        hp = struct.unpack_from(rp.PEERF, rg[4]); gp = struct.unpack_from(rp.PEERF, rh[4])
        ok = (hp[1] == host.ep[1] and gp[1] == g.ep[1] and rg[2] == 0 and rh[2] >= 1 and gp[6] == gnonce)
    check("peer", ok, f"guest slot={rh[2] if rh else '-'}")
    gslot = rh[2] if rh else 1

    guests = [g]
    for i in range(6):
        c = Client(server); c.hello(rp.JOIN, room); expect(c, rp.PEER, None); expect(host, rp.PEER, None); guests.append(c)
    extra = Client(server)
    n, _ = extra.hello(rp.JOIN, room)
    r = expect(extra, rp.ERR, "full", n)
    check("full", bool(r) and struct.unpack_from(rp.ERRF, r[4])[0] == 2, f"members={1 + len(guests)}")

    payload = b"AC4C" + bytes(60)
    g.send(struct.pack(rp.HDR, rp.MAGIC, rp.VERSION, rp.DATA, 0, room) + payload)
    r = expect(host, rp.DATA, None)
    ok1 = bool(r) and r[2] == gslot and r[4] == payload
    host.send(struct.pack(rp.HDR, rp.MAGIC, rp.VERSION, rp.DATA, rp.ALL_SLOTS, room) + payload)
    got = sum(1 for c in guests if expect(c, rp.DATA, None))
    check("data", ok1 and got == len(guests), f"guest->host slot={r[2] if r else '-'} host->all reached {got}/{len(guests)}")

    s = Client(server)
    s.send(struct.pack(rp.HDR, rp.MAGIC, rp.VERSION, rp.DATA, 0, room) + payload)
    s.send(struct.pack(rp.HDR, rp.MAGIC, rp.VERSION, rp.KEEPALIVE, 0, room))
    leak_host = expect(host, rp.DATA, None, timeout=0.3)
    answer = s.recv(0.3)[0]
    check("stranger", leak_host is None and answer is None)

    bad = [(w, q, p) for (w, q, p) in AMP if p > q]
    check("amplification", not bad, f"{len(AMP)} replies measured" + (f", larger: {bad}" if bad else ""))

    rs = Client(server)
    fake = random.getrandbits(40) | 1
    n, _ = rs.hello(rp.REGISTER, fake)
    r = expect(rs, rp.REGISTERED, "restart-id", n)
    check("restart-id", bool(r) and r[3] == fake, f"{fake:010x}")

    f = Client(server)
    for _ in range(2000):
        f.send(struct.pack(rp.HDR, rp.MAGIC, rp.VERSION, rp.KEEPALIVE, 0, 1))
    n, _ = host.hello(rp.REGISTER, room)
    r = expect(host, rp.REGISTERED, None)
    check("flood", bool(r), "server still answers a member after a 2000-datagram burst from another endpoint")

    if a.slow:
        print("  ... expiry: waiting 65 s with the host silent", flush=True)
        t_end = time.monotonic() + 65
        while time.monotonic() < t_end:   # keep one guest alive so only the host's silence matters
            g.send(struct.pack(rp.HDR, rp.MAGIC, rp.VERSION, rp.KEEPALIVE, 0, room)); time.sleep(5)
        late = Client(server)
        late.hello(rp.JOIN, room)
        r = expect(late, rp.ERR, None)
        check("expiry", bool(r) and struct.unpack_from(rp.ERRF, r[4])[0] == 1)

    fails = [x for x in RESULTS if not x[1]]
    print(f"[relay_check] {'PASS' if not fails else 'FAIL'} {len(RESULTS) - len(fails)}/{len(RESULTS)}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

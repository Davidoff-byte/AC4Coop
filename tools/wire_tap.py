#!/usr/bin/env python3
"""wire_tap.py - record a real game's co-op stream and replay it to a game later (one-PC sync test rig).

Why: host<->guest sync needs two games, and a second PC is not always there. This tool is the other player:
  record   joins (or hosts for) the running game through the lobby handshake and saves every game datagram it
           receives, with timing, to a .tap file. While recording it sends minimal PlayerStates (valid=0, which the
           game applies nothing from: phase1.cpp) so the game keeps treating it as a live peer.
  replay   hosts (or joins) and sends a .tap file back with the original timing, so the game receives exactly what a
           real peer sent: the other player's PlayerState stream and, from a host recording, guard / ship / kill
           snapshots. Loops with monotonic seq and a fresh entity epoch per pass.
  info     prints what a .tap file contains.

Typical use (same PC, game bound to its LAN address):
  host recording : game Mode=host  ->  py tools\\wire_tap.py record host.tap  join <LAN code> --seconds 120
  guest test     : py tools\\wire_tap.py replay host.tap host --port 47811   ->  game Mode=join, JoinCode=<printed code>
  guest recording: py tools\\wire_tap.py record guest.tap host --port 47811  ->  game Mode=join ...
  host test      : game Mode=host  ->  py tools\\wire_tap.py replay guest.tap join <LAN code>

Wire facts used (src/netstate.h): PlayerState = 576 B, 'AC4C', u32 protoVer, u32 seq at +8, u64 worldId at +12.
Entity datagram = 32 B header: 'AC4E', u32 protoVer, u32 seq(+8), u32 epoch(+12), u32 count(+16), u64 worldId(+20),
u16 chunkIndex(+28), u16 chunkTotal(+30). Nothing else is interpreted; payloads are replayed byte for byte.
Standard library only. Exit 0 = ok, 1 = failed, 2 = usage.
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
import lobby_code  # noqa: E402
import lobby_peer as lp  # noqa: E402

TAP_MAGIC = b"AC4TAP1\n"


# ---------------------------------------------------------------- .tap files
def tap_write(path, records, meta):
    with open(path, "wb") as f:
        f.write(TAP_MAGIC)
        m = repr(meta).encode()
        f.write(struct.pack("<I", len(m))); f.write(m)
        for t, data in records:
            f.write(struct.pack("<dH", t, len(data))); f.write(data)


def tap_read(path):
    b = Path(path).read_bytes()
    if b[:8] != TAP_MAGIC:
        raise ValueError(f"{path}: not a .tap file")
    n = struct.unpack_from("<I", b, 8)[0]
    meta = eval(b[12:12 + n].decode(), {"__builtins__": {}})  # written by tap_write: a plain dict literal
    o = 12 + n; recs = []
    while o + 10 <= len(b):
        t, ln = struct.unpack_from("<dH", b, o); o += 10
        recs.append((t, b[o:o + ln])); o += ln
    return meta, recs


def kind(d):
    m = struct.unpack_from("<I", d)[0] if len(d) >= 4 else 0
    return "player" if m == lp.NET_MAGIC and len(d) == lp.SIZEOF_PLAYERSTATE else "entity" if m == lp.NET_ENTITY_MAGIC and len(d) >= 32 else "other"


def summarize(recs):
    # PlayerState offsets printed by cl from netstate.h (2026-11-12): sceneKey +20, matrix +32 (position = floats at
    # +80), shipValid +523, valid +564.
    s = {"player": 0, "entity": 0, "other": 0, "bytes": 0, "worlds": set(), "ent_records": 0, "epochs": set(),
         "valid": 0, "aboard": 0, "scenes": set(), "pos_first": None, "pos_last": None}
    for _, d in recs:
        k = kind(d); s[k] += 1; s["bytes"] += len(d)
        if k == "player":
            s["worlds"].add(struct.unpack_from("<Q", d, 12)[0])
            if d[564]:
                s["valid"] += 1; s["aboard"] += 1 if d[523] else 0
                s["scenes"].add(struct.unpack_from("<I", d, 20)[0])
                pos = struct.unpack_from("<3f", d, 80)
                s["pos_first"] = s["pos_first"] or pos; s["pos_last"] = pos
        elif k == "entity":
            _, _, _, epoch, count, world, ci, ct = struct.unpack_from("<IIIIIQHH", d)
            s["epochs"].add(epoch); s["worlds"].add(world)
            if ci == 0: s["ent_records"] += count
    s["seconds"] = recs[-1][0] - recs[0][0] if recs else 0.0
    return s


def fmt_summary(s):
    return (f"{s['player']} player + {s['entity']} entity datagrams ({s['other']} other), {s['bytes']} B over {s['seconds']:.1f} s; "
            f"entity records {s['ent_records']} in {len(s['epochs'])} epoch(s); worldIds " + ",".join(f"{w:x}" for w in sorted(s['worlds']))
            + f"; live player packets {s['valid']} ({s['aboard']} aboard a ship), {len(s['scenes'])} scene key(s)"
            + (f", position {tuple(round(v, 1) for v in s['pos_first'])} -> {tuple(round(v, 1) for v in s['pos_last'])}" if s['pos_first'] else ""))


# ---------------------------------------------------------------- link (handshake as host or guest)
def link_join(code, proto):
    dec = lobby_code.decode(code)
    if dec is None:
        print(f"[tap] code {code!r} is invalid"); return None, None
    peer = dec
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(("0.0.0.0", 0))
    nonce = random.getrandbits(32); sock.settimeout(0.25)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 600:
        sock.sendto(lp.pack_lobby(lp.HELLO, nonce, lp.code_check(*peer), proto), peer)
        t_slot = time.monotonic() + (0.25 if time.monotonic() - t0 < 15 else 2.0)
        while time.monotonic() < t_slot:
            try:
                data, src = sock.recvfrom(16384)
            except socket.timeout:
                break
            except ConnectionResetError:
                time.sleep(max(0.0, t_slot - time.monotonic())); break
            m = lp.unpack_lobby(data)
            if m and src == peer and m["nonce"] == nonce:
                if m["type"] == lp.WELCOME:
                    print(f"[tap] joined {peer[0]}:{peer[1]}", flush=True); return sock, peer
                if m["type"] == lp.REJECT:
                    print(f"[tap] REJECTED: host proto {m['proto']}, ours {proto}"); return None, None
    print("[tap] no WELCOME in 10 min"); return None, None


def link_host(port, proto, wait):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(("0.0.0.0", port))
    lan = lp.lan_ip()
    if lan:
        print(f"[tap] hosting. LAN code {lobby_code.encode(lan, port)}  (same-PC code {lobby_code.encode('127.0.0.1', port)})", flush=True)
    sock.settimeout(1.0); t0 = time.monotonic()
    while time.monotonic() - t0 < wait:
        try:
            data, src = sock.recvfrom(16384)
        except (socket.timeout, ConnectionResetError):
            continue
        m = lp.unpack_lobby(data)
        if m and m["type"] == lp.HELLO:
            if m["proto"] != proto:
                sock.sendto(lp.pack_lobby(lp.REJECT, m["nonce"], 0, proto), src); continue
            sock.sendto(lp.pack_lobby(lp.WELCOME, m["nonce"], 0, proto), src)
            print(f"[tap] guest joined {src[0]}:{src[1]}", flush=True); return sock, src
    print(f"[tap] no guest in {wait:.0f} s"); return None, None


def answer_hello(sock, data, src, peer, proto):
    m = lp.unpack_lobby(data)
    if m is None:
        return False
    if m["type"] == lp.HELLO and src == peer:   # a retry that crossed our WELCOME
        sock.sendto(lp.pack_lobby(lp.WELCOME, m["nonce"], 0, proto), src)
    return True


# ---------------------------------------------------------------- record / replay
def cmd_record(a):
    sock, peer = link_join(a.code, a.proto) if a.role == "join" else link_host(a.port, a.proto, a.wait)
    if not sock:
        return 1
    recs = []; sock.settimeout(0.01)
    t0 = time.monotonic(); nxt = 0.0; seq = 0; last_print = t0
    try:
        while time.monotonic() - t0 < a.seconds:
            now = time.monotonic()
            if now >= nxt:
                sock.sendto(lp.player_state(seq, a.proto), peer); seq += 1; nxt = now + 0.05
            try:
                data, src = sock.recvfrom(16384)
            except (socket.timeout, ConnectionResetError):
                continue
            if src != peer or answer_hello(sock, data, src, peer, a.proto):
                continue
            recs.append((time.monotonic() - t0, data))
            if now - last_print > 10:
                last_print = now; print(f"[tap] {now - t0:5.0f} s  {len(recs)} datagrams", flush=True)
    except KeyboardInterrupt:
        pass
    tap_write(a.file, recs, {"role": a.role, "proto": a.proto, "recorded": time.strftime("%Y-%m-%d %H:%M:%S")})
    s = summarize(recs)
    print(f"[tap] RESULT record={'OK' if s['player'] else 'EMPTY'} file={a.file} " + fmt_summary(s))
    return 0 if s["player"] else 1


def cmd_replay(a):
    meta, recs = tap_read(a.file)
    if not recs:
        print("[tap] empty file"); return 1
    print(f"[tap] {a.file}: " + fmt_summary(summarize(recs)), flush=True)
    sock, peer = link_join(a.code, a.proto) if a.role == "join" else link_host(a.port, a.proto, a.wait)
    if not sock:
        return 1
    sock.settimeout(0.0)
    c = lp.Counter(); sent = 0; t_begin = time.monotonic()
    span_p = max((struct.unpack_from("<I", d, 8)[0] for _, d in recs if kind(d) == "player"), default=0) + 1
    span_e = max((struct.unpack_from("<I", d, 8)[0] for _, d in recs if kind(d) == "entity"), default=0) + 1
    first = recs[0][0]; passes = 0
    try:
        while passes < a.loops and time.monotonic() - t_begin < a.seconds:
            t_pass = time.monotonic()
            for t, d in recs:
                while True:
                    try:
                        data, src = sock.recvfrom(16384)
                        if src == peer and not answer_hello(sock, data, src, peer, a.proto):
                            c.add(data)
                    except (BlockingIOError, ConnectionResetError):
                        pass
                    dt = (t - first) / a.speed - (time.monotonic() - t_pass)
                    if dt <= 0:
                        break
                    time.sleep(min(dt, 0.005))
                k = kind(d)
                if passes and k == "player":      # later passes: keep seq rising
                    d = d[:8] + struct.pack("<I", (struct.unpack_from("<I", d, 8)[0] + passes * span_p) & 0xFFFFFFFF) + d[12:]
                elif passes and k == "entity":    # ... and give the pass its own delta epoch (receivers drop their base on a new epoch)
                    seq_, ep = struct.unpack_from("<II", d, 8)
                    d = d[:8] + struct.pack("<II", (seq_ + passes * span_e) & 0xFFFFFFFF, (ep + passes * 100003) & 0xFFFFFFFF) + d[16:]
                sock.sendto(d, peer); sent += 1
                if time.monotonic() - t_begin >= a.seconds:
                    break
            passes += 1
            print(f"[tap] pass {passes} done: sent {sent}, received {c}", flush=True)
    except KeyboardInterrupt:
        pass
    print(f"[tap] RESULT replay=OK passes={passes} sent={sent} {c}")
    return 0


def cmd_info(a):
    meta, recs = tap_read(a.file)
    print(f"[tap] {a.file}: meta={meta}")
    print("[tap] " + fmt_summary(summarize(recs)))
    return 0


def selftest():
    import os, tempfile
    recs = [(0.0, lp.player_state(1)), (0.05, struct.pack("<IIIIIQHH", lp.NET_ENTITY_MAGIC, lp.NET_PROTO_VER, 7, 3, 2, 0x1234, 0, 1) + b"x" * 40),
            (0.10, lp.player_state(2))]
    p = os.path.join(tempfile.gettempdir(), "wire_tap_selftest.tap")
    tap_write(p, recs, {"role": "join", "proto": lp.NET_PROTO_VER})
    meta, back = tap_read(p); os.remove(p)
    s = summarize(back)
    ok = back == recs and meta["role"] == "join" and s["player"] == 2 and s["entity"] == 1 and s["ent_records"] == 2 and 0x1234 in s["worlds"]
    print(f"[tap] selftest: {'PASS' if ok else 'FAIL'} (file round trip, classification, summary)")
    return 0 if ok else 1


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    ap = argparse.ArgumentParser(description="record / replay a co-op stream")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("record", "replay"):
        p = sub.add_parser(name); p.add_argument("file"); p.add_argument("role", choices=("join", "host")); p.add_argument("code", nargs="?")
        p.add_argument("--port", type=int, default=47811); p.add_argument("--seconds", type=float, default=120 if name == "record" else 3600)
        p.add_argument("--proto", type=int, default=lp.NET_PROTO_VER); p.add_argument("--wait", type=float, default=900)
        if name == "replay":
            p.add_argument("--loops", type=int, default=1000); p.add_argument("--speed", type=float, default=1.0)
    i = sub.add_parser("info"); i.add_argument("file")
    a = ap.parse_args(argv)
    if a.cmd in ("record", "replay") and a.role == "join" and not a.code:
        print("join needs a code"); return 2
    return {"record": cmd_record, "replay": cmd_replay, "info": cmd_info}.get(a.cmd, lambda _: (ap.print_help(), 2)[1])(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

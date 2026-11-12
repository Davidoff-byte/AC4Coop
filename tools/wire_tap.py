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


# ---------------------------------------------------------------- entity records (src/net.cpp EncodeEntityPacket)
# Record = u32 key, u8 kind (1 ship, 2 guard, 3 kill), u8 idKind (0 = real objId, 1 = made from kind+key), u16 mask,
# u64 wireId, then the fields the mask names: POS 1 (3f), QUAT 2 (4f), ACTION 4 (u32), HP 0x10 (u16), ARCH 0x20 (u16),
# DEAD 0x40 (u8), STAGE 0x80 (u8). The record region is cut into 1368 B chunks by byte offset, so a record can
# straddle two datagrams: chunks of one seq are joined before decoding.
EF = ((1, "pos", "<3f"), (2, "quat", "<4f"), (4, "action", "<I"), (0x10, "hp", "<H"), (0x20, "arch", "<H"), (0x40, "dead", "<B"), (0x80, "stage", "<B"))


def entity_snapshots(recs):
    """Yield (t, seq, epoch, [record dict]) for every complete snapshot, in order."""
    parts = {}
    for t, d in recs:
        if kind(d) != "entity": continue
        _, _, seq, epoch, count, _, ci, ct = struct.unpack_from("<IIIIIQHH", d)
        p = parts.setdefault(seq, {"t": t, "epoch": epoch, "count": count, "ct": ct, "c": {}})
        p["c"][ci] = d[32:]
        if len(p["c"]) < ct: continue
        del parts[seq]
        b = b"".join(p["c"][i] for i in range(ct)); o = 0; out = []
        for _ in range(p["count"]):
            if o + 16 > len(b): break
            key, k, idk, mask, wid = struct.unpack_from("<IBBHQ", b, o); o += 16
            r = {"key": key, "kind": k, "idKind": idk, "id": wid, "mask": mask}
            for bit, name, fmt in EF:
                if mask & bit:
                    v = struct.unpack_from(fmt, b, o); o += struct.calcsize(fmt)
                    r[name] = v if len(v) > 1 else v[0]
            out.append(r)
        yield p["t"], seq, p["epoch"], out


def entity_table(recs):
    """One row per (kind, idKind, id): what the host said about it over the whole recording."""
    rows = {}
    for t, _, _, snap in entity_snapshots(recs):
        for r in snap:
            e = rows.setdefault((r["kind"], r["idKind"], r["id"]), {"n": 0, "keys": set(), "t0": t, "first": None, "last": None, "arch": None,
                                                                    "dead": 0, "hp": None, "stages": set(), "actions": set()})
            e["n"] += 1; e["keys"].add(r["key"]); e["t1"] = t
            if "pos" in r: e["first"] = e["first"] or r["pos"]; e["last"] = r["pos"]
            if "arch" in r: e["arch"] = r["arch"]
            if "hp" in r: e["hp"] = r["hp"]
            if "stage" in r: e["stages"].add(r["stage"])
            if "action" in r: e["actions"].add(r["action"])
            e["dead"] |= r.get("dead", 0)
    return rows


def fmt_entities(rows):
    names = {1: "ship", 2: "guard", 3: "kill"}; out = []
    for (k, idk, wid), e in sorted(rows.items(), key=lambda kv: (kv[0][0], kv[1]["t0"])):
        p = lambda v: "(" + ",".join(f"{x:.1f}" for x in v) + ")" if v else "-"
        out.append(f"  {names.get(k, k):5} id={'obj' if idk == 0 else 'syn'}:{wid:016x} keys={sorted(e['keys'])[:4]} arch={e['arch']} recs={e['n']:4} "
                   f"t={e['t0']:.0f}-{e['t1']:.0f}s pos={p(e['first'])}->{p(e['last'])} hp={e['hp']} dead={e['dead']} stages={sorted(e['stages'])} actions={len(e['actions'])}")
    return out


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
    rec_worlds = sorted(w for w in summarize(recs)["worlds"] if w)
    live_world = [0]; warned = [False]

    def note_world(data):   # the live game's worldId (PlayerState +12): the entity decoder drops other worlds
        if kind(data) != "player" or not data[564]:
            return
        w = struct.unpack_from("<Q", data, 12)[0]
        if w and w != live_world[0]:
            live_world[0] = w
            same = w in rec_worlds
            print(f"[tap] live game worldId {w:x}; recording has {','.join(f'{x:x}' for x in rec_worlds) or 'none'} -> "
                  + ("MATCH" if same else ("MISMATCH, rewriting (--adopt-world)" if a.adopt_world else
                     "MISMATCH: the game will drop the replayed entities and hide the puppet (use --adopt-world to rewrite)")), flush=True)
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
                            c.add(data); note_world(data)
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
                if a.adopt_world and live_world[0]:   # worldId: PlayerState +12, entity header +20
                    if k == "player" and struct.unpack_from("<Q", d, 12)[0]:
                        d = d[:12] + struct.pack("<Q", live_world[0]) + d[20:]
                    elif k == "entity":
                        d = d[:20] + struct.pack("<Q", live_world[0]) + d[28:]
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
    if a.entities:
        rows = entity_table(recs)
        by = {}
        for (k, idk, _), _e in rows.items(): by[(k, idk)] = by.get((k, idk), 0) + 1
        print(f"[tap] {len(rows)} distinct entities: " + ", ".join(f"kind {k} {'objId' if i == 0 else 'synthetic'} x{n}" for (k, i), n in sorted(by.items())))
        for line in fmt_entities(rows): print(line)
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
    # two records, cut mid-record across two chunks: a guard with pos+arch+hp and a ship with pos only
    body = (struct.pack("<IBBHQ3fHH", 5, 2, 0, 0x31, 0xABCDEF0123, 1.0, 2.0, 3.0, 77, 412)      # field order: pos, hp, arch
            + struct.pack("<IBBHQ3f", 9, 1, 1, 0x01, (1 << 32) | 9, 4.0, 5.0, 6.0))
    hdr = lambda ci: struct.pack("<IIIIIQHH", lp.NET_ENTITY_MAGIC, lp.NET_PROTO_VER, 50, 1, 2, 0x1234, ci, 2)
    rows = entity_table([(0.0, hdr(0) + body[:20]), (0.01, hdr(1) + body[20:])])
    g = rows.get((2, 0, 0xABCDEF0123)); sh = rows.get((1, 1, (1 << 32) | 9))
    ok = ok and len(rows) == 2 and g and sh and g["hp"] == 77 and g["arch"] == 412 and g["first"] == (1.0, 2.0, 3.0) and sh["first"] == (4.0, 5.0, 6.0)
    print(f"[tap] selftest: {'PASS' if ok else 'FAIL'} (file round trip, classification, summary, entity decode across chunks)")
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
            p.add_argument("--adopt-world", action="store_true", help="rewrite the recording's worldId to the live game's")
    i = sub.add_parser("info"); i.add_argument("file"); i.add_argument("--entities", action="store_true", help="list every entity the stream names")
    a = ap.parse_args(argv)
    if a.cmd in ("record", "replay") and a.role == "join" and not a.code:
        print("join needs a code"); return 2
    return {"record": cmd_record, "replay": cmd_replay, "info": cmd_info}.get(a.cmd, lambda _: (ap.print_help(), 2)[1])(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

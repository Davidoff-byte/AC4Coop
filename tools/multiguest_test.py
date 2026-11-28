#!/usr/bin/env python3
"""multiguest_test.py - offline wire oracle for the multi-guest slices (design-multi-guest-2026-11-27.md S0-S5).

Builds tools/netharness/link_main.cpp (the REAL src/net.cpp link in a console exe, no game) against a source tree,
runs it as host or join next to a generated AC4Coop.ini, and drives it with Python peers on 127.0.0.1.

Cases
  r1host   MaxPeers=1 host + 1 Python guest: the guest records what the host sends. The first PlayerState datagram
           and the (magic, length) order of the first 60 game datagrams are compared with the golden file captured
           from the BASE build (feat/integration-1121 d9b463a). Expected diff: protoVer (+4) only, when the tree's
           NET_PROTO_VER differs from the golden's.
  r1join   MaxPeers=1 join harness + Python host: the same comparison for what a guest sends.
  multi    MaxPeers=3 host + guests A, B, C from 3 ports of 127.0.0.1 (same IP: ip+port attribution).
           S3: A -> slot 1, B -> slot 2, C refused (full=1); A and B both receive host PlayerStates and entity chunks.
           S5 (tree proto >= 18): A receives B's relayed PlayerStates (originSlot 2) and never its own; B receives A's
           (originSlot 1); roster 'AC4S' reaches both with presentMask 0b111 and yourSlot 1 / 2.
  ctl1     control for multi: the same 3 guests against MaxPeers=1. Old rule: one guest slot; a HELLO from the same
           IP on another port RE-LOCKS (sameMachine), no relay, no roster.

  py tools\\multiguest_test.py [--tree PATH] [--cases r1host,r1join,multi,ctl1] [--capture-golden]

--capture-golden writes tools/testdata/multiguest_golden.json from --tree (run it on the base tree once).
Exit 0 = every selected case PASS. Nothing here touches the game folder or the ASI.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lobby_code  # noqa: E402

GOLDEN = ROOT / "tools" / "testdata" / "multiguest_golden.json"
HARNESS = ROOT / "tools" / "netharness" / "link_main.cpp"
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat")
SOURCES = ["net.cpp", "peer_clock.cpp", "paths.cpp", "lobby_code.cpp", "upnp.cpp", "stun.cpp"]
LIBS = ["ws2_32.lib", "winhttp.lib", "iphlpapi.lib", "user32.lib", "bcrypt.lib"]

NET_MAGIC, NET_ENTITY_MAGIC, NET_LOBBY_MAGIC, NET_INTENT_MAGIC = 0x43344341, 0x45344341, 0x4C344341, 0x49344341
NET_ROSTER_MAGIC = 0x53344341  # 'AC4S' (proto 18)
HELLO, WELCOME, REJECT = 1, 2, 3
PS_SIZE, ORIGIN_OFF = 576, 569


def tree_proto(tree: Path) -> int:
    m = re.search(r"NET_PROTO_VER\s*=\s*(\d+)", (tree / "src" / "netstate.h").read_text(encoding="utf-8", errors="replace"))
    return int(m.group(1))


def build(tree: Path, tag: str) -> Path:
    out = ROOT / "bin" / "netharness" / tag
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "link_main.exe"
    srcs = " ".join(f'"{tree / "src" / s}"' for s in SOURCES)
    cl = (f'cl /nologo /EHsc /O2 /MT /W3 /DWIN32 /DNDEBUG /I"{tree / "src"}" "{HARNESS}" {srcs} '
          f'/Fo"{out}\\\\" /Fe"{exe}" /link ' + " ".join(LIBS))
    r = subprocess.run(f'call "{VCVARS}" >nul && {cl}', shell=True, cwd=out, capture_output=True, text=True)
    if r.returncode != 0 or not exe.exists():
        print(r.stdout[-3000:], r.stderr[-1500:])
        raise SystemExit(f"[mg] build FAIL for {tree}")
    return exe


def write_ini(d: Path, net: dict, coop: dict | None = None):
    base = {"Enabled": 1, "Upnp": 0, "StunServer": "", "RelayServer": "", "BindIP": "any"}
    base.update(net)
    c = {"NetStrictPeer": 1, "IntentLane": 0, "PeerClock": 0}
    c.update(coop or {})
    txt = "[Net]\n" + "".join(f"{k}={v}\n" for k, v in base.items()) + "[Coop]\n" + "".join(f"{k}={v}\n" for k, v in c.items())
    (d / "AC4Coop.ini").write_text(txt, encoding="ascii")
    for f in d.glob("net_log*.txt"):
        f.unlink()


def net_log(d: Path) -> str:
    return "".join(f.read_text(encoding="latin-1") for f in sorted(d.glob("net_log*.txt")))


def player_bytes(seq: int, proto: int) -> bytes:
    return struct.pack("<III", NET_MAGIC, proto, seq) + bytes(PS_SIZE - 12)


class Guest(threading.Thread):
    """Python guest: HELLO until WELCOME, then a PlayerState every 50 ms; records every datagram from the host."""

    def __init__(self, name: str, host_port: int, proto: int, seq_base: int, seconds: float):
        super().__init__(daemon=True)
        self.name, self.host, self.proto, self.seq_base, self.seconds = name, ("127.0.0.1", host_port), proto, seq_base, seconds
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.02)
        self.port = self.sock.getsockname()[1]
        self.nonce = (0x5EED0000 | (self.port & 0xFFFF)) & 0xFFFFFFFF
        self.welcomed = False
        self.recs: list[bytes] = []

    def run(self):
        t0 = time.monotonic(); nxt = 0.0; seq = 0
        while time.monotonic() - t0 < self.seconds:
            now = time.monotonic()
            if now >= nxt:
                nxt = now + (0.05 if self.welcomed else 0.25)
                if self.welcomed:
                    seq += 1
                    self.sock.sendto(player_bytes(self.seq_base + seq, self.proto), self.host)
                else:
                    self.sock.sendto(struct.pack("<IIIII", NET_LOBBY_MAGIC, self.proto, HELLO, self.nonce, 0), self.host)
            try:
                d, src = self.sock.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue
            if len(d) == 20 and struct.unpack_from("<I", d)[0] == NET_LOBBY_MAGIC:
                _, _, t, n, _ = struct.unpack("<IIIII", d)
                if t == WELCOME and n == self.nonce:
                    self.welcomed = True
                continue
            self.recs.append(d)
        self.sock.close()

    def players(self):
        return [d for d in self.recs if len(d) == PS_SIZE and struct.unpack_from("<I", d)[0] == NET_MAGIC]

    def entities(self):
        return [d for d in self.recs if len(d) >= 36 and struct.unpack_from("<I", d)[0] == NET_ENTITY_MAGIC]

    def rosters(self):
        return [struct.unpack("<IIIBBH", d) for d in self.recs if len(d) == 16 and struct.unpack_from("<I", d)[0] == NET_ROSTER_MAGIC]


def game_seq(recs, n=60):
    """(magic, length) of the first n game datagrams, entity hostMs/seq are not part of it."""
    out = []
    for d in recs:
        m = struct.unpack_from("<I", d)[0]
        if m in (NET_MAGIC, NET_ENTITY_MAGIC):
            out.append([f"{m:08x}", len(d)])
        if len(out) == n:
            break
    return out


def norm_player(d: bytes, golden_proto: int) -> bytes:
    """seq (+8) zeroed, protoVer (+4) set to the golden's: the remaining bytes must be identical."""
    b = bytearray(d); b[4:8] = struct.pack("<I", golden_proto); b[8:12] = bytes(4)
    return bytes(b)


def norm_entity(d: bytes, golden_proto: int) -> bytes:
    b = bytearray(d); b[4:8] = struct.pack("<I", golden_proto); b[8:12] = bytes(4); b[32:36] = bytes(4)
    return bytes(b)


def run_harness(exe: Path, seconds: float):
    return subprocess.Popen([str(exe), str(int(seconds))], cwd=exe.parent, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def case_r1host(exe: Path, proto: int, port: int):
    write_ini(exe.parent, {"Mode": "host", "LocalPort": port, "PeerPort": port, "MaxPeers": 1})
    h = run_harness(exe, 6); time.sleep(0.8)
    g = Guest("A", port, proto, 0xA0000000, 4.5); g.start(); g.join()
    out = h.communicate(timeout=20)[0]
    ps, es = g.players(), g.entities()
    return {"welcomed": g.welcomed, "player": ps[0].hex() if ps else "", "entity": [e.hex() for e in es[:4]],
            "sequence": game_seq(g.recs), "nPlayer": len(ps), "nEntity": len(es),
            "originNonZero": sum(1 for d in ps if d[ORIGIN_OFF] != 0), "rosters": len(g.rosters()),
            "harness": out.strip().splitlines()[-1:] if out else [], "log": net_log(exe.parent)}


def case_r1join(exe: Path, proto: int, port: int):
    """Python host on `port`; harness joins with the same-PC code."""
    hs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); hs.bind(("127.0.0.1", port)); hs.settimeout(0.05)
    write_ini(exe.parent, {"Mode": "join", "LocalPort": port + 7, "PeerPort": port + 7, "MaxPeers": 1,
                           "JoinCode": lobby_code.encode("127.0.0.1", port)})
    h = run_harness(exe, 5)
    recs, guest, t0 = [], None, time.monotonic()
    while time.monotonic() - t0 < 5.5:
        try:
            d, src = hs.recvfrom(65536)
        except (socket.timeout, ConnectionResetError):
            continue
        if len(d) == 20 and struct.unpack_from("<I", d)[0] == NET_LOBBY_MAGIC:
            _, p, t, n, _ = struct.unpack("<IIIII", d)
            if t == HELLO:
                hs.sendto(struct.pack("<IIIII", NET_LOBBY_MAGIC, proto, WELCOME, n, 0), src); guest = src
            continue
        if src == guest:
            recs.append(d)
    hs.close()
    out = h.communicate(timeout=20)[0]
    ps = [d for d in recs if len(d) == PS_SIZE and struct.unpack_from("<I", d)[0] == NET_MAGIC]
    return {"welcomed": guest is not None, "player": ps[0].hex() if ps else "", "sequence": game_seq(recs), "nPlayer": len(ps),
            "harness": out.strip().splitlines()[-1:] if out else [], "log": net_log(exe.parent)}


def case_multi(exe: Path, proto: int, port: int, max_peers: int):
    write_ini(exe.parent, {"Mode": "host", "LocalPort": port, "PeerPort": port, "MaxPeers": max_peers})
    h = run_harness(exe, 8); time.sleep(0.8)
    a = Guest("A", port, proto, 0xA0000000, 6.0); a.start(); time.sleep(0.6)
    b = Guest("B", port, proto, 0xB0000000, 5.4); b.start(); time.sleep(0.6)
    c = Guest("C", port, proto, 0xC0000000, 4.8); c.start()
    for g in (a, b, c):
        g.join()
    out = h.communicate(timeout=20)[0]
    res = {"harness": out.strip().splitlines()[-1:] if out else [], "log": net_log(exe.parent)}
    for g in (a, b, c):
        ps = g.players()
        own = sum(1 for d in ps if struct.unpack_from("<I", d, 8)[0] >> 28 == g.seq_base >> 28)
        by_origin = {}
        for d in ps:
            hi = struct.unpack_from("<I", d, 8)[0] >> 28
            key = f"o{d[ORIGIN_OFF]}:{'host' if hi == 0 else chr(ord('A') + hi - 0xA)}"
            by_origin[key] = by_origin.get(key, 0) + 1
        ro = g.rosters()
        res[g.name] = {"port": g.port, "welcomed": g.welcomed, "nPlayer": len(ps), "nEntity": len(g.entities()), "own": own,
                       "byOrigin": by_origin, "rosters": len(ro),
                       "lastRoster": {"epoch": ro[-1][2], "you": ro[-1][3], "mask": ro[-1][4]} if ro else None}
    return res


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=str(ROOT))
    ap.add_argument("--cases", default="s0,slots,r1host,r1join,multi,ctl1")
    ap.add_argument("--capture-golden", action="store_true")
    ap.add_argument("--port", type=int, default=47930)
    a = ap.parse_args(argv)
    tree = Path(a.tree).resolve()
    proto = tree_proto(tree)
    exe = build(tree, "golden" if a.capture_golden else "tree")
    print(f"[mg] tree={tree} proto={proto} harness={exe}")
    if a.capture_golden:
        r1, r2 = case_r1host(exe, proto, a.port), case_r1join(exe, proto, a.port + 1)
        if not (r1["player"] and r2["player"]):
            print("[mg] capture FAIL: no player datagram", r1["harness"], r2["harness"]); return 1
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_text(json.dumps({"tree": str(tree), "proto": proto,
                                      "host": {k: r1[k] for k in ("player", "entity", "sequence")},
                                      "join": {k: r2[k] for k in ("player", "sequence")}}, indent=1), encoding="ascii")
        print(f"[mg] golden written: {GOLDEN} (host player {len(r1['player']) // 2} B, {len(r1['sequence'])} seq entries; "
              f"join player {len(r2['player']) // 2} B)")
        return 0
    gold = json.loads(GOLDEN.read_text())
    gp = gold["proto"]
    fails, cases = [], a.cases.split(",")
    if "s0" in cases:   # S0: [Net] MaxPeers logged; absent -> 1, 0 -> 1, 9 -> 4 (clamped), 3 -> 3
        got = {}
        for v, want in ((None, "[Net] MaxPeers=1\n"), (0, "[Net] MaxPeers=1 (ini 0 clamped to 1..4)"),
                        (9, "[Net] MaxPeers=4 (ini 9 clamped to 1..4)"), (3, "[Net] MaxPeers=3\n")):
            net = {"Mode": "host", "LocalPort": a.port + 9, "PeerPort": a.port + 9}
            if v is not None:
                net["MaxPeers"] = v
            write_ini(exe.parent, net)
            run_harness(exe, 1).communicate(timeout=20)
            got[str(v)] = want in net_log(exe.parent)
        ok = all(got.values())
        print(f"[mg] s0 MaxPeers log/clamp={'PASS' if ok else 'FAIL'} {got}")
        if not ok:
            fails.append("s0")
    if "slots" in cases:   # S1: 4 copies (one shared dir + ini) -> instance slot 1,2,3,4 once each, one net_log per slot
        write_ini(exe.parent, {"Mode": "join", "LocalPort": a.port + 8, "PeerPort": a.port + 8})
        procs = []
        for _ in range(4):
            procs.append(run_harness(exe, 3)); time.sleep(0.4)
        outs = [p.communicate(timeout=30)[0] for p in procs]
        slots = [int(m.group(1)) for o in outs for m in [re.search(r"\[Net\] instance slot=(\d+)", o)] if m]
        files = {k: (exe.parent / ("net_log.txt" if k == 1 else f"net_log_slot{k}.txt")) for k in (1, 2, 3, 4)}
        per_file = {k: (f.exists() and f.read_text(encoding="latin-1").count(f"[Net] instance slot={k} ") == 1) for k, f in files.items()}
        ok = sorted(slots) == [1, 2, 3, 4] and all(per_file.values())
        print(f"[mg] slots S1 instance slots={'PASS' if ok else 'FAIL'} stdout slots={slots} per-slot net_log={per_file}")
        if not ok:
            fails.append("slots")
    if "r1host" in cases:
        r = case_r1host(exe, proto, a.port)
        okP = bool(r["player"]) and norm_player(bytes.fromhex(r["player"]), gp) == norm_player(bytes.fromhex(gold["host"]["player"]), gp)
        diff = [i for i, (x, y) in enumerate(zip(bytes.fromhex(r["player"]), bytes.fromhex(gold["host"]["player"]))) if x != y and not 8 <= i < 12] if r["player"] else []
        okE = len(r["entity"]) >= 1 and all(norm_entity(bytes.fromhex(x), gp) == norm_entity(bytes.fromhex(y), gp)
                                            for x, y in zip(r["entity"], gold["host"]["entity"]))
        okS = r["sequence"] == gold["host"]["sequence"]
        okX = r["originNonZero"] == 0 and r["rosters"] == 0
        print(f"[mg] r1host golden player={'PASS' if okP else 'FAIL'} (byte diffs outside seq: {diff} = protoVer {gp}->{proto}) "
              f"entity={'PASS' if okE else 'FAIL'} ({len(r['entity'])} datagrams) order={'PASS' if okS else 'FAIL'} "
              f"({len(r['sequence'])} datagrams) originSlot0/noRoster={'PASS' if okX else 'FAIL'} | {r['harness']}")
        if not okS:
            print("  got ", r["sequence"][:20]); print("  gold", gold["host"]["sequence"][:20])
        if not (okP and okE and okS and okX and r["welcomed"]):
            fails.append("r1host")
    if "r1join" in cases:
        r = case_r1join(exe, proto, a.port + 1)
        okP = bool(r["player"]) and norm_player(bytes.fromhex(r["player"]), gp) == norm_player(bytes.fromhex(gold["join"]["player"]), gp)
        okS = r["sequence"] == gold["join"]["sequence"]
        print(f"[mg] r1join golden player={'PASS' if okP else 'FAIL'} order={'PASS' if okS else 'FAIL'} ({len(r['sequence'])} datagrams) | {r['harness']}")
        if not okS:
            print("  got ", r["sequence"][:20]); print("  gold", gold["join"]["sequence"][:20])
        if not (okP and okS and r["welcomed"]):
            fails.append("r1join")
    if "multi" in cases:
        r = case_multi(exe, proto, a.port + 2, 3)
        log = r["log"]
        A, B, C = r["A"], r["B"], r["C"]
        s3 = (A["welcomed"] and B["welcomed"] and not C["welcomed"] and "slot=1" in log and "slot=2" in log and "full=1" in log
              and A["byOrigin"].get("o0:host", 0) > 20 and B["byOrigin"].get("o0:host", 0) > 20
              and A["nEntity"] > 20 and B["nEntity"] > 20 and C["nPlayer"] == 0)
        ok = s3
        line = f"[mg] multi S3 admit/fan-out={'PASS' if s3 else 'FAIL'}"
        if proto >= 18:
            s5 = (A["byOrigin"].get("o2:B", 0) > 20 and B["byOrigin"].get("o1:A", 0) > 20
                  and A["own"] == 0 and B["own"] == 0 and C["byOrigin"] == {}
                  and A["lastRoster"] and A["lastRoster"]["you"] == 1 and A["lastRoster"]["mask"] == 0b111
                  and B["lastRoster"] and B["lastRoster"]["you"] == 2 and B["lastRoster"]["mask"] == 0b111)
            ok = ok and s5
            line += f" S5 relay/roster={'PASS' if s5 else 'FAIL'}"
        print(line)
        for n in "ABC":
            print(f"  {n}: {r[n]}")
        print(f"  harness: {r['harness']}")
        for ln in log.splitlines():
            if "[Lobby] guest joined" in ln or "ignored" in ln or "[MG]" in ln:
                print("  log:", ln)
        if not ok:
            fails.append("multi")
    if "ctl1" in cases:
        r = case_multi(exe, proto, a.port + 3, 1)
        log = r["log"]
        A, B, C = r["A"], r["B"], r["C"]
        relocks = log.count(" re-lock")
        relayed = sum(v for g in (A, B, C) for k, v in g["byOrigin"].items() if not k.endswith("host"))
        ok = relocks >= 1 and relayed == 0 and A["rosters"] == B["rosters"] == C["rosters"] == 0 and "full=1" not in log
        print(f"[mg] ctl1 MaxPeers=1 control={'PASS' if ok else 'FAIL'} (re-lock lines={relocks}, relayed={relayed}, "
              f"rosters={A['rosters'] + B['rosters'] + C['rosters']})")
        for n in "ABC":
            print(f"  {n}: {r[n]}")
        if not ok:
            fails.append("ctl1")
    print(f"[mg] RESULT {'PASS' if not fails else 'FAIL ' + ','.join(fails)}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

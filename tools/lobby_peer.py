#!/usr/bin/env python3
"""lobby_peer.py - a Python stand-in for the other player in lobby mode (docs/design/lobby-code.md, slice 3).

It speaks the lobby handshake (LobbyPacket, src/netstate.h) and then streams minimal PlayerState datagrams
(576 B, magic + protoVer + seq, valid=0 so the game applies nothing: phase1.cpp:1641/9874) to prove the
link both ways. It does NOT model the full player or entity wire: tools/peer9.py does that (and is still
at proto 9). Standard library only.

  py tools\\lobby_peer.py join XXXX-XXXX-XXXX [--seconds 20] [--proto N]
      Act as the guest: HELLO every 250 ms to the address in the code until WELCOME/REJECT or 15 s,
      then stream for --seconds. Exit 0 only if WELCOMEd AND game datagrams came back from the host.
  py tools\\lobby_peer.py host [--port 47801] [--seconds 60] [--proto N]
      Act as the host: print the LAN and same-PC codes, wait for a HELLO, answer WELCOME (REJECT on a
      foreign protoVer), then stream. Exit 0 only if a guest joined AND game datagrams arrived from it.
  py tools\\lobby_peer.py --selftest
      Pack/unpack round trip, and the constants checked against src/netstate.h (drift = FAIL).

--proto N sends a different protocol version: the negative control (the game must refuse / be refused).
Exit codes: 0 pass, 1 fail, 2 usage.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lobby_code  # noqa: E402  (same directory; the code format twin)

ROOT = Path(__file__).resolve().parents[1]
NETSTATE = ROOT / "src" / "netstate.h"

NET_MAGIC = 0x43344341         # 'AC4C'  netstate.h
NET_ENTITY_MAGIC = 0x45344341  # 'AC4E'  netstate.h
NET_LOBBY_MAGIC = 0x4C344341   # 'AC4L'  netstate.h
NET_INTENT_MAGIC = 0x49344341  # 'AC4I'  netstate.h, proto 15 intention lane
NET_PROTO_VER = 15   # 15: intention lane datagram 'AC4I'; PlayerState, entity header (36 B) and LobbyPacket unchanged
SIZEOF_PLAYERSTATE = 576       # netstate.h static_assert

# ---- proto 15 intention lane (src/netstate.h IntentWireHeader, src/net.cpp IntentPut*/IntentParse) ----
# header: magic, protoVer, senderInc, peerInc, hostMs, ackCum, u8 ackRanges, u8 msgCount, u16 reserved (28 B)
# range : u32 firstMsgId, u16 count (6 B)
# msg   : u32 msgId, u32 slot, u64 wireId, u8 channel, u8 flags, u8 len, u32 chanSeq, u32 hostMs (27 B) + payload
INTENT_HDR_FMT, INTENT_RANGE_FMT, INTENT_MSG_FMT = "<IIIIIIBBH", "<IH", "<IIQBBBII"
INTENT_PAYLOAD_MAX = 32
INTENT_CHANNELS = {1: "ACTION", 2: "LOCO", 3: "WEAPON", 4: "LOOKAT"}   # IntentChannel; 1..15 legal on the wire
IF_BASE = 1
# The same 64 bytes as kGolden in src/net.cpp SelfTestIntent (proto 15): senderInc 0x11223344, peerInc 0x55667788,
# hostMs 0x01020304, ackCum 7, one run {9, 2}, one message {msgId 5, slot 3, wireId 0x0102030405060708, ACTION,
# BASE, "xyz", chanSeq 0x0A0B0C0D, hostMs 0x0E0F1011}. selftest() checks both sides against it.
INTENT_GOLDEN_HEX = ("414334490f00000044332211887766550403020107000000010100000900000002000500000003000000"
                     "08070605040302010101030d0c0b0a11100f0e78797a")


def pack_intent(sender_inc: int, peer_inc: int, host_ms: int, ack_cum: int, ranges=(), msgs=(), proto: int = NET_PROTO_VER) -> bytes:
    """msgs: dicts with msgId, slot, wireId, channel, flags, seq, hostMs, payload (bytes)."""
    out = struct.pack(INTENT_HDR_FMT, NET_INTENT_MAGIC, proto, sender_inc, peer_inc, host_ms & 0xFFFFFFFF,
                      ack_cum, len(ranges), len(msgs), 0)
    for first, count in ranges:
        out += struct.pack(INTENT_RANGE_FMT, first, count)
    for m in msgs:
        pl = bytes(m["payload"])
        out += struct.pack(INTENT_MSG_FMT, m["msgId"], m["slot"], m["wireId"], m["channel"], m["flags"], len(pl),
                           m["seq"], m["hostMs"]) + pl
    return out


def unpack_intent(data: bytes):
    """Parse an 'AC4I' datagram with net.cpp IntentParse's rules. None = not one (or malformed)."""
    hs = struct.calcsize(INTENT_HDR_FMT)
    if len(data) < hs:
        return None
    magic, proto, si, pi, hms, cum, nr, nm, res = struct.unpack_from(INTENT_HDR_FMT, data)
    if magic != NET_INTENT_MAGIC or si == 0 or res != 0 or nr > 16 or nm > 64 or (cum == 0 and nr):
        return None
    o = hs; ranges = []; msgs = []
    for _ in range(nr):
        if o + 6 > len(data): return None
        ranges.append(struct.unpack_from(INTENT_RANGE_FMT, data, o)); o += 6
    for _ in range(nm):
        if o + 27 > len(data): return None
        mid, slot, wid, ch, fl, ln, seq, ms = struct.unpack_from(INTENT_MSG_FMT, data, o); o += 27
        if mid == 0 or not 1 <= ch <= 15 or fl & ~IF_BASE or ln > INTENT_PAYLOAD_MAX or o + ln > len(data): return None
        msgs.append({"msgId": mid, "slot": slot, "wireId": wid, "channel": ch, "flags": fl, "seq": seq, "hostMs": ms,
                     "payload": data[o:o + ln]}); o += ln
    if o != len(data):
        return None
    return {"proto": proto, "senderInc": si, "peerInc": pi, "hostMs": hms, "ackCum": cum, "ranges": ranges, "msgs": msgs}
LOBBY_FMT = "<IIIII"           # magic, protoVer, type, nonce, codeCheck  (frozen, 20 B)
HELLO, WELCOME, REJECT = 1, 2, 3
TYPE_NAME = {HELLO: "HELLO", WELCOME: "WELCOME", REJECT: "REJECT"}


def pack_lobby(ptype: int, nonce: int, check: int = 0, proto: int = NET_PROTO_VER) -> bytes:
    return struct.pack(LOBBY_FMT, NET_LOBBY_MAGIC, proto, ptype, nonce & 0xFFFFFFFF, check & 0xFFFFFFFF)


def unpack_lobby(data: bytes):
    if len(data) != struct.calcsize(LOBBY_FMT):
        return None
    magic, proto, ptype, nonce, check = struct.unpack(LOBBY_FMT, data)
    if magic != NET_LOBBY_MAGIC:
        return None
    return {"proto": proto, "type": ptype, "nonce": nonce, "check": check}


def player_state(seq: int, proto: int = NET_PROTO_VER) -> bytes:
    """576 B: magic, protoVer, seq, everything else zero (valid=0 -> the game applies nothing)."""
    return struct.pack("<III", NET_MAGIC, proto, seq) + bytes(SIZEOF_PLAYERSTATE - 12)


def code_check(ip: str, port: int) -> int:
    raw = bytes(int(p) for p in ip.split(".")) + port.to_bytes(2, "big")
    return lobby_code.crc16_ccitt_false(raw) & 0x0FFF


def lan_ip() -> str | None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 53))  # no packet is sent for a UDP connect
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


class Counter:
    def __init__(self):
        self.player = self.entity = self.intent = self.other = 0

    def add(self, data: bytes):
        magic = struct.unpack_from("<I", data)[0] if len(data) >= 4 else 0
        if magic == NET_MAGIC:
            self.player += 1
        elif magic == NET_ENTITY_MAGIC:
            self.entity += 1
        elif magic == NET_INTENT_MAGIC:   # proto 15, only with [Coop] IntentLane=1 on the game
            self.intent += 1
        else:
            self.other += 1

    def total(self):
        return self.player + self.entity + self.intent

    def __str__(self):
        return f"rx_player={self.player} rx_entity={self.entity} rx_intent={self.intent} rx_other={self.other}"


def stream(sock, peer, seconds: float, proto: int, counter: Counter, on_lobby=None) -> int:
    """Send PlayerState at 20 Hz for `seconds`, counting what comes back from `peer`. Returns tx count."""
    sock.settimeout(0.01)
    seq = tx = 0
    t_end = time.monotonic() + seconds
    next_tx = 0.0
    while time.monotonic() < t_end:
        now = time.monotonic()
        if now >= next_tx:
            sock.sendto(player_state(seq, proto), peer)
            seq += 1; tx += 1
            next_tx = now + 0.05
        try:
            data, src = sock.recvfrom(16384)
        except (socket.timeout, ConnectionResetError):
            continue
        if src[0] != peer[0]:
            continue
        lp = unpack_lobby(data)
        if lp is not None:
            if on_lobby:
                on_lobby(lp, src)
            continue
        counter.add(data)
    return tx


def cmd_join(code: str, seconds: float, proto: int) -> int:
    dec = lobby_code.decode(code)
    if dec is None:
        print(f"[lobby_peer] code {code!r} is invalid - nothing sent")
        return 1
    ip, port = dec
    host = (ip, port)
    nonce = random.getrandbits(32)
    check = code_check(ip, port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    print(f"[lobby_peer] join {code} -> {ip}:{port} from :{sock.getsockname()[1]} proto={proto} nonce={nonce:08x}")
    sock.settimeout(0.25)
    t0 = time.monotonic()
    hellos = 0
    answer = None
    while time.monotonic() - t0 < 15 and answer is None:
        sock.sendto(pack_lobby(HELLO, nonce, check, proto), host)
        hellos += 1
        t_slot = time.monotonic() + 0.25
        while time.monotonic() < t_slot:
            try:
                data, src = sock.recvfrom(16384)
            except socket.timeout:
                break
            except ConnectionResetError:  # Windows: ICMP port-unreachable from a closed port; keep the 250 ms pace
                time.sleep(max(0.0, t_slot - time.monotonic()))
                break
            lp = unpack_lobby(data)
            if lp and src == host and lp["nonce"] == nonce and lp["type"] in (WELCOME, REJECT):
                answer = lp
                break
    if answer is None:
        print(f"[lobby_peer] RESULT join=TIMEOUT hellos={hellos} (no WELCOME/REJECT in 15 s)")
        return 1
    if answer["type"] == REJECT:
        print(f"[lobby_peer] RESULT join=REJECTED host_proto={answer['proto']} ours={proto} hellos={hellos}")
        return 1
    print(f"[lobby_peer] WELCOME from {ip}:{port} after {hellos} HELLO(s); streaming {seconds:.0f} s")
    c = Counter()
    tx = stream(sock, host, seconds, proto, c)
    ok = c.total() > 0
    print(f"[lobby_peer] RESULT join=OK tx_player={tx} {c} link={'BOTH-WAYS' if ok else 'ONE-WAY(no rx)'}")
    return 0 if ok else 1


def cmd_host(port: int, seconds: float, proto: int) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    lan = lan_ip()
    if lan:
        print(f"[lobby_peer] host LAN code {lobby_code.encode(lan, port)} ({lan}:{port})")
    print(f"[lobby_peer] host same-PC code {lobby_code.encode('127.0.0.1', port)} (127.0.0.1:{port}) proto={proto}")
    sock.settimeout(1.0)
    t0 = time.monotonic()
    guest = None
    while time.monotonic() - t0 < seconds and guest is None:
        try:
            data, src = sock.recvfrom(16384)
        except (socket.timeout, ConnectionResetError):
            continue
        lp = unpack_lobby(data)
        if not lp or lp["type"] != HELLO:
            continue
        if lp["proto"] != proto:
            sock.sendto(pack_lobby(REJECT, lp["nonce"], 0, proto), src)
            print(f"[lobby_peer] HELLO from {src[0]}:{src[1]} refused: guest proto={lp['proto']} ours={proto}")
            continue
        sock.sendto(pack_lobby(WELCOME, lp["nonce"], 0, proto), src)
        guest = src
        print(f"[lobby_peer] guest joined {src[0]}:{src[1]} nonce={lp['nonce']:08x} codeCheck={lp['check'] & 0xFFF:03x}")
    if guest is None:
        print(f"[lobby_peer] RESULT host=NO-GUEST in {seconds:.0f} s")
        return 1

    def again(lp, src):  # a HELLO retry that crossed our WELCOME
        if lp["type"] == HELLO and src == guest:
            sock.sendto(pack_lobby(WELCOME, lp["nonce"], 0, proto), src)

    c = Counter()
    left = max(5.0, seconds - (time.monotonic() - t0))
    tx = stream(sock, guest, left, proto, c, on_lobby=again)
    ok = c.total() > 0
    print(f"[lobby_peer] RESULT host=OK tx_player={tx} {c} link={'BOTH-WAYS' if ok else 'ONE-WAY(no rx)'}")
    return 0 if ok else 1


def selftest() -> int:
    fails = []
    p = pack_lobby(HELLO, 0x01020304, 0x0ABC)
    if len(p) != 20 or p[:4] != b"AC4L" or p[4] != NET_PROTO_VER or p[8] != 1 or p[12] != 4 or p[15] != 1:
        fails.append(f"pack layout {p.hex()}")
    u = unpack_lobby(p)
    if u != {"proto": NET_PROTO_VER, "type": HELLO, "nonce": 0x01020304, "check": 0x0ABC}:
        fails.append(f"unpack {u}")
    if unpack_lobby(p[:19]) is not None or unpack_lobby(b"XXXX" + p[4:]) is not None:
        fails.append("unpack accepted a short or foreign datagram")
    if len(player_state(1)) != SIZEOF_PLAYERSTATE:
        fails.append("player_state size")
    if code_check("203.0.113.7", 47801) != 0x5E7:  # = the low 12 bits of SC07-21XT-Q5F7 (net.cpp SelfTestLobby)
        fails.append(f"code_check {code_check('203.0.113.7', 47801):#x}")
    # Drift check against the C++ header: the numbers above must be the numbers there.
    h = NETSTATE.read_text(encoding="utf-8", errors="replace")
    want = {
        "NET_MAGIC": NET_MAGIC, "NET_ENTITY_MAGIC": NET_ENTITY_MAGIC,
        "NET_LOBBY_MAGIC": NET_LOBBY_MAGIC, "NET_PROTO_VER": NET_PROTO_VER,
        "NET_INTENT_MAGIC": NET_INTENT_MAGIC,
    }
    for name, val in want.items():
        m = re.search(rf"static const uint32_t {name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", h)
        if not m or int(m.group(1), 0) != val:
            fails.append(f"{name} differs from src/netstate.h ({m.group(1) if m else 'missing'} vs {val:#x})")
    if not re.search(rf"sizeof\(PlayerState\)\s*==\s*{SIZEOF_PLAYERSTATE}\b", h):
        fails.append("sizeof(PlayerState) static_assert differs from 576")
    if not re.search(r"sizeof\(LobbyPacket\)\s*==\s*20\b", h):
        fails.append("sizeof(LobbyPacket) static_assert differs from 20")
    for name, val in (("LOBBY_HELLO", HELLO), ("LOBBY_WELCOME", WELCOME), ("LOBBY_REJECT", REJECT)):
        m = re.search(rf"{name}\s*=\s*(\d+)", h)
        if not m or int(m.group(1)) != val:
            fails.append(f"{name} differs from src/netstate.h")
    # proto 15 intention lane: sizes and ids against netstate.h, the golden datagram against net.cpp's kGolden
    if struct.calcsize(INTENT_HDR_FMT) != 28 or struct.calcsize(INTENT_MSG_FMT) != 27 or struct.calcsize(INTENT_RANGE_FMT) != 6:
        fails.append("intent struct formats are not 28/27/6 B")
    if not re.search(r"sizeof\(IntentWireHeader\)\s*==\s*28\b", h):
        fails.append("sizeof(IntentWireHeader) static_assert differs from 28")
    for name, val in (("INTENT_MSG_FIXED", 27), ("INTENT_PAYLOAD_MAX", INTENT_PAYLOAD_MAX), ("INTENT_ACK_RANGES_MAX", 16),
                      ("INTENT_MSGS_MAX", 64), ("IC_LIMIT", 16), ("IF_BASE", None)):
        m = re.search(rf"\b{name}\s*=\s*([^,;]+)[,;]", h)
        if not m or (val is not None and m.group(1).strip() != str(val)):
            fails.append(f"{name} differs from src/netstate.h ({m.group(1).strip() if m else 'missing'} vs {val})")
    for cid, cname in INTENT_CHANNELS.items():
        m = re.search(rf"\bIC_{cname}\s*=\s*(\d+)", h)
        if not m or int(m.group(1)) != cid:
            fails.append(f"IC_{cname} differs from src/netstate.h")
    golden = bytes.fromhex(INTENT_GOLDEN_HEX)
    mine = pack_intent(0x11223344, 0x55667788, 0x01020304, 7, [(9, 2)],
                       [{"msgId": 5, "slot": 3, "wireId": 0x0102030405060708, "channel": 1, "flags": IF_BASE,
                         "seq": 0x0A0B0C0D, "hostMs": 0x0E0F1011, "payload": b"xyz"}])
    if mine != golden:
        fails.append(f"pack_intent != golden: {mine.hex()}")
    gi = unpack_intent(golden)
    if not gi or gi["ackCum"] != 7 or gi["ranges"] != [(9, 2)] or gi["msgs"][0]["payload"] != b"xyz" or gi["proto"] != NET_PROTO_VER:
        fails.append(f"unpack_intent(golden) {gi}")
    if unpack_intent(golden[:-1]) is not None or unpack_intent(golden + b"\0") is not None \
            or unpack_intent(golden[:50] + b"\0" + golden[51:]) is not None:   # truncated / trailing / channel 0
        fails.append("unpack_intent accepted a malformed datagram")
    src = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"kGolden\[64\]\s*=\s*\{([^}]*)\}", src)
    cbytes = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))) if m else b""
    if cbytes != golden:
        fails.append(f"src/net.cpp kGolden differs from INTENT_GOLDEN_HEX ({cbytes.hex() or 'missing'})")
    if fails:
        print("[lobby_peer] selftest: FAIL " + "; ".join(fails))
        return 1
    print("[lobby_peer] selftest: PASS (layout, round trip, negatives, code check, netstate.h constants, "
          "proto 15 intent golden vector vs net.cpp)")
    return 0


def main(argv: list[str]) -> int:
    if argv[:1] == ["--selftest"]:
        return selftest()
    ap = argparse.ArgumentParser(description="lobby-mode stand-in peer (slice 3)")
    sub = ap.add_subparsers(dest="cmd")
    j = sub.add_parser("join"); j.add_argument("code"); j.add_argument("--seconds", type=float, default=20)
    j.add_argument("--proto", type=int, default=NET_PROTO_VER)
    h = sub.add_parser("host"); h.add_argument("--port", type=int, default=47801)
    h.add_argument("--seconds", type=float, default=60); h.add_argument("--proto", type=int, default=NET_PROTO_VER)
    a = ap.parse_args(argv)
    if a.cmd == "join":
        return cmd_join(a.code, a.seconds, a.proto)
    if a.cmd == "host":
        return cmd_host(a.port, a.seconds, a.proto)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

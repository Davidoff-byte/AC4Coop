"""Reference implementation of the co-op EVENT layer, testable with no game running.

This is the P2 layer from docs/ops/PLAN-COOP-V2.md: UDP with sequence numbers, cumulative + bitmap
acks, retransmission, and exactly-once delivery by event id. Both ends are Python so the protocol
can be proven under simulated loss and jitter before a line of C++ exists.

    python event_peer.py selftest                 # 200 events, 30% loss, 20-60 ms jitter
    python event_peer.py selftest --loss 0.5 --n 500
    python event_peer.py recv --port 47901        # one side
    python event_peer.py send --port 47902 --to 127.0.0.1:47901 --n 200

Wire format (little endian):

    u32 magic 'AC4V' | u8 ver | u8 flags | u32 seq | u64 ev_id | u32 kind | u32 ack |
    u32 ack_bits | u16 payload_len | payload

`flags` bit0 = ACK_ONLY (keepalive carrying nothing but an ack).
`kind` is the event type (1 damage, 2 death, 3 mission, 4 pickup).
"""
from __future__ import annotations

import argparse
import os
import random
import socket
import struct
import sys
import time

MAGIC = b"AC4V"
VER = 1
HDR = struct.Struct("<4sBBIQIIIH")      # 32 bytes
FLAG_ACK_ONLY = 1

KIND_DAMAGE, KIND_DEATH, KIND_MISSION, KIND_PICKUP = 1, 2, 3, 4

RETX_MS = 40
MAX_RETX = 40


def now_ms():
    return int(time.monotonic() * 1000)


class Sender:
    """Keeps unacked events and retransmits until the peer acknowledges them."""

    def __init__(self, sock, peer, loss=0.0, jitter_ms=0):
        self.sock = sock
        self.peer = peer
        self.loss = loss
        self.jitter = jitter_ms
        self.seq = 0
        self.pending = {}      # seq -> (packet, first_ms, last_ms, tries, ev_id)
        self.sent = 0
        self.retx = 0
        self.acked = set()

    def _emit(self, packet):
        if self.loss and random.random() < self.loss:
            return False        # simulated drop
        if self.jitter:
            time.sleep(random.uniform(0, self.jitter) / 1000.0)
        self.sock.sendto(packet, self.peer)
        return True

    def push(self, ev_id, kind, payload=b""):
        self.seq += 1
        pkt = HDR.pack(MAGIC, VER, 0, self.seq, ev_id, kind, 0, 0,
                       len(payload)) + payload
        self.pending[self.seq] = [pkt, now_ms(), 0, 0, ev_id]
        self.sent += 1
        self._emit(pkt)

    def on_ack(self, ack, bits):
        for seq in list(self.pending):
            if seq <= ack:
                self.acked.add(self.pending[seq][4])
                del self.pending[seq]
            elif seq <= ack + 32 and (bits >> (seq - ack - 1)) & 1:
                self.acked.add(self.pending[seq][4])
                del self.pending[seq]

    def tick(self):
        t = now_ms()
        for seq, rec in list(self.pending.items()):
            pkt, first, last, tries, ev_id = rec
            ref = first if tries == 0 else last
            if t - ref >= RETX_MS and tries < MAX_RETX:
                self._emit(pkt)
                rec[2] = tries + 1
                rec[3] = t
                self.retx += 1


class Receiver:
    """Dedupes by event id and answers every packet with an ack."""

    def __init__(self, sock, peer=None):
        self.sock = sock
        self.peer = peer
        self.next_seq = 1
        self.bits = 0
        self.seen = set()
        self.received = set()
        self.delivered = []
        self.dups = 0
        self.latency = []

    def _ack_packet(self):
        return HDR.pack(MAGIC, VER, FLAG_ACK_ONLY, self.next_seq, 0, 0,
                        self.next_seq - 1, self.bits, 0)

    def handle(self, data, addr, drop=0.0, jitter_ms=0):
        if len(data) < HDR.size:
            return
        magic, ver, flags, seq, ev_id, kind, ack, bits, plen = HDR.unpack_from(data, 0)
        if magic != MAGIC:
            return
        self.peer = addr
        if ack or bits:
            pass      # sender-side ack handled by Sender.on_ack
        if flags & FLAG_ACK_ONLY:
            return
        if seq > 0:
            self.received.add(seq)
        # advance only over a CONTIGUOUS prefix: a cumulative ack must never imply delivery of a
        # sequence that is still missing, or the sender deletes an event the peer never saw.
        while self.next_seq in self.received:
            self.received.discard(self.next_seq)
            self.next_seq += 1
        self.bits = 0
        for i in range(32):
            if (self.next_seq + i) in self.received:
                self.bits |= 1 << i
        if ev_id in self.seen:
            self.dups += 1
        else:
            self.seen.add(ev_id)
            self.delivered.append(ev_id)
            self.latency.append(ev_id)
        if drop and random.random() < drop:
            return                       # drop the ack, forcing a retransmit
        if jitter_ms:
            time.sleep(random.uniform(0, jitter_ms) / 1000.0)
        self.sock.sendto(self._ack_packet(), addr)


def selftest(argv):
    n = 200
    loss = 0.30
    jitter = 40
    for i, a in enumerate(argv):
        if a == "--n":
            n = int(argv[i + 1])
        elif a == "--loss":
            loss = float(argv[i + 1])
        elif a == "--jitter":
            jitter = int(argv[i + 1])

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.002)
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.bind(("127.0.0.1", 0))
    r.settimeout(0.002)

    sender = Sender(s, r.getsockname(), loss=loss, jitter_ms=jitter)
    recv = Receiver(r)

    total = 0
    t0 = now_ms()
    pushed = 0
    while True:
        t = now_ms()
        if pushed < n and t - t0 >= pushed * 5:      # 200/s
            pushed += 1
            sender.push(ev_id=0xA000 + pushed, kind=KIND_DAMAGE, payload=b"\x01\x02")
        sender.tick()
        # pump sockets both ways
        try:
            data, addr = s.recvfrom(2048)
            _, _, flags, _, _, _, ack, bits, _ = HDR.unpack_from(data, 0)
            sender.on_ack(ack, bits)
        except socket.timeout:
            pass
        try:
            data, addr = r.recvfrom(2048)
            recv.handle(data, addr, drop=loss, jitter_ms=jitter)
        except socket.timeout:
            pass
        if pushed >= n and not sender.pending:
            break
        if t - t0 > 60000:
            break

    elapsed = now_ms() - t0
    ok = len(recv.delivered) == n and recv.dups == 0
    print(f"events pushed      {n}")
    print(f"delivered          {len(recv.delivered)}")
    print(f"duplicates         {recv.dups}")
    print(f"retransmissions    {sender.retx}  ({100.0*sender.retx/max(1,n):.0f}% of events)")
    print(f"elapsed            {elapsed} ms")
    print(f"simulated loss     {loss*100:.0f}%  jitter {jitter} ms")
    print(f"RESULT             {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def recv_loop(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    r = Receiver(s)
    print(f"listening on {port}")
    while True:
        data, addr = s.recvfrom(2048)
        r.handle(data, addr)
        if len(r.delivered) % 20 == 0:
            print(f"  delivered {len(r.delivered)}  dups {r.dups}  from {addr}")


def send_loop(port, to, n):
    peer = (to.split(":")[0], int(to.split(":")[1]))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    s.settimeout(0.002)
    tx = Sender(s, peer)
    t0 = now_ms()
    for i in range(n):
        tx.push(0xB000 + i, KIND_DEATH, b"")
        time.sleep(0.005)
    while tx.pending and now_ms() - t0 < 30000:
        tx.tick()
        try:
            data, _ = s.recvfrom(2048)
            _, _, _, _, _, _, ack, bits, _ = HDR.unpack_from(data, 0)
            tx.on_ack(ack, bits)
        except socket.timeout:
            pass
    print(f"sent {n}, retx {tx.retx}, acked {len(tx.acked)}, unacked {len(tx.pending)}")


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cmd = argv[0]
    if cmd == "selftest":
        return selftest(argv[1:])
    if cmd == "recv":
        recv_loop(int(argv[argv.index("--port") + 1]))
    if cmd == "send":
        port = int(argv[argv.index("--port") + 1])
        to = argv[argv.index("--to") + 1]
        n = int(argv[argv.index("--n") + 1]) if "--n" in argv else 100
        send_loop(port, to, n)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))




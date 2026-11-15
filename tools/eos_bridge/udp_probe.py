"""udp_probe.py - stands in for the game on the bridge's local UDP side (docs/design/eos-transport.md, bridge slice 1).

Binds 127.0.0.1:<game-port> (the mod's LocalPort, default 47801), sends sequenced datagrams to the local bridge
(127.0.0.1:<bridge-port>, default 47900) and counts what comes back. With the remote bridge started with --echo, every
datagram travels: probe -> bridge -> EOS P2P -> remote bridge -> EOS P2P -> bridge -> probe.

    py -3.14 tools/eos_bridge/udp_probe.py [--bridge-port 47900] [--game-port 47801]

Phases mirror the spike: 576 B @60 Hz x600, 1170 B @30 Hz x300, plus an oversize 1400 B check (expected: dropped by the
bridge, counted as tooBig). PASS = loss < 2 % on the first two phases and 0 oversize datagrams back.
"""
import argparse, socket, statistics, struct, time

MAGIC = 0x50425541  # 'AUBP'


def phase(sock, dst, name, count, size, hz, pid):
    step = 1.0 / hz
    sent, rtts, seen = 0, [], set()
    t_next = time.perf_counter()
    deadline = t_next + count * step + 8.0
    while time.perf_counter() < deadline and (sent < count or len(seen) < sent):
        now = time.perf_counter()
        if sent < count and now >= t_next:
            body = struct.pack("<IIId", MAGIC, pid, sent, now)
            sock.sendto(body + b"\xA5" * (size - len(body)), dst)
            sent += 1
            t_next += step
        try:
            data, _ = sock.recvfrom(4096)
        except (BlockingIOError, socket.timeout):
            time.sleep(0.0005)
            continue
        except ConnectionResetError:   # Windows ICMP port-unreachable when the bridge is not up yet
            continue
        if len(data) >= 20:
            magic, p, seq, t0 = struct.unpack_from("<IIId", data)
            if magic == MAGIC and p == pid and seq not in seen:
                seen.add(seq)
                rtts.append((time.perf_counter() - t0) * 1000.0)
    s = sorted(rtts)
    med = statistics.median(s) if s else 0.0
    p95 = s[min(len(s) - 1, len(s) * 95 // 100)] if s else 0.0
    loss = 100.0 * (sent - len(seen)) / sent if sent else 0.0
    print(f"  {name:<26} sent={sent:4d} back={len(seen):4d} loss={loss:5.1f}% rtt median={med:7.1f} ms p95={p95:7.1f} ms")
    return sent, len(seen), loss


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bridge-port", type=int, default=47900)
    ap.add_argument("--game-port", type=int, default=47801)
    a = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", a.game_port))
    sock.setblocking(False)
    dst = ("127.0.0.1", a.bridge_port)
    print(f"udp_probe: game side 127.0.0.1:{a.game_port} -> bridge 127.0.0.1:{a.bridge_port}")
    r1 = phase(sock, dst, "576 B @60 Hz", 600, 576, 60.0, 1)
    r2 = phase(sock, dst, "1170 B @30 Hz", 300, 1170, 30.0, 2)
    r3 = phase(sock, dst, "1400 B oversize @10 Hz", 20, 1400, 10.0, 3)
    ok = r1[0] and r1[2] < 2.0 and r2[2] < 2.0 and r3[1] == 0
    print(f"\nVERDICT: {'PASS' if ok else 'FAIL'} (loss < 2 % on 576/1170 B, oversize never delivered)")


if __name__ == "__main__":
    main()

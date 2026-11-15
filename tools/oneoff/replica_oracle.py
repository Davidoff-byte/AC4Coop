"""replica_oracle.py - apply the docs/design/native-replica.md oracle to a phase1 log and the tap that was replayed.
Produced docs/evidence/native-replica-2026-11-15.md (runs 17580 / 19364 / 26108).

    py -3.14 tools/oneoff/replica_oracle.py <phase1_log> <tap>

Reads the `[Replica] win` (2 s windows) and `[Replica] snap` lines. Peer ground path per pass comes from the tap
(PlayerState+80 position, +134 contextId == 4, +100 destSpeed; wire_tap.summarize offsets). A pass boundary is a snap
with err > 10 m (the tap loops from its end back to its start). Reports, then checks:
  1 own movement while the peer runs within 20 % of the peer's ground path per full pass
  2 no snap inside a ground walking segment (only spawn / pass reset / return from another context)
  3 session p95 planar error <= 1.0 m
  4 ground stage mismatch 0 on every window
"""
import math, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import wire_tap as wt  # noqa: E402

log, tap = sys.argv[1], sys.argv[2]
_, recs = wt.tap_read(tap)
peer_path, last = 0.0, None
for _, d in recs:
    if wt.kind(d) != "player" or not d[564]:
        continue
    x, y = struct.unpack_from("<2f", d, 80)
    ctx = struct.unpack_from("<i", d, 134)[0]
    if last and ctx == 4:
        s = math.dist((x, y), last)
        if s < 3: peer_path += s
    last = (x, y)

win = re.compile(r"\[Replica\] win n=(\d+) err mean=([\d.]+) max=([\d.]+) >0.75m=(\d+) moved=([\d.]+)m stage nat=(-?\d+) "
                 r"peer=(-?\d+) mism=(\d+) \| session frames=(\d+) p50=([\d.]+) p95=([\d.]+) snaps=(\d+)")
snap = re.compile(r"\[Replica\] snap #(\d+) err=([\d.]+) dz=(-?[\d.]+)")
passes, cur, snaps_in_pass, lines = [], 0.0, [], Path(log).read_text(errors="replace").splitlines()
p95 = mism = 0; mism_total = 0; started = False
for ln in lines:
    m = snap.search(ln)
    if m:
        err = float(m.group(2))
        if err > 10.0:
            if started: passes.append((cur, snaps_in_pass))
            cur, snaps_in_pass, started = 0.0, [], True
        else:
            snaps_in_pass.append(err)
        continue
    m = win.search(ln)
    if m:
        cur += float(m.group(5)); mism_total += int(m.group(8)); p95 = float(m.group(11)); frames = int(m.group(9))
print(f"tap ground path per pass: {peer_path:.1f} m")
for i, (mv, sn) in enumerate(passes, 1):
    print(f"pass {i}: replica own movement {mv:.1f} m ({100 * mv / peer_path:.0f} % of peer) snaps inside: {len(sn)} {sn}")
print(f"session p95={p95:.2f} m  stage mismatches={mism_total}  full passes={len(passes)}")
ok1 = bool(passes) and all(abs(mv - peer_path) <= 0.2 * peer_path for mv, _ in passes)
ok3, ok4 = p95 <= 1.0, mism_total == 0
print(f"1 movement within 20%: {'PASS' if ok1 else 'FAIL'} | 2 snaps inside passes: see list (judge against the tap's "
      f"context changes) | 3 p95<=1.0: {'PASS' if ok3 else 'FAIL'} | 4 stage: {'PASS' if ok4 else 'FAIL'}")

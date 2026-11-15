"""r41_parkour_report.py - summarise R4.1 (NativeReplicaParkour) windows of a phase1 log, one block per puppet spawn.
Produced docs/evidence/native-replica-parkour-2026-11-15.md (run 26944).

    py -3.14 tools/oneoff/r41_parkour_report.py <phase1_log> [label ...]

Per spawn (labels name the replayed taps in order): snaps (all / parkour / stuck), the longest run of consecutive
snap lines, off-ground windows (pk n > 0) with their 3D error, and the union of the replica's own context ids
(repCtx) versus the peer's (peerCtx) on those windows. A replica context other than 4 (ground) on a parkour window
is the R4.1 oracle's "entered a climb context natively".
"""
import re, sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(errors="replace").splitlines()
labels = sys.argv[2:]
win = re.compile(r"\[Replica\] win .*pk n=(\d+) err3d mean=([\d.]+) max=([\d.]+) repCtx=0x([0-9A-F]+) peerCtx=0x([0-9A-F]+)")
snap = re.compile(r"\[Replica\] snap #\d+ err=([\d.]+) dz=(-?[\d.]+).*?(\[parkour.*\])?$")
blocks, cur = [], None
for ln in lines:
    if "spawn succeeded" in ln:
        cur = {"snaps": 0, "pk": 0, "stuck": 0, "run": 0, "maxrun": 0, "wins": [], "rep": 0, "peer": 0}
        blocks.append(cur)
        continue
    if cur is None:
        continue
    m = snap.search(ln)
    if m:
        cur["snaps"] += 1; cur["run"] += 1; cur["maxrun"] = max(cur["maxrun"], cur["run"])
        if m.group(3): cur["pk"] += 1
        if m.group(3) and "stuck" in m.group(3): cur["stuck"] += 1
        continue
    m = win.search(ln)
    if m:
        cur["run"] = 0
        n = int(m.group(1))
        if n:
            cur["wins"].append((n, float(m.group(2)), float(m.group(3))))
            cur["rep"] |= int(m.group(4), 16); cur["peer"] |= int(m.group(5), 16)


def ids(mask):
    return [i for i in range(32) if mask >> i & 1]


for i, b in enumerate(blocks):
    name = labels[i] if i < len(labels) else f"spawn {i + 1}"
    w = b["wins"]
    mean = sum(n * e for n, e, _ in w) / max(1, sum(n for n, _, _ in w))
    print(f"== {name}: snaps {b['snaps']} (parkour {b['pk']}, stuck {b['stuck']}), longest consecutive snap run {b['maxrun']}")
    print(f"   off-ground windows {len(w)}: 3D error frame-weighted mean {mean:.2f} m, max {max([x for _, _, x in w] or [0]):.2f} m")
    print(f"   peer contexts on them {ids(b['peer'])}; replica contexts {ids(b['rep'])}"
          f" -> native non-ground context: {'YES' if [c for c in ids(b['rep']) if c != 4] else 'NO'}")

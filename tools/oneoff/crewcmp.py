# One-off: compares [CrewDiag] deck-crew layouts (host log vs guest log); produced docs/evidence/spawn-lib-2026-11-28.md s12 (7/7).
# Usage: py -3.14 -I tools/oneoff/crewcmp.py <host phase1_log> <guest phase1_log>
import re, sys
def load(path):
    ships = {}
    for line in open(path, encoding='latin1'):
        m = re.match(r'\[CrewDiag\] ship=(\w+) crew=(\w+) kind=(\d+) local=\(([-\d.]+), ([-\d.]+), ([-\d.]+)\) evt=(\w+) comps=(\d+)', line)
        if m: ships.setdefault(m.group(1), []).append((float(m.group(4)), float(m.group(5)), float(m.group(6)), m.group(7), int(m.group(8))))
    return ships
h = load(sys.argv[1]); g = load(sys.argv[2])
for tag, d in (('host', h), ('guest', g)):
    for s, crew in d.items():
        print(f"{tag} ship {s}: {len(crew)} crew, evt {sorted(set(c[3] for c in crew))}, comps {sorted(set(c[4] for c in crew))}")
def key(c): return (round(c[0], 1), round(c[1], 1), round(c[2], 1))
hs = [sorted(key(c) for c in v) for v in h.values()]; gs = [sorted(key(c) for c in v) for v in g.values()]
for i, gl in enumerate(gs):
    best = max(((sum(1 for p in gl if any(abs(p[0]-q[0]) <= 0.15 and abs(p[1]-q[1]) <= 0.15 and abs(p[2]-q[2]) <= 0.15 for q in hl)), j) for j, hl in enumerate(hs)), default=(0, -1))
    print(f"guest hull {i}: {len(gl)} crew, best host match {best[0]}/{len(gl)} offsets (host ship #{best[1]}, {len(hs[best[1]]) if best[1] >= 0 else 0} crew)")

# One-off: produced the run table in docs/evidence/native-replica-climb-input-2026-11-15.md
"""cisum.py <log> <from_line> <to_line>: summarise a NativeReplicaClimbIntent replay window of phase1_log."""
import sys, re
lines = open(sys.argv[1], errors='replace').read().splitlines()[int(sys.argv[2]):int(sys.argv[3])]
fw = fs = 0.0; real = der = 0; snaps = pk = stuck = defer = 0; pkn = pks = 0.0; walls = 0; ctxs = {}
for l in lines:
    m = re.search(r'climb intent: frames=(\d+) mean \|err on wall\|=([\d.]+) m.*real=(\d+) derived=(\d+)', l)
    if m:
        f, e = int(m[1]), float(m[2]); fw += f; fs += f * e; real, der = int(m[3]), int(m[4])
    if '] snap #' in l:
        snaps += 1
        if 'parkour' in l: pk += 1
    if 'snap deferred' in l: defer += 1
    m = re.search(r'pk n=(\d+) err3d mean=([\d.]+)', l)
    if m and int(m[1]):
        pkn += int(m[1]); pks += int(m[1]) * float(m[2])
    if re.search(r'slot21 -> 1|slot21 -> \d / 1', l): walls += 1
print(f'on-wall frames={fw:.0f} weighted mean err={fs / fw if fw else 0:.2f} m | climb input frames real={real} derived={der}')
print(f'parkour frames={pkn:.0f} weighted err3d={pks / pkn if pkn else 0:.2f} m | snaps={snaps} (parkour {pk}) deferred={defer} | wall-run accepts={walls}')

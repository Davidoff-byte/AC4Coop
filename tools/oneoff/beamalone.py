# One-off: produced the beam-alone / both-climbing shares in docs/evidence/climb-moves-2026-11-15.md
"""beamalone.py <log> <from> <to>: share of [Replica] 2 s windows with the replica on a beam (ctx 9) the peer is not on,
and with both in Climbing (30)."""
import sys, re
lines = open(sys.argv[1], errors='replace').read().splitlines()[int(sys.argv[2]):int(sys.argv[3])]
n = b = c = 0
for l in lines:
    if '[Replica] win' not in l:
        continue
    m = re.search(r'repCtx=0x([0-9a-f]+) peerCtx=0x([0-9a-f]+)', l)
    if not m:
        continue
    n += 1; r = int(m[1], 16); p = int(m[2], 16)
    if r & 0x200 and not p & 0x200: b += 1
    if r & (1 << 30) and p & (1 << 30): c += 1
print(f'windows={n} replica-on-beam-alone={b} ({100 * b / max(n, 1):.0f}%) both-climbing={c} ({100 * c / max(n, 1):.0f}%)')

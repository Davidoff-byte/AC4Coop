# One-off: produced the ClimbBlock counts in docs/evidence/native-replica-climb-input-2026-11-15.md
"""tapclimb.py <repo> <tap>: count PlayerState datagrams carrying a ClimbBlock ('CLMB' magic) and print the input timeline."""
import sys, struct
sys.path.insert(0, sys.argv[1] + '/tools')
import wire_tap as wt
meta, recs = wt.tap_read(sys.argv[2])
n = hit = 0; last = None; nz = 0
for t, d in recs:
    i = d.find(b'CLMB')
    if len(d) != 576 and i < 0:
        continue
    n += 1
    if i < 0:
        continue
    hit += 1
    x, y, sp = struct.unpack_from('<3f', d, i + 4)
    if abs(x) + abs(y) > 0.05: nz += 1
    cur = (round(x, 2), round(y, 2), round(sp, 2))
    if cur != last:
        print(f'{t:7.2f}s off={i} dir=({x:+.2f},{y:+.2f}) speed={sp:.2f}')
        last = cur
print(f'datagrams~PlayerState={n} with ClimbBlock={hit} nonzero input={nz}')

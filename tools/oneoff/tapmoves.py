# One-off: produced the move list in docs/evidence/climb-moves-2026-11-15.md
"""tapmoves.py <repo> <tap>: list the climbing moves (ClimbBlock moveSeq / hand holds / ObjectID) a tap carries."""
import sys, struct
sys.path.insert(0, sys.argv[1] + '/tools')
import wire_tap as wt
meta, recs = wt.tap_read(sys.argv[2])
last = None; n = 0
for t, d in recs:
    i = d.find(b'CLMB')
    if i < 0 or len(d) != 576:   # PlayerState datagrams only (other lanes can contain the bytes by chance)
        continue
    seq, = struct.unpack_from('<I', d, i + 16)
    h = struct.unpack_from('<6f', d, i + 20); obj, = struct.unpack_from('<Q', d, i + 44)
    if seq and seq != last:
        n += 1; last = seq
        m = [(h[k] + h[k + 3]) / 2 for k in range(3)]
        print(f'{t:7.2f}s move {seq:3d} hands mid=({m[0]:.2f},{m[1]:.2f},{m[2]:.2f}) span={((h[0]-h[3])**2+(h[1]-h[4])**2)**0.5:.2f} obj={obj:016X}')
print(f'moves={n}')

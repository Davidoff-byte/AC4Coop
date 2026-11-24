#!/usr/bin/env python3
"""puppet_check.py HOST_LOG GUEST_LOG - score [Coop] BrainPuppet (docs/evidence/brain-s0-2026-11-22.md, run 21).

Reads `[Brain] ppath role=... hostMs=... path=...` lines (every change of the host player's / the guest puppet's Ground path,
stamped on the shared host clock). For each host path inside the fight branch (contains /163/), looks for the same path on
the guest within [-50, +250] ms. Prints matched / total, the lag distribution, and the host fight states (path from 163 down)
that never arrived on the puppet, by count.
"""
import re, sys
from collections import Counter


def read(path, role):
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(rf'\[Brain\] ppath role={role} hostMs=(\d+) path=([\d/]+)', line)
            if m: out.append((int(m.group(1)), m.group(2)))
    return out


def fight_tail(p):
    parts = p.split('/')
    return '/'.join(parts[parts.index('163'):]) if '163' in parts else None


def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(len(v) * q))] if v else None


def main(hp, gp):
    H, G = read(hp, 'host'), read(gp, 'guest')
    hf = [(t, p) for t, p in H if fight_tail(p)]
    lags, missed = [], Counter()
    for t, p in hf:
        cand = [gt - t for gt, gpth in G if gpth == p and -50 <= gt - t <= 250]
        if cand: lags.append(min(cand, key=abs))
        else: missed[fight_tail(p)] += 1
    n = len(hf)
    frac = len(lags) / n if n else 0
    print(f"puppet: host fight paths={n} matched={len(lags)} ({100 * frac:.1f}%) lag ms p50={pct(lags, .5)} p95={pct(lags, .95)} "
          f"max={max(lags) if lags else None} -> {'PASS' if n and frac >= .90 else 'FAIL'} (bar 90 %)")
    print("  host paths total", len(H), "guest paths total", len(G))
    print("  never on the puppet (host fight state: count):", ', '.join(f'{k}: {v}' for k, v in missed.most_common(20)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

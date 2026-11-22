#!/usr/bin/env python3
"""brain_check.py HOST_LOG GUEST_LOG - score a [Coop] Brain run against docs/evidence/brain-s0-2026-11-22.md.

Clock: every guest [Clock] line from 10 s after its first estimate; error = offUs - skewUs(host). PASS p95 < 5000 us.
Lane: markers (issue on the host) matched by (id, execMs) to the host's and guest's exec lines; moment = execMs + lateMs.
PASS: >= 99 % of markers executed on both, and |dt| <= 33 ms for >= 99 % of those.
Attacks: for every guest exec of slot 155/160/546, the host s173 and guest s173 for the same (id, execMs), dt in host ms.
"""
import re, sys

def lines(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.read().splitlines()

def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(len(v) * q))] if v else None

def main(hp, gp):
    H, G = lines(hp), lines(gp)
    skew = 0
    for l in H:
        m = re.search(r'\[Clock\] role=host skewUs=(-?\d+)', l)
        if m: skew = int(m.group(1)); break
    errs, t0 = [], None
    for l in G:
        m = re.search(r'\[Clock\] role=guest .*have=1 offUs=(-?\d+).*localUs=(\d+)', l)
        if not m: continue
        lu = int(m.group(2))
        if t0 is None: t0 = lu
        if lu - t0 >= 10_000_000: errs.append(abs(int(m.group(1)) - skew))
    p95 = pct(errs, 0.95)
    print(f"clock: n={len(errs)} skewUs={skew} errUs p50={pct(errs, .5)} p95={p95} max={max(errs) if errs else None} -> "
          f"{'PASS' if errs and p95 < 5000 else 'FAIL'}")
    issue = [(m.group(1), int(m.group(2))) for l in H for m in [re.search(r'\[Brain\] issue slot=mark id=(\w+) execMs=(\d+)', l)] if m]
    def execs(L, role):
        d = {}
        for l in L:
            m = re.search(rf'\[Brain\] exec role={role} slot=mark id=(\w+) execMs=(\d+) lateMs=(-?\d+)', l)
            if m: d[(m.group(1), int(m.group(2)))] = int(m.group(3))
        return d
    he, ge = execs(H, 'host'), execs(G, 'guest')
    both = [(k, he[k], ge[k]) for k in issue if k in he and k in ge]
    hostOnly = [k for k in issue if k in he and k not in ge]
    dts = [abs(g - h) for _, h, g in both]
    ok = sum(1 for d in dts if d <= 33)
    frac = len(both) / len(issue) if issue else 0
    print(f"lane: issued={len(issue)} hostExec={sum(1 for k in issue if k in he)} bothExec={len(both)} ({100 * frac:.1f}%) "
          f"hostOnly={len(hostOnly)} |dt|ms p50={pct(dts, .5)} p95={pct(dts, .95)} max={max(dts) if dts else None} "
          f"within33={ok}/{len(dts)} -> {'PASS' if issue and frac >= .99 and ok >= .99 * len(dts) else 'FAIL'}")
    if hostOnly: print("  host-only markers (first 10):", hostOnly[:10])
    # attacks
    gx = [(m.group(2), int(m.group(3)), m.group(1)) for l in G for m in [re.search(r'\[Brain\] exec role=guest slot=(155|160|546) id=(\w+) .*execMs=(\d+)', l)] if m]
    def s173(L, role):
        d = {}
        for l in L:
            m = re.search(rf'\[Brain\] s173 role={role} id=(\w+) execMs=(\d+) hostMs=(\d+)', l)
            if m: d.setdefault((m.group(1), int(m.group(2))), int(m.group(3)))
        return d
    hs, gs = s173(H, 'host'), s173(G, 'guest')
    caps = sum(1 for l in H if '[Brain] cap ' in l)
    adt = [abs(gs[(i, e)] - hs[(i, e)]) for i, e, _ in gx if (i, e) in hs and (i, e) in gs]
    print(f"attacks: hostCapLines={caps} guestExec={len(gx)} hostS173={len(hs)} guestS173={len(gs)} matched={len(adt)} "
          f"|dt|ms p50={pct(adt, .5)} p95={pct(adt, .95)} within33={sum(1 for d in adt if d <= 33)}/{len(adt)}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

"""ship_coop_metrics.py - pass/fail oracle for [Coop] ShipCoop (docs/design/ship-coop.md).

    py -3.14 tools/ship_coop_metrics.py HOST.csv GUEST.csv [--json out.json]

Inputs are the two ship_coop_<pid>.csv files ([Coop] ShipCoopMetrics=1) of a same-PC two-copy run: both copies
stamp rows with the same QueryPerformanceCounter clock, so rows are joined by time directly.

Criteria (fixed before the 2026-11-16 run; changing them after a run is not allowed - add a new criterion instead):
  S1 ship co-location   while a side follows (engaged) and the owner ship moves > 1 m/s: horizontal distance
                        between the two ships' entity positions at the same instant, p95 < 1.0 m
  S2 ship smoothness    every keyframe step on each side (K rows, the pose the hull is keyframed to): horizontal
                        acceleration from second differences, max < 20 m/s^2 (a 0.1 m snap at 60 Hz is ~360 m/s^2);
                        reported separately for the +/-2 s windows around every helm grant
  D1 deck accuracy      the puppet's position in the LOCAL ship frame vs the peer's own deck position at t - d,
                        best d in [0, 400] ms: p95 < 0.35 m, and best d < 250 ms
  D2 puppet deck step   largest frame-to-frame jump of the puppet in the local ship frame < 0.30 m
  D3 glue               windows >= 2 s where the peer stands still on deck (own deck speed < 0.05 m/s) while the ship
                        moves > 1 m/s: puppet deck range < 0.10 m
A criterion with no qualifying data is reported NO-DATA (never PASS).
"""
from __future__ import annotations
import argparse, bisect, json, math, sys


def load(path):
    F, K, H = [], [], []
    for ln in open(path, encoding='utf-8', errors='replace'):
        if not ln or ln[0] == '#':
            continue
        p = ln.rstrip('\n').split(',')
        try:
            if p[0] == 'F' and len(p) >= 24:
                F.append(dict(t=float(p[1]), owner=int(p[3]), follow=int(p[4]), eng=int(p[5]), ctx=int(p[6]), on=int(p[7]),
                              sx=float(p[8]), sy=float(p[9]), sz=float(p[10]), dx=float(p[13]), dy=float(p[14]), dz=float(p[15]),
                              pup=int(p[16]), px=float(p[17]), py=float(p[18]), pz=float(p[19]), holder=int(p[21]), epoch=int(p[22])))
            elif p[0] == 'K' and len(p) >= 14:
                K.append(dict(t=float(p[1]), mode=int(p[3]), x=float(p[4]), y=float(p[5]), z=float(p[6]), dt=float(p[11]), err=float(p[12])))
            elif p[0] == 'H':
                H.append(dict(t=float(p[1]), who=p[2], holder=int(p[4]), epoch=int(p[5])))
        except ValueError:
            pass
    return F, K, H


def pct(v, q):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


_TS = {}


def interp(rows, t, keys):
    ts = _TS.get(id(rows))
    if ts is None or len(ts) != len(rows):
        ts = _TS[id(rows)] = [r['t'] for r in rows]
    i = bisect.bisect_left(ts, t)
    if i <= 0 or i >= len(rows):
        return None
    a, b = rows[i - 1], rows[i]
    if b['t'] - a['t'] > 200:
        return None
    u = (t - a['t']) / (b['t'] - a['t'])
    return {k: a[k] + (b[k] - a[k]) * u for k in keys}


def speed_at(rows, i):
    j = max(0, i - 5)
    if rows[i]['t'] - rows[j]['t'] <= 0:
        return 0.0
    return math.hypot(rows[i]['sx'] - rows[j]['sx'], rows[i]['sy'] - rows[j]['sy']) / ((rows[i]['t'] - rows[j]['t']) / 1000)


def accel(K, windows=None):
    out = []
    for i in range(2, len(K)):
        a, b, c = K[i - 2], K[i - 1], K[i]
        d1, d2 = (b['t'] - a['t']) / 1000, (c['t'] - b['t']) / 1000
        if not (0.002 < d1 < 0.2 and 0.002 < d2 < 0.2):
            continue
        if windows is not None and not any(w0 <= c['t'] <= w1 for w0, w1 in windows):
            continue
        v1 = ((b['x'] - a['x']) / d1, (b['y'] - a['y']) / d1)
        v2 = ((c['x'] - b['x']) / d2, (c['y'] - b['y']) / d2)
        out.append((math.hypot(v2[0] - v1[0], v2[1] - v1[1]) / ((d1 + d2) / 2), c['t'], c['mode']))
    return out


def verdict(ok, have):
    return 'NO-DATA' if not have else ('PASS' if ok else 'FAIL')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host'); ap.add_argument('guest'); ap.add_argument('--json')
    a = ap.parse_args()
    hF, hK, hH = load(a.host)
    gF, gK, gH = load(a.guest)
    res = {}
    print(f"host: F={len(hF)} K={len(hK)} H={len(hH)}   guest: F={len(gF)} K={len(gK)} H={len(gH)}")
    grants = sorted([h['t'] for h in hH + gH])
    print("grants:", [(round(h['t']), h['who'], h['holder'], h['epoch']) for h in sorted(hH + gH, key=lambda x: x['t'])])

    # S1 co-location
    d = []
    for side, (F, O) in {'guest-follows': (gF, hF), 'host-follows': (hF, gF)}.items():
        for i, r in enumerate(F):
            if not (r['eng'] and r['on']):
                continue
            o = interp(O, r['t'], ['sx', 'sy'])
            if o is None or speed_at(F, i) < 1.0:
                continue
            d.append(math.hypot(r['sx'] - o['sx'], r['sy'] - o['sy']))
    p95 = pct(d, 0.95)
    res['S1'] = dict(n=len(d), p50=pct(d, .5), p95=p95, max=max(d) if d else None, verdict=verdict(p95 is not None and p95 < 1.0, bool(d)))

    # S2 smoothness
    win = [(t - 2000, t + 2000) for t in grants]
    for name, K in (('host', hK), ('guest', gK)):
        al = accel(K)
        aw = accel(K, win) if win else []
        m = max(al)[0] if al else None
        mw = max(aw)[0] if aw else None
        ov = [x[0] for x in al if x[2] == 1]
        nv = [x[0] for x in al if x[2] == 0]
        res[f'S2_{name}'] = dict(n=len(al), max=m, at=max(al)[1] if al else None, p99=pct([x[0] for x in al], .99),
                                 overridden_max=max(ov) if ov else None, native_max=max(nv) if nv else None,
                                 grant_windows_n=len(aw), grant_windows_max=mw,
                                 verdict=verdict(m is not None and m < 20.0, bool(al)))

    # D1/D2/D3 deck (puppet on side X = peer Y's body)
    for name, X, Y in (('guest-on-host', hF, gF), ('host-on-guest', gF, hF)):
        P = [r for r in X if r['pup'] and r['on']]
        Yd = [r for r in Y if r['on']]
        if len(P) < 30 or len(Yd) < 30:
            res[f'D1_{name}'] = dict(verdict='NO-DATA', n=len(P)); res[f'D2_{name}'] = dict(verdict='NO-DATA'); res[f'D3_{name}'] = dict(verdict='NO-DATA')
            continue
        best = None
        for dms in range(0, 401, 10):
            e = []
            for r in P:
                y = interp(Yd, r['t'] - dms, ['dx', 'dy', 'dz'])
                if y:
                    e.append(math.dist((r['px'], r['py'], r['pz']), (y['dx'], y['dy'], y['dz'])))
            if e and (best is None or pct(e, .95) < best[1]):
                best = (dms, pct(e, .95), pct(e, .5), len(e))
        e0 = [math.dist((r['px'], r['py'], r['pz']), (y['dx'], y['dy'], y['dz'])) for r in P
              for y in [interp(Yd, r['t'], ['dx', 'dy', 'dz'])] if y]
        res[f'D1_{name}'] = dict(best_delay_ms=best[0], p95=best[1], p50=best[2], n=best[3], p95_at_0ms=pct(e0, .95),
                                 verdict=verdict(best[1] < 0.35 and best[0] < 250, True))
        t0 = P[0]['t'] + 1000
        steps = [(math.dist((P[i]['px'], P[i]['py'], P[i]['pz']), (P[i - 1]['px'], P[i - 1]['py'], P[i - 1]['pz'])), P[i]['t'])
                 for i in range(1, len(P)) if P[i]['t'] > t0 and P[i]['t'] - P[i - 1]['t'] < 200]
        ms = max(steps) if steps else (None, None)
        res[f'D2_{name}'] = dict(max_step=ms[0], at=ms[1], verdict=verdict(ms[0] is not None and ms[0] < 0.30, bool(steps)))
        # D3: still windows of the peer
        wins, i = [], 0
        while i < len(Yd):
            j = i
            while j + 1 < len(Yd) and math.dist((Yd[j + 1]['dx'], Yd[j + 1]['dy']), (Yd[i]['dx'], Yd[i]['dy'])) < 0.05 * max(1.0, (Yd[j + 1]['t'] - Yd[i]['t']) / 1000):
                j += 1
            if Yd[j]['t'] - Yd[i]['t'] >= 2000:
                wins.append((Yd[i]['t'], Yd[j]['t']))
            i = j + 1
        ranges = []
        for w0, w1 in wins:
            pts = [r for r in P if w0 + g_delay(best) <= r['t'] <= w1]
            idx = [k for k, r in enumerate(X) if w0 <= r['t'] <= w1]
            if len(pts) < 10 or not idx or speed_at(X, idx[len(idx) // 2]) < 1.0:
                continue
            xs, ys, zs = [p['px'] for p in pts], [p['py'] for p in pts], [p['pz'] for p in pts]
            ranges.append(max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)))
        res[f'D3_{name}'] = dict(windows=len(ranges), max_range=max(ranges) if ranges else None,
                                 verdict=verdict(bool(ranges) and max(ranges) < 0.10, bool(ranges)))
    for k, v in res.items():
        print(f"{k:22s} {v['verdict']:8s} {json.dumps({x: y for x, y in v.items() if x != 'verdict'})}")
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)


def g_delay(best):
    return (best[0] if best else 0) + 200


if __name__ == '__main__':
    main()

"""scorecard.py - one table of "does co-op feel seamless" numbers from a two-copy run's logs (tools/two_copy.ps1).

    py -3.14 tools/scorecard.py <game_dir> <host_pid> <guest_pid> [--json out.json]

Reads phase1_log_<pid>.txt of both copies (only lines the plugin already writes) and reports:
  puppet action match   [ActMatch] (needs [Coop] ActMatchProbe=1): matched / scored frames per peer context, per side
  puppet smoothing      [Smooth]: share of frames interpolated vs held / extrapolated / clamped, mean and max per-frame
                        movement (a large maxD is a visible pop)
  frame rate proxy      [ActMatch] frames per 5 s window -> apply calls per second on each side
  guard sync (guest)    [Sync] bind meanErr / maxErr (m), mirror nobod ratio, [Bind] doubleNow max
  fight intents         [FI] stats rx vs applied, unresolved
  owner damage          [OD] stats blocked / tx / host applied / kills / unresolved
  liveness              whether each pid is still running (a crash shows here and in %LOCALAPPDATA%\\CrashDumps)
The thresholds in THRESH are the pass bars used for slices of docs/design/storm-replication.md; a row that misses its
bar prints FAIL. They are stated up front so a change cannot move them after the fact.
"""
from __future__ import annotations
import json
import os
import re
import subprocess
import sys

THRESH = {
    'act_ground_pct': 95.0,      # puppet plays the peer's action on the ground
    'act_offground_pct': 80.0,   # parkour / air contexts (6, 7, 8, 9, 10, 30) pooled
    'smooth_interp_pct': 90.0,   # puppet frames interpolated (not held / extrapolated / clamped)
    'smooth_maxD_m': 0.50,       # largest single-frame puppet move in a window (a pop)
    'bind_maxErr_m': 1.00,       # linked guard position error vs host
    'apply_hz_min': 25.0,        # puppet apply calls per second (frame-rate proxy)
    'guard_step_m': 1.00,        # largest single position write of a linked guard (a pop), [Bind] takeover maxStep
    # enemy placement (docs/design/enemy-placement.md section 5), set 2026-11-16 before the first [Place] run:
    'place_step_m': 0.50,        # largest write step of a linked guard, same metric as guard_step_m, tighter bar
    'place_nobody_near_pct': 5.0,   # alive host guards < 30 m from the guest player with no guest body
    'place_nobody_all_pct': 12.0,   # all alive host guard records with no guest body
    'place_err_mean_m': 0.35,    # settled links (>= 3 s, not approaching): body vs host's newest position, at apply
    'place_err_max_m': 1.50,
    'place_frozen': 0,           # links whose host guard moved > 1 m in >= 2 s while the body moved < 0.1 m
}
OFFGROUND = {6, 7, 8, 9, 10, 30}


def lines(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read().splitlines()
    except OSError:
        return []


def alive(pid):
    out = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/NH'], capture_output=True, text=True).stdout
    return str(pid) in out


def side(game, pid):
    L = lines(os.path.join(game, f'phase1_log_{pid}.txt'))
    r = {'pid': pid, 'alive': alive(pid), 'act': {}, 'act_frames': [], 'smooth': [], 'bind': [], 'nobod': [0, 0],
         'double': 0, 'fi': None, 'od': None}
    for s in L:
        m = re.search(r'\[ActMatch\] mode=\w+ ctx=(\d+) frames=(\d+) match=(\d+)/(\d+)', s)
        if m:
            c, fr, ok, sc = map(int, m.groups())
            a = r['act'].setdefault(c, [0, 0]); a[0] += ok; a[1] += sc
            if c == 4 and fr > 50: r['act_frames'].append(fr)
            continue
        m = re.search(r'\[Smooth\] frames=(\d+) hold=(\d+) ext=(\d+) clamp=(\d+).*avgD=([\d.]+) maxD=([\d.]+)', s)
        if m:
            fr, ho, ex, cl = map(int, m.groups()[:4]); r['smooth'].append((fr, ho, ex, cl, float(m.group(5)), float(m.group(6))))
            m3 = re.search(r'maxD3=([\d.]+) dtAtMax=([\d.]+)ms maxV=([\d.]+)', s)   # added 2026-11-16, informational
            if m3: r.setdefault('smooth3', []).append(tuple(map(float, m3.groups())))
            continue
        m = re.search(r'\[Sync\] bind sent=(\d+) bound=(\d+) unmatched=(\d+) meanErr=([\d.]+) maxErr=([\d.]+)', s)
        if m: r['bind'].append((float(m.group(4)), float(m.group(5)))); continue
        m = re.search(r'mirror ok=(\d+) nobod=(\d+)', s)
        if m: r['nobod'][0] += int(m.group(1)); r['nobod'][1] += int(m.group(2))
        m = re.search(r'\[Bind\] lock=\d+ linked=\d+ doubleNow=(\d+)', s)
        if m: r['double'] = max(r['double'], int(m.group(1)))
        m = re.search(r'\[Bind\] takeover .*maxStep=([\d.]+)', s)
        if m: r['step'] = max(r.get('step', 0.0), float(m.group(1)))
        m = re.search(r'\[Place\] on=(\d) band<30 rec=(\d+) linked=(\d+).*30-60 rec=(\d+) linked=(\d+).*>=60 rec=(\d+) linked=(\d+).*'
                      r'settledErr mean=([\d.]+) max=([\d.]+) n=(\d+) \| frozen=(\d+) \| orphansNear=(\d+)', s)
        if m:
            v = m.groups(); p = r.setdefault('place', {'band': [[0, 0], [0, 0], [0, 0]], 'errw': 0.0, 'errn': 0, 'errmax': 0.0,
                                                       'frozen': 0, 'orph': []})
            for b in range(3): p['band'][b][0] += int(v[1 + 2 * b]); p['band'][b][1] += int(v[2 + 2 * b])
            n = int(v[9]); p['errw'] += float(v[7]) * n; p['errn'] += n; p['errmax'] = max(p['errmax'], float(v[8]))
            p['frozen'] += int(v[10]); p['orph'].append(int(v[11]))
            continue
        m = re.search(r'\[Place\] writer .*movedByOther=(\d+) max=([\d.]+) m \| stuck=(\d+)(?: recycled=\d+)? \| maxStep=([\d.]+) m steps>0.5=(\d+)', s)
        if m:
            w = r.setdefault('placew', [0, 0.0, 0, 0.0, 0])
            w[0] += int(m.group(1)); w[1] = max(w[1], float(m.group(2))); w[2] += int(m.group(3)); w[3] = max(w[3], float(m.group(4)))
            w[4] += int(m.group(5))
            m2 = re.search(r'gap<=100ms maxStep=([\d.]+) m \| gap>100ms writes=(\d+) steps>0.5=(\d+)', s)
            if m2:
                w2 = r.setdefault('placew2', [0.0, 0, 0])
                w2[0] = max(w2[0], float(m2.group(1))); w2[1] += int(m2.group(2)); w2[2] += int(m2.group(3))
            continue
        if s.startswith('[FI] stats'): r['fi'] = s
        if s.startswith('[OD] stats'): r['od'] = s
    return r


def pct(a, b):
    return 100.0 * a / b if b else float('nan')


def report(h, g):
    rows = []
    def row(name, val, bar=None, good=None):
        verdict = '' if good is None else ('PASS' if good else 'FAIL')
        rows.append((name, val, '' if bar is None else bar, verdict))
    for tag, s in (('guest puppet (of host)', g), ('host puppet (of guest)', h)):
        gr = s['act'].get(4, [0, 0]); og = [sum(s['act'].get(c, [0, 0])[0] for c in OFFGROUND), sum(s['act'].get(c, [0, 0])[1] for c in OFFGROUND)]
        if gr[1]: row(f'{tag}: action match ground', f'{pct(*gr):.1f}% ({gr[0]}/{gr[1]})', f">={THRESH['act_ground_pct']}", pct(*gr) >= THRESH['act_ground_pct'])
        if og[1]: row(f'{tag}: action match off-ground', f'{pct(*og):.1f}% ({og[0]}/{og[1]})', f">={THRESH['act_offground_pct']}", pct(*og) >= THRESH['act_offground_pct'])
        if s['act_frames']:
            hz = sum(s['act_frames']) / len(s['act_frames']) / 5.0
            row(f'{tag}: apply rate', f'{hz:.1f}/s', f">={THRESH['apply_hz_min']}", hz >= THRESH['apply_hz_min'])
        if s['smooth']:
            fr = sum(x[0] for x in s['smooth']); bad = sum(x[1] + x[2] + x[3] for x in s['smooth'])
            mx = max(x[5] for x in s['smooth'])
            row(f'{tag}: smoothing interpolated', f'{pct(fr - bad, fr):.1f}% of {fr}', f">={THRESH['smooth_interp_pct']}", pct(fr - bad, fr) >= THRESH['smooth_interp_pct'])
            row(f'{tag}: largest per-frame move', f'{mx:.3f} m', f"<={THRESH['smooth_maxD_m']}", mx <= THRESH['smooth_maxD_m'])
            if s.get('smooth3'):   # no bar: tells a pop from a long frame (true 3D distance, its frame time, peak speed)
                d3, dt, _ = max(s['smooth3']); v = max(x[2] for x in s['smooth3'])
                row(f'{tag}: largest move 3D / frame time / peak speed', f'{d3:.3f} m in {dt:.1f} ms / {v:.1f} m/s')
    if g['bind']:
        mx = max(b[1] for b in g['bind']); mean = sum(b[0] for b in g['bind']) / len(g['bind'])
        row('guest guards: position error', f'mean {mean:.2f} m, max {mx:.2f} m', f"max<={THRESH['bind_maxErr_m']}", mx <= THRESH['bind_maxErr_m'])
    ok, nb = g['nobod']
    if ok + nb: row('guest guards: records with no body', f'{pct(nb, ok + nb):.1f}% ({nb}/{ok + nb})')
    row('guest guards: double links (max)', str(g['double']), '0', g['double'] == 0)
    if 'step' in g:
        row('guest guards: largest write step', f"{g['step']:.2f} m", f"<={THRESH['guard_step_m']}", g['step'] <= THRESH['guard_step_m'])
    if 'step' in g:
        row('guest guards: largest write step (placement bar)', f"{g['step']:.2f} m", f"<={THRESH['place_step_m']}", g['step'] <= THRESH['place_step_m'])
    p = g.get('place')
    if p:
        names = ('<30 m', '30-60 m', '>=60 m')
        for b in range(3):
            rec, lk = p['band'][b]
            if rec: row(f'guest guards {names[b]}: records with no body', f'{pct(rec - min(lk, rec), rec):.1f}% ({rec - min(lk, rec)}/{rec})',
                        f"<={THRESH['place_nobody_near_pct']}" if b == 0 else '', (pct(rec - min(lk, rec), rec) <= THRESH['place_nobody_near_pct']) if b == 0 else None)
        rec = sum(x[0] for x in p['band']); lk = sum(min(x[1], x[0]) for x in p['band'])
        if rec: row('guest guards (alive, all bands): no body', f'{pct(rec - lk, rec):.1f}% ({rec - lk}/{rec})', f"<={THRESH['place_nobody_all_pct']}", pct(rec - lk, rec) <= THRESH['place_nobody_all_pct'])
        if p['errn']:
            mean = p['errw'] / p['errn']
            row('guest guards: settled error mean', f"{mean:.2f} m (n={p['errn']})", f"<={THRESH['place_err_mean_m']}", mean <= THRESH['place_err_mean_m'])
            row('guest guards: settled error max', f"{p['errmax']:.2f} m", f"<={THRESH['place_err_max_m']}", p['errmax'] <= THRESH['place_err_max_m'])
        row('guest guards: frozen bodies', str(p['frozen']), f"<={THRESH['place_frozen']}", p['frozen'] <= THRESH['place_frozen'])
        if p['orph']: row('guest-only fighters < 40 m (orphans), max per window', str(max(p['orph'])))
    w = g.get('placew')
    if w: row('place writer: moved-by-other / max / stuck / maxStep / steps>0.5', f'{w[0]} / {w[1]:.2f} m / {w[2]} / {w[3]:.3f} m / {w[4]}')
    w2 = g.get('placew2')
    if w2: row('place writer: maxStep at frame rate / writes after >100 ms gap / of them >0.5 m', f'{w2[0]:.3f} m / {w2[1]} / {w2[2]}')
    for tag, s in (('host', h), ('guest', g)):
        if s['fi']: row(f'{tag} fight intents', s['fi'].replace('[FI] stats ', ''))
        if s['od']: row(f'{tag} owner damage', s['od'].replace('[OD] stats ', ''))
        row(f'{tag} still running', 'yes' if s['alive'] else 'NO (crash or exit)', 'yes', s['alive'])
    w = max(len(r[0]) for r in rows)
    for r in rows:
        print(f'{r[0]:<{w}}  {r[1]:<45} {r[2]:<10} {r[3]}')
    return rows


if __name__ == '__main__':
    game, hp, gp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    h, g = side(game, hp), side(game, gp)
    rows = report(h, g)
    if '--json' in sys.argv:
        with open(sys.argv[sys.argv.index('--json') + 1], 'w') as f:
            json.dump({'thresh': THRESH, 'rows': rows}, f, indent=1)

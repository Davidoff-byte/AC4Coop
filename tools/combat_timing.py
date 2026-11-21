#!/usr/bin/env python3
"""combat_timing.py - score a [Coop] CombatTimingProbe run (docs/evidence/combat-timing-2026-11-17.md).

    py -3.14 tools/combat_timing.py <host phase1_log> <guest phase1_log> [--from-us T] [--to-us T] [--attacks]

Both logs must come from one same-PC session: the [CTP] lines carry QueryPerformanceCounter microseconds, which the two
copies share. Prints the frame, lane and per-attack numbers the PASS bar is written against, then PASS/FAIL per bar.
Reads only; the bar (B1-B4) is fixed here and in the evidence file, not on the command line.
"""
import re, sys, statistics

BAR_SKEW_MS = 100.0      # B1: |hit latency - animation latency| p95
BAR_ALIAS_PCT = 95.0     # B1: copy plays the host's attack action at the hit
BAR_LEAD_MS = 100.0      # B2: |guest warning lead - host warning lead| p95
BAR_FRAME_P95_US = 25000 # B3: guest frame interval p95 in fight windows
BAR_LANE_P95_MS = 100.0  # B4: host capture -> guest apply p95

KV = re.compile(r'(\w+)=(-?[0-9A-Fa-f.]+)')

def parse(path):
    out = []
    with open(path, encoding='latin-1') as f:
        for line in f:
            if not line.startswith('[CTP] '): continue
            p = line.split()
            if len(p) < 3 or p[1] not in ('H', 'G'): continue
            d = {'role': p[1], 'kind': p[2], 'raw': line.rstrip()}
            for seg_i, seg in enumerate(line.split('|')):
                for k, v in KV.findall(seg):
                    key = k if seg_i == 0 or k not in d else f'{k}{seg_i}'
                    d[key] = v
            out.append(d)
    return out

def num(d, k, default=None, base=10):
    v = d.get(k)
    if v is None: return default
    try: return int(v, base) if base == 16 else float(v)
    except ValueError: return default

def pct(v, p):
    if not v: return float('nan')
    s = sorted(v); i = min(len(s) - 1, int(len(s) * p / 100))
    return s[i]

def stats(v, unit='ms'):
    if not v: return 'n=0'
    return f'n={len(v)} p50={pct(v,50):.0f} p95={pct(v,95):.0f} max={max(v):.0f} {unit}'

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2: print(__doc__); return 2
    opt = sys.argv[1:]
    t0 = float(opt[opt.index('--from-us') + 1]) if '--from-us' in opt else 0
    t1 = float(opt[opt.index('--to-us') + 1]) if '--to-us' in opt else 1e30
    H = [d for d in parse(args[0]) if t0 <= num(d, 't', 0) <= t1]
    G = [d for d in parse(args[1]) if t0 <= num(d, 't', 0) <= t1]
    if not any(d['role'] == 'H' for d in H) or not any(d['role'] == 'G' for d in G):
        print('no [CTP] H lines in the first log or no [CTP] G lines in the second (is CombatTimingProbe=1? host first?)'); return 2
    kind = lambda L, k: [d for d in L if d['kind'] == k]

    # ---- attack events ------------------------------------------------------------------------------------------
    hfde = kind(H, 'fde'); hhit = [d for d in hfde if d.get('ph') == '1' and d.get('type') != '35']
    htx = {int(num(d, 'seq')): d for d in kind(H, 'tx')}
    hpdr = {int(num(d, 'n')): d for d in kind(H, 'pdr')}
    hanim = kind(H, 'anim'); gbody = kind(G, 'body'); grx = kind(G, 'rx'); gact = kind(G, 'act')
    ghits = [d for d in kind(G, 'atk') if d.get('ph') == '1'] + kind(G, 'pdr')
    fight_t = sorted(num(d, 't') for d in hfde + kind(G, 'atk') + kind(G, 'pdr'))

    def host_hit_for(g):
        """The host's H fde hit line this guest hit came from (through seq / hit number)."""
        n = int(num(g, 'n', -1))
        tx = htx.get(n) if g['kind'] == 'atk' else hpdr.get(n)
        if not tx: return None, None
        cands = [h for h in hhit if h.get('id') == tx.get('id') and 0 <= num(tx, 't') - num(h, 't') < 250000]
        if g['kind'] == 'pdr':   # the relay line carries no guard id check on purpose: nearest hit before the publish
            cands = [h for h in hfde if h.get('ph') == '1' and 0 <= num(tx, 't') - num(h, 't') < 250000]
        return (max(cands, key=lambda h: num(h, 't')) if cands else None), tx

    rows = []; unmatched = 0
    for g in sorted(ghits, key=lambda d: num(d, 't')):
        h, tx = host_hit_for(g)
        if not h: unmatched += 1; continue
        tH, tG = num(h, 't'), num(g, 't')
        alias = h.get('alias'); gid = h.get('id')
        tHanim = tH - num(h, 'aliasAgeMs', 0) * 1000
        # when did the guest's copy start that action? (its own update saw the switch)
        gb = [b for b in gbody if b.get('id') == gid and b.get('alias') == alias and tHanim - 100000 <= num(b, 't') <= tH + 1500000]
        rx = [b for b in grx if b.get('id') == gid and b.get('alias') == alias and tHanim - 100000 <= num(b, 't') <= tH + 1500000]
        ac = [b for b in gact if b.get('id') == gid and b.get('alias') == alias and tHanim - 100000 <= num(b, 't') <= tH + 1500000]
        row = {
            'tH': tH, 'id': gid, 'alias': alias, 'mode': g['kind'],
            'hitLat': (tG - tH) / 1000.0,
            'txLat': (num(tx, 't') - tH) / 1000.0,
            'animLat': (num(gb[0], 't') - tHanim) / 1000.0 if gb else None,
            'rxLat': (num(rx[0], 't') - tHanim) / 1000.0 if rx else None,
            'actLat': (num(ac[0], 't') - tHanim) / 1000.0 if ac else None,
            'hostLead': num(h, 'firstWarnAgeMs', 0), 'hostWarns': num(h, 'warns', 0),
            'guestLead': num(g, 'firstWarnAgeMs', 0), 'guestWarns': num(g, 'warns', 0),
            'aliasAtHit': g.get('alias') == alias, 'guestAlias': g.get('alias'),
            'hostRatio': num(h, 'ratio'), 'guestRatio': num(g, 'ratio'),
            'hostAnimAge': num(h, 'aliasAgeMs', 0), 'dHost': num(h, 'd'), 'dGuest': num(g, 'd'),
            'posted': g.get('posted'), 'copy': g.get('copy'),
        }
        row['skew'] = row['hitLat'] - row['animLat'] if row['animLat'] is not None else None
        rows.append(row)

    # ---- frames -------------------------------------------------------------------------------------------------
    def frame_rows(L):
        fr = []
        for d in kind(L, 'frame'):
            t = num(d, 't'); fight = any(t - 5000000 <= x <= t for x in fight_t)
            fr.append((t, fight, num(d, 'p50', 0), num(d, 'p95', 0), num(d, 'p99', 0), num(d, 'max', 0), d))
        return fr
    def frame_line(name, fr):
        for label, sel in (('fight windows', [f for f in fr if f[1]]), ('other windows', [f for f in fr if not f[1]])):
            if not sel: print(f'  {name} {label}: none'); continue
            fg = [re.search(r'foreground=(\d+)/(\d+)', f[6]['raw']) for f in sel]
            fgs = sum(int(m.group(1)) for m in fg if m); tot = sum(int(m.group(2)) for m in fg if m)
            hz = sorted({re.search(r'displayHz=(-?\d+) remote=(\d+)', f[6]['raw']).group(0) for f in sel})
            print(f'  {name} {label}: windows={len(sel)} frame interval p50 median={statistics.median(f[2] for f in sel)/1000:.1f} ms, '
                  f'p95 median={statistics.median(f[3] for f in sel)/1000:.1f} ms, worst p95={max(f[3] for f in sel)/1000:.1f} ms, '
                  f'worst max={max(f[5] for f in sel)/1000:.1f} ms | foreground {fgs}/{tot} | {" ".join(hz)}')
    hf, gf = frame_rows(H), frame_rows(G)
    print('== frames (5 s windows; a fight window has an attack event in it)')
    frame_line('host ', hf); frame_line('guest', gf)

    # ---- lanes --------------------------------------------------------------------------------------------------
    print('== entity lane')
    for d in kind(H, 'lane')[-3:]: print('  ' + d['raw'][6:])
    lat95 = []
    for d in kind(G, 'lane'):
        m = re.search(r'capture->apply ms n=(\d+) mean=(\d+) p50=(\d+) p95=(\d+) p99=(\d+) max=(\d+)', d['raw'])
        if m and any(num(d, 't') - 5000000 <= x <= num(d, 't') for x in fight_t): lat95.append((int(m.group(3)), int(m.group(4)), int(m.group(6))))
    iv = []
    for d in kind(G, 'lane'):
        m = re.search(r'apply interval us n=(\d+) mean=(\d+) p50=(\d+) p95=(\d+)', d['raw'])
        if m and any(num(d, 't') - 5000000 <= x <= num(d, 't') for x in fight_t): iv.append((int(m.group(3)), int(m.group(4))))
    if lat95: print(f'  guest capture->apply in fight windows: p50 median={statistics.median(x[0] for x in lat95):.0f} ms, p95 median={statistics.median(x[1] for x in lat95):.0f} ms, max={max(x[2] for x in lat95)} ms')
    if iv: print(f'  guest snapshot apply interval in fight windows: p50 median={statistics.median(x[0] for x in iv)/1000:.0f} ms, p95 median={statistics.median(x[1] for x in iv)/1000:.0f} ms')

    # ---- attacks ------------------------------------------------------------------------------------------------
    print(f'== attacks: host hit posts={len(hhit)} guest hit events={len(ghits)} joined={len(rows)} unjoined={unmatched}')
    if '--attacks' in opt:
        for r in rows:
            f = lambda x: 'none' if x is None else f'{x:.0f}'
            print(f"  t={r['tH']/1e6:.2f}s {r['mode']} id={r['id'][-6:]} alias={r['alias']} hostAnimAge={r['hostAnimAge']:.0f} hitLat={r['hitLat']:.0f} "
                  f"rxLat={f(r['rxLat'])} actLat={f(r['actLat'])} animLat={f(r['animLat'])} skew={f(r['skew'])} | lead host={r['hostLead']:.0f} guest={r['guestLead']:.0f} "
                  f"| atHit copyAlias={r['guestAlias']} ratio host={r['hostRatio']} guest={r['guestRatio']} | d host={r['dHost']} guest={r['dGuest']} posted={r['posted']}")
    sk = [abs(r['skew']) for r in rows if r['skew'] is not None]
    sks = [r['skew'] for r in rows if r['skew'] is not None]
    played = sum(1 for r in rows if r['animLat'] is not None)
    at_hit = sum(1 for r in rows if r['aliasAtHit'])
    lead = [abs(r['guestLead'] - r['hostLead']) for r in rows if r['mode'] == 'atk' and r['hostWarns'] > 0]
    nolead = sum(1 for r in rows if r['mode'] == 'atk' and r['hostWarns'] > 0 and r['guestWarns'] == 0)
    print('  hit latency host post -> guest post: ' + stats([r['hitLat'] for r in rows]))
    print('  host post -> host publish:           ' + stats([r['txLat'] for r in rows]))
    print('  action latency host start -> wire rx on guest:   ' + stats([r['rxLat'] for r in rows if r['rxLat'] is not None]))
    print('  action latency host start -> copy plays it:      ' + stats([r['animLat'] for r in rows if r['animLat'] is not None]))
    print(f'  copy started the host\'s attack action within 1.5 s of the hit: {played}/{len(rows)}; copy is in it at the hit: {at_hit}/{len(rows)}')
    print('  skew (hit latency - action latency), signed: ' + stats(sks) + (f' min={min(sks):.0f}' if sks else ''))
    print('  |skew|: ' + stats(sk))
    print('  host warning lead (first warning -> hit): ' + stats([r['hostLead'] for r in rows if r['hostWarns'] > 0]))
    print('  guest warning lead:                       ' + stats([r['guestLead'] for r in rows if r['mode'] == 'atk' and r['guestWarns'] > 0]))
    print(f'  |guest lead - host lead|: ' + stats(lead) + f' | hits whose warning never reached the guest: {nolead}')
    dd = [abs(r['dGuest'] - r['dHost']) for r in rows if r['dGuest'] is not None and r['dHost'] is not None and r['dGuest'] >= 0 and r['dHost'] >= 0]
    print('  |guard-victim distance guest - host| at the hit: ' + stats(dd, 'm x1') if dd else '  distance: n=0')

    # ---- verdicts -----------------------------------------------------------------------------------------------
    print('== bar')
    def verdict(name, ok, detail): print(f'  {name}: {"PASS" if ok else "FAIL"} ({detail})'); return ok
    n = len(rows)
    if n < 10: print(f'  only {n} joined hits: below the 10 needed for a verdict (INCONCLUSIVE)')
    b1 = n >= 10 and bool(sk) and pct(sk, 95) <= BAR_SKEW_MS and 100.0 * at_hit / n >= BAR_ALIAS_PCT
    verdict('B1 hit vs animation', b1, f'|skew| p95={pct(sk,95):.0f} ms (bar {BAR_SKEW_MS:.0f}); copy in the attack action at the hit {100.0*at_hit/max(n,1):.0f}% (bar {BAR_ALIAS_PCT:.0f}%)')
    if any(r['mode'] == 'atk' for r in rows):
        verdict('B2 warning lead', bool(lead) and pct(lead, 95) <= BAR_LEAD_MS and nolead == 0, f'|lead diff| p95={pct(lead,95):.0f} ms (bar {BAR_LEAD_MS:.0f}); missing warnings {nolead}')
    else: print('  B2 warning lead: n/a (no forwarded warnings in this mode: PlayerDamageRelay sends hits only)')
    gfw = [f for f in gf if f[1]]
    verdict('B3 guest frame time', bool(gfw) and statistics.median(f[3] for f in gfw) < BAR_FRAME_P95_US, f'p95 median={statistics.median(f[3] for f in gfw)/1000:.1f} ms (bar {BAR_FRAME_P95_US/1000:.0f})' if gfw else 'no fight windows')
    verdict('B4 entity lane latency', bool(lat95) and statistics.median(x[1] for x in lat95) <= BAR_LANE_P95_MS, f'capture->apply p95 median={statistics.median(x[1] for x in lat95):.0f} ms (bar {BAR_LANE_P95_MS:.0f})' if lat95 else 'no data')
    return 0

if __name__ == '__main__':
    sys.exit(main())

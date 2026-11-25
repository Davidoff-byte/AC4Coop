"""spawn_key_diff.py - compare [KD] spawn-request dumps of two game copies (spawn_census.cpp, 2026-11-25).
Produced: docs/evidence/spawn-manager-2026-11-25.md section 5 (which fields are equal across copies and unique).
usage: py -3.14 tools/oneoff/spawn_key_diff.py <phase1_log_A> <phase1_log_B>"""
import re, sys, collections
def load(path):
    parts = collections.defaultdict(dict)
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\[KD\+\] n=(\d+) p=(\d+) (.*)$', line.rstrip('\n'))
        if m: parts[int(m[1])][int(m[2])] = m[3]
    recs = {}
    for n, ps in parts.items():
        s = ''.join(ps[i] for i in sorted(ps))
        h = re.search(r'ivt=(\w+) owner=\w+ ovt=(\w+) rq=(\w+)', s)
        I = s.split('| I:')[1].split('| R:')[0].split()
        def seg(tag):
            if tag not in s: return []
            rest = s.split(tag)[1]
            return re.split(r'\| [A-Z]:', rest)[0].split()
        R = seg('| R:')
        recs[n] = dict(ivt=h[1], ovt=h[2], rq=h[3] != '00000000', I=I, R=R, X=seg('| X:'), S=seg('| S:'), P=seg('| P:'))
    return recs
A, B = load(sys.argv[1]), load(sys.argv[2])
print(f"A {len(A)} recs, B {len(B)} recs")
same_order = sum(1 for n in A if n in B and A[n]['ivt'] == B[n]['ivt'] and A[n]['ovt'] == B[n]['ovt'])
print(f"same (ivt,ovt) at the same n: {same_order}/{min(len(A),len(B))}")
def val(tok):   # value part; pointers are 'v@vt'
    return tok.split('@')[0], ('@' in tok)
import struct
def flt(h):
    try: return struct.unpack('<f', int(h, 16).to_bytes(4, 'little'))[0]
    except Exception: return float('nan')
for part in ('P', 'X', 'S', 'R'):
    width = max(len(r[part]) for r in A.values())
    print(f"\n== {part}: per word, matched by n (only coordinator requests for R) ==")
    for i in range(width):
        eq = tot = ptr = 0; va = []
        for n in A:
            if n not in B: continue
            if part == 'R' and not (A[n]['rq'] and B[n]['rq']): continue
            if part == 'P' and not (A[n]['P'] and B[n]['P']): continue
            if i >= len(A[n][part]) or i >= len(B[n][part]): continue
            a, ap = val(A[n][part][i]); b, bp = val(B[n][part][i])
            tot += 1; eq += a == b; ptr += ap; va.append(a)
        if not tot: continue
        card = len(set(va))
        flag = '  <== key candidate' if eq / tot >= 0.9 and card >= 0.5 * tot and ptr < tot * 0.5 and card > 1 else ''
        ex = va[0] if va else ''
        fl = flt(ex) if ex else 0
        fs = f' ({fl:.2f})' if ex and 1e-3 < abs(fl) < 1e5 else ''
        print(f"  +0x{4*i:03X}  equal {eq:3}/{tot:3}  distinct {card:3}  ptr {ptr:3}  e.g. {ex}{fs}{flag}")



# One-off: guard action latency host -> guest from two [Coop] CombatTimingProbe logs of one same-PC session.
#   py -3.14 tools/oneoff/action_latency.py <host phase1_log> <guest phase1_log>
# Joins every `H anim` (a host guard changed its action, own update) with the first `G rx` (an applied snapshot names
# it), `G act` (SetTargetAction issued on the copy) and `G body` (the copy plays it) of the same guard id and alias
# within 3 s. Produced the action-latency tables of docs/evidence/combat-timing-2026-11-17.md (runs 1-3).
import re,sys,bisect,collections
KV=re.compile(r'(\w+)=(-?[0-9A-Fa-f.]+)')
def parse(p,kinds):
    out=collections.defaultdict(list)
    for l in open(p,encoding='latin-1'):
        if not l.startswith('[CTP] '): continue
        q=l.split()
        if len(q)<3 or q[2] not in kinds: continue
        d=dict(KV.findall(l.split('|')[0])); out[q[2]].append((int(d['t']),d['id'],d['alias'],d))
    return out
H=parse(sys.argv[1],{'anim'}); G=parse(sys.argv[2],{'rx','act','body'})
t0=max(min(x[0] for x in H['anim']),min(x[0] for x in G['rx']))+2_000_000
t1=min(max(x[0] for x in H['anim']),max(x[0] for x in G['rx']))-3_000_000
idx={k:collections.defaultdict(list) for k in G}
for k in G:
    for t,i,a,d in G[k]: idx[k][(i,a)].append(t)
gids={i for t,i,a,d in G['rx']}
def pct(v,p): s=sorted(v); return s[min(len(s)-1,int(len(s)*p/100))]
res={k:[] for k in G}; miss={k:0 for k in G}; n=0; dur=[]
hs=sorted(H['anim']); byid=collections.defaultdict(list)
for t,i,a,d in hs: byid[i].append((t,a))
for i,L in byid.items():
    if i not in gids: continue
    for j,(t,a) in enumerate(L):
        if not (t0<=t<=t1): continue
        n+=1
        hold=(L[j+1][0]-t)/1000 if j+1<len(L) else 99999
        dur.append(hold)
        for k in G:
            ts=idx[k].get((i,a),[]); p=bisect.bisect_left(ts,t-20000)
            if p<len(ts) and ts[p]-t<3_000_000: res[k].append(((ts[p]-t)/1000,hold))
            else: miss[k]+=1
print(f'host action switches of guards the guest mirrors: {n} over {(t1-t0)/1e6:.0f} s')
for k,name in (('rx','wire rx (snapshot applied names it)'),('act','SetTargetAction issued on the copy'),('body','copy plays it (own update)')):
    v=[x[0] for x in res[k]]
    if v: print(f'  {name}: reached {len(v)}/{n} ({100*len(v)/n:.0f}%), latency ms p50={pct(v,50):.0f} p95={pct(v,95):.0f} max={max(v):.0f}; never={miss[k]}')
    else: print(f'  {name}: 0/{n}')
for lo,hi in ((0,150),(150,300),(300,600),(600,1500),(1500,1e9)):
    sel=[h for h in dur if lo<=h<hi]; ok=[x for x in res['body'] if lo<=x[1]<hi]
    if sel: print(f'  host held the action {lo}-{hi if hi<1e9 else "inf"} ms: {len(sel)} switches, copy played {len(ok)} ({100*len(ok)/len(sel):.0f}%)' + (f', latency p50={pct([x[0] for x in ok],50):.0f} p95={pct([x[0] for x in ok],95):.0f} ms' if ok else ''))

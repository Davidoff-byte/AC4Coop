import os
D = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ref\AC4Coop\decomp\evidence"
os.makedirs(D, exist_ok=True)
HDR = """{name} @ 0x{addr}  (size {size})   -- probe receipt, no cl 16.00.40219.01 on this machine
measured 2026-11-08 by ORCH against the pinned binary
  AC4BFSP.exe sha256 bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150
  candidate body: ac4-decomp\\{addr}.c (submitted verbatim)

claim
{claim}

cross-check / limit of this receipt
{cross}
"""
items = {
 "17114C0": dict(name="EnemyOf", size=64, claim="""\
    EnemyOf(a, b) = faction(a) && faction(b) && contains(faction(b), faction(a)+0x14).
    That is: the target's faction is in the source's ENEMY list. Arguments are
    entity pointers (each goes through the faction resolver sub_133A4C0).""",
   cross="""\
    the +0x14 offset is fixed by the body; the list membership test is
    sub_19017C0. The decompiled signature is __cdecl (int,int), so a
    bool(__cdecl*)(void*,void*) cast is safe by signature - the ABI job checks
    the epilogue independently."""),
 "1711500": dict(name="AllyOrSame", size=68, claim="""\
    AllyOrSame(a, b) = same faction pointer, or faction(b) is in faction(a)+0x0C
    (the ALLY list).""",
   cross="""\
    paired with EnemyOf this fixes both relation-array offsets (+0x0C ally,
    +0x14 enemy) from two independent bodies rather than from a note."""),
 "1719FF0": dict(name="IsEnemyOfOwner", size=72, claim="""\
    IsEnemyOfOwner(e) = EnemyOf(e, ownerOf(e)), where ownerOf = sub_6F6010(e).
    One argument; returns false when either faction is null.""",
   cross="""\
    polarity is proven by the debug handler sub_13C1040, which kills when this
    returns true for "Nuke Enemies" and false for "Nuke Allies". This is the PC
    equivalent of the PS3 statechart name the plan was chasing; the name itself
    does not survive on PC (0 string hits)."""),
 "171A040": dict(name="IsAllyOfOwner", size=76, claim="""\
    IsAllyOfOwner(e) = AllyOrSame(e, ownerOf(e)). Same shape as 1719FF0 with the
    ally list.""",
   cross="""\
    the pair 1719FF0/171A040 is the usable player-relative hostility test a
    co-op filter needs; both take a single entity pointer."""),
 "133A4C0": dict(name="ResolveFaction", size=125, claim="""\
    ResolveFaction(entity): if the entity's AI pointer (sub_6F5F70) exists and
    sub_F916A0 accepts it, return ai[41] (+0xA4). Otherwise take the
    FactionSelector path (sub_11DDD60) and return the masked pointer
    (ptr & (refcount >> 31), the same >>31 idiom as sub_1C6FA90), releasing the
    temporary reference it took.""",
   cross="""\
    this is the entity -> faction-object step every relation test starts with.
    The fallback's ownership rules (when the temporary ref is released) are the
    part a caller must not disturb; calling it from a filter is read-only."""),
}
for addr, it in items.items():
    p = os.path.join(D, addr + ".txt")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR.format(addr=addr, **it))
    print(p, os.path.getsize(p), "B")

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
 "528200": dict(name="SetGlobalMatrixInternal", size=139, claim="""\
    Applies a matrix through sub_63C2D0(a2, a3), marks the object dirty with
    0x20000 at +0x50, kicks sub_520F80(this+0xE1) -> sub_423470, calls the
    virtual at vtbl+0x80(this, a3), and when a3 is non-zero walks the child list
    (sub_5269A0/sub_526A60 over this+0xDB) calling vtbl+0xBC(child, this+0x10).""",
   cross="""\
    the +0x50 flag write is the same 'dirty' word the project's transform probes
    watch, and the a3 boolean is the updatePhysics argument the SPAWN path passes
    as 1. The receipt explains why a teleport needs the boolean: the child
    notification only runs when it is set."""),
 "70E810": dict(name="WorldGetPlayer", size=14, claim="""\
    GetPlayer(world) = sub_736540(*(*(world+0x7F8)+4)). One load chain, no
    null checks of its own.""",
   cross="""\
    fixes the exact path (world+0x7F8 -> +4 -> sub_736540) so a future probe can
    validate it with three reads instead of a scan. sub_736540 is not decompiled
    here, so what it does with the value stays open."""),
 "70E860": dict(name="WorldAddEntity", size=27, claim="""\
    AddEntity(world, e) = sub_5A8A80(world, e, 0xFFFF, 0).""",
   cross="""\
    the same sub_5A8A80 the domino spawn op calls with (0xFFFF, 1); the trailing
    flag is 0 on the plain add and 1 on the domino route. That one-bit difference
    is the only distinction the bodies show."""),
 "733FB0": dict(name="NullReturnSubject", size=50, claim="""\
    Two-state dispatcher keyed on the byte at +0xD4: 4 returns the object itself;
    5 takes *(this+0x230), returns sub_E10E50(that), and if that is zero returns
    sub_4956E0(that); any other state returns 0.""",
   cross="""\
    this is the function the NULL-return observer detours (HOOK-TABLE #21), so the
    receipt fixes what a NULL result means: either state != 4/5, or state 5 with
    sub_E10E50 returning 0 and sub_4956E0 also returning 0. The observer's
    caller-RA logging therefore covers three distinct causes, not one."""),
 "527BA0": dict(name="EntityBuilderInstantiate", size=117, claim="""\
    Instantiate: sub_A2D5A0(0,1), vtbl+0xC(this,0,0) -> instance, sub_A2F320(),
    store to *a2. Sets instance flags 4 | 8 | 0x800000 at +0x50 (preserving
    +0x54), reads scale from instance[0x17] -> +0x728 float (default 1.0), and
    finishes with sub_522610(instance, 0, scale*scale).""",
   cross="""\
    the (0,0) arguments are the Instantiate contract noted in 5FD520's receipt;
    the flag triplet is what makes a newly built entity visible, and the scale is
    squared before the final call, which is the classic uniform-scale square."""),
 "106ABF0": dict(name="DominoSpawnOp", size=142, claim="""\
    The domino spawn op: requires *(this+0x32C) handle and a4; instantiate via
    sub_527BA0(a4), place with sub_431490(instance, a3), enqueue with
    sub_5A8A80(world, instance, 0xFFFF, 1), resolve the component with
    sub_5200C0(instance, sub_15C92E0()) and attach with sub_105AE30 - except when
    the caller's handle is the &dword_4DD5F8C singleton, where attach is skipped.""",
   cross="""\
    this is the exact chain WP8/AMBIENT call the 'working' spawn, and the body
    confirms the singleton guard at the end: an unregistered caller gets an
    instance with no component attached. That is the documented 'spawns but is
    inert' behaviour, not a failure."""),
 "6608A0": dict(name="StrongHandleRegister", size=246, claim="""\
    Registers an object in the strong-handle table: only when the object's
    +0x880 word is <= 1; takes its handle, calls sub_15DFF10, drops the temporary
    refcount, and (when sub_15DFF10 returned 0) re-takes the handle and calls
    sub_1401BB0; then notifies every entry of the list at this+8 (u16 count +0xE)
    through vtbl+0x54.""",
   cross="""\
    the +0x880 gate matches the 'observer phase gate' the project already tracks;
    the list here is this+8, while the unregister below walks this+0x18 - two
    different arrays, which is why register and unregister are not symmetric on
    the same offset."""),
 "6609A0": dict(name="StrongHandleUnregister", size=173, claim="""\
    Removes an object from the handle array at this+0x18 (u16 count at +0x1E):
    takes its handle, finds the slot by pointer equality, drops the refcount,
    calls sub_11B44C0(index), then notifies the this+8 list entries through
    vtbl+0x58.""",
   cross="""\
    paired with 6608A0 this fixes both notify slots (+0x54 on add, +0x58 on
    remove) and the two distinct arrays. A caller that only implements add will
    leak the handle, because removal is a separate list."""),
}
for addr, it in items.items():
    p = os.path.join(D, addr + ".txt")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR.format(addr=addr, **it))
    print(p, os.path.getsize(p), "B")

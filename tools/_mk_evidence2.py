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
 "1371140": dict(name="GetService_CSrvNPCHealth", size=80, claim="""\
    Service lookup by class CRC. Returns a cached pointer from this+0x58 if set,
    else walks the component array (this[3] + 0x70, u16 count at +0x76) and
    compares sub_A1D0D0(entry) against 1475276487. Null when not found.""",
   cross="""\
    the 0x70/+0x76 pair is the same component container the project already
    models (Entity+0x60/+0x64/+0x66 family); the CRC is the only class id used,
    so the receipt fixes WHICH service this is, not the container layout."""),
 "17106C0": dict(name="DamageFilter", size=104, claim="""\
    Predicate + dispatch on a damage event. Reads the packed word at target+0x9C
    and requires (w & 7) == 2 and (w & 0x1F8) == 152, i.e. role 19 = Animal.
    Then switches on event kinds ([a3+0x20] in {3,6,14} and [a3+0x10] in
    {5,14,23,25,26}) and forwards to the death state-machine sub_1A90920.""",
   cross="""\
    152 = 19 << 3, and 19 is EntityDescriptorNPC_Animal in the PS3 ELF enum
    (DIE 0x2BAEC3) - the packing matches IsSoldier/IsCivilian exactly. So this
    filter is the ANIMAL damage path, not a generic one; code that reuses it for
    human NPCs would be filtering on the wrong role."""),
 "18B9EA0": dict(name="DamageAmountCompute", size=56, claim="""\
    Resolves a per-attack damage function: indexes dword_4E20544 by byte +0x35,
    takes the virtual at (*(this+0x20) -> vtbl+0x30) + 4*index and calls it with
    a2; returns 3 when any link is null.""",
   cross="""\
    confirms COMBAT-FIELDS' reading that DAT_04e20544[+0x35] selects the damage
    function, and that 3 is the default/guard return. It does NOT give the table's
    row layout - that stays unverified and must not be inferred from this body."""),
 "1A90920": dict(name="DeathStateMachineTick", size=123, claim="""\
    Drives a small state machine on a stack context: returns 3 when the subject
    pointer is null; otherwise sub_A00CB0, then loops sub_9F6260 while the last
    handler return is 2, calling v4[0x30](v4[0x28]) each pass, and finally
    sub_A01070. Return 2 means 'continue', 3 means 'stop'.""",
   cross="""\
    the 2/3 return contract is what the damage filter above relies on; both bodies
    are now receipts, so a future change that treats 2 as terminal is refutable."""),
 "66F630": dict(name="ObserverListInsert", size=246, claim="""\
    Appends an observer to the list at this+8 (u16 count at +0xE, capacity
    (this+0xC)&0x3FFF, growth via sub_A09AB0 with a 4-byte element), refcounts it,
    then notifies every existing entry through vtbl+0x54 and finally checks
    sub_45BED0 before calling vtbl+0x48.""",
   cross="""\
    this is the only insertion into Universe+0x08 the project found (WP8). The
    receipt proves the list mechanics and the two notification slots; it does not
    prove that registering a mod-created object is safe, which still needs a probe."""),
 "657BE0": dict(name="FrameObserverPhase1", size=47, claim="""\
    One of the five per-frame observer passes: walks the same list (this+8, u16
    count at +0xE) and calls vtbl+0x3C on every non-null entry.""",
   cross="""\
    phase 1 of 5 (siblings 0x657C10/0x657C40/0x657C70/0x657CA0). The project's
    note that none of the five is AI/perception is consistent with this body: it
    dispatches blind, with no type test."""),
 "6F5F70": dict(name="GetEntityAI", size=29, claim="""\
    GetEntityAI(e) = e ? *(*(e+0xE8)+0x1C) : 0. Two dereferences and a null guard;
    no other work.""",
   cross="""\
    the 29 bytes settle the open WP5 question: if +0xE8 is cleared the result is
    null, so any death/AI write through this helper is silently skipped - it cannot
    'fall back' to something else."""),
 "9F0B50": dict(name="GetObjectID", size=16, claim="""\
    GetObjectID(this) = sub_A200D0(this, 0, 1), returning a 64-bit value. This
    function is a one-line wrapper; the identity logic lives in sub_A200D0.""",
   cross="""\
    this closes the STATUS-only claim that the id is 64-bit in EDX:EAX: the __int64
    return type is in the decompiled signature. It does NOT verify which of the
    four sub_A200D0 paths produced the value on a given call."""),
 "744CF0": dict(name="GetUnusedPadID", size=192, claim="""\
    Scans the pad-slot array for a free slot; on success writes the slot index to
    *a2 and returns 1, otherwise returns 0. The array snapshot comes from
    sub_A062A0/sub_A0A790 and is released through dword_2AC40F0's vtbl+0x14.""",
   cross="""\
    matches the live result already recorded (pad forced to an empty slot, player
    pad kept). The receipt adds the return contract: 0 means 'no free slot', and
    *a2 must be treated as untouched in that case."""),
 "79B8D0": dict(name="SpawningRequestSpawn", size=74, claim="""\
    Spawn entry: gated on the object's own virtual predicate (*this)(), sets
    this+0x24=1, +0x2C=0, +0x28=0, +0x20=1, calls sub_48A390(request), then
    vtbl+0xC(this) and finally vtbl+0x88(that, this). Returns 0 if the predicate
    refuses.""",
   cross="""\
    the prologue the hook-table patch expects (56 8B F1 8B 06 = push esi; mov esi,ecx;
    mov eax,[esi]) is consistent with the __thiscall body, but the NAME is still
    contested in the project (HOOK-TABLE says SpawningRequest::Spawn at RVA
    0x39B8D0; SPAWN-RETHINK reports spawnreq at 0x5A8700). This receipt proves the
    bytes and the body, not the name."""),
 "5FD520": dict(name="RowSelectorFactoryStage2", size=59, claim="""\
    Stage 2 of the RowSelector-driven factory: sub_5FAC60(this,a2), take the
    object's virtual at vtbl+0xC called with (0,0), then sub_526590(instance) and
    sub_A2E820(). Returns the instance or null.""",
   cross="""\
    pairs with row-selector stage bodies already in the project (5FAC00 / 79BC50).
    The (0,0) arguments to vtbl+0xC are the Instantiate contract; a caller that
    passes anything else is not calling this path."""),
 "A0BDE0": dict(name="HandleManagerFindOrCreate", size=108, claim="""\
    find-or-create over the handle map: with a non-zero (lo,hi) it locks
    this+0x7C, calls sub_A0BBF0(this,lo,hi), bumps the entry reference count and
    unlocks; with (0,0) it bumps dword_4DD5F90 and returns &dword_4DD5F8C, the
    shared empty handle.""",
   cross="""\
    the (0,0) branch explains the 'all-zero handle' the project sees for objects
    that were never registered: it is a real singleton, not a null pointer, so a
    test for == nullptr would miss it."""),
}
for addr, it in items.items():
    p = os.path.join(D, addr + ".txt")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR.format(addr=addr, **it))
    print(p, os.path.getsize(p), "B")

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
 "636480": dict(name="StateBitfieldSetter", size=421, claim="""\
    State/index setter on a small object: reads (*this & 0x3F) as the current
    state, writes bits 26/27/28 of *this according to which state is being
    entered, updates the low 6 bits, and notifies the object at *(this+2).""",
   cross="""\
    found by the cite gate: phase1.cpp calls it directly and it had no unit.
    The body is what the gate exists to force - the call had been made without a
    written claim about what the function does."""),
 "A1D0D0": dict(name="ComponentClassId", size=11, claim="""\
    __thiscall(void* component) -> int: calls the component vtable slot at +20
    and returns the dword at +20 of its result. That is the class id / CRC the
    health-service getters compare against (see ops/evidence/health-service-crcs.md).""",
   cross="""\
    the entire 13-getter health-service block keys on this value, so its ABI
    matters: __thiscall, one pointer, returns int in EAX. Verified from the body
    (`*(_DWORD *)this + 20` call, then `+ 20` load)."""),
 "10EF8C0": dict(name="GetComponentByCrc1588608236", size=72, claim="""\
    __cdecl(int entity) -> void*: get the AI pointer (sub_6F5F70), walk its
    component list at +0x5C (92) with the u16 count at +0x62 (98), and return the
    first component whose class id (sub_A1D0D0) equals 1588608236. Null when the
    AI pointer or the component is missing.""",
   cross="""\
    1588608236 is one of the 13 CRCs of the health-service block - it is the
    getter at 0x1370E20 (ops/evidence/health-service-crcs.md). So this helper is
    the entity -> service lookup for that type, which is exactly the shape the
    ship hull-HP hunt needs: find the CRC on a ship and this is how the service
    is reached."""),
 "133F550": dict(name="IsAliveByCrcService", size=70, claim="""\
    __cdecl(int entity) -> bool: false when the entity has no AI; otherwise false
    when sub_7025F0(entity) yields a byte at +184 whose bits 0xC equal 4; else
    look up the 1588608236 service via 0x10EF8C0 and return false when its dword
    [1] has bit 0x1000 set. True only when neither test fires.""",
   cross="""\
    this is the 'is this body alive' predicate combat_observe already watches at
    0x133F550 (its site 4). The receipt fixes the polarity: TRUE = alive, and the
    service's bit 0x1000 is the DEAD bit, matching the svc+0x60 dead-flag family
    used elsewhere."""),
}
for addr, it in items.items():
    p = os.path.join(D, addr + ".txt")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR.format(addr=addr, **it))
    print(p, os.path.getsize(p), "B")

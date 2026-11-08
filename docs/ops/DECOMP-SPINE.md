# The decompilation spine — how it works and where it stops

Built because every technical claim this project made about the binary was argued from decompiled
prose, and one of them (`kind == 3` = soldier) shipped and made enemy sync **100 % dead**. A spine
turns "I read it and it does X" into a receipt that a machine re-checks.

## The unit of work

```
decomp/
  units.json             one row per function: address, size, name, status, owner, receipts
  candidates/<ADDR>.cpp  the candidate source, stored with the receipt
  objects/<ADDR>.obj     the compiled object, when the byte arm was used
  evidence/<ADDR>.txt    behavioural evidence, when the probe arm was used
  receipts/<ADDR>.json   hashes of all of the above + the verdict
```

Status: `unknown → claimed → candidate → matched`. `matched` means **byte arm**, nothing else.

## Commands

```powershell
py tools\decomp.py init
py tools\decomp.py add    0x5200C0 51 GetComponentByType
py tools\decomp.py claim  0x5200C0 --owner seat-re        # refuses a second owner
py tools\decomp.py submit 0x5200C0 candidate.cpp [--obj cand.obj] [--evidence log.txt]
py tools\decomp.py check                                   # re-derives every receipt
py tools\decomp.py list [--status candidate]
```

## Two arms, and only one of them can say MATCH

**byte** — the candidate object's `.text` is compared with the original function's bytes, with the
bytes covered by the object's own relocations zeroed on both sides. Requires the object to be built
with the **pinned compiler**, `cl 16.00.40219.01` (see `DECOMPILATION.md`: the game is VC++ 2010 SP1,
measured from the PE Rich header). A clean, length-equal comparison is the only path to `MATCH`.

**probe** — no compiler on the machine. The receipt records the candidate's hash plus the hash of a
log excerpt, and the verdict is `UNVERIFIED-EVIDENCE`. It is deliberately *not* a weaker kind of
match; it simply is not a match, and `check` says so.

## The rule that makes it an oracle rather than paperwork

`check` never trusts a stored verdict. For every receipt it re-derives:

- the original bytes, from `AC4BFSP.exe`, hashed again;
- the candidate source, hashed again;
- the object, hashed again, then **recompiled comparison** for any `MATCH`;
- the evidence file, hashed again.

Any drift fails that receipt and the exit code is 1. Verified by trying to cheat it:

```
edit a receipt to say MATCH      -> FAIL 005200C0: receipt claims MATCH with no object to re-derive it
edit the candidate after submit  -> FAIL 005200C0: candidate source drifted
```

## Where it stops today

No `cl 16.00.40219.01`, so the byte arm cannot run and **no unit can reach `matched`**. Everything
else — index, ownership, receipts, tamper detection — works and is exercised above. The day that
compiler is installed, `--obj` starts producing real `MATCH` receipts and the corpus begins to mean
something.

Seeded units (the load-bearing functions, not the 128 k):

| address | size | what |
|---|---|---|
| `0x5200C0` | 51 | `Entity::GetComponentByType` |
| `0x504F60` | 464 | `Entity::AddComponent` (the `+0x66` count writer) |
| `0x133F630` | 297 | the friendly-fire / damage gate |

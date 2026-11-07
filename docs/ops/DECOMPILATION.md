# Matching decompilation — the method, adopted

Notes on the Fallout: New Vegas approach (`gavinbrow/Decompilation-MCP-Server`, "FNV fully
decompiled") and what of it applies to AC4, measured on our binaries.

## What the FNV method actually is

1. **Pin the original compiler.** Matching means *byte-identical output*, which is only achievable
   with the exact compiler build the game was built with. For FNV that is Microsoft VC++ 2008 SP1
   `15.00.30729.01`, x86 — not "a 2008 compiler", that specific build.
2. **One function = one unit of work.** An agent claims a function, reads the decompiled original,
   writes candidate C++, compiles it, and compares the emitted bytes with the original function.
3. **A checker decides, not an agent.** Only an official MATCH (byte equality, checked by a tool
   that did not write the candidate) may be submitted. A diagnostic score or "looks close" never
   authorises anything.
4. **Bounded search on small functions.** Pattern engines handle the trivial families (wrappers,
   getters, dispatch). Humans/agents take the rest.
5. **Durable coordination.** Ownership, leases, receipts, quarantine — because two agents editing one
   unit of work silently corrupts the corpus.

## Which compiler built AC4 — measured

`tools/rich_header.py` decodes the PE Rich header. On both our binaries
(`BC11AD79` 45,056,040 B and `54CA73AF` 45,123,288 B):

```
Utc1600_CPP   build 40219   x1138      <- dominant
Utc1600_C     build 40219   x730
Cvtres900     build 30729   x35        <- VS2008 SP1 objects, third-party libs
Utc1500_C/CPP build 30729   x47
Linker1000    build 40219   x1
```

**AC4 (PC 1.05) was compiled with Microsoft VC++ 2010 SP1, `16.00.40219.01`, x86**, linked with
`Linker1000` build 40219, plus some VS2008 SP1 objects from bundled libraries.

Consequence: matching decompilation is *possible* for this binary in the same sense as FNV — but only
with cl 16.00.40219.01 and the right flags. Without that exact compiler, byte equality cannot be the
gate, and the method degrades to "plausible C++".

## What that buys us immediately, without the compiler

Both AC4 builds came from the *same* toolchain. Therefore any function whose source did not change
compiles to **identical bytes** in both, and any function that differs genuinely changed. That is a
free, exact oracle for the port question.

`tools/fn_identity.py` measures it over the 101 relocation sites:

```
sites 101   identical 15   changed 79   no-body 7
of the identical ones, unique in target: 7
```

The headline is misleading until the sites are classified: most "changed" entries are **data**, not
code — vtables (`0x1e41e58`, `0x26e34d8`, `0x2727088`), globals and IAT slots (`0x1a3a20c`). Those
must be relocated as data references, not matched as functions. Of the real code sites, only a
minority are byte-identical.

**Rule adopted:** classify each address as code or data *before* asking whether it moved. Treating a
vtable pointer as a function is exactly how the earlier 22/101 number got inflated.

## The gate this project adopts

Claims gain a status **MATCH**: the candidate compiles, with the pinned compiler and recorded flags,
to bytes identical to the original function (relocations normalised). Nothing weaker is called a
match. `MATCH` outranks `VERIFIED-BINARY` in `docs/ops/CLAIMS.md` because it cannot be argued with.

Until cl 16.00.40219.01 is available on this machine, MATCH is **not available** and no claim may
carry it. What we can do today is the cheap half: exact byte-identity between builds (done), and
`VERIFIED-BINARY` for everything else.

## What is deliberately not adopted

- **No account-bound service.** The FNV workflow is tied to `fnvdecomp.com` claims and an official
  checker; AC4 has no such service. Our equivalent is local: `docs/ops/CLAIMS.md` plus the repo's
  checker scripts.
- **No corpus of solved functions** — not distributed, and we have none yet.
- **No claim that this makes full decompilation tractable soon.** 128,610 functions, one gate each.

"""spine_gate.py - the hard gate for the decompilation spine.

Six rules, all machine-checked, exit 0 only when all hold:

  1. RECEIPTS RE-DERIVE.  Every receipt in decomp/receipts/ is re-checked by
     `decomp.py check` against the pinned binary. A drifted candidate, a missing
     evidence file or a hand-edited verdict fails.
  2. NO UNDOCUMENTED ENGINE CALL.  Every direct call into the game made from the
     plugin (`))0xADDR)(` cast-and-call form) must have a unit in
     decomp/units.json. If we call an engine function, it gets a receipt - that is
     the rule that keeps "I called it and it seemed fine" out of the codebase.
  3. NO UNDOCUMENTED ENGINE CALL, ADDRESS IN ARGUMENT POSITION.  The same rule for
     the two call shapes rule 2 is blind to:

       (a) `Hook::CallClassMethod<...>(0xADDR, ...)` - and CallCdeclMethod /
           CallStdMethod / CallFastCallMethod - where the callee address is the
           template helper's FIRST ARGUMENT. It never takes the `))0xADDR)(` shape,
           so rule 2 cannot see it: that is how ConstructGhostBody called
           0x00C4F2A0 and ResolveEntityByIdRaw called 0x00A1F160 with no unit.
           The argument list may wrap onto the next line, so the match spans lines.
       (b) `))0xADDR)(` with a NINE-hex-digit literal, which rule 2's {6,8} bound
           cannot match. A 9-digit literal is an engine address with a stray leading
           zero (`0x004DD9824` is `0x04DD9824`); the source should spell it with 8
           digits, but if one survives the gate must still catch the call.

     Addresses from every rule are normalised to 8 hex digits before lookup, so
     `0x9F0B50` and `0x009F0B50` are the same unit. A literal that is not a unit is
     reported, never skipped: the gate does not decide which literals "look like"
     engine addresses.

  4. NO UNDOCUMENTED ENGINE CALL, BASE-RELATIVE CALLEE.  `(base() + 0xRVA)(args)`
     spells the callee as an RVA added to the module base, so no rule above sees an
     absolute literal. The rule fires ONLY in callee position - the literal must be
     followed by the closing ')' of the cast and then '(' - which is what separates
     a call from a data access (`*(void**)(base() + 0x24B168C)` is not a call and
     is not gated here). The RVA is normalised with IMAGE_BASE (0x400000, the
     ImageBase in the pinned exe's PE header) and the resulting VA must have a unit.
     A site the rule cannot resolve (RVA overflowing 32 bits) is reported, not
     skipped.

  5. EXCEPTIONS ARE NAMED, NOT INVISIBLE.  Two escape hatches, both printed on every
     run and both machine-checked:

       EXCEPTIONS            a VA rule 4 resolves that is NOT a function entry. Each
                             entry names the real callee, and the gate fails unless
                             that callee has a unit. Current entry: 0x013B3220, the
                             result of `base() + 0x00FB3220`, which is mid-function
                             because 0x00FB3220 is already a VA (double base add).
       STORED_POINTER_ALLOWLIST  a call whose address reaches the call site through a
                             variable, e.g. `ShipSpawnReqFn fn = (ShipSpawnReqFn)
                             (base() + 0x00C82810); ... fn()`. Text cannot follow the
                             variable, so the site is named with its unit; the gate
                             fails if the unit disappears or the literal/variable is
                             no longer in the source (stale entries are errors).

  6. NO NINE-DIGIT LITERALS.  src/ must contain no `0x` literal of nine or more hex
     digits: that spelling means an engine VA with a stray leading zero
     (`0x004DD9824` for `0x04DD9824`) and has already hidden a call once.

Usage:
    py tools/spine_gate.py            # check, human output
    py tools/spine_gate.py --json     # machine output (used by the audit)
"""
import argparse, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                    # ref/AC4Coop
UNITS = os.path.join(ROOT, "decomp", "units.json")
SRC = os.path.join(ROOT, "src")

# ImageBase of the pinned AC4BFSP.exe, read from its PE optional header. `base()` in
# src/ is GetModuleHandle(NULL) - the same value (phase1.cpp:9189 says "an RVA; image
# base 0x4..." while computing data addresses the same way).
IMAGE_BASE = 0x400000

# `...cast...))0x19FB4C0)(args)`  -> an absolute call into the game image
CALL_RE = re.compile(r"\)\s*0x([0-9A-Fa-f]{6,8})\s*\)\s*\(")

# The same shape with nine hex digits: `))0x19FB4C0_)(args)`. CALL_RE's {6,8} bound
# cannot match these, and a 9-digit literal is a real engine address (`0x004DD9824`).
CALL9_RE = re.compile(r"\)\s*0x([0-9A-Fa-f]{9})\s*\)\s*\(")

# `Hook::CallClassMethod<void, void*>(0xADDR, ...)` - callee is the helper's
# first argument, optionally cast, and the argument list may wrap onto the next line.
# `[^;{}]*?` keeps the template-argument list one expression: template arguments
# cannot contain ';' or '{'.
CALL_HELPER_RE = re.compile(
    r"\bCall[A-Za-z0-9_]*Method\s*<[^;{}]*?>\s*\(\s*(?:\([^()]*\)\s*)?0x([0-9A-Fa-f]{6,9})\b")

# rule 4: `base() + 0xRVA`. Callee position is decided by the text after the literal:
# one or more ')' (cast close) then '(' (the call). A value use keeps no such tail.
BASE_REL_RE = re.compile(r"base\(\)\s*\+\s*0x([0-9A-Fa-f]{6,9})\b")
CALLEE_TAIL_RE = re.compile(r"\s*\)+\s*\(")

# rule 6: any 9+ hex-digit literal is a misspelt VA.
NINE_ANY_RE = re.compile(r"0x([0-9A-Fa-f]{9,})\b")

# rule 4 exceptions: VA -> the real callee + why the VA is not one. Machine-checked:
# `intended` must be a unit, and the entry is printed on every run.
EXCEPTIONS = {
    "013B3220": {
        "intended": "00FB3220",
        "reason": ("base() + 0x00FB3220, but 0x00FB3220 is already a VA (RVA 0xBB3220); "
                   "0x013B3220 is inside a function whose body ends 0x013B3277 `ret 8`, "
                   "so calling it enters mid-body. Source bug, logic left frozen; the "
                   "intended callee 0x00FB3220 has a unit."),
    },
}

# rule 5: address reaching the call through a variable. Each entry names the variable
# and the VA; the gate requires the unit AND the source literal/variable to still exist.
STORED_POINTER_ALLOWLIST = [
    {"va": "01082810", "variable": "ShipSpawnReqFn fn", "file": "phase1.cpp", "line": 7703,
     "literal": "0x00C82810",
     "reason": ("the VA is stored into a local function pointer and the call is `fn()`; "
                "no textual rule follows a variable, so the site is named here instead "
                "of being silently out of reach.")},
]


def load_units():
    with open(UNITS, encoding="utf-8") as f:
        d = json.load(f)
    return {k.upper().lstrip("0").zfill(8): v for k, v in d.get("units", {}).items()}


def norm(lit):
    return lit.upper().lstrip("0").zfill(8)


def _sources():
    for fn in sorted(os.listdir(SRC)):
        if fn.endswith((".cpp", ".h")):
            path = os.path.join(SRC, fn)
            try:
                yield fn, open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue


def scan_calls(rule):
    hits = {}
    for fn, txt in _sources():
        for m in rule.finditer(txt):
            hits.setdefault(norm(m.group(1)), []).append(fn)
    return hits


def scan_nine_digit():
    hits = []
    for fn, txt in _sources():
        for m in NINE_ANY_RE.finditer(txt):
            hits.append(f"{fn}:{txt.count(chr(10), 0, m.start()) + 1} 0x{m.group(1)}")
    return hits


def scan_base_relative():
    """Rule 4. Returns (callee, value, unresolved); sites are `file:line` strings."""
    callee, value, unresolved = {}, {}, []
    for fn, txt in _sources():
        for m in BASE_REL_RE.finditer(txt):
            where = f"{fn}:{txt.count(chr(10), 0, m.start()) + 1}"
            rva = int(m.group(1), 16)
            if IMAGE_BASE + rva > 0xFFFFFFFF:
                unresolved.append(f"{where} (base()+0x{m.group(1)} overflows 32 bits)")
                continue
            va = f"{IMAGE_BASE + rva:08X}"
            if CALLEE_TAIL_RE.match(txt, m.end()):
                callee.setdefault(norm(va), []).append(where)
            else:
                value.setdefault(norm(va), []).append(where)
    return callee, value, unresolved


def check_exceptions(units):
    """Every exception must point at a VA that is genuinely not a unit, and its real
    callee must have one. Returns (bad, ok-lines)."""
    bad, ok = [], []
    for va, exc in EXCEPTIONS.items():
        intended = norm(exc["intended"])
        if va in units:
            bad.append(f"EXCEPTION {va} is now a unit - drop the exception")
            continue
        if intended not in units:
            bad.append(f"EXCEPTION {va}: intended callee {intended} has NO unit")
            continue
        ok.append(f"EXCEPTION {va} -> intended unit {intended} present [BUG: {exc['reason']}]")
    return bad, ok


def check_allowlist(units):
    bad, ok = [], []
    for ent in STORED_POINTER_ALLOWLIST:
        va = norm(ent["va"])
        if va not in units:
            bad.append(f"ALLOW {va} ({ent['variable']}): NO unit")
            continue
        path = os.path.join(SRC, ent["file"])
        txt = open(path, encoding="utf-8", errors="replace").read() if os.path.exists(path) else ""
        if ent["literal"] not in txt or ent["variable"] not in txt:
            bad.append(f"ALLOW {va}: stale - `{ent['variable']}` / `{ent['literal']}` "
                       f"no longer in {ent['file']}")
            continue
        ok.append(f"ALLOW {va} via `{ent['variable']}` {ent['file']}:{ent['line']} "
                  f"-> unit present")
    return bad, ok


def run_receipts():
    p = subprocess.run([sys.executable, os.path.join(HERE, "decomp.py"), "check"],
                       capture_output=True, text=True, cwd=ROOT)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    units = load_units()
    calls = scan_calls(CALL_RE)          # rule 2: cast-and-call, 6-8 digits (unchanged)
    calls9 = scan_calls(CALL9_RE)        # rule 3(b): cast-and-call, 9 digits
    calls_t = scan_calls(CALL_HELPER_RE)  # rule 3(a): Call*Method<...>(0xADDR, ...)
    baserel, baserel_value, baserel_unres = scan_base_relative()   # rule 4
    nine = scan_nine_digit()             # rule 6

    # rule 3 = (a) and (b) merged; the same address found by both is one entry.
    arg_calls = {}
    for src in (calls9, calls_t):
        for addr, files in src.items():
            arg_calls.setdefault(addr, []).extend(files)

    missing = {addr: files for addr, files in calls.items() if addr not in units}
    missing_arg = {addr: files for addr, files in arg_calls.items() if addr not in units}
    missing_baserel = {addr: sites for addr, sites in baserel.items()
                       if addr not in units and addr not in EXCEPTIONS}
    exc_bad, exc_ok = check_exceptions(units)
    allow_bad, allow_ok = check_allowlist(units)

    rc, out = run_receipts()
    receipts_ok = (rc == 0)
    n_receipts = out.count("UNVERIFIED-EVIDENCE") + out.count("MATCH")

    ok = (receipts_ok and not missing and not missing_arg and not missing_baserel
          and not baserel_unres and not exc_bad and not allow_bad and not nine)

    report = {
        "receipts_ok": receipts_ok,
        "receipts_rc": rc,
        "receipts": n_receipts,
        "units": len(units),
        "engine_calls": len(calls),
        "calls_without_unit": len(missing),
        "missing": {k: sorted(set(v)) for k, v in sorted(missing.items())},
        "argument_position_calls": len(arg_calls),
        "argument_position_without_unit": len(missing_arg),
        "argument_position_missing": {k: sorted(set(v)) for k, v in sorted(missing_arg.items())},
        "image_base": f"0x{IMAGE_BASE:08X}",
        "base_relative_callee_calls": len(baserel),
        "base_relative_without_unit": len(missing_baserel),
        "base_relative_missing": {k: sorted(set(v)) for k, v in sorted(missing_baserel.items())},
        "base_relative_unresolved": baserel_unres,
        "base_relative_value_sites": len(baserel_value),
        "exceptions": len(EXCEPTIONS),
        "exceptions_bad": exc_bad,
        "exceptions_ok": exc_ok,
        "stored_pointer_allowlist": len(STORED_POINTER_ALLOWLIST),
        "allowlist_bad": allow_bad,
        "allowlist_ok": allow_ok,
        "nine_digit_literals": nine,
        "ok": ok,
    }

    if a.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"receipts: {'OK' if receipts_ok else 'FAIL'} ({n_receipts} receipt(s), rc={rc})")
        print(f"units: {len(units)}   engine calls found: {len(calls)}   without a unit: {len(missing)}")
        for addr, files in sorted(missing.items()):
            print(f"  MISSING UNIT 0x{addr}  called from {', '.join(sorted(set(files)))}"
                  f"  [rule: ))0xADDR)(]")
        print(f"          address-in-argument calls found: {len(arg_calls)}"
              f"   without a unit: {len(missing_arg)}")
        for addr, files in sorted(missing_arg.items()):
            print(f"  MISSING UNIT 0x{addr}  called from {', '.join(sorted(set(files)))}"
                  f"  [rule: Call*Method<...> first argument / 9-digit cast-and-call]")
        print(f"          base()+0xRVA callee calls found: {len(baserel)}"
              f"   without a unit: {len(missing_baserel)}   (image base 0x{IMAGE_BASE:08X})")
        for addr, sites in sorted(missing_baserel.items()):
            print(f"  MISSING UNIT 0x{addr}  called from {', '.join(sorted(set(sites)))}"
                  f"  [rule: (base() + 0xRVA)(...)]")
        for u in baserel_unres:
            print(f"  UNRESOLVED base()+0x site {u}  [rule: (base() + 0xRVA)(...)]")
        print(f"          base()+0xRVA value sites (not callee position, not gated): "
              f"{len(baserel_value)}")
        print(f"          exceptions: {len(EXCEPTIONS)}")
        for line in exc_ok:
            print(f"  {line}")
        for line in exc_bad:
            print(f"  BAD {line}")
        print(f"          stored-pointer allowlist: {len(STORED_POINTER_ALLOWLIST)}")
        for line in allow_ok:
            print(f"  {line}")
        for line in allow_bad:
            print(f"  BAD {line}")
        print(f"          9-digit literals in src/: {len(nine)}")
        for line in nine:
            print(f"  NINE-DIGIT {line}  [rule: no 0x literal of 9+ hex digits]")
        if not receipts_ok:
            print("--- decomp.py check output ---")
            print(out.strip())
        print(f"VERDICT: {'OK' if ok else 'FAIL'}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

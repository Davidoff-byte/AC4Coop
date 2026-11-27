"""ini_keys.py - I-706 / I-408: every INI key the plugin reads, against the template and the config dump.

    py -3.14 tools/ini_keys.py [--check] [--ini AC4Coop.ini] [--csv]

Scans src/**/*.cpp for GetPrivateProfileIntA/StringA/IntW/StringW("Section", "Key", default, ...) with literal
section/key and prints one row per (section, key):
    section  key  code default(s)  file:line (first read)  in-template?  template value  in-dump-table?
"in-dump-table" = the key is a row of the `[AC4Coop] CFG` dump (a `{ "S", "K", d }` triple in src/*.cpp or an
explicit `CFG S.K=` format string). Calls whose section/key is not a string literal are listed as DYNAMIC
(not scored). Wrapper helpers are resolved: a key that is a parameter of the enclosing function is
attributed to every `Func("Lit"` call site and to every `Func(tbl[i].field)` call site (the literal at `field`'s
position in each row of `T tbl[] = {...}`, e.g. the ship Tune* table), and `arr[i]` to the literals of
`arr[] = {...}`. Defaults through a wrapper are resolved (literal, the call site's argument, or the parallel
`def[i]` element). `--selftest` runs the resolver on synthetic sources. Read-only: never edits sources or the template.

--check exits 1 when any key read in code is missing from the dump table or from the template, or the template
has a key no code reads. The three lists are printed with counts. Exit 0 otherwise.
"""
from __future__ import annotations
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CALL_RE = re.compile(r'GetPrivateProfile(Int|String)([AW])\s*\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*("(?:[^"\\]|\\.)*"|[^,)]+)', re.S)
LIT_RE = re.compile(r'^L?"([^"]*)"$')
# Dump-table row: { "Section", "Key", <int default | "string default" | nullptr> } (int table keys[], string table skeys[]).
TRIPLE_RE = re.compile(r'\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*("[^"\n]*"|[^{}",\n]+?)\s*\}')   # 3rd: literal or expression (fi ? 12 : 0)
CFG_FMT_RE = re.compile(r'CFG (\w+)\.(\w+)=')


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, keeping line numbers and string literals."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i); j = n if j < 0 else j
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def scan_code(src: str):
    reads: dict = {}
    dynamic: list = []
    dump: set = set()
    texts: dict = {}
    for path in sorted(glob.glob(os.path.join(src, '**', '*.cpp'), recursive=True)):
        with open(path, encoding='utf-8', errors='replace') as f:
            text = strip_comments(f.read())
        rel = os.path.relpath(path, ROOT)
        for m in CALL_RE.finditer(text):
            line = text.count('\n', 0, m.start()) + 1
            s, k = LIT_RE.match(m.group(3)), LIT_RE.match(m.group(4))
            if not (s and k):
                dynamic.append({'where': '%s:%d' % (rel, line), 'sec': m.group(3), 'key': m.group(4),
                                'def': m.group(5).strip()})
                continue
            key = (s.group(1), k.group(1))
            r = reads.setdefault(key, {'defaults': [], 'where': '%s:%d' % (rel, line), 'n': 0})
            d = m.group(5).strip()
            if d not in r['defaults']:
                r['defaults'].append(d)
            r['n'] += 1
        for m in TRIPLE_RE.finditer(text):
            dump.add((m.group(1), m.group(2)))
        for m in CFG_FMT_RE.finditer(text):
            dump.add((m.group(1), m.group(2)))
        texts[rel] = text
    # Second pass: a dynamic call with a literal section and an identifier key is resolved when the key is
    #  (a) a parameter of the enclosing function  -> every `Func("Lit"` call site in src is a read of Section.Lit
    #      ... and every `Func(tbl[...].field)` call site, with `T tbl[] = { {..}, .. }` and `struct T { .. field .. }`
    #      in the same file -> the string literal at field's position in each row is a read (struct-table keys,
    #      e.g. the ship Tune* table read through TuneReadFloat(kTuneFields[i].iniKey)).
    #  (b) `arr[...]` with `arr[...] = { "a", "b" }` in the same file -> each literal is a read.
    # The default is resolved too: a literal default is reported as is; a default that is a parameter of the wrapper
    # is the call site's argument at that position; `def[...]` parallel to `arr[...]` is the element at the same index.
    # Anything else stays DYNAMIC (unscored).
    unresolved = []
    for d in dynamic:
        where, sec, keyexpr, dexpr = d['where'], d['sec'], d['key'], d['def']
        rel, line = where.rsplit(':', 1)
        text = texts[rel]
        s = LIT_RE.match(sec)
        found = []
        if s:
            ka = re.fullmatch(r'(\w+)\s*\[.*\]', keyexpr)
            ki = re.fullmatch(r'\w+', keyexpr)
            if ka:
                items = array_items(text, ka.group(1))
                dm = re.fullmatch(r'(\w+)\s*\[.*\]', dexpr)
                defs = array_items(text, dm.group(1)) if dm else None
                for i, it in enumerate(items or []):
                    lm = re.fullmatch(r'"(\w+)"', it)
                    if lm:
                        dv = defs[i] if defs and i < len(defs) else dexpr
                        found.append((lm.group(1), where, 'via %s[]' % ka.group(1), dv))
            elif ki:
                head = text[:sum(len(x) + 1 for x in text.split('\n')[:int(line) - 1])]
                fm = None
                for m2 in re.finditer(r'(\w+)\s*\(([^()]*)\)\s*(?:const\s*)?\{', head):
                    if m2.group(1) not in ('if', 'while', 'for', 'switch', 'catch', '__except', 'sizeof'):
                        fm = m2
                if fm and re.search(r'\b%s\b' % re.escape(keyexpr), fm.group(2)):
                    fn = fm.group(1)
                    pnames = [(re.findall(r'\w+', p) or [''])[-1] for p in split_top(fm.group(2))]
                    di = pnames.index(dexpr) if dexpr in pnames else -1
                    for r2, t2 in texts.items():
                        for cm in re.finditer(r'\b%s\s*\(' % re.escape(fn), t2):
                            args = split_top(paren_body(t2, cm.end() - 1))
                            if not args:
                                continue
                            w = '%s:%d' % (r2, t2.count('\n', 0, cm.start()) + 1)
                            dv = (args[di] if 0 <= di < len(args) else dexpr)
                            lm = re.fullmatch(r'"(\w+)"', args[0])
                            if lm:
                                found.append((lm.group(1), w, 'via %s()' % fn, dv))
                                continue
                            tm = re.fullmatch(r'(\w+)\s*\[[^\]]*\]\s*\.\s*(\w+)', args[0])
                            if tm:
                                fi = struct_field_index(t2, tm.group(1), tm.group(2))
                                for row in (array_items(t2, tm.group(1)) or []) if fi >= 0 else []:
                                    cols = split_top(row.strip()[1:-1]) if row.strip().startswith('{') else []
                                    km = re.fullmatch(r'"(\w+)"', cols[fi]) if fi < len(cols) else None
                                    if km:
                                        found.append((km.group(1), w, 'via %s(%s[].%s)' % (fn, tm.group(1), tm.group(2)), dv))
        if not found:
            unresolved.append('%s %s, %s' % (where, sec, keyexpr))
            continue
        for lit, w, how, dv in found:
            r = reads.setdefault((s.group(1), lit), {'defaults': [], 'where': '%s %s' % (w, how), 'n': 0})
            if dv not in r['defaults']:
                r['defaults'].append(dv)
            r['n'] += 1
    return reads, unresolved, dump


def paren_body(text: str, i: int) -> str:
    """Text between the bracket at text[i] ('(' or '{') and its balanced closer (string literals respected)."""
    op = text[i]; cl = ')' if op == '(' else '}'
    depth, j, n = 0, i, len(text)
    while j < n:
        c = text[j]
        if c == '"':
            j += 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
        elif c in '({':
            depth += 1
        elif c in ')}':
            depth -= 1
            if depth == 0:
                return text[i + 1:j] if c == cl else ''
        j += 1
    return ''


def split_top(s: str) -> list:
    """Split on commas at bracket depth 0 (string literals respected); items stripped, empty tail dropped."""
    out, depth, cur, i, n = [], 0, [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur.append(s[i:j + 1]); i = j + 1; continue
        if c in '({[':
            depth += 1
        elif c in ')}]':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(''.join(cur).strip()); cur = []
        else:
            cur.append(c)
        i += 1
    tail = ''.join(cur).strip()
    if tail:
        out.append(tail)
    return out


def array_items(text: str, name: str):
    """Top-level initializer items of `name[...] = { ... }` in text, or None."""
    m = re.search(r'\b%s\s*\[[^\]]*\]\s*=\s*\{' % re.escape(name), text)
    return split_top(paren_body(text, m.end() - 1)) if m else None


def struct_field_index(text: str, arr: str, field: str) -> int:
    """Position of `field` in the struct that `arr` is declared as (`T arr[...] =` + `struct T { ... }`), or -1."""
    tm = re.search(r'\b(\w+)\s+%s\s*\[[^\]]*\]\s*=' % re.escape(arr), text)
    sm = re.search(r'\bstruct\s+%s\s*\{' % re.escape(tm.group(1)), text) if tm else None
    if not sm:
        return -1
    names = []
    for decl in paren_body(text, sm.end() - 1).split(';'):
        decl = decl.strip()
        if not decl:
            continue
        parts = split_top(decl)
        names.append((re.findall(r'\w+', parts[0]) or [''])[-1])
        names += [(re.findall(r'\w+', p) or [''])[0] for p in parts[1:]]
    return names.index(field) if field in names else -1


def scan_ini(path: str) -> dict:
    keys: dict = {}
    sec = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            s = ln.strip()
            if s.startswith('[') and s.endswith(']'):
                sec = s[1:-1]
            elif sec and s and not s.startswith(';') and '=' in s:
                k, v = s.split('=', 1)
                keys[(sec, k.strip())] = v.split(';', 1)[0].strip()
    return keys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--ini', default=os.path.join(ROOT, 'AC4Coop.ini'))
    ap.add_argument('--src', default=os.path.join(ROOT, 'src'))
    ap.add_argument('--csv', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    reads, dynamic, dump = scan_code(a.src)
    tmpl = scan_ini(a.ini)
    sep = ',' if a.csv else '\t'
    print(sep.join(['section', 'key', 'code_default', 'first_read', 'in_template', 'template_value', 'in_dump']))
    for key in sorted(reads, key=lambda x: (x[0].lower(), x[1].lower())):
        r = reads[key]
        print(sep.join([key[0], key[1], '|'.join(r['defaults']), r['where'] + ('' if r['n'] == 1 else ' (+%d)' % (r['n'] - 1)),
                        'Y' if key in tmpl else 'N', tmpl.get(key, ''), 'Y' if key in dump else 'N']))
    no_dump = sorted(k for k in reads if k not in dump)
    no_tmpl = sorted(k for k in reads if k not in tmpl)
    orphan = sorted(k for k in tmpl if k not in reads)
    stale_dump = sorted(k for k in dump if k not in reads)
    print('\nkeys read in code: %d   template keys: %d   dump-table rows: %d   dynamic calls (not scored): %d'
          % (len(reads), len(tmpl), len(dump), len(dynamic)))
    for title, lst in (('read in code, NOT in dump table', no_dump), ('read in code, NOT in template', no_tmpl),
                       ('in template, read by NO code', orphan), ('in dump table, read by no code (info)', stale_dump)):
        print('\n%s: %d' % (title, len(lst)))
        for s, k in lst:
            print('  %s.%s%s' % (s, k, ('  ' + reads[(s, k)]['where']) if (s, k) in reads else ''))
    if dynamic:
        print('\ndynamic section/key calls: %d' % len(dynamic))
        for d in dynamic:
            print('  ' + d)
    if a.check:
        bad = bool(no_dump or no_tmpl or orphan)
        print('\ncheck: %s' % ('FAIL' if bad else 'PASS'))
        return 1 if bad else 0
    return 0


SELFTEST_SRC = r'''
struct TF { const char* iniKey; const char* logName; unsigned off; };
static const TF kTbl[] = {
    { "TblA", "Day.A", 0x0C },   // a comment, with { braces }
    { "TblB", "Day.B", 0x10 },
};
static int ReadF(const char* key) {
    char b[8]; GetPrivateProfileStringA("Coop", key, sent, b, 8, Paths::Ini()); return 0;
}
static int Flag(const char* key, int* c) {
    return GetPrivateProfileIntA("Coop", key, 0, Paths::Ini());
}
static int Num(const char* key, int def, int* c) {
    return GetPrivateProfileIntA("Coop", key, def, Paths::Ini());
}
static void Use() {
    for (int i = 0; i < 2; ++i) ReadF(kTbl[i].iniKey);
    ReadF("TblLit");
    Flag("FlagK", nullptr);
    Num("NumK", 120, nullptr);
    static const char* kName[2] = { "ArrA", "ArrB" };
    static const int   kDef[2]  = { 0, 1 };
    for (int f = 0; f < 2; ++f) GetPrivateProfileIntA("Coop", kName[f], kDef[f], Paths::Ini());
    GetPrivateProfileIntA("Coop", "Plain", 7, Paths::Ini());
    GetPrivateProfileIntA(keys[i].s, keys[i].k, keys[i].d, Paths::Ini());
}
static const K keys[] = { { "Coop", "Plain", 7 } };
static const S skeys[] = { { "Net", "StrK", "udp" }, { "Net", "NulK", nullptr } };
const K dyn[] = { { "Coop", "ExprK", fi ? 12 : 0 } };
'''


def selftest() -> int:
    """Resolver cases (each must hold; a failing case prints FAIL and exits 1)."""
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, 'st.cpp'), 'w', encoding='utf-8') as f:
            f.write(SELFTEST_SRC)
        reads, unresolved, dump = scan_code(td)
    want = {('Coop', 'TblA'): 'sent', ('Coop', 'TblB'): 'sent', ('Coop', 'TblLit'): 'sent', ('Coop', 'FlagK'): '0',
            ('Coop', 'NumK'): '120', ('Coop', 'ArrA'): '0', ('Coop', 'ArrB'): '1', ('Coop', 'Plain'): '7'}
    bad = 0
    for k, dv in want.items():
        got = reads.get(k, {}).get('defaults')
        ok = got == [dv]
        bad += not ok
        print('%s read %s.%s default=%s (want %s)' % ('PASS' if ok else 'FAIL', k[0], k[1], got, dv))
    for k in (('Coop', 'Day'), ('Coop', 'logName')):
        ok = k not in reads
        bad += not ok
        print('%s no false read %s.%s' % ('PASS' if ok else 'FAIL', *k))
    for k in (('Coop', 'Plain'), ('Net', 'StrK'), ('Net', 'NulK'), ('Coop', 'ExprK')):
        ok = k in dump
        bad += not ok
        print('%s dump row %s.%s' % ('PASS' if ok else 'FAIL', *k))
    ok = len(unresolved) == 1 and 'keys[i].k' in unresolved[0]
    bad += not ok
    print('%s unresolved = the dump loop only: %s' % ('PASS' if ok else 'FAIL', unresolved))
    print('selftest: %s' % ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

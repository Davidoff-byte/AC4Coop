"""ini_keys.py - I-706 / I-408: every INI key the plugin reads, against the template and the config dump.

    py -3.14 tools/ini_keys.py [--check] [--ini AC4Coop.ini] [--csv]

Scans src/**/*.cpp for GetPrivateProfileIntA/StringA/IntW/StringW("Section", "Key", default, ...) with literal
section/key and prints one row per (section, key):
    section  key  code default(s)  file:line (first read)  in-template?  template value  in-dump-table?
"in-dump-table" = the key is a row of the `[AC4Coop] CFG` dump (a `{ "S", "K", d }` triple in src/*.cpp or an
explicit `CFG S.K=` format string). Calls whose section/key is not a string literal are listed as DYNAMIC
(not scored). Wrapper helpers are resolved: a key that is a parameter of the enclosing function is
attributed to every `Func("Lit"` call site, and `arr[i]` to the literals of `arr[] = {...}`. Known blind spot:
keys carried in a struct table and read through a field (the ship Tune* table, phase1.cpp ~9870) show as
false orphans. Read-only: never edits sources or the template.

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
TRIPLE_RE = re.compile(r'\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*(-?\w+)\s*\}')
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
                dynamic.append({'where': '%s:%d' % (rel, line), 'sec': m.group(3), 'key': m.group(4)})
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
    #  (b) `arr[...]` with `arr[...] = { "a", "b" }` in the same file -> each literal is a read.
    # Anything else stays DYNAMIC (unscored).
    unresolved = []
    for d in dynamic:
        where, sec, keyexpr = d['where'], d['sec'], d['key']
        rel, line = where.rsplit(':', 1)
        text = texts[rel]
        s = LIT_RE.match(sec)
        found = []
        if s:
            ka = re.fullmatch(r'(\w+)\s*\[.*\]', keyexpr)
            ki = re.fullmatch(r'\w+', keyexpr)
            if ka:
                am = re.search(r'\b%s\s*\[\w*\]\s*=\s*\{([^}]*)\}' % re.escape(ka.group(1)), text)
                if am:
                    found = [(lit, where, 'via %s[]' % ka.group(1)) for lit in re.findall(r'"(\w+)"', am.group(1))]
            elif ki:
                head = text[:sum(len(x) + 1 for x in text.split('\n')[:int(line) - 1])]
                fm = None
                for m2 in re.finditer(r'(\w+)\s*\(([^()]*)\)\s*(?:const\s*)?\{', head):
                    if m2.group(1) not in ('if', 'while', 'for', 'switch', 'catch', '__except', 'sizeof'):
                        fm = m2
                if fm and re.search(r'\b%s\b' % re.escape(keyexpr), fm.group(2)):
                    fn = fm.group(1)
                    for r2, t2 in texts.items():
                        for cm in re.finditer(r'\b%s\s*\(\s*"(\w+)"' % re.escape(fn), t2):
                            found.append((cm.group(1), '%s:%d' % (r2, t2.count('\n', 0, cm.start()) + 1), 'via %s()' % fn))
        if not found:
            unresolved.append('%s %s, %s' % (where, sec, keyexpr))
            continue
        for lit, w, how in found:
            r = reads.setdefault((s.group(1), lit), {'defaults': [], 'where': w, 'n': 0})
            if how not in r['defaults']:
                r['defaults'].append(how)
            r['n'] += 1
    return reads, unresolved, dump


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
    a = ap.parse_args()
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


if __name__ == '__main__':
    sys.exit(main())

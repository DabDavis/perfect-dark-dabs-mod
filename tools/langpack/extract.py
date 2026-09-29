#!/usr/bin/env python3
"""
Writes the English source catalogs a translator translates from
(CLAUDE-notes/languages.md, "Source catalogs"):

    lang/_source/port.json       the port's own strings: { key: English }
    lang/_source/port.ctx.json   { key: ["file:line", ...] }, where each shows
    lang/_source/ge.json         GoldenEye's English as GE Plus uses it,
                                 { "ge.<bank>.<slot>": English }

    python3 tools/langpack/extract.py [--ge-text DIR] [--out DIR]
    python3 tools/langpack/extract.py --leftovers

lang/_source/ is generated and gitignored: GoldenEye's English is Rare's text,
read from the GoldenEye decomp tree (the same strings as the player's US ROM),
and is never committed. Perfect Dark's English needs no catalog - a translator
reads src/assets/ntsc-final/lang/<bank>.json's "en".

A port string is catalogued when the code hands it to a lookup, so the key is
exactly what langTr() is asked for (its trailing newline dropped, as the game
drops it):
- a menu item with MENUITEMFLAG_LITERAL_TEXT (its label and right-hand text);
- a dialog with MENUDIALOGFLAG_LITERAL_TEXT (its title);
- a dropdown's options: what a MENUOP_GETOPTIONTEXT handler returns, a
  literal or a row of a string table it indexes (menuitem.c translates them);
- langTr("..."), langTrFind("..."), langTrCtx("ctx", "...") (key "ctx|..."),
  langAddPortText("...") and LANG_N("...") - the no-op marker for a string
  kept in a table (or written by a worker thread) and translated where it is
  used - and gebean.c's POOLBODY() rows, a Combat Simulator name + "\n".
A string with no letter in it is left out: there is nothing to translate.
The port's strings come from port/src, src/game and src/game/mplayer; a file
of the decompilation with none of those markers is not read at all.

GoldenEye's are every string of its US English text files (L*E.c, #if'd as
the US cartridge: LlenE's and LtitleE's Japanese-only rows are not the US
ROM's), keyed ge.<bank>.<slot>: the file name less its L and E, lower case,
and the row. The u/ and j/ folders are Japanese only.

--leftovers lists literals in port/src that look drawn (a capital letter and a
trailing newline) but are not catalogued: the next hooks to wire.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def default_ge_text():
    """$PD_GE_TEXT, else a 007/assets/obseg/text beside this tree or beside a
    parent of it (a git worktree sits a few levels inside the checkout)."""
    if os.environ.get('PD_GE_TEXT'):
        return os.environ['PD_GE_TEXT']
    d = ROOT
    while os.path.dirname(d) != d:
        cand = os.path.join(os.path.dirname(d), '007', 'assets', 'obseg', 'text')
        if os.path.isdir(cand):
            return cand
        d = os.path.dirname(d)
    return os.path.normpath(os.path.join(ROOT, '..', '007', 'assets', 'obseg', 'text'))


# where the port's strings are looked for, relative to ROOT
PORT_DIRS = ['port/src', 'src/game', 'src/game/mplayer']

# The US cartridge's defines (the GoldenEye decomp's Makefile, VERSION=US)
GE_DEFINES = {'VERSION_US', 'LANG_US', 'REFRESH_NTSC', 'LEFTOVERDEBUG', 'LEFTOVERSPECTRUM', 'BUGFIX_R0', 'BYTEMATCH'}

# a call whose argument is a port string: (which argument, what the code adds
# to it) - gebean.c's POOLBODY() writes a Combat Simulator name as name "\n"
CALLS = {'langTr': (0, ''), 'langTrFind': (0, ''), 'langAddPortText': (0, ''), 'LANG_N': (0, ''),
         'langTrCtx': (1, ''), 'POOLBODY': (3, '\n')}
MARKERS = ('LITERAL_TEXT', 'MENUOP_GETOPTIONTEXT') + tuple(CALLS)


# ---------------------------------------------------------------- C tokens

class Tok:
    def __init__(self, kind, text, line):
        self.kind = kind    # 'id', 'str', 'num', 'punct'
        self.text = text    # a string literal's value, unescaped
        self.line = line

    def __repr__(self):
        return '%s(%r)@%d' % (self.kind, self.text, self.line)


ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '0': '\0', 'a': '\a',
           'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}


def unescape(body):
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n == 'x':
            m = re.match(r'[0-9a-fA-F]+', body[i + 2:])
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        elif n in '01234567':
            m = re.match(r'[0-7]{1,3}', body[i + 1:])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        else:
            out.append(ESCAPES.get(n, n))
            i += 2
    s = ''.join(out)
    # a literal is UTF-8 bytes; \xNN escapes spell them out
    try:
        return s.encode('latin-1').decode('utf-8')
    except (UnicodeEncodeError, UnicodeDecodeError):
        return s


TOKEN = re.compile(r'''
    (?P<dir>(?<![^\n])[ \t]*\#(?:\\\n|[^\n])*)
  | (?P<nl>\n)
  | (?P<ws>[ \t\r\f\v]+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?(?:\*/|\Z))
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])*')
  | (?P<id>[A-Za-z_]\w*)
  | (?P<num>(?:0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][-+]?\d+)?)[uUlLfF]*)
  | (?P<punct>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|.)
''', re.S | re.X)


def lex(src, defines=None):
    """
    Tokens of C source. Preprocessor lines are dropped; with `defines` set,
    #if/#ifdef/#else/#endif are evaluated (defined(X), !, ||, &&) and the
    code they exclude is dropped too - with None every branch is kept.
    Adjacent string literals are joined.
    """
    toks = []
    line = 1
    stack = []  # (outer active, a branch taken) per #if level
    active = True
    joinable = False

    def eval_cond(expr):
        expr = re.sub(r'/\*.*?\*/|//.*', '', expr)
        expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: '1' if m.group(1) in defines else '0', expr)
        expr = re.sub(r'defined\s+(\w+)', lambda m: '1' if m.group(1) in defines else '0', expr)
        expr = expr.replace('||', ' or ').replace('&&', ' and ').replace('!', ' not ')
        expr = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: m.group(0) if m.group(0) in ('or', 'and', 'not') else '0', expr)
        try:
            return bool(eval(expr, {}, {}))
        except Exception:
            return False

    for m in TOKEN.finditer(src):
        kind = m.lastgroup
        text = m.group(0)
        if kind == 'nl':
            line += 1
            continue
        if kind == 'ws':
            continue
        if kind in ('lc', 'bc', 'dir'):
            if kind == 'dir' and defines is not None:
                directive = text.strip()[1:].replace('\\\n', ' ').strip()
                d = re.match(r'(\w+)\s*(.*)', directive, re.S)
                word, rest = (d.group(1), d.group(2)) if d else ('', '')
                if word in ('if', 'ifdef', 'ifndef'):
                    if word == 'ifdef':
                        cond = bool(rest.split()) and rest.split()[0] in defines
                    elif word == 'ifndef':
                        cond = not rest.split() or rest.split()[0] not in defines
                    else:
                        cond = eval_cond(rest)
                    stack.append((active, cond))
                    active = active and cond
                elif word == 'elif' and stack:
                    outer, taken = stack[-1]
                    cond = not taken and eval_cond(rest)
                    stack[-1] = (outer, taken or cond)
                    active = outer and cond
                elif word == 'else' and stack:
                    outer, taken = stack[-1]
                    stack[-1] = (outer, True)
                    active = outer and not taken
                elif word == 'endif' and stack:
                    active = stack.pop()[0]
            line += text.count('\n')
            continue
        if not active:
            continue
        if kind == 'str':
            value = unescape(text[1:-1])
            if joinable and toks and toks[-1].kind == 'str':
                toks[-1].text += value
            else:
                toks.append(Tok('str', value, line))
            joinable = True
            continue
        joinable = False
        toks.append(Tok('num' if kind == 'chr' else kind, text, line))

    return toks


_pairs = {}


def matching(toks, i):
    """The index of the bracket closing toks[i] (all pairs found once a list)."""
    pairs = _pairs.get(id(toks))
    if pairs is None or pairs[0] is not toks:
        m = {}
        stack = []
        for j, t in enumerate(toks):
            if t.kind == 'punct':
                if t.text in ('{', '(', '['):
                    stack.append(j)
                elif t.text in ('}', ')', ']') and stack:
                    m[stack.pop()] = j
        pairs = (toks, m)
        _pairs[id(toks)] = pairs
    return pairs[1].get(i, len(toks) - 1)


def direct(toks, a, b):
    """The tokens of toks[a+1:b] not inside a nested bracket."""
    out = []
    depth = 0
    for t in toks[a + 1:b]:
        if t.kind == 'punct' and t.text in '{([':
            depth += 1
        elif t.kind == 'punct' and t.text in '})]':
            depth -= 1
        elif depth == 0:
            out.append(t)
    return out


# ---------------------------------------------------------------- port strings

def has_letter(s):
    return any(ch.isalpha() for ch in s)


def key_of(s):
    return s[:-1] if s.endswith('\n') else s


class Catalog:
    def __init__(self):
        self.strings = {}   # key -> English
        self.where = {}     # key -> [file:line]

    def add(self, english, path, line, ctx=None):
        if not english or not has_letter(english):
            return
        key = key_of(english)
        if ctx:
            key = ctx + '|' + key
        if key not in self.strings:
            self.strings[key] = english
        loc = '%s:%d' % (path, line)
        where = self.where.setdefault(key, [])
        if loc not in where:
            where.append(loc)


def string_arrays(toks):
    """{ name: (open brace index, close) } of `name[...] = { ... }` tables."""
    arrays = {}
    for i, t in enumerate(toks):
        if t.kind == 'id' and i + 1 < len(toks) and toks[i + 1].text == '[':
            j = matching(toks, i + 1)
            # a second dimension, [N][M]
            while j + 1 < len(toks) and toks[j + 1].text == '[':
                j = matching(toks, j + 1)
            if j + 2 < len(toks) and toks[j + 1].text == '=' and toks[j + 2].text == '{':
                arrays.setdefault(t.text, (j + 2, matching(toks, j + 2)))
    return arrays


def literals_in(toks, a, b):
    return [t for t in toks[a:b + 1] if t.kind == 'str']


def functions(toks):
    """(body open, body close) of every function definition at file scope."""
    out = []
    i = 0
    depth = 0
    while i < len(toks):
        t = toks[i]
        if t.kind == 'punct' and t.text == '{':
            if depth == 0 and i > 0 and toks[i - 1].text == ')':
                j = matching(toks, i)
                out.append((i, j))
                i = j + 1
                continue
            depth += 1
        elif t.kind == 'punct' and t.text == '}':
            depth -= 1
        i += 1
    return out


def extract_port_file(cat, path, rel):
    with open(path, encoding='utf-8', errors='replace') as f:
        src = f.read()
    # most of src/game is the decompilation, with none of the port's text
    if not any(m in src for m in MARKERS):
        return
    toks = lex(src)
    arrays = string_arrays(toks)

    # menu items and dialogs with literal text: every brace group
    for i, t in enumerate(toks):
        if t.kind != 'punct' or t.text != '{':
            continue
        j = matching(toks, i)
        inner = direct(toks, i, j)
        ids = set(x.text for x in inner if x.kind == 'id')
        literal = False
        if 'MENUITEMFLAG_LITERAL_TEXT' in ids and any(x.startswith('MENUITEMTYPE_') for x in ids):
            literal = True
        if 'MENUDIALOGFLAG_LITERAL_TEXT' in ids and any(x.startswith('MENUDIALOGTYPE_') for x in ids):
            literal = True
        if literal:
            for x in inner:
                if x.kind == 'str':
                    cat.add(x.text, rel, x.line)

    # explicit lookups and markers
    for i, t in enumerate(toks):
        if t.kind == 'id' and t.text in CALLS and i + 1 < len(toks) and toks[i + 1].text == '(':
            j = matching(toks, i + 1)
            args = []
            cur = []
            for x in direct(toks, i + 1, j):
                if x.kind == 'punct' and x.text == ',':
                    args.append(cur)
                    cur = []
                else:
                    cur.append(x)
            args.append(cur)
            if t.text == 'langTrCtx':
                if len(args) == 2 and len(args[0]) == 1 and args[0][0].kind == 'str':
                    for x in args[1]:
                        if x.kind == 'str':
                            cat.add(x.text, rel, x.line, ctx=args[0][0].text)
                continue
            # langTr(cond ? "a" : "b") gives both
            argn, suffix = CALLS[t.text]
            for x in args[argn] if len(args) > argn else []:
                if x.kind == 'str':
                    cat.add(x.text + suffix, rel, x.line)

    # dropdown options
    for a, b in functions(toks):
        body = toks[a:b + 1]
        if not any(x.kind == 'id' and x.text == 'MENUOP_GETOPTIONTEXT' for x in body):
            continue
        local = string_arrays(body)
        for k in range(a, b):
            if toks[k].kind != 'id' or toks[k].text != 'return':
                continue
            # return (intptr_t)"x" / return (intptr_t)opts[i] / cond ? "a" : "b"
            end = k + 1
            while end < b and toks[end].text != ';':
                end += 1
            expr = toks[k + 1:end]
            for m, x in enumerate(expr):
                if x.kind == 'str':
                    cat.add(x.text, rel, x.line)
                elif x.kind == 'id' and m + 1 < len(expr) and expr[m + 1].text == '[':
                    if x.text in local:
                        oa, ob = local[x.text]
                        for s in literals_in(body, oa, ob):
                            cat.add(s.text, rel, s.line)
                    elif x.text in arrays:
                        oa, ob = arrays[x.text]
                        for s in literals_in(toks, oa, ob):
                            cat.add(s.text, rel, s.line)


def port_files():
    for d in PORT_DIRS:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if name.endswith('.c'):
                yield os.path.join(full, name), d + '/' + name


def extract_port():
    cat = Catalog()
    for path, rel in port_files():
        extract_port_file(cat, path, rel)
    return cat


LOGGERS = {'sysLogPrintf', 'printf', 'fprintf', 'sysFatalError', 'osSyncPrintf', 'fputs', 'puts',
           'configRegisterInt', 'configRegisterFloat', 'configRegisterString', 'configRegisterUInt',
           'getenv', 'sysArgCheck', 'sysArgGetString', 'fsFileLoad', 'fsFullPath', 'strcmp', 'strncmp',
           'strstr', 'traceLog', 'assert'}


def leftovers(cat):
    """Literals that look drawn and are not catalogued, with where they are."""
    known = set(cat.strings)
    out = []
    for path, rel in port_files():
        if not rel.startswith('port/'):
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            toks = lex(f.read())
        skip_until = -1
        for i, t in enumerate(toks):
            if t.kind == 'id' and t.text in LOGGERS and i + 1 < len(toks) and toks[i + 1].text == '(':
                skip_until = max(skip_until, matching(toks, i + 1))
            if i <= skip_until or t.kind != 'str':
                continue
            s = t.text
            if not s.endswith('\n') or not re.search(r'[A-Z][a-z]', s) or key_of(s) in known:
                continue
            out.append('%s:%d: %r' % (rel, t.line, s))
    return out


# ---------------------------------------------------------------- GoldenEye

def ge_bank_of(filename):
    """LdamE.c -> dam (the key convention: file name less L and E, lower case)."""
    stem = filename[:-2]
    return stem[1:-1].lower()


def extract_ge(textdir):
    """{ bank: { slot: English } } from the decomp's US English L*E.c files."""
    banks = {}
    for name in sorted(os.listdir(textdir)):
        if not re.fullmatch(r'L[a-z0-9]+E\.c', name):
            continue
        with open(os.path.join(textdir, name), encoding='utf-8', errors='replace') as f:
            toks = lex(f.read(), GE_DEFINES)
        # char *LdamE[] = { ... };
        start = None
        for i, t in enumerate(toks):
            if t.kind == 'punct' and t.text == '{':
                start = i
                break
        if start is None:
            continue
        end = matching(toks, start)
        rows = []
        cur = []
        for t in toks[start + 1:end]:
            if t.kind == 'punct' and t.text == ',':
                rows.append(cur)
                cur = []
            else:
                cur.append(t)
        if cur:
            rows.append(cur)
        slots = {}
        for slot, row in enumerate(rows):
            strs = [t.text for t in row if t.kind == 'str']
            if strs and has_letter(strs[0]):
                slots[slot] = strs[0]
        banks[ge_bank_of(name)] = slots
    return banks


# ---------------------------------------------------------------- output

def write_json(path, data):
    text = json.dumps(data, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    try:
        with open(path, encoding='utf-8') as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ge-text', default=default_ge_text(),
                    help='the GoldenEye decomp\'s assets/obseg/text (default: %(default)s)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'lang', '_source'))
    ap.add_argument('--leftovers', action='store_true', help='list drawn-looking literals not catalogued')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    cat = extract_port()

    if args.leftovers:
        for line in leftovers(cat):
            print(line)
        return 0

    os.makedirs(args.out, exist_ok=True)
    write_json(os.path.join(args.out, 'port.json'), cat.strings)
    write_json(os.path.join(args.out, 'port.ctx.json'), cat.where)

    ge = {}
    per_bank = {}
    if os.path.isdir(args.ge_text):
        for bank, slots in extract_ge(args.ge_text).items():
            per_bank[bank] = (len(slots), sum(len(s) for s in slots.values()))
            for slot, text in slots.items():
                ge['ge.%s.%d' % (bank, slot)] = text
        write_json(os.path.join(args.out, 'ge.json'), ge)
    else:
        print('extract: no GoldenEye text at %s (--ge-text); ge.json not written' % args.ge_text, file=sys.stderr)

    if not args.quiet:
        print('port.json: %d strings, %d characters' % (len(cat.strings), sum(len(s) for s in cat.strings.values())))
        if ge:
            print('ge.json: %d strings, %d characters' % (len(ge), sum(len(s) for s in ge.values())))
            for bank in sorted(b for b in per_bank if per_bank[b][0]):
                print('  %-12s %4d strings %7d characters' % (bank, per_bank[bank][0], per_bank[bank][1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())

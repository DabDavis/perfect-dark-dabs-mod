#!/usr/bin/env python3
"""
Diffs every data table of the N64 build against the XBLA release's loaded image.

The release is 4J's recompile of the same C, big-endian with 32-bit pointers,
so a table keeps its layout and can be found by its own bytes.

Finding a table: windows of three pointer-free words from the N64 table vote
on where it starts in the image. Tables that match whole and in one place are
anchors; every other table is also tried where its anchored neighbours say it
should sit (tables keep their order), and no two tables may claim the same
start.

Comparing it: the N64 layout of each table is rebuilt from the port's debug
info (the port shares the decomp's structs; pointers are re-laid at 4 bytes),
so fields are compared one by one - a pointer only by whether it is NULL, a
float to a rounding, anything else exactly - and named by row and field. A
table with no usable layout is compared word by word.

The stage1.elf of the decomp has no debug info, which is why the port's is
used; a struct the port grew under #ifndef PLATFORM_N64 no longer fits the
N64 table's size and falls back to words.

The image comes from .xbla-work/gunturn/dumpimage.sh (Xenia's decrypted
default.xex at guest 0x82000000), read after the game started, so a table the
game writes at startup shows its running values. The report carries bytes of
the release: keep it out of the repository.

  tablediff.py [--elf ELF] [--image BIN] [--port pd.x86_64] [--min BYTES] [--out REPORT]
"""

import argparse
import bisect
import collections
import json
import os
import re
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
WORK = os.path.normpath(os.path.join(ROOT, '..'))
IMAGE_BASE = 0x82000000


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=os.path.join(WORK, 'pd-upstream/build/ntsc-final/stage1.elf'))
    ap.add_argument('--image', default=os.path.join(WORK, '.xbla-work/default.image-82000000.early.bin'),
                    help='the image read the moment it loaded (dumpimage.sh early)')
    ap.add_argument('--late', default=os.path.join(WORK, '.xbla-work/default.image-82000000.bin'),
                    help='the same run read after startup; tables that differ were written by the game')
    ap.add_argument('--port', default=os.path.join(ROOT, 'build/pd.x86_64'))
    ap.add_argument('--min', type=int, default=32, help='smallest table to compare, in bytes')
    ap.add_argument('--out', default=os.path.join(WORK, '.xbla-work/xex-tablediff.txt'))
    ap.add_argument('--max-changes', type=int, default=150, help='changes listed per table')
    return ap.parse_args()


# ---------------------------------------------------------------- the N64 side

def elf_tables(elf, minsize):
    sections = {}
    for line in subprocess.run(['readelf', '-SW', elf], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if m:
            sections[int(m.group(1))] = dict(name=m.group(2), type=m.group(3), addr=int(m.group(4), 16),
                                             off=int(m.group(5), 16), size=int(m.group(6), 16))
    data = open(elf, 'rb').read()
    tables, seen = [], set()
    for line in subprocess.run(['readelf', '-sW', elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) < 8 or p[3] != 'OBJECT' or not p[6].isdigit():
            continue
        addr, size, ndx, name = int(p[1], 16), int(p[2]), int(p[6]), p[7]
        sec = sections.get(ndx)
        if size < minsize or not sec or sec['type'] != 'PROGBITS' or (addr, name) in seen:
            continue
        seen.add((addr, name))
        off = sec['off'] + addr - sec['addr']
        tables.append(dict(name=name, addr=addr, size=size, section=sec['name'], bytes=data[off:off + size]))
    tables.sort(key=lambda t: t['addr'])
    return tables


# The port's debug info, re-laid out the way the N64 compiler did: pointers are
# 4 bytes, everything else keeps its size, alignment is the size of the
# largest member, bitfields fill units of their declared type from the top.
GDB_LAYOUT = r'''
import gdb, json, os

def leafkind(t):
    c = t.code
    if c == gdb.TYPE_CODE_PTR or c == gdb.TYPE_CODE_FUNC:
        return 'ptr'
    if c == gdb.TYPE_CODE_FLT:
        return 'float'
    return 'int'

class Unsupported(Exception):
    pass

def layout(t, depth=0):
    """(size, align, [(offset, size, kind, path)])"""
    if depth > 16:
        raise Unsupported('too deep')
    t = t.strip_typedefs()
    c = t.code
    if c in (gdb.TYPE_CODE_PTR, gdb.TYPE_CODE_FUNC):
        return 4, 4, [(0, 4, 'ptr', '')]
    if c in (gdb.TYPE_CODE_INT, gdb.TYPE_CODE_ENUM, gdb.TYPE_CODE_BOOL, gdb.TYPE_CODE_CHAR):
        s = t.sizeof
        if s == 8 and c == gdb.TYPE_CODE_INT and t.name and 'long' in t.name and 'long long' not in t.name:
            s = 4
        return s, s, [(0, s, 'int', '')]
    if c == gdb.TYPE_CODE_FLT:
        return t.sizeof, t.sizeof, [(0, t.sizeof, 'float', '')]
    if c == gdb.TYPE_CODE_ARRAY:
        lo, hi = t.range()
        n = hi - lo + 1
        if n <= 0:
            return 0, 1, []
        es, ea, ef = layout(t.target(), depth + 1)
        leaves = []
        if n * len(ef) > 20000:
            raise Unsupported('huge')
        for i in range(n):
            for off, s, k, p in ef:
                leaves.append((i * es + off, s, k, '[%d]%s' % (i, p)))
        return es * n, ea, leaves
    if c in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        isunion = c == gdb.TYPE_CODE_UNION
        off = 0
        align = 1
        size = 0
        leaves = []
        unit_off = unit_size = bitcur = None
        for f in t.fields():
            if f.is_base_class or not hasattr(f, 'bitpos'):
                continue
            name = f.name or ''
            if f.bitsize:
                fs = f.type.strip_typedefs().sizeof
                align = max(align, fs)
                if isunion:
                    leaves.append((0, fs, 'bits', '.' + name))
                    size = max(size, fs)
                    continue
                if unit_size != fs or bitcur + f.bitsize > fs * 8:
                    off = (off + fs - 1) // fs * fs if unit_off is None or unit_size != fs or bitcur + f.bitsize > fs * 8 else off
                    if unit_off is not None and unit_size is not None:
                        off = max(off, unit_off + unit_size)
                        off = (off + fs - 1) // fs * fs
                    unit_off, unit_size, bitcur = off, fs, 0
                    leaves.append((unit_off, fs, 'bits', '.' + name))
                else:
                    leaves[-1] = (leaves[-1][0], leaves[-1][1], 'bits', leaves[-1][3] + '/' + name)
                bitcur += f.bitsize
                off = unit_off + unit_size
                continue
            unit_off = unit_size = bitcur = None
            fs, fa, fl = layout(f.type, depth + 1)
            align = max(align, fa)
            if isunion:
                if not leaves:
                    leaves = [(o, s, k, '.' + name + p) for o, s, k, p in fl]
                size = max(size, fs)
                continue
            off = (off + fa - 1) // fa * fa
            leaves += [(off + o, s, k, '.' + name + p) for o, s, k, p in fl]
            off += fs
        if not isunion:
            size = off
        size = (size + align - 1) // align * align
        return size, align, leaves
    raise Unsupported('type code %d' % c)

names = json.load(open(os.environ["TD_NAMES"]))
out = {}
for n in names:
    try:
        s = gdb.lookup_global_symbol(n) or gdb.lookup_static_symbol(n)
    except gdb.error:
        s = None
    if not s:
        continue
    t = s.type.strip_typedefs()
    try:
        if t.code == gdb.TYPE_CODE_ARRAY:
            lo, hi = t.range()
            es, ea, ef = layout(t.target())
            out[n] = dict(elem=str(t.target()), count=hi - lo + 1, elemsize=es, leaves=ef)
        else:
            es, ea, ef = layout(t)
            out[n] = dict(elem=str(s.type), count=1, elemsize=es, leaves=ef)
    except (Unsupported, gdb.error, RuntimeError) as e:
        out[n] = dict(elem=str(s.type), count=0, elemsize=0, leaves=[], why=str(e))
json.dump(out, open(os.environ["TD_OUT"], "w"))
'''


def port_layouts(port, names):
    if not os.path.exists(port):
        return {}
    with tempfile.TemporaryDirectory() as tmp:
        np_, op, sp = (os.path.join(tmp, f) for f in ('names.json', 'out.json', 'layout.py'))
        json.dump(names, open(np_, 'w'))
        open(sp, 'w').write(GDB_LAYOUT)
        env = dict(os.environ, TD_NAMES=np_, TD_OUT=op)
        r = subprocess.run(['gdb', '-batch', '-nx', port, '-ex', 'source ' + sp], env=env, capture_output=True, text=True)
        if not os.path.exists(op):
            print(r.stderr[-2000:])
            return {}
        return json.load(open(op))


def table_layout(t, lay):
    """The leaves of the whole N64 table, or None when the port's layout does not fit it."""
    if not lay or not lay.get('elemsize'):
        return None, None
    es = lay['elemsize']
    rows = t['size'] // es
    if rows == 0 or t['size'] - rows * es >= es or (lay['count'] > 1 and rows < 1):
        return None, None
    # A table the port declares with a different count (a raised cap) still has
    # the N64 row size; a single object must be the whole symbol.
    if lay['count'] == 1 and es != t['size'] and not (0 <= t['size'] - es < 4):
        return None, None
    leaves = []
    for r in range(rows):
        for off, s, k, p in lay['leaves']:
            leaves.append((r * es + off, s, k, ('[%d]' % r if lay['count'] > 1 or rows > 1 else '') + p))
    return leaves, es


# --------------------------------------------------------------- the matching

def n64_pointer(w):
    return (0x70000000 <= w < 0x70100000 or 0x7f000000 <= w < 0x7f900000
            or 0x80000000 <= w < 0x80400000 or 0x02000000 <= w < 0x02600000)


def xbla_pointer(w):
    return 0x30000000 <= w < 0xc0000000


def words(b):
    n = len(b) // 4
    return list(struct.unpack('>%dI' % n, b[:n * 4]))


def as_float(w):
    return struct.unpack('>f', struct.pack('>I', w))[0]


def float_close(a, b, size):
    if size == 4:
        fa, fb = as_float(a), as_float(b)
    else:
        fa, fb = struct.unpack('>d', a.to_bytes(8, 'big'))[0], struct.unpack('>d', b.to_bytes(8, 'big'))[0]
    if fa != fa or fb != fb:
        return False
    return abs(fa - fb) <= 1e-5 * max(1.0, abs(fa))


def score_at(t, img, start):
    w = words(t['bytes'])
    xw = words(img[start:start + len(w) * 4])
    if len(xw) < len(w) or not w:
        return 0.0
    ok = 0
    for a, x in zip(w, xw):
        if a == x or (n64_pointer(a) and (x == 0) == (a == 0) and xbla_pointer(x)):
            ok += 1
    return ok / len(w)


def vote(t, img):
    b = t['bytes']
    w = words(b)
    good = [i for i in range(len(w) - 2)
            if not any(n64_pointer(x) for x in w[i:i + 3]) and len(set(b[i * 4:i * 4 + 12])) >= 5]
    votes = collections.Counter()
    if not good:
        return votes
    step = max(1, len(good) // 16)
    for i in good[::step][:16]:
        chunk = b[i * 4:i * 4 + 12]
        p, hits = img.find(chunk), 0
        while p >= 0 and hits < 64:
            s = p - i * 4
            if s >= 0 and s % 4 == 0:
                votes[s] += 1
            hits += 1
            p = img.find(chunk, p + 1)
    return votes


def locate(tables, img):
    for t in tables:
        t['votes'] = vote(t, img)
        t['cands'] = sorted(((score_at(t, img, s), s) for s, _ in t['votes'].most_common(6)), reverse=True)

    claimed = {}

    def claim(t, start, score, how):
        t['start'], t['score'], t['how'] = start, score, how
        claimed[start] = t

    # Anchors: whole, and nowhere else as good.
    for t in tables:
        c = t['cands']
        if c and c[0][0] == 1.0 and (len(c) == 1 or c[1][0] < 1.0) and c[0][1] not in claimed:
            claim(t, c[0][1], 1.0, 'anchor')

    by_section = collections.defaultdict(list)
    for t in tables:
        by_section[t['section']].append(t)

    for sec, ts in by_section.items():
        ts.sort(key=lambda t: t['addr'])
        for idx, t in enumerate(ts):
            if t.get('start') is not None:
                continue
            preds = []
            for step in (-1, 1):
                j = idx + step
                while 0 <= j < len(ts):
                    n = ts[j]
                    if n.get('how') == 'anchor':
                        preds.append((abs(n['addr'] - t['addr']), n['start'] + (t['addr'] - n['addr'])))
                        break
                    j += step
            options = {s: sc for sc, s in t['cands']}
            for dist, p in sorted(preds)[:2]:
                for d in range(-256, 260, 4):
                    s = p + d
                    if 0 <= s < len(img) and s not in options:
                        options[s] = score_at(t, img, s)
            if not options:
                t['start'], t['why'] = None, 'no anchor window and no anchored neighbour'
                continue
            nearest = sorted(preds)[0][1] if preds else None

            def key(item):
                s, sc = item
                return (sc, -abs(s - nearest) if nearest is not None else 0)
            ranked = sorted(options.items(), key=key, reverse=True)
            for s, sc in ranked:
                if s in claimed:
                    continue
                if sc >= 0.5:
                    claim(t, s, sc, 'voted' if s in t['votes'] else 'predicted')
                elif nearest is not None and abs(s - nearest) <= 256 and sc >= 0.2:
                    claim(t, s, sc, 'predicted, heavily changed')
                break
            if t.get('start') is None:
                t['start'] = None
                t['why'] = 'not found (best %.0f%%)' % (ranked[0][1] * 100) if ranked else 'not found'
    return tables


# ---------------------------------------------------------------- the diff

def is_text_id(v):
    return 0x0200 <= v < 0x8000


def diff_table(t, img, lay):
    leaves, es = table_layout(t, lay)
    # The N64 symbol sizes run a few bytes past the last row (805 for 22 rows
    # of 36), but a struct the port grew leaves most of a row over; that one
    # is not the N64 layout and is compared by words instead.
    misfit = leaves and t['size'] - (t['size'] // es) * es > 16
    if misfit:
        leaves = None
    start = t['start']
    x = img[start:start + t['size']]
    b = t['bytes']
    changes, rounds = [], 0
    if leaves:
        for off, size, kind, path in leaves:
            if off + size > len(b):
                break
            a = int.from_bytes(b[off:off + size], 'big')
            y = int.from_bytes(x[off:off + size], 'big')
            if a == y:
                continue
            # A pointer kept in an integer field (the decomp types some as u32).
            if size == 4 and n64_pointer(a) and IMAGE_BASE <= y < IMAGE_BASE + len(img):
                continue
            if kind == 'ptr':
                if (a == 0) != (y == 0):
                    changes.append((off, size, path, a, y, 'pointer vs NULL'))
                continue
            if kind == 'float' and size in (4, 8) and float_close(a, y, size):
                rounds += 1
                continue
            changes.append((off, size, path, a, y, kind))
        t['layout'] = 'fields (%d-byte rows)' % es
    else:
        w, xw = words(b), words(x)
        for i, (a, y) in enumerate(zip(w, xw)):
            if a == y:
                continue
            if n64_pointer(a) and xbla_pointer(y):
                continue
            if n64_pointer(a) and y == 0:
                changes.append((i * 4, 4, '', a, y, 'pointer vs NULL'))
                continue
            changes.append((i * 4, 4, '', a, y, 'word'))
        why = ('port struct does not fit the N64 size' if misfit else lay['why'] if lay and lay.get('why')
               else '' if lay else 'no debug type')
        t['layout'] = 'words' + (' (%s)' % why if why else '')
    t['changes'], t['rounds'] = changes, rounds

    def text_pair(a, y):
        return a == y or (is_text_id(a) and is_text_id(y) and (a >> 9) == (y >> 9))

    def text_change(c):
        off, size, path, a, y, kind = c
        if kind not in ('int', 'word', 'bits'):
            return False
        if size == 2:
            return text_pair(a, y)
        if size == 4 and kind == 'word':
            return text_pair(a >> 16, y >> 16) and text_pair(a & 0xffff, y & 0xffff)
        return size == 4 and text_pair(a, y)
    t['textonly'] = bool(changes) and all(text_change(c) for c in changes)


def fmt(v, size, kind):
    if kind == 'float' and size == 4:
        return '%g' % as_float(v)
    if kind == 'float' and size == 8:
        return '%g' % struct.unpack('>d', v.to_bytes(8, 'big'))[0]
    if kind in ('ptr', 'pointer vs NULL'):
        return 'NULL' if v == 0 else 'ptr'
    signed = v - (1 << (size * 8)) if v >> (size * 8 - 1) else v
    s = '0x%0*x (%d)' % (size * 2, v, signed)
    if kind == 'word' and (v >> 23) & 0xff not in (0, 0xff) and 1e-4 < abs(as_float(v)) < 1e7:
        s += ' ~%g' % as_float(v)
    return s


def main():
    args = parse_args()
    img = open(args.image, 'rb').read()
    tables = elf_tables(args.elf, args.min)
    lays = port_layouts(args.port, sorted({t['name'] for t in tables}))
    locate(tables, img)
    for t in tables:
        if t.get('start') is not None:
            diff_table(t, img, lays.get(t['name']))

    # N64 platform tables: libultra, the audio library, video modes, crash
    # text, RDP tasks, texture formats, memory budgets. 4J had no reason to
    # keep these, so a miss or a change there says little about the game.
    platform_re = re.compile(r'^(os|__os|g_Os|n_|argv$|sintable$|g_Crash|g_Rdp|g_TexFormat|g_StageAllocations'
                             r'|SMALLROOM|BIGROOM|g_BitRateTable|g_SampleRateTable|filenames$)')

    def platform(t):
        return t['section'] == '.lib' or bool(platform_re.match(t['name']))

    def by_changes(ts):
        return sorted(ts, key=lambda t: -len(t['changes']))

    located = [t for t in tables if t.get('start') is not None]
    sure = [t for t in located if t['score'] >= 0.5]
    unsure = by_changes(t for t in located if t['score'] < 0.5)
    missing = [t for t in tables if t.get('start') is None]
    same = [t for t in sure if not t['changes']]
    textonly = [t for t in sure if t['textonly']]
    changed = [t for t in sure if t['changes'] and not t['textonly']]
    menus = by_changes(t for t in changed if t['name'].endswith('MenuItems'))
    plat = by_changes(t for t in changed if platform(t) and not t['name'].endswith('MenuItems'))
    game = by_changes(t for t in changed if not platform(t) and not t['name'].endswith('MenuItems'))

    # The 360 linker orders data its own way, so the distance between two
    # tables says nothing; what does is whether the game wrote a table at
    # startup, which the same run read twice shows.
    late = open(args.late, 'rb').read() if args.late and os.path.exists(args.late) and args.late != args.image else None
    for t in located:
        s = t['start']
        t['startup'] = bool(late) and img[s:s + t['size']] != late[s:s + t['size']]

    def table_line(t):
        return '  %-36s %4d %6d %-14s %08x -> %08x  %-26s %5.1f%%  %s%s%s' % (
            t['name'], len(t['changes']), t['size'], t['section'], t['addr'], IMAGE_BASE + t['start'],
            t['how'], t['score'] * 100, t['layout'], '  +%d float rounds' % t['rounds'] if t['rounds'] else '',
            '  [written at startup]' if t['startup'] else '')

    out = []
    p = out.append
    p('XBLA table diff: %d N64 tables >= %d bytes; image %s%s' % (
        len(tables), args.min, os.path.basename(args.image),
        ', startup writes from %s' % os.path.basename(args.late) if late else ' (no late image: startup writes not told apart)'))
    p('  %d identical (pointers aside); changed: %d game data, %d menus, %d N64 platform; %d only in text ids;'
      % (len(same), len(game), len(menus), len(plat), len(textonly)))
    p('  %d placed with under 50%% agreement; %d not located (%d of them N64 platform); %d tables written at startup'
      % (len(unsure), len(missing), sum(platform(t) for t in missing), sum(t['startup'] for t in located)))
    p('  compared by fields where the port\'s layout fits the N64 table, else by words')
    header = '  (changes, bytes, section, N64 -> release, how located, match, compared by)'
    for title, ts in (('CHANGED GAME DATA', game), ('CHANGED MENUS', menus), ('CHANGED N64 PLATFORM TABLES', plat),
                      ('PLACED WITH UNDER 50% AGREEMENT (likely wrong place; details below are unreliable)', unsure)):
        p('')
        p('== ' + title)
        p(header)
        for t in ts:
            p(table_line(t))
    p('')
    p('== CHANGED ONLY IN TEXT IDS (same language bank, a few ids apart: the release\'s strings moved)')
    p('  ' + ' '.join('%s(%d)' % (t['name'], len(t['changes'])) for t in textonly))
    p('')
    p('== NOT LOCATED')
    for t in sorted(missing, key=platform):
        p('  %-36s %6d bytes %-14s %s%s' % (t['name'], t['size'], t['section'], t.get('why', ''),
                                            '  [N64 platform]' if platform(t) else ''))
    p('')
    p('== DETAILS')
    for t in game + menus + plat + unsure + textonly:
        p('')
        p('-- %s  (%s, %d bytes, N64 %08x, release %08x, %s, match %.1f%%, %s)' % (
            t['name'], t['section'], t['size'], t['addr'], IMAGE_BASE + t['start'], t['how'], t['score'] * 100, t['layout']))
        for off, size, path, a, y, kind in t['changes'][:args.max_changes]:
            p('   +0x%04x %-44s %-26s -> %s' % (off, path or '', fmt(a, size, kind), fmt(y, size, kind)))
        if len(t['changes']) > args.max_changes:
            p('   ... %d more' % (len(t['changes']) - args.max_changes))
    p('')
    p('== IDENTICAL')
    p('  ' + ' '.join(t['name'] for t in same))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    open(args.out, 'w').write('\n'.join(out) + '\n')
    print('\n'.join(out[:3]))
    print('report: %s' % args.out)


if __name__ == '__main__':
    main()

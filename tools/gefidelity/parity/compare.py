#!/usr/bin/env python3
"""Compare two GE Plus conversions file by file: the Python converter's
(tools/geconvert/geconvert.py) against the C one's (port/src/geconvert.c).

    compare.py A B [--allow FILE] [--json OUT] [--quiet]
    compare.py --null A B          the control alone

A file is the SAME when its bytes are, or when both are 1173-compressed
(rzip1173: 11 73, a 24-bit length, raw deflate) and the inflated bytes are -
two zlib builds may compress one input differently, and the game only ever
reads the inflated bytes. A bg .seg file is parsed (header, compressed
primary, compressed rooms, sections 2 and 3) and compared part by part, with
the room pointers in the primary's room table taken as room ordinals, since
they move with the compressed sizes. Everything else is DIFFERENT, with the
first differing offset (in the inflated bytes where it is compressed) or
line, or ONLY-A / ONLY-B.

Exit 0 only when nothing differs and every one-sided file is in --allow
(default: allow.txt beside this script). Every run first proves it can see a
difference (the null): one identical raw file and one identical compressed
file are copied into a scratch pair of trees, one byte is flipped in the
copy - in the inflated bytes of the compressed one, recompressed - and the
tree comparison must report both. A null that passes nothing exits 3.
"""
import json, os, shutil, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SEG = 0x0f000000


def is1173(b):
    return len(b) >= 5 and b[:2] == b'\x11\x73'


def inflate1173(b, at=0):
    """(inflated bytes, bytes consumed) of a 1173 block at `at`."""
    n = int.from_bytes(b[at + 2:at + 5], 'big')
    d = zlib.decompressobj(-15)
    out = d.decompress(b[at + 5:])
    used = len(b) - at - len(d.unused_data)
    if len(out) != n:
        raise ValueError('1173 block at %#x inflates to %d bytes, header says %d' % (at, len(out), n))
    return out, used


def rzip1173(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    n = len(data)
    return b'\x11\x73' + bytes([(n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff]) + body


def parse_seg(b):
    """A converted bg file (geconvert.py write_bg()) as [(part name, bytes)],
    every compressed part inflated and the room table's pointers made
    ordinals."""
    inf, sec1, primzlen = struct.unpack_from('>III', b, 0)
    primary, used = inflate1173(b, 12)
    if used != primzlen:
        raise ValueError('primary is %d stored bytes, header says %d' % (used, primzlen))
    rooms = []
    o = 12 + primzlen
    end1 = 12 + sec1
    while o < end1 and is1173(b[o:o + 5]):
        r, used = inflate1173(b, o)
        rooms.append(r)
        o += used
    parts = []
    # the room table: 20-byte rows {ptr, centre, two bytes, pad} from primary +4's
    # address; a row's ptr is SEG + inf + the compressed rooms before it
    primary = bytearray(primary)
    table_at = struct.unpack_from('>I', primary, 4)[0] - SEG
    if 0 < table_at < len(primary):
        ptrs = []
        k = 0
        while table_at + 20 * k + 20 <= len(primary):
            p = struct.unpack_from('>I', primary, table_at + 20 * k)[0]
            if k > 0 and p < SEG + inf:
                break
            ptrs.append((k, p))
            k += 1
            if k > len(rooms) + 2:
                break
        order = {p: i for i, p in enumerate(sorted(set(p for _, p in ptrs if p >= SEG + inf)))}
        for k, p in ptrs:
            if p in order:
                struct.pack_into('>I', primary, table_at + 20 * k, 0xee000000 | order[p])
    parts.append(('header.inf', struct.pack('>I', inf)))
    parts.append(('primary', bytes(primary)))
    for i, r in enumerate(rooms, 1):
        parts.append(('room%d' % i, r))
    o = end1
    for name in ('section2', 'section3'):
        if o + 4 > len(b):
            break
        ln, zlen = struct.unpack_from('>HH', b, o)
        d, used = inflate1173(b, o + 4)
        parts.append((name, struct.pack('>H', ln) + d))
        o += 4 + zlen
    return parts


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else None


def compare_bytes(name, a, b):
    """None when the same, else a one-line description of the first difference."""
    if a == b:
        return None
    if name.endswith('.seg'):
        try:
            pa, pb = parse_seg(a), parse_seg(b)
        except Exception as e:
            pa = pb = None
            why = 'unparsed (%s)' % e
        if pa is not None:
            na, nb = [p[0] for p in pa], [p[0] for p in pb]
            if na != nb:
                return 'parts differ: %d vs %d (%s ... / %s ...)' % (len(na), len(nb), na[-1], nb[-1])
            for (part, x), (_, y) in zip(pa, pb):
                at = first_diff(x, y)
                if at is not None:
                    return '%s differs at inflated %#x (%s vs %s; %d vs %d bytes)' % (
                        part, at, x[at:at + 4].hex() or '-', y[at:at + 4].hex() or '-', len(x), len(y))
            return None
        at = first_diff(a, b)
        return 'raw differs at %#x (%s)' % (at, why)
    if is1173(a) and is1173(b):
        try:
            x, _ = inflate1173(a)
            y, _ = inflate1173(b)
        except Exception as e:
            return 'raw differs at %#x (inflate failed: %s)' % (first_diff(a, b), e)
        at = first_diff(x, y)
        if at is None:
            return None
        return 'inflated differs at %#x (%s vs %s; %d vs %d bytes)' % (
            at, x[at:at + 4].hex() or '-', y[at:at + 4].hex() or '-', len(x), len(y))
    if name.endswith('.txt'):
        la, lb = a.decode('latin-1').splitlines(), b.decode('latin-1').splitlines()
        for i, (x, y) in enumerate(zip(la, lb)):
            if x != y:
                c = first_diff(x, y)
                s = max(0, c - 30)
                return 'line %d col %d: ...%r vs ...%r' % (i + 1, c + 1, x[s:c + 50], y[s:c + 50])
        return 'line %d: one side ends (%d vs %d lines)' % (min(len(la), len(lb)) + 1, len(la), len(lb))
    at = first_diff(a, b)
    return 'raw differs at %#x (%s vs %s; %d vs %d bytes)' % (at, a[at:at + 4].hex() or '-',
                                                             b[at:at + 4].hex() or '-', len(a), len(b))


def walk(root):
    out = set()
    for d, _, fs in os.walk(root):
        for f in fs:
            out.add(os.path.relpath(os.path.join(d, f), root))
    return out


def compare_trees(A, B):
    fa, fb = walk(A), walk(B)
    res = dict(same=0, recompressed=0, different={}, only_a=sorted(fa - fb), only_b=sorted(fb - fa))
    for f in sorted(fa & fb):
        a = open(os.path.join(A, f), 'rb').read()
        b = open(os.path.join(B, f), 'rb').read()
        why = compare_bytes(f, a, b)
        if why is None:
            res['same' if a == b else 'recompressed'] += 1
        else:
            res['different'][f] = why
    return res


def load_allow(path):
    allow = {}
    if path and os.path.exists(path):
        for line in open(path):
            line = line.split('#', 1)[0].strip()
            if line:
                side, name = line.split(None, 1)
                allow[name] = side
    return allow


def null(A, B, scratch):
    """The control: a one-byte flip in a copy must be reported, raw and inflated."""
    same = sorted(f for f in walk(A) & walk(B)
                  if open(os.path.join(A, f), 'rb').read() == open(os.path.join(B, f), 'rb').read())
    raw = next((f for f in same if not is1173(open(os.path.join(A, f), 'rb').read(5))
                and not f.endswith('.seg') and os.path.getsize(os.path.join(A, f)) > 16), None)
    comp = next((f for f in same if is1173(open(os.path.join(A, f), 'rb').read(5))), None)
    if not raw or not comp:
        return False, 'null: no identical raw and compressed file to perturb (raw %s, compressed %s)' % (raw, comp)
    ca, cb = os.path.join(scratch, 'nullA'), os.path.join(scratch, 'nullB')
    shutil.rmtree(scratch, ignore_errors=True)
    for f in (raw, comp):
        for root in (ca, cb):
            os.makedirs(os.path.dirname(os.path.join(root, f)), exist_ok=True)
            shutil.copyfile(os.path.join(A, f), os.path.join(root, f))
    b = bytearray(open(os.path.join(cb, raw), 'rb').read())
    b[len(b) // 2] ^= 0x01
    open(os.path.join(cb, raw), 'wb').write(b)
    d = bytearray(inflate1173(open(os.path.join(cb, comp), 'rb').read())[0])
    d[len(d) // 2] ^= 0x01
    open(os.path.join(cb, comp), 'wb').write(rzip1173(bytes(d)))
    res = compare_trees(ca, cb)
    ok = raw in res['different'] and comp in res['different'] and res['same'] == 0
    shutil.rmtree(scratch, ignore_errors=True)
    return ok, 'null: flipped %s (raw) and %s (inflated) -> %s' % (
        raw, comp, 'both reported' if ok else 'NOT REPORTED: %s' % res)


def main():
    args = sys.argv[1:]
    js = allowpath = None
    quiet = False
    if '--json' in args:
        i = args.index('--json'); js = args[i + 1]; del args[i:i + 2]
    if '--allow' in args:
        i = args.index('--allow'); allowpath = args[i + 1]; del args[i:i + 2]
    else:
        allowpath = os.path.join(HERE, 'allow.txt')
    if '--quiet' in args:
        args.remove('--quiet'); quiet = True
    nullonly = '--null' in args
    if nullonly:
        args.remove('--null')
    A, B = args
    scratch = os.path.join(os.path.dirname(os.path.abspath(B.rstrip('/'))), 'parity-null')
    ok, msg = null(A, B, scratch)
    print(msg)
    if not ok:
        print('PARITY NULL FAILED: the comparison cannot see a one-byte change; no verdict')
        return 3
    if nullonly:
        return 0
    res = compare_trees(A, B)
    allow = load_allow(allowpath)
    only_a = [f for f in res['only_a'] if allow.get(f) != 'A']
    only_b = [f for f in res['only_b'] if allow.get(f) != 'B']
    print('compared %d files: %d identical, %d the same inflated, %d different; only A %d, only B %d (%d allowed)' % (
        res['same'] + res['recompressed'] + len(res['different']), res['same'], res['recompressed'],
        len(res['different']), len(res['only_a']), len(res['only_b']),
        len(res['only_a']) + len(res['only_b']) - len(only_a) - len(only_b)))
    lim = 40 if quiet else 10 ** 9
    for f, why in list(res['different'].items())[:lim]:
        print('DIFFERENT %s: %s' % (f, why))
    for f in only_a[:lim]:
        print('ONLY-A %s' % f)
    for f in only_b[:lim]:
        print('ONLY-B %s' % f)
    if js:
        json.dump(dict(res, only_a_unallowed=only_a, only_b_unallowed=only_b), open(js, 'w'), indent=1)
    bad = len(res['different']) + len(only_a) + len(only_b)
    print('PARITY %s' % ('IDENTICAL' if not bad else 'DIFFERS (%d)' % bad))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())

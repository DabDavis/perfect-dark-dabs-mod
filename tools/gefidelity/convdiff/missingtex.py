#!/usr/bin/env python3
"""Every texture a converted model's display lists load (0xc0 commands, both
tiles) that its mod's textures/ does not have - a picture the game will draw
with whatever texture it last had (Goldfinger 64's Dink: a head drawn as a
pale box).

    missingtex.py MODDIR [--null GEDIR]

The null (--null, the GoldenEye conversion's dir): GoldenEye's own conversion
must come out with nothing missing, and with one texture a model there loads
taken out of the set the scan believes present, exactly the models loading it
must come back. Exit 2 when the null fails.
"""
import argparse, os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdmodel as P


def dls(m):
    root = struct.unpack_from('>I', m, 0)[0]
    out = []
    for depth, addr, typ, name, extra in P.walk(m, root):
        rod = struct.unpack_from('>I', m, addr - P.BASE + 4)[0]
        if (typ & 0xff) in (0x18, 0x04) and rod:
            for k in range(2):
                a = struct.unpack_from('>I', m, rod - P.BASE + 4 * k)[0]
                if a and (a >> 24) == 5:
                    out.append(a)
    return out


def textures_of(m):
    got = set()
    for a in dls(m):
        o = a - P.BASE
        for i in range(4000):
            if o + 8 * i + 8 > len(m):
                break
            w0, w1 = struct.unpack_from('>II', m, o + 8 * i)
            if (w0 >> 24) == 0xc0:
                got.add(w1 & 0xfff)
                if (w0 & 7) == 1:
                    got.add((w1 >> 12) & 0xfff)
            if (w0 >> 24) == 0xb8:
                break
    return got


def scan(mod, drop=None):
    have = {int(f[:4], 16) for f in os.listdir(os.path.join(mod, 'textures')) if f.endswith('.bin')}
    if drop is not None:
        have.discard(drop)
    uses, bad = {}, {}
    for f in sorted(os.listdir(os.path.join(mod, 'files'))):
        p = os.path.join(mod, 'files', f)
        if not os.path.isfile(p):
            continue
        try:
            m = P.load(p)
            if len(m) < 32 or struct.unpack_from('>I', m, 0)[0] >> 24 != 5:
                continue
            t = textures_of(m)
        except Exception:
            continue
        uses[f] = t
        miss = sorted(x for x in t if x not in have)
        if miss:
            bad[f] = miss
    return bad, uses


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mod')
    ap.add_argument('--null', help="the GoldenEye conversion's mod dir")
    a = ap.parse_args()
    if a.null:
        clean, uses = scan(a.null)
        some = next((t for f in sorted(uses) for t in sorted(uses[f])), None)
        planted, _ = scan(a.null, drop=some)
        want = {f for f, t in uses.items() if some in t}
        if clean or set(planted) != want:
            print('null FAILED: clean copy %d missing, planted %04x found in %d of %d models' % (
                len(clean), some or 0, len(set(planted) & want), len(want)), file=sys.stderr)
            return 2
        print('null: the GoldenEye conversion is clean and a planted gap (%04x) is found in all %d models '
              'loading it' % (some, len(want)), file=sys.stderr)
    bad, _ = scan(a.mod)
    for f, miss in bad.items():
        print(f, ' '.join('%04x' % t for t in miss))
    print('%d model files load textures the mod does not have' % len(bad), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())

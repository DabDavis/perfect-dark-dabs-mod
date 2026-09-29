#!/usr/bin/env python3
"""
How far the XBLA release's drawn level surfaces sit from the collision tiles.

Decals (blood, bullet holes) are laid on the collision tile a shot or a body
hits (the bg_*_tilesZ file), so they sit flush only where the drawn room
triangles lie in that tile's plane. The release rewrote the rooms with 2-4x the
triangles but ships the ROM's tiles file. This measures the gap:

  for sample points on every collision tile (type 0, integer tiles), cast a
  ray along the tile's normal both ways and find the nearest drawn triangle
  that is roughly parallel (|n.n'| > 0.9) within --reach units; report the
  signed distance (+ = drawn surface in front of the tile, i.e. on the side
  the tile faces, which hides a decal laid on the tile; - = drawn surface
  behind it, the decal floats in front).

Done for the ROM's own rooms (the baseline, should be ~0) and the release's.

Inputs are the directories used by ~/.cache/claude-xblastage:
  --rom-bg    the ROM's bg_*.seg (sections compressed)
  --rom-tiles the ROM's bg_*_tiles.bin (inflated)
  --rel       the release's PackedSegFile records dumped as <fileid>.bin
Usage: colgap.py bg_sho [bg_pete ...]
"""

import argparse, os, re, struct, sys, zlib
import numpy as np

SEG = 0x0f000000


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def inflate(buf, off):
    if buf[off:off + 2] != b'\x11\x73':
        return None
    size = (buf[off + 2] << 16) | (buf[off + 3] << 8) | buf[off + 4]
    return zlib.decompressobj(-15).decompress(buf[off + 5:], size)


def rooms(data):
    """[(roomnum, roombase, roombytes, pos)] as bgLoadRoom() finds them."""
    infsize, _s1, primcmp = struct.unpack_from('>3I', data, 0)
    primary = inflate(data, 0xc)
    if primary is None:
        primary = data[0xc:0xc + primcmp]
    table = be32(primary, 4) - SEG
    ents = []
    i = 0
    while True:
        base = be32(primary, table + i * 20)
        pos = struct.unpack_from('>3f', primary, table + i * 20 + 4)
        ents.append((base, pos))
        if i >= 1 and base == 0:
            break
        i += 1
    skew = infsize - primcmp - 0xc
    out = []
    for r in range(1, len(ents) - 1):
        base, nxt = ents[r][0], ents[r + 1][0]
        if not base or not nxt or nxt <= base:
            continue
        off = (base - SEG) - skew
        room = inflate(data, off) or data[off:off + (nxt - base)]
        out.append((r, base, room, ents[r][1]))
    return out


def room_tris(room, base, pos, out, rid):
    """World triangles of every leaf block's list (opa and xlu alike)."""
    vtx0 = be32(room, 0)
    if vtx0 == 0:
        return
    end = vtx0 - base
    p = 0x18
    leaves = []
    while p + 20 <= min(end, len(room)):
        bt = room[p]
        gdl = be32(room, p + 8)
        vt = be32(room, p + 12)
        if bt == 1:
            if vt - base < end:
                end = vt - base
        elif bt == 0 and gdl:
            leaves.append((gdl - base, vt - base))
        p += 20
    for g, vbase in leaves:
        cache = [None] * 16
        while 0 <= g and g + 8 <= len(room):
            c = room[g]
            w0 = be32(room, g)
            w1 = be32(room, g + 4)
            if c == 0xb8:
                break
            if c == 0x04:
                n = ((w0 >> 20) & 0xf) + 1
                v0 = (w0 >> 16) & 0xf
                a = vbase + (w1 & 0xffffff)
                for k in range(n):
                    o = a + k * 12
                    if o + 6 <= len(room) and v0 + k < 16:
                        x, y, z = struct.unpack_from('>3h', room, o)
                        cache[v0 + k] = (x + pos[0], y + pos[1], z + pos[2])
            elif c == 0xb1:
                for k in range(4):
                    x = (w1 >> (8 * k)) & 0xf
                    y = (w1 >> (8 * k + 4)) & 0xf
                    z = (w0 >> (4 * k)) & 0xf
                    if x or y or z:
                        t = (cache[x], cache[y], cache[z])
                        if None not in t:
                            out.append(t + (rid,))
            elif c == 0xbf:
                t = (cache[((w1 >> 16) & 0xff) // 10], cache[((w1 >> 8) & 0xff) // 10], cache[(w1 & 0xff) // 10])
                if None not in t:
                    out.append(t + (rid,))
            g += 8


def level_tris(data):
    out = []
    for r, base, room, pos in rooms(data):
        try:
            room_tris(room, base, pos, out, r)
        except Exception as e:
            print('  room %d: %s' % (r, e), file=sys.stderr)
    a = np.array([[t[0], t[1], t[2]] for t in out], dtype=np.float64)
    rid = np.array([t[3] for t in out], dtype=np.int32)
    return a, rid


def tiles(data):
    """[(roomnum, flags, polygon Nx3)] for every integer tile."""
    n = be32(data, 0)
    offs = struct.unpack_from('>%dI' % (n + 1), data, 4)
    out = []
    for r in range(n):
        p, end = offs[r], offs[r + 1]
        while p < end:
            typ, nv, flags = data[p], data[p + 1], struct.unpack_from('>H', data, p + 2)[0]
            if typ == 0:
                v = np.array(struct.unpack_from('>%dh' % (nv * 3), data, p + 14), dtype=np.float64).reshape(nv, 3)
                out.append((r, flags, v))
                p += 14 + nv * 6
            elif typ == 1:
                p += 16 + nv * 12
            elif typ == 2:
                p += 12 + 64
            elif typ == 3:
                p += 24
            else:
                raise ValueError('geo type %d at %x' % (typ, p))
    return out


class Grid:
    def __init__(self, tris, cell=200.0):
        self.t = tris
        self.cell = cell
        e1 = tris[:, 1] - tris[:, 0]
        e2 = tris[:, 2] - tris[:, 0]
        n = np.cross(e1, e2)
        ln = np.linalg.norm(n, axis=1)
        ok = ln > 1e-6
        self.n = np.zeros_like(n)
        self.n[ok] = n[ok] / ln[ok, None]
        self.ok = ok
        lo = np.floor(tris.min(axis=1) / cell).astype(int)
        hi = np.floor(tris.max(axis=1) / cell).astype(int)
        self.cells = {}
        for i in np.nonzero(ok)[0]:
            for x in range(lo[i, 0], hi[i, 0] + 1):
                for y in range(lo[i, 1], hi[i, 1] + 1):
                    for z in range(lo[i, 2], hi[i, 2] + 1):
                        self.cells.setdefault((x, y, z), []).append(i)

    def near(self, p, reach):
        c = self.cell
        lo = np.floor((p - reach) / c).astype(int)
        hi = np.floor((p + reach) / c).astype(int)
        s = set()
        for x in range(lo[0], hi[0] + 1):
            for y in range(lo[1], hi[1] + 1):
                for z in range(lo[2], hi[2] + 1):
                    s.update(self.cells.get((x, y, z), ()))
        return np.fromiter(s, dtype=np.int64)

    def gap(self, p, d, reach, par=0.9):
        """Signed distance along d to the nearest parallel triangle, or None."""
        idx = self.near(p, reach)
        if not len(idx):
            return None
        n = self.n[idx]
        par_ok = np.abs(n @ d) > par
        idx = idx[par_ok]
        if not len(idx):
            return None
        t = self.t[idx]
        e1 = t[:, 1] - t[:, 0]
        e2 = t[:, 2] - t[:, 0]
        h = np.cross(np.broadcast_to(d, e2.shape), e2)
        a = np.einsum('ij,ij->i', e1, h)
        f = 1.0 / a
        s = p - t[:, 0]
        u = f * np.einsum('ij,ij->i', s, h)
        q = np.cross(s, e1)
        v = f * (q @ d)
        tt = f * np.einsum('ij,ij->i', e2, q)
        eps = 1e-4
        hit = (u >= -eps) & (v >= -eps) & (u + v <= 1 + eps) & (np.abs(tt) <= reach)
        if not hit.any():
            return None
        tt = tt[hit]
        return tt[np.argmin(np.abs(tt))]


def samples(poly, rng, k):
    """k points inside a convex tile polygon (fan), inset from the edges."""
    c = poly.mean(axis=0)
    out = [c]
    m = len(poly)
    for _ in range(k - 1):
        j = rng.integers(0, m)
        a, b = poly[j], poly[(j + 1) % m]
        w = rng.dirichlet([1, 1, 1]) * 0.9 + np.array([0.1, 0, 0])
        w /= w.sum()
        out.append(w[0] * c + w[1] * a + w[2] * b)
    return out


def main():
    ap = argparse.ArgumentParser()
    home = os.path.expanduser('~/.cache/claude-xblastage')
    ap.add_argument('--rom-bg', default=home + '/rom/src/assets/ntsc-final/files/bgdata')
    ap.add_argument('--rom-tiles', default=home + '/rom/extracted/ntsc-final/files/bgdata')
    ap.add_argument('--rel', default=home + '/rel')
    ap.add_argument('--ids', default=home + '/rom/ids.txt')
    ap.add_argument('--reach', type=float, default=40.0)
    ap.add_argument('--per-tile', type=int, default=3)
    ap.add_argument('--max-tiles', type=int, default=6000)
    ap.add_argument('--hist', action='store_true')
    ap.add_argument('levels', nargs='+')
    args = ap.parse_args()
    ids = {}
    for line in open(args.ids):
        p = line.split(None, 1)
        if len(p) == 2:
            ids[p[1].strip()] = int(p[0])
    rng = np.random.default_rng(1)
    for lv in args.levels:
        rom = open(os.path.join(args.rom_bg, lv + '.seg'), 'rb').read()
        rel = open(os.path.join(args.rel, '%d.bin' % ids['bgdata/%s.seg' % lv]), 'rb').read()
        til = open(os.path.join(args.rom_tiles, lv + '_tiles.bin'), 'rb').read()
        romt, _ = level_tris(rom)
        relt, _ = level_tris(rel)
        tl = tiles(til)
        print('%s: %d ROM tris, %d release tris, %d collision tiles' % (lv, len(romt), len(relt), len(tl)))
        grids = {'ROM': Grid(romt), 'REL': Grid(relt)}
        order = rng.permutation(len(tl))[:args.max_tiles]
        res = {'ROM': {'floor': [], 'wall': []}, 'REL': {'floor': [], 'wall': []}}
        miss = {'ROM': 0, 'REL': 0}
        total = 0
        for ti in order:
            r, flags, poly = tl[ti]
            if len(poly) < 3:
                continue
            n = np.cross(poly[1] - poly[0], poly[2] - poly[0])
            # use Newell's normal for robustness
            nn = np.zeros(3)
            for j in range(len(poly)):
                a, b = poly[j], poly[(j + 1) % len(poly)]
                nn += np.array([(a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]), (a[0] - b[0]) * (a[1] + b[1])])
            ln = np.linalg.norm(nn)
            if ln < 1e-6:
                continue
            nn /= ln
            kind = 'floor' if abs(nn[1]) > 0.7 else 'wall'
            # a floor faces up; orient walls however the tile is wound
            if kind == 'floor' and nn[1] < 0:
                nn = -nn
            for p in samples(poly, rng, args.per_tile):
                total += 1
                for k, g in grids.items():
                    d = g.gap(p, nn, args.reach)
                    if d is None:
                        miss[k] += 1
                    else:
                        res[k][kind].append(d)
        for k in ('ROM', 'REL'):
            print('  %s: %d samples, %d with no parallel drawn surface within %g' % (k, total, miss[k], args.reach))
            for kind in ('floor', 'wall'):
                a = np.abs(np.array(res[k][kind]))
                if not len(a):
                    continue
                sgn = np.array(res[k][kind])
                print('    %-5s n=%5d  |gap| median %.2f  p90 %.2f  p99 %.2f  max %.1f   >0.5: %4.1f%%  >2: %4.1f%%  >5: %4.1f%%  >10: %4.1f%%   drawn-in-front(>0.5) %4.1f%% behind(<-0.5) %4.1f%%' % (
                    kind, len(a), np.median(a), np.percentile(a, 90), np.percentile(a, 99), a.max(),
                    100 * (a > 0.5).mean(), 100 * (a > 2).mean(), 100 * (a > 5).mean(), 100 * (a > 10).mean(),
                    100 * (sgn > 0.5).mean(), 100 * (sgn < -0.5).mean()))
                if args.hist and k == 'REL':
                    h, e = np.histogram(sgn, bins=[-40, -20, -10, -5, -2, -1, -0.5, 0.5, 1, 2, 5, 10, 20, 40])
                    print('      ' + '  '.join('[%g,%g):%d' % (e[i], e[i + 1], h[i]) for i in range(len(h))))


if __name__ == '__main__':
    main()

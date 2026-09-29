#!/usr/bin/env python3
"""
How often a flat mark laid on a drawn triangle (the way every bg wall hit is:
bgTestHitInRoom() hits a room's vertex batch, the mark is a quad on that
triangle's plane) would cross a crease into a surface that leaves the plane.

Samples points on drawn triangles (area-weighted), then for a ring of points at
radius R around each in the triangle's plane, casts along -n from +H and
measures how far the drawn surface there is from the plane. A deviation over
the edge clip's tolerance (~1.5 + 0.03*size units) is a straight cut with Clip
Decals at Edges on, and a buried/z-fighting part with it off.
Usage: creases.py bg_sho [bg_pete ...]   (same inputs as colgap.py)
"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import colgap as cg


def measure(T, R, n_samples, rng, H=30.0):
    G = cg.Grid(T)
    e1 = T[:, 1] - T[:, 0]; e2 = T[:, 2] - T[:, 0]
    area = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
    p = area / area.sum()
    out = {'floor': [], 'wall': []}
    for i in rng.choice(len(T), n_samples, p=p):
        n = G.n[i]
        if not G.ok[i]:
            continue
        w = rng.dirichlet([1, 1, 1])
        c = w @ T[i]
        a = np.cross(n, [0, 1, 0] if abs(n[1]) < 0.9 else [1, 0, 0]); a /= np.linalg.norm(a)
        b = np.cross(n, a)
        worst = 0.0
        for k in range(8):
            ang = k * np.pi / 4
            q = c + R * (np.cos(ang) * a + np.sin(ang) * b) + H * n
            d = G.gap(q, -n, H + 40, par=0.5)
            if d is None:      # nothing under: an edge/drop, not a crease
                continue
            dev = abs(d - H) if d > 0 else abs(-d + H)
            worst = max(worst, min(dev, 40))
        out['floor' if abs(n[1]) > 0.7 else 'wall'].append(worst)
    return out


def main():
    home = os.path.expanduser('~/.cache/claude-xblastage')
    ids = {}
    for line in open(home + '/rom/ids.txt'):
        s = line.split(None, 1)
        if len(s) == 2:
            ids[s[1].strip()] = int(s[0])
    rng = np.random.default_rng(3)
    for lv in sys.argv[1:]:
        rom, _ = cg.level_tris(open(home + '/rom/src/assets/ntsc-final/files/bgdata/%s.seg' % lv, 'rb').read())
        rel, _ = cg.level_tris(open(home + '/rel/%d.bin' % ids['bgdata/%s.seg' % lv], 'rb').read())
        for R in (10.0, 25.0):
            for name, T in (('ROM', rom), ('REL', rel)):
                o = measure(T, R, 1500, rng)
                s = []
                for kind in ('floor', 'wall'):
                    a = np.array(o[kind])
                    tol = 1.5 + 0.03 * R * 2.8
                    s.append('%s n=%d over tol(%.1f) %4.1f%%  >5 %4.1f%%' % (kind, len(a), tol, 100 * (a > tol).mean(), 100 * (a > 5).mean()))
                print('%-8s R=%2d %s  %s' % (lv, R, name, '   '.join(s)))


if __name__ == '__main__':
    main()

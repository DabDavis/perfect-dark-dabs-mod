#!/usr/bin/env python3
"""Decomposes 4J's per-draw world-view-projection (vertex constants c2-c5 as
rows, row-vector convention: clip = [x y z 1] . M) into the world transform the
port needs, for the boot logos. The camera is the N64 title's: vertical fov 46
degrees, 4:3, looking at the origin from z = 4000.

World axes: x right, y up, z towards the camera. For each draw:
    A[i] = [c(2+i).x / P00, c(2+i).y / P11, -c(2+i).w]   model axis i in world
    translation = [c5.x / P00, c5.y / P11, 4000 - c5.w]
Angles assume R = Rx(pitch) . Ry(spin) acting on column vectors, the order the
title turns its cubes in: pitch = atan2(Y.z, Y.y), and
sin(spin) = X.y sin(pitch) - X.z cos(pitch), cos(spin) = X.x.
"""
import sys, os, math, csv
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from drawlog import load

P11 = 1.0 / math.tan(math.radians(23.0))
P00 = P11 / (4.0 / 3.0)
MESHES = {1185: 'red', 600: 'pd', 19620: 'rare', 14868: 'mgs'}


def decompose(vsc):
    rows = vsc[8:24].reshape(4, 4).astype(np.float64)  # c2..c5
    A = np.array([[r[0] / P00, r[1] / P11, -r[3]] for r in rows[:3]])
    t = np.array([rows[3][0] / P00, rows[3][1] / P11, 4000.0 - rows[3][3]])
    scales = np.linalg.norm(A, axis=1)
    R = A / scales[:, None]
    X, Y = R[0], R[1]
    pitch = math.atan2(Y[2], Y[1])
    sin_s = X[1] * math.sin(pitch) - X[2] * math.cos(pitch)
    spin = math.atan2(sin_s, X[0])
    ortho = float(np.abs(R @ R.T - np.eye(3)).max())
    return scales, pitch, spin, t, ortho, R


def main():
    recs = load(sys.argv[1])
    nidx = recs['init'] >> 16
    out = csv.writer(open(sys.argv[2], 'w'))
    out.writerow(['frame', 'mesh', 'sx', 'sy', 'sz', 'pitch_deg', 'spin_deg', 'tx', 'ty', 'tz', 'ortho',
                  'ps0r', 'ps0g', 'ps0b', 'ps0a', 'ps1r', 'ps1g', 'ps1b', 'ps1a', 'blend'])
    seen = set()
    rows = []
    for i in np.nonzero(np.isin(nidx, list(MESHES)))[0]:
        r = recs[i]
        key = (int(r['frame']), MESHES[int(nidx[i])])
        if key in seen:
            continue
        seen.add(key)
        sc, p, s, t, o, R = decompose(r['vsc'])
        ps = r['psc']
        row = [key[0], key[1], *['%.5f' % x for x in sc], '%.3f' % math.degrees(p), '%.3f' % math.degrees(s),
               *['%.2f' % x for x in t], '%.4f' % o, *['%.4f' % x for x in ps[0:8]], '%08x' % r['r22'][1]]
        out.writerow(row)
        rows.append(row)
    print('%d frame/mesh rows' % len(rows))
    step = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    last = {}
    for row in rows:
        fr, mesh = row[0], row[1]
        if mesh not in last or fr - last[mesh] >= step:
            last[mesh] = fr
            print('%5d %-4s s %s %s %s  pitch %8s spin %9s  t %s %s %s  ortho %s  ps0 %s %s %s %s  ps1 %s %s %s %s' % tuple(row[:19]))


if __name__ == '__main__':
    main()

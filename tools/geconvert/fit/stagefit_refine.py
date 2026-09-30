#!/usr/bin/env python3
"""Refines stagetable.json's scales: least squares over every GE-X vertex that
has a Bean HD vertex near it (translation 0, as every fit found), a few rounds
with a shrinking radius. Writes the scale back and the rooms' coverage at 2.5."""
import json, os, sys
import numpy as np
from scipy.spatial import cKDTree
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths
import pdbg
from stagefit import bean_points
GEX = os.path.join(paths.GEX_FILES, 'bgdata/%s.seg')
rows = json.load(open(os.path.join(HERE, 'stagetable.json')))
cache = {}
for r in rows:
    if not r['bean']:
        continue
    v, _ = pdbg.room_vertices(GEX % r['bg'])
    G = np.array([p for vs in v.values() for p in vs], float)
    if r['bean'] not in cache:
        cache[r['bean']] = bean_points('new', r['bean'])
    B = cache[r['bean']]
    s = r['scale']
    for radius in (6, 4, 3, 2.5):
        t = cKDTree(B * s)
        d, i = t.query(G)
        keep = d < radius
        b = B[i[keep]]; g = G[keep]
        s = float(np.sum(b * g) / np.sum(b * b))
    d, _ = cKDTree(B * s).query(G)
    rooms_ok = 0
    for vs in v.values():
        if vs:
            dd, _ = cKDTree(B * s).query(np.array(vs, float)) if False else (None, None)
    print('%-8s %-12s scale %.5f -> %.6f  within 2.5: %.2f' % (r['bg'], r['bean'], r['scale'], s, np.mean(d < 2.5)))
    r['scale'] = s
json.dump(rows, open(os.path.join(HERE, 'stagetable.json'), 'w'), indent=1)

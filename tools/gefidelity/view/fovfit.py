#!/usr/bin/env python3
"""Measure the release's field of view against ours, from matched pairs.

    view/fovfit.py OUT/<mission>          # a view run with --oracle xenia

For each pair whose cameras agree, find the zoom (kx, ky) about the picture's
centre that maps our picture onto Bean's best (correlation of blurred edge
maps, the gun and HUD - the bottom third - left out), in normalised frame
coordinates, so a window that squeezes the release's 1280x720 into 1280x695
shows as ky != kx rather than as a zoom. With our vertical field of view F
(the run's pd.ini) and aspect A (16:9), Bean's is

    fovy_bean = 2 atan(tan(F/2) / ky),   its frame aspect = A * ky / kx.

Prints the median over pairs and writes OUT/<mission>/fovfit.json. Its null:
our picture against itself must fit at (1, 1), and against itself zoomed by a
known 1.10 must fit at 1.10 - or nothing is reported.
"""
import json, math, os, re, sys

import numpy as np
from PIL import Image
from scipy import ndimage

WORK = (320, 180)


def edges(img):
    a = np.asarray(img.convert('L').resize(WORK, Image.BILINEAR), dtype=np.float64)
    b = ndimage.gaussian_filter(a, 1.5)
    return ndimage.gaussian_filter(np.hypot(*np.gradient(b)), 1.5)


def sample(e, kx, ky):
    """e zoomed by (kx, ky) about the centre, on the same grid (nan outside)."""
    h, w = e.shape
    v, u = np.mgrid[0:h, 0:w].astype(np.float64)
    cu, cv = (w - 1) / 2.0, (h - 1) / 2.0
    su, sv = cu + (u - cu) / kx, cv + (v - cv) / ky
    out = ndimage.map_coordinates(e, [sv, su], order=1, cval=np.nan)
    return out


def score(a, b, mask):
    m = mask & ~np.isnan(b)
    if m.sum() < 500:
        return -1.0
    x, y = a[m] - a[m].mean(), b[m] - b[m].mean()
    d = math.sqrt(float((x * x).sum() * (y * y).sum()))
    return float((x * y).sum() / d) if d > 0 else -1.0


def fit(bean_e, ours_e):
    mask = np.zeros(bean_e.shape, bool)
    mask[: int(bean_e.shape[0] * 2 / 3), :] = True          # the gun and HUD are the bottom third
    best = (-2.0, 1.0, 1.0)
    for step, lo, hi, cx, cy in ((0.04, 0.6, 1.6, None, None), (0.01, None, None, 0.05, 0.05)):
        if cx is None:
            ks = np.arange(lo, hi + 1e-9, step)
            grid = [(kx, ky) for kx in ks for ky in ks]
        else:
            grid = [(best[1] + dx, best[2] + dy) for dx in np.arange(-cx, cx + 1e-9, step)
                    for dy in np.arange(-cy, cy + 1e-9, step)]
        for kx, ky in grid:
            s = score(bean_e, sample(ours_e, kx, ky), mask)
            if s > best[0]:
                best = (s, kx, ky)
    return best


def our_fovy(out):
    try:
        for line in open(os.path.join(out, 'pd', 'save', 'pd.ini')):
            m = re.match(r'FovY=([0-9.]+)', line.strip())
            if m:
                return float(m.group(1))
    except OSError:
        pass
    return 60.0


def main():
    out = sys.argv[1]
    sc = json.load(open(os.path.join(out, 'scores.json')))
    rows = [r for r in sc['rows'] if not r['cam']['status'].startswith('mismatch')]
    # the null: our picture against itself, and against itself zoomed 1.10 -
    # on the run's most detailed picture (a flat or striped wall cannot pin a
    # vertical zoom, which is what failed this on Facility's worst pair)
    pe = max((edges(Image.open(os.path.join(out, 'pairs', r['tag'] + '_pd.png'))) for r in rows[:40]),
             key=lambda e: float(np.var(e[: int(e.shape[0] * 2 / 3)])))
    s, kx, ky = fit(pe, pe)
    s2, kx2, ky2 = fit(sample(pe, 1.10, 1.10), pe)
    if abs(kx - 1) > 0.015 or abs(ky - 1) > 0.015 or abs(kx2 - 1.10) > 0.02 or abs(ky2 - 1.10) > 0.02:
        print('NULL FAILED: self fit (%.3f, %.3f), known 1.10 fit (%.3f, %.3f)' % (kx, ky, kx2, ky2))
        return 2
    fits = []
    for r in rows:
        ge = os.path.join(out, 'pairs', r['tag'] + '_ge.png')
        pdp = os.path.join(out, 'pd', 'shot_%03d.png' % next(
            s['n'] for s in json.load(open(os.path.join(out, 'pd', 'manifest.json')))['shots']
            if s['pad'] == r['pad'] and s['head'] == r['head']))
        gfull = os.path.join(out, 'ge', next(
            s['file'] for s in json.load(open(os.path.join(out, 'ge', 'manifest.json')))['shots']
            if s['pad'] == r['pad'] and s['head'] == r['head']))
        s, kx, ky = fit(edges(Image.open(gfull)), edges(Image.open(pdp)))
        if s > 0.3:
            fits.append((kx, ky, s))
    if not fits:
        print('no pair fitted (correlation > 0.3)')
        return 1
    kx = float(np.median([f[0] for f in fits]))
    ky = float(np.median([f[1] for f in fits]))
    F = our_fovy(out)
    fb = 2 * math.degrees(math.atan(math.tan(math.radians(F / 2)) / ky))
    res = {'pairs': len(fits), 'kx': round(kx, 4), 'ky': round(ky, 4), 'our_fovy': F,
           'bean_fovy': round(fb, 2), 'bean_frame_aspect': round(16 / 9 * ky / kx, 4),
           'spread_kx': round(float(np.std([f[0] for f in fits])), 4),
           'spread_ky': round(float(np.std([f[1] for f in fits])), 4)}
    json.dump(res, open(os.path.join(out, 'fovfit.json'), 'w'), indent=1)
    print('null: self fit (1, 1), known zoom 1.10 found; ' + json.dumps(res))
    return 0


if __name__ == '__main__':
    sys.exit(main())

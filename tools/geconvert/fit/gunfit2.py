#!/usr/bin/env python3
"""Lay each Bean pickup on GoldenEye's own N64 pickup, and write gunfit.json.

The N64 pickup's frame is Perfect Dark's, so what comes out of here is what the
game draws in a character's hand and on the floor (gebeanBuildRigid(), through
gen_guntable.py -> port/src/gegunstable.h).

The first cut searched all 48 signed axis permutations with a free scale and
scored by nearest vertex. Two things went wrong with that, and the player saw
both:

  * **Half of the 48 are mirrors.** A mirrored gun fits a gun almost as well as
    the true rotation does - the silhouette is the same - so the score could not
    tell them apart, and eleven pickups came out mirrored, with every asymmetric
    detail on the wrong side.
  * **A free rotation can pick a free axis to scale along.** The AR33 and the
    RC-P90 are the two whose N64 pickups are much fatter than their guns, their
    fits went to a permutation that laid Bean's *length* along GoldenEye's
    width, and the scale that came out - 0.70 and 1.55 against everything else's
    0.21 - drew guns three and seven times their size standing beside the
    player.

So the rotation is not searched at all any more. GoldenEye modelled every
pickup the same way up, and 23 of the 26 free searches agreed on which way that
is (`CANONICAL`): Bean's y is the barrel and GoldenEye's x is, pointing the
same way, z is up in both, and y follows from those and a right hand. The three
that disagreed are nearly symmetric about the axis in question (the rocket
launcher's tube, the grenade, the hunting knife) and disagreed by less than 1%
of the score, which is not a reason to turn a gun round.

The scale is the ratio of the two models' extents along GoldenEye's longest
axis. That is what the old fit's scale worked out to on every gun it got right,
and it is the one measurement of a gun that neither model's stray geometry can
move much: the guns are long, and a clump 10% past the muzzle is a 10% error
where a nearest-vertex score can be 50% out. Only the translation is searched,
by trimmed ICP - trimmed because a part one game modelled and the other did not
should not drag the gun sideways.

Usage: gunfit2.py [--report] ; then gen_guntable.py to write gegunstable.h.
"""
import json, os, re, sys
import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
from bean2obj import Model

HERE = os.path.dirname(os.path.abspath(__file__))
BEAN = os.path.join(paths.BEAN, 'files/new/prop')
GE = os.path.join(paths.GE_DECOMP, 'assets/obseg/prop')
OUT = os.path.join(HERE, 'gunfit.json')

# The share of GoldenEye's vertices the translation is fitted over, worst
# dropped: a part Bean left off would otherwise pull the gun towards it.
TRIM = 0.9

# Axis k of the result is sign[k] * Bean axis perm[k], which is how the port
# reads the pair (gebeanBuildRigid()).
CANONICAL = ((1, 0, 2), (1, -1, 1))

# GoldenEye models its throwing knife 180 degrees from its hunting knife: in
# its own pickups the hunting knife's blade tapers to a point at high z and the
# throwing knife's at low z (slice the vertices along z and watch the cross
# section close). Bean copies that, so the canonical rotation lays both knives
# faithfully - and the throwing knife then hangs handle up, blade down through
# the fist, on a host whose hand holds Perfect Dark's combat knife blade up.
#
# From 2026-09-16 it was turned a half turn here, about the axis named, with
# the translation turned about GoldenEye's own box centre so the knife stayed
# where the fit put it. The ICP cannot find this by itself: a knife flipped end
# for end still lands its points on the other knife's surface.
#
# Not any more (f15515659, 2026-09-25, F3 20260925-032416 on Surface: "throwing
# knife needs to be flipped"). GoldenEye holds its throwing knife by the blade
# with the handle up, ready to throw - the decomp's native port shows it so in
# first person and all through a throw, and the N64 look already drew it that
# way on GoldenEye's own model. The turn held it by the handle, blade up, in
# Perfect Dark's hand. This pickup is also the knife in a character's hand, so
# its row is back to the canonical rotation, which lays it on GoldenEye's
# chrthrowknife blade to blade (7.85 mean distance from GoldenEye's vertices to
# Bean's, against 15.90 turned); in third person the fist is on the blade below
# the guard, where GoldenEye's model puts its origin. The turn is kept as an
# opt-in: HALF_TURN_AXIS=1 gunfit2.py brings the old row back.
HALF_TURN = ({'chrthrowknife': int(os.environ['HALF_TURN_AXIS'])}
             if os.environ.get('HALF_TURN_AXIS') else {})


def bean_points(name):
    m = Model(os.path.join(BEAN, name, 'default.bin'))
    pts = []
    for d in m.draws:
        if d['vb'] is None:
            continue
        verts = m.vertices(d['vb'])
        used = set()
        for t in m.triangles(d):
            used.update(t)
        for i in used:
            if i < len(verts):
                pts.append(verts[i]['pos'])
    return np.array(pts, dtype=np.float64)


def ge_points(name):
    src = re.sub(r'//[^\n]*', '', open(os.path.join(GE, name, 'Model.c')).read())
    pts = []
    for m in re.finditer(r'Vertex\s+\w+\[[^\]]*\]\s*=\s*\{(.*?)\n\};', src, re.S):
        for row in re.finditer(r'\{\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', m.group(1)):
            pts.append([float(x) for x in row.groups()])
    return np.unique(np.array(pts, dtype=np.float64), axis=0)


def rotations():
    """The 24 proper signed axis permutations, as (perm, sign)."""
    out = []
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        for bits in range(8):
            sign = [1 - 2 * ((bits >> k) & 1) for k in range(3)]
            m = np.zeros((3, 3))
            for k in range(3):
                m[k, perm[k]] = sign[k]
            if round(np.linalg.det(m)) == 1:
                out.append((perm, tuple(sign)))
    return out


def rotate(bean, perm, sign):
    rot = np.zeros((3, 3))
    for k in range(3):
        rot[k, perm[k]] = sign[k]
    return bean @ rot.T


def scale_for(rotated, ge):
    """GoldenEye's longest axis, and the two models' extents along it."""
    extent = ge.max(axis=0) - ge.min(axis=0)
    axis = int(np.argmax(extent))
    span = rotated[:, axis].max() - rotated[:, axis].min()
    return extent[axis] / span, axis


def fit_one(rotated, ge, scale):
    """Translation by trimmed ICP, and what it is worth.

    Nearest neighbours are taken in Bean's own units - a uniform scale does not
    reorder them - so the tree is built once.
    """
    tree = cKDTree(rotated)
    keep = max(4, int(len(ge) * TRIM))
    t = (np.percentile(ge, [2, 98], axis=0).mean(axis=0)
            - scale * np.percentile(rotated, [2, 98], axis=0).mean(axis=0))

    for _ in range(20):
        dist, idx = tree.query((ge - t) / scale)
        order = np.argsort(dist)[:keep]
        t = t + (ge[order] - (rotated[idx[order]] * scale + t)).mean(axis=0)

    dist, _ = tree.query((ge - t) / scale)
    return t, np.sort(dist)[:keep].mean() * scale


def main():
    report = '--report' in sys.argv
    perm, sign = CANONICAL
    rows = {}

    for name in sorted(json.load(open(OUT))):
        bean, ge = bean_points(name), ge_points(name)
        best = None

        for p, s in rotations():
            rotated = rotate(bean, p, s)
            scale, _ = scale_for(rotated, ge)
            t, score = fit_one(rotated, ge, scale)

            if (p, s) == (perm, sign):
                chosen = (scale, t, score)
            if best is None or score < best[0]:
                best = (score, p, s, scale)

        scale, t, score = chosen
        rowperm, rowsign = perm, sign

        if name in HALF_TURN:
            axis = HALF_TURN[name]
            f = [-1 if k != axis else 1 for k in range(3)]
            centre = (ge.min(axis=0) + ge.max(axis=0)) * 0.5
            t = [centre[k] - f[k] * centre[k] + f[k] * t[k] for k in range(3)]
            rowsign = tuple(f[k] * sign[k] for k in range(3))

        rows[name] = dict(perm=list(rowperm), signs=list(rowsign), scale=round(float(scale), 5),
                          beancentre=[0.0, 0.0, 0.0], n64centre=[round(float(x), 2) for x in t],
                          meandist=round(float(score), 2))

        if report:
            free = '' if (best[1], best[2]) == (perm, sign) else (
                    '   free search wanted %s %s at %.3f, %.2f' % (
                        list(best[1]), list(best[2]), best[3], best[0]))
            print('%-18s scale %.4f  dist %5.2f%s' % (name, scale, score, free))

    json.dump(rows, open(OUT, 'w'), indent=1)
    print('wrote %s (%d pickups)' % (OUT, len(rows)))


if __name__ == '__main__':
    main()

"""Bean's HD props laid onto the GoldenEye remake's own prop models: for each
prop, how a Bean vertex becomes a model-space one - axis k is sign[k] * Bean
axis perm[k], centred on beancentre, scaled and moved to n64centre - as
gebeanBuildRigid() takes a pickup's (gegunstable.h).

Fitted on Bean's N64-look copy (files/original/prop/<name>), which is
GoldenEye's own geometry in Bean's frame, against GoldenEye's model-space
vertices (each list's vertices plus the origins of the groups over it):
every axis order and sign, the scale from the extents along the longest axis,
refined by nearest points; scored by the share of Bean's points that land on
GoldenEye's. Writes propfit.json.
"""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
import itertools, json, os, re, struct, sys
import numpy as np
from scipy.spatial import cKDTree
from cafftool import Caff
import gefiles, gemodelconv

BEAN = os.path.join(paths.BEAN, 'files')
SEG = 0x05000000


# GoldenEye model numbers whose release prop has another name: the large
# cardboard boxes (card_box4_lg -> cardbox4), the disc reader (disk), Streets'
# road-and-building props (the release puts a p before st_pete_room)
ALIAS = {21: 'cardbox4', 22: 'cardbox5', 23: 'cardbox6', 41: 'diskreader',
         320: 'pstpeteroom1i', 321: 'pstpeteroom2i', 322: 'pstpeteroom3t',
         323: 'pstpeteroom5c', 324: 'pstpeteroom6c'}


def bean_names():
    norm = lambda s: re.sub(r'[^a-z0-9]', '', s.lower())
    return {norm(n): n for n in os.listdir(BEAN + '/new/prop') if not n.endswith('_hits')}, norm


def bean_points(look, name):
    c = Caff('%s/%s/prop/%s/default.bin' % (BEAN, look, name))
    get = lambda a, s: [c.blob(f) for f in c.files if f['asset'] == a and c.sections[f['sect'] - 1]['name'] == s]
    data, gpu, st = get(1, '.data')[0], get(1, '.gpu')[0], get(1, '.stream')[0]
    u = lambda b, o: struct.unpack_from('>I', b, o)[0]
    vbs, pc = set(), 0x24
    while pc < len(st) - 4:
        tag = u(st, pc)
        size, typ = tag >> 16, (tag >> 8) & 0xff
        if size < 4:
            break
        if typ == 0x2e:
            vbs.add(u(st, pc + 8))
        pc += size
    pts = []
    for vb in vbs:
        stride, _, off, sz = struct.unpack_from('>4I', data, vb)
        n = sz // stride if stride else 0
        if n == 0 or off + n * stride > len(gpu):
            continue
        a = np.frombuffer(gpu[off:off + n * stride], dtype=np.uint8).reshape(n, stride)[:, :12].copy().view('>f4').reshape(n, 3).astype(float)
        # Statue Park's props are in its 1248x frame (millions of units) -
        # 1e8, as stagefit's bound for the level (2026-09-27)
        pts.append(a[np.all(np.abs(a) < 1e8, axis=1)])
    return np.concatenate(pts) if pts else np.zeros((0, 3))


def ge_points(name):
    h = gemodelconv.header(name)
    fname, _ = gemodelconv.prop_record(name)
    d = gefiles.rom_file('P%sZ' % fname)
    u = lambda o: struct.unpack_from('>I', d, o)[0]
    root = 4 * h['numswitches'] + 12 * h['numtextures']
    pts = []

    def walk(o, origin):
        while o:
            t = struct.unpack_from('>H', d, o)[0]
            ro = u(o + 4) - SEG
            here = origin
            if t == 0x02 or t == 0x15:
                here = origin + np.array(struct.unpack_from('>3f', d, ro))
            elif t in (0x04, 0x18):
                vtx, n = (u(ro + 12), struct.unpack_from('>H', d, ro + 0x10)[0]) if t == 0x04 else (u(ro + 8), struct.unpack_from('>h', d, ro + 12)[0])
                for k in range(n):
                    pts.append(np.array(struct.unpack_from('>3h', d, vtx - SEG + 16 * k), float) + here)
            child = u(o + 20)
            if child:
                walk(child - SEG, here)
            nxt = u(o + 12)
            o = nxt - SEG if nxt else 0
    walk(root, np.zeros(3))
    return np.array(pts)


def fit(B, G):
    tree = cKDTree(G)
    tol = 0.01 * np.ptp(G, axis=0).max()
    best = None
    gext = np.ptp(G, axis=0)
    gcen = (G.min(0) + G.max(0)) / 2
    for perm in itertools.permutations(range(3)):
        for sign in itertools.product((1, -1), repeat=3):
            Q = B[:, perm] * np.array(sign)
            qext = np.ptp(Q, axis=0)
            ax = int(np.argmax(gext))
            if qext[ax] <= 0:
                continue
            s = gext[ax] / qext[ax]
            qcen = (Q.min(0) + Q.max(0)) / 2
            t = gcen - s * qcen
            for _ in range(8):
                dd, ii = tree.query(Q * s + t)
                keep = dd <= np.percentile(dd, 60)
                A, T = Q[keep], G[ii[keep]]
                ma, mt = A.mean(0), T.mean(0)
                den = np.sum((A - ma) ** 2)
                if den <= 0:
                    break
                s = np.sum((A - ma) * (T - mt)) / den
                t = mt - s * ma
            if s <= 0:
                continue
            placed = Q * s + t
            # both ways, or a scale near nothing piles Bean's points on one of
            # GoldenEye's and scores perfectly
            score = min(float(np.mean(tree.query(placed)[0] < tol)),
                        float(np.mean(cKDTree(placed).query(G)[0] < tol)))
            # the axes as they are, unless another order is clearly better
            if (perm, sign) != ((0, 1, 2), (1, 1, 1)):
                score -= 0.05
            if best is None or score > best['score']:
                best = dict(perm=list(perm), sign=list(sign), scale=float(s), t=t, score=score)
    # A release copy with more vertices than GoldenEye's model (desk1's
    # kickplate split, glassware1's extra rings, 2026-09-27) scores low from
    # Bean's side however exactly it lies, and the refinement above chases
    # the extra points off the fit. The plain fit - axes as they are, one
    # scale that agrees on all three extents to 1%, boxes centred - is taken
    # on GoldenEye's side alone when every GoldenEye vertex lands on Bean's
    # and it beats the refined one.
    qext = np.ptp(B, axis=0)
    if np.all(qext > 0) and np.all(gext > 0):
        ratios = gext / qext
        if np.ptp(ratios) <= 0.01 * ratios.max():
            s = float(ratios[int(np.argmax(gext))])
            t = gcen - s * (B.min(0) + B.max(0)) / 2
            covered = float(np.mean(cKDTree(B * s + t).query(G)[0] < tol))
            if covered >= 0.95 and covered > best['score']:
                best = dict(perm=[0, 1, 2], sign=[1, 1, 1], scale=s, t=t, score=covered, plain=True)
    # as gebeanBuildRigid() takes it: (q - beancentre) * scale + n64centre
    best['beancentre'] = [0.0, 0.0, 0.0]
    best['n64centre'] = [float(x) for x in best.pop('t')]
    return best


if __name__ == '__main__':
    bn, norm = bean_names()
    names = gemodelconv.prop_names()
    want = [int(a) for a in sys.argv[1:]] if len(sys.argv) > 1 else range(len(names))
    OUT = paths.data('propfit.json')
    out = json.load(open(OUT)) if os.path.exists(OUT) else {}
    for i in want:
        b = ALIAS.get(i) or bn.get(norm(names[i]))
        if not b or not os.path.exists('%s/original/prop/%s/default.bin' % (BEAN, b)):
            continue
        try:
            B, G = bean_points('original', b), ge_points(names[i])
        except Exception as e:
            print(i, names[i], 'failed', e)
            continue
        if len(B) < 3 or len(G) < 3:
            continue
        f = fit(B, G)
        f['bean'] = b
        out[str(i)] = f
        print('%3d %-22s %-20s score %.2f scale %.4f perm %s sign %s' % (i, names[i], b, f['score'], f['scale'], f['perm'], f['sign']), flush=True)
    json.dump(out, open(OUT, 'w'), indent=1)

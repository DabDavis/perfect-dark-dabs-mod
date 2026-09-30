"""The HD fist (Bean gun/fist) laid on the ROM's GfistZ, for the Igx001Z row in
port/src/gebean.c: propfit.py's fit on both looks. Prints the fit; the row is
written by hand (n64centre = fit offset minus GfistZ's root position).
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
import numpy as np
import propfit
from propfit import fit, SEG
import gefiles
from cafftool import Caff
BEAN = propfit.BEAN

def bean_pts(look):
    c = Caff('%s/%s/gun/fist/default.bin' % (BEAN, look))
    get = lambda a, s: [c.blob(f) for f in c.files if f['asset'] == a and c.sections[f['sect'] - 1]['name'] == s]
    data, gpu, st = get(1, '.data')[0], get(1, '.gpu')[0], get(1, '.stream')[0]
    u = lambda b, o: struct.unpack_from('>I', b, o)[0]
    vbs, pc = set(), 0x24
    while pc < len(st) - 4:
        tag = u(st, pc); size, typ = tag >> 16, (tag >> 8) & 0xff
        if size < 4: break
        if typ == 0x2e: vbs.add(u(st, pc + 8))
        pc += size
    pts = []
    for vb in vbs:
        stride, _, off, sz = struct.unpack_from('>4I', data, vb)
        n = sz // stride if stride else 0
        a = np.frombuffer(gpu[off:off + n * stride], dtype=np.uint8).reshape(n, stride)[:, :12].copy().view('>f4').reshape(n, 3).astype(float)
        print(look, 'vb stride', stride, 'n', n, a.min(0), a.max(0))
        pts.append(a)
    return np.concatenate(pts)

def ge_pts(nsw, ntex):
    d = gefiles.rom_file('GfistZ')
    u = lambda o: struct.unpack_from('>I', d, o)[0]
    root = 4 * nsw + 12 * ntex
    pts = []
    def walk(o, origin, depth):
        while o:
            t = struct.unpack_from('>H', d, o)[0]
            ro = u(o + 4) - SEG
            here = origin
            if t == 0x02 or t == 0x15:
                here = origin + np.array(struct.unpack_from('>3f', d, ro))
                print('  '*depth, 'pos t%x' % t, struct.unpack_from('>3f', d, ro), 'extra', d[ro+12:ro+24].hex())
            elif t in (0x04, 0x18):
                vtx, n = (u(ro + 12), struct.unpack_from('>H', d, ro + 0x10)[0]) if t == 0x04 else (u(ro + 8), struct.unpack_from('>h', d, ro + 12)[0])
                p = [np.array(struct.unpack_from('>3h', d, vtx - SEG + 16 * k), float) + here for k in range(n)]
                if p:
                    P = np.array(p); print('  '*depth, 'dl t%x n %d' % (t, n), P.min(0), P.max(0))
                pts.extend(p)
            else:
                print('  '*depth, 'node t%x' % t)
            child = u(o + 20)
            if child: walk(child - SEG, here, depth + 1)
            nxt = u(o + 12)
            o = nxt - SEG if nxt else 0
    walk(root, np.zeros(3), 0)
    return np.array(pts)

G = ge_pts(0x24, 0xE)
print('GE', len(G), G.min(0), G.max(0))
for look in ('original', 'new'):
    B = bean_pts(look)
    f = fit(B, G)
    print(look, f)

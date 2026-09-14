#!/usr/bin/env python3
"""The sky's draws in a capture from the Xenia hook's buffer dump.

    skyvb.py DIR index               write DIR/vbindex.npy: one row per .vb record
    skyvb.py DIR frames              frames whose no-depth draws bind a big DXT texture
    skyvb.py DIR dump FRAME [OUT]    those draws in FRAME: textures, indices, vertices

DIR holds draws.bin and draws.bin.vb. The .vb record is 'PDVB', draw index,
frame, index format, index endianness, index byte count, the index bytes, a
binding count and per binding fetch constant, stride in words, word count and
the words - see tools/xblaintro/xenia-drawlog.patch. The file is walked by
seeking, since a capture into gameplay is several gigabytes.
"""
import os
import pickle
import struct
import sys

import numpy as np

sys.path.insert(0, '/home/sdg/perfect-dark/perfect_dark/tools/xblaintro')
import drawlog  # noqa: E402

d, cmd = sys.argv[1], sys.argv[2]
vbpath = d + '/draws.bin.vb'


def build_index():
    rows = []
    size = os.path.getsize(vbpath)
    with open(vbpath, 'rb') as f:
        p = 0
        while p + 24 <= size:
            f.seek(p)
            head = f.read(24)
            magic, draw, frame, fmt, endian, nbytes = struct.unpack('<6I', head)
            if magic != 0x42564450:
                break
            start = p
            p += 24 + nbytes
            f.seek(p)
            b = f.read(4)
            if len(b) < 4:
                break
            nbind = struct.unpack('<I', b)[0]
            p += 4
            for _ in range(nbind):
                f.seek(p)
                b = f.read(12)
                if len(b) < 12:
                    p = size + 1
                    break
                words = struct.unpack('<3I', b)[2]
                p += 12 + words * 4
            if p > size:
                break
            rows.append((draw, frame, start))
    idx = np.array(rows, dtype=np.int64)
    np.save(d + '/vbindex.npy', idx)
    return idx


def textures(r):
    out = []
    for f in sorted(set(int(x) for x in r['psfetch'] if x != 0xff)):
        q = r['fetch'][f * 6:f * 6 + 6]
        out.append(((int(q[2]) & 0x1fff) + 1, ((int(q[2]) >> 13) & 0x1fff) + 1,
                    int(q[1]) & 0x3f, (int(q[1]) >> 12) << 12))
    return out


def is_sky(tex):
    return any(w >= 512 and h >= 512 and fmt in (18, 19, 20) for w, h, fmt, _ in tex)


def read_record(f, start):
    f.seek(start)
    magic, draw, frame, fmt, endian, nbytes = struct.unpack('<6I', f.read(24))
    idx = f.read(nbytes)
    nbind = struct.unpack('<I', f.read(4))[0]
    binds = []
    for _ in range(nbind):
        fetch, stride, words = struct.unpack('<3I', f.read(12))
        binds.append((fetch, stride, f.read(words * 4)))
    ind = np.frombuffer(idx, dtype='>u2' if fmt == 0 else '>u4') if idx else None
    return ind, binds


idx = np.load(d + '/vbindex.npy') if cmd != 'index' and os.path.exists(d + '/vbindex.npy') else None
if cmd == 'index' or idx is None:
    idx = build_index()
    print('%d records, frames %d..%d' % (len(idx), idx[:, 1].min(), idx[:, 1].max()))
    if cmd == 'index':
        sys.exit()

recs = drawlog.load(d + '/draws.bin')
idx = idx[idx[:, 0] < len(recs)]

if cmd == 'frames':
    # Only look at one no-depth draw a frame's first dozen: the sky is drawn first.
    last = -1
    runs = []
    for draw, frame, start in idx:
        if frame == last:
            continue
        tex = textures(recs[draw])
        if is_sky(tex):
            runs.append(frame)
            last = frame
    runs = np.array(runs)
    if len(runs):
        breaks = np.nonzero(np.diff(runs) > 1)[0]
        lo = np.concatenate(([runs[0]], runs[breaks + 1]))
        hi = np.concatenate((runs[breaks], [runs[-1]]))
        for a, b in zip(lo, hi):
            print('frames %d-%d' % (a, b))

elif cmd == 'dump':
    frame = int(sys.argv[3])
    out = {}
    np.set_printoptions(precision=4, suppress=True, linewidth=170)
    with open(vbpath, 'rb') as f:
        for draw, fr, start in idx[idx[:, 1] == frame]:
            r = recs[draw]
            tex = textures(r)
            if not is_sky(tex):
                continue
            ind, binds = read_record(f, start)
            nidx = int(r['init']) >> 16
            print('draw %d n=%d vs=%08x ps=%08x depth=%08x blend=%08x cull=%08x tex %s' % (
                draw, nidx, int(r['vs']) & 0xffffffff, int(r['ps']) & 0xffffffff,
                int(r['r22'][0]), int(r['r22'][1]), int(r['r22'][5]),
                ['%dx%d fmt%d @%08x' % t for t in tex]))
            print('   indices', None if ind is None else ind[:24].tolist())
            verts = []
            for fetch, stride, data in binds:
                v = np.frombuffer(data, dtype='>f4').reshape(-1, stride)
                verts.append((fetch, stride, v))
                print('   fetch %d stride %d verts %d' % (fetch, stride, len(v)))
                print(v[:8])
            vsc = np.array(r['vsc']).reshape(64, 4)
            psc = np.array(r['psc']).reshape(16, 4)
            out[int(draw)] = dict(nidx=nidx, tex=tex, ind=ind, verts=verts, vsc=vsc, psc=psc,
                                  r22=np.array(r['r22']), r21=np.array(r['r21']))
    if len(sys.argv) > 4:
        pickle.dump(out, open(sys.argv[4], 'wb'))

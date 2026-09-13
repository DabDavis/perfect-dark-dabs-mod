#!/usr/bin/env python3
"""Reads the draw log written by the PD research hook in Xenia
(xenia-canary src/xenia/gpu/pm4_command_processor_implement.h, PD_DRAWLOG).

One 1484 byte record per draw, little endian: magic 'PDDL', frame, raw
VGT_DRAW_INITIATOR (index count in the top 16 bits), vertex and pixel shader
objects (u64 each), vertex float constants c0-c63, pixel float constants
c0-c15, registers 0x2100-0x211F (blend colour at 0x2105-0x2108) and
0x2200-0x220B (depth, blend control, colour control, cull mode at 0x2205).

Usage:
    drawlog.py LOG summary              index counts per frame, where they change
    drawlog.py LOG meshes               the boot logo meshes' draws, frame by frame
    drawlog.py LOG slots INDICES        which constants change across those draws
    drawlog.py LOG dump INDICES [F0 F1] full constants for those draws
    drawlog.py LOG textures [F0]        (PDD2 logs) each boot mesh's bound textures
"""
import sys
import numpy as np

REC = np.dtype([('magic', '<u4'), ('frame', '<u4'), ('init', '<u4'),
                ('vs', '<u8'), ('ps', '<u8'),
                ('vsc', '<f4', 256), ('psc', '<f4', 64),
                ('r21', '<u4', 32), ('r22', '<u4', 12)])
assert REC.itemsize == 1484

# The second version ('PDD2') adds the 32 texture fetch constants (registers
# 0x4800-0x48BF, six dwords each, xe_gpu_texture_fetch_t) and the fetch
# constant each shader's texture bindings read, 8 a shader, 0xff for none.
REC2 = np.dtype(REC.descr + [('fetch', '<u4', 192), ('vsfetch', 'u1', 8), ('psfetch', 'u1', 8)])
assert REC2.itemsize == 1484 + 768 + 16

MESHES = {
    1185: 'red 4J cube',
    600: 'PD cube draw 0',
    168: 'PD cube draw 1',
    192: 'PD cube draw 2',
    19620: 'Rare logo',
    14868: 'Microsoft Game Studios',
}


def load(path):
    raw = np.memmap(path, dtype=np.uint8, mode='r')
    rec, magic = (REC2, 0x32444450) if bytes(raw[:4]) == b'PDD2' else (REC, 0x4C444450)
    n = len(raw) // rec.itemsize
    recs = np.frombuffer(raw[:n * rec.itemsize], dtype=rec)
    bad = np.nonzero(recs['magic'] != magic)[0]
    if len(bad):
        recs = recs[:bad[0]]
    return recs


def f4(v):
    return '[' + ' '.join('%10.4f' % x for x in v) + ']'


def blend_rgba(rec):
    return rec['r21'][5:9].view('<f4')


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    recs = load(path)
    nidx = recs['init'] >> 16
    print('%d draws, frames %d..%d' % (len(recs), recs['frame'][0], recs['frame'][-1]))

    if cmd == 'summary':
        frames = recs['frame']
        last = None
        for fr in np.unique(frames):
            sel = nidx[frames == fr]
            sig = tuple(sorted(set(int(x) for x in sel)))
            if sig != last:
                print('frame %5d: %4d draws, index counts %s' % (fr, len(sel), ' '.join(str(x) for x in sig[:30])))
                last = sig

    elif cmd == 'meshes':
        idx = np.nonzero(np.isin(nidx, list(MESHES)))[0]
        lastkey = None
        for i in idx:
            r = recs[i]
            key = (int(nidx[i]), int(r['frame']))
            print('frame %5d  %-24s vs %06x ps %06x blend %s c0 %s' % (
                r['frame'], MESHES[int(nidx[i])], r['vs'] & 0xffffff, r['ps'] & 0xffffff,
                f4(blend_rgba(r)), f4(r['vsc'][0:4])))

    elif cmd == 'slots':
        want = int(sys.argv[3])
        sel = recs[nidx == want]
        print('%d draws with %d indices, frames %d..%d' % (len(sel), want, sel['frame'][0], sel['frame'][-1]))
        for c in range(64):
            block = sel['vsc'][:, c * 4:c * 4 + 4]
            distinct = len(np.unique(block, axis=0))
            if distinct > 1 or np.any(block != 0):
                print('  c%-2d %4d distinct  first %s  last %s' % (c, distinct, f4(block[0]), f4(block[-1])))
        for c in range(16):
            block = sel['psc'][:, c * 4:c * 4 + 4]
            distinct = len(np.unique(block, axis=0))
            if distinct > 1 or np.any(block != 0):
                print('  ps c%-2d %4d distinct  first %s  last %s' % (c, distinct, f4(block[0]), f4(block[-1])))

    elif cmd == 'textures':
        # PDD2 only: for each boot mesh, every distinct set of fetch constants
        # its shaders bind - base address, size, format, filters, mip levels.
        FROM = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        seen = set()
        for i in np.nonzero(np.isin(nidx, list(MESHES)) & (recs['frame'] >= FROM))[0]:
            r = recs[i]
            fetches = sorted(set(int(x) for x in r['psfetch'] if x != 0xff) | set(int(x) for x in r['vsfetch'] if x != 0xff))
            desc = []
            for f in fetches:
                d = r['fetch'][f * 6:f * 6 + 6]
                desc.append('fetch %d: addr %08x %dx%d fmt %d tiled %d filt mag %d min %d mip %d mips %d-%d clamp %d/%d packed %d mipaddr %08x' % (
                    f, (int(d[1]) >> 12) << 12, (int(d[2]) & 0x1fff) + 1, ((int(d[2]) >> 13) & 0x1fff) + 1,
                    int(d[1]) & 0x3f, int(d[0]) >> 31, (int(d[3]) >> 19) & 3, (int(d[3]) >> 21) & 3, (int(d[3]) >> 23) & 3,
                    (int(d[4]) >> 2) & 15, (int(d[4]) >> 6) & 15, (int(d[0]) >> 10) & 7, (int(d[0]) >> 13) & 7,
                    (int(d[5]) >> 11) & 1, (int(d[5]) >> 12) << 12))
            key = (int(nidx[i]), r['vs'], r['ps'], tuple(desc))
            if key in seen:
                continue
            seen.add(key)
            print('frame %5d %-24s vs %06x ps %06x vsfetch %s psfetch %s' % (
                r['frame'], MESHES[int(nidx[i])], r['vs'] & 0xffffff, r['ps'] & 0xffffff,
                list(r['vsfetch']), list(r['psfetch'])))
            for s in desc:
                print('    ' + s)

    elif cmd == 'dump':
        want = int(sys.argv[3])
        f0 = int(sys.argv[4]) if len(sys.argv) > 4 else 0
        f1 = int(sys.argv[5]) if len(sys.argv) > 5 else 1 << 30
        for i in np.nonzero((nidx == want) & (recs['frame'] >= f0) & (recs['frame'] <= f1))[0]:
            r = recs[i]
            print('frame %d draw %d indices %d prim %d vs %x ps %x' % (
                r['frame'], i, want, r['init'] & 0x3f, r['vs'], r['ps']))
            for c in range(16):
                print('  c%-2d %s' % (c, f4(r['vsc'][c * 4:c * 4 + 4])))
            for c in range(4):
                print('  ps c%d %s' % (c, f4(r['psc'][c * 4:c * 4 + 4])))
            print('  blend rgba %s  blendcontrol0 %08x depth %08x mode %08x' % (
                f4(blend_rgba(r)), r['r22'][1], r['r22'][0], r['r22'][5]))


if __name__ == '__main__':
    main()

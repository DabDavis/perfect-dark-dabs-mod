#!/usr/bin/env python3
"""Per-frame curves of 4J's Perfect Dark logo stage, from the draw log.

For each frame of the stage (the frames that draw the red 4J cube), one row:
    tick      game ticks since the stage's first frame (from the .swaps
              timestamps at 60 per second; the frame number when there are
              none)
    scale     the cubes' uniform scale
    pitch     degrees; spin degrees, unwrapped
    pdy       the Perfect Dark cube's Y scale over its X scale (4J's morph)
    red_a     alpha of the red cube's colour pass (1 when it has none)
    pd_a      alpha of the Perfect Dark cube's colour pass (0 when it has none)
    text_a    alpha of the "4J STUDIOS" glyph quads (0 when there are none)

Only the first of the three identical render passes of each frame is read.
"""
import sys, os, math, struct, csv
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from drawlog import load
from analyze4j import decompose

GLYPH_VS = 0x093580  # low 24 bits of the glyph quads' vertex shader object


def swap_ticks(path):
    p = path + '.swaps'
    if not os.path.exists(p):
        return None
    data = open(p, 'rb').read()
    n = len(data) // 12
    out = {}
    for i in range(n):
        frame, micros = struct.unpack_from('<IQ', data, i * 12)
        out[frame] = micros
    return out


def main():
    log, outcsv = sys.argv[1], sys.argv[2]
    recs = load(log)
    nidx = recs['init'] >> 16
    frames = recs['frame']
    swaps = swap_ticks(log)
    red_frames = np.unique(frames[nidx == 1185])
    if not len(red_frames):
        raise SystemExit('no red cube draws')
    first = int(red_frames[0])
    rows = []
    unwrap = 0.0
    lastspin = None
    for fr in red_frames:
        sel = np.nonzero(frames == fr)[0]
        # the first render pass: up to the second red cube opaque draw's block
        red = [i for i in sel if nidx[i] == 1185]
        pd = [i for i in sel if nidx[i] == 600]
        # The name's glyph quads: the vertex shader used by the most 6-index
        # draws of the frame (shader objects are host pointers, so an id from
        # one run means nothing in the next).
        six = [i for i in sel if nidx[i] == 6]
        glyph = []
        if six:
            counts = {}
            for i in six:
                counts[int(recs['vs'][i])] = counts.get(int(recs['vs'][i]), 0) + 1
            vs, n = max(counts.items(), key=lambda kv: kv[1])
            if n >= 9:
                glyph = [i for i in six if int(recs['vs'][i]) == vs]
        r0 = recs[red[0]]
        sc, p, s, t, o, R = decompose(r0['vsc'])
        spin = math.degrees(s)
        if lastspin is not None:
            d = (spin - lastspin + 180.0) % 360.0 - 180.0
            unwrap += d
        else:
            unwrap = spin
        lastspin = spin
        scp, _, _, _, _, _ = decompose(recs[pd[0]]['vsc'])
        def pass_alpha(draws):
            # The colour pass is the one with a colour mask: blended by its
            # pixel constant c1's alpha, or opaque (alpha 1) with ONE/ZERO.
            # A cube with no colour pass at all is not drawn (alpha 0).
            for i in draws[:2]:
                if recs['r21'][i][4] & 7:
                    if recs['r22'][i][1] == 0x07060706:
                        return float(recs['psc'][i][7])
                    return 1.0
            return 0.0
        red_a = pass_alpha(red)
        pd_a = pass_alpha(pd)
        text_a = float(recs['psc'][glyph[0]][3]) if glyph else 0.0
        if swaps and fr in swaps and first in swaps:
            tick = (swaps[fr] - swaps[first]) * 60.0 / 1e6
        else:
            tick = float(fr - first)
        mask = [recs['r21'][i][4] for i in red[:2]]
        rows.append([int(fr), tick, sc.mean(), math.degrees(p), unwrap, scp[1] / scp[0], red_a, pd_a, text_a,
                     ' '.join('%x' % m for m in mask)])
    with open(outcsv, 'w') as f:
        w = csv.writer(f)
        w.writerow(['frame', 'tick', 'scale', 'pitch', 'spin', 'pdy', 'red_a', 'pd_a', 'text_a', 'colour_masks'])
        for r in rows:
            w.writerow([r[0], '%.2f' % r[1], '%.5f' % r[2], '%.3f' % r[3], '%.3f' % r[4], '%.4f' % r[5],
                        '%.4f' % r[6], '%.4f' % r[7], '%.4f' % r[8], r[9]])
    print('%d frames, stage from frame %d, %s' % (len(rows), first, 'ticks from swap timestamps' if swaps else 'NO timestamps: tick = frame'))
    step = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    for k, r in enumerate(rows):
        if k % step == 0:
            print('%5d t%7.1f s %.4f pitch %7.2f spin %8.2f pdy %.3f red %.3f pd %.3f text %.3f masks %s' % tuple(r))


if __name__ == '__main__':
    main()

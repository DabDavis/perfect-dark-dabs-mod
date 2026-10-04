#!/usr/bin/env python3
"""Tell same-mesh GoldenEye characters apart by the textures a GE-X file uses.

GE-X's pictures come from a --dump-textures run of the game with GE-X
mounted (texture-dumps/pd-n64/<texnum>_<fmt>.png, bottom row first);
GoldenEye's originals from the Bean release's original/ tree (decoded by
cafftool.py, the game's row order). Each texture is reduced to 16x16 RGB
and a candidate scores the mean, over the file's textures, of its closest
original (either row order). Writes texcompare.json.
"""
import glob, json, os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
from pdmodel import PdModel, load

# S: scratch for the contact sheet; ORIG: cafftool.py's decode of the release's
# original/ characters and heads, one directory per GoldenEye model
S = os.environ.get('GEFIT_SCRATCH') or paths.repo('build')
DUMP = paths.GEX_TEXDUMP
GEX = paths.GEX_FILES
ORIG = os.environ.get('GEFIT_ORIGTEX') or os.path.join(S, 'origtex')
FAMILIES = [
    ['redman', 'blueman', 'cardiman', 'checkman', 'greyman'],
    ['moonfemale', 'jeanwoman', 'techwoman', 'bluewoman', 'fattechwoman'],
    ['headjoe', 'headgraham', 'headjoe2'],
    ['headshaun', 'headscott', 'headlee', 'headdave'],
]


def feat(img):
    a = np.asarray(img.convert('RGB').resize((16, 16), Image.BILINEAR), dtype=np.float32)
    return a


def gex_textures(fn):
    d = load(os.path.join(GEX, fn))
    PdModel(d)
    n = struct.unpack_from('>h', d, 22)[0]
    tp = struct.unpack_from('>I', d, 24)[0] & 0xffffff
    out = []
    for i in range(n):
        num, = struct.unpack_from('>I', d, tp + 12 * i)
        hits = glob.glob(os.path.join(DUMP, '%04x_*.png' % num))
        if hits:
            out.append((num, hits[0]))
    return out


def main():
    assign = json.load(open(paths.data('gexassign.json')))
    orig = {}
    for fam in FAMILIES:
        for m in fam:
            files = sorted(glob.glob(os.path.join(ORIG, m, '*.png')))
            orig[m] = [(f, feat(Image.open(f))) for f in files]
    result = {}
    pairs = []
    for fn, a in sorted(assign.items()):
        tops = [t[0] for t in a['top']]
        fams = [fam for fam in FAMILIES if any(t in fam for t in tops[:2])]
        if not fams:
            continue
        fam = fams[0]
        texs = gex_textures(fn)
        if not texs:
            continue
        feats = [(num, path, feat(Image.open(path))) for num, path in texs]
        scores = {}
        best_for = {}
        for m in fam:
            if not orig[m]:
                continue
            total = 0.0
            for num, path, g in feats:
                best = None
                for of, o in orig[m]:
                    for cand in (o, o[::-1]):
                        dist = float(np.abs(g - cand).mean())
                        if best is None or dist < best[0]:
                            best = (dist, of)
                total += best[0]
                best_for[(m, num)] = best
            scores[m] = total / len(feats)
        ranked = sorted(scores.items(), key=lambda kv: kv[1])
        margin = ranked[1][1] - ranked[0][1] if len(ranked) > 1 else 0
        result[fn] = dict(ranked=[(m, round(s, 2)) for m, s in ranked], vertex_best=a['best'],
                          texture_best=ranked[0][0], margin=round(margin, 2))
        flag = 'CHANGED' if ranked[0][0] != a['best'] else 'same   '
        print('%s %-18s vertex %-12s texture %-12s margin %5.2f  %s' % (
            flag, fn, a['best'], ranked[0][0], margin, ' '.join('%s %.1f' % (m, s) for m, s in ranked)))
        win = ranked[0][0]
        for num, path, g in feats[:3]:
            pairs.append((fn, num, path, win, best_for[(win, num)][1]))
    json.dump(result, open(paths.data('texcompare.json'), 'w'), indent=1)

    tile = 96
    rows = pairs[:48]
    sheet = Image.new('RGB', (tile * 2 * 4, tile * ((len(rows) + 3) // 4)), (40, 40, 40))
    for i, (fn, num, gp, win, op) in enumerate(rows):
        x, y = (i % 4) * tile * 2, (i // 4) * tile
        sheet.paste(Image.open(gp).convert('RGB').resize((tile, tile)), (x, y))
        sheet.paste(Image.open(op).convert('RGB').transpose(Image.FLIP_TOP_BOTTOM).resize((tile, tile)), (x + tile, y))
    sheet.save(S + '/texcompare_sheet.png')
    print('sheet: %d pairs (left GE-X, right best Bean original, flipped for display)' % len(rows))


if __name__ == '__main__':
    main()

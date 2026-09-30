#!/usr/bin/env python3
"""Convert every GoldenEye XBLA (Bean) character and head for the GE-X files they replace.

Reads gemap.json (gemap.py: GE model -> GE-X files, arrays and joints) and
gexassign.json (each GE-X file's best GE model by overlap), writes the pack
into PACK/n64 and a report into batch_report.txt.

    batch_gex.py [PACK_N64_DIR]
"""
import glob, json, os, statistics, sys, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
from bean2obj import Model
from bean2pack import convert_body, convert_head

HERE = os.path.dirname(os.path.abspath(__file__))
BEAN = os.path.join(paths.BEAN, 'files/new')
GEX = paths.GEX_FILES
OUT = sys.argv[1] if len(sys.argv) > 1 else paths.repo('build/model-packs/bean/n64')

# Bean characters that are broken in the release itself: blueman's mesh with
# greatguard2's greatcoat atlas under it (bluewoman and greyman are the same
# file), which renders as a scrambled pattern with no conversion involved.
# Their GE-X files keep their N64 look.
BROKEN_SOURCES = {'blueman', 'bluewoman', 'greyman'}

# Families whose GoldenEye models share one mesh and differ by texture, so a
# vertex match cannot tell which outfit a GE-X file carries.
SAME_MESH = [
    {'redman', 'blueman', 'cardiman', 'checkman', 'greyman'},
    {'moonfemale', 'jeanwoman', 'techwoman', 'bluewoman', 'fattechwoman'},
    {'headjoe', 'headgraham', 'headjoe2'},
]


def family_of(ge):
    for fam in SAME_MESH + [{'headshaun', 'headscott', 'headlee', 'headdave'}]:
        if ge in fam:
            return fam
    return None


def apply_textures(assign, log):
    """Settle same-mesh families by texture (texcompare.py -> texcompare.json).

    The texture winner is taken when the vertex match is in the same family,
    the winner is close (mean 16x16 RGB distance under 16) and ahead by at
    least 0.5. A head that fails that is not converted - GE-X ships faces of
    its own that GoldenEye never had (a blond boy, a bald man), and a close
    mesh with somebody else's face is worse than the N64 head; a body that
    fails keeps its vertex match.
    """
    path = os.path.join(HERE, 'texcompare.json')
    if not os.path.exists(path):
        return
    for fn, t in json.load(open(path)).items():
        if fn not in assign:
            continue
        a = assign[fn]
        fam = family_of(a['best'])
        best, score = t['ranked'][0]
        ok = fam is not None and best in fam and score < 16 and t['margin'] >= 0.5
        if ok:
            if best != a['best'] or len(a['tied']) > 1:
                log('  texture: %-18s %s -> %s (distance %.1f, margin %.2f)' % (fn, a['best'], best, score, t['margin']))
            a['best'], a['tied'] = best, [best]
        elif fn.startswith('Chead') and fam is not None:
            log('  texture: %-18s no GoldenEye face matches (best %s at %.1f) - left N64' % (fn, best, score))
            a['best'], a['tied'], a['score'] = None, [], 0


def main():
    gemap = json.load(open(os.path.join(HERE, 'gemap.json')))
    gemap.pop('_arrays')
    assign = json.load(open(os.path.join(HERE, 'gexassign.json')))
    report = []
    log = lambda s: (print(s), report.append(s))
    log('== texture decisions')
    apply_textures(assign, log)

    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, '*')):
        os.remove(f)

    # Integrated heads: a GE model whose head-joint arrays GE-X moved into a
    # Chead file. Each head file goes to the model with the most of them.
    headclaim = {}
    for ge, per in gemap.items():
        for fn, v in per.items():
            n = v['joints'].get('3', 0)
            if fn.startswith('Chead') and n:
                if fn not in headclaim or n > headclaim[fn][1] or (n == headclaim[fn][1] and ge < headclaim[fn][0]):
                    headclaim[fn] = (ge, n)
    integrated = {}
    for fn, (ge, n) in headclaim.items():
        integrated.setdefault(ge, []).append(fn)

    log('== bodies')
    scales = []
    failures = []
    for fn, a in sorted(assign.items()):
        if fn.startswith('Chead') or a['nodes'] != 25 or a['score'] < 0.5:
            continue
        ge = a['best']
        if ge in BROKEN_SOURCES:
            log('  %-20s <- %s: broken in the Bean release, left N64' % (fn, ge))
            continue
        bean = os.path.join(BEAN, 'char', ge, 'default.bin')
        if not os.path.exists(bean):
            log('  %-20s <- %s: no Bean character, skipped' % (fn, ge))
            continue
        heads = [(os.path.join(GEX, h), h) for h in sorted(integrated.get(ge, []))]
        flag = ''
        for fam in SAME_MESH:
            if ge in fam:
                flag = '   [same-mesh family: outfit unverified]'
        try:
            scales.append(convert_body(bean, os.path.join(GEX, fn), fn, OUT, ge, heads, log))
            if flag:
                report[-1 - len(heads)] += flag
        except Exception as e:
            failures.append((fn, ge, repr(e)))
            log('  %-20s <- %s: FAILED %r' % (fn, ge, e))
            traceback.print_exc()

    scale = statistics.median(scales) if scales else 0.213
    log('== separate heads (scale %.4f, the bodies\' median)' % scale)
    for fn, a in sorted(assign.items()):
        if not fn.startswith('Chead') or fn in headclaim:
            continue
        ge = a['best']
        if not ge:
            continue
        if len(a['tied']) > 1:
            log('  %-20s tie between %s: skipped' % (fn, ', '.join(a['tied'])))
            continue
        bean = os.path.join(BEAN, 'head', ge, 'default.bin')
        if not ge.startswith('head') or not os.path.exists(bean):
            log('  %-20s <- %s: no Bean head, skipped' % (fn, ge))
            continue
        m = Model(bean)
        if not m.remap or len(m.pose) != 16:
            log('  %-20s <- %s: a static N64-style model, not a skinned head, skipped' % (fn, ge))
            continue
        try:
            convert_head(bean, os.path.join(GEX, fn), fn, OUT, ge, scale, log)
            for fam in SAME_MESH:
                if ge in fam:
                    report[-1] += '   [same-mesh family: face unverified]'
        except Exception as e:
            failures.append((fn, ge, repr(e)))
            log('  %-20s <- %s: FAILED %r' % (fn, ge, e))
            traceback.print_exc()

    objs = len(glob.glob(os.path.join(OUT, '*.obj')))
    pngs = len(glob.glob(os.path.join(OUT, '*.png')))
    size = sum(os.path.getsize(f) for f in glob.glob(os.path.join(OUT, '*'))) / 1e6
    log('== %d OBJ files, %d textures, %.1f MB, %d failures' % (objs, pngs, size, len(failures)))
    open(os.path.join(HERE, 'batch_report.txt'), 'w').write('\n'.join(report) + '\n')


if __name__ == '__main__':
    main()

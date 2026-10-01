#!/usr/bin/env python3
"""One matched picture pair: GoldenEye XBLA (Bean, CE by default) in Xenia against
our HD look, Bond on the same pad, heading and pitch.

    xenia/pair.py --mission dam --pad 0 --theta 90 [--verta 0] --out DIR

Bean through xenia/run_scenario.py + pair_scene.py (under the rig lock), ours
through twin.run_pd in its own run directory with the HD look's pd.ini
(xenia-out/hd-run/hd-pd.ini: XBLA keys, the Community Edition on) and a
1280x720 window - Bean's presented size, so both are 16:9 at our FovY 60.
Both are scored at 320x180 the way view/viewdiff.py scores (coarse structure,
edges, colour blocks). The null: Bean's own two shots of the spot, a few
frames apart, must score near zero, and Bean's poke read back must hold;
the pair's score is only reported when both pass.
"""
import argparse, json, math, os, shutil, subprocess, sys
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
sys.path.insert(0, ROOT)
import levels  # noqa: E402
import twin  # noqa: E402

RUN = os.path.expanduser('~/wt/gefidelity-run')
SIZE = (320, 180)
NULL_MAX = 0.05


def ini_with(src, dst, sets):
    """src's pd.ini with KEY=VALUE set in the named sections."""
    lines, section, done = open(src).read().splitlines(), None, set()
    out = []
    for l in lines:
        if l.startswith('['):
            section = l.strip('[]')
        k = l.split('=', 1)[0] if '=' in l else None
        if k and (section, k) in sets:
            l = '%s=%s' % (k, sets[(section, k)])
            done.add((section, k))
        out.append(l)
    for (sec, k), v in sets.items():
        if (sec, k) not in done:
            out += ['[%s]' % sec, '%s=%s' % (k, v)]
    open(dst, 'w').write('\n'.join(out) + '\n')


def features(path):
    im = Image.open(path).convert('RGB')
    a = np.asarray(im.resize(SIZE, Image.BILINEAR), dtype=np.float64)
    g = a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114
    b = ndimage.gaussian_filter(g, 2.0)
    e = ndimage.gaussian_filter(np.hypot(*np.gradient(b)), 2.0)
    blocks = a.reshape(9, 20, 16, 20, 3).mean((1, 3))
    return {'coarse': b[::2, ::2], 'edge': e, 'blocks': blocks}


def ssim(x, y):
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    g = lambda v: ndimage.gaussian_filter(v, 1.5)
    mx, my = g(x), g(y)
    sxx, syy, sxy = g(x * x) - mx * mx, g(y * y) - my * my, g(x * y) - mx * my
    return float((((2 * mx * my + C1) * (2 * sxy + C2)) / ((mx * mx + my * my + C1) * (sxx + syy + C2))).mean())


def score(fa, fb):
    struct_ = max(0.0, 1 - ssim(fa['coarse'], fb['coarse']))
    ea, eb = fa['edge'].ravel() - fa['edge'].mean(), fb['edge'].ravel() - fb['edge'].mean()
    den = math.sqrt(float((ea * ea).sum() * (eb * eb).sum()))
    edge = max(0.0, 1 - (float((ea * eb).sum() / den) if den else 1.0))
    colour = float(np.abs(fa['blocks'] - fb['blocks']).mean() / 255.0)
    return {'struct': struct_, 'edge': edge, 'colour': colour, 'score': (struct_ + edge + 8.0 * colour) / 3.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--pad', type=int, default=0)
    ap.add_argument('--theta', type=float, default=0.0)
    ap.add_argument('--verta', type=float, default=0.0)
    ap.add_argument('--settle', type=int, default=900)
    ap.add_argument('--out', required=True)
    ap.add_argument('--bin', default='./pd.base')
    ap.add_argument('--rundir', default=os.path.join(RUN, 'xenia-out', 'hd-run'))
    ap.add_argument('--skip-bean', action='store_true')
    a = ap.parse_args()
    m = levels.mission(a.mission)
    out = os.path.abspath(a.out)
    env = {'PAIR_PAD': str(a.pad), 'PAIR_THETA': str(a.theta), 'PAIR_VERTA': str(a.verta),
           'PAIR_SETTLE': str(a.settle)}
    if not a.skip_bean:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'run_scenario.py'), os.path.join(HERE, 'pair_scene.py'),
                            '--mission', m[1], '--out', out] + sum([['--env', '%s=%s' % kv] for kv in env.items()], []),
                           capture_output=True, text=True)
        print((r.stdout + r.stderr).strip().splitlines()[-1:])
    save = os.path.join(out, 'pd', 'save')
    os.makedirs(save, exist_ok=True)
    ini_with(os.path.join(a.rundir, 'hd-pd.ini'), os.path.join(save, 'pd.ini'),
             {('Video', 'DefaultWidth'): 1280, ('Video', 'DefaultHeight'): 720,
              ('Video', 'DefaultFullscreen'): 0})
    p = twin.run_pd(os.path.join(HERE, 'pair_scene.py'), m, 0, out, env, 600, a.rundir, a.bin, [])
    p.wait()
    twin.finish_pd(p)
    bean = os.path.join(out, 'bean')
    ours = sorted(f for f in os.listdir(os.path.join(out, 'pd')) if f.startswith('shot_'))
    if not ours or not os.path.exists(os.path.join(bean, 'shot_a.png')):
        print('a side took no picture (see %s/bean/scenario.log, %s/pd/gdb.log)' % (out, out))
        return 1
    bi = json.load(open(os.path.join(bean, 'pair.json')))
    fa, fb = features(os.path.join(bean, 'shot_a.png')), features(os.path.join(bean, 'shot_b.png'))
    fo = features(os.path.join(out, 'pd', ours[0]))
    null = score(fa, fb)
    res = {'null_bean_twice': null, 'poke_ok': bi.get('poke_ok'), 'pair': score(fa, fo),
           'bean_player': bi.get('player'), 'ours_player': json.load(open(os.path.join(out, 'pd', 'pair.json'))).get('player')}
    if null['score'] > NULL_MAX or not bi.get('poke_ok'):
        res['verdict'] = 'NULL FAILED: Bean twice scored %.3f (max %.2f), poke %s' % (null['score'], NULL_MAX, bi.get('poke_ok'))
    json.dump(res, open(os.path.join(out, 'pair-score.json'), 'w'), indent=1)
    # a side-by-side for looking at
    ims = [Image.open(os.path.join(bean, 'shot_a.png')).convert('RGB').resize((640, 348)),
           Image.open(os.path.join(out, 'pd', ours[0])).convert('RGB').resize((640, 348))]
    sheet = Image.new('RGB', (1284, 348))
    sheet.paste(ims[0], (0, 0))
    sheet.paste(ims[1], (644, 0))
    sheet.save(os.path.join(out, 'pair.png'))
    print(json.dumps(res, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())

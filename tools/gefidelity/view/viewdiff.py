#!/usr/bin/env python3
"""The view diff: the same pictures from GoldenEye and from our N64 look, scored,
worst first.

    view/viewdiff.py run   --mission dam [--step 6] [--max 40] [--out DIR]
    view/viewdiff.py score DIR                 # (re)score a run that is on disk
    view/viewdiff.py sweep [--missions all] [--out DIR]

`run` drives view/tour.py on both sides (GoldenEye on the oracle host through
twin.py, its rasteriser on only for the frames a picture is taken from; ours in
a private run directory at GoldenEye's viewport aspect), then scores. Output: DIR/<mission>/index.html, scores.json,
and pairs/*.png (GoldenEye's viewport, ours, and a difference map).

Same picture geometry. GoldenEye draws a level into a 320x220 viewport of its
320x240 framebuffer (bondview2.c: viSetFovY(60), aspect = viewport w/h); ours
renders at 640x440 (a pd.ini written into the run's own save directory) with
the same 60 degree vertical field of view, so after GoldenEye's viewport is
cut out the two are the same picture at two resolutions. Both are compared at
GoldenEye's 320x220.

Cameras are checked, not trusted. Each side records where its camera really
was; after the level's offset (measured from the pads the two tours stood on)
a pair more than CAM_XZ units apart across the floor or CAM_ANG degrees apart
in where it looks is a camera mismatch and is not scored as a picture
difference. A difference in eye height alone is scored and shown (the world
diff measured ours 8.3-11 units under GoldenEye's on Dam; it is that finding,
not this instrument's, to fix).

Score, 0 = the same picture: the mean of three terms, taken at a scale where
the two renderers' texture filtering and dithering are gone (blur 2) and a
missing wall is not - struct (1 - SSIM at half resolution), edge (1 - the
correlation of the blurred edge maps: geometry, moved props) and 8 x colour
(the mean difference of 20x20 block colours: sky, fog, lighting, a wrong
texture). Tuned on Dam, where matched pairs score 0.19 and one pad off 0.64.

The null runs on every invocation and the ranking is refused when it fails:
each GoldenEye picture scored against itself must give 0, and each matched pair
must score better than the same GoldenEye picture joined to our picture of the
next pad (or, with one pad, the next heading). If the join is not clearly
better than one pad off, the scores measure the renderers, not the levels.
And a flat grey 64x64 block planted in our picture must raise the score of 90%
of the pairs by 0.05: an instrument that cannot see a planted wall cannot rank.
"""
import argparse, concurrent.futures as cf, html, json, math, os, shutil, subprocess, sys, threading, time

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
import levels  # noqa: E402

RUNROOT = os.path.expanduser(os.environ.get('GF_RUNDIR_ROOT', '~/wt/gefidelity-run'))
DEFAULT_OUT = {'ares': os.path.join(RUNROOT, 'view-ares'), 'port': os.path.join(RUNROOT, 'view-out'),
               'xenia': os.path.join(RUNROOT, 'view-xenia')}
# --oracle xenia: GoldenEye XBLA ("Bean") is the reference for our HD look, and
# both sides run the Community Edition (user 2026-10-01: "use Community
# edition, since that is what we use for ge plus"): Bean's CE build in Xenia,
# Mod.GeXblaCommunityEdition with the updater in our added-content
BEAN_CE_XEX = '/home/sdg/perfect-dark/.xbla-work/ge-bean/BeanCE/defaultCE.xex'
CE_ZIP = '/home/sdg/perfect-dark/perfect_dark/build/gexrom/added-content/CommunityEditionUpdaterV6.zip'
BEAN_DIR = '/home/sdg/perfect-dark/.xbla-work/ge-bean/Bean'
HD_KEYS = {'XblaMeshes': 1, 'XblaStages': 1, 'XblaSkies': 1, 'XblaMeshTextures': 1, 'GeXblaCommunityEdition': 1}
BEAN_FRAME = (1280, 720)        # what Bean presents; the rig photographs it into 1280x695
W, H = 320, 220                 # the compare size: GoldenEye's in-level viewport (set_geometry() per run)
PD_SCALE = 2                    # ours renders at twice GoldenEye's viewport
CAM_XZ = 4.0                    # units across the floor
CAM_ANG = 1.0                   # degrees between the two look vectors
CAM_DY = 4.0                    # eye height delta that is flagged (still scored)
CAM_STOREY = 40.0               # past this the two cameras are on different floors: a mismatch
CHR_IN_CAMERA = 40.0            # a chr this close across the floor on either side: the camera is inside him
CONTROL_RATIO = 0.85            # matched median must be under this x the one-pad-off median
CONTROL_WIN = 0.75              # and better than one pad off on this share of pairs
PLANT_RISE = 0.05               # a planted 64x64 grey block must raise a pair's score this much
PLANT_SEEN = 0.90               # on this share of pairs
# the cartridge photographed twice: aresge pins GoldenEye's RNG, but the tick a
# picture lands on still varies by 1-3 between runs (the gun's sway and the
# view's bob with it), so the repeat is not zero - measured over 19 missions the
# median is 0.01-0.035. The null wants the oracle's own jitter well under what
# the diff tells apart: a picture against the wrong one (the control, one pad
# off). It was judged against the matched pairs' median until 2026-10-01, which
# failed the null as soon as ours came close to the cartridge (Bond's eye fixed:
# Bunker, Silo, Cradle and Runway matched at 0.07-0.11 against a jitter of
# 0.02-0.04) - it punished getting closer. A matched median down near the
# jitter is reported (oracle_repeat.vs_matched), not failed: from there on the
# pairs' order is the cartridge's own noise. A pair whose own picture changes
# more than ORACLE_UNSTEADY between runs (an explosion going off, Aztec pad 34)
# is not ranked
ORACLE_REPEAT_RATIO = 0.25      # median jitter must be under this x the one-pad-off median
ORACLE_REPEAT_MAX = 0.06        # and under this outright: pinned runs measure 0.01-0.04
ORACLE_UNSTEADY = 0.15


# ------------------------------------------------------------------ running

def _rundirs(binary='./pd.base', hd=False):
    """Private run directories for our side, one per concurrent run: pd.log and
    screenshots/ are per directory, and the binary is hard-linked so the port's
    "next to the executable" paths land here too."""
    out = []
    for i in range(2):
        d = os.path.join(RUNROOT, ('view-hd-rundir-%d' if hd else 'view-rundir-%d') % i)
        os.makedirs(d, exist_ok=True)
        for name in ('data', 'mods') if hd else ('data', 'added-content', 'mods'):
            p = os.path.join(d, name)
            if not os.path.lexists(p):
                os.symlink(os.path.join('..', name), p)
        if hd:
            # the HD look wants the release (and the CE updater) beside the ROM;
            # the HD level cache is per run directory (next to the binary)
            ac = os.path.join(d, 'added-content')
            os.makedirs(ac, exist_ok=True)
            for name, src in (('GoldenEye 007 (U) [!].n64', os.path.join(RUNROOT, 'added-content', 'GoldenEye 007 (U) [!].n64')),
                              ('goldeneye', BEAN_DIR), ('CommunityEditionUpdaterV6.zip', CE_ZIP)):
                q = os.path.join(ac, name)
                if not os.path.lexists(q):
                    os.symlink(os.path.realpath(src), q)
        src = binary if os.path.isabs(binary) else os.path.normpath(os.path.join(RUNROOT, binary))
        b = os.path.join(d, os.path.basename(src))
        if not os.path.exists(b) or os.stat(b).st_ino != os.stat(src).st_ino:
            if os.path.lexists(b):
                os.remove(b)
            try:
                os.link(src, b)
            except OSError:
                shutil.copy2(src, b)
        out.append(d)
    return out


def _twin(side, mission, out, env, rundir=None, timeout=3600, oracle='ares', binary=None):
    cmd = [sys.executable, os.path.join(ROOT, 'twin.py'), side, os.path.join(HERE, 'tour.py'),
           '--mission', mission, '--out', out, '--timeout', str(timeout), '--no-sync', '--oracle', oracle]
    if binary:
        cmd += ['--bin', './' + os.path.basename(binary)]
    # GoldenEye: no --render (twin.py boots the oracle with its rasteriser off,
    # the menu walk's 1300 frames cost nothing, tour.py turns it on for the
    # three frames before each picture), and twin.py's default pad script
    # (common/solo-quiet.padscript: one fire press, no bullet holes)
    if rundir:
        cmd += ['--rundir', rundir]
    for k, v in env.items():
        cmd += ['--env', '%s=%s' % (k, v)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip().splitlines()[-1:]


def _prepare_pd(out, viewport=(0, 10, 320, 220), fovy=60.0, hd=False):
    """Our window at GoldenEye's viewport aspect (twice its size) and its
    vertical field of view, in a pd.ini in the run's own save directory; the
    HD look (and the Community Edition) for --oracle xenia, at the frame Bean
    presents."""
    save = os.path.join(out, 'pd', 'save')
    os.makedirs(save, exist_ok=True)
    w, h = (BEAN_FRAME if hd else (PD_SCALE * viewport[2], PD_SCALE * viewport[3]))
    with open(os.path.join(save, 'pd.ini'), 'w') as fh:
        fh.write('[Video]\nDefaultWidth=%d\nDefaultHeight=%d\n\n[Game.Player1]\nFovY=%f\n' % (w, h, fovy or 60.0))
        if hd:
            fh.write('\n[Mod]\n' + ''.join('%s=%d\n' % kv for kv in HD_KEYS.items()))


def _xenia_ge(mission, out, env, ce=True):
    """Bean through xenia/run_scenario.py: the locked rig (one Xenia and one
    virtual pad on this machine), the tour with xeniage as gdbge. Its output
    lands in OUT/bean and is moved to OUT/ge."""
    cmd = [sys.executable, os.path.join(ROOT, 'xenia', 'run_scenario.py'), os.path.join(HERE, 'tour.py'),
           '--mission', mission, '--out', out]
    for k, v in env.items():
        cmd += ['--env', '%s=%s' % (k, v)]
    e = dict(os.environ)
    # the rig runs the CE-patched release by default (agent Z's rig.sh); say so
    # only when its default is not the CE, never compare retail against our CE
    rigsh = open(os.path.join(ROOT, 'xenia', 'rig.sh')).read()
    if ce and 'CE' not in rigsh.split('PKG=', 1)[-1].split('\n', 1)[0] and os.path.exists(BEAN_CE_XEX):
        e['GF_XENIA_PKG'] = BEAN_CE_XEX
    r = subprocess.run(cmd, capture_output=True, text=True, env=e)
    bean, ge = os.path.join(out, 'bean'), os.path.join(out, 'ge')
    if os.path.isdir(bean):
        if os.path.isdir(ge):
            shutil.rmtree(ge)
        os.rename(bean, ge)
        log = os.path.join(ge, 'scenario.log')
        if os.path.exists(log):
            shutil.copy(log, os.path.join(ge, 'gdb.log'))
    ok = r.returncode == 0 and os.path.exists(os.path.join(ge, 'manifest.json'))
    return (0 if ok else 1), (r.stdout + r.stderr).strip().splitlines()[-1:]


def tour_env(a):
    env = {'VIEW_STEP': str(a.step), 'VIEW_MAX': str(a.max), 'VIEW_HEADS': a.heads}
    if a.only:
        env['VIEW_ONLY'] = a.only
    return env


def run_one(mission, a, ge_sem, pd_slots):
    m = levels.mission(mission)
    out = os.path.join(a.out, m[1])
    same = bool(getattr(a, 'reuse_oracle', None)) and os.path.abspath(a.reuse_oracle) == os.path.abspath(a.out)
    if same:
        # scoring an --oracle-only run in place: keep its oracle side, redo ours
        for d in ('pd', 'pairs'):
            shutil.rmtree(os.path.join(out, d), ignore_errors=True)
    elif os.path.isdir(out) and not a.keep:
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)
    env = tour_env(a)
    res = {}

    def ge():
        if getattr(a, 'reuse_oracle', None):
            # the oracle's pictures do not depend on our binary: take a base
            # sweep's instead of shooting them again
            src = os.path.join(a.reuse_oracle, m[1], 'ge')
            if same and os.path.exists(os.path.join(src, 'manifest.json')):
                res['ge'] = (0, ['in place ' + src])
            elif os.path.exists(os.path.join(src, 'manifest.json')):
                for d in ('ge', 'ge_repeat'):
                    if os.path.isdir(os.path.join(out, d)):
                        shutil.rmtree(os.path.join(out, d))
                    if os.path.isdir(os.path.join(a.reuse_oracle, m[1], d)):
                        shutil.copytree(os.path.join(a.reuse_oracle, m[1], d), os.path.join(out, d))
                res['ge'] = (0, ['reused ' + src])
            else:
                res['ge'] = (1, ['no oracle side to reuse in ' + src])
            return
        skip = []
        with ge_sem:
            # mission-ending pads come in runs of neighbouring waypoints (an
            # exit area: Statue 227/226/225, Archives 172/171/170), so allow six
            for attempt in range(7):
                e2 = dict(env, VIEW_SKIP=','.join(str(p) for p in skip)) if skip else env
                if a.oracle == 'xenia':
                    res['ge'] = _xenia_ge(m[1], out, dict(e2, VIEW_BEAN_FOVY=str(a.bean_fovy or 0)), ce=not a.no_ce)
                else:
                    res['ge'] = _twin('ge', m[1], out, e2, oracle=a.oracle)
                # a pad that ends the mission (the level clock went back to 0
                # there): GoldenEye's own, nothing the tour froze; try without it
                log = os.path.join(out, 'ge', 'gdb.log')
                ended = [l.split()[-1] for l in open(log, errors='replace')
                         if l.startswith('GF level ended at pad')] if os.path.exists(log) else []
                if res['ge'][0] == 0 or not ended or attempt == 6:
                    break
                skip.append(int(ended[-1]))
                print('%-10s pad %s ends the mission on GoldenEye; again without it' % (m[1], ended[-1]), flush=True)
            if res['ge'][0] == 0 and a.oracle == 'ares' and not a.no_oracle_repeat:
                # the null's third leg: the cartridge photographed twice must
                # give the same pictures (aresge.boot pins GoldenEye's RNG)
                rep = os.path.join(out, '_oracle_repeat')
                if os.path.isdir(rep):
                    shutil.rmtree(rep)
                gmf = json.load(open(os.path.join(out, 'ge', 'manifest.json')))
                e3 = dict(env, VIEW_ONLY=','.join(gmf['pads'].keys()))
                if skip:
                    e3['VIEW_SKIP'] = ','.join(str(p) for p in skip)
                r2 = _twin('ge', m[1], rep, e3, oracle=a.oracle)
                if r2[0] == 0 and os.path.isdir(os.path.join(rep, 'ge')):
                    if os.path.isdir(os.path.join(out, 'ge_repeat')):
                        shutil.rmtree(os.path.join(out, 'ge_repeat'))
                    os.rename(os.path.join(rep, 'ge'), os.path.join(out, 'ge_repeat'))
                shutil.rmtree(rep, ignore_errors=True)

    def pd():
        slot = pd_slots.get()
        try:
            res['pd'] = _twin('pd', m[1], out, env, rundir=slot, binary=getattr(a, 'bin', None))
        finally:
            pd_slots.put(slot)
    if getattr(a, 'oracle_only', False):
        ge()
        print('%-10s oracle %s' % (m[1], 'ok' if res['ge'][0] == 0 else 'FAILED %s' % res['ge'][1]), flush=True)
        return m[1], None
    if a.oracle in ('ares', 'xenia') or getattr(a, 'reuse_oracle', None):
        # the cartridge first: our window and field of view are set from the
        # viewport and fovy it reports (VideoSettings), not assumed
        ge()
        try:
            gmf = json.load(open(os.path.join(out, 'ge', 'manifest.json')))
            hd = gmf.get('oracle') == 'xenia'
            _prepare_pd(out, gmf['view']['viewport'],
                        (a.bean_fovy or 60.0) if hd else gmf['view'].get('fovy', 60.0), hd=hd)
            # the same schedule: the cartridge reaches first person later than
            # our --skip-mission-intro, so ours starts at its first-shot tick
            env = dict(env, VIEW_T0=str(gmf['t0']), VIEW_DT=str(gmf['dt']),
                       VIEW_TICKS=','.join(str(sh['tick']) for sh in gmf['shots']),
                       VIEW_ONLY=','.join(gmf['pads'].keys()), VIEW_HEADS=','.join(
                           '%g' % h for h in sorted({sh['head'] for sh in gmf['shots']})))
        except Exception:
            _prepare_pd(out, hd=a.oracle == 'xenia')
        if res['ge'][0] == 0:
            pd()
        else:
            res['pd'] = (1, ['not run: the cartridge side failed'])
    else:
        _prepare_pd(out)
        ts = [threading.Thread(target=ge), threading.Thread(target=pd)]
        [t.start() for t in ts]
        [t.join() for t in ts]
    ok = all(res[s][0] == 0 for s in res)
    print('%-10s ge %s pd %s' % (m[1], 'ok' if res['ge'][0] == 0 else 'FAILED %s' % res['ge'][1],
                                 'ok' if res['pd'][0] == 0 else 'FAILED %s' % res['pd'][1]), flush=True)
    if not ok:
        return m[1], None
    return m[1], score_dir(out)


# ------------------------------------------------------------------ scoring

def set_geometry(viewport):
    """The compare size from GoldenEye's viewport: 320 wide, its aspect,
    whole 20x20 blocks."""
    global W, H, BLOCKS
    vw, vh = viewport[2], viewport[3]
    W = 320
    H = max(20, int(round(W * vh / vw / 20.0)) * 20)
    BLOCKS = (H // 20, W // 20)


def load_ge(path, viewport):
    x, y, w, h = viewport
    img = Image.open(path).convert('RGB').crop((x, y, x + w, y + h))
    return img.resize((W, H), Image.BILINEAR) if img.size != (W, H) else img


def load_pd(path):
    return Image.open(path).convert('RGB').resize((W, H), Image.LANCZOS)


def _gray(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def _ssim_map(x, y):
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    g = lambda v: ndimage.gaussian_filter(v, 1.5)
    mx, my = g(x), g(y)
    sxx, syy, sxy = g(x * x) - mx * mx, g(y * y) - my * my, g(x * y) - mx * my
    return ((2 * mx * my + C1) * (2 * sxy + C2)) / ((mx * mx + my * my + C1) * (sxx + syy + C2))


BLUR = 2.0          # the scale the two renderers agree at (tuned on Dam: texture filtering and
                    # dithering are gone, a 64-pixel block is not)
BLOCKS = (11, 16)   # rows, columns of 20x20 blocks over 220x320
COLOUR_SCALE = 8.0  # brings the block colour term to the other two's range (Dam: matched 0.13, one pad off 0.67)


def features(img):
    a = np.asarray(img, dtype=np.float64)
    gr = _gray(a)
    b = ndimage.gaussian_filter(gr, BLUR)
    e = np.hypot(*np.gradient(b))
    return {'coarse': b[::2, ::2], 'edge': ndimage.gaussian_filter(e, 2.0),
            'blocks': a.reshape(BLOCKS[0], H // BLOCKS[0], BLOCKS[1], W // BLOCKS[1], 3).mean((1, 3)),
            'gray': gr}


def score(fa, fb):
    struct = float(1 - _ssim_map(fa['coarse'], fb['coarse']).mean())
    ea, eb = fa['edge'].ravel(), fb['edge'].ravel()
    ea, eb = ea - ea.mean(), eb - eb.mean()
    den = math.sqrt(float((ea * ea).sum() * (eb * eb).sum()))
    corr = float((ea * eb).sum() / den) if den > 0 else (1.0 if not ea.any() and not eb.any() else 0.0)
    colour = float(np.abs(fa['blocks'] - fb['blocks']).mean() / 255.0)
    parts = {'struct': max(0.0, struct), 'edge': max(0.0, 1 - corr), 'colour': colour}
    parts['score'] = (parts['struct'] + parts['edge'] + COLOUR_SCALE * colour) / 3.0
    return parts


def plant(img):
    """Our picture with a flat 64x64 block over its middle: the sensitivity
    half of the null, about a wall's worth of a near view. The block is black
    over a light region and white over a dark one, so it is a change on a grey
    level too (a mid-grey block vanished into Facility's walls)."""
    a = np.asarray(img).copy()
    region = a[60:124, 120:184]
    region[...] = 0 if region.mean() > 128 else 255
    return Image.fromarray(a)


def diff_image(fa, fb):
    """Where the pictures differ beyond what the two renderers do to the same
    picture: coarse structure loss and block colour difference, shown in red
    over GoldenEye's grey only past the matched pairs' noise."""
    loss = np.clip(1 - _ssim_map(fa['coarse'], fb['coarse']), 0, 1)
    loss = np.kron(loss, np.ones((2, 2)))[:H, :W]
    blk = np.abs(fa['blocks'] - fb['blocks']).mean(-1) / 255.0 * COLOUR_SCALE
    blk = np.kron(blk, np.ones((H // BLOCKS[0], W // BLOCKS[1])))
    m = np.maximum(loss, blk)
    alpha = np.clip((m - 0.3) / 0.5, 0, 1)
    base = fa['gray'] * 0.55
    out = np.stack([base * (1 - alpha) + 255 * alpha, base * (1 - alpha), base * (1 - alpha)], -1)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def _norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v] if n else v


def camera_check(gs, ps, off):
    ge, pd = gs['cam'], ps['cam']
    pe = [pd['eye'][k] - off[k] for k in range(3)]
    dxz = math.hypot(pe[0] - ge['eye'][0], pe[2] - ge['eye'][2])
    dy = pe[1] - ge['eye'][1]
    la, lb = _norm(ge['look']), _norm(pd['look'])
    ang = math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(la, lb))))))
    if not gs.get('tile', True):
        status = 'mismatch: no floor tile under the pad in GoldenEye'
    elif dxz > CAM_XZ or ang > CAM_ANG:
        status = 'mismatch: %.1f units apart, %.2f deg' % (dxz, ang)
    elif abs(dy) > CAM_STOREY:
        status = 'mismatch: eyes %+.0f units apart in height (a different floor)' % dy
    elif min(v for v in (ge.get('chr_near'), pd.get('chr_near'), 1e9) if v is not None) < CHR_IN_CAMERA:
        status = 'mismatch: a chr stands in the camera (GoldenEye %s, ours %s units)' % (
            ge.get('chr_near'), pd.get('chr_near'))
    elif abs(dy) > CAM_DY:
        status = 'eye %+.1f' % dy
    else:
        status = 'ok'
    return {'dxz': round(dxz, 2), 'dy': round(dy, 2), 'ang': round(ang, 3), 'status': status}


def score_dir(out):
    gm = json.load(open(os.path.join(out, 'ge', 'manifest.json')))
    pm = json.load(open(os.path.join(out, 'pd', 'manifest.json')))
    vp = gm['view']['viewport']
    set_geometry(vp)
    pairs_dir = os.path.join(out, 'pairs')
    os.makedirs(pairs_dir, exist_ok=True)
    # the level's offset from the pads both tours stood on
    offs = [[pm['pads'][p][k] - gm['pads'][p][k] for k in range(3)] for p in gm['pads'] if p in pm['pads']]
    off = [float(np.median([o[k] for o in offs])) for k in range(3)] if offs else [0.0, 0.0, 0.0]
    gshots = {(s['pad'], s['head']): s for s in gm['shots']}
    pshots = {(s['pad'], s['head']): s for s in pm['shots']}
    keys = [(s['pad'], s['head']) for s in gm['shots'] if (s['pad'], s['head']) in pshots]
    feats_g, feats_p, feats_planted, rows, problems = {}, {}, {}, [], []
    for key in keys:
        gs, ps = gshots[key], pshots[key]
        gpath = os.path.join(out, 'ge', gs['file'])
        ppath = os.path.join(out, 'pd', 'shot_%03d.png' % ps['n'])
        if not os.path.exists(gpath) or not os.path.exists(ppath):
            problems.append('pad %d heading %d: a picture is missing' % key)
            continue
        gi, pi = load_ge(gpath, vp), load_pd(ppath)
        feats_g[key], feats_p[key] = features(gi), features(pi)
        feats_planted[key] = features(plant(pi))
        tag = 'p%04d_h%03d' % (key[0], int(key[1]))
        gi.save(os.path.join(pairs_dir, tag + '_ge.png'))
        pi.save(os.path.join(pairs_dir, tag + '_pd.png'))
        cam = camera_check(gs, ps, off)
        row = {'pad': key[0], 'head': key[1], 'tag': tag, 'cam': cam, 'tick': [gs.get('tick'), ps.get('tick')]}
        if cam['status'].startswith('mismatch'):
            row['score'] = None
        else:
            row.update(score(feats_g[key], feats_p[key]))
            diff_image(feats_g[key], feats_p[key]).save(os.path.join(pairs_dir, tag + '_diff.png'))
        rows.append(row)

    # ---- the null
    null = {'identity_max': 0.0, 'control': None}
    for key in feats_g:
        null['identity_max'] = max(null['identity_max'], score(feats_g[key], feats_g[key])['score'])
    scored = [r for r in rows if r.get('score') is not None]
    pads = sorted({k[0] for k in feats_g}, key=lambda p: list(gm['pads']).index(str(p)))
    heads = sorted({k[1] for k in feats_g})
    matched, offset, wins = [], [], 0
    for r in scored:
        key = (r['pad'], r['head'])
        if len(pads) > 1:
            other = (pads[(pads.index(r['pad']) + 1) % len(pads)], r['head'])
            kind = 'next pad'
        else:
            other = (r['pad'], heads[(heads.index(r['head']) + 1) % len(heads)])
            kind = 'next heading'
        if other not in feats_p or other == key:
            continue
        s_off = score(feats_g[key], feats_p[other])['score']
        matched.append(r['score'])
        offset.append(s_off)
        wins += r['score'] < s_off
        r['control'] = round(s_off, 4)
    if matched:
        mm, mo = float(np.median(matched)), float(np.median(offset))
        share = wins / len(matched)
        ok = null['identity_max'] < 1e-6 and mm < CONTROL_RATIO * mo and share >= CONTROL_WIN
        null['control'] = {'joined_with': kind, 'pairs': len(matched), 'matched_median': round(mm, 4),
                           'offset_median': round(mo, 4), 'matched_better_share': round(share, 3), 'pass': ok}
    else:
        null['control'] = {'pass': False, 'why': 'nothing to join against'}
    # the oracle against itself: the same cartridge run twice
    repdir = os.path.join(out, 'ge_repeat')
    if os.path.exists(os.path.join(repdir, 'manifest.json')):
        rm = json.load(open(os.path.join(repdir, 'manifest.json')))
        rshots = {(sh['pad'], sh['head']): sh for sh in rm['shots']}
        jit = []
        for r in rows:
            key = (r['pad'], r['head'])
            if key in rshots and key in feats_g:
                fr = features(load_ge(os.path.join(repdir, rshots[key]['file']), rm['view']['viewport']))
                j = score(feats_g[key], fr)['score']
                r['oracle_jitter'] = round(j, 4)
                jit.append(j)
                if j > ORACLE_UNSTEADY and r.get('score') is not None:
                    r['score'] = None
                    r['cam']['status'] = 'mismatch: GoldenEye\'s own picture changes between two runs (%.3f)' % j
        scored = [r for r in rows if r.get('score') is not None]
        if jit:
            med = float(np.median(jit))
            mo = null['control'].get('offset_median') or 0.0
            mm = null['control'].get('matched_median') or 0.0
            null['oracle_repeat'] = {'pairs': len(jit), 'median': round(med, 4), 'p90': round(float(np.percentile(jit, 90)), 4),
                                     'max': round(max(jit), 4), 'unsteady': sum(1 for j in jit if j > ORACLE_UNSTEADY),
                                     'vs_offset': round(med / mo, 3) if mo else None,
                                     'vs_matched': round(med / mm, 3) if mm else None,
                                     'pass': bool(mo) and med <= ORACLE_REPEAT_RATIO * mo and med <= ORACLE_REPEAT_MAX}
            if not null['oracle_repeat']['pass']:
                null['control']['pass'] = False
                problems.append('oracle repeat: GoldenEye photographed twice differs by a median %.3f (allowed: %.0f%% of '
                                'the one-pad-off median %.3f, and %.2f)' % (med, 100 * ORACLE_REPEAT_RATIO, mo, ORACLE_REPEAT_MAX))
    # sensitivity: the planted block must raise nearly every matched pair's score
    rises = [score(feats_g[(r['pad'], r['head'])], feats_planted[(r['pad'], r['head'])])['score'] - r['score']
             for r in scored]
    if rises:
        seen = sum(1 for d in rises if d >= PLANT_RISE) / len(rises)
        null['sensitivity'] = {'planted_rise_median': round(float(np.median(rises)), 4), 'seen_share': round(seen, 3),
                               'pass': seen >= PLANT_SEEN}
        if seen < PLANT_SEEN:
            null['control']['pass'] = False
            problems.append('sensitivity: a planted 64x64 block raised only %.0f%% of pairs by %.2f' % (100 * seen, PLANT_RISE))
    if null['identity_max'] >= 1e-6:
        null['control']['pass'] = False
        problems.append('identity: a picture scored %.3g against itself' % null['identity_max'])

    rows.sort(key=lambda r: -(r['score'] if r.get('score') is not None else -1))
    for r in rows:
        for k in ('score', 'struct', 'edge', 'colour'):
            if r.get(k) is not None:
                r[k] = round(r[k], 4)
    summary = {'mission': levels.MISSIONS[gm['mission']][1], 'pairs': len(rows), 'scored': len(scored),
               'mismatches': sum(1 for r in rows if r.get('score') is None),
               'median_score': round(float(np.median([r['score'] for r in scored])), 4) if scored else None,
               'median_eye_dy': round(float(np.median([r['cam']['dy'] for r in rows])), 2) if rows else None,
               'offset': [round(v, 3) for v in off], 'null': null, 'problems': problems,
               'ranked': bool(null['control'].get('pass')), 'frozen': [gm.get('frozen'), pm.get('frozen')],
               'oracle': gm.get('oracle', 'port'), 'ge_view': gm['view'], 'skipped': gm.get('skipped', [])}
    json.dump({'summary': summary, 'rows': rows}, open(os.path.join(out, 'scores.json'), 'w'), indent=1)
    write_html(out, summary, rows)
    return summary


# ------------------------------------------------------------------ report

CSS = """body{font:13px/1.4 system-ui,sans-serif;margin:16px;background:#111;color:#ddd}
table{border-collapse:collapse}td,th{border-bottom:1px solid #333;padding:4px 6px;vertical-align:top}
img{width:320px;height:220px;image-rendering:pixelated;display:block}.bad{color:#f66}.ok{color:#6c6}
.warn{color:#fc6}a{color:#8cf}.sm{color:#999;font-size:12px}"""


def write_html(out, s, rows):
    ctl = s['null']['control']
    sen = s['null'].get('sensitivity', {})
    banner = ('<p class="ok">Null passed: identity max %.2g; matched median %.4f vs %s %.4f, matched better on %.0f%% '
              'of %d pairs; a planted block raised %.0f%% of pairs (median +%.3f)%s. Ranked worst first.</p>' % (
                  s['null']['identity_max'], ctl['matched_median'], ctl['joined_with'], ctl['offset_median'],
                  100 * ctl['matched_better_share'], ctl['pairs'], 100 * sen.get('seen_share', 0),
                  sen.get('planted_rise_median', 0),
                  ('; GoldenEye photographed twice: median %.4f, max %.4f, %d pairs unsteady (not ranked)' % (
                      s['null']['oracle_repeat']['median'], s['null']['oracle_repeat']['max'],
                      s['null']['oracle_repeat']['unsteady'])) if 'oracle_repeat' in s['null'] else '')
              if s['ranked'] else
              '<p class="bad"><b>NULL FAILED - not ranked.</b> %s</p>' % html.escape(json.dumps(ctl)))
    h = ['<!doctype html><meta charset="utf-8"><title>View diff: %s</title><style>%s</style>' % (s['mission'], CSS),
         '<h1>View diff: %s</h1>' % html.escape(s['mission']),
         '<p class="sm">GoldenEye: %s (%s); viewport %s, fovy %s</p>' % (
             'the cartridge in ares' if s.get('oracle') == 'ares' else 'the decomp\'s native port',
             s.get('oracle'), s['ge_view'].get('viewport'), s['ge_view'].get('fovy', 60)), banner,
         '<p>%d pairs, %d scored, %d camera mismatches; median score %s; median eye height ours - GoldenEye %s units; '
         'offset %s; chrs frozen (GoldenEye, ours) %s.</p>' % (s['pairs'], s['scored'], s['mismatches'],
                                                               s['median_score'], s['median_eye_dy'], s['offset'],
                                                               s['frozen'])]
    for p in s['problems']:
        h.append('<p class="warn">%s</p>' % html.escape(p))
    if s.get('skipped'):
        h.append('<p class="warn">Left out: pad %s - putting Bond there ended the mission on GoldenEye\'s side '
                 '(nothing the tour froze; an exit the level checks itself).</p>' % ', '.join(map(str, s['skipped'])))
    h.append('<p class="sm">score = mean of struct (1-SSIM at blur %.0f, half resolution), edge (1-correlation '
             'of blurred edge maps) and %.0f x colour (mean difference of 20x20 block colours, 0..1); 0 is the same '
             'picture. control = the same GoldenEye picture against our next pad. Red in the difference map is past the '
             'two renderers\' noise.</p>' % (BLUR, COLOUR_SCALE))
    h.append('<table><tr><th>#</th><th>pad / heading</th><th>GoldenEye</th><th>ours</th><th>difference</th></tr>')
    for i, r in enumerate(rows):
        if r.get('score') is None:
            sc = '<span class="bad">%s</span>' % html.escape(r['cam']['status'])
        else:
            sc = ('<b>%.3f</b><br>struct %.3f<br>edge %.3f<br>colour %.3f<br>control %s<br>camera %s' % (
                r['score'], r['struct'], r['edge'], r['colour'], r.get('control', '-'),
                html.escape(r['cam']['status'])))
        diff = ('<img loading="lazy" src="pairs/%s_diff.png">' % r['tag']) if r.get('score') is not None else ''
        h.append('<tr id="%s"><td>%d</td><td>pad %d<br>heading %d<br>%s<br><span class="sm">ticks %s</span></td>'
                 '<td><img loading="lazy" src="pairs/%s_ge.png"></td><td><img loading="lazy" src="pairs/%s_pd.png"></td>'
                 '<td>%s</td></tr>' % (r['tag'], i + 1, r['pad'], r['head'], sc, r['tick'], r['tag'], r['tag'], diff))
    h.append('</table>')
    open(os.path.join(out, 'index.html'), 'w').write('\n'.join(h))


def write_sweep_index(out, results):
    h = ['<!doctype html><meta charset="utf-8"><title>View diff sweep</title><style>%s</style>' % CSS,
         '<h1>View diff sweep</h1><table><tr><th>mission</th><th>pairs</th><th>median</th><th>worst</th>'
         '<th>eye dy</th><th>null</th></tr>']
    for name, s in results:
        if s is None:
            h.append('<tr><td>%s</td><td colspan=5 class="bad">run failed - not compared</td></tr>' % name)
            continue
        worst = ''
        try:
            rows = json.load(open(os.path.join(out, name, 'scores.json')))['rows']
            worst = ' '.join('%.3f' % r['score'] for r in rows[:3] if r.get('score') is not None)
        except Exception:
            pass
        h.append('<tr><td><a href="%s/index.html">%s</a></td><td>%d</td><td>%s</td><td>%s</td><td>%s</td>'
                 '<td class="%s">%s</td></tr>' % (name, name, s['pairs'], s['median_score'], worst,
                                                   s['median_eye_dy'], 'ok' if s['ranked'] else 'bad',
                                                   'pass' if s['ranked'] else 'FAILED'))
    h.append('</table>')
    open(os.path.join(out, 'index.html'), 'w').write('\n'.join(h))


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('run', 'sweep'):
        p = sub.add_parser(name)
        p.add_argument('--oracle', choices=['ares', 'port', 'xenia'], default='ares',
                       help='GoldenEye: the cartridge in ares (default), the native port under gdb, or the '
                            'XBLA release in Xenia against our HD look')
        p.add_argument('--bean-fovy', type=float, default=None,
                       help='--oracle xenia: our FovY for Bean (measure it: viewdiff.py fovfit DIR)')
        p.add_argument('--no-ce', action='store_true', help='--oracle xenia: Bean without the Community Edition')
        p.add_argument('--oracle-only', action='store_true',
                       help="GoldenEye's pictures only; score later with --reuse-oracle on the same OUT")
        p.add_argument('--no-oracle-repeat', action='store_true',
                       help='ares: do not photograph the cartridge a second time for the null')
        p.add_argument('--out', default=None, help='default ~/wt/gefidelity-run/view-ares (view-out with --oracle port)')
        p.add_argument('--step', type=int, default=6, help='every Nth waypoint pad')
        p.add_argument('--max', type=int, default=40, help='pads per mission, 0 = all')
        p.add_argument('--heads', default='0,90,180,270')
        p.add_argument('--only', help='comma list of pads (overrides --step)')
        p.add_argument('--keep', action='store_true', help='do not clear the mission directory first')
        p.add_argument('--bin', default='./pd.base', help='our binary, relative to ~/wt/gefidelity-run')
        p.add_argument('--reuse-oracle', help="a previous run's OUT: take GoldenEye's pictures from it")
        if name == 'run':
            p.add_argument('--mission', required=True)
        else:
            p.add_argument('--missions', default='all')
            p.add_argument('--ge-jobs', type=int, default=4)
            p.add_argument('--pd-jobs', type=int, default=1, help='ours at once (one GPU, shared)')
    p = sub.add_parser('score')
    p.add_argument('dirs', nargs='+')
    a = ap.parse_args()

    if a.cmd == 'score':
        bad = 0
        for d in a.dirs:
            s = score_dir(d)
            print(json.dumps(s))
            bad |= not s['ranked']
        return 2 if bad else 0

    a.out = a.out or DEFAULT_OUT[a.oracle]
    import queue
    subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, %r); import twin; twin.sync_tools()' % ROOT],
                   check=True)
    slots = queue.Queue()
    rundirs = _rundirs(a.bin, hd=a.oracle == 'xenia')
    njobs = 1 if a.cmd == 'run' else a.pd_jobs
    for d in rundirs[:max(1, min(njobs, len(rundirs)))]:
        slots.put(d)
    ge_sem = threading.Semaphore(1 if a.cmd == 'run' or a.oracle == 'xenia' else a.ge_jobs)
    ms = [a.mission] if a.cmd == 'run' else (
        [m[1] for m in levels.MISSIONS] if a.missions == 'all' else a.missions.split(','))
    os.makedirs(a.out, exist_ok=True)
    workers = 1 if a.cmd == 'run' else max(a.ge_jobs, a.pd_jobs)
    with cf.ThreadPoolExecutor(workers) as ex:
        results = list(ex.map(lambda m: run_one(m, a, ge_sem, slots), ms))
    for name, s in results:
        if s:
            print('%-10s pairs %3d median %s eye dy %s null %s -> %s' % (
                name, s['pairs'], s['median_score'], s['median_eye_dy'], 'pass' if s['ranked'] else 'FAILED',
                os.path.join(a.out, name, 'index.html')))
    if a.cmd == 'sweep':
        write_sweep_index(a.out, results)
        print('sweep index:', os.path.join(a.out, 'index.html'))
    return 0 if all(s and s['ranked'] for _, s in results) else (2 if any(s and not s['ranked'] for _, s in results) else 1)


if __name__ == '__main__':
    sys.exit(main())

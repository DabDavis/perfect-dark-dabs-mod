#!/usr/bin/env python3
"""Run guns/gunscen.py over GoldenEye's guns on both sides, in groups.

    guns/run.py --out ~/wt/gefidelity-run/guns-out/run1 [--guns pp7,kf7] [--mission dam]
                [--ge-jobs 3] [--pd-jobs 2] [--sides ge,pd] [--hold 480]

Writes OUT/<side>/gun_<item>.json. The oracle side goes through twin.py's
run_ge (rendering off). Our side cannot use twin.py's: it passes --no-sound,
and with no sound bank GE Plus never starts a GoldenEye sound at all
(geSfxGet() returns 0), so the sound column would be empty by construction.
Here every group gets its own run directory of links (its own pd.log and
savedir), SDL's dummy audio driver, and the bank loaded.
"""
import argparse, os, shutil, subprocess, sys, time, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'common'))
import twin, levels  # noqa: E402
import gunlist  # noqa: E402


import threading
_LAUNCH = threading.Lock()


def groups(items, n):
    n = max(1, min(n, len(items)))
    return [items[i::n] for i in range(n)]


def run_ge_group(items, m, diff, out, hold, timeout, render=False):
    env = {'GF_GUNS': ','.join(map(str, items)), 'GF_GUNSDIR': '$HOME/gefidelity/guns', 'GF_HOLD': str(hold)}
    sub = os.path.join(out, 'ge-%s' % '-'.join(map(str, items)))
    # twin.run_ge() names the oracle's directory by the second and our pid, so
    # two launched in one second would share it (guns/shared-lib.patch)
    with _LAUNCH:
        p = twin.run_ge(os.path.join(HERE, 'gunscen.py'), m, diff, sub, env, timeout, render)
        time.sleep(1.2)
    p.wait()
    twin.fetch_ge(p)
    os.makedirs(os.path.join(out, 'ge'), exist_ok=True)
    for f in os.listdir(p.gf_local):
        if f.startswith('gun_'):
            shutil.copy(os.path.join(p.gf_local, f), os.path.join(out, 'ge', f))
    log = os.path.join(p.gf_local, 'gdb.log')
    return 'ge', items, open(log, errors='replace').read() if os.path.exists(log) else 'GF FAILED (no log)'


def run_pd_group(items, m, diff, out, hold, timeout, rundir, binary):
    tag = 'pd-%s' % '-'.join(map(str, items))
    rd = os.path.abspath(os.path.join(out, tag))
    os.makedirs(rd, exist_ok=True)
    for name in ('data', 'added-content', 'mods'):
        if not os.path.exists(os.path.join(rd, name)):
            os.symlink(os.path.join(rundir, name), os.path.join(rd, name))
    b = os.path.join(rd, 'pd.bin')
    if not os.path.exists(b):
        os.symlink(os.path.abspath(os.path.join(rundir, binary)), b)
    os.makedirs(os.path.join(rd, 'save'), exist_ok=True)
    os.makedirs(os.path.join(out, 'pd'), exist_ok=True)
    e = dict(os.environ)
    e.update({'SDL_VIDEODRIVER': os.environ.get('SDL_VIDEODRIVER', 'offscreen'), 'SDL_AUDIODRIVER': 'dummy',
              'GF_SIDE': 'pd', 'GF_COMMON': os.path.join(ROOT, 'common'), 'GF_GUNSDIR': HERE,
              'GF_LEVELID': m[3], 'GF_MISSION': str(m[0]), 'GF_DIFF': str(diff),
              'GF_OUT': os.path.join(os.path.abspath(out), 'pd'), 'GF_GUNS': ','.join(map(str, items)),
              'GF_HOLD': str(hold)})
    args = ['timeout', '-k', '5', str(timeout), 'gdb', '-batch', '-x', os.path.join(HERE, 'gunscen.py'), '--args',
            './pd.bin', '--savedir', os.path.join(rd, 'save'), '--skip-intro', '--boot-ge-mission', str(m[0]),
            '--skip-mission-intro', '--fixed-step', '--rng-seed', '1', '--log']
    with open(os.path.join(rd, 'gdb.log'), 'w') as log:
        subprocess.run(args, cwd=rd, env=e, stdout=log, stderr=subprocess.STDOUT)
    return 'pd', items, open(os.path.join(rd, 'gdb.log'), errors='replace').read()


def run(out, guns, mission='dam', diff='agent', sides=('ge', 'pd'), ge_jobs=3, pd_jobs=2, hold=480,
        timeout=3000, rundir=twin.DEFAULT_RUNDIR, binary='./pd.base', render=False):
    m = levels.mission(mission)
    d = levels.difficulty(diff)
    items = [g[0] for g in guns]
    os.makedirs(out, exist_ok=True)
    jobs = []
    if 'ge' in sides:
        twin.sync_tools()
    with cf.ThreadPoolExecutor(ge_jobs + pd_jobs) as ex:
        if 'ge' in sides:
            jobs += [ex.submit(run_ge_group, g, m, d, out, hold, timeout, render) for g in groups(items, ge_jobs)]
        if 'pd' in sides:
            jobs += [ex.submit(run_pd_group, g, m, d, out, hold, timeout, rundir, binary) for g in groups(items, pd_jobs)]
        bad = []
        for j in cf.as_completed(jobs):
            side, its, log = j.result()
            ok = 'GF FAILED' not in log and all(os.path.exists(os.path.join(out, side, 'gun_%d.json' % i)) for i in its)
            print('%s %s: %s' % (side, its, 'ok' if ok else 'FAILED'), flush=True)
            if not ok:
                bad.append((side, its, log.splitlines()[-12:]))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--guns', default='all')
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--diff', default='agent')
    ap.add_argument('--sides', default='ge,pd')
    ap.add_argument('--ge-jobs', type=int, default=3)
    ap.add_argument('--pd-jobs', type=int, default=2)
    ap.add_argument('--hold', type=int, default=480)
    ap.add_argument('--timeout', type=int, default=3000)
    ap.add_argument('--rundir', default=twin.DEFAULT_RUNDIR)
    ap.add_argument('--bin', default='./pd.base')
    ap.add_argument('--render', action='store_true', help='the oracle renders (slower; console-like frame pace)')
    a = ap.parse_args()
    guns = gunlist.GUNS if a.guns == 'all' else [gunlist.gun(k) for k in a.guns.split(',')]
    bad = run(a.out, guns, a.mission, a.diff, a.sides.split(','), a.ge_jobs, a.pd_jobs, a.hold, a.timeout,
              a.rundir, a.bin, a.render)
    for side, its, tail in bad:
        print('---', side, its, *tail, sep='\n')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

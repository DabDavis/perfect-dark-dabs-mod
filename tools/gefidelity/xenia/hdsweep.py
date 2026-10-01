#!/usr/bin/env python3
"""The world diff for the HD look: our port in the XBLA look against GoldenEye
XBLA ("Bean") itself, mission by mission, in world/sweep.py's finding format.

    xenia/hdsweep.py --out DIR [--bin ./pd.base] [--missions dam,facility] [--diff agent]
                     [--reuse-oracle DIR] [--n64 DIR] [--null-repeat]

Per mission: Bean's world (xenia/run_scenario.py world/dump.py, through the
Xenia rig under its machine-wide lock - one mission at a time), ours in the HD
look (twin.run_pd in a run directory of its own under --rundir, its pd.ini from
--ini), and the N64 oracle's world (--n64: a world/sweep.py output with
<mission>/ge) to align Bean's records to GoldenEye's. xenia/beandiff.py then
gives:
- "bean.*" findings: Bean's own deliberate changes from the N64 ROM (hats on
  every Dam guard, 4J's record sizes, extra pads, room lists...). Listed apart
  in report.md; never a conversion fault of ours.
- the world diff's own kinds for ours against Bean (Bean re-indexed into the
  N64's record order), keyed by the N64 record index like world/sweep.py's.

DIR/report.json has every finding ({mission, kind, key, detail, mag}),
DIR/report.md the two lists apart. --reuse-oracle DIR takes Bean's dumps from
an earlier run's DIR/<mission>/bean (they do not depend on our binary), so a
gate's base and test sweeps run Xenia once. The nulls: beandiff's on every
mission (Bean against itself clean, a planted drop and move found), and with
--null-repeat our side is dumped twice on the first mission and must give the
same placement findings both times.

Time: Bean's side is ~75 s a mission (Xenia boot and the folder screens), ours
~10 s; all twenty ~30 min, plus any wait for the Xenia lock.
"""
import argparse, json, os, shutil, subprocess, sys, time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
sys.path.insert(0, os.path.join(ROOT, 'world'))
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)
import levels  # noqa: E402
import twin  # noqa: E402
import worlddiff  # noqa: E402
import beandiff  # noqa: E402

RUN = os.path.expanduser('~/wt/gefidelity-run')


def bean_side(m, a, out):
    dst = os.path.join(out, m[1], 'bean')
    if a.reuse_oracle:
        src = os.path.join(a.reuse_oracle, m[1], 'bean')
        if not os.path.isdir(src):
            return 'no Bean dump to reuse in %s' % src
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst)
        return None
    r = subprocess.run([sys.executable, os.path.join(HERE, 'run_scenario.py'), os.path.join(ROOT, 'world', 'dump.py'),
                        '--mission', m[1], '--diff', a.diff, '--out', os.path.join(out, m[1]),
                        '--env', 'GF_TICKS=' + a.ticks], capture_output=True, text=True)
    return None if r.returncode == 0 else (r.stdout + r.stderr).strip().splitlines()[-1:]


def ours_side(m, a, out, tag='pd'):
    side = os.path.join(out, m[1])
    base = side if tag == 'pd' else os.path.join(side, tag + '-x')
    save = os.path.join(base, 'pd', 'save')            # where twin.run_pd puts --savedir
    os.makedirs(save, exist_ok=True)
    shutil.copy(a.ini, os.path.join(save, 'pd.ini'))    # the HD look's settings
    env = {'GF_TICKS': a.ticks}
    p = twin.run_pd(os.path.join(ROOT, 'world', 'dump.py'), m, levels.difficulty(a.diff),
                    base, env, a.timeout, a.rundir, a.bin, [])
    p.wait()
    twin.finish_pd(p)
    if tag != 'pd':
        # twin writes OUT/pd; the repeat lives beside it
        src = os.path.join(side, tag + '-x', 'pd')
        dst = os.path.join(side, tag)
        shutil.rmtree(dst, ignore_errors=True)
        shutil.move(src, dst)
        shutil.rmtree(os.path.join(side, tag + '-x'), ignore_errors=True)
    d = os.path.join(side, tag)
    return None if beandiff.ticks(d) else 'our dump wrote nothing (%s/gdb.log)' % d


def placement_keys(findings):
    return sorted((f['kind'], f['key']) for f in findings
                  if not f['kind'].startswith('bean.') and f['kind'] not in worlddiff.BEHAVIOUR)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--missions', default='all')
    ap.add_argument('--diff', default='agent')
    ap.add_argument('--ticks', default='1,300')
    ap.add_argument('--bin', default='./pd.base', help='our binary, relative to --rundir')
    ap.add_argument('--rundir', default=os.path.join(RUN, 'xenia-out', 'hd-run'),
                    help='the HD-look base: data/, added-content/ (the ROM and goldeneye -> Bean), mods/')
    ap.add_argument('--ini', default=None, help='pd.ini for the HD look (default RUNDIR/hd-pd.ini)')
    ap.add_argument('--n64', default=os.path.join(RUN, 'out', 'sweep5'),
                    help='a world/sweep.py output: <mission>/ge is the N64 oracle\'s world')
    ap.add_argument('--reuse-oracle', default=None, help='take Bean\'s dumps from DIR/<mission>/bean')
    ap.add_argument('--null-repeat', action='store_true', help='dump ours twice on the first mission')
    ap.add_argument('--timeout', type=int, default=600)
    a = ap.parse_args()
    a.ini = a.ini or os.path.join(a.rundir, 'hd-pd.ini')
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    ms = levels.MISSIONS if a.missions == 'all' else [levels.mission(k) for k in a.missions.split(',')]
    findings, failed = [], {}
    for k, m in enumerate(ms):
        t = time.time()
        err = bean_side(m, a, out) or ours_side(m, a, out)
        n64 = os.path.join(a.n64, m[1], 'ge')
        if not err and not beandiff.ticks(n64):
            err = 'no N64 world for %s in %s' % (m[1], n64)
        if err:
            failed[m[1]] = err
            print('%-10s FAILED %s' % (m[1], err), flush=True)
            continue
        try:
            f = beandiff.run(os.path.join(out, m[1], 'bean'), n64, os.path.join(out, m[1], 'pd'), m[1])
        except SystemExit as e:
            print(e)
            return 2
        if a.null_repeat and k == 0:
            err = ours_side(m, a, out, 'pd2')
            f2 = beandiff.run(os.path.join(out, m[1], 'bean'), n64, os.path.join(out, m[1], 'pd2'), m[1]) if not err else []
            if err or placement_keys(f) != placement_keys(f2):
                print('NULL FAILED: two runs of the same binary gave different placement findings on %s' % m[1])
                return 2
            print('null: two runs of our binary gave the same %d placement findings on %s' % (
                len(placement_keys(f)), m[1]), flush=True)
        findings += f
        print('%-10s %d findings (%d Bean\'s own) %.0fs' % (m[1], len(f), sum(1 for x in f if x['kind'].startswith('bean.')),
                                                           time.time() - t), flush=True)
    json.dump(findings, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    lines = ['# HD world diff: ours in the XBLA look against GoldenEye XBLA (Bean), %d missions, %s' % (len(ms), a.diff), '']
    if failed:
        lines += ['**Not compared:** ' + ', '.join('%s (%s)' % kv for kv in failed.items()), '']
    for title, pick in (('Ours against Bean', lambda x: not x['kind'].startswith('bean.')),
                        ('Bean\'s own changes from the N64 ROM (bean.*, not ours to fix)', lambda x: x['kind'].startswith('bean.'))):
        lines += ['## ' + title, '']
        by = defaultdict(lambda: defaultdict(list))
        for x in findings:
            if pick(x):
                by[x['mission']][x['kind']].append(x)
        for mission in [m[1] for m in ms if m[1] in by]:
            lines += ['### %s' % mission, '']
            for kind in sorted(by[mission], key=lambda k: (k in worlddiff.BEHAVIOUR, k)):
                xs = sorted(by[mission][kind], key=lambda x: -x['mag'])
                lines.append('- **%s** (%d): %s%s' % (kind, len(xs), xs[0]['detail'], '' if len(xs) == 1 else ' ...'))
            lines.append('')
    open(os.path.join(out, 'report.md'), 'w').write('\n'.join(lines))
    print('report:', os.path.join(out, 'report.md'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

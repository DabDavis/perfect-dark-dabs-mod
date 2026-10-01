#!/usr/bin/env python3
"""The gun diff as the fidelity gate's 'guns' leg: one binary's sweep, written
as findings in worlddiff's shape so world/compare.py can set two sweeps side by
side.

    guns/gatesweep.py --bin ./pd.fix --out DIR [--oracle ares|port] [--reuse-oracle BASE_DIR]
                      [--mission dam] [--guns all]

Writes DIR/{ge,pd}/gun_*.json and DIR/repeat/{ge,pd}/ (the controls' second
run), DIR/gundiff.md + gundiff.json (the full table), DIR/report.json and
DIR/report-missions.json.

A finding is one quantity of one gun that disagrees beyond gundiff.py's
tolerance: mission = the mission measured on, kind = gun.<quantity>
(gun.reload_refill, gun.raise, gun.cadence, gun.casings, gun.fuse,
gun.sound_hold, ...), key = the gun's name, mag = how far beyond the
tolerance in ticks or counts (1 for a categorical mismatch: a sound, a mode, a
value present on one side only), detail = both sides' values. Agreeing
quantities make no finding.

--reuse-oracle takes the cartridge's measurements (and its repeat run) from a
base sweep's directory: they do not depend on our binary, so a gate measures
the cartridge once. Their oracle must be the one asked for.

Exit 2 and no report when gundiff's controls fail (a gun run twice on one side
disagrees, or a planted clip fault is not the one mismatch) or when a gun has
no measurement on either side - a gun that did not run must never read as a
mismatch fixed.
"""
import argparse, glob, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run as gunrun  # noqa: E402
import gunlist  # noqa: E402
import gundiff  # noqa: E402

KIND = {'raise_ticks': 'raise', 'casings_per_use': 'casings', 'impacts_per_use': 'impacts',
        'release_latency': 'release', 'dry_click_interval': 'dry_click', 'dual_allguns': 'dual',
        'clip_drawn': 'clip', 'empties_in_hold': 'empties'}


def kind(q):
    if q.startswith('sounds_'):
        return 'gun.sound_' + q[7:]
    return 'gun.' + KIND.get(q, q)


def magnitude(q, a, b, ge):
    num = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)
    if num(a) and num(b):
        return round(max(abs(a - b) - gundiff.tolerance(q, ge), 0.01), 2)
    return 1.0


def reuse(base, out, oracle):
    for sub in ('ge', os.path.join('repeat', 'ge')):
        src = os.path.join(base, sub)
        files = sorted(glob.glob(os.path.join(src, 'gun_*.json')))
        if not files:
            raise SystemExit('--reuse-oracle %s has no %s/gun_*.json' % (base, sub))
        got = {json.load(open(f)).get('oracle', 'port') for f in files}
        if got != {oracle}:
            raise SystemExit('--reuse-oracle %s measured on %s, not %s' % (base, ', '.join(sorted(got)), oracle))
        dst = os.path.join(out, sub)
        os.makedirs(dst, exist_ok=True)
        for f in files:
            shutil.copy(f, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', required=True, help='our binary, relative to the run directory')
    ap.add_argument('--out', required=True)
    ap.add_argument('--oracle', default='ares', choices=['ares', 'port'])
    ap.add_argument('--reuse-oracle', help="a base sweep's directory whose GoldenEye measurements to take")
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--guns', default='all')
    ap.add_argument('--repeat-gun', default='pp7')
    ap.add_argument('--ge-jobs', type=int, default=3)
    ap.add_argument('--pd-jobs', type=int, default=1)
    ap.add_argument('--timeout', type=int, default=5400)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    guns = gunlist.GUNS if a.guns == 'all' else [gunlist.gun(k) for k in a.guns.split(',')]
    rep = [gunlist.gun(a.repeat_gun)]
    sides = ('pd',) if a.reuse_oracle else ('ge', 'pd')
    if a.reuse_oracle:
        reuse(os.path.abspath(a.reuse_oracle), out, a.oracle)
    bad = gunrun.run(out, guns, a.mission, sides=sides, ge_jobs=a.ge_jobs, pd_jobs=a.pd_jobs, timeout=a.timeout,
                     binary=a.bin, oracle=a.oracle)
    bad += gunrun.run(os.path.join(out, 'repeat'), rep, a.mission, sides=sides, ge_jobs=1, pd_jobs=1,
                      timeout=a.timeout, binary=a.bin, oracle=a.oracle)
    for side, its, tail in bad:
        print('--- run failed:', side, its, *tail, sep='\n')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'gundiff.py'), out,
                        '--md', os.path.join(out, 'gundiff.md'), '--json', os.path.join(out, 'gundiff.json')],
                       stdout=subprocess.PIPE, text=True)
    first = r.stdout.splitlines()[:3]
    if r.returncode == 2 or not os.path.exists(os.path.join(out, 'gundiff.json')):
        print('\n'.join(first))
        print('gun gate: controls failed - no report')
        return 2
    wanted = {g[1] for g in guns}
    table = [row for row in json.load(open(os.path.join(out, 'gundiff.json'))) if row['gun'] in wanted]
    missing = [(row['gun'], row['missing']) for row in table if 'missing' in row]
    if missing:
        print('gun gate: not measured on every side: %s - no report' % missing)
        return 2
    findings = []
    for row in table:
        for mm in row.get('mismatches', []):
            q = mm['quantity']
            findings.append({'mission': a.mission, 'kind': kind(q), 'key': row['gun'],
                             'mag': magnitude(q, mm['ge'], mm['pd'], row['ge']),
                             'detail': '%s %s: GoldenEye %s, ours %s%s' % (
                                 row['gun'], q, gundiff.fmt(mm['ge'], q.startswith('sounds_')),
                                 gundiff.fmt(mm['pd'], q.startswith('sounds_')),
                                 (' (%s)' % mm['note']) if mm.get('note') else '')})
    json.dump(findings, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    json.dump({'compared': [a.mission], 'oracle': a.oracle, 'guns': [g[1] for g in guns]},
              open(os.path.join(out, 'report-missions.json'), 'w'))
    print('\n'.join(first[:1]))
    print('gun gate: %d findings over %d guns (%s)' % (len(findings), len(guns), os.path.join(out, 'report.json')))
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())

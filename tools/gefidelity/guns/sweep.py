#!/usr/bin/env python3
"""The gun diff end to end: every gun on both sides, the repeat control, the report.

    guns/sweep.py --out ~/wt/gefidelity-run/guns-out/sweep1 [--guns all] [--repeat-gun pp7] [--mission dam]

Writes OUT/{ge,pd}/gun_*.json, OUT/repeat/{ge,pd}/gun_*.json, OUT/report.md and
OUT/report.json. Exit 0 all agree, 1 mismatches, 2 a control failed (no report).
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run as gunrun  # noqa: E402
import gunlist  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--guns', default='all')
    ap.add_argument('--repeat-gun', default='pp7')
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--ge-jobs', type=int, default=3)
    ap.add_argument('--pd-jobs', type=int, default=2)
    ap.add_argument('--bin', default='./pd.base')
    ap.add_argument('--skip-run', action='store_true', help='report on what is already in OUT')
    a = ap.parse_args()
    guns = gunlist.GUNS if a.guns == 'all' else [gunlist.gun(k) for k in a.guns.split(',')]
    if not a.skip_run:
        bad = gunrun.run(a.out, guns, a.mission, ge_jobs=a.ge_jobs, pd_jobs=a.pd_jobs, binary=a.bin)
        bad += gunrun.run(os.path.join(a.out, 'repeat'), [gunlist.gun(a.repeat_gun)], a.mission, ge_jobs=1,
                          pd_jobs=1, binary=a.bin)
        for side, its, tail in bad:
            print('--- run failed:', side, its, *tail, sep='\n')
    return subprocess.run([sys.executable, os.path.join(HERE, 'gundiff.py'), a.out]).returncode


if __name__ == '__main__':
    sys.exit(main())

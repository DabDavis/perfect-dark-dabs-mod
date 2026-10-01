#!/usr/bin/env python3
"""The world diff over every mission: dump both sides, diff, one report.

    world/sweep.py --out ~/wt/gefidelity-run/out/sweep [--missions dam,facility] [--diff agent] [-j 4]

Writes OUT/<mission>/{ge,pd}/world_t*.json, OUT/report.md and OUT/report.json.
A mission whose dump failed on either side is listed at the top of the report
as not compared - never silently dropped.
"""
import argparse, os, subprocess, sys, concurrent.futures as cf
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
import levels  # noqa: E402
sys.path.insert(0, ROOT)
import twin  # noqa: E402


def one(m, a):
    out = os.path.join(a.out, m[1])
    cmd = [sys.executable, os.path.join(ROOT, 'twin.py'), 'both', os.path.join(HERE, 'dump.py'),
           '--mission', m[1], '--diff', a.diff, '--out', out, '--no-sync', '--timeout', str(a.timeout),
           '--env', 'GF_TICKS=' + a.ticks, '--bin', a.bin] + (['--oracle', a.oracle] if a.oracle else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return m, r.returncode, (r.stdout + r.stderr).strip().splitlines()[-2:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--missions', default='all')
    ap.add_argument('--diff', default='agent')
    ap.add_argument('--ticks', default='1,300')
    ap.add_argument('--bin', default='./pd.base')
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('-j', type=int, default=4)
    ap.add_argument('--oracle', choices=['ares', 'port'], default='ares',
                    help='GoldenEye side: the cartridge in ares (default) or the native port')
    ap.add_argument('--skip-dump', action='store_true', help='diff what is already in OUT')
    a = ap.parse_args()
    ms = levels.MISSIONS if a.missions == 'all' else [levels.mission(k) for k in a.missions.split(',')]
    failed = []
    if not a.skip_dump:
        twin.sync_tools()
        with cf.ThreadPoolExecutor(a.j) as ex:
            for m, rc, tail in ex.map(lambda m: one(m, a), ms):
                print('%-10s %s %s' % (m[1], 'ok' if rc == 0 else 'FAILED', ' | '.join(tail)), flush=True)
                if rc:
                    failed.append(m[1])
    good = [os.path.join(a.out, m[1]) for m in ms if m[1] not in failed]
    r = subprocess.run([sys.executable, os.path.join(HERE, 'worlddiff.py'), *good,
                        '--md', os.path.join(a.out, 'report.body.md'), '--json', os.path.join(a.out, 'report.json')])
    if r.returncode == 2:
        return 2
    head = ['# World diff: %d missions, %s' % (len(ms), a.diff), '']
    if failed:
        head += ['**Not compared (a dump failed):** ' + ', '.join(failed), '']
    # what our own port said while loading each mission: a refused spawn or an
    # unlinked door is often the reason behind a finding below
    said = []
    for m in ms:
        log = os.path.join(a.out, m[1], 'pd', 'pd.log')
        if not os.path.exists(log):
            continue
        for line in open(log, errors='replace'):
            l = line.strip()
            if any(k in l for k in ('WARNING', 'refused', 'never created', 'not registered', 'ERROR')) \
                    and not l.endswith(' 0 refused for something in the way at the pad') and 'EEPROM' not in l:
                said.append('- %s: `%s`' % (m[1], l[:200]))
    if said:
        head += ['## Our log while loading', ''] + said[:200] + ['']
    body = open(os.path.join(a.out, 'report.body.md')).read().split('\n', 2)[-1] if os.path.exists(os.path.join(a.out, 'report.body.md')) else ''
    open(os.path.join(a.out, 'report.md'), 'w').write('\n'.join(head) + body)
    print('report:', os.path.join(a.out, 'report.md'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

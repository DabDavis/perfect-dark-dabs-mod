#!/usr/bin/env python3
"""The AI trace diff over every mission, then one report.

    ai/sweep.py [--missions dam,facility] [-j 2] [--ticks 5400] [--visible] [--skip-run]

Each mission traces both sides (ai/aidiff.py --run: one GoldenEye run on the
oracle host, one of ours in its own run directory), so -j 2 means at most two
of each at once. Writes OUT/<mission>/report.{md,json} and OUT/report.md, the
deterministic splits (logic, then world) first. A mission whose trace or null
failed is listed at the top as not compared - never dropped.
"""
import argparse, json, os, subprocess, sys, concurrent.futures as cf
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
sys.path.insert(0, ROOT)
import levels  # noqa: E402

OUTBASE = os.path.expanduser(os.environ.get('GF_AI_OUT', '~/wt/gefidelity-run/ai-out'))


def trace(side, m, a):
    """One side's trace of one mission (twin.py ge|pd), into OUT/<mission>/<side>."""
    import aidiff
    out = os.path.join(a.out, m[1])
    cmd = [sys.executable, os.path.join(ROOT, 'twin.py'), side, os.path.join(HERE, 'trace.py'),
           '--mission', m[1], '--out', out, '--timeout', str(a.timeout), '--no-sync',
           '--env', 'PORT_AI_TRACE=0:2000000000', '--env', 'GF_AI_TICKS=%d' % a.ticks]
    if not a.visible:
        cmd += ['--env', 'GF_AI_INVISIBLE=1']
    if side == 'pd':
        cmd += ['--rundir', aidiff.rundir_for(m[1]), '--pd-arg=--ai-trace', '--pd-arg=%d' % (a.ticks + 10)]
    else:
        cmd += ['--oracle', a.oracle]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode


def compare(m, a):
    cmd = [sys.executable, os.path.join(HERE, 'aidiff.py'), '--mission', m[1], '--ticks', str(a.ticks),
           '--out', os.path.join(a.out, m[1])]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return m, r.returncode, (r.stdout + r.stderr).strip().splitlines()[-1:] or ['']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=OUTBASE)
    ap.add_argument('--missions', default='all')
    ap.add_argument('--ticks', type=int, default=3600)
    ap.add_argument('--visible', action='store_true', help='guards may see Bond (default: unseen, invincible)')
    ap.add_argument('--skip-run', action='store_true', help='compare traces already in OUT')
    ap.add_argument('--side', choices=['both', 'ge', 'pd'], default='both',
                    help='trace one side only (the other side\'s traces in OUT are reused)')
    ap.add_argument('-j', type=int, default=3, help='GoldenEye traces at once (ours: at most 2)')
    ap.add_argument('--timeout', type=int, default=3000)
    ap.add_argument('--oracle', choices=['ares', 'port'], default='ares',
                    help='GoldenEye side: the cartridge in ares (default) or the native port')
    a = ap.parse_args()
    ms = levels.MISSIONS if a.missions == 'all' else [levels.mission(k) for k in a.missions.split(',')]
    if not a.skip_run and a.side != 'pd':
        import twin
        twin.sync_tools()
    failed = []
    if not a.skip_run:
        # the oracle host takes a.j GoldenEye runs; ours stays at two, each
        # mission in its own run directory (aidiff.rundir_for)
        with cf.ThreadPoolExecutor(a.j) as gex, cf.ThreadPoolExecutor(min(2, a.j)) as pex:
            futs = {}
            sides = ('ge', 'pd') if a.side == 'both' else (a.side,)
            for m in ms:
                if 'ge' in sides:
                    futs[(m[1], 'ge')] = gex.submit(trace, 'ge', m, a)
                if 'pd' in sides:
                    futs[(m[1], 'pd')] = pex.submit(trace, 'pd', m, a)
            for m in ms:
                bad = [side for side in sides if futs[(m[1], side)].result()]
                if bad:
                    print('%-10s trace FAILED on %s' % (m[1], ' and '.join(bad)), flush=True)
    for m in ms:
        m, rc, tail = compare(m, a)
        print('%-10s %s %s' % (m[1], 'ok' if rc == 0 else 'FAILED', tail[0]), flush=True)
        if rc:
            failed.append(m[1])
    head = ['# AI trace diff: %d missions, first %d ticks, Bond %s and invincible' % (
        len(ms), a.ticks, 'visible' if a.visible else 'unseen'), '']
    if failed:
        head += ['**Not compared (a trace or a null failed):** ' + ', '.join(failed), '']
    order = {'logic': 0, 'unexplained': 0, 'world': 1, 'timing': 2, 'rng': 3, 'window-end': 4}
    det = []
    for m in ms:
        p = os.path.join(a.out, m[1], 'report.json')
        if m[1] in failed or not os.path.exists(p):
            continue
        r = json.load(open(p))
        for f in r['findings']:
            g = f.get('first_deterministic') or (f if f.get('class') in ('logic', 'world', 'unexplained') else None)
            if f['kind'] == 'missing' or g:
                det.append((m, f, g))
    head += ['## Deterministic splits (logic, world) and missing streams, all missions', '']
    for m, f, g in sorted(det, key=lambda x: (order.get((x[2] or {}).get('class', 'logic'), 9), x[0][0])):
        if f['kind'] == 'missing':
            head.append('- %s `%s`: %s' % (m[2], f['stream'], f['detail']))
        else:
            head.append('- %s `%s` **%s**, GoldenEye step %s tick %s / ours step %s tick %s: after `%s` GoldenEye `%s`, ours `%s`' % (
                m[2], f['stream'], g['class'], g['ge_step'], g['ge_tick'], g['pd_step'], g['pd_tick'],
                g['branch'] or g.get('from') or '(start of run)', ' '.join(g['ge_next'][:3]), ' '.join(g['pd_next'][:3])))
    head.append('')
    body = []
    for m in ms:
        p = os.path.join(a.out, m[1], 'report.md')
        if m[1] not in failed and os.path.exists(p):
            body.append(open(p).read())
    open(os.path.join(a.out, 'report.md'), 'w').write('\n'.join(head + body))
    print('report:', os.path.join(a.out, 'report.md'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

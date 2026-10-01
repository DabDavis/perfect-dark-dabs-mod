#!/usr/bin/env python3
"""Run a twin scenario (world/dump.py, world/inspect.py's readers, ...) against
GoldenEye XBLA ("Bean") in Xenia, with common/xeniage.py standing in for gdbge.

    xenia/run_scenario.py world/dump.py --mission dam --out OUT [--diff agent] [--env GF_TICKS=1,300]

Starts the rig (xenia/rig.sh; it refuses while another xenia_canary runs),
walks the folder screens into the mission, runs the scenario with GF_SIDE=ge
(so it imports "gdbge", which is xeniage here), and stops the rig. Output in
OUT/bean, the scenario's prints in OUT/bean/scenario.log.
"""
import argparse, os, runpy, subprocess, sys, time, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
import levels  # noqa: E402
import xeniage  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('script')
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--diff', default='agent')
    ap.add_argument('--out', required=True)
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--keep', action='store_true', help='leave the rig running afterwards')
    a = ap.parse_args()
    m = levels.mission(a.mission)
    d = levels.difficulty(a.diff)
    out = os.path.abspath(os.path.join(a.out, 'bean'))
    os.makedirs(out, exist_ok=True)
    for kv in a.env:
        k, v = kv.split('=', 1)
        os.environ[k] = v
    os.environ.update({'GF_SIDE': 'ge', 'GF_COMMON': os.path.join(ROOT, 'common'), 'GF_LEVELID': m[3],
                       'GF_MISSION': str(m[0]), 'GF_DIFF': str(d), 'GF_OUT': out})
    # a rig of its own: two runs sharing a state directory drive each other's
    # game (the second's start is refused, but its pad and channel would talk
    # to the first's rig). GF_XENIA_STATE set by the caller is respected.
    state = os.environ.get('GF_XENIA_STATE') or os.path.join(out, 'rig')
    os.environ['GF_XENIA_STATE'] = state
    xeniage.use_state(state)
    # rig.sh start's output to a file, never a pipe: what it leaves running
    # (the lock holder, Xvfb, the pad, Xenia) must not hold our read open
    rlog = os.path.join(out, 'rig-start.log')
    with open(rlog, 'w') as fh:
        r = subprocess.run([os.path.join(HERE, 'rig.sh'), 'start'], stdout=fh, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL)
    print(open(rlog).read().strip().splitlines()[-1:])
    if r.returncode:
        return 3
    rc = 0
    log = open(os.path.join(out, 'scenario.log'), 'w')
    real_stdout, real_stderr = sys.stdout, sys.stderr
    try:
        sys.modules['gdbge'] = xeniage
        orig_boot = xeniage.boot
        # the scenario calls boot(levelid, difficulty); Bean is driven by mission number
        xeniage.boot = lambda levelid=None, difficulty=0: orig_boot(None, difficulty, m[0])
        xeniage.look_ahead_off = True
        sys.stdout = sys.stderr = log     # the scenario's tracebacks go to stderr
        runpy.run_path(os.path.abspath(a.script), run_name='__main__')
    except SystemExit:
        pass
    except Exception:
        traceback.print_exc(file=log)
        rc = 1
    finally:
        sys.stdout, sys.stderr = real_stdout, real_stderr
        log.close()
        if not a.keep:
            subprocess.run([os.path.join(HERE, 'rig.sh'), 'stop'], capture_output=True)
    text = open(os.path.join(out, 'scenario.log')).read()
    if 'GF FAILED' in text:
        rc = 1
    print('bean: %s  %s' % ('ran' if rc == 0 else 'FAILED', ' | '.join(text.strip().splitlines()[-2:])))
    return rc


if __name__ == '__main__':
    sys.exit(main())

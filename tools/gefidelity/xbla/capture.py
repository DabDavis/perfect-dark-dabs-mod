#!/usr/bin/env python3
"""One capture of what the GoldenEye XBLA release draws: Bean in Xenia on the
fidelity rig (tools/gefidelity/xenia, Z's), driven into a solo mission by
common/xeniage.py, with the draw-log hook on; then matched to Bean's files
(xdraws.py) and the log deleted.

    capture.py --mission dam --out ~/wt/gefidelity-run/xbla-xenia/cap [--seconds 40]
    xdraws.py merge xbla/release-drawn.json attract=... dam=OUT/dam-drawn.json ...

In the mission: stand, turn a full circle, fire the starting gun in bursts (so
the release's muzzle-flash draws, if it makes any, are in the log), press BACK
(Bean's own N64-look switch, where the build has one) and look again.

Only one Xenia on the box, and only one virtual pad: /dev/uinput pads are
global and Xenia's SDL input takes every one, so two rigs drive each other's
games. rig.sh holds the machine-wide lock (~/wt/gefidelity-run/xenia.lock) for
its whole lifetime; this retries rig.sh start until it gets it (--wait), with
its own display (:121) and state directory, and stops only its own rig.
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RIG = os.path.join(ROOT, 'xenia', 'rig.sh')
sys.path.insert(0, os.path.join(ROOT, 'common'))
import levels  # noqa: E402


# the level file each mission draws (the CE's own for both Surfaces)
LEVELFILE = {'dam': 'dam', 'facility': 'facility', 'runway': 'runway', 'surface': 'sf1', 'bunker': 'bunker',
             'silo': 'silo', 'frigate': 'frigate', 'surface2': 'sf2', 'bunker2': 'bunker2', 'statue': 'statuepark',
             'archives': 'archives', 'streets': 'streets', 'depot': 'depot', 'train': 'train', 'jungle': 'jungle',
             'control': 'control', 'caverns': 'cavern', 'cradle': 'cradle', 'aztec': 'aztec', 'egyptian': 'temple'}


def reached(drawn):
    """The mission a capture shows, by the level file the release drew most."""
    levels_drawn = sorted(((e['frames'][2], s) for s, e in drawn.items() if s.startswith('new/background/')), reverse=True)
    if not levels_drawn:
        return None
    lv = levels_drawn[0][1].split('/')[-1]
    for m, f in LEVELFILE.items():
        if f == lv:
            return m
    return lv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--out', default=os.path.expanduser('~/wt/gefidelity-run/xbla-xenia/cap'))
    ap.add_argument('--seconds', type=int, default=40)
    ap.add_argument('--display', default=':121')
    ap.add_argument('--index', default=os.path.expanduser('~/wt/gefidelity-run/xbla-xenia/beanindex.pkl'))
    ap.add_argument('--keep-log', action='store_true')
    ap.add_argument('--expect', help='a release file the mission certainly draws (the null), e.g. new/char/greatguard2')
    ap.add_argument('--wait', type=int, default=3600, help='seconds to wait for another Xenia to finish')
    a = ap.parse_args()
    m = levels.mission(a.mission)
    os.makedirs(a.out, exist_ok=True)
    state = os.path.join(a.out, 'rig-' + m[1])
    os.makedirs(state, exist_ok=True)
    log = os.path.join(a.out, '%s.bin' % m[1])
    env = dict(os.environ, GF_XENIA_STATE=state, GF_XENIA_DISPLAY=a.display, PD_DRAWLOG=log,
               PD_DRAWLOG_FIRST='1000', PD_DRAWLOG_FRAMES='100000')
    t = time.time()
    while subprocess.run([RIG, 'start'], env=env).returncode != 0:   # the lock is another rig's
        if time.time() - t > a.wait:
            print('the Xenia rig lock stayed taken for %d s; giving up' % a.wait)
            return 3
        time.sleep(30)
    os.environ['GF_XENIA_STATE'] = state
    import xeniage as x   # reads GF_XENIA_STATE at import
    marks = {}
    try:
        x.boot(mission=m[0])
        marks['start'] = x.frame()
        subprocess.run([RIG, 'shot', os.path.join(a.out, '%s-start.png' % m[1])], env=env)
        for _ in range(8):                      # a full turn, in eighths
            x.pad('stick RX 32000 0.35', 1.2)
        for _ in range(4):                      # bursts of fire
            x.pad('trigger RT 255 0.6', 1.0)
        x.pad('stick RY 20000 0.3', 1.0)
        marks['fired'] = x.frame()
        subprocess.run([RIG, 'shot', os.path.join(a.out, '%s-fired.png' % m[1])], env=env)
        x.pad('press BACK', 3)                  # the release's own look switch, if it has one
        subprocess.run([RIG, 'shot', os.path.join(a.out, '%s-back.png' % m[1])], env=env)
        for _ in range(4):
            x.pad('stick RX 32000 0.35', 1.2)
        x.pad('trigger RT 255 0.6', 1.0)
        time.sleep(max(0, a.seconds - 30))
        marks['end'] = x.frame()
    finally:
        subprocess.run([RIG, 'stop'], env=env)
    json.dump(marks, open(os.path.join(a.out, '%s-marks.json' % m[1]), 'w'))
    out = os.path.join(a.out, '%s-drawn.json' % m[1])
    match = [sys.executable, os.path.join(HERE, 'xdraws.py'), 'match', log, a.index, out,
             '--first', str(marks['start']), '--last', str(marks['end'])]
    r = subprocess.run(match + (['--expect', a.expect] if a.expect else []))
    # the route can land on another mission (its grid pulses get lost): name the
    # capture by the level the release drew, and run the null again with that
    # level as the file it certainly draws
    drawn = json.load(open(out))['drawn'] if os.path.exists(out) else {}
    got = reached(drawn)
    # only when the expected level was not drawn at all: a mission can draw
    # two level files (the CE's Surface draws sf1 and sf2)
    if got and got != m[1] and (not a.expect or a.expect not in drawn):
        print('the route reached %s, not %s' % (got, m[1]), flush=True)
        n = 2
        name = got
        while os.path.exists(os.path.join(a.out, '%s-drawn.json' % name)):
            name = '%s#%d' % (got, n)
            n += 1
        moved = os.path.join(a.out, '%s-drawn.json' % name)
        match[5] = moved
        r = subprocess.run(match + ['--expect', 'new/background/' + LEVELFILE.get(got, got)])
        os.remove(out)
        out = moved
    if not a.keep_log and r.returncode == 0:
        for ext in ('', '.vb'):
            try:
                os.remove(log + ext)
            except OSError:
                pass
    print('marks', marks, '->', out)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())

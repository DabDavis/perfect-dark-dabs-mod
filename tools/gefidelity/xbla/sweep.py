#!/usr/bin/env python3
"""The HD draw census end to end, for the fidelity gate (world/gate.py's
hd-census leg): our census runs, the analysis against the release's capture,
and the gate's findings.

    xbla/sweep.py --bin ./pd.census --out DIR [--missions dam,facility] [--no-repeat]

--bin is relative to the run directory (--rundir, default
~/wt/gefidelity-run/xbla-run), as twin.py takes it, and must be a build WITH
the census hook (feat/gefidelity-xbla, e80453245): a binary without it logs no
census lines and the sweep exits 2. To gate a change, build the change with the
hook on top: `git cherry-pick -n e80453245` (or `git apply
xbla/census-hook.patch`) in a scratch worktree of the change, build, copy in.

Writes DIR/report.json (findings: mission = release file, kind hd.*, key =
draw place / picture, mag = triangles), DIR/report.md and DIR/census.json.
The release side is xbla/release-drawn.json (xdraws.py merge of Xenia
captures; capture.py makes them) and is not re-captured per sweep.

Null: census.py's (a struck draw, the planted fault), and repeatability -
the first mission is run a second time and both runs must give the same
findings keys for the files it loads. Exit 2 when either fails.
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    print('+', ' '.join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def keys(path):
    return sorted((f['mission'], f['kind'], f['key']) for f in json.load(open(path)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', default='./pd.census')
    ap.add_argument('--out', required=True)
    ap.add_argument('--rundir', default=os.path.expanduser('~/wt/gefidelity-run/xbla-run'))
    ap.add_argument('--missions', default='all')
    ap.add_argument('--release', default=os.path.join(HERE, 'release-drawn.json'))
    ap.add_argument('--no-repeat', action='store_true', help='skip the repeatability null')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    py = sys.executable
    if run([py, os.path.join(HERE, 'run.py'), '--out', out, '--bin', a.bin, '--rundir', a.rundir,
            '--missions', a.missions]):
        return 2
    plant = open(os.path.join(out, 'plant.txt')).read().strip()
    logs = sorted(os.path.join(out, 'runs', f) for f in os.listdir(os.path.join(out, 'runs')) if f.endswith('.log'))
    rc = run([py, os.path.join(HERE, 'census.py'), *logs, '--plant', plant, '--release', a.release,
              '--md', os.path.join(out, 'report.md'), '--json', os.path.join(out, 'census.json'),
              '--findings', os.path.join(out, 'report.json')])
    if rc:
        return 2
    if not a.no_repeat:
        first = 'dam' if a.missions == 'all' else a.missions.split(',')[0]
        rep = os.path.join(out, 'repeat')
        # the mission and the gun pass again (the gun pass is where timing once
        # decided which models loaded)
        if run([py, os.path.join(HERE, 'run.py'), '--out', rep, '--bin', a.bin, '--rundir', a.rundir,
                '--missions', first, '--guns', first, '--pool', 'none', '--arenas', 'none']):
            return 2
        got = []
        for d in (out, rep):
            f = os.path.join(rep, 'one-%s.json' % ('a' if d == out else 'b'))
            logs2 = [os.path.join(d, 'runs', first + '.log'), os.path.join(d, 'runs', first + '-guns.log')]
            if run([py, os.path.join(HERE, 'census.py'), *logs2, '--plant', plant,
                    '--release', a.release, '--md', f + '.md', '--findings', f]):
                return 2
            got.append(keys(f))
        if got[0] != got[1]:
            print('NULL FAILED - two runs of %s gave different findings keys: %s' % (
                first, sorted(set(got[0]) ^ set(got[1]))[:8]))
            return 2
        print('null: %s and its gun pass twice, %d findings keys alike' % (first, len(got[0])))
    n = len(json.load(open(os.path.join(out, 'report.json'))))
    print('report: %s (%d findings)' % (os.path.join(out, 'report.json'), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())

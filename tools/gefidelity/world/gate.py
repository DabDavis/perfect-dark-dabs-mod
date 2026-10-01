#!/usr/bin/env python3
"""The fidelity gate: is a change closer to the originals or further from them?

    world/gate.py --base ./pd.base --test ./pd.fix --out ~/wt/gefidelity-run/gate/<name>
    world/gate.py --base-report OUT/base --test ./pd.fix --out ...   # reuse base sweeps
    world/gate.py ... --legs world                                  # one leg only

Each leg sweeps the base and the test binary (relative to that leg's run
directory, as twin.py takes them) and compare.py judges the two reports:

- world     the N64 look's world against the cartridge in ares (world/sweep.py)
- view      the N64 look's pictures against the cartridge's, in ares (view/)
- guns      GoldenEye's 25 guns measured against the cartridge's, in ares (guns/)
- ai        each chr's script against the cartridge's, first divergence, in ares (ai/)
- hd-census the HD look's draws against what the release draws in Xenia (xbla/)
- hd-world  the HD look's world against the release's own, in Xenia (xenia/)
- hd-view   the HD look's pictures against the release's, in Xenia (view/)
- census    the converter's ROM reads, on a source tree (--base-tree/--test-tree): a field
            newly dropped that the cartridge reads fails it (census/)
- convdiff  which converted files the change altered, and where (convdiff/); it informs,
            it never passes or fails - the footprint, before any sweep runs
- replay    tools/ci/replaytest.sh compare: which seeded runs diverge and at which frame.
            It informs; with --neutral (a change meant to leave gameplay alone) a
            divergence fails the gate

The replay test (tools/ci/replaytest.sh) answers "did this change gameplay at
all"; the gate answers "did it move GE Plus towards GoldenEye or away", per leg
and per mission. Exit 0 when no leg has a new or worse placement finding, 1
when one has, 2 when a leg's null failed or its sweep made no report. A leg
whose tool is not in the tree is skipped and says so.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# leg: (the tool that sweeps one binary, extra arguments). The tool takes
# --bin BINARY --out DIR and writes DIR/report.json in worlddiff's finding shape.
LEGS = {
    'world': (os.path.join(HERE, 'sweep.py'), lambda a: ['--oracle', a.oracle, '--diff', a.diff, '-j', str(a.j)]
              + (['--missions', a.missions] if a.missions != 'all' else [])),
    'hd-census': (os.path.join(ROOT, 'xbla', 'sweep.py'), lambda a: []),
    # Bean (the Community Edition build by default, xenia/rig.sh GF_BEAN) against ours in the HD look;
    # its run dir and pd.ini are hdsweep's own (xenia-out/hd-run), so --bin is relative to that
    'hd-world': (os.path.join(ROOT, 'xenia', 'hdsweep.py'), lambda a: ['--diff', a.diff]
                 + (['--missions', a.missions] if a.missions != 'all' else [])),
    # the view diff as findings: kind view.pair / hd-view.pair, key pad:heading, mag the pair's score
    'view': (os.path.join(ROOT, 'view', 'gatesweep.py'), lambda a: ['--oracle', a.oracle]
             + (['--missions', a.missions] if a.missions != 'all' else [])),
    'hd-view': (os.path.join(ROOT, 'view', 'gatesweep.py'), lambda a: ['--oracle', 'xenia']
                + (['--missions', a.missions] if a.missions != 'all' else [])),
    # the gun diff as findings: kind gun.<quantity>, key the gun, mag the gap beyond tolerance
    'guns': (os.path.join(ROOT, 'guns', 'gatesweep.py'), lambda a: ['--oracle', a.oracle]),
    # the AI trace diff as findings: kind ai.diverge (deterministic) or ai.timing (random/timing),
    # key chr/list, mag larger the earlier the scripts part
    'ai': (os.path.join(ROOT, 'ai', 'gatesweep.py'), lambda a: ['--oracle', a.oracle]
           + (['--missions', a.missions] if a.missions != 'all' else [])),
    # the converter read census: judged on a source tree, not a binary (--base-tree/--test-tree);
    # kind census.dropped (a field the cartridge reads) gates, census.unread (it never does) is listed
    'census': (os.path.join(ROOT, 'census', 'gatesweep.py'), lambda a: []),
    'convdiff': (os.path.join(ROOT, 'convdiff', 'diff.py'), lambda a: []),
    'replay': (os.environ.get('GF_REPLAYTEST', os.path.normpath(os.path.join(ROOT, '..', 'ci', 'replaytest.sh'))), lambda a: []),
}
# legs judged on a source tree (geconvert.c) rather than a game binary
TREE_LEGS = {'census', 'convdiff'}
# legs that only inform: what a change altered, never pass or fail
INFO_LEGS = {'convdiff'}
# the replay test (tools/ci/replaytest.sh compare): did gameplay change at all? A
# fidelity fix is meant to change its mission's game, so it informs by default;
# --neutral (a render, menu or refactor change) makes any divergence fail
# this tree's replaytest.sh (it hides every controller, the Xenia rig's virtual
# pad included) run against a checkout's build/ (data, mods, the converted maps)
REPLAY = os.environ.get('GF_REPLAYTEST', os.path.normpath(os.path.join(ROOT, '..', 'ci', 'replaytest.sh')))
REPLAY_BUILD = os.environ.get('GF_REPLAY_BUILD', os.path.expanduser('~/perfect-dark/perfect_dark/build'))


def replay(a, out):
    rundir = os.path.expanduser(os.environ.get('GF_RUNDIR', '~/wt/gefidelity-run'))
    absbin = lambda b: b if os.path.isabs(b) else os.path.normpath(os.path.join(rundir, b))
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, OUT=os.path.join(out, 'runs'), BUILD=REPLAY_BUILD)
    r = subprocess.run([REPLAY, 'compare', absbin(a.base), absbin(a.test)], env=env, capture_output=True, text=True)
    open(os.path.join(out, 'replay.log'), 'w').write(r.stdout + r.stderr)
    lines = [l for l in r.stdout.splitlines() if l.startswith(('same ', 'DIFF ', 'FAIL ', 'skip '))]
    md = ['# Replay test (did gameplay change?)', '', '`%s compare %s %s`' % (REPLAY, a.base, a.test), '']
    md += ['- ' + l for l in lines]
    open(os.path.join(out, 'gate.md'), 'w').write('\n'.join(md) + '\n')
    diffs = [l for l in lines if l.startswith('DIFF')]
    if r.returncode == 2:
        return 2, 'a run failed (%s)' % os.path.join(out, 'replay.log')
    what = '%d cases the same, %d diverged%s' % (len([l for l in lines if l.startswith('same')]), len(diffs),
                                                 (': ' + '; '.join(d[5:] for d in diffs)) if diffs else '')
    if diffs and a.neutral:
        return 1, what + ' - and the change was declared neutral'
    return 0, what


def convdiff(a, out):
    """The conversion diff: each tree's geconvert.c converts the ROM once, and the
    two conversions are compared file by file (convdiff/)."""
    cd = os.path.join(ROOT, 'convdiff')
    sides = {}
    for side, tree in (('base', a.base_tree), ('test', a.test_tree)):
        d = os.path.join(out, side)
        r = subprocess.run([os.path.join(cd, 'convert.sh'), '--tree', tree, '--out', d])
        if r.returncode:
            return 2, 'the %s tree did not convert (%s)' % (side, os.path.join(d, 'convert.log'))
        sides[side] = os.path.join(d, 'conv')
    r = subprocess.run([sys.executable, os.path.join(cd, 'diff.py'), sides['base'], sides['test'],
                        '--md', os.path.join(out, 'gate.md'), '--json', os.path.join(out, 'diff.json')],
                       capture_output=True, text=True)
    print(r.stdout, end='')
    last = [l for l in r.stdout.splitlines() if 'files identical' in l]
    return (2 if r.returncode else 0), (last[0] if last else r.stdout.strip()[-200:])
# legs whose oracle side does not depend on our binary, and can reuse the base sweep's
REUSES_ORACLE = {'hd-world', 'view', 'hd-view', 'guns', 'ai'}


def sweep(leg, binary, out, a, extra=()):
    tool, args = LEGS[leg]
    hd = leg.startswith('hd-')
    if leg in TREE_LEGS:
        cmd = [sys.executable, tool, '--out', out, '--tree', binary]
    else:
        cmd = [sys.executable, tool, '--out', out, '--bin', a.hd_bin_prefix + binary if hd and a.hd_bin_prefix else binary]
    cmd += args(a) + list(extra)
    print('[%s] sweep %s -> %s' % (leg, binary, out), flush=True)
    r = subprocess.run(cmd)
    return r.returncode != 2 and os.path.exists(os.path.join(out, 'report.json'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', help='the binary before the change')
    ap.add_argument('--base-report', help='a directory holding <leg>/base sweeps to reuse instead of sweeping --base')
    ap.add_argument('--test', required=True, help='the binary after the change')
    ap.add_argument('--out', required=True)
    ap.add_argument('--legs', default='convdiff,replay,world,view,guns,ai,hd-census,hd-world,hd-view,census')
    ap.add_argument('--neutral', action='store_true',
                    help='the change is meant to leave gameplay alone: a replay divergence fails the gate')
    ap.add_argument('--base-tree', help='source tree before the change, for the census leg')
    ap.add_argument('--test-tree', help='source tree after the change, for the census leg (no tree, no census leg)')
    ap.add_argument('--missions', default='all')
    ap.add_argument('--diff', default='agent')
    ap.add_argument('--oracle', default='ares', choices=['ares', 'port'])
    ap.add_argument('--hd-bin-prefix', default='', help='prefix for the binary name in the HD legs (their run dirs differ)')
    ap.add_argument('-j', type=int, default=4)
    a = ap.parse_args()
    if not a.base and not a.base_report:
        ap.error('--base or --base-report')
    worst = 0
    summary = []
    for leg in [l.strip() for l in a.legs.split(',') if l.strip()]:
        if leg not in LEGS:
            ap.error('no leg %r; legs are %s' % (leg, ', '.join(LEGS)))
        if not os.path.exists(LEGS[leg][0]):
            summary.append('%-10s skipped: %s is not in the tree yet' % (leg, os.path.relpath(LEGS[leg][0], ROOT)))
            continue
        if leg in TREE_LEGS and not a.test_tree:
            summary.append('%-10s skipped: judged on source trees; pass --test-tree (and --base-tree or --base-report)' % leg)
            continue
        if leg == 'replay':
            if not a.base:
                summary.append('%-10s skipped: needs --base (a binary; a base report cannot replay)' % leg)
                continue
            rc, what = replay(a, os.path.join(a.out, leg))
            summary.append('%-10s %s: %s (%s)' % (leg, {0: 'informs' if not a.neutral else 'passed', 1: 'FAILED',
                                                        2: 'INCONCLUSIVE'}[rc], what, os.path.join(a.out, leg, 'gate.md')))
            worst = max(worst, rc)
            continue
        if leg in INFO_LEGS:
            if not a.base_tree:
                summary.append('%-10s skipped: needs --base-tree as well' % leg)
                continue
            rc, what = convdiff(a, os.path.join(a.out, leg))
            summary.append('%-10s %s: %s (%s)' % (leg, 'informs' if rc == 0 else 'INCONCLUSIVE', what,
                                                  os.path.join(a.out, leg, 'gate.md')))
            worst = max(worst, rc)
            continue
        base = os.path.join(a.base_report, leg) if a.base_report else os.path.join(a.out, leg, 'base')
        if a.base_report and leg == 'world' and os.path.exists(os.path.join(a.base_report, 'report.json')):
            base = a.base_report     # a plain world sweep directory works as the world leg's base
        test = os.path.join(a.out, leg, 'test')
        base_what = a.base_tree if leg in TREE_LEGS else a.base
        test_what = a.test_tree if leg in TREE_LEGS else a.test
        if not a.base_report and not base_what:
            summary.append('%-10s skipped: no base for it (--base-tree for the census)' % leg)
            continue
        if not a.base_report and not sweep(leg, base_what, base, a):
            summary.append('%-10s FAILED: the base sweep made no report' % leg)
            worst = 2
            continue
        # the oracle's side does not depend on our binary: let a leg reuse it
        extra = ['--reuse-oracle', base] if leg in REUSES_ORACLE else []
        if not sweep(leg, test_what, test, a, extra):
            summary.append('%-10s FAILED: the test sweep made no report' % leg)
            worst = 2
            continue
        rc = subprocess.run([sys.executable, os.path.join(HERE, 'compare.py'), base, test,
                             '--md', os.path.join(a.out, leg, 'gate.md')]).returncode
        summary.append('%-10s %s (%s)' % (leg, {0: 'passed', 1: 'FAILED: a placement finding appeared or grew',
                                                2: 'INCONCLUSIVE: null failed or missions missing'}.get(rc, 'rc %d' % rc),
                                          os.path.join(a.out, leg, 'gate.md')))
        worst = max(worst, rc)
    print('\n'.join(['', 'GATE'] + summary))
    return worst


if __name__ == '__main__':
    sys.exit(main())

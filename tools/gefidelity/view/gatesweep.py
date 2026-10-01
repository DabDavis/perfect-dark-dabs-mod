#!/usr/bin/env python3
"""The view diff as the fidelity gate's 'view' and 'hd-view' legs.

    view/gatesweep.py --bin ./pd.fix --out DIR [--oracle ares|xenia|port] [--missions dam,facility]
                      [--reuse-oracle BASE_DIR]

Sweeps the view diff for one binary of ours (relative to ~/wt/gefidelity-run,
as the gate takes it) and writes DIR/report.json in worlddiff's finding shape:

- every ranked pair: mission = levels.py key, kind 'view.pair' ('hd-view.pair'
  with --oracle xenia), key 'pad:heading' (the same pads and headings every
  run), mag = the pair's score, detail = the score and the pair's row on the
  mission's page;
- every pair the view diff does not rank (cameras apart, a frozen guard in the
  camera): kind 'view.unranked' / 'hd-view.unranked', mag 0 - behaviour, which
  compare.py never fails on.

compare.py calls a pair worse when its score grows by a quarter and by 0.05.

--reuse-oracle BASE_DIR takes GoldenEye's pictures from the base sweep (they
do not depend on our binary), so the gate shoots the cartridge or the release
once.

The null, every run: each mission's own view-diff null (identity, one pad off,
planted block) must pass, every mission must have run, and the view diff must
repeat - the first mission is swept a second time with the oracle's pictures
reused and must give the same keys and scores within 0.05. Exit 2 when any of
it fails (the gate reads 2 as the leg's null failing), else 0; the findings
themselves are compare.py's to judge.
"""
import argparse, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import levels  # noqa: E402

REPEAT_TOL = 0.05


def findings(out, missions, prefix):
    rows, problems = [], []
    for m in missions:
        sp = os.path.join(out, m, 'scores.json')
        if not os.path.exists(sp):
            problems.append('%s: no scores (the run failed)' % m)
            continue
        d = json.load(open(sp))
        s = d['summary']
        if not s.get('ranked'):
            problems.append('%s: the view diff null failed: %s' % (m, json.dumps(s['null'])))
            continue
        page = os.path.join(out, m, 'index.html')
        for r in d['rows']:
            key = '%d:%d' % (r['pad'], int(r['head']))
            if r.get('score') is None:
                rows.append({'mission': m, 'kind': prefix + '.unranked', 'key': key, 'mag': 0.0,
                             'detail': 'pad %d heading %d not ranked: %s' % (r['pad'], r['head'], r['cam']['status'])})
            else:
                rows.append({'mission': m, 'kind': prefix + '.pair', 'key': key, 'mag': r['score'],
                             'detail': 'pad %d heading %d: %.3f (struct %.3f, edge %.3f, colour %.3f) %s#%s' % (
                                 r['pad'], r['head'], r['score'], r['struct'], r['edge'], r['colour'], page,
                                 r['tag'])})
    return rows, problems


def viewdiff(cmd, args):
    return subprocess.run([sys.executable, os.path.join(HERE, 'viewdiff.py'), cmd] + args).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--oracle', default='ares', choices=['ares', 'xenia', 'port'])
    ap.add_argument('--missions', default='all')
    ap.add_argument('--reuse-oracle')
    ap.add_argument('--step', type=int, default=6)
    ap.add_argument('--max', type=int, default=40)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    ms = [m[1] for m in levels.MISSIONS] if a.missions == 'all' else [levels.mission(k)[1] for k in a.missions.split(',')]
    prefix = 'hd-view' if a.oracle == 'xenia' else 'view'
    common = ['--oracle', a.oracle, '--bin', a.bin, '--step', str(a.step), '--max', str(a.max)]
    sweep = common + ['--out', out, '--missions', ','.join(ms)]
    if a.reuse_oracle:
        sweep += ['--reuse-oracle', os.path.abspath(a.reuse_oracle)]
    viewdiff('sweep', sweep)
    rows, problems = findings(out, ms, prefix)

    # repeatability: the first mission that ranked, swept again on the same
    # oracle pictures, must give the same keys and scores
    first = next((m for m in ms if os.path.exists(os.path.join(out, m, 'scores.json'))), None)
    if first:
        rep = os.path.join(out, '_repeat')
        viewdiff('run', common + ['--mission', first, '--out', rep, '--reuse-oracle', out])
        again, p2 = findings(rep, [first], prefix)
        a1 = {f['key']: f['mag'] for f in rows if f['mission'] == first}
        a2 = {f['key']: f['mag'] for f in again}
        if p2 or set(a1) != set(a2):
            problems.append('repeat: %s gave other keys or none the second time (%s)' % (first, p2))
        else:
            worst = max((abs(a1[k] - a2[k]) for k in a1), default=0.0)
            if worst > REPEAT_TOL:
                problems.append('repeat: %s scores moved by up to %.3f between two sweeps of one binary' % (first, worst))
            print('repeat: %s swept twice, %d keys the same, scores within %.3f' % (first, len(a1), worst))
    else:
        problems.append('repeat: no mission ran')

    json.dump(rows, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    if problems:
        print('NULL FAILED - the view leg cannot be trusted:', *problems, sep='\n  ')
        return 2
    print('view leg: %d pairs ranked, %d not, %d missions -> %s' % (
        sum(1 for f in rows if f['kind'].endswith('.pair')), sum(1 for f in rows if f['kind'].endswith('.unranked')),
        len(ms), os.path.join(out, 'report.json')))
    return 0


if __name__ == '__main__':
    sys.exit(main())

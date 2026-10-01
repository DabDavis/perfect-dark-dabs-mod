#!/usr/bin/env python3
"""The fidelity gate's comparison: two world-diff reports, before and after a
change, as findings fixed, findings new and findings that moved.

    world/compare.py BASE TEST            # each a sweep directory or its report.json
    world/compare.py BASE TEST --md gate.md

A finding is keyed by (mission, kind, key), so a fix shows as its row going
away and a regression as a row appearing - on the mission the change was for or
on any other. Placement findings are fixed at load and decide the exit code;
behaviour findings (chrs after their scripts have run, worlddiff.BEHAVIOUR) are
listed separately and never fail the gate, because a timing change can move
them without anything being wrong.

Exit 0: no new or worse placement finding. 1: a placement finding appeared or
grew. 2: the null failed, or a mission is in one report only (inconclusive -
a mission one sweep could not dump is never counted as fixed). The null runs every time: BASE against itself must
show no change, and BASE against a copy with one finding removed and one
planted must show exactly that one fixed and that one new.
"""
import argparse, copy, json, os, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from worlddiff import BEHAVIOUR  # noqa: E402

GROW = 1.25      # a finding's magnitude must grow by a quarter (and 1 unit) to count as worse
GROW_ABS = 1.0
# kinds whose magnitude is not in units: a picture's score runs 0..1
GROW_ABS_BY_PREFIX = {'view.': 0.05, 'hd-view.': 0.05}


def grow_abs(kind):
    for p, v in GROW_ABS_BY_PREFIX.items():
        if kind.startswith(p):
            return v
    return GROW_ABS


def load(path):
    """(findings, the missions the report speaks for). A sweep writes the second
    as report-missions.json beside report.json; without it the missions are
    taken from the findings, which cannot see a mission that had none."""
    d = path if os.path.isdir(path) else os.path.dirname(path)
    rp = os.path.join(path, 'report.json') if os.path.isdir(path) else path
    findings = json.load(open(rp))
    side = os.path.join(d, 'report-missions.json')
    if os.path.exists(side):
        return findings, set(json.load(open(side))['compared'])
    # an older sweep: its findings' missions, and every mission directory
    # that holds that mission's own result (ai: report.json, view:
    # scores.json) - a mission swept clean has no findings but is there
    swept = set()
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common'))
        import levels
        for m in levels.MISSIONS:
            md = os.path.join(d, m[1])
            try:
                if os.path.exists(os.path.join(md, 'scores.json')):        # view: ranked = its null passed
                    ok = json.load(open(os.path.join(md, 'scores.json')))['summary'].get('ranked')
                elif os.path.exists(os.path.join(md, 'report.json')):      # ai: no problems = its nulls passed
                    r = json.load(open(os.path.join(md, 'report.json')))
                    ok = isinstance(r, dict) and not r.get('problems')
                else:
                    ok = False
            except (ValueError, KeyError, OSError):
                ok = False
            if ok:
                swept.add(m[1])
    except ImportError:
        pass
    print('note: %s has no report-missions.json; taking its missions from its findings%s' % (
        d, ' and its %d mission directories' % len(swept) if swept else ''))
    return findings, {f['mission'] for f in findings} | swept


def keyed(findings):
    out = {}
    for f in findings:
        k = (f['mission'], f['kind'], f['key'])
        # two findings with one key (a key repeated by kind): keep the larger
        if k not in out or f['mag'] > out[k]['mag']:
            out[k] = f
    return out


def compare(base, test):
    a, b = keyed(base), keyed(test)
    res = {'fixed': [], 'new': [], 'worse': [], 'better': []}
    for k in sorted(set(a) - set(b)):
        res['fixed'].append(a[k])
    for k in sorted(set(b) - set(a)):
        res['new'].append(b[k])
    for k in sorted(set(a) & set(b)):
        ma, mb = a[k]['mag'], b[k]['mag']
        g = grow_abs(k[1])
        if mb > ma * GROW and mb - ma > g:
            res['worse'].append(dict(b[k], was=ma))
        elif ma > mb * GROW and ma - mb > g:
            res['better'].append(dict(b[k], was=ma))
    return res


def restrict(findings, missions):
    return [f for f in findings if f['mission'] in missions]


def is_behaviour(f):
    # the release's own deliberate changes ('bean.') and drops it confirms by
    # design ('hd.accepted') are listed, never gate
    return f['kind'] in BEHAVIOUR or f['kind'].startswith(('bean.', 'hd.accepted')) \
        or f['kind'] in ('view.unranked', 'hd-view.unranked') or f['kind'].startswith(('ai.timing', 'census.unread'))


def selftest(base, missions):
    problems = []
    # a mission one report lacks must come out as not compared, never as fixed
    if len(missions) > 1:
        drop = sorted(missions)[0]
        kept = [f for f in base if f['mission'] != drop]
        r = compare(restrict(base, missions - {drop}), restrict(kept, missions - {drop}))
        if r['fixed']:
            problems.append('a mission missing from one report came out as %d fixes' % len(r['fixed']))
    same = compare(base, base)
    if any(same.values()):
        problems.append('a report against itself shows changes')
    placement = [f for f in base if not is_behaviour(f)]
    if not placement:
        return problems   # nothing to plant against; the self-compare still ran
    test = copy.deepcopy(base)
    gone = placement[0]
    test = [f for f in test if (f['mission'], f['kind'], f['key']) != (gone['mission'], gone['kind'], gone['key'])]
    planted = {'mission': gone['mission'], 'kind': 'obj.pos', 'key': 'null-plant', 'detail': 'planted', 'mag': 99.0}
    test.append(planted)
    r = compare(base, test)
    if [(f['mission'], f['kind'], f['key']) for f in r['fixed']] != [(gone['mission'], gone['kind'], gone['key'])]:
        problems.append('a removed finding did not come out as the one fix')
    if [(f['mission'], f['kind'], f['key']) for f in r['new']] != [(planted['mission'], 'obj.pos', 'null-plant')]:
        problems.append('a planted finding did not come out as the one new finding')
    return problems


def markdown(res, base_name, test_name):
    lines = ['# Fidelity gate: %s -> %s' % (base_name, test_name), '']
    for title, want in (('Placement', False), ('Behaviour (after the scripts have run; never fails the gate)', True)):
        lines += ['## ' + title, '']
        for what in ('new', 'worse', 'fixed', 'better'):
            fs = [f for f in res[what] if is_behaviour(f) == want]
            if not fs:
                continue
            by = defaultdict(list)
            for f in fs:
                by[f['mission']].append(f)
            lines.append('### %s (%d)' % (what, len(fs)))
            for m in sorted(by):
                for f in sorted(by[m], key=lambda f: -f['mag'])[:30]:
                    was = ' (was %.1f)' % f['was'] if 'was' in f else ''
                    lines.append('- %s %s: %s%s' % (m, f['kind'], f['detail'], was))
                if len(by[m]) > 30:
                    lines.append('- %s: ... %d more' % (m, len(by[m]) - 30))
            lines.append('')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base')
    ap.add_argument('test')
    ap.add_argument('--md')
    a = ap.parse_args()
    (base, bm), (test, tm) = load(a.base), load(a.test)
    problems = selftest(base, bm)
    if problems:
        print('NULL FAILED - the gate cannot be trusted:', *problems, sep='\n  ')
        return 2
    both = bm & tm
    missing = sorted((bm | tm) - both)
    res = compare(restrict(base, both), restrict(test, both))
    text = markdown(res, a.base, a.test)
    if missing:
        text = ('**Not compared - in one report only: %s.** The gate is inconclusive until they are swept on both '
                'sides.\n\n' % ', '.join(missing)) + text
    if a.md:
        open(a.md, 'w').write(text)
    bad = [f for f in res['new'] + res['worse'] if not is_behaviour(f)]
    good = [f for f in res['fixed'] + res['better'] if not is_behaviour(f)]
    print('null: a report against itself is unchanged, one removed and one planted finding came out as such')
    print('placement: %d fixed, %d better, %d new, %d worse; behaviour: %d fixed, %d new' % (
        len([f for f in res['fixed'] if not is_behaviour(f)]), len([f for f in res['better'] if not is_behaviour(f)]),
        len([f for f in res['new'] if not is_behaviour(f)]), len([f for f in res['worse'] if not is_behaviour(f)]),
        len([f for f in res['fixed'] if is_behaviour(f)]), len([f for f in res['new'] if is_behaviour(f)])))
    if not a.md:
        print(text)
    if missing:
        print('not compared (in one report only): %s' % ', '.join(missing))
    print('GATE %s' % ('FAILED: a placement finding appeared or grew' if bad else
                       'INCONCLUSIVE: missions missing from one side' if missing else 'passed'))
    return 1 if bad else 2 if missing else 0


if __name__ == '__main__':
    sys.exit(main())

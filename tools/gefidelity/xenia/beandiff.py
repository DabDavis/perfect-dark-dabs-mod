#!/usr/bin/env python3
"""GoldenEye XBLA ("Bean") against the N64 ROM, and our HD look against Bean.

    xenia/beandiff.py BEAN_DIR N64_DIR                  Bean's own changes ("bean.*")
    xenia/beandiff.py BEAN_DIR N64_DIR --ours PD_DIR    and ours against Bean (worlddiff's kinds)

BEAN_DIR holds world_t<N>.json from xenia/run_scenario.py world/dump.py; N64_DIR
the oracle's (world/sweep.py's <mission>/ge, ares or port); PD_DIR ours.

Bean is GoldenEye's own setup with 4J's changes, not record for record the
N64's: Dam has 39 hats inserted at record 106, a weapon record one word
longer, a door one shorter, the truck 15 longer, 46 more pads. So Bean's
records are first ALIGNED to the N64's by (type, model, pad), and:
- Bean's additions, removals and changed fields are "bean.*" findings: the
  release's deliberate changes, never a conversion fault of ours;
- for ours against Bean, Bean is re-indexed into the N64's order (records Bean
  does not have are filled with the N64's, so they compare as the N64 look
  does) and handed to world/worlddiff.py's compare() as the GoldenEye side -
  the same finding kinds as the world sweep, keyed by the N64 record index. A
  finding on a record and field where Bean already differs from the N64 is
  Bean's change seen against ours: it becomes "bean.via.<kind>".

Null on every run: Bean's dump aligned against itself gives no bean.* finding,
and a copy with one record dropped and one object moved 50 units gives exactly
bean.rec.missing for the one and bean.obj.pos for the other.
"""
import argparse, copy, difflib, glob, json, math, os, re, sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'world'))
sys.path.insert(0, os.path.join(ROOT, 'common'))
import worlddiff  # noqa: E402

OBJ = worlddiff.TYPE_NAMES


def key(r):
    return (r['type'], r.get('model'), r.get('pad'))


def align(n64, bean):
    """[(n64 index or None, bean index or None)] in order."""
    a = [key(r) for r in n64]
    b = [key(r) for r in bean]
    out = []
    for op, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
        if op == 'equal':
            out += list(zip(range(i1, i2), range(j1, j2)))
        else:
            out += [(i, None) for i in range(i1, i2)] + [(None, j) for j in range(j1, j2)]
    return out


def bean_vs_n64(n0, b0, n1, b1, mission):
    """Bean's own changes against the N64 ROM, as bean.* findings. Both sides use
    GoldenEye's own representation and coordinates, so nothing is translated."""
    rep = worlddiff.Report(mission)
    pairs = align(n0['props'], b0['props'])
    extra = defaultdict(list)
    missing = defaultdict(list)
    for i, j in pairs:
        if i is None:
            r = b0['props'][j]
            extra[(r['type'], r.get('model'))].append(j)
        elif j is None:
            r = n0['props'][i]
            missing[(r['type'], r.get('model'))].append(i)
    for (t, m), js in sorted(extra.items(), key=lambda kv: str(kv[0])):
        rep.add('bean.rec.extra', '%d:%s' % (t, m), 'Bean adds %d %s record(s)%s (Bean records %s)' % (
            len(js), worlddiff.tname(t), '' if m is None else ' of model %d' % m, js[:12]), len(js))
    for (t, m), is_ in sorted(missing.items(), key=lambda kv: str(kv[0])):
        rep.add('bean.rec.missing', '%d:%s' % (t, m), 'Bean has no %d of the N64\'s %s record(s)%s (N64 records %s)' % (
            len(is_), worlddiff.tname(t), '' if m is None else ' of model %d' % m, is_[:12]), len(is_))
    for t, d in sorted((b0.get('bean_deltas') or {}).items(), key=lambda kv: int(kv[0])):
        rep.add('bean.rec.size', t, 'Bean\'s %s record is %+d word(s) on the N64\'s' % (worlddiff.tname(int(t)), d), abs(d))
    for i, j in pairs:
        if i is None or j is None:
            continue
        a, b = n0['props'][i], b0['props'][j]
        if 'model' not in a:
            continue
        what = '%s %d (model %d)' % (worlddiff.tname(a['type']), i, a['model'])
        if a.get('exists') != b.get('exists'):
            rep.add('bean.obj.exists', i, '%s: %s on the N64, %s in Bean' % (
                what, 'made' if a.get('exists') else 'not made', 'made' if b.get('exists') else 'not made'))
            continue
        if not a.get('exists') or a.get('attached') or b.get('attached'):
            continue
        for k, kind in (('rtpos', 'bean.obj.pos'), ('pos', 'bean.obj.refpos')):
            if k in a and k in b:
                d = math.dist(a[k], b[k])
                if d > worlddiff.TOL_OBJ:
                    rep.add(kind, i, '%s %s is %.1f units out in Bean' % (what, k, d), d)
        rot = max(abs(a['rot'][r][c] - b['rot'][r][c]) for r in range(3) for c in range(3))
        if rot > worlddiff.TOL_ROT:
            rep.add('bean.obj.rot', i, '%s turned differently in Bean (%.3f)' % (what, rot), rot)
        if a.get('scale') and b.get('scale') and abs(a['scale'] - b['scale']) > 0.002 * max(1, a['scale']):
            rep.add('bean.obj.scale', i, '%s scale N64 %.4f, Bean %.4f' % (what, a['scale'], b['scale']))
        if set(a.get('rooms', [])) != set(b.get('rooms', [])):
            rep.add('bean.obj.rooms', i, '%s rooms N64 %s, Bean %s' % (what, sorted(a['rooms']), sorted(b['rooms'])))
        if (a.get('maxdamage'), a.get('damage')) != (b.get('maxdamage'), b.get('damage')):
            rep.add('bean.obj.health', i, '%s health words N64 %s, Bean %s' % (
                what, (a.get('maxdamage'), a.get('damage')), (b.get('maxdamage'), b.get('damage'))))
    # pads: Bean keeps the N64's and adds its own
    gp = {r[0]: r for r in n0['pads']}
    bp = {r[0]: r for r in b0['pads']}
    for k in sorted(set(gp) & set(bp)):
        d = math.dist(gp[k][1:4], bp[k][1:4])
        if d > worlddiff.TOL_PAD:
            rep.add('bean.pad.pos', k, 'pad %d is %.1f units out in Bean' % (k, d), d)
        if gp[k][4] >= 0 and bp[k][4] >= 0 and gp[k][4] != bp[k][4]:
            rep.add('bean.pad.room', k, 'pad %d room N64 %d, Bean %d' % (k, gp[k][4], bp[k][4]))
    for lo, name in ((0, 'pads'), (10000, 'bound pads')):
        ex = sorted(k for k in set(bp) - set(gp) if (k >= 10000) == (lo == 10000))
        if ex:
            rep.add('bean.pad.extra', name, 'Bean adds %d %s (%d-%d)' % (len(ex), name, ex[0], ex[-1]), len(ex))
        mi = sorted(k for k in set(gp) - set(bp) if (k >= 10000) == (lo == 10000))
        if mi:
            rep.add('bean.pad.missing', name, 'Bean has no %d of the N64\'s %s' % (len(mi), name), len(mi))
    # chrs by number, at the later tick; Bean's head numbers are its own
    gc = {c['chrnum']: c for c in n1['chrs']}
    bc = {c['chrnum']: c for c in b1['chrs']}
    for n in sorted(set(gc) ^ set(bc)):
        rep.add('bean.chr.set', n, 'chr %d is in %s only' % (n, 'the N64' if n in gc else 'Bean'))
    heads = []
    for n in sorted(set(gc) & set(bc)):
        a, b = gc[n], bc[n]
        for k in ('bodynum', 'maxdamage', 'visionrange', 'hearingscale', 'ailist', 'weapons'):
            if a.get(k) != b.get(k):
                rep.add('bean.chr.' + k, n, 'chr %d %s N64 %s, Bean %s' % (n, k, a.get(k), b.get(k)))
        heads.append((n, a['headnum'], b['headnum']))
    worlddiff.mapping_check(rep, 'bean.map.head', heads, 'head')
    a, b = n0['player'], b0['player']
    d = math.dist(a['pos'], b['pos'])
    if d > 1.0:
        rep.add('bean.player.pos', '-', 'Bond spawns %.1f units from the N64\'s spot in Bean' % d, d)
    if abs(((b['theta'] - a['theta'] + 180) % 360) - 180) > 1.0:
        rep.add('bean.player.theta', '-', 'Bond faces %.1f in Bean, %.1f on the N64' % (b['theta'], a['theta']))
    return rep, pairs


def reindex(n64, bean, pairs):
    """Bean's world in the N64's record order (worlddiff compares by index);
    records Bean lacks are the N64's, Bean's extras are dropped."""
    out = copy.deepcopy(bean)
    by = {i: j for i, j in pairs if i is not None and j is not None}
    props = []
    for i, r in enumerate(n64['props']):
        props.append(dict(bean['props'][by[i]], i=i) if i in by else dict(r, from_n64=1))
    out['props'] = props
    gp = {r[0] for r in n64['pads']}
    out['pads'] = [p for p in bean['pads'] if p[0] in gp]   # Bean's own pads have no record of ours
    return out


def selftest(n0, b0, n1, b1):
    """The null: Bean against itself is clean; one dropped record and one moved
    object come back as exactly those two."""
    problems = []
    rep, _ = bean_vs_n64(b0, b0, b1, b1, 'null')
    rep.items = [f for f in rep.items if f['kind'] != 'bean.rec.size']
    if rep.items:
        problems.append('Bean against itself: %d findings, first %s' % (len(rep.items), rep.items[0]['detail']))
    c0 = copy.deepcopy(b0)
    objs = [r for r in c0['props'] if r.get('exists') and not r.get('attached') and 'rtpos' in r]
    if len(objs) < 2:
        return problems + ['too few objects to plant in']
    drop = objs[0]['i']
    moved = objs[len(objs) // 2]
    moved['rtpos'] = [moved['rtpos'][0] + 50.0] + moved['rtpos'][1:]
    c0['props'] = [r for r in c0['props'] if r['i'] != drop]
    rep, _ = bean_vs_n64(b0, c0, b1, b1, 'null')
    kinds = Counter(f['kind'] for f in rep.items if f['kind'] != 'bean.rec.size')
    if kinds != Counter({'bean.rec.missing': 1, 'bean.obj.pos': 1}):
        problems.append('planted drop + move gave %s' % dict(kinds))
    return problems


def ticks(d):
    return sorted(int(re.search(r'_t(\d+)\.json$', f).group(1)) for f in glob.glob(os.path.join(d, 'world_t*.json')))


def load(d, t):
    return json.load(open(os.path.join(d, 'world_t%d.json' % t)))


def run(bean_dir, n64_dir, ours_dir=None, mission='?'):
    tb, tn = ticks(bean_dir), ticks(n64_dir)
    b0, b1 = load(bean_dir, tb[0]), load(bean_dir, tb[-1])
    n0, n1 = load(n64_dir, tn[0]), load(n64_dir, tn[-1])
    problems = selftest(n0, b0, n1, b1)
    if problems:
        raise SystemExit('NULL FAILED on %s: %s' % (bean_dir, '; '.join(problems)))
    rep, pairs = bean_vs_n64(n0, b0, n1, b1, mission)
    findings = list(rep.items)
    if ours_dir:
        to = ticks(ours_dir)
        o0, o1 = load(ours_dir, to[0]), load(ours_dir, to[-1])
        g0 = reindex(n0, b0, pairs)
        pairs1 = align(n1['props'], b1['props'])
        g1 = reindex(n1, b1, pairs1)
        cmp_ = worlddiff.compare(g0, o0, g1, o1, mission)
        # where Bean itself differs from the N64 on the same record and field,
        # ours (a conversion of the N64's) differs from Bean because of Bean:
        # that is Bean's change showing through, listed with Bean's own
        bean_keys = {(f['kind'][len('bean.'):], f['key']) for f in rep.items}
        for f in cmp_.items:
            if (f['kind'], f['key']) in bean_keys or (f['kind'] == 'map.head' and any(k[0] == 'map.head' for k in bean_keys)):
                f['kind'] = 'bean.via.' + f['kind']
                f['detail'] += ' (Bean differs from the N64 here too)'
        findings += cmp_.items
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('bean')
    ap.add_argument('n64')
    ap.add_argument('--ours')
    ap.add_argument('--mission', default='?')
    ap.add_argument('--json')
    a = ap.parse_args()
    f = run(a.bean, a.n64, a.ours, a.mission)
    print('null: Bean against itself clean; a planted drop and move found')
    by = defaultdict(list)
    for x in f:
        by[x['kind']].append(x)
    for kind in sorted(by, key=lambda k: (not k.startswith('bean.'), k)):
        print('### %s (%d)' % (kind, len(by[kind])))
        for x in sorted(by[kind], key=lambda x: -x['mag'])[:12]:
            print('-', x['detail'])
    if a.json:
        json.dump(f, open(a.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())

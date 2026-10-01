#!/usr/bin/env python3
"""The HD draw census: what each GoldenEye XBLA (Bean) model file holds
against what our builds took from it.

    census.py analyse OUT/runs/*.log [--md OUT/report.md --json OUT/report.json]
    census.py plant-check OUT/runs/dam.log new/background/dam@1a2b

The game side comes from a run of the hook build (feat/gefidelity-xbla) with
GEBEAN_CENSUS=1, whose pd.log carries, per load of a release file:

    census: load <bm> <source> keepparts K draws N tex T path P
    census: draw <bm> <pc> prim P count C piece X section S tex T     (one per draw walked)
    census: built|level <bm> <source>  then  census: took <bm> <pc> <triangles>
    census: unbuilt <bm> <source>      (a load nothing was built from: measured against)

The file side is beanref.py, which reads the same file (the path the game
opened, so a Community Edition overlay is judged as itself) without the game's
code. Per file, every draw the release draws is classed:

    drawn       our builds took triangles from it
    unwalked    on the release's path but our walk never kept it
                (a piece or section it leaves out, a planted fault)
    unbuilt     walked, but no build took a triangle (a filter in a build)
    short       took fewer triangles than the draw holds (beyond the
                builds' own small culls, TOL_SHORT)

and rows are sorted by the share of the file's triangles that go undrawn.
Missing and placeholder pictures come from the game's own gebean: lines.

The null runs every time: one draw of a well-built file is struck from a copy
of the data and must come back as unbuilt, and when the runs carry a planted
fault (GEBEAN_CENSUS_SKIP, census.json's "plant") it must come back as
unwalked. Either failing exits 2 with no report.
"""
import argparse, collections, copy, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import beanref  # noqa: E402

TOL_SHORT = 0.10   # a draw that loses more than a tenth of its triangles

RE = {
    'load': re.compile(r'census: load (\S+) (\S+) keepparts (\d+) draws (\d+) tex (\d+) path (.+)$'),
    'draw': re.compile(r'census: draw (\S+) ([0-9a-f]+) prim (\d+) count (\d+) piece (-?\d+) section (-?\d+) tex (\d+)'),
    'end': re.compile(r'census: (built|level|unbuilt) (\S+) (\S+)$'),
    'took': re.compile(r'census: took (\S+) ([0-9a-f]+) (\d+)'),
    'placeholder': re.compile(r'gebean: (\S+): texture (\d+) \(([^)]*)\) is the release\'s magenta placeholder, (drawn with its neighbour|painted)'),
    'nodecode': re.compile(r'gebean: texture (\d+) of (\S+) would not decode'),
    'leftout': re.compile(r'gebean: (\S+) <- (\S+): .*?(\d+) draws left out'),
}


def parse_logs(paths):
    """{source: {'path', 'walked': {pc: drawinfo}, 'took': {pc: max tris}, 'loads', 'built', 'kinds', 'runs'}}"""
    files = {}
    pics = collections.defaultdict(dict)
    leftout = collections.defaultdict(set)
    for path in paths:
        run = os.path.splitext(os.path.basename(path))[0]
        live = {}      # bm -> source, for this run's open loads
        for line in open(path, errors='replace'):
            line = line.rstrip('\n')
            m = RE['load'].search(line)
            if m:
                bm, src = m.group(1), m.group(2)
                live[bm] = src
                f = files.setdefault(src, {'path': m.group(6), 'walked': {}, 'took': {}, 'loads': 0,
                                           'kinds': collections.Counter(), 'runs': set()})
                f['loads'] += 1
                f['runs'].add(run)
                continue
            m = RE['draw'].search(line)
            if m and m.group(1) in live:
                f = files[live[m.group(1)]]
                pc = int(m.group(2), 16)
                f['walked'][pc] = {'prim': int(m.group(3)), 'count': int(m.group(4)),
                                   'piece': int(m.group(5)), 'section': int(m.group(6))}
                continue
            m = RE['end'].search(line)
            if m:
                src = m.group(3)
                if src in files:
                    files[src]['kinds'][m.group(1)] += 1
                live[m.group(2)] = src
                continue
            m = RE['took'].search(line)
            if m and m.group(1) in live:
                f = files[live[m.group(1)]]
                pc = int(m.group(2), 16)
                f['took'][pc] = max(f['took'].get(pc, 0), int(m.group(3)))
                continue
            m = RE['placeholder'].search(line)
            if m:
                pics[m.group(1)][int(m.group(2))] = (m.group(3), 'placeholder ' + m.group(4))
                continue
            m = RE['nodecode'].search(line)
            if m:
                pics[m.group(2)][int(m.group(1))] = ('?', 'would not decode')
                continue
            m = RE['leftout'].search(line)
            if m:
                leftout[m.group(2)].add(int(m.group(3)))
    for src in files:
        files[src]['runs'] = sorted(files[src]['runs'])
    return files, pics, leftout


def classify(src, f, ref):
    """One row: the file's draws against what we walked and took."""
    built = f['kinds'].get('built', 0) + f['kinds'].get('level', 0)
    rows = []
    total = sum(d['tris'] for d in ref.draws if d['how'] == 'main')
    lost = 0
    for d in ref.draws:
        if d['how'] != 'main':
            rows.append(dict(d, state='not on the release path (%s)' % d['how']))
            continue
        pc = d['pc']
        if pc not in f['walked']:
            state = 'unwalked'
        elif not built:
            state = 'measured only'
        else:
            took = f['took'].get(pc, 0)
            if took == 0:
                state = 'unbuilt'
            elif d['tris'] and took < d['tris'] * (1 - TOL_SHORT) and took < d['tris'] - 2:
                state = 'short'
            else:
                state = 'drawn'
            d = dict(d, took=took)
        if state in ('unwalked', 'unbuilt'):
            lost += d['tris']
        elif state == 'short':
            lost += d['tris'] - d['took']
        rows.append(dict(d, state=state))
    return {'source': src, 'path': f['path'], 'loads': f['loads'], 'built': built,
            'kinds': dict(f['kinds']), 'runs': f['runs'], 'tris': total, 'lost': lost,
            'share': (lost / total) if total else 0.0, 'draws': rows}


def analyse(files, pics, leftout, accepted):
    out = []
    for src, f in sorted(files.items()):
        try:
            ref = beanref.BeanFile(src, path=f['path'])
        except Exception as e:  # noqa: BLE001
            out.append({'source': src, 'error': str(e), 'share': 1.0, 'lost': 0, 'tris': 0, 'draws': []})
            continue
        row = classify(src, f, ref)
        row['pictures'] = {str(k): v for k, v in sorted(pics.get(src, {}).items())}
        for d in row['draws']:
            if d['state'] in ('unwalked', 'unbuilt', 'short'):
                d['band'] = ref.where(d)
        row['leftout_logged'] = sorted(leftout.get(src, ()))
        for d in row['draws']:
            reason = accepted_reason(src, d, accepted)
            if reason and d['state'] in ('unwalked', 'unbuilt', 'short'):
                d['accepted'] = reason
        acc = sum(d['tris'] if d['state'] != 'short' else d['tris'] - d.get('took', 0)
                  for d in row['draws'] if d.get('accepted'))
        row['lost_unaccepted'] = row['lost'] - acc
        row['share_unaccepted'] = row['lost_unaccepted'] / row['tris'] if row['tris'] else 0.0
        out.append(row)
    out.sort(key=lambda r: (-r.get('share_unaccepted', r['share']), -r['lost'], r['source']))
    return out


def accepted_reason(src, d, accepted):
    for a in accepted:
        if not re.fullmatch(a.get('source', '.*'), src):
            continue
        if 'state' in a and not re.fullmatch(a['state'], d['state']):
            continue
        if 'minband' in a and not (d.get('band') and d['band'][0] >= a['minband']):
            continue
        if 'section' in a and a['section'] not in d.get('sections', []):
            continue
        if 'piece' in a and a['piece'] != d.get('piece'):
            continue
        if 'pc' in a and int(a['pc'], 16) != d['pc']:
            continue
        return a['reason']
    return None


# ------------------------------------------------------------------ the null

def null_strike(files):
    """Strike one draw of a fully built file from a copy; it must come back unbuilt."""
    for src, f in sorted(files.items()):
        if not (f['kinds'].get('built') and f['took'] and all(v > 0 for v in f['took'].values())):
            continue
        g = copy.deepcopy(f)
        pc = sorted(g['took'])[len(g['took']) // 2]
        g['took'][pc] = 0
        try:
            ref = beanref.BeanFile(src, path=g['path'])
        except Exception:  # noqa: BLE001
            continue
        row = classify(src, g, ref)
        hit = [d for d in row['draws'] if d['pc'] == pc and d['state'] == 'unbuilt']
        return (src, pc, bool(hit))
    return (None, None, False)


def null_plant(logs, plant):
    """The planted run alone (other runs load the same file whole): the
    planted draw must come back unwalked there."""
    if not plant:
        return None
    src, pc = plant.split('@')
    for log in logs:
        if not any('GEBEAN_CENSUS_SKIP' in l or 'census: load' in l for l in open(log, errors='replace')):
            continue
        files, _, _ = parse_logs([log])
        if src not in files or not os.path.exists(log.replace('.log', '.plant')):
            continue
        row = classify(src, files[src], beanref.BeanFile(src, path=files[src]['path']))
        return any(d['pc'] == int(pc, 16) and d['state'] == 'unwalked' for d in row['draws'])
    return False


# ---------------------------------------------------------------- the report

def markdown(rows, plant, strike, coverage, tableless=()):
    L = ['# HD draw census', '',
         'Each GoldenEye XBLA (Bean) file a census run loaded, against what our builds took from it. '
         '"lost" is the share of the file\'s triangles no build drew; accepted draws (accepted.json) are '
         'counted out of the share and marked.', '',
         'Null: struck draw %s@%x %s; planted fault %s %s.' % (
             strike[0], strike[1] or 0, 'found' if strike[2] else 'MISSED',
             plant or '(none in these runs)', '' if plant is None else 'found'), '']
    L += ['| file | lost | triangles | loads/built | runs | what goes undrawn |', '|---|---|---|---|---|---|']
    for r in rows:
        if r.get('error'):
            L.append('| %s | ? | ? | ? | ? | could not read: %s |' % (r['source'], r['error']))
            continue
        bad = [d for d in r['draws'] if d['state'] in ('unwalked', 'unbuilt', 'short') and not d.get('accepted')]
        if not bad and not r['pictures'] and r['built']:
            continue
        what = []
        for d in bad[:6]:
            what.append('%s %x: %d tris%s, %s%s%s%s' % (
                d['state'], d['pc'], d['tris'], (' (took %d)' % d['took']) if d['state'] == 'short' else '',
                d['texname'], (' piece %d' % d['piece']) if d['piece'] >= 0 else '',
                (' section %s' % d['sections']) if d['sections'] else '',
                (' height %.2f-%.2f' % tuple(d['band'])) if d.get('band') else ''))
        if len(bad) > 6:
            what.append('... %d more' % (len(bad) - 6))
        if not r['built']:
            what.insert(0, 'measured only (never built in these runs)')
        for t, (name, how) in r['pictures'].items():
            what.append('picture %s %s: %s' % (t, name, how))
        L.append('| %s | %.0f%% | %d | %d/%d | %s | %s |' % (
            r['source'], 100 * r['share_unaccepted'], r['tris'], r['loads'], r['built'],
            ','.join(r['runs'][:3]) + ('...' if len(r['runs']) > 3 else ''), '<br>'.join(what) or '-'))
    clean = [r['source'] for r in rows if not r.get('error') and r['built'] and r['share_unaccepted'] == 0
             and not r['pictures']]
    L += ['', '%d files drawn whole: %s' % (len(clean), ', '.join(clean)), '']
    if tableless:
        L += ['## Pictures the decoder refuses (static check, every release file)', '',
              'Animated pictures with no frame table: beanDecodeTexture() takes the first frame\'s offset '
              'through a table that is not there and gives up ("would not decode"), so their draws have no picture.', '']
        for src, t, name, frames in tableless:
            L.append('- %s texture %d (%s), %d frames' % (src, t, name, frames))
        L.append('')
    if coverage:
        L += ['## Not exercised', '', 'Release files of these kinds no census run loaded (not judged):', '']
        for kind, names in sorted(coverage.items()):
            L.append('- %s (%d): %s' % (kind, len(names), ', '.join(names)))
    return '\n'.join(L) + '\n'


def tableless_frames():
    """Every release picture with frames (+0x38) and no frame table (+0x3c 0 or
    past the header): beanDecodeTexture() reads its first frame's offset
    through the table and refuses the picture, in files no run loaded too."""
    import struct
    from cafftool import Caff
    out = []
    root = os.path.join(beanref.BEAN, 'files', 'new')
    for kind in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        for name in sorted(os.listdir(os.path.join(root, kind))):
            p = os.path.join(root, kind, name, 'default.bin')
            if not os.path.exists(p):
                continue
            try:
                c = Caff(p)
            except Exception:  # noqa: BLE001
                continue
            t = 0
            for f in c.files:
                b = c.blob(f)
                if b[:8] != b'texture\0':
                    continue
                frames, tab = struct.unpack_from('>II', b, 0x38)
                if frames and (tab == 0 or tab + 4 > len(b)):
                    out.append(('new/%s/%s' % (kind, name), t, c.asset_name(f), frames))
                t += 1
    return out


def coverage_gaps(files):
    gaps = {}
    for kind in ('new/gun', 'new/prop', 'new/char', 'new/head', 'new/background'):
        d = os.path.join(beanref.BEAN, 'files', kind)
        if not os.path.isdir(d):
            continue
        names = sorted(n for n in os.listdir(d) if os.path.exists(os.path.join(d, n, 'default.bin')))
        gaps[kind] = [n for n in names if '%s/%s' % (kind, n) not in files]
    return gaps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--md')
    ap.add_argument('--json')
    ap.add_argument('--plant', help='source@pc the runs planted with GEBEAN_CENSUS_SKIP')
    ap.add_argument('--accepted', default=os.path.join(HERE, 'accepted.json'))
    a = ap.parse_args()
    files, pics, leftout = parse_logs(a.logs)
    if not files:
        print('no census lines in the logs - was the hook build run with GEBEAN_CENSUS=1?')
        return 2
    accepted = json.load(open(a.accepted)) if os.path.exists(a.accepted) else []
    rows = analyse(files, pics, leftout, accepted)
    strike = null_strike(files)
    plant = null_plant(a.logs, a.plant)
    if not strike[2] or plant is False:
        print('NULL FAILED - refusing to report: struck draw %s %s, planted %s %s' % (
            strike[0], 'found' if strike[2] else 'missed', a.plant, plant))
        return 2
    if a.plant:   # the planted draw is the null's, not a finding
        src, pc = a.plant.split('@')
        for r in rows:
            for d in r['draws']:
                if r['source'] == src and d['pc'] == int(pc, 16):
                    d['accepted'] = 'the census\'s planted fault (GEBEAN_CENSUS_SKIP)'
            if r['source'] == src:
                r['lost_unaccepted'] = sum(d['tris'] if d['state'] != 'short' else d['tris'] - d.get('took', 0)
                                           for d in r['draws'] if d['state'] in ('unwalked', 'unbuilt', 'short')
                                           and not d.get('accepted'))
                r['share_unaccepted'] = r['lost_unaccepted'] / r['tris'] if r['tris'] else 0.0
        rows.sort(key=lambda r: (-r.get('share_unaccepted', r['share']), -r['lost'], r['source']))
    tl = tableless_frames()
    md = markdown(rows, a.plant, strike, coverage_gaps(files), tl)
    if a.md:
        open(a.md, 'w').write(md)
    else:
        print(md)
    if a.json:
        slim = [dict(r, draws=[d for d in r['draws'] if d['state'] != 'drawn']) for r in rows]
        json.dump({'plant': a.plant, 'strike': strike, 'files': slim, 'tableless_pictures': tl},
                  open(a.json, 'w'), indent=1, default=list)
    print('null: struck draw found, planted fault %s' % ('found' if plant else 'not planted'), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())

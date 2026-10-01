#!/usr/bin/env python3
"""The census as a gate leg: which GoldenEye fields a source tree's converter
drops, as findings world/compare.py can set against another tree's.

    census/gatesweep.py --tree SRC --out DIR [--rom GE.z64] [--cart CART.json]

Builds the read-tracked port/src/geconvert.c from SRC (ctrack/, its own build
under DIR/run), runs the census and writes DIR/report.json and
DIR/report-missions.json. A finding:

- mission: the level the record came from (a solo setup's mission, an arena's
  `arena:<key>`, a bg/stan file's mission or arena), or the file kind:
  `models`, `characters`, `all` (the data segment and the ROM's own tables)
- kind: `census.dropped` - a non-zero field the converter never reads and
  GoldenEye on the cartridge does (ares/run.py's cart.json: read in any of the
  twenty missions); `census.partial` - read in part, and the cartridge reads
  it; `census.unread` - one the cartridge never reads, only relocates (a model
  pointer modelPromoteNodeOffsetsToPointers turns into an address), or that no
  cartridge run has observed (compare.py lists those and never fails on them)
- key: `<record kind>.<field>` (stable across trees)
- mag: the records that drop it on that mission
- detail: what GoldenEye does with the field (the cartridge's readers)

Without a cart.json every dropped field is census.dropped (unjudged, so it
gates). Null, every run: the census's own (Dam's pad positions hidden must all
come back dropped) and repeatability - the instrumented converter is run twice
on the tree and the two must give the same keys and magnitudes - else exit 2.
"""
import argparse, json, os, subprocess, sys
from collections import defaultdict

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'common'))
import census
import levels

CART = os.path.expanduser('~/wt/gefidelity-run/census-ares/cart.json')
# a solo setup -> its mission; a level's bg/stan stem -> the first mission on it
SOLO = {'UsetupdamZ': 'dam', 'UsetuparkZ': 'facility', 'UsetuprunZ': 'runway', 'UsetupsevxZ': 'surface',
        'UsetupsevbunkerZ': 'bunker', 'UsetupsiloZ': 'silo', 'UsetupdestZ': 'frigate', 'UsetupsevxbZ': 'surface2',
        'UsetupsevbZ': 'bunker2', 'UsetupstatueZ': 'statue', 'UsetuparchZ': 'archives', 'UsetuppeteZ': 'streets',
        'UsetupdepoZ': 'depot', 'UsetuptraZ': 'train', 'UsetupjunZ': 'jungle', 'UsetupcontrolZ': 'control',
        'UsetupcaveZ': 'caverns', 'UsetupcradZ': 'cradle', 'UsetupaztZ': 'aztec', 'UsetupcrypZ': 'egyptian'}
LEVEL = {'dam': 'dam', 'ark': 'facility', 'run': 'runway', 'sevx': 'surface', 'sev': 'bunker', 'silo': 'silo',
         'dest': 'frigate', 'sevb': 'bunker2', 'stat': 'statue', 'arch': 'archives', 'pete': 'streets',
         'depo': 'depot', 'tra': 'train', 'jun': 'jungle', 'arec': 'control', 'cave': 'caverns', 'crad': 'cradle',
         'azt': 'aztec', 'cryp': 'egyptian'}


def mission_of(label):
    if label in SOLO:
        return SOLO[label]
    if label.startswith('Ump_setup'):
        return 'arena:' + label[len('Ump_setup'):-1]
    for pre, suf in (('Tbg_', '_all_p_stanZ'), ('bg_', '_all_p')):
        if label.startswith(pre) and label.endswith(suf):
            lv = label[len(pre):-len(suf)]
            return LEVEL.get(lv, 'arena:' + lv)
    if label in ('data', 'rom'):
        return 'all'
    if label.startswith('C'):
        return 'characters'
    return 'models'


def findings(rows, cart):
    """Per mission and field. Where the cartridge held the very file a record was
    dropped from, the record's own fate decides (census.dropped: the cartridge
    read that record's field; census.unread: it held it and never read it);
    where it never held that file (an arena's setup, the ROM's own tables), the
    field's fate anywhere else does."""
    out = []
    stems = set(cart.get('stems', ())) if cart else set()
    for x in rows:
        says = census.cart_says(cart, x['kind'], x['field'], x)
        readers = ', '.join('%s %d' % (n, k) for n, k in says.get('readers', [])[:3])
        key = '%s.%s' % (x['kind'], x['field'])
        bym = defaultdict(lambda: [0, 0, 0, 0])     # dropped, partial, read by the cartridge, observed
        for label, n in x['byfile'].items():
            m = bym[mission_of(label)]
            m[0] += n
            m[1] += x['partfile'].get(label, 0)
            if label in stems:
                m[2] += x.get('drop_cartfile', {}).get(label, 0)
                m[3] += n
        for mission, (dropped, partial, cartread, observed) in sorted(bym.items()):
            if not cart:
                out.append(dict(mission=mission, kind='census.dropped', key=key, mag=float(dropped),
                                detail='not checked on the cartridge'))
                continue
            if observed:
                if says['state'] == 'relocated':
                    cartread = 0        # turning an offset into a pointer is not a use
                if cartread:
                    kind = 'census.partial' if partial >= dropped else 'census.dropped'
                    out.append(dict(mission=mission, kind=kind, key=key, mag=float(cartread),
                                    detail='GoldenEye reads %d of the %d dropped records on the cartridge (%s)' % (
                                        cartread, observed, readers)))
                if observed > cartread:
                    out.append(dict(mission=mission, kind='census.unread', key=key, mag=float(observed - cartread),
                                    detail='GoldenEye held %d dropped records on the cartridge and never read them' % (
                                        observed - cartread)))
                if dropped == observed:
                    continue
                dropped -= observed
            if says['state'] == 'read':
                out.append(dict(mission=mission, kind='census.dropped', key=key, mag=float(dropped),
                                detail='not held on the cartridge here; GoldenEye reads the field elsewhere: '
                                       '%d/%d records (%s)' % (says['read'], says['records'], readers)))
            else:
                why = {'relocated': 'GoldenEye only relocates it (%s)' % readers,
                       'never': 'GoldenEye never read it on the cartridge (0/%d records)' % says.get('records', 0)}
                out.append(dict(mission=mission, kind='census.unread', key=key, mag=float(dropped),
                                detail=why.get(says['state'], 'not observed on the cartridge')))
    return sorted(out, key=lambda f: (f['mission'], f['kind'], f['key']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rom', default=census.ROM_DEFAULT)
    ap.add_argument('--cart', default=CART)
    a = ap.parse_args()
    tree, out = os.path.abspath(a.tree), os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    cart = json.load(open(a.cart)) if a.cart and os.path.exists(a.cart) else None
    if not cart:
        print('census gate: no cartridge column (%s); every dropped field gates' % a.cart)
    try:
        runs = []
        for k in (1, 2):
            roots, info, ok, msg = census.census(tree, a.rom, os.path.join(out, 'run%d' % k))
            print('[run %d] %s' % (k, msg))
            if not ok:
                print('census gate: NULL FAILED (%s)' % msg)
                return 2
            c = census.analyse(info, roots, cartrecs=census.cart_records(cart))
            census.report(c, info, msg, os.path.join(out, 'census%d.md' % k), os.path.join(out, 'census%d.json' % k),
                          60, cart)
            rows = json.load(open(os.path.join(out, 'census%d.json' % k)))['dropped']
            runs.append((findings(rows, cart), sorted({mission_of(r.key[5:].split('#')[0])
                                                        for r in roots.values() if r.key.startswith('file:')} | {'all'})))
    except (subprocess.CalledProcessError, SystemExit, OSError) as e:
        print('census gate: the census did not run on %s: %s' % (tree, e))
        return 2
    sig = lambda fs: [(f['mission'], f['kind'], f['key'], f['mag']) for f in fs]
    if sig(runs[0][0]) != sig(runs[1][0]):
        print('census gate: NULL FAILED - two runs on one tree disagree (%d vs %d findings)' % (
            len(runs[0][0]), len(runs[1][0])))
        return 2
    fs, missions = runs[0]
    json.dump(fs, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    json.dump({'compared': missions}, open(os.path.join(out, 'report-missions.json'), 'w'), indent=1)
    n = defaultdict(int)
    for f in fs:
        n[f['kind']] += 1
    print('census gate: %s; repeatable over two runs; %s' % (
        ', '.join('%d %s' % (v, k) for k, v in sorted(n.items())), os.path.join(out, 'report.json')))
    return 0


if __name__ == '__main__':
    sys.exit(main())

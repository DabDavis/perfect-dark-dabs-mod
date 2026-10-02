#!/usr/bin/env python3
"""Compares world/collect.py's two sides: per record, whether each game made
the pickup and Bond carries its prop after walking onto it, and the
objectives after each pickup and at the end.

    world/collectdiff.py OUT        # OUT/ge/collect.json against OUT/pd/collect.json

The items' numbers are each game's own (a ROM hack's collectable is its item
number on the cartridge and a key card 0x45-0x4c here), so what is compared is
how many each gained, never the numbers. GoldenEye carries a collectable twice,
as its item and as a prop entry naming the record's own prop (what
PROPDEF_OBJECTIVE_COLLECT_OBJECT looks for); ours once, as a key card that is
both, so the cartridge's entry for the record's own prop counts with its item
as one pickup (and alone, for an item carried only as a prop, as one).
A record Bond could not be stood beside on either side (collect.py's
`reached`) is listed UNREACHED and not compared. Exit 1 on any difference, 2
when only unreached records stand in the way.
"""
import json, os, sys


def load(out, side):
    return json.load(open(os.path.join(out, side, 'collect.json')))


def main():
    out = sys.argv[1]
    ge, pd = load(out, 'ge'), load(out, 'pd')
    bad = []
    if len(ge['objectives_before']) != len(pd['objectives_before']):
        bad.append('objective count: cartridge %d, ours %d' % (len(ge['objectives_before']), len(pd['objectives_before'])))
    if ge['objectives_before'] != pd['objectives_before']:
        bad.append('objectives before: cartridge %s, ours %s' % (ge['objectives_before'], pd['objectives_before']))
    pdrows = {r['i']: r for r in pd['records']}
    print('mission %d  objectives before  ge %s  pd %s' % (ge['mission'], ge['objectives_before'], pd['objectives_before']))
    unreached = []
    for g in ge['records']:
        p = pdrows.get(g['i'], {})
        if g.get('reached') is False or p.get('reached') is False:
            # Bond never stood on the floor under it on one side: inconclusive
            unreached.append(g['i'])
            print('  record %4d  UNREACHED (cartridge %s, ours %s): not compared' % (
                g['i'], g.get('reached'), p.get('reached')))
            continue
        cells = []
        for key, fmt in (('had_prop', '%s'), ('carried', '%s'), ('gained', '%s'), ('objectives', '%s')):
            gv, pv = g.get(key), p.get(key)
            if key == 'gained':
                own = g.get('prop')
                isown = lambda w: (isinstance(w, list) and w[0] == 'prop' and own
                                   and int(w[1], 16) & 0xffffffff == int(own, 16))
                others = [w for w in (gv or []) if not isown(w)]
                # the item and its prop entry are one pickup; a prop entry alone is one too
                n = len(others) or (1 if any(isown(w) for w in (gv or [])) else 0)
                same = n == len(pv or [])
                gv = others or [w for w in (gv or []) if isown(w)]
            else:
                same = gv == pv
            # what the inventory gained is shown, never judged: GoldenEye lists a
            # second gold bar as a prop entry of its own, ours holds one key card
            # for all eight; whether each was taken is `carried`
            cells.append('%s %s/%s%s' % (key, fmt % (gv,), fmt % (pv,), '' if same else (' ~' if key == 'gained' else ' <-')))
            if not same and key != 'gained':
                bad.append('record %d %s: cartridge %s, ours %s' % (g['i'], key, gv, pv))
        print('  record %4d  %s' % (g['i'], '  '.join(cells)))
    print('objectives after  ge %s  pd %s' % (ge['objectives_after'], pd['objectives_after']))
    if ge['objectives_after'] != pd['objectives_after']:
        bad.append('objectives after: cartridge %s, ours %s' % (ge['objectives_after'], pd['objectives_after']))
    for b in bad:
        print('DIFF', b)
    if unreached:
        print('UNREACHED records %s: not compared, run again' % unreached)
    print('same' if not bad and not unreached else 'same where compared' if not bad else '%d differences' % len(bad))
    return 1 if bad else (2 if unreached else 0)


if __name__ == '__main__':
    sys.exit(main())

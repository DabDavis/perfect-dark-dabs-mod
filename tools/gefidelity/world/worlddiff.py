#!/usr/bin/env python3
"""Compare the two sides' world dumps (world/dump.py) of one mission.

    worlddiff.py OUT/dam                      # report on stdout
    worlddiff.py OUT/dam --md r.md --json r.json
    worlddiff.py --selftest OUT/dam           # the null alone

OUT/<mission> holds ge/world_t<N>.json and pd/world_t<N>.json. Placement (pads,
records, objects) is judged at the earliest tick both sides dumped, chrs at the
latest - guards spend their first seconds settling onto the floor.

A converted level is GoldenEye's world plus one offset per level; the offset is
measured here from the pads, never assumed, and its spread is itself a finding.
Room numbers carry over unchanged. Everything else that differs by design is in
TRANSLATE below, each with the reason; anything that differs and is not
explained there is a finding. A finding someone has looked at and accepted goes
in world/accepted.json with the reason, and is then counted, not listed.

Every run first runs its null (selftest()): GoldenEye's own dump moved by the
offset must give no findings, and the same dump with five planted faults must
give exactly those five. A diff that cannot see a planted fault exits 2.
"""
import argparse, copy, glob, json, math, os, re, statistics, sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'common'))
import levels  # noqa: E402

# ------------------------------------------------------------- translations
# What the conversion changes on purpose. Each is checked as a mapping: every
# GoldenEye value must translate to one Perfect Dark value, the same one each
# time it occurs.

GE_BOUNDPAD_BASE = 10000   # GoldenEye numbers bound pads from 10000; the conversion appends them to the pads


def ge_pad_to_pd(n, ge_numpads):
    return ge_numpads + (n - GE_BOUNDPAD_BASE) if n >= GE_BOUNDPAD_BASE else n


def ge_ailist_to_pd(i):
    """GoldenEye's global AI lists (ids under 0x400) become our ids 0x800 + n."""
    if isinstance(i, int) and 0 <= i < 0x400:
        return 0x800 + i
    return i


def ge_obj_hp(o):
    """The decomp names an object's two health words the other way round from
    ours: GoldenEye's `damage - maxdamage` is our `maxdamage - damage`
    (chrai.c:2284 against propobj.c:12005)."""
    return (o.get('maxdamage'), o.get('damage'))   # (taken, max), our order


def pd_obj_hp(o):
    return (o.get('damage'), o.get('maxdamage'))


# GoldenEye's PROPDEF_END is 0x30; ours is 0x34. Every other type number is the
# same except these, which the conversion rewrites on purpose (the record keeps
# its place, so the indices still line up):
GE_TYPE_TO_PD = {
    0x22: 0x1a,   # PROPDEF_OBJECTIVE_COPY_ITEM (Bunker's "copy the key") becomes a
                  # completion condition on the flag the key analyser sets; our 0x22
                  # is the one-word "nothing" (gesolo.py GE_COPYITEM)
}
TYPE_NAMES = {1: 'door', 3: 'prop', 4: 'key', 5: 'alarm', 6: 'cctv', 7: 'magazine', 8: 'weapon', 9: 'guard',
              10: 'monitor', 11: 'multimonitor', 12: 'rack', 13: 'autogun', 14: 'link', 17: 'hat',
              18: 'chrattr', 19: 'switch', 20: 'ammo', 21: 'armour', 22: 'tag', 23: 'obj-start',
              24: 'obj-end', 35: 'objtext', 36: 'gas', 37: 'rename', 38: 'lockdoor', 39: 'vehicle',
              40: 'aircraft', 42: 'glass', 43: 'safe', 44: 'safeitem', 45: 'tank', 46: 'campos', 47: 'tintedglass'}

TOL_PAD = 1.0        # units; pads are copied and rounded to whole units (Bunker's sit up to 1.0 out)
TOL_OBJ = 2.0        # units; an object GoldenEye places at load
TOL_CHR = 10.0       # units; a chr after its first seconds
TOL_ROT = 0.02       # largest element difference of the 3x3
TOL_DOOR = 0.05      # door open fraction


def ge_door_open(o):
    """How far open a GoldenEye door is, 0..1: openPosition out of maxFrac. The
    decomp's `frac` is not that - with unkac and unkb0 it is the slide vector
    (propobj.c:12506 `frac * openPosition + runtime_pos.x`), dumped as door_slide."""
    op, mx = o.get('door_openPosition'), o.get('door_maxFrac')
    if op is None:
        return None
    if op == 0:
        return 0.0
    return min(1.0, abs(op) / abs(mx)) if mx else 1.0


def pd_door_open(o):
    """Ours: frac out of maxfrac."""
    mx = o.get('door_maxFrac')
    if mx is None:
        return None
    if o['door_frac'] == 0:
        return 0.0
    return min(1.0, abs(o['door_frac']) / abs(mx)) if mx else 1.0


def _bitnames(path, prefixes):
    """{prefix: {bit value: name}} from a header's #defines or enum lines."""
    out = {p: {} for p in prefixes}
    if not os.path.exists(path):
        return out
    for line in open(path, errors='replace'):
        m = re.match(r'\s*(?:#define\s+)?(\w+)\s*=?\s*(0x[0-9a-fA-F]+)', line)
        if not m:
            continue
        for p in prefixes:
            if m.group(1).startswith(p):
                v = int(m.group(2), 16)
                if v and v & (v - 1) == 0:
                    out[p].setdefault(v, m.group(1))
    return out


_GE_TREE = os.environ.get('GF_GE_SRC', os.path.expanduser('~/perfect-dark/claude-007/007'))
_PD_TREE = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
GE_BITS = _bitnames(os.path.join(_GE_TREE, 'src/bondconstants.h'), ('CHRHIDDEN_', 'CHRFLAG_'))
PD_BITS = _bitnames(os.path.join(_PD_TREE, 'src/include/constants.h'), ('CHRHFLAG_', 'CHRCFLAG_'))
BITFIELDS = {'hidden': ('CHRHIDDEN_', 'CHRHFLAG_'), 'chrflags': ('CHRFLAG_', 'CHRCFLAG_')}
# the wide dump (dump.py with GF_WIDE=1) is compared by widediff.py; objects' flag words by these names
import widediff  # noqa: E402
GE_OBJBITS = _bitnames(os.path.join(_GE_TREE, 'src/bondconstants.h'), ('PROPFLAG_', 'PROPFLAG2_'))
PD_OBJBITS = _bitnames(os.path.join(_PD_TREE, 'src/include/constants.h'), ('OBJFLAG_', 'OBJFLAG2_'))


def bitname_obj(field, m):
    g, p = ('PROPFLAG_', 'OBJFLAG_') if field == 'flags' else ('PROPFLAG2_', 'OBJFLAG2_')
    return '%s / %s' % (GE_OBJBITS[g].get(m, '?'), PD_OBJBITS[p].get(m, '?'))


def bitname(field, m):
    g, p = BITFIELDS[field]
    return '%s / %s' % (GE_BITS[g].get(m, '?'), PD_BITS[p].get(m, '?'))


def tname(t):
    return TYPE_NAMES.get(t, 'type%d' % t)


def offset_from_pads(ge, pd):
    gp = {r[0]: r for r in ge['pads'] if r[0] < GE_BOUNDPAD_BASE}
    pp = {r[0]: r for r in pd['pads']}
    pairs = [(gp[i], pp[i]) for i in gp if i in pp]
    if not pairs:
        return [0.0, 0.0, 0.0], math.inf
    offs = [[b[k] - a[k] for k in (1, 2, 3)] for a, b in pairs]
    off = [statistics.median(o[k] for o in offs) for k in range(3)]
    # the spread of the middle 95%: one pad out is that pad's finding, not the level's
    devs = sorted(max(abs(o[k] - off[k]) for k in range(3)) for o in offs)
    spread = devs[min(len(devs) - 1, int(len(devs) * 0.95))]
    return off, spread


class Report:
    def __init__(self, mission):
        self.mission = mission
        self.items = []

    def add(self, kind, key, detail, mag=0.0):
        self.items.append({'mission': self.mission, 'kind': kind, 'key': str(key), 'detail': detail,
                           'mag': round(float(mag), 3)})


# GoldenEye's random head pools (chr.c's random_male_heads, random_female_heads):
# a head outside them was named by the guard's record
GE_POOL_HEADS = {57, 54, 55, 62, 59, 56, 58, 53, 52, 51, 42, 43, 44, 45, 46, 47, 48, 49, 50,
                 63, 64, 65, 66, 67, 68, 70, 71, 72, 73}


def named_heads(ge0):
    """The chrs whose guard record names a head (GoldenEye's wide dump), or None
    without a wide dump."""
    if 'wide' not in ge0:
        return None
    return {r['chrnum'] for r in ge0['wide'].get('props', {}).values()
            if isinstance(r, dict) and 'bitflags' in r and r.get('head', -1) >= 0}


def body_key(body, head, named):
    """A body as the conversion maps it: worn with a head its record names, a
    GoldenEye body takes a row of its own that carries that head (gexplus.c's
    geRomBodyRow(): a record's head byte cannot name a head row, which are past
    255), so Archives' chr 1 - body 19 with head 69 - is row 153 and its other
    body-19 guards, whose heads are the pool's, row 155. The same body under two
    keys is that, not a fault."""
    return '%d+head%d' % (body, head) if named else body


def mapping_check(rep, kind, pairs, note):
    """pairs: (key, ge value, pd value). Learns ge -> pd by majority and reports
    each key that disagrees with what its value maps to everywhere else."""
    votes = defaultdict(Counter)
    for _, g, p in pairs:
        votes[g][p] += 1
    best = {g: c.most_common(1)[0][0] for g, c in votes.items()}
    for g, c in sorted(votes.items(), key=lambda kv: str(kv[0])):
        if len(c) > 1:
            keys = [k for k, gg, p in pairs if gg == g and p != best[g]]
            rep.add(kind, g, '%s: GoldenEye %r becomes %s (by count) - one GoldenEye value, several of ours; '
                    'keys off the majority: %s' % (note, g, dict(c.most_common()), keys[:12]), sum(c.values()) - c[best[g]])
    return best


def compare(ge0, pd0, ge1, pd1, mission):
    """ge0/pd0: earliest tick (placement), ge1/pd1: latest tick (chrs)."""
    rep = Report(mission)
    off, spread = offset_from_pads(ge0, pd0)
    rep.offset, rep.spread = off, spread
    if spread > TOL_PAD:
        rep.add('pad.offset', '-', 'the pads do not share one offset: median %s, 95%% of pads within %.2f' % (
            [round(v, 2) for v in off], spread), spread)

    def g2p(pos):
        return [pos[k] + off[k] for k in range(3)]

    # ---- pads
    ge_numpads = sum(1 for r in ge0['pads'] if r[0] < GE_BOUNDPAD_BASE)
    gp = {ge_pad_to_pd(r[0], ge_numpads): r for r in ge0['pads']}
    pp = {r[0]: r for r in pd0['pads']}
    if len(gp) != len(pp):
        rep.add('pad.count', '-', 'GoldenEye %d pads (bound pads included), ours %d' % (len(gp), len(pp)),
                abs(len(gp) - len(pp)))
    for i in sorted(set(gp) & set(pp)):
        a, b = gp[i], pp[i]
        d = math.dist(g2p(a[1:4]), b[1:4])
        if d > TOL_PAD:
            rep.add('pad.pos', a[0], 'pad %d (ours %d) is %.1f units out' % (a[0], i, d), d)
        if a[4] >= 0 and a[4] != b[4]:
            rep.add('pad.room', a[0], 'pad %d: GoldenEye room %d, ours %d' % (a[0], a[4], b[4]))

    # ---- setup records, one ours for each of GoldenEye's
    gr, pr = ge0['props'], pd0['props']
    if len(gr) != len(pr):
        rep.add('rec.count', '-', 'GoldenEye %d setup records, ours %d' % (len(gr), len(pr)), abs(len(gr) - len(pr)))
    model_pairs = []
    for a, b in zip(gr, pr):
        i = a['i']
        if GE_TYPE_TO_PD.get(a['type'], a['type']) != b['type']:
            rep.add('rec.type', i, 'record %d: GoldenEye %s, ours %s - the records are out of step' % (
                i, tname(a['type']), tname(b['type'])))
            continue
        if 'model' not in a:
            continue
        what = '%s %d (model %d)' % (tname(a['type']), i, a['model'])
        model_pairs.append((i, a['model'], b.get('model')))
        if a.get('exists') != b.get('exists'):
            rep.add('obj.exists', i, '%s: %s in GoldenEye, %s in ours' % (
                what, 'made' if a.get('exists') else 'not made', 'made' if b.get('exists') else 'not made'))
            continue
        if not a.get('exists'):
            continue
        if a.get('attached') or b.get('attached'):
            if a.get('attached') != b.get('attached'):
                rep.add('obj.attached', i, '%s: %s in GoldenEye, %s in ours' % (
                    what, 'held/attached' if a.get('attached') else 'free', 'held/attached' if b.get('attached') else 'free'))
            continue
        # GoldenEye places the model and takes its rooms at runtime_pos; for some
        # types (glass) it then moves prop->pos off it. Ours has one position.
        place = a.get('rtpos', a['pos'])
        here = b['pos']
        if a['type'] == 1 and 'door_startpos' in b:
            # a door is judged shut: its prop moves as it slides in ours, not in GoldenEye
            here = b['door_startpos']
            # door_slide is dumped on both sides but not compared: GoldenEye's is the
            # whole slide in units (Caverns 219.95), our unk98 is not the same
            # quantity (0.15, and of the other sign) - find what it is first
        d = math.dist(g2p(place), here)
        if d > TOL_OBJ:
            delta = [round(here[k] - g2p(place)[k], 1) for k in range(3)]
            rep.add('obj.pos', i, '%s is %.1f units out (ours - GoldenEye = %s)' % (what, d, delta), d)
        elif math.dist(g2p(a['pos']), b.get('refpos', here)) > TOL_OBJ:
            rep.add('obj.refpos', i, '%s: ours sits at GoldenEye\'s runtime_pos (its collision), but GoldenEye\'s '
                    'prop->pos (its rooms, propobj.c:952) is %.1f units away at %s - which of the two GoldenEye '
                    'draws from is for the view diff to show' % (
                        what, math.dist(a['pos'], place), [round(v, 1) for v in g2p(a['pos'])]),
                    math.dist(a['pos'], place))
        rot = max(abs(a['rot'][r][c] - b['rot'][r][c]) for r in range(3) for c in range(3))
        if rot > TOL_ROT:
            rep.add('obj.rot', i, '%s turned differently (largest element %.3f out)' % (what, rot), rot)
        if a.get('scale') is not None and b.get('scale') is not None and abs(a['scale'] - b['scale']) > 0.002 * max(1, a['scale']):
            rep.add('obj.scale', i, '%s scale GoldenEye %.4f, ours %.4f' % (what, a['scale'], b['scale']),
                    abs(math.log(max(b['scale'], 1e-6) / max(a['scale'], 1e-6))))
        if set(a['rooms']) != set(b['rooms']):
            rep.add('obj.rooms', i, '%s rooms GoldenEye %s, ours %s' % (what, sorted(a['rooms']), sorted(b['rooms'])))
        ha, hb = ge_obj_hp(a), pd_obj_hp(b)
        if ha != hb:
            rep.add('obj.health', i, '%s health (taken, max) GoldenEye %s, ours %s' % (what, ha, hb),
                    abs((ha[1] or 0) - (hb[1] or 0)))
        if a['type'] == 1 and 'door_frac' in b:
            fa, fb = ge_door_open(a), pd_door_open(b)
            if fa is not None and fb is not None and abs(fa - fb) > TOL_DOOR:
                rep.add('obj.door', i, '%s open GoldenEye %.2f, ours %.2f (GoldenEye openPosition %s of %s; ours frac %s of %s)' % (
                    what, fa, fb, a.get('door_openPosition'), a.get('door_maxFrac'), b['door_frac'], b.get('door_maxFrac')),
                    abs(fa - fb))
    mapping_check(rep, 'map.model', model_pairs, 'model')

    # ---- chrs, by chr number, at the latest tick
    # a chr one side made at load is placement; one a script spawned later is behaviour
    g0 = {c['chrnum'] for c in ge0['chrs']}
    p0 = {c['chrnum'] for c in pd0['chrs'] if not c.get('player')}
    for n in sorted(g0 - p0):
        rep.add('chr.missing', n, 'chr %d is made at load in GoldenEye and not in ours' % n)
    for n in sorted(p0 - g0):
        rep.add('chr.extra', n, 'chr %d is made at load in ours and not in GoldenEye' % n)
    gc = {c['chrnum']: c for c in ge1['chrs']}
    pc = {c['chrnum']: c for c in pd1['chrs'] if not c.get('player')}
    for n in sorted((set(gc) - set(pc)) - (g0 - p0)):
        rep.add('chr.spawned', n, 'chr %d is there at tick %d in GoldenEye only' % (n, ge1['tick']))
    for n in sorted((set(pc) - set(gc)) - (p0 - g0)):
        rep.add('chr.spawned', n, 'chr %d is there at tick %d in ours only' % (n, ge1['tick']))
    heads, bodies, weapons, ails = [], [], [], []
    bitdiff = Counter()
    for n in sorted(set(gc) & set(pc)):
        a, b = gc[n], pc[n]
        d = math.dist(g2p(a['pos']), b['pos'])
        if d > TOL_CHR:
            rep.add('chr.pos', n, 'chr %d is %.1f units out at tick %d' % (n, d, ge1['tick']), d)
        if set(a['rooms']) != set(b['rooms']):
            rep.add('chr.rooms', n, 'chr %d rooms GoldenEye %s, ours %s' % (n, sorted(a['rooms']), sorted(b['rooms'])))
        for k in ('maxdamage', 'damage', 'accuracyrating', 'speedrating', 'visionrange', 'hearingscale', 'morale', 'alertness'):
            if k in a and k in b and abs(float(a[k]) - float(b[k])) > 1e-3:
                rep.add('chr.' + k, n, 'chr %d %s GoldenEye %s, ours %s' % (n, k, a[k], b[k]),
                        abs(float(a[k]) - float(b[k])))
        if a.get('scale') and b.get('scale') and abs(a['scale'] - b['scale']) > 0.002:
            rep.add('chr.scale', n, 'chr %d model scale GoldenEye %.4f, ours %.4f' % (n, a['scale'], b['scale']))
        ails.append((n, ge_ailist_to_pd(a['ailist']), b['ailist']))
        for field in ('hidden', 'chrflags'):
            x, y = a.get(field, 0), b.get(field, 0)
            for bit in range(32):
                m = 1 << bit
                if (x & m) != (y & m):
                    bitdiff[(field, m, 'GoldenEye only' if x & m else 'ours only')] += 1
    # what a chr is made of is settled at load: judge it there, before a script
    # hands a guard a second gun
    gl = {c['chrnum']: c for c in ge0['chrs']}
    pl = {c['chrnum']: c for c in pd0['chrs'] if not c.get('player')}
    named = named_heads(ge0)
    for n in sorted(set(gl) & set(pl)):
        a, b = gl[n], pl[n]
        heads.append((n, a['headnum'], b['headnum']))
        bodies.append((n, body_key(a['bodynum'], a['headnum'], n in named if named is not None
                                   else a['headnum'] not in GE_POOL_HEADS), b['bodynum']))
        weapons.append((n, tuple(a['weapons']), tuple(b['weapons'])))
    mapping_check(rep, 'map.body', bodies, 'body')
    mapping_check(rep, 'map.head', heads, 'head')
    mapping_check(rep, 'map.weapon', weapons, 'weapons held')
    for n, ga, pa in ails:
        if ga != pa:
            rep.add('chr.ailist', n, 'chr %d runs AI list %s in GoldenEye (0x%x as ours), %s in ours' % (
                n, ga, ga if isinstance(ga, int) else 0, pa))
    for (field, m, side), cnt in sorted(bitdiff.items()):
        rep.add('chr.bits', '%s:0x%x' % (field, m), '%s bit 0x%x (GoldenEye / ours: %s) set in %s on %d chrs - '
                'the same bit means different things in the two games unless the names agree' % (
                    field, m, bitname(field, m), side, cnt), cnt)

    # ---- the wide dump: what each record decides beyond its placement, and the tile graph
    if 'wide' in ge0 and 'wide' in pd0:
        widediff.compare(rep, ge0, pd0, off, ge_numpads, ge_pad_to_pd, ge_ailist_to_pd, mapping_check, tname, bitname_obj)
    if 'wide_chrweapons' in ge1 and 'wide_chrweapons' in pd1:
        widediff.compare_chrweapons(rep, ge1, pd1, mapping_check, bitname_obj)

    # ---- the player
    a, b = ge1['player'], pd1['player']
    d = math.dist([a['pos'][0] + off[0], a['pos'][2] + off[2]], [b['pos'][0], b['pos'][2]])
    if d > TOL_CHR:
        rep.add('player.pos', '-', 'Bond is %.1f units out across the floor at tick %d (the native port leaves '
                'his prop at its spawn values for a while, so judge this one on --oracle ares)' % (d, ge1['tick']), d)
    # the eye's height over the floor under Bond, at spawn: later GoldenEye may be
    # playing an animation (Caverns at tick 300 has his eye 106 over the floor)
    a0, b0 = ge0['player'], pd0['player']
    if 'ground' in a0 and 'ground' in b0:
        dy = (b0['eye'][1] - b0['ground']) - (a0['eye'][1] - a0['ground'])
        what = 'over the floor'
    else:
        dy = (b['eye'][1]) - (a['eye'][1] + off[1])
        what = '(absolute; no floor height dumped)'
    if abs(dy) > 1.0:
        rep.add('player.eye', '-', 'Bond\'s eye is %.1f units %s than GoldenEye\'s %s at spawn' % (
            abs(dy), 'higher' if dy > 0 else 'lower', what), abs(dy))
    dth = (b['theta'] - a['theta'] + 180) % 360 - 180
    if abs(dth) > 1.0:
        rep.add('player.theta', '-', 'Bond faces %.1f degrees off GoldenEye at tick %d' % (dth, ge1['tick']), abs(dth))
    return rep


# ------------------------------------------------------------------ the null

def _as_ours(ge, off):
    """GoldenEye's own dump dressed as ours: the offset added, the translations applied."""
    pd = copy.deepcopy(ge)
    numpads = sum(1 for r in ge['pads'] if r[0] < GE_BOUNDPAD_BASE)
    pd['pads'] = [[ge_pad_to_pd(r[0], numpads)] + [r[k] + off[k - 1] for k in (1, 2, 3)] + [r[4]] for r in ge['pads']]
    for o in pd['props']:
        o['type'] = GE_TYPE_TO_PD.get(o['type'], o['type'])
        if o.get('exists') and not o.get('attached'):
            # a perfect conversion would also move its prop off the model where
            # GoldenEye does; ours has no field for it, so the copy carries one
            o['refpos'] = [o['pos'][k] + off[k] for k in range(3)]
            o['pos'] = [o.get('rtpos', o['pos'])[k] + off[k] for k in range(3)]
            o.pop('rtpos', None)
        if 'maxdamage' in o:
            o['maxdamage'], o['damage'] = o['damage'], o['maxdamage']
        if o.get('type') == 1 and 'door_openPosition' in o:
            o['door_frac'], o['door_maxFrac'] = o['door_openPosition'], o.get('door_maxFrac')
            if o.get('exists'):
                o['door_startpos'] = list(o['pos'])
    for c in pd['chrs']:
        c['pos'] = [c['pos'][k] + off[k] for k in range(3)]
        c['ailist'] = ge_ailist_to_pd(c['ailist'])
    for k in ('pos', 'eye'):
        pd['player'][k] = [pd['player'][k][j] + off[j] for j in range(3)]
    if 'ground' in pd['player']:
        pd['player']['ground'] += off[1]
    if 'wide' in ge:
        pd['wide'] = widediff.as_ours(ge['wide'], off, numpads, ge_pad_to_pd, ge_ailist_to_pd)
        for o in pd['props']:
            if o.get('type') == 1 and 'flags' in o:
                o['flags'] = widediff.door_top_byte(o['flags'])
    return pd


def selftest(ge0, ge1):
    off = [-1234.5, 678.0, 9012.25]
    pd0, pd1 = _as_ours(ge0, off), _as_ours(ge1, off)
    clean = compare(ge0, pd0, ge1, pd1, 'null')
    problems = []
    if clean.items:
        problems.append('a clean copy gave %d findings, first: %s' % (len(clean.items), clean.items[0]['detail']))
    planted = {}
    objs = [o for o in pd0['props'] if o.get('exists') and not o.get('attached')]
    if objs:
        o = objs[len(objs) // 2]
        o['pos'][0] += 50.0
        if 'door_startpos' in o:
            o['door_startpos'][0] += 50.0   # a door is judged where it sits shut
        planted['obj.pos'] = str(o['i'])
    if len(objs) > 1:
        objs[0]['rooms'] = objs[0]['rooms'] + [254]
        planted['obj.rooms'] = str(objs[0]['i'])
    if pd0['pads']:
        pd0['pads'][3][2] += 7.0
        planted['pad.pos'] = str(ge0['pads'][3][0])
    if pd1['chrs']:
        pd1['chrs'][-1]['maxdamage'] = float(pd1['chrs'][-1]['maxdamage']) + 1
        planted['chr.maxdamage'] = str(pd1['chrs'][-1]['chrnum'])
    pd1['player']['theta'] += 30.0
    planted['player.theta'] = '-'
    if 'wide' in pd0:
        planted.update(widediff.plant(pd0['wide']))
        # and an object's flag word, compared bit by bit only on a wide dump
        fo = next((o for o in pd0['props'] if 'flags' in o and o.get('type') not in (None, 1)), None)
        if fo is not None:
            fo['flags'] ^= 0x00000400
            planted['obj.flagbits'] = 'flags:0x400:%s' % tname(fo['type'])
    dirty = compare(ge0, pd0, ge1, pd1, 'null')
    got = {(f['kind'], f['key']) for f in dirty.items}
    for kind, key in planted.items():
        if (kind, key) not in got:
            problems.append('planted %s %s was not found' % (kind, key))
    extra = got - set(planted.items())
    if extra:
        problems.append('planting %d faults gave %d unplanted findings: %s' % (len(planted), len(extra), sorted(extra)[:5]))
    return problems


# ---------------------------------------------------------------- reporting

def load_accepted(path):
    if not path or not os.path.exists(path):
        return []
    return json.load(open(path))


def is_accepted(f, acc):
    for a in acc:
        if a.get('mission', '*') not in ('*', f['mission']):
            continue
        if a.get('kind') != f['kind'] and not (a.get('kind', '').endswith('*') and f['kind'].startswith(a['kind'][:-1])):
            continue
        if a.get('key', '*') not in ('*', f['key']):
            continue
        return a.get('reason', 'accepted')
    return None


def ticks_in(d):
    return sorted(int(re.search(r'_t(\d+)\.json$', f).group(1)) for f in glob.glob(os.path.join(d, 'world_t*.json')))


def load_pair(outdir):
    ts = sorted(set(ticks_in(os.path.join(outdir, 'ge'))) & set(ticks_in(os.path.join(outdir, 'pd'))))
    if not ts:
        raise SystemExit('%s: no tick dumped on both sides' % outdir)
    rd = lambda side, t: json.load(open(os.path.join(outdir, side, 'world_t%d.json' % t)))
    return rd('ge', ts[0]), rd('pd', ts[0]), rd('ge', ts[-1]), rd('pd', ts[-1])


# Findings about chrs and the player at the later tick come after scripts have
# run, so random commands and timing can make them differ; placement findings
# are fixed at load and are the confident ones.
BEHAVIOUR = ('chr.spawned', 'chr.pos', 'chr.rooms', 'chr.damage', 'chr.accuracyrating', 'chr.speedrating', 'chr.visionrange',
             'chr.hearingscale', 'chr.morale', 'chr.alertness', 'chr.ailist', 'chr.bits', 'player.pos',
             'player.theta', 'map.head',
             # the wide dump's held weapons are what the scripts handed out (Cradle's paired
             # gun goes to chr 4 on the cartridge and chr 5 in ours: a random pick in their list)
             'chr.held', 'chr.weaponflags', 'map.heldweapon')


def markdown(rep, accepted_count):
    lines = ['## %s' % rep.mission, '',
             'Offset (ours - GoldenEye) %s, spread %s. %d findings, %d accepted.' % (
                 [round(v, 2) for v in rep.offset], round(rep.spread, 3), len(rep.items), accepted_count), '']
    by = defaultdict(list)
    for f in rep.items:
        by[f['kind']].append(f)
    order = sorted(by, key=lambda k: (k in BEHAVIOUR, -max(f['mag'] for f in by[k]), k))
    shown_behaviour = False
    for kind in order:
        if kind in BEHAVIOUR and not shown_behaviour:
            lines += ['#### After the scripts have run (may be random or timing)', '']
            shown_behaviour = True
        fs = sorted(by[kind], key=lambda f: -f['mag'])
        lines.append('### %s (%d)' % (kind, len(fs)))
        # the same finding on many records is one line: "door N (model M) ..." alike
        groups = defaultdict(list)
        for f in fs:
            # same words with different numbers is the same finding on another record
            key = re.sub(r'-?\d+(\.\d+)?', '#', f['detail'])
            groups[key].append(f)
        shown = 0
        for pat, g in sorted(groups.items(), key=lambda kv: -max(f['mag'] for f in kv[1])):
            if shown >= 40:
                lines.append('- ... %d more' % (len(fs) - shown))
                break
            if len(g) >= 4:
                lines.append('- %d alike, e.g. %s (keys %s)' % (len(g), g[0]['detail'], ', '.join(f['key'] for f in g[:15]) + (' ...' if len(g) > 15 else '')))
                shown += len(g)
            else:
                for f in g:
                    lines.append('- %s' % f['detail'])
                    shown += 1
        lines.append('')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('outdirs', nargs='+')
    ap.add_argument('--accepted', default=os.path.join(HERE, 'accepted.json'))
    ap.add_argument('--md')
    ap.add_argument('--json')
    ap.add_argument('--selftest', action='store_true', help='run the null only')
    a = ap.parse_args()
    acc = load_accepted(a.accepted)
    md, allf, nulled = [], [], False
    for d in a.outdirs:
        ge0, pd0, ge1, pd1 = load_pair(d)
        # the null runs on every mission: each exercises different translations
        problems = selftest(ge0, ge1)
        if problems:
            print('NULL FAILED on %s - the diff cannot be trusted:' % d, *problems, sep='\n  ')
            return 2
        nulled = True
        if a.selftest:
            continue
        name = levels.MISSIONS[ge0['mission']][1] if 'mission' in ge0 else os.path.basename(d.rstrip('/'))
        rep = compare(ge0, pd0, ge1, pd1, name)
        kept, nacc = [], 0
        for f in rep.items:
            r = is_accepted(f, acc)
            if r:
                nacc += 1
            else:
                kept.append(f)
        rep.items = kept
        allf += kept
        md.append(markdown(rep, nacc))
    print('null: on each of %d missions a clean copy came out clean and every planted fault was found '
          '(five, and four more on a wide dump)' % len(a.outdirs))
    if a.selftest:
        return 0
    text = '\n'.join(md)
    if a.md:
        open(a.md, 'w').write('# World diff\n\n' + text)
    else:
        print(text)
    if a.json:
        json.dump(allf, open(a.json, 'w'), indent=1)
    return 1 if allf else 0


if __name__ == '__main__':
    sys.exit(main())

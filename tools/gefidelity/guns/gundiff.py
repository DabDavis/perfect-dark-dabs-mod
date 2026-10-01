#!/usr/bin/env python3
"""Compare GoldenEye's guns on both sides from guns/run.py's logs.

    guns/gundiff.py OUT [--repeat REPEAT] [--md OUT/report.md] [--json OUT/report.json]

OUT/{ge,pd}/gun_<item>.json are the main run; REPEAT the same scenario run
again for one gun or more (guns/sweep.py makes it). Every invocation runs both
controls before it reports anything, and refuses to report when either fails:

- **repeat**: a gun run twice on the same side must measure the same within
  the tolerances below (the run is deterministic enough to diff at all);
- **planted fault**: a copy of the oracle's metrics with the clip one larger
  must come out as exactly that one mismatch.

**The frame rule (native port only).** GoldenEye counts an automatic gun's
rate in frames (gunfire.c: `field_88C % bondwalkItemGetAutomaticFiringRate()`,
one count a frame), and the native port under PORT_LOCKSTEP runs about a tick a
frame, far faster than the console. The port fixes GoldenEye's frame at two
ticks (geguns.c's gegunsRpm(), "thirty frames a second"). So against the native
port a held automatic's cadence is compared as GoldenEye's *frames* per shot
times FRAME_TICKS against our ticks per shot.

**On the cartridge (ares, the default) there is no frame rule**: the game runs
at its own frame rate, so its ticks per shot are what a player gets and are
compared as they are; its frames per shot and mean ticks per frame are kept in
the report to show what the cartridge's frame rate was. Everything GoldenEye
times in ticks (reload and draw animations add g_ClockTimer) is compared in
ticks on both oracles.
"""
import argparse, collections, copy, json, os, statistics, sys

FRAME_TICKS = 2          # geguns.c: one GoldenEye frame is two sixtieths
# gunfire.c's automatic case: the rate is field_88C % rate, counted in frames
GE_AUTOMATIC = {7, 8, 9, 10, 11, 12, 13, 14}
# thrown from the hand: a "reload" is the next one raised
THROWN = {3, 26, 27, 28, 29}
TOL_TICKS = 4            # draw, reload, fuse
TOL_CADENCE = 1.0        # ticks per shot

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gunlist  # noqa: E402

# GoldenEye's EMPTY_GUN_FIRE_SFX and the rest are read off its sound ids; ours
# are mapped back to GoldenEye's ids when they are its samples (gunscen.py).


EMPTY_GUN_FIRE = 89      # GoldenEye's EMPTY_GUN_FIRE_SFX; ours is mapped to its ids
try:
    SFX_NAMES = {int(k): v for k, v in json.load(open(os.path.join(HERE, 'ge_sfx_names.json'))).items()}
except OSError:
    SFX_NAMES = {}


def sfxname(i):
    try:
        return SFX_NAMES.get(int(i), str(i)).replace('_SFX', '')
    except (TypeError, ValueError):
        return str(i)


def family(i):
    """A random pick's family: HIT_BULLET_STONE1/2 -> HIT_BULLET_STONE, RICO_* -> RICO."""
    n = sfxname(i)
    if n.startswith('RICO'):
        return 'RICO'
    return n.rstrip('0123456789').rstrip('_')


def source(callers):
    """Who asked for a sound, from the callers gunscen.py logged with it -
    so that sounds GoldenEye picks at random (a hit's surface, a ricochet) are
    compared as families and the gun's own as ids."""
    s = ' '.join(callers)
    if 'casing' in s:
        return 'casing'
    if 'Hit' in s or 'hits' in s or 'explosion' in s or 'shotCalculate' in s:
        return 'impact'
    if 'gunTick' in s or 'bgun' in s or 'gunUpdate' in s or 'geguns' in s or 'gunfire' in s:
        return 'gun'
    return 'other'


def frame_of(fr, t):
    """The poller's frame index at tick t (events are logged by tick)."""
    for k, f in enumerate(fr):
        if f[0] >= t:
            return k
    return len(fr)


def load(path):
    return json.load(open(path))


def uses(d):
    """Each use of the gun inside the fire windows: a clip going down, or
    (thrown things) the reserve going down with no clip going up. The draw is
    left out - asking for the gun already held moves the clip about.
    [(t, frame index, amount)]"""
    out = []
    fr = d['frames']
    tap0 = d['schedule']['tap'][0]
    for k in range(1, len(fr)):
        if fr[k][0] < tap0:
            continue
        a, b = fr[k - 1], fr[k]
        n = 0
        for c in (2, 5):                      # clip right, clip left
            if b[c] < a[c] and b[1 if c == 2 else 4] == a[1 if c == 2 else 4]:
                n += a[c] - b[c]
        refill = (b[2] > a[2]) or (b[5] > a[5])
        if n == 0 and not refill and 0 <= b[7] < a[7]:
            n = a[7] - b[7]
        if n:
            out.append((b[0], k, n))
    return out


def metrics(d):
    sch = d['schedule']
    tap0, tap1 = sch['tap']
    h0, h1 = sch['hold_at'], sch['hold_at'] + d['hold']
    fr = d['frames']
    target = d['item'] if d['side'] == 'ge' else d['weapon']
    m = {'reserve_max': d.get('reserve_max'), 'dual_allguns': d.get('dual_allguns'),
         'automatic': d['item'] in GE_AUTOMATIC}
    if d['side'] == 'ge':
        m['oracle'] = d.get('oracle', 'port')
        m['ticks_per_frame'] = d.get('ticks_per_frame')
    clipless = d.get('ammotype', 0) <= 0          # the laser, the hunting knife: GoldenEye still counts its magazine down
    thrown = d['item'] in THROWN
    # the oracle's frame step: a tick timing is only as fine as its frames
    ts = [f[0] for f in fr]
    m['frame_step'] = max((b - a for a, b in zip(ts, ts[1:])), default=1)
    drawn = [f[0] for f in fr if f[1] == target and f[3] == 0]
    m['draw_ticks'] = drawn[0] if drawn else None
    # the raise alone, from the hand taking the new gun to idle: the lowering
    # before it is the previous gun's, and the two sides' groups differ
    took = [f[0] for f in fr if f[1] == target]
    m['raise_ticks'] = (drawn[0] - took[0]) if (drawn and took and took[0] > 1) else None
    pre = [f for f in fr if f[0] < tap0]
    m['clip_drawn'] = (pre[-1][2] if pre else None) if not clipless else None
    m['reserve_drawn'] = pre[-1][7] if pre else None
    u = uses(d)
    m['clip_uses'] = sum(x[2] for x in u)
    # what the gun did, from its own events (a laser or a knife uses no clip)
    fired = [(e[0], frame_of(fr, e[0]), e[2] if isinstance(e[2], int) and e[2] > 0 else 1)
             for e in d['events'] if e[1] in ('shot', 'throw', 'launch') and e[0] >= tap0]
    # our shoot function runs every tick an automatic is attacking, so a clip,
    # where there is one, is the count; the events are for a laser or a knife
    if fired and not u:
        u = fired
    m['throws'] = sum(1 for e in d['events'] if e[1] == 'throw' and e[0] >= tap0)
    m['launches'] = sum(1 for e in d['events'] if e[1] == 'launch' and e[0] >= tap0)
    tap = [x for x in u if tap0 <= x[0] < h0]
    hold = [x for x in u if h0 <= x[0] < h1 + 2]
    m['tap_shots'] = sum(x[2] for x in tap)
    m['per_use'] = max((x[2] for x in u), default=None)
    empt = [] if (clipless or thrown) else [f[0] for f in fr if h0 <= f[0] < h1 and f[2] == 0 and f[1] == target]
    m['empties_in_hold'] = bool(empt)
    t_empty = empt[0] if empt else None
    before = [x for x in hold if t_empty is None or x[0] <= t_empty]
    m['hold_shots'] = sum(x[2] for x in hold)
    if len(before) >= 3:
        m['ticks_per_shot'] = statistics.median(b[0] - a[0] for a, b in zip(before, before[1:]))
        m['frames_per_shot'] = statistics.median(b[1] - a[1] for a, b in zip(before, before[1:]))
    else:
        m['ticks_per_shot'] = m['frames_per_shot'] = None
    # the reload: while held after the clip ran dry, or at the release
    if t_empty is not None:
        refill = [f[0] for f in fr if f[0] > t_empty and f[2] > 0]
        m['reload_on'] = 'none' if not refill else ('held' if refill[0] < h1 else 'release')
        start = t_empty if m['reload_on'] == 'held' else h1
        if refill:
            m['reload_refill'] = refill[0] - start
            idle = [f[0] for f in fr if f[0] > refill[0] and f[3] == 0]
            m['reload_idle'] = (idle[0] - start) if idle else None
    else:
        m['reload_on'] = None
    if clipless or thrown:
        m['empties_in_hold'] = m['reload_on'] = None
    ev = d['events']
    nuse = max(1, sum(x[2] for x in u))
    m['casings_per_use'] = round(sum(1 for e in ev if e[1] == 'casing') / nuse, 1)
    m['impacts_per_use'] = round(sum(1 for e in ev if e[1] == 'impact') / nuse, 1)
    expl = sorted(e[0] for e in ev if e[1] == 'expl' and e[2] not in (1,))
    m['explosions'] = len(expl)
    rel = sorted(e[0] for e in ev if e[1] in ('throw', 'launch') and e[0] >= tap0)
    first = rel[0] if rel else (tap[0][0] if tap else None)
    # from the press to the thing leaving the hand
    m['release_latency'] = (rel[0] - tap0) if rel and rel[0] < h0 else None
    # from the moment it left the hand: a cooked grenade or a rocket's flight
    m['fuse'] = (expl[0] - first) if (expl and first is not None) else None
    snds = [(e[0], e[2], source(e[3] if len(e) > 3 else [])) for e in ev if e[1] == 'snd' and e[2] not in (0, None)]
    for name, lo, hi in (('draw', 0, tap0), ('tap', tap0, h0), ('hold', h0, h1), ('after', h1, 10 ** 9)):
        c = collections.Counter(i for t, i, src in snds if src in ('gun', 'other') and lo <= t < hi)
        m['snd_' + name] = dict(sorted(((str(k), v) for k, v in c.items())))
        m['fam_' + name] = sorted({family(k) for k in c})
    m['snd_casing'] = sorted({str(i) for t, i, src in snds if src == 'casing'})
    m['snd_impact_families'] = sorted({family(i) for t, i, src in snds if src == 'impact'})
    m['snd_impact'] = dict(collections.Counter(str(i) for t, i, src in snds if src == 'impact'))
    clicks = [t for t, i, src in snds if src == 'gun' and i == EMPTY_GUN_FIRE and h0 <= t < h1]
    m['dry_click_interval'] = statistics.median(b - a for a, b in zip(clicks, clicks[1:])) if len(clicks) >= 3 else None
    return m


def cadence(ge):
    """GoldenEye's held cadence in our units: on the cartridge its ticks as
    they are; on the native port frames x FRAME_TICKS for its automatics, ticks
    for the rest (their refire is animation, in ticks)."""
    if ge.get('oracle') == 'ares':
        return ge.get('ticks_per_shot')
    if ge.get('automatic'):
        return ge['frames_per_shot'] * FRAME_TICKS if ge.get('frames_per_shot') is not None else None
    return ge.get('ticks_per_shot')


def cadence_tol(ge):
    """How far apart two cadences may be: a tick, plus the oracle's frame step
    where its shots fall on frames that are several ticks long (the cartridge,
    and the native port's non-automatics)."""
    if ge.get('automatic') and ge.get('oracle') != 'ares':
        return TOL_CADENCE
    return TOL_CADENCE + ge.get('frame_step', 1)


def tolerance(quantity, ge):
    """The tolerance compare() allows for a quantity (0: must be equal), so a
    mismatch's size beyond it can be told (guns/gatesweep.py's magnitude)."""
    if quantity in ('raise_ticks', 'reload_refill', 'reload_idle', 'fuse', 'release_latency', 'dry_click_interval'):
        return TOL_TICKS + ge.get('frame_step', 1)
    if quantity == 'impacts_per_use':
        return 0.5
    if quantity == 'cadence':
        return cadence_tol(ge)
    return 0


def compare(ge, pd):
    """[(quantity, ge value, our value, note)]"""
    out = []

    def eq(k, note=''):
        if ge.get(k) != pd.get(k):
            out.append((k, ge.get(k), pd.get(k), note))

    def near(k, tol, note=''):
        a, b = ge.get(k), pd.get(k)
        if (a is None) != (b is None) or (a is not None and abs(a - b) > tol):
            out.append((k, a, b, note))

    for k in ('reserve_max', 'dual_allguns', 'clip_drawn', 'per_use', 'empties_in_hold', 'reload_on',
              'casings_per_use', 'explosions', 'throws', 'launches'):
        eq(k)
    if not ge.get('automatic'):
        eq('tap_shots')     # an automatic's tap is frames long on GoldenEye's side
    near('impacts_per_use', 0.5)
    slack = TOL_TICKS + ge.get('frame_step', 1)
    for k in ('raise_ticks', 'reload_refill', 'reload_idle', 'fuse', 'release_latency'):
        near(k, slack, 'ticks, %d allowed' % slack)
    a, b = cadence(ge), pd.get('ticks_per_shot')
    tol = cadence_tol(ge)
    if (a is None) != (b is None) or (a is not None and abs(a - b) > tol):
        if ge.get('oracle') == 'ares':
            note = 'ticks on the cartridge (%s frames a shot at %s ticks a frame)' % (
                ge.get('frames_per_shot'), ge.get('ticks_per_frame'))
        elif ge.get('automatic'):
            note = 'GoldenEye frames x %d (raw %s ticks)' % (FRAME_TICKS, ge.get('ticks_per_shot'))
        else:
            note = 'ticks'
        out.append(('cadence', a, b, note))
    near('dry_click_interval', slack, 'ticks between dry clicks held empty')
    for ph in ('draw', 'tap', 'hold', 'after'):
        # by family (KNIFE_THROW1-3 are one random pick), the ids shown
        a, b = ge.get('snd_' + ph, {}), pd.get('snd_' + ph, {})
        fa, fb = set(ge.get('fam_' + ph, [])), set(pd.get('fam_' + ph, []))
        if fa != fb:
            out.append(('sounds_' + ph, {k: v for k, v in a.items() if family(k) not in fb},
                        {k: v for k, v in b.items() if family(k) not in fa},
                        'the gun\'s own sounds, families heard on one side only (counts)'))
    if ge.get('snd_casing') != pd.get('snd_casing'):
        out.append(('sounds_casing', ge.get('snd_casing'), pd.get('snd_casing'), 'casing landing'))
    if ge.get('snd_impact_families') and pd.get('snd_impact_families') and \
            set(ge['snd_impact_families']) != set(pd['snd_impact_families']):
        out.append(('sounds_impact', ge['snd_impact_families'], pd['snd_impact_families'],
                    'families of the hit sounds (random picks within a family are not compared)'))
    return out


def controls(main, repeat):
    problems = []
    for side in ('ge', 'pd'):
        for f in sorted(os.listdir(os.path.join(repeat, side))) if os.path.isdir(os.path.join(repeat, side)) else []:
            if not f.startswith('gun_'):
                continue
            a = metrics(load(os.path.join(main, side, f)))
            b = metrics(load(os.path.join(repeat, side, f)))
            # same side twice: compare like with like (no frame rule)
            diffs = [x for x in compare_same(a, b)]
            if diffs:
                problems.append('repeat %s %s disagrees with itself: %s' % (side, f, diffs[:3]))
    if not problems and not any(os.path.isdir(os.path.join(repeat, s)) for s in ('ge', 'pd')):
        problems.append('no repeat run in %s' % repeat)
    # planted fault
    files = sorted(f for f in os.listdir(os.path.join(main, 'ge')) if f.startswith('gun_'))
    if files:
        g = metrics(load(os.path.join(main, 'ge', files[0])))
        p = copy.deepcopy(g)
        p['clip_drawn'] = (p['clip_drawn'] or 0) + 1
        got = [x[0] for x in compare_same(g, p)]
        if got != ['clip_drawn']:
            problems.append('planted clip fault came out as %s' % got)
    return problems


def compare_same(a, b):
    """One side against itself: the tolerances compare() allows, plus the
    oracle's frame step (its frames are host-paced under lockstep)."""
    out = []
    step = max(a.get('frame_step', 1), b.get('frame_step', 1))
    for k in a:
        if k in ('frame_step', 'snd_impact', 'ticks_per_frame', 'oracle') or k.startswith('snd_') or (k == 'frames_per_shot' and not a.get('automatic')):
            continue
        x, y = a[k], b.get(k)
        if isinstance(x, (int, float)) and isinstance(y, (int, float)) and not isinstance(x, bool):
            tol = TOL_CADENCE + (0 if k == 'frames_per_shot' else step) if k in ('ticks_per_shot', 'frames_per_shot') else (
                TOL_TICKS + step if k in ('draw_ticks', 'raise_ticks', 'reload_refill', 'reload_idle', 'fuse', 'dry_click_interval',
                                          'release_latency') else (
                    0.5 if k == 'impacts_per_use' else 0))
            if abs(x - y) > tol:
                out.append((k, x, y))
        elif k.startswith('snd_'):
            if set(x) != set(y or {}):
                out.append((k, x, y))
        elif x != y:
            out.append((k, x, y))
    return out


def fmt(v, names=False):
    if isinstance(v, dict):
        return ', '.join('%s x%d' % (sfxname(k) if names else k, n) for k, n in v.items()) or '-'
    if isinstance(v, list) and names:
        return ', '.join(sfxname(k) if str(k).isdigit() else str(k) for k in v) or '-'
    if isinstance(v, float):
        return ('%.1f' % v).rstrip('0').rstrip('.')
    return '-' if v is None else str(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('--repeat')
    ap.add_argument('--md')
    ap.add_argument('--json')
    a = ap.parse_args()
    repeat = a.repeat or os.path.join(a.out, 'repeat')
    problems = controls(a.out, repeat)
    if problems:
        print('CONTROLS FAILED - not reporting:', *problems, sep='\n  ')
        return 2
    print('controls: repeat run agrees with itself, planted clip fault found')
    rows, table = [], []
    for item, key, weapon in gunlist.GUNS:
        fg = os.path.join(a.out, 'ge', 'gun_%d.json' % item)
        fp = os.path.join(a.out, 'pd', 'gun_%d.json' % item)
        if not (os.path.exists(fg) and os.path.exists(fp)):
            table.append({'gun': key, 'missing': [s for s, f in (('ge', fg), ('pd', fp)) if not os.path.exists(f)]})
            continue
        g, p = metrics(load(fg)), metrics(load(fp))
        diffs = compare(g, p)
        table.append({'gun': key, 'item': item, 'weapon': weapon, 'ge': g, 'pd': p,
                      'mismatches': [{'quantity': q, 'ge': x, 'pd': y, 'note': n} for q, x, y, n in diffs]})
    cols = ['clip_drawn', 'reserve_max', 'tap_shots', 'cadence', 'hold_shots', 'empties_in_hold', 'reload_on',
            'reload_refill', 'reload_idle', 'dry_click_interval', 'raise_ticks', 'draw_ticks', 'casings_per_use', 'impacts_per_use',
            'throws', 'launches', 'release_latency', 'explosions', 'fuse', 'dual_allguns']
    oracles = sorted({r['ge'].get('oracle', 'port') for r in table if 'ge' in r})
    md = ['# Gun diff: GoldenEye (%s) / ours' % ', '.join('the cartridge in ares' if o == 'ares' else 'the native port'
                                                         for o in oracles), '',
          'Each cell is `GoldenEye / ours`; **bold** cells disagree. Cadence is ticks per shot held: on the cartridge '
          'its own ticks, on the native port GoldenEye\'s frames x %d (see gundiff.py, "the frame rule"). Sound '
          'mismatches are listed under the table.' % FRAME_TICKS, '',
          '| gun | ' + ' | '.join(cols) + ' |', '|' + '---|' * (len(cols) + 1)]
    for r in table:
        if 'missing' in r:
            md.append('| %s | not run on %s |' % (r['gun'], ', '.join(r['missing'])) + ' |' * (len(cols) - 1))
            continue
        bad = {m['quantity'] for m in r['mismatches']}
        cells = []
        for c in cols:
            if c == 'cadence':
                gv, pv = cadence(r['ge']), r['pd'].get('ticks_per_shot')
            else:
                gv, pv = r['ge'].get(c), r['pd'].get(c)
            cell = '%s / %s' % (fmt(gv), fmt(pv))
            cells.append('**%s**' % cell if c in bad else cell)
        md.append('| %s | %s |' % (r['gun'], ' | '.join(cells)))
    md += ['', '## Sounds that differ', '',
           'Sound ids are GoldenEye\'s (ours mapped back through gesfx.c). "gun" sounds are the gun logic\'s own; '
           'hits are compared as families because both games pick within a family at random.', '']
    for r in table:
        for m in r.get('mismatches', []):
            if m['quantity'].startswith('sounds_'):
                md.append('- **%s** %s: GoldenEye only {%s}; ours only {%s}' % (
                    r['gun'], m['quantity'][7:], fmt(m['ge'], True), fmt(m['pd'], True)))
    text = '\n'.join(md) + '\n'
    open(a.md or os.path.join(a.out, 'report.md'), 'w').write(text)
    json.dump(table, open(a.json or os.path.join(a.out, 'report.json'), 'w'), indent=1)
    print(text)
    return 1 if any(r.get('mismatches') or r.get('missing') for r in table) else 0


if __name__ == '__main__':
    sys.exit(main())

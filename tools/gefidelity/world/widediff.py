"""The wide world dump's comparison (dump.py with GF_WIDE=1): what each setup
record decides beyond its placement, and the tile graph.

worlddiff.compare() calls compare() when both sides carry w['wide'], and its
null calls as_ours() and plant(); nothing here runs on a dump without it, so
reports made before the wide dump keep their kinds.

TRANSLATE - what the conversion changes on purpose, each from the code that does it:

- Doors (geconvert.c doorRecord()): maxFrac, perimFrac, maxSpeed, the flag
  word, keyflags and autoCloseFrames are copied; accel and decel are written x1000
  into the file, and our load divides by 65536000 where GoldenEye's divides by
  65536, so at run time the two are equal (Control's doors: 0.0002 both). Our
  soundtype is GoldenEye's doorOpenSound's low byte (`out[0xc6] = raw[0xa7]`).
  Our xludist is GoldenEye's TintDist and our opadist the s32 GoldenEye reads
  over CullDist, soundType and fadeTime60 (DOOR_OPADIST()), each clamped to an
  s16. The sibling is a record index on both sides. Our port sets
  DOORFLAG_0020 and DOORFLAG_0080 on doors as it builds them (propobj.c:20742,
  20894: a cached perimeter, the door types that slide) - masked from ours.
- An object's flag word: copied (baseRecord()), but for a door its top byte is
  remapped - GoldenEye's 0x80 is our 0x40, its 0x08 our 0x80 (doorFlags()).
- Guard records (guardRecord()): GoldenEye's bitflags & 0x0b are our spawn
  flags; its 0x04 (clone on heard gunfire) is our flags' 0x20000000
  (CHRFLAG0_CAN_HEARSPAWN). The converter writes GoldenEye's body and head as
  they are, but our port writes its own rows over them at the load
  (gexplus.c gexPlusMissionChr()), so they are checked as mappings. AI lists move as worlddiff.ge_ailist_to_pd(), pads as
  ge_pad_to_pd(). GoldenEye's "health" and "reaction time" are our hearscale
  and viewdist.
- Pickups' weapon numbers, monitors' pictures: through a table each, so they
  are checked as mappings (one GoldenEye value, one of ours, every time).
- Ammo crates: GoldenEye's 13 slots against our 19 - the quantities carried,
  compared as a sorted list.
- Tiles (geconvert.c stanRead()/writeStan()): one of ours for each of
  GoldenEye's, in its order; a point at GoldenEye's place times the level scale
  plus the level's offset, rounded to a unit; across the edge to the next
  point, the tile index GoldenEye's link names, or -1/-2 for none, with 0x4000
  where the conversion also raised a wall on a link that climbs.
"""
import math
from collections import Counter

DOOR_PD_RUNTIME = 0x0020 | 0x0080
TOL_F = 1e-3
TOL_TILE = 1.5


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def door_top_byte(f):
    top = f >> 24
    nw = top & ~0xc8 & 0xff
    if top & 0x80:
        nw |= 0x40
    if top & 0x08:
        nw |= 0x80
    return (nw << 24) | (f & 0xffffff)


def _feq(a, b):
    if a != a or b != b:          # NaN: the same only when both are
        return a != a and b != b
    return abs(a - b) <= TOL_F * max(1.0, abs(a), abs(b))


def _quantities(slots):
    return sorted(q for _, q in slots if q)


def as_ours(gw, off, numpads, ge_pad_to_pd, ge_ailist_to_pd):
    """GoldenEye's wide dump as a perfect conversion would hold it (the null's clean copy)."""
    props = {}
    for k, w in gw['props'].items():
        o = dict(w)
        if 'doorflags' in w:
            o = {'doorflags': w['doorflags'], 'doortype': w['doortype'], 'keyflags': w['keyflags'],
                 'autoclose': w['autoclose'], 'maxfrac': w['maxfrac'], 'perimfrac': w['perimfrac'],
                 'accel': w['accel'], 'decel': w['decel'], 'maxspeed': w['maxspeed'],
                 'soundtype': w['opensound'] & 0xff, 'fadetime60': w['fadetime60'],
                 'xludist': clamp16(w['tintdist']), 'opadist': clamp16(w['culldist_word']),
                 'portal': w['portal'], 'sibling': w['sibling']}
        elif 'ammo' in w:
            o = {'ammo': list(w['ammo']) + [[0, 0]] * 6}
        elif 'tintdist' in w:
            o = {'xludist': clamp16(w['tintdist']), 'opadist': clamp16(w['culldist']), 'portal': w['portal']}
        elif 'targetpad' in w:
            o = dict(w, targetpad=ge_pad_to_pd(w['targetpad'], numpads))
        elif 'bitflags' in w:
            o = {'chrnum': w['chrnum'], 'pad': ge_pad_to_pd(w['pad'], numpads), 'body': w['body'] & 0xff,
                 'head': 0xff if w['head'] < 0 else w['head'] & 0xff, 'ailist': ge_ailist_to_pd(w['ailist']),
                 'preset': ge_pad_to_pd(w['preset'], numpads) if w['preset'] != 0xffff else 0xffff,
                 'chrpreset': w['chrpreset'], 'hearscale': w['hearscale'], 'viewdist': w['viewdist'],
                 'spawnflags': w['bitflags'] & 0x0b, 'flags': 0x20000000 if w['bitflags'] & 0x04 else 0, 'flags2': 0}
        elif 'grenadeprob' in w:
            o = {'chrnum': w['chrnum'], 'grenadeprob': w['grenadeprob'] & 0xff}
        props[k] = o
    tiles = None
    if gw.get('tiles'):
        tiles = {'tiles': [[room, special, [[int(round(x + off[0])), int(round(y + off[1])), int(round(z + off[2])),
                                             n if n >= 0 else -1] for x, y, z, n in pts]]
                           for room, special, pts in gw['tiles']['tiles']]}
    return {'props': props, 'tiles': tiles}


def plant(pw):
    """Three faults in a clean copy of ours: a door flag, a tile link, a guard
    record's field. Returns {kind: key} of what the comparison must find."""
    planted = {}
    for k, w in sorted(pw['props'].items(), key=lambda kv: int(kv[0])):
        if 'doorflags' in w and 'door.doorflags' not in planted:
            w['doorflags'] ^= 0x0100
            planted['door.doorflags'] = k
        if 'viewdist' in w and 'guard.viewdist' not in planted:
            w['viewdist'] += 7
            planted['guard.viewdist'] = k
    if pw.get('tiles'):
        for i, (room, special, pts) in enumerate(pw['tiles']['tiles']):
            hit = next((j for j, p in enumerate(pts) if p[3] >= 0), None)
            if hit is not None:
                pts[hit][3] = -1
                planted['tile.link'] = '%d:%d' % (i, hit)
                break
    return planted


def compare_chrweapons(rep, ge1, pd1, mapping_check, bitname_obj):
    """The weapons each chr holds after its scripts have run, with the weapon
    prop's flag word: where an AI list's TRYGiveMeItem flags land (Surface's
    paired Klobbs). Weapon numbers as a mapping; flag words bit for bit (a
    weapon's flag word is copied by the conversion, baseRecord())."""
    gc, pc = ge1.get('wide_chrweapons'), pd1.get('wide_chrweapons')
    if gc is None or pc is None:
        return
    held, bits = [], Counter()
    for n in sorted(set(gc) & set(pc), key=int):
        for h, (a, b) in enumerate(zip(gc[n], pc[n])):
            if a is None or b is None:
                if (a is None) != (b is None):
                    rep.add('chr.held', '%s:%d' % (n, h), 'chr %s hand %d holds %s in GoldenEye, %s in ours' % (
                        n, h, 'nothing' if a is None else 'weapon %d' % a[0], 'nothing' if b is None else 'weapon %d' % b[0]), 1)
                continue
            held.append(('%s:%d' % (n, h), a[0], b[0]))
            for bit in range(32):
                m = 1 << bit
                if (a[1] & m) != (b[1] & m):
                    bits[(m, 'GoldenEye only' if a[1] & m else 'ours only')] += 1
                    rep.add('chr.weaponflags', '%s:%d:0x%x' % (n, h, m), 'chr %s hand %d: held weapon flag 0x%x '
                            '(GoldenEye / ours: %s) set in %s at tick %d' % (
                                n, h, m, bitname_obj('flags', m), 'GoldenEye only' if a[1] & m else 'ours only',
                                ge1['tick']), 1)
    mapping_check(rep, 'map.heldweapon', held, 'held weapon')


def compare(rep, ge0, pd0, off, numpads, ge_pad_to_pd, ge_ailist_to_pd, mapping_check, tname, bitname_obj):
    gw, pw = ge0['wide'], pd0['wide']
    gp, pp = gw['props'], pw['props']
    gtypes = {str(r['i']): r for r in ge0['props']}
    ptypes = {str(r['i']): r for r in pd0['props']}
    pickups, images, gbodies, gheads = [], [], [], []
    for k in sorted(set(gp) & set(pp), key=int):
        a, b = gp[k], pp[k]
        t = gtypes.get(k, {}).get('type')
        what = '%s %s' % (tname(t) if t is not None else 'record', k)
        if 'doorflags' in a:
            df_ours = b['doorflags'] & ~DOOR_PD_RUNTIME
            if a['doorflags'] != df_ours:
                d = a['doorflags'] ^ df_ours
                rep.add('door.doorflags', k, '%s door flags GoldenEye 0x%04x, ours 0x%04x (bits 0x%04x; ours less the '
                        '0x%04x our port sets itself)' % (what, a['doorflags'], df_ours, d, DOOR_PD_RUNTIME),
                        bin(d).count('1'))
            # not fadetime60: GoldenEye's is the low byte of the word it reads whole as its
            # opaque distance (DOOR_OPADIST(0, 2, 88) = 600), which our opadist carries
            for f in ('doortype', 'keyflags', 'autoclose', 'sibling', 'portal'):
                if a[f] != b[f]:
                    rep.add('door.' + f, k, '%s %s GoldenEye %s, ours %s' % (what, f, a[f], b[f]),
                            1 if f in ('sibling', 'portal', 'doortype') else abs(a[f] - b[f]))
            for f in ('maxfrac', 'perimfrac', 'accel', 'decel', 'maxspeed'):
                if not _feq(a[f], b[f]):
                    rep.add('door.' + f, k, '%s %s GoldenEye %s, ours %s' % (what, f, a[f], b[f]),
                            abs(a[f] - b[f]) / max(abs(a[f]), 1e-6))
            if (a['opensound'] & 0xff) != b['soundtype']:
                rep.add('door.sound', k, '%s sound GoldenEye %d (doorOpenSound), ours soundtype %d' % (
                    what, a['opensound'], b['soundtype']), 1)
            for f, ge in (('xludist', clamp16(a['tintdist'])), ('opadist', clamp16(a['culldist_word']))):
                if ge != b[f]:
                    rep.add('door.' + f, k, '%s %s GoldenEye %d, ours %d' % (what, f, ge, b[f]), abs(ge - b[f]))
        elif 'weaponnum' in a:
            pickups.append((k, a['weaponnum'], b['weaponnum']))
            if a['dual'] != b['dual']:
                rep.add('pickup.dual', k, '%s paired with record %s in GoldenEye, %s in ours' % (what, a['dual'], b['dual']), 1)
        elif 'ammo' in a:
            qa, qb = _quantities(a['ammo']), _quantities(b['ammo'])
            if qa != qb:
                rep.add('ammo.contents', k, '%s holds %s in GoldenEye, %s in ours (quantities)' % (what, qa, qb),
                        abs(sum(qa) - sum(qb)) or 1)
        elif 'keyflags' in a:
            if a['keyflags'] != b['keyflags']:
                rep.add('key.keyflags', k, '%s key flags GoldenEye 0x%x, ours 0x%x' % (what, a['keyflags'], b['keyflags']), 1)
        elif 'imagenum' in a:
            images.append((k, a['imagenum'], b['imagenum']))
            for f in ('owneroffset', 'ownerpart'):
                if a[f] != b[f]:
                    rep.add('monitor.' + f, k, '%s %s GoldenEye %s, ours %s' % (what, f, a[f], b[f]), 1)
        elif 'imagenums' in a:
            for j in range(4):
                images.append(('%s.%d' % (k, j), a['imagenums'][j], b['imagenums'][j]))
        elif 'targetpad' in a:
            if ge_pad_to_pd(a['targetpad'], numpads) != b['targetpad']:
                rep.add('autogun.targetpad', k, '%s faces pad %s in GoldenEye, %s in ours' % (what, a['targetpad'], b['targetpad']), 1)
            for f in ('maxspeed', 'aimdist', 'ymaxleft', 'ymaxright'):
                if not _feq(a[f], b[f]):
                    rep.add('autogun.' + f, k, '%s %s GoldenEye %s, ours %s' % (what, f, a[f], b[f]),
                            abs(a[f] - b[f]) / max(abs(a[f]), 1e-6))
        elif 'tintdist' in a:
            for f, ge in (('xludist', clamp16(a['tintdist'])), ('opadist', clamp16(a['culldist']))):
                if ge != b[f]:
                    rep.add('glass.' + f, k, '%s %s GoldenEye %d, ours %d' % (what, f, ge, b[f]), abs(ge - b[f]))
            if a['portal'] != b['portal']:
                rep.add('glass.portal', k, '%s portal GoldenEye %d, ours %d' % (what, a['portal'], b['portal']), 1)
        elif 'initialamount' in a:
            for f in ('initialamount', 'amount'):
                if not _feq(a[f], b[f]):
                    rep.add('armour.' + f, k, '%s %s GoldenEye %s, ours %s' % (what, f, a[f], b[f]), abs(a[f] - b[f]))
        elif 'bitflags' in a:
            # body and head: our port writes its own body/head rows into the record
            # at the load (gexplus.c gexPlusMissionChr()), so they are mappings
            # a body worn with a head its record names takes a row of its own
            # (worlddiff.body_key(), gexplus.c's geRomBodyRow())
            gbodies.append((k, '%d+head%d' % (a['body'], a['head']) if a['head'] >= 0 else a['body'], b['body']))
            gheads.append((k, a['head'], b['head']))
            want = {'chrnum': a['chrnum'], 'pad': ge_pad_to_pd(a['pad'], numpads), 'ailist': ge_ailist_to_pd(a['ailist']),
                    'preset': ge_pad_to_pd(a['preset'], numpads) if a['preset'] != 0xffff else 0xffff,
                    'chrpreset': a['chrpreset'], 'hearscale': a['hearscale'], 'viewdist': a['viewdist'],
                    'spawnflags': a['bitflags'] & 0x0b}
            for f, v in want.items():
                if v != b[f]:
                    rep.add('guard.' + f, k, 'guard record %s (chr %d) %s: GoldenEye %s means ours %s, ours has %s' % (
                        k, a['chrnum'], f, a[f] if f in a else '-', v, b[f]), 1 if f not in ('hearscale', 'viewdist') else abs(v - b[f]))
            if bool(a['bitflags'] & 0x04) != bool(b['flags'] & 0x20000000):
                rep.add('guard.clone', k, 'guard record %s (chr %d): GoldenEye clone-on-gunfire %s, ours CHRFLAG0_CAN_HEARSPAWN %s' % (
                    k, a['chrnum'], bool(a['bitflags'] & 0x04), bool(b['flags'] & 0x20000000)), 1)
        elif 'grenadeprob' in a:
            if a['chrnum'] != b['chrnum'] or (a['grenadeprob'] & 0xff) != b['grenadeprob']:
                rep.add('guard.grenadeprob', k, 'chr %d grenade probability GoldenEye %d, ours %d (chr %d)' % (
                    a['chrnum'], a['grenadeprob'] & 0xff, b['grenadeprob'], b['chrnum']), 1)
    for k in sorted(set(gp) - set(pp), key=int):
        rep.add('wide.missing', k, 'record %s has GoldenEye fields (%s) and no counterpart in ours' % (k, ', '.join(sorted(gp[k]))[:80]), 1)
    mapping_check(rep, 'map.guardbody', gbodies, 'guard record body')
    mapping_check(rep, 'map.guardhead', gheads, 'guard record head')
    mapping_check(rep, 'map.pickup', pickups, 'weapon pickup')
    mapping_check(rep, 'map.image', images, 'monitor picture')

    # every object's flag words, bit by bit through the converter's map, with both games' names
    bits = Counter()
    for k in sorted(set(gtypes) & set(ptypes), key=int):
        a, b = gtypes[k], ptypes[k]
        if 'flags' not in a or 'flags' not in b:
            continue
        for field in ('flags', 'flags2'):
            ga = a[field] if field == 'flags2' or a['type'] != 1 else door_top_byte(a[field])
            pb = b[field]
            for bit in range(32):
                m = 1 << bit
                if (ga & m) != (pb & m):
                    bits[(field, m, 'GoldenEye only' if ga & m else 'ours only', tname(a['type']))] += 1
    for (field, m, side, ty), n in sorted(bits.items()):
        rep.add('obj.flagbits', '%s:0x%x:%s' % (field, m, ty), '%s bit 0x%x (GoldenEye / ours: %s) set in %s on %d %s records' % (
            field, m, bitname_obj(field, m), side, n, ty), n)

    # the tile graph
    gt, pt = gw.get('tiles'), pw.get('tiles')
    if gt and pt:
        ga, pa = gt['tiles'], pt['tiles']
        if len(ga) != len(pa):
            rep.add('tile.count', '-', 'GoldenEye %d tiles, ours %d' % (len(ga), len(pa)), abs(len(ga) - len(pa)))
        far = Counter()
        for i in range(min(len(ga), len(pa))):
            (gr, gs, gpts), (pr, ps, ppts) = ga[i], pa[i]
            if gr != pr:
                rep.add('tile.room', str(i), 'tile %d: GoldenEye room %d, ours %d' % (i, gr, pr), 1)
            if gs != ps:
                rep.add('tile.special', str(i), 'tile %d: GoldenEye special %d, ours %d' % (i, gs, ps), 1)
            if len(gpts) != len(ppts):
                rep.add('tile.shape', str(i), 'tile %d: GoldenEye %d points, ours %d' % (i, len(gpts), len(ppts)), 1)
                continue
            for j, (g, p) in enumerate(zip(gpts, ppts)):
                d = math.dist([g[0] + off[0], g[1] + off[1], g[2] + off[2]], p[:3])
                if d > TOL_TILE:
                    far[i] = max(far[i], d)
                n, across = g[3], p[3]
                ours = (across & ~0x4000) if across >= 0 else -1
                if n != ours:
                    rep.add('tile.link', '%d:%d' % (i, j), 'tile %d edge %d: GoldenEye %s, ours %s' % (
                        i, j, 'links tile %d' % n if n >= 0 else 'unlinked',
                        'links tile %d' % ours if ours >= 0 else 'unlinked (%s)' % ('wall' if across == -1 else 'no wall')), 1)
        for i, d in far.items():
            rep.add('tile.point', str(i), 'tile %d: a point %.1f units off GoldenEye\'s' % (i, d), d)

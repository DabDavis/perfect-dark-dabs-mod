"""The wide world dump's half on our side: the same records and the tile graph
as wide_ares.py reads from the cartridge, from our port under gdb (gdbpd.py's
process). world/dump.py adds it as w['wide'] when GF_WIDE=1.

Values are ours as the port holds them; world/widediff.py translates
GoldenEye's into ours before comparing.
"""
import struct

import gdb

import gdbpd as P


def _r(v):
    return round(float(v), 4)


def _records():
    out = []
    p = P.ev('(unsigned int *)g_StageSetup.props')
    objptr = gdb.lookup_type('struct defaultobj').pointer()
    i = 0
    while i < 5000:
        t = int(p.cast(objptr).dereference()['type'])
        if t == P.OBJTYPE_END:
            break
        out.append((i, t, p))
        p = p + int(P.ev('setupGetCmdLength((u32 *)%d)' % int(p)))
        i += 1
    return out


def _as(p, typ):
    return p.cast(gdb.lookup_type(typ).pointer()).dereference()


def props():
    recs = _records()
    index_of = {int(p): i for i, t, p in recs}
    out = {}
    for i, t, p in recs:
        w = None
        if t == 1:
            d = _as(p, 'struct doorobj')
            sib = int(d['sibling'])
            w = {'doorflags': int(d['doorflags']), 'doortype': int(d['doortype']), 'keyflags': int(d['keyflags']) & 0xffffffff,
                 'autoclose': int(d['autoclosetime']), 'maxfrac': _r(d['maxfrac']), 'perimfrac': _r(d['perimfrac']),
                 'accel': _r(d['accel']), 'decel': _r(d['decel']), 'maxspeed': _r(d['maxspeed']),
                 'soundtype': int(d['soundtype']), 'fadetime60': int(d['fadetime60']),
                 'xludist': int(d['xludist']), 'opadist': int(d['opadist']), 'portal': int(d['portalnum']),
                 'laserfade': int(d['laserfade']), 'sibling': index_of.get(sib, -1) if sib else -1}
        elif t == 8:
            r = _as(p, 'struct weaponobj')
            dual = int(r['dualweapon'])
            w = {'weaponnum': int(r['weaponnum']), 'dual': index_of.get(dual, -1) if dual else -1}
        elif t == 20:
            r = _as(p, 'struct multiammocrateobj')
            w = {'ammo': [[int(r['slots'][k]['modelnum']), int(r['slots'][k]['quantity'])] for k in range(19)]}
        elif t == 4:
            w = {'keyflags': int(_as(p, 'struct keyobj')['keyflags']) & 0xffffffff}
        elif t == 10:
            r = _as(p, 'struct singlemonitorobj')
            w = {'owneroffset': int(r['owneroffset']), 'ownerpart': int(r['ownerpart']), 'imagenum': int(r['imagenum'])}
        elif t == 11:
            r = _as(p, 'struct multimonitorobj')
            w = {'imagenums': [int(r['imagenums'][k]) for k in range(4)]}
        elif t == 13:
            r = _as(p, 'struct autogunobj')
            w = {'targetpad': int(r['targetpad']), 'maxspeed': _r(r['maxspeed']), 'aimdist': _r(r['aimdist']),
                 'ymaxleft': _r(r['ymaxleft']), 'ymaxright': _r(r['ymaxright'])}
        elif t == 47:
            r = _as(p, 'struct tintedglassobj')
            w = {'xludist': int(r['xludist']), 'opadist': int(r['opadist']), 'portal': int(r['portalnum'])}
        elif t == 21:
            r = _as(p, 'struct shieldobj')
            w = {'initialamount': _r(r['initialamount']), 'amount': _r(r['amount'])}
        elif t == 9:
            g = _as(p, 'struct packedchr')
            w = {'chrnum': int(g['chrnum']), 'pad': int(g['padnum']) & 0xffff, 'body': int(g['bodynum']) & 0xff,
                 'head': int(g['headnum']) & 0xff, 'ailist': int(g['ailistnum']) & 0xffff,
                 'preset': int(g['padpreset']) & 0xffff, 'chrpreset': int(g['chrpreset']) & 0xffff,
                 'hearscale': int(g['hearscale']) & 0xffff, 'viewdist': int(g['viewdist']) & 0xffff,
                 'spawnflags': int(g['spawnflags']) & 0xffffffff, 'flags': int(g['flags']) & 0xffffffff,
                 'flags2': int(g['flags2']) & 0xffffffff}
        elif t == 18:
            g = _as(p, 'struct grenadeprobobj')
            w = {'chrnum': int(g['chrnum']), 'grenadeprob': int(g['probability'])}
        if w is not None:
            out[str(i)] = w
    return out


def tiles():
    """The conversion's tile graph as gestan.c holds it: per tile [room, special,
    [[x, y, z, across], ...]], across the tile index over the edge to the next
    point, -1 an unlinked edge with a wall, -2 one with no length in plan (no
    wall), with 0x4000 added where the link climbs (a wall raised on it too)."""
    try:
        S = P.ev("'gestan.c'::g_Stan")
    except gdb.error:
        return None
    if not int(S['active']) or int(S['numtiles']) <= 0:
        return None
    n = int(S['numtiles'])
    tt = gdb.lookup_type('struct stantile')
    pt = gdb.lookup_type('struct stanpoint')
    inf = gdb.selected_inferior()
    tb = bytes(inf.read_memory(int(S['tiles']), n * tt.sizeof))
    toff = {f.name: f.bitpos // 8 for f in tt.fields()}
    poff = {f.name: f.bitpos // 8 for f in pt.fields()}
    tiles = []
    npoints = 0
    for i in range(n):
        o = i * tt.sizeof
        room = struct.unpack_from('<h', tb, o + toff['room'])[0]
        special = tb[o + toff['special']]
        npts = tb[o + toff['npts']]
        first = struct.unpack_from('<i', tb, o + toff['first'])[0]
        tiles.append((room, special, npts, first))
        npoints = max(npoints, first + npts)
    pb = bytes(inf.read_memory(int(S['points']), max(1, npoints) * pt.sizeof))
    out = []
    for room, special, npts, first in tiles:
        pts = []
        for k in range(npts):
            o = (first + k) * pt.sizeof
            x, y, z, across = (struct.unpack_from('<h', pb, o + poff[f])[0] for f in ('x', 'y', 'z', 'across'))
            climb = pb[o + poff['climbwall']]
            pts.append([x, y, z, across + (0x4000 if climb and across >= 0 else 0)])
        out.append([room, special, pts])
    return {'tiles': out}


def wide():
    return {'props': props(), 'tiles': tiles()}


def chrweapons():
    """Each chr's held weapons as [weaponnum, the weapon prop's flag word]."""
    out = {}
    player = int(P.ev('g_Vars.currentplayer->prop'))
    for k in range(int(P.ev('g_NumChrSlots'))):
        c = P.ev('g_ChrSlots[%d]' % k)
        prop = c['prop']
        if int(c['chrnum']) < 0 or int(prop) == 0 or int(prop) == player:
            continue
        try:
            if int(prop['chr']) != int(c.address):
                continue
        except gdb.MemoryError:
            continue
        held = []
        for h in range(2):
            wp = c['weapons_held'][h]
            if int(wp) == 0 or int(wp['weapon']) == 0:
                held.append(None)
                continue
            wo = wp['weapon'].dereference()
            held.append([int(wo['weaponnum']), int(wo['base']['flags']) & 0xffffffff])
        out[str(int(c['chrnum']))] = held
    return out

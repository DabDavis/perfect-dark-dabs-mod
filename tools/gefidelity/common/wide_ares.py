"""The wide world dump's cartridge half: what decides how each setup record
behaves (doors, pickups, crates, keys, monitors, autoguns, glass, armour, the
guard records and their grenade odds) and GoldenEye's tile graph, read from
the cartridge in ares through aresge.py's n64twin connection.

world/dump.py adds it as w['wide'] when GF_WIDE=1. Its layouts are
common/ares_layout_wide.json (gen_ares_layout_wide.py), kept apart from the
shared ares_layout.json. The values are GoldenEye's own, untranslated: the
translation to ours is world/widediff.py's, with the converter lines that
prove it.
"""
import json, os, struct

import aresge as A

HERE = os.path.dirname(os.path.abspath(__file__))
_W = json.load(open(os.path.join(HERE, 'ares_layout_wide.json')))
WT = _W['types']
WSYM = _W['symbols']


class R:
    """One record of the wide layout, read whole."""
    def __init__(self, typ, addr, data=None):
        self.t = WT[typ]
        self.addr = addr
        self.b = data if data is not None else A.peek(addr, self.t['size'])

    def ent(self, f):
        return self.t['fields'][f]

    def u(self, f, size=None, signed=False):
        e = self.ent(f)
        n = size or e['size']
        fmt = {1: 'b', 2: 'h', 4: 'i'}[n]
        return struct.unpack_from('>' + (fmt if signed else fmt.upper()), self.b, e['off'])[0]

    def s(self, f, size=None):
        return self.u(f, size, True)

    def f(self, f):
        return struct.unpack_from('>f', self.b, self.ent(f)['off'])[0]

    def at(self, off, fmt):
        return struct.unpack_from('>' + fmt, self.b, off)[0]


def _r(v):
    return round(float(v), 4)


def _records():
    """Every setup record: (index, type, address), as aresge.props() walks them."""
    setup = A.Rec('stagesetup', A.SYM['g_CurrentSetup'])
    p = setup.ptr('propDefs')
    i = 0
    out = []
    while i < 5000:
        t = A.Rec('PropDefHeaderRecord', p)['type']
        if t == A.PROPDEF_END:
            break
        out.append((i, t, p))
        p += 4 * A.SIZEPROPDEF.get(t, 1)
        i += 1
    return out


def props():
    recs = _records()
    index_of = {addr: i for i, t, addr in recs}
    out = {}
    for i, t, p in recs:
        w = None
        if t == 1:
            d = R('DoorRecord', p)
            linked = d.u('linkedDoor')
            w = {'doorflags': d.u('doorFlags'), 'doortype': d.u('doorType'), 'keyflags': d.u('keyflags'),
                 'autoclose': d.s('autoCloseFrames'), 'maxfrac': _r(d.f('maxFrac')), 'perimfrac': _r(d.f('perimFrac')),
                 'accel': _r(d.f('accel')), 'decel': _r(d.f('decel')), 'maxspeed': _r(d.f('maxSpeed')),
                 'opensound': d.u('doorOpenSound'), 'soundtype': d.u('soundType'), 'fadetime60': d.u('fadeTime60'),
                 # GoldenEye reads its opaque distance as the whole word over CullDist,
                 # soundType and fadeTime60 (the decomp's DOOR_OPADIST(); geconvert.c doorRecord)
                 'tintdist': d.s('TintDist'), 'culldist_word': d.at(d.ent('CullDist')['off'], 'i'),
                 'portal': d.s('portalNumber'), 'sibling': index_of.get(linked, -1) if linked else -1}
        elif t == 8:
            r = R('WeaponObjRecord', p)
            dual = r.u('dualweapon')
            w = {'weaponnum': r.u('weaponnum'), 'dual': index_of.get(dual, -1) if dual else -1}
        elif t == 20:
            r = R('MultiAmmoCrateRecord', p)
            off = r.ent('slots')['off']
            w = {'ammo': [list(struct.unpack_from('>hh', r.b, off + 4 * k)) for k in range(13)]}
        elif t == 4:
            w = {'keyflags': R('KeyRecord', p).u('keyflags')}
        elif t == 10:
            r = R('MonitorObjRecord', p)
            w = {'owneroffset': r.s('OwnerOffset'), 'ownerpart': r.s('OwnerPart'), 'imagenum': r.s('ImageNum')}
        elif t == 11:
            r = R('MultiMonitorObjRecord', p)
            off = r.ent('ImageNums')['off']
            w = {'imagenums': list(r.b[off:off + 4])}
        elif t == 13:
            r = R('AutogunRecord', p)
            w = {'targetpad': r.s('padID'), 'maxspeed': _r(r.f('speed')), 'aimdist': _r(r.f('aimdist')),
                 'ymaxleft': _r(r.f('unk88')), 'ymaxright': _r(r.f('unk8C'))}
        elif t == 47:
            r = R('TintedGlassRecord', p)
            w = {'tintdist': r.s('TintDist'), 'culldist': r.s('CullDist'), 'portal': r.s('portalnum')}
        elif t == 21:
            r = R('BodyArmourRecord', p)
            w = {'initialamount': _r(r.f('initialamount')), 'amount': _r(r.f('amount'))}
        elif t == 9:
            g = R('GuardRecord', p)
            w = {'chrnum': g.s('chrnum'), 'pad': g.u('PadID'), 'body': g.u('BodyID'), 'head': g.s('HeadID'),
                 'ailist': g.u('AIListID'), 'preset': g.u('Preset'), 'chrpreset': g.u('chrpreset1'),
                 'hearscale': g.u('health'), 'viewdist': g.u('ReactionTime'), 'bitflags': g.u('bitflags')}
        elif t == 18:
            g = R('GuardAttributeRecord', p)
            w = {'chrnum': g.s('chrnum'), 'grenadeprob': g.s('GrenadeProb')}
        if w is not None:
            out[str(i)] = w
    return out


def tiles():
    """GoldenEye's tile graph as loaded: per tile [room, special, [[x, y, z, neighbour], ...]],
    x/y/z in the world (the stan file's units times the level scale, as the pads
    are), neighbour the index of the tile across the edge to the next point or -1.
    A link names its tile by address: standTileStart + (link << 3), standTileStart
    being the first tile less 0x80, and a link under 16 names nothing (stan.c;
    geconvert.c stanRead())."""
    walk = _tile_records()
    if walk is None:
        return None
    start, scale, raw = walk
    index_of = {addr: i for i, (addr, _, _, _) in enumerate(raw)}
    out = []
    for addr, room, special, pts in raw:
        out.append([room, special, [[_r(x * scale), _r(y * scale), _r(z * scale),
                                     index_of.get(start + (link << 3), -1) if link >> 4 else -1]
                                    for x, y, z, link in pts]])
    return {'scale': _r(scale), 'tiles': out}


def _tile_records():
    """The loaded tile graph raw: (standTileStart, level scale, [(address, room,
    special, [(x, y, z, link), ...]), ...]) in the stan file's units."""
    start = A.u32(WSYM['standTileStart'])
    if not start:
        return None
    scale = struct.unpack('>f', A.peek(WSYM['room_data_float2'], 4))[0]
    first = start + 0x80
    blob = b''
    raw = []
    o = 0
    while True:
        while o + 8 > len(blob):
            blob += A.peek(first + len(blob), 4096)
        if blob[o:o + 8] == b'\0' * 8:
            break
        room = blob[o + 3]
        special = struct.unpack_from('>H', blob, o + 4)[0] >> 12
        npts = struct.unpack_from('>H', blob, o + 6)[0] >> 12
        while o + 8 + 8 * npts > len(blob):
            blob += A.peek(first + len(blob), 4096)
        pts = [struct.unpack_from('>hhhH', blob, o + 8 + 8 * k) for k in range(npts)]
        raw.append((first + o, room, special, pts))
        o += 8 + 8 * npts
        if len(raw) > 20000:
            raise RuntimeError('no end to the tile graph')
    return start, scale, raw


def tile_under(x, y, z, above=60.0):
    """The address of the highest tile whose outline holds x, z and whose floor
    is under y (within `above`) - where a man stands to reach something at x,
    y, z. A pad's own tile need not be it: Goldfinger has pads hundreds of
    units under theirs (Plane's pad 0x56: the upper deck's tile, the pad on
    the hold's floor), and that floor's height: (address, y). None if no
    floor is under it."""
    walk = _tile_records()
    if walk is None:
        return None
    start, scale, raw = walk
    best = None
    for addr, room, special, pts in raw:
        xs = [p[0] * scale for p in pts]
        zs = [p[2] * scale for p in pts]
        inside = False
        for i in range(len(pts)):
            xi, zi, xj, zj = xs[i], zs[i], xs[i - 1], zs[i - 1]
            if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
                inside = not inside
        if not inside:
            continue
        fy = sum(p[1] for p in pts) * scale / len(pts)
        if fy <= y + above and (best is None or fy > best[0]):
            best = (fy, addr)
    return (best[1], best[0]) if best else None


def wide():
    return {'props': props(), 'tiles': tiles()}


def chrweapons():
    """Each chr's held weapons as [weaponnum, the weapon prop's flag word] - the
    flag word is where an AI list's TRYGiveMeItem flags land (the paired flag)."""
    n = A.s32(A.SYM['g_NumChrSlots'])
    base = A.u32(A.SYM['g_ChrSlots'])
    size = A.T['ChrRecord']['size']
    blob = A.peek(base, size * n) if n > 0 else b''
    out = {}
    for k in range(n):
        c = A.Rec('ChrRecord', base + k * size, blob[k * size:(k + 1) * size])
        pa = c.ptr('prop')
        if c['chrnum'] < 0 or not pa or A.Rec('PropRecord', pa).ptr('chr') != base + k * size:
            continue
        held = []
        wo = c.off('weapons_held')
        for h in range(2):
            wp = struct.unpack_from('>I', c.b, wo + 4 * h)[0]
            if not wp:
                held.append(None)
                continue
            w = A.Rec('PropRecord', wp).ptr('weapon')
            if not w:
                held.append(None)
                continue
            r = R('WeaponObjRecord', w)
            held.append([r.u('weaponnum'), struct.unpack_from('>I', r.b, 8)[0]])
        out[str(c['chrnum'])] = held
    return out

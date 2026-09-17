#!/usr/bin/env python3
"""GoldenEye levels as Perfect Dark arenas: a maps-only mod the Stage Loader
registers (CLAUDE-notes/mods.md "The Stage Loader").

    GE_ROM=goldeneye.z64 geconvert.py OUTDIR [level ...]     (levels: gefiles.LEVELS keys)

port/src/geconvert.c is this, step for step, and writes the same bytes: the
game runs that one at startup on the player's ROM. Keep the two in step, and
compare them (the C built alone with -DGECONVERT_MAIN) after changing either.
With GE_ARENAS_JSON set, each level's scale and offset are recorded there for
.xbla-work/ge-bean/gen_stagetable.py.

Per level it writes files/bgdata/bg_gxNAME.seg, _tilesZ, _padsZ,
files/Ump_setupgxNAMEZ, the textures the rooms name (GoldenEye's own texture
files are Perfect Dark's format byte for byte: its 161 is GE-X's 073e.bin), and
a modconfig.txt `maps` block.

Units: GoldenEye's bg units divided by the level scale (centimetres), as
GoldenEye does at run time and as GE-X converted the levels it kept.

What is and is not carried across:
- rooms: GoldenEye's own display lists, vertices repacked 16 -> 12 bytes with
  one colour table entry per vertex, G_TRI1 as G_TRI4; primary lists opaque,
  secondary translucent
- portals: GoldenEye's, scaled; the visibility commands are not (an END only)
- tiles: every stan tile a floor, every stan edge with no neighbour a wall
- pads and waypoints: the setup's
- multiplayer: GoldenEye's own spawns and weapon/ammo pads where it has a
  multiplayer setup, else spawns and weapons on pads spread over the level
- sky and fog: the level's row of GoldenEye's fog table (bgfog.c, the US
  table), which is Perfect Dark's fog table row field for field; Caves has no
  row and GoldenEye draws it with the fogless default, which is the port's too
- doors and props: GoldenEye's own objects (geobjects.py) with GoldenEye's own
  models converted (gemodelconv.py, the `models` block); nothing from GE-X
- lights: a Perfect Dark light round each fixture GoldenEye draws with a light
  texture, which a shot breaks
"""
import collections, math, os, struct, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gefiles
import geobjects
import texremap
import gemodelconv

SEG = 0x0f000000

# GoldenEye's level ids, as its fog table names them
LEVELIDS = {'dam': 'DAM', 'run': 'RUNWAY', 'stat': 'STATUE', 'tra': 'TRAIN',
            'pete': 'STREETS', 'jun': 'JUNGLE', 'oat': 'CAVES',
            'dish': 'TEMPLE', 'ref': 'COMPLEX', 'lib': 'LIBRARY', 'base': 'BASEMENT', 'stack': 'STACK',
            'ark': 'FACILITY', 'sevb': 'BUNKER2', 'arch': 'ARCHIVES', 'cave': 'CAVERNS', 'cryp': 'EGYPT',
            'crad': 'CRADLE', 'sevx': 'SURFACE', 'sevxb': 'SURFACE2', 'silo': 'SILO',
            'dest': 'FRIGATE', 'depo': 'DEPOT', 'arec': 'CONTROL', 'sev': 'BUNKER1', 'azt': 'AZTEC'}

# GoldenEye's menu folder (PROP_WALLETBOND), its pictures, and its two fonts and its
# music, raw in the ROM: {name, ROM address, size}
MENU_FOLDER_MODEL = 278
# the crosshair cursor (IMAGE_CROSSHAIR1), the film strip's holes (IMAGE_DOT),
# a stage picture for every level (IMAGE_MP_ARCHIVES..TRAIN, TEMPLE..CAVES, RANDOM)
# and the character portraits' tiles (IMAGE_BROSNAN_UL..DALTON_LR, BORIS_UL..ODDJOB_LR,
# RANDOM_UL..LR, MISHKIN)
MENU_IMAGES = ([2236, 2631] + list(range(2578, 2598)) + list(range(2686, 2690)) + [2695]
               + list(range(2602, 2618)) + list(range(2632, 2672)) + list(range(2682, 2686))
               + list(range(2691, 2695)))
MENU_RAW = (('fontbankgothic.bin', 0x2e63f0, 0x24b0), ('fontzurichbold.bin', 0x2e88a0, 0x3540),
              # and its music: the instrument bank, and the sequence table ({u16 count, pad, then
              # u32 offset, u16 inflated, u16 zipped} a sequence) with the sequences after it
              ('instrumentsctl', 0x3b4450, 0x43a0), ('instrumentstbl', 0x3b87f0, 0x60fa0),
              ('sequences', 0x419790, 0x1eed0))
# The missions' text, in GoldenEye's mission order: each mission's briefing file
# (front.h's struct BriefStruct - four paragraph text ids then ten objectives of
# {text id, the difficulty it starts at}) and the level's own text bank, which
# every id in that file indexes (a text id is bank * 0x400 + slot).
MENU_TEXT = (('UbriefdamZ', 'LdamE'), ('UbriefarkZ', 'LarkE'),
             ('UbriefrunZ', 'LrunE'), ('UbriefsevxZ', 'LsevxE'),
             ('UbriefsevbunkerZ', 'LsevE'), ('UbriefsiloZ', 'LsiloE'),
             ('UbriefdestZ', 'LdestE'), ('UbriefsevxbZ', 'LsevxbE'),
             ('UbriefsevbZ', 'LsevbE'), ('UbriefstatueZ', 'LstatE'),
             ('UbriefarchZ', 'LarchE'), ('UbriefpeteZ', 'LpeteE'),
             ('UbriefdepoZ', 'LdepoE'), ('UbrieftraZ', 'LtraE'),
             ('UbriefjunZ', 'LjunE'), ('UbriefcontrolZ', 'LarecE'),
             ('UbriefcaveZ', 'LcaveE'), ('UbriefcradZ', 'LcradE'),
             ('UbriefaztZ', 'LaztE'), ('UbriefcrypZ', 'LcrypE'))

NAMES = {'dam': 'Dam', 'run': 'Runway', 'stat': 'Statue Park', 'tra': 'Train',
         'pete': 'Streets', 'jun': 'Jungle', 'oat': 'Caves',
         'dish': 'Temple', 'ref': 'Complex', 'lib': 'Library', 'base': 'Basement', 'stack': 'Stack',
         'ark': 'Facility', 'sevb': 'Bunker', 'arch': 'Archives', 'cave': 'Caverns', 'cryp': 'Egyptian',
         'crad': 'Cradle', 'sevx': 'Surface', 'sevxb': 'Surface 2', 'silo': 'Silo',
         'dest': 'Frigate', 'depo': 'Depot', 'arec': 'Control', 'sev': 'Bunker 1', 'azt': 'Aztec'}


# ---------------------------------------------------------------------------
# helpers

def rzip1173(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    n = len(data)
    return b'\x11\x73' + bytes([(n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff]) + body


def pad(b, n):
    return b + b'\0' * ((-len(b)) % n)


def s16(v):
    r = int(round(v))
    if r < -32768 or r > 32767:
        raise OverflowError(v)
    return r


# ---------------------------------------------------------------------------
# rooms

# GoldenEye's light textures (lightfixture.c check_if_imageID_is_light()):
# a triangle drawn with one is a light fixture, which a shot breaks
LIGHT_IMAGES = {201, 203, 205, 252, 254, 255, 256, 428, 982, 1383}


def convert_list(dl, vtx, out_vtx, out_col, base_vtx, textures, lighttris=None):
    """One GoldenEye display list -> Perfect Dark room list words.

    Every G_VTX load gets its own copy of the vertices it loads, with a G_COL
    for their colours, so a vertex's colour byte is its index in that load.
    A triangle drawn with a light texture goes into lighttris as its three
    room-relative positions."""
    words = []
    slots = {}
    curtex = None
    o = 0
    while o + 8 <= len(dl):
        w0, w1 = struct.unpack_from('>II', dl, o)
        o += 8
        op = w0 >> 24
        if op == 0x04:
            n = ((w0 >> 20) & 0xf) + 1
            v0 = (w0 >> 16) & 0xf
            src = (w1 & 0xffffff) // 16
            start = len(out_vtx) - base_vtx
            for i in range(n):
                k = src + i
                x, y, z, flag, s, t, r, g, b, a = struct.unpack_from('>3hHhh4B', vtx, 16 * k)
                slots[v0 + i] = len(out_vtx)
                out_vtx.append((x, y, z, i << 2, s, t))
                out_col.append((r << 24) | (g << 16) | (b << 8) | a)
            words.append(((0x07 << 24) | (((n - 1) << 2) << 16) | (n * 4), 0x0d000000 | (start * 4)))
            words.append(((0x04 << 24) | ((n - 1) << 20) | (v0 << 16) | (n * 12), 0x0e000000 | (start * 12)))
        elif op == 0xbf:
            # F3D G_TRI1: w1 bytes 1-3 hold the indices times ten
            a, b, c = ((w1 >> 16) & 0xff) // 10, ((w1 >> 8) & 0xff) // 10, (w1 & 0xff) // 10
            words.append(((0xb1 << 24) | c, (b << 4) | a))
            if lighttris is not None and curtex in LIGHT_IMAGES and all(k in slots for k in (a, b, c)):
                lighttris.append([out_vtx[slots[k]][:3] for k in (a, b, c)])
        elif op == 0xb8:
            words.append((w0, w1))
            break
        else:
            if op == 0xc0:
                curtex = w1 & 0xfff
                textures.add(w1 & 0xfff)
                w1 = (w1 & ~0xfff) | texremap.remap(w1 & 0xfff)
                if (w0 & 7) == 1:
                    textures.add((w1 >> 12) & 0xfff)
                    w1 = (w1 & ~0xfff000) | (texremap.remap((w1 >> 12) & 0xfff) << 12)
            words.append((w0, w1))
    return words


def loaded_vertices(dl):
    out = []
    for o in range(0, len(dl) - 7, 8):
        w0, w1 = struct.unpack_from('>II', dl, o)
        if w0 >> 24 == 0x04:
            src = (w1 & 0xffffff) // 16
            out.extend(range(src, src + ((w0 >> 20) & 0xf) + 1))
        elif w0 >> 24 == 0xb8:
            break
    return out


def scaled_room(room, inv, offset):
    """The room's vertices at world scale, and a room position that keeps
    them inside an s16."""
    vtx = room['vtx'] or b''
    n = len(vtx) // 16
    pts = np.array([struct.unpack_from('>3h', vtx, 16 * k) for k in range(n)], float).reshape(-1, 3)
    pos = np.array(room['pos'], float)
    world = (pts + pos) * inv - offset
    if n:
        # Vertex arrays carry entries no list loads (zeros at GoldenEye's
        # origin in Streets), so the middle is of the loaded ones
        loaded = sorted(set(k for dl in (room['pri'], room['sec']) if dl for k in loaded_vertices(dl)))
        ref = world[[k for k in loaded if k < n]] if loaded else world
        centre = np.round((ref.min(0) + ref.max(0)) / 2)
    else:
        centre = np.round(pos * inv - offset)
    rel = world - centre
    if n and np.abs(rel).max() > 32767:
        raise OverflowError('room spans %s' % (world.max(0) - world.min(0)))
    newvtx = bytearray(vtx)
    for k in range(n):
        struct.pack_into('>3h', newvtx, 16 * k, *[s16(v) for v in rel[k]])
    return bytes(newvtx), centre, world


def fixture_lights(tris):
    """Perfect Dark lights for a room's light-texture triangles: those that
    touch make one fixture, and its light is the rectangle round it in its own
    plane (corners in order, as lightsHandleHit() splits it into (0,1,3) and
    (1,2,3)), facing into the room - the room's position is its middle - and
    its glare down unless it is on a wall."""
    if not tris:
        return []
    key = lambda p: tuple(int(round(c)) for c in p)
    parent = list(range(len(tris)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    owner = {}
    for i, t in enumerate(tris):
        for p in t:
            j = owner.setdefault(key(p), i)
            parent[find(i)] = find(j)
    groups = collections.defaultdict(list)
    for i in range(len(tris)):
        groups[find(i)].append(np.array(tris[i], float))
    lights = []
    for members in groups.values():
        normal = sum(np.cross(t[1] - t[0], t[2] - t[0]) for t in members)
        if np.linalg.norm(normal) < 1e-6:
            continue
        n = normal / np.linalg.norm(normal)
        pts = np.concatenate(members)
        middle = pts.mean(0)
        if np.dot(n, -middle) < 0:
            n = -n
        # a ceiling or hanging fixture lights the floor, wherever the middle
        # of its room's geometry is (Dam's lamps hang below some of theirs)
        glare = n if abs(n[1]) < 0.5 else np.array([0.0, -1.0, 0.0])
        u = np.cross(n, [0, 1, 0] if abs(n[1]) < 0.9 else [1, 0, 0])
        u /= np.linalg.norm(u)
        v = np.cross(n, u)
        pu, pv = pts @ u, pts @ v
        d = float(np.mean(pts @ n))
        corners = [d * n + a * u + b * v for a, b in
                   ((pu.min(), pv.min()), (pu.max(), pv.min()), (pu.max(), pv.max()), (pu.min(), pv.max()))]
        lights.append(dict(corners=[[s16(c) for c in q] for q in corners],
                           dir=[int(round(c * 127)) for c in glare]))
    return lights


def write_room(room, inv, offset, base_ptr, textures, lightsindex=0):
    vtx, centre, world = scaled_room(room, inv, offset)
    leaves = []
    out_vtx, out_col = [], []
    lighttris = []
    for kind in ('pri', 'sec'):
        dl = room[kind]
        if not dl:
            leaves.append(None)
            continue
        base = len(out_vtx)
        words = convert_list(dl, vtx, out_vtx, out_col, base, textures, lighttris)
        leaves.append((base, words))
    lights = fixture_lights(lighttris)[:255]
    nleaves = sum(1 for l in leaves if l)
    # vertices start on an eight byte boundary and are an even number, so the
    # colour table does too (gebeanstage.c writeRoom())
    vtxat = (0x18 + 20 * nleaves + 7) & ~7
    v_pad = len(out_vtx) & 1
    if v_pad:
        out_vtx.append(out_vtx[-1][:3] + (0, 0, 0))
        out_col.append(0)
    colat = vtxat + 12 * len(out_vtx)
    gdlat = (colat + 4 * len(out_col) + 7) & ~7
    body = bytearray(gdlat)
    struct.pack_into('>II', body, 0, base_ptr + vtxat, base_ptr + colat)
    blocks = []
    gdls = b''
    at = 0x18
    ptrs = [0, 0]
    for i, leaf in enumerate(leaves):
        if not leaf:
            continue
        base, words = leaf
        ptrs[i] = base_ptr + at
        gdl = b''.join(struct.pack('>II', a, b) for a, b in words)
        struct.pack_into('>BxxxIIII', body, at, 0, 0, base_ptr + gdlat + len(gdls),
                         base_ptr + vtxat + 12 * base, base_ptr + colat + 4 * base)
        gdls += gdl
        at += 20
    struct.pack_into('>II', body, 8, ptrs[0], ptrs[1])
    struct.pack_into('>hhhh', body, 0x10, lightsindex if lights else -1, len(lights), min(len(out_vtx), 32767), min(len(out_col), 32767))
    for k, (x, y, z, c, s, t) in enumerate(out_vtx):
        struct.pack_into('>3hBBhh', body, vtxat + 12 * k, x, y, z, 0, c, s, t)
    for k, c in enumerate(out_col):
        struct.pack_into('>I', body, colat + 4 * k, c)
    data = bytes(body) + gdls
    bbox = None
    used = np.array([v[:3] for v in out_vtx[:len(out_vtx) - (1 if len(out_vtx) and out_col[-1] == 0 and v_pad else 0)]], float) if out_vtx else np.zeros((0, 3))
    if len(used):
        mn = np.floor(used.min(0))
        mx = np.ceil(used.max(0))
        bbox = [s16(v) for v in list(mn) + list(mx)]
    else:
        bbox = [0] * 6
    return data, centre, bbox, lights


def write_bg(bg, ls, offset):
    inv = 1.0 / ls
    textures = set()
    n = bg.numrooms
    # primary: header, room table (n + 3 entries), bgcmds, portals, portal vertices
    table_at = 0x18
    # the rooms are converted first, since their lights go in the primary data
    converted = []
    alllights = []
    for r, room in enumerate(bg.rooms, 1):
        converted.append(write_room(room, inv, offset, 0, textures, len(alllights)))
        alllights += [(r, l) for l in converted[-1][3]]
    # the lights go straight before the bgcmds: the port's preprocessor counts
    # them by the distance between the two (filebg.c convertPrimaryLights())
    lights_at = table_at + 20 * (n + 3)
    lights = b''.join(struct.pack('>HHBBBbbb', r, 0xffff, 0, 0, 0, *l['dir'])
                      + b''.join(struct.pack('>3h', *c) for c in l['corners']) for r, l in alllights)
    cmds_at = lights_at + len(lights) + (-len(lights)) % 4
    cmds = struct.pack('>BBxxi', 0, 1, 0)
    portals_at = cmds_at + len(cmds)
    portals = b''
    groups = b''
    for i, p in enumerate(bg.portals):
        portals += struct.pack('>HhhBx', i + 1, p['room1'], p['room2'], 0)
        groups += struct.pack('>Bxxx', len(p['points']))
        for q in p['points']:
            groups += struct.pack('>3f', *(np.array(q) * inv - offset))
    portals += struct.pack('>HhhBx', 0, 0, 0, 0)
    groups += b'\0\0\0\0'
    groups_at = portals_at + len(portals)
    primary_len = groups_at + len(groups)
    primary = bytearray(pad(bytes(primary_len), 4))
    struct.pack_into('>IIIIII', primary, 0, 0, SEG + table_at, SEG + portals_at, SEG + cmds_at, SEG + lights_at if lights else 0, 0)
    primary[lights_at:lights_at + len(lights)] = lights
    primary[cmds_at:cmds_at + len(cmds)] = cmds
    primary[portals_at:portals_at + len(portals)] = portals
    primary[groups_at:groups_at + len(groups)] = groups
    inf = len(primary)
    # rooms, at pointers after the inflated primary
    ptr = SEG + inf
    blobs, bboxes, lens = [], [], []
    lightcounts = []
    for r, room in enumerate(bg.rooms, 1):
        lightsindex = sum(lightcounts)
        data, centre, bbox, roomlights = write_room(room, inv, offset, ptr, textures, lightsindex)
        lightcounts.append(len(roomlights))
        z = pad(rzip1173(data), 1)
        struct.pack_into('>I3fBBxx', primary, table_at + 20 * r, ptr, *centre, 128, 255)  # GE-X's brightness range; 0 draws every room black
        blobs.append(z)
        bboxes.append(bbox)
        lens.append(len(data))
        ptr += len(z)
    struct.pack_into('>I3fBBxx', primary, table_at + 20 * (n + 1), ptr, 0, 0, 0, 0, 0)
    primz = rzip1173(bytes(primary))
    rooms = b''.join(blobs)
    out = bytearray(struct.pack('>III', inf, 0, len(primz)))
    out += primz
    # room pointers are file offset + (inf - primz - 0xc) from SEG: rooms follow primz
    assert len(out) == 12 + len(primz)
    out += rooms
    out = bytearray(pad(bytes(out), 2))
    struct.pack_into('>I', out, 4, len(out) - 0xc)
    # section 2: texture numbers
    tex = sorted(textures)
    t = b''.join(struct.pack('>H', texremap.remap(x)) for x in tex)
    tz = rzip1173(t)
    # section 3 is read straight after section 2's stored bytes (g_BgSection3)
    out += struct.pack('>HH', 0x8000 | len(t), len(tz)) + tz
    # section 3: bounding boxes, gfxdatalen, light counts
    s3 = b''.join(struct.pack('>6h', *b) for b in bboxes)
    s3 += b''.join(struct.pack('>H', min(0xffff, l // 16 + 1)) for l in lens)
    s3 += bytes(lightcounts)
    s3z = rzip1173(s3)
    out += struct.pack('>HH', 0x8000 | len(s3), len(s3z)) + s3z
    return pad(bytes(out), 16), tex, len(alllights)


# ---------------------------------------------------------------------------
# tiles

WALL_BELOW = 50.0   # world units a wall reaches below the floor edge it stands on
WALL_ABOVE = 400.0  # and above


def read_stan(data):
    """Tiles: [(room, [(x, y, z, link)], special)] in bg units, and each tile's
    file offset (a link names its neighbour by offset)."""
    first = struct.unpack_from('>I', data, 4)[0]
    tiles = []
    o = first
    while o + 8 <= len(data):
        hdr = data[o:o + 8]
        if hdr == bytes(8):
            break
        room = data[o + 3]
        special = struct.unpack_from('>H', data, o + 4)[0] >> 12
        npts = struct.unpack_from('>H', data, o + 6)[0] >> 12
        pts = [struct.unpack_from('>3hH', data, o + 8 + 8 * k) for k in range(npts)]
        tiles.append(dict(room=room, points=pts, special=special, offset=o))
        o += 8 + 8 * npts
    return tiles


def write_tiles(stan, numrooms, ls, offset):
    inv = 1.0 / ls
    rooms = [[] for _ in range(numrooms + 1)]
    walls = 0
    for t in stan:
        pts = [(x * inv - offset[0], y * inv - offset[1], z * inv - offset[2]) for x, y, z, _ in t['points']]
        flags = 0x0001 | 0x0002 | 0x0008 | 0x0010
        if t['special'] == 3:
            flags |= 0x0040
        rooms[t['room']].append((flags, pts))
        n = len(t['points'])
        for i in range(n):
            link = t['points'][i][3]
            if link >> 4:
                continue
            a, b = pts[i], pts[(i + 1) % n]
            quad = [(a[0], a[1] - WALL_BELOW, a[2]), (b[0], b[1] - WALL_BELOW, b[2]),
                    (b[0], b[1] + WALL_ABOVE, b[2]), (a[0], a[1] + WALL_ABOVE, a[2])]
            rooms[t['room']].append((0x0004, quad))
            walls += 1
    body = []
    for tiles in rooms:
        b = b''
        for flags, pts in tiles:
            ints = [[s16(c) for c in p] for p in pts]
            mn = [min(range(len(ints)), key=lambda k: ints[k][a]) for a in range(3)]
            mx = [max(range(len(ints)), key=lambda k: ints[k][a]) for a in range(3)]
            b += struct.pack('>BBHH6BH', 0, len(ints), flags, 0, *mn, *mx, 0x0fff)
            for p in ints:
                b += struct.pack('>3h', *p)
        body.append(b)
    out = struct.pack('>I', len(rooms))
    pos = 4 + 4 * len(rooms) + 4
    for b in body:
        out += struct.pack('>I', pos)
        pos += len(b)
    out += struct.pack('>I', pos)
    out += b''.join(body)
    return rzip1173(pad(out, 16)), walls


# ---------------------------------------------------------------------------
# setups and pads

def read_setup(data):
    h = struct.unpack_from('>10I', data, 0)
    pads = []
    o = h[6]
    while True:
        pos = struct.unpack_from('>3f', data, o)
        up = struct.unpack_from('>3f', data, o + 12)
        look = struct.unpack_from('>3f', data, o + 24)
        plink = struct.unpack_from('>I', data, o + 36)[0]
        if plink == 0:
            break
        pads.append(dict(pos=pos, up=up, look=look))
        o += 0x2c

    def s32list(at):
        out = []
        while True:
            v = struct.unpack_from('>i', data, at)[0]
            if v == -1:
                return out
            out.append(v)
            at += 4
    waypoints = []
    if h[0]:
        o = h[0]
        while True:
            padid, nb, group, _ = struct.unpack_from('>iIii', data, o)
            if padid < 0:
                break
            waypoints.append(dict(pad=padid, neighbours=s32list(nb), group=group))
            o += 16
    groups = []
    if h[1]:
        o = h[1]
        while True:
            nb, wps, _ = struct.unpack_from('>IIi', data, o)
            if nb == 0:
                break
            groups.append(dict(neighbours=s32list(nb), waypoints=s32list(wps)))
            o += 12
    spawns = []
    if h[2]:
        o = h[2]
        lengths = {0: 3, 1: 4, 2: 4, 3: 8, 4: 2, 5: 2, 6: 13, 7: 3, 8: 2}
        while o + 4 <= len(data):
            t = struct.unpack_from('>i', data, o)[0] & 0xff
            if t == 9:
                break
            if t == 0:
                spawns.append(struct.unpack_from('>i', data, o + 4)[0])
            if t not in lengths:
                break
            o += 4 * lengths[t]
    weapons, ammo = [], []
    if h[3]:
        o = h[3]
        sizes = {8: 0x22, 20: 0x2d, 21: 0x22, 3: None}
        while o + 4 <= len(data):
            w0 = struct.unpack_from('>I', data, o)[0]
            typ = w0 & 0xff
            if typ == 48:
                break
            if typ not in (8, 20, 21):
                # A multiplayer setup's other props (doors, boxes) are not
                # carried across; one this cannot size ends the walk
                break
            padnum = struct.unpack_from('>I', data, o + 4)[0] & 0xffff
            if typ == 8:
                wnum = data[o + 0x80]
                if wnum >= 0xf0:
                    weapons.append((padnum, wnum - 0xf0))
            elif typ == 20:
                ammo.append(padnum)
            o += 4 * sizes[typ]
    return dict(pads=pads, waypoints=waypoints, groups=groups, spawns=spawns, weapons=weapons, ammo=ammo)


def write_pads(setup, ls, offset, rooms, gexpads=None, gext=None, boundpads=None):
    """The pads, each with the room it is in (an object is only placed on a pad
    with a room). With GE-X's pads for the level, its records are written -
    GoldenEye's pads by index and its bound pads after them, which GE-X's
    objects stand on - moved from GE-X's frame into the arena's."""
    inv = 1.0 / ls
    pads = setup['pads']
    records = []
    if gexpads:
        for raw, flags, pos in gexpads:
            world = pos - gext - offset
            rec = bytearray(raw)
            if flags & 1:
                struct.pack_into('>3h', rec, 4, *[s16(v) for v in world])
            else:
                struct.pack_into('>3f', rec, 4, *world)
            records.append((flags, world, bytes(rec)))
    else:
        for p in pads:
            world = np.array(p['pos']) * inv - offset
            rec = struct.pack('>I', 0) + struct.pack('>3f', *world) + struct.pack('>3f', *p['up']) + struct.pack('>3f', *p['look'])
            records.append((0, world, rec))
        records += boundpads or []
    offsets = []
    body = b''
    start = (0x14 + 2 * len(records) + 3) & ~3
    for flags, world, rec in records:
        offsets.append(start + len(body))
        room = rooms.room(world)
        hdr = struct.unpack_from('>I', rec, 0)[0]
        hdr = (flags << 14) | ((room & 0x3ff) << 4) | (hdr & 0xf)
        body += pad(struct.pack('>I', hdr) + rec[4:], 4)
    head = b''.join(struct.pack('>H', x) for x in offsets)
    head = pad(head, 4)
    pads_blob = head + body
    wp_start = 0x14 + len(pads_blob)
    wps = setup['waypoints']
    wp_rec = b''
    nb_blob = b''
    npos = wp_start + 16 * len(wps) + 16
    for w in wps:
        wp_rec += struct.pack('>iIiI', w['pad'], npos, max(0, w['group']), 0)
        lst = b''.join(struct.pack('>i', x) for x in w['neighbours']) + struct.pack('>i', -1)
        nb_blob += lst
        npos += len(lst)
    wp_rec += struct.pack('>iIII', -1, 0, 0, 0)
    waypoints = wp_rec + nb_blob
    wg_start = wp_start + len(waypoints)
    groups = setup['groups']
    wlists = b''.join(b''.join(struct.pack('>i', x) for x in g['waypoints']) + struct.pack('>i', -1) for g in groups)
    wpos = wg_start + 12 * len(groups) + 12
    npos = wpos + len(wlists)
    rec = b''
    nlists = b''
    for g in groups:
        rec += struct.pack('>III', npos, wpos, 0)
        n = b''.join(struct.pack('>i', x) for x in g['neighbours']) + struct.pack('>i', -1)
        nlists += n
        npos += len(n)
        wpos += 4 * len(g['waypoints']) + 4
    rec += struct.pack('>III', 0, 0, 0)
    waygroups = rec + wlists + nlists
    cover_start = wg_start + len(waygroups)
    out = struct.pack('>iiIII', len(records), 0, wp_start, wg_start, cover_start)
    out += pads_blob + waypoints + waygroups
    return rzip1173(pad(out, 16))


def spread(points, k, seed=0):
    """k indices of points spread out by farthest-point sampling."""
    pts = np.array(points, float)
    if len(pts) <= k:
        return list(range(len(pts)))
    chosen = [int(np.argmin(np.linalg.norm(pts - pts.mean(0), axis=1)))]
    d = np.linalg.norm(pts - pts[chosen[0]], axis=1)
    while len(chosen) < k:
        i = int(np.argmax(d))
        chosen.append(i)
        d = np.minimum(d, np.linalg.norm(pts - pts[i], axis=1))
    return chosen


# Motorbikes, which GoldenEye has as a model and never places: by level, how
# many, set down beside its tank (GE-X adds two to its Runway)
BIKES = {'run': 2}


def bike_pads(key, setup, stan, bg, gedata):
    """Pads for a level's motorbikes: floored pads on its tank's floor 3 to 16 metres (GoldenEye
    units over its level scale) from its tank, spread from each other."""
    n = BIKES.get(key, 0)
    if not n:
        return []
    ls = gefiles.LEVELS[key][4]
    pads = setup['pads']
    tanks = [struct.unpack_from('>hh', b, 4)[1] & 0xffff for t, b in geobjects.records(gedata) if t == 45]
    if not tanks:
        return []
    tank = np.array(pads[tanks[0]]['pos']) / ls
    near = []
    for i in floored_pads(pads, stan, bg):
        pos = np.array(pads[i]['pos']) / ls
        if 300 <= np.linalg.norm((pos - tank)[[0, 2]]) <= 1600 and abs(pos[1] - tank[1]) < 60:
            near.append(i)
    chosen = []
    for i in sorted(near, key=lambda i: np.linalg.norm(np.array(pads[i]['pos']) / ls - tank)):
        pos = np.array(pads[i]['pos']) / ls
        if all(np.linalg.norm(pos - np.array(pads[j]['pos']) / ls) >= 250 for j in chosen):
            chosen.append(i)
        if len(chosen) == n:
            break
    print('%-5s motorbikes on pads %s' % (key, chosen))
    return chosen


AI_1000 = bytes.fromhex('01850145014600 05fd0000 0004'.replace(' ', ''))
AI_1001 = bytes.fromhex('01b21600 05fd0000 0004'.replace(' ', ''))


def floored_pads(pads, stan, bg):
    """The pads standing over a stan tile, the floor a little below them: a
    solo setup keeps pads where no one walks (Train's outside its carriages),
    and a spawn there falls for ever. The tile's room must have something to
    draw, too: Streets ships rooms 20-54 as one shared empty list over floor
    that runs on to z 33822, and a spawn there saw nothing but sky."""
    tiles = []
    for t in stan:
        if not (0 < t['room'] <= bg.numrooms and bg.rooms[t['room'] - 1]['vtx']):
            continue
        pts = np.array([q[:3] for q in t['points']], float)
        tiles.append((pts, pts[:, 0].min(), pts[:, 0].max(), pts[:, 2].min(), pts[:, 2].max()))
    out = []
    for i, p in enumerate(pads):
        x, y, z = p['pos']
        for pts, x0, x1, z0, z1 in tiles:
            if not (x0 <= x <= x1 and z0 <= z <= z1):
                continue
            inside = False
            n = len(pts)
            for k in range(n):
                ax, az = pts[k][0], pts[k][2]
                bx, bz = pts[(k + 1) % n][0], pts[(k + 1) % n][2]
                if (az > z) != (bz > z) and x < (bx - ax) * (z - az) / (bz - az) + ax:
                    inside = not inside
            if inside and -30 <= y - pts[:, 1].mean() <= 60:
                out.append(i)
                break
    return out


def write_mpsetup(setup, mp, stan, bg, objects_for=None):
    """objects_for(first_index) gives more objects for the props list, whose
    commands start at that index."""
    pads = setup['pads']
    if mp and mp['spawns']:
        spawns = mp['spawns']
        weapons = mp['weapons']
        ammo = mp['ammo']
    else:
        ok = floored_pads(pads, stan, bg)
        chosen = [ok[i] for i in spread([pads[i]['pos'] for i in ok], 28)]
        spawns = chosen[:12]
        weapons = [(p, i % 6) for i, p in enumerate(chosen[12:24])]
        ammo = chosen[24:28]
    intro = b''.join(struct.pack('>iii', 0, p, 0) for p in spawns) + struct.pack('>i', 0x0c)
    props = b''
    for padnum, loc in weapons:
        props += struct.pack('>23I', (0x0100 << 16) | 0x08, padnum & 0xffff, 1, 0, 0, *([0] * 14), 1000, 0, 0, 0x0fff0000)
        props += struct.pack('>3I', ((0xf0 + loc) << 24), 0x00ffffff, 0)
    for padnum in ammo:
        props += struct.pack('>23I', (0x00cc << 16) | 0x14, (0x00c1 << 16) | (padnum & 0xffff), 1, 0, 0, *([0] * 14), 1000, 0, 0, 0x0fff0000)
        props += struct.pack('>19I', *([0xffff0000] * 19))
    if objects_for:
        props += b''.join(objects_for(len(weapons) + len(ammo)))
    props += struct.pack('>I', 0x34)
    header_len = 0x20
    intro_at = header_len
    props_at = intro_at + len(intro)
    paths_at = props_at + len(props)
    paths = struct.pack('>IBBH', 0, 0, 0, 0)
    ai_at = paths_at + len(paths)
    ai_code_at = ai_at + 12
    lists = struct.pack('>Ii', ai_code_at, 0x1000) + struct.pack('>Ii', ai_code_at + len(AI_1000), 0x1001) + struct.pack('>Ii', 0, 0)
    ai_code_at = ai_at + len(lists)
    lists = struct.pack('>Ii', ai_code_at, 0x1000) + struct.pack('>Ii', ai_code_at + len(AI_1000), 0x1001) + struct.pack('>Ii', 0, 0)
    code = AI_1000 + AI_1001
    out = struct.pack('>8I', 0, 0, 0, intro_at, props_at, paths_at, ai_at, 0)
    out += intro + props + paths + lists + code
    return rzip1173(pad(out, 16)), len(spawns), len(weapons), len(ammo)


# ---------------------------------------------------------------------------

def fog_rows():
    """GoldenEye's one-player fog table rows (US), by level id name.

    Columns: id, near, far, fade opaque, fade translucent, fade reference, (unused),
    (intensity), fog min, fog max, sky rgb, clouds, cloud repeat, cloud image,
    (reserved), cloud rgb, water, (pad x3), water repeat, water image,
    (reserved), water rgb, water concavity. Perfect Dark's fogenvironment has the
    same fields: near far opaperc xluperc refdist fogmin fogmax sky, clouds
    enabled/scale/type/colour, water enabled/scale/type/colour, clouds_height."""
    return gefiles.rom().fog_rows()


def fog_value(r, offset):
    """A fog row as the maps block's `fog` string (modloader.c).

    The cloud and water repeats are the heights of their planes in world units
    (sky.c takes the camera's y from them), so they move with the level: Dam
    re-centred 13219 up put the camera over its clouds, which drew black."""
    r = list(r)
    r[13] -= offset[1]
    r[23] -= offset[1]
    i = lambda k: int(round(r[k - 1]))
    rgb = lambda a: '%02x%02x%02x' % (i(a), i(a + 1), i(a + 2))
    return '%d %d %d %d %d %d %d %s %d %d %d %s %d %d %d %s %d' % (
        i(1), i(2), i(3), i(4), i(5), i(8), i(9), rgb(10),
        i(13), i(14), i(15), rgb(17), i(20), i(24), i(25), rgb(27), i(30))


def main():
    outdir = sys.argv[1]
    keys = sys.argv[2:] or list(gefiles.LEVELS)
    os.makedirs(os.path.join(outdir, 'files/bgdata'), exist_ok=True)
    os.makedirs(os.path.join(outdir, 'textures'), exist_ok=True)
    maps = []
    fogs = fog_rows()
    alltex = set()
    allmodels = set()
    record = {}
    for key in keys:
        bgname, stanname, solo, mpname, ls = gefiles.LEVELS[key]
        bg = gefiles.Bg(gefiles.rom_file(bgname))
        stan = read_stan(gefiles.rom_file(stanname))
        # The level is moved so the middle of its walkable area is the origin:
        # tiles are s16 world coordinates, and Dam's run to 44683 from
        # GoldenEye's own origin
        sp = np.array([q[:3] for t in stan for q in t['points']], float) / ls
        offset = np.round((sp.min(0) + sp.max(0)) / 2)
        bgdata, tex, numlights = write_bg(bg, ls, offset)
        alltex.update(tex)
        tiles, walls = write_tiles(stan, bg.numrooms, ls, offset)
        setup = read_setup(gefiles.rom_file(solo or mpname))
        mp = read_setup(gefiles.rom_file(mpname)) if mpname else None
        if mp:
            setup = mp
        rooms = geobjects.Rooms(stan, ls, offset, bg)
        # the objects come from the setup the pads do: the multiplayer one where
        # there is one
        gedata = gefiles.rom_file(mpname or solo)
        boundpads = geobjects.bound_pads(gedata, ls, offset)
        padsdata = write_pads(setup, ls, offset, rooms, None, None, boundpads)
        short = 'gx' + key
        objs = []
        # GoldenEye's own objects, the remake's own models: a stock body
        # armour model for Caves's armour
        bikepads = bike_pads(key, setup, stan, bg, gedata)
        def objects_for(first):
            got, used = geobjects.objects(gedata, len(setup['pads']), first, bodyarmour=0x182, bikepads=bikepads)
            objs.extend(got)
            allmodels.update(used)
            return got
        mpsetup, nsp, nw, na = write_mpsetup(setup, mp, stan, bg, objects_for)
        print('%-5s %d objects, %d bound pads' % (key, len(objs), len(boundpads)))
        record[key] = dict(bg=os.path.join(outdir, 'files/bgdata/bg_%s.seg' % short), levelscale=ls,
                           offset=[float(x) for x in offset])
        files = {'bgdata/bg_%s.seg' % short: bgdata, 'bgdata/bg_%s_tilesZ' % short: tiles,
                 'bgdata/bg_%s_padsZ' % short: padsdata, 'Ump_setup%sZ' % short: mpsetup}
        for rel, data in files.items():
            with open(os.path.join(outdir, 'files', rel), 'wb') as f:
                f.write(data)
        maps.append('  map "%s" bg "bgdata/bg_%s.seg" tiles "bgdata/bg_%s_tilesZ" pads "bgdata/bg_%s_padsZ" mpsetup "Ump_setup%sZ"%s' % (
            NAMES[key], short, short, short, short,
            (' fog "%s"' % fog_value(fogs[LEVELIDS[key]], offset) if LEVELIDS[key] in fogs else '')))
        print('%-5s lights %d' % (key, numlights))
        print('%-5s rooms %3d portals %3d textures %3d tiles %4d (+%d walls) pads %3d waypoints %3d spawns %2d weapons %2d ammo %2d  bg %d bytes' % (
            key, bg.numrooms, len(bg.portals), len(tex), len(stan), walls, len(setup['pads']), len(setup['waypoints']), nsp, nw, na, len(bgdata)))
    # GE-X Plus's menus are GoldenEye's own folder screens (port/src/gexfront.c):
    # the folder is a prop model, the cursor and the stage pictures global images,
    # and the fonts, the music and the title screen's strings are copied as
    # GoldenEye stores them
    allmodels.add(MENU_FOLDER_MODEL)
    alltex.update(MENU_IMAGES)
    os.makedirs(os.path.join(outdir, 'menu'), exist_ok=True)
    rom = gefiles.rom()
    for name, at, size in MENU_RAW:
        with open(os.path.join(outdir, 'menu', name), 'wb') as f:
            f.write(rom.rom[at:at + size])
    with open(os.path.join(outdir, 'menu', 'LtitleE'), 'wb') as f:
        f.write(gefiles.rom_file('LtitleE'))
    # and the solo missions' briefings, with the text bank each one indexes
    for names in MENU_TEXT:
        for name in names:
            with open(os.path.join(outdir, 'menu', name), 'wb') as f:
                f.write(gefiles.rom_file(name))
    # the remake's prop models: GoldenEye's own, converted (gemodelconv.py)
    modellines = []
    for num in sorted(allmodels):
        data, images, scale = gemodelconv.convert(num)
        alltex.update(images)
        fname = 'Pgx%03dZ' % num
        with open(os.path.join(outdir, 'files', fname), 'wb') as f:
            f.write(rzip1173(data))   # a model file is stored compressed, as the ROM's are
        modellines.append('  %d "%s" %d' % (num, fname, round(scale * 4096)))
    print('models written %d' % len(modellines))
    missing = []
    for num in sorted(alltex):
        data = gefiles.rom().image(num)
        if data is None or len(data) > 4096:
            missing.append(num)
            continue
        with open(os.path.join(outdir, 'textures/%04x.bin' % texremap.remap(num)), 'wb') as f:
            f.write(data)
    with open(os.path.join(outdir, 'modconfig.txt'), 'w') as f:
        f.write('# GoldenEye levels converted from the GoldenEye ROM (port/src/geconvert.c, tools/geconvert)\nmaps {\n%s\n}\n' % '\n'.join(maps))
        f.write('# GoldenEye\'s prop models: slot (GoldenEye model number), file, scale (4096 = 1.0)\nmodels {\n%s\n}\n' % '\n'.join(modellines))
    print('textures written %d, missing %s' % (len(alltex) - len(missing), missing))
    # For gen_stagetable.py: world = GoldenEye bg units / levelscale - offset
    import json
    rec = os.environ.get('GE_ARENAS_JSON')
    if not rec:
        return
    old = json.load(open(rec)) if os.path.exists(rec) else {}
    old.update(record)
    json.dump(old, open(rec, 'w'), indent=1)


if __name__ == '__main__':
    main()

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
- portals: GoldenEye's, scaled, with the front room written in room2 as
  Perfect Dark reads it; the visibility commands are not (an END only)
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
import gerom
import geobjects
import gesolo
import texremap
import gemodelconv
import gechr
import geanim
import geanimtable

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
              ('sequences', 0x419790, 0x1eed0),
              # and the gun barrel's sniper-sight backdrop, the folder screens'
              # 440x299 8-bit background run-length encoded ({u16 w, u16 h, six
              # bytes, then count/value pairs}, rle.c's rle_expand_8bit)
              ('introbg.bin', 0x2a4d50, 107890))
# the gun barrel's blood, in the data segment rather than the ROM: the frames of
# the wash down the lens, each decoded from the last (blood_decrypt.c)
INTRO_BLOOD_AT = 0xada0
INTRO_BLOOD_SIZE = 2524
# GE Plus's intro (port/src/geintro.c) is GoldenEye's own: the gun barrel, the
# GoldenEye logo and the cast reel. It needs the logo's model, the guns the cast
# holds and the PP7 Bond fires (PROP_CHRWPPK, 191), every character in the ROM
# (gechr.py writes all 80 - the cast is 33 bodies and a head pool of 33, and the
# rest cost 430KB together), and these animations, whose records are where
# GoldenEye's animation_data segment has them. The order is the order the port
# reads them in (`menu/intro.bin` names each one, so nothing depends on it).
INTRO_LOGO_MODEL = 277
INTRO_GUNS = (184, 185, 187, 188, 190, 191, 193, 195, 197, 204, 205, 207, 208, 210)
INTRO_ANIMS = (
    # the gun barrel: Bond walks in, turns and fires
    ('bond_eye_walk', 0x292ac4), ('bond_eye_fire', 0x292c18),
    # the cast reel: front.c's intro_animation_table, and idle for a character
    # whose animation is still loading
    ('idle', 0x28e99c), ('spotting_bond', 0x294690),
    ('fire_standing_draw_fast', 0x294bd4), ('fire_standing_draw_slow', 0x294cfc),
    ('fire_step_right', 0x295188), ('fire_kneel_forward_fast', 0x2956d0),
    ('running_one_handed', 0x2960fc), ('draw_and_stand_up', 0x296428),
    ('aim_left_right', 0x2965cc), ('cock_and_turn_around', 0x296684),
    ('cock_turn_stand_up', 0x29688c), ('draw_and_turn_around', 0x296934),
    ('drop_weapon_fight', 0x299af4), ('laughing', 0x29ad90),
    ('fire_hip_forward', 0x294fc4), ('fire_standing_left_fast', 0x295398),
    ('fire_kneel_left_fast', 0x295c84), ('draw_and_look_around', 0x296248),
    ('aim_left', 0x2992cc), ('aim_right', 0x29935c),
    ('conversation', 0x29962c), ('conversation_listener', 0x29a900),
    ('conversation_cleaned', 0x29a5c0))

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

# GoldenEye's twenty solo missions in the folder's order: the level they are on
# and the setup file that is the mission. Surface and Bunker are two missions
# each on one level, and mission n is what gexfront.c's folder starts.
MISSIONS = (('dam', 'UsetupdamZ', 'Dam'), ('ark', 'UsetuparkZ', 'Facility'),
            ('run', 'UsetuprunZ', 'Runway'), ('sevx', 'UsetupsevxZ', 'Surface'),
            ('sev', 'UsetupsevbunkerZ', 'Bunker'), ('silo', 'UsetupsiloZ', 'Silo'),
            ('dest', 'UsetupdestZ', 'Frigate'), ('sevxb', 'UsetupsevxbZ', 'Surface 2'),
            ('sevb', 'UsetupsevbZ', 'Bunker 2'), ('stat', 'UsetupstatueZ', 'Statue Park'),
            ('arch', 'UsetuparchZ', 'Archives'), ('pete', 'UsetuppeteZ', 'Streets'),
            ('depo', 'UsetupdepoZ', 'Depot'), ('tra', 'UsetuptraZ', 'Train'),
            ('jun', 'UsetupjunZ', 'Jungle'), ('arec', 'UsetupcontrolZ', 'Control'),
            ('cave', 'UsetupcaveZ', 'Caverns'), ('crad', 'UsetupcradZ', 'Cradle'),
            ('azt', 'UsetupaztZ', 'Aztec'), ('cryp', 'UsetupcrypZ', 'Egyptian'))

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


def scaled_room(room, inv, offset, tilebox=None):
    """The room's vertices at world scale, and a room position that keeps
    them inside an s16. A room that draws nothing (Streets files thirty-five
    of them) has no vertices to sit among, so it takes the middle of its own
    tiles: GoldenEye's own room position for those is the level origin, which
    is nowhere near them."""
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
    elif tilebox is not None:
        centre = np.round((np.array(tilebox[0], float) + np.array(tilebox[1], float)) / 2)
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


def write_room(room, inv, offset, base_ptr, textures, lightsindex=0, tilebox=None):
    vtx, centre, world = scaled_room(room, inv, offset, tilebox)
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
    if tilebox is not None:
        # The room holds the tiles GoldenEye files under it, which need not be
        # inside what the room draws - and a room that draws nothing has a
        # bbox of a point. bgFindRoomsByPos() answers from these boxes, and it
        # is what bwalkUpdateVertical() asks when the rooms a walker carries
        # hold no floor under them (bondwalk.c, "fell through right here on
        # runway"), so a room whose box misses its own floor cannot be found.
        for c in range(3):
            bbox[c] = max(-32768, min(bbox[c], int(math.floor(tilebox[0][c] - centre[c]))))
            bbox[3 + c] = min(32767, max(bbox[3 + c], int(math.ceil(tilebox[1][c] - centre[c]))))
    return data, centre, bbox, lights


# A portal's front side, and the room the conversion files on it.
PORTAL_NEAR = 300.0      # a tile or vertex this close to a portal speaks for its room
PORTAL_EPS = 1.0         # a point this close to the plane says nothing
PORTAL_MARGIN = 40.0     # how far the room's own geometry must clear the plane


def portal_metric(v):
    """The normal a portal's winding gives it, and the slab its vertices span,
    exactly as bg.c works them out at the load (g_PortalMetrics in bgSetup())."""
    n = np.zeros(3)
    for j in range(len(v)):
        w = v[(j + 1) % len(v)]
        n[0] += (v[j][1] - w[1]) * (v[j][2] + w[2])
        n[1] += (v[j][2] - w[2]) * (v[j][0] + w[0])
        n[2] += (v[j][0] - w[0]) * (v[j][1] + w[1])
    d = -np.sqrt(n.dot(n))
    if d == 0.0:
        return None
    n = n / d
    vals = v.dot(n)
    return n, float(vals.min()), float(vals.max())


def portal_side(points, n, mid, at, keep):
    """Which side of a portal's plane a room's points near it lie on, as the
    median signed distance of the ones that speak. `keep` is how many of the
    nearest to fall back on when too few are within PORTAL_NEAR."""
    if not len(points):
        return None
    d = np.linalg.norm(points - at, axis=1)
    pick = d <= PORTAL_NEAR
    if pick.sum() < 3:
        pick = np.zeros(len(points), bool)
        pick[np.argsort(d, kind='stable')[:min(keep, len(points))]] = True
    s = points[pick].dot(n) - mid
    s = s[np.abs(s) > PORTAL_EPS]
    if not len(s):
        return None
    return float(np.median(s))


def room_world_vtx(room, inv, offset):
    """A room's own vertices at world scale - scaled_room()'s `world`."""
    vtx = room['vtx'] or b''
    n = len(vtx) // 16
    if not n:
        return np.zeros((0, 3))
    pts = np.array([struct.unpack_from('>3h', vtx, 16 * k) for k in range(n)], float).reshape(-1, 3)
    return (pts + np.array(room['pos'], float)) * inv - offset


def portal_room_order(bg, inv, offset, stan):
    """Which of a portal's two rooms goes in room2: the one on its front.

    Perfect Dark takes the room on the front of a portal's normal to be room2
    (bg.c bgTestPosInRoomCheap, and the camera's side in the renderer's snake),
    and GoldenEye's own record does not: over the twenty-six levels its room1
    is the front room 668 times and its room2 1068, which is a coin toss.
    Nearly all of that is repaired at the load, where bgInitPortal() swaps the
    two when room1's *centre* is in front of the plane - the file's order only
    survives where both centres fall on the front, and there it is right. Seven
    portal records in the game are backwards in play even so: Facility's two
    into the hole at 12/15, Archives' two at 41/42, and the 85/12 that the
    three Bunker 2 levels share.

    Each room's own geometry settles it. The tiles either side of a doorway
    are on the sides their rooms are, and where a room has none - or both
    rooms' tiles fall one side, as Runway's 13/14 does - the room's drawn
    vertices say the same thing (given PORTAL_MARGIN they agree with the tiles
    662 times out of 662 where both speak; at 20 they part twice, and one of
    those two was Depot's 27/18, where a wall reaches 27 units past the plane
    and the room's bulk is plainly the other side). Where neither says
    anything, GoldenEye's own order is kept: bgInitPortal() will have it.
    """
    centroids = {}
    for t in stan:
        if len(t['points']) >= 3:
            c = np.mean([[x * inv - offset[0], y * inv - offset[1], z * inv - offset[2]]
                         for x, y, z, _ in t['points']], axis=0)
            centroids.setdefault(t['room'], []).append(c)
    centroids = {r: np.array(v) for r, v in centroids.items()}
    worlds = {}
    out = []
    for p in bg.portals:
        r1, r2 = p['room1'], p['room2']
        v = np.array([np.array(q, float) * inv - offset for q in p['points']])
        m = portal_metric(v)
        if m is None:
            out.append((r1, r2))
            continue
        n, lo, hi = m
        at = v.mean(0)
        mid = (lo + hi) / 2.0
        front = None
        s1 = portal_side(centroids.get(r1, np.zeros((0, 3))), n, mid, at, 5)
        s2 = portal_side(centroids.get(r2, np.zeros((0, 3))), n, mid, at, 5)
        if s1 is not None and s2 is not None and (s1 > 0) != (s2 > 0):
            front = r1 if s1 > 0 else r2
        else:
            for r in (r1, r2):
                if r not in worlds:
                    worlds[r] = room_world_vtx(bg.rooms[r - 1], inv, offset) if 1 <= r <= bg.numrooms \
                            else np.zeros((0, 3))
            d1 = portal_side(worlds[r1], n, mid, at, 20)
            d2 = portal_side(worlds[r2], n, mid, at, 20)
            if (d1 is not None and d2 is not None and (d1 > 0) != (d2 > 0)
                    and min(abs(d1), abs(d2)) > PORTAL_MARGIN):
                front = r1 if d1 > 0 else r2
        if front == r1:
            r1, r2 = r2, r1
        out.append((r1, r2))
    return out


def write_bg(bg, ls, offset, tilebounds=None, stan=None):
    inv = 1.0 / ls
    textures = set()
    n = bg.numrooms
    # primary: header, room table (n + 3 entries), bgcmds, portals, portal vertices
    table_at = 0x18
    # the rooms are converted first, since their lights go in the primary data
    converted = []
    alllights = []
    for r, room in enumerate(bg.rooms, 1):
        converted.append(write_room(room, inv, offset, 0, textures, len(alllights),
                                    tilebounds[r] if tilebounds else None))
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
    order = portal_room_order(bg, inv, offset, stan) if stan is not None else [
            (p['room1'], p['room2']) for p in bg.portals]
    for i, p in enumerate(bg.portals):
        portals += struct.pack('>HhhBx', i + 1, order[i][0], order[i][1], 0)
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
        data, centre, bbox, roomlights = write_room(room, inv, offset, ptr, textures, lightsindex,
                                                    tilebounds[r] if tilebounds else None)
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
WALL_ABOVE = 400.0  # and above, where nothing walkable stands in the way

# The geo flags a floor tile and a wall carry (constants.h, GEOFLAG_*). Perfect
# Dark reads a tile's flags for the question being asked and nothing else -
# cdCollectGeoForCylFromList() takes a tile only where `geo->flags & geoflags` -
# so a wall that carries GEOFLAG_WALL alone stops a body walking into it and is
# not there at all to a line of sight (GEOFLAG_BLOCK_SIGHT, chrHasLosToChr) or
# to a bullet (GEOFLAG_BLOCK_SHOOT). GoldenEye's own sight is its stan graph,
# which walks from link to link (chrCanSeeBond's stanTestLineUnobstructed) and
# so cannot cross an unlinked edge either: a wall standing on one blocks sight
# and shots there in GoldenEye too, and the walls carry both.
FLOOR_FLAGS = 0x0001 | 0x0002 | 0x0008 | 0x0010   # FLOOR1 FLOOR2 SIGHT SHOOT
WALL_FLAGS = 0x0004 | 0x0008 | 0x0010             # WALL SIGHT SHOOT

# A wall is raised round every unlinked tile edge, and GoldenEye's own walls
# are the edge and nothing more: its collision walks out from the tile the
# player stands on through the links alone (stan.c's sub_GAME_7F0B1DDC), so an
# edge belonging to another storey's floor can never stop them. Perfect Dark's
# is a quad in the world, so a wall raised WALL_ABOVE over a staircase's own
# tiles stands in the air across the flight above it, and one dropped
# WALL_BELOW under a ledge's own tiles stands in the way of a player walking
# beneath. Every wall stops under the lowest walkable surface that passes over
# it, and over the head of anyone standing on one that passes under it.
WALL_HEADROOM = 60.0  # a surface this far from the edge is another floor, not
                      # the step or kerb the wall itself belongs to
WALL_STEP = 20.0      # world units between the samples along an edge
WALL_SIDE = 4.0       # the slack on the box a surface is looked for in
WALL_REACH = 30.0     # the player's own radius: they stand this far from a
                      # wall, and on a slope that is lower ground than the
                      # surface over the wall itself
WALL_HEAD = 160.0     # and their collision box reaches this far over the floor
                      # they stand on (playerGetBbox(): a chr's is less)
WALL_CLEAR = 2.0      # the foot goes this much further, since the collision's
                      # own comparison against a tile's ymin is inclusive
WALL_RISE = 50.0      # a wall's foot may be lifted this far over its own edge
                      # and still meet the box of a walker on its own tile.
                      # However deep a player crouches - and they crouch twice
                      # - playerGetBbox() holds their box at manground+30 to
                      # manground+80 at the least, and the deepest a chr ducks
                      # is chr->height 90 over manground+20. A lift that would
                      # have to go further clears nobody, so it is not made at
                      # all: the surface under such a wall is a platform beside
                      # its own tile rather than a floor under it, and a player
                      # beside a platform belongs against its side


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


def room_tile_bounds(stan, numrooms, ls, offset):
    """Each room's own tiles in world units, with the head and foot room of
    the walls write_tiles() raises round every unlinked tile edge: the box a
    player standing on this room's floor is inside."""
    inv = 1.0 / ls
    out = [None] * (numrooms + 2)
    for t in stan:
        if t['room'] < 1 or t['room'] > numrooms:
            continue
        for x, y, z, _ in t['points']:
            p = (s16(x * inv - offset[0]), s16(y * inv - offset[1]), s16(z * inv - offset[2]))
            b = out[t['room']]
            if b is None:
                out[t['room']] = [list(p), list(p)]
            else:
                for c in range(3):
                    b[0][c] = min(b[0][c], p[c])
                    b[1][c] = max(b[1][c], p[c])
    for b in out:
        if b is not None:
            b[0][1] -= WALL_BELOW
            b[1][1] += WALL_ABOVE
    return out


def tile_surface_y(pts, x, z):
    """The tile's own surface at x/z, from the fan triangle that holds the
    point, or None where the point is outside the tile."""
    a = pts[0]
    for k in range(1, len(pts) - 1):
        b, c = pts[k], pts[k + 1]
        det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if det == 0.0:
            continue
        w0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det
        w1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det
        w2 = 1.0 - w0 - w1
        if w0 >= 0.0 and w1 >= 0.0 and w2 >= 0.0:
            return w0 * a[1] + w1 * b[1] + w2 * c[1]
    return None


def wall_span(world, bbox, self_i, a, b):
    """How far a wall on this edge may rise and how far it may reach down: up
    to the lowest walkable surface that passes over it, and down to the head of
    a player standing on the highest one that passes under it."""
    dx, dz = b[0] - a[0], b[2] - a[2]
    length = math.sqrt(dx * dx + dz * dz)
    reach = WALL_REACH + WALL_SIDE
    cand = np.nonzero((bbox[:, 0] <= max(a[0], b[0]) + reach)
                      & (bbox[:, 1] >= min(a[0], b[0]) - reach)
                      & (bbox[:, 2] <= max(a[2], b[2]) + reach)
                      & (bbox[:, 3] >= min(a[2], b[2]) - reach))[0]
    above, below = WALL_ABOVE, WALL_BELOW
    # the quad blocks between its own lowest and highest vertex, whatever its
    # corners are (cdCollectGeoForCylFromList() reads the tile's ymin/ymax), so
    # what has to stay under the surface over it is its higher end, and what
    # has to stay over the one under it is its lower
    top, foot = max(a[1], b[1]), min(a[1], b[1])
    # a riser's own side is an edge that goes straight down, and a point of a
    # wall in plan; Perfect Dark blocks within the player's radius of one all
    # the same, so it takes the one sample at its own place
    nsteps = int(length / WALL_STEP) + 1 if length >= 1.0 else 1
    for s in range(nsteps):
        f = (s + 1) / (nsteps + 1)
        px, pz = a[0] + dx * f, a[2] + dz * f
        for j in cand:
            if j == self_i:
                continue
            # the player stands anywhere within their own radius of the wall,
            # and where the surface reaches over any of that it is what the
            # wall has to keep clear of: its lowest for the surface over the
            # wall, its highest for the one under it
            low = high = None
            for ox, oz in ((0.0, 0.0), (WALL_REACH, 0.0), (-WALL_REACH, 0.0),
                           (0.0, WALL_REACH), (0.0, -WALL_REACH)):
                y = tile_surface_y(world[j], px + ox, pz + oz)
                if y is None:
                    continue
                if low is None or y < low:
                    low = y
                if high is None or y > high:
                    high = y
            if low is None:
                continue
            gap = low - top
            if WALL_HEADROOM <= gap < above:
                above = gap
            drop = foot - high - WALL_HEAD - WALL_CLEAR
            if high + WALL_HEADROOM <= foot and -WALL_RISE <= drop < below:
                below = drop
    return above, below


def write_tiles(stan, numrooms, ls, offset):
    inv = 1.0 / ls
    rooms = [[] for _ in range(numrooms + 1)]
    walls = 0
    world = [[(x * inv - offset[0], y * inv - offset[1], z * inv - offset[2])
              for x, y, z, _ in t['points']] for t in stan]
    bbox = np.array([[min(p[0] for p in w), max(p[0] for p in w),
                      min(p[2] for p in w), max(p[2] for p in w)] for w in world], float)
    for ti, t in enumerate(stan):
        pts = world[ti]
        flags = FLOOR_FLAGS
        if t['special'] == 3:
            flags |= 0x0040
        rooms[t['room']].append((flags, pts))
        n = len(t['points'])
        for i in range(n):
            link = t['points'][i][3]
            if link >> 4:
                continue
            a, b = pts[i], pts[(i + 1) % n]
            above, below = wall_span(world, bbox, ti, a, b)
            quad = [(a[0], a[1] - below, a[2]), (b[0], b[1] - below, b[2]),
                    (b[0], b[1] + above, b[2]), (a[0], a[1] + above, a[2])]
            rooms[t['room']].append((WALL_FLAGS, quad))
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
        lengths = {0: 3, 1: 4, 2: 4, 3: 8, 4: 2, 5: 2, 6: 10, 7: 3, 8: 2}
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
            # GoldenEye stands a pad on the floor and is happy with that;
            # Perfect Dark's ground search takes the highest floor *strictly*
            # below the position it is handed and answers -2^32 for none, so a
            # pad exactly level with its floor has no ground under it at all.
            # The player spawned on one of those in Egyptian, Control and
            # Caverns and fell out of the world. One unit - a centimetre - is
            # the whole of the lift, and only 63 of the 5616 pads take it.
            fy = rooms.floor(world)
            if fy is not None and fy >= world[1]:
                world = world.copy()
                world[1] = fy + 1
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
    missions = []
    fogs = fog_rows()
    alltex = set()
    allmodels = set()
    allanims = set()
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
        tilebounds = room_tile_bounds(stan, bg.numrooms, ls, offset)
        bgdata, tex, numlights = write_bg(bg, ls, offset, tilebounds, stan)
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
        files = {}
        mpsetup, nsp, nw, na = write_mpsetup(setup, mp, stan, bg, objects_for)
        print('%-5s %d objects, %d bound pads' % (key, len(objs), len(boundpads)))
        # and the solo mission on this level, where there is one: its own pads
        # (the mission's pad list is not the arena's) and its own setup, over
        # the same rooms and tiles
        for mkey, msetup, mname in MISSIONS:
            if mkey != key:
                continue
            md = gefiles.rom_file(msetup)
            msetupdata = read_setup(md)
            mboundpads = geobjects.bound_pads(md, ls, offset)
            mpads = write_pads(msetupdata, ls, offset, rooms, None, None, mboundpads)
            mprops, mmodels, mstats = gesolo.convert(md, len(msetupdata['pads']), gesolo.STOCK_BODIES, ls, offset)
            allmodels.update(mmodels)
            allanims.update(mstats['anims'])
            files['bgdata/bg_gs%s_padsZ' % mkey] = mpads
            files['Usetupgs%sZ' % mkey] = rzip1173(pad(mprops, 16))
            missions.append('  mission %d "%s" bg "bgdata/bg_%s.seg" tiles "bgdata/bg_%s_tilesZ"'
                            ' pads "bgdata/bg_gs%s_padsZ" setup "Usetupgs%sZ"%s' % (
                                [m[0] for m in MISSIONS].index(mkey), mname, short, short, mkey, mkey,
                                (' fog "%s"' % fog_value(fogs[LEVELIDS[key]], offset)
                                 if LEVELIDS[key] in fogs else '')))
            print('%-5s mission %-12s props %4d (+%d) pads %3d ai %5d (+%d) unknown %d' % (
                key, mname, sum(mstats['kept'].values()), sum(mstats['dropped'].values()),
                len(msetupdata['pads']), mstats['ai_kept'], sum(mstats['ai_dropped'].values()),
                mstats['ai_unknown']))
        record[key] = dict(bg=os.path.join(outdir, 'files/bgdata/bg_%s.seg' % short), levelscale=ls,
                           offset=[float(x) for x in offset])
        files.update({'bgdata/bg_%s.seg' % short: bgdata, 'bgdata/bg_%s_tilesZ' % short: tiles,
                      'bgdata/bg_%s_padsZ' % short: padsdata, 'Ump_setup%sZ' % short: mpsetup})
        for rel, data in files.items():
            with open(os.path.join(outdir, 'files', rel), 'wb') as f:
                f.write(data)
        maps.append('  map "%s" bg "bgdata/bg_%s.seg" tiles "bgdata/bg_%s_tilesZ" pads "bgdata/bg_%s_padsZ" mpsetup "Ump_setup%sZ"%s' % (
            NAMES[key], short, short, short, short,
            (' fog "%s"' % fog_value(fogs[LEVELIDS[key]], offset) if LEVELIDS[key] in fogs else '')))
        print('%-5s lights %d' % (key, numlights))
        print('%-5s rooms %3d portals %3d textures %3d tiles %4d (+%d walls) pads %3d waypoints %3d spawns %2d weapons %2d ammo %2d  bg %d bytes' % (
            key, bg.numrooms, len(bg.portals), len(tex), len(stan), walls, len(setup['pads']), len(setup['waypoints']), nsp, nw, na, len(bgdata)))
    # GE Plus's menus are GoldenEye's own folder screens (port/src/gexfront.c):
    # the folder is a prop model, the cursor and the stage pictures global images,
    # and the fonts, the music and the title screen's strings are copied as
    # GoldenEye stores them
    allmodels.add(MENU_FOLDER_MODEL)
    allmodels.add(INTRO_LOGO_MODEL)
    allmodels.update(INTRO_GUNS)
    alltex.update(MENU_IMAGES)
    os.makedirs(os.path.join(outdir, 'menu'), exist_ok=True)
    rom = gefiles.rom()
    for name, at, size in MENU_RAW:
        with open(os.path.join(outdir, 'menu', name), 'wb') as f:
            f.write(rom.rom[at:at + size])
    with open(os.path.join(outdir, 'menu', 'LtitleE'), 'wb') as f:
        f.write(gefiles.rom_file('LtitleE'))
    with open(os.path.join(outdir, 'menu', 'introblood.bin'), 'wb') as f:
        f.write(rom.data[INTRO_BLOOD_AT:INTRO_BLOOD_AT + INTRO_BLOOD_SIZE])
    # and the solo missions' briefings, with the text bank each one indexes
    for names in MENU_TEXT:
        for name in names:
            with open(os.path.join(outdir, 'menu', name), 'wb') as f:
                f.write(gefiles.rom_file(name))
    # the intro's characters: every one in the ROM, converted (gechr.py), and
    # its animations in one file with a table naming each (geanim.py)
    for num in range(gerom.NUM_CHRS):
        data, images, scale = gechr.convert(num)
        alltex.update(images)
        with open(os.path.join(outdir, 'files', 'Cgx%03dZ' % num), 'wb') as f:
            f.write(rzip1173(data))
    # menu/intro.bin: "GEI2", the characters' scales, then a row an animation
    # (its 32-byte name, Perfect Dark's animtableentry fields, and where its
    # bytes are). The name is what the port matches the row by, and a 20-byte
    # field truncated ten of the twenty-five - two of them to the same 19
    # characters - so it has to be wide enough for the longest of them
    rows, blob = [], bytearray()
    for name, at in INTRO_ANIMS:
        data, e = geanim.convert(at)
        rows.append((name, e, len(blob), len(data)))
        blob += data
    # a character: its number, whether it is male and whether it wears a head of
    # its own (c_item_entries' flags), and its scale
    chrs = b''.join(struct.pack('>HHf', num, h['ismale'] | (h['hashead'] << 1), scale)
                    for num, (_, scale, h) in enumerate(gefiles.rom().chrs()))
    base = 8 + len(chrs) + 48 * len(rows)
    index = b''.join(struct.pack('>32sHHHBBII', name.encode()[:31], e['numframes'], e['bytesperframe'],
                                 e['headerlen'], e['framelen'], e['looping'], base + off, size)
                     for name, e, off, size in rows)
    with open(os.path.join(outdir, 'menu', 'intro.bin'), 'wb') as f:
        f.write(struct.pack('>4sHH', b'GEI2', gerom.NUM_CHRS, len(rows)) + chrs + index + bytes(blob))
    print('characters written %d, animations %d in %d bytes' % (gerom.NUM_CHRS, len(INTRO_ANIMS), len(blob)))
    # menu/geanims.bin: the animations the missions' PlayAnimation commands
    # name (geanimtable.py), each under GoldenEye's own id. The port appends
    # them to Perfect Dark's table and gives the id its number there
    # (gexplusanim.c), which is what the converted aiChrDoAnimation asks for.
    rows, blob = [], bytearray()
    for anim in sorted(allanims):
        name, at = geanimtable.TABLE[anim]
        data, e = geanim.convert(geanimtable.BASE + at)
        rows.append((anim, e, len(blob), len(data)))
        blob += data
    base = 8 + 20 * len(rows)
    index = b''.join(struct.pack('>HHHHBBxxII', anim, e['numframes'], e['bytesperframe'], e['headerlen'],
                                 e['framelen'], e['looping'], base + off, size)
                     for anim, e, off, size in rows)
    with open(os.path.join(outdir, 'menu', 'geanims.bin'), 'wb') as f:
        f.write(struct.pack('>4sHH', b'GEA1', len(rows), 0) + index + bytes(blob))
    print('mission animations %d in %d bytes' % (len(rows), len(blob)))
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
        # GoldenEye's own twenty solo missions, in the folder's order
        f.write('# GoldenEye\'s solo missions, in its own mission order (port/src/gexfront.c)\nmissions {\n%s\n}\n' % '\n'.join(missions))
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

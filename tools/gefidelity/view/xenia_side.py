"""The view tour's GoldenEye side on the Xbox 360 release ("Bean", in Xenia,
through common/xeniage.py and the locked rig in xenia/). tour.py uses these
when xenia/run_scenario.py runs it with xeniage standing in for gdbge.

Bean keeps GoldenEye's setup records, tile graphs and world coordinates byte
for byte; its player struct is 4J's (xeniage.py has the offsets found), with
GoldenEye's collision434 inside it unchanged. So, as on the cartridge, nothing
is called in the game:

- Bond goes onto the pad's own tile at the tile's height (stanGetPositionYValue,
  with the level's scale from GoldenEye's levelinfotable, bg.c:195) plus his
  eye height, written where change_player_pos_to_target() writes; Bean's eye
  height is measured on the spawn tile (its player struct does not have the
  N64's eyeheight where the N64 has it).
- Guards' AI lists (ChrRecord + 260, the N64 layout Bean keeps) are cleared.
- Look Ahead is off (Bean's settings bit 0x80).
- The picture is xeniage.shot(): the frame the paused game is presenting,
  1280x695 under Xenia's menu bar.
"""
import math, os, struct

import gdbge as lib   # xeniage, through xenia/run_scenario.py

HERE = os.path.dirname(os.path.abspath(__file__))

# GoldenEye's levelinfotable levelscale (bg.c:195), by the decomp's LEVELID name
LEVELSCALE = {'LEVELID_BUNKER1': 0.53931433, 'LEVELID_SILO': 0.47256002, 'LEVELID_STATUE': 0.107202865,
              'LEVELID_CONTROL': 0.49886572, 'LEVELID_ARCHIVES': 0.50678575, 'LEVELID_TRAIN': 0.15019713,
              'LEVELID_FRIGATE': 0.44757429, 'LEVELID_BUNKER2': 0.53931433, 'LEVELID_AZTEC': 0.35300568,
              'LEVELID_STREETS': 0.34187999, 'LEVELID_DEPOT': 0.21847887, 'LEVELID_EGYPT': 0.25608,
              'LEVELID_DAM': 0.23363999, 'LEVELID_FACILITY': 1.20648, 'LEVELID_RUNWAY': 0.089571431,
              'LEVELID_SURFACE': 0.45445713, 'LEVELID_JUNGLE': 0.094662853, 'LEVELID_CAVERNS': 0.26824287,
              'LEVELID_CRADLE': 0.23571429, 'LEVELID_SURFACE2': 0.45445713}
COLL_POS, COLL_POS3, COLL_EYE, COLL_LOOK = 4, 28, 44, 56     # collision434 (N64 layout inside Bean's player)
# Bean's own copies of Bond's x and z in its player struct, found by dumping it
# around a warp on Dam (the README's +0x150 is not one: it moves to unrelated
# numbers when Bond does): x,z at 0x4ec/0x4f4 and 0x530/0x538, and ten times
# them at 0x4e0/0x4e8 (the N64's field_3B8 = pos / 0.1). Bean slides Bond from
# these to where he is put, so a warp across a wall (Dam 329 -> 323) was
# refused until they were written too. Only x and z: the game settles y.
BEAN_XZ = ((0x4ec, 0x4f4, 1.0), (0x530, 0x538, 1.0), (0x4e0, 0x4e8, 10.0))
CHR_AILIST = 260
_st = {'eyeheight': None}


def _P():
    return lib.u32(lib.PLAYERS)


def _prop():
    return lib.u32(_P() + lib.PL_PROP)


def _vec(addr):
    return list(struct.unpack('>3f', lib.peek(addr, 12)))


def level_scale():
    return LEVELSCALE[os.environ['GF_LEVELID']]


def tile_height(stan, x, z):
    """stanGetPositionYValue() (stan.c:2868) on Bean's copy of the tile."""
    head = lib.peek(stan, 8)
    tail = struct.unpack_from('>h', head, 6)[0]
    pc, c, d = tail & 0xf, (tail >> 4) & 0xf, (tail >> 8) & 0xf
    n = max(pc, c, d) + 1
    raw = lib.peek(stan + 8, 8 * n)
    P = [struct.unpack_from('>hhh', raw, 8 * i) for i in range(n)]
    ls = level_scale()
    ils = 1.0 / ls
    px, pz = x * ls, z * ls
    a = [float(P[c][k] - P[d][k]) for k in range(3)]
    b = [float(P[pc][k] - P[d][k]) for k in range(3)]
    cp = [int(a[1] * b[2] - a[2] * b[1]), int(a[2] * b[0] - a[0] * b[2]), int(a[0] * b[1] - a[1] * b[0])]
    rsum = cp[0] * P[d][0] + cp[1] * P[d][1] + cp[2] * P[d][2]
    if cp[1] == 0:
        return P[d][1] * ils
    return ((rsum - px * cp[0]) - pz * cp[2]) / cp[1] * ils


def eyeheight():
    """Bond's eye over his tile, measured where he stands now (the spawn)."""
    if _st['eyeheight'] is None:
        lib.pause()
        prop = _prop()
        pos = _vec(prop + lib.PR_POS)
        stan = lib.u32(prop + lib.PR_STAN)
        eye = _vec(_P() + lib.PL_COLL + COLL_EYE)
        _st['eyeheight'] = eye[1] - tile_height(stan, pos[0], pos[2])
        _st['spawn'] = (pos[0], pos[1], pos[2], stan)
        lib.say('bean eye height %.2f (eye %.1f on tile 0x%08x)' % (_st['eyeheight'], eye[1], stan))
    return _st['eyeheight']


def freeze_ai():
    done = 0
    for pr in lib._all_props():
        if lib.peek(pr, 1)[0] != 3:
            continue
        c = lib.u32(pr + lib.PR_OBJ)
        if c:
            lib.poke(c + CHR_AILIST, b'\0\0\0\0')
            done += 1
    return done


def waypoint_pads():
    a = lib.setup(0)
    out = []
    i = 0
    blob = b''
    while a and i < 4000:
        if (i + 1) * 16 > len(blob):
            blob += lib.peek(a + len(blob), 16 * 64)
        p = struct.unpack_from('>i', blob, i * 16)[0]
        if p < 0:
            break
        out.append(p)
        i += 1
    return out


def pad_pos(n):
    x, y, z, stan = lib.pad_spot(n)
    return [x, y, z, stan]


def warp(x, y, z, theta, verta, stan):
    if not stan:
        lib.place(x, y, z, theta, verta, None)
        return None
    h = tile_height(stan, x, z)
    ey = eyeheight() + h
    _st['spot'] = (x, ey, z)
    v = struct.pack('>fff', x, ey, z)
    coll = _P() + lib.PL_COLL
    for a in (_prop() + lib.PR_POS, coll + COLL_POS, coll + COLL_POS3, coll + COLL_EYE):
        lib.poke(a, v)
    place(x, y, z, theta, verta, stan)
    return h


def place(x, y, z, theta, verta, stan):
    """xeniage.place() plus Bean's own position: written alone, GoldenEye's
    fields were put back by Bean - after the first pad Bond stayed where he was."""
    lib.place(x, y, z, theta, verta, stan)
    P = _P()
    for ox, oz, k in BEAN_XZ:
        lib.poke(P + ox, struct.pack('>f', x * k))
        lib.poke(P + oz, struct.pack('>f', z * k))


def camera():
    coll = _P() + lib.PL_COLL
    r2 = lambda v, nd=2: [round(c, nd) for c in v]
    return {'eye': r2(_vec(coll + COLL_EYE)), 'look': r2(_vec(coll + COLL_LOOK), 4),
            'feet': r2(_vec(_prop() + lib.PR_POS)), 'theta': round(lib.f32(_P() + lib.PL_THETA), 2),
            'verta': round(lib.f32(_P() + lib.PL_VERTA), 2)}


_chrs = {'pos': None}


def nearest_chr(eye):
    if _chrs['pos'] is None:
        _chrs['pos'] = [c['pos'] for c in lib.chrs()]
    best = None
    for p in _chrs['pos']:
        d = math.hypot(p[0] - eye[0], p[2] - eye[2])
        if best is None or d < best:
            best = d
    return round(best, 1) if best is not None else None


class LevelEnded(RuntimeError):
    pass


def _landed(x, z):
    e = _vec(_P() + lib.PL_COLL + COLL_EYE)
    return math.hypot(e[0] - x, e[2] - z) <= 4.0


def stand(x, y, z, theta, verta, stan, until, tick):
    """Bean runs in real time; frames() resumes for n swaps and pauses again,
    until_tick() lets it run to the tick and pauses there.

    Out of some places Bean will not let Bond be put anywhere else: on Dam's
    pad 329 he never quite settles (a unit off, sinking), and every warp from
    there to 323 was refused, where the same warp from the spawn or from 317
    lands exactly. So a warp that has not landed after a few frames goes back
    to the spawn first and tries again; one that still has not is left to the
    camera check, which does not rank it."""
    lib.look_ahead(False)
    warp(x, y, z, theta, verta, stan)
    lib.frames(6)
    if not _landed(x, z) and _st.get('spawn'):
        sx, sy, sz, ss = _st['spawn']
        for _ in range(3):
            warp(sx, sy, sz, theta, verta, ss)
            lib.frames(3)
        for _ in range(3):
            warp(x, y, z, theta, verta, stan)
            lib.frames(3)
        lib.say('warp to (%.0f, %.0f) went by the spawn: %s' % (x, z, 'landed' if _landed(x, z) else 'still not'))
    k = 0
    last = tick()
    # the whole warp every step, height and Bean's copies included: the channel
    # pauses Bean at a swap, but its game thread can be a frame or two ahead
    # and finish a move from the old place over a single write (Dam 329 -> 323
    # took three stands to stick when only x/z were held)
    while k < 3 or tick() < until - 3:
        warp(x, y, z, theta, verta, stan)
        lib.frames(3)
        now = tick()
        if now < last:
            raise LevelEnded('the level clock went from %d back to %d' % (last, now))
        last = now
        k += 1
    warp(x, y, z, theta, verta, stan)
    if tick() < until:
        lib.until_tick(until)
    warp(x, y, z, theta, verta, stan)
    lib.frames(2)
    return bool(stan)


def shoot(path):
    lib.shot(path)
    try:
        from PIL import Image
        return os.path.basename(path), list(Image.open(path).size)
    except Exception:
        return os.path.basename(path), [1280, 695]

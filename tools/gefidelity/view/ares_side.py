"""The view tour's GoldenEye side on the real cartridge (ares, through
n64twin and common/aresge.py). tour.py uses these in place of its gdb ones
when twin.py runs it with --oracle ares.

Nothing here calls a function in the game - n64twin cannot. What the native
port's tour asked the game for is done in memory instead:

- Bond goes onto the pad's own floor tile (PadRecord.stan) at the tile's height
  plus his eye height, written to every position change_player_pos_to_target()
  writes (bondview2.c:1780); the height is stanGetPositionYValue()'s (stan.c:
  2868) computed here from the tile's points and level_scale.
- Guards' AI lists are cleared by a write.
- First person is waited for on g_CameraMode; a mission whose opening still
  ignores the one fire press gets another, as a player would give it.
- The picture is n64twin's `shot`: the framebuffer the game hands to
  osViSwapBuffer, a finished frame.
- The viewport and field of view are read from the cartridge's own
  VideoSettings (g_ViBackData: viewx/viewy/viewleft/viewtop, fovy, aspect),
  not assumed from the port.

Addresses not in common/ares_layout.json are in view/ares_view_syms.json.
"""
import json, math, os, struct

import gdbge as lib   # aresge, through common/ares/gdbge.py

HERE = os.path.dirname(os.path.abspath(__file__))
V = json.load(open(os.path.join(HERE, 'ares_view_syms.json')))
VS = V['symbols']
T, SYM = lib.T, lib.SYM
PF = T['struct player']['fields']
CAMERAMODE_FP = 4


def _player():
    return lib.u32(SYM['g_CurrentPlayer'])


def _f32(a):
    return struct.unpack('>f', lib.peek(a, 4))[0]


def _wf(a, v):
    lib.poke(a, struct.pack('>f', v))


def _w32(a, v):
    lib.poke(a, struct.pack('>i', v))


def frame():
    return lib.s32(SYM['currentFrameCounter'])


def camera_mode():
    return lib.s32(VS['g_CameraMode'])


class LevelEnded(RuntimeError):
    pass


def freeze_ai():
    """Every chr's AI list cleared, the background chrs (5000 and up) too:
    they run the level's own script, and on Bunker 1 one of them ended the
    mission when Bond was put down at waypoint pad 101 (the screen went black,
    lvlStageLoad zeroed the level clock, and the next hold waited 200000
    frames on a counter that no longer moved)."""
    base = lib.u32(SYM['g_ChrSlots'])
    size = T['ChrRecord']['size']
    n = lib.s32(SYM['g_NumChrSlots'])
    blob = lib.peek(base, size * n) if n > 0 else b''
    off = T['ChrRecord']['fields']['ailist']['off']
    done = 0
    for k in range(n):
        c = lib.Rec('ChrRecord', base + k * size, blob[k * size:(k + 1) * size])
        # no prop test: the background chrs that run the level's own script
        # have none (and kept Bunker 1's running until it ended the mission)
        if c['chrnum'] < 0 or not c.ptr('ailist'):
            continue
        lib.poke(base + k * size + off, b'\0\0\0\0')
        done += 1
    return done


def first_person(settle_frames=900):
    """Wait for first person; press fire again if the opening still is still up."""
    start = frame()
    pressed = 0
    while camera_mode() != CAMERAMODE_FP:
        if frame() - start > settle_frames:
            if pressed >= 3:
                raise RuntimeError('never reached first person (mode %d)' % camera_mode())
            f = frame()
            lib.twin()('cue %d 0 Z' % (f + 2))
            lib.twin()('cue %d 0 -' % (f + 12))
            pressed += 1
            start = frame()
            lib.say('opening still at frame', f, 'mode', camera_mode(), '- fire pressed again')
        lib.frames(10)
    lib.frames(30)


def view_info():
    p = lib.u32(VS['g_ViBackData'])
    addr = p if (p >> 24) == 0x80 else VS['g_ViBackData']
    raw = lib.peek(addr, V['VideoSettings_s']['size'])
    out = {}
    for k, v in V['VideoSettings_s'].items():
        if k != 'size':
            out[k] = struct.unpack_from('>' + v[1], raw, v[0])[0]
    return {'viewport': [out['viewleft'], out['viewtop'], out['viewx'], out['viewy']],
            'fovy': round(out['fovy'], 4), 'aspect': round(out['aspect'], 5), 'vi_xy': [out['x'], out['y']],
            'buf': [out['bufx'], out['bufy']], 'mode': camera_mode()}


def waypoint_pads():
    setup = lib.Rec('stagesetup', SYM['g_CurrentSetup'])
    a = setup.ptr('pathwaypoints')
    out = []
    if not a:
        return out
    blob = b''
    i = 0
    while i < 4000:
        if (i + 1) * 16 > len(blob):
            blob += lib.peek(a + len(blob), 16 * 256)
        p = struct.unpack_from('>i', blob, i * 16)[0]   # waypoint.padID; 16 bytes a waypoint
        if p < 0:
            break
        out.append(p)
        i += 1
    return out


_pads = {}


def pad_pos(n):
    """(x, y, z, the pad's own tile) - the same slots the port's tour fills."""
    if not _pads:
        for r in lib.pads():
            _pads[r[0]] = r
    r = _pads[n]
    return [r[1], r[2], r[3], lib.pad_tile(n)]


def tile_height(stan, x, z):
    """stanGetPositionYValue(): the plane through three of the tile's points,
    in GoldenEye's world units. The third point's index is the tile's point
    count, as the game takes it."""
    head = lib.peek(stan, 8)
    tail = struct.unpack_from('>h', head, 6)[0]
    pc, c, d = tail & 0xf, (tail >> 4) & 0xf, (tail >> 8) & 0xf
    n = max(pc, c, d) + 1
    raw = lib.peek(stan + 8, 8 * n)
    P = [struct.unpack_from('>hhh', raw, 8 * i) for i in range(n)]
    ls, ils = _f32(VS['level_scale']), _f32(VS['inv_level_scale'])
    px, pz = x * ls, z * ls
    a = [float(P[c][k] - P[d][k]) for k in range(3)]
    b = [float(P[pc][k] - P[d][k]) for k in range(3)]
    tr = lambda v: int(v)    # (s64) of a float truncates toward zero
    cp = [tr(a[1] * b[2] - a[2] * b[1]), tr(a[2] * b[0] - a[0] * b[2]), tr(a[0] * b[1] - a[1] * b[0])]
    rsum = cp[0] * P[d][0] + cp[1] * P[d][1] + cp[2] * P[d][2]
    if cp[1] == 0:
        return P[d][1] * ils
    return ((rsum - px * cp[0]) - pz * cp[2]) / cp[1] * ils


def warp(x, y, z, theta, verta, stan):
    """change_player_pos_to_target() by hand, as GoldenEye's spawn uses it."""
    P = _player()
    if not stan:
        lib.place(x, y, z, theta, verta, None)
        return None
    h = tile_height(stan, x, z)
    ey = _f32(P + PF['eyeheight']['off']) + h
    prop = lib.u32(P + PF['prop']['off'])
    ppos = prop + T['PropRecord']['fields']['pos']['off']
    for base in (ppos, P + PF['bondprevpos']['off'], P + PF['field_488.collision_position']['off'],
                 P + PF['field_488.pos']['off'], P + PF['field_488.pos3']['off']):
        lib.poke(base, struct.pack('>fff', x, ey, z))
    _wf(P + PF['stanHeight']['off'], h)
    _wf(P + PF['field_70']['off'], h)
    lib.place(x, y, z, theta, verta, stan)
    return h


def no_lookahead():
    P = _player()
    for f in ('automovecentreenabled', 'automovecentre', 'docentreupdown'):
        _w32(P + PF[f]['off'], 0)


def camera():
    P = lib.Rec('struct player', _player())
    prop = lib.Rec('PropRecord', P.ptr('prop'))
    r2 = lambda v, nd=2: [round(c, nd) for c in v]
    return {'eye': r2(P.vec('field_488.pos')), 'look': r2(P.vec('field_488.applied_view'), 4),
            'feet': r2(prop.vec('pos')), 'theta': round(P.f('vv_theta'), 2), 'verta': round(P.f('vv_verta'), 2)}


_chrs = {'pos': None}


def nearest_chr(eye):
    """The chrs are frozen, so their places are read once."""
    if _chrs['pos'] is None:
        _chrs['pos'] = [c['pos'] for c in lib.chrs()]
    best = None
    for p in _chrs['pos']:
        d = math.hypot(p[0] - eye[0], p[2] - eye[2])
        if best is None or d < best:
            best = d
    return round(best, 1) if best is not None else None


def stand(x, y, z, theta, verta, stan, until, tick):
    """Held until the level clock reads `until`. The cartridge runs 2-3 ticks
    a video frame (the port under lockstep runs one), so the minimum hold is
    three holds of three frames, not eight: eight overran a 40-tick slot and
    the cartridge fell 500 ticks behind ours by the sixth pad."""
    no_lookahead()
    warp(x, y, z, theta, verta, stan)
    k = 0
    last = tick()
    while k < 3 or tick() < until - 3:
        lib.place(x, y, z, theta, verta, stan or None)
        no_lookahead()
        lib.frames(3)
        now = tick()
        if now < last:
            raise LevelEnded('the level clock went from %d back to %d: the mission ended here' % (last, now))
        last = now
        k += 1
    lib.place(x, y, z, theta, verta, stan or None)
    lib.until_tick(until)
    lib.place(x, y, z, theta, verta, stan or None)
    lib.frames(2)
    return bool(stan)


def shoot(path):
    """The next frame the game hands to osViSwapBuffer: finished, never torn."""
    ans = lib.twin()('shot %s' % path)
    return os.path.basename(path), [int(v) for v in ans.split()[:2]]

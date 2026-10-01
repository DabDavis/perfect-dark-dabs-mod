"""The view tour, the same on both sides: the player on every VIEW_STEP-th
waypoint's pad, one shot per heading, and a manifest of where each camera
really was when its picture was taken.

    twin.py both view/tour.py --mission dam --out OUT/dam --env VIEW_STEP=6 [--oracle ares|port]

GoldenEye is the cartridge in ares (view/ares_side.py, no function calls) or
the decomp's native port under gdb (the functions below).

Run it through view/viewdiff.py, which also scores and reports. Pad numbers and
vv_theta mean the same in both games (a converted level is GoldenEye's world
plus one offset), so pad N heading H is the same picture on both sides.

Only x and z are placed; each game finds its own floor and puts its own eye
over it, so the manifest's camera heights are a measurement, not an echo of
what was written. (The world diff found ours 8.3 units under GoldenEye's on
Dam; viewdiff.py reports the height delta per pair rather than hiding it.)

Each shot is taken at level tick VIEW_T0 + n * VIEW_DT on both sides (400 and
40), and with VIEW_FREEZE=1 (the default) every chr's AI list is taken away
on the first frame, so guards and the vehicles their scripts drive stand
still: a parked truck or a guard in the wrong place is then the conversion's.

Env: VIEW_STEP (6), VIEW_HEADS ("0,90,180,270"), VIEW_VERTA (-5),
VIEW_MAX (pads, 0 = all), VIEW_ONLY (comma list of pads, overrides the walk),
VIEW_SETTLE (the GoldenEye frame by which it must be in first person, 1800).
GoldenEye must run on twin.py's default common/solo-quiet.padscript, which
presses fire once rather than solo.padscript's four times: a shot fired before
the tour leaves bullet holes in its pictures.
"""
import os, sys, json, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
SIDE = os.environ['GF_SIDE']
lib = __import__('gdbge' if SIDE == 'ge' else 'gdbpd')
ev = getattr(lib, 'ev', None)
# GoldenEye on the cartridge (twin.py --oracle ares): no gdb, no function
# calls; view/ares_side.py does each step in memory
ARES = SIDE == 'ge' and getattr(lib, 'ORACLE', 'port') == 'ares'
# GoldenEye XBLA ("Bean") in Xenia: xenia/run_scenario.py hands this scenario
# common/xeniage.py as gdbge; view/xenia_side.py does each step in memory
XENIA = SIDE == 'ge' and getattr(lib, 'SIDE', '') == 'xenia'
if ARES or XENIA:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if ARES:
        import ares_side as memside
    else:
        import xenia_side as memside
OUT = os.environ['GF_OUT']
STEP = int(os.environ.get('VIEW_STEP', '6'))
HEADS = [float(h) for h in os.environ.get('VIEW_HEADS', '0,90,180,270').split(',')]
VERTA = float(os.environ.get('VIEW_VERTA', '-5'))
MAXP = int(os.environ.get('VIEW_MAX', '0'))
ONLY = os.environ.get('VIEW_ONLY')
# pads that end the mission when Bond is put down on them (Bunker 1's 101 on
# the cartridge): viewdiff.py retries the oracle without them
SKIP = {int(p) for p in os.environ.get('VIEW_SKIP', '').split(',') if p.strip()}
# the oracle's own tick for each picture, in shot order (viewdiff.py hands them
# to our side): the release in Xenia runs in real time and falls far behind the
# T0 + n * DT schedule (Dam's last shots at tick 10987 against our 5844, with
# its truck somewhere else by then), so ours waits for the tick the oracle had
TICKS = [int(t) for t in os.environ.get('VIEW_TICKS', '').split(',') if t.strip()]
SETTLE = int(os.environ.get('VIEW_SETTLE', '1800'))
CAMERAMODE_SWIRL, CAMERAMODE_FP = 3, 4   # GoldenEye's CAMERAMODE enum
FREEZE = os.environ.get('VIEW_FREEZE', '1') == '1'
T0 = int(os.environ.get('VIEW_T0', '400'))
DT = int(os.environ.get('VIEW_DT', '40'))


def freeze_ai():
    """Every chr's AI list taken away on the level's first frame, on both
    sides: the guards stand where the setup put them, nobody raises an alarm or
    shoots Bond, and the vehicles their lists drive stay parked. What is left in
    a picture is the level as converted, not two runs of its scripts. The
    background chrs (5000 and up) too: they run the level's own script, and
    one ended Bunker 1 when Bond was put down in its exit room."""
    n = int(ev('g_NumChrSlots'))
    done = 0
    for k in range(n):
        c = ev('g_ChrSlots[%d]' % k)
        # background chrs (the level's own scripts) have no prop: freeze them too
        if int(c['chrnum']) < 0 or int(c['ailist']) == 0:
            continue
        if SIDE == 'pd' and int(c['prop']) and int(c['prop']) == int(ev('g_Vars.currentplayer->prop')):
            continue
        lib.gdb.execute('set variable g_ChrSlots[%d].ailist = 0' % k)
        done += 1
    return done


def waypoint_pads():
    out = []
    expr = ('g_CurrentSetup.pathwaypoints[%d].padID' if SIDE == 'ge' else 'g_StageSetup.waypoints[%d].padnum')
    base = 'g_CurrentSetup.pathwaypoints' if SIDE == 'ge' else 'g_StageSetup.waypoints'
    if int(ev('(long)' + base)) == 0:
        return out
    i = 0
    while i < 4000:
        p = int(ev(expr % i))
        if p < 0:
            break
        out.append(p)
        i += 1
    return out


def pad_pos(n):
    """(x, y, z, room) of pad n in this game's own world; on GoldenEye's side
    the room slot carries the pad's own floor tile (pad->stan) instead."""
    if SIDE == 'ge':
        rec = ev('g_CurrentSetup.pads[%d]' % n) if n < 10000 else ev('g_CurrentSetup.boundpads[%d]' % (n - 10000))
        return [float(rec['pos'][a]) for a in 'xyz'] + [int(rec['stan'])]
    p = lib._pad(n)
    return [float(p['pos'][a]) for a in 'xyz'] + [int(p['room'])]


_pdscratch = {'v': None}


def pd_ground(x, y, z, room):
    """Our floor under a pad, asked the way bwalk asks it each tick
    (cdFindGroundAtCyl in the pad's room). Some waypoint pads stand hundreds
    of units over their floor in both games (Frigate's 151: 833); GoldenEye
    puts Bond on the tile at once, our walk would still be falling when the
    picture is taken."""
    if _pdscratch['v'] is None:
        lib.gdb.execute('set variable $gfc = (struct coord *)malloc(sizeof(struct coord))')
        lib.gdb.execute('set variable $gfr = (RoomNum *)malloc(16 * sizeof(RoomNum))')
        lib.gdb.execute('set variable $gfcol = (u16 *)malloc(8)')
        lib.gdb.execute('set variable $gftype = (u8 *)malloc(8)')
        _pdscratch['v'] = True
    for a, v in (('x', x), ('y', y + 30.0), ('z', z)):
        lib.gdb.execute('set variable $gfc->%s = %f' % (a, v))
    lib.gdb.execute('set variable $gfr[0] = %d' % room)
    lib.gdb.execute('set variable $gfr[1] = -1')
    g = float(ev('cdFindGroundAtCyl($gfc, g_Vars.currentplayer->bond2.radius, $gfr, $gfcol, $gftype)'))
    return g if g > -1e6 else None


def _vec(v, nd=2):
    return [round(float(v[a]), nd) for a in 'xyz']


def nearest_chr(eye):
    """How far across the floor the nearest chr stands from the camera. Guards
    are frozen on their setup pads, and a waypoint pad can be one: the camera
    is then inside a guard, and the two games' eye heights (ours 8 units
    lower) show different parts of him - a black hat brim in GoldenEye, a green
    jacket in ours on Facility's pad 150. viewdiff.py does not rank those."""
    best = None
    n = int(ev('g_NumChrSlots'))
    me = int(ev('g_Vars.currentplayer->prop')) if SIDE == 'pd' else int(ev('g_CurrentPlayer->prop'))
    for k in range(n):
        c = ev('g_ChrSlots[%d]' % k)
        pr = c['prop']
        if int(pr) == 0 or int(c['chrnum']) < 0 or int(pr) == me:
            continue
        try:
            d = ((float(pr['pos']['x']) - eye[0]) ** 2 + (float(pr['pos']['z']) - eye[2]) ** 2) ** 0.5
        except lib.gdb.MemoryError:
            continue   # GoldenEye keeps stale slots (chrnum 1, a dangling prop) past its real chrs
        if best is None or d < best:
            best = d
    return round(best, 1) if best is not None else None


def camera():
    if SIDE == 'ge':
        P = ev('g_CurrentPlayer')
        return {'eye': _vec(P['field_488']['pos']), 'look': _vec(P['field_488']['applied_view'], 4),
                'feet': _vec(P['prop']['pos']), 'theta': round(float(P['vv_theta']), 2),
                'verta': round(float(P['vv_verta']), 2)}
    P = ev('g_Vars.currentplayer')
    return {'eye': _vec(P['cam_pos']), 'look': _vec(P['cam_look'], 4), 'feet': _vec(P['prop']['pos']),
            'theta': round(float(P['vv_theta']), 2), 'verta': round(float(P['vv_verta']), 2),
            'eyeheight': round(float(P['vv_eyeheight']), 2), 'ground': round(float(P['vv_ground']), 2),
            'headlook': _vec(P['headlook'], 4), 'headup': _vec(P['headup'], 4),
            'health': round(float(P['bondhealth']), 3)}


def pd_place_xz(x, z, theta, verta, room=None):
    P = 'g_Vars.currentplayer'
    for var, val in (('prop->pos.x', x), ('prop->pos.z', z), ('vv_theta', theta), ('vv_verta', verta)):
        lib.gdb.execute('set variable %s->%s = %f' % (P, var, val))
    if room is not None and room >= 0:
        lib.gdb.execute('set variable %s->prop->rooms[0] = %d' % (P, room))
        lib.gdb.execute('set variable %s->prop->rooms[1] = -1' % P)


def render(on):
    """GoldenEye's software rasteriser costs 200-300 ms a frame under gdb and
    10-60 ms without, so it is off while Bond is held and on for the three
    frames a picture is taken from. The game's own frame (display lists,
    on-screen props) runs either way; only the RDP's drawing is skipped
    (port/shim/os_vi.c, portRenderEnabled(): a frame is drawn when the graphics
    task count reaches `from`)."""
    if SIDE == 'ge':
        lib.gdb.execute('set variable portRenderEnabled::from = %d' % (0 if on else 2000000000))


_gepos = {'v': None}


def ge_warp(x, y, z, theta, verta, stan):
    """Bond onto the floor tile under (x, y, z) the way GoldenEye spawns him
    (bondview2.c:8732): the tile's height at x, z plus his eye height, through
    change_player_pos_to_target(). Writing x and z alone (gdbge.place) leaves
    his height where it was, and on a level of several decks (Frigate) he
    stayed inside the deck below."""
    if not stan:
        lib.place(x, y, z, theta, verta, stan)
        return
    if _gepos['v'] is None:
        lib.gdb.execute('set variable $gfpos = (coord3d *)malloc(sizeof(coord3d))')
        _gepos['v'] = True
    P = 'g_CurrentPlayer'
    h = float(ev('bondviewYPositionRelated((StandTile *)%d, %f, %f)' % (stan, x, z)))
    ey = float(ev('%s->eyeheight' % P)) + h
    for a, v in (('x', x), ('y', ey), ('z', z)):
        lib.gdb.execute('set variable $gfpos->%s = %f' % (a, v))
    lib.call('(void)change_player_pos_to_target(&%s->field_488, $gfpos, (StandTile *)%d)' % (P, stan))
    for a, v in (('x', x), ('y', ey), ('z', z)):
        lib.gdb.execute('set variable %s->prop->pos.%s = %f' % (P, a, v))
        lib.gdb.execute('set variable %s->bondprevpos.%s = %f' % (P, a, v))
    lib.gdb.execute('set variable %s->stanHeight = %f' % (P, h))
    lib.gdb.execute('set variable %s->field_70 = %f' % (P, h))
    lib.place(x, y, z, theta, verta, stan)


def no_lookahead():
    """Look Ahead off and unlatched: GoldenEye pulls vv_verta toward the slope
    under Bond once docentreupdown has latched (bondview2.c:5864), which put
    its pitch 2-6 degrees off the -5 written on Frigate. Ours has the same
    three fields (bondmove.c)."""
    P = 'g_CurrentPlayer' if SIDE == 'ge' else 'g_Vars.currentplayer'
    for f in ('automovecentreenabled', 'automovecentre', 'docentreupdown'):
        lib.gdb.execute('set variable %s->%s = 0' % (P, f))


def stand(x, y, z, theta, room, until):
    no_lookahead()
    """Both games: feet over (x, z), facing theta, held there until the level
    clock reads `until` (and at least 24 frames); the game owns y."""
    if SIDE == 'ge':
        # the pad's own tile (GoldenEye set pad->stan when it loaded the
        # setup); stanFindFloorTileBelowY is not "the highest tile below" -
        # on Frigate's pad 133 it gave a deck 440 units under the pad's
        stan = room or lib.floor_tile(x, y, z)
        render(False)
        ge_warp(x, y, z, theta, VERTA, stan)
        k = 0
        while k < 8 or lib.tick() < until - 3:
            lib.place(x, y, z, theta, VERTA, stan)
            no_lookahead()
            lib.frames(3)
            k += 1
        lib.place(x, y, z, theta, VERTA, stan)
        lib.until_tick(until)
        lib.place(x, y, z, theta, VERTA, stan)
        # the rasteriser back on for the frames the picture is taken from
        render(True)
        lib.frames(3)
        return stan != 0
    # the first placement sets the height too - on our floor under the pad,
    # as GoldenEye's warp does - the rest only x/z so our vertical movement
    # settles the eye over the floor it finds
    lib.place(x, y, z, theta, VERTA, room)
    g = pd_ground(x, y, z, room)
    if g is not None:
        P = 'g_Vars.currentplayer'
        eh = float(ev(P + '->vv_eyeheight'))
        for var, val in (('prop->pos.y', g + eh), ('vv_manground', g), ('vv_ground', g)):
            lib.gdb.execute('set variable %s->%s = %f' % (P, var, val))
    lib.frames(3)
    k = 0
    while k < 7 or lib.tick() < until - 3:
        pd_place_xz(x, z, theta, VERTA, room)
        no_lookahead()
        lib.frames(3)
        k += 1
    pd_place_xz(x, z, theta, VERTA, room)
    lib.until_tick(until)
    pd_place_xz(x, z, theta, VERTA, room)
    lib.frames(2)
    return True


def shoot(n):
    if SIDE == 'ge':
        # Not gdbge.shot(): the framebuffer on display can be one the parallel
        # rasteriser is still filling, and with the rasteriser off between
        # pictures the unfilled part is black (Facility's worst pairs were
        # 60% black). fast3dRunTask() calls f3dDumpFrameToDir() once a task's
        # bands are harvested; the colour image is the finished frame there.
        path = os.path.join(OUT, 'shot_%03d.ppm' % n)
        lib.gdb.execute('break f3dDumpFrameToDir')
        lib._go()
        lib.gdb.execute('delete')
        lib.call('(int)fast3dWritePPM("%s", (const unsigned short *)g_colorImage, g_colorImageWidth, '
                 'g_scissorLry)' % path)
        return os.path.basename(path)
    lib.shot()
    return None   # twin.py renames the port's own file to shot_NNN.<ext> in order


def main_ares():
    """GoldenEye on the cartridge (or the release in Xenia): the same tour,
    each step in memory."""
    ares_side = memside
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    if XENIA:
        lib.until_tick(int(os.environ.get('VIEW_BEAN_SETTLE', '120')))
        ares_side.eyeheight()
    frozen = ares_side.freeze_ai() if FREEZE else 0
    if ARES:
        ares_side.first_person()
        view = ares_side.view_info()
    else:
        # the release's frame as the rig photographs it; its field of view is
        # measured by view/fovfit.py, not assumed, and handed in
        view = {'viewport': [0, 0, 1280, 695], 'fovy': float(os.environ.get('VIEW_BEAN_FOVY', '0')) or None,
                'eyeheight': round(ares_side.eyeheight(), 3), 'levelscale': ares_side.level_scale()}
    ways = ares_side.waypoint_pads()
    pads = [int(p) for p in ONLY.split(',')] if ONLY else [p for p in ways if p not in SKIP][::STEP]
    if MAXP:
        pads = pads[:MAXP]
    t0 = max(T0, lib.tick() + DT)
    lib.say('tour', len(ways), 'waypoints', len(pads), 'pads; frozen', frozen, 'chrs; view', view,
            '; first shot at tick', t0)
    manifest = {'side': SIDE, 'oracle': 'ares' if ARES else 'xenia', 'mission': int(os.environ['GF_MISSION']), 'view': view,
                'pads': {}, 'shots': [], 'frozen': frozen, 't0': t0, 'dt': DT, 'skipped': sorted(SKIP)}
    n = 0
    for pad in pads:
        x, y, z, stan = ares_side.pad_pos(pad)
        manifest['pads'][str(pad)] = [round(x, 3), round(y, 3), round(z, 3), stan]
        for h in HEADS:
            try:
                ok = ares_side.stand(x, y, z, h, VERTA, stan, t0 + n * DT, lib.tick)
            except ares_side.LevelEnded:
                lib.say('level ended at pad', pad)
                raise
            cam = ares_side.camera()
            cam['chr_near'] = ares_side.nearest_chr(cam['eye'])
            name, size = ares_side.shoot(os.path.join(OUT, 'shot_%03d.%s' % (n, 'ppm' if ARES else 'png')))
            if 'fb' not in view:
                view['fb'] = size
            manifest['shots'].append({'n': n, 'pad': pad, 'head': h, 'file': name, 'tile': ok, 'cam': cam,
                                      'tick': lib.tick(), 'size': size})
            n += 1
        lib.say('pad', pad, 'done', n)
    with open(os.path.join(OUT, 'manifest.json'), 'w') as fh:
        json.dump(manifest, fh, indent=1)
    lib.say('wrote manifest', n, 'shots')


def main():
    if ARES or XENIA:
        return main_ares()
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    frozen = freeze_ai() if FREEZE else 0
    if SIDE == 'ge':
        # GoldenEye's opening waits for fire; common/solo-quiet.padscript
        # presses it once (frame 1300). A mission whose opening ignored that press is
        # put into first person the way GoldenEye does with no swirl to play.
        while int(ev('g_CameraMode')) != CAMERAMODE_FP and int(ev('currentFrameCounter')) < SETTLE:
            lib.frames(10)
        if int(ev('g_CameraMode')) != CAMERAMODE_FP:
            lib.say('opening still at frame', int(ev('currentFrameCounter')), 'mode', int(ev('g_CameraMode')),
                    '- forced to first person')
            lib.gdb.execute('set variable g_IntroSwirl = 0')
            lib.call('(void)bondviewSetCameraMode(%d)' % CAMERAMODE_SWIRL)
            lib.frames(30)
        lib.frames(30)
        view = {'fb': [int(ev('g_colorImageWidth')), int(ev('g_scissorLry'))],
                'viewport': [int(ev('get_curplayer_viewport_ulx()')), int(ev('bondviewGetCurrentPlayerViewportUly()')),
                             int(ev('bondviewGetCurrentPlayerViewportWidth()')),
                             int(ev('bondviewGetCurrentPlayerViewportHeight()'))],
                'mode': int(ev('g_CameraMode'))}
    else:
        lib.frames(30)
        lib.gdb.execute('set variable g_Vars.currentplayer->invincible = 1')
        # Head Roll off: the look is vv_theta and vv_verta and nothing else.
        # With it on the head animation's look (bmoveUpdateHead()) tilts the
        # view by a few tenths of a degree in a phase the whole simulation
        # feeds, so a change that only moves which rooms are on screen moved
        # Streets 89's camera 0.45 degree (headlook x 0.0052 -> -0.0031) and
        # its picture 0.05 - the base and the test no longer differed by what
        # they draw. GoldenEye's own head look stays in its pictures; the
        # same on base and test, as the oracle's pictures are reused.
        # And Always Show Target off (0x200): on a GE Plus level it keeps
        # GoldenEye's sight up with the gun lowered, which GoldenEye shows
        # only while aiming (its SIGHT ON-SCREEN, measured on the cartridge).
        mp = int(ev('g_Vars.currentplayerstats->mpindex'))
        lib.gdb.execute('set variable g_PlayerConfigsArray[%d].options = g_PlayerConfigsArray[%d].options & ~0x280'
                        % (mp, mp))
        w, h = int(ev('videoGetWindowWidth()')), int(ev('videoGetWindowHeight()'))
        view = {'fb': [w, h], 'viewport': [0, 0, w, h]}
    ways = waypoint_pads()
    pads = [int(p) for p in ONLY.split(',')] if ONLY else ways[::STEP]
    if MAXP:
        pads = pads[:MAXP]
    lib.say('tour', len(ways), 'waypoints', len(pads), 'pads', 'tick', lib.tick())
    t0 = max(T0, lib.tick() + DT)
    lib.say('frozen', frozen, 'chrs; first shot at tick', t0)
    manifest = {'side': SIDE, 'mission': int(os.environ['GF_MISSION']), 'view': view, 'pads': {}, 'shots': [],
                'frozen': frozen, 't0': t0, 'dt': DT}
    n = 0
    for pad in pads:
        x, y, z, room = pad_pos(pad)
        manifest['pads'][str(pad)] = [round(x, 3), round(y, 3), round(z, 3), room]
        for h in HEADS:
            ok = stand(x, y, z, h, room, TICKS[n] if n < len(TICKS) else t0 + n * DT)
            cam = camera()
            cam['chr_near'] = nearest_chr(cam['eye'])
            name = shoot(n)
            manifest['shots'].append({'n': n, 'pad': pad, 'head': h, 'file': name, 'tile': bool(ok), 'cam': cam,
                                      'tick': lib.tick()})
            n += 1
        lib.say('pad', pad, 'done', n)
    with open(os.path.join(OUT, 'manifest.json'), 'w') as fh:
        json.dump(manifest, fh, indent=1)
    lib.say('wrote manifest', n, 'shots')


try:
    main()
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
if SIDE == 'pd':
    # pd.log names the screenshots, and a killed process loses its buffer
    lib.call('(int)fflush(0)')
lib.finish()

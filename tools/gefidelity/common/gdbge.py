"""The GoldenEye half of the twin driver: gdb Python against the decomp's native
port (sdg@10.8.0.3:~/claude-007/007/build/port/ge007, an -m32 build).

Load it from a gdb script with

    import os, sys; sys.path.insert(0, os.environ['GF_COMMON']); from gdbge import *

and run under `twin.py ge` (or by hand: PORT_PAD_SCRIPT=solo.padscript
PORT_BOOT_FRAMES=1000000 PORT_LOCKSTEP=1 PORT_VI_LOCKSTEP=1 gdb -batch -x S
--args ./build/port/ge007 --boot). Every trap below cost an investigation once:

- The pad script walks the front end into solo **Dam**; boot() swaps the level
  in at bossSetLoadedStage. It enters on 00 Agent, so boot() also writes
  g_SelectedDifficulty (g_AiAccuracyModifier is rewritten every frame - never
  set that one).
- Without PORT_LOCKSTEP=1 PORT_VI_LOCKSTEP=1 each gdb stop makes a frame nine
  sixtieths long.
- The level clock is g_GlobalTimer (60ths, from the level's start); the pad
  script's clock is currentFrameCounter (frames since power on).
- Bond is placed by writing the prop, the collision position and all three
  tile pointers, and must be held there for a few frames.
"""
import gdb, os, json, math, struct


def _int_bits(v):
    """A float's bits as the s32 they were written as."""
    return struct.unpack('<i', struct.pack('<f', float(v)))[0]

gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set print elements 0')

SIDE = 'ge'
_st = {'started': False}


def ev(expr):
    return gdb.parse_and_eval(expr)


def call(expr):
    return gdb.execute('call ' + expr, to_string=True)


def say(*a):
    print('GF', *a, flush=True)


def _go():
    gdb.execute('continue' if _st['started'] else 'run')
    _st['started'] = True


def boot(levelid, difficulty=0):
    """Run until GoldenEye loads Dam from the pad script, and load levelid instead."""
    gdb.execute('break bossSetLoadedStage if stage == LEVELID_DAM')
    _go()
    gdb.execute('set variable stage = %s' % levelid)
    gdb.execute('delete')
    # the mission is picked before its difficulty: the front end writes
    # g_SelectedDifficulty after bossSetLoadedStage, so set it at the setup load
    gdb.execute('break proplvreset2')
    _go()
    gdb.execute('set variable g_SelectedDifficulty = %d' % difficulty)
    gdb.execute('delete')
    # the first rendered frame of the level proper
    gdb.execute('break lvlRender')
    _go()
    gdb.execute('delete')
    _st['t0'] = int(ev('g_GlobalTimer')) - 1
    say('boot', levelid, 'difficulty', int(ev('g_SelectedDifficulty')), 'tick', tick(),
        'frame', int(ev('currentFrameCounter')), 'globaltimer', int(ev('g_GlobalTimer')))


def tick():
    """The level's clock in 60ths, 1 on its first frame as Perfect Dark's
    lvframe60 is. g_GlobalTimer itself runs from power on."""
    return int(ev('g_GlobalTimer')) - _st.get('t0', 0)


def until_tick(t):
    if tick() >= t:
        return
    gdb.execute('break lvlRender if g_GlobalTimer >= %d' % (t + _st.get('t0', 0)))
    _go()
    gdb.execute('delete')


def frames(n=1):
    """Let n rendered frames go by."""
    target = int(ev('currentFrameCounter')) + n
    gdb.execute('break lvlRender if currentFrameCounter >= %d' % target)
    _go()
    gdb.execute('delete')


def floor_tile(x, y, z):
    return int(ev('(unsigned long)stanFindFloorTileBelowY(%f, %f, %f, 0.0f)' % (x, y + 30, z)))


def place(x, y, z, theta=None, verta=None, stan=None):
    """Bond's feet at x, y, z (GoldenEye's world). Returns the tile, 0 if none."""
    if stan is None:
        stan = floor_tile(x, y, z)
    P = 'g_CurrentPlayer'
    for a, v in (('x', x), ('z', z)):
        gdb.execute('set variable %s->prop->pos.%s = %f' % (P, a, v))
        gdb.execute('set variable %s->field_488.collision_position.%s = %f' % (P, a, v))
    if stan:
        for f in ('prop->stan', 'field_488.current_tile_ptr', 'field_488.current_tile_ptr_for_portals'):
            gdb.execute('set variable %s->%s = (StandTile *)%d' % (P, f, stan))
    if theta is not None:
        gdb.execute('set variable %s->vv_theta = %f' % (P, theta))
    if verta is not None:
        gdb.execute('set variable %s->vv_verta = %f' % (P, verta))
    return stan


def hold(x, y, z, theta=None, verta=None, n=8):
    """Place Bond and keep him there n times three frames, so the view settles."""
    stan = floor_tile(x, y, z)
    for _ in range(n):
        place(x, y, z, theta, verta, stan)
        frames(3)
    place(x, y, z, theta, verta, stan)
    frames(1)
    return stan


def shot(path):
    """The framebuffer as a PPM (the front end's resolution, not the window's)."""
    call('(int)fast3dWritePPM("%s", (const unsigned short *)osViGetCurrentFramebuffer(), '
         'g_colorImageWidth, g_scissorLry)' % path)
    return path


def finish():
    try:
        gdb.execute('kill')
    except gdb.error:
        pass
    gdb.execute('quit')


# ---------------------------------------------------------------- the world

OBJ_TYPES = {1, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 17, 20, 21, 36, 39, 40, 42, 43, 45, 47}
PROPDEF_END = 0x30


def _f(v):
    return round(float(v), 3)


def _rooms(prop):
    out = []
    for r in range(4):
        b = int(prop['rooms'][r])
        if b == 0xff:
            break
        out.append(b)
    return out


def pads():
    """Every pad: [index, x, y, z, tile room or -1]."""
    out = []
    base = ev('g_CurrentSetup.pads')
    if int(base) == 0:
        return out
    i = 0
    while i < 4000:
        p = base[i]
        if int(p['plink']) == 0:
            break
        stan = int(p['stan'])
        room = int(ev('((StandTile *)%d)->room' % stan)) if stan else -1
        out.append([i, _f(p['pos']['x']), _f(p['pos']['y']), _f(p['pos']['z']), room])
        i += 1
    # bound pads are numbered from 10000 in GoldenEye's commands and records
    base = ev('g_CurrentSetup.boundpads')
    i = 0
    while int(base) != 0 and i < 4000:
        p = base[i]
        if int(p['plink']) == 0:
            break
        stan = int(p['stan'])
        room = int(ev('((StandTile *)%d)->room' % stan)) if stan else -1
        out.append([10000 + i, _f(p['pos']['x']), _f(p['pos']['y']), _f(p['pos']['z']), room])
        i += 1
    return out


def _mtx3(m):
    """A 4x4 Mtxf's 3x3 part as rows, plus each row's length (the scale)."""
    rows = [[float(m['m'][r][c]) for c in range(3)] for r in range(3)]
    return rows


def _ailist_ids():
    ids = {}
    for table in ('g_CurrentSetup.ailists', 'g_GlobalAILists'):
        base = ev(table)
        if base.type.code == gdb.TYPE_CODE_ARRAY:
            base = base[0].address
        if int(base) == 0:
            continue
        i = 0
        while i < 2000:
            rec = base[i]
            lst = int(rec['ailist'])
            if lst == 0:
                break
            ids[lst] = int(rec['ID'])
            i += 1
    return ids


def _weaponnum(prop):
    if int(prop) == 0:
        return None
    try:
        return int(prop['weapon']['weaponnum'])
    except gdb.error:
        return None


def props():
    out = []
    p = ev('(unsigned int *)g_CurrentSetup.propDefs')
    i = 0
    while i < 5000:
        hdr = p.cast(gdb.lookup_type('PropDefHeaderRecord').pointer()).dereference()
        t = int(hdr['type'])
        if t == PROPDEF_END:
            break
        words = int(ev('sizepropdef((PropDefHeaderRecord *)%d)' % int(p)))
        rec = {'i': i, 'type': t, 'words': words}
        if t in OBJ_TYPES:
            o = p.cast(gdb.lookup_type('ObjectRecord').pointer()).dereference()
            rec.update({'model': int(o['obj']), 'pad': int(o['pad']),
                        'flags': int(o['flags']) & 0xffffffff, 'flags2': int(o['flags2']) & 0xffffffff,
                        'state': int(hdr['state']), 'extrascale': int(hdr['extrascale'])})
            prop = o['prop']
            if int(prop) != 0:
                rec.update({'exists': 1,
                            'pos': [_f(prop['pos'][a]) for a in 'xyz'],
                            # where the model is placed and the rooms are taken from;
                            # prop->pos is moved off it for some types (glass)
                            'rtpos': [_f(o['runtime_pos'][a]) for a in 'xyz'],
                            'rooms': _rooms(prop),
                            'rot': _mtx3(o['mtx']),
                            'damage': _f(o['damage']), 'maxdamage': _f(o['maxdamage']),
                            'rtflags': int(o['runtime_bitflags']) & 0xffffffff,
                            'attached': 1 if int(prop['parent']) else 0,
                            'propflags': int(prop['flags'])})
                if int(o['model']) != 0:
                    rec['scale'] = _f(o['model']['scale'])
                if t == 1:
                    # setupDoor() never runs domakedefaultobj()'s `damage = word / 65536`
                    # (prop.c:158): a door's float health words hold the setup's 16.16 integer
                    rec['damage'] = _f(_int_bits(o['damage']) / 65536.0)
                    rec['maxdamage'] = _f(_int_bits(o['maxdamage']) / 65536.0)
                    rec['health_raw'] = 1

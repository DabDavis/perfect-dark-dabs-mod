"""The Perfect Dark half of the twin driver: gdb Python against our port, booted
straight into a converted mission with --boot-ge-mission.

Load it from a gdb script with

    import os, sys; sys.path.insert(0, os.environ['GF_COMMON']); from gdbpd import *

and run under `twin.py pd`, which supplies the command line (--boot-ge-mission N
--skip-intro --skip-mission-intro --fixed-step --rng-seed 1 --no-sound
--savedir ...) from a run directory holding the binary, data/, added-content/
and mods/. The API is gdbge.py's, name for name, so one scenario can be written
once and run on both sides; positions are each game's own (ours = GoldenEye's
plus the level's offset, which worlddiff.py measures from the pads).

Traps: g_Difficulty is set at lvReset, before setupCreateProps() filters the
props by difficulty. The level clock is g_Vars.lvframe60 (60ths); with
--fixed-step every frame is exactly one tick. screenshotRequest() writes the
next frame; the file name is the pd.log line "screenshot: <path>".
"""
import gdb, os, json

gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set print elements 0')

SIDE = 'pd'
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


def boot(levelid=None, difficulty=0):
    """The mission is on the command line; set the difficulty before the props
    are made and run to the level's first frame. levelid is ignored (it is the
    oracle's), kept so a scenario calls boot() the same way on both sides."""
    gdb.execute('break lvReset')
    _go()
    gdb.execute('set variable g_Difficulty = %d' % difficulty)
    # a mission GE Plus's own folder screens start has this; --boot-ge-mission
    # does not, and the All Guns list and others key off it (gexfront.c)
    try:
        gdb.execute("set variable 'gexfront.c'::g_FrontInside = 1")
    except gdb.error:
        pass
    gdb.execute('delete')
    gdb.execute('break videoEndFrame if g_Vars.lvframenum >= 1')
    _go()
    gdb.execute('delete')
    say('boot stage', hex(int(ev('g_Vars.stagenum'))), 'difficulty', int(ev('g_Difficulty')),
        'tick', tick())


def tick():
    return int(ev('g_Vars.lvframe60'))


def until_tick(t):
    if tick() >= t:
        return
    gdb.execute('break videoEndFrame if g_Vars.lvframe60 >= %d' % t)
    _go()
    gdb.execute('delete')


def frames(n=1):
    target = int(ev('g_Vars.lvframenum')) + n
    gdb.execute('break videoEndFrame if g_Vars.lvframenum >= %d' % target)
    _go()
    gdb.execute('delete')


_padbuf = {'v': None}


def _pad(padnum):
    if _padbuf['v'] is None:
        gdb.execute('set variable $gfpad = (struct pad *)malloc(sizeof(struct pad))')
        _padbuf['v'] = True
    call('(void)padUnpack(%d, 0x42, $gfpad)' % padnum)
    return ev('$gfpad')


def place(x, y, z, theta=None, verta=None, room=None):
    """The player's feet at x, y, z (our world)."""
    P = 'g_Vars.currentplayer'
    for var, val in (('prop->pos.x', x), ('prop->pos.y', y + 160.0), ('prop->pos.z', z),
                     ('vv_manground', y), ('vv_ground', y)):
        gdb.execute('set variable %s->%s = %f' % (P, var, val))
    if theta is not None:
        gdb.execute('set variable %s->vv_theta = %f' % (P, theta))
    if verta is not None:
        gdb.execute('set variable %s->vv_verta = %f' % (P, verta))
    if room is not None:
        gdb.execute('set variable %s->prop->rooms[0] = %d' % (P, room))
        gdb.execute('set variable %s->prop->rooms[1] = -1' % P)


def hold(x, y, z, theta=None, verta=None, n=8, room=None):
    for _ in range(n):
        place(x, y, z, theta, verta, room)
        frames(3)
    place(x, y, z, theta, verta, room)
    frames(1)


def shot(path=None):
    """Asks for a screenshot of the next frame; the port names the file itself
    (pd.log's "screenshot:" line). path is ignored, kept for gdbge.py's API."""
    call('(void)screenshotRequest()')
    frames(2)
    return None


def finish():
    try:
        gdb.execute('kill')
    except gdb.error:
        pass
    gdb.execute('quit')


# ---------------------------------------------------------------- the world

OBJ_TYPES = {1, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 17, 20, 21, 36, 39, 40, 42, 43, 45, 47}
OBJTYPE_END = 0x34  # GoldenEye ends its list with 0x30; the conversion writes ours


def _f(v):
    return round(float(v), 3)


def _rooms(prop):
    out = []
    for r in range(8):
        b = int(prop['rooms'][r])
        if b < 0:
            break
        out.append(b)
    return out


def pads():
    out = []
    n = int(ev('g_PadsFile ? g_PadsFile->numpads : 0'))
    for i in range(n):
        p = _pad(i)
        out.append([i, _f(p['pos']['x']), _f(p['pos']['y']), _f(p['pos']['z']), int(p['room'])])
    return out


def _ailist_ids():
    ids = {}
    base = ev('g_StageSetup.ailists')
    if int(base) != 0:
        i = 0
        while i < 4000:
            rec = base[i]
            lst = int(rec['list'])
            if lst == 0:
                break
            ids[lst] = int(rec['id'])
            i += 1
    try:
        g = ev('g_GlobalAilists')
        for i in range(int(g.type.range()[1]) + 1):
            lst = int(g[i]['list'])
            if lst == 0:
                break
            ids[lst] = int(g[i]['id'])
    except gdb.error:
        pass
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
    p = ev('(unsigned int *)g_StageSetup.props')
    objptr = gdb.lookup_type('struct defaultobj').pointer()
    i = 0
    while i < 5000:
        o = p.cast(objptr).dereference()
        t = int(o['type'])
        if t == OBJTYPE_END:
            break
        words = int(ev('setupGetCmdLength((u32 *)%d)' % int(p)))
        rec = {'i': i, 'type': t, 'words': words}
        if t in OBJ_TYPES:
            rec.update({'model': int(o['modelnum']), 'pad': int(o['pad']),
                        'flags': int(o['flags']) & 0xffffffff, 'flags2': int(o['flags2']) & 0xffffffff,
                        'flags3': int(o['flags3']) & 0xffffffff, 'extrascale': int(o['extrascale'])})
            prop = o['prop']
            if int(prop) != 0:
                rec.update({'exists': 1,
                            'pos': [_f(prop['pos'][a]) for a in 'xyz'],
                            'rooms': _rooms(prop),
                            'rot': [[float(o['realrot'][r][c]) for c in range(3)] for r in range(3)],
                            'damage': _f(o['damage']), 'maxdamage': _f(o['maxdamage']),
                            'hidden': int(o['hidden']) & 0xffffffff, 'hidden2': int(o['hidden2']),
                            'attached': 1 if int(prop['parent']) else 0,
                            'propflags': int(prop['flags'])})
                if int(o['model']) != 0:
                    rec['scale'] = _f(o['model']['scale'])
                if t == 1:
                    d = p.cast(gdb.lookup_type('struct doorobj').pointer()).dereference()
                    # a door sits at startpos + unk98 * frac (propobj.c doorUpdatePosition)
                    rec['door_frac'] = _f(d['frac'])
                    rec['door_maxFrac'] = _f(d['maxfrac'])
                    rec['door_mode'] = int(d['mode'])
                    rec['door_startpos'] = [_f(d['startpos'][a]) for a in 'xyz']
                    rec['door_slide'] = [_f(d['unk98'][a]) for a in 'xyz']
            else:
                rec['exists'] = 0
        out.append(rec)
        p = p + words
        i += 1
    return out


def chrs():
    ids = _ailist_ids()
    out = []
    n = int(ev('g_NumChrSlots'))
    for k in range(n):
        c = ev('g_ChrSlots[%d]' % k)
        if int(c['chrnum']) < 0 or int(c['prop']) == 0:
            continue
        prop = c['prop']
        # a slot not in use can keep a stale prop; a live chr's prop points back at it
        try:
            if int(prop['chr']) != int(c.address):
                continue
        except gdb.MemoryError:
            continue
        ail = int(c['ailist'])
        rec = {'chrnum': int(c['chrnum']), 'slot': k,
               'headnum': int(c['headnum']), 'bodynum': int(c['bodynum']),
               'actiontype': int(c['actiontype']), 'sleep': int(c['sleep']),
               'chrflags': int(c['chrflags']) & 0xffffffff, 'hidden': int(c['hidden']),
               'flags': int(c['flags']) & 0xffffffff, 'flags2': int(c['flags2']) & 0xffffffff,
               'damage': _f(c['damage']), 'maxdamage': _f(c['maxdamage']),
               'morale': int(c['morale']), 'alertness': int(c['alertness']),
               'accuracyrating': int(c['accuracyrating']), 'speedrating': int(c['speedrating']),
               'visionrange': _f(c['visionrange']), 'hearingscale': _f(c['hearingscale']),
               'ailist': ids.get(ail, -1 if ail == 0 else 'ptr'), 'aioffset': int(c['aioffset']),
               'padpreset1': int(c['padpreset1']), 'chrpreset1': int(c['chrpreset1']),
               'team': int(c['team']), 'squadron': int(c['squadron']),
               'pos': [_f(prop['pos'][a]) for a in 'xyz'], 'rooms': _rooms(prop),
               'weapons': [_weaponnum(c['weapons_held'][h]) for h in range(2)]}
        if int(c['model']) != 0:
            rec['scale'] = _f(c['model']['scale'])
        if int(prop) == int(ev('g_Vars.currentplayer->prop')):
            rec['player'] = 1
        out.append(rec)
    return out


def player():
    P = ev('g_Vars.currentplayer')
    prop = P['prop']
    return {'pos': [_f(prop['pos'][a]) for a in 'xyz'], 'rooms': _rooms(prop),
            'theta': _f(P['vv_theta']), 'verta': _f(P['vv_verta']),
            'eye': [_f(P['cam_pos'][a]) for a in 'xyz'],
            'health': _f(P['bondhealth'])}


def world():
    return {'side': SIDE, 'tick': tick(), 'pads': pads(), 'props': props(), 'chrs': chrs(),
            'player': player()}


def emit(path, data):
    with open(path, 'w') as fh:
        json.dump(data, fh, separators=(',', ':'))
    say('wrote', path)

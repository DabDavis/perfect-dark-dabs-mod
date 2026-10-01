"""Print chosen setup records, their props and pads in full, on either side.

    twin.py both world/inspect.py --mission dam --out out/i \
        --env GF_RECORDS=292,307 [--env GF_PADS=74,10074] [--env GF_TICKS=1] [--env GF_EXPR='g_Vars.lvframe60;...']

Each record is printed by gdb as its own type (GoldenEye's ObjectRecord /
DoorRecord / ..., our struct defaultobj / doorobj / ...), so nothing is lost to
a dump format; the world diff names the records worth looking at. Output is the
gdb log (OUT/<side>/gdb.log), lines starting "GF".
"""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
side = os.environ['GF_SIDE']
lib = __import__('gdbge' if side == 'ge' else 'gdbpd')
gdb = lib.gdb

GE_TYPES = {1: 'DoorRecord', 9: 'GuardRecord', 47: 'TintedGlassRecord'}
PD_TYPES = {9: 'struct packedchr', 1: 'struct doorobj', 42: 'struct glassobj', 47: 'struct tintedglassobj', 8: 'struct weaponobj',
            39: 'struct chopperobj', 40: 'struct aircraftobj', 45: 'struct tankobj'}


def records(want):
    if side == 'ge':
        p = lib.ev('(unsigned int *)g_CurrentSetup.propDefs')
        size = lambda q: int(lib.ev('sizepropdef((PropDefHeaderRecord *)%d)' % int(q)))
        typeof = lambda q: int(q.cast(gdb.lookup_type('PropDefHeaderRecord').pointer()).dereference()['type'])
        end = 0x30
    else:
        p = lib.ev('(unsigned int *)g_StageSetup.props')
        size = lambda q: int(lib.ev('setupGetCmdLength((u32 *)%d)' % int(q)))
        typeof = lambda q: int(q.cast(gdb.lookup_type('struct defaultobj').pointer()).dereference()['type'])
        end = 0x34
    i = 0
    while i < 5000:
        t = typeof(p)
        if t == end:
            break
        if i in want:
            yield i, t, p
        p = p + size(p)
        i += 1


def show(i, t, p):
    if side == 'ge':
        ty = GE_TYPES.get(t, 'ObjectRecord' if t in lib.OBJ_TYPES else 'PropDefHeaderRecord')
    else:
        ty = PD_TYPES.get(t, 'struct defaultobj')
    lib.say('record', i, 'type', t, 'as', ty)
    # the raw words too: a type's struct can be wrong, the words cannot
    n = int(lib.ev('sizepropdef((PropDefHeaderRecord *)%d)' % int(p))) if side == 'ge' else \
        int(lib.ev('setupGetCmdLength((u32 *)%d)' % int(p)))
    lib.say('words', ' '.join('%08x' % (int(p[k]) & 0xffffffff) for k in range(min(n, 64))))
    print(gdb.execute('print *(%s *)%d' % (ty, int(p)), to_string=True), flush=True)
    if t in lib.OBJ_TYPES:
        o = p.cast(gdb.lookup_type(ty).pointer()) if side == 'ge' else p.cast(gdb.lookup_type('struct defaultobj').pointer())
        prop = o.dereference()['prop'] if side == 'ge' else o.dereference()['prop']
        if int(prop):
            print(gdb.execute('print *(%s)%d' % (prop.type, int(prop)), to_string=True), flush=True)


try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    for t in [int(x) for x in os.environ.get('GF_TICKS', '1').split(',')]:
        lib.until_tick(t)
        lib.say('tick', lib.tick())
        want = {int(x) for x in os.environ.get('GF_RECORDS', '').split(',') if x}
        for i, ty, p in records(want):
            show(i, ty, p)
        for pad in [int(x) for x in os.environ.get('GF_PADS', '').split(',') if x]:
            if side == 'ge':
                e = ('g_CurrentSetup.boundpads[%d]' % (pad - 10000)) if pad >= 10000 else ('g_CurrentSetup.pads[%d]' % pad)
                lib.say('pad', pad, gdb.execute('print ' + e, to_string=True).strip())
            else:
                q = lib._pad(pad)
                lib.say('pad', pad, gdb.execute('print *$gfpad', to_string=True).strip())
        for e in [x for x in os.environ.get('GF_EXPR', '').split(';') if x.strip()]:
            try:
                lib.say('expr', e, '=', gdb.execute('print ' + e, to_string=True).strip())
            except gdb.error as err:
                lib.say('expr', e, 'error', err)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

"""A converted mission's path-following vehicles, tick by tick, both sides.

    twin.py both world/vehpath.py --mission dam --out OUT --oracle ares [--env GF_LAST=5300]

Writes vehpath.json: for each vehicle (setup record order), samples of
(tick, x, y, z, roty, speed, turnrot60, nextstep). The cartridge is sampled
every video frame (a game frame is 2-3 of them, and its tick moves at the
game frame's start), ours every GF_EVERY ticks (default 2); vehdiff.py keeps
the cartridge's last sample of each tick and puts both on one clock.

GoldenEye's truck (propobj.c, PROPDEF_VEHICHLE): it steers towards the next
waypoint's pad with chrobjCallsApplySpeed() on roty (accelerating turn, capped
at 22.5 degrees a second) and pivots about its rear wheels - the step carries
a sideways term of the rear wheel node's z times sin(turn), sp460.
"""
import json, os, struct, sys
sys.path.insert(0, os.environ['GF_COMMON'])
SIDE = os.environ['GF_SIDE']
lib = __import__('gdbge' if SIDE == 'ge' else 'gdbpd')
LAST = int(os.environ.get('GF_LAST', '5300'))
EVERY = int(os.environ.get('GF_EVERY', '2'))


def ge_vehicles():
    import aresge as A
    setup = A.Rec('stagesetup', A.SYM['g_CurrentSetup'])
    p, out = setup.ptr('propDefs'), []
    for _ in range(5000):
        t = A.peek(p + 3, 1)[0]
        if t == A.PROPDEF_END:
            break
        if t == 39:
            out.append(p)
        p += 4 * A.SIZEPROPDEF.get(t, 1)
    return A, out


def ge_sample(A, v):
    b = A.peek(v, 0xb0)
    f = lambda o: struct.unpack_from('>f', b, o)[0]
    return [A.tick() if False else None, f(0x58), f(0x5c), f(0x60), f(0xa0), f(0x88), f(0x9c),
            struct.unpack_from('>i', b, 0xa8)[0]]


def pd_vehicles():
    p = lib.ev('g_Vars.activeprops')
    out = []
    while int(p) != 0:
        if int(p['type']) == 1 and int(p['obj']['type']) == 0x27:
            out.append(p)
        p = p['next']
    # setup order: by the object's index in the setup
    return sorted(out, key=lambda q: int(q['obj']['pad']))


def pd_sample(p):
    t = lib.ev('(struct truckobj *)%d' % int(p['obj']))
    return [None, float(p['pos']['x']), float(p['pos']['y']), float(p['pos']['z']), float(t['roty']),
            float(t['speed']), float(t['turnrot60']), int(t['nextstep'])]


import traceback
res = {'side': SIDE, 'vehicles': []}
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    if SIDE == 'ge':
        A, vs = ge_vehicles()
        res['vehicles'] = [[] for _ in vs]
        while lib.tick() <= LAST:
            t = lib.tick()
            for k, v in enumerate(vs):
                s = ge_sample(A, v)
                s[0] = t
                res['vehicles'][k].append(s)
            # one video frame, not one game frame: a game frame spans 2-3, and a
            # stop at a video frame's end can fall either side of propsTick();
            # the last sample of each tick is the one after the vehicle moved
            A.twin()('frames 1')
    else:
        vs = pd_vehicles()
        res['vehicles'] = [[] for _ in vs]
        t = 1
        while t <= LAST:
            lib.until_tick(t)
            for k, p in enumerate(vs):
                s = pd_sample(p)
                s[0] = lib.tick()
                res['vehicles'][k].append(s)
            t += EVERY
    lib.say('vehicles', len(res['vehicles']), 'samples', [len(x) for x in res['vehicles']])
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
json.dump(res, open(os.path.join(os.environ['GF_OUT'], 'vehpath.json'), 'w'))
lib.finish()

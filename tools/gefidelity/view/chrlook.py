"""Ours: Bond put in front of one chr at given distances, looking at its head,
a picture each - where a character's model is looked at close up (a head that
draws wrong, a LOD that switches early). Ours only; the chr's AI list is taken
away first so it stays where its setup put it.

    twin.py pd view/chrlook.py --mission miami --game gf --out OUT --env GF_CHR=4 --env GF_DISTS=60,150,300

Pictures are OUT/pd/shot_NNN.png in GF_DISTS order; chrlook.json has the
camera and the chr for each.
"""
import os, sys, json, math, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbpd')
ev = lib.ev
OUT = os.environ['GF_OUT']
CHR = int(os.environ.get('GF_CHR', '0'))
VERTA = float(os.environ.get('GF_VERTA', '-5'))
DISTS = [float(d) for d in os.environ.get('GF_DISTS', '60,150,300').split(',')]
res = []
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(int(os.environ.get('GF_AT', '60')))
    slot = None
    for k in range(int(ev('g_NumChrSlots'))):
        c = ev('g_ChrSlots[%d]' % k)
        if int(c['chrnum']) == CHR and int(c['prop']) != 0:
            slot = k
            lib.gdb.execute('set variable g_ChrSlots[%d].ailist = 0' % k)
    if slot is None:
        raise RuntimeError('no chr %d' % CHR)
    P = 'g_Vars.currentplayer'
    for d in DISTS:
        c = ev('g_ChrSlots[%d]' % slot)
        cx, cy, cz = (float(c['prop']['pos'][a]) for a in 'xyz')
        room = int(c['prop']['rooms'][0])
        me = ev(P + '->prop->pos')
        bx, bz = float(me['x']), float(me['z'])
        # come at the chr from where Bond stands now, d units off it
        dx, dz = bx - cx, bz - cz
        n = math.hypot(dx, dz) or 1.0
        px, pz = cx + dx / n * d, cz + dz / n * d
        # PD: theta 0 looks down +z, rising toward -x? measure it instead of assuming
        best = None
        for th in (math.degrees(math.atan2(-dx, -dz)), math.degrees(math.atan2(dx, dz)),
                   math.degrees(math.atan2(dx, -dz)), math.degrees(math.atan2(-dx, dz))):
            th %= 360
            lib.hold(px, float(me['y']) - 160.0, pz, th, -5.0, n=4, room=room)
            look = ev(P + '->cam_look')
            lx, lz = float(look['x']), float(look['z'])
            dot = (lx * (cx - px) + lz * (cz - pz)) / (math.hypot(lx, lz) * d or 1)
            if best is None or dot > best[0]:
                best = (dot, th)
        lib.hold(px, float(me['y']) - 160.0, pz, best[1], VERTA, n=6, room=room)
        lib.shot()
        cam = ev(P + '->cam_pos')
        res.append({'dist': d, 'theta': best[1], 'facing': round(best[0], 3),
                    'cam': [round(float(cam[a]), 1) for a in 'xyz'], 'chr': [round(v, 1) for v in (cx, cy, cz)],
                    'body': int(c['bodynum']), 'head': int(c['headnum'])})
        lib.say('shot at', d, res[-1])
    json.dump(res, open(os.path.join(OUT, 'chrlook.json'), 'w'), indent=1)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

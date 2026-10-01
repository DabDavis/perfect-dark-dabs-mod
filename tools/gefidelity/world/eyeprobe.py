"""Bond's eye over his floor, standing, squatting and half crouched, both sides.

    twin.py both world/eyeprobe.py --mission dam --out OUT --oracle ares

GoldenEye: eyeheight = headpos.y * player_perspective_height + 7, the camera at
field_70 + eyeheight + (field_88 + ducking_height_offset) * pph (bondview2.c).
Ours: vv_height = headpos.y / standheight * vv_eyeheight, the camera at
vv_manground + vv_height + crouchoffsetrealsmall + crouchheight (bondwalk.c).
Writes eye.json: per sample the eye over the floor and the terms behind it.
"""
import json, os, struct, sys
sys.path.insert(0, os.environ['GF_COMMON'])
SIDE = os.environ['GF_SIDE']
lib = __import__('gdbge' if SIDE == 'ge' else 'gdbpd')
if SIDE == 'ge':
    import aresge as A   # the cartridge only: Rec, poke and the player's address (not in gdbge's *)

STAND_AT = int(os.environ.get('GF_EYE_STAND', '903'))


def sample(tag):
    if SIDE == 'ge':
        P = A.Rec('struct player', A._player())
        eye = P.vec('field_488.pos')[1]
        d = {'tag': tag, 'tick': lib.tick(), 'eye_over_floor': eye - P.f('field_70'),
             'standheight': P.f('standheight'), 'headpos_y': P.vec('headpos')[1], 'eyeheight': P.f('eyeheight'),
             'field_88': P.f('field_88'), 'ducking': P.f('ducking_height_offset'), 'crouchpos': P['crouchpos']}
    else:
        P = lib.ev('g_Vars.currentplayer')
        f = lambda e: float(lib.ev('g_Vars.currentplayer->' + e))
        d = {'tag': tag, 'tick': lib.tick(), 'eye_over_floor': f('cam_pos.y') - f('vv_manground'),
             'standheight': f('standheight'), 'headpos_y': f('headpos.y'), 'vv_eyeheight': f('vv_eyeheight'),
             'vv_height': f('vv_height'), 'crouchoffset': f('crouchoffset'), 'crouchoffsetrealsmall': f('crouchoffsetrealsmall'),
             'crouchheight': f('crouchheight'), 'crouchpos': int(lib.ev('g_Vars.currentplayer->crouchpos'))}
    lib.say('EYE', json.dumps(d))
    return d


def crouch(pos):
    if SIDE == 'ge':
        A.poke(A._player() + A.T['struct player']['fields']['crouchpos']['off'], struct.pack('>i', pos))
    else:
        lib.ev('g_Vars.currentplayer->crouchpos = %d' % pos)


out = []
import traceback
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(1)
    out.append(sample('spawn'))
    for t in (STAND_AT, STAND_AT + 60):
        lib.until_tick(t)
        out.append(sample('stand'))
    for pos, name in ((0, 'squat'), (1, 'half'), (2, 'stand-again')):
        crouch(pos)
        base = lib.tick()
        for dt in (6, 12, 120):
            lib.until_tick(base + dt)
            out.append(sample('%s+%d' % (name, dt)))
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
with open(os.path.join(os.environ['GF_OUT'], 'eye.json'), 'w') as fh:
    json.dump(out, fh, indent=1)
lib.finish()

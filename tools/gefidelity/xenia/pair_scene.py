"""One matched picture, Bean (GF_SIDE ge through xenia/run_scenario.py) or ours
(GF_SIDE pd through twin.py): Bond on pad PAIR_PAD's own floor tile, heading
PAIR_THETA, pitch PAIR_VERTA, at level tick PAIR_SETTLE (after the opening).

On Bean it also takes the controls: the same spot photographed twice, a few
frames apart (shot_a, shot_b), and a poke read back (the heading written,
then read) - recorded in pair.json beside the shots.
"""
import os, sys, json, struct, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
side = os.environ['GF_SIDE']
lib = __import__('gdbge' if side == 'ge' else 'gdbpd')
OUT = os.environ['GF_OUT']
PAD = int(os.environ.get('PAIR_PAD', '0'))
TH = float(os.environ.get('PAIR_THETA', '0'))
VA = float(os.environ.get('PAIR_VERTA', '0'))
SETTLE = int(os.environ.get('PAIR_SETTLE', '900'))
info = {'pad': PAD, 'theta': TH, 'verta': VA, 'settle': SETTLE}
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(SETTLE)
    if getattr(lib, 'SIDE', '') == 'xenia':
        lib.hold_pad(PAD, TH, VA)
        lib.shot(os.path.join(OUT, 'shot_a.png'))
        lib.frames(6)
        lib.hold_pad(PAD, TH, VA)
        lib.shot(os.path.join(OUT, 'shot_b.png'))
        P = lib.u32(lib.PLAYERS)
        lib.poke(P + lib.PL_THETA, struct.pack('>f', 123.25))
        back = lib.f32(P + lib.PL_THETA)
        lib.poke(P + lib.PL_THETA, struct.pack('>f', TH))
        info.update({'poke_readback': back, 'poke_ok': back == 123.25, 'player': lib.player(),
                     'tick': lib.tick()})
    else:
        p = lib._pad(PAD)
        x, y, z = (float(p['pos'][a]) for a in 'xyz')
        room = int(p['room'])
        lib.hold(x, y, z, TH, VA, room=room)
        lib.shot()
        info.update({'player': lib.player(), 'tick': lib.tick()})
    json.dump(info, open(os.path.join(OUT, 'pair.json'), 'w'), indent=1)
    lib.say('pair done', json.dumps(info)[:200])
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

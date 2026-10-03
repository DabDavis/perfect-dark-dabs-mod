"""One shot or more on the cartridge (twin.py ge --oracle ares --render) at
SPOT="x,z,theta,verta;..." in GoldenEye world units: Bond stood on the nearest
pad's tile, AI frozen, spot_N.ppm per spot. For checking an F3 picture against
the cartridge at the tester's own camera (ours = GoldenEye + the level offset)."""
import os, sys, math
sys.path.insert(0, os.environ['GF_COMMON'])
import gdbge as lib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as A
OUT = os.environ['GF_OUT']
lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
A.freeze_ai()
A.first_person()
spots = [tuple(float(v) for v in s.split(',')) for s in os.environ['SPOT'].split(';')]
pads = [r for r in lib.pads()]
t = max(400, lib.tick() + 40)
for i, (x, z, th, va) in enumerate(spots):
    best = min(pads, key=lambda r: (r[1]-x)**2 + (r[3]-z)**2)
    stan = lib.pad_tile(best[0])
    lib.say('spot', i, 'nearest pad', best[0], best[1:4])
    A.stand(x, best[2], z, th, va, stan, t, lib.tick)
    lib.say('cam', A.camera())
    A.shoot(os.path.join(OUT, 'spot_%d.ppm' % i))
    t += 40

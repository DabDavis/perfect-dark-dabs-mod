"""spot3_ares.py with Bond on the tile under each spot (wide_ares.tile_under())
rather than the nearest pad's: SPOT="x,y,z,theta,verta;..." (GoldenEye's
world, y the feet). The tile's room is said, so a room's popping can be read."""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
import gdbge as lib
import wide_ares as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as A
OUT = os.environ['GF_OUT']
lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
A.freeze_ai()
A.first_person()
spots = [tuple(float(v) for v in s.split(',')) for s in os.environ['SPOT'].split(';')]
t = max(400, lib.tick() + 40)
for i, (x, y, z, th, va) in enumerate(spots):
    tu = W.tile_under(x, y + 30, z)
    stan = tu[0] if tu else None
    lib.say('spot', i, 'tile', hex(stan) if stan else None, 'floor', tu[1] if tu else None)
    A.stand(x, y, z, th, va, stan, t, lib.tick)
    lib.say('cam', A.camera())
    A.shoot(os.path.join(OUT, 'spot_%d.ppm' % i))
    t += 40

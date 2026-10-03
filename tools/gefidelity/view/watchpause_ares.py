"""The pause watch on the cartridge: Bond held at a pitch (WP_VERTA, default
-75), START pressed, a picture every WP_EVERY frames while the arm comes up,
START again and pictures while it goes down. pause_<n>.ppm / close_<n>.ppm in
GF_OUT. For comparing the arm's way in and out with gewatch.c's."""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
import gdbge as lib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as A
OUT = os.environ['GF_OUT']
VA = float(os.environ.get('WP_VERTA', '-75'))
EVERY = int(os.environ.get('WP_EVERY', '3'))
N = int(os.environ.get('WP_N', '30'))
lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
A.freeze_ai()
A.first_person()
c = A.camera()
x, y, z = c['feet']
for _ in range(10):
    lib.place(x, y, z, c['theta'], VA, None)
    A.no_lookahead()
    lib.frames(3)
lib.say('cam', A.camera())
tw = lib.twin()
def press(btn):
    f = A.frame()
    tw('cue %d 0 %s' % (f + 2, btn))
    tw('cue %d 0 -' % (f + 8))
press('START')
for i in range(N):
    lib.frames(EVERY)
    A.shoot(os.path.join(OUT, 'pause_%02d.ppm' % i))
    lib.say('pause', i, A.camera()['verta'])
lib.frames(60)
press('START')
for i in range(N):
    lib.frames(EVERY)
    A.shoot(os.path.join(OUT, 'close_%02d.ppm' % i))
    lib.say('close', i, A.camera()['verta'])

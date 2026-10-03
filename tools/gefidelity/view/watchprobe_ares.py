"""watchpause_ares.py's walk with the player's watch fields read each step (no pictures)."""
import os, sys, struct
sys.path.insert(0, os.environ['GF_COMMON'])
import gdbge as lib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as A
VA = float(os.environ.get('WP_VERTA', '-75'))
lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
A.freeze_ai()
A.first_person()
c = A.camera()
x, y, z = c['feet']
for _ in range(10):
    lib.place(x, y, z, c['theta'], VA, None)
    A.no_lookahead()
    lib.frames(3)
F = lib.T['struct player']['fields']
P = lib.u32(lib.SYM['g_CurrentPlayer'])
def f(name, n=1):
    return [round(v, 3) for v in struct.unpack('>%df' % n, lib.peek(P + F[name]['off'], 4 * n))]
MB = P + F['something_with_watch_object_instance']['off']
def mdump(tag):
    scale = struct.unpack('>f', lib.peek(MB + 20, 4))[0]
    anim, = struct.unpack('>I', lib.peek(MB + 32, 4))
    fa, fb = struct.unpack('>hh', lib.peek(MB + 48, 4))
    mp, = struct.unpack('>I', lib.peek(MB + 12, 4))
    out = 'scale %.4f anim %08x fa %d fb %d mtx %08x' % (scale, anim, fa, fb, mp)
    if (mp >> 24) == 0x80:
        raw = lib.peek(mp, 64 * 9)
        for j in range(9):
            ip = struct.unpack_from('>16h', raw, 64 * j)
            fp = struct.unpack_from('>16H', raw, 64 * j + 32)
            m = [(ip[k] * 65536 + fp[k]) / 65536.0 for k in range(16)]
            sc = (m[0]**2 + m[1]**2 + m[2]**2) ** 0.5
            out += ' m%d(%.1f %.1f %.1f s%.4f)' % (j, m[12], m[13], m[14], sc)
    lib.say(tag, out)
def dump(tag):
    mdump('M' + tag)
    lib.say(tag, A.frame(), 'hbo', f('headbodyoffset', 3), 'col', f('field_488.collision_position', 3), 'eye', f('field_488.pos', 3),
            'tt', f('field_488.theta_transform', 3), 'cnt', f('pause_animation_counter'), 'adj', f('pause_watch_related_adjust'),
            'verta', A.camera()['verta'], 'scale', f('watch_scale_destination'), 'pwp', f('pause_watch_position'), 'fovy', f('zoominfovy'))
tw = lib.twin()
def press(btn):
    fr = A.frame()
    tw('cue %d 0 %s' % (fr + 2, btn))
    tw('cue %d 0 -' % (fr + 8))
dump('before')
press('START')
for i in range(40):
    lib.frames(2); dump('pause%d' % i)
lib.frames(60)
press('START')
for i in range(40):
    lib.frames(2); dump('close%d' % i)

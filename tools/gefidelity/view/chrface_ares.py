"""A chr brought in front of Bond on the cartridge and pictured from four sides -
where a head's shape is in question (ROM hacks' own heads). Ares only:

    GF_GAME=tnd GF_LEVELID=25 GF_CHR=2 GF_OUT=... python3 view/chrface_ares.py

GF_LEVELID is the level number, GF_DIST the distance, GF_AT the tick to wait for.
Pictures are OUT/chr<n>_<angle>.ppm."""
import os, sys, math, struct, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
CHR = int(os.environ.get('GF_CHR', '0'))
DIST = float(os.environ.get('GF_DIST', '70'))
OUT = os.environ['GF_OUT']
try:
    lv = os.environ['GF_LEVELID']
    lib.boot(int(lv) if lv.isdigit() else lv, int(os.environ.get('GF_DIFF', '0')))
    lib.until_play()
    lib.until_tick(max(lib.tick(), int(os.environ.get('GF_AT', '120'))))
    cs = lib.chrs()
    for c in cs:
        lib.say('chr', c['chrnum'], 'body', c['bodynum'], c['bodyfile'], 'head', c['headnum'], c['headfile'], 'pos', c['pos'])
    c = [c for c in cs if c['chrnum'] == CHR][0]
    base = lib.u32(lib.SYM['g_ChrSlots'])
    size = lib.T['ChrRecord']['size']
    lib.poke(base + c['slot'] * size + lib.T['ChrRecord']['fields']['ailist']['off'], b'\0\0\0\0')
    pl = lib.player()
    lib.say('player', pl)
    # Bond on the chr's own pad tile, DIST off it, looking at it (GoldenEye's
    # theta turns the other way from Perfect Dark's)
    padnum = int(os.environ.get('GF_PAD', '-1'))
    stan = lib.pad_tile(padnum) if padnum >= 0 else None
    cx, cy, cz = c['pos']
    for ang in [float(a) for a in os.environ.get('GF_ANGS', '0,90,180,270').split(',')]:
        a = math.radians(ang)
        bx, bz = cx + math.sin(a) * DIST, cz + math.cos(a) * DIST
        th = (-math.degrees(math.atan2(cx - bx, cz - bz))) % 360
        lib.hold(bx, cy, bz, th, float(os.environ.get('GF_VERTA', '5')), n=8, stan=stan)
        path = os.path.join(OUT, 'chr%d_%03d.ppm' % (CHR, int(ang)))
        lib.shot(path)
        lib.frames(2)
        lib.say('shot', path, lib.player())
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

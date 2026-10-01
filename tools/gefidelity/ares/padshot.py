"""Bond on a pad's own floor tile, a picture per heading - on the cartridge.

    twin.py ge ares/padshot.py --oracle ares --mission frigate --out OUT \
        --env GF_PAD=151 --env GF_HEADS=0,90,180,270 [--env GF_AT=880]

Only the shared API plus aresge's pad_tile()/poke(), so it needs no function
calls: guards' AI lists are cleared by a write (as view/tour.py's freeze_ai()
does under gdb), and the game's own walk code sets Bond's height from the tile
over the frames hold() keeps him there. Pictures are OUT/ge/p<pad>_h<head>.ppm,
the whole framebuffer the game hands to osViSwapBuffer.
"""
import os, sys, struct, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')

PAD = int(os.environ.get('GF_PAD', '151'))
HEADS = [float(h) for h in os.environ.get('GF_HEADS', '0,90,180,270').split(',')]
AT = int(os.environ.get('GF_AT', '880'))
VERTA = float(os.environ.get('GF_VERTA', '-5'))
OUT = os.environ['GF_OUT']

try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(2)
    # still every chr: ailist = 0
    base = lib.u32(lib.SYM['g_ChrSlots'])
    size = lib.T['ChrRecord']['size']
    off = lib.T['ChrRecord']['fields']['ailist']['off']
    for k in range(lib.s32(lib.SYM['g_NumChrSlots'])):
        lib.poke(base + k * size + off, b'\0\0\0\0')
    pad = [p for p in lib.pads() if p[0] == PAD][0]
    stan = lib.pad_tile(PAD)
    lib.say('pad', pad, 'tile 0x%08x' % stan)
    for h in HEADS:
        lib.hold(pad[1], pad[2], pad[3], h, VERTA, n=10, stan=stan)
        lib.until_tick(max(lib.tick(), AT))
        lib.place(pad[1], pad[2], pad[3], h, VERTA, stan)
        lib.frames(2)
        path = os.path.join(OUT, 'p%04d_h%03d.ppm' % (PAD, int(h)))
        lib.shot(path)
        pl = lib.player()
        lib.say('shot', path, 'tick', lib.tick(), 'eye', pl['eye'], 'theta', pl['theta'])
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

"""Ours only, a diagnostic: a picture at GF_AT (default tick 300), then every
loaded room's G_SETCOMBINE whose second cycle's alpha is 0 (Ad1 = 7) patched in
memory to ENVIRONMENT (5) and a second picture. Where a level's walls appear in
the second, the combined alpha 0 is what hid them: the RDP takes coverage for
alpha under ALPHA_CVG_SEL, our renderer blends by the combiner's (Goldfinger
64's Cartel, Bodega, China, Crab Key). The patch is crude - it hits other
draws too (Grounds' and Alps' terrain go dark) - so it says where to look, it
is not a fix.

    twin.py pd view/roomalpha.py --mission cartel --game gf --out OUT
"""
import os, sys, struct
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbpd')
lib.boot(None, 0)
lib.until_tick(int(os.environ.get('GF_AT', '300')))
lib.shot()
inf = lib.gdb.selected_inferior()
n = int(lib.ev('g_Vars.roomcount'))
patched = 0
for r in range(1, n):
    g = int(lib.ev('(long)g_Rooms[%d].gfxdata' % r))
    if not g:
        continue
    start = int(lib.ev('(long)g_Rooms[%d].gfxdata->colours' % r))
    try:
        blob = bytearray(inf.read_memory(start, 0x20000))
    except Exception:
        continue
    for o in range(0, len(blob) - 16, 8):
        w0, w1 = struct.unpack_from('<QQ', blob, o)
        if w0 >> 32 == 0 and (w0 >> 24) == 0xfc and w1 >> 32 == 0 and (w1 & 7) == 7:
            inf.write_memory(start + o + 8, struct.pack('<Q', (w1 & ~7) | 5))
            patched += 1
lib.say('patched', patched)
lib.frames(4)
lib.shot()
lib.finish()

"""texsample_ares.py's GF_PASSES and GF_CI on our side: hold at a camera, take
a picture, then one a pass with an image rewritten in our memory and the
renderer's texture cache cleared so it is uploaded again.

    (mkdir -p OUT/pd/save; a pd.ini there with the view run's window, 640x440 FovY 60)
    twin.py pd view/texsample_pd.py --game gf --mission crabkey --bin ./pd.fix --out OUT \
        --env GF_FEET=-1904.89,-270,854.62 --env GF_HEAD=90 --env GF_AT=1398 \
        --env GF_TEXHEX=<first bytes> --env GF_CI=32

GF_FEET is x, the floor's height and z (a view run's pd/manifest.json: feet x
and z, ground). The image is found by GF_TEXHEX in the process's writable
memory (GF_HIT picks among several hits; GF_ADDR gives the address instead).
Our images hold their rows as the renderer reads them, the cartridge's odd rows
word-swapped, so the same GF_PASSES offsets mean the same texels only within a
row; GF_CI writes the ramps unswapped here, and the palette after the levels as
our lists load it (the cartridge's can sit elsewhere: GF_CIPAL there).
Pictures are OUT/pd/shot_000.png (as it is) and one a pass in order, with a
"GF pass <name>" line in gdb.log before each.
"""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbpd')
gdb = lib.gdb
try:
    lib.boot(os.environ['GF_LEVELID'], 0)
    lib.until_tick(int(os.environ.get('GF_AT', '400')))
    x, y, z = [float(v) for v in os.environ['GF_FEET'].split(',')]
    head = float(os.environ.get('GF_HEAD', '90'))
    verta = float(os.environ.get('GF_VERTA', '-5'))
    lib.hold(x, y, z, head, verta, n=6)
    inf = gdb.selected_inferior()
    sig = bytes.fromhex(os.environ['GF_TEXHEX'])
    if os.environ.get('GF_ADDR'):
        addr = int(os.environ['GF_ADDR'], 16)
    else:
        hits = []
        for line in open('/proc/%d/maps' % inf.pid):
            f = line.split()
            if 'rw' not in f[1]:
                continue
            lo, hi = (int(v, 16) for v in f[0].split('-'))
            a = lo
            while a < hi:
                try:
                    r = inf.search_memory(a, hi - a, sig)
                except gdb.error:
                    break
                if r is None:
                    break
                hits.append(r)
                a = r + 1
        lib.say('hits', ['0x%x' % h for h in hits])
        addr = hits[int(os.environ.get('GF_HIT', '0'))]
    got = bytes(inf.read_memory(addr, len(sig)))
    if got != sig:
        raise RuntimeError('GF_TEXHEX not at 0x%x: %s' % (addr, got.hex()))
    lib.shot()
    CI = int(os.environ.get('GF_CI', '0'))

    def ci_image(kind):
        # texsample_ares.py's ci_image() without the odd rows' swap
        out = bytearray()
        w, n = CI, 0
        while w >= 2:
            line = max(8, w)
            for yy in range(w):
                row = bytearray(line)
                for xx in range(w):
                    c = yy if kind == 'rows' else xx
                    row[xx] = 31 if kind == 'shade' else min(31, ((c << n) + (1 << n >> 1)) * 32 // CI)
                out += row
            w, n = w // 2, n + 1
        return bytes(out)

    passes = []
    if CI:
        cipal = int(os.environ.get('GF_CIPAL', '0'), 0) or len(ci_image('shade'))
        pal = b''.join(((i << 11) | (i << 6) | (i << 1) | 1).to_bytes(2, 'big') for i in range(32))
        passes = [(k, [('img', ci_image(k)), ('pal', pal)]) for k in ('shade', 'rows', 'cols')]
        size = max(len(ci_image('shade')), cipal + 64)
    else:
        for spec in os.environ['GF_PASSES'].split(';'):
            name, edits = spec.split('=', 1)
            passes.append((name, [tuple(int(v, 0) for v in e.split(':')) for e in edits.split('+')]))
        size = max(o + n for _, e in passes for o, _, n in e)
    orig = bytes(inf.read_memory(addr, size))
    for name, edits in passes:
        img = bytearray(orig)
        for e in edits:
            if e[0] == 'img':
                img[:len(e[1])] = e[1]
            elif e[0] == 'pal':
                img[cipal:cipal + 64] = e[1]
            else:
                off, byte, n = e
                img[off:off + n] = bytes([byte]) * n
        inf.write_memory(addr, bytes(img))
        lib.call('gfx_texture_cache_clear()')
        lib.hold(x, y, z, head, verta, n=2)
        lib.say('pass', name)
        lib.shot()
    inf.write_memory(addr, orig)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

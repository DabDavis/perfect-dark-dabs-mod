"""Where the cartridge samples an image: find it in RDRAM by its first bytes
(GF_TEXHEX, as our renderer loaded it), take a picture, then overwrite it with
a ramp along its rows and another along its columns and take one under each.
A surface's brightness under each ramp is its texel row and column.

    twin.py ge view/texsample_ares.py --oracle ares --game gf --mission cartel --out OUT \
        --env GF_PAD=134 --env GF_HEAD=270 --env GF_TEXHEX=<hex> --env GF_TEXW=64 --env GF_TEXH=64

Only I4 images for now (GF_TEXW x GF_TEXH, mip levels after it halving to 4).
The ramp is 4-bit, 16 steps of 4 texels on a 64-wide image, offset half an
image so the step it jumps at is in the middle, away from the wrap.
Pictures are OUT/ge/{orig,rows,cols}.ppm; with GF_FILL=<byte hex> the image is
filled with that byte instead, one picture OUT/ge/fill.ppm (the shade alone,
with ff). GF_FILLLEN=<bytes> fills that many bytes from each hit instead of the
I4 image and its mips: any format, or a CI image's palette (GF_TEXHEX its
first bytes, GF_FILL=ff: every entry white, the shade alone again).
GF_PASSES=name=OFF:BYTE:LEN+OFF:BYTE:LEN;name2=... takes one picture a pass,
OUT/ge/<name>.ppm, each with the image put back first and then LEN bytes from
OFF (hex or decimal, from each hit) set to BYTE: a CI image's mip levels each
given a different index, say, to see which level a surface draws.
GF_CI=<width> does the ramps on a square CI8 image and its mip levels (TMEM
lines of 8 bytes at least, odd rows word-swapped as RDRAM holds them; the
palette after the levels, or at GF_CIPAL): entries 0-31 grey 0-31, pictures
OUT/ge/{shade,rows,cols}.ppm - every texel 31, its row, its column (0-31
across level 0, levels scaled to match). rows/shade x 31 is then the t a
pixel samples, cols/shade the s: where a texgenned surface reads its map.
"""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as side  # noqa: E402

PAD = int(os.environ.get('GF_PAD', '134'))
HEAD = float(os.environ.get('GF_HEAD', '270'))
VERTA = float(os.environ.get('GF_VERTA', '-5'))
AT = int(os.environ.get('GF_AT', '400'))
SIG = bytes.fromhex(os.environ['GF_TEXHEX'])
W = int(os.environ.get('GF_TEXW', '64'))
H = int(os.environ.get('GF_TEXH', '64'))
OUT = os.environ['GF_OUT']
RAM = int(os.environ.get('GF_RAMSIZE', str(8 << 20)))
FILLLEN = int(os.environ.get('GF_FILLLEN', '0'))
CI = int(os.environ.get('GF_CI', '0'))


def ci_image(kind):
    """GF_CI: every texel 31 ('shade'), its row or its column, 0-31 across
    level 0 (a level's coordinate scaled to level 0's), odd rows word-swapped."""
    out = bytearray()
    w, n = CI, 0
    while w >= 2:
        line = max(8, w)
        for yy in range(w):
            row = bytearray(line)
            for xx in range(w):
                c = yy if kind == 'rows' else xx
                row[xx] = 31 if kind == 'shade' else min(31, ((c << n) + (1 << n >> 1)) * 32 // CI)
            if yy & 1:
                row = b''.join(row[i + 4:i + 8] + row[i:i + 4] for i in range(0, line, 8))
            out += row
        w, n = w // 2, n + 1
    return bytes(out)


def ci_palette():
    return b''.join(((i << 11) | (i << 6) | (i << 1) | 1).to_bytes(2, 'big') for i in range(32))


def ramp(w, h, k, cols):
    out = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            v = []
            for xx in (x, x + 1):
                c = (xx << k) if cols else (y << k)
                v.append(((c + W // 2) % W) * 16 // W if cols else ((c + H // 2) % H) * 16 // H)
            out.append((v[0] << 4) | v[1])
    return bytes(out)


def image(cols):
    out = b''
    w, h, k = W, H, 0
    while w >= 4 and h >= 4:
        out += ramp(w, h, k, cols)
        w, h, k = w // 2, h // 2, k + 1
    return out


try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(2)
    side.freeze_ai()
    side.first_person()
    x, y, z, stan = side.pad_pos(PAD)
    side.stand(x, y, z, HEAD, VERTA, stan, max(lib.tick(), AT) + 10, lib.tick)
    lib.say('camera', side.camera())
    lib.shot(os.path.join(OUT, 'orig.ppm'))
    hits = []
    chunk = 1 << 16
    for a in range(0, RAM, chunk):
        blob = lib.peek(0x80000000 + a, chunk + len(SIG))
        i = blob.find(SIG)
        while 0 <= i < chunk:
            hits.append(0x80000000 + a + i)
            i = blob.find(SIG, i + 1)
    lib.say('hits', ['0x%08x' % h for h in hits])
    if hits:
        size = FILLLEN or len(image(False))
        if CI:
            cipal = int(os.environ.get('GF_CIPAL', '0'), 0) or len(ci_image('shade'))
            size = max(len(ci_image('shade')), cipal + 64)
        orig = {h: lib.peek(h, size) for h in hits}
        for h in hits:
            lib.say('ram', '0x%08x' % h, orig[h][:48].hex(), 'mip1', orig[h][W * H // 2:W * H // 2 + 16].hex())
        fill = os.environ.get('GF_FILL')
        passes = [('fill', None)] if fill else [('rows', False), ('cols', True)]
        pokes = {}
        if CI:
            # the first picture after orig can be a frame stale: a throwaway first
            passes = [('warm', None), ('shade', None), ('rows', None), ('cols', None)]
        if os.environ.get('GF_PASSES'):
            passes = []
            for spec in os.environ['GF_PASSES'].split(';'):
                name, edits = spec.split('=', 1)
                passes.append((name, None))
                pokes[name] = [tuple(int(v, 0) for v in e.split(':')) for e in edits.split('+')]
            size = max([size] + [o + n for e in pokes.values() for o, _, n in e])
            orig = {h: lib.peek(h, size) for h in hits}
        for name, cols in passes:
            for h in hits:
                if name in pokes:
                    img = bytearray(orig[h])
                    for off, byte, n in pokes[name]:
                        img[off:off + n] = bytes([byte]) * n
                    lib.poke(h, bytes(img))
                    continue
                if CI:
                    img = bytearray(orig[h])
                    ci = ci_image('shade' if name == 'warm' else name)
                    img[:len(ci)] = ci
                    img[cipal:cipal + 64] = ci_palette()
                    lib.poke(h, bytes(img))
                    continue
                lib.poke(h, bytes([int(fill, 16)]) * size if fill else image(cols))
            lib.place(x, y, z, HEAD, VERTA, stan)
            side.no_lookahead()
            lib.frames(3)
            lib.shot(os.path.join(OUT, name + '.ppm'))
        for h in hits:
            lib.poke(h, orig[h])
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

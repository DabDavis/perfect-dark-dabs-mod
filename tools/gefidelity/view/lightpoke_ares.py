"""What the cartridge's room light does: GlobalLight (bg.c, the Lights1 every
level is lit by) found in RDRAM by its bytes and rewritten, one picture per
setting, with Bond held at pad GF_PAD heading GF_HEAD (the view tour's camera).

    twin.py ge view/lightpoke_ares.py --oracle ares --game gf --mission crabkey --out OUT \
        --env GF_PAD=85 --env GF_HEAD=90 \
        --env GF_LIGHTS='orig;px:0,255,127,0,0;nx:0,255,-127,0,0' \
        [--env GF_TEXHEX=<palette bytes> --env GF_FILLLEN=300]

GF_LIGHTS is a ;-list of settings, each `name:ambient,colour,dx,dy,dz` (one
grey ambient, one grey light, its direction as the s8 bytes gdSPDefLights1
takes), or `orig` for the light as the game has it. Pictures are
OUT/ge/<name>.ppm. With GF_TEXHEX (and GF_FILLLEN, as texsample_ares.py) the
bytes found there are filled with ff first: a CI image's palette made white,
so the surface's brightness is its shade alone. With ambient 0 and a light
dimmer than 255, the shade is the light times N.L unclamped: a direction along
a world axis and the same along an eye axis light different faces, which
tells which space the RSP reads the light in. More than one copy of the bytes
may be found (another table's, a gun's); GF_LIGHTONLY=<n> rewrites the n-th.
"""
import os, sys, struct, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as side  # noqa: E402

PAD = int(os.environ.get('GF_PAD', '134'))
HEAD = float(os.environ.get('GF_HEAD', '270'))
VERTA = float(os.environ.get('GF_VERTA', '-5'))
AT = int(os.environ.get('GF_AT', '400'))
OUT = os.environ['GF_OUT']
RAM = int(os.environ.get('GF_RAMSIZE', str(8 << 20)))
# GlobalLight as gdSPDefLights1(150,150,150, 255,255,255, 77,77,46) lays it out
LIGHTSIG = bytes.fromhex(os.environ.get('GF_LIGHTSIG', '9696960096969600ffffff00ffffff004d4d2e00'))
SETTINGS = [s for s in os.environ.get('GF_LIGHTS', 'orig').split(';') if s]
TEXSIG = bytes.fromhex(os.environ['GF_TEXHEX']) if os.environ.get('GF_TEXHEX') else None
FILLLEN = int(os.environ.get('GF_FILLLEN', '0'))


def find(sig):
    hits = []
    chunk = 1 << 16
    for a in range(0, RAM, chunk):
        blob = lib.peek(0x80000000 + a, chunk + len(sig))
        i = blob.find(sig)
        while 0 <= i < chunk:
            hits.append(0x80000000 + a + i)
            i = blob.find(sig, i + 1)
    return hits


def lights1(amb, col, d):
    a = bytes([amb] * 3 + [0]) * 2
    l = bytes([col] * 3 + [0]) * 2 + struct.pack('>bbbb', d[0], d[1], d[2], 0)
    return a + l


try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(2)
    side.freeze_ai()
    side.first_person()
    x, y, z, stan = side.pad_pos(PAD)
    side.stand(x, y, z, HEAD, VERTA, stan, max(lib.tick(), AT) + 10, lib.tick)
    lib.say('camera', side.camera())
    if TEXSIG:
        th = find(TEXSIG)
        lib.say('texhits', ['0x%08x' % h for h in th])
        for h in th:
            lib.poke(h, b'\xff' * FILLLEN)
    hits = find(LIGHTSIG)
    lib.say('lighthits', ['0x%08x' % h for h in hits])
    for h in hits:
        lib.say('around', '0x%08x' % h, lib.peek(h - 16, 64).hex())
    # GF_LIGHTONLY=<n>: rewrite only the n-th copy found (another may be a gun's)
    if os.environ.get('GF_LIGHTONLY'):
        hits = [hits[int(os.environ['GF_LIGHTONLY'])]]
    orig = {h: lib.peek(h, len(LIGHTSIG)) for h in hits}
    for s in SETTINGS:
        name, _, args = s.partition(':')
        for h in hits:
            if name == 'orig':
                lib.poke(h, orig[h])
            else:
                v = [int(t) for t in args.split(',')]
                lib.poke(h, lights1(v[0], v[1], v[2:5]))
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

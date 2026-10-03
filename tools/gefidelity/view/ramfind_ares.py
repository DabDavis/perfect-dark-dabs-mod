"""Count byte patterns (GF_PATS, comma-separated hex) in the cartridge's RDRAM
with Bond held at pad GF_PAD heading GF_HEAD (the view tour's camera), and
print where the first few are with the 16 bytes before each.

    twin.py ge view/ramfind_ares.py --oracle ares --game gf --mission cartel --out OUT \
        --env GF_PAD=134 --env GF_PATS=707684ff,e0ecffff
"""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as side  # noqa: E402

PAD = int(os.environ.get('GF_PAD', '134'))
HEAD = float(os.environ.get('GF_HEAD', '270'))
PATS = [bytes.fromhex(p) for p in os.environ['GF_PATS'].split(',')]
RAM = int(os.environ.get('GF_RAMSIZE', str(8 << 20)))

try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(2)
    side.freeze_ai()
    side.first_person()
    x, y, z, stan = side.pad_pos(PAD)
    side.stand(x, y, z, HEAD, -5.0, stan, lib.tick() + 20, lib.tick)
    hits = {p: [] for p in PATS}
    chunk = 1 << 16
    for a in range(0, RAM, chunk):
        blob = lib.peek(0x80000000 + a, chunk + 16)
        for p in PATS:
            i = blob.find(p)
            while 0 <= i < chunk:
                hits[p].append((0x80000000 + a + i, blob[max(0, i - 12):i + 4].hex()))
                i = blob.find(p, i + 1)
    for p, h in hits.items():
        lib.say('pat', p.hex(), 'count', len(h), [('0x%08x' % a, ctx) for a, ctx in h[:4]])
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

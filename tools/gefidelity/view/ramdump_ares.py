"""Dump the cartridge's RDRAM GF_FROM..GF_TO (hex) to OUT/ge/ram.bin with
Bond held at pad GF_PAD heading GF_HEAD (the view tour's camera).

    twin.py ge view/ramdump_ares.py --oracle ares --game gf --mission cartel --out OUT \
        --env GF_FROM=80080000 --env GF_TO=800c0000
"""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ares_side as side  # noqa: E402

PAD = int(os.environ.get('GF_PAD', '134'))
HEAD = float(os.environ.get('GF_HEAD', '270'))
A = int(os.environ['GF_FROM'], 16)
B = int(os.environ['GF_TO'], 16)

try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(2)
    side.freeze_ai()
    side.first_person()
    x, y, z, stan = side.pad_pos(PAD)
    side.stand(x, y, z, HEAD, -5.0, stan, lib.tick() + 20, lib.tick)
    with open(os.path.join(os.environ['GF_OUT'], 'ram.bin'), 'wb') as f:
        f.write(lib.peek(A, B - A))
    lib.say('dumped', hex(A), hex(B))
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

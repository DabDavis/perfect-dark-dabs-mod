"""The settled world of one mission, the same on both sides: every setup record,
every object's placement and state, every chr, the pads and the player.

    twin.py both world/dump.py --mission dam --out out/dam [--env GF_TICKS=1,300]

Writes world_t<N>.json per requested tick into GF_OUT; worlddiff.py compares the
two sides' files.
"""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge' if os.environ['GF_SIDE'] == 'ge' else 'gdbpd')

import traceback
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    for t in [int(x) for x in os.environ.get('GF_TICKS', '1,300').split(',')]:
        lib.until_tick(t)
        w = lib.world()
        w.update({'mission': int(os.environ['GF_MISSION']), 'difficulty': int(os.environ.get('GF_DIFF', '0')),
                  'asked': t})
        lib.emit(os.path.join(os.environ['GF_OUT'], 'world_t%d.json' % t), w)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

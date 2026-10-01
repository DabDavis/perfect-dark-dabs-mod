"""The settled world of one mission, the same on both sides: every setup record,
every object's placement and state, every chr, the pads and the player.

    twin.py both world/dump.py --mission dam --out out/dam [--env GF_TICKS=1,300]

Writes world_t<N>.json per requested tick into GF_OUT; worlddiff.py compares the
two sides' files.
"""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge' if os.environ['GF_SIDE'] == 'ge' else 'gdbpd')

# The wide dump as well - what each setup record decides beyond its placement,
# and the tile graph (common/wide_ares.py, wide_pd.py), at the first tick asked
# for only (the records are the setup's, read at the load). On by default for
# ours and the cartridge (GF_WIDE=auto); there is no wide reader for the native
# port or for Xenia, so those dump the narrow world whatever GF_WIDE says.
# GF_WIDE=0 turns it off.
wide = None
_backend_ok = os.environ['GF_SIDE'] == 'pd' or os.environ.get('GF_ORACLE', 'port') == 'ares' \
    or getattr(lib, 'ORACLE', None) == 'ares'
if os.environ.get('GF_WIDE', 'auto') in ('1', 'auto') and _backend_ok:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'common'))
    if os.environ['GF_SIDE'] == 'pd':
        import wide_pd as wide
    else:
        import wide_ares as wide

import traceback
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    for n, t in enumerate([int(x) for x in os.environ.get('GF_TICKS', '1,300').split(',')]):
        lib.until_tick(t)
        w = lib.world()
        if wide is not None and n == 0:
            w['wide'] = wide.wide()
        if wide is not None:
            w['wide_chrweapons'] = wide.chrweapons()
        w.update({'mission': int(os.environ['GF_MISSION']), 'difficulty': int(os.environ.get('GF_DIFF', '0')),
                  'asked': t})
        lib.emit(os.path.join(os.environ['GF_OUT'], 'world_t%d.json' % t), w)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

"""Pictures of a mission's opening (the intro cinema) at chosen level ticks,
on either side, the opening left to play (GF_MISSION_INTRO=1, which twin.py
and aresge.boot() honour). The question it answers: is what ours shows in an
opening shot what the cartridge shows at that tick?

    twin.py both view/introshot.py --mission miami --game gf --out OUT \
        --env GF_MISSION_INTRO=1 --env GF_SHOT_TICKS=300,600,900

The cartridge's pictures are OUT/ge/intro_t<tick>.ppm (the whole framebuffer);
ours are OUT/pd/shot_NNN.png in tick order. Each side also writes
intro_chrs.json: every chr's number, position, body and head at each tick (and
on the cartridge the files its body and head rows name).
"""
import os, sys, json, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge' if os.environ['GF_SIDE'] == 'ge' else 'gdbpd')
OUT = os.environ['GF_OUT']
if os.environ.get('GF_MISSION_INTRO') != '1':
    lib.say('FAILED: set GF_MISSION_INTRO=1, or the opening is skipped')
    lib.finish()
ticks = [int(t) for t in os.environ.get('GF_SHOT_TICKS', '300,600,900').split(',')]
seen = {}
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    for t in ticks:
        lib.until_tick(t)
        got = lib.tick()
        lib.shot(os.path.join(OUT, 'intro_t%05d.ppm' % t))
        seen[t] = {'tick': got, 'chrs': lib.chrs()}
        lib.say('shot', t, 'at tick', got)
    json.dump(seen, open(os.path.join(OUT, 'intro_chrs.json'), 'w'))
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

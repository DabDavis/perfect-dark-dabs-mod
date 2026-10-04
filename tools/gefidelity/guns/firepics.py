"""Each chosen hand item fired on the cartridge, photographed frame by frame -
where the muzzle flash's place is in question (Tomorrow Never Dies 64's MP5s
and Remington, F3 20261004-171602/-171616).

    GF_GAME=tnd GF_LEVELID=25 GF_ITEMS=10,15 GF_OUT=... python3 guns/firepics.py   (on the oracle host)

gunpics.py's give with a full reserve (gunscen_ares.give), GF_WAIT frames to
raise it, then Z held for GF_FRAMES frames, a picture each:
item_<n>_<k>_f<flash>.ppm, flash being hand field_87D (gunfire.c lights the
flash switch on it).
"""
import os, sys, traceback
os.environ.setdefault('GF_GUNSDIR', os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.environ['GF_COMMON'])
sys.path.insert(0, os.environ['GF_GUNSDIR'])
import gdbge as lib  # noqa: E402
import gunscen_ares as G  # noqa: E402

try:
    lv = os.environ['GF_LEVELID']
    lib.boot(int(lv) if lv.isdigit() else lv, int(os.environ.get('GF_DIFF', '0')))
    lib.until_play()
    tw = lib.twin()
    tw('pad 0 -')
    for _ in range(200):
        G.player_addrs()
        if G.rd(G.hand(0, 'weaponnum'), 4) != 0 and G.rd(G.hand(0, 'weapon_action_state'), 4) == 0:
            break
        G.frames(3)
    else:
        raise RuntimeError('Bond never stood idle with his gun drawn')
    G.player_addrs()
    G.wr(G.P['p'] + G.fo('struct player', 'cheatBondInvincible'), 1, 1)
    lib.say('removed', G.remove_chrs(), 'chrs at tick', lib.tick())
    wait = int(os.environ.get('GF_WAIT', '90'))
    nfr = int(os.environ.get('GF_FRAMES', '12'))
    for item in [int(x, 0) for x in os.environ['GF_ITEMS'].split(',')]:
        G.give(item)
        G.frames(wait)
        held = G.rd(G.hand(0, 'weaponnum'), 4)
        lib.say('item', item, 'held', held)
        tw('pad 0 Z')
        for k in range(nfr):
            G.frames(1)
            fl = G.rd(G.hand(0, 'field_87D'), 1, False)
            lib.shot(os.path.join(os.environ['GF_OUT'], 'item_%d_%02d_f%d.ppm' % (item, k, fl)))
        tw('pad 0 -')
        G.frames(60)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

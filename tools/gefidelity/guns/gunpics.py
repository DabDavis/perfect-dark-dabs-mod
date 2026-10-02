"""Each chosen hand item in Bond's hand on the cartridge, photographed - what
settles whether a gun drawn otherwise on ours is the original's art or lost on
the way (Goldfinger 64's flat-grey pistols, 2026-10-02).

    twin.py ge guns/gunpics.py --game gf --mission cartel --render --out OUT --env GF_ITEMS=4,7,8,21

GF_ITEMS: the cartridge's own item numbers (a ROM hack's are its own: CLAUDE-
notes/goldfinger64.md). The give is gunscen_ares.py's - GoldenEye's
bondinvAddInvItem and gunRequestHandWeaponChange replayed as writes - with no
ammunition, so each is photographed drawn and idle, GF_WAIT frames after the
request (default 90). Writes item_<n>.ppm into GF_OUT; the line "GF item N
held H" says which item the hand took, and a picture whose hand holds another
item is labelled so.
"""
import os, sys, traceback
os.environ.setdefault('GF_GUNSDIR', os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.environ['GF_COMMON'])
sys.path.insert(0, os.environ['GF_GUNSDIR'])
import gdbge as lib  # noqa: E402
import gunscen_ares as G  # noqa: E402

try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
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
    lib.say('start item', G.rd(G.hand(0, 'weaponnum'), 4))
    wait = int(os.environ.get('GF_WAIT', '90'))
    for item in [int(x, 0) for x in os.environ['GF_ITEMS'].split(',')]:
        G.inv_add(item)
        G.request(0, item, 1)
        G.frames(wait)
        held = G.rd(G.hand(0, 'weaponnum'), 4)
        name = 'item_%d.ppm' % item if held == item else 'item_%d_held_%d.ppm' % (item, held)
        lib.shot(os.path.join(os.environ['GF_OUT'], name))
        lib.say('item', item, 'held', held, 'state', G.rd(G.hand(0, 'weapon_action_state'), 4))
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

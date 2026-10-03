"""A mission's pickups picked up as each game picks them up: Bond put on each
chosen record's prop in turn and held there, then what the inventory gained,
whether the prop went and where the objectives stand - the same on both sides.

    twin.py both world/collect.py --game gf --mission miami --out OUT --env GF_RECORDS=548
    world/collectdiff.py OUT

GF_RECORDS: setup record numbers, comma-separated (the two sides' records are
one for one). Ours without it takes every pickup on Perfect Dark's key cards
0x45-0x4c (a ROM hack's collectables, converter 104) and prints them; the
cartridge has no such mark and needs the list. The walk starts when each side's
first-person play does (the cartridge's opening still is dismissed at 190
frames, and its objective statuses only move in play), or at tick GF_AT if
later. Writes collect.json into GF_OUT.

Control, before the first pickup: Bond held where he stands for as long as a
pickup's hold must gain nothing; it did, or the objectives' count is out of
range, and the run says FAILED rather than report. The diff's null:
--env GF_PLANT_SKIP=<record> on one side leaves that record where it is, and
collectdiff.py must name it (Miami's 548: carried and objectives, 2026-10-02).
"""
import os, sys, json, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
SIDE = os.environ['GF_SIDE']
lib = __import__('gdbge' if SIDE == 'ge' else 'gdbpd')
HOLD = 6


def put(pos, where, floor=None):
    """Bond onto pos and held there; whether he stands on the floor asked for
    (the cartridge's walk once put him on a roof 426 above after a long jump
    between Shipyard's gold bars - a pickup he cannot reach is no finding)."""
    want = floor if floor is not None else pos[1]
    for attempt in range(3):
        # the cartridge places Bond on a tile, ours in a room (each side's prop_tile())
        if SIDE == 'ge':
            lib.hold(pos[0], pos[1], pos[2], n=HOLD, stan=where, floor=floor)
        else:
            lib.hold(pos[0], pos[1], pos[2], n=HOLD, room=where)
        lib.frames(10)
        g = lib.player().get('ground')
        if g is None or want is None or abs(g - want) < 30:
            return True
    return False


def ours_keycards():
    import gdb
    out = []
    for i in range(3000):
        o = gdb.parse_and_eval('setupGetObjByCmdIndex(%d)' % i)
        if int(o) == 0 or int(o['type']) != 8:
            continue
        w = int(gdb.parse_and_eval('((struct weaponobj *)%d)->weaponnum' % int(o)))
        if 0x45 <= w <= 0x4c:
            out.append(i)
    return out


try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    if SIDE == 'pd':
        lib.gdb.execute('set variable g_Vars.currentplayer->invincible = 1')
    # both in play: the cartridge's objective statuses stand still through its opening
    lib.say('play at tick', lib.until_play())
    if os.environ.get('GF_AT'):
        lib.until_tick(int(os.environ['GF_AT']))
    recs = [int(x) for x in os.environ.get('GF_RECORDS', '').split(',') if x.strip()]
    if not recs:
        if SIDE != 'pd':
            raise RuntimeError('the cartridge needs GF_RECORDS')
        recs = ours_keycards()
        lib.say('records', ','.join(str(i) for i in recs))
    res = {'mission': int(os.environ['GF_MISSION']), 'records': [], 'start_tick': lib.tick(),
           'objectives_before': lib.objectives(), 'inventory_before': lib.inventory()}
    # the null: a hold on Bond's own spot gains nothing
    me = lib.player()['pos']
    put(me, lib.bond_where())
    if lib.inventory() != res['inventory_before']:
        raise RuntimeError('control: holding Bond on his own spot changed the inventory %s -> %s'
                           % (res['inventory_before'], lib.inventory()))
    for i in recs:
        floor = None
        before = lib.inventory()
        pt = lib.prop_tile(i)
        row = {'i': i, 'had_prop': pt is not None}
        if pt is not None and SIDE == 'ge':
            # the cartridge's prop keeps its pad's tile, which may be another
            # floor (Plane's pad 0x56); stand Bond on the floor under it, as
            # ours finds it (gdbpd.prop_tile)
            import wide_ares
            under = wide_ares.tile_under(*pt[0])
            floor = None
            if under:
                floor = under[1]
                if under[0] != pt[1]:
                    row['tile_moved'] = '%#x -> %#x' % (pt[1], under[0])
                    pt = (pt[0], under[0]) + tuple(pt[2:])
        if pt is not None:
            row['pos'] = pt[0]
            if len(pt) > 2:
                row['prop'] = pt[2]
            if str(i) != os.environ.get('GF_PLANT_SKIP'):
                row['reached'] = put(pt[0], pt[1], floor if SIDE == 'ge' else None)
            else:
                lib.frames(HOLD * 3 + 11)   # the planted fault: Bond never goes to it
            after = lib.inventory()
            pl = lib.player()
            row['bond'] = pl['pos']
            row['ground'] = pl.get('ground')
            row['gained'] = [w for w in after if w not in before]
            row['carried'] = lib.carries(i)
            now = lib.prop_tile(i)
            row['gone'] = now is None
            if now is not None and len(now) > 2:
                row['prop_after'] = now[2]
            row['objectives'] = lib.objectives()
        lib.say('record', i, json.dumps(row))
        res['records'].append(row)
    lib.frames(30)
    res.update({'objectives_after': lib.objectives(), 'inventory_after': lib.inventory(), 'end_tick': lib.tick()})
    lib.emit(os.path.join(os.environ['GF_OUT'], 'collect.json'), res)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

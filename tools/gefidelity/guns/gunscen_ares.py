"""GoldenEye's side of the gun diff on the REAL cartridge: the US ROM in ares
through n64twin, the same timeline and the same gun_<item>.json as
gunscen.py writes from the native port, so gundiff.py reads either.

Run by guns/run.py with --oracle ares (twin.run_ge's ares branch runs it as
plain python on the oracle host; GF_GUNS, GF_GUNSDIR, GF_OUT as for gunscen.py).

What replaces the port's gdb, and why:
- **Events** are n64twin `trace`s at function entries (ares only fires PC hooks
  at function entries): the clock word (g_GlobalTimer) is logged with each, so
  an event's tick is exact, not a frame's. A sound's id is sndPlaySfx's a1; its
  callers are ra plus the return addresses found in 24 words of its stack (a
  heuristic stack walk - MIPS keeps no frame chain; stale words can add a name).
  A shot is joyRumblePakStart called from gunTickHandState (ra in its range).
- **Per-frame state** is an n64twin `watch` at lvlRender: both hands' first 48
  bytes and the ammo array, read in the frame, no stop.
- **Input** is n64twin `pad-when`: Z pressed and released when g_GlobalTimer
  reaches the schedule's ticks, read by the game's own controller poll.
- **The give** cannot call a function on the console, so it is GoldenEye's own
  code replayed as writes, each line from the decomp: bondinvAddInvItem (a free
  p_itemcur slot linked in and sorted as bondinvSortInv does),
  give_cur_player_ammo, gunRequestHandWeaponChange(0, item, 1), and the dual
  rule bondinvItemAvailableForHand(item, item) with equipallguns (BUGFIX_R0, the
  US cartridge) evaluated over the cartridge's own weapon-stats flags.
- **Timing**: no lockstep - the cartridge runs at its own frame rate, so every
  timing is in its 60ths and each gun's mean ticks per frame is recorded.
"""
import bisect, json, os, struct, sys, traceback

sys.path.insert(0, os.environ['GF_COMMON'])
sys.path.insert(0, os.environ['GF_GUNSDIR'])
import gdbge as lib  # noqa: E402  (common/ares/gdbge.py: aresge under the API's name)
import gunlist  # noqa: E402

GL = json.load(open(os.path.join(os.environ['GF_GUNSDIR'], 'ares_gun_layout.json')))
SYM, EN, TY = GL['symbols'], GL['enums'], GL['types']
FUNCS = sorted(GL['functions'])
FSTART = [a for a, n in FUNCS]
FN = {n: a for a, n in FUNCS}

ZBIT = 0x2000
DRAW = 150
TAP = (150, 156)
HOLD = int(os.environ.get('GF_HOLD', '480'))
HOLD_AT = 260
END = HOLD_AT + HOLD + 330
VERTA = -30.0
START = int(os.environ.get('GF_START', '650'))
STACK_WORDS = 24
CAMERAMODE_FP = 4      # enum CAMERAMODE (bondconstants.h): NONE, INTRO, FADESWIRL, SWIRL, FP


def frames(n):
    """n game frames, failing fast if the cartridge stops counting them (a frozen
    game would otherwise run aresge's 200000-frame cap)."""
    lib.twin()('until-word 0x%08x >= %d %d' % (SYM['currentFrameCounter'], rd(SYM['currentFrameCounter'], 4) + n,
                                               20 * n + 200))


def until_timer(v, budget):
    lib.twin()('until-word 0x%08x >= %d %d' % (SYM['g_GlobalTimer'], v, budget))


def fo(t, f):
    return TY[t]['fields'][f]['off']


def rd(addr, size, signed=True):
    b = lib.peek(addr, size)
    return int.from_bytes(b, 'big', signed=signed)


def wr(addr, size, v):
    lib.poke(addr, (v & ((1 << (8 * size)) - 1)).to_bytes(size, 'big'))


def funcname(a):
    i = bisect.bisect_right(FSTART, a) - 1
    if i < 0 or i + 1 >= len(FUNCS) or a == FSTART[i]:
        return None
    if a >= FSTART[i + 1] or a & 3:
        return None
    return FUNCS[i][1]


def in_func(a, name):
    s = FN[name]
    i = FSTART.index(s)
    return s < a < FSTART[i + 1]


def cmd_lines(cmd):
    """An n64twin command whose answer is lines before the ok (trace-dump)."""
    tw = lib.twin()
    tw.p.stdin.write(cmd + '\n')
    tw.p.stdin.flush()
    lines = []
    while True:
        line = tw.p.stdout.readline()
        if not line:
            raise RuntimeError('n64twin exited')
        line = line.rstrip('\n')
        if line.startswith('ok'):
            return lines
        if line.startswith('err'):
            raise RuntimeError('n64twin: ' + line)
        if line.startswith('T ') or line.startswith('W '):
            lines.append(line)


P = {}   # the player's addresses, read once in the level


def player_addrs():
    p = rd(SYM['g_CurrentPlayer'], 4, False)
    P.update({'p': p, 'hand0': p + fo('struct player', 'hands'),
              'hand1': p + fo('struct player', 'hands') + TY['struct hand']['size'],
              'ammo': p + fo('struct player', 'ammoheldarr')})


def hand(k, field):
    return P['hand%d' % k] + fo('struct hand', field)


def stats(item):
    rec = SYM['gitem_structs'] + item * TY['struct GunModelFileRecord']['size']
    if rd(rec + fo('struct GunModelFileRecord', 'has_no_model'), 4):
        return SYM['default_weaponstats']
    return rd(rec + fo('struct GunModelFileRecord', 'item_weapon_stats'), 4, False)


def ammotype(item):
    return rd(stats(item) + fo('struct WeaponStats', 'AmmoType'), 4)


def magsize(item):
    return rd(stats(item) + fo('struct WeaponStats', 'MagSize'), 2)


def bitflags(item):
    return rd(stats(item) + fo('struct WeaponStats', 'BitFlags'), 4, False)


def maxammo(t):
    return rd(SYM['ammo_related'] + t * TY['struct AmmoStats']['size'] + fo('struct AmmoStats', 'MaxAmmo'), 4, False)


# ------------------------------------------------- GoldenEye's give, as writes

def inv_add(item):
    """bondinvAddInvItem(item): a free slot (type -1) linked in at the head and
    sorted along (bondinvSortInv: by weapon, duals after, props last)."""
    pl = P['p']
    first_a = pl + fo('struct player', 'ptr_inventory_first_in_cycle')
    base = rd(pl + fo('struct player', 'p_itemcur'), 4, False)
    n = rd(pl + fo('struct player', 'equipmaxitems'), 4)
    sz = TY['struct InvItem']['size']
    F = lambda f: fo('struct InvItem', f)
    first = rd(first_a, 4, False)
    # already there?
    seen, cur = [], first
    while cur and cur not in seen:
        seen.append(cur)
        if rd(cur + F('type'), 4) == EN['INV_ITEM_WEAPON'] and rd(cur + F('type_inv_item.type_weap.weapon'), 4) == item:
            return False
        cur = rd(cur + F('next'), 4, False)
        if cur == first:
            break
    slot = next((base + i * sz for i in range(n) if rd(base + i * sz + F('type'), 4) == -1), None)
    if slot is None:
        raise RuntimeError('no free inventory slot')
    wr(slot + F('type'), 4, EN['INV_ITEM_WEAPON'])
    wr(slot + F('type_inv_item.type_weap.weapon'), 4, item)

    def key(it):
        t = rd(it + F('type'), 4)
        if t == EN['INV_ITEM_WEAPON']:
            return (rd(it + F('type_inv_item.type_weap.weapon'), 4), -1)
        if t == 3:   # INV_ITEM_DUAL
            return (rd(it + F('type_inv_item.type_dual.weapon_right'), 4), rd(it + F('type_inv_item.type_dual.weapon_left'), 4))
        if t == 2:   # INV_ITEM_PROP: 1000 as a candidate, 2000 as the subject - after every weapon
            return (1000, -1)
        return (-1, -1)
    items = seen + [slot]
    # bondinvSortInv leaves the list in key order with the smallest at the head
    items.sort(key=key)
    for k, it in enumerate(items):
        wr(it + F('next'), 4, items[(k + 1) % len(items)])
        wr(it + F('prev'), 4, items[k - 1])
    wr(first_a, 4, items[0])
    return True


def give_ammo(t, amount):
    """give_cur_player_ammo(t, amount) (gunfire.c:5678)."""
    cur = rd(hand(0, 'weaponnum'), 4)
    if ammotype(cur) == t and bitflags(cur) & EN['WEAPONSTATBITFLAG_AMMO_CLIP_LIMIT']:
        m = rd(hand(0, 'weapon_ammo_in_magazine'), 4) + amount
        wr(hand(0, 'weapon_ammo_in_magazine'), 4, min(m, magsize(cur)))
        wr(P['ammo'] + 4 * t, 4, 0)
        return
    wr(P['ammo'] + 4 * t, 4, min(amount, maxammo(t)))


def request(handnum, item, cycle=1):
    """gunRequestHandWeaponChange(hand, item, cycle) (gun.c:997), US branch."""
    st = rd(hand(handnum, 'weapon_action_state'), 4)
    lower, swap = EN['GUN_ANIM_STATE_SWITCH_LOWER'], EN['GUN_ANIM_STATE_SWITCH_SWAP']
    if st in (lower, swap):
        wr(hand(handnum, 'field_8B0'), 4, rd(hand(handnum, 'field_890'), 4) + 0x11)
    # get_next_weapon_in_cycle_for_hand(hand, 0)
    nxt = rd(hand(handnum, 'weapon_next_weapon'), 4) if st in (lower, swap) else rd(hand(handnum, 'weaponnum'), 4)
    if nxt != item:
        if st not in (lower, swap):
            wr(hand(handnum, 'weapon_current_animation'), 4, 5)
        wr(hand(handnum, 'weapon_next_weapon'), 4, item)
        wr(hand(handnum, 'weapon_animation_trigger'), 4, 1)
        wr(hand(handnum, 'field_8B8'), 4, cycle)


AMMO = {'type': -1}


def give(item):
    out = {}
    if rd(hand(0, 'weaponnum'), 4) == item:
        wr(hand(0, 'weapon_ammo_in_magazine'), 4, magsize(item))
    inv_add(item)
    t = ammotype(item)
    AMMO['type'] = t
    if t > 0:
        mx = maxammo(t)
        give_ammo(t, mx)
        out['reserve_max'] = mx
    # bondinvItemAvailableForHand(item, item) with equipallguns, one player, BUGFIX_R0
    out['dual_allguns'] = int(item < EN['ITEM_BOMBCASE'] and bool(bitflags(item) & EN['WEAPONSTATBITFLAG_CAN_DUAL_WIELD']))
    request(0, item, 1)
    lib.poke(P['p'] + fo('struct player', 'vv_verta'), struct.pack('>f', VERTA))
    out['ammotype'] = t
    return out


def remove_chrs():
    n = rd(SYM['g_NumChrSlots'], 4)
    base = rd(SYM['g_ChrSlots'], 4, False)
    size = TY['ChrRecord']['size']
    gone = 0
    for k in range(n):
        c = base + k * size
        prop = rd(c + fo('ChrRecord', 'prop'), 4, False)
        if rd(c + fo('ChrRecord', 'chrnum'), 2) < 0 or not prop:
            continue
        # a free slot can keep a stale prop (Dam: 46 slots for 36 guards), so only a
        # chr its prop points back at; and only a guard's prop (type 3): Dam's chr
        # 5036 is a type-6 prop (Bond's own body in the opening) and marking it for
        # removal freezes the cartridge's frame counter for good (the native port
        # took it)
        if rd(prop + fo('PropRecord', 'chr'), 4, False) != c or rd(prop + fo('PropRecord', 'type'), 1, False) != 3:
            continue
        h = rd(c + fo('ChrRecord', 'hidden'), 2, False)
        wr(c + fo('ChrRecord', 'hidden'), 2, h | 0x20)     # CHRHIDDEN_REMOVE
        gone += 1
    return gone


def install_hooks():
    tw = lib.twin()
    tw('trace-clock 0x%08x' % SYM['g_GlobalTimer'])
    tw('trace 0x%08x snd r5 stack %d' % (FN['sndPlaySfx'], STACK_WORDS))
    tw('trace 0x%08x casing' % FN['casingCreate'])
    tw('trace 0x%08x expl r7' % FN['explosionCreate'])
    tw('trace 0x%08x impact' % FN['explosionCreateBulletImpact'])
    tw('trace 0x%08x shot' % FN['joyRumblePakStart'])
    for f in ('generate_player_thrown_grenade', 'generate_player_thrown_knife', 'generate_player_thrown_object'):
        tw('trace 0x%08x throw' % FN[f])
    tw('trace 0x%08x launch' % FN['gunSpawnGLGrenade'])
    tw('trace 0x%08x launch' % FN['gunFireTankShell'])
    tw('watch 0x%08x frame 0x%08x 48 0x%08x 48 0x%08x 120' % (FN['lvlRender'], P['hand0'], P['hand1'], P['ammo']))


def trigger_at(t):
    return 1 if (TAP[0] <= t < TAP[1] or HOLD_AT <= t < HOLD_AT + HOLD) else 0


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def collect(g):
    events, frames = [], []
    for line in cmd_lines('trace-dump'):
        p = line.split()
        if p[0] == 'T':
            kind, clock = p[1], int(p[2])
            ra = int(p[5], 16)
            regs = {x.split('=')[0]: int(x.split('=')[1], 16) for x in p[6:] if x.startswith('r') and '=' in x}
            stack = []
            for x in p[6:]:
                if x.startswith('s='):
                    stack = [int(w, 16) for w in x[2:].split(',')]
            t = clock - g
            if kind == 'snd':
                callers = []
                for a in [ra] + stack:
                    n = funcname(a)
                    if n and n not in callers and n != 'sndPlaySfx':
                        callers.append(n)
                events.append([t, 'snd', s16(regs.get('r5', 0)), callers[:4]])
            elif kind == 'expl':
                events.append([t, 'expl', s16(regs.get('r7', 0))])
            elif kind == 'shot':
                if in_func(ra, 'gunTickHandState'):
                    events.append([t, 'shot', 1])
            else:
                events.append([t, kind, 1])
        else:
            clock = int(p[2])
            blobs = [bytes.fromhex(x) for x in p[4].split(':')]
            h0, h1, am = blobs
            hf = lambda b, f: int.from_bytes(b[fo('struct hand', f):fo('struct hand', f) + 4], 'big', signed=True)
            res = int.from_bytes(am[4 * AMMO['type']:4 * AMMO['type'] + 4], 'big', signed=True) if AMMO['type'] >= 0 else -1
            t = clock - g
            frames.append([t, hf(h0, 'weaponnum'), hf(h0, 'weapon_ammo_in_magazine'), hf(h0, 'weapon_action_state'),
                           hf(h1, 'weaponnum'), hf(h1, 'weapon_ammo_in_magazine'), hf(h1, 'weapon_action_state'),
                           res, trigger_at(t)])
    events.sort(key=lambda e: e[0])
    return events, frames


try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    f0 = rd(SYM['currentFrameCounter'], 4)
    lib.until_tick(START)
    # the boot's one Z (the opening still) is 190 frames after the level's first
    while rd(SYM['currentFrameCounter'], 4) < f0 + 230:
        frames(10)
    tw = lib.twin()
    tw('pad 0 -')
    # On the cartridge the opening runs later against the level clock than on the
    # native port, and when it ends GoldenEye equips Bond's starting gun - over a
    # gun given before it (Dam: the silenced PP7 came back at tick ~850 and the
    # tap went unfired). So wait for play proper: the first-person camera
    # (CAMERAMODE_FP) and the starting gun drawn and idle.
    tw('until-word 0x%08x == %d 40000' % (GL['symbols'].get('g_CameraMode', 0x80036494), CAMERAMODE_FP))
    for _ in range(200):
        player_addrs()
        if rd(hand(0, 'weaponnum'), 4) != 0 and rd(hand(0, 'weapon_action_state'), 4) == 0:
            break
        frames(3)
    else:
        raise RuntimeError('Bond never stood idle with his gun drawn')
    frames(30)
    player_addrs()
    wr(P['p'] + fo('struct player', 'cheatBondInvincible'), 1, 1)
    lib.say('removed', remove_chrs(), 'chrs', 'at tick', lib.tick(), 'oracle ares')
    frames(4)
    install_hooks()
    cmd_lines('trace-dump')     # what the frames above logged
    for item in [int(x) for x in os.environ['GF_GUNS'].split(',')]:
        gdef = [x for x in gunlist.GUNS if x[0] == item][0]
        tw('pad-when-clear')
        tw('pad 0 -')
        cmd_lines('trace-dump')
        g = rd(SYM['g_GlobalTimer'], 4)
        info = give(item)
        # the trigger on the game's own clock (the port's poller pressed for the
        # next tick: trigger_at(t + 1), so one tick early here too)
        for at, btn in ((TAP[0] - 1, 'Z'), (TAP[1] - 1, '-'), (HOLD_AT - 1, 'Z'), (HOLD_AT + HOLD - 1, '-')):
            tw('pad-when 0x%08x >= %d 0 %s' % (SYM['g_GlobalTimer'], g + at, btn))
        # GoldenEye runs 2-4 ticks a frame and ares has several video frames to one
        # of the game's; a budget of 3 video frames a tick is ample and fails fast
        until_timer(g + END, 3 * END + 600)
        events, fr = collect(g)
        ts = [f[0] for f in fr]
        steps = [b - a for a, b in zip(ts, ts[1:]) if b > a]
        info.update({'side': 'ge', 'oracle': 'ares', 'item': item, 'gun': gdef[1], 'weapon': gdef[2], 'hold': HOLD,
                     'schedule': {'draw': DRAW, 'tap': TAP, 'hold_at': HOLD_AT, 'end': END},
                     'ticks_per_frame': round(sum(steps) / len(steps), 3) if steps else None,
                     'events': events, 'frames': fr})
        lib.emit(os.path.join(os.environ['GF_OUT'], 'gun_%d.json' % item), info)
        tw('pad-when-clear')
        tw('pad 0 -')
        frames(30)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

"""One side of the gun diff: GoldenEye's guns drawn, tapped, held and emptied
in a mission with every chr removed, every event the gun causes logged with
the level tick. Run by guns/run.py (GF_SIDE ge or pd, GF_GUNS a comma list
of GoldenEye item numbers, GF_OUT the directory for guns_<item>.json).

The timeline, in level ticks after the gun is asked for (the same on both
sides; see SCHEDULE): drawn and left alone, the trigger tapped once, left,
held for HOLD ticks (through an empty clip and its reload), released and
watched while anything thrown goes off.

Input is the controller itself, so the game's own input path decides what a
press does: GoldenEye's pad script state (the script's cues are all spent
before the first gun), our joy sample just after osContGetReadData() filled
it. Events are breakpoints that record and carry on (stop() returns False):
sounds as requested (GoldenEye's sndPlaySfx, our sndStart, ours mapped back to
GoldenEye's id through gesfx.c's g_SfxGeId), casings, explosions and bullet
impacts. Each frame the poller reads both hands and the reserve.
"""
import os, sys, json, struct, traceback
import gdb

sys.path.insert(0, os.environ['GF_COMMON'])
sys.path.insert(0, os.environ['GF_GUNSDIR'])
SIDE = os.environ['GF_SIDE']
lib = __import__('gdbge' if SIDE == 'ge' else 'gdbpd')
import gunlist  # noqa: E402

ZBIT = 0x2000          # Z, both games' fire
DRAW = 150             # ticks to draw
TAP = (150, 156)       # one press
HOLD = int(os.environ.get('GF_HOLD', '480'))
HOLD_AT = 260
END = HOLD_AT + HOLD + 330
VERTA = -30.0          # look at the floor ahead, so shots leave impacts
# both sides start at the same level tick: GoldenEye's pad script is not done
# with the intro until ~600, and a gun asked for while Bond is still coming out
# of the intro takes longer to swap (our draw measured 60 at tick 120, 45 later)
START = int(os.environ.get('GF_START', '650'))

ev = lib.ev
inf = gdb.selected_inferior()


def raw_tick():
    return int(ev('g_GlobalTimer' if SIDE == 'ge' else 'g_Vars.lvframe60'))


REC = {'on': False, 'g': 0, 'events': [], 'frames': [], 'trigger': 0}


def trigger_at(t):
    return 1 if (TAP[0] <= t < TAP[1] or HOLD_AT <= t < HOLD_AT + HOLD) else 0


class Hook(gdb.Breakpoint):
    def __init__(self, spec, kind, reader, caller=None):
        super().__init__(spec, internal=True)
        self.kind, self.reader, self.caller = kind, reader, caller

    def stop(self):
        if REC['on']:
            if self.caller:
                try:
                    if gdb.newest_frame().older().name() != self.caller:
                        return False
                except (gdb.error, AttributeError):
                    return False
            try:
                v = self.reader()
            except gdb.error:
                v = None
            ev_ = [raw_tick() - REC['g'], self.kind, v]
            if self.kind == 'snd':
                ev_.append(callers(4))
            REC['events'].append(ev_)
        return False


def callers(n):
    """The functions that asked for a sound, innermost first."""
    out = []
    try:
        f = gdb.newest_frame().older()
        while f is not None and len(out) < n:
            out.append(f.name() or '?')
            f = f.older()
    except gdb.error:
        pass
    return out


def _pd_sound():
    packed = int(ev('sound')) & 0xffff
    if packed & 0x8000:     # hasconfig: the id is the config's
        packed = int(ev('g_AudioRussMappings[%d].soundnum' % (packed & 0x7fff))) & 0xffff
    sid = packed & 0x7ff
    if sid == 0:
        return 0
    # on a converted level n_sndplayer.c plays GoldenEye's sample for 1..261
    # (geSfxRemap(): all but its own few, and 62 is silent)
    if sid <= 261 and sid not in (2, 7, 9, 16, 43, 55, 100, 101, 245):
        return 0 if sid == 62 else sid
    try:
        ge = int(ev("'gesfx.c'::g_SfxGeId[%d]" % sid))
    except gdb.error:
        ge = 0
    return ge if ge > 0 else 'pd%d' % sid


def install_hooks():
    if SIDE == 'ge':
        Hook('sndPlaySfx', 'snd', lambda: int(ev('soundIndex')))
        Hook('casingCreate', 'casing', lambda: 1)
        Hook('explosionCreate', 'expl', lambda: int(ev('explosion_type')))
        Hook('explosionCreateBulletImpact', 'impact', lambda: 1)
        # every shot rumbles from the firing branch (gunfire.c, "weapon_firing_status
        # != 0"), the laser's and the camera's aside; nothing else rumbles from there
        Hook('joyRumblePakStart', 'shot', lambda: 1, caller='gunTickHandState')
        for f in ('generate_player_thrown_grenade', 'generate_player_thrown_knife', 'generate_player_thrown_object'):
            Hook(f, 'throw', lambda: 1)
        Hook('gunSpawnGLGrenade', 'launch', lambda: 1)
        Hook('gunFireTankShell', 'launch', lambda: 1)    # the decomp's name; it fires the rocket
    else:
        Hook('sndStart', 'snd', _pd_sound)
        Hook('casingCreate', 'casing', lambda: 1)
        Hook('explosionCreate', 'expl', lambda: int(ev('type')))
        Hook('wallhitCreateWith20Args', 'impact', lambda: 1)
        Hook('bgun0f09a6f8', 'shot', lambda: int(ev('hand->shotstotake')))
        Hook('bgunCreateThrownProjectile', 'throw', lambda: 1)
        Hook('bgunCreateFiredProjectile', 'launch', lambda: 1)


class PdInput(gdb.Breakpoint):
    """Our controller: OR the trigger into the sample osContGetReadData() just wrote."""
    def __init__(self):
        super().__init__('joy.c:613', internal=True)

    def stop(self):
        if REC['trigger']:
            a = int(ev('(unsigned long)&g_JoyData[0].samples[g_JoyData[0].nextlast].pads[0].button'))
            b = struct.unpack('<I', bytes(inf.read_memory(a, 4)))[0]
            inf.write_memory(a, struct.pack('<I', b | ZBIT))
        return False


def set_trigger(on):
    REC['trigger'] = on
    if SIDE == 'ge':
        inf.write_memory(GE_PADBTN, struct.pack('<H', ZBIT if on else 0))


def hands_state():
    if SIDE == 'ge':
        P = 'g_CurrentPlayer->hands[%d].'
        h = [[int(ev(P % k + 'weaponnum')), int(ev(P % k + 'weapon_ammo_in_magazine')),
              int(ev(P % k + 'weapon_action_state'))] for k in (0, 1)]
        res = int(ev('g_CurrentPlayer->ammoheldarr[%d]' % AMMO['type'])) if AMMO['type'] >= 0 else -1
    else:
        P = 'g_Vars.currentplayer->hands[%d].'
        h = [[int(ev(P % k + 'gset.weaponnum')), int(ev(P % k + 'loadedammo[0]')),
              int(ev(P % k + 'state'))] for k in (0, 1)]
        res = int(ev('g_Vars.currentplayer->ammoheldarr[%d]' % AMMO['type'])) if AMMO['type'] >= 0 else -1
    return h, res


class Poller(gdb.Breakpoint):
    def __init__(self):
        super().__init__('lvlRender' if SIDE == 'ge' else 'videoEndFrame', internal=True)
        self.enabled = False

    def stop(self):
        t = raw_tick() - REC['g']
        h, res = hands_state()
        REC['frames'].append([t, h[0][0], h[0][1], h[0][2], h[1][0], h[1][1], h[1][2], res, REC['trigger']])
        want = trigger_at(t + 1)
        if want != REC['trigger']:
            set_trigger(want)
        return t >= END


AMMO = {'type': -1}


def remove_chrs():
    n = int(ev('g_NumChrSlots'))
    gone = 0
    for k in range(n):
        c = 'g_ChrSlots[%d]' % k
        if int(ev(c + '.chrnum')) < 0 or int(ev('(unsigned long)' + c + '.prop')) == 0:
            continue
        if SIDE == 'pd' and int(ev('(unsigned long)' + c + '.prop')) == int(ev('(unsigned long)g_Vars.currentplayer->prop')):
            continue
        gdb.execute('set variable %s.hidden = %s.hidden | 0x20' % (c, c))
        gone += 1
    return gone


def give(item, weapon):
    """Give the gun, a full reserve, and ask for it. Returns what the side reports."""
    out = {}
    if SIDE == 'ge':
        # Bond starts holding a gun, and the pad script's in-level Z presses
        # (which skip the intro) fire it; give it its magazine back
        if int(ev('g_CurrentPlayer->hands[0].weaponnum')) == item:
            gdb.execute('set variable g_CurrentPlayer->hands[0].weapon_ammo_in_magazine = '
                        'get_ptr_item_statistics(%d)->MagSize' % item)
        lib.call('(int)bondinvAddInvItem(%d)' % item)
        t = int(ev('(int)get_ammo_type_for_weapon(%d)' % item))
        AMMO['type'] = t
        if t > 0:
            mx = int(ev('(int)get_max_ammo_for_type(%d)' % t))
            lib.call('(void)give_cur_player_ammo(%d, %d)' % (t, mx))
            out['reserve_max'] = mx
        gdb.execute('set variable g_CurrentPlayer->equipallguns = 1')
        out['dual_allguns'] = int(ev('(int)bondinvItemAvailableForHand(%d, %d)' % (item, item)))
        gdb.execute('set variable g_CurrentPlayer->equipallguns = 0')
        lib.call('(void)gunRequestHandWeaponChange(0, %d, 1)' % item)
        gdb.execute('set variable g_CurrentPlayer->vv_verta = %f' % VERTA)
    else:
        lib.call('(int)invGiveSingleWeapon(%d)' % weapon)
        a = ev('weaponGetAmmoByFunction(%d, 0)' % weapon)
        t = int(a['type']) if int(a) else -1
        AMMO['type'] = t
        if t > 0:
            mx = int(ev('(int)bgunGetCapacityByAmmotype(%d)' % t))
            lib.call('(void)bgunSetAmmoQuantity(%d, %d)' % (t, mx))
            out['reserve_max'] = mx
        gdb.execute('set variable g_Vars.currentplayer->equipallguns = 1')
        out['dual_allguns'] = int(ev('(int)invHasDoubleWeaponIncAllGuns(%d, %d)' % (weapon, weapon)))
        gdb.execute('set variable g_Vars.currentplayer->equipallguns = 0')
        lib.call('(void)bgunEquipWeapon(%d)' % weapon)
        gdb.execute('set variable g_Vars.currentplayer->vv_verta = %f' % VERTA)
    out['ammotype'] = AMMO['type']
    return out


GE_PADBTN = 0
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    if SIDE == 'ge':
        # let the pad script's last cues (the intro's skip) go by, then it is ours
        while int(ev('currentFrameCounter')) < 1700:
            lib.frames(20)
        gdb.execute("set variable 'os_cont.c'::g_padCueNext = 'os_cont.c'::g_padCueCount")
        lib.until_tick(START)
        GE_PADBTN = int(ev("(unsigned long)&'os_cont.c'::g_padScriptState[0].buttons"))
        gdb.execute('set variable g_CurrentPlayer->cheatBondInvincible = 1')
    else:
        lib.until_tick(START)
        gdb.execute('set variable g_Vars.currentplayer->invincible = 1')
        # a mission GE Plus's folder started has this; --boot-ge-mission does
        # not, and the All Guns list (inv.c's invAllGunsAreGe()) keys off it
        gdb.execute("set variable 'gexfront.c'::g_FrontInside = 1")
        PdInput()
    set_trigger(0)
    lib.say('removed', remove_chrs(), 'chrs', 'at tick', lib.tick())
    lib.frames(4)
    install_hooks()
    poll = Poller()
    for item in [int(x) for x in os.environ['GF_GUNS'].split(',')]:
        g = [x for x in gunlist.GUNS if x[0] == item][0]
        REC.update({'events': [], 'frames': [], 'trigger': 0})
        set_trigger(0)
        REC['g'] = raw_tick()
        info = give(item, g[2])
        REC['on'] = True
        poll.enabled = True
        gdb.execute('continue')
        poll.enabled = False
        REC['on'] = False
        set_trigger(0)
        info.update({'side': SIDE, 'item': item, 'gun': g[1], 'weapon': g[2], 'hold': HOLD,
                     'schedule': {'draw': DRAW, 'tap': TAP, 'hold_at': HOLD_AT, 'end': END},
                     'events': REC['events'], 'frames': REC['frames']})
        lib.emit(os.path.join(os.environ['GF_OUT'], 'gun_%d.json' % item), info)
        lib.frames(30)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

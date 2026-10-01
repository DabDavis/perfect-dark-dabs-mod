"""The GoldenEye XBLA ("Bean") side of the twin driver: Bean in the self-built
xenia-canary, read and written through the twin channel (tools/gefidelity/xenia,
xenia-twin.patch), driven to a mission with a virtual pad.

Not a gdb scenario: xenia/run_scenario.py runs a world/ scenario with this module
standing in for gdbge, so `world/dump.py` runs against Bean unchanged.

What is sound here and what is not:
- pads(), props() read GoldenEye's own setup in Bean's RAM. Bean keeps the N64
  setup's records (same types, sizes and world coordinates), so these use the
  N64 layouts.
- player() reads Bond from players[0] (0x82F1FA98). Bean's player struct is
  4J's, not the N64 one; its offsets were found on Dam by matching the N64
  oracle's numbers (README).
- chrs(): Bean's chr struct is likewise its own - only the fields located are read.
- tick() is Bean's frame counter (0x8308851C, 60 a second from boot), relative
  to the level's first frame - Xenia runs Bean at 60, so it is 60ths of a second.
"""
import os, json, struct, subprocess, time

SIDE = 'xenia'
BEAN = os.environ.get('GF_BEAN', 'ce')   # which build rig.sh runs: ce (default) or retail
HERE = os.path.dirname(os.path.abspath(__file__))
RIG = os.path.join(HERE, '..', 'xenia', 'rig.sh')
STATE = os.environ.get('GF_XENIA_STATE', os.path.expanduser('~/wt/gefidelity-run/xenia-out/rig'))

FRAME_COUNTER = 0x8308851C
MENU_XY = 0x8272B37C        # the folder screens' crosshair, x then y (f32); the recomp's GE_MENU_XY
MENU_SEL = 0x82F60AF4       # the folder's selected row/cell under it (found on the mission grid)
# GoldenEye's pad 0 per mission (the N64 oracle's), to tell which level loaded
PAD0 = {0: (16294.3, -77.042, 12827.428), 1: (348.12, -389.563, 828.029), 2: (7535.885, -424.242, -5146.73),
        3: (-10599.459, 110.021, -19060.104), 4: (1092.127, 64.897, 730.557), 5: (-105.807, -1860.081, 3861.943),
        6: (-274.815, 831.147, -346.311), 7: (-50.61, -345.467, -1852.76), 8: (498.781, 207.671, -865.914),
        9: (-5867.38, 289.171, 839.53), 10: (-2778.294, 319.662, -2839.464), 11: (693.226, 20.475, -1070.551),
        12: (-1057.311, 132.736, -6046.352), 13: (-772.318, 193.08, -119.842), 14: (3982.555, 528.19, -5197.393),
        15: (966.192, 386.878, -853.937), 16: (5524.844, -2706.502, -1409.171), 17: (-1272.727, 1357.576, 2197.576),
        18: (6569.299, 36.827, 0.0), 19: (-2846.767, 93.721, -1749.453)}
OPENING_PRESS_TICK = 190
PLAYERS = 0x82F1FA98
_st = {'chan': None, 't0': 0}


def say(*a):
    print('GF', *a, flush=True)


class Channel:
    """Never blocks on a rig that is not there: cmd is opened non-blocking (it
    fails at once when no Xenia holds it), answers are read with a timeout."""
    def __init__(self, state=None, timeout=120.0):
        state = state or STATE   # the module's at call time: run_scenario.py points it at its own rig
        try:
            self.cmd = os.open(os.path.join(state, 'cmd'), os.O_WRONLY | os.O_NONBLOCK)
        except OSError as e:
            raise IOError('no Xenia on the twin channel in %s (rig.sh start): %s' % (state, e))
        self.ans = os.open(os.path.join(state, 'ans'), os.O_RDONLY | os.O_NONBLOCK)
        self.buf = b''
        self.timeout = timeout

    def q(self, line):
        import select
        data = (line + '\n').encode()
        while data:
            select.select([], [self.cmd], [], self.timeout)
            n = os.write(self.cmd, data)
            data = data[n:]
        deadline = time.time() + self.timeout
        while b'\n' not in self.buf:
            left = deadline - time.time()
            if left <= 0:
                raise IOError('%s: no answer in %.0f s (is Xenia still running?)' % (line, self.timeout))
            r, _, _ = select.select([self.ans], [], [], left)
            if r:
                try:
                    chunk = os.read(self.ans, 1 << 20)
                except BlockingIOError:
                    chunk = b''
                self.buf += chunk
        i = self.buf.index(b'\n')
        r, self.buf = self.buf[:i].decode().strip(), self.buf[i + 1:]
        if r.startswith('err'):
            raise IOError('%s -> %s' % (line, r))
        return r


def chan():
    if _st['chan'] is None:
        _st['chan'] = Channel()
    return _st['chan']


def peek(addr, n):
    return bytes.fromhex(chan().q('peek %x %d' % (addr, n))[3:])


def poke(addr, data):
    chan().q('poke %x %s' % (addr, data.hex()))


def u32(addr):
    return struct.unpack('>I', peek(addr, 4))[0]


def f32(addr):
    return struct.unpack('>f', peek(addr, 4))[0]


def find(addr, length, data):
    r = chan().q('find %x %x %s' % (addr, length, data.hex()))
    return [int(x, 16) for x in r.split()[1:]]


def dump(addr, length, path):
    chan().q('dump %x %x %s' % (addr, length, path))
    return open(path, 'rb').read()


def frame():
    return int(chan().q('frame').split()[1])


def counter():
    return u32(FRAME_COUNTER)


def use_state(state):
    """Talk to the rig whose state directory is `state` (rig.sh's GF_XENIA_STATE)."""
    global STATE
    STATE = state
    _st['chan'] = None


def pad(cmdline, wait=0.0):
    with open(os.path.join(STATE, 'pad.fifo'), 'w') as f:
        f.write(cmdline + '\n')
    if wait:
        time.sleep(wait)


# ---------------------------------------------------------------- the route

def boot(levelid=None, difficulty=0, mission=None):
    """From a fresh rig.sh start: the pad's walk through Bean's folder screens
    into solo mission `mission` (GoldenEye's number; GF_MISSION when None) at
    `difficulty`, the opening skipped."""
    if mission is None:
        mission = int(os.environ.get('GF_MISSION', '0'))
    # the channel answers from the first swap; wait for the title
    deadline = time.time() + 120
    while True:
        try:
            if counter() > 1200:
                break
        except IOError:
            pass
        if time.time() > deadline:
            raise RuntimeError('Bean never reached its title')
        time.sleep(1)
    # each folder page opens with the crosshair at its own spot; a press is
    # repeated until the next page's spot is there (a press during a page turn
    # or the gun barrel is lost)
    _to_page(PAGE_MAIN, 'press A', first='press START')   # title -> main folder
    _to_page(PAGE_GRID, 'press A')                          # 1. SELECT MISSION -> the grid
    # The folder's cursor is a crosshair that the stick glides (a 0.12 s pulse
    # is about half a cell), so pulses land between cells and a mission is
    # missed (Surface came out Facility). The crosshair is written instead -
    # GE_MENU_XY, as the recomp's mouse hook does - and the folder's own
    # selection (MENU_SEL) checked before accepting.
    _menu_pick(mission, 73.0 + 70.0 * (mission % 5), 62.0 + 70.0 * (mission // 5), 'mission')
    _to_page(PAGE_DIFF, 'press A')                          # the difficulty page (on the hardest open)
    _menu_pick(difficulty, 106.0, 200.0 + 30.0 * difficulty, 'difficulty')
    pad('press A', 3)            # the briefing
    # the level's first frame is the clock's 1, as GoldenEye's and ours are:
    # caught by the setup's pads pointer changing when the mission loads
    # (zeroed first: an attract demo may have loaded the same level at the same
    # address, and the front end does not read it)
    poke(SETUP + 24, b'\0\0\0\0')
    before = 0
    pad('press START')           # the mission loads and opens on its still
    deadline = time.time() + 60
    while True:
        try:
            now = u32(SETUP + 24)
            if now and now != before and u32(PLAYERS):
                break
        except IOError:
            pass
        if time.time() > deadline:
            raise RuntimeError('mission %d never loaded' % mission)
        time.sleep(0.03)
    _st['t0'] = counter() - 1
    look_ahead(False)
    # the oracle's quiet pad script dismisses the opening still with one press
    # about 190 frames into the level; the same here
    while tick() < OPENING_PRESS_TICK:
        time.sleep(0.05)
    pad('press A')
    # the level that loaded is the one asked for: its pad 0 is GoldenEye's
    # (read now, not at the load: the pads are scaled to world units after the
    # pointer is set)
    want = PAD0.get(mission)
    if want:
        got = struct.unpack('>3f', peek(u32(SETUP + 24), 12))
        if max(abs(a - b) for a, b in zip(got, want)) > 1.0:
            raise RuntimeError('mission %d asked, but the level that loaded has pad 0 at %s (wanted %s)' % (
                mission, [round(v, 1) for v in got], want))
    say('boot mission', mission, 'difficulty', difficulty, 'tick', tick())


PAGE_MAIN, PAGE_GRID, PAGE_DIFF = (176.0, 131.0), (73.0, 62.0), (106.0, 276.0)   # where each page opens the crosshair


def _menu_xy():
    return struct.unpack('>2f', peek(MENU_XY, 8))


def _to_page(sig, press, first=None, tries=10):
    """Press until the folder's crosshair sits where page `sig` opens it."""
    for i in range(tries):
        pad(first if (i == 0 and first) else press)
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if max(abs(a - b) for a, b in zip(_menu_xy(), sig)) < 0.5:
                time.sleep(0.5)          # let the page finish turning
                return
            time.sleep(0.1)
    raise RuntimeError('the folder never reached the page whose crosshair opens at %s (it is at %s)' % (
        sig, tuple(round(v, 1) for v in _menu_xy())))


def _menu_pick(target, x, y, what, tries=8):
    """The folder's crosshair to (x, y) until its selection reads target."""
    for i in range(tries):
        poke(MENU_XY, struct.pack('>2f', x, y))
        time.sleep(0.25)
        if u32(MENU_SEL) == target:
            return
        time.sleep(0.5)          # the page may still be turning
    raise RuntimeError('could not select %s %d on the folder (selection reads %d)' % (what, target, u32(MENU_SEL)))


def tick():
    return counter() - _st['t0']


def pause():
    """The GPU thread waits at the next swap. Commands are then answered at once
    (free running, one is answered per swap), so read the world paused."""
    if not _st.get('paused'):
        chan().q('pause')
        _st['paused'] = True


def resume():
    if _st.get('paused'):
        chan().q('run')
        _st['paused'] = False


def until_tick(t):
    """Runs to the level's tick t (60ths) and leaves the game waiting there."""
    resume()
    while tick() < t:
        time.sleep(max(0.02, (t - tick()) / 60.0 * 0.8))
    pause()


def frames(n=1):
    """n swaps, then the GPU thread waits again."""
    r = chan().q('frames %d' % n)
    _st['paused'] = True
    return int(r.split()[1])


def finish():
    try:
        resume()
    except Exception:
        pass
    say('finish')


# ------------------------------------------------------------ where things are
# GoldenEye_Nov2007_Release. The fixed addresses are the recomp's
# (GoldenEye-XBLA-Recomp/src/ge_hooks.cpp); the rest was found on Dam by
# matching the N64 oracle's numbers (tools/gefidelity/xenia/README.md).

SETUP = 0x82F303A0          # GoldenEye's g_CurrentSetup: the N64's ten pointers in the N64's order
SETTINGS = 0x83088228       # -> settings; +0x298 bit flags (0x80 look ahead, 0x10 auto aim)
PL_PROP = 0x1AC             # Bean's struct player (4J's layout, not the N64's)
PL_THETA = 0x254
PL_VERTA = 0x264
PL_COLL = 0x5B0             # GoldenEye's collision434 (field_488), N64 layout inside
PR_OBJ, PR_POS, PR_STAN, PR_PARENT, PR_ROOMS = 0x08, 0x0C, 0x18, 0x20, 0x30   # Bean's PropRecord

# N64 record sizes in words; Bean's own sizes are learned per type as the walk
# meets them (a weapon is one word longer, a door one shorter, the truck 15 longer)
N64_WORDS = {1: 64, 2: 2, 3: 32, 4: 33, 5: 32, 6: 59, 7: 33, 8: 34, 9: 7, 10: 64, 11: 149, 12: 32, 13: 54,
             14: 3, 17: 32, 18: 3, 19: 4, 20: 45, 21: 34, 22: 4, 23: 4, 24: 1, 25: 2, 26: 2, 27: 2, 28: 2,
             29: 2, 30: 4, 31: 1, 32: 4, 33: 5, 34: 1, 35: 4, 36: 32, 37: 10, 38: 4, 39: 44, 40: 45, 42: 32,
             43: 32, 44: 5, 45: 56, 46: 7, 47: 37}
OBJ_TYPES = {1, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 17, 20, 21, 36, 39, 40, 42, 43, 45, 47}
PROPDEF_END = 0x30
DELTAS = sorted(range(-4, 25), key=abs)
# Bean's sizes already proven (Dam): a record type's size is fixed, so these are
# taken as given rather than relearned from data that may look like a header
BEAN_DELTAS = {8: 1, 1: -1, 39: 15}


def setup(slot):
    return u32(SETUP + 4 * slot)


def _f(v):
    return round(float(v), 3)


def walk(maxwords=40000):
    """Bean's setup records: (index, type, words, offset). A size is taken only
    when the next three records parse from it - a word inside a record can look
    like a header."""
    base = setup(3)
    blob = peek(base, 4 * maxwords) if False else _peek_big(base, 4 * maxwords)

    def hdr(off):
        w = struct.unpack_from('>I', blob, off)[0]
        return (w >> 16) & 0xffff, (w >> 8) & 0xff, w & 0xff

    def plausible(off):
        if off < 0 or off + 8 > len(blob):
            return False
        es, st, t = hdr(off)
        if t == PROPDEF_END:
            return True
        if t not in N64_WORDS or st >= 0x20:
            return False
        if t in OBJ_TYPES:
            m, pd = struct.unpack_from('>hh', blob, off + 4)
            # a pad of -1..-256 puts the object inside an earlier one (stacked crates)
            return 0 <= m < 0x200 and -256 <= pd < 20000
        return es == 0 or t == 9

    def chain(off, delta, depth):
        if depth == 0:
            return True
        if not plausible(off):
            return False
        t = hdr(off)[2]
        if t == PROPDEF_END:
            return True
        return any(chain(off + 4 * (N64_WORDS[t] + d), delta, depth - 1)
                   for d in ([delta[t]] if t in delta else DELTAS))

    p, out, delta = 0, [], dict(BEAN_DELTAS)
    while True:
        t = hdr(p)[2]
        if t == PROPDEF_END:
            break
        if t not in N64_WORDS:
            raise RuntimeError('unknown record type %d at +0x%x after %d records' % (t, p, len(out)))
        n = N64_WORDS[t]
        for d in ([delta[t]] if t in delta else DELTAS):
            if chain(p + 4 * (n + d), delta, 5):
                break
        else:
            raise RuntimeError('no next record after type %d at +0x%x (record %d)' % (t, p, len(out)))
        delta.setdefault(t, d)
        out.append((len(out), t, n + d, base + p, blob[p:p + 4 * (n + d)]))
        p += 4 * (n + d)
    _st['deltas'] = {t: d for t, d in delta.items() if d and any(r[1] == t for r in out)}
    return out


def _peek_big(addr, n):
    out = b''
    while n > 0:
        k = min(n, 1 << 19)
        out += peek(addr, k)
        addr += k
        n -= k
    return out


def _rooms(propaddr):
    out = []
    for b in peek(propaddr + PR_ROOMS, 4):
        if b == 0xff:
            break
        out.append(b)
    return out


def _tile_room(stan):
    """A StandTile's room: the low byte of its first word (name:24, room:8 - on
    the big-endian console that is byte 3; the port's -m32 layout has it first)."""
    return peek(stan, 4)[3] if stan else -1


def pads():
    out = []
    for slot, first, stride in ((6, 0, 0x2c), (7, 10000, 0x44)):
        base = setup(slot)
        if not base:
            continue
        i = 0
        while i < 4000:
            rec = peek(base + i * stride, 0x2c)
            plink, stan = struct.unpack_from('>II', rec, 0x24)
            if plink == 0:
                break
            x, y, z = struct.unpack_from('>3f', rec, 0)
            out.append([first + i, _f(x), _f(y), _f(z), _tile_room(stan)])
            i += 1
    return out


def props():
    out = []
    for i, t, words, addr, blob in walk():
        rec = {'i': i, 'type': t, 'words': words}
        if t in OBJ_TYPES:
            es, st = struct.unpack_from('>HB', blob, 0)
            model, padn, flags, flags2, prop, mdl = struct.unpack_from('>hhIIII', blob, 4)
            rec.update({'model': model, 'pad': padn, 'flags': flags, 'flags2': flags2, 'state': st,
                        'extrascale': es})
            pb = None
            if prop:
                try:
                    pb = peek(prop, 0x34)
                except IOError:
                    rec['prop_raw'] = '%08x' % prop   # not a pointer: the record is not made (yet)
            if pb is not None:
                m = struct.unpack_from('>16f', blob, 0x18)
                rec.update({'exists': 1,
                            'pos': [_f(v) for v in struct.unpack_from('>3f', pb, PR_POS)],
                            'rtpos': [_f(v) for v in struct.unpack_from('>3f', blob, 0x58)],
                            'rooms': _rooms(prop),
                            'rot': [[m[r * 4 + c] for c in range(3)] for r in range(3)],
                            'maxdamage': _f(struct.unpack_from('>f', blob, 0x70)[0]),
                            'damage': _f(struct.unpack_from('>f', blob, 0x74)[0]),
                            'rtflags': struct.unpack_from('>I', blob, 0x64)[0],
                            'attached': 1 if struct.unpack_from('>I', pb, PR_PARENT)[0] else 0,
                            'propflags': pb[1]})
                if mdl:
                    rec['scale'] = _f(f32(mdl + 20))
                if t == 1 and len(blob) >= 0xb8:
                    rec['door_slide'] = [_f(v) for v in struct.unpack_from('>3f', blob, 168)]
                    rec['door_maxFrac'] = _f(struct.unpack_from('>f', blob, 132)[0])
                    rec['door_openPosition'] = _f(struct.unpack_from('>f', blob, 180)[0])
            else:
                rec['exists'] = 0
        out.append(rec)
    return out


_ailist_cache = {}


def _ailist_id(ptr):
    """An AI list pointer's id: the setup's own table, else GoldenEye's global
    table in the xex image (found by the pointer; the id follows it)."""
    if not ptr:
        return -1
    if ptr in _ailist_cache:
        return _ailist_cache[ptr]
    base, i, r = setup(5), 0, 'ptr'
    while base and i < 2000:
        lst, ident = struct.unpack('>Ii', peek(base + 8 * i, 8))
        if lst == 0:
            break
        if lst == ptr:
            r = ident
            break
        i += 1
    if r == 'ptr':
        hits = find(0x82000000, 0x2000000, struct.pack('>I', ptr))
        for h in hits:
            ident = struct.unpack('>i', peek(h + 4, 4))[0]
            if 0 <= ident < 0x2000:
                r = ident
                break
    _ailist_cache[ptr] = r
    return r


def _all_props():
    """Bean's live prop list, from players[0]'s prop through prev/next."""
    p = u32(u32(PLAYERS) + PL_PROP)
    seen = set()
    while True:
        pv = u32(p + 0x28)
        if not pv or pv in seen or len(seen) > 6000:
            break
        seen.add(pv)
        p = pv
    out, seen = [], set()
    while p and p not in seen and len(seen) < 6000:
        seen.add(p)
        out.append(p)
        p = u32(p + 0x2c)
    return out


def chrs():
    """Bean's chrs (props of type 3); ChrRecord is the N64 layout as far as read here."""
    out = []
    for pr in _all_props():
        pb = peek(pr, 0x34)
        if pb[0] != 3:
            continue
        c = struct.unpack_from('>I', pb, PR_OBJ)[0]
        b = peek(c, 0x170)
        chrnum, acc, spd = struct.unpack_from('>hbb', b, 0)
        rec = {'chrnum': chrnum, 'slot': c, 'headnum': struct.unpack_from('>b', b, 6)[0],
               'actiontype': struct.unpack_from('>b', b, 7)[0], 'sleep': struct.unpack_from('>b', b, 8)[0],
               'bodynum': struct.unpack_from('>b', b, 15)[0], 'hidden': struct.unpack_from('>H', b, 18)[0],
               'chrflags': struct.unpack_from('>I', b, 20)[0],
               'accuracyrating': acc, 'speedrating': spd,
               'visionrange': _f(struct.unpack_from('>f', b, 208)[0]),
               'hearingscale': _f(struct.unpack_from('>f', b, 236)[0]),
               'damage': _f(struct.unpack_from('>f', b, 252)[0]),
               'maxdamage': _f(struct.unpack_from('>f', b, 256)[0]),
               'ailist': _ailist_id(struct.unpack_from('>I', b, 260)[0]),
               'aioffset': struct.unpack_from('>H', b, 264)[0],
               'morale': b[268], 'alertness': b[269], 'flags2': b[270],
               'padpreset1': struct.unpack_from('>h', b, 276)[0], 'chrpreset1': struct.unpack_from('>h', b, 278)[0],
               'pos': [_f(v) for v in struct.unpack_from('>3f', pb, PR_POS)], 'rooms': _rooms(pr),
               'weapons': []}
        for h in range(2):
            wp = struct.unpack_from('>I', b, 352 + 4 * h)[0]
            wn = None
            if wp:
                o = u32(wp + PR_OBJ)
                wn = struct.unpack('>b', peek(o + 4 * 32, 1))[0] if o else None   # WeaponObjRecord.weaponnum (N64 +0x80)
            rec['weapons'].append(wn)
        mdl = struct.unpack_from('>I', b, 28)[0]
        if mdl:
            rec['scale'] = _f(f32(mdl + 20))
        out.append(rec)
    return out


def player():
    P = u32(PLAYERS)
    prop = u32(P + PL_PROP)
    coll = P + PL_COLL
    return {'pos': [_f(v) for v in struct.unpack('>3f', peek(prop + PR_POS, 12))], 'rooms': _rooms(prop),
            'theta': _f(f32(P + PL_THETA)), 'verta': _f(f32(P + PL_VERTA)),
            'eye': [_f(v) for v in struct.unpack('>3f', peek(coll + 44, 12))]}


def world():
    pause()
    return {'side': SIDE, 'bean': BEAN, 'tick': tick(), 'pads': pads(), 'props': props(), 'chrs': chrs(),
            'player': player(), 'bean_deltas': {str(k): v for k, v in _st.get('deltas', {}).items()}}


def emit(path, data):
    with open(path, 'w') as fh:
        json.dump(data, fh, separators=(',', ':'))
    say('wrote', path)


# --------------------------------------------------------- Bond on the spot

def look_ahead(on):
    """GoldenEye's Look Ahead (settings bit 0x80) tilts the view on slopes; off for pictures."""
    sp = u32(SETTINGS)
    if sp:
        v = u32(sp + 0x298)
        v = (v | 0x80) if on else (v & ~0x80)
        poke(sp + 0x298, struct.pack('>I', v))


def pad_spot(n):
    """A pad's (x, y, z) and its own floor tile, as GoldenEye's spawn code uses it."""
    if n >= 10000:
        base, stride, k = setup(7), 0x44, n - 10000
    else:
        base, stride, k = setup(6), 0x2c, n
    rec = peek(base + k * stride, 0x2c)
    x, y, z = struct.unpack_from('>3f', rec, 0)
    return x, y, z, struct.unpack_from('>I', rec, 0x28)[0]


BEAN_XZ = ((0x4ec, 0x4f4, 1.0), (0x530, 0x538, 1.0), (0x4e0, 0x4e8, 10.0))


def place(x, y, z, theta=None, verta=None, stan=None):
    """Bond's feet at x, z (GoldenEye's world, which Bean keeps) on tile `stan`
    (pad_spot()'s); writes what gdbge.place() writes, at Bean's offsets."""
    P = u32(PLAYERS)
    prop = u32(P + PL_PROP)
    coll = P + PL_COLL
    poke(prop + PR_POS, struct.pack('>f', x))
    poke(prop + PR_POS + 8, struct.pack('>f', z))
    poke(coll + 4, struct.pack('>f', x))
    poke(coll + 12, struct.pack('>f', z))
    # Bean keeps Bond's x/z three more times in its own player struct and slides
    # him back to them if only the prop and collision copies move (the view
    # agent's view/xenia_side.py found it): at +0x4ec/+0x4f4, +0x530/+0x538, and
    # ten times them at +0x4e0/+0x4e8 (the N64's field_3B8 = pos / 0.1)
    for ox, oz, k in BEAN_XZ:
        poke(P + ox, struct.pack('>f', x * k))
        poke(P + oz, struct.pack('>f', z * k))
    if stan:
        for a in (prop + PR_STAN, coll, coll + 80):
            poke(a, struct.pack('>I', stan))
    if theta is not None:
        poke(P + PL_THETA, struct.pack('>f', theta))
    if verta is not None:
        poke(P + PL_VERTA, struct.pack('>f', verta))
    return stan


def hold(x, y, z, theta=None, verta=None, n=8, stan=None):
    """Place Bond and keep him there for n times three frames, the view settling;
    the game is left waiting at a swap, so shot() photographs this frame."""
    for _ in range(n):
        place(x, y, z, theta, verta, stan)
        frames(3)
    place(x, y, z, theta, verta, stan)
    frames(1)
    return stan


def hold_pad(n, theta, verta=0.0, frames_n=8):
    x, y, z, stan = pad_spot(n)
    return hold(x, y, z, theta, verta, frames_n, stan)


def shot(path):
    """The presented frame, 1280x695 (the window under Xenia's 25 px menu bar).
    Take it while the game waits at a swap (after frames()/hold())."""
    subprocess.run([RIG, 'shot', path], check=True, capture_output=True)
    return path

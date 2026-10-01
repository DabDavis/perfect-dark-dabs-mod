"""The GoldenEye half of the twin driver on the REAL cartridge: the US ROM in
ares, through n64twin (~/claude-007/ares/ares-nightly/oracle/twin.cpp on the
oracle host), with gdbge.py's API name for name - so world/dump.py and any
scenario that keeps to the API runs unchanged on the console.

    twin.py both world/dump.py --mission dam --out OUT            # ares is the default
    twin.py both world/dump.py --mission dam --out OUT --oracle port

Why: the native port is a decompilation run natively with its own RDP and is
not complete; where it and ours disagree it can be the port that is wrong
(Frigate's sea came out green there). ares runs the cartridge.

What is different from gdbge.py, and why:
- No function calls: sizepropdef() is the decomp's table below, and Bond is
  placed on a tile the caller names (a pad's own stan, PadRecord.stan) - there
  is no stanFindFloorTileBelowY to ask.
- Memory is read through the CPU's data cache (n64twin does it), big-endian, by
  whole record; the layouts are ares_layout.json (gen_ares_layout.py: the
  -m32 port's DWARF, whose record structs are the ROM's, and the N64 ELF's
  symbols).
- The front end is walked by pressing START until GoldenEye asks for Dam, not
  by a fixed pad script: the cartridge reaches each folder screen tens of
  frames later than the port does (solo-quiet.padscript stalls on the
  briefing). Dam's load is swapped for the wanted level at bossSetLoadedStage
  (a0), the difficulty is written at proplvreset2, and the one Z press that
  dismisses the opening still lands 190 frames after the level's first frame,
  as solo-quiet.padscript's does on the port.
- A stop is at the end of a video frame, not at a function entry: a tick can
  overshoot by the frame's step (GoldenEye advances 2-3 ticks a frame).
"""
import json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIDE = 'ge'
ORACLE = 'ares'
L = json.load(open(os.path.join(HERE, 'ares_layout.json')))
SYM = L['symbols']
T = L['types']
# ares/build.sh installs build/n64twin.installed atomically; a build run any other
# way writes build/n64twin in place, which is the fallback
_INSTALLED = os.path.expanduser('~/claude-007/ares/ares-nightly/build/n64twin.installed')
EXE = os.path.expanduser(os.environ.get('GF_ARES_EXE', _INSTALLED if os.path.exists(_INSTALLED)
                                        else '~/claude-007/ares/ares-nightly/build/n64twin'))


def _snapshot(exe):
    """A private copy of n64twin, named by the binary's own time and size: a
    rebuild in progress (other work rebuilds it on the oracle host) leaves the
    build path unreadable or half written for a moment, and a sweep that ran
    the build path straight died with 'Permission denied' on eight missions."""
    import shutil, time
    snapdir = os.path.expanduser('~/gefidelity-bin')
    os.makedirs(snapdir, exist_ok=True)
    for attempt in range(60):
        try:
            st = os.stat(exe)
            # a link in progress leaves the file empty or short for longer than five
            # seconds (a 0-byte snapshot was taken once and ran as 'Exec format error')
            with open(exe, 'rb') as fh:
                if st.st_size < (1 << 20) or fh.read(4) != b'\x7fELF':
                    raise OSError('n64twin is not a whole executable yet (%d bytes); a build is writing it' % st.st_size)
            snap = os.path.join(snapdir, 'n64twin.%d.%d' % (int(st.st_mtime), st.st_size))
            if not os.path.exists(snap):
                if time.time() - st.st_mtime < 5:
                    raise OSError('n64twin was written %.1f s ago; letting the build finish' % (time.time() - st.st_mtime))
                tmp = '%s.tmp%d' % (snap, os.getpid())
                shutil.copy2(exe, tmp)
                if os.path.getsize(tmp) != st.st_size:
                    raise OSError('n64twin changed while it was copied')
                os.chmod(tmp, 0o755)
                os.replace(tmp, snap)
            return snap
        except OSError:
            time.sleep(2)
    raise RuntimeError('n64twin at %s stayed unreadable for two minutes' % exe)
ROM = os.path.expanduser(os.environ.get('GF_ARES_ROM', '~/claude-007/007/build/u/ge007.u.z64'))

# sizepropdef() in words (loadobjectmodel.c:47), checked against the port's walk
# of all twenty missions; anything else is the one-word header
SIZEPROPDEF = {1: 64, 2: 2, 3: 32, 4: 33, 5: 32, 6: 0x3b, 7: 0x21, 8: 0x22, 9: 7, 10: 0x40, 11: 0x95,
               12: 32, 13: 0x36, 14: 3, 17: 32, 18: 3, 19: 4, 20: 0x2d, 21: 0x22, 22: 4, 23: 4, 24: 1,
               25: 2, 26: 2, 27: 2, 28: 2, 29: 2, 30: 4, 31: 1, 32: 4, 33: 5, 34: 1, 35: 4, 36: 32,
               37: 10, 38: 4, 39: 0x2c, 40: 0x2d, 42: 32, 43: 32, 44: 5, 45: 0x38, 46: 7, 47: 37}
OBJ_TYPES = {1, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 17, 20, 21, 36, 39, 40, 42, 43, 45, 47}
PROPDEF_END = 0x30
DISMISS_STILL_AFTER = 190    # frames; solo-quiet.padscript's Z at 1300 against a level start at ~1110

_st = {'t0': 0}


def say(*a):
    print('GF', *a, flush=True)


def _slot():
    """One of GF_ARES_SLOTS (default 5) machine-wide slots on the oracle host,
    held until this process exits. Every n64twin is started through here: four
    agents each told "at most 4" once ran nine at a time on eight cores with a
    gigabyte of memory left, and a run that is starved looks like a hang."""
    import fcntl, time
    n = int(os.environ.get('GF_ARES_SLOTS', '5'))
    d = os.path.expanduser('~/gefidelity-bin')
    os.makedirs(d, exist_ok=True)
    waited = 0
    while True:
        for i in range(n):
            fh = open(os.path.join(d, 'slot.%d' % i), 'a+')
            try:
                fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                fh.close()
                continue
            fh.seek(0); fh.truncate(); fh.write('%d %s\n' % (os.getpid(), ' '.join(sys.argv)[:200])); fh.flush()
            return fh
        if waited % 60 == 0:
            say('waiting for one of %d n64twin slots (%d s)' % (n, waited))
        time.sleep(2)
        waited += 2


class _Twin:
    def __init__(self):
        self.slot = _slot()
        self.p = subprocess.Popen([_snapshot(EXE), '--rom', ROM], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.read()

    def read(self):
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('n64twin exited')
            line = line.rstrip('\n')
            if line.startswith('ok'):
                return line[2:].strip()
            if line.startswith('err'):
                raise RuntimeError('n64twin: ' + line)
            # ares' own chatter (shader compiles and the like) is not an answer

    def __call__(self, cmd):
        self.p.stdin.write(cmd + '\n')
        self.p.stdin.flush()
        return self.read()

    def close(self):
        try:
            self('quit')
        except Exception:
            pass
        try:
            self.p.wait(10)
        except Exception:
            self.p.kill()


_tw = None


def twin():
    global _tw
    if _tw is None:
        _tw = _Twin()
    return _tw


# ------------------------------------------------------------------ memory

def peek(addr, n):
    if n <= 0:
        return b''
    out = b''
    while n > 0:
        k = min(n, 4096)
        out += bytes.fromhex(twin()('peek 0x%08x %d' % (addr, k)))
        addr += k
        n -= k
    return out


def poke(addr, data):
    twin()('poke 0x%08x %s' % (addr, data.hex()))


def u32(addr):
    return struct.unpack('>I', peek(addr, 4))[0]


def s32(addr):
    return struct.unpack('>i', peek(addr, 4))[0]


def _fmt(ent):
    t = ent['type']
    if '*' in t:
        return '>I'
    sz = ent['size']
    if t in ('f32', 'float'):
        return '>f'
    signed = not (t.startswith('u') or 'unsigned' in t)
    return {1: '>b' if signed else '>B', 2: '>h' if signed else '>H', 4: '>i' if signed else '>I'}.get(sz)


class Rec:
    """One record read whole; fields by the port's DWARF names."""
    def __init__(self, typ, addr, data=None):
        self.t = T[typ]
        self.addr = addr
        self.b = data if data is not None else peek(addr, self.t['size'])

    def off(self, f):
        return self.t['fields'][f]['off']

    def __getitem__(self, f):
        ent = self.t['fields'][f]
        fmt = _fmt(ent)
        v = struct.unpack_from(fmt, self.b, ent['off'])[0]
        if 'bits' in ent:
            v = (v >> (ent['size'] * 8 - ent['bitpos'] - ent['bits'])) & ((1 << ent['bits']) - 1)
        return v

    def f(self, f, i=0):
        return struct.unpack_from('>f', self.b, self.off(f) + 4 * i)[0]

    def vec(self, f):
        return [self.f(f, i) for i in range(3)]

    def ptr(self, f):
        return struct.unpack_from('>I', self.b, self.off(f))[0]

    def byte(self, f, i=0):
        return self.b[self.off(f) + i]


def _addr(field_of, typ, field):
    return field_of + T[typ]['fields'][field]['off']


# GoldenEye's two generators, u64 each (nm build/u/ge007.u.elf: the US ROM's
# addresses; the layout's own symbols win when it has them)
SEED_SYMS = {'g_randomSeed': 0x80024460, 'g_chrObjRandomSeed': 0x80040160}
# where the random-head rotation starts (u32 each, chr.c:197): it differs from
# boot to boot, so with the seeds pinned the heads still came out shifted
HEAD_SYMS = {'current_random_male_head': 0x8002ce38, 'current_random_female_head': 0x8002ce3c}

# ------------------------------------------------------------- the clock

def boot(levelid, difficulty=0):
    tw = twin()
    lv = L['levelids']
    want = lv[levelid] if isinstance(levelid, str) else int(levelid)
    dam = lv['LEVELID_DAM']
    tw('on-pc 0x%08x if 4 %d set-gpr 4 %d' % (SYM['bossSetLoadedStage'], dam, want))
    # walk the front end: START until GoldenEye asks for Dam (title, folder,
    # mission, difficulty, briefing all take it)
    for i in range(400):
        tw('pad 0 START')
        tw('frames 3')
        tw('pad 0 -')
        tw('frames 6')
        if int(tw('fired').split()[0]) >= 1:
            break
    else:
        raise RuntimeError('the front end never asked for Dam')
    tw('on-pc 0x%08x poke 0x%08x %08x' % (SYM['proplvreset2'], SYM['g_SelectedDifficulty'], difficulty & 0xffffffff))
    # The cartridge's random seeds, pinned at the setup load (before a guard
    # picks a random head): without this two runs of one binary differed in
    # every random head, sleep timer and patrol, so the oracle could not be
    # compared with itself. A pinned seed is still one of the cartridge's own
    # runs, as --rng-seed is one of ours. GF_SEED=off leaves it alone.
    nfire = 2
    seed = os.environ.get('GF_SEED', '1')
    if seed != 'off':
        for name in SEED_SYMS:
            addr = SYM.get(name, SEED_SYMS[name])
            tw('on-pc 0x%08x poke 0x%08x %016x' % (SYM['proplvreset2'], addr, int(seed, 0) & 0xffffffffffffffff))
            nfire += 1
        for name in HEAD_SYMS:
            addr = SYM.get(name, HEAD_SYMS[name])
            tw('on-pc 0x%08x poke 0x%08x %08x' % (SYM['proplvreset2'], addr, 0))
            nfire += 1
    tw('until-fired %d 20000' % nfire)
    tw('until-pc 0x%08x 1 20000' % SYM['lvlRender'])
    # positive controls: the cartridge took the difficulty and the swap, or nothing here is the level asked for
    if s32(SYM['g_SelectedDifficulty']) != difficulty:
        raise RuntimeError('difficulty %d did not take (cartridge has %d)' % (difficulty, s32(SYM['g_SelectedDifficulty'])))
    if int(tw('fired').split()[0]) < nfire:
        raise RuntimeError('the level swap, the difficulty or the seed writes never fired')
    _st['t0'] = s32(SYM['g_GlobalTimer']) - 1
    f0 = s32(SYM['currentFrameCounter'])
    tw('cue %d 0 Z' % (f0 + DISMISS_STILL_AFTER))
    tw('cue %d 0 -' % (f0 + DISMISS_STILL_AFTER + 10))
    tw('pad 0 script')
    say('boot', levelid, 'difficulty', s32(SYM['g_SelectedDifficulty']), 'tick', tick(), 'frame', f0,
        'globaltimer', s32(SYM['g_GlobalTimer']), 'oracle ares')


def tick():
    return s32(SYM['g_GlobalTimer']) - _st['t0']


def until_tick(t):
    if tick() >= t:
        return
    twin()('until-word 0x%08x >= %d 200000' % (SYM['g_GlobalTimer'], t + _st['t0']))


def frames(n=1):
    twin()('until-word 0x%08x >= %d 200000' % (SYM['currentFrameCounter'], s32(SYM['currentFrameCounter']) + n))


def _player():
    return u32(SYM['g_CurrentPlayer'])


def place(x, y, z, theta=None, verta=None, stan=None):
    """Bond's feet at x, z on tile `stan` (GoldenEye's world). There is no tile
    search on the console: pass a pad's own tile (pad_tile()), or none to keep
    Bond's current tile."""
    P = _player()
    prop = u32(P + T['struct player']['fields']['prop']['off'])
    F = T['struct player']['fields']
    pk = lambda a, v: poke(a, struct.pack('>f', v))
    pk(prop + T['PropRecord']['fields']['pos']['off'] + 0, x)
    pk(prop + T['PropRecord']['fields']['pos']['off'] + 8, z)
    pk(P + F['field_488.collision_position']['off'] + 0, x)
    pk(P + F['field_488.collision_position']['off'] + 8, z)
    if stan:
        s = struct.pack('>I', stan)
        poke(prop + T['PropRecord']['fields']['stan']['off'], s)
        poke(P + F['field_488.current_tile_ptr']['off'], s)
        poke(P + F['field_488.current_tile_ptr_for_portals']['off'], s)
    if theta is not None:
        pk(P + F['vv_theta']['off'], theta)
    if verta is not None:
        pk(P + F['vv_verta']['off'], verta)
    return stan


def hold(x, y, z, theta=None, verta=None, n=8, stan=None):
    for _ in range(n):
        place(x, y, z, theta, verta, stan)
        frames(3)
    place(x, y, z, theta, verta, stan)
    frames(1)
    return stan


def pad_tile(padnum):
    """A pad's own floor tile, as GoldenEye's spawn code takes it."""
    setup = Rec('stagesetup', SYM['g_CurrentSetup'])
    if padnum >= 10000:
        a = setup.ptr('boundpads') + (padnum - 10000) * T['BoundPadRecord']['size']
        return Rec('BoundPadRecord', a).ptr('stan')
    a = setup.ptr('pads') + padnum * T['PadRecord']['size']
    return Rec('PadRecord', a).ptr('stan')


def shot(path):
    twin()('shot %s' % path)
    return path


def finish():
    if _tw is not None:
        _tw.close()
    sys.exit(0)


# ------------------------------------------------------------- the world

def _f(v):
    return round(float(v), 3)


def _rooms(prop):
    out = []
    for r in range(4):
        b = prop.byte('rooms', r)
        if b == 0xff:
            break
        out.append(b)
    return out


def _tileroom(stan):
    if not stan:
        return -1
    return Rec('StandTile', stan)['room']


def pads():
    out = []
    setup = Rec('stagesetup', SYM['g_CurrentSetup'])
    for field, rec, base in (('pads', 'PadRecord', 0), ('boundpads', 'BoundPadRecord', 10000)):
        a = setup.ptr(field)
        if not a:
            continue
        size = T[rec]['size']
        blob = peek(a, size * 64)
        i = 0
        while i < 4000:
            if (i + 1) * size > len(blob):
                blob += peek(a + len(blob), size * 64)
            p = Rec(rec, a + i * size, blob[i * size:(i + 1) * size])
            if p.ptr('plink') == 0:
                break
            pos = p.vec('pos')
            out.append([base + i, _f(pos[0]), _f(pos[1]), _f(pos[2]), _tileroom(p.ptr('stan'))])
            i += 1
    return out


def _ailist_ids():
    ids = {}
    setup = Rec('stagesetup', SYM['g_CurrentSetup'])
    for a in (setup.ptr('ailists'), SYM['g_GlobalAILists']):
        i = 0
        while a and i < 2000:
            r = Rec('AIListRecord', a + 8 * i)
            if r.ptr('ailist') == 0:
                break
            ids[r.ptr('ailist')] = r['ID']
            i += 1
    return ids


def _weaponnum(propaddr):
    if not propaddr:
        return None
    prop = Rec('PropRecord', propaddr)
    w = prop.ptr('weapon')
    if not w:
        return None
    return Rec('WeaponObjRecord', w)['weaponnum']


def props():
    out = []
    setup = Rec('stagesetup', SYM['g_CurrentSetup'])
    p = setup.ptr('propDefs')
    i = 0
    while i < 5000:
        hdr = Rec('PropDefHeaderRecord', p)
        t = hdr['type']
        if t == PROPDEF_END:
            break
        words = SIZEPROPDEF.get(t, 1)
        rec = {'i': i, 'type': t, 'words': words}
        if t in OBJ_TYPES:
            o = Rec('ObjectRecord', p)
            rec.update({'model': o['obj'], 'pad': o['pad'], 'flags': o['flags'] & 0xffffffff,
                        'flags2': o['flags2'] & 0xffffffff, 'state': hdr['state'], 'extrascale': hdr['extrascale']})
            pa = o.ptr('prop')
            if pa:
                prop = Rec('PropRecord', pa)
                mo = o.off('mtx')
                rot = [[_f(struct.unpack_from('>f', o.b, mo + 16 * r + 4 * c)[0]) for c in range(3)] for r in range(3)]
                rec.update({'exists': 1, 'pos': [_f(v) for v in prop.vec('pos')],
                            'rtpos': [_f(v) for v in o.vec('runtime_pos')],
                            'rooms': _rooms(prop), 'rot': rot,
                            'damage': _f(o.f('damage')), 'maxdamage': _f(o.f('maxdamage')),
                            'rtflags': o['runtime_bitflags'] & 0xffffffff,
                            'attached': 1 if prop.ptr('parent') else 0, 'propflags': prop['flags']})
                if t == 1:
                    # setupDoor() never runs domakedefaultobj()'s `damage = word / 65536`
                    # (prop.c:158), so a door keeps the setup's 16.16 integer in its
                    # float health words (Dam's: 0x03e80000, 1.4e-36 as a float, read
                    # as 0.0 here until 2026-10-01 - every door "had no health")
                    rec['damage'] = _f(struct.unpack_from('>i', o.b, o.off('damage'))[0] / 65536.0)
                    rec['maxdamage'] = _f(struct.unpack_from('>i', o.b, o.off('maxdamage'))[0] / 65536.0)
                    rec['health_raw'] = 1

"""Every AI command each side runs, for one mission's first GF_AI_TICKS 60ths,
with Bond invincible (and, with GF_AI_INVISIBLE=1, unseen) at his spawn.

    twin.py both ai/trace.py --mission dam --out OUT --env PORT_AI_TRACE=0:2000000000 \
        --env GF_AI_TICKS=5400 --pd-arg=--ai-trace --pd-arg=5410

(ai/aidiff.py --run does exactly that.) GoldenEye's commands come from the
oracle's own PORT_AI_TRACE printf, one "[ai] ... chr N: offset O cmd C random R"
line each; which list they are in, and the tick, is not in that line, so a
breakpoint at ai()'s first instruction prints a marker before every list run:

    GFAI <g_GlobalTimer> <c|o> <chrnum or -1> <record address> <list id>[g] <aioffset> <aireturnlist>

Ours comes from --ai-trace in pd.log ("ai: f<frame> chr N list L +O cmd C ...").
Ours plays the US cartridge's setups, as the oracle does (GF_AI_REVISION=1 for
the later revision's, Mod.GePlusRevisionFixes' default).

The breakpoint goes in before the level's first frame, which is why this boots
GoldenEye itself rather than through gdbge.boot(): that deletes every
breakpoint on its way, and a guard's first tick is where its list starts.
"""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
if os.environ.get('GF_ORACLE') != 'ares':
    import gdb

SIDE = os.environ['GF_SIDE']
TICKS = int(os.environ.get('GF_AI_TICKS', '5400'))
INVISIBLE = os.environ.get('GF_AI_INVISIBLE', '0') == '1'


def ge():
    import gdbge as L
    ev = L.ev
    st = {'ids': None, 'n': 0}

    def ids():
        out = {}
        for table in ('g_CurrentSetup.ailists', 'g_GlobalAILists'):
            base = ev(table)
            if base.type.code == gdb.TYPE_CODE_ARRAY:
                base = base[0].address
            if int(base) == 0:
                continue
            rows, i = [], 0
            while i < 4000:
                lst = int(base[i]['ailist'])
                if lst == 0:
                    break
                rows.append((lst, int(base[i]['ID'])))
                i += 1
            if table == 'g_CurrentSetup.ailists':
                # a second row of a background id runs as a list of its own;
                # the conversion gives it the next free id (aimap.renumber_bg_duplicates)
                nextbg = max([0x1000] + [r[1] + 1 for r in rows if 0x1000 <= r[1] < 0xffff])
                seen = set()
                for k, (lst, lid) in enumerate(rows):
                    if 0x1000 <= lid < 0xffff and lid in seen and lst not in out:
                        rows[k] = (lst, nextbg)
                        nextbg += 1
                    seen.add(lid)
            for lst, lid in rows:
                out.setdefault(lst, (lid, table != 'g_CurrentSetup.ailists'))
        return out

    inf = gdb.selected_inferior

    # ChrRecord's chrnum, ailist, aioffset, aireturnlist and a vehicle's or an
    # aircraft's ailist block, from `ptype /o` against the -m32 build. Read raw:
    # one stop per list run, and gdb.Value lookups made it ten times slower.
    CHR_AILIST, OBJ_AILIST = 260, 0x80
    timer_at = int(ev('(unsigned int)&g_GlobalTimer'))

    class AiEntry(gdb.Breakpoint):
        def stop(self):
            # break *ai: the -m32 arguments are on the stack above the return address
            I = inf()
            sp = int(gdb.selected_frame().read_register('esp'))
            mem = I.read_memory(sp + 4, 8).tobytes()
            ent = int.from_bytes(mem[0:4], 'little')
            et = int.from_bytes(mem[4:8], 'little')
            if et == 3:
                kind = 'c'
                who = int.from_bytes(I.read_memory(ent, 2).tobytes(), 'little', signed=True)
                raw = I.read_memory(ent + CHR_AILIST, 8).tobytes()
            else:
                kind, who = 'o', -1
                raw = I.read_memory(ent + OBJ_AILIST, 8).tobytes()
            lst = int.from_bytes(raw[0:4], 'little')
            off = int.from_bytes(raw[4:6], 'little')
            ret = int.from_bytes(raw[6:8], 'little', signed=True)
            timer = int.from_bytes(I.read_memory(timer_at, 4).tobytes(), 'little', signed=True)
            if st['ids'] is None or (lst and lst not in st['ids']):
                st['ids'] = ids()
            lid, glob = st['ids'].get(lst, (-1, False))
            print('GFAI %d %s %d %d %d%s %d %d' % (timer, kind, who, ent, lid, 'g' if glob else '', off, ret),
                  flush=True)
            st['n'] += 1
            return False

    gdb.execute('break bossSetLoadedStage if stage == LEVELID_DAM')
    gdb.execute('run')
    gdb.execute('set variable stage = %s' % os.environ['GF_LEVELID'])
    gdb.execute('delete')
    gdb.execute('break proplvreset2')
    gdb.execute('continue')
    gdb.execute('set variable g_SelectedDifficulty = %d' % int(os.environ.get('GF_DIFF', '0')))
    gdb.execute('delete')
    AiEntry('*ai')
    first = gdb.Breakpoint('lvlRender', temporary=True)
    gdb.execute('continue')
    t0 = int(ev('g_GlobalTimer')) - 1
    print('GFAI t0 %d' % t0, flush=True)
    gdb.execute('set variable g_CurrentPlayer->cheatBondInvincible = 1')
    if INVISIBLE:
        L.call('(void)bondviewSetVisibleToGuardsFlag(0)')
    L.say('ge boot', os.environ['GF_LEVELID'], 'difficulty', int(ev('g_SelectedDifficulty')), 't0', t0)
    end = gdb.Breakpoint('lvlRender', internal=True)
    end.condition = 'g_GlobalTimer >= %d' % (TICKS + t0)
    gdb.execute('continue')
    L.say('ge done tick', int(ev('g_GlobalTimer')) - t0, 'ai runs', st['n'])
    L.finish()


def ge_ares():
    """GoldenEye on the cartridge (ares, aresge.py). ares hooks a function's
    entry, never a line inside one, so the interpreter's commands cannot be
    traced one by one; instead every chr's (ailist, aioffset) is read each
    video frame. GoldenEye runs a chr's list at most once a frame and a run
    ends at a Yield, so the trail of where each list sits is the chain of
    Yields it reached - which branches it took, between two of them:

        GFAS <tick> <stream> <our list id> <GoldenEye offset>

    printed when a stream's place changes. Streams: a chr slot's occupant
    (named chrN by the number it first had, spawn@L for 5000 and up -
    SetMyChrNum renames it, the slot keeps it), g_ActiveChrs[k] as bg@L (one
    per background row, in table order), a vehicle's record as obj@L.
    """
    import struct, subprocess
    sys.path.insert(0, os.path.dirname(os.environ['GF_COMMON'].rstrip('/')))
    import aresge as A, time
    # n64twin is rebuilt in place now and then (another agent extends it):
    # wait for an executable binary rather than fail the mission
    for _ in range(180):
        if os.access(A.EXE, os.X_OK) and os.path.getsize(A.EXE) > 0:
            break
        time.sleep(5)
    time.sleep(2)
    syms = dict(A.SYM)
    # three the layout does not carry, from the ROM's own ELF (fallbacks read
    # off it 2026-10-01, and cross-checked against the layout's g_ChrSlots)
    extra = {'g_ActiveChrs': 0x8003097c, 'g_ActiveChrsCount': 0x80030980, 'g_VisibleToGuardsFlag': 0x800364c4}
    elf = os.path.expanduser('~/claude-007/007/build/u/ge007.u.elf')
    for nm in ('mips-linux-gnu-nm', 'nm'):
        try:
            out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
        except OSError:
            continue
        got = {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}
        if got.get('g_ChrSlots') == syms['g_ChrSlots']:
            extra = {k: got.get(k, v) for k, v in extra.items()}
            break
    syms.update(extra)
    A.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    P = A.u32(syms['g_CurrentPlayer'])
    A.poke(P + A.T['struct player']['fields']['cheatBondInvincible']['off'], b'\x01')
    if INVISIBLE:
        A.poke(syms['g_VisibleToGuardsFlag'], struct.pack('>i', 0))

    # pointer -> our list id: the level's rows (a second background row of
    # one id renumbered as the conversion does) and the globals at 0x800 + id
    setup = A.Rec('stagesetup', syms['g_CurrentSetup'])
    rows, a = [], setup.ptr('ailists')
    while a:
        lst, lid = struct.unpack('>Ii', A.peek(a, 8))
        if not lst:
            break
        rows.append([lst, lid])
        a += 8
    nextbg = max([0x1000] + [r[1] + 1 for r in rows if 0x1000 <= r[1] < 0xffff])
    seen = set()
    for r in rows:
        if 0x1000 <= r[1] < 0xffff and r[1] in seen:
            r[1] = nextbg
            nextbg += 1
        seen.add(r[1])
    ids = {}
    for lst, lid in rows:
        ids.setdefault(lst, lid)
    a = syms['g_GlobalAILists']
    while True:
        lst, lid = struct.unpack('>Ii', A.peek(a, 8))
        if not lst:
            break
        ids.setdefault(lst, 0x800 + lid)
        a += 8
    bgnames = ['bg@%d' % lid for lst, lid in rows if lid >= 0x1000]
    # vehicles and aircraft run lists from their records (ailist at 0x80)
    vehicles = []
    p = setup.ptr('propDefs')
    for _ in range(5000):
        t = A.peek(p + 3, 1)[0]
        if t == A.PROPDEF_END:
            break
        if t in (39, 40):
            vehicles.append(p)
        p += 4 * A.SIZEPROPDEF.get(t, 1)
    CS = A.T['ChrRecord']['size']
    MODEL = A.T['ChrRecord']['fields']['model']['off']
    occupant, last = {}, {}
    nocc = [0]

    def emit(key, lst, off, tick):
        pid = ids.get(lst, -1)
        if last.get(key) != (pid, off):
            last[key] = (pid, off)
            print('GFAS %d %s %d %d' % (tick, key, pid, off), flush=True)

    A.say('ge ares boot t0', A._st['t0'], 'bg', len(bgnames), 'vehicles', len(vehicles))
    while True:
        tick = A.tick()
        n = A.s32(syms['g_NumChrSlots'])
        base = A.u32(syms['g_ChrSlots'])
        blob = A.peek(base, CS * n) if n > 0 else b''
        for k in range(n):
            c = blob[k * CS:(k + 1) * CS]
            num = struct.unpack_from('>h', c, 0)[0]
            lst = struct.unpack_from('>I', c, 260)[0]
            # GoldenEye's own test for a live slot is its model (chrFree()
            # clears it); slots past the allocated ones hold junk
            if num < 0 or not struct.unpack_from('>I', c, MODEL)[0]:
                occupant.pop(k, None)
                continue
            if k not in occupant:
                if num >= 5000:
                    if lst not in ids:
                        continue            # named by its first list: wait for one
                    occupant[k] = 'spawn@%d' % ids[lst]
                else:
                    occupant[k] = 'chr%d' % num
            if lst:
                emit(occupant[k], lst, struct.unpack_from('>H', c, 264)[0], tick)
        na = A.s32(syms['g_ActiveChrsCount'])
        ab = A.u32(syms['g_ActiveChrs'])
        if na > 0 and ab:
            blob = A.peek(ab, CS * na)
            for k in range(min(na, len(bgnames))):
                c = blob[k * CS:(k + 1) * CS]
                lst = struct.unpack_from('>I', c, 260)[0]
                if lst:
                    emit(bgnames[k], lst, struct.unpack_from('>H', c, 264)[0], tick)
        for v in vehicles:
            lst, off = struct.unpack('>IH', A.peek(v + 0x80, 6))
            if lst:
                emit('obj@%d' % ids.get(lst, -1), lst, off, tick)
        if tick >= TICKS:
            break
        A.frames(1)
    A.say('ge done tick', A.tick())
    A.finish()


def pd():
    import gdbpd as L
    # twin.py always passes --skip-mission-intro; GoldenEye's mission opens on
    # its still and swirls down to Bond, and its lists wait on that
    # (IFCameraIsInIntro, IFCameraIsInBondSwirl), so ours plays its opening
    # too unless GF_AI_INTRO=0: gecinemaStageStart() read the flag, and the
    # pending opening is put back as it returns.
    intro = os.environ.get('GF_AI_INTRO', '1') == '1'
    want = {'lvReset'} | ({'gecinemaStageStart'} if intro else set())
    for fn in sorted(want):
        gdb.execute('break ' + fn)
    gdb.execute('run')
    L._st['started'] = True
    while want:
        fn = gdb.selected_frame().name()
        if fn == 'lvReset':
            gdb.execute('set variable g_Difficulty = %d' % int(os.environ.get('GF_DIFF', '0')))
            # as gdbpd.boot(): a mission started from GE Plus's folder has this
            gdb.execute("set variable 'gexfront.c'::g_FrontInside = 1")
            # the oracle is the US cartridge; ours plays the later revision's
            # setups (Mod.GePlusRevisionFixes, on by default) unless told not to
            gdb.execute("set variable 'gexfront.c'::g_GePlusRevisionFixes = %d" % int(os.environ.get('GF_AI_REVISION', '0')))
        elif fn == 'gecinemaStageStart':
            gdb.execute('finish')
            gdb.execute("set variable 'gecinema.c'::g_GeIntroPending = 1")
        want.discard(fn)
        if want:
            gdb.execute('continue')
    gdb.execute('delete')
    gdb.execute('break videoEndFrame if g_Vars.lvframenum >= 1')
    gdb.execute('continue')
    gdb.execute('delete')
    L.say('pd boot stage', hex(int(L.ev('g_Vars.stagenum'))), 'difficulty', int(L.ev('g_Difficulty')),
          'intro', int(L.ev("'gecinema.c'::g_GeIntroPending")) or int(L.ev("'gecinema.c'::g_GeIntroStage")))
    gdb.execute('set variable g_Vars.currentplayer->invincible = 1')
    if INVISIBLE:
        gdb.execute('set variable g_Vars.bondvisible = 0')

        # the mission's opening hands control back with bondvisible = true
        # (gecinema.c), which GoldenEye's cheat flag never sees: held at 0
        class KeepUnseen(gdb.Breakpoint):
            def stop(self):
                if int(L.ev('g_Vars.bondvisible')):
                    gdb.execute('set variable g_Vars.bondvisible = 0')
                return False

        KeepUnseen('g_Vars.bondvisible', gdb.BP_WATCHPOINT, gdb.WP_WRITE, internal=True)
    # GoldenEye's pad script (solo-quiet) dismisses the opening still with Z
    # at frame 1300, about tick 190 of the level; ours is let time out, which
    # is later, and lists that test what Bond holds or where the camera is
    # read the two openings' lengths as divergences. Dismissed at the same
    # tick as a press would (gecinemaIntroTick: the still ends once its timer
    # passes the shot's end).
    still = int(os.environ.get('GF_AI_STILL_TICK', '190'))
    if intro and still > 0:
        L.until_tick(still)
        if int(L.ev("'gecinema.c'::g_GeIntroStage")) == 1:
            gdb.execute("set variable 'gecinema.c'::g_GeIntroTimer = 100000")
            L.say('pd still dismissed at tick', L.tick())
    L.until_tick(TICKS)
    gdb.execute('call (int)fflush(0)')
    L.say('pd done tick', L.tick())
    L.finish()


try:
    if SIDE == 'ge':
        ge_ares() if os.environ.get('GF_ORACLE') == 'ares' else ge()
    else:
        pd()
except Exception:
    import traceback
    traceback.print_exc()
    print('GF FAILED', flush=True)
    if os.environ.get('GF_ORACLE') == 'ares' and SIDE == 'ge':
        sys.exit(1)
    gdb.execute('quit')

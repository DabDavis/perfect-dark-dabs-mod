"""A mission's ending on the cartridge, its cutscene camera pictured and what
it looks at logged - where a converted ending's framing is in question.
Ares only:

    twin.py ge view/outrocam_ares.py --oracle ares --render --mission surface \
        --env GF_GAME=tnd --env GF_LEVELID=36 --env GF_LIST=0x1001 \
        --env GF_FRAMES=30,240,720,840

The level's background list GF_LIST (GoldenEye's id, the same as ours) is
sent to its first HideAllChrs (the start of every ending), or to GF_OFF when
given: the list's runner is a chr record with no prop and no slot in
g_ChrSlots, found by the list's address in RDRAM (the setup's list table
aside). At each of GF_FRAMES after that the run logs Bond's prop, his eye
(field_488.pos) and the smoothed point a look-at-Bond shot aims at
(field_3C4..3CC, bondview2.c), and takes a picture OUT/end_<frame>.ppm. Ours:
--cinema-ending. GoldenEye's command lengths come from
common/geaitable_lengths.txt, port/include/geaitable.h's first column (the
toolkit goes to the oracle host without the port's tree). TND64 Press, 2026-10-10:
the shots aim at y 157-163 with Bond's root at 105-111 (F3 20261006-034615)."""
import os, sys, struct, re, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
import aresge  # noqa: E402

OUT = os.environ['GF_OUT']
LIST = int(os.environ.get('GF_LIST', '0x1001'), 0)
FRAMES = [int(x) for x in os.environ.get('GF_FRAMES', '60,300,540,800').split(',')]


def ge_lengths():
    """GoldenEye's command lengths, from the port's own table (geaitable.h's first column)."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(here, 'common', 'geaitable_lengths.txt')
    lens = {}
    for line in open(path):
        w = line.split()
        if len(w) == 2:
            lens[int(w[0], 16)] = int(w[1])
    return lens


try:
    lv = os.environ['GF_LEVELID']
    lib.boot(int(lv) if lv.isdigit() else lv, int(os.environ.get('GF_DIFF', '0')))
    lib.until_play()
    lib.until_tick(max(lib.tick(), int(os.environ.get('GF_AT', '120'))))
    ids = aresge._ailist_ids()
    ptr = [p for p, i in ids.items() if i == LIST]
    if not ptr:
        raise RuntimeError('no list 0x%x: %s' % (LIST, sorted(hex(i) for i in ids.values())))
    ptr = ptr[0]
    lens = ge_lengths()
    data = lib.peek(ptr, 4096)
    off = int(os.environ['GF_OFF'], 0) if os.environ.get('GF_OFF') else None
    k = 0
    while off is None and k < len(data):
        op = data[k]
        if op == 0xdd:      # HideAllChrs
            off = k
            break
        if op == 0x04:      # EndList
            break
        k += lens.get(op, 0) or 1
    if off is None:
        raise RuntimeError('no HideAllChrs in list 0x%x' % LIST)
    lib.say('list 0x%x at 0x%08x, the ending from +%d: %s' % (LIST, ptr, off, data[off:off + 48].hex()))
    # A background list (0x1000 on) is run by a chr record with no prop that
    # is in no slot of g_ChrSlots: found by the list's address in RDRAM
    # (the setup's own list table aside)
    F = lib.T['ChrRecord']['fields']
    table = aresge.Rec('stagesetup', lib.SYM['g_CurrentSetup']).ptr('ailists')
    ram = b''.join(lib.peek(base, 0x10000) for base in range(0x80000000, 0x80800000, 0x10000))

    def runner_of(p):
        pat = struct.pack('>I', p)
        k = ram.find(pat)
        while k >= 0:
            at = 0x80000000 + k
            if k % 4 == 0 and not (table <= at < table + 8 * 512):
                return at - F['ailist']['off']
            k = ram.find(pat, k + 1)
        return None

    addr = runner_of(ptr)
    if addr is None:
        # a list nothing runs yet (an ending a chr is handed when the
        # objectives are done): it goes on the runner of a background list
        # (0x1000 on), which HideAllChrs does not stop as it stops a chr
        for p, i in sorted(ids.items(), key=lambda e: e[1]):
            if i >= 0x1000 and runner_of(p) is not None:
                addr = runner_of(p)
                lib.say('list 0x%x has no runner: borrowing list 0x%x\'s' % (LIST, i))
                break
    if addr is None:
        raise RuntimeError('nothing runs list 0x%x' % LIST)
    c = aresge.Rec('ChrRecord', addr)
    c = {'chrnum': c['chrnum'], 'slot': hex(addr), 'ailist': hex(LIST), 'aioffset': c['aioffset']}
    lib.poke(addr + F['ailist']['off'], struct.pack('>I', ptr))
    lib.poke(addr + F['aioffset']['off'], struct.pack('>H' if F['aioffset']['size'] == 2 else '>I', off))
    lib.poke(addr + F['sleep']['off'], bytes(F['sleep']['size']))
    lib.say('runner chr', c['chrnum'], 'at', c['slot'], 'on list', c['ailist'], 'offset was', c['aioffset'])
    t0 = lib.tick()
    P = 'struct player'
    for fr in FRAMES:
        lib.until_tick(t0 + fr)
        pl = aresge.Rec(P, aresge._player())
        prop = aresge.Rec('PropRecord', pl.ptr('prop'))
        look = [pl.f('field_3C4'), pl.f('field_3C8'), pl.f('field_3CC')]
        eye = pl.vec('field_488.pos')
        bm = pl.ptr('bodyModel')
        lib.say('end', fr, 'tick', lib.tick(), 'cameramode', pl['cameramode'],
                'prop', [round(v, 1) for v in prop.vec('pos')], 'eye', [round(v, 1) for v in eye],
                'lookat', [round(v, 1) for v in look], 'ground', round(pl.f('field_70'), 1),
                'bodymodel', hex(bm))
        lib.shot(os.path.join(OUT, 'end_%04d.ppm' % fr))
        lib.frames(2)
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()

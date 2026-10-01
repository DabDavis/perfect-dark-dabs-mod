#!/usr/bin/env python3
"""Which of GoldenEye's ROM bytes does the converter never read?

    census.py [--tree SRC] [--rom GE.z64] [--out DIR] [--reuse] [--top N] [--cart CART.json]

Builds port/src/geconvert.c - the only converter (the Python twin was retired
on 2026-10-01) - from SRC (default: this tree) with every load it makes
instrumented (ctrack/: GCC's kernel-address instrumentation calling shim.c,
geconvert.c itself untouched; its output is byte for byte the plain build's),
converts the ROM, and walks every file it read the way GoldenEye's own loaders
walk them - setup sections and propdefs by type, pads, waypoints, intro
records, AI commands, stan tiles and points, bg rooms, portals and vis
commands, the rooms' vertices and display lists, model nodes and their rodata,
the data segment's tables, animation headers - and reports, per record kind
and per GoldenEye field (named from the decomp's own DWARF, fields.json), the
bytes that are NON-ZERO in the ROM and that the converter never read. Sorted by
how many such bytes each field drops.

A field read only in part (a byte of a word) is reported as partial: that is
the shape of "a one-byte render mode read as a word".

With --cart (ares/run.py join's cart.json) each row also says whether
GoldenEye on the cartridge reads the field, and who.

Outputs: report.md and report.json beside this script (or --report-dir), and
the conversion, the instrumented build and the read maps under --out (default
~/wt/gefidelity-run/census-out).

**The null, every run.** Dam's mission setup gets a second read map in which
every pad's position (PadRecord.pos, the first 12 bytes of each pad - a field
the converter certainly reads) is hidden from the tracker. The census must
find (1) in the real map, every pad's position read, and (2) in the hidden
map, every non-zero pad position reported dropped. If either fails the
instrument cannot tell read from unread and the run exits 4 with no report.
"""
import collections, json, os, struct, subprocess, sys, time
sys.dont_write_bytecode = True      # no __pycache__ here or in tools/geconvert

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
CONV = os.path.join(TREE, 'tools', 'geconvert')
sys.path.insert(0, HERE)
import romlayout

FIELDS = json.load(open(os.path.join(HERE, 'fields.json')))

# port/shim/setup_bswap.c portSetupPropdef: PROPDEF type -> (name, words, struct).
# The word counts are sizepropdef's (loadobjectmodel.c), the walk's own stride.
PROPDEFS = {
    1: ('DOOR', 64, 'DoorRecord'), 2: ('DOOR_SCALE', 2, 'GlobalDoorScaleRecord'),
    3: ('PROP', 32, 'ObjectRecord'), 4: ('KEY', 33, 'KeyRecord'), 5: ('ALARM', 32, 'ObjectRecord'),
    6: ('CCTV', 59, 'CCTVRecord'), 7: ('MAGAZINE', 33, 'AmmoCrateRecord'),
    8: ('COLLECTABLE', 34, 'WeaponObjRecord'), 9: ('GUARD', 7, 'GuardRecord'),
    10: ('MONITOR', 64, 'MonitorObjRecord'), 11: ('MULTI_MONITOR', 149, 'MultiMonitorObjRecord'),
    12: ('RACK', 32, 'ObjectRecord'), 13: ('AUTOGUN', 54, 'AutogunRecord'), 14: ('LINK', 3, 'LinkRecord'),
    17: ('HAT', 32, 'HatRecord'), 18: ('GUARD_ATTRIBUTE', 3, 'GuardAttributeRecord'),
    19: ('SWITCH', 4, 'LinkRecord'), 20: ('AMMO', 45, 'MultiAmmoCrateRecord'),
    21: ('ARMOUR', 34, 'BodyArmourRecord'), 22: ('TAG', 4, 'TagObjectRecord'),
    23: ('OBJECTIVE_START', 4, 'MissionObjectiveRecord'), 24: ('OBJECTIVE_END', 1, 'MissionObjectiveRecord'),
    25: ('OBJECTIVE_DESTROY_OBJECT', 2, 'MissionObjectiveRecord'),
    26: ('OBJECTIVE_COMPLETE_CONDITION', 2, 'MissionObjectiveRecord'),
    27: ('OBJECTIVE_FAIL_CONDITION', 2, 'MissionObjectiveRecord'),
    28: ('OBJECTIVE_COLLECT_OBJECT', 2, 'MissionObjectiveRecord'),
    29: ('OBJECTIVE_DEPOSIT_OBJECT', 2, 'MissionObjectiveRecord'),
    30: ('OBJECTIVE_PHOTOGRAPH', 4, 'MissionObjectiveRecord'), 31: ('OBJECTIVE_NULL', 1, 'MissionObjectiveRecord'),
    32: ('OBJECTIVE_ENTER_ROOM', 4, 'MissionObjectiveRecord'),
    33: ('OBJECTIVE_DEPOSIT_OBJECT_IN_ROOM', 5, 'MissionObjectiveRecord'),
    34: ('OBJECTIVE_COPY_ITEM', 1, 'MissionObjectiveRecord'),
    35: ('WATCH_MENU_OBJECTIVE_TEXT', 4, 'struct watchMenuObjectiveText'),
    36: ('GAS_RELEASING', 32, 'ObjectRecord'), 37: ('RENAME', 10, 'RenameObjectRecord'),
    38: ('LOCK_DOOR', 4, 'LockDoorRecord'), 39: ('VEHICHLE', 44, 'VehichleRecord'),
    40: ('AIRCRAFT', 45, 'AircraftRecord'), 42: ('GLASS', 32, 'GlassRecord'), 43: ('SAFE', 32, 'SafeRecord'),
    44: ('SAFE_ITEM', 5, 'SafeObjectRecord'), 45: ('TANK', 56, 'TankRecord'), 46: ('CAMERAPOS', 7, 'CutsceneRecord'),
    47: ('TINTED_GLASS', 37, 'TintedGlassRecord'),
}
PROPDEF_END = 48
# port/shim/setup_bswap.c portSetupIntro: INTROTYPE -> (name, words, struct)
INTROS = {0: ('SPAWN', 3, 'SetupIntroSpawn'), 1: ('ITEM', 4, 'struct SetupIntroItem'),
          2: ('AMMO', 4, 'struct SetupIntroAmmo'), 3: ('SWIRL', 8, 'struct SetupIntroSwirl'),
          4: ('ANIM', 2, 'struct SetupIntroAnim'), 5: ('CUFF', 2, 'struct SetupIntroCuff'),
          6: ('CAMERA', 10, 'SetupIntroCamera'), 7: ('WATCH', 3, 'struct SetupIntroWatch'),
          8: ('CREDITS', 2, 'struct SetupIntroCredits')}
INTRO_END = 9
# bondconstants.h MODELNODE_OPCODE -> rodata struct
NODES = {1: 'ModelRoData_HeaderRecord', 2: 'ModelRoData_GroupRecord', 4: 'ModelRoData_DisplayListRecord',
         5: 'ModelRoData_Op05Record', 6: 'ModelRoData_Op06Record', 7: 'ModelRoData_Op07Record',
         8: 'ModelRoData_LODRecord', 9: 'ModelRoData_BSPRecord', 10: 'ModelRoData_BoundingBoxRecord',
         11: 'ModelRoData_Op11Record', 12: 'ModelRoData_GunfireRecord', 13: 'ModelRoData_ShadowRecord',
         14: 'ModelRoData_Op14Record', 15: 'ModelRoData_InterlinkageRecord', 16: 'ModelNode_Op16Record',
         17: 'ModelRoData_Op17Record', 18: 'ModelRoData_SwitchRecord', 21: 'ModelRoData_GroupSimpleRecord',
         22: 'ModelRoData_DisplayListPrimaryRecord', 23: 'ModelRoData_HeadPlaceholderRecord',
         24: 'ModelRoData_DisplayList_CollisionRecord'}
NODE_NAMES = {0: 'NULL', 1: 'HEADER', 2: 'GROUP', 3: 'OP03', 4: 'DL', 5: 'OP05', 6: 'OP06', 7: 'OP07', 8: 'LOD',
              9: 'BSP', 10: 'BBOX', 11: 'OP11', 12: 'GUNFIRE', 13: 'SHADOW', 14: 'OP14', 15: 'INTERLINK',
              16: 'OP16', 17: 'OP17', 18: 'SWITCH', 19: 'OP19', 20: 'OP20', 21: 'GROUPSIMPLE',
              22: 'DLPRIMARY', 23: 'HEAD', 24: 'DLCOLLISION'}
# Fast3D (GoldenEye's microcode, with its own TRI4) opcode names
GBI = {0x01: 'G_MTX', 0x03: 'G_MOVEMEM', 0x04: 'G_VTX', 0x06: 'G_DL', 0x07: 'G_DL_SEG7', 0xb1: 'G_TRI4',
       0xb2: 'G_RDPHALF_CONT', 0xb3: 'G_RDPHALF_2', 0xb4: 'G_RDPHALF_1', 0xb5: 'G_LINE3D',
       0xb6: 'G_CLEARGEOMETRYMODE', 0xb7: 'G_SETGEOMETRYMODE', 0xb8: 'G_ENDDL', 0xb9: 'G_SETOTHERMODE_L',
       0xba: 'G_SETOTHERMODE_H', 0xbb: 'G_TEXTURE', 0xbc: 'G_MOVEWORD', 0xbd: 'G_POPMTX', 0xbe: 'G_CULLDL',
       0xbf: 'G_TRI1', 0xc0: 'G_NOOP', 0xe4: 'G_TEXRECT', 0xe5: 'G_TEXRECTFLIP', 0xe6: 'G_RDPLOADSYNC',
       0xe7: 'G_RDPPIPESYNC', 0xe8: 'G_RDPTILESYNC', 0xe9: 'G_RDPFULLSYNC', 0xf0: 'G_LOADTLUT',
       0xf2: 'G_SETTILESIZE', 0xf3: 'G_LOADBLOCK', 0xf4: 'G_LOADTILE', 0xf5: 'G_SETTILE', 0xf6: 'G_FILLRECT',
       0xf7: 'G_SETFILLCOLOR', 0xf8: 'G_SETFOGCOLOR', 0xf9: 'G_SETBLENDCOLOR', 0xfa: 'G_SETPRIMCOLOR',
       0xfb: 'G_SETENVCOLOR', 0xfc: 'G_SETCOMBINE', 0xfd: 'G_SETTIMG', 0xfe: 'G_SETZIMG', 0xff: 'G_SETCIMG'}
SEG = 0x0f000000
MSEG = 0x05000000
# display list nodes: (offsets of their list pointers, (vertices ptr, count) offsets, count format)
DLNODES = {4: ((0, 4), (12, 16), '>H'), 22: ((8,), (4, 0), '>i'), 24: ((0, 4), (8, 12), '>H')}


class Root:
    """One buffer the converter read: its bytes and a read map (0/1), how each
    byte was read (1 parsed, 2 copied whole) and the null's second map."""
    __slots__ = ('key', 'data', 'map', 'how', 'shadow', 'mask')

    def __init__(self, key, data):
        self.key, self.data = key, bytes(data)
        self.map = bytearray(len(self.data))
        self.how = bytearray(len(self.data))
        self.shadow = self.mask = None


# --------------------------------------------------------------- field layouts

def clean(path):
    return '.'.join(p for p in path.split('.') if p != '<anon>') or path


_LAYOUTS = {}


def layout(struct_name, length):
    """[(offset, size, name)] covering `length` bytes: the struct's fields,
    then anything past the struct as words."""
    key = (struct_name, length)
    if key in _LAYOUTS:
        return _LAYOUTS[key]
    t = FIELDS.get(struct_name) if struct_name else None
    out = []
    covered = 0
    size = 0
    if t and 'leaves' in t:
        size = t['size']
        bystart = collections.defaultdict(list)
        for off, sz, path, ctype in t['leaves']:
            bystart[off].append((sz, clean(path)))
        for off in sorted(bystart):
            if off < covered or off >= length:
                continue
            cands = bystart[off]
            big = max(sz for sz, _ in cands)
            names = []
            for sz, n in cands:
                if sz == big and n not in names:
                    names.append(n)
            if covered < off:
                out.append((covered, off - covered, '(gap +%#x)' % covered))
            sz = min(big, length - off)
            out.append((off, sz, '|'.join(names[:3])))
            covered = off + sz
        if covered < min(size, length):
            out.append((covered, min(size, length) - covered, '(gap +%#x)' % covered))
            covered = min(size, length)
    o = covered
    while o < length:
        out.append((o, min(4, length - o), '+%#x (past the struct)' % o if t else '+%#x' % o))
        o += 4
    _LAYOUTS[key] = out
    return out


def byte_layout(length, first='opcode'):
    return [(0, 1, first)] + [(i, 1, 'b%d' % i) for i in range(1, length)]


# --------------------------------------------------------------- aggregation

class Census:
    def __init__(self, pcs=None, recids=None, cartrecs=None):
        self.pcs = pcs          # root key -> (first, last) reader PCs per byte (the cartridge's maps)
        self.recids = recids    # set of 'kind|field' whose read records to list (the join)
        self.cartrecs = cartrecs  # 'kind|field' -> set of 'stem@offset' the cartridge read (the census)
        # kind -> field name -> stats
        self.k = collections.defaultdict(lambda: collections.defaultdict(lambda: dict(
            n=0, nonzero=0, dropped=0, partial=0, dropped_bytes=0, files=set(), values=[], where=[], off=None,
            size=None, blind=0, blind_files=set(), blind_values=[], rd_any=0, rd_nz=0, rd_files=set(), pcs=[],
            byfile=collections.Counter(), partfile=collections.Counter(), bytesfile=collections.Counter(),
            rd_recs=set(), drop_cart=0, drop_cartfile=collections.Counter())))
        self.records = collections.Counter()
        self.claimed = {}       # root key -> bytearray of claimed bytes

    def claim(self, r, a, b):
        c = self.claimed.setdefault(r.key, bytearray(len(r.data)))
        a, b = max(a, 0), min(b, len(r.data))
        if b > a:
            c[a:b] = b'\x01' * (b - a)

    def record(self, kind, r, rmap, at, length, fields, label, how=None):
        """One record of `kind` at `at` in root `r` (read map `rmap`)."""
        how = r.how if how is None else how
        length = max(0, min(length, len(r.data) - at))
        if length <= 0:
            return
        self.records[kind] += 1
        self.claim(r, at, at + length)
        data = r.data
        for off, size, name in fields:
            if off >= length:
                break
            size = min(size, length - off)
            a = at + off
            vals = data[a:a + size]
            st = self.k[kind][name]
            st['n'] += 1
            if st['off'] is None:
                st['off'], st['size'] = off, size
            rd = rmap[a:a + size]
            anyrd = any(rd)
            if anyrd:
                st['rd_any'] += 1
                st['rd_files'].add(r.key.split('#')[1] if '#' in r.key else label)
                if self.recids is not None and '%s|%s' % (kind, name) in self.recids:
                    st['rd_recs'].add('%s@%x' % (label, at))
                if self.pcs and r.key in self.pcs and len(st['pcs']) < 400:
                    first, last = self.pcs[r.key]
                    for i in range(size):
                        if rd[i]:
                            st['pcs'] += [first[a + i], last[a + i]]
            if not any(vals):
                continue
            st['nonzero'] += 1
            if anyrd:
                st['rd_nz'] += 1
            drop = sum(1 for i in range(size) if vals[i] and not rd[i])
            hw = how[a:a + size] if how is not None else None
            if hw is not None and not drop and not any(x & 1 for x in hw):
                # every non-zero byte reached the output, none was looked at
                st['blind'] += 1
                st['blind_files'].add(label)
                h = vals.hex()
                if h not in st['blind_values'] and len(st['blind_values']) < 6:
                    st['blind_values'].append(h)
            if drop:
                st['dropped'] += 1
                st['dropped_bytes'] += drop
                if self.cartrecs and '%s@%x' % (label, at) in self.cartrecs.get('%s|%s' % (kind, name), ()):
                    st['drop_cart'] += 1
                    st['drop_cartfile'][label] += 1
                st['byfile'][label] += 1
                st['bytesfile'][label] += drop
                if any(rd):
                    st['partial'] += 1
                    st['partfile'][label] += 1
                st['files'].add(label)
                h = vals.hex()
                if h not in st['values'] and len(st['values']) < 6:
                    st['values'].append(h)
                if len(st['where']) < 3:
                    st['where'].append('%s+%#x' % (label, a))


# --------------------------------------------------------------- segmenters

def u32(d, o):
    return struct.unpack_from('>I', d, o)[0] if o + 4 <= len(d) else 0


def s32(d, o):
    return struct.unpack_from('>i', d, o)[0] if o + 4 <= len(d) else -1


def seg_setup(c, r, rmap, label, aitable):
    d = r.data
    if len(d) < 40:
        return
    h = struct.unpack_from('>10I', d, 0)
    c.record('setup header', r, rmap, 0, 40, layout('stagesetupfile', 40), label)
    lists = set()
    # pads (prop.c:1353: until plink == 0), bound pads
    o = h[6]
    while h[6] and o + 0x2c <= len(d) and u32(d, o + 36):
        c.record('setup pad', r, rmap, o, 0x2c, layout('PadRecord', 0x2c), label)
        o += 0x2c
    c.claim(r, o, o + 0x2c)
    o = h[7]
    while h[7] and o + 0x44 <= len(d) and u32(d, o + 36):
        c.record('setup boundpad', r, rmap, o, 0x44, layout('BoundPadRecord', 0x44), label)
        o += 0x44
    # waypoints and groups, and the -1 lists they point at
    o = h[0]
    while h[0] and o + 16 <= len(d) and s32(d, o) >= 0:
        c.record('setup waypoint', r, rmap, o, 16, layout('waypoint', 16), label)
        lists.add(u32(d, o + 4))
        o += 16
    c.claim(r, o, o + 16)
    o = h[1]
    while h[1] and o + 12 <= len(d) and u32(d, o):
        c.record('setup waygroup', r, rmap, o, 12, layout('waygroup', 12), label)
        lists.add(u32(d, o))
        lists.add(u32(d, o + 4))
        o += 12
    c.claim(r, o, o + 12)
    # patrol paths
    o = h[4]
    while h[4] and o + 8 <= len(d) and u32(d, o):
        c.record('setup path', r, rmap, o, 8, layout('PathRecord', 8), label)
        lists.add(u32(d, o))
        o += 8
    c.claim(r, o, o + 8)
    for at in lists:
        o = at
        while 0 < o and o + 4 <= len(d) and s32(d, o) != -1:
            c.record('setup id list entry', r, rmap, o, 4, [(0, 4, 'id')], label)
            o += 4
        c.claim(r, o, o + 4)
    # intro records
    o = h[2]
    while h[2] and o + 4 <= len(d):
        t = u32(d, o) & 0xff
        if t == INTRO_END:
            c.claim(r, o, o + 4)
            break
        if t not in INTROS:
            break
        name, words, sname = INTROS[t]
        c.record('setup intro %s' % name, r, rmap, o, 4 * words, layout(sname, 4 * words), label)
        o += 4 * words
    # propdefs
    o = h[3]
    while h[3] and o + 4 <= len(d):
        t = d[o + 3]
        if t == PROPDEF_END:
            c.claim(r, o, o + 4)
            break
        if t not in PROPDEFS:
            print('census: %s: propdef type %d at %#x, walk stopped' % (label, t, o))
            break
        name, words, sname = PROPDEFS[t]
        c.record('propdef %s' % name, r, rmap, o, 4 * words, layout(sname, 4 * words), label)
        o += 4 * words
    # AI lists
    o = h[5]
    while h[5] and o + 8 <= len(d):
        ptr, lid = struct.unpack_from('>Ii', d, o)
        if not ptr and not lid:
            c.claim(r, o, o + 8)
            break
        c.record('setup ailist row', r, rmap, o, 8, layout('AIListRecord', 8), label)
        seg_ai(c, r, rmap, ptr, label, aitable, 'ai')
        o += 8
    # pad names: u32 pointers to strings, until 0
    for hi in (8, 9):
        o = h[hi]
        while h[hi] and o + 4 <= len(d) and u32(d, o):
            p = u32(d, o)
            c.record('setup padname ptr', r, rmap, o, 4, [(0, 4, 'name')], label)
            if 0 < p < len(d):
                e = d.find(b'\0', p)
                c.claim(r, p, e + 1 if e >= 0 else p)
            o += 4


def seg_ai(c, r, rmap, at, label, aitable, prefix):
    d = r.data
    n = 0
    while 0 <= at < len(d) and n < 20000:
        op = d[at]
        if op >= len(aitable):
            break
        name, ln, args = aitable[op][0], aitable[op][1], aitable[op][2]
        if ln is None:
            e = d.find(b'\0', at + 1)
            ln = (e - at + 1) if e >= 0 else 1
            fields = [(0, 1, 'opcode'), (1, ln - 1, 'text')]
        else:
            fields = [(0, 1, 'opcode')]
            o = 1
            for a, w in args:
                fields.append((o, w, a))
                o += w
            while o < ln:
                fields.append((o, 1, '+%d (no argument)' % o))
                o += 1
        c.record('%s %s' % (prefix, name), r, rmap, at, ln, fields, label)
        at += ln
        n += 1
        if name == 'EndList':
            break


def seg_stan(c, r, rmap, label):
    d = r.data
    c.record('stan header', r, rmap, 0, 8, layout('StandFileHeader', 8), label)
    first = u32(d, 4)
    o = first
    tile = [(0, 3, 'id:24'), (3, 1, 'room'), (4, 2, 'mid (special:4 r:4 g:4 b:4)'),
            (6, 2, 'tail (pointCount:4 headerC:4 headerD:4 headerE:4)')]
    while o + 8 <= len(d):
        if d[o:o + 8] == bytes(8):
            c.claim(r, o, o + 8)
            o += 8
            break
        npts = struct.unpack_from('>H', d, o + 6)[0] >> 12
        c.record('stan tile', r, rmap, o, 8, tile, label)
        for k in range(npts):
            c.record('stan point', r, rmap, o + 8 + 8 * k, 8, layout('StandFilePoint', 8), label)
        o += 8 + 8 * npts
    if o + 24 <= len(d):
        c.record('stan footer', r, rmap, o, len(d) - o, layout('StandFileFooter', len(d) - o), label)


def seg_bg(c, r, rmap, label, inflated):
    d = r.data
    c.record('bg header', r, rmap, 0, 16, [(0, 4, 'word0'), (4, 4, 'rooms'), (8, 4, 'portals'),
                                           (12, 4, 'globalvis')], label)
    rooms_at, portals_at, vis_at = u32(d, 4) & 0xffffff, u32(d, 8) & 0xffffff, u32(d, 12) & 0xffffff
    entries = []
    i = 0
    while rooms_at and rooms_at + 24 * i + 24 <= len(d):
        o = rooms_at + 24 * i
        pt, pri, sec = u32(d, o), u32(d, o + 4), u32(d, o + 8)
        c.record('bg room row', r, rmap, o, 24, layout('bg_room_data', 24), label)
        entries.append((pt, pri, sec))
        if i > 0 and pri == 0:
            break
        i += 1
    # the rooms' inflated blobs: vertices and the two display lists
    for n, (pt, pri, sec) in enumerate(entries):
        for what, ptr in (('vtx', pt), ('pri', pri), ('sec', sec)):
            if not ptr:
                continue
            key = 'inflated:%s@%#x' % (r.key, ptr & 0xffffff)
            ir = inflated.get(key)
            if ir is None:
                continue
            imap = ir.map
            if what == 'vtx':
                vl = layout('Vtx_t', 16)
                for k in range(len(ir.data) // 16):
                    c.record('room vertex', ir, imap, 16 * k, 16, vl, label)
            else:
                seg_gdl(c, ir, imap, 0, label, 'room %s gdl' % what)
    o = portals_at
    while portals_at and o + 8 <= len(d) and u32(d, o):
        c.record('bg portal row', r, rmap, o, 8, layout('bg_portal_data_entry', 8), label)
        p = u32(d, o) & 0xffffff
        if p + 4 <= len(d):
            npts = d[p]
            flds = [(0, 1, 'numPoints'), (1, 3, 'padding')] + [
                (4 + 12 * k + 4 * j, 4, 'point[%d].%s' % (k, 'xyz'[j])) for k in range(npts) for j in range(3)]
            c.record('bg portal points', r, rmap, p, 4 + 12 * npts, flds, label)
        o += 8
    c.claim(r, o, o + 8)
    o = vis_at
    while vis_at and o + 8 <= len(d):
        t, ln = d[o], d[o + 1]
        arg = s32(d, o + 4)
        c.record('bg vis command', r, rmap, o, 8, [(0, 1, 'type'), (1, 1, 'len'), (2, 2, 'pad'),
                                                   (4, 4, 'arg')], label)
        o += 8
        if t == 0 and ln == 0 and arg == 0:
            break


def seg_gdl(c, r, rmap, at, label, prefix, limit=200000):
    d = r.data
    n = 0
    while at + 8 <= len(d) and n < limit:
        op = d[at]
        c.record('%s %s' % (prefix, GBI.get(op, 'op%02x' % op)), r, rmap, at, 8, byte_layout(8), label)
        at += 8
        n += 1
        if op == 0xb8:
            break


def seg_model(c, r, rmap, label, numswitches, numtextures):
    d = r.data
    for i in range(numswitches):
        c.record('model switch ptr', r, rmap, 4 * i, 4, [(0, 4, 'node')], label)
    textab = 4 * numswitches
    for i in range(numtextures):
        c.record('model texture row', r, rmap, textab + 12 * i, 12,
                 layout('ModelFileTextures', 12), label)
    root = textab + 12 * numtextures
    seen = set()
    stack = [root]
    while stack:
        o = stack.pop()
        while o and o not in seen and o + 24 <= len(d):
            seen.add(o)
            t = struct.unpack_from('>H', d, o)[0]
            c.record('model node', r, rmap, o, 24, layout('ModelNode', 24), label)
            op = t & 0xff
            ro = u32(d, o + 4)
            if ro and NODES.get(op):
                ro -= MSEG
                sname = NODES[op]
                size = FIELDS.get(sname, {}).get('size', 4)
                if 0 <= ro < len(d):
                    c.record('model rodata %s' % NODE_NAMES.get(op, op), r, rmap, ro, size,
                             layout(sname, size), label)
                    if op in DLNODES:
                        # display lists (each until G_ENDDL) and the vertices they load
                        lists, vtxat, vtxfmt = DLNODES[op]
                        for fo in lists:
                            p = u32(d, ro + fo)
                            if p and MSEG <= p < MSEG + len(d):
                                seg_gdl(c, r, rmap, p - MSEG, label, 'model gdl', 20000)
                        nv = struct.unpack_from(vtxfmt, d, ro + vtxat[1])[0]
                        v = u32(d, ro + vtxat[0])
                        vl = layout('Vtx_t', 16)
                        if v and MSEG <= v < MSEG + len(d) and 0 < nv < 65536:
                            for k in range(nv):
                                c.record('model vertex', r, rmap, v - MSEG + 16 * k, 16, vl, label)
                        if op == 24:
                            ncv = struct.unpack_from('>H', d, ro + 14)[0]
                            cv = u32(d, ro + 16)
                            if cv and MSEG <= cv < MSEG + len(d):
                                for k in range(ncv):
                                    c.record('model collision vertex', r, rmap, cv - MSEG + 16 * k, 16, vl, label)
            child = u32(d, o + 20)
            nxt = u32(d, o + 12)
            if child:
                stack.append(child - MSEG)
            o = (nxt - MSEG) if nxt else 0


# --------------------------------------------------------------- running it

ROM_DEFAULT = os.path.expanduser('~/wt/gefidelity-run/added-content/GoldenEye 007 (U) [!].n64')
NONZERO = bytes(1 if x else 0 for x in range(256))
NULL_STEM = 'UsetupdamZ'


def null_spec(rom):
    """GECENSUS_NULL for the shim: Dam's setup stream and every pad's position."""
    addr, _ = rom.files[NULL_STEM]
    d = rom.file(NULL_STEM)
    o = struct.unpack_from('>I', d, 24)[0]
    ranges = []
    while o + 0x2c <= len(d) and struct.unpack_from('>I', d, o + 36)[0]:
        ranges.append('%d-%d' % (o, o + 12))
        o += 0x2c
    return '%d:%s' % (addr + 2, ','.join(ranges))


def run_c(tree, romfile, out):
    """Build the instrumented converter from `tree`, convert, dump the maps."""
    rom = romlayout.Rom(romfile)
    binp = os.path.join(out, 'bin', 'gecensus')
    subprocess.run([os.path.join(HERE, 'ctrack', 'build.sh'), tree, binp], check=True,
                   stdout=subprocess.DEVNULL)
    dump = os.path.join(out, 'dump')
    conv = os.path.join(out, 'conv')
    for d in (dump, conv):
        subprocess.run(['rm', '-rf', d], check=True)
        os.makedirs(d)
    t0 = time.time()
    env = dict(os.environ, GECENSUS_NULL=null_spec(rom))
    p = subprocess.run([binp, os.path.abspath(romfile), conv, dump], env=env, capture_output=True, text=True)
    if p.returncode:
        raise SystemExit('census: the instrumented converter failed:\n' + p.stderr[-2000:])
    print('census: geconvert.c (instrumented) ran in %.0f s' % (time.time() - t0))
    return rom


def load_c_dump(dumpdir, rom):
    """The shim's streams as the census's roots: the ROM, the data segment,
    each file by its stem, each bg room blob as inflated:file:<bg>@offset."""
    streams = json.load(open(os.path.join(dumpdir, 'streams.json')))
    byaddr = {addr + 2: stem for stem, (addr, size) in rom.files.items() if not stem.startswith('bg_')}
    bgs = sorted((addr, addr + size, stem) for stem, (addr, size) in rom.files.items() if stem.startswith('bg_'))
    roots, skipped = {}, 0

    def mk(key, data, m, shadow=None):
        r = Root(key, data)
        r.map = bytearray(m.translate(NONZERO))
        r.how = bytearray(m)
        if shadow is not None:
            r.shadow = bytearray(shadow.translate(NONZERO))
            r.mask = bytearray(len(data))
        roots[key] = r
        return r

    rommap = None
    for s in streams:
        rd = lambda ext: open(os.path.join(dumpdir, 's%04d.%s' % (s['id'], ext)), 'rb').read()
        if s['key'] == -1:
            rommap = rd('map')
            mk('rom', rom.rom, rommap)
            continue
        if s['src'] != 0:
            skipped += 1
            continue
        k = s['key']
        shadow = rd('shadow') if s['null'] else None
        if k == romlayout.DATA_ROM + 2:
            mk('data', rd('data'), rd('map'))
        elif k in byaddr:
            mk('file:' + byaddr[k], rd('data'), rd('map'), shadow)
        else:
            bg = next((b for b in bgs if b[0] <= k - 2 < b[1]), None)
            if bg:
                mk('inflated:file:%s@%#x' % (bg[2], k - 2 - bg[0]), rd('data'), rd('map'))
            else:
                skipped += 1
    # the bg files are stored, not compressed: the ROM's own bytes and map
    for addr, end, stem in bgs:
        if rommap is not None and any(rommap[addr:end]):
            mk('file:' + stem, rom.rom[addr:end], rommap[addr:end])
    anims = []
    sys.path.insert(0, CONV)
    import geanimtable
    for _, at in list(geanimtable.TABLE) + list(geanimtable.VEHICLES):
        a = geanimtable.BASE + at
        if at > 1 and rommap is not None and any(rommap[a:a + 20]):
            anims.append(a)
    info = dict(props=rom.models(), chrs=[], anims=sorted(set(anims)),
                global_ai_at=romlayout.GLOBAL_AI_AT - romlayout.DATA_VRAM, gerom=romlayout)
    print('census: %d streams, %d roots, %d not placed' % (len(streams), len(roots), skipped))
    return roots, info


def analyse(info, roots, maps=None, only=None, files_only=False, pcs=None, recids=None, cartrecs=None):
    """Census over the roots; `maps` overrides a root's read map (the null).
    `files_only` walks the files alone (no data segment, ROM or unplaced
    bytes) and `pcs` collects each field's reader PCs - the cartridge's join."""
    sys.path.insert(0, CONV)
    import geaitable
    c = Census(pcs, recids, cartrecs)
    gerom = info['gerom']
    inflated = {k: r for k, r in roots.items() if k.startswith('inflated:')}
    models = {name: (ns, nt) for name, ns, nt in info['props'] + info['chrs']}
    for key, r in roots.items():
        if not key.startswith('file:'):
            continue
        name = key[5:].split('#')[0]
        if only and name != only:
            continue
        rmap = (maps or {}).get(key, r.map)
        if name.startswith('Usetup') or name.startswith('Ump_setup'):
            seg_setup(c, r, rmap, name, geaitable.TABLE)
        elif name.endswith('_stanZ'):
            seg_stan(c, r, rmap, name)
        elif name.startswith('bg_') and name.endswith('_all_p'):
            seg_bg(c, r, rmap, name, inflated)
        elif name in models:
            seg_model(c, r, rmap, name, *models[name])
    if only or files_only:
        return c
    # the data segment: the tables the converter reads
    data = roots.get('data')
    if data:
        dm = data.map
        DV = gerom.DATA_VRAM
        for k in range(gerom.NUM_PROPS):
            o = gerom.PROPS_AT + 12 * k
            c.record('data PitemZ row', data, dm, o, 12, layout('ItemModelFileRecord', 12), 'data')
            hdr = u32(data.data, o) - DV
            if 0 <= hdr < len(data.data):
                sz = FIELDS['ModelFileHeader']['size']
                c.record('data model header (prop)', data, dm, hdr, sz, layout('ModelFileHeader', sz), 'data')
        for k in range(gerom.NUM_CHRS):
            o = gerom.CHRS_AT + 20 * k
            c.record('data c_item row', data, dm, o, 20, layout('ChrModelFileRecord', 20), 'data')
            hdr = u32(data.data, o) - DV
            if 0 <= hdr < len(data.data):
                sz = FIELDS['ModelFileHeader']['size']
                c.record('data model header (chr)', data, dm, hdr, sz, layout('ModelFileHeader', sz), 'data')
        o = gerom.FOG_AT
        while o + 92 <= len(data.data):
            lid = u32(data.data, o)
            if (lid == 0 and o > gerom.FOG_AT) or lid >= 0x10000:
                break
            c.record('data fog row', data, dm, o, 92, layout('EnvironmentRecord', 92), 'data')
            o += 92
        for i in range(gerom.LEVELINFO_ROWS):
            c.record('data levelinfo row', data, dm, gerom.LEVELINFO_AT - DV + 24 * i, 24,
                     [(4 * j, 4, 'word%d' % j) for j in range(6)], 'data')
        import geaitable
        at = info['global_ai_at']
        n = 0
        while u32(data.data, at + 8 * n):
            c.record('data global ailist row', data, dm, at + 8 * n, 8, layout('AIListRecord', 8), 'data')
            seg_ai(c, data, dm, u32(data.data, at + 8 * n) - DV, 'data', geaitable.TABLE, 'global ai')
            n += 1
    rom = roots.get('rom')
    if rom:
        hdr = [(0, 4, 'address'), (4, 2, 'numframes (unk04)'), (6, 1, 'width (unk06)'), (7, 1, 'loop (unk07)'),
               (8, 4, 'bitDescriptors'), (12, 2, 'joints (unk0C)'), (14, 2, 'bitsperframe (unk0E)'),
               (16, 4, 'bitStream')]
        for at in info['anims']:
            c.record('anim header', rom, rom.map, at, 20, hdr, 'rom')
            bd = gerom.ANIM_DATA_ROM + (u32(rom.data, at + 8) & 0xffffff)
            for i in range(4):
                c.record('anim root channel', rom, rom.map, bd + 6 * i, 6,
                         layout('ModelAnimRootMotionChannel', 6), 'rom')
    # unclaimed: bytes of a walked file no segmenter placed
    unclaimed = {}
    for key, r in roots.items():
        cl = c.claimed.get(key)
        if cl is None or not key.startswith('file:'):
            continue
        d, m = r.data, r.map
        nz = sum(1 for i in range(len(d)) if d[i] and not cl[i])
        nzu = sum(1 for i in range(len(d)) if d[i] and not cl[i] and not m[i])
        if nzu:
            unclaimed[key[5:]] = dict(nonzero_unplaced=nz, nonzero_unplaced_unread=nzu)
    c.unclaimed = unclaimed
    return c


def null_check(info, roots, nullkey):
    """Proves the instrument on this run: see the module docstring."""
    key = 'file:' + nullkey
    r = roots.get(key)
    if r is None or r.shadow is None:
        return False, 'null: %s was never read, so it could not be armed' % nullkey
    padsat = u32(r.data, 24)
    pads = []
    o = padsat
    while padsat and o + 0x2c <= len(r.data) and u32(r.data, o + 36):
        pads.append(o)
        o += 0x2c
    seen = sum(1 for o in pads if all(r.map[o:o + 12]))
    nonzero = sum(1 for o in pads if any(r.data[o:o + 12]))
    if seen != len(pads):
        return False, 'null: the tracker saw %d of %d pad positions read in %s' % (seen, len(pads), nullkey)
    c = analyse(info, {key: r}, maps={key: r.shadow}, only=nullkey)
    st = c.k.get('setup pad', {})
    dropped = max([v['dropped'] for n, v in st.items() if n.startswith('pos')] or [0])
    real = analyse(info, {key: r}, only=nullkey).k.get('setup pad', {})
    realdrop = max([v['dropped'] for n, v in real.items() if n.startswith('pos')] or [0])
    ok = dropped == nonzero and nonzero > 0 and realdrop == 0
    return ok, ('null: %s, %d pads: the tracker saw all %d positions read; hidden from it, the census reports '
                '%d of the %d non-zero positions dropped (real map: %d)' % (
                    nullkey, len(pads), seen, dropped, nonzero, realdrop))


def cart_records(cart):
    """'kind|field' -> the 'stem@offset' records the cartridge read (cart.json's recs)."""
    if not cart:
        return None
    return {k: set(v.get('recs', ())) for k, v in cart['fields'].items()}


# GoldenEye code that reads a field only to turn a file offset into a pointer
RELOCATORS = {'modelPromoteNodeOffsetsToPointers'}
# ... and where a reader does both, the kinds it only relocates (the setup's
# debug pad-name table: proplvreset2 rebases the pointers, nothing reads a name)
RELOCATORS_BY_KIND = {'setup padname ptr': {'proplvreset2'}}


def cart_says(cart, kind, field, row=None):
    """What the cartridge did with this field: read (in which of GoldenEye's
    functions), never read, or not observed - with a short text for the table."""
    if not cart:
        return dict(state='unchecked', text='-')
    f = cart['fields'].get('%s|%s' % (kind, field))
    if not f or not f['records']:
        return dict(state='unobserved', text='not observed')
    if f['read'] and f['readers'] and all(n in RELOCATORS | RELOCATORS_BY_KIND.get(kind, set())
                                          for n, _ in f['readers']):
        return dict(state='relocated', text='relocated only %d/%d (%s)' % (f['read'], f['records'],
                                                                         f['readers'][0][0]), **f)
    if f['read']:
        who = ', '.join(n for n, _ in f['readers'][:2])
        same = '' if row is None else ('; %d of the dropped records' % row.get('drop_cart', 0))
        return dict(state='read', text='**read** %d/%d (%s)%s' % (f['read'], f['records'], who, same), **f)
    return dict(state='never', text='never (0/%d)' % f['records'], **f)


def report(c, info, nullmsg, outmd, outjson, top, cart=None):
    rows = []
    for kind, fields in c.k.items():
        for name, st in fields.items():
            if st['dropped']:
                rows.append(dict(kind=kind, field=name, offset=st['off'], size=st['size'], records=st['n'],
                                 nonzero=st['nonzero'], dropped=st['dropped'], partial=st['partial'],
                                 dropped_bytes=st['dropped_bytes'], files=sorted(st['files']),
                                 values=st['values'], where=st['where'], byfile=dict(st['byfile']),
                                 partfile=dict(st['partfile']), bytesfile=dict(st['bytesfile']),
                                 drop_cart=st['drop_cart'], drop_cartfile=dict(st['drop_cartfile'])))
    rows.sort(key=lambda x: (-x['dropped_bytes'], x['kind'], x['offset']))
    for x in rows:
        x['cart'] = cart_says(cart, x['kind'], x['field'], x)
    js = dict(null=nullmsg, records=dict(c.records), unclaimed=c.unclaimed, dropped=rows)
    json.dump(js, open(outjson, 'w'), indent=1)
    L = ['# Converter read census', '',
         'Bytes of GoldenEye\'s ROM that are **non-zero and never read** by the converter '
         '(port/src/geconvert.c, every load instrumented: census/ctrack), by record kind and GoldenEye '
         'field (names from the decomp\'s DWARF). Generated by `tools/gefidelity/census/census.py`; '
         'do not edit by hand.', '',
         '- %s' % nullmsg,
         '- %s' % (('cartridge column: ares/run.py join, ' + cart['null'] + '; observed: ' +
                    ', '.join('%d %s' % (v, k) for k, v in sorted(cart['observed'].items())))
                   if cart else 'cartridge column: not run (census/ares/run.py sweep, then join)'),
         '- records walked: %d in %d kinds; fields with dropped data: %d' % (
             sum(c.records.values()), len(c.records), len(rows)),
         '- "partial" = the field was read in part (a byte of a word) and a non-zero byte of it was not',
         '- "kind" is how GoldenEye\'s own loader walks the file; a dropped field may still be used at run time '
         'by GE Plus code reading something else - see README.md before acting on a row', '']
    # by kind summary
    bykind = collections.defaultdict(lambda: [0, 0])
    for x in rows:
        bykind[x['kind']][0] += x['dropped_bytes']
        bykind[x['kind']][1] += 1
    L += ['## By record kind', '', '| kind | records | fields dropping data | non-zero bytes dropped |',
          '|---|---:|---:|---:|']
    for kind, (b, n) in sorted(bykind.items(), key=lambda kv: -kv[1][0]):
        L.append('| %s | %d | %d | %d |' % (kind, c.records[kind], n, b))
    L += ['', '## Top %d fields' % top, '',
          '| # | kind | field (+offset) | records | non-zero | dropped (partial) | bytes | read by the cartridge | files | example values | where |',
          '|---:|---|---|---:|---:|---|---:|---|---|---|---|']
    for i, x in enumerate(rows[:top], 1):
        files = ', '.join(x['files'][:4]) + (' +%d' % (len(x['files']) - 4) if len(x['files']) > 4 else '')
        L.append('| %d | %s | `%s` +%#x | %d | %d | %d (%d) | %d | %s | %s | %s | %s |' % (
            i, x['kind'], x['field'].replace('|', ' / '), x['offset'], x['records'], x['nonzero'], x['dropped'], x['partial'],
            x['dropped_bytes'], x['cart']['text'], files, ' '.join(x['values'][:4]), x['where'][0] if x['where'] else ''))
    blind = []
    for kind, fields in c.k.items():
        if not kind.startswith(('propdef', 'setup', 'ai ', 'global ai', 'stan', 'bg ')):
            continue
        for name, st in fields.items():
            if st['blind']:
                blind.append(dict(kind=kind, field=name, offset=st['off'], records=st['n'], nonzero=st['nonzero'],
                                  blind=st['blind'], files=sorted(st['blind_files']), values=st['blind_values'],
                                  cart=cart_says(cart, kind, name)))
    blind.sort(key=lambda x: (-x['blind'], x['kind'], x['offset']))
    js['carried_blind'] = blind
    json.dump(js, open(outjson, 'w'), indent=1)
    L += ['', '## Carried without being looked at (review list)', '',
          'Fields of setup, AI, stan and bg records that reach the converted file only by a wholesale copy '
          '(the converter never unpacks or indexes them). They are not dropped, but Perfect Dark reads them '
          'with its own struct: each one is a place where GoldenEye\'s and Perfect Dark\'s meaning of the '
          'same bytes must agree (GoldenEye\'s object flag 0x200 was Perfect Dark\'s OBJFLAG_ORTHOGONAL). '
          'A field copied into a bytearray and then parsed from the copy also lands here. %d fields; top 50:' % len(blind),
          '', '| kind | field (+offset) | records | non-zero | copied unread | read by the cartridge | files | example values |',
          '|---|---|---:|---:|---:|---|---|---|']
    for x in blind[:50]:
        files = ', '.join(x['files'][:3]) + (' +%d' % (len(x['files']) - 3) if len(x['files']) > 3 else '')
        L.append('| %s | `%s` +%#x | %d | %d | %d | %s | %s | %s |' % (x['kind'], x['field'].replace('|', ' / '), x['offset'], x['records'],
                                                                   x['nonzero'], x['blind'], x['cart']['text'], files,
                                                                   ' '.join(x['values'][:4])))
    if c.unclaimed:
        L += ['', '## Bytes no walk placed', '',
              'Non-zero bytes of a file the converter read that no segmenter here assigns to a record '
              '(strings, display lists and vertices a model reaches by pointer, data past a terminator). '
              'Read ones are fine; unread ones are listed.', '',
              '| file | non-zero unplaced | of those unread |', '|---|---:|---:|']
        for f, v in sorted(c.unclaimed.items(), key=lambda kv: -kv[1]['nonzero_unplaced_unread'])[:40]:
            L.append('| %s | %d | %d |' % (f, v['nonzero_unplaced'], v['nonzero_unplaced_unread']))
    open(outmd, 'w').write('\n'.join(L) + '\n')


def census(tree=TREE, romfile=ROM_DEFAULT, out=None, reuse=False):
    """Run (or with reuse, reload) the C census -> (roots, info, null ok, null message)."""
    out = out or os.path.expanduser('~/wt/gefidelity-run/census-out')
    os.makedirs(out, exist_ok=True)
    if reuse and os.path.exists(os.path.join(out, 'dump', 'streams.json')):
        rom = romlayout.Rom(romfile)
    else:
        rom = run_c(tree, romfile, out)
    roots, info = load_c_dump(os.path.join(out, 'dump'), rom)
    ok, msg = null_check(info, roots, NULL_STEM)
    return roots, info, ok, msg


def main():
    a = sys.argv[1:]

    def opt(name, default):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return v
        return default
    tree = os.path.abspath(opt('--tree', TREE))
    romfile = os.path.abspath(opt('--rom', ROM_DEFAULT))
    out = os.path.abspath(opt('--out', os.path.expanduser('~/wt/gefidelity-run/census-out')))
    rdir = os.path.abspath(opt('--report-dir', HERE))
    cart = opt('--cart', os.path.expanduser('~/wt/gefidelity-run/census-ares/cart.json'))
    top = int(opt('--top', '60'))
    roots, info, ok, msg = census(tree, romfile, out, '--reuse' in a)
    print(msg)
    if not ok:
        print('CENSUS NULL FAILED: the census cannot tell a read field from an unread one; no report')
        return 4
    cartj = json.load(open(cart)) if cart and os.path.exists(cart) else None
    c = analyse(info, roots, cartrecs=cart_records(cartj))
    md, js = os.path.join(rdir, 'report.md'), os.path.join(rdir, 'report.json')
    report(c, info, msg, md, js, top, cartj)
    if rdir != out:
        for f in (md, js):
            open(os.path.join(out, os.path.basename(f)), 'w').write(open(f).read())
    n = sum(1 for kind in c.k.values() for st in kind.values() if st['dropped'])
    print('census: %d records, %d fields drop non-zero data; report in %s' % (sum(c.records.values()), n, md))
    return 0


if __name__ == '__main__':
    sys.exit(main())

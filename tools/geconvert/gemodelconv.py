"""GoldenEye's prop models as Perfect Dark model files, for the GoldenEye
remake's own model table (no GE-X models).

GoldenEye's model format is Perfect Dark's ancestor. The node types are the
same numbers (GoldenEye's name -> Perfect Dark's):

  0x01 header        -> position     vehicles; PD's chrinfo reads an
                                     animation a standing prop has none of
  0x02 group         -> position     last word: a child pointer where PD reads
                                     a draw distance
  0x04 display list  -> written as 0x18 (dl)
  0x08 LOD           -> distance     same layout
  0x09 BSP           -> reorder      same layout
  0x0a box           -> box          same layout
  0x0c gunfire       -> chrgunfire   same layout (texture row pointer)
  0x12 switch        -> toggle       same layout
  0x15 group simple  -> positionheld same layout
  0x16 primary DL    -> stargunfire  vertices in fours, colours after them
  0x18 collision DL  -> dl

The texture rows are the same 12 bytes and the lists already use G_TRI4, a
G_VTX into segment 4 and image ids in their texture commands. A GoldenEye
vertex is 16 bytes with its colour in it; Perfect Dark's is 12 with a colour
index, the colours after the vertices and loaded by G_COL (segment 6). Every
G_VTX gets its own copy of what it loads with a G_COL of their colours, as the
room converter does. GoldenEye's header is in its code (the decomp's
ModelFileHeader.inc.c) and the file starts with the switch table; a Perfect
Dark file starts with its modeldef, and the switches are its parts, numbered
in order.
"""
import re, struct, sys, os, collections
import gefiles
import texremap

SEG = 0x05000000
GEPROP = '/home/sdg/perfect-dark/claude-007/007/assets/obseg/prop'


def prop_names():
    """GoldenEye's prop models in model number order (PitemZ_entries), by
    their directories in the decomp: for the offline HD fit tools only."""
    return re.findall(r'prop/([A-Za-z0-9_]+)/', open(GEPROP + '/propItemModelFileRecord.inc.c').read())


def prop_record(name):
    txt = open(os.path.join(GEPROP, name, 'propFileRecord.inc.c')).read()
    m = re.search(r'PROPFILERECORD\(\s*(\w+)\s*,\s*([-0-9.eE]+)', txt)
    return m.group(1), float(m.group(2))


def header(name):
    txt = open(os.path.join(GEPROP, name, 'ModelFileHeader.inc.c')).read()
    args = [a.strip() for a in re.search(r'MODELFILEHEADER\((.*)\)', txt).group(1).split(',')]
    return dict(numswitches=int(args[4], 0), nummatrices=int(args[5], 0), radius=float(args[6].rstrip('f')),
                numtextures=int(args[8], 0))


class Writer:
    def __init__(self):
        self.out = bytearray()

    def align(self, n):
        while len(self.out) % n:
            self.out += b'\0'

    def put(self, data, align=4):
        self.align(align)
        at = len(self.out)
        self.out += data
        return at


def convert_lists(d, vtxptr, lists, out, fours=False):
    """GoldenEye lists over 16-byte vertices -> (vertex offset, count, colour
    offset, colour count, [(GoldenEye list address, words)]): the vertices and
    colours are written into `out` now, the lists at the end of the file
    (convert())."""
    u = lambda o: struct.unpack_from('>I', d, o)[0]
    newv, cols, words_of = [], [], []
    for gdl in lists:
        if not gdl:
            words_of.append(None)
            continue
        words = []
        o = gdl - SEG
        for _ in range(100000):
            w0, w1 = struct.unpack_from('>II', d, o)
            o += 8
            op = w0 >> 24
            if op == 0x04:
                n = ((w0 >> 20) & 0xf) + 1
                v0 = (w0 >> 16) & 0xf
                seg = w1 >> 24
                src = (w1 & 0xffffff) // 16 if seg == 0x04 else ((w1 - vtxptr) // 16)
                start = len(newv)
                for i in range(n):
                    x, y, z, flag, s, t, r, g, b, a = struct.unpack_from('>3hHhh4B', d, (vtxptr - SEG) + 16 * (src + i))
                    newv.append((x, y, z, i << 2, s, t))
                    cols.append((r << 24) | (g << 16) | (b << 8) | a)
                if not fours:
                    words.append(((0x07 << 24) | (((n - 1) << 2) << 16) | (n * 4), 0x06000000 | (start * 4)))
                words.append(((0x04 << 24) | ((n - 1) << 20) | (v0 << 16) | (n * 12), 0x04000000 | (start * 12)))
            else:
                if op == 0xc0:
                    w1 = (w1 & ~0xfff) | texremap.remap(w1 & 0xfff)
                    if (w0 & 7) == 1:
                        w1 = (w1 & ~0xfff000) | (texremap.remap((w1 >> 12) & 0xfff) << 12)
                words.append((w0, w1))
                if op == 0xb8:
                    break
        words_of.append(words)
    while len(newv) % (4 if fours else 2):
        newv.append((newv[-1][:3] if newv else (0, 0, 0)) + (0, 0, 0))
        cols.append(0)
    out.align(8)
    vat = len(out.out)
    for x, y, z, c, s, t in newv:
        out.out += struct.pack('>3hBBhh', x, y, z, 0, c, s, t)
    out.align(8)
    cat = len(out.out)
    for c in cols:
        out.out += struct.pack('>I', c)
    return vat, len(newv), cat, len(cols), [(g, w) for g, w in zip(lists, words_of)]


def convert(num):
    """GoldenEye prop model number `num` -> (Perfect Dark model file bytes,
    image ids it uses, its scale), from the ROM alone."""
    name, scale, h = gefiles.rom().props()[num]
    d = gefiles.rom_file(name)
    u = lambda o: struct.unpack_from('>I', d, o)[0]
    textab = 4 * h['numswitches']
    root = textab + 12 * h['numtextures']
    switches = [u(4 * i) for i in range(h['numswitches'])]

    nodes = []
    def walk(o, parent):
        while o:
            node = dict(at=o, type=struct.unpack_from('>H', d, o)[0], rodata=u(o + 4), parent=parent,
                        child=u(o + 20) - SEG if u(o + 20) else 0, next=u(o + 12) - SEG if u(o + 12) else 0)
            nodes.append(node)
            if node['child']:
                walk(node['child'], o)
            o = node['next']
    walk(root, 0)

    w = Writer()
    w.out += bytearray(0x1c)
    rows = bytearray(d[textab:root])
    for i in range(h['numtextures']):
        struct.pack_into('>I', rows, 12 * i, texremap.remap(struct.unpack_from('>I', rows, 12 * i)[0]))
    texat = w.put(bytes(rows))
    partsat = w.put(bytearray(6 * len(switches)))
    nodesat = w.put(bytearray(0x18 * len(nodes)))
    addr = {n['at']: SEG + nodesat + 0x18 * i for i, n in enumerate(nodes)}
    reloc_node = lambda p: addr.get(p - SEG, 0) if p else 0
    reloc_tex = lambda p: SEG + texat + (p - SEG - textab) if p else 0
    images = set(struct.unpack_from('>I', d, textab + 12 * i)[0] for i in range(h['numtextures']))

    # Every list goes at the end, in the order GoldenEye's file has them: the
    # loader walks the lists in node order and takes each one's size as the
    # distance to the next (modeldef0f1a7560()), and GoldenEye's loader has the
    # same rule, so its own order is that walk's
    gdls = []
    for i, n in enumerate(nodes):
        t, ro = n['type'], n['rodata'] - SEG
        if t == 0x01:
            # a vehicle's header: a chrinfo node reads the model's animation,
            # which a standing prop has none of, so it is a position node on
            # the same matrix at the origin
            animpart, mtx = struct.unpack_from('>Hh', d, ro)
            rat = w.put(struct.pack('>3fHhhhf', 0, 0, 0, animpart, mtx, -1, -1, h['radius']))
            n['type'] = 0x02
        elif t == 0x02:
            rat = w.put(d[ro:ro + 0x14] + struct.pack('>f', h['radius']))
        elif t in (0x08, 0x12):
            rec = bytearray(d[ro:ro + (0x10 if t == 0x08 else 0x08)])
            at = 8 if t == 0x08 else 0
            struct.pack_into('>I', rec, at, reloc_node(struct.unpack_from('>I', rec, at)[0]))
            rat = w.put(bytes(rec))
        elif t == 0x09:
            rec = bytearray(d[ro:ro + 0x24])
            for at in (0x18, 0x1c):
                struct.pack_into('>I', rec, at, reloc_node(struct.unpack_from('>I', rec, at)[0]))
            rat = w.put(bytes(rec))
        elif t == 0x0a:
            rat = w.put(d[ro:ro + 0x1c])
        elif t == 0x0c:
            rec = bytearray(d[ro:ro + 0x28])
            struct.pack_into('>I', rec, 0x18, reloc_tex(struct.unpack_from('>I', rec, 0x18)[0]))
            struct.pack_into('>I', rec, 0x24, 0)
            rat = w.put(bytes(rec))
        elif t == 0x15:
            rat = w.put(d[ro:ro + 0x14])
        elif t in (0x04, 0x18):
            # the render mode type (1 to 4), which PD's dl record calls mcount:
            # it picks the preset render mode and whether the second list draws
            if t == 0x04:
                pri, sec, vtx = u(ro), u(ro + 4), u(ro + 12)
                mode = struct.unpack_from('>H', d, ro + 0x12)[0]
            else:
                pri, sec, vtx = u(ro), u(ro + 4), u(ro + 8)
                mode = struct.unpack_from('>h', d, ro + 0x18)[0]
            vat, nv, cat, nc, got = convert_lists(d, vtx, (pri, sec), w)
            rat = w.put(struct.pack('>IIIIhhHH', 0, 0, SEG + cat, SEG + vat, nv, mode, 0, nc))
            for k, (g, words) in enumerate(got):
                if words is not None:
                    gdls.append((g, words, rat + 4 * k))
            n['type'] = 0x18
        elif t == 0x16:
            nverts, vtx, pri = struct.unpack_from('>iII', d, ro)
            vat, nv, cat, nc, got = convert_lists(d, vtx, (pri,), w, fours=True)
            # a muzzle flash: a standing prop never fires, so it draws no stars
            # (the list stays, for the loader's sizes)
            rat = w.put(struct.pack('>iIII', 0, SEG + vat, 0, SEG + cat))
            gdls.append((got[0][0], got[0][1], rat + 8))
        else:
            raise ValueError('%s: node type %#x' % (name, t))
        at = nodesat + 0x18 * i
        struct.pack_into('>HHIIIII', w.out, at, n['type'], 0, SEG + rat, addr.get(n['parent'], 0),
                         addr.get(n['next'], 0), 0, addr.get(n['child'], 0))
    for g, words, fixup in sorted(gdls):
        at = w.put(b''.join(struct.pack('>II', a, b) for a, b in words), 8)
        struct.pack_into('>I', w.out, fixup, SEG + at)
    # prev pointers
    for i, n in enumerate(nodes):
        if n['next']:
            struct.pack_into('>I', w.out, addr[n['next']] - SEG + 0x10, SEG + nodesat + 0x18 * i)
    for i, p in enumerate(switches):
        struct.pack_into('>I', w.out, partsat + 4 * i, reloc_node(p))
        struct.pack_into('>h', w.out, partsat + 4 * len(switches) + 2 * i, i)
    struct.pack_into('>IIIhhfhhI', w.out, 0, SEG + nodesat, 2, SEG + partsat if switches else 0, len(switches),
                     h['nummatrices'], h['radius'], 0, h['numtextures'], SEG + texat)
    w.align(16)
    return bytes(w.out), sorted(images), scale


if __name__ == '__main__':
    ok, bad = 0, []
    for i, name in enumerate(prop_names()):
        try:
            data, images, scale = convert(i)
            ok += 1
        except Exception as e:
            bad.append((i, name, repr(e)[:80]))
    print('converted', ok, 'of', len(prop_names()))
    for b in bad[:20]:
        print(b)

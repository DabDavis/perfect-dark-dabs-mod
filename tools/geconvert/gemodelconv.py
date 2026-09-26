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

# A model header's second word is its skeleton. GoldenEye stores a pointer into
# its data segment and Perfect Dark an id it resolves through g_Skeletons[] at
# the load, so a converted prop was written with SKEL_BASIC whatever GoldenEye
# gave it - and Perfect Dark poses a *door* by skeleton: doorInitMatrices()
# writes matrix 0 and then, only for g_Skel11 and g_Skel13, the leaves. Caverns'
# eyelid and iris doors have three and thirteen matrices, so twelve of them were
# left as whatever gfxAllocate() handed over that frame: the doors were not
# where they belonged and a leaf landed across the view often enough for a
# tester to call it a triangle popping on screen.
#
# Perfect Dark kept both skeletons and poses them exactly as GoldenEye does
# (propobj.c's doorInitMatrices() against propobj.c's render at 5885), so the
# two are carried across. The rest of GoldenEye's skeletons are listed for what
# they are and left at SKEL_BASIC: their models are one matrix or are posed by
# their object type (a CCTV, an autogun, a mount), and the Perfect Dark code
# that reads those skeletons is about shooting glass out of a door or a lens,
# which is not converted.
SKEL_BASIC = 2
GE_SKELETONS = {
    0x8003a05c: 0x0f,        # cctv -> g_SkelCctv (4 switches; converter 74: the lens shot)
    0x8003a070: SKEL_BASIC,  # console_one_screen
    0x8003a084: SKEL_BASIC,  # console_four_screen
    0x8003a0b0: SKEL_BASIC,  # tv_holder
    0x8003a0e0: SKEL_BASIC,  # rotating_stuff (the autoguns)
    0x8003a100: 0x11,        # eyelid_door -> g_Skel11  (Pdoor_eyelidZ, 3 matrices)
    0x8003a15c: 0x13,        # iris_door   -> g_Skel13  (Pdoor_irisZ, 13 matrices)
    0x8003a170: SKEL_BASIC,  # walletbond
    0x8003a19c: SKEL_BASIC,  # car
    0x8003a1c8: SKEL_BASIC,  # flying
    0x8003a1dc: 0x10,        # door (windowed) -> g_SkelWindowedDoor (4 switches)
    0x8003a208: SKEL_BASIC,  # tank
    0x8003a21c: SKEL_BASIC,  # hat
    0x8003c4d8: SKEL_BASIC,  # standard_object
    0x8003c4fc: 0x03,        # prop_weapon -> g_SkelChrGun (3 switches)
}


def prop_skel(skeleton, numswitches):
    """The Perfect Dark skeleton id for a GoldenEye prop's skeleton pointer.
    A windowed door needs its four switches: box, toggle, box, glass."""
    skel = GE_SKELETONS.get(skeleton, SKEL_BASIC)
    if skel in (0x0f, 0x10) and numswitches < 4:
        return SKEL_BASIC
    if skel == 0x03 and numswitches < 3:
        return SKEL_BASIC
    return skel


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


def texdata_size(width, height, level, depth):
    """The bytes a texture stored in a model file takes: every mipmap level of
    it, `depth` bytes a texel (the GoldenEye logo's is 32x32 over six levels)."""
    n = 0
    for i in range(max(level, 1)):
        n += max(width >> i, 1) * max(height >> i, 1) * depth
    return (n + 7) & ~7


def texture_rows(d, w, textab, root, numtextures):
    """The model's texture table written into `w` -> (its offset, the image ids
    it names, where its embedded textures moved to). A row whose first word is a
    0x05 segment pointer is a texture stored in the file itself rather than a
    global image (the GoldenEye logo's two): its bytes are copied over and the
    row repointed, which is what the port's model preprocessing expects of one
    (CT_TEXDATA).

    The moves are given back because a list loads such a texture by its own
    address (`G_SETTIMG`) and not through the row, so convert_lists() has to
    move those with it."""
    rows = bytearray(d[textab:root])
    images, embedded = set(), []
    for i in range(numtextures):
        ptr, width, height, level, fmt, depth = struct.unpack_from('>IBBBBB', rows, 12 * i)
        if (ptr & 0xff000000) == SEG:
            embedded.append((i, ptr - SEG, texdata_size(width, height, level, depth)))
        else:
            images.add(ptr)
            struct.pack_into('>I', rows, 12 * i, texremap.remap(ptr))
    texat = w.put(bytes(rows))
    moved = []
    for i, at, size in embedded:
        new = w.put(d[at:at + size], 8)
        struct.pack_into('>I', w.out, texat + 12 * i, SEG + new)
        moved.append((at, size, new))
    return texat, images, moved


def convert_lists(d, vtxptr, lists, out, fours=False, moved=()):
    """GoldenEye lists over 16-byte vertices -> (vertex offset, count, colour
    offset, colour count, [(GoldenEye list address, words)]): the vertices and
    colours are written into `out` now, the lists at the end of the file
    (convert()).

    `moved` is texture_rows()'s account of where the file's own textures went.
    A `G_SETTIMG` naming one is repointed with it: the address is the file's, so
    left alone it read whatever the new layout had put there - which is what
    drew the GoldenEye logo without its red ellipse (the ring's own texel landed
    inside the node table, alpha 0) and sampled the letters a few texels in."""
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
                elif op == 0xfd and (w1 & 0xff000000) == SEG:
                    a = w1 - SEG
                    for at, size, new in moved:
                        if at <= a < at + size:
                            w1 = SEG + new + (a - at)
                            break
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
            # 0x0f, GoldenEye's interlink, drawn as nothing and a node
            # Perfect Dark has no reader for: left out, as gechr.py leaves a
            # shadow out (no prop has one; the watch detonator's hand does)
            if (node['type'] & 0xff) == 0x0f:
                if node['child'] or node['next']:
                    raise ValueError('%s: an interlink node with more after it' % name)
            else:
                nodes.append(node)
                if node['child']:
                    walk(node['child'], o)
            o = node['next']
    walk(root, 0)

    w = Writer()
    w.out += bytearray(0x1c)
    texat, images, moved = texture_rows(d, w, textab, root, h['numtextures'])
    partsat = w.put(bytearray(6 * len(switches)))
    nodesat = w.put(bytearray(0x18 * len(nodes)))
    addr = {n['at']: SEG + nodesat + 0x18 * i for i, n in enumerate(nodes)}
    reloc_node = lambda p: addr.get(p - SEG, 0) if p else 0
    reloc_tex = lambda p: SEG + texat + (p - SEG - textab) if p else 0

    # Every list goes at the end, in the order GoldenEye's file has them: the
    # loader walks the lists in node order and takes each one's size as the
    # distance to the next (modeldef0f1a7560()), and GoldenEye's loader has the
    # same rule, so its own order is that walk's
    gdls = []
    for i, n in enumerate(nodes):
        t, ro = n['type'], n['rodata'] - SEG
        if t == 0x01:
            # An aircraft's header, and only ever that: the four flying models
            # (PplaneZ, PtigerZ and the two helicopters) carry one and nothing
            # else in the ROM does. It is Perfect Dark's own chrinfo node -
            # `{u16 animpart, s16 mtxindex, f32, u16 rwdataindex}` against
            # GoldenEye's same two fields - and it is what an animation is
            # played through, so it stays one. The last two words are the
            # game's to fill: `modelCalculateRwDataIndexes()` writes the index
            # at the load and nothing ever reads the float.
            #
            # It used to be demoted to a position node, because
            # `modelUpdateChrNodeMtx()` reads `model->anim` with no guard and a
            # standing aircraft has none. The guard is in the port now
            # (model.c), which is GoldenEye's own answer: with no animation the
            # node's matrix is its parent's.
            animpart, mtx = struct.unpack_from('>Hh', d, ro)
            rwdata = struct.unpack_from('>H', d, ro + 0x0c)[0]
            rat = w.put(struct.pack('>HhfH2x', animpart, mtx, 0.0, rwdata))
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
                # one byte (s8 ModelType), where the 0x18 record's is a word
                mode = struct.unpack_from('>b', d, ro + 0x12)[0]
            else:
                pri, sec, vtx = u(ro), u(ro + 4), u(ro + 8)
                mode = struct.unpack_from('>h', d, ro + 0x18)[0]
            vat, nv, cat, nc, got = convert_lists(d, vtx, (pri, sec), w, moved=moved)
            rat = w.put(struct.pack('>IIIIhhHH', 0, 0, SEG + cat, SEG + vat, nv, mode, 0, nc))
            for k, (g, words) in enumerate(got):
                if words is not None:
                    gdls.append((g, words, rat + 4 * k))
            n['type'] = 0x18
        elif t == 0x16:
            nverts, vtx, pri = struct.unpack_from('>iII', d, ro)
            vat, nv, cat, nc, got = convert_lists(d, vtx, (pri,), w, fours=True, moved=moved)
            # a muzzle flash: GoldenEye's own count of quads; a gun's star is
            # under switch 2, shown only while it fires (g_SkelChrGun)
            rat = w.put(struct.pack('>iIII', nverts, SEG + vat, 0, SEG + cat))
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
        # an interlink left out ends its chain and has nothing to point back
        if n['next'] and n['next'] in addr:
            struct.pack_into('>I', w.out, addr[n['next']] - SEG + 0x10, SEG + nodesat + 0x18 * i)
    for i, p in enumerate(switches):
        struct.pack_into('>I', w.out, partsat + 4 * i, reloc_node(p))
        struct.pack_into('>h', w.out, partsat + 4 * len(switches) + 2 * i, i)
    struct.pack_into('>IIIhhfhhI', w.out, 0, SEG + nodesat, prop_skel(h['skeleton'], len(switches)),
                     SEG + partsat if switches else 0, len(switches),
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

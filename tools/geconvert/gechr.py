"""GoldenEye's character models (bodies and heads) as Perfect Dark model files,
for the GoldenEye remake's intro (port/src/geintro.c).

GoldenEye's characters are its props' format with three more node types, and
its skeleton is Perfect Dark's own: `g_SkelChrJoints` is GoldenEye's guard
joint list, joint for joint, with GoldenEye's `ModelJoint {NodeType, mtxA,
mtxB}` (6 bytes, the matrix a channel index stepping by 3) written as Perfect
Dark's `things[2]` (the part and its mirror, so mtx / 3). Every character in
the ROM uses that one skeleton, so a converted body is loaded with `SKEL_CHR`
and needs no skeleton of its own.

  c_item_entries   20-byte {header, filename, f32 scale, f32 pov, flags} rows
                   from data 0x1d080, 80 of them - the bodies first, then the
                   heads (`gerom.chrs()`)

What the prop converter (gemodelconv.py) does not meet in a prop but does here:

  0x01 header    -> chrinfo    a prop's became a position node because a prop
                               has no animation; a character's is the node the
                               animation plays on, so it stays a chrinfo:
                               GoldenEye's first-group pointer and its two
                               group numbers are dropped, since Perfect Dark
                               reads a f32 there
  0x0d shadow    -> dropped    the blob a character stands on: Perfect Dark's
                               model format has no such node and the port's
                               model preprocessing refuses one, and every one of
                               the 42 in the ROM is a childless leaf at the end
                               of its chain, so leaving it out relinks nothing
  0x17 head      -> headspot   one u16, the same
  the type's high byte         GoldenEye sets 0x100 on a group whose matrix the
                               animation drives; Perfect Dark reads `type & 0xff`
                               and keeps the flags, so the whole u16 is written

A head file is one display list node on one matrix and no skeleton; it is
converted the same way and loaded with `SKEL_HEAD`.
"""
import struct
import gefiles
import gemodelconv

SEG = gemodelconv.SEG


def convert(num):
    """GoldenEye character number `num` (its c_item_entries row) -> (Perfect
    Dark model file bytes, the image ids it uses, its scale), from the ROM
    alone. `num` is what the intro's tables name (BODY_*/HEAD_* is the row)."""
    name, scale, h = gefiles.rom().chrs()[num]
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
            if (node['type'] & 0xff) != 0x0d:
                nodes.append(node)
                if node['child']:
                    walk(node['child'], o)
            o = node['next']

    walk(root, 0)

    w = gemodelconv.Writer()
    w.out += bytearray(0x1c)
    texat, images, moved = gemodelconv.texture_rows(d, w, textab, root, h['numtextures'])
    partsat = w.put(bytearray(6 * len(switches)))
    nodesat = w.put(bytearray(0x18 * len(nodes)))
    addr = {n['at']: SEG + nodesat + 0x18 * i for i, n in enumerate(nodes)}
    reloc_node = lambda p: addr.get(p - SEG, 0) if p else 0
    reloc_tex = lambda p: SEG + texat + (p - SEG - textab) if p else 0

    gdls = []
    for i, n in enumerate(nodes):
        t, flags, ro = n['type'] & 0xff, n['type'] & 0xff00, n['rodata'] - SEG
        if t == 0x01:
            # the node the animation plays on: GoldenEye's animpart and matrix,
            # then the f32 Perfect Dark reads where GoldenEye keeps its first
            # group, and the rwdata index
            animpart, mtx = struct.unpack_from('>Hh', d, ro)
            rwdata = struct.unpack_from('>H', d, ro + 0x0c)[0]
            rat = w.put(struct.pack('>HhfH2x', animpart, mtx, 0.0, rwdata))
        elif t == 0x02:
            # GoldenEye numbers a character's animated parts from 1, with the
            # header node above them as 0, and its skeleton's joint j holds the
            # channel of part j; Perfect Dark numbers them from 0 and reads the
            # channels in part order, so every part moves down one and its
            # skeleton is then g_SkelChrJoints joint for joint
            rec = bytearray(d[ro:ro + 0x14])
            part = struct.unpack_from('>H', rec, 0x0c)[0]
            if part == 1:
                # the hips, and the one node that must not keep its channel:
                # Perfect Dark's chrinfo *is* the hip node and turns on part 0
                # itself, while GoldenEye's header node applies no joint
                # rotation at all and leaves the turn to this group. Shifted to
                # 0 the channel was applied at both, so bond_eye_fire's ninety
                # degrees came out as a hundred and eighty and Bond finished
                # the gun barrel facing away from the camera. It keeps its
                # place and its matrix as a held position and loses only the
                # rotation; its origin is (0, 0, 0) in all 43 bodies, and it is
                # the header's own child in every one of them.
                rat = w.put(bytes(rec[:0x0c]) + bytes(rec[0x0e:0x10]))
                t = 0x15
            else:
                struct.pack_into('>H', rec, 0x0c, part - 1 if part else 0)
                rat = w.put(bytes(rec) + struct.pack('>f', h['radius']))
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
        elif t == 0x17:
            rat = w.put(d[ro:ro + 2])
        elif t in (0x04, 0x18):
            if t == 0x04:
                pri, sec, vtx = u(ro), u(ro + 4), u(ro + 12)
                mode = struct.unpack_from('>H', d, ro + 0x12)[0]
            else:
                pri, sec, vtx = u(ro), u(ro + 4), u(ro + 8)
                mode = struct.unpack_from('>h', d, ro + 0x18)[0]
            vat, nv, cat, nc, got = gemodelconv.convert_lists(d, vtx, (pri, sec), w, moved=moved)
            rat = w.put(struct.pack('>IIIIhhHH', 0, 0, SEG + cat, SEG + vat, nv, mode, 0, nc))
            for k, (g, words) in enumerate(got):
                if words is not None:
                    gdls.append((g, words, rat + 4 * k))
            t = 0x18
        elif t == 0x16:
            nverts, vtx, pri = struct.unpack_from('>iII', d, ro)
            vat, nv, cat, nc, got = gemodelconv.convert_lists(d, vtx, (pri,), w, fours=True, moved=moved)
            rat = w.put(struct.pack('>iIII', 0, SEG + vat, 0, SEG + cat))
            gdls.append((got[0][0], got[0][1], rat + 8))
        else:
            raise ValueError('%s: node type %#x' % (name, n['type']))
        at = nodesat + 0x18 * i
        struct.pack_into('>HHIIIII', w.out, at, t | flags, 0, SEG + rat, addr.get(n['parent'], 0),
                         addr.get(n['next'], 0), 0, addr.get(n['child'], 0))
    for g, words, fixup in sorted(gdls):
        at = w.put(b''.join(struct.pack('>II', a, b) for a, b in words), 8)
        struct.pack_into('>I', w.out, fixup, SEG + at)
    for i, n in enumerate(nodes):
        # a next that was the shadow left out ends the chain there and has
        # nothing to point back
        if n['next'] and n['next'] in addr:
            struct.pack_into('>I', w.out, addr[n['next']] - SEG + 0x10, SEG + nodesat + 0x18 * i)
    for i, p in enumerate(switches):
        struct.pack_into('>I', w.out, partsat + 4 * i, reloc_node(p))
        struct.pack_into('>h', w.out, partsat + 4 * len(switches) + 2 * i, i)
    # SKEL_CHR where the character has GoldenEye's guard skeleton, SKEL_HEAD
    # where it has none (a head is one list on one matrix)
    skel = 0x09 if h['skeleton'] else 0x0d
    struct.pack_into('>IIIhhfhhI', w.out, 0, SEG + nodesat, skel, SEG + partsat if switches else 0,
                     len(switches), h['nummatrices'], h['radius'], 0, h['numtextures'], SEG + texat)
    w.align(16)
    return bytes(w.out), sorted(images), scale


if __name__ == '__main__':
    ok, bad = 0, []
    rows = gefiles.rom().chrs()
    for i, (name, scale, h) in enumerate(rows):
        try:
            data, images, s = convert(i)
            ok += 1
        except Exception as e:
            bad.append((i, name, repr(e)[:90]))
    print('converted %d of %d characters' % (ok, len(rows)))
    for b in bad:
        print(' ', b)

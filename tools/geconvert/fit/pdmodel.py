#!/usr/bin/env python3
"""Perfect Dark model file reader (research tool): the list nodes in the game's walk.

Reads an *inflated* model file (a ROM's 0x1173 blob: skip 5 bytes, raw
deflate). Pointers are segment 5, offset = ptr & 0xffffff. Node order is
xblaMeshEnumListNodes()'s depth-first walk; a list node's rest offset is the
sum of the POSITION/POSITIONHELD nodes above it (xblaMeshNodeRestOffset()),
which is the space model-packs/<pack>/n64/ OBJs are written in.
"""
import struct, sys, zlib

T_CHRINFO, T_POSITION, T_GUNDL, T_DISTANCE, T_TOGGLE, T_POSHELD, T_HEADSPOT, T_DL = 0x01, 0x02, 0x04, 0x08, 0x12, 0x15, 0x17, 0x18


def load(path):
    d = open(path, 'rb').read()
    if d[:2] == b'\x11\x73':
        d = zlib.decompressobj(-15).decompress(d[5:])
    return d


class PdModel:
    def __init__(self, data):
        self.d = data
        self.root = self.ptr(0)
        self.numparts, self.nummatrices = struct.unpack_from('>hh', data, 12)
        self.scale = struct.unpack_from('>f', data, 16)[0]
        self.listnodes = []
        self.walk()

    def ptr(self, off):
        p = struct.unpack_from('>I', self.d, off)[0]
        return None if p == 0 else p & 0xffffff

    def node(self, off):
        typ = struct.unpack_from('>H', self.d, off)[0] & 0xff
        return dict(off=off, type=typ, rodata=self.ptr(off + 4), parent=self.ptr(off + 8),
                    next=self.ptr(off + 12), child=self.ptr(off + 20))

    def rest(self, off):
        o = [0.0, 0.0, 0.0]
        part = mtx = None
        while off is not None:
            n = self.node(off)
            if n['type'] in (T_POSITION, T_POSHELD) and n['rodata'] is not None:
                x, y, z = struct.unpack_from('>3f', self.d, n['rodata'])
                o[0] += x; o[1] += y; o[2] += z
                if part is None and n['type'] == T_POSITION:
                    part, mtx = struct.unpack_from('>Hh', self.d, n['rodata'] + 12)
            off = n['parent']
        return o, part, mtx

    def walk(self):
        off, walked = self.root, 0
        while off is not None and walked < 4096:
            walked += 1
            n = self.node(off)
            if n['type'] in (T_DL, T_GUNDL):
                ro = n['rodata']
                if n['type'] == T_DL:
                    vptr, nv = self.ptr(ro + 12), struct.unpack_from('>h', self.d, ro + 16)[0]
                else:
                    vptr, nv = self.ptr(ro + 12), struct.unpack_from('>h', self.d, ro + 16)[0]
                rest, part, mtx = self.rest(off)
                verts = [struct.unpack_from('>hhh', self.d, vptr + 12 * i) for i in range(nv)] if vptr is not None else []
                # the ancestors' types, to tell a far LOD or a toggle apart
                anc, a = [], n['parent']
                while a is not None:
                    an = self.node(a)
                    anc.append(an['type'])
                    a = an['parent']
                self.listnodes.append(dict(index=len(self.listnodes), off=off, type=n['type'], rest=rest,
                                           part=part, mtx=mtx, verts=verts, ancestors=anc))
            if n['child'] is not None:
                off = n['child']
                continue
            while off is not None:
                n = self.node(off)
                if n['next'] is not None:
                    off = n['next']
                    break
                off = n['parent']


if __name__ == '__main__':
    m = PdModel(load(sys.argv[1]))
    print('%s: parts %d matrices %d scale %.4f, %d list nodes' % (sys.argv[1], m.numparts, m.nummatrices, m.scale, len(m.listnodes)))
    for ln in m.listnodes:
        v = ln['verts']
        box = ('%s..%s' % ([min(p[a] for p in v) for a in range(3)], [max(p[a] for p in v) for a in range(3)])) if v else '-'
        flags = ''.join('D' if t == T_DISTANCE else 'T' if t == T_TOGGLE else 'H' if t == T_HEADSPOT else '' for t in ln['ancestors'])
        print('  node%-2d type %02x part %-3s mtx %-3s rest %s nv %3d %-4s box %s' % (
            ln['index'], ln['type'], ln['part'], ln['mtx'], ' '.join('%7.1f' % x for x in ln['rest']), len(v), flags, box))

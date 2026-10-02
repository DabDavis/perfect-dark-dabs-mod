"""Walk a converted (Perfect Dark format) model file's node tree: type, rodata, depth."""
import sys, zlib, struct
BASE = 0x05000000
TYPES = {1: 'chrinfo', 2: 'position', 4: 'gundl', 5: 'type05', 8: 'distance', 9: 'reorder', 0xa: 'bbox', 0xb: 'type0b',
         0xc: 'chrgunfire', 0xd: 'type0d', 0xe: 'type0e', 0xf: 'type0f', 0x11: 'type11', 0x12: 'toggle', 0x15: 'positionheld',
         0x16: 'stargunfire', 0x17: 'headspot', 0x18: 'dl', 0x19: 'type19'}

def load(path):
    d = open(path, 'rb').read()
    if d[:2] == b'\x11\x73':
        return zlib.decompressobj(-15).decompress(d[5:])
    return d

def walk(m, addr, depth=0, out=None):
    out = [] if out is None else out
    seen = 0
    while addr and seen < 2000:
        o = addr - BASE
        typ, rod, parent, nxt, prev, child = struct.unpack_from('>HxxIIIII', m, o)
        extra = ''
        ro = rod - BASE if rod else None
        if (typ & 0xff) == 0x18 and ro is not None:
            opa, xlu, colours, vertices, numvertices, mcount, rwi, numcolours = struct.unpack_from('>IIIIhhhh', m, ro)
            extra = 'opa %x xlu %x verts %d cols %d' % (opa, xlu, numvertices, numcolours)
        elif (typ & 0xff) == 0x12 and ro is not None:
            target, rwi = struct.unpack_from('>Ih', m, ro)
            extra = 'target %x rwdata %d' % (target, rwi)
        elif (typ & 0xff) == 0x08 and ro is not None:
            near, far, target, rwi = struct.unpack_from('>ffIh', m, ro)
            extra = 'near %.0f far %.0f target %x' % (near, far, target)
        elif (typ & 0xff) == 0x09 and ro is not None:
            a, b, c = struct.unpack_from('>fff', m, ro)
            u18, u1c, side = struct.unpack_from('>IIh', m, ro + 0x18)
            extra = 'reorder %.1f %.1f %.1f front %x back %x side %d' % (a, b, c, u18, u1c, side)
        elif (typ & 0xff) == 0x02 and ro is not None:
            x, y, z, part = struct.unpack_from('>fffh', m, ro)
            extra = 'pos %.1f %.1f %.1f part %d' % (x, y, z, part)
        out.append((depth, addr, typ, TYPES.get(typ & 0xff, hex(typ)), extra))
        if child:
            walk(m, child, depth + 1, out)
        addr = nxt
        seen += 1
    return out

if __name__ == '__main__':
    m = load(sys.argv[1])
    root = struct.unpack_from('>I', m, 0)[0]
    for depth, addr, typ, name, extra in walk(m, root):
        print('%s%s @%x %s' % ('  ' * depth, name, addr, extra))

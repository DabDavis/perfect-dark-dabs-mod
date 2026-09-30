"""Objects in GE-X's solo setups (Perfect Dark's N64 setup format)."""
import struct, zlib, collections, sys
import gexdata
SIZES = {}
NAMES = {}
for line in open(__file__.rsplit('/', 1)[0] + '/n64objsizes.txt'):
    n, t, s = line.split(); SIZES[int(t)] = int(s); NAMES[int(t)] = n

def inflate(b):
    if b[:2] == b'\x11\x73': return zlib.decompressobj(-15).decompress(b[5:])
    return b

def setup(name):
    return inflate(open(gexdata.MOD + '/files/' + name, 'rb').read())

def props(d):
    h = struct.unpack_from('>8I', d, 0)
    o = h[4]; out = []
    while True:
        w0 = struct.unpack_from('>I', d, o)[0]; t = w0 & 0xff
        if t == 0x34: break
        n = SIZES.get(t)
        if n is None: raise ValueError('type %#x at %#x' % (t, o))
        out.append((t, o, d[o:o + 4 * n]))
        o += 4 * n
    return out

if __name__ == '__main__':
    st = gexdata.stages()
    for key, sid in [('dam',0x30),('run',0x22),('tra',0x23),('pete',0x1c),('jun',0x2d)]:
        d = setup(st[sid]['setup'])
        p = props(d)
        c = collections.Counter(NAMES[t] for t, _, _ in p)
        print(key, st[sid]['setup'], len(p), dict(c))

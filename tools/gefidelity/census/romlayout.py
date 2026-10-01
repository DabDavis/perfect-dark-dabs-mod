"""Where things are in GoldenEye's US ROM, for the census's own walks: the data
segment, its file table, the prop and character tables, the fog and level
tables, the animation segments. The numbers are port/src/geconvert.c's (the
only converter since 2026-10-01; it reads the same places).
"""
import struct, zlib

US_SIZE = 0xc00000
DATA_ROM = 0x21990
DATA_VRAM = 0x80020d90
FILES_AT = 0x252c4
PROPS_AT = 0x19498
NUM_PROPS = 340
CHRS_AT = 0x1d080
NUM_CHRS = 80
FOG_AT = 0x24080
LEVELINFO_AT = 0x8004448c
LEVELINFO_ROWS = 38
ANIM_DATA_ROM = 0x28e980
GLOBAL_AI_AT = 0x8003744c


def to_z64(rom):
    head = rom[:4]
    if head == b'\x80\x37\x12\x40':
        return bytes(rom)
    b = bytearray(rom)
    if head == b'\x37\x80\x40\x12':
        b[0::2], b[1::2] = rom[1::2], rom[0::2]
    elif head == b'\x40\x12\x37\x80':
        b[0::4], b[1::4], b[2::4], b[3::4] = rom[3::4], rom[2::4], rom[1::4], rom[0::4]
    else:
        raise ValueError('not an N64 ROM')
    return bytes(b)


def inflate1172(b):
    if b[:2] != b'\x11\x72':
        raise ValueError('not a 1172 block')
    return zlib.decompressobj(-15).decompress(b[2:])


def string(data, ptr):
    o = ptr - DATA_VRAM
    return data[o:data.index(b'\0', o)].decode('latin-1')


class Rom:
    def __init__(self, path):
        self.rom = to_z64(open(path, 'rb').read())
        self.data = inflate1172(self.rom[DATA_ROM:])
        self.files = {}         # stem -> (ROM address, stored size)
        rows = []
        k = 0
        while struct.unpack_from('>I', self.data, FILES_AT + 12 * k)[0] == k:
            _, name, addr = struct.unpack_from('>III', self.data, FILES_AT + 12 * k)
            rows.append((string(self.data, name) if name else '', addr))
            k += 1
        for (name, addr), (_, nxt) in zip(rows[1:], rows[2:]):
            self.files[name.rsplit('/', 1)[-1].rsplit('.', 1)[0]] = (addr, nxt - addr)

    def file(self, stem):
        addr, size = self.files[stem]
        d = self.rom[addr:addr + size]
        return d if stem.startswith('bg_') else inflate1172(d)

    def models(self):
        """[(stem, numswitches, numtextures)] of the prop table, then the characters'."""
        out = []
        for at, n, row in ((PROPS_AT, NUM_PROPS, 12), (CHRS_AT, NUM_CHRS, 20)):
            for k in range(n):
                hdr, name = struct.unpack_from('>II', self.data, at + row * k)
                ho = hdr - DATA_VRAM
                nsw, ntex = struct.unpack_from('>h', self.data, ho + 12)[0], struct.unpack_from('>h', self.data, ho + 22)[0]
                out.append((string(self.data, name), nsw, ntex))
        return out

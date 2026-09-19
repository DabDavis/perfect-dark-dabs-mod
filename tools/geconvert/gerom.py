"""Everything the remake's converter reads, out of the player's GoldenEye ROM
alone (US, 1.0): no decomp tree. port/src/geconvert.c reads the same places.

The game's data segment is one 1172 blob at ROM 0x21990 that runs at
0x80020d90. In it:
- file_resource_table: 12-byte {index, name, ROM address} rows from 0x252c4,
  index 0 empty, running while index counts up; a file's size is the distance
  to the next row's address
- g_Textures: 8-byte rows from 0x28570, the low 24 bits of the first word the
  image's stored size; images sit end to end from ROM 0x8f7df0 in that order
- fog_tables (bgfog.c, US): 92-byte rows from 0x24080, the first word the
  level id; ids of 100 and up are the cinema/alt/player-count variants
- PitemZ_entries: 12-byte {header, filename, scale} rows from 0x19498, 340 of
  them, the header a ModelFileHeader in the segment
"""
import hashlib, struct, zlib

US_MD5 = '70c525880240c1e838b8b1be35666c3b'
DATA_ROM = 0x21990
DATA_VRAM = 0x80020d90
FILES_AT = 0x252c4
IMAGES_AT = 0x28570
IMAGES_ROM = 0x8f7df0
NUM_IMAGES = 2698
FOG_AT = 0x24080
FOG_ROW = 92
PROPS_AT = 0x19498
NUM_PROPS = 340
CHRS_AT = 0x1d080
NUM_CHRS = 80
# the animations: two segments of their own, raw in the ROM. A record in the
# data one is a 0x14 header {entry, u16 numframes, u8 width, u8 loop,
# bitDescriptors, u16 joints, u16 bitsperframe, bitStream}, then its four
# root-motion descriptors {u16 bitoffset, u8 bitcount, pad, u16 base} and then
# their bit stream. **The two pointers in the header are where those two live**:
# they are offsets into animation_data, relocated into pointers when the segment
# loads, and the blocks sit between the records rather than after their own - so
# reading the descriptors at record+0x14 and the stream at record+0x2c gives the
# *next* animation's, which is what gave bond_eye_walk the stride of
# bond_eye_fire and bond_eye_fire no root motion at all. The header's `entry` is
# the offset of the animation's frames in the entry segment, each frame
# bitsperframe/8 bytes of joint rotations, `width` bits a channel.
ANIM_ENTRY_ROM = 0x124ac0
ANIM_DATA_ROM = 0x28e980

# GoldenEye's level ids (bondconstants.h LEVELID)
LEVELIDS = {'BUNKER1': 9, 'SILO': 20, 'STATUE': 22, 'CONTROL': 23, 'ARCHIVES': 24, 'TRAIN': 25,
            'FRIGATE': 26, 'BUNKER2': 27, 'AZTEC': 28, 'STREETS': 29, 'DEPOT': 30, 'COMPLEX': 31,
            'EGYPT': 32, 'DAM': 33, 'FACILITY': 34, 'RUNWAY': 35, 'SURFACE': 36, 'JUNGLE': 37,
            'TEMPLE': 38, 'CAVERNS': 39, 'CITADEL': 40, 'CRADLE': 41, 'SURFACE2': 43, 'BASEMENT': 45, 'STACK': 46,
            'LIBRARY': 48, 'CAVES': 50}


def to_z64(rom):
    """A ROM in big-endian (.z64) order, from any of the three dumps."""
    head = rom[:4]
    if head == b'\x80\x37\x12\x40':
        return rom
    b = bytearray(rom)
    if head == b'\x37\x80\x40\x12':
        b[0::2], b[1::2] = rom[1::2], rom[0::2]
    elif head == b'\x40\x12\x37\x80':
        b[0::4], b[1::4], b[2::4], b[3::4] = rom[3::4], rom[2::4], rom[1::4], rom[0::4]
    else:
        raise ValueError('not an N64 ROM')
    return bytes(b)


def inflate1172(b):
    assert b[:2] == b'\x11\x72', b[:4].hex()
    return zlib.decompressobj(-15).decompress(b[2:])


class Rom:
    def __init__(self, path):
        self.rom = to_z64(open(path, 'rb').read())
        if hashlib.md5(self.rom).hexdigest() != US_MD5:
            raise ValueError('%s is not GoldenEye 007 (US)' % path)
        self.data = inflate1172(self.rom[DATA_ROM:])
        self.files = {}
        rows = []
        k = 0
        while struct.unpack_from('>I', self.data, FILES_AT + 12 * k)[0] == k:
            _, name, addr = struct.unpack_from('>III', self.data, FILES_AT + 12 * k)
            rows.append((self.string(name) if name else '', addr))
            k += 1
        for (name, addr), (_, nxt) in zip(rows[1:], rows[2:]):
            self.files[name.rsplit('/', 1)[-1].rsplit('.', 1)[0]] = (addr, nxt - addr)

    def string(self, ptr):
        o = ptr - DATA_VRAM
        return self.data[o:self.data.index(b'\0', o)].decode()

    def file(self, stem):
        """A file by its name without directory or extension: stored bytes for
        a bg file (its rooms are compressed inside), inflated for the rest."""
        addr, size = self.files[stem]
        d = self.rom[addr:addr + size]
        return d if stem.startswith('bg_') else inflate1172(d)

    def image(self, num):
        """Image `num`'s stored bytes (Perfect Dark's texture format), or None."""
        if not 0 <= num < NUM_IMAGES:
            return None
        at = IMAGES_ROM
        for k in range(num):
            at += struct.unpack_from('>I', self.data, IMAGES_AT + 8 * k)[0] & 0xffffff
        size = struct.unpack_from('>I', self.data, IMAGES_AT + 8 * num)[0] & 0xffffff
        return self.rom[at:at + size]

    def fog_rows(self):
        """{level id name: the row's 30 values after the id}, the one-player rows."""
        names = {v: k for k, v in LEVELIDS.items()}
        rows = {}
        o = FOG_AT
        while True:
            lid = struct.unpack_from('>I', self.data, o)[0]
            if lid == 0 and o > FOG_AT or lid >= 0x10000:
                break
            if lid in names:
                f = struct.unpack_from('>6f3I4BfHH3fB3xfHH4f', self.data, o + 4)
                # as bgfog.c lists them: iswater's three padding bytes are columns too
                rows[names[lid]] = list(f[:20]) + [0, 0, 0] + list(f[20:])
            o += FOG_ROW
        return rows

    def props(self):
        """[(file stem, scale, header dict)] in model number order."""
        out = []
        for k in range(NUM_PROPS):
            hdr, name, scale = struct.unpack_from('>IIf', self.data, PROPS_AT + 12 * k)
            ho = hdr - DATA_VRAM
            _, _, _, nsw, nmtx, radius, _, ntex = struct.unpack_from('>IIIhhfhh', self.data, ho)
            out.append((self.string(name), scale, dict(numswitches=nsw, nummatrices=nmtx, radius=radius, numtextures=ntex)))
        return out

    def chrs(self):
        """[(file stem, scale, header dict)] in character number order
        (c_item_entries: the bodies, then the heads)."""
        out = []
        for k in range(NUM_CHRS):
            hdr, name, scale, pov, flags = struct.unpack_from('>IIffI', self.data, CHRS_AT + 20 * k)
            ho = hdr - DATA_VRAM
            _, skel, _, nsw, nmtx, radius, _, ntex = struct.unpack_from('>IIIhhfhh', self.data, ho)
            out.append((self.string(name), scale,
                        dict(numswitches=nsw, nummatrices=nmtx, radius=radius, numtextures=ntex,
                             skeleton=skel, ismale=(flags >> 24) & 1, hashead=(flags >> 16) & 1,
                             pov=pov)))
        return out

    def anim(self, at):
        """The animation whose record is at ROM address `at`: its header, its
        root-motion descriptors and the two bit streams, read from the ROM."""
        entry, w1, bd, w3, bs = struct.unpack_from('>IIIII', self.rom, at)
        numframes, width, loop = w1 >> 16, (w1 >> 8) & 0xff, w1 & 0xff
        bitsperframe = w3 & 0xffff
        descat = ANIM_DATA_ROM + (bd & 0xffffff)
        streamat = ANIM_DATA_ROM + (bs & 0xffffff)
        desc = [struct.unpack_from('>HBxH', self.rom, descat + 6 * i) for i in range(4)]
        rootbits = sum(c for _, c, _ in desc)
        root = self.rom[streamat:streamat + (rootbits * numframes + 7) // 8]
        frames = self.rom[ANIM_ENTRY_ROM + entry:ANIM_ENTRY_ROM + entry + numframes * (bitsperframe // 8)]
        return dict(numframes=numframes, width=width, loop=loop, bitsperframe=bitsperframe,
                    descriptors=desc, rootbits=rootbits, root=root, frames=frames)

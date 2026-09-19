"""GoldenEye's animations as Perfect Dark animations, for the GoldenEye
remake's intro (port/src/geintro.c).

GoldenEye keeps an animation in two places: a record in the `animation_data`
segment (the header, the four root-motion channels' descriptors, and their bit
stream) and the frames themselves in `animation_entries`, `bitsperframe / 8`
bytes a frame of joint rotations, three channels a joint, `width` bits a
channel, in joint order (`Joints[j].mtxA * width` is joint j's bit, and mtxA
steps by three).

Perfect Dark reads the same thing out of one buffer: `headerlen` bytes that say
how to read a frame, then `numframes * bytesperframe` frames, `framelen` the
bits an angle is stored in. Its header is a record a part:

    u8 type, then 4 x {u16 base, u8 bits} when type & 8 (the root's motion,
    skipped by the renderer and read by the game), then 3 x {u16 base, u8 bits}
    when type & 1 (the part's three rotations, each `read(bits) + base` shifted
    up by `16 - framelen`)

which is GoldenEye's own scheme with the root channels moved from their own
stream into the front of every frame. So a converted animation is:

    header    0x09 + GoldenEye's four descriptors + 3 x {0, width},
              then 0x01 + 3 x {0, width} for each of the other fourteen parts
              - 162 bytes, which is what Perfect Dark's own character
              animations are
    a frame   GoldenEye's root-motion bits for that frame, then its frame's
              rotation bits unchanged

Nothing is re-encoded: every bit GoldenEye stored is written out at the width
it stored it, and `framelen` is GoldenEye's own `width` (12).

The root stride is the sum of the four descriptors' bit counts, and the
descriptors are the ones the record's own third word points at - not the block
at record+0x14, which belongs to the *next* animation. Reading them there gave
`bond_eye_walk` the stride of `bond_eye_fire` (19 bits, hip base 1045) and gave
`bond_eye_fire` no root motion at all, so the gun barrel's Bond stopped walking
and dropped to the floor the moment he turned to fire. The walk's own stride is
15 and matches its record's `joints` field; the fire's is 19.
"""
import struct
import gefiles

# GoldenEye's guard skeleton, which every character in the ROM has: sixteen
# joints, the first the header node the animation plays on and the other
# fifteen the parts Perfect Dark's g_SkelChrJoints lists
PARTS = 15


class Bits:
    """A big-endian bit writer, the order both games read frames in."""

    def __init__(self):
        self.out = bytearray()
        self.nbits = 0

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            if self.nbits % 8 == 0:
                self.out.append(0)
            if (value >> i) & 1:
                self.out[-1] |= 0x80 >> (self.nbits % 8)
            self.nbits += 1

    def copy(self, src, bitoff, n):
        for i in range(n):
            self.put((src[(bitoff + i) >> 3] >> (7 - ((bitoff + i) & 7))) & 1, 1)

    def align(self, n):
        while self.nbits % n:
            self.put(0, 1)


def convert(at, parts=PARTS):
    """The animation whose record is at ROM address `at` -> (Perfect Dark
    animation bytes, {numframes, bytesperframe, headerlen, framelen, flags,
    looping}), from the ROM alone.

    `parts` is how many parts the animation moves, which is fifteen for a
    character and **one** for an aircraft: GoldenEye's three vehicle
    animations (animation_table_ptrs2[]) are the four root-motion channels and
    one part's three rotations, 40 bits a frame at width 12, and the model they
    play on is a body with a rotor rather than a skeleton.
    """
    a = gefiles.rom().anim(at)
    width = a['width']
    framebytes = a['bitsperframe'] // 8
    rotbits = 3 * width * parts

    if width == 0 or rotbits > a['bitsperframe'] or not a['numframes']:
        raise ValueError('animation %#x is not a character animation' % at)

    header = bytearray()
    for part in range(parts):
        if part == 0:
            header.append(0x09)
            for _, count, value in a['descriptors']:
                header += struct.pack('>HB', value, count)
        else:
            header.append(0x01)
        for _ in range(3):
            header += struct.pack('>HB', 0, width)

    frames = bytearray()
    for f in range(a['numframes']):
        b = Bits()
        off = 0
        for bitoff, count, _ in a['descriptors']:
            b.copy(a['root'], a['rootbits'] * f + bitoff, count)
            off += count
        b.copy(a['frames'][f * framebytes:(f + 1) * framebytes], 0, rotbits)
        b.align(8)
        frames += b.out

    bytesperframe = (a['rootbits'] + rotbits + 7) // 8
    assert len(frames) == bytesperframe * a['numframes']

    return bytes(header) + bytes(frames), dict(numframes=a['numframes'], bytesperframe=bytesperframe,
                                               headerlen=len(header), framelen=width, flags=0,
                                               looping=a['loop'] & 1)

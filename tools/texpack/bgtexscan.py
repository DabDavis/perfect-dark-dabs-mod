#!/usr/bin/env python3

"""
Finds the texture slots the XBLA release reused for other pictures.

Record N of the release's Textures.raw is texture N of the ROM for the first
NUM_TEXTURES records, except where 4J put a different picture in the slot and
pointed its rewritten rooms at it - 0222 is a police car's light bar in the
ROM and a Villa cliff in the release. Those slots have to be known: the pack
must leave them out so a ROM room or a ROM model keeps its own picture, and
the level loader must serve them to a release room whatever pack is on
(port/include/xblaslots.h, which is the list this produces).

The test is what the rooms bind. This reads a level's rooms the way
bgReset() and bgLoadRoom() do - header, primary, room table, roomgfxdata -
and walks every room's display lists collecting the 0xc0 texture commands,
from the ROM's copy of the file and from the release's. A slot the release's
rooms of a level bind that the ROM's rooms of that same level do not is one
4J repointed; the "dropped" column is the other half of the same move, the
level that used to draw the slot's old picture letting go of it.

Doing it on the files rather than on a running frame covers every room of
every level rather than the rooms one run walked through.

That leaves the slots no room binds at all, which are the ones a *model*
binds - the taxi's "FOR HIRE" sign, the police car's door star. --models
takes the ROM's extracted props/chrs/guns/ob and lists what each model of
interest binds, to be compared picture by picture.

Usage:

    tools/texpack/bgtexscan.py \\
        --rom-bgdata  ~/pd-extract/files/bgdata \\
        --package     "/path/to/Perfect Dark XBLA.7z"

--rom-bgdata is a directory of the ROM's extracted bg_*.seg. --package takes
the 7z, the STFS package inside it, or a PackedSegFile already pulled out;
--xbla-bgdata takes a directory of records dumped from one, named by file id
(31.bin is bgdata/bg_ear.seg), which is quicker to re-run against.
"""

import argparse
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import x360

SEG = 0x0f000000
NUM_TEXTURES = 3503

PACKED_SEG_PATH = 'DataFiles/PackedSegFile'

# One roomblock as the N64 wrote it: u8 type, then four 32 bit pointers with
# the type's three bytes of padding in front of them.
ROOMBLOCK_SIZE = 20
BGROOM_SIZE = 20

LIST_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', '..', 'src', 'assets', 'ntsc-final', 'files', 'list.c')


def file_ids():
    """{file id: 'bg_eld.seg'} for the bg files, from the asset list."""
    out = {}
    for line in open(LIST_C):
        m = re.match(r'\s*/\*(0x[0-9a-f]+)\*/\s*"bgdata/(bg_\w+\.seg)"', line)
        if m:
            out[int(m.group(1), 16)] = m.group(2)
    if not out:
        sys.exit('no bgdata entries in %s' % LIST_C)
    return out


def inflate(buf, off):
    """The 1173 blob at off, or None if what is there is not one."""
    if buf[off:off + 2] != b'\x11\x73':
        return None
    size = (buf[off + 2] << 16) | (buf[off + 3] << 8) | buf[off + 4]
    return zlib.decompressobj(-15).decompress(buf[off + 5:], size)


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def gdl_textures(room, start, release, out):
    """The 0xc0 commands of one display list, up to its G_ENDDL.

    texLoadFromGdl() reads the texture number as the low twelve bits, and
    sixteen for a release room, where the records past the ROM's table need
    the width (subcmd 1 keeps a second texture in the bits over them).
    """
    p = start

    while p + 8 <= len(room):
        cmd = room[p]

        if cmd == 0xb8:
            return

        if cmd == 0xc0:
            w0 = be32(room, p)
            w1 = be32(room, p + 4)
            subcmd = w0 & 7

            out.add((w1 & 0xffff) if release and subcmd != 1 else (w1 & 0xfff))

            if subcmd == 1:
                out.add((w1 >> 12) & 0xfff)

        p += 8


def room_textures(room, roombase, release, out):
    """Every display list of one room, found through its roomblocks."""
    vertices = be32(room, 0)

    if vertices == 0:
        return

    end = vertices - roombase
    pos = 0x18
    gdls = []

    while pos + ROOMBLOCK_SIZE <= min(end, len(room)):
        blocktype = room[pos]
        gdl = be32(room, pos + 8)
        vtx = be32(room, pos + 12)

        if blocktype == 1:
            # A parent block's coords sit between the blocks and the vertices
            if vtx - roombase < end:
                end = vtx - roombase
        elif gdl:
            gdls.append(gdl - roombase)

        pos += ROOMBLOCK_SIZE

    for gdl in gdls:
        if 0 <= gdl < len(room):
            gdl_textures(room, gdl, release, out)


def scan_bg(data, release):
    """The texture slots every room of one bg file binds."""
    infsize, _section1, primcmp = struct.unpack_from('>3I', data, 0)

    # The release stores section 1 inflated; the game copes and so does this
    primary = inflate(data, 0xc)

    if primary is None:
        primary = data[0xc:0xc + primcmp]

    table = be32(primary, 4) - SEG
    bases = []
    i = 0

    while True:
        base = be32(primary, table + i * BGROOM_SIZE)
        bases.append(base)

        if i >= 1 and base == 0:
            break

        i += 1

    # What bgLoadRoom() takes off a room's address to reach it in the file:
    # the rooms follow the *compressed* primary rather than the inflated one
    skew = infsize - primcmp - 0xc

    out = set()
    rooms = 0

    for r in range(1, len(bases) - 1):
        base, nxt = bases[r], bases[r + 1]

        if not base or not nxt or nxt <= base:
            continue

        off = (base - SEG) - skew
        room = inflate(data, off) or data[off:off + (nxt - base)]
        room_textures(room, base, release, out)
        rooms += 1

    return out, rooms


def model_textures(data):
    """The slots a model file binds. Its lists are not walked, so this reads
    every aligned word that looks like a 0xc0 command."""
    out = set()

    for p in range(0, len(data) - 8, 8):
        if data[p] != 0xc0:
            continue

        w0, w1 = struct.unpack_from('>II', data, p)
        subcmd = w0 & 7

        if subcmd > 3:
            continue

        out.add(w1 & 0xfff)

        if subcmd == 1:
            out.add((w1 >> 12) & 0xfff)

    out.discard(0)
    return out


def packed_seg_records(raw):
    """{file id: bytes} for the release's own copies of the files it kept.

    PackedSegFile is {u32 count, count x {offset, uncompressed, compressed,
    flags}, data}, and slot i is the game's file id i + 1.
    """
    count = be32(raw, 0)
    table = 4
    out = {}

    for i in range(count):
        off, usz, csz = struct.unpack_from('>3I', raw, table + i * 16)

        if usz == 0 or usz > 8 * 1024 * 1024:
            continue

        out[i + 1] = (off, usz, csz)

    return out


def read_packed(raw, rec):
    off, usz, csz = rec

    if csz == 0:
        return raw[off:off + usz]

    return x360.lzx_decompress(raw, off, csz, usz)


def load_packed_seg(path):
    with open(path, 'rb') as f:
        head = f.read(8)

    if head[:4] in x360.STFS_MAGICS:
        return x360.STFS(path).extract(PACKED_SEG_PATH)

    if head[:2] == b'7z' or path.lower().endswith(('.7z', '.zip')):
        sys.exit('extract the package from the archive first and pass it, or\n'
                 'pass a directory of dumped records with --xbla-bgdata')

    return open(path, 'rb').read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom-bgdata', required=True,
                    help="a directory of the ROM's extracted bg_*.seg")
    ap.add_argument('--package', help='the XBLA STFS package or its PackedSegFile')
    ap.add_argument('--xbla-bgdata', help='a directory of records dumped from one, <fileid>.bin')
    ap.add_argument('--models', help="a directory of the ROM's extracted models, to list what "
                                     'each binds instead of scanning levels')
    args = ap.parse_args()

    if args.models:
        for name in sorted(os.listdir(args.models)):
            with open(os.path.join(args.models, name), 'rb') as f:
                slots = model_textures(f.read())
            print('%-28s %s' % (name, ' '.join('%04x' % t for t in sorted(slots))))
        return

    if not args.package and not args.xbla_bgdata:
        sys.exit('--package or --xbla-bgdata is needed for the release\'s copies')

    raw = recs = None

    if args.package:
        raw = load_packed_seg(args.package)
        recs = packed_seg_records(raw)

    names = file_ids()
    reused = {}

    for fid in sorted(names):
        rompath = os.path.join(args.rom_bgdata, names[fid])

        if not os.path.exists(rompath):
            continue

        if args.xbla_bgdata:
            relpath = os.path.join(args.xbla_bgdata, '%d.bin' % fid)

            if not os.path.exists(relpath):
                continue

            with open(relpath, 'rb') as f:
                reldata = f.read()
        else:
            if fid not in recs:
                continue

            reldata = read_packed(raw, recs[fid])

        with open(rompath, 'rb') as f:
            romdata = f.read()

        romset, rooms = scan_bg(romdata, False)
        relset, _rooms = scan_bg(reldata, True)

        added = sorted(t for t in relset - romset if 0 < t < NUM_TEXTURES)
        dropped = sorted(romset - relset)

        print('%-14s %3d rooms  added %-44s dropped %s'
              % (names[fid][3:-4], rooms,
                 ' '.join('%04x' % t for t in added) or '-',
                 ' '.join('%04x' % t for t in dropped) or '-'))

        for t in added:
            reused.setdefault(t, []).append(names[fid][3:-4])

    print('\n%d slots the release\'s rooms bind that the ROM\'s rooms of the same '
          'level do not.\nEach one is a candidate: compare the two pictures before '
          'listing it in xblaslots.h,\nsince 4J also retextured surfaces with slots '
          'the level simply had not used.\n' % len(reused))

    for t in sorted(reused):
        print('  %04x  %s' % (t, ' '.join(reused[t])))


if __name__ == '__main__':
    main()

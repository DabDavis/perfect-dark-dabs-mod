#!/usr/bin/env python3

"""
Converts the Xbox 360 XBLA release's textures into a pack this port can load.

The XBLA build is the N64 game with its art replaced, and it kept the game's
own texture numbering: Textures.raw holds one record per texture in texture
number order, so unlike an emulator pack there is nothing to match up and no
checksum to reproduce. Record N is texture N, and the first NUM_TEXTURES of
them are the replacements. The records past that are the console release's own
art - its dashboard panels, its achievement screens - and have no texture
number to go to, so they are left out.

Not every one of those replacements is an upscale. 4J redrew a good part of the
environment art rather than enlarging it, so a wall in the pack may be a
different picture from the wall in the ROM rather than a sharper version of it.
Both come across; which of them is wanted is a matter of taste, and the pack
can be switched off in Extended Options like any other.

Usage:

    tools/texpack/xblaconvert.py \\
        --package "/path/to/Perfect Dark XBLA.7z" \\
        --out ~/.local/share/perfectdark/texture-packs/"PD XBLA"

--package takes the 7z as downloaded, the STFS package inside it, or a
Textures.raw already pulled out of one. Written under texture-packs/ the result
appears in the Texture Pack list in Extended Options, which is also what turns
on Mod.LoadTextures.

Needs Pillow and numpy (pip install pillow numpy), and either the py7zr module
or a 7z command on PATH if it is handed the archive.
"""

import argparse
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import x360

# Texture numbers the game has. ntsc-final and pal-final agree on 3503;
# jpn-final has 3511, but the XBLA release was built from the NTSC data, so its
# records stop where the NTSC list does.
NUM_TEXTURES = 3503

# One texture's record in Textures.raw. There are two tables of this stride:
# the first is the metadata below, the second an array of D3DTexture structs
# whose last six dwords are the GPU fetch constant.
RECORD_SIZE = 52
FETCH_FIELD = 7

PACKAGE_PATH = 'DataFiles/Textures.raw'

# Textures the pack leaves out: the release's copy is a different picture drawn
# for the console's own renderer, not a version of the ROM's, and the game
# draws it wrong. 0x13 is the sky's cloud texture (g_TcSkyWaterConfigs[0]):
# skyRender() lerps sky colour to cloud colour by the texel, and the release's
# 64x64 noise never rises above 122 and is uncorrelated with the ROM's fractal,
# so it flattens the sunset over Crash Site to a plain gradient. The in-game
# conversion (port/src/xblaimport.c, leftOut[]) keeps the same list.
LEFT_OUT = frozenset([0x13])

# Plus the slots the release reused for other pictures (0222 is a Chicago sign
# in the ROM and a Villa cliff in the release): a ROM room binding one wants
# the ROM's picture, and the level loader gives a release room the release's
# whatever pack is on. The list lives in port/include/xblaslots.h, read here
# so the game and this script cannot drift apart.
SLOTS_HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            '..', '..', 'port', 'include', 'xblaslots.h')


def reused_slots():
    try:
        text = open(SLOTS_HEADER).read()
    except OSError:
        sys.exit('%s is needed for the list of reused slots' % SLOTS_HEADER)
    import re
    # The define runs over as many continuation lines as the list needs
    m = re.search(r'#define XBLA_REUSED_SLOTS((?:[^\n]*\\\n)*[^\n]*)', text)
    if not m:
        sys.exit('no XBLA_REUSED_SLOTS in %s' % SLOTS_HEADER)
    return frozenset(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1)))


def read_records(raw):
    """(metadata, fetch constant, data offset) for every texture in the file."""
    count = struct.unpack('>I', raw[:4])[0]
    table_a = 4
    table_b = table_a + count * RECORD_SIZE
    data = table_b + count * RECORD_SIZE

    if data > len(raw):
        sys.exit('Textures.raw is truncated: %d records need %d bytes, file is %d'
                 % (count, data, len(raw)))

    meta, fetch = [], []
    for i in range(count):
        a = struct.unpack('>13I', raw[table_a + i * RECORD_SIZE:
                                      table_a + (i + 1) * RECORD_SIZE])
        b = struct.unpack('>13I', raw[table_b + i * RECORD_SIZE:
                                      table_b + (i + 1) * RECORD_SIZE])
        meta.append(a)
        fetch.append(x360.FetchConstant(b[FETCH_FIELD:FETCH_FIELD + 6]))

    return meta, fetch, data, count


def load_textures_raw(path):
    """Accepts the 7z, the STFS package, or Textures.raw itself."""
    with open(path, 'rb') as f:
        head = f.read(8)

    if head[:4] in x360.STFS_MAGICS:
        return x360.STFS(path).extract(PACKAGE_PATH)

    if head[:2] == b'7z' or path.lower().endswith(('.7z', '.zip')):
        return load_from_archive(path)

    # Textures.raw leads with its record count, which is a few thousand.
    count = struct.unpack('>I', head[:4])[0]
    if 0 < count < 1 << 20:
        with open(path, 'rb') as f:
            return f.read()

    sys.exit('%s is not a 7z, an STFS package or a Textures.raw' % path)


def load_from_archive(path):
    with tempfile.TemporaryDirectory() as tmp:
        try:
            import py7zr
            with py7zr.SevenZipFile(path, 'r') as z:
                z.extractall(tmp)
        except ImportError:
            import shutil
            import subprocess
            exe = shutil.which('7z') or shutil.which('7za') or shutil.which('7zr')
            if not exe:
                sys.exit('Reading the archive needs py7zr (pip install py7zr) or a\n'
                         '7z command. Extract it yourself and pass the package file.')
            r = subprocess.run([exe, 'x', '-y', '-o' + tmp, path],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if r.returncode != 0:
                sys.exit('7z failed: %s' % r.stderr.decode('utf-8', 'replace').strip())

        for root, _dirs, files in os.walk(tmp):
            for name in files:
                full = os.path.join(root, name)
                with open(full, 'rb') as f:
                    if f.read(4) in x360.STFS_MAGICS:
                        return x360.STFS(full).extract(PACKAGE_PATH)

    sys.exit('no STFS package found inside %s' % path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--package', required=True,
                    help='the XBLA 7z, its STFS package, or a Textures.raw')
    ap.add_argument('--out', required=True, help='where to write the pack')
    ap.add_argument('--max-texture', type=int, default=NUM_TEXTURES,
                    help='stop after this texture number (default %d)' % NUM_TEXTURES)
    ap.add_argument('--only-upscales', action='store_true',
                    help='skip textures the release redrew at the original size')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        sys.exit('Pillow is needed to write the pack (pip install pillow).')

    raw = load_textures_raw(args.package)
    meta, fetch, data_start, count = read_records(raw)

    if not args.quiet:
        print('%d texture records; converting %d of them'
              % (count, min(count, args.max_texture)))

    outdir = os.path.join(args.out, 'textures')
    os.makedirs(outdir, exist_ok=True)

    written = skipped = 0
    failures = []
    left_out = LEFT_OUT | reused_slots()

    for n in range(min(count, args.max_texture)):
        offset, width, height, srcw, srch, usize, csize = meta[n][:7]

        if usize == 0 or csize == 0:
            skipped += 1
            continue

        if args.only_upscales and (width, height) == (srcw, srch):
            skipped += 1
            continue

        if n in left_out:
            stale = os.path.join(outdir, '%04x.png' % n)
            if os.path.exists(stale):
                os.remove(stale)
            skipped += 1
            continue

        try:
            surface = x360.lzx_decompress(raw, data_start + offset, csize, usize)
            rgba = x360.decode_texture(surface, width, height, fetch[n])
        except Exception as ex:
            failures.append((n, ex))
            continue

        # The console art is stored in N64 row order, upside down on screen.
        # A pack file carrying our own <texnum>.png name is expected the right
        # way up, and the loader turns it back over - see write_image() in
        # riceconvert.py, which does the same thing for an emulator pack.
        img = Image.fromarray(rgba, 'RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        img.save(os.path.join(outdir, '%04x.png' % n))
        written += 1

        if not args.quiet and written % 250 == 0:
            print('  %d/%d' % (written, min(count, args.max_texture)))

    print('wrote %d textures to %s' % (written, outdir))

    if skipped:
        print('%d records skipped (empty, not an upscale, or left out)' % skipped)

    if failures:
        print('%d records could not be decoded:' % len(failures))
        for n, ex in failures[:10]:
            print('  %04x: %s' % (n, ex))
        if len(failures) > 10:
            print('  ... and %d more' % (len(failures) - 10))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3

"""
Converts a Rice-format high resolution texture pack into the port's own naming.

Emulator packs name their files after a checksum of the original texel bytes,
because an emulator has nothing else to identify a texture by:

    Perfect Dark#1135D097#3#1_all.png
                 ^crc     ^f ^siz

The port does have something else - the texture number - so it reads plain
textures/<texnum>.png and never hashes anything at runtime. This bridges the
two, once, offline: it reproduces the checksum over the game's own texture data
and renames the pack's files accordingly.

Reproducing that checksum needs two things that are easy to get wrong:

  * The bytes must be swizzled the way the N64 wants them. The port skips that
    step deliberately (see texSwizzle() in src/game/texdecompress.c, which is a
    stub on PC), so it has to be put back before hashing.
  * The 32-bit words must be read big-endian, which is the order they have in
    the ROM and the order the emulator's RDRAM presents them in.

Usage:

    # once, to write out every texture's raw data (exits when done)
    ./pd.x86_64 --dump-textures

    tools/texpack/riceconvert.py \\
        --dump ~/.local/share/perfectdark/texturedump/ntsc-final \\
        --pack "/path/to/Perfect Dark Forever 0.4.7z" \\
        --out  ~/.local/share/perfectdark/mypack

Then point the game at the result with --moddir, or copy its textures/ next to
the executable.

--pack takes a directory, a .zip or a .7z. Zip entries are read where they lie;
a .7z is unpacked to a temporary directory first, because its files usually
share one solid compressed block and there is no cheap way to pull one out.

Pillow is needed: emulator packs store their images upside down relative to this
port, so converting has to turn them over rather than copy them. Reading a .7z
needs either the py7zr module or a 7z command on PATH.
"""

import argparse
import contextlib
import csv
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

# The name in a Rice pack is <rom>#<crc>#<fmt>#<siz>[#<palettecrc>]_<kind>.png.
NAME_RE = re.compile(
    r'#(?P<crc>[0-9A-Fa-f]{8})'
    r'#(?P<fmt>[0-9A-Fa-f])'
    r'#(?P<siz>[0-9A-Fa-f])'
    r'(?:#(?P<pal>[0-9A-Fa-f]{8}))?'
    r'_(?P<kind>[A-Za-z]+)\.png$')

# Kinds that already hold the whole image, best first. _rgb needs its _a pair.
WHOLE_IMAGE_KINDS = ('all', 'allciByRGBA', 'ciByRGBA', 'ci')


def words_per_row(width, siz):
    """The row pitch texSwizzle() works in, in 32-bit words."""
    if siz == 3:
        return (width + 3) & 0xffc
    if siz == 2:
        return ((width + 3) & 0xffc) >> 1
    if siz == 1:
        return ((width + 7) & 0xff8) >> 2
    return ((width + 0xf) & 0xff0) >> 3


def swizzle(data, width, height, siz):
    """Puts back the every-other-row word swap the PC port leaves out."""
    wpr = words_per_row(width, siz)
    out = bytearray(data)
    step = 4 if siz == 3 else 2

    for y in range(1, height, 2):
        base = y * wpr * 4
        for x in range(0, wpr, step):
            for k in range(step // 2):
                a = base + (x + k) * 4
                b = base + (x + k + step // 2) * 4
                if b + 4 <= len(out):
                    out[a:a + 4], out[b:b + 4] = out[b:b + 4], out[a:a + 4]

    return bytes(out)


def rice_crc32(data, width, height, siz, stride):
    """
    Rice's checksum, as GlideHQ computes it - see TxUtil::RiceCRC32().

    Rows are walked forwards while the counter mixed into each one runs
    backwards, and each row is read from its end towards its start in 32-bit
    steps. word_hash deliberately survives between rows: a row too narrow for a
    single word contributes the previous row's value, and packs were built
    against that.
    """
    bytes_per_width = ((width << siz) + 1) >> 1
    crc = 0
    word_hash = 0
    row = 0

    for counter in range(height - 1, -1, -1):
        pos = bytes_per_width - 4
        while pos >= 0:
            if row + pos + 4 > len(data):
                return None
            word = int.from_bytes(data[row + pos:row + pos + 4], 'big')
            word_hash = (pos ^ word) & 0xffffffff
            crc = (word_hash + (((crc << 4) | (crc >> 28)) & 0xffffffff)) & 0xffffffff
            pos -= 4
        crc = (crc + (counter ^ word_hash)) & 0xffffffff
        row += stride

    return crc


def read_dump(dumpdir):
    """Maps every checksum the game's own textures produce to a texture number."""
    manifest = os.path.join(dumpdir, 'manifest.csv')

    if not os.path.exists(manifest):
        sys.exit(f'{manifest} not found - run the game with --dump-textures first')

    crcs = {}
    collisions = 0

    with open(manifest) as f:
        for row in csv.DictReader(f):
            texnum = row['texnum']
            siz = int(row['siz'])
            width = int(row['tilewidth'])
            height = int(row['tileheight'])
            stride = int(row['linesize'])

            try:
                with open(os.path.join(dumpdir, texnum + '.raw'), 'rb') as rf:
                    raw = rf.read()
            except FileNotFoundError:
                continue

            if width <= 0 or height <= 0 or stride <= 0:
                continue

            crc = rice_crc32(swizzle(raw, width, height, siz), width, height, siz, stride)

            if crc is None:
                continue

            if crc in crcs:
                # Two textures with identical bytes. Either would draw the same,
                # so the first is as good an answer as the second.
                collisions += 1
                continue

            crcs[crc] = texnum

    return crcs, collisions


class DirSource:
    """A pack that is already a directory on disk."""

    def __init__(self, path):
        self.path = path

    def names(self):
        for root, _dirs, files in os.walk(self.path):
            for name in files:
                yield os.path.join(root, name)

    def read(self, name):
        with open(name, 'rb') as f:
            return f.read()

    def close(self):
        pass


class ZipSource:
    """
    A .zip, read where it lies.

    Each entry is deflated on its own, so pulling one out costs only that
    entry - no reason to unpack the whole thing first.
    """

    def __init__(self, path):
        self.zf = zipfile.ZipFile(path)

    def names(self):
        return [i.filename for i in self.zf.infolist() if not i.is_dir()]

    def read(self, name):
        return self.zf.read(name)

    def close(self):
        self.zf.close()


def _extract_7z(path, dest):
    """
    Unpacks a .7z, by whatever means are to hand.

    py7zr if it is installed, otherwise a 7z command. Unlike zip there is no
    cheap way to read one entry: the files usually share a single solid block,
    so anything short of unpacking the archive decompresses most of it anyway.
    """
    try:
        import py7zr
    except ImportError:
        pass
    else:
        with py7zr.SevenZipFile(path, 'r') as z:
            z.extractall(dest)
        return

    for exe in ('7z', '7za', '7zr'):
        if shutil.which(exe):
            result = subprocess.run([exe, 'x', '-y', '-o' + dest, path],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if result.returncode != 0:
                sys.exit(f'{exe} could not read {path}:\n'
                         + result.stderr.decode(errors='replace').strip())
            return

    sys.exit(f'{path} is a .7z and there is nothing here that can read one.\n'
             'Install py7zr (pip install py7zr) or p7zip, or extract it yourself\n'
             'and pass the directory to --pack.')


class SevenZipSource(DirSource):
    """A .7z, unpacked to a temporary directory and then read as one."""

    def __init__(self, path):
        self.tmp = tempfile.TemporaryDirectory(prefix='riceconvert-')
        print(f'unpacking {os.path.basename(path)}...', flush=True)
        _extract_7z(path, self.tmp.name)
        super().__init__(self.tmp.name)

    def close(self):
        self.tmp.cleanup()


def open_pack(path):
    """Opens a pack directory, .zip or .7z, whichever it turns out to be."""
    if os.path.isdir(path):
        return DirSource(path)

    if not os.path.exists(path):
        sys.exit(f'{path} does not exist')

    ext = os.path.splitext(path)[1].lower()

    if ext == '.7z':
        return SevenZipSource(path)

    if ext in ('.zip', '.pk3'):
        return ZipSource(path)

    sys.exit(f'{path} is neither a directory nor an archive this can open')


def group_pack(source):
    """Collects a pack's entries by checksum, keeping every kind found for each."""
    groups = {}

    for name in source.names():
        m = NAME_RE.search(os.path.basename(name))
        if m:
            key = int(m.group('crc'), 16)
            groups.setdefault(key, {})[m.group('kind')] = name

    return groups


def _pil():
    try:
        from PIL import Image
    except ImportError:
        sys.exit('Pillow is needed to convert a pack (pip install pillow).\n'
                 'Emulator packs store their images upside down relative to this\n'
                 'port, so the files have to be turned over rather than copied.')
    return Image


def write_image(data, out_path):
    """
    Writes one pack image out the way this port wants it.

    Turned over, because the two conventions disagree. GlideHQ dumps a texture
    in raw N64 row order, which is upside down on screen, and pack artists have
    always edited them that way; the port's own dumps are written the right way
    up so they can be opened in an image editor. The game knows the difference
    from the filename, and a converted file carries our name.
    """
    Image = _pil()
    im = Image.open(io.BytesIO(data))
    im = im.convert('RGBA') if im.mode not in ('RGB', 'RGBA') else im
    im.transpose(Image.FLIP_TOP_BOTTOM).save(out_path)


def combine_rgb_alpha(rgb_bytes, alpha_bytes, out_path):
    """Merges a pack's separate colour and alpha images into one RGBA file."""
    Image = _pil()

    rgb = Image.open(io.BytesIO(rgb_bytes)).convert('RGB')
    alpha = Image.open(io.BytesIO(alpha_bytes)).convert('L')

    if alpha.size != rgb.size:
        alpha = alpha.resize(rgb.size, Image.LANCZOS)

    out = rgb.convert('RGBA')
    out.putalpha(alpha)
    out.transpose(Image.FLIP_TOP_BOTTOM).save(out_path)

    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', required=True, help='texturedump/<romid> from --dump-textures')
    ap.add_argument('--pack', required=True,
            help='the Rice pack: a directory, a .zip or a .7z')
    ap.add_argument('--out', required=True, help='where to write the converted pack')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    crcs, collisions = read_dump(args.dump)
    print(f'{len(crcs)} distinct textures in the dump'
          + (f' ({collisions} shared their bytes with another)' if collisions else ''))

    source = open_pack(args.pack)

    with contextlib.closing(source):
        groups = group_pack(source)
        print(f'{len(groups)} distinct textures in the pack')

        outdir = os.path.join(args.out, 'textures')
        os.makedirs(outdir, exist_ok=True)

        converted = 0
        unmatched = 0

        for crc, kinds in sorted(groups.items()):
            texnum = crcs.get(crc)

            if texnum is None:
                unmatched += 1
                continue

            out_path = os.path.join(outdir, f'{texnum}.png')
            src = next((kinds[k] for k in WHOLE_IMAGE_KINDS if k in kinds), None)

            if src:
                write_image(source.read(src), out_path)
                converted += 1
            elif 'rgb' in kinds and 'a' in kinds:
                combine_rgb_alpha(source.read(kinds['rgb']), source.read(kinds['a']), out_path)
                converted += 1
            elif 'rgb' in kinds:
                write_image(source.read(kinds['rgb']), out_path)
                converted += 1
            else:
                unmatched += 1

    print(f'\nwrote {converted} textures to {outdir}')

    if unmatched:
        print(f'{unmatched} pack textures had no match in the dump')
        print('  Usually mip levels, or textures from a different ROM revision '
              'than the one\n  the dump came from. They are left out rather than guessed at.')


if __name__ == '__main__':
    main()

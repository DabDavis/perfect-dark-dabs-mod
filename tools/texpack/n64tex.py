#!/usr/bin/env python3

"""
Turns a --dump-textures dump into RGBA images.

The game writes each texture's texels as they sit in the ROM, plus a manifest
saying how to read them. This does what the renderer does with them, so the
whole catalogue can be looked at or fed to an upscaler without the game having
to draw every texture first.

The conversions match import_texture_* in port/fast3d/gfx_pc.cpp; where the two
disagree, that file is right.
"""

import csv
import os

# G_IM_FMT_*
FMT_RGBA, FMT_YUV, FMT_CI, FMT_IA, FMT_I = 0, 1, 2, 3, 4

# G_IM_SIZ_*
SIZ_4, SIZ_8, SIZ_16, SIZ_32 = 0, 1, 2, 3

FMT_NAMES = {FMT_RGBA: 'rgba', FMT_YUV: 'yuv', FMT_CI: 'ci', FMT_IA: 'ia', FMT_I: 'i'}
SIZ_NAMES = {SIZ_4: '4', SIZ_8: '8', SIZ_16: '16', SIZ_32: '32'}

# lutmodeindex: 2 is RGBA16 entries, 3 is IA16
LUT_RGBA16, LUT_IA16 = 2, 3


def _s5(v):
    return (v * 0xff) // 0x1f


def _s4(v):
    return v * 0x11


def _s3(v):
    return v * 0x24


def _entry_to_rgba(entry, lutmode):
    """One 16-bit palette entry, big-endian, as RGBA."""
    if lutmode == LUT_IA16:
        intensity = entry & 0xff
        return (intensity, intensity, intensity, entry >> 8)

    return (_s5(entry >> 11), _s5((entry >> 6) & 0x1f), _s5((entry >> 1) & 0x1f),
            255 if entry & 1 else 0)


def texels_to_rgba(data, width, height, fmt, siz, stride, palette=None, lutmode=LUT_RGBA16):
    """
    Expands one texture's texels to a flat RGBA bytearray, row by row.

    stride is the distance between rows in the data, which is not always the
    width in bytes: the RDP pads a row out to a multiple of eight bytes, and a
    32-bit texel is split across the two halves of TMEM.
    """
    out = bytearray(width * height * 4)
    pal = palette or []

    for y in range(height):
        row = y * stride
        o = y * width * 4

        for x in range(width):
            if fmt == FMT_RGBA and siz == SIZ_16:
                i = row + x * 2
                if i + 1 >= len(data):
                    break
                c = (data[i] << 8) | data[i + 1]
                px = (_s5(c >> 11), _s5((c >> 6) & 0x1f), _s5((c >> 1) & 0x1f),
                      255 if c & 1 else 0)
            elif fmt == FMT_RGBA and siz == SIZ_32:
                # Stored byte-swapped within each texel; the renderer undoes it
                # with PD_BE32, and preprocessModelTextures() swaps embedded
                # ones to match.
                i = row + x * 4
                if i + 3 >= len(data):
                    break
                px = (data[i + 3], data[i + 2], data[i + 1], data[i])
            elif fmt == FMT_IA and siz == SIZ_16:
                i = row + x * 2
                if i + 1 >= len(data):
                    break
                px = (data[i], data[i], data[i], data[i + 1])
            elif fmt == FMT_IA and siz == SIZ_8:
                i = row + x
                if i >= len(data):
                    break
                v = data[i]
                c = _s4(v >> 4)
                px = (c, c, c, _s4(v & 0xf))
            elif fmt == FMT_IA and siz == SIZ_4:
                i = row + x // 2
                if i >= len(data):
                    break
                part = (data[i] >> (4 - (x % 2) * 4)) & 0xf
                c = _s3(part >> 1)
                px = (c, c, c, 255 if part & 1 else 0)
            elif fmt == FMT_I and siz == SIZ_8:
                i = row + x
                if i >= len(data):
                    break
                v = data[i]
                px = (v, v, v, v)
            elif fmt == FMT_I and siz == SIZ_4:
                i = row + x // 2
                if i >= len(data):
                    break
                v = _s4((data[i] >> (4 - (x % 2) * 4)) & 0xf)
                px = (v, v, v, v)
            elif fmt == FMT_CI and siz == SIZ_8:
                i = row + x
                if i >= len(data):
                    break
                idx = data[i]
                px = _entry_to_rgba(pal[idx], lutmode) if idx < len(pal) else (0, 0, 0, 0)
            elif fmt == FMT_CI and siz == SIZ_4:
                i = row + x // 2
                if i >= len(data):
                    break
                idx = (data[i] >> (4 - (x % 2) * 4)) & 0xf
                px = _entry_to_rgba(pal[idx], lutmode) if idx < len(pal) else (0, 0, 0, 0)
            else:
                return None

            out[o:o + 4] = bytes(px)
            o += 4

    return out


def read_palette(path):
    with open(path, 'rb') as f:
        raw = f.read()
    return [(raw[i] << 8) | raw[i + 1] for i in range(0, len(raw) - 1, 2)]


def load_dump(dumpdir):
    """
    Yields (texnum, name, width, height, rgba_bytes) for every texture in a dump.

    name is <texnum>_<fmt><siz>.png, the same shape the in-game dumper writes, so
    the two can be mixed in one pack directory.
    """
    with open(os.path.join(dumpdir, 'manifest.csv')) as f:
        rows = list(csv.DictReader(f))

    for r in rows:
        texnum = r['texnum']
        fmt, siz = int(r['fmt']), int(r['siz'])
        stride, size = int(r['linesize']), int(r['size'])

        if stride <= 0 or size <= 0:
            continue

        # The renderer uploads the padded row, not the tile: an RDP row is a
        # whole number of 64-bit words, so a 44 wide 4-bit texture is uploaded
        # 48 across. A replacement has to be the same shape, because that is
        # what the texture coordinates are normalised against - so the size
        # here comes from the row pitch, exactly as import_texture_* does it.
        line = stride // 2 if siz == SIZ_32 else stride
        width = line * 2 if siz == SIZ_4 else line if siz == SIZ_8 else line // 2
        height = size // stride

        if width <= 0 or height <= 0:
            continue

        try:
            with open(os.path.join(dumpdir, texnum + '.raw'), 'rb') as f:
                data = f.read()
        except FileNotFoundError:
            continue

        palette = None
        lutmode = int(r.get('lutmode') or 0)

        if lutmode:
            palfile = os.path.join(dumpdir, texnum + '.pal')
            if os.path.exists(palfile):
                palette = read_palette(palfile)

        rgba = texels_to_rgba(data, width, height, fmt, siz, stride, palette, lutmode)

        if rgba is None:
            continue

        # Bottom row first, which is how the in-game dumper writes them and so
        # the right way up in an image editor - the game's texture data runs the
        # other way. See texpackDumpTexture().
        rowbytes = width * 4
        flipped = bytearray(len(rgba))

        for y in range(height):
            src = (height - 1 - y) * rowbytes
            flipped[y * rowbytes:(y + 1) * rowbytes] = rgba[src:src + rowbytes]

        name = f"{texnum}_{FMT_NAMES.get(fmt, 'fmt')}{SIZ_NAMES.get(siz, '?')}.png"
        yield texnum, name, width, height, flipped

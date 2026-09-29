#!/usr/bin/env python3
"""
Bake M PLUS Rounded 1c into Perfect Dark's glyph format for Japanese.

Output: port/src/langfont_cjk.c + port/include/langfont_cjk.h, generated.
Regenerate (from the repository root):

    python3 tools/langfont/bake.py [--extra-from lang/ja] [--sheet DIR]

## What a glyph is

Perfect Dark's fonts are CI4, 16 texels to a row (8 bytes), height + 2 rows,
drawn through the two banks of var8007fb5c (src/game/game_1531a0.c):

    indices 9-15   body, alpha 0x18 .. 0xff in the body bank
    indices 1-7    the outline band, alpha 0x58 .. 0xff in the outline bank
                   (8-15 are opaque there too, so the body sits on its band)

Texel (1, 1) is the cell's top left; row 0 and column 0 are the band's
margin. The band is the body grown one texel: a texel next to the body
(left, right, above, below) takes the body's level, one only touching it
corner-on takes a little over half of it - the ROM's '|' has 7 beside its
stroke and 4 at the corners.

Two size classes, as Rare's own JP font had:

    small  12 columns x 11 rows, em 11 px  (xs, sm, md)
    big    14 columns x 15 rows, em 14 px  (lg; 14 is all a 16 texel row
                                          holds with a texel of band each side)

The width is the advance as well: the small class keeps a column free after
an 11 px em so a run of kanji does not merge (at a 12 px em in 12 columns it
did); the big one's em fills its cell, and its kana and punctuation leave
their own gap.

## What is stored

Only the body, as levels 0-7 (0 none, n -> index 8 + n) at BPP bits a texel,
cell sized, packed MSB first. The default rendering is FreeType's hinted
monochrome, which is what reads at 11-12 px (grey anti-aliasing smears a
complex kanji into a blot at this size), so BPP is 1 and the table is about
44 bytes a character (both classes). The C side grows the band and unpacks CI4 (and I8 for
GE Plus's front end) the first time a character is asked for, into a buffer
that then stays put: the renderer caches textures by address.
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FONTDIR = os.path.join(HERE, 'fonts')
FONTFILE = 'MPLUSRounded1c-%s.ttf'

# body bank alphas of indices 8 + n, n = 0..7 (var8007fb5c bank 1)
BODY_ALPHA = [0x00, 0x18, 0x30, 0x5c, 0x88, 0xb4, 0xd8, 0xff]
# outline bank alphas of indices 0..7 (var8007fb5c bank 0)
BAND_ALPHA = [0x00, 0x58, 0x74, 0x90, 0xac, 0xc8, 0xe4, 0xff]

# name: (rows, columns, em px, top offset)
CLASSES = {
    'small': dict(rows=11, cols=12, px=11, top=0),
    'big': dict(rows=15, cols=14, px=14, top=0),
}

# Japanese punctuation and forms beyond JIS X 0208 that texts use
EXTRA = (
    '〜～'          # wave dash, full-width tilde
    '―—–'    # dashes
    '−－'          # minus signs
    '…‥'          # ellipses
    '・･'          # middle dots
    '「」『』【】〈〉《》〔〕'
    '（）［］｛｝'
    '、。，．！？：；'
    '゛゜ーヽヾゝゞ々〆〇'
    'ヴヵヶゔゕゖヷヸヹヺ'
    '￥￠￡￢×÷★☆○●△▲□■→←↑↓'
    '　'
)


def jis_rows(rows):
    """Every character JIS X 0208 has in the given rows (1-based ku)."""
    out = []
    for ku in rows:
        for ten in range(1, 95):
            b = bytes([0xa0 + ku, 0xa0 + ten])
            try:
                s = b.decode('euc_jp')
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.append(s)
    return out


def scan_extra_dir(path):
    """Every non-ASCII code point in the JSON files under path."""
    found = set()
    for dirpath, _, files in os.walk(path):
        for name in files:
            if not name.endswith('.json'):
                continue
            with open(os.path.join(dirpath, name), encoding='utf-8') as fd:
                text = fd.read()
            try:
                # decode escapes like あ the way the game will see them
                strings = []

                def walk(v):
                    if isinstance(v, str):
                        strings.append(v)
                    elif isinstance(v, dict):
                        for k, x in v.items():
                            walk(k)
                            walk(x)
                    elif isinstance(v, list):
                        for x in v:
                            walk(x)
                walk(json.loads(text))
                text = ''.join(strings)
            except ValueError:
                print('warning: %s is not JSON, scanning its text' % name, file=sys.stderr)
            found.update(c for c in text if ord(c) >= 0x80)
    return found


def read_extra_file(path):
    """Characters, or U+XXXX / 0xXXXX tokens, one or many a line; # comments."""
    found = set()
    with open(path, encoding='utf-8') as fd:
        for line in fd:
            line = line.split('#', 1)[0].strip()
            for tok in line.split():
                t = tok.upper()
                if t.startswith('U+') or t.startswith('0X'):
                    found.add(chr(int(t[2:], 16)))
                else:
                    found.update(c for c in tok if ord(c) >= 0x80)
    return found


def build_charset(args, cmap):
    chars = set()
    chars.update(jis_rows([1, 2, 3, 4, 5]))        # symbols, digits, Latin, kana
    chars.update(jis_rows(range(16, 48)))           # level 1 kanji
    chars.update(EXTRA)
    for path in args.extra or []:
        chars.update(read_extra_file(path))
    for path in args.extra_from or []:
        chars.update(scan_extra_dir(path))

    out = []
    for c in sorted(chars):
        cp = ord(c)
        if cp < 0x80:
            continue
        if cp > 0xffff:
            print('warning: U+%X is outside the BMP, skipped' % cp, file=sys.stderr)
            continue
        if cp not in cmap:
            if not args.quiet:
                print('note: the font has no U+%04X %s, skipped' % (cp, c), file=sys.stderr)
            continue
        out.append(cp)
    return out


class Renderer:
    def __init__(self, weight, cls, mode):
        self.cls = cls
        self.mode = mode
        self.font = ImageFont.truetype(os.path.join(FONTDIR, FONTFILE % weight), cls['px'])
        self.ascent = 0.86  # typo ascender of the 1000 unit em
        self.clipped = []

    def body(self, cp):
        """Levels 0-7, rows x cols."""
        rows, cols, px, top = self.cls['rows'], self.cls['cols'], self.cls['px'], self.cls['top']
        pad = 8
        im = Image.new('L', (cols + pad * 2, rows + pad * 2), 0)
        d = ImageDraw.Draw(im)
        if self.mode == 'mono':
            d.fontmode = '1'
        x = pad + (cols - px) // 2
        y = pad + top + round(self.ascent * px)
        d.text((x, y), chr(cp), font=self.font, fill=255, anchor='ls')
        a = np.asarray(im, dtype=np.float64) / 255.0
        # ink past an edge of the cell (a dakuten past the advance, a dash
        # wider than the em) is slid back in while the far side has room
        ys, xs = np.nonzero(a > 0.02)
        dx = dy = 0
        if len(xs):
            x1, x2 = xs.min() - pad, xs.max() - pad
            y1, y2 = ys.min() - pad, ys.max() - pad
            if x2 >= cols:
                dx = -min(x2 - cols + 1, max(x1, 0))
            elif x1 < 0:
                dx = min(-x1, max(cols - 1 - x2, 0))
            if y2 >= rows:
                dy = -min(y2 - rows + 1, max(y1, 0))
            elif y1 < 0:
                dy = min(-y1, max(rows - 1 - y2, 0))
        cell = a[pad - dy:pad - dy + rows, pad - dx:pad - dx + cols]
        if a.sum() - cell.sum() > 0.5:
            self.clipped.append(cp)
        if self.mode == 'mono':
            return np.where(cell > 0.5, 7, 0).astype(np.uint8)
        # grey: nearest body alpha, with a little contrast
        c = np.clip((cell - 0.08) / 0.8, 0, 1) * 255
        lv = np.abs(c[..., None] - np.array(BODY_ALPHA)[None, None, :]).argmin(axis=2)
        return lv.astype(np.uint8)


def band_of(body):
    """The outline index of every texel of a body padded by one each side.
    Must match langfontCjkUnpack() in the generated C."""
    rows, cols = body.shape
    b = np.zeros((rows + 2, cols + 2), dtype=np.int32)
    b[1:-1, 1:-1] = body
    p = np.pad(b, 1)
    ortho = np.maximum.reduce([p[:-2, 1:-1], p[2:, 1:-1], p[1:-1, :-2], p[1:-1, 2:]])
    diag = np.maximum.reduce([p[:-2, :-2], p[:-2, 2:], p[2:, :-2], p[2:, 2:]])
    band = np.maximum(ortho, (diag * 4 + 3) // 7)
    ci = np.where(b > 0, 8 + b, band)
    return ci  # (rows + 2) x (cols + 2)


def ci4_bytes(ci):
    """16 texels a row, height + 2 rows."""
    h, w = ci.shape
    full = np.zeros((h, 16), dtype=np.uint8)
    full[:, :w] = ci
    return bytes(((full[:, 0::2] << 4) | full[:, 1::2]).flatten())


def pack_bits(levels, bpp):
    bits = []
    for v in levels.flatten():
        for i in range(bpp - 1, -1, -1):
            bits.append((int(v) >> i) & 1)
    while len(bits) % 8:
        bits.append(0)
    out = bytearray()
    for i in range(0, len(bits), 8):
        byte = 0
        for bit in bits[i:i + 8]:
            byte = (byte << 1) | bit
        out.append(byte)
    return bytes(out)


def to_level_bits(body, bpp):
    if bpp == 1:
        return (body > 0).astype(np.uint8)
    return body


# ---------------------------------------------------------------- contact sheet

def composite(ci, colour=(0x40, 0xff, 0x40), bg=(0x28, 0x30, 0x48)):
    """How the outline pass then the body pass leave a glyph on a backdrop."""
    h, w = ci.shape
    img = np.zeros((h, w, 3)) + np.array(bg, dtype=np.float64)
    band = np.array([BAND_ALPHA[i] if i < 8 else 255 for i in range(16)]) / 255.0
    body = np.array([0] * 8 + BODY_ALPHA) / 255.0
    a0 = band[ci][..., None]
    a1 = body[ci][..., None]
    img = img * (1 - a0)
    img = img * (1 - a1) + np.array(colour) * a1
    return img


def sheet(path, lines, glyphs, cls_name, scale):
    """lines: list of lists of code points; glyphs[cp] = ci array."""
    cls = CLASSES[cls_name]
    cw, rh = cls['cols'], cls['rows'] + 4
    width = max(len(l) for l in lines) * cw + 4
    img = np.zeros((len(lines) * rh + 4, width, 3)) + np.array((0x28, 0x30, 0x48))
    for li, line in enumerate(lines):
        x = 2
        y = 2 + li * rh
        for cp in line:
            if cp in glyphs:
                ci = glyphs[cp]
                h, w = ci.shape
                # draw at x - 1, y - 1 like text0f156a24, band over the neighbour
                sub = img[y:y + h, x:x + w]
                comp = composite(ci)
                band = np.array([BAND_ALPHA[i] if i < 8 else 255 for i in range(16)])[ci]
                mask = (band > 0)[..., None]
                sub[:] = np.where(mask, comp, sub)
            x += cw
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    if scale != 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    im.save(path)


# ---------------------------------------------------------------------- C out

HEADER = '''\
// Generated by tools/langfont/bake.py - do not edit.
// M PLUS Rounded 1c (SIL Open Font License 1.1, tools/langfont/fonts/OFL.txt),
// baked into Perfect Dark's glyph format. See tools/langfont/README.md.
#ifndef _IN_LANGFONT_CJK_H
#define _IN_LANGFONT_CJK_H

#include <PR/ultratypes.h>

#define LANGFONT_CJK_COUNT {count}

// cell sizes: small for xs/sm/md, big for lg
#define LANGFONT_CJK_SMALL_W {sw}
#define LANGFONT_CJK_SMALL_H {sh}
#define LANGFONT_CJK_BIG_W {bw}
#define LANGFONT_CJK_BIG_H {bh}

/**
 * The glyph of code point cp, or NULL if the font has none.
 *
 * CI4 in the ROM fonts' layout: 16 texels (8 bytes) a row, *height + 2 rows,
 * body in indices 9-15 and the outline band in 1-7 (var8007fb5c); texel
 * (1, 1) is the cell's top left, so it is drawn at x - 1, y - 1 over
 * *width + 2 columns like any ROM glyph. *width is the advance too (the font
 * is monospaced and the gap is inside the cell). *baseline is the fontchar
 * field: rows from the line's top to the cell's.
 *
 * big selects the 15 row class (the lg font); otherwise 11 rows.
 * The pointer stays valid and unchanged for the life of the program.
 */
const u8 *langfontCjkGlyph(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline);

/**
 * The same glyph as I8 for GE Plus's front end (struct gefont): body alone,
 * no band, rows of (*width + 7) & ~7 bytes, *height rows, 0x00-0xff.
 */
const u8 *langfontCjkGlyphI8(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline);

// whether the font has cp
s32 langfontCjkHas(u32 cp);

#endif
'''

SOURCE_HEAD = '''\
// Generated by tools/langfont/bake.py - do not edit.
// M PLUS Rounded 1c, Copyright 2016 The Rounded M+ Project Authors, under the
// SIL Open Font License 1.1 (tools/langfont/fonts/OFL.txt). The glyphs below
// are a bitmap rendering of that font; see tools/langfont/README.md.
// {count} characters: {desc}
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "langfont_cjk.h"

#define BPP {bpp}
#define SMALL_W {sw}
#define SMALL_H {sh}
#define SMALL_STRIDE {sstride}
#define BIG_W {bw}
#define BIG_H {bh}
#define BIG_STRIDE {bstride}

'''

SOURCE_TAIL = r'''
static const u8 s_BodyAlpha[8] = { 0x00, 0x18, 0x30, 0x5c, 0x88, 0xb4, 0xd8, 0xff };

// unpacked glyphs by index: CI4 small, CI4 big, I8 small, I8 big
static u8 **s_Unpacked[4];

static s32 langfontCjkFind(u32 cp)
{
	s32 lo = 0;
	s32 hi = LANGFONT_CJK_COUNT - 1;

	if (cp > 0xffff) {
		return -1;
	}

	while (lo <= hi) {
		const s32 mid = (lo + hi) / 2;

		if (s_CodePoints[mid] == cp) {
			return mid;
		}

		if (s_CodePoints[mid] < cp) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return -1;
}

s32 langfontCjkHas(u32 cp)
{
	return langfontCjkFind(cp) >= 0;
}

/**
 * The body levels (0-7) of glyph i, into body[(h + 2) * (w + 2)] with a
 * margin of one all round.
 */
static void langfontCjkBody(s32 i, s32 big, u8 *body)
{
	const s32 w = big ? BIG_W : SMALL_W;
	const s32 h = big ? BIG_H : SMALL_H;
	const u8 *src = big ? &s_Big[i * BIG_STRIDE] : &s_Small[i * SMALL_STRIDE];
	u32 bit = 0;
	s32 x, y, k;

	memset(body, 0, (w + 2) * (h + 2));

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			u32 v = 0;

			for (k = 0; k < BPP; k++, bit++) {
				v = (v << 1) | ((src[bit >> 3] >> (7 - (bit & 7))) & 1);
			}

#if BPP == 1
			v *= 7;
#endif
			body[(y + 1) * (w + 2) + x + 1] = (u8) v;
		}
	}
}

/**
 * CI4, the band grown one texel round the body: the body's level beside it,
 * (level * 4 + 3) / 7 corner-on. Keep with band_of() in bake.py.
 */
static u8 *langfontCjkUnpack(s32 i, s32 big)
{
	const s32 w = (big ? BIG_W : SMALL_W) + 2;
	const s32 h = (big ? BIG_H : SMALL_H) + 2;
	u8 body[(BIG_W + 2) * (BIG_H + 2)];
	u8 *out = calloc(h, 8);
	s32 x, y;

	if (!out) {
		return NULL;
	}

	langfontCjkBody(i, big, body);

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
#define B(xx, yy) (((xx) < 0 || (yy) < 0 || (xx) >= w || (yy) >= h) ? 0 : body[(yy) * w + (xx)])
			s32 ci = B(x, y);

			if (ci) {
				ci += 8;
			} else {
				s32 ortho = B(x - 1, y);
				s32 diag = B(x - 1, y - 1);

				if (B(x + 1, y) > ortho) ortho = B(x + 1, y);
				if (B(x, y - 1) > ortho) ortho = B(x, y - 1);
				if (B(x, y + 1) > ortho) ortho = B(x, y + 1);
				if (B(x + 1, y - 1) > diag) diag = B(x + 1, y - 1);
				if (B(x - 1, y + 1) > diag) diag = B(x - 1, y + 1);
				if (B(x + 1, y + 1) > diag) diag = B(x + 1, y + 1);

				diag = (diag * 4 + 3) / 7;
				ci = ortho > diag ? ortho : diag;
			}
#undef B

			out[y * 8 + x / 2] |= (x & 1) ? ci : ci << 4;
		}
	}

	return out;
}

static u8 *langfontCjkUnpackI8(s32 i, s32 big)
{
	const s32 w = big ? BIG_W : SMALL_W;
	const s32 h = big ? BIG_H : SMALL_H;
	const s32 pitch = (w + 7) & ~7;
	u8 body[(BIG_W + 2) * (BIG_H + 2)];
	u8 *out = calloc(h, pitch);
	s32 x, y;

	if (!out) {
		return NULL;
	}

	langfontCjkBody(i, big, body);

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			out[y * pitch + x] = s_BodyAlpha[body[(y + 1) * (w + 2) + x + 1]];
		}
	}

	return out;
}

static const u8 *langfontCjkCached(s32 i, s32 big, s32 i8)
{
	u8 ***slot = &s_Unpacked[i8 * 2 + big];

	if (!*slot) {
		*slot = calloc(LANGFONT_CJK_COUNT, sizeof(u8 *));

		if (!*slot) {
			return NULL;
		}
	}

	if (!(*slot)[i]) {
		(*slot)[i] = i8 ? langfontCjkUnpackI8(i, big) : langfontCjkUnpack(i, big);
	}

	return (*slot)[i];
}

static s32 langfontCjkMetrics(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline)
{
	const s32 i = langfontCjkFind(cp);

	big = big ? 1 : 0;

	if (width) *width = big ? BIG_W : SMALL_W;
	if (height) *height = big ? BIG_H : SMALL_H;
	if (baseline) *baseline = 0;

	return i;
}

const u8 *langfontCjkGlyph(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline)
{
	const s32 i = langfontCjkMetrics(cp, big, width, height, baseline);

	if (i < 0) {
		return NULL;
	}

	return langfontCjkCached(i, big ? 1 : 0, 0);
}

const u8 *langfontCjkGlyphI8(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline)
{
	const s32 i = langfontCjkMetrics(cp, big, width, height, baseline);

	if (i < 0) {
		return NULL;
	}

	return langfontCjkCached(i, big ? 1 : 0, 1);
}
'''


def c_bytes(name, data, per_line=24):
    out = ['static const u8 %s[%d] = {' % (name, len(data))]
    for i in range(0, len(data), per_line):
        out.append('\t' + ', '.join('0x%02x' % b for b in data[i:i + per_line]) + ',')
    out.append('};\n')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--weight-small', default='Regular')
    ap.add_argument('--weight-big', default='Regular')
    ap.add_argument('--mode', choices=['mono', 'grey'], default='mono')
    ap.add_argument('--extra', action='append', help='file of extra characters or U+XXXX')
    ap.add_argument('--extra-from', action='append', help='directory of JSON (a lang pack) to scan')
    ap.add_argument('--out-c', default=os.path.join(ROOT, 'port', 'src', 'langfont_cjk.c'))
    ap.add_argument('--out-h', default=os.path.join(ROOT, 'port', 'include', 'langfont_cjk.h'))
    ap.add_argument('--sheet', help='directory for PNG contact sheets')
    ap.add_argument('--dump-ref', help='write every CI4 glyph (small then big) to this file, for checking the C')
    ap.add_argument('--px-small', type=int, default=CLASSES['small']['px'], help='em size of the small class')
    ap.add_argument('--px-big', type=int, default=CLASSES['big']['px'], help='em size of the big class')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    from fontTools.ttLib import TTFont
    cmap = TTFont(os.path.join(FONTDIR, FONTFILE % args.weight_small), lazy=True).getBestCmap()
    cps = build_charset(args, cmap)
    CLASSES['small']['px'] = args.px_small
    CLASSES['big']['px'] = args.px_big
    for c in CLASSES.values():
        c['top'] = (c['rows'] - c['px']) // 2
    bpp = 1 if args.mode == 'mono' else 3

    blobs = {}
    glyphs = {}
    for name, weight in (('small', args.weight_small), ('big', args.weight_big)):
        cls = CLASSES[name]
        r = Renderer(weight, cls, args.mode)
        data = bytearray()
        g = {}
        for cp in cps:
            body = r.body(cp)
            data += pack_bits(to_level_bits(body, bpp), bpp)
            g[cp] = band_of(body)
        if r.clipped:
            print('%s: %d glyphs clipped by the cell, e.g. %s' % (name, len(r.clipped),
                  ' '.join(chr(c) for c in r.clipped[:20])), file=sys.stderr)
        blobs[name] = bytes(data)
        glyphs[name] = g

    sstride = (CLASSES['small']['rows'] * CLASSES['small']['cols'] * bpp + 7) // 8
    bstride = (CLASSES['big']['rows'] * CLASSES['big']['cols'] * bpp + 7) // 8

    desc = 'JIS X 0208 rows 1-5 and level 1 kanji, extra punctuation'
    if args.extra or args.extra_from:
        desc += ', and every character of ' + ', '.join((args.extra or []) + [os.path.relpath(p, ROOT) for p in args.extra_from or []])

    src = [SOURCE_HEAD.format(count=len(cps), desc=desc, bpp=bpp,
                              sw=CLASSES['small']['cols'], sh=CLASSES['small']['rows'],
                              sstride=sstride,
                              bw=CLASSES['big']['cols'], bh=CLASSES['big']['rows'],
                              bstride=bstride)]
    src.append('static const u16 s_CodePoints[%d] = {' % len(cps))
    for i in range(0, len(cps), 16):
        src.append('\t' + ', '.join('0x%04x' % c for c in cps[i:i + 16]) + ',')
    src.append('};\n')
    src.append(c_bytes('s_Small', blobs['small']))
    src.append(c_bytes('s_Big', blobs['big']))
    src.append(SOURCE_TAIL)

    with open(args.out_c, 'w', newline='\n') as fd:
        fd.write('\n'.join(src))
    with open(args.out_h, 'w', newline='\n') as fd:
        fd.write(HEADER.format(count=len(cps),
                               sw=CLASSES['small']['cols'], sh=CLASSES['small']['rows'],
                               bw=CLASSES['big']['cols'], bh=CLASSES['big']['rows']))

    total = len(blobs['small']) + len(blobs['big']) + 2 * len(cps)
    print('%d characters, %d bpp: small %d + big %d + table %d = %d bytes' % (
        len(cps), bpp, len(blobs['small']), len(blobs['big']), 2 * len(cps), total))

    if args.dump_ref:
        with open(args.dump_ref, 'wb') as fd:
            for name in ('small', 'big'):
                for cp in cps:
                    fd.write(ci4_bytes(glyphs[name][cp]))

    if args.sheet:
        os.makedirs(args.sheet, exist_ok=True)
        kanji = [cp for cp in cps if 0x4e00 <= cp <= 0x9fff]
        samples = [
            'パーフェクト・ダーク',
            '任務　敵　武器　「ミッション」『完了』',
            'ジョアンナ・ダーク、キャリントン研究所へようこそ。',
            'あいうえおかきくけこさしすせそたちつてと',
            'がぎぐげござじずぜぞぱぴぷぺぽっゃゅょー',
            'アイウエオヴァィゥェォッャュョヶ・〜！？（）',
            '０１２３４５６７８９ＡＢＣＤＥＦａｂｃｄｅｆ',
        ]
        lines = [[ord(c) for c in s] for s in samples]
        for i in range(0, 200, 25):
            lines.append(kanji[i:i + 25])
        for name in ('small', 'big'):
            for scale in (1, 4):
                p = os.path.join(args.sheet, 'langfont-cjk-%s-%dx.png' % (name, scale))
                sheet(p, lines, glyphs[name], name, scale)
                print('wrote', p)


if __name__ == '__main__':
    main()

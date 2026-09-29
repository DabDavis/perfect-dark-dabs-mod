# langfont: Japanese glyphs for the port

`bake.py` renders **M PLUS Rounded 1c** into Perfect Dark's own glyph format
and writes `port/src/langfont_cjk.c` and `port/include/langfont_cjk.h`. Both
are generated and committed; the build does not run the baker (it needs PIL
with FreeType, fontTools and numpy, which the build machines are not asked to
have).

## The font

`fonts/` holds the font exactly as Google Fonts ships it:

| File | SHA-256 |
|------|---------|
| `MPLUSRounded1c-Regular.ttf` | `b75708b53e45b06d17d470aeeca5b766e3d1b3999f03f13ec4eb863ca846c14c` |
| `MPLUSRounded1c-Medium.ttf` | `adfde1b6bae58719c4e0144612a94232e72fc5ca655c4722165fe88d06521a70` |
| `METADATA.pb` | Google Fonts' metadata for the family |
| `OFL.txt` | the licence |

- Source: https://github.com/google/fonts/tree/84efd8ad78c3710ad14bd909e3bc407151885628/ofl/mplusrounded1c
  (google/fonts commit `84efd8ad78c3710ad14bd909e3bc407151885628`, 2026-05-01),
  fetched 2026-09-29.
- Copyright 2016 The Rounded M+ Project Authors. Licensed under the **SIL Open
  Font License 1.1** (`fonts/OFL.txt`; the font's own name table says so,
  records 13 and 14). Google Fonts' folder for this family carries no
  `OFL.txt` of its own, so ours is the OFL 1.1 text from its sister family
  `ofl/mplus1p/OFL.txt` at the same commit with this family's copyright line
  (name record 0) at the top.
- The baked bitmaps in `langfont_cjk.c` are a rendering of the font and go
  out under the same licence; the file's header says so. The OFL's Reserved
  Font Name clause does not apply: nothing is shipped under the font's name.

Only Regular is baked today; Medium is kept for trying a heavier weight
(`--weight-small Medium`, `--weight-big Medium`).

## Regenerate

From the repository root:

```sh
python3 tools/langfont/bake.py                          # the default set
python3 tools/langfont/bake.py --extra-from lang/ja     # plus every character the ja pack uses
python3 tools/langfont/bake.py --sheet /tmp/sheets      # and PNG contact sheets at 1x and 4x
```

Re-run with `--extra-from lang/ja` whenever the Japanese pack gains a
character outside the set, and commit the two generated files. `--extra FILE`
takes characters or `U+XXXX` tokens (with `#` comments). Characters the font
does not have, and any outside the BMP, are skipped with a note.

`--dump-ref FILE` writes every glyph as the C side will unpack it (small class
then big, in code point order); compiling `langfont_cjk.c` with a small harness
and comparing is how the C was checked against the Python (byte for byte).

## What is baked

- **Set:** JIS X 0208 rows 1-5 (symbols, full-width digits and Latin,
  hiragana, katakana incl. small forms, ヴ, ー and the voiced marks), JIS
  level 1 kanji (rows 16-47, 2965), a few forms beyond JIS that texts use
  (〜 ～ ― − ・ 【】 《》 ヵ ヶ ゔ ヷ-ヺ ★ etc.), and whatever `--extra` /
  `--extra-from` adds: 3358 characters by default.
- **Two size classes**, as Rare's JP font had: *small* 12 columns x 11 rows at
  an 11 px em for xs/sm/md, *big* 14 x 15 at a 14 px em for lg. The width is the
  advance; a 16 texel row holds 14 columns plus a texel of outline each side.
  At a 12 px em in 12 columns a run of kanji merged into one block, so the
  small class keeps a free column.
- **Rendering:** FreeType's hinted monochrome. Grey anti-aliasing at 11 px
  turns a dense kanji into a smear; the hinted bitmap reads like a bitmap
  font, which is what Rare's was too. `--mode grey` is there to compare.
- **Format:** the ROM's CI4 glyph - 16 texels a row, height + 2 rows, texel
  (1, 1) the cell's top left, body in palette indices 9-15 and the outline band
  in 1-7 of `var8007fb5c`. The band is the body grown one texel: the body's
  level beside it, `(level * 4 + 3) / 7` corner-on (7 and 4 for a solid body,
  as the ROM's '|' has).
- **Storage:** only the body is stored, one bit a texel (`BPP` 1; grey mode
  stores 3), 17 bytes a small glyph and 27 a big one: 154 KB of tables in all.
  The band and the CI4 (or I8) are made the first time a character is asked
  for, into a buffer that then never moves - the renderer caches a texture by
  its address.

## The C side

```c
const u8 *langfontCjkGlyph(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline);
const u8 *langfontCjkGlyphI8(u32 cp, s32 big, u8 *width, u8 *height, s8 *baseline);
s32 langfontCjkHas(u32 cp);
```

`langfontCjkGlyph()` answers the CI4 glyph (NULL if there is none) with the
fontchar fields: width 12 or 14, height 11 or 15, baseline 0 (the cell's top
on the line's top; the renderer places the line). `langfontCjkGlyphI8()` is the
body alone as I8 for GE Plus's front end (`struct gefont`): rows of
`(width + 7) & ~7` bytes, `height` rows. Any pointer answered stays valid for
the life of the program. The set includes some Latin-1 symbols of JIS row 1
(× ÷ ° § ¢ £ ¨ ´ ¶ ± ¬) as full-width glyphs; whether a Latin character is
drawn from here or from the ROM font is the renderer's choice.

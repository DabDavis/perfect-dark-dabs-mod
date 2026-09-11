#ifndef _IN_XBLAFONT_H
#define _IN_XBLAFONT_H

#include <stdio.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's own font, on the game's own text.
 *
 * The 2010 release set its menus in the same typeface the N64 game uses -
 * Handel Gothic - but rendered from the outline at five sizes rather than as
 * 16 texel bitmaps, and its glyphs are in the package: seven atlases in
 * Textures.raw (records 0db7 to 0dbe), one per font file, with the cell and
 * the ABC widths of every glyph in a `DataFiles/<name>.abc` beside them. So
 * the release's text art can be had the same way its textures are - decoded
 * out of the package as the renderer asks for a glyph - and does not need a
 * conversion, a pack, or a font renderer.
 *
 * What arrives is a *picture of a glyph*, not a font: every measurement the
 * game makes is still the ROM's. The N64 glyph's cell, its width, its
 * baseline and the font's kerning table decide where a character goes and how
 * wide the line is, exactly as they did, and the release's ink stays inside
 * the columns the ROM's ink filled. Nothing reflows, nothing can overlap, and
 * a menu that fitted before fits now.
 *
 * Down the line it is the *font* that is fitted rather than each glyph: one
 * scale and one offset per font, measured off the ink of both. A 16 texel
 * bitmap's own box carries a texel of the rasteriser's rounding with it, which
 * seats one letter higher than the next when a crisp glyph is stretched to
 * fill it - see "Fitting it to the line" in xblafont.c. A character the ROM
 * drew somewhere of its own, which the tile is then too short for, is shrunk
 * into the tile rather than squashed to fill it: it comes out smaller than the
 * rest of the font, which is the ROM's box being the shape it is, rather than
 * flattened, which reads as a glyph with its ends cut off.
 *
 * Across the line the box is still each character's own, and there the
 * question is how far the ROM's ink reaches rather than how much of it a
 * column holds: a crossbar two texels long is a letter that wide, not a faint
 * one (xblaFontSpan).
 *
 * This is the same road a texture pack's font folder takes (texpack.h's
 * TEXPACK_GLYPH_SET): a glyph has no texture number, so the display list says
 * which font and which character it is, and the renderer swaps the picture in
 * on its way to the GPU. A pack outranks this, glyph by glyph, the way it
 * outranks the release's numbered textures.
 */

/**
 * Mod.XblaFont, the menu's "Enable Font".
 *
 * Live, and live for the same reason the textures' switch is: it is read here,
 * where a picture is handed to the renderer, and not where a display list is
 * built. The setter drops the texture cache and that is the whole of it.
 *
 * Setting it on is also what may unpack a player's archive, so that a 250MB
 * unpack lands on the frame they asked for it on rather than on the first
 * glyph of the next menu.
 */
s32 xblaFontGetEnabled(void);
void xblaFontSetEnabled(s32 enabled);

/** The renderer's early out: the switch, a package, and a font to read. */
s32 xblaFontHaveGlyphs(void);

/**
 * The release's picture for one glyph, named the way gDPSetFontGlyphEXT names
 * it, as RGBA32 in the game's row order. NULL when there is nothing to draw -
 * no package, no such glyph, a character with no ink, or a font this does not
 * cover - and the ROM's own glyph is then drawn as before.
 *
 * The image is of the whole 16 texel wide, height + 2 row block the game
 * uploads for a glyph, at an integer scale of it, which is what a pack's
 * glyph images are too: the renderer normalises texture coordinates by the
 * tile, so the character has to sit in the same corner of the same block.
 *
 * Freed by the caller with xblaFontFreeGlyph(). Called on the render thread.
 */
u8 *xblaFontLoadGlyph(u32 glyph, s32 *outWidth, s32 *outHeight);
void xblaFontFreeGlyph(u8 *rgba);

/** Drops the atlases and the metrics. The decoded glyphs are kept. */
void xblaFontShutdown(void);

/** What has been read and built so far, for the F3 trace dump. */
void xblaFontTrace(FILE *f);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _IN_LANGFONT_H
#define _IN_LANGFONT_H

#include <PR/ultratypes.h>

struct fontchar;

/**
 * Glyphs for the characters Perfect Dark's NTSC fonts do not have: accented
 * Latin letters composed at first use from the ROM's own glyph plus a
 * diacritic drawn per font size, a few joined or turned glyphs (ae, oe,
 * guillemets, inverted marks), a drawn sharp s, and the baked Japanese glyphs
 * (langfont_cjk.c). The table of what can be made is langfonttable.h, written
 * by tools/langpack/latin.py, which is also what the pack checker reads.
 *
 * A glyph's pixel data is kept for the life of the program, so the renderer,
 * which keeps a texture by the address it was uploaded from, never sees one
 * address with two pictures.
 */

// The fonts, numbered as gDPSetFontGlyphEXT numbers them (textSetFontGlyph()).
#define LANGFONT_SM      0
#define LANGFONT_MD      1
#define LANGFONT_XS      2
#define LANGFONT_LG      3
#define LANGFONT_NUMERIC 4
#define LANGFONT_NUM     5

// A glyph named by gDPSetFontGlyphEXT that is none of the font's own 94:
// 0x100 + the slot langfontGlyphSlot() answers.
#define LANGFONT_GLYPH_EXT_BASE 0x100

/**
 * The glyph to draw for code point cp (not ASCII) in the font whose 94 ROM
 * glyphs are chars, numbered fontid (LANGFONT_*, or -1 for a font this does
 * not know, which gets the bare base letter).
 *
 * Answers NULL when the character is drawn as a space. *kernchar is the ASCII
 * character whose kerning class the glyph takes: what the caller's "previous
 * character" becomes after it.
 */
struct fontchar *langfontGlyph(struct fontchar *chars, s32 fontid, u32 cp, u8 *kernchar);

// The slot of a glyph langfontGlyph() made, or -1 for any other.
s32 langfontGlyphSlot(const struct fontchar *glyph);

/**
 * A font was loaded (textLoadFont()), or the language changed: PAL's one
 * extra row of line spacing under the '|' glyph, for the accents, is applied
 * to the fonts that take it while a Latin pack with accents is selected and
 * taken off otherwise.
 */
void langfontFontLoaded(struct fontchar *chars, s32 fontid);
void langfontLanguageChanged(void);

/**
 * Decodes one character of UTF-8 at *text and moves past it. A byte that
 * does not start a well-formed sequence is taken as Latin-1, so a mod's text
 * in the old single-byte encodings still draws.
 */
u32 langfontNextCodepoint(const char **text);

// The smallest line height text may be laid out at: 14 for Japanese.
s32 langfontMinLineHeight(void);

/**
 * The same recipes and marks for a renderer with fonts of its own (GE Plus's
 * front end, whose glyphs are 8-bit intensity with no outline band). kind is
 * latin.py's KIND_*: 0 base + marks, 1 two joined, 2 turned, 3 drawn as
 * another ASCII character, 4 the drawn sharp s, 5 a lone mark. A size class is
 * 0 small, 1 medium, 2 large; a mark's rows are hex intensity (9-f) or '.'.
 */
#define LANGFONT_KIND_MARK     0
#define LANGFONT_KIND_JOIN     1
#define LANGFONT_KIND_TURN     2
#define LANGFONT_KIND_FOLD     3
#define LANGFONT_KIND_DRAWN    4
#define LANGFONT_KIND_MARKONLY 5

s32 langfontRecipeOf(u32 cp, s32 *kind, const char **text, s32 *mark1, s32 *mark2);
const char *const *langfontMarkRows(s32 sizeclass, s32 mark, s32 *width, s32 *height);
s32 langfontMarkIsBelow(s32 mark);
const char *const *langfontSharpSRows(s32 sizeclass, s32 *width, s32 *height);

#endif

/**
 * The XBLA release's font, fitted into the ROM's glyph cells.
 * See xblafont.h for what this is; this file is the how.
 *
 * Three things have to come out of the package and meet: an atlas of glyphs
 * (a Textures.raw record), the cell of each glyph inside it (a `.abc` file),
 * and the character the game is drawing (the display list says so). The
 * release kept the typeface and the character set, so the third one is a
 * subtraction - a glyph id names a font and an index into it, and index 0 is
 * '!' - and the first two are read once per font.
 *
 * ## The `.abc` files
 *
 * `{u32 version, f32 lineheight, ..., u16 chars, ...}` to 0x58, then a
 * character table of u16 and then one 16 byte record per glyph:
 *
 *     u16 x1, y1, x2, y2   the cell in the atlas
 *     s16 a                left bearing
 *     u16 b                the cell's own width, x2 - x1
 *     u16 c                the advance
 *     u16 0
 *
 * `y1` and `y2` are the same for every glyph of a font - the cell is a line
 * box, not the ink - and the character table is a Unicode map from 0x20 up,
 * one-based, so `table[c - 0x20] - 1` is the record for character c. For the
 * printable ASCII the two are the same subtraction, which is the check: the
 * record for 'A' in all five Handel Gothic files decodes to an 'A'.
 *
 * The table's length is not stated anywhere that can be trusted - the count at
 * 0x14 is 30 longer than the table is, in all eight of the release's files -
 * so the glyph records are placed from the *end* of the file instead, which
 * they reach exactly, and the table is whatever is left in front of them. A
 * record is only accepted while it reads as a cell (b == x2 - x1, the last
 * field zero), which is what stops the walk inside the table: a run of table
 * entries cannot satisfy that.
 *
 * ## Which atlas is which font
 *
 * Records 0db7 to 0dbe, one per font file, and the pairing is by the size of
 * the picture: an atlas is exactly as tall as its file's lowest cell needs.
 * Two of them measure 1024x670 and only the subject separates those - 0dbc is
 * Handel Gothic 54 and 0dbe the Japanese MaruGothic 18 - so the table below
 * is written down rather than worked out, and the dimensions in it are checked
 * against the record as it is read.
 *
 * ## Fitting it to the line
 *
 * The release's cell and the ROM's are not the same box: 4J's is the font's
 * line box, the same height for every character, and the ROM's is the ink with
 * the baseline held separately. The two are matched on *ink*, taken out of the
 * character's pixel data through the font's palette - but on the ink of the
 * *font*, not of the glyph.
 *
 * Across a character the ROM's box is not a reliable thing to sit a letter in.
 * Its glyphs are 16 texel bitmaps of an antialiased rendering, and whether the
 * rasteriser spilled a faint row past an edge depended on where that edge fell
 * against the texel grid: the md font's 'A' has such a row under its baseline
 * and its 'H' has not, and the cell heights the font carries (11 and 10) took
 * the spill with them. Filling each box in turn therefore drew a letter a
 * whole texel taller than the one beside it - five pixels at 1080p - and the
 * text came out unevenly seated, which is what this is all about. The same
 * rounding puts the odd letter a texel off in the other fonts.
 *
 * So the ink is measured to a fraction of a texel (xblaFontSpan) and the
 * *font* is placed rather than the glyph: one scale and one offset per font,
 * fitted through the ink boxes of all 94 characters at once, which is a line
 * through a cloud of points whose inliers are every letter and digit - they
 * share a baseline, a cap line and an x-height in both fonts, the two being
 * the same typeface - and whose outliers are the characters the ROM drew
 * somewhere of its own (its '_' is an overbar at the cap line, its '=' sits up
 * there with it, its ';' has no tail). A glyph is placed on that line.
 *
 * The tile is what the line has to fit inside: the quad the text renderers lay
 * a glyph on starts a texel into the tile and runs the character's own height,
 * so a glyph has that height to sit in and anything past it is uploaded and
 * never drawn. Where the line asks for a fraction of a texel more than that -
 * a round letter's overshoot - the glyph is moved and squeezed by the fraction
 * and stays on the line with every other letter. Where it asks for much more,
 * which is the outliers, the glyph is *shrunk* into what the tile has: the
 * same factor in both directions, so what it loses is size and not shape, and
 * placed at the end of the tile nearest the line. Filling the ROM's own box
 * instead - which is what this did first - squashed the characters whose box
 * is a different shape from the release's: the ROM's colon is a pair of dots
 * two thirds the height of the release's, and stretching the release's into it
 * drew the dots as flat bars, which reads as a colon with its ends cut off.
 *
 * Everything the game measures is still the ROM's. A glyph's width, its
 * advance, its baseline and the kerning table lay the text out untouched, the
 * ink stays inside the columns the game draws of the character - its advance,
 * which the ROM's own ink box sits inside (see "Filling the line") - and
 * nothing reflows.
 *
 * Reading 4J's metrics instead would mean deciding where the ROM's baseline
 * sits inside a line box the ROM has no notion of, per font, and being wrong
 * about it moves text off its row. The fit never asks: it measures what both
 * fonts actually drew and puts the one on the other.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "video.h"
#include "game/modoptions.h"
#include "x360.h"
#include "texpack.h"
#include "xblaimport.h"
#include "xblatex.h"
#include "xblafont.h"

// The fonts gDPSetFontGlyphEXT names, in the order it numbers them:
// handelgothicsm, md, xs, lg, numeric.
#define XBLAFONT_NUM_FONTS 5

// Characters of a font this covers. chars[0] is '!' - textSetFontGlyph()'s
// index is `curchar - font->chars` and the text renderers index the table as
// `chars[c - 0x21]`. PAL's three big fonts carry 41 more for the accented
// characters; those are not in ASCII order and are left to the ROM.
#define XBLAFONT_NUM_CHARS  94
#define XBLAFONT_FIRST_CHAR 0x21

// The glyph tile the text renderers upload: 16 texels a row, the character's
// height plus two rows. A replacement is an integer multiple of it.
#define XBLAFONT_TILE_TEXELS 16
#define XBLAFONT_TILE_ROWS(h) ((h) + 2)

// The border the character sits inside, and so the first row and column of
// the tile the game samples. Its quad starts a texel in and runs the
// character's own width and height from there (text0f15568c's rectangle takes
// s and t from 32, a texel in s10.5), which leaves the outermost rows
// uploaded and never drawn: ink put there is ink thrown away.
#define XBLAFONT_TILE_BORDER 1

// Scale of the picture handed over, in texels of that tile. Chosen per glyph
// so the release's ink is never shrunk (which would throw away the detail
// this is all for), floored at 2 so that a half texel border is a pixel wide,
// and capped so that a 4J cell against a one texel ROM glyph cannot ask for a
// megabyte.
#define XBLAFONT_MIN_SCALE 2
#define XBLAFONT_MAX_SCALE 12

// The halo the outline pass draws around the body, in texels of the tile: how
// far it reaches, and how much of that reach is opaque before it falls away.
// See xblaFontBuildOutline for why they are not the shader's half a texel of
// solid ink. Both are floored in pixels of the picture rather than texels -
// the core at one, the fall at another past it - because a tenth of a texel is
// half a pixel in the xs font's five, and rounded away to nothing there it left
// the smallest text with no edge at all where it needs one most.
#define XBLAFONT_OUTLINE_REACH 0.4f
#define XBLAFONT_OUTLINE_CORE  0.1f

// Offsets the halo is gathered from: a disc of the reach, at the biggest
// picture a glyph can ask for. The bound covers both floors as well as the
// reach itself, so retuning either constant cannot quietly overrun the table.
#define XBLAFONT_OUTLINE_MAX_R \
	((s32)((XBLAFONT_OUTLINE_REACH + XBLAFONT_OUTLINE_CORE) * XBLAFONT_MAX_SCALE) + 2)
#define XBLAFONT_OUTLINE_MAX_TAPS \
	((XBLAFONT_OUTLINE_MAX_R * 2 + 1) * (XBLAFONT_OUTLINE_MAX_R * 2 + 1))

// How near a glyph has to sit to the font's line to be counted as sitting on
// it, in ROM texels. A third of a texel is under a twentieth of a capital in
// any of these fonts and well inside what the ROM's own rounding does, so the
// letters are inliers and the characters the ROM deliberately put somewhere
// else - its '_' is an overbar, its '=' is drawn at the cap line - are not.
#define XBLAFONT_FIT_TOL 0.34f

// How far a glyph may be moved, and how much of it squeezed away, to keep it
// inside the tile it is uploaded in before the line is given up on for it. A
// third of a texel covers the overshoot of a round letter, which is all that
// ever sticks out.
#define XBLAFONT_FIT_NUDGE 0.34f

// Anchors a line has to be fitted through, and how far apart two of them have
// to be to define one: a cap line against a baseline, never two baselines.
#define XBLAFONT_FIT_MIN_ANCHORS 16
#define XBLAFONT_FIT_MIN_SPREAD 0.25f

// Characters a font's condensation is taken from. Its own, not the line's: the
// numeric font draws fourteen characters in all, which is a middle worth
// having and half what a line is fitted through. Fall through this and nothing
// is widened, which is what the font did before there was a number for it.
#define XBLAFONT_COND_MIN_CHARS 8

// The character table's first character, and the file's header size.
#define XBLAFONT_ABC_FIRST 0x20
#define XBLAFONT_ABC_HEADER 0x58
#define XBLAFONT_ABC_RECORD 16

// The fonts' TLUT, two banks of 16 IA16 entries: bank 0 covers the body and
// the border the font bakes around it, bank 1 the body alone. So a texel is
// body when bank 1 gives it any alpha and inside the cell when bank 0 does.
extern u16 var8007fb5c[];

#define XBLAFONT_TLUT_BODY 16

struct xblafontcell {
	u16 x1;
	u16 y1;
	u16 x2;
	u16 y2;
	s16 a;
	u16 b;
	u16 c;
};

// A box of ink, measured to a fraction of a texel (of a glyph tile) or of a
// pixel (of an atlas). x2 and y2 are the far edges, not the last texel.
struct xblafontbox {
	f32 x1;
	f32 y1;
	f32 x2;
	f32 y2;
};

/**
 * Where one of the release's fonts sits on the ROM font's line, and how far
 * across it the ROM drew the same font.
 *
 * `y * scale + offset` takes a row of the atlas's line box to a row of the
 * ROM's line - the coordinate a glyph's baseline is an offset into - so one
 * pair of numbers places every character of a font. See "Fitting it to the
 * line" for why it is fitted rather than read off either font's metrics.
 *
 * `cond` is the same idea across: what the ROM's own ink boxes allow against
 * what the release's glyphs ask for at that scale, over the whole font. The
 * ROM's fonts are hand-condensed at these sizes - about nine tenths in the
 * three big ones, two thirds in the numeric - and a glyph is never drawn wider
 * than the font is condensed. See "Filling the line".
 */
struct xblafontline {
	s32 tried;   // 1 fitted, -1 gave up
	f32 scale;   // ROM texels to the atlas pixel
	f32 offset;
	f32 cond;    // the font's own condensation across, 1 being uncondensed
};

struct xblafontface {
	const char *abc;  // in the package
	u32 record;       // the atlas in Textures.raw
	s16 width;        // what that record must measure, as a check
	s16 height;
};

// Every font file the release ships. The Japanese pair and Times New Roman
// are here because knowing what a record is is worth as much as using it -
// nothing asks for them (see faceOfFont).
#define XBLAFONT_FACE_H12  0
#define XBLAFONT_FACE_H14  1
#define XBLAFONT_FACE_H22  2
#define XBLAFONT_FACE_H46  3
#define XBLAFONT_FACE_H54  4
#define XBLAFONT_NUM_FACES 5

static const struct xblafontface faces[XBLAFONT_NUM_FACES] = {
	/* H12 */ { "DataFiles/Handel Gothic_12.abc", 0x0db8, 1024,  48 },
	/* H14 */ { "DataFiles/Handel Gothic_14.abc", 0x0db9, 1024,  80 },
	/* H22 */ { "DataFiles/Handel Gothic_22.abc", 0x0dba, 1024, 150 },
	/* H46 */ { "DataFiles/Handel Gothic_46.abc", 0x0dbb, 1024, 512 },
	/* H54 */ { "DataFiles/Handel Gothic_54.abc", 0x0dbc, 1024, 670 },
};

/**
 * The size of the release's font each of the game's fonts is drawn from.
 *
 * A glyph is scaled into the ROM's cell whichever size it comes from, so this
 * only decides how much detail there is to scale: what is wanted is the size
 * whose ink is about as many pixels tall as the glyph is drawn on screen. The
 * menu is 220 units tall however big the window is, so at 1080p a unit is
 * five pixels and the small font's seven unit cell is a 35 pixel glyph - which
 * is Handel Gothic 46's ink, near enough. The big font takes 54, the largest
 * the release has, and the extra small one 22.
 *
 * The two small sizes (12 and 14) are hinted bitmaps of a few pixels and are
 * no better than the ROM's own, which is why nothing reads them.
 */
static const u8 faceOfFont[XBLAFONT_NUM_FONTS] = {
	/* handelgothicsm */ XBLAFONT_FACE_H46,
	/* handelgothicmd */ XBLAFONT_FACE_H54,
	/* handelgothicxs */ XBLAFONT_FACE_H22,
	/* handelgothiclg */ XBLAFONT_FACE_H54,
	/* numeric        */ XBLAFONT_FACE_H46,
};

struct xblafontatlas {
	s32 tried;                  // 1 read, -1 gave up
	u8 *alpha;                  // one byte a texel; the colour is white throughout
	s32 width;
	s32 height;
	struct xblafontcell *cells;
	u32 numCells;
	u16 *trans;                 // the character table, one based
	u32 numTrans;
};

struct xblafontglyph {
	u8 *rgba;
	s32 width;
	s32 height;
	s32 tried;
};

static s32 optEnabled = 1;

static struct xblafontatlas atlases[XBLAFONT_NUM_FACES];
static struct xblafontline lines[XBLAFONT_NUM_FONTS];
static struct xblafontglyph glyphs[2][XBLAFONT_NUM_FONTS][XBLAFONT_NUM_CHARS];
static s32 numBuilt;
static s32 numMissing;
static s32 numOffLine;

PD_CONSTRUCTOR static void xblaFontInit(void)
{
	configRegisterInt("Mod.XblaFont", &optEnabled, 0, 1);
}

/* -------------------------------------------------------------------------
 * The ROM's glyph
 * ------------------------------------------------------------------------- */

/**
 * The font as the ROM holds it, rather than the copy the game loaded.
 *
 * textLoadFont() allocates its copy from the stage pool and it is freed and
 * rebuilt at every level load, so a pointer to one is not a thing to read from
 * the render thread. The segment is the same data - preprocessFont() left it
 * as a struct font whose pixeldata are offsets from its own start - and it is
 * there for the life of the process. texpack.c reads it the same way to hash
 * the glyphs an emulator pack names.
 */
static const struct font *xblaFontRomFont(s32 id, const u8 **outStart, u32 *outLen)
{
	extern u8 *_fonthandelgothicsmSegmentRomStart, *_fonthandelgothicsmSegmentRomEnd;
	extern u8 *_fonthandelgothicmdSegmentRomStart, *_fonthandelgothicmdSegmentRomEnd;
	extern u8 *_fonthandelgothicxsSegmentRomStart, *_fonthandelgothicxsSegmentRomEnd;
	extern u8 *_fonthandelgothiclgSegmentRomStart, *_fonthandelgothiclgSegmentRomEnd;
	extern u8 *_fontnumericSegmentRomStart, *_fontnumericSegmentRomEnd;
	const u8 *const starts[XBLAFONT_NUM_FONTS] = {
		_fonthandelgothicsmSegmentRomStart, _fonthandelgothicmdSegmentRomStart,
		_fonthandelgothicxsSegmentRomStart, _fonthandelgothiclgSegmentRomStart,
		_fontnumericSegmentRomStart,
	};
	const u8 *const ends[XBLAFONT_NUM_FONTS] = {
		_fonthandelgothicsmSegmentRomEnd, _fonthandelgothicmdSegmentRomEnd,
		_fonthandelgothicxsSegmentRomEnd, _fonthandelgothiclgSegmentRomEnd,
		_fontnumericSegmentRomEnd,
	};

	if (id < 0 || id >= XBLAFONT_NUM_FONTS || !starts[id] || ends[id] <= starts[id]) {
		return NULL;
	}

	*outStart = starts[id];
	*outLen = (u32)(ends[id] - starts[id]);

	return (const struct font *)starts[id];
}

/**
 * The outer edges of a run of coverage, to a fraction of a texel.
 *
 * Handed a reading of each row (or column) of a glyph, in any unit. The
 * outermost row of an antialiased glyph is often not a row of the letter at
 * all but the spill the rasteriser left when the edge fell near the texel
 * boundary, and taking that row at face value is what puts one letter a whole
 * texel taller than the next. Its own ink against the ink of the row inside it
 * says where the edge really was: a row as full as its neighbour is ink to its
 * far side, a tenth of one is a tenth of a texel of it.
 *
 * The ratio only means that where the two lines hold the same amount of
 * letter, which is why what is handed in is not the same reading both ways
 * round (xblaFontRomInk):
 *
 * - **Down** a glyph it is the row's *total*. The rows of a letter that a
 *   line is fitted through are flat edges - a baseline, a cap line, an
 *   x-height - and there the total is the coverage. Where the letter tapers
 *   instead - the apex of an 'A', the point of a 'V' - the reading is short,
 *   and such a glyph is an outlier of the fit rather than a thing the fit is
 *   taken from.
 * - **Across** it, it is the column's *deepest texel*. A first column is
 *   often a stroke a row or two tall against a column of full height beside
 *   it - the arm of a 't' crossbar, the flag of a '1', the foot of a 'W' -
 *   and its total reads as a fringe, which pulls the edge in over a texel and
 *   squeezes the letter that is drawn there. Spill is still discounted: a
 *   column the rasteriser only grazed is faint at its deepest texel too. And
 *   there is no fit to be an outlier of across a glyph - the box measured is
 *   the box the release's glyph is drawn in, one character at a time.
 */
static void xblaFontSpan(const f32 *ink, s32 num, f32 *outLo, f32 *outHi)
{
	s32 first = 0;
	s32 last = num - 1;

	while (first < num && ink[first] <= 0) {
		first++;
	}

	while (last >= 0 && ink[last] <= 0) {
		last--;
	}

	if (first > last) {
		*outLo = 0;
		*outHi = 0;
		return;
	}

	*outLo = first;
	*outHi = last + 1;

	if (first < last && ink[first + 1] > ink[first]) {
		*outLo = first + 1.0f - ink[first] / ink[first + 1];
	}

	if (last > first && ink[last - 1] > ink[last]) {
		*outHi = last + ink[last] / ink[last - 1];
	}
}

/**
 * The character as the ROM holds it, and its tile.
 *
 * The glyph is CI4 at eight bytes a row, height + 2 rows tall, with the
 * character in the top left corner - and drawn at `baseline`, which is what
 * makes the tiles of two characters comparable at all.
 */
static const struct fontchar *xblaFontRomChar(s32 id, s32 index, const u8 **outPixels, s32 *outRows)
{
	const struct font *font;
	const u8 *start;
	u32 len;
	u32 ofs;
	s32 rows;

	font = xblaFontRomFont(id, &start, &len);

	if (!font || index < 0 || index >= XBLAFONT_NUM_CHARS) {
		return NULL;
	}

	ofs = (u32)(uintptr_t)font->chars[index].pixeldata;
	rows = XBLAFONT_TILE_ROWS(font->chars[index].height);

	if (!ofs || rows < 1 || ofs + (u32)rows * (XBLAFONT_TILE_TEXELS / 2) > len) {
		return NULL;
	}

	*outPixels = start + ofs;
	*outRows = rows;

	return &font->chars[index];
}

/**
 * The box a character's own body texels fill, in texels of its tile, and the
 * box the font's baked border fills around it.
 *
 * An index is body when the palette's body bank gives it alpha and part of the
 * cell when the other bank does. The body box is measured to a fraction of a
 * texel, since it is what a glyph is sat in; the border's is whole texels,
 * since all it does is bound the outline pass's halo.
 *
 * A row is read as its total ink and a column as its deepest texel, which is
 * the difference between measuring a flat edge and measuring how far a stroke
 * reaches - see xblaFontSpan. The sm 't' is the case that shows it: its
 * crossbar is one faint row, so as totals the letter measures 1.6 texels wide
 * against the 2.7 its ink covers, and the release's 't' was drawn at half the
 * width of its own.
 */
static s32 xblaFontRomInk(s32 id, s32 index, struct xblafontbox *body, s32 *cell, s32 *outRows)
{
	const struct fontchar *ch;
	const u8 *data;
	f32 rowink[XBLAFONT_TILE_ROWS(255)];  // each row's total
	f32 colink[XBLAFONT_TILE_TEXELS];     // each column's deepest texel
	s32 rows;
	s32 row;
	s32 col;
	s32 any = 0;

	ch = xblaFontRomChar(id, index, &data, &rows);

	if (!ch) {
		return 0;
	}

	cell[0] = XBLAFONT_TILE_TEXELS;
	cell[1] = rows;
	cell[2] = -1;
	cell[3] = -1;

	for (col = 0; col < XBLAFONT_TILE_TEXELS; col++) {
		colink[col] = 0;
	}

	for (row = 0; row < rows; row++) {
		rowink[row] = 0;

		for (col = 0; col < XBLAFONT_TILE_TEXELS; col++) {
			const u8 pair = data[row * (XBLAFONT_TILE_TEXELS / 2) + col / 2];
			const u8 ci = (col & 1) ? (pair & 0xf) : (pair >> 4);
			const s32 alpha = PD_BE16(var8007fb5c[XBLAFONT_TLUT_BODY + ci]) & 0xff;
			const s32 isCell = alpha || (PD_BE16(var8007fb5c[ci]) & 0xff) != 0;

			if (alpha) {
				rowink[row] += alpha;

				if (alpha > colink[col]) {
					colink[col] = alpha;
				}

				any = 1;
			}

			if (isCell) {
				if (col < cell[0]) cell[0] = col;
				if (row < cell[1]) cell[1] = row;
				if (col > cell[2]) cell[2] = col;
				if (row > cell[3]) cell[3] = row;
			}
		}
	}

	if (!any) {
		return 0;
	}

	xblaFontSpan(colink, XBLAFONT_TILE_TEXELS, &body->x1, &body->x2);
	xblaFontSpan(rowink, rows, &body->y1, &body->y2);

	*outRows = rows;

	return body->x2 > body->x1 && body->y2 > body->y1;
}

/**
 * The character whose release glyph is drawn for this one.
 *
 * The extra small font is written in capitals: every one of its lowercase
 * characters is the same bitmap as the capital, because a six texel cell has
 * no room for two cases, and the width the game lays the text out to is the
 * capital's. The release has both cases at every size, so drawing its
 * lowercase there would put an x-height letter in a capital's cell - small,
 * and adrift in a space measured for something else. Where the ROM draws the
 * two cases with the same texels, so does this.
 */
static s32 xblaFontSourceIndex(s32 id, s32 index)
{
	const s32 ch = index + XBLAFONT_FIRST_CHAR;
	const struct fontchar *lower;
	const struct fontchar *upper;
	const u8 *lowerpx;
	const u8 *upperpx;
	s32 lowerrows;
	s32 upperrows;
	s32 upperindex;

	if (ch < 'a' || ch > 'z') {
		return index;
	}

	upperindex = index - ('a' - 'A');

	lower = xblaFontRomChar(id, index, &lowerpx, &lowerrows);
	upper = xblaFontRomChar(id, upperindex, &upperpx, &upperrows);

	if (!lower || !upper || lowerrows != upperrows || lower->width != upper->width
			|| lower->baseline != upper->baseline) {
		return index;
	}

	if (memcmp(lowerpx, upperpx, (u32)lowerrows * (XBLAFONT_TILE_TEXELS / 2))) {
		return index;
	}

	return upperindex;
}

/* -------------------------------------------------------------------------
 * The release's glyph
 * ------------------------------------------------------------------------- */

static u32 xblaFontBE16(const u8 *p)
{
	return ((u32)p[0] << 8) | p[1];
}

/**
 * Reads one `.abc`: the glyph records off the end of the file, and the
 * character table in front of them.
 */
static s32 xblaFontReadAbc(struct xblafontatlas *atlas, const u8 *data, u32 len)
{
	u32 numCells = 0;
	u32 start;
	u32 i;

	while (len >= XBLAFONT_ABC_HEADER + (numCells + 1) * XBLAFONT_ABC_RECORD) {
		const u8 *p = data + len - (numCells + 1) * XBLAFONT_ABC_RECORD;
		const u32 x1 = xblaFontBE16(p);
		const u32 y1 = xblaFontBE16(p + 2);
		const u32 x2 = xblaFontBE16(p + 4);
		const u32 y2 = xblaFontBE16(p + 6);

		// Where the walk stops: in front of the first record is the character
		// table, whose entries cannot read as a cell.
		if (x2 <= x1 || y2 <= y1 || xblaFontBE16(p + 10) != x2 - x1 || xblaFontBE16(p + 14)) {
			break;
		}

		numCells++;
	}

	// Every font of the release covers the printable ASCII, so a table that
	// does not reach '~' was not read right and is not worth guessing with.
	if (numCells < 0x7f - XBLAFONT_ABC_FIRST) {
		return 0;
	}

	start = len - numCells * XBLAFONT_ABC_RECORD;

	atlas->cells = malloc(numCells * sizeof(struct xblafontcell));
	atlas->numTrans = (start - XBLAFONT_ABC_HEADER) / 2;
	atlas->trans = malloc(atlas->numTrans * sizeof(u16));

	if (!atlas->cells || !atlas->trans) {
		return 0;
	}

	for (i = 0; i < numCells; i++) {
		const u8 *p = data + start + i * XBLAFONT_ABC_RECORD;

		atlas->cells[i].x1 = xblaFontBE16(p);
		atlas->cells[i].y1 = xblaFontBE16(p + 2);
		atlas->cells[i].x2 = xblaFontBE16(p + 4);
		atlas->cells[i].y2 = xblaFontBE16(p + 6);
		atlas->cells[i].a = (s16)xblaFontBE16(p + 8);
		atlas->cells[i].b = xblaFontBE16(p + 10);
		atlas->cells[i].c = xblaFontBE16(p + 12);
	}

	for (i = 0; i < atlas->numTrans; i++) {
		atlas->trans[i] = xblaFontBE16(data + XBLAFONT_ABC_HEADER + i * 2);
	}

	atlas->numCells = numCells;

	return 1;
}

/**
 * The atlas and the metrics for one face, read once.
 *
 * The picture is kept as alpha alone. Every texel of these records is white
 * with an alpha, so three quarters of an RGBA copy would be the same byte
 * repeated, and the largest of them is a megabyte and a half that way.
 */
static struct xblafontatlas *xblaFontOpenFace(s32 which)
{
	struct xblafontatlas *atlas = &atlases[which];
	const char *path;
	struct x360stfs stfs;
	s32 index;
	u8 *abc = NULL;
	u32 abclen = 0;
	u8 *rgba;
	s32 width;
	s32 height;
	s32 i;

	if (atlas->tried) {
		return atlas->tried > 0 ? atlas : NULL;
	}

	atlas->tried = -1;

	// Never unpacks: the switch's setter is what pays for that, so that a
	// glyph drawn on the render thread cannot land a 250MB extract there.
	path = xblaImportGetReadyStfsPath();

	if (!path || !path[0]) {
		return NULL;
	}

	if (!x360StfsOpen(&stfs, path)) {
		return NULL;
	}

	index = x360StfsFind(&stfs, faces[which].abc);

	if (index >= 0) {
		abc = x360StfsRead(&stfs, (u32)index, &abclen);
	}

	x360StfsClose(&stfs);

	if (!abc) {
		sysLogPrintf(LOG_ERROR, "xblafont: no %s in the package", faces[which].abc);
		return NULL;
	}

	if (!xblaFontReadAbc(atlas, abc, abclen)) {
		sysLogPrintf(LOG_ERROR, "xblafont: %s is not metrics this understands", faces[which].abc);
		free(abc);
		free(atlas->cells);
		free(atlas->trans);
		atlas->cells = NULL;
		atlas->trans = NULL;
		return NULL;
	}

	free(abc);

	rgba = xblaTexDecodeRecord(faces[which].record, &width, &height);

	if (!rgba) {
		sysLogPrintf(LOG_ERROR, "xblafont: record %04x will not decode", faces[which].record);
		return NULL;
	}

	// The pairing of a record with a font file is written down rather than
	// derived, so this is where a package that is not the one it was written
	// against says so instead of drawing a wall texture as text.
	if (width != faces[which].width || height != faces[which].height) {
		sysLogPrintf(LOG_ERROR, "xblafont: record %04x is %dx%d, not the %dx%d %s wants",
				faces[which].record, width, height,
				faces[which].width, faces[which].height, faces[which].abc);
		free(rgba);
		return NULL;
	}

	atlas->alpha = malloc((u32)width * height);

	if (!atlas->alpha) {
		free(rgba);
		return NULL;
	}

	for (i = 0; i < width * height; i++) {
		atlas->alpha[i] = rgba[i * 4 + 3];
	}

	free(rgba);

	atlas->width = width;
	atlas->height = height;
	atlas->tried = 1;

	sysLogPrintf(LOG_NOTE, "xblafont: %s, %u glyphs in record %04x",
			faces[which].abc, atlas->numCells, faces[which].record);

	return atlas;
}

/** The glyph record for a character: one based in the table, so a zero is none. */
static const struct xblafontcell *xblaFontCellOf(const struct xblafontatlas *atlas, s32 ch)
{
	u32 i = (u32)(ch - XBLAFONT_ABC_FIRST);
	u32 g;

	if (ch < XBLAFONT_ABC_FIRST || i >= atlas->numTrans) {
		return NULL;
	}

	g = atlas->trans[i];

	if (!g || g > atlas->numCells) {
		return NULL;
	}

	return &atlas->cells[g - 1];
}

/**
 * The ink inside a cell, in pixels of the atlas and to a fraction of one.
 *
 * Measured the same way the ROM's is - rows totalled, columns taken at their
 * deepest texel, and the edges read by xblaFontSpan - which keeps the two
 * sides of the fit the same measurement. At 46 pixels to a capital it is all
 * but exact either way: a fringe here is a pixel of an edge rather than a
 * sixth of a letter.
 */
static s32 xblaFontCellInk(const struct xblafontatlas *atlas, const struct xblafontcell *cell, struct xblafontbox *ink)
{
	const s32 w = cell->x2 - cell->x1;
	const s32 h = cell->y2 - cell->y1;
	f32 *spans;
	s32 x;
	s32 y;

	if (cell->x2 > atlas->width || cell->y2 > atlas->height || w < 1 || h < 1) {
		return 0;
	}

	// One allocation for both: a cell is up to a megapixel of atlas and a line
	// of it is a thousandth of that, but they are still too big for the render
	// thread's stack.
	spans = calloc((u32)(w + h), sizeof(f32));

	if (!spans) {
		return 0;
	}

	for (y = 0; y < h; y++) {
		const u8 *row = &atlas->alpha[(cell->y1 + y) * atlas->width + cell->x1];

		for (x = 0; x < w; x++) {
			spans[w + y] += row[x];

			if (row[x] > spans[x]) {
				spans[x] = row[x];
			}
		}
	}

	xblaFontSpan(spans, w, &ink->x1, &ink->x2);
	xblaFontSpan(spans + w, h, &ink->y1, &ink->y2);

	free(spans);

	if (ink->x2 <= ink->x1 || ink->y2 <= ink->y1) {
		return 0;
	}

	// Back into the atlas, which is where the sampling reads from.
	ink->x1 += cell->x1;
	ink->x2 += cell->x1;
	ink->y1 += cell->y1;
	ink->y2 += cell->y1;

	return 1;
}

/* -------------------------------------------------------------------------
 * The line a font sits on
 * ------------------------------------------------------------------------- */

/**
 * How far across the ROM drew the same font: the middle of what its own ink
 * boxes allow against what the release's glyphs ask for at the font's scale.
 *
 * The ROM's fonts are drawn condensed at these sizes - a 16 texel bitmap of a
 * seven texel letter has no room to be anything else - and by about the same
 * amount all the way through a font, which is what makes one number of it: nine
 * tenths in the three big fonts, a little more in the xs one, two thirds in the
 * numeric, whose digits are all three texels wide.
 *
 * The middle is taken rather than the mean, for the same reason the line is
 * taken by agreement: a handful of characters the ROM drew narrower than its
 * own font - its '1' is the case this is here for - would otherwise pull the
 * whole font in to meet them.
 */
static f32 xblaFontBuildCond(const struct xblafontatlas *atlas, s32 id, f32 scale)
{
	f32 ratios[XBLAFONT_NUM_CHARS];
	s32 num = 0;
	s32 index;
	s32 i;
	s32 j;

	for (index = 0; index < XBLAFONT_NUM_CHARS; index++) {
		const struct xblafontcell *cell;
		struct xblafontbox body;
		struct xblafontbox ink;
		s32 cellbox[4];
		s32 rows;
		f32 want;

		if (!xblaFontRomInk(id, index, &body, cellbox, &rows)) {
			continue;
		}

		cell = xblaFontCellOf(atlas, xblaFontSourceIndex(id, index) + XBLAFONT_FIRST_CHAR);

		if (!cell || !xblaFontCellInk(atlas, cell, &ink)) {
			continue;
		}

		want = scale * (ink.x2 - ink.x1);

		if (want <= 0) {
			continue;
		}

		// Insertion sorted as they are taken, since the middle is what is
		// wanted and there are at most 94 of them.
		for (i = num; i > 0 && ratios[i - 1] > (body.x2 - body.x1) / want; i--) {
			ratios[i] = ratios[i - 1];
		}

		ratios[i] = (body.x2 - body.x1) / want;
		num++;
	}

	if (num < XBLAFONT_COND_MIN_CHARS) {
		// Nothing to go on, so nothing is widened: every glyph keeps the
		// columns the ROM's own ink filled.
		return 1;
	}

	j = num / 2;

	return num & 1 ? ratios[j] : (ratios[j - 1] + ratios[j]) * 0.5f;
}

/**
 * The line through the two fonts' ink: `rom = scale * atlas + offset`.
 *
 * Every character contributes two anchors, the top and the bottom of its ink,
 * the ROM's in the line coordinate its baseline is an offset into and the
 * release's in rows of its line box. A letter's anchors agree with every other
 * letter's - the two fonts are the same typeface, so a cap line is a cap line
 * in both - and the characters the ROM drew elsewhere disagree with everything,
 * which is why the line is taken by counting agreement rather than by least
 * squares: a fit that averaged them in would drag the whole font off its row to
 * meet an overbar.
 *
 * Every pair of anchors far enough apart to be a cap line against a baseline
 * proposes a line, and the one the most anchors sit on wins; it is then
 * re-taken as the mean of those anchors, so the answer is not two glyphs' worth
 * of rounding. Sixteen thousand candidates against ninety-odd anchors, once per
 * font, on the frame its first glyph is drawn.
 */
static s32 xblaFontBuildLine(struct xblafontline *out, s32 id)
{
	const struct xblafontatlas *atlas = xblaFontOpenFace(faceOfFont[id]);
	f32 rom[2 * XBLAFONT_NUM_CHARS];
	f32 rel[2 * XBLAFONT_NUM_CHARS];
	f32 spread;
	f32 lo;
	f32 hi;
	f32 bestscale = 0;
	f32 bestoffset = 0;
	s32 bestcount = 0;
	s32 num = 0;
	s32 index;
	s32 i;
	s32 j;
	s32 k;

	if (!atlas) {
		return 0;
	}

	for (index = 0; index < XBLAFONT_NUM_CHARS; index++) {
		const struct xblafontcell *cell;
		const struct fontchar *ch;
		struct xblafontbox body;
		struct xblafontbox ink;
		const u8 *pixels;
		s32 cellbox[4];
		s32 rows;

		ch = xblaFontRomChar(id, index, &pixels, &rows);

		if (!ch || !xblaFontRomInk(id, index, &body, cellbox, &rows)) {
			continue;
		}

		cell = xblaFontCellOf(atlas, xblaFontSourceIndex(id, index) + XBLAFONT_FIRST_CHAR);

		if (!cell || !xblaFontCellInk(atlas, cell, &ink)) {
			continue;
		}

		rom[num] = ch->baseline + body.y1;
		rel[num] = ink.y1 - cell->y1;
		num++;
		rom[num] = ch->baseline + body.y2;
		rel[num] = ink.y2 - cell->y1;
		num++;
	}

	if (num < XBLAFONT_FIT_MIN_ANCHORS) {
		return 0;
	}

	lo = hi = rel[0];

	for (i = 1; i < num; i++) {
		if (rel[i] < lo) lo = rel[i];
		if (rel[i] > hi) hi = rel[i];
	}

	spread = (hi - lo) * XBLAFONT_FIT_MIN_SPREAD;

	if (spread <= 0) {
		return 0;
	}

	for (i = 0; i < num; i++) {
		for (j = i + 1; j < num; j++) {
			const f32 d = rel[j] - rel[i];
			f32 scale;
			f32 offset;
			s32 count = 0;

			if (d < spread && -d < spread) {
				continue;
			}

			scale = (rom[j] - rom[i]) / d;
			offset = rom[i] - scale * rel[i];

			// A font that came out mirrored or flat is not a reading of
			// anything, whatever it agrees with.
			if (scale <= 0) {
				continue;
			}

			for (k = 0; k < num; k++) {
				const f32 r = scale * rel[k] + offset - rom[k];

				if (r < XBLAFONT_FIT_TOL && -r < XBLAFONT_FIT_TOL) {
					count++;
				}
			}

			if (count > bestcount) {
				bestcount = count;
				bestscale = scale;
				bestoffset = offset;
			}
		}
	}

	if (bestcount < XBLAFONT_FIT_MIN_ANCHORS) {
		return 0;
	}

	// Re-taken over everything that sits on it, twice, so that the line is the
	// font's rather than the two anchors' that proposed it.
	for (i = 0; i < 2; i++) {
		f32 sx = 0;
		f32 sy = 0;
		f32 sxx = 0;
		f32 sxy = 0;
		f32 den;
		s32 count = 0;

		for (k = 0; k < num; k++) {
			const f32 r = bestscale * rel[k] + bestoffset - rom[k];

			if (r >= XBLAFONT_FIT_TOL || -r >= XBLAFONT_FIT_TOL) {
				continue;
			}

			sx += rel[k];
			sy += rom[k];
			sxx += rel[k] * rel[k];
			sxy += rel[k] * rom[k];
			count++;
		}

		den = count * sxx - sx * sx;

		if (count < XBLAFONT_FIT_MIN_ANCHORS || den <= 0) {
			break;
		}

		bestscale = (count * sxy - sx * sy) / den;
		bestoffset = (sy - bestscale * sx) / count;
	}

	out->scale = bestscale;
	out->offset = bestoffset;
	out->cond = xblaFontBuildCond(atlas, id, bestscale);

	sysLogPrintf(LOG_NOTE, "xblafont: font %d sits on %s at %.4f x + %.3f, %d of %d anchors, condensed to %.3f",
			id, faces[faceOfFont[id]].abc, bestscale, bestoffset, bestcount, num, out->cond);

	return 1;
}

/** The line for one font, fitted once. NULL when it could not be. */
static const struct xblafontline *xblaFontGetLine(s32 id)
{
	struct xblafontline *line = &lines[id];

	if (!line->tried) {
		line->tried = xblaFontBuildLine(line, id) ? 1 : -1;
	}

	return line->tried > 0 ? line : NULL;
}

/* -------------------------------------------------------------------------
 * Building the picture
 * ------------------------------------------------------------------------- */

/**
 * One destination pixel's worth of the atlas: the mean alpha over the source
 * rectangle it covers.
 *
 * The release's glyph is about the size of the box it is going into - 46 pixel
 * ink into a seven texel cell at five pixels a texel - so a point sample of
 * any kind throws away most of the coverage and leaves a stair-stepped edge
 * next to the smoothed CI4 glyph it replaces. Averaging the footprint is what
 * makes the edge an edge; it costs a handful of texels per pixel, once per
 * glyph.
 *
 * The footprint is never narrower than one source texel, which is what makes
 * the same loop right in both directions: wider than a texel it is an area
 * average, and exactly a texel centred on the sample point it is the linear
 * blend of the two texels it straddles. Left as the bare footprint, a glyph
 * being scaled *up* a little - which is most of them, the scale being a whole
 * number of texels - would take one texel per pixel and come out as blocky as
 * a point sample.
 */
static u8 xblaFontArea(const struct xblafontatlas *atlas, f32 x0, f32 x1, f32 y0, f32 y1)
{
	f32 sum = 0;
	f32 weight = 0;
	s32 ix;
	s32 iy;

	for (iy = (s32)y0; iy <= (s32)(y1 - 0.0001f); iy++) {
		// How much of this source row the destination pixel covers, which is
		// a fraction at the first and last row and all of it between.
		const f32 ry = (iy + 1 < y1 ? iy + 1 : y1) - (iy > y0 ? iy : y0);

		if (iy < 0 || iy >= atlas->height || ry <= 0) {
			continue;
		}

		for (ix = (s32)x0; ix <= (s32)(x1 - 0.0001f); ix++) {
			const f32 rx = (ix + 1 < x1 ? ix + 1 : x1) - (ix > x0 ? ix : x0);

			if (ix < 0 || ix >= atlas->width || rx <= 0) {
				continue;
			}

			sum += atlas->alpha[iy * atlas->width + ix] * rx * ry;
			weight += rx * ry;
		}
	}

	if (weight <= 0) {
		return 0;
	}

	return (u8)(sum / weight + 0.5f);
}

/**
 * A glyph's columns: the width asked for, centred on the ROM's own ink and
 * held inside the band the game samples.
 *
 * ## Filling the line
 *
 * The ROM's ink box was the whole of this to begin with, and for a letter it
 * still is: the two fonts are the same typeface, so the columns the ROM's 'A'
 * filled are the columns an 'A' fills, and the space either side of it inside
 * the advance is the letter's own bearing.
 *
 * What it is not is a bound on the *character*, and the digit one is where the
 * two come apart. Every one of the game's fonts draws it as a bare stem with no
 * flag - in three of the five it is the same bitmap as the 'I' - and Handel
 * Gothic's own '1' carries a flag two thirds as wide again as its stem. Filling
 * the ROM's columns with that takes the stem in with the flag: the sm stem came
 * out 0.71 texels against the 1.19 the same font's 'I' gets, which is a '1'
 * drawn at half the weight of every other stroke on the line, and reads as a
 * thin one.
 *
 * What actually bounds a glyph is the band the game samples - the quad runs the
 * character's own width from one texel in, so ink outside it is uploaded and
 * never drawn, and it cannot reach the character beside it whatever the kerning
 * does. So a glyph may have the room its advance has.
 *
 * How much of it: never wider than the font is condensed (line->cond), which is
 * the same idea across as the fitted line is down - one number for the font,
 * taken from what the ROM's boxes allow over the whole of it. A glyph the ROM
 * drew at the font's own condensation does not move at all, so the letters keep
 * their bearings and the text has the colour it had; the characters the ROM
 * drew narrower than its own font take the room their advance has and no more.
 *
 * Where the advance has no room - the md and xs '1', whose bar already fills
 * it - the glyph stays as condensed as the ROM's two texels make it. That is
 * the box the game measures its text with, and it is not ours to widen.
 */
static void xblaFontFillAcross(struct xblafontbox *dst, const struct xblafontbox *body,
		f32 wide, f32 bandleft, f32 bandright)
{
	const f32 mid = (body->x1 + body->x2) * 0.5f;

	if (wide > bandright - bandleft) {
		wide = bandright - bandleft;
	}

	// Grown about the middle of the ROM's own ink, and slid back inside the
	// band where that has taken it out of one end.
	dst->x1 = mid - wide * 0.5f;

	if (dst->x1 < bandleft) {
		dst->x1 = bandleft;
	}

	if (dst->x1 + wide > bandright) {
		dst->x1 = bandright - wide;
	}

	dst->x2 = dst->x1 + wide;
}

/**
 * The body picture: the release's ink placed on the font's line, on a canvas
 * of the whole tile.
 *
 * Across the tile the ink fills the columns the ROM's own ink filled, so a
 * character never reaches into the one beside it whatever either font's widths
 * are. Down it the glyph sits where the font's line puts it (see "Fitting it
 * to the line"), which is what makes one letter flush with the next; only the
 * characters the ROM drew somewhere of its own - and they announce themselves
 * by not fitting in the tile at all - keep their own box.
 */
static s32 xblaFontBuildBody(struct xblafontglyph *out, s32 id, s32 index)
{
	const struct xblafontatlas *atlas;
	const struct xblafontline *line;
	const struct xblafontcell *cell;
	const struct fontchar *ch;
	const u8 *pixels;
	struct xblafontbox body;
	struct xblafontbox ink;
	struct xblafontbox dst;
	s32 cellbox[4];
	s32 rows;
	s32 scale;
	s32 px0;
	s32 py0;
	s32 px1;
	s32 py1;
	s32 x;
	s32 y;

	atlas = xblaFontOpenFace(faceOfFont[id]);

	if (!atlas) {
		return 0;
	}

	ch = xblaFontRomChar(id, index, &pixels, &rows);

	if (!ch || !xblaFontRomInk(id, index, &body, cellbox, &rows)) {
		// No body texels: a space, or a character this font does not draw.
		return 0;
	}

	cell = xblaFontCellOf(atlas, xblaFontSourceIndex(id, index) + XBLAFONT_FIRST_CHAR);

	if (!cell || !xblaFontCellInk(atlas, cell, &ink)) {
		return 0;
	}

	dst = body;

	line = xblaFontGetLine(id);

	if (line) {
		// What the game draws of the tile: the quad starts at the border and
		// runs the character's own width and height from there, so the glyph
		// has those to sit in, one texel in from the top left, and anything
		// past them is uploaded and never sampled.
		const f32 bandtop = XBLAFONT_TILE_BORDER;
		const f32 bandbot = ch->height + XBLAFONT_TILE_BORDER;
		const f32 bandleft = XBLAFONT_TILE_BORDER;
		const f32 bandright = ch->width + XBLAFONT_TILE_BORDER < XBLAFONT_TILE_TEXELS
				? ch->width + XBLAFONT_TILE_BORDER : XBLAFONT_TILE_TEXELS;
		const f32 room = bandbot - bandtop;

		// The line is in the coordinate the baseline is an offset into, so the
		// baseline comes back off to land in the tile.
		const f32 top = line->scale * (ink.y1 - cell->y1) + line->offset - ch->baseline;
		const f32 bot = line->scale * (ink.y2 - cell->y1) + line->offset - ch->baseline;
		const f32 want = bot - top;

		// What the tile has no room for, and where the glyph goes once it has
		// been given up: as near the line as the band allows, which for a
		// character the ROM drew somewhere of its own is against the end of
		// the band nearest the line.
		const f32 over = want > room ? want - room : 0;
		const f32 high = want - over;
		f32 y = top;

		if (y + high > bandbot) {
			y = bandbot - high;
		}

		if (y < bandtop) {
			y = bandtop;
		}

		if (over <= XBLAFONT_FIT_NUDGE && y - top <= XBLAFONT_FIT_NUDGE && top - y <= XBLAFONT_FIT_NUDGE) {
			// A round letter overshoots its line by a fraction of a texel and
			// a tile has no room for it, so it is moved and squeezed rather
			// than cut: less than a third of a texel of either, and the same
			// amount for every letter that overshoots.
			dst.y1 = y;
			dst.y2 = y + high;

			// Across, the ROM's ink box is where the glyph goes unless the
			// character the ROM drew is a narrower one than the release's -
			// see "Filling the line" - and then it takes what the band has.
			// Never narrower than the box: a letter keeps its own columns.
			{
				const f32 asks = line->scale * line->cond * (ink.x2 - ink.x1);

				xblaFontFillAcross(&dst, &body,
						asks > body.x2 - body.x1 ? asks : body.x2 - body.x1,
						bandleft, bandright);
			}
		} else {
			// A character the line cannot place is *shrunk* rather than
			// squashed: the tile's own height, and the width that goes with
			// it, so that what it loses is size and not shape. The ROM's
			// colon is the case that shows it - a pair of dots two thirds the
			// height of the release's, drawn between the baseline and the
			// x-height rather than on the baseline - and filling that box in
			// both directions drew the dots as flat bars, which is what reads
			// as a colon with its top and bottom cut off. It is placed across
			// the same way as any other glyph, at the width its own shape now
			// asks for, so the band still stops it reaching the character
			// beside it.
			xblaFontFillAcross(&dst, &body,
					line->scale * (ink.x2 - ink.x1) * (high / want), bandleft, bandright);

			dst.y1 = y;
			dst.y2 = y + high;
			numOffLine++;
		}
	}

	// One scale for both axes, so the sampling is as fine across as it is
	// down, and never below the release's own resolution.
	scale = (s32)((ink.x2 - ink.x1) / (dst.x2 - dst.x1) + 0.999f);

	y = (s32)((ink.y2 - ink.y1) / (dst.y2 - dst.y1) + 0.999f);

	if (y > scale) {
		scale = y;
	}

	if (scale < XBLAFONT_MIN_SCALE) {
		scale = XBLAFONT_MIN_SCALE;
	} else if (scale > XBLAFONT_MAX_SCALE) {
		scale = XBLAFONT_MAX_SCALE;
	}

	out->width = XBLAFONT_TILE_TEXELS * scale;
	out->height = rows * scale;
	out->rgba = calloc((u32)out->width * out->height, 4);

	if (!out->rgba) {
		return 0;
	}

	// The box in canvas pixels, which is a whole scale finer than the texel
	// grid the ROM's own metrics are quantised to.
	px0 = (s32)(dst.x1 * scale + 0.5f);
	py0 = (s32)(dst.y1 * scale + 0.5f);
	px1 = (s32)(dst.x2 * scale + 0.5f);
	py1 = (s32)(dst.y2 * scale + 0.5f);

	if (px0 < 0) px0 = 0;
	if (py0 < 0) py0 = 0;
	if (px1 > out->width) px1 = out->width;
	if (py1 > out->height) py1 = out->height;

	if (px1 <= px0 || py1 <= py0) {
		return 0;
	}

	{
		const f32 stepx = (ink.x2 - ink.x1) / (px1 - px0);
		const f32 stepy = (ink.y2 - ink.y1) / (py1 - py0);
		const f32 halfx = (stepx > 1 ? stepx : 1) * 0.5f;
		const f32 halfy = (stepy > 1 ? stepy : 1) * 0.5f;

		for (y = py0; y < py1; y++) {
			const f32 sy = ink.y1 + (y - py0 + 0.5f) * stepy;

			for (x = px0; x < px1; x++) {
				const f32 sx = ink.x1 + (x - px0 + 0.5f) * stepx;
				u8 *p = out->rgba + (y * out->width + x) * 4;

				p[0] = 255;
				p[1] = 255;
				p[2] = 255;
				p[3] = xblaFontArea(atlas, sx - halfx, sx + halfx, sy - halfy, sy + halfy);
			}
		}
	}

	return 1;
}

/**
 * The outline pass's picture: the body as a halo around itself.
 *
 * The outline pass draws one glyph twice in a two-cycle combiner, tile 0
 * through the palette bank that covers body and border and tile 1 through the
 * one that is only the body. The border the ROM's font bakes in is the whole
 * of the cell - an 'e' is a solid block with the strokes cut out of it - which
 * reads as a bold outline at 320x240 and as a slab behind every letter when it
 * is magnified, so gfx_opengl.cpp shapes a border out of the body instead when
 * Clean Text Outlines is on. That is what this does to the release's glyph.
 *
 * It cannot be left to the shader, which measures its half texel in texels of
 * whatever was uploaded: against a picture eight times the size of the tile
 * the border would come out an eighth as wide as it is meant to be.
 *
 * **The shader's arithmetic is not the shader's look** (2026-09-12), which is
 * what this did first and what came back as "the black outline is too thick".
 * The shader reaches half a texel out of a body it reads *bilinearly*, so the
 * band it draws is nothing like half a texel of ink: the body's own blur
 * bleeds over the inner half of it, and the outer half is the far end of a
 * bilinear ramp and fades. What reaches the screen is a soft edge. The
 * release's glyph is crisp - that is the whole point of it - so the same
 * reach, dilated by a plain maximum, is half a texel of *solid* black with a
 * hard rim: at 720p a two pixel ring around a three pixel stem, and the
 * counters of 'e' and 'a' filled in. Measured on the md font it was worse
 * still, because the eightfold `* 5 / 2` push towards opaque - which is there
 * to make one antialiased texel of a 16 texel ROM glyph count as coverage,
 * and has nothing to answer to in a picture whose edges are already a pixel
 * wide - grew the source shape before the dilation and put the 'o' out at
 * 0.83 texels against the 'B' at 0.43.
 *
 * So the band is shaped rather than dilated: the body's alpha gathered over a
 * disc of XBLAFONT_OUTLINE_REACH, each tap weighted by how far out it is -
 * opaque to XBLAFONT_OUTLINE_CORE and falling away to nothing at the reach -
 * which is a distance falloff, since for a pixel d out of the body the tap
 * that wins is the one at d. That reads at about the weight of the ROM font's
 * own outline beside it, keeps the crisp body it is drawn around, rounds the
 * corners the way the shader's weighted diagonals did, and has no rim to
 * alias when the picture is minified onto the tile's quad. The cell is still
 * the limit, and the tap at the centre keeps the halo under the body's own
 * antialiased edge so no seam opens between the two.
 *
 * With Clean Text Outlines off nothing is handed over at all, so tile 0 stays
 * the ROM's filled cell - which is the look that switch means.
 */
static s32 xblaFontBuildOutline(struct xblafontglyph *out, s32 id, s32 index)
{
	const struct xblafontglyph *src = &glyphs[0][id][index];
	struct xblafontbox body;
	struct {
		s32 dx;
		s32 dy;
		f32 weight;
	} taps[XBLAFONT_OUTLINE_MAX_TAPS];
	s32 numtaps = 0;
	s32 cellbox[4];
	f32 reach;
	f32 core;
	s32 rows;
	s32 scale;
	s32 r;
	s32 x;
	s32 y;
	s32 i;

	if (!g_ModOptions.cleantext || !src->rgba) {
		return 0;
	}

	if (!xblaFontRomInk(id, index, &body, cellbox, &rows) || rows < 1) {
		return 0;
	}

	scale = src->height / rows;

	if (scale < 1) {
		return 0;
	}

	core = XBLAFONT_OUTLINE_CORE * scale;

	if (core < 1.0f) {
		core = 1.0f;
	}

	reach = XBLAFONT_OUTLINE_REACH * scale;

	if (reach < core + 1.0f) {
		reach = core + 1.0f;
	}

	r = (s32)reach;

	// The disc, and each tap's share of the band. Once per glyph, so the loop
	// over the cell is a table walk.
	for (y = -r; y <= r; y++) {
		for (x = -r; x <= r; x++) {
			const f32 d = sqrtf((f32)(x * x + y * y));
			f32 weight;

			if (d >= reach) {
				continue;
			}

			weight = d <= core ? 1.0f : (reach - d) / (reach - core);

			// Kept in descending weight, so the loop over the cell can stop
			// at the first tap that cannot beat what it already has. A disc
			// is scores of taps against the eight this used to be, and every
			// pixel inside a stroke is answered by the first of them.
			for (i = numtaps; i > 0 && taps[i - 1].weight < weight; i--) {
				taps[i] = taps[i - 1];
			}

			taps[i].dx = x;
			taps[i].dy = y;
			taps[i].weight = weight;
			numtaps++;
		}
	}

	out->width = src->width;
	out->height = src->height;
	out->rgba = calloc((u32)out->width * out->height, 4);

	if (!out->rgba) {
		return 0;
	}

	// The cell in canvas pixels: the border does not leave the box the ROM's
	// glyph drew in, so a halo cannot grow the line's height or run into the
	// character beside it.
	for (y = cellbox[1] * scale; y < (cellbox[3] + 1) * scale && y < out->height; y++) {
		for (x = cellbox[0] * scale; x < (cellbox[2] + 1) * scale && x < out->width; x++) {
			f32 o = 0;
			u8 *p;

			for (i = 0; i < numtaps; i++) {
				const s32 sx = x + taps[i].dx;
				const s32 sy = y + taps[i].dy;
				f32 a;

				if (taps[i].weight * 255.0f <= o) {
					break;
				}

				if (sx < 0 || sy < 0 || sx >= out->width || sy >= out->height) {
					continue;
				}

				a = src->rgba[(sy * out->width + sx) * 4 + 3] * taps[i].weight;

				if (a > o) {
					o = a;
				}
			}

			if (o > 255.0f) {
				o = 255.0f;
			}

			p = out->rgba + (y * out->width + x) * 4;
			p[0] = 255;
			p[1] = 255;
			p[2] = 255;
			p[3] = (u8)(o + 0.5f);
		}
	}

	return 1;
}

/* -------------------------------------------------------------------------
 * What the renderer asks
 * ------------------------------------------------------------------------- */

s32 xblaFontGetEnabled(void)
{
	return optEnabled;
}

void xblaFontSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled == optEnabled) {
		return;
	}

	optEnabled = enabled;

	// Turning it on is where an archive comes apart, on the game thread and on
	// the frame the player asked: the render thread never unpacks anything.
	if (enabled) {
		xblaTexGetNumRecords();
	}

	// The glyphs already uploaded are what is on screen, so the switch only
	// shows when their cache entries go.
	videoResetTextureCache();
}

s32 xblaFontHaveGlyphs(void)
{
	// Every texture upload in the game asks this, so it is two flag reads: the
	// switch, and whether a package was found at startup. Without the second
	// one a machine with no package would ask for a glyph, be told no, and
	// remember that per glyph - which works, and puts the renderer through a
	// branch it can never come out of.
	return optEnabled && xblaImportIsAvailable();
}

u8 *xblaFontLoadGlyph(u32 glyph, s32 *outWidth, s32 *outHeight)
{
	struct xblafontglyph *out;
	s32 outline;
	s32 id;
	s32 index;
	u8 *copy;

	if (!optEnabled || !(glyph & TEXPACK_GLYPH_SET)) {
		return NULL;
	}

	outline = TEXPACK_GLYPH_IS_OUTLINE(glyph) ? 1 : 0;
	id = TEXPACK_GLYPH_FONT(glyph);
	index = TEXPACK_GLYPH_INDEX(glyph);

	if (id >= XBLAFONT_NUM_FONTS || index >= XBLAFONT_NUM_CHARS) {
		return NULL;
	}

	out = &glyphs[outline][id][index];

	if (!out->tried) {
		// The outline is made out of the body, so the body is built first
		// whichever of the two is asked for. That is also what keeps the pair
		// in step: one picture, one scale, one shape.
		if (outline && !glyphs[0][id][index].tried) {
			glyphs[0][id][index].tried =
				xblaFontBuildBody(&glyphs[0][id][index], id, index) ? 1 : -1;
		}

		out->tried = (outline ? xblaFontBuildOutline(out, id, index)
				: xblaFontBuildBody(out, id, index)) ? 1 : -1;

		if (out->tried > 0) {
			numBuilt++;
		} else {
			numMissing++;
			free(out->rgba);
			out->rgba = NULL;
		}
	}

	if (out->tried < 0 || !out->rgba) {
		return NULL;
	}

	// Kept and copied out rather than handed over, like a pack's glyphs: there
	// are hundreds of them, the renderer's cache is not big enough to hold
	// them against a stage's textures, and a glyph that had to be rebuilt on
	// every eviction would show the ROM's for a frame each time.
	copy = malloc((u32)out->width * out->height * 4);

	if (!copy) {
		return NULL;
	}

	memcpy(copy, out->rgba, (u32)out->width * out->height * 4);

	*outWidth = out->width;
	*outHeight = out->height;

	return copy;
}

void xblaFontFreeGlyph(u8 *rgba)
{
	free(rgba);
}

void xblaFontShutdown(void)
{
	s32 i;

	for (i = 0; i < XBLAFONT_NUM_FACES; i++) {
		free(atlases[i].alpha);
		free(atlases[i].cells);
		free(atlases[i].trans);
		memset(&atlases[i], 0, sizeof(atlases[i]));
	}

	// The lines were measured off those atlases, so they go with them.
	memset(lines, 0, sizeof(lines));
}

void xblaFontTrace(FILE *f)
{
	s32 i;

	fprintf(f, "xblafont: %s, %d glyphs built, %d without one, %d off the font's line\n",
			optEnabled ? "on" : "off", numBuilt, numMissing, numOffLine);

	for (i = 0; i < XBLAFONT_NUM_FONTS; i++) {
		if (lines[i].tried > 0) {
			fprintf(f, "  font %d: on %s, %.4f x + %.3f\n", i,
					faces[faceOfFont[i]].abc, lines[i].scale, lines[i].offset);
		} else if (lines[i].tried) {
			fprintf(f, "  font %d: no line, every glyph in its own box\n", i);
		}
	}

	for (i = 0; i < XBLAFONT_NUM_FACES; i++) {
		if (atlases[i].tried) {
			fprintf(f, "  %s: %s, %u glyphs, %dx%d\n", faces[i].abc,
					atlases[i].tried > 0 ? "read" : "gave up",
					atlases[i].numCells, atlases[i].width, atlases[i].height);
		}
	}
}

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
 * ## Fitting it into the cell
 *
 * The release's cell and the ROM's are not the same box: 4J's is the font's
 * line box, the same height for every character, and the ROM's is the ink with
 * the baseline held separately. So the two are matched on *ink*: the release's
 * glyph is scaled into the box the ROM's glyph's own body texels occupy, taken
 * out of the character's pixel data through the font's palette. That is what
 * makes this a drop-in - every glyph lands where the game already put it,
 * whatever either font thinks its metrics are, and a menu laid out to the
 * ROM's widths still fits.
 *
 * Mapping 4J's metrics onto the game's instead would mean deciding where the
 * ROM's baseline sits inside a line box the ROM has no notion of, per font,
 * and being wrong about it moves text off its row.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

// Scale of the picture handed over, in texels of that tile. Chosen per glyph
// so the release's ink is never shrunk (which would throw away the detail
// this is all for), floored at 2 so that a half texel border is a pixel wide,
// and capped so that a 4J cell against a one texel ROM glyph cannot ask for a
// megabyte.
#define XBLAFONT_MIN_SCALE 2
#define XBLAFONT_MAX_SCALE 12

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
static struct xblafontglyph glyphs[2][XBLAFONT_NUM_FONTS][XBLAFONT_NUM_CHARS];
static s32 numBuilt;
static s32 numMissing;

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
 * The box a character's own body texels fill, in texels of its tile, and the
 * box the font's baked border fills around it.
 *
 * The glyph is CI4 at eight bytes a row, height + 2 rows tall, with the
 * character in the top left corner. An index is body when the palette's body
 * bank gives it alpha and part of the cell when the other bank does.
 */
static s32 xblaFontRomInk(s32 id, s32 index, s32 *body, s32 *cell, s32 *outRows)
{
	const struct font *font;
	const u8 *start;
	const u8 *data;
	u32 len;
	u32 ofs;
	s32 rows;
	s32 row;
	s32 col;

	font = xblaFontRomFont(id, &start, &len);

	if (!font || index < 0 || index >= XBLAFONT_NUM_CHARS) {
		return 0;
	}

	ofs = (u32)(uintptr_t)font->chars[index].pixeldata;
	rows = XBLAFONT_TILE_ROWS(font->chars[index].height);

	if (!ofs || rows < 1 || ofs + (u32)rows * (XBLAFONT_TILE_TEXELS / 2) > len) {
		return 0;
	}

	data = start + ofs;

	body[0] = cell[0] = XBLAFONT_TILE_TEXELS;
	body[1] = cell[1] = rows;
	body[2] = cell[2] = -1;
	body[3] = cell[3] = -1;

	for (row = 0; row < rows; row++) {
		for (col = 0; col < XBLAFONT_TILE_TEXELS; col++) {
			const u8 pair = data[row * (XBLAFONT_TILE_TEXELS / 2) + col / 2];
			const u8 ci = (col & 1) ? (pair & 0xf) : (pair >> 4);
			const s32 isBody = (PD_BE16(var8007fb5c[XBLAFONT_TLUT_BODY + ci]) & 0xff) != 0;
			const s32 isCell = isBody || (PD_BE16(var8007fb5c[ci]) & 0xff) != 0;

			if (isBody) {
				if (col < body[0]) body[0] = col;
				if (row < body[1]) body[1] = row;
				if (col > body[2]) body[2] = col;
				if (row > body[3]) body[3] = row;
			}

			if (isCell) {
				if (col < cell[0]) cell[0] = col;
				if (row < cell[1]) cell[1] = row;
				if (col > cell[2]) cell[2] = col;
				if (row > cell[3]) cell[3] = row;
			}
		}
	}

	*outRows = rows;

	return body[2] >= body[0] && body[3] >= body[1];
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

/** The ink inside a cell, which is what is matched to the ROM's ink. */
static s32 xblaFontCellInk(const struct xblafontatlas *atlas, const struct xblafontcell *cell, s32 *ink)
{
	s32 x;
	s32 y;

	if (cell->x2 > atlas->width || cell->y2 > atlas->height) {
		return 0;
	}

	ink[0] = cell->x2;
	ink[1] = cell->y2;
	ink[2] = -1;
	ink[3] = -1;

	for (y = cell->y1; y < cell->y2; y++) {
		for (x = cell->x1; x < cell->x2; x++) {
			if (!atlas->alpha[y * atlas->width + x]) {
				continue;
			}

			if (x < ink[0]) ink[0] = x;
			if (y < ink[1]) ink[1] = y;
			if (x > ink[2]) ink[2] = x;
			if (y > ink[3]) ink[3] = y;
		}
	}

	return ink[2] >= ink[0] && ink[3] >= ink[1];
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
 * The body picture: the release's ink scaled into the box the ROM's body
 * texels filled, on a canvas of the whole tile.
 */
static s32 xblaFontBuildBody(struct xblafontglyph *out, s32 id, s32 index)
{
	const struct xblafontatlas *atlas;
	const struct xblafontcell *cell;
	s32 body[4];
	s32 cellbox[4];
	s32 ink[4];
	s32 rows;
	s32 scale;
	s32 dw;
	s32 dh;
	s32 x;
	s32 y;

	atlas = xblaFontOpenFace(faceOfFont[id]);

	if (!atlas) {
		return 0;
	}

	if (!xblaFontRomInk(id, index, body, cellbox, &rows)) {
		// No body texels: a space, or a character this font does not draw.
		return 0;
	}

	cell = xblaFontCellOf(atlas, index + XBLAFONT_FIRST_CHAR);

	if (!cell || !xblaFontCellInk(atlas, cell, ink)) {
		return 0;
	}

	// One scale for both axes, so a glyph is not stretched, and never below
	// the release's own resolution.
	scale = (ink[2] - ink[0] + 1 + body[2] - body[0]) / (body[2] - body[0] + 1);

	y = (ink[3] - ink[1] + 1 + body[3] - body[1]) / (body[3] - body[1] + 1);

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

	dw = (body[2] - body[0] + 1) * scale;
	dh = (body[3] - body[1] + 1) * scale;

	{
		const f32 stepx = (f32)(ink[2] - ink[0] + 1) / dw;
		const f32 stepy = (f32)(ink[3] - ink[1] + 1) / dh;
		const f32 halfx = (stepx > 1 ? stepx : 1) * 0.5f;
		const f32 halfy = (stepy > 1 ? stepy : 1) * 0.5f;

		for (y = 0; y < dh; y++) {
			const f32 sy = ink[1] + y * stepy;

			for (x = 0; x < dw; x++) {
				const f32 sx = ink[0] + x * stepx;
				u8 *p = out->rgba + (((body[1] * scale + y) * out->width) + body[0] * scale + x) * 4;

				p[0] = 255;
				p[1] = 255;
				p[2] = 255;
				p[3] = xblaFontArea(atlas,
						sx + stepx * 0.5f - halfx, sx + stepx * 0.5f + halfx,
						sy + stepy * 0.5f - halfy, sy + stepy * 0.5f + halfy);
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
 * Clean Text Outlines is on. That is what this does to the release's glyph,
 * and by the same arithmetic: the body's alpha half a texel out in eight
 * directions, pushed towards opaque, the diagonals counting for less so the
 * corners round off, and the cell still the limit.
 *
 * It cannot be left to the shader, which measures its half texel in texels of
 * whatever was uploaded: against a picture eight times the size of the tile
 * the border would come out an eighth as wide as it is meant to be.
 *
 * With Clean Text Outlines off nothing is handed over at all, so tile 0 stays
 * the ROM's filled cell - which is the look that switch means.
 */
static s32 xblaFontBuildOutline(struct xblafontglyph *out, s32 id, s32 index)
{
	static const s32 offs[8][2] = {
		{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
		{ 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
	};
	const struct xblafontglyph *src = &glyphs[0][id][index];
	s32 body[4];
	s32 cellbox[4];
	s32 rows;
	s32 scale;
	s32 r;
	s32 x;
	s32 y;
	s32 i;

	if (!g_ModOptions.cleantext || !src->rgba) {
		return 0;
	}

	if (!xblaFontRomInk(id, index, body, cellbox, &rows) || rows < 1) {
		return 0;
	}

	scale = src->height / rows;

	if (scale < 1) {
		return 0;
	}

	r = scale / 2;

	if (r < 1) {
		r = 1;
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
			s32 o = src->rgba[(y * out->width + x) * 4 + 3] * 5 / 2;
			u8 *p;

			if (o > 255) {
				o = 255;
			}

			for (i = 0; i < 8; i++) {
				const s32 sx = x + offs[i][0] * r;
				const s32 sy = y + offs[i][1] * r;
				s32 a;

				if (sx < 0 || sy < 0 || sx >= out->width || sy >= out->height) {
					continue;
				}

				a = src->rgba[(sy * out->width + sx) * 4 + 3] * 5 / 2;

				if (a > 255) {
					a = 255;
				}

				if (i >= 4) {
					a = a * 4 / 5;
				}

				if (a > o) {
					o = a;
				}
			}

			p = out->rgba + (y * out->width + x) * 4;
			p[0] = 255;
			p[1] = 255;
			p[2] = 255;
			p[3] = (u8)o;
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
}

void xblaFontTrace(FILE *f)
{
	s32 i;

	fprintf(f, "xblafont: %s, %d glyphs built, %d without one\n",
			optEnabled ? "on" : "off", numBuilt, numMissing);

	for (i = 0; i < XBLAFONT_NUM_FACES; i++) {
		if (atlases[i].tried) {
			fprintf(f, "  %s: %s, %u glyphs, %dx%d\n", faces[i].abc,
					atlases[i].tried > 0 ? "read" : "gave up",
					atlases[i].numCells, atlases[i].width, atlases[i].height);
		}
	}
}

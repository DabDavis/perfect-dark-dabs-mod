#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "types.h"
#include "system.h"
#include "langfont.h"
#include "langfont_cjk.h"
#include "langpack.h"

/**
 * Glyphs past ASCII for Perfect Dark's fonts. See langfont.h.
 *
 * A ROM glyph (types.h struct fontchar) is CI4, 16 texels (8 bytes) a row and
 * height + 2 rows: the ink is at texels 1..width of rows 1..height, and row 0,
 * the last row and the columns either side are the outline band. Indices 9-15
 * are the body (its intensity), 1-7 the band's cover (var8007fb5c), 8 is band
 * with no body. The fill pass draws texels 1..width of rows 1..height at
 * (x, y + baseline); the outline pass the whole cell one texel up and left.
 *
 * A composed glyph is the base letter's cell with rows added above (or below)
 * for the mark: the mark's body is drawn in, the band is grown one texel
 * round it (7 beside, a partial cover at a corner - the ROM's own shape), and
 * baseline moves up by the rows added, so the letter itself stays where the
 * ROM puts it and only the accent reaches into the line above. That is why a
 * Latin pack with accents takes PAL's one extra row of line spacing.
 */

struct langfontrecipe {
	u16 cp;
	u8 kind;
	const char *text;
	u8 mark1;
	u8 mark2;
};

#define KIND_MARK     0
#define KIND_JOIN     1
#define KIND_TURN     2
#define KIND_FOLD     3
#define KIND_DRAWN    4
#define KIND_MARKONLY 5

#include "langfonttable.h"

/* ---- the marks, per size ------------------------------------------------ */

// Hex digits are body intensity (9-f), '.' is nothing. Rows top to bottom.
struct langfontmark {
	const char *rows[4];
};

// Size classes: small (xs, sm, numeric), medium (md), large (lg).
#define SIZE_S 0
#define SIZE_M 1
#define SIZE_L 2

static const struct langfontmark g_Marks[3][LANGFONT_NUM_MARKS] = {
	[SIZE_S] = {
		[LANGFONT_MARK_ACUTE]       = { { ".f", "f." } },
		[LANGFONT_MARK_GRAVE]       = { { "f.", ".f" } },
		[LANGFONT_MARK_CIRCUMFLEX]  = { { ".f.", "f.f" } },
		[LANGFONT_MARK_DIAERESIS]   = { { "f.f", "..." } },
		[LANGFONT_MARK_TILDE]       = { { ".f.f", "f.f." } },
		[LANGFONT_MARK_RING]        = { { "ff", "ff" } },
		[LANGFONT_MARK_CEDILLA]     = { { ".f", "ff" } },
		[LANGFONT_MARK_CARON]       = { { "f.f", ".f." } },
		[LANGFONT_MARK_BREVE]       = { { "f..f", ".ff." } },
		[LANGFONT_MARK_MACRON]      = { { "fff", "..." } },
		[LANGFONT_MARK_DOTABOVE]    = { { "f", "." } },
		[LANGFONT_MARK_DOUBLEACUTE] = { { ".f.f", "f.f." } },
		[LANGFONT_MARK_OGONEK]      = { { "f.", ".f" } },
		[LANGFONT_MARK_COMMABELOW]  = { { ".f", "f." } },
	},
	[SIZE_M] = {
		[LANGFONT_MARK_ACUTE]       = { { ".cf", "fc." } },
		[LANGFONT_MARK_GRAVE]       = { { "fc.", ".cf" } },
		[LANGFONT_MARK_CIRCUMFLEX]  = { { ".ff.", "f..f" } },
		[LANGFONT_MARK_DIAERESIS]   = { { "ff.ff", "ff.ff" } },
		[LANGFONT_MARK_TILDE]       = { { ".ff.f", "f.ff." } },
		[LANGFONT_MARK_RING]        = { { ".ff.", "f..f", ".ff." } },
		[LANGFONT_MARK_CEDILLA]     = { { ".f", "ff" } },
		[LANGFONT_MARK_CARON]       = { { "f..f", ".ff." } },
		[LANGFONT_MARK_BREVE]       = { { "f..f", ".ff." } },
		[LANGFONT_MARK_MACRON]      = { { "ffff" } },
		[LANGFONT_MARK_DOTABOVE]    = { { "ff", "ff" } },
		[LANGFONT_MARK_DOUBLEACUTE] = { { ".f.f", "f.f." } },
		[LANGFONT_MARK_OGONEK]      = { { "f.", ".f" } },
		[LANGFONT_MARK_COMMABELOW]  = { { ".f", "f." } },
	},
	[SIZE_L] = {
		[LANGFONT_MARK_ACUTE]       = { { "..cf", ".cf.", "cf.." } },
		[LANGFONT_MARK_GRAVE]       = { { "fc..", ".fc.", "..fc" } },
		[LANGFONT_MARK_CIRCUMFLEX]  = { { "..f..", ".f.f.", "f...f" } },
		[LANGFONT_MARK_DIAERESIS]   = { { "ff.ff", "ff.ff" } },
		[LANGFONT_MARK_TILDE]       = { { ".ff..f", "f..ff." } },
		[LANGFONT_MARK_RING]        = { { ".ff.", "f..f", ".ff." } },
		[LANGFONT_MARK_CEDILLA]     = { { ".f.", "..f", "ff." } },
		[LANGFONT_MARK_CARON]       = { { "f...f", ".f.f.", "..f.." } },
		[LANGFONT_MARK_BREVE]       = { { "f...f", ".fff." } },
		[LANGFONT_MARK_MACRON]      = { { "fffff" } },
		[LANGFONT_MARK_DOTABOVE]    = { { "ff", "ff" } },
		[LANGFONT_MARK_DOUBLEACUTE] = { { "..f..f", ".f..f.", "f..f.." } },
		[LANGFONT_MARK_OGONEK]      = { { "f..", ".ff" } },
		[LANGFONT_MARK_COMMABELOW]  = { { ".ff", ".f.", "f.." } },
	},
};

// The sharp s, drawn per size (xs is its own: the font is small capitals).
static const char *const g_SharpS[4][15] = {
	{ ".ff.", "f..f", "f.f.", "f..f", "f.f." },
	{ ".fff.", "f...f", "f..f.", "f.ff.", "f...f", "f...f", "f.ff." },
	{ ".ffff..", "ff..ff.", "ff..ff.", "ff.ff..", "ff.ff..", "ff..ff.", "ff...ff", "ff...ff", "ff...ff", "ff..ff.", "ff.ff.." },
	{ "..ffff..", ".ff..ff.", "ff....ff", "ff....ff", "ff...ff.", "ff..ff..", "ff..ff..", "ff...ff.", "ff....ff", "ff....ff", "ff....ff", "ff....ff", "ff...ff.", "ff.fff.." },
};

static s32 langfontIsBelow(s32 mark)
{
	return mark == LANGFONT_MARK_CEDILLA || mark == LANGFONT_MARK_OGONEK || mark == LANGFONT_MARK_COMMABELOW;
}

static s32 langfontSizeClass(s32 fontid)
{
	switch (fontid) {
	case LANGFONT_MD: return SIZE_M;
	case LANGFONT_LG: return SIZE_L;
	default: return SIZE_S;
	}
}

// Rows between a mark above and the letter under it.
static s32 langfontMarkGap(s32 fontid)
{
	return fontid == LANGFONT_MD || fontid == LANGFONT_LG ? 1 : 0;
}

/* ---- a working cell ---------------------------------------------------- */

#define CELL_W 16
#define CELL_H 40

struct cell {
	s32 top;      // y of row 0 of the ink, relative to the line (= baseline)
	s32 width;    // ink columns
	s32 height;   // ink rows
	u8 t[CELL_H + 2][CELL_W]; // [row + 1][col]: row -1 .. height is the cell
};

#define T(c, x, y) (c)->t[(y) + 1][(x)]

static void cellClear(struct cell *c)
{
	memset(c, 0, sizeof(*c));
}

/** A ROM glyph into a cell: ink row r at cell row r, band included. */
static void cellFromGlyph(struct cell *c, const struct fontchar *g)
{
	cellClear(c);
	c->top = g->baseline;
	c->width = g->width;
	c->height = g->height;

	for (s32 r = 0; r < g->height + 2 && r < CELL_H + 2; r++) {
		for (s32 x = 0; x < CELL_W; x++) {
			const u8 byte = g->pixeldata[r * 8 + x / 2];
			c->t[r][x] = (x & 1) ? (byte & 0xf) : (byte >> 4);
		}
	}
}

/**
 * Grows the cell to hold ink from line row y0 to y1 inclusive (either may be
 * outside it), keeping what it has at its line rows.
 */
static s32 cellGrow(struct cell *c, s32 y0, s32 y1)
{
	s32 newtop = y0 < c->top ? y0 : c->top;
	s32 newbottom = y1 > c->top + c->height - 1 ? y1 : c->top + c->height - 1;
	s32 shift = c->top - newtop;
	s32 newheight = newbottom - newtop + 1;

	if (newheight > CELL_H) {
		return 0;
	}

	if (shift > 0) {
		memmove(&c->t[shift][0], &c->t[0][0], (CELL_H + 2 - shift) * CELL_W);
		memset(&c->t[0][0], 0, shift * CELL_W);
	}

	c->top = newtop;
	c->height = newheight;

	return 1;
}

/** Widens the cell to w ink columns, moving what it has to the middle. */
static s32 cellWiden(struct cell *c, s32 w)
{
	s32 shift;

	if (w <= c->width) {
		return 1;
	}

	if (w > CELL_W - 2) {
		return 0;
	}

	shift = (w - c->width) / 2;

	for (s32 r = 0; r < CELL_H + 2; r++) {
		memmove(&c->t[r][shift], &c->t[r][0], CELL_W - shift);
		memset(&c->t[r][0], 0, shift);
	}

	c->width = w;

	return 1;
}

static s32 hexval(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	return 0;
}

static s32 markRows(const struct langfontmark *m)
{
	s32 n = 0;

	while (n < 4 && m->rows[n]) {
		n++;
	}

	return n;
}

static s32 markWidth(const struct langfontmark *m)
{
	return m->rows[0] ? (s32)strlen(m->rows[0]) : 0;
}

/** Body texels of rows at line row y0, ink column x0 (1-based like the cell). */
static void cellPaint(struct cell *c, const char *const *rows, s32 numrows, s32 x0, s32 y0)
{
	for (s32 r = 0; r < numrows; r++) {
		const char *row = rows[r];

		for (s32 i = 0; row[i]; i++) {
			const s32 v = hexval(row[i]);
			const s32 x = x0 + i;
			const s32 y = y0 + r - c->top;

			if (v >= 9 && x >= 1 && x <= CELL_W - 2 && y >= 0 && y < c->height) {
				T(c, x, y) = v;
			}
		}
	}
}

/**
 * The band round the body in line rows y0..y1: every texel beside body is
 * band at full cover, one only diagonal to it at a corner's cover. The band
 * already there is kept where it is more.
 */
static void cellBand(struct cell *c, s32 y0, s32 y1)
{
	const s32 r0 = y0 - c->top - 1;
	const s32 r1 = y1 - c->top + 1;

	for (s32 y = r0; y <= r1; y++) {
		if (y < -1 || y > c->height) {
			continue;
		}

		for (s32 x = 0; x < CELL_W; x++) {
			s32 side = 0;
			s32 corner = 0;
			s32 v;

			if (T(c, x, y) >= 9) {
				continue;
			}

			for (s32 dy = -1; dy <= 1; dy++) {
				for (s32 dx = -1; dx <= 1; dx++) {
					const s32 xx = x + dx;
					const s32 yy = y + dy;

					if ((dx || dy) && xx >= 0 && xx < CELL_W && yy >= -1 && yy <= c->height && T(c, xx, yy) >= 9) {
						if (dx == 0 || dy == 0) {
							side = 1;
						} else {
							corner = 1;
						}
					}
				}
			}

			v = side ? 7 : corner ? 4 : 0;

			if (v > T(c, x, y)) {
				T(c, x, y) = v;
			}
		}
	}
}

/** The line row of the top of a letter's ink, and of the row under it. */
static s32 inkTop(const struct cell *c)
{
	for (s32 y = 0; y < c->height; y++) {
		for (s32 x = 1; x <= c->width; x++) {
			if (T(c, x, y) >= 9) {
				return c->top + y;
			}
		}
	}

	return c->top;
}

static s32 inkBottom(const struct cell *c)
{
	for (s32 y = c->height - 1; y >= 0; y--) {
		for (s32 x = 1; x <= c->width; x++) {
			if (T(c, x, y) >= 9) {
				return c->top + y + 1;
			}
		}
	}

	return c->top + c->height;
}

/**
 * i and j lose their dot under a mark: the rows of the glyph above the
 * x-height are cut off, and the band remade along the new top.
 */
static void cellDotless(struct cell *c, s32 xheighttop)
{
	const s32 cut = xheighttop - c->top;

	if (cut <= 0 || cut >= c->height) {
		return;
	}

	memmove(&c->t[0][0], &c->t[cut][0], (CELL_H + 2 - cut) * CELL_W);
	c->top += cut;
	c->height -= cut;

	// what was the gap between dot and stem is band now, the dot's own faint
	// body included
	for (s32 x = 0; x < CELL_W; x++) {
		if (T(c, x, -1) >= 8) {
			T(c, x, -1) = 7;
		}
	}
}

static void cellMark(struct cell *c, s32 fontid, s32 mark, s32 stack)
{
	const struct langfontmark *m = &g_Marks[langfontSizeClass(fontid)][mark];
	const s32 mh = markRows(m);
	const s32 mw = markWidth(m);
	s32 x0;
	s32 y0;

	if (mh == 0 || !cellWiden(c, mw)) {
		return;
	}

	x0 = 1 + (c->width - mw + (mark == LANGFONT_MARK_ACUTE ? 1 : 0)) / 2;

	if (mark == LANGFONT_MARK_OGONEK) {
		x0 = c->width - mw + 1;
	}

	if (langfontIsBelow(mark)) {
		y0 = inkBottom(c);
	} else {
		y0 = inkTop(c) - langfontMarkGap(fontid) - mh;

		// a second mark goes over the first with a row between
		if (stack) {
			y0 -= 1;
		}
	}

	if (!cellGrow(c, y0, y0 + mh - 1)) {
		return;
	}

	cellPaint(c, m->rows, mh, x0, y0);
	cellBand(c, y0, y0 + mh - 1);
}

/** Two glyphs side by side, overlapping by as many columns as it takes. */
static s32 cellJoin(struct cell *c, const struct fontchar *a, const struct fontchar *b, s32 overlap)
{
	struct cell cb;
	s32 w;
	s32 top;
	s32 bottom;

	cellFromGlyph(c, a);
	cellFromGlyph(&cb, b);

	w = a->width + b->width - overlap;

	if (w > CELL_W - 2) {
		overlap += w - (CELL_W - 2);
		w = CELL_W - 2;

		if (overlap > 3) {
			return 0;
		}
	}

	top = a->baseline < b->baseline ? a->baseline : b->baseline;
	bottom = a->baseline + a->height > b->baseline + b->height ? a->baseline + a->height : b->baseline + b->height;

	if (!cellGrow(c, top, bottom - 1) || !cellGrow(&cb, top, bottom - 1)) {
		return 0;
	}

	for (s32 y = -1; y <= c->height; y++) {
		for (s32 x = 0; x < CELL_W; x++) {
			const s32 xs = x - (a->width - overlap);

			if (xs >= 0 && xs < CELL_W) {
				const u8 v = T(&cb, xs, y);

				if (v > T(c, x, y)) {
					T(c, x, y) = v;
				}
			}
		}
	}

	c->width = w;

	return 1;
}

/** A glyph turned half round, hung from the x-height (inverted ! and ?). */
static void cellTurn(struct cell *c, const struct fontchar *g, s32 xheighttop)
{
	struct cell src;

	cellFromGlyph(&src, g);
	cellClear(c);
	c->width = g->width;
	c->height = g->height;
	c->top = xheighttop > g->baseline ? xheighttop : g->baseline;

	for (s32 y = -1; y <= g->height; y++) {
		for (s32 x = 0; x <= g->width + 1; x++) {
			T(c, x, y) = T(&src, g->width + 1 - x, g->height - 1 - y);
		}
	}
}

static void cellDrawn(struct cell *c, const char *const *rows, s32 bottom)
{
	s32 n = 0;
	s32 w;

	while (n < 15 && rows[n]) {
		n++;
	}

	w = n ? (s32)strlen(rows[0]) : 0;

	cellClear(c);
	c->width = w;
	c->top = bottom - n;
	c->height = n;

	cellPaint(c, rows, n, 1, c->top);
	cellBand(c, c->top, c->top + n - 1);
}

/* ---- the cache -------------------------------------------------------- */

#define MAX_GLYPHS 4096
#define HASH_SIZE 8192

struct langfontglyph {
	struct fontchar fc;
	u32 cp;
	s8 fontid;
	u8 kernchar;
	u8 space;
	u8 fold;    // could not be made: drawn as the ASCII kernchar
	u8 used;
};

static struct langfontglyph g_Glyphs[MAX_GLYPHS];
static s32 g_NumGlyphs;
static u16 g_Hash[HASH_SIZE]; // index + 1

static u32 langfontHash(u32 cp, s32 fontid)
{
	return (cp * 2654435761u ^ (u32)(fontid + 1) * 40503u) & (HASH_SIZE - 1);
}

static struct langfontglyph *langfontFind(u32 cp, s32 fontid)
{
	u32 h = langfontHash(cp, fontid);

	while (g_Hash[h]) {
		struct langfontglyph *g = &g_Glyphs[g_Hash[h] - 1];

		if (g->cp == cp && g->fontid == fontid) {
			return g;
		}

		h = (h + 1) & (HASH_SIZE - 1);
	}

	return NULL;
}

static struct langfontglyph *langfontNew(u32 cp, s32 fontid)
{
	struct langfontglyph *g;
	u32 h;

	if (g_NumGlyphs >= MAX_GLYPHS) {
		return NULL;
	}

	g = &g_Glyphs[g_NumGlyphs++];
	g->cp = cp;
	g->fontid = fontid;
	g->used = 1;

	h = langfontHash(cp, fontid);

	while (g_Hash[h]) {
		h = (h + 1) & (HASH_SIZE - 1);
	}

	g_Hash[h] = g_NumGlyphs;

	return g;
}

/** The cell as a glyph whose pixels live for good. */
static s32 langfontStore(struct langfontglyph *g, const struct cell *c, s32 kerningindex)
{
	const s32 rows = c->height + 2;
	u8 *pix = calloc(rows * 8 + 16, 1);

	if (!pix) {
		return 0;
	}

	for (s32 r = 0; r < rows; r++) {
		for (s32 x = 0; x < CELL_W; x += 2) {
			pix[r * 8 + x / 2] = (u8)((c->t[r][x] & 0xf) << 4 | (c->t[r][x + 1] & 0xf));
		}
	}

	g->fc.index = 0;
	g->fc.baseline = c->top;
	g->fc.height = c->height;
	g->fc.width = c->width;
	g->fc.kerningindex = kerningindex;
	g->fc.pixeldata = pix;

	return 1;
}

static const struct langfontrecipe *langfontRecipe(u32 cp)
{
	s32 lo = 0;
	s32 hi = (s32)(sizeof(g_LangFontRecipes) / sizeof(g_LangFontRecipes[0])) - 1;

	while (lo <= hi) {
		const s32 mid = (lo + hi) / 2;

		if (g_LangFontRecipes[mid].cp == cp) {
			return &g_LangFontRecipes[mid];
		}

		if (g_LangFontRecipes[mid].cp < cp) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return NULL;
}

static struct fontchar *asciiGlyph(struct fontchar *chars, u8 c)
{
	if (c < 0x21 || c > 0x7e) {
		c = '?';
	}

	return &chars[c - 0x21];
}

/**
 * Builds the glyph for cp in a font this knows. 0 when it cannot, and the
 * caller falls back.
 */
static s32 langfontBuild(struct langfontglyph *g, struct fontchar *chars, s32 fontid, const struct langfontrecipe *r)
{
	struct cell c;
	const struct fontchar *base = asciiGlyph(chars, r->text[0]);
	const s32 xheighttop = chars['x' - 0x21].baseline;
	const s32 captop = chars['H' - 0x21].baseline;

	g->kernchar = r->text[0];

	switch (r->kind) {
	case KIND_MARK:
		cellFromGlyph(&c, base);

		if ((r->text[0] == 'i' || r->text[0] == 'j') && r->mark1 && !langfontIsBelow(r->mark1)) {
			cellDotless(&c, xheighttop);
		}

		if (r->mark1) {
			cellMark(&c, fontid, r->mark1, 0);
		}

		if (r->mark2) {
			cellMark(&c, fontid, r->mark2, !langfontIsBelow(r->mark1) && !langfontIsBelow(r->mark2));
		}

		return langfontStore(g, &c, base->kerningindex);
	case KIND_JOIN: {
		const s32 letters = r->text[0] >= 'A';

		if (!cellJoin(&c, base, asciiGlyph(chars, r->text[1]), letters ? 1 : 0)) {
			return 0;
		}

		return langfontStore(g, &c, base->kerningindex);
	}
	case KIND_TURN:
		cellTurn(&c, base, xheighttop);
		return langfontStore(g, &c, base->kerningindex);
	case KIND_DRAWN: {
		const s32 size = fontid == LANGFONT_XS ? 0 : fontid == LANGFONT_MD ? 2 : fontid == LANGFONT_LG ? 3 : 1;
		const struct fontchar *x = asciiGlyph(chars, 'x');

		cellDrawn(&c, g_SharpS[size], x->baseline + x->height);
		g->kernchar = 'B';
		return langfontStore(g, &c, asciiGlyph(chars, 'B')->kerningindex);
	}
	case KIND_MARKONLY: {
		const struct langfontmark *m = &g_Marks[langfontSizeClass(fontid)][r->mark1];
		const s32 mh = markRows(m);
		const struct fontchar *x = asciiGlyph(chars, 'x');
		const s32 y0 = langfontIsBelow(r->mark1) ? x->baseline + x->height : captop;

		cellClear(&c);
		c.width = markWidth(m);
		c.top = y0;
		c.height = mh;
		cellPaint(&c, m->rows, mh, 1, y0);
		cellBand(&c, y0, y0 + mh - 1);
		g->kernchar = '\'';
		return langfontStore(g, &c, asciiGlyph(chars, '\'')->kerningindex);
	}
	}

	return 0;
}

static struct langfontglyph *langfontCjk(struct fontchar *chars, s32 fontid, u32 cp)
{
	struct langfontglyph *g;
	u8 width;
	u8 height;
	s8 baseline;
	const u8 *pix = langfontCjkGlyph(cp, fontid == LANGFONT_LG, &width, &height, &baseline);

	if (!pix) {
		return NULL;
	}

	g = langfontNew(cp, fontid);

	if (!g) {
		return NULL;
	}

	g->fc.index = 0;
	g->fc.baseline = baseline;
	g->fc.height = height;
	g->fc.width = width;
	g->fc.kerningindex = chars ? chars['H' - 0x21].kerningindex : 0;
	g->fc.pixeldata = (u8 *)pix;
	g->kernchar = 'H';

	return g;
}

struct fontchar *langfontGlyph(struct fontchar *chars, s32 fontid, u32 cp, u8 *kernchar)
{
	const struct langfontrecipe *r;
	struct langfontglyph *g;

	if (fontid < 0 || fontid >= LANGFONT_NUM) {
		fontid = -1;
	}

	if (cp >= 0x21 && cp <= 0x7e) {
		*kernchar = cp;
		return &chars[cp - 0x21];
	}

	g = langfontFind(cp, fontid);

	if (g) {
		*kernchar = g->kernchar;

		if (g->fold) {
			return asciiGlyph(chars, g->kernchar);
		}

		return g->space ? NULL : &g->fc;
	}

	r = langfontRecipe(cp);

	if (r && r->kind == KIND_FOLD) {
		if (r->text[0] == ' ') {
			*kernchar = 'H';
			return NULL;
		}

		*kernchar = r->text[0];
		return asciiGlyph(chars, r->text[0]);
	}

	// In a Latin pack the ROM's letters come first; a Japanese glyph only for
	// what they cannot make.
	if (r && fontid >= 0 && (langpackScript() != LANGPACK_SCRIPT_CJK || !langfontCjkHas(cp))) {
		g = langfontNew(cp, fontid);

		if (g) {
			if (langfontBuild(g, chars, fontid, r)) {
				*kernchar = g->kernchar;
				return &g->fc;
			}

			// could not be made: drawn as its base letter from now on (the
			// font's own glyph, asked for each time - a font is reloaded
			// with every stage)
			g->used = 0;
			g->fold = 1;
			g->kernchar = r->text[0];
			*kernchar = g->kernchar;
			return asciiGlyph(chars, g->kernchar);
		}
	}

	if (langfontCjkHas(cp)) {
		g = langfontCjk(chars, fontid, cp);

		if (g) {
			*kernchar = g->kernchar;
			return &g->fc;
		}
	}

	if (r) {
		// a font this does not know: the bare letter
		*kernchar = r->text[0] == ' ' ? 'H' : r->text[0];
		return r->text[0] == ' ' ? NULL : asciiGlyph(chars, r->text[0]);
	}

	if (cp == 0x3000) { // ideographic space with no glyph
		*kernchar = 'H';
		return NULL;
	}

	*kernchar = '?';
	return asciiGlyph(chars, '?');
}

s32 langfontGlyphSlot(const struct fontchar *glyph)
{
	const struct langfontglyph *g = (const struct langfontglyph *)glyph;

	if (g < g_Glyphs || g >= g_Glyphs + g_NumGlyphs || !g->used) {
		return -1;
	}

	return g - g_Glyphs;
}

/* ---- line spacing ------------------------------------------------------ */

extern struct fontchar *g_CharsHandelGothicXs;
extern struct fontchar *g_CharsHandelGothicSm;
extern struct fontchar *g_CharsHandelGothicMd;

static struct fontchar *g_Spaced[LANGFONT_NUM];

static s32 langfontTakesSpacing(s32 fontid)
{
	return fontid == LANGFONT_SM || fontid == LANGFONT_XS || fontid == LANGFONT_MD;
}

static void langfontApplySpacing(struct fontchar *chars, s32 fontid)
{
	const s32 want = langpackNeedsAccentSpacing();

	if (!chars || !langfontTakesSpacing(fontid)) {
		return;
	}

	if (want && g_Spaced[fontid] != chars) {
		chars['|' - 0x21].baseline++;
		g_Spaced[fontid] = chars;
	} else if (!want && g_Spaced[fontid] == chars) {
		chars['|' - 0x21].baseline--;
		g_Spaced[fontid] = NULL;
	}
}

void langfontFontLoaded(struct fontchar *chars, s32 fontid)
{
	if (fontid >= 0 && fontid < LANGFONT_NUM) {
		// fresh out of the ROM, so not spaced whatever was here before
		if (g_Spaced[fontid] == chars) {
			g_Spaced[fontid] = NULL;
		}

		langfontApplySpacing(chars, fontid);
	}
}

void langfontLanguageChanged(void)
{
	langfontApplySpacing(g_CharsHandelGothicSm, LANGFONT_SM);
	langfontApplySpacing(g_CharsHandelGothicXs, LANGFONT_XS);
	langfontApplySpacing(g_CharsHandelGothicMd, LANGFONT_MD);
}

s32 langfontMinLineHeight(void)
{
	return langpackScript() == LANGPACK_SCRIPT_CJK ? 14 : 0;
}

/* ---- UTF-8 ------------------------------------------------------------ */

u32 langfontNextCodepoint(const char **text)
{
	const u8 *s = (const u8 *)*text;
	u32 cp;
	s32 n;

	if (s[0] < 0x80) {
		*text += 1;
		return s[0];
	}

	if ((s[0] & 0xe0) == 0xc0) {
		cp = s[0] & 0x1f;
		n = 1;
	} else if ((s[0] & 0xf0) == 0xe0) {
		cp = s[0] & 0x0f;
		n = 2;
	} else if ((s[0] & 0xf8) == 0xf0) {
		cp = s[0] & 0x07;
		n = 3;
	} else {
		*text += 1;
		return s[0]; // Latin-1
	}

	for (s32 i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*text += 1;
			return s[0]; // not UTF-8 after all: Latin-1
		}

		cp = (cp << 6) | (s[i] & 0x3f);
	}

	// overlong forms and surrogates are not UTF-8 either
	if ((n == 1 && cp < 0x80) || (n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp < 0xe000))) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff))) {
		*text += 1;
		return s[0];
	}

	*text += n + 1;

	return cp;
}

/* ---- line breaking and case ------------------------------------------- */

s32 langfontIsCjkBreakable(u32 cp)
{
	return (cp >= 0x2e80 && cp < 0xa000) || (cp >= 0xf900 && cp < 0xfb00) || (cp >= 0xff00 && cp < 0xffa0);
}

s32 langfontNoBreakBefore(u32 cp)
{
	switch (cp) {
	case 0x3001: case 0x3002: case 0xff0c: case 0xff0e: // 、。，．
	case 0x300d: case 0x300f: case 0x3011: case 0xff09: // 」』】）
	case 0x3009: case 0x300b: case 0x3015: case 0xff3d: // 〉》〕］
	case 0x30fc: case 0x301c: case 0xff5e:              // ー〜～
	case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: // small kana
	case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308e:
	case 0x30a1: case 0x30a3: case 0x30a5: case 0x30a7: case 0x30a9:
	case 0x30c3: case 0x30e3: case 0x30e5: case 0x30e7: case 0x30ee:
	case 0x30f5: case 0x30f6: case 0x309d: case 0x309e: case 0x30fd: case 0x30fe: case 0x3005:
	case 0xff01: case 0xff1f: case 0x30fb: case 0xff1a: case 0xff1b: // ！？・：；
	case 0x2026: case 0x2025:                           // … ‥
		return 1;
	}

	return cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == ')' || cp == ':' || cp == ';' || cp == '%';
}

s32 langfontNoBreakAfter(u32 cp)
{
	switch (cp) {
	case 0x300c: case 0x300e: case 0x3010: case 0xff08: // 「『【（
	case 0x3008: case 0x300a: case 0x3014: case 0xff3b: // 〈《〔［
		return 1;
	}

	return cp == '(';
}

void langfontToUpper(char *text)
{
	u8 *s = (u8 *)text;

	while (*s) {
		if (*s < 0x80) {
			if (*s >= 'a' && *s <= 'z') {
				*s -= 0x20;
			}

			s++;
		} else if (s[0] == 0xc3 && s[1] >= 0xa0 && s[1] <= 0xbe && s[1] != 0xb7) {
			// U+00E0-U+00FE less the division sign: capital is 0x20 below
			s[1] -= 0x20;
			s += 2;
		} else if (s[0] == 0xc3 && s[1] == 0xbf) {
			// y with diaeresis: U+0178
			s[0] = 0xc5;
			s[1] = 0xb8;
			s += 2;
		} else if (s[0] == 0xc5 && s[1] == 0x93) {
			// oe: U+0152
			s[1] = 0x92;
			s += 2;
		} else {
			const char *p = (const char *)s;

			langfontNextCodepoint(&p);
			s = (u8 *)p;
		}
	}
}

/* ---- for another renderer's fonts (GE Plus's, gexfront.c) --------------- */

s32 langfontRecipeOf(u32 cp, s32 *kind, const char **text, s32 *mark1, s32 *mark2)
{
	const struct langfontrecipe *r = langfontRecipe(cp);

	if (!r) {
		return 0;
	}

	*kind = r->kind;
	*text = r->text;
	*mark1 = r->mark1;
	*mark2 = r->mark2;

	return 1;
}

const char *const *langfontMarkRows(s32 sizeclass, s32 mark, s32 *width, s32 *height)
{
	const struct langfontmark *m;

	if (sizeclass < 0 || sizeclass > SIZE_L || mark <= 0 || mark >= LANGFONT_NUM_MARKS) {
		return NULL;
	}

	m = &g_Marks[sizeclass][mark];
	*width = markWidth(m);
	*height = markRows(m);

	return *height ? m->rows : NULL;
}

s32 langfontMarkIsBelow(s32 mark)
{
	return langfontIsBelow(mark);
}

const char *const *langfontSharpSRows(s32 sizeclass, s32 *width, s32 *height)
{
	const char *const *rows = g_SharpS[sizeclass == SIZE_M ? 2 : sizeclass == SIZE_L ? 3 : 1];
	s32 n = 0;

	while (n < 15 && rows[n]) {
		n++;
	}

	*width = n ? (s32)strlen(rows[0]) : 0;
	*height = n;

	return rows;
}

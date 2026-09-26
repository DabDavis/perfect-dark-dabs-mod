/**
 * The folder screens' art out of the GoldenEye XBLA release (gexfront.c).
 *
 * GE Plus draws GoldenEye's own folder: its model, its photograph, its stamps
 * and its mission slides, converted out of the player's ROM (geconvert.c) and
 * laid out the way GoldenEye lays them out. The release has the same folder,
 * and 4J redrew every picture on it - the cover is 1124x714 against the ROM's
 * four 65x65 tiles, the paper 512x512 against a 64x64 one, "CONFIDENTIAL" a
 * 1024x128 stamp against two 96x32 halves.
 *
 * What is *not* different is the folder itself. Rare built the release on the
 * cartridge, so the geometry, the layout and the UVs are GoldenEye's own, and
 * a picture of 4J's is the same picture larger. So this does not draw the
 * release's model: it repaints GoldenEye's with the release's art, one texture
 * at a time, and everything gexfront.c measures against the folder - the tabs,
 * the cursor, the text frames, the mission grid - stands exactly where it did.
 *
 * The swap is one entry per texture in xblatex's registry, bound at the
 * address the model's own display lists already name
 * (xblaTexBindPictureAt()), so nothing is rebuilt and forgetting it puts the
 * ROM's own texels back. It follows the release's meshes (F6).
 *
 * **A picture is not always the whole of a texture.** GoldenEye splits what
 * will not fit in TMEM: the photograph of Bond is four 65x65 quarters, each
 * stamp is halves or quarters of a word, and the paper and the cover are one
 * small tile repeated across the page. The release keeps each of those whole,
 * so a row here names the rectangle of the release's picture that stands for
 * the ROM's texture - quarters for the photograph, halves for a word - and
 * for a tile that repeats, a patch of the release's picture the size the ROM's
 * tile covers.
 *
 * **And a texture is wider than it looks.** The N64 loads whole 64-bit lines,
 * so a 65 texel wide 4-bit texture is 80 texels of data with the picture in
 * the left 65 and the rest never sampled - and the renderer normalises the
 * model's texture coordinates by that 80. A replacement is stretched across
 * the whole tile, so each one is padded out here the same way, with its last
 * column repeated into the padding.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"
#include "system.h"
#include "video.h"
#include "gebean.h"
#include "gefolder.h"
#include "xblamesh.h"
#include "xblatex.h"
#include "pngwrite.h"
#include "fs.h"
#include "gbiex.h"
#include "constants.h"
#include "game/tex.h"
#include "lib/model.h"

#ifndef PLATFORM_N64

#define GEFOLDER_SOURCE "new/prop/walletbond"

// Every texture of the converted folder model, in the order the file carries
// them; a row is checked against the model's own before it is used.
#define GEFOLDER_MAXROWS 96

struct gefolderrow {
	s16 config;   // which of the model's textures this is
	s16 width;    // and what it must be, or the file is not the one we know
	s16 height;
	s16 picture;  // the release's picture that carries it
	s16 tiles;    // whether the ROM repeats this one across the page
	s16 mask;     // the ROM's is a cutout and the release's carries no alpha:
	              // take the release's brightness as the alpha and draw it white
	s16 onpaper;  // it carries the page behind it, so its own paper is brought
	              // to the colour the page is drawn in (geFolderMatchPaper());
	              // 2: it lies over exactly one repeat of the page's paper tile,
	              // so its ink is printed on that tile (geFolderPrintOnPaper())
	f32 u0, v0, u1, v1; // the part of it that is this texture, as it is seen
	s16 grey;     // the ROM's is an intensity texture the node's shade colours:
	              // bring the release's to the ROM's brightness, neutral in hue
	              // (geFolderMatchGrey())
};

#define WHOLE 0.0f, 0.0f, 1.0f, 1.0f

/**
 * The folder's own pictures. The photograph and the stamps are named by the
 * ROM's texture ids in the comments, since that is what the conversion writes
 * and what `--dump-texture` takes.
 *
 * The mission slides and the briefing photographs are the two long runs at the
 * end, and the release keeps them at their own numbers, so those are filled in
 * below rather than written out one by one.
 */
static const struct gefolderrow folderRows[] = {
	// config, w, h, picture, tiles, mask, onpaper, u0, v0, u1, v1

	// The paperclip and the cursor's shadow: the ROM's are cutouts and the
	// release's carry no alpha of their own, so they are drawn as masks
	{  4, 32, 64, 66, 0, 1, 0, WHOLE },                              // 0x03f6, the paperclip
	{ 17, 32, 32, 64, 0, 1, 0, WHOLE },                              // 0x04fd
	{ 23, 16, 16, 67, 0, 0, 0, WHOLE },                              // 0x0a47, the film strip's hole

	// The royal crest, four quarters (0x0a26-0x0a29) of the one the release
	// has printed on its page. The ROM's quarters carry the paper behind the
	// crest, so they are taken off the release's page and brought to the
	// colour the page is drawn in (geFolderMatchPaper()) - otherwise the crest
	// stands on a square of a slightly different paper.
	{  0, 65, 65,  2, 0, 0, 1, 0.7250f, 0.0300f, 0.8500f, 0.1250f },
	{  1, 65, 65,  2, 0, 0, 1, 0.8500f, 0.0300f, 0.9750f, 0.1250f },
	{  2, 65, 65,  2, 0, 0, 1, 0.7250f, 0.1250f, 0.8500f, 0.2200f },
	{  3, 65, 65,  2, 0, 0, 1, 0.8500f, 0.1250f, 0.9750f, 0.2200f },
	//
	// The whole crest is the one the menu draws, and its square lies over the
	// page with the page's own coordinates running on across it: one repeat
	// of the paper tile, from the tile's corner, under the same shade. A
	// picture of the crest on 4J's paper stood out from the page as a square
	// of flatter, yellower paper, so its ink alone is printed on the tile
	// the page is drawn with, and its edge meets the page texel for texel.
	{ 12, 64, 64,  2, 0, 0, 2, 0.7250f, 0.0300f, 0.9750f, 0.2200f, 1 }, // 0x0a46, whole

	// Brosnan, in four quarters (0x0a2a-0x0a2d) of the release's 256x512
	{  5, 65, 65, 23, 0, 0, 0, 0.0f, 0.0f, 0.5f, 0.5f },             // 0x0a2a
	{  6, 65, 65, 23, 0, 0, 0, 0.5f, 0.0f, 1.0f, 0.5f },             // 0x0a2b
	{  7, 65, 65, 23, 0, 0, 0, 0.0f, 0.5f, 0.5f, 1.0f },             // 0x0a2c
	{  8, 65, 65, 23, 0, 0, 0, 0.5f, 0.5f, 1.0f, 1.0f },             // 0x0a2d

	// The cover and the paper are one tile repeated over the page, so what
	// stands for them is a patch of the release's own picture - which covers
	// the page once - the size that tile covers, cross-faded into itself so
	// that the page is not a grid of the same blotch (geFolderMakeSeamless()).
	// Both are the flattest patch of their picture, found by walking it for
	// the square with the least grain and the least shading across it: a
	// crumple or a fold repeated sixty-four times over a page is a pattern,
	// and paper is the one thing here nobody should be able to see repeat.
	//
	// Both of GoldenEye's are intensity textures - grey, 4 bits - which the
	// model colours with its vertex shade: the backdrop beige, the page khaki.
	// The release's are the colour they are to be seen in, so bound as they
	// come they are coloured twice, and the page came out the olive of the
	// cover it lies on. So each is brought to its ROM tile's own brightness
	// and left grey, and the shade colours it as it colours the ROM's.
	{ 10, 64, 64,  0, 1, 0, 0, 0.2420f, 0.1667f, 0.3665f, 0.3627f, 1 }, // 0x0a42, the cover
	{ 11, 64, 64,  1, 1, 0, 0, 0.4688f, 0.8125f, 0.5938f, 0.9375f, 1 }, // 0x0a45, the paper

	// The stamps. The briefing's OHMSS title is white on black in the release
	// and a cutout in the ROM, so it goes on as a mask like the paperclip; the
	// red ones carry their own alpha.
	//
	// GoldenEye cuts a stamp into pieces of equal width and fills each one with
	// its own share of the word, so a piece is not a share of the release's
	// picture: "FOR" fills its 95 texels there and an eighth of the release's
	// 1024 here. Every one of these is the ink measured on both sides - the
	// release's own words, and its halves of a word GoldenEye halves.
	{  9, 95, 32, 65, 0, 1, 0, 0.0000f, 0.0625f, 0.5000f, 0.9844f }, // 0x0a43 "OHM"
	{ 18, 95, 32, 65, 0, 1, 0, 0.5000f, 0.0625f, 1.0000f, 0.9844f }, // 0x0a44 "SS"
	{ 21, 95, 32, 70, 0, 0, 0, 0.0762f, 0.0078f, 0.4951f, 1.0000f }, // 0x0a3a "CLASS"
	{ 22, 95, 32, 70, 0, 0, 0, 0.4951f, 0.0078f, 0.9141f, 1.0000f }, // 0x0a3b "SIFIED"
	{ 19, 95, 32, 69, 0, 0, 0, 0.0742f, 0.0234f, 0.4624f, 0.9609f }, // 0x0a3c "CONFID"
	{ 20, 95, 32, 69, 0, 0, 0, 0.4624f, 0.0234f, 0.8506f, 0.9609f }, // 0x0a3d "ENTIAL"
	{ 14, 95, 32, 68, 0, 0, 0, 0.5049f, 0.0234f, 0.7285f, 0.9609f }, // 0x0a3e "EYES"
	{ 15, 95, 32, 68, 0, 0, 0, 0.7598f, 0.0234f, 0.9990f, 0.9609f }, // 0x0a3f "ONLY"
	{ 13, 95, 32, 68, 0, 0, 0, 0.0068f, 0.0234f, 0.1855f, 0.9609f }, // 0x0a40 "FOR"
	{ 16, 95, 32, 68, 0, 0, 0, 0.2100f, 0.0234f, 0.4727f, 0.9609f }, // 0x0a41 "YOUR"
};

// The two long runs: the briefing photographs (0x09ea on, 128x64) and the
// mission slides (0x0a12 on, 68x44).
#define GEFOLDER_BRIEF_FIRST   24
#define GEFOLDER_BRIEF_COUNT   40
#define GEFOLDER_SLIDE_FIRST   64
#define GEFOLDER_SLIDE_COUNT   20

/**
 * Which of the release's pictures each of them is.
 *
 * The release has the same shots - 4J rendered them again rather than redrawing
 * them, and every one matches GoldenEye's to a correlation of .99 - but not in
 * GoldenEye's order: its pair of briefing photographs for a mission is the
 * other way round, and its slides are shuffled. So both runs are matched
 * picture against picture rather than taken in order, which is what stopped
 * Dam's slide being Jungle's.
 */
static const u8 slidePicture[GEFOLDER_SLIDE_COUNT] = {
	13, 18,  4, 21, 19, 20, 22,  3, 15,  9,
	17, 14,  5,  7, 11,  6, 10,  8, 12, 16,
};

static const u8 briefPicture[GEFOLDER_BRIEF_COUNT] = {
	45, 25, 24, 44, 61, 60, 57, 56, 59, 58,
	63, 62, 49, 48, 37, 36, 53, 52, 47, 46,
	29, 28, 33, 32, 41, 40, 31, 30, 39, 38,
	35, 34, 43, 42, 51, 50, 27, 26, 55, 54,
};

// What was bound, so it can be put back.
static const void *bound[GEFOLDER_MAXROWS];
static s32 numBound;
static s32 warned;

/**
 * The texels a line of this texture really holds: the N64 loads whole 64-bit
 * lines (texGetLineSizeInBytes()), so a 65 texel 4-bit row is 80 texels wide
 * and the model's coordinates are measured against the 80.
 */
static s32 geFolderPaddedWidth(s32 width, s32 depth)
{
	switch (depth) {
	case G_IM_SIZ_32b:
	case G_IM_SIZ_16b:
		return (width + 3) & ~3;
	case G_IM_SIZ_8b:
		return (width + 7) & ~7;
	default:
		return (width + 15) & ~15;
	}
}

/**
 * The colour the page is drawn in: the mean of the patch that stands in for
 * the paper tile, worked out before anything is bound so that the pictures
 * which carry paper behind them can be brought to it.
 */
static s32 paperColour[3];
static s32 havePaperColour;

static void geFolderMeanOf(const u8 *rgba, s32 w, s32 h, s32 border, s32 *out)
{
	s64 sum[3] = { 0, 0, 0 };
	s64 n = 0;

	for (s32 y = 0; y < h; y++) {
		for (s32 x = 0; x < w; x++) {
			// The border alone, for a picture whose middle is the crest and
			// whose edges are the paper it is printed on
			if (border && x >= border && x < w - border && y >= border && y < h - border) {
				continue;
			}

			const u8 *px = rgba + ((size_t)y * w + x) * 4;

			for (s32 k = 0; k < 3; k++) {
				sum[k] += px[k];
			}

			n++;
		}
	}

	for (s32 k = 0; k < 3; k++) {
		out[k] = n ? (s32)(sum[k] / n) : 0;
	}
}

/**
 * Brings the paper a picture is printed on to the colour the page is drawn in.
 *
 * GoldenEye's crest is a picture of the crest *on paper*, four quarters of it,
 * and the page under it is the paper tile repeated. Taking the crest off the
 * release's own page and leaving it there puts a square of a slightly
 * different paper in the middle of the page, so the whole crop is shifted by
 * what its edges are away from the page's own colour.
 */
static void geFolderMatchPaper(u8 *rgba, s32 w, s32 h)
{
	s32 edge[3];
	s32 shift[3];

	if (!havePaperColour) {
		return;
	}

	geFolderMeanOf(rgba, w, h, w < h ? w / 8 : h / 8, edge);

	for (s32 k = 0; k < 3; k++) {
		shift[k] = paperColour[k] - edge[k];
	}

	for (s32 i = 0; i < w * h; i++) {
		u8 *px = rgba + (size_t)i * 4;

		for (s32 k = 0; k < 3; k++) {
			const s32 v = px[k] + shift[k];

			px[k] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
}

/**
 * Takes the shading out of a patch that is going to be repeated, leaving its
 * grain.
 *
 * A fold or a shadow in the patch is a fold in every one of the sixty-four
 * copies of it on the page, and the eye reads that as a woven pattern even
 * where the seams are gone. What is wanted from the release's paper here is
 * the paper, not the crumple of the one square it was taken from, so
 * everything coarser than an eighth of the patch is subtracted and the mean
 * put back.
 */
static void geFolderFlatten(u8 *rgba, s32 w, s32 h)
{
	const s32 r = (w < h ? w : h) / 8;
	s32 mean[3];
	u8 *copy;

	if (r < 1) {
		return;
	}

	copy = malloc((size_t)w * h * 4);

	if (!copy) {
		return;
	}

	memcpy(copy, rgba, (size_t)w * h * 4);
	geFolderMeanOf(copy, w, h, 0, mean);

	for (s32 y = 0; y < h; y++) {
		for (s32 x = 0; x < w; x++) {
			s32 sum[3] = { 0, 0, 0 };
			s32 n = 0;

			// The blur wraps, as the patch itself does on the page
			for (s32 dy = -r; dy <= r; dy += 2) {
				const s32 sy = ((y + dy) % h + h) % h;

				for (s32 dx = -r; dx <= r; dx += 2) {
					const s32 sx = ((x + dx) % w + w) % w;
					const u8 *px = copy + ((size_t)sy * w + sx) * 4;

					for (s32 k = 0; k < 3; k++) {
						sum[k] += px[k];
					}

					n++;
				}
			}

			u8 *out = rgba + ((size_t)y * w + x) * 4;

			for (s32 k = 0; k < 3; k++) {
				const s32 v = out[k] - (n ? sum[k] / n : 0) + mean[k];

				out[k] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
			}
		}
	}

	free(copy);
}

/**
 * A patch that is going to be repeated across a page is cross-faded into
 * itself, so that its right edge is its left and its bottom is its top.
 *
 * The cover and the paper are one small tile repeated in GoldenEye and one
 * page-sized picture in the release, so what stands in for the tile is a patch
 * of that picture - and a patch of crumpled paper does not meet itself. The
 * fade is over an eighth of the patch and is what keeps the page from being a
 * grid of the same blotch.
 */
static void geFolderMakeSeamless(u8 *rgba, s32 w, s32 h)
{
	const s32 fx = w / 8;
	const s32 fy = h / 8;

	for (s32 y = 0; y < h; y++) {
		for (s32 x = 0; x < fx; x++) {
			u8 *a = rgba + ((size_t)y * w + x) * 4;
			const u8 *b = rgba + ((size_t)y * w + (w - fx + x)) * 4;
			const s32 t = (fx > 1) ? 255 * x / (fx * 2) : 0; // half weight at the seam

			for (s32 k = 0; k < 4; k++) {
				a[k] = (u8)((a[k] * (255 - (127 - t)) + b[k] * (127 - t) + 127) / 255);
			}
		}
	}

	for (s32 y = 0; y < fy; y++) {
		for (s32 x = 0; x < w; x++) {
			u8 *a = rgba + ((size_t)y * w + x) * 4;
			const u8 *b = rgba + ((size_t)(h - fy + y) * w + x) * 4;
			const s32 t = (fy > 1) ? 255 * y / (fy * 2) : 0;

			for (s32 k = 0; k < 4; k++) {
				a[k] = (u8)((a[k] * (255 - (127 - t)) + b[k] * (127 - t) + 127) / 255);
			}
		}
	}
}

/**
 * The mean brightness, 0-255, of a ROM intensity texture as loaded, and how
 * far its texels stray from it: I4 or I8 texels, rows padded to whole 64-bit
 * lines. 0 for any other depth.
 */
static s32 geFolderRomGrey(const struct textureconfig *tc, s32 *mean, s32 *spread)
{
	const s32 w = geFolderPaddedWidth(tc->width, tc->depth);
	const u8 *texels = tc->textureptr;
	s64 sum = 0;
	s64 sumsq = 0;
	s64 n = 0;

	if (!texels || (tc->depth != G_IM_SIZ_4b && tc->depth != G_IM_SIZ_8b)) {
		return 0;
	}

	for (s32 y = 0; y < tc->height; y++) {
		for (s32 x = 0; x < tc->width; x++) {
			s32 v;

			if (tc->depth == G_IM_SIZ_4b) {
				const u8 b = texels[((size_t)y * w + x) / 2];

				v = ((x & 1) ? (b & 0xf) : (b >> 4)) * 17;
			} else {
				v = texels[(size_t)y * w + x];
			}

			sum += v;
			sumsq += v * v;
			n++;
		}
	}

	if (!n) {
		return 0;
	}

	*mean = (s32)(sum / n);
	*spread = (s32)sqrtf((f32)(sumsq / n - (sum / n) * (sum / n)));

	return 1;
}

/**
 * Makes a picture of the release's stand in for a ROM intensity texture: its
 * brightness brought to the ROM texture's, and its grain to the ROM texture's
 * spread about it. The picture's own detail stays; its overall colour goes,
 * since the node's shade supplies that, as it does for the ROM's - and its
 * grain is not magnified with its brightness, which turned a slightly
 * mottled brown paper into a camouflage beige one.
 */
static void geFolderMatchGrey(u8 *rgba, s32 w, s32 h, s32 grey, s32 spread)
{
	s32 mean[3];
	f64 sumsq = 0;
	f32 gain;

	geFolderMeanOf(rgba, w, h, 0, mean);

	for (s32 i = 0; i < w * h; i++) {
		const u8 *px = rgba + (size_t)i * 4;
		const f32 d = ((px[0] - mean[0]) + (px[1] - mean[1]) + (px[2] - mean[2])) / 3.0f;

		sumsq += d * d;
	}

	gain = sumsq > 0 ? spread / sqrtf((f32)(sumsq / ((f64)w * h))) : 0.0f;

	for (s32 i = 0; i < w * h; i++) {
		u8 *px = rgba + (size_t)i * 4;

		for (s32 k = 0; k < 3; k++) {
			const s32 v = grey + (s32)((px[k] - mean[k]) * gain);

			px[k] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
}

/**
 * The rectangle of `src` a row names, padded out to the shape of the tile it
 * is standing in for. RGBA32 throughout, malloc'd, the caller's.
 *
 * A row's rectangle is written the way the picture is *seen* - the top left of
 * the photograph is (0,0) - and the release's picture arrives in the game's
 * own row order, which is the other way up, so v is turned over here.
 */
static u8 *geFolderCrop(const u8 *src, s32 srcw, s32 srch, const struct gefolderrow *row,
		s32 tilewidth, s32 paddedwidth, s32 *outw, s32 *outh)
{
	s32 x0 = (s32)(row->u0 * srcw + 0.5f);
	s32 y0 = (s32)((1.0f - row->v1) * srch + 0.5f);
	s32 x1 = (s32)(row->u1 * srcw + 0.5f);
	s32 y1 = (s32)((1.0f - row->v0) * srch + 0.5f);
	s32 cw, ch, w;
	u8 *out;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > srcw) x1 = srcw;
	if (y1 > srch) y1 = srch;

	cw = x1 - x0;
	ch = y1 - y0;

	if (cw <= 0 || ch <= 0) {
		return NULL;
	}

	// The picture keeps its own scale and the padding is added beside it, so
	// the part of the tile the model samples is the part that carries it.
	w = tilewidth > 0 ? (s32)(((s64)cw * paddedwidth + tilewidth / 2) / tilewidth) : cw;

	if (w < cw) {
		w = cw;
	}

	out = malloc((size_t)w * ch * 4);

	if (!out) {
		return NULL;
	}

	for (s32 y = 0; y < ch; y++) {
		const u8 *in = src + (((size_t)(y0 + y) * srcw) + x0) * 4;
		u8 *dst = out + (size_t)y * w * 4;

		memcpy(dst, in, (size_t)cw * 4);

		for (s32 x = cw; x < w; x++) {
			memcpy(dst + (size_t)x * 4, in + (size_t)(cw - 1) * 4, 4);
		}
	}

	if (row->tiles) {
		geFolderFlatten(out, w, ch);
		geFolderMakeSeamless(out, w, ch);
	}

	if (row->onpaper) {
		geFolderMatchPaper(out, w, ch);
	}

	// A cutout of the ROM's against a picture of the release's that is opaque
	// from edge to edge: what the release draws the shape with is its
	// brightness, so that becomes the alpha and the colour is left white for
	// the node's own combiner to tint, as it tints the ROM's.
	if (row->mask) {
		for (s32 i = 0; i < w * ch; i++) {
			u8 *px = out + (size_t)i * 4;
			const s32 lum = px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2]) : (px[1] > px[2] ? px[1] : px[2]);

			px[0] = px[1] = px[2] = 0xff;
			px[3] = (u8)lum;
		}
	}

	*outw = w;
	*outh = ch;

	return out;
}

// The picture last decoded, since the rows that share one are written together
static u8 *lastPicture;
static s32 lastIndex = -1;
static s32 lastWidth;
static s32 lastHeight;

static void geFolderDropCache(void)
{
	free(lastPicture);
	lastPicture = NULL;
	lastIndex = -1;
}

static const u8 *geFolderPicture(struct gebeanpictures *pics, s32 index, s32 *w, s32 *h)
{
	if (index != lastIndex || !lastPicture) {
		geFolderDropCache();
		lastPicture = gebeanPicturesDecode(pics, index, &lastWidth, &lastHeight);
		lastIndex = lastPicture ? index : -1;
	}

	*w = lastWidth;
	*h = lastHeight;

	return lastPicture;
}

// The paper tile as it is bound, for the crest to be printed on
static u8 *paperTile;
static s32 paperTileWidth;
static s32 paperTileHeight;

/**
 * Prints a picture's ink on the page's paper tile: whatever of it is darker
 * than its own paper - by more than that paper's own grain - is taken off the
 * tile at the same place, and the rest of it is the tile. The picture stands
 * over one repeat of the tile, so texel (x, y) of it is texel (x, y) of the
 * tile scaled to its size.
 */
static void geFolderPrintOnPaper(u8 *rgba, s32 w, s32 h)
{
	const s32 border = (w < h ? w : h) / 8;
	s32 edge[3];
	s32 level;
	f64 sumsq = 0;
	s64 n = 0;
	s32 grain;

	if (!paperTile || w <= 0 || h <= 0) {
		return;
	}

	geFolderMeanOf(rgba, w, h, border, edge);
	level = (edge[0] + edge[1] + edge[2]) / 3;

	for (s32 y = 0; y < h; y++) {
		for (s32 x = 0; x < w; x++) {
			if (x >= border && x < w - border && y >= border && y < h - border) {
				continue;
			}

			const u8 *px = rgba + ((size_t)y * w + x) * 4;
			const s32 d = (px[0] + px[1] + px[2]) / 3 - level;

			sumsq += d * d;
			n++;
		}
	}

	grain = n ? (s32)sqrtf((f32)(sumsq / n)) : 0;

	for (s32 y = 0; y < h; y++) {
		for (s32 x = 0; x < w; x++) {
			u8 *px = rgba + ((size_t)y * w + x) * 4;
			const u8 *pp = paperTile + (((size_t)(y * paperTileHeight / h) * paperTileWidth)
					+ x * paperTileWidth / w) * 4;
			s32 ink = level - (px[0] + px[1] + px[2]) / 3 - grain;

			if (ink < 0) {
				ink = 0;
			}

			for (s32 k = 0; k < 3; k++) {
				const s32 v = pp[k] - ink;

				px[k] = (u8)(v < 0 ? 0 : v);
			}

			px[3] = 0xff;
		}
	}
}

/**
 * A row's picture as it is to be bound, malloc'd and the caller's, or NULL
 * where the model's texture is not the one the row expects.
 */
static u8 *geFolderMake(struct gebeanpictures *pics, const struct modeldef *modeldef,
		const struct gefolderrow *row, s32 *outw, s32 *outh)
{
	const struct textureconfig *tc;
	s32 srcw = 0, srch = 0;
	const u8 *src;
	u8 *crop;

	if (row->config < 0 || row->config >= modeldef->numtexconfigs) {
		return NULL;
	}

	tc = &modeldef->texconfigs[row->config];

	// A conversion that is not the one these rows were written against: the
	// folder keeps the ROM's art rather than taking a picture meant for
	// another texture.
	if (tc->width != row->width || tc->height != row->height || !tc->textureptr) {
		if (!warned) {
			warned = 1;
			sysLogPrintf(LOG_WARNING, "gefolder: the folder's texture %d is %dx%d and this "
					"expects %dx%d - the conversion is not the one these rows were written "
					"against, and the folder keeps the ROM's own art",
					row->config, tc->width, tc->height, row->width, row->height);
		}

		return NULL;
	}

	src = geFolderPicture(pics, row->picture, &srcw, &srch);

	if (!src) {
		return NULL;
	}

	crop = geFolderCrop(src, srcw, srch, row, tc->width,
			geFolderPaddedWidth(tc->width, tc->depth), outw, outh);

	if (!crop) {
		return NULL;
	}

	if (row->grey) {
		s32 grey, spread;

		if (geFolderRomGrey(tc, &grey, &spread)) {
			geFolderMatchGrey(crop, *outw, *outh, grey, spread);
		}
	}

	if (row->onpaper == 2) {
		geFolderPrintOnPaper(crop, *outw, *outh);
	}

	return crop;
}

static void geFolderBind(struct gebeanpictures *pics, const struct modeldef *modeldef,
		const struct gefolderrow *row)
{
	const struct textureconfig *tc;
	s32 outw = 0, outh = 0;
	u8 *crop;

	if (numBound >= GEFOLDER_MAXROWS) {
		return;
	}

	crop = geFolderMake(pics, modeldef, row, &outw, &outh);

	if (!crop) {
		return;
	}

	tc = &modeldef->texconfigs[row->config];

	// Taken over by the registry, or freed there.
	if (xblaTexBindPictureAt(tc->textureptr, crop, outw, outh)) {
		bound[numBound++] = tc->textureptr;

		// The renderer keeps a texture by the address it was uploaded from, so
		// whatever it has for this one is the ROM's picture and has to go.
		videoFreeCachedTexture(tc->textureptr);
	}
}

struct gebeanpictures;
static s32 geFolderBeanBuild(struct gebeanpictures *pics, struct modeldef *modeldef);

s32 geFolderRepaint(struct modeldef *modeldef)
{
	struct gebeanpictures *pics;
	struct gefolderrow row;

	geFolderForget();
	warned = 0;

	if (!modeldef || !modeldef->texconfigs || !gebeanGetEnabled() || !xblaMeshGetEnabled()) {
		return 0;
	}

	pics = gebeanPicturesOpen(GEFOLDER_SOURCE);

	if (!pics) {
		return 0;
	}

	// --dump-folder-pictures: every picture the release's folder carries, as
	// $E/gefolder_NN.png, which is what the table above was written against
	// and what to look at when one of its rows is wrong.
	if (sysArgCheck("--dump-folder-pictures")) {
		for (s32 i = 0; i < gebeanPicturesCount(pics); i++) {
			s32 w = 0, h = 0;
			u8 *rgba = gebeanPicturesDecode(pics, i, &w, &h);
			char path[256];

			if (rgba) {
				snprintf(path, sizeof(path), "$E/gefolder_%02d.png", i);
				pngWrite(fsFullPath(path), rgba, w, h, 4, 0);
				free(rgba);
			}
		}
	}

	// The page's own colour, before anything is bound: the crest is printed on
	// paper and has to match it (geFolderMatchPaper())
	havePaperColour = 0;

	for (s32 i = 0; i < ARRAYCOUNT(folderRows); i++) {
		const struct gefolderrow *row = &folderRows[i];
		s32 w = 0, h = 0;
		const u8 *rgba;
		u8 *patch;

		if (!row->tiles || row->picture != 1) {
			continue;
		}

		rgba = geFolderPicture(pics, row->picture, &w, &h);

		if (!rgba) {
			continue;
		}

		patch = geFolderCrop(rgba, w, h, row, 0, 0, &w, &h);

		if (patch) {
			geFolderMeanOf(patch, w, h, 0, paperColour);
			havePaperColour = 1;
			free(patch);
		}
	}

	// The paper tile itself, as it will be bound, for the crest to be printed on
	for (s32 i = 0; i < ARRAYCOUNT(folderRows) && !paperTile; i++) {
		if (folderRows[i].tiles && folderRows[i].picture == 1) {
			paperTile = geFolderMake(pics, modeldef, &folderRows[i], &paperTileWidth, &paperTileHeight);
		}
	}

	for (s32 i = 0; i < ARRAYCOUNT(folderRows); i++) {
		geFolderBind(pics, modeldef, &folderRows[i]);
	}

	free(paperTile);
	paperTile = NULL;

	// The briefing photographs and the mission slides, which the release keeps
	// in the model's own order.
	for (s32 i = 0; i < GEFOLDER_BRIEF_COUNT; i++) {
		row.config = (s16)(GEFOLDER_BRIEF_FIRST + i);
		row.width = 128;
		row.height = 64;
		row.picture = (s16)briefPicture[i];
		row.tiles = 0;
		row.mask = 0;
		row.onpaper = 0;
		row.u0 = 0.0f;
		row.v0 = 0.0f;
		row.u1 = 1.0f;
		row.v1 = 1.0f;
		row.grey = 0;
		geFolderBind(pics, modeldef, &row);
	}

	for (s32 i = 0; i < GEFOLDER_SLIDE_COUNT; i++) {
		row.config = (s16)(GEFOLDER_SLIDE_FIRST + i);
		row.width = 68;
		row.height = 44;
		row.picture = (s16)slidePicture[i];
		row.tiles = 0;
		row.mask = 0;
		row.onpaper = 0;
		row.u0 = 0.0f;
		row.v0 = 0.0f;
		row.u1 = 1.0f;
		row.v1 = 1.0f;
		row.grey = 0;
		geFolderBind(pics, modeldef, &row);
	}

	geFolderDropCache();

	// and the release's own folder over it, where it stands on GoldenEye's
	geFolderBeanBuild(pics, modeldef);

	gebeanPicturesClose(pics);

	sysLogPrintf(LOG_NOTE, "gefolder: %d of the folder's pictures are the release's", numBound);

	return numBound;
}

/**
 * The menus' pictures that are not on the folder model - the portraits, the
 * stage pictures, the cursor - are files of their own in the release
 * (files/texture/), drawn by gexfront.c as rectangles of their own. Each is
 * decoded the first time it is asked for and bound as a stand-in for the life
 * of the game, so this is a lookup after that; a picture the release has not
 * got is remembered as missing and GoldenEye's own is drawn.
 */
#define GEFOLDER_MAXMENU 96

static struct {
	char name[40];
	const void *tile;
	s32 width;
	s32 height;
} menuPictures[GEFOLDER_MAXMENU];
static s32 numMenuPictures;

const void *geFolderMenuPicture(const char *name, s32 *width, s32 *height)
{
	char source[64];
	char key[64];
	u8 *rgba;
	s32 w = 0, h = 0;
	s32 i;

	if (!name || !gebeanGetEnabled() || !xblaMeshGetEnabled()) {
		return NULL;
	}

	for (i = 0; i < numMenuPictures; i++) {
		if (strcmp(menuPictures[i].name, name) == 0) {
			break;
		}
	}

	if (i == numMenuPictures) {
		if (numMenuPictures >= GEFOLDER_MAXMENU) {
			return NULL;
		}

		snprintf(menuPictures[i].name, sizeof(menuPictures[i].name), "%s", name);
		numMenuPictures++;

		snprintf(source, sizeof(source), "texture/%s", name);
		rgba = gebeanDecodePictureFile(source, &w, &h);

		// The release's crosshairs - the menus' cursor and the HUD's HD one
		// (gehud.c) - are blue, which is not how they are seen - the release
		// colours them as it draws them - and GoldenEye's is red, drawn white.
		// Their red is the blue channel.
		if (rgba && (strcmp(name, "sight") == 0 || strcmp(name, "bg/sight") == 0)) {
			for (s32 k = 0; k < w * h; k++) {
				u8 *px = rgba + (size_t)k * 4;
				const u8 r = px[0];

				px[0] = px[2];
				px[2] = r;
			}
		}

		// The HUD's is lit from above - a bevel, pale along the ring's top
		// edge - and its rows are turned over here so that it stands the right
		// way up when drawn as GoldenEye's own crosshair is, top row first. A
		// rectangle stepping up the picture instead sits a texel higher.
		if (rgba && strcmp(name, "bg/sight") == 0) {
			const size_t pitch = (size_t)w * 4;
			u8 *row = malloc(pitch);

			for (s32 y = 0; row && y < h / 2; y++) {
				memcpy(row, rgba + y * pitch, pitch);
				memcpy(rgba + y * pitch, rgba + (h - 1 - y) * pitch, pitch);
				memcpy(rgba + (h - 1 - y) * pitch, row, pitch);
			}

			free(row);
		}

		if (rgba) {
			snprintf(key, sizeof(key), "gemenu/%s", name);
			menuPictures[i].tile = xblaTexBindImage(key, rgba, w, h);
			menuPictures[i].width = w;
			menuPictures[i].height = h;
		} else {
			sysLogPrintf(LOG_WARNING, "gefolder: the release has no menu picture %s", source);
		}
	}

	if (menuPictures[i].tile) {
		*width = menuPictures[i].width;
		*height = menuPictures[i].height;
	}

	return menuPictures[i].tile;
}

/**
 * The menus' two fonts (gebeanFontOpen()), cut up a glyph a picture: the
 * release's glyphs are packed in one picture, and a rectangle over a share of
 * a stand-in counts its texels in 32nds of the nominal size, half a pixel of
 * the release's picture, which blurs or clips an edge; and a glyph of its own,
 * clamped, cannot filter in its neighbour's. The boxes' UVs are the texel
 * centres (Direct3D 9's), so a box starts at the texel it names. Built once,
 * kept for the life of the game; a font the release has not got is tried
 * once.
 */
#define GEFOLDER_GLYPH_BORDER 1

static struct gefolderfont *menuFonts[2];
static s32 menuFontTried[2];

static struct gefolderfont *geFolderBuildFont(const char *name)
{
	struct gebeanfont *src = gebeanFontOpen(name);
	struct gefolderfont *font;
	s32 cut = 0;

	if (!src) {
		return NULL;
	}

	font = calloc(1, sizeof(*font));

	if (!font) {
		gebeanFontClose(src);
		return NULL;
	}

	font->lineheight = src->lineheight;
	font->ascent = src->ascent;
	font->space = src->space;

	for (s32 i = 0; i < src->numglyphs; i++) {
		const struct gebeanglyph *g = &src->glyphs[i];
		struct gefolderglyph *out;
		const s32 b = GEFOLDER_GLYPH_BORDER;
		const s32 x0 = (s32)g->u0;
		const s32 y0 = (s32)g->v0;
		const s32 pw = g->width + 2 * b;
		const s32 ph = g->height + 2 * b;
		char key[48];
		u8 *rgba;

		if (g->ch < 0x21 || g->ch >= 0x7f) {
			continue;
		}

		out = &font->glyphs[g->ch - 0x21];
		out->advance = g->advance;
		out->left = g->left - b;
		out->top = g->top + b;
		out->width = pw;
		out->height = ph;

		if (g->ch == 'H') {
			font->capheight = g->top;
		}

		if (g->width == 0 || g->height == 0 || x0 < 0 || y0 < 0
				|| x0 + g->width > src->width || y0 + g->height > src->height) {
			continue;
		}

		rgba = calloc((size_t)pw * ph, 4);

		if (!rgba) {
			continue;
		}

		// both pictures bottom-up: the box's row r from the top is the
		// release picture's row height-1-(y0+r) and this one's ph-1-(b+r)
		for (s32 r = 0; r < g->height; r++) {
			memcpy(rgba + ((size_t)(ph - 1 - (b + r)) * pw + b) * 4,
					src->rgba + ((size_t)(src->height - 1 - (y0 + r)) * src->width + x0) * 4,
					(size_t)g->width * 4);
		}

		snprintf(key, sizeof(key), "gefont/%s/%02x", name, g->ch);
		out->tile = xblaTexBindImage(key, rgba, pw, ph);

		if (out->tile) {
			cut++;
		}
	}

	gebeanFontClose(src);

	if (!cut || !font->capheight) {
		free(font);
		return NULL;
	}

	sysLogPrintf(LOG_NOTE, "gefolder: the release's font %s, %d glyphs", name, cut);

	return font;
}

const struct gefolderfont *geFolderFont(s32 gothic)
{
	gothic = gothic ? 1 : 0;

	if (!gebeanGetEnabled() || !xblaMeshGetEnabled()) {
		return NULL;
	}

	if (!menuFonts[gothic] && !menuFontTried[gothic]) {
		menuFontTried[gothic] = 1;
		menuFonts[gothic] = geFolderBuildFont(gothic ? "doc0" : "alps3");
	}

	return menuFonts[gothic];
}

/**
 * What the release draws behind the folder: not GoldenEye's frame of cover
 * cloth but a dark desk out of focus - olive going to near black at the
 * edges, one soft light above and left of the folder and a cool shadow under
 * it. The release does not carry it as a picture (it is its lighting), so it
 * is drawn here, measured off a capture of the release's menu, once, as a
 * stand-in kept for the life of the game.
 */
#define GEFOLDER_BACKDROP_W 160
#define GEFOLDER_BACKDROP_H 90

static f32 geFolderGlow(f32 u, f32 v, f32 cu, f32 cv, f32 su, f32 sv)
{
	const f32 du = (u - cu) / su;
	const f32 dv = (v - cv) / sv;

	return expf(-(du * du + dv * dv) * 0.5f);
}

const void *geFolderBackdrop(void)
{
	static const void *tile;
	static s32 tried;
	u8 *rgba;

	if (!gebeanGetEnabled() || !xblaMeshGetEnabled()) {
		return NULL;
	}

	if (tried) {
		return tile;
	}

	tried = 1;
	rgba = malloc(GEFOLDER_BACKDROP_W * GEFOLDER_BACKDROP_H * 4);

	if (!rgba) {
		return NULL;
	}

	for (s32 y = 0; y < GEFOLDER_BACKDROP_H; y++) {
		for (s32 x = 0; x < GEFOLDER_BACKDROP_W; x++) {
			static const f32 edge[3] = { 27, 27, 20 };
			static const f32 middle[3] = { 52, 48, 40 };
			static const f32 light[3] = { 26, 26, 25 };
			static const f32 shadow[3] = { 10, 8, 4 };
			const f32 u = (x + 0.5f) / GEFOLDER_BACKDROP_W;
			// the picture is uploaded in the game's row order, bottom row first
			const f32 v = 1.0f - (y + 0.5f) / GEFOLDER_BACKDROP_H;
			const f32 du = (u - 0.5f) / 0.62f;
			const f32 dv = (v - 0.48f) / 0.75f;
			f32 vig = 1.0f - (du * du + dv * dv);
			const f32 glow = geFolderGlow(u, v, 0.38f, 0.12f, 0.13f, 0.2f);
			const f32 dark = geFolderGlow(u, v, 0.25f, 0.45f, 0.08f, 0.13f);
			u8 *px = rgba + ((size_t)y * GEFOLDER_BACKDROP_W + x) * 4;

			vig = vig < 0 ? 0 : powf(vig, 0.8f);

			for (s32 k = 0; k < 3; k++) {
				const f32 c = edge[k] + (middle[k] - edge[k]) * vig + light[k] * glow - shadow[k] * dark;

				px[k] = (u8)(c < 0 ? 0 : c > 255 ? 255 : c + 0.5f);
			}

			px[3] = 0xff;
		}
	}

	tile = xblaTexBindImage("gemenu/backdrop", rgba, GEFOLDER_BACKDROP_W, GEFOLDER_BACKDROP_H);

	return tile;
}


/* -------------------------------------------------------------------------
 * The release's own folder, drawn 1:1
 * ------------------------------------------------------------------------- */

/**
 * The release does not repaint GoldenEye's folder: it draws a model of its own,
 * walletbond, recorded draw for draw in Xenia (2026-09-22). It is GoldenEye's
 * folder rebuilt - the same nodes, the same switches, each list node's quads
 * where GoldenEye's are but a fifth the size (bean = rom x 0.2 + a shift) -
 * with new UVs and vertex colours: the cover and the page are each one whole
 * picture across their quads, where GoldenEye repeats a tile, and it adds a
 * back cover and spine to the left of the frame, which GoldenEye never had.
 *
 * So under the release's look every list node of GoldenEye's model is drawn
 * with the release's own triangles for it, brought into GoldenEye's units:
 * GoldenEye's switches still say what shows (gexfront.c's frontDrawFolder()),
 * and its camera and pans still say where. Nothing of the release's geometry
 * is kept in the source - it is read out of the player's own file here.
 *
 * Which node a draw belongs to is what the release numbers it with: a 0x30
 * draw names its list node, and a plain one stands in the 0x17 section of
 * the switch above it. Both numberings are the release's own, and are matched
 * to GoldenEye's list nodes (in the order a walk of the model meets them,
 * toggle targets included) by the geometry, once, offline: every one lands
 * on its node's own rectangle to two units in five thousand.
 */
#define BEANFOLDER_ROMNODES 46   // GoldenEye's list nodes, walked in order
#define BEANFOLDER_SCALE    0.2f // the release's units in GoldenEye's
#define BEANFOLDER_BATCH    24   // vertices a G_VTX (see XBLAMESH_BATCH)
#define BEANFOLDER_TEXELS   32   // a stand-in's nominal square

// A draw the release makes with no picture at all: the mission grid's black
// board (its triangles name the clip's picture with every uv at 0, and the
// release binds nothing for them - the draw log's untextured 256)
#define BEANFOLDER_NOTEX    -2

// ROM node 0 is the frame the folder stands in (gexfront.c's backdrop), 1 the
// tabs, 2 the paper, 3 the blank page, 4 and 5 the slides and their film
// strip, 6-10 the stamps, 11-16 the photograph of Bond, its clip and its
// shadow, 17-38 the briefing photographs, 39-45 the cover's photographs.
static const struct { s16 node; s16 rom; } beanFolderByNode[] = {
	{  0,  0 }, {  1, 39 }, {  3,  3 }, {  4,  4 }, {  7,  5 }, {  8, 16 },
	{  9, 12 }, { 10, 13 }, { 11, 14 }, { 12, 15 }, { 13, 45 }, { 14, 41 },
	{ 15, 42 }, { 16, 43 }, { 17, 44 }, { 18, 38 }, { 39,  7 }, { 40,  6 },
	{ 41,  8 }, { 42,  9 }, { 43, 10 },
	// 19-38 are the twenty missions' pairs of briefing photographs, 18-37
};

static const struct { s16 cond; s16 rom; } beanFolderByCond[] = {
	{ -1,  0 }, // the back cover and spine, which GoldenEye has no node for
	{  1, 39 }, {  2,  1 }, {  5,  2 }, {  8, 11 }, { 18, 17 }, { 13, 40 },
};

struct beanfoldertri {
	s16 rom;
	s16 tex;
	struct gebeanmodelvtx v[3];
};

static struct {
	s32 built;       // 1 built, -1 tried and not to be used
	Gfx *gdl[BEANFOLDER_ROMNODES];
	Vtx *vtx[BEANFOLDER_ROMNODES];
	Col *col[BEANFOLDER_ROMNODES];
	s32 numvtx[BEANFOLDER_ROMNODES];
	struct modelnode *nodes[BEANFOLDER_ROMNODES];
	Gfx *saved[BEANFOLDER_ROMNODES];
	s32 swapped;
	struct beanfoldertri *tris;
	s32 numtris;
	s32 maxtris;
} beanFolder;

static s32 beanFolderRomFor(const struct gebeanmodeldraw *d)
{
	if (d->node >= 19 && d->node <= 38) {
		return d->node - 1;
	}

	if (d->node >= 0) {
		for (s32 i = 0; i < ARRAYCOUNT(beanFolderByNode); i++) {
			if (beanFolderByNode[i].node == d->node) {
				return beanFolderByNode[i].rom;
			}
		}

		return -1;
	}

	for (s32 i = 0; i < ARRAYCOUNT(beanFolderByCond); i++) {
		if (beanFolderByCond[i].cond == (d->numconds ? d->conds[d->numconds - 1] : -1)) {
			return beanFolderByCond[i].rom;
		}
	}

	return -1;
}

static void beanFolderTake(const struct gebeanmodeldraw *d, void *arg)
{
	const s32 rom = beanFolderRomFor(d);
	s32 tex = BEANFOLDER_NOTEX;

	for (s32 i = 0; i < d->numvtx; i++) {
		if (d->vtx[i].uv[0] != 0.0f || d->vtx[i].uv[1] != 0.0f) {
			tex = d->tex;
			break;
		}
	}

	if (rom < 0) {
		sysLogPrintf(LOG_WARNING, "gefolder: the release's folder draws node %d (section %d), "
				"which no node of GoldenEye's is - left out", d->node, d->numconds ? d->conds[d->numconds - 1] : -1);
		return;
	}

	for (s32 i = 0; i + 2 < d->numvtx; i += 3) {
		if (beanFolder.numtris == beanFolder.maxtris) {
			const s32 more = beanFolder.maxtris ? beanFolder.maxtris * 2 : 256;
			struct beanfoldertri *grown = realloc(beanFolder.tris, sizeof(*grown) * more);

			if (!grown) {
				return;
			}

			beanFolder.tris = grown;
			beanFolder.maxtris = more;
		}

		beanFolder.tris[beanFolder.numtris].rom = rom;
		beanFolder.tris[beanFolder.numtris].tex = tex;
		memcpy(beanFolder.tris[beanFolder.numtris].v, &d->vtx[i], sizeof(struct gebeanmodelvtx) * 3);
		beanFolder.numtris++;
	}
}

/** GoldenEye's list nodes in walk order, toggle targets included. */
static s32 beanFolderRomNodes(struct modelnode *node, struct modelnode **out, s32 num)
{
	while (node) {
		const u32 type = node->type & 0xff;
		struct modelnode *child = node->child;

		if (type == MODELNODETYPE_DL) {
			if (num < BEANFOLDER_ROMNODES) {
				out[num] = node;
			}

			num++;
		} else if (type == MODELNODETYPE_TOGGLE) {
			child = node->rodata->toggle.target;
		}

		if (child) {
			num = beanFolderRomNodes(child, out, num);
		}

		node = node->next;
	}

	return num;
}

/** A list node's own vertices' extent, in GoldenEye's units. */
static void beanFolderRomExtent(const struct modelnode *node, f32 *lo, f32 *hi)
{
	const struct modelrodata_dl *dl = &node->rodata->dl;

	for (s32 k = 0; k < 3; k++) {
		lo[k] = 1e9f;
		hi[k] = -1e9f;
	}

	for (s32 i = 0; i < dl->numvertices; i++) {
		const f32 p[3] = { dl->vertices[i].x, dl->vertices[i].y, dl->vertices[i].z };

		for (s32 k = 0; k < 3; k++) {
			if (p[k] < lo[k]) lo[k] = p[k];
			if (p[k] > hi[k]) hi[k] = p[k];
		}
	}
}

/** The release's triangles for one of GoldenEye's nodes: their extent in its own units. */
static s32 beanFolderBeanExtent(s32 rom, s32 tex, f32 *lo, f32 *hi)
{
	s32 found = 0;

	for (s32 k = 0; k < 3; k++) {
		lo[k] = 1e9f;
		hi[k] = -1e9f;
	}

	for (s32 i = 0; i < beanFolder.numtris; i++) {
		if (beanFolder.tris[i].rom != rom || (tex >= 0 && beanFolder.tris[i].tex != tex)) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			for (s32 k = 0; k < 3; k++) {
				const f32 p = beanFolder.tris[i].v[j].pos[k];

				if (p < lo[k]) lo[k] = p;
				if (p > hi[k]) hi[k] = p;
			}
		}

		found = 1;
	}

	return found;
}

/** The first matrix a list of GoldenEye's loads, for its replacement to load too. */
static s32 beanFolderListMatrix(const struct modelnode *node, Gfx *out)
{
	const uintptr_t addr = (uintptr_t)node->rodata->dl.opagdl;
	const Gfx *list = node->rodata->dl.opagdl;

	// the list is behind the node's colours, named by its offset in segment 5
	// with the low bit set (gfx_pc.cpp's seg_addr())
	if ((addr & 1) && ((addr >> 24) & 0xf) == SPSEGMENT_MODEL_COL1) {
		list = (const Gfx *)((const u8 *)node->rodata->dl.colours + (addr & 0x00fffffe));
	}

	for (s32 i = 0; list && i < 256; i++) {
		const u8 op = (u8)(list[i].words.w0 >> 24);

		if (op == (u8)G_MTX) {
			*out = list[i];
			return 1;
		}

		if (op == (u8)G_ENDDL) {
			break;
		}
	}

	return 0;
}

/**
 * How the release draws each of its pictures, read off its three pixel
 * shaders in the draw log. Most are the picture times the vertex colour. The
 * cursor's shadow and the OHMSS title carry no alpha: their brightness is how
 * much of the vertex colour covers the page (black for the title). The
 * paperclip is the same but keeps its own grey: over the white of a
 * photograph the release's clip is a grey loop (234 goes to about 175, the
 * dark desk's 4 to about 34), where white coverage left it white on white
 * and it vanished into the picture it holds. The stamps are grey ink with an
 * alpha of its own, in the vertex colour - a dark red, itself two thirds
 * opaque.
 */
#define BEANFOLDER_CLIP    66
#define BEANFOLDER_PICTURE 0
#define BEANFOLDER_MASK    1
#define BEANFOLDER_STAMP   2
#define BEANFOLDER_PLAIN   3

static s32 beanFolderKind(s32 tex)
{
	if (tex == BEANFOLDER_NOTEX) {
		return BEANFOLDER_PLAIN;
	}

	if (tex >= 64 && tex <= 66) {
		return BEANFOLDER_MASK;
	}

	if (tex >= 68 && tex <= 70) {
		return BEANFOLDER_STAMP;
	}

	return BEANFOLDER_PICTURE;
}

// A picture's stand-in by the model's index, bound once for the game's life:
// a mask as white with its brightness for alpha
static const void *beanFolderTile(struct gebeanpictures *pics, s32 tex)
{
	char key[48];
	s32 w = 0, h = 0;
	u8 *rgba;

	snprintf(key, sizeof(key), "gefolder/bean/%d", tex);
	rgba = gebeanPicturesDecode(pics, tex, &w, &h);

	if (rgba && beanFolderKind(tex) == BEANFOLDER_MASK) {
		for (s32 i = 0; i < w * h; i++) {
			u8 *px = rgba + (size_t)i * 4;

			px[3] = (u8)((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8);

			if (tex != BEANFOLDER_CLIP) {
				px[0] = px[1] = px[2] = 0xff;
			}
		}
	}

	return rgba ? xblaTexBindImage(key, rgba, w, h) : NULL;
}

/** The state a picture is drawn in, after texSelect(), which sets modes of its own. */
static Gfx *beanFolderPictureState(Gfx *g, s32 tex)
{
	gDPPipeSync(g++);
	gDPSetCycleType(g++, G_CYC_1CYCLE);
	gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetTexturePersp(g++, G_TP_PERSP);
	gDPSetTextureFilter(g++, G_TF_BILERP);
	gDPSetTextureLUT(g++, G_TT_NONE);
	gDPSetAlphaCompare(g++, G_AC_NONE);

	if (beanFolderKind(tex) == BEANFOLDER_PLAIN) {
		gDPSetCombineLERP(g++, 0, 0, 0, SHADE, 0, 0, 0, SHADE, 0, 0, 0, SHADE, 0, 0, 0, SHADE);
	} else if (beanFolderKind(tex) == BEANFOLDER_STAMP) {
		gDPSetCombineLERP(g++, 0, 0, 0, SHADE, TEXEL0, 0, SHADE, 0, 0, 0, 0, SHADE, TEXEL0, 0, SHADE, 0);
	} else {
		gDPSetCombineLERP(g++, TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0);
	}

	return g;
}

// Whether the release repeats the picture or holds its edge (the draw log's
// fetch constants): the cover, the paper and the cursor's shadow repeat, the
// photographs, the clip, the stamps and the film strip's holes do not
static s32 beanFolderWraps(s32 tex)
{
	return tex <= 2 || tex == 64;
}

static void beanFolderFree(void)
{
	for (s32 i = 0; i < BEANFOLDER_ROMNODES; i++) {
		free(beanFolder.gdl[i]);
		free(beanFolder.vtx[i]);
		free(beanFolder.col[i]);
		beanFolder.gdl[i] = NULL;
		beanFolder.vtx[i] = NULL;
		beanFolder.col[i] = NULL;
		beanFolder.numvtx[i] = 0;
		beanFolder.nodes[i] = NULL;
	}

	free(beanFolder.tris);
	beanFolder.tris = NULL;
	beanFolder.numtris = 0;
	beanFolder.maxtris = 0;
	beanFolder.built = 0;
}

/**
 * One node's list: GoldenEye's own matrix, the release's pictures, colours and
 * triangles, and the state the release draws them in - blended, both faces,
 * texel times vertex colour, unlit.
 */
static Gfx *beanFolderBuildList(struct gebeanpictures *pics, s32 rom, const f32 *shift, Vtx **outvtx, Col **outcol)
{
	s32 numtris = 0, numdraws = 0;
	s32 lasttex = -1;
	Gfx *gdl, *g;
	Vtx *vtx;
	Col *col;
	s32 nv = 0;

	for (s32 i = 0; i < beanFolder.numtris; i++) {
		if (beanFolder.tris[i].rom == rom) {
			numtris++;

			if (beanFolder.tris[i].tex != lasttex) {
				numdraws++;
				lasttex = beanFolder.tris[i].tex;
			}
		}
	}

	*outvtx = NULL;
	*outcol = NULL;

	if (numtris == 0) {
		return NULL;
	}

	gdl = malloc(sizeof(Gfx) * (32 + numdraws * 32 + numtris * 4));
	vtx = malloc(sizeof(Vtx) * numtris * 3);
	col = malloc(sizeof(Col) * numtris * 3);

	if (!gdl || !vtx || !col) {
		free(gdl);
		free(vtx);
		free(col);
		return NULL;
	}

	g = gdl;
	gDPPipeSync(g++);

	if (!beanFolderListMatrix(beanFolder.nodes[rom], g)) {
		free(gdl);
		free(vtx);
		free(col);
		return NULL;
	}

	g++;
	gSPClearGeometryMode(g++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_CULL_BOTH | G_FOG);
	gSPSetGeometryMode(g++, G_SHADE | G_SHADING_SMOOTH);
	gSPTexture(g++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);

	lasttex = -1;

	for (s32 i = 0; i < beanFolder.numtris; ) {
		const struct beanfoldertri *t = &beanFolder.tris[i];
		s32 first = nv;
		s32 n = 0;

		if (t->rom != rom) {
			i++;
			continue;
		}

		if (t->tex != lasttex) {
			struct textureconfig tc;

			memset(&tc, 0, sizeof(tc));
			tc.textureptr = t->tex >= 0 ? (u8 *)beanFolderTile(pics, t->tex) : NULL;
			tc.width = BEANFOLDER_TEXELS;
			tc.height = BEANFOLDER_TEXELS;
			tc.format = G_IM_FMT_RGBA;
			tc.depth = G_IM_SIZ_32b;
			tc.s = beanFolderWraps(t->tex) ? G_TX_WRAP : G_TX_CLAMP;
			tc.t = tc.s;

			if (tc.textureptr) {
				texSelect(&g, &tc, 1, 0, 2, 1, NULL);
			}

			g = beanFolderPictureState(g, t->tex);
			lasttex = t->tex;
		}

		// a batch: up to BEANFOLDER_BATCH vertices of this node's triangles in
		// this picture, in the release's own order
		while (i < beanFolder.numtris && n + 3 <= BEANFOLDER_BATCH) {
			const struct beanfoldertri *u = &beanFolder.tris[i];

			if (u->rom != rom) {
				i++;
				continue;
			}

			if (u->tex != lasttex) {
				break;
			}

			for (s32 j = 0; j < 3; j++) {
				const struct gebeanmodelvtx *bv = &u->v[j];
				Vtx *v = &vtx[nv];
				Col *c = &col[nv];

				v->x = (s16)floorf((bv->pos[0] - shift[0]) / BEANFOLDER_SCALE + 0.5f);
				v->y = (s16)floorf((bv->pos[1] - shift[1]) / BEANFOLDER_SCALE + 0.5f);
				v->z = (s16)floorf((bv->pos[2] - shift[2]) / BEANFOLDER_SCALE + 0.5f);
				v->flags = 0;
				v->colour = (u8)(n * 4);

				// s10.5 of the stand-in's nominal square; the release's v runs
				// down its picture and the picture is held in the game's row
				// order, bottom up
				v->s = (s16)floorf(bv->uv[0] * BEANFOLDER_TEXELS * 32.0f + 0.5f);
				v->t = (s16)floorf((1.0f - bv->uv[1]) * BEANFOLDER_TEXELS * 32.0f + 0.5f);

				c->r = (bv->argb >> 16) & 0xff;
				c->g = (bv->argb >> 8) & 0xff;
				c->b = bv->argb & 0xff;
				c->a = (bv->argb >> 24) & 0xff;

				nv++;
				n++;
			}

			i++;
		}

		if (n > 0) {
			gSPColor(g++, &col[first], n);
			gSPVertex(g++, &vtx[first], n, 0);

			for (s32 k = 0; k < n; k += 3) {
				gSP1Triangle(g++, k, k + 1, k + 2, 0);
			}
		}
	}

	gDPPipeSync(g++);
	gSPEndDisplayList(g++);

	*outvtx = vtx;
	*outcol = col;
	beanFolder.numvtx[rom] = nv;

	return gdl;
}

/**
 * Reads the release's folder and builds a list for every node of GoldenEye's
 * that it has triangles for. Checked against GoldenEye's model first: the
 * paper and the tabs have to land on GoldenEye's own, or this is not the
 * folder the tables were written for and the repainted one is drawn instead.
 */
static s32 geFolderBeanBuild(struct gebeanpictures *pics, struct modeldef *modeldef)
{
	f32 romlo[3], romhi[3], beanlo[3], beanhi[3];
	f32 shift[3];
	s32 numnodes;
	s32 lists = 0;

	beanFolderFree();
	beanFolder.built = -1;

	numnodes = beanFolderRomNodes(modeldef->rootnode, beanFolder.nodes, 0);

	if (numnodes != BEANFOLDER_ROMNODES) {
		sysLogPrintf(LOG_WARNING, "gefolder: GoldenEye's folder has %d list nodes, not %d - "
				"the release's own folder is not drawn", numnodes, BEANFOLDER_ROMNODES);
		return 0;
	}

	if (gebeanPicturesWalk(pics, beanFolderTake, NULL) == 0) {
		return 0;
	}

	// The shift, off the paper with the crest (node 2, picture 2): the middle
	// of the release's rectangle less a fifth of the middle of GoldenEye's
	beanFolderRomExtent(beanFolder.nodes[2], romlo, romhi);

	if (!beanFolderBeanExtent(2, 2, beanlo, beanhi)) {
		sysLogPrintf(LOG_WARNING, "gefolder: the release's folder has no paper - not drawn");
		return 0;
	}

	for (s32 k = 0; k < 3; k++) {
		shift[k] = (beanlo[k] + beanhi[k]) * 0.5f - (romlo[k] + romhi[k]) * 0.5f * BEANFOLDER_SCALE;
	}

	// and the paper's size and the tabs' place, to two of GoldenEye's units
	// a fifth of the way
	beanFolderRomExtent(beanFolder.nodes[1], romlo, romhi);

	if (!beanFolderBeanExtent(1, -1, beanlo, beanhi)
			|| fabsf((beanlo[0] - shift[0]) / BEANFOLDER_SCALE - romlo[0]) > 25.0f
			|| fabsf((beanhi[1] - shift[1]) / BEANFOLDER_SCALE - romhi[1]) > 25.0f) {
		sysLogPrintf(LOG_WARNING, "gefolder: the release's folder does not stand on GoldenEye's - not drawn");
		return 0;
	}

	for (s32 rom = 0; rom < BEANFOLDER_ROMNODES; rom++) {
		beanFolder.gdl[rom] = beanFolderBuildList(pics, rom, shift, &beanFolder.vtx[rom], &beanFolder.col[rom]);

		if (beanFolder.gdl[rom]) {
			lists++;
		}
	}

	sysLogPrintf(LOG_NOTE, "gefolder: the release's own folder, %d triangles on %d of GoldenEye's %d nodes "
			"(shift %.1f %.1f %.1f)", beanFolder.numtris, lists, BEANFOLDER_ROMNODES, shift[0], shift[1], shift[2]);

	free(beanFolder.tris);
	beanFolder.tris = NULL;
	beanFolder.numtris = 0;
	beanFolder.maxtris = 0;
	beanFolder.built = lists > 0 ? 1 : -1;

	return beanFolder.built > 0;
}

s32 geFolderBeanActive(void)
{
	return beanFolder.built > 0 && gebeanGetEnabled() && xblaMeshGetEnabled();
}

void geFolderBeanSwap(struct model *model, s32 on)
{
	if (on && !beanFolder.swapped && geFolderBeanActive()) {
		for (s32 i = 0; i < BEANFOLDER_ROMNODES; i++) {
			union modelrwdata *rw = beanFolder.nodes[i] ? modelGetNodeRwData(model, beanFolder.nodes[i]) : NULL;

			beanFolder.saved[i] = rw ? rw->dl.gdl : NULL;

			// a node the release has nothing for is not drawn: its picture
			// is GoldenEye's and would stand out
			if (rw && rw->dl.gdl) {
				rw->dl.gdl = beanFolder.gdl[i] ? beanFolder.gdl[i] : NULL;
			}
		}

		beanFolder.swapped = 1;
	} else if (!on && beanFolder.swapped) {
		for (s32 i = 0; i < BEANFOLDER_ROMNODES; i++) {
			union modelrwdata *rw = beanFolder.nodes[i] ? modelGetNodeRwData(model, beanFolder.nodes[i]) : NULL;

			if (rw && beanFolder.saved[i]) {
				rw->dl.gdl = beanFolder.saved[i];
			}
		}

		beanFolder.swapped = 0;
	}
}

void geFolderBeanColourSlides(struct modelnode *node, const Col *colours, s32 numslides)
{
	const struct modelrodata_dl *dl;
	s32 rom = -1;

	if (!node || beanFolder.built <= 0) {
		return;
	}

	for (s32 i = 0; i < BEANFOLDER_ROMNODES; i++) {
		if (beanFolder.nodes[i] == node) {
			rom = i;
			break;
		}
	}

	if (rom < 0 || !beanFolder.col[rom]) {
		return;
	}

	// the release's slides are its own quads in its own order, so each vertex
	// takes the colour of the ROM slide whose middle it is nearest
	dl = &node->rodata->dl;

	if (dl->numvertices < numslides * 4) {
		return;
	}

	for (s32 i = 0; i < beanFolder.numvtx[rom]; i++) {
		const Vtx *v = &beanFolder.vtx[rom][i];
		f32 best = 1e30f;
		s32 slide = 0;

		for (s32 m = 0; m < numslides; m++) {
			f32 x = 0, y = 0, d;

			for (s32 j = 0; j < 4; j++) {
				x += dl->vertices[m * 4 + j].x * 0.25f;
				y += dl->vertices[m * 4 + j].y * 0.25f;
			}

			d = (v->x - x) * (v->x - x) + (v->y - y) * (v->y - y);

			if (d < best) {
				best = d;
				slide = m;
			}
		}

		beanFolder.col[rom][i] = colours[slide];
	}
}

void geFolderForget(void)
{
	if (!beanFolder.swapped) {
		beanFolderFree();
	}

	for (s32 i = 0; i < numBound; i++) {
		xblaTexForgetPicture(bound[i]);
		videoFreeCachedTexture(bound[i]);
		bound[i] = NULL;
	}

	numBound = 0;
}

void geFolderSwitched(struct modeldef *modeldef)
{
	if (modeldef) {
		geFolderRepaint(modeldef);
	} else {
		geFolderForget();
	}
}

#endif

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

		// The release's crosshair is blue, which is not how it is seen - the
		// release colours it as it draws it - and GoldenEye's is red, drawn
		// white. Its red is the blue channel.
		if (rgba && strcmp(name, "sight") == 0) {
			for (s32 k = 0; k < w * h; k++) {
				u8 *px = rgba + (size_t)k * 4;
				const u8 r = px[0];

				px[0] = px[2];
				px[2] = r;
			}
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

void geFolderForget(void)
{
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

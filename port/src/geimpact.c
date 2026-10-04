/**
 * GoldenEye's own bullet holes on a converted GoldenEye level.
 *
 * Perfect Dark's wallhits are GoldenEye's bullet impacts renumbered - its
 * g_WallhitTexes and g_TcWallhitConfigs are explosion.c's g_ImpactTypes and
 * oddtextures.c's s_impactimages taken over and redrawn - so a converted
 * level left Perfect Dark's art in GoldenEye's walls (F3 20260927-003252).
 * GoldenEye picks a hole the same way Perfect Dark does, from a short list per
 * surface (tex.c's g_HitTypeSounds, whose index is the low nibble of the
 * image's g_Textures byte, which getexsurface.c already gives the level's
 * textures): chrprop.c's bg hit and chr.c's prop hit take thing2[random %
 * count]. Water's list is empty: no hole.
 *
 * Its images are GoldenEye ROM images the conversion writes to the level's
 * textures/ like any other (geconvert.c's impact images), served under their
 * own numbers; a conversion from before they were written has none of them,
 * and those levels keep Perfect Dark's holes. In the release's look (F6) each
 * image is the release's own texture/bulletholes/ picture - the release keeps
 * all thirteen under the names the decomp gives them - bound to the loaded
 * texture's address the way gefolder.c binds the folder's.
 *
 * The holes are wallhit texture numbers past Perfect Dark's eighteen
 * (WALLHITTEX_GE_FIRST): everything else about them is Perfect Dark's
 * wallhit.c.
 */
#include <ultra64.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "modloader.h"
#include "system.h"
#include "video.h"
#include "game/texdecompress.h"
#include "lib/rng.h"
#include "gebean.h"
#include "geconvert.h"
#include "geimpact.h"
#include "xblamesh.h"
#include "xblatex.h"

#ifndef PLATFORM_N64

// wallhit.c's colourings: GoldenEye's apptype 0 (dark), 1 (pale), 2 (paint)
#define IMPACT_SOFT   0
#define IMPACT_BULLET 1
#define IMPACT_PAINT  4

struct geimpacttype {
	u16 image;         // GoldenEye's image number (assets/images.def order)
	u8 width;          // the image, s_impactimages
	u8 height;
	u8 level;
	u8 format;
	u8 depth;
	f32 size;          // the hole, g_ImpactTypes (width = height throughout)
	u8 colouring;
	const char *bean;  // the release's texture/bulletholes/<name>
};

// GoldenEye's image numbers for the impact images
#define GE_IMAGE_IMPACTLOTS         206
#define GE_IMAGE_IMPACTREDBRICK1    1475
#define GE_IMAGE_IMPACTREDBRICK3    1476
#define GE_IMAGE_IMPACTBROWNBRICK1  1478
#define GE_IMAGE_IMPACTBROWNBRICK2  1479
#define GE_IMAGE_IMPACT1            2168
#define GE_IMAGE_IMPACT2            2169
#define GE_IMAGE_IMPACT3            2170
#define GE_IMAGE_IMPACT4            2171
#define GE_IMAGE_IMPACTMULTI        2172
#define GE_IMAGE_IMPACTREDBRICK2    2173
#define GE_IMAGE_IMPACTBRICK2       2174
#define GE_IMAGE_IMPACTBRICK3       2175

static const struct geimpacttype g_GeImpactTypes[GEIMPACT_NUMTYPES] = {
	/* 0*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,  10, IMPACT_BULLET, "windowhit" },
	/* 1*/ { GE_IMAGE_IMPACT2,           0x40, 0x20, 7, G_IM_FMT_IA,   G_IM_SIZ_8b,   6, IMPACT_BULLET, "bullethit" },
	/* 2*/ { GE_IMAGE_IMPACT3,           0x40, 0x40, 0, G_IM_FMT_IA,   G_IM_SIZ_8b,   8, IMPACT_SOFT,   "wallhit" },
	/* 3*/ { GE_IMAGE_IMPACTLOTS,        0x40, 0x20, 7, G_IM_FMT_IA,   G_IM_SIZ_8b,  20, IMPACT_BULLET, "bulletholesplaster" },
	/* 4*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,   6, IMPACT_BULLET, "windowhit" },
	/* 5*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,   8, IMPACT_BULLET, "windowhit" },
	/* 6*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,  12, IMPACT_BULLET, "windowhit" },
	/* 7*/ { GE_IMAGE_IMPACT4,           0x20, 0x20, 0, G_IM_FMT_IA,   G_IM_SIZ_8b,   6, IMPACT_BULLET, "bullethole" },
	/* 8*/ { GE_IMAGE_IMPACTMULTI,       0x20, 0x20, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 20, IMPACT_BULLET, "bulletholesplasterrgb" },
	/* 9*/ { GE_IMAGE_IMPACTREDBRICK1,   0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b, 20, IMPACT_BULLET, "wallhole1" },
	/*10*/ { GE_IMAGE_IMPACTREDBRICK2,   0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b, 20, IMPACT_BULLET, "wallhole2" },
	/*11*/ { GE_IMAGE_IMPACTREDBRICK3,   0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b, 20, IMPACT_BULLET, "wallhole3" },
	/*12*/ { GE_IMAGE_IMPACTBRICK2,      0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b, 20, IMPACT_BULLET, "wallhole6" },
	/*13*/ { GE_IMAGE_IMPACTBRICK3,      0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b, 24, IMPACT_BULLET, "wallhole7" },
	/*14*/ { GE_IMAGE_IMPACTBROWNBRICK1, 0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b,  6, IMPACT_BULLET, "wallhole8" },
	/*15*/ { GE_IMAGE_IMPACTBROWNBRICK2, 0x20, 0x20, 6, G_IM_FMT_RGBA, G_IM_SIZ_16b,  6, IMPACT_BULLET, "wallhole9" },
	/*16*/ { GE_IMAGE_IMPACT3,           0x40, 0x40, 0, G_IM_FMT_IA,   G_IM_SIZ_8b,  24, IMPACT_PAINT,  "wallhit" },
	/*17*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,   6, IMPACT_BULLET, "windowhit" },
	/*18*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,   8, IMPACT_BULLET, "windowhit" },
	/*19*/ { GE_IMAGE_IMPACT1,           0x30, 0x30, 6, G_IM_FMT_IA,   G_IM_SIZ_8b,  12, IMPACT_BULLET, "windowhit" },
};

// tex.c's thing2 lists by surface (HIT_DEFAULT..HIT_GLASS_XLU = SURFACETYPE_*)
static const u8 g_GeSurfaceImpacts[][4] = {
	/* default   */ { 1, 0x07 },
	/* stone     */ { 1, 0x01 },
	/* wood      */ { 1, 0x01 },
	/* metal     */ { 1, 0x07 },
	/* glass     */ { 3, 0x04, 0x05, 0x06 },
	/* water     */ { 0 },
	/* snow      */ { 1, 0x01 },
	/* dirt      */ { 1, 0x02 },
	/* mud       */ { 1, 0x02 },
	/* tile      */ { 1, 0x01 },
	/* metal obj */ { 2, 0x01, 0x07 },
	/* chr       */ { 1, 0x02 },
	/* glass xlu */ { 3, 0x11, 0x12, 0x13 },
};

static struct textureconfig g_GeImpactConfigs[GEIMPACT_NUMTYPES];
static s32 g_GeImpactOn;

// the release's picture bound at each loaded texture's address, and the look
// it was bound for
static const void *g_GeImpactBound[GEIMPACT_NUMTYPES];
static s32 g_GeImpactBoundHd[GEIMPACT_NUMTYPES];

void geImpactStageStart(s32 stagenum)
{
	const char *dir;
	char path[FS_MAXPATH + 1];

	// the texture pool the last stage's addresses were in is gone
	for (s32 i = 0; i < GEIMPACT_NUMTYPES; i++) {
		if (g_GeImpactBound[i]) {
			xblaTexForgetPicture(g_GeImpactBound[i]);
			g_GeImpactBound[i] = NULL;
		}

		g_GeImpactBoundHd[i] = 0;
	}

	g_GeImpactOn = false;

	for (s32 i = 0; i < GEIMPACT_NUMTYPES; i++) {
		const struct geimpacttype *t = &g_GeImpactTypes[i];
		struct textureconfig *tc = &g_GeImpactConfigs[i];

		memset(tc, 0, sizeof(*tc));
		tc->texturenum = geconvertTexRemap(t->image);
		tc->width = t->width;
		tc->height = t->height;
		tc->level = t->level;
		tc->format = t->format;
		tc->depth = t->depth;
		tc->s = G_TX_CLAMP;
		tc->t = G_TX_CLAMP;
	}

	if (!modloaderStageIsRemake(stagenum) || (dir = modloaderGetStageModDir(stagenum)) == NULL) {
		return;
	}

	// All of them or none: a number the level does not serve is Perfect
	// Dark's own texture of that number, and a conversion from before they
	// were written has none.
	for (s32 i = 0; i < GEIMPACT_NUMTYPES; i++) {
		snprintf(path, sizeof(path), "%s/textures/%04x.bin", dir, geconvertTexRemap(g_GeImpactTypes[i].image));

		if (fsFileSize(path) <= 0) {
			sysLogPrintf(LOG_NOTE, "geimpact: stage 0x%02x: the conversion has no image %d, Perfect Dark's bullet holes",
					stagenum, g_GeImpactTypes[i].image);
			return;
		}
	}

	g_GeImpactOn = true;
}

s32 geImpactActive(void)
{
	return g_GeImpactOn;
}

s32 geImpactIsGe(s32 texnum)
{
	return texnum >= WALLHITTEX_GE_FIRST && texnum < WALLHITTEX_GE_FIRST + GEIMPACT_NUMTYPES;
}

s16 geImpactTexnum(s32 surfacetype, s16 pdtexnum)
{
	const u8 *list;

	if (!g_GeImpactOn) {
		return pdtexnum;
	}

	if (surfacetype < 0 || surfacetype >= (s32)ARRAYCOUNT(g_GeSurfaceImpacts)) {
		surfacetype = SURFACETYPE_DEFAULT;
	}

	list = g_GeSurfaceImpacts[surfacetype];

	if (list[0] == 0) {
		return -1;
	}

	return WALLHITTEX_GE_FIRST + list[1 + rngRandom() % list[0]];
}

void geImpactSize(s32 texnum, f32 *width, f32 *height, u8 *type)
{
	const struct geimpacttype *t = &g_GeImpactTypes[texnum - WALLHITTEX_GE_FIRST];

	*width = t->size;
	*height = t->size;
	*type = t->colouring;
}

static s32 geImpactReleaseLook(void)
{
	return gebeanGetEnabled() && xblaMeshGetEnabled();
}

struct textureconfig *geImpactConfig(s32 texnum)
{
	const s32 i = texnum - WALLHITTEX_GE_FIRST;
	struct textureconfig *tc = &g_GeImpactConfigs[i];
	const s32 hd = geImpactReleaseLook();
	s32 owner = i;

	if ((u32)tc->texturenum < NUM_TEXTURES) {
		texLoadFromConfigs(tc, 1, NULL, 0);
	}

	if (tc->unk0b != 1) {
		return tc;
	}

	// The release's picture for the image in its look, the ROM's otherwise;
	// F6 flips the look under a hole already on the wall. Several types share
	// one image, and the first of them keeps its binding.
	for (s32 k = 0; k < i; k++) {
		if (g_GeImpactTypes[k].image == g_GeImpactTypes[i].image) {
			owner = k;
			break;
		}
	}

	if (hd != g_GeImpactBoundHd[owner]) {
		g_GeImpactBoundHd[owner] = hd;

		if (g_GeImpactBound[owner]) {
			xblaTexForgetPicture(g_GeImpactBound[owner]);
			g_GeImpactBound[owner] = NULL;
		}

		if (hd) {
			char source[64];
			s32 w = 0, h = 0;
			u8 *rgba;

			snprintf(source, sizeof(source), "texture/bulletholes/%s", g_GeImpactTypes[i].bean);
			rgba = gebeanDecodePictureFile(source, &w, &h);

			// taken over by the registry, or freed there
			if (rgba && xblaTexBindPictureAt(tc->textureptr, rgba, w, h)) {
				g_GeImpactBound[owner] = tc->textureptr;
			}
		}

		// the renderer keeps a texture by the address it was uploaded from:
		// its copy alone, the registry still naming the picture the
		// conversion's (gemonitor.c's geMonitorBindPicture())
		videoEvictCachedTexture(tc->textureptr);
	}

	return tc;
}

#endif

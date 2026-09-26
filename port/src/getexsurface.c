/**
 * What a shot does to a converted GoldenEye level's surfaces: GoldenEye's own.
 *
 * Perfect Dark asks g_Textures[num] what a bullet sounds like on a texture and
 * what hole it leaves (soundsurfacetype, surfacetype; bgunPlayPropHitSound(),
 * bgunPlayBgHitSound(), objHit()'s wallhit, splat.c). A converted level draws
 * GoldenEye's images under numbers of its own (the image's own number, or the
 * one texRemap() moved it to), and those entries are Perfect Dark's for
 * whatever image of its own sits at that number: Dam's brown doors are
 * GoldenEye's metal (HIT_METAL both ways), and Perfect Dark's 0x2af says
 * default sound and mud's soft smudge for a hole, its 0x2ae glass. So the door
 * left no hole anyone could see, and sounded of glass where 0x2ae is.
 *
 * GoldenEye keeps the same two nibbles in the first byte of its own image
 * entry, with the same numbers (HIT_DEFAULT..HIT_GLASS_XLU are
 * SURFACETYPE_DEFAULT..SURFACETYPE_GLASSXLU), so a stage load puts GoldenEye's
 * byte in every entry the level's textures/ serves and gives Perfect Dark's
 * back on the next stage that is not one. geimagesurfaces.h is the decomp's
 * images.def, which is the ROM's table byte for byte.
 */
#include <ultra64.h>
#include <stdio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "modloader.h"
#include "system.h"
#include "geconvert.h"
#include "getexsurface.h"
#include "geimagesurfaces.h"

#ifndef PLATFORM_N64

// Perfect Dark's own two nibbles for every number, kept the first time a
// converted level changes any
static u8 g_PdSurfaces[NUM_TEXTURES];
static bool g_PdSurfacesKept;
static bool g_GeSurfacesIn;

static void texSurfaceSet(s32 num, u8 byte)
{
	g_Textures[num].soundsurfacetype = byte >> 4;
	g_Textures[num].surfacetype = byte & 0xf;
}

void geTexSurfaceReset(s32 stagenum)
{
	const char *dir;
	char path[FS_MAXPATH + 1];
	s32 changed = 0;

	if (g_Textures == NULL) {
		return;
	}

	if (g_GeSurfacesIn) {
		for (s32 i = 0; i < NUM_TEXTURES; i++) {
			texSurfaceSet(i, g_PdSurfaces[i]);
		}

		g_GeSurfacesIn = false;
	}

	if (!modloaderStageIsRemake(stagenum) || (dir = modloaderGetStageModDir(stagenum)) == NULL) {
		return;
	}

	if (!g_PdSurfacesKept) {
		for (s32 i = 0; i < NUM_TEXTURES; i++) {
			g_PdSurfaces[i] = (g_Textures[i].soundsurfacetype << 4) | g_Textures[i].surfacetype;
		}

		g_PdSurfacesKept = true;
	}

	for (s32 image = 0; image < GE_NUM_IMAGES; image++) {
		const u32 num = geconvertTexRemap(image);

		if (num >= NUM_TEXTURES) {
			continue;
		}

		// only a number the level itself serves: anything else at that
		// number is Perfect Dark's image and keeps Perfect Dark's surface
		snprintf(path, sizeof(path), "%s/textures/%04x.bin", dir, num);

		if (fsFileSize(path) <= 0) {
			continue;
		}

		if (g_PdSurfaces[num] != g_GeImageSurfaces[image]) {
			changed++;
		}

		texSurfaceSet(num, g_GeImageSurfaces[image]);
	}

	g_GeSurfacesIn = true;

	sysLogPrintf(LOG_NOTE, "getexsurface: stage 0x%02x: %d of the level's textures take GoldenEye's surface over Perfect Dark's",
			stagenum, changed);
}

#endif

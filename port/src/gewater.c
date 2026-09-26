/**
 * GoldenEye's moving water on a converted level.
 *
 * GoldenEye's texture loader (tex.c) follows two pictures with a list of its
 * own: 1508, Caverns' water, and 1511, Dam's reservoir (also the bottom of
 * Complex's pits, black under its shading). The list (unk_092E50.c,
 * `MipMap2C_Something_Setup` and `MipMap2C_Something2_Setup`) draws the one
 * picture twice, tile 0 and tile 1, cross-fades them with the primitive LOD
 * fraction in the first cycle and shades the result in the second, under
 * `G_RM_PASS, G_RM_AA_ZB_OPA_SURF2` - no fog - with the back faces culled.
 * `sub_GAME_7F092E50()`, once a frame from the level's tick, moves tile 0's
 * origin 0.25 and 0.1 of a quarter texel a frame (s and t, wrapping at 256
 * quarters, which is two repeats of the 32 texel picture), keeps tile 1 at 90
 * and 150 quarters from it, and swings the fraction round `sin(phase) * 127 +
 * 128`, the phase gaining 0.04 radians a frame: the two copies creep across
 * the surface and one fades into the other every 157 frames, which is the
 * reservoir's ripple. The list is data the loader's output calls, and the tick
 * rewrites it, so every room drawing the picture moves with it.
 *
 * Only the motion is taken: GoldenEye's list also sets `G_RM_PASS,
 * G_RM_AA_ZB_OPA_SURF2` (no fog) and back-face culling, and with its render
 * mode the converted reservoir lost the fog it has always had here and read
 * dark green to the horizon against fogged rock. The room's own render mode
 * and culling stay; whether GoldenEye's reservoir is really unfogged is for
 * the oracle to say.
 *
 * Perfect Dark's loader has no such test, and the conversion keeps both
 * picture numbers, so the converted reservoir stood still (F3 20260926-064418).
 * The HD look's reservoir is Bean's own mesh under its water colour picture;
 * the release moves it with a normal map in its own shader, which is not
 * drawn, so it takes GoldenEye's motion on that picture instead.
 */
#include <ultra64.h>
#include <math.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/vars.h"
#include "modloader.h"
#include "gewater.h"

#ifndef PLATFORM_N64

#define GEWATER_SLOTS 4
#define GEWATER_CMDS 16

struct gewaterslot {
	s32 w;
	s32 h;
	Gfx gdl[GEWATER_CMDS];
};

static struct gewaterslot g_GeWaterSlots[GEWATER_SLOTS];
static s32 g_GeWaterNumSlots;

// GoldenEye's three: tile 0's s and t in quarter texels, and the fade's phase
static f32 g_GeWaterS;
static f32 g_GeWaterT;
static f32 g_GeWaterPhase;

static void geWaterBuild(struct gewaterslot *slot)
{
	Gfx *gdl = slot->gdl;
	const s32 s = (s32)g_GeWaterS;
	const s32 t = (s32)g_GeWaterT;
	const s32 s1 = (s + 90) & 0xff;
	const s32 t1 = (t + 150) & 0xff;
	const s32 lodfrac = (s32)(sinf(g_GeWaterPhase) * 127.0f + 128.0f);

	// The lower right corner keeps the tile its own size: the renderer reads
	// the picture's size off it (GoldenEye leaves it at 0, which its RDP only
	// wraps)
	gDPSetTileSize(gdl++, 0, s, t, s + ((slot->w - 1) << 2), t + ((slot->h - 1) << 2));
	gDPSetTileSize(gdl++, 1, s1, t1, s1 + ((slot->w - 1) << 2), t1 + ((slot->h - 1) << 2));
	gDPSetPrimColor(gdl++, 0, lodfrac, 0xff, 0xff, 0xff, 0xff);
	gDPSetTextureDetail(gdl++, G_TD_CLAMP);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetCombineLERP(gdl++, TEXEL1, TEXEL0, PRIM_LOD_FRAC, TEXEL0, TEXEL1, TEXEL0, PRIM_LOD_FRAC, TEXEL0,
			COMBINED, 0, SHADE, 0, COMBINED, 0, SHADE, 0);

	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetCycleType(gdl++, G_CYC_2CYCLE);

	gSPEndDisplayList(gdl++);
}

void geWaterTick(void)
{
	const f32 delta = g_Vars.lvupdate60freal;

	if (g_GeWaterNumSlots == 0 || !modloaderStageIsRemake(g_Vars.stagenum)) {
		return;
	}

	g_GeWaterS += delta * 0.25f;

	while (g_GeWaterS >= 256.0f) {
		g_GeWaterS -= 256.0f;
	}

	g_GeWaterT += delta * 0.1f;

	while (g_GeWaterT >= 256.0f) {
		g_GeWaterT -= 256.0f;
	}

	g_GeWaterPhase += delta * 0.04f;

	while (g_GeWaterPhase >= 6.2831802f) {
		g_GeWaterPhase -= 6.2831802f;
	}

	for (s32 i = 0; i < g_GeWaterNumSlots; i++) {
		geWaterBuild(&g_GeWaterSlots[i]);
	}
}

s32 geWaterIsWaterTexture(s32 texturenum)
{
	return (texturenum == GEWATER_TEX_DAM || texturenum == GEWATER_TEX_CAVERNS)
		&& modloaderStageIsRemake(g_Vars.stagenum);
}

Gfx *geWaterWrite(Gfx *gdl, s32 w, s32 h)
{
	struct gewaterslot *slot = NULL;

	for (s32 i = 0; i < g_GeWaterNumSlots; i++) {
		if (g_GeWaterSlots[i].w == w && g_GeWaterSlots[i].h == h) {
			slot = &g_GeWaterSlots[i];
			break;
		}
	}

	if (!slot) {
		if (g_GeWaterNumSlots >= GEWATER_SLOTS) {
			return gdl;
		}

		slot = &g_GeWaterSlots[g_GeWaterNumSlots++];
		slot->w = w;
		slot->h = h;
		geWaterBuild(slot);
	}

	gSPDisplayList(gdl++, slot->gdl);

	return gdl;
}

#endif

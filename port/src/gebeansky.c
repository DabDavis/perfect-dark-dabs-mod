/**
 * GoldenEye XBLA's skies over a level served in HD. See gebeansky.h for what
 * this is; this file is the how.
 *
 * A sky file is one dome and a cloud cap (gebeanSkyOpen()), in the release's
 * own units and axes, which are the converted level's up to a scale
 * (gebeanstagetable.h) - so the dome needs nothing but the camera's turn. It
 * is drawn round the eye as the Perfect Dark release's cube is (xblasky.c):
 * the view's rotation with its translation taken out, no depth test, sized to
 * sit inside the far plane. The dome's own height is kept, since that is what
 * the Community Edition tuned level by level (Dam's horizon ring is 4000 over
 * the placeholder's, Silo's 10000, Statue Park's 10000 under it).
 *
 * The triangles are built into a display list of their own once a level, a
 * list per picture; each frame binds the pictures and calls them.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "game/camera.h"
#include "game/env.h"
#include "game/gfxmemory.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "xblatex.h"
#include "xblastage.h"
#include "gebean.h"
#include "gebeanstage.h"
#include "gebeansky.h"

#ifndef PLATFORM_N64

#define GEBEANSKY_MAXTEX   4
#define GEBEANSKY_BATCH    15      // vertices a load, five triangles: a G_VTX holds 16 at most
#define GEBEANSKY_EXTENT   30000.0f // the farthest vertex, in what a Vtx holds

/**
 * GoldenEye's key for a level -> the Community Edition's sky for it. Five of
 * these (aztec, bunker, caves, multitemple, temple) are the release's
 * placeholder the Community Edition left in place, and drawn as it draws
 * them; Facility and Jungle have none and keep GoldenEye's.
 */
static const struct { const char *key, *sky; } skyNames[] = {
	{ "arch",  "archives" },
	{ "arec",  "control" },
	{ "azt",   "aztec" },
	{ "base",  "library" },
	{ "cave",  "caves" },
	{ "crad",  "cradle" },
	{ "cryp",  "temple" },
	{ "dam",   "dam" },
	{ "depo",  "depot" },
	{ "dest",  "frigate" },
	{ "dish",  "multitemple" },
	{ "lib",   "library" },
	{ "oat",   "caves" },
	{ "pete",  "streets" },
	{ "ref",   "complex" },
	{ "run",   "runway" },
	{ "sev",   "bunker" },
	{ "sevb",  "bunker2" },
	{ "sevx",  "sf1" },
	{ "sevxb", "sf2" },
	{ "silo",  "silo" },
	{ "stack", "library" },
	{ "tra",   "train" },
};

/**
 * GoldenEye's key for a level -> the release's own sky, drawn without the
 * Community Edition. The release's eleven are one picture, and it is
 * Surface's: a blue sky over the peaks and low sun of Surface's panorama (the
 * level's backdrop, gebeanStageRenderBackdrop()), and the Community Edition's
 * Surface sky (sf1) is that picture with its top half retouched - its bottom
 * half differs by 2 levels in 255 on average. Everywhere else the placeholder is
 * wrong and the level keeps GoldenEye's sky; on Surface GoldenEye's is a
 * lavender dusk with orange clouds, which drawn over the release's daylit
 * panorama met it in a hard seam of two skies (F3 20260925-074723).
 */
static const struct { const char *key, *sky; } releaseSkyNames[] = {
	{ "sevx",  "surface" },
};

/**
 * GoldenEye's key for a level -> the colour of its Community Edition dome at
 * the horizon, which the fog and the fill under the dome take in place of the
 * release's, only while the Community Edition is on (gebeanSkyFogColour()).
 *
 * Surface 2: the CE's sf2 is a grey storm, where the release's fog row for
 * the level (retail and the CE's alike) is GoldenEye's dark red 0x201010 -
 * under the dome that left a dark red band where the ground and the
 * panorama fogged out, and the same red filled the screen below the dome's
 * lowest ring. The CE carries no other colour for it (its fog row changes
 * only the distance, 10000 -> 6500), so the colour is the dome's own: the
 * mean of the bottom 20 of its picture's 1024 rows, the band its lowest ring
 * of vertices is drawn with (v 0.98 to 1.0), round the whole turn. The
 * bottom 4 rows give 0x575e5b, the bottom 50 0x58605e.
 */
static const struct { const char *key; u32 rgb; } ceHorizons[] = {
	{ "sevxb", 0x575f5d },
};

s32 gebeanSkyFogColour(u8 *rgb)
{
	const char *key;

	if (!gebeanCeIsActive() || (key = gebeanStageLevelKey()) == NULL) {
		return 0;
	}

	for (s32 i = 0; i < ARRAYCOUNT(ceHorizons); i++) {
		if (strcmp(ceHorizons[i].key, key) == 0) {
			rgb[0] = ceHorizons[i].rgb >> 16;
			rgb[1] = ceHorizons[i].rgb >> 8;
			rgb[2] = ceHorizons[i].rgb;
			return 1;
		}
	}

	return 0;
}

/** The sky file drawn over a level, or NULL for GoldenEye's own sky. */
static const char *skyNameFor(const char *key)
{
	if (gebeanCeIsActive()) {
		for (s32 i = 0; i < ARRAYCOUNT(skyNames); i++) {
			if (strcmp(skyNames[i].key, key) == 0) {
				return skyNames[i].sky;
			}
		}

		return NULL;
	}

	for (s32 i = 0; i < ARRAYCOUNT(releaseSkyNames); i++) {
		if (strcmp(releaseSkyNames[i].key, key) == 0) {
			return releaseSkyNames[i].sky;
		}
	}

	return NULL;
}

struct skylist {
	const void *tile;   // the picture's stand-in (xblatex.c)
	u8 alpha;
	s32 numtris;
	s32 captris;
	f32 *tris;          // numtris x 3 x {x, y, z, u, v}
	Vtx *vtx;
	Col *col;           // a colour a vertex: white, and the cap's fade in alpha
	Gfx *gdl;
};

static struct {
	const char *key;    // what was built, or tried and found nothing
	const char *name;   // the sky file it was built from, NULL for none
	s32 tried;
	struct gebeanlevel *level;
	s32 numlists;
	struct skylist lists[GEBEANSKY_MAXTEX];
	f32 extent;         // the farthest vertex from the dome's middle, file units
} sky;

static s32 skyDrawn;

static void skyFree(void)
{
	for (s32 i = 0; i < GEBEANSKY_MAXTEX; i++) {
		free(sky.lists[i].tris);
		free(sky.lists[i].vtx);
		free(sky.lists[i].col);
		free(sky.lists[i].gdl);
	}

	if (sky.level) {
		gebeanLevelClose(sky.level);
	}

	memset(&sky, 0, sizeof(sky));
}

void gebeanSkyLevelReset(void)
{
	skyFree();
	skyDrawn = 0;
}

static void skyTakeTriangle(void *arg, s32 tex, const struct gebeanlevelvtx *v)
{
	struct skylist *l;

	if (tex < 0 || tex >= GEBEANSKY_MAXTEX) {
		return;
	}

	if (tex >= sky.numlists) {
		sky.numlists = tex + 1;
	}

	l = &sky.lists[tex];

	if (l->numtris == l->captris) {
		const s32 cap = l->captris ? l->captris * 2 : 1024;
		f32 *t = realloc(l->tris, sizeof(f32) * 15 * cap);

		if (!t) {
			return;
		}

		l->tris = t;
		l->captris = cap;
	}

	for (s32 k = 0; k < 3; k++) {
		f32 *out = &l->tris[(l->numtris * 3 + k) * 5];
		const f32 r = sqrtf(v[k].pos[0] * v[k].pos[0] + v[k].pos[1] * v[k].pos[1] + v[k].pos[2] * v[k].pos[2]);

		out[0] = v[k].pos[0];
		out[1] = v[k].pos[1];
		out[2] = v[k].pos[2];
		out[3] = v[k].uv[0];
		out[4] = v[k].uv[1];

		if (r > sky.extent) {
			sky.extent = r;
		}
	}

	l->numtris++;
}

/**
 * How much of the cloud cap shows at a UV's distance from its middle. Rare's
 * own cap (the release's placeholder) fades in its picture's alpha - whole to
 * 0.33 of the way out, gone by 0.475 - and the Community Edition's caps are
 * photographs with no alpha at all, which drew their rim as a hard circle
 * across the dome some twenty degrees up. They take Rare's fade here instead.
 */
static u8 skyCapFade(f32 u, f32 v)
{
	const f32 r = sqrtf((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
	f32 t;

	if (r <= 0.33f) {
		return 255;
	}

	if (r >= 0.475f) {
		return 0;
	}

	t = (0.475f - r) / (0.475f - 0.33f);

	return (u8)(255.0f * t * t * (3.0f - 2.0f * t));
}

static s32 skyBuildList(struct skylist *l, s32 fade)
{
	const f32 k = GEBEANSKY_EXTENT / sky.extent;
	const s32 numvtx = l->numtris * 3;
	const s32 numloads = (numvtx + GEBEANSKY_BATCH - 1) / GEBEANSKY_BATCH;
	Gfx *gdl;

	l->vtx = calloc(numvtx, sizeof(Vtx));
	l->col = calloc(numvtx, sizeof(Col));
	// a load's colours, its vertices, its triangles two to a command, and the end
	l->gdl = calloc(numloads * (2 + GEBEANSKY_BATCH / 6 + 1) + 1, sizeof(Gfx));

	if (!l->vtx || !l->col || !l->gdl) {
		return 0;
	}

	for (s32 i = 0; i < numvtx; i++) {
		const f32 *in = &l->tris[i * 5];
		Vtx *v = &l->vtx[i];

		v->x = (s16)(in[0] * k);
		v->y = (s16)(in[1] * k);
		v->z = (s16)(in[2] * k);
		// The dome's u runs one repeat past 1 (0.48 to 1.48) and is wrapped;
		// v is turned over, the pictures being decoded bottom row first
		// (beanDecodeTexture(), as gebeanstage.c's rooms undo it too)
		v->s = (s16)(in[3] * XBLATEX_TILE_SCALE);
		v->t = (s16)((1.0f - in[4]) * XBLATEX_TILE_SCALE);
		v->colour = (i % GEBEANSKY_BATCH) * 4;

		l->col[i].r = 0xff;
		l->col[i].g = 0xff;
		l->col[i].b = 0xff;
		l->col[i].a = fade ? skyCapFade(in[3], in[4]) : 0xff;
	}

	gdl = l->gdl;

	for (s32 first = 0; first < numvtx; first += GEBEANSKY_BATCH) {
		const s32 n = numvtx - first < GEBEANSKY_BATCH ? numvtx - first : GEBEANSKY_BATCH;
		s32 t;

		gSPColor(gdl++, osVirtualToPhysical(&l->col[first]), n);
		gSPVertex(gdl++, osVirtualToPhysical(&l->vtx[first]), n, 0);

		for (t = 0; t + 6 <= n; t += 6) {
			gSPTri2(gdl++, t, t + 1, t + 2, t + 3, t + 4, t + 5);
		}

		if (t + 3 <= n) {
			gSPTri1(gdl++, t, t + 1, t + 2);
		}
	}

	gSPEndDisplayList(gdl++);

	free(l->tris);
	l->tris = NULL;

	return 1;
}

static s32 skyBuild(const char *key, const char *name)
{
	sky.key = key;
	sky.name = name;
	sky.tried = 1;

	if (!name) {
		return 0;
	}

	sky.level = gebeanSkyOpen(name);

	if (!sky.level) {
		return 0;
	}

	gebeanLevelTriangles(sky.level, skyTakeTriangle, NULL);

	if (sky.extent <= 0.0f || sky.numlists == 0) {
		return 0;
	}

	for (s32 i = 0; i < sky.numlists; i++) {
		struct skylist *l = &sky.lists[i];

		if (l->numtris == 0) {
			continue;
		}

		l->tile = gebeanLevelTexture(sky.level, i, &l->alpha, &(u8){0});

		// Past the first (the dome) is the cloud cap, faded where its picture
		// does not fade it
		if (!l->tile || !skyBuildList(l, i > 0 && !l->alpha)) {
			l->numtris = 0;
		}
	}

	sysLogPrintf(LOG_NOTE, "gebeansky: %s (GoldenEye's %s): %d pictures, %d + %d triangles, extent %.0f",
			name, key, sky.numlists, sky.lists[0].numtris, sky.numlists > 1 ? sky.lists[1].numtris : 0, sky.extent);

	return 1;
}

Gfx *gebeanSkyRender(Gfx *gdl)
{
	struct environment *env = envGetCurrent();
	const char *key;
	const char *name;
	u8 fill[3];
	struct zrange zrange;
	Mtxf *mtx;
	f32 scale;
	s32 any = 0;

	skyDrawn = 0;

	if (!xblaStageDrawsEveryRoom() || g_Vars.currentplayer->visionmode == VISIONMODE_XRAY) {
		return NULL;
	}

	key = gebeanStageLevelKey();

	if (!key) {
		return NULL;
	}

	// The Community Edition's where it is on, else the release's own where
	// it is the level's (skyNameFor())
	name = skyNameFor(key);

	if (!name) {
		return NULL;
	}

	if (!sky.tried || strcmp(sky.key, key) != 0 || sky.name != name) {
		skyFree();
		skyBuild(key, name);
	}

	for (s32 i = 0; i < sky.numlists; i++) {
		any |= sky.lists[i].numtris > 0;
	}

	if (!any) {
		return NULL;
	}

	mtx = gfxAllocateMatrix();

	if (!mtx) {
		return NULL;
	}

	// Under the dome's lowest ring: the level's own sky colour, as the game
	// fills a level with no clouds - or the dome's horizon, where the fog
	// takes it too (gebeanSkyFogColour())
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_FILL);

	if (gebeanSkyFogColour(fill)) {
		gdl = viSetFillColour(gdl, fill[0], fill[1], fill[2]);
	} else {
		gdl = viSetFillColour(gdl, env->sky_r, env->sky_g, env->sky_b);
	}

	gDPSetRenderMode(gdl++, G_RM_NOOP, G_RM_NOOP2);
	gDPFillRectangle(gdl++,
			g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop,
			g_Vars.currentplayer->viewleft + g_Vars.currentplayer->viewwidth - 1,
			g_Vars.currentplayer->viewtop + g_Vars.currentplayer->viewheight - 1);
	gDPPipeSync(gdl++);

	// The camera's turn and nothing of its position, the farthest vertex at
	// half the far plane: there is no depth test, but the far plane clips
	viGetZRange(&zrange);
	scale = zrange.far * 0.5f / GEBEANSKY_EXTENT;

	*mtx = *camGetWorldToScreenMtxf();

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			mtx->m[i][j] *= scale;
		}

		mtx->m[3][i] = 0;
	}

	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureConvert(gdl++, G_TC_FILT);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gSPClearGeometryMode(gdl++, G_ZBUFFER | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);

	gSPMatrix(gdl++, osVirtualToPhysical(camGetPerspectiveMtxL()), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);

	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);

	// The dome, then the cloud cap blended over it: its colour the picture's,
	// its alpha the picture's times the vertex's fade
	for (s32 i = 0; i < sky.numlists; i++) {
		const struct skylist *l = &sky.lists[i];

		if (l->numtris == 0) {
			continue;
		}

		gDPPipeSync(gdl++);

		if (i > 0) {
			gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
			gDPSetCombineLERP(gdl++, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0);
		} else {
			gDPSetRenderMode(gdl++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
			gDPSetCombineMode(gdl++, G_CC_DECALRGB, G_CC_DECALRGB);
		}

		{
			// The dome's u wraps round the horizon; nothing else repeats
			gDPLoadTextureBlock(gdl++, (void *)l->tile, G_IM_FMT_RGBA, G_IM_SIZ_16b,
					XBLATEX_TILE, XBLATEX_TILE, 0,
					i > 0 ? G_TX_CLAMP : G_TX_WRAP, G_TX_CLAMP,
					XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);

			gSPDisplayList(gdl++, osVirtualToPhysical(l->gdl));
		}
	}

	// The frame turned the depth test on before the sky and nothing after it
	// turns it on again (xblasky.c)
	gSPSetGeometryMode(gdl++, G_ZBUFFER);
	gDPPipeSync(gdl++);

	skyDrawn = 1;

	return gdl;
}

s32 gebeanSkyIsDrawn(void)
{
	return skyDrawn;
}

#else

#include <PR/ultratypes.h>
#include "gebeansky.h"

Gfx *gebeanSkyRender(Gfx *gdl) { return NULL; }
s32 gebeanSkyFogColour(u8 *rgb) { return 0; }
s32 gebeanSkyIsDrawn(void) { return 0; }
void gebeanSkyLevelReset(void) { }

#endif

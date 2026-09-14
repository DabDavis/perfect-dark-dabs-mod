/**
 * The release's skies, drawn in place of the game's. See xblasky.h for what
 * this is; this file is the how.
 *
 * Everything here was read off the release's own draws (a Xenia capture of
 * Defection, CLAUDE-notes/xbla.md "The skies"): 4J transforms the sky on the
 * CPU, so each vertex of each sky draw carries its clip position, and fitting
 * the six faces' corners gives their view-projection exactly, which is what
 * turns the cloud layer's clip positions back into a shape.
 */

#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "config.h"
#include "system.h"
#include "gbiex.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "xblaimport.h"
#include "xblatex.h"
#include "xblasky.h"

#define XBLASKY_FACES 6

// The first face of each cube. The record before each is its cloud layer.
// Found by joining the faces up: every one of these meets its neighbours along
// all twelve edges once its rows are turned over, and no other run of six in
// 0e56-0e92 does (0e87-0e8b are five loose pictures).
enum {
	XBLASKY_GREY,   // 0e57, a grey storm with a bright band at the horizon
	XBLASKY_SUNSET, // 0e5e, a red sunset with the sun on the horizon
	XBLASKY_SPACE,  // 0e65, stars and a planet
	XBLASKY_DUSK,   // 0e6c, brown dusty dusk
	XBLASKY_FIRE,   // 0e73, a fiery orange evening
	XBLASKY_CITY,   // 0e7a, a night skyline with a moon
	XBLASKY_SKEDAR, // 0e81, the Skedar planet: an aurora over a dark horizon, three suns
	XBLASKY_DAY,    // 0e8d, a blue day above the clouds
	XBLASKY_NUMCUBES
};

static const u16 xblaSkyFirstFace[XBLASKY_NUMCUBES] = {
	0x0e57, 0x0e5e, 0x0e65, 0x0e6c, 0x0e73, 0x0e7a, 0x0e81, 0x0e8d,
};

struct xblaskystage {
	s32 stagenum;
	s32 cube;
};

// What the release draws where. "Recorded" rows are what a Xenia capture of
// the release showed; the rest are chosen by the picture and say so.
//
// Levels with no row keep the game's sky: the Carrington Institute (its menu
// drew no sky cube in the release), Deep Sea and Investigation (nothing of a
// sky to see), and the arenas whose captures showed only ceilings (Grid,
// Area 52, Base, Fortress).
static const struct xblaskystage xblaSkyStages[] = {
	// Recorded from the release (Xenia, Defection and the Combat Simulator).
	{ STAGE_DEFECTION,    XBLASKY_CITY },   // the skyline, frame for frame
	{ STAGE_MP_SKEDAR,    XBLASKY_SKEDAR }, // 0e83 overhead
	{ STAGE_MP_RAVINE,    XBLASKY_DUSK },   // brown overcast overhead
	{ STAGE_MP_RUINS,     XBLASKY_DUSK },   // brown overcast through the roof
	{ STAGE_MP_VILLA,     XBLASKY_DAY },    // blue day, thin cloud
	{ STAGE_MP_TEMPLE,    XBLASKY_DAY },    // a flat blue patch up a shaft, as 0e8f; thin evidence

	// Chosen by the picture against the N64 environment table (env.c).
	{ STAGE_VILLA,        XBLASKY_DAY },    // the arena's map, which was recorded
	{ STAGE_CRASHSITE,    XBLASKY_SUNSET }, // red sky and a low sun; the only sun on a horizon
	{ STAGE_ATTACKSHIP,   XBLASKY_SPACE },  // the only cube in space
	{ STAGE_SKEDARRUINS,  XBLASKY_SKEDAR }, // three suns, as the arena
	{ STAGE_WAR,          XBLASKY_SKEDAR },
	{ STAGE_CHICAGO,      XBLASKY_DUSK },   // the brown cloud colour Ravine's is
	{ STAGE_G5BUILDING,   XBLASKY_DUSK },   // Chicago's night, the same clouds
	{ STAGE_EXTRACTION,   XBLASKY_CITY },   // Defection's district
	{ STAGE_MBR,          XBLASKY_CITY },
	{ STAGE_AIRBASE,      XBLASKY_FIRE },   // a low evening sun
	{ STAGE_INFILTRATION, XBLASKY_FIRE },   // Area 51's yellow sun
	{ STAGE_RESCUE,       XBLASKY_FIRE },
	{ STAGE_ESCAPE,       XBLASKY_FIRE },
	{ STAGE_AIRFORCEONE,  XBLASKY_GREY },   // weather at altitude
	{ STAGE_PELAGIC,      XBLASKY_GREY },   // grey-blue sea fog
};

// The cube's half size in a vertex's own units; the matrix scales it to sit
// inside the far plane. Everything below is measured in halves of the cube.
#define XBLASKY_EXTENT 1000

// The cloud layer: a flat square of 8 by 8 quads 0.119 of the cube above the
// eye and 0.116 a side, its picture repeating 0.748 times a quad, its vertices
// transparent round the rim, 60 on the next ring in and 120 inside, and its
// picture drifting 0.0245 and 0.0122 of a repeat a second.
#define XBLASKY_CLOUD_QUADS  8
#define XBLASKY_CLOUD_HEIGHT 0.119f
#define XBLASKY_CLOUD_QUAD   0.116f
#define XBLASKY_CLOUD_REPEAT 0.748f
#define XBLASKY_CLOUD_DRIFTS 0.0245f
#define XBLASKY_CLOUD_DRIFTT 0.0122f

#define XBLASKY_CLOUD_VERTS  (XBLASKY_CLOUD_QUADS + 1)

static s32 optEnabled = 1;

static const void *xblaSkyTiles[XBLASKY_NUMCUBES][XBLASKY_FACES + 1];
static Vtx xblaSkyVertices[XBLASKY_FACES * 4];
static Col xblaSkyWhite[4];
static Col xblaSkyCloudColours[XBLASKY_CLOUD_VERTS * XBLASKY_CLOUD_VERTS];
static s32 xblaSkyVerticesBuilt;
static s32 xblaSkyDrawn;

PD_CONSTRUCTOR static void xblaSkyInit(void)
{
	configRegisterInt("Mod.XblaSkies", &optEnabled, 0, 1);
}

s32 xblaSkyGetEnabled(void)
{
	return optEnabled;
}

void xblaSkySetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled == optEnabled) {
		return;
	}

	optEnabled = enabled;

	// As with the explosions, turning it on is where a player's archive comes
	// apart; the draw never unpacks anything.
	if (enabled) {
		xblaTexGetNumRecords();
	}
}

static s32 xblaSkyCubeOfStage(s32 stagenum)
{
	for (s32 i = 0; i < ARRAYCOUNT(xblaSkyStages); i++) {
		if (xblaSkyStages[i].stagenum == stagenum) {
			return xblaSkyStages[i].cube;
		}
	}

	return -1;
}

/**
 * Where a point of a face lies in the game's world, sc and tc being the face's
 * own coordinates from -1 to 1, across and down.
 *
 * Direct3D's cube lookup gives the direction; the release's world is that
 * direction turned by (x, y, z) -> (-z, y, -x). That is a mirror, not a turn,
 * and it is what the capture says: at Defection's spawn the release's camera
 * looks down -z of the lookup with -x to its right, and the game's looks down
 * +x with +z to its right.
 */
static void xblaSkyFacePoint(s32 face, f32 sc, f32 tc, f32 out[3])
{
	f32 d[3];

	switch (face) {
	case 0: d[0] = 1;   d[1] = -tc; d[2] = -sc; break;
	case 1: d[0] = -1;  d[1] = -tc; d[2] = sc;  break;
	case 2: d[0] = sc;  d[1] = 1;   d[2] = tc;  break;
	case 3: d[0] = sc;  d[1] = -1;  d[2] = -tc; break;
	case 4: d[0] = sc;  d[1] = -tc; d[2] = 1;   break;
	default: d[0] = -sc; d[1] = -tc; d[2] = -1; break;
	}

	out[0] = -d[2];
	out[1] = d[1];
	out[2] = -d[0];
}

/**
 * Four vertices a face, the same for every cube. A face's texture coordinates
 * run over the stand-in tile, and t counts from the first row the record
 * stores, which is the bottom of Direct3D's face - hence 1 - v. Each face is
 * loaded on its own, so its four vertices name colours 0 to 3.
 */
static void xblaSkyBuildVertices(void)
{
	static const f32 corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

	for (s32 face = 0; face < XBLASKY_FACES; face++) {
		for (s32 c = 0; c < 4; c++) {
			const f32 u = corners[c][0];
			const f32 v = corners[c][1];
			Vtx *vtx = &xblaSkyVertices[face * 4 + c];
			f32 pos[3];

			xblaSkyFacePoint(face, u * 2 - 1, v * 2 - 1, pos);

			vtx->x = (s16)(pos[0] * XBLASKY_EXTENT);
			vtx->y = (s16)(pos[1] * XBLASKY_EXTENT);
			vtx->z = (s16)(pos[2] * XBLASKY_EXTENT);
			vtx->flags = 0;
			vtx->colour = (u8)(c << 2);
			vtx->s = (s16)(u * XBLATEX_TILE_SCALE);
			vtx->t = (s16)((1 - v) * XBLATEX_TILE_SCALE);
		}
	}

	for (s32 c = 0; c < 4; c++) {
		xblaSkyWhite[c].r = xblaSkyWhite[c].g = xblaSkyWhite[c].b = xblaSkyWhite[c].a = 0xff;
	}

	for (s32 row = 0; row < XBLASKY_CLOUD_VERTS; row++) {
		for (s32 col = 0; col < XBLASKY_CLOUD_VERTS; col++) {
			const s32 ring = MIN(MIN(row, col), MIN(XBLASKY_CLOUD_QUADS - row, XBLASKY_CLOUD_QUADS - col));
			Col *c = &xblaSkyCloudColours[row * XBLASKY_CLOUD_VERTS + col];

			c->r = c->g = c->b = 0xff;
			c->a = ring == 0 ? 0 : ring == 1 ? 60 : 120;
		}
	}

	xblaSkyVerticesBuilt = 1;
}

static s32 xblaSkyBindCube(s32 cube)
{
	for (s32 i = 0; i <= XBLASKY_FACES; i++) {
		if (!xblaSkyTiles[cube][i]) {
			// Slot 6 is the cloud layer, the record in front of the faces.
			const u32 record = i < XBLASKY_FACES ? xblaSkyFirstFace[cube] + i : xblaSkyFirstFace[cube] - 1;

			xblaSkyTiles[cube][i] = xblaTexBind(record);

			if (!xblaSkyTiles[cube][i]) {
				return 0;
			}
		}
	}

	return 1;
}

/**
 * The cloud layer, a row of quads at a time: a row is eighteen vertices, under
 * the renderer's 25, and names its colours from the grid's two rows it spans.
 * The picture's coordinates follow the world, u along x and v along z, and
 * the drift is kept to under one repeat so a Vtx's 10.5 can hold it.
 */
static Gfx *xblaSkyRenderClouds(Gfx *gdl, const void *tile)
{
	const f32 secs = g_Vars.lvframe60 / 60.0f;
	f32 drifts = XBLASKY_CLOUD_DRIFTS * secs;
	f32 driftt = XBLASKY_CLOUD_DRIFTT * secs;
	const f32 perunit = XBLASKY_CLOUD_REPEAT / XBLASKY_CLOUD_QUAD;
	const f32 half = XBLASKY_CLOUD_QUADS * XBLASKY_CLOUD_QUAD / 2;

	drifts -= (s32)drifts;
	driftt -= (s32)driftt;

	gDPPipeSync(gdl++);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA2);

	gDPLoadTextureBlock(gdl++, tile, G_IM_FMT_RGBA, G_IM_SIZ_16b,
			XBLATEX_TILE, XBLATEX_TILE, 0,
			G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
			XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);

	for (s32 row = 0; row < XBLASKY_CLOUD_QUADS; row++) {
		Vtx *vtx = gfxAllocateVertices(XBLASKY_CLOUD_VERTS * 2);

		if (!vtx) {
			break;
		}

		for (s32 r = 0; r < 2; r++) {
			for (s32 col = 0; col < XBLASKY_CLOUD_VERTS; col++) {
				const f32 x = col * XBLASKY_CLOUD_QUAD - half;
				const f32 z = (row + r) * XBLASKY_CLOUD_QUAD - half;
				const s32 k = r * XBLASKY_CLOUD_VERTS + col;

				vtx[k].x = (s16)(x * XBLASKY_EXTENT);
				vtx[k].y = (s16)(XBLASKY_CLOUD_HEIGHT * XBLASKY_EXTENT);
				vtx[k].z = (s16)(z * XBLASKY_EXTENT);
				vtx[k].flags = 0;
				vtx[k].colour = (u8)(k << 2);
				vtx[k].s = (s16)((x * perunit + drifts) * XBLATEX_TILE_SCALE);
				vtx[k].t = (s16)((z * perunit + driftt) * XBLATEX_TILE_SCALE);
			}
		}

		gSPColor(gdl++, osVirtualToPhysical(&xblaSkyCloudColours[row * XBLASKY_CLOUD_VERTS]), XBLASKY_CLOUD_VERTS * 2);
		gSPVertex(gdl++, osVirtualToPhysical(vtx), XBLASKY_CLOUD_VERTS * 2, 0);

		for (s32 col = 0; col < XBLASKY_CLOUD_QUADS; col++) {
			const s32 a = col;
			const s32 b = col + 1;
			const s32 c = XBLASKY_CLOUD_VERTS + col + 1;
			const s32 d = XBLASKY_CLOUD_VERTS + col;

			gSP1Triangle(gdl++, a, b, c, 0);
			gSP1Triangle(gdl++, a, c, d, 0);
		}
	}

	return gdl;
}

Gfx *xblaSkyRender(Gfx *gdl)
{
	struct zrange zrange;
	Mtxf *mtx;
	f32 scale;
	s32 cube;

	xblaSkyDrawn = 0;

	// The record stand-ins are the release's art only while Enable Textures
	// is on; off, every face would be the tile's own white.
	if (!optEnabled || !xblaTexGetEnabled() || !xblaImportGetReadyStfsPath()) {
		return NULL;
	}

	if (g_Vars.currentplayer->visionmode == VISIONMODE_XRAY) {
		return NULL;
	}

	cube = xblaSkyCubeOfStage(mainGetStageNum());

	if (cube < 0 || !xblaSkyBindCube(cube)) {
		return NULL;
	}

	if (!xblaSkyVerticesBuilt) {
		xblaSkyBuildVertices();
	}

	mtx = gfxAllocateMatrix();

	if (!mtx) {
		return NULL;
	}

	// The camera's turn and nothing of its position, so the cube stays round
	// the eye. Sized so its corners sit inside the far plane: there is no
	// depth test to lose to, but the far plane still clips.
	viGetZRange(&zrange);
	scale = zrange.far * 0.5f / XBLASKY_EXTENT;

	*mtx = *camGetWorldToScreenMtxf();

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			mtx->m[i][j] *= scale;
		}

		mtx->m[3][i] = 0;
	}

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureConvert(gdl++, G_TC_FILT);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetRenderMode(gdl++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
	gDPSetCombineMode(gdl++, G_CC_DECALRGB, G_CC_DECALRGB2);
	gSPClearGeometryMode(gdl++, G_ZBUFFER | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);

	gSPMatrix(gdl++, osVirtualToPhysical(camGetPerspectiveMtxL()), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);

	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
	gSPColor(gdl++, osVirtualToPhysical(xblaSkyWhite), 4);

	for (s32 face = 0; face < XBLASKY_FACES; face++) {
		// Clamped: a face's edge meets the next face, not its own far edge.
		gDPLoadTextureBlock(gdl++, xblaSkyTiles[cube][face], G_IM_FMT_RGBA, G_IM_SIZ_16b,
				XBLATEX_TILE, XBLATEX_TILE, 0,
				G_TX_CLAMP, G_TX_CLAMP,
				XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);

		gSPVertex(gdl++, osVirtualToPhysical(&xblaSkyVertices[face * 4]), 4, 0);
		gSP1Triangle(gdl++, 0, 1, 2, 0);
		gSP1Triangle(gdl++, 0, 2, 3, 0);
	}

	gdl = xblaSkyRenderClouds(gdl, xblaSkyTiles[cube][XBLASKY_FACES]);

	// The frame turned the depth test on once before the sky (zbuf.c) and
	// nothing after it turns it on again: leaving it off drew every room, prop
	// and gun in list order, the far ones through the near.
	gSPSetGeometryMode(gdl++, G_ZBUFFER);
	gDPPipeSync(gdl++);

	xblaSkyDrawn = 1;

	return gdl;
}

s32 xblaSkyIsDrawn(void)
{
	return xblaSkyDrawn;
}

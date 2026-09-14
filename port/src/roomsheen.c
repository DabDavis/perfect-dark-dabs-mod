/**
 * Level Sheen - see roomsheen.h.
 *
 * A room's copy is built from its display lists as they stand after
 * texLoadFromGdl() and the fog replacements, so the render modes read here
 * are the ones the room is drawn with. A model part's is built from its
 * opaque list the first time it is drawn, since the texture commands go in
 * after the model registers.
 *
 * Either copy is triangle soup: three vertices a triangle, four triangles a
 * batch, each batch its own G_COL window of normals, which keeps every colour
 * index and every G_TRI4 index in range. The vertices are named through
 * segment 4 so that a draw can bind its own copy of them: a model part's
 * positions are read again from its vertex array at every draw (a door trims
 * its own as it opens, and two props of one model share the part), where the
 * normals are the ones worked out when the copy was built.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "gbiex.h"
#include "bss.h"
#include "config.h"
#include "system.h"
#include "game/bg.h"
#include "game/dlights.h"
#include "game/gfxmemory.h"
#include "xblamesh.h"
#include "xblatex.h"
#include "roomsheen.h"

// A normal is shared with a neighbour's only when the faces meet at less
// than about 37 degrees, so a box keeps its edges and a curve is smooth.
#define ROOMSHEEN_SMOOTH_COS 0.8f

// The K7's own texture scale for its texgen spans (G_TEXTURE 0x0800).
#define ROOMSHEEN_K7_SCALE 0x0800

#define ROOMSHEEN_TRIS_PER_BATCH 4

// bgGetNextGdlInLayer()'s opaque layer; bg.c keeps the name to itself
#define ROOMSHEEN_LAYER_OPA 0x01

// The segment the copy's vertices are named through
#define ROOMSHEEN_VTXSEG ((uintptr_t)SPSEGMENT_MODEL_VTX << 24)

// objRender()'s renderdata for a solid prop that is not fading
#define ROOMSHEEN_PROP_OPAQUE 9

// The model part cache: open addressing, and never more than three quarters
// full, so a probe always ends
#define ROOMSHEEN_NODE_SLOTS 8192

struct roomsheen {
	Gfx *gdl;     // NULL when there is nothing to shine
	Vtx *vtx;
	Col *col;
	u32 *srcidx;  // a model part's: each vertex's index in the part's array
	s32 numvtx;
	s32 colkey;   // the style, share and brightness the alphas were written for
};

struct roomsheennode {
	const void *node;
	Gfx *list;
	u32 w0;             // the list's first command, so a part freed and its
	uintptr_t w1;       // address reused inside a stage is not taken for it
	struct roomsheen *sheen;
	Vtx *drawbase;      // the vertex array the shared copy was last filled from
	s32 drawframe;
};

static s32 optLevel = 0;
static s32 optStyle = ROOMSHEEN_STYLE_K7;

static struct prop *currentProp;
static struct roomsheennode *nodeSlots;
static s32 numNodes;

// The sheen's share of the surface at each level, as vertex alpha. The K7's
// own metal draws at the whole of it.
static const u8 roomSheenShares[] = { 0, 64, 128, 255 };

PD_CONSTRUCTOR static void roomSheenConfigInit(void)
{
	configRegisterInt("Mod.LevelSheen", &optLevel, 0, 3);
	configRegisterInt("Mod.LevelSheenStyle", &optStyle, ROOMSHEEN_STYLE_K7, ROOMSHEEN_STYLE_PIXEL);
}

s32 roomSheenGetLevel(void)
{
	return optLevel;
}

void roomSheenSetLevel(s32 level)
{
	optLevel = level < 0 ? 0 : level > 3 ? 3 : level;
}

s32 roomSheenGetStyle(void)
{
	return optStyle;
}

void roomSheenSetStyle(s32 style)
{
	optStyle = style == ROOMSHEEN_STYLE_PIXEL ? ROOMSHEEN_STYLE_PIXEL : ROOMSHEEN_STYLE_K7;
}

struct roomsheencollect {
	f32 *pos;       // nine a triangle
	Vtx **src;      // three a triangle: where each corner was read from
	s32 num;
	s32 cap;
	Vtx *slot[16];  // what each vertex slot holds, as the lists load them
	s32 skip;       // the current render mode is a decal, a cutout or translucent
};

static void roomSheenAddTri(struct roomsheencollect *c, s32 a, s32 b, s32 d)
{
	const s32 idx[3] = { a, b, d };
	f32 *p;

	if (c->skip || a > 15 || b > 15 || d > 15 || !c->slot[a] || !c->slot[b] || !c->slot[d]) {
		return;
	}

	if (c->num == c->cap) {
		s32 cap = c->cap ? c->cap * 2 : 256;
		f32 *grownpos = realloc(c->pos, (size_t)cap * 9 * sizeof(f32));
		Vtx **grownsrc;

		if (!grownpos) {
			return;
		}

		c->pos = grownpos;
		grownsrc = realloc(c->src, (size_t)cap * 3 * sizeof(Vtx *));

		if (!grownsrc) {
			return;
		}

		c->src = grownsrc;
		c->cap = cap;
	}

	p = &c->pos[c->num * 9];

	for (s32 k = 0; k < 3; k++) {
		Vtx *v = c->slot[idx[k]];

		p[k * 3 + 0] = v->x;
		p[k * 3 + 1] = v->y;
		p[k * 3 + 2] = v->z;
		c->src[c->num * 3 + k] = v;
	}

	c->num++;
}

static void roomSheenWalk(struct roomsheencollect *c, Gfx *gdl, Vtx *base, s32 depth)
{
	for (Gfx *g = gdl; ; g++) {
		const u32 w0 = g->words.w0;
		const uintptr_t w1 = g->words.w1;
		const u8 op = (u8)(w0 >> 24);

		if (op == (u8)G_ENDDL) {
			return;
		}

		if (op == G_VTX) {
			const s32 n = (s32)((w0 >> 20) & 0xf) + 1;
			const s32 v0 = (s32)((w0 >> 16) & 0xf);
			Vtx *v = (Vtx *)((uintptr_t)base + (UNSEGADDR(w1) & 0xffffff));

			for (s32 i = 0; i < n && v0 + i < 16; i++) {
				c->slot[v0 + i] = &v[i];
			}
		} else if (op == (u8)G_TRI1) {
			roomSheenAddTri(c, (s32)((w1 >> 16) & 0xff) / 10, (s32)((w1 >> 8) & 0xff) / 10, (s32)(w1 & 0xff) / 10);
		} else if (op == (u8)G_TRI4) {
			for (s32 k = 0; k < 4; k++) {
				const s32 x = (s32)((w1 >> (k * 8)) & 0xf);
				const s32 y = (s32)((w1 >> (k * 8 + 4)) & 0xf);
				const s32 z = (s32)((w0 >> (k * 4)) & 0xf);

				// gfx_sp_tri4() draws a slot unless all three are zero
				if (x || y || z) {
					roomSheenAddTri(c, x, y, z);
				}
			}
		} else if (op == (u8)G_SETOTHERMODE_L) {
			const u32 shift = (w0 >> 8) & 0xff;

			// The render mode's constants are already in their place in the
			// word, so a whole-word set and a render-mode set read the same.
			// A model's lists set none: model.c writes its render modes.
			if (shift == G_MDSFT_RENDERMODE || shift == 0) {
				const u32 zmode = (u32)w1 & ZMODE_DEC;

				c->skip = zmode == ZMODE_DEC || zmode == ZMODE_XLU || ((u32)w1 & CVG_X_ALPHA);
			}
		} else if (op == G_DL) {
			// A room's lists are flat as far as the files go; a branch to an
			// address, never a segment, is followed in case one is not
			if (!(w1 & 1) && w1 && depth < 4) {
				roomSheenWalk(c, (Gfx *)w1, base, depth + 1);
			}

			if ((w0 >> 16) & 1) {
				return;
			}
		}
	}
}

struct roomsheencorner {
	u64 key;
	s32 index;
};

static int roomSheenCornerCmp(const void *a, const void *b)
{
	const u64 ka = ((const struct roomsheencorner *)a)->key;
	const u64 kb = ((const struct roomsheencorner *)b)->key;

	return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static void roomSheenFreeCopy(struct roomsheen *sheen)
{
	if (sheen) {
		free(sheen->gdl);
		free(sheen->vtx);
		free(sheen->col);
		free(sheen->srcidx);
		free(sheen);
	}
}

/**
 * The copy, from what a walk collected. base is the vertex array a model
 * part's corners index, or NULL for a room, whose corners are never read
 * again. Takes the collection's arrays over.
 */
static struct roomsheen *roomSheenFromCollect(struct roomsheencollect *c, Vtx *base)
{
	struct roomsheen *sheen = calloc(1, sizeof(*sheen));
	struct roomsheencorner *corners = NULL;
	f32 *face = NULL;
	f32 *area = NULL;
	f32 *normal = NULL;
	const s32 numtris = c->num;

	if (!sheen || numtris == 0) {
		goto done;
	}

	corners = malloc((size_t)numtris * 3 * sizeof(*corners));
	face = malloc((size_t)numtris * 3 * sizeof(f32));
	area = malloc((size_t)numtris * 3 * sizeof(f32));
	normal = malloc((size_t)numtris * 9 * sizeof(f32));
	sheen->vtx = malloc((size_t)numtris * 3 * sizeof(Vtx));
	sheen->col = malloc((size_t)numtris * 3 * sizeof(Col));
	sheen->gdl = malloc(((size_t)(numtris + ROOMSHEEN_TRIS_PER_BATCH - 1) / ROOMSHEEN_TRIS_PER_BATCH * 3 + 1) * sizeof(Gfx));

	if (base) {
		sheen->srcidx = malloc((size_t)numtris * 3 * sizeof(u32));
	}

	if (!corners || !face || !area || !normal || !sheen->vtx || !sheen->col || !sheen->gdl
			|| (base && !sheen->srcidx)) {
		free(sheen->vtx);
		free(sheen->col);
		free(sheen->gdl);
		free(sheen->srcidx);
		memset(sheen, 0, sizeof(*sheen));
		goto done;
	}

	// Each face's normal, unit and area-weighted (the cross product itself)
	for (s32 t = 0; t < numtris; t++) {
		const f32 *p = &c->pos[t * 9];
		const f32 e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] };
		const f32 e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
		const f32 n[3] = {
			e1[1] * e2[2] - e1[2] * e2[1],
			e1[2] * e2[0] - e1[0] * e2[2],
			e1[0] * e2[1] - e1[1] * e2[0],
		};
		const f32 len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

		for (s32 k = 0; k < 3; k++) {
			area[t * 3 + k] = n[k];
			face[t * 3 + k] = len > 1e-6f ? n[k] / len : 0.0f;
		}

		for (s32 k = 0; k < 3; k++) {
			const u16 x = (u16)(s16)p[k * 3 + 0];
			const u16 y = (u16)(s16)p[k * 3 + 1];
			const u16 z = (u16)(s16)p[k * 3 + 2];

			corners[t * 3 + k].key = ((u64)x << 32) | ((u64)y << 16) | z;
			corners[t * 3 + k].index = t * 3 + k;
		}
	}

	qsort(corners, (size_t)numtris * 3, sizeof(*corners), roomSheenCornerCmp);

	// Each corner's normal: the faces meeting at its position that turn from
	// its own face by a shallow angle, weighted by their area
	for (s32 lo = 0, hi; lo < numtris * 3; lo = hi) {
		for (hi = lo + 1; hi < numtris * 3 && corners[hi].key == corners[lo].key; hi++);

		for (s32 i = lo; i < hi; i++) {
			const s32 ti = corners[i].index / 3;
			const f32 *fi = &face[ti * 3];
			f32 sum[3] = { 0, 0, 0 };
			f32 len;

			for (s32 j = lo; j < hi; j++) {
				const s32 tj = corners[j].index / 3;
				const f32 *fj = &face[tj * 3];

				if (fi[0] * fj[0] + fi[1] * fj[1] + fi[2] * fj[2] >= ROOMSHEEN_SMOOTH_COS) {
					sum[0] += area[tj * 3];
					sum[1] += area[tj * 3 + 1];
					sum[2] += area[tj * 3 + 2];
				}
			}

			len = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);

			for (s32 k = 0; k < 3; k++) {
				normal[corners[i].index * 3 + k] = len > 1e-6f ? sum[k] / len : fi[k];
			}
		}
	}

	// The copy: three vertices a triangle, a batch of four triangles to a
	// G_COL window and a G_VTX load, then one G_TRI4
	{
		Gfx *g = sheen->gdl;

		for (s32 t = 0; t < numtris; t += ROOMSHEEN_TRIS_PER_BATCH) {
			const s32 ntris = numtris - t < ROOMSHEEN_TRIS_PER_BATCH ? numtris - t : ROOMSHEEN_TRIS_PER_BATCH;
			const s32 first = t * 3;
			u32 tw0 = (u32)G_TRI4 << 24;
			u32 tw1 = 0;

			for (s32 k = 0; k < ntris * 3; k++) {
				const s32 i = first + k;
				const f32 *p = &c->pos[(t + k / 3) * 9 + (k % 3) * 3];
				const f32 *n = &normal[i * 3];
				Vtx *v = &sheen->vtx[i];

				v->x = (s16)p[0];
				v->y = (s16)p[1];
				v->z = (s16)p[2];
				v->flags = 0;
				v->colour = (u8)(k * 4);

				// One cell of a one-cell atlas, the way the per-pixel lookup
				// reads it: s is the cell's middle, t one over the cells
				v->s = XBLATEX_TILE * 32 / 2;
				v->t = XBLATEX_TILE * 32;

				sheen->col[i].r = (u8)(s8)lroundf(n[0] * 127.0f);
				sheen->col[i].g = (u8)(s8)lroundf(n[1] * 127.0f);
				sheen->col[i].b = (u8)(s8)lroundf(n[2] * 127.0f);
				sheen->col[i].a = 0;

				if (base) {
					sheen->srcidx[i] = (u32)(c->src[i] - base);
				}
			}

			for (s32 k = 0; k < ntris; k++) {
				tw1 |= (u32)(k * 3) << (k * 8);
				tw1 |= (u32)(k * 3 + 1) << (k * 8 + 4);
				tw0 |= (u32)(k * 3 + 2) << (k * 4);
			}

			g->words.w0 = ((u32)G_COL << 24) | (u32)(ntris * 3 * sizeof(Col));
			g->words.w1 = (uintptr_t)&sheen->col[first];
			g++;

			gSPVertex(g, SEGADDR(ROOMSHEEN_VTXSEG | (uintptr_t)(first * sizeof(Vtx))), ntris * 3, 0);
			g++;

			g->words.w0 = tw0;
			g->words.w1 = tw1;
			g++;
		}

		gSPEndDisplayList(g);
	}

	sheen->numvtx = numtris * 3;
	sheen->colkey = -1;

done:
	free(corners);
	free(face);
	free(area);
	free(normal);
	free(c->pos);
	free(c->src);
	c->pos = NULL;
	c->src = NULL;

	return sheen;
}

static struct roomsheen *roomSheenBuild(s32 roomnum)
{
	struct roomsheencollect c;

	memset(&c, 0, sizeof(c));

	for (Gfx *gdl = bgGetNextGdlInLayer(roomnum, NULL, ROOMSHEEN_LAYER_OPA); gdl;
			gdl = bgGetNextGdlInLayer(roomnum, gdl, ROOMSHEEN_LAYER_OPA)) {
		Vtx *base = bgFindVerticesForGdl(roomnum, gdl);

		if (base) {
			memset(c.slot, 0, sizeof(c.slot));
			c.skip = 0;
			roomSheenWalk(&c, gdl, base, 0);
		}
	}

	return roomSheenFromCollect(&c, NULL);
}

void roomSheenFree(s32 roomnum)
{
	roomSheenFreeCopy(g_Rooms[roomnum].sheen);
	g_Rooms[roomnum].sheen = NULL;
}

/** The alphas: the share, and for a room drawn per pixel the room's light. */
static void roomSheenWriteAlpha(struct roomsheen *sheen, s32 bright)
{
	const s32 share = roomSheenShares[optLevel];
	const s32 key = (optStyle << 16) | (share << 8) | bright;

	if (key != sheen->colkey) {
		const u8 alpha = (u8)((share * bright + 127) / 255);

		for (s32 i = 0; i < sheen->numvtx; i++) {
			sheen->col[i].a = alpha;
		}

		sheen->colkey = key;
	}
}

// How far the player moves, in world units, for the streaks to scroll one
// whole span of the texgen (two tiles of 0x3eb at the K7's scale)
#define ROOMSHEEN_SHIFT_PERIOD 400.0f

// A move longer than this in one frame is a teleport, a respawn or a cut, and
// scrolls nothing
#define ROOMSHEEN_SHIFT_JUMP 200.0f

static struct {
	struct coord pos;
	f32 s, t;
	s32 frame;
	s32 seen;
} roomSheenShifts[MAX_PLAYERS];

static f32 roomSheenWrap(f32 v)
{
	return v - floorf(v);
}

Gfx *roomSheenTexgenShift(Gfx *gdl)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;

	if (!player || playernum < 0 || playernum >= MAX_PLAYERS) {
		return gdl;
	}

	if (roomSheenShifts[playernum].frame != g_Vars.lvframenum) {
		const struct coord *pos = &player->cam_pos;
		const struct coord *look = &player->cam_look;
		const struct coord *up = &player->cam_up;
		const f32 dx = pos->x - roomSheenShifts[playernum].pos.x;
		const f32 dy = pos->y - roomSheenShifts[playernum].pos.y;
		const f32 dz = pos->z - roomSheenShifts[playernum].pos.z;

		if (roomSheenShifts[playernum].seen && dx * dx + dy * dy + dz * dz < ROOMSHEEN_SHIFT_JUMP * ROOMSHEEN_SHIFT_JUMP) {
			// Across the view scrolls s, and along it or up and down scrolls t
			const f32 rx = look->y * up->z - look->z * up->y;
			const f32 ry = look->z * up->x - look->x * up->z;
			const f32 rz = look->x * up->y - look->y * up->x;
			const f32 across = dx * rx + dy * ry + dz * rz;
			const f32 along = dx * look->x + dy * look->y + dz * look->z;
			const f32 rise = dx * up->x + dy * up->y + dz * up->z;

			roomSheenShifts[playernum].s = roomSheenWrap(roomSheenShifts[playernum].s + across / ROOMSHEEN_SHIFT_PERIOD);
			roomSheenShifts[playernum].t = roomSheenWrap(roomSheenShifts[playernum].t + (along - rise) / ROOMSHEEN_SHIFT_PERIOD);
		}

		roomSheenShifts[playernum].pos = *pos;
		roomSheenShifts[playernum].frame = g_Vars.lvframenum;
		roomSheenShifts[playernum].seen = true;
	}

	gDPSetTexgenShiftEXT(gdl++, roomSheenShifts[playernum].s * 16384.0f, roomSheenShifts[playernum].t * 16384.0f);

	return gdl;
}

/**
 * The pass itself, over a copy whose vertices are vtx. lightroom is the room
 * whose light the K7's texgen is lit by, or zero for the default light.
 */
static Gfx *roomSheenEmit(Gfx *gdl, struct roomsheen *sheen, const Vtx *vtx, const void *tile, s32 lightroom, s32 fog)
{
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_2CYCLE);

	// Under fog the first cycle is the fog blend the room's own lists take,
	// and the renderer fades what the draw adds by the fog rather than
	// mixing it towards the fog colour (SHADER_OPT_FOG_FADE)
	if (fog && g_FogEnabled) {
		gDPSetRenderMode(gdl++, G_RM_FOG_SHADE_A, G_RM_AA_ZB_XLU_INTER2);
	} else {
		gDPSetRenderMode(gdl++, G_RM_AA_ZB_XLU_INTER, G_RM_AA_ZB_XLU_INTER2);
	}

	gDPLoadTextureBlock(gdl++, tile, G_IM_FMT_RGBA, G_IM_SIZ_16b,
			XBLATEX_TILE, XBLATEX_TILE, 0,
			G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
			XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);
	gDPSetTile(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b,
			((XBLATEX_TILE * G_IM_SIZ_16b_LINE_BYTES) + 7) >> 3, 0, 1, 0,
			G_TX_WRAP | G_TX_NOMIRROR, XBLATEX_TILE_MASK, G_TX_NOLOD,
			G_TX_WRAP | G_TX_NOMIRROR, XBLATEX_TILE_MASK, G_TX_NOLOD);
	gDPSetTileSize(gdl++, 1, 0, 0,
			(XBLATEX_TILE - 1) << G_TEXTURE_IMAGE_FRAC,
			(XBLATEX_TILE - 1) << G_TEXTURE_IMAGE_FRAC);

	gSPSegment(gdl++, SPSEGMENT_MODEL_VTX, osVirtualToPhysical((void *)vtx));

	// Multiplied into what was drawn rather than added on top of it
	// (G_MULADD_EXT: dst + src * dst), so a dark wall stays dark and a lit one
	// catches the streak. The blend reads no alpha, so the amount is folded
	// into the colour: the second cycle is the first times the shade alpha.
	if (optStyle == ROOMSHEEN_STYLE_PIXEL) {
		gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
		gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE_ALPHA, 0, 0, 0, 0, SHADE,
				0, 0, 0, COMBINED, 0, 0, 0, COMBINED);
		gSPSetExtraGeometryModeEXT(gdl++, G_MULADD_EXT | G_ENVMAP_EXT);
		gSPDisplayList(gdl++, sheen->gdl);
		gSPClearExtraGeometryModeEXT(gdl++, G_MULADD_EXT | G_ENVMAP_EXT);
	} else {
		gSPTexture(gdl++, ROOMSHEEN_K7_SCALE, ROOMSHEEN_K7_SCALE, 0, G_TX_RENDERTILE, G_ON);

		if (lightroom > 0) {
			gdl = lightsSetForRoom(gdl, lightroom);
		} else {
			gdl = lightsSetDefault(gdl);
		}

		gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, 0, 0, 0, 0, SHADE,
				COMBINED, 0, SHADE_ALPHA, 0, 0, 0, 0, COMBINED);
		gdl = roomSheenTexgenShift(gdl);
		gSPSetGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN);
		gSPSetExtraGeometryModeEXT(gdl++, G_MULADD_EXT | G_TEXGEN_EYE_EXT);
		gSPDisplayList(gdl++, sheen->gdl);
		gSPClearExtraGeometryModeEXT(gdl++, G_MULADD_EXT | G_TEXGEN_EYE_EXT);
		gSPClearGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN);
		gdl = lightsSetDefault(gdl);
	}

	return gdl;
}

Gfx *roomSheenRender(Gfx *gdl, s32 roomnum)
{
	struct roomsheen *sheen;
	const void *tile;

	if (optLevel <= 0 || roomnum <= 0 || roomnum >= g_Vars.roomcount
			|| !g_Rooms[roomnum].loaded240 || !g_Rooms[roomnum].gfxdata) {
		return gdl;
	}

	if (!g_Rooms[roomnum].sheen) {
		g_Rooms[roomnum].sheen = roomSheenBuild(roomnum);
	}

	sheen = g_Rooms[roomnum].sheen;
	tile = xblaMeshSheenTile();

	if (!sheen || !sheen->gdl || !tile) {
		return gdl;
	}

	// The K7's texgen is lit, so the room's light reaches it through the
	// lights; the per-pixel lookup reads shade RGB as the normal, so it takes
	// the room's light in the alpha
	roomSheenWriteAlpha(sheen, optStyle == ROOMSHEEN_STYLE_PIXEL ? roomGetFinalBrightnessForPlayer(roomnum) : 255);

	return roomSheenEmit(gdl, sheen, sheen->vtx, tile, roomnum, true);
}

void roomSheenSetProp(struct prop *prop)
{
	currentProp = prop;
}

void roomSheenResetNodes(void)
{
	if (nodeSlots) {
		for (s32 i = 0; i < ROOMSHEEN_NODE_SLOTS; i++) {
			roomSheenFreeCopy(nodeSlots[i].sheen);
		}

		memset(nodeSlots, 0, ROOMSHEEN_NODE_SLOTS * sizeof(*nodeSlots));
	}

	numNodes = 0;
}

/** The part's cache entry, made (without a copy) if there is room; or NULL. */
static struct roomsheennode *roomSheenFindNode(const void *node, Gfx *list)
{
	u32 i;

	if (!nodeSlots) {
		nodeSlots = calloc(ROOMSHEEN_NODE_SLOTS, sizeof(*nodeSlots));

		if (!nodeSlots) {
			return NULL;
		}
	}

	i = (u32)(((uintptr_t)node >> 3) * 2654435761u ^ ((uintptr_t)list >> 3)) & (ROOMSHEEN_NODE_SLOTS - 1);

	for (s32 probe = 0; probe < ROOMSHEEN_NODE_SLOTS; probe++, i = (i + 1) & (ROOMSHEEN_NODE_SLOTS - 1)) {
		struct roomsheennode *e = &nodeSlots[i];

		if (e->node == NULL) {
			if (numNodes >= ROOMSHEEN_NODE_SLOTS * 3 / 4) {
				return NULL;
			}

			e->node = node;
			e->list = list;
			e->w0 = list->words.w0;
			e->w1 = list->words.w1;
			e->sheen = NULL;
			e->drawbase = NULL;
			e->drawframe = -1;
			numNodes++;
			return e;
		}

		if (e->node == node && e->list == list) {
			// The same addresses for a different list: the part was freed and
			// its memory reused, so its copy is of something else
			if (e->w0 != list->words.w0 || e->w1 != list->words.w1) {
				roomSheenFreeCopy(e->sheen);
				e->sheen = NULL;
				e->w0 = list->words.w0;
				e->w1 = list->words.w1;
				e->drawbase = NULL;
				e->drawframe = -1;
			}

			return e;
		}
	}

	return NULL;
}

void roomSheenRenderNode(struct modelrenderdata *renderdata, const void *node, Gfx *list, Vtx *base, s32 cutout)
{
	struct roomsheennode *e;
	struct roomsheen *sheen;
	const void *tile;
	Vtx *vtx;
	s32 lightroom;

	if (optLevel <= 0 || cutout || !currentProp || !list || !base || !node
			|| !(renderdata->flags & MODELRENDERFLAG_OPA) || !renderdata->zbufferenabled
			|| renderdata->unk30 != ROOMSHEEN_PROP_OPAQUE || currentProp->parent
			// A low alpha byte is glass or a fade, drawn with its texture's
			// alpha cut out (modelApplyRenderModeType3)
			|| (renderdata->envcolour & 0xff) != 0
			|| (currentProp->type != PROPTYPE_OBJ && currentProp->type != PROPTYPE_DOOR
				&& currentProp->type != PROPTYPE_WEAPON)) {
		return;
	}

	tile = xblaMeshSheenTile();
	e = roomSheenFindNode(node, list);

	if (!tile || !e) {
		return;
	}

	if (!e->sheen) {
		struct roomsheencollect c;

		memset(&c, 0, sizeof(c));
		roomSheenWalk(&c, list, base, 0);
		e->sheen = roomSheenFromCollect(&c, base);
	}

	sheen = e->sheen;

	if (!sheen || !sheen->gdl) {
		return;
	}

	// The positions, from the part's vertices as they are now. The shared
	// copy serves the first draw of a frame; a second prop of the same model
	// in that frame fills a copy of its own, since the renderer reads them
	// all after the frame's lists are built.
	if (e->drawframe == g_Vars.lvframenum && e->drawbase != base) {
		vtx = gfxAllocate((u32)sheen->numvtx * sizeof(Vtx));

		if (!vtx) {
			return;
		}

		memcpy(vtx, sheen->vtx, (size_t)sheen->numvtx * sizeof(Vtx));
	} else {
		vtx = sheen->vtx;
		e->drawframe = g_Vars.lvframenum;
		e->drawbase = base;
	}

	for (s32 i = 0; i < sheen->numvtx; i++) {
		const Vtx *src = &base[sheen->srcidx[i]];

		vtx[i].x = src->x;
		vtx[i].y = src->y;
		vtx[i].z = src->z;
	}

	roomSheenWriteAlpha(sheen, 255);

	lightroom = currentProp->rooms[0];

	renderdata->gdl = roomSheenEmit(renderdata->gdl, sheen, vtx, tile,
			lightroom > 0 && lightroom < g_Vars.roomcount ? lightroom : 0, false);

	gSPSegment(renderdata->gdl++, SPSEGMENT_MODEL_VTX, osVirtualToPhysical(base));
}

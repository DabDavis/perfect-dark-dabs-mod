/**
 * GoldenEye XBLA's HD levels as the rooms of the levels converted from
 * GoldenEye's own ROM - gebeanstage.h has the shape of it,
 * CLAUDE-notes/ge-bean.md "The levels" the reasons.
 */

#ifndef PLATFORM_N64

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "system.h"
#include "lib/rzip.h"
#include "game/bg.h"
#include "lib/vi.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "game/env.h"
#include "game/modoptions.h"
#include "game/pad.h"
#include "romdata.h"
#include "xblatex.h"
#include "xblastage.h"
#include "gebean.h"
#include "fs.h"
#include "gebeanstage.h"
#include "gebeansky.h"
#include "gewater.h"
#include "texpack.h"

#define SEG 0x0f000000

// The room format's sizes, as the file has them (preprocess/filebg.c)
#define GFXHEADER 0x18
#define ROOMBLOCKSIZE 20
#define VTXSIZE   12
#define COLSIZE   4

// The grid Bean's own triangles are filed in, for finding the ones a decal
// lies on (file units)
#define BEAN_CELL 64.0f

// A Bean triangle goes to the room of the nearest triangle of the level file,
// looked for this many grid cells out.
#define ASSIGN_CELL  64.0f
#define ASSIGN_RINGS 2

// A triangle lying this close to the plane of a triangle of another picture
// (file units), and over it, is a decal on it: Rare drew the Aztec's BAY-4
// lettering, floor arrows and hazard stripes flat on the floor
#define DECAL_DIST 1.0f
#define DECAL_COS  0.999f
// How far a decal is moved off its base, world units: more than the room's
// whole-unit vertices can put it behind (writeLeaf())
#define DECAL_LIFT 2.0f
// Vertex colours this far apart (mean of r, g, b) make one picture two (triOther())
#define SHADE_APART 48.0f
// How far the back of a face that fights another back to back is pushed from
// the eye (G_SETDEPTHBIAS_EXT, the depth buffer's smallest steps)
#define FIGHT_DEPTH_BIAS 8

#define MAXPALETTE 64
#define BATCHVERTS 16

struct stagerow {
	// GoldenEye's own key for the level, which is the name the conversion
	// writes it under: files/bgdata/bg_gx<key>.seg (geconvert.c)
	const char *key;
	const char *bean;
	f32 scale;
	// Taken off after scaling: the conversion moves a level so that the middle
	// of its walkable area is the origin (geconvert.c's offset[])
	f32 offset[3];
};

static const struct stagerow stageRows[] = {
#include "gebeanstagetable.h"
};

extern u32 var8007fc54;

struct stri {
	f32 pos[3][3];
	f32 uv[3][2];
	u32 argb[3];
	s16 tex;
	u16 room;
	u8 decal;
	u8 lift;    // a decal lifted off its base before rounding (markDecalLifts())
	s8 sink;    // a translucent base sunk under its decal instead: +1 along its normal's back, -1 its front
	s32 decalbase; // the triangle a decal was found lying on, or -1
	u8 nofog;   // on a triangle GoldenEye draws without fog (fileRoomTrianglesEach())
	u8 backed;  // one face of a two-faced sheet, drawn culled (markBacked())
	u8 fights;  // a face with another face back to back over part of it (markFights())
	u8 blend;   // drawn in the release's blended pass (triFades())
	u8 undersea; // the reflection under a sea, faded and culled (markUnderSea())
	u8 plain;   // of a draw with no UV and no picture of its own (gebeanlevelvtx)
	u8 overlap; // a face with another room's space behind it, drawn culled (markOverlaps())
};

// The level being served, built when its first room is asked for
static s32 tried;
static s32 built;
static const struct stagerow *row;
static struct gebeanlevel *level;
static u8 **roomData;
static u32 *roomLen;
static s32 numRooms;
static s32 numServed;
static u8 *roomHidden;  // a room of the file's that Bean's mesh leaves out (gebeanStageRoomHidden())
static s32 numHidden;
// A room whose translucent chain ends in GoldenEye's own water leaves
// (fileWaterLeaves()), after its HD leaf where it has one (1 + that)
static u8 *roomWaterKept;
static s32 numWaterKept;
static s32 levelHasWater; // Bean draws water of its own (texWater[])

static const void *texTile[GEBEAN_MAXMATS];
static u8 texAlpha[GEBEAN_MAXMATS];
static u8 texSoft[GEBEAN_MAXMATS];
// A cut-out whose every triangle keeps v within one repeat [k, k + 1]: drawn
// with t clamped, from a batch whose v shift is k + 1 (texClampShift[])
static u8 texClampV[GEBEAN_MAXMATS];
static s16 texClampShift[GEBEAN_MAXMATS];
// The reservoir's picture: Bean's water buffers (stride 36) draw it
static u8 texWater[GEBEAN_MAXMATS];

// How the file scrolls a picture, in repeats a second (gebeanLevelTextureScroll(),
// the release's texture matrix): its rooms draw it by moving its tile, which a
// list of its own does each frame (scrollSlot())
static f32 texScroll[GEBEAN_MAXMATS][2];

#define SCROLL_SLOTS 8

static struct {
	s32 tex;
	s32 w;
	s32 h;
	Gfx gdl[6];
} scrollSlots[SCROLL_SLOTS];
static s32 numScrollSlots;

// The level's backdrop: a picture whose every triangle stands outside the
// level (gebeanStageRenderBackdrop())
static struct stri *backdrop;
static s32 numBackdrop;
static s32 *backdropOrder;
static f32 *backdropDist;
static f32 backdropMid[3];

// The HD mesh's own extent (the rooms' triangles, not the backdrop), for the
// far plane (gebeanStageTickFar())
static f32 meshMin[3];
static f32 meshMax[3];
static s32 farRaised;
static f32 farOwn;
static f32 farSet;
static f32 farLogged; // the own plane last logged as raised

static void fogTableLoad(void);

#define GRID_BITS 20

static u32 gridKey(s32 x, s32 y, s32 z)
{
	return ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & ((1u << GRID_BITS) - 1);
}

/* -------------------------------------------------------------------------
 * A triangle grid: each triangle filed in every cell its box touches
 * ------------------------------------------------------------------------- */

struct tgrid {
	f32 cell;
	s32 *head;
	s32 *entnext;
	s32 *enttri;
	s32 nument, capent;
	f32 *tri;
	// The room a file triangle is in, or the index of a Bean one
	s32 *room;
	s32 numtri, captri;
	// tgridNearest()'s: each triangle's box, and the last ask that measured
	// it (a triangle is filed in every cell it touches, and was measured
	// again in each of them)
	f32 *box;
	u32 *seen;
	u32 ask;
};

static s32 tgridInit(struct tgrid *g, f32 cell)
{
	memset(g, 0, sizeof(*g));
	g->cell = cell;
	g->head = malloc(sizeof(s32) << GRID_BITS);

	if (!g->head) {
		return 0;
	}

	memset(g->head, 0xff, sizeof(s32) << GRID_BITS);

	return 1;
}

static void tgridFree(struct tgrid *g)
{
	free(g->head);
	free(g->entnext);
	free(g->enttri);
	free(g->tri);
	free(g->room);
	free(g->box);
	free(g->seen);
	memset(g, 0, sizeof(*g));
}

static void tgridAdd(struct tgrid *g, const f32 v[3][3], s32 room)
{
	s32 lo[3], hi[3];
	s32 cells = 1;

	if (g->numtri >= g->captri) {
		s32 cap = g->captri ? g->captri * 2 : 16384;
		f32 *t = realloc(g->tri, sizeof(f32) * 9 * cap);
		s32 *r = realloc(g->room, sizeof(s32) * cap);
		f32 *b = realloc(g->box, sizeof(f32) * 6 * cap);
		u32 *sn = realloc(g->seen, sizeof(u32) * cap);

		if (t) g->tri = t;
		if (r) g->room = r;
		if (b) g->box = b;
		if (sn) g->seen = sn;
		if (!t || !r || !b || !sn) return;

		g->captri = cap;
	}

	for (s32 k = 0; k < 3; k++) {
		f32 mn = v[0][k], mx = v[0][k];

		for (s32 j = 1; j < 3; j++) {
			if (v[j][k] < mn) mn = v[j][k];
			if (v[j][k] > mx) mx = v[j][k];
		}

		lo[k] = (s32)floorf(mn / g->cell);
		hi[k] = (s32)floorf(mx / g->cell);
		cells *= hi[k] - lo[k] + 1;
	}

	// A triangle that big is sky or distant scenery, and no room's vertex
	// work would be saved by filing it everywhere
	if (cells > 4096) {
		return;
	}

	for (s32 k = 0; k < 3; k++) {
		memcpy(g->tri + g->numtri * 9 + k * 3, v[k], sizeof(f32) * 3);
	}

	g->room[g->numtri] = room;
	g->seen[g->numtri] = 0;

	for (s32 k = 0; k < 3; k++) {
		f32 mn = v[0][k], mx = v[0][k];

		for (s32 j = 1; j < 3; j++) {
			if (v[j][k] < mn) mn = v[j][k];
			if (v[j][k] > mx) mx = v[j][k];
		}

		g->box[g->numtri * 6 + k] = mn;
		g->box[g->numtri * 6 + 3 + k] = mx;
	}

	for (s32 x = lo[0]; x <= hi[0]; x++) {
		for (s32 y = lo[1]; y <= hi[1]; y++) {
			for (s32 z = lo[2]; z <= hi[2]; z++) {
				const u32 key = gridKey(x, y, z);

				if (g->nument >= g->capent) {
					s32 cap = g->capent ? g->capent * 2 : 65536;
					s32 *n = realloc(g->entnext, sizeof(s32) * cap);
					s32 *t = realloc(g->enttri, sizeof(s32) * cap);

					if (n) g->entnext = n;
					if (t) g->enttri = t;
					if (!n || !t) return;

					g->capent = cap;
				}

				g->entnext[g->nument] = g->head[key];
				g->enttri[g->nument] = g->numtri;
				g->head[key] = g->nument;
				g->nument++;
			}
		}
	}

	g->numtri++;
}

static f32 dot3(const f32 *a, const f32 *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/** Squared distance from p to the triangle abc (Ericson, closest point on a triangle). */
static f32 pointTriDist(const f32 *p, const f32 *a, const f32 *b, const f32 *c)
{
	f32 ab[3], ac[3], ap[3], bp[3], cp[3], q[3], d[3];
	f32 d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;

	for (s32 k = 0; k < 3; k++) {
		ab[k] = b[k] - a[k];
		ac[k] = c[k] - a[k];
		ap[k] = p[k] - a[k];
	}

	d1 = dot3(ab, ap);
	d2 = dot3(ac, ap);

	if (d1 <= 0 && d2 <= 0) {
		memcpy(q, a, sizeof(q));
		goto done;
	}

	for (s32 k = 0; k < 3; k++) bp[k] = p[k] - b[k];

	d3 = dot3(ab, bp);
	d4 = dot3(ac, bp);

	if (d3 >= 0 && d4 <= d3) {
		memcpy(q, b, sizeof(q));
		goto done;
	}

	vc = d1 * d4 - d3 * d2;

	if (vc <= 0 && d1 >= 0 && d3 <= 0) {
		v = d1 / (d1 - d3);
		for (s32 k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v;
		goto done;
	}

	for (s32 k = 0; k < 3; k++) cp[k] = p[k] - c[k];

	d5 = dot3(ab, cp);
	d6 = dot3(ac, cp);

	if (d6 >= 0 && d5 <= d6) {
		memcpy(q, c, sizeof(q));
		goto done;
	}

	vb = d5 * d2 - d1 * d6;

	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		w = d2 / (d2 - d6);
		for (s32 k = 0; k < 3; k++) q[k] = a[k] + ac[k] * w;
		goto done;
	}

	va = d3 * d6 - d5 * d4;

	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		for (s32 k = 0; k < 3; k++) q[k] = b[k] + (c[k] - b[k]) * w;
		goto done;
	}

	denom = 1.0f / (va + vb + vc);
	v = vb * denom;
	w = vc * denom;

	for (s32 k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;

done:
	for (s32 k = 0; k < 3; k++) d[k] = p[k] - q[k];

	return dot3(d, d);
}

/** The squared distance from p to a box, which no point of what is in it is nearer than. */
static f32 boxDist(const f32 *p, const f32 *box)
{
	f32 d = 0.0f;

	for (s32 k = 0; k < 3; k++) {
		const f32 out = p[k] < box[k] ? box[k] - p[k] : (p[k] > box[3 + k] ? p[k] - box[3 + k] : 0.0f);

		d += out * out;
	}

	return d;
}

/**
 * The triangle nearest p within `rings` cells, or -1; its squared distance in *outd.
 *
 * Two shortcuts, neither of which can change the answer: a triangle already
 * measured in another cell of this ask is not measured again (the same
 * distance, and only a nearer one takes the place), and one whose box is
 * clearly further than the best so far is not measured at all (its triangle
 * is no nearer than its box; the margin is for the rounding of the exact
 * measurement). Dealing Dam's 110000 triangles took 1.1 of its 2.9 seconds.
 */
static s32 tgridNearest(const struct tgrid *g, const f32 *p, s32 rings, f32 *outd)
{
	const s32 cx = (s32)floorf(p[0] / g->cell);
	const s32 cy = (s32)floorf(p[1] / g->cell);
	const s32 cz = (s32)floorf(p[2] / g->cell);
	s32 best = -1;
	f32 bestd = 0;
	u32 ask;

	// a tgrid is the builder's alone; the ask count is not part of what it holds
	((struct tgrid *)g)->ask++;
	ask = g->ask;

	if (ask == 0) {
		memset(g->seen, 0, sizeof(u32) * g->numtri);
		((struct tgrid *)g)->ask = ask = 1;
	}

	for (s32 r = 0; r <= rings; r++) {
		for (s32 x = cx - r; x <= cx + r; x++) {
			for (s32 y = cy - r; y <= cy + r; y++) {
				for (s32 z = cz - r; z <= cz + r; z++) {
					if (x != cx - r && x != cx + r && y != cy - r && y != cy + r && z != cz - r && z != cz + r) {
						continue;
					}

					for (s32 e = g->head[gridKey(x, y, z)]; e >= 0; e = g->entnext[e]) {
						const s32 t = g->enttri[e];
						f32 d;

						if (g->seen[t] == ask) {
							continue;
						}

						g->seen[t] = ask;

						if (best >= 0 && boxDist(p, g->box + t * 6) > bestd * 1.001f + 1.0f) {
							continue;
						}

						d = pointTriDist(p, g->tri + t * 9, g->tri + t * 9 + 3, g->tri + t * 9 + 6);

						if (best < 0 || d < bestd) {
							best = t;
							bestd = d;
						}
					}
				}
			}
		}

		if (best >= 0 && bestd <= (r * g->cell) * (r * g->cell)) {
			break;
		}
	}

	*outd = bestd;

	return best;
}

/* -------------------------------------------------------------------------
 * The level file's rooms as they are
 * ------------------------------------------------------------------------- */

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 be16(const u8 *p)
{
	return ((u16)p[0] << 8) | p[1];
}

static void put32(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void put16(u8 *p, u16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

/** A room of the level file as the file has it (inflated, big-endian), or NULL. */
static u8 *readRoom(s32 r, u32 *outLen)
{
	const u32 base = g_BgRooms[r].unk00;
	const u32 len = g_BgRooms[r + 1].unk00 - base;
	u8 *packed;
	u8 *raw;
	u32 rawlen;
	u8 scratch[5120];

	*outLen = 0;

	if (len < GFXHEADER || len > 16 * 1024 * 1024) {
		return NULL;
	}

	packed = malloc(ALIGN16(len) + 16);

	if (!packed) {
		return NULL;
	}

	bgLoadFile(packed, base - SEG - var8007fc54, ALIGN16(len));

	if (!rzipIs1173(packed)) {
		*outLen = len;
		return packed;
	}

	rawlen = ((u32)packed[2] << 16) | ((u32)packed[3] << 8) | packed[4];
	raw = malloc(rawlen + 16);

	if (!raw || rzipInflate(packed, raw, scratch) != (s32)rawlen) {
		free(packed);
		free(raw);
		return NULL;
	}

	free(packed);
	*outLen = rawlen;

	return raw;
}

/**
 * Files every triangle of a room of the level file, as its display lists draw
 * them: each leaf's G_VTX loads up to 16 of the leaf's vertices, each G_TRI4
 * draws up to four of them. `xlutoo` 0 walks the opaque leaves, 1 both, and
 * EACH_XLU_ONLY the translucent ones alone.
 */
#define EACH_XLU_ONLY 2

// The texture number of the triangle fileRoomTrianglesEach() is handing over
static s32 eachTex = -1;
// and the alpha of the environment colour its leaf last set (G_SETENVCOLOR)
static u8 eachEnvAlpha = 0xff;
// and its three vertices' s and t, as the file stores them
static s16 eachSt[3][2];
// and how bright its three vertices' colours are (the brightest of r, g and
// b), or 0xff when the leaf's colours cannot be read
static u8 eachShade[3];
// and their colours, r, g and b (white where eachShade is 0xff for want of them)
static u8 eachRgb[3][3];

static void fileRoomTrianglesEach(s32 r, const u8 *raw, u32 len, s32 xlutoo,
		void (*fn)(void *arg, const f32 v[3][3], s32 room), void *arg)
{
	const u32 base = g_BgRooms[r].unk00;
	u32 stack[64];
	s32 depth = 0;
	// whether GoldenEye culls what it draws here: its rooms are drawn culled,
	// and a leaf turns it off for a sheet meant to be seen from both sides -
	// the Cradle's platform, seen from under it in the ending
	s32 twosided = 0;

	if (xlutoo != EACH_XLU_ONLY) {
		stack[depth++] = be32(raw + 8);
	}

	if (xlutoo) {
		stack[depth++] = be32(raw + 12);
	}

	while (depth > 0) {
		u32 b = stack[--depth];

		for (s32 guard = 0; b && guard < 256; guard++) {
			const u32 o = b - base;
			u32 gdl, vtx;

			if (o + ROOMBLOCKSIZE > len) {
				break;
			}

			gdl = be32(raw + o + 8);
			vtx = be32(raw + o + 12);

			if (raw[o] == 1) {
				if (depth < ARRAYCOUNT(stack) && gdl) {
					stack[depth++] = gdl;
				}
			} else if (gdl && gdl - base < len && vtx - base < len) {
				f32 loaded[16][3];
				s16 loadedst[16][2];
				u8 loadedcol[16];
				u8 loadedrgb[16][3];
				// the leaf's colours and where its last G_COL loaded from
				const u32 colbase = be32(raw + o + 16);
				s32 colat = -1;

				// the texture GoldenEye's hit test reads for the triangles
				// that follow: the last one the leaf's own list loads
				eachTex = -1;
				eachEnvAlpha = 0xff;
				// Whether the leaf's render mode is one bg.c's fog swap
				// (g_GfxGroup01/05) leaves alone: cycle 1 neither G_RM_PASS,
				// which the swap turns to fog, nor fog already. GoldenEye draws
				// Caverns' water in G_RM_AA_ZB_OPA_SURF, one cycle, unfogged
				s32 nofog = 0;

				for (u32 c = gdl - base; c + 8 <= len; c += 8) {
					const u8 op = raw[c];

					if (op == (u8)G_CLEARGEOMETRYMODE && (be32(raw + c + 4) & G_CULL_BACK)) {
						twosided = 1;
					} else if (op == (u8)G_SETGEOMETRYMODE && (be32(raw + c + 4) & G_CULL_BACK)) {
						twosided = 0;
					} else if (op == 0xc0) {
						eachTex = be32(raw + c + 4) & 0xfff;
					} else if (op == (u8)G_SETENVCOLOR) {
						eachEnvAlpha = raw[c + 7];

					} else if (op == (u8)G_SETOTHERMODE_L && be32(raw + c) == 0xb900031d) {
						const u32 c1 = be32(raw + c + 4) & 0xcccc0000;

						nofog = c1 != (GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1) & 0xcccc0000)
							&& c1 != (GBL_c1(G_BL_CLR_FOG, G_BL_A_SHADE, G_BL_CLR_IN, G_BL_1MA) & 0xcccc0000);
					} else if (op == G_COL) {
						colat = colbase ? (s32)(colbase - base + (be32(raw + c + 4) & 0xffffff)) : -1;
					} else if (op == G_VTX) {
						const s32 num = (raw[c + 1] >> 4) + 1;
						const u32 at = vtx - base + (be32(raw + c + 4) & 0xffffff);

						for (s32 i = 0; i < num && at + i * VTXSIZE + 6 <= len; i++) {
							// the vertex's colour: byte 7 is its offset into the
							// colours G_COL loaded, four bytes each (r, g, b, a)
							const u32 ca = colat >= 0 && at + i * VTXSIZE + 8 <= len
								? (u32)colat + (raw[at + i * VTXSIZE + 7] & ~3) : len;

							loadedcol[i] = ca + 3 <= len
								? MAX(raw[ca], MAX(raw[ca + 1], raw[ca + 2])) : 0xff;
							for (s32 ch = 0; ch < 3; ch++) {
								loadedrgb[i][ch] = ca + 3 <= len ? raw[ca + ch] : 0xff;
							}
							loadedst[i][0] = at + i * VTXSIZE + 12 <= len ? (s16)be16(raw + at + i * VTXSIZE + 8) : 0;
							loadedst[i][1] = at + i * VTXSIZE + 12 <= len ? (s16)be16(raw + at + i * VTXSIZE + 10) : 0;
							loaded[i][0] = g_BgRooms[r].pos.x + (s16)be16(raw + at + i * VTXSIZE);
							loaded[i][1] = g_BgRooms[r].pos.y + (s16)be16(raw + at + i * VTXSIZE + 2);
							loaded[i][2] = g_BgRooms[r].pos.z + (s16)be16(raw + at + i * VTXSIZE + 4);
						}
					} else if (op == (u8)G_TRI4) {
						const u32 w0 = be32(raw + c);
						const u32 w1 = be32(raw + c + 4);

						for (s32 k = 0; k < 4; k++) {
							const s32 x = (w1 >> (k * 8)) & 0xf;
							const s32 y = (w1 >> (k * 8 + 4)) & 0xf;
							const s32 z = (w0 >> (k * 4)) & 0xf;
							f32 v[3][3];

							if (x == 0 && y == 0 && z == 0) {
								continue;
							}

							memcpy(v[0], loaded[x], sizeof(v[0]));
							memcpy(v[1], loaded[y], sizeof(v[1]));
							memcpy(v[2], loaded[z], sizeof(v[2]));
							memcpy(eachSt[0], loadedst[x], sizeof(eachSt[0]));
							memcpy(eachSt[1], loadedst[y], sizeof(eachSt[1]));
							memcpy(eachSt[2], loadedst[z], sizeof(eachSt[2]));
							eachShade[0] = loadedcol[x];
							eachShade[1] = loadedcol[y];
							eachShade[2] = loadedcol[z];
							memcpy(eachRgb[0], loadedrgb[x], sizeof(eachRgb[0]));
							memcpy(eachRgb[1], loadedrgb[y], sizeof(eachRgb[1]));
							memcpy(eachRgb[2], loadedrgb[z], sizeof(eachRgb[2]));
							fn(arg, (const f32 (*)[3])v, (u16)r | (nofog << 16) | (twosided << 17));
						}
					} else if (op == (u8)G_ENDDL) {
						break;
					}
				}
			}

			b = be32(raw + o + 4);
		}
	}
}

/**
 * Counts a room's triangles and those of them that lie on Bean's mesh (arg:
 * the grid of Bean's triangles).
 */
static s32 hiddenTris, hiddenNear;

static void fileTriNearBean(void *arg, const f32 v[3][3], s32 room)
{
	f32 mid[3], d;

	hiddenTris++;

	for (s32 j = 0; j < 3; j++) {
		mid[j] = (v[0][j] + v[1][j] + v[2][j]) / 3.0f;
	}

	if (tgridNearest(arg, mid, 1, &d) >= 0 && d < BEAN_CELL * BEAN_CELL) {
		hiddenNear++;
	}
}

static void gridBrightNote(s32 index, s32 room);

static void fileTriToGrid(void *arg, const f32 v[3][3], s32 room)
{
	struct tgrid *g = arg;
	const s32 before = g->numtri;

	tgridAdd(g, v, room);

	if (g->numtri > before) {
		gridBrightNote(before, room);
	}
}

static void fileRoomTriangles(struct tgrid *g, s32 r, const u8 *raw, u32 len)
{
	fileRoomTrianglesEach(r, raw, len, 1, fileTriToGrid, g);
}

/* -------------------------------------------------------------------------
 * Is the camera outside the level?
 *
 * GoldenEye culls the back faces of its opaque room geometry, and it frames
 * its own cameras - a mission's opening shots, the swirl down to Bond, its
 * cutscenes - with that in mind: Caverns' first shot stands under the
 * shaft's water and looks up through it, its second inside the rock beside
 * the shaft, and its swirl begins under the lift floor Bond stands on. Bean's
 * mesh has to be drawn two-sided, since 4J built decks, stair treads and
 * roofs as single planes whose undersides are meant to be seen (Cradle,
 * Facility, Runway), so from those cameras the HD level showed the
 * undersides of the water, the rock and the floor instead.
 *
 * So while one of GoldenEye's own cameras is in charge, a grid of rays
 * through the view is tested against GoldenEye's own opaque triangles, which
 * are closed where Bean's are not (its decks are boxes). Mostly back faces
 * means the camera is outside GoldenEye's level, and the HD rooms' opaque
 * leaves are drawn culled for that frame (bgRenderRoomOpaque()).
 * ------------------------------------------------------------------------- */

static f32 *shellTri;
static u8 *shellTwo;  // per triangle: GoldenEye draws it unculled, so it has no back
static s32 shellNumTwo;
static s16 *shellTex;  // each one's texture number, for gebeanStageHitTexture()
static s32 shellNum;
static s32 shellCap;
static s32 *shellFirst;
static s32 *shellCount;
static s32 cullOutside;
static s32 shellHits;  // the last test's rays that met the level, and of those its back
static s32 shellBacks;

#define SHELL_RAYS_X 8
#define SHELL_RAYS_Y 6

static void fileTriToShell(void *arg, const f32 v[3][3], s32 room)
{
	if (shellNum >= shellCap) {
		const s32 cap = shellCap ? shellCap * 2 : 16384;
		f32 *t = realloc(shellTri, sizeof(f32) * 9 * cap);
		u8 *two;
		s16 *tex;

		if (!t) {
			return;
		}

		shellTri = t;
		two = realloc(shellTwo, cap);

		if (!two) {
			return;
		}

		shellTwo = two;

		tex = realloc(shellTex, sizeof(s16) * cap);

		if (!tex) {
			return;
		}

		shellTex = tex;
		shellCap = cap;
	}

	memcpy(shellTri + shellNum * 9, v, sizeof(f32) * 9);
	shellTwo[shellNum] = (room >> 17) & 1;
	shellNumTwo += shellTwo[shellNum];
	shellTex[shellNum] = (s16)eachTex;
	shellNum++;
}

static s32 normalize3(f32 *v)
{
	const f32 len = sqrtf(dot3(v, v));

	if (len < 1e-6f) {
		return 0;
	}

	v[0] /= len;
	v[1] /= len;
	v[2] /= len;

	return 1;
}

static void shellForget(void);

/**
 * GoldenEye's own opaque triangles, room by room, for the camera test
 * (gebeanStageTickCamera()) and the hit's texture (gebeanStageHitTexture()).
 */
static void shellTake(u8 **filerooms, u32 *filelens, s32 n)
{
	shellFirst = calloc(n + 1, sizeof(*shellFirst));
	shellCount = calloc(n + 1, sizeof(*shellCount));

	for (s32 r = 1; r < n && shellFirst && shellCount; r++) {
		if (filerooms[r]) {
			shellFirst[r] = shellNum;
			fileRoomTrianglesEach(r, filerooms[r], filelens[r], 0, fileTriToShell, NULL);
			shellCount[r] = shellNum - shellFirst[r];
		}
	}

	if (!shellFirst || !shellCount) {
		shellForget();
	} else {
		sysLogPrintf(LOG_NOTE, "gebeanstage: %d of GoldenEye's opaque triangles for the camera test, %d of them unculled",
				shellNum, shellNumTwo);
	}
}

static void shellForget(void)
{
	free(shellTri);
	free(shellTwo);
	free(shellTex);
	shellTex = NULL;
	free(shellFirst);
	free(shellCount);
	shellTri = NULL;
	shellTwo = NULL;
	shellNumTwo = 0;
	shellFirst = shellCount = NULL;
	shellNum = shellCap = 0;
	cullOutside = 0;
}

static s32 rayHitsBox(const f32 *o, const f32 *inv, const f32 *mn, const f32 *mx, f32 best)
{
	f32 t0 = 0.0f, t1 = best;

	for (s32 k = 0; k < 3; k++) {
		f32 a = (mn[k] - o[k]) * inv[k];
		f32 b = (mx[k] - o[k]) * inv[k];

		if (a > b) {
			const f32 t = a;
			a = b;
			b = t;
		}

		t0 = a > t0 ? a : t0;
		t1 = b < t1 ? b : t1;

		if (t0 > t1) {
			return 0;
		}
	}

	return 1;
}

/** The nearest of GoldenEye's triangles along the ray: 0 none, 1 its front, -1 its back. */
static s32 shellRay(const f32 *o, const f32 *d)
{
	f32 inv[3];
	f32 best = 1e9f;
	s32 facing = 0;

	for (s32 k = 0; k < 3; k++) {
		inv[k] = 1.0f / (d[k] != 0.0f ? d[k] : 1e-9f);
	}

	for (s32 r = 1; r < numRooms; r++) {
		if (shellCount[r] == 0
				|| !rayHitsBox(o, inv, g_Rooms[r].bbmin, g_Rooms[r].bbmax, best)) {
			continue;
		}

		for (s32 i = shellFirst[r]; i < shellFirst[r] + shellCount[r]; i++) {
			const f32 *v = shellTri + i * 9;
			f32 e1[3], e2[3], p[3], q[3], s[3], det, u, w, t;

			for (s32 k = 0; k < 3; k++) {
				e1[k] = v[3 + k] - v[k];
				e2[k] = v[6 + k] - v[k];
			}

			p[0] = d[1] * e2[2] - d[2] * e2[1];
			p[1] = d[2] * e2[0] - d[0] * e2[2];
			p[2] = d[0] * e2[1] - d[1] * e2[0];
			det = dot3(e1, p);

			if (det > -1e-6f && det < 1e-6f) {
				continue;
			}

			for (s32 k = 0; k < 3; k++) {
				s[k] = o[k] - v[k];
			}

			u = dot3(s, p) / det;

			if (u < 0.0f || u > 1.0f) {
				continue;
			}

			q[0] = s[1] * e1[2] - s[2] * e1[1];
			q[1] = s[2] * e1[0] - s[0] * e1[2];
			q[2] = s[0] * e1[1] - s[1] * e1[0];
			w = dot3(d, q) / det;

			if (w < 0.0f || u + w > 1.0f) {
				continue;
			}

			t = dot3(e2, q) / det;

			if (t > 1.0f && t < best) {
				best = t;
				// det is the triangle's normal (e1 x e2) against -d, so it is
				// positive where the ray meets the side that faces it - the
				// side G_CULL_BACK keeps
				facing = det > 0.0f || shellTwo[i] ? 1 : -1;
			}
		}
	}

	return facing;
}

void gebeanStageTickCamera(s32 authored)
{
	struct player *pl = g_Vars.currentplayer;
	f32 o[3], look[3], up[3], right[3], ty, tx;
	s32 hits = 0, backs = 0;

	cullOutside = 0;
	shellHits = shellBacks = 0;

	if (!authored || !built || !shellTri || !pl) {
		return;
	}

	o[0] = pl->cam_pos.x;
	o[1] = pl->cam_pos.y;
	o[2] = pl->cam_pos.z;
	look[0] = pl->cam_look.x;
	look[1] = pl->cam_look.y;
	look[2] = pl->cam_look.z;
	up[0] = pl->cam_up.x;
	up[1] = pl->cam_up.y;
	up[2] = pl->cam_up.z;
	// cam_look is a look-at offset and cam_up the world's up, neither of
	// them a unit or square to the other: the basis is built here, and each
	// ray is a unit so that shellRay()'s distances are units of the level
	normalize3(look);
	right[0] = look[1] * up[2] - look[2] * up[1];
	right[1] = look[2] * up[0] - look[0] * up[2];
	right[2] = look[0] * up[1] - look[1] * up[0];

	if (!normalize3(right)) {
		return;
	}

	up[0] = right[1] * look[2] - right[2] * look[1];
	up[1] = right[2] * look[0] - right[0] * look[2];
	up[2] = right[0] * look[1] - right[1] * look[0];

	ty = tanf(viGetFovY() * (3.14159265f / 360.0f));
	tx = ty * viGetAspect();

	for (s32 y = 0; y < SHELL_RAYS_Y; y++) {
		for (s32 x = 0; x < SHELL_RAYS_X; x++) {
			const f32 sx = ((x + 0.5f) / SHELL_RAYS_X * 2.0f - 1.0f) * tx;
			const f32 sy = ((y + 0.5f) / SHELL_RAYS_Y * 2.0f - 1.0f) * ty;
			f32 d[3];
			s32 f;

			for (s32 k = 0; k < 3; k++) {
				d[k] = look[k] + right[k] * sx + up[k] * sy;
			}

			normalize3(d);
			f = shellRay(o, d);

			if (f) {
				hits++;
				backs += f < 0;
			}
		}
	}

	// Two thirds of what the camera sees being the back of GoldenEye's level
	// is outside it; from inside, a back face is only ever a seam
	shellHits = hits;
	shellBacks = backs;
	cullOutside = hits >= 8 && backs * 3 >= hits * 2;

	{
		static s32 logged = -1;
		static s32 wantlog = -1;

		if (wantlog < 0) {
			wantlog = getenv("PD_CULLLOG") != NULL;
		}

		if (wantlog && cullOutside != logged) {
			logged = cullOutside;
			sysLogPrintf(LOG_NOTE, "gebeanstage: frame %d camera outside %d (%d of %d back)",
					g_Vars.lvframenum, cullOutside, backs, hits);
		}
	}
}

s32 gebeanStageCullsBackFaces(void)
{
	return cullOutside;
}

/**
 * The room whose bounding box is nearest p: for the scenery past every room -
 * Frigate's hull and sea, most of its triangles - which searching the grid
 * outwards for took six seconds a level.
 */
static u16 nearestRoomBox(const f32 *p, s32 n)
{
	s32 best = 0;
	f32 bestd = 0;

	for (s32 r = 1; r < n; r++) {
		f32 d = 0;

		for (s32 k = 0; k < 3; k++) {
			f32 e = 0;

			if (p[k] < g_Rooms[r].bbmin[k]) {
				e = g_Rooms[r].bbmin[k] - p[k];
			} else if (p[k] > g_Rooms[r].bbmax[k]) {
				e = p[k] - g_Rooms[r].bbmax[k];
			}

			d += e * e;
		}

		if (best == 0 || d < bestd) {
			best = r;
			bestd = d;
		}
	}

	return (u16)best;
}

/**
 * The row for the level being played, or NULL for a level that is not one of
 * GoldenEye's.
 *
 * A level is named by the key GoldenEye itself gives it, which is the name the
 * conversion writes its file under - files/bgdata/bg_gx<key>.seg, the same
 * file for the arena and for the solo mission on it. Nothing about the
 * geometry is asked: the conversion is GoldenEye's own data, which is what
 * Bean's HD mesh was built on, so the pairing holds however the conversion
 * changes. (It was the room count and a hash of the room positions until
 * 2026-09-22, and Streets had silently lost its HD when its rooms moved.)
 */
static const struct stagerow *levelRow(void)
{
	const char *name;
	const char *slash;

	if (g_StageIndex < 0 || g_StageIndex >= (s32)ARRAYCOUNT(g_Stages)) {
		return NULL;
	}

	name = romdataFileGetName(g_Stages[g_StageIndex].bgfileid);

	if (!name) {
		return NULL;
	}

	slash = strrchr(name, '/');

	if (slash) {
		name = slash + 1;
	}

	if (strncmp(name, "bg_gx", 5) != 0) {
		return NULL;
	}

	name += 5;

	for (s32 i = 0; i < ARRAYCOUNT(stageRows); i++) {
		const size_t len = strlen(stageRows[i].key);

		if (strncmp(name, stageRows[i].key, len) == 0 && strcmp(name + len, ".seg") == 0) {
			return &stageRows[i];
		}
	}

	sysLogPrintf(LOG_NOTE, "gebeanstage: no GoldenEye XBLA level is paired with GoldenEye's %s", name);

	return NULL;
}

/* -------------------------------------------------------------------------
 * Writing a room
 * ------------------------------------------------------------------------- */

struct gdlbuf {
	u8 *data;
	u32 len;
	u32 cap;
};

static void emit(struct gdlbuf *b, u32 w0, u32 w1)
{
	if (b->len + 8 > b->cap) {
		u32 cap = b->cap ? b->cap * 2 : 4096;
		u8 *d = realloc(b->data, cap);

		if (!d) {
			return;
		}

		b->data = d;
		b->cap = cap;
	}

	put32(b->data + b->len, w0);
	put32(b->data + b->len + 4, w1);
	b->len += 8;
}

struct rvtx {
	s16 x, y, z;
	u8 colour;
	s16 s, t;
};

struct leaf {
	struct gdlbuf gdl;
	struct rvtx *vtx;
	s32 numvtx;
	s32 capvtx;
};

struct batch {
	struct rvtx v[BATCHVERTS];
	s32 numv;
	u8 tris[64][3];
	s32 numtris;
	f32 shiftu, shiftv;
	s32 shifted;
};

static void leafAddVtx(struct leaf *l, const struct rvtx *v)
{
	if (l->numvtx >= l->capvtx) {
		s32 cap = l->capvtx ? l->capvtx * 2 : 1024;
		struct rvtx *n = realloc(l->vtx, sizeof(*n) * cap);

		if (!n) {
			return;
		}

		l->vtx = n;
		l->capvtx = cap;
	}

	l->vtx[l->numvtx++] = *v;
}

static void batchFlush(struct leaf *l, struct batch *b)
{
	const u32 offset = l->numvtx * VTXSIZE;

	if (b->numv == 0) {
		return;
	}

	for (s32 i = 0; i < b->numv; i++) {
		leafAddVtx(l, &b->v[i]);
	}

	emit(&l->gdl, (G_VTX << 24) | ((b->numv - 1) << 20) | (b->numv * VTXSIZE), 0x0e000000 | offset);

	for (s32 i = 0; i < b->numtris; i += 4) {
		u32 w0 = (u32)G_TRI4 << 24;
		u32 w1 = 0;

		for (s32 k = 0; k < 4 && i + k < b->numtris; k++) {
			w1 |= (u32)b->tris[i + k][0] << (k * 8);
			w1 |= (u32)b->tris[i + k][1] << (k * 8 + 4);
			w0 |= (u32)b->tris[i + k][2] << (k * 4);
		}

		emit(&l->gdl, w0, w1);
	}

	b->numv = 0;
	b->numtris = 0;
	b->shifted = 0;
}

static s32 batchFind(struct batch *b, const struct rvtx *v)
{
	for (s32 i = 0; i < b->numv; i++) {
		if (memcmp(&b->v[i], v, sizeof(*v)) == 0) {
			return i;
		}
	}

	return -1;
}

static s32 clampS16(f32 f, s16 *out)
{
	const f32 r = roundf(f);

	if (r < -32768.0f || r > 32767.0f) {
		return 0;
	}

	*out = (s16)r;

	return 1;
}

/** Whether a triangle's corners are all within a Vtx's reach of room r's position. */
static s32 triFitsRoom(const struct stri *tri, s32 r)
{
	const f32 roompos[3] = { g_BgRooms[r].pos.x, g_BgRooms[r].pos.y, g_BgRooms[r].pos.z };
	s16 v;

	for (s32 k = 0; k < 3; k++) {
		for (s32 j = 0; j < 3; j++) {
			if (!clampS16(tri->pos[k][j] - roompos[j], &v)) {
				return 0;
			}
		}
	}

	return 1;
}

static u8 paletteIndex(const u32 *palette, s32 num, u32 argb);

/*
 * paletteIndex() for every corner a leaf writes, remembered for the leaf: a
 * room's triangles share their colours, and each ask walks the whole palette.
 * leafPaletteBegin() forgets what the last leaf's palette answered.
 */
#define LEAFPAL_BITS 10

static u32 leafPalKey[1 << LEAFPAL_BITS];
static u32 leafPalGen[1 << LEAFPAL_BITS];
static u8 leafPalVal[1 << LEAFPAL_BITS];
static u32 leafPalNow;

static void leafPaletteBegin(void)
{
	if (++leafPalNow == 0) {
		memset(leafPalGen, 0, sizeof(leafPalGen));
		leafPalNow = 1;
	}
}

static u8 leafPaletteIndex(const u32 *palette, s32 num, u32 argb)
{
	const u32 h = (argb * 2654435761u) >> (32 - LEAFPAL_BITS);

	if (leafPalGen[h] != leafPalNow || leafPalKey[h] != argb) {
		leafPalGen[h] = leafPalNow;
		leafPalKey[h] = argb;
		leafPalVal[h] = paletteIndex(palette, num, argb);
	}

	return leafPalVal[h];
}

static u8 paletteIndex(const u32 *palette, s32 num, u32 argb)
{
	s32 best = 0;
	s32 bestd = 0x7fffffff;

	for (s32 i = 0; i < num; i++) {
		s32 d = 0;

		for (s32 k = 0; k < 32; k += 8) {
			const s32 a = (palette[i] >> k) & 0xff;
			const s32 c = (argb >> k) & 0xff;
			d += (a - c) * (a - c);
		}

		if (d < bestd) {
			best = i;
			bestd = d;
		}
	}

	return (u8)best;
}

/**
 * A room's palette: its vertices' colours brought down to what one G_COL can
 * index (64) - the most used ones as seeds, every colour then going to its
 * nearest and each seed moving to the weighted mean of its own. GoldenEye's
 * shading takes a few dozen colours a room; Frigate's hull, thousands.
 */
static s32 buildPalette(const struct stri *tris, const s32 *list, s32 num, u32 *palette)
{
	enum { HASHBITS = 14 };
	u32 *keys = calloc(1 << HASHBITS, sizeof(u32));
	s32 *weight = calloc(1 << HASHBITS, sizeof(s32));
	u32 *cols = malloc(sizeof(u32) << HASHBITS);
	s32 *counts = malloc(sizeof(s32) << HASHBITS);
	s32 *order = malloc(sizeof(s32) << HASHBITS);
	s32 numcols = 0;
	s32 numpal;

	if (!keys || !weight || !cols || !counts || !order) {
		free(keys);
		free(weight);
		free(cols);
		free(counts);
		free(order);
		palette[0] = 0xffffffff;
		return 1;
	}

	for (s32 i = 0; i < num; i++) {
		for (s32 k = 0; k < 3; k++) {
			// Keyed one above the colour, so that 0 is an empty slot
			const u32 c = tris[list[i]].argb[k];
			u32 h = (c * 2654435761u) >> (32 - HASHBITS);

			while (weight[h] && keys[h] != c) {
				h = (h + 1) & ((1 << HASHBITS) - 1);
			}

			if (!weight[h]) {
				if (numcols >= (1 << HASHBITS) - 1) {
					continue;
				}

				keys[h] = c;
				cols[numcols] = c;
				numcols++;
			}

			// The reflection's few fades (markUnderSea()) are always
			// seeds: a handful of vertices among the hull's thousands, they
			// went to the nearest of the hull's own colours, at full alpha
			weight[h] += tris[list[i]].undersea ? 1 << 20 : 1;
		}
	}

	for (s32 i = 0; i < numcols; i++) {
		u32 h = (cols[i] * 2654435761u) >> (32 - HASHBITS);

		while (keys[h] != cols[i]) {
			h = (h + 1) & ((1 << HASHBITS) - 1);
		}

		counts[i] = weight[h];
		order[i] = i;
	}

	free(keys);
	free(weight);

	// Heaviest first (a selection is plenty for 64 of them)
	numpal = numcols < MAXPALETTE ? numcols : MAXPALETTE;

	for (s32 i = 0; i < numpal; i++) {
		s32 best = i;

		for (s32 j = i + 1; j < numcols; j++) {
			if (counts[order[j]] > counts[order[best]]) {
				best = j;
			}
		}

		{
			const s32 tmp = order[i];
			order[i] = order[best];
			order[best] = tmp;
		}

		palette[i] = cols[order[i]];
	}

	if (numcols > MAXPALETTE) {
		f64 sum[MAXPALETTE][4];
		f64 total[MAXPALETTE];

		memset(sum, 0, sizeof(sum));
		memset(total, 0, sizeof(total));

		for (s32 i = 0; i < numcols; i++) {
			const s32 p = paletteIndex(palette, numpal, cols[i]);

			for (s32 k = 0; k < 4; k++) {
				sum[p][k] += ((cols[i] >> (8 * k)) & 0xff) * (f64)counts[i];
			}

			total[p] += counts[i];
		}

		for (s32 p = 0; p < numpal; p++) {
			if (total[p] > 0) {
				u32 c = 0;

				for (s32 k = 0; k < 4; k++) {
					c |= (u32)(sum[p][k] / total[p] + 0.5) << (8 * k);
				}

				palette[p] = c;
			}
		}
	}

	free(cols);
	free(counts);
	free(order);

	return numpal ? numpal : 1;
}

/** The middle of a triangle's range of u (axis 0) or v (axis 1). */
static f32 triUvMiddle(const struct stri *t, s32 axis)
{
	f32 lo = t->uv[0][axis], hi = lo;

	for (s32 k = 1; k < 3; k++) {
		lo = t->uv[k][axis] < lo ? t->uv[k][axis] : lo;
		hi = t->uv[k][axis] > hi ? t->uv[k][axis] : hi;
	}

	return (lo + hi) * 0.5f;
}

static s32 texIsXlu(s32 tex)
{
	return tex >= 0 && texSoft[tex];
}

static s32 texHasAlpha(s32 tex)
{
	return tex >= 0 && texAlpha[tex];
}

/**
 * Whether the opaque leaf draws the triangle's front alone at first: one face
 * of a two-faced sheet (markBacked()) or one that fights another over part of
 * its face (markFights(), whose back is drawn after, pushed away).
 */
static s32 triCulled(const struct stri *t)
{
	return t->backed || t->fights || t->overlap;
}

// Below this a vertex alpha is a fade and not a rounding, as xblamesh.c's
// XBLAMESH_FADE_ALPHA reads a mesh's
#define FADE_ALPHA 0xf0

/**
 * A triangle of a picture with alpha that the release fades by its vertices'
 * alpha is translucent, whatever its picture's texels are: as a cut-out its
 * alpha would be the threshold's, and Dam's server room lamp threw a cone of
 * solid white, its picture being opaque at the lamp (F3 20260925-235806).
 *
 * So is one the release draws in its blended pass (source alpha over the
 * rest) whatever its picture: Archives' light shafts are an opaque 32x32 grey
 * (DXT1) on vertices of alpha 0 to 126, and in the opaque leaf they stood as
 * solid white slabs across the rooms (F3 20260926-101221). Outside that pass
 * an opaque picture's vertex alpha is not a fade - the stride 32 vertex's
 * blend word goes into it - and is left alone.
 */
static s32 triFades(const struct stri *t)
{
	return (texHasAlpha(t->tex) || t->blend)
		&& ((t->argb[0] >> 24) < FADE_ALPHA || (t->argb[1] >> 24) < FADE_ALPHA || (t->argb[2] >> 24) < FADE_ALPHA);
}

static const struct stri *sortTris;
static s32 sortCutoutsLast;

static int compareTex(const void *a, const void *b)
{
	const struct stri *ta = &sortTris[*(const s32 *)a];
	const struct stri *tb = &sortTris[*(const s32 *)b];

	// An opaque leaf's cut-outs after its solid pictures, which are drawn
	// culled or not by the room (writeLeaf()); a decal's base is never a
	// cut-out under a solid picture (markDecals())
	if (sortCutoutsLast && texHasAlpha(ta->tex) != texHasAlpha(tb->tex)) {
		return texHasAlpha(ta->tex) - texHasAlpha(tb->tex);
	}

	// The faces of two-faced sheets after the rest of their kind, which
	// is where writeLeaf() turns culling on for them
	if (triCulled(ta) != triCulled(tb)) {
		// solid pictures: the sheets last; cut-outs: the sheets first,
		// before the cut-outs that turn culling off
		return texHasAlpha(ta->tex) ? triCulled(tb) - triCulled(ta) : triCulled(ta) - triCulled(tb);
	}

	// The reflection's culled faces after the rest
	if (ta->undersea != tb->undersea) {
		return ta->undersea - tb->undersea;
	}

	// Decals after what they lie on
	if (ta->decal != tb->decal) {
		return ta->decal - tb->decal;
	}

	if (ta->tex != tb->tex) {
		return ta->tex - tb->tex;
	}

	return ta->nofog != tb->nofog ? ta->nofog - tb->nofog : *(const s32 *)a - *(const s32 *)b;
}

/**
 * One layer's display list - the opaque or the translucent - in the render
 * modes and combiners GoldenEye X's own rooms use, so bg.c's fog swaps
 * (gfxReplaceGbiCommandsRecursively()) find them.
 */
static f32 triNormal(const struct stri *t, f32 *n);

static s32 writeLeaf(struct leaf *l, const struct stri *tris, s32 *list, s32 num, s32 xlu,
		const f32 *roompos, const u32 *palette, s32 numpal, s32 *dropped)
{
	struct batch b;
	s32 curtex = -2;
	s32 curdecal = -1;
	s32 curalpha = -1;
	s32 curnofog = 0;
	s32 curbacked = -1;
	s32 curundersea = 0;
	// -1 while the solid pictures take the room's culling
	// (bgRenderRoomOpaque()), then 1 once a two-faced sheet has turned it on
	// or 0 once the cut-outs, sorted last, have turned it off
	s32 curcull = -1;
	s32 numfights = 0;

	if (num == 0) {
		return 0;
	}

	leafPaletteBegin();

	for (s32 i = 0; i < num && !xlu; i++) {
		numfights += tris[list[i]].fights;
	}

	sortTris = tris;
	sortCutoutsLast = !xlu;
	qsort(list, num, sizeof(*list), compareTex);

	memset(&b, 0, sizeof(b));

	emit(&l->gdl, 0xe7000000, 0x00000000);
	emit(&l->gdl, 0xba001402, 0x00100000);
	emit(&l->gdl, 0xb900031d, xlu ? 0x0c184dd8 : 0x0c182078);
	emit(&l->gdl, 0xba001001, 0x00010000);
	emit(&l->gdl, 0xba001102, 0x00000000);
	emit(&l->gdl, 0xba000c02, 0x00002000);
	// Both sides are drawn: 4J built decks, stair treads and roofs as single
	// planes. The opaque leaf's solid pictures are the exception - they come
	// first and take whatever bgRenderRoomOpaque() set, which is culled when
	// one of GoldenEye's own cameras stands outside the level
	// (gebeanStageTickCamera(); Bean's winding agrees with its vertex
	// normals on every level, all but a few hundred of 600,000 triangles)
	if (xlu) {
		emit(&l->gdl, 0xb6000000, 0x00002000);
	}
	// The alpha combiner reads the environment colour, which is whatever the
	// last list left it as unless it is set here, as the file's lists set it
	emit(&l->gdl, 0xfb000000, 0x000000ff);
	emit(&l->gdl, (G_COL << 24) | (((numpal - 1) << 2) << 16) | (numpal * COLSIZE), 0x0d000000);

	// Side 0 is every triangle, side 1 the backs of those that fight
	// another face back to back (markFights()): culled to their backs and
	// pushed away from the eye, so a face turned to the camera is always
	// drawn over the back of the one it shares its plane with, and a back
	// with no face over it is still drawn
	for (s32 side = 0; side < (numfights ? 2 : 1); side++) {
	if (side == 1) {
		batchFlush(l, &b);
		memset(&b, 0, sizeof(b));
		curtex = -2;
		curdecal = -1;
		curalpha = -1;
		curbacked = -1;
		curcull = 0;
		emit(&l->gdl, 0xb6000000, G_CULL_BACK);
		emit(&l->gdl, 0xb7000000, G_CULL_FRONT);
		emit(&l->gdl, G_SETDEPTHBIAS_EXT << 24, FIGHT_DEPTH_BIAS);
	}

	for (s32 i = 0; i < num; i++) {
		const struct stri *t = &tris[list[i]];
		struct rvtx rv[3];
		f32 lift[3];
		u8 idx[3];
		s32 ok = 1;
		s32 need = 0;

		if (side == 1 && !t->fights) {
			continue;
		}

		// Compared whole in batchFind(), padding and all
		memset(rv, 0, sizeof(rv));

		if (t->tex != curtex || t->decal != curdecal || t->nofog != curnofog || triCulled(t) != curbacked
				|| t->undersea != curundersea) {
			const s32 alpha = xlu || texHasAlpha(t->tex);

			batchFlush(l, &b);

			if (alpha != curalpha || t->decal != curdecal || t->nofog != curnofog) {
				if (alpha != curalpha) {
					emit(&l->gdl, 0xfc26a004, alpha ? 0x1f1093ff : 0x1ffc93fc);
				}

				// A cut-out picture in the opaque leaf is drawn as a texture
				// edge (CVG_X_ALPHA), which the renderer discards under a fifth
				// alpha. Without it the clear texels of Jungle's leaves wrote
				// depth, and a room drawn after them showed the sky colour in
				// the shape of the leaf. A decal takes the file's decal modes
				// (ZMODE_DEC), which fog swaps know too; the translucent
				// leaf's mode is a decal one already. A solid decal writes
				// depth as well: the file's decal mode does not, and where a
				// decal overhangs its base, or its base is in a room drawn
				// after it, whatever came next painted over it (Bunker's
				// wall panel under the rock of the room behind it)
				//
				// Where GoldenEye draws the surface without fog (Caverns'
				// water), cycle 1 is the plain pass (all zeros) instead of
				// G_RM_PASS, which the fog swap would make fog of
				if (!xlu || t->nofog != curnofog) {
					u32 mode = xlu ? 0x0c184dd8 : t->decal
							? (alpha ? G_RM_AA_ZB_XLU_DECAL | G_RM_AA_ZB_XLU_DECAL2 : G_RM_AA_ZB_OPA_DECAL | G_RM_AA_ZB_OPA_DECAL2 | Z_UPD)
							: (alpha ? 0x0c183078 : 0x0c182078);

					if (t->nofog) {
						mode &= ~0xcccc0000;
					}

					emit(&l->gdl, 0xb900031d, mode);
				}

				curalpha = alpha;
				curdecal = t->decal;
				curnofog = t->nofog;
			}

			// A two-faced sheet culls, whatever else in the leaf does: its
			// faces lie in one plane back to back and both sides would be
			// drawn, which is Surface's platform decks fighting their own
			// undersides (markBacked())
			if (side == 1) {
				// culled to the back already, for the whole side
			} else if (xlu) {
				// The reflection under a sea (markUnderSea()) shows its near
				// side alone, as the hull it mirrors would
				if (t->undersea != curundersea) {
					emit(&l->gdl, t->undersea ? 0xb7000000 : 0xb6000000, 0x00002000);
				}
			} else if (!xlu && triCulled(t) && curcull != 1) {
				curcull = 1;
				emit(&l->gdl, 0xb7000000, 0x00002000);
			} else if (!xlu && !triCulled(t) && texHasAlpha(t->tex) && curcull != 0) {
				curcull = 0;
				emit(&l->gdl, 0xb6000000, 0x00002000);
			}

			curbacked = triCulled(t);
			curundersea = t->undersea;

			emit(&l->gdl, alpha ? 0xbb002801 : 0xbb003001, 0xffffffff);
			// Bits 20-21 are t's mode (xblaStageWriteTexture()), 1 the clamp
			emit(&l->gdl, 0xc0080002 | (t->tex >= 0 && texClampV[t->tex] ? 0x00100000 : 0),
					t->tex >= 0 ? GEBEANSTAGE_TEXBASE + t->tex : GEBEANSTAGE_TEXNONE);
			curtex = t->tex;
		}

		if (!b.shifted) {
			b.shiftu = floorf(t->uv[0][0]);
			b.shiftv = t->tex >= 0 && texClampV[t->tex] ? texClampShift[t->tex] : floorf(t->uv[0][1]);
			b.shifted = 1;
		}

		// A decal is lifted off its base before its corners are rounded to
		// whole units: on a wall square to no axis the rounding put one end
		// of Dam's CTON up to a unit behind the wall it is painted on, and
		// the decal offset lost it there by distance (F3 20260926-205834,
		// -213530: the N came and went as Bond walked)
		lift[0] = lift[1] = lift[2] = 0.0f;

		if (t->lift || t->sink) {
			f32 n[3];
			const f32 by = t->lift ? DECAL_LIFT : -t->sink * DECAL_LIFT;

			if (triNormal(t, n) > 0.0f) {
				for (s32 j = 0; j < 3; j++) {
					lift[j] = n[j] * by;
				}
			}
		}

		for (s32 pass = 0; pass < 2; pass++) {
			ok = 1;

			for (s32 k = 0; k < 3 && ok; k++) {
				ok = clampS16(t->pos[k][0] + lift[0] - roompos[0], &rv[k].x)
					&& clampS16(t->pos[k][1] + lift[1] - roompos[1], &rv[k].y)
					&& clampS16(t->pos[k][2] + lift[2] - roompos[2], &rv[k].z)
					&& clampS16((t->uv[k][0] - b.shiftu) * XBLATEX_TILE_SCALE, &rv[k].s)
					// Turned over: the picture was decoded bottom row first for the
					// renderer (beanDecodeTexture()), which the meshes undo by
					// flipping v and a room has to as well - the Temple's
					// carvings stood on their heads
					&& clampS16((b.shiftv - t->uv[k][1]) * XBLATEX_TILE_SCALE, &rv[k].t);
				rv[k].colour = leafPaletteIndex(palette, numpal, t->argb[k]) << 2;
			}

			if (ok || pass == 1) {
				break;
			}

			// Outside this batch's texture window: start one centred on this
			// triangle. Centred on its UVs' middle and not on its first
			// corner, or a strip whose picture repeats more than 32 times one
			// way from that corner is dropped though its whole range fits in
			// a Vtx: Facility's catwalk decks over the tank room (v -5.8 to
			// 38.9) were missing, and the guards on them were seen from below
			// through the floor (F3 20260925-025537)
			batchFlush(l, &b);
			b.shiftu = floorf(triUvMiddle(t, 0));
			b.shiftv = t->tex >= 0 && texClampV[t->tex] ? texClampShift[t->tex] : floorf(triUvMiddle(t, 1));
			b.shifted = 1;
		}

		if (!ok) {
			if (*dropped < 8) {
				f32 umin = t->uv[0][0], umax = umin, vmin = t->uv[0][1], vmax = vmin;

				for (s32 k = 1; k < 3; k++) {
					umin = t->uv[k][0] < umin ? t->uv[k][0] : umin;
					umax = t->uv[k][0] > umax ? t->uv[k][0] : umax;
					vmin = t->uv[k][1] < vmin ? t->uv[k][1] : vmin;
					vmax = t->uv[k][1] > vmax ? t->uv[k][1] : vmax;
				}

				sysLogPrintf(LOG_NOTE, "gebeanstage: dropped tri tex %d at (%.0f %.0f %.0f) room pos (%.0f %.0f %.0f) u %.1f..%.1f v %.1f..%.1f",
						t->tex, (t->pos[0][0] + t->pos[1][0] + t->pos[2][0]) / 3.0f, (t->pos[0][1] + t->pos[1][1] + t->pos[2][1]) / 3.0f,
						(t->pos[0][2] + t->pos[1][2] + t->pos[2][2]) / 3.0f, roompos[0], roompos[1], roompos[2], umin, umax, vmin, vmax);
			}

			(*dropped)++;
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			if (batchFind(&b, &rv[k]) < 0) {
				need++;
			}
		}

		if (b.numv + need > BATCHVERTS || b.numtris >= 64) {
			const f32 su = b.shiftu;
			const f32 sv = b.shiftv;

			batchFlush(l, &b);
			b.shiftu = su;
			b.shiftv = sv;
			b.shifted = 1;
		}

		for (s32 k = 0; k < 3; k++) {
			s32 at = batchFind(&b, &rv[k]);

			if (at < 0) {
				at = b.numv++;
				b.v[at] = rv[k];
			}

			idx[k] = (u8)at;
		}

		b.tris[b.numtris][0] = idx[0];
		b.tris[b.numtris][1] = idx[1];
		b.tris[b.numtris][2] = idx[2];
		b.numtris++;
	}
	}

	batchFlush(l, &b);

	if (numfights) {
		emit(&l->gdl, 0xb6000000, G_CULL_FRONT);
		emit(&l->gdl, G_SETDEPTHBIAS_EXT << 24, 0);
	}

	// Culling back off for what follows in the room, as it was before the
	// sheets unless a camera outside the level had it on (the next room
	// sets its own either way)
	if (curcull == 1 || curundersea) {
		emit(&l->gdl, 0xb6000000, 0x00002000);
	}

	emit(&l->gdl, 0xb8000000, 0x00000000);

	return 1;
}

/**
 * GoldenEye's water pictures: the grey (1508) and the blue (1509), which it
 * also draws its sea with, and Dam and Complex's (1511)
 */
static s32 texIsGeWater(u32 texnum)
{
	return texnum == 0x5e4 || texnum == 0x5e5 || texnum == 0x5e7;
}

/**
 * A leaf of the file's room kept under the HD level: its display list, and
 * the vertices and colours it loads, all in the file's own form
 */
struct keptleaf {
	const u8 *gdl;
	u32 gdllen;
	const u8 *vtx;
	u32 numvtx;
	const u8 *col;
	u32 numcol;
};

#define MAXKEPTLEAVES 8

/**
 * The translucent leaves of room r of the file that draw nothing but
 * GoldenEye's water, which Bean's mesh has nowhere: Egyptian's pool in the
 * pillared hall is a plain stone basin in the release, and GoldenEye's is a
 * sheet of its blue water at half alpha laid over it (room 3's translucent
 * leaf; F3 20260927-000453, "lighting/pool"). A level whose Bean mesh draws
 * water (Dam's reservoir) keeps none, or it would draw both.
 */
static s32 fileWaterLeaves(s32 r, const u8 *raw, u32 len, struct keptleaf *out, s32 max)
{
	const u32 base = g_BgRooms[r].unk00;
	u32 stack[64];
	s32 depth = 0;
	s32 num = 0;

	if (levelHasWater || len < GFXHEADER || !be32(raw + 12)) {
		return 0;
	}

	stack[depth++] = be32(raw + 12);

	while (depth > 0 && num < max) {
		u32 b = stack[--depth];

		for (s32 guard = 0; b && guard < 256 && num < max; guard++) {
			const u32 o = b - base;

			if (o + ROOMBLOCKSIZE > len) {
				break;
			}

			if (raw[o] == 1) {
				if (depth < ARRAYCOUNT(stack) && be32(raw + o + 8)) {
					stack[depth++] = be32(raw + o + 8);
				}
			} else if (raw[o] == 0) {
				const u32 gdl = be32(raw + o + 8) - base;
				const u32 vtx = be32(raw + o + 12) - base;
				const u32 col = be32(raw + o + 16) - base;
				u32 vtxend = 0, colend = 0, c;
				s32 water = 0, other = 0;

				for (c = gdl; c + 8 <= len; c += 8) {
					const u8 op = raw[c];
					const u32 w0 = be32(raw + c), w1 = be32(raw + c + 4);

					if (op == 0xc0) {
						if (texIsGeWater(w1 & 0xfff)) {
							water = 1;
						} else {
							other = 1;
						}
					} else if (op == G_VTX) {
						const u32 end = (w1 & 0xffffff) + (((w0 >> 20) & 0xf) + 1) * VTXSIZE;

						vtxend = end > vtxend ? end : vtxend;
					} else if (op == G_COL) {
						const u32 end = (w1 & 0xffffff) + ((((w0 >> 16) & 0xff) >> 2) + 1) * COLSIZE;

						colend = end > colend ? end : colend;
					} else if (op == (u8)G_DL) {
						other = 1;
					} else if (op == (u8)G_ENDDL) {
						c += 8;
						break;
					}
				}

				if (water && !other && gdl < len && vtxend && colend
						&& vtx + vtxend <= len && col + colend <= len) {
					out[num].gdl = raw + gdl;
					out[num].gdllen = c - gdl;
					out[num].vtx = raw + vtx;
					out[num].numvtx = vtxend / VTXSIZE;
					out[num].col = raw + col;
					out[num].numcol = colend / COLSIZE;
					num++;
				}
			}

			b = be32(raw + o + 4);
		}
	}

	return num;
}

/**
 * Room r in the room format, big-endian as the file stores it, its pointers
 * relative to the room's own entry in the room table (as the release's rooms
 * are served, xblastage.c): the header, an opaque and a translucent leaf, the
 * vertices of both, the shared palette, then the two display lists.
 */
static u8 *writeRoom(s32 r, const struct stri *tris, s32 *list, s32 num, const u8 *fileroom, u32 filelen, u32 *outLen, s32 *dropped)
{
	struct keptleaf keep[MAXKEPTLEAVES];
	const s32 numkeep = fileWaterLeaves(r, fileroom, filelen, keep, ARRAYCOUNT(keep));
	u32 keepvtx = 0, keepcol = 0, keepgdl = 0;
	const u32 base = g_BgRooms[r].unk00;
	const f32 roompos[3] = { g_BgRooms[r].pos.x, g_BgRooms[r].pos.y, g_BgRooms[r].pos.z };
	struct leaf opa, xlu;
	u32 palette[MAXPALETTE];
	s32 numpal;
	s32 *opalist = malloc(sizeof(s32) * (num + 1));
	s32 *xlulist = malloc(sizeof(s32) * (num + 1));
	s32 numopa = 0, numxlu = 0;
	s32 numblocks;
	u32 at, vtxat, colat, opagdlat, xlugdlat, total;
	u8 *out;

	*outLen = 0;

	if (!opalist || !xlulist) {
		free(opalist);
		free(xlulist);
		return NULL;
	}

	for (s32 i = 0; i < num; i++) {
		if (texIsXlu(tris[list[i]].tex) || triFades(&tris[list[i]])) {
			xlulist[numxlu++] = list[i];
		} else {
			opalist[numopa++] = list[i];
		}
	}

	numpal = buildPalette(tris, list, num, palette);

	memset(&opa, 0, sizeof(opa));
	memset(&xlu, 0, sizeof(xlu));
	writeLeaf(&opa, tris, opalist, numopa, 0, roompos, palette, numpal, dropped);
	writeLeaf(&xlu, tris, xlulist, numxlu, 1, roompos, palette, numpal, dropped);

	free(opalist);
	free(xlulist);

	numblocks = (opa.gdl.len ? 1 : 0) + (xlu.gdl.len ? 1 : 0);

	for (s32 k = 0; k < numkeep; k++) {
		keepvtx += keep[k].numvtx;
		keepcol += keep[k].numcol;
		keepgdl += keep[k].gdllen;
	}

	// The converter (convertRoomGfxData()) starts the host's vertices and
	// colours each on an eight byte boundary of their own but places the
	// first display list by the file's distance from the colours, so that
	// distance must come out whole: the colours start on a boundary here too,
	// which an even number of vertices after an aligned start gives. The
	// padding is short of a block, so the block walk does not read it.
	if ((opa.numvtx + xlu.numvtx + keepvtx) & 1) {
		struct rvtx pad;

		memset(&pad, 0, sizeof(pad));
		leafAddVtx(xlu.gdl.len ? &xlu : &opa, &pad);
	}

	vtxat = ALIGN8(GFXHEADER + ROOMBLOCKSIZE * (numblocks + numkeep));
	colat = vtxat + (opa.numvtx + xlu.numvtx + keepvtx) * VTXSIZE;
	opagdlat = ALIGN8(colat + (numpal + keepcol) * COLSIZE);
	xlugdlat = opagdlat + opa.gdl.len;
	total = ALIGN8(xlugdlat + xlu.gdl.len + keepgdl);

	out = calloc(1, total + 16);

	if (!out || numblocks == 0) {
		free(out);
		free(opa.gdl.data);
		free(xlu.gdl.data);
		free(opa.vtx);
		free(xlu.vtx);
		return NULL;
	}

	put32(out + 0x00, base + vtxat);
	put32(out + 0x04, base + colat);
	put32(out + 0x08, opa.gdl.len ? base + GFXHEADER : 0);
	put32(out + 0x0c, xlu.gdl.len || numkeep ? base + GFXHEADER + (opa.gdl.len ? ROOMBLOCKSIZE : 0) : 0);
	// The lights are the file's room's: the level's light table is its
	memcpy(out + 0x10, fileroom + 0x10, 4);
	put16(out + 0x14, (u16)(opa.numvtx + xlu.numvtx + keepvtx));
	put16(out + 0x16, (u16)(numpal + keepcol));

	at = GFXHEADER;

	if (opa.gdl.len) {
		out[at] = 0; // leaf
		put32(out + at + 4, 0);
		put32(out + at + 8, base + opagdlat);
		put32(out + at + 12, base + vtxat);
		put32(out + at + 16, base + colat);
		at += ROOMBLOCKSIZE;
	}

	if (xlu.gdl.len) {
		out[at] = 0;
		put32(out + at + 4, numkeep ? base + at + ROOMBLOCKSIZE : 0);
		put32(out + at + 8, base + xlugdlat);
		put32(out + at + 12, base + vtxat + opa.numvtx * VTXSIZE);
		put32(out + at + 16, base + colat);
		at += ROOMBLOCKSIZE;
	}

	// GoldenEye's water, after the HD leaf: the file's own lists, vertices
	// and colours, each leaf loading from where its own were put
	{
		u32 vat = vtxat + (opa.numvtx + xlu.numvtx) * VTXSIZE;
		u32 cat = colat + numpal * COLSIZE;
		u32 gat = xlugdlat + xlu.gdl.len;

		for (s32 k = 0; k < numkeep; k++) {
			out[at] = 0;
			put32(out + at + 4, k + 1 < numkeep ? base + at + ROOMBLOCKSIZE : 0);
			put32(out + at + 8, base + gat);
			put32(out + at + 12, base + vat);
			put32(out + at + 16, base + cat);
			at += ROOMBLOCKSIZE;

			memcpy(out + vat, keep[k].vtx, keep[k].numvtx * VTXSIZE);
			memcpy(out + cat, keep[k].col, keep[k].numcol * COLSIZE);
			memcpy(out + gat, keep[k].gdl, keep[k].gdllen);
			vat += keep[k].numvtx * VTXSIZE;
			cat += keep[k].numcol * COLSIZE;
			gat += keep[k].gdllen;
		}
	}

	if (numkeep && roomWaterKept && r < numRooms) {
		roomWaterKept[r] = 1 + (xlu.gdl.len ? 1 : 0);
		numWaterKept += numkeep;
	}

	at = vtxat;

	for (s32 pass = 0; pass < 2; pass++) {
		const struct leaf *l = pass ? &xlu : &opa;

		for (s32 i = 0; i < l->numvtx; i++, at += VTXSIZE) {
			put16(out + at, (u16)l->vtx[i].x);
			put16(out + at + 2, (u16)l->vtx[i].y);
			put16(out + at + 4, (u16)l->vtx[i].z);
			out[at + 6] = 0;
			out[at + 7] = l->vtx[i].colour;
			put16(out + at + 8, (u16)l->vtx[i].s);
			put16(out + at + 10, (u16)l->vtx[i].t);
		}
	}

	for (s32 i = 0; i < numpal; i++) {
		// ARGB to the RGBA a Col holds
		put32(out + colat + i * COLSIZE, (palette[i] << 8) | (palette[i] >> 24));
	}

	memcpy(out + opagdlat, opa.gdl.data, opa.gdl.len);
	memcpy(out + xlugdlat, xlu.gdl.data, xlu.gdl.len);

	free(opa.gdl.data);
	free(xlu.gdl.data);
	free(opa.vtx);
	free(xlu.vtx);

	*outLen = total;

	return out;
}

/* -------------------------------------------------------------------------
 * Building the level
 * ------------------------------------------------------------------------- */

struct collect {
	struct stri *tris;
	s32 num;
	s32 cap;
	f32 scale;
	const f32 *offset;
};

static void collectTri(void *arg, s32 tex, const struct gebeanlevelvtx *v)
{
	struct collect *c = arg;
	struct stri *t;

	if (c->num >= c->cap) {
		s32 cap = c->cap ? c->cap * 2 : 65536;
		struct stri *n = realloc(c->tris, sizeof(*n) * cap);

		if (!n) {
			return;
		}

		c->tris = n;
		c->cap = cap;
	}

	t = &c->tris[c->num++];

	for (s32 k = 0; k < 3; k++) {
		for (s32 j = 0; j < 3; j++) {
			t->pos[k][j] = v[k].pos[j] * c->scale - c->offset[j];
		}

		t->uv[k][0] = v[k].uv[0];
		t->uv[k][1] = v[k].uv[1];
		t->argb[k] = v[k].argb;
	}

	t->tex = (s16)tex;
	t->blend = v[0].blend;
	t->room = 0;
	t->decal = 0;
	t->lift = 0;
	t->sink = 0;
	t->decalbase = -1;
	t->nofog = 0;
	t->backed = 0;
	t->fights = 0;
	t->undersea = 0;
	t->plain = v[0].plain;
	t->overlap = 0;
}

/*
 * The normals and shades of the level's triangles, worked out once while
 * markBacked(), markFights() and markDecals() ask for them: those ask a
 * triangle's normal once for every triangle sharing a grid cell with it,
 * which on Frigate was 5 of the 7 seconds the level took to build (F3
 * 20260929-143105, "GE Plus levels load much slower"). The cached values are
 * the ones triNormal() and triShade() work out, to the bit.
 */
static const struct stri *triCacheBase;
static s32 triCacheNum;
static f32 (*triCacheNormal)[4]; // x, y, z, and the area triNormal() returns
static f32 *triCacheShade;

static f32 triNormalWork(const struct stri *t, f32 *n);
static f32 triShadeWork(const struct stri *t);

static void triCacheFill(const struct stri *tris, s32 num)
{
	triCacheNormal = malloc(sizeof(*triCacheNormal) * (num > 0 ? num : 1));
	triCacheShade = malloc(sizeof(*triCacheShade) * (num > 0 ? num : 1));

	if (!triCacheNormal || !triCacheShade) {
		free(triCacheNormal);
		free(triCacheShade);
		triCacheNormal = NULL;
		triCacheShade = NULL;
		return;
	}

	for (s32 i = 0; i < num; i++) {
		triCacheNormal[i][3] = triNormalWork(&tris[i], triCacheNormal[i]);
		triCacheShade[i] = triShadeWork(&tris[i]);
	}

	triCacheBase = tris;
	triCacheNum = num;
}

static void triCacheForget(void)
{
	free(triCacheNormal);
	free(triCacheShade);
	triCacheNormal = NULL;
	triCacheShade = NULL;
	triCacheBase = NULL;
	triCacheNum = 0;
}

static inline __attribute__((always_inline)) s32 triCacheIndex(const struct stri *t)
{
	if (triCacheBase && t >= triCacheBase && t < triCacheBase + triCacheNum) {
		return (s32)(t - triCacheBase);
	}

	return -1;
}

static inline __attribute__((always_inline)) f32 triNormal(const struct stri *t, f32 *n)
{
	const s32 i = triCacheIndex(t);

	if (i >= 0) {
		n[0] = triCacheNormal[i][0];
		n[1] = triCacheNormal[i][1];
		n[2] = triCacheNormal[i][2];

		return triCacheNormal[i][3];
	}

	return triNormalWork(t, n);
}

static f32 triNormalWork(const struct stri *t, f32 *n)
{
	f32 e1[3], e2[3], len;

	for (s32 k = 0; k < 3; k++) {
		e1[k] = t->pos[1][k] - t->pos[0][k];
		e2[k] = t->pos[2][k] - t->pos[0][k];
	}

	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	len = sqrtf(dot3(n, n));

	if (len > 0) {
		n[0] /= len;
		n[1] /= len;
		n[2] /= len;
	}

	return len * 0.5f;
}

/*
 * The marking passes ask, of each of up to seven points on a triangle,
 * whether a triangle of the cell the point is in lies under it - one that
 * passes a test of its own against the triangle (facing, plane, picture) and
 * comes within DECAL_DIST of the point. The test does not depend on the point
 * and the points of one triangle mostly share a cell, so a cell is walked
 * once, the test asked once of each of its triangles, and only the distance
 * measured per point; the answers are the ones asking point by point gives.
 */
enum { MARKFILTER_BACKED, MARKFILTER_FIGHTS, MARKFILTER_DECAL };

static s32 triOther(const struct stri *t, const struct stri *u);

static s32 markCandidate(const struct stri *tris, s32 i, const f32 *ni, s32 o, s32 kind)
{
	const struct stri *t = &tris[i];
	const struct stri *u = &tris[o];
	f32 nu[3], area, cosang;
	s32 flat = 1;

	// The facing first, out of the cached normals: nearly every triangle of
	// a cell fails it, and it is the one test that does not have to read the
	// triangle itself. The tests only ever reject, so their order is free.
	if (o == i) {
		return 0;
	}

	area = triNormal(u, nu);

	if (area <= 0) {
		return 0;
	}

	cosang = dot3(ni, nu);

	if (kind != MARKFILTER_DECAL) {
		if (cosang > -DECAL_COS || texIsXlu(u->tex)) {
			return 0;
		}

		if (kind == MARKFILTER_BACKED) {
			return texHasAlpha(u->tex) == texHasAlpha(t->tex);
		}
	} else {
		if ((cosang < DECAL_COS && cosang > -DECAL_COS) || !triOther(t, u)
				|| (cosang < 0 && triCulled(t) && triCulled(u))) {
			return 0;
		}
	}

	for (s32 k = 0; k < 3 && flat; k++) {
		f32 rel[3] = { t->pos[k][0] - u->pos[0][0], t->pos[k][1] - u->pos[0][1], t->pos[k][2] - u->pos[0][2] };

		flat = fabsf(dot3(rel, nu)) <= DECAL_DIST;
	}

	return flat;
}

/**
 * Whether every point (`all`) or any point is on a triangle of its cell that
 * passes markCandidate().
 */
static s32 markPointsOn(const struct stri *tris, const struct tgrid *g, s32 i, const f32 *ni,
		const f32 (*pts)[3], s32 npts, s32 kind, s32 all)
{
	u32 key[7];
	u8 hit[7] = { 0 };
	u8 done[7] = { 0 };

	for (s32 k = 0; k < npts; k++) {
		key[k] = gridKey((s32)floorf(pts[k][0] / g->cell), (s32)floorf(pts[k][1] / g->cell), (s32)floorf(pts[k][2] / g->cell));
	}

	for (s32 k = 0; k < npts; k++) {
		s32 left = 0;

		if (done[k]) {
			continue;
		}

		for (s32 m = k; m < npts; m++) {
			if (key[m] == key[k]) {
				done[m] = 1;
				left++;
			}
		}

		for (s32 e = g->head[key[k]]; e >= 0 && left > 0; e = g->entnext[e]) {
			const s32 o = g->room[g->enttri[e]];
			const struct stri *u = &tris[o];

			if (!markCandidate(tris, i, ni, o, kind)) {
				continue;
			}

			for (s32 m = k; m < npts; m++) {
				if (key[m] == key[k] && !hit[m]
						&& pointTriDist(pts[m], u->pos[0], u->pos[1], u->pos[2]) <= DECAL_DIST * DECAL_DIST) {
					hit[m] = 1;
					left--;

					if (!all) {
						return 1;
					}
				}
			}
		}

		if (all && left > 0) {
			return 0;
		}
	}

	return all;
}

/**
 * Marks the faces of two-faced sheets: a triangle whose whole face is
 * covered by triangles facing the other way in its own plane. 4J built
 * decks, treads and roofs as single planes, some of them with a second face
 * underneath (Surface's platform decks: the planks on top in white, the same
 * planks underneath in a grey), and an HD room is drawn two-sided, so from
 * either side the far face fought the near one (F3 20260925-032341: "z
 * fighting/flickering under this platform"). writeLeaf() draws these culled,
 * which leaves each side its own face. Covered is its middle and its corners
 * (pulled a tenth of the way in) all on such triangles, so a sheet with a
 * face only under part of it keeps its other face two-sided. Solid pictures
 * pair with solid ones and cut-outs with cut-outs; the translucent layer is
 * left alone.
 */
static s32 markBacked(struct stri *tris, s32 num, const struct tgrid *g)
{
	s32 count = 0;

	for (s32 i = 0; i < num; i++) {
		struct stri *t = &tris[i];
		f32 ni[3], mid[3], pts[4][3];
		s32 covered = 1;

		if (texIsXlu(t->tex) || triNormal(t, ni) <= 0) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
		}

		for (s32 k = 0; k < 3; k++) {
			for (s32 j = 0; j < 3; j++) {
				pts[k][j] = t->pos[k][j] + (mid[j] - t->pos[k][j]) * 0.1f;
			}
		}

		memcpy(pts[3], mid, sizeof(mid));

		covered = markPointsOn(tris, g, i, ni, (const f32 (*)[3])pts, 4, MARKFILTER_BACKED, 1);

		if (covered) {
			t->backed = 1;
			count++;
		}
	}

	return count;
}

/**
 * Marks the faces that fight another back to back over part of their face.
 * markBacked() takes a sheet whose face is wholly covered by faces the other
 * way; where they meet over part of it only, each is left two-sided so the
 * part with no face under it shows from both sides - and over the part they
 * share, the back of each fought the face of the other. Cradle's stairwell
 * landings are a tread grating over a deck whose underside is one big plate
 * facing down (F3 20260926-082748: "z fighting when turning camera here").
 * writeLeaf() draws such a triangle's face culled and then its back, pushed
 * away from the eye, so the face turned to the camera wins wherever two
 * share the plane. Asked at its middle and its corners pulled a tenth of the
 * way in; the translucent layer is left alone, as markBacked() leaves it.
 */
static s32 markFights(struct stri *tris, s32 num, const struct tgrid *g)
{
	s32 count = 0;

	for (s32 i = 0; i < num; i++) {
		struct stri *t = &tris[i];
		f32 ni[3], mid[3], pts[4][3];
		s32 fights = 0;

		if (t->backed || texIsXlu(t->tex) || triNormal(t, ni) <= 0) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
		}

		for (s32 k = 0; k < 3; k++) {
			for (s32 j = 0; j < 3; j++) {
				pts[k][j] = t->pos[k][j] + (mid[j] - t->pos[k][j]) * 0.1f;
			}
		}

		memcpy(pts[3], mid, sizeof(mid));

		fights = markPointsOn(tris, g, i, ni, (const f32 (*)[3])pts, 4, MARKFILTER_FIGHTS, 0);

		if (fights) {
			t->fights = 1;
			count++;
		}
	}

	return count;
}

/** How bright a triangle's vertex colours are, 0 to 255. */
static inline __attribute__((always_inline)) f32 triShade(const struct stri *t)
{
	const s32 i = triCacheIndex(t);

	return i >= 0 ? triCacheShade[i] : triShadeWork(t);
}

static f32 triShadeWork(const struct stri *t)
{
	f32 sum = 0.0f;

	for (s32 k = 0; k < 3; k++) {
		sum += ((t->argb[k] >> 16) & 0xff) + ((t->argb[k] >> 8) & 0xff) + (t->argb[k] & 0xff);
	}

	return sum / 9.0f;
}

/**
 * Whether u is another picture than t for the decals: another texture, or
 * the same one painted much darker or lighter by its vertices. Frigate's
 * black hazard arrows are the wall's own picture on vertices of 0x000020,
 * lying on the wall, and the two fought in dots (F3 20260926-225352).
 */
static s32 triOther(const struct stri *t, const struct stri *u)
{
	return u->tex != t->tex || fabsf(triShade(t) - triShade(u)) > SHADE_APART;
}

/**
 * Unlit faces: triangles whose three corners are opaque black where the
 * level round them is lit. Rare baked no light into a few faces they did not
 * expect to be seen - the ends and tops of walls, a filler block - and the
 * release draws them solid black: in Aztec a block filling a wall's corner
 * at the top of a shaft and the top of the wall beside it drew a black L
 * across the corner (F3 20260929-055243, "black nothing texture in corner of
 * aztec"). GoldenEye has lit wall there. Such a face takes the light of the
 * nearest lit face - one in its own plane, facing its way, before any other
 * - and one with no picture laid on it (the same UV at every corner) takes
 * that face's picture too, carried on across it.
 *
 * Only a picture that is lit nearly everywhere else counts: the faces of a
 * picture black on much of its use are dark on purpose (Aztec's pit, a
 * recess), and so is a black face with its picture laid on under a cast
 * shadow (markShadows()).
 */
#define UNLIT_SHARE 0.05f   // a picture black on more than this share of its faces is dark on purpose
#define UNLIT_REACH 400.0f  // how far a lit face is looked for

static const f32 (*unlitMid)[3];

static s32 unlitCmp(const void *a, const void *b)
{
	const f32 xa = unlitMid[*(const s32 *)a][0];
	const f32 xb = unlitMid[*(const s32 *)b][0];

	return xa < xb ? -1 : xa > xb ? 1 : 0;
}

static s32 triIsBlack(const struct stri *t)
{
	return t->argb[0] == 0xff000000 && t->argb[1] == 0xff000000 && t->argb[2] == 0xff000000;
}

static s32 paintUnlit(struct collect *c)
{
	s32 *total = calloc(GEBEAN_MAXMATS + 1, sizeof(s32));
	s32 *black = calloc(GEBEAN_MAXMATS + 1, sizeof(s32));
	f32 (*mid)[3] = malloc(sizeof(*mid) * (c->num ? c->num : 1));
	s32 *byx = malloc(sizeof(s32) * (c->num ? c->num : 1));
	s32 painted = 0;

	if (!total || !black || !mid || !byx) {
		free(byx);
		free(total);
		free(black);
		free(mid);
		return 0;
	}

	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];
		const s32 slot = tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS ? tri->tex : GEBEAN_MAXMATS;

		for (s32 j = 0; j < 3; j++) {
			mid[t][j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
		}

		total[slot]++;

		if (triIsBlack(tri)) {
			black[slot]++;
		}
	}

	// Faces in order along x, so that only those within reach are looked at
	for (s32 t = 0; t < c->num; t++) {
		byx[t] = t;
	}

	unlitMid = (const f32 (*)[3])mid;
	qsort(byx, c->num, sizeof(s32), unlitCmp);

	for (s32 t = 0; t < c->num; t++) {
		struct stri *tri = &c->tris[t];
		const s32 slot = tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS ? tri->tex : GEBEAN_MAXMATS;
		const s32 bare = tri->uv[0][0] == tri->uv[1][0] && tri->uv[0][0] == tri->uv[2][0]
			&& tri->uv[0][1] == tri->uv[1][1] && tri->uv[0][1] == tri->uv[2][1];
		f32 n[3];
		f32 best = UNLIT_REACH * UNLIT_REACH;
		s32 from = -1;

		if (!triIsBlack(tri) || (!bare && black[slot] > total[slot] * UNLIT_SHARE)
				|| texHasAlpha(tri->tex) || triNormal(tri, n) <= 0.0f) {
			continue;
		}

		// The nearest lit face; one in the same plane, facing the same way,
		// before any other, whose picture then carries straight on
		for (s32 pass = 0; pass < 2 && from < 0; pass++) {
			s32 lo = 0;
			s32 hi = c->num;

			// the first face within reach along x
			while (lo < hi) {
				const s32 m = (lo + hi) / 2;

				if (mid[byx[m]][0] < mid[t][0] - UNLIT_REACH) {
					lo = m + 1;
				} else {
					hi = m;
				}
			}

			for (s32 i = lo; i < c->num && mid[byx[i]][0] <= mid[t][0] + UNLIT_REACH; i++) {
				const s32 u = byx[i];
				const struct stri *o = &c->tris[u];
				const f32 d[3] = { mid[u][0] - mid[t][0], mid[u][1] - mid[t][1], mid[u][2] - mid[t][2] };
				const f32 dist = dot3(d, d);
				f32 no[3];

				if (u == t || dist >= best || triIsBlack(o) || o->tex < 0 || texHasAlpha(o->tex) || o->blend
						|| triNormal(o, no) <= 0.0f) {
					continue;
				}

				if (pass == 0) {
					const f32 off[3] = { o->pos[0][0] - tri->pos[0][0], o->pos[0][1] - tri->pos[0][1], o->pos[0][2] - tri->pos[0][2] };

					if (dot3(n, no) < 0.99f || fabsf(dot3(off, n)) > 1.0f) {
						continue;
					}
				}

				best = dist;
				from = u;
			}
		}

		if (from < 0) {
			continue;
		}

		{
			const struct stri *o = &c->tris[from];
			u32 sum[3] = { 0, 0, 0 };
			u32 argb;

			for (s32 k = 0; k < 3; k++) {
				for (s32 j = 0; j < 3; j++) {
					sum[j] += (o->argb[k] >> (16 - j * 8)) & 0xff;
				}
			}

			argb = 0xff000000 | (sum[0] / 3) << 16 | (sum[1] / 3) << 8 | (sum[2] / 3);

			if (bare) {
				// o's picture laid on as o has it: each corner put in o's
				// plane and given the UV o's own corners make there
				f32 no[3], e1[3], e2[3], d00, d01, d11, den;

				triNormal(o, no);

				for (s32 j = 0; j < 3; j++) {
					e1[j] = o->pos[1][j] - o->pos[0][j];
					e2[j] = o->pos[2][j] - o->pos[0][j];
				}

				d00 = dot3(e1, e1);
				d01 = dot3(e1, e2);
				d11 = dot3(e2, e2);
				den = d00 * d11 - d01 * d01;

				if (den == 0.0f) {
					continue;
				}

				for (s32 k = 0; k < 3; k++) {
					const f32 p[3] = { tri->pos[k][0] - o->pos[0][0], tri->pos[k][1] - o->pos[0][1], tri->pos[k][2] - o->pos[0][2] };
					const f32 d20 = dot3(p, e1);
					const f32 d21 = dot3(p, e2);
					const f32 v = (d11 * d20 - d01 * d21) / den;
					const f32 w = (d00 * d21 - d01 * d20) / den;

					for (s32 j = 0; j < 2; j++) {
						tri->uv[k][j] = o->uv[0][j] + v * (o->uv[1][j] - o->uv[0][j]) + w * (o->uv[2][j] - o->uv[0][j]);
					}
				}

				tri->tex = o->tex;
			}

			for (s32 k = 0; k < 3; k++) {
				tri->argb[k] = argb;
			}

			painted++;
		}
	}

	free(byx);
	free(total);
	free(black);
	free(mid);

	return painted;
}

/**
 * A cast shadow: a cut-out decal whose corners are all black. Rare's release
 * lays the thing's own picture flat under it in black - Archives' ceiling
 * fan, its blades and its grille, on the floor of the room Natalya is held in
 * - and draws it solid, a hard black cut-out on the floor ("a strange texture
 * of the shadow of the fan", F3 20260929-011949). GoldenEye's shadow there is
 * a see-through darkening of the floor. Such a decal is drawn half through,
 * in the translucent leaf (triFades()). So is a black decal in the release's
 * blended pass whose picture has no alpha of its own: Bunker II's pillar
 * shadow across the cell corridor came out a solid black strip (F3
 * 20260930-030521), where GoldenEye darkens the floor.
 */
#define SHADOW_ALPHA 0x80

static s32 markShadows(struct stri *tris, s32 num)
{
	s32 marked = 0;

	for (s32 t = 0; t < num; t++) {
		struct stri *tri = &tris[t];

		if (!tri->decal || (!tri->blend && (!texHasAlpha(tri->tex) || texIsXlu(tri->tex)))
				|| (tri->argb[0] & 0xffffff) || (tri->argb[1] & 0xffffff) || (tri->argb[2] & 0xffffff)
				|| (tri->argb[0] >> 24) < FADE_ALPHA || (tri->argb[1] >> 24) < FADE_ALPHA || (tri->argb[2] >> 24) < FADE_ALPHA) {
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			tri->argb[k] = (u32)SHADOW_ALPHA << 24;
		}

		marked++;
	}

	return marked;
}

/**
 * Whether a triangle lies wholly on triangles of other pictures in its plane:
 * its middle and its corners, pulled a tenth of the way in, and the middles
 * of its edges, pulled in the same way, each on one.
 */
static s32 decalCovered(const struct stri *tris, const struct tgrid *g, s32 i)
{
	const struct stri *t = &tris[i];
	f32 ni[3], mid[3], pts[7][3];
	s32 n = 0;

	if (triNormal(t, ni) <= 0) {
		return 0;
	}

	for (s32 j = 0; j < 3; j++) {
		mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
	}

	memcpy(pts[n++], mid, sizeof(mid));

	for (s32 k = 0; k < 3; k++) {
		for (s32 j = 0; j < 3; j++) {
			pts[n][j] = t->pos[k][j] + (mid[j] - t->pos[k][j]) * 0.1f;
		}

		n++;

		for (s32 j = 0; j < 3; j++) {
			const f32 edge = (t->pos[k][j] + t->pos[(k + 1) % 3][j]) * 0.5f;

			pts[n][j] = edge + (mid[j] - edge) * 0.1f;
		}

		n++;
	}

	return markPointsOn(tris, g, i, ni, (const f32 (*)[3])pts, n, MARKFILTER_DECAL, 1);
}

/**
 * Marks the triangles that lie flat on another picture's triangle. Bean's
 * decals share the plane of the surface under them exactly, and drawn with
 * the ordinary depth test the two fought (a tester's F3 on Aztec, every HD
 * level): the decal is drawn in a decal render mode instead, pulled towards
 * the camera. Of a pair, the one lying wholly on other pictures is the decal
 * (decalCovered()); if both or neither do, the one with a cut-out picture
 * over the one without, else (both wholly on the other) the one Bean draws
 * later, else the smaller, else the one Bean draws later.
 *
 * Wholly on first: Bunker's hammer and sickle plaques overlap a wall panel
 * and hang past it onto the panels round it, and the panel's half-quad was
 * the smaller of the pair. It was made the decal, the other half of its quad
 * was not, and the plaque and the panel fought where they overlapped, while
 * the part of the panel off the plaque, drawn as a decal on nothing, was
 * painted over by the rock of a room drawn after it (F3 20260925-231104:
 * "z-fighting texture and inconsistent wall").
 *
 * Where each lies wholly on the other, the one Bean draws later, as the
 * release's depth test (less or equal) shows it: Bunker's other hammer and
 * sickle plaques lie on a wall cut into pieces smaller than the plaque, and
 * by size every piece was the decal and the wall was drawn over the plaque
 * (F3 20260926-210116).
 */
static s32 markDecals(struct stri *tris, s32 num, const struct tgrid *g)
{
	s32 count = 0;
	u8 *full = calloc(num > 0 ? num : 1, 1);

	for (s32 i = 0; full && i < num; i++) {
		full[i] = decalCovered(tris, g, i);
	}

	for (s32 i = 0; i < num; i++) {
		struct stri *t = &tris[i];
		f32 ni[3], mid[3];
		f32 ai;

		ai = triNormal(t, ni);

		if (ai <= 0) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
		}

		for (s32 e = g->head[gridKey((s32)floorf(mid[0] / g->cell), (s32)floorf(mid[1] / g->cell), (s32)floorf(mid[2] / g->cell))];
				e >= 0 && !t->decal; e = g->entnext[e]) {
			const s32 o = g->room[g->enttri[e]];
			const struct stri *u = &tris[o];
			const s32 alphai = texHasAlpha(t->tex), alphau = texHasAlpha(u->tex);
			f32 nu[3], au, cosang, d;
			s32 flat = 1;

			if (o == i || !triOther(t, u)) {
				continue;
			}

			au = triNormal(u, nu);
			cosang = dot3(ni, nu);

			if (au <= 0 || (cosang < DECAL_COS && cosang > -DECAL_COS)) {
				continue;
			}

			// Back to back and drawn culled (markBacked(), markFights()):
			// each side shows its own face, and a decal would show from
			// behind as well
			if (cosang < 0 && triCulled(t) && triCulled(u)) {
				continue;
			}

			for (s32 k = 0; k < 3 && flat; k++) {
				f32 rel[3] = { t->pos[k][0] - u->pos[0][0], t->pos[k][1] - u->pos[0][1], t->pos[k][2] - u->pos[0][2] };

				flat = fabsf(dot3(rel, nu)) <= DECAL_DIST;
			}

			if (!flat) {
				continue;
			}

			d = pointTriDist(mid, u->pos[0], u->pos[1], u->pos[2]);

			if (d > DECAL_DIST * DECAL_DIST) {
				continue;
			}

			if (full && full[i] != full[o] ? full[i]
					: alphai != alphau ? alphai > alphau
					: full && full[i] ? i > o
					: ai < au * 0.999f ? 1
					: ai <= au * 1.001f && i > o) {
				t->decal = 1;
				t->decalbase = o;
				count++;
			}
		}
	}

	free(full);

	return count;
}

/**
 * Which decals are lifted off their base (DECAL_LIFT, writeLeaf()).
 *
 * The lift is for a picture painted on a surface - Dam's CTON on its wall, a
 * sign - whose corners are not the surface's own: rounded to whole units they
 * can land behind it (646b97868). It is wrong for a decal that is part of a
 * mesh. Surface's snow drifts are an opaque snow face over a blended one on
 * the same corners, and the snow face is the decal; lifted two units, it came
 * away from the faces beside it that are not decals, and every edge between
 * them was a crack onto the sky (F3 20260928-025456: "colored lines on snow
 * floor near one of the cabins").
 *
 * So decals are gathered into pieces by the corners they share once rounded
 * (the same rounding writeLeaf() does), and a piece is lifted only when none
 * of its corners is a corner of a triangle that is not a decal. A piece that
 * shares a corner with the mesh around it stays where it is, and the decal
 * mode alone keeps it in front, as before the lift.
 */
struct liftcorner {
	s32 key[3];
	s32 tri;
};

static s32 liftCornerCmp(const void *a, const void *b)
{
	const struct liftcorner *ca = a;
	const struct liftcorner *cb = b;

	for (s32 j = 0; j < 3; j++) {
		if (ca->key[j] != cb->key[j]) {
			return ca->key[j] < cb->key[j] ? -1 : 1;
		}
	}

	return ca->tri - cb->tri;
}

static s32 liftFind(s32 *parent, s32 i)
{
	while (parent[i] != i) {
		parent[i] = parent[parent[i]];
		i = parent[i];
	}

	return i;
}

static s32 markDecalLifts(struct stri *tris, s32 num)
{
	struct liftcorner *corners;
	s32 *parent;
	u8 *pinned;
	s32 numcorners = 0;
	s32 lifted = 0;
	s32 kept = 0;
	s32 sunk = 0;
	s32 i;
	s32 j;

	if (num <= 0) {
		return 0;
	}

	corners = malloc(sizeof(*corners) * num * 3);
	parent = malloc(sizeof(*parent) * num);
	pinned = calloc(num, 1);

	if (!corners || !parent || !pinned) {
		// Nothing lifted: the decal mode alone, as before the lift
		free(corners);
		free(parent);
		free(pinned);
		return 0;
	}

	for (i = 0; i < num; i++) {
		parent[i] = i;

		for (s32 k = 0; k < 3; k++) {
			for (j = 0; j < 3; j++) {
				corners[numcorners].key[j] = (s32)roundf(tris[i].pos[k][j]);
			}

			corners[numcorners].tri = i;
			numcorners++;
		}
	}

	qsort(corners, numcorners, sizeof(*corners), liftCornerCmp);

	for (i = 0; i < numcorners; i = j) {
		s32 first = -1;
		s32 plain = 0;

		for (j = i; j < numcorners && memcmp(corners[j].key, corners[i].key, sizeof(corners[i].key)) == 0; j++) {
			const s32 t = corners[j].tri;

			if (!tris[t].decal) {
				plain = 1;
			} else if (first < 0) {
				first = t;
			} else {
				const s32 a = liftFind(parent, first);
				const s32 b = liftFind(parent, t);

				if (a != b) {
					parent[b] = a;
					pinned[a] |= pinned[b];
				}
			}
		}

		if (plain && first >= 0) {
			pinned[liftFind(parent, first)] = 1;
		}
	}

	for (i = 0; i < num; i++) {
		if (tris[i].decal) {
			tris[i].lift = !pinned[liftFind(parent, i)];

			if (tris[i].lift) {
				lifted++;
			} else {
				kept++;
			}
		}
	}

	// A decal kept in its mesh is in the plane of its base again, and a
	// translucent base drawn after it at the same depth passes the depth test
	// and shows over it: the snow drifts' blended rock came through the snow
	// in hard-edged patches. Such a base is sunk DECAL_LIFT behind the decal
	// instead. A crack that opens in a blended layer is only the layer
	// missing along a hairline over the solid face under it; an opaque base
	// stays, and the decal mode keeps the decal in front of it.
	for (i = 0; i < num; i++) {
		const s32 o = tris[i].decalbase;
		f32 ni[3];
		f32 no[3];

		if (!tris[i].decal || tris[i].lift || o < 0 || o >= num || tris[o].decal || tris[o].sink) {
			continue;
		}

		if (!texIsXlu(tris[o].tex) && !triFades(&tris[o])) {
			continue;
		}

		if (triNormal(&tris[i], ni) <= 0.0f || triNormal(&tris[o], no) <= 0.0f) {
			continue;
		}

		tris[o].sink = dot3(ni, no) >= 0.0f ? 1 : -1;
		sunk++;
	}

	sysLogPrintf(LOG_NOTE, "gebeanstage: %d decal triangles lifted off their base, %d kept in their mesh, %d translucent bases sunk under them",
			lifted, kept, sunk);

	free(corners);
	free(parent);
	free(pinned);

	return lifted;
}

static f32 cornerNormal(const f32 (*p)[3], f32 *n)
{
	const f32 a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
	const f32 b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
	f32 len;

	n[0] = a[1] * b[2] - a[2] * b[1];
	n[1] = a[2] * b[0] - a[0] * b[2];
	n[2] = a[0] * b[1] - a[1] * b[0];
	len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

	if (len > 0.0f) {
		n[0] /= len;
		n[1] /= len;
		n[2] /= len;
	}

	return len;
}

/** A triangle's corners as writeLeaf() will write them: whole units from its room's position. */
static void cornersRounded(const struct stri *t, f32 (*out)[3])
{
	const s32 r = t->room;

	for (s32 k = 0; k < 3; k++) {
		for (s32 j = 0; j < 3; j++) {
			const f32 rp = j == 0 ? g_BgRooms[r].pos.x : j == 1 ? g_BgRooms[r].pos.y : g_BgRooms[r].pos.z;

			out[k][j] = roundf(t->pos[k][j] - rp) + rp;
		}
	}
}

/**
 * The corners of a decal kept in its mesh (markDecalLifts()) moved out along
 * the decal's face just far enough that the whole-unit corner writeLeaf()
 * makes of them is not behind its base as that is written.
 *
 * Train's riveted plates are two decal bands on a wall bent along the
 * middle of the plate, pinned to the mesh by corners they share with the
 * wall at the bend and with a fin inside the wall. Their other corners,
 * rounded to whole units, went up to half a unit behind the wall (itself
 * rounded), so the wall was drawn over most of the plate: a thin strip of
 * rivets seen head on, the whole plate at a slant (F3 20260930-014411
 * "clipping at certain distances").
 *
 * A corner that rounds onto its base's plane is not moved, so a decal on its
 * base's own corners (Surface's snow drifts) stays in the mesh. A corner is
 * moved the same for every decal it belongs to, and one a triangle other
 * than a decal shares only along that triangle's plane (a crack would open
 * otherwise: Depot's roof trim went missing from below).
  */
static s32 markDecalCorners(struct stri *tris, s32 num)
{
	struct liftcorner *corners;
	s32 numcorners = 0;
	s32 moved = 0;
	s32 i;
	s32 j;

	if (num <= 0) {
		return 0;
	}

	corners = malloc(sizeof(*corners) * num * 3);

	if (!corners) {
		return 0;
	}

	for (i = 0; i < num; i++) {
		for (s32 k = 0; k < 3; k++) {
			for (s32 m = 0; m < 3; m++) {
				corners[numcorners].key[m] = (s32)roundf(tris[i].pos[k][m]);
			}

			corners[numcorners].tri = i * 3 + k;
			numcorners++;
		}
	}

	qsort(corners, numcorners, sizeof(*corners), liftCornerCmp);

	for (i = 0; i < numcorners; i = j) {
		s32 usable = 1;
		s32 shared = 0;
		s32 count = 0;
		f32 dir[3] = { 0.0f, 0.0f, 0.0f };
		f32 len;
		f32 by = -1.0f;

		for (j = i; j < numcorners && memcmp(corners[j].key, corners[i].key, sizeof(corners[i].key)) == 0; j++) {
			const struct stri *t = &tris[corners[j].tri / 3];
			f32 n[3];

			if (!t->decal) {
				shared++;
				continue;
			}

			if (t->lift || t->decalbase < 0 || t->decalbase >= num || t->room <= 0
					|| tris[t->decalbase].room <= 0 || cornerNormal((const f32 (*)[3])t->pos, n) <= 0.0f) {
				usable = 0;
				continue;
			}

			dir[0] += n[0];
			dir[1] += n[1];
			dir[2] += n[2];
			count++;
		}

		if (!usable || count == 0) {
			continue;
		}

		len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);

		if (len < 0.5f * count) {
			continue;
		}

		dir[0] /= len;
		dir[1] /= len;
		dir[2] /= len;

		// A corner another triangle shares moves only along that triangle's
		// own plane, so it stays on it and no crack opens (Train's plates
		// share corners with a fin inside the wall, square to them); one on
		// a surface it would leave stays where it is
		for (s32 q = i; q < j && shared; q++) {
			const struct stri *t = &tris[corners[q].tri / 3];
			f32 n[3];

			if (!t->decal && cornerNormal((const f32 (*)[3])t->pos, n) > 0.0f && fabsf(dot3(n, dir)) > 0.05f) {
				usable = 0;
				break;
			}
		}

		if (!usable) {
			continue;
		}

		// Steps of a quarter unit up to DECAL_LIFT; the first that leaves the
		// corner on or in front of every base it lies on
		for (s32 step = 0; step <= 8 && by < 0.0f; step++) {
			const f32 s = step * (DECAL_LIFT / 8.0f);
			s32 ok = 1;

			for (s32 q = i; q < j && ok; q++) {
				const struct stri *t = &tris[corners[q].tri / 3];

				if (!t->decal) {
					continue;
				}

				const struct stri *b = &tris[t->decalbase];
				const s32 k = corners[q].tri % 3;
				f32 bc[3][3], nb[3], nt[3], w[3], rel[3];

				cornersRounded(b, bc);

				if (cornerNormal((const f32 (*)[3])bc, nb) <= 0.0f && cornerNormal((const f32 (*)[3])b->pos, nb) <= 0.0f) {
					continue;
				}

				cornerNormal((const f32 (*)[3])t->pos, nt);

				for (s32 m = 0; m < 3; m++) {
					const f32 rp = m == 0 ? g_BgRooms[t->room].pos.x : m == 1 ? g_BgRooms[t->room].pos.y : g_BgRooms[t->room].pos.z;

					w[m] = roundf(t->pos[k][m] + dir[m] * s - rp) + rp;
					rel[m] = w[m] - bc[0][m];
				}

				ok = dot3(rel, nb) * (dot3(nt, nb) >= 0.0f ? 1.0f : -1.0f) >= -0.01f;
			}

			if (ok) {
				by = s;
			}
		}

		if (by <= 0.0f) {
			continue;
		}

		for (s32 q = i; q < j; q++) {
			struct stri *t = &tris[corners[q].tri / 3];
			const s32 k = corners[q].tri % 3;

			if (!t->decal) {
				continue;
			}

			t->pos[k][0] += dir[0] * by;
			t->pos[k][1] += dir[1] * by;
			t->pos[k][2] += dir[2] * by;
		}

		moved++;
	}

	free(corners);

	return moved;
}

// What build() leaves, the level it read left open
static void forgetBuilt(void)
{
	shellForget();

	if (roomData) {
		for (s32 r = 0; r <= numRooms; r++) {
			free(roomData[r]);
		}
	}

	free(roomData);
	free(roomLen);
	free(roomHidden);
	free(roomWaterKept);
	free(backdrop);
	free(backdropOrder);
	free(backdropDist);
	backdrop = NULL;
	backdropOrder = NULL;
	backdropDist = NULL;
	numBackdrop = 0;
	farRaised = 0;
	farLogged = -1.0f;
	roomData = NULL;
	roomLen = NULL;
	roomHidden = NULL;
	roomWaterKept = NULL;
	numRooms = 0;
	numServed = 0;
	numHidden = 0;
	numWaterKept = 0;
	levelHasWater = 0;
}

static void forget(void)
{
	forgetBuilt();
	gebeanLevelClose(level);
	level = NULL;
	row = NULL;
	built = 0;
	numScrollSlots = 0;

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		texScroll[t][0] = texScroll[t][1] = 0.0f;
	}
}

/**
 * Bean's level meshes are not quite welded: where two of Rare's pieces meet,
 * a corner of one can stand a tenth of a unit off the same corner of the
 * other. Drawn in floats that is no gap, but a room's vertices are whole units
 * (writeLeaf()), and two corners either side of a half round apart: the
 * ceiling of Dam's tunnel under the road showed a thin wedge of sky along one
 * such seam, a unit wide at its end (F3 20260928-231924). Corners this near
 * each other are made one, the first met standing for the rest, so that they
 * round alike.
 */
#define WELD_DIST 0.25f

static s32 weldVertices(struct collect *c)
{
	enum { HASHBITS = 20 };
	const s32 numvtx = c->num * 3;
	s32 *head = malloc(sizeof(s32) << HASHBITS);
	s32 *next = malloc(sizeof(s32) * (numvtx + 1));
	s32 welded = 0;

	if (!head || !next) {
		free(head);
		free(next);
		return 0;
	}

	for (s32 i = 0; i < (1 << HASHBITS); i++) {
		head[i] = -1;
	}

	for (s32 v = 0; v < numvtx; v++) {
		f32 *pos = c->tris[v / 3].pos[v % 3];
		const s32 cx = (s32)floorf(pos[0] / WELD_DIST);
		const s32 cy = (s32)floorf(pos[1] / WELD_DIST);
		const s32 cz = (s32)floorf(pos[2] / WELD_DIST);
		s32 found = -1;

		for (s32 dx = -1; dx <= 1 && found < 0; dx++) {
			for (s32 dy = -1; dy <= 1 && found < 0; dy++) {
				for (s32 dz = -1; dz <= 1 && found < 0; dz++) {
					const u32 h = (((u32)(cx + dx) * 73856093u) ^ ((u32)(cy + dy) * 19349663u)
							^ ((u32)(cz + dz) * 83492791u)) & ((1 << HASHBITS) - 1);

					for (s32 r = head[h]; r >= 0; r = next[r]) {
						const f32 *q = c->tris[r / 3].pos[r % 3];
						const f32 ex = q[0] - pos[0];
						const f32 ey = q[1] - pos[1];
						const f32 ez = q[2] - pos[2];

						if (ex * ex + ey * ey + ez * ez <= WELD_DIST * WELD_DIST) {
							found = r;
							break;
						}
					}
				}
			}
		}

		if (found >= 0) {
			const f32 *q = c->tris[found / 3].pos[found % 3];

			if (q[0] != pos[0] || q[1] != pos[1] || q[2] != pos[2]) {
				pos[0] = q[0];
				pos[1] = q[1];
				pos[2] = q[2];
				welded++;
			}
		} else {
			const u32 h = (((u32)cx * 73856093u) ^ ((u32)cy * 19349663u) ^ ((u32)cz * 83492791u))
					& ((1 << HASHBITS) - 1);

			next[v] = head[h];
			head[h] = v;
		}
	}

	free(head);
	free(next);

	return welded;
}

/**
 * The level's backdrop, out of the triangles to be dealt into rooms.
 *
 * 4J ring some levels with a panorama on a band of a few dozen triangles far
 * outside the level - Surface's snowy peaks (a 2048x1024 photograph, 64
 * triangles 22000 to 26000 from the middle, up to 9800 high), Dam's and
 * Runway's - whose top vertices fade to alpha 0 into the sky. Dealt into the
 * rooms they were fogged solid in GoldenEye's fog colour and cut by its far
 * plane, which at Surface's 12500 left two flat lavender slabs standing in
 * the sky with slanting sides (F3 20260925-030405). A picture is the
 * backdrop when no vertex of any of its triangles lies over the level's rooms
 * in plan. It is drawn after the sky instead (gebeanStageRenderBackdrop()).
 *
 * Cradle's ring of canyon cliffs (a 1024x512 photograph, 360 triangles) dips
 * under the platform, so 142 of its triangles are over the level's rooms in
 * plan; drawn fogged it was a flat blue wall round the horizon once the far
 * plane no longer cut it away. A picture is the backdrop too when at least
 * half its triangles are outside and it reaches out past the level by more
 * than a third of the level's size: that is Cradle's cliffs and the Bunkers'
 * sky dome and cloud cap, and nothing that stands at a level's edge (the
 * Bunkers' own outside, 1700 past a level 8000 across, is the nearest).
 */
static void takeBackdrop(struct collect *c, s32 n)
{
	f32 mn[2] = { 1e30f, 1e30f };
	f32 mx[2] = { -1e30f, -1e30f };
	s32 *total = calloc(GEBEAN_MAXMATS, sizeof(s32));
	s32 *outside = calloc(GEBEAN_MAXMATS, sizeof(s32));
	f32 (*reach)[4] = calloc(GEBEAN_MAXMATS, sizeof(*reach));
	u8 *isbackdrop = calloc(GEBEAN_MAXMATS, 1);
	s32 kept = 0;
	f32 size;

	if (!total || !outside || !reach || !isbackdrop) {
		free(total);
		free(outside);
		free(reach);
		free(isbackdrop);
		return;
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		reach[t][0] = reach[t][1] = 1e30f;
		reach[t][2] = reach[t][3] = -1e30f;
	}

	for (s32 r = 1; r < n; r++) {
		mn[0] = MIN(mn[0], g_Rooms[r].bbmin[0]);
		mn[1] = MIN(mn[1], g_Rooms[r].bbmin[2]);
		mx[0] = MAX(mx[0], g_Rooms[r].bbmax[0]);
		mx[1] = MAX(mx[1], g_Rooms[r].bbmax[2]);
	}

	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];
		s32 over = 0;

		if (tri->tex < 0 || tri->tex >= GEBEAN_MAXMATS) {
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			over |= tri->pos[k][0] >= mn[0] && tri->pos[k][0] <= mx[0]
				&& tri->pos[k][2] >= mn[1] && tri->pos[k][2] <= mx[1];
		}

		total[tri->tex]++;
		outside[tri->tex] += !over;

		for (s32 k = 0; k < 3; k++) {
			reach[tri->tex][0] = MIN(reach[tri->tex][0], tri->pos[k][0]);
			reach[tri->tex][1] = MIN(reach[tri->tex][1], tri->pos[k][2]);
			reach[tri->tex][2] = MAX(reach[tri->tex][2], tri->pos[k][0]);
			reach[tri->tex][3] = MAX(reach[tri->tex][3], tri->pos[k][2]);
		}
	}

	size = MAX(mx[0] - mn[0], mx[1] - mn[1]);

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		const f32 past = MAX(MAX(mn[0] - reach[t][0], mn[1] - reach[t][1]),
				MAX(reach[t][2] - mx[0], reach[t][3] - mx[1]));

		isbackdrop[t] = total[t] > 0 && (outside[t] == total[t]
				|| (outside[t] * 2 >= total[t] && past > size / 3.0f));
	}

	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];

		if (tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS && isbackdrop[tri->tex]) {
			struct stri *n2 = realloc(backdrop, sizeof(*backdrop) * (numBackdrop + 1));

			if (n2) {
				backdrop = n2;
				backdrop[numBackdrop++] = *tri;
				continue;
			}
		}

		c->tris[kept++] = *tri;
	}

	c->num = kept;

	backdropMid[0] = (mn[0] + mx[0]) * 0.5f;
	backdropMid[1] = 0;
	backdropMid[2] = (mn[1] + mx[1]) * 0.5f;

	free(total);
	free(outside);
	free(reach);
	free(isbackdrop);
}

/**
 * A cut-out picture whose triangles keep v within one repeat is drawn with t
 * clamped. Wrapped, the filter at its clear edge reached round to the far
 * edge: Surface's forest wall (1024x512, clear at the top, snow at the foot)
 * drew its top edge as a thin line of the snow's texels, a dotted wire across
 * the sky above the treeline (F3 20260925-030405).
 *
 * So is a picture of triangles the release fades by their vertices
 * (triFades()) when their v starts at a repeat and runs on past it by less
 * than half, and the picture is itself a fade - opaque at one edge, clear at
 * the other - which does not repeat: the rows past its end are its clear
 * edge's, not its opaque edge again. Dam's server room lamp throws a cone of
 * light (v 0 at the lamp to 1.32 at the floor); wrapped, its foot was a band
 * of the lamp end, a solid white skirt round the cone (F3 20260925-235806).
 */
#define CLAMP_SLACK (1.0f / 32.0f) // of a repeat, past either end of a clamped cut-out

static void clampCutouts(const struct collect *c)
{
	f32 vmin[GEBEAN_MAXMATS];
	f32 vmax[GEBEAN_MAXMATS];
	u8 fades[GEBEAN_MAXMATS];

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		vmin[t] = 1e30f;
		vmax[t] = -1e30f;
		fades[t] = 0;
	}

	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];

		if (tri->tex < 0 || tri->tex >= GEBEAN_MAXMATS) {
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			vmin[tri->tex] = MIN(vmin[tri->tex], tri->uv[k][1]);
			vmax[tri->tex] = MAX(vmax[tri->tex], tri->uv[k][1]);
		}

		fades[tri->tex] |= triFades(tri);
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		// The UVs are whole 1/256ths, or 1/1024ths, of a repeat. A cut-out
		// may run a few 256ths past its repeat's ends: Statue Park's treeline
		// card (v 0 to 1.0117) was left wrapped for its foot's 3 rows, and its
		// top edge, clear, drew the dark foot's texels as a thin black line
		// across the sky (F3 20260928-233213). Clamped, those rows are the
		// picture's own edge row.
		const f32 k = floorf(vmin[t] + CLAMP_SLACK);
		s32 first, last;

		if (texAlpha[t] && vmin[t] <= vmax[t] && (vmax[t] <= k + 1.0f + CLAMP_SLACK
					|| (fades[t] && vmin[t] <= k + 1.0f / 2048.0f && vmax[t] < k + 1.5f
						&& xblaTexImageEdgeAlpha(texTile[t], &first, &last)
						&& (first - last >= 0x80 || last - first >= 0x80)))) {
			texClampV[t] = 1;
			texClampShift[t] = (s16)(k + 1.0f);
		}
	}
}

/**
 * Bean's doorways are Rare's remodel of GoldenEye's, and a few units wider or
 * taller than the door GoldenEye's setup sizes to its pad's box: round
 * Runway's double doors to the Facility the opening stood 4.5 above the doors
 * and 2 past their right edge, and as the doors lead nowhere, the sky showed
 * through in a strip round them (F3 20260925-230105). The N64 look has none:
 * GoldenEye's own opening is the door's box. Over every GoldenEye mission
 * most doors have a gap of a unit or two of the same kind, and a few up to
 * ten. A vertex of Bean's mesh at the door's faces, just past a side or the
 * top of a door's box, is pulled onto that edge, so that the opening is the
 * door's size again, as GoldenEye has it; one further past the edge is some
 * other wall's and stays, and so does every vertex past a side the level
 * already closes. The foot of a door stands on the floor and is left, and
 * the side a sliding door goes into: Bunker's doors rise into a slot over
 * them, which pulled down would have shut on them.
 */
#define DOORGAP_REACH 12.0f  // the most a vertex is pulled
#define DOORGAP_SHARE 0.06f  // and no more than this share of the door's side
#define DOORGAP_DEPTH 10.0f  // in front of or behind the door's faces
#define DOORGAP_ON    0.05f  // a vertex this near a door's edge is on it

struct doorbox {
	f32 mid[3];
	f32 axis[3][3]; // the pad's normal, up and look, as units
	f32 half[3];
	f32 reach[3];
	f32 radius;     // past this far from the middle, nothing is the door's
	s32 thin;       // the axis through the door
	s8 foot[3];     // the side of an axis that points down, 0 for neither
	s8 slide[3];    // the side a sliding door goes into the wall at, 0 for neither
	f32 travel[3];  // how far it goes along each axis, fully open (fillDoorSlots())
	s32 first;      // its run of the triangles near it (doorNearTris())
	s32 numnear;
	u8 open[3][2];  // a side with nothing of the level past it (doorSideOpen())
};

static s32 doorBoxes(struct doorbox **out)
{
	struct doorbox *boxes = NULL;
	s32 num = 0;

	for (s32 list = 0; list < 2; list++) {
		for (struct prop *prop = list ? g_Vars.pausedprops : g_Vars.activeprops; prop; prop = prop->next) {
			struct doorbox *b;
			struct doorbox *grown;
			struct pad pad;
			f32 lo[3], hi[3];

			if (prop->type != PROPTYPE_DOOR || !prop->door || prop->door->base.pad < 0) {
				continue;
			}

			grown = realloc(boxes, sizeof(*boxes) * (num + 1));

			if (!grown) {
				break;
			}

			boxes = grown;
			b = &boxes[num];

			padUnpack(prop->door->base.pad, PADFIELD_POS | PADFIELD_LOOK | PADFIELD_UP | PADFIELD_NORMAL | PADFIELD_BBOX, &pad);

			lo[0] = pad.bbox.xmin; hi[0] = pad.bbox.xmax;
			lo[1] = pad.bbox.ymin; hi[1] = pad.bbox.ymax;
			lo[2] = pad.bbox.zmin; hi[2] = pad.bbox.zmax;

			for (s32 k = 0; k < 3; k++) {
				b->axis[0][k] = pad.normal.f[k];
				b->axis[1][k] = pad.up.f[k];
				b->axis[2][k] = pad.look.f[k];
			}

			if (!normalize3(b->axis[0]) || !normalize3(b->axis[1]) || !normalize3(b->axis[2])) {
				continue;
			}

			b->thin = 0;

			for (s32 a = 0; a < 3; a++) {
				b->half[a] = (hi[a] - lo[a]) * 0.5f;
				b->reach[a] = MIN(DOORGAP_REACH, (hi[a] - lo[a]) * DOORGAP_SHARE);
				b->foot[a] = b->axis[a][1] < -0.7f ? 1 : b->axis[a][1] > 0.7f ? -1 : 0;

				if (b->half[a] < b->half[b->thin]) {
					b->thin = a;
				}
			}

			b->radius = sqrtf(b->half[0] * b->half[0] + b->half[1] * b->half[1] + b->half[2] * b->half[2])
				+ DOORGAP_DEPTH + DOORGAP_REACH;

			// A sliding door's move for all of frac (doorUpdateTiles())
			for (s32 a = 0; a < 3; a++) {
				const struct doorobj *door = prop->door;
				const f32 by = door->doorflags & DOORFLAG_0080
					? door->unk98.x * b->axis[a][0] + door->unk98.y * b->axis[a][1] + door->unk98.z * b->axis[a][2] : 0.0f;

				b->slide[a] = by > 0.01f ? 1 : by < -0.01f ? -1 : 0;
				b->travel[a] = fabsf(by) * door->maxfrac;
			}

			for (s32 k = 0; k < 3; k++) {
				b->mid[k] = pad.pos.f[k];

				for (s32 a = 0; a < 3; a++) {
					b->mid[k] += (lo[a] + hi[a]) * 0.5f * b->axis[a][k];
				}
			}

			num++;
		}
	}

	*out = boxes;

	return num;
}

/**
 * Where a vertex stands in a door's box: its distance along each axis from
 * the middle. Whether it is past the door's faces by more than the depth.
 */
static s32 doorBoxAt(const struct doorbox *b, const f32 *p, f32 *at)
{
	f32 d[3] = { p[0] - b->mid[0], p[1] - b->mid[1], p[2] - b->mid[2] };

	for (s32 a = 0; a < 3; a++) {
		at[a] = dot3(d, b->axis[a]);
	}

	return fabsf(at[b->thin]) <= b->half[b->thin] + DOORGAP_DEPTH;
}

static s32 addTri(struct collect *c, const struct stri *t)
{
	if (c->num >= c->cap) {
		s32 cap = c->cap ? c->cap * 2 : 65536;
		struct stri *n = realloc(c->tris, sizeof(*n) * cap);

		if (!n) {
			return 0;
		}

		c->tris = n;
		c->cap = cap;
	}

	c->tris[c->num++] = *t;

	return 1;
}

/**
 * Whether a point is on a triangle of the level already, one lying in the
 * plane with that normal: a strip there would fight it. The triangles are
 * the ones near the door (list) and every strip added since the level's own
 * (from first on).
 */
static s32 stripCovered(const struct collect *c, const s32 *list, s32 numlist, s32 first, const f32 *p, const f32 *normal)
{
	for (s32 i = 0; i < numlist + c->num - first; i++) {
		const struct stri *tri = &c->tris[i < numlist ? list[i] : first + i - numlist];
		f32 n[3], d[3];
		s32 inside = 1;

		for (s32 j = 0; j < 3 && inside; j++) {
			inside = p[j] >= MIN(tri->pos[0][j], MIN(tri->pos[1][j], tri->pos[2][j])) - 0.5f
				&& p[j] <= MAX(tri->pos[0][j], MAX(tri->pos[1][j], tri->pos[2][j])) + 0.5f;
		}

		if (!inside || triNormal(tri, n) <= 0.0f || fabsf(dot3(n, normal)) < 0.9f) {
			continue;
		}

		d[0] = p[0] - tri->pos[0][0];
		d[1] = p[1] - tri->pos[0][1];
		d[2] = p[2] - tri->pos[0][2];

		if (fabsf(dot3(d, n)) > 0.5f) {
			continue;
		}

		// on the inner side of each edge, about the triangle's own normal
		for (s32 k = 0; k < 3 && inside; k++) {
			const f32 *a = tri->pos[k];
			const f32 *b = tri->pos[(k + 1) % 3];
			const f32 e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const f32 q[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
			const f32 x[3] = { e[1] * q[2] - e[2] * q[1], e[2] * q[0] - e[0] * q[2], e[0] * q[1] - e[1] * q[0] };

			inside = dot3(x, n) >= 0.0f;
		}

		if (inside) {
			return 1;
		}
	}

	return 0;
}

/**
 * Bean's walls are sheets with no thickness, and a door stands in the middle
 * of GoldenEye's, so the sheet round a doorway is a few units in front of the
 * door's face (Runway's 2.4) and there is nothing between the two: from an
 * angle the far side of the opening showed a line of sky past the door's
 * edge even once the opening was the door's size, and looking down, a line
 * of it along the door's foot, where the floor stops at the wall. Each edge
 * of a wall's triangle that lies along a side or the top of a door, in front
 * of its face, gets a strip from the wall back to the door's middle, in the
 * wall's picture smeared across - the reveal a thicker wall would have - and
 * so does the edge of the floor in front of the door's foot, in the floor's
 * plane. To the middle and not the face: ending on the door's edge it left a
 * dotted line of sky where the two met. Not where the level has a triangle
 * there already (Bean's own reveal, or the floor going on under the door).
 */
static s32 fillDoorReveals(struct collect *c, const struct doorbox *boxes, s32 num, const s32 *nearby)
{
	const s32 numtris = c->num;
	s32 fillers = 0;

	for (s32 i = 0; i < num; i++) {
		const struct doorbox *b = &boxes[i];
		const s32 *near = &nearby[b->first];
		const s32 numnear = b->numnear;

		for (s32 q = 0; q < numnear; q++) {
			struct stri from;
			f32 n[3];
			s32 wall;

			// a copy: the strips added below can move the array
			if (triNormal(&c->tris[near[q]], n) <= 0.0f) {
				continue;
			}

			from = c->tris[near[q]];

			wall = fabsf(dot3(n, b->axis[b->thin])) >= 0.9f;

			for (s32 k = 0; k < 3; k++) {
				const s32 k2 = (k + 1) % 3;
				f32 at0[3], at1[3];
				s32 face;

				if (!doorBoxAt(b, from.pos[k], at0) || !doorBoxAt(b, from.pos[k2], at1)) {
					continue;
				}

				face = at0[b->thin] < 0.0f ? -1 : 1;

				if (at0[b->thin] * face - b->half[b->thin] < 0.25f || at1[b->thin] * face - b->half[b->thin] < 0.25f) {
					continue;
				}

				for (s32 a = 0; a < 3; a++) {
					const s32 o = 3 - a - b->thin;
					const s32 side = at0[a] < 0.0f ? -1 : 1;
					struct stri strip[2];
					f32 u[2], lo, hi;
					f32 end[2][3], back[2][3], mid[3];
					f32 sn[3];

					if (a == b->thin || fabsf(at1[o] - at0[o]) < 0.5f) {
						continue;
					}

					if (wall) {
						// along a side or the top
						if (side == b->foot[a]
								|| fabsf(at0[a] - side * b->half[a]) > DOORGAP_ON
								|| fabsf(at1[a] - side * b->half[a]) > DOORGAP_ON) {
							continue;
						}
					} else {
						// the floor's edge across the door's foot
						if (side != b->foot[a] || fabsf(dot3(n, b->axis[a])) < 0.9f
								|| fabsf(at0[b->thin] - at1[b->thin]) > 0.5f
								|| fabsf(fabsf(at0[a]) - b->half[a]) > b->reach[a]
								|| fabsf(fabsf(at1[a]) - b->half[a]) > b->reach[a]) {
							continue;
						}
					}

					// The part of the edge along this door: the wall over a
					// double door runs across both leaves
					u[0] = (-b->half[o] - at0[o]) / (at1[o] - at0[o]);
					u[1] = (b->half[o] - at0[o]) / (at1[o] - at0[o]);

					if (u[0] > u[1]) {
						const f32 swap = u[0];
						u[0] = u[1];
						u[1] = swap;
					}

					u[0] = MAX(u[0], 0.0f);
					u[1] = MIN(u[1], 1.0f);
					lo = at0[o] + (at1[o] - at0[o]) * u[0];
					hi = at0[o] + (at1[o] - at0[o]) * u[1];

					if (fabsf(hi - lo) < 0.5f || u[0] >= u[1]) {
						continue;
					}

					for (s32 e = 0; e < 2; e++) {
						const f32 depth = at0[b->thin] + (at1[b->thin] - at0[b->thin]) * u[e];

						for (s32 j = 0; j < 3; j++) {
							end[e][j] = from.pos[k][j] + (from.pos[k2][j] - from.pos[k][j]) * u[e];
							back[e][j] = end[e][j] - depth * b->axis[b->thin][j];
						}
					}

					for (s32 j = 0; j < 3; j++) {
						mid[j] = (end[0][j] + end[1][j] + back[0][j] + back[1][j]) * 0.25f;
					}

					// a wall's strip lies square to the side, the floor's in the floor
					if (stripCovered(c, near, numnear, numtris, mid, wall ? b->axis[a] : n)) {
						continue;
					}

					strip[0] = strip[1] = from;
					strip[0].decal = strip[1].decal = 0;
					strip[0].backed = strip[1].backed = 0;
					strip[0].fights = strip[1].fights = 0;

					// end 0, end 1, back of 1; end 0, back of 1, back of 0
					memcpy(strip[0].pos[0], end[0], sizeof(f32) * 3);
					memcpy(strip[0].pos[1], end[1], sizeof(f32) * 3);
					memcpy(strip[0].pos[2], back[1], sizeof(f32) * 3);
					memcpy(strip[1].pos[0], end[0], sizeof(f32) * 3);
					memcpy(strip[1].pos[1], back[1], sizeof(f32) * 3);
					memcpy(strip[1].pos[2], back[0], sizeof(f32) * 3);

					for (s32 j = 0; j < 2; j++) {
						const f32 uv0 = from.uv[k][j] + (from.uv[k2][j] - from.uv[k][j]) * u[0];
						const f32 uv1 = from.uv[k][j] + (from.uv[k2][j] - from.uv[k][j]) * u[1];

						strip[0].uv[0][j] = strip[1].uv[0][j] = strip[1].uv[2][j] = uv0;
						strip[0].uv[1][j] = strip[0].uv[2][j] = strip[1].uv[1][j] = uv1;
					}

					strip[0].argb[0] = strip[1].argb[0] = strip[1].argb[2] = from.argb[k];
					strip[0].argb[1] = strip[0].argb[2] = strip[1].argb[1] = from.argb[k2];

					// The side that culling keeps: a wall's strip faces into
					// the doorway, the floor's up as the floor does
					triNormal(&strip[0], sn);

					if (wall ? dot3(sn, b->axis[a]) * side > 0.0f : dot3(sn, n) < 0.0f) {
						for (s32 m = 0; m < 2; m++) {
							f32 tp[3], tuv[2];
							u32 targb = strip[m].argb[1];

							memcpy(tp, strip[m].pos[1], sizeof(tp));
							memcpy(strip[m].pos[1], strip[m].pos[2], sizeof(tp));
							memcpy(strip[m].pos[2], tp, sizeof(tp));
							memcpy(tuv, strip[m].uv[1], sizeof(tuv));
							memcpy(strip[m].uv[1], strip[m].uv[2], sizeof(tuv));
							memcpy(strip[m].uv[2], tuv, sizeof(tuv));
							strip[m].argb[1] = strip[m].argb[2];
							strip[m].argb[2] = targb;
						}
					}

					if (!addTri(c, &strip[0]) || !addTri(c, &strip[1])) {
						return fillers;
					}

					fillers++;
				}
			}
		}
	}

	return fillers;
}

/**
 * How far a triangle's UVs move for a move of delta along it: what is off its
 * plane moves nothing (a reveal pulled sideways).
 */
static void triUvShift(const struct stri *t, const f32 *delta, f32 *duv)
{
	const f32 e1[3] = { t->pos[1][0] - t->pos[0][0], t->pos[1][1] - t->pos[0][1], t->pos[1][2] - t->pos[0][2] };
	const f32 e2[3] = { t->pos[2][0] - t->pos[0][0], t->pos[2][1] - t->pos[0][1], t->pos[2][2] - t->pos[0][2] };
	const f32 g11 = dot3(e1, e1), g12 = dot3(e1, e2), g22 = dot3(e2, e2);
	const f32 det = g11 * g22 - g12 * g12;
	f32 r1, r2, x, y;

	duv[0] = duv[1] = 0.0f;

	if (det <= 1e-6f * g11 * g22) {
		return;
	}

	// delta's part in the plane as x e1 + y e2
	r1 = dot3(delta, e1);
	r2 = dot3(delta, e2);
	x = (r1 * g22 - r2 * g12) / det;
	y = (r2 * g11 - r1 * g12) / det;

	for (s32 j = 0; j < 2; j++) {
		duv[j] = x * (t->uv[1][j] - t->uv[0][j]) + y * (t->uv[2][j] - t->uv[0][j]);
	}
}

/**
 * Each door's run of the triangles whose box comes within its reach, in one
 * list: a level has up to 180000 triangles and 50 doors.
 */
static s32 *doorNearTris(const struct collect *c, struct doorbox *boxes, s32 num)
{
	f32 (*tbox)[6] = malloc(sizeof(*tbox) * MAX(c->num, 1));
	s32 *list = NULL;
	s32 len = 0, cap = 0;

	if (!tbox) {
		return NULL;
	}

	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];

		for (s32 j = 0; j < 3; j++) {
			tbox[t][j] = MIN(tri->pos[0][j], MIN(tri->pos[1][j], tri->pos[2][j]));
			tbox[t][3 + j] = MAX(tri->pos[0][j], MAX(tri->pos[1][j], tri->pos[2][j]));
		}
	}

	for (s32 i = 0; i < num; i++) {
		struct doorbox *b = &boxes[i];

		b->first = len;
		b->numnear = 0;

		for (s32 t = 0; t < c->num; t++) {
			f32 dd = 0.0f;

			// a big floor triangle's corners can all be far off
			for (s32 j = 0; j < 3; j++) {
				const f32 off = b->mid[j] < tbox[t][j] ? tbox[t][j] - b->mid[j]
					: b->mid[j] > tbox[t][3 + j] ? b->mid[j] - tbox[t][3 + j] : 0.0f;

				dd += off * off;
			}

			if (dd > b->radius * b->radius) {
				continue;
			}

			if (len >= cap) {
				s32 *grown = realloc(list, sizeof(*list) * (cap ? cap * 2 : 4096));

				if (!grown) {
					free(list);
					free(tbox);
					return NULL;
				}

				list = grown;
				cap = cap ? cap * 2 : 4096;
			}

			list[len++] = t;
			b->numnear++;
		}
	}

	free(tbox);

	return list;
}

/**
 * Whether a line through the door's thickness, from a point just past one of
 * its sides, meets nothing of the level: the side stands open onto whatever
 * is behind. A side the level closes (Bunker's bevelled door frames, their
 * slopes a few units over the doors) keeps its vertices where they are.
 */
static s32 doorSideOpen(const struct collect *c, const struct doorbox *b, const s32 *near, s32 a, s32 side)
{
	const s32 o = 3 - a - b->thin;
	const f32 reach = b->half[b->thin] + DOORGAP_DEPTH;
	const f32 *d = b->axis[b->thin];

	for (s32 f = -1; f <= 1; f++) {
		f32 q[3];
		s32 hit = 0;

		for (s32 j = 0; j < 3; j++) {
			q[j] = b->mid[j] + b->axis[a][j] * side * (b->half[a] + 0.5f) + b->axis[o][j] * f * b->half[o] * 0.7f;
		}

		for (s32 m = 0; m < b->numnear && !hit; m++) {
			const struct stri *tri = &c->tris[near[m]];
			f32 e1[3], e2[3], h[3], w[3], x[3];
			f32 det, u, v, t;

			for (s32 j = 0; j < 3; j++) {
				e1[j] = tri->pos[1][j] - tri->pos[0][j];
				e2[j] = tri->pos[2][j] - tri->pos[0][j];
				w[j] = q[j] - tri->pos[0][j];
			}

			h[0] = d[1] * e2[2] - d[2] * e2[1];
			h[1] = d[2] * e2[0] - d[0] * e2[2];
			h[2] = d[0] * e2[1] - d[1] * e2[0];
			det = dot3(e1, h);

			if (fabsf(det) < 1e-6f) {
				continue;
			}

			u = dot3(w, h) / det;
			x[0] = w[1] * e1[2] - w[2] * e1[1];
			x[1] = w[2] * e1[0] - w[0] * e1[2];
			x[2] = w[0] * e1[1] - w[1] * e1[0];
			v = dot3(d, x) / det;
			t = dot3(e2, x) / det;

			hit = u >= 0.0f && v >= 0.0f && u + v <= 1.0f && fabsf(t) <= reach;
		}

		if (!hit) {
			return 1;
		}
	}

	return 0;
}

/**
 * What lies under a level's sea is seen through it, as GoldenEye has it.
 *
 * GoldenEye's sea is the sky's water plane, drawn before everything, so
 * nothing can be under it; Frigate's reflection is the hull built again
 * upside down below its foot (-767 to -1384), in room 35's translucent leaf
 * at half alpha (environment alpha 0x7f) over the sea. Bean builds the same
 * mirrored hull, on both sides where GoldenEye has only the port one, but
 * among the hull's own opaque draws: served as it is it stood under the ship
 * as a second, solid hull and the ship looked to be flying (F3
 * 20260927-214623, 214759).
 *
 * So below the lowest of GoldenEye's opaque geometry, where that is under the
 * sea and GoldenEye draws something translucent there, Bean's triangles are
 * drawn translucent: at that leaf's alpha at the waterline, fading to nothing
 * at the deepest, as a reflection is clearest where the hull meets it (Bean
 * lights its mirror brightest at its foot, where the deck edge is, and at a
 * flat half alpha it still read as a second hull in pale grey). They take
 * the mirror's mean colour and a few steps of fade, which the room's palette
 * keeps (buildPalette()), and are turned outwards and culled so that the far
 * side's mirror is not seen through the near one's. What is above - the
 * hull's own foot, the decks inside - is as it was.
 */
struct undersea {
	f32 floor;  // the lowest of GoldenEye's opaque vertices
	s32 alpha;  // the most opaque of its translucent leaves wholly under that
};

// Slack between GoldenEye's vertices and Bean's on the same surface
#define UNDERSEA_SLACK 2.0f
// Steps of the reflection's fade
#define UNDERSEA_STEPS 8

static void fileTriLowest(void *arg, const f32 v[3][3], s32 room)
{
	struct undersea *u = arg;

	for (s32 k = 0; k < 3; k++) {
		u->floor = MIN(u->floor, v[k][1]);
	}
}

static void fileTriBelow(void *arg, const f32 v[3][3], s32 room)
{
	struct undersea *u = arg;

	if (v[0][1] <= u->floor + UNDERSEA_SLACK && v[1][1] <= u->floor + UNDERSEA_SLACK
			&& v[2][1] <= u->floor + UNDERSEA_SLACK) {
		u->alpha = MAX(u->alpha, eachEnvAlpha);
	}
}

static s32 triUnderSea(const struct stri *tri, f32 top)
{
	return tri->pos[0][1] <= top && tri->pos[1][1] <= top && tri->pos[2][1] <= top;
}

static s32 markUnderSea(struct collect *c, u8 **filerooms, u32 *filelens, s32 n)
{
	struct environment *env = envGetCurrent();
	struct undersea u = { 1e30f, 0 };
	f32 top, bottom = 1e30f;
	f32 centre[3] = { 0, 0, 0 };
	f64 mean[3] = { 0, 0, 0 };
	u32 rgb = 0;
	s32 num = 0;
	s32 marked = 0;

	if (!env || !env->water_enabled || !filerooms || !filelens) {
		return 0;
	}

	for (s32 r = 1; r < n; r++) {
		if (filerooms[r]) {
			fileRoomTrianglesEach(r, filerooms[r], filelens[r], 0, fileTriLowest, &u);
		}
	}

	if (u.floor >= env->water_scale) {
		return 0;
	}

	for (s32 r = 1; r < n; r++) {
		if (filerooms[r]) {
			fileRoomTrianglesEach(r, filerooms[r], filelens[r], EACH_XLU_ONLY, fileTriBelow, &u);
		}
	}

	if (u.alpha == 0) {
		return 0;
	}

	top = u.floor + UNDERSEA_SLACK;

	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];

		if (!triUnderSea(tri, top)) {
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			bottom = MIN(bottom, tri->pos[k][1]);

			for (s32 j = 0; j < 3; j++) {
				centre[j] += tri->pos[k][j];
				mean[j] += (tri->argb[k] >> (16 - j * 8)) & 0xff;
			}
		}

		num += 3;
	}

	if (num == 0) {
		return 0;
	}

	for (s32 j = 0; j < 3; j++) {
		centre[j] /= num;
		rgb |= (u32)(mean[j] / num + 0.5) << (16 - j * 8);
	}

	for (s32 t = 0; t < c->num; t++) {
		struct stri *tri = &c->tris[t];
		f32 mid[3], normal[3];

		if (!triUnderSea(tri, top)) {
			continue;
		}

		// Turned to face away from the middle of the mirror, so culling
		// keeps the side towards the eye
		for (s32 j = 0; j < 3; j++) {
			mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
		}

		if (triNormal(tri, normal) > 0.0f
				&& normal[0] * (mid[0] - centre[0]) + normal[2] * (mid[2] - centre[2]) < 0.0f) {
			const struct stri swap = *tri;

			for (s32 j = 0; j < 3; j++) {
				tri->pos[1][j] = swap.pos[2][j];
				tri->pos[2][j] = swap.pos[1][j];
			}

			for (s32 j = 0; j < 2; j++) {
				tri->uv[1][j] = swap.uv[2][j];
				tri->uv[2][j] = swap.uv[1][j];
			}
		}

		for (s32 k = 0; k < 3; k++) {
			const f32 fade = top > bottom ? (tri->pos[k][1] - bottom) / (top - bottom) : 1.0f;
			const s32 step = (s32)(fade * UNDERSEA_STEPS + 0.5f);

			tri->argb[k] = rgb | (u32)(u.alpha * step / UNDERSEA_STEPS) << 24;
		}

		tri->blend = 1;
		tri->undersea = 1;
		marked++;
	}

	return marked;
}

static s32 closeDoorGaps(struct collect *c)
{
	struct doorbox *boxes;
	const s32 num = doorBoxes(&boxes);
	s32 *nearby = num > 0 ? doorNearTris(c, boxes, num) : NULL;
	u8 *state = nearby ? calloc(c->num * 3, 1) : NULL; // 1 in a doorway, 2 done with
	s32 moved = 0;
	s32 fillers = 0;

	if (!state) {
		free(nearby);
		free(boxes);
		return 0;
	}

	// In a doorway (the other leaf's), whatever it is, it stays
	for (s32 i = 0; i < num; i++) {
		const struct doorbox *b = &boxes[i];

		for (s32 q = 0; q < b->numnear; q++) {
			const s32 t = nearby[b->first + q];

			for (s32 k = 0; k < 3; k++) {
				f32 at[3];
				s32 out = 0;

				if (!doorBoxAt(b, c->tris[t].pos[k], at)) {
					continue;
				}

				for (s32 a = 0; a < 3; a++) {
					out |= a != b->thin && fabsf(at[a]) > b->half[a];
				}

				if (!out) {
					state[t * 3 + k] = 1;
				}
			}
		}
	}

	for (s32 i = 0; i < num; i++) {
		for (s32 a = 0; a < 3; a++) {
			for (s32 side = 0; side < 2; side++) {
				boxes[i].open[a][side] = a != boxes[i].thin && boxes[i].slide[a] != (side ? 1 : -1)
					&& doorSideOpen(c, &boxes[i], &nearby[boxes[i].first], a, side ? 1 : -1);
			}
		}
	}

	// the first door a vertex is near has it
	for (s32 i = 0; i < num; i++) {
		const struct doorbox *b = &boxes[i];

		for (s32 q = 0; q < b->numnear; q++) {
			const s32 t = nearby[b->first + q];

			for (s32 k = 0; k < 3; k++) {
				f32 *p = c->tris[t].pos[k];
				f32 at[3];
				f32 delta[3] = { 0.0f, 0.0f, 0.0f };
				s32 near = 1;
				s32 pulled = 0;

				if (state[t * 3 + k] || !doorBoxAt(b, p, at)) {
					continue;
				}

				for (s32 a = 0; a < 3; a++) {
					if (a != b->thin && fabsf(at[a]) - b->half[a] > b->reach[a]) {
						near = 0;
					}
				}

				if (!near) {
					continue;
				}

				for (s32 a = 0; a < 3; a++) {
					const s32 side = at[a] < 0.0f ? -1 : 1;
					const f32 by = side * b->half[a] - at[a];

					if (a == b->thin || fabsf(at[a]) <= b->half[a] || side == b->foot[a] || !b->open[a][side > 0]) {
						continue;
					}

					for (s32 j = 0; j < 3; j++) {
						delta[j] += by * b->axis[a][j];
					}

					pulled = 1;
				}

				if (pulled) {
					f32 duv[2];

					state[t * 3 + k] = 2;

					// the picture stays where it was on the wall or floor
					triUvShift(&c->tris[t], delta, duv);

					for (s32 j = 0; j < 3; j++) {
						p[j] += delta[j];
					}

					c->tris[t].uv[k][0] += duv[0];
					c->tris[t].uv[k][1] += duv[1];
					moved++;
				}
			}
		}
	}

	fillers = fillDoorReveals(c, boxes, num, nearby);

	free(state);
	free(nearby);
	free(boxes);

	sysLogPrintf(LOG_NOTE, "gebeanstage: %d vertices pulled onto the edges of %d doors, %d reveals filled", moved, num, fillers);

	return moved;
}

/**
 * The wall a raised door goes up into. A door that slides up in GoldenEye
 * rises into the wall over its doorway, and GoldenEye's wall there is in the
 * door's own thickness - Depot's warehouses have it on the door's middle.
 * The N64 look never shows the raised door because the door is drawn through
 * the doorway's portal, clipped to it. The HD look draws every room whole
 * (gebeanStageDrawsEveryRoom()), and Bean's wall is a sheet a few units
 * behind the door's face, so the raised door was drawn over the wall: a
 * hazard-striped shutter over Depot's corrugated metal (F3 20260929-192603).
 *
 * A door that slides sideways into the wall beside its doorway is the same:
 * Control's lift doors went over the corrugated wall either side of the lift
 * (F3 20260930-013246), and Train's compartment doors, Bunker's, Egyptian's.
 *
 * For each door that slides into the level, up, down or sideways, the band it
 * travels through past its opening is looked at from each face, cell by cell: a line
 * from a little in front of the face to the door's far face. Where GoldenEye's
 * room has a triangle on that line (the wall the door goes into) and Bean's
 * has none in front of the door's face, the cell gets wall a hair in front of
 * the face, in the picture of Bean's own wall on that side - its plane's
 * mapping carried across, so the pattern runs on - and lit like it. Only
 * where GoldenEye has wall: a doorway with sky over it stays open. The cells
 * go down as a few rectangles.
 */
#define SLOT_CELL   12.0f  // a cell of the band, at most this big
#define SLOT_OUT    40.0f  // how far in front of the face the line starts
#define SLOT_FACE   0.25f  // at or this near the face is in front of it
#define SLOT_LIFT   2.0f   // a patch stands this far in front of the face
#define SLOT_PAINT  0.25f  // and what was painted on the wall this far in front of that

struct slotge {
	f32 (*tris)[3][3];
	s32 num;
	s32 cap;
	f32 lo[3], hi[3];
};

static void fileTriToSlot(void *arg, const f32 v[3][3], s32 room)
{
	struct slotge *g = arg;

	for (s32 j = 0; j < 3; j++) {
		if (MAX(v[0][j], MAX(v[1][j], v[2][j])) < g->lo[j] || MIN(v[0][j], MIN(v[1][j], v[2][j])) > g->hi[j]) {
			return;
		}
	}

	if (g->num >= g->cap) {
		const s32 cap = g->cap ? g->cap * 2 : 256;
		f32 (*grown)[3][3] = realloc(g->tris, sizeof(*grown) * cap);

		if (!grown) {
			return;
		}

		g->tris = grown;
		g->cap = cap;
	}

	memcpy(g->tris[g->num++], v, sizeof(f32) * 9);
}

// How far along d (a unit) the line from o meets the triangle, either face,
// or -1
static f32 slotRayTri(const f32 *o, const f32 *d, const f32 v0[3], const f32 v1[3], const f32 v2[3])
{
	const f32 e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
	const f32 e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
	const f32 p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
	const f32 det = dot3(e1, p);
	f32 tv[3], q[3], u, v, inv;

	if (fabsf(det) < 1e-6f) {
		return -1.0f;
	}

	inv = 1.0f / det;
	tv[0] = o[0] - v0[0];
	tv[1] = o[1] - v0[1];
	tv[2] = o[2] - v0[2];
	u = dot3(tv, p) * inv;

	if (u < -1e-4f || u > 1.0001f) {
		return -1.0f;
	}

	q[0] = tv[1] * e1[2] - tv[2] * e1[1];
	q[1] = tv[2] * e1[0] - tv[0] * e1[2];
	q[2] = tv[0] * e1[1] - tv[1] * e1[0];
	v = dot3(d, q) * inv;

	if (v < -1e-4f || u + v > 1.0001f) {
		return -1.0f;
	}

	return dot3(e2, q) * inv;
}

// A point's picture on a triangle's plane, its mapping carried past its edges
static void slotUvAt(const struct stri *t, const f32 *p, f32 *uv)
{
	const f32 delta[3] = { p[0] - t->pos[0][0], p[1] - t->pos[0][1], p[2] - t->pos[0][2] };
	f32 duv[2];

	triUvShift(t, delta, duv);
	uv[0] = t->uv[0][0] + duv[0];
	uv[1] = t->uv[0][1] + duv[1];
}

// A rectangle of wall a hair in front of a door's face, u0 to u1 across it
// and a0 to a1 up its rise, in the donor's picture, facing out along -d
static s32 slotPatch(struct collect *c, const struct doorbox *b, s32 t, s32 w, s32 a, s32 face,
		const struct stri *donor, const f32 *d, f32 u0, f32 u1, f32 a0, f32 a1)
{
	struct stri patch = *donor;
	f32 corner[4][3];
	f32 nrm[3];
	u32 argb = donor->argb[0] & 0xff000000;
	s32 added = 0;

	for (s32 k = 0; k < 4; k++) {
		const f32 cu = k == 1 || k == 2 ? u1 : u0;
		const f32 ca = k >= 2 ? a1 : a0;

		for (s32 j = 0; j < 3; j++) {
			corner[k][j] = b->mid[j] + face * (b->half[t] + SLOT_LIFT) * b->axis[t][j]
				+ cu * b->axis[w][j] + b->slide[a] * ca * b->axis[a][j];
		}
	}

	for (s32 ch = 0; ch < 3; ch++) {
		u32 sum = 0;

		for (s32 k = 0; k < 3; k++) {
			sum += (donor->argb[k] >> (ch * 8)) & 0xff;
		}

		argb |= (sum / 3) << (ch * 8);
	}

	patch.decal = 0;
	patch.lift = 0;
	patch.sink = 0;
	patch.decalbase = -1;
	patch.backed = 0;
	patch.fights = 0;

	for (s32 half = 0; half < 2; half++) {
		const s32 idx[3] = { 0, half ? 2 : 1, half ? 3 : 2 };

		for (s32 k = 0; k < 3; k++) {
			memcpy(patch.pos[k], corner[idx[k]], sizeof(f32) * 3);
			slotUvAt(donor, patch.pos[k], patch.uv[k]);
			patch.argb[k] = argb;
		}

		// facing the way the donor faces, out of the door
		if (triNormalWork(&patch, nrm) > 0.0f && dot3(nrm, d) > 0.0f) {
			const struct stri swap = patch;

			for (s32 j = 0; j < 3; j++) {
				patch.pos[1][j] = swap.pos[2][j];
				patch.pos[2][j] = swap.pos[1][j];
			}

			for (s32 j = 0; j < 2; j++) {
				patch.uv[1][j] = swap.uv[2][j];
				patch.uv[2][j] = swap.uv[1][j];
			}
		}

		added += addTri(c, &patch);
	}

	return added;
}

// Bean's wall on one side of a door: the nearest triangle facing out that
// way, beside or over the doorway, in front of the door's middle
static const struct stri *slotDonor(const struct collect *c, const struct doorbox *b, const s32 *near, s32 numnear,
		s32 t, s32 w, s32 a, s32 face, const f32 *d, f32 extent)
{
	const struct stri *donor = NULL;
	f32 donordist = 1e30f;

	for (s32 q = 0; q < numnear; q++) {
		const struct stri *tri = &c->tris[near[q]];
		f32 nrm[3], mid[3], at[3];
		f32 dw, da, dist;

		// a cut-out is no wall: Depot's "C5" stencil over a warehouse door
		// was taken, and the raised door showed through its holes
		if (texHasAlpha(tri->tex) || texIsXlu(tri->tex) || triNormal(tri, nrm) <= 0.0f || dot3(nrm, d) > -0.9f) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f - b->mid[j];
		}

		for (s32 k = 0; k < 3; k++) {
			at[k] = dot3(mid, b->axis[k]);
		}

		if (at[t] * face < 0.0f || at[t] * face > extent || at[a] * b->slide[a] < b->half[a] * 0.5f) {
			continue;
		}

		dw = MAX(0.0f, fabsf(at[w]) - b->half[w]);
		da = fabsf(at[a] * b->slide[a] - (b->half[a] + b->travel[a] * 0.5f));
		dist = dw * dw + da * da;

		if (dist < donordist) {
			donordist = dist;
			donor = tri;
		}
	}

	return donor;
}

static s32 fillDoorSlots(struct collect *c, u8 **filerooms, u32 *filelens, s32 n)
{
	struct doorbox *boxes = NULL;
	const s32 num = doorBoxes(&boxes);
	const s32 numtris = c->num;
	s32 patched = 0;
	s32 doors = 0;

	if (num <= 0 || !filerooms || !filelens) {
		free(boxes);
		return 0;
	}

	for (s32 i = 0; i < num; i++) {
		const struct doorbox *b = &boxes[i];
		const s32 t = b->thin;
		const s32 before = patched;
		struct slotge ge = { NULL, 0, 0 };
		s32 *near;
		s32 numnear = 0;
		s32 a = -1, w;
		s32 nw, na;
		f32 cw, ca, extent;

		for (s32 k = 0; k < 3; k++) {
			if (k != t && b->slide[k] && b->travel[k] > 1.0f) {
				a = k;
			}
		}

		if (a < 0) {
			continue;
		}

		w = 3 - t - a;
		extent = b->half[t] + SLOT_OUT;

		// The band's box, both faces' lines and all
		for (s32 j = 0; j < 3; j++) {
			f32 lo = 1e30f, hi = -1e30f;

			for (s32 corner = 0; corner < 8; corner++) {
				f32 ext[3];
				f32 v = b->mid[j];

				ext[t] = (corner & 1 ? 1 : -1) * extent;
				ext[w] = (corner & 2 ? 1 : -1) * b->half[w];
				ext[a] = b->slide[a] * (corner & 4 ? b->half[a] + b->travel[a] : b->half[a]);

				for (s32 k = 0; k < 3; k++) {
					v += ext[k] * b->axis[k][j];
				}

				lo = MIN(lo, v);
				hi = MAX(hi, v);
			}

			ge.lo[j] = lo - 1.0f;
			ge.hi[j] = hi + 1.0f;
		}

		for (s32 r = 1; r < n; r++) {
			if (filerooms[r]) {
				fileRoomTrianglesEach(r, filerooms[r], filelens[r], 0, fileTriToSlot, &ge);
			}
		}

		near = ge.num ? malloc(sizeof(*near) * MAX(numtris, 1)) : NULL;

		for (s32 q = 0; near && q < numtris; q++) {
			const struct stri *tri = &c->tris[q];
			s32 in = !tri->decal && !tri->blend;

			for (s32 j = 0; j < 3 && in; j++) {
				in = MAX(tri->pos[0][j], MAX(tri->pos[1][j], tri->pos[2][j])) >= ge.lo[j] - SLOT_OUT
					&& MIN(tri->pos[0][j], MIN(tri->pos[1][j], tri->pos[2][j])) <= ge.hi[j] + SLOT_OUT;
			}

			if (in) {
				near[numnear++] = q;
			}
		}

		nw = MAX(1, (s32)ceilf(b->half[w] * 2.0f / SLOT_CELL));
		na = MAX(1, (s32)ceilf(b->travel[a] / SLOT_CELL));
		cw = b->half[w] * 2.0f / nw;
		ca = b->travel[a] / na;

		for (s32 face = -1; near && face <= 1; face += 2) {
			const struct stri *found;
			struct stri donor;
			const s32 facebefore = patched;
			s32 cells = 0, gecov = 0, hdcov = 0;
			u8 *need;
			u8 *hdfaces;
			f32 d[3];

			for (s32 j = 0; j < 3; j++) {
				d[j] = -face * b->axis[t][j];
			}

			found = slotDonor(c, b, near, numnear, t, w, a, face, d, extent);

			if (!found) {
				continue;
			}

			// a copy: the patches added below can move the array
			donor = *found;
			need = calloc((size_t)nw * na, 1);
			hdfaces = calloc(MAX(numnear, 1), 1);

			for (s32 q = 0; hdfaces && q < numnear; q++) {
				f32 nrm[3];

				hdfaces[q] = !texHasAlpha(c->tris[near[q]].tex) && triNormal(&c->tris[near[q]], nrm) > 0.0f && dot3(nrm, d) < 0.0f;
			}

			if (!hdfaces) {
				free(need);
				need = NULL;
			}

			for (s32 iw = 0; need && iw < nw; iw++) {
				for (s32 ia = 0; ia < na; ia++) {
					const f32 u0 = -b->half[w] + (iw + 0.5f) * cw;
					const f32 a0 = b->half[a] + (ia + 0.5f) * ca;
					f32 besthd = 1e30f, bestge = 1e30f;
					f32 o[3];

					for (s32 j = 0; j < 3; j++) {
						o[j] = b->mid[j] + face * extent * b->axis[t][j] + u0 * b->axis[w][j]
							+ b->slide[a] * a0 * b->axis[a][j];
					}

					cells++;

					for (s32 q = 0; q < ge.num; q++) {
						const f32 h = slotRayTri(o, d, ge.tris[q][0], ge.tris[q][1], ge.tris[q][2]);

						if (h >= 0.0f && h < bestge) {
							bestge = h;
						}
					}

					// GoldenEye has wall here, in the door's thickness or in
					// front of it
					if (bestge > SLOT_OUT + b->half[t] * 2.0f + SLOT_FACE) {
						continue;
					}

					gecov++;

					// Bean's faces this way: one turned away is culled
					for (s32 q = 0; q < numnear; q++) {
						if (hdfaces[q]) {
							const struct stri *tri = &c->tris[near[q]];
							const f32 h = slotRayTri(o, d, tri->pos[0], tri->pos[1], tri->pos[2]);

							if (h >= 0.0f && h < besthd) {
								besthd = h;
							}
						}
					}

					// and Bean's is not in front of the door's face
					if (besthd <= SLOT_OUT + SLOT_FACE) {
						need[iw * na + ia] = 3;
						hdcov++;
						continue;
					}

					need[iw * na + ia] = 1;
				}
			}

			// A cell Bean covers beside one it does not is covered only as
			// far as its middle, where it was looked at: Depot's beam over
			// the doorway left the raised door's top edge showing under it.
			// The wall goes on under the cover, which is in front of it.
			for (s32 iw = 0; need && iw < nw; iw++) {
				for (s32 ia = 0; ia < na; ia++) {
					if (need[iw * na + ia] == 3
							&& ((iw > 0 && need[(iw - 1) * na + ia] == 1) || (iw + 1 < nw && need[(iw + 1) * na + ia] == 1)
								|| (ia > 0 && need[iw * na + ia - 1] == 1) || (ia + 1 < na && need[iw * na + ia + 1] == 1))) {
						need[iw * na + ia] = 4;
					}
				}
			}

			for (s32 q = 0; need && q < nw * na; q++) {
				if (need[q] == 4) {
					need[q] = 1;
				}
			}

			// The cells as few rectangles: a column's run, carried over the
			// columns after it that need the same run
			for (s32 iw = 0; need && iw < nw; iw++) {
				for (s32 ia = 0; ia < na; ia++) {
					s32 top = ia, right = iw;

					if (need[iw * na + ia] != 1) {
						continue;
					}

					while (top + 1 < na && need[iw * na + top + 1] == 1) {
						top++;
					}

					while (right + 1 < nw) {
						s32 same = 1;

						for (s32 k = ia; k <= top && same; k++) {
							same = need[(right + 1) * na + k] == 1;
						}

						if (!same) {
							break;
						}

						right++;
					}

					for (s32 x = iw; x <= right; x++) {
						for (s32 k = ia; k <= top; k++) {
							need[x * na + k] = 2;
						}
					}

					patched += slotPatch(c, b, t, w, a, face, &donor, d,
							-b->half[w] + iw * cw, -b->half[w] + (right + 1) * cw,
							b->half[a] + ia * ca, b->half[a] + (top + 1) * ca);
				}
			}

			// What Bean painted on its wall inside the band - Depot's "C5"
			// over the warehouse door - comes forward with it
			if (patched > facebefore) {
				for (s32 q = 0; q < numnear; q++) {
					struct stri copy = c->tris[near[q]];
					f32 nrm[3];
					f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
					f32 by;

					if (copy.tex == donor.tex || triNormal(&c->tris[near[q]], nrm) <= 0.0f || dot3(nrm, d) > -0.9f) {
						continue;
					}

					for (s32 k = 0; k < 3; k++) {
						f32 rel[3];

						for (s32 j = 0; j < 3; j++) {
							rel[j] = copy.pos[k][j] - b->mid[j];
						}

						for (s32 x = 0; x < 3; x++) {
							const f32 at = dot3(rel, b->axis[x]) * (x == t ? face : x == a ? b->slide[a] : 1);

							lo[x] = MIN(lo[x], at);
							hi[x] = MAX(hi[x], at);
						}
					}

					if (lo[w] < -b->half[w] - SLOT_FACE || hi[w] > b->half[w] + SLOT_FACE
							|| lo[a] < b->half[a] - SLOT_FACE || hi[a] > b->half[a] + b->travel[a] + SLOT_FACE
							|| lo[t] < -b->half[t] || hi[t] > b->half[t] + SLOT_LIFT) {
						continue;
					}

					by = b->half[t] + SLOT_LIFT + SLOT_PAINT - (lo[t] + hi[t]) * 0.5f;

					for (s32 k = 0; k < 3; k++) {
						for (s32 j = 0; j < 3; j++) {
							copy.pos[k][j] += face * by * b->axis[t][j];
						}
					}

					patched += addTri(c, &copy);
				}
			}

			free(need);
			free(hdfaces);

			if (gecov > hdcov) {
				sysLogPrintf(LOG_NOTE, "gebeanstage: door at %.0f %.0f %.0f, side %d (%.2f %.2f %.2f), going (%.2f %.2f %.2f): GoldenEye's wall over %d of %d cells of its rise, Bean's in front of it at %d",
						b->mid[0], b->mid[1], b->mid[2], face, face * b->axis[t][0], face * b->axis[t][1], face * b->axis[t][2],
						b->slide[a] * b->axis[a][0], b->slide[a] * b->axis[a][1], b->slide[a] * b->axis[a][2],
						gecov, cells, hdcov);
			}
		}

		if (patched > before) {
			sysLogPrintf(LOG_NOTE, "gebeanstage: door at %.0f %.0f %.0f going (%.2f %.2f %.2f) %.0f, through (%.2f %.2f %.2f): %d triangles of wall",
					b->mid[0], b->mid[1], b->mid[2],
					b->slide[a] * b->axis[a][0], b->slide[a] * b->axis[a][1], b->slide[a] * b->axis[a][2], b->travel[a],
					b->axis[t][0], b->axis[t][1], b->axis[t][2], patched - before);
			doors++;
		}

		free(near);
		free(ge.tris);
	}

	free(boxes);

	if (patched) {
		sysLogPrintf(LOG_NOTE, "gebeanstage: %d triangles of wall over %d raised doors, where GoldenEye's wall hides them", patched, doors);
	}

	return patched;
}

/**
 * GoldenEye's own moving water (gewater.c) under Bean's pictures that lie on
 * it. Bean draws Dam's reservoir with a water shader of its own (stride 36,
 * gebeanLevelTextureIsWater()), but Caverns' pools are plain pictures, which
 * the release too moves in a shader of its own, not drawn here; they stood
 * still (F3 20260928-082038). A picture of Bean's is taken for water when most
 * of its triangles lie on GoldenEye's water (the two pictures its loader
 * moves), so it moves as the N64 look's water does, on every level.
 */
#define WATER_CELL 1024.0f
#define WATER_NEAR 64.0f
// GoldenEye's water on vertices no brighter than this is drawn black
#define WATER_BLACK 8

struct watermeasure {
	struct tgrid grid;
	// how the picture's s and t (rows) change across the ground (x, z), in
	// quarter texels a unit, summed over the triangles weighted by their area
	f64 grad[2][2];
	f64 area;
};

/**
 * Adds a triangle's gradient of two values across the ground (x and z) to
 * m's sums, weighted by its area on the ground. A wall adds nothing.
 */
static void waterGrad(struct watermeasure *m, const f32 v[3][3], const f32 f[3][2])
{
	const f32 ax = v[1][0] - v[0][0], az = v[1][2] - v[0][2];
	const f32 bx = v[2][0] - v[0][0], bz = v[2][2] - v[0][2];
	const f32 det = ax * bz - az * bx;
	const f32 area = fabsf(det) * 0.5f;

	if (area < 1.0f) {
		return;
	}

	for (s32 j = 0; j < 2; j++) {
		const f32 da = f[1][j] - f[0][j], db = f[2][j] - f[0][j];

		m->grad[j][0] += (da * bz - db * az) / det * area;
		m->grad[j][1] += (db * ax - da * bx) / det * area;
	}

	m->area += area;
}

static void fileTriToWater(void *arg, const f32 v[3][3], s32 room)
{
	struct watermeasure *m = arg;

	// GoldenEye shades its water by the vertices, and Complex's pits are its
	// reservoir picture on black ones: a black floor whose motion nobody sees.
	// Bean paints them a plain rusty floor, which crept like water under
	// GoldenEye's motion (F3 20260928-170819), so water shaded black is none
	if (eachShade[0] <= WATER_BLACK && eachShade[1] <= WATER_BLACK && eachShade[2] <= WATER_BLACK) {
		return;
	}

	if (eachTex == GEWATER_TEX_CAVERNS || eachTex == GEWATER_TEX_DAM) {
		// the file's s and t are 32nds of a texel (s10.5), so an eighth of
		// them is a quarter texel, the unit GoldenEye moves its water in
		f32 f[3][2];

		for (s32 k = 0; k < 3; k++) {
			f[k][0] = eachSt[k][0] / 8.0f;
			f[k][1] = eachSt[k][1] / 8.0f;
		}

		tgridAdd(&m->grid, v, room & 0xffff);
		waterGrad(m, v, (const f32 (*)[2])f);
	}
}

// What the build last handed gewater.c, for the cache to hand it again
static s32 waterMeasured;
static f32 waterRate[2];

static void setWaterRates(s32 measured, f32 s, f32 t)
{
	waterMeasured = measured;
	waterRate[0] = s;
	waterRate[1] = t;
	geWaterSetHdRates(measured, s, t);
}

static s32 markWaterPictures(const struct collect *c, u8 **filerooms, u32 *filelens, s32 n)
{
	struct watermeasure ge, hd;
	struct tgrid *const water = &ge.grid;
	s32 *on, *all;
	s32 marked = 0;
	f32 at[GEBEAN_MAXMATS][3];

	memset(&ge, 0, sizeof(ge));
	memset(&hd, 0, sizeof(hd));
	setWaterRates(0, 0, 0);

	if (!tgridInit(water, WATER_CELL)) {
		return 0;
	}

	for (s32 r = 1; r < n; r++) {
		if (filerooms[r]) {
			fileRoomTrianglesEach(r, filerooms[r], filelens[r], 1, fileTriToWater, &ge);
		}
	}

	on = calloc(GEBEAN_MAXMATS, sizeof(*on));
	all = calloc(GEBEAN_MAXMATS, sizeof(*all));

	if (water->numtri && on && all) {
		for (s32 t = 0; t < c->num; t++) {
			const struct stri *tri = &c->tris[t];
			f32 mid[3], d;

			if (tri->tex < 0 || tri->tex >= GEBEAN_MAXMATS) {
				continue;
			}

			for (s32 j = 0; j < 3; j++) {
				mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
			}

			all[tri->tex]++;

			if (tgridNearest(water, mid, 1, &d) >= 0 && d < WATER_NEAR * WATER_NEAR) {
				if (on[tri->tex]++ == 0) {
					memcpy(at[tri->tex], mid, sizeof(mid));
				}
			}
		}

		for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
			if (!texWater[t] && on[t] >= 2 && on[t] * 2 >= all[t]) {
				texWater[t] = 1;
				marked++;
				sysLogPrintf(LOG_NOTE, "gebeanstage: %s: picture %d (%s) moves as GoldenEye's water, %d of its %d triangles on it (one at %.0f %.0f %.0f)",
						row->bean, t, gebeanLevelTextureName(level, t), on[t], all[t], at[t][0], at[t][1], at[t][2]);
			}
		}
	}

	// How Bean's water pictures lie on the ground against GoldenEye's: its UV
	// is in repeats of the picture, drawn as a stand-in tile of XBLATEX_TILE
	// texels, t running the other way (writeRoom())
	for (s32 t = 0; t < c->num && water->numtri; t++) {
		const struct stri *tri = &c->tris[t];
		f32 mid[3], d, f[3][2];

		if (tri->tex < 0 || tri->tex >= GEBEAN_MAXMATS || !texWater[tri->tex]) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
		}

		if (tgridNearest(water, mid, 1, &d) < 0 || d >= WATER_NEAR * WATER_NEAR) {
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			f[k][0] = tri->uv[k][0] * XBLATEX_TILE * 4;
			f[k][1] = -tri->uv[k][1] * XBLATEX_TILE * 4;
		}

		waterGrad(&hd, (const f32 (*)[3])tri->pos, (const f32 (*)[2])f);
	}

	// GoldenEye moves its tile 0.25 of a quarter texel a frame in s and 0.1
	// in t: the same motion across the ground, in Bean's picture
	if (ge.area > 0 && hd.area > 0) {
		f64 g[2][2], b[2][2], det, dx, dz, rs, rt;

		for (s32 j = 0; j < 2; j++) {
			for (s32 k = 0; k < 2; k++) {
				g[j][k] = ge.grad[j][k] / ge.area;
				b[j][k] = hd.grad[j][k] / hd.area;
			}
		}

		det = g[0][0] * g[1][1] - g[0][1] * g[1][0];

		if (fabs(det) > 1e-9) {
			dx = (0.25 * g[1][1] - 0.1 * g[0][1]) / det;
			dz = (0.1 * g[0][0] - 0.25 * g[1][0]) / det;
			rs = b[0][0] * dx + b[0][1] * dz;
			rt = b[1][0] * dx + b[1][1] * dz;

			setWaterRates(1, (f32)rs, (f32)rt);

			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: water moves %.3f %.3f units a frame, in Bean's picture %.3f %.3f quarter texels (GoldenEye's s/t %.4f %.4f %.4f %.4f, Bean's %.4f %.4f %.4f %.4f a unit)",
					row->bean, dx, dz, rs, rt, g[0][0], g[0][1], g[1][0], g[1][1], b[0][0], b[0][1], b[1][0], b[1][1]);
		}
	}

	free(on);
	free(all);
	tgridFree(water);

	return marked;
}

/* -------------------------------------------------------------------------
 * The built level, kept on disk
 *
 * Building the release's mesh into the rooms takes a second or more a load
 * (Dam 1.6 s, Frigate 2.5 s) and comes out the same every time for the same
 * inputs, so what it leaves behind is written to
 * cache/xbla/goldeneye/levels/<key>_<file>[_ce].bin and read back instead.
 *
 * The key is a hash of everything the build reads: HDCACHE_VERSION (bump it
 * on ANY change to what build() or the functions it calls produce, here or
 * in gebean.c's level walk and pictures), the binary's commit, the struct
 * layouts, the level's table row, the release's level file byte for
 * byte (which is also what the Community Edition's overlay changes), whether
 * that overlay is on, the pictures' alpha as bound, the level file's rooms
 * (their bytes, bases, positions and boxes), the doors' boxes and the
 * environment's water. No setting is read by the build. A file whose header,
 * length, key or checksum is wrong is built over and rewritten.
 *
 * Nothing written holds a host address: rooms carry the file's own segment
 * addresses (g_BgRooms[].unk00 plus offsets, and the bases are in the key),
 * which bg.c relocates as it does a room read from the file; the backdrop is
 * plain triangles. The camera test's shell is GoldenEye's own rooms and is
 * taken again each load.
 * ------------------------------------------------------------------------- */

#define HDCACHE_VERSION 12
#define HDCACHE_MAGIC "GEHDLVL"

struct hdcachehead {
	char magic[8];
	u32 version;
	u32 headsize;
	u64 key;
	u64 paylen;
	u64 paysum;
};

static u64 hashMix(u64 h)
{
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;

	return h;
}

static u64 hashBytes(u64 h, const void *data, size_t len)
{
	const u8 *p = data;
	u64 a = h ^ 0x9e3779b97f4a7c15ull, b = h + len;

	// two lanes, so that the multiplies overlap
	while (len >= 16) {
		u64 k0, k1;

		memcpy(&k0, p, 8);
		memcpy(&k1, p + 8, 8);
		a = (a ^ (k0 * 0x87c37b91114253d5ull)) * 0x4cf5ad432745937full;
		b = (b ^ (k1 * 0x4cf5ad432745937full)) * 0x87c37b91114253d5ull;
		a = (a << 29) | (a >> 35);
		b = (b << 31) | (b >> 33);
		p += 16;
		len -= 16;
	}

	while (len > 0) {
		a = (a ^ *p++) * 0x100000001b3ull;
		len--;
	}

	return hashMix(a ^ hashMix(b));
}

static u64 hashU32(u64 h, u32 v)
{
	return hashBytes(h, &v, sizeof(v));
}

static u64 hashStr(u64 h, const char *str)
{
	return hashBytes(h, str ? str : "", str ? strlen(str) + 1 : 1);
}

static u64 hdcacheKey(const char *levelname, u8 **filerooms, u32 *filelens, s32 n)
{
	const u32 layout[] = {
		HDCACHE_VERSION, sizeof(struct stri), sizeof(struct hdcachehead), GEBEAN_MAXMATS,
		sizeof(void *), 0x01020304,
	};
	struct environment *env = envGetCurrent();
	struct doorbox *boxes = NULL;
	const u8 *file;
	u32 filelen;
	s32 numboxes;
	u64 h = hashBytes(0, layout, sizeof(layout));

#ifdef VERSION_HASH
	// and the binary's own commit, so that a released build never serves a
	// level an older one built, whether or not the version above was bumped
	h = hashStr(h, VERSION_HASH);
#endif
	h = hashStr(h, row->key);
	h = hashStr(h, row->bean);
	h = hashBytes(h, &row->scale, sizeof(row->scale));
	h = hashBytes(h, row->offset, sizeof(row->offset));
	h = hashStr(h, levelname);
	h = hashU32(h, gebeanCeIsActive());

	file = gebeanLevelFileBytes(level, &filelen);
	h = hashU32(h, filelen);
	h = hashBytes(h, file, filelen);

	h = hashU32(h, gebeanLevelNumTextures(level));
	h = hashBytes(h, texAlpha, sizeof(texAlpha));
	h = hashBytes(h, texSoft, sizeof(texSoft));

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		h = hashU32(h, texTile[t] != NULL);
	}

	h = hashU32(h, n);

	for (s32 r = 0; r <= n; r++) {
		h = hashU32(h, (u32)g_BgRooms[r].unk00);
	}

	for (s32 r = 1; r < n; r++) {
		h = hashBytes(h, &g_BgRooms[r].pos, sizeof(g_BgRooms[r].pos));
		h = hashBytes(h, g_Rooms[r].bbmin, sizeof(g_Rooms[r].bbmin));
		h = hashBytes(h, g_Rooms[r].bbmax, sizeof(g_Rooms[r].bbmax));
		h = hashU32(h, filelens[r]);

		if (filerooms[r]) {
			h = hashBytes(h, filerooms[r], filelens[r]);
		}
	}

	numboxes = doorBoxes(&boxes);
	h = hashU32(h, numboxes);

	for (s32 i = 0; i < numboxes; i++) {
		// what doorBoxes() fills; the rest is the build's own
		h = hashBytes(h, &boxes[i], offsetof(struct doorbox, slide) + sizeof(boxes[i].slide));
	}

	free(boxes);

	h = hashU32(h, env != NULL);

	if (env) {
		h = hashU32(h, env->water_enabled);
		h = hashBytes(h, &env->water_scale, sizeof(env->water_scale));
		h = hashU32(h, (u32)env->water_type);
		h = hashBytes(h, &env->water_r, sizeof(env->water_r));
		h = hashBytes(h, &env->water_g, sizeof(env->water_g));
		h = hashBytes(h, &env->water_b, sizeof(env->water_b));
	}

	return h;
}

static s32 hdcachePath(char *dst, u32 dstLen, const char *levelname, s32 temp)
{
	char dir[FS_MAXPATH + 1];

	if (!gebeanGetCacheDir(dir, sizeof(dir))) {
		return 0;
	}

	snprintf(dst, dstLen, "%s/levels", dir);

	if (fsFileSize(dst) < 0) {
		fsCreateDir(dst);
	}

	snprintf(dst, dstLen, "%s/levels/%s_%s%s.bin%s", dir, row->key, levelname,
			gebeanCeIsActive() ? "_ce" : "", temp ? ".tmp" : "");

	return 1;
}

struct hdbuf {
	u8 *d;
	size_t len, cap;
	s32 bad;
};

static void hdPut(struct hdbuf *b, const void *src, size_t len)
{
	if (b->bad || len == 0) {
		return;
	}

	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap : 1 << 20;
		u8 *d;

		while (cap < b->len + len) {
			cap *= 2;
		}

		d = realloc(b->d, cap);

		if (!d) {
			b->bad = 1;
			return;
		}

		b->d = d;
		b->cap = cap;
	}

	memcpy(b->d + b->len, src, len);
	b->len += len;
}

static void hdPut32(struct hdbuf *b, u32 v)
{
	hdPut(b, &v, sizeof(v));
}

// A triangle field by field, so that no padding (whatever the build's copies
// left in it) reaches the file
#define HD_TRI_FIELDS(OP, t) \
	OP(t->pos) OP(t->uv) OP(t->argb) OP(t->tex) OP(t->room) OP(t->decal) OP(t->lift) OP(t->sink) \
	OP(t->decalbase) OP(t->nofog) OP(t->backed) OP(t->fights) OP(t->blend) OP(t->undersea) OP(t->plain)

static void hdPutTri(struct hdbuf *b, const struct stri *t)
{
#define HD_PUT(f) hdPut(b, &(f), sizeof(f));
	HD_TRI_FIELDS(HD_PUT, t)
#undef HD_PUT
}

static void hdcacheSave(u64 key, const char *levelname)
{
	char path[FS_MAXPATH + 1];
	char tmp[FS_MAXPATH + 1];
	struct hdcachehead head;
	struct hdbuf b = { 0 };
	FILE *fp;
	s32 ok;

	if (!hdcachePath(path, sizeof(path), levelname, 0) || !hdcachePath(tmp, sizeof(tmp), levelname, 1)) {
		return;
	}

	hdPut32(&b, numRooms);
	hdPut32(&b, numServed);
	hdPut32(&b, numHidden);
	hdPut32(&b, numWaterKept);
	hdPut32(&b, levelHasWater);
	hdPut32(&b, numBackdrop);
	hdPut32(&b, waterMeasured);
	hdPut(&b, waterRate, sizeof(waterRate));
	hdPut(&b, meshMin, sizeof(meshMin));
	hdPut(&b, meshMax, sizeof(meshMax));
	hdPut(&b, backdropMid, sizeof(backdropMid));
	hdPut(&b, texClampV, sizeof(texClampV));
	hdPut(&b, texClampShift, sizeof(texClampShift));
	hdPut(&b, texWater, sizeof(texWater));
	hdPut(&b, roomHidden, numRooms + 1);
	hdPut(&b, roomWaterKept, numRooms + 1);

	for (s32 r = 0; r <= numRooms; r++) {
		hdPut32(&b, roomData[r] ? 1 : 0);
		hdPut32(&b, roomLen[r]);
	}

	for (s32 r = 0; r <= numRooms; r++) {
		if (roomData[r]) {
			hdPut(&b, roomData[r], roomLen[r]);
		}
	}

	for (s32 t = 0; t < numBackdrop; t++) {
		hdPutTri(&b, &backdrop[t]);
	}

	if (b.bad) {
		free(b.d);
		return;
	}

	memset(&head, 0, sizeof(head));
	memcpy(head.magic, HDCACHE_MAGIC, sizeof(HDCACHE_MAGIC));
	head.version = HDCACHE_VERSION;
	head.headsize = sizeof(head);
	head.key = key;
	head.paylen = b.len;
	head.paysum = hashBytes(key, b.d, b.len);

	fp = fopen(tmp, "wb");
	ok = fp && fwrite(&head, sizeof(head), 1, fp) == 1 && fwrite(b.d, 1, b.len, fp) == b.len;

	if (fp && fclose(fp) != 0) {
		ok = 0;
	}

	free(b.d);

	if (!ok || fsReplaceFile(tmp, path) != 0) {
		sysLogPrintf(LOG_WARNING, "gebeanstage: could not keep the built level in %s", path);
		fsRemoveFile(tmp);
		return;
	}

	sysLogPrintf(LOG_NOTE, "gebeanstage: %s: built level kept in %s (%u bytes)", row->bean, path, (u32)(sizeof(head) + b.len));
}

struct hdread {
	const u8 *d;
	size_t len, at;
	s32 bad;
};

static const void *hdTake(struct hdread *r, size_t len)
{
	const void *p;

	if (r->bad || len > r->len - r->at) {
		r->bad = 1;
		return NULL;
	}

	p = r->d + r->at;
	r->at += len;

	return p;
}

static void hdGet(struct hdread *r, void *dst, size_t len)
{
	const void *p = hdTake(r, len);

	if (p) {
		memcpy(dst, p, len);
	} else {
		memset(dst, 0, len);
	}
}

static void hdGetTri(struct hdread *r, struct stri *t)
{
	memset(t, 0, sizeof(*t));
#define HD_GET(f) hdGet(r, &(f), sizeof(f));
	HD_TRI_FIELDS(HD_GET, t)
#undef HD_GET
}

static u32 hdGet32(struct hdread *r)
{
	u32 v;

	hdGet(r, &v, sizeof(v));

	return v;
}

/**
 * The built level from the cache, into the state build() leaves. 0, with
 * nothing kept, when there is none or it is not this level's as it is now.
 */
static s32 hdcacheLoad(u64 key, const char *levelname, s32 n)
{
	char path[FS_MAXPATH + 1];
	struct hdcachehead head;
	struct hdread rd = { 0 };
	u8 *data = NULL;
	u8 *present = NULL;
	FILE *fp;
	long size = 0;
	const char *why = NULL;

	if (!hdcachePath(path, sizeof(path), levelname, 0)) {
		return 0;
	}

	fp = fopen(path, "rb");

	if (!fp) {
		return 0;
	}

	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < (long)sizeof(head) || size > 512L * 1024 * 1024) {
		why = "a size that is not a built level's";
	} else if (fseek(fp, 0, SEEK_SET) != 0 || fread(&head, sizeof(head), 1, fp) != 1) {
		why = "an unreadable header";
	} else if (memcmp(head.magic, HDCACHE_MAGIC, sizeof(HDCACHE_MAGIC)) != 0 || head.headsize != sizeof(head)) {
		why = "a header that is not a built level's";
	} else if (head.version != HDCACHE_VERSION) {
		why = "an older builder's level";
	} else if (head.key != key) {
		why = "a level built from other inputs";
	} else if (head.paylen != (u64)size - sizeof(head)) {
		why = "a length that is not its header's (cut short?)";
	} else if ((data = malloc(head.paylen ? head.paylen : 1)) == NULL
			|| fread(data, 1, head.paylen, fp) != head.paylen) {
		why = "a body that could not be read";
	} else if (hashBytes(key, data, head.paylen) != head.paysum) {
		why = "a checksum that does not match";
	}

	fclose(fp);

	if (why) {
		sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %s has %s, building it again", row->bean, path, why);
		free(data);
		return 0;
	}

	rd.d = data;
	rd.len = head.paylen;

	numRooms = hdGet32(&rd);
	numServed = hdGet32(&rd);
	numHidden = hdGet32(&rd);
	numWaterKept = hdGet32(&rd);
	levelHasWater = hdGet32(&rd);
	numBackdrop = hdGet32(&rd);
	waterMeasured = hdGet32(&rd);
	hdGet(&rd, waterRate, sizeof(waterRate));
	hdGet(&rd, meshMin, sizeof(meshMin));
	hdGet(&rd, meshMax, sizeof(meshMax));
	hdGet(&rd, backdropMid, sizeof(backdropMid));
	hdGet(&rd, texClampV, sizeof(texClampV));
	hdGet(&rd, texClampShift, sizeof(texClampShift));
	hdGet(&rd, texWater, sizeof(texWater));

	if (numRooms != n || numBackdrop < 0 || numBackdrop > 16 * 1024 * 1024) {
		rd.bad = 1;
	}

	if (!rd.bad) {
		roomData = calloc(n + 1, sizeof(*roomData));
		roomLen = calloc(n + 1, sizeof(*roomLen));
		roomHidden = calloc(n + 1, sizeof(*roomHidden));
		roomWaterKept = calloc(n + 1, sizeof(*roomWaterKept));
		present = calloc(n + 1, 1);

		if (!roomData || !roomLen || !roomHidden || !roomWaterKept || !present) {
			rd.bad = 1;
		}
	}

	if (!rd.bad) {
		hdGet(&rd, roomHidden, n + 1);
		hdGet(&rd, roomWaterKept, n + 1);

		for (s32 r = 0; r <= n; r++) {
			present[r] = hdGet32(&rd) != 0;
			roomLen[r] = hdGet32(&rd);
		}

		for (s32 r = 0; r <= n && !rd.bad; r++) {
			if (present[r]) {
				const void *src = hdTake(&rd, roomLen[r]);

				roomData[r] = src ? malloc(roomLen[r] ? roomLen[r] : 1) : NULL;

				if (!roomData[r]) {
					rd.bad = 1;
					break;
				}

				memcpy(roomData[r], src, roomLen[r]);
			}
		}
	}

	if (!rd.bad && numBackdrop) {
		const struct stri *t = backdrop;
		size_t each = 0;

#define HD_SIZE(f) each += sizeof(f);
		HD_TRI_FIELDS(HD_SIZE, t)
#undef HD_SIZE

		backdrop = (size_t)numBackdrop * each == rd.len - rd.at ? malloc(sizeof(*backdrop) * numBackdrop) : NULL;

		for (s32 t = 0; backdrop && t < numBackdrop; t++) {
			hdGetTri(&rd, &backdrop[t]);
		}

		if (!backdrop) {
			rd.bad = 1;
		}
	}

	if (!rd.bad && rd.at != rd.len) {
		rd.bad = 1;
	}

	free(data);
	free(present);

	if (rd.bad) {
		sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %s does not read as this level's, building it again", row->bean, path);
		forgetBuilt();
		return 0;
	}

	geWaterSetHdRates(waterMeasured, waterRate[0], waterRate[1]);

	return 1;
}

/* -------------------------------------------------------------------------
 * HD rooms as bright as GoldenEye's own
 *
 * The release bakes its light into the level's vertex colours, and in places
 * baked it much darker than GoldenEye's own rooms are lit: Train's corridors
 * drew at about 40% of the N64 look's brightness (F3 20260929-200831, "screen
 * is too dark in this corridor"). The release was never finished (its lighting
 * shader is never turned on), so on a level listed in brightLevels[] each
 * room's HD vertex colours are scaled so that the room comes out as bright as
 * the converted room it stands in.
 *
 * How bright a room is: over its opaque triangles, by area, the brightness
 * (Rec. 601 luma) of vertex colour times picture colour, channel by channel -
 * what the screen shows before the room's light. Both sides are measured the
 * same way: the release's pictures as bound, GoldenEye's as the port decodes
 * them. Pictures count because Train's HD wood is a darker, redder picture
 * than GoldenEye's; matching the vertex colours alone left the corridor at
 * two thirds of the N64 look.
 *
 * Each HD triangle is held against GoldenEye's triangle it lies on (the one
 * the dealing found), and one factor is found per room and picture from those
 * pairs (matchN64Brightness()), the same for every vertex it touches, so the
 * release's light and shade across a wall and its hues are kept; a channel
 * past 255 is clamped, and the factor is capped at BRIGHT_MAX_GAIN. Only a
 * room darker than GoldenEye's as a whole is changed, and nothing is ever
 * made darker. No corner is lifted past GoldenEye's own brightness under it
 * (brightCorner()), so a place the release already lit well stays as it is
 * and nothing ends brighter than the N64 look (the owner: "lets match n64").
 * GoldenEye's intensity (I4/I8) pictures are measured over every texel
 * (texpackTextureMeans()); taken over their bright texels only, Silo's
 * targets were twice what the N64 look draws (its floor grate is an I4).
 *
 * Every level measures its rooms and logs those under BRIGHT_REPORT of the
 * N64 look's brightness; only the listed levels are changed (the owner's
 * call, level by level). Turning a level on is one more key in
 * brightLevels[] plus a bump of HDCACHE_VERSION.
 * ------------------------------------------------------------------------- */

static const char *const brightLevels[] = {
	"tra",   // Train (F3 20260929-200831)
	// the owner, 2026-09-29: "turn it on for all the darker levels"
	"depo",  // Depot
	"cave",  // Caverns
	"silo",  // Silo
	"lib",   // Library
	"base",  // Basement
	"stack", // Stack
	"ref",   // Complex
	"stat",  // Statue Park
	"azt",   // Aztec
	"dest",  // Frigate
	"dam",   // Dam
	"sevx",  // Surface
	"sevxb", // Surface 2
	"pete",  // Streets
};

#define BRIGHT_REPORT   0.7f
#define BRIGHT_MAX_GAIN 4.0f
// A room with less of either surface than this (square units) is not measured
#define BRIGHT_MIN_AREA 1000.0f
// The converted level's texture numbers (the low 12 bits of its 0xc0 words)
#define BRIGHT_TEXNUMS 0x1000

static const f32 brightLuma[3] = { 0.299f, 0.587f, 0.114f };

struct brightsum {
	f64 area;
	f64 sum;
};

// The pictures' mean colours, r g b, the last entry of each the mean of those
// that could be read (standing in for one that could not)
static f32 brightHdTex[GEBEAN_MAXMATS + 1][3];
static f32 (*brightFileTex)[3];
static u8 *brightFileUsed;

static f32 triArea3(const f32 v[3][3])
{
	const f32 a[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
	const f32 b[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
	const f32 x = a[1] * b[2] - a[2] * b[1];
	const f32 y = a[2] * b[0] - a[0] * b[2];
	const f32 z = a[0] * b[1] - a[1] * b[0];

	return 0.5f * sqrtf(x * x + y * y + z * z);
}

static void fileTriUsed(void *arg, const f32 v[3][3], s32 room)
{
	if (eachTex >= 0 && eachTex < BRIGHT_TEXNUMS) {
		brightFileUsed[eachTex] = 1;
	}
}

/** How bright the file triangle fileRoomTrianglesEach() is handing over is. */
static f32 fileTriBright(s32 room)
{
	const f32 *tex = eachTex >= 0 && eachTex < BRIGHT_TEXNUMS && brightFileTex[eachTex][0] >= 0.0f
		? brightFileTex[eachTex] : brightFileTex[BRIGHT_TEXNUMS];
	f32 luma = 0.0f;

	for (s32 ch = 0; ch < 3; ch++) {
		luma += brightLuma[ch] * (eachRgb[0][ch] + eachRgb[1][ch] + eachRgb[2][ch]) / 3.0f * tex[ch] / 255.0f;
	}

	return luma;
}

static void fileTriToBright(void *arg, const f32 v[3][3], s32 room)
{
	struct brightsum *b = arg;
	const f32 area = triArea3(v);

	b->area += area;
	b->sum += area * fileTriBright(room);
}

/**
 * The brightness of each file triangle in the dealing grid (by its index
 * there), so that each HD triangle can be held against the one of
 * GoldenEye's it lies on.
 */
static f32 *gridBright;
static s32 gridBrightCap;

static void gridBrightNote(s32 index, s32 room)
{
	if (!brightFileTex) {
		return;
	}

	if (index >= gridBrightCap) {
		const s32 cap = MAX(index + 1, gridBrightCap ? gridBrightCap * 2 : 16384);
		f32 *m = realloc(gridBright, sizeof(f32) * cap);

		if (!m) {
			return;
		}

		gridBright = m;
		gridBrightCap = cap;
	}

	gridBright[index] = fileTriBright(room);
}

static f32 hdTriBright(const struct stri *t)
{
	const f32 *tex = t->tex >= 0 && t->tex < GEBEAN_MAXMATS && brightHdTex[t->tex][0] >= 0.0f
		? brightHdTex[t->tex] : brightHdTex[GEBEAN_MAXMATS];
	f32 luma = 0.0f;

	// argb[] holds r in bits 16-23, g 8-15, b 0-7
	for (s32 ch = 0; ch < 3; ch++) {
		const s32 sh = 16 - ch * 8;
		const f32 v = (((t->argb[0] >> sh) & 0xff) + ((t->argb[1] >> sh) & 0xff) + ((t->argb[2] >> sh) & 0xff)) / 3.0f;

		luma += brightLuma[ch] * v * tex[ch] / 255.0f;
	}

	return luma;
}

static s32 hdTriMeasured(const struct stri *t)
{
	return t->room > 0 && !t->blend && !t->plain && !t->undersea;
}

static void brightHdSums(const struct collect *c, s32 n, struct brightsum *hd)
{
	for (s32 t = 0; t < c->num; t++) {
		const struct stri *tri = &c->tris[t];

		if (hdTriMeasured(tri) && tri->room < n) {
			const f32 area = triArea3(tri->pos);

			hd[tri->room].area += area;
			hd[tri->room].sum += area * hdTriBright(tri);
		}
	}
}

/** Reads the pictures' colours for the measure; 0 if out of memory. */
static s32 brightTexturesRead(u8 **filerooms, u32 *filelens, s32 n)
{
	s32 *nums;
	f32 *means;
	s32 num = 0;
	f64 sum[3] = { 0 };
	s32 known = 0;

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		brightHdTex[t][0] = -1.0f;

		if (t < gebeanLevelNumTextures(level) && texTile[t] && xblaTexImageMean(texTile[t], brightHdTex[t])) {
			for (s32 ch = 0; ch < 3; ch++) {
				sum[ch] += brightHdTex[t][ch];
			}

			known++;
		} else {
			brightHdTex[t][0] = -1.0f;
		}
	}

	for (s32 ch = 0; ch < 3; ch++) {
		brightHdTex[GEBEAN_MAXMATS][ch] = known ? (f32)(sum[ch] / known) : 255.0f;
		sum[ch] = 0.0;
	}

	brightFileTex = calloc(BRIGHT_TEXNUMS + 1, sizeof(*brightFileTex));
	brightFileUsed = calloc(BRIGHT_TEXNUMS, 1);
	nums = malloc(sizeof(s32) * BRIGHT_TEXNUMS);
	means = malloc(sizeof(f32) * 3 * BRIGHT_TEXNUMS);

	if (!brightFileTex || !brightFileUsed || !nums || !means) {
		free(nums);
		free(means);
		return 0;
	}

	for (s32 r = 1; r < n; r++) {
		if (filerooms[r]) {
			fileRoomTrianglesEach(r, filerooms[r], filelens[r], 0, fileTriUsed, NULL);
		}
	}

	for (s32 t = 0; t < BRIGHT_TEXNUMS; t++) {
		brightFileTex[t][0] = -1.0f;

		if (brightFileUsed[t]) {
			nums[num++] = t;
		}
	}

	texpackTextureMeans(nums, num, means);
	known = 0;

	for (s32 i = 0; i < num; i++) {
		memcpy(brightFileTex[nums[i]], &means[i * 3], sizeof(brightFileTex[0]));

		if (means[i * 3] >= 0.0f) {
			for (s32 ch = 0; ch < 3; ch++) {
				sum[ch] += means[i * 3 + ch];
			}

			known++;
		}
	}

	for (s32 ch = 0; ch < 3; ch++) {
		brightFileTex[BRIGHT_TEXNUMS][ch] = known ? (f32)(sum[ch] / known) : 255.0f;
	}

	free(nums);
	free(means);

	return 1;
}

/*
 * A ceiling at each corner: the release baked some places much brighter than
 * the rest of their room (Statue Park's fields: a pool of light where the
 * player starts in grass that is otherwise deep shade, while GoldenEye lights
 * the field evenly), so one factor for the room's grass took that pool to
 * half as bright again as the N64 look. Each corner is given no more than
 * GoldenEye's brightness under the triangles that meet at it over theirs, so
 * nothing ends brighter than the N64 look there; corners at one position
 * with one picture share the sums, so a surface stays smooth.
 */
struct brightcell {
	s32 x, y, z;
	s32 tex;
	f64 hd, go, area;
};

static struct brightcell *brightCells;
static u32 brightCellMask;

static u32 brightCellHash(s32 x, s32 y, s32 z, s32 tex)
{
	u32 h = (u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u ^ (u32)(tex + 1) * 2654435761u;

	return h & brightCellMask;
}

static struct brightcell *brightCorner(const f32 *p, s32 tex, s32 add)
{
	const s32 x = (s32)floorf(p[0] + 0.5f);
	const s32 y = (s32)floorf(p[1] + 0.5f);
	const s32 z = (s32)floorf(p[2] + 0.5f);
	u32 i = brightCellHash(x, y, z, tex);

	for (u32 probe = 0; probe <= brightCellMask; probe++, i = (i + 1) & brightCellMask) {
		struct brightcell *e = &brightCells[i];

		if (e->tex == -2) {
			if (!add) {
				return NULL;
			}

			e->x = x;
			e->y = y;
			e->z = z;
			e->tex = tex;
			return e;
		}

		if (e->x == x && e->y == y && e->z == z && e->tex == tex) {
			return e;
		}
	}

	return NULL;
}

static void brightForget(void)
{
	free(brightFileTex);
	free(brightFileUsed);
	free(gridBright);
	brightFileTex = NULL;
	brightFileUsed = NULL;
	gridBright = NULL;
	gridBrightCap = 0;
}

/**
 * target[t]: how bright GoldenEye's triangle under HD triangle t is (the one
 * the dealing found for it), or < 0.
 *
 * The factor is found per room and per picture within it: Train's corridor
 * walls are the release's darkest picture while its vents and ceiling came
 * out brighter than GoldenEye's, so one factor for the room left the walls,
 * most of what is on screen, at half the N64 look. A picture with less than
 * BRIGHT_MIN_AREA of a room takes the room's factor.
 */
static s32 matchN64Brightness(struct collect *c, u8 **filerooms, u32 *filelens, s32 n, const f32 *target)
{
	struct brightsum *hd = calloc(n + 1, sizeof(*hd));
	struct brightsum *file = calloc(n + 1, sizeof(*file));
	// the HD side and GoldenEye's under it, per room and per room and picture
	struct brightsum *rhd = calloc(n + 1, sizeof(*rhd));
	struct brightsum *rgo = calloc(n + 1, sizeof(*rgo));
	struct brightsum *phd = calloc((size_t)(n + 1) * GEBEAN_MAXMATS, sizeof(*phd));
	struct brightsum *pgo = calloc((size_t)(n + 1) * GEBEAN_MAXMATS, sizeof(*pgo));
	u8 *under1 = calloc(n + 1, 1);
	f64 hdall = 0.0, hdallarea = 0.0, fileall = 0.0, fileallarea = 0.0;
	s32 on = 0, changed = 0, under = 0, measured = 0, clamped = 0, paired = 0;
	char list[1536];
	s32 at = 0;

	list[0] = '\0';

	for (s32 i = 0; i < (s32)ARRAYCOUNT(brightLevels); i++) {
		if (strcmp(brightLevels[i], row->key) == 0) {
			on = 1;
		}
	}

	if (hd && file && rhd && rgo && phd && pgo && under1 && brightFileTex) {
		brightHdSums(c, n, hd);
		for (s32 r = 1; r < n; r++) {
			f32 ratio;

			if (!filerooms[r] || hd[r].area < BRIGHT_MIN_AREA) {
				continue;
			}

			fileRoomTrianglesEach(r, filerooms[r], filelens[r], 0, fileTriToBright, &file[r]);

			if (file[r].area < BRIGHT_MIN_AREA || file[r].sum <= 0.0) {
				continue;
			}

			measured++;
			hdall += hd[r].sum;
			hdallarea += hd[r].area;
			fileall += file[r].sum;
			fileallarea += file[r].area;

			// HD brightness over N64 brightness
			ratio = (hd[r].sum / hd[r].area) / (file[r].sum / file[r].area);
			under1[r] = ratio < 1.0f;

			if (ratio < BRIGHT_REPORT) {
				under++;

				if (at < (s32)sizeof(list) - 40) {
					at += snprintf(list + at, sizeof(list) - at, " %d:%.2f(%.0f/%.0f)", r, ratio,
							hd[r].sum / hd[r].area, file[r].sum / file[r].area);
				}
			}
		}

		sysLogPrintf(LOG_NOTE, "gebeanstage: %s brightness against GoldenEye's rooms: level %.2f (HD %.1f, N64 %.1f), %d of %d rooms under %.2f (room:ratio(HD/N64))%s%s",
				row->bean, hdallarea > 0 && fileall > 0 ? (hdall / hdallarea) / (fileall / fileallarea) : 0.0,
				hdallarea > 0 ? hdall / hdallarea : 0.0, fileallarea > 0 ? fileall / fileallarea : 0.0,
				under, measured, BRIGHT_REPORT, list, on ? " - matched" : "");
	}

	if (on && hd && file && rhd && rgo && phd && pgo && under1 && brightFileTex && target) {
		for (s32 t = 0; t < c->num; t++) {
			const struct stri *tri = &c->tris[t];

			if (hdTriMeasured(tri) && tri->room < n && under1[tri->room] && target[t] >= 0.0f
					&& tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS) {
				const f32 area = triArea3(tri->pos);
				const f32 mine = hdTriBright(tri);
				const s32 k = tri->room * GEBEAN_MAXMATS + tri->tex;

				rhd[tri->room].area += area;
				rhd[tri->room].sum += area * mine;
				rgo[tri->room].sum += area * target[t];
				phd[k].area += area;
				phd[k].sum += area * mine;
				pgo[k].sum += area * target[t];
				paired++;
			}
		}

		// the corners: three a triangle at most, at most a third full
		for (brightCellMask = 1024; brightCellMask < (u32)paired * 9; brightCellMask <<= 1);
		brightCells = malloc(sizeof(*brightCells) * brightCellMask);
		brightCellMask--;

		for (u32 i = 0; brightCells && i <= brightCellMask; i++) {
			brightCells[i].tex = -2;
			brightCells[i].area = brightCells[i].hd = brightCells[i].go = 0.0;
		}

		for (s32 t = 0; brightCells && t < c->num; t++) {
			const struct stri *tri = &c->tris[t];

			if (hdTriMeasured(tri) && tri->room < n && under1[tri->room] && target[t] >= 0.0f
					&& tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS) {
				const f32 area = triArea3(tri->pos);
				const f32 mine = hdTriBright(tri);

				for (s32 k = 0; k < 3; k++) {
					struct brightcell *e = brightCorner(tri->pos[k], tri->tex, 1);

					if (e) {
						e->area += area;
						e->hd += area * mine;
						e->go += area * target[t];
					}
				}
			}
		}

		for (s32 t = 0; t < c->num; t++) {
			struct stri *tri = &c->tris[t];
			const s32 r = tri->room;
			f32 gain = 1.0f;
			f32 vgain[3];
			s32 any;

			// Blended triangles keep their colours: glows, lamp flares, glass
			// and water are the release's own, not light it baked too dark
			if (r <= 0 || r >= n || tri->plain || tri->blend || tri->undersea
					|| (tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS && texWater[tri->tex])
					|| rhd[r].area < BRIGHT_MIN_AREA || rhd[r].sum <= 0.0) {
				continue;
			}

			gain = rgo[r].sum / rhd[r].sum;

			if (tri->tex >= 0 && tri->tex < GEBEAN_MAXMATS) {
				const s32 k = r * GEBEAN_MAXMATS + tri->tex;

				if (phd[k].area >= BRIGHT_MIN_AREA && phd[k].sum > 0.0) {
					gain = pgo[k].sum / phd[k].sum;
				}
			}

			// each corner's share, no more than its ceiling (brightCorner())
			any = 0;

			for (s32 k = 0; k < 3; k++) {
				const struct brightcell *e = brightCells ? brightCorner(tri->pos[k], tri->tex, 0) : NULL;

				vgain[k] = MIN(gain, BRIGHT_MAX_GAIN);

				if (e && e->hd > 0.0) {
					vgain[k] = MIN(vgain[k], (f32)(e->go / e->hd));
				}

				any |= vgain[k] > 1.0f;
			}

			if (!any) {
				continue;
			}

			for (s32 k = 0; k < 3; k++) {
				const u32 v = tri->argb[k];
				u32 out = v & 0xff000000;
				const f32 g = MAX(vgain[k], 1.0f);

				for (s32 sh = 0; sh <= 16; sh += 8) {
					const s32 ch = (s32)(((v >> sh) & 0xff) * g + 0.5f);

					out |= (u32)MIN(ch, 255) << sh;
					clamped += ch > 255 && ((v >> sh) & 0xff) < 255;
				}

				tri->argb[k] = out;
			}

			changed++;
		}

		free(brightCells);
		brightCells = NULL;

		{
			struct brightsum *post = calloc(n + 1, sizeof(*post));
			f64 postall = 0.0, postarea = 0.0;

			if (post) {
				brightHdSums(c, n, post);

				for (s32 r = 1; r < n; r++) {
					if (file[r].area >= BRIGHT_MIN_AREA && post[r].area >= BRIGHT_MIN_AREA) {
						postall += post[r].sum;
						postarea += post[r].area;
					}
				}

				free(post);
			}

			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d triangles brightened to GoldenEye's rooms (%d colour channels held at 255), level now %.2f",
					row->bean, changed, clamped, postarea > 0 && fileall > 0 ? (postall / postarea) / (fileall / fileallarea) : 0.0);
		}
	}

	free(hd);
	free(file);
	free(rhd);
	free(rgo);
	free(phd);
	free(pgo);
	free(under1);

	return changed;
}

/** Where a ray from o along d (unit) meets triangle v, or -1. */
static f32 rayTriT(const f32 *o, const f32 *d, const f32 *v0, const f32 *v1, const f32 *v2)
{
	const f32 e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
	const f32 e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
	const f32 p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
	const f32 det = dot3(e1, p);
	f32 s[3], q[3], u, v, inv;

	if (fabsf(det) < 1e-6f) {
		return -1.0f;
	}

	inv = 1.0f / det;
	s[0] = o[0] - v0[0];
	s[1] = o[1] - v0[1];
	s[2] = o[2] - v0[2];
	u = dot3(s, p) * inv;

	if (u < 0.0f || u > 1.0f) {
		return -1.0f;
	}

	q[0] = s[1] * e1[2] - s[2] * e1[1];
	q[1] = s[2] * e1[0] - s[0] * e1[2];
	q[2] = s[0] * e1[1] - s[1] * e1[0];
	v = dot3(d, q) * inv;

	if (v < 0.0f || u + v > 1.0f) {
		return -1.0f;
	}

	return dot3(e2, q) * inv;
}

#define OVERLAP_REACH 64.0f
#define OVERLAP_SPACE 400.0f

/**
 * The first of GoldenEye's triangles (the grid of them) a ray from o along
 * unit d meets within reach, leaving out those in the plane n.x = pd (the
 * face under test): its index, *t its distance and nf its normal, or -1.
 */
static s32 rayFirst(const struct tgrid *g, const f32 *o, const f32 *d, f32 reach, const f32 *n, f32 pd, f32 *t, f32 *nf)
{
	s32 hit = -1;
	f32 best = reach;

	for (f32 step = 0.0f; step <= best + g->cell; step += g->cell * 0.5f) {
		const f32 at[3] = { o[0] + d[0] * step, o[1] + d[1] * step, o[2] + d[2] * step };
		const s32 cx = (s32)floorf(at[0] / g->cell);
		const s32 cy = (s32)floorf(at[1] / g->cell);
		const s32 cz = (s32)floorf(at[2] / g->cell);

		for (s32 e = g->head[gridKey(cx, cy, cz)]; e >= 0; e = g->entnext[e]) {
			const s32 u = g->enttri[e];
			const f32 *v = g->tri + u * 9;
			f32 tt;

			if (fabsf(dot3(n, v) - pd) < 1.0f && fabsf(dot3(n, v + 3) - pd) < 1.0f && fabsf(dot3(n, v + 6) - pd) < 1.0f) {
				continue;
			}

			tt = rayTriT(o, d, v, v + 3, v + 6);

			if (tt > 0.0f && tt < best) {
				best = tt;
				hit = u;
			}
		}
	}

	if (hit >= 0) {
		const f32 *v = g->tri + hit * 9;
		const f32 p[3][3] = { { v[0], v[1], v[2] }, { v[3], v[4], v[5] }, { v[6], v[7], v[8] } };

		if (cornerNormal(p, nf) <= 0.0f) {
			return -1;
		}

		*t = best;
	}

	return hit;
}

/**
 * Bean's solid faces drawn culled where GoldenEye's own face under them has
 * another room's space behind it: the first of GoldenEye's faces a line
 * back from it meets, within OVERLAP_REACH, faces the same way (a wall of
 * the space behind), not back at it (the far side of a slab, a deck's
 * underside, which Bean draws with its one plane from both sides), and that
 * space is closed on all six sides by GoldenEye's faces turned into it, not
 * the open air under a roof's edge or behind a gantry's panel (Depot, the
 * Cradle), which Bean draws with one plane and GoldenEye leaves open.
 *
 * GoldenEye's rooms overlap and rely on their faces being culled: Aztec's
 * computer room by the exhaust vent is a box whose west wall runs through
 * the vent's last 50 units, and from the vent GoldenEye shows the room
 * through the back of that wall, the body armour on the floor. Bean's wall,
 * drawn two-sided like every HD room (writeLeaf()), was a stone face across
 * the vent (F3 20260929-234633 "disjointed geometry/texture within exhaust
 * tunnel, where the armor would normally be").
 */
static s32 markOverlaps(struct stri *tris, s32 num, const struct tgrid *filetris)
{
	static const f32 axes[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	s32 count = 0;

	for (s32 i = 0; i < num; i++) {
		tris[i].overlap = 0;
	}

	for (s32 i = 0; i < num; i++) {
		struct stri *t = &tris[i];
		f32 n[3], mid[3], o[3], back[3], nf[3], d, pd;
		f32 best = OVERLAP_REACH;
		s32 closed = 1;
		s32 g;

		if (t->decal || t->blend || t->backed || t->fights || t->undersea || texHasAlpha(t->tex) || triNormal(t, n) <= 0.0f) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
			back[j] = -n[j];
			o[j] = mid[j] - n[j] * 1.0f;
		}

		pd = dot3(n, mid);

		// GoldenEye has a face here, facing the same way
		g = tgridNearest(filetris, mid, 0, &d);

		if (g < 0 || d > 4.0f) {
			continue;
		}

		{
			const f32 *v = filetris->tri + g * 9;
			const f32 p[3][3] = { { v[0], v[1], v[2] }, { v[3], v[4], v[5] }, { v[6], v[7], v[8] } };

			if (cornerNormal(p, nf) <= 0.0f || dot3(nf, n) < 0.9f) {
				continue;
			}
		}

		// The first of GoldenEye's faces behind it faces the same way
		// (a space: GoldenEye's rooms sharing a wall have their two faces
		// within a unit of each other, and Depot's warehouse wall's top edge
		// went missing from below when such a pair was taken for one)
		if (rayFirst(filetris, o, back, OVERLAP_REACH, n, pd, &best, nf) < 0 || dot3(nf, n) < 0.5f || best < 8.0f) {
			continue;
		}

		// and the space between is closed all round
		{
			const f32 in[3] = { o[0] + back[0] * best * 0.5f, o[1] + back[1] * best * 0.5f, o[2] + back[2] * best * 0.5f };

			for (s32 a = 0; a < 6 && closed; a++) {
				f32 far = OVERLAP_SPACE;
				f32 na[3];

				closed = rayFirst(filetris, in, axes[a], OVERLAP_SPACE, n, pd, &far, na) >= 0 && dot3(na, axes[a]) < 0.0f;
			}
		}

		if (!closed) {
			continue;
		}

		t->overlap = 1;
		count++;
	}

	return count;
}

static s32 build(void)
{
	const u64 start = sysGetMicroseconds();
	const s32 n = g_Vars.roomcount; // rooms 1 to n - 1, n being the end entry
	u8 **filerooms;
	u32 *filelens;
	struct tgrid filetris, beantris;
	u64 mark[4] = { 0 };
	struct collect c;
	s32 **lists;
	s32 *listlen;
	s32 kept = 0, dropped = 0, moved = 0, farOff = 0, decals = 0, backed = 0, fights = 0, nofogs = 0, plainDecals = 0;
	u32 bytes = 0;
	const char *levelname;
	u64 key = 0;
	s32 keyed = 0;
	s32 complete = 0;
	f32 *hdTarget = NULL;

	row = levelRow();
	fogTableLoad();

	if (!row) {
		return 0;
	}

	// the Community Edition keeps Surface's two halves as files of their own
	levelname = gebeanCeLevelName(row->key, row->bean);
	level = gebeanLevelOpen(levelname);

	if (!level) {
		sysLogPrintf(LOG_WARNING, "gebeanstage: GoldenEye XBLA's %s is not on disk", row->bean);
		return 0;
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		texTile[t] = NULL;
		texAlpha[t] = texSoft[t] = texClampV[t] = texWater[t] = 0;
		texClampShift[t] = 0;
	}

	for (s32 t = 0; t < gebeanLevelNumTextures(level) && t < GEBEAN_MAXMATS; t++) {
		texTile[t] = gebeanLevelTexture(level, t, &texAlpha[t], &texSoft[t]);
		texScroll[t][0] = texScroll[t][1] = 0.0f;
		gebeanLevelTextureScroll(level, t, &texScroll[t][0], &texScroll[t][1]);
	}

	numScrollSlots = 0;

	mark[0] = sysGetMicroseconds();

	// The level file's rooms: where their vertices are, for dealing Bean's out
	// and for telling which rooms Bean has
	filerooms = calloc(n + 1, sizeof(*filerooms));
	filelens = calloc(n + 1, sizeof(*filelens));

	for (s32 r = 1; r < n && filerooms; r++) {
		filerooms[r] = readRoom(r, &filelens[r]);
	}

	// What an earlier load built from these same inputs, if it was kept
	if (filerooms && filelens) {
		key = hdcacheKey(levelname, filerooms, filelens, n);
		keyed = 1;
		mark[1] = sysGetMicroseconds();

		if (hdcacheLoad(key, levelname, n)) {
			shellTake(filerooms, filelens, n);

			for (s32 r = 0; r <= n; r++) {
				free(filerooms[r]);
			}

			free(filerooms);
			free(filelens);

			if (numBackdrop) {
				backdropOrder = malloc(sizeof(s32) * numBackdrop);
				backdropDist = malloc(sizeof(f32) * numBackdrop);

				if (!backdropOrder || !backdropDist) {
					numBackdrop = 0;
				}
			}

			sysLogPrintf(LOG_NOTE, "gebeanstage: %s (GoldenEye's %s): %d of %d rooms from GoldenEye XBLA as built before (%d kept, %d of them not drawn), %d triangles of backdrop, %.0f ms (pictures %.0f, key %.0f, reading %.0f)",
					row->bean, row->key, numServed, n - 1, n - 1 - numServed, numHidden, numBackdrop,
					(sysGetMicroseconds() - start) / 1000.0, (mark[0] - start) / 1000.0,
					(mark[1] - mark[0]) / 1000.0, (sysGetMicroseconds() - mark[1]) / 1000.0);

			return numServed > 0;
		}
	}

	memset(&c, 0, sizeof(c));
	c.scale = row->scale;
	c.offset = row->offset;
	gebeanLevelTriangles(level, collectTri, &c);

	levelHasWater = 0;

	for (s32 t = 0; t < gebeanLevelNumTextures(level) && t < GEBEAN_MAXMATS; t++) {
		texWater[t] = gebeanLevelTextureIsWater(level, t);
		levelHasWater |= texWater[t];
	}

	{
		const s32 welded = weldVertices(&c);

		if (welded) {
			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d corners welded to a corner within %.2f of them", row->bean, welded, WELD_DIST);
		}
	}

	{
		const u64 from = sysGetMicroseconds();
		const s32 unlit = paintUnlit(&c);

		if (unlit) {
			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d black faces given the light of the lit faces beside them (%.1f ms)",
					row->bean, unlit, (sysGetMicroseconds() - from) / 1000.0f);
		}
	}

	takeBackdrop(&c, n);
	clampCutouts(&c);
	closeDoorGaps(&c);
	fillDoorSlots(&c, filerooms, filelens, n);

	{
		const s32 undersea = markUnderSea(&c, filerooms, filelens, n);

		if (undersea) {
			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d triangles of the reflection under the sea drawn through it",
					row->bean, undersea);
		}
	}

	for (s32 j = 0; j < 3; j++) {
		meshMin[j] = 1e30f;
		meshMax[j] = -1e30f;
	}

	for (s32 t = 0; t < c.num; t++) {
		for (s32 k = 0; k < 3; k++) {
			for (s32 j = 0; j < 3; j++) {
				meshMin[j] = MIN(meshMin[j], c.tris[t].pos[k][j]);
				meshMax[j] = MAX(meshMax[j], c.tris[t].pos[k][j]);
			}
		}
	}

	mark[1] = sysGetMicroseconds();

	lists = calloc(n + 1, sizeof(*lists));
	listlen = calloc(n + 1, sizeof(*listlen));
	roomData = calloc(n + 1, sizeof(*roomData));
	roomLen = calloc(n + 1, sizeof(*roomLen));
	roomHidden = calloc(n + 1, sizeof(*roomHidden));
	roomWaterKept = calloc(n + 1, sizeof(*roomWaterKept));
	numWaterKept = 0;
	numRooms = n;

	if (!filerooms || !filelens || !lists || !listlen || !roomData || !roomLen || c.num == 0
			|| !tgridInit(&filetris, ASSIGN_CELL)
			|| !tgridInit(&beantris, BEAN_CELL)) {
		sysLogPrintf(LOG_ERROR, "gebeanstage: %s: could not build", row->bean);
		// fall through to the frees; nothing is served
		c.num = 0;
	}

	if (c.num) {
		// the pictures' colours first: the grid notes how bright each of
		// GoldenEye's triangles is as it is filled (matchN64Brightness())
		if (!brightTexturesRead(filerooms, filelens, n)) {
			brightForget();
		}

		for (s32 r = 1; r < n; r++) {
			if (filerooms[r]) {
				fileRoomTriangles(&filetris, r, filerooms[r], filelens[r]);
			}
		}

		shellTake(filerooms, filelens, n);

		for (s32 t = 0; t < c.num; t++) {
			tgridAdd(&beantris, (const f32 (*)[3])c.tris[t].pos, t);
		}

		triCacheFill(c.tris, c.num);
		backed = markBacked(c.tris, c.num, &beantris);
		fights = markFights(c.tris, c.num, &beantris);
		decals = markDecals(c.tris, c.num, &beantris);
		triCacheForget();
		markDecalLifts(c.tris, c.num);
		{
			const s32 shadows = markShadows(c.tris, c.num);

			if (shadows) {
				sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d triangles of cast shadows drawn see-through", row->bean, shadows);
			}
		}
		{
			const s32 overlaps = markOverlaps(c.tris, c.num, &filetris);

			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d faces drawn culled over another room's space", row->bean, overlaps);
		}
		markWaterPictures(&c, filerooms, filelens, n);

		mark[2] = sysGetMicroseconds();

		hdTarget = malloc(sizeof(f32) * c.num);

		for (s32 t = 0; hdTarget && t < c.num; t++) {
			hdTarget[t] = -1.0f;
		}

		// Deal each triangle to the room whose own triangle its middle lies
		// on. Rooms share the vertices along their borders, so the nearest
		// vertex named the room next door for a big floor triangle about as
		// often as not, and that room's portal scissored it away.
		for (s32 t = 0; t < c.num; t++) {
			struct stri *tri = &c.tris[t];
			f32 mid[3];

			// A decal of a draw with no UV, no picture of its own and no
			// colour in its vertices either (white, opaque) is a surface its
			// shader alone shades, which the port has nothing to draw with,
			// lying on one that is whole without it (gebeanLevelTriangles():
			// Bunker's helipad, a grey band of the wrong picture)
			if (tri->plain && tri->decal && tri->argb[0] == 0xffffffff
					&& tri->argb[1] == 0xffffffff && tri->argb[2] == 0xffffffff) {
				tri->room = 0;
				plainDecals++;
				continue;
			}

			for (s32 j = 0; j < 3; j++) {
				mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
			}

			{
				f32 d;
				const s32 near = tgridNearest(&filetris, mid, ASSIGN_RINGS, &d);

				const s32 file = near >= 0 ? filetris.room[near] : nearestRoomBox(mid, n);

				tri->room = file & 0xffff;
				tri->nofog = (file >> 16) & 1;

				if (hdTarget) {
					hdTarget[t] = near >= 0 && gridBright && near < gridBrightCap ? gridBright[near] : -1.0f;
				}
				nofogs += tri->nofog;
			}

			if (tri->room > 0) {
				listlen[tri->room]++;
			}
		}

		// A triangle too far from its room's position for a Vtx to hold
		// (Dam's far mountains, dealt to the rooms of GoldenEye's backdrop
		// 35000 nearer) goes to the nearest room of Bean's it fits. Every room
		// is drawn, so where it is dealt changes nothing on the screen; left
		// out, it was a hole onto the sky in the mountainside, hidden only
		// while the fog there was whole
		for (s32 t = 0; t < c.num; t++) {
			struct stri *tri = &c.tris[t];
			s32 best = -1;
			f32 bestd = 0.0f;

			if (tri->room <= 0 || triFitsRoom(tri, tri->room)) {
				continue;
			}

			for (s32 r = 1; r < n; r++) {
				if (r != tri->room && listlen[r] > 0 && triFitsRoom(tri, r)) {
					const f32 dx = tri->pos[0][0] - g_BgRooms[r].pos.x;
					const f32 dy = tri->pos[0][1] - g_BgRooms[r].pos.y;
					const f32 dz = tri->pos[0][2] - g_BgRooms[r].pos.z;
					const f32 d = dx * dx + dy * dy + dz * dz;

					if (best < 0 || d < bestd) {
						best = r;
						bestd = d;
					}
				}
			}

			if (best > 0) {
				listlen[tri->room]--;
				listlen[best]++;
				tri->room = best;
				moved++;
			} else {
				// Out of every room's reach: drawn with the backdrop, which
				// is drawn where it stands, furthest first behind the rooms
				struct stri *n2 = realloc(backdrop, sizeof(*backdrop) * (numBackdrop + 1));

				if (n2) {
					backdrop = n2;
					backdrop[numBackdrop++] = *tri;
					listlen[tri->room]--;
					tri->room = 0;
					farOff++;
				}
			}
		}

		if (plainDecals) {
			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d white decal triangles of draws with no UV or picture left out", row->bean, plainDecals);
		}

		{
			const s32 cornersmoved = markDecalCorners(c.tris, c.num);

			sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d corners of decals kept in their mesh moved off their base's rounding", row->bean, cornersmoved);
		}

		for (s32 r = 1; r < n; r++) {
			lists[r] = listlen[r] ? malloc(sizeof(s32) * listlen[r]) : NULL;
			listlen[r] = 0;
		}

		for (s32 t = 0; t < c.num; t++) {
			const s32 r = c.tris[t].room;

			if (r > 0 && lists[r]) {
				lists[r][listlen[r]++] = t;
			}
		}

		// Every room Bean's mesh reaches is Bean's. The level file is
		// GoldenEye's own data, which Bean's mesh was built on, so a room is
		// only ever left as the file's because Bean has nothing there -
		// terrain it re-meshed away, or a room it never drew. (The share of a
		// room's surface Bean covers used to be measured here and rooms under
		// 97% left alone. That was for GoldenEye X, whose levels were rebuilt
		// from GoldenEye's and whose rooms Bean therefore disagreed with.)
		matchN64Brightness(&c, filerooms, filelens, n, hdTarget);
		free(hdTarget);
		hdTarget = NULL;
		brightForget();

		mark[3] = sysGetMicroseconds();

		for (s32 r = 1; r < n; r++) {
			if (!filerooms[r] || listlen[r] == 0) {
				// Nothing of Bean's is on a kept room's surfaces anywhere:
				// GoldenEye's own backdrop, which the release remodelled
				// further out (Dam's far cliffs, Cradle's duct) and does not
				// draw. Drawn over the HD level, a fogged cliff edge showed
				// against the sky past Bean's trees ("sky tear"). A kept room
				// Bean's mesh does lie on (Depot's) stays as it was. One that
				// touches Bean's mesh with a triangle or two is backdrop all
				// the same: Dam's room 31, a low grey boulder of a cliff over
				// the far end of the reservoir, 1 of its triangles within a
				// cell of Bean's mountainside, which stands behind it
				if (filerooms[r] && roomHidden) {
					hiddenTris = hiddenNear = 0;
					fileRoomTrianglesEach(r, filerooms[r], filelens[r], 1, fileTriNearBean, &beantris);

					if (hiddenTris > 0 && hiddenNear * 4 < hiddenTris) {
						roomHidden[r] = 1;
						numHidden++;
					}
				}

				kept++;
				continue;
			}

			roomData[r] = writeRoom(r, c.tris, lists[r], listlen[r], filerooms[r], filelens[r], &roomLen[r], &dropped);

			if (roomData[r]) {
				numServed++;
				bytes += roomLen[r];
			} else {
				kept++;
			}
		}

		tgridFree(&filetris);
		tgridFree(&beantris);
		complete = 1;
	}

	for (s32 r = 0; r <= n; r++) {
		if (filerooms) {
			free(filerooms[r]);
		}

		if (lists) {
			free(lists[r]);
		}
	}

	free(filerooms);
	free(filelens);
	free(lists);
	free(listlen);
	free(c.tris);

	// The backdrop's draw order, once it has everything it takes: its
	// pictures (takeBackdrop()) and what no room can reach
	if (numBackdrop) {
		backdropOrder = malloc(sizeof(s32) * numBackdrop);
		backdropDist = malloc(sizeof(f32) * numBackdrop);

		if (!backdropOrder || !backdropDist) {
			numBackdrop = 0;
		}
	}

	{
		s32 clamped = 0;

		for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
			clamped += texClampV[t];
		}

		sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d triangles of backdrop, %d cut-outs clamped in t", row->bean, numBackdrop, clamped);
	}

	if (numWaterKept) {
		sysLogPrintf(LOG_NOTE, "gebeanstage: %s: %d of GoldenEye's water leaves kept over the HD rooms", row->bean, numWaterKept);
	}

	sysLogPrintf(LOG_NOTE, "gebeanstage: %s (GoldenEye's %s) at scale %.5f: %d of %d rooms from GoldenEye XBLA (%d kept, %d of them not drawn), %d triangles (%d decals, %d two-faced, %d back to back in part, %d unfogged), %u bytes, %d triangles off a room's range (%d dealt to another in reach, %d to the backdrop), %.0f ms (pictures %.0f, mesh %.0f, grids %.0f, dealing %.0f, writing %.0f)",
			row->bean, row->key, row->scale, numServed, n - 1, kept, numHidden, c.num, decals, backed, fights, nofogs, bytes, dropped, moved, farOff,
			(sysGetMicroseconds() - start) / 1000.0,
			(mark[0] - start) / 1000.0, (mark[1] - mark[0]) / 1000.0, mark[2] ? (mark[2] - mark[1]) / 1000.0 : 0.0,
			mark[3] ? (mark[3] - mark[2]) / 1000.0 : 0.0,
			mark[3] ? (sysGetMicroseconds() - mark[3]) / 1000.0 : 0.0);

	if (complete && keyed && numServed > 0) {
		hdcacheSave(key, levelname);
	}

	return numServed > 0;
}

/* -------------------------------------------------------------------------
 * The far plane
 * ------------------------------------------------------------------------- */

/**
 * The far plane while the HD rooms are served: at least the length of the HD
 * mesh's box, so that nothing of the level is ever cut by it.
 *
 * GoldenEye's own far plane is its fog table's (Surface 12500, Jungle 2500,
 * Train 1500), set for a level drawn through its portals and fogged to the sky
 * colour before it. An HD level draws every room, so trees came and went at
 * 12500 as the player walked (F3 20260925-030603: "can see trees in distance
 * drawing live"). The depth buffer's precision is the near plane's, which
 * stays.
 *
 * What the HD level is fogged by is the release's own fog (gebeanStageFog()),
 * not the plane's; a level the release has no fog for keeps GoldenEye's where
 * it was by distance (gebeanStageFogFactor()). envTick() goes on reckoning the
 * fog distance the guards see by and the chrs' portal walk from the level's
 * own plane (gebeanStageFarOwn()).
 *
 * With the fog off (Disable Fog) every fogged level's plane goes out past the
 * level's box the same way, HD or not: a level's fog ends at its far plane,
 * and with nothing fogged the plane cut the level where the fog had hidden it.
 *
 * Put back when the HD rooms go (F6) and the fog is back on.
 */
void gebeanStageTickFar(void)
{
	struct zrange zrange;
	f32 want = 0.0f;
	f32 len = 0.0f;

	viGetZRange(&zrange);

	if (xblaStageDrawsEveryRoom() && meshMin[0] <= meshMax[0]) {
		const f32 dx = meshMax[0] - meshMin[0];
		const f32 dy = meshMax[1] - meshMin[1];
		const f32 dz = meshMax[2] - meshMin[2];

		len = sqrtf(dx * dx + dy * dy + dz * dz);
	}

	if (modIsFogDisabled() && g_FogEnabled) {
		len = MAX(len, bgLevelLength());
	}

	if (len > 0.0f) {
		want = len * 1.05f * bgGetScaleBg2Gfx();
	}

	// Something set the plane since it was raised (an environment's
	// transition, Facility's gas): that is the level's own now
	if (farRaised && fabsf(zrange.far - farSet) > 1.0f) {
		farRaised = 0;
	}

	if (want > zrange.far + 1.0f) {
		if (!farRaised) {
			farOwn = zrange.far;
		}

		// Once, not every tick an environment's transition sets the plane
		if (farOwn != farLogged) {
			farLogged = farOwn;
			sysLogPrintf(LOG_NOTE, "gebeanstage: far plane %.0f -> %.0f for the %s", farOwn, want,
					xblaStageDrawsEveryRoom() ? "HD level" : "level without its fog");
		}

		farRaised = 1;
		farSet = want;
		viSetZRange(zrange.near, want);
		envTick();
	} else if (farRaised && want <= 0.0f) {
		farRaised = 0;
		viSetZRange(zrange.near, farOwn);
		envTick();
	}
}

/** The level's own far plane while it is raised for the HD rooms. */
s32 gebeanStageFarOwn(f32 *far)
{
	if (!farRaised) {
		return 0;
	}

	*far = farOwn;

	return 1;
}

/**
 * For an HD level the release has no fog for: the fog line (gSPFogFactor()'s
 * multiplier and offset, as floats for gSPFogLineEXT()) for fog positions min
 * and max, which are the level's own for its own far plane, under the raised
 * one. The fog is linear in the depth: z(w) = A - B / w, A = (f + n) / (f -
 * n), B = 2fn / (f - n); fog = z * fm + fo. Keeping fog(w) the same for every
 * w under a new A' and B' is fm' = fm B / B' and fo' = fo + A fm - A' fm'.
 */
s32 gebeanStageFogFactor(s32 min, s32 max, f32 *fm, f32 *fo)
{
	struct zrange zrange;
	f64 n, f, f2, a, b, a2, b2, m, o, m2;

	if (!farRaised || max <= min) {
		return 0;
	}

	viGetZRange(&zrange);

	n = zrange.near;
	f = farOwn;
	f2 = zrange.far;

	if (f <= n || f2 <= n) {
		return 0;
	}

	a = (f + n) / (f - n);
	b = 2.0 * f * n / (f - n);
	a2 = (f2 + n) / (f2 - n);
	b2 = 2.0 * f2 * n / (f2 - n);
	m = 128000.0 / (max - min);
	o = (500.0 - min) * 256.0 / (max - min);
	m2 = m * b / b2;

	*fm = m2;
	*fo = o + a * m - a2 * m2;

	return 1;
}

/* -------------------------------------------------------------------------
 * The release's fog
 * ------------------------------------------------------------------------- */

/**
 * GoldenEye XBLA's own fog for each level, from its default.xex.
 *
 * The release's environment table (at 0x82858860 in the image, the file's
 * 0x84b860) is Perfect Dark's fogenvironment row, 56 bytes, with GoldenEye's
 * own columns for the N64 look (its near and far, fog positions and colour,
 * all as the ROM has them) and 4J's for the HD look after them: at +0x24 the
 * distance the fog is whole at, +0x28 its colour, +0x2c and +0x30 the HD far
 * and near planes. The environment tick (0x82118168) sets the fog from -100
 * to that distance, linear, in that colour, and the HD shaders mix to it by
 * saturate(dist * c0.x + c0.y) (c0 = -1 / (end - start), end / (end - start),
 * 0x823adab8). The N64 look is fogged from -100 to GoldenEye's far plane in
 * GoldenEye's colour instead. The distances are in the level's world units,
 * which the conversion keeps (geconvert.c).
 *
 * Levels 4J left as GoldenEye had them carry the far plane and colour (the
 * indoor levels: fogged whole at the far plane, linearly); Dam, Runway and
 * Surface take a light blue and a warm grey haze and their fog much further
 * out. Levels GoldenEye draws without fog (Frigate, Silo, Bunker 1, the
 * multiplayer-only ones) have no row in either.
 *
 * The table is read out of the player's own copy (fogTableLoad()): the
 * Community Edition patches it too (Surface 2's fog nearer "to closer match
 * N64", Jungle, Temple, Train, Archives, Statue...), so with the CE's overlay
 * on its rows are the ones its patched default.xex has (gebeance.c writes
 * them into the overlay). The retail rows below are only for a copy whose
 * default.xex is not unpacked.
 *
 * Facility's colour in the release is 0x102001, which reads as GoldenEye's
 * 0x102010 with its last two nibbles swapped (every other row keeps
 * GoldenEye's colour or a new one outright); GoldenEye's is used.
 */
struct beanfog {
	s16 levelid;     // GoldenEye's level id, the table's first column
	s32 end;         // where the fog is whole (+0x24)
	u32 rgb;         // its colour (+0x28)
};

#define BEANFOG_ROW  56
#define BEANFOG_MAX  128

// The retail table's HD columns, for a copy whose default.xex is not there
static const struct beanfog beanFogsRetail[] = {
	{ 22, 3500,  0x000008 }, // Statue
	{ 23, 10000, 0x000000 }, // Control
	{ 24, 3000,  0x000000 }, // Archives
	{ 25, 1500,  0x000008 }, // Train
	{ 27, 10000, 0x100000 }, // Bunker 2
	{ 28, 15000, 0x000000 }, // Aztec
	{ 29, 7500,  0x101820 }, // Streets
	{ 30, 5000,  0x000008 }, // Depot
	{ 31, 5000,  0x280000 }, // Complex
	{ 32, 20000, 0x103060 }, // Egyptian
	{ 33, 15000, 0x85adca }, // Dam
	{ 34, 5000,  0x102001 }, // Facility
	{ 35, 55000, 0x85adca }, // Runway
	{ 36, 45000, 0xa49682 }, // Surface
	{ 37, 2500,  0x182000 }, // Jungle
	{ 38, 6000,  0x181828 }, // Temple
	{ 39, 6000,  0x080008 }, // Caverns
	{ 41, 30000, 0x6080a0 }, // Cradle
	{ 43, 10000, 0x201010 }, // Surface 2
};

// GoldenEye's key for a level (stageRows[]) and its level id (geconvert.c)
static const struct {
	const char *key;
	s16 levelid;
} beanFogLevels[] = {
	{ "sev", 9 }, { "silo", 20 }, { "stat", 22 }, { "arec", 23 }, { "arch", 24 }, { "tra", 25 },
	{ "dest", 26 }, { "sevb", 27 }, { "azt", 28 }, { "pete", 29 }, { "depo", 30 }, { "ref", 31 },
	{ "cryp", 32 }, { "dam", 33 }, { "ark", 34 }, { "run", 35 }, { "sevx", 36 }, { "jun", 37 },
	{ "dish", 38 }, { "cave", 39 }, { "crad", 41 }, { "sevxb", 43 }, { "base", 45 }, { "stack", 46 },
	{ "lib", 48 }, { "oat", 50 },
};

static struct beanfog beanFogs[BEANFOG_MAX];
static s32 numBeanFogs = -1;

// Where every level's fog starts (the environment tick's -100)
#define BEANFOG_START -100.0f

// Dam's fog reaches further the lower the player is: the tick adds 65000 at
// 2782 and under, nothing at 9161 and over (GoldenEye's world heights, the
// constants before the table). The conversion moved Dam up by 13219.
#define BEANFOG_DAM_REACH 65000.0f
#define BEANFOG_DAM_LOW   (2782.0f + 13219.0f)
#define BEANFOG_DAM_HIGH  (9161.0f + 13219.0f)
#define BEANFOG_DAM       33
#define BEANFOG_SURFACE2  43

static u32 beBe32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/**
 * Where the environment table starts in a default.xex (the release's is stored
 * uncompressed), and how many bytes of rows it has with its end row: Statue's
 * row is first in the release and the Community Edition alike (level 22, near
 * 15, far 3500), and the rows run in 56s to one whose level is 0. 0 when it is
 * not there.
 */
u32 gebeanStageFogTableFind(const u8 *xex, u32 len, u32 *at)
{
	static const u8 first[] = { 0x00, 0x16, 0x00, 0x0f, 0x0d, 0xac };

	for (u32 o = 0; o + BEANFOG_ROW <= len; o += 2) {
		u32 n;

		if (memcmp(xex + o, first, sizeof(first)) != 0) {
			continue;
		}

		for (n = 0; n < BEANFOG_MAX && o + (n + 1) * BEANFOG_ROW <= len; n++) {
			const s16 id = (s16)(xex[o + n * BEANFOG_ROW] << 8 | xex[o + n * BEANFOG_ROW + 1]);

			if (id == 0) {
				*at = o;
				return (n + 1) * BEANFOG_ROW;
			}

			if (id < 0 || id >= 1000) {
				break;
			}
		}
	}

	return 0;
}

/** beanFogs[] out of a table's rows (gebeanStageFogTableFind()'s bytes). */
static void fogTableRead(const u8 *rows, u32 len)
{
	numBeanFogs = 0;

	for (u32 o = 0; o + BEANFOG_ROW <= len && numBeanFogs < BEANFOG_MAX; o += BEANFOG_ROW) {
		struct beanfog *f = &beanFogs[numBeanFogs];

		f->levelid = (s16)(rows[o] << 8 | rows[o + 1]);

		if (f->levelid == 0) {
			break;
		}

		f->end = (s32)beBe32(rows + o + 0x24);
		f->rgb = beBe32(rows + o + 0x28) >> 8;
		numBeanFogs++;
	}
}

static u8 *fogFileLoad(const char *path, u32 *len)
{
	FILE *fp = fopen(path, "rb");
	u8 *data = NULL;
	long size;

	*len = 0;

	if (!fp) {
		return NULL;
	}

	if (fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) > 0 && fseek(fp, 0, SEEK_SET) == 0
			&& (data = malloc(size)) != NULL) {
		if (fread(data, 1, size, fp) == (size_t)size) {
			*len = size;
		} else {
			free(data);
			data = NULL;
		}
	}

	fclose(fp);

	return data;
}

/**
 * The table, once a session: the Community Edition's rows when its overlay is
 * drawn, else the player's default.xex beside the release's files/, else the
 * retail rows built in.
 */
static void fogTableLoad(void)
{
	char root[FS_MAXPATH + 1], archive[FS_MAXPATH + 1], cache[FS_MAXPATH + 1], path[FS_MAXPATH + 1];
	const char *from = "the retail rows built in";
	u8 *data;
	u32 len, at, n;

	if (numBeanFogs >= 0) {
		return;
	}

	numBeanFogs = 0;

	if (gebeanCeFogTablePath(path, sizeof(path)) && (data = fogFileLoad(path, &len)) != NULL) {
		fogTableRead(data, len);
		free(data);
		from = "the Community Edition's default.xex";
	} else if (gebeanTreeInfo(root, sizeof(root), archive, sizeof(archive), cache, sizeof(cache))) {
		snprintf(path, sizeof(path), "%s/../default.xex", root);

		if ((data = fogFileLoad(path, &len)) != NULL) {
			if ((n = gebeanStageFogTableFind(data, len, &at)) > 0) {
				fogTableRead(data + at, n);
				from = "the release's default.xex";
			}

			free(data);
		}
	}

	if (numBeanFogs == 0) {
		for (u32 i = 0; i < ARRAYCOUNT(beanFogsRetail); i++) {
			beanFogs[numBeanFogs++] = beanFogsRetail[i];
		}
	}

	for (s32 i = 0; i < numBeanFogs; i++) {
		if (beanFogs[i].levelid == 34 && beanFogs[i].rgb == 0x102001) {
			beanFogs[i].rgb = 0x102010;
		}

		// Surface 2's fog whole at 6500, not the release's 10000: the
		// Community Edition's one correction to the table ("reduced fog
		// distance to closer match N64"), taken whether its zip is on or
		// not. GoldenEye's own fog there is half at about 450 units and nine
		// tenths at about 2300 (957 of its 1000 over a 10..10000 range);
		// linear to 10000 it was a third at 3300, linear to 6500 half.
		if (beanFogs[i].levelid == BEANFOG_SURFACE2 && beanFogs[i].end == 10000) {
			beanFogs[i].end = 6500;
		}
	}

	sysLogPrintf(LOG_NOTE, "gebeanstage: the HD levels' fog: %d rows from %s", numBeanFogs, from);
}

static const struct beanfog *fogRow(void)
{
	s16 levelid = -1;

	if (!row) {
		return NULL;
	}

	fogTableLoad();

	for (u32 i = 0; i < ARRAYCOUNT(beanFogLevels); i++) {
		if (strcmp(beanFogLevels[i].key, row->key) == 0) {
			levelid = beanFogLevels[i].levelid;
		}
	}

	for (s32 i = 0; i < numBeanFogs; i++) {
		if (beanFogs[i].levelid == levelid) {
			return &beanFogs[i];
		}
	}

	return NULL;
}

/**
 * The release's fog for the HD level being drawn: where it starts and where
 * it is whole, in the level's world units, and its colour. 0 when the HD rooms
 * are not served, the level has no fog, or the release has none for it.
 */
s32 gebeanStageFog(f32 *start, f32 *end, u8 *rgb)
{
	const struct beanfog *f;

	if (!g_FogEnabled || modIsFogDisabled() || !xblaStageDrawsEveryRoom() || (f = fogRow()) == NULL) {
		return 0;
	}

	*start = BEANFOG_START;
	*end = f->end;

	// Dam's rule is the player's height; the camera's stands in for it, and
	// is the player's own but in the opening and closing shots
	if (f->levelid == BEANFOG_DAM) {
		const f32 y = g_Vars.currentplayer->cam_pos.y;
		const f32 t = (BEANFOG_DAM_HIGH - y) / (BEANFOG_DAM_HIGH - BEANFOG_DAM_LOW);

		*end += BEANFOG_DAM_REACH * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
	}

	rgb[0] = f->rgb >> 16;
	rgb[1] = f->rgb >> 8;
	rgb[2] = f->rgb;

	// A Community Edition dome whose horizon is not the fog's colour gives
	// it its own (Surface 2's grey storm)
	gebeanSkyFogColour(rgb);

	// GoldenEye's gas cloud (and sky switch) fades the level's fog to its
	// second sky (bgfog.c's fogSwitchToSolosky2(); env.c's
	// envApplyTransitionFrac()): the release's fog closes in by the same
	// share as GoldenEye's own far fog - Facility's from 5000 to 1000 - and
	// takes the second sky's colour, the gas's green
	{
		struct fogenvironment *from, *to;
		f32 frac;

		if (envGetTransition(&frac, &from, &to) && from->far > 0) {
			const f32 share = 1.0f + frac * ((f32)to->far / (f32)from->far - 1.0f);
			const u8 torgb[3] = { to->sky_r, to->sky_g, to->sky_b };

			*end = *start + (*end - *start) * share;

			for (s32 i = 0; i < 3; i++) {
				rgb[i] = rgb[i] + frac * ((f32)torgb[i] - (f32)rgb[i]);
			}
		}
	}

	return 1;
}

/**
 * The release's fog as a linear fog line for gSPFogLineEXT(): factor = w *
 * mul + offset, 0 at the start and 255 where it is whole. `depth` is what a
 * world unit of distance is in the eye depth of what is drawn - the level's
 * render scale for the rooms, more for something drawn scaled towards the eye.
 */
s32 gebeanStageFogLine(f32 depth, f32 *mul, f32 *offset, u8 *rgb)
{
	f32 start, end;

	if (!gebeanStageFog(&start, &end, rgb) || end <= start) {
		return 0;
	}

	*mul = 255.0f / ((end - start) * depth);
	*offset = -start * 255.0f / (end - start);

	return 1;
}

/**
 * A prop's or chr's share of the release's fog at its depth z (world units),
 * for envGetObjShadeMode(): the rooms' own fog at that distance, capped at
 * whole, so that a prop past the fog is drawn in the fog's colour as the rooms
 * behind it are.
 */
s32 gebeanStageObjFog(f32 z, f32 *frac, u8 *rgb)
{
	f32 start, end;

	if (!gebeanStageFog(&start, &end, rgb) || end <= start) {
		return 0;
	}

	*frac = (z - start) / (end - start);

	if (*frac > 1.0f) {
		*frac = 1.0f;
	}

	return 1;
}

/**
 * A room served from the HD level, once bg.c's swaps have run over it. With
 * fog, every render mode left without the fog blend in its first cycle takes
 * it. The swap knows GoldenEye's own modes; the HD rooms also draw cut-outs
 * (the pines, the forest wall, Jungle's leaves: the texture edge with
 * 1 - alpha, 0x0c183078), decals in the decal modes, and the surfaces
 * GoldenEye drew unfogged, and the release fogs every one of them.
 *
 * With fog or without transparency, the swap also takes the vertex alpha out
 * of the combiner (G_CC_MODULATEIA2 to G_CC_CUSTOM_06, the environment's
 * alpha): on the N64 the RSP writes the fog into the shade alpha, and a level
 * without transparency draws none. The renderer keeps the fog apart from the
 * shade alpha, and the release fades pictures by their vertices' alpha - Dam's
 * painted road markings (white at half), the lamp's light cone in the server
 * room (0x82) - so the picture leaves take it back. Drawn by the texels'
 * alpha alone, the markings were solid white patches and the cone a white
 * pyramid (F3 20260925-234751, 20260925-235806).
 */
void gebeanStageFogRoom(s32 roomnum, struct roomblock *opa, struct roomblock *xlu, s32 fog)
{
	struct roomblock *blocks[2] = { opa, xlu };

	if (!built || roomnum < 1 || roomnum >= numRooms || !roomData[roomnum] || !xblaStageIsRelease()) {
		return;
	}

	for (s32 i = 0; i < 2; i++) {
		struct roomblock *stack[16];
		s32 depth = 0;
		struct roomblock *block = blocks[i];
		// GoldenEye's own water leaves, which end the translucent chain, keep
		// the file's modes as bg.c's swaps left them (fileWaterLeaves())
		s32 hdleaves = i == 1 && roomWaterKept && roomWaterKept[roomnum] ? roomWaterKept[roomnum] - 1 : 99;

		while (block || depth > 0) {
			if (!block) {
				block = stack[--depth];
				continue;
			}

			if (block->type == ROOMBLOCKTYPE_LEAF && hdleaves-- <= 0) {
				break;
			}

			if (block->type == ROOMBLOCKTYPE_LEAF) {
				for (Gfx *gdl = block->gdl; gdl && (u8)(gdl->words.w0 >> 24) != (u8)G_ENDDL; gdl++) {
					if (fog && (u32)gdl->words.w0 == 0xb900031d
							&& ((u32)gdl->words.w1 & 0xcccc0000) != (G_RM_FOG_SHADE_A & 0xcccc0000)) {
						gdl->words.w1 = ((u32)gdl->words.w1 & ~0xcccc0000) | (G_RM_FOG_SHADE_A & 0xcccc0000);
					}

					// writeLeaf()'s picture combiner, as the swap left it
					if ((u32)gdl->words.w0 == 0xfc26a004 && (u32)gdl->words.w1 == 0x1f1493ff) {
						gdl->words.w1 = 0x1f1093ff;
					}
				}

				block = block->next;
			} else if (block->type == ROOMBLOCKTYPE_PARENT) {
				if (depth < ARRAYCOUNT(stack)) {
					stack[depth++] = block->next;
				}

				block = block->child;
			} else {
				block = NULL;
			}
		}
	}
}

/* -------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

#define BACKDROP_EXTENT 16000.0f // the farthest vertex, in what a Vtx holds
#define BACKDROP_BATCH  15       // vertices a load: a G_VTX holds 16 at most

/**
 * The level's backdrop (takeBackdrop()), drawn after the sky and before the
 * rooms, as the release shows it: whole, faded into the sky by its vertex
 * alpha, and fogged by the release's fog at the distance it really stands at
 * (gebeanStageFogLine()) - it is part of the level's file and drawn with the
 * level's shaders, and unfogged it stood out bright past the hazed ground in
 * front of it. The triangles no room's position can reach are drawn with it.
 *
 * It is drawn where it is - moving the camera moves it against the peaks, as
 * a band 25000 out should - but scaled towards the eye so that its farthest
 * vertex sits inside the far plane. Scaling about the eye moves nothing on
 * the screen. With no depth test, the rooms drawn after it cover it, and its
 * own triangles are drawn furthest first (Cradle's cliffs fold).
 */
static int compareBackdropFar(const void *a, const void *b)
{
	const f32 da = backdropDist[*(const s32 *)a];
	const f32 db = backdropDist[*(const s32 *)b];

	return da > db ? -1 : da < db ? 1 : *(const s32 *)a - *(const s32 *)b;
}

/**
 * A scrolling picture's tiles, moved to where the release's texture matrix has
 * them at the level's clock: the picture slides under the room's coordinates
 * as the matrix slides the coordinates over it. The release reads at v + d
 * in the file's UVs turned up (Maya's, which the export turned over), which
 * the room has turned over again (writeLeaf()), so both axes move the tile's
 * origin back by the slide. Whole quarter texels in the size, the rest as the
 * tile's fraction, as gewater.c moves the reservoir.
 */
static void scrollBuild(s32 i, f32 clock)
{
	Gfx *gdl = scrollSlots[i].gdl;
	const s32 t = scrollSlots[i].tex;
	const s32 w = scrollSlots[i].w, h = scrollSlots[i].h;
	f32 q[2];
	s32 whole[2];
	u32 frac[2];

	for (s32 a = 0; a < 2; a++) {
		const f32 size = (a == 0 ? w : h) * 4.0f;

		q[a] = fmodf(-texScroll[t][a] * clock * size, size);

		if (q[a] < 0.0f) {
			q[a] += size;
		}

		whole[a] = (s32)floorf(q[a]);
		frac[a] = (u32)((q[a] - whole[a]) * 0.25f * 65536.0f) & 0xffff;
	}

	gDPSetTileSize(gdl++, 0, whole[0], whole[1], whole[0] + ((w - 1) << 2), whole[1] + ((h - 1) << 2));
	gDPSetTileSize(gdl++, 1, whole[0], whole[1], whole[0] + ((w - 1) << 2), whole[1] + ((h - 1) << 2));
	gDPSetTileOffsetEXT(gdl++, 0, frac[0], frac[1]);
	gDPSetTileOffsetEXT(gdl++, 1, frac[0], frac[1]);
	gSPEndDisplayList(gdl++);
}

const Gfx *gebeanStageScrollSlot(u32 record, s32 w, s32 h)
{
	const u32 t = record - GEBEANSTAGE_TEXBASE;

	if (!gebeanStageOwnsRecord(record) || t >= GEBEAN_MAXMATS || (texScroll[t][0] == 0.0f && texScroll[t][1] == 0.0f)) {
		return NULL;
	}

	for (s32 i = 0; i < numScrollSlots; i++) {
		if (scrollSlots[i].tex == (s32)t && scrollSlots[i].w == w && scrollSlots[i].h == h) {
			return scrollSlots[i].gdl;
		}
	}

	if (numScrollSlots >= SCROLL_SLOTS) {
		return NULL;
	}

	scrollSlots[numScrollSlots].tex = (s32)t;
	scrollSlots[numScrollSlots].w = w;
	scrollSlots[numScrollSlots].h = h;
	scrollBuild(numScrollSlots, g_Vars.lvframe60 / 60.0f);

	return scrollSlots[numScrollSlots++].gdl;
}

Gfx *gebeanStageRenderBackdrop(Gfx *gdl)
{
	const s32 numvtx = numBackdrop * 3;
	struct coord *cam;
	struct zrange zrange;
	f32 far2 = 0.0f;
	f32 k;
	f32 scale;
	Mtxf *mtx;
	Vtx *vtx;
	Col *col;
	s32 curtex = -2;
	f32 fm, fo;
	u8 rgb[3];

	// The clock GoldenEye XBLA's animated pictures are shown by
	// (xblaTexBindAnimation()): the level's, as the frame is drawn
	xblaTexSetAnimClock(g_Vars.lvframe60 / 60.0f);

	for (s32 i = 0; i < numScrollSlots; i++) {
		scrollBuild(i, g_Vars.lvframe60 / 60.0f);
	}

	if (numBackdrop == 0 || !xblaStageDrawsEveryRoom()
			|| g_Vars.currentplayer->visionmode == VISIONMODE_XRAY) {
		return gdl;
	}

	cam = &g_Vars.currentplayer->cam_pos;

	for (s32 t = 0; t < numBackdrop; t++) {
		f32 mid[3] = { 0, 0, 0 };

		for (s32 j = 0; j < 3; j++) {
			const f32 x = backdrop[t].pos[j][0] - cam->x;
			const f32 y = backdrop[t].pos[j][1] - cam->y;
			const f32 z = backdrop[t].pos[j][2] - cam->z;

			far2 = MAX(far2, x * x + y * y + z * z);
			mid[0] += x;
			mid[1] += y;
			mid[2] += z;
		}

		backdropOrder[t] = t;
		backdropDist[t] = mid[0] * mid[0] + mid[1] * mid[1] + mid[2] * mid[2];
	}

	qsort(backdropOrder, numBackdrop, sizeof(s32), compareBackdropFar);

	vtx = gfxAllocateVertices(numvtx);
	col = gfxAllocateColours(numvtx);
	mtx = gfxAllocateMatrix();

	if (far2 <= 1.0f || !vtx || !col || !mtx) {
		return gdl;
	}

	k = BACKDROP_EXTENT / sqrtf(far2);

	for (s32 t = 0; t < numBackdrop; t++) {
		const struct stri *tri = &backdrop[backdropOrder[t]];
		// a triangle's own window on the picture, which wraps
		const f32 su = floorf(tri->uv[0][0]);
		const f32 sv = floorf(tri->uv[0][1]) + 1.0f;

		for (s32 j = 0; j < 3; j++) {
			const s32 i = t * 3 + j;
			Vtx *v = &vtx[i];
			const u32 argb = tri->argb[j];

			v->x = (s16)((tri->pos[j][0] - cam->x) * k);
			v->y = (s16)((tri->pos[j][1] - cam->y) * k);
			v->z = (s16)((tri->pos[j][2] - cam->z) * k);
			v->s = (s16)((tri->uv[j][0] - su) * XBLATEX_TILE_SCALE);
			// turned over, as the rooms' are (writeLeaf())
			v->t = (s16)((sv - tri->uv[j][1]) * XBLATEX_TILE_SCALE);
			v->colour = (i % BACKDROP_BATCH) * 4;

			col[i].r = (argb >> 16) & 0xff;
			col[i].g = (argb >> 8) & 0xff;
			col[i].b = argb & 0xff;
			col[i].a = argb >> 24;
		}
	}

	// The camera's turn and nothing of its position, the farthest vertex
	// under half the far plane (the Community Edition's dome is at half)
	viGetZRange(&zrange);
	scale = zrange.far * 0.45f / BACKDROP_EXTENT;

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
	gSPClearGeometryMode(gdl++, G_ZBUFFER | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);

	// Fogged as the level's rooms are, by where it really stands: it is part
	// of the level's file and the release draws it with the level's shaders.
	// A world unit of its distance is k * scale of the eye depth it is drawn at
	if (gebeanStageFogLine(k * scale, &fm, &fo, rgb)) {
		gDPSetRenderMode(gdl++, G_RM_FOG_SHADE_A, G_RM_XLU_SURF2);
		gDPSetFogColor(gdl++, rgb[0], rgb[1], rgb[2], 0xff);
		gSPFogLineEXT(gdl++, G_FOGLINE_LINEAR_EXT, fm);
		gSPFogLineEXT(gdl++, G_FOGLINE_LINEAR_EXT | G_FOGLINE_OFFSET_EXT, fo);
		gSPSetGeometryMode(gdl++, G_FOG);
	} else {
		gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	}
	gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0);

	gSPMatrix(gdl++, osVirtualToPhysical(camGetPerspectiveMtxL()), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION | CAM_PROJ_MTX_FLAGS);
	gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);

	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);

	for (s32 first = 0; first < numvtx; first += BACKDROP_BATCH) {
		const s32 count = MIN(numvtx - first, BACKDROP_BATCH);
		s32 t;

		// a load is five whole triangles, each of one picture
		for (t = 0; t < count; t += 3) {
			const s32 tex = backdrop[backdropOrder[(first + t) / 3]].tex;

			if (tex != curtex) {
				curtex = tex;

				gDPPipeSync(gdl++);
				gDPLoadTextureBlock(gdl++, (void *)texTile[tex], G_IM_FMT_RGBA, G_IM_SIZ_16b,
						XBLATEX_TILE, XBLATEX_TILE, 0, G_TX_WRAP, G_TX_WRAP,
						XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);
			}

			if (t == 0) {
				gSPColor(gdl++, osVirtualToPhysical(&col[first]), count);
				gSPVertex(gdl++, osVirtualToPhysical(&vtx[first]), count, 0);
			}

			gSPTri1(gdl++, t, t + 1, t + 2);
		}
	}

	// The frame turned the depth test on before the sky and nothing after it
	// turns it on again (xblasky.c)
	gSPClearGeometryMode(gdl++, G_FOG);
	gSPSetGeometryMode(gdl++, G_ZBUFFER);
	gDPPipeSync(gdl++);

	return gdl;
}

/* -------------------------------------------------------------------------
 * The hooks
 * ------------------------------------------------------------------------- */

void gebeanStageLevelReset(void)
{
	forget();
	tried = 0;
}

u32 gebeanStageRoomSize(s32 roomnum)
{
	if (!gebeanGetEnabled()) {
		return 0;
	}

	if (!tried) {
		tried = 1;
		built = build();
	}

	if (!built || roomnum < 1 || roomnum >= numRooms || !roomData[roomnum]) {
		return 0;
	}

	return roomLen[roomnum];
}

uintptr_t gebeanStageRoomRead(s32 roomnum, u8 *dst, u32 len)
{
	memcpy(dst, roomData[roomnum], len);

	return g_BgRooms[roomnum].unk00;
}

/**
 * Whether the rooms are to be drawn without the portals: a converted level's
 * portals are GoldenEye's, cut for an N64 level that walled its views in
 * with opaque foliage and rock, and Bean's HD mesh opens those views up. On
 * Jungle the portals reached two rooms from the start, and the rest of the
 * HD mesh in view showed the sky colour in the shape of what was missing.
 */
s32 gebeanStageDrawsEveryRoom(void)
{
	return built && row && numServed > 0;
}

s32 gebeanStageRoomServed(s32 roomnum)
{
	return xblaStageDrawsEveryRoom() && built && roomnum > 0 && roomnum < numRooms && roomData[roomnum];
}

s32 gebeanStageRoomHidden(s32 roomnum)
{
	return xblaStageDrawsEveryRoom() && roomHidden && roomnum > 0 && roomnum < numRooms && roomHidden[roomnum];
}

const char *gebeanStageLevelKey(void)
{
	return gebeanStageDrawsEveryRoom() ? row->key : NULL;
}

s32 gebeanStageOwnsRecord(u32 record)
{
	return built && record >= GEBEANSTAGE_TEXBASE && record <= GEBEANSTAGE_TEXNONE;
}

s32 gebeanStageRecordIsWater(u32 record)
{
	const u32 t = record - GEBEANSTAGE_TEXBASE;

	return gebeanStageOwnsRecord(record) && t < GEBEAN_MAXMATS && texWater[t];
}

const void *gebeanStageTile(u32 record)
{
	const u32 t = record - GEBEANSTAGE_TEXBASE;

	return t < GEBEAN_MAXMATS ? texTile[t] : NULL;
}

/**
 * Whether the picture a served room loads from `tile` (the G_SETTIMG that
 * xblaStageWriteTexture() made of the leaf's record) is one a bullet goes
 * through: a cut-out (railings, fences, grates, leaves - texels with holes,
 * drawn as a texture edge in the opaque leaf) or a translucent one.
 * GoldenEye's bullets test a room's primary list only
 * (bgTestBulletHitBackground()), and its railings are not in it, so they never
 * stopped a shot; Bean draws them as cut-outs in the opaque leaf, where
 * bgTestHitInRoom() found them (F3 20260925-225349, Facility's stairs).
 */
s32 gebeanStageTilePassesShots(uintptr_t tile)
{
	if (!built || !tile) {
		return 0;
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		if ((uintptr_t)texTile[t] == tile) {
			return texHasAlpha(t) || texIsXlu(t);
		}
	}

	return 0;
}

s32 gebeanStageIsTile(uintptr_t tile)
{
	if (!built || !tile) {
		return 0;
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		if ((uintptr_t)texTile[t] == tile) {
			return 1;
		}
	}

	return 0;
}

/**
 * The texture number of GoldenEye's own triangle a shot at pos in room hit.
 *
 * An HD room is Bean's mesh, whose lists load the release's pictures from
 * stand-in tiles, so what a hit reads before a G_SETTIMG's image (bg.c) is
 * whatever bytes precede the tile - different from one build to the next
 * (F3 12th pass, W vs L). GoldenEye's room is still here (the shell), so the
 * hit takes the texture of its opaque triangle nearest the point, the one the
 * N64 look's hit reads: -1 (default) when none is within a few units.
 */
s32 gebeanStageHitTexture(s32 room, const struct coord *pos)
{
	const f32 p[3] = { pos->x, pos->y, pos->z };
	f32 best = 8.0f * 8.0f;
	s32 tex = -1;

	if (!built || !shellTri || !shellTex || !shellFirst || !shellCount || room <= 0 || room >= numRooms) {
		return -1;
	}

	for (s32 i = shellFirst[room]; i < shellFirst[room] + shellCount[room]; i++) {
		const f32 *v = shellTri + i * 9;
		f32 e1[3], e2[3], n[3], d[3], q[3];
		f32 len, dist, u, w, d00, d01, d11, d20, d21, den;

		for (s32 k = 0; k < 3; k++) {
			e1[k] = v[3 + k] - v[k];
			e2[k] = v[6 + k] - v[k];
			d[k] = p[k] - v[k];
		}

		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		len = sqrtf(dot3(n, n));

		if (len < 1e-6f) {
			continue;
		}

		dist = dot3(d, n) / len;

		if (dist * dist >= best) {
			continue;
		}

		// inside the triangle once projected onto its plane (barycentric)
		for (s32 k = 0; k < 3; k++) {
			q[k] = d[k] - n[k] / len * dist;
		}

		d00 = dot3(e1, e1);
		d01 = dot3(e1, e2);
		d11 = dot3(e2, e2);
		d20 = dot3(q, e1);
		d21 = dot3(q, e2);
		den = d00 * d11 - d01 * d01;

		if (den == 0.0f) {
			continue;
		}

		u = (d11 * d20 - d01 * d21) / den;
		w = (d00 * d21 - d01 * d20) / den;

		if (u < -0.01f || w < -0.01f || u + w > 1.01f) {
			continue;
		}

		best = dist * dist;
		tex = shellTex[i];
	}

	return tex;
}

void gebeanStageTrace(FILE *f)
{
	fprintf(f, "gebeanstage: tried %d built %d level %s scale %.5f, %d of %d rooms served, %d of the file's not drawn\n",
			tried, built, row ? row->bean : "-", row ? row->scale : 0.0f, numServed, numRooms ? numRooms - 1 : 0, numHidden);
	fprintf(f, "gebeanstage: camera outside the level %d (%d of %d rays on its back faces, %d of GoldenEye's triangles, %d of them unculled)\n",
			cullOutside, shellBacks, shellHits, shellNum, shellNumTwo);
	fprintf(f, "gebeanstage: %d triangles of backdrop\n", numBackdrop);
}

#else

#include <stdio.h>
#include <PR/ultratypes.h>
#include "gebeanstage.h"

u32 gebeanStageRoomSize(s32 roomnum) { return 0; }
uintptr_t gebeanStageRoomRead(s32 roomnum, u8 *dst, u32 len) { return 0; }
void gebeanStageLevelReset(void) { }
s32 gebeanStageDrawsEveryRoom(void) { return 0; }
s32 gebeanStageRoomHidden(s32 roomnum) { return 0; }
s32 gebeanStageRoomServed(s32 roomnum) { return 0; }
void gebeanStageTickCamera(s32 authored) { }
s32 gebeanStageCullsBackFaces(void) { return 0; }
const char *gebeanStageLevelKey(void) { return NULL; }
s32 gebeanStageOwnsRecord(u32 record) { return 0; }
const void *gebeanStageTile(u32 record) { return NULL; }
s32 gebeanStageRecordIsWater(u32 record) { return 0; }
s32 gebeanStageTilePassesShots(uintptr_t tile) { return 0; }
s32 gebeanStageIsTile(uintptr_t tile) { return 0; }
s32 gebeanStageHitTexture(s32 room, const struct coord *pos) { return -1; }
void gebeanStageTrace(FILE *f) { }
Gfx *gebeanStageRenderBackdrop(Gfx *gdl) { return gdl; }
const Gfx *gebeanStageScrollSlot(u32 record, s32 w, s32 h) { return NULL; }
void gebeanStageTickFar(void) { }
s32 gebeanStageFarOwn(f32 *far) { return 0; }
s32 gebeanStageFogFactor(s32 min, s32 max, f32 *fm, f32 *fo) { return 0; }
s32 gebeanStageFog(f32 *start, f32 *end, u8 *rgb) { return 0; }
s32 gebeanStageFogLine(f32 depth, f32 *mul, f32 *offset, u8 *rgb) { return 0; }
s32 gebeanStageObjFog(f32 z, f32 *frac, u8 *rgb) { return 0; }
void gebeanStageFogRoom(s32 roomnum, struct roomblock *opa, struct roomblock *xlu, s32 fog) { }
u32 gebeanStageFogTableFind(const u8 *xex, u32 len, u32 *at) { return 0; }

#endif

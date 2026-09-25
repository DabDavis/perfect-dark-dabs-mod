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
#include "romdata.h"
#include "xblatex.h"
#include "xblastage.h"
#include "gebean.h"
#include "gebeanstage.h"

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
	u8 nofog;   // on a triangle GoldenEye draws without fog (fileRoomTrianglesEach())
	u8 backed;  // one face of a two-faced sheet, drawn culled (markBacked())
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

static const void *texTile[GEBEAN_MAXMATS];
static u8 texAlpha[GEBEAN_MAXMATS];
static u8 texSoft[GEBEAN_MAXMATS];
// A cut-out whose every triangle keeps v within one repeat [k, k + 1]: drawn
// with t clamped, from a batch whose v shift is k + 1 (texClampShift[])
static u8 texClampV[GEBEAN_MAXMATS];
static s16 texClampShift[GEBEAN_MAXMATS];

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

		if (t) g->tri = t;
		if (r) g->room = r;
		if (!t || !r) return;

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

/** The triangle nearest p within `rings` cells, or -1; its squared distance in *outd. */
static s32 tgridNearest(const struct tgrid *g, const f32 *p, s32 rings, f32 *outd)
{
	const s32 cx = (s32)floorf(p[0] / g->cell);
	const s32 cy = (s32)floorf(p[1] / g->cell);
	const s32 cz = (s32)floorf(p[2] / g->cell);
	s32 best = -1;
	f32 bestd = 0;

	for (s32 r = 0; r <= rings; r++) {
		for (s32 x = cx - r; x <= cx + r; x++) {
			for (s32 y = cy - r; y <= cy + r; y++) {
				for (s32 z = cz - r; z <= cz + r; z++) {
					if (x != cx - r && x != cx + r && y != cy - r && y != cy + r && z != cz - r && z != cz + r) {
						continue;
					}

					for (s32 e = g->head[gridKey(x, y, z)]; e >= 0; e = g->entnext[e]) {
						const s32 t = g->enttri[e];
						const f32 d = pointTriDist(p, g->tri + t * 9, g->tri + t * 9 + 3, g->tri + t * 9 + 6);

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
 * draws up to four of them.
 */
static void fileRoomTrianglesEach(s32 r, const u8 *raw, u32 len, s32 xlutoo,
		void (*fn)(void *arg, const f32 v[3][3], s32 room), void *arg)
{
	const u32 base = g_BgRooms[r].unk00;
	u32 stack[64];
	s32 depth = 0;

	stack[depth++] = be32(raw + 8);

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
				// Whether the leaf's render mode is one bg.c's fog swap
				// (g_GfxGroup01/05) leaves alone: cycle 1 neither G_RM_PASS,
				// which the swap turns to fog, nor fog already. GoldenEye draws
				// Caverns' water in G_RM_AA_ZB_OPA_SURF, one cycle, unfogged
				s32 nofog = 0;

				for (u32 c = gdl - base; c + 8 <= len; c += 8) {
					const u8 op = raw[c];

					if (op == (u8)G_SETOTHERMODE_L && be32(raw + c) == 0xb900031d) {
						const u32 c1 = be32(raw + c + 4) & 0xcccc0000;

						nofog = c1 != (GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1) & 0xcccc0000)
							&& c1 != (GBL_c1(G_BL_CLR_FOG, G_BL_A_SHADE, G_BL_CLR_IN, G_BL_1MA) & 0xcccc0000);
					} else if (op == G_VTX) {
						const s32 num = (raw[c + 1] >> 4) + 1;
						const u32 at = vtx - base + (be32(raw + c + 4) & 0xffffff);

						for (s32 i = 0; i < num && at + i * VTXSIZE + 6 <= len; i++) {
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
							fn(arg, (const f32 (*)[3])v, (u16)r | (nofog << 16));
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

static void fileTriToGrid(void *arg, const f32 v[3][3], s32 room)
{
	tgridAdd(arg, v, room);
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

		if (!t) {
			return;
		}

		shellTri = t;
		shellCap = cap;
	}

	memcpy(shellTri + shellNum * 9, v, sizeof(f32) * 9);
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

static void shellForget(void)
{
	free(shellTri);
	free(shellFirst);
	free(shellCount);
	shellTri = NULL;
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
				facing = det > 0.0f ? 1 : -1;
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

	// Statue Park is the one GoldenEye level the release remodelled
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

			weight[h]++;
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
	if (ta->backed != tb->backed) {
		// solid pictures: the sheets last; cut-outs: the sheets first,
		// before the cut-outs that turn culling off
		return texHasAlpha(ta->tex) ? tb->backed - ta->backed : ta->backed - tb->backed;
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
static s32 writeLeaf(struct leaf *l, const struct stri *tris, s32 *list, s32 num, s32 xlu,
		const f32 *roompos, const u32 *palette, s32 numpal, s32 *dropped)
{
	struct batch b;
	s32 curtex = -2;
	s32 curdecal = -1;
	s32 curalpha = -1;
	s32 curnofog = 0;
	s32 curbacked = -1;
	// -1 while the solid pictures take the room's culling
	// (bgRenderRoomOpaque()), then 1 once a two-faced sheet has turned it on
	// or 0 once the cut-outs, sorted last, have turned it off
	s32 curcull = -1;

	if (num == 0) {
		return 0;
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

	for (s32 i = 0; i < num; i++) {
		const struct stri *t = &tris[list[i]];
		struct rvtx rv[3];
		u8 idx[3];
		s32 ok = 1;
		s32 need = 0;

		// Compared whole in batchFind(), padding and all
		memset(rv, 0, sizeof(rv));

		if (t->tex != curtex || t->decal != curdecal || t->nofog != curnofog || t->backed != curbacked) {
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
				// leaf's mode is a decal one already
				//
				// Where GoldenEye draws the surface without fog (Caverns'
				// water), cycle 1 is the plain pass (all zeros) instead of
				// G_RM_PASS, which the fog swap would make fog of
				if (!xlu || t->nofog != curnofog) {
					u32 mode = xlu ? 0x0c184dd8 : t->decal
							? (alpha ? G_RM_AA_ZB_XLU_DECAL | G_RM_AA_ZB_XLU_DECAL2 : G_RM_AA_ZB_OPA_DECAL | G_RM_AA_ZB_OPA_DECAL2)
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
			if (!xlu && t->backed && curcull != 1) {
				curcull = 1;
				emit(&l->gdl, 0xb7000000, 0x00002000);
			} else if (!xlu && !t->backed && texHasAlpha(t->tex) && curcull != 0) {
				curcull = 0;
				emit(&l->gdl, 0xb6000000, 0x00002000);
			}

			curbacked = t->backed;

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

		for (s32 pass = 0; pass < 2; pass++) {
			ok = 1;

			for (s32 k = 0; k < 3 && ok; k++) {
				ok = clampS16(t->pos[k][0] - roompos[0], &rv[k].x)
					&& clampS16(t->pos[k][1] - roompos[1], &rv[k].y)
					&& clampS16(t->pos[k][2] - roompos[2], &rv[k].z)
					&& clampS16((t->uv[k][0] - b.shiftu) * XBLATEX_TILE_SCALE, &rv[k].s)
					// Turned over: the picture was decoded bottom row first for the
					// renderer (beanDecodeTexture()), which the meshes undo by
					// flipping v and a room has to as well - the Temple's
					// carvings stood on their heads
					&& clampS16((b.shiftv - t->uv[k][1]) * XBLATEX_TILE_SCALE, &rv[k].t);
				rv[k].colour = paletteIndex(palette, numpal, t->argb[k]) << 2;
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

	batchFlush(l, &b);

	// Culling back off for what follows in the room, as it was before the
	// sheets unless a camera outside the level had it on (the next room
	// sets its own either way)
	if (curcull == 1) {
		emit(&l->gdl, 0xb6000000, 0x00002000);
	}

	emit(&l->gdl, 0xb8000000, 0x00000000);

	return 1;
}

/**
 * Room r in the room format, big-endian as the file stores it, its pointers
 * relative to the room's own entry in the room table (as the release's rooms
 * are served, xblastage.c): the header, an opaque and a translucent leaf, the
 * vertices of both, the shared palette, then the two display lists.
 */
static u8 *writeRoom(s32 r, const struct stri *tris, s32 *list, s32 num, const u8 *fileroom, u32 *outLen, s32 *dropped)
{
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
		if (texIsXlu(tris[list[i]].tex)) {
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

	// The converter (convertRoomGfxData()) starts the host's vertices and
	// colours each on an eight byte boundary of their own but places the
	// first display list by the file's distance from the colours, so that
	// distance must come out whole: the colours start on a boundary here too,
	// which an even number of vertices after an aligned start gives. The
	// padding is short of a block, so the block walk does not read it.
	if ((opa.numvtx + xlu.numvtx) & 1) {
		struct rvtx pad;

		memset(&pad, 0, sizeof(pad));
		leafAddVtx(xlu.gdl.len ? &xlu : &opa, &pad);
	}

	vtxat = ALIGN8(GFXHEADER + ROOMBLOCKSIZE * numblocks);
	colat = vtxat + (opa.numvtx + xlu.numvtx) * VTXSIZE;
	opagdlat = ALIGN8(colat + numpal * COLSIZE);
	xlugdlat = opagdlat + opa.gdl.len;
	total = ALIGN8(xlugdlat + xlu.gdl.len);

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
	put32(out + 0x0c, xlu.gdl.len ? base + GFXHEADER + (opa.gdl.len ? ROOMBLOCKSIZE : 0) : 0);
	// The lights are the file's room's: the level's light table is its
	memcpy(out + 0x10, fileroom + 0x10, 4);
	put16(out + 0x14, (u16)(opa.numvtx + xlu.numvtx));
	put16(out + 0x16, (u16)numpal);

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
		put32(out + at + 4, 0);
		put32(out + at + 8, base + xlugdlat);
		put32(out + at + 12, base + vtxat + opa.numvtx * VTXSIZE);
		put32(out + at + 16, base + colat);
		at += ROOMBLOCKSIZE;
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
	t->room = 0;
	t->decal = 0;
	t->nofog = 0;
	t->backed = 0;
}

static f32 triNormal(const struct stri *t, f32 *n)
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

		for (s32 k = 0; k < 4 && covered; k++) {
			const f32 *q = pts[k];

			covered = 0;

			for (s32 e = g->head[gridKey((s32)floorf(q[0] / g->cell), (s32)floorf(q[1] / g->cell), (s32)floorf(q[2] / g->cell))];
					e >= 0 && !covered; e = g->entnext[e]) {
				const s32 o = g->room[g->enttri[e]];
				const struct stri *u = &tris[o];
				f32 nu[3];

				if (o == i || texIsXlu(u->tex) || texHasAlpha(u->tex) != texHasAlpha(t->tex)
						|| triNormal(u, nu) <= 0 || dot3(ni, nu) > -DECAL_COS) {
					continue;
				}

				covered = pointTriDist(q, u->pos[0], u->pos[1], u->pos[2]) <= DECAL_DIST * DECAL_DIST;
			}
		}

		if (covered) {
			t->backed = 1;
			count++;
		}
	}

	return count;
}

/**
 * Marks the triangles that lie flat on another picture's triangle. Bean's
 * decals share the plane of the surface under them exactly, and drawn with
 * the ordinary depth test the two fought (a tester's F3 on Aztec, every HD
 * level): the decal is drawn in a decal render mode instead, pulled towards
 * the camera. Of a pair, the one with a cut-out picture over the one without
 * is the decal, else the smaller, else the one Bean draws later.
 */
static s32 markDecals(struct stri *tris, s32 num, const struct tgrid *g)
{
	s32 count = 0;

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

			if (o == i || u->tex == t->tex) {
				continue;
			}

			au = triNormal(u, nu);
			cosang = dot3(ni, nu);

			if (au <= 0 || (cosang < DECAL_COS && cosang > -DECAL_COS)) {
				continue;
			}

			// Back to back and drawn culled (markBacked()): each side shows
			// its own face, and a decal would show from behind as well
			if (cosang < 0 && t->backed && u->backed) {
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

			if (alphai != alphau ? alphai > alphau
					: ai < au * 0.999f ? 1
					: ai <= au * 1.001f && i > o) {
				t->decal = 1;
				count++;
			}
		}
	}

	return count;
}

static void forget(void)
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
	free(backdrop);
	free(backdropOrder);
	free(backdropDist);
	backdrop = NULL;
	backdropOrder = NULL;
	backdropDist = NULL;
	numBackdrop = 0;
	farRaised = 0;
	roomData = NULL;
	roomLen = NULL;
	roomHidden = NULL;
	numRooms = 0;
	numServed = 0;
	numHidden = 0;
	gebeanLevelClose(level);
	level = NULL;
	row = NULL;
	built = 0;
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
 */
static void clampCutouts(const struct collect *c)
{
	f32 vmin[GEBEAN_MAXMATS];
	f32 vmax[GEBEAN_MAXMATS];

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		vmin[t] = 1e30f;
		vmax[t] = -1e30f;
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
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		// the UVs are whole 1/256ths, or 1/1024ths, of a repeat
		const f32 k = floorf(vmin[t] + 1.0f / 2048.0f);

		if (texAlpha[t] && vmin[t] <= vmax[t] && vmax[t] <= k + 1.0f + 1.0f / 2048.0f) {
			texClampV[t] = 1;
			texClampShift[t] = (s16)(k + 1.0f);
		}
	}
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
	s32 kept = 0, dropped = 0, moved = 0, farOff = 0, decals = 0, backed = 0, nofogs = 0;
	u32 bytes = 0;

	row = levelRow();

	if (!row) {
		return 0;
	}

	// the Community Edition keeps Surface's two halves as files of their own
	level = gebeanLevelOpen(gebeanCeLevelName(row->key, row->bean));

	if (!level) {
		sysLogPrintf(LOG_WARNING, "gebeanstage: GoldenEye XBLA's %s is not on disk", row->bean);
		return 0;
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		texTile[t] = NULL;
		texAlpha[t] = texSoft[t] = texClampV[t] = 0;
		texClampShift[t] = 0;
	}

	for (s32 t = 0; t < gebeanLevelNumTextures(level) && t < GEBEAN_MAXMATS; t++) {
		texTile[t] = gebeanLevelTexture(level, t, &texAlpha[t], &texSoft[t]);
	}

	mark[0] = sysGetMicroseconds();

	// The level file's rooms: where their vertices are, for dealing Bean's out
	// and for telling which rooms Bean has
	filerooms = calloc(n + 1, sizeof(*filerooms));
	filelens = calloc(n + 1, sizeof(*filelens));

	for (s32 r = 1; r < n && filerooms; r++) {
		filerooms[r] = readRoom(r, &filelens[r]);
	}

	memset(&c, 0, sizeof(c));
	c.scale = row->scale;
	c.offset = row->offset;
	gebeanLevelTriangles(level, collectTri, &c);

	takeBackdrop(&c, n);
	clampCutouts(&c);

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
	numRooms = n;

	if (!filerooms || !filelens || !lists || !listlen || !roomData || !roomLen || c.num == 0
			|| !tgridInit(&filetris, ASSIGN_CELL)
			|| !tgridInit(&beantris, BEAN_CELL)) {
		sysLogPrintf(LOG_ERROR, "gebeanstage: %s: could not build", row->bean);
		// fall through to the frees; nothing is served
		c.num = 0;
	}

	if (c.num) {
		shellFirst = calloc(n + 1, sizeof(*shellFirst));
		shellCount = calloc(n + 1, sizeof(*shellCount));

		for (s32 r = 1; r < n; r++) {
			if (filerooms[r]) {
				fileRoomTriangles(&filetris, r, filerooms[r], filelens[r]);

				if (shellFirst && shellCount) {
					shellFirst[r] = shellNum;
					fileRoomTrianglesEach(r, filerooms[r], filelens[r], 0, fileTriToShell, NULL);
					shellCount[r] = shellNum - shellFirst[r];
				}
			}
		}

		if (!shellFirst || !shellCount) {
			shellForget();
		}

		for (s32 t = 0; t < c.num; t++) {
			tgridAdd(&beantris, (const f32 (*)[3])c.tris[t].pos, t);
		}

		backed = markBacked(c.tris, c.num, &beantris);
		decals = markDecals(c.tris, c.num, &beantris);

		mark[2] = sysGetMicroseconds();

		// Deal each triangle to the room whose own triangle its middle lies
		// on. Rooms share the vertices along their borders, so the nearest
		// vertex named the room next door for a big floor triangle about as
		// often as not, and that room's portal scissored it away.
		for (s32 t = 0; t < c.num; t++) {
			struct stri *tri = &c.tris[t];
			f32 mid[3];

			for (s32 j = 0; j < 3; j++) {
				mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
			}

			{
				f32 d;
				const s32 near = tgridNearest(&filetris, mid, ASSIGN_RINGS, &d);

				const s32 file = near >= 0 ? filetris.room[near] : nearestRoomBox(mid, n);

				tri->room = file & 0xffff;
				tri->nofog = file >> 16;
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

			roomData[r] = writeRoom(r, c.tris, lists[r], listlen[r], filerooms[r], &roomLen[r], &dropped);

			if (roomData[r]) {
				numServed++;
				bytes += roomLen[r];
			} else {
				kept++;
			}
		}

		tgridFree(&filetris);
		tgridFree(&beantris);
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

	sysLogPrintf(LOG_NOTE, "gebeanstage: %s (GoldenEye's %s) at scale %.5f: %d of %d rooms from GoldenEye XBLA (%d kept, %d of them not drawn), %d triangles (%d decals, %d two-faced, %d unfogged), %u bytes, %d triangles off a room's range (%d dealt to another in reach, %d to the backdrop), %.0f ms (pictures %.0f, mesh %.0f, grids %.0f, dealing %.0f, writing %.0f)",
			row->bean, row->key, row->scale, numServed, n - 1, kept, numHidden, c.num, decals, backed, nofogs, bytes, dropped, moved, farOff,
			(sysGetMicroseconds() - start) / 1000.0,
			(mark[0] - start) / 1000.0, (mark[1] - mark[0]) / 1000.0, mark[2] ? (mark[2] - mark[1]) / 1000.0 : 0.0,
			mark[3] ? (mark[3] - mark[2]) / 1000.0 : 0.0,
			mark[3] ? (sysGetMicroseconds() - mark[3]) / 1000.0 : 0.0);

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

		sysLogPrintf(LOG_NOTE, "gebeanstage: far plane %.0f -> %.0f for the %s", farOwn, want,
				xblaStageDrawsEveryRoom() ? "HD level" : "level without its fog");

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
 * Facility's colour in the release is 0x102001, which reads as GoldenEye's
 * 0x102010 with its last two nibbles swapped (every other row keeps
 * GoldenEye's colour or a new one outright); GoldenEye's is used.
 */
struct beanfog {
	const char *key; // GoldenEye's key for the level (stageRows[])
	s32 end;         // where the fog is whole
	u32 rgb;
};

static const struct beanfog beanFogs[] = {
	{ "stat",  3500,   0x000008 },
	{ "arec",  10000,  0x000000 },
	{ "arch",  3000,   0x000000 },
	{ "tra",   1500,   0x000008 },
	{ "sevb",  10000,  0x100000 },
	{ "azt",   15000,  0x000000 },
	{ "pete",  7500,   0x101820 },
	{ "depo",  5000,   0x000008 },
	{ "ref",   5000,   0x280000 },
	{ "cryp",  20000,  0x103060 },
	{ "dam",   15000,  0x85adca },
	{ "ark",   5000,   0x102010 },
	{ "run",   55000,  0x85adca },
	{ "sevx",  45000,  0xa49682 },
	{ "jun",   2500,   0x182000 },
	{ "dish",  6000,   0x181828 },
	{ "cave",  6000,   0x080008 },
	{ "crad",  30000,  0x6080a0 },
	{ "sevxb", 10000,  0x201010 },
};

// Where every level's fog starts (the environment tick's -100)
#define BEANFOG_START -100.0f

// Dam's fog reaches further the lower the player is: the tick adds 65000 at
// 2782 and under, nothing at 9161 and over (GoldenEye's world heights, the
// constants before the table). The conversion moved Dam up by 13219.
#define BEANFOG_DAM_REACH 65000.0f
#define BEANFOG_DAM_LOW   (2782.0f + 13219.0f)
#define BEANFOG_DAM_HIGH  (9161.0f + 13219.0f)

static const struct beanfog *fogRow(void)
{
	if (!row) {
		return NULL;
	}

	for (u32 i = 0; i < ARRAYCOUNT(beanFogs); i++) {
		if (strcmp(beanFogs[i].key, row->key) == 0) {
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
	if (strcmp(f->key, "dam") == 0) {
		const f32 y = g_Vars.currentplayer->cam_pos.y;
		const f32 t = (BEANFOG_DAM_HIGH - y) / (BEANFOG_DAM_HIGH - BEANFOG_DAM_LOW);

		*end += BEANFOG_DAM_REACH * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
	}

	rgb[0] = f->rgb >> 16;
	rgb[1] = f->rgb >> 8;
	rgb[2] = f->rgb;

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
 * A room served from the HD level, once bg.c's fog swap has run over it: every
 * render mode left without the fog blend in its first cycle takes it. The swap
 * knows GoldenEye's own modes; the HD rooms also draw cut-outs (the pines, the
 * forest wall, Jungle's leaves: the texture edge with 1 - alpha, 0x0c183078),
 * decals in the decal modes, and the surfaces GoldenEye drew unfogged, and the
 * release fogs every one of them.
 */
void gebeanStageFogRoom(s32 roomnum, struct roomblock *opa, struct roomblock *xlu)
{
	struct roomblock *blocks[2] = { opa, xlu };

	if (!built || roomnum < 1 || roomnum >= numRooms || !roomData[roomnum] || !xblaStageIsRelease()) {
		return;
	}

	for (s32 i = 0; i < 2; i++) {
		struct roomblock *stack[16];
		s32 depth = 0;
		struct roomblock *block = blocks[i];

		while (block || depth > 0) {
			if (!block) {
				block = stack[--depth];
				continue;
			}

			if (block->type == ROOMBLOCKTYPE_LEAF) {
				for (Gfx *gdl = block->gdl; gdl && (u8)(gdl->words.w0 >> 24) != (u8)G_ENDDL; gdl++) {
					if ((u32)gdl->words.w0 == 0xb900031d
							&& ((u32)gdl->words.w1 & 0xcccc0000) != (G_RM_FOG_SHADE_A & 0xcccc0000)) {
						gdl->words.w1 = ((u32)gdl->words.w1 & ~0xcccc0000) | (G_RM_FOG_SHADE_A & 0xcccc0000);
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

	gSPMatrix(gdl++, osVirtualToPhysical(camGetPerspectiveMtxL()), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
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

const void *gebeanStageTile(u32 record)
{
	const u32 t = record - GEBEANSTAGE_TEXBASE;

	return t < GEBEAN_MAXMATS ? texTile[t] : NULL;
}

void gebeanStageTrace(FILE *f)
{
	fprintf(f, "gebeanstage: tried %d built %d level %s scale %.5f, %d of %d rooms served, %d of the file's not drawn\n",
			tried, built, row ? row->bean : "-", row ? row->scale : 0.0f, numServed, numRooms ? numRooms - 1 : 0, numHidden);
	fprintf(f, "gebeanstage: camera outside the level %d (%d of %d rays on its back faces, %d of GoldenEye's triangles)\n",
			cullOutside, shellBacks, shellHits, shellNum);
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
void gebeanStageTickCamera(s32 authored) { }
s32 gebeanStageCullsBackFaces(void) { return 0; }
const char *gebeanStageLevelKey(void) { return NULL; }
s32 gebeanStageOwnsRecord(u32 record) { return 0; }
const void *gebeanStageTile(u32 record) { return NULL; }
void gebeanStageTrace(FILE *f) { }
Gfx *gebeanStageRenderBackdrop(Gfx *gdl) { return gdl; }
void gebeanStageTickFar(void) { }
s32 gebeanStageFarOwn(f32 *far) { return 0; }
s32 gebeanStageFogFactor(s32 min, s32 max, f32 *fm, f32 *fo) { return 0; }
s32 gebeanStageFog(f32 *start, f32 *end, u8 *rgb) { return 0; }
s32 gebeanStageFogLine(f32 depth, f32 *mul, f32 *offset, u8 *rgb) { return 0; }
s32 gebeanStageObjFog(f32 z, f32 *frac, u8 *rgb) { return 0; }
void gebeanStageFogRoom(s32 roomnum, struct roomblock *opa, struct roomblock *xlu) { }

#endif

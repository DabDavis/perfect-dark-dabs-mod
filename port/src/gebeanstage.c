/**
 * GoldenEye XBLA's HD levels as the rooms of GoldenEye X's - gebeanstage.h
 * has the shape of it, CLAUDE-notes/ge-bean.md "The levels" the reasons.
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
#include "xblatex.h"
#include "gebean.h"
#include "gebeanstage.h"

#define SEG 0x0f000000

// The room format's sizes, as the file has them (preprocess/filebg.c)
#define GFXHEADER 0x18
#define ROOMBLOCKSIZE 20
#define VTXSIZE   12
#define COLSIZE   4

// A GE-X room is taken over only when Bean's mesh lies over this share of its
// surface: the area of its triangles whose middles have a Bean triangle within
// the distance (GE-X units). GE-X rebuilt some rooms of the levels it kept,
// and Bean's mesh there is GoldenEye's, not the room the tiles collide with.
// The distance is not small because GE-X rounded a room's position and its
// vertices' offsets from it each to a whole unit, which puts a GoldenEye
// vertex up to a unit and a half off on each axis; at 1.5 Frigate kept all 34
// of its rooms.
#define COVER_DIST  3.0f
#define COVER_SHARE 0.97f
#define COVER_CELL  64.0f
// Nor when one GE-X triangle bigger than this (square units) is missing from
// Bean's mesh however much else is there: the stair pits GE-X cut into two of
// Archives BZ's rooms are a quad each, 2 to 3% of the room, and drew as holes
#define COVER_HOLE  20000.0f

// A Bean triangle goes to the room of the nearest GE-X triangle, looked for
// this many grid cells out.
#define ASSIGN_CELL  64.0f
#define ASSIGN_RINGS 2

#define MAXPALETTE 64
#define BATCHVERTS 16

struct stagerow {
	s32 rooms;
	u32 hash;
	const char *bean;
	f32 scale;
	// Taken off after scaling: GE-X kept GoldenEye's origin, the converted
	// arenas (.xbla-work/ge-arena/geconvert.py) moved it to their walkable middle
	f32 offset[3];
	// The level file was converted from GoldenEye's own data, which Bean's is
	// built on, so no room of it was changed and none is checked: Bean's HD
	// terrain is re-meshed (Runway's rooms read 10 to 80% covered) and the
	// hole size is in the file's units, which a 7x level (Caves) outgrows
	s32 trusted;
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

static const void *texTile[GEBEAN_MAXMATS];
static u8 texAlpha[GEBEAN_MAXMATS];
static u8 texSoft[GEBEAN_MAXMATS];

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
	u16 *room;
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

static void tgridAdd(struct tgrid *g, const f32 v[3][3], u16 room)
{
	s32 lo[3], hi[3];
	s32 cells = 1;

	if (g->numtri >= g->captri) {
		s32 cap = g->captri ? g->captri * 2 : 16384;
		f32 *t = realloc(g->tri, sizeof(f32) * 9 * cap);
		u16 *r = realloc(g->room, sizeof(u16) * cap);

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
 * GE-X's rooms as they are
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
 * Files every triangle of a GE-X room, as its display lists draw them: each
 * leaf's G_VTX loads up to 16 of the leaf's vertices, each G_TRI4 draws up to
 * four of them.
 */
static void fileRoomTriangles(struct tgrid *g, s32 r, const u8 *raw, u32 len)
{
	const u32 base = g_BgRooms[r].unk00;
	u32 stack[64];
	s32 depth = 0;

	stack[depth++] = be32(raw + 8);
	stack[depth++] = be32(raw + 12);

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

				for (u32 c = gdl - base; c + 8 <= len; c += 8) {
					const u8 op = raw[c];

					if (op == G_VTX) {
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
							tgridAdd(g, v, (u16)r);
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

static u32 levelHash(void)
{
	u32 h = 0x811c9dc5u;

	for (s32 r = 1; r <= g_Vars.roomcount; r++) {
		for (s32 k = 0; k < 3; k++) {
			const f32 v = k == 0 ? g_BgRooms[r].pos.x : k == 1 ? g_BgRooms[r].pos.y : g_BgRooms[r].pos.z;
			const s32 i = (s32)lroundf(v);

			for (s32 b = 0; b < 4; b++) {
				h = (h ^ (((u32)i >> (8 * b)) & 0xff)) * 0x01000193u;
			}
		}
	}

	return h;
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

static s32 texIsXlu(s32 tex)
{
	return tex >= 0 && texSoft[tex];
}

static s32 texHasAlpha(s32 tex)
{
	return tex >= 0 && texAlpha[tex];
}

static const struct stri *sortTris;

static int compareTex(const void *a, const void *b)
{
	const s32 ta = sortTris[*(const s32 *)a].tex;
	const s32 tb = sortTris[*(const s32 *)b].tex;

	return ta != tb ? ta - tb : *(const s32 *)a - *(const s32 *)b;
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
	s32 curalpha = -1;

	if (num == 0) {
		return 0;
	}

	sortTris = tris;
	qsort(list, num, sizeof(*list), compareTex);

	memset(&b, 0, sizeof(b));

	emit(&l->gdl, 0xe7000000, 0x00000000);
	emit(&l->gdl, 0xba001402, 0x00100000);
	emit(&l->gdl, 0xb900031d, xlu ? 0x0c184dd8 : 0x0c182078);
	emit(&l->gdl, 0xba001001, 0x00010000);
	emit(&l->gdl, 0xba001102, 0x00000000);
	emit(&l->gdl, 0xba000c02, 0x00002000);
	// GoldenEye X's rooms cull back faces; Bean's winding is not known to
	// agree, so both sides are drawn
	emit(&l->gdl, 0xb6000000, 0x00002000);
	// The alpha combiner reads the environment colour, which is whatever the
	// last list left it as unless it is set here, as GE-X's lists set it
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

		if (t->tex != curtex) {
			const s32 alpha = xlu || texHasAlpha(t->tex);

			batchFlush(l, &b);

			if (alpha != curalpha) {
				emit(&l->gdl, 0xfc26a004, alpha ? 0x1f1093ff : 0x1ffc93fc);

				// A cut-out picture in the opaque leaf is drawn as a texture
				// edge (CVG_X_ALPHA), which the renderer discards under a fifth
				// alpha. Without it the clear texels of Jungle's leaves wrote
				// depth, and a room drawn after them showed the sky colour in
				// the shape of the leaf
				if (!xlu) {
					emit(&l->gdl, 0xb900031d, alpha ? 0x0c183078 : 0x0c182078);
				}

				curalpha = alpha;
			}

			emit(&l->gdl, alpha ? 0xbb002801 : 0xbb003001, 0xffffffff);
			emit(&l->gdl, 0xc0080002, t->tex >= 0 ? GEBEANSTAGE_TEXBASE + t->tex : GEBEANSTAGE_TEXNONE);
			curtex = t->tex;
		}

		if (!b.shifted) {
			b.shiftu = floorf(t->uv[0][0]);
			b.shiftv = floorf(t->uv[0][1]);
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

			// Outside this batch's texture window: start one centred on this triangle
			batchFlush(l, &b);
			b.shiftu = floorf(t->uv[0][0]);
			b.shiftv = floorf(t->uv[0][1]);
			b.shifted = 1;
		}

		if (!ok) {
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
	emit(&l->gdl, 0xb8000000, 0x00000000);

	return 1;
}

/**
 * Room r in the room format, big-endian as the file stores it, its pointers
 * relative to the room's own entry in the room table (as the release's rooms
 * are served, xblastage.c): the header, an opaque and a translucent leaf, the
 * vertices of both, the shared palette, then the two display lists.
 */
static u8 *writeRoom(s32 r, const struct stri *tris, s32 *list, s32 num, const u8 *gexroom, u32 *outLen, s32 *dropped)
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
	// The lights are GE-X's room's: the level's light table is its
	memcpy(out + 0x10, gexroom + 0x10, 4);
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
}

static void forget(void)
{
	if (roomData) {
		for (s32 r = 0; r <= numRooms; r++) {
			free(roomData[r]);
		}
	}

	free(roomData);
	free(roomLen);
	roomData = NULL;
	roomLen = NULL;
	numRooms = 0;
	numServed = 0;
	gebeanLevelClose(level);
	level = NULL;
	row = NULL;
	built = 0;
}

static s32 build(void)
{
	const u64 start = sysGetMicroseconds();
	const s32 n = g_Vars.roomcount; // rooms 1 to n - 1, n being the end entry
	const u32 hash = levelHash();
	u8 **gexrooms;
	u32 *gexlens;
	struct tgrid gextris, beantris;
	u64 mark[5] = { 0 };
	f32 *area, *areacovered, *uncoveredmax;
	struct collect c;
	s32 **lists;
	s32 *listlen;
	s32 kept = 0, dropped = 0;
	u32 bytes = 0;

	row = NULL;

	for (s32 i = 0; i < ARRAYCOUNT(stageRows); i++) {
		if (stageRows[i].rooms == n && stageRows[i].hash == hash) {
			row = &stageRows[i];
			break;
		}
	}

	if (!row) {
		return 0;
	}

	level = gebeanLevelOpen(row->bean);

	if (!level) {
		sysLogPrintf(LOG_WARNING, "gebeanstage: GoldenEye XBLA's %s is not on disk", row->bean);
		return 0;
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		texTile[t] = NULL;
		texAlpha[t] = texSoft[t] = 0;
	}

	for (s32 t = 0; t < gebeanLevelNumTextures(level) && t < GEBEAN_MAXMATS; t++) {
		texTile[t] = gebeanLevelTexture(level, t, &texAlpha[t], &texSoft[t]);
	}

	mark[0] = sysGetMicroseconds();

	// GE-X's rooms: where their vertices are, for dealing Bean's out and for
	// telling which rooms Bean has
	gexrooms = calloc(n + 1, sizeof(*gexrooms));
	gexlens = calloc(n + 1, sizeof(*gexlens));

	for (s32 r = 1; r < n && gexrooms; r++) {
		gexrooms[r] = readRoom(r, &gexlens[r]);
	}

	memset(&c, 0, sizeof(c));
	c.scale = row->scale;
	c.offset = row->offset;
	gebeanLevelTriangles(level, collectTri, &c);

	mark[1] = sysGetMicroseconds();

	lists = calloc(n + 1, sizeof(*lists));
	listlen = calloc(n + 1, sizeof(*listlen));
	roomData = calloc(n + 1, sizeof(*roomData));
	roomLen = calloc(n + 1, sizeof(*roomLen));
	numRooms = n;

	if (!gexrooms || !gexlens || !lists || !listlen || !roomData || !roomLen || c.num == 0
			|| !tgridInit(&gextris, ASSIGN_CELL)
			|| !tgridInit(&beantris, COVER_CELL)) {
		sysLogPrintf(LOG_ERROR, "gebeanstage: %s: could not build", row->bean);
		// fall through to the frees; nothing is served
		c.num = 0;
	}

	if (c.num) {
		for (s32 r = 1; r < n; r++) {
			if (gexrooms[r]) {
				fileRoomTriangles(&gextris, r, gexrooms[r], gexlens[r]);
			}
		}

		for (s32 t = 0; t < c.num; t++) {
			tgridAdd(&beantris, (const f32 (*)[3])c.tris[t].pos, 0);
		}

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
				const s32 near = tgridNearest(&gextris, mid, ASSIGN_RINGS, &d);

				tri->room = near >= 0 ? gextris.room[near] : nearestRoomBox(mid, n);
			}

			if (tri->room > 0) {
				listlen[tri->room]++;
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

		// A room is Bean's when Bean's mesh lies over its own surface: the share
		// of the area of GE-X's triangles whose middle has one of Bean's
		// triangles on it. A share of its vertices was asked first, and passed
		// a room of Archives BZ that GE-X had cut a stair pit into, since the
		// pit's corners are on Bean's floor - its missing floor showed the
		// clear colour.
		mark[3] = sysGetMicroseconds();
		area = calloc(n + 1, sizeof(*area));
		areacovered = calloc(n + 1, sizeof(*areacovered));
		uncoveredmax = calloc(n + 1, sizeof(*uncoveredmax));

		for (s32 t = 0; area && areacovered && t < gextris.numtri; t++) {
			const f32 *v = gextris.tri + t * 9;
			const s32 r = gextris.room[t];
			f32 e1[3], e2[3], cr[3], mid[3], d;
			f32 a;

			for (s32 k = 0; k < 3; k++) {
				e1[k] = v[3 + k] - v[k];
				e2[k] = v[6 + k] - v[k];
				mid[k] = (v[k] + v[3 + k] + v[6 + k]) / 3.0f;
			}

			cr[0] = e1[1] * e2[2] - e1[2] * e2[1];
			cr[1] = e1[2] * e2[0] - e1[0] * e2[2];
			cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
			a = sqrtf(dot3(cr, cr)) * 0.5f;

			area[r] += a;

			if (tgridNearest(&beantris, mid, 1, &d) >= 0 && d <= COVER_DIST * COVER_DIST) {
				areacovered[r] += a;
			} else if (uncoveredmax && a > uncoveredmax[r]) {
				uncoveredmax[r] = a;
			}
		}

		mark[4] = sysGetMicroseconds();

		for (s32 r = 1; r < n; r++) {
			const f32 share = COVER_SHARE;

			if (!gexrooms[r] || listlen[r] == 0 || !area || !areacovered || !uncoveredmax) {
				kept++;
				continue;
			}

			if (!row->trusted && (area[r] <= 0 || areacovered[r] < area[r] * share || uncoveredmax[r] > COVER_HOLE)) {
				sysLogPrintf(LOG_NOTE, "gebeanstage: %s room %d stays GE-X's: Bean's mesh is on %.1f%% of its surface, largest triangle it misses %.0f",
						row->bean, r, area[r] > 0 ? 100.0f * areacovered[r] / area[r] : 0.0f, uncoveredmax[r]);
				kept++;
				continue;
			}

			roomData[r] = writeRoom(r, c.tris, lists[r], listlen[r], gexrooms[r], &roomLen[r], &dropped);

			if (roomData[r]) {
				numServed++;
				bytes += roomLen[r];
			} else {
				kept++;
			}
		}

		tgridFree(&gextris);
		tgridFree(&beantris);
		free(area);
		free(areacovered);
		free(uncoveredmax);
	}

	for (s32 r = 0; r <= n; r++) {
		if (gexrooms) {
			free(gexrooms[r]);
		}

		if (lists) {
			free(lists[r]);
		}
	}

	free(gexrooms);
	free(gexlens);
	free(lists);
	free(listlen);
	free(c.tris);

	sysLogPrintf(LOG_NOTE, "gebeanstage: %s at scale %.5f: %d of %d rooms from GoldenEye XBLA (%d kept), %d triangles, %u bytes, %d triangles off a room's range, %.0f ms (pictures %.0f, mesh %.0f, grids %.0f, dealing %.0f, coverage %.0f, writing %.0f)",
			row->bean, row->scale, numServed, n - 1, kept, c.num, bytes, dropped,
			(sysGetMicroseconds() - start) / 1000.0,
			(mark[0] - start) / 1000.0, (mark[1] - mark[0]) / 1000.0, mark[2] ? (mark[2] - mark[1]) / 1000.0 : 0.0,
			mark[3] ? (mark[3] - mark[2]) / 1000.0 : 0.0, mark[4] ? (mark[4] - mark[3]) / 1000.0 : 0.0,
			mark[4] ? (sysGetMicroseconds() - mark[4]) / 1000.0 : 0.0);

	return numServed > 0;
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
 * Whether the rooms are to be drawn without the portals: a converted arena's
 * portals are GoldenEye's, cut for an N64 level that walled its views in
 * with opaque foliage and rock, and Bean's HD mesh opens those views up. On
 * Jungle the portals reached two rooms from the start, and the rest of the
 * HD mesh in view showed the sky colour in the shape of what was missing.
 */
s32 gebeanStageDrawsEveryRoom(void)
{
	return built && row && row->trusted && numServed > 0;
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
	fprintf(f, "gebeanstage: tried %d built %d level %s scale %.5f, %d of %d rooms served\n",
			tried, built, row ? row->bean : "-", row ? row->scale : 0.0f, numServed, numRooms ? numRooms - 1 : 0);
}

#else

#include <stdio.h>
#include <PR/ultratypes.h>
#include "gebeanstage.h"

u32 gebeanStageRoomSize(s32 roomnum) { return 0; }
uintptr_t gebeanStageRoomRead(s32 roomnum, u8 *dst, u32 len) { return 0; }
void gebeanStageLevelReset(void) { }
s32 gebeanStageDrawsEveryRoom(void) { return 0; }
s32 gebeanStageOwnsRecord(u32 record) { return 0; }
const void *gebeanStageTile(u32 record) { return NULL; }
void gebeanStageTrace(FILE *f) { }

#endif

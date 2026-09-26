/**
 * Clip Decals at Edges - see wallhitclip.h.
 *
 * F3 20260925-224436 (Parabolee, G5 Building): "the blood decals hovering when
 * they go over a surface edge". wallhitCreateWith20Args() lays each mark out
 * as a quad of its own size on the plane of the triangle the shot, splat or
 * explosion hit, and the N64 draws all of it: past the end of a ledge the rest
 * of the splat floats over the drop.
 *
 * **What is kept.** The quad is cut down to the room triangles that lie in its
 * plane, found the way bgTestHitInRoom() finds the one that was hit: the
 * rooms' vertex batches, which are the geometry the rooms draw - the ROM's,
 * the XBLA release's (xblastage.c) or GoldenEye's, converted or HD
 * (gebeanstage.c) alike. A triangle counts when it faces within about 37
 * degrees of the quad and all of it that lies under the quad is within a
 * couple of units of the quad's plane (more for a big scorch), so a floor
 * tessellated over gentle terrain still holds the whole splat while a ledge's
 * face, the floor below it or a wall at a corner do not. Where two of those
 * triangles overlap (a level's own coplanar decal on a floor, a two-sided
 * sheet) the overlap is kept once, so a translucent mark is not doubled.
 *
 * The clip is worked out in the quad's own coordinates, (a, b) in [-1, 1]
 * along its two axes, so the texture coordinates and the corner colours follow
 * from where a point is, and an expanding blood splat (drawn at a fraction of
 * its size about its centre while it spreads, wallhitsTick()) is the stored
 * clip cut again to that smaller square each frame. A mark whose quad the
 * triangles cover whole - nearly all of them - keeps no clip and is drawn by
 * the stock quad.
 *
 * **When.** Once per mark, from wallhitsTick(), at most CLIP_PERTICK a tick;
 * until then, and forever when the setting is off, the stock quad is drawn. A
 * room that could hold part of the surface must be loaded to be read, so a
 * mark next to an unloaded one waits (CLIP_RETRYTICKS) rather than being cut
 * where that room's floor goes on. Marks on props and doors are left alone:
 * their triangles are a model's, under its matrices.
 */

#include <math.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "gbiex.h"
#include "game/bg.h"
#include "game/gfxmemory.h"
#include "game/modoptions.h"
#include "lib/memp.h"
#include "wallhitclip.h"

#define CLIP_UNTRIED 0
#define CLIP_WHOLE   1 // drawn as the stock quad: nothing hangs over, or no clip could be made
#define CLIP_CLIPPED 2

#define CLIP_MAXPIECES   32  // convex pieces kept for one mark
#define CLIP_MAXVERTS    160 // and their corners, all told
#define CLIP_MAXTRIS     256 // triangles in the quad's plane, before giving up
#define CLIP_MAXWORK     64  // pieces while one triangle's overlaps are taken off
#define CLIP_POLYMAX     16  // corners of one piece: the square cut by a triangle and the edges of others
#define CLIP_PERTICK     8
#define CLIP_RETRYTICKS  30
#define CLIP_FACING      0.8f  // cosine: a triangle facing further from the quad than this is another surface
#define CLIP_MINAREA     1e-6f // of the square's 4

struct wallhitclip {
	u8 state;
	u8 numpieces;
	u16 numverts;
	s32 retryframe;
	f32 scale;
	u8 piecesizes[CLIP_MAXPIECES];
	f32 ab[CLIP_MAXVERTS][2];
};

struct clippoly {
	s32 n;
	f32 p[CLIP_POLYMAX][2];
};

struct cliptri {
	f32 p[3][2]; // counter-clockwise in (a, b)
	f32 min[2];
	f32 max[2];
};

// The quad in its room's coordinates: mid + a * u + b * v, with n its unit
// normal and the inverse of u and v's Gram matrix to take a point back to
// (a, b) - the axes are not quite square once the corners are rounded
struct clipframe {
	f32 mid[3];
	f32 u[3];
	f32 v[3];
	f32 n[3];
	f32 inv[2][2];
};

static struct wallhitclip *g_WallhitClips;
static s32 g_WallhitClipsMax;
static s32 g_WallhitClipBudgetFrame = -1;
static s32 g_WallhitClipBudgetUsed;

static struct cliptri g_ClipTris[CLIP_MAXTRIS];
static s32 g_ClipNumTris;
static bool g_ClipOverflow;

static struct wallhitclip *wallhitClipFor(struct wallhit *wallhit)
{
	s32 index;

	if (g_WallhitClips == NULL || wallhit == NULL) {
		return NULL;
	}

	index = wallhit - g_Wallhits;

	if (index < 0 || index >= g_WallhitClipsMax) {
		return NULL;
	}

	return &g_WallhitClips[index];
}

void wallhitClipReset(void)
{
	u32 size;

	g_WallhitClips = NULL;
	g_WallhitClipsMax = 0;

	if (g_WallhitsMax <= 0 || g_Wallhits == NULL) {
		return;
	}

	size = ALIGN16(g_WallhitsMax * sizeof(struct wallhitclip));
	g_WallhitClips = mempAlloc(size, MEMPOOL_STAGE);

	if (g_WallhitClips != NULL) {
		memset(g_WallhitClips, 0, size);
		g_WallhitClipsMax = g_WallhitsMax;
	}
}

void wallhitClipBegin(struct wallhit *wallhit)
{
	struct wallhitclip *clip = wallhitClipFor(wallhit);

	if (clip) {
		clip->state = CLIP_UNTRIED;
		clip->numpieces = 0;
		clip->numverts = 0;
		clip->retryframe = 0;
		clip->scale = 1.0f;
	}
}

void wallhitClipForgetAll(void)
{
	s32 i;

	for (i = 0; i < g_WallhitClipsMax; i++) {
		g_WallhitClips[i].state = CLIP_UNTRIED;
		g_WallhitClips[i].retryframe = 0;
	}
}

void wallhitClipSetScale(struct wallhit *wallhit, f32 scale)
{
	struct wallhitclip *clip = wallhitClipFor(wallhit);

	if (clip) {
		clip->scale = scale;
	}
}

static bool wallhitClipFrame(struct wallhit *wallhit, struct clipframe *f)
{
	Vtx *v = wallhit->vertices;
	f32 uu;
	f32 uv;
	f32 vv;
	f32 det;
	f32 len;
	s32 i;

	// The corners are mid + u + v, mid + u - v, mid - u - v and mid - u + v
	// (wallhitCreateWith20Args()'s sp17c)
	for (i = 0; i < 3; i++) {
		f->mid[i] = (v[0].v[i] + v[1].v[i] + v[2].v[i] + v[3].v[i]) * 0.25f;
		f->u[i] = ((v[0].v[i] + v[1].v[i]) - (v[2].v[i] + v[3].v[i])) * 0.25f;
		f->v[i] = ((v[0].v[i] + v[3].v[i]) - (v[1].v[i] + v[2].v[i])) * 0.25f;
	}

	f->n[0] = f->u[1] * f->v[2] - f->u[2] * f->v[1];
	f->n[1] = f->u[2] * f->v[0] - f->u[0] * f->v[2];
	f->n[2] = f->u[0] * f->v[1] - f->u[1] * f->v[0];

	len = sqrtf(f->n[0] * f->n[0] + f->n[1] * f->n[1] + f->n[2] * f->n[2]);

	if (len < 0.01f) {
		return false;
	}

	f->n[0] /= len;
	f->n[1] /= len;
	f->n[2] /= len;

	uu = f->u[0] * f->u[0] + f->u[1] * f->u[1] + f->u[2] * f->u[2];
	uv = f->u[0] * f->v[0] + f->u[1] * f->v[1] + f->u[2] * f->v[2];
	vv = f->v[0] * f->v[0] + f->v[1] * f->v[1] + f->v[2] * f->v[2];
	det = uu * vv - uv * uv;

	if (det < 0.0001f) {
		return false;
	}

	f->inv[0][0] = vv / det;
	f->inv[0][1] = -uv / det;
	f->inv[1][0] = -uv / det;
	f->inv[1][1] = uu / det;

	return true;
}

/**
 * Keeps the part of the polygon where nx * a + ny * b >= d.
 */
static void wallhitClipHalf(const struct clippoly *in, struct clippoly *out, f32 nx, f32 ny, f32 d)
{
	s32 i;

	out->n = 0;

	for (i = 0; i < in->n; i++) {
		const f32 *p = in->p[i];
		const f32 *q = in->p[(i + 1) % in->n];
		f32 fp = nx * p[0] + ny * p[1] - d;
		f32 fq = nx * q[0] + ny * q[1] - d;

		if (fp >= 0.0f && out->n < CLIP_POLYMAX) {
			out->p[out->n][0] = p[0];
			out->p[out->n][1] = p[1];
			out->n++;
		}

		if (((fp >= 0.0f) != (fq >= 0.0f)) && out->n < CLIP_POLYMAX) {
			f32 t = fp / (fp - fq);

			out->p[out->n][0] = p[0] + (q[0] - p[0]) * t;
			out->p[out->n][1] = p[1] + (q[1] - p[1]) * t;
			out->n++;
		}
	}
}

static f32 wallhitClipArea(const struct clippoly *poly)
{
	f32 sum = 0.0f;
	s32 i;

	for (i = 0; i < poly->n; i++) {
		const f32 *p = poly->p[i];
		const f32 *q = poly->p[(i + 1) % poly->n];

		sum += p[0] * q[1] - q[0] * p[1];
	}

	return sum * 0.5f;
}

/**
 * The square [-k, k] on both axes, counter-clockwise.
 */
static void wallhitClipSquare(struct clippoly *poly, f32 k)
{
	poly->n = 4;
	poly->p[0][0] = -k; poly->p[0][1] = -k;
	poly->p[1][0] = k;  poly->p[1][1] = -k;
	poly->p[2][0] = k;  poly->p[2][1] = k;
	poly->p[3][0] = -k; poly->p[3][1] = k;
}

static void wallhitClipToSquare(struct clippoly *poly, f32 k)
{
	struct clippoly tmp;

	wallhitClipHalf(poly, &tmp, 1.0f, 0.0f, -k);
	wallhitClipHalf(&tmp, poly, -1.0f, 0.0f, -k);
	wallhitClipHalf(poly, &tmp, 0.0f, 1.0f, -k);
	wallhitClipHalf(&tmp, poly, 0.0f, -1.0f, -k);
}

/**
 * The half-plane inside the triangle's edge e (left of it, the triangle
 * being counter-clockwise), or the one outside it.
 */
static void wallhitClipEdge(const struct clippoly *in, struct clippoly *out, const struct cliptri *tri, s32 e, bool inside)
{
	const f32 *p = tri->p[e];
	const f32 *q = tri->p[(e + 1) % 3];
	f32 nx = -(q[1] - p[1]);
	f32 ny = q[0] - p[0];
	f32 d = nx * p[0] + ny * p[1];

	if (inside) {
		wallhitClipHalf(in, out, nx, ny, d);
	} else {
		wallhitClipHalf(in, out, -nx, -ny, -d);
	}
}

static void wallhitClipToTri(struct clippoly *poly, const struct cliptri *tri)
{
	struct clippoly tmp;

	wallhitClipEdge(poly, &tmp, tri, 0, true);
	wallhitClipEdge(&tmp, poly, tri, 1, true);
	wallhitClipEdge(poly, &tmp, tri, 2, true);
	*poly = tmp;
}

static void wallhitClipBounds(const struct clippoly *poly, f32 *min, f32 *max)
{
	s32 i;

	min[0] = max[0] = poly->p[0][0];
	min[1] = max[1] = poly->p[0][1];

	for (i = 1; i < poly->n; i++) {
		if (poly->p[i][0] < min[0]) min[0] = poly->p[i][0];
		if (poly->p[i][0] > max[0]) max[0] = poly->p[i][0];
		if (poly->p[i][1] < min[1]) min[1] = poly->p[i][1];
		if (poly->p[i][1] > max[1]) max[1] = poly->p[i][1];
	}
}

/**
 * Takes a triangle's corners, relative to the quad's middle, and keeps the
 * triangle if it is part of the quad's surface.
 */
static void wallhitClipConsider(const struct clipframe *f, f32 d[3][3], f32 tol)
{
	struct cliptri tri;
	struct clippoly poly;
	f32 h[3];
	f32 tn[3];
	f32 e1[3];
	f32 e2[3];
	f32 len;
	f32 den;
	s32 i;

	for (i = 0; i < 3; i++) {
		f32 du = d[i][0] * f->u[0] + d[i][1] * f->u[1] + d[i][2] * f->u[2];
		f32 dv = d[i][0] * f->v[0] + d[i][1] * f->v[1] + d[i][2] * f->v[2];

		tri.p[i][0] = f->inv[0][0] * du + f->inv[0][1] * dv;
		tri.p[i][1] = f->inv[1][0] * du + f->inv[1][1] * dv;
		h[i] = d[i][0] * f->n[0] + d[i][1] * f->n[1] + d[i][2] * f->n[2];
	}

	tri.min[0] = fminf(tri.p[0][0], fminf(tri.p[1][0], tri.p[2][0]));
	tri.max[0] = fmaxf(tri.p[0][0], fmaxf(tri.p[1][0], tri.p[2][0]));
	tri.min[1] = fminf(tri.p[0][1], fminf(tri.p[1][1], tri.p[2][1]));
	tri.max[1] = fmaxf(tri.p[0][1], fmaxf(tri.p[1][1], tri.p[2][1]));

	if (tri.max[0] <= -1.0f || tri.min[0] >= 1.0f || tri.max[1] <= -1.0f || tri.min[1] >= 1.0f) {
		return;
	}

	if ((h[0] > tol && h[1] > tol && h[2] > tol) || (h[0] < -tol && h[1] < -tol && h[2] < -tol)) {
		return;
	}

	for (i = 0; i < 3; i++) {
		e1[i] = d[1][i] - d[0][i];
		e2[i] = d[2][i] - d[0][i];
	}

	tn[0] = e1[1] * e2[2] - e1[2] * e2[1];
	tn[1] = e1[2] * e2[0] - e1[0] * e2[2];
	tn[2] = e1[0] * e2[1] - e1[1] * e2[0];
	len = sqrtf(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);

	if (len < 0.0001f || fabsf(tn[0] * f->n[0] + tn[1] * f->n[1] + tn[2] * f->n[2]) < CLIP_FACING * len) {
		return;
	}

	den = (tri.p[1][0] - tri.p[0][0]) * (tri.p[2][1] - tri.p[0][1]) - (tri.p[2][0] - tri.p[0][0]) * (tri.p[1][1] - tri.p[0][1]);

	if (den < 0.0f) {
		f32 tmp;

		for (i = 0; i < 2; i++) {
			tmp = tri.p[1][i];
			tri.p[1][i] = tri.p[2][i];
			tri.p[2][i] = tmp;
		}

		tmp = h[1];
		h[1] = h[2];
		h[2] = tmp;
		den = -den;
	}

	if (den < CLIP_MINAREA) {
		return;
	}

	// All of it that lies under the quad must be near the quad's plane
	wallhitClipSquare(&poly, 1.0f);
	wallhitClipToTri(&poly, &tri);

	if (poly.n < 3 || wallhitClipArea(&poly) < CLIP_MINAREA) {
		return;
	}

	for (i = 0; i < poly.n; i++) {
		f32 x = poly.p[i][0] - tri.p[0][0];
		f32 y = poly.p[i][1] - tri.p[0][1];
		f32 l1 = (x * (tri.p[2][1] - tri.p[0][1]) - y * (tri.p[2][0] - tri.p[0][0])) / den;
		f32 l2 = ((tri.p[1][0] - tri.p[0][0]) * y - (tri.p[1][1] - tri.p[0][1]) * x) / den;
		f32 hh = h[0] + l1 * (h[1] - h[0]) + l2 * (h[2] - h[0]);

		if (hh > tol || hh < -tol) {
			return;
		}
	}

	if (g_ClipNumTris >= CLIP_MAXTRIS) {
		g_ClipOverflow = true;
		return;
	}

	g_ClipTris[g_ClipNumTris++] = tri;
}

/**
 * Every triangle of one vertex batch, as bgTestHitInVtxBatch() reads them.
 */
static void wallhitClipGatherBatch(const struct clipframe *f, s32 roomnum, struct vtxbatch *batch, const f32 *offset, f32 tol)
{
	Gfx *gdl = batch->gdl;
	Gfx *iter;
	Vtx *vtx;
	f32 pos[16][3];
	s32 numvertices;
	s32 i;

	vtx = bgFindVerticesForGdl(roomnum, gdl);

	if (vtx == NULL) {
		return;
	}

	iter = &gdl[batch->gbicmdindex];
	vtx = (Vtx *)((UNSEGADDR(iter->words.w1) & 0xffffff) + (uintptr_t)vtx);
	numvertices = (((u32) iter->bytes[GFX_W0_BYTE(1)] >> 4) & 0xf) + 1;

	for (i = 0; i < numvertices; i++) {
		pos[i][0] = offset[0] + vtx[i].x;
		pos[i][1] = offset[1] + vtx[i].y;
		pos[i][2] = offset[2] + vtx[i].z;
	}

	iter++;

	while (iter->dma.cmd != G_VTX && iter->dma.cmd != G_ENDDL) {
		s32 points[4][3];
		s32 numtris = 0;
		s32 t;

		if (iter->dma.cmd == G_TRI1) {
			points[0][0] = iter->tri.tri.v[GFX_TRI_VTX(0)] / 10;
			points[0][1] = iter->tri.tri.v[GFX_TRI_VTX(1)] / 10;
			points[0][2] = iter->tri.tri.v[GFX_TRI_VTX(2)] / 10;
			numtris = 1;
		} else if (iter->dma.cmd == G_TRI4) {
			points[0][0] = iter->tri4.x1; points[0][1] = iter->tri4.y1; points[0][2] = iter->tri4.z1;
			points[1][0] = iter->tri4.x2; points[1][1] = iter->tri4.y2; points[1][2] = iter->tri4.z2;
			points[2][0] = iter->tri4.x3; points[2][1] = iter->tri4.y3; points[2][2] = iter->tri4.z3;
			points[3][0] = iter->tri4.x4; points[3][1] = iter->tri4.y4; points[3][2] = iter->tri4.z4;
			numtris = 4;
		}

		for (t = 0; t < numtris; t++) {
			f32 d[3][3];
			s32 j;

			if (points[t][0] == 0 && points[t][1] == 0 && points[t][2] == 0) {
				continue;
			}

			if (points[t][0] >= numvertices || points[t][1] >= numvertices || points[t][2] >= numvertices) {
				continue;
			}

			for (j = 0; j < 3; j++) {
				d[j][0] = pos[points[t][j]][0];
				d[j][1] = pos[points[t][j]][1];
				d[j][2] = pos[points[t][j]][2];
			}

			wallhitClipConsider(f, d, tol);
		}

		iter++;
	}
}

/**
 * The part of the square the kept triangles cover, as disjoint convex pieces:
 * each triangle's share of the square less every triangle before it.
 */
static s32 wallhitClipUnion(struct wallhitclip *clip)
{
	static struct clippoly pieces[CLIP_MAXPIECES];
	static struct clippoly work[CLIP_MAXWORK];
	static struct clippoly next[CLIP_MAXWORK];
	s32 numpieces = 0;
	f32 area = 0.0f;
	s32 numverts;
	s32 k;
	s32 i;

	for (k = 0; k < g_ClipNumTris; k++) {
		s32 numwork = 1;
		s32 j;

		wallhitClipSquare(&work[0], 1.0f);
		wallhitClipToTri(&work[0], &g_ClipTris[k]);

		if (work[0].n < 3 || wallhitClipArea(&work[0]) < CLIP_MINAREA) {
			continue;
		}

		for (j = 0; j < k && numwork > 0; j++) {
			const struct cliptri *tri = &g_ClipTris[j];
			s32 numnext = 0;
			s32 w;

			for (w = 0; w < numwork; w++) {
				struct clippoly cur = work[w];
				f32 min[2];
				f32 max[2];
				s32 e;

				wallhitClipBounds(&cur, min, max);

				if (max[0] <= tri->min[0] || min[0] >= tri->max[0] || max[1] <= tri->min[1] || min[1] >= tri->max[1]) {
					if (numnext >= CLIP_MAXWORK) {
						return CLIP_WHOLE;
					}

					next[numnext++] = cur;
					continue;
				}

				// What lies outside each edge in turn is kept, and the rest
				// goes on to the next edge; what is inside all three goes
				for (e = 0; e < 3; e++) {
					struct clippoly out;
					struct clippoly in;

					wallhitClipEdge(&cur, &out, tri, e, false);

					if (out.n >= 3 && wallhitClipArea(&out) >= CLIP_MINAREA) {
						if (numnext >= CLIP_MAXWORK) {
							return CLIP_WHOLE;
						}

						next[numnext++] = out;
					}

					wallhitClipEdge(&cur, &in, tri, e, true);

					if (in.n < 3 || wallhitClipArea(&in) < CLIP_MINAREA) {
						break;
					}

					cur = in;
				}
			}

			memcpy(work, next, numnext * sizeof(work[0]));
			numwork = numnext;
		}

		for (i = 0; i < numwork; i++) {
			if (numpieces >= CLIP_MAXPIECES) {
				return CLIP_WHOLE;
			}

			area += wallhitClipArea(&work[i]);
			pieces[numpieces++] = work[i];
		}
	}

	// Nothing hangs over, or nothing was found under it at all (which leaves
	// the mark as it always was rather than making it vanish)
	if (numpieces == 0 || area >= 4.0f * 0.999f) {
		return CLIP_WHOLE;
	}

	numverts = 0;

	for (i = 0; i < numpieces; i++) {
		numverts += pieces[i].n;
	}

	if (numverts > CLIP_MAXVERTS) {
		return CLIP_WHOLE;
	}

	numverts = 0;

	for (i = 0; i < numpieces; i++) {
		s32 j;

		clip->piecesizes[i] = pieces[i].n;

		for (j = 0; j < pieces[i].n; j++) {
			clip->ab[numverts][0] = pieces[i].p[j][0];
			clip->ab[numverts][1] = pieces[i].p[j][1];
			numverts++;
		}
	}

	clip->numpieces = numpieces;
	clip->numverts = numverts;

	return CLIP_CLIPPED;
}

/**
 * Returns the mark's new state, or -1 to try again later.
 */
static s32 wallhitClipCompute(struct wallhit *wallhit, struct wallhitclip *clip)
{
	struct clipframe f;
	struct coord *roompos;
	f32 min[3];
	f32 max[3];
	f32 tol;
	s32 roomnum = wallhit->roomnum;
	s32 r;
	s32 i;

	if (roomnum <= 0 || roomnum >= g_Vars.roomcount || !wallhitClipFrame(wallhit, &f)) {
		return CLIP_WHOLE;
	}

	roompos = &g_BgRooms[roomnum].pos;

	// Room geometry is flat to a unit or so and the quad's corners are
	// rounded to whole units; a big scorch spans more of any unevenness
	tol = 1.5f + 0.03f * sqrtf(f.u[0] * f.u[0] + f.u[1] * f.u[1] + f.u[2] * f.u[2]
			+ f.v[0] * f.v[0] + f.v[1] * f.v[1] + f.v[2] * f.v[2]);

	for (i = 0; i < 3; i++) {
		f32 ext = fabsf(f.u[i]) + fabsf(f.v[i]) + tol;

		min[i] = roompos->f[i] + f.mid[i] - ext;
		max[i] = roompos->f[i] + f.mid[i] + ext;
	}

	for (r = 1; r < g_Vars.roomcount; r++) {
		if (g_Rooms[r].bbmin[0] <= max[0] && g_Rooms[r].bbmax[0] >= min[0]
				&& g_Rooms[r].bbmin[1] <= max[1] && g_Rooms[r].bbmax[1] >= min[1]
				&& g_Rooms[r].bbmin[2] <= max[2] && g_Rooms[r].bbmax[2] >= min[2]
				&& g_Rooms[r].loaded240 == 0) {
			return -1;
		}
	}

	g_ClipNumTris = 0;
	g_ClipOverflow = false;

	for (r = 1; r < g_Vars.roomcount && !g_ClipOverflow; r++) {
		struct vtxbatch *batch;
		f32 offset[3];
		s32 b;

		if (!(g_Rooms[r].bbmin[0] <= max[0] && g_Rooms[r].bbmax[0] >= min[0]
				&& g_Rooms[r].bbmin[1] <= max[1] && g_Rooms[r].bbmax[1] >= min[1]
				&& g_Rooms[r].bbmin[2] <= max[2] && g_Rooms[r].bbmax[2] >= min[2])) {
			continue;
		}

		if (g_Rooms[r].gfxdata == NULL || g_Rooms[r].vtxbatches == NULL) {
			continue;
		}

		// The room's vertices, in the mark's room's space less the quad's middle
		for (i = 0; i < 3; i++) {
			offset[i] = g_BgRooms[r].pos.f[i] - roompos->f[i] - f.mid[i];
		}

		batch = g_Rooms[r].vtxbatches;

		for (b = 0; b < g_Rooms[r].numvtxbatches && !g_ClipOverflow; b++, batch++) {
			if (batch->bbmin.x <= max[0] && batch->bbmax.x >= min[0]
					&& batch->bbmin.y <= max[1] && batch->bbmax.y >= min[1]
					&& batch->bbmin.z <= max[2] && batch->bbmax.z >= min[2]) {
				wallhitClipGatherBatch(&f, r, batch, offset, tol);
			}
		}
	}

	if (g_ClipOverflow) {
		return CLIP_WHOLE;
	}

	return wallhitClipUnion(clip);
}

void wallhitClipTick(struct wallhit *wallhit)
{
	struct wallhitclip *clip;
	s32 state;

	if (!modIsDecalClipOn()) {
		return;
	}

	clip = wallhitClipFor(wallhit);

	if (clip == NULL || clip->state != CLIP_UNTRIED || !wallhit->inuse) {
		return;
	}

	if (wallhit->objprop != NULL) {
		clip->state = CLIP_WHOLE;
		return;
	}

	if (g_Vars.lvframenum < clip->retryframe) {
		return;
	}

	if (g_WallhitClipBudgetFrame != g_Vars.lvframenum) {
		g_WallhitClipBudgetFrame = g_Vars.lvframenum;
		g_WallhitClipBudgetUsed = 0;
	}

	if (g_WallhitClipBudgetUsed >= CLIP_PERTICK) {
		return;
	}

	g_WallhitClipBudgetUsed++;

	state = wallhitClipCompute(wallhit, clip);

	if (state < 0) {
		clip->retryframe = g_Vars.lvframenum + CLIP_RETRYTICKS;
	} else {
		clip->state = state;
	}
}

static s16 wallhitClipRound(f32 value)
{
	return value >= 0.0f ? (s16)(value + 0.5f) : (s16)(value - 0.5f);
}

bool wallhitClipRender(Gfx **gdlptr, struct wallhit *wallhit)
{
	struct wallhitclip *clip = wallhitClipFor(wallhit);
	struct clipframe f;
	Gfx *gdl;
	Vtx *vtx;
	f32 k;
	s32 first = 0;
	s32 p;

	if (clip == NULL || clip->state != CLIP_CLIPPED || !modIsDecalClipOn() || !wallhitClipFrame(wallhit, &f)) {
		return false;
	}

	// An expanding splat's corners are its own pulled in towards its middle
	k = wallhit->vertices2 != NULL ? clip->scale : 1.0f;

	if (k > 1.0f) {
		k = 1.0f;
	} else if (k < 0.01f) {
		k = 0.01f;
	}

	vtx = gfxAllocateVertices(clip->numverts + clip->numpieces * 4);
	gdl = *gdlptr;

	for (p = 0; p < clip->numpieces; p++) {
		struct clippoly poly;
		s32 i;

		poly.n = clip->piecesizes[p];

		for (i = 0; i < poly.n; i++) {
			poly.p[i][0] = clip->ab[first + i][0];
			poly.p[i][1] = clip->ab[first + i][1];
		}

		first += poly.n;

		if (k < 1.0f) {
			wallhitClipToSquare(&poly, k);
		}

		if (poly.n < 3) {
			continue;
		}

		for (i = 0; i < poly.n; i++) {
			f32 a = poly.p[i][0];
			f32 b = poly.p[i][1];
			f32 sa = a / k;
			f32 sb = b / k;
			f32 w[4];
			f32 s = 0.0f;
			f32 t = 0.0f;
			s32 corner;
			s32 c;

			// Corners 0 to 3 are at (1, 1), (1, -1), (-1, -1) and (-1, 1)
			w[0] = (1.0f + sa) * (1.0f + sb) * 0.25f;
			w[1] = (1.0f + sa) * (1.0f - sb) * 0.25f;
			w[2] = (1.0f - sa) * (1.0f - sb) * 0.25f;
			w[3] = (1.0f - sa) * (1.0f + sb) * 0.25f;

			for (c = 0; c < 4; c++) {
				s += w[c] * wallhit->vertices[c].s;
				t += w[c] * wallhit->vertices[c].t;
			}

			corner = sa >= 0.0f ? (sb >= 0.0f ? 0 : 1) : (sb >= 0.0f ? 3 : 2);

			vtx[i].x = wallhitClipRound(f.mid[0] + a * f.u[0] + b * f.v[0]);
			vtx[i].y = wallhitClipRound(f.mid[1] + a * f.u[1] + b * f.v[1]);
			vtx[i].z = wallhitClipRound(f.mid[2] + a * f.u[2] + b * f.v[2]);
			vtx[i].flags = wallhit->vertices[corner].flags;
			vtx[i].colour = wallhit->vertices[corner].colour;
			vtx[i].s = wallhitClipRound(s);
			vtx[i].t = wallhitClipRound(t);
		}

		gSPVertex(gdl++, vtx, poly.n, 0);

		// The pieces are counter-clockwise in (a, b) and the stock quad's
		// triangles clockwise, so the fan goes the other way round
		for (i = 0; i < poly.n - 2; i += 4) {
			s32 tri[4][3];
			s32 j;

			memset(tri, 0, sizeof(tri));

			for (j = 0; j < 4 && i + j < poly.n - 2; j++) {
				tri[j][1] = i + j + 2;
				tri[j][2] = i + j + 1;
			}

			gSPTri4(gdl++,
					tri[0][0], tri[0][1], tri[0][2],
					tri[1][0], tri[1][1], tri[1][2],
					tri[2][0], tri[2][1], tri[2][2],
					tri[3][0], tri[3][1], tri[3][2]);
		}

		vtx += poly.n;
	}

	*gdlptr = gdl;

	return true;
}

void wallhitClipCounts(s32 *clipped, s32 *whole, s32 *waiting)
{
	s32 i;

	*clipped = *whole = *waiting = 0;

	for (i = 0; i < g_WallhitClipsMax; i++) {
		if (!g_Wallhits[i].inuse) {
			continue;
		}

		switch (g_WallhitClips[i].state) {
		case CLIP_CLIPPED: (*clipped)++; break;
		case CLIP_WHOLE: (*whole)++; break;
		default: (*waiting)++; break;
		}
	}
}

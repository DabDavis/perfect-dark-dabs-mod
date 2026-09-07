/**
 * Model Smoothing's mesh pass.
 *
 * The renderer bends each lit triangle into a curved patch using the normals
 * at its corners (gfx_sp_tri_smooth in gfx_pc.cpp). That is right wherever
 * the corner normal is the surface's true direction, and wrong wherever the
 * artist averaged it across a hard edge: the Falcon's slide is a box whose
 * corner normals point diagonally out of every corner, so the patch reads
 * them as a dome and pillows the flat top. A triangle on its own cannot tell
 * a box from a barrel - the rim edge is 90 degrees on both - but the mesh
 * can: what matters is which of the faces around a corner are smoothly
 * joined to *this* face. A box top's neighbours all meet it at right angles,
 * so its true normal is its own face normal and the patch stays flat. A
 * barrel's sides meet each other at 45 degrees and only the cap at 90, so a
 * side face's corner normal is the average of the sides alone, which points
 * radially, and the circumference rounds while the rim stays sharp.
 *
 * So, once per model at load: gather every triangle from the display lists,
 * merge vertices by position (the display lists split a vertex wherever its
 * texture or colour changes), and for each triangle corner average the
 * normals of the faces reachable from this one across edges gentler than the
 * crease angle. That is "auto smooth" as any modelling tool does it, applied
 * per corner rather than per vertex because a vertex on a crease belongs to
 * both surfaces. The result goes to the renderer keyed by the three vertex
 * addresses, which is what a triangle is identified by on the way through a
 * display list. The model's own normals are left alone: they still shade
 * the surface as the artist saw it, this only decides how it bends.
 *
 * The game draws some nodes from a copy of their vertices (a door that fits
 * its frame is stretched into one, a TV screen's quad goes through the
 * frame's buffer with its texture scrolled, a dead chr is disfigured, a
 * destroyed prop is crushed), and a copy's addresses are not in the table.
 * modelRenderNodeDl notes every such copy as it draws it
 * (modelSmoothNoteCopy) and the renderer looks the triangle up again by
 * what it is a copy of.
 *
 * A lit triangle that still cannot be found is drawn flat rather than bent
 * with its own normals. Those are the averaged ones this pass exists to see
 * past, and the case that reaches the renderer is a room surface drawn
 * with lighting on for its reflection - the Institute lobby's glass table
 * top, whose corner normals lean out over its bevel - which is room
 * geometry this pass never reads, and which rose into a dome.
 *
 * Runs from modeldefLoad() and the gun loader, straight after
 * modelPromoteOffsetsToPointers() has given the model file its pointers, so a
 * model is in the table before its first frame. It is called from there
 * rather than from inside the promotion, which is decompiled code that is not
 * told how big the file it is walking is: every address this pass follows is
 * bounded by that buffer, and there is nothing else that says where a model
 * ends.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "system.h"
#include "modelsmooth.h"
#include "../fast3d/gfx_api.h"

// Faces meeting at more than this are a deliberate edge, not a curve. A
// hexagonal arm is 60 degrees between sides; a box is 90.
#define SMOOTH_CREASE_COS 0.2588f // cos 75 degrees

#define SMOOTH_SLOTS   128 // the renderer's vertex cache
#define SMOOTH_GRIDN      4 // pieces per edge the subdivided surface is kept at
#define SMOOTH_GRIDPOINTS ((SMOOTH_GRIDN + 1) * (SMOOTH_GRIDN + 2) / 2)
#define SMOOTH_MAXRING 64  // faces around one vertex we are prepared to walk

// How much has been through here, for gdb
u32 g_ModelSmoothTris = 0;
u32 g_ModelSmoothModels = 0;
u64 g_ModelSmoothUs = 0; // spent in modelSmoothClassify, all models so far

struct smoothtri {
	const Vtx *v[3];
	s32 pos[3];
	f32 normal[3];
	f32 area;
	bool joint;  // drawn under two matrices, so the renderer leaves it flat
	u8 straight; // bit c: edge from corner c to the next is drawn as a line
	bool hasgrid;
	f32 grid[SMOOTH_GRIDPOINTS][3]; // the subdivided surface over this triangle, see meshSubdivide
};

struct smoothpos {
	s16 x, y, z;
	s32 id;
};

struct smoothmesh {
	struct smoothtri *tris;
	s32 numtris;
	s32 maxtris;

	const Vtx *slots[SMOOTH_SLOTS];
	u32 slotmtx[SMOOTH_SLOTS]; // the matrix each slot was loaded under
	u32 curmtx;                // bumped by every matrix command, as the renderer's is
	const Vtx *vtxbase;
	s32 numvertices;
	const u8 *modelbase;
	const u8 *fileend;  // one past the model file's buffer; nothing outside it is read
	s32 numbad; // vertex loads outside the node's array
	s32 numstray; // display list addresses outside the model file

	// merged positions
	struct smoothpos *postab;
	s32 postabsize; // power of two
	s32 numpos;

	// faces around each position, CSR
	s32 *incoff;
	s32 *inclist;
};

static void meshAddTri(struct smoothmesh *m, s32 sa, s32 sb, s32 sc)
{
	const Vtx *a = m->slots[sa];
	const Vtx *b = m->slots[sb];
	const Vtx *c = m->slots[sc];

	if (!a || !b || !c) {
		return;
	}

	if (m->numtris == m->maxtris) {
		m->maxtris = m->maxtris ? m->maxtris * 2 : 256;
		m->tris = realloc(m->tris, sizeof(*m->tris) * m->maxtris);
	}

	struct smoothtri *t = &m->tris[m->numtris++];
	t->v[0] = a;
	t->v[1] = b;
	t->v[2] = c;
	// A triangle across a joint: some of its vertices went through one
	// bone's matrix and the rest through the next. The renderer draws it
	// flat, since a vertex it made up could go through only one.
	t->joint = m->slotmtx[sa] != m->curmtx || m->slotmtx[sb] != m->curmtx || m->slotmtx[sc] != m->curmtx;
	t->straight = 0;
}

/**
 * A segmented address the way the renderer resolves one while this model is
 * being drawn: segment 4 is the node's vertex array, segment 5 the model.
 *
 * Everything this pass reads lives in the model file's own buffer, so an
 * address outside it is not one of the model's and comes back NULL. Nothing
 * else here checks: the offset in a segmented address is 24 bits, a raw one
 * is whatever word the file holds, and a mod's model can carry either. Left
 * unchecked they are a read wherever they point, which on Linux is usually
 * mapped heap and silently walks another model, and on Windows was an
 * access violation in a GE-X mission.
 */
static const void *meshResolve(struct smoothmesh *m, uintptr_t w1)
{
	const u8 *p;

	if (w1 & 1) {
		u32 seg = (w1 & 0x0f000000) >> 24;
		uintptr_t off = w1 & 0x00fffffe;

		if (seg == SPSEGMENT_MODEL_VTX) {
			p = (const u8 *)m->vtxbase + off;
		} else if (seg == SPSEGMENT_MODEL_COL1) {
			p = m->modelbase + off;
		} else {
			return NULL;
		}
	} else {
		p = (const u8 *)w1;
	}

	if (p < m->modelbase || p >= m->fileend) {
		if (p) {
			m->numstray++;
		}

		return NULL;
	}

	return p;
}

/**
 * Run a display list for its triangles only, tracking what each vertex
 * cache slot holds. The decoding is gfx_run_dl's.
 */
static void meshWalkGdl(struct smoothmesh *m, const Gfx *gdl, s32 depth)
{
	if (!gdl || depth > 8) {
		return;
	}

	for (;;) {
		u32 w0;
		uintptr_t w1;
		u32 op;

		// A list that never reaches its G_ENDDL walks off the end of the
		// file otherwise, which is the same read as a stray address.
		if ((const u8 *)(gdl + 1) > m->fileend) {
			m->numstray++;
			return;
		}

		w0 = (u32)gdl->words.w0;
		w1 = gdl->words.w1;
		op = w0 >> 24;

		switch (op) {
		case G_VTX: {
			// How many vertices, from the count the microcode reads
			// (gSPVertex writes n-1 above v0), not from the DMA length
			// beside it: those agree in the game's own lists because one
			// macro writes both, and GE-X has models whose length was
			// worked out with the 16 byte Vtx of stock libultra rather
			// than this game's 12 byte one, which asked for four vertices
			// past the end of the node's array.
			s32 n = ((w0 >> 20) & 0xf) + 1;
			s32 v0 = (w0 >> 16) & 0xf;
			const Vtx *src = meshResolve(m, w1);

			for (s32 i = 0; i < n && v0 + i < SMOOTH_SLOTS; i++) {
				const Vtx *v = src ? src + i : NULL;

				if (v && (v < m->vtxbase || v >= m->vtxbase + m->numvertices)) {
					m->numbad++;
					v = NULL;
				}

				m->slots[v0 + i] = v;
				m->slotmtx[v0 + i] = m->curmtx;
			}
			break;
		}
		case G_MTX:
		case (u8)G_POPMTX:
			m->curmtx++;
			break;
		case (u8)G_TRI1:
			meshAddTri(m, ((w1 >> 16) & 0xff) / 10, ((w1 >> 8) & 0xff) / 10, (w1 & 0xff) / 10);
			break;
		case (u8)G_TRI4:
			for (s32 k = 0; k < 4; k++) {
				u32 x = (w1 >> (8 * k)) & 0xf;
				u32 y = (w1 >> (8 * k + 4)) & 0xf;
				u32 z = (w0 >> (4 * k)) & 0xf;

				if (x || y || z) {
					meshAddTri(m, x, y, z);
				}
			}
			break;
		case G_DL:
			if (((w0 >> 16) & 1) == 0) {
				meshWalkGdl(m, meshResolve(m, w1), depth + 1);
			} else {
				gdl = meshResolve(m, w1);

				if (!gdl) {
					return;
				}

				continue;
			}
			break;
		case (u8)G_ENDDL:
			return;
		}

		gdl++;
	}
}

static s32 meshPosId(struct smoothmesh *m, const Vtx *v)
{
	u32 h = (u32)(u16)v->x * 73856093u ^ (u32)(u16)v->y * 19349663u ^ (u32)(u16)v->z * 83492791u;
	s32 mask = m->postabsize - 1;
	s32 i = h & mask;

	for (;;) {
		struct smoothpos *p = &m->postab[i];

		if (p->id < 0) {
			p->x = v->x;
			p->y = v->y;
			p->z = v->z;
			p->id = m->numpos++;
			return p->id;
		}

		if (p->x == v->x && p->y == v->y && p->z == v->z) {
			return p->id;
		}

		i = (i + 1) & mask;
	}
}

static void meshBuild(struct smoothmesh *m)
{
	s32 size = 64;

	while (size < m->numtris * 6) {
		size *= 2;
	}

	m->postabsize = size;
	m->postab = malloc(sizeof(*m->postab) * size);

	for (s32 i = 0; i < size; i++) {
		m->postab[i].id = -1;
	}

	m->numpos = 0;

	for (s32 i = 0; i < m->numtris; i++) {
		struct smoothtri *t = &m->tris[i];
		f32 p[3][3];

		for (s32 c = 0; c < 3; c++) {
			t->pos[c] = meshPosId(m, t->v[c]);
			p[c][0] = t->v[c]->x;
			p[c][1] = t->v[c]->y;
			p[c][2] = t->v[c]->z;
		}

		f32 e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
		f32 e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
		f32 n[3] = {
			e1[1] * e2[2] - e1[2] * e2[1],
			e1[2] * e2[0] - e1[0] * e2[2],
			e1[0] * e2[1] - e1[1] * e2[0],
		};
		f32 len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

		t->area = len * 0.5f;

		if (len > 1e-6f) {
			t->normal[0] = n[0] / len;
			t->normal[1] = n[1] / len;
			t->normal[2] = n[2] / len;
		} else {
			t->normal[0] = t->normal[1] = t->normal[2] = 0;
		}

		if (t->pos[0] == t->pos[1] || t->pos[1] == t->pos[2] || t->pos[0] == t->pos[2]) {
			t->area = 0; // degenerate: sits in no ring
		}
	}

	// faces around each position
	m->incoff = calloc(m->numpos + 1, sizeof(s32));

	for (s32 i = 0; i < m->numtris; i++) {
		if (m->tris[i].area > 0) {
			for (s32 c = 0; c < 3; c++) {
				m->incoff[m->tris[i].pos[c] + 1]++;
			}
		}
	}

	for (s32 i = 0; i < m->numpos; i++) {
		m->incoff[i + 1] += m->incoff[i];
	}

	m->inclist = malloc(sizeof(s32) * (m->incoff[m->numpos] + 1));

	s32 *fill = calloc(m->numpos + 1, sizeof(s32));

	for (s32 i = 0; i < m->numtris; i++) {
		if (m->tris[i].area > 0) {
			for (s32 c = 0; c < 3; c++) {
				s32 v = m->tris[i].pos[c];
				m->inclist[m->incoff[v] + fill[v]++] = i;
			}
		}
	}

	free(fill);

	// Which edges are drawn as lines however much the surface bends: an
	// edge with nothing on the other side (the sleeve's end where the hand
	// begins, which is another mesh), a crease (a box keeps its corners),
	// and the edges of a joint triangle, which the renderer keeps flat.
	// The patch on either side puts its boundary on the straight line, so
	// the two meet exactly; a curve on one side only would open a gap.
	for (s32 i = 0; i < m->numtris; i++) {
		struct smoothtri *t = &m->tris[i];

		if (t->area <= 0) {
			t->straight = 7;
			continue;
		}

		for (s32 c = 0; c < 3; c++) {
			s32 a = t->pos[c];
			s32 b = t->pos[(c + 1) % 3];
			bool straight = t->joint;
			bool found = false;

			for (s32 k = m->incoff[a]; k < m->incoff[a + 1]; k++) {
				s32 j = m->inclist[k];
				const struct smoothtri *o = &m->tris[j];

				if (j == i || (o->pos[0] != b && o->pos[1] != b && o->pos[2] != b)) {
					continue;
				}

				found = true;

				if (o->joint) {
					straight = true;
				}

				f32 dot = t->normal[0] * o->normal[0] + t->normal[1] * o->normal[1] + t->normal[2] * o->normal[2];

				if (dot < SMOOTH_CREASE_COS) {
					straight = true;
				}
			}

			if (!found || straight) {
				t->straight |= 1 << c;
			}
		}
	}
}

static bool meshShareEdgeAt(const struct smoothtri *a, const struct smoothtri *b, s32 v)
{
	for (s32 i = 0; i < 3; i++) {
		if (a->pos[i] == v) {
			continue;
		}

		for (s32 j = 0; j < 3; j++) {
			if (b->pos[j] == a->pos[i]) {
				return true;
			}
		}
	}

	return false;
}

/**
 * The normal of the surface face `t` belongs to, at its corner `c`: the
 * area-weighted average of the faces around that corner that face `t` can
 * reach across gentle edges.
 */
static void meshLiftNormal(struct smoothmesh *m, s32 tidx, s32 c, f32 out[3])
{
	const struct smoothtri *t = &m->tris[tidx];
	s32 v = t->pos[c];
	s32 start = m->incoff[v];
	s32 count = m->incoff[v + 1] - start;
	u8 seen[SMOOTH_MAXRING];
	s32 queue[SMOOTH_MAXRING];
	s32 qhead = 0, qtail = 0;
	f32 sum[3] = { 0, 0, 0 };

	out[0] = t->normal[0];
	out[1] = t->normal[1];
	out[2] = t->normal[2];

	if (t->area <= 0 || count > SMOOTH_MAXRING) {
		return;
	}

	memset(seen, 0, sizeof(seen));

	for (s32 i = 0; i < count; i++) {
		if (m->inclist[start + i] == tidx) {
			seen[i] = 1;
			queue[qtail++] = i;
		}
	}

	while (qhead < qtail) {
		const struct smoothtri *f = &m->tris[m->inclist[start + queue[qhead++]]];

		sum[0] += f->normal[0] * f->area;
		sum[1] += f->normal[1] * f->area;
		sum[2] += f->normal[2] * f->area;

		for (s32 i = 0; i < count; i++) {
			if (seen[i]) {
				continue;
			}

			const struct smoothtri *g = &m->tris[m->inclist[start + i]];
			f32 dot = f->normal[0] * g->normal[0] + f->normal[1] * g->normal[1] + f->normal[2] * g->normal[2];

			if (dot >= SMOOTH_CREASE_COS && meshShareEdgeAt(f, g, v)) {
				seen[i] = 1;
				queue[qtail++] = i;
			}
		}
	}

	f32 len = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);

	if (len > 1e-6f) {
		out[0] = sum[0] / len;
		out[1] = sum[1] / len;
		out[2] = sum[2] / len;
	}
}

/*
 * The subdivided surface: Modified Butterfly.
 *
 * A curved patch bent from one triangle's corners (the renderer's PN
 * triangles) meets its neighbour along the edge and nowhere else; the two
 * bulge independently, and a body reads as quilted. Butterfly subdivision
 * decides each new point from the triangles around it, so the surface is
 * smooth across the edges as well as along them, and it interpolates: the
 * corners stay where the model put them, which is what keeps a joint
 * triangle or the next bone's mesh meeting this one exactly.
 *
 * It runs here, once per model node, to SMOOTH_GRIDN pieces per edge, and
 * the points over each triangle are handed to the renderer in the grid order
 * it draws them. The mesh is first cut along the edges the renderer draws
 * straight (creases, joints, open edges), so nothing pulls across a crease
 * and every straight edge is a boundary, whose midpoint is its middle.
 */
struct bfmesh {
	f32 (*pos)[3];
	s32 npos;
	s32 (*tri)[3];
	s32 ntri;
	s32 *incoff;  // tris around each vertex, CSR
	s32 *inclist;
};

static void bfBuildRings(struct bfmesh *m)
{
	m->incoff = calloc(m->npos + 1, sizeof(s32));

	for (s32 i = 0; i < m->ntri; i++) {
		for (s32 c = 0; c < 3; c++) {
			m->incoff[m->tri[i][c] + 1]++;
		}
	}

	for (s32 i = 0; i < m->npos; i++) {
		m->incoff[i + 1] += m->incoff[i];
	}

	m->inclist = malloc(sizeof(s32) * (m->incoff[m->npos] + 1));

	s32 *fill = calloc(m->npos + 1, sizeof(s32));

	for (s32 i = 0; i < m->ntri; i++) {
		for (s32 c = 0; c < 3; c++) {
			s32 v = m->tri[i][c];
			m->inclist[m->incoff[v] + fill[v]++] = i;
		}
	}

	free(fill);
}

static bool bfTriHas(const struct bfmesh *m, s32 t, s32 v)
{
	return m->tri[t][0] == v || m->tri[t][1] == v || m->tri[t][2] == v;
}

// the vertex of tri t that is neither a nor b
static s32 bfThird(const struct bfmesh *m, s32 t, s32 a, s32 b)
{
	for (s32 c = 0; c < 3; c++) {
		if (m->tri[t][c] != a && m->tri[t][c] != b) {
			return m->tri[t][c];
		}
	}

	return -1;
}

// the tris sharing edge a-b, up to two; the count found (3 for more)
static s32 bfEdgeTris(const struct bfmesh *m, s32 a, s32 b, s32 out[2])
{
	s32 n = 0;

	for (s32 k = m->incoff[a]; k < m->incoff[a + 1]; k++) {
		s32 t = m->inclist[k];

		if (bfTriHas(m, t, b)) {
			if (n < 2) {
				out[n] = t;
			}

			n++;
		}
	}

	return n > 2 ? 3 : n;
}

// the tri across edge a-b from tri t, or -1
static s32 bfAcross(const struct bfmesh *m, s32 t, s32 a, s32 b)
{
	s32 pair[2];
	s32 n = bfEdgeTris(m, a, b, pair);

	if (n != 2) {
		return -1;
	}

	return pair[0] == t ? pair[1] : pair[0];
}

/**
 * The neighbours of a in order around it, starting from b, walking from
 * triangle to triangle. Returns how many, and whether the walk came back
 * to b (a closed ring: an interior vertex).
 */
static s32 bfRing(const struct bfmesh *m, s32 a, s32 b, s32 *ring, s32 max, bool *closed)
{
	s32 pair[2];
	s32 n = 0;
	s32 t;
	s32 prev = b;

	*closed = false;

	if (bfEdgeTris(m, a, b, pair) < 1) {
		return 0;
	}

	t = pair[0];
	ring[n++] = b;

	while (n < max) {
		s32 next = bfThird(m, t, a, prev);

		if (next < 0) {
			break;
		}

		if (next == b) {
			*closed = true;
			break;
		}

		ring[n++] = next;
		t = bfAcross(m, t, a, next);

		if (t < 0) {
			break;
		}

		prev = next;
	}

	return n;
}

static void bfAdd(f32 out[3], const f32 p[3], f32 w)
{
	out[0] += p[0] * w;
	out[1] += p[1] * w;
	out[2] += p[2] * w;
}

/**
 * One end's opinion of the new point on edge a-b, Zorin's rule for a
 * vertex of any valence: three quarters of a and a weighted ring. Valence
 * six gives the classic butterfly weights. An open ring (a is on a
 * boundary) just wants the middle of the edge.
 */
static void bfEndRule(const struct bfmesh *m, s32 a, s32 b, f32 out[3])
{
	s32 ring[SMOOTH_MAXRING];
	bool closed;
	s32 k = bfRing(m, a, b, ring, SMOOTH_MAXRING, &closed);

	out[0] = out[1] = out[2] = 0;

	if (!closed || k < 3) {
		bfAdd(out, m->pos[a], 0.5f);
		bfAdd(out, m->pos[b], 0.5f);
		return;
	}

	bfAdd(out, m->pos[a], 0.75f);

	if (k == 3) {
		bfAdd(out, m->pos[ring[0]], 5.0f / 12.0f);
		bfAdd(out, m->pos[ring[1]], -1.0f / 12.0f);
		bfAdd(out, m->pos[ring[2]], -1.0f / 12.0f);
	} else if (k == 4) {
		bfAdd(out, m->pos[ring[0]], 3.0f / 8.0f);
		bfAdd(out, m->pos[ring[2]], -1.0f / 8.0f);
	} else {
		for (s32 j = 0; j < k; j++) {
			f32 w = (0.25f + cosf(2.0f * M_PI * j / k) + 0.5f * cosf(4.0f * M_PI * j / k)) / k;
			bfAdd(out, m->pos[ring[j]], w);
		}
	}
}

static bool bfRegular(const struct bfmesh *m, s32 a, s32 b)
{
	s32 ring[SMOOTH_MAXRING];
	bool closed;
	s32 k = bfRing(m, a, b, ring, SMOOTH_MAXRING, &closed);

	return closed && k == 6;
}

/**
 * The new point on edge a-b.
 */
static void bfMidpoint(const struct bfmesh *m, s32 a, s32 b, f32 out[3])
{
	s32 pair[2];
	s32 n = bfEdgeTris(m, a, b, pair);

	out[0] = out[1] = out[2] = 0;

	if (n != 2) {
		// a boundary (every straight edge is one, after the cut), or worse
		bfAdd(out, m->pos[a], 0.5f);
		bfAdd(out, m->pos[b], 0.5f);
		return;
	}

	bool rega = bfRegular(m, a, b);
	bool regb = bfRegular(m, b, a);

	if (rega && regb) {
		// the eight point stencil: the ends, the two across, four wings
		s32 c = bfThird(m, pair[0], a, b);
		s32 d = bfThird(m, pair[1], a, b);
		s32 wing[4];
		s32 t;

		bfAdd(out, m->pos[a], 0.5f);
		bfAdd(out, m->pos[b], 0.5f);
		bfAdd(out, m->pos[c], 0.125f);
		bfAdd(out, m->pos[d], 0.125f);

		t = bfAcross(m, pair[0], a, c); wing[0] = t < 0 ? c : bfThird(m, t, a, c);
		t = bfAcross(m, pair[0], b, c); wing[1] = t < 0 ? c : bfThird(m, t, b, c);
		t = bfAcross(m, pair[1], a, d); wing[2] = t < 0 ? d : bfThird(m, t, a, d);
		t = bfAcross(m, pair[1], b, d); wing[3] = t < 0 ? d : bfThird(m, t, b, d);

		for (s32 i = 0; i < 4; i++) {
			bfAdd(out, m->pos[wing[i]], -0.0625f);
		}
		return;
	}

	if (rega) {
		bfEndRule(m, b, a, out);
	} else if (regb) {
		bfEndRule(m, a, b, out);
	} else {
		f32 pa[3], pb[3];
		bfEndRule(m, a, b, pa);
		bfEndRule(m, b, a, pb);
		for (s32 i = 0; i < 3; i++) {
			out[i] = (pa[i] + pb[i]) * 0.5f;
		}
	}
}

/*
 * Edges to the vertex made on them, open addressing.
 */
struct bfedgemap {
	s64 *key; // (min << 32 | max) + 1, 0 for empty
	s32 *val;
	s32 size;
};

static void bfEdgeMapInit(struct bfedgemap *e, s32 count)
{
	e->size = 64;

	while (e->size < count * 4) {
		e->size *= 2;
	}

	e->key = calloc(e->size, sizeof(s64));
	e->val = malloc(sizeof(s32) * e->size);
}

static s32 *bfEdgeMapSlot(struct bfedgemap *e, s32 a, s32 b)
{
	s32 lo = a < b ? a : b, hi = a < b ? b : a;
	s64 key = (((s64)lo << 32) | (u32)hi) + 1;
	u32 h = (u32)(key * 0x9E3779B97F4A7C15ull >> 32) & (e->size - 1);

	for (;;) {
		if (e->key[h] == 0) {
			e->key[h] = key;
			e->val[h] = -1;
			return &e->val[h];
		}

		if (e->key[h] == key) {
			return &e->val[h];
		}

		h = (h + 1) & (e->size - 1);
	}
}

/**
 * One level: a vertex on every edge, four triangles for each one, in the
 * order the renderer's grid triangulates them.
 */
static void bfSubdivide(const struct bfmesh *in, struct bfmesh *out, struct bfedgemap *edges)
{
	bfEdgeMapInit(edges, in->ntri * 3);

	out->npos = in->npos;
	out->pos = malloc(sizeof(*out->pos) * (in->npos + in->ntri * 3));
	memcpy(out->pos, in->pos, sizeof(*out->pos) * in->npos);
	out->ntri = 0;
	out->tri = malloc(sizeof(*out->tri) * in->ntri * 4);

	for (s32 i = 0; i < in->ntri; i++) {
		s32 mid[3];

		for (s32 c = 0; c < 3; c++) {
			s32 a = in->tri[i][c], b = in->tri[i][(c + 1) % 3];
			s32 *slot = bfEdgeMapSlot(edges, a, b);

			if (*slot < 0) {
				*slot = out->npos;
				bfMidpoint(in, a, b, out->pos[out->npos]);
				out->npos++;
			}

			mid[c] = *slot;
		}

		s32 a = in->tri[i][0], b = in->tri[i][1], c = in->tri[i][2];
		s32 mab = mid[0], mbc = mid[1], mca = mid[2];
		s32 (*t)[3] = &out->tri[out->ntri];
		t[0][0] = a;   t[0][1] = mab; t[0][2] = mca;
		t[1][0] = mab; t[1][1] = b;   t[1][2] = mbc;
		t[2][0] = mab; t[2][1] = mbc; t[2][2] = mca;
		t[3][0] = mca; t[3][1] = mbc; t[3][2] = c;
		out->ntri += 4;
	}

	bfBuildRings(out);
}

static void bfFree(struct bfmesh *m)
{
	free(m->pos);
	free(m->tri);
	free(m->incoff);
	free(m->inclist);
	memset(m, 0, sizeof(*m));
}

static s32 ufFind(s32 *parent, s32 x)
{
	while (parent[x] != x) {
		parent[x] = parent[parent[x]];
		x = parent[x];
	}

	return x;
}

static void ufUnion(s32 *parent, s32 a, s32 b)
{
	a = ufFind(parent, a);
	b = ufFind(parent, b);

	if (a != b) {
		parent[a] = b;
	}
}

// index of grid point (i, j) at n pieces an edge, the renderer's order
static s32 gridIndex(s32 n, s32 i, s32 j)
{
	return j * (n + 1) - j * (j - 1) / 2 + i;
}

/**
 * Subdivide the node's mesh twice and keep, for each triangle, the points
 * of its SMOOTH_GRIDN grid: corner 0 at (0, 0), corner 1 at (n, 0), corner
 * 2 at (0, n). Needs meshBuild() and the straight edges.
 */
static void meshSubdivide(struct smoothmesh *m)
{
	// Corners of triangles that meet at a position are one vertex only when
	// joined through an edge that bends; a straight edge separates the
	// two sides, and a corner on a crease belongs to one side or the other.
	s32 *parent = malloc(sizeof(s32) * m->numtris * 3);

	for (s32 i = 0; i < m->numtris * 3; i++) {
		parent[i] = i;
	}

	for (s32 i = 0; i < m->numtris; i++) {
		struct smoothtri *t = &m->tris[i];

		if (t->area <= 0) {
			continue;
		}

		for (s32 c = 0; c < 3; c++) {
			if (t->straight & (1 << c)) {
				continue;
			}

			s32 a = t->pos[c], b = t->pos[(c + 1) % 3];

			for (s32 k = m->incoff[a]; k < m->incoff[a + 1]; k++) {
				s32 j = m->inclist[k];
				const struct smoothtri *o = &m->tris[j];

				if (j == i || (o->pos[0] != b && o->pos[1] != b && o->pos[2] != b)) {
					continue;
				}

				for (s32 oc = 0; oc < 3; oc++) {
					if (o->pos[oc] == a) {
						ufUnion(parent, i * 3 + c, j * 3 + oc);
					} else if (o->pos[oc] == b) {
						ufUnion(parent, i * 3 + (c + 1) % 3, j * 3 + oc);
					}
				}
			}
		}
	}

	struct bfmesh level0;
	memset(&level0, 0, sizeof(level0));
	level0.pos = malloc(sizeof(*level0.pos) * m->numtris * 3);
	level0.tri = malloc(sizeof(*level0.tri) * m->numtris);

	s32 *vertof = malloc(sizeof(s32) * m->numtris * 3); // corner -> level0 vertex
	s32 *triof = malloc(sizeof(s32) * m->numtris);      // tri -> level0 tri, or -1

	for (s32 i = 0; i < m->numtris * 3; i++) {
		vertof[i] = -1;
	}

	for (s32 i = 0; i < m->numtris; i++) {
		struct smoothtri *t = &m->tris[i];

		triof[i] = -1;
		t->hasgrid = false;

		if (t->area <= 0) {
			continue;
		}

		for (s32 c = 0; c < 3; c++) {
			s32 root = ufFind(parent, i * 3 + c);

			if (vertof[root] < 0) {
				vertof[root] = level0.npos;
				level0.pos[level0.npos][0] = t->v[c]->x;
				level0.pos[level0.npos][1] = t->v[c]->y;
				level0.pos[level0.npos][2] = t->v[c]->z;
				level0.npos++;
			}

			level0.tri[level0.ntri][c] = vertof[root];
		}

		triof[i] = level0.ntri++;
	}

	free(parent);
	free(vertof);

	if (level0.ntri == 0) {
		free(level0.pos);
		free(level0.tri);
		free(triof);
		return;
	}

	bfBuildRings(&level0);

	struct bfmesh level1, level2;
	struct bfedgemap edges1, edges2;
	memset(&level1, 0, sizeof(level1));
	memset(&level2, 0, sizeof(level2));
	bfSubdivide(&level0, &level1, &edges1);
	bfSubdivide(&level1, &level2, &edges2);

	for (s32 i = 0; i < m->numtris; i++) {
		struct smoothtri *t = &m->tris[i];
		s32 g1[3][3]; // level 1 grid: vertex ids at (i, j), i + j <= 2
		s32 a, b, c;

		if (triof[i] < 0) {
			continue;
		}

		a = level0.tri[triof[i]][0];
		b = level0.tri[triof[i]][1];
		c = level0.tri[triof[i]][2];
		g1[0][0] = a;
		g1[2][0] = b;
		g1[0][2] = c;
		g1[1][0] = *bfEdgeMapSlot(&edges1, a, b);
		g1[1][1] = *bfEdgeMapSlot(&edges1, b, c);
		g1[0][1] = *bfEdgeMapSlot(&edges1, a, c);

		for (s32 j = 0; j <= SMOOTH_GRIDN; j++) {
			for (s32 ii = 0; ii <= SMOOTH_GRIDN - j; ii++) {
				s32 v;

				if ((ii & 1) == 0 && (j & 1) == 0) {
					v = g1[ii / 2][j / 2];
				} else if ((j & 1) == 0) {
					v = *bfEdgeMapSlot(&edges2, g1[(ii - 1) / 2][j / 2], g1[(ii + 1) / 2][j / 2]);
				} else if ((ii & 1) == 0) {
					v = *bfEdgeMapSlot(&edges2, g1[ii / 2][(j - 1) / 2], g1[ii / 2][(j + 1) / 2]);
				} else {
					v = *bfEdgeMapSlot(&edges2, g1[(ii + 1) / 2][(j - 1) / 2], g1[(ii - 1) / 2][(j + 1) / 2]);
				}

				if (v < 0 || v >= level2.npos) {
					v = a; // cannot happen: every level 1 edge was subdivided
				}

				memcpy(t->grid[gridIndex(SMOOTH_GRIDN, ii, j)], level2.pos[v], sizeof(f32) * 3);
			}
		}

		t->hasgrid = true;
	}

	free(triof);
	free(edges1.key); free(edges1.val);
	free(edges2.key); free(edges2.val);
	bfFree(&level0);
	bfFree(&level1);
	bfFree(&level2);
}

static s8 meshNormalByte(f32 f)
{
	f32 v = f * 127.0f;

	if (v > 127.0f) {
		v = 127.0f;
	} else if (v < -127.0f) {
		v = -127.0f;
	}

	return (s8)lroundf(v);
}

static void meshClassifyNode(const u8 *modelbase, const u8 *fileend, const Vtx *vertices, s32 numvertices, const Gfx *opagdl, const Gfx *xlugdl)
{
	struct smoothmesh m;

	if (!vertices || numvertices <= 0) {
		return;
	}

	// The node's own array is the one thing here that is not reached through
	// meshResolve(), so it is checked against the file the same way. The
	// subtraction rather than vertices + numvertices because numvertices is
	// whatever the file says and the sum can leave the address space.
	if ((const u8 *)vertices < modelbase || (const u8 *)vertices > fileend
			|| (size_t)(fileend - (const u8 *)vertices) < (size_t)numvertices * sizeof(Vtx)) {
		return;
	}

	memset(&m, 0, sizeof(m));
	m.curmtx = 1;
	m.vtxbase = vertices;
	m.numvertices = numvertices;
	m.modelbase = modelbase;
	m.fileend = fileend;

	// The renderer runs a node's translucent list straight after its opaque
	// one (modelRenderNodeDl, mcount 3) with the vertex cache as the opaque
	// one left it, so the slots carry over between the two walks.
	meshWalkGdl(&m, meshResolve(&m, (uintptr_t)opagdl), 0);
	meshWalkGdl(&m, meshResolve(&m, (uintptr_t)xlugdl), 0);

	if (m.numbad) {
		sysLogPrintf(LOG_WARNING, "modelsmooth: model %p node %p loads %d vertices from outside its array", modelbase, vertices, m.numbad);
	}

	if (m.numstray) {
		sysLogPrintf(LOG_WARNING, "modelsmooth: model %p node %p has %d display list addresses outside the model file", modelbase, vertices, m.numstray);
	}

	if (m.numtris == 0) {
		free(m.tris);
		return;
	}

	meshBuild(&m);
	meshSubdivide(&m);

	for (s32 i = 0; i < m.numtris; i++) {
		struct smoothtri *t = &m.tris[i];
		s8 normals[9];

		for (s32 c = 0; c < 3; c++) {
			f32 n[3];

			meshLiftNormal(&m, i, c, n);
			normals[c * 3 + 0] = meshNormalByte(n[0]);
			normals[c * 3 + 1] = meshNormalByte(n[1]);
			normals[c * 3 + 2] = meshNormalByte(n[2]);
		}

		gfx_smooth_model_add_tri(t->v[0], t->v[1], t->v[2], normals, t->straight, t->hasgrid ? &t->grid[0][0] : NULL);
	}

	g_ModelSmoothTris += m.numtris;

	free(m.inclist);
	free(m.incoff);
	free(m.postab);
	free(m.tris);
}

/**
 * `filelen` is the model file's buffer, which is what bounds every address
 * this pass follows. A caller that does not know it passes 0, and the pass
 * does nothing rather than read on trust.
 */
void modelSmoothClassify(struct modeldef *modeldef, u32 filelen)
{
	struct modelnode *node = modeldef->rootnode;
	const u8 *modelbase = (const u8 *)modeldef;
	const u8 *fileend = modelbase + filelen;
	const u64 start = sysGetMicroseconds();

	if (filelen == 0) {
		return;
	}

	gfx_smooth_model_begin(modeldef);

	while (node) {
		union modelrodata *rodata = node->rodata;

		switch (node->type & 0xff) {
		case MODELNODETYPE_DL:
			meshClassifyNode(modelbase, fileend, rodata->dl.vertices, rodata->dl.numvertices, rodata->dl.opagdl, rodata->dl.xlugdl);
			break;
		case MODELNODETYPE_GUNDL:
			meshClassifyNode(modelbase, fileend, rodata->gundl.vertices, rodata->gundl.numvertices, rodata->gundl.opagdl, rodata->gundl.xlugdl);
			break;
		}

		if (node->child) {
			node = node->child;
		} else {
			while (node) {
				if (node->next) {
					node = node->next;
					break;
				}

				node = node->parent;
			}
		}
	}

	gfx_smooth_model_end();
	g_ModelSmoothModels++;
	g_ModelSmoothUs += sysGetMicroseconds() - start;
}

void modelSmoothNoteCopy(const Vtx *copy, const Vtx *orig, s32 numvertices)
{
	if (copy != orig && gfx_model_smoothing_level) {
		gfx_smooth_alias_vertices(copy, orig, numvertices, sizeof(Vtx));
	}
}

void modelSmoothForgetRange(const void *start, const void *end)
{
	gfx_smooth_alias_forget(start, end);
}

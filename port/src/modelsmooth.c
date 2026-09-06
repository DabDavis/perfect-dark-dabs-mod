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
 * Runs from modelPromoteOffsetsToPointers(), which is where every model file
 * gets its pointers, so a model is in the table before its first frame.
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
#define SMOOTH_MAXRING 64  // faces around one vertex we are prepared to walk

// How much has been through here, for gdb
u32 g_ModelSmoothTris = 0;
u32 g_ModelSmoothModels = 0;

struct smoothtri {
	const Vtx *v[3];
	s32 pos[3];
	f32 normal[3];
	f32 area;
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
	const Vtx *vtxbase;
	s32 numvertices;
	const u8 *modelbase;
	s32 numbad; // vertex loads outside the node's array

	// merged positions
	struct smoothpos *postab;
	s32 postabsize; // power of two
	s32 numpos;

	// faces around each position, CSR
	s32 *incoff;
	s32 *inclist;
};

static void meshAddTri(struct smoothmesh *m, const Vtx *a, const Vtx *b, const Vtx *c)
{
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
}

/**
 * A segmented address the way the renderer resolves one while this model is
 * being drawn: segment 4 is the node's vertex array, segment 5 the model.
 */
static const void *meshResolve(struct smoothmesh *m, uintptr_t w1)
{
	if (w1 & 1) {
		u32 seg = (w1 & 0x0f000000) >> 24;
		uintptr_t off = w1 & 0x00fffffe;

		if (seg == SPSEGMENT_MODEL_VTX) {
			return (const u8 *)m->vtxbase + off;
		}

		if (seg == SPSEGMENT_MODEL_COL1) {
			return m->modelbase + off;
		}

		return NULL;
	}

	return (const void *)w1;
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
		u32 w0 = (u32)gdl->words.w0;
		uintptr_t w1 = gdl->words.w1;
		u32 op = w0 >> 24;

		switch (op) {
		case G_VTX: {
			s32 n = (w0 & 0xffff) / sizeof(Vtx);
			s32 v0 = (w0 >> 16) & 0xf;
			const Vtx *src = meshResolve(m, w1);

			for (s32 i = 0; i < n && v0 + i < SMOOTH_SLOTS; i++) {
				const Vtx *v = src ? src + i : NULL;

				if (v && (v < m->vtxbase || v >= m->vtxbase + m->numvertices)) {
					m->numbad++;
					v = NULL;
				}

				m->slots[v0 + i] = v;
			}
			break;
		}
		case (u8)G_TRI1:
			meshAddTri(m, m->slots[((w1 >> 16) & 0xff) / 10], m->slots[((w1 >> 8) & 0xff) / 10], m->slots[(w1 & 0xff) / 10]);
			break;
		case (u8)G_TRI4:
			for (s32 k = 0; k < 4; k++) {
				u32 x = (w1 >> (8 * k)) & 0xf;
				u32 y = (w1 >> (8 * k + 4)) & 0xf;
				u32 z = (w0 >> (4 * k)) & 0xf;

				if (x || y || z) {
					meshAddTri(m, m->slots[x], m->slots[y], m->slots[z]);
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

static void meshClassifyNode(const u8 *modelbase, const Vtx *vertices, s32 numvertices, const Gfx *opagdl, const Gfx *xlugdl)
{
	struct smoothmesh m;

	if (!vertices || numvertices <= 0) {
		return;
	}

	memset(&m, 0, sizeof(m));
	m.vtxbase = vertices;
	m.numvertices = numvertices;
	m.modelbase = modelbase;

	// The renderer runs a node's translucent list straight after its opaque
	// one (modelRenderNodeDl, mcount 3) with the vertex cache as the opaque
	// one left it, so the slots carry over between the two walks.
	meshWalkGdl(&m, meshResolve(&m, (uintptr_t)opagdl), 0);
	meshWalkGdl(&m, meshResolve(&m, (uintptr_t)xlugdl), 0);

	if (m.numbad) {
		sysLogPrintf(LOG_WARNING, "modelsmooth: model %p node %p loads %d vertices from outside its array", modelbase, vertices, m.numbad);
	}

	if (m.numtris == 0) {
		free(m.tris);
		return;
	}

	meshBuild(&m);

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

		gfx_smooth_model_add_tri(t->v[0], t->v[1], t->v[2], normals);
	}

	g_ModelSmoothTris += m.numtris;

	free(m.inclist);
	free(m.incoff);
	free(m.postab);
	free(m.tris);
}

void modelSmoothClassify(struct modeldef *modeldef)
{
	struct modelnode *node = modeldef->rootnode;
	const u8 *modelbase = (const u8 *)modeldef;

	gfx_smooth_model_begin(modeldef);

	while (node) {
		union modelrodata *rodata = node->rodata;

		switch (node->type & 0xff) {
		case MODELNODETYPE_DL:
			meshClassifyNode(modelbase, rodata->dl.vertices, rodata->dl.numvertices, rodata->dl.opagdl, rodata->dl.xlugdl);
			break;
		case MODELNODETYPE_GUNDL:
			meshClassifyNode(modelbase, rodata->gundl.vertices, rodata->gundl.numvertices, rodata->gundl.opagdl, rodata->gundl.xlugdl);
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

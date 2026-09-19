/**
 * Seating a head on a body it was not made for, by measurement.
 *
 * Perfect Dark moves a head up or down by a table of head and body types
 * (bodyCalculateHeadOffset()), which only knows the game's own pairs. A head
 * or a body from anywhere else - GoldenEye's, a mod's - gets the answer for
 * whichever type its table row happens to carry, and floats or sinks; and the
 * release's meshes never took the offset at all, since they are drawn from
 * their own vertices rather than the shifted N64 ones.
 *
 * Measured over every Combat Simulator body with its own head (GoldenEye X
 * borrowed, 115 bodies), a head's neck begins 40 to 77 units below the top of
 * its own body's neck, in the body's space above the headspot: the neck is
 * tucked into the collar. So a head is put where the body's own head would be
 * - its neck's base on that head's base - and a body that names no head of
 * its own takes the top of its neck less the usual tuck.
 *
 * Only for a head on a body it was not made for (the user's choice): the
 * body's own head, and the ROM's own pairs of one type, are left exactly as
 * the game draws them. Height only: the head keeps its size.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "system.h"
#include "romdata.h"
#include "data.h"
#include "bss.h"
#include "lib/model.h"
#include "game/file.h"
#include "game/modeldef.h"
#include "gebean.h"
#include "gexplus.h"
#include "headfit.h"

#define HEADFIT_MAXVERTS 8192
#define HEADFIT_MAXMTX   64

// The tuck of a head's neck into its body's collar where the body names no head:
// the middle of the measured 40 to 77.
#define HEADFIT_TUCK      -55.0f
#define HEADFIT_MAXOFFSET 200

static void headfitRestOffset(const struct modelnode *node, f32 out[3])
{
	out[0] = out[1] = out[2] = 0.0f;

	for (; node; node = node->parent) {
		const u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_POSITION) {
			out[0] += node->rodata->position.pos.x;
			out[1] += node->rodata->position.pos.y;
			out[2] += node->rodata->position.pos.z;
		} else if (type == MODELNODETYPE_POSITIONHELD) {
			out[0] += node->rodata->positionheld.pos.x;
			out[1] += node->rodata->positionheld.pos.y;
			out[2] += node->rodata->positionheld.pos.z;
		}
	}
}

static const struct modelnode *headfitJoint(const struct modelnode *node)
{
	for (node = node ? node->parent : NULL; node; node = node->parent) {
		if ((node->type & 0xff) == MODELNODETYPE_POSITION) {
			return node;
		}
	}

	return NULL;
}

static s32 headfitUnderToggle(const struct modelnode *node)
{
	for (node = node ? node->parent : NULL; node; node = node->parent) {
		if ((node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			return 1;
		}
	}

	return 0;
}

static int headfitCompareF32(const void *a, const void *b)
{
	const f32 x = *(const f32 *)a;
	const f32 y = *(const f32 *)b;

	return x < y ? -1 : x > y;
}

/** Each matrix's joint, in the model's own space: its position node's offset and every one above it. */
static void headfitMatrixRests(struct modeldef *modeldef, f32 rests[HEADFIT_MAXMTX][3], u8 have[HEADFIT_MAXMTX])
{
	struct modelnode *node = modeldef->rootnode;
	s32 walked = 0;

	memset(have, 0, HEADFIT_MAXMTX);

	while (node && walked++ < 4096) {
		const u32 type = node->type & 0xff;
		s32 mtx = -1;

		if (type == MODELNODETYPE_POSITION) {
			mtx = node->rodata->position.mtxindex0;
		} else if (type == MODELNODETYPE_POSITIONHELD) {
			mtx = node->rodata->positionheld.mtxindex;
		} else if (type == MODELNODETYPE_CHRINFO) {
			mtx = node->rodata->chrinfo.mtxindex;
		}

		if (mtx >= 0 && mtx < HEADFIT_MAXMTX && !have[mtx]) {
			headfitRestOffset(node, rests[mtx]);
			have[mtx] = 1;
		}

		// A head grafted onto this body is not the body's
		if (node->child && type != MODELNODETYPE_HEADSPOT) {
			node = node->child;
			continue;
		}

		while (node) {
			if (node->next) {
				node = node->next;
				break;
			}

			node = node->parent;
		}
	}
}

/**
 * Calls fn for every vertex of a model's untoggled list nodes, in the model's
 * own space. A list's vertices are in the space of the matrix loaded when they
 * were, which is not always the node's: a body's neck list holds its lower
 * ring in the back's space and its upper in the neck's, which put the "top of
 * the neck" 400 units up until it was walked that way. A head file has one
 * space, so a head skips the walk (filebase NULL and single set).
 */
static void headfitEachVertex(struct modeldef *modeldef, const u8 *filebase, s32 walkmatrices,
		void (*fn)(const f32 pos[3], const struct modelnode *node, void *arg), void *arg)
{
	struct modelnode *node = NULL;
	struct modelnode *prev;
	Gfx *gdl;
	f32 rests[HEADFIT_MAXMTX][3];
	u8 have[HEADFIT_MAXMTX];
	s16 *vtxmtx = NULL;
	s32 cap = 0;

	if (walkmatrices) {
		headfitMatrixRests(modeldef, rests, have);
	}

	do {
		prev = node;
		modelIterateDisplayLists(modeldef, &node, &gdl);

		if (node && node != prev && (node->type & 0xff) == MODELNODETYPE_DL && !headfitUnderToggle(node)) {
			const struct modelrodata_dl *dl = &node->rodata->dl;
			f32 rest[3];
			s32 own = -1;

			if (!dl->vertices || dl->numvertices <= 0) {
				continue;
			}

			headfitRestOffset(node, rest);

			if (walkmatrices) {
				if (dl->numvertices > cap) {
					s16 *grown = realloc(vtxmtx, dl->numvertices * sizeof(s16));

					if (!grown) {
						break;
					}

					vtxmtx = grown;
					cap = dl->numvertices;
				}

				own = gebeanListNodeMatrix(node);
				gebeanListVertexMatrices(node, filebase, vtxmtx, dl->numvertices);
			}

			for (s32 i = 0; i < dl->numvertices; i++) {
				const s32 mtx = !walkmatrices ? -1 : vtxmtx[i] >= 0 ? vtxmtx[i] : own;
				const f32 *r = mtx >= 0 && mtx < HEADFIT_MAXMTX && have[mtx] ? rests[mtx] : rest;
				f32 p[3];

				p[0] = dl->vertices[i].x + r[0];
				p[1] = dl->vertices[i].y + r[1];
				p[2] = dl->vertices[i].z + r[2];
				fn(p, node, arg);
			}
		}
	} while (node);

	free(vtxmtx);
}

struct headfitcollect {
	const struct modelnode *joint;
	f32 origin[3];
	f32 *y;
	f32 *xz;
	s32 num;
	f32 sectortop[16];
};

static s32 headfitSector(f32 dx, f32 dz)
{
	// libm's atan2, -pi..pi: the game's own atan2f() answers 0..tau and is
	// the one that links, which folded every direction into the upper half
	s32 k = (s32)floor((atan2((f64)dx, (f64)dz) + M_PI) / (2.0 * M_PI) * 16.0);

	return k < 0 ? 0 : k > 15 ? 15 : k;
}

f32 headfitNeckRadiusToward(const struct headfitbody *body, f32 dx, f32 dz)
{
	const s32 k = headfitSector(dx, dz);

	if (body->sectorradius[k] > 0.0f) {
		return body->sectorradius[k];
	}

	{
		const f32 a = body->sectorradius[(k + 15) % 16];
		const f32 b = body->sectorradius[(k + 1) % 16];

		return a > 0.0f && b > 0.0f ? (a < b ? a : b) : a > b ? a : b > 0.0f ? b : 1e9f;
	}
}

f32 headfitNeckTopToward(const struct headfitbody *body, f32 dx, f32 dz)
{
	const s32 k = headfitSector(dx, dz);

	// An empty sector (a low-poly ring has twelve or so vertices) takes its
	// neighbours', the lower of the two
	if (body->sectortop[k] > -1e8f) {
		return body->sectortop[k];
	}

	{
		const f32 a = body->sectortop[(k + 15) % 16];
		const f32 b = body->sectortop[(k + 1) % 16];

		if (a > -1e8f && b > -1e8f) {
			return a < b ? a : b;
		}

		return a > -1e8f ? a : b > -1e8f ? b : body->necktop;
	}
}

static void headfitCollectHead(const f32 pos[3], const struct modelnode *node, void *arg)
{
	struct headfitcollect *c = arg;

	if (c->num < HEADFIT_MAXVERTS) {
		c->y[c->num++] = pos[1];
	}
}

static void headfitCollectBody(const f32 pos[3], const struct modelnode *node, void *arg)
{
	struct headfitcollect *c = arg;

	if (headfitJoint(node) == c->joint && c->num < HEADFIT_MAXVERTS) {
		const s32 k = headfitSector(pos[0] - c->origin[0], pos[2] - c->origin[2]);
		const f32 y = pos[1] - c->origin[1];

		if (c->xz) {
			c->xz[c->num * 2] = pos[0] - c->origin[0];
			c->xz[c->num * 2 + 1] = pos[2] - c->origin[2];
		}

		c->y[c->num++] = y;

		if (y > c->sectortop[k]) {
			c->sectortop[k] = y;
		}
	}
}

static s32 headfitMeasureHeadAt(struct modeldef *head, struct headfithead *out)
{
	struct headfitcollect c;

	memset(out, 0, sizeof(*out));
	memset(&c, 0, sizeof(c));
	c.y = head ? malloc(HEADFIT_MAXVERTS * sizeof(f32)) : NULL;

	if (!c.y) {
		return 0;
	}

	headfitEachVertex(head, NULL, 0, headfitCollectHead, &c);

	if (c.num > 0) {
		qsort(c.y, c.num, sizeof(f32), headfitCompareF32);
		out->bottom = c.y[0];
		out->base = c.y[c.num / 50];
		out->top = c.y[c.num - 1];
		out->numverts = c.num;
	}

	free(c.y);

	return c.num > 0;
}

static s32 headfitMeasureBodyAt(struct modeldef *body, const u8 *filebase, struct headfitbody *out)
{
	struct headfitcollect c;
	struct modelnode *spot;

	memset(out, 0, sizeof(*out));

	spot = body ? modelGetPart(body, MODELPART_CHR_HEADSPOT) : NULL;

	if (!spot) {
		return 0;
	}

	memset(&c, 0, sizeof(c));
	c.y = malloc(HEADFIT_MAXVERTS * sizeof(f32));
	c.xz = malloc(HEADFIT_MAXVERTS * 2 * sizeof(f32));

	if (!c.y || !c.xz) {
		free(c.y);
		free(c.xz);
		return 0;
	}

	c.joint = headfitJoint(spot);
	headfitRestOffset(spot, c.origin);

	for (s32 k = 0; k < 16; k++) {
		c.sectortop[k] = -1e9f;
	}

	headfitEachVertex(body, filebase, 1, headfitCollectBody, &c);

	// Each direction's radius: the widest of its vertices within 20 of its top
	for (s32 i = 0; i < c.num; i++) {
		const f32 dx = c.xz[i * 2];
		const f32 dz = c.xz[i * 2 + 1];
		const s32 k = headfitSector(dx, dz);
		const f32 r = sqrtf(dx * dx + dz * dz);

		if (c.y[i] >= c.sectortop[k] - 20.0f && r > out->sectorradius[k]) {
			out->sectorradius[k] = r;
		}
	}

	free(c.xz);

	if (c.num > 0) {
		qsort(c.y, c.num, sizeof(f32), headfitCompareF32);
		out->necktop = c.y[c.num - 1];
		out->neckbottom = c.y[0];
		out->numverts = c.num;
	}

	memcpy(out->spot, c.origin, sizeof(out->spot));
	memcpy(out->sectortop, c.sectortop, sizeof(out->sectortop));

	free(c.y);

	return c.num > 0;
}

static s32 headfitCached(s32 filenum, s32 ishead, struct headfithead *head, struct headfitbody *body);

s32 headfitMeasureHead(struct modeldef *head, struct headfithead *out)
{
	return headfitMeasureHeadAt(head, out);
}

s32 headfitMeasureBodyFile(s32 filenum, struct headfitbody *out)
{
	return headfitCached(filenum, 0, NULL, out);
}

/* -------------------------------------------------------------------------
 * Measuring a file nothing has loaded
 * ------------------------------------------------------------------------- */

/**
 * A model file inflated into a buffer of its own with its pointers promoted,
 * and nothing else: no textures, and not shown to the XBLA mesh matcher, which
 * a real load registers and a freed buffer must never be. The file's size
 * record is put back, since loading writes it.
 */
static struct modeldef *headfitLoadFile(s32 filenum, u8 **outbuf)
{
	struct fileinfo saved;
	u32 size;
	u8 *buf;
	struct modeldef *modeldef;

	*outbuf = NULL;

	if (filenum <= 0 || filenum >= NUM_FILE_SLOTS) {
		return NULL;
	}

	size = fileGetInflatedSize(filenum, LOADTYPE_MODEL);

	if (size == 0 || size > 0x400000) {
		return NULL;
	}

	// what fileLoadToNew() allows for it, so the widening fits
	size = ((size + 0x20) & 0xfffffff0) + 0x8000;
	buf = calloc(size, 1);

	if (!buf) {
		return NULL;
	}

	saved = g_FileInfo[filenum];
	// Byteswapped and widened to this build's layout by the loader, which asks this
	g_LoadType = LOADTYPE_MODEL;
	modeldef = fileLoadToAddr(filenum, FILELOADMETHOD_EXTRAMEM, buf, size);
	g_FileInfo[filenum] = saved;

	if (!modeldef) {
		free(buf);
		return NULL;
	}

	modelPromoteTypeToPointer(modeldef);
	modelPromoteOffsetsToPointers(modeldef, 0x5000000, (uintptr_t)modeldef);

	*outbuf = buf;

	return modeldef;
}

#define HEADFIT_CACHE 512

static struct {
	u16 filenum;
	u8 ishead;
	u8 ok;
	struct headfithead head;
	struct headfitbody body;
} cache[HEADFIT_CACHE];
static s32 numCache;

static s32 headfitCached(s32 filenum, s32 ishead, struct headfithead *head, struct headfitbody *body)
{
	u8 *buf;
	struct modeldef *modeldef;
	s32 ok;

	for (s32 i = 0; i < numCache; i++) {
		if (cache[i].filenum == filenum && cache[i].ishead == ishead) {
			if (head) *head = cache[i].head;
			if (body) *body = cache[i].body;
			return cache[i].ok;
		}
	}

	modeldef = headfitLoadFile(filenum, &buf);
	ok = modeldef && (ishead ? headfitMeasureHeadAt(modeldef, head) : headfitMeasureBodyAt(modeldef, buf, body));
	free(buf);

	if (numCache < HEADFIT_CACHE) {
		cache[numCache].filenum = (u16)filenum;
		cache[numCache].ishead = (u8)ishead;
		cache[numCache].ok = (u8)ok;
		if (head) cache[numCache].head = *head;
		if (body) cache[numCache].body = *body;
		numCache++;
	}

	return ok;
}

/* -------------------------------------------------------------------------
 * The fit
 * ------------------------------------------------------------------------- */

/** The head a Combat Simulator list gives a body as its own, or -1. */
static s32 headfitOwnHead(s32 bodynum)
{
	for (s32 i = 0; i < g_MpListCounts.bodies; i++) {
		if (g_MpBodies[i].bodynum == bodynum) {
			const s32 h = g_MpBodies[i].headnum;

			return h >= 0 && h < NUM_HEADSANDBODIES && g_HeadsAndBodies[h].filenum ? h : -1;
		}
	}

	return -1;
}

s32 headfitWanted(s32 headnum, s32 bodynum)
{
	if (headnum <= 0 || bodynum < 0 || headnum >= NUM_HEADSANDBODIES || bodynum >= NUM_HEADSANDBODIES) {
		return 0;
	}

	// The release's pool draws its meshes over a host's models, whose N64
	// geometry says nothing about where the mesh is
	if (gebeanIsPoolRow(headnum) || gebeanIsPoolRow(bodynum)) {
		return 0;
	}

	// A converted mission's guard is GoldenEye's own head on GoldenEye's own
	// body, sitting on the body's headspot where GoldenEye put it: a neck
	// measured between two of them would move one that already fits
	if (gexPlusRomIsPoolRow(headnum) && gexPlusRomIsPoolRow(bodynum)) {
		return 0;
	}

	// The ROM's own pairs of one type: the game's answer
	if (g_HeadsAndBodies[headnum].type == g_HeadsAndBodies[bodynum].type
			&& romdataFileIsStock(g_HeadsAndBodies[headnum].filenum)
			&& romdataFileIsStock(g_HeadsAndBodies[bodynum].filenum)) {
		return 0;
	}

	// The head the body was made for
	return headfitOwnHead(bodynum) != headnum;
}

s32 headfitOffset(struct modeldef *headmodeldef, s32 headnum, s32 bodynum, struct modeldef *bodymodeldef)
{
	struct headfithead head;
	struct headfithead own;
	struct headfitbody body;
	const s32 ownhead = headfitOwnHead(bodynum);
	f32 target;

	(void)bodymodeldef;
	f32 offset;
	const char *how;

	if (!headfitMeasureHeadAt(headmodeldef, &head)) {
		return 0;
	}

	if (ownhead > 0 && headfitCached(g_HeadsAndBodies[ownhead].filenum, 1, &own, NULL)) {
		target = own.base;
		how = "the body's own head";
	} else if (headfitCached(g_HeadsAndBodies[bodynum].filenum, 0, NULL, &body)) {
		// From the file even when the body is loaded: a loaded model's list
		// addresses are rewritten, and the walk that says which matrix a vertex
		// was loaded under then reads the neck's top 130 units too high
		target = body.necktop + HEADFIT_TUCK;
		how = "the body's neck";
	} else {
		return 0;
	}

	offset = target - head.base;

	if (offset > HEADFIT_MAXOFFSET) {
		offset = HEADFIT_MAXOFFSET;
	} else if (offset < -HEADFIT_MAXOFFSET) {
		offset = -HEADFIT_MAXOFFSET;
	}

	sysLogPrintf(LOG_NOTE, "headfit: head %d (%s) on body %d (%s): base %.0f to %.0f by %s, offset %d",
			headnum, romdataFileGetName(g_HeadsAndBodies[headnum].filenum) ? romdataFileGetName(g_HeadsAndBodies[headnum].filenum) : "?",
			bodynum, romdataFileGetName(g_HeadsAndBodies[bodynum].filenum) ? romdataFileGetName(g_HeadsAndBodies[bodynum].filenum) : "?",
			head.base, target, how, (s32)lroundf(offset));

	return (s32)lroundf(offset);
}

/* -------------------------------------------------------------------------
 * The offset a head copy was given, for the release's meshes
 * ------------------------------------------------------------------------- */

#define HEADFIT_APPLIED 256

static struct {
	const struct modeldef *modeldef;
	const struct modelnode *root;
	s32 offset;
	s32 measured;
} applied[HEADFIT_APPLIED];
static s32 nextApplied;

void headfitNoteApplied(const struct modeldef *modeldef, s32 offset, s32 measured)
{
	s32 at = -1;

	for (s32 i = 0; i < HEADFIT_APPLIED; i++) {
		if (applied[i].modeldef == modeldef) {
			at = i;
			break;
		}
	}

	if (at < 0) {
		at = nextApplied;
		nextApplied = (nextApplied + 1) % HEADFIT_APPLIED;
	}

	if (applied[at].modeldef != modeldef) {
		applied[at].measured = 0;
	}

	applied[at].modeldef = modeldef;
	applied[at].root = modeldef ? modeldef->rootnode : NULL;
	applied[at].offset = offset;

	if (measured >= 0) {
		applied[at].measured = measured;
	}
}

s32 headfitWasMeasured(const struct modeldef *modeldef)
{
	for (s32 i = 0; modeldef && i < HEADFIT_APPLIED; i++) {
		if (applied[i].modeldef == modeldef) {
			return applied[i].root == modeldef->rootnode && applied[i].measured;
		}
	}

	return 0;
}

s32 headfitAppliedOffset(const struct modeldef *modeldef)
{
	if (!modeldef) {
		return 0;
	}

	for (s32 i = 0; i < HEADFIT_APPLIED; i++) {
		// A freed modeldef's address can come back as another file, whose root differs
		if (applied[i].modeldef == modeldef) {
			return applied[i].root == modeldef->rootnode ? applied[i].offset : 0;
		}
	}

	return 0;
}

void headfitReset(void)
{
	memset(applied, 0, sizeof(applied));
	nextApplied = 0;
	numCache = 0;
}

/* -------------------------------------------------------------------------
 * Research
 * ------------------------------------------------------------------------- */

struct headfitprofile {
	f32 *p; // x, y, z
	s32 num;
	s32 cap;
};

static void headfitCollectPoints(const f32 pos[3], const struct modelnode *node, void *arg)
{
	struct headfitprofile *c = arg;

	if (c->num >= c->cap) {
		const s32 cap = c->cap ? c->cap * 2 : 1024;
		f32 *grown = realloc(c->p, cap * 3 * sizeof(f32));

		if (!grown) {
			return;
		}

		c->p = grown;
		c->cap = cap;
	}

	memcpy(&c->p[c->num * 3], pos, 3 * sizeof(f32));
	c->num++;
}

/** Research: a head's width and reach by height, from its base up, to the log. */
static void headfitProfileHead(s32 headnum)
{
	struct headfitprofile c;
	struct headfithead h;
	u8 *buf;
	struct modeldef *modeldef = headfitLoadFile(g_HeadsAndBodies[headnum].filenum, &buf);
	char line[1024];
	s32 at = 0;

	memset(&c, 0, sizeof(c));

	if (!modeldef || !headfitMeasureHeadAt(modeldef, &h)) {
		free(buf);
		return;
	}

	headfitEachVertex(modeldef, NULL, 0, headfitCollectPoints, &c);

	{
		f32 zmax = -1e9f;
		char chins[128];
		s32 cat = 0;

		for (s32 i = 0; i < c.num; i++) {
			if (c.p[i * 3 + 2] > zmax && c.p[i * 3 + 1] < h.base + 250.0f) zmax = c.p[i * 3 + 2];
		}

		for (s32 t = 5; t <= 8; t++) {
			f32 chin = 1e9f;

			for (s32 i = 0; i < c.num; i++) {
				if (c.p[i * 3 + 2] >= zmax * t / 10.0f && c.p[i * 3 + 1] < chin) chin = c.p[i * 3 + 1];
			}

			cat += snprintf(chins + cat, sizeof(chins) - cat, " c%d %.0f", t, chin);
		}

		sysLogPrintf(LOG_NOTE, "headfit: chin head %d %s base %.0f top %.0f zmax %.0f%s", headnum,
				romdataFileGetName(g_HeadsAndBodies[headnum].filenum) ? romdataFileGetName(g_HeadsAndBodies[headnum].filenum) : "?",
				h.base, h.top, zmax, chins);
	}

	for (s32 bin = 0; bin < 24 && at < (s32)sizeof(line) - 40; bin++) {
		const f32 lo = h.base + bin * 10.0f;
		f32 xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
		s32 n = 0;

		for (s32 i = 0; i < c.num; i++) {
			const f32 *q = &c.p[i * 3];

			if (q[1] >= lo && q[1] < lo + 10.0f) {
				if (q[0] < xmin) xmin = q[0];
				if (q[0] > xmax) xmax = q[0];
				if (q[2] < zmin) zmin = q[2];
				if (q[2] > zmax) zmax = q[2];
				n++;
			}
		}

		at += snprintf(line + at, sizeof(line) - at, n ? " %.0f:%.0f/%.0f..%.0f" : " %.0f:-", lo, xmax - xmin, zmin, zmax);
	}

	sysLogPrintf(LOG_NOTE, "headfit: profile head %d %s base %.0f top %.0f |%s",
			headnum, romdataFileGetName(g_HeadsAndBodies[headnum].filenum) ? romdataFileGetName(g_HeadsAndBodies[headnum].filenum) : "?",
			h.base, h.top, line);

	free(c.p);
	free(buf);
}

void headfitSurveyHeads(void)
{
	for (s32 i = 0; i < g_MpListCounts.heads; i++) {
		headfitProfileHead(g_MpHeads[i].headnum);
	}
}

/** Every Combat Simulator body with its own head, measured, to the log. */
void headfitSurvey(void)
{
	for (s32 i = 0; i < g_MpListCounts.bodies; i++) {
		const s32 bodynum = g_MpBodies[i].bodynum;
		const s32 headnum = headfitOwnHead(bodynum);
		const char *bname = romdataFileGetName(g_HeadsAndBodies[bodynum].filenum);
		struct headfitbody b;
		struct headfithead h;

		if (!headfitCached(g_HeadsAndBodies[bodynum].filenum, 0, NULL, &b)) {
			sysLogPrintf(LOG_NOTE, "headfit: body %3d %s: no neck", bodynum, bname ? bname : "?");
			continue;
		}

		if (headnum > 0 && !g_HeadsAndBodies[bodynum].unk00_01
				&& headfitCached(g_HeadsAndBodies[headnum].filenum, 1, &h, NULL)) {
			const char *hname = romdataFileGetName(g_HeadsAndBodies[headnum].filenum);

			sysLogPrintf(LOG_NOTE, "headfit: body %3d %-22s type %d neck %6.1f..%6.1f | head %3d %-20s type %d base %6.1f | tuck %6.1f",
					bodynum, bname ? bname : "?", g_HeadsAndBodies[bodynum].type, b.neckbottom, b.necktop,
					headnum, hname ? hname : "?", g_HeadsAndBodies[headnum].type, h.base, h.base - b.necktop);
		} else {
			sysLogPrintf(LOG_NOTE, "headfit: body %3d %-22s type %d neck %6.1f..%6.1f | no own head",
					bodynum, bname ? bname : "?", g_HeadsAndBodies[bodynum].type, b.neckbottom, b.necktop);
		}
	}
}

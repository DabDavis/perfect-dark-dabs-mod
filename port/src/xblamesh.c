/**
 * Drawing the XBLA release's meshes in place of the game's display lists.
 * See xblamesh.h for what this is and what shapes it.
 *
 * Three stages, each of which can fail on its own and leave the game drawing
 * its own geometry:
 *
 *   * the package, opened once and kept - PackedSegFile's record table is 42KB
 *     and stays in memory, the 30MB of data behind it does not;
 *   * a model's nodes, matched against the release's copy of the same file as
 *     it loads, which is the only thing that says which mesh replaces what;
 *   * a mesh, read and turned into a display list the first time something
 *     asks to draw it, and kept for as long as the game runs. Two lists per
 *     group, in fact: the draws whose material carries alpha are a span of
 *     their own, so that a node the game draws a translucent list for can
 *     have one too.
 *
 * Nothing here is on the render thread's critical path except the display list
 * pointer it ends up branching to.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "config.h"
#include "system.h"
#include "input.h"
#include "lib/main.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "x360.h"
#include "xblaimport.h"
#include "xblastage.h"
#include "romdata.h"
#include "files.h"
#include "xblamesh.h"
#include "xblatex.h"

#ifndef PLATFORM_N64

// Inside the package. The models and the game's own files share this one.
#define XBLAMESH_PACKED "DataFiles/PackedSegFile"

// {offset, uncompressedSize, compressedSize, flags}. Slot i is file id i + 1.
#define XBLAMESH_RECORD 16

// The fixed part of a mesh header. Everything past it the header measures.
#define XBLAMESH_HEADER 32

// Every table between the header and the vertices is three words wide, and a
// matrix is three rows of four floats.
#define XBLAMESH_ENTRY 12
#define XBLAMESH_MATRIX 48

// A mesh whose header asks for more than this is not one.
#define XBLAMESH_MAXVERTS  0x40000
#define XBLAMESH_MAXDRAWS  0x1000
#define XBLAMESH_MAXFILE   (16 * 1024 * 1024)

/**
 * Vertices addressable by one batch.
 *
 * gSP1Triangle multiplies its indices by 10 into a byte, which is what caps
 * this at 25 rather than at the renderer's 128 slots. The real microcode holds
 * 16; nothing this builds ever reaches an RSP.
 */
#define XBLAMESH_BATCH 25

// The three spans a group's draws are sorted into; what each one is, is
// explained above xblaMeshDrawSpan().
#define XBLAMESH_SPAN_SOLID 0
#define XBLAMESH_SPAN_ALPHA 1
#define XBLAMESH_SPAN_FADE  2

// The two strides, unskinned and skinned. Everything before the weights is
// laid out the same in both.
#define XBLAMESH_STRIDE_RIGID 36
#define XBLAMESH_STRIDE_SKIN 48

// A power of two; open addressed. Every list node of a matched model takes an
// entry now, not only the ones the release named - a character body is one
// named node and fifteen more the mesh covers - so this is ten times the
// nodes a level used to file, plus the tombstones a stage's reloads leave.
#define XBLAMESH_HASHSIZE 16384

// Plain list nodes of one model collected while its tree is walked, to be
// filed as covered once the walk says the model really did match. The most any
// model in the release has is 22 (the Nintendo logo); a character body has 15.
#define XBLAMESH_COVERED 64

// What a suppressed entry is suppressed for.
#define XBLAMESH_SUPPRESS_HAIR 1    // a head's stock hair, which the mesh paints on
#define XBLAMESH_SUPPRESS_COVERED 2 // geometry the model's own mesh has already

// How many draws --xbla-mesh-verbose names before it stops
#define XBLAMESH_DRAWLOG 12

// How far up a node's parents to look for the model that is drawing it. A head
// hangs off a body, so its nodes are the head's own depth plus the body's.
#define XBLAMESH_PARENTSCAN 32

// Commands into a node's own list to look for its matrix in. The game's own
// lists load one inside the first handful, before any geometry.
#define XBLAMESH_MTXSCAN 32

// A model's vertices go through a segment, so one built list can draw from
// either the bind pose or a posed copy of it.
#define XBLAMESH_VTXSEG (SPSEGMENT_MODEL_VTX << 24)

// Parts a model can have, which is what the id's nibble can count.
#define XBLAMESH_MAXPARTS 16

// Palette entries a mesh can have. The largest in the release has 46.
#define XBLAMESH_MAXMTX 64

u32 g_XblaMeshNumMeshes = 0;
u32 g_XblaMeshNumNodes = 0;
u32 g_XblaMeshNumSlots = 0; // taken, live or tombstoned - the table's headroom
u32 g_XblaMeshNumTris = 0;
u32 g_XblaMeshBytes = 0;

struct xblameshentry {
	const struct modelnode *node;      // key
	const struct modeldef *modeldef;   // which load of which model it belongs to
	u16 slot;
	u16 part;
	s32 use;                           // into uses[], or -1
	s32 suppress;                      // XBLAMESH_SUPPRESS_*: stock geometry that draws nothing
};

/**
 * One model's use of one mesh: which node is which entry of its palette.
 *
 * A mesh replaces a whole model and every part of the model carries its id
 * with that part's number in the top nibble, so this is the map a skinned draw
 * needs - palette entry i is posed by whatever the game has done to the node
 * that carries part i.
 */
struct xblameshuse {
	const struct modeldef *modeldef;
	u16 slot;
	u16 numparts;
	struct modelnode *parts[XBLAMESH_MAXPARTS];
	s16 partmtx[XBLAMESH_MAXPARTS];   // which of the model's matrices poses it
};

struct xblameshbuilt {
	Gfx *gdl;
	Vtx *vertices;
	Col *colours;
	f32 *grad;         // four per emitted vertex: ds/dx dt/dx ds/dy dt/dy; NULL when skinned
	s32 numvertices;   // as emitted, which repeats one shared between batches
	s32 numtris;
	s32 state;         // 0 untried, 1 built, -1 no good
	s32 logged;
	s32 xlulogged;
	s32 fadelogged;
	s32 posedlog;

	// Skinning, for a mesh that has a matrix palette. The vertices above are
	// the bind pose; these are what it takes to put them in a pose of the
	// game's. Positions are kept as they were read rather than as the s16 the
	// Vtx holds, since they are transformed before they are rounded.
	s32 nummatrices;
	s32 numgroups;
	f32 scale;         // mesh units to the game's, out of the header
	s32 groupgfx[XBLAMESH_MAXPARTS]; // into gdl: where each group's opaque list starts
	s32 groupxlu[XBLAMESH_MAXPARTS]; // its alpha materials, or -1 if it has none
	s32 groupfade[XBLAMESH_MAXPARTS]; // its draws that fade by vertex alpha, or -1
	s32 allgfx;                      // and the ones that call every group
	s32 allxlu;
	s32 allfade;

	// The posed copy already made this frame, and who for. Every part of a
	// model draws its own group now, so without this Dr Carroll would pose
	// thirteen copies of himself a frame and fill the arena with twelve of
	// them. One entry is enough: the renderer walks a model's tree in one go,
	// so a model's parts are drawn one after another.
	const struct model *posedmodel;
	u32 posedframe;
	Vtx *posedvtx;
	Mtxf *posedmtx;    // the matrix that copy is drawn under, when it is not the bone's own
	s32 posedfine;     // and how many steps of that copy make one of the game's units

	// The trimmed copy already made this frame, for a door the game is
	// drawing from trimmed vertices - see xblaMeshNodeTrim(). Keyed the way
	// the posed copy is, plus the trim itself, since two doors of one model
	// can be open by different amounts in one frame.
	const struct model *trimmodel;
	u32 trimframe;
	s32 trimaxis;
	s16 trimref;
	Vtx *trimvtx;
	s32 trimlogged;
	Mtxf *invbind;     // one per palette entry
	f32 *bindpos;      // three per emitted vertex
	f32 *weights;      // three per emitted vertex; only the first two are ever set
	u8 *bones;         // three per emitted vertex, and the count in the fourth
	f32 bindlo[3];     // the box the bind positions stand in, which bounds the posed ones
	f32 bindhi[3];
};

static s32 optEnabled;
static s32 optOnlySlot; // Mod.XblaMeshOnly: draw one mesh and leave the rest alone
static s32 optBoth;     // Mod.XblaMeshBoth: draw the game's geometry over it too

/**
 * Mod.XblaMeshPose: drive a skinned mesh's palette from the game's matrices.
 *
 * On, and what makes a character out of a skinned mesh: without it the whole
 * body draws in its bind pose under one bone's matrix, which is a heap of
 * limbs rather than a person. Turning it off is how a shape that is wrong is
 * told apart from a pose that is - the same job Mod.XblaMeshTextures does for
 * the art.
 */
static s32 optPose = 1;
static s32 opened; // 0 untried, 1 open, -1 no package

/**
 * The switch is what opened the package, and it did it in a level.
 *
 * True only in the one case the meshes cannot be live in: a player whose copy
 * was still inside its .7z matched nothing as the level loaded, because a
 * model load never unpacks, so the level standing behind the menu has no mesh
 * in it and turning the switch on does not change a thing that is drawn. It is
 * cleared by xblaMeshResetModels(), which runs at lvReset() before a stage's
 * models load - so it is set for exactly as long as it is true, and the menu
 * can say so rather than guess.
 */
static s32 openedLate;

static struct x360stfs stfs;
static struct x360stfsstream packed;
static u32 *recOffset;
static u32 *recUncSize;
static u32 *recCompSize;
static s32 numRecords;

static struct xblameshbuilt *built;      // one per slot, allocated with the table
static struct xblameshentry hash[XBLAMESH_HASHSIZE];
static struct xblameshuse *uses;
static s32 numUses, capUses;

static u32 xblaMeshBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 xblaMeshBE16(const u8 *p)
{
	return (u16)(((u32)p[0] << 8) | p[1]);
}

static f32 xblaMeshBEF32(const u8 *p)
{
	union { u32 u; f32 f; } bits;
	bits.u = xblaMeshBE32(p);
	return bits.f;
}

/* -------------------------------------------------------------------------
 * The package
 * ------------------------------------------------------------------------- */

static void xblaMeshCloseUp(void)
{
	x360StfsStreamClose(&packed);
	x360StfsClose(&stfs);
	free(recOffset);
	free(recUncSize);
	free(recCompSize);
	free(built);
	recOffset = NULL;
	recUncSize = NULL;
	recCompSize = NULL;
	built = NULL;
	numRecords = 0;
	opened = -1;
}

/**
 * Opens the package and reads PackedSegFile's record table.
 *
 * The table is {count, count x 16 bytes}: an offset, an inflated size, a
 * compressed size (zero when the file is stored as it is) and flags. Unused
 * slots hold leftover bytes rather than zeros and give themselves away by
 * having no inflated size.
 */
static s32 xblaMeshOpen(s32 mayUnpack)
{
	const char *path;
	s32 index;
	u8 head[4];
	u8 *table;
	u32 tableLen;

	if (opened) {
		return opened > 0;
	}

	// Every model load asks for this, so it must not be the thing that unpacks
	// a 250MB archive on somebody who only wanted the texture pack: at a model
	// load the package is taken only if it is ready to read. The switch asks
	// with mayUnpack, which is where that cost belongs - somebody has just
	// asked for the meshes.
	path = mayUnpack ? xblaImportGetStfsPath() : xblaImportGetReadyStfsPath();

	if (!path || !path[0]) {
		// Not a failure while the package is still in its archive: asking
		// again after the switch has unpacked it has to be able to succeed.
		return 0;
	}

	opened = -1;

	if (!x360StfsOpen(&stfs, path)) {
		sysLogPrintf(LOG_ERROR, "xblamesh: %s is not a package", path);
		return 0;
	}

	index = x360StfsFind(&stfs, XBLAMESH_PACKED);

	if (index < 0 || !x360StfsStreamOpen(&stfs, index, &packed)) {
		sysLogPrintf(LOG_ERROR, "xblamesh: no " XBLAMESH_PACKED " in %s", path);
		x360StfsClose(&stfs);
		return 0;
	}

	if (!x360StfsStreamRead(&packed, 0, sizeof(head), head)) {
		xblaMeshCloseUp();
		return 0;
	}

	numRecords = (s32)xblaMeshBE32(head);

	if (numRecords <= 0 || numRecords > 0x10000) {
		sysLogPrintf(LOG_ERROR, "xblamesh: %d records is not a PackedSegFile", numRecords);
		xblaMeshCloseUp();
		return 0;
	}

	tableLen = (u32)numRecords * XBLAMESH_RECORD;
	table = malloc(tableLen);
	recOffset = malloc(sizeof(u32) * numRecords);
	recUncSize = malloc(sizeof(u32) * numRecords);
	recCompSize = malloc(sizeof(u32) * numRecords);
	built = calloc(numRecords, sizeof(struct xblameshbuilt));

	if (!table || !recOffset || !recUncSize || !recCompSize || !built) {
		free(table);
		xblaMeshCloseUp();
		return 0;
	}

	if (!x360StfsStreamRead(&packed, 4, tableLen, table)) {
		free(table);
		sysLogPrintf(LOG_ERROR, "xblamesh: record table is short");
		xblaMeshCloseUp();
		return 0;
	}

	for (s32 i = 0; i < numRecords; i++) {
		const u8 *r = table + (u32)i * XBLAMESH_RECORD;
		recOffset[i] = xblaMeshBE32(r);
		recUncSize[i] = xblaMeshBE32(r + 4);
		recCompSize[i] = xblaMeshBE32(r + 8);
	}

	free(table);

	opened = 1;

	sysLogPrintf(LOG_NOTE, "xblamesh: %d slots in %s", numRecords, path);

	return 1;
}

/**
 * One slot, inflated. The caller frees it.
 *
 * A zero compressed size means the file is stored as it is; everything else is
 * one chunked XMemCompress stream.
 */
static u8 *xblaMeshReadSlot(s32 slot, u32 *outLen)
{
	u8 *packedBytes;
	u8 *out;
	u32 usize;
	u32 csize;

	if (slot < 0 || slot >= numRecords) {
		return NULL;
	}

	usize = recUncSize[slot];
	csize = recCompSize[slot];

	if (usize == 0 || usize > XBLAMESH_MAXFILE) {
		return NULL;
	}

	out = malloc(usize);

	if (!out) {
		return NULL;
	}

	if (csize == 0) {
		if (!x360StfsStreamRead(&packed, recOffset[slot], usize, out)) {
			free(out);
			return NULL;
		}

		*outLen = usize;
		return out;
	}

	if (csize > XBLAMESH_MAXFILE) {
		free(out);
		return NULL;
	}

	packedBytes = malloc(csize);

	if (!packedBytes) {
		free(out);
		return NULL;
	}

	if (!x360StfsStreamRead(&packed, recOffset[slot], csize, packedBytes) ||
			x360LzxDecompress(packedBytes, csize, out, usize) != usize) {
		free(packedBytes);
		free(out);
		return NULL;
	}

	free(packedBytes);

	*outLen = usize;
	return out;
}

/* -------------------------------------------------------------------------
 * Matching a model's nodes against the release's copy of the same file
 * ------------------------------------------------------------------------- */

/**
 * One step of the walk over their copy, which has to be the same walk the
 * promotion does over ours.
 *
 * A node is {u16 type, u16 meshid, u32 rodata, parent, next, prev, child} of
 * big-endian words at segment 0x05 addresses. The only place the two walks
 * could disagree is a distance node, whose child the promotion overwrites with
 * the rodata's target, so that is done here too.
 */
static u32 xblaMeshFileChild(const u8 *file, u32 len, u32 off, u32 type)
{
	u32 rodata;

	if (type == MODELNODETYPE_DISTANCE) {
		rodata = xblaMeshBE32(file + off + 4) & 0xffffff;

		// The word is at +8, so the bytes it takes reach +12.
		if (rodata && rodata + 12 <= len) {
			// struct modelrodata_distance: near, far, then the target
			return xblaMeshBE32(file + rodata + 8) & 0xffffff;
		}

		return 0;
	}

	return xblaMeshBE32(file + off + 20) & 0xffffff;
}

/**
 * Which of the model's matrices a node is drawn under.
 *
 * Not modelFindNodeMtxIndex(): that walks up to the nearest chrinfo or position
 * node, which for every model here is the same one for all of its parts - Dr
 * Carroll's thirteen parts all come back with one matrix. The index that
 * actually poses a part is the offset in the `G_MTX` its own display list
 * loads out of segment 3, and the only place to read that before the game has
 * rewritten the list is the model file itself. The release's copy is byte for
 * byte ours apart from the mesh ids, so it does just as well.
 *
 * -1 when the node's list does not load one.
 */
static s16 xblaMeshNodeMtx(const u8 *file, u32 len, u32 nodeoff)
{
	const u32 rodata = xblaMeshBE32(file + nodeoff + 4) & 0xffffff;
	u32 gdl;

	if (!rodata || rodata + 4 > len) {
		return -1;
	}

	// Both dl and gundl keep the opaque list first.
	gdl = xblaMeshBE32(file + rodata) & 0xffffff;

	for (s32 i = 0; gdl && i < XBLAMESH_MTXSCAN; i++, gdl += 8) {
		u32 w0;
		u32 w1;

		if (gdl + 8 > len) {
			return -1;
		}

		w0 = xblaMeshBE32(file + gdl);
		w1 = xblaMeshBE32(file + gdl + 4);

		switch (w0 >> 24) {
		case G_MTX:
			return (s16)((w1 & 0xffffff) / sizeof(Mtxf));
		case G_VTX:
		case (u8)G_ENDDL:
		case G_DL:
			return -1;
		}
	}

	return -1;
}

/**
 * The entry for this node, or the slot one would go in.
 *
 * Open addressing cannot leave a hole behind, so a dropped entry keeps its node
 * as a tombstone and only loses its model - a probe has to walk over it to
 * reach whatever was filed behind it. **Which means a tombstone has to be
 * handed back for reuse**, because models are freed and loaded again all the
 * way through a stage - every weapon the player switches to, every body and
 * head a simulant spawns with - and each of those loads leaves its nodes behind
 * as tombstones. Taking only empty slots fills the table with the dead in one
 * long match: registration stops working, and every lookup in the draw path
 * walks all 4096 entries before giving up. So the probe runs to the end of the
 * chain looking for the node and hands back the first slot it could take.
 */
static struct xblameshentry *xblaMeshSlotFor(const struct modelnode *node)
{
	u32 h = (u32)(((uintptr_t)node >> 4) * 2654435761u) & (XBLAMESH_HASHSIZE - 1);
	struct xblameshentry *reusable = NULL;

	for (s32 i = 0; i < XBLAMESH_HASHSIZE; i++) {
		struct xblameshentry *e = &hash[(h + i) & (XBLAMESH_HASHSIZE - 1)];

		if (e->node == node) {
			return e;
		}

		// The end of the chain: nothing is filed past here, so the search is
		// over and this slot - or a tombstone passed on the way - is the one.
		if (!e->node) {
			return reusable ? reusable : e;
		}

		if (!e->modeldef && !reusable) {
			reusable = e;
		}
	}

	return reusable;
}

/** The record of this model's use of this mesh, made if there is not one. */
static s32 xblaMeshUseFor(const struct modeldef *modeldef, s32 slot)
{
	s32 free = -1;

	for (s32 i = 0; i < numUses; i++) {
		if (uses[i].modeldef == modeldef && uses[i].slot == slot) {
			return i;
		}

		if (!uses[i].modeldef && free < 0) {
			free = i;
		}
	}

	if (free < 0) {
		if (numUses >= capUses) {
			s32 cap = capUses ? capUses * 2 : 64;
			struct xblameshuse *grown = realloc(uses, (size_t)cap * sizeof(*uses));

			if (!grown) {
				return -1;
			}

			uses = grown;
			capUses = cap;
		}

		free = numUses++;
	}

	memset(&uses[free], 0, sizeof(uses[free]));
	uses[free].modeldef = modeldef;
	uses[free].slot = (u16)slot;

	for (s32 i = 0; i < XBLAMESH_MAXPARTS; i++) {
		uses[free].partmtx[i] = -1;
	}

	return free;
}

/**
 * Drops every entry belonging to one modeldef, live or dead.
 *
 * Every model load comes through here now (see xblaMeshMatchModel), including
 * on a machine that has no package at all, so the table walk is skipped when
 * there is nothing in the table to walk over.
 */
static void xblaMeshForgetModel(const struct modeldef *modeldef)
{
	for (s32 i = 0; i < numUses; i++) {
		if (uses[i].modeldef == modeldef) {
			uses[i].modeldef = NULL;
		}
	}

	if (!g_XblaMeshNumNodes) {
		return;
	}

	// Open addressing cannot leave a hole behind, so a dropped entry keeps its
	// node as a tombstone and loses its model: a probe still walks over it,
	// and a lookup that lands on it sees no model and gives up.
	for (s32 i = 0; i < XBLAMESH_HASHSIZE; i++) {
		if (hash[i].node && hash[i].modeldef == modeldef) {
			hash[i].modeldef = NULL;
			hash[i].slot = 0;
			g_XblaMeshNumNodes--;
		}
	}
}

static s32 xblaMeshVerbose;
static s32 xblaMeshDrawLog;
static s32 xblaMeshFileId;

/**
 * What the node was going to draw, next to what will be drawn instead.
 *
 * The release's geometry is meant to be the model's own coordinates 1:1, so
 * the two boxes should sit on top of each other. --xbla-mesh-verbose is how
 * that gets checked on something the game is actually drawing rather than on
 * a file.
 */
static void xblaMeshLogNode(const struct modelnode *node, u32 type, s32 slot, s32 part)
{
	const Vtx *vertices = NULL;
	s32 numvertices = 0;
	s16 lo[3];
	s16 hi[3];

	if (type == MODELNODETYPE_DL) {
		vertices = node->rodata->dl.vertices;
		numvertices = node->rodata->dl.numvertices;
	} else {
		vertices = node->rodata->gundl.vertices;
		numvertices = node->rodata->gundl.numvertices;
	}

	if (!vertices || numvertices <= 0) {
		sysLogPrintf(LOG_NOTE, "xblamesh: node %p slot %d part %d, no stock vertices",
				node, slot, part);
		return;
	}

	for (s32 i = 0; i < 3; i++) {
		lo[i] = vertices[0].v[i];
		hi[i] = vertices[0].v[i];
	}

	for (s32 i = 1; i < numvertices; i++) {
		for (s32 j = 0; j < 3; j++) {
			if (vertices[i].v[j] < lo[j]) {
				lo[j] = vertices[i].v[j];
			}

			if (vertices[i].v[j] > hi[j]) {
				hi[j] = vertices[i].v[j];
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "xblamesh: node %p slot %d part %d, stock %d verts "
			"[%d %d %d]..[%d %d %d]",
			node, slot, part, numvertices, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
}

/**
 * Whether this node is the near copy of a head's hair, which is the one piece
 * of stock geometry the release's mesh has already.
 *
 * Walked on our side rather than theirs, since the two trees are the same tree
 * here by construction, and a head is not grafted onto a body yet at the load
 * this runs from - so the walk ends at the head model's own root.
 *
 * The game names it: a head model's parts number its toggled pieces, and
 * `MODELPART_HEAD_HAT` (1) is the hair - `Cheadwlab`'s slab of it, the piece
 * that came out hanging in the air over the release's own short hair. 53 of
 * the 76 heads have one and the release gives **none** of the 53 a mesh id,
 * where it gives the sunglasses beside them one in 45 of the 51 heads that
 * have those. A piece 4J modelled gets a group; the hair never does, in any
 * head, because it is painted into the head itself.
 *
 * Two things this will not do, both of which the code it replaced did:
 *
 *   * **the sunglasses of the six heads the release left at zero**
 *     (`Cheadanka`, `Cheaddarling`, `Cheaddavec`, `Cheadfem_guard`,
 *     `Cheadjon`, `Cheadjonathan`) keep their own geometry. Nothing in the
 *     mesh replaces them, so suppressing them took a character's glasses off;
 *   * **a far LOD alternative** keeps its own geometry, hair included. The
 *     head a chr is drawn from past 6000 units is the game's own - of the
 *     132 ids the release gives a head, the 124 under a distance node are
 *     every one on an alternative that starts at 0 - so a distant head that
 *     lost its hair would just be bald. Only the alternative drawn where the
 *     mesh is drawn is the one the mesh has already.
 *
 * And it is asked of a head only. Part 1 is a toggle in one other model,
 * `MODELPART_DRCAROLL_0001`, and a part number means nothing outside the
 * skeleton that numbered it - so the model's skeleton is checked first. A
 * head's is the one skeleton the game never promotes to a pointer (there is no
 * `g_SkelHead` in `g_Skeletons[]`), so it is still the number `SKEL_HEAD` here,
 * which is how body.c reads it too.
 */
static s32 xblaMeshIsHeadModel(const struct modeldef *modeldef)
{
	return (uintptr_t)modeldef->skel < 0x10000 && (s16)(uintptr_t)modeldef->skel == SKEL_HEAD;
}

/** Whether the model's parts table names this node at all. */
static s32 xblaMeshNodeIsNumbered(const struct modeldef *modeldef, const struct modelnode *node)
{
	for (s32 i = 0; i < modeldef->numparts; i++) {
		if (modeldef->parts[i] == node) {
			return 1;
		}
	}

	return 0;
}

static s32 xblaMeshIsHairList(struct modeldef *modeldef, const struct modelnode *node)
{
	const struct modelnode *hat;

	if (!xblaMeshIsHeadModel(modeldef)) {
		return 0;
	}

	hat = modelGetPart(modeldef, MODELPART_HEAD_HAT);

	if (hat && (hat->type & 0xff) != MODELNODETYPE_TOGGLE) {
		return 0;
	}

	if (!hat && xblaMeshFileId != FILE_CHEADROBIN) {
		// One head keeps its hair under a toggle its parts table does not
		// number - Robin's (`CheadrobinZ`: parts 0x191 and the sunglasses,
		// and the hair's toggle in neither). The game can reach a toggle
		// only through its part number, so that one is on for ever, and it
		// was the last hair slab still hanging over a release head once the
		// numbered ones were gone. For Robin the hair is "the toggle no part
		// names"; for every other head without a hat part there is nothing
		// to suppress.
		return 0;
	}

	for (s32 i = 0; node && i < XBLAMESH_PARENTSCAN; i++) {
		const u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_TOGGLE) {
			// The hair's toggle, or some other toggled piece.
			if (hat) {
				return node == hat;
			}

			return !xblaMeshNodeIsNumbered(modeldef, node);
		}

		if (type == MODELNODETYPE_DISTANCE) {
			// The far alternative of the pair, which the mesh does not stand
			// in for. Both of a hair piece's two lists are under one of these.
			if (!node->rodata || node->rodata->distance.near != 0.0f) {
				return 0;
			}
		} else if (node == modeldef->rootnode) {
			// The whole way up without meeting the hair's toggle.
			return 0;
		}

		node = node->parent;
	}

	return 0;
}

/**
 * Whether the model's own mesh has this node's geometry already.
 *
 * **A mesh is the whole model, and the release names it on one node.** The id
 * is written into one list node's padding and the fourteen or fifteen others
 * of a character - a thigh, a forearm, a shoulder - are left at zero, because
 * 4J had no reason to mark what their own art already contains. Read as "a
 * zero keeps its own geometry", which is what this did, every guard in the
 * game drew the release's body *and* the N64 body inside it: `CcarringtonZ` is
 * thirty list nodes, one of them named, and the mesh named on it is 4792
 * vertices over fifteen palette entries - the whole standing figure - while
 * the other twenty-nine draw 2391 vertices of a complete second character in
 * the same place. 127 of the 134 character models are that shape, and so are
 * the Carrington Institute's sofa, its hovercopter and its autosurgeon: 946
 * list nodes across the release, against 1796 the old reading kept.
 *
 * What the mesh does *not* stand in for is the two kinds of list the game
 * draws instead of, rather than beside, the one it was named on:
 *
 *   * **a far LOD alternative** - a distance node whose near threshold is not
 *     zero. The mesh's own node is under the near alternative of its pair, so
 *     past that distance nothing of the mesh is drawn and the game's low-poly
 *     copy is the whole model. Suppressed, a distant guard would be nothing at
 *     all. (This is the same test xblaMeshMatchBySize's leftovers must pass.)
 *   * **a toggled piece** - geometry the game switches on and off: a gun's
 *     muzzle flash (eleven of them), the sunglasses of the six heads the
 *     release left at zero, the pieces of the two Nintendo logos. 4J marked
 *     the toggled pieces they *did* remodel with an id and the ones they kept
 *     with 0xFFFF, so a toggled zero is one they never looked at, and taking
 *     it away takes a character's glasses off. The one exception is a head's
 *     hair, which is toggled and *is* in the mesh, and which
 *     xblaMeshIsHairList() names from the game's own MODELPART_HEAD_HAT
 *     before this is asked.
 */
static s32 xblaMeshIsCovered(struct modeldef *modeldef, const struct modelnode *node)
{
	for (s32 i = 0; node && i < XBLAMESH_PARENTSCAN; i++) {
		const u32 type = node->type & 0xff;

		if (node == modeldef->rootnode) {
			return 1;
		}

		if (type == MODELNODETYPE_DISTANCE) {
			if (!node->rodata || node->rodata->distance.near != 0.0f) {
				return 0;
			}
		} else if (type == MODELNODETYPE_TOGGLE) {
			return 0;
		}

		node = node->parent;
	}

	return 0;
}

/**
 * Files a node that draws nothing while the model's mesh is drawn.
 */
static void xblaMeshSuppressNode(struct modeldef *modeldef, struct modelnode *node,
		s32 slot, s32 kind)
{
	struct xblameshentry *e = xblaMeshSlotFor(node);

	if (!e) {
		return;
	}

	if (!e->modeldef) {
		g_XblaMeshNumNodes++;
	}

	if (!e->node) {
		g_XblaMeshNumSlots++;
	}

	e->node = node;
	e->modeldef = modeldef;
	e->slot = (u16)(slot > 0 ? slot : 0);
	e->part = 0;
	e->use = -1;
	e->suppress = kind;
}

/**
 * Walks our tree and theirs together, recording every node they replace.
 *
 * The two files hold the same nodes in the same order - the release edited two
 * padding bytes and nothing else - so the walk is a straight zip, and any
 * disagreement about a node's type means we are not looking at the same model
 * and the whole file is left alone.
 */
static s32 xblaMeshMatchNodes(struct modeldef *modeldef, const u8 *file, u32 len)
{
	struct modelnode *ournode = modeldef->rootnode;
	u32 theiroff = xblaMeshBE32(file) & 0xffffff;
	struct modelnode *covered[XBLAMESH_COVERED];
	s32 numcovered = 0;
	s32 firstslot = -1;
	s32 found = 0;
	s32 suppressed = 0;
	s32 walked = 0;

	// Both walks are the same depth-first order the game's own iteration uses:
	// down to the child, then along next, then back up to the parent's next.
	while (ournode && theiroff) {
		u32 ourtype;
		u32 theirtype;
		u32 theirchild;
		u16 id;

		if (theiroff + 24 > len) {
			return 0;
		}

		ourtype = ournode->type & 0xff;
		theirtype = xblaMeshBE16(file + theiroff) & 0xff;

		if (ourtype != theirtype) {
			return 0;
		}

		if (++walked > 4096) {
			return 0;
		}

		id = xblaMeshBE16(file + theiroff + 2);

		// 0xFFFF is not an id: it means this node keeps its own geometry.
		if (id && id != 0xffff &&
				(ourtype == MODELNODETYPE_DL || ourtype == MODELNODETYPE_GUNDL)) {
			struct xblameshentry *e = xblaMeshSlotFor(ournode);

			// The low 12 bits are a PackedSegFile file id, and slot i is file
			// id i + 1 the way the game's own files are - so the mesh is one
			// slot below the number in the node. Reading it as the slot itself
			// gives a mesh for every model and the wrong one for nearly all of
			// them, which is a much harder mistake to see than a miss: a chair
			// comes back as the chair beside it.
			s32 slot = (s32)(id & 0xfff) - 1;

			if (e && slot >= 0 && slot < numRecords && recUncSize[slot]) {
				// A slot with no model in it is empty or a tombstone, and
				// either way this is one more live entry. One that has a model
				// is an entry being taken over, which is one out and one in.
				if (!e->modeldef) {
					g_XblaMeshNumNodes++;
				}

				if (!e->node) {
					g_XblaMeshNumSlots++;
				}

				e->node = ournode;
				e->modeldef = modeldef;
				e->slot = (u16)slot;
				e->part = (u16)(id >> 12);

				// Which mesh the model's covered lists wait on, and whether
				// there is one at all: only a mesh named on a list the game
				// draws beside them can stand in for them. `CheadgreyZ` names
				// its only mesh on a toggled alternative, and suppressing the
				// head beside it would leave the Grey with no head whenever
				// that toggle is off.
				if (firstslot < 0 && xblaMeshIsCovered(modeldef, ournode)) {
					firstslot = slot;
				}

				e->use = xblaMeshUseFor(modeldef, slot);
				e->suppress = 0;
				found++;

				if (e->use >= 0 && e->part < XBLAMESH_MAXPARTS) {
					struct xblameshuse *use = &uses[e->use];

					use->parts[e->part] = ournode;
					use->partmtx[e->part] = xblaMeshNodeMtx(file, len, theiroff);

					if (e->part >= use->numparts) {
						use->numparts = (u16)(e->part + 1);
					}
				}

				if (xblaMeshVerbose) {
					xblaMeshLogNode(ournode, ourtype, slot, id >> 12);
				}

				if (xblaMeshVerbose) {
					sysLogPrintf(LOG_NOTE, "xblamesh:   walked %d nodes to it", walked);
				}

				if (xblaMeshVerbose) {
					sysLogPrintf(LOG_NOTE, "xblamesh:   file %d node %p -> slot %d",
							xblaMeshFileId, ournode, slot);
				}
			}
		} else if (!id && (ourtype == MODELNODETYPE_DL || ourtype == MODELNODETYPE_GUNDL)) {
			// A zero is not "this node keeps what it has". The release names a
			// mesh on one node of a model and leaves the rest of that model's
			// lists at zero, and the mesh is the whole model - so a zero is
			// geometry the mesh has already, unless it is one of the lists the
			// game draws *instead of* the named one. xblaMeshIsCovered() is
			// where that is decided, and the hair is asked about first because
			// it is a toggled piece that the mesh does have.
			//
			// 4J marked a toggled piece they kept with 0xFFFF and gave one
			// they remodelled an id; the hair gets neither, in any of the 53
			// heads that have one, because it is painted into the head.
			// Drawing the game's over the top gives a guard two hairdos - the
			// N64 one hanging above the release's head, where the scalp it was
			// cut to fit no longer is. Which node that is comes from the game
			// rather than from the shape of the tree: xblaMeshIsHairList()
			// asks the model for its MODELPART_HEAD_HAT.
			if (xblaMeshIsHairList(modeldef, ournode)) {
				xblaMeshSuppressNode(modeldef, ournode, 0, XBLAMESH_SUPPRESS_HAIR);
				suppressed++;
			} else if (xblaMeshIsCovered(modeldef, ournode) && numcovered < XBLAMESH_COVERED) {
				// Held until the walk is over: a model whose tree stops
				// matching part way through leaves through one of the returns
				// above, and nothing of it may be left filed.
				covered[numcovered++] = ournode;
			}
		}

		theirchild = xblaMeshFileChild(file, len, theiroff, theirtype);

		if (ournode->child && theirchild) {
			ournode = ournode->child;
			theiroff = theirchild;
			continue;
		}

		if ((ournode->child == NULL) != (theirchild == 0)) {
			return 0;
		}

		while (ournode) {
			// Every read here is a word out of the node at theiroff, and after
			// a step up to the parent that offset came out of the file and has
			// not been looked at - so it is checked against the node's whole
			// 24 bytes before either link is read out of it.
			if (theiroff + 24 > len) {
				return 0;
			}

			if (ournode->next) {
				ournode = ournode->next;
				theiroff = xblaMeshBE32(file + theiroff + 12) & 0xffffff;
				break;
			}

			ournode = ournode->parent;
			theiroff = xblaMeshBE32(file + theiroff + 8) & 0xffffff;
		}

		if (!ournode) {
			break;
		}

		if (!theiroff) {
			return 0;
		}
	}

	// Only a model that really did match: the walk registers as it goes, and a
	// model that names no mesh at all must keep every list it has.
	if (found && firstslot >= 0) {
		for (s32 i = 0; i < numcovered; i++) {
			xblaMeshSuppressNode(modeldef, covered[i], firstslot,
					XBLAMESH_SUPPRESS_COVERED);
		}
	}

	if (found && xblaMeshVerbose) {
		if (suppressed) {
			sysLogPrintf(LOG_NOTE, "xblamesh:   %d toggled stock pieces the mesh has already",
					suppressed);
		}

		if (numcovered) {
			sysLogPrintf(LOG_NOTE, "xblamesh:   %d more lists the mesh covers, drawing nothing",
					numcovered);
		}
	}

	return found;
}

/**
 * How many vertices one of our nodes draws, or -1 if it is not a list node.
 */
static s32 xblaMeshNodeNumVertices(const struct modelnode *node)
{
	const u32 type = node->type & 0xff;

	if (!node->rodata) {
		return -1;
	}

	if (type == MODELNODETYPE_DL) {
		return node->rodata->dl.numvertices;
	}

	if (type == MODELNODETYPE_GUNDL) {
		return node->rodata->gundl.numvertices;
	}

	return -1;
}

/** The next node of our tree, in the order the game's own iteration takes. */
static struct modelnode *xblaMeshNextNode(struct modelnode *node)
{
	if (node->child) {
		return node->child;
	}

	while (node) {
		if (node->next) {
			return node->next;
		}

		node = node->parent;
	}

	return NULL;
}

/**
 * The last resort for a model the release rebuilt: pair the parts by size.
 *
 * Three files in the release are not ours with two bytes changed - they are
 * ours with the nodes rearranged, and the zip above refuses them because a
 * type does not line up. All three are Joanna's own head (combat, frock and
 * aqua), which is the model a player looks at most in multiplayer, so they are
 * worth having. What 4J did to them was to move the toggled piece - the
 * earpiece on the right of her head - in front of the head itself, and in the
 * aqua one to add a node.
 *
 * The pairing that gets all three right is by size: our biggest list is the
 * head and takes part 0, our next biggest is the earpiece and takes part 1.
 * That holds because the mesh's own groups come the same way round - the head
 * group of her mesh is 2341 vertices against the earpiece's 672 - and because
 * the part number is what says which group a node stands for.
 *
 * It is deliberately narrow. It runs only once the zip has failed, it wants
 * the release's copy to name exactly one mesh and to number its parts 0..n-1
 * with no gaps, and it wants at least that many lists on our side. Anything
 * else keeps its own geometry, which is what all three of these did before.
 */
static s32 xblaMeshMatchBySize(struct modeldef *modeldef, const u8 *file, u32 len)
{
	u32 theiroff[XBLAMESH_MAXPARTS];
	struct modelnode *ours[XBLAMESH_MAXPARTS];
	s32 ourverts[XBLAMESH_MAXPARTS];
	struct modelnode *node;
	s32 slot = -1;
	s32 numparts = 0;
	s32 numours = 0;
	s32 walked = 0;
	u32 off = xblaMeshBE32(file) & 0xffffff;

	for (s32 i = 0; i < XBLAMESH_MAXPARTS; i++) {
		theiroff[i] = 0;
	}

	// Their side: every node that names a mesh, by part number.
	while (off && walked++ < 4096) {
		u32 type;
		u32 id;
		u32 child;

		if (off + 24 > len) {
			return 0;
		}

		type = xblaMeshBE16(file + off) & 0xff;
		id = xblaMeshBE16(file + off + 2);

		if (id && id != 0xffff &&
				(type == MODELNODETYPE_DL || type == MODELNODETYPE_GUNDL)) {
			const s32 theirslot = (s32)(id & 0xfff) - 1;
			const u32 part = id >> 12;

			if ((slot >= 0 && theirslot != slot) || part >= XBLAMESH_MAXPARTS ||
					theiroff[part]) {
				return 0;
			}

			slot = theirslot;
			theiroff[part] = off;

			if ((s32)part + 1 > numparts) {
				numparts = (s32)part + 1;
			}
		}

		child = xblaMeshFileChild(file, len, off, type);

		if (child) {
			off = child;
			continue;
		}

		while (off) {
			const u32 next = xblaMeshBE32(file + off + 12) & 0xffffff;

			if (next) {
				off = next;
				break;
			}

			off = xblaMeshBE32(file + off + 8) & 0xffffff;

			if (off && off + 24 > len) {
				return 0;
			}
		}
	}

	if (slot < 0 || slot >= numRecords || !recUncSize[slot]) {
		return 0;
	}

	for (s32 i = 0; i < numparts; i++) {
		if (!theiroff[i]) {
			return 0; // a gap in the part numbers
		}
	}

	// Our side: the lists, biggest first. An insertion sort, because a model
	// that gets this far has a handful of them.
	for (node = modeldef->rootnode; node; node = xblaMeshNextNode(node)) {
		const s32 verts = xblaMeshNodeNumVertices(node);
		s32 at = numours < XBLAMESH_MAXPARTS ? numours : XBLAMESH_MAXPARTS - 1;

		if (verts <= 0) {
			continue;
		}

		while (at > 0 && ourverts[at - 1] < verts) {
			ours[at] = ours[at - 1];
			ourverts[at] = ourverts[at - 1];
			at--;
		}

		ours[at] = node;
		ourverts[at] = verts;

		if (numours < XBLAMESH_MAXPARTS) {
			numours++;
		}
	}

	if (numours < numparts) {
		return 0;
	}

	for (s32 part = 0; part < numparts; part++) {
		struct xblameshentry *e = xblaMeshSlotFor(ours[part]);

		if (!e) {
			return 0;
		}

		if (!e->modeldef) {
			g_XblaMeshNumNodes++;
		}

		if (!e->node) {
			g_XblaMeshNumSlots++;
		}

		e->node = ours[part];
		e->modeldef = modeldef;
		e->slot = (u16)slot;
		e->part = (u16)part;
		e->use = xblaMeshUseFor(modeldef, slot);
		e->suppress = 0;

		if (e->use >= 0) {
			struct xblameshuse *use = &uses[e->use];

			use->parts[part] = ours[part];
			use->partmtx[part] = xblaMeshNodeMtx(file, len, theiroff[part]);

			if (part >= use->numparts) {
				use->numparts = (u16)(part + 1);
			}
		}

		if (xblaMeshVerbose) {
			sysLogPrintf(LOG_NOTE, "xblamesh:   by size: part %d -> node %p, "
					"%d verts, slot %d", part, ours[part], ourverts[part], slot);
		}
	}

	// Everything the pairing did not take has to be something it was right to
	// leave. A model that gets this far is one whose tree did not zip against
	// the release's copy, so there is nothing to say which of its lists the
	// mesh stands for beyond their sizes - and the three heads this path
	// exists for leave exactly one thing behind: a far LOD alternative, which
	// the game draws instead of the near list rather than beside it.
	//
	// Anything else left over is drawn *with* what the mesh replaced, over the
	// top of it, and that is not a pairing that has understood the model. A
	// mod's model is the case that showed it: one 159-vertex list took the
	// whole of a release mesh and the other twenty-four - near lists, toggled
	// pieces, a head's hair - carried on drawing the game's own geometry
	// through it. Refused here, the model keeps all of its own geometry, which
	// is the right answer for a model this cannot read.
	for (node = modeldef->rootnode; node; node = xblaMeshNextNode(node)) {
		const struct modelnode *up;
		s32 paired = 0;
		s32 lod = 0;

		if (xblaMeshNodeNumVertices(node) <= 0) {
			continue;
		}

		for (s32 i = 0; i < numparts; i++) {
			if (ours[i] == node) {
				paired = 1;
				break;
			}
		}

		if (paired) {
			continue;
		}

		// The far half of an LOD pair: a distance node whose near threshold is
		// not zero, reached before the root and before any toggle.
		for (up = node; up && !lod; up = up->parent) {
			const u32 t = up->type & 0xff;

			if (t == MODELNODETYPE_DISTANCE) {
				lod = up->rodata && up->rodata->distance.near != 0.0f;
				break;
			}

			if (t == MODELNODETYPE_TOGGLE || up == modeldef->rootnode) {
				break;
			}
		}

		if (!lod) {
			if (xblaMeshVerbose) {
				sysLogPrintf(LOG_NOTE, "xblamesh:   by size refused slot %d: a %d vertex "
						"list is neither paired nor an LOD alternative, so the game "
						"would draw it over the mesh", slot,
						xblaMeshNodeNumVertices(node));
			}

			return 0;
		}
	}

	return numparts;
}

/**
 * The models the boot sequence draws, which the release remade as its own
 * boot sequence.
 *
 * 4J's package holds a mesh for each of them, and every one of those meshes is
 * the Xbox 360 release's logo rather than the N64's: file 221's mesh is
 * "Microsoft Game Studios" where the game's own is the Nintendo wordmark, and
 * file 1376's is the flat orange Rare plaque on a full screen orange field
 * where the game's is the gold one on black. Matched, they replace the N64
 * intro with the 360's, which reads as the logos being discoloured - the Rare
 * screen turns orange to its edges and the Nintendo screen turns into
 * Microsoft's.
 *
 * They are the one place where the release's mesh is a *different logo*, not a
 * better model of the same thing, so the boot sequence keeps the game's own.
 */
static s32 xblaMeshIsBootLogo(u16 fileid)
{
	switch (fileid) {
	case FILE_PRARELOGO:
	case FILE_PNINTENDOLOGO:
	case FILE_PNLOGO:
	case FILE_PNLOGO2:
	case FILE_PNLOGO3:
	case FILE_PJPNLOGO:
		return 1;
	}

	return 0;
}

/**
 * Matches one model's nodes against the release's copy of the same file.
 *
 * Runs for every model that loads. It will not unpack an archive to do it -
 * see xblaMeshRegisterModel() - so on a machine whose package is not ready
 * this does nothing at all.
 */
static void xblaMeshMatchModel(struct modeldef *modeldef, u16 fileid)
{
	u8 *file;
	u32 len;
	s32 found;

	if (!modeldef || !modeldef->rootnode) {
		return;
	}

	// Whatever is registered at this address belongs to a model that has been
	// freed, since this one has only just been loaded into it. Dropped here,
	// before anything can return: a model the release has no copy of leaves
	// through one of the three returns below, and if it kept the dead entries
	// it would inherit their meshes with them - the draw path's definition
	// test cannot tell the two apart, both models being the same address. That
	// is a stage's own doing rather than a stage change's, so lvReset() is not
	// where it can be caught.
	xblaMeshForgetModel(modeldef);

	// A model load never unpacks a 250MB archive on somebody who might only
	// want the texture pack - unless they have already asked for the meshes, in
	// which case the first model load is exactly who should pay for it. Without
	// that, Mod.XblaMeshes=1 in pd.ini with the release still inside its .7z
	// drew no mesh at all and said nothing: nothing unpacked, so nothing was
	// ever matched, and the checkbox that would have unpacked it was already
	// on. Off, this is still the speculative pass that keeps the switch live.
	if (!xblaMeshOpen(optEnabled)) {
		return;
	}

	// A mod's model is not the model this mesh is a mesh for.
	//
	// The release's package is keyed on the game's own file ids, and a mod
	// replaces a file's contents while keeping its id: GoldenEye X's file 447
	// is a GoldenEye character where the release's 447 is CbiotechZ. Matched
	// anyway, the node-for-node zip refuses it - the trees disagree - and the
	// pairing by size then takes it, hands the model's biggest list the whole
	// of somebody else's mesh, and leaves its other twenty-four lists drawing
	// their own geometry over it. That is what "the release's models and the
	// game's are both on the screen, and the hair floats above the head" is:
	// the head's hair is one of the twenty-four.
	//
	// So a model is only paired when its bytes came out of the ROM. A mod that
	// leaves a file alone still gets the release's mesh for it, which is most
	// of them.
	if (!romdataFileIsStock(fileid)) {
		if (xblaMeshVerbose) {
			sysLogPrintf(LOG_NOTE, "xblamesh: model file %d is a mod's, not the "
					"release's - left alone", fileid);
		}

		return;
	}

	if (xblaMeshIsBootLogo(fileid)) {
		if (xblaMeshVerbose) {
			sysLogPrintf(LOG_NOTE, "xblamesh: model file %d is a boot logo - the "
					"release's is a different logo, left alone", fileid);
		}

		return;
	}

	// Slot i is the game's file id i + 1.
	file = xblaMeshReadSlot((s32)fileid - 1, &len);

	if (!file) {
		return;
	}

	xblaMeshFileId = fileid;

	if (xblaMeshVerbose) {
		sysLogPrintf(LOG_NOTE, "xblamesh: model file %d, %u bytes", fileid, len);
	}

	found = xblaMeshMatchNodes(modeldef, file, len);

	if (xblaMeshVerbose) {
		sysLogPrintf(LOG_NOTE, "xblamesh: model file %d matched %d", fileid, found);
	}

	if (found == 0) {
		// Either the release replaced nothing in this model, or the two trees
		// disagreed and the partial matching has to come back out.
		xblaMeshForgetModel(modeldef);

		found = xblaMeshMatchBySize(modeldef, file, len);

		if (found == 0) {
			xblaMeshForgetModel(modeldef);
		} else if (xblaMeshVerbose) {
			sysLogPrintf(LOG_NOTE, "xblamesh: model file %d matched %d by size",
					fileid, found);
		}
	}

	free(file);
}

/**
 * Every model is matched as it loads, whether or not the meshes are switched
 * on, so that switching them on is a live thing to do.
 *
 * This used to be gated on the switch, which meant a level loaded with the
 * meshes off had nothing to draw when they were turned on: the models had
 * never been looked at, and a register of loaded models to go back over is not
 * something that can be kept - a modeldef can be freed inside a stage and its
 * address handed out again, and walking one that has been is a wild pointer
 * away from a crash (it was: file 1369 in the G5 Building, whose rootnode had
 * become 0xbe0003ffe0 by the time the switch was flipped).
 *
 * So the work is done up front instead, and it is affordable: 55 models in the
 * G5 Building carry a mesh, matching them is a slot read and a tree walk each,
 * and five loads of the level with this always on and five with it gated came
 * out inside each other's run-to-run spread. What is *not* affordable is
 * unpacking a 250MB archive for somebody who only ever wanted the texture
 * pack, so this asks for the package only if there is one ready to read -
 * xblaMeshOpen(0). A player whose copy is still inside its .7z pays for the
 * unpack the first time they switch the meshes on, and from the next level
 * load onwards is in the same place as everybody else.
 */
void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid)
{
	xblaMeshMatchModel(modeldef, fileid);
}

// Both sides of the pose arena: kilobytes held and chunks taken.
static void xblaMeshArenaStats(u32 *kb, s32 *chunks);

/**
 * Drops everything keyed on a model, because the stage pool that held all of
 * those addresses has just been handed back.
 *
 * Called from lvReset() beside the texture ids, which are dropped there for
 * exactly the same reason: the addresses are about to be given out again to
 * different things. Without it the registry carries a stage's worth of dead
 * nodes into the next stage, where an address that comes back has to be caught
 * by the modeldef test at draw time; with it there is nothing to catch. The
 * built meshes themselves stay - they are keyed on a slot in the release's
 * package, which no stage load can change.
 *
 * This is not the only place a dead entry goes, and it must not be the one
 * that is relied on: a modeldef is freed and reused inside a stage as well,
 * which no reset sees. What covers that is the load itself - a model forgets
 * whatever was registered at its own address before it looks at anything, so
 * the table only ever holds entries put there by a model that is still in the
 * memory they name. This reset is then what clears the ones whose model is
 * never loaded again.
 */
void xblaMeshResetModels(void)
{
	// Read before the table is emptied: this is the level that is ending.
	const u32 nodes = g_XblaMeshNumNodes;
	const u32 slots = g_XblaMeshNumSlots;

	numUses = 0;
	openedLate = 0;
	xblaMeshDrawLog = 0;

	memset(hash, 0, sizeof(hash));
	g_XblaMeshNumNodes = 0;
	g_XblaMeshNumSlots = 0;

	for (s32 i = 0; i < numRecords && built; i++) {
		built[i].posedmodel = NULL;
	}

	// What the meshes built so far are holding. They are kept for the life of
	// the process on purpose - a mesh is the same in every level that uses it,
	// and the alternative is freeing a display list the render thread may
	// still be running - and the ceiling is what makes that affordable: there
	// are 595 meshes in the release and building every one of them comes to
	// about 69MB, against the 250MB package the player already has on disk. A
	// level's own set is a small fraction of that (14 meshes in the G5
	// Building), so this line is how a session that has drifted upwards would
	// show itself.
	if (g_XblaMeshNumMeshes) {
		// The pose arena is named beside them because it is the other thing
		// that only ever grows, and the two are read together: a level that
		// wants more poses than the last one keeps the chunks it took for
		// them.
		u32 arenakb;
		s32 chunks;

		xblaMeshArenaStats(&arenakb, &chunks);

		// The node table is named too, since every list of a matched model is
		// filed now and not only the ones the release named - fifteen entries
		// a character rather than one - and a full table stops registering
		// anything at all.
		sysLogPrintf(LOG_NOTE, "xblamesh: %u meshes built, %u KB; pose arena %u KB "
				"in %d chunks; %u nodes in %u of %d table slots",
				g_XblaMeshNumMeshes, (g_XblaMeshBytes + 1023) / 1024, arenakb,
				chunks, nodes, slots, XBLAMESH_HASHSIZE);
	}
}

/* -------------------------------------------------------------------------
 * Turning a mesh into a display list
 * ------------------------------------------------------------------------- */

struct xblameshhdr {
	u32 numvertices;
	u32 vertexoffset;
	u32 indexoffset;
	u32 numdraws;
	u32 drawoffset;
	u32 nummatrices;
	f32 unknown;
	u32 groupoffset;
};

/**
 * Reads a header and checks it against the file, which is what says this slot
 * holds a mesh at all - the same slots also hold the game's own files.
 *
 * Every invariant here holds over all 595 meshes that parse (xbla.md): the
 * matrices reach the group table, the draws reach the vertices, and the stride
 * that falls out of the vertex span is one of the two.
 */
static s32 xblaMeshReadHeader(struct xblameshhdr *h, const u8 *file, u32 len, u32 *stride)
{
	u32 span;

	if (len < XBLAMESH_HEADER) {
		return 0;
	}

	h->numvertices = xblaMeshBE32(file);
	h->vertexoffset = xblaMeshBE32(file + 4);
	h->indexoffset = xblaMeshBE32(file + 8);
	h->numdraws = xblaMeshBE32(file + 12);
	h->drawoffset = xblaMeshBE32(file + 16);
	h->nummatrices = xblaMeshBE32(file + 20);
	h->unknown = xblaMeshBEF32(file + 24);
	h->groupoffset = xblaMeshBE32(file + 28);

	if (h->numvertices == 0 || h->numvertices > XBLAMESH_MAXVERTS) {
		return 0;
	}

	if (h->numdraws == 0 || h->numdraws > XBLAMESH_MAXDRAWS) {
		return 0;
	}

	// The palette is read straight out of the header's count, and the count is
	// what the group offset below is checked against - so it is bounded here,
	// where a slot that holds one of the game's own files rather than a mesh is
	// still being told apart from one that does. The largest palette in the
	// release has 46 entries; an unskinned mesh has none.
	if (h->nummatrices > XBLAMESH_MAXMTX) {
		return 0;
	}

	if (h->vertexoffset < XBLAMESH_HEADER || h->indexoffset <= h->vertexoffset ||
			h->indexoffset > len) {
		return 0;
	}

	if (h->groupoffset != XBLAMESH_HEADER + XBLAMESH_MATRIX * h->nummatrices) {
		return 0;
	}

	// The draw table sits between the groups and the vertices and fills the gap
	// exactly. Named as the two ends rather than as one sum, since a wild
	// offset plus the table's length can wrap round to the right answer.
	if (h->drawoffset < h->groupoffset || h->drawoffset >= h->vertexoffset ||
			h->vertexoffset - h->drawoffset != XBLAMESH_ENTRY * h->numdraws) {
		return 0;
	}

	span = h->indexoffset - h->vertexoffset;

	if (span % h->numvertices) {
		return 0;
	}

	*stride = span / h->numvertices;

	if (*stride != XBLAMESH_STRIDE_RIGID && *stride != XBLAMESH_STRIDE_SKIN) {
		return 0;
	}

	return 1;
}

/**
 * What one of a mesh's units is worth in the game's.
 *
 * The float at +0x18 is a scale: 100.0 means the mesh is in the model file's
 * own coordinates and 1000.0 means it is at a tenth of them. Every unskinned
 * mesh says 100 and draws 1:1 against the geometry it replaces - an Area 51
 * crate is 100 units across in both - and 267 of the 277 skinned ones say
 * 1000, with 200.0 twice, 750.0 once and 100.0 seven times.
 *
 * What says it is a scale rather than a number that happens to sort them: take
 * every model that names a mesh, walk its joints, and compare the offset the
 * model file states for each one against the offset the mesh's palette implies
 * for the same pair of bones. Of the 100 models with eight or more joints to
 * compare, 96 come out at this exactly. The four that do not are 4J's
 * remodelled Bonds - Connery, Dalton, Moore and the DJ - which are a uniform
 * 10% larger than the skeleton the game poses them with.
 *
 * A mesh whose header says zero - there is one - keeps its own units rather
 * than collapsing to a point.
 */
static f32 xblaMeshScale(const struct xblameshhdr *h)
{
	if (h->unknown <= 0.0f || h->unknown > 100000.0f) {
		return 1.0f;
	}

	return h->unknown / 100.0f;
}

static s16 xblaMeshRound(f32 v)
{
	f32 r = v < 0 ? v - 0.5f : v + 0.5f;

	if (r > 32767.0f) {
		return 32767;
	}

	if (r < -32768.0f) {
		return -32768;
	}

	return (s16)r;
}

struct xblameshbuilder {
	Gfx *gdl;
	Vtx *vertices;
	Col *colours;
	f32 *bindpos;
	f32 *weights;
	u8 *bones;
	s32 skinned;
	f32 scale;

	// The texture gradient along x and along y at every emitted vertex, and
	// how good the triangle it came from was for the purpose - see
	// xblaMeshNoteTriangle(). Unskinned meshes only: a door is never skinned,
	// and a character is most of the vertices there are.
	f32 *grad;
	f32 *gradscore;

	// One list per group, by index into gdl until the array stops moving, and
	// the little list that calls all of them for a model whose parts and
	// groups do not line up.
	//
	// Twice over: a group's draws are split by the alpha flag of the material
	// each one names, so that the piece of a model the game draws in the
	// translucent pass can be drawn there. groupxlu is -1 for a group with no
	// alpha material in it, which is most of them.
	s32 groupgfx[XBLAMESH_MAXPARTS];
	s32 groupxlu[XBLAMESH_MAXPARTS];
	s32 groupfade[XBLAMESH_MAXPARTS];
	s32 numgroups;
	s32 allgfx;
	s32 allxlu;
	s32 allfade;

	s32 numgfx, capgfx;
	s32 numvtx, capvtx;
	s32 numtris;

	// Which span of the group is being built, which is what says whether a
	// vertex's alpha is kept - see xblaMeshAddVertex().
	s32 span;

	// The batch being filled: which mesh vertex is in each slot, where its
	// vertices start, and the two commands reserved at its head for the colour
	// table and the vertex load, which cannot be written until the batch is
	// closed and its count is known.
	s32 slotof[XBLAMESH_BATCH];
	s32 numslots;
	s32 batchvtx;
	s32 batchgfx;
	s32 batchopen;

	// And every batch closed so far. The two commands hold addresses inside
	// the vertex and colour arrays, and those arrays are still growing - a
	// realloc after the command was written would leave it pointing into freed
	// memory, which draws as one enormous triangle across the screen. So the
	// batches are remembered by index and the addresses are filled in at the
	// end, when nothing can move any more.
	struct xblameshbatch {
		s32 gfx;
		s32 vtx;
		s32 count;
	} *batches;
	s32 numbatches, capbatches;
};

// What the colour table is put back to when a mesh has finished drawing.
static const Col xblaMeshWhite[64] = {
	[0 ... 63] = { .r = 0xff, .g = 0xff, .b = 0xff, .a = 0xff },
};

static s32 xblaMeshRoomForGfx(struct xblameshbuilder *b, s32 want)
{
	Gfx *grown;

	if (b->numgfx + want <= b->capgfx) {
		return 1;
	}

	b->capgfx = b->capgfx ? b->capgfx * 2 : 256;

	while (b->numgfx + want > b->capgfx) {
		b->capgfx *= 2;
	}

	grown = realloc(b->gdl, (size_t)b->capgfx * sizeof(Gfx));

	if (!grown) {
		return 0;
	}

	b->gdl = grown;

	return 1;
}

static s32 xblaMeshRoomForVtx(struct xblameshbuilder *b, s32 want)
{
	Vtx *grownv;
	Col *grownc;

	if (b->numvtx + want <= b->capvtx) {
		return 1;
	}

	b->capvtx = b->capvtx ? b->capvtx * 2 : 256;

	while (b->numvtx + want > b->capvtx) {
		b->capvtx *= 2;
	}

	grownv = realloc(b->vertices, (size_t)b->capvtx * sizeof(Vtx));

	if (!grownv) {
		return 0;
	}

	b->vertices = grownv;

	grownc = realloc(b->colours, (size_t)b->capvtx * sizeof(Col));

	if (!grownc) {
		return 0;
	}

	b->colours = grownc;

	if (!b->skinned) {
		f32 *g = realloc(b->grad, (size_t)b->capvtx * 4 * sizeof(f32));

		if (!g) {
			return 0;
		}

		b->grad = g;
		g = realloc(b->gradscore, (size_t)b->capvtx * 2 * sizeof(f32));

		if (!g) {
			return 0;
		}

		b->gradscore = g;
	}

	if (b->skinned) {
		f32 *pos = realloc(b->bindpos, (size_t)b->capvtx * 3 * sizeof(f32));
		f32 *wt;
		u8 *bn;

		if (!pos) {
			return 0;
		}

		b->bindpos = pos;
		wt = realloc(b->weights, (size_t)b->capvtx * 3 * sizeof(f32));

		if (!wt) {
			return 0;
		}

		b->weights = wt;
		bn = realloc(b->bones, (size_t)b->capvtx * 4);

		if (!bn) {
			return 0;
		}

		b->bones = bn;
	}

	return 1;
}

static s32 xblaMeshFindSlot(const struct xblameshbuilder *b, u32 index)
{
	for (s32 i = 0; i < b->numslots; i++) {
		if (b->slotof[i] == (s32)index) {
			return i;
		}
	}

	return -1;
}

/**
 * Loads one mesh vertex into the batch.
 *
 * A Perfect Dark vertex names its colour by a byte offset into a table rather
 * than carrying one, so the table is built alongside and a vertex points at
 * its own entry. That is also why the table is per batch: the offset is a
 * byte, so one table can hold 64 colours, and the release gives every vertex
 * its own. A batch is 25 vertices, so it always fits and nothing has to be
 * quantised away.
 */
static s32 xblaMeshAddVertex(struct xblameshbuilder *b, const u8 *file,
		const struct xblameshhdr *h, u32 stride, u32 index)
{
	const u8 *v = file + h->vertexoffset + index * stride;
	Vtx *vtx;
	Col *col;
	u32 colour;

	if (!xblaMeshRoomForVtx(b, 1)) {
		return -1;
	}

	vtx = &b->vertices[b->numvtx];
	col = &b->colours[b->numvtx];

	if (b->grad) {
		for (s32 i = 0; i < 4; i++) {
			b->grad[b->numvtx * 4 + i] = 0.0f;
		}

		b->gradscore[b->numvtx * 2] = -1.0f;
		b->gradscore[b->numvtx * 2 + 1] = -1.0f;
	}

	// position, then the UV pair, then a unit normal, then the colour. The
	// position is in the mesh's own units, which the header's scale turns into
	// the model file's - the identity for everything unskinned.
	vtx->x = xblaMeshRound(xblaMeshBEF32(v) * b->scale);
	vtx->y = xblaMeshRound(xblaMeshBEF32(v + 4) * b->scale);
	vtx->z = xblaMeshRound(xblaMeshBEF32(v + 8) * b->scale);
	vtx->flags = 0;
	vtx->colour = (u8)(b->numslots * 4);

	// The UVs, on to the stand-in tile the texture is bound through. A Perfect
	// Dark vertex measures s and t in texels of the tile as 10.5 fixed point
	// and the renderer normalises them by the tile, so a UV of one is one
	// tile's width whatever size the picture that lands on it turns out to be
	// - which is the same arrangement a texture pack's larger image draws
	// under. XBLATEX_TILE_SCALE is what a UV of one comes to; a coordinate
	// past what a Vtx holds is clamped rather than wrapped round, since a
	// wrapped one would draw a stripe of the wrong part of the picture.
	//
	// **v is turned over on the way in.** The two halves of this meet here and
	// they count rows from opposite ends: the picture is uploaded in the row
	// order x360DecodeTexture() produced, which is Perfect Dark's own order
	// and is why a texture pack needs no flip either, while a mesh's v is
	// Direct3D's and is measured from the top of the picture as it was drawn.
	// Left alone, every mesh in the release draws its texture mirrored top to
	// bottom - which reads as art that is merely wrong rather than as anything
	// upside down, since a body's own pieces move about: the CI's lab tech
	// wears her sleeves across her chest and her waistband round her hips.
	vtx->s = xblaMeshRound(xblaMeshBEF32(v + 12) * XBLATEX_TILE_SCALE);
	vtx->t = xblaMeshRound((1.0f - xblaMeshBEF32(v + 16)) * XBLATEX_TILE_SCALE);

	// ARGB. The alpha is real: the roof fan's column of light is written
	// with 0x7d at the fan and 0 at the top, and that fade is the whole of
	// what makes it a beam rather than a slab - see xblaMeshDrawFades().
	//
	// Kept only in the fading span. Everywhere else it goes in as 255, because
	// the combiner the game lights a model with reads the vertex alpha - mode 7
	// is (texel - env) * shade alpha + env - and the three opaque models with
	// a few stray zeros on a vertex would draw those vertices in the
	// environment tint, a dark red, where the release means nothing by them.
	colour = xblaMeshBE32(v + 32);
	col->r = (u8)(colour >> 16);
	col->g = (u8)(colour >> 8);
	col->b = (u8)colour;
	col->a = b->span == XBLAMESH_SPAN_FADE ? (u8)(colour >> 24) : 0xff;

	if (b->skinned) {
		// Two weights and a packed {bone0, bone1, bone2, count}. The third
		// weight is what is left of one: the two stored ones sum to 1.0 on 93%
		// of the release's skinned vertices and to as little as 0.5 on the
		// rest, so the remainder belongs to the third bone and dropping it
		// pulls those vertices towards the origin.
		//
		// The byte that reads like a count is not one. It runs 1 to 6 against
		// three bones, it is the same value for every vertex of a draw, and
		// its 2s carry three real influences as often as its 3s do - 4.4% of
		// them have bone0 and bone1 the same where 95% repeat bone1 in bone2,
		// which is how a vertex with fewer than three bones is written. So all
		// three are always applied and the repeats collapse themselves.
		f32 *pos = &b->bindpos[b->numvtx * 3];
		f32 *wt = &b->weights[b->numvtx * 3];
		u8 *bn = &b->bones[b->numvtx * 4];
		const u32 packed = xblaMeshBE32(v + 44);

		pos[0] = xblaMeshBEF32(v) * b->scale;
		pos[1] = xblaMeshBEF32(v + 4) * b->scale;
		pos[2] = xblaMeshBEF32(v + 8) * b->scale;

		wt[0] = xblaMeshBEF32(v + 36);
		wt[1] = xblaMeshBEF32(v + 40);
		wt[2] = 1.0f - wt[0] - wt[1];

		if (wt[2] < 0.0f) {
			wt[2] = 0.0f;
		}

		bn[0] = (u8)(packed >> 24);
		bn[1] = (u8)(packed >> 16);
		bn[2] = (u8)(packed >> 8);
		bn[3] = (u8)(packed & 0xff);
	}

	b->slotof[b->numslots] = (s32)index;
	b->numvtx++;

	return b->numslots++;
}

/** Notes the open batch for writing later, and forgets it. */
static s32 xblaMeshCloseBatch(struct xblameshbuilder *b)
{
	struct xblameshbatch *grown;

	if (!b->batchopen) {
		return 1;
	}

	b->batchopen = 0;

	if (b->numslots == 0) {
		// Nothing went into it. The two commands reserved at its head will
		// never be written now, so they are taken back rather than left in the
		// list as whatever the allocation happened to hold. A batch that did
		// take a vertex also took a triangle, so this cannot swallow one.
		b->numgfx = b->batchgfx;

		return 1;
	}

	if (b->numbatches >= b->capbatches) {
		b->capbatches = b->capbatches ? b->capbatches * 2 : 64;
		grown = realloc(b->batches, (size_t)b->capbatches * sizeof(*b->batches));

		if (!grown) {
			return 0;
		}

		b->batches = grown;
	}

	b->batches[b->numbatches].gfx = b->batchgfx;
	b->batches[b->numbatches].vtx = b->batchvtx;
	b->batches[b->numbatches].count = b->numslots;
	b->numbatches++;

	b->numslots = 0;

	return 1;
}

/** The addresses, once the arrays have stopped moving. */
static void xblaMeshWriteBatches(struct xblameshbuilder *b)
{
	for (s32 i = 0; i < b->numbatches; i++) {
		const struct xblameshbatch *batch = &b->batches[i];
		Gfx *g = &b->gdl[batch->gfx];

		// G_COL carries a byte length, which is what the renderer divides by
		// four to get the count.
		g->words.w0 = ((u32)G_COL << 24) | (u32)(batch->count * 4);
		g->words.w1 = (uintptr_t)&b->colours[batch->vtx];

		// The vertices are named by a segment rather than by address, so the
		// same list can be pointed at a posed copy of them - see the drawing
		// below. Segment 4 is the game's own for a model's vertices, and it is
		// rebound before this list is branched to either way.
		gSPVertex(&b->gdl[batch->gfx + 1],
				SEGADDR(XBLAMESH_VTXSEG | (uintptr_t)(batch->vtx * sizeof(Vtx))),
				batch->count, 0);
	}
}

/** Reserves the head of a new batch. */
static s32 xblaMeshOpenBatch(struct xblameshbuilder *b)
{
	if (!xblaMeshRoomForGfx(b, 2)) {
		return 0;
	}

	b->batchgfx = b->numgfx;
	b->batchvtx = b->numvtx;
	b->numgfx += 2;
	b->numslots = 0;
	b->batchopen = 1;

	return 1;
}

/**
 * The state one draw's material asks for, written into the list.
 *
 * The material word is a Textures.raw record in its low 13 bits and an alpha
 * flag in bit 15 - the flag is exactly the records whose format carries alpha,
 * which is what says it is a flag and not part of the number. The two bytes
 * above that are not understood and are not read here.
 *
 * A record that will not bind - no package, or a number past the table - draws
 * shaded and untextured, which is what the whole mesh looked like before any
 * of this. So a texture that cannot be found costs that one material rather
 * than the mesh.
 *
 * What a material does *not* write is the combiner, the cycle type and the
 * render mode. Those belong to the node, because they are how a Perfect Dark
 * model is lit: there is no G_LIGHTING in it anywhere. modelRenderNodeDl()
 * writes, round each of its own lists, a two-cycle combiner against the
 * environment colour and a G_RM_FOG_PRIM_A blend towards the fog colour, and
 * the fog colour is the prop's shade colour - the room's brightness and the
 * floor's colour, as propCalculateShadeColour() worked them out. A list that
 * wrote a one-cycle texture-times-shade in their place, as this one did, drew
 * every mesh at full brightness in the darkest room, which was the "the XBLA
 * models are not affected by lights, they stay bright" report.
 * xblaMeshRenderNode() writes the same state the game writes for the node,
 * and the list leaves it alone.
 *
 * The fading span is the one exception and carries its own combiner: a beam
 * of light is not lit by the room, and the game's combiner reads the vertex
 * alpha as (texel - env) * alpha + env, which would turn the beam's zero into
 * the environment colour, opaque. G_CC_PASS2 in the second cycle keeps it
 * right under whichever cycle type the node left behind.
 *
 * Tile 1 is declared as a copy of tile 0, as texWriteTileLods() does for a
 * texture with one level: the props' combiner is G_CC_TRILERP, and the lod
 * fraction this port feeds it runs 0.7 to 1.0, so TEXEL1 is most of what a
 * prop draws with. Left undeclared it is whatever the last texture the game
 * loaded left there.
 */
// A white RGBA16 tile, for a record that will not bind. The node's combiner
// reads a texel whatever the material says, so a draw with no picture has to
// be given a white one to be shade times the room's light like the rest.
static u16 xblaMeshWhiteTile[XBLATEX_TILE * XBLATEX_TILE] __attribute__((aligned(64)));

static s32 xblaMeshSetMaterial(struct xblameshbuilder *b, u32 material, s32 span)
{
	const u32 record = material & 0x1fff;
	const s32 alpha = (material >> 15) & 1;
	// Always bound, whether or not the art is switched on: what a stand-in
	// holds is white, so a material with the pictures turned off draws the
	// same flat solid a list built without a texture would. That is what lets
	// Mod.XblaMeshTextures be a live toggle rather than a rebuild - see
	// xblaTexSetEnabled().
	const void *tile = xblaTexBind(record);
	Gfx *gdl;

	if (!xblaMeshRoomForGfx(b, 13)) {
		return 0;
	}

	gdl = &b->gdl[b->numgfx];

	gDPPipeSync(gdl++);

	if (span == XBLAMESH_SPAN_FADE) {
		gDPSetCombineMode(gdl++, G_CC_MODULATERGBA, G_CC_PASS2);
	}

	if (!tile) {
		if (xblaMeshWhiteTile[0] != 0xffff) {
			memset(xblaMeshWhiteTile, 0xff, sizeof(xblaMeshWhiteTile));
		}

		tile = xblaMeshWhiteTile;
	}

	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);

	// The stand-in tile. Its texels are never read - the renderer swaps the
	// real picture in against this address, see xblatex.h - but the tile it
	// declares is real, and is what every texture coordinate is measured
	// against.
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

	b->numgfx = (s32)(gdl - b->gdl);

	if (xblaMeshVerbose) {
		sysLogPrintf(LOG_NOTE, "xblamesh:   material %08x -> record %u%s%s%s",
				material, record, alpha ? " alpha" : "",
				span == XBLAMESH_SPAN_FADE ? " (fades)" : "",
				tile == xblaMeshWhiteTile ? " (no texture; white)" : "");
	}

	return 1;
}

/**
 * Builds one mesh's display list.
 *
 * The batch split depends on how the triangles happen to share vertices, so it
 * is decided here as they are walked and the arrays grow behind it. A triangle
 * that would not fit closes the batch before any of it is loaded, which is
 * what keeps a vertex from being stranded in the batch it is not drawn in.
 */
/**
 * One group's worth of the mesh, as a display list of its own.
 *
 * A group is one part of the model - the same order and the same count, in all
 * 542 (model, mesh) pairs the release has, with the parts numbered 0..n-1 and
 * no gaps - so a list per group is a list per part, and the node that carries
 * part p draws group p. That is what makes a piece the game hides stay hidden:
 * a head's earpiece is its own part under a toggle, and one list for the whole
 * mesh drew it whatever the toggle said.
 *
 * Each list stands alone: it sets the geometry mode it wants at the top and
 * puts the state back at the bottom, because any one of them can be entered
 * without the others having run.
 *
 * Built twice per group, once for the draws whose material carries alpha and
 * once for the rest, so that the two can go in different passes. The split is
 * by material and the order within each half is the file's own.
 */
/**
 * The three spans a group's draws are sorted into.
 *
 * XBLAMESH_SPAN_FADE is the release's own volumetric light: geometry whose
 * vertex alpha runs below 255. The roof fan on dataDyne's helipad is the
 * type - a 9 vertex disc in the game's model, and in the release a column
 * of light a thousand units tall, its vertices 0x7d alpha at the fan and 0 at
 * the top, textured with a soft beam whose alpha never reaches 179. Drawn as
 * a cutout with that alpha thrown away it was a solid lavender sheet from the
 * roof to the sky, which is what the "fans have light pouring out but it
 * looks like a sheet" report was. A draw like that wants blending and no
 * depth write, in the translucent pass, whatever the node it hangs off says
 * about itself - the fan's node has no translucent list of its own for the
 * span to ride on.
 *
 * The same span takes a draw whose fade is in its texture rather than its
 * vertices: a material whose picture has next to no opaque texel (the comhub's
 * screen glow, record 4134, never reaches 108; a tinted pane at a flat 104; a
 * green glow at 138). Those were cutouts, and a cutout of a soft picture is a
 * solid wherever the alpha clears the threshold - a white half-disc on every
 * screen of the comhub. xblaTexRecordIsSoft() is the test.
 */
// (The XBLAMESH_SPAN_* values are defined at the top of the file, since the
// vertex loader and the material setup need them first.)

// Below this a vertex alpha is a fade and not a rounding. Counted over the
// release: 26 draws in 18 meshes carry any alpha under 255, and 13 of those
// meshes mean it - the two fans, the five hovercars' lights, four glass panes
// at a flat 127 or 153 and a set of lamps running 0 to 232 - while a
// speaker's 254 on every vertex, a 251 on one vertex of 168 and three
// vertices of a 794 vertex body are texture cutouts that would lose their
// depth write for nothing. The material's own alpha flag is required as
// well: a fade on a material with no alpha channel is a few stray zeros on
// three opaque models, and those draw as they always did.
#define XBLAMESH_FADE_ALPHA 0xf0

/** Whether any vertex of a draw carries an alpha that reads as a fade. */
static s32 xblaMeshDrawFades(const u8 *file, u32 len, const struct xblameshhdr *h,
		u32 stride, u32 firsttri, u32 drawtris)
{
	for (u32 t = 0; t < drawtris; t++) {
		const u8 *idx = file + h->indexoffset + (firsttri + t) * 6;

		for (s32 i = 0; i < 3; i++) {
			const u32 v = xblaMeshBE16(idx + i * 2);

			if (v < h->numvertices && file[h->vertexoffset + v * stride + 32] < XBLAMESH_FADE_ALPHA) {
				return 1;
			}
		}
	}

	return 0;
}

static s32 xblaMeshDrawSpan(const u8 *file, u32 len, const struct xblameshhdr *h,
		u32 stride, u32 d)
{
	const u8 *draw = file + h->drawoffset + d * XBLAMESH_ENTRY;
	const u32 firsttri = xblaMeshBE32(draw);
	const u32 drawtris = xblaMeshBE32(draw + 4);
	const u32 material = xblaMeshBE32(draw + 8);
	const u32 numtris = (len - h->indexoffset) / 6;

	if (!((material >> 15) & 1)) {
		return XBLAMESH_SPAN_SOLID;
	}

	if (firsttri <= numtris && drawtris <= numtris - firsttri &&
			xblaMeshDrawFades(file, len, h, stride, firsttri, drawtris)) {
		return XBLAMESH_SPAN_FADE;
	}

	// The fade can be in the picture instead of the vertices: a material
	// whose texture has no opaque texel to speak of is a glow or a tinted
	// pane, and a cutout has no edge to cut it at - it draws a solid wherever
	// the alpha clears the threshold, which was the white half-disc on the
	// comhub's screens. Blended, then, like the beams; the vertex alpha is
	// 255 and drops out of the product. Answered from the package once per
	// record and remembered, so this costs a decode the first time only.
	if (xblaTexRecordIsSoft(material & 0x1fff)) {
		return XBLAMESH_SPAN_FADE;
	}

	return XBLAMESH_SPAN_ALPHA;
}

/**
 * What one triangle says about how its texture runs along x and along y, kept
 * at each of its three emitted vertices.
 *
 * This is for the door trim (xblaMeshNodeTrim()). The game trims a door by
 * moving a vertex on to the trim plane and sliding its texture coordinate
 * along the edge to the vertex next to it, so the picture stays put and the
 * door looks cut rather than squashed. Its models are quads with an edge
 * along the slide, so "the vertex next to it" is a neighbour on the same
 * row; the release's meshes are triangles at any angle, so the equivalent is
 * the texture's gradient along the axis within the triangle's own plane -
 * the same thing for a face that runs along the axis, which is every face
 * of a door that the trim can cross. The gradient is the least-squares
 * answer to "which in-plane direction is the axis", so a face at right
 * angles to the axis - the door's end - gets a gradient of nothing and keeps
 * its coordinates, exactly as the game's rule leaves those alone.
 *
 * A vertex is shared between the triangles of its batch, so it keeps the
 * gradient from the triangle whose plane holds the axis best: the score is
 * the square of how much of the axis lies in the plane, one for a face along
 * it and nothing for a face across it.
 */
static void xblaMeshNoteTriangle(struct xblameshbuilder *b, s32 i0, s32 i1, s32 i2)
{
	const Vtx *p0 = &b->vertices[i0];
	const Vtx *p1 = &b->vertices[i1];
	const Vtx *p2 = &b->vertices[i2];
	const s32 idx[3] = { i0, i1, i2 };
	f32 e1[3], e2[3], n[3];
	f32 du1, dv1, du2, dv2;
	f32 a, bb, c, det, nn;

	for (s32 j = 0; j < 3; j++) {
		e1[j] = (f32)(p1->v[j] - p0->v[j]);
		e2[j] = (f32)(p2->v[j] - p0->v[j]);
	}

	du1 = (f32)(p1->s - p0->s);
	dv1 = (f32)(p1->t - p0->t);
	du2 = (f32)(p2->s - p0->s);
	dv2 = (f32)(p2->t - p0->t);

	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];

	a = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
	bb = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
	c = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
	det = a * c - bb * bb;

	// A sliver or a point has no plane to speak of.
	if (nn <= 0.0f || det <= 0.0f) {
		return;
	}

	for (s32 axis = 0; axis < 2; axis++) {
		// The axis as p e1 + q e2, as near as the plane allows.
		const f32 r1 = e1[axis];
		const f32 r2 = e2[axis];
		const f32 p = (c * r1 - bb * r2) / det;
		const f32 q = (a * r2 - bb * r1) / det;
		const f32 ds = p * du1 + q * du2;
		const f32 dt = p * dv1 + q * dv2;
		const f32 score = 1.0f - n[axis] * n[axis] / nn;

		for (s32 i = 0; i < 3; i++) {
			f32 *g = &b->grad[idx[i] * 4 + axis * 2];
			f32 *best = &b->gradscore[idx[i] * 2 + axis];

			if (score > *best) {
				*best = score;
				g[0] = ds;
				g[1] = dt;
			}
		}
	}
}

static s32 xblaMeshBuildGroup(struct xblameshbuilder *b, const u8 *file, u32 len,
		const struct xblameshhdr *h, u32 stride, u32 firstdraw, u32 numdraws,
		s32 wantspan)
{
	const u32 numtris = (len - h->indexoffset) / 6;

	// The material the list is currently set up for. The top two bytes of a
	// material word are not understood, so this compares the whole word rather
	// than the part it reads - two draws whose materials differ only up there
	// get a redundant setup, which is cheaper than being wrong about it.
	u32 lastmaterial = 0;

	// Whether anything has been written yet, which is what says the material
	// has to be set - not the first draw of the group, since the first draws
	// of it may all belong to the other span.
	s32 emitted = 0;

	b->span = wantspan;

	// How the mesh is lit, which no draw changes: its own vertex colours,
	// both faces, no lighting and no generated coordinates.
	//
	// Perfect Dark's own lists only set what they change and rely on the rest
	// persisting, so what this writes leaks into whatever draws next until
	// that sets its own. Every node that draws its own geometry goes through a
	// texture command that writes the combiner and the tile, so the window is
	// between this mesh and the next node - but the end of the list puts the
	// combiner, the render mode, the texture switch and the colour table back
	// anyway, since a textured combiner left behind is a worse thing to leak
	// than a shade-only one.
	if (!xblaMeshRoomForGfx(b, 2)) {
		return 0;
	}

	gSPClearGeometryMode(&b->gdl[b->numgfx], G_LIGHTING | G_CULL_BOTH |
			G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
	b->numgfx++;
	gSPSetGeometryMode(&b->gdl[b->numgfx], G_SHADE | G_SHADING_SMOOTH);
	b->numgfx++;

	for (u32 d = firstdraw; d < firstdraw + numdraws; d++) {
		const u8 *draw = file + h->drawoffset + d * XBLAMESH_ENTRY;
		const u32 firsttri = xblaMeshBE32(draw);
		const u32 drawtris = xblaMeshBE32(draw + 4);
		const u32 material = xblaMeshBE32(draw + 8);

		if (firsttri > numtris || drawtris > numtris - firsttri) {
			return 0;
		}

		if (xblaMeshDrawSpan(file, len, h, stride, d) != wantspan) {
			continue;
		}

		// A draw is one material's worth of triangles, and consecutive draws
		// share one more often than not - a character's head and hands are the
		// same skin. The batch has to close first: a vertex load and the
		// triangles that index it belong to the state they were written under.
		if (!emitted || material != lastmaterial) {
			if (!xblaMeshCloseBatch(b) ||
					!xblaMeshSetMaterial(b, material, wantspan) ||
					!xblaMeshOpenBatch(b)) {
				return 0;
			}

			lastmaterial = material;
			emitted = 1;
		}

		for (u32 t = 0; t < drawtris; t++) {
			const u8 *idx = file + h->indexoffset + (firsttri + t) * 6;
			u32 mesh[3];
			s32 slot[3];
			s32 fresh = 0;

			mesh[0] = xblaMeshBE16(idx);
			mesh[1] = xblaMeshBE16(idx + 2);
			mesh[2] = xblaMeshBE16(idx + 4);

			if (mesh[0] >= h->numvertices || mesh[1] >= h->numvertices ||
					mesh[2] >= h->numvertices) {
				continue;
			}

			for (s32 i = 0; i < 3; i++) {
				if (xblaMeshFindSlot(b, mesh[i]) >= 0) {
					continue;
				}

				if ((i > 0 && mesh[0] == mesh[i]) || (i > 1 && mesh[1] == mesh[i])) {
					continue;
				}

				fresh++;
			}

			// Three vertices always fit in an empty batch, so this only ever
			// has to happen once per triangle.
			if (b->numslots + fresh > XBLAMESH_BATCH) {
				if (!xblaMeshCloseBatch(b) || !xblaMeshOpenBatch(b)) {
					return 0;
				}
			}

			for (s32 i = 0; i < 3; i++) {
				slot[i] = xblaMeshFindSlot(b, mesh[i]);

				if (slot[i] < 0) {
					slot[i] = xblaMeshAddVertex(b, file, h, stride, mesh[i]);
				}

				if (slot[i] < 0) {
					return 0;
				}
			}

			if (!xblaMeshRoomForGfx(b, 1)) {
				return 0;
			}

			gSP1Triangle(&b->gdl[b->numgfx], slot[0], slot[1], slot[2], 0);
			b->numgfx++;
			b->numtris++;

			if (b->grad) {
				xblaMeshNoteTriangle(b, b->batchvtx + slot[0],
						b->batchvtx + slot[1], b->batchvtx + slot[2]);
			}
		}
	}

	if (!xblaMeshCloseBatch(b)) {
		return 0;
	}

	if (!xblaMeshRoomForGfx(b, 6)) {
		return 0;
	}

	// Put the texture switch back to what an untextured list would have left,
	// so that what leaks past the end of this is the same whether the mesh
	// drew with textures or without. The next node sets its own before it
	// draws, so this only has to be harmless rather than right. The combiner
	// and the render mode are not touched: they are the node's, written
	// before this list by xblaMeshRenderNode(), and the cutout list drawn
	// straight after this one has to find them still there.
	gDPPipeSync(&b->gdl[b->numgfx]);
	b->numgfx++;
	gSPTexture(&b->gdl[b->numgfx], 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
	b->numgfx++;

	// Put the colour table back. There is no telling what it was - the
	// renderer keeps one pointer and nothing saves it - so it goes back to a
	// full table of white, which is the neutral value for a shade multiply and
	// the one wrong answer that cannot darken anything. Every node that draws
	// its own geometry sets its own table before using it, so the window this
	// covers is between this mesh and the next node.
	b->gdl[b->numgfx].words.w0 = ((u32)G_COL << 24) | (u32)(sizeof(xblaMeshWhite));
	b->gdl[b->numgfx].words.w1 = (uintptr_t)xblaMeshWhite;
	b->numgfx++;

	gSPEndDisplayList(&b->gdl[b->numgfx]);
	b->numgfx++;

	return 1;
}

/**
 * Every group's list, and one that calls all of them.
 *
 * The whole-mesh list is what a model draws when its parts and the mesh's
 * groups do not line up - which nothing in the release does, but a mod's model
 * or a half matched one could - and what a mesh with no group table at all is
 * built as. It is a call per group rather than a copy of them, so it costs a
 * command each and cannot fall out of step with what it calls.
 */
static s32 xblaMeshBuildLists(struct xblameshbuilder *b, const u8 *file, u32 len,
		const struct xblameshhdr *h, u32 stride)
{
	const u32 intable = (h->drawoffset - h->groupoffset) / XBLAMESH_ENTRY;

	// A mesh with no group table, or with more groups than a model can have
	// parts, is built as one group of everything - one file in the release has
	// no group table at all. The count that comes out of here is what a part
	// number is checked against, so it is always at least the one.
	const s32 numgroups = (intable >= 1 && intable <= XBLAMESH_MAXPARTS) ? (s32)intable : 1;

	s32 numxlu = 0;
	s32 numfade = 0;

	b->numgroups = numgroups;
	b->allxlu = -1;
	b->allfade = -1;

	for (s32 g = 0; g < numgroups; g++) {
		u32 firstdraw = 0;
		u32 numdraws = h->numdraws;
		s32 anyalpha = 0;
		s32 anyfade = 0;

		if ((u32)numgroups == intable) {
			const u8 *group = file + h->groupoffset + (u32)g * XBLAMESH_ENTRY;

			firstdraw = xblaMeshBE32(group);
			numdraws = xblaMeshBE32(group + 4);

			if (firstdraw > h->numdraws || numdraws > h->numdraws - firstdraw) {
				return 0;
			}
		}

		// Whether this group has anything for a second span at all. 124 of the
		// release's 556 meshes have an alpha material anywhere in them, so for
		// most groups this is the end of it and groupxlu stays -1.
		for (u32 d = firstdraw; d < firstdraw + numdraws; d++) {
			const s32 span = xblaMeshDrawSpan(file, len, h, stride, d);

			if (span == XBLAMESH_SPAN_ALPHA) {
				anyalpha = 1;
			} else if (span == XBLAMESH_SPAN_FADE) {
				anyfade = 1;
			}
		}

		b->groupgfx[g] = b->numgfx;

		if (!xblaMeshBuildGroup(b, file, len, h, stride, firstdraw, numdraws, XBLAMESH_SPAN_SOLID)) {
			return 0;
		}

		b->groupxlu[g] = -1;
		b->groupfade[g] = -1;

		if (anyalpha) {
			b->groupxlu[g] = b->numgfx;
			numxlu++;

			if (!xblaMeshBuildGroup(b, file, len, h, stride, firstdraw, numdraws, XBLAMESH_SPAN_ALPHA)) {
				return 0;
			}
		}

		if (anyfade) {
			b->groupfade[g] = b->numgfx;
			numfade++;

			if (!xblaMeshBuildGroup(b, file, len, h, stride, firstdraw, numdraws, XBLAMESH_SPAN_FADE)) {
				return 0;
			}
		}
	}

	// The lists that call them all. Their commands hold addresses inside the
	// array they are in, which is still growing, so they go in after the
	// batches do - by then nothing moves again.
	b->allgfx = b->numgfx;

	if (!xblaMeshRoomForGfx(b, numgroups + 1)) {
		return 0;
	}

	b->numgfx += numgroups + 1;

	if (numxlu) {
		b->allxlu = b->numgfx;

		if (!xblaMeshRoomForGfx(b, numxlu + 1)) {
			return 0;
		}

		b->numgfx += numxlu + 1;
	}

	if (numfade) {
		b->allfade = b->numgfx;

		if (!xblaMeshRoomForGfx(b, numfade + 1)) {
			return 0;
		}

		b->numgfx += numfade + 1;
	}

	xblaMeshWriteBatches(b);

	for (s32 g = 0; g < numgroups; g++) {
		gSPDisplayList(&b->gdl[b->allgfx + g], &b->gdl[b->groupgfx[g]]);
	}

	gSPEndDisplayList(&b->gdl[b->allgfx + numgroups]);

	if (numxlu) {
		s32 at = 0;

		for (s32 g = 0; g < numgroups; g++) {
			if (b->groupxlu[g] >= 0) {
				gSPDisplayList(&b->gdl[b->allxlu + at], &b->gdl[b->groupxlu[g]]);
				at++;
			}
		}

		gSPEndDisplayList(&b->gdl[b->allxlu + at]);
	}

	if (numfade) {
		s32 at = 0;

		for (s32 g = 0; g < numgroups; g++) {
			if (b->groupfade[g] >= 0) {
				gSPDisplayList(&b->gdl[b->allfade + at], &b->gdl[b->groupfade[g]]);
				at++;
			}
		}

		gSPEndDisplayList(&b->gdl[b->allfade + at]);
	}

	return 1;
}

/**
 * The palette, which is the inverse of every bind matrix, in the game's terms.
 *
 * A vertex has to come out of the bind pose before the game's pose for its
 * bone can be put on it, and the inverse bind is what the file already holds -
 * nothing here inverts anything. What it does do is change convention: the
 * mesh stores three rows of four floats with the translation in the last
 * column, to be applied on the left of a column vector, and an Mtxf is read by
 * mtx4TransformVec the other way round - three basis vectors in rows with the
 * translation in the fourth. So the 3x3 transposes and the column becomes the
 * row.
 */
static s32 xblaMeshReadBind(struct xblameshbuilt *m, const u8 *file,
		const struct xblameshhdr *h, f32 scale)
{
	if (h->nummatrices > XBLAMESH_MAXMTX) {
		return 0;
	}

	m->invbind = calloc(h->nummatrices, sizeof(Mtxf));

	if (!m->invbind) {
		return 0;
	}

	m->nummatrices = (s32)h->nummatrices;

	// A group's third word is a palette entry, and nothing reads it: what
	// poses a vertex is the bone bytes the vertex itself carries, and the
	// entry a group names is the one its part is weighted to most. It is worth
	// knowing when a group is being matched to a part; it is not a step in
	// drawing one.
	for (u32 i = 0; i < h->nummatrices; i++) {
		const u8 *src = file + XBLAMESH_HEADER + i * XBLAMESH_MATRIX;
		Mtxf *dst = &m->invbind[i];
		f32 row[3][4];

		for (s32 r = 0; r < 3; r++) {
			for (s32 c = 0; c < 4; c++) {
				row[r][c] = xblaMeshBEF32(src + (r * 4 + c) * 4);
			}
		}

		// Stored as three rows of four with the translation in the last
		// column, which is a matrix meant to be applied on the left of a
		// column vector. An Mtxf is read the other way round - three basis
		// vectors in rows, translation in the fourth - so the 3x3 transposes
		// and the column becomes the row.
		//
		// What is stored is the *inverse* bind, model space to bone space, and
		// not the bind itself: palette entry 0 of the evening dress mesh
		// translates by -146 in y where the mesh stands from 0 to 148, which
		// is a bone at +146 written the other way about. Inverting it here as
		// well - which is what "the matrices fold a character in half" was -
		// puts every vertex through the bone twice.
		for (s32 r = 0; r < 3; r++) {
			for (s32 c = 0; c < 3; c++) {
				dst->m[c][r] = row[r][c];
			}

			dst->m[r][3] = 0.0f;
		}

		// The last column is the translation of that same inverse bind and
		// goes in as it stands - the bone's *position* is what it is not.
		// Entry 0 of the evening dress mesh translates by -146 in y where the
		// mesh stands from 0 to 149, and the head is at +146: a point at the
		// head lands on the origin of the bone, which is what an inverse bind
		// is for. Rotating and negating it here to make a position out of it -
		// which is what this used to do - moves every bone with a rotation to
		// somewhere else entirely and every bone without one to twice its own
		// height away.
		//
		// In the game's units, since that is what the vertices were read in:
		// the rotation does not care, the translation does.
		for (s32 c = 0; c < 3; c++) {
			dst->m[3][c] = row[c][3] * scale;
		}

		dst->m[3][3] = 1.0f;
	}

	return 1;
}

static struct xblameshbuilt *xblaMeshBuild(s32 slot)
{
	struct xblameshbuilt *m = &built[slot];
	struct xblameshbuilder b;
	struct xblameshhdr h;
	u8 *file;
	u32 len;
	u32 stride;

	if (m->state) {
		return m->state > 0 ? m : NULL;
	}

	m->state = -1;

	file = xblaMeshReadSlot(slot, &len);

	if (!file) {
		return NULL;
	}

	if (!xblaMeshReadHeader(&h, file, len, &stride)) {
		free(file);
		return NULL;
	}

	memset(&b, 0, sizeof(b));
	b.skinned = stride == XBLAMESH_STRIDE_SKIN && h.nummatrices > 0;
	b.scale = xblaMeshScale(&h);
	m->scale = b.scale;

	if (!xblaMeshBuildLists(&b, file, len, &h, stride) || b.numtris == 0) {
		free(b.gdl);
		free(b.vertices);
		free(b.colours);
		free(b.grad);
		free(b.gradscore);
		free(b.batches);
		free(file);
		sysLogPrintf(LOG_ERROR, "xblamesh: slot %d did not build", slot);
		return NULL;
	}

	free(b.batches);
	free(b.gradscore);

	if (b.skinned && !xblaMeshReadBind(m, file, &h, b.scale)) {
		free(b.gdl);
		free(b.vertices);
		free(b.colours);
		free(b.grad);
		free(b.bindpos);
		free(b.weights);
		free(b.bones);
		free(file);
		return NULL;
	}

	free(file);

	m->gdl = b.gdl;
	m->vertices = b.vertices;
	m->colours = b.colours;
	m->grad = b.grad;
	m->numvertices = b.numvtx;
	m->numtris = b.numtris;
	m->bindpos = b.bindpos;
	m->weights = b.weights;
	m->bones = b.bones;
	m->allgfx = b.allgfx;
	m->allxlu = b.allxlu;
	m->allfade = b.allfade;
	m->numgroups = b.numgroups;
	m->state = 1;

	// The box the bind positions stand in. A posed vertex is a blend of the
	// same point put through one palette matrix or another, so every one of
	// them lands inside this box carried through those matrices - which is
	// what bounds the pose, and so how finely it can be written down.
	if (m->bindpos) {
		for (s32 j = 0; j < 3; j++) {
			m->bindlo[j] = m->bindhi[j] = m->bindpos[j];
		}

		for (s32 i = 1; i < b.numvtx; i++) {
			for (s32 j = 0; j < 3; j++) {
				const f32 v = m->bindpos[i * 3 + j];

				if (v < m->bindlo[j]) {
					m->bindlo[j] = v;
				}

				if (v > m->bindhi[j]) {
					m->bindhi[j] = v;
				}
			}
		}
	}

	for (s32 g = 0; g < XBLAMESH_MAXPARTS; g++) {
		m->groupgfx[g] = b.groupgfx[g];
		// The builder is zeroed, and zero is a real index, so a group it never
		// reached says -1 rather than "the list at the top of the array".
		m->groupxlu[g] = g < b.numgroups ? b.groupxlu[g] : -1;
		m->groupfade[g] = g < b.numgroups ? b.groupfade[g] : -1;
	}

	g_XblaMeshNumMeshes++;
	g_XblaMeshNumTris += (u32)b.numtris;

	// Everything this mesh is now holding, the skinning included - a skinned
	// mesh keeps a bind position, two weights and its bones for every vertex
	// it emitted, which is more than the vertices themselves come to.
	g_XblaMeshBytes += (u32)((size_t)b.numgfx * sizeof(Gfx) +
			(size_t)b.numvtx * (sizeof(Vtx) + sizeof(Col)) +
			(size_t)m->nummatrices * sizeof(Mtxf) +
			(m->bindpos ? (size_t)b.numvtx * (6 * sizeof(f32) + 3) : 0) +
			(m->grad ? (size_t)b.numvtx * 4 * sizeof(f32) : 0));

	if (xblaMeshVerbose) {
		s16 lo[3];
		s16 hi[3];

		for (s32 i = 0; i < 3; i++) {
			lo[i] = b.vertices[0].v[i];
			hi[i] = b.vertices[0].v[i];
		}

		for (s32 i = 1; i < b.numvtx; i++) {
			for (s32 j = 0; j < 3; j++) {
				if (b.vertices[i].v[j] < lo[j]) {
					lo[j] = b.vertices[i].v[j];
				}

				if (b.vertices[i].v[j] > hi[j]) {
					hi[j] = b.vertices[i].v[j];
				}
			}
		}

		sysLogPrintf(LOG_NOTE, "xblamesh: slot %d mesh [%d %d %d]..[%d %d %d]",
				slot, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
	}

	sysLogPrintf(LOG_NOTE, "xblamesh: slot %d built, %d tris, %d verts, %d cmds",
			slot, b.numtris, b.numvtx, b.numgfx);

	return m;
}

/* -------------------------------------------------------------------------
 * A frame's posed vertices
 * ------------------------------------------------------------------------- */

/**
 * Where a skinned mesh's posed vertices go for one frame.
 *
 * Not the game's vtx pool: that is sized for what an N64 drew and a single
 * character here is eleven thousand vertices, which would push a match's worth
 * of chrs straight through the end of it. This is its own arena, doubled the
 * way the game doubles its pools - the display list built this frame is run
 * while the next one is being built, so last frame's vertices have to survive
 * one more frame.
 *
 * **It grows by adding a chunk, never by growing one.** The obvious arena - one
 * block per side, `realloc`ed to fit - cannot be grown inside a frame at all,
 * because a `realloc` moves vertices that commands already written this frame
 * point at. So it grew between frames instead, to whatever the frame before it
 * asked for, and the frame that first wanted more than that drew the tail of
 * its meshes in their **bind pose**: for a head, whose bind vertices are in the
 * body's space around y 1400, a head hanging a body's height above the body for
 * one frame. That is every frame a character first comes into view, 12 of them
 * over 3000 frames of an eight-simulant match.
 *
 * A chunk list has neither problem. What has been handed out never moves, so a
 * chunk can be added in the middle of a frame, and a frame that wants more than
 * has ever been wanted gets it there and then rather than one frame late. Each
 * side keeps its own chunks and hands them back at the top of its next frame;
 * they are not freed, because the cap is what bounds this and what a match
 * actually holds is small: one chunk a side for eight simulants, six for
 * eighty.
 */
#define XBLAMESH_ARENA_MAX (48 * 1024 * 1024)
#define XBLAMESH_CHUNK (1024 * 1024) // the smallest chunk asked for

struct xblameshchunk {
	u8 *data;
	u32 size;
	u32 pos;
};

// One list per side of the double buffer. The descriptors move when the list
// grows; the chunks they name do not, which is the whole point.
static struct xblameshchunk *frameChunks[2];
static s32 frameNumChunks[2];
static s32 frameCurChunk[2];
static u32 frameBytes[2];  // held by that side's chunks, against the cap
static u32 frameWanted;    // this frame's demand, for the log
static s32 frameIndex;

// Which frame this is, for the posed copy a mesh keeps. It only has to differ
// from the frame before it, so wrapping is no more than one wasted pose.
static u32 frameCount;

// What this frame has done so far and what the last one did, for the F3
// trace dump: opaque lists emitted, poses written, poses the arena refused.
static u32 frameDraws, frameDrawsLast;
static u32 framePoses, framePosesLast;
static u32 framePoseFails, framePoseFailsLast;

static void xblaMeshReportOverlaps(void);

void xblaMeshFrameReset(void)
{
	// Unconditional, including with the meshes switched off. The frame counter
	// is what a mesh's posed copy is keyed on, and one that stops moving while
	// the switch is off is still the current frame when it comes back on - so
	// the first frame after would draw a pose left over from before, in an
	// arena whose high water mark had not been given back either.
	if (xblaMeshVerbose) {
		xblaMeshReportOverlaps();
	}

	frameIndex ^= 1;
	frameCount++;

	// This side's chunks are two frames old now: the list they were written
	// for has been run. Handed back where they are rather than freed.
	for (s32 i = 0; i < frameNumChunks[frameIndex]; i++) {
		frameChunks[frameIndex][i].pos = 0;
	}

	frameCurChunk[frameIndex] = 0;
	frameWanted = 0;

	frameDrawsLast = frameDraws;
	framePosesLast = framePoses;
	framePoseFailsLast = framePoseFails;
	frameDraws = framePoses = framePoseFails = 0;
}

/**
 * One more chunk on this side, big enough for what is being asked for.
 *
 * A single mesh's pose is one allocation, so a chunk has to be able to hold
 * whatever the largest of them comes to rather than a fixed size; the minimum
 * is what keeps a room of characters from taking a chunk each.
 */
static struct xblameshchunk *xblaMeshFrameAddChunk(u32 size)
{
	const u32 want = size > XBLAMESH_CHUNK ? size : XBLAMESH_CHUNK;
	struct xblameshchunk *grown;
	struct xblameshchunk *chunk;
	u8 *data;

	if (frameBytes[frameIndex] + want > XBLAMESH_ARENA_MAX) {
		return NULL;
	}

	grown = realloc(frameChunks[frameIndex],
			(size_t)(frameNumChunks[frameIndex] + 1) * sizeof(*grown));

	if (!grown) {
		return NULL;
	}

	frameChunks[frameIndex] = grown;
	data = malloc(want);

	if (!data) {
		return NULL;
	}

	chunk = &grown[frameNumChunks[frameIndex]];
	chunk->data = data;
	chunk->size = want;
	chunk->pos = 0;

	frameCurChunk[frameIndex] = frameNumChunks[frameIndex];
	frameNumChunks[frameIndex]++;
	frameBytes[frameIndex] += want;

	return chunk;
}

static void *xblaMeshFrameAlloc(u32 size)
{
	struct xblameshchunk *chunk = NULL;
	void *ptr;

	size = (size + 15) & ~15u;
	frameWanted += size;

	// The chunk being filled, or the next one along that this fits in. A chunk
	// passed over keeps whatever was left in it until this side comes round
	// again, which is what a bump allocator trades for never moving anything;
	// a chunk is large enough that the loss is noise.
	for (s32 cur = frameCurChunk[frameIndex]; cur < frameNumChunks[frameIndex]; cur++) {
		struct xblameshchunk *c = &frameChunks[frameIndex][cur];

		if (c->pos + size <= c->size) {
			frameCurChunk[frameIndex] = cur;
			chunk = c;
			break;
		}
	}

	if (!chunk) {
		chunk = xblaMeshFrameAddChunk(size);
	}

	if (!chunk) {
		return NULL;
	}

	ptr = chunk->data + chunk->pos;
	chunk->pos += size;

	return ptr;
}

static void xblaMeshArenaStats(u32 *kb, s32 *chunks)
{
	*kb = (frameBytes[0] + frameBytes[1] + 1023) / 1024;
	*chunks = frameNumChunks[0] + frameNumChunks[1];
}

/**
 * The inverse of one of the game's matrices.
 *
 * An Mtxf is read as three basis vectors in rows with the translation in the
 * fourth, so a point is v * R + t and the inverse is v * R-1 - t * R-1.
 */
static void xblaMeshInvert(const Mtxf *src, Mtxf *dst)
{
	f32 inv[3][3];
	f32 det;

	inv[0][0] = src->m[1][1] * src->m[2][2] - src->m[1][2] * src->m[2][1];
	inv[0][1] = src->m[0][2] * src->m[2][1] - src->m[0][1] * src->m[2][2];
	inv[0][2] = src->m[0][1] * src->m[1][2] - src->m[0][2] * src->m[1][1];
	inv[1][0] = src->m[1][2] * src->m[2][0] - src->m[1][0] * src->m[2][2];
	inv[1][1] = src->m[0][0] * src->m[2][2] - src->m[0][2] * src->m[2][0];
	inv[1][2] = src->m[0][2] * src->m[1][0] - src->m[0][0] * src->m[1][2];
	inv[2][0] = src->m[1][0] * src->m[2][1] - src->m[1][1] * src->m[2][0];
	inv[2][1] = src->m[0][1] * src->m[2][0] - src->m[0][0] * src->m[2][1];
	inv[2][2] = src->m[0][0] * src->m[1][1] - src->m[0][1] * src->m[1][0];

	det = src->m[0][0] * inv[0][0] + src->m[0][1] * inv[1][0] + src->m[0][2] * inv[2][0];

	if (det > -1e-12f && det < 1e-12f) {
		mtx4LoadIdentity(dst);
		return;
	}

	det = 1.0f / det;

	for (s32 row = 0; row < 3; row++) {
		for (s32 col = 0; col < 3; col++) {
			dst->m[row][col] = inv[row][col] * det;
		}

		dst->m[row][3] = 0.0f;
	}

	for (s32 col = 0; col < 3; col++) {
		dst->m[3][col] = -(src->m[3][0] * dst->m[0][col] +
				src->m[3][1] * dst->m[1][col] + src->m[3][2] * dst->m[2][col]);
	}

	dst->m[3][3] = 1.0f;
}

/** The matrix the game is posing one part of the model with, or NULL. */
static Mtxf *xblaMeshPartMtx(struct model *model, struct xblameshuse *use, s32 part)
{
	s32 index;

	if (!model || !model->matrices || !model->definition ||
			part < 0 || part >= use->numparts) {
		return NULL;
	}

	index = use->partmtx[part];

	if (index < 0 || index >= model->definition->nummatrices) {
		return NULL;
	}

	return &model->matrices[index];
}

/**
 * How many steps of a posed copy make one of the game's units.
 *
 * A Perfect Dark vertex holds an s16, and the game's own models are drawn in
 * whole units because that is what an N64 model file could say. **The
 * release's skinned meshes are not**: the header's scale is a tenth for nearly
 * every one of them, so a character's geometry is stated to a tenth of a unit
 * and rounding the pose to whole ones throws nine tenths of what 4J drew away.
 * A head is where that shows - the vertices across a nose are three or four
 * units apart, so half a unit of rounding is a tenth of the spacing, scattered
 * a different way on every vertex, and what should be a straight ridge comes
 * out bent. It is a hundredth of the spacing on a wall or a crate, which is
 * why nothing else looked wrong.
 *
 * A vertex cannot hold more than an s16, but the matrix it is drawn under can
 * be divided: write the pose in sixteenths and hand back the bone's matrix
 * with its three rows divided by sixteen, and the two cancel at the vertex
 * that reaches the screen. The vertices stay inside the s16 as long as the
 * mesh does, so how far the pose can be taken is what the pose *reaches*.
 *
 * The divided matrix has to reach the list as floats (G_MTX_FLOATS). The
 * game's rows carry the model's scale, a tenth, and a tenth of a sixteenth is
 * 0.006: in the N64's s15.16 that keeps two and a half decimal digits, and
 * against vertices written sixteen times larger the error is sixteen times
 * the game's own - a few pixels on a gun held at arm's length, snapping as
 * the pose turns. See xblaMeshPose().
 *
 * Which is bounded without posing anything. A posed vertex is a blend of one
 * bind position put through each of the palette's matrices, and a blend of
 * points inside a box carried through an affine matrix is inside that box's
 * image - so the eight corners of the bind box, through every palette entry,
 * bound every vertex this is about to write. Cheap: 24 entries at most, eight
 * corners each, once a frame per mesh.
 *
 * The cap is 16 because it is already far past what the mesh states - a
 * sixteenth of a unit is a millimetre and a half of a person - and because a
 * bound that is met exactly still has to leave the rounding somewhere to go.
 */
#define XBLAMESH_MAXFINE 16
#define XBLAMESH_FINEROOM 30000.0f

static s32 xblaMeshPoseFineness(const struct xblameshbuilt *m, const Mtxf *pal)
{
	f32 reach = 0.0f;
	s32 fine = 1;

	if (!m->bindpos) {
		return 1;
	}

	for (s32 i = 0; i < m->nummatrices; i++) {
		for (s32 corner = 0; corner < 8; corner++) {
			struct coord in;
			struct coord out;

			in.x = (corner & 1) ? m->bindhi[0] : m->bindlo[0];
			in.y = (corner & 2) ? m->bindhi[1] : m->bindlo[1];
			in.z = (corner & 4) ? m->bindhi[2] : m->bindlo[2];

			mtx4TransformVec((Mtxf *)&pal[i], &in, &out);

			for (s32 j = 0; j < 3; j++) {
				const f32 v = out.f[j] < 0.0f ? -out.f[j] : out.f[j];

				if (v > reach) {
					reach = v;
				}
			}
		}
	}

	while (fine < XBLAMESH_MAXFINE && reach * (fine * 2) <= XBLAMESH_FINEROOM) {
		fine *= 2;
	}

	return fine;
}

/**
 * Poses one mesh into a copy of its vertices, and hands back the copy.
 *
 * Palette entry i is posed by whatever the game has done to the node carrying
 * part i, so the transform for it is: out of the bind pose (invbind), into the
 * game's pose for that bone (its matrix), and then back out of the matrix the
 * list is going to be drawn under, which is the first part's. The last step is
 * what keeps the vertices small enough to be the s16 a Perfect Dark vertex
 * holds - they come out in the first part's own space rather than the view's.
 *
 * The copy is written **finer than the game's units**, and the matrix it is
 * drawn under is handed back divided by the same number. See
 * xblaMeshPoseFineness() for why, and for what picks the number.
 *
 * NULL when there is no room this frame, and the caller draws the bind pose.
 */
static Vtx *xblaMeshPose(struct xblameshbuilt *m, struct model *model, Mtxf *root,
		Mtxf **outmtx, s32 *outfine)
{
	Mtxf pal[XBLAMESH_MAXMTX];
	Mtxf invroot;
	Vtx *out;
	Mtxf *fmtx = NULL;
	s32 posable;
	s32 fine;

	*outmtx = NULL;
	*outfine = 1;

	if (m->nummatrices > XBLAMESH_MAXMTX || !model->definition) {
		return NULL;
	}

	out = xblaMeshFrameAlloc((u32)m->numvertices * sizeof(Vtx));

	if (!out) {
		return NULL;
	}

	xblaMeshInvert(root, &invroot);

	// Entry i of the mesh's palette is matrix i of the model, and that is the
	// whole of the mapping: not the part number, which only looked like it
	// because a small model has few of both.
	//
	// It is checked the way the mesh id was. Take every model that names a
	// mesh, and for each of its joints compare the offset the model file
	// states from its parent joint against the offset the palette implies for
	// the same two entries, turned into the parent's frame. Under this mapping
	// they agree; under the palette read one, two or three entries along they
	// do not, and the identity wins for 166 of the 170 models with a palette.
	// It is the same argument as the mesh id's, on the same kind of evidence.
	posable = m->nummatrices;

	if (posable > model->definition->nummatrices) {
		posable = model->definition->nummatrices;
	}

	for (s32 i = 0; i < m->nummatrices; i++) {
		Mtxf step;

		// An entry the model has no matrix for follows the first entry, so the
		// part of the mesh weighted to it stays rigidly attached to the bone
		// that does have one rather than being left behind in the mesh's own
		// space. Every head mesh has three entries against the one matrix a
		// head model file carries, and its neck is weighted to the second: a
		// head drawn on its own - not grafted onto a body, which is where the
		// other eighteen matrices come from - would otherwise trail its neck
		// back to where the body would have been.
		if (i >= posable) {
			if (posable > 0) {
				mtx4Copy(&pal[0], &pal[i]);
			} else {
				mtx4LoadIdentity(&pal[i]);
			}

			continue;
		}

		// Out of the bind pose, into the game's, and then out of the matrix
		// this list is drawn under - which is what keeps the result small
		// enough to be the s16 a Perfect Dark vertex holds.
		mtx4MultMtx4(&model->matrices[i], &m->invbind[i], &step);
		mtx4MultMtx4(&invroot, &step, &pal[i]);
	}

	if (xblaMeshVerbose && !m->posedlog) {
		m->posedlog = 1;
		sysLogPrintf(LOG_NOTE, "xblamesh:   %d palette entries, %d posed by the "
				"model's %d matrices, scale %g", m->nummatrices, posable,
				model->definition->nummatrices, m->scale);

		// Whether the two rigs are the same rig, which is the thing to look at
		// when a pose comes out wrong. A bone's distance from the first one is
		// the same in both if they are, once the game's is taken out of the
		// matrix's own scale - so every ratio here should be the same number,
		// and it should be near 1. A bone whose ratio is on its own is one the
		// release moved; all of them being off by a constant is the header's
		// scale being read wrong.
		for (s32 i = 0; i < posable && i < 12; i++) {
			const Mtxf *g = &model->matrices[i];
			const f32 *b = &m->invbind[i].m[3][0];
			const f32 *b0 = &m->invbind[0].m[3][0];
			f32 mesh = 0.0f;
			f32 game = 0.0f;
			f32 mscale;

			// The bind's translation is the bone's position rotated into the
			// bone's own frame, so a difference of two is only a distance
			// after each has come back out of its rotation. Distances are
			// what this needs, so it takes them one at a time.
			for (s32 j = 0; j < 3; j++) {
				const f32 mb = -(b[0] * m->invbind[i].m[j][0] +
						b[1] * m->invbind[i].m[j][1] + b[2] * m->invbind[i].m[j][2]);
				const f32 mb0 = -(b0[0] * m->invbind[0].m[j][0] +
						b0[1] * m->invbind[0].m[j][1] + b0[2] * m->invbind[0].m[j][2]);
				const f32 gd = g->m[3][j] - model->matrices[0].m[3][j];

				mesh += (mb - mb0) * (mb - mb0);
				game += gd * gd;
			}

			mesh = sqrtf(mesh);
			game = sqrtf(game);
			mscale = sqrtf(g->m[0][0] * g->m[0][0] + g->m[0][1] * g->m[0][1] +
					g->m[0][2] * g->m[0][2]);

			sysLogPrintf(LOG_NOTE, "xblamesh:     pal %2d  from pal 0: mesh "
					"%8.2f  game %8.2f  ratio %6.3f", i, mesh,
					mscale > 1e-6f ? game / mscale : game,
					mesh > 1e-3f && mscale > 1e-6f ? game / mscale / mesh : 0.0f);
		}
	}

	// How finely this pose can be written down, and the matrix that takes the
	// fineness back out. A mesh whose posed vertices will not fit any finer
	// than the game's own units gets no copy and no division, and is written
	// exactly as it was before.
	fine = xblaMeshPoseFineness(m, pal);

	if (fine > 1) {
		fmtx = xblaMeshFrameAlloc(sizeof(Mtxf));

		if (fmtx) {
			const f32 inv = 1.0f / fine;

			for (s32 r = 0; r < 3; r++) {
				for (s32 c = 0; c < 4; c++) {
					fmtx->m[r][c] = root->m[r][c] * inv;
				}
			}

			// The translation is not divided: it is where the whole thing
			// stands, and the vertices multiplied by `fine` are what the
			// divided rows are there to bring back to the game's units.
			for (s32 c = 0; c < 4; c++) {
				fmtx->m[3][c] = root->m[3][c];
			}

			// Not converted to the N64's s15.16, which is what the list reads
			// a matrix as unless told otherwise: this copy is handed over
			// with G_MTX_FLOATS and read as the floats it is. It was
			// converted once (mtxF2L, in place, the way the game converts a
			// model's own matrices after listing them), and that is what
			// shook every posed mesh. The game's rows carry the model's
			// scale, a tenth, so divided by sixteen they are 0.006, and the
			// s16 fraction's 1/65536 is a quarter of a percent of that -
			// against vertices written sixteen times larger, up to a tenth
			// of a unit of error on a gun eighteen units from the eye, three
			// or four pixels, landing differently each time a row crossed a
			// step of the fraction. Slow motion showed it as a shake and a
			// fast one as a blur. A float's fraction is a thousand times
			// finer, and the error goes with it.
		} else {
			// No room for the matrix. The vertices have not been written yet,
			// so this simply goes back to what it did before rather than
			// drawing a mesh sixteen times its size.
			fine = 1;
		}
	}

	*outmtx = fmtx;
	*outfine = fine;

	for (s32 i = 0; i < m->numvertices; i++) {
		const f32 *pos = &m->bindpos[i * 3];
		const f32 *weight = &m->weights[i * 3];
		const u8 *bone = &m->bones[i * 4];
		struct coord in;
		f32 x = 0.0f;
		f32 y = 0.0f;
		f32 z = 0.0f;

		in.x = pos[0];
		in.y = pos[1];
		in.z = pos[2];

		// All three, always: the fourth byte is not the number of them. A
		// vertex with fewer repeats a bone in the bytes it does not need and
		// leaves the weight at zero, so the extra terms add nothing.
		for (s32 j = 0; j < 3; j++) {
			struct coord moved;
			const s32 which = bone[j] < m->nummatrices ? bone[j] : 0;

			mtx4TransformVec(&pal[which], &in, &moved);

			x += moved.x * weight[j];
			y += moved.y * weight[j];
			z += moved.z * weight[j];
		}

		out[i] = m->vertices[i];
		out[i].x = xblaMeshRound(x * fine);
		out[i].y = xblaMeshRound(y * fine);
		out[i].z = xblaMeshRound(z * fine);
	}

	if (xblaMeshVerbose && m->posedlog == 1) {
		s16 lo[3];
		s16 hi[3];

		m->posedlog = 2;

		for (s32 j = 0; j < 3; j++) {
			lo[j] = out[0].v[j];
			hi[j] = out[0].v[j];
		}

		for (s32 i = 1; i < m->numvertices; i++) {
			for (s32 j = 0; j < 3; j++) {
				if (out[i].v[j] < lo[j]) lo[j] = out[i].v[j];
				if (out[i].v[j] > hi[j]) hi[j] = out[i].v[j];
			}
		}

		sysLogPrintf(LOG_NOTE, "xblamesh:   posed box [%d %d %d]..[%d %d %d], "
				"written in %ds of a unit",
				lo[0] / fine, lo[1] / fine, lo[2] / fine,
				hi[0] / fine, hi[1] / fine, hi[2] / fine, fine);
	}

	return out;
}

/* -------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

/**
 * What the node is drawing this frame, against what it was authored with.
 *
 * The game draws some nodes from a copy of their vertices that it has moved -
 * a door stretched to fit its frame is the one that matters here - and a mesh
 * put in the node's place has the authored geometry and none of that. This is
 * how a mesh that is the right size but the wrong size on screen is told apart
 * from one that was read wrong.
 */
static void xblaMeshLogDrawn(struct model *model, struct modelnode *node, s32 slot)
{
	union modelrodata *rodata = node->rodata;
	union modelrwdata *rwdata;
	const Vtx *ro;
	const Vtx *rw;
	s32 n;

	if ((node->type & 0xff) != MODELNODETYPE_DL || !model) {
		return;
	}

	rwdata = modelGetNodeRwData(model, node);
	ro = rodata->dl.vertices;
	rw = rwdata ? rwdata->dl.vertices : NULL;
	n = rodata->dl.numvertices;

	if (!ro || !rw || n <= 0) {
		return;
	}

	{
		s16 alo[3], ahi[3], blo[3], bhi[3];

		for (s32 j = 0; j < 3; j++) {
			alo[j] = ahi[j] = ro[0].v[j];
			blo[j] = bhi[j] = rw[0].v[j];
		}

		for (s32 i = 1; i < n; i++) {
			for (s32 j = 0; j < 3; j++) {
				if (ro[i].v[j] < alo[j]) alo[j] = ro[i].v[j];
				if (ro[i].v[j] > ahi[j]) ahi[j] = ro[i].v[j];
				if (rw[i].v[j] < blo[j]) blo[j] = rw[i].v[j];
				if (rw[i].v[j] > bhi[j]) bhi[j] = rw[i].v[j];
			}
		}

		sysLogPrintf(LOG_NOTE, "xblamesh: slot %d drawn: authored "
				"[%d %d %d]..[%d %d %d] drawn [%d %d %d]..[%d %d %d]%s",
				slot, alo[0], alo[1], alo[2], ahi[0], ahi[1], ahi[2],
				blo[0], blo[1], blo[2], bhi[0], bhi[1], bhi[2],
				(alo[0] == blo[0] && ahi[0] == bhi[0] && alo[1] == blo[1] &&
				 ahi[1] == bhi[1] && alo[2] == blo[2] && ahi[2] == bhi[2])
						? "" : "  <- the game moved this node's vertices (a door's trim is mirrored on the mesh)");
	}
}

/* ---------------------------------------------------------------------------
 * The door trim
 *
 * A sliding door with DOORFLAG_0004 - nearly every one in dataDyne, the G5
 * Building's, Chicago's shutters, the Cetan's - does not hide inside the
 * wall as it opens. The game trims it: door0f08cb20() copies the door's
 * vertices with everything past a plane moved on to the plane, and the plane
 * walks across the door with its opening fraction (doorGetBbox()), so what is
 * drawn is only the part still in the doorway. A vertical door is the same
 * from the top down. The copy is what the node's rwdata points at, and a mesh
 * drawn in the node's place from its own authored vertices is the whole door,
 * standing in the wall it was meant to have slid into - which reads as the
 * door showing through the wall and fighting it for the surface.
 *
 * The trim is read back off the game's copy rather than off the door, which
 * this has no way to reach from a node: the copy's box against the authored
 * box says which axis was trimmed and where, exactly, since the plane is at a
 * whole unit and the vertices are s16. The same trim then goes on to a copy
 * of the mesh for the frame, with the texture coordinates carried along the
 * gradient xblaMeshNoteTriangle() kept, so the picture stays where it was.
 * ------------------------------------------------------------------------- */

/**
 * Whether the game is drawing this node from trimmed vertices, and the trim:
 * axis 0 is a sliding door, everything at or below ref in x moved to ref;
 * axis 1 a vertical one, everything at or above ref in y moved to ref.
 */
static s32 xblaMeshNodeTrim(struct model *model, struct modelnode *node, s32 *axis, s16 *ref)
{
	union modelrodata *rodata = node->rodata;
	union modelrwdata *rwdata;
	const Vtx *ro;
	const Vtx *rw;
	s16 rominx, rwminx, romaxy, rwmaxy;
	s32 n;

	if ((node->type & 0xff) != MODELNODETYPE_DL || !model) {
		return 0;
	}

	rwdata = modelGetNodeRwData(model, node);
	ro = rodata->dl.vertices;
	rw = rwdata ? rwdata->dl.vertices : NULL;
	n = rodata->dl.numvertices;

	if (!ro || !rw || rw == ro || n <= 0) {
		return 0;
	}

	rominx = ro[0].x;
	rwminx = rw[0].x;
	romaxy = ro[0].y;
	rwmaxy = rw[0].y;

	for (s32 i = 1; i < n; i++) {
		if (ro[i].x < rominx) rominx = ro[i].x;
		if (rw[i].x < rwminx) rwminx = rw[i].x;
		if (ro[i].y > romaxy) romaxy = ro[i].y;
		if (rw[i].y > rwmaxy) rwmaxy = rw[i].y;
	}

	if (rwminx > rominx) {
		*axis = 0;
		*ref = rwminx;
		return 1;
	}

	if (rwmaxy < romaxy) {
		*axis = 1;
		*ref = rwmaxy;
		return 1;
	}

	return 0;
}

/**
 * The mesh's vertices with the trim applied, for this frame. The copy lives
 * in the frame arena like a pose does, and is kept for the model and the
 * frame so the translucent pass and the other parts find it made.
 */
static Vtx *xblaMeshTrimCopy(struct xblameshbuilt *m, const struct model *model,
		s32 axis, s16 ref)
{
	Vtx *out;

	if (m->trimvtx && m->trimmodel == model && m->trimframe == frameCount &&
			m->trimaxis == axis && m->trimref == ref) {
		return m->trimvtx;
	}

	out = xblaMeshFrameAlloc((u32)m->numvertices * sizeof(Vtx));

	if (!out) {
		return NULL;
	}

	memcpy(out, m->vertices, (size_t)m->numvertices * sizeof(Vtx));

	for (s32 i = 0; i < m->numvertices; i++) {
		Vtx *v = &out[i];
		const f32 *g = &m->grad[i * 4 + axis * 2];
		f32 d;

		if (axis == 0) {
			if (v->x > ref) {
				continue;
			}

			d = (f32)(ref - v->x);
			v->x = ref;
		} else {
			if (v->y < ref) {
				continue;
			}

			d = (f32)(ref - v->y);
			v->y = ref;
		}

		v->s = xblaMeshRound((f32)v->s + d * g[0]);
		v->t = xblaMeshRound((f32)v->t + d * g[1]);
	}

	m->trimmodel = model;
	m->trimframe = frameCount;
	m->trimaxis = axis;
	m->trimref = ref;
	m->trimvtx = out;

	return out;
}

/* ---------------------------------------------------------------------------
 * Overlap diagnostic (--xbla-mesh-verbose only)
 *
 * "The release's model and the game's are both on the screen" is one question:
 * did any node of this model draw from the mesh this frame while another node
 * of the same model was left to draw its own geometry? Counted per (model,
 * slot) and reported at the end of the frame, with the reason the stock draw
 * was let through, so a report of two models on top of each other names the
 * model and the branch rather than needing the level it was seen in.
 * ------------------------------------------------------------------------- */
#define XBLAMESH_OVERLAPS 64  // (model, slot) pairs watched in one frame
#define XBLAMESH_OVERLAPSEEN 96 // (slot, reason) pairs reported before it goes quiet

struct xblameshoverlap {
	const struct model *model;
	s32 slot;
	s32 mesh;   // nodes drawn from the release's mesh this frame
	s32 stock;  // nodes left to the game this frame
	s32 reason; // why the last of those was left to it
};

static struct xblameshoverlap overlaps[XBLAMESH_OVERLAPS];
static s32 numOverlaps;

// One line per (slot, reason), not per frame: an overlap that is there is there
// every frame the model is on the screen, and 60 copies a second of it says
// nothing the first did not.
static s32 seenSlot[XBLAMESH_OVERLAPSEEN];
static s32 seenReason[XBLAMESH_OVERLAPSEEN];
static s32 numSeen;

static const char *const xblaMeshOverlapReasons[] = {
	"?",
	"the model is not the one the entry was filed under, and no headspot on the way up",
	"Mod.XblaMeshOnly names another slot",
	"a toggled piece the mesh has already, on a node that is not grafted",
	"the mesh would not build",
	"the translucent pass, where the mesh has no alpha span",
	"Mod.XblaMeshBoth",
};

static void xblaMeshNoteDraw(const struct model *model, s32 slot, s32 mesh, s32 reason)
{
	struct xblameshoverlap *o = NULL;

	for (s32 i = 0; i < numOverlaps; i++) {
		if (overlaps[i].model == model && overlaps[i].slot == slot) {
			o = &overlaps[i];
			break;
		}
	}

	if (!o) {
		if (numOverlaps >= XBLAMESH_OVERLAPS) {
			return;
		}

		o = &overlaps[numOverlaps++];
		o->model = model;
		o->slot = slot;
		o->mesh = 0;
		o->stock = 0;
		o->reason = 0;
	}

	if (mesh) {
		o->mesh++;
	} else {
		o->stock++;
		o->reason = reason;
	}
}

static void xblaMeshReportOverlaps(void)
{
	for (s32 i = 0; i < numOverlaps; i++) {
		const struct xblameshoverlap *o = &overlaps[i];
		s32 seen = 0;

		if (!o->mesh || !o->stock) {
			continue;
		}

		for (s32 j = 0; j < numSeen; j++) {
			if (seenSlot[j] == o->slot && seenReason[j] == o->reason) {
				seen = 1;
				break;
			}
		}

		if (seen || numSeen >= XBLAMESH_OVERLAPSEEN) {
			continue;
		}

		seenSlot[numSeen] = o->slot;
		seenReason[numSeen] = o->reason;
		numSeen++;

		sysLogPrintf(LOG_NOTE, "xblamesh: OVERLAP model %p slot %d: %d nodes drew the "
				"mesh and %d drew the game's own - %s",
				o->model, o->slot, o->mesh, o->stock,
				xblaMeshOverlapReasons[(u32)o->reason < ARRAYCOUNT(xblaMeshOverlapReasons)
						? o->reason : 0]);
	}

	numOverlaps = 0;
}

/**
 * Whether a node from another model is one this model is drawing.
 *
 * Perfect Dark keeps a character's head in a model file of its own and hangs
 * it off the body at a `HEADSPOT` node: `modelApplyHeadRelations()` makes the
 * head's root a child of that node and gives the head's top level nodes the
 * body node as their parent. So a head's display list reaches
 * `modelRenderNodeDl()` with the *body's* model and the *head's* modeldef, and
 * the definition check above would throw every head away - which is what used
 * to leave a release body under an N64 head.
 *
 * Walking up to the model's own root through a `HEADSPOT` is what says the
 * node has been grafted into this tree rather than being a stale address that
 * happens to hash here, so this replaces the definition check rather than
 * skipping it. A head is the only thing the game grafts - every `->parent`
 * the renderer writes is a headspot's - so a walk that reaches the root
 * without passing one is a node of this model's own tree and no business of
 * an entry that was filed under another model. The walk is a head's depth
 * plus the body's, both small; the bound is what stops a parent chain that is
 * being rewritten from going round for ever.
 */
static s32 xblaMeshNodeIsGrafted(const struct model *model, const struct modelnode *node)
{
	const struct modelnode *root = model->definition->rootnode;
	s32 crossed = 0;

	for (s32 i = 0; node && i < XBLAMESH_PARENTSCAN; i++) {
		if (node == root) {
			return crossed;
		}

		if ((node->type & 0xff) == MODELNODETYPE_HEADSPOT) {
			crossed = 1;
		}

		node = node->parent;
	}

	return 0;
}

/**
 * Whether the game would draw a translucent list of its own for this node, in
 * the translucent pass.
 *
 * A list node holds two lists and a count that says how the pair is used: 4 is
 * the one that puts the second list in the translucent pass, and it is the
 * only one that does. 3 draws it inside the opaque pass, 1 and 2 do not draw
 * it at all. So this is the question "is there a piece of this node that
 * belongs in the other pass", asked of the game rather than guessed at, and
 * the release's own answer - an alpha material in the mesh - is what has to
 * meet it.
 */
static s32 xblaMeshNodeDrawsXlu(const struct modelnode *node)
{
	const u32 type = node->type & 0xff;

	if (!node->rodata) {
		return 0;
	}

	if (type == MODELNODETYPE_DL) {
		return node->rodata->dl.mcount == 4 && node->rodata->dl.xlugdl != NULL;
	}

	if (type == MODELNODETYPE_GUNDL) {
		return node->rodata->gundl.unk12 == 4 && node->rodata->gundl.xlugdl != NULL;
	}

	return 0;
}

/**
 * The mode word a node's list is drawn under: the mcount of a display list
 * node, the unk12 of a gun one, which modelRenderNodeDl() and
 * modelRenderNodeGunDl() switch on to pick the render mode. 1 is untextured
 * one-cycle, 2 is a pass-through first cycle, 3 and 4 are the fog blend, and
 * 4 is also the one whose translucent list the game draws in the translucent
 * pass.
 */
static s32 xblaMeshNodeMode(const struct modelnode *node)
{
	const u32 type = node->type & 0xff;

	if (!node->rodata) {
		return 0;
	}

	if (type == MODELNODETYPE_DL) {
		return node->rodata->dl.mcount;
	}

	if (type == MODELNODETYPE_GUNDL) {
		return node->rodata->gundl.unk12;
	}

	return 0;
}

/**
 * Writes the state the game writes round the node's own list, by the same
 * functions modelRenderNodeDl() calls - which is where a model's lighting is.
 *
 * A Perfect Dark model has no G_LIGHTING: its vertex colours are baked, and
 * the room reaches it through the render state. For a chr (mode 7) that is a
 * two-cycle combiner, (texel - env) * shade alpha + env then times shade,
 * under a G_RM_FOG_PRIM_A blend towards the fog colour, which chrRender() set
 * to the chr's shade colour - the floor's colour times the room's brightness,
 * with an alpha that grows as the room darkens, so that a guard in a dark
 * room is mixed most of the way to a dark colour. Props are the same blend
 * under G_CC_TRILERP. The mesh's lists used to write a one-cycle
 * texture-times-shade over all of that, and drew at full brightness in every
 * room.
 *
 * `opa` is the pass: the opaque one takes the switch the game's opaque draw
 * takes, the translucent one takes what the game's translucent draw of a
 * mode-4 node takes.
 */
static void xblaMeshApplyNodeMode(struct modelrenderdata *renderdata,
		const struct modelnode *node, s32 opa)
{
	if (!opa) {
		modelApplyRenderModeType4(renderdata, false);
		return;
	}

	switch (xblaMeshNodeMode(node)) {
	case 1:
		modelApplyRenderModeType1(renderdata);
		break;
	case 3:
		modelApplyRenderModeType3(renderdata, true);
		break;
	case 4:
		modelApplyRenderModeType4(renderdata, true);
		break;
	case 2:
		modelApplyRenderModeType2(renderdata);
		break;
	default:
		modelApplyRenderModeType3(renderdata, true);
		break;
	}
}

/**
 * A render mode for the mesh's alpha span that keeps the node's first cycle.
 *
 * What the game wrote is two-cycle with the fog blend in the first cycle (or
 * a pass-through, for mode 2; or one-cycle, for mode 1), and the second cycle
 * is where the surface type goes. A one-cycle pair written over it - the
 * cutout's TEX_EDGE, the blend's XLU_SURF - puts the surface in cycle one and
 * the blend towards the shade colour is gone, and the span draws unlit beside
 * a body that is lit. So the first cycle is kept as the game set it and only
 * the second is chosen: `cycle2` is the *2 half of a G_RM pair.
 */
static void xblaMeshSetSpanMode(struct modelrenderdata *renderdata,
		const struct modelnode *node, u32 cycle2, u32 onecycle)
{
	const s32 mode = xblaMeshNodeMode(node);
	u32 word;

	if (mode == 1) {
		word = onecycle | cycle2;
	} else if (mode == 2) {
		word = G_RM_PASS | cycle2;
	} else {
		word = G_RM_FOG_PRIM_A | cycle2;
	}

	gDPPipeSync(renderdata->gdl++);
	gSPSetOtherMode(renderdata->gdl++, G_SETOTHERMODE_L, G_MDSFT_RENDERMODE, 29, word);
}

s32 xblaMeshRenderNode(struct modelrenderdata *renderdata, struct model *model,
		struct modelnode *node)
{
	struct xblameshentry *e;
	struct xblameshbuilt *m;
	struct xblameshuse *use;
	Mtxf *finemtx = NULL;
	s32 fine = 1;
	Mtxf *root;
	Mtxf *drawmtx = NULL;
	Vtx *posed;
	Gfx *list;
	Gfx *xlulist = NULL;
	Gfx *fadelist = NULL;
	s32 grafted = 0;
	s32 xlupart = -1;
	s32 fadepart = -1;
	const s32 opa = (renderdata->flags & MODELRENDERFLAG_OPA) != 0;
	const s32 xlu = (renderdata->flags & MODELRENDERFLAG_XLU) != 0;

	if (!optEnabled || opened <= 0 || !node || !g_XblaMeshNumNodes) {
		return 0;
	}

	if (!opa && !xlu) {
		return 0;
	}

	e = xblaMeshSlotFor(node);

	if (!e || e->node != node || !e->modeldef) {
		return 0;
	}

	// A model file can be loaded twice at once, and a freed one's address can
	// come back as something else's node. The definition the model is being
	// drawn from is what says this entry is about this model - except for a
	// head, which is a model of its own grafted into the body's tree.
	if (model && model->definition && model->definition != e->modeldef) {
		if (!xblaMeshNodeIsGrafted(model, node)) {
			if (xblaMeshVerbose && !e->suppress) {
				xblaMeshNoteDraw(model, e->slot, 0, 1);
			}

			return 0;
		}

		grafted = 1;
	}

	if (optOnlySlot && e->slot != optOnlySlot) {
		if (xblaMeshVerbose) {
			xblaMeshNoteDraw(model, e->slot, 0, 2);
		}

		return 0;
	}

	// A toggled stock piece the release's mesh carries itself: a head's hair.
	// Drawing nothing is the whole of it, since the mesh beside it has one
	// already - and it is only ever a head's, because xblaMeshIsHairList()
	// files nothing else (it asks for the head skeleton before the hat).
	//
	// Whether the head is grafted is not asked here, and it used to be: the
	// Combat Simulator's Character page, and the Ghost Trials pages made from
	// it, zoom on a head by loading the head file as a model of its own - no
	// body, no headspot, `model->definition` *is* the head - and every head
	// with a hat piece came up there with the N64 hair hanging over the
	// release's. A chr's head is the same head grafted; both draw the mesh,
	// so both leave the hair to it. Mod.XblaMeshBoth keeps the hair, that
	// switch being there to put the two on top of each other.
	if (e->suppress == XBLAMESH_SUPPRESS_HAIR) {
		if (xblaMeshVerbose && optBoth) {
			sysLogPrintf(LOG_NOTE, "xblamesh: a toggled piece the mesh has already drew "
					"the game's own: model %p node %p grafted %d both %d",
					model, node, grafted, optBoth);
		}

		return optBoth ? 0 : 1;
	}

	// A list of a model whose mesh has that geometry already: every list of a
	// matched model but the one the id was written on, the far LOD
	// alternatives and the toggled pieces. Drawing it is drawing the N64 model
	// inside the release's one - fifteen body parts inside every guard.
	//
	// It is the mesh being drawn that this depends on, so the mesh is built
	// here too: one that will not build leaves the model drawing all of its
	// own geometry rather than most of it drawing nothing at all.
	if (e->suppress == XBLAMESH_SUPPRESS_COVERED) {
		m = xblaMeshBuild(e->slot);

		if (!m || optBoth) {
			if (xblaMeshVerbose) {
				xblaMeshNoteDraw(model, e->slot, 0, m ? 6 : 4);
			}

			return 0;
		}

		// The translucent pass, on one of the fifteen nodes here that draw a
		// pane of their own: the same rule the replaced nodes follow. A mesh
		// with an alpha material somewhere has the pane, and one with none
		// anywhere has nothing to put where the game's would have been.
		if (!opa && xblaMeshNodeDrawsXlu(node) && m->allxlu < 0) {
			if (xblaMeshVerbose) {
				xblaMeshNoteDraw(model, e->slot, 0, 5);
			}

			return 0;
		}

		return 1;
	}

	// The mesh is built before the part is looked at, so that a mesh that will
	// not build leaves every part of the model drawing its own geometry rather
	// than only the first one.
	m = xblaMeshBuild(e->slot);

	if (!m) {
		if (xblaMeshVerbose) {
			xblaMeshNoteDraw(model, e->slot, 0, 4);
		}

		return 0;
	}

	// Which of the mesh's lists this node draws. A group is one part of the
	// model, in the same order and the same number - true of all 542 (model,
	// mesh) pairs in the release, with the parts numbered 0..n-1 - so the node
	// carrying part p draws group p and nothing else. That is what lets a
	// piece the game has hidden stay hidden: a head's earpiece is a part of
	// its own under a toggle, and one list for the whole mesh drew it whatever
	// the toggle said.
	//
	// A model whose parts do not line up with the mesh's groups - which is
	// nothing in the release, but could be a mod's model or a model matched by
	// size - falls back to what this did before: the first part draws every
	// group and the rest draw nothing.
	use = (e->use >= 0 && e->use < numUses && uses[e->use].modeldef == e->modeldef)
			? &uses[e->use] : NULL;

	if (use && use->numparts == m->numgroups && e->part < m->numgroups) {
		list = &m->gdl[m->groupgfx[e->part]];
		xlupart = m->groupxlu[e->part];
		fadepart = m->groupfade[e->part];
	} else if (e->part == 0) {
		list = &m->gdl[m->allgfx];
		xlupart = m->allxlu;
		fadepart = m->allfade;
	} else {
		return 1;
	}

	// The release's own light, which fades by vertex alpha: blended, in the
	// translucent pass, whatever the node says - see XBLAMESH_SPAN_FADE.
	if (fadepart >= 0) {
		fadelist = &m->gdl[fadepart];
	}

	// The release's own translucent geometry, and where it goes.
	//
	// A mesh's materials say which of its draws carry alpha, and those are
	// built as a span of their own. Whether that span is a cutout in the
	// opaque pass or a blend in the translucent one is the node's business,
	// not the material's: a node the game draws a translucent list for (mcount
	// 4, 54 of the nodes the release replaces) has a piece that belongs in the
	// other pass, and everywhere else an alpha material is a grille or a fence
	// that the game drew opaque and this keeps drawing opaque.
	//
	// Where the span does go in the translucent pass, the game's own list must
	// not: 27 of those 54 have the same surface in both, and drawing them one
	// over the other doubles a window's darkening. Where the release has no
	// alpha for a node that has a translucent list - the other 27 - returning
	// 0 leaves the game to draw its own, which is the same rule the hair
	// follows: take nothing away that nothing here replaces.
	if (xlupart >= 0 && xblaMeshNodeDrawsXlu(node)) {
		xlulist = &m->gdl[xlupart];
	}

	if (!opa && !xlulist && !fadelist) {
		// Only a node the game actually draws a translucent list for is a
		// stock draw; every other replaced node reaches here in the
		// translucent pass and the game draws nothing for it either.
		if (xblaMeshVerbose && xblaMeshNodeDrawsXlu(node)) {
			xblaMeshNoteDraw(model, e->slot, 0, 5);
		}

		return 0;
	}

	// The matrix this is drawn under is the first part's, whichever part is
	// drawing. Every group is in the mesh's one space - a door's window pane
	// is where the door has it, not where its own node would put it, and all
	// five multi-part meshes the release has that are not skinned load one
	// matrix for every part anyway - and a posed mesh comes out in the first
	// part's space by construction. Naming it as well as loading it is what
	// keeps the vertices inside the s16 a Perfect Dark vertex holds.
	posed = m->vertices;
	root = NULL;

	if (use) {
		root = xblaMeshPartMtx(model, use, 0);

		if (optPose && m->nummatrices && root) {
			Vtx *pose;

			if (m->posedmodel == model && m->posedframe == frameCount &&
					m->posedvtx) {
				pose = m->posedvtx;
				finemtx = m->posedmtx;
			} else {
				pose = xblaMeshPose(m, model, root, &finemtx, &fine);

				if (pose) {
					framePoses++;
				} else {
					framePoseFails++;
				}

				// Not remembered when there was no room this frame, so that
				// the next part tries again rather than inheriting a miss.
				if (pose) {
					m->posedmodel = model;
					m->posedframe = frameCount;
					m->posedvtx = pose;
					m->posedmtx = finemtx;
					m->posedfine = fine;
				}
			}

			if (pose) {
				posed = pose;

				// The pose was written finer than the game's units, so it goes
				// under the matrix that takes that back out rather than under
				// the bone's own. A pose that could not be taken any finer
				// leaves this NULL and the bone's matrix stands.
				drawmtx = finemtx;
			} else if (xblaMeshVerbose) {
				// Only reachable now at the cap or on a failed malloc: the
				// arena adds a chunk for anything under it, in the frame that
				// asks. Worth keeping, because what it draws instead is the
				// bind pose - a head a body's height above the body.
				sysLogPrintf(LOG_NOTE, "xblamesh: slot %d drew its bind pose - the frame "
						"arena would not grow (%d chunks, %u bytes held, %u wanted "
						"this frame)", e->slot, frameNumChunks[frameIndex],
						frameBytes[frameIndex], frameWanted);
			}
		}
	}

	// A door the game is drawing trimmed: trim the mesh the same way. Only
	// from the bind pose, which is in the model's own units like the game's
	// vertices; a posed copy is finer and in the first part's space, and
	// nothing the game trims is skinned.
	if (posed == m->vertices && m->grad) {
		s32 axis;
		s16 ref;

		if (xblaMeshNodeTrim(model, node, &axis, &ref)) {
			Vtx *trimmed = xblaMeshTrimCopy(m, model, axis, ref);

			if (trimmed) {
				posed = trimmed;
			}

			if (xblaMeshVerbose && !m->trimlogged) {
				m->trimlogged = 1;
				sysLogPrintf(LOG_NOTE, "xblamesh: slot %d is a door the game trims: "
						"%s %d, %s", e->slot, axis == 0 ? "x at or below" : "y at or above",
						ref, trimmed ? "mirrored on the mesh" : "no room in the arena");
			}
		}
	}

	if (!root) {
		root = modelFindNodeMtx(model, node, 0);
	}

	if (xblaMeshVerbose && !m->logged) {
		xblaMeshLogDrawn(model, node, e->slot);
		m->logged = 1;
	}

	// The first few draws, with the model each came from. One node is shared
	// by every instance of its model, so this is what says whether a mesh that
	// covers more of the screen than the geometry it replaced is too big or is
	// two of them - the G5 car lift doors are two.
	if (xblaMeshVerbose && xblaMeshDrawLog < XBLAMESH_DRAWLOG) {
		xblaMeshDrawLog++;
		sysLogPrintf(LOG_NOTE, "xblamesh: draw %d: slot %d node %p model %p, "
				"part %d of %d groups, %d palette entries, %s",
				xblaMeshDrawLog, e->slot, node, model, e->part, m->numgroups,
				m->nummatrices, posed == m->vertices ? "bind pose" :
				m->posedfine > 1 ? "posed, in fractions of a unit" : "posed");
	}

	if (!drawmtx) {
		drawmtx = root;
	}

	// The divided copy is floats and says so; the bone's own matrix is one of
	// the model's, which the game converts to s15.16 in place after listing
	// the model, and is read the way every matrix of the game's is.
	if (drawmtx) {
		gSPMatrix(renderdata->gdl++, osVirtualToPhysical(drawmtx),
				G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW |
				(drawmtx != root ? G_MTX_FLOATS : 0));
	}

	gSPSegment(renderdata->gdl++, SPSEGMENT_MODEL_VTX, osVirtualToPhysical(posed));

	if (opa) {
		// The lighting: the state the game would have written round its own
		// list for this node. The list itself writes no combiner and no
		// render mode - see xblaMeshSetMaterial() and xblaMeshApplyNodeMode().
		xblaMeshApplyNodeMode(renderdata, node, 1);
		gSPDisplayList(renderdata->gdl++, list);
		frameDraws++;

		// An alpha span that is not going to the translucent pass is a cutout
		// and belongs here, after the solid part of the same group - a grille,
		// a fence, the leaves of a plant. This is where every alpha material
		// was drawn before the span was split out; the mode is TEX_EDGE in the
		// node's own first cycle, so it stays as lit as the rest.
		if (xlupart >= 0 && !xlulist) {
			xblaMeshSetSpanMode(renderdata, node,
					renderdata->zbufferenabled ? G_RM_AA_ZB_TEX_EDGE2 : G_RM_AA_TEX_EDGE2,
					renderdata->zbufferenabled ? G_RM_AA_ZB_TEX_EDGE : G_RM_AA_TEX_EDGE);
			gSPDisplayList(renderdata->gdl++, &m->gdl[xlupart]);
		}
	}

	if (xlu && xlulist) {
		if (xblaMeshVerbose && !m->xlulogged) {
			m->xlulogged = 1;
			sysLogPrintf(LOG_NOTE, "xblamesh: slot %d part %d draws its alpha span "
					"in the translucent pass", e->slot, e->part);
		}

		// The state the game's translucent draw of this node writes - the
		// combiner, the fog and environment colours, the cycle - and then the
		// translucent surface in the second cycle, with the fog blend kept in
		// the first so the glass is lit like the body it is set in.
		xblaMeshApplyNodeMode(renderdata, node, 0);
		xblaMeshSetSpanMode(renderdata, node,
				renderdata->zbufferenabled ? G_RM_AA_ZB_XLU_SURF2 : G_RM_AA_XLU_SURF2,
				renderdata->zbufferenabled ? G_RM_AA_ZB_XLU_SURF : G_RM_AA_XLU_SURF);
		gSPDisplayList(renderdata->gdl++, xlulist);
	}

	if (xlu && fadelist) {
		if (xblaMeshVerbose && !m->fadelogged) {
			m->fadelogged = 1;
			sysLogPrintf(LOG_NOTE, "xblamesh: slot %d part %d draws a fading span "
					"in the translucent pass", e->slot, e->part);
		}

		// Blended by texel times vertex alpha, no depth write: a beam of light
		// that darkens nothing behind it and hides nothing behind it. Not lit:
		// the span carries its own combiner (see xblaMeshSetMaterial()) and
		// the pair here has no fog blend in either cycle.
		gDPPipeSync(renderdata->gdl++);
		gDPSetRenderMode(renderdata->gdl++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
		gSPDisplayList(renderdata->gdl++, fadelist);
	}

	// Put the bone's own matrix back, because the divided one is this list's
	// business and nobody else's. A display list node does not load a matrix -
	// a chr is drawn under one matrix for the whole model, with the pose baked
	// into its vertices - so whatever is loaded here is what the next node
	// inherits, and a node that kept its own geometry would draw at a
	// sixteenth of its size. This leaves behind exactly what a mesh drawn
	// without the division leaves behind.
	if (drawmtx != root && root) {
		gSPMatrix(renderdata->gdl++, osVirtualToPhysical(root),
				G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
	}

	if (xblaMeshVerbose) {
		xblaMeshNoteDraw(model, e->slot, 1, 0);

		if (optBoth) {
			xblaMeshNoteDraw(model, e->slot, 0, 6);
		}
	}

	// Mod.XblaMeshBoth: draw the game's geometry as well, so the two can be
	// seen on top of each other. The only way to tell a mesh that is in the
	// wrong place from one that is the wrong size.
	return optBoth ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------- */

/**
 * Whether there is a package to draw from. Asked of xblaimport rather than of
 * the path, because the path unpacks an archive to answer and this is a
 * question a menu draw is allowed to ask every frame.
 */
s32 xblaMeshIsAvailable(void)
{
	return xblaImportIsAvailable();
}

s32 xblaMeshGetEnabled(void)
{
	return optEnabled;
}

/**
 * A live switch either way: the models were matched as they loaded whether or
 * not this was on, so turning it on draws them from the next frame.
 *
 * The one thing that happens here rather than at a model load is the unpack.
 * A machine whose package is still inside its .7z has nothing to match against
 * and has matched nothing, so the archive comes apart at the moment somebody
 * first asks for the meshes - a few seconds, once, in a menu they have just
 * clicked something in - and the level after that has them.
 *
 * Which is the one case where the switch does nothing anybody can see, so it
 * is noted for the page to say so. The test is that this call is what opened
 * the package: if it was already open the models were matched as they loaded
 * and there is nothing to explain, and if it will not open there is no package
 * and the page's items are not there to read.
 */
void xblaMeshSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled == optEnabled) {
		return;
	}

	optEnabled = enabled;

	if (enabled) {
		const s32 wasopen = opened > 0;

		if (xblaMeshOpen(1) && !wasopen && STAGE_IS_LEVEL(mainGetStageNum())) {
			openedLate = 1;
		}
	}

	// The rooms follow this switch (xblastage.h), and unlike the models they
	// are not matched at the draw: the ones loaded so far have to go
	xblaStageSwitched();
}

s32 xblaMeshModelsAreLate(void)
{
	return openedLate;
}

/**
 * The meshes on and off from a key, the way F8 does texture packs: for looking
 * at the release's model against the game's own without leaving the level.
 * The switch is live at the draw (xblaMeshRenderNode() reads it), so the
 * models change under the player on the next frame, and the rooms with them
 * (xblaStageSwitched() drops the loaded ones) - except on a machine
 * whose package was still in its archive, where the first press opens it and
 * the level after this one has them (xblaMeshSetEnabled()).
 *
 * Resolved to a scancode on first use, as texpack.c does: inputInit() fills
 * the table the name is looked up in, so it cannot be done when the config is
 * read.
 */
#define XBLAMESH_KEYNAME_LEN 32
static char toggleKeyName[XBLAMESH_KEYNAME_LEN] = "F6";
static s32 toggleKeyVk = -1;

s32 xblaMeshToggleGetKey(void)
{
	if (toggleKeyVk < 0) {
		if (!toggleKeyName[0] || !strcmp(toggleKeyName, "NONE")) {
			toggleKeyVk = 0;
		} else {
			toggleKeyVk = inputGetKeyByName(toggleKeyName);

			if (toggleKeyVk < 0) {
				toggleKeyVk = 0;
			}
		}
	}

	return toggleKeyVk;
}

void xblaMeshToggleSetKey(s32 vk)
{
	if (vk <= 0 || vk >= VK_TOTAL_COUNT) {
		toggleKeyName[0] = '\0';
		toggleKeyVk = 0;
		return;
	}

	strncpy(toggleKeyName, inputGetKeyName(vk), sizeof(toggleKeyName) - 1);
	toggleKeyName[sizeof(toggleKeyName) - 1] = '\0';
	toggleKeyVk = vk;
}

void xblaMeshTick(void)
{
	const s32 vk = xblaMeshToggleGetKey();

	// inputKeyJustPressed() consumes the edge, so ask once a frame and only
	// when the key is actually bound.
	if (vk > 0 && inputKeyJustPressed(vk)) {
		xblaMeshSetEnabled(!optEnabled);
		sysLogPrintf(LOG_NOTE, "xblamesh: meshes %s%s", optEnabled ? "on" : "off",
				optEnabled && !xblaMeshIsAvailable() ? " (no package found)" : "");
	}
}

PD_CONSTRUCTOR static void xblaMeshConfigInit(void)
{
	configRegisterInt("Mod.XblaMeshes", &optEnabled, 0, 1);
	configRegisterString("Mod.XblaMeshKey", toggleKeyName, sizeof(toggleKeyName));

	// Debugging one mesh at a time: everything else keeps its own geometry, so
	// what is on screen is the game's except for the one thing being looked at
	configRegisterInt("Mod.XblaMeshOnly", &optOnlySlot, 0, 0xffff);
	configRegisterInt("Mod.XblaMeshBoth", &optBoth, 0, 1);
	configRegisterInt("Mod.XblaMeshPose", &optPose, 0, 1);

	// Mod.XblaMeshTextures is registered by xblatex.c, which is where the flag
	// lives now: read at the point a picture is handed to the renderer rather
	// than where a list is built, so it can be turned on and off in the menu.
}

void xblaMeshSetVerbose(s32 verbose)
{
	xblaMeshVerbose = verbose;
}

u8 *xblaMeshReadFile(u16 fileid, u32 *outLen)
{
	if (fileid == 0 || !xblaMeshOpen(0)) {
		return NULL;
	}

	// Slot i is file id i + 1, as everywhere in this container
	return xblaMeshReadSlot((s32)fileid - 1, outLen);
}

void xblaMeshTrace(FILE *f)
{
	u32 arenakb = 0;
	s32 chunks = 0;
	u32 nodes = 0;
	u32 slots = 0;

	xblaMeshArenaStats(&arenakb, &chunks);

	for (u32 i = 0; i < XBLAMESH_HASHSIZE; i++) {
		if (hash[i].node) {
			slots++;

			if (hash[i].modeldef) {
				nodes++;
			}
		}
	}

	fprintf(f, "xblamesh: enabled %d opened %d pose %d; %u meshes built, %u KB; pose arena %u KB in %d chunks, cap %d MB; %u nodes in %u of %d table slots; frame %u\n",
			optEnabled, opened, optPose, g_XblaMeshNumMeshes,
			(g_XblaMeshBytes + 1023) / 1024, arenakb, chunks,
			XBLAMESH_ARENA_MAX / (1024 * 1024), nodes, slots, XBLAMESH_HASHSIZE,
			frameCount);
	fprintf(f, "xblamesh last frame: %u opaque lists drawn, %u poses written, %u poses refused by the arena (%u bytes wanted)\n",
			frameDrawsLast, framePosesLast, framePoseFailsLast, frameWanted);
}

s32 xblaMeshTraceModel(FILE *f, const struct model *model, const char *indent)
{
	struct modelnode *node;
	s32 count = 0;
	s32 walked = 0;

	if (!model || !model->definition) {
		return 0;
	}

	for (node = model->definition->rootnode; node; node = xblaMeshNextNode(node)) {
		const struct xblameshentry *e = xblaMeshSlotFor(node);
		const struct xblameshbuilt *m;

		walked++;

		if (!e || e->node != node) {
			continue;
		}

		if (e->modeldef) {
			count++;
		}

		if (!f) {
			continue;
		}

		m = built && e->slot < numRecords ? &built[e->slot] : NULL;

		fprintf(f, "%snode %p type %02x slot %d part %d def %p%s%s%s built %d",
				indent ? indent : "", (const void *)node, node->type & 0xff, e->slot, e->part,
				(const void *)e->modeldef,
				e->modeldef ? "" : " DROPPED",
				e->suppress == XBLAMESH_SUPPRESS_HAIR ? " HAIR-suppressed" :
				e->suppress == XBLAMESH_SUPPRESS_COVERED ? " covered" : "",
				e->modeldef && e->modeldef != model->definition ? " (grafted or another load)" : "",
				m ? m->state : 0);

		if (m && m->state > 0) {
			fprintf(f, " %d verts %d tris %d groups %d matrices, posed for %s at mesh frame %u (now %u) fine %d",
					m->numvertices, m->numtris, m->numgroups, m->nummatrices,
					m->posedmodel == model ? "this model" : "another model",
					m->posedframe, frameCount, m->posedfine);
		}

		fprintf(f, "\n");
	}

	if (f && count == 0) {
		fprintf(f, "%sno release mesh on any of the %d nodes walked (def %p)\n",
				indent ? indent : "", walked, (const void *)model->definition);
	}

	return count;
}

#else

void xblaMeshTrace(FILE *f) { }
s32 xblaMeshTraceModel(FILE *f, const struct model *model, const char *indent) { return 0; }
void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid) { }
s32 xblaMeshRenderNode(struct modelrenderdata *renderdata, struct model *model,
		struct modelnode *node) { return 0; }
void xblaMeshFrameReset(void) { }
s32 xblaMeshIsAvailable(void) { return 0; }
s32 xblaMeshGetEnabled(void) { return 0; }
void xblaMeshSetEnabled(s32 enabled) { }
s32 xblaMeshToggleGetKey(void) { return 0; }
void xblaMeshToggleSetKey(s32 vk) { }
void xblaMeshTick(void) { }
void xblaMeshResetModels(void) { }
s32 xblaMeshModelsAreLate(void) { return 0; }
u8 *xblaMeshReadFile(u16 fileid, u32 *outLen) { return NULL; }

#endif

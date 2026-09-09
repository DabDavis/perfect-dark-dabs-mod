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
 *     asks to draw it, and kept for as long as the game runs.
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
#include "lib/main.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "x360.h"
#include "xblaimport.h"
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

// The two strides, unskinned and skinned. Everything before the weights is
// laid out the same in both.
#define XBLAMESH_STRIDE_RIGID 36
#define XBLAMESH_STRIDE_SKIN 48

#define XBLAMESH_HASHSIZE 4096 // a power of two; open addressed

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
u32 g_XblaMeshNumTris = 0;
u32 g_XblaMeshBytes = 0;

struct xblameshentry {
	const struct modelnode *node;      // key
	const struct modeldef *modeldef;   // which load of which model it belongs to
	u16 slot;
	u16 part;
	s32 use;                           // into uses[], or -1
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
	s32 numvertices;   // as emitted, which repeats one shared between batches
	s32 numtris;
	s32 state;         // 0 untried, 1 built, -1 no good
	s32 logged;
	s32 posedlog;

	// Skinning, for a mesh that has a matrix palette. The vertices above are
	// the bind pose; these are what it takes to put them in a pose of the
	// game's. Positions are kept as they were read rather than as the s16 the
	// Vtx holds, since they are transformed before they are rounded.
	s32 nummatrices;
	s32 numgroups;
	f32 scale;         // mesh units to the game's, out of the header
	s32 groupgfx[XBLAMESH_MAXPARTS]; // into gdl: where each group's list starts
	s32 allgfx;                      // and the one that calls every group

	// The posed copy already made this frame, and who for. Every part of a
	// model draws its own group now, so without this Dr Carroll would pose
	// thirteen copies of himself a frame and fill the arena with twelve of
	// them. One entry is enough: the renderer walks a model's tree in one go,
	// so a model's parts are drawn one after another.
	const struct model *posedmodel;
	u32 posedframe;
	Vtx *posedvtx;
	Mtxf *invbind;     // one per palette entry
	f32 *bindpos;      // three per emitted vertex
	f32 *weights;      // three per emitted vertex; only the first two are ever set
	u8 *bones;         // three per emitted vertex, and the count in the fourth
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

		if (rodata && rodata + 8 <= len) {
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

static struct xblameshentry *xblaMeshSlotFor(const struct modelnode *node)
{
	u32 h = (u32)(((uintptr_t)node >> 4) * 2654435761u) & (XBLAMESH_HASHSIZE - 1);

	for (s32 i = 0; i < XBLAMESH_HASHSIZE; i++) {
		struct xblameshentry *e = &hash[(h + i) & (XBLAMESH_HASHSIZE - 1)];

		if (!e->node || e->node == node) {
			return e;
		}
	}

	return NULL;
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
	s32 found = 0;
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
				if (!e->node) {
					g_XblaMeshNumNodes++;
				}

				e->node = ournode;
				e->modeldef = modeldef;
				e->slot = (u16)slot;
				e->part = (u16)(id >> 12);
				e->use = xblaMeshUseFor(modeldef, slot);
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
			if (ournode->next) {
				ournode = ournode->next;
				theiroff = xblaMeshBE32(file + theiroff + 12) & 0xffffff;
				break;
			}

			ournode = ournode->parent;

			if (theiroff + 12 <= len) {
				theiroff = xblaMeshBE32(file + theiroff + 8) & 0xffffff;
			} else {
				return 0;
			}
		}

		if (!ournode) {
			break;
		}

		if (!theiroff) {
			return 0;
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

		if (!e->node) {
			g_XblaMeshNumNodes++;
		}

		e->node = ours[part];
		e->modeldef = modeldef;
		e->slot = (u16)slot;
		e->part = (u16)part;
		e->use = xblaMeshUseFor(modeldef, slot);

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

	return numparts;
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

	if (!xblaMeshOpen(0)) {
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
	numUses = 0;
	openedLate = 0;

	memset(hash, 0, sizeof(hash));
	g_XblaMeshNumNodes = 0;

	for (s32 i = 0; i < numRecords && built; i++) {
		built[i].posedmodel = NULL;
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

	if (h->vertexoffset < XBLAMESH_HEADER || h->indexoffset <= h->vertexoffset ||
			h->indexoffset > len) {
		return 0;
	}

	if (h->groupoffset != XBLAMESH_HEADER + XBLAMESH_MATRIX * h->nummatrices) {
		return 0;
	}

	if (h->drawoffset < h->groupoffset ||
			h->drawoffset + XBLAMESH_ENTRY * h->numdraws != h->vertexoffset) {
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

	// One list per group, by index into gdl until the array stops moving, and
	// the little list that calls all of them for a model whose parts and
	// groups do not line up.
	s32 groupgfx[XBLAMESH_MAXPARTS];
	s32 numgroups;
	s32 allgfx;

	s32 numgfx, capgfx;
	s32 numvtx, capvtx;
	s32 numtris;

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

	colour = xblaMeshBE32(v + 32);
	col->r = (u8)(colour >> 16);
	col->g = (u8)(colour >> 8);
	col->b = (u8)colour;
	col->a = 0xff;

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
 */
static s32 xblaMeshSetMaterial(struct xblameshbuilder *b, u32 material)
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

	if (!xblaMeshRoomForGfx(b, 12)) {
		return 0;
	}

	gdl = &b->gdl[b->numgfx];

	gDPPipeSync(gdl++);

	if (tile) {
		// Texture times shade, in both channels. The release colours every
		// vertex, so the shade is doing work here and is not a flat white.
		gDPSetCombineMode(gdl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);

		// A material that carries alpha is a cutout - a grille, a fence, a
		// leaf - and goes through the alpha compare rather than the blender,
		// since this only ever draws in the opaque pass. One that does not
		// keeps the plain opaque mode the untextured list used.
		if (alpha) {
			gDPSetRenderMode(gdl++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
		} else {
			gDPSetRenderMode(gdl++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
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
	} else {
		gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
		gDPSetRenderMode(gdl++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
		gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
	}

	b->numgfx = (s32)(gdl - b->gdl);

	if (xblaMeshVerbose) {
		sysLogPrintf(LOG_NOTE, "xblamesh:   material %08x -> record %u%s%s",
				material, record, alpha ? " alpha" : "",
				tile ? "" : " (no texture; shade only)");
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
 */
static s32 xblaMeshBuildGroup(struct xblameshbuilder *b, const u8 *file, u32 len,
		const struct xblameshhdr *h, u32 stride, u32 firstdraw, u32 numdraws)
{
	const u32 numtris = (len - h->indexoffset) / 6;

	// The material the list is currently set up for. The top two bytes of a
	// material word are not understood, so this compares the whole word rather
	// than the part it reads - two draws whose materials differ only up there
	// get a redundant setup, which is cheaper than being wrong about it.
	u32 lastmaterial = 0;

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

		// A draw is one material's worth of triangles, and consecutive draws
		// share one more often than not - a character's head and hands are the
		// same skin. The batch has to close first: a vertex load and the
		// triangles that index it belong to the state they were written under.
		if (d == firstdraw || material != lastmaterial) {
			if (!xblaMeshCloseBatch(b) || !xblaMeshSetMaterial(b, material) ||
					!xblaMeshOpenBatch(b)) {
				return 0;
			}

			lastmaterial = material;
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
		}
	}

	if (!xblaMeshCloseBatch(b)) {
		return 0;
	}

	if (!xblaMeshRoomForGfx(b, 6)) {
		return 0;
	}

	// Put the state back to what an untextured list would have left, so that
	// what leaks past the end of this is the same whether the mesh drew with
	// textures or without. The next node sets its own before it draws, so this
	// only has to be harmless rather than right.
	gDPPipeSync(&b->gdl[b->numgfx]);
	b->numgfx++;
	gSPTexture(&b->gdl[b->numgfx], 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
	b->numgfx++;
	gDPSetCombineMode(&b->gdl[b->numgfx], G_CC_SHADE, G_CC_SHADE);
	b->numgfx++;
	gDPSetRenderMode(&b->gdl[b->numgfx], G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
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

	b->numgroups = numgroups;

	for (s32 g = 0; g < numgroups; g++) {
		u32 firstdraw = 0;
		u32 numdraws = h->numdraws;

		if ((u32)numgroups == intable) {
			const u8 *group = file + h->groupoffset + (u32)g * XBLAMESH_ENTRY;

			firstdraw = xblaMeshBE32(group);
			numdraws = xblaMeshBE32(group + 4);

			if (firstdraw > h->numdraws || numdraws > h->numdraws - firstdraw) {
				return 0;
			}
		}

		b->groupgfx[g] = b->numgfx;

		if (!xblaMeshBuildGroup(b, file, len, h, stride, firstdraw, numdraws)) {
			return 0;
		}
	}

	// The list that calls them all. Its commands hold addresses inside the
	// array they are in, which is still growing, so they go in after the
	// batches do - by then nothing moves again.
	b->allgfx = b->numgfx;

	if (!xblaMeshRoomForGfx(b, numgroups + 1)) {
		return 0;
	}

	b->numgfx += numgroups + 1;

	xblaMeshWriteBatches(b);

	for (s32 g = 0; g < numgroups; g++) {
		gSPDisplayList(&b->gdl[b->allgfx + g], &b->gdl[b->groupgfx[g]]);
	}

	gSPEndDisplayList(&b->gdl[b->allgfx + numgroups]);

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
		free(b.batches);
		free(file);
		sysLogPrintf(LOG_ERROR, "xblamesh: slot %d did not build", slot);
		return NULL;
	}

	free(b.batches);

	if (b.skinned && !xblaMeshReadBind(m, file, &h, b.scale)) {
		free(b.gdl);
		free(b.vertices);
		free(b.colours);
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
	m->numvertices = b.numvtx;
	m->numtris = b.numtris;
	m->bindpos = b.bindpos;
	m->weights = b.weights;
	m->bones = b.bones;
	m->allgfx = b.allgfx;
	m->numgroups = b.numgroups;
	m->state = 1;

	for (s32 g = 0; g < XBLAMESH_MAXPARTS; g++) {
		m->groupgfx[g] = b.groupgfx[g];
	}

	g_XblaMeshNumMeshes++;
	g_XblaMeshNumTris += (u32)b.numtris;
	g_XblaMeshBytes += (u32)((size_t)b.numgfx * sizeof(Gfx) +
			(size_t)b.numvtx * (sizeof(Vtx) + sizeof(Col)));

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
 * It only grows between frames. Growing inside one would move vertices that
 * commands already written this frame point at, which is the same mistake as
 * building a list around a growing array; a frame that asks for more than
 * there is draws what fits and the rest next frame, one bigger.
 */
#define XBLAMESH_ARENA_MAX (48 * 1024 * 1024)

static u8 *frameArena[2];
static u32 frameCap[2];
static u32 framePos;
static u32 frameWanted;
static s32 frameIndex;

// Which frame this is, for the posed copy a mesh keeps. It only has to differ
// from the frame before it, so wrapping is no more than one wasted pose.
static u32 frameCount;

void xblaMeshFrameReset(void)
{
	if (!optEnabled) {
		return;
	}

	frameIndex ^= 1;
	frameCount++;

	if (frameWanted > frameCap[frameIndex] && frameWanted <= XBLAMESH_ARENA_MAX) {
		u8 *grown = realloc(frameArena[frameIndex], frameWanted);

		if (grown) {
			frameArena[frameIndex] = grown;
			frameCap[frameIndex] = frameWanted;
		}
	}

	framePos = 0;
	frameWanted = 0;
}

static void *xblaMeshFrameAlloc(u32 size)
{
	void *ptr;

	size = (size + 15) & ~15u;
	frameWanted += size;

	if (framePos + size > frameCap[frameIndex]) {
		return NULL;
	}

	ptr = frameArena[frameIndex] + framePos;
	framePos += size;

	return ptr;
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
 * Poses one mesh into a copy of its vertices, and hands back the copy.
 *
 * Palette entry i is posed by whatever the game has done to the node carrying
 * part i, so the transform for it is: out of the bind pose (invbind), into the
 * game's pose for that bone (its matrix), and then back out of the matrix the
 * list is going to be drawn under, which is the first part's. The last step is
 * what keeps the vertices small enough to be the s16 a Perfect Dark vertex
 * holds - they come out in the first part's own space rather than the view's.
 *
 * NULL when there is no room this frame, and the caller draws the bind pose.
 */
static Vtx *xblaMeshPose(struct xblameshbuilt *m, struct xblameshuse *use,
		struct model *model, Mtxf *root)
{
	Mtxf pal[XBLAMESH_MAXMTX];
	Mtxf invroot;
	Vtx *out;
	s32 posable;

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
		out[i].x = xblaMeshRound(x);
		out[i].y = xblaMeshRound(y);
		out[i].z = xblaMeshRound(z);
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

		sysLogPrintf(LOG_NOTE, "xblamesh:   posed box [%d %d %d]..[%d %d %d]",
				lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
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
						? "" : "  <- the game moved this node's vertices");
	}
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

s32 xblaMeshRenderNode(struct modelrenderdata *renderdata, struct model *model,
		struct modelnode *node)
{
	struct xblameshentry *e;
	struct xblameshbuilt *m;
	struct xblameshuse *use;
	Mtxf *root;
	Vtx *posed;
	Gfx *list;

	if (!optEnabled || opened <= 0 || !node) {
		return 0;
	}

	// Only the opaque pass for now: nothing here reads the material's alpha
	// flag yet, so a mesh drawn again in the translucent pass would be drawn
	// twice.
	if (!(renderdata->flags & MODELRENDERFLAG_OPA)) {
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
	if (model && model->definition && model->definition != e->modeldef &&
			!xblaMeshNodeIsGrafted(model, node)) {
		return 0;
	}

	if (optOnlySlot && e->slot != optOnlySlot) {
		return 0;
	}

	// The mesh is built before the part is looked at, so that a mesh that will
	// not build leaves every part of the model drawing its own geometry rather
	// than only the first one.
	m = xblaMeshBuild(e->slot);

	if (!m) {
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
	} else if (e->part == 0) {
		list = &m->gdl[m->allgfx];
	} else {
		return 1;
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
			} else {
				pose = xblaMeshPose(m, use, model, root);

				// Not remembered when there was no room this frame, so that
				// the next part tries again rather than inheriting a miss.
				if (pose) {
					m->posedmodel = model;
					m->posedframe = frameCount;
					m->posedvtx = pose;
				}
			}

			if (pose) {
				posed = pose;
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
				m->nummatrices, posed == m->vertices ? "bind pose" : "posed");
	}

	if (root) {
		gSPMatrix(renderdata->gdl++, osVirtualToPhysical(root),
				G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
	}

	gSPSegment(renderdata->gdl++, SPSEGMENT_MODEL_VTX, osVirtualToPhysical(posed));
	gSPDisplayList(renderdata->gdl++, list);

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
}

s32 xblaMeshModelsAreLate(void)
{
	return openedLate;
}

PD_CONSTRUCTOR static void xblaMeshConfigInit(void)
{
	configRegisterInt("Mod.XblaMeshes", &optEnabled, 0, 1);

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

#else

void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid) { }
s32 xblaMeshRenderNode(struct modelrenderdata *renderdata, struct model *model,
		struct modelnode *node) { return 0; }
void xblaMeshFrameReset(void) { }
s32 xblaMeshIsAvailable(void) { return 0; }
s32 xblaMeshGetEnabled(void) { return 0; }
void xblaMeshSetEnabled(s32 enabled) { }
void xblaMeshResetModels(void) { }
s32 xblaMeshModelsAreLate(void) { return 0; }

#endif

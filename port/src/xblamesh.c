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
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "config.h"
#include "system.h"
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
	u8 *groupmtx;      // one per group: which palette entry that group's part is
	Mtxf *invbind;     // one per palette entry
	f32 *bindpos;      // three per emitted vertex
	f32 *weights;      // three per emitted vertex; only the first two are ever set
	u8 *bones;         // three per emitted vertex, and the count in the fourth
};

static s32 optEnabled;
static s32 optOnlySlot; // Mod.XblaMeshOnly: draw one mesh and leave the rest alone
static s32 optBoth;     // Mod.XblaMeshBoth: draw the game's geometry over it too
static s32 optTextures = 1; // Mod.XblaMeshTextures

/**
 * Mod.XblaMeshPose: drive a skinned mesh's palette from the game's matrices.
 *
 * Off, because it is not right yet and what it is not right about is written
 * down in xbla.md - the release's palette is not Perfect Dark's skeleton in a
 * space this can use. What is here is the harness the measurements came out
 * of, and the next attempt wants it rather than a blank page.
 */
static s32 optPose;
static s32 opened; // 0 untried, 1 open, -1 no package

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
static s32 xblaMeshOpen(void)
{
	const char *path;
	s32 index;
	u8 head[4];
	u8 *table;
	u32 tableLen;

	if (opened) {
		return opened > 0;
	}

	opened = -1;

	path = xblaImportGetStfsPath();

	if (!path || !path[0]) {
		return 0;
	}

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

static void xblaMeshForgetModel(const struct modeldef *modeldef)
{
	for (s32 i = 0; i < numUses; i++) {
		if (uses[i].modeldef == modeldef) {
			uses[i].modeldef = NULL;
		}
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

void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid)
{
	u8 *file;
	u32 len;
	s32 found;

	if (!optEnabled || !modeldef || !modeldef->rootnode) {
		return;
	}

	if (!xblaMeshOpen()) {
		return;
	}

	// Slot i is the game's file id i + 1.
	file = xblaMeshReadSlot((s32)fileid - 1, &len);

	if (!file) {
		return;
	}

	xblaMeshForgetModel(modeldef);

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
	}

	free(file);
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

	// position, then the UV pair, then a unit normal, then the colour.
	vtx->x = xblaMeshRound(xblaMeshBEF32(v));
	vtx->y = xblaMeshRound(xblaMeshBEF32(v + 4));
	vtx->z = xblaMeshRound(xblaMeshBEF32(v + 8));
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
	vtx->s = xblaMeshRound(xblaMeshBEF32(v + 12) * XBLATEX_TILE_SCALE);
	vtx->t = xblaMeshRound(xblaMeshBEF32(v + 16) * XBLATEX_TILE_SCALE);

	colour = xblaMeshBE32(v + 32);
	col->r = (u8)(colour >> 16);
	col->g = (u8)(colour >> 8);
	col->b = (u8)colour;
	col->a = 0xff;

	if (b->skinned) {
		// Two weights and a packed {bone0, bone1, bone2, count}. There are
		// only the two: they sum to 1.0 on every skinned vertex in the
		// release, so the third slot below is always zero and is kept only so
		// the arrays stay three wide. The count runs 1 to 6 against those two
		// weights, so it is not a count of weights; a rigid vertex repeats its
		// bone in all three bytes ({33,33,33,1}).
		f32 *pos = &b->bindpos[b->numvtx * 3];
		f32 *wt = &b->weights[b->numvtx * 3];
		u8 *bn = &b->bones[b->numvtx * 4];
		const u32 packed = xblaMeshBE32(v + 44);
		const s32 n = (s32)(packed & 0xff);

		pos[0] = xblaMeshBEF32(v);
		pos[1] = xblaMeshBEF32(v + 4);
		pos[2] = xblaMeshBEF32(v + 8);

		wt[0] = xblaMeshBEF32(v + 36);
		wt[1] = xblaMeshBEF32(v + 40);
		wt[2] = 1.0f - wt[0] - wt[1];

		bn[0] = (u8)(packed >> 24);
		bn[1] = (u8)(packed >> 16);
		bn[2] = (u8)(packed >> 8);
		bn[3] = (u8)(n < 1 ? 1 : (n > 3 ? 3 : n));

		if (bn[3] < 2) {
			wt[0] = 1.0f;
			wt[1] = 0.0f;
		}

		if (bn[3] < 3) {
			wt[2] = 0.0f;
		}
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
	const void *tile = optTextures ? xblaTexBind(record) : NULL;
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
static s32 xblaMeshBuildLists(struct xblameshbuilder *b, const u8 *file, u32 len,
		const struct xblameshhdr *h, u32 stride)
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

	for (u32 d = 0; d < h->numdraws; d++) {
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
		if (d == 0 || material != lastmaterial) {
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

	xblaMeshWriteBatches(b);

	return 1;
}

/**
 * The inverse of every bind matrix in the palette.
 *
 * The mesh stores a bone's bind pose as three rows of four floats with the
 * translation in the last column, which is the other convention from Perfect
 * Dark's Mtxf - that one is read by mtx4TransformVec as a row of basis vectors
 * with the translation in the fourth row. So the 3x3 is transposed on the way
 * in, and then inverted: a vertex has to come out of the bind pose before the
 * game's pose can be put on it.
 *
 * The rotations here are orthonormal, but this inverts by cofactors anyway - a
 * transpose that is wrong on a scaled bone would be a very quiet mistake.
 */
static s32 xblaMeshReadBind(struct xblameshbuilt *m, const u8 *file,
		const struct xblameshhdr *h)
{
	const u32 numgroups = (h->drawoffset - h->groupoffset) / XBLAMESH_ENTRY;

	if (h->nummatrices > XBLAMESH_MAXMTX || numgroups > XBLAMESH_MAXPARTS) {
		return 0;
	}

	m->invbind = calloc(h->nummatrices, sizeof(Mtxf));
	m->groupmtx = calloc(numgroups ? numgroups : 1, 1);

	if (!m->invbind || !m->groupmtx) {
		return 0;
	}

	m->nummatrices = (s32)h->nummatrices;
	m->numgroups = (s32)numgroups;

	// A group is one part of the model - a model's parts and a mesh's groups
	// come in the same order and the same number - and the group's third word
	// says which palette entry that part poses. That is the indirection the
	// part number on its own does not give: a Falcon 2's five parts are
	// palette entries 33, 38, 42, 40 and 33, nowhere near 0 to 4.
	for (u32 i = 0; i < numgroups; i++) {
		const u32 mtx = xblaMeshBE32(file + h->groupoffset + i * XBLAMESH_ENTRY + 8);

		m->groupmtx[i] = (u8)(mtx < h->nummatrices ? mtx : 0);
	}

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

		// The 3x3 is the bone's rotation the other way round already - the
		// game's matrix for a bone and this one cancel to a pure identity,
		// which is what says the two rigs are the same rig. The last column is
		// not its translation to match: it is the bone's position in the mesh,
		// which has to go through the rotation and change sign to become one.
		for (s32 c = 0; c < 3; c++) {
			dst->m[3][c] = -(row[c][0] * row[0][3] + row[c][1] * row[1][3] +
					row[c][2] * row[2][3]);
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

	if (b.skinned && !xblaMeshReadBind(m, file, &h)) {
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
	m->state = 1;

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

void xblaMeshFrameReset(void)
{
	if (!optEnabled) {
		return;
	}

	frameIndex ^= 1;

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

	// Entry i of the mesh's palette is matrix i of the model. Not the part
	// number, which only looked like it because a small model has few of both:
	// a Falcon 2's nodes load matrices 33, 36, 38, 40 and 42 and its mesh's
	// palette is 43 entries, and Dr Carroll's nodes load 0 to 3 against a
	// palette of exactly 4. Over every model that names a mesh, 242 of 243
	// have every one of their nodes' matrix indexes inside the palette.
	posable = m->nummatrices;

	if (posable > model->definition->nummatrices) {
		posable = model->definition->nummatrices;
	}

	for (s32 i = 0; i < m->nummatrices; i++) {
		Mtxf step;

		// An entry the model has no matrix for keeps its bind pose, which is
		// where the mesh already has it.
		if (i >= posable) {
			mtx4LoadIdentity(&pal[i]);
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
				"model's %d matrices", m->nummatrices, posable,
				model->definition->nummatrices);

		// If the two rigs are the same rig then every entry's transform comes
		// out near the identity for a model standing still: the game's matrix
		// for a bone undoes the mesh's bind for it, leaving only the root.
		for (s32 i = posable > 10 ? posable - 10 : 0; i < posable; i++) {
			// What the bind's translation would have to be for this entry to
			// come out as the identity, against what the file has.
			Mtxf inv;
			struct coord want;
			struct coord in;

			xblaMeshInvert(&model->matrices[i], &inv);

			in.x = root->m[3][0] - model->matrices[i].m[3][0];
			in.y = root->m[3][1] - model->matrices[i].m[3][1];
			in.z = root->m[3][2] - model->matrices[i].m[3][2];
			mtx4RotateVec(&inv, &in, &want);

			sysLogPrintf(LOG_NOTE, "xblamesh:     pal %2d  diag %.3f %.3f %.3f"
					"  have %.2f %.2f %.2f  want %.2f %.2f %.2f", i,
					pal[i].m[0][0], pal[i].m[1][1], pal[i].m[2][2],
					m->invbind[i].m[3][0], m->invbind[i].m[3][1], m->invbind[i].m[3][2],
					want.x, want.y, want.z);
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

		for (s32 j = 0; j < bone[3]; j++) {
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

s32 xblaMeshRenderNode(struct modelrenderdata *renderdata, struct model *model,
		struct modelnode *node)
{
	struct xblameshentry *e;
	struct xblameshbuilt *m;
	Mtxf *root;
	Vtx *posed;

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
	// drawn from is what says this entry is about this model.
	if (model && model->definition && model->definition != e->modeldef) {
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

	// One mesh stands in for the whole model, and every part of the model
	// carries its id - Dr Carroll's 13 nodes all name the same one. Drawing it
	// at each of them draws the model over itself once per part, every copy
	// under a different bone's matrix, which is what turned a room into
	// overlapping sheets. Only the first part draws it; the rest draw nothing,
	// because the mesh already has their geometry in it.
	if (e->part != 0) {
		return 1;
	}

	// The matrix this is drawn under: the one the node's own list would have
	// loaded out of segment 3. A posed mesh needs it named as well as loaded,
	// because its vertices come out in that matrix's space - which is what
	// keeps them inside the s16 a Perfect Dark vertex holds.
	posed = m->vertices;
	root = NULL;

	if (e->use >= 0 && e->use < numUses && uses[e->use].modeldef == e->modeldef) {
		struct xblameshuse *use = &uses[e->use];

		root = xblaMeshPartMtx(model, use, 0);

		if (optPose && m->nummatrices && root) {
			Vtx *pose = xblaMeshPose(m, use, model, root);

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
				"%d palette entries, %s",
				xblaMeshDrawLog, e->slot, node, model, m->nummatrices,
				posed == m->vertices ? "bind pose" : "posed");
	}

	if (root) {
		gSPMatrix(renderdata->gdl++, osVirtualToPhysical(root),
				G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
	}

	gSPSegment(renderdata->gdl++, SPSEGMENT_MODEL_VTX, osVirtualToPhysical(posed));
	gSPDisplayList(renderdata->gdl++, m->gdl);

	// Mod.XblaMeshBoth: draw the game's geometry as well, so the two can be
	// seen on top of each other. The only way to tell a mesh that is in the
	// wrong place from one that is the wrong size.
	return optBoth ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------- */

s32 xblaMeshIsAvailable(void)
{
	const char *path = xblaImportGetStfsPath();

	return path && path[0];
}

s32 xblaMeshGetEnabled(void)
{
	return optEnabled;
}

void xblaMeshSetEnabled(s32 enabled)
{
	optEnabled = enabled ? 1 : 0;
}

PD_CONSTRUCTOR static void xblaMeshConfigInit(void)
{
	configRegisterInt("Mod.XblaMeshes", &optEnabled, 0, 1);

	// Debugging one mesh at a time: everything else keeps its own geometry, so
	// what is on screen is the game's except for the one thing being looked at
	configRegisterInt("Mod.XblaMeshOnly", &optOnlySlot, 0, 0xffff);
	configRegisterInt("Mod.XblaMeshBoth", &optBoth, 0, 1);
	configRegisterInt("Mod.XblaMeshPose", &optPose, 0, 1);

	// The release's own art, drawn on the release's own geometry. On, because
	// an untextured mesh is a flat pale solid and is not what anyone is
	// turning Mod.XblaMeshes on to see; off is for telling a shape that is
	// wrong from a texture that is.
	configRegisterInt("Mod.XblaMeshTextures", &optTextures, 0, 1);
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

#endif

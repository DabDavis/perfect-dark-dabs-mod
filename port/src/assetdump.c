/**
 * The asset dump. See assetdump.h for what it writes and where.
 *
 * A state machine stepped from the frame tick: one texture, one record, one
 * model or one mesh per step, as many steps a frame as fit in the budget.
 * Nothing here is on a thread of its own, because the texture decoder and
 * the file loader are the game's and are not made to share.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "files.h"
#include "system.h"
#include "fs.h"
#include "romdata.h"
#include "texpack.h"
#include "xblaimport.h"
#include "xblatex.h"
#include "xblamesh.h"
#include "objmesh.h"
#include "assetdump.h"
#include "game/file.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "lib/model.h"

#ifndef PLATFORM_N64

#define ASSETDUMP_DIR "model-dumps"
#define ASSETDUMP_N64_SUB "n64"
#define ASSETDUMP_XBLA_SUB "xbla"

// Microseconds of a frame given to the dump when it runs from the menu.
#define ASSETDUMP_BUDGET 12000

// Enough for the largest texture in the ROM plus the tex that describes it.
#define ASSETDUMP_TEXPOOL (128 * 1024)

// A model's list nodes are numbered in xblaMeshEnumListNodes()'s order; a
// model has at most a few dozen.
#define ASSETDUMP_MAXNODES 256

// Vertices one G_VTX can load, which is the vertex cache the triangles index.
#define ASSETDUMP_CACHE 32

enum {
	PHASE_IDLE,
	PHASE_TEXTURES,
	PHASE_RECORDS,
	PHASE_MODELS,
	PHASE_MESHES,
	PHASE_DONE,
};

static s32 phase = PHASE_IDLE;
static s32 index;
static s32 total;
static s32 haveXbla;
static s32 numTextures, numRecords, numModels, numMeshes, numRefused, numUnnamed;
static u64 phaseStart; // for the log: how long each pass took
static char status[128];
static char modelDir[FS_MAXPATH + 1];   // model-dumps, expanded
static char texDir[FS_MAXPATH + 1];     // texture-dumps/<romid>, expanded
static char texRel[FS_MAXPATH + 1];     // the same as an MTL from model-dumps/n64/ sees it

// What is known about each texture number as the models are dumped: the
// padded size a coordinate is measured against, whether the coordinates were
// halved at load, and whether the picture has alpha at all.
struct assetdumptex {
	s16 width;
	s16 height;
	u8 known;
	u8 halve;
	u8 alpha;
	u8 fmt;
	u8 siz;
};

static struct assetdumptex *texInfo;
static u8 *texPool;

static void assetDumpLogPhase(const char *what, s32 count)
{
	const u64 now = sysGetMicroseconds();

	sysLogPrintf(LOG_NOTE, "assetdump: %s: %d in %.1f s", what, count, (now - phaseStart) / 1000000.0);
	phaseStart = now;
}

static void assetDumpSetStatus(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(status, sizeof(status), fmt, ap);
	va_end(ap);
}

/* -------------------------------------------------------------------------
 * Where it goes
 * ------------------------------------------------------------------------- */

static s32 assetDumpMakeDir(const char *path)
{
	return fsFileSize(path) >= 0 || fsCreateDir(path) == 0;
}

/**
 * model-dumps/ with n64/ and xbla/ inside it, and how the texture dump is
 * reached from there. Both directories come from fsChooseOutputDir(), so
 * they are normally under one root and the MTL can say ../../texture-dumps;
 * when they are not, the MTL says the whole path.
 */
static s32 assetDumpOpenDirs(void)
{
	char rel[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];
	const char *tex;
	const char *romid;

	if (fsChooseOutputDir(ASSETDUMP_DIR, rel, sizeof(rel)) != 0) {
		sysLogPrintf(LOG_ERROR, "assetdump: nowhere to write " ASSETDUMP_DIR);
		return 0;
	}

	snprintf(modelDir, sizeof(modelDir), "%s", fsFullPath(rel));

	snprintf(sub, sizeof(sub), "%s/" ASSETDUMP_N64_SUB, rel);

	if (!assetDumpMakeDir(sub)) {
		sysLogPrintf(LOG_ERROR, "assetdump: could not create %s", fsFullPath(sub));
		return 0;
	}

	snprintf(sub, sizeof(sub), "%s/" ASSETDUMP_XBLA_SUB, rel);

	if (!assetDumpMakeDir(sub)) {
		sysLogPrintf(LOG_ERROR, "assetdump: could not create %s", fsFullPath(sub));
		return 0;
	}

	tex = texpackGetDumpDir();

	if (!tex) {
		sysLogPrintf(LOG_ERROR, "assetdump: nowhere to write the textures");
		return 0;
	}

	snprintf(texDir, sizeof(texDir), "%s", tex);

	// texture-dumps/<romid> is the last two names of the texture path; the
	// two dumps share a root when the model path's parent is the same as
	// the texture path's grandparent.
	romid = strrchr(texDir, '/');

	if (romid && strrchr(modelDir, '/')) {
		const size_t modelroot = (size_t)(strrchr(modelDir, '/') - modelDir);
		const char *texname = NULL;
		size_t texroot = 0;

		for (const char *p = texDir; p < romid; p++) {
			if (*p == '/') {
				texname = p;
			}
		}

		if (texname) {
			texroot = (size_t)(texname - texDir);
		}

		if (texname && texroot == modelroot && !strncmp(texDir, modelDir, texroot)) {
			snprintf(texRel, sizeof(texRel), "../..%s", texname);
		} else {
			snprintf(texRel, sizeof(texRel), "%s", texDir);
		}
	} else {
		snprintf(texRel, sizeof(texRel), "%s", texDir);
	}

	return 1;
}

/* -------------------------------------------------------------------------
 * The ROM's models
 * ------------------------------------------------------------------------- */

static u32 assetDumpBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/**
 * What the port's converter would fatally refuse, refused quietly here
 * first: the file's own header, read big-endian before any conversion. The
 * converter checks more than this, but every stock file that passes this is
 * a model.
 */
static s32 assetDumpLooksLikeModel(const u8 *raw, u32 len)
{
	u32 root, parts, texconfigs;
	u16 numparts;
	u16 type;

	if (len < 28) {
		return 0;
	}

	root = assetDumpBE32(raw);
	parts = assetDumpBE32(raw + 8);
	texconfigs = assetDumpBE32(raw + 24);
	numparts = (u16)((raw[12] << 8) | raw[13]);

	if ((root >> 24) != 0x05 || (root & 0xffffff) + 24 > len) {
		return 0;
	}

	if (parts && ((parts >> 24) != 0x05 || (parts & 0xffffff) + (u32)numparts * 4 > len)) {
		return 0;
	}

	if (texconfigs && ((texconfigs >> 24) != 0x05 || (texconfigs & 0xffffff) > len)) {
		return 0;
	}

	type = (u16)((raw[(root & 0xffffff)] << 8) | raw[(root & 0xffffff) + 1]) & 0xff;

	return type == MODELNODETYPE_CHRINFO || type == MODELNODETYPE_POSITION ||
			type == MODELNODETYPE_GUNDL || type == MODELNODETYPE_DL ||
			type == MODELNODETYPE_DISTANCE || type == MODELNODETYPE_TOGGLE ||
			type == MODELNODETYPE_BBOX || type == MODELNODETYPE_REORDER ||
			type == MODELNODETYPE_HEADSPOT || type == MODELNODETYPE_POSITIONHELD ||
			type == MODELNODETYPE_0B || type == MODELNODETYPE_0D ||
			type == MODELNODETYPE_0E || type == MODELNODETYPE_0F ||
			type == MODELNODETYPE_11 || type == MODELNODETYPE_05 ||
			type == MODELNODETYPE_CHRGUNFIRE || type == MODELNODETYPE_STARGUNFIRE;
}

/**
 * Loads one model file into a buffer of this file's own, converted to the
 * port's layout and with its pointers made real, and nothing else - no
 * textures loaded, no meshes matched. The texture commands are still the
 * ROM's compact ones with the texture number in them, which is what makes
 * the file worth reading in this state.
 */
static struct modeldef *assetDumpLoadModel(s32 fileid, s32 gun, u8 **outBuffer)
{
	const u32 loadtype = gun ? LOADTYPE_GUN : LOADTYPE_MODEL;
	struct modeldef *modeldef;
	u32 size;
	u32 rawsize;
	u8 *buffer;

	size = fileGetInflatedSize(fileid, loadtype);

	if (size == 0) {
		return NULL;
	}

	size = ((size + 0x20) & ~0xfu) + 0x8000;
	buffer = malloc(size);

	if (!buffer) {
		return NULL;
	}

	// The file as the ROM holds it, to look at before the converter does:
	// the converter stops the game on anything that is not a model.
	g_LoadType = LOADTYPE_NONE;
	fileLoadToAddr(fileid, FILELOADMETHOD_EXTRAMEM, buffer, size);
	rawsize = fileGetLoadedSize(fileid);

	if (rawsize == 0 || !assetDumpLooksLikeModel(buffer, rawsize)) {
		free(buffer);
		return NULL;
	}

	g_LoadType = (u8)loadtype;
	fileLoadToAddr(fileid, FILELOADMETHOD_EXTRAMEM, buffer, size);

	if (fileGetLoadedSize(fileid) == 0) {
		free(buffer);
		return NULL;
	}

	modeldef = (struct modeldef *)buffer;
	modelPromoteOffsetsToPointers(modeldef, 0x05000000, (uintptr_t)buffer);

	*outBuffer = buffer;

	return modeldef;
}

/** The texture's size and flags, decoded once and remembered. */
static const struct assetdumptex *assetDumpTexInfo(s32 texturenum)
{
	struct assetdumptex *info;
	struct texpool pool;
	struct tex *tex;

	if (texturenum < 0 || texturenum >= NUM_TEXTURES || !texInfo || !texPool) {
		return NULL;
	}

	info = &texInfo[texturenum];

	if (info->known) {
		return info->width > 0 ? info : NULL;
	}

	info->known = 1;

	texInitPool(&pool, texPool, ASSETDUMP_TEXPOOL);
	texLoadFromTextureNum((u32)texturenum, &pool);

	tex = texFindInPool(texturenum, &pool);

	if (tex && tex->data) {
		s32 width = 0;
		s32 height = 0;

		if (texpackTexGetPaddedSize(tex, &width, &height)) {
			u8 *rgba = texpackTexToRgba(tex, &width, &height);

			info->width = (s16)width;
			info->height = (s16)height;
			info->halve = tex->unk0c_03 ? 1 : 0;
			info->fmt = (u8)tex->gbiformat;
			info->siz = (u8)tex->depth;

			if (rgba) {
				for (s32 i = 0; i < width * height; i++) {
					if (rgba[i * 4 + 3] < 0xf0) {
						info->alpha = 1;
						break;
					}
				}

				free(rgba);
			}
		}
	}

	texpackForgetRange(texPool, texPool + ASSETDUMP_TEXPOOL);

	return info->width > 0 ? info : NULL;
}

static const char *assetDumpFormatName(u32 fmt, u32 siz)
{
	static const char *const fmts[] = { "rgba", "yuv", "ci", "ia", "i" };
	static const char *const sizs[] = { "4", "8", "16", "32" };
	static char name[16];

	snprintf(name, sizeof(name), "%s%s", fmt < 5 ? fmts[fmt] : "x", siz < 4 ? sizs[siz] : "");

	return name;
}

/**
 * The material for a texture number: named for it, pointing at the dump's
 * PNG of it.
 */
static s32 assetDumpN64Material(struct objmesh *m, s32 texturenum)
{
	const struct assetdumptex *info = assetDumpTexInfo(texturenum);
	char name[OBJMESH_NAMELEN];
	char image[FS_MAXPATH + 1];

	if (texturenum < 0) {
		return -1;
	}

	snprintf(name, sizeof(name), "n64_%04x", texturenum);

	if (!info) {
		return objmeshAddMaterial(m, name, OBJMAT_N64, (u32)texturenum, 0, NULL);
	}

	snprintf(image, sizeof(image), "%s/%04x_%s.png", texRel, texturenum,
			assetDumpFormatName(info->fmt, info->siz));

	return objmeshAddMaterial(m, name, OBJMAT_N64, (u32)texturenum, info->alpha, image);
}

// One list node being read: where its vertices and colours are, the batch
// the last G_VTX loaded, and which OBJ vertex each loaded one became.
struct assetdumplist {
	struct objmesh *m;
	const u8 *buffer;
	const Vtx *vertices;
	s32 numvertices;
	f32 offset[3];
	s32 texturenum;
	s32 material;
	s32 draw;            // the open draw, or -1
	const Col *colours;  // the table the last G_COL loaded
	s32 numcolours;
	const Vtx *cache[ASSETDUMP_CACHE];
	s32 cacheobj[ASSETDUMP_CACHE];
	s32 numemitted;
	s32 numtris;
};

/**
 * A list pointer as the file holds it after promotion: the vertices were
 * made real, the display lists were not - they stay segment 5 addresses
 * until the texture rewrite the dump skips would have replaced them.
 */
static const Gfx *assetDumpResolveGdl(const u8 *buffer, const Gfx *gdl)
{
	const uintptr_t addr = (uintptr_t)gdl;

	if (!gdl) {
		return NULL;
	}

	if ((addr & 1) || ((UNSEGADDR(addr) >> 24) & 0xff) == 0x05) {
		return (const Gfx *)(buffer + (UNSEGADDR(addr) & 0xffffff));
	}

	return gdl;
}

/** The Vtx a segmented address names, for this node. */
static const Vtx *assetDumpResolveVtx(const struct assetdumplist *l, uintptr_t addr)
{
	const u32 seg = (u32)((UNSEGADDR(addr) >> 24) & 0xf);
	const u32 offset = (u32)(UNSEGADDR(addr) & 0xffffff);

	if (seg == SPSEGMENT_MODEL_VTX) {
		return (const Vtx *)((const u8 *)l->vertices + offset);
	}

	return (const Vtx *)(l->buffer + offset);
}

static s32 assetDumpEmitVertex(struct assetdumplist *l, s32 slot)
{
	const Vtx *v;
	struct objvertex ov;
	const struct assetdumptex *info;
	s32 added;

	if (slot < 0 || slot >= ASSETDUMP_CACHE || !l->cache[slot]) {
		return -1;
	}

	if (l->cacheobj[slot] >= 0) {
		return l->cacheobj[slot];
	}

	v = l->cache[slot];
	memset(&ov, 0, sizeof(ov));
	ov.pos[0] = v->x + l->offset[0];
	ov.pos[1] = v->y + l->offset[1];
	ov.pos[2] = v->z + l->offset[2];
	ov.nrm[1] = 1.0f;
	ov.weight[0] = 1.0f;
	ov.rgba[0] = ov.rgba[1] = ov.rgba[2] = ov.rgba[3] = 0xff;

	// A vertex names its colour by a byte offset into the table the list
	// last loaded.
	if (l->colours && (v->colour >> 2) < l->numcolours) {
		const Col *c = &l->colours[v->colour >> 2];
		ov.rgba[0] = c->r;
		ov.rgba[1] = c->g;
		ov.rgba[2] = c->b;
		ov.rgba[3] = c->a;
	}

	// s and t are texels of the tile in 10.5, against the padded row the
	// picture was written with; a texture the loader halves them for is
	// measured at twice the size here, so the file's numbers come out the
	// same as what the game draws.
	info = assetDumpTexInfo(l->texturenum);

	if (info) {
		const f32 div = info->halve ? 2.0f : 1.0f;
		ov.uv[0] = (f32)v->s / 32.0f / div / (f32)info->width;
		ov.uv[1] = (f32)v->t / 32.0f / div / (f32)info->height;
	}

	added = objmeshAddVertex(l->m, &ov);
	l->cacheobj[slot] = added;
	l->numemitted++;

	return added;
}

static void assetDumpTriangle(struct assetdumplist *l, s32 a, s32 b, s32 c)
{
	const s32 va = assetDumpEmitVertex(l, a);
	const s32 vb = assetDumpEmitVertex(l, b);
	const s32 vc = assetDumpEmitVertex(l, c);

	if (va < 0 || vb < 0 || vc < 0) {
		return;
	}

	if (l->draw < 0 || l->m->draws[l->draw].material != l->material) {
		l->draw = objmeshAddDraw(l->m, l->m->numtris, 0, l->material);

		if (l->draw < 0) {
			return;
		}
	}

	if (objmeshAddTriangle(l->m, (u32)va, (u32)vb, (u32)vc) >= 0) {
		l->m->draws[l->draw].numtris++;
		l->numtris++;
	}
}

/**
 * Walks one display list, in the port's layout, for its triangles.
 *
 * What it reads: the ROM's compact texture command (0xc0, texture number in
 * the low twelve bits of its second word), the vertex loads, the colour
 * table loads, the two triangle commands, and calls and branches to other
 * lists. Everything else is state the renderer wants and a mesh does not.
 */
static void assetDumpWalkList(struct assetdumplist *l, const Gfx *gdl, s32 depth)
{
	s32 steps = 0;

	if (!gdl || depth > 8) {
		return;
	}

	while (steps++ < 100000) {
		const u32 w0 = (u32)gdl->words.w0;
		const uintptr_t w1 = gdl->words.w1;
		const u8 cmd = (u8)(w0 >> 24);

		switch (cmd) {
		case G_NOOP: // the texture command, repurposed
			l->texturenum = (s32)(w1 & 0xfff);
			l->material = assetDumpN64Material(l->m, l->texturenum);
			break;
		case G_VTX: {
			const s32 n = (s32)((w0 & 0xffff) / sizeof(Vtx));
			const s32 v0 = (s32)((w0 >> 16) & 0xf);
			const Vtx *v = assetDumpResolveVtx(l, w1);

			for (s32 i = 0; i < n && v0 + i < ASSETDUMP_CACHE; i++) {
				l->cache[v0 + i] = &v[i];
				l->cacheobj[v0 + i] = -1;
			}
			break;
		}
		case G_COL:
			l->colours = (const Col *)(l->buffer + (UNSEGADDR(w1) & 0xffffff));
			l->numcolours = (s32)((w0 & 0xffff) / 4);

			// A new table means the cached vertices' colours are stale.
			for (s32 i = 0; i < ASSETDUMP_CACHE; i++) {
				l->cacheobj[i] = -1;
			}
			break;
		case (u8)G_TRI1:
			assetDumpTriangle(l, (s32)((w1 >> 16) & 0xff) / 10, (s32)((w1 >> 8) & 0xff) / 10,
					(s32)(w1 & 0xff) / 10);
			break;
		case (u8)G_TRI4: {
			const u32 lo = (u32)w1;

			for (s32 t = 0; t < 4; t++) {
				const s32 x = (s32)((lo >> (t * 8)) & 0xf);
				const s32 y = (s32)((lo >> (t * 8 + 4)) & 0xf);
				const s32 z = (s32)((w0 >> (t * 4)) & 0xf);

				if (x || y || z) {
					assetDumpTriangle(l, x, y, z);
				}
			}
			break;
		}
		case G_DL: {
			const Gfx *target = (const Gfx *)(l->buffer + (UNSEGADDR(w1) & 0xffffff));

			if (((w0 >> 16) & 1) == 0) {
				assetDumpWalkList(l, target, depth + 1);
			} else {
				gdl = target;
				continue;
			}
			break;
		}
		case (u8)G_ENDDL:
			return;
		}

		gdl++;
	}
}

/**
 * One model as an OBJ: a group per list node, in xblaMeshEnumListNodes()'s
 * order and named for its place in it, each moved to where the model's rest
 * position nodes put it, so the file opens as the whole model and the pack
 * loader can take each group back to its node.
 */
static s32 assetDumpModel(s32 fileid, const char *name)
{
	struct modelnode *nodes[ASSETDUMP_MAXNODES];
	struct modeldef *modeldef;
	struct objmesh *m;
	char path[FS_MAXPATH + 1];
	char comment[512];
	u8 *buffer = NULL;
	s32 n;
	s32 written = 0;

	modeldef = assetDumpLoadModel(fileid, name[0] == 'G', &buffer);

	if (!modeldef) {
		return 0;
	}

	m = objmeshAlloc(name);

	if (!m) {
		free(buffer);
		return 0;
	}

	n = xblaMeshEnumListNodes(modeldef, nodes, ASSETDUMP_MAXNODES);

	if (n > ASSETDUMP_MAXNODES) {
		n = ASSETDUMP_MAXNODES;
	}

	for (s32 k = 0; k < n; k++) {
		struct assetdumplist l;
		const struct modelnode *node = nodes[k];
		const u32 type = node->type & 0xff;
		char gname[OBJMESH_NAMELEN];
		s32 group;
		const Gfx *opa;
		const Gfx *xlu;

		memset(&l, 0, sizeof(l));
		l.m = m;
		l.buffer = buffer;
		l.texturenum = -1;
		l.material = -1;
		l.draw = -1;

		for (s32 i = 0; i < ASSETDUMP_CACHE; i++) {
			l.cacheobj[i] = -1;
		}

		if (type == MODELNODETYPE_DL) {
			l.vertices = node->rodata->dl.vertices;
			l.numvertices = node->rodata->dl.numvertices;
			opa = node->rodata->dl.opagdl;
			xlu = node->rodata->dl.xlugdl;
		} else {
			l.vertices = node->rodata->gundl.vertices;
			l.numvertices = node->rodata->gundl.numvertices;
			opa = node->rodata->gundl.opagdl;
			xlu = node->rodata->gundl.xlugdl;
		}

		xblaMeshNodeRestOffset(node, l.offset);

		snprintf(gname, sizeof(gname), "node%d", k);
		group = objmeshAddGroup(m, gname, m->numdraws, 0);

		if (group < 0) {
			break;
		}

		assetDumpWalkList(&l, assetDumpResolveGdl(buffer, opa), 0);

		// The translucent list is its own draw run, after the opaque ones.
		l.draw = -1;
		assetDumpWalkList(&l, assetDumpResolveGdl(buffer, xlu), 0);

		m->groups[group].numdraws = m->numdraws - m->groups[group].firstdraw;
	}

	if (m->numtris > 0) {
		snprintf(path, sizeof(path), "%s/" ASSETDUMP_N64_SUB "/%s.obj", modelDir, name);
		snprintf(comment, sizeof(comment),
				"model %s, file id 0x%04x, %d list nodes\n"
				"a group per list node (node0..), in the order the pack loader numbers them,\n"
				"each at the model's rest position; textures are n64_<number> in %s",
				name, fileid, n, texRel);
		written = objmeshWrite(m, path, comment);
	}

	objmeshFree(m);
	free(buffer);

	return written;
}

/* -------------------------------------------------------------------------
 * The release's meshes
 * ------------------------------------------------------------------------- */

/**
 * The mesh table is in the model names' own order.
 *
 * Slots 2021 to 2615 are the meshes, and taking the name of each one's model
 * in slot order gives a sorted list: the props, then the characters, then the
 * guns, and inside a group the name uppercased with the ROM's trailing Z
 * dropped (`Pa51wastebinZ` before `Pa51_crate1Z`, since `_` sorts after `Z`;
 * `Pg5_chairZ` before `Pg5_chair2Z`). That holds for all 556 slots a model
 * names, with no exception - so 4J built the table by walking their model
 * list in order, and where a slot is is where its model's name sorts.
 *
 * Which is the only thing there is to say about the 39 slots **no** model
 * names. They are not leftovers: they are meshes for names the NTSC ROM does
 * not have (six more heads between `Cheadelvis_gogsZ` and `Cheadfem_guardZ`,
 * eleven more pairs of hands, a dozen props), so 4J's build had models this
 * one does not. Nothing in the game can reach them - the id that names a mesh
 * is written on a model's nodes, and no model here carries these - so they
 * are dumped to be looked at and cannot be replaced by a pack.
 *
 * Rather than `slotNNNN`, such a mesh is named for where it sorts:
 * `Ghand_a51guardZ+1` is the first mesh after `Ghand_a51guardZ`'s, and the
 * files land beside the ones they belong between in a directory listing.
 */
static char meshPrevName[OBJMESH_NAMELEN];
static s32 meshSincePrev;

/** The ROM's name for the model whose nodes name this mesh, or NULL. */
static const char *assetDumpMeshModelName(s32 slot)
{
	const s32 fileid = xblaMeshSlotModelFile(slot);
	const char *name = fileid ? romdataFileGetName(fileid) : NULL;

	return name && name[0] ? name : NULL;
}

/** The next mesh along that a model does name, for the "before" half. */
static const char *assetDumpMeshNextName(s32 slot, s32 numslots)
{
	for (s32 i = slot + 1; i < numslots; i++) {
		const char *name = assetDumpMeshModelName(i);

		if (name) {
			return name;
		}
	}

	return NULL;
}

static s32 assetDumpMesh(s32 slot, s32 numslots)
{
	const s32 fileid = xblaMeshSlotModelFile(slot);
	const char *filename = assetDumpMeshModelName(slot);
	const char *nextname = NULL;
	char name[OBJMESH_NAMELEN];
	char path[FS_MAXPATH + 1];
	char comment[768];
	char where[320];
	char sorts[160];
	struct objmesh *m;
	s32 written;

	where[0] = '\0';

	// Most of the package is not a mesh - the model files come first, and a
	// record can be an unused slot - so the name is settled after the read
	// rather than before it: the counting is of meshes, not of slots.
	m = xblaMeshSlotToObj(slot, filename ? filename : "");

	if (!m) {
		return 0;
	}

	if (filename) {
		snprintf(name, sizeof(name), "%s", filename);
		snprintf(meshPrevName, sizeof(meshPrevName), "%s", filename);
		meshSincePrev = 0;
	} else {
		nextname = assetDumpMeshNextName(slot, numslots);
		meshSincePrev++;
		numUnnamed++;

		if (meshPrevName[0]) {
			snprintf(name, sizeof(name), "%s+%d", meshPrevName, meshSincePrev);
		} else if (nextname) {
			// Before the first mesh a model names, so count off the one after.
			snprintf(name, sizeof(name), "%s-%d", nextname, meshSincePrev);
		} else {
			snprintf(name, sizeof(name), "slot%04d", slot);
		}

		if (meshPrevName[0] && nextname) {
			snprintf(sorts, sizeof(sorts), "after %s's and before %s's", meshPrevName, nextname);
		} else if (nextname) {
			snprintf(sorts, sizeof(sorts), "before %s's, first in the table", nextname);
		} else {
			snprintf(sorts, sizeof(sorts), "after %s's, last in the table", meshPrevName);
		}

		snprintf(where, sizeof(where),
				"\nno model of this ROM names this mesh: it is one 4J's build had and this one has not.\n"
				"The table is in the model names' order, and this slot sorts %s.\n"
				"Nothing draws it and a model pack cannot replace it - it is dumped to be looked at",
				sorts);

		snprintf(m->name, sizeof(m->name), "%s", name);
	}

	for (u32 i = 0; i < m->nummaterials; i++) {
		struct objmaterial *mat = &m->materials[i];

		if (mat->kind == OBJMAT_XBLA) {
			snprintf(mat->image, sizeof(mat->image), "%s/xbla/%04x.png", texRel, mat->id);
		}
	}

	snprintf(path, sizeof(path), "%s/" ASSETDUMP_XBLA_SUB "/%s.obj", modelDir, name);
	snprintf(comment, sizeof(comment),
			"XBLA mesh for %s: PackedSegFile slot %d, named by model file id 0x%04x, %u groups, %u palette entries, header scale %g\n"
			"a group per part of the model (part0..); textures are xbla_<record> in %s/xbla%s",
			name, slot, fileid, m->numgroups, m->nummatrices, m->headerscale, texRel, where);
	written = objmeshWrite(m, path, comment);
	objmeshFree(m);

	return written;
}

/* -------------------------------------------------------------------------
 * Driving it
 * ------------------------------------------------------------------------- */

static void assetDumpFinish(void)
{
	texpackDumpClose();
	free(texInfo);
	free(texPool);
	texInfo = NULL;
	texPool = NULL;
	phase = PHASE_DONE;

	assetDumpSetStatus("Wrote %d textures, %d XBLA textures, %d models, %d XBLA meshes",
			numTextures, numRecords, numModels, numMeshes);
	sysLogPrintf(LOG_NOTE, "assetdump: %s; textures in %s, models in %s%s", status, texDir, modelDir,
			haveXbla ? "" : " (no XBLA package, so no XBLA textures or meshes)");

	if (numRefused) {
		sysLogPrintf(LOG_NOTE, "assetdump: %d files with a model's name were not models and were left out", numRefused);
	}

	if (numUnnamed) {
		sysLogPrintf(LOG_NOTE, "assetdump: %d meshes no model of this ROM names, written as <the mesh before it>+N", numUnnamed);
	}
}

void assetDumpStart(void)
{
	if (phase != PHASE_IDLE && phase != PHASE_DONE) {
		return;
	}

	numTextures = numRecords = numModels = numMeshes = numRefused = numUnnamed = 0;
	meshPrevName[0] = '\0';
	meshSincePrev = 0;
	index = 0;
	total = 0;

	if (!assetDumpOpenDirs() || !texpackDumpOpen()) {
		phase = PHASE_DONE;
		assetDumpSetStatus("Nowhere to write to - see the log");
		return;
	}

	texInfo = calloc(NUM_TEXTURES, sizeof(*texInfo));
	texPool = malloc(ASSETDUMP_TEXPOOL);

	if (!texInfo || !texPool) {
		assetDumpFinish();
		return;
	}

	haveXbla = xblaImportIsAvailable();
	phaseStart = sysGetMicroseconds();
	phase = PHASE_TEXTURES;
	total = NUM_TEXTURES;
	assetDumpSetStatus("Textures: 0 of %d", total);
	sysLogPrintf(LOG_NOTE, "assetdump: starting; textures to %s, models to %s", texDir, modelDir);
}

/** One step: one texture, record, model or mesh. */
static void assetDumpStep(void)
{
	switch (phase) {
	case PHASE_TEXTURES:
		if (index < NUM_TEXTURES) {
			numTextures += texpackDumpTextureNum(index);
			index++;
			assetDumpSetStatus("Textures: %d of %d", index, NUM_TEXTURES);
		} else {
			texpackDumpClose();
			assetDumpLogPhase("textures", numTextures);
			phase = PHASE_RECORDS;
			index = 0;
			// Opens the package, which unpacks the archive if that has not
			// happened yet - a few seconds, once.
			total = haveXbla ? (s32)xblaTexGetNumRecords() : 0;
		}
		break;
	case PHASE_RECORDS:
		if (index < total) {
			s32 width = 0;
			s32 height = 0;
			u8 *rgba = xblaTexDecodeRecord((u32)index, &width, &height);

			if (rgba) {
				numRecords += texpackWriteXblaRecord(rgba, (u32)width, (u32)height, (u32)index);
				free(rgba);
			}

			index++;
			assetDumpSetStatus("XBLA textures: %d of %d", index, total);
		} else {
			assetDumpLogPhase("XBLA textures", numRecords);
			phase = PHASE_MODELS;
			index = 1;
			total = NUM_FILES;
		}
		break;
	case PHASE_MODELS:
		if (index < NUM_FILES) {
			const char *name = romdataFileGetName(index);

			// The characters, the props and the guns are the ROM's models,
			// named C, P and G; what else has those initials is caught by
			// the header check in assetDumpLoadModel().
			if (name && (name[0] == 'C' || name[0] == 'P' || name[0] == 'G')) {
				if (assetDumpModel(index, name)) {
					numModels++;
				} else {
					numRefused++;
					sysLogPrintf(LOG_NOTE, "assetdump: file %d (%s) is not a model, or has no geometry", index, name);
				}
			}

			index++;
			assetDumpSetStatus("Models: file %d of %d (%d written)", index, NUM_FILES, numModels);
		} else {
			assetDumpLogPhase("models", numModels);
			phase = PHASE_MESHES;
			index = 0;
			total = haveXbla ? xblaMeshGetNumPackageSlots() : 0;
		}
		break;
	case PHASE_MESHES:
		if (index < total) {
			numMeshes += assetDumpMesh(index, total);
			index++;
			assetDumpSetStatus("XBLA meshes: slot %d of %d (%d written)", index, total, numMeshes);
		} else {
			assetDumpLogPhase("XBLA meshes", numMeshes);
			assetDumpFinish();
		}
		break;
	default:
		break;
	}
}

void assetDumpTick(void)
{
	u64 start;

	if (phase == PHASE_IDLE || phase == PHASE_DONE) {
		return;
	}

	start = sysGetMicroseconds();

	while (phase != PHASE_DONE && sysGetMicroseconds() - start < ASSETDUMP_BUDGET) {
		assetDumpStep();
	}
}

void assetDumpCancel(void)
{
	if (phase == PHASE_IDLE || phase == PHASE_DONE) {
		return;
	}

	assetDumpFinish();
	assetDumpSetStatus("Stopped: %d textures, %d XBLA textures, %d models, %d XBLA meshes",
			numTextures, numRecords, numModels, numMeshes);
}

s32 assetDumpIsRunning(void)
{
	return phase != PHASE_IDLE && phase != PHASE_DONE;
}

const char *assetDumpGetStatus(void)
{
	return status;
}

void assetDumpFromCommandLine(void)
{
	if (!sysArgCheck("--dump-assets") && !sysArgCheck("--dump-textures")) {
		return;
	}

	assetDumpStart();

	while (phase != PHASE_DONE) {
		assetDumpStep();
	}

	exit(0);
}

#else

void assetDumpStart(void) { }
void assetDumpTick(void) { }
void assetDumpCancel(void) { }
s32 assetDumpIsRunning(void) { return 0; }
const char *assetDumpGetStatus(void) { return ""; }
void assetDumpFromCommandLine(void) { }

#endif

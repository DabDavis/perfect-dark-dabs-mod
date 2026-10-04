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
#include <strings.h>
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
#include "mod.h"
#include "pngwrite.h"
#include "gebean.h"
#include "geconvert.h"
#include "gexplusrom.h"
#include "modloader.h"
#include "game/file.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "lib/model.h"

#ifndef PLATFORM_N64

#define ASSETDUMP_DIR "model-dumps"
// Perfect Dark's, named as GoldenEye's are (n64 and xbla before 2026-10,
// still read by a model pack, never written)
#define ASSETDUMP_N64_SUB "pd-n64"
#define ASSETDUMP_XBLA_SUB "pd-xbla"
// GoldenEye's, each in a folder of its own under both model-dumps/ and
// texture-dumps/: the conversion of the player's ROM, and the XBLA release.
// A ROM hack's conversion is <its tag>-n64 under texture-dumps/ alone
// (gexPlusRomDirTag(): gf64-n64, tnd64-n64)
#define ASSETDUMP_GEN64_SUB "ge-n64"
#define ASSETDUMP_GEXBLA_SUB "ge-xbla"

// Conversions the textures pass can visit: GoldenEye's and its hacks'
#define ASSETDUMP_MAXCONV 8

// Texture numbers a model's texture command can name: twelve bits, which is
// more than the ROM's table, since GoldenEye's conversion has its own
#define ASSETDUMP_MAXTEX 4096

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
	PHASE_GE_TEXTURES,
	PHASE_GE_MODELS,
	PHASE_BEAN,
	PHASE_DONE,
};

static s32 phase = PHASE_IDLE;
static s32 cursor;
static s32 total;
static s32 haveXbla;
static s32 numTextures, numRecords, numModels, numMeshes, numRefused, numUnnamed;
static s32 numGeTextures, numGeModels, numBeanModels, numBeanTextures, numBeanRefused;
static u64 phaseStart; // for the log: how long each pass took
static char status[128];
static char modelDir[FS_MAXPATH + 1];   // model-dumps, expanded
static char texDir[FS_MAXPATH + 1];     // texture-dumps/<romid>, expanded
static char texRel[FS_MAXPATH + 1];     // the same as an MTL from model-dumps/pd-n64/ sees it
static char texXblaRel[FS_MAXPATH + 1]; // texture-dumps/pd-xbla as an MTL from model-dumps/pd-xbla/ sees it
static char texRoot[FS_MAXPATH + 1];    // texture-dumps, expanded
static char texRootName[64];            // its own name, "texture-dumps"
static s32 texRootShared;               // under the same folder as model-dumps

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

static void assetDumpTexRelFrom(char *dst, size_t len, s32 depth, const char *sub);

/**
 * model-dumps/ with pd-n64/ and pd-xbla/ inside it, and how the texture dump is
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

	texRootShared = 0;

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
			texRootShared = 1;
		} else {
			snprintf(texRel, sizeof(texRel), "%s", texDir);
		}

		snprintf(texRoot, sizeof(texRoot), "%.*s", (int)(romid - texDir), texDir);
	} else {
		snprintf(texRel, sizeof(texRel), "%s", texDir);
		snprintf(texRoot, sizeof(texRoot), "%s/..", texDir);
	}

	{
		const char *slash = strrchr(texRoot, '/');

		snprintf(texRootName, sizeof(texRootName), "%s", slash ? slash + 1 : texRoot);
	}

	{
		// the release's records beside the ROM's textures, not inside them
		const char *xbla = texpackGetDumpXblaDir();
		const char *slash = xbla ? strrchr(xbla, '/') : NULL;

		assetDumpTexRelFrom(texXblaRel, sizeof(texXblaRel), 1, slash ? slash + 1 : ASSETDUMP_XBLA_SUB);
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

	if (texturenum < 0 || texturenum >= ASSETDUMP_MAXTEX || !texInfo || !texPool) {
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
static s32 assetDumpModelTo(s32 fileid, const char *name, s32 gun, const char *path, const char *about);

static s32 assetDumpModel(s32 fileid, const char *name)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/" ASSETDUMP_N64_SUB "/%s.obj", modelDir, name);

	return assetDumpModelTo(fileid, name, name[0] == 'G', path, NULL);
}

/**
 * The same for any model file, written to path. about, when there is one, is
 * a line for the top of the file saying whose model it is.
 */
static s32 assetDumpModelTo(s32 fileid, const char *name, s32 gun, const char *path, const char *about)
{
	struct modelnode *nodes[ASSETDUMP_MAXNODES];
	struct modeldef *modeldef;
	struct objmesh *m;
	char comment[768];
	u8 *buffer = NULL;
	s32 n;
	s32 written = 0;

	modeldef = assetDumpLoadModel(fileid, gun, &buffer);

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
		snprintf(comment, sizeof(comment),
				"%s%smodel %s, file id 0x%04x, %d list nodes\n"
				"a group per list node (node0..), in the order the pack loader numbers them,\n"
				"each at the model's rest position; textures are n64_<number> in %s",
				about ? about : "", about ? "\n" : "", name, fileid, n, texRel);
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
			snprintf(mat->image, sizeof(mat->image), "%s/%04x.png", texXblaRel, mat->id);
		}
	}

	snprintf(path, sizeof(path), "%s/" ASSETDUMP_XBLA_SUB "/%s.obj", modelDir, name);
	snprintf(comment, sizeof(comment),
			"XBLA mesh for %s: PackedSegFile slot %d, named by model file id 0x%04x, %u groups, %u palette entries, header scale %g\n"
			"a group per part of the model (part0..); textures are xbla_<record> in %s%s",
			name, slot, fileid, m->numgroups, m->nummatrices, m->headerscale, texXblaRel, where);
	written = objmeshWrite(m, path, comment);
	objmeshFree(m);

	return written;
}

/* -------------------------------------------------------------------------
 * GoldenEye's, in folders of their own
 *
 * model-dumps/ge-n64/{props,chars,hand}/  the conversion of the player's
 *                                          GoldenEye ROM (mods/GoldenEye
 *                                          Arenas/files/Pgx, Cgx, Igx), named
 *                                          by GoldenEye's own file names
 * texture-dumps/ge-n64/                    its textures/, by the number the
 *                                          conversion gave each
 * model-dumps/ge-xbla/<look>/<kind>/       the XBLA release's (Project
 *                                          Bean's) files/new/ and original/
 *                                          models, by Rare's own names
 * texture-dumps/ge-xbla/<look>/<kind>/<name>/  each model's pictures, and
 *                                          files/texture/ as one picture a
 *                                          folder
 *
 * Only what is there: the conversion when it is mounted, the release when the
 * game has found it (added-content/).
 * ------------------------------------------------------------------------- */

// The names of what the pass is to walk, sorted so a run is the same twice
static char **jobList;
static s32 numJobs, maxJobs;

static void assetDumpJobsClear(void)
{
	for (s32 i = 0; i < numJobs; i++) {
		free(jobList[i]);
	}

	free(jobList);
	jobList = NULL;
	numJobs = maxJobs = 0;
}

static void assetDumpJobAdd(const char *name)
{
	if (numJobs == maxJobs) {
		const s32 grow = maxJobs ? maxJobs * 2 : 256;
		char **list = realloc(jobList, sizeof(*jobList) * grow);

		if (!list) {
			return;
		}

		jobList = list;
		maxJobs = grow;
	}

	jobList[numJobs] = malloc(strlen(name) + 1);

	if (jobList[numJobs]) {
		memcpy(jobList[numJobs], name, strlen(name) + 1);
	}

	if (jobList[numJobs]) {
		numJobs++;
	}
}

static int assetDumpJobCompare(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void assetDumpJobsSort(void)
{
	if (numJobs > 1) {
		qsort(jobList, numJobs, sizeof(*jobList), assetDumpJobCompare);
	}
}

/** mkdir -p, for a path already expanded. */
static s32 assetDumpMakeDirs(const char *path)
{
	char buf[FS_MAXPATH + 1];

	snprintf(buf, sizeof(buf), "%s", path);

	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			assetDumpMakeDir(buf);
			*p = '/';
		}
	}

	return assetDumpMakeDir(buf);
}

/**
 * The texture folder sub as an MTL depth folders under model-dumps/ reaches
 * it: relative when the two dumps share a root, the whole path when not.
 */
static void assetDumpTexRelFrom(char *dst, size_t len, s32 depth, const char *sub)
{
	size_t at = 0;

	if (!texRootShared) {
		snprintf(dst, len, "%s/%s", texRoot, sub);
		return;
	}

	dst[0] = '\0';

	for (s32 i = 0; i <= depth && at + 3 < len; i++) {
		memcpy(dst + at, "../", 3);
		at += 3;
		dst[at] = '\0';
	}

	snprintf(dst + at, len - at, "%s/%s", texRootName, sub);
}

/** A file name made of anything: letters, digits, - _ . and nothing else. */
static void assetDumpSafeName(char *dst, size_t len, const char *src)
{
	size_t i = 0;

	for (; src && *src && i + 1 < len; src++) {
		const char c = *src;

		dst[i++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
				|| c == '-' || c == '_' || c == '.' ? c : '_';
	}

	dst[i] = '\0';
}

/* ---- the conversion of the GoldenEye ROM ---- */

#define GENAMES_KINDS 3
#define GENAMES_MAX 340
#define GENAMES_LEN 40

static s32 geDir = -1;               // the mounted mod dir holding GoldenEye's conversion
static char geTexOut[FS_MAXPATH + 1];    // texture-dumps/<tag>-n64 of the conversion in hand, expanded
static char geModelOut[FS_MAXPATH + 1];  // model-dumps/ge-n64, expanded
static char (*geNames)[GENAMES_MAX][GENAMES_LEN];
static FILE *geIndex;
static char texRelRom[FS_MAXPATH + 1];   // texRel while the GE models have it

/**
 * The conversions the textures pass dumps, each by the converter's numbers
 * into texture-dumps/<tag>-n64/: GoldenEye's (ge-n64) and each ROM hack's
 * (gf64-n64, tnd64-n64). Found by their folders' names, never by what is in
 * them: all three have the same files, and the first mounted folder holding
 * GoldenEye's first body was whichever the mod list happened to list first.
 */
struct assetdumpconv {
	s32 dir;            // mounted mod dir
	const char *tag;    // gexPlusRomDirTag()
	const char *game;   // for the log and index.csv's first line
	s32 count;          // textures written
};

static struct assetdumpconv convs[ASSETDUMP_MAXCONV];
static s32 numConvs;
static s32 convAt;      // the one the textures pass is on
static s32 convDir = -1; // and its mounted dir
static u16 *convImages; // its remap.csv: texture number -> ROM image + 1, 0 for none

static void assetDumpGeName(void *arg, int kind, int num, const char *file)
{
	if (geNames && kind >= 0 && kind < GENAMES_KINDS && num >= 0 && num < GENAMES_MAX) {
		snprintf(geNames[kind][num], GENAMES_LEN, "%s", file);
	}
}

/**
 * The mounted dir of the conversion in mods/<name>, mounting it for its maps
 * if it is not - the player may have switched its maps off in the Stage
 * Loader, or started with --moddir. Mounted as modborrow.c mounts the mod it
 * borrows from, until the game exits. -1 when it is not there at all.
 */
static s32 assetDumpFindConvDir(const char *name)
{
	static const char *const containers[] = { "$E/mods", "$H/mods" };

	const char *want = gexPlusRomDirTag(name);

	for (s32 i = 0; want && i < fsGetNumModDirs(); i++) {
		const char *tag = gexPlusRomDirTag(fsGetModDirAt(i));

		if (tag && !strcmp(tag, want)) {
			return i;
		}
	}

	for (u32 i = 0; i < ARRAYCOUNT(containers); i++) {
		char path[FS_MAXPATH + 1];
		char dir[FS_MAXPATH + 1];

		snprintf(dir, sizeof(dir), "%s/%s", containers[i], name);
		snprintf(path, sizeof(path), "%s/modconfig.txt", dir);

		if (fsFileSize(path) > 0) {
			const s32 index = fsAddMapsDir(dir);

			if (index >= 0) {
				sysLogPrintf(LOG_NOTE, "assetdump: %s mounted for the dump", fsGetModDirAt(index));
			}

			return index;
		}
	}

	return -1;
}

static void assetDumpConvAdd(const char *name, const char *game)
{
	const s32 dir = numConvs < ASSETDUMP_MAXCONV ? assetDumpFindConvDir(name) : -1;

	if (dir >= 0) {
		convs[numConvs].dir = dir;
		convs[numConvs].tag = gexPlusRomDirTag(name);
		convs[numConvs].game = game;
		convs[numConvs].count = 0;
		numConvs++;
	}
}

static void assetDumpConvsFind(void)
{
	numConvs = 0;
	convAt = -1;
	convDir = -1;
	geDir = modloaderGexPlusDirIndex();

	if (geDir < 0) {
		geDir = assetDumpFindConvDir(GEXPLUSROM_DIR);
	}

	if (geDir >= 0) {
		convs[numConvs].dir = geDir;
		convs[numConvs].tag = geconvertGoldenEyeTag();
		convs[numConvs].game = "GoldenEye 007";
		convs[numConvs].count = 0;
		numConvs++;
	} else {
		sysLogPrintf(LOG_NOTE, "assetdump: no GoldenEye conversion (mods/" GEXPLUSROM_DIR "), so no " ASSETDUMP_GEN64_SUB " dump");
	}

	for (s32 i = 0; geconvertVariantNameAt(i); i++) {
		assetDumpConvAdd(geconvertVariantNameAt(i), geconvertVariantNameAt(i));
	}
}

/**
 * The conversion's textures/remap.csv into convImages: the image each number
 * was written from, which for a hack's moved numbers is known to nothing else
 * (geconvert.c writes it since version 122). 0 when there is none.
 */
static s32 assetDumpReadRemap(s32 dir)
{
	char path[FS_MAXPATH + 1];
	char line[64];
	s32 rows = 0;
	FILE *f;

	free(convImages);
	convImages = NULL;

	snprintf(path, sizeof(path), "%s/textures/remap.csv", fsGetModDirAt(dir));
	f = fopen(path, "rb");

	if (!f) {
		return 0;
	}

	convImages = calloc(ASSETDUMP_MAXTEX, sizeof(*convImages));

	while (convImages && fgets(line, sizeof(line), f)) {
		u32 num;
		u32 image;

		if (sscanf(line, "%x,%u", &num, &image) == 2 && num < ASSETDUMP_MAXTEX && image < 0xffff) {
			convImages[num] = (u16)(image + 1);
			rows++;
		}
	}

	fclose(f);

	return rows;
}

static void assetDumpScanGeTexture(const char *name, void *arg)
{
	u32 num;
	char tail[8];

	if (strlen(name) == 8 && sscanf(name, "%4x%7s", &num, tail) == 2 && !strcmp(tail, ".bin")
			&& num < ASSETDUMP_MAXTEX) {
		assetDumpJobAdd(name);
	}
}

static void assetDumpScanGeModel(const char *name, void *arg)
{
	if ((name[0] == 'P' || name[0] == 'C' || name[0] == 'I') && name[1] == 'g' && name[2] == 'x'
			&& strlen(name) == 7 && name[6] == 'Z') {
		assetDumpJobAdd(name);
	}
}

/** One conversion's textures pass list; 0 when there is nothing to dump. */
static s32 assetDumpGeTexturesBegin(const struct assetdumpconv *conv)
{
	char path[FS_MAXPATH + 1];
	char stamp[64] = "geconvert " GECONVERT_VERSION_STR;
	s32 remapped;

	assetDumpJobsClear();
	convDir = conv->dir;

	snprintf(geTexOut, sizeof(geTexOut), "%s/%s" GEXPLUSROM_DUMP_SUFFIX, texRoot, conv->tag);

	if (!assetDumpMakeDirs(geTexOut)) {
		sysLogPrintf(LOG_ERROR, "assetdump: could not create %s", geTexOut);
		convDir = -1;
		return 0;
	}

	snprintf(path, sizeof(path), "%s/textures", fsGetModDirAt(convDir));
	fsScanDir(path, assetDumpScanGeTexture, NULL);
	assetDumpJobsSort();

	remapped = assetDumpReadRemap(convDir);

	{
		// The converter that wrote it, CONVERT.txt's first line: the
		// numbers are that version's
		FILE *f;

		snprintf(path, sizeof(path), "%s/CONVERT.txt", fsGetModDirAt(convDir));
		f = fopen(path, "rb");

		if (f) {
			char line[64];

			if (fgets(line, sizeof(line), f) && !strncmp(line, "geconvert ", 10)) {
				line[strcspn(line, "\r\n")] = '\0';
				snprintf(stamp, sizeof(stamp), "%s", line);
			}

			fclose(f);
		}
	}

	snprintf(path, sizeof(path), "%s/index.csv", geTexOut);
	geIndex = fopen(path, "wb");

	if (geIndex) {
		fprintf(geIndex, "# %s, %s\n", conv->game, stamp);
		fprintf(geIndex, "texnum,rom_image,fmt,width,height,png\n");
	}

	sysLogPrintf(LOG_NOTE, "assetdump: %s's conversion in %s: %d textures%s", conv->game,
			fsGetModDirAt(convDir), numJobs,
			remapped ? "" : conv->tag == geconvertGoldenEyeTag() ? ""
				: " (no textures/remap.csv: converted before 122, so the moved numbers' ROM images are not known)");

	return numJobs;
}

/** The next conversion's textures pass list, 0 when they are all done. */
static s32 assetDumpGeTexturesNext(void)
{
	while (++convAt < numConvs) {
		const s32 count = assetDumpGeTexturesBegin(&convs[convAt]);

		if (count > 0) {
			return count;
		}
	}

	return 0;
}

/** One of the conversion's textures, as the models' MTLs name it. */
static s32 assetDumpGeTexture(const char *file)
{
	char path[FS_MAXPATH + 1];
	struct texpool pool;
	struct tex *tex;
	u32 num = 0;
	s32 written = 0;
	s32 prev;

	if (convDir < 0 || sscanf(file, "%4x", &num) != 1) {
		return 0;
	}

	prev = modSetTextureSourceMod(convDir);

	texInitPool(&pool, texPool, ASSETDUMP_TEXPOOL);
	texLoadFromTextureNum(num, &pool);
	tex = texFindInPool((s32)num, &pool);

	if (tex && tex->data) {
		s32 width = 0;
		s32 height = 0;
		u8 *rgba = texpackTexToRgba(tex, &width, &height);

		if (rgba) {
			const char *fmt = assetDumpFormatName(tex->gbiformat, tex->depth);

			snprintf(path, sizeof(path), "%s/%04x_%s.png", geTexOut, num, fmt);
			written = pngWrite(path, rgba, width, height, 4, 0) != 0;

			if (written && geIndex) {
				// GoldenEye's own numbers move by one fixed table; a hack's
				// by the order its arenas first used them, which only its
				// remap.csv has - blank where that is missing
				char image[16] = "";

				if (convImages && convImages[num]) {
					snprintf(image, sizeof(image), "%u", convImages[num] - 1);
				} else if (convDir == geDir) {
					snprintf(image, sizeof(image), "%u", geconvertTexUnremap(num));
				}

				fprintf(geIndex, "%04x,%s,%s,%d,%d,%04x_%s.png\n", num, image, fmt,
						width, height, num, fmt);
			}

			free(rgba);
		}
	}

	texpackForgetRange(texPool, texPool + ASSETDUMP_TEXPOOL);
	modSetTextureSourceMod(prev);

	if (written && convAt >= 0 && convAt < numConvs) {
		convs[convAt].count++;
	}

	return written;
}

static void assetDumpGeTexturesEnd(void)
{
	if (geIndex) {
		fclose(geIndex);
		geIndex = NULL;
	}

	free(convImages);
	convImages = NULL;
}

static const char *const geKindDirs[GENAMES_KINDS] = { "props", "chars", "hand" };

static s32 assetDumpGeModelsBegin(void)
{
	char path[FS_MAXPATH + 1];
	s32 named = 0;

	assetDumpJobsClear();

	// GoldenEye's own models only: a hack's are not dumped (yet), and
	// GoldenEye's names below would be wrong for them
	if (geDir < 0 || !fsGetModDirAt(geDir)) {
		return 0;
	}

	snprintf(geModelOut, sizeof(geModelOut), "%s/" ASSETDUMP_GEN64_SUB, modelDir);

	// A model's numbers are the conversion's now, not the ROM's
	memset(texInfo, 0, sizeof(*texInfo) * ASSETDUMP_MAXTEX);

	for (s32 k = 0; k < GENAMES_KINDS; k++) {
		snprintf(path, sizeof(path), "%s/%s", geModelOut, geKindDirs[k]);

		if (!assetDumpMakeDirs(path)) {
			sysLogPrintf(LOG_ERROR, "assetdump: could not create %s", path);
			return 0;
		}
	}

	geNames = calloc(GENAMES_KINDS, sizeof(*geNames));

	if (geNames && gexPlusRomReadNames(assetDumpGeName, NULL)) {
		named = 1;
	} else {
		sysLogPrintf(LOG_NOTE, "assetdump: no GoldenEye ROM to read its own names from; the conversion's are used");
	}

	snprintf(path, sizeof(path), "%s/files", fsGetModDirAt(geDir));
	fsScanDir(path, assetDumpScanGeModel, NULL);
	assetDumpJobsSort();

	snprintf(path, sizeof(path), "%s/index.csv", geModelOut);
	geIndex = fopen(path, "wb");

	if (geIndex) {
		fprintf(geIndex, "obj,conversion_file,kind,goldeneye_number,goldeneye_file\n");
	}

	// The MTLs' way to texture-dumps/ge-n64 is one folder deeper than n64/'s
	snprintf(texRelRom, sizeof(texRelRom), "%s", texRel);
	assetDumpTexRelFrom(texRel, sizeof(texRel), 2, ASSETDUMP_GEN64_SUB);

	sysLogPrintf(LOG_NOTE, "assetdump: %d of GoldenEye's converted models%s", numJobs,
			named ? ", named by the ROM's own file names" : "");

	return numJobs;
}

/** One of the conversion's models, under GoldenEye's own name for it. */
static s32 assetDumpGeModel(const char *file)
{
	const s32 kind = file[0] == 'P' ? 0 : file[0] == 'C' ? 1 : 2;
	const s32 num = atoi(file + 3);
	const char *gename = geNames && num >= 0 && num < GENAMES_MAX && geNames[kind][num][0] ? geNames[kind][num] : NULL;
	char outname[GENAMES_LEN + 16];
	char path[FS_MAXPATH + 1];
	char about[256];
	const s32 existed = romdataFileGetNumForName(file);
	s32 fileid;
	s32 written;
	s32 prev;

	fileid = romdataRegisterModFile(file, geDir);

	if (fileid <= 0) {
		return 0;
	}

	assetDumpSafeName(outname, sizeof(outname), gename ? gename : file);

	// Two of GoldenEye's numbers can be the one file (hand items share
	// models); the later is told apart by its number
	for (s32 i = 0; gename && i < num; i++) {
		if (!strcmp(geNames[kind][i], gename)) {
			char taken[GENAMES_LEN + 16];

			snprintf(taken, sizeof(taken), "%s", outname);
			snprintf(outname, sizeof(outname), "%s_%03d", taken, num);
			break;
		}
	}

	snprintf(path, sizeof(path), "%s/%s/%s.obj", geModelOut, geKindDirs[kind], outname);

	snprintf(about, sizeof(about), "GoldenEye's %s %d%s%s, converted from the ROM as %s (mods/" GEXPLUSROM_DIR "/files/%s)",
			kind == 0 ? "prop" : kind == 1 ? "character" : "hand item", num,
			gename ? ", " : "", gename ? gename : "", file, file);

	prev = modSetTextureSourceMod(geDir);
	written = assetDumpModelTo(fileid, file, 0, path, about);
	modSetTextureSourceMod(prev);

	// what this pass loaded is let go again, unless the game had it before
	if (existed <= 0) {
		romdataFileFree(fileid);
	}

	if (written && geIndex) {
		fprintf(geIndex, "%s/%s.obj,%s,%s,%d,%s\n", geKindDirs[kind], outname, file,
				kind == 0 ? "prop" : kind == 1 ? "character" : "hand item", num, gename ? gename : "");
	}

	return written;
}

static void assetDumpGeModelsEnd(void)
{
	if (geIndex) {
		fclose(geIndex);
		geIndex = NULL;
	}

	free(geNames);
	geNames = NULL;
	// back to the ROM's, for anything after
	if (texRelRom[0]) {
		snprintf(texRel, sizeof(texRel), "%s", texRelRom);
		texRelRom[0] = '\0';
	}
}

/* ---- the GoldenEye XBLA release (Project Bean) ---- */

static char beanRoot[FS_MAXPATH + 1];    // the release's files/
static char beanModelOut[FS_MAXPATH + 1]; // model-dumps/ge-xbla
static char beanTexOut[FS_MAXPATH + 1];   // texture-dumps/ge-xbla

// What of the release is dumped, by look and kind. A model's collision
// copy (<name>_hits) is left out: it is not drawn.
static const char *const beanLooks[] = { "new", "original" };
static const char *const beanKinds[] = { "char", "head", "gun", "prop", "background", "skydome" };

static char beanScanPrefix[FS_MAXPATH + 1];

static void assetDumpScanBeanModel(const char *name, void *arg)
{
	const size_t len = strlen(name);
	char path[FS_MAXPATH + 1];
	char job[FS_MAXPATH + 1];

	if (len > 5 && !strcmp(name + len - 5, "_hits")) {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s/%s/default.bin", beanRoot, beanScanPrefix, name);

	if (fsFileSize(path) > 0) {
		snprintf(job, sizeof(job), "m:%s/%s", beanScanPrefix, name);
		assetDumpJobAdd(job);
	}
}

// files/texture/ and files/new/texture/: a folder a picture, some of them
// folders of folders
static void assetDumpScanBeanPictures(const char *rel);

static void assetDumpScanBeanPicture(const char *name, void *arg)
{
	const char *rel = arg;
	char path[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	snprintf(sub, sizeof(sub), "%s/%s", rel, name);
	snprintf(path, sizeof(path), "%s/%s/default.rba", beanRoot, sub);

	if (fsFileSize(path) > 0) {
		char job[FS_MAXPATH + 1];

		snprintf(job, sizeof(job), "t:%s", sub);
		assetDumpJobAdd(job);
	} else if (!strchr(name, '.')) {
		assetDumpScanBeanPictures(sub);
	}
}

static void assetDumpScanBeanPictures(const char *rel)
{
	char path[FS_MAXPATH + 1];
	s32 slashes = 0;

	for (const char *p = rel; *p; p++) {
		slashes += *p == '/';
	}

	// texture/sfx/firebomb1 is as deep as they go
	if (slashes > 4) {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", beanRoot, rel);
	fsScanDir(path, assetDumpScanBeanPicture, (void *)rel);
}

static s32 assetDumpBeanBegin(void)
{
	char archive[FS_MAXPATH + 1];
	char cache[FS_MAXPATH + 1];

	assetDumpJobsClear();

	// Unpacks the archive if nothing has yet, once, as the game would
	if (!gebeanIsAvailable() || !gebeanTreeInfo(beanRoot, sizeof(beanRoot), archive, sizeof(archive), cache, sizeof(cache))) {
		sysLogPrintf(LOG_NOTE, "assetdump: no GoldenEye XBLA release in " FS_ADDED_CONTENT_DIR "/, so no " ASSETDUMP_GEXBLA_SUB " dump");
		return 0;
	}

	snprintf(beanModelOut, sizeof(beanModelOut), "%s/" ASSETDUMP_GEXBLA_SUB, modelDir);
	snprintf(beanTexOut, sizeof(beanTexOut), "%s/" ASSETDUMP_GEXBLA_SUB, texRoot);

	for (u32 l = 0; l < ARRAYCOUNT(beanLooks); l++) {
		for (u32 k = 0; k < ARRAYCOUNT(beanKinds); k++) {
			char path[FS_MAXPATH + 1];

			snprintf(beanScanPrefix, sizeof(beanScanPrefix), "%s/%s", beanLooks[l], beanKinds[k]);
			snprintf(path, sizeof(path), "%s/%s", beanRoot, beanScanPrefix);
			fsScanDir(path, assetDumpScanBeanModel, NULL);
		}
	}

	assetDumpScanBeanPictures("texture");
	assetDumpScanBeanPictures("new/texture");
	assetDumpJobsSort();

	sysLogPrintf(LOG_NOTE, "assetdump: GoldenEye XBLA release in %s: %d models and pictures", beanRoot, numJobs);

	return numJobs;
}

// One Bean model being made into an OBJ: its vertices shared where they are
// the same, found through a hash of the last few thousand
#define BEANHASH_BITS 16

struct beanobj {
	struct objmesh *m;
	s32 *hash;          // (1 << BEANHASH_BITS) vertex indices, -1 for none
	s32 material;
	s32 draw;
	const s32 *texmat;  // picture index -> material
	s32 numtex;
	s32 lastnode;
	s32 lastconds;
};

static u32 assetDumpHashVertex(const struct objvertex *v)
{
	u32 h = 2166136261u;
	const u8 *p = (const u8 *)v->pos;

	for (size_t i = 0; i < sizeof(v->pos) + sizeof(v->uv); i++) {
		h = (h ^ p[i]) * 16777619u;
	}

	p = (const u8 *)v->nrm;

	for (size_t i = 0; i < sizeof(v->nrm); i++) {
		h = (h ^ p[i]) * 16777619u;
	}

	for (s32 i = 0; i < 4; i++) {
		h = (h ^ v->rgba[i]) * 16777619u;
	}

	return h >> (32 - BEANHASH_BITS);
}

static s32 assetDumpBeanVertex(struct beanobj *o, const struct objvertex *v)
{
	u32 h = assetDumpHashVertex(v);
	const s32 at = o->hash ? o->hash[h] : -1;

	// one probe: a collision just writes the vertex again
	if (at >= 0 && (u32)at < o->m->numvertices) {
		const struct objvertex *w = &o->m->vertices[at];

		if (!memcmp(w->pos, v->pos, sizeof(v->pos)) && !memcmp(w->uv, v->uv, sizeof(v->uv))
				&& !memcmp(w->nrm, v->nrm, sizeof(v->nrm)) && !memcmp(w->rgba, v->rgba, sizeof(v->rgba))) {
			return at;
		}
	}

	{
		const s32 added = objmeshAddVertex(o->m, v);

		if (added >= 0 && o->hash) {
			o->hash[h] = added;
		}

		return added;
	}
}

/** One triangle of a Bean model or level, in its picture's material. */
static void assetDumpBeanTriangle(struct beanobj *o, s32 tex, const f32 pos[3][3], const f32 uv[3][2], const f32 nrm[3][3],
		const u32 argb[3])
{
	const s32 material = tex >= 0 && tex < o->numtex ? o->texmat[tex] : -1;
	struct objvertex v[3];
	s32 idx[3];
	f32 e1[3], e2[3], n[3], len;

	for (s32 j = 0; j < 3; j++) {
		e1[j] = pos[1][j] - pos[0][j];
		e2[j] = pos[2][j] - pos[0][j];
	}

	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

	if (!(len > 0)) {
		return;
	}

	for (s32 k = 0; k < 3; k++) {
		memset(&v[k], 0, sizeof(v[k]));
		memcpy(v[k].pos, pos[k], sizeof(v[k].pos));
		// the release's v runs down the picture, OBJ's up it, as for the
		// PD release's meshes
		v[k].uv[0] = uv[k][0];
		v[k].uv[1] = 1.0f - uv[k][1];

		// the file's own normal, so a smooth surface stays smooth and its
		// vertices shared; the face's where the file has none
		{
			const f32 vl = sqrtf(nrm[k][0] * nrm[k][0] + nrm[k][1] * nrm[k][1] + nrm[k][2] * nrm[k][2]);

			for (s32 j = 0; j < 3; j++) {
				v[k].nrm[j] = vl > 1e-6f ? nrm[k][j] / vl : n[j] / len;
			}
		}

		v[k].rgba[0] = (argb[k] >> 16) & 0xff;
		v[k].rgba[1] = (argb[k] >> 8) & 0xff;
		v[k].rgba[2] = argb[k] & 0xff;
		v[k].rgba[3] = argb[k] >> 24;
		v[k].weight[0] = 1.0f;
		idx[k] = assetDumpBeanVertex(o, &v[k]);

		if (idx[k] < 0) {
			return;
		}
	}

	if (o->draw < 0 || o->m->draws[o->draw].material != material) {
		o->draw = objmeshAddDraw(o->m, o->m->numtris, 0, material);

		if (o->draw < 0) {
			return;
		}
	}

	if (objmeshAddTriangle(o->m, (u32)idx[0], (u32)idx[1], (u32)idx[2]) >= 0) {
		o->m->draws[o->draw].numtris++;
	}
}

static void assetDumpBeanCloseGroup(struct beanobj *o)
{
	if (o->m->numgroups) {
		struct objgroup *g = &o->m->groups[o->m->numgroups - 1];

		g->numdraws = o->m->numdraws - g->firstdraw;
	}
}

static void assetDumpBeanModelDraw(const struct gebeanmodeldraw *d, void *arg)
{
	struct beanobj *o = arg;

	// A group per node the draws name, and per section they stand in, so
	// what the release shows only some of the time can be hidden
	if (o->m->numgroups == 0 || d->node != o->lastnode || d->numconds != o->lastconds) {
		char gname[OBJMESH_NAMELEN];

		if (d->numconds) {
			snprintf(gname, sizeof(gname), "%s%d_if%x", d->node >= 0 ? "node" : "part", d->node >= 0 ? d->node : o->m->numgroups,
					(u32)d->conds[d->numconds - 1]);
		} else if (d->node >= 0) {
			snprintf(gname, sizeof(gname), "node%d", d->node);
		} else {
			snprintf(gname, sizeof(gname), "part%u", o->m->numgroups);
		}

		assetDumpBeanCloseGroup(o);

		if (objmeshAddGroup(o->m, gname, o->m->numdraws, 0) < 0) {
			return;
		}

		o->lastnode = d->node;
		o->lastconds = d->numconds;
		o->draw = -1;
	}

	for (s32 t = 0; t + 2 < d->numvtx; t += 3) {
		f32 pos[3][3], uv[3][2], nrm[3][3];
		u32 argb[3];

		for (s32 k = 0; k < 3; k++) {
			memcpy(pos[k], d->vtx[t + k].pos, sizeof(pos[k]));
			memcpy(uv[k], d->vtx[t + k].uv, sizeof(uv[k]));
			memcpy(nrm[k], d->vtx[t + k].nrm, sizeof(nrm[k]));
			argb[k] = d->vtx[t + k].argb;
		}

		assetDumpBeanTriangle(o, d->tex, pos, uv, nrm, argb);
	}
}

static void assetDumpBeanLevelTri(void *arg, s32 tex, const struct gebeanlevelvtx *v)
{
	struct beanobj *o = arg;
	f32 pos[3][3], uv[3][2], nrm[3][3];
	u32 argb[3];

	for (s32 k = 0; k < 3; k++) {
		memcpy(pos[k], v[k].pos, sizeof(pos[k]));
		memcpy(uv[k], v[k].uv, sizeof(uv[k]));
		memcpy(nrm[k], v[k].nrm, sizeof(nrm[k]));
		argb[k] = v[k].argb;
	}

	assetDumpBeanTriangle(o, tex, pos, uv, nrm, argb);
}

static void assetDumpBeanTexName(char *dst, size_t len, const char *name);

/**
 * The pictures of one model, written to texture-dumps/ge-xbla/<source>/ as
 * <index>_<the file's name for it>.png, and a material each in m.
 */
static s32 assetDumpBeanPictures(struct objmesh *m, const char *source, s32 numtex, s32 *texmat,
		u8 *(*decode)(void *handle, s32 i, s32 *w, s32 *h), const char *(*texname)(void *handle, s32 i), void *handle)
{
	char dir[FS_MAXPATH + 1];
	char rel[FS_MAXPATH + 1];
	s32 written = 0;

	snprintf(dir, sizeof(dir), "%s/%s", beanTexOut, source);

	if (numtex > 0 && !assetDumpMakeDirs(dir)) {
		sysLogPrintf(LOG_ERROR, "assetdump: could not create %s", dir);
	}

	// model-dumps/ge-xbla/<look>/<kind>/<name>.obj is three folders down
	{
		char sub[FS_MAXPATH + 1];

		snprintf(sub, sizeof(sub), ASSETDUMP_GEXBLA_SUB "/%s", source);
		assetDumpTexRelFrom(rel, sizeof(rel), 3, sub);
	}

	for (s32 i = 0; i < numtex; i++) {
		char safe[64];
		char file[128];
		char path[FS_MAXPATH + 1];
		char image[FS_MAXPATH + 1];
		char mname[OBJMESH_NAMELEN];
		s32 w = 0, h = 0, alpha = 0;
		u8 *rgba = decode(handle, i, &w, &h);

		texmat[i] = -1;
		assetDumpBeanTexName(safe, sizeof(safe), texname(handle, i));
		snprintf(file, sizeof(file), "%02d%s%s.png", i, safe[0] ? "_" : "", safe);
		snprintf(mname, sizeof(mname), "bean_%02d%s%.40s", i, safe[0] ? "_" : "", safe);

		if (!rgba || w <= 0 || h <= 0) {
			free(rgba);
			texmat[i] = objmeshAddMaterial(m, mname, OBJMAT_NONE, 0, 0, NULL);
			continue;
		}

		for (s32 p = 0; p < w * h; p++) {
			if (rgba[p * 4 + 3] < 0xf0) {
				alpha = 1;
				break;
			}
		}

		snprintf(path, sizeof(path), "%s/%s", dir, file);

		if (pngWrite(path, rgba, w, h, 4, 1) != 0) {
			written++;
		}

		free(rgba);
		snprintf(image, sizeof(image), "%s/%s", rel, file);
		texmat[i] = objmeshAddMaterial(m, mname, OBJMAT_IMAGE, 0, alpha, image);
	}

	return written;
}

/** "_0x0169EB91.tga.bin" as "0x0169EB91": the release names a picture by a hash. */
static void assetDumpBeanTexName(char *dst, size_t len, const char *name)
{
	static const char *const tails[] = { ".bin", ".tga", ".dds", ".png" };
	size_t n;

	while (name && *name == '_') {
		name++;
	}

	assetDumpSafeName(dst, len, name);

	for (s32 again = 1; again; ) {
		again = 0;
		n = strlen(dst);

		for (u32 i = 0; i < ARRAYCOUNT(tails); i++) {
			const size_t t = strlen(tails[i]);

			if (n > t && !strcasecmp(dst + n - t, tails[i])) {
				dst[n - t] = '\0';
				again = 1;
				break;
			}
		}
	}
}

static u8 *assetDumpBeanPicsDecode(void *handle, s32 i, s32 *w, s32 *h)
{
	return gebeanPicturesDecode(handle, i, w, h);
}

static const char *assetDumpBeanPicsName(void *handle, s32 i)
{
	return gebeanPicturesName(handle, i);
}

static u8 *assetDumpBeanLevelDecode(void *handle, s32 i, s32 *w, s32 *h)
{
	return gebeanLevelDecode(handle, i, w, h);
}

static const char *assetDumpBeanLevelName(void *handle, s32 i)
{
	return gebeanLevelTextureName(handle, i);
}

/** One model, level or sky of the release: source is "new/prop/cardbox1". */
static s32 assetDumpBeanModel(const char *source)
{
	const char *slash = strrchr(source, '/');
	const char *name = slash ? slash + 1 : source;
	const s32 islevel = strstr(source, "/background/") != NULL;
	const s32 issky = strstr(source, "/skydome/") != NULL;
	struct gebeanpictures *pics = NULL;
	struct gebeanlevel *level = NULL;
	struct beanobj o;
	s32 texmat[GEBEAN_MAXMATS];
	s32 numtex;
	char path[FS_MAXPATH + 1];
	char dir[FS_MAXPATH + 1];
	char comment[512];
	s32 written = 0;

	if (issky) {
		level = strncmp(source, "new/", 4) == 0 ? gebeanSkyOpen(name) : NULL;
	} else if (islevel) {
		level = gebeanLevelOpenSource(source);
	} else {
		pics = gebeanPicturesOpen(source);
	}

	if (!pics && !level) {
		return 0;
	}

	memset(&o, 0, sizeof(o));
	o.m = objmeshAlloc(name);
	o.hash = malloc(sizeof(s32) << BEANHASH_BITS);
	o.draw = -1;
	o.lastnode = -2;
	o.texmat = texmat;

	if (!o.m) {
		free(o.hash);
		gebeanPicturesClose(pics);
		gebeanLevelClose(level);
		return 0;
	}

	if (o.hash) {
		memset(o.hash, 0xff, sizeof(s32) << BEANHASH_BITS);
	}

	numtex = pics ? gebeanPicturesCount(pics) : gebeanLevelNumTextures(level);

	if (numtex > GEBEAN_MAXMATS) {
		numtex = GEBEAN_MAXMATS;
	}

	o.numtex = numtex;
	numBeanTextures += pics
		? assetDumpBeanPictures(o.m, source, numtex, texmat, assetDumpBeanPicsDecode, assetDumpBeanPicsName, pics)
		: assetDumpBeanPictures(o.m, source, numtex, texmat, assetDumpBeanLevelDecode, assetDumpBeanLevelName, level);

	if (pics) {
		gebeanPicturesWalk(pics, assetDumpBeanModelDraw, &o);
		assetDumpBeanCloseGroup(&o);
	} else {
		objmeshAddGroup(o.m, islevel ? "level" : "sky", 0, 0);
		gebeanLevelTriangles(level, assetDumpBeanLevelTri, &o);
		assetDumpBeanCloseGroup(&o);
	}

	if (o.m->numtris > 0) {
		snprintf(dir, sizeof(dir), "%s/%.*s", beanModelOut, (int)(slash ? slash - source : 0), source);
		assetDumpMakeDirs(dir);
		snprintf(path, sizeof(path), "%s/%s.obj", dir, name);
		snprintf(comment, sizeof(comment),
				"GoldenEye XBLA (Project Bean) %s files/%s, in the file's own units and pose\n"
				"%s; pictures in texture-dumps/" ASSETDUMP_GEXBLA_SUB "/%s",
				islevel ? "level" : issky ? "sky" : "model", source,
				pics ? "a group per node the draws name (node<N>), part<N> for a plain draw, _if<id> for one in a section the release shows only sometimes"
					: "one group; a level is drawn whole, with no rooms",
				source);
		written = objmeshWrite(o.m, path, comment);
	}

	objmeshFree(o.m);
	free(o.hash);
	gebeanPicturesClose(pics);
	gebeanLevelClose(level);

	return written;
}

/** One of the release's pictures that is a file of its own: texture/circle. */
static s32 assetDumpBeanPictureFile(const char *source)
{
	char path[FS_MAXPATH + 1];
	char dir[FS_MAXPATH + 1];
	const char *slash = strrchr(source, '/');
	s32 w = 0, h = 0;
	s32 written = 0;
	u8 *rgba = gebeanDecodePictureFile(source, &w, &h);

	if (!rgba) {
		return 0;
	}

	snprintf(dir, sizeof(dir), "%s/%.*s", beanTexOut, (int)(slash ? slash - source : 0), source);
	assetDumpMakeDirs(dir);
	snprintf(path, sizeof(path), "%s/%s.png", beanTexOut, source);

	if (w > 0 && h > 0) {
		written = pngWrite(path, rgba, w, h, 4, 1) != 0;
	}

	free(rgba);

	return written;
}

static void assetDumpBeanStep(const char *job)
{
	if (job[0] == 'm') {
		if (assetDumpBeanModel(job + 2)) {
			numBeanModels++;
		} else {
			numBeanRefused++;
			sysLogPrintf(LOG_NOTE, "assetdump: files/%s is not a model this reads, or has no triangles", job + 2);
		}
	} else {
		numBeanTextures += assetDumpBeanPictureFile(job + 2);
	}
}

/* -------------------------------------------------------------------------
 * Driving it
 * ------------------------------------------------------------------------- */

static void assetDumpFinish(void)
{
	texpackDumpClose();
	texpackDumpXblaClose();
	free(texInfo);
	free(texPool);
	texInfo = NULL;
	texPool = NULL;
	phase = PHASE_DONE;
	assetDumpGeTexturesEnd();
	assetDumpGeModelsEnd();
	assetDumpJobsClear();

	if (numGeModels || numBeanModels) {
		assetDumpSetStatus("Wrote %d textures, %d models, %d XBLA meshes; GoldenEye: %d models, %d XBLA models",
				numTextures + numRecords + numGeTextures + numBeanTextures, numModels, numMeshes, numGeModels, numBeanModels);
	} else {
		assetDumpSetStatus("Wrote %d textures, %d XBLA textures, %d models, %d XBLA meshes",
				numTextures, numRecords, numModels, numMeshes);
	}
	sysLogPrintf(LOG_NOTE, "assetdump: %s; textures in %s, models in %s%s", status, texDir, modelDir,
			haveXbla ? "" : " (no XBLA package, so no XBLA textures or meshes)");

	for (s32 i = 0; i < numConvs; i++) {
		char models[FS_MAXPATH + 32] = "";

		if (convs[i].dir == geDir && numGeModels) {
			snprintf(models, sizeof(models), ", %d models in %s", numGeModels, geModelOut);
		}

		if (convs[i].count || models[0]) {
			sysLogPrintf(LOG_NOTE, "assetdump: %s's conversion: %d textures in %s/%s" GEXPLUSROM_DUMP_SUFFIX "%s",
					convs[i].game, convs[i].count, texRoot, convs[i].tag, models);
		}
	}

	if (numBeanModels || numBeanTextures) {
		sysLogPrintf(LOG_NOTE, "assetdump: GoldenEye XBLA: %d models in %s, %d pictures in %s (%d files not read)",
				numBeanModels, beanModelOut, numBeanTextures, beanTexOut, numBeanRefused);
	}

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
	numGeTextures = numGeModels = numBeanModels = numBeanTextures = numBeanRefused = 0;
	geDir = -1;
	convDir = -1;
	numConvs = 0;
	meshPrevName[0] = '\0';
	meshSincePrev = 0;
	cursor = 0;
	total = 0;

	if (!assetDumpOpenDirs() || !texpackDumpOpen()) {
		phase = PHASE_DONE;
		assetDumpSetStatus("Nowhere to write to - see the log");
		return;
	}

	texInfo = calloc(ASSETDUMP_MAXTEX, sizeof(*texInfo));
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
		if (cursor < NUM_TEXTURES) {
			numTextures += texpackDumpTextureNum(cursor);
			cursor++;
			assetDumpSetStatus("Textures: %d of %d", cursor, NUM_TEXTURES);
		} else {
			texpackDumpClose();
			assetDumpLogPhase("textures", numTextures);
			phase = PHASE_RECORDS;
			cursor = 0;
			// Opens the package, which unpacks the archive if that has not
			// happened yet - a few seconds, once.
			total = haveXbla ? (s32)xblaTexGetNumRecords() : 0;
		}
		break;
	case PHASE_RECORDS:
		if (cursor < total) {
			s32 width = 0;
			s32 height = 0;
			u8 *rgba = xblaTexDecodeRecord((u32)cursor, &width, &height);

			if (rgba) {
				numRecords += texpackWriteXblaRecord(rgba, (u32)width, (u32)height, (u32)cursor);
				free(rgba);
			}

			cursor++;
			assetDumpSetStatus("XBLA textures: %d of %d", cursor, total);
		} else {
			texpackDumpXblaClose();
			assetDumpLogPhase("XBLA textures", numRecords);
			phase = PHASE_MODELS;
			cursor = 1;
			total = NUM_FILES;
		}
		break;
	case PHASE_MODELS:
		if (cursor < NUM_FILES) {
			const char *name = romdataFileGetName(cursor);

			// The characters, the props and the guns are the ROM's models,
			// named C, P and G; what else has those initials is caught by
			// the header check in assetDumpLoadModel().
			if (name && (name[0] == 'C' || name[0] == 'P' || name[0] == 'G')) {
				if (assetDumpModel(cursor, name)) {
					numModels++;
				} else {
					numRefused++;
					sysLogPrintf(LOG_NOTE, "assetdump: file %d (%s) is not a model, or has no geometry", cursor, name);
				}
			}

			cursor++;
			assetDumpSetStatus("Models: file %d of %d (%d written)", cursor, NUM_FILES, numModels);
		} else {
			assetDumpLogPhase("models", numModels);
			phase = PHASE_MESHES;
			cursor = 0;
			total = haveXbla ? xblaMeshGetNumPackageSlots() : 0;
		}
		break;
	case PHASE_MESHES:
		if (cursor < total) {
			numMeshes += assetDumpMesh(cursor, total);
			cursor++;
			assetDumpSetStatus("XBLA meshes: slot %d of %d (%d written)", cursor, total, numMeshes);
		} else {
			assetDumpLogPhase("XBLA meshes", numMeshes);
			phase = PHASE_GE_TEXTURES;
			cursor = 0;
			assetDumpConvsFind();
			total = assetDumpGeTexturesNext();
		}
		break;
	case PHASE_GE_TEXTURES:
		if (cursor < total) {
			numGeTextures += assetDumpGeTexture(jobList[cursor]);
			cursor++;
			assetDumpSetStatus("%s textures: %d of %d", convs[convAt].game, cursor, total);
		} else {
			// one conversion after another, GoldenEye's first
			assetDumpGeTexturesEnd();
			cursor = 0;
			total = assetDumpGeTexturesNext();

			if (total <= 0) {
				assetDumpLogPhase("converted GoldenEye textures", numGeTextures);
				phase = PHASE_GE_MODELS;
				total = assetDumpGeModelsBegin();
			}
		}
		break;
	case PHASE_GE_MODELS:
		if (cursor < total) {
			if (assetDumpGeModel(jobList[cursor])) {
				numGeModels++;
			} else {
				numRefused++;
				sysLogPrintf(LOG_NOTE, "assetdump: GoldenEye's %s is not a model, or has no geometry", jobList[cursor]);
			}

			cursor++;
			assetDumpSetStatus("GoldenEye models: %d of %d", cursor, total);
		} else {
			assetDumpGeModelsEnd();
			assetDumpLogPhase("GoldenEye models", numGeModels);
			phase = PHASE_BEAN;
			cursor = 0;
			// Unpacks the GoldenEye release's archive if nothing has yet
			total = assetDumpBeanBegin();
		}
		break;
	case PHASE_BEAN:
		if (cursor < total) {
			assetDumpBeanStep(jobList[cursor]);
			cursor++;
			assetDumpSetStatus("GoldenEye XBLA: %d of %d (%d models)", cursor, total, numBeanModels);
		} else {
			assetDumpLogPhase("GoldenEye XBLA models and pictures", numBeanModels + numBeanTextures);
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

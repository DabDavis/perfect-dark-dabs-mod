/**
 * GoldenEye 007 for the Xbox 360 (Rare's "Project Bean" build) drawn on
 * GoldenEye X's characters. See gebean.h, and CLAUDE-notes/ge-bean.md for the
 * formats.
 *
 * Every file of Bean's is a Rare CAFF 07.08.06.0036 bundle, uncompressed and
 * big-endian. A character is a rendergraph: vertex and index buffers in the
 * .gpu section, a command stream in .stream naming a vertex buffer, a
 * material, a bone palette and a draw, and a 16 bone SKEL_* skeleton whose
 * bind is translation only. What this does with one is what
 * .xbla-work/ge-bean/bean2pack.py did offline, with the one step the OBJ
 * could not carry kept: the weights. Each Bean bone is turned so its segment
 * lies along the matching joint of the N64 model's rest pose (which is a star:
 * arms out, legs splayed), the whole is scaled to the N64 skeleton, and every
 * vertex keeps up to three of Bean's bones - as the model's own matrices, with
 * each joint's rest as the inverse bind. The knees and elbows then bend with
 * the skin instead of creasing where two rigid pieces meet.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "config.h"
#include "system.h"
#include "fs.h"
#include "romdata.h"
#include "archive.h"
#include "x360.h"
#include "xblatex.h"
#include "xblamesh.h"
#include "gebean.h"

#ifndef PLATFORM_N64

#define GEBEAN_XBLA_DIR "xbla"
#define GEBEAN_CACHE_DIR "cache"
#define GEBEAN_DONE_FILE ".extracted"
#define GEBEAN_SCAN_DEPTH 2

// What says a folder is Bean's, and which of an archive's entries are wanted:
// the characters and heads, where Rare put them. The rest of the archive is
// levels, guns, music and the N64-look originals.
#define GEBEAN_TREE "files/new/char"
#define GEBEAN_WANT_CHARS "files/new/char/"
#define GEBEAN_WANT_HEADS "files/new/head/"

#define GEBEAN_BODY           0
#define GEBEAN_BODY_WITH_HEAD 1
#define GEBEAN_HEAD           2

/**
 * Bean's units in GoldenEye's. A body measures its own (the two skeletons'
 * limb lengths summed) and every one the offline batch converted came out at
 * 0.2130; a head file has no skeleton to measure against, and takes that.
 */
#define GEBEAN_HEAD_SCALE 0.213f

#define GEBEAN_MAXMTX   64
#define GEBEAN_MAXVERTS 65535
#define GEBEAN_MAXDRAWS 0x1000

#define BEAN_MAXDRAWS 512
#define BEAN_MAXBONES 32
#define BEAN_MAXPAL   64
#define BEAN_MAXIBS   256
#define CAFF_MAXSECTS 16

struct gebeanrow {
	const char *file;
	s16 numnodes;
	s16 numvertices;
	u8 kind;
	const char *source;
};

static const struct gebeanrow rows[] = {
#include "gebeantable.h"
};

static s32 enabled = 0;

PD_CONSTRUCTOR static void gebeanConfigInit(void)
{
	configRegisterInt("Mod.XblaGoldenEye", &enabled, 0, 1);
}

s32 gebeanGetEnabled(void)
{
	return enabled;
}

void gebeanSetEnabled(s32 on)
{
	enabled = on ? 1 : 0;
}

static u32 gebeanBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 gebeanBE16(const u8 *p)
{
	return (u16)(((u32)p[0] << 8) | p[1]);
}

static f32 gebeanBEF32(const u8 *p)
{
	union { u32 u; f32 f; } bits;
	bits.u = gebeanBE32(p);
	return bits.f;
}

static void gebeanPutBE32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static void gebeanPutBEF32(u8 *p, f32 f)
{
	union { u32 u; f32 f; } bits;
	bits.f = f;
	gebeanPutBE32(p, bits.u);
}

/** [ofs, ofs + len) inside size, written so that it cannot wrap. */
static s32 gebeanFits(u64 ofs, u64 len, u64 size)
{
	return ofs <= size && len <= size - ofs;
}

/* -------------------------------------------------------------------------
 * Finding the copy
 * ------------------------------------------------------------------------- */

static char rootPath[FS_MAXPATH + 1];    // .../files/new, once it is on disk
static char archivePath[FS_MAXPATH + 1]; // what the player dropped, when it is an archive
static s32 scanned;
static s32 unpackFailed;

static s32 gebeanIsDir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static s32 gebeanHasTree(const char *dir)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/" GEBEAN_TREE, dir);

	return gebeanIsDir(path);
}

struct gebeanscan {
	const char *dir;
	s32 archives;
	s32 depth;
	char found[FS_MAXPATH + 1];
};

static s32 gebeanScan(const char *dir, s32 archives, s32 depth, char *dst, u32 dstLen);

static void gebeanScanEntry(const char *name, void *arg)
{
	struct gebeanscan *scan = arg;
	char path[FS_MAXPATH + 1];

	if (scan->found[0] || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", scan->dir, name);

	if (gebeanIsDir(path)) {
		if (!scan->archives && gebeanHasTree(path)) {
			snprintf(scan->found, sizeof(scan->found), "%s", path);
			return;
		}

		if (scan->depth > 0) {
			gebeanScan(path, scan->archives, scan->depth - 1, scan->found, sizeof(scan->found));
		}

		return;
	}

	// An archive is looked into, never taken on its name: the Perfect Dark
	// release sits in the same folder, as an archive of its own.
	if (scan->archives && archiveIsSupported(path) && archiveFindEntry(path, GEBEAN_WANT_CHARS)) {
		snprintf(scan->found, sizeof(scan->found), "%s", path);
	}
}

/** The first Bean folder (holding files/new/char) or Bean archive at or under an expanded dir. */
static s32 gebeanScan(const char *dir, s32 archives, s32 depth, char *dst, u32 dstLen)
{
	struct gebeanscan scan;

	if (!archives && gebeanHasTree(dir)) {
		snprintf(dst, dstLen, "%s", dir);
		return 1;
	}

	memset(&scan, 0, sizeof(scan));
	scan.dir = dir;
	scan.archives = archives;
	scan.depth = depth;

	fsScanDir(dir, gebeanScanEntry, &scan);

	if (!scan.found[0]) {
		return 0;
	}

	snprintf(dst, dstLen, "%s", scan.found);

	return 1;
}

/** cache/xbla/goldeneye/, made if it has to be. dst gets the expanded path. */
static s32 gebeanCacheDir(char *dst, u32 dstLen)
{
	char rel[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	if (fsChooseOutputDir(GEBEAN_CACHE_DIR, rel, sizeof(rel)) != 0) {
		return 0;
	}

	snprintf(sub, sizeof(sub), "%s/xbla", rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(sub, sizeof(sub), "%s/xbla/goldeneye", rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(dst, dstLen, "%s", fsFullPath(sub));

	return 1;
}

static s32 gebeanWantEntry(const char *name, void *arg)
{
	char lower[FS_MAXPATH + 1];
	size_t i;

	for (i = 0; name[i] && i + 1 < sizeof(lower); i++) {
		char c = name[i] == '\\' ? '/' : name[i];
		lower[i] = (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
	}

	lower[i] = '\0';

	return strstr(lower, GEBEAN_WANT_CHARS) != NULL || strstr(lower, GEBEAN_WANT_HEADS) != NULL;
}

static void gebeanSetRoot(const char *tree)
{
	snprintf(rootPath, sizeof(rootPath), "%s/files/new", tree);
	sysLogPrintf(LOG_NOTE, "gebean: GoldenEye XBLA characters in %s", rootPath);
}

/**
 * 1 when rootPath names the characters on disk. The folders are scanned once;
 * an archive is unpacked only when mayUnpack says the caller is the one to pay
 * for it, and only once.
 */
static s32 gebeanLocate(s32 mayUnpack)
{
	static const char *const dirs[] = {
		"$E/" GEBEAN_XBLA_DIR,
		"$H/" GEBEAN_XBLA_DIR,
		"./" GEBEAN_XBLA_DIR,
		"$S/" GEBEAN_XBLA_DIR,
	};
	char cache[FS_MAXPATH + 1];
	char marker[FS_MAXPATH + 1];
	char found[FS_MAXPATH + 1];
	s32 written;
	FILE *fp;

	if (rootPath[0]) {
		return 1;
	}

	if (!scanned) {
		scanned = 1;

		// A folder the player unpacked themselves before an archive to unpack.
		for (s32 d = 0; d < ARRAYCOUNT(dirs); d++) {
			char dir[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s", fsFullPath(dirs[d]));

			if (gebeanIsDir(dir) && gebeanScan(dir, 0, GEBEAN_SCAN_DEPTH, found, sizeof(found))) {
				gebeanSetRoot(found);
				return 1;
			}
		}

		for (s32 d = 0; d < ARRAYCOUNT(dirs) && !archivePath[0]; d++) {
			char dir[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s", fsFullPath(dirs[d]));

			if (gebeanIsDir(dir)) {
				gebeanScan(dir, 1, GEBEAN_SCAN_DEPTH, archivePath, sizeof(archivePath));
			}
		}

		if (archivePath[0]) {
			sysLogPrintf(LOG_NOTE, "gebean: the GoldenEye XBLA release is %s", archivePath);
		}
	}

	if (!archivePath[0] || !gebeanCacheDir(cache, sizeof(cache))) {
		return 0;
	}

	snprintf(marker, sizeof(marker), "%s/" GEBEAN_DONE_FILE, cache);

	if (fsFileSize(marker) >= 0 && gebeanScan(cache, 0, GEBEAN_SCAN_DEPTH + 1, found, sizeof(found))) {
		gebeanSetRoot(found);
		return 1;
	}

	if (!mayUnpack || unpackFailed) {
		return 0;
	}

	sysLogPrintf(LOG_NOTE, "gebean: unpacking the characters from %s into %s, this happens once",
			archivePath, cache);

	written = archiveExtractMatching(archivePath, cache, gebeanWantEntry, NULL);

	if (written <= 0 || !gebeanScan(cache, 0, GEBEAN_SCAN_DEPTH + 1, found, sizeof(found))) {
		sysLogPrintf(LOG_ERROR, "gebean: no GoldenEye characters came out of %s", archivePath);
		unpackFailed = 1;
		return 0;
	}

	fp = fopen(fsFullPath(marker), "wb");

	if (fp) {
		fclose(fp);
	}

	sysLogPrintf(LOG_NOTE, "gebean: unpacked %d files", written);
	gebeanSetRoot(found);

	return 1;
}

s32 gebeanIsAvailable(void)
{
	return gebeanLocate(0) || archivePath[0];
}

s32 gebeanPrepare(void)
{
	return gebeanLocate(1);
}

/* -------------------------------------------------------------------------
 * The model file this pairs with
 * ------------------------------------------------------------------------- */

/** The next node of the game's depth-first walk (xblaMeshEnumListNodes()'s order). */
static struct modelnode *gebeanNextNode(struct modelnode *node)
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

const char *gebeanRowName(s32 row)
{
	return row >= 0 && row < ARRAYCOUNT(rows) ? rows[row].file : "?";
}

s32 gebeanFindRow(u16 fileid, struct modeldef *modeldef)
{
	const char *name;
	s32 row = -1;
	s32 nodes = 0;
	s32 verts = 0;
	s32 walked = 0;

	if (!modeldef || !modeldef->rootnode || romdataFileIsStock(fileid)) {
		return -1;
	}

	name = romdataFileGetName(fileid);

	if (!name) {
		return -1;
	}

	for (s32 i = 0; i < ARRAYCOUNT(rows); i++) {
		if (strcmp(rows[i].file, name) == 0) {
			row = i;
			break;
		}
	}

	if (row < 0) {
		return -1;
	}

	for (struct modelnode *node = modeldef->rootnode; node && walked < 4096; node = gebeanNextNode(node), walked++) {
		const u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_DL) {
			nodes++;
			verts += node->rodata->dl.numvertices;
		} else if (type == MODELNODETYPE_GUNDL) {
			nodes++;
			verts += node->rodata->gundl.numvertices;
		}
	}

	if (nodes != rows[row].numnodes || verts != rows[row].numvertices) {
		sysLogPrintf(LOG_NOTE, "gebean: model file %d is called %s but is not GoldenEye X's "
				"(%d lists, %d vertices against %d and %d) - left alone",
				fileid, name, nodes, verts, rows[row].numnodes, rows[row].numvertices);
		return -1;
	}

	return row;
}

s32 gebeanListNodeMatrix(const struct modelnode *node)
{
	s32 walked = 0;

	for (node = node ? node->parent : NULL; node && walked < 64; node = node->parent, walked++) {
		switch (node->type & 0xff) {
		case MODELNODETYPE_POSITION:
			return node->rodata->position.mtxindex0;
		case MODELNODETYPE_CHRINFO:
			return node->rodata->chrinfo.mtxindex;
		case MODELNODETYPE_POSITIONHELD:
			return node->rodata->positionheld.mtxindex;
		case MODELNODETYPE_HEADSPOT:
			return 0;
		}
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * CAFF
 * ------------------------------------------------------------------------- */

struct caffsect {
	char name[16];
	u64 offset;
	u32 size;
};

struct cafffile {
	u32 asset;
	u32 start;
	u32 size;
	u8 sect;
};

struct caff {
	const u8 *d;
	u32 len;
	s32 be;
	s32 numsects;
	struct caffsect sects[CAFF_MAXSECTS];
	u32 numassets;
	u32 nametable; // numassets offsets into the labels
	u32 labels;
	u32 numfiles;
	struct cafffile *files;
};

static u32 caffHeader32(const struct caff *c, u64 o)
{
	const u8 *p;

	if (!gebeanFits(o, 4, c->len)) {
		return 0;
	}

	p = c->d + o;

	return c->be ? gebeanBE32(p) : ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
}

/**
 * The layout is CAFFeinated's BundleV36 (OlieGamerTV) checked against Bean's
 * own files: a header, the section table, the asset names, one more table the
 * files do not need, and a 14 byte record per file naming its asset, section,
 * start and size. The sections' data follows in section order.
 */
static s32 caffOpen(struct caff *c, const u8 *d, u32 len)
{
	u32 stroff[CAFF_MAXSECTS];
	u64 pos;
	u64 data;
	u32 hsize, namelen, sectsize, filesize, total, adb;

	memset(c, 0, sizeof(*c));
	c->d = d;
	c->len = len;

	if (len < 0x68 || memcmp(d, "CAFF", 4) != 0) {
		return 0;
	}

	c->be = d[0x48] == 1;
	hsize = caffHeader32(c, 0x14);
	c->numassets = caffHeader32(c, 0x1c);
	c->numfiles = caffHeader32(c, 0x20);
	c->numsects = d[0x49];
	namelen = caffHeader32(c, 0x4c);
	sectsize = caffHeader32(c, 0x50);
	filesize = caffHeader32(c, 0x64);

	if (d[0x4a] != 0) {
		sysLogPrintf(LOG_WARNING, "gebean: a compressed CAFF (%u) is not one this reads", d[0x4a]);
		return 0;
	}

	if (c->numsects > CAFF_MAXSECTS || c->numassets > 0x10000 || c->numfiles > 0x10000) {
		return 0;
	}

	pos = hsize;

	if (!gebeanFits(pos, 0x21ull * c->numsects, len)) {
		return 0;
	}

	for (s32 i = 0; i < c->numsects; i++) {
		stroff[i] = caffHeader32(c, pos);
		c->sects[i].size = caffHeader32(c, pos + 9);
		pos += 0x21;
	}

	for (s32 i = 0; i < c->numsects; i++) {
		const u64 at = pos + stroff[i];

		for (s32 j = 0; j < (s32)sizeof(c->sects[i].name) - 1 && at + j < len && d[at + j]; j++) {
			c->sects[i].name[j] = (char)d[at + j];
		}
	}

	pos += namelen;
	total = caffHeader32(c, pos);
	c->nametable = (u32)(pos + 4);
	c->labels = (u32)(pos + 4 + 4ull * c->numassets);
	pos = (u64)c->labels + total;
	adb = caffHeader32(c, pos);
	pos += 4ull + adb;

	if (!gebeanFits(pos, 14ull * c->numfiles, len)) {
		return 0;
	}

	c->files = calloc(c->numfiles ? c->numfiles : 1, sizeof(*c->files));

	if (!c->files) {
		return 0;
	}

	for (u32 i = 0; i < c->numfiles; i++) {
		c->files[i].asset = caffHeader32(c, pos);
		c->files[i].start = caffHeader32(c, pos + 4);
		c->files[i].size = caffHeader32(c, pos + 8);
		c->files[i].sect = d[pos + 12];
		pos += 14;
	}

	data = (u64)hsize + sectsize + filesize;

	for (s32 i = 0; i < c->numsects; i++) {
		c->sects[i].offset = data;
		data += c->sects[i].size;
	}

	return 1;
}

static void caffClose(struct caff *c)
{
	free(c->files);
	c->files = NULL;
}

static const u8 *caffBlob(const struct caff *c, s32 index, u32 *outLen)
{
	const struct cafffile *f;
	u64 at;

	if (index < 0 || (u32)index >= c->numfiles) {
		return NULL;
	}

	f = &c->files[index];

	if (f->sect == 0 || f->sect > c->numsects) {
		return NULL;
	}

	at = c->sects[f->sect - 1].offset + f->start;

	if (!gebeanFits(at, f->size, c->len)) {
		return NULL;
	}

	*outLen = f->size;

	return c->d + at;
}

static const char *caffSectName(const struct caff *c, s32 index)
{
	const u8 sect = c->files[index].sect;

	return sect && sect <= c->numsects ? c->sects[sect - 1].name : "";
}

/** The last component of an asset's name ("...\texture pairs"), or "". */
static const char *caffAssetName(const struct caff *c, u32 asset)
{
	u32 off;
	const char *name;
	const char *last;
	u64 at;

	if (asset == 0 || asset > c->numassets) {
		return "";
	}

	off = caffHeader32(c, c->nametable + 4ull * (asset - 1));
	at = (u64)c->labels + off;

	if (at >= c->len || !memchr(c->d + at, '\0', c->len - at)) {
		return "";
	}

	name = (const char *)c->d + at;
	last = strrchr(name, '\\');

	return last ? last + 1 : name;
}

/** The first file of an asset in a section whose name starts with prefix, or -1. */
static s32 caffFind(const struct caff *c, u32 asset, const char *prefix)
{
	for (u32 i = 0; i < c->numfiles; i++) {
		if (c->files[i].asset == asset && strncmp(caffSectName(c, i), prefix, strlen(prefix)) == 0) {
			return (s32)i;
		}
	}

	return -1;
}

/* -------------------------------------------------------------------------
 * The rendergraph
 * ------------------------------------------------------------------------- */

enum {
	SK_BASE, SK_BACK, SK_NECK, SK_POSITION,
	SK_LF_SHOULDER, SK_LF_ELBOW, SK_LF_WRIST,
	SK_RT_SHOULDER, SK_RT_ELBOW, SK_RT_WRIST,
	SK_LF_HIP, SK_LF_KNEE, SK_LF_ANKLE,
	SK_RT_HIP, SK_RT_KNEE, SK_RT_ANKLE,
	SK_COUNT
};

static const char *const skelNames[SK_COUNT] = {
	"SKEL_BASE", "SKEL_BACK", "SKEL_NECK", "SKEL_POSITION",
	"SKEL_LF_SHOULDER", "SKEL_LF_ELBOW", "SKEL_LF_WRIST",
	"SKEL_RT_SHOULDER", "SKEL_RT_ELBOW", "SKEL_RT_WRIST",
	"SKEL_LF_HIP", "SKEL_LF_KNEE", "SKEL_LF_ANKLE",
	"SKEL_RT_HIP", "SKEL_RT_KNEE", "SKEL_RT_ANKLE",
};

// The joint each bone's segment runs to, which is what the bone is turned to
// line up; -1 for the ends of the chains.
static const s8 skelChild[SK_COUNT] = {
	SK_BACK, SK_NECK, -1, -1,
	SK_LF_ELBOW, SK_LF_WRIST, -1,
	SK_RT_ELBOW, SK_RT_WRIST, -1,
	SK_LF_KNEE, SK_LF_ANKLE, -1,
	SK_RT_KNEE, SK_RT_ANKLE, -1,
};

// And the bone an end turns with.
static const s8 skelInherit[SK_COUNT] = {
	-1, -1, SK_BACK, SK_BASE,
	-1, -1, SK_LF_ELBOW,
	-1, -1, SK_RT_ELBOW,
	-1, -1, SK_LF_KNEE,
	-1, -1, SK_RT_KNEE,
};

struct beandraw {
	u32 vb;
	u32 tex;
	u32 prim;
	u32 count;
	u32 ib;
	u8 numpal;
	u8 pal[BEAN_MAXPAL];
};

struct beanib {
	u32 obj;
	u32 off;
	u32 size;
};

struct beanmodel {
	u8 *file;
	struct caff caff;
	const u8 *data;
	u32 datalen;
	const u8 *gpu;
	u32 gpulen;
	const u8 *stream;
	u32 streamlen;

	s32 numbones;
	s8 skel[BEAN_MAXBONES];      // SK_* per pose bone, or -1
	f32 bind[BEAN_MAXBONES][3];  // absolute

	s32 numremap;
	u16 remap[BEAN_MAXPAL];      // palette number -> pose bone

	s32 numdraws;
	struct beandraw *draws;

	s32 numtex;
	s32 texfile[GEBEAN_MAXMATS]; // a texture's header, by file index

	s32 numibs;
	struct beanib ibs[BEAN_MAXIBS];

	f32 uvscale;
};

struct beanvb {
	u32 stride;
	u32 off;
	u32 count;
	s32 col28; // stride 28 carries a colour rather than a UV
};

struct beanvtx {
	f32 pos[3];
	f32 nrm[3];
	f32 uv[2];
	s8 slot[4];   // palette slot, or -1
	u8 weight[4];
};

static s32 beanReadVb(const struct beanmodel *bm, u32 desc, struct beanvb *vb)
{
	u32 size;

	memset(vb, 0, sizeof(*vb));

	if (!gebeanFits(desc, 16, bm->datalen)) {
		return 0;
	}

	vb->stride = gebeanBE32(bm->data + desc);
	vb->off = gebeanBE32(bm->data + desc + 8);
	size = gebeanBE32(bm->data + desc + 12);

	if (vb->stride < 20 || !gebeanFits(vb->off, size, bm->gpulen)) {
		return 0;
	}

	vb->count = size / vb->stride;

	// Stride 28 is a skinned vertex with one of a UV and a colour: a colour's
	// alpha byte is 0xff on every vertex, a UV's high byte is not.
	if (vb->stride == 28) {
		vb->col28 = 1;

		for (u32 i = 0; i < vb->count; i++) {
			if (bm->gpu[vb->off + i * 28 + 24] != 0xff) {
				vb->col28 = 0;
				break;
			}
		}
	}

	return 1;
}

static void beanUnpackNormal(u32 w, f32 *out)
{
	for (s32 i = 0; i < 3; i++) {
		s32 v = (w >> (10 * i)) & 1023;

		if (v & 512) {
			v -= 1024;
		}

		out[i] = v / 511.0f;
	}
}

/**
 * One vertex. Every layout starts with a float position; a skinned one then
 * has four u16 palette slots written three times over (0xf000 for none), and
 * the stride 36 one four weight bytes in reverse; then a 10:10:10 normal, an
 * s16 UV and an ARGB colour, whichever of those the stride has room for.
 */
static s32 beanVertex(const struct beanmodel *bm, const struct beanvb *vb, u32 i, struct beanvtx *v)
{
	const u8 *p;
	u32 k;
	s32 hasuv = 1;

	if (i >= vb->count) {
		return 0;
	}

	p = bm->gpu + vb->off + i * vb->stride;
	memset(v, 0, sizeof(*v));

	for (s32 j = 0; j < 3; j++) {
		v->pos[j] = gebeanBEF32(p + j * 4);
	}

	for (s32 j = 0; j < 4; j++) {
		v->slot[j] = -1;
	}

	v->weight[0] = 255;

	switch (vb->stride) {
	case 28:
	case 32:
	case 36:
		for (s32 j = 0; j < 4; j++) {
			const u16 s = gebeanBE16(p + 12 + j * 2);
			v->slot[j] = s == 0xf000 || s / 3 >= BEAN_MAXPAL ? -1 : (s8)(s / 3);
		}

		if (vb->stride == 36) {
			for (s32 j = 0; j < 4; j++) {
				v->weight[j] = p[23 - j];
			}

			k = 24;
		} else {
			k = 20;
		}

		hasuv = !(vb->stride == 28 && vb->col28);
		break;
	case 20:
		k = 12;
		hasuv = 0;
		break;
	case 24:
		k = 12;
		break;
	default:
		return 0;
	}

	beanUnpackNormal(gebeanBE32(p + k), v->nrm);

	if (hasuv) {
		v->uv[0] = (s16)gebeanBE16(p + k + 4) / bm->uvscale;
		v->uv[1] = (s16)gebeanBE16(p + k + 6) / bm->uvscale;
	}

	return 1;
}

/**
 * What one texture repeat is in this file's s16 UVs. Rare exported both
 * 1/16384 and 1/32768 and nothing in the file says which; over every
 * character and head the largest UV is either at most 16404 (the uniformed
 * guards, the pilot, Xenia, Boris, six heads) or at least 32031. Divided by
 * the wrong one, a model samples the top-left quarter of its atlas - the black
 * guards and Boris's patchwork of the first batch.
 */
static f32 beanMeasureUvScale(struct beanmodel *bm)
{
	u32 biggest = 0;

	for (s32 d = 0; d < bm->numdraws; d++) {
		struct beanvb vb;
		u32 uvo;
		s32 seen = 0;

		for (s32 e = 0; e < d; e++) {
			if (bm->draws[e].vb == bm->draws[d].vb) {
				seen = 1;
				break;
			}
		}

		if (seen || !beanReadVb(bm, bm->draws[d].vb, &vb)) {
			continue;
		}

		switch (vb.stride) {
		case 36: uvo = 28; break;
		case 32: uvo = 24; break;
		case 28: uvo = 24; break;
		case 24: uvo = 16; break;
		default: continue;
		}

		if (vb.stride == 28 && vb.col28) {
			continue;
		}

		for (u32 i = 0; i < vb.count; i++) {
			const u8 *p = bm->gpu + vb.off + i * vb.stride + uvo;
			const s32 u = (s16)gebeanBE16(p);
			const s32 w = (s16)gebeanBE16(p + 2);
			const u32 au = (u32)(u < 0 ? -u : u);
			const u32 aw = (u32)(w < 0 ? -w : w);

			if (au > biggest) {
				biggest = au;
			}

			if (aw > biggest) {
				biggest = aw;
			}
		}
	}

	return biggest > 0 && biggest <= 17000 ? 16384.0f : 32768.0f;
}

/**
 * The command stream, first alternative at every switch. Each record is a
 * tagged u32 (size << 16 | type << 8): 0x12 palette remap, 0x13 bone palette,
 * 0x16 switch, 0x19 jump, 0x1d end, 0x2d material, 0x2e vertex buffer, 0x01
 * draw.
 */
static void beanWalkStream(struct beanmodel *bm)
{
	const u8 *st = bm->stream;
	const u32 len = bm->streamlen;
	u32 end;
	u32 pc = 0x24;
	u32 vb = 0;
	u32 tex = 0;
	u8 pal[BEAN_MAXPAL];
	u8 numpal = 1;

	pal[0] = 0;

	if (len < 0x28) {
		return;
	}

	end = gebeanBE32(st + 4);

	if (end > len) {
		end = len;
	}

	for (s32 steps = 0; pc + 4 <= end && steps < 100000; steps++) {
		const u32 tag = gebeanBE32(st + pc);
		const u32 size = tag >> 16;
		const u32 type = (tag >> 8) & 0xff;

		if (size < 4 || !gebeanFits(pc, size, len)) {
			break;
		}

		if (type == 0x16) {
			if (!gebeanFits(pc, 12, len)) {
				break;
			}

			pc = gebeanBE32(st + pc + 8);
			continue;
		}

		if (type == 0x19) {
			pc = gebeanBE32(st + pc + 4);
			continue;
		}

		if (type == 0x1d) {
			break;
		}

		if (type == 0x12 && size >= 12) {
			u32 count = gebeanBE16(st + pc + 8);

			if (count > BEAN_MAXPAL) {
				count = BEAN_MAXPAL;
			}

			bm->numremap = 0;

			for (u32 k = 0; k < count && gebeanFits(pc + 12 + 8 * k, 2, len); k++) {
				bm->remap[bm->numremap++] = gebeanBE16(st + pc + 12 + 8 * k);
			}
		} else if (type == 0x2e && size >= 12) {
			vb = gebeanBE32(st + pc + 8);
		} else if (type == 0x2d) {
			if (size == 20) {
				tex = gebeanBE32(st + pc + 12);
			} else if (size >= 24) {
				tex = gebeanBE32(st + pc + 20);
			}
		} else if (type == 0x13 && size >= 12) {
			u32 count = gebeanBE16(st + pc + 8);

			if (count > BEAN_MAXPAL) {
				count = BEAN_MAXPAL;
			}

			if (!gebeanFits(pc + 12, count, len)) {
				count = 0;
			}

			memcpy(pal, st + pc + 12, count);
			numpal = (u8)count;
		} else if (type == 0x01 && size >= 16 && bm->numdraws < BEAN_MAXDRAWS) {
			struct beandraw *d = &bm->draws[bm->numdraws++];

			d->vb = vb;
			d->tex = tex;
			d->prim = gebeanBE32(st + pc + 4);
			d->count = gebeanBE32(st + pc + 8);
			d->ib = gebeanBE32(st + pc + 12);
			d->numpal = numpal;
			memcpy(d->pal, pal, numpal);
		}

		pc += size;
	}
}

/**
 * The skeleton: a 'pose' record (19.12.06.0036) holds a count at +0x18 and
 * its entries at the offset in +0x34, 52 bytes each - a local translation,
 * the absolute bind, a spare, 1.0, then parent/child and sibling/self - in
 * the pool's SKEL_* name order.
 */
static void beanReadPose(struct beanmodel *bm, const char **names, s32 numnames)
{
	const u8 *d = bm->data;
	u32 at;
	u32 count;
	u32 entries;

	for (at = 0; at + 0x38 <= bm->datalen; at += 4) {
		if (memcmp(d + at, "pose\0\0\0\0", 8) == 0) {
			break;
		}
	}

	if (at + 0x38 > bm->datalen) {
		return;
	}

	count = gebeanBE32(d + at + 0x18);
	entries = gebeanBE32(d + at + 0x34);

	if (count > BEAN_MAXBONES) {
		count = BEAN_MAXBONES;
	}

	for (u32 i = 0; i < count && gebeanFits(entries + 52ull * i, 52, bm->datalen); i++) {
		const u8 *e = d + entries + 52 * i;

		bm->skel[i] = -1;

		for (s32 k = 0; k < 3; k++) {
			bm->bind[i][k] = gebeanBEF32(e + 12 + k * 4);
		}

		if ((s32)i < numnames) {
			for (s32 s = 0; s < SK_COUNT; s++) {
				if (strcmp(names[i], skelNames[s]) == 0) {
					bm->skel[i] = (s8)s;
					break;
				}
			}
		}

		bm->numbones = (s32)i + 1;
	}
}

/**
 * The index buffers. A draw names a runtime object; the descriptor is the
 * table row {object, .gpu offset, byte size, 1} that holds it, found by
 * looking for rows of that shape in .data - first one wins.
 */
static void beanFindIndexBuffers(struct beanmodel *bm)
{
	const u8 *d = bm->data;

	for (u32 o = 0; o + 16 <= bm->datalen && bm->numibs < BEAN_MAXIBS; o += 4) {
		const u32 obj = gebeanBE32(d + o);
		const u32 off = gebeanBE32(d + o + 4);
		const u32 size = gebeanBE32(d + o + 8);
		s32 known = 0;

		if (gebeanBE32(d + o + 12) != 1 || size == 0 || (size & 1) || !gebeanFits(off, size, bm->gpulen)
				|| obj >= bm->datalen) {
			continue;
		}

		for (s32 i = 0; i < bm->numibs; i++) {
			if (bm->ibs[i].obj == obj) {
				known = 1;
				break;
			}
		}

		if (!known) {
			bm->ibs[bm->numibs].obj = obj;
			bm->ibs[bm->numibs].off = off;
			bm->ibs[bm->numibs].size = size;
			bm->numibs++;
		}
	}
}

/** A draw's triangles as index triples into its vertex buffer. The caller frees *out. */
static s32 beanTriangles(const struct beanmodel *bm, const struct beandraw *d, u16 **out)
{
	const struct beanib *ib = NULL;
	const u8 *idx;
	u16 *tris;
	s32 n = 0;

	*out = NULL;

	for (s32 i = 0; i < bm->numibs; i++) {
		if (bm->ibs[i].obj == d->ib) {
			ib = &bm->ibs[i];
			break;
		}
	}

	if (!ib || d->count < 3 || (u64)d->count * 2 > ib->size) {
		return 0;
	}

	idx = bm->gpu + ib->off;
	tris = malloc(sizeof(u16) * 3 * ((size_t)d->count * 2));

	if (!tris) {
		return 0;
	}

	if (d->prim == 4) {
		for (u32 i = 0; i + 2 < d->count; i += 3, n++) {
			tris[n * 3] = gebeanBE16(idx + i * 2);
			tris[n * 3 + 1] = gebeanBE16(idx + (i + 1) * 2);
			tris[n * 3 + 2] = gebeanBE16(idx + (i + 2) * 2);
		}
	} else if (d->prim == 13) {
		for (u32 i = 0; i + 3 < d->count; i += 4) {
			const u16 a = gebeanBE16(idx + i * 2);
			const u16 b = gebeanBE16(idx + (i + 1) * 2);
			const u16 c = gebeanBE16(idx + (i + 2) * 2);
			const u16 e = gebeanBE16(idx + (i + 3) * 2);

			tris[n * 3] = a; tris[n * 3 + 1] = b; tris[n * 3 + 2] = c; n++;
			tris[n * 3] = a; tris[n * 3 + 1] = c; tris[n * 3 + 2] = e; n++;
		}
	} else if (d->prim == 5) {
		for (u32 i = 0; i + 2 < d->count; i++, n++) {
			const u16 a = gebeanBE16(idx + i * 2);
			const u16 b = gebeanBE16(idx + (i + 1) * 2);
			const u16 c = gebeanBE16(idx + (i + 2) * 2);

			tris[n * 3] = (i & 1) ? b : a;
			tris[n * 3 + 1] = (i & 1) ? a : b;
			tris[n * 3 + 2] = c;
		}
	}

	*out = tris;

	return n;
}

static void beanFree(struct beanmodel *bm)
{
	caffClose(&bm->caff);
	free(bm->draws);
	free(bm->file);
	memset(bm, 0, sizeof(*bm));
}

static s32 beanLoad(struct beanmodel *bm, const char *source)
{
	const char *names[BEAN_MAXBONES];
	s32 numnames = 0;
	char path[FS_MAXPATH + 1];
	FILE *fp;
	long size;
	struct caff *c = &bm->caff;
	s32 idata = -1, igpu = -1, istream = -1, ipool = -1;
	const u8 *pool;
	u32 poollen = 0;

	memset(bm, 0, sizeof(*bm));
	snprintf(path, sizeof(path), "%s/%s/default.bin", rootPath, source);

	fp = fopen(path, "rb");

	if (!fp) {
		sysLogPrintf(LOG_ERROR, "gebean: %s is missing", path);
		return 0;
	}

	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) <= 0 || size > 64 * 1024 * 1024) {
		fclose(fp);
		return 0;
	}

	rewind(fp);
	bm->file = malloc((size_t)size);

	if (!bm->file || fread(bm->file, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		beanFree(bm);
		return 0;
	}

	fclose(fp);

	bm->draws = calloc(BEAN_MAXDRAWS, sizeof(*bm->draws));

	if (!bm->draws || !caffOpen(c, bm->file, (u32)size)) {
		sysLogPrintf(LOG_ERROR, "gebean: %s is not a CAFF this reads", path);
		beanFree(bm);
		return 0;
	}

	idata = caffFind(c, 1, ".data");
	igpu = caffFind(c, 1, ".gpu");
	istream = caffFind(c, 1, ".stream");
	ipool = caffFind(c, 2, ".data");

	if (idata < 0 || igpu < 0 || istream < 0) {
		sysLogPrintf(LOG_ERROR, "gebean: %s is not a rendergraph", path);
		beanFree(bm);
		return 0;
	}

	bm->data = caffBlob(c, idata, &bm->datalen);
	bm->gpu = caffBlob(c, igpu, &bm->gpulen);
	bm->stream = caffBlob(c, istream, &bm->streamlen);

	if (!bm->data || !bm->gpu || !bm->stream) {
		beanFree(bm);
		return 0;
	}

	// The pool names the bones in pose order among its other strings.
	pool = ipool >= 0 ? caffBlob(c, ipool, &poollen) : NULL;

	for (u32 at = 0; pool && at < poollen && numnames < BEAN_MAXBONES; ) {
		const u8 *nul = memchr(pool + at, '\0', poollen - at);
		const u32 end = nul ? (u32)(nul - pool) : poollen;

		if (nul && end - at > 5 && memcmp(pool + at, "SKEL_", 5) == 0) {
			names[numnames++] = (const char *)pool + at;
		}

		at = end + 1;
	}

	for (u32 i = 0; i < c->numfiles && bm->numtex < GEBEAN_MAXMATS; i++) {
		u32 blen;
		const u8 *b = caffBlob(c, (s32)i, &blen);

		if (b && blen >= 8 && memcmp(b, "texture\0", 8) == 0) {
			bm->texfile[bm->numtex++] = (s32)i;
		}
	}

	beanReadPose(bm, names, numnames);
	beanWalkStream(bm);
	beanFindIndexBuffers(bm);
	bm->uvscale = beanMeasureUvScale(bm);

	return 1;
}

/* -------------------------------------------------------------------------
 * The pictures
 * ------------------------------------------------------------------------- */

#define GEBEAN_TEXCACHE 512

static struct {
	char key[80];
	const void *tile;
	u8 alpha;
	u8 soft;
} texCache[GEBEAN_TEXCACHE];
static s32 numTexCache;

/**
 * A texture as RGBA in the game's row order. The header is a D3D texture: the
 * format word at +0x18 (its low byte 0x52/0x53/0x54 DXT1/3/5, 0x86 8888, with
 * 0x40 the tiled bit and byte +0x1a the 8-in-16 swap), width and height at
 * +0x24. The texels are the asset's own .gpu entry, or - where it has none -
 * the file's shared "texture pairs" .gpu entry at the base offset in +0x28.
 */
static u8 *beanDecodeTexture(const struct beanmodel *bm, s32 t, s32 *outW, s32 *outH)
{
	const struct caff *c = &bm->caff;
	struct x360fetch fetch;
	u32 blen, glen;
	const u8 *b = caffBlob(c, bm->texfile[t], &blen);
	const u8 *g;
	s32 gi;
	u32 base, first, w, h, bpe, ew, eh;
	u64 need, have;
	u8 *copy;
	u8 *rgba;

	if (!b || blen < 0x40) {
		return NULL;
	}

	w = gebeanBE16(b + 0x24);
	h = gebeanBE16(b + 0x26);
	base = gebeanBE32(b + 0x28);
	first = 0;

	if (gebeanBE32(b + 0x38) && gebeanFits(gebeanBE32(b + 0x3c), 4, blen)) {
		first = gebeanBE32(b + gebeanBE32(b + 0x3c));
	}

	gi = caffFind(c, c->files[bm->texfile[t]].asset, ".gpu");

	if (gi >= 0) {
		base = 0;
	} else {
		for (u32 i = 0; i < c->numfiles && gi < 0; i++) {
			if (strcmp(caffAssetName(c, c->files[i].asset), "texture pairs") == 0
					&& strncmp(caffSectName(c, (s32)i), ".gpu", 4) == 0) {
				gi = (s32)i;
			}
		}
	}

	g = gi >= 0 ? caffBlob(c, gi, &glen) : NULL;

	if (!g || w == 0 || h == 0 || w > 4096 || h > 4096 || (u64)base + first > glen) {
		return NULL;
	}

	memset(&fetch, 0, sizeof(fetch));
	fetch.width = w;
	fetch.height = h;
	fetch.format = b[0x1b] & 0x3f;
	fetch.tiled = 1;
	fetch.endian = b[0x1a] ? (fetch.format == X360_FMT_8888 ? 2 : 1) : 0;

	if (!x360FetchSupported(&fetch)) {
		sysLogPrintf(LOG_WARNING, "gebean: texture format %02x is not one this decodes", b[0x1b]);
		return NULL;
	}

	// The whole tiled surface is what the decode reaches into; a file that
	// stops short of it is padded rather than refused.
	if (fetch.format == X360_FMT_8888) {
		bpe = 4;
		fetch.pitch = w;
		ew = (w + 31) & ~31u;
		eh = (h + 31) & ~31u;
	} else {
		bpe = fetch.format == X360_FMT_DXT1 ? 8 : 16;
		fetch.pitch = (w + 3) & ~3u;
		ew = (((w + 3) / 4) + 31) & ~31u;
		eh = (((h + 3) / 4) + 31) & ~31u;
	}

	need = (u64)ew * eh * bpe;
	have = glen - ((u64)base + first);
	copy = calloc((size_t)need, 1);
	rgba = malloc((size_t)w * h * 4);

	if (!copy || !rgba) {
		free(copy);
		free(rgba);
		return NULL;
	}

	memcpy(copy, g + base + first, (size_t)(have < need ? have : need));

	if (!x360DecodeTexture(copy, (u32)need, &fetch, rgba)) {
		free(copy);
		free(rgba);
		return NULL;
	}

	free(copy);

	// Decoded top row first, as a PNG of it would be; the renderer wants the
	// first uploaded row first (modelpackBindMaterial() does the same).
	for (u32 y = 0; y < h / 2; y++) {
		u8 *ra = rgba + (size_t)y * w * 4;
		u8 *rb = rgba + (size_t)(h - 1 - y) * w * 4;

		for (u32 x = 0; x < w * 4; x++) {
			const u8 tmp = ra[x];
			ra[x] = rb[x];
			rb[x] = tmp;
		}
	}

	*outW = (s32)w;
	*outH = (s32)h;

	return rgba;
}

static s32 beanBindTexture(const struct beanmodel *bm, const char *source, s32 t,
		const void **tile, u8 *alpha, u8 *soft)
{
	char key[80];
	s32 w, h, a = 0, s = 0;
	u8 *rgba;

	snprintf(key, sizeof(key), "gebean:%s:%d", source, t);

	for (s32 i = 0; i < numTexCache; i++) {
		if (strcmp(texCache[i].key, key) == 0) {
			*tile = texCache[i].tile;
			*alpha = texCache[i].alpha;
			*soft = texCache[i].soft;
			return *tile != NULL;
		}
	}

	rgba = beanDecodeTexture(bm, t, &w, &h);
	*tile = rgba ? xblaTexBindImage(key, rgba, w, h) : NULL;

	if (*tile) {
		xblaTexImageInfo(*tile, &a, &s);
	} else {
		sysLogPrintf(LOG_WARNING, "gebean: texture %d of %s would not decode", t, source);
	}

	*alpha = (u8)a;
	*soft = (u8)s;

	if (numTexCache < GEBEAN_TEXCACHE) {
		snprintf(texCache[numTexCache].key, sizeof(texCache[numTexCache].key), "%s", key);
		texCache[numTexCache].tile = *tile;
		texCache[numTexCache].alpha = *alpha;
		texCache[numTexCache].soft = *soft;
		numTexCache++;
	}

	return *tile != NULL;
}

/* -------------------------------------------------------------------------
 * The mesh
 * ------------------------------------------------------------------------- */

static f32 vecLen(const f32 *a)
{
	return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

/** The shortest rotation taking direction a to direction b (Rodrigues), rows. */
static void rotationBetween(const f32 *a0, const f32 *b0, f32 r[3][3])
{
	const f32 la = vecLen(a0);
	const f32 lb = vecLen(b0);
	f32 a[3], b[3], v[3], vx[3][3];
	f32 c, k;

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			r[i][j] = i == j ? 1.0f : 0.0f;
		}
	}

	if (la < 1e-6f || lb < 1e-6f) {
		return;
	}

	for (s32 i = 0; i < 3; i++) {
		a[i] = a0[i] / la;
		b[i] = b0[i] / lb;
	}

	v[0] = a[1] * b[2] - a[2] * b[1];
	v[1] = a[2] * b[0] - a[0] * b[2];
	v[2] = a[0] * b[1] - a[1] * b[0];
	c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

	if (vecLen(v) < 1e-9f) {
		if (c < 0.0f) {
			for (s32 i = 0; i < 3; i++) {
				r[i][i] = -1.0f;
			}
		}

		return;
	}

	vx[0][0] = 0.0f;  vx[0][1] = -v[2]; vx[0][2] = v[1];
	vx[1][0] = v[2];  vx[1][1] = 0.0f;  vx[1][2] = -v[0];
	vx[2][0] = -v[1]; vx[2][1] = v[0];  vx[2][2] = 0.0f;
	k = 1.0f / (1.0f + c);

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			f32 sq = 0.0f;

			for (s32 m = 0; m < 3; m++) {
				sq += vx[i][m] * vx[m][j];
			}

			r[i][j] += vx[i][j] + sq * k;
		}
	}
}

static void rotApply(const f32 r[3][3], const f32 *p, f32 *out)
{
	for (s32 i = 0; i < 3; i++) {
		out[i] = r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2];
	}
}

/** What the N64 model's rest skeleton says, by Bean bone. */
struct beanrig {
	s32 have[SK_COUNT];
	f32 joint[SK_COUNT][3];
	s32 mtx[SK_COUNT];
	f32 rot[SK_COUNT][3][3];
	f32 scale;

	// Every joint of the model by matrix, for the palette's inverse binds.
	s32 hasrest[GEBEAN_MAXMTX];
	f32 rest[GEBEAN_MAXMTX][3];
};

struct beanlimbjoint {
	f32 ax;
	struct modelnode *node;
};

/**
 * The model's joints named as Bean's bones, from their rest positions: the
 * chrinfo root is the base, a joint on the middle line is the back below 200
 * units and the neck above, and the rest are three to a limb - an arm above
 * 200, a leg below, left where x is positive - in order outwards. That is
 * GoldenEye's star rest pose, where the limbs run along x.
 */
static s32 beanRigFromModel(struct modeldef *modeldef, struct beanrig *rig,
		struct modelnode **joints, s8 *jointskel, s32 *outnumjoints)
{
	struct beanlimbjoint limbs[4][8];
	s32 numlimb[4] = { 0, 0, 0, 0 };
	s32 numjoints = 0;
	s32 walked = 0;

	memset(rig, 0, sizeof(*rig));

	for (struct modelnode *node = modeldef->rootnode; node && walked < 4096; node = gebeanNextNode(node), walked++) {
		const u32 type = node->type & 0xff;
		f32 rest[3];
		s32 mtx;
		s32 skel = -1;

		if (type == MODELNODETYPE_CHRINFO) {
			mtx = node->rodata->chrinfo.mtxindex;
			rest[0] = rest[1] = rest[2] = 0.0f;
		} else if (type == MODELNODETYPE_POSITION) {
			mtx = node->rodata->position.mtxindex0;
			xblaMeshNodeRestOffset(node, rest);
		} else {
			continue;
		}

		if (mtx >= 0 && mtx < GEBEAN_MAXMTX) {
			rig->hasrest[mtx] = 1;
			memcpy(rig->rest[mtx], rest, sizeof(rest));
		}

		if (type == MODELNODETYPE_CHRINFO) {
			skel = rig->have[SK_BASE] ? -1 : SK_BASE;
		} else if (fabsf(rest[0]) < 30.0f) {
			skel = rest[1] < 200.0f ? SK_BACK : SK_NECK;

			if (rig->have[skel]) {
				skel = -1;
			}
		} else {
			const s32 limb = (rest[0] > 0.0f ? 0 : 1) * 2 + (rest[1] > 200.0f ? 0 : 1);

			if (numlimb[limb] < 8) {
				limbs[limb][numlimb[limb]].ax = fabsf(rest[0]);
				limbs[limb][numlimb[limb]].node = node;
				numlimb[limb]++;
			}
		}

		if (numjoints < 64) {
			joints[numjoints] = node;
			jointskel[numjoints] = (s8)skel;
			numjoints++;
		}

		if (skel >= 0) {
			rig->have[skel] = 1;
			rig->mtx[skel] = mtx;
			memcpy(rig->joint[skel], rest, sizeof(rest));
		}
	}

	// limb 0 left arm, 1 left leg, 2 right arm, 3 right leg
	for (s32 limb = 0; limb < 4; limb++) {
		static const s8 order[4][3] = {
			{ SK_LF_SHOULDER, SK_LF_ELBOW, SK_LF_WRIST },
			{ SK_LF_HIP, SK_LF_KNEE, SK_LF_ANKLE },
			{ SK_RT_SHOULDER, SK_RT_ELBOW, SK_RT_WRIST },
			{ SK_RT_HIP, SK_RT_KNEE, SK_RT_ANKLE },
		};

		if (numlimb[limb] != 3) {
			sysLogPrintf(LOG_WARNING, "gebean: a limb of the model has %d joints, not 3", numlimb[limb]);
			return 0;
		}

		// Three, so a sort is a few swaps.
		for (s32 i = 0; i < 3; i++) {
			for (s32 j = i + 1; j < 3; j++) {
				if (limbs[limb][j].ax < limbs[limb][i].ax) {
					const struct beanlimbjoint tmp = limbs[limb][i];
					limbs[limb][i] = limbs[limb][j];
					limbs[limb][j] = tmp;
				}
			}
		}

		for (s32 i = 0; i < 3; i++) {
			const struct modelnode *node = limbs[limb][i].node;
			const s32 skel = order[limb][i];

			rig->have[skel] = 1;
			rig->mtx[skel] = node->rodata->position.mtxindex0;
			xblaMeshNodeRestOffset(node, rig->joint[skel]);

			for (s32 j = 0; j < numjoints; j++) {
				if (joints[j] == node) {
					jointskel[j] = (s8)skel;
				}
			}
		}
	}

	for (s32 s = 0; s < SK_COUNT; s++) {
		if (s != SK_POSITION && !rig->have[s]) {
			sysLogPrintf(LOG_WARNING, "gebean: the model has no joint for %s", skelNames[s]);
			return 0;
		}
	}

	rig->have[SK_POSITION] = 1;
	rig->mtx[SK_POSITION] = rig->mtx[SK_BASE];
	memcpy(rig->joint[SK_POSITION], rig->joint[SK_BASE], sizeof(rig->joint[0]));

	*outnumjoints = numjoints;

	return 1;
}

/** The scale and each bone's turn onto the rig, from Bean's bind. */
static s32 beanFitRig(struct beanrig *rig, const f32 bind[SK_COUNT][3], const s32 *havebind)
{
	f32 num = 0.0f;
	f32 den = 0.0f;

	for (s32 a = 0; a < SK_COUNT; a++) {
		const s32 b = skelChild[a];
		f32 dj[3], db[3];

		if (b < 0) {
			continue;
		}

		if (!havebind[a] || !havebind[b]) {
			sysLogPrintf(LOG_WARNING, "gebean: Bean's skeleton has no %s or %s", skelNames[a], skelNames[b]);
			return 0;
		}

		for (s32 k = 0; k < 3; k++) {
			dj[k] = rig->joint[b][k] - rig->joint[a][k];
			db[k] = bind[b][k] - bind[a][k];
		}

		if (a != SK_BASE) {
			num += vecLen(dj);
			den += vecLen(db);
		}

		rotationBetween(db, dj, rig->rot[a]);
	}

	for (s32 a = 0; a < SK_COUNT; a++) {
		if (skelInherit[a] >= 0) {
			memcpy(rig->rot[a], rig->rot[skelInherit[a]], sizeof(rig->rot[a]));
		}
	}

	rig->scale = den > 0.0f ? num / den : GEBEAN_HEAD_SCALE;

	return 1;
}

struct beantri {
	u16 group;
	u16 tex;
	u32 order;
	u16 v[3];
};

struct beanout {
	s32 numverts, capverts;
	f32 *pos;    // 3 per vertex
	f32 *nrm;    // 3
	f32 *uv;     // 2
	f32 *weight; // 3
	u8 *bone;    // 3
	s32 numtris, captris;
	struct beantri *tris;
};

static s32 beanAddVertex(struct beanout *o, const f32 *pos, const f32 *nrm, const f32 *uv,
		const u8 *bone, const f32 *weight)
{
	if (o->numverts >= GEBEAN_MAXVERTS) {
		return -1;
	}

	if (o->numverts >= o->capverts) {
		const s32 cap = o->capverts ? o->capverts * 2 : 4096;
		f32 *p = realloc(o->pos, cap * 3 * sizeof(f32));
		f32 *n = p ? realloc(o->nrm, cap * 3 * sizeof(f32)) : NULL;
		f32 *u = n ? realloc(o->uv, cap * 2 * sizeof(f32)) : NULL;
		f32 *w = u ? realloc(o->weight, cap * 3 * sizeof(f32)) : NULL;
		u8 *b = w ? realloc(o->bone, cap * 3) : NULL;

		if (p) o->pos = p;
		if (n) o->nrm = n;
		if (u) o->uv = u;
		if (w) o->weight = w;
		if (b) o->bone = b;

		if (!b) {
			return -1;
		}

		o->capverts = cap;
	}

	memcpy(o->pos + o->numverts * 3, pos, 3 * sizeof(f32));
	memcpy(o->nrm + o->numverts * 3, nrm, 3 * sizeof(f32));
	memcpy(o->uv + o->numverts * 2, uv, 2 * sizeof(f32));
	memcpy(o->weight + o->numverts * 3, weight, 3 * sizeof(f32));
	memcpy(o->bone + o->numverts * 3, bone, 3);

	return o->numverts++;
}

static s32 beanAddTri(struct beanout *o, s32 group, s32 tex, u16 a, u16 b, u16 c)
{
	struct beantri *t;

	if (o->numtris >= o->captris) {
		const s32 cap = o->captris ? o->captris * 2 : 8192;
		struct beantri *grown = realloc(o->tris, cap * sizeof(*grown));

		if (!grown) {
			return 0;
		}

		o->tris = grown;
		o->captris = cap;
	}

	t = &o->tris[o->numtris];
	t->group = (u16)group;
	t->tex = (u16)tex;
	t->order = (u32)o->numtris;
	t->v[0] = a;
	t->v[1] = b;
	t->v[2] = c;
	o->numtris++;

	return 1;
}

static void beanOutFree(struct beanout *o)
{
	free(o->pos);
	free(o->nrm);
	free(o->uv);
	free(o->weight);
	free(o->bone);
	free(o->tris);
}

static int beanTriCompare(const void *a, const void *b)
{
	const struct beantri *x = a;
	const struct beantri *y = b;

	if (x->group != y->group) {
		return x->group < y->group ? -1 : 1;
	}

	if (x->tex != y->tex) {
		return x->tex < y->tex ? -1 : 1;
	}

	return x->order < y->order ? -1 : x->order > y->order;
}

/**
 * The triangles laid out in 4J's mesh layout (xblamesh.py, CLAUDE-notes/
 * xbla.md "The mesh format"): a group per list node, a draw per material
 * within it, a skinned vertex of stride 48, and a palette of the model's
 * matrices whose entries translate each joint's rest back to its origin. The
 * header's scale is 100, so everything is in the model's own units.
 */
static u8 *beanWriteMesh(struct beanout *o, s32 numgroups, s32 nummatrices, const struct beanrig *rig,
		const u32 *matwords, s32 nummatwords, u64 *outAbsent, u32 *outLen)
{
	u32 numdraws = 0;
	u32 groupoffset, drawoffset, vertexoffset, indexoffset, len;
	u8 *file;
	u32 drawat = 0;
	s32 t = 0;

	if (o->numtris == 0 || o->numverts == 0) {
		return NULL;
	}

	qsort(o->tris, o->numtris, sizeof(*o->tris), beanTriCompare);

	for (s32 i = 0; i < o->numtris; i++) {
		if (i == 0 || o->tris[i].group != o->tris[i - 1].group || o->tris[i].tex != o->tris[i - 1].tex) {
			numdraws++;
		}
	}

	if (numdraws > GEBEAN_MAXDRAWS) {
		return NULL;
	}

	groupoffset = 32 + 48 * (u32)nummatrices;
	drawoffset = groupoffset + 12 * (u32)numgroups;
	vertexoffset = drawoffset + 12 * numdraws;
	indexoffset = vertexoffset + 48 * (u32)o->numverts;
	len = indexoffset + 6 * (u32)o->numtris;

	file = calloc(len, 1);

	if (!file) {
		return NULL;
	}

	gebeanPutBE32(file, (u32)o->numverts);
	gebeanPutBE32(file + 4, vertexoffset);
	gebeanPutBE32(file + 8, indexoffset);
	gebeanPutBE32(file + 12, numdraws);
	gebeanPutBE32(file + 16, drawoffset);
	gebeanPutBE32(file + 20, (u32)nummatrices);
	gebeanPutBEF32(file + 24, 100.0f);
	gebeanPutBE32(file + 28, groupoffset);

	// Three rows of four: identity turns, each joint's rest taken back off.
	for (s32 i = 0; i < nummatrices; i++) {
		u8 *mtx = file + 32 + 48 * i;

		for (s32 r = 0; r < 3; r++) {
			gebeanPutBEF32(mtx + (r * 4 + r) * 4, 1.0f);
			gebeanPutBEF32(mtx + (r * 4 + 3) * 4, rig && i < GEBEAN_MAXMTX && rig->hasrest[i] ? -rig->rest[i][r] : 0.0f);
		}
	}

	*outAbsent = 0;

	for (s32 g = 0; g < numgroups; g++) {
		const u8 *group = file + groupoffset + 12 * g;
		const u32 firstdraw = drawat;

		while (t < o->numtris && o->tris[t].group == g) {
			const s32 from = t;
			const u16 tex = o->tris[t].tex;
			u8 *draw = file + drawoffset + 12 * drawat;

			while (t < o->numtris && o->tris[t].group == g && o->tris[t].tex == tex) {
				t++;
			}

			gebeanPutBE32(draw, (u32)from);
			gebeanPutBE32(draw + 4, (u32)(t - from));
			gebeanPutBE32(draw + 8, matwords[tex < nummatwords ? tex : nummatwords - 1]);
			drawat++;
		}

		gebeanPutBE32((u8 *)group, firstdraw);
		gebeanPutBE32((u8 *)group + 4, drawat - firstdraw);
		gebeanPutBE32((u8 *)group + 8, 0);

		if (drawat == firstdraw && g < 64) {
			*outAbsent |= 1ull << g;
		}
	}

	for (s32 i = 0; i < o->numverts; i++) {
		u8 *v = file + vertexoffset + 48 * i;
		const u8 *bone = o->bone + i * 3;
		const f32 *w = o->weight + i * 3;

		for (s32 k = 0; k < 3; k++) {
			gebeanPutBEF32(v + k * 4, o->pos[i * 3 + k]);
			gebeanPutBEF32(v + 20 + k * 4, o->nrm[i * 3 + k]);
		}

		gebeanPutBEF32(v + 12, o->uv[i * 2]);
		gebeanPutBEF32(v + 16, o->uv[i * 2 + 1]);
		gebeanPutBE32(v + 32, 0xffffffff);
		gebeanPutBEF32(v + 36, w[0]);
		gebeanPutBEF32(v + 40, w[1]);
		gebeanPutBE32(v + 44, ((u32)bone[0] << 24) | ((u32)bone[1] << 16) | ((u32)bone[2] << 8) | 3);
	}

	for (s32 i = 0; i < o->numtris; i++) {
		u8 *idx = file + indexoffset + 6 * i;

		for (s32 k = 0; k < 3; k++) {
			idx[k * 2] = (u8)(o->tris[i].v[k] >> 8);
			idx[k * 2 + 1] = (u8)o->tris[i].v[k];
		}
	}

	*outLen = len;

	return file;
}

/** A node under a toggle: a head's glasses, hat or second hair, which Bean's head already has. */
static s32 beanNodeIsToggled(const struct modelnode *node)
{
	s32 walked = 0;

	for (node = node->parent; node && walked < 64; node = node->parent, walked++) {
		if ((node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			return 1;
		}
	}

	return 0;
}

/** The Bean bone a list node of the body hangs off, or -1. */
static s32 beanNodeSkel(const struct modelnode *node, struct modelnode **joints, const s8 *jointskel, s32 numjoints)
{
	s32 walked = 0;

	for (node = node->parent; node && walked < 64; node = node->parent, walked++) {
		const u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_POSITION || type == MODELNODETYPE_CHRINFO) {
			for (s32 j = 0; j < numjoints; j++) {
				if (joints[j] == node) {
					return jointskel[j];
				}
			}

			return -1;
		}
	}

	return -1;
}

u8 *gebeanBuild(s32 row, struct modeldef *modeldef, struct modelnode **nodes, s32 numnodes,
		struct gebeanmats *mats, u64 *outAbsent, u32 *outLen)
{
	const struct gebeanrow *r;
	struct beanmodel bm;
	struct beanrig rig;
	struct beanout out;
	struct modelnode *joints[64];
	s8 jointskel[64];
	s8 nodeskel[64];
	s32 numjoints = 0;
	f32 bind[SK_COUNT][3];
	s32 havebind[SK_COUNT];
	f32 headrot[3][3];
	f32 headscale = GEBEAN_HEAD_SCALE;
	u32 matwords[GEBEAN_MAXMATS];
	s32 nummatwords;
	s32 ishead;
	s32 fromchar;
	s32 dropped = 0;
	u8 *file = NULL;
	s32 nummatrices;

	*outLen = 0;
	*outAbsent = 0;

	if (row < 0 || row >= ARRAYCOUNT(rows) || !modeldef || numnodes <= 0 || numnodes > 64) {
		return NULL;
	}

	r = &rows[row];
	ishead = r->kind == GEBEAN_HEAD;
	fromchar = strncmp(r->source, "char/", 5) == 0;

	if (!gebeanLocate(1) || !beanLoad(&bm, r->source)) {
		return NULL;
	}

	if (!bm.numbones || !bm.numdraws) {
		sysLogPrintf(LOG_WARNING, "gebean: %s has no skeleton or no draws", r->source);
		beanFree(&bm);
		return NULL;
	}

	memset(havebind, 0, sizeof(havebind));

	for (s32 b = 0; b < bm.numbones; b++) {
		if (bm.skel[b] >= 0) {
			memcpy(bind[(s32)bm.skel[b]], bm.bind[b], sizeof(bind[0]));
			havebind[(s32)bm.skel[b]] = 1;
		}
	}

	memset(&out, 0, sizeof(out));
	memset(&rig, 0, sizeof(rig));

	if (ishead) {
		// A head is rigid on the neck, in the head file's own space, whose
		// origin is the body's neck joint. One taken from a whole character
		// is turned so Bean's spine points straight up, which GoldenEye's
		// does to within a third of a degree; a head file of Bean's stands
		// as it is.
		static const f32 up[3] = { 0.0f, 1.0f, 0.0f };

		if (!havebind[SK_NECK] || (fromchar && !havebind[SK_BACK])) {
			sysLogPrintf(LOG_WARNING, "gebean: %s has no neck", r->source);
			beanFree(&bm);
			return NULL;
		}

		if (fromchar) {
			f32 spine[3];

			for (s32 k = 0; k < 3; k++) {
				spine[k] = bind[SK_NECK][k] - bind[SK_BACK][k];
			}

			rotationBetween(spine, up, headrot);
		} else {
			rotationBetween(up, up, headrot);
		}

		nummatrices = 1;
	} else {
		if (!beanRigFromModel(modeldef, &rig, joints, jointskel, &numjoints)
				|| !beanFitRig(&rig, (const f32 (*)[3])bind, havebind)) {
			beanFree(&bm);
			return NULL;
		}

		for (s32 k = 0; k < numnodes; k++) {
			nodeskel[k] = (s8)beanNodeSkel(nodes[k], joints, jointskel, numjoints);
		}

		nummatrices = modeldef->nummatrices;

		if (nummatrices <= 0 || nummatrices > GEBEAN_MAXMTX) {
			beanFree(&bm);
			return NULL;
		}
	}

	for (s32 di = 0; di < bm.numdraws; di++) {
		const struct beandraw *d = &bm.draws[di];
		struct beanvb vb;
		u16 *tris;
		s32 numtris;
		s32 *mapped;

		if (!beanReadVb(&bm, d->vb, &vb)) {
			continue;
		}

		numtris = beanTriangles(&bm, d, &tris);

		if (numtris <= 0) {
			free(tris);
			continue;
		}

		// A buffer is shared by draws with different palettes, so a vertex is
		// taken once per draw: its bones mean different things in each.
		mapped = malloc(vb.count * sizeof(s32));

		if (!mapped) {
			free(tris);
			continue;
		}

		for (u32 i = 0; i < vb.count; i++) {
			mapped[i] = -1;
		}

		for (s32 t = 0; t < numtris; t++) {
			struct beanvtx v3[3];
			s32 sk[3][4];
			f32 wt[3][4];
			f32 total[SK_COUNT];
			s32 dominant = -1;
			s32 ok = 1;
			u16 idx[3];

			memset(total, 0, sizeof(total));

			for (s32 i = 0; i < 3 && ok; i++) {
				ok = beanVertex(&bm, &vb, tris[t * 3 + i], &v3[i]);

				for (s32 s = 0; s < 4 && ok; s++) {
					const s32 slot = v3[i].slot[s];
					s32 bone;

					sk[i][s] = -1;
					wt[i][s] = 0.0f;

					if (slot < 0 || slot >= d->numpal || v3[i].weight[s] == 0) {
						continue;
					}

					bone = d->pal[slot];
					bone = bm.numremap && bone < bm.numremap ? bm.remap[bone] : bone;

					if (bone >= bm.numbones || bm.skel[bone] < 0) {
						continue;
					}

					sk[i][s] = bm.skel[bone];
					wt[i][s] = (f32)v3[i].weight[s];
					total[sk[i][s]] += wt[i][s];
				}
			}

			if (!ok) {
				continue;
			}

			for (s32 s = 0; s < SK_COUNT; s++) {
				if (total[s] > 0.0f && (dominant < 0 || total[s] > total[dominant])) {
					dominant = s;
				}
			}

			if (dominant < 0) {
				continue;
			}

			// The neck belongs to the head file: a body leaves it out, and a
			// head takes only it.
			if ((dominant == SK_NECK) != (ishead != 0)) {
				dropped++;
				continue;
			}

			for (s32 i = 0; i < 3 && ok; i++) {
				const u16 vi = tris[t * 3 + i];
				f32 pos[3] = { 0.0f, 0.0f, 0.0f };
				f32 nrm[3];
				f32 uv[2];
				u8 bone[3] = { 0, 0, 0 };
				f32 weight[3] = { 1.0f, 0.0f, 0.0f };

				if (mapped[vi] >= 0) {
					idx[i] = (u16)mapped[vi];
					continue;
				}

				uv[0] = v3[i].uv[0];
				uv[1] = v3[i].uv[1];

				if (ishead) {
					f32 rel[3];

					for (s32 k = 0; k < 3; k++) {
						rel[k] = v3[i].pos[k] - bind[SK_NECK][k];
					}

					rotApply(headrot, rel, pos);

					for (s32 k = 0; k < 3; k++) {
						pos[k] *= headscale;
					}

					rotApply(headrot, v3[i].nrm, nrm);
				} else {
					// Each bone's own reading of the vertex on the N64 rig,
					// blended; the bones become the model's matrices, merged
					// where two Bean bones share one, the three heaviest kept.
					s32 mtx[4];
					f32 mw[4];
					s32 nm = 0;
					f32 sum = 0.0f;
					s32 heaviest = -1;

					for (s32 s = 0; s < 4; s++) {
						const s32 b = sk[i][s];
						f32 rel[3], turned[3];
						s32 at = -1;

						if (b < 0) {
							continue;
						}

						for (s32 k = 0; k < 3; k++) {
							rel[k] = v3[i].pos[k] - bind[b][k];
						}

						rotApply((const f32 (*)[3])rig.rot[b], rel, turned);

						for (s32 k = 0; k < 3; k++) {
							pos[k] += wt[i][s] * (rig.joint[b][k] + rig.scale * turned[k]);
						}

						sum += wt[i][s];

						if (heaviest < 0 || wt[i][s] > wt[i][heaviest]) {
							heaviest = s;
						}

						for (s32 m = 0; m < nm; m++) {
							if (mtx[m] == rig.mtx[b]) {
								at = m;
							}
						}

						if (at >= 0) {
							mw[at] += wt[i][s];
						} else {
							mtx[nm] = rig.mtx[b];
							mw[nm] = wt[i][s];
							nm++;
						}
					}

					if (sum <= 0.0f || heaviest < 0) {
						ok = 0;
						break;
					}

					for (s32 k = 0; k < 3; k++) {
						pos[k] /= sum;
					}

					rotApply((const f32 (*)[3])rig.rot[sk[i][heaviest]], v3[i].nrm, nrm);

					for (s32 m = 0; m < nm; m++) {
						for (s32 n = m + 1; n < nm; n++) {
							if (mw[n] > mw[m]) {
								const f32 tw = mw[m];
								const s32 tm = mtx[m];
								mw[m] = mw[n]; mtx[m] = mtx[n];
								mw[n] = tw; mtx[n] = tm;
							}
						}
					}

					if (nm > 3) {
						nm = 3;
					}

					sum = 0.0f;

					for (s32 m = 0; m < nm; m++) {
						sum += mw[m];
					}

					for (s32 m = 0; m < 3; m++) {
						bone[m] = (u8)(m < nm ? mtx[m] : mtx[0]);
						weight[m] = m < nm ? mw[m] / sum : 0.0f;
					}
				}

				mapped[vi] = beanAddVertex(&out, pos, nrm, uv, bone, weight);

				if (mapped[vi] < 0) {
					ok = 0;
					break;
				}

				idx[i] = (u16)mapped[vi];
			}

			if (!ok) {
				continue;
			}

			for (s32 k = 0; k < numnodes; k++) {
				s32 takes;

				if (ishead) {
					takes = !beanNodeIsToggled(nodes[k]);
				} else {
					s32 want = dominant == SK_POSITION ? SK_BASE : dominant;
					s32 any = 0;

					for (s32 j = 0; j < numnodes; j++) {
						if (nodeskel[j] == want) {
							any = 1;
							break;
						}
					}

					if (!any) {
						want = SK_BASE;
					}

					takes = nodeskel[k] == want;
				}

				if (takes && !beanAddTri(&out, k, (s32)d->tex, idx[0], idx[1], idx[2])) {
					ok = 0;
					break;
				}
			}
		}

		free(mapped);
		free(tris);
	}

	// Nodes that must draw nothing rather than keep their N64 geometry: a
	// head's toggled pieces, which Bean's head has already, and the neck of a
	// body whose head file takes Bean's neck. A generic body's neck is left
	// absent - the N64 stub stays under whatever head GoldenEye X grafts on.
	if (out.numverts > 0) {
		for (s32 k = 0; k < numnodes; k++) {
			const s32 blank = ishead ? beanNodeIsToggled(nodes[k])
					: r->kind == GEBEAN_BODY_WITH_HEAD && nodeskel[k] == SK_NECK;

			if (blank) {
				beanAddTri(&out, k, 0, 0, 0, 0);
			}
		}
	}

	// The pictures: only those a draw names, bound once per character.
	nummatwords = bm.numtex + 1 < GEBEAN_MAXMATS ? bm.numtex + 1 : GEBEAN_MAXMATS;
	memset(mats, 0, sizeof(*mats));
	mats->num = nummatwords;

	for (s32 i = 0; i < nummatwords; i++) {
		matwords[i] = XBLAMESH_MAT_TABLE | (u32)i;
	}

	for (s32 i = 0; i < bm.numtex && i < nummatwords; i++) {
		s32 used = 0;

		for (s32 t = 0; t < out.numtris; t++) {
			if (out.tris[t].tex == i) {
				used = 1;
				break;
			}
		}

		if (used && beanBindTexture(&bm, r->source, i, &mats->tile[i], &mats->alpha[i], &mats->soft[i])
				&& mats->alpha[i]) {
			matwords[i] |= 0x8000;
		}
	}

	// A draw naming a texture the file does not have takes the untextured entry at the end.
	for (s32 t = 0; t < out.numtris; t++) {
		if (out.tris[t].tex >= bm.numtex) {
			out.tris[t].tex = (u16)(nummatwords - 1);
		}
	}

	file = beanWriteMesh(&out, numnodes, nummatrices, ishead ? NULL : &rig, matwords, nummatwords, outAbsent, outLen);

	sysLogPrintf(LOG_NOTE, "gebean: %s <- %s: %d vertices, %d triangles over %d lists, %s %.4f%s",
			r->file, r->source, out.numverts, out.numtris, numnodes,
			ishead ? "rigid on the neck, scale" : "skinned to the model's matrices, scale",
			ishead ? headscale : rig.scale, file ? "" : " - did not write");

	(void)dropped;

	beanOutFree(&out);
	beanFree(&bm);

	return file;
}

#endif

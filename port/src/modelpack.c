/**
 * Model packs: the folder, the list, and what a pack has for a model.
 * See modelpack.h.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "files.h"
#include "config.h"
#include "system.h"
#include "fs.h"
#include "romdata.h"
#include "texpack.h"
#include "pngread.h"
#include "xblatex.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "modelpack.h"

#ifndef PLATFORM_N64

#define MODELPACK_DIR "model-packs"
#define MODELPACK_N64_SUB "n64"
#define MODELPACK_XBLA_SUB "xbla"
#define MODELPACK_MAXPACKS 64
#define MODELPACK_NAMELEN 64

// Enough for the largest texture in the ROM plus the tex that describes it,
// as texpack's dump uses.
#define MODELPACK_TEXPOOL (128 * 1024)

static s32 loadModels = 0;
static char packName[MODELPACK_NAMELEN] = "";

static char packNames[MODELPACK_MAXPACKS][MODELPACK_NAMELEN];
static s32 numPacks;
static s32 selectedPack = -1;
static s32 listed;

// The selected pack's files by file id, expanded paths, or NULL.
static char **n64Paths;
static char **xblaPaths;
static s32 indexedFor = -2; // which pack the two tables describe; -2 = none yet
static u32 generation = 1;

PD_CONSTRUCTOR static void modelpackConfigInit(void)
{
	configRegisterInt("Mod.LoadModels", &loadModels, 0, 1);
	configRegisterString("Mod.ModelPack", packName, sizeof(packName));
}

s32 modelpackLoadEnabled(void)
{
	return loadModels;
}

void modelpackSetLoadEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled != loadModels) {
		loadModels = enabled;
		generation++;
		sysLogPrintf(LOG_NOTE, "modelpack: packs %s", loadModels ? "on" : "off");
	}
}

static s32 modelpackIsDir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * model-packs/ beside the executable where that can be written and in the
 * save directory where it cannot - fsChooseOutputDir(), the same choice the
 * texture packs make - created so there is somewhere obvious to put one.
 */
const char *modelpackGetPacksDirPath(void)
{
	static char dir[FS_MAXPATH + 1];
	char rel[FS_MAXPATH + 1];

	if (dir[0]) {
		return dir;
	}

	if (fsChooseOutputDir(MODELPACK_DIR, rel, sizeof(rel)) != 0) {
		return NULL;
	}

	snprintf(dir, sizeof(dir), "%s", fsFullPath(rel));

	return dir;
}

static void modelpackListAdd(const char *name, void *arg)
{
	char path[FS_MAXPATH + 1];

	if (numPacks >= MODELPACK_MAXPACKS || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", (const char *)arg, name);

	if (!modelpackIsDir(path)) {
		return;
	}

	strncpy(packNames[numPacks], name, MODELPACK_NAMELEN - 1);
	packNames[numPacks][MODELPACK_NAMELEN - 1] = '\0';
	numPacks++;
}

void modelpackRefreshPacks(void)
{
	const char *dir = modelpackGetPacksDirPath();

	numPacks = 0;
	listed = 1;

	if (!dir) {
		return;
	}

	fsScanDir(dir, modelpackListAdd, (void *)dir);

	// A pack's place in the list can move when one is added or taken away,
	// so the selection is kept by name.
	selectedPack = -1;

	for (s32 i = 0; i < numPacks; i++) {
		if (packName[0] && !strcmp(packNames[i], packName)) {
			selectedPack = i;
		}
	}
}

s32 modelpackGetNumPacks(void)
{
	if (!listed) {
		modelpackRefreshPacks();
	}

	return numPacks;
}

const char *modelpackGetPackName(s32 index)
{
	if (index < 0 || index >= modelpackGetNumPacks()) {
		return "";
	}

	return packNames[index];
}

s32 modelpackGetSelectedPack(void)
{
	modelpackGetNumPacks();

	return selectedPack;
}

void modelpackSetSelectedPack(s32 index)
{
	modelpackGetNumPacks();

	if (index < 0 || index >= numPacks) {
		index = -1;
	}

	if (index == selectedPack) {
		return;
	}

	selectedPack = index;
	packName[0] = '\0';

	if (index >= 0) {
		strncpy(packName, packNames[index], sizeof(packName) - 1);
		packName[sizeof(packName) - 1] = '\0';
	}

	generation++;
	sysLogPrintf(LOG_NOTE, "modelpack: selected %s", index >= 0 ? packName : "none");
}

/* -------------------------------------------------------------------------
 * The index: what the pack has for each file id
 * ------------------------------------------------------------------------- */

struct modelpackscan {
	char **table;
	const char *dir;
	s32 found;
};

static void modelpackIndexAdd(const char *name, void *arg)
{
	struct modelpackscan *scan = arg;
	char stem[FS_MAXPATH + 1];
	size_t len = strlen(name);
	s32 fileid;

	if (len <= 4 || strcmp(name + len - 4, ".obj")) {
		return;
	}

	snprintf(stem, sizeof(stem), "%.*s", (int)(len - 4), name);

	// The ROM's own name for the model, or a bare file id.
	fileid = romdataFileGetNumForName(stem);

	if (fileid < 0) {
		char *end;
		long n = strtol(stem, &end, 0);

		if (end != stem && *end == '\0' && n > 0 && n < NUM_FILE_SLOTS) {
			fileid = (s32)n;
		}
	}

	if (fileid <= 0 || fileid >= NUM_FILE_SLOTS) {
		sysLogPrintf(LOG_WARNING, "modelpack: %s/%s names no model of the game's", scan->dir, name);
		return;
	}

	free(scan->table[fileid]);
	scan->table[fileid] = malloc(strlen(scan->dir) + 1 + len + 1);

	if (scan->table[fileid]) {
		sprintf(scan->table[fileid], "%s/%s", scan->dir, name);
		scan->found++;
	}
}

static void modelpackClearTable(char **table)
{
	if (!table) {
		return;
	}

	for (s32 i = 0; i < NUM_FILE_SLOTS; i++) {
		free(table[i]);
		table[i] = NULL;
	}
}

static void modelpackIndex(void)
{
	const char *packs;
	char dir[FS_MAXPATH + 1];
	struct modelpackscan scan;
	s32 n64 = 0;
	s32 xbla = 0;

	modelpackGetNumPacks();

	if (indexedFor == selectedPack) {
		return;
	}

	indexedFor = selectedPack;

	if (!n64Paths) {
		n64Paths = calloc(NUM_FILE_SLOTS, sizeof(char *));
		xblaPaths = calloc(NUM_FILE_SLOTS, sizeof(char *));
	}

	modelpackClearTable(n64Paths);
	modelpackClearTable(xblaPaths);

	if (selectedPack < 0 || !n64Paths || !xblaPaths) {
		return;
	}

	packs = modelpackGetPacksDirPath();

	if (!packs) {
		return;
	}

	snprintf(dir, sizeof(dir), "%s/%s/" MODELPACK_N64_SUB, packs, packNames[selectedPack]);
	scan.table = n64Paths;
	scan.dir = dir;
	scan.found = 0;
	fsScanDir(dir, modelpackIndexAdd, &scan);
	n64 = scan.found;

	snprintf(dir, sizeof(dir), "%s/%s/" MODELPACK_XBLA_SUB, packs, packNames[selectedPack]);
	scan.table = xblaPaths;
	scan.dir = dir;
	scan.found = 0;
	fsScanDir(dir, modelpackIndexAdd, &scan);
	xbla = scan.found;

	sysLogPrintf(LOG_NOTE, "modelpack: %s has %d N64 model%s and %d XBLA mesh%s",
			packNames[selectedPack], n64, n64 == 1 ? "" : "s", xbla, xbla == 1 ? "" : "es");
}

const char *modelpackFindN64(s32 fileid)
{
	if (!loadModels || fileid <= 0 || fileid >= NUM_FILE_SLOTS) {
		return NULL;
	}

	modelpackIndex();

	return n64Paths ? n64Paths[fileid] : NULL;
}

const char *modelpackFindXbla(s32 fileid)
{
	if (!loadModels || fileid <= 0 || fileid >= NUM_FILE_SLOTS) {
		return NULL;
	}

	modelpackIndex();

	return xblaPaths ? xblaPaths[fileid] : NULL;
}

u32 modelpackGetGeneration(void)
{
	return generation;
}

/* -------------------------------------------------------------------------
 * Pictures
 * ------------------------------------------------------------------------- */

/** Top row first to first-uploaded row first, in place. */
static void modelpackFlipRows(u8 *rgba, s32 width, s32 height)
{
	const size_t row = (size_t)width * 4;
	u8 *tmp = malloc(row);

	if (!tmp) {
		return;
	}

	for (s32 y = 0; y < height / 2; y++) {
		u8 *a = rgba + row * y;
		u8 *b = rgba + row * (height - 1 - y);

		memcpy(tmp, a, row);
		memcpy(a, b, row);
		memcpy(b, tmp, row);
	}

	free(tmp);
}

/**
 * One of the ROM's textures as RGBA in the game's row order: the texture
 * pack's picture for it if there is one, else the ROM's own, decoded through
 * a pool of this file's own so nothing of the stage's is touched.
 */
static u8 *modelpackDecodeN64Texture(s32 texturenum, s32 *outWidth, s32 *outHeight)
{
	struct texpool pool;
	struct tex *tex;
	u8 *buffer;
	u8 *rgba;

	if (texturenum < 0 || texturenum >= NUM_TEXTURES) {
		return NULL;
	}

	rgba = texpackDecodeReplacementNow(texturenum, outWidth, outHeight);

	if (rgba) {
		return rgba;
	}

	buffer = malloc(MODELPACK_TEXPOOL);

	if (!buffer) {
		return NULL;
	}

	texInitPool(&pool, buffer, MODELPACK_TEXPOOL);
	texLoadFromTextureNum((u32)texturenum, &pool);

	tex = texFindInPool(texturenum, &pool);
	rgba = (tex && tex->data) ? texpackTexToRgba(tex, outWidth, outHeight) : NULL;

	// The texels came out bottom row first, for a PNG; the renderer wants
	// them the way the game uploads them.
	if (rgba) {
		modelpackFlipRows(rgba, *outWidth, *outHeight);
	}

	texpackForgetRange(buffer, buffer + MODELPACK_TEXPOOL);
	free(buffer);

	return rgba;
}

const void *modelpackBindMaterial(const struct objmaterial *mat, s32 *outAlpha, s32 *outSoft)
{
	const void *tile = NULL;
	char key[FS_MAXPATH + 16];
	s32 width = 0;
	s32 height = 0;
	u8 *rgba = NULL;

	*outAlpha = 0;
	*outSoft = 0;

	if (mat->kind == OBJMAT_N64) {
		snprintf(key, sizeof(key), "n64_%04x", mat->id);
		rgba = modelpackDecodeN64Texture((s32)mat->id, &width, &height);

		if (!rgba) {
			sysLogPrintf(LOG_WARNING, "modelpack: texture %04x would not decode", mat->id);
		}
	} else if (mat->kind == OBJMAT_IMAGE) {
		snprintf(key, sizeof(key), "file:%s", mat->image);
		rgba = pngRead(mat->image, &width, &height);

		if (rgba) {
			modelpackFlipRows(rgba, width, height);
		}
	} else {
		return NULL;
	}

	if (!rgba) {
		return NULL;
	}

	tile = xblaTexBindImage(key, rgba, width, height);

	if (tile) {
		xblaTexImageInfo(tile, outAlpha, outSoft);
	}

	return tile;
}

#endif

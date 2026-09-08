/**
 * Building a texture pack out of the XBLA release. See xblaimport.h.
 *
 * The whole job is files, so it runs on a worker thread and the tick below
 * only watches for the end of it. Two things shape the code more than they
 * look like they should:
 *
 *   * Textures.raw is 166MB. It is never held in memory - the record tables
 *     are read once and each texture is streamed out of the package as it is
 *     converted, which keeps the peak at one texture rather than the file.
 *   * A record's index is the texture number, so there is no matching step.
 *     Anything at or past NUM_TEXTURES is the console release's own dashboard
 *     art, which has no texture number and is left out. The pack loader would
 *     refuse those names anyway (texpackParseNativeName()), so writing them
 *     would only waste the disk.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "platform.h"
#include "archive.h"
#include "config.h"
#include "fs.h"
#include "pngwrite.h"
#include "system.h"
#include "texpack.h"
#include "x360.h"
#include "xblaimport.h"

#define XBLAIMPORT_NAMELEN 64

// The pack's name in Extended Options. Fixed, so converting twice replaces the
// pack rather than leaving two of them.
#define XBLAIMPORT_PACK_NAME "PD XBLA"

// Where an archive is unpacked. Starts with a dot so the pack scan skips it.
#define XBLAIMPORT_WORK_DIR ".xbla"

// Inside the package. The only file this reads.
#define XBLAIMPORT_TEXTURES "DataFiles/Textures.raw"

// One texture's record. Two tables of this stride sit before the data: the
// metadata, then an array of D3DTexture structs whose last six dwords are the
// GPU fetch constant.
#define XBLAIMPORT_RECORD 52
#define XBLAIMPORT_FETCH_DWORD 7

// A record whose dimensions are past this is not something the game asked for
// and not something worth a 64MB allocation on a malformed table.
#define XBLAIMPORT_MAXDIM 4096

// What a package might be called where the player left it.
static const char *const xblaKnownNames[] = {
	"Perfect Dark XBLA.7z",
	"Perfect Dark XBLA.zip",
	"Perfect Dark XBLA",
};

static s32 state;
static s32 optUpscalesOnly;
static char statusText[128];
static char packName[XBLAIMPORT_NAMELEN] = XBLAIMPORT_PACK_NAME;
static char packagePath[FS_MAXPATH + 1];
static char configuredPath[FS_MAXPATH + 1];
static char packDir[FS_MAXPATH + 1];
static char workDir[FS_MAXPATH + 1];
static s32 detected;

static SDL_Thread *worker;
static SDL_atomic_t workerDone;
static SDL_atomic_t workerFailed;
static SDL_atomic_t workerStage;
static SDL_atomic_t workerCount; // textures written so far
static SDL_atomic_t workerTotal;
static SDL_atomic_t cancelled;

static void xblaSetStatus(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(statusText, sizeof(statusText), fmt, args);
	va_end(args);
}

static u32 xblaBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/**
 * Whether this file is a package or an archive holding one.
 *
 * An STFS package is named after a content id hash, so it cannot be found by
 * name - only by looking at what is in it.
 */
static s32 xblaLooksLikePackage(const char *path)
{
	u8 magic[4];
	FILE *fp = fopen(path, "rb");
	s32 ok;

	if (!fp) {
		return 0;
	}

	ok = fread(magic, 1, sizeof(magic), fp) == sizeof(magic) &&
			(!memcmp(magic, "LIVE", 4) || !memcmp(magic, "CON ", 4) ||
			 !memcmp(magic, "PIRS", 4));
	fclose(fp);

	return ok;
}

struct xblascan {
	char found[FS_MAXPATH + 1];
	const char *dir;
};

static void xblaScanForPackage(const char *name, void *arg)
{
	struct xblascan *scan = arg;
	char path[FS_MAXPATH + 1];

	if (scan->found[0] || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", scan->dir, name);

	if (xblaLooksLikePackage(path)) {
		strncpy(scan->found, path, sizeof(scan->found) - 1);
	}
}

/** The first STFS package directly inside dir, if there is one. */
static s32 xblaFindPackageIn(const char *dir, char *dst, u32 dstLen)
{
	struct xblascan scan;

	memset(&scan, 0, sizeof(scan));
	scan.dir = dir;

	fsScanDir(dir, xblaScanForPackage, &scan);

	if (!scan.found[0]) {
		return 0;
	}

	strncpy(dst, scan.found, dstLen - 1);
	dst[dstLen - 1] = '\0';

	return 1;
}

static s32 xblaTryPath(const char *path)
{
	if (!path || !path[0]) {
		return 0;
	}

	if (fsFileSize(path) < 0) {
		return 0;
	}

	// An archive is taken on its extension; a package on what it starts with.
	if (!archiveIsSupported(path) && !xblaLooksLikePackage(path)) {
		return 0;
	}

	strncpy(packagePath, path, sizeof(packagePath) - 1);
	packagePath[sizeof(packagePath) - 1] = '\0';

	return 1;
}

static void xblaDetect(void)
{
	static const char *const roots[] = { "$H", "$E", "$S" };

	detected = 1;
	packagePath[0] = '\0';

	if (xblaTryPath(configuredPath)) {
		return;
	}

	for (s32 r = 0; r < ARRAYCOUNT(roots); r++) {
		const char *root = fsFullPath(roots[r]);
		char path[FS_MAXPATH + 1];

		if (!root || !root[0]) {
			continue;
		}

		for (s32 i = 0; i < ARRAYCOUNT(xblaKnownNames); i++) {
			snprintf(path, sizeof(path), "%s/%s", root, xblaKnownNames[i]);

			if (xblaTryPath(path)) {
				return;
			}
		}

		// Failing a name we know, anything in there that is one.
		if (xblaFindPackageIn(root, path, sizeof(path)) && xblaTryPath(path)) {
			return;
		}
	}
}

s32 xblaImportIsAvailable(void)
{
	if (!detected) {
		xblaDetect();
	}

	return packagePath[0] != '\0';
}

const char *xblaImportGetPackagePath(void)
{
	xblaImportIsAvailable();

	return packagePath;
}

const char *xblaImportGetStfsPath(void)
{
	static char stfsPath[FS_MAXPATH + 1];
	char rel[FS_MAXPATH + 1];
	char dir[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	if (!xblaImportIsAvailable()) {
		return NULL;
	}

	if (xblaLooksLikePackage(packagePath)) {
		return packagePath;
	}

	if (stfsPath[0]) {
		return stfsPath;
	}

	// What the player has is an archive. An earlier conversion will have left
	// the package inside it unpacked in the work directory, under whatever the
	// archive called it - a content id hash, or a subdirectory named after the
	// game.
	if (fsChooseOutputDir(TEXPACK_PACKS_DIR, rel, sizeof(rel)) != 0) {
		return NULL;
	}

	snprintf(dir, sizeof(dir), "%s/" XBLAIMPORT_WORK_DIR, fsFullPath(rel));

	if (xblaFindPackageIn(dir, stfsPath, sizeof(stfsPath))) {
		return stfsPath;
	}

	snprintf(sub, sizeof(sub), "%s/Perfect Dark", dir);

	if (xblaFindPackageIn(sub, stfsPath, sizeof(stfsPath))) {
		return stfsPath;
	}

	return NULL;
}

void xblaImportRedetect(void)
{
	detected = 0;
	xblaImportIsAvailable();
}

/**
 * Converts every replaced texture. Returns 0 having said why in the log.
 */
static s32 xblaConvert(const char *pkgPath)
{
	struct x360stfs stfs;
	struct x360stfsstream stream;
	u8 *tables = NULL;
	u8 *compressed = NULL;
	u8 *surface = NULL;
	u8 *rgba = NULL;
	u32 maxCompressed = 0;
	u32 maxSurface = 0;
	u32 maxPixels = 0;
	u32 count;
	u32 dataBase;
	u32 limit;
	s32 index;
	s32 result = 0;

	if (!x360StfsOpen(&stfs, pkgPath)) {
		sysLogPrintf(LOG_ERROR, "xbla: %s is not an Xbox 360 package", pkgPath);
		return 0;
	}

	index = x360StfsFind(&stfs, XBLAIMPORT_TEXTURES);

	if (index < 0) {
		sysLogPrintf(LOG_ERROR, "xbla: the package has no " XBLAIMPORT_TEXTURES
				" - is it Perfect Dark?");
		x360StfsClose(&stfs);
		return 0;
	}

	if (!x360StfsStreamOpen(&stfs, (u32)index, &stream)) {
		sysLogPrintf(LOG_ERROR, "xbla: could not read " XBLAIMPORT_TEXTURES);
		x360StfsClose(&stfs);
		return 0;
	}

	u8 head[4];

	if (!x360StfsStreamRead(&stream, 0, sizeof(head), head)) {
		sysLogPrintf(LOG_ERROR, "xbla: " XBLAIMPORT_TEXTURES " is empty");
		goto out;
	}

	count = xblaBE32(head);
	dataBase = 4 + count * 2 * XBLAIMPORT_RECORD;

	if (count == 0 || count > 0x10000 || dataBase > stream.size) {
		sysLogPrintf(LOG_ERROR, "xbla: %u texture records do not fit in %u bytes",
				count, stream.size);
		goto out;
	}

	tables = malloc(dataBase - 4);

	if (!tables || !x360StfsStreamRead(&stream, 4, dataBase - 4, tables)) {
		sysLogPrintf(LOG_ERROR, "xbla: could not read the texture tables");
		goto out;
	}

	limit = count < XBLAIMPORT_NUM_REPLACED ? count : XBLAIMPORT_NUM_REPLACED;

	if (limit > NUM_TEXTURES) {
		limit = NUM_TEXTURES;
	}

	SDL_AtomicSet(&workerTotal, (int)limit);
	sysLogPrintf(LOG_NOTE, "xbla: %u records, converting %u", count, limit);

	for (u32 n = 0; n < limit; n++) {
		const u8 *a = tables + n * XBLAIMPORT_RECORD;
		const u8 *b = tables + (count + n) * XBLAIMPORT_RECORD;
		const u32 offset = xblaBE32(a);
		const u32 width = xblaBE32(a + 4);
		const u32 height = xblaBE32(a + 8);
		const u32 srcWidth = xblaBE32(a + 12);
		const u32 srcHeight = xblaBE32(a + 16);
		const u32 usize = xblaBE32(a + 20);
		const u32 csize = xblaBE32(a + 24);
		struct x360fetch fetch;
		u32 dwords[6];
		char path[FS_MAXPATH + 1];

		if (SDL_AtomicGet(&cancelled)) {
			goto out;
		}

		if (!usize || !csize) {
			continue;
		}

		// The release redrew a good part of the environment art at its
		// original size rather than enlarging it. Those are a matter of
		// taste, so they can be left out.
		if (optUpscalesOnly && width == srcWidth && height == srcHeight) {
			continue;
		}

		for (u32 k = 0; k < 6; k++) {
			dwords[k] = xblaBE32(b + (XBLAIMPORT_FETCH_DWORD + k) * 4);
		}

		x360FetchRead(&fetch, dwords);

		if (fetch.width != width || fetch.height != height) {
			// The two tables disagreeing means one of them was read wrong,
			// and going on would write a picture of the wrong size.
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x is %ux%u in the table "
					"and %ux%u in the fetch constant; skipped",
					n, width, height, fetch.width, fetch.height);
			continue;
		}

		if (!width || !height || width > XBLAIMPORT_MAXDIM || height > XBLAIMPORT_MAXDIM) {
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x is %ux%u; skipped",
					n, width, height);
			continue;
		}

		if (!x360FetchSupported(&fetch)) {
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x is format %u, which is "
					"not one this reads; skipped", n, fetch.format);
			continue;
		}

		if (offset > stream.size || csize > stream.size - offset ||
				dataBase + offset < dataBase) {
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x runs past the file; skipped", n);
			continue;
		}

		// The three buffers are grown as needed and kept, rather than
		// allocated per texture: there are thousands of them.
		if (csize > maxCompressed) {
			free(compressed);
			compressed = malloc(csize);
			maxCompressed = csize;
		}

		if (usize > maxSurface) {
			free(surface);
			surface = malloc(usize);
			maxSurface = usize;
		}

		if (width * height > maxPixels) {
			free(rgba);
			rgba = malloc((size_t)width * height * 4);
			maxPixels = width * height;
		}

		if (!compressed || !surface || !rgba) {
			sysLogPrintf(LOG_ERROR, "xbla: out of memory at texture %04x", n);
			goto out;
		}

		if (!x360StfsStreamRead(&stream, dataBase + offset, csize, compressed)) {
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x could not be read; skipped", n);
			continue;
		}

		if (x360LzxDecompress(compressed, csize, surface, usize) != usize) {
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x did not decompress; skipped", n);
			continue;
		}

		if (!x360DecodeTexture(surface, usize, &fetch, rgba)) {
			sysLogPrintf(LOG_WARNING, "xbla: texture %04x is %ux%u but only %u bytes "
					"decompressed; skipped", n, width, height, usize);
			continue;
		}

		snprintf(path, sizeof(path), "%s/%04x.png", packDir, n);

		// The console art is in N64 row order, upside down on screen, and a
		// pack file carrying our own <texnum>.png name is expected the right
		// way up - the loader turns it back over. pngWrite() does the flip.
		if (pngWrite(path, rgba, (s32)width, (s32)height, 4, 1)) {
			SDL_AtomicAdd(&workerCount, 1);
		}
	}

	result = 1;

out:
	free(tables);
	free(compressed);
	free(surface);
	free(rgba);
	x360StfsStreamClose(&stream);
	x360StfsClose(&stfs);

	return result;
}

static int xblaImportWorker(void *arg)
{
	char pkgPath[FS_MAXPATH + 1];

	(void)arg;

	strncpy(pkgPath, packagePath, sizeof(pkgPath) - 1);
	pkgPath[sizeof(pkgPath) - 1] = '\0';

	// An archive has to come apart first. A .7z is usually solid, so there is
	// no reading one file out of it cheaply - see archiveExtract().
	if (!xblaLooksLikePackage(pkgPath)) {
		char found[FS_MAXPATH + 1];

		SDL_AtomicSet(&workerStage, XBLAIMPORT_EXTRACTING);

		if (archiveExtract(pkgPath, workDir) <= 0) {
			sysLogPrintf(LOG_ERROR, "xbla: could not unpack %s", pkgPath);
			SDL_AtomicSet(&workerFailed, 1);
			SDL_AtomicSet(&workerDone, 1);
			return 0;
		}

		// The package inside is named after a content id, so it is found by
		// its magic. Archives in the wild put it in a subdirectory.
		if (!xblaFindPackageIn(workDir, found, sizeof(found))) {
			char sub[FS_MAXPATH + 1];
			struct xblascan scan;

			memset(&scan, 0, sizeof(scan));
			snprintf(sub, sizeof(sub), "%s/Perfect Dark", workDir);
			scan.dir = sub;
			fsScanDir(sub, xblaScanForPackage, &scan);
			strncpy(found, scan.found, sizeof(found) - 1);
		}

		if (!found[0]) {
			sysLogPrintf(LOG_ERROR, "xbla: no Xbox 360 package inside %s", pkgPath);
			SDL_AtomicSet(&workerFailed, 1);
			SDL_AtomicSet(&workerDone, 1);
			return 0;
		}

		strncpy(pkgPath, found, sizeof(pkgPath) - 1);
		pkgPath[sizeof(pkgPath) - 1] = '\0';
	}

	SDL_AtomicSet(&workerStage, XBLAIMPORT_READING);

	if (SDL_AtomicGet(&cancelled)) {
		SDL_AtomicSet(&workerDone, 1);
		return 0;
	}

	SDL_AtomicSet(&workerStage, XBLAIMPORT_CONVERTING);

	if (!xblaConvert(pkgPath)) {
		SDL_AtomicSet(&workerFailed, 1);
	}

	SDL_AtomicSet(&workerDone, 1);

	return 0;
}

s32 xblaImportStart(void)
{
	char rel[FS_MAXPATH + 1];

	if (state == XBLAIMPORT_EXTRACTING || state == XBLAIMPORT_READING ||
			state == XBLAIMPORT_CONVERTING) {
		return 0;
	}

	if (!xblaImportIsAvailable()) {
		xblaSetStatus("No XBLA package found");
		state = XBLAIMPORT_FAILED;
		return 0;
	}

	if (fsChooseOutputDir(TEXPACK_PACKS_DIR, rel, sizeof(rel)) != 0) {
		xblaSetStatus("nowhere to write a pack");
		state = XBLAIMPORT_FAILED;
		return 0;
	}

	snprintf(workDir, sizeof(workDir), "%s/" XBLAIMPORT_WORK_DIR, fsFullPath(rel));
	fsCreateDir(workDir);

	snprintf(packDir, sizeof(packDir), "%s/%s", fsFullPath(rel), packName);
	fsCreateDir(packDir);
	snprintf(packDir, sizeof(packDir), "%s/%s/" "textures", fsFullPath(rel), packName);
	fsCreateDir(packDir);

	SDL_AtomicSet(&workerDone, 0);
	SDL_AtomicSet(&workerFailed, 0);
	SDL_AtomicSet(&workerCount, 0);
	SDL_AtomicSet(&workerTotal, XBLAIMPORT_NUM_REPLACED);
	SDL_AtomicSet(&workerStage, XBLAIMPORT_READING);
	SDL_AtomicSet(&cancelled, 0);

	worker = SDL_CreateThread(xblaImportWorker, "pd-xbla", NULL);

	if (!worker) {
		xblaSetStatus("could not start");
		state = XBLAIMPORT_FAILED;
		return 0;
	}

	state = XBLAIMPORT_READING;
	xblaSetStatus("Reading the package");

	return 1;
}

void xblaImportCancel(void)
{
	if (state != XBLAIMPORT_EXTRACTING && state != XBLAIMPORT_READING &&
			state != XBLAIMPORT_CONVERTING) {
		return;
	}

	SDL_AtomicSet(&cancelled, 1);
	xblaSetStatus("Stopping");
}

void xblaImportTick(void)
{
	if (state != XBLAIMPORT_EXTRACTING && state != XBLAIMPORT_READING &&
			state != XBLAIMPORT_CONVERTING) {
		return;
	}

	state = SDL_AtomicGet(&workerStage);

	switch (state) {
	case XBLAIMPORT_EXTRACTING:
		xblaSetStatus("Unpacking the archive");
		break;
	case XBLAIMPORT_READING:
		xblaSetStatus("Reading the package");
		break;
	default:
		xblaSetStatus("Converting textures - %d of %d",
				SDL_AtomicGet(&workerCount), SDL_AtomicGet(&workerTotal));
		break;
	}

	if (!SDL_AtomicGet(&workerDone)) {
		return;
	}

	SDL_WaitThread(worker, NULL);
	worker = NULL;

	if (SDL_AtomicGet(&cancelled)) {
		state = XBLAIMPORT_IDLE;
		xblaSetStatus("Cancelled");
		return;
	}

	if (SDL_AtomicGet(&workerFailed)) {
		state = XBLAIMPORT_FAILED;
		xblaSetStatus("The conversion failed - see the log");
		return;
	}

	state = XBLAIMPORT_DONE;

	// Select it, rather than saying where to find what was just waited for.
	texpackRefreshPacks();

	for (s32 i = 0; i < texpackGetNumPacks(); i++) {
		if (!strcmp(texpackGetPackName(i), packName)) {
			texpackSetSelectedPack(i);
			break;
		}
	}

	if (!texpackLoadEnabled()) {
		texpackSetLoadEnabled(1);
	}

	xblaSetStatus("Done - %d textures, %s is now selected",
			SDL_AtomicGet(&workerCount), packName);
}

s32 xblaImportGetState(void) { return state; }
const char *xblaImportGetStatus(void) { return statusText; }
const char *xblaImportGetPackName(void) { return packName; }
s32 xblaImportGetUpscalesOnly(void) { return optUpscalesOnly; }
void xblaImportSetUpscalesOnly(s32 enabled) { optUpscalesOnly = enabled ? 1 : 0; }

s32 xblaImportGetPercent(void)
{
	const s32 total = SDL_AtomicGet(&workerTotal);

	if (state == XBLAIMPORT_DONE) {
		return 100;
	}

	if (state != XBLAIMPORT_CONVERTING || total <= 0) {
		return 0;
	}

	return SDL_AtomicGet(&workerCount) * 100 / total;
}

/**
 * --xbla-import: convert a pack and stop, without touching the menus.
 *
 * The same use as --upscayl-build: setting a machine up without sitting in
 * front of it, and the only way to exercise the conversion in a headless run.
 */
void xblaImportFromCommandLine(void)
{
	s32 last = -1;

	if (!sysArgCheck("--xbla-import")) {
		return;
	}

	if (!xblaImportIsAvailable()) {
		sysLogPrintf(LOG_ERROR, "xbla: no package found - set Mod.XblaPackage");
		exit(1);
	}

	if (!xblaImportStart()) {
		sysLogPrintf(LOG_ERROR, "xbla: %s", statusText);
		exit(1);
	}

	while (state != XBLAIMPORT_DONE && state != XBLAIMPORT_FAILED) {
		const s32 pct = xblaImportGetPercent();

		xblaImportTick();

		if (pct / 10 != last) {
			last = pct / 10;
			sysLogPrintf(LOG_NOTE, "xbla: %s %d%%", statusText, pct);
		}

		SDL_Delay(50);
	}

	sysLogPrintf(state == XBLAIMPORT_DONE ? LOG_NOTE : LOG_ERROR, "xbla: %s", statusText);

	exit(state == XBLAIMPORT_DONE ? 0 : 1);
}

PD_CONSTRUCTOR static void xblaImportConfigInit(void)
{
	configRegisterString("Mod.XblaPackage", configuredPath, sizeof(configuredPath));
}

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
#include <sys/stat.h>
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
#include "xblaslots.h"

#define XBLAIMPORT_NAMELEN 64

// The pack's name in Extended Options. Fixed, so converting twice replaces the
// pack rather than leaving two of them.
#define XBLAIMPORT_PACK_NAME "PD XBLA"

// Where the player puts their copy: a folder of its own beside the executable,
// the way mods/ has one. Anything in there that is a package, or an archive
// holding one, is found - so the file keeps whatever name it came with.
#define XBLAIMPORT_XBLA_DIR "xbla"

// How far into that folder to look. An archive in the wild wraps the package
// in a folder of its own ("Perfect Dark/<content id>"), and a player who
// unpacked one by hand has that folder sitting in xbla/.
#define XBLAIMPORT_SCAN_DEPTH 2

// Where an archive dropped in xbla/ comes apart: cache/xbla/ beside the
// executable (or in the save directory), so that xbla/ itself holds nothing
// but what the player put there. The file is written once the archive has
// come apart completely - an extraction that was interrupted is done again
// rather than half used.
#define XBLAIMPORT_CACHE_DIR "cache"
#define XBLAIMPORT_CACHE_SUB "xbla"
#define XBLAIMPORT_DONE_FILE ".extracted"

// Where an archive came apart before the cache existed: a dot directory
// inside xbla/. Read and never written, like the older place below it.
#define XBLAIMPORT_UNPACK_DIR ".unpacked"

// Where an archive was unpacked before xbla/ existed, under the texture packs.
// Read and never written: an install that already has the 250MB package there
// is not made to unpack it a second time.
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
static char statusText[128];
static char packName[XBLAIMPORT_NAMELEN] = XBLAIMPORT_PACK_NAME;

/**
 * Textures the pack leaves out, and clears from a pack already on disk.
 *
 * These are the ones whose release copy is not a version of the ROM's picture
 * but a different one, drawn for the console's own renderer, that the game's
 * renderer draws wrong.
 *
 * 0013 is the sky's cloud texture, g_TcSkyWaterConfigs[0], the one every stage
 * with clouds uses. skyRender() lerps between the sky colour and the cloud
 * colour by the texel, so the ROM's full-range fractal is what makes the
 * streaked sunset over Crash Site. The release's is a 64x64 noise that never
 * rises above 122 and is uncorrelated with the ROM's - a different picture,
 * not a blurred or resized one - and through the same combiner it flattens
 * the sky to a plain gradient (2026-09-10, "the sky is messed up for xbla").
 * The release's water (0014) and second cloud (0c90) are copies of the ROM's
 * and stay in.
 *
 * The rest are the slots the release reused for other pictures, xblaslots.h:
 * a ROM room binding one wants the ROM's picture, and a release room binding
 * one is given the release's by the level loader, pack or no pack.
 */
static const u16 leftOut[] = { 0x0013, XBLA_REUSED_SLOTS };

s32 xblaImportTextureIsLeftOut(s32 texturenum)
{
	for (u32 i = 0; i < sizeof(leftOut) / sizeof(leftOut[0]); i++) {
		if (leftOut[i] == texturenum) {
			return 1;
		}
	}

	return 0;
}

/**
 * Drops the left-out textures from the pack's textures directory - in the
 * "$E/..." form or expanded, fsFullPath() takes both - if a conversion from
 * before they were left out put them there. Nothing is created, so a
 * directory that is not there is a no-op.
 */
static void xblaImportDropLeftOut(const char *texturesDir)
{
	char path[FS_MAXPATH + 1];

	for (u32 i = 0; i < sizeof(leftOut) / sizeof(leftOut[0]); i++) {
		snprintf(path, sizeof(path), "%s/%04x.png", texturesDir, leftOut[i]);

		if (fsFileSize(path) >= 0 && fsRemoveFile(path) == 0) {
			sysLogPrintf(LOG_NOTE, "xbla: removed texture %04x from the %s pack, "
					"the game's own is the one that draws right", leftOut[i], packName);
		}
	}
}
static char packagePath[FS_MAXPATH + 1];
static char configuredPath[FS_MAXPATH + 1];
static char packDir[FS_MAXPATH + 1];
static s32 detected;

// The package itself, once there is one on disk: what the player dropped when
// that was already a package, and what came out of it when it was an archive.
static char unpackedPath[FS_MAXPATH + 1];
static s32 unpackFailed;

// A lookup that was not allowed to unpack has already come up empty. Set the
// first time that happens and cleared only by xblaImportRedetect(), because
// the one other thing that can change the answer - an unpack - fills
// unpackedPath above and is found before this is read.
static s32 notReady;

// Held across an unpack, because both the game thread (the mesh loader asking
// for the package as a level loads) and the import worker can be the one to
// find it missing, and two of them extracting 250MB into the same directory at
// once is not something either would survive.
static SDL_mutex *unpackMutex;

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

static s32 xblaPathIsDir(const char *path)
{
	struct stat st;

	return stat(fsFullPath(path), &st) == 0 && S_ISDIR(st.st_mode);
}

struct xblascan {
	char found[FS_MAXPATH + 1];
	const char *dir;
	s32 archives; // take a .7z or a .zip as well as a package
	s32 depth;    // directories still to look into
};

static s32 xblaScanDir(const char *dir, s32 archives, s32 depth, char *dst, u32 dstLen);

static void xblaScanForPackage(const char *name, void *arg)
{
	struct xblascan *scan = arg;
	char path[FS_MAXPATH + 1];

	if (scan->found[0] || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", scan->dir, name);

	// A package is taken on what it starts with, an archive on its extension:
	// a package is named after a content id hash and an archive is not
	// necessarily named anything in particular either.
	if (xblaLooksLikePackage(path) ||
			(scan->archives && archiveIsSupported(path) && fsFileSize(path) >= 0)) {
		strncpy(scan->found, path, sizeof(scan->found) - 1);
		return;
	}

	if (scan->depth > 0 && xblaPathIsDir(path)) {
		xblaScanDir(path, scan->archives, scan->depth - 1, scan->found, sizeof(scan->found));
	}
}

/**
 * The first package - or archive, when asked for one - at or under dir.
 *
 * dir must already be expanded: xblaLooksLikePackage() opens what it is given
 * and fsFullPath()'s buffer is one deep, so a "$E/..." handed down through the
 * recursion would be re-expanded under itself.
 */
static s32 xblaScanDir(const char *dir, s32 archives, s32 depth, char *dst, u32 dstLen)
{
	struct xblascan scan;

	memset(&scan, 0, sizeof(scan));
	scan.dir = dir;
	scan.archives = archives;
	scan.depth = depth;

	fsScanDir(dir, xblaScanForPackage, &scan);

	if (!scan.found[0]) {
		return 0;
	}

	strncpy(dst, scan.found, dstLen - 1);
	dst[dstLen - 1] = '\0';

	return 1;
}

/** The first STFS package directly inside dir, if there is one. */
static s32 xblaFindPackageIn(const char *dir, char *dst, u32 dstLen)
{
	return xblaScanDir(dir, 0, 0, dst, dstLen);
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

/**
 * The xbla/ folder: where the player is told to put their copy.
 *
 * Beside the executable where that can be written and in the save directory
 * where it cannot, the same choice screenshots and recordings make - and it is
 * created here, so a player who has never had a package still has somewhere
 * obvious to put one. dst gets the expanded path.
 */
static s32 xblaDropDir(char *dst, u32 dstLen)
{
	char rel[FS_MAXPATH + 1];

	if (fsChooseOutputDir(XBLAIMPORT_XBLA_DIR, rel, sizeof(rel)) != 0) {
		dst[0] = '\0';
		return 0;
	}

	snprintf(dst, dstLen, "%s", fsFullPath(rel));

	return 1;
}

static void xblaDetect(void)
{
	// xbla/ wherever it is, then the places that were searched before it
	// existed, so an install that was already working keeps working.
	static const char *const dirs[] = {
		"$E/" XBLAIMPORT_XBLA_DIR,
		"$H/" XBLAIMPORT_XBLA_DIR,
		"./" XBLAIMPORT_XBLA_DIR,
		"$S/" XBLAIMPORT_XBLA_DIR,
	};
	static const char *const roots[] = { "$H", "$E", "$S" };

	detected = 1;
	packagePath[0] = '\0';

	if (xblaTryPath(configuredPath)) {
		return;
	}

	for (s32 d = 0; d < ARRAYCOUNT(dirs); d++) {
		// fsFullPath() hands back one buffer, so the expansion is copied out
		// before anything else is composed against it.
		char dir[FS_MAXPATH + 1];
		char path[FS_MAXPATH + 1];

		snprintf(dir, sizeof(dir), "%s", fsFullPath(dirs[d]));

		// A package outright before an archive that would have to be unpacked:
		// a player who has both has already paid for the extraction once.
		if (xblaScanDir(dir, 0, XBLAIMPORT_SCAN_DEPTH, path, sizeof(path)) && xblaTryPath(path)) {
			return;
		}

		if (xblaScanDir(dir, 1, XBLAIMPORT_SCAN_DEPTH, path, sizeof(path)) && xblaTryPath(path)) {
			return;
		}
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

/**
 * The package an earlier run left unpacked under the texture packs, before
 * xbla/ existed. Read only: 250MB is not worth extracting twice to move it.
 */
static s32 xblaFindLegacyUnpacked(char *dst, u32 dstLen)
{
	char rel[FS_MAXPATH + 1];
	char dir[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	if (fsChooseOutputDir(TEXPACK_PACKS_DIR, rel, sizeof(rel)) != 0) {
		return 0;
	}

	snprintf(dir, sizeof(dir), "%s/" XBLAIMPORT_WORK_DIR, fsFullPath(rel));

	if (xblaFindPackageIn(dir, dst, dstLen)) {
		return 1;
	}

	snprintf(sub, sizeof(sub), "%s/Perfect Dark", dir);

	return xblaFindPackageIn(sub, dst, dstLen);
}

/**
 * cache/xbla/, made if it has to be. dst gets the expanded path.
 */
static s32 xblaCacheDir(char *dst, u32 dstLen)
{
	char rel[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	if (fsChooseOutputDir(XBLAIMPORT_CACHE_DIR, rel, sizeof(rel)) != 0) {
		dst[0] = '\0';
		return 0;
	}

	snprintf(sub, sizeof(sub), "%s/" XBLAIMPORT_CACHE_SUB, rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(dst, dstLen, "%s", fsFullPath(sub));

	return 1;
}

/**
 * A finished extraction in dir: its marker is there and a package is inside.
 */
static s32 xblaFindExtractedIn(const char *dir, char *dst, u32 dstLen)
{
	char marker[FS_MAXPATH + 1];

	snprintf(marker, sizeof(marker), "%s/" XBLAIMPORT_DONE_FILE, dir);

	return fsFileSize(marker) >= 0 &&
			xblaScanDir(dir, 0, XBLAIMPORT_SCAN_DEPTH, dst, dstLen);
}

static const char *xblaEnsureUnpackedLocked(s32 mayUnpack)
{
	char drop[FS_MAXPATH + 1];
	char dir[FS_MAXPATH + 1];
	char marker[FS_MAXPATH + 1];
	FILE *fp;

	if (unpackedPath[0]) {
		return unpackedPath;
	}

	if (unpackFailed || !xblaImportIsAvailable()) {
		return NULL;
	}

	// What the player dropped is already a package - nothing to do.
	if (xblaLooksLikePackage(packagePath)) {
		snprintf(unpackedPath, sizeof(unpackedPath), "%s", packagePath);
		return unpackedPath;
	}

	if (!xblaCacheDir(dir, sizeof(dir))) {
		unpackFailed = 1;
		return NULL;
	}

	// The archive comes apart in cache/xbla/, and the marker goes in there
	// with it. xbla/ itself is the player's: only what they dropped in it.
	snprintf(marker, sizeof(marker), "%s/" XBLAIMPORT_DONE_FILE, dir);

	// A previous run's work, here or in either of the places it used to go.
	if (xblaFindExtractedIn(dir, unpackedPath, sizeof(unpackedPath))) {
		return unpackedPath;
	}

	if (xblaDropDir(drop, sizeof(drop))) {
		char old[FS_MAXPATH + 1];

		snprintf(old, sizeof(old), "%s/" XBLAIMPORT_UNPACK_DIR, drop);

		if (xblaFindExtractedIn(old, unpackedPath, sizeof(unpackedPath))) {
			sysLogPrintf(LOG_NOTE, "xbla: using the copy unpacked in %s; it can go, "
					"the next unpack lands in %s", old, dir);
			return unpackedPath;
		}
	}

	if (xblaFindLegacyUnpacked(unpackedPath, sizeof(unpackedPath))) {
		return unpackedPath;
	}

	if (!mayUnpack) {
		// Nothing on disk yet and the caller is not the one who should pay for
		// it. Not remembered as a *failure* - the next caller may be willing
		// to unpack, and has to be able to - but remembered as an answer, so
		// that the model loads still to come get it for a flag read.
		notReady = 1;
		return NULL;
	}

	sysLogPrintf(LOG_NOTE, "xbla: unpacking %s into %s, this happens once", packagePath, dir);

	if (archiveExtract(packagePath, dir) <= 0) {
		sysLogPrintf(LOG_ERROR, "xbla: could not unpack %s", packagePath);
		unpackFailed = 1;
		return NULL;
	}

	if (!xblaScanDir(dir, 0, XBLAIMPORT_SCAN_DEPTH, unpackedPath, sizeof(unpackedPath))) {
		sysLogPrintf(LOG_ERROR, "xbla: no Xbox 360 package inside %s", packagePath);
		unpackFailed = 1;
		return NULL;
	}

	fp = fopen(fsFullPath(marker), "wb");

	if (fp) {
		fclose(fp);
	}

	sysLogPrintf(LOG_NOTE, "xbla: unpacked %s", unpackedPath);

	return unpackedPath;
}

static const char *xblaEnsureUnpacked(s32 mayUnpack)
{
	const char *path;

	if (unpackedPath[0]) {
		return unpackedPath;
	}

	// Every model load asks this, so the empty answer has to be cheap. Without
	// it each one is a mutex, a stat, two directory scans and a read of the
	// first four bytes of the player's archive to see whether it is a package
	// after all - straced at 57 opens of a 233MB archive and 110 directory
	// probes over one load of the G5 Building, all of them re-deciding what
	// the first one decided. Read outside the lock the way
	// unpackedPath above is, and for the same reason: the worst a race can do
	// is one more scan than was needed.
	if (!mayUnpack && notReady) {
		return NULL;
	}

	if (unpackMutex) {
		SDL_LockMutex(unpackMutex);
	}

	path = xblaEnsureUnpackedLocked(mayUnpack);

	if (unpackMutex) {
		SDL_UnlockMutex(unpackMutex);
	}

	return path;
}

const char *xblaImportGetStfsPath(void)
{
	return xblaEnsureUnpacked(1);
}

const char *xblaImportGetReadyStfsPath(void)
{
	return xblaEnsureUnpacked(0);
}

const char *xblaImportGetDropDir(void)
{
	static char dir[FS_MAXPATH + 1];

	if (!dir[0]) {
		xblaDropDir(dir, sizeof(dir));
	}

	return dir;
}

void xblaImportRedetect(void)
{
	detected = 0;
	unpackedPath[0] = '\0';
	unpackFailed = 0;
	notReady = 0;
	xblaImportIsAvailable();
}

/**
 * Makes the xbla/ folder and says in the log what is in it, so a player who
 * has never had a package still finds somewhere to put one and a player whose
 * copy was not found can see where it was looked for.
 *
 * Nothing is unpacked here: that waits until something actually reads the
 * package, which is the texture conversion or the mesh loader with
 * Mod.XblaMeshes on.
 */
void xblaImportInit(void)
{
	char dir[FS_MAXPATH + 1];

	if (!unpackMutex) {
		unpackMutex = SDL_CreateMutex();
	}

	// A pack converted before a texture was left out still has it, and a
	// player is not made to convert again to lose it. Both places a pack can
	// be are looked at rather than asking texpackPacksDir(), which would
	// create one; this needs no package, so it goes before the drop folder.
	{
		static const char *const roots[] = { "$E", "$S" };

		for (u32 i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
			snprintf(dir, sizeof(dir), "%s/" TEXPACK_PACKS_DIR "/%s/textures", roots[i], packName);
			xblaImportDropLeftOut(dir);
		}
	}

	if (!xblaDropDir(dir, sizeof(dir))) {
		return;
	}

	if (xblaImportIsAvailable()) {
		sysLogPrintf(LOG_NOTE, "xbla: using %s", packagePath);
	} else {
		sysLogPrintf(LOG_NOTE, "xbla: no package; put Perfect Dark XBLA.7z in %s", dir);
	}
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

		if (xblaImportTextureIsLeftOut((s32)n)) {
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

	// An archive has to come apart first. A .7z is one LZMA stream, so there
	// is no reading a single file out of it cheaply - see archiveExtract().
	// The mesh loader wants the same package, so both go through the one
	// unpack and whichever gets there first pays for it.
	if (!xblaLooksLikePackage(packagePath)) {
		SDL_AtomicSet(&workerStage, XBLAIMPORT_EXTRACTING);
	}

	{
		const char *found = xblaImportGetStfsPath();

		if (!found) {
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

	snprintf(packDir, sizeof(packDir), "%s/%s", fsFullPath(rel), packName);
	fsCreateDir(packDir);
	snprintf(packDir, sizeof(packDir), "%s/%s/" "textures", fsFullPath(rel), packName);
	fsCreateDir(packDir);

	// The loop below passes over a left-out texture rather than writing it,
	// so a file an older conversion put there has to be removed outright.
	xblaImportDropLeftOut(packDir);

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
 * For setting a machine up without sitting in front of it, and the only way to
 * exercise the conversion in a headless run.
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

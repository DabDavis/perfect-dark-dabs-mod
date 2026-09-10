/**
 * The XBLA release's own textures, handed to the renderer by address.
 * See xblatex.h for what this is and why it cannot go through a texture pack.
 *
 * Everything here is one texture at a time. Textures.raw is 166MB and is never
 * held in memory - the two record tables are read once when the package is
 * opened, and a picture is streamed, decompressed and decoded only when the
 * renderer asks for it, which is once per texture per fill of its cache.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "config.h"
#include "system.h"
#include "video.h"
#include "x360.h"
#include "xblaimport.h"
#include "xblatex.h"

// Inside the package. The only file this reads.
#define XBLATEX_TEXTURES "DataFiles/Textures.raw"

// The two 52 byte tables: the metadata, then the D3DTexture structs whose last
// six dwords are the GPU fetch constant. Same shape xblaimport.c reads.
#define XBLATEX_RECORD 52
#define XBLATEX_FETCH_DWORD 7

// A record whose dimensions are past this is not something to allocate for.
#define XBLATEX_MAXDIM 4096

// How many decodes are named in the log before it stops.
#define XBLATEX_DECODELOG 16

// Bound records, open addressed on the address of the stand-in. The meshes'
// materials name records 3741 to 5746, so 2006 is every texture the release
// could ever ask for; this is sized so half of it is comfortably past that and
// the table never fills or slows down.
#define XBLATEX_HASHSIZE 8192

// What a stand-in tile holds if anything ever reads it - which only happens
// when the decode below fails, since a picture that arrives replaces it. White
// is the one wrong answer that cannot darken what it multiplies.
#define XBLATEX_TILE_TEXELS (XBLATEX_TILE * XBLATEX_TILE)
#define XBLATEX_TILE_BYTES  (XBLATEX_TILE_TEXELS * 2)

struct xblatexentry {
	u8 *addr;      // the stand-in the display list binds; the key
	u32 record;
};

static s32 opened; // 0 untried, 1 open, -1 no package
static s32 numBound;
static s32 numDecoded;

/**
 * Mod.XblaMeshTextures: the release's own art on the release's own meshes.
 *
 * On, because an untextured mesh is a flat pale solid and is not what anyone
 * turns the meshes on to see; off is how a shape that is wrong is told apart
 * from a texture that is.
 *
 * The flag lives here, at the point a picture is handed over, rather than
 * where a display list is built - which is what makes it a live toggle. A
 * material always binds its stand-in, and this only decides whether the
 * release's picture arrives in the stand-in's place; the stand-in's own texels
 * are white, so with this off a material draws white times shade, which is the
 * same flat solid as a list built with no texture at all. Nothing has to be
 * built again, and the menu only has to drop the texture cache.
 */
static s32 optEnabled = 1;

static struct x360stfs stfs;
static struct x360stfsstream stream;
static u8 *tables;
static u32 numRecords;
static u32 dataBase;

static struct xblatexentry hash[XBLATEX_HASHSIZE];
static u8 **byRecord;  // one stand-in per record, or NULL
static u8 *badRecord;  // a record that did not decode, so it is not tried again
static u8 *softRecord; // XBLATEX_SOFT_* per record, see xblaTexRecordIsSoft()

// What xblaTexRecordIsSoft() has found out about a record so far.
#define XBLATEX_SOFT_UNKNOWN 0
#define XBLATEX_SOFT_NO      1
#define XBLATEX_SOFT_YES     2

// A texel at or above this alpha is opaque, allowing for what DXT5 does to a
// flat 255 - the same line xblamesh.c's XBLAMESH_FADE_ALPHA draws for a vertex.
#define XBLATEX_OPAQUE_ALPHA 0xf0

// A record with fewer opaque texels than this, in hundredths, is a soft one:
// a glow, a glass, a haze. Counted over the 59 alpha records the meshes use,
// nine are under it and every one of them is a picture with no edge to cut at
// (the least opaque of the rest is a lamp with 15% of its texels at 255).
#define XBLATEX_SOFT_PERCENT 1

// The meshes are built on the game thread and uploaded on the render thread,
// so both the registry and the package handle are shared. Everything below
// that touches either takes this. Made before either thread exists, so that
// nothing has to decide whether it is the one to make it.
static SDL_mutex *lock;

PD_CONSTRUCTOR static void xblaTexInit(void)
{
	lock = SDL_CreateMutex();

	configRegisterInt("Mod.XblaMeshTextures", &optEnabled, 0, 1);
}

static u32 xblaTexBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void xblaTexCloseUp(void)
{
	x360StfsStreamClose(&stream);
	x360StfsClose(&stfs);
	free(tables);
	free(byRecord);
	free(badRecord);
	free(softRecord);
	tables = NULL;
	byRecord = NULL;
	badRecord = NULL;
	softRecord = NULL;
	numRecords = 0;
	opened = -1;
}

/**
 * Opens the package and reads both record tables.
 *
 * The data does not start after the first table but after both of them, which
 * is what makes the offsets chain: reading the base as one table's worth
 * leaves every texture short by exactly one table.
 */
static s32 xblaTexOpen(void)
{
	const char *path;
	s32 index;
	u8 head[4];

	if (opened) {
		return opened > 0;
	}

	opened = -1;

	path = xblaImportGetStfsPath();

	if (!path || !path[0]) {
		return 0;
	}

	if (!x360StfsOpen(&stfs, path)) {
		sysLogPrintf(LOG_ERROR, "xblatex: %s is not a package", path);
		return 0;
	}

	index = x360StfsFind(&stfs, XBLATEX_TEXTURES);

	if (index < 0 || !x360StfsStreamOpen(&stfs, (u32)index, &stream)) {
		sysLogPrintf(LOG_ERROR, "xblatex: no " XBLATEX_TEXTURES " in %s", path);
		x360StfsClose(&stfs);
		return 0;
	}

	if (!x360StfsStreamRead(&stream, 0, sizeof(head), head)) {
		xblaTexCloseUp();
		return 0;
	}

	numRecords = xblaTexBE32(head);
	dataBase = 4 + numRecords * 2 * XBLATEX_RECORD;

	if (numRecords == 0 || numRecords > 0x10000 || dataBase > stream.size) {
		sysLogPrintf(LOG_ERROR, "xblatex: %u records do not fit in %u bytes",
				numRecords, stream.size);
		xblaTexCloseUp();
		return 0;
	}

	tables = malloc(dataBase - 4);
	byRecord = calloc(numRecords, sizeof(u8 *));
	badRecord = calloc(numRecords, 1);
	softRecord = calloc(numRecords, 1);

	if (!tables || !byRecord || !badRecord || !softRecord ||
			!x360StfsStreamRead(&stream, 4, dataBase - 4, tables)) {
		sysLogPrintf(LOG_ERROR, "xblatex: could not read the texture tables");
		xblaTexCloseUp();
		return 0;
	}

	opened = 1;

	sysLogPrintf(LOG_NOTE, "xblatex: %u texture records in %s", numRecords, path);

	return 1;
}

/* -------------------------------------------------------------------------
 * The registry
 * ------------------------------------------------------------------------- */

static u32 xblaTexHashOf(const void *addr)
{
	// A malloc'd pointer's low bits are all alignment, so they are shifted off
	// before the mix rather than left to collide.
	u64 x = (u64)(uintptr_t)addr >> 4;

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 29;

	return (u32)x & (XBLATEX_HASHSIZE - 1);
}

static struct xblatexentry *xblaTexFind(const void *addr)
{
	u32 slot = xblaTexHashOf(addr);

	for (u32 i = 0; i < XBLATEX_HASHSIZE; i++) {
		struct xblatexentry *e = &hash[(slot + i) & (XBLATEX_HASHSIZE - 1)];

		if (!e->addr) {
			return NULL;
		}

		if (e->addr == addr) {
			return e;
		}
	}

	return NULL;
}

const void *xblaTexBind(u32 record)
{
	u8 *addr;
	u32 slot;

	if (!lock) {
		return NULL;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || numBound >= XBLATEX_HASHSIZE / 2) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	if (byRecord[record]) {
		addr = byRecord[record];
		SDL_UnlockMutex(lock);
		return addr;
	}

	addr = malloc(XBLATEX_TILE_BYTES);

	if (!addr) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	memset(addr, 0xff, XBLATEX_TILE_BYTES);

	slot = xblaTexHashOf(addr);

	while (hash[slot].addr) {
		slot = (slot + 1) & (XBLATEX_HASHSIZE - 1);
	}

	hash[slot].addr = addr;
	hash[slot].record = record;
	byRecord[record] = addr;
	numBound++;

	SDL_UnlockMutex(lock);

	return addr;
}

s32 xblaTexHaveTextures(void)
{
	return numBound > 0 && optEnabled;
}

s32 xblaTexGetEnabled(void)
{
	return optEnabled;
}

void xblaTexSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled == optEnabled) {
		return;
	}

	optEnabled = enabled;

	// What a texture holds is decided as it is uploaded, and an upload is kept
	// against the address it came from - so without this the meshes keep the
	// art they already have until something else evicts it.
	videoResetTextureCache();
}

/* -------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------- */

/**
 * One record, decompressed and decoded. Called with the lock held.
 *
 * Every failure here is the same failure to the caller - it gets NULL and the
 * stand-in's white is drawn - so the record is marked rather than retried, and
 * the reason is logged once.
 */
static u8 *xblaTexDecode(u32 record, s32 *outWidth, s32 *outHeight)
{
	const u8 *a = tables + record * XBLATEX_RECORD;
	const u8 *b = tables + (numRecords + record) * XBLATEX_RECORD;
	const u32 offset = xblaTexBE32(a);
	const u32 width = xblaTexBE32(a + 4);
	const u32 height = xblaTexBE32(a + 8);
	const u32 usize = xblaTexBE32(a + 20);
	const u32 csize = xblaTexBE32(a + 24);
	struct x360fetch fetch;
	u32 dwords[6];
	u8 *compressed = NULL;
	u8 *surface = NULL;
	u8 *rgba = NULL;

	for (u32 k = 0; k < 6; k++) {
		dwords[k] = xblaTexBE32(b + (XBLATEX_FETCH_DWORD + k) * 4);
	}

	x360FetchRead(&fetch, dwords);

	if (!usize || !csize || !width || !height ||
			width > XBLATEX_MAXDIM || height > XBLATEX_MAXDIM ||
			fetch.width != width || fetch.height != height ||
			!x360FetchSupported(&fetch) ||
			offset > stream.size || csize > stream.size - offset) {
		sysLogPrintf(LOG_WARNING, "xblatex: record %u is %ux%u format %u, "
				"which is not a texture this reads", record, width, height, fetch.format);
		badRecord[record] = 1;
		return NULL;
	}

	compressed = malloc(csize);
	surface = malloc(usize);
	rgba = malloc((size_t)width * height * 4);

	if (!compressed || !surface || !rgba) {
		free(compressed);
		free(surface);
		free(rgba);
		badRecord[record] = 1;
		return NULL;
	}

	if (!x360StfsStreamRead(&stream, dataBase + offset, csize, compressed) ||
			x360LzxDecompress(compressed, csize, surface, usize) != usize ||
			!x360DecodeTexture(surface, usize, &fetch, rgba)) {
		sysLogPrintf(LOG_WARNING, "xblatex: record %u did not decode", record);
		badRecord[record] = 1;
		free(compressed);
		free(surface);
		free(rgba);
		return NULL;
	}

	free(compressed);
	free(surface);

	numDecoded++;

	if (numDecoded <= XBLATEX_DECODELOG) {
		sysLogPrintf(LOG_NOTE, "xblatex: record %u decoded, %ux%u format %u",
				record, width, height, fetch.format);
	}

	*outWidth = (s32)width;
	*outHeight = (s32)height;

	return rgba;
}

s32 xblaTexRecordSize(u32 record, s32 *outWidth, s32 *outHeight)
{
	const u8 *a;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords) {
		SDL_UnlockMutex(lock);
		return 0;
	}

	a = tables + record * XBLATEX_RECORD;
	*outWidth = (s32)xblaTexBE32(a + 4);
	*outHeight = (s32)xblaTexBE32(a + 8);

	SDL_UnlockMutex(lock);

	return *outWidth > 0 && *outHeight > 0;
}

/**
 * The N64 size a record stands in for - equal to its own size for the
 * release's own art, and the ROM tile's size for a replacement.
 */
s32 xblaTexRecordSrcSize(u32 record, s32 *outWidth, s32 *outHeight)
{
	const u8 *a;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords) {
		SDL_UnlockMutex(lock);
		return 0;
	}

	a = tables + record * XBLATEX_RECORD;
	*outWidth = (s32)xblaTexBE32(a + 12);
	*outHeight = (s32)xblaTexBE32(a + 16);

	SDL_UnlockMutex(lock);

	return *outWidth > 0 && *outHeight > 0;
}

s32 xblaTexRecordIsSoft(u32 record)
{
	s32 width = 0;
	s32 height = 0;
	u8 *rgba;
	u32 opaque = 0;
	u32 total;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || badRecord[record]) {
		SDL_UnlockMutex(lock);
		return 0;
	}

	if (softRecord[record] == XBLATEX_SOFT_UNKNOWN) {
		// Decoded once here and thrown away; the render thread decodes it
		// again when the list draws, which it would have done anyway.
		rgba = xblaTexDecode(record, &width, &height);

		if (!rgba) {
			SDL_UnlockMutex(lock);
			return 0;
		}

		total = (u32)width * (u32)height;

		for (u32 i = 0; i < total; i++) {
			if (rgba[i * 4 + 3] >= XBLATEX_OPAQUE_ALPHA) {
				opaque++;
			}
		}

		free(rgba);

		softRecord[record] = (opaque * 100 < total * XBLATEX_SOFT_PERCENT)
				? XBLATEX_SOFT_YES : XBLATEX_SOFT_NO;

		sysLogPrintf(LOG_NOTE, "xblatex: record %u is %ux%u with %u of %u texels opaque%s",
				record, width, height, opaque, total,
				softRecord[record] == XBLATEX_SOFT_YES ? " - soft, drawn blended" : "");
	}

	SDL_UnlockMutex(lock);

	return softRecord[record] == XBLATEX_SOFT_YES;
}

u8 *xblaTexLoadReplacement(const void *addr, s32 *outWidth, s32 *outHeight)
{
	struct xblatexentry *e;
	u8 *rgba;

	if (numBound == 0 || !lock) {
		return NULL;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (!e || opened <= 0 || badRecord[e->record]) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	rgba = xblaTexDecode(e->record, outWidth, outHeight);

	SDL_UnlockMutex(lock);

	return rgba;
}

void xblaTexFreeReplacement(u8 *rgba)
{
	free(rgba);
}

void xblaTexShutdown(void)
{
	if (lock) {
		SDL_LockMutex(lock);
	}

	// Decodes against bound records. A decode is one LZX stream and it happens
	// on the render thread, so the two numbers being far apart means the
	// texture cache is evicting these and paying for them again - which is the
	// thing to look at if a mesh-heavy room stutters.
	if (numBound) {
		sysLogPrintf(LOG_NOTE, "xblatex: %d records bound, %d decodes",
				numBound, numDecoded);
	}

	if (opened > 0) {
		x360StfsStreamClose(&stream);
		x360StfsClose(&stfs);
		free(tables);
		tables = NULL;
		opened = -1;
	}

	if (lock) {
		SDL_UnlockMutex(lock);
	}
}

void xblaTexTrace(FILE *f)
{
	fprintf(f, "xblatex: enabled %d opened %d (0 untried, 1 open, -1 no package), %u records, %d bound, %d decodes\n",
			optEnabled, opened, numRecords, numBound, numDecoded);
}

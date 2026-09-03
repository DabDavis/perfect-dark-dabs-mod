/**
 * Texture identity registry and texture dumping.
 *
 * Emulators have to earn a texture's identity the hard way: all they see is
 * bytes landing in TMEM, so they hash the texels and the palette and hope the
 * result is both stable and collision free. A port does not have that problem.
 * struct tex carries the texture number right next to the decompressed data
 * pointer that ends up in gDPSetTextureImage, so the two only need connecting.
 * That is the registry below, and it makes dumps exactly named rather than
 * named after a checksum.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ultra64.h>
#include "constants.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "pngread.h"
#include "pngwrite.h"
#include "system.h"
#include "texpack.h"
#include "versioninfo.h"

#define TEXPACK_DUMP_DIR_NAME "texturedump"

// Matches the "textures" directory modTextureLoad() already reads its %04x.bin
// replacements from, so one pack directory holds both kinds.
#define TEXPACK_DIR_NAME "textures"

// Slots are never fewer than this, so the table is allocated once for a level
// rather than grown through the small sizes on the way up.
#define TEXPACK_MIN_SLOTS 4096

// A slot whose texture has been forgotten. Linear probing cannot simply blank
// one: that would cut the probe chain of anything that collided with it and
// landed further along, losing entries that are still live.
#define TEXPACK_TOMBSTONE ((const void *)(uintptr_t)1)

struct texpackslot {
	const void *data;
	s32 texturenum;
};

static struct texpackslot *slots;
static u32 numSlots;    // always a power of two
static u32 numOccupied; // live entries plus tombstones
static u32 numLive;

static s32 loadTextures = 1;
static char **replacePaths;   // one per texture number, NULL where there is none
static s32 replaceScanned;    // the scan runs once, on the first texture drawn
static s32 numReplacements;

static s32 dumpTextures = 0;
static s32 dumpTextureData = 0;
static FILE *dumpManifest;
static char dumpDir[FS_MAXPATH + 1];
static s32 dumpDirState; // 0 = not looked at yet, 1 = ready, -1 = gave up
static u8 dumpDone[(NUM_TEXTURES + 7) / 8];

static inline u32 texpackHash(const void *data)
{
	// Pool allocations are 8 and 16 byte aligned, so the low bits of the
	// pointer carry almost nothing and the top ones differ only between pools.
	// Mixing is what spreads them over the table.
	u64 x = (u64)(uintptr_t)data;

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 29;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 32;

	return (u32)x;
}

/**
 * Reallocates the table with room for at least wantSlots and reinserts what is
 * live, which is also how tombstones get cleared out.
 *
 * Returns false with the old table still in place if the allocation failed.
 */
static s32 texpackResize(u32 wantSlots)
{
	struct texpackslot *old = slots;
	const u32 oldSlots = numSlots;
	u32 n = TEXPACK_MIN_SLOTS;
	u32 i;

	while (n < wantSlots) {
		n <<= 1;
	}

	slots = calloc(n, sizeof(struct texpackslot));

	if (!slots) {
		slots = old;
		sysLogPrintf(LOG_ERROR, "texpack: could not size the id registry to %u slots", n);
		return 0;
	}

	numSlots = n;
	numOccupied = 0;
	numLive = 0;

	for (i = 0; i < oldSlots; i++) {
		if (old[i].data && old[i].data != TEXPACK_TOMBSTONE) {
			// Cannot recurse into another resize: n was chosen to hold these.
			texpackRegisterTexture(old[i].data, old[i].texturenum);
		}
	}

	free(old);

	return 1;
}

void texpackRegisterTexture(const void *data, s32 texturenum)
{
	u32 firstTombstone = (u32)-1;
	u32 base;
	u32 i;

	if (!data || data == TEXPACK_TOMBSTONE || texturenum < 0) {
		return;
	}

	// Half full is where linear probing starts costing more than the memory
	// saves. Tombstones count towards it because they lengthen probes just as
	// live entries do, so a table churned by level loads still gets rebuilt.
	if (numSlots == 0 || (numOccupied + 1) * 2 > numSlots) {
		if (!texpackResize((numLive + 1) * 4) && numSlots == 0) {
			return;
		}
	}

	base = texpackHash(data);

	for (i = 0; i < numSlots; i++) {
		const u32 slot = (base + i) & (numSlots - 1);

		if (slots[slot].data == data) {
			slots[slot].texturenum = texturenum;
			return;
		}

		if (slots[slot].data == TEXPACK_TOMBSTONE) {
			if (firstTombstone == (u32)-1) {
				firstTombstone = slot;
			}
			continue;
		}

		if (slots[slot].data == NULL) {
			if (firstTombstone != (u32)-1) {
				// Already counted in numOccupied when it was live.
				slots[firstTombstone].data = data;
				slots[firstTombstone].texturenum = texturenum;
			} else {
				slots[slot].data = data;
				slots[slot].texturenum = texturenum;
				numOccupied++;
			}

			numLive++;
			return;
		}
	}

	// Only reachable when the resize above failed and the table is genuinely
	// full. Dropping the texture costs a dump or a replacement, nothing more.
}

s32 texpackGetTextureNum(const void *data)
{
	u32 base;
	u32 i;

	if (!slots || !data || data == TEXPACK_TOMBSTONE) {
		return -1;
	}

	base = texpackHash(data);

	for (i = 0; i < numSlots; i++) {
		const u32 slot = (base + i) & (numSlots - 1);

		if (slots[slot].data == data) {
			return slots[slot].texturenum;
		}

		if (slots[slot].data == NULL) {
			break;
		}
	}

	return -1;
}

void texpackForgetTexture(const void *data)
{
	u32 base;
	u32 i;

	if (!slots || !data || data == TEXPACK_TOMBSTONE) {
		return;
	}

	base = texpackHash(data);

	for (i = 0; i < numSlots; i++) {
		const u32 slot = (base + i) & (numSlots - 1);

		if (slots[slot].data == data) {
			slots[slot].data = TEXPACK_TOMBSTONE;
			slots[slot].texturenum = -1;
			numLive--;
			return;
		}

		if (slots[slot].data == NULL) {
			return;
		}
	}
}

void texpackForgetRange(const void *start, const void *end)
{
	u32 i;

	if (!slots || !start || start >= end) {
		return;
	}

	// No way to probe for this: the entries wanted are contiguous in memory,
	// not in the table.
	for (i = 0; i < numSlots; i++) {
		if (slots[i].data && slots[i].data != TEXPACK_TOMBSTONE
				&& slots[i].data >= start && slots[i].data < end) {
			slots[i].data = TEXPACK_TOMBSTONE;
			slots[i].texturenum = -1;
			numLive--;
		}
	}
}

void texpackForgetAll(void)
{
	if (slots) {
		memset(slots, 0, numSlots * sizeof(struct texpackslot));
	}

	numOccupied = 0;
	numLive = 0;
}

/**
 * Records one candidate filename against its texture number.
 *
 * The name is <texnum>.png, four lowercase hex digits, optionally followed by
 * an underscore and anything at all - which is what the dumper writes
 * (0a9a_i8.png), so a dump can be edited and dropped back in without renaming.
 */
static void texpackIndexFile(const char *name, void *arg)
{
	const char *dir = arg;
	char digits[5];
	const char *rest;
	char *end;
	char *path;
	u32 len;
	s32 texturenum;

	if (strlen(name) < 8) { // 4 digits + ".png"
		return;
	}

	memcpy(digits, name, 4);
	digits[4] = '\0';

	texturenum = (s32)strtol(digits, &end, 16);

	if (*end || texturenum < 0 || texturenum >= NUM_TEXTURES) {
		return;
	}

	rest = name + 4;

	if (*rest == '_') {
		rest = strrchr(rest, '.');

		if (!rest) {
			return;
		}
	}

	if (strcasecmp(rest, ".png")) {
		return;
	}

	len = strlen(dir) + strlen(name) + 2;
	path = malloc(len);

	if (!path) {
		return;
	}

	snprintf(path, len, "%s/%s", dir, name);

	// A later directory outranks an earlier one: the scan walks from the base
	// directory up through the mod directories in reverse priority order, so
	// whatever is found last is what the path search would have picked.
	if (replacePaths[texturenum]) {
		free(replacePaths[texturenum]);
	} else {
		numReplacements++;
	}

	replacePaths[texturenum] = path;
}

static void texpackScanDir(const char *dir)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/" TEXPACK_DIR_NAME, dir);

	// fsScanDir() takes a VFS path and resolves it through the search order,
	// which would collapse every mod directory onto whichever one wins. These
	// are absolute paths precisely so each is scanned in its own right.
	fsScanDir(path, texpackIndexFile, path);
}

/**
 * Builds the texture-number-to-file index, once.
 *
 * Done up front rather than by looking for a file per texture: a miss is the
 * common case by far, and a stat on every texture load - through the mod search
 * path, so several stats - would be paid forever for packs that do not exist.
 */
static void texpackScan(void)
{
	s32 i;

	replaceScanned = 1;

	if (!loadTextures) {
		return;
	}

	replacePaths = calloc(NUM_TEXTURES, sizeof(char *));

	if (!replacePaths) {
		sysLogPrintf(LOG_ERROR, "texpack: could not alloc the replacement index");
		return;
	}

	texpackScanDir(fsFullPath("$B"));

	for (i = fsGetNumModDirs() - 1; i >= 0; i--) {
		texpackScanDir(fsGetModDirAt(i));
	}

	if (numReplacements) {
		sysLogPrintf(LOG_NOTE, "texpack: %d replacement textures", numReplacements);
	} else {
		free(replacePaths);
		replacePaths = NULL;
	}
}

s32 texpackHaveReplacements(void)
{
	if (!replaceScanned) {
		texpackScan();
	}

	return replacePaths != NULL;
}

u8 *texpackLoadReplacement(const void *data, s32 *outWidth, s32 *outHeight)
{
	s32 texturenum;
	u8 *rgba;
	s32 width;
	s32 height;
	s32 y;

	if (!texpackHaveReplacements()) {
		return NULL;
	}

	texturenum = texpackGetTextureNum(data);

	if (texturenum < 0 || texturenum >= NUM_TEXTURES || !replacePaths[texturenum]) {
		return NULL;
	}

	rgba = pngRead(replacePaths[texturenum], &width, &height);

	if (!rgba) {
		// Whatever is wrong with the file will not fix itself, and retrying on
		// every cache miss would log it forever. Drop it and draw the original.
		free(replacePaths[texturenum]);
		replacePaths[texturenum] = NULL;
		numReplacements--;
		return NULL;
	}

	// Back into the game's row order. See the note over the dump for why the
	// two disagree.
	for (y = 0; y < height / 2; y++) {
		u8 *a = rgba + (size_t)width * 4 * y;
		u8 *b = rgba + (size_t)width * 4 * (height - 1 - y);
		u32 x;

		for (x = 0; x < (u32)width * 4; x++) {
			const u8 tmp = a[x];
			a[x] = b[x];
			b[x] = tmp;
		}
	}

	*outWidth = width;
	*outHeight = height;

	return rgba;
}

void texpackFreeReplacement(u8 *rgba)
{
	free(rgba);
}

s32 texpackDumpEnabled(void)
{
	return dumpTextures != 0;
}

static const char *texpackFormatName(u32 fmt, u32 siz)
{
	static const char *const fmts[] = { "rgba", "yuv", "ci", "ia", "i" };
	static const char *const sizes[] = { "4", "8", "16", "32" };
	static char name[16];

	snprintf(name, sizeof(name), "%s%s",
			fmt < (u32)ARRAYCOUNT(fmts) ? fmts[fmt] : "fmt",
			siz < (u32)ARRAYCOUNT(sizes) ? sizes[siz] : "?");

	return name;
}

/**
 * Picks and creates the dump directory on the first texture written, so a run
 * with dumping off never touches the disk.
 */
static s32 texpackOpenDumpDir(void)
{
	char rel[FS_MAXPATH + 1];
	u32 len;

	if (dumpDirState != 0) {
		return dumpDirState > 0;
	}

	dumpDirState = -1;

	if (fsChooseOutputDir(TEXPACK_DUMP_DIR_NAME, rel, sizeof(rel)) != 0) {
		sysLogPrintf(LOG_ERROR, "texpack: nowhere to put %s that can be written",
				TEXPACK_DUMP_DIR_NAME);
		return 0;
	}

	// One directory per ROM version. Texture numbers index that version's own
	// table, so a dump from an NTSC build names different textures to a PAL
	// one and the two must not land on top of each other.
	len = strlen(rel);
	snprintf(rel + len, sizeof(rel) - len, "/%s", VERSION_ROMID);

	if (fsFileSize(rel) < 0 && fsCreateDir(rel) != 0) {
		sysLogPrintf(LOG_ERROR, "texpack: could not create %s", rel);
		return 0;
	}

	strncpy(dumpDir, fsFullPath(rel), sizeof(dumpDir) - 1);
	dumpDir[sizeof(dumpDir) - 1] = '\0';
	dumpDirState = 1;

	sysLogPrintf(LOG_NOTE, "texpack: dumping textures to %s", dumpDir);

	return 1;
}

/**
 * Writes the N64 texel bytes and the tile geometry they sit under, for a
 * converter that has to reproduce an existing pack's checksum of them.
 *
 * One manifest line per texture plus a .raw beside it, and for a CI texture the
 * palette as big-endian 16 bit entries - the byte order it has in the ROM,
 * which is what a checksum computed by an emulator saw.
 */
static void texpackDumpRaw(s32 texturenum, u32 fmt, u32 siz, const struct texpackrawinfo *raw)
{
	char path[FS_MAXPATH + 1];
	FILE *f;

	if (!raw || !raw->data || !raw->sizeBytes) {
		return;
	}

	if (!dumpManifest) {
		snprintf(path, sizeof(path), "%s/manifest.csv", dumpDir);
		dumpManifest = fopen(path, "wb");

		if (!dumpManifest) {
			sysLogPrintf(LOG_ERROR, "texpack: could not open %s", path);
			dumpTextureData = 0;
			return;
		}

		fprintf(dumpManifest, "texnum,fmt,siz,tilewidth,tileheight,linesize,size,palidx\n");
	}

	fprintf(dumpManifest, "%04x,%u,%u,%u,%u,%u,%u,%u\n", texturenum, fmt, siz,
			raw->tileWidth, raw->tileHeight, raw->lineSizeBytes, raw->sizeBytes, raw->paletteIndex);

	// A dumping session usually ends by killing the game rather than quitting
	// it, and the manifest is worth nothing if the last buffer never lands.
	fflush(dumpManifest);

	snprintf(path, sizeof(path), "%s/%04x.raw", dumpDir, texturenum);
	f = fopen(path, "wb");

	if (f) {
		fwrite(raw->data, 1, raw->sizeBytes, f);
		fclose(f);
	}

	if (raw->palette) {
		snprintf(path, sizeof(path), "%s/%04x.pal", dumpDir, texturenum);
		f = fopen(path, "wb");

		if (f) {
			s32 i;

			for (i = 0; i < 256; i++) {
				const u8 be[2] = { raw->palette[i] >> 8, raw->palette[i] & 0xff };
				fwrite(be, 1, 2, f);
			}

			fclose(f);
		}
	}
}

void texpackDumpTexture(const u8 *rgba32, u32 width, u32 height, u32 fmt, u32 siz,
		const struct texpackrawinfo *raw)
{
	char path[FS_MAXPATH + 1];
	s32 texturenum;

	if (!dumpTextures || !rgba32 || width == 0 || height == 0) {
		return;
	}

	texturenum = texpackGetTextureNum(raw ? raw->data : NULL);

	if (texturenum < 0 || texturenum >= NUM_TEXTURES) {
		// Framebuffer captures, the Japanese font glyph cache and anything
		// drawn from a pointer the texture loader never produced all arrive
		// here unregistered. There is no number for a pack to key a
		// replacement on, so there is no point writing them out either.
		return;
	}

	if (dumpDone[texturenum >> 3] & (1 << (texturenum & 7))) {
		return;
	}

	if (!texpackOpenDumpDir()) {
		return;
	}

	// Marked before the write rather than after, so a texture that cannot be
	// written is not retried on every cache miss for the rest of the run.
	dumpDone[texturenum >> 3] |= 1 << (texturenum & 7);

	snprintf(path, sizeof(path), "%s/%04x_%s.png", dumpDir, texturenum, texpackFormatName(fmt, siz));

	// Written bottom row first. The texture data runs the other way up to the
	// image it draws as - nothing in the decompressor, the upload or the shader
	// flips it, so the game's own texture coordinates are what account for it -
	// and an artist opening a dump wants the picture the right way up. A loader
	// reading a pack back has to undo this.
	if (pngWrite(path, rgba32, width, height, 4, 1)) {
		sysLogPrintf(LOG_NOTE, "texpack: dumped %04x %ux%u %s",
				texturenum, width, height, texpackFormatName(fmt, siz));
	}

	if (dumpTextureData) {
		texpackDumpRaw(texturenum, fmt, siz, raw);
	}
}

/**
 * Enough for the largest texture in the ROM plus the tex that describes it. The
 * pool is re-initialised per texture rather than left to fill, because nothing
 * here needs two textures at once.
 */
#define TEXPACK_DUMPALL_POOL (128 * 1024)

void texpackDumpAll(void)
{
	char path[FS_MAXPATH + 1];
	struct texpool pool;
	FILE *manifest;
	u8 *buffer;
	s32 count = 0;
	s32 n;

	if (!sysArgCheck("--dump-textures")) {
		return;
	}

	if (!texpackOpenDumpDir()) {
		return;
	}

	buffer = malloc(TEXPACK_DUMPALL_POOL);

	if (!buffer) {
		sysLogPrintf(LOG_ERROR, "texpack: could not alloc a pool to dump into");
		return;
	}

	snprintf(path, sizeof(path), "%s/manifest.csv", dumpDir);
	manifest = fopen(path, "wb");

	if (!manifest) {
		sysLogPrintf(LOG_ERROR, "texpack: could not open %s", path);
		free(buffer);
		return;
	}

	fprintf(manifest, "texnum,fmt,siz,tilewidth,tileheight,linesize,size,palidx\n");

	for (n = 0; n < NUM_TEXTURES; n++) {
		struct tex *tex;
		s32 size;
		FILE *f;

		texInitPool(&pool, buffer, TEXPACK_DUMPALL_POOL);
		texLoadFromTextureNum(n, &pool);

		tex = texFindInPool(n, &pool);

		if (!tex || !tex->data) {
			// Texture numbers with no data behind them are normal: the table
			// has gaps where a texture was cut.
			continue;
		}

		// texGetLineSizeInBytes() and texGetSizeInBytes() are both named for
		// bytes and both return 64-bit words - the RDP's "line" - so the
		// manifest converts once here rather than leaving every reader to
		// discover it.
		size = texGetSizeInBytes(tex, 0) * 8;

		if (size <= 0) {
			continue;
		}

		fprintf(manifest, "%04x,%u,%u,%d,%d,%d,%d,0\n", n, tex->gbiformat, tex->depth,
				texGetWidthAtLod(tex, 0), texGetHeightAtLod(tex, 0),
				texGetLineSizeInBytes(tex, 0) * 8, size);

		snprintf(path, sizeof(path), "%s/%04x.raw", dumpDir, n);
		f = fopen(path, "wb");

		if (f) {
			fwrite(tex->data, 1, size, f);
			fclose(f);
		}

		count++;
	}

	fclose(manifest);
	free(buffer);

	sysLogPrintf(LOG_NOTE, "texpack: wrote raw data for %d of %d textures to %s",
			count, NUM_TEXTURES, dumpDir);

	exit(0);
}

PD_CONSTRUCTOR static void texpackConfigInit(void)
{
	configRegisterInt("Mod.LoadTextures", &loadTextures, 0, 1);
	configRegisterInt("Mod.DumpTextures", &dumpTextures, 0, 1);
	configRegisterInt("Mod.DumpTextureData", &dumpTextureData, 0, 1);
}

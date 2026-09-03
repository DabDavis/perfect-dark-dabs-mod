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
#include "bss.h"
#include "constants.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "platform.h"
#include "config.h"
#include "archive.h"
#include "fs.h"
#include "input.h"
#include "pngread.h"
#include "pngwrite.h"
#include "system.h"
#include "texpack.h"
#include "video.h"
#include "versioninfo.h"

#define TEXPACK_DUMP_DIR_NAME "texturedump"

// Matches the "textures" directory modTextureLoad() already reads its %04x.bin
// replacements from, so one pack directory holds both kinds.
#define TEXPACK_DIR_NAME "textures"

// Where packs are looked for, and where an archive is unpacked to: its own
// folder beside the executable, the way screenshots and recordings get one -
// see fsChooseOutputDir(). The cache name starts with a dot so the scan skips
// it when listing packs.
#define TEXPACK_PACKS_DIR "texture-packs"
#define TEXPACK_CACHE_DIR ".cache"
#define TEXPACK_MAXPACKS 32
#define TEXPACK_NAMELEN 48
#define TEXPACK_KEYNAME_LEN 32

// Written into an unpacked archive once it is complete, so an extraction that
// was interrupted is done again rather than half used.
#define TEXPACK_DONE_FILE ".extracted"

// Slots in the checksum index. A power of two comfortably over the number of
// textures, so the table stays half empty and probes stay short.
#define TEXPACK_RICE_SLOTS 8192

// Enough for the largest texture in the ROM. The checksum is taken over a
// swizzled copy, because the original has to stay as the renderer wants it.
#define TEXPACK_RICE_SCRATCH (128 * 1024)

// Pack files that no texture number claimed, kept to be matched against the
// texels of whatever gets drawn. A power of two well clear of how many there
// usually are - 259 of 1509 for the pack this was built against.
#define TEXPACK_UNPLACED_SLOTS 2048

// Which image a pack file holds, best first. A Rice pack may ship several for
// one texture, and only some of them are the whole picture.
#define TEXPACK_KIND_NATIVE 0 // <texnum>.png, ours
#define TEXPACK_KIND_ALL    1 // _all, _allciByRGBA, _ciByRGBA, _ci
#define TEXPACK_KIND_RGB    2 // _rgb, whose alpha is a separate _a file
#define TEXPACK_KIND_NONE   127

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

struct texpackricecrc {
	u32 crc;
	s32 texturenum; // -1 in an empty slot
};

static struct texpackricecrc *riceCrcs;
static s32 riceIndexState; // 0 = not built, 1 = built, -1 = gave up

struct texpackunplaced {
	u32 crc;
	char *path;
};

static struct texpackunplaced *unplaced;
static s32 numUnplaced;
static s32 numTexelMatched; // of those, how many have actually turned up
static u8 *riceScratch;

struct texpackpack {
	char name[TEXPACK_NAMELEN];
	char path[FS_MAXPATH + 1];
	s32 isArchive;
};

static struct texpackpack packs[TEXPACK_MAXPACKS];
static s32 numPacks;
static s32 packsListed;
static char packName[TEXPACK_NAMELEN];  // the selected pack, empty for none

static char dumpKeyName[TEXPACK_KEYNAME_LEN] = "F7";
static char toggleKeyName[TEXPACK_KEYNAME_LEN] = "F8";
static s32 dumpKeyVk = -1;    // -1 until the name has been looked up
static s32 toggleKeyVk = -1;

static s32 loadTextures = 1;
static char **replacePaths;   // one per texture number, NULL where there is none
static char **replaceAlphaPaths; // the _a half of a Rice pack's split images
static u8 *replaceKinds;      // what kind of file replacePaths[i] is
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
 * The N64's every-other-row word swap, applied to a copy.
 *
 * texSwizzle() is a stub on PC - "The N64 GPU wants swizzled textures, we
 * don't" - so the port's texture data is in a shape no emulator ever sees. A
 * Rice checksum was taken over the swizzled form, so it has to be put back
 * before one can be reproduced. Only the pixel size matters here, which is why
 * this keys on siz rather than the full texture format.
 */
static void texpackSwizzle(u8 *data, s32 width, s32 height, s32 siz, u32 len)
{
	// Words per row, matching texSwizzleInternal()'s padding for each depth.
	const s32 wordsPerRow = siz == G_IM_SIZ_32b ? ((width + 3) & 0xffc)
			: siz == G_IM_SIZ_16b ? (((width + 3) & 0xffc) >> 1)
			: siz == G_IM_SIZ_8b ? (((width + 7) & 0xff8) >> 2)
			: (((width + 0xf) & 0xff0) >> 3);
	const s32 step = siz == G_IM_SIZ_32b ? 4 : 2;
	s32 y;

	for (y = 1; y < height; y += 2) {
		u32 *row = (u32 *)data + (u32)y * wordsPerRow;
		s32 x;

		for (x = 0; x + step <= wordsPerRow; x += step) {
			s32 k;

			for (k = 0; k < step / 2; k++) {
				u32 *a = row + x + k;
				u32 *b = row + x + k + step / 2;
				u32 tmp;

				if ((u8 *)(b + 1) > data + len) {
					break;
				}

				tmp = *a;
				*a = *b;
				*b = tmp;
			}
		}
	}
}

static u32 texpackReadBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/**
 * Rice's CRC32, as GlideHQ computes it - see TxUtil::RiceCRC32() in Project64.
 *
 * Rows are walked forwards while the counter mixed into each runs backwards,
 * and each row is read from its end towards its start in 32-bit steps. Words
 * are read big-endian, the order they have in the ROM and the order an
 * emulator's RDRAM presents them in. wordHash deliberately survives between
 * rows: a row too narrow for a single word contributes the previous row's
 * value, and packs were built against that.
 */
static u32 texpackRiceCrc(const u8 *data, u32 len, s32 width, s32 height, s32 siz, s32 stride)
{
	const s32 bytesPerWidth = ((width << siz) + 1) >> 1;
	u32 crc = 0;
	u32 wordHash = 0;
	s32 row = 0;
	s32 counter;

	for (counter = height - 1; counter >= 0; counter--) {
		s32 pos;

		for (pos = bytesPerWidth - 4; pos >= 0; pos -= 4) {
			if ((u32)(row + pos + 4) > len) {
				return 0;
			}

			wordHash = (u32)pos ^ texpackReadBE32(data + row + pos);
			crc = wordHash + ((crc << 4) | (crc >> 28));
		}

		crc += (u32)counter ^ wordHash;
		row += stride;
	}

	return crc;
}

static void texpackRiceInsert(u32 crc, s32 texturenum)
{
	u32 slot = crc & (TEXPACK_RICE_SLOTS - 1);
	u32 i;

	for (i = 0; i < TEXPACK_RICE_SLOTS; i++, slot = (slot + 1) & (TEXPACK_RICE_SLOTS - 1)) {
		if (riceCrcs[slot].texturenum < 0) {
			riceCrcs[slot].crc = crc;
			riceCrcs[slot].texturenum = texturenum;
			return;
		}

		if (riceCrcs[slot].crc == crc) {
			// Two textures with identical bytes. Either draws the same, so the
			// first found is as good an answer as the second.
			return;
		}
	}
}

/**
 * Checksums every texture in the ROM, so a pack named after those checksums can
 * be matched to texture numbers without being converted first.
 *
 * Costs well under a second - the whole table, decompressed, is about that -
 * and only runs when a pack with such names is actually installed.
 */
static void texpackBuildRiceIndex(void)
{
	struct texcacheitem savedItems[ARRAYCOUNT(g_TexCacheItems)];
	const s32 savedCount = g_TexCacheCount;
	struct texpool pool;
	u8 *buffer;
	u8 *scratch;
	s32 count = 0;
	s32 n;

	riceIndexState = -1;

	if (!g_Textures) {
		return;
	}

	riceCrcs = malloc(TEXPACK_RICE_SLOTS * sizeof(struct texpackricecrc));
	buffer = malloc(TEXPACK_RICE_SCRATCH);
	scratch = malloc(TEXPACK_RICE_SCRATCH);

	if (!riceCrcs || !buffer || !scratch) {
		sysLogPrintf(LOG_ERROR, "texpack: could not alloc the checksum index");
		free(riceCrcs);
		free(buffer);
		free(scratch);
		riceCrcs = NULL;
		return;
	}

	// -1 in every texturenum, which is what marks a slot empty.
	memset(riceCrcs, 0xff, TEXPACK_RICE_SLOTS * sizeof(struct texpackricecrc));

	// Loading a texture appends to the LOD size cache, which is a ring of 150
	// and is read for textures the game currently has on screen. Walking the
	// whole table would push every real entry out of it, so it is put back.
	memcpy(savedItems, g_TexCacheItems, sizeof(savedItems));

	for (n = 0; n < NUM_TEXTURES; n++) {
		struct tex *tex;
		s32 width;
		s32 height;
		s32 stride;
		s32 size;

		texInitPool(&pool, buffer, TEXPACK_RICE_SCRATCH);
		texLoadFromTextureNum(n, &pool);

		tex = texFindInPool(n, &pool);

		if (!tex || !tex->data) {
			continue;
		}

		// Both of these are named for bytes and both return 64-bit words.
		stride = texGetLineSizeInBytes(tex, 0) * 8;
		size = texGetSizeInBytes(tex, 0) * 8;
		width = texGetWidthAtLod(tex, 0);
		height = texGetHeightAtLod(tex, 0);

		// A 32-bit texel is split across the two halves of TMEM, so the tile
		// line counts half of it and the data is twice as long as the line
		// suggests. gfx_pc does the same doubling when it loads one.
		if (tex->depth == G_IM_SIZ_32b) {
			stride *= 2;
			size *= 2;
		}

		if (size <= 0 || size > TEXPACK_RICE_SCRATCH || width <= 0 || height <= 0 || stride <= 0) {
			continue;
		}

		memcpy(scratch, tex->data, size);
		texpackSwizzle(scratch, width, height, tex->depth, size);
		texpackRiceInsert(texpackRiceCrc(scratch, size, width, height, tex->depth, stride), n);
		count++;
	}

	memcpy(g_TexCacheItems, savedItems, sizeof(savedItems));
	g_TexCacheCount = savedCount;

	free(buffer);
	free(scratch);

	riceIndexState = 1;

	sysLogPrintf(LOG_NOTE, "texpack: checksummed %d textures to match this pack", count);
}

static s32 texpackRiceLookup(u32 crc)
{
	u32 slot;
	u32 i;

	if (!riceIndexState) {
		texpackBuildRiceIndex();
	}

	if (riceIndexState < 0) {
		return -1;
	}

	slot = crc & (TEXPACK_RICE_SLOTS - 1);

	for (i = 0; i < TEXPACK_RICE_SLOTS; i++, slot = (slot + 1) & (TEXPACK_RICE_SLOTS - 1)) {
		if (riceCrcs[slot].texturenum < 0) {
			return -1;
		}

		if (riceCrcs[slot].crc == crc) {
			return riceCrcs[slot].texturenum;
		}
	}

	return -1;
}

/**
 * Reads exactly n hex digits, and only that many.
 */
static s32 texpackHex(const char *s, s32 n, u32 *out)
{
	u32 value = 0;
	s32 i;

	for (i = 0; i < n; i++) {
		const char c = s[i];

		if (c >= '0' && c <= '9') {
			value = (value << 4) | (u32)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			value = (value << 4) | (u32)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			value = (value << 4) | (u32)(c - 'A' + 10);
		} else {
			return 0;
		}
	}

	*out = value;

	return 1;
}

/**
 * Pulls the checksum and the image kind out of a Rice pack's filename:
 *
 *     Perfect Dark#1135D097#3#1_all.png
 *                  ^crc     ^f ^siz ^kind
 *
 * The ROM name in front is ignored - it is whatever the pack's author had, and
 * the checksum already says which texture this is.
 */
static s32 texpackParseRiceName(const char *name, u32 *crc, s32 *kind, s32 *isAlpha)
{
	const char *p;

	for (p = strchr(name, '#'); p; p = strchr(p + 1, '#')) {
		u32 fmt;
		u32 siz;
		u32 palcrc;
		const char *rest;

		if (!texpackHex(p + 1, 8, crc) || p[9] != '#'
				|| !texpackHex(p + 10, 1, &fmt) || p[11] != '#'
				|| !texpackHex(p + 12, 1, &siz)) {
			continue;
		}

		rest = p + 13;

		if (*rest == '#' && texpackHex(rest + 1, 8, &palcrc)) {
			rest += 9;
		}

		if (*rest != '_') {
			continue;
		}

		rest++;
		*isAlpha = 0;

		if (!strcasecmp(rest, "all.png") || !strcasecmp(rest, "allciByRGBA.png")
				|| !strcasecmp(rest, "ciByRGBA.png") || !strcasecmp(rest, "ci.png")) {
			*kind = TEXPACK_KIND_ALL;
			return 1;
		}

		if (!strcasecmp(rest, "rgb.png")) {
			*kind = TEXPACK_KIND_RGB;
			return 1;
		}

		if (!strcasecmp(rest, "a.png")) {
			*kind = TEXPACK_KIND_RGB;
			*isAlpha = 1;
			return 1;
		}
	}

	return 0;
}

/**
 * Records one candidate filename against its texture number.
 *
 * The name is <texnum>.png, four lowercase hex digits, optionally followed by
 * an underscore and anything at all - which is what the dumper writes
 * (0a9a_i8.png), so a dump can be edited and dropped back in without renaming.
 */
/**
 * The texture number a file is for, by our own naming: four hex digits,
 * optionally followed by an underscore and anything at all - which is what the
 * dumper writes (0a9a_i8.png), so a dump can be edited and dropped back in
 * without renaming. Returns -1 if the name is not one of ours.
 */
static s32 texpackParseNativeName(const char *name)
{
	const char *rest;
	u32 texturenum;

	if (strlen(name) < 8) { // 4 digits + ".png"
		return -1;
	}

	if (!texpackHex(name, 4, &texturenum) || texturenum >= NUM_TEXTURES) {
		return -1;
	}

	rest = name + 4;

	if (*rest == '_') {
		rest = strrchr(rest, '.');

		if (!rest) {
			return -1;
		}
	}

	if (strcasecmp(rest, ".png")) {
		return -1;
	}

	return (s32)texturenum;
}

static void texpackAddUnplaced(u32 crc, char *path)
{
	u32 slot;
	u32 i;

	if (!path) {
		return;
	}

	if (!unplaced) {
		unplaced = calloc(TEXPACK_UNPLACED_SLOTS, sizeof(struct texpackunplaced));

		if (!unplaced) {
			free(path);
			return;
		}
	}

	slot = crc & (TEXPACK_UNPLACED_SLOTS - 1);

	for (i = 0; i < TEXPACK_UNPLACED_SLOTS; i++, slot = (slot + 1) & (TEXPACK_UNPLACED_SLOTS - 1)) {
		if (!unplaced[slot].path) {
			unplaced[slot].crc = crc;
			unplaced[slot].path = path;
			numUnplaced++;
			return;
		}

		if (unplaced[slot].crc == crc) {
			// The same texture under another kind of file. The first found is
			// the better one, because kinds are looked at in that order.
			free(path);
			return;
		}
	}

	free(path);
}

static const char *texpackFindUnplaced(u32 crc)
{
	u32 slot;
	u32 i;

	if (!unplaced) {
		return NULL;
	}

	slot = crc & (TEXPACK_UNPLACED_SLOTS - 1);

	for (i = 0; i < TEXPACK_UNPLACED_SLOTS; i++, slot = (slot + 1) & (TEXPACK_UNPLACED_SLOTS - 1)) {
		if (!unplaced[slot].path) {
			return NULL;
		}

		if (unplaced[slot].crc == crc) {
			return unplaced[slot].path;
		}
	}

	return NULL;
}

static char *texpackJoin(const char *dir, const char *name)
{
	const u32 len = strlen(dir) + strlen(name) + 2;
	char *path = malloc(len);

	if (path) {
		snprintf(path, len, "%s/%s", dir, name);
	}

	return path;
}

/**
 * Records one candidate filename against its texture number.
 *
 * Two namings are accepted. Ours is <texnum>.png and costs nothing to resolve.
 * A pack built for an emulator names its files after a checksum of the original
 * texels instead, because that is all an emulator has to go on; those are
 * resolved through the index above, which is built the first time one is seen.
 */
// How deep a pack's own folders are followed. A Rice pack sorts its images into
// a folder per level, and the ones in the wild are two or three deep.
#define TEXPACK_MAXDEPTH 8

struct texpackscan {
	const char *dir;
	s32 depth;
};

static void texpackScanPathAt(const char *path, s32 depth);

static void texpackIndexFile(const char *name, void *arg)
{
	const struct texpackscan *scan = arg;
	const char *dir = scan->dir;
	s32 texturenum = texpackParseNativeName(name);
	s32 kind = TEXPACK_KIND_NATIVE;
	s32 isAlpha = 0;
	char *path;

	if (texturenum < 0) {
		u32 crc;

		if (!texpackParseRiceName(name, &crc, &kind, &isAlpha)) {
			// Not an image this understands - but a pack sorts its files into
			// folders, and the only way to tell one from a file here is to try
			// opening it as one.
			if (scan->depth < TEXPACK_MAXDEPTH) {
				char sub[FS_MAXPATH + 1];
				snprintf(sub, sizeof(sub), "%s/%s", dir, name);
				texpackScanPathAt(sub, scan->depth + 1);
			}

			return;
		}

		texturenum = texpackRiceLookup(crc);

		if (texturenum < 0) {
			// No texture in the table has these texels. It may still be drawn:
			// a model's textures live inside the model file and never get a
			// number. Keep it to be matched against what is drawn instead.
			if (!isAlpha) {
				texpackAddUnplaced(crc, texpackJoin(dir, name));
			}

			return;
		}
	}

	path = texpackJoin(dir, name);

	if (!path) {
		return;
	}

	if (isAlpha) {
		// The alpha half of a split image. Kept aside; it is only used if the
		// colour half is what ends up chosen.
		free(replaceAlphaPaths[texturenum]);
		replaceAlphaPaths[texturenum] = path;
		return;
	}

	// A whole image beats a colour-only one, and a later directory outranks an
	// earlier one - the scan walks from the base directory up through the mod
	// directories in reverse priority order, so whatever is found last is what
	// the file search would have picked.
	if (replacePaths[texturenum]) {
		if (kind > replaceKinds[texturenum]) {
			free(path);
			return;
		}

		free(replacePaths[texturenum]);
	} else {
		numReplacements++;
	}

	replacePaths[texturenum] = path;
	replaceKinds[texturenum] = (u8)kind;
}

static void texpackFreeIndex(void)
{
	s32 i;

	for (i = 0; replacePaths && i < NUM_TEXTURES; i++) {
		free(replacePaths[i]);
	}

	for (i = 0; replaceAlphaPaths && i < NUM_TEXTURES; i++) {
		free(replaceAlphaPaths[i]);
	}

	for (i = 0; unplaced && i < TEXPACK_UNPLACED_SLOTS; i++) {
		free(unplaced[i].path);
	}

	free(replacePaths);
	free(replaceAlphaPaths);
	free(replaceKinds);
	free(unplaced);
	free(riceScratch);

	replacePaths = NULL;
	replaceAlphaPaths = NULL;
	replaceKinds = NULL;
	unplaced = NULL;
	riceScratch = NULL;
	numUnplaced = 0;
	numTexelMatched = 0;
}

/**
 * Indexes one directory of images. path is absolute: fsScanDir() resolves a
 * relative one through the mod search order, which would collapse every mod
 * directory onto whichever one wins.
 */
static void texpackScanPathAt(const char *path, s32 depth)
{
	struct texpackscan scan = { path, depth };

	fsScanDir(path, texpackIndexFile, &scan);
}

static void texpackScanPath(const char *path)
{
	texpackScanPathAt(path, 0);
}

static void texpackScanDir(const char *dir)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/" TEXPACK_DIR_NAME, dir);
	texpackScanPath(path);
}

static void texpackListEntry(const char *name, void *arg)
{
	const char *dir = arg;
	struct texpackpack *pack;
	const char *dot;
	s32 i;
	u32 len;

	if (name[0] == '.' || numPacks >= TEXPACK_MAXPACKS) {
		return;
	}

	pack = &packs[numPacks];
	pack->isArchive = archiveIsSupported(name);

	// An archive is listed under its name without the extension, so that
	// mypack.zip and a mypack folder do not read as two different things.
	dot = pack->isArchive ? strrchr(name, '.') : NULL;
	len = dot ? (u32)(dot - name) : strlen(name);

	if (len == 0 || len >= TEXPACK_NAMELEN) {
		return;
	}

	memcpy(pack->name, name, len);
	pack->name[len] = '\0';

	for (i = 0; i < numPacks; i++) {
		if (!strcasecmp(packs[i].name, pack->name)) {
			// Already found in a directory searched earlier, which outranks
			// this one the same way the file search order does.
			return;
		}
	}

	snprintf(pack->path, sizeof(pack->path), "%s/%s", dir, name);
	numPacks++;
}

/**
 * The texture-packs folder, created on the first look.
 *
 * Beside the executable where that can be written and in the save directory
 * where it cannot, which is where a player would expect to drop a pack and
 * where screenshots and recordings already go. Returns NULL if neither works.
 */
static const char *texpackPacksDir(void)
{
	static char dir[FS_MAXPATH + 1];
	static s32 state; // 0 = not looked at yet, 1 = ready, -1 = gave up
	char rel[FS_MAXPATH + 1];

	if (state) {
		return state > 0 ? dir : NULL;
	}

	state = -1;

	if (fsChooseOutputDir(TEXPACK_PACKS_DIR, rel, sizeof(rel)) != 0) {
		sysLogPrintf(LOG_ERROR, "texpack: nowhere to put %s that can be written",
				TEXPACK_PACKS_DIR);
		return NULL;
	}

	strncpy(dir, fsFullPath(rel), sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	state = 1;

	return dir;
}

void texpackRefreshPacks(void)
{
	const char *dir = texpackPacksDir();

	numPacks = 0;
	packsListed = 1;

	if (dir) {
		fsScanDir(dir, texpackListEntry, (void *)dir);
	}
}

s32 texpackGetNumPacks(void)
{
	if (!packsListed) {
		texpackRefreshPacks();
	}

	return numPacks;
}

const char *texpackGetPackName(s32 index)
{
	if (index < 0 || index >= texpackGetNumPacks()) {
		return "None";
	}

	return packs[index].name;
}

s32 texpackGetSelectedPack(void)
{
	s32 i;

	if (!packName[0]) {
		return -1;
	}

	for (i = 0; i < texpackGetNumPacks(); i++) {
		if (!strcasecmp(packs[i].name, packName)) {
			return i;
		}
	}

	return -1;
}

/**
 * Unpacks an archive pack into texturepacks/.cache/<name>, once.
 *
 * Returns the directory to read the pack from, which for a folder pack is the
 * folder itself, or NULL if there is nothing usable.
 */
static const char *texpackResolveSelected(void)
{
	static char dir[FS_MAXPATH + 1];
	char marker[FS_MAXPATH + 1];
	const struct texpackpack *pack;
	const s32 index = texpackGetSelectedPack();
	s32 count;

	if (index < 0) {
		return NULL;
	}

	pack = &packs[index];

	if (!pack->isArchive) {
		return pack->path;
	}

	{
		const char *root = texpackPacksDir();

		if (!root) {
			return NULL;
		}

		snprintf(dir, sizeof(dir), "%s/" TEXPACK_CACHE_DIR, root);
		fsCreateDir(dir);
		snprintf(dir, sizeof(dir), "%s/" TEXPACK_CACHE_DIR "/%s", root, pack->name);
	}

	snprintf(marker, sizeof(marker), "%s/" TEXPACK_DONE_FILE, dir);

	if (fsFileSize(marker) >= 0) {
		return dir;
	}

	sysLogPrintf(LOG_NOTE, "texpack: unpacking %s, this happens once", pack->name);

	count = archiveExtract(pack->path, dir);

	if (count <= 0) {
		sysLogPrintf(LOG_ERROR, "texpack: nothing came out of %s", pack->path);
		return NULL;
	}

	{
		FILE *f = fopen(marker, "wb");

		if (f) {
			fclose(f);
		}
	}

	sysLogPrintf(LOG_NOTE, "texpack: unpacked %d files from %s", count, pack->name);

	return dir;
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
	replaceAlphaPaths = calloc(NUM_TEXTURES, sizeof(char *));
	replaceKinds = calloc(NUM_TEXTURES, sizeof(u8));

	if (!replacePaths || !replaceAlphaPaths || !replaceKinds) {
		sysLogPrintf(LOG_ERROR, "texpack: could not alloc the replacement index");
		texpackFreeIndex();
		return;
	}

	texpackScanDir(fsFullPath("$B"));

	for (i = fsGetNumModDirs() - 1; i >= 0; i--) {
		texpackScanDir(fsGetModDirAt(i));
	}

	{
		// Last, so the pack the player chose in the menu wins over any
		// textures/ a mod happens to ship. Both layouts are accepted: images
		// at the root of the pack, or under a textures/ inside it.
		const char *dir = texpackResolveSelected();

		if (dir) {
			texpackScanPath(dir);
			texpackScanDir(dir);
		}
	}

	if (numReplacements) {
		sysLogPrintf(LOG_NOTE, "texpack: %d replacement textures", numReplacements);

		if (numUnplaced) {
			sysLogPrintf(LOG_NOTE, "texpack: %d more are matched by their texels when drawn"
					" - models keep their textures to themselves", numUnplaced);
		}
	} else {
		if (packName[0]) {
			sysLogPrintf(LOG_WARNING, "texpack: nothing in %s matches a texture in this ROM",
					packName);
		}

		texpackFreeIndex();
	}
}

s32 texpackHaveReplacements(void)
{
	if (!replaceScanned) {
		texpackScan();
	}

	return replacePaths != NULL;
}

/**
 * Decodes one replacement image into the row order the game's texture data uses.
 *
 * Which way that is depends on where the file came from. Our own dumps are
 * written the right way up, because someone opens them in an image editor - see
 * the note over the dump - so they get flipped back here. A pack built for an
 * emulator is not: GlideHQ writes textures out in raw N64 row order, which is
 * upside down on screen, and its artists have always edited them that way. Such
 * a file is already in the order wanted and must be left alone.
 */
static u8 *texpackLoadImage(const char *path, s32 flip, s32 *outWidth, s32 *outHeight)
{
	s32 width;
	s32 height;
	s32 y;
	u8 *rgba = pngRead(path, &width, &height);

	if (!rgba) {
		return NULL;
	}

	for (y = 0; flip && y < height / 2; y++) {
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

s32 texpackGetNumUnplaced(void)
{
	return numUnplaced;
}

s32 texpackGetNumTexelMatched(void)
{
	return numTexelMatched;
}

s32 texpackHaveUnplacedFiles(void)
{
	return texpackHaveReplacements() && numUnplaced > 0;
}

u8 *texpackLoadReplacementForTexels(const u8 *data, u32 size, s32 width, s32 height,
		s32 siz, s32 stride, s32 *outWidth, s32 *outHeight)
{
	const char *path;
	u32 crc;

	if (!data || !size || size > TEXPACK_RICE_SCRATCH
			|| width <= 0 || height <= 0 || stride <= 0) {
		return NULL;
	}

	if (!riceScratch) {
		riceScratch = malloc(TEXPACK_RICE_SCRATCH);

		if (!riceScratch) {
			return NULL;
		}
	}

	// The checksum is over the texels as the N64 wants them, which is not how
	// the port keeps them - so a swizzled copy is made to hash.
	memcpy(riceScratch, data, size);
	texpackSwizzle(riceScratch, width, height, siz, size);
	crc = texpackRiceCrc(riceScratch, size, width, height, siz, stride);

	path = texpackFindUnplaced(crc);

	if (!path) {
		return NULL;
	}

	numTexelMatched++;

	return texpackLoadImage(path, 0, outWidth, outHeight);
}

u8 *texpackLoadReplacement(const void *data, s32 *outWidth, s32 *outHeight)
{
	s32 texturenum;
	u8 *rgba;

	if (!texpackHaveReplacements()) {
		return NULL;
	}

	texturenum = texpackGetTextureNum(data);

	if (texturenum < 0 || texturenum >= NUM_TEXTURES || !replacePaths[texturenum]) {
		return NULL;
	}

	// Only our own naming means the file was written the right way up.
	rgba = texpackLoadImage(replacePaths[texturenum],
			replaceKinds[texturenum] == TEXPACK_KIND_NATIVE, outWidth, outHeight);

	if (!rgba) {
		// Whatever is wrong with the file will not fix itself, and retrying on
		// every cache miss would log it forever. Drop it and draw the original.
		free(replacePaths[texturenum]);
		replacePaths[texturenum] = NULL;
		numReplacements--;
		return NULL;
	}

	if (replaceAlphaPaths[texturenum] && replaceKinds[texturenum] == TEXPACK_KIND_RGB) {
		// A Rice pack may split a texture into colour and alpha images. The
		// colour one is opaque on its own, so the alpha has to be pasted back
		// over it; only its red channel carries anything. Both halves come out
		// of texpackLoadImage() the same way up, so they line up.
		s32 alphaWidth;
		s32 alphaHeight;
		u8 *alpha = texpackLoadImage(replaceAlphaPaths[texturenum], 0, &alphaWidth, &alphaHeight);

		if (alpha) {
			if (alphaWidth == *outWidth && alphaHeight == *outHeight) {
				s32 i;

				for (i = 0; i < alphaWidth * alphaHeight; i++) {
					rgba[i * 4 + 3] = alpha[i * 4];
				}
			} else {
				sysLogPrintf(LOG_WARNING, "texpack: %s is %dx%d but its alpha is %dx%d",
						replacePaths[texturenum], *outWidth, *outHeight, alphaWidth, alphaHeight);
			}

			free(alpha);
		}
	}

	return rgba;
}

void texpackFreeReplacement(u8 *rgba)
{
	free(rgba);
}

/**
 * Drops the index and the renderer's texture cache so the pack is read again.
 *
 * The id registry is deliberately left alone: it maps pool addresses to texture
 * numbers and nothing about those has changed, and the entries are only put
 * back when the game loads a texture, which it has no reason to do again.
 */
void texpackReload(void)
{
	texpackFreeIndex();

	numReplacements = 0;
	replaceScanned = 0;
	packsListed = 0;

	videoResetTextureCache();

	sysLogPrintf(LOG_NOTE, "texpack: reloaded %s",
			packName[0] ? packName : "textures (no pack selected)");
}

void texpackSetSelectedPack(s32 index)
{
	const char *name = (index >= 0 && index < texpackGetNumPacks()) ? packs[index].name : "";

	strncpy(packName, name, sizeof(packName) - 1);
	packName[sizeof(packName) - 1] = '\0';

	texpackReload();
}

s32 texpackLoadEnabled(void)
{
	return loadTextures;
}

void texpackSetLoadEnabled(s32 enabled)
{
	const s32 want = enabled ? 1 : 0;

	if (want != loadTextures) {
		loadTextures = want;
		texpackReload();
		sysLogPrintf(LOG_NOTE, "texpack: texture packs %s", want ? "on" : "off");
	}
}

s32 texpackGetNumReplacements(void)
{
	if (!replaceScanned) {
		texpackScan();
	}

	return numReplacements;
}

s32 texpackGetDumpEnabled(void)
{
	return dumpTextures;
}

void texpackSetDumpEnabled(s32 enabled)
{
	dumpTextures = enabled ? 1 : 0;

	if (dumpTextures) {
		// Everything already uploaded would otherwise never come back through
		// the importer, so a dump turned on mid-level would write out only
		// whatever happened to be loaded next.
		videoResetTextureCache();
	}

	sysLogPrintf(LOG_NOTE, "texpack: dumping %s", dumpTextures ? "on" : "off");
}

/**
 * Resolves a key name to a scancode on first use, the way the screenshot bind
 * does: inputInit() is what fills the table it is looked up in, so it cannot be
 * done when the config is read.
 */
static s32 texpackResolveKey(const char *name, s32 *vk)
{
	if (*vk < 0) {
		if (!name[0] || !strcmp(name, "NONE")) {
			*vk = 0;
		} else {
			*vk = inputGetKeyByName(name);

			if (*vk < 0) {
				*vk = 0;
			}
		}
	}

	return *vk;
}

static void texpackSetKey(s32 vk, char *name, u32 nameSize, s32 *keyVk)
{
	if (vk <= 0 || vk >= VK_TOTAL_COUNT) {
		name[0] = '\0';
		*keyVk = 0;
		return;
	}

	strncpy(name, inputGetKeyName(vk), nameSize - 1);
	name[nameSize - 1] = '\0';
	*keyVk = vk;
}

s32 texpackDumpGetKey(void)
{
	return texpackResolveKey(dumpKeyName, &dumpKeyVk);
}

void texpackDumpSetKey(s32 vk)
{
	texpackSetKey(vk, dumpKeyName, sizeof(dumpKeyName), &dumpKeyVk);
}

s32 texpackToggleGetKey(void)
{
	return texpackResolveKey(toggleKeyName, &toggleKeyVk);
}

void texpackToggleSetKey(s32 vk)
{
	texpackSetKey(vk, toggleKeyName, sizeof(toggleKeyName), &toggleKeyVk);
}

void texpackTick(void)
{
	const s32 dumpVk = texpackDumpGetKey();
	const s32 toggleVk = texpackToggleGetKey();

	// inputKeyJustPressed() consumes the edge, so each key wants asking about
	// exactly once a frame and only when it is actually bound.
	if (dumpVk > 0 && inputKeyJustPressed(dumpVk)) {
		texpackSetDumpEnabled(!dumpTextures);
	}

	// Off and on again rather than a reload, because turning it off is what
	// you want when comparing against the original - and switching it back on
	// re-reads the pack anyway, so an edited image still shows up.
	if (toggleVk > 0 && inputKeyJustPressed(toggleVk)) {
		texpackSetLoadEnabled(!loadTextures);
	}
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

	fprintf(manifest, "texnum,fmt,siz,tilewidth,tileheight,linesize,size,palidx,numlods,hasloddata,lutmode,numcolours\n");

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
		// discover it. A 32-bit texel is split across the two halves of TMEM,
		// so both are half of what the texture really occupies.
		const s32 wide = tex->depth == G_IM_SIZ_32b ? 2 : 1;

		size = texGetSizeInBytes(tex, 0) * 8 * wide;

		if (size <= 0) {
			continue;
		}

		fprintf(manifest, "%04x,%u,%u,%d,%d,%d,%d,0,%u,%u,%u,%u\n", n, tex->gbiformat, tex->depth,
				texGetWidthAtLod(tex, 0), texGetHeightAtLod(tex, 0),
				texGetLineSizeInBytes(tex, 0) * 8 * wide, size, tex->numlods, tex->hasloddata,
				tex->lutmodeindex, tex->unk0a + 1);

		snprintf(path, sizeof(path), "%s/%04x.raw", dumpDir, n);
		f = fopen(path, "wb");

		if (f) {
			fwrite(tex->data, 1, size, f);
			fclose(f);
		}

		// A paletted texture keeps its palette in the same allocation, right
		// after the pixels of every LOD - which is what texGetDepthAndSize()
		// measures, in 16-bit units.
		if (tex->lutmodeindex) {
			s32 depth;
			s32 len;

			texGetDepthAndSize(tex, &depth, &len);

			snprintf(path, sizeof(path), "%s/%04x.pal", dumpDir, n);
			f = fopen(path, "wb");

			if (f) {
				fwrite(tex->data + len * 2, 2, tex->unk0a + 1, f);
				fclose(f);
			}
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
	configRegisterString("Mod.TexturePack", packName, sizeof(packName));
	configRegisterString("Mod.DumpTexturesKey", dumpKeyName, sizeof(dumpKeyName));
	configRegisterString("Mod.TexturePackKey", toggleKeyName, sizeof(toggleKeyName));
	configRegisterInt("Mod.DumpTextures", &dumpTextures, 0, 1);
	configRegisterInt("Mod.DumpTextureData", &dumpTextureData, 0, 1);
}

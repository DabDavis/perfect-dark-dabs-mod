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
#include <ultra64.h>
#include "constants.h"
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "pngwrite.h"
#include "system.h"
#include "texpack.h"
#include "versioninfo.h"

#define TEXPACK_DUMP_DIR_NAME "texturedump"

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

static s32 dumpTextures = 0;
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

void texpackDumpTexture(const void *data, const u8 *rgba32, u32 width, u32 height, u32 fmt, u32 siz)
{
	char path[FS_MAXPATH + 1];
	s32 texturenum;

	if (!dumpTextures || !rgba32 || width == 0 || height == 0) {
		return;
	}

	texturenum = texpackGetTextureNum(data);

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
}

PD_CONSTRUCTOR static void texpackConfigInit(void)
{
	configRegisterInt("Mod.DumpTextures", &dumpTextures, 0, 1);
}

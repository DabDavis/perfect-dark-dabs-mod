#include <ultra64.h>
#include "constants.h"
#include "game/prop.h"
#include "game/game_1531a0.h"
#include "game/bg.h"
#include "bss.h"
#include "lib/dma.h"
#include "lib/memp.h"
#include "lib/mtx.h"
#include "lib/anim.h"
#include "lib/lib_2f490.h"
#include "lib/libc/ll.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include <string.h>
#include "mod.h"
#ifndef PLATFORM_N64
#include "system.h"
#endif
#endif

#define ANIM_HEADER_CACHE_SIZE 40
#define ANIM_FRAME_CACHE_SIZE  32

u8 *g_AnimFrameByteSlots;
u8 **g_AnimFrameBytes;
s16 *g_AnimFrameAnimNums;
s16 *g_AnimFrameFrameNums;
u8 *g_AnimFrameBirths;
u8 *g_AnimHeaderByteSlots;
u8 **g_AnimHeaderBytes;
s16 *g_AnimHeaderAnimNums;
s32 *g_AnimHeaderBirths;
s16 g_NumRomAnimations;
struct animtableentry *g_RomAnims;

u32 g_NextAnimFrameIndex = 0;
s32 g_NextAnimHeaderIndex = 0;
s16 g_NumAnimations = 0;
struct animtableentry *g_Anims = NULL;
u8 *g_AnimToHeaderSlot = NULL;
s16 *var8005f014 = NULL;
s32 g_AnimMaxBytesPerFrame = 176;
s32 g_AnimMaxHeaderLength = 608;
bool g_AnimHostEnabled = false;
u8 *g_AnimHostSegment = NULL;

#ifndef PLATFORM_N64
u8 **g_AnimReplacements;

// Rows past the ROM's table for animations taken from a mounted mod
// (animAppendExternal()): a borrowed gun's reload is the mod's animation,
// and the mod's number for it means something else to the ROM
#define ANIM_EXTRA_CAPACITY 1024

static s32 g_AnimCapacity;
#endif

extern u8 EXT_SEG _animationsTableRomStart;
extern u8 EXT_SEG _animationsTableRomEnd;
extern u8 EXT_SEG _animationsSegmentRomStart;

void animsInit(void)
{
	s32 i;
	u32 *ptr;
	u32 tablelen = ALIGN64(REF_SEG _animationsTableRomEnd - REF_SEG _animationsTableRomStart);

	ptr = mempAlloc(tablelen, MEMPOOL_PERMANENT);

#ifdef PLATFORM_N64
	dmaExec(ptr, (romptr_t) REF_SEG _animationsTableRomStart, tablelen);
#else
	// The buffer is rounded up to 64, the table itself is not, and the copy
	// takes the table's own length. Reading the rounded length is up to 63
	// bytes past the end of the animations segment: the rest of the ROM image
	// when the segment is the ROM's, and somebody else's memory when a mod
	// replaced it with a segs/animations of its own (ASan, GE-X, 2026-09-12).
	dmaExec(ptr, (romptr_t) REF_SEG _animationsTableRomStart,
			REF_SEG _animationsTableRomEnd - REF_SEG _animationsTableRomStart);
#endif

	g_NumAnimations = g_NumRomAnimations = ptr[0];
	g_Anims = g_RomAnims = (struct animtableentry *)&ptr[1];

#ifndef PLATFORM_N64
	g_AnimCapacity = g_NumAnimations + ANIM_EXTRA_CAPACITY;

	{
		struct animtableentry *grown = mempAlloc(ALIGN64(g_AnimCapacity * sizeof(*grown)), MEMPOOL_PERMANENT);

		memcpy(grown, g_RomAnims, g_NumAnimations * sizeof(*grown));
		g_Anims = g_RomAnims = grown;
	}
#endif

	g_AnimMaxHeaderLength = 1;
	g_AnimMaxBytesPerFrame = 1;

	for (i = 0; i < g_NumAnimations; i++) {
		if (g_Anims[i].headerlen > g_AnimMaxHeaderLength) {
			g_AnimMaxHeaderLength = g_Anims[i].headerlen;
		}

		if (g_Anims[i].bytesperframe > g_AnimMaxBytesPerFrame) {
			g_AnimMaxBytesPerFrame = g_Anims[i].bytesperframe;
		}
	}

	g_AnimMaxHeaderLength = ALIGN16(g_AnimMaxHeaderLength + 34);
	g_AnimMaxBytesPerFrame = ALIGN16(g_AnimMaxBytesPerFrame + 34);

#ifdef PLATFORM_N64
	g_AnimToHeaderSlot    = mempAlloc(ALIGN64(g_NumAnimations), MEMPOOL_PERMANENT);
	var8005f014           = mempAlloc(ALIGN64(g_NumAnimations * sizeof(*var8005f014)), MEMPOOL_PERMANENT);
#else
	g_AnimToHeaderSlot    = mempAlloc(ALIGN64(g_AnimCapacity), MEMPOOL_PERMANENT);
	var8005f014           = mempAlloc(ALIGN64(g_AnimCapacity * sizeof(*var8005f014)), MEMPOOL_PERMANENT);
#endif
	g_AnimFrameByteSlots  = mempAlloc(ALIGN64(ANIM_FRAME_CACHE_SIZE * g_AnimMaxBytesPerFrame), MEMPOOL_PERMANENT);
	g_AnimFrameBytes      = mempAlloc(ALIGN64(ANIM_FRAME_CACHE_SIZE * sizeof(*g_AnimFrameBytes)), MEMPOOL_PERMANENT);
	g_AnimFrameAnimNums   = mempAlloc(ALIGN64(ANIM_FRAME_CACHE_SIZE * sizeof(*g_AnimFrameAnimNums)), MEMPOOL_PERMANENT);
	g_AnimFrameFrameNums  = mempAlloc(ALIGN64(ANIM_FRAME_CACHE_SIZE * sizeof(*g_AnimFrameFrameNums)), MEMPOOL_PERMANENT);
	g_AnimFrameBirths     = mempAlloc(ALIGN64(ANIM_FRAME_CACHE_SIZE * sizeof(*g_AnimFrameBirths)), MEMPOOL_PERMANENT);
	g_AnimHeaderByteSlots = mempAlloc(ALIGN64(ANIM_HEADER_CACHE_SIZE * g_AnimMaxHeaderLength), MEMPOOL_PERMANENT);
	g_AnimHeaderBytes     = mempAlloc(ALIGN64(ANIM_HEADER_CACHE_SIZE * sizeof(*g_AnimHeaderBytes)), MEMPOOL_PERMANENT);
	g_AnimHeaderAnimNums  = mempAlloc(ALIGN64(ANIM_HEADER_CACHE_SIZE * sizeof(*g_AnimHeaderAnimNums)), MEMPOOL_PERMANENT);
	g_AnimHeaderBirths    = mempAlloc(ALIGN64(ANIM_HEADER_CACHE_SIZE * sizeof(*g_AnimHeaderBirths)), MEMPOOL_PERMANENT);
#ifndef PLATFORM_N64
	g_AnimReplacements    = mempAlloc(ALIGN64(g_AnimCapacity * sizeof(u8 *)), MEMPOOL_PERMANENT);
	bzero(g_AnimReplacements, g_AnimCapacity * sizeof(u8 *));
#endif

	animsInitTables();

	g_AnimHostSegment = NULL;
	g_AnimHostEnabled = false;
}

#ifndef PLATFORM_N64
/**
 * Adds an animation after the ROM's, whose header and frames are data (the
 * bytes from the entry's data offset on, as the segment holds them), and
 * returns its number, or -1 when the rows are used up. data is kept.
 *
 * It is served the way a mod's external replacement is (data 0xffffffff), and
 * counts as one of the ROM's so animsReset() keeps it.
 */
s32 animAppendExternal(const struct animtableentry *entry, u8 *data)
{
	s32 num;

	if (!g_Anims || g_NumRomAnimations >= g_AnimCapacity || !data) {
		return -1;
	}

	// it is read into the ROM's slot buffers, which were sized by the ROM's
	// largest header and frame
	if (entry->headerlen > g_AnimMaxHeaderLength || entry->bytesperframe > g_AnimMaxBytesPerFrame) {
		return -1;
	}

	num = g_NumRomAnimations;

	g_RomAnims[num] = *entry;
	g_RomAnims[num].data = 0xffffffff;
	g_AnimReplacements[num] = data;
	g_AnimToHeaderSlot[num] = 0xff;
	var8005f014[num] = 0;

	g_NumRomAnimations++;

	if (g_Anims == g_RomAnims) {
		g_NumAnimations = g_NumRomAnimations;
	}

	return num;
}

/**
 * Serves animation num out of animation from's data until animRestore(num),
 * or puts num back when from is negative - so every number the game's code
 * names keeps its meaning while the frames behind it change (GE Plus plays
 * GoldenEye's own animations under Perfect Dark's numbers, gechranims.c).
 *
 * Only between stages: a model playing num has its header and frames cached
 * by number, and those are forgotten here.
 */
static struct animtableentry *g_AnimSaved;
static u8 **g_AnimSavedData;
static u8 *g_AnimIsOverridden;

static void animForget(s32 num)
{
	s32 i;

	if (g_AnimToHeaderSlot[num] != 0xff) {
		g_AnimHeaderAnimNums[g_AnimToHeaderSlot[num]] = 0;
		g_AnimHeaderBirths[g_AnimToHeaderSlot[num]] = -2;
		g_AnimToHeaderSlot[num] = 0xff;
	}

	for (i = 0; i < ANIM_FRAME_CACHE_SIZE; i++) {
		if (g_AnimFrameAnimNums[i] == num) {
			g_AnimFrameAnimNums[i] = 0;
			g_AnimFrameFrameNums[i] = 0;
			g_AnimFrameBirths[i] = 0;
		}
	}
}

s32 animOverride(s32 num, s32 from)
{
	if (!g_Anims || num <= 0 || num >= g_NumRomAnimations || from >= g_NumRomAnimations) {
		return 0;
	}

	if (!g_AnimSaved) {
		g_AnimSaved = sysMemZeroAlloc(g_AnimCapacity * sizeof(*g_AnimSaved));
		g_AnimSavedData = sysMemZeroAlloc(g_AnimCapacity * sizeof(*g_AnimSavedData));
		g_AnimIsOverridden = sysMemZeroAlloc(g_AnimCapacity);

		if (!g_AnimSaved || !g_AnimSavedData || !g_AnimIsOverridden) {
			return 0;
		}
	}

	if (from < 0) {
		if (!g_AnimIsOverridden[num]) {
			return 1;
		}

		g_RomAnims[num] = g_AnimSaved[num];
		g_AnimReplacements[num] = g_AnimSavedData[num];
		g_AnimIsOverridden[num] = 0;
	} else {
		if (!g_AnimIsOverridden[num]) {
			g_AnimSaved[num] = g_RomAnims[num];
			g_AnimSavedData[num] = g_AnimReplacements[num];
			g_AnimIsOverridden[num] = 1;
		}

		// the source is read the way an appended animation is: its bytes are
		// handed over, and a row the ROM serves from its segment keeps its offset
		g_RomAnims[num] = g_AnimIsOverridden[from] ? g_AnimSaved[from] : g_RomAnims[from];
		g_AnimReplacements[num] = g_AnimIsOverridden[from] ? g_AnimSavedData[from] : g_AnimReplacements[from];
	}

	animForget(num);

	return 1;
}

/**
 * Whether animation num as the game has it is entry with the bytes at data
 * (header then frames, as a segment holds them): a borrowed mod's animation
 * that is the same as ours plays under our number, with nothing appended.
 */
s32 animIsSame(s32 num, const struct animtableentry *entry, const u8 *data)
{
	const struct animtableentry *ours;
	u32 len;

	if (!g_Anims || num < 0 || num >= g_NumRomAnimations) {
		return 0;
	}

	ours = &g_RomAnims[num];

	if (ours->numframes != entry->numframes || ours->bytesperframe != entry->bytesperframe
			|| ours->headerlen != entry->headerlen || ours->framelen != entry->framelen
			|| ours->flags != entry->flags) {
		return 0;
	}

	len = entry->headerlen + (u32)entry->numframes * entry->bytesperframe;

	if (ours->data == 0xffffffff) {
		return g_AnimReplacements[num] && memcmp(g_AnimReplacements[num], data, len) == 0;
	}

	return memcmp((u8 *)((romptr_t) REF_SEG _animationsSegmentRomStart + ours->data), data, len) == 0;
}
#endif

void animsInitTables(void)
{
	s32 i;

	for (i = 0; i < g_NumAnimations; i++) {
		g_AnimToHeaderSlot[i] = 0xff;
		var8005f014[i] = 0;
	}

	for (i = 0; i < ANIM_FRAME_CACHE_SIZE; i++) {
		g_AnimFrameAnimNums[i] = 0;
		g_AnimFrameFrameNums[i] = 0;
		g_AnimFrameBirths[i] = 0;
	}

	for (i = 0; i < ANIM_HEADER_CACHE_SIZE; i++) {
		g_AnimHeaderAnimNums[i] = 0;
		g_AnimHeaderBirths[i] = -2;
	}
}

void animsReset(void)
{
	g_NumAnimations = g_NumRomAnimations;
	g_Anims = g_RomAnims;
	g_AnimHostEnabled = false;
}

s32 animGetNumFrames(s16 animnum)
{
	return g_Anims[animnum].numframes;
}

bool animHasFrames(s16 animnum)
{
	return animnum < g_NumAnimations && g_Anims[animnum].numframes > 0;
}

s32 animGetNumAnimations(void)
{
	return g_NumAnimations;
}

extern u8 EXT_SEG _animationsSegmentRomStart;

u8 *animDma(u8 *dst, u32 segoffset, u32 len)
{
	if (g_AnimHostEnabled) {
		bcopy(&g_AnimHostSegment[segoffset], dst, len);
		return dst;
	}

	return dmaExecWithAutoAlign(dst, (romptr_t) REF_SEG _animationsSegmentRomStart + segoffset, len);
}

/**
 * Return -1 if the given apparent frame is a repeat frame, or if not a repeat
 * frame then remap the apparent frame to a real one and return it.
 *
 * The end of the header can contain a sequence of shorts such as:
 * -1, 55, 30
 *
 * The values are iterated backwards in pairs of 2 and are terminated by -1.
 *
 * In each pair, the right value is the repeatfromframe and the left value is
 * the repeattoframe. In the above example, apparent frames 30 to 55 are
 * repeated, so the remapping looks like:
 * 29 -> 29
 * 30 -> -1
 * ...
 * 55 -> -1
 * 56 -> 30
 * 57 -> 31
 */
s32 animGetRemappedFrame(s16 animnum, s32 apparentframe)
{
	u8 *ptr = (u8 *)(g_AnimHeaderBytes[g_AnimToHeaderSlot[animnum]] + g_Anims[animnum].headerlen - 2);
	s32 realframe = apparentframe;

	while (true) {
		s16 repeatfromframe = ptr[0] << 8 | ptr[1];
		s16 repeattoframe;

		if (repeatfromframe < 0) {
			break;
		}

		repeattoframe = ptr[-2] << 8 | ptr[-1];
		ptr -= 4;

		if (repeatfromframe <= apparentframe) {
			if (repeattoframe < apparentframe) {
				realframe = realframe - repeattoframe + repeatfromframe - 1;
			} else {
				realframe = -1;
				break;
			}
		}
	}

	return realframe;
}

/**
 * Similar to the above, but with the following differences:
 * - Write the remapped frame to the frameptr pointer instead of returning it.
 * - If the apparent frame is a repeat, write the original frame rather than -1.
 * - Return true if the frame is original or false if it's a repeat.
 */
bool animRemapFrameForLoad(s16 animnum, s32 apparentframe, s32 *frameptr)
{
	u8 *ptr = (u8 *)(g_AnimHeaderBytes[g_AnimToHeaderSlot[animnum]] + g_Anims[animnum].headerlen - 2);
	s32 result = apparentframe;
	bool ret = true;

	while (true) {
		s16 repeatfromframe = ptr[0] << 8 | ptr[1];
		s16 repeattoframe;

		if (repeatfromframe < 0) {
			break;
		}

		repeattoframe = ptr[-2] << 8 | ptr[-1];
		ptr -= 4;

		if (repeatfromframe <= apparentframe) {
			if (repeattoframe < apparentframe) {
				result = result - repeattoframe + repeatfromframe - 1;
			} else {
				result = result - apparentframe + repeatfromframe;
				ret = false;
				break;
			}
		}
	}

	*frameptr = result;

	return ret;
}

/**
 * Return true if the given animation and frame should be skipped.
 *
 * Used by cutscenes.
 *
 * The skip frame numbers are stored at the tail end of the header, prior to the
 * frame repeat data. The frame numbers are stored as a list of shorts.
 * The list is terminated on the left side with a negative value.
 */
bool animIsFrameCutSkipped(s16 animnum, s32 frame)
{
	u8 *ptr = (u8 *)(g_AnimHeaderBytes[g_AnimToHeaderSlot[animnum]] + g_Anims[animnum].headerlen - 2);

	// Iterate past the repeat list
	if (g_Anims[animnum].flags & ANIMFLAG_HASREPEATFRAMES) {
		while (true) {
			s16 repeatfromframe = ptr[0] << 8 | ptr[1];

			if (repeatfromframe < 0) {
				break;
			}

			ptr -= 4;
		}

		ptr -= 2;
	}

	while (true) {
		s16 skipframe = ptr[0] << 8 | ptr[1];

		if (skipframe < 0) {
			break;
		}

		if (skipframe == frame) {
			return true;
		}

		ptr -= 2;
	}

	return false;
}

u8 animLoadFrame(s16 animnum, s32 framenum)
{
	s32 slot = -1;
	s32 i;
	s32 offset;
	s32 stack;
	s32 loadframenum = framenum;

	for (i = 0; i < ANIM_FRAME_CACHE_SIZE; i++) {
		if (g_AnimFrameAnimNums[i] == animnum && g_AnimFrameFrameNums[i] == loadframenum) {
			slot = i;
			break;
		}
	}

	if (slot >= 0) {
		g_AnimFrameBirths[slot] = 1;
	} else {
		slot = g_NextAnimFrameIndex;

		while (g_AnimFrameBirths[slot]) {
			slot = (slot + 1) % ANIM_FRAME_CACHE_SIZE;
		}

		if (g_Anims[animnum].flags & ANIMFLAG_HASREPEATFRAMES) {
			animRemapFrameForLoad(animnum, framenum, &loadframenum);
		}

		if (g_Anims[animnum].bytesperframe) {
			offset = g_Anims[animnum].bytesperframe * loadframenum + (g_Anims[animnum].data + g_Anims[animnum].headerlen);
#ifndef PLATFORM_N64
			if (g_Anims[animnum].data == 0xffffffff) {
				// load external replacement (this will fatal error if there's no data)
				if (!g_AnimReplacements[animnum]) {
					g_AnimReplacements[animnum] = modAnimationLoadData(animnum);
				}
				offset = g_Anims[animnum].bytesperframe * loadframenum + g_Anims[animnum].headerlen;
				// Into the frame slot, as the ROM's are: the bit reader measures a
				// frame by how far the header's end is above it
				// (modelasmReadFrameData(): t3ptr8 - t6ptr8), which holds for the
				// slot buffers - allocated below the header slots - and not for a
				// frame lying just past its own header in one buffer, where the
				// distance is zero or less and the reader never finishes
				g_AnimFrameBytes[slot] = &g_AnimFrameByteSlots[slot * g_AnimMaxBytesPerFrame];
				bcopy(g_AnimReplacements[animnum] + offset, g_AnimFrameBytes[slot], g_Anims[animnum].bytesperframe);
			} else
#endif
			g_AnimFrameBytes[slot] = animDma(&g_AnimFrameByteSlots[slot * g_AnimMaxBytesPerFrame], offset, g_Anims[animnum].bytesperframe);
		} else {
			g_AnimFrameBytes[slot] = &g_AnimFrameByteSlots[slot * g_AnimMaxBytesPerFrame];
		}

		g_AnimFrameAnimNums[slot] = animnum;
		g_AnimFrameFrameNums[slot] = framenum;
		g_AnimFrameBirths[slot] = 1;
		g_NextAnimFrameIndex = (slot + 1) % ANIM_FRAME_CACHE_SIZE;
	}

	return slot;
}

void animForgetFrameBirths(void)
{
	s32 i;

	for (i = 0; i < ANIM_FRAME_CACHE_SIZE; i++) {
		g_AnimFrameBirths[i] = 0;
	}
}

void animLoadHeader(s16 animnum)
{
	s32 i;

	if (g_AnimToHeaderSlot[animnum] != 0xff) {
		g_AnimHeaderBirths[g_AnimToHeaderSlot[animnum]] = g_Vars.thisframestart240;
		g_NextAnimHeaderIndex = (g_AnimToHeaderSlot[animnum] + 1) % ANIM_HEADER_CACHE_SIZE;
	} else {
		s32 tmp;
		s32 slot = g_NextAnimHeaderIndex;
		s32 stack;

		for (i = 0; i < ANIM_HEADER_CACHE_SIZE; i++) {
			if (g_AnimHeaderBirths[i] < g_AnimHeaderBirths[slot]) {
				slot = i;
			}
		}

		if (g_AnimHeaderBirths[slot]);
		if (&g_Vars && &g_Vars);

		if (g_AnimHeaderAnimNums[slot]) {
			g_AnimToHeaderSlot[g_AnimHeaderAnimNums[slot]] = 0xff;
		}

		tmp = g_Anims[animnum].headerlen;

#ifndef PLATFORM_N64
		if (g_Anims[animnum].data == 0xffffffff) {
			// load external replacement (this will fatal error if there's no data)
			if (!g_AnimReplacements[animnum]) {
				g_AnimReplacements[animnum] = modAnimationLoadData(animnum);
			}
			// into the header slot, for the same reason as a frame
			g_AnimHeaderBytes[slot] = &g_AnimHeaderByteSlots[slot * g_AnimMaxHeaderLength];
			bcopy(g_AnimReplacements[animnum], g_AnimHeaderBytes[slot], tmp);
		} else
#endif
		g_AnimHeaderBytes[slot] = animDma(&g_AnimHeaderByteSlots[slot * g_AnimMaxHeaderLength], g_Anims[animnum].data, tmp);
		g_AnimToHeaderSlot[animnum] = slot;
		g_AnimHeaderAnimNums[slot] = animnum;
		g_AnimHeaderBirths[slot] = g_Vars.thisframestart240;
		g_NextAnimHeaderIndex = (slot + 1) % ANIM_HEADER_CACHE_SIZE;
	}
}

/**
 * Read a number of bits from the given ptr and return it as an integer.
 *
 * remainingbits in the number of bits to read.
 * bitoffset is the starting bit offset relative to ptr.
 */
s32 animReadBits(u8 *ptr, u8 remainingbits, u32 bitoffset)
{
	u32 result = 0;
	u32 mask;
	u8 numbitsthisbyte;

	result *= bitoffset / 8;

	// Move ptr forward past all the bytes that should be fully skipped
	ptr += bitoffset / 8;

	// Calculate the number of bits to read in the first byte
	bitoffset %= 8;
	numbitsthisbyte = 8 - bitoffset;

	// Iterate bytes, except for the last if it's a partial read
	while (remainingbits >= numbitsthisbyte) {
		remainingbits -= numbitsthisbyte;
		mask = (1 << numbitsthisbyte) - 1;
		result |= (*ptr & mask) << remainingbits;
		ptr++;
		numbitsthisbyte = 8;
	}

	// Read bits from the final byte if it's partial read
	if (remainingbits > 0) {
		mask = (1 << remainingbits) - 1;
		result |= (*ptr >> (numbitsthisbyte - remainingbits)) & mask;
	}

	return result;
}

s32 animReadSignedShort(u8 *ptr, u8 readbitlen, s32 bitoffset)
{
	u16 result = animReadBits(ptr, readbitlen, bitoffset);

	if (readbitlen < 16 && (result & (1 << (readbitlen - 1)))) {
		result |= ((1 << (16 - readbitlen)) - 1) << readbitlen;
	}

	return result;
}

/**
 * Read the rotation, position and scale values for the given part for the frame
 * at the given frameslot.
 *
 * Both the anim header and frame data must be loaded already.
 */
void animGetRotTranslateScale(s32 part, bool flip, struct skeleton *skel, s16 animnum, u8 frameslot, struct coord *rot, struct coord *translate, struct coord *scale)
{
	s32 i;
	u16 introt[3];
	u8 readbitlen;
	u8 *framebytes = g_AnimFrameBytes[frameslot];
	u8 framelen;
	u8 *ptr;
	u8 *end;
	s32 bitoffset;
	u32 stack;

	if (flip) {
		part = skel->things[part][1];
	}

	framelen = g_Anims[animnum].framelen;
	ptr = g_AnimHeaderBytes[g_AnimToHeaderSlot[animnum]];
	bitoffset = 0;
	end = ptr + g_Anims[animnum].headerlen;

	for (i = 0; i < part && ptr < end; i++) {
		u8 flags = *ptr;
		ptr++;

		if (flags & ANIMFIELD_08) {
			bitoffset += ptr[2] + ptr[5] + ptr[8] + ptr[11];
			ptr += 12;
		} else if (flags & ANIMFIELD_S16_TRANSLATE) {
			bitoffset += ptr[2] + ptr[5] + ptr[8];
			ptr += 9;
		} else if (flags & ANIMFIELD_S32_TRANSLATE) {
			bitoffset += ptr[0] + ptr[5] + ptr[10];
			ptr += 15;
		}

		if (flags & ANIMFIELD_S16_ROTATE) {
			bitoffset += ptr[2] + ptr[5] + ptr[8];
			ptr += 9;
		} else if (flags & ANIMFIELD_F32_ROTATE) {
			bitoffset += 96;
		}

		if (flags & ANIMFIELD_CAMERA) {
			bitoffset += ptr[0];
			ptr += 5;
		}

		if (flags & ANIMFIELD_F32_SCALE) {
			bitoffset += 0x60;
		}
	}

	if (ptr < end) {
		u8 flags = *ptr;
		ptr++;

		if (flags & ANIMFIELD_S16_TRANSLATE) {
			readbitlen = ptr[2];
			translate->x = (s16) (animReadSignedShort(framebytes, readbitlen, bitoffset) + (ptr[0] << 8) + ptr[1]);
			bitoffset += readbitlen;

			readbitlen = ptr[5];
			translate->y = (s16) (animReadSignedShort(framebytes, readbitlen, bitoffset) + (ptr[3] << 8) + ptr[4]);
			bitoffset += readbitlen;

			readbitlen = ptr[8];
			translate->z = (s16) (animReadSignedShort(framebytes, readbitlen, bitoffset) + (ptr[6] << 8) + ptr[7]);
			bitoffset += readbitlen;

			ptr += 9;
		} else if (flags & ANIMFIELD_S32_TRANSLATE) {
			readbitlen = ptr[0];
			translate->x = (animReadBits(framebytes, readbitlen, bitoffset) + ((ptr[1] << 24) + (ptr[2] << 16) + (ptr[3] << 8) + ptr[4])) * 0.001f;
			bitoffset += readbitlen;

			readbitlen = ptr[5];
			translate->y = (animReadBits(framebytes, readbitlen, bitoffset) + ((ptr[6] << 24) + (ptr[7] << 16) + (ptr[8] << 8) + ptr[9])) * 0.001f;
			bitoffset += readbitlen;

			readbitlen = ptr[10];
			translate->z = (animReadBits(framebytes, readbitlen, bitoffset) + ((ptr[11] << 24) + (ptr[12] << 16) + (ptr[13] << 8) + ptr[14])) * 0.001f;
			bitoffset += readbitlen;

			ptr += 15;
		} else {
			if (flags & ANIMFIELD_08) {
				bitoffset += ptr[2] + ptr[5] + ptr[8] + ptr[11];
				ptr += 12;
			}

			translate->x = translate->y = translate->z = 0.0f;
		}

		if (flags & ANIMFIELD_S16_ROTATE) {
			readbitlen = ptr[2];
			introt[0] = animReadBits(framebytes, readbitlen, bitoffset);
			introt[0] += (ptr[0] << 8) + ptr[1];
			introt[0] <<= 16 - framelen;
			bitoffset += readbitlen;

			readbitlen = ptr[5];
			introt[1] = animReadBits(framebytes, readbitlen, bitoffset);
			introt[1] += (ptr[3] << 8) + ptr[4];
			introt[1] <<= 16 - framelen;
			bitoffset += readbitlen;

			readbitlen = ptr[8];
			introt[2] = animReadBits(framebytes, readbitlen, bitoffset);
			introt[2] += (ptr[6] << 8) + ptr[7];
			introt[2] <<= 16 - framelen;
			bitoffset += readbitlen;

			rot->x = introt[0] * M_BADTAU / 65536.0f;

			if (flip) {
				if (introt[1] != 0) {
					rot->y = (0x10000 - introt[1]) * M_BADTAU / 65536.0f;
				} else {
					rot->y = 0.0f;
				}

				if (introt[2] != 0) {
					rot->z = (0x10000 - introt[2]) * M_BADTAU / 65536.0f;
				} else {
					rot->z = 0.0f;
				}
			} else {
				rot->y = introt[1] * M_BADTAU / 65536.0f;
				rot->z = introt[2] * M_BADTAU / 65536.0f;
			}
		} else if (flags & ANIMFIELD_F32_ROTATE) {
			s32 sp38;

			sp38 = animReadBits(framebytes, 32, bitoffset);
			rot->x = *(f32 *)&sp38;
			bitoffset += 32;

			sp38 = animReadBits(framebytes, 32, bitoffset);
			rot->y = *(f32 *)&sp38;
			bitoffset += 32;

			sp38 = animReadBits(framebytes, 32, bitoffset);
			rot->z = *(f32 *)&sp38;
			bitoffset += 32;

			if (flip) {
				if (rot->y != 0.0f) {
					rot->y = M_BADTAU - rot->y;
				}

				if (rot->z != 0.0f) {
					rot->z = M_BADTAU - rot->z;
				}
			}
		} else {
			rot->x = rot->y = rot->z = 0.0f;
		}

		if (flags & ANIMFIELD_F32_SCALE) {
			s32 word;

			word = animReadBits(framebytes, 32, bitoffset);
			scale->x = *(f32 *)&word;
			bitoffset += 32;

			word = animReadBits(framebytes, 32, bitoffset);
			scale->y = *(f32 *)&word;
			bitoffset += 32;

			word = animReadBits(framebytes, 32, bitoffset);
			scale->z = *(f32 *)&word;
		} else {
			scale->x = scale->y = scale->z = 1.0f;
		}

		return;
	}

	rot->x = rot->y = rot->z = 0.0f;
	translate->x = translate->y = translate->z = 0.0f;
	scale->x = scale->y = scale->z = 1.0f;
}

/**
 * Read the position and Y rotation (?) values for the given part at the given
 * frame number.
 *
 * No data needs to be loaded by the caller - the function will ensure the
 * header and frame are loaded.
 */
u16 animGetPosAngleAsInt(s32 part, bool flip, struct skeleton *skel, s16 animnum, s32 framenum, s16 inttranslate[3], bool arg6)
{
	u16 result = 0;
	s32 bitoffset;
	u8 readbitlen;
	u8 slot;
	u8 *framebytes;
	u8 *ptr;
	s32 i;

	if (arg6) {
		inttranslate[0] = 0;
		inttranslate[1] = 0;
		inttranslate[2] = var8005f014[animnum];
	} else {
		animLoadHeader(animnum);
		slot = animLoadFrame(animnum, framenum);
		animForgetFrameBirths();

		framebytes = g_AnimFrameBytes[slot];

		if (flip) {
			part = skel->things[part][1];
		}

		bitoffset = 0;
		ptr = g_AnimHeaderBytes[g_AnimToHeaderSlot[animnum]];

		for (i = 0; i < part; i++) {
			u8 flags = *ptr;
			ptr++;

			if (flags & ANIMFIELD_08) {
				bitoffset += ptr[2] + ptr[5] + ptr[8] + ptr[11];
				ptr += 12;
			} else if (flags & ANIMFIELD_S16_TRANSLATE) {
				bitoffset += ptr[2] + ptr[5] + ptr[8];
				ptr += 9;
			} else if (flags & ANIMFIELD_S32_TRANSLATE) {
				bitoffset += ptr[0] + ptr[5] + ptr[10];
				ptr += 15;
			}

			if (flags & ANIMFIELD_S16_ROTATE) {
				bitoffset += ptr[2] + ptr[5] + ptr[8];
				ptr += 9;
			} else if (flags & ANIMFIELD_F32_ROTATE) {
				bitoffset += 96;
			}

			if (flags & ANIMFIELD_CAMERA) {
				bitoffset += *ptr;
				ptr += 5;
			}

			if (flags & ANIMFIELD_F32_SCALE) {
				bitoffset += 96;
			}
		}

		readbitlen = ptr[3];
		inttranslate[0] = animReadSignedShort(framebytes, readbitlen, bitoffset) + ptr[1] * 256 + ptr[2];
		bitoffset += readbitlen;

		readbitlen = ptr[6];
		inttranslate[1] = animReadSignedShort(framebytes, readbitlen, bitoffset) + ptr[4] * 256 + ptr[5];
		bitoffset += readbitlen;

		readbitlen = ptr[9];
		inttranslate[2] = animReadSignedShort(framebytes, readbitlen, bitoffset) + ptr[7] * 256 + ptr[8];
		bitoffset += readbitlen;

		readbitlen = ptr[12];
		result = animReadSignedShort(framebytes, readbitlen, bitoffset) + ptr[10] * 256 + ptr[11];

		if (flip) {
			inttranslate[0] = -inttranslate[0];

			if (result != 0) {
				result = 0x10000 - result;
			}
		}
	}

	return result;
}

f32 animGetTranslateAngle(s32 part, bool flip, struct skeleton *skel, s16 animnum, s32 framenum, struct coord *translate, bool arg6)
{
	s16 inttranslate[3];

	f32 angle = animGetPosAngleAsInt(part, flip, skel, animnum, framenum, inttranslate, arg6);

	translate->x = inttranslate[0];
	translate->y = inttranslate[1];
	translate->z = inttranslate[2];

	return angle * M_BADTAU / 65536.0f;
}

/**
 * Return a camera value (FOV Y or blur frac) for the current frame.
 *
 * The function assumes the current frame's data has been loaded.
 * Its slot is provided by the frameslot argument.
 *
 * When part = 1, the returned value is the FOV Y.
 * When part = 2, the returned value is the blur frac.
 */
f32 animGetCameraValue(s32 part, s16 animnum, u8 frameslot)
{
	u32 stack[2];
	u8 *framebytes = g_AnimFrameBytes[frameslot];
	u8 *ptr = g_AnimHeaderBytes[g_AnimToHeaderSlot[animnum]];
	f32 result = 0;
	s32 bitoffset = 0;
	s32 i;
	u8 *end = ptr + g_Anims[animnum].headerlen;

	for (i = 0; i < part && ptr < end; i++) {
		u8 flags = ptr[0];
		ptr++;

		if (flags & ANIMFIELD_08) {
			bitoffset += ptr[2] + ptr[5] + ptr[8] + ptr[11];
			ptr += 12;
		} else if (flags & ANIMFIELD_S16_TRANSLATE) {
			bitoffset += ptr[2] + ptr[5] + ptr[8];
			ptr += 9;
		} else if (flags & ANIMFIELD_S32_TRANSLATE) {
			bitoffset += ptr[0] + ptr[5] + ptr[10];
			ptr += 15;
		}

		if (flags & ANIMFIELD_S16_ROTATE) {
			bitoffset += ptr[2] + ptr[5] + ptr[8];
			ptr += 9;
		} else if (flags & ANIMFIELD_F32_ROTATE) {
			bitoffset += 0x60;
		}

		if (flags & ANIMFIELD_CAMERA) {
			bitoffset += ptr[0];
			ptr += 5;
		}

		if (flags & ANIMFIELD_F32_SCALE) {
			bitoffset += 0x60;
		}
	}

	if (ptr < end) {
		u8 flags = ptr[0];
		ptr++;

		if (flags & ANIMFIELD_CAMERA) {
			/**
			 * In the header:
			 * ptr[0] = number of bits to read in the frame data
			 * ptr[1,2,3,4] = base value
			 *
			 * The value in the frame data is an adjustment value that is added
			 * to the base value.
			 */
			s32 framevalue = animReadBits(framebytes, ptr[0], bitoffset);
			result = (framevalue + ptr[1] * 0x1000000 + ptr[2] * 0x10000 + ptr[3] * 0x100 + ptr[4]) * 0.001f;
		}
	}

	return result;
}

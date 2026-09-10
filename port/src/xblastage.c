/**
 * The XBLA release's level geometry. See xblastage.h for what this is and
 * CLAUDE-notes/xbla.md, "The level files", for how the format was worked out.
 *
 * The shape of it: the file slot serves the ROM's copy of a level as it always
 * has, and bgLoadRoom() asks here for each room as it loads it. While the
 * switches are on and the level is a stock one the release rewrote, the room's
 * bytes come out of the release's copy of the file instead, found through the
 * release's own room table, and everything else about the level - the room
 * table bg.c holds, the portals, the lights, the bounding boxes - stays the
 * ROM's. The two copies agree on all of that, checked byte for byte across
 * the 24 levels the release stores inflated: the only differences in sections
 * 1 and 3 are the room offsets and the per-room size hints. That is what
 * makes the switch live inside a level, the way the meshes' is: a room can be
 * loaded from either copy under the same level, so flipping it drops the
 * loaded rooms and the next frame loads them again from the other copy.
 */

#ifndef PLATFORM_N64

#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "config.h"
#include "system.h"
#include "romdata.h"
#include "lib/main.h"
#include "game/bg.h"
#include "game/tex.h"
#include "xblamesh.h"
#include "xblatex.h"
#include "xblastage.h"

// The bg file's header: primary inflated size, section 1 size, primary stored
// size. The primary's pointers are in the 0x0f000000 segment.
#define XBLASTAGE_HEADER 12
#define XBLASTAGE_SEG    0x0f000000

// A file bigger than any level the release has (bg_lue is 1.4MB) is not one.
#define XBLASTAGE_MAXFILE (8 * 1024 * 1024)

// struct bgroom as it sits in the file, and the room gfxdata header ahead of
// the first roomblock (20 bytes each in the file).
#define XBLASTAGE_ROOMENTRY   20
#define XBLASTAGE_GFXHEADER   0x18
#define XBLASTAGE_BLOCK       20

static s32 optEnabled = 1; // Mod.XblaStages
static s32 xblaStageVerbose;

// The release's copy of the running level's file, read when the first room
// is asked for and kept until lvReset(), and where each room is in it: room
// r's segment address in the release's own table, for r = 1 to numrooms + 1,
// the last being the end of the last room.
static u8 *relData;
static u32 relLen;
static s32 relFileNum;
static s32 relNumRooms;
static u32 *relRoomAddr;
static s32 relTried; // the file was looked at for this level, whichever way it went

// What the rooms now loaded were built under, so that a flip of either switch
// knows whether there is anything to reload.
static s32 roomsWant;

// The room bgLoadRoom() is converting came from the release, which is what
// texLoadFromGdl() asks about a texture number the ROM does not have.
static s32 curRoomRelease;

// File ids whose release copy was looked at and turned down - stored the
// ROM's way, or not a level this can make safe. The package does not change
// under a running game, so once is enough.
#define XBLASTAGE_MAXFILEID 0x2000
static u8 refused[XBLASTAGE_MAXFILEID / 8];

// A white RGBA16 tile for a record that will not bind, the same reason the
// meshes keep one: the room's combiner reads a texel regardless.
static u16 xblaStageWhiteTile[XBLATEX_TILE * XBLATEX_TILE] __attribute__((aligned(64)));

static u32 xblaStageBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void xblaStageWriteBE32(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static s32 xblaStageIs1173(const u8 *p)
{
	return p[0] == 0x11 && p[1] == 0x73;
}

static s32 xblaStageWant(void)
{
	// The stage geometry is the models feature applied to the rooms, so it
	// follows that switch: a player with the meshes off sees the game's own
	// levels whatever this is set to.
	return optEnabled && xblaMeshGetEnabled();
}

s32 xblaStageGetEnabled(void)
{
	return optEnabled;
}

void xblaStageSetEnabled(s32 enabled)
{
	optEnabled = enabled ? 1 : 0;
	xblaStageSwitched();
}

void xblaStageSetVerbose(s32 verbose)
{
	xblaStageVerbose = verbose;
}

static void xblaStageForget(void)
{
	free(relData);
	free(relRoomAddr);
	relData = NULL;
	relRoomAddr = NULL;
	relLen = 0;
	relFileNum = 0;
	relNumRooms = 0;
	relTried = 0;
	curRoomRelease = 0;
}

void xblaStageLevelReset(void)
{
	// The last level's file goes. The next level's is read when its first
	// room asks, under whatever the switches say then.
	xblaStageForget();
	roomsWant = xblaStageWant();
}

void xblaStageSwitched(void)
{
	const s32 want = xblaStageWant();

	if (want) {
		// The meshes' switch may just have unpacked the package that an
		// earlier look found still inside its archive: look again.
		relTried = 0;
	}

	if (want == roomsWant || !STAGE_IS_LEVEL(mainGetStageNum())) {
		return;
	}

	// The rooms loaded so far are the other copy's. Dropping them is what the
	// game does when it is short of memory, and the visible ones come back on
	// the next frame - from the copy the switches now name.
	bgUnloadAllRooms();
	roomsWant = want;

	if (xblaStageVerbose) {
		sysLogPrintf(LOG_NOTE, "xblastage: rooms dropped to reload from the %s", want ? "release" : "ROM");
	}
}

s32 xblaStageIsRelease(void)
{
	return curRoomRelease;
}

/**
 * One room's data, checked and corrected in place.
 *
 * `room` is the room's bytes in the file, `base` the segment address the
 * room's own pointers are relative to (its entry in the room table), `size`
 * how many bytes it has. Returns 0 for a room this cannot make sense of.
 */
static s32 xblaStageFixRoom(u8 *room, u32 base, u32 size, s32 roomnum,
		u32 *numloads, u32 *numstale)
{
	u32 vertices;
	u32 end;
	u32 p;

	if (size < XBLASTAGE_GFXHEADER) {
		return 0;
	}

	if (xblaStageIs1173(room)) {
		// Compressed as the ROM's are: the game inflates it as it always has,
		// and it is not the release's shape, so there is nothing to correct.
		return 1;
	}

	vertices = xblaStageBE32(room);

	if (vertices == 0) {
		// A room with nothing in it: the header alone, 24 bytes. Four of
		// the release's levels have one.
		return 1;
	}

	if (vertices < base || vertices - base > size) {
		return 0;
	}

	end = vertices - base;

	// The roomblocks run from the header to the vertices, except that a parent
	// block's coordinates sit between the last block and the vertices - the
	// same rule the port's converter uses (convertRoomGfxData).
	for (p = XBLASTAGE_GFXHEADER; p + XBLASTAGE_BLOCK <= end; p += XBLASTAGE_BLOCK) {
		const u8 type = room[p];
		const u32 a = xblaStageBE32(room + p + 8);
		const u32 b = xblaStageBE32(room + p + 12);

		if (type == 1) {
			if (b >= base && b - base < end) {
				end = b - base;
			}
		} else if (type == 0 && a != 0 && (a < base || a - base >= size)) {
			// A leaf the release left pointing at the ROM's layout of the
			// room. Every one found is an orphan nothing links to, but the
			// loader relocates every block in the table whether or not it is
			// drawn, and a list at a negative offset is read all the same.
			// Emptied, and an empty leaf is what the game skips.
			xblaStageWriteBE32(room + p + 8, 0);
			xblaStageWriteBE32(room + p + 12, 0);
			xblaStageWriteBE32(room + p + 16, 0);
			(*numstale)++;
		}
	}

	// The lists: each leaf's, walked to its end
	for (p = XBLASTAGE_GFXHEADER; p + XBLASTAGE_BLOCK <= end; p += XBLASTAGE_BLOCK) {
		const u8 type = room[p];
		const u32 a = xblaStageBE32(room + p + 8);
		u32 q;

		if (type != 0 || a == 0) {
			continue;
		}

		for (q = a - base; q + 8 <= size; q += 8) {
			const u32 w0 = xblaStageBE32(room + q);
			const u8 op = w0 >> 24;

			if (op == G_ENDDL) {
				break;
			}

			if (op == G_VTX && (w0 & 0xffff) == 0) {
				// gSPVertex packs (n-1)<<4|v0 into byte 1 and the byte
				// length into the low half. The release wrote the count and
				// left the length at zero; gfx_pc divides the length by
				// sizeof(Vtx) for the count.
				const u32 n = ((w0 >> 20) & 0xf) + 1;

				xblaStageWriteBE32(room + q, w0 | (n * sizeof(Vtx)));
				(*numloads)++;
			}
		}
	}

	return 1;
}

/**
 * The whole file: the header, the primary data's room table, each room.
 * Fills `addrs` with each room's segment address - the entry past the last
 * room too, since that is where the last room ends - up to `maxrooms` rooms,
 * and returns how many rooms there are, or 0 when this is not a file to
 * read rooms out of.
 */
static s32 xblaStageFix(u8 *data, u32 len, const char *name, u32 *addrs, s32 maxrooms)
{
	u32 inflated;
	u32 section1;
	u32 primstored;
	const u8 *prim;
	u32 rooms;
	s32 numrooms = 0;
	u32 numloads = 0;
	u32 numstale = 0;
	s32 r;

	if (len < XBLASTAGE_HEADER + 24) {
		sysLogPrintf(LOG_WARNING, "xblastage: %s: %u bytes is not a level", name, len);
		return 0;
	}

	inflated = xblaStageBE32(data);
	section1 = xblaStageBE32(data + 4);
	primstored = xblaStageBE32(data + 8);

	if (XBLASTAGE_HEADER + section1 > len || primstored > section1) {
		sysLogPrintf(LOG_WARNING, "xblastage: %s: section 1 of %u bytes does not fit %u", name, section1, len);
		return 0;
	}

	if (inflated != primstored || xblaStageIs1173(data + XBLASTAGE_HEADER)) {
		// The ROM's shape - primary data compressed, rooms compressed. Seven
		// of the release's files are this, and they are the ROM's geometry
		// to the byte: nothing to gain, and the rooms cannot be walked
		// without inflating them. The ROM's copy is as good.
		if (xblaStageVerbose) {
			sysLogPrintf(LOG_NOTE, "xblastage: %s: stored compressed, using the ROM's", name);
		}

		return 0;
	}

	prim = data + XBLASTAGE_HEADER;
	rooms = xblaStageBE32(prim + 4);

	if (rooms < XBLASTAGE_SEG || rooms - XBLASTAGE_SEG + XBLASTAGE_ROOMENTRY * 2 > inflated) {
		sysLogPrintf(LOG_WARNING, "xblastage: %s: no room table", name);
		return 0;
	}

	rooms -= XBLASTAGE_SEG;

	// Room r is [entry r, entry r+1); the entry past the last room holds the
	// end of section 1 and the one past that is zero. With the primary stored
	// as it is, a segment address maps to the file as addr - seg + header.
	for (r = 1; ; r++) {
		u32 start;
		u32 next;

		if (rooms + (u32)(r + 2) * XBLASTAGE_ROOMENTRY > inflated) {
			sysLogPrintf(LOG_WARNING, "xblastage: %s: room table runs off the primary data", name);
			return 0;
		}

		start = xblaStageBE32(prim + rooms + (u32)r * XBLASTAGE_ROOMENTRY);
		next = xblaStageBE32(prim + rooms + (u32)(r + 1) * XBLASTAGE_ROOMENTRY);

		if (start == 0 || next == 0) {
			break;
		}

		if (start < XBLASTAGE_SEG || next <= start || next - XBLASTAGE_SEG + XBLASTAGE_HEADER > XBLASTAGE_HEADER + section1) {
			sysLogPrintf(LOG_WARNING, "xblastage: %s: room %d at %08x..%08x is outside section 1", name, r, start, next);
			return 0;
		}

		if (r > maxrooms) {
			sysLogPrintf(LOG_WARNING, "xblastage: %s: more than %d rooms", name, maxrooms);
			return 0;
		}

		if (!xblaStageFixRoom(data + XBLASTAGE_HEADER + (start - XBLASTAGE_SEG), start, next - start, r, &numloads, &numstale)) {
			sysLogPrintf(LOG_WARNING, "xblastage: %s: room %d is not a room", name, r);
			return 0;
		}

		addrs[r] = start;
		addrs[r + 1] = next;
		numrooms++;
	}

	sysLogPrintf(LOG_NOTE, "xblastage: %s from the release: %u bytes, %d rooms, %u vertex loads sized, %u stale blocks emptied",
			name, len, numrooms, numloads, numstale);

	return numrooms;
}

/**
 * The release's copy of the running level's file, read and checked. Returns
 * whether there is one to serve rooms from.
 */
static s32 xblaStageLoadLevel(void)
{
	const char *name;
	s32 fileNum;
	s32 numrooms;
	u8 *raw;
	u32 len = 0;
	u32 *addrs;

	if (g_StageIndex < 0) {
		return 0;
	}

	fileNum = g_Stages[g_StageIndex].bgfileid;

	if (fileNum <= 0 || fileNum >= XBLASTAGE_MAXFILEID) {
		return 0;
	}

	if (refused[fileNum >> 3] & (1 << (fileNum & 7))) {
		return 0;
	}

	// A mod's level under a stock name means what the mod put there, and
	// the release's copy is a copy of the ROM's level.
	if (!romdataFileIsStock(fileNum)) {
		return 0;
	}

	name = romdataFileGetName(fileNum);

	if (!name) {
		name = "bg file";
	}

	// Ready-only: a room load is not the place to take a 250MB archive
	// apart. The meshes' switch did that, or will, and this is asked again
	// after it has (xblaStageSwitched()).
	raw = xblaMeshReadFile((u16)fileNum, &len);

	if (!raw) {
		return 0;
	}

	if (len > XBLASTAGE_MAXFILE) {
		free(raw);
		refused[fileNum >> 3] |= 1 << (fileNum & 7);
		return 0;
	}

	// The ROM's table has the same rooms, and g_Vars.roomcount counts them
	// with the entry past the last: the release's file must agree, since its
	// rooms are being read under the ROM's numbering.
	addrs = calloc((size_t)g_Vars.roomcount + 2, sizeof(u32));

	if (!addrs) {
		free(raw);
		return 0;
	}

	numrooms = xblaStageFix(raw, len, name, addrs, g_Vars.roomcount - 1);

	if (numrooms != g_Vars.roomcount - 1) {
		if (numrooms) {
			sysLogPrintf(LOG_WARNING, "xblastage: %s: %d rooms where the ROM has %d, using the ROM's",
					name, numrooms, g_Vars.roomcount - 1);
		}

		free(raw);
		free(addrs);
		refused[fileNum >> 3] |= 1 << (fileNum & 7);
		return 0;
	}

	relData = raw;
	relLen = len;
	relFileNum = fileNum;
	relNumRooms = numrooms;
	relRoomAddr = addrs;

	return 1;
}

u32 xblaStageRoomSize(s32 roomnum)
{
	const s32 want = xblaStageWant();

	// Whatever this room comes from, it is loading under the switches as
	// they are now
	roomsWant = want;
	curRoomRelease = 0;

	if (!want) {
		return 0;
	}

	if (!relData) {
		if (relTried) {
			return 0;
		}

		relTried = 1;

		if (!xblaStageLoadLevel()) {
			return 0;
		}
	}

	if (roomnum < 1 || roomnum > relNumRooms) {
		return 0;
	}

	return relRoomAddr[roomnum + 1] - relRoomAddr[roomnum];
}

uintptr_t xblaStageRoomRead(s32 roomnum, u8 *dst, u32 len)
{
	const u32 addr = relRoomAddr[roomnum];

	memcpy(dst, relData + XBLASTAGE_HEADER + (addr - XBLASTAGE_SEG), len);
	curRoomRelease = 1;

	if (xblaStageVerbose) {
		sysLogPrintf(LOG_NOTE, "xblastage: room %d from the release, %u bytes at %08x", roomnum, len, addr);
	}

	return addr;
}

void xblaStageRoomDone(void)
{
	curRoomRelease = 0;
}

/**
 * The tile state for a record the ROM has no texture for.
 *
 * What is written is the meshes' material, in the room's terms: the stand-in
 * tile on tile 0 with the command's wrap modes, a copy on tile 1 for a
 * combiner that reads TEXEL1, and a gSPTexture whose scale turns the room's
 * coordinates into the tile's. 4J measured those coordinates in texels of the
 * full picture - a 1024 wide texture runs to s = 32767, the top of what a Vtx
 * holds - and the renderer scales s and t by this before it divides by the
 * tile, so 32/width on a 32 texel tile puts a coordinate of one picture width
 * at one tile width, which is where the replacement is sampled from.
 */
Gfx *xblaStageWriteTexture(Gfx *gdl, const Gfx *cmd, u32 record)
{
	const s32 smode = (cmd->words.w0 >> 22) & 3;
	const s32 tmode = (cmd->words.w0 >> 20) & 3;
	const s32 cms = texModeToGbiMode(smode);
	const s32 cmt = texModeToGbiMode(tmode);
	const void *tile = xblaTexBind(record);
	s32 width = 0;
	s32 height = 0;
	u32 scales = 0xffff;
	u32 scalet = 0xffff;

	if (!tile || !xblaTexRecordSize(record, &width, &height)) {
		if (xblaStageWhiteTile[0] != 0xffff) {
			memset(xblaStageWhiteTile, 0xff, sizeof(xblaStageWhiteTile));
		}

		tile = xblaStageWhiteTile;
	}

	if (width > XBLATEX_TILE) {
		scales = 0x10000 * XBLATEX_TILE / width;
	}

	if (height > XBLATEX_TILE) {
		scalet = 0x10000 * XBLATEX_TILE / height;
	}

	if (!g_TexPipeSynced) {
		gDPPipeSync(gdl++);
		g_TexPipeSynced = true;
	}

	gDPLoadTextureBlock(gdl++, tile, G_IM_FMT_RGBA, G_IM_SIZ_16b,
			XBLATEX_TILE, XBLATEX_TILE, 0,
			cms, cmt, XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);

	gDPSetTile(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b,
			((XBLATEX_TILE * G_IM_SIZ_16b_LINE_BYTES) + 7) >> 3, 0, 1, 0,
			cmt, XBLATEX_TILE_MASK, G_TX_NOLOD,
			cms, XBLATEX_TILE_MASK, G_TX_NOLOD);
	gDPSetTileSize(gdl++, 1, 0, 0,
			(XBLATEX_TILE - 1) << G_TEXTURE_IMAGE_FRAC,
			(XBLATEX_TILE - 1) << G_TEXTURE_IMAGE_FRAC);

	gSPTexture(gdl++, scales, scalet, 0, G_TX_RENDERTILE, G_ON);

	gDPPipeSync(gdl++);
	g_TexPipeSynced = true;

	// The tile state cache in tex.c knows nothing of what was just written,
	// and the next texture must not be allowed to skip a command on the
	// strength of it.
	texResetTiles();

	if (xblaStageVerbose) {
		sysLogPrintf(LOG_NOTE, "xblastage: record %u (%dx%d) bound%s, scale %04x %04x",
				record, width, height, tile == xblaStageWhiteTile ? " white" : "", scales, scalet);
	}

	return gdl;
}

PD_CONSTRUCTOR static void xblaStageConfigInit(void)
{
	configRegisterInt("Mod.XblaStages", &optEnabled, 0, 1);
}

#else

s32 xblaStageGetEnabled(void) { return 0; }
void xblaStageSetEnabled(s32 enabled) { }
void xblaStageSwitched(void) { }
void xblaStageLevelReset(void) { }
u32 xblaStageRoomSize(s32 roomnum) { return 0; }
uintptr_t xblaStageRoomRead(s32 roomnum, u8 *dst, u32 len) { return 0; }
void xblaStageRoomDone(void) { }
s32 xblaStageIsRelease(void) { return 0; }
void xblaStageSetVerbose(s32 verbose) { }

#endif

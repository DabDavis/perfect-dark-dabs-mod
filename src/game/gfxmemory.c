#include <ultra64.h>
#include "constants.h"
#include "game/gfxmemory.h"
#include "game/stubs/game_175f50.h"
#include "bss.h"
#include "lib/args.h"
#include "lib/main.h"
#include "lib/rzip.h"
#include "lib/dma.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/str.h"
#include "data.h"
#include "types.h"
#include "platform.h"
#include "system.h"

/**
 * This file handles memory usage for graphics related tasks.
 *
 * There are two pools, "gfx" and "vtx", which are used to store different data.
 *
 * The gfx pool (g_GfxBuffers) is sized based on the stage's -mgfx and -mgfxtra
 * arguments. It contains only the master display list's GBI bytecode.
 * The master gdl is passed through all rendering functions in the game engine,
 * where each appends to the display list.
 *
 * The vtx pool (g_VtxBuffers) is sized based on the stage's -mvtx argument.
 * It is used for auxiliary graphics data such as vertex arrays, matrices and
 * colours.
 *
 * Both the gfx and vtx pools are split into two buffers of equal size.
 * Only one buffer is active at a time - the other is being drawn to the screen
 * while the active one is being built. Each time a frame is finished the active
 * buffer index is swapped to the other one.
 *
 * Both the gfx and vtx pools have a third element in them, but this is just a
 * marker for the end of the second element's allocation.
 */

/**
 * On 64-bit platforms the Gfx struct is twice as large.
*/
#ifdef PLATFORM_64BIT
#define GFX_SIZE_MULTIPLIER 2
#else
#define GFX_SIZE_MULTIPLIER 1
#endif

u8 *g_GfxBuffers[NUM_GFXTASKS + 1];
u32 var800aa58c;
u8 *g_VtxBuffers[NUM_GFXTASKS + 1];
u8 *g_GfxMemPos;
u8 g_GfxActiveBufferIndex;
u32 g_GfxRequestedDisplayList;
static bool g_GfxVtxOverflowReported = false;

u32 g_GfxSizesByPlayerCount[] = {
	0x00010000 * GFX_SIZE_MULTIPLIER,
	0x00018000 * GFX_SIZE_MULTIPLIER,
	0x00020000 * GFX_SIZE_MULTIPLIER,
	0x00028000 * GFX_SIZE_MULTIPLIER,
};

u32 g_VtxSizesByPlayerCount[] = {
	0x00010000,
	0x00018000,
	0x00020000,
	0x00028000,
};

s32 g_GfxNumSwapsPerBuffer[NUM_GFXTASKS] = {0, 1};
u32 g_GfxNumSwaps = 2;

/**
 * Allocate graphics memory from the heap. Presumably called on stage load.
 *
 * Comments in this function are strings that appear in an XBLA debug build.
 * They were likely in the N64 version but ifdeffed out.
 */
void gfxReset(void)
{
	s32 stack;

	if (argFindByPrefix(1, "-mgfx")) {
		// Argument specified master_dl_size\n
		s32 gfx;
		s32 gfxtra = 0;

		gfx = strtol(argFindByPrefix(1, "-mgfx"), NULL, 0) * 1024;

		if (argFindByPrefix(1, "-mgfxtra")) {
			// ******** Extra specified but are we in the correct game mode I wonder???\n
			if ((g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) && PLAYERCOUNT() == 2) {
				// ******** Extra Display List Memeory Required\n
				// ******** Shall steal from video buffer\n
				// ******** If you try and run hi-res then\n
				// ******** you're gonna shafted up the arse\n
				// ******** so don't blame me\n
				gfxtra = strtol(argFindByPrefix(1, "-mgfxtra"), NULL, 0) * 1024;
			} else {
				// ******** No we're not so there\n
			}
		}

		// ******** Original Amount required = %dK ber buffer\n
		// ******** Extra Amount required = %dK ber buffer\n
		// ******** Total of %dK (Double Buffered)\n
		g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1] = (gfx + gfxtra) * GFX_SIZE_MULTIPLIER;
	}

	if (argFindByPrefix(1, "-mvtx")) {
		// Argument specified mtxvtx_size\n
		g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1] = strtol(argFindByPrefix(1, "-mvtx"), NULL, 0) * 1024;
	}

	// %d Players : Allocating %d bytes for master dl's\n
	g_GfxBuffers[0] = mempAlloc(g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1] * NUM_GFXTASKS, MEMPOOL_STAGE);
	g_GfxBuffers[1] = g_GfxBuffers[0] + g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1];
	g_GfxBuffers[2] = g_GfxBuffers[1] + g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1];

	// Allocating %d bytes for mtxvtx space\n
	g_VtxBuffers[0] = mempAlloc(g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1] * NUM_GFXTASKS, MEMPOOL_STAGE);
	g_VtxBuffers[1] = g_VtxBuffers[0] + g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1];
	g_VtxBuffers[2] = g_VtxBuffers[1] + g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1];

	g_GfxActiveBufferIndex = 0;
	g_GfxRequestedDisplayList = false;
	g_GfxMemPos = g_VtxBuffers[0];
	g_GfxVtxOverflowReported = false;
}

Gfx *gfxGetMasterDisplayList(void)
{
	g_GfxRequestedDisplayList = true;

	return (Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex];
}

/**
 * None of the allocators below bounds check - they just bump g_GfxMemPos.
 *
 * On N64 that was safe by construction: every stage had a -mvtx figure tuned
 * until the stage fitted, so the pool could not be overrun. A stage the
 * allocation table does not know about falls through to the table's default
 * entry instead, and anything past the end lands in whatever MEMPOOL_STAGE
 * handed out next - silent corruption surfacing a long way from here.
 *
 * Report the first overflow of each stage rather than fixing it up: the
 * allocation is what is wrong, and callers hold the returned pointers.
 */
static void gfxCheckVtxPool(const char *what)
{
	const u8 *end = g_VtxBuffers[g_GfxActiveBufferIndex + 1];

	if (g_GfxMemPos > end && !g_GfxVtxOverflowReported) {
		g_GfxVtxOverflowReported = true;
		sysLogPrintf(LOG_ERROR, "gfx: stage 0x%02x overran the vtx pool in %s: %d bytes past a %d byte buffer",
				mainGetStageNum(), what, (s32)(g_GfxMemPos - end),
				(s32)(g_VtxBuffers[1] - g_VtxBuffers[0]));
	}
}

void gfxCheckGfxPool(const Gfx *gdl)
{
	static bool reported = false;
	const Gfx *end = (const Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex + 1];

	if (gdl > end && !reported) {
		reported = true;
		sysLogPrintf(LOG_ERROR, "gfx: stage 0x%02x overran the master display list: %d commands past a %d command buffer",
				mainGetStageNum(), (s32)(gdl - end),
				(s32)((const Gfx *)g_GfxBuffers[1] - (const Gfx *)g_GfxBuffers[0]));
	}
}

Vtx *gfxAllocateVertices(u32 count)
{
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += count * sizeof(Vtx);
	g_GfxMemPos = (u8 *)ALIGN16((uintptr_t)g_GfxMemPos);
	gfxCheckVtxPool("vertices");

	return ptr;
}

void *gfxAllocateMatrix(void)
{
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += sizeof(Mtx);
	gfxCheckVtxPool("a matrix");

	return ptr;
}

/**
 * sizeof(LookAt) is 0x10 and it consists of two Light structs of 0x8 each.
 * The function allocates 0x8 for every count, so it could be allocating lights
 * instead, however it's only used for LookAts so it's named as LookAt.
 */
LookAt *gfxAllocateLookAt(s32 count)
{
	void *ptr = g_GfxMemPos;
#ifdef PLATFORM_64BIT
	g_GfxMemPos += count * (sizeof(LookAt) * 2);
#else
	g_GfxMemPos += count * (sizeof(LookAt) / 2);
#endif
	gfxCheckVtxPool("a lookat");

	return ptr;
}

Col *gfxAllocateColours(s32 count)
{
	void *ptr = g_GfxMemPos;
	count = ALIGN16(count * sizeof(Col));
	g_GfxMemPos += count;
	gfxCheckVtxPool("colours");

	return ptr;
}

void *gfxAllocate(u32 size)
{
	void *ptr = g_GfxMemPos;
	size = ALIGN16(size);
	g_GfxMemPos += size;
	gfxCheckVtxPool("a general allocation");

	return ptr;
}

void gfxSwapBuffers(void)
{
	g_GfxActiveBufferIndex ^= 1;
	g_GfxRequestedDisplayList = false;
	g_GfxMemPos = g_VtxBuffers[g_GfxActiveBufferIndex];
	g_GfxNumSwapsPerBuffer[g_GfxActiveBufferIndex] = g_GfxNumSwaps;
	g_GfxNumSwaps++;

	if (g_GfxNumSwaps == -1) {
		g_GfxNumSwaps = 2;
	}
}

s32 gfxGetFreeGfx(Gfx *gdl)
{
	return (Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex + 1] - gdl;
}

u32 gfxGetFreeVtx(void)
{
	return g_VtxBuffers[g_GfxActiveBufferIndex + 1] - g_GfxMemPos;
}

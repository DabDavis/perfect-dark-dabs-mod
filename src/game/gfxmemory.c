#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
#ifndef PLATFORM_N64
#include "xblamesh.h"
#include "system.h"
#include "net/net.h"
#endif

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

#ifndef PLATFORM_N64
/**
 * How much bigger the two graphics pools are than the stage asked for.
 *
 * Both are sized per stage for what an N64 could draw. The master display list
 * holds one frame's GBI, and the vtx pool holds the matrices - every chr that
 * ticks takes nummatrices * sizeof(Mtxf) out of it, some two kilobytes each, so
 * a stage's two hundred kilobytes is about a hundred chrs. Eighty simulants and
 * a match's worth of bodies in one room is several times that, and neither
 * allocator checks: g_GfxMemPos walks straight off the end of the vtx pool and
 * into the display list sitting next to it, which the renderer then reads as
 * GBI and stops on with "Unknown GBI opcode".
 *
 * This is memory the N64 did not have. The guards below are what makes running
 * out survivable rather than merely unlikely.
 */
#define GFX_POOL_SCALE 8
#endif

u8 *g_GfxBuffers[NUM_GFXTASKS + 1];
u32 var800aa58c;
u8 *g_VtxBuffers[NUM_GFXTASKS + 1];
u8 *g_GfxMemPos;
u8 g_GfxActiveBufferIndex;
u32 g_GfxRequestedDisplayList;
static bool g_GfxVtxOverflowReported = false;

#ifndef PLATFORM_N64
/**
 * The vtx pool on the PC: sized, guarded and grown (crash reports
 * 20261014-022841, -023303, -024154, -024211: an online match on a mod's map
 * drew more room colours than the pool held, the overrun wrote over whatever
 * MEMPOOL_STAGE handed out next, and the game fell over a second later in four
 * different places - the HUD's messages, the text renderer, collision).
 *
 * Each of the two buffers is the stage's figure (g_VtxNominal) followed by a
 * guard (g_VtxGuard) that nothing is meant to use. An allocation that crosses
 * the figure is still handed out contiguously from the guard, so the callers'
 * pointers and the address order camera.c relies on hold; one that would not
 * fit in the guard either is handed a scratch block off the heap (g_VtxSink)
 * - that frame draws whatever the scratch holds, which is a glitch for a frame
 * rather than corruption. Either way the pool is grown at the next buffer swap
 * to what the frame asked for and half again, into a heap block of its own;
 * the blocks it leaves behind are kept (the frame being drawn, and pointers a
 * frame keeps to the last one's matrices, are in them) and all freed at the
 * next stage load.
 */
static u32 g_VtxNominal;     // usable bytes per buffer
static u32 g_VtxGuard;       // guard bytes after each buffer
static u8 *g_VtxEnd;         // the active buffer's usable end
static u32 g_VtxSinkDemand;  // bytes this frame wanted that did not fit
static bool g_VtxGrowPending;
static u8 *g_VtxSink;
static u32 g_VtxSinkSize;
static void **g_VtxHeapBlocks;
static s32 g_VtxNumHeapBlocks;
static s32 g_VtxMaxHeapBlocks;
static u8 *g_VtxPrevStart;   // the generation before the last growth
static u8 *g_VtxPrevEnd;

#define VTX_GUARD_MIN (256 * 1024)
#define VTX_GROW_STEP (64 * 1024)
#define VTX_GROW_MAX (256u * 1024 * 1024)

static u32 gfxVtxGuardFor(u32 nominal)
{
	u32 guard = nominal / 4;

	return ALIGN16(guard < VTX_GUARD_MIN ? VTX_GUARD_MIN : guard);
}

static void *gfxVtxHeapAlloc(u32 size)
{
	u8 *raw;

	if (g_VtxNumHeapBlocks == g_VtxMaxHeapBlocks) {
		s32 max = g_VtxMaxHeapBlocks ? g_VtxMaxHeapBlocks * 2 : 16;
		void **blocks = realloc(g_VtxHeapBlocks, max * sizeof(*blocks));

		if (!blocks) {
			return NULL;
		}

		g_VtxHeapBlocks = blocks;
		g_VtxMaxHeapBlocks = max;
	}

	raw = calloc(1, (size_t)size + 16);

	if (!raw) {
		return NULL;
	}

	g_VtxHeapBlocks[g_VtxNumHeapBlocks++] = raw;

	return (void *)ALIGN16((uintptr_t)raw);
}

static void gfxVtxHeapFreeAll(void)
{
	s32 i;

	for (i = 0; i < g_VtxNumHeapBlocks; i++) {
		free(g_VtxHeapBlocks[i]);
	}

	g_VtxNumHeapBlocks = 0;
	g_VtxSink = NULL;
	g_VtxSinkSize = 0;
	g_VtxPrevStart = NULL;
	g_VtxPrevEnd = NULL;
}

static void gfxVtxSetBuffers(u8 *base, u32 nominal, u32 guard)
{
	g_VtxNominal = nominal;
	g_VtxGuard = guard;
	g_VtxBuffers[0] = base;
	g_VtxBuffers[1] = g_VtxBuffers[0] + nominal + guard;
	g_VtxBuffers[2] = g_VtxBuffers[1] + nominal + guard;
}
#endif

#ifndef PLATFORM_N64
// How much of each pool the last frame used, for the F3 trace dump: the
// master list as gfxCheckGfxPool() saw it at the end of the frame, the vtx
// pool as it stood when the buffers swapped.
static u32 g_GfxTraceGfxUsed;
static u32 g_GfxTraceGfxSize;
static u32 g_GfxTraceVtxUsed;
static u32 g_GfxTraceVtxSize;

void gfxTraceGetPools(u32 *gfxused, u32 *gfxsize, u32 *vtxused, u32 *vtxsize)
{
	*gfxused = g_GfxTraceGfxUsed;
	*gfxsize = g_GfxTraceGfxSize;
	*vtxused = g_GfxTraceVtxUsed;
	*vtxsize = g_GfxTraceVtxSize;
}
#endif

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
	s32 sizeindex = PLAYERCOUNT() - 1;

#ifndef PLATFORM_N64
	// a dedicated server can have no player of its own: never row -1
	if (g_NetMode != NETMODE_NONE) sizeindex = netGfxSizeIndex(sizeindex);
#endif

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
		g_GfxSizesByPlayerCount[sizeindex] = (gfx + gfxtra) * GFX_SIZE_MULTIPLIER;
	}

	if (argFindByPrefix(1, "-mvtx")) {
		// Argument specified mtxvtx_size\n
		g_VtxSizesByPlayerCount[sizeindex] = strtol(argFindByPrefix(1, "-mvtx"), NULL, 0) * 1024;
	}

	// Scaled into locals rather than back into the tables, which gfxReset()
	// would otherwise multiply again on every stage load.
	{
		u32 gfxsize = g_GfxSizesByPlayerCount[sizeindex];
		u32 vtxsize = g_VtxSizesByPlayerCount[sizeindex];

#ifndef PLATFORM_N64
		// netplay (phase 8): remote passes keep their matrices and vertices
		if (g_NetMode != NETMODE_NONE) vtxsize += netGfxVtxExtra();

		gfxsize *= GFX_POOL_SCALE;
		vtxsize *= GFX_POOL_SCALE;
#endif

		// %d Players : Allocating %d bytes for master dl's\n
		g_GfxBuffers[0] = mempAlloc(gfxsize * NUM_GFXTASKS, MEMPOOL_STAGE);
		g_GfxBuffers[1] = g_GfxBuffers[0] + gfxsize;
		g_GfxBuffers[2] = g_GfxBuffers[1] + gfxsize;

		// Allocating %d bytes for mtxvtx space\n
#ifdef PLATFORM_N64
		g_VtxBuffers[0] = mempAlloc(vtxsize * NUM_GFXTASKS, MEMPOOL_STAGE);
		g_VtxBuffers[1] = g_VtxBuffers[0] + vtxsize;
		g_VtxBuffers[2] = g_VtxBuffers[1] + vtxsize;
#else
		// the last stage's grown pools and scratch are not drawn from again
		gfxVtxHeapFreeAll();
		g_VtxSinkDemand = 0;
		g_VtxGrowPending = false;

		{
			u32 guard = gfxVtxGuardFor(vtxsize);

			gfxVtxSetBuffers(mempAlloc((vtxsize + guard) * NUM_GFXTASKS, MEMPOOL_STAGE), vtxsize, guard);
		}
#endif
	}

	g_GfxActiveBufferIndex = 0;
	g_GfxRequestedDisplayList = false;
	g_GfxMemPos = g_VtxBuffers[0];
	g_GfxVtxOverflowReported = false;
#ifndef PLATFORM_N64
	g_VtxEnd = g_VtxBuffers[0] + g_VtxNominal;
#endif
}

/**
 * Whether ptr lies in the frames' display list or vertex space - memory that
 * the next frame but one writes over. A monitor's screen is drawn into it
 * (tvscreenRender() points the node's rwdata gdl and vertices there), and the
 * hit tests of a later frame must not walk that list: by then it is another
 * frame's commands, run to wherever an end command happens to be, and their
 * vertex loads read relative to a stale four-vertex block (crash reports
 * 20260930-050213 and -053223, bgTestHitOnObj() from propFindAimingAt() on
 * the Carrington Institute).
 */
bool gfxIsFrameMemory(const void *ptr)
{
	const u8 *p = ptr;

	if (g_GfxBuffers[0] && p >= g_GfxBuffers[0] && p < g_GfxBuffers[NUM_GFXTASKS]) {
		return true;
	}

	if (g_VtxBuffers[0] && p >= g_VtxBuffers[0] && p < g_VtxBuffers[NUM_GFXTASKS]) {
		return true;
	}

#ifndef PLATFORM_N64
	// the frames drawn just before the pool last grew
	if (g_VtxPrevStart && p >= g_VtxPrevStart && p < g_VtxPrevEnd) {
		return true;
	}

	if (g_VtxSink && p >= g_VtxSink && p < g_VtxSink + g_VtxSinkSize) {
		return true;
	}
#endif

	return false;
}

#ifndef PLATFORM_N64
/**
 * Whose memory ptr is, in words, for the renderer's reports of a list it
 * could not read ("Unknown GBI opcode"): the frame's display list or vtx
 * buffers (the frame being drawn's - the buffers have swapped by the time it
 * is drawn - or the next one's), or a room's display lists - the
 * nearest loaded room's allocation below it, with the offset against the
 * length the room's lists came to. Anything else is "elsewhere" (a model
 * file, a static list, the heap).
 */
void gfxDescribeAddress(const void *ptr, char *buf, s32 size)
{
	const u8 *p = ptr;
	s32 bestroom = -1;
	uintptr_t bestoff = 0;
	s32 i;

	for (i = 0; i < NUM_GFXTASKS; i++) {
		if (g_GfxBuffers[i] && p >= g_GfxBuffers[i] && p < g_GfxBuffers[i + 1]) {
			snprintf(buf, size, "frame display list buffer %d (%s) +0x%lx", i,
					i == g_GfxActiveBufferIndex ? "the next frame's" : "the frame being drawn",
					(unsigned long)(p - g_GfxBuffers[i]));
			return;
		}

		if (g_VtxBuffers[i] && p >= g_VtxBuffers[i] && p < g_VtxBuffers[i + 1]) {
			snprintf(buf, size, "frame vtx buffer %d (%s) +0x%lx, in use to +0x%lx", i,
					i == g_GfxActiveBufferIndex ? "the next frame's" : "the frame being drawn",
					(unsigned long)(p - g_VtxBuffers[i]),
					(unsigned long)(i == g_GfxActiveBufferIndex ? (u32)(g_GfxMemPos - g_VtxBuffers[i]) : g_GfxTraceVtxUsed));
			return;
		}
	}

	if (g_Rooms) {
		for (i = 1; i < g_Vars.roomcount; i++) {
			const u8 *base = (const u8 *)g_Rooms[i].gfxdata;

			if (base && p >= base && (bestroom < 0 || (uintptr_t)(p - base) < bestoff)) {
				bestroom = i;
				bestoff = p - base;
			}
		}
	}

	if (bestroom >= 0 && bestoff < 0x100000) {
		snprintf(buf, size, "room %d's data +0x%lx (its lists end at +0x%x; loaded %d)", bestroom,
				(unsigned long)bestoff, g_Rooms[bestroom].gfxdatalen, g_Rooms[bestroom].loaded240);
		return;
	}

	snprintf(buf, size, "elsewhere (not frame memory, no loaded room's)");
}
#endif

Gfx *gfxGetMasterDisplayList(void)
{
	g_GfxRequestedDisplayList = true;

	return (Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex];
}

/**
 * On N64 none of the allocators below bounds checked - they just bumped
 * g_GfxMemPos. That was safe by construction: every stage had a -mvtx figure
 * tuned until the stage fitted. A stage the allocation table does not know
 * about falls through to the table's default entry instead, and the port draws
 * a great deal the N64 did not (twelve players online, eighty simulants, a
 * mod's rooms), and anything past the end used to land in whatever
 * MEMPOOL_STAGE handed out next - silent corruption surfacing a long way from
 * here. On the PC gfxVtxTake() serves every allocation from the pool, its
 * guard or the scratch block, and the pool grows to fit (see g_VtxNominal).
 */
#ifdef PLATFORM_N64
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
#else
static void gfxVtxReportOverflow(const char *what, u32 past)
{
	g_VtxGrowPending = true;

	if (!g_GfxVtxOverflowReported) {
		g_GfxVtxOverflowReported = true;
		sysLogPrintf(LOG_WARNING, "gfx: stage 0x%02x overran the vtx pool in %s: %u bytes past a %u byte buffer (given out from its guard, not over other memory; growing it)",
				mainGetStageNum(), what, past, g_VtxNominal);
	}
}

/**
 * Hand out size bytes of this frame's vtx memory: from the pool, from the
 * guard past it, or from the scratch block. Never a pointer past the end of
 * what was allocated.
 */
static void *gfxVtxTake(u32 size, const char *what)
{
	u8 *ptr = g_GfxMemPos;

	if ((uintptr_t)(g_VtxBuffers[g_GfxActiveBufferIndex + 1] - ptr) >= size) {
		g_GfxMemPos += size;

		if (g_GfxMemPos > g_VtxEnd) {
			gfxVtxReportOverflow(what, (u32)(g_GfxMemPos - g_VtxEnd));
		}

		return ptr;
	}

	// past the guard too: a scratch block every such allocation shares, so
	// the frame draws garbage where it ran out, and nothing else is written
	g_VtxSinkDemand += size;
	gfxVtxReportOverflow(what, (u32)(g_GfxMemPos - g_VtxEnd) + size);

	if (size > g_VtxSinkSize) {
		u32 sinksize = ALIGN16(size > g_VtxSinkSize * 2 ? size : g_VtxSinkSize * 2);
		u8 *sink = gfxVtxHeapAlloc(sinksize);

		if (!sink) {
			sysFatalError("gfx: out of memory for %u bytes of vertex scratch", sinksize);
		}

		g_VtxSink = sink;
		g_VtxSinkSize = sinksize;
	}

	return g_VtxSink;
}

/**
 * At the buffer swap: when the frame just built ran past the pool, or went
 * without for want of it, the pool grows to what it wanted and half again.
 * The frame just built is drawn after this from the old block, which is kept.
 */
static void gfxVtxGrow(void)
{
	u32 demand = (u32)(g_GfxMemPos - g_VtxBuffers[g_GfxActiveBufferIndex]) + g_VtxSinkDemand;
	u32 nominal = demand + demand / 2;
	u32 guard;
	u8 *base;

	g_VtxGrowPending = false;
	g_VtxSinkDemand = 0;

	if (nominal < g_VtxNominal + g_VtxNominal / 2) {
		nominal = g_VtxNominal + g_VtxNominal / 2;
	}

	nominal = (nominal + VTX_GROW_STEP - 1) / VTX_GROW_STEP * VTX_GROW_STEP;

	if (nominal > VTX_GROW_MAX || nominal <= g_VtxNominal) {
		return;
	}

	guard = gfxVtxGuardFor(nominal);
	base = gfxVtxHeapAlloc((nominal + guard) * NUM_GFXTASKS);

	if (!base) {
		sysLogPrintf(LOG_WARNING, "gfx: could not grow the vtx pool to %u bytes", nominal);
		return;
	}

	sysLogPrintf(LOG_NOTE, "gfx: stage 0x%02x: vtx pool grown from %u to %u bytes a buffer (the frame wanted %u)",
			mainGetStageNum(), g_VtxNominal, nominal, demand);

	g_VtxPrevStart = g_VtxBuffers[0];
	g_VtxPrevEnd = g_VtxBuffers[NUM_GFXTASKS];
	gfxVtxSetBuffers(base, nominal, guard);
}
#endif

void gfxCheckGfxPool(const Gfx *gdl)
{
	static bool reported = false;
	const Gfx *end = (const Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex + 1];

#ifndef PLATFORM_N64
	g_GfxTraceGfxUsed = (u32)(gdl - (const Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex]);
	g_GfxTraceGfxSize = (u32)(end - (const Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex]);
#endif

	if (gdl > end && !reported) {
		reported = true;
		sysLogPrintf(LOG_ERROR, "gfx: stage 0x%02x overran the master display list: %d commands past a %d command buffer",
				mainGetStageNum(), (s32)(gdl - end),
				(s32)((const Gfx *)g_GfxBuffers[1] - (const Gfx *)g_GfxBuffers[0]));
	}
}

Vtx *gfxAllocateVertices(u32 count)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += count * sizeof(Vtx);
	g_GfxMemPos = (u8 *)ALIGN16((uintptr_t)g_GfxMemPos);
	gfxCheckVtxPool("vertices");

	return ptr;
#else
	// the same bump as the N64's: the size, then the position to 16
	uintptr_t pos = (uintptr_t)g_GfxMemPos;

	return gfxVtxTake((u32)(ALIGN16(pos + (uintptr_t)count * sizeof(Vtx)) - pos), "vertices");
#endif
}

void *gfxAllocateMatrix(void)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += sizeof(Mtx);
	gfxCheckVtxPool("a matrix");

	return ptr;
#else
	return gfxVtxTake(sizeof(Mtx), "a matrix");
#endif
}

#ifndef PLATFORM_N64
/**
 * Whether the vtx pool can still give out this much.
 *
 * Callers of gfxAllocate() do not check what they get back, and cannot: the
 * pool never used to run out. Anything that can now ask for a lot of it - one
 * chr's matrices, once per chr per frame - asks here first and goes without,
 * rather than being handed a pointer past the end of the pool.
 */
bool gfxHasVtxSpace(u32 size)
{
	// Compared as pointers rather than as an unsigned difference: the callers
	// that do not check can already have pushed g_GfxMemPos past the end of
	// the pool, and a negative difference cast to u32 is a very large one.
	// The usable end, not the guard's: the guard is for callers that cannot
	// go without. A caller told no makes the pool grow for the next frame.
	if (g_GfxMemPos + size <= g_VtxEnd) {
		return true;
	}

	g_VtxGrowPending = true;
	g_VtxSinkDemand += size;

	return false;
}
#endif

/**
 * sizeof(LookAt) is 0x10 and it consists of two Light structs of 0x8 each.
 * The function allocates 0x8 for every count, so it could be allocating lights
 * instead, however it's only used for LookAts so it's named as LookAt.
 */
LookAt *gfxAllocateLookAt(s32 count)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += count * (sizeof(LookAt) / 2);
	gfxCheckVtxPool("a lookat");

	return ptr;
#elif defined(PLATFORM_64BIT)
	return gfxVtxTake(count * (sizeof(LookAt) * 2), "a lookat");
#else
	return gfxVtxTake(count * (sizeof(LookAt) / 2), "a lookat");
#endif
}

Col *gfxAllocateColours(s32 count)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	count = ALIGN16(count * sizeof(Col));
	g_GfxMemPos += count;
	gfxCheckVtxPool("colours");

	return ptr;
#else
	return gfxVtxTake(ALIGN16(count * sizeof(Col)), "colours");
#endif
}

void *gfxAllocate(u32 size)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	size = ALIGN16(size);
	g_GfxMemPos += size;
	gfxCheckVtxPool("a general allocation");

	return ptr;
#else
	return gfxVtxTake(ALIGN16(size), "a general allocation");
#endif
}

void gfxSwapBuffers(void)
{
#ifndef PLATFORM_N64
	g_GfxTraceVtxUsed = (u32)(g_GfxMemPos - g_VtxBuffers[g_GfxActiveBufferIndex]);
	g_GfxTraceVtxSize = g_VtxNominal;

	if (g_VtxGrowPending) {
		gfxVtxGrow();
	}
#endif
	g_GfxActiveBufferIndex ^= 1;
	g_GfxRequestedDisplayList = false;
	g_GfxMemPos = g_VtxBuffers[g_GfxActiveBufferIndex];
#ifndef PLATFORM_N64
	g_VtxEnd = g_GfxMemPos + g_VtxNominal;
	g_VtxSinkDemand = 0;
	xblaMeshFrameReset();
#endif
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
#ifndef PLATFORM_N64
	return g_GfxMemPos < g_VtxEnd ? (u32)(g_VtxEnd - g_GfxMemPos) : 0;
#endif
	return g_VtxBuffers[g_GfxActiveBufferIndex + 1] - g_GfxMemPos;
}

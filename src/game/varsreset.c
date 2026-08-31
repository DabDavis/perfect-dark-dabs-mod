#include <ultra64.h>
#include "constants.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"

void varsResetRoomProps(void);

void varsReset(void)
{
	s32 i;

	g_Vars.props = mempAlloc(ALIGN64(g_Vars.maxprops * sizeof(struct prop)), MEMPOOL_STAGE);
	g_Vars.onscreenprops = mempAlloc(ALIGN64((MAX_ONSCREENPROPS + 1) * sizeof(void *)), MEMPOOL_STAGE);

#ifndef PLATFORM_N64
	// One stamp per prop, so roomGetProps() can tell whether it has already
	// written a propnum without rescanning what it has written. See prop.c.
	roomPropSeenAlloc(g_Vars.maxprops);
#endif

	g_AutoAimScale = 1;

	g_Vars.activeprops = g_Vars.activepropstail = NULL;
	g_Vars.pausedprops = NULL;

	g_Vars.numonscreenprops = 0;
	g_Vars.onscreenprops[0] = NULL;
	g_Vars.endonscreenprops = g_Vars.onscreenprops;

	g_Vars.freeprops = g_Vars.props;

	// @bug: The tail of the freeprops list will have an uninitialised next pointer.
	// This will likely crash the game if too many props get allocated,
	// but there is no known way to exhaust the free props list.
	for (i = 0; i < g_Vars.maxprops - 1; i++) {
		g_Vars.props[i].next = &g_Vars.props[i + 1];
	}

#ifndef PLATFORM_N64
	// There is a way now. Eighty simulants dying into bodies that stay where
	// they fell, each one a prop, will find the end of this list, and what
	// propAllocate() hands out at that point is whatever the last prop's next
	// pointer happened to contain - a non-NULL pointer to nothing, which every
	// caller's "if (prop)" check waves through. Terminating the list turns that
	// into the NULL they all already handle.
	g_Vars.props[g_Vars.maxprops - 1].next = NULL;
#endif

	varsResetRoomProps();

	if (g_Vars.normmplayerisrunning) {
		g_Vars.numpropstates = 4;
	} else {
		g_Vars.numpropstates = 7;
	}

	g_Vars.allocstateindex = 0;
	g_Vars.runstateindex = 0;
	g_Vars.alwaystick = 0;
	g_Vars.updateframe = 0xfffe;
	g_Vars.prevupdateframe = 0xffff;

	for (i = 0; i < ARRAYCOUNT(g_Vars.propstates); i++) {
		g_Vars.propstates[i].propcount = 0;
		g_Vars.propstates[i].chrpropcount = 0;
		g_Vars.propstates[i].updatetime = 0;
		g_Vars.propstates[i].chrupdatetime = 0;
		g_Vars.propstates[i].slotupdate240 = 0;
		g_Vars.propstates[i].slotupdate60error = 2;
	}
}

void varsResetRoomProps(void)
{
	s32 i;
	s32 j;

	g_RoomPropListChunkIndexes = mempAlloc(ALIGN16(g_Vars.roomcount * sizeof(s16)), MEMPOOL_STAGE);
	g_RoomPropListChunks = mempAlloc(MAX_ROOMPROPLISTCHUNKS * sizeof(struct roomproplistchunk), MEMPOOL_STAGE);

	for (i = 0; i < g_Vars.roomcount; i++) {
		g_RoomPropListChunkIndexes[i] = -1;
	}

	for (i = 0; i < MAX_ROOMPROPLISTCHUNKS; i++) {
		g_RoomPropListChunks[i].propnums[0] = -2;

		for (j = 1; j < ARRAYCOUNT(g_RoomPropListChunks[i].propnums); j++) {
			g_RoomPropListChunks[i].propnums[j] = -1;
		}
	}
}

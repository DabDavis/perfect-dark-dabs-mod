#include <ultra64.h>
#include "constants.h"
#include "game/bg.h"
#include "game/pad.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/anim.h"
#include "data.h"
#include "types.h"

void roomsReset(void)
{
	s32 i;

	g_RoomMtxNumSlots = PLAYERCOUNT() >= 2 ? 200 : 120;

#ifndef PLATFORM_N64
	// roomAllocateMtx() answers slot 0 when every slot is taken, and a list
	// names its matrix by address - so every room past the cache is drawn
	// with whichever of them was written last, and so is the room that holds
	// slot 0 honestly, which is the camera's. The portal walk never puts 120
	// rooms on screen; the spectator and a GoldenEye XBLA level draw every
	// room, and Dam has 136: its HD look had no ground under the player and
	// its cliffs stood in the wrong places.
	//
	// A room needs **NUM_GFXTASKS slots, not one**: roomFreeMtx() sets a
	// slot's age to NUM_GFXTASKS and the tick counts it down, so a slot let go
	// of is unavailable for that many frames while the graphics task that
	// still names it finishes. Sized at one slot a room, a level that draws
	// all of them ran out the moment the camera moved and any room changed
	// hands - which on Dam took the whole guard hut at the top of the dam and
	// the ground under it, while the door and the window in its wall, which
	// are props, stayed in the air.
	{
		const s32 wanted = (g_Vars.roomcount + 1) * PLAYERCOUNT() * NUM_GFXTASKS;

		if (g_RoomMtxNumSlots < wanted) {
			g_RoomMtxNumSlots = wanted;
		}
	}
#endif

	g_RoomMtxAges = mempAlloc(ALIGN16(g_RoomMtxNumSlots), MEMPOOL_STAGE);
	g_RoomMtxLinkedRooms = mempAlloc(ALIGN16(g_RoomMtxNumSlots * sizeof(*g_RoomMtxLinkedRooms)), MEMPOOL_STAGE);
	g_RoomMtxBaseRooms = mempAlloc(ALIGN16(g_RoomMtxNumSlots * sizeof(*g_RoomMtxBaseRooms)), MEMPOOL_STAGE);
	g_RoomMtxScales = mempAlloc(ALIGN16(g_RoomMtxNumSlots * sizeof(*g_RoomMtxScales)), MEMPOOL_STAGE);
	g_RoomMtxMatrices = mempAlloc(ALIGN16(g_RoomMtxNumSlots * sizeof(*g_RoomMtxMatrices)), MEMPOOL_STAGE);

	for (i = 0; i < PLAYERCOUNT(); i++) {
		g_Vars.players[i]->lastroomforoffset = -1;
	}

	for (i = 0; i < g_RoomMtxNumSlots; i++) {
		g_RoomMtxLinkedRooms[i] = -1;
		g_RoomMtxAges[i] = 2;
		g_RoomMtxBaseRooms[i] = -1;
		g_RoomMtxScales[i] = 1;
	}

	for (i = 0; i < g_Vars.roomcount; i++) {
		g_Rooms[i].roommtxindex = -1;
	}
}

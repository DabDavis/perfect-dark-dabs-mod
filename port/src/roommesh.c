/**
 * The level's rooms drawn from the renderer's copy of their vertices on the
 * GPU (G_MESH_EXT, gfxmesh.h, and "Rooms" in CLAUDE-notes/gpu-vertices.md).
 *
 * Every room's opaque and translucent passes (bgRenderRoomOpaque(),
 * bgRenderRoomXlu()) are bracketed by a gSPMeshEXT() naming the room's whole
 * vertex array and its colour table as roomHighlight() made it this frame.
 * That holds for every kind of room bg.c loads - the ROM's, the XBLA
 * release's (xblastage.c) and an HD level's (gebeanstage.c) - since all three
 * are drawn the same way, a block at a time with the block's vertices and
 * colours in segments 14 and 13 and its list behind them.
 *
 * The renderer keeps a room's copy until roomMeshForget(): when the room is
 * unloaded, or its vertices are rewritten in place.
 */

#ifndef PLATFORM_N64

#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "game/dlights.h"
#include "game/gfxmemory.h"
#include "gfxmesh.h"
#include "roommesh.h"

// One per room of the stage, grown with the room count
static struct gfxmesh *roomMeshes;
static s32 roomMeshCount;
static s32 roomMeshOpen;

static struct gfxmesh *roomMeshFor(s32 roomnum)
{
	if (roomnum <= 0 || roomnum >= g_Vars.roomcount) {
		return NULL;
	}

	// Grown to the stage's whole count at the first ask, never part way
	// through a frame's list: draws already in it point into the array
	if (g_Vars.roomcount > roomMeshCount) {
		s32 count = g_Vars.roomcount;
		struct gfxmesh *grown = realloc(roomMeshes, sizeof(*grown) * count);

		if (!grown) {
			return NULL;
		}

		memset(&grown[roomMeshCount], 0, sizeof(*grown) * (count - roomMeshCount));
		roomMeshes = grown;
		roomMeshCount = count;
	}

	return &roomMeshes[roomnum];
}

Gfx *roomMeshBegin(Gfx *gdl, s32 roomnum)
{
	struct roomgfxdata *gfx;
	struct gfxmesh *m;
	struct gfxmeshdraw *draw;

	roomMeshOpen = 0;

	if (!gfxMeshGpuAvailable() || (m = roomMeshFor(roomnum)) == NULL
			|| !g_Rooms[roomnum].loaded240 || (gfx = g_Rooms[roomnum].gfxdata) == NULL
			|| !gfx->vertices || gfx->numvertices <= 0 || gfx->numcolours <= 0) {
		return gdl;
	}

	// The frame's colours, which bgRenderRoomPass() would make at its first
	// block: once a frame, so asking first changes nothing
	roomHighlight(roomnum);

	if (m->vertices != gfx->vertices || m->numvertices != gfx->numvertices) {
		gfxMeshForget(m);
		memset(m, 0, sizeof(*m));
		m->vertices = gfx->vertices;
		m->numvertices = gfx->numvertices;
		m->room = 1;
	}

	m->dynamic = (g_Rooms[roomnum].flags & ROOMFLAG_HASDYNTEX) != 0;

	draw = gfxAllocate(sizeof(*draw));
	draw->mesh = m;
	draw->palette = NULL;
	draw->colours = g_Rooms[roomnum].colours
		? (const void *)g_Rooms[roomnum].colours
		: (const void *)ALIGN8((uintptr_t)&gfx->vertices[gfx->numvertices]);
	draw->numcolours = gfx->numcolours;

	gSPMeshEXT(gdl++, draw);
	roomMeshOpen = 1;

	return gdl;
}

Gfx *roomMeshEnd(Gfx *gdl)
{
	if (roomMeshOpen) {
		gSPMeshEXT(gdl++, NULL);
		roomMeshOpen = 0;
	}

	return gdl;
}

void roomMeshForget(s32 roomnum)
{
	if (roomnum > 0 && roomnum < roomMeshCount && roomMeshes[roomnum].vertices) {
		gfxMeshForget(&roomMeshes[roomnum]);
		memset(&roomMeshes[roomnum], 0, sizeof(roomMeshes[roomnum]));
	}
}

#endif

#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "geroom.h"
#include "system.h"
#include "gestan.h"
#include "game/bg.h"
#include "game/prop.h"
#include "lib/collision.h"
#include "lib/lib_17ce0.h"

#ifndef PLATFORM_N64

#define GEROOM_BATCH 8
#define GEROOM_NOGROUND -1000000.0f

s32 geRoomActive(void)
{
	return modloaderStageIsRemake(g_Vars.stagenum);
}

struct geroomfloor {
	f32 ground;
	u16 floorcol;
	u8 floortype;
	u16 floorflags;
	RoomNum floorroom;
	s32 inlift;
	struct prop *lift;
};

/** The ground from one batch of rooms, kept if it is the highest yet. */
static void geRoomAsk(struct coord *pos, f32 radius, RoomNum *rooms, struct geroomfloor *best)
{
	struct geroomfloor got = { 0 };

	got.floorroom = -1;
	got.ground = cdFindGroundInfoAtCyl(pos, radius, rooms, &got.floorcol, &got.floortype,
			&got.floorflags, &got.floorroom, &got.inlift, &got.lift);

	if (got.ground > best->ground) {
		*best = got;
	}
}

f32 geRoomGround(struct coord *pos, f32 radius, RoomNum *rooms, u16 *floorcol, u8 *floortype,
		u16 *floorflags, RoomNum *floorroom, s32 *inlift, struct prop **lift)
{
	struct geroomfloor best = { 0 };
	RoomNum batch[GEROOM_BATCH + 1];
	s32 count = 0;

	best.ground = -4294967296.0f;
	best.floorroom = -1;

	// the rooms they are said to be in first, which is the whole of the answer
	// wherever the tiles and the portals agree, and carries any lift
	geRoomAsk(pos, radius, rooms, &best);

	// and then every room whose box holds the position. A room's box holds its
	// own tiles (the conversion sees to it) and is grown to its portals at the
	// load, so the tile underfoot is in one of these whichever room it is. They
	// go eight at a time because cdCollectGeoForCyl() keeps twenty geos however
	// many rooms it is handed, and thirteen of Dam's boxes meet over its tower
	// stair. The highest floor *below* the position is the one stood on, so a
	// storey above or below does not take it.
	for (s32 r = 1; r < g_Vars.roomcount; r++) {
		if (!bgRoomContainsCoord(pos, r)) {
			continue;
		}

		batch[count++] = r;

		if (count == GEROOM_BATCH) {
			batch[count] = -1;
			geRoomAsk(pos, radius, batch, &best);
			count = 0;
		}
	}

	if (count > 0) {
		batch[count] = -1;
		geRoomAsk(pos, radius, batch, &best);
	}

	if (best.ground > GEROOM_NOGROUND) {
		if (floorcol) {
			*floorcol = best.floorcol;
		}
		if (floortype) {
			*floortype = best.floortype;
		}
		if (floorflags) {
			*floorflags = best.floorflags;
		}
		if (floorroom) {
			*floorroom = best.floorroom;
		}
	}

	if (inlift) {
		*inlift = best.inlift;
	}
	if (lift) {
		*lift = best.lift;
	}

	return best.ground;
}


s32 geRoomCamera(struct coord *eye, f32 ground, s32 room)
{
	struct coord foot = *eye;
	s32 last = -1;

	foot.y = ground;

	if (g_BgPortals == NULL) {
		return room;
	}

	// GoldenEye's own loop (bg.c, bgRoomVisibilityRelated()): eleven
	// crossings at most, never the same portal twice running, and an upright
	// portal - a doorway - is not asked at all, since the line is a plumb one
	for (s32 depth = 0; depth < 11; depth++) {
		s32 p;

		for (p = 0; g_BgPortals[p].verticesoffset != 0; p++) {
			const struct coord *n = &g_PortalMetrics[p].normal;

			if (p == last || n->x * n->x + n->z * n->z >= 0.999f) {
				continue;
			}

			if ((room == g_BgPortals[p].roomnum1 || room == g_BgPortals[p].roomnum2)
					&& portalCalculateIntersection(p, eye, &foot) != PORTALINTERSECTION_NONE) {
				last = p;
				room = room == g_BgPortals[p].roomnum1 ? g_BgPortals[p].roomnum2 : g_BgPortals[p].roomnum1;
				break;
			}
		}

		if (g_BgPortals[p].verticesoffset == 0) {
			break;
		}
	}

	return room;
}

s32 geRoomCutsceneCamera(struct coord *campos, struct coord *padpos, s32 padroom)
{
	s32 room;
	f32 ground;

	// bondviewSetCurrentPlayerPosition(): the camera's tile is walked to from
	// its pad's, and the room is that tile's carried up the plumb line to the
	// camera like any other eye's. Dam's ending looks back at the dam from out
	// over the valley: the walk stops on the brink, which is the dam's face
	if (!geStanWalk(padpos, campos, &room, &ground) || room <= 0 || room >= g_Vars.roomcount) {
		return padroom;
	}

	return geRoomCamera(campos, ground, room);
}

void geRoomDoorPortalRooms(struct prop *prop, s32 portalnum)
{
	const RoomNum want[2] = { g_BgPortals[portalnum].roomnum1, g_BgPortals[portalnum].roomnum2 };
	const RoomNum was = prop->rooms[0];
	bool changed = false;
	s32 k;

	for (k = 0; k < 2; k++) {
		s32 i;

		if (want[k] < 0 || want[k] >= g_Vars.roomcount) {
			continue;
		}

		for (i = 0; i < 7 && prop->rooms[i] != -1 && prop->rooms[i] != want[k]; i++);

		// already there, or the list is full
		if (i >= 7 || prop->rooms[i] != -1) {
			continue;
		}

		if (!changed) {
			propDeregisterRooms(prop);
			changed = true;
		}

		prop->rooms[i] = want[k];
		prop->rooms[i + 1] = -1;

		sysLogPrintf(LOG_NOTE, "gexplus: door on portal %d also drawn in room %d (it was room %d's)",
				portalnum, want[k], was);
	}

	if (changed) {
		propRegisterRooms(prop);
	}
}

f32 geRoomPortalThickness(s32 portalnum)
{
	const u8 code = g_BgPortals[portalnum].gethickness;

	return (code & 0xf) * 0.25f * (f32)(1u << (code >> 4));
}


#endif

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
// how far the conversion can move a floor off GoldenEye's: one rounding to a
// whole world unit (tools/geconvert/geconvert.py, write_tiles()/write_stan())
#define GEROOM_FLOOR_ROUNDING 0.5f
// geRoomDoorSideRooms(): where the floor either side of a door is sampled, past
// the leaf's face - near, and a body's width out - and how far under the leaf's
// foot the floor may sit and still be the one it stands on
#define GEROOM_DOORSIDE_NEAR 20.0f
#define GEROOM_DOORSIDE_FAR  60.0f
#define GEROOM_DOORSIDE_SINK 40.0f

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

	// The room is the tile's under the middle of the body, as GoldenEye's is.
	// The search above takes it from whichever floor in the body's circle is
	// highest, and where two rooms' floors meet level at a doorway that is the
	// first room asked - the one the body was in. On Dam's tunnel under the
	// dam (rooms 92 and 93, the upright portal 171 between them) a player
	// twenty units past the portal into 93, with their circle still over 92's
	// floor, stayed in 92 and drew from it: 92 is all behind the portal, so
	// the screen was sky (F3 report 20260926-111451). GoldenEye has them on
	// 93's tile and draws the tunnel.
	if (floorroom && best.ground > GEROOM_NOGROUND && !best.inlift) {
		const s32 tileroom = geStanRoomUnder(pos, best.ground, best.floorroom);

		if (tileroom > 0 && tileroom < g_Vars.roomcount) {
			best.floorroom = tileroom;
		}
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

void geRoomAddNear(struct coord *pos, f32 radius, f32 ymin, f32 ymax, RoomNum *rooms, s32 maxlen)
{
	s32 len;

	for (len = 0; rooms[len] != -1; len++);

	for (s32 r = 1; r < g_Vars.roomcount && len < maxlen; r++) {
		s32 k;

		if (pos->x + radius < g_Rooms[r].bbmin[0] || pos->x - radius > g_Rooms[r].bbmax[0]
				|| pos->z + radius < g_Rooms[r].bbmin[2] || pos->z - radius > g_Rooms[r].bbmax[2]
				|| pos->y + ymax < g_Rooms[r].bbmin[1] || pos->y + ymin > g_Rooms[r].bbmax[1]) {
			continue;
		}

		for (k = 0; k < len && rooms[k] != r; k++);

		if (k == len) {
			rooms[len++] = r;
			rooms[len] = -1;
		}
	}
}


/**
 * Whether the plumb line from the foot, standing in `room`, up to the eye goes
 * through portal `p`.
 *
 * A floor laid in a floor portal's plane is the case that needs care. In
 * GoldenEye it is exactly on the plane - its tiles and portals are in the same
 * whole bg units - and whether sub_GAME_7F0B9F14() calls that crossed is down
 * to the last bit of a float: Egyptian's fountain basin (room 14, under the
 * courtyard's portal 18) comes out crossed, Surface's room 14 (over its portal
 * 94 to room 33) not. The conversion keeps a portal's corners as they are and
 * rounds a tile to the nearest world unit, so our floor lies up to half a unit
 * to either side of the plane, and which side is the rounding's choice: the
 * basin's floor came out 0.335 over its portal, the line never crossed it, and
 * standing in the pool drew the basin alone against the sky (F3 report
 * 20260925-163132); the half unit taken the other way for every floor puts
 * Surface's players under their own floor.
 *
 * A floor within the rounding of the plane is taken as lying on its own room's
 * side of it, which is where a tile of that room is: the line crosses when the
 * eye is on the far side, and the room it arrives in is the one the eye is in.
 * That is GoldenEye's answer everywhere its float lands on the right side -
 * Egyptian, Archives and Aztec cross, Surface does not.
 */
static bool geRoomPlumbCrosses(s32 p, s32 room, struct coord *eye, struct coord *foot)
{
	const struct portalmetric *m = &g_PortalMetrics[p];
	const f32 at = m->normal.x * foot->x + m->normal.y * foot->y + m->normal.z * foot->z;
	struct coord onside = *foot;

	if (at > m->min - GEROOM_FLOOR_ROUNDING && at < m->max + GEROOM_FLOOR_ROUNDING
			&& g_BgPortals[p].roomnum1 != g_BgPortals[p].roomnum2) {
		// roomnum1 is behind the normal and roomnum2 in front of it: the walk
		// in bgConsumeSnakeItem() goes from roomnum1 only with the camera
		// not in front
		const f32 to = room == g_BgPortals[p].roomnum1
			? m->min - GEROOM_FLOOR_ROUNDING
			: m->max + GEROOM_FLOOR_ROUNDING;

		onside.x += m->normal.x * (to - at);
		onside.y += m->normal.y * (to - at);
		onside.z += m->normal.z * (to - at);
	}

	return portalCalculateIntersection(p, eye, &onside) != PORTALINTERSECTION_NONE;
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
					&& geRoomPlumbCrosses(p, room, eye, &foot)) {
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

/** Adds `room` to a prop's list unless it is there or the list is full; deregisters first on the first change. */
static bool geRoomPropAdd(struct prop *prop, s32 room, bool *changed)
{
	s32 i;

	if (room < 0 || room >= g_Vars.roomcount) {
		return false;
	}

	for (i = 0; i < 7 && prop->rooms[i] != -1 && prop->rooms[i] != room; i++);

	// already there, or the list is full
	if (i >= 7 || prop->rooms[i] != -1) {
		return false;
	}

	if (!*changed) {
		propDeregisterRooms(prop);
		*changed = true;
	}

	prop->rooms[i] = room;
	prop->rooms[i + 1] = -1;

	return true;
}

void geRoomDoorSideRooms(struct prop *prop, struct pad *pad)
{
	// the pad's box is along its normal (x), up (y) and look (z); a door leaf
	// is thin along one of them, and that is the way through it
	const f32 ext[3] = {
		pad->bbox.xmax - pad->bbox.xmin,
		pad->bbox.ymax - pad->bbox.ymin,
		pad->bbox.zmax - pad->bbox.zmin,
	};
	const struct coord *axes[3] = { &pad->normal, &pad->up, &pad->look };
	const RoomNum was = prop->rooms[0];
	bool changed = false;
	s32 thin = 0;
	f32 bottom = 1e30f;
	s32 i, side, step;

	for (i = 1; i < 3; i++) {
		if (ext[i] < ext[thin]) {
			thin = i;
		}
	}

	// a door lying flat (Surface's grate, Train's floor panel) has floors
	// above and below it, not beside it
	if (axes[thin]->y > 0.7f || axes[thin]->y < -0.7f) {
		return;
	}

	// the leaf's lowest corner, where the floor either side of it is
	for (i = 0; i < 8; i++) {
		const f32 a = (i & 1) ? pad->bbox.xmax : pad->bbox.xmin;
		const f32 b = (i & 2) ? pad->bbox.ymax : pad->bbox.ymin;
		const f32 c = (i & 4) ? pad->bbox.zmax : pad->bbox.zmin;
		const f32 y = pad->pos.y + a * pad->normal.y + b * pad->up.y + c * pad->look.y;

		if (y < bottom) {
			bottom = y;
		}
	}

	for (side = -1; side <= 1; side += 2) {
		for (step = 0; step < 2; step++) {
			const f32 dist = ext[thin] * 0.5f + (step ? GEROOM_DOORSIDE_FAR : GEROOM_DOORSIDE_NEAR);
			struct coord pt;
			s32 room;

			pt.x = prop->pos.x + axes[thin]->x * dist * side;
			pt.y = bottom;
			pt.z = prop->pos.z + axes[thin]->z * dist * side;

			room = geStanRoomUnder(&pt, bottom + GEROOM_DOORSIDE_SINK, -1);

			if (geRoomPropAdd(prop, room, &changed)) {
				sysLogPrintf(LOG_NOTE, "gexplus: door on pad %d also in room %d, the floor beside it (it was room %d's)",
						prop->door ? prop->door->base.pad : -1, room, was);
			}
		}
	}

	if (changed) {
		propRegisterRooms(prop);
	}
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

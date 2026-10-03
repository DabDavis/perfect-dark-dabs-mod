#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "gexfront.h"
#include "geroom.h"
#include "system.h"
#include "gestan.h"
#include "game/bg.h"
#include "game/prop.h"
#include "lib/collision.h"
#include "lib/lib_17ce0.h"
#include "xblamesh.h"
#include "gebean.h"
#include "game/pad.h"
#include "game/setuputils.h"
#include "game/propobj.h"

#ifndef PLATFORM_N64

#define GEROOM_BATCH 8
#define GEROOM_NOGROUND -1000000.0f
// how far the conversion can move a floor off GoldenEye's: one rounding to a
// whole world unit (tools/geconvert/geconvert.py, write_tiles()/write_stan())
#define GEROOM_FLOOR_ROUNDING 0.5f

s32 geRoomActive(void)
{
	return modloaderStageIsRemake(g_Vars.stagenum);
}

/**
 * Whether a converted stage loads the Community Edition's copies of its files
 * (modloaderGetStageCeFile()): the release's look on and the Community
 * Edition applied, as the stage loads. The user, 2026-10-01: the Community
 * Edition's fixes belong to the HD look only, and the N64 look is the
 * cartridge as it is. Like everything a stage reads from its setup, pads and
 * tiles, the choice is made at the load: a switch of look in a mission takes
 * effect at the next one (or at a restart of it).
 */
s32 geRoomCeData(void)
{
	return xblaMeshGetEnabled() && gebeanCeIsActive();
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

/**
 * Complex's catwalk wall (geroom.h). The face is GoldenEye room 41's own,
 * four triangles on the plane x = -2009 of the converted level (offset
 * 241 0 602 from GoldenEye's world), 0 to 241 high and z 41 to 1246, in two
 * bands: room 44 lies behind it. Matched by the map, the room and all three corners in that slab, so
 * no other wall of the room - nor of any other level - is touched.
 */
bool geRoomTriPassesShots(s32 roomnum, struct coord *p1, struct coord *p2, struct coord *p3)
{
	static s32 stagenum = -1;
	static bool complex = false;
	struct coord *pts[3] = { p1, p2, p3 };

	if (roomnum != 41) {
		return false;
	}

	if (stagenum != g_Vars.stagenum) {
		const char *map = modloaderGetStageMapName(g_Vars.stagenum);
		const char *dir = modloaderGetStageModDir(g_Vars.stagenum);

		stagenum = g_Vars.stagenum;
		complex = map && dir && strcmp(map, "Complex") == 0 && strstr(dir, "GoldenEye Arenas") != NULL;
	}

	if (!complex || !geRoomActive()) {
		return false;
	}

	for (s32 i = 0; i < 3; i++) {
		if (pts[i]->x < -2010 || pts[i]->x > -2008
				|| pts[i]->y < -1 || pts[i]->y > 242
				|| pts[i]->z < 40 || pts[i]->z > 1247) {
			return false;
		}
	}

	return true;
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
	struct coord centre;
	struct coord pt1;
	struct coord pt2;
	bool changed = false;
	s32 thin = 0;
	s32 rooms[2];
	s32 i;

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

	// GoldenEye's own rooms for the door (prop.c's setupDoor() through
	// sub_GAME_7F00324C()): from the pad's tile to the middle of the box,
	// then fifty either way along the door's normal over the tile graph,
	// each walk stopping at an edge with no tile beyond it. Sampling the
	// floor under points past each face instead found floors the graph never
	// reaches: Frigate's six doors set in the ends of corridors (pads 70, 71,
	// 74, 85, 86, 89) have no portal and stand in rooms of their own behind
	// the corridor wall, which GoldenEye never draws from the corridor - and
	// the floor sample filed each under the corridor too, so a door stood in
	// every one of those alcoves (F3 20260929-095345, -095411, -095420,
	// -095435, "this door doesn't exist in the original game").
	centre.x = pad->pos.x + ((pad->bbox.xmin + pad->bbox.xmax) * pad->normal.x
			+ (pad->bbox.ymin + pad->bbox.ymax) * pad->up.x
			+ (pad->bbox.zmin + pad->bbox.zmax) * pad->look.x) * 0.5f;
	centre.y = pad->pos.y + ((pad->bbox.xmin + pad->bbox.xmax) * pad->normal.y
			+ (pad->bbox.ymin + pad->bbox.ymax) * pad->up.y
			+ (pad->bbox.zmin + pad->bbox.zmax) * pad->look.y) * 0.5f;
	centre.z = pad->pos.z + ((pad->bbox.xmin + pad->bbox.xmax) * pad->normal.z
			+ (pad->bbox.ymin + pad->bbox.ymax) * pad->up.z
			+ (pad->bbox.zmin + pad->bbox.zmax) * pad->look.z) * 0.5f;

	if (!geStanDoorSideRooms(&pad->pos, &centre, &pad->normal, &rooms[0], &rooms[1], &pt1, &pt2)) {
		return;
	}

	for (i = 0; i < 2; i++) {
		if (geRoomPropAdd(prop, rooms[i], &changed)) {
			sysLogPrintf(LOG_NOTE, "gexplus: door on pad %d also in room %d, GoldenEye's room beside it (it was room %d's)",
					prop->door ? prop->door->base.pad : -1, rooms[i], was);
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

void geRoomRevisionPortals(s32 stagenum)
{
	const u16 *portals;
	const u8 *codes;
	const s32 n = gexFrontGetRevisionFixes() ? modloaderGetStageRevisionPortals(stagenum, &portals, &codes) : 0;
	s32 numportals = 0;

	if (n <= 0 || !g_BgPortals) {
		return;
	}

	while (g_BgPortals[numportals].verticesoffset != 0) {
		numportals++;
	}

	for (s32 i = 0; i < n; i++) {
		if (portals[i] < numportals) {
			g_BgPortals[portals[i]].gethickness = codes[i];
		}
	}

	sysLogPrintf(LOG_NOTE, "gexplus: %d portals as GoldenEye's PAL cartridge has them", n);
}


#endif

/**
 * A converted object's rooms the way GoldenEye counts them at its setup
 * (loadobjectmodel.c's setupUpdateObjectRoomPosition() into chrprop.c's
 * chrpropUpdateRoomList()): the rooms of the tiles round its prop->pos -
 * within the box's widest reach from it, thirty more each way across - walked
 * from its prop->stan (geStanObjectTile()), then every room a portal that is
 * not shut opens onto, where the portal's own box meets the object's box
 * widened thirty across (bg.c's sub_GAME_7F0BA2D4()), at most seven. An
 * object with GoldenEye's flags2 0x20000 is in its tile's room alone.
 * Perfect Dark counted the rooms its box entered from its origin's
 * (bgFindEnteredRooms()), one room more or fewer than GoldenEye's on ~300
 * objects over the twenty missions (FINDINGS row 18). Placement and collision
 * stay on the object's position, which is GoldenEye's runtime_pos.
 */
void geRoomObjRooms(struct defaultobj *obj)
{
	struct prop *prop = obj->prop;
	struct modelrodata_bbox *bbox;
	struct coord min, max, seed, centre;
	bool haveCentre = false;
	struct pad pad;
	s32 rooms[8];
	s32 count, tile;
	f32 radius = 0.0f;
	f32 rot[3][3];
	RoomNum out[8];

	if (!prop || prop->parent || obj->pad < 0) {
		return;
	}

	padUnpack(obj->pad, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_FLAGS, &pad);

	// a bound pad's object starts from its box's middle, unless it is placed
	// at its pad (GoldenEye's flags2 & 1)
	if ((pad.flags & PADFLAG_HASBBOXDATA) && !(obj->flags2 & 1)) {
		padGetCentre(obj->pad, &centre);
		haveCentre = true;
	}

	if (!geStanObjectTile(&pad.pos, pad.room, haveCentre ? &centre : NULL, &prop->pos, &tile, &seed)) {
		return;
	}

	if (obj->flags2 & 0x00020000) {
		rooms[0] = geStanTileRoom(tile);
		count = rooms[0] > 0 ? 1 : 0;
	} else {
		bbox = objFindBboxRodata(obj);

		if (!bbox) {
			return;
		}

		// The box over the object's turn as GoldenEye has it: Perfect Dark
		// turns a converted object a quarter by 1.5705463 rather than pi/2
		// (the decomp's M_BADPI and its 4.7116389), which leaves 2.5e-4 of
		// the other axes in a row that should hold none and tips a pane's
		// box 0.06 under a floor portal it stands on exactly - on the
		// cartridge it is 0.01 over (Dam's windows, traced on ares), and
		// the portal's room was taken. Such slivers are taken as nought.
		{
			f32 big = 0.0f;

			for (s32 r = 0; r < 3; r++) {
				for (s32 c = 0; c < 3; c++) {
					if (fabsf(obj->realrot[r][c]) > big) {
						big = fabsf(obj->realrot[r][c]);
					}
				}
			}

			for (s32 r = 0; r < 3; r++) {
				for (s32 c = 0; c < 3; c++) {
					rot[r][c] = fabsf(obj->realrot[r][c]) < big * 0.002f ? 0.0f : obj->realrot[r][c];
				}
			}
		}

		min.x = prop->pos.x + objGetRotatedLocalXMinByMtx3(bbox, rot);
		min.y = prop->pos.y + objGetRotatedLocalYMinByMtx3(bbox, rot);
		min.z = prop->pos.z + objGetRotatedLocalZMinByMtx3(bbox, rot);
		max.x = prop->pos.x + objGetRotatedLocalXMaxByMtx3(bbox, rot);
		max.y = prop->pos.y + objGetRotatedLocalYMaxByMtx3(bbox, rot);
		max.z = prop->pos.z + objGetRotatedLocalZMaxByMtx3(bbox, rot);

		// the box about the object, thirty wider across, and its widest
		// reach across from the object's own place
		min.x -= prop->pos.x + 30.0f;
		min.z -= prop->pos.z + 30.0f;
		max.x -= prop->pos.x - 30.0f;
		max.z -= prop->pos.z - 30.0f;

		if (radius < -min.x) radius = -min.x;
		if (radius < -min.z) radius = -min.z;
		if (radius < max.x) radius = max.x;
		if (radius < max.z) radius = max.z;

		min.x += prop->pos.x;
		min.z += prop->pos.z;
		max.x += prop->pos.x;
		max.z += prop->pos.z;

		count = geStanLocusRooms(tile, seed.x, seed.z, radius, rooms, 7);
		count = geRoomPortalsOverBox(&min, &max, rooms, count, 7);
	}

	if (count <= 0) {
		return;
	}

	for (s32 i = 0; i < count; i++) {
		out[i] = rooms[i];
	}

	out[count] = -1;


	propDeregisterRooms(prop);
	roomsCopy(out, prop->rooms);
	propRegisterRooms(prop);
}

/**
 * bg.c's sub_GAME_7F0BA2D4(): to `rooms` every room a portal of one of them
 * leads to, where the portal is not shut and its box meets [min, max]; again
 * for the rooms so added, until nothing is added or there are `max`.
 */
#define GEROOM_TOUCH 0.02f

s32 geRoomPortalsOverBox(struct coord *min, struct coord *max, s32 *rooms, s32 count, s32 maxcount)
{
	s32 i = 0;
	s32 saved = count;

	while (true) {
		for (; i < saved; i++) {
			const s32 room = rooms[i];

			for (s32 p = 0; g_BgPortals[p].verticesoffset != 0; p++) {
				struct portalvertices *pv;
				f32 pmin[3] = { 3.4028235e38f, 3.4028235e38f, 3.4028235e38f };
				f32 pmax[3] = { -3.4028235e38f, -3.4028235e38f, -3.4028235e38f };
				s32 other, k;

				if ((g_BgPortals[p].flags & PORTALFLAG_CLOSED)
						|| (room != g_BgPortals[p].roomnum1 && room != g_BgPortals[p].roomnum2)) {
					continue;
				}

				pv = (struct portalvertices *)((uintptr_t)g_BgPortals + g_BgPortals[p].verticesoffset);

				for (s32 j = 0; j < pv->count; j++) {
					for (k = 0; k < 3; k++) {
						const f32 v = pv->vertices[j].f[k];

						if (v < pmin[k]) pmin[k] = v;
						if (pmax[k] < v) pmax[k] = v;
					}
				}

				// bgIsBboxOverlapping(), less boxes that only touch: a pane
				// standing on a floor portal is 0.01 over it on the cartridge
				// (Dam's windows, traced on ares) and level with it here, its
				// scale rounded the other way. A pane lying flat in a flat
				// portal's own plane is no touch but the portal itself, and
				// GoldenEye's test, which takes equal as over, files it in
				// both rooms: Control's floor of glass at the stairwell
				// (tinted glass 133, portal 75), 0.0004 under the portal
				// here, was in the lower room alone and never drawn from the
				// upper floor (F3 20261003-152909)
				for (k = 0; k < 3; k++) {
					if (pmax[k] - pmin[k] < GEROOM_TOUCH && max->f[k] - min->f[k] < GEROOM_TOUCH) {
						if (fabsf((min->f[k] + max->f[k]) - (pmin[k] + pmax[k])) * 0.5f > GEROOM_TOUCH) {
							break;
						}
					} else if (min->f[k] > pmax[k] - GEROOM_TOUCH || max->f[k] < pmin[k] + GEROOM_TOUCH) {
						break;
					}
				}

				if (k < 3) {
					continue;
				}

				other = room == g_BgPortals[p].roomnum1 ? g_BgPortals[p].roomnum2 : g_BgPortals[p].roomnum1;


				for (k = 0; k < count && rooms[k] != other; k++);

				if (k == count) {
					if (count < maxcount) {
						rooms[count++] = other;
					}

					if (count >= maxcount) {
						return count;
					}
				}
			}
		}

		if (count == saved) {
			break;
		}

		saved = count;
	}

	return count;
}

/** Whether the conversion gave the door GoldenEye's own portal and rooms (converter 97). */
bool geRoomDoorRoomsGiven(struct doorobj *door)
{
	return door->unusedmaybe[0] == GE_DOOR_PORTAL_GIVEN && door->unusedmaybe[1] != 0xff;
}

/**
 * The door's rooms as GoldenEye's setupDoor() sets them: its tile's room and
 * the side room, nothing else (prop->rooms[0], [1]). A door with neither
 * portal flag is in its tile's room alone, and GoldenEye draws it only from
 * there - Frigate's six doors at the corridors' ends (FINDINGS rows 18, 23).
 */
void geRoomDoorGivenRooms(struct prop *prop, struct doorobj *door)
{
	RoomNum rooms[3];
	s32 n = 0;

	rooms[n++] = door->unusedmaybe[1];

	if (door->unusedmaybe[2] != 0xff && door->unusedmaybe[2] != door->unusedmaybe[1]) {
		rooms[n++] = door->unusedmaybe[2];
	}

	rooms[n] = -1;

	propDeregisterRooms(prop);
	roomsCopy(rooms, prop->rooms);
	propRegisterRooms(prop);
}


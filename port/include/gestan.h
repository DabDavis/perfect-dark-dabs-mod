#ifndef _IN_GESTAN_H
#define _IN_GESTAN_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's own collision for a body, on a level converted from it
 * (port/src/gestan.c).
 *
 * GoldenEye's collision is two dimensional and is its tile graph and nothing
 * else: a body stands on a tile, a move is walked from tile to tile through the
 * edges that are *linked*, and what stops it is an edge that is not
 * (stan.c's stanTestLineUnobstructed(), stanTestVolume(),
 * walkTilesBetweenPoints()). So the only walls that exist for a body are the
 * unlinked edges of the tiles linked to the one it is on, as far out as it
 * reaches. A wall a storey up, or the other side of a rail on a tile it could
 * never step to, is not consulted at all.
 *
 * The conversion has no such graph. It raises a quad round every unlinked edge
 * and hands them all to Perfect Dark's collision, which tests every wall in the
 * room against a cylinder - so it has needed a rule for how high each wall may
 * stand before it blocks the storey above, another for how far its foot must
 * lift over a head below, and none of them could help where a wall of the
 * ground floor ends inside a player's radius of a stair: Dam's first guard
 * tower, which nobody could climb.
 *
 * So the graph is kept (the conversion's `bg_gx<level>_stan`, in the order the
 * walls were raised in) and asked the one question GoldenEye asks: **is this
 * wall's tile linked to the one the body stands on, within its reach?** Where it
 * is not, the wall is not there for that body. Everything Perfect Dark does
 * with a wall that *is* there - the edge, the slide along it, the push - is
 * left as it is, and so is everything that is not a body: a shot and a line of
 * sight ask for other flags and are not filtered.
 *
 * One kind of wall is not an unlinked edge: GoldenEye links a floor to one far
 * over it through tiles that stand on edge, and walks through them; the
 * conversion raises a wall on the low side of such a link (geconvert.c's
 * stanClimb()) and marks the link in the graph file, where it is a link still.
 */

/**
 * Whether a wall of the converted level is to be left out of a body's
 * collision test: the stage is a converted one, `geo` is one of its raised
 * walls, the body is over a tile, and the wall's tile is not linked to that one
 * within `reach` of `pos`.
 *
 * `limit` is how high a floor may be and still be the one stood on: the tile
 * taken is the highest under the position whose surface is at or under it, so a
 * flight passing over a body's head is not mistaken for its floor. `rise` is
 * how far over the limit the floor may be all the same, for a foot that lags
 * the stair it is climbing (geStanRise()).
 */
bool geStanWallSkipped(struct geo *geo, struct coord *pos, struct coord *to, f32 limit, f32 rise, f32 reach);

/**
 * Whether a wall of the converted level stands in a body's way however far
 * over its top the body is: GoldenEye's collision is the plan alone, and an
 * edge with nothing across it stops Bond at any height (stan.c's
 * sub_GAME_7F0B1DDC walks out from his tile through links, and an unlinked
 * edge his circle touches is a wall). True for a wall raised on an unlinked
 * edge of a floor tile the body over `pos` reaches (as geStanWallSkipped()
 * works that out), not for a climb wall or a tile on edge.
 */
bool geStanWallOverhead(struct geo *geo, struct coord *pos, f32 limit, f32 rise, f32 reach);

/**
 * The floor a player's move from `pos` to `to` ends on in GoldenEye's own
 * collision: its line walk (stanWalkLine(), links only) from the tile the
 * player was left on - GoldenEye keeps Bond's, current_tile_ptr - or, where
 * that no longer holds them in plan, the tile under them at `ground`. True, with the
 * floor's height at `to`, where the walk ends on a tile that holds `to`.
 *
 * GoldenEye lifts Bond onto that floor however far over his feet it is, up
 * to 175 over his eye. Perfect Dark's feet find only a floor a step over
 * them, and a floor joined by tiles that lean rather than stand straight up
 * has no climb wall raised before it (geconvert.c's stanClimb()): Facility's
 * vent ends in a lip leaning 10 across and 257 up from the toilet seat, the
 * player walked up it on Perfect Dark's feet, on under the vent, off the lip
 * and down to the stairs a storey below (F3 report 20260927-051458).
 */
bool geStanFloorAhead(s32 playernum, struct coord *pos, struct coord *to, f32 ground, f32 *surface, bool *sheer);

/** The player's tile is to be found afresh: they fell, climbed a ladder or rode. */
void geStanForgetPlayerTile(s32 playernum);

/**
 * The player whose own move the walls are asked about next, or -1 when done:
 * the tile their cylinder stands on is the one GoldenEye's walk from their
 * tile reaches (geStanFloorAhead() kept it), not the one nearest by height.
 */
void geStanSetMover(s32 playernum);

/** The `rise` for a body's cylinder: a couple of steps where the limit is its foot, else none. */
f32 geStanRise(bool checkvertical);

/** The `limit` for a body's cylinder: its foot where the test has one, else well under its middle. */
f32 geStanLimit(struct coord *pos, bool checkvertical, f32 ymin);

/**
 * GoldenEye's walk from tile to tile along a line in plan (stan.c's
 * walkTilesBetweenPoints()): from the tile under `from` towards `to`, through
 * linked edges only. Gives the room of the tile it ends on and that tile's
 * surface at `to`; false where the level has no graph or `from` is over no tile.
 */
bool geStanWalk(struct coord *from, struct coord *to, s32 *room, f32 *ground);

/**
 * geStanWalk() from a tile of room `fromroom` where `from` is on the edge
 * between it and another room's tile at the same height: GoldenEye starts from
 * the tile the pad names, and the conversion's pad room is that tile's.
 */
bool geStanDoorSideRooms(struct coord *padpos, struct coord *centre, struct coord *normal,
		s32 *room1, s32 *room2, struct coord *pt1, struct coord *pt2);
bool geStanWalkFromRoom(struct coord *from, s32 fromroom, struct coord *to, s32 *room, f32 *ground);

/**
 * The room of the tile under the middle of a body standing on the floor at
 * `ground` - GoldenEye's room for it (bondview2.c's current_tile_ptr, which
 * is the tile under the point, not under the circle) - the tile of room
 * `prefer` where the point is on a seam between two at the same height. -1
 * where the level has no graph, or no tile is under the point within a stair's
 * rise of the floor (the middle out over a drop).
 */
s32 geStanRoomUnder(struct coord *pos, f32 ground, s32 prefer);

/**
 * GoldenEye's prop->stan and prop->pos for an object set down from a pad: the
 * pad's tile walked to the object, or the pad's own where the walk fails.
 */
bool geStanObjectTile(struct coord *padpos, s32 padroom, struct coord *centre, struct coord *objpos,
		s32 *tile, struct coord *seed);

/** A tile's room, or -1. */
s32 geStanTileRoom(s32 tile);

/** The rooms of the tiles within `radius` of x/z, walking from `tile` (stan.c's sub_GAME_7F0B21B0()). */
s32 geStanLocusRooms(s32 tile, f32 x, f32 z, f32 radius, s32 *rooms, s32 max);

/**
 * Whether a guard standing at `from` on the floor at `ground` may run straight
 * to `to`, past waypoints: GoldenEye's walk along the tile graph from its tile
 * ends on the tile under `to` (sub_GAME_7F030128). True with no graph.
 */
bool geStanReaches(struct coord *from, f32 ground, struct coord *to);

/**
 * Whether a body could walk the straight line from `from`, on its floor at
 * `fromground`, to `to` on its floor at `toground`, by GoldenEye's tile graph:
 * the walk from the tile under the one ends on the tile under the other (or
 * one at the same height there), crossing no unlinked edge, and no link the
 * conversion raised a climb wall on unless `climbs`. 1 yes, 0 no, -1 not
 * known: no graph, or no tile under either end.
 */
s32 geStanLinks(struct coord *from, f32 fromground, struct coord *to, f32 toground, bool climbs);

/**
 * The surface of the tile under `pos` at or under its height (a pad's floor),
 * or -1e30 where there is no graph or no tile.
 */
f32 geStanFloorAt(struct coord *pos);

// GoldenEye's own truck test: whether lines laid end to end in plan (x, z pairs)
// cross no wall of the tile graph, starting on the tile under the first at y
bool geStanLinesClear(const f32 (*pts)[2], s32 n, f32 y);
/**
 * The highest floor a player walking from `pos` (feet at `ground`) to `to`
 * touches there with his circle, of the tiles linked to the one under his foot
 * within `radius` - through tiles on edge, which is how GoldenEye joins a floor
 * to a ledge, a sill or a conveyor well over it and lifts Bond up (bondview2.c's
 * bondviewTryMoveToStan()). Only a floor the way to which crosses one of the
 * conversion's climb walls: the rest are walked onto. GESTAN_NOCLIMBFLOOR
 * where there is none.
 */
#define GESTAN_NOCLIMBFLOOR (-1e30f)
f32 geStanClimbFloor(struct coord *pos, struct coord *to, f32 ground, f32 radius);

/** Whether a body's circle at `pos` touches a floor at height `y` (the climb's hold). */
bool geStanTouchesFloor(struct coord *pos, f32 radius, f32 y);

/**
 * Whether a body at `pos` is on, or within `reach` of an edge linked to, a tile
 * GoldenEye forces a crouch on (STANTILEFLAG_FORCECROUCH: a vent, a crawl space).
 * `hold` (may be NULL) says whether one was reached other than the tile under
 * `from`, where the body stands: GoldenEye holds such a move until Bond is
 * fully down.
 */
bool geStanForcesCrouch(struct coord *pos, f32 limit, f32 rise, f32 reach, struct coord *from, bool *hold);

/** Counters for a probe: walls asked about, walls left out, bodies found over no tile. */
// GoldenEye's death camera's line along the tile graph (gedeathcam.c): 1 clear
// to the end, 0 stopped at hitx/hitz, -1 no graph here
s32 geStanLineReach(struct coord *from, f32 x1, f32 z1, f32 *hitx, f32 *hitz, s32 *room, f32 *ground);

// Whether the tile graph's floor stays under the line from a to b
bool geStanSightClear(struct coord *a, struct coord *b);

// GoldenEye's interact test's tile walk in plan (walkTilesBetweenPoints_NoCallback):
// 1 reached, 0 stopped by an edge with nothing across, -1 no graph here
s32 geStanWalkReaches(struct coord *from, f32 x1, f32 z1);

/**
 * GoldenEye's autogun's sight of its target (propobj.c's autogun tick): the
 * tile graph walked in plan from the tile of its pad `from` (room `fromroom`)
 * towards `to`, seeing only where the walk ends on the tile the target stands
 * on (feet at `toground`). 1 sees, 0 does not, -1 where the level has no graph
 * or either end is over no tile.
 */
s32 geStanAutogunSees(struct coord *from, s32 fromroom, struct coord *to, f32 toground);

/**
 * GoldenEye's pickup test (propobj.c's objTestForPickup()): the tile graph
 * walked in plan from the player's tile to the object, taking it only where the
 * walk ends on the object's tile. 1 may, 0 may not, -1 no graph here.
 */
s32 geStanPickupReaches(struct coord *from, f32 ground, struct coord *to);

/**
 * GoldenEye's test of a guard's pad as its setup is loaded (expand_09_characters()'s
 * getposstan(&pad->pos, pad->stan, 20, ...), which is stanTestVolume()): 1 the
 * guard is made at the pad, 0 it is not made at all, -1 where the level has no
 * graph or the pad is over no tile.
 */
s32 geStanSpawnLegal(struct coord *pos, s32 padroom, f32 radius);

extern s32 g_GeStanAsked;
extern s32 g_GeStanSkipped;
extern s32 g_GeStanNoTile;

// The SHA-256 (32 bytes) of the stage's tile graph file, for netplay's stage
// hash; 0 if the stage has none
s32 geStanFileHash(u8 *out);

#endif

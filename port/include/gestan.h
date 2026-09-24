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
bool geStanWalkFromRoom(struct coord *from, s32 fromroom, struct coord *to, s32 *room, f32 *ground);

// GoldenEye's own truck test: whether lines laid end to end in plan (x, z pairs)
// cross no wall of the tile graph, starting on the tile under the first at y
bool geStanLinesClear(const f32 (*pts)[2], s32 n, f32 y);
/**
 * Whether a body at `pos` is on, or within `reach` of an edge linked to, a tile
 * GoldenEye forces a crouch on (STANTILEFLAG_FORCECROUCH: a vent, a crawl space).
 */
bool geStanForcesCrouch(struct coord *pos, f32 limit, f32 rise, f32 reach);

/** Counters for a probe: walls asked about, walls left out, bodies found over no tile. */
extern s32 g_GeStanAsked;
extern s32 g_GeStanSkipped;
extern s32 g_GeStanNoTile;

#endif

#ifndef _IN_GEROOM_H
#define _IN_GEROOM_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's rule for which room somebody is in, on a level converted from it
 * (modloaderStageIsRemake()). port/src/geroom.c.
 *
 * GoldenEye keeps no room for a player or a guard at all: it keeps the **tile**
 * they stand on, walks the tile links as they move, and the room is the tile's
 * (bondview2.c's current_tile_ptr_for_portals->room, chrprop.c's
 * chrpropUpdateRoomList() seeded from prop->stan). Perfect Dark keeps a room
 * list and changes it only when a move passes *through a portal's polygon*, and
 * a GoldenEye level's portals were never made to be walked through that way -
 * they are narrower than the openings they stand in, and the tiles change room
 * where no portal is. So a player walks past one, stays in the room behind
 * them, and the rooms ahead are not drawn ("left in the void until you keep
 * stepping"); their floor is not found either, which is the fall through the
 * world the earlier fallbacks were for.
 */

/** Whether the stage takes its rooms from the tile underfoot. */
s32 geRoomActive(void);

/**
 * cdFindGroundInfoAtCyl() asked of the rooms handed in **and** of every room
 * whose box holds the position: the highest floor below it, whichever room's
 * tile it is. Any of the out pointers may be NULL, as cdFindGroundInfoAtCyl()'s
 * may.
 */
f32 geRoomGround(struct coord *pos, f32 radius, RoomNum *rooms, u16 *floorcol, u8 *floortype,
		u16 *floorflags, RoomNum *floorroom, s32 *inlift, struct prop **lift);

/**
 * The room the picture is drawn from, for an eye standing on `ground` in the
 * tile's room. The tile's room is where GoldenEye starts, and then it follows a
 * plumb line from the floor under the eye up to the eye across every portal
 * that is not upright, changing room at each: a player two steps down a
 * stairwell stands on the stair's tiles with their eye still over the deck the
 * stairwell is cut in, and the deck is another room's. Without it that room is
 * reached, if at all, through some doorway far below and drawn in the doorway's
 * box - the deck round the stairwell goes, and the sky shows through it (Dam's
 * first tower, F3 report 20260920-220247).
 */
s32 geRoomCamera(struct coord *eye, f32 ground, s32 room);

#endif

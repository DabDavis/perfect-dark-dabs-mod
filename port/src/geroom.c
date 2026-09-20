#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "geroom.h"
#include "game/bg.h"
#include "game/prop.h"
#include "lib/collision.h"

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


#endif

/**
 * Reflections that follow movement - see roomsheen.h.
 */

#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "gbiex.h"
#include "bss.h"
#include "config.h"
#include "system.h"
#include "roomsheen.h"

static s32 optStockFollow = 0;

PD_CONSTRUCTOR static void roomSheenConfigInit(void)
{
	configRegisterInt("Mod.LevelReflectFollow", &optStockFollow, 0, 1);
}

s32 roomSheenGetStockFollow(void)
{
	return optStockFollow;
}

void roomSheenSetStockFollow(s32 on)
{
	optStockFollow = on ? 1 : 0;
}

Gfx *roomSheenStockBegin(Gfx *gdl)
{
	if (optStockFollow) {
		// A room stands still in the world, so the eye ray alone moves its
		// reflections as the player walks; the scroll a gun needs is zeroed
		gDPSetTexgenShiftEXT(gdl++, 0, 0);
		gSPSetExtraGeometryModeEXT(gdl++, G_TEXGEN_EYE_EXT);
	}

	return gdl;
}

Gfx *roomSheenStockEnd(Gfx *gdl)
{
	if (optStockFollow) {
		gSPClearExtraGeometryModeEXT(gdl++, G_TEXGEN_EYE_EXT);
	}

	return gdl;
}

// How far the player moves, in world units, for the streaks to scroll one
// whole span of the texgen (two tiles of 0x3eb at the K7's scale)
#define ROOMSHEEN_SHIFT_PERIOD 400.0f

// A move longer than this in one frame is a teleport, a respawn or a cut, and
// scrolls nothing
#define ROOMSHEEN_SHIFT_JUMP 200.0f

static struct {
	struct coord pos;
	f32 s, t;
	s32 frame;
	s32 seen;
} roomSheenShifts[MAX_PLAYERS];

static f32 roomSheenWrap(f32 v)
{
	return v - floorf(v);
}

Gfx *roomSheenTexgenShift(Gfx *gdl)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;

	if (!player || playernum < 0 || playernum >= MAX_PLAYERS) {
		return gdl;
	}

	if (roomSheenShifts[playernum].frame != g_Vars.lvframenum) {
		const struct coord *pos = &player->cam_pos;
		const struct coord *look = &player->cam_look;
		const struct coord *up = &player->cam_up;
		const f32 dx = pos->x - roomSheenShifts[playernum].pos.x;
		const f32 dy = pos->y - roomSheenShifts[playernum].pos.y;
		const f32 dz = pos->z - roomSheenShifts[playernum].pos.z;

		if (roomSheenShifts[playernum].seen && dx * dx + dy * dy + dz * dz < ROOMSHEEN_SHIFT_JUMP * ROOMSHEEN_SHIFT_JUMP) {
			// Across the view scrolls s, and along it or up and down scrolls t
			const f32 rx = look->y * up->z - look->z * up->y;
			const f32 ry = look->z * up->x - look->x * up->z;
			const f32 rz = look->x * up->y - look->y * up->x;
			const f32 across = dx * rx + dy * ry + dz * rz;
			const f32 along = dx * look->x + dy * look->y + dz * look->z;
			const f32 rise = dx * up->x + dy * up->y + dz * up->z;

			roomSheenShifts[playernum].s = roomSheenWrap(roomSheenShifts[playernum].s + across / ROOMSHEEN_SHIFT_PERIOD);
			roomSheenShifts[playernum].t = roomSheenWrap(roomSheenShifts[playernum].t + (along - rise) / ROOMSHEEN_SHIFT_PERIOD);
		}

		roomSheenShifts[playernum].pos = *pos;
		roomSheenShifts[playernum].frame = g_Vars.lvframenum;
		roomSheenShifts[playernum].seen = true;
	}

	gDPSetTexgenShiftEXT(gdl++, roomSheenShifts[playernum].s * 16384.0f, roomSheenShifts[playernum].t * 16384.0f);

	return gdl;
}

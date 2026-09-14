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

// On by default since 2026-09-14, at the user's request, so testers have it.
static s32 optStockFollow = 1;

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

// How far the player moves, in world units, for the streaks to scroll one
// whole span of the texgen (two tiles of 0x3eb at the K7's scale)
#define ROOMSHEEN_SHIFT_PERIOD 400.0f

// How far the player moves for a level's reflections to turn once round. Half
// a turn sweeps the sphere map from one edge to the other, which is the span a
// gun's streaks scroll over ROOMSHEEN_SHIFT_PERIOD, and it is a whole number
// of those spans, so both can be read off one wrapped distance with no seam.
#define ROOMSHEEN_TURN_PERIOD (2.0f * ROOMSHEEN_SHIFT_PERIOD)

// A move longer than this in one frame is a teleport, a respawn or a cut, and
// scrolls nothing
#define ROOMSHEEN_SHIFT_JUMP 200.0f

// Distance walked across the view and along it (minus rise), in turns of
// ROOMSHEEN_TURN_PERIOD, wrapped to [0, 1)
static struct {
	struct coord pos;
	f32 across, along;
	s32 frame;
	s32 seen;
} roomSheenShifts[MAX_PLAYERS];

static f32 roomSheenWrap(f32 v)
{
	return v - floorf(v);
}

static s32 roomSheenAccumulate(void)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;

	if (!player || playernum < 0 || playernum >= MAX_PLAYERS) {
		return -1;
	}

	if (roomSheenShifts[playernum].frame != g_Vars.lvframenum) {
		const struct coord *pos = &player->cam_pos;
		const struct coord *look = &player->cam_look;
		const struct coord *up = &player->cam_up;
		const f32 dx = pos->x - roomSheenShifts[playernum].pos.x;
		const f32 dy = pos->y - roomSheenShifts[playernum].pos.y;
		const f32 dz = pos->z - roomSheenShifts[playernum].pos.z;

		if (roomSheenShifts[playernum].seen && dx * dx + dy * dy + dz * dz < ROOMSHEEN_SHIFT_JUMP * ROOMSHEEN_SHIFT_JUMP) {
			const f32 rx = look->y * up->z - look->z * up->y;
			const f32 ry = look->z * up->x - look->x * up->z;
			const f32 rz = look->x * up->y - look->y * up->x;
			const f32 across = dx * rx + dy * ry + dz * rz;
			const f32 along = dx * look->x + dy * look->y + dz * look->z;
			const f32 rise = dx * up->x + dy * up->y + dz * up->z;

			roomSheenShifts[playernum].across = roomSheenWrap(roomSheenShifts[playernum].across + across / ROOMSHEEN_TURN_PERIOD);
			roomSheenShifts[playernum].along = roomSheenWrap(roomSheenShifts[playernum].along + (along - rise) / ROOMSHEEN_TURN_PERIOD);
		}

		roomSheenShifts[playernum].pos = *pos;
		roomSheenShifts[playernum].frame = g_Vars.lvframenum;
		roomSheenShifts[playernum].seen = true;
	}

	return playernum;
}

// Between roomSheenStockBegin() and End() while the lists are built, so a
// K7 sheen drawn inside (an XBLA mesh on a prop) can put the state back
static s32 roomSheenStockOpen = false;

Gfx *roomSheenTexgenTurn(Gfx *gdl)
{
	// A level's reflections turn as the player walks, the way they turn as
	// the camera does (G_TEXGEN_TURN_EXT), across the view yawing them and
	// along it pitching them. The eye ray still bends each vertex's lookup.
	const s32 playernum = roomSheenAccumulate();

	if (playernum >= 0) {
		gDPSetTexgenShiftEXT(gdl++, roomSheenShifts[playernum].across * 16384.0f, roomSheenShifts[playernum].along * 16384.0f);
	} else {
		gDPSetTexgenShiftEXT(gdl++, 0, 0);
	}

	return gdl;
}

static Gfx *roomSheenStockEmit(Gfx *gdl)
{
	gdl = roomSheenTexgenTurn(gdl);
	gSPSetExtraGeometryModeEXT(gdl++, G_TEXGEN_EYE_EXT | G_TEXGEN_TURN_EXT);

	return gdl;
}

Gfx *roomSheenStockBegin(Gfx *gdl)
{
	if (optStockFollow) {
		roomSheenStockOpen = true;
		gdl = roomSheenStockEmit(gdl);
	}

	return gdl;
}

Gfx *roomSheenStockResume(Gfx *gdl)
{
	if (roomSheenStockOpen) {
		gdl = roomSheenStockEmit(gdl);
	}

	return gdl;
}

Gfx *roomSheenStockEnd(Gfx *gdl)
{
	if (roomSheenStockOpen) {
		roomSheenStockOpen = false;
		gSPClearExtraGeometryModeEXT(gdl++, G_TEXGEN_EYE_EXT | G_TEXGEN_TURN_EXT);
	}

	return gdl;
}

Gfx *roomSheenTexgenShift(Gfx *gdl)
{
	const s32 playernum = roomSheenAccumulate();

	if (playernum < 0) {
		return gdl;
	}

	// Across the view scrolls s, and along it or up and down scrolls t, one
	// span per ROOMSHEEN_SHIFT_PERIOD
	const f32 spans = ROOMSHEEN_TURN_PERIOD / ROOMSHEEN_SHIFT_PERIOD;

	gDPSetTexgenShiftEXT(gdl++, roomSheenWrap(roomSheenShifts[playernum].across * spans) * 16384.0f,
			roomSheenWrap(roomSheenShifts[playernum].along * spans) * 16384.0f);

	return gdl;
}

#include <ultra64.h>
#include "constants.h"
#include "game/bondmove.h"
#include "game/bondwalk.h"
#include "game/modspectate.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"

/**
 * Spectator mode: the camera comes off the player and flies.
 *
 * The camera stays on the player prop rather than becoming a thing of its own,
 * because what makes a free camera hard here is not moving it, it is knowing
 * which rooms it is in. Everything the renderer draws is reached from a prop's
 * room list, and building one from nothing means duplicating the portal walk.
 * The player already carries a room list that the rest of the frame trusts, so
 * spectating keeps the prop and takes away the things that make it a
 * participant: it is not drawn, not collided with, not shot at, and not moved
 * by the walk code.
 *
 * The move itself is deliberately not a collision test. func0f065e74() resolves
 * the destination rooms by walking portals, falls back to bgFindRoomsByPos()
 * when the portal walk comes up empty - which is what going through a wall
 * does - and falls back again to keeping the rooms we already had when the
 * destination is outside the level entirely. That last case is the one that
 * matters: the list is never returned empty, so flying into the void leaves the
 * camera drawing the room it left rather than handing the renderer nothing.
 * That is why this can skip collision at all.
 *
 * g_Vars.bondvisible is the game's own invisibility, and every check that asks
 * whether a chr can see or target the player already reads it (bot.c's
 * botIsTargetInvisible, chraction.c's target tests, propobj.c's drawing). It is
 * a global rather than per player, so in splitscreen one spectator makes every
 * player's body invisible - accepted, because splitscreen spectating is not a
 * thing this is for.
 */

// How far the camera moves in one frame at full stick.
f32 g_ModSpectateSpeed = 20.0f;

// --spectate: begin the stage already spectating. There is no way to press a
// button before the first frame, and driving the menus is exactly what the
// headless benchmark runs cannot do.
s32 g_ModSpectateStart = 0;

static bool g_ModSpectating[MAX_PLAYERS] = { false, false, false, false };

// bondvisible and bondcollisions are global and cheats also write them, so what
// goes back on exit is what was there on entry rather than an assumed true.
static bool g_ModSpectateOldVisible = true;
static bool g_ModSpectateOldCollisions = true;
static u8 g_ModSpectateOldInvincible = 0;

// Which stage --spectate has already been acted on for.
static s32 g_ModSpectateAppliedStage = -1;

bool modSpectateIsOn(void)
{
	if (g_Vars.currentplayernum < 0 || g_Vars.currentplayernum >= MAX_PLAYERS) {
		return false;
	}

	return g_ModSpectating[g_Vars.currentplayernum];
}

void modSpectateSetOn(bool on)
{
	if (g_Vars.currentplayernum < 0 || g_Vars.currentplayernum >= MAX_PLAYERS) {
		return;
	}

	if (g_ModSpectating[g_Vars.currentplayernum] == on) {
		return;
	}

	g_ModSpectating[g_Vars.currentplayernum] = on;

	if (on) {
		g_ModSpectateOldVisible = g_Vars.bondvisible;
		g_ModSpectateOldCollisions = g_Vars.bondcollisions;

		g_Vars.bondvisible = false;
		g_Vars.bondcollisions = false;

		if (g_Vars.currentplayer) {
			g_ModSpectateOldInvincible = g_Vars.currentplayer->invincible;
			g_Vars.currentplayer->invincible = true;

			// Nothing should walk into a camera. The perimeter is what other
			// props collide against, and it is restored on the way out.
			if (g_Vars.currentplayer->prop) {
				propSetPerimEnabled(g_Vars.currentplayer->prop, false);
			}
		}
	} else {
		g_Vars.bondvisible = g_ModSpectateOldVisible;
		g_Vars.bondcollisions = g_ModSpectateOldCollisions;

		if (g_Vars.currentplayer) {
			g_Vars.currentplayer->invincible = g_ModSpectateOldInvincible;

			if (g_Vars.currentplayer->prop) {
				propSetPerimEnabled(g_Vars.currentplayer->prop, true);
			}
		}
	}
}

void modSpectateToggle(void)
{
	modSpectateSetOn(!modSpectateIsOn());
}

/**
 * Act on --spectate, once per stage.
 *
 * It cannot be done at boot: playermgrAllocatePlayer() ends by putting
 * bondvisible and bondcollisions back to true, so anything set before it runs
 * is undone. It waits for a prop as well, because entering the mode turns the
 * prop's perimeter off. The first movement tick of a stage is the first moment
 * both are true.
 */
void modSpectateApplyStart(void)
{
	if (!g_ModSpectateStart || g_ModSpectateAppliedStage == g_Vars.stagenum) {
		return;
	}

	if (g_Vars.currentplayer == NULL || g_Vars.currentplayer->prop == NULL) {
		return;
	}

	g_ModSpectateAppliedStage = g_Vars.stagenum;
	modSpectateSetOn(true);
}

/**
 * Forget the mode across a stage load. The prop and the room list on the far
 * side belong to a different level, and the saved bondvisible does not.
 */
void modSpectateReset(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ModSpectating[i] = false;
	}

	g_ModSpectateOldVisible = true;
	g_ModSpectateOldCollisions = true;
	g_ModSpectateOldInvincible = 0;
	g_ModSpectateAppliedStage = -1;
}

/**
 * Stands in for bwalkTick() while spectating.
 *
 * bmoveProcessInput() has already run in bmoveTick() and left the look angles
 * and the stick in speedforwards/speedsideways, so this only has to turn them
 * into a position. bmoveUpdateVerta() is called the way every other movement
 * mode's tick calls it, because it is what refreshes the look vector this then
 * reads.
 */
void modSpectateTick(void)
{
	struct prop *prop;
	struct coord dstpos;
	RoomNum dstrooms[8];
	f32 speed;
	f32 fwd;
	f32 side;

	if (g_Vars.currentplayer == NULL || g_Vars.currentplayer->prop == NULL) {
		return;
	}

	prop = g_Vars.currentplayer->prop;

	// The head of bwalkTick(), minus the parts that walk. Theta is the stick's
	// half of turning, the mouse having already been taken in
	// bmoveProcessInput(), and bmoveUpdateVerta() is what turns both angles
	// into the look vector read below.
	bwalkUpdatePrevPos();
	bwalkUpdateTheta();
	bmoveUpdateVerta();

	fwd = g_Vars.currentplayer->speedforwards;
	side = g_Vars.currentplayer->speedsideways;
	speed = g_ModSpectateSpeed * g_Vars.lvupdate60freal;

	// Forward follows the look vector including its pitch, so looking up and
	// pushing forward climbs. Strafing stays level, which is what a flying
	// camera is expected to do. The horizontal pair is the same expression
	// bwalkTick() uses, so the sense of the stick does not change when the
	// mode does.
	dstpos.x = prop->pos.x + ((g_Vars.currentplayer->bond2.unk00.x * g_Vars.currentplayer->vv_cosverta * fwd)
			- (g_Vars.currentplayer->bond2.unk00.z * side)) * speed;
	dstpos.z = prop->pos.z + ((g_Vars.currentplayer->bond2.unk00.z * g_Vars.currentplayer->vv_cosverta * fwd)
			+ (g_Vars.currentplayer->bond2.unk00.x * side)) * speed;

	// vv_sinverta is taken from vv_verta360, so looking up gives +1 and looking
	// down gives -1 without a sign flip here.
	dstpos.y = prop->pos.y + g_Vars.currentplayer->vv_sinverta * fwd * speed;

	func0f065e74(&prop->pos, prop->rooms, &dstpos, dstrooms);

	prop->pos.x = dstpos.x;
	prop->pos.y = dstpos.y;
	prop->pos.z = dstpos.z;

	propDeregisterRooms(prop);
	roomsCopy(dstrooms, prop->rooms);

	bmoveUpdateRooms(g_Vars.currentplayer);

	// The tail of bwalkTick(), and the part that is easy to leave out: moving
	// the prop does not move the view. bmove0f0cc654() refreshes the look and
	// up vectors, and bmove0f0cc19c() puts the eye on the prop - the camera is
	// built from those three, so without them the prop flies off and the
	// picture stays where the walk last left it.
	//
	// The three arguments are the speeds bwalkTick() passes to lean and sway
	// the view with the stride. A flying camera has no stride, so they are zero
	// and the view stays level.
	bmove0f0cc654(0, 0, 0);
	bmove0f0cc19c(&prop->pos);
}

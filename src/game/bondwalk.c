#include <ultra64.h>
#include "constants.h"
#include "game/bondmove.h"
#include "game/bondwalk.h"
#include "game/mplayer/mplayer.h"
#include "game/cheats.h"
#include "game/chraction.h"
#include "game/debug.h"
#include "game/footstep.h"
#include "game/game_006900.h"
#include "game/chr.h"
#include "game/prop.h"
#include "game/propsnd.h"
#include "game/objectives.h"
#include "game/bondgun.h"
#include "game/player.h"
#include "game/inv.h"
#include "game/bondhead.h"
#include "game/playermgr.h"
#include "game/propobj.h"
#include "bss.h"
#include "lib/model.h"
#include "lib/snd.h"
#include "lib/rng.h"
#include "lib/mtx.h"
#include "lib/anim.h"
#include "lib/collision.h"
#include "data.h"
#include "types.h"
#include "game/modoptions.h"
#include "game/modrun.h"
#include "game/modrules.h"
#include "game/bg.h"
#ifndef PLATFORM_N64
#include "modloader.h"
#include "geroom.h"
#include "gestan.h"
#ifndef PLATFORM_N64
#include "getank.h"
#endif
#endif
#ifndef PLATFORM_N64
extern f32 fabsf(f32);
#endif

#ifndef PLATFORM_N64
// a converted level's ladder taken from the top, and by whom: it keeps its hold
static struct player *g_GeLadderTopPlayer = NULL;

/**
 * The rooms one of the player's collision tests is asked of. On a level
 * converted from GoldenEye that is the portal walk's rooms and every room whose
 * box meets the player's (geRoomAddNear()): a wall is filed under its tile's
 * room, and at the head of Cradle's stair down to the shaft the player, still
 * in the landing's room, slid into the stair's side wall and was inside it,
 * stuck for good, the moment the stair's room was theirs (F3 report
 * 20260926-005903). `rooms` is left alone - it is the room list the player
 * goes on with, which the AI asks "is Bond in the room with pad N" of - and
 * the test's list is written to `out` (`maxlen` rooms at most).
 */
static RoomNum *bwalkCdRooms(RoomNum *rooms, struct coord *pos, f32 radius, f32 ymin, f32 ymax, RoomNum *out, s32 maxlen)
{
	s32 i;

	if (!geRoomActive()) {
		return rooms;
	}

	for (i = 0; i < maxlen && rooms[i] != -1; i++) {
		out[i] = rooms[i];
	}

	out[i] = -1;

	geRoomAddNear(pos, radius, ymin, ymax, out, maxlen);

	return out;
}
#endif

void bwalkInit(void)
{
	u32 prevmode = g_Vars.currentplayer->bondmovemode;
	s32 i;

#ifndef PLATFORM_N64
	if (g_GeLadderTopPlayer == g_Vars.currentplayer) {
		g_GeLadderTopPlayer = NULL;
	}
#endif

	g_Vars.currentplayer->bondmovemode = MOVEMODE_WALK;
	g_Vars.currentplayer->bondonground = 0;
	g_Vars.currentplayer->tank = NULL;
	g_Vars.currentplayer->unk1af0 = NULL;
	g_Vars.currentplayer->bondonturret = false;

	g_Vars.currentplayer->swaypos = 0;
	g_Vars.currentplayer->swayoffset = 0;
	g_Vars.currentplayer->swaytarget = 0;
	g_Vars.currentplayer->swayoffset0 = 0;
	g_Vars.currentplayer->swayoffset2 = 0;

	g_Vars.currentplayer->bdeltapos.x = 0;
	g_Vars.currentplayer->bdeltapos.y = -0.0001f;
	g_Vars.currentplayer->bdeltapos.z = 0;

	g_Vars.currentplayer->isfalling = false;
	g_Vars.currentplayer->fallstart = 0;

	g_Vars.currentplayer->gunextraaimx = 0;
	g_Vars.currentplayer->gunextraaimy = 0;

	g_Vars.currentplayer->bondforcespeed.x = 0;
	g_Vars.currentplayer->bondforcespeed.y = 0;
	g_Vars.currentplayer->bondforcespeed.z = 0;

#ifndef PLATFORM_N64
	g_Vars.currentplayer->rollspeed.x = 0;
	g_Vars.currentplayer->rollspeed.y = 0;
	g_Vars.currentplayer->rollspeed.z = 0;
	g_Vars.currentplayer->rolltime60 = 0;
	g_Vars.currentplayer->camtiltroll = 0;
	g_Vars.currentplayer->camtiltpitch = 0;
	g_Vars.currentplayer->camstepphase = 0;
	g_Vars.currentplayer->camstepamp = 0;
#endif

	if (prevmode != MOVEMODE_WALK && prevmode != MOVEMODE_CUTSCENE) {
		g_Vars.currentplayer->sumcrouch = 0;
		g_Vars.currentplayer->crouchheight = 0;
		g_Vars.currentplayer->crouchtime240 = 0;
		g_Vars.currentplayer->crouchfall = 0;
		g_Vars.currentplayer->crouchpos = CROUCHPOS_STAND;
		g_Vars.currentplayer->autocrouchpos = CROUCHPOS_STAND;
		g_Vars.currentplayer->crouchspeed = 0;
		g_Vars.currentplayer->crouchoffset = 0;

#if VERSION < VERSION_NTSC_1_0
		bwalkUpdateCrouchOffsetReal();
#endif

		g_Vars.currentplayer->guncloseroffset = 0;
	}

#if VERSION >= VERSION_NTSC_1_0
	bwalkUpdateCrouchOffsetReal();
#endif

	if (prevmode != MOVEMODE_GRAB && prevmode != MOVEMODE_WALK) {
		for (i = 0; i != 3; i++) {
			g_Vars.currentplayer->bondshotspeed.f[i] = 0;
		}

		g_Vars.currentplayer->speedsideways = 0;
		g_Vars.currentplayer->speedstrafe = 0;
		g_Vars.currentplayer->speedgo = 0;
		g_Vars.currentplayer->speedboost = 1;
		g_Vars.currentplayer->speedmaxtime60 = 0;
		g_Vars.currentplayer->speedforwards = 0;
		g_Vars.currentplayer->speedtheta = 0;
		g_Vars.currentplayer->speedthetacontrol = 0;
	}

	if (g_Vars.currentplayer->walkinitmove) {
		struct coord delta;
		mtx00016b58(&g_Vars.currentplayer->walkinitmtx,
				0, 0, 0,
				-g_Vars.currentplayer->bond2.unk1c.x, -g_Vars.currentplayer->bond2.unk1c.y, -g_Vars.currentplayer->bond2.unk1c.z,
				g_Vars.currentplayer->bond2.unk28.x, g_Vars.currentplayer->bond2.unk28.y, g_Vars.currentplayer->bond2.unk28.z);
		g_Vars.currentplayer->walkinitt = 0;
		g_Vars.currentplayer->walkinitt2 = 0;
		g_Vars.currentplayer->walkinitstart.x = g_Vars.currentplayer->prop->pos.x;
		g_Vars.currentplayer->walkinitstart.y = g_Vars.currentplayer->prop->pos.y;
		g_Vars.currentplayer->walkinitstart.z = g_Vars.currentplayer->prop->pos.z;

		delta.x = g_Vars.currentplayer->walkinitpos.x - g_Vars.currentplayer->prop->pos.x;
		delta.y = 0;
		delta.z = g_Vars.currentplayer->walkinitpos.z - g_Vars.currentplayer->prop->pos.z;

		propSetPerimEnabled(g_Vars.currentplayer->hoverbike, false);
		bwalkCalculateNewPositionWithPush(&delta, 0, true, 0, CDTYPE_ALL);
		propSetPerimEnabled(g_Vars.currentplayer->hoverbike, true);
	} else if (prevmode != MOVEMODE_GRAB && prevmode != MOVEMODE_WALK) {
		g_Vars.currentplayer->moveinitspeed.x = 0;
		g_Vars.currentplayer->moveinitspeed.y = 0;
		g_Vars.currentplayer->moveinitspeed.z = 0;
	}
}

void bwalkSetSwayTargetf(f32 value) {
	g_Vars.currentplayer->swaytarget = value * 75.f;
}

void bwalkSetSwayTarget(s32 value)
{
	g_Vars.currentplayer->swaytarget = value * 75.0f;
}

void bwalkAdjustCrouchPos(s32 value)
{
	g_Vars.currentplayer->crouchpos += value;

	if (g_Vars.currentplayer->crouchpos < CROUCHPOS_SQUAT) {
		g_Vars.currentplayer->crouchpos = CROUCHPOS_SQUAT;
	} else if (g_Vars.currentplayer->crouchpos > CROUCHPOS_STAND) {
		g_Vars.currentplayer->crouchpos = CROUCHPOS_STAND;
	}
}

void bwalk0f0c3b38(struct coord *reltarget, struct defaultobj *obj)
{
	struct coord posunk;
	struct coord vector;
	struct coord tween;
	struct coord globalthinga;
	struct coord globalthingb;
	struct coord abstarget;

	abstarget.x = reltarget->x + g_Vars.currentplayer->prop->pos.x;
	abstarget.y = g_Vars.currentplayer->prop->pos.y;
	abstarget.z = reltarget->z + g_Vars.currentplayer->prop->pos.z;

#if VERSION >= VERSION_NTSC_1_0
	cdGetEdge(&globalthinga, &globalthingb, 223, "bondwalk.c");
#else
	cdGetEdge(&globalthinga, &globalthingb, 221, "bondwalk.c");
#endif

	vector.x = globalthingb.z - globalthinga.z;
	vector.y = 0;
	vector.z = globalthinga.x - globalthingb.x;

	if (vector.f[0] != 0 || vector.f[2] != 0) {
		guNormalize(&vector.x, &vector.y, &vector.z);
	} else {
		vector.z = 1;
	}

	func0f02e3dc(&globalthinga, &globalthingb, &abstarget, &vector, &posunk);

	tween.x = (abstarget.x - g_Vars.currentplayer->prop->pos.x) / g_Vars.lvupdate60freal;
	tween.y = 0;
	tween.z = (abstarget.z - g_Vars.currentplayer->prop->pos.z) / g_Vars.lvupdate60freal;

	func0f082e84(obj, &posunk, &vector, &tween, false);
}

/**
 * Attempt to move the current player up vertically by the given amount.
 *
 * Collision checks are done for the new location, and if successful the
 * player's positional values are updated.
 *
 * The function is called with amount = 0 when attempting to stand up from a
 * crouch, after increasing the player's bbox to the standing size.
 */
s32 bwalkTryMoveUpwards(f32 amount)
{
	bool result;
	struct coord newpos;
	RoomNum rooms[8];
#ifndef PLATFORM_N64
	RoomNum cdrooms[21];
#endif
	u32 stack;
	u32 types;
	f32 ymax;
	f32 ymin;
	f32 radius;

	if (g_Vars.currentplayer->floorflags & GEOFLAG_SLOPE) {
		g_Vars.enableslopes = false;
	} else {
		g_Vars.enableslopes = true;
	}

	newpos.x = g_Vars.currentplayer->prop->pos.x;
	newpos.y = g_Vars.currentplayer->prop->pos.y + amount;
	newpos.z = g_Vars.currentplayer->prop->pos.z;

	types = g_Vars.bondcollisions ? CDTYPE_ALL : CDTYPE_BG;

	playerGetBbox(g_Vars.currentplayer->prop, &radius, &ymax, &ymin);
	func0f065e74(&g_Vars.currentplayer->prop->pos, g_Vars.currentplayer->prop->rooms, &newpos, rooms);
	bmoveFindEnteredRoomsByPos(g_Vars.currentplayer, &newpos, rooms);
	propSetPerimEnabled(g_Vars.currentplayer->prop, false);

#ifndef PLATFORM_N64
	// a tank he is being lifted onto (getank.c) is no more in the way of the
	// lift than it is of his steps: with it left on, he rose until his box was
	// inside the hull's and stood there, knee deep in it
	if (g_Vars.currentplayer->tank) {
		propSetPerimEnabled(g_Vars.currentplayer->tank, false);
	}
#endif

	ymin -= 0.1f;

#ifndef PLATFORM_N64
	result = cdTestVolume(&newpos, radius,
			bwalkCdRooms(rooms, &newpos, radius, ymin - g_Vars.currentplayer->prop->pos.y,
				ymax - g_Vars.currentplayer->prop->pos.y, cdrooms, ARRAYCOUNT(cdrooms) - 1),
			types, CHECKVERTICAL_YES,
			ymax - g_Vars.currentplayer->prop->pos.y,
			ymin - g_Vars.currentplayer->prop->pos.y);
#else
	result = cdTestVolume(&newpos, radius, rooms, types, CHECKVERTICAL_YES,
			ymax - g_Vars.currentplayer->prop->pos.y,
			ymin - g_Vars.currentplayer->prop->pos.y);
#endif

	propSetPerimEnabled(g_Vars.currentplayer->prop, true);

#ifndef PLATFORM_N64
	if (g_Vars.currentplayer->tank) {
		propSetPerimEnabled(g_Vars.currentplayer->tank, true);
	}
#endif

	if (result == CDRESULT_NOCOLLISION) {
		g_Vars.currentplayer->prop->pos.y = newpos.y;
		propDeregisterRooms(g_Vars.currentplayer->prop);
		roomsCopy(rooms, g_Vars.currentplayer->prop->rooms);
	}

	g_Vars.enableslopes = true;

	return result;
}

bool bwalkCanMoveUpwards(f32 amount)
{
	bool result;
	struct coord newpos;
	RoomNum rooms[8];
#ifndef PLATFORM_N64
	RoomNum cdrooms[21];
#endif
	u32 stack;
	u32 types;
	f32 ymax;
	f32 ymin;
	f32 radius;

	if (g_Vars.currentplayer->floorflags & GEOFLAG_SLOPE) {
		g_Vars.enableslopes = false;
	} else {
		g_Vars.enableslopes = true;
	}

	newpos.x = g_Vars.currentplayer->prop->pos.x;
	newpos.y = g_Vars.currentplayer->prop->pos.y + amount;
	newpos.z = g_Vars.currentplayer->prop->pos.z;

	types = g_Vars.bondcollisions ? CDTYPE_ALL : CDTYPE_BG;

	playerGetBbox(g_Vars.currentplayer->prop, &radius, &ymax, &ymin);
	func0f065e74(&g_Vars.currentplayer->prop->pos, g_Vars.currentplayer->prop->rooms, &newpos, rooms);
	bmoveFindEnteredRoomsByPos(g_Vars.currentplayer, &newpos, rooms);
	propSetPerimEnabled(g_Vars.currentplayer->prop, false);

	ymin -= 0.1f;

#ifndef PLATFORM_N64
	result = cdTestVolume(&newpos, radius,
			bwalkCdRooms(rooms, &newpos, radius, ymin - g_Vars.currentplayer->prop->pos.y,
				ymax - g_Vars.currentplayer->prop->pos.y, cdrooms, ARRAYCOUNT(cdrooms) - 1),
			types, CHECKVERTICAL_YES,
			ymax - g_Vars.currentplayer->prop->pos.y,
			ymin - g_Vars.currentplayer->prop->pos.y);
#else
	result = cdTestVolume(&newpos, radius, rooms, types, CHECKVERTICAL_YES,
			ymax - g_Vars.currentplayer->prop->pos.y,
			ymin - g_Vars.currentplayer->prop->pos.y);
#endif

	propSetPerimEnabled(g_Vars.currentplayer->prop, true);

	g_Vars.enableslopes = true;

	return (result == CDRESULT_NOCOLLISION);
}

#ifndef PLATFORM_N64
/**
 * Whether a roll still has the player's body.
 *
 * Nothing else may be started while it does - see ROLL_BUSY. Measured from the
 * frame the roll was thrown rather than read off the body's animation, because
 * in solo first person there is no body and the roll has to be the same move
 * either way.
 */
bool bwalkIsRolling(void)
{
	if (g_Vars.currentplayer->rolltime60 == 0) {
		return false;
	}

	return g_Vars.lvframe60 - g_Vars.currentplayer->rolltime60 < ROLL_BUSY;
}
#endif

/**
 * Leave the ground, if we are on it.
 *
 * bdeltapos.y is the player's vertical velocity and the integrator that reads
 * it is symmetric, so a jump is just a positive value in it - see the matching
 * change in bwalkUpdateVertical(), which otherwise only looks at that velocity
 * once something else has already lifted the player off the floor.
 *
 * The headroom test is not strictly needed, since bwalkTryMoveUpwards() stops
 * the ascent at a ceiling anyway, but starting a jump with nothing overhead to
 * rise into just spends the impulse on nothing.
 *
 * Called from lvRender() on a use press that found nothing to use, which is a
 * frame later than the tick that read the button. bwalkUpdateVertical() takes
 * the branch on the velocity itself, not on being airborne, so an impulse left
 * here survives to the next tick and is integrated there.
 *
 * Solo jumps too. This was once an arena rule and could not, but it is a global
 * setting now - see modoptions.h - and a mission is somewhere to use it.
 */
void bwalkTryJump(void)
{
	if (!modIsJumpEnabled()
			|| g_Vars.currentplayer->bondmovemode != MOVEMODE_WALK) {
		return;
	}

	if (g_Vars.currentplayer->isfalling
			|| g_Vars.currentplayer->bdeltapos.y > 0.0f
			|| g_Vars.currentplayer->vv_manground > g_Vars.currentplayer->vv_ground) {
		return;
	}

#ifndef PLATFORM_N64
	if (bwalkIsRolling()) {
		return;
	}
#endif

	const f32 impulse = modGetJumpImpulse();

	if (!bwalkCanMoveUpwards(impulse)) {
		return;
	}

	g_Vars.currentplayer->bdeltapos.y = impulse;
}

#ifndef PLATFORM_N64
/**
 * Throw the player sideways, and the body with them.
 *
 * The push goes in where bondforcespeed goes in and decays on the chr's curve,
 * so a roll covers the same ground whoever throws it - see ROLL_IMPULSE. It is
 * held in world space rather than as a direction and a speed, so a player who
 * spins the mouse mid roll still lands where the roll was aimed.
 *
 * Which way is the way they are already going: the strafe the roll came out of
 * is the one they meant, and standing still rolls right.
 *
 * The direction comes off bond2.unk00, the flat look direction, and not off
 * chrGetSideVector() the way the simulants' does. A simulant's look angle is
 * where it is going, but a player's body carries angleoffset - the lean the
 * walk animation is given so a strafe points where it is headed - and rolling
 * along that would send the roll somewhere the camera is not pointing. The
 * camera is what the player aimed the roll with.
 *
 * The animation is only half of it and can be missing entirely - solo in first
 * person has no body to roll - so it is asked for and not waited on. The push
 * is the move.
 */
void bwalkTryRoll(void)
{
	struct chrdata *chr = g_Vars.currentplayer->prop->chr;
	struct coord side;
	bool toleft;

	if (!modCanPlayerRoll()
			|| g_Vars.currentplayer->isdead
			|| g_Vars.currentplayer->bondmovemode != MOVEMODE_WALK
			|| g_Vars.currentplayer->isfalling
			|| g_Vars.currentplayer->onladder
			|| g_Vars.lvframe60 - g_Vars.currentplayer->rolltime60 < ROLL_COOLDOWN) {
		return;
	}

	toleft = g_Vars.currentplayer->speedsideways < 0;

	// chrGetSideVector()'s left, written out of the player's own basis:
	// strafing right is bond2.unk00 turned that way, so left is its negative.
	side.x = -g_Vars.currentplayer->bond2.unk00.z;
	side.z = g_Vars.currentplayer->bond2.unk00.x;

	if (toleft) {
		side.x = -side.x;
		side.z = -side.z;
	}

	g_Vars.currentplayer->rollspeed.x = side.x * ROLL_IMPULSE;
	g_Vars.currentplayer->rollspeed.y = 0;
	g_Vars.currentplayer->rollspeed.z = side.z * ROLL_IMPULSE;
	g_Vars.currentplayer->rolltime60 = g_Vars.lvframe60;

	chrPlayRollAnimation(chr, toleft);
}
#endif

bool bwalkCalculateNewPosition(struct coord *vel, f32 rotateamount, bool apply, f32 extrawidth, s32 checktypes)
{
	s32 result = CDRESULT_NOCOLLISION;
	f32 halfradius;
	struct coord dstpos;
	RoomNum dstrooms[8];
	bool copyrooms = false;
	RoomNum sp64[22];
#ifndef PLATFORM_N64
	RoomNum cdrooms[22];
#endif
	s32 types;
	f32 ymax;
	f32 ymin;
	f32 radius;
	f32 xdiff;
	f32 zdiff;
	s32 i;

	if (g_Vars.currentplayer->floorflags & GEOFLAG_SLOPE) {
		g_Vars.enableslopes = false;
	} else {
		g_Vars.enableslopes = true;
	}

	dstpos.x = g_Vars.currentplayer->prop->pos.x;
	dstpos.y = g_Vars.currentplayer->prop->pos.y;
	dstpos.z = g_Vars.currentplayer->prop->pos.z;

	if (vel->x || vel->y || vel->z) {
		if (g_Vars.currentplayer->tank) {
			propSetPerimEnabled(g_Vars.currentplayer->tank, false);
		}

		propSetPerimEnabled(g_Vars.currentplayer->prop, false);

		dstpos.x += vel->x;
		dstpos.y += vel->y;
		dstpos.z += vel->z;

		types = g_Vars.bondcollisions ? checktypes : CDTYPE_BG;

		playerGetBbox(g_Vars.currentplayer->prop, &radius, &ymax, &ymin);
		radius += extrawidth;

		func0f065dfc(&g_Vars.currentplayer->prop->pos, g_Vars.currentplayer->prop->rooms,
				&dstpos, dstrooms, sp64, 20);

#if VERSION < VERSION_NTSC_1_0
		for (i = 0; dstrooms[i] != -1; i++) {
			if (dstrooms[i] == g_Vars.currentplayer->floorroom) {
				dstrooms[0] = g_Vars.currentplayer->floorroom;
				dstrooms[1] = -1;
				break;
			}
		}
#endif

		bmoveFindEnteredRoomsByPos(g_Vars.currentplayer, &dstpos, dstrooms);

		copyrooms = true;

		// Check if the player is moving at least half their radius along the
		// X or Z axis in a single frame. If less, only do a collision check for
		// the dst position. If more, do a halfway check too?
		xdiff = dstpos.x - g_Vars.currentplayer->prop->pos.x;
		zdiff = dstpos.z - g_Vars.currentplayer->prop->pos.z;
		halfradius = radius * 0.5f;

		// The Randomizer's run seals its room until the room's objective is
		// done, and the doorway is then a wall from the inside. It is answered
		// here rather than by throwing the move away afterwards because what
		// follows a collision reads the collision - the slide along the
		// obstacle's edge, the push - and modRunSealMove() leaves one to read.
		if (modRunSealMove(g_Vars.currentplayer->prop->rooms, &g_Vars.currentplayer->prop->pos,
					dstrooms, &dstpos)) {
			result = CDRESULT_COLLISION;
		} else if (xdiff > halfradius || zdiff > halfradius || xdiff < -halfradius || zdiff < -halfradius) {
			result = cdExamCylMove06(&g_Vars.currentplayer->prop->pos,
					g_Vars.currentplayer->prop->rooms,
					&dstpos, dstrooms, radius, types, 1,
					ymax - g_Vars.currentplayer->prop->pos.y,
					ymin - g_Vars.currentplayer->prop->pos.y);

			if (result == CDRESULT_NOCOLLISION) {
#ifndef PLATFORM_N64
				result = cdExamCylMove02(&g_Vars.currentplayer->prop->pos,
						&dstpos, radius,
						bwalkCdRooms(dstrooms, &dstpos, radius, ymin - g_Vars.currentplayer->prop->pos.y,
							ymax - g_Vars.currentplayer->prop->pos.y, cdrooms, ARRAYCOUNT(cdrooms) - 1),
						types, true,
						ymax - g_Vars.currentplayer->prop->pos.y,
						ymin - g_Vars.currentplayer->prop->pos.y);
#else
				result = cdExamCylMove02(&g_Vars.currentplayer->prop->pos,
						&dstpos, radius, dstrooms, types, true,
						ymax - g_Vars.currentplayer->prop->pos.y,
						ymin - g_Vars.currentplayer->prop->pos.y);
#endif
			}
		} else {
#ifndef PLATFORM_N64
			result = cdExamCylMove02(&g_Vars.currentplayer->prop->pos,
					&dstpos, radius,
					bwalkCdRooms(sp64, &dstpos, radius, ymin - g_Vars.currentplayer->prop->pos.y,
						ymax - g_Vars.currentplayer->prop->pos.y, cdrooms, ARRAYCOUNT(cdrooms) - 1),
					types, true,
					ymax - g_Vars.currentplayer->prop->pos.y,
					ymin - g_Vars.currentplayer->prop->pos.y);
#else
			result = cdExamCylMove02(&g_Vars.currentplayer->prop->pos,
					&dstpos, radius, sp64, types, true,
					ymax - g_Vars.currentplayer->prop->pos.y,
					ymin - g_Vars.currentplayer->prop->pos.y);
#endif
		}

		propSetPerimEnabled(g_Vars.currentplayer->prop, true);

		if (g_Vars.currentplayer->tank) {
			propSetPerimEnabled(g_Vars.currentplayer->tank, true);
		}
	}

	if (result == CDRESULT_NOCOLLISION && apply) {
		f32 angle = g_Vars.currentplayer->vv_theta + (rotateamount * 360) / M_BADTAU;

		while (angle < 0) {
			angle += 360;
		}

		while (angle >= 360) {
			angle -= 360;
		}

		g_Vars.currentplayer->vv_theta = angle;

		g_Vars.currentplayer->prop->pos.x = dstpos.x;
		g_Vars.currentplayer->prop->pos.y = dstpos.y;
		g_Vars.currentplayer->prop->pos.z = dstpos.z;

		if (copyrooms) {
			propDeregisterRooms(g_Vars.currentplayer->prop);
			roomsCopy(dstrooms, g_Vars.currentplayer->prop->rooms);
		}
	}

	g_Vars.enableslopes = true;

	return result;
}

bool bwalkCalculateNewPositionWithPush(struct coord *delta, f32 rotateamount, bool apply, f32 extrawidth, s32 types)
{
	s32 result = bwalkCalculateNewPosition(delta, rotateamount, apply, extrawidth, types);

	if (result != CDRESULT_NOCOLLISION) {
		struct prop *obstacle = cdGetObstacleProp();

		if (obstacle && g_Vars.lvupdate240 > 0) {
			if (obstacle->type == PROPTYPE_DOOR) {
				struct doorobj *door = obstacle->door;
				struct coord sp90;
				struct coord sp84;
				struct coord sp78;

				if (door->doorflags & DOORFLAG_DAMAGEONCONTACT) {
					if (!g_Vars.currentplayer->isdead) {
#if VERSION >= VERSION_NTSC_1_0
						cdGetEdge(&sp84, &sp78, 465, "bondwalk.c");
#else
						cdGetEdge(&sp84, &sp78, 460, "bondwalk.c");
#endif

						sp90.x = sp78.f[2] - sp84.f[2];
						sp90.y = 0;
						sp90.z = sp84.f[0] - sp78.f[0];

						if (sp90.f[0] || sp90.f[2]) {
							guNormalize(&sp90.x, &sp90.y, &sp90.z);
						} else {
							sp90.z = 1;
						}

						chrDamageByLaser(g_Vars.currentplayer->prop->chr, 0.4f, &sp90, 0, g_Vars.currentplayer->prop);

						// Laser zap sound
						sndStart(var80095200, SFX_PICKUP_LASER, 0, -1, -1, -1, -1, -1);
					}
				}
			} else if (obstacle->type == PROPTYPE_CHR) {
				struct chrdata *chr = obstacle->chr;
				struct coord newpos;
				RoomNum newrooms[8];
				f32 movingdist;
				f32 xdist;
				f32 zdist;
				f32 disttochr;
				bool canpush = false;

				if (g_Vars.normmplayerisrunning) {
					if (chrCompareTeams(g_Vars.currentplayer->prop->chr, chr, COMPARE_FRIENDS)) {
						// AI bot on same team
						canpush = true;
					}
				} else if (chr->chrflags & CHRCFLAG_PUSHABLE) {
					if (g_Vars.antiplayernum < 0
							|| g_Vars.currentplayer != g_Vars.anti
							|| (chr->hidden & CHRHFLAG_ANTINONINTERACTABLE) == 0) {
						canpush = true;
					}
				}

				if (canpush) {
					movingdist = sqrtf(delta->f[0] * delta->f[0] + delta->f[2] * delta->f[2]) / LVUPDATE60FREAL();

					xdist = obstacle->pos.x - g_Vars.currentplayer->prop->pos.x;
					zdist = obstacle->pos.z - g_Vars.currentplayer->prop->pos.z;

					if (xdist || zdist) {
						disttochr = sqrtf(xdist * xdist + zdist * zdist);

						if (disttochr > 0) {
							disttochr = movingdist / disttochr;

							xdist *= disttochr;
							zdist *= disttochr;

							chr->pushspeed[0] = 0.5f * xdist;
							chr->pushspeed[1] = 0.5f * zdist;

							newpos.x = obstacle->pos.x + chr->pushspeed[0] * LVUPDATE60FREAL();
							newpos.y = obstacle->pos.y;
							newpos.z = obstacle->pos.z + chr->pushspeed[1] * LVUPDATE60FREAL();

							chrCalculatePushPos(chr, &newpos, newrooms, false);

							obstacle->pos.x = newpos.x;
							obstacle->pos.y = newpos.y;
							obstacle->pos.z = newpos.z;

							propDeregisterRooms(obstacle);
							roomsCopy(newrooms, obstacle->rooms);
							chr0f0220ac(chr);
							modelSetRootPosition(chr->model, &newpos);

							result = bwalkCalculateNewPosition(delta, rotateamount, apply, extrawidth, types);
						}
					}
				}
			} else if (obstacle->type == PROPTYPE_PLAYER) {
				// empty
			} else if (obstacle->type == PROPTYPE_OBJ) {
				struct defaultobj *obj = obstacle->obj;
				bool dothething;

				if ((obj->hidden & OBJHFLAG_MOUNTED) == 0 && (obj->hidden & OBJHFLAG_GRABBED) == 0) {
					if (g_Vars.currentplayer->unk1af0 == 0 && obj->type == OBJTYPE_TANK) {
						g_Vars.currentplayer->tank = obstacle;
					} else if (obj->flags3 & OBJFLAG3_PUSHABLE) {
						g_Vars.currentplayer->speedmaxtime60 = 0;
						dothething = true;

						if ((obj->hidden & OBJHFLAG_PROJECTILE) &&
								(obj->projectile->flags & PROJECTILEFLAG_00001000)) {
							dothething = false;
						}

						if (dothething) {
							bwalk0f0c3b38(delta, obj);

							if (obj->hidden & OBJHFLAG_PROJECTILE && (obj->projectile->flags & PROJECTILEFLAG_SLIDING)) {
								bool somevalue;
								bool embedded = false;
								somevalue = projectileTick(obj, &embedded);

								if (obj->hidden & OBJHFLAG_PROJECTILE) {
									obj->projectile->flags |= PROJECTILEFLAG_00001000;

									if (somevalue) {
										obj->projectile->flags |= PROJECTILEFLAG_00002000;
									} else {
										obj->projectile->flags &= ~PROJECTILEFLAG_00002000;
									}
								}

								if (somevalue) {
									result = bwalkCalculateNewPosition(delta, rotateamount, apply, extrawidth, types);
								}
							}
						}
					}
				}
			}
		}
	}

	return result;
}

s32 bwalk0f0c4764(struct coord *delta, struct coord *arg1, struct coord *arg2, s32 types)
{
	s32 result = bwalkCalculateNewPositionWithPush(delta, 0, true, 0, types);

	if (result == CDRESULT_COLLISION) {
#if VERSION >= VERSION_NTSC_1_0
		cdGetEdge(arg1, arg2, 607, "bondwalk.c");
#else
		cdGetEdge(arg1, arg2, 602, "bondwalk.c");
#endif
	}

	return result;
}

s32 bwalk0f0c47d0(struct coord *a, struct coord *b, struct coord *c,
		struct coord *d, struct coord *e, s32 types)
{
	struct coord quarter;
	bool result;

	if (cd00024ea4()) {
		f32 mult = cd00024e98();
		quarter.x = a->x * mult * 0.25f;
		quarter.y = a->y * mult * 0.25f;
		quarter.z = a->z * mult * 0.25f;
		result = bwalkCalculateNewPositionWithPush(&quarter, 0, true, 0, types);

		if (result == CDRESULT_NOCOLLISION) {
			return CDRESULT_NOCOLLISION;
		}

		if (result == CDRESULT_COLLISION) {
#if VERSION >= VERSION_NTSC_1_0
			cdGetEdge(d, e, 635, "bondwalk.c");
#else
			cdGetEdge(d, e, 630, "bondwalk.c");
#endif

			if (b->x != d->x
					|| b->y != d->y
					|| b->z != d->z
					|| c->x != e->x
					|| c->y != e->y
					|| c->z != e->z) {
				return CDRESULT_COLLISION;
			}
		}
	}

	return CDRESULT_ERROR;
}

s32 bwalk0f0c494c(struct coord *a, struct coord *b, struct coord *c, s32 types)
{
	if (b->f[0] != c->f[0] || b->f[2] != c->f[2]) {
		f32 tmp;
		struct coord sp38;
		struct coord sp2c;

		sp38.x = c->x - b->x;
		sp38.y = 0;
		sp38.z = c->z - b->z;

		tmp = sqrtf(sp38.f[0] * sp38.f[0] + sp38.f[2] * sp38.f[2]);

		sp38.x *= 1.0f / tmp;
		sp38.z *= 1.0f / tmp;

		tmp = a->f[0] * sp38.f[0] + a->f[2] * sp38.f[2];

		sp2c.x = sp38.x * tmp;
		sp2c.y = 0;
		sp2c.z = sp38.z * tmp;

		return bwalkCalculateNewPositionWithPush(&sp2c, 0, true, 0, types);
	}

	return -1;
}

s32 bwalk0f0c4a5c(struct coord *arg0, struct coord *arg1, struct coord *arg2, s32 types)
{
	struct coord sp34;
	struct coord sp28;
	f32 ymax;
	f32 ymin;
	f32 tmp;
	f32 radius;

	playerGetBbox(g_Vars.currentplayer->prop, &radius, &ymax, &ymin);

	sp34.x = arg1->x - (g_Vars.currentplayer->prop->pos.x + arg0->f[0]);
	sp34.z = arg1->z - (g_Vars.currentplayer->prop->pos.z + arg0->f[2]);

	if (sp34.f[0] * sp34.f[0] + sp34.f[2] * sp34.f[2] <= radius * radius) {
		if (arg1->f[0] != g_Vars.currentplayer->prop->pos.f[0] || arg1->f[2] != g_Vars.currentplayer->prop->pos.f[2]) {
			sp34.x = -(arg1->z - g_Vars.currentplayer->prop->pos.z);
			sp34.y = 0;
			sp34.z = arg1->x - g_Vars.currentplayer->prop->pos.x;

			tmp = sqrtf(sp34.f[0] * sp34.f[0] + sp34.f[2] * sp34.f[2]);

			sp34.x = sp34.f[0] * (1.0f / tmp);
			sp34.z = sp34.f[2] * (1.0f / tmp);

			tmp = arg0->f[0] * sp34.f[0] + arg0->f[2] * sp34.f[2];

			sp34.x = sp34.x * tmp;
			sp34.z = sp34.z * tmp;

			sp28.x = sp34.x;
			sp28.y = 0;
			sp28.z = sp34.z;

			if (bwalkCalculateNewPositionWithPush(&sp28, 0, true, 0, types) == CDRESULT_NOCOLLISION) {
				return true;
			}
		}
	} else {
		sp34.x = arg2->x - (g_Vars.currentplayer->prop->pos.x + arg0->f[0]);
		sp34.z = arg2->z - (g_Vars.currentplayer->prop->pos.z + arg0->f[2]);

		if (sp34.f[0] * sp34.f[0] + sp34.f[2] * sp34.f[2] <= radius * radius) {
			if (arg2->f[0] != g_Vars.currentplayer->prop->pos.f[0] || arg2->f[2] != g_Vars.currentplayer->prop->pos.f[2]) {
				sp34.x = -(arg2->z - g_Vars.currentplayer->prop->pos.z);
				sp34.y = 0;
				sp34.z = arg2->x - g_Vars.currentplayer->prop->pos.x;

				tmp = sqrtf(sp34.f[0] * sp34.f[0] + sp34.f[2] * sp34.f[2]);

				sp34.x = sp34.f[0] * (1.0f / tmp);
				sp34.z = sp34.f[2] * (1.0f / tmp);

				tmp = arg0->f[0] * sp34.f[0] + arg0->f[2] * sp34.f[2];

				sp34.x = sp34.x * tmp;
				sp34.z = sp34.z * tmp;

				sp28.x = sp34.x;
				sp28.y = 0;
				sp28.z = sp34.z;

				if (bwalkCalculateNewPositionWithPush(&sp28, 0, true, 0, types) == CDRESULT_NOCOLLISION) {
					return true;
				}
			}
		}
	}

	return false;
}

void bwalk0f0c4d98(void)
{
	// empty
}

void bwalkUpdateSpeedSideways(f32 targetspeed, f32 accelspeed, s32 mult)
{
	if (g_Vars.normmplayerisrunning) {
		targetspeed = (g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].base.unk1c + 25.0f) / 100 * targetspeed;
	}

	if (g_Vars.currentplayer->speedstrafe > targetspeed) {
		g_Vars.currentplayer->speedstrafe -= PALUPF(accelspeed * mult);

		if (g_Vars.currentplayer->speedstrafe < targetspeed) {
			g_Vars.currentplayer->speedstrafe = targetspeed;
		}
	} else if (g_Vars.currentplayer->speedstrafe < targetspeed) {
		g_Vars.currentplayer->speedstrafe += PALUPF(accelspeed * mult);

		if (g_Vars.currentplayer->speedstrafe > targetspeed) {
			g_Vars.currentplayer->speedstrafe = targetspeed;
		}
	}

	g_Vars.currentplayer->speedsideways = g_Vars.currentplayer->speedstrafe;
}

void bwalkUpdateSpeedForwards(f32 targetspeed, f32 accelspeed)
{
	if (g_Vars.normmplayerisrunning) {
		targetspeed = (g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].base.unk1c + 25.0f) / 100 * targetspeed;
	}

	if (g_Vars.currentplayer->speedgo < targetspeed) {
		g_Vars.currentplayer->speedgo += accelspeed * g_Vars.lvupdate60freal;

		if (g_Vars.currentplayer->speedgo > targetspeed) {
			g_Vars.currentplayer->speedgo = targetspeed;
		}
	} else if (g_Vars.currentplayer->speedgo > targetspeed) {
		g_Vars.currentplayer->speedgo -= accelspeed * g_Vars.lvupdate60freal;

		if (g_Vars.currentplayer->speedgo < targetspeed) {
			g_Vars.currentplayer->speedgo = targetspeed;
		}
	}

	g_Vars.currentplayer->speedforwards = g_Vars.currentplayer->speedgo;
}

void bwalkUpdateVertical(void)
{
	s32 i;
	f32 newfallspeed;
	f32 radius;
	f32 ymax;
	f32 ymin;
	f32 ground;
	bool onladder;
	bool onladder2 = false;
	RoomNum rooms[8];
#ifndef PLATFORM_N64
	RoomNum ladderroomsbuf[21];
	RoomNum *ladderrooms;
#define LADDERROOMS(stock) (geRoomActive() ? ladderrooms : (stock))
#else
#define LADDERROOMS(stock) (stock)
#endif
	struct coord testpos;
	struct coord newpos;
	RoomNum newrooms[8];
	s32 newinlift;
	struct prop *lift = NULL;
	f32 sumground;
	f32 moveamount;
#if VERSION >= VERSION_NTSC_1_0
	f32 limit;
	f32 amount;
	struct prop *prop;
#endif
	f32 newmanground;
	f32 fallspeed;
	f32 eyeheight;
	f32 multiplier;
#if VERSION >= VERSION_NTSC_1_0
	struct defaultobj *obj;
#endif

	playerGetBbox(g_Vars.currentplayer->prop, &radius, &ymax, &ymin);

#if VERSION >= VERSION_NTSC_1_0
	// Maybe reset counter-op's radius - not sure why
	// Maybe it gets set to 0 when they die?
	if (g_Vars.antiplayernum >= 0
			&& g_Vars.currentplayer == g_Vars.anti
			&& g_Vars.currentplayer->bond2.radius != 30
			&& cdTestVolume(&g_Vars.currentplayer->prop->pos, 30, g_Vars.currentplayer->prop->rooms, CDTYPE_ALL, CHECKVERTICAL_YES, ymax - g_Vars.currentplayer->prop->pos.y, ymin - g_Vars.currentplayer->prop->pos.y)) {
		g_Vars.currentplayer->prop->chr->radius = 30;
		g_Vars.currentplayer->bond2.radius = 30;
		radius = 30;
	}
#endif

	// Determine if player is on a ladder
	// If this comes up false, a second check is done... maybe checking if the
	// player is touching a ladder from a room which shares the same coordinate
	// space?
#ifndef PLATFORM_N64
	// A converted level's ladder is filed under its tile's room, which is
	// seldom the room of the floor at its head: Cradle's shaft ladder is room
	// 7's, the deck round its hatch room 8's, and from the deck it was never
	// found at all (F3 report 20260926-005903). So it is looked for in every
	// room near the player too (bwalkCdRooms()).
	ladderrooms = bwalkCdRooms(g_Vars.currentplayer->prop->rooms, &g_Vars.currentplayer->prop->pos,
			radius * 2.0f, g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->prop->pos.y - 10,
			ymax - g_Vars.currentplayer->prop->pos.y, ladderroomsbuf, ARRAYCOUNT(ladderroomsbuf) - 1);
#endif

	onladder = cdFindLadder(&g_Vars.currentplayer->prop->pos,
			radius * 1.2f, ymax - g_Vars.currentplayer->prop->pos.y,
			g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->prop->pos.y + 1,
			LADDERROOMS(g_Vars.currentplayer->prop->rooms), GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY,
			&g_Vars.currentplayer->laddernormal);

	if (!onladder) {
		testpos.x = g_Vars.currentplayer->prop->pos.x;
		testpos.y = g_Vars.currentplayer->prop->pos.y - 10;
		testpos.z = g_Vars.currentplayer->prop->pos.z;
		roomsCopy(g_Vars.currentplayer->prop->rooms, rooms);
		bmoveFindEnteredRoomsByPos(g_Vars.currentplayer, &testpos, rooms);
		onladder2 = cdFindLadder(&g_Vars.currentplayer->prop->pos,
				radius * 1.1f, ymax - g_Vars.currentplayer->prop->pos.y,
				g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->prop->pos.y - 10,
				LADDERROOMS(rooms), GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY, &g_Vars.currentplayer->laddernormal);
	}

	testpos.x = g_Vars.currentplayer->prop->pos.x;
	testpos.y = g_Vars.currentplayer->prop->pos.y;
	testpos.z = g_Vars.currentplayer->prop->pos.z;

	if (g_Vars.currentplayer->inlift) {
		testpos.y -= g_Vars.currentplayer->crouchheight + g_Vars.currentplayer->crouchoffsetrealsmall;
	}

	roomsCopy(g_Vars.currentplayer->prop->rooms, rooms);
	bmoveFindEnteredRoomsByPos(g_Vars.currentplayer, &testpos, rooms);
	ground = cdFindGroundInfoAtCyl(&testpos, g_Vars.currentplayer->bond2.radius, rooms,
			&g_Vars.currentplayer->floorcol, &g_Vars.currentplayer->floortype,
			&g_Vars.currentplayer->floorflags, &g_Vars.currentplayer->floorroom,
			&newinlift, &lift);

#ifndef PLATFORM_N64
	// A converted GoldenEye level files each tile under GoldenEye's own room
	// for it, and GoldenEye found the tile under the player without asking
	// which room they were in - it has no room for a player at all, only the
	// tile they stand on, and the room is the tile's (geroom.h). Perfect Dark's
	// search only ever looks at the rooms it is handed, and those change only
	// when a move passes through a portal's polygon, which GoldenEye's portals
	// were never made for. Where the two disagree there was no floor under the
	// player ("fell through right here on runway": a strip of Runway's snow
	// eight hundred units wide carries tiles of room 14 while a walker on it is
	// in room 13 alone), or the floor of the storey below in place of the one
	// they were standing on.
	//
	// So the floor is asked of every room whose box holds the player as well,
	// and bwalkTick() then moves them into the room of the tile they are on.
	// This used to be two fallbacks taken only when the first answer was
	// nothing at all, which held the player up and left them in the wrong room
	// - drawing from it, which is the void a player walked into past a portal.
	if (geRoomActive()) {
		ground = geRoomGround(&testpos, g_Vars.currentplayer->bond2.radius, rooms,
				&g_Vars.currentplayer->floorcol, &g_Vars.currentplayer->floortype,
				&g_Vars.currentplayer->floorflags, &g_Vars.currentplayer->floorroom,
				&newinlift, &lift);
	}
#endif

	ground += g_Vars.currentplayer->bondonground;

	if (ground < -30000) {
		ground = -30000;
	}

#if PIRACYCHECKS
	if (g_Vars.currentplayer->inlift && newinlift == false) {
		// Exiting a lift
		piracyRestore();
	}
#endif

	if (g_Vars.currentplayer->inlift && newinlift && g_Vars.currentplayer->onladder == false) {
		// Remaining in a lift
		moveamount = ground - g_Vars.currentplayer->vv_ground;

#if VERSION >= VERSION_NTSC_1_0
		if (moveamount != 0)
#endif
		{
			// The lift is moving
			if (g_Vars.currentplayer->isfalling == false && lift == g_Vars.currentplayer->lift) {
				if (g_Vars.currentplayer->liftground - g_Vars.currentplayer->vv_manground < 1.0f
						&& g_Vars.currentplayer->liftground - g_Vars.currentplayer->vv_manground > -1.0f) {
					// It's actually moving, and not a floating point precision issue
					g_Vars.currentplayer->vv_ground += moveamount;

					if (moveamount > 0
							|| lift == NULL
							|| lift->obj == NULL
							|| (lift->obj->flags & OBJFLAG_CHOPPER_INACTIVE) == 0
							|| bwalkTryMoveUpwards(moveamount) == CDRESULT_NOCOLLISION) {
						// Going up
						g_Vars.currentplayer->vv_manground += moveamount;
						g_Vars.currentplayer->sumground = g_Vars.currentplayer->vv_manground / (PAL ? 0.054400026798248f : 0.045499980449677f);
					}
				}
			}

			if (g_Vars.currentplayer->walkinitmove) {
				g_Vars.currentplayer->walkinitstart.y += moveamount;
			}
		}
	} else {
		lift = NULL;
	}

	g_Vars.currentplayer->inlift = newinlift;

	if (newinlift) {
		g_Vars.currentplayer->liftground = ground;
	}

	g_Vars.currentplayer->lift = lift;

#ifndef PLATFORM_N64
	// A converted level's ladder, taken from the top. Perfect Dark takes hold
	// of a ladder that reaches over the player's feet (the test above asks
	// from manground + 1 up), and its own ladders come up through a hatch and
	// stand proud of the floor. GoldenEye's end level with the floor at their
	// head, which is straight on over the top, and it takes hold of Bond there
	// through the tile's link - so a converted ladder could be climbed and
	// never climbed down: stepping off the top was a fall its whole height.
	// (Raising the ladder's head instead does not work: a ladder's normal is
	// turned to face the player, so from the floor behind it the head is
	// climbed and then fallen off.) The second test already finds a ladder
	// whose head is just under the feet; where it does and the player has
	// stepped out over the drop, that is the ladder taken from the top.
	if (geRoomActive()) {
		// The drop is only found once the player's circle is clear of the
		// floor's edge - a radius out from a ladder at the edge - and one step
		// more is past the reach of the test above: from the deck round
		// Cradle's shaft the player walked out across the hatch, fell, and hung
		// in its far lip. So at the drop the ladder is looked for twice as far
		// out, and the player is put back to just clear of the edge, where the
		// hold is kept. It is looked for a step's height down too: Dam's three
		// short ladders have their heads at the foot of a ramp 34 under the
		// deck, and a player walking off the deck was never within 10 of one;
		// they are let down onto the head.
		if (!onladder && !onladder2 && !g_Vars.currentplayer->onladder
				&& ground < g_Vars.currentplayer->vv_manground - 30.0f) {
			struct coord normal;
			f32 dist;
			f32 top;

			if (cdFindLadderDist(&g_Vars.currentplayer->prop->pos,
						radius * 2.0f, ymax - g_Vars.currentplayer->prop->pos.y,
						g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->prop->pos.y - 60,
						ladderrooms, GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY, &normal, &dist, &top)) {
				const f32 want = g_Vars.currentplayer->bond2.radius + 1.5f;

				onladder2 = true;
				g_Vars.currentplayer->laddernormal = normal;

				if (dist > want) {
					RoomNum backrooms[21];

					guNormalize(&normal.x, &normal.y, &normal.z);

					newpos.x = g_Vars.currentplayer->prop->pos.x - normal.x * (dist - want);
					newpos.y = g_Vars.currentplayer->prop->pos.y;
					newpos.z = g_Vars.currentplayer->prop->pos.z - normal.z * (dist - want);

					propSetPerimEnabled(g_Vars.currentplayer->prop, false);

					if (cdTestVolume(&newpos, radius,
								bwalkCdRooms(g_Vars.currentplayer->prop->rooms, &newpos, radius,
									ymin - g_Vars.currentplayer->prop->pos.y, ymax - g_Vars.currentplayer->prop->pos.y,
									backrooms, ARRAYCOUNT(backrooms) - 1),
								CDTYPE_BG, CHECKVERTICAL_YES,
								ymax - g_Vars.currentplayer->prop->pos.y,
								ymin - g_Vars.currentplayer->prop->pos.y) == CDRESULT_NOCOLLISION) {
						g_Vars.currentplayer->prop->pos.x = newpos.x;
						g_Vars.currentplayer->prop->pos.z = newpos.z;
					}

					propSetPerimEnabled(g_Vars.currentplayer->prop, true);
				}

				// the hold is on a ladder reaching over the feet
				if (top - 2.0f < g_Vars.currentplayer->vv_manground
						&& bwalkTryMoveUpwards(top - 2.0f - g_Vars.currentplayer->vv_manground) == CDRESULT_NOCOLLISION) {
					g_Vars.currentplayer->vv_manground = top - 2.0f;
				}
			}
		}

		if (!onladder && onladder2 && ground < g_Vars.currentplayer->vv_manground - 30.0f) {
			onladder = true;

			if (!g_Vars.currentplayer->onladder) {
				g_GeLadderTopPlayer = g_Vars.currentplayer;
			}
		} else if (!onladder && g_GeLadderTopPlayer == g_Vars.currentplayer) {
			g_GeLadderTopPlayer = NULL;
		}
	}
#endif

	// Ladders
	if (g_Vars.currentplayer->onladder) {
		if (g_Vars.currentplayer->ladderupdown >= 0 ||
				(ground <= g_Vars.currentplayer->vv_manground &&
				 ground <= g_Vars.currentplayer->vv_manground + g_Vars.currentplayer->ladderupdown)) {
			// Still on ladder
			if (bwalkTryMoveUpwards(g_Vars.currentplayer->ladderupdown) == CDRESULT_NOCOLLISION) {
				g_Vars.currentplayer->vv_manground += g_Vars.currentplayer->ladderupdown;
			}
		} else {
			if (bwalkTryMoveUpwards(ground - g_Vars.currentplayer->vv_manground) == CDRESULT_NOCOLLISION) {
				g_Vars.currentplayer->vv_manground = ground;
				onladder = false;
			}
		}
	}

	g_Vars.currentplayer->onladder = onladder;

	if (g_Vars.currentplayer->onladder) {
		g_Vars.currentplayer->vv_ground = g_Vars.currentplayer->vv_manground;
	} else if (onladder2 == false) {
		g_Vars.currentplayer->vv_ground = ground;
	}

	// Standing on flat ground, or going up stairs, ledges or ramps
	// In other words, not falling
	if (g_Vars.currentplayer->bdeltapos.y >= 0.0f
			|| g_Vars.currentplayer->vv_ground > g_Vars.currentplayer->vv_manground) {
		g_Vars.currentplayer->sumground = g_Vars.currentplayer->vv_manground / (PAL ? 0.054400026798248f : 0.045499980449677f);

		for (i = 0; i < g_Vars.lvupdate240; i++) {
			g_Vars.currentplayer->sumground =
				g_Vars.currentplayer->sumground * (PAL ? 0.94559997320175f : 0.9545f) + g_Vars.currentplayer->vv_ground;
		}

		if (g_Vars.currentplayer->vv_manground < g_Vars.currentplayer->vv_ground) {
			// Feet are lower than the ground
			sumground = g_Vars.currentplayer->sumground * (PAL ? 0.054400026798248f : 0.045499980449677f);

			if (sumground < g_Vars.currentplayer->vv_ground - 50) {
				sumground = g_Vars.currentplayer->vv_ground - 50;
			}

			if (bwalkTryMoveUpwards(sumground - g_Vars.currentplayer->vv_manground) == CDRESULT_NOCOLLISION) {
				g_Vars.currentplayer->vv_manground = sumground;
			}
#if VERSION >= VERSION_NTSC_1_0
			else {
				// Not enough room above. If on a hoverbike, blow it up
				prop = cdGetObstacleProp();

				if (prop
						&& g_Vars.currentplayer->prop->pos.y < prop->pos.y
						&& prop->type == PROPTYPE_OBJ) {
					obj = prop->obj;

					if (obj->modelnum == MODEL_HOVBIKE) {
						amount = (obj->maxdamage - obj->damage + 1) / 250.0f;
						obj->flags &= ~OBJFLAG_INVINCIBLE;
						objDamage(obj, amount, &obj->prop->pos, WEAPON_REMOTEMINE, -1);
					}
				}
			}
#endif
		}

		// Kill player if standing on tile with GEOFLAG_DIE
		if ((g_Vars.currentplayer->floorflags & GEOFLAG_DIE)
				&& g_Vars.currentplayer->vv_manground - 20.0f < g_Vars.currentplayer->vv_ground
				&& g_Vars.currentplayer->onladder == false
				&& onladder2 == false) {
			playerDie(true);
		}
	}

	// A jump leaves the player still standing on the ground with an upward
	// bdeltapos.y. Without the second test that velocity would sit there unread
	// until something else lifted them off the floor, so the impulse would do
	// nothing at all.
	if (g_Vars.currentplayer->vv_manground > g_Vars.currentplayer->vv_ground
			|| g_Vars.currentplayer->bdeltapos.y > 0.0f) {
		// Not standing on ground - probably falling, jumping, or on an object
		fallspeed = g_Vars.currentplayer->bdeltapos.y;
		newmanground = g_Vars.currentplayer->vv_manground;

		if (debugIsTurboModeEnabled()
				&& g_Vars.currentplayer->bondforcespeed.x == 0
				&& g_Vars.currentplayer->bondforcespeed.z == 0) {
			multiplier = 0.277777777f * 5;
		} else {
			multiplier = 0.277777777f;
		}

		newfallspeed = fallspeed - g_Vars.lvupdate60freal * multiplier;
		newmanground += g_Vars.lvupdate60freal * (fallspeed + newfallspeed) * 0.5f;
		fallspeed = newfallspeed;

		if (newmanground < g_Vars.currentplayer->vv_ground) {
			newfallspeed = g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->vv_ground;
			newmanground = g_Vars.currentplayer->vv_ground;

			fallspeed = sqrtf(g_Vars.currentplayer->bdeltapos.y *
					g_Vars.currentplayer->bdeltapos.y +
					(((newfallspeed + newfallspeed) * 0.277777777f) / 60.0f) * 60.0f);
			fallspeed = -fallspeed;
		}

		if (bwalkTryMoveUpwards(newmanground - g_Vars.currentplayer->vv_manground) == CDRESULT_NOCOLLISION) {
			// Falling
			g_Vars.currentplayer->vv_manground = newmanground;
			g_Vars.currentplayer->bdeltapos.y = fallspeed;

			if (g_Vars.currentplayer->isfalling == false) {
				// Just started falling
				g_Vars.currentplayer->isfalling = true;
				g_Vars.currentplayer->fallstart = g_Vars.lvframe60;
			} else {
				if (geRoomActive() && !g_Vars.normmplayerisrunning) {
					// GoldenEye has no clock on a fall, and its missions
					// count on that: Dam's dive is 350 ticks of one before
					// the list fades to the ending. Only a fall out of the
					// world kills.
					if (newmanground <= -30000) {
						playerDie(true);
					}
				} else if (g_Vars.lvframe60 - g_Vars.currentplayer->fallstart > TICKS(240)) {
					// Have been falling for 4 seconds
					playerDie(true);
				}
			}
		} else {
			// Not falling
#if VERSION >= VERSION_NTSC_1_0
			if (g_Vars.normmplayerisrunning == false
					&& g_Vars.currentplayer->vv_ground < g_Vars.currentplayer->vv_manground - 30) {
				// Not falling - but still at least 30 units off the ground.
				// Must be something in the way...
				prop = cdGetObstacleProp();

				if (prop) {
					if (prop->type == PROPTYPE_CHR) {
						// Landed on top of a chr
						if (prop->chr->inlift) {
							chrYeetFromPos(prop->chr, &g_Vars.currentplayer->prop->pos, 0);
						}
					} else if (prop->type == PROPTYPE_PLAYER) {
						// Landed on top of a player
						u32 prevplayernum = g_Vars.currentplayernum;
						setCurrentPlayerNum(playermgrGetPlayerNumByProp(prop));

						if (g_Vars.currentplayer->inlift) {
							playerDieByShooter(prevplayernum, true);
						}

						setCurrentPlayerNum(prevplayernum);
					}
				}
			}
#endif

			g_Vars.currentplayer->bdeltapos.y = VERSION >= VERSION_NTSC_1_0 ? 0.0f : 0;

			if (g_Vars.currentplayer->isfalling) {
				g_Vars.currentplayer->isfalling = false;
			}

			if (g_Vars.currentplayer->vv_manground <= -30000) {
				playerDie(true);
			}
		}
	} else {
		// Not falling
		if (g_Vars.currentplayer->isfalling) {
			g_Vars.currentplayer->isfalling = false;
		}

		if (g_Vars.currentplayer->vv_manground <= -30000) {
			playerDie(true);
		}
	}

	if (g_Vars.currentplayer->bdeltapos.y < 0 &&
			g_Vars.currentplayer->vv_manground <= g_Vars.currentplayer->vv_ground) {
		// Landing after a fall
		if (g_Vars.currentplayer->isfalling) {
			g_Vars.currentplayer->isfalling = false;
		}

		// I suspect these crouch fields are related to the recovery during
		// landing. Eg. The faster the fall speed, the longer Jo will take to
		// stand back to full height again.
		if (g_Vars.currentplayer->bdeltapos.y < -13.333333f) {
			g_Vars.currentplayer->crouchtime240 = TICKS(60);
			g_Vars.currentplayer->crouchfall = -90;
		} else if (g_Vars.currentplayer->bdeltapos.y < -5.0f) {
			g_Vars.currentplayer->crouchtime240 = TICKS(60);
			g_Vars.currentplayer->crouchfall =
				(-5.0f - g_Vars.currentplayer->bdeltapos.y) * -90.0f / 8.333333f;
		}

		if (g_Vars.currentplayer->bdeltapos.y < -6.0f) {
			// Play footstep sounds
			s32 sound;
			struct chrdata *chr = g_Vars.currentplayer->prop->chr;
			chr->floortype = g_Vars.currentplayer->floortype;
			chr->footstep = 1;

			sound = footstepChooseSound(chr, true);

			if (sound != -1) {
				if (sound != -1) {
					psCreate(NULL, g_Vars.currentplayer->prop, sound,
							-1, -1, PSFLAG_0400 | PSFLAG_IGNOREROOMS, 0, PSTYPE_NONE, 0, -1, NULL, -1, -1, -1, -1);
				}

				chr->footstep = 2;
				sound = footstepChooseSound(chr, true);

				if (sound != -1) {
					psCreate(NULL, g_Vars.currentplayer->prop, sound,
							-1, -1, PSFLAG_0400 | PSFLAG_IGNOREROOMS, 0, PSTYPE_NONE, 0, -1, NULL, -1, -1, -1, -1);
				}
			}

			if (g_Vars.mplayerisrunning == false
					&& (chr->headnum == HEAD_DARK_COMBAT || chr->headnum == HEAD_DARK_FROCK)
					&& g_Vars.lvframe60 - g_Vars.currentplayer->fallstart > TICKS(40)) {
				// Play Jo landing grunt
				s32 sounds[] = {
					SFX_JO_LANDING_046F,
					SFX_JO_LANDING_05B6,
					SFX_JO_LANDING_05B7
				};

				psCreate(NULL, g_Vars.currentplayer->prop, sounds[rngRandom() % 3],
						-1, -1, PSFLAG_0400 | PSFLAG_IGNOREROOMS, 0, PSTYPE_NONE, 0, -1, NULL, -1, -1, -1, -1);
			}
		}

		g_Vars.currentplayer->bdeltapos.y = 0;
	}

	// Decrease crouchtime240 for this tick.
	// If reached 0 and crouchfall is negative, start increasing
	// crouchfall over the next several ticks until it reaches 0.
	for (i = 0; i < g_Vars.lvupdate240; i++) {
		if (g_Vars.currentplayer->crouchtime240 > 0) {
			g_Vars.currentplayer->sumcrouch =
				g_Vars.currentplayer->sumcrouch * (PAL ? 0.93540000915527f : 0.9456f) + g_Vars.currentplayer->crouchfall;
			g_Vars.currentplayer->crouchtime240--;
		} else {
			if (g_Vars.currentplayer->crouchfall < 0) {
				g_Vars.currentplayer->crouchfall -= (PAL ? -1.3636363744736f : -1.125f);

				if (g_Vars.currentplayer->crouchfall >= 0) {
					g_Vars.currentplayer->crouchfall = 0;
				}
			}

			g_Vars.currentplayer->sumcrouch =
				g_Vars.currentplayer->sumcrouch * (PAL ? 0.93540000915527f : 0.9456f) + g_Vars.currentplayer->crouchfall;
		}
	}

	{
		g_Vars.currentplayer->crouchheight = g_Vars.currentplayer->sumcrouch * (PAL ? 0.064599990844727f : 0.054400026798248f);
		g_Vars.currentplayer->vv_height =
			(g_Vars.currentplayer->headpos.y / g_Vars.currentplayer->standheight)
			* g_Vars.currentplayer->vv_eyeheight;

		eyeheight = g_Vars.currentplayer->vv_height +
			g_Vars.currentplayer->crouchoffsetrealsmall +
			g_Vars.currentplayer->crouchheight *
			g_Vars.currentplayer->vv_eyeheight * 0.0062893079593778f;

		if (eyeheight < 30) {
			eyeheight = 30;
		}

#ifndef PLATFORM_N64
		eyeheight = geTankEyeHeight(eyeheight);
#endif

		newpos.x = g_Vars.currentplayer->prop->pos.x;
		newpos.y = g_Vars.currentplayer->vv_manground + eyeheight;
		newpos.z = g_Vars.currentplayer->prop->pos.z;
	}

#if VERSION >= VERSION_NTSC_1_0
	if (newpos.y < g_Vars.currentplayer->vv_ground + 10) {
		newpos.y = g_Vars.currentplayer->vv_ground + 10;
	}
#endif

	if (newpos.x != g_Vars.currentplayer->prop->pos.x
			|| newpos.y != g_Vars.currentplayer->prop->pos.y
			|| newpos.z != g_Vars.currentplayer->prop->pos.z) {
		func0f065e74(&g_Vars.currentplayer->prop->pos, g_Vars.currentplayer->prop->rooms, &newpos, newrooms);

		g_Vars.currentplayer->prop->pos.x = newpos.x;
		g_Vars.currentplayer->prop->pos.y = newpos.y;
		g_Vars.currentplayer->prop->pos.z = newpos.z;

		propDeregisterRooms(g_Vars.currentplayer->prop);
		roomsCopy(newrooms, g_Vars.currentplayer->prop->rooms);
	}
}

void bwalkApplyCrouchSpeed(void)
{
	if (bmoveGetCrouchPos() == CROUCHPOS_DUCK) {
		g_Vars.currentplayer->speedforwards *= 0.5f;
		g_Vars.currentplayer->speedsideways *= 0.5f;
	} else if (bmoveGetCrouchPos() == CROUCHPOS_SQUAT) {
		g_Vars.currentplayer->speedforwards *= 0.35f;
		g_Vars.currentplayer->speedsideways *= 0.35f;
	}
}

void bwalkUpdateCrouchOffsetReal(void)
{
	if (g_Vars.currentplayer->vv_eyeheight + -90.0f * g_Vars.currentplayer->vv_eyeheight * (1.0f / 159.0f) < 69.0f) {
		g_Vars.currentplayer->crouchoffsetreal = g_Vars.currentplayer->crouchoffset * ((69.0f - g_Vars.currentplayer->vv_eyeheight) / -90.0f);
	} else {
		g_Vars.currentplayer->crouchoffsetreal = g_Vars.currentplayer->crouchoffset * g_Vars.currentplayer->vv_eyeheight * (1.0f / 159.0f);
	}

	if (cheatIsActive(CHEAT_SMALLJO)) {
		g_Vars.currentplayer->crouchoffsetsmall = 69.0f - g_Vars.currentplayer->vv_eyeheight;
		g_Vars.currentplayer->crouchoffsetrealsmall = 69.0f - g_Vars.currentplayer->vv_eyeheight;
	} else {
		g_Vars.currentplayer->crouchoffsetsmall = g_Vars.currentplayer->crouchoffset;
		g_Vars.currentplayer->crouchoffsetrealsmall = g_Vars.currentplayer->crouchoffsetreal;
	}
}

bool bwalkCanUncrouch(void)
{
	f32 targetoffset = 0;

	if (g_Vars.currentplayer->crouchpos == CROUCHPOS_SQUAT) {
		targetoffset = -90;
	} else if (g_Vars.currentplayer->crouchpos == CROUCHPOS_DUCK) {
		targetoffset = -45;
	}

	if (targetoffset != g_Vars.currentplayer->crouchoffset) {
		f32 prevcrouchoffset = g_Vars.currentplayer->crouchoffset;
		f32 prevcrouchoffsetreal = g_Vars.currentplayer->crouchoffsetreal;
		f32 prevcrouchoffsetsmall = g_Vars.currentplayer->crouchoffsetsmall;
		f32 prevcrouchoffsetrealsmall = g_Vars.currentplayer->crouchoffsetrealsmall;
		f32 prevcrouchspeed = g_Vars.currentplayer->crouchspeed;

		g_Vars.currentplayer->crouchoffset = targetoffset;

		bwalkUpdateCrouchOffsetReal();

		const bool result = bwalkCanMoveUpwards(0);

		g_Vars.currentplayer->crouchoffset = prevcrouchoffset;
		g_Vars.currentplayer->crouchoffsetreal = prevcrouchoffsetreal;
		g_Vars.currentplayer->crouchoffsetsmall = prevcrouchoffsetsmall;
		g_Vars.currentplayer->crouchoffsetrealsmall = prevcrouchoffsetrealsmall;
		g_Vars.currentplayer->crouchspeed = prevcrouchspeed;

		return result;
	}

	return true;
}

void bwalkUpdateCrouchOffset(void)
{
	f32 targetoffset = 0;

	if (bmoveGetCrouchPos() == CROUCHPOS_SQUAT) {
		targetoffset = -90;
	} else if (bmoveGetCrouchPos() == CROUCHPOS_DUCK) {
		targetoffset = -45;
	} else if (bmoveGetCrouchPos() == CROUCHPOS_STAND) {
		// empty
	}

	if (targetoffset != g_Vars.currentplayer->crouchoffset) {
		f32 prevcrouchoffset = g_Vars.currentplayer->crouchoffset;
		f32 prevcrouchoffsetreal = g_Vars.currentplayer->crouchoffsetreal;
		f32 prevcrouchoffsetsmall = g_Vars.currentplayer->crouchoffsetsmall;
		f32 prevcrouchoffsetrealsmall = g_Vars.currentplayer->crouchoffsetrealsmall;

		// f32 *frac, f32 maxfrac, f32 *fracspeed, f32 accel, f32 decel, f32 maxspeed
		applySpeed(&g_Vars.currentplayer->crouchoffset, targetoffset,
				&g_Vars.currentplayer->crouchspeed, PALUPF(0.5f), PALUPF(0.5f), PALUPF(5.0f));

		bwalkUpdateCrouchOffsetReal();

		if (bwalkTryMoveUpwards(0) == CDRESULT_COLLISION) {
			// Crouch adjustment is blocked by ceiling
			g_Vars.currentplayer->crouchoffset = prevcrouchoffset;
			g_Vars.currentplayer->crouchoffsetreal = prevcrouchoffsetreal;
			g_Vars.currentplayer->crouchoffsetsmall = prevcrouchoffsetsmall;
			g_Vars.currentplayer->crouchoffsetrealsmall = prevcrouchoffsetrealsmall;
			g_Vars.currentplayer->crouchspeed = 0;
			bwalkAdjustCrouchPos(-1);
		}
	}

	if (targetoffset == g_Vars.currentplayer->crouchoffset) {
		g_Vars.currentplayer->crouchspeed = 0;
	}

	g_Vars.currentplayer->guncloseroffset = g_Vars.currentplayer->crouchoffset / -90;
}

void bwalkUpdateTheta(void)
{
	f32 mult;
	f32 rotateamount;
	struct coord delta = {0, 0, 0};

#ifdef PLATFORM_N64
	// Turn speed is calculated from the chr's height
	mult = 159.0f / g_Vars.currentplayer->vv_eyeheight;
#else
	// Same turn speed for all heights
	mult = 1.f;
#endif
	rotateamount = g_Vars.currentplayer->speedtheta * mult
		* g_Vars.lvupdate60freal * 0.0174505133f * 3.5f;

	bwalkCalculateNewPositionWithPush(&delta, rotateamount, true, 0, CDTYPE_ALL);
}

void bwalk0f0c63bc(struct coord *arg0, u32 arg1, s32 types)
{
	struct coord sp100;
	struct coord sp88;

	g_Vars.currentplayer->bondonturret = false;
	g_Vars.currentplayer->autocrouchpos = CROUCHPOS_STAND;

#ifndef PLATFORM_N64
	// GoldenEye's tank: one he has walked into is no obstacle to him (above,
	// where `tank` is switched off for his moves) because this is where
	// GoldenEye lifts him onto it, holding the move until he is up. Perfect
	// Dark kept the first half, and he walked through it (getank.c)
	if (geTankBoard()) {
		return;
	}

	// GoldenEye's vents and crawl spaces: a converted level has no ceiling to
	// hold the player down, and GoldenEye has none either - its tiles say
	// where Bond squats, asked where this move is going (gestan.h)
	if (geRoomActive()) {
		struct coord target;

		target.x = g_Vars.currentplayer->prop->pos.x + arg0->x;
		target.y = g_Vars.currentplayer->prop->pos.y;
		target.z = g_Vars.currentplayer->prop->pos.z + arg0->z;

		if (geStanForcesCrouch(&target, g_Vars.currentplayer->vv_manground + 40.0f,
					geStanRise(true), g_Vars.currentplayer->bond2.radius)) {
			g_Vars.currentplayer->autocrouchpos = CROUCHPOS_SQUAT;
		}
	}
#endif

	bwalk0f0c4d98();

	if (bwalk0f0c4764(arg0, &sp100, &sp88, types) == CDRESULT_COLLISION) {
		struct coord sp76;
		struct coord sp64;

		s32 result = bwalk0f0c47d0(arg0, &sp100, &sp88, &sp76, &sp64, types);

		if (result >= CDRESULT_NOCOLLISION || result <= CDRESULT_ERROR) {
			if (result >= CDRESULT_NOCOLLISION) {
				bwalk0f0c4d98();
			}

			if (arg1
					&& bwalk0f0c494c(arg0, &sp100, &sp88, types) <= CDRESULT_COLLISION
					&& bwalk0f0c4a5c(arg0, &sp100, &sp88, types) <= CDRESULT_COLLISION) {
				// empty
			}
		} else if (result == CDRESULT_COLLISION) {
			struct coord sp48;
			struct coord sp36;

			if (bwalk0f0c47d0(arg0, &sp76, &sp64, &sp48, &sp36, types) >= CDRESULT_NOCOLLISION) {
				bwalk0f0c4d98();
			}

			if (arg1
					&& bwalk0f0c494c(arg0, &sp76, &sp64, types) <= CDRESULT_COLLISION
					&& bwalk0f0c494c(arg0, &sp100, &sp88, types) <= CDRESULT_COLLISION
					&& bwalk0f0c4a5c(arg0, &sp76, &sp64, types) <= CDRESULT_COLLISION) {
				bwalk0f0c4a5c(arg0, &sp100, &sp88, types);
			}
		}
	}

	bwalk0f0c4d98();
}

void bwalkUpdatePrevPos(void)
{
	g_Vars.currentplayer->bondprevpos.x = g_Vars.currentplayer->prop->pos.x;
	g_Vars.currentplayer->bondprevpos.y = g_Vars.currentplayer->prop->pos.y;
	g_Vars.currentplayer->bondprevpos.z = g_Vars.currentplayer->prop->pos.z;

	roomsCopy(g_Vars.currentplayer->prop->rooms, g_Vars.currentplayer->bondprevrooms);
}

void bwalkHandleActivate(void)
{
	if (g_Vars.currentplayer->walkinitmove) {
		g_Vars.currentplayer->bondactivateorreload = 0;
	}
}

void bwalkApplyMoveData(struct movedata *data)
{
#ifndef PLATFORM_N64
	// driving GoldenEye's tank, the sticks are the tank's (getank.c)
	if (geTankApplyMoveData(data)) {
		return;
	}
#endif

	if (g_Vars.currentplayer->walkinitmove == false) {
		// Sideways
		if (data->digitalstepleft) {
			bwalkUpdateSpeedSideways(-1, 0.2f, data->digitalstepleft);
		} else if (data->digitalstepright) {
			bwalkUpdateSpeedSideways(1, 0.2f, data->digitalstepright);
		} else if (data->unk14 == false) {
			bwalkUpdateSpeedSideways(0, 0.2f, g_Vars.lvupdate60);
		} else if (data->unk14){
			bwalkUpdateSpeedSideways(data->analogstrafe * 0.014285714365542f, 0.2f, g_Vars.lvupdate60);
		}


		// Forward/back
		if (data->digitalstepforward) {
			bwalkUpdateSpeedForwards(1, 1);
			g_Vars.currentplayer->speedmaxtime60 += g_Vars.lvupdate60;
		} else if (data->digitalstepback) {
			bwalkUpdateSpeedForwards(-1, 1);
		} else if (data->canlookahead == false) {
			bwalkUpdateSpeedForwards(0, 1);
		} else {
			bwalkUpdateSpeedForwards(data->analogwalk * 0.014285714365542f, 1);
		}


		if (data->canlookahead) {
			if (data->analogwalk > 60) {
				g_Vars.currentplayer->speedmaxtime60 += g_Vars.lvupdate60;
			} else {
				g_Vars.currentplayer->speedmaxtime60 = 0;
			}
		}

		// Force speeds to range -1 to 1
		if (g_Vars.currentplayer->speedforwards > 1) {
			g_Vars.currentplayer->speedforwards = 1;
		}

		if (g_Vars.currentplayer->speedforwards < -1) {
			g_Vars.currentplayer->speedforwards = -1;
		}

		if (g_Vars.currentplayer->speedsideways > 1) {
			g_Vars.currentplayer->speedsideways = 1;
		}

		if (g_Vars.currentplayer->speedsideways < -1) {
			g_Vars.currentplayer->speedsideways = -1;
		}

		g_Vars.currentplayer->speedforwards *= 1.08f;
		g_Vars.currentplayer->speedforwards *= g_Vars.currentplayer->speedboost;

		if ((data->canlookahead == false && data->digitalstepforward == false) ||
				bmoveGetCrouchPos() != CROUCHPOS_STAND) {
			g_Vars.currentplayer->speedmaxtime60 = 0;
		}

#ifndef PLATFORM_N64
		if (data->rleanleft) {
			bwalkSetSwayTarget(-1);
		} else if (data->rleanright) {
			bwalkSetSwayTarget(1);
		} else if (fabsf(data->analoglean)) {
			bwalkSetSwayTargetf(data->analoglean);
		} else {
			bwalkSetSwayTarget(0);
		}
#else
		if (data->rleanleft) {
			bwalkSetSwayTarget(-1);
		} else if (data->rleanright) {
			bwalkSetSwayTarget(1);
		} else {
			bwalkSetSwayTarget(0);
		}
#endif

		while (data->crouchdown-- > 0) {
			bwalkAdjustCrouchPos(-1);
		}

		while (data->crouchup-- > 0) {
			bwalkAdjustCrouchPos(1);
		}

		g_Vars.currentplayer->eyesshut = data->eyesshut;
	}
}

void bwalkUpdateSpeedTheta(void)
{
#ifdef PLATFORM_N64
	if (bmoveGetCrouchPos() == CROUCHPOS_SQUAT) {
		g_Vars.currentplayer->speedtheta *= 0.5f;
	} else if (bmoveGetCrouchPos() == CROUCHPOS_DUCK) {
		g_Vars.currentplayer->speedtheta *= 0.75f;
	}
#endif
}

void bwalk0f0c69b8(void)
{
	s32 i;
	f32 spe0;
	f32 spdc;
	f32 spd8;
	struct coord spcc = {0, 0, 0};
	f32 spc8;
	f32 spc4;
	f32 spc0;
	f32 tmp1;
	f32 tmp2;
	f32 spb4;
	f32 spb0;
	f32 dist;
	f32 spa8;
	f32 mult;
	f32 f0;
	f32 lvupdate60f;
	s32 lvupdate240;
	s32 cdresult;
	struct escalatorobj *esc;
	f32 sp8c;
	f32 sp88;
	f32 speedforwards;
	f32 speedsideways;
	f32 speedtheta;
	f32 maxspeed;
	f32 sp74;
	f32 radius;
	f32 ymax;
	f32 ymin;
	f32 xdiff;
	f32 zdiff;
	f32 xdelta;
	f32 zdelta;
	f32 sp54;
	f32 sp50;
	f32 sp4c;
	f32 sp48;
	f32 sp44;
	f32 sp40;
	f32 sp3c;
	f32 breathing;

	spc0 = g_Vars.currentplayer->vv_eyeheight - 159;

	if (invHasBriefcase() && ((g_MpSetup.scenario == MPSCENARIO_HOLDTHEBRIEFCASE || g_MpSetup.scenario == MPSCENARIO_CAPTURETHECASE))) {
		spc0 = -63.600006f;
	}

	spc0 = spc0 / 353.33331298828f + 1.0f;

	// the multiplier is the mod's to set, and a mission cheat may give it
	// too (GE-X's cheat 6, game/modrules.h)
	if (g_Vars.normmplayerisrunning) {
		if (g_MpSetup.options & MPOPTION_FASTMOVEMENT) {
			spc0 *= g_ModFastMoveScale;
		}
	} else if (g_ModFastMoveCheat >= 0 && cheatIsActive(g_ModFastMoveCheat)) {
		spc0 *= g_ModFastMoveScale;
	}

#if VERSION >= VERSION_NTSC_1_0
	if (cheatIsActive(CHEAT_SMALLJO)) {
		spc0 *= 0.4f;
	}
#endif

	if (g_Vars.currentplayer->walkinitmove) {
		g_Vars.currentplayer->walkinitt += g_Vars.lvupdate60freal * (1.0f / 60.0f);

		if (g_Vars.currentplayer->walkinitt >= 1.0f) {
			g_Vars.currentplayer->walkinitt = 1.0f;
			g_Vars.currentplayer->walkinitmove = false;
		}

		g_Vars.currentplayer->walkinitt2 = 1.0f - (cosf(g_Vars.currentplayer->walkinitt * M_BADPI) + 1.0f) * 0.5f;

		bmoveUpdateHead(0.0f, 0.0f, 0.0f, &g_Vars.currentplayer->walkinitmtx, 1.0f - g_Vars.currentplayer->walkinitt2);

		g_Vars.currentplayer->gunspeed = 0.0f;

		bmoveUpdateMoveInitSpeed(&spcc);
		bwalkCalculateNewPositionWithPush(&spcc, 0.0f, true, 0.0f, CDTYPE_ALL);
	} else {
		bwalkApplyCrouchSpeed();
		bwalkUpdateCrouchOffset();

		bmove0f0cba88(&spc8, &spc4,
				&g_Vars.currentplayer->bondshotspeed,
				g_Vars.currentplayer->vv_sintheta, g_Vars.currentplayer->vv_costheta);

		tmp1 = -g_Vars.currentplayer->swaytarget * g_Vars.currentplayer->bond2.unk00.f[2];
		tmp2 = g_Vars.currentplayer->swaytarget * g_Vars.currentplayer->bond2.unk00.f[0];
		tmp1 *= spc0;
		tmp2 *= spc0;
		spa8 = 0.0f;

		if (g_Vars.currentplayer->crouchoffset < -45.0f) {
			tmp1 *= 0.35f;
			tmp2 *= 0.35f;
		} else if (g_Vars.currentplayer->crouchoffset < 0.0f) {
			tmp1 *= 0.5f;
			tmp2 *= 0.5f;
		}

		spb4 = tmp1 - g_Vars.currentplayer->swayoffset0;
		spb0 = tmp2 - g_Vars.currentplayer->swayoffset2;

		dist = sqrtf(spb4 * spb4 + spb0 * spb0);

		if (g_Vars.lvupdate60freal > PALUPF(4)) {
			lvupdate60f = PALUPF(4);
			lvupdate240 = 4;
		} else {
			lvupdate60f = g_Vars.lvupdate60freal;
			lvupdate240 = g_Vars.lvupdate60;
		}

		for (i = 0; i < lvupdate240; i++) {
			spa8 += (dist - spa8) * PALUPF(0.1f);
		}

		spa8 += 3.75f * lvupdate60f;

		if (g_Vars.currentplayer->crouchoffset < -45.0f) {
			spa8 *= 0.35f;
		} else if (g_Vars.currentplayer->crouchoffset < 0.0f) {
			spa8 *= 0.5f;
		}

		if (spa8 < dist) {
			spa8 /= dist;
			spb4 *= spa8;
			spb0 *= spa8;
		}

		speedsideways = (g_Vars.currentplayer->speedsideways + spc4) * 0.8f;
		speedforwards = g_Vars.currentplayer->speedforwards + spc8;
		speedtheta = g_Vars.currentplayer->speedtheta * 0.8f;

		if (speedsideways < 0.0f) {
			speedsideways = -speedsideways;
		}

		if (speedforwards < 0.0f) {
			speedforwards = -speedforwards;
		}

		if (speedtheta < 0.0f) {
			speedtheta = -speedtheta;
		}

		maxspeed = speedforwards;

		if (speedsideways > maxspeed) {
			maxspeed = speedsideways;
		}

		if (speedtheta > maxspeed) {
			maxspeed = speedtheta;
		}

		if (dist >= 0.1f && maxspeed < 0.8f) {
			maxspeed = 0.8f;
		}

		if (maxspeed >= 0.75f) {
			g_Vars.currentplayer->bondbreathing += (maxspeed - 0.75f) * g_Vars.lvupdate60freal / 900;
		} else {
			g_Vars.currentplayer->bondbreathing -= (0.75f - maxspeed) * g_Vars.lvupdate60freal / 2700;
		}

		if (g_Vars.currentplayer->bondbreathing < 0.0f) {
			g_Vars.currentplayer->bondbreathing = 0.0f;
		} else if (g_Vars.currentplayer->bondbreathing > 1.0f) {
			g_Vars.currentplayer->bondbreathing = 1.0f;
		}

		mult = g_HeadAnims[HEADANIM_MOVING].translateperframe * 0.5f * g_Vars.lvupdate60freal;
		spe0 = (g_Vars.currentplayer->speedsideways * spc0 + spc4) * mult;

#if VERSION >= VERSION_NTSC_1_0
		if (cheatIsActive(CHEAT_SMALLJO)) {
			spe0 /= 0.4f;
		}
#endif

		bmove0f0cc654(maxspeed, g_Vars.currentplayer->speedforwards * spc0 + spc8, spe0);

		g_Vars.currentplayer->gunspeed = maxspeed;

		spdc = g_Vars.currentplayer->headpos.x;
		spd8 = g_Vars.currentplayer->headpos.z;

#if VERSION >= VERSION_NTSC_1_0
		if (cheatIsActive(CHEAT_SMALLJO)) {
			spdc *= 0.4f;
		}
#endif

		spcc.f[0] += (spd8 * g_Vars.currentplayer->bond2.unk00.f[0] - spdc * g_Vars.currentplayer->bond2.unk00.f[2]) * g_Vars.lvupdate60freal;
		spcc.f[2] += (spd8 * g_Vars.currentplayer->bond2.unk00.f[2] + spdc * g_Vars.currentplayer->bond2.unk00.f[0]) * g_Vars.lvupdate60freal;
		spcc.f[0] += spb4;
		spcc.f[2] += spb0;

		bmoveUpdateMoveInitSpeed(&spcc);

		if (debugIsTurboModeEnabled()) {
			spcc.f[0] += (g_Vars.currentplayer->bond2.unk00.f[0] * g_Vars.currentplayer->speedforwards - g_Vars.currentplayer->bond2.unk00.f[2] * g_Vars.currentplayer->speedsideways) * g_Vars.lvupdate60freal * 10.0f;
			spcc.f[2] += (g_Vars.currentplayer->bond2.unk00.f[2] * g_Vars.currentplayer->speedforwards + g_Vars.currentplayer->bond2.unk00.f[0] * g_Vars.currentplayer->speedsideways) * g_Vars.lvupdate60freal * 10.0f;
		}

		if (g_Vars.currentplayer->bondforcespeed.f[0] != 0.0f || g_Vars.currentplayer->bondforcespeed.f[2] != 0.0f) {
			spcc.f[0] += g_Vars.currentplayer->bondforcespeed.f[0] * g_Vars.lvupdate60freal;
			spcc.f[2] += g_Vars.currentplayer->bondforcespeed.f[2] * g_Vars.lvupdate60freal;
		}

#ifndef PLATFORM_N64
		// The combat roll, pushed in alongside the scripted one above and taken
		// through the same collision, then decayed a tick at a time the way a
		// chr sheds the shove an explosion gave it. Movement input is left
		// alone rather than locked out, so a roll thrown while running carries
		// the run with it.
		if (g_Vars.currentplayer->rollspeed.f[0] != 0.0f || g_Vars.currentplayer->rollspeed.f[2] != 0.0f) {
			spcc.f[0] += g_Vars.currentplayer->rollspeed.f[0] * g_Vars.lvupdate60freal;
			spcc.f[2] += g_Vars.currentplayer->rollspeed.f[2] * g_Vars.lvupdate60freal;

			for (i = 0; i < g_Vars.lvupdate60; i++) {
				g_Vars.currentplayer->rollspeed.f[0] *= ROLL_DECAY;
				g_Vars.currentplayer->rollspeed.f[2] *= ROLL_DECAY;
			}

			if (g_Vars.currentplayer->rollspeed.f[0] < ROLL_STOPPED
					&& g_Vars.currentplayer->rollspeed.f[0] > -ROLL_STOPPED
					&& g_Vars.currentplayer->rollspeed.f[2] < ROLL_STOPPED
					&& g_Vars.currentplayer->rollspeed.f[2] > -ROLL_STOPPED) {
				g_Vars.currentplayer->rollspeed.f[0] = 0;
				g_Vars.currentplayer->rollspeed.f[2] = 0;
			}
		}
#endif

#ifndef PLATFORM_N64
		// and in GoldenEye's tank the step is the tank's, taken through the
		// same collision
		geTankDrive(&spcc);
#endif

		if (g_Vars.currentplayer->onladder) {
			guNormalize(&g_Vars.currentplayer->laddernormal.x, &g_Vars.currentplayer->laddernormal.y, &g_Vars.currentplayer->laddernormal.z);

			sp74 = -(spcc.f[0] * g_Vars.currentplayer->laddernormal.f[0] + spcc.f[2] * g_Vars.currentplayer->laddernormal.f[2]);

#ifndef PLATFORM_N64
			// moving off a ladder faster than this lets go of it - which, for
			// one taken from the top (bwalkUpdateVertical()), is every player
			// who walked off the top rather than crept. It keeps its hold for
			// as long as they keep walking; once they have slowed the ladder
			// is any other, and pushing off it lets go
			if (-4.0f * g_Vars.lvupdate60freal < sp74) {
				g_GeLadderTopPlayer = NULL;
			}

			if (-4.0f * g_Vars.lvupdate60freal < sp74 || g_GeLadderTopPlayer == g_Vars.currentplayer) {
#else
			if (-4.0f * g_Vars.lvupdate60freal < sp74) {
#endif
				if (sp74 < 0.0f) {
					spcc.f[0] += sp74 * g_Vars.currentplayer->laddernormal.f[0];
					spcc.f[2] += sp74 * g_Vars.currentplayer->laddernormal.f[2];
					g_Vars.currentplayer->ladderupdown = sp74 * 0.3f;
				} else {
#ifndef PLATFORM_N64
					RoomNum ladderrooms[21];
#endif

					playerGetBbox(g_Vars.currentplayer->prop, &radius, &ymax, &ymin);

					if (!cd0002a13c(&g_Vars.currentplayer->prop->pos,
							radius * 1.1f, ymax - g_Vars.currentplayer->prop->pos.y,
							(g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->prop->pos.y) + 1.0f,
#ifndef PLATFORM_N64
							// asked of the rooms near as well (bwalkUpdateVertical())
							bwalkCdRooms(g_Vars.currentplayer->prop->rooms, &g_Vars.currentplayer->prop->pos,
								radius * 1.1f, (g_Vars.currentplayer->vv_manground - g_Vars.currentplayer->prop->pos.y) + 1.0f,
								ymax - g_Vars.currentplayer->prop->pos.y, ladderrooms, ARRAYCOUNT(ladderrooms) - 1),
#else
							g_Vars.currentplayer->prop->rooms,
#endif
							GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY)) {
						g_Vars.currentplayer->ladderupdown = 0.0f;
					} else {
						spcc.f[0] += sp74 * g_Vars.currentplayer->laddernormal.f[0];
						spcc.f[2] += sp74 * g_Vars.currentplayer->laddernormal.f[2];
						g_Vars.currentplayer->ladderupdown = sp74 * 0.3f;
					}
				}

				spcc.x *= 0.3f;
				spcc.z *= 0.3f;
			} else {
				g_Vars.currentplayer->ladderupdown = 0.0f;
			}
		}

		if (g_Vars.currentplayer->lift) {
			esc = (struct escalatorobj *) g_Vars.currentplayer->lift->obj;

			if (esc->base.type == OBJTYPE_ESCASTEP) {
				spcc.x += esc->base.prop->pos.x - esc->prevpos.x;
				spcc.z += esc->base.prop->pos.z - esc->prevpos.z;
			}
		}

		sp8c = g_Vars.currentplayer->prop->pos.x;
		sp88 = g_Vars.currentplayer->prop->pos.z;

		bwalk0f0c63bc(&spcc, g_Vars.currentplayer->swaytarget == 0.0f, CDTYPE_ALL);

		xdelta = g_Vars.currentplayer->prop->pos.x - g_Vars.currentplayer->bondprevpos.x;
		zdelta = g_Vars.currentplayer->prop->pos.z - g_Vars.currentplayer->bondprevpos.z;

		sp54 = -xdelta * g_Vars.currentplayer->bond2.unk00.f[2] + zdelta * g_Vars.currentplayer->bond2.unk00.f[0];
		sp50 = xdelta * g_Vars.currentplayer->bond2.unk00.f[0] + zdelta * g_Vars.currentplayer->bond2.unk00.f[2];

		sp4c = -spcc.f[0] * g_Vars.currentplayer->bond2.unk00.f[2] + spcc.f[2] * g_Vars.currentplayer->bond2.unk00.f[0];
		sp48 = spcc.f[0] * g_Vars.currentplayer->bond2.unk00.f[0] + spcc.f[2] * g_Vars.currentplayer->bond2.unk00.f[2];

		if (xdelta >= 0.0f) {
			if (g_Vars.currentplayer->bondshotspeed.f[0] > 0.0f) {
				if (spcc.f[0] >= 0.0f && xdelta < spcc.f[0]) {
					g_Vars.currentplayer->bondshotspeed.f[0] *= xdelta / spcc.f[0];
				}
			} else {
				if (spcc.f[0] < 0.0f) {
					g_Vars.currentplayer->bondshotspeed.f[0] = 0.0f;
				}
			}
		} else {
			if (g_Vars.currentplayer->bondshotspeed.f[0] < 0.0f) {
				if (spcc.f[0] <= 0.0f && spcc.f[0] < xdelta) {
					g_Vars.currentplayer->bondshotspeed.f[0] *= xdelta / spcc.f[0];
				}
			} else {
				if (spcc.f[0] > 0.0f) {
					g_Vars.currentplayer->bondshotspeed.f[0] = 0.0f;
				}
			}
		}

		if (zdelta >= 0.0f) {
			if (g_Vars.currentplayer->bondshotspeed.f[2] > 0.0f) {
				if (spcc.f[2] >= 0.0f && zdelta < spcc.f[2]) {
					g_Vars.currentplayer->bondshotspeed.f[2] *= zdelta / spcc.f[2];
				}
			} else {
				if (spcc.f[2] < 0.0f) {
					g_Vars.currentplayer->bondshotspeed.f[2] = 0.0f;
				}
			}
		} else {
			if (g_Vars.currentplayer->bondshotspeed.f[2] < 0.0f) {
				if (spcc.f[2] <= 0.0f && spcc.f[2] < zdelta) {
					g_Vars.currentplayer->bondshotspeed.f[2] *= zdelta / spcc.f[2];
				}
			} else {
				if (spcc.f[2] > 0.0f) {
					g_Vars.currentplayer->bondshotspeed.f[2] = 0.0f;
				}
			}
		}

		if (sp4c != 0.0f && g_Vars.currentplayer->speedstrafe * sp4c > 0.0f) {
			sp54 /= sp4c;

			if (sp54 <= 0.0f) {
				g_Vars.currentplayer->speedstrafe = 0.0f;
			} else if (sp54 < 1.0f) {
				g_Vars.currentplayer->speedstrafe *= sp54;
			}
		}

		if (sp48 != 0.0f) {
			if (g_Vars.currentplayer->speedgo * sp48 > 0.0f) {
				sp50 /= sp48;

				if (sp50 <= 0.0f) {
					g_Vars.currentplayer->speedgo = 0.0f;
				} else if (sp50 < 1.0f) {
					g_Vars.currentplayer->speedgo *= sp50;
				}
			}
		}

		xdiff = g_Vars.currentplayer->prop->pos.x - sp8c;
		zdiff = g_Vars.currentplayer->prop->pos.z - sp88;
		f0 = spcc.f[0] * spcc.f[0] + spcc.f[2] * spcc.f[2];

		if (f0 != 0.0f) {
			f0 = (xdiff * xdiff + zdiff * zdiff) / f0;
		}

		f0 = sqrtf(f0);
		g_Vars.currentplayer->swayoffset0 += f0 * spb4;
		g_Vars.currentplayer->swayoffset2 += f0 * spb0;
	}

	sp44 = g_Vars.currentplayer->speedtheta;
	sp40 = g_Vars.currentplayer->speedverta / 0.7f + g_Vars.currentplayer->crouchspeed / PALUPF(5.0f);
	sp3c = g_Vars.currentplayer->gunspeed;

	breathing = bheadGetBreathingValue();

	if (sp40 > 1.0f) {
		sp40 = 1.0f;
	} else if (sp40 < -1.0f) {
		sp40 = -1.0f;
	}

	if (g_Vars.currentplayer->headanim == HEADANIM_MOVING) {
		breathing *= 1.2f;
	}

	bgun0f09d8dc(breathing, sp3c, sp40, sp44, 0.0f);
	bgunSetAdjustPos(g_Vars.currentplayer->vv_verta360 * 0.017450513318181f);
}

void bwalkTick(void)
{
	bwalkUpdatePrevPos();
	bwalkUpdateTheta();
	bmoveUpdateVerta();
	bwalk0f0c69b8();
	bwalkUpdateVertical();

#ifndef PLATFORM_N64
	geTankTick();
#endif

#if VERSION >= VERSION_NTSC_1_0
	{
		s32 i;

#ifndef PLATFORM_N64
		// GoldenEye's rule on a level converted from it: the player is in the
		// room of the tile they stand on, whether or not the walk through the
		// portals ever put them there (geroom.h)
		if (geRoomActive()
				&& g_Vars.currentplayer->vv_ground > -30000
				&& g_Vars.currentplayer->floorroom > 0
				&& g_Vars.currentplayer->floorroom < g_Vars.roomcount
				&& g_Vars.currentplayer->prop->rooms[0] != g_Vars.currentplayer->floorroom) {
			propDeregisterRooms(g_Vars.currentplayer->prop);
			g_Vars.currentplayer->prop->rooms[0] = g_Vars.currentplayer->floorroom;
			g_Vars.currentplayer->prop->rooms[1] = -1;
		}
#endif

		for (i = 0; g_Vars.currentplayer->prop->rooms[i] != -1; i++) {
			if (g_Vars.currentplayer->floorroom == g_Vars.currentplayer->prop->rooms[i]) {
				propDeregisterRooms(g_Vars.currentplayer->prop);
				g_Vars.currentplayer->prop->rooms[0] = g_Vars.currentplayer->floorroom;
				g_Vars.currentplayer->prop->rooms[1] = -1;
				break;
			}
		}
	}
#endif

	bmoveUpdateRooms(g_Vars.currentplayer);
	objectiveCheckRoomEntered(g_Vars.currentplayer->prop->rooms[0]);

	if (g_Vars.currentplayer->walkinitmove) {
		struct coord coord;
		coord.x = (g_Vars.currentplayer->walkinitstart.x - g_Vars.currentplayer->walkinitpos.x)
			* (1.0f - g_Vars.currentplayer->walkinitt2) + g_Vars.currentplayer->prop->pos.x;

		coord.y = (g_Vars.currentplayer->walkinitstart.y - g_Vars.currentplayer->prop->pos.y)
			* (1.0f - g_Vars.currentplayer->walkinitt2) + g_Vars.currentplayer->prop->pos.y;

		coord.z = (g_Vars.currentplayer->walkinitstart.z - g_Vars.currentplayer->walkinitpos.z)
			* (1.0f - g_Vars.currentplayer->walkinitt2) + g_Vars.currentplayer->prop->pos.z;

		bmove0f0cc19c(&coord);
	} else {
		bmove0f0cc19c(&g_Vars.currentplayer->prop->pos);
	}

	playerUpdatePerimInfo();
	doorsCheckAutomatic();
}

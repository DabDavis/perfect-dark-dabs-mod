/**
 * GoldenEye's tank, on a converted GoldenEye mission.
 *
 * Perfect Dark counts the record at the load (setupCreateProps()) and builds
 * nothing for it: GoldenEye's tank is a thing the player drives, its code is
 * spread through the player's own (bondview2.c names it 460 times), and
 * Perfect Dark kept the player's fields - `tank` is GoldenEye's
 * g_WorldTankProp, `unk1af0` its g_PlayerTankProp, the bondtank* and
 * bondturret* floats its angles - and none of the code, which became the
 * hoverbike's.
 *
 * **It is Bond who moves, and the tank is carried under him.** That is
 * GoldenEye's design and it is kept: in the tank the player is still walking
 * (MOVEMODE_WALK), and this file changes what walking means -
 *
 *  - the input (geTankApplyMoveData()): forwards and backwards are the engine,
 *    the strafe axis turns the hull, and looking is the turret, which turns
 *    the view with it as the hull turns;
 *  - the step (geTankDrive()): the hull's heading times its speed, in place of
 *    the walk's, once the hull's own rectangle has been tried there
 *    (tankRectBlocked()) - turned, then stepped, then slid along what stopped
 *    it, as GoldenEye does it, and an object in its way driven over
 *    (tankDriveOver()) - and then through the walk's own collision, a circle
 *    of the hull's half width about Bond (geTankRadius());
 *  - the eye (geTankEyeHeight()): at the seat, which is all of GoldenEye's
 *    tank view (tankSeat()) - with the port's own third person switched on,
 *    its camera is pulled back far enough to see a tank (geTankCamera());
 *  - and after the walk (geTankTick()) the tank is put under him, turned to
 *    its heading and set on the ground under it, guards under it are run
 *    down, and the engine is heard.
 *
 * GoldenEye's numbers (bondview.c, bondview2.c; the US ROM's): 15 units a
 * frame flat out, the acceleration ((d/4)/15 + 0.5) * 10 / 60 a frame, the
 * hull's turn 0.3 through a 0.92 filter and times 3.5 - 63 degrees a second -
 * 45 frames to climb in on a cosine, a quarter of all damage while inside,
 * the view held over -20 degrees, the barrel between -5 and 25, a shell at
 * 66.67 a frame, and ninety ticks at half speed after driving over an object.
 *
 * The shell is Perfect Dark's rocket out of the muzzle, and the shells are
 * ammunition type 0x1d, which is GoldenEye's AMMO_TANK still in Perfect Dark's
 * list under no name - so a converted crate of tank shells is one already.
 *
 * Not GoldenEye's: Bond climbs in from beside the tank rather than from on top
 * of it (Perfect Dark's walk has no step up onto a prop), and gets out beside
 * it where GoldenEye leaves him standing on it, since here that is inside its
 * collision. The shells are a weapon as GoldenEye's are (WEAPON_GE_TANKSHELLS,
 * on the Data Uplink as the gadgets with nothing in the hand are): given and
 * held as he climbs in, taken as he climbs out, their trigger the cannon, and
 * anything else he switches to in there fires as it always does.
 */
#include <math.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/vars.h"
#include "lib/mtx.h"
#include "lib/model.h"
#include "lib/collision.h"
#include "lib/snd.h"
#include "game/bg.h"
#include "game/bondgun.h"
#include "game/bondmove.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/explosions.h"
#include "game/inv.h"
#include "game/lv.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/propsnd.h"
#include "gesfx.h"
#include "getank.h"
#include "gexplusveh.h"
#include "modloader.h"

#define TANK_MAX_SPEED      15.0f
#define TANK_TURN_FILTER    0.92f
#define TANK_TURN_INPUT     0.3f
#define TANK_TURN_SCALE     3.5f
#define TANK_ENTER_FRAMES   45.0f
#define TANK_PENALTY_TICKS  90
#define TANK_SHELL_SPEED    66.666664f
#define TANK_SHELL_GAP      TICKS(60)
#define TANK_AMMOTYPE       AMMOTYPE_1D

// a standing eye is 159 over the feet; half a crouch takes about a third off
// and GoldenEye another 37
#define TANK_EYE_OVER_SEAT 70.0f

// the hull's sides are tested this far over the ground: over a kerb, under a
// doorway's head
#define TANK_RECT_HEIGHT 60.0f

// what stops it: a wall, and an object's own collision, which is not flagged
// as a wall but as something that blocks sight and shots
#define TANK_RECT_GEOFLAGS (GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT | GEOFLAG_BLOCK_SHOOT)

#define TANK_PART_TURRET 1
#define TANK_PART_SEAT   2
#define TANK_PART_BARREL 3
#define TANK_PART_MUZZLE 4
#define TANK_PART_FLASH  7
#define TANK_PART_OFF    8

#define TANK_OUT      0
#define TANK_ENTERING 1
#define TANK_RUNNING  2

static struct {
	s32 state;
	f32 entert;
	struct coord enterpos;
	f32 turnsum;      // g_TankTurnSpeed, the filter's accumulator
	f32 speedtheta;   // what the hull turned by, for the engine's note
	f32 walk;         // the input, -1 to 1
	f32 turn;
	s32 penalty;
	s32 lastshot60;
	s32 weaponwas;
	s32 crushes;
	f32 entertheta;
	f32 enterverta;
} g_Tank[MAX_PLAYERS];

// A probe's hands on the sticks (gdb sets all three; build/gexrom/tankdrive.py):
// a headless run has no pad, and the input is read fresh every frame
s32 g_TankTestInput;
f32 g_TankTestWalk;
f32 g_TankTestTurn;

static f32 tankWrap(f32 angle)
{
	while (angle >= M_BADTAU) {
		angle -= M_BADTAU;
	}

	while (angle < 0.0f) {
		angle += M_BADTAU;
	}

	return angle;
}

static struct tankobj *tankDriven(void)
{
	struct prop *prop = g_Vars.currentplayer->unk1af0;

	return prop && prop->obj && prop->obj->type == OBJTYPE_TANK ? (struct tankobj *)prop->obj : NULL;
}

static f32 tankScale(struct tankobj *tank)
{
	return tank->base.model ? tank->base.model->scale : 1.0f;
}

/** The hull's half width and half length, and how high it stands. */
static void tankSize(struct tankobj *tank, f32 *halfwidth, f32 *halflength, f32 *height, f32 *bottom)
{
	struct modelrodata_bbox *bbox = objFindBboxRodata(&tank->base);
	const f32 scale = tankScale(tank);

	if (bbox) {
		*halfwidth = (bbox->xmax - bbox->xmin) * 0.5f * scale;
		*halflength = (bbox->zmax - bbox->zmin) * 0.5f * scale;
		*height = (bbox->ymax - bbox->ymin) * scale;
		*bottom = bbox->ymin * scale;
	} else {
		*halfwidth = 150;
		*halflength = 300;
		*height = 250;
		*bottom = 0;
	}
}

/** The view's heading as the tank's own angle: a prop heads (sin, cos), the player (-sin, cos). */
static f32 tankViewYaw(void)
{
	return tankWrap(M_BADTAU - g_Vars.currentplayer->vv_theta * M_BADTAU / 360.0f);
}

static void tankFace(struct tankobj *tank)
{
	f32 (*m)[3] = tank->base.realrot;
	const f32 scale = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
	Mtxf rot;

	// realrot carries the object's scale: read off it, as a vehicle's is
	mtx4LoadYRotation(tank->hullyaw, &rot);
	mtx4ToMtx3(&rot, tank->base.realrot);

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			m[i][j] *= scale;
		}
	}
}

void geTankCreate(struct defaultobj *obj)
{
	struct tankobj *tank = (struct tankobj *)obj;

	// prop.c's PROPDEF_TANK: both of the turret's angles nothing, the hull's
	// read off the matrix the pad gave it
	tank->hullyaw = tankWrap(atan2f(obj->realrot[2][0], obj->realrot[2][2]));
	tank->turretyaw = 0;
	tank->turretpitch = 0;
	tank->groundsum = 0;
	tank->shells = 0;
	tank->firing = 0;
	tank->speed = 0;
	tank->turnspeed = 0;
}

void geTankReset(void)
{
	memset(g_Tank, 0, sizeof(g_Tank));
}

s32 geTankIsDriving(void)
{
	return g_Tank[g_Vars.currentplayernum].state != TANK_OUT && tankDriven() != NULL;
}

s32 geTankPlayerDriving(struct player *player)
{
	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Vars.players[i] == player) {
			return g_Tank[i].state != TANK_OUT && player->unk1af0 != NULL;
		}
	}

	return 0;
}

s32 geTankHidesChr(struct chrdata *chr)
{
	// the driver is inside it: the body the third person camera builds for
	// him is not drawn, as GoldenEye draws none
	return chr && chr->prop && chr->prop->type == PROPTYPE_PLAYER
		&& geTankPlayerDriving(g_Vars.players[playermgrGetPlayerNumByProp(chr->prop)]);
}

void geTankCamera(f32 *dist, f32 *height, f32 *side, f32 *fwd)
{
	struct tankobj *tank = geTankIsDriving() ? tankDriven() : NULL;

	if (tank) {
		f32 halfwidth, halflength, height0, bottom;

		tankSize(tank, &halfwidth, &halflength, &height0, &bottom);

		// behind and over the whole of it, whatever the player's own camera
		// is set to for walking: twice its length back, and its height up
		*dist = halflength * 4.0f;
		*height = height0 * 0.75f;
		*side = 0;
		*fwd = 0;
	}
}

s32 geTankAnyoneDriving(void)
{
	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Tank[i].state != TANK_OUT && g_Vars.players[i] && g_Vars.players[i]->unk1af0) {
			return 1;
		}
	}

	return 0;
}

f32 geTankRadius(struct prop *playerprop, f32 radius)
{
	if (playerprop == g_Vars.currentplayer->prop && g_Tank[g_Vars.currentplayernum].state == TANK_RUNNING) {
		struct tankobj *tank = tankDriven();

		if (tank) {
			f32 halfwidth, halflength, height, bottom;

			tankSize(tank, &halfwidth, &halflength, &height, &bottom);

			return halfwidth > radius ? halfwidth : radius;
		}
	}

	return radius;
}

/**
 * Where Bond sits, from the middle of the tank, in the world: GoldenEye's
 * g_TankModelPositionOffset (bondviewTankModelRotationRelated()) - the turret's
 * pivot, part 1, and the seat, part 2, carried round it with the turret. In
 * GoldenEye's model that is the commander's hatch: 30 units left of the
 * turret's middle, 42 behind it and 75 over its roof, which is the whole of
 * GoldenEye's tank view - the player's own eye, up there, with the barrel
 * ahead and the hull below. There is no other camera in its code.
 */
static void tankSeat(struct tankobj *tank, struct coord *seat)
{
	struct modeldef *def = tank->base.model ? tank->base.model->definition : NULL;
	struct modelnode *pivot = def ? modelGetPart(def, TANK_PART_TURRET) : NULL;
	struct modelnode *node = def ? modelGetPart(def, TANK_PART_SEAT) : NULL;
	const f32 scale = tankScale(tank);
	struct coord local = {0, 0, 0};
	f32 s;
	f32 c;

	seat->x = seat->y = seat->z = 0;

	if (!pivot || (pivot->type & 0xff) != MODELNODETYPE_POSITION
			|| !node || (node->type & 0xff) != MODELNODETYPE_POSITION) {
		f32 halfwidth, halflength, height, bottom;

		tankSize(tank, &halfwidth, &halflength, &height, &bottom);
		seat->y = bottom + height + 40.0f;
		return;
	}

	// the seat about the pivot by the turret, then the pair by the hull
	s = sinf(tank->turretyaw);
	c = cosf(tank->turretyaw);

	local.x = pivot->rodata->position.pos.x + node->rodata->position.pos.x * c + node->rodata->position.pos.z * s;
	local.y = pivot->rodata->position.pos.y + node->rodata->position.pos.y;
	local.z = pivot->rodata->position.pos.z - node->rodata->position.pos.x * s + node->rodata->position.pos.z * c;

	s = sinf(tank->hullyaw);
	c = cosf(tank->hullyaw);

	seat->x = (local.x * c + local.z * s) * scale;
	seat->y = local.y * scale;
	seat->z = (-local.x * s + local.z * c) * scale;
}

f32 geTankEyeHeight(f32 eyeheight)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = g_Tank[p].state != TANK_OUT ? tankDriven() : NULL;
	struct coord seat;
	f32 target;
	f32 t;

	if (!tank) {
		return eyeheight;
	}

	// the seat is where he *stands*, half crouched (bondview2.c sets
	// CROUCH_HALF as he gets in, and takes 37 off the height besides): the
	// eye is a crouched man's over it. With the eye at the seat itself it is
	// level with the turret's roof and inside the turret, whose own faces
	// are then culled - no roof under him and no barrel until well past the
	// mantlet, which is what it looked like
	tankSeat(tank, &seat);
	target = tank->base.prop->pos.y + seat.y + TANK_EYE_OVER_SEAT - g_Vars.currentplayer->vv_manground;

	if (target < eyeheight) {
		target = eyeheight;
	}

	// GoldenEye's ease in: (cos(t * pi) + 1) / 2 is what remains
	t = g_Tank[p].state == TANK_RUNNING ? 1.0f : g_Tank[p].entert;

	return eyeheight + (target - eyeheight) * (1.0f - (cosf(t * M_BADTAU * 0.5f) + 1.0f) * 0.5f);
}

f32 geTankDamageScale(void)
{
	return geTankIsDriving() ? 0.25f : 1.0f;
}

/* ------------------------------------------------------------------------ */
/* In and out                                                                */
/* ------------------------------------------------------------------------ */

static struct prop *tankNear(void)
{
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct prop *best = NULL;
	f32 bestdist = 0;

	for (s32 i = 0; i < g_Vars.maxprops; i++) {
		struct prop *prop = &g_Vars.props[i];
		f32 halfwidth, halflength, height, bottom;
		f32 reach;
		f32 dx;
		f32 dz;
		f32 dist;

		if (prop->type != PROPTYPE_OBJ || !prop->obj || prop->obj->type != OBJTYPE_TANK
				|| !prop->obj->model || !objIsHealthy(prop->obj)) {
			continue;
		}

		tankSize((struct tankobj *)prop->obj, &halfwidth, &halflength, &height, &bottom);

		dx = prop->pos.x - playerprop->pos.x;
		dz = prop->pos.z - playerprop->pos.z;
		dist = sqrtf(dx * dx + dz * dz);
		reach = sqrtf(halfwidth * halfwidth + halflength * halflength) + 100.0f;

		if (dist < reach && playerprop->pos.y - prop->pos.y < height + 250.0f
				&& playerprop->pos.y - prop->pos.y > -100.0f
				&& (!best || dist < bestdist)) {
			best = prop;
			bestdist = dist;
		}
	}

	return best;
}

static void tankEnter(struct prop *prop)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = (struct tankobj *)prop->obj;

	memset(&g_Tank[p], 0, sizeof(g_Tank[p]));

	g_Vars.currentplayer->unk1af0 = prop;
	g_Tank[p].state = TANK_ENTERING;
	g_Tank[p].enterpos = g_Vars.currentplayer->prop->pos;
	g_Tank[p].lastshot60 = g_Vars.lvframe60 - TANK_SHELL_GAP;
	g_Tank[p].entertheta = g_Vars.currentplayer->vv_theta;
	g_Tank[p].enterverta = g_Vars.currentplayer->vv_verta;

	// what was left in it (bondview2.c: add_ammo_to_weapon(ITEM_TANKSHELLS, unkD8))
	bgunSetAmmoQuantity(TANK_AMMOTYPE, bgunGetReservedAmmoCount(TANK_AMMOTYPE) + tank->shells);
	tank->shells = 0;
	tank->speed = 0;
	tank->turnspeed = 0;

	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;

	// bondview2.c: he is handed the tank's shells as he gets in, and holds
	// them - GoldenEye's ITEM_TANKSHELLS, nothing in the hand
	g_Tank[p].weaponwas = bgunGetWeaponNum(HAND_RIGHT);
	invGiveSingleWeapon(WEAPON_GE_TANKSHELLS);
	bgunEquipWeapon2(HAND_RIGHT, WEAPON_GE_TANKSHELLS);
	bgunEquipWeapon2(HAND_LEFT, WEAPON_NONE);

	// the player is inside it now, and walks its collision about himself
	tank->base.hidden |= OBJHFLAG_MOUNTED;
	propSetPerimEnabled(prop, false);
}

/** A place to stand beside the tank, as a bike's rider is put down (bbikeTryDismountAngle()). */
static s32 tankTryExit(struct tankobj *tank, f32 angle, f32 distance, struct coord *out, RoomNum *outrooms)
{
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct coord pos;
	RoomNum rooms[8];
	f32 radius;
	f32 ymax;
	f32 ymin;
	s32 result;

	radius = g_Vars.currentplayer->bond2.radius;
	ymin = g_Vars.currentplayer->vv_manground + 30;
	ymax = g_Vars.currentplayer->vv_manground + g_Vars.currentplayer->vv_headheight;

	angle = tankWrap(tank->hullyaw + angle);
	distance += radius + 20.0f;

	pos.x = tank->base.prop->pos.x + sinf(angle) * distance;
	pos.y = playerprop->pos.y;
	pos.z = tank->base.prop->pos.z + cosf(angle) * distance;

	func0f065e74(&playerprop->pos, playerprop->rooms, &pos, rooms);
	bmoveFindEnteredRoomsByPos(g_Vars.currentplayer, &pos, rooms);

	result = cdTestCylMove02(&playerprop->pos, playerprop->rooms, &pos, rooms, CDTYPE_BG, true,
			ymax - playerprop->pos.y, ymin - playerprop->pos.y);

	if (result == CDRESULT_NOCOLLISION) {
		result = cdTestVolume(&pos, radius, rooms, CDTYPE_ALL, CHECKVERTICAL_YES,
				ymax - playerprop->pos.y, ymin - playerprop->pos.y);
	}

	if (result != CDRESULT_NOCOLLISION) {
		return 0;
	}

	*out = pos;
	roomsCopy(rooms, outrooms);

	return 1;
}

static void tankStopSounds(struct prop *prop)
{
	psStopSound(prop, PSTYPE_CHOPPERHUM1, 0xffff);
	psStopSound(prop, PSTYPE_CHOPPERHUM2, 0xffff);
}

static s32 tankExit(s32 force)
{
	static const f32 angles[8] = { 1.5707963f, 4.7123890f, 0.7853982f, 5.4977871f, 2.3561945f, 3.9269908f, 0, 3.1415927f };
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = tankDriven();
	struct prop *playerprop = g_Vars.currentplayer->prop;
	f32 halfwidth, halflength, height, bottom;
	struct coord pos;
	RoomNum rooms[8];
	s32 found = 0;

	if (!tank) {
		g_Tank[p].state = TANK_OUT;
		g_Vars.currentplayer->unk1af0 = NULL;
		return 1;
	}

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	// the tank is solid again for the test, and the player is not in it
	propSetPerimEnabled(tank->base.prop, true);
	propSetPerimEnabled(playerprop, false);

	for (s32 i = 0; i < 8 && !found; i++) {
		const f32 side = fabsf(sinf(angles[i]));
		const f32 distance = i < 2 ? halfwidth : (i < 6 ? sqrtf(halfwidth * halfwidth + halflength * halflength) : halflength);

		(void)side;
		found = tankTryExit(tank, angles[i], distance, &pos, rooms);
	}

	propSetPerimEnabled(playerprop, true);

	if (!found && !force) {
		// nowhere to stand: stay in it
		propSetPerimEnabled(tank->base.prop, false);
		return 0;
	}

	if (found) {
		playerprop->pos.x = pos.x;
		playerprop->pos.z = pos.z;
		propDeregisterRooms(playerprop);
		roomsCopy(rooms, playerprop->rooms);
		propRegisterRooms(playerprop);
	}

	// bondview2.c: what he had goes back into the tank
	tank->shells = bgunGetReservedAmmoCount(TANK_AMMOTYPE);
	bgunSetAmmoQuantity(TANK_AMMOTYPE, 0);

	// and the shells are taken off him: bondinvRemoveItemByID(), then
	// whatever he held before them
	if (bgunGetWeaponNum(HAND_RIGHT) == WEAPON_GE_TANKSHELLS) {
		bgunEquipWeapon2(HAND_RIGHT, g_Tank[p].weaponwas > 0 ? g_Tank[p].weaponwas : WEAPON_UNARMED);
	}

	invRemoveItemByNum(WEAPON_GE_TANKSHELLS);
	tank->firing = 0;
	tank->speed = 0;
	tank->turnspeed = 0;
	tank->base.hidden &= ~OBJHFLAG_MOUNTED;

	tankStopSounds(tank->base.prop);

	g_Tank[p].state = TANK_OUT;
	g_Vars.currentplayer->unk1af0 = NULL;
	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;

	return 1;
}

s32 geTankActivate(void)
{
	const s32 p = g_Vars.currentplayernum;

	if (!modloaderStageIsMission(g_Vars.stagenum)
			|| !(g_Vars.currentplayer->bondactivateorreload & JO_ACTION_ACTIVATE)
			|| g_Vars.currentplayer->isdead) {
		return 0;
	}

	if (g_Tank[p].state == TANK_RUNNING) {
		tankExit(0);
		g_Vars.currentplayer->bondactivateorreload = 0;
		return 1;
	}

	if (g_Tank[p].state == TANK_OUT) {
		struct prop *prop = tankNear();

		if (prop) {
			tankEnter(prop);
			g_Vars.currentplayer->bondactivateorreload = 0;
			return 1;
		}
	}

	return g_Tank[p].state != TANK_OUT;
}

/* ------------------------------------------------------------------------ */
/* Driving                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * Whether the hull's rectangle, moved by `step` and turned to `yaw`, is in
 * anything: GoldenEye's bondviewTankCollisionStatus(), which draws the four
 * sides of the hull and a line out to each corner through its collision and
 * asks whether any of them is cut - by the level, an object, a door or
 * something blocking a path, and never by a guard, who is run down instead.
 * The side that was cut is handed back for the slide, and the object that did
 * it is remembered for tankDriveOver().
 */
static struct prop *g_TankObstacle;

static s32 tankRectBlocked(struct tankobj *tank, struct coord *step, f32 yaw, struct coord *edgea, struct coord *edgeb)
{
	const s32 types = CDTYPE_BG | CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_OBJSIMMUNETOEXPLOSIONS;
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct coord seat;
	struct coord centre;
	struct coord corners[4];
	RoomNum centrerooms[8];
	RoomNum cornerrooms[4][8];
	f32 halfwidth, halflength, height, bottom;
	const f32 s = sinf(yaw);
	const f32 c = cosf(yaw);
	const f32 savedyaw = tank->hullyaw;

	s32 result = 0;

	g_TankObstacle = NULL;

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	// not by itself, whatever state its own collision was left in
	propSetPerimEnabled(tank->base.prop, false);

	// where the tank's middle would be: Bond, less where he sits in it at
	// that heading
	tank->hullyaw = yaw;
	tankSeat(tank, &seat);
	tank->hullyaw = savedyaw;

	centre.x = playerprop->pos.x - seat.x + (step ? step->x : 0.0f);
	centre.y = g_Vars.currentplayer->vv_manground + TANK_RECT_HEIGHT;
	centre.z = playerprop->pos.z - seat.z + (step ? step->z : 0.0f);

	func0f065e74(&playerprop->pos, playerprop->rooms, &centre, centrerooms);

	for (s32 i = 0; i < 4; i++) {
		const f32 across = (i == 0 || i == 3) ? -halfwidth : halfwidth;
		const f32 along = i < 2 ? halflength : -halflength;

		corners[i].x = centre.x + across * c + along * s;
		corners[i].y = centre.y;
		corners[i].z = centre.z - across * s + along * c;

		// out to the corner, which is also how its rooms are found
		if (cdExamLos08(&centre, centrerooms, &corners[i], types, TANK_RECT_GEOFLAGS) == CDRESULT_COLLISION) {
			goto blocked;
		}

		func0f065e74(&centre, centrerooms, &corners[i], cornerrooms[i]);
	}

	for (s32 i = 0; i < 4; i++) {
		if (cdExamLos08(&corners[i], cornerrooms[i], &corners[(i + 1) % 4], types, TANK_RECT_GEOFLAGS) == CDRESULT_COLLISION) {
			goto blocked;
		}
	}

	goto done;

blocked:
	result = 1;
	g_TankObstacle = cdGetObstacleProp();

	if (edgea && edgeb) {
		cdGetEdge(edgea, edgeb, 0, "getank.c");
	}

done:
	return result;
}

/**
 * What the hull has just met, if it is an object: GoldenEye's tank destroys
 * whatever of the level's furniture it touches (maybe_detonate_object_and_
 * its_children() at 10000), and is held to half its speed for ninety ticks
 * for it - the only harm a tank ever comes to. What cannot be destroyed is
 * a wall to it, and a door is left to be a door.
 */
static void tankDriveOver(struct tankobj *tank)
{
	const s32 p = g_Vars.currentplayernum;
	struct prop *prop = g_TankObstacle;

	g_TankObstacle = NULL;

	if (!prop || prop->type != PROPTYPE_OBJ || !prop->obj || prop == tank->base.prop
			|| prop->obj->type == OBJTYPE_TANK
			|| (prop->obj->flags & OBJFLAG_INVINCIBLE)
			|| !objIsHealthy(prop->obj)) {
		return;
	}

	objDamage(prop->obj, 10000.0f, &prop->pos, WEAPON_NONE, g_Vars.currentplayernum);
	g_Tank[p].penalty = TANK_PENALTY_TICKS;
}

/** Whether two rectangles in the plan overlap: the four sides of each as separating axes. */
static s32 tankRectsOverlap(const f32 a[4][2], const f32 b[4][2])
{
	for (s32 pass = 0; pass < 2; pass++) {
		const f32 (*r)[2] = pass == 0 ? a : b;

		for (s32 i = 0; i < 4; i++) {
			const f32 nx = r[(i + 1) % 4][1] - r[i][1];
			const f32 nz = -(r[(i + 1) % 4][0] - r[i][0]);
			f32 amin = 0, amax = 0, bmin = 0, bmax = 0;

			for (s32 k = 0; k < 4; k++) {
				const f32 pa = a[k][0] * nx + a[k][1] * nz;
				const f32 pb = b[k][0] * nx + b[k][1] * nz;

				if (k == 0 || pa < amin) amin = pa;
				if (k == 0 || pa > amax) amax = pa;
				if (k == 0 || pb < bmin) bmin = pb;
				if (k == 0 || pb > bmax) bmax = pb;
			}

			if (amax < bmin || bmax < amin) {
				return 0;
			}
		}
	}

	return 1;
}

/**
 * Everything of the level's furniture the hull is over, driven over:
 * bondview2.c's walk through the props of the tank's rooms, each one's bounds
 * in the plan (chraiGetCollisionBoundsWithoutY()) against the hull's rectangle
 * (chrobjTestPolygonsTouchingOrOverlap2D()). It is not the collision that
 * finds them - a parked truck's is not cut by the lines tankRectBlocked()
 * draws - and what is destroyed does not stop the tank; what cannot be is
 * left for the collision to be a wall.
 */
static void tankDriveOverProps(struct tankobj *tank)
{
	const s32 p = g_Vars.currentplayernum;
	struct prop *tankprop = tank->base.prop;
	f32 halfwidth, halflength, height, bottom;
	f32 hull[4][2];
	const f32 s = sinf(tank->hullyaw);
	const f32 c = cosf(tank->hullyaw);

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	for (s32 i = 0; i < 4; i++) {
		const f32 across = (i == 0 || i == 3) ? -halfwidth : halfwidth;
		const f32 along = i < 2 ? halflength : -halflength;

		hull[i][0] = tankprop->pos.x + across * c + along * s;
		hull[i][1] = tankprop->pos.z - across * s + along * c;
	}

	for (s32 n = 0; n < g_Vars.maxprops; n++) {
		struct prop *prop = &g_Vars.props[n];
		struct defaultobj *obj;
		struct modelrodata_bbox *bbox;
		f32 box[4][2];
		f32 reach;
		f32 dx;
		f32 dz;

		if (prop == tankprop || prop->type != PROPTYPE_OBJ || !prop->obj) {
			continue;
		}

		obj = prop->obj;

		if (!obj->model || obj->type == OBJTYPE_TANK || (obj->flags & OBJFLAG_INVINCIBLE)
				|| (obj->hidden & (OBJHFLAG_PROJECTILE | OBJHFLAG_MOUNTED | OBJHFLAG_GRABBED))
				|| prop->parent || !objIsHealthy(obj)
				|| fabsf(prop->pos.y - tankprop->pos.y) > height + 100.0f) {
			continue;
		}

		dx = prop->pos.x - tankprop->pos.x;
		dz = prop->pos.z - tankprop->pos.z;
		reach = halflength + halfwidth + 1500.0f;

		if (dx * dx + dz * dz > reach * reach || !(bbox = objFindBboxRodata(obj))) {
			continue;
		}

		// its box in the plan, by its own matrix, which carries its scale
		for (s32 i = 0; i < 4; i++) {
			const f32 x = (i == 0 || i == 3) ? bbox->xmin : bbox->xmax;
			const f32 z = i < 2 ? bbox->zmax : bbox->zmin;

			box[i][0] = prop->pos.x + x * obj->realrot[0][0] + z * obj->realrot[2][0];
			box[i][1] = prop->pos.z + x * obj->realrot[0][2] + z * obj->realrot[2][2];
		}

		if (tankRectsOverlap(hull, box)) {
			objDamage(obj, 10000.0f, &prop->pos, WEAPON_NONE, g_Vars.currentplayernum);
			g_Tank[p].penalty = TANK_PENALTY_TICKS;
		}
	}
}

s32 geTankApplyMoveData(struct movedata *data)
{
	const s32 p = g_Vars.currentplayernum;

	if (!geTankIsDriving()) {
		return 0;
	}

	g_Tank[p].walk = 0;
	g_Tank[p].turn = 0;

	if (g_Tank[p].state == TANK_RUNNING) {
		if (data->digitalstepforward) {
			g_Tank[p].walk = 1;
		} else if (data->digitalstepback) {
			g_Tank[p].walk = -1;
		} else if (data->canlookahead) {
			g_Tank[p].walk = data->analogwalk / 70.0f;
		}

		if (data->digitalstepleft) {
			g_Tank[p].turn = -1;
		} else if (data->digitalstepright) {
			g_Tank[p].turn = 1;
		} else if (data->unk14) {
			g_Tank[p].turn = data->analogstrafe / 70.0f;
		}

		if (g_TankTestInput) {
			g_Tank[p].walk = g_TankTestWalk;
			g_Tank[p].turn = g_TankTestTurn;
		}

		if (g_Tank[p].walk > 1) g_Tank[p].walk = 1;
		if (g_Tank[p].walk < -1) g_Tank[p].walk = -1;
		if (g_Tank[p].turn > 1) g_Tank[p].turn = 1;
		if (g_Tank[p].turn < -1) g_Tank[p].turn = -1;
	}

	// no stride and no bob: the walk is given nothing to walk with
	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;
	g_Vars.currentplayer->speedmaxtime60 = 0;

	return 1;
}

void geTankDrive(struct coord *delta)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = geTankIsDriving() ? tankDriven() : NULL;
	const f32 frames = g_Vars.lvupdate60freal;

	if (!tank) {
		return;
	}

	if (g_Tank[p].state == TANK_ENTERING) {
		// across to the seat, on GoldenEye's cosine
		const f32 remain = (cosf(g_Tank[p].entert * M_BADTAU * 0.5f) + 1.0f) * 0.5f;
		struct coord seat;
		f32 x;
		f32 z;

		tankSeat(tank, &seat);

		x = tank->base.prop->pos.x + seat.x + (g_Tank[p].enterpos.x - tank->base.prop->pos.x - seat.x) * remain;
		z = tank->base.prop->pos.z + seat.z + (g_Tank[p].enterpos.z - tank->base.prop->pos.z - seat.z) * remain;

		delta->x = x - g_Vars.currentplayer->prop->pos.x;
		delta->y = 0;
		delta->z = z - g_Vars.currentplayer->prop->pos.z;

		// and the view comes round to where the turret points, the short
		// way, and up to GoldenEye's -20 if it was lower: it is the turret
		// that Bond gets into, and it does not swing to meet him
		{
			const f32 target = 360.0f - tankWrap(tank->hullyaw + tank->turretyaw) * 360.0f / M_BADTAU;
			f32 diff = target - g_Tank[p].entertheta;

			while (diff > 180.0f) diff -= 360.0f;
			while (diff < -180.0f) diff += 360.0f;

			g_Vars.currentplayer->vv_theta = g_Tank[p].entertheta + diff * (1.0f - remain);

			while (g_Vars.currentplayer->vv_theta < 0.0f) g_Vars.currentplayer->vv_theta += 360.0f;
			while (g_Vars.currentplayer->vv_theta >= 360.0f) g_Vars.currentplayer->vv_theta -= 360.0f;

			if (g_Tank[p].enterverta < -20.0f) {
				g_Vars.currentplayer->vv_verta = g_Tank[p].enterverta + (-20.0f - g_Tank[p].enterverta) * (1.0f - remain);
			}
		}

		return;
	}

	// the engine: GoldenEye's ramp towards what the stick asks for, at half
	// of it and four times as hard for a while after driving over something
	{
		f32 target = g_Tank[p].walk * TANK_MAX_SPEED;
		f32 hard = 1.0f;
		f32 diff;
		f32 step;

		if (g_Tank[p].penalty > 0) {
			target *= 0.5f;
			hard = 4.0f;
			g_Tank[p].penalty -= g_Vars.lvupdate60;
		}

		diff = target - tank->speed;
		step = ((fabsf(diff) / 4.0f) / TANK_MAX_SPEED + 0.5f) * hard * 10.0f / 60.0f * frames;

		if (fabsf(diff) <= step) {
			tank->speed = target;
		} else {
			tank->speed += diff > 0.0f ? step : -step;
		}
	}

	// the hull: the input at 0.3 through a one pole filter, whose steady
	// state is the input over (1 - 0.92), so times that to come back to it
	{
		const f32 oldyaw = tank->hullyaw;
		const s32 wasblocked = tankRectBlocked(tank, NULL, oldyaw, NULL, NULL);
		struct coord step;
		struct coord edgea;
		struct coord edgeb;
		f32 turnedby;

		for (s32 i = 0; i < g_Vars.lvupdate60; i++) {
			g_Tank[p].turnsum = g_Tank[p].turnsum * TANK_TURN_FILTER + g_Tank[p].turn * TANK_TURN_INPUT;
		}

		g_Tank[p].speedtheta = g_Tank[p].turnsum * (1.0f - TANK_TURN_FILTER);
		turnedby = g_Tank[p].speedtheta * frames * TANK_TURN_SCALE * M_BADTAU / 360.0f;

		// bondview2.c: the turn is tried where it stands, and is not taken
		// if the hull's corners would swing into something. One that is
		// already in something - parked against a fence, or let in beside a
		// wall - is let out of it rather than held
		if (turnedby != 0.0f && !wasblocked
				&& tankRectBlocked(tank, NULL, tankWrap(oldyaw + turnedby), NULL, NULL)) {
			tankDriveOver(tank);
			turnedby = 0.0f;
			g_Tank[p].turnsum = 0.0f;
			g_Tank[p].speedtheta = 0.0f;
		}

		tank->hullyaw = tankWrap(oldyaw + turnedby);
		tank->turnspeed = turnedby;

		// and the turret is carried round with it, so the view is
		g_Vars.currentplayer->vv_theta -= turnedby * 360.0f / M_BADTAU;

		while (g_Vars.currentplayer->vv_theta < 0.0f) {
			g_Vars.currentplayer->vv_theta += 360.0f;
		}

		while (g_Vars.currentplayer->vv_theta >= 360.0f) {
			g_Vars.currentplayer->vv_theta -= 360.0f;
		}

		// then the step, with the hull's whole length: the walk's own
		// collision after this is a circle of its half width about Bond,
		// which is inside the rectangle and only ever stops it at a guard
		step.x = sinf(tank->hullyaw) * tank->speed * frames;
		step.y = 0;
		step.z = cosf(tank->hullyaw) * tank->speed * frames;

		if (!wasblocked && (step.x != 0.0f || step.z != 0.0f)
				&& tankRectBlocked(tank, &step, tank->hullyaw, &edgea, &edgeb)) {
			struct coord slide = {0, 0, 0};
			f32 ex = edgeb.x - edgea.x;
			f32 ez = edgeb.z - edgea.z;
			const f32 len = sqrtf(ex * ex + ez * ez);

			tankDriveOver(tank);

			// along what stopped it, as GoldenEye slides the hull and as a
			// bike is slid here (bbike0f0d3840())
			if (len > 0.0f) {
				const f32 along = (step.x * ex + step.z * ez) / (len * len);

				slide.x = ex * along;
				slide.z = ez * along;
			}

			if ((slide.x != 0.0f || slide.z != 0.0f) && !tankRectBlocked(tank, &slide, tank->hullyaw, NULL, NULL)) {
				step = slide;
			} else {
				step.x = 0;
				step.z = 0;
				tank->speed = 0;
			}
		}

		*delta = step;
	}
}

/* ------------------------------------------------------------------------ */
/* The cannon                                                                */
/* ------------------------------------------------------------------------ */

static void tankFire(struct tankobj *tank)
{
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct weaponobj *shell;
	struct modelnode *node;
	struct coord pos;
	struct coord dir;
	struct coord speed;
	Mtxf rot;
	Mtxf identity;
	f32 yaw = tankWrap(tank->hullyaw + tank->turretyaw);
	f32 halfwidth, halflength, height, bottom;

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	dir.x = sinf(yaw) * cosf(tank->turretpitch);
	dir.y = sinf(tank->turretpitch);
	dir.z = cosf(yaw) * cosf(tank->turretpitch);

	// out of the muzzle: the barrel's length in front of the turret, at the
	// barrel's height
	node = tank->base.model ? modelGetPart(tank->base.model->definition, TANK_PART_BARREL) : NULL;

	pos.x = tank->base.prop->pos.x + dir.x * (halflength + 60.0f);
	pos.y = tank->base.prop->pos.y + bottom + height * 0.8f + dir.y * (halflength + 60.0f);
	pos.z = tank->base.prop->pos.z + dir.z * (halflength + 60.0f);

	if (node && (node->type & 0xff) == MODELNODETYPE_POSITION) {
		pos.y = tank->base.prop->pos.y + node->rodata->position.pos.y * tankScale(tank) + dir.y * (halflength + 60.0f);
	}

	shell = weaponCreateProjectileFromWeaponNum(MODEL_CHRDYROCKETMIS, WEAPON_ROCKET, playerprop->chr);

	if (!shell) {
		return;
	}

	mtx4LoadIdentity(&identity);
	mtx4LoadXRotation(-tank->turretpitch, &rot);
	mtx4LoadYRotation(yaw, &identity);
	mtx4MultMtx4InPlace(&identity, &rot);
	mtx4LoadIdentity(&identity);

	speed.x = dir.x * TANK_SHELL_SPEED;
	speed.y = dir.y * TANK_SHELL_SPEED;
	speed.z = dir.z * TANK_SHELL_SPEED;

	bgun0f09ebcc(&shell->base, &pos, playerprop->rooms, &rot, &speed, &identity, playerprop, &pos);

	if (shell->base.hidden & OBJHFLAG_PROJECTILE) {
		shell->timer240 = -1;
		shell->base.projectile->flags |= PROJECTILEFLAG_POWERED;
		shell->base.projectile->unk010 = dir.x * 0.27777776f;
		shell->base.projectile->unk014 = dir.y * 0.27777776f;
		shell->base.projectile->unk018 = dir.z * 0.27777776f;
	}

	// GoldenEye's GUN_TANK2BIGBIG_1, the grenade launcher's thump, is the
	// tank shells' Sound in its weapon table
	if (geSfxStage()) {
		geSfxPlay(12, GESFX_VOLUME);
	}

	tank->firing = TICKS(8);
}

void geTankFireCannon(void)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = geTankIsDriving() ? tankDriven() : NULL;

	// a shell a second, which is as fast as GoldenEye's reloads
	if (tank && g_Tank[p].state == TANK_RUNNING
			&& g_Vars.lvframe60 - g_Tank[p].lastshot60 >= TANK_SHELL_GAP
			&& bgunGetReservedAmmoCount(TANK_AMMOTYPE) > 0
			&& !lvIsPaused()) {
		bgunSetAmmoQuantity(TANK_AMMOTYPE, bgunGetReservedAmmoCount(TANK_AMMOTYPE) - 1);
		g_Tank[p].lastshot60 = g_Vars.lvframe60;
		tankFire(tank);
	}
}

/* ------------------------------------------------------------------------ */
/* After the walk                                                            */
/* ------------------------------------------------------------------------ */

static void tankCrush(struct tankobj *tank, f32 halfwidth, f32 halflength)
{
	const s32 p = g_Vars.currentplayernum;
	struct prop *tankprop = tank->base.prop;
	const f32 s = sinf(tank->hullyaw);
	const f32 c = cosf(tank->hullyaw);
	s32 numchrs = chrsGetNumSlots();

	for (s32 i = 0; i < numchrs; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop;
		f32 dx;
		f32 dz;
		f32 along;
		f32 across;

		if (!chr->model || !chr->prop || chr->prop->type != PROPTYPE_CHR) {
			continue;
		}

		prop = chr->prop;
		dx = prop->pos.x - tankprop->pos.x;
		dz = prop->pos.z - tankprop->pos.z;
		along = dx * s + dz * c;
		across = dx * c - dz * s;

		// the hull's rectangle, as GoldenEye tests a guard against its own
		if (fabsf(along) > halflength + 20.0f || fabsf(across) > halfwidth + 20.0f
				|| fabsf(prop->pos.y - tankprop->pos.y) > 300.0f) {
			continue;
		}

		if (chrIsDead(chr)) {
			continue;
		}

		{
			struct coord dir = { dx, 0, dz };
			const s32 which = g_Tank[p].crushes++ % 3;

			guNormalize(&dir.x, &dir.y, &dir.z);
			chrDamageByMisc(chr, 1000.0f, &dir, NULL, g_Vars.currentplayer->prop);

			// bondview2.c: the yell for two in three, the crunch for two in three
			if (geSfxStage()) {
				if (which < 2) {
					geSfxPlayAt(183, prop, NULL, NULL, PSTYPE_NONE, PSFLAG_0400);
				}

				if (which > 0) {
					geSfxPlayAt(213, prop, NULL, NULL, PSTYPE_NONE, PSFLAG_0400);
				}
			}
		}
	}
}

/** The engine under load: TRUCK_RUN always, the TANK tread loop while it moves or turns. */
static void tankSounds(struct tankobj *tank)
{
	const s32 p = g_Vars.currentplayernum;
	struct prop *prop = tank->base.prop;
	f32 util = fabsf(tank->speed) / TANK_MAX_SPEED;
	const f32 turnutil = fabsf(g_Tank[p].speedtheta) / TANK_TURN_INPUT;

	if (turnutil > util) {
		util = turnutil;
	}

	if (!geSfxStage() || lvIsPaused() || g_Vars.in_cutscene) {
		return;
	}

	if (geSfxNum(65)) {
		psCreateIfNotDupe(prop, geSfxNum(65), PSTYPE_CHOPPERHUM1);
	}

	if (util > 0.02f) {
		if (geSfxNum(62)) {
			psCreateIfNotDupe(prop, geSfxNum(62), PSTYPE_CHOPPERHUM2);
		}
	} else {
		psStopSound(prop, PSTYPE_CHOPPERHUM2, 0xffff);
	}
}

void geTankTick(void)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank;
	struct prop *prop;
	struct prop *playerprop = g_Vars.currentplayer->prop;
	f32 halfwidth, halflength, height, bottom;

	if (g_Tank[p].state == TANK_OUT) {
		return;
	}

	tank = tankDriven();

	if (!tank || !objIsHealthy(&tank->base)) {
		g_Tank[p].state = TANK_OUT;
		g_Vars.currentplayer->unk1af0 = NULL;
		return;
	}

	prop = tank->base.prop;

	if (g_Vars.currentplayer->isdead) {
		// bondview2.c: a tank explodes with Bond in it
		explosionCreateSimple(prop, &prop->pos, prop->rooms, EXPLOSIONTYPE_ROCKET, g_Vars.currentplayernum);
		tankExit(1);
		return;
	}

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	if (g_Tank[p].state == TANK_ENTERING) {
		if (g_Tank[p].entert == 0.0f && geSfxStage()) {
			// TRUCK_START as the engine catches
			geSfxPlayAt(66, prop, NULL, NULL, PSTYPE_NONE, PSFLAG_0400);
		}

		g_Tank[p].entert += g_Vars.lvupdate60freal / TANK_ENTER_FRAMES;

		if (g_Tank[p].entert >= 1.0f) {
			g_Tank[p].entert = 1.0f;
			g_Tank[p].state = TANK_RUNNING;
		}

		return;
	}

	// the view no lower than GoldenEye lets it go
	if (g_Vars.currentplayer->vv_verta < -20.0f) {
		g_Vars.currentplayer->vv_verta = -20.0f;
	}

	// the turret is where he looks
	tank->turretyaw = tankWrap(tankViewYaw() - tank->hullyaw);
	tank->turretpitch = g_Vars.currentplayer->vv_verta * M_BADTAU / 360.0f;

	if (tank->turretpitch < -0.087266468f) {
		tank->turretpitch = -0.087266468f;
	} else if (tank->turretpitch > 25.0f * M_BADTAU / 360.0f) {
		tank->turretpitch = 25.0f * M_BADTAU / 360.0f;
	}

	if (tank->firing > 0) {
		tank->firing -= g_Vars.lvupdate60;
	}

	// and the tank is put under him, on the ground that is under it
	{
		struct coord next;
		RoomNum inrooms[21];
		RoomNum aboverooms[21];
		RoomNum bestroom;

		struct coord seat;

		// bondview2.c: the tank is where Bond is, less where he sits in it
		tankSeat(tank, &seat);

		next.x = playerprop->pos.x - seat.x;
		next.y = prop->pos.y;
		next.z = playerprop->pos.z - seat.z;

		bgFindRoomsByPos(&next, inrooms, aboverooms, 20, &bestroom);

		if (inrooms[0] != -1) {
			const f32 ground = g_Vars.currentplayer->vv_manground;

			if (ground > -30000.0f) {
				next.y = ground - bottom + 4.0f;
			}

			prop->pos = next;
			propDeregisterRooms(prop);
			inrooms[7] = -1;
			roomsCopy(inrooms, prop->rooms);
			propRegisterRooms(prop);
		}

		tankFace(tank);

		// and its rooms are every room its box is in, as a bike's are found
		// after it moves: with the middle's alone, the hull ahead of the
		// turret stood in the next room and was cut off at that room's edge
		// of the screen
		func0f069c70(&tank->base, true, true);

		// which builds its collision again and switches it back on: off, or
		// the hull's own sides are the first thing the hull runs into
		propSetPerimEnabled(prop, false);
	}

	if (tank->speed != 0.0f || tank->turnspeed != 0.0f) {
		tankCrush(tank, halfwidth, halflength);
		tankDriveOverProps(tank);
	}

	tankSounds(tank);
}

/* ------------------------------------------------------------------------ */
/* The model                                                                 */
/* ------------------------------------------------------------------------ */

void geTankUpdateModel(struct prop *prop)
{
	struct tankobj *tank = (struct tankobj *)prop->obj;
	struct model *model = tank->base.model;
	Mtxf rot;

	if (!model || !model->definition) {
		return;
	}

	// propobj.c's PROPDEF_TANK: the turret about its pivot by the angle against
	// the hull, the barrel under it about its own by the elevation
	mtx4LoadYRotation(tankWrap(tank->turretyaw), &rot);
	gexPlusVehiclePutPart(model, TANK_PART_TURRET, &rot);

	mtx4LoadXRotation(tankWrap(-tank->turretpitch), &rot);
	gexPlusVehiclePutPart(model, TANK_PART_BARREL, &rot);

	mtx4LoadYRotation(M_BADTAU * 0.25f, &rot);
	gexPlusVehiclePutPart(model, TANK_PART_MUZZLE, &rot);

	// the muzzle's flash while it fires, and GoldenEye's switch 8, which it
	// turns off every time it draws a tank
	{
		struct modelnode *flash = modelGetPart(model->definition, TANK_PART_FLASH);
		struct modelnode *off = modelGetPart(model->definition, TANK_PART_OFF);

		if (flash && (flash->type & 0xff) == MODELNODETYPE_CHRGUNFIRE) {
			((union modelrwdata *)modelGetNodeRwData(model, flash))->chrgunfire.visible = tank->firing > 0;
		}

		if (off && (off->type & 0xff) == MODELNODETYPE_TOGGLE) {
			((union modelrwdata *)modelGetNodeRwData(model, off))->toggle.visible = false;
		}
	}
}

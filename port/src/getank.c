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
 * Bond climbs onto it as GoldenEye has him do (geTankBoard()): a tank he walks
 * into lifts him onto its hull, and its turret from there. Not GoldenEye's: he
 * can get in from close beside it as well as from on top of the turret. He
 * gets out on top of it, as GoldenEye leaves him (beside it only when a death
 * or an ending pushes him out). The shells are a weapon as GoldenEye's are (WEAPON_GE_TANKSHELLS,
 * on the Data Uplink as the gadgets with nothing in the hand are): given and
 * held as he climbs in, taken as he climbs out, their trigger the cannon, and
 * anything else he switches to in there fires as it always does.
 */
#include <math.h>
#include <stdlib.h>
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
#include "game/camera.h"
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
#include "gedeathcam.h"
#include "gexplusveh.h"
#include "modloader.h"
#include "romdata.h"
#include "net/net.h"
#include "system.h"

#define TANK_MAX_SPEED      15.0f
#define TANK_TURN_FILTER    0.92f
#define TANK_TURN_INPUT     0.3f
#define TANK_TURN_SCALE     3.5f
#define TANK_ENTER_FRAMES   45.0f
#define TANK_PENALTY_TICKS  90
#define TANK_SHELL_SPEED    66.666664f
// GoldenEye's tank shells (gunWeaponStats.inc.c tank_stats): a magazine of
// one, and after each shot the hand's recoil state holds for the first byte
// of its RecoilSpeed, 0x78 = 120 ticks (a rocket launcher's is 0), then the
// empty magazine's reload swaps for 17 (WHEN_A_FLD8B0; the hand is hidden,
// so nothing is lowered or raised) - about two and a quarter seconds
#define TANK_SHELL_GAP      TICKS(120 + 17)
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

// how far the turret is turned between tests of the barrel: 5 degrees
#define TANK_TURRET_STEP (5.0f * M_BADTAU / 360.0f)

#define TANK_PART_TURRET 1
#define TANK_PART_SEAT   2
#define TANK_PART_BARREL 3
#define TANK_PART_MUZZLE 4
#define TANK_PART_FLASH  7
#define TANK_PART_OFF    8
#define TANK_PART_HULLBOX   5
#define TANK_PART_TURRETBOX 6

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
	s32 leftweaponwas; // the left hand's, which under Akimbo is its own gun
	s32 shellsin;     // ticks until the shells he was handed are put in his hand
	s32 crushes;
	f32 entertheta;
	f32 enterverta;
	f32 enterclimb;   // how far up the tank he stood as he got in
	s32 enginevol;    // g_TankEngineSfxVolume, GoldenEye's 0 to 32767
	struct prop *fadeprop; // the tank whose engine dies away after he left it
	u32 engineuuid;   // the engine's and the treads' channels, and the
	s32 enginebase;   // volume each was started at, which GoldenEye's
	u32 treaduuid;    // volume is a share of
	s32 treadbase;
} g_Tank[MAX_PLAYERS];

// the stage's tanks (geTankCreate), for a climb onto one being driven online
#define TANK_MAX_LISTED 8
static struct defaultobj *g_TankObjs[TANK_MAX_LISTED];
static s32 g_NumTankObjs;

// netplay: a player riding on a tank somebody else drives (geTankRideCarry):
// the tank's place and heading when this player's last tick looked
static struct {
	struct prop *prop;
	f32 pos[3];
	f32 yaw;
} g_TankRide[MAX_PLAYERS];

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

	// the shells are the setup's (thirty in both of GoldenEye's tanks), and
	// the loader has put them there
	if (tank->shells < 0 || tank->shells > 999) {
		tank->shells = 0;
	}

	tank->firing = 0;
	tank->speed = 0;
	tank->turnspeed = 0;

	if (g_NumTankObjs < TANK_MAX_LISTED) {
		g_TankObjs[g_NumTankObjs++] = obj;
	}
}

void geTankReset(void)
{
	memset(g_Tank, 0, sizeof(g_Tank));
	memset(g_TankRide, 0, sizeof(g_TankRide));
	g_NumTankObjs = 0;
}

s32 geTankIsDriving(void)
{
	return g_Tank[g_Vars.currentplayernum].state != TANK_OUT && tankDriven() != NULL;
}

/**
 * The cannon in hand: its sight stays in the middle of the view. The shell
 * leaves along the turret, which is the view (tankFire()), wherever the
 * crosshair is - GoldenEye's gunFireTankShell() takes the turret's direction
 * whenever the tank is running and never the sight's - and GoldenEye itself
 * holds the sight across in aim mode with the shells (bondview2.c:
 * controlStickXRaw = 0, the stick turning the tank). Perfect Dark's sight
 * swung with the turn and the mouse and pointed where no shell went (F3
 * 20260930-185749, "the crosshair should be locked in the middle").
 */
s32 geTankHoldsSight(void)
{
	return geTankIsDriving() && bgunGetWeaponNum(HAND_RIGHT) == WEAPON_GE_TANKSHELLS;
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

/**
 * The tank a projectile's owner is driving, which its flight test passes
 * through. The shell starts at the barrel's end (tankMuzzle()), and the
 * turret's and barrel's collision blocks are the model's second and later
 * ones, which propSetPerimEnabled() leaves standing (objUpdateGeometry()):
 * every shell hit its own tank on its first tick and went off at the muzzle
 * (F3 20260928-230650, Runway: "it explodes right away").
 */
struct prop *geTankShellTank(struct prop *ownerprop)
{
	if (!ownerprop || ownerprop->type != PROPTYPE_PLAYER) {
		return NULL;
	}

	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		struct player *player = g_Vars.players[i];

		if (player && player->prop == ownerprop && g_Tank[i].state != TANK_OUT) {
			struct prop *tank = player->unk1af0;

			return tank && tank->type == PROPTYPE_OBJ && tank->obj
				&& tank->obj->type == OBJTYPE_TANK ? tank : NULL;
		}
	}

	return NULL;
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

static struct modelrodata_bbox *tankPartBbox(struct tankobj *tank, s32 part);

f32 geTankEyeHeight(f32 eyeheight)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = g_Tank[p].state != TANK_OUT ? tankDriven() : NULL;
	struct modelrodata_bbox *hull;
	struct modelrodata_bbox *turret;
	struct coord seat;
	f32 target;
	f32 t;

	if (!tank) {
		return eyeheight;
	}

	// GoldenEye's height in the tank (bondview2.c, g_PlayerTankYOffset):
	// Bond's floor is the hull's box and the turret's box stacked on the
	// ground under the tank, 37 down into the hatch, and he is half crouched
	// in it (CROUCH_HALF: 60 off a standing eye). Goldfinger 64's Aston
	// Martin (Alps) is the tank with a low car's boxes: the eye comes out
	// 115 over the road, in the seat, where a seat-plus-a-crouched-man rule
	// held at least a standing eye stood him on the bonnet (F3
	// 20261011-062449, "Bond is supposed to sit inside"). GoldenEye's own
	// tank comes out within four units of that rule.
	hull = tankPartBbox(tank, TANK_PART_HULLBOX);
	turret = tankPartBbox(tank, TANK_PART_TURRETBOX);

	if (hull) {
		target = (hull->ymax - hull->ymin) * tankScale(tank) - 37.0f - 60.0f + eyeheight;

		if (turret) {
			target += (turret->ymax - turret->ymin) * tankScale(tank);
		}
	} else {
		// no box: the seat is where he *stands*, and the eye a crouched
		// man's over it
		tankSeat(tank, &seat);
		target = tank->base.prop->pos.y + seat.y + TANK_EYE_OVER_SEAT - g_Vars.currentplayer->vv_manground;

		if (target < eyeheight) {
			target = eyeheight;
		}
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
/* On the tank                                                               */
/* ------------------------------------------------------------------------ */

#define TANK_CLIMB_SPEED    20.0f

// the collision stops him a hair short of touching
#define TANK_TOUCH_SLACK    5.0f

static struct modelrodata_bbox *tankPartBbox(struct tankobj *tank, s32 part)
{
	struct modeldef *def = tank->base.model ? tank->base.model->definition : NULL;
	struct modelnode *node = def ? modelGetPart(def, part) : NULL;

	return node && (node->type & 0xff) == MODELNODETYPE_BBOX ? &node->rodata->bbox : NULL;
}

/** Whether a circle in the plan touches a box of the tank's, given in the box's own axes about `yaw`. */
static s32 tankBoxTouches(f32 dx, f32 dz, f32 yaw, f32 radius, f32 xmin, f32 xmax, f32 zmin, f32 zmax)
{
	const f32 s = sinf(yaw);
	const f32 c = cosf(yaw);
	const f32 across = dx * c - dz * s;
	const f32 along = dx * s + dz * c;
	const f32 ox = across < xmin ? xmin - across : (across > xmax ? across - xmax : 0.0f);
	const f32 oz = along < zmin ? zmin - along : (along > zmax ? along - zmax : 0.0f);

	return ox * ox + oz * oz <= radius * radius;
}

/**
 * Whether a circle in the plan touches the tank: against its own collision's
 * polygon, as GoldenEye asks (chraiGetCollisionBoundsWithoutY()) - it is the
 * collision that stopped him there, and a box worked out any other way
 * disagrees with it by enough that he was let go of and stopped again for
 * ever - and against the hull's box where it has none.
 */
static s32 tankHullTouches(struct tankobj *tank, f32 x, f32 z, f32 radius)
{
	struct defaultobj *obj = &tank->base;

	if (obj->geoblock && (obj->hidden2 & OBJH2FLAG_08) && !(obj->flags3 & OBJFLAG3_GEOCYL)
			&& obj->geoblock->header.type == GEOTYPE_BLOCK && obj->geoblock->header.numvertices >= 3) {
		const struct geoblock *block = obj->geoblock;
		const s32 num = block->header.numvertices;
		s32 side = 0;
		s32 inside = 1;

		for (s32 i = 0; i < num; i++) {
			const f32 ax = block->vertices[i][0];
			const f32 az = block->vertices[i][1];
			const f32 ex = block->vertices[(i + 1) % num][0] - ax;
			const f32 ez = block->vertices[(i + 1) % num][1] - az;
			const f32 cross = ex * (z - az) - ez * (x - ax);
			const f32 len2 = ex * ex + ez * ez;
			f32 t = len2 > 0.0f ? ((x - ax) * ex + (z - az) * ez) / len2 : 0.0f;
			f32 nx;
			f32 nz;

			// inside a convex polygon is the same side of every edge
			if (cross != 0.0f) {
				if (side == 0) {
					side = cross > 0.0f ? 1 : -1;
				} else if ((cross > 0.0f ? 1 : -1) != side) {
					inside = 0;
				}
			}

			t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
			nx = x - (ax + ex * t);
			nz = z - (az + ez * t);

			if (nx * nx + nz * nz <= radius * radius) {
				return 1;
			}
		}

		return inside;
	}

	{
		struct modelrodata_bbox *hull = tankPartBbox(tank, TANK_PART_HULLBOX);
		const f32 scale = tankScale(tank);

		return hull && tankBoxTouches(x - obj->prop->pos.x, z - obj->prop->pos.z, tank->hullyaw, radius,
				hull->xmin * scale, hull->xmax * scale, hull->zmin * scale, hull->zmax * scale);
	}
}

/**
 * Bond on the tank, before he is in it: the top of GoldenEye's
 * bondviewCalcUpdatePlayerCollision(), which Perfect Dark kept every field of
 * and none of the code. A tank Bond walks into is remembered (`tank`, its
 * g_WorldTankProp - bondwalk.c still does that much) and is no obstacle to him
 * from then on, because GoldenEye *lifts him onto it*: while he is over its
 * hull the ground under him is the hull's top (`bondonground`, its
 * g_PlayerTankYOffset, climbed at twenty a frame with his move held until he
 * is up), over its turret it is the turret's top and he can get in
 * (`bondonturret`, its g_BondCanEnterTank), and when he walks off it the tank
 * is let go of and is an obstacle again. With the first half alone he walked
 * through it ("Bond now clips through the tank").
 *
 * Answers whether the move is to be held.
 */
s32 geTankBoard(void)
{
	struct player *player = g_Vars.currentplayer;
	struct prop *prop = player->tank;
	struct prop *playerprop = player->prop;
	struct tankobj *tank;
	struct modelrodata_bbox *hull;
	struct modelrodata_bbox *turret;
	struct modelnode *root;
	struct modelnode *pivot;
	f32 scale;
	f32 dx;
	f32 dz;
	f32 top;

	player->bondonturret = false;

	if (g_Tank[g_Vars.currentplayernum].state != TANK_OUT) {
		// in it: he sits on the ground the tank stands on, and the seat is
		// the eye's business (geTankEyeHeight()). What he had climbed goes as
		// he is taken across to the seat
		const f32 remain = g_Tank[g_Vars.currentplayernum].state == TANK_ENTERING
			? (cosf(g_Tank[g_Vars.currentplayernum].entert * M_BADTAU * 0.5f) + 1.0f) * 0.5f : 0.0f;
		const f32 was = player->bondonground;

		player->bondonground = g_Tank[g_Vars.currentplayernum].enterclimb * remain;

		// and he is lowered with it, as GoldenEye sets his height outright
		// (g_PlayerTankYOffset). Left to the walk, the ground went out from
		// under him and he fell after it, lagging and then landing hard: the
		// eye went down past the seat and bounced back up to it (F3
		// 20261014-224847)
		if (was > player->bondonground && !player->isfalling
				&& player->vv_manground >= player->vv_ground - 1.0f) {
			player->vv_manground -= was - player->bondonground;
			player->vv_ground = player->vv_manground;
			player->bdeltapos.y = 0.0f;
		}

		return 0;
	}

	if (!prop && g_NetMode != NETMODE_NONE && !player->isdead) {
		// online, a tank somebody else drives: its collision is off while it
		// is driven (geTankTick), so walking into it never named it as the
		// walk names a parked one (bondwalk.c, which also passes over a
		// mounted one); one whose hull he is over is climbed the same way.
		// Offline nobody else drives
		for (s32 i = 0; i < g_NumTankObjs; i++) {
			struct defaultobj *obj = g_TankObjs[i];
			struct prop *tprop = obj ? obj->prop : NULL;
			f32 hw, hl, height, bottom;

			if (!tprop || !(obj->hidden & OBJHFLAG_MOUNTED) || !obj->model || !objIsHealthy(obj)
					|| player->unk1af0 == tprop) {
				continue;
			}

			tankSize((struct tankobj *)obj, &hw, &hl, &height, &bottom);

			if (playerprop->pos.y - tprop->pos.y > height + 250.0f || playerprop->pos.y - tprop->pos.y < -100.0f) {
				continue;
			}

			if (tankHullTouches((struct tankobj *)obj, playerprop->pos.x, playerprop->pos.z, player->bond2.radius)) {
				player->tank = prop = tprop;
				break;
			}
		}
	}

	if (!prop) {
		return 0;
	}

	if (prop->type != PROPTYPE_OBJ || !prop->obj || prop->obj->type != OBJTYPE_TANK
			|| !prop->obj->model || !objIsHealthy(prop->obj) || player->isdead) {
		player->tank = NULL;
		player->bondonground = 0;
		return 0;
	}

	tank = (struct tankobj *)prop->obj;

	hull = tankPartBbox(tank, TANK_PART_HULLBOX);
	turret = tankPartBbox(tank, TANK_PART_TURRETBOX);
	root = tank->base.model->definition->rootnode;
	pivot = modelGetPart(tank->base.model->definition, TANK_PART_TURRET);
	scale = tankScale(tank);

	if (!hull) {
		// not GoldenEye's model: nothing to stand on, so it stays a wall
		player->tank = NULL;
		player->bondonground = 0;
		return 0;
	}

	dx = playerprop->pos.x - prop->pos.x;
	dz = playerprop->pos.z - prop->pos.z;

	// the hull's box hangs from the root, whose own offset moves it
	if (root && (root->type & 0xff) == MODELNODETYPE_POSITION) {
		const f32 s = sinf(tank->hullyaw);
		const f32 c = cosf(tank->hullyaw);

		dx -= (root->rodata->position.pos.x * c + root->rodata->position.pos.z * s) * scale;
		dz -= (-root->rodata->position.pos.x * s + root->rodata->position.pos.z * c) * scale;
	}

	if (!tankHullTouches(tank, playerprop->pos.x, playerprop->pos.z, player->bond2.radius + TANK_TOUCH_SLACK)) {
		// off it: bondview2.c lets go of the tank, which is solid again
		player->tank = NULL;
		player->bondonground = 0;
		return 0;
	}

	top = (hull->ymax - hull->ymin) * scale;

	if (turret && pivot && (pivot->type & 0xff) == MODELNODETYPE_POSITION) {
		// the turret's box is about its pivot and turns with it; his middle
		// has to be over it, as GoldenEye asks of its rectangle
		const f32 s = sinf(tank->hullyaw);
		const f32 c = cosf(tank->hullyaw);
		const f32 px = pivot->rodata->position.pos.x * scale;
		const f32 pz = pivot->rodata->position.pos.z * scale;

		if (tankBoxTouches(dx - (px * c + pz * s), dz - (-px * s + pz * c),
					tankWrap(tank->hullyaw + tank->turretyaw), 0.0f,
					turret->xmin * scale, turret->xmax * scale, turret->zmin * scale, turret->zmax * scale)) {
			top += (turret->ymax - turret->ymin) * scale;
			player->bondonturret = true;
		}
	}

	if (player->bondonground < top) {
		player->bondonground += TANK_CLIMB_SPEED * g_Vars.lvupdate60freal;

		if (player->bondonground < top) {
			return 1;
		}
	}

	player->bondonground = top;

	return 0;
}

/* ------------------------------------------------------------------------ */
/* In and out                                                                */
/* ------------------------------------------------------------------------ */

static struct prop *tankNear(void)
{
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct prop *best = NULL;
	f32 bestdist = 0;

	// The live props, active then paused - never g_Vars.props slot by slot: a
	// freed slot keeps its type and a pointer to an object that is gone, and
	// pressing activate on Dam read one (crash report 20260921-055851)
	s32 guard = 0;

	for (struct prop *prop = g_Vars.activeprops; prop && guard++ < g_Vars.maxprops; prop = prop->next) {
		f32 halfwidth, halflength, height, bottom;
		f32 reach;
		f32 dx;
		f32 dz;
		f32 dist;

		if (prop->type != PROPTYPE_OBJ || !prop->obj || prop->obj->type != OBJTYPE_TANK
				|| !prop->obj->model || !objIsHealthy(prop->obj)) {
			continue;
		}

		// somebody is in it already: one driver to a tank. GoldenEye has
		// one player on its missions; co-op (online) seats several, and a
		// second had climbed into the tank another drove (F3 20261010-050254)
		if (prop->obj->hidden & OBJHFLAG_MOUNTED) {
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
	struct prop *fadeprop = g_Tank[p].fadeprop;
	const u32 engineuuid = g_Tank[p].engineuuid;
	const s32 enginebase = g_Tank[p].enginebase;

	// back in while the last tank's engine was still dying away: GoldenEye
	// keeps that sound and raises it again (g_TankSfxState[0] is not NULL,
	// so TRUCK_START is not played afresh). Another tank's is let go.
	if (fadeprop && fadeprop != prop) {
		psStopSound(fadeprop, PSTYPE_CHOPPERHUM1, 0xffff);
	}

	memset(&g_Tank[p], 0, sizeof(g_Tank[p]));

	// the dying engine's own volume is what it is raised again from
	g_Tank[p].engineuuid = fadeprop == prop ? engineuuid : 0;
	g_Tank[p].enginebase = fadeprop == prop ? enginebase : -1;
	g_Tank[p].treadbase = -1;

	g_Vars.currentplayer->unk1af0 = prop;
	g_Tank[p].state = TANK_ENTERING;
	g_Tank[p].enterpos = g_Vars.currentplayer->prop->pos;
	g_Tank[p].lastshot60 = g_Vars.lvframe60 - TANK_SHELL_GAP;
	g_Tank[p].entertheta = g_Vars.currentplayer->vv_theta;
	g_Tank[p].enterverta = g_Vars.currentplayer->vv_verta;
	g_Tank[p].enterclimb = g_Vars.currentplayer->tank == prop ? g_Vars.currentplayer->bondonground : 0.0f;

	// driven, it is unk1af0's: the walk switches `tank` back on after every
	// step it takes, and a tank being driven stays off (geTankTick())
	g_Vars.currentplayer->tank = NULL;

	// what was left in it (bondview2.c: add_ammo_to_weapon(ITEM_TANKSHELLS, unkD8))
	bgunSetAmmoQuantity(TANK_AMMOTYPE, bgunGetReservedAmmoCount(TANK_AMMOTYPE) + tank->shells);
	tank->shells = 0;
	tank->speed = 0;
	tank->turnspeed = 0;

	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;

	// bondview2.c: he is handed the tank's shells as he gets in, and holds
	// them - GoldenEye's ITEM_TANKSHELLS, nothing in the hand. They go in
	// his hand two ticks on (geTankTick()), once the lists have run with
	// him in it: a level's script may take them away again (TND64's City)
	g_Tank[p].weaponwas = bgunGetWeaponNum(HAND_RIGHT);
	g_Tank[p].leftweaponwas = bgunGetWeaponNum(HAND_LEFT);
	invGiveSingleWeapon(WEAPON_GE_TANKSHELLS);
	g_Tank[p].shellsin = 2;

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

/**
 * The shells are taken off him (bondinvRemoveItemByID()), and what he held
 * as he climbed in goes back in his hands - both of them. Under Akimbo the
 * left hand holds a gun of its own, and a switch of the right hand alone
 * lets the weapon switch choose the left: the gun the right held, carried
 * over, and failing that a second of the right's gun if the inventory has
 * two. The shells are out of the inventory by then, so it was the second
 * copy - the pistol in both hands the tester climbed out with, whatever the
 * left had held - or nothing. bgunEquipHands() gives back the pair.
 *
 * Both only when the shells are in a hand: anything else he switched to in
 * there is his to keep, and has the left hand's gun given back beside it,
 * since the driver has none. (Before the driver lost his left hand, Akimbo
 * carried the shells into it as he switched away from them - they are the
 * Data Uplink's, which Akimbo counts as a gun - and a hand left holding them
 * would hold nothing he has.)
 */
static void tankGiveBackHands(void)
{
	const s32 p = g_Vars.currentplayernum;

	if (bgunGetWeaponNum(HAND_RIGHT) == WEAPON_GE_TANKSHELLS
			|| bgunGetWeaponNum(HAND_LEFT) == WEAPON_GE_TANKSHELLS
			|| g_Vars.currentplayer->gunctrl.switchtoweaponnum == WEAPON_GE_TANKSHELLS) {
		s32 right = g_Tank[p].weaponwas > WEAPON_NONE ? g_Tank[p].weaponwas : WEAPON_UNARMED;
		s32 left = g_Tank[p].leftweaponwas > WEAPON_NONE ? g_Tank[p].leftweaponwas : WEAPON_NONE;

		if (right == WEAPON_GE_TANKSHELLS) {
			right = WEAPON_UNARMED;
		}

		if (left == WEAPON_GE_TANKSHELLS) {
			left = WEAPON_NONE;
		}

		bgunEquipHands(right, left);
	} else if (g_Tank[p].leftweaponwas > WEAPON_NONE
			&& g_Tank[p].leftweaponwas != WEAPON_GE_TANKSHELLS
			&& bgunGetWeaponNum(HAND_LEFT) != g_Tank[p].leftweaponwas) {
		// he switched to something else in there, which is his to keep,
		// and the driver has no left hand (bgunTickSwitch2()): the left's
		// gun is given back beside it
		struct gunctrl *ctrl = &g_Vars.currentplayer->gunctrl;

		bgunEquipHands(ctrl->switchtoweaponnum >= 0 ? ctrl->switchtoweaponnum : ctrl->weaponnum,
				g_Tank[p].leftweaponwas);
	}

	invRemoveItemByNum(WEAPON_GE_TANKSHELLS);
}

/**
 * A tank sound's volume as GoldenEye sets it (sndCreatePostEvent(.., 8, vol)),
 * as a share of the volume its channel was started at. False if it is not
 * playing.
 */
static s32 tankSoundVolume(struct prop *prop, s32 type, s32 vol, u32 *uuid, s32 *base)
{
	for (s32 i = 0; i < 40; i++) {
		struct pschannel *channel = &g_PsChannels[i];

		if ((channel->flags & PSFLAG_FREE) == 0 && (channel->flags2 & PSFLAG2_STOPPED) == 0
				&& channel->prop == prop && channel->type == type) {
			if (channel->uuid != *uuid || *base < 0) {
				*uuid = channel->uuid;
				*base = channel->vol10;
			}

			if (vol < 0) {
				vol = 0;
			} else if (vol > AL_VOL_FULL) {
				vol = AL_VOL_FULL;
			}

			channel->vol10 = *base * vol / AL_VOL_FULL;
			return true;
		}
	}

	return false;
}

/**
 * Out of the tank: bondview2.c's not-in-tank branch stops the treads (TANK)
 * at once and lets the engine die away, 1000 off its volume a tick
 * (geTankTick() carries it on). The engine had been TRUCK_START played once
 * at a volume worked out where the player stood, so it ran on everywhere
 * at that volume (F3 20260930-235603).
 */
static void tankStopSounds(struct prop *prop)
{
	const s32 p = g_Vars.currentplayernum;

	psStopSound(prop, PSTYPE_CHOPPERHUM2, 0xffff);

	if (g_Tank[p].enginevol > 0 && g_Tank[p].enginebase >= 0) {
		g_Tank[p].fadeprop = prop;
	} else {
		psStopSound(prop, PSTYPE_CHOPPERHUM1, 0xffff);
	}
}

/** The engine dying away after he climbed out (tankStopSounds()). */
static void tankFadeEngine(void)
{
	const s32 p = g_Vars.currentplayernum;
	struct prop *prop = g_Tank[p].fadeprop;

	if (!prop) {
		return;
	}

	g_Tank[p].enginevol -= 1000 * g_Vars.lvupdate60;

	if (g_Tank[p].enginevol <= 0
			|| !tankSoundVolume(prop, PSTYPE_CHOPPERHUM1, g_Tank[p].enginevol, &g_Tank[p].engineuuid, &g_Tank[p].enginebase)) {
		psStopSound(prop, PSTYPE_CHOPPERHUM1, 0xffff);
		g_Tank[p].fadeprop = NULL;
		g_Tank[p].enginevol = 0;
	}
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

	// GoldenEye's way out (bondview2.c, B in the tank): he stands up where he
	// sits, on the turret, and climbs the 37 out of the hatch as he does onto
	// the hull (geTankBoard()), now that the tank is under him and not
	// round him. The eye is where it was in the seat - the hull's top and the
	// turret's, 37 and a half crouch's 60 down (geTankEyeHeight()) - and goes
	// on up from there. Pushed out by a death or an ending he is put down
	// beside it, as is a tank whose model has no boxes to stand on
	if (!force && !g_Vars.currentplayer->isdead && tankPartBbox(tank, TANK_PART_HULLBOX)) {
		struct modelrodata_bbox *hull = tankPartBbox(tank, TANK_PART_HULLBOX);
		struct modelrodata_bbox *turret = tankPartBbox(tank, TANK_PART_TURRETBOX);
		struct player *player = g_Vars.currentplayer;
		f32 top = (hull->ymax - hull->ymin) * tankScale(tank);
		f32 climb;

		if (turret) {
			top += (turret->ymax - turret->ymin) * tankScale(tank);
		}

		climb = top - 37.0f - 60.0f;

		if (climb < 0.0f) {
			climb = 0.0f;
		}

		tank->shells = bgunGetReservedAmmoCount(TANK_AMMOTYPE);
		bgunSetAmmoQuantity(TANK_AMMOTYPE, 0);
		tankGiveBackHands();
		tank->firing = 0;
		tank->speed = 0;
		tank->turnspeed = 0;
		tank->base.hidden &= ~OBJHFLAG_MOUNTED;

		tankStopSounds(tank->base.prop);

		g_Tank[p].state = TANK_OUT;
		player->unk1af0 = NULL;

		// solid again to everyone else; to him it is what he stands on, and
		// the walk leaves it out of his moves while it is `tank`
		propSetPerimEnabled(tank->base.prop, true);
		player->tank = tank->base.prop;
		player->bondonground = climb;
		player->vv_manground += climb;
		player->vv_ground = player->vv_manground;
		player->sumground = player->vv_manground / (PAL ? 0.054400026798248f : 0.045499980449677f);
		player->bdeltapos.y = 0.0f;
		player->speedforwards = 0;
		player->speedsideways = 0;

		return 1;
	}

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

	if (found && !g_Vars.currentplayer->isdead) {
		playerprop->pos.x = pos.x;
		playerprop->pos.z = pos.z;
		propDeregisterRooms(playerprop);
		roomsCopy(rooms, playerprop->rooms);
		propRegisterRooms(playerprop);
	}

	// bondview2.c: what he had goes back into the tank
	tank->shells = bgunGetReservedAmmoCount(TANK_AMMOTYPE);
	bgunSetAmmoQuantity(TANK_AMMOTYPE, 0);

	// and the shells are taken off him, and whatever he held before them
	// goes back in his hands
	tankGiveBackHands();
	tank->firing = 0;
	tank->speed = 0;
	tank->turnspeed = 0;
	tank->base.hidden &= ~OBJHFLAG_MOUNTED;

	tankStopSounds(tank->base.prop);

	g_Tank[p].state = TANK_OUT;
	g_Vars.currentplayer->unk1af0 = NULL;

	// put down beside it, he has let go of it (geTankBoard()): it is solid
	// to him again until he next walks into it
	g_Vars.currentplayer->tank = NULL;
	g_Vars.currentplayer->bondonground = 0;
	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;

	return 1;
}

/**
 * The player died, from playerDieByShooter() - the walk that ticks the tank
 * (geTankTick()) is not run for a dead player, so a death in the tank left him
 * in it and blew nothing up (F3 20261002-233320, "the tank should be
 * exploding"). bondview2.c: a tank explodes with Bond in it - on a mission
 * played alone as each death replay starts, watched instead of his body
 * (gedeathcam.c), and anywhere else there and then. He is let out of it where
 * he is (tankExit() leaves a dead player where he fell), so a respawn does not
 * carry the tank with him.
 */
void geTankPlayerDied(void)
{
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank;
	struct prop *prop;

	if (g_Tank[p].state == TANK_OUT) {
		return;
	}

	tank = tankDriven();

	if (tank && objIsHealthy(&tank->base)) {
		prop = tank->base.prop;

		if (!geDeathCamTankDeath(prop)) {
			explosionCreateSimple(prop, &prop->pos, prop->rooms, EXPLOSIONTYPE_ROCKET, p);
		}
	}

	tankExit(1);
}

/**
 * An ending's camera (CameraSwitch) takes the player out of the tank.
 * GoldenEye never does: its ending draws a Bond of its own, loaded by the
 * CameraSwitch, and the tank's Bond is not drawn at all. Here the ending's
 * Bond is the player's own body, which a driver's is not drawn
 * (geTankHidesChr()) and which the tank is put under every tick - Streets'
 * ending (F3 20260927-000905) teleported him to his pad inside a tank no one
 * could see him in.
 */
void geTankLeaveForCutscene(void)
{
	const s32 prevplayernum = g_Vars.currentplayernum;

	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Tank[i].state != TANK_OUT) {
			setCurrentPlayerNum(i);
			tankExit(1);
		}
	}

	setCurrentPlayerNum(prevplayernum);
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

static s32 tankMuzzle(struct tankobj *tank, struct coord *out);

/**
 * Whether the barrel is in something with the hull at `hullyaw` and the
 * turret at `turretyaw`: the end of bondviewTankCollisionStatus(), a line
 * from where Bond sits out to the barrel's tip (the muzzle's offset from the
 * seat, level, turned by hull and turret), through what stops the hull and
 * at the hull's own height, as GoldenEye's stan line is flat. `step` moves
 * him first, as it moves the hull.
 */
static s32 tankBarrelBlocked(struct tankobj *tank, f32 hullyaw, f32 turretyaw, struct coord *step)
{
	const s32 types = CDTYPE_BG | CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_OBJSIMMUNETOEXPLOSIONS;
	struct prop *playerprop = g_Vars.currentplayer->prop;
	const f32 savedhull = tank->hullyaw;
	const f32 savedturret = tank->turretyaw;
	const f32 savedpitch = tank->turretpitch;
	struct coord seat;
	struct coord tip;
	struct coord from;
	struct coord to;
	RoomNum fromrooms[8];
	s32 ok;

	tank->hullyaw = hullyaw;
	tank->turretyaw = turretyaw;
	tank->turretpitch = 0;
	tankSeat(tank, &seat);
	ok = tankMuzzle(tank, &tip);
	tank->hullyaw = savedhull;
	tank->turretyaw = savedturret;
	tank->turretpitch = savedpitch;

	if (!ok) {
		return false;
	}

	from.x = playerprop->pos.x + (step ? step->x : 0.0f);
	from.y = g_Vars.currentplayer->vv_manground + TANK_RECT_HEIGHT;
	from.z = playerprop->pos.z + (step ? step->z : 0.0f);

	to.x = from.x + tip.x - seat.x;
	to.y = from.y;
	to.z = from.z + tip.z - seat.z;

	propSetPerimEnabled(tank->base.prop, false);
	func0f065e74(&playerprop->pos, playerprop->rooms, &from, fromrooms);

	return cdExamLos08(&from, fromrooms, &to, types, TANK_RECT_GEOFLAGS) == CDRESULT_COLLISION;
}

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

	// and the barrel, which GoldenEye tests with the hull
	if (tankBarrelBlocked(tank, yaw, tank->turretyaw, step)) {
		goto blocked;
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

	// the live props only, as tankNear() walks them; the next one is taken
	// first, since what is driven over may leave the list
	struct prop *next = NULL;
	s32 guard = 0;

	for (struct prop *prop = g_Vars.activeprops; prop && guard++ < g_Vars.maxprops; prop = next) {
		struct defaultobj *obj;

		next = prop->next;

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
		// The view's angle grows as it turns to the right (a stick pushed right
		// is a positive speedtheta, and bwalkUpdateTheta() adds it), and a
		// prop's heading - (sin, cos) where the player's is (-sin, cos) - grows
		// to the left: so a push to the right takes the hull's angle down.
		// Written the other way round until a tester drove it: left was right
		turnedby = -g_Tank[p].speedtheta * frames * TANK_TURN_SCALE * M_BADTAU / 360.0f;

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

/**
 * Where the barrel ends, from the middle of the tank, in the world: the muzzle
 * (part 4) along the barrel (part 3) tipped by the elevation, the pair about
 * the turret's pivot (part 1) by the turret's angle, and all of it by the
 * hull's - carried round as tankSeat() carries the seat, and as
 * geTankUpdateModel() poses the parts. The model's own matrices are no use
 * here: once drawn they are the renderer's fixed point.
 */
static s32 tankMuzzle(struct tankobj *tank, struct coord *out)
{
	struct modeldef *def = tank->base.model ? tank->base.model->definition : NULL;
	struct modelnode *pivot = def ? modelGetPart(def, TANK_PART_TURRET) : NULL;
	struct modelnode *barrel = def ? modelGetPart(def, TANK_PART_BARREL) : NULL;
	struct modelnode *muzzle = def ? modelGetPart(def, TANK_PART_MUZZLE) : NULL;
	const f32 scale = tankScale(tank);
	struct coord m;
	struct coord b;
	struct coord local;
	f32 s;
	f32 c;

	if (!pivot || (pivot->type & 0xff) != MODELNODETYPE_POSITION
			|| !barrel || (barrel->type & 0xff) != MODELNODETYPE_POSITION
			|| !muzzle || (muzzle->type & 0xff) != MODELNODETYPE_POSITION) {
		return false;
	}

	// the muzzle about the barrel's pivot by the elevation (up is +y)
	m = muzzle->rodata->position.pos;
	s = sinf(tank->turretpitch);
	c = cosf(tank->turretpitch);

	b.x = barrel->rodata->position.pos.x + m.x;
	b.y = barrel->rodata->position.pos.y + m.y * c + m.z * s;
	b.z = barrel->rodata->position.pos.z - m.y * s + m.z * c;

	// then about the turret's pivot, then with the hull
	s = sinf(tank->turretyaw);
	c = cosf(tank->turretyaw);

	local.x = pivot->rodata->position.pos.x + b.x * c + b.z * s;
	local.y = pivot->rodata->position.pos.y + b.y;
	local.z = pivot->rodata->position.pos.z - b.x * s + b.z * c;

	s = sinf(tank->hullyaw);
	c = cosf(tank->hullyaw);

	out->x = (local.x * c + local.z * s) * scale;
	out->y = local.y * scale;
	out->z = (-local.x * s + local.z * c) * scale;

	return true;
}

/**
 * A shell out of pos along dir (yaw and pitch its angles), the player's own:
 * the tank's cannon, or GoldenEye's shells fired on foot (tankFireOnFoot()).
 */
static void tankLaunchShell(struct coord *posarg, struct coord *dirarg, f32 yaw, f32 pitch)
{
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct weaponobj *shell;
	struct coord pos = *posarg;
	struct coord dir = *dirarg;
	struct coord speed;
	Mtxf rot;
	Mtxf identity;

	shell = weaponCreateProjectileFromWeaponNum(MODEL_CHRDYROCKETMIS, WEAPON_ROCKET, playerprop->chr);

	if (!shell) {
		return;
	}

	mtx4LoadIdentity(&identity);
	mtx4LoadXRotation(-pitch, &rot);
	mtx4LoadYRotation(yaw, &identity);
	mtx4MultMtx4InPlace(&identity, &rot);
	mtx4LoadIdentity(&identity);

	speed.x = dir.x * TANK_SHELL_SPEED;
	speed.y = dir.y * TANK_SHELL_SPEED;
	speed.z = dir.z * TANK_SHELL_SPEED;

	bgun0f09ebcc(&shell->base, &pos, playerprop->rooms, &rot, &speed, &identity, playerprop, &pos);

	// An explosion's sustained damage sets off a rocket in it (objDamage()),
	// and each shell into the smoke of the last went off in it, the next one
	// sooner, walking the blasts back towards the tank (F3 20260925-230611).
	// GoldenEye hurts what is in a blast once every quarter of the blast's
	// life (explosion.c), not every frame, and its shells all but always fly
	// through: this one goes off at what it hits
	shell->base.flags2 |= OBJFLAG2_IMMUNETOEXPLOSIONS;

	// GoldenEye's shell is not a rocket: gunFireTankShell() sets only
	// PROJECTILEFLAG_LAUNCHING, never the rocket's powered flight (flag 0x20,
	// then POWERED) nor its thrust (unk10), so it flies as a thrown thing does
	// and falls under the same gravity, 0.2778 a tick squared (propobj.c,
	// PROP_PROJECTILE_GRAVITY_MODIFIER, Perfect Dark's 1 / 3.6). It had been
	// launched powered, pushed along its line and never falling, "unnaturally
	// linear" (F3 20260929-221354). A rocket that hits anything still goes off
	// there (the rocket's CDRESULT_COLLISION, powered or not).
	if (shell->base.hidden & OBJHFLAG_PROJECTILE) {
		shell->timer240 = -1;
		shell->base.projectile->flags &= ~(PROJECTILEFLAG_POWERED | PROJECTILEFLAG_00000020);
		shell->base.projectile->unk010 = 0.0f;
		shell->base.projectile->unk014 = 0.0f;
		shell->base.projectile->unk018 = 0.0f;
		shell->base.projectile->unk01c = 0.0f;
	}
}

static void tankFire(struct tankobj *tank)
{
	struct coord pos;
	struct coord dir;
	f32 yaw = tankWrap(tank->hullyaw + tank->turretyaw);
	f32 halfwidth, halflength, height, bottom;

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	dir.x = sinf(yaw) * cosf(tank->turretpitch);
	dir.y = sinf(tank->turretpitch);
	dir.z = cosf(yaw) * cosf(tank->turretpitch);

	// out of the muzzle: GoldenEye's gunFireTankShell() starts the shell at
	// the model's matrix 4, the muzzle node at the barrel's end
	// (tankMuzzle()). The hull's half length in front of its middle, at the
	// barrel's own height under the turret's, was short of the end and to
	// one side of it once the turret turned, the barrel's pivot being ahead
	// of the hull's middle (F3 20260926-204021, "tank shell slightly off
	// center and back from barrel")
	if (tankMuzzle(tank, &pos)) {
		pos.x += tank->base.prop->pos.x;
		pos.y += tank->base.prop->pos.y;
		pos.z += tank->base.prop->pos.z;
	} else {
		pos.x = tank->base.prop->pos.x + dir.x * (halflength + 60.0f);
		pos.y = tank->base.prop->pos.y + bottom + height * 0.8f + dir.y * (halflength + 60.0f);
		pos.z = tank->base.prop->pos.z + dir.z * (halflength + 60.0f);
	}

	tankLaunchShell(&pos, &dir, yaw, tank->turretpitch);

	// The tank's row in GoldenEye's weapon table (tank_stats) has Sound 0xB,
	// GUN_TANK2BIG_1; 12, GUN_TANK2BIGBIG_1, is the grenade launcher's (F3
	// 20260930-004826, "tank firing uses grenade sound")
	if (geSfxStage()) {
		geSfxPlay(11, GESFX_VOLUME);
	}

	tank->firing = TICKS(8);
}

/**
 * GoldenEye's shells on foot: gunFireTankShell() with no running tank starts
 * the shell at Bond along the sight's line, at the tank's
 * own speed. Only an ammunition cheat leaves any shells outside the seat
 * (Infinite Ammo's set_max_ammo_for_cur_player() fills AMMO_TANK as every
 * other type), and with them the All Guns list's "Tank" fires on foot as it
 * does on the cartridge (F3 20261015-024254).
 */
static void tankFireOnFoot(void)
{
	struct coord gunpos;
	struct coord dir;
	struct coord pos;

	bgunCalculatePlayerShotSpread(&gunpos, &dir, HAND_RIGHT, true);
	mtx4RotateVecInPlace(camGetProjectionMtxF(), &dir);
	guNormalize(&dir.x, &dir.y, &dir.z);

	// at the eye and a little along the line: the player's prop is at
	// his waist here, and a shell started inside him went off on him
	pos.x = g_Vars.currentplayer->cam_pos.x + dir.x * 30.0f;
	pos.y = g_Vars.currentplayer->cam_pos.y + dir.y * 30.0f;
	pos.z = g_Vars.currentplayer->cam_pos.z + dir.z * 30.0f;

	tankLaunchShell(&pos, &dir, atan2f(dir.x, dir.z), asinf(dir.y));

	if (geSfxStage()) {
		geSfxPlay(11, GESFX_VOLUME);
	}
}

void geTankFireCannon(void)
{
	if (g_NetReplaying) return; // netplay: a prediction replay moves the player and does nothing else (netpredict.c)
	const s32 p = g_Vars.currentplayernum;
	struct tankobj *tank = geTankIsDriving() ? tankDriven() : NULL;

	if (!geTankIsDriving()) {
		if (g_Vars.lvframe60 - g_Tank[p].lastshot60 >= TANK_SHELL_GAP
				&& bgunGetReservedAmmoCount(TANK_AMMOTYPE) > 0
				&& !g_Vars.currentplayer->isdead
				&& !lvIsPaused()) {
			bgunSetAmmoQuantity(TANK_AMMOTYPE, bgunGetReservedAmmoCount(TANK_AMMOTYPE) - 1);
			g_Tank[p].lastshot60 = g_Vars.lvframe60;
			tankFireOnFoot();
		}

		return;
	}

	// a shell as fast as GoldenEye's recoil and reload allow
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

/**
 * The engine under load, as bondview2.c sounds it: TRUCK_START, whose sound
 * runs on as the engine's note (GoldenEye plays TRUCK_RUN only once that
 * slot is empty, and TRUCK_START never ends), from 25000 at idle to full at
 * nine tenths of its speed or turn; and the TANK tread loop while it moves or
 * turns, from nothing to 20000 by 0.15 and full by 0.9.
 */
static void tankSounds(struct tankobj *tank)
{
	const s32 p = g_Vars.currentplayernum;
	struct prop *prop = tank->base.prop;
	f32 util = fabsf(tank->speed) / TANK_MAX_SPEED;
	const f32 turnutil = fabsf(g_Tank[p].speedtheta) / TANK_TURN_INPUT;

	if (turnutil > util) {
		util = turnutil;
	}

	if (util > 1.0f) {
		util = 1.0f;
	}

	if (!geSfxStage() || lvIsPaused() || g_Vars.in_cutscene) {
		return;
	}

	if (geSfxNum(66)) {
		psCreateIfNotDupe(prop, geSfxNum(66), PSTYPE_CHOPPERHUM1);
	}

	g_Tank[p].enginevol = util < 0.9f ? (s32)(util * 7767.0f / 0.9f + 25000.0f) : AL_VOL_FULL;
	tankSoundVolume(prop, PSTYPE_CHOPPERHUM1, g_Tank[p].enginevol, &g_Tank[p].engineuuid, &g_Tank[p].enginebase);

	if (util > 0.02f) {
		s32 vol = AL_VOL_FULL;

		if (geSfxNum(62)) {
			psCreateIfNotDupe(prop, geSfxNum(62), PSTYPE_CHOPPERHUM2);
		}

		if (util < 0.15f) {
			vol = (s32)(util * 20000.0f / 0.15f);
		} else if (util < 0.9f) {
			vol = (s32)((util - 0.15f) * 12767.0f / 0.75f + 20000.0f);
		}

		tankSoundVolume(prop, PSTYPE_CHOPPERHUM2, vol, &g_Tank[p].treaduuid, &g_Tank[p].treadbase);
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
		if (!g_NetReplaying) {
			tankFadeEngine();
		}

		return;
	}

	tank = tankDriven();

	// netplay: a prediction replay (netpredict.c) walks the tank again and
	// does nothing else - no sound, no hands, no crush; the live tick did
	// those once
	if (g_NetReplaying && (!tank || !objIsHealthy(&tank->base) || g_Vars.currentplayer->isdead)) {
		return;
	}

	if (!tank || !objIsHealthy(&tank->base)) {
		// blown up under him: the shells went with it, and his hands are
		// his own again as they are when he climbs out
		bgunSetAmmoQuantity(TANK_AMMOTYPE, 0);
		tankGiveBackHands();

		if (g_Vars.currentplayer->unk1af0) {
			psStopSound(g_Vars.currentplayer->unk1af0, PSTYPE_CHOPPERHUM1, 0xffff);
			psStopSound(g_Vars.currentplayer->unk1af0, PSTYPE_CHOPPERHUM2, 0xffff);
		}

		g_Tank[p].state = TANK_OUT;
		g_Vars.currentplayer->unk1af0 = NULL;
		return;
	}

	prop = tank->base.prop;

	if (g_Vars.currentplayer->isdead) {
		geTankPlayerDied();
		return;
	}

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);

	// The shells he was handed as he got in go in his hand - unless a level's
	// script took them again: Tomorrow Never Dies 64's City, whose motorbike
	// is the tank, has a list take item 32 for as long as he rides it, so the
	// cartridge's rider keeps his gun out and has no shells (F3
	// 20261004-151609). Two ticks, so the lists have run once with him in it
	// whichever comes first in the frame; and if they are taken after all,
	// his hands are his own again, as they are when he climbs out
	if (!g_NetReplaying && g_Tank[p].shellsin > 0 && --g_Tank[p].shellsin == 0
			&& invHasSingleWeaponExcAllGuns(WEAPON_GE_TANKSHELLS)) {
		bgunEquipWeapon2(HAND_RIGHT, WEAPON_GE_TANKSHELLS);
		bgunEquipWeapon2(HAND_LEFT, WEAPON_NONE);
	}

	if (!g_NetReplaying && !invHasSingleWeaponExcAllGuns(WEAPON_GE_TANKSHELLS)
			&& (bgunGetWeaponNum(HAND_RIGHT) == WEAPON_GE_TANKSHELLS
				|| bgunGetWeaponNum(HAND_LEFT) == WEAPON_GE_TANKSHELLS
				|| g_Vars.currentplayer->gunctrl.switchtoweaponnum == WEAPON_GE_TANKSHELLS)) {
		tankGiveBackHands();
	}

	if (g_Tank[p].state == TANK_ENTERING) {
		if (g_Tank[p].entert == 0.0f && !g_NetReplaying && geSfxStage() && geSfxNum(66)) {
			// TRUCK_START as the engine catches, at 25000 (0x61a8), from
			// the tank and as the tank's engine - tankSounds() keeps it
			// and tankStopSounds() lets it go
			psCreateIfNotDupe(prop, geSfxNum(66), PSTYPE_CHOPPERHUM1);
			g_Tank[p].enginevol = 25000;
			tankSoundVolume(prop, PSTYPE_CHOPPERHUM1, g_Tank[p].enginevol, &g_Tank[p].engineuuid, &g_Tank[p].enginebase);
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

	// the turret is where he looks, unless the barrel would swing into
	// something: bondview2.c puts the turret's angle back when
	// bondviewCallTankCollisionStatus() fails after it turned (F3
	// 20260930-235538). Here the turret is the view, so the view is held
	// with it. It is walked round in small steps, so a flick of the mouse
	// does not carry the barrel through a post, and one already in
	// something is let out of it.
	{
		const f32 oldyaw = tank->turretyaw;
		const f32 wantyaw = tankWrap(tankViewYaw() - tank->hullyaw);
		f32 diff = wantyaw - oldyaw;
		f32 gotyaw = wantyaw;

		while (diff > M_PI) diff -= M_BADTAU;
		while (diff < -M_PI) diff += M_BADTAU;

		if (diff != 0.0f && !tankBarrelBlocked(tank, tank->hullyaw, oldyaw, NULL)) {
			const s32 steps = (s32)ceilf(fabsf(diff) / TANK_TURRET_STEP);

			gotyaw = oldyaw;

			for (s32 k = 1; k <= steps; k++) {
				const f32 yaw = tankWrap(oldyaw + diff * k / steps);

				if (tankBarrelBlocked(tank, tank->hullyaw, yaw, NULL)) {
					break;
				}

				gotyaw = yaw;
			}

			if (gotyaw != wantyaw) {
				g_Vars.currentplayer->vv_theta = tankWrap(M_BADTAU - tankWrap(tank->hullyaw + gotyaw)) * 360.0f / M_BADTAU;
			}
		}

		tank->turretyaw = gotyaw;
	}
	// bondview2.c: the barrel is lifted 10 degrees over the view while the
	// shells are in hand, so a shell from the muzzle, which is under the eye,
	// comes down on what the sight is on. Without it every shell landed short
	// of the sight (F3 20261002-155025, "the tank shells shoot a little low")
	tank->turretpitch = g_Vars.currentplayer->vv_verta * M_BADTAU / 360.0f;

	if (bgunGetWeaponNum(HAND_RIGHT) == WEAPON_GE_TANKSHELLS) {
		tank->turretpitch += 0.17453294f;
	}

	if (tank->turretpitch < -0.087266468f) {
		tank->turretpitch = -0.087266468f;
	} else if (tank->turretpitch > 25.0f * M_BADTAU / 360.0f) {
		tank->turretpitch = 25.0f * M_BADTAU / 360.0f;
	}

	if (tank->firing > 0 && !g_NetReplaying) {
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

	if (g_NetReplaying) {
		return;
	}

	if (tank->speed != 0.0f || tank->turnspeed != 0.0f) {
		tankCrush(tank, halfwidth, halflength);
		tankDriveOverProps(tank);
	}

	tankSounds(tank);
}

/* ------------------------------------------------------------------------ */
/* Riding on top (netplay)                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Online, one player drives the tank and the others can climb onto its hull
 * (geTankBoard). The tank moves under the driver's walk and nothing moved the
 * players on top: they were left behind as it drove off (F3
 * 20261011-065750). A rider is carried by the hull's step and turn before its
 * own walk each tick (bmoveTick), on whichever machine simulates it: the host
 * for everybody, and a client for its own player - by the tank as that
 * client sees it, which is the host's a little in the past. So the two agree
 * where the rider stands *on the tank*, not in the world: the local-player
 * block carries the rider's place in the hull's own axes (protocol 31) and
 * the client compares and corrects in those (netpredict.c). Offline there is
 * one player and nothing of this runs.
 *
 * The rider's view keeps its heading; only where it stands turns with the
 * hull. A guard or sim never stands on the hull (only a player climbs it).
 */

/** The tank this player stands on and does not drive, or NULL. */
struct prop *geTankRidden(s32 playernum)
{
	struct player *player;
	struct prop *prop;

	if (playernum < 0 || playernum >= MAX_PLAYERS || !(player = g_Vars.players[playernum]) || !player->prop
			|| g_Tank[playernum].state != TANK_OUT || player->isdead || player->bondmovemode != MOVEMODE_WALK
			|| player->bondonground <= 0.0f || !(prop = player->tank)) {
		return NULL;
	}

	if (prop->type != PROPTYPE_OBJ || !prop->obj || prop->obj->type != OBJTYPE_TANK
			|| !prop->obj->model || !objIsHealthy(prop->obj)) {
		return NULL;
	}

	return prop;
}

/** A point given in the tank's axes (across, up, along) put in the world by a tank place and heading. */
void geTankRideToWorld(const f32 *tankpos, f32 yaw, const f32 *rel, f32 *out)
{
	const f32 s = sinf(yaw);
	const f32 c = cosf(yaw);

	out[0] = tankpos[0] + rel[0] * c + rel[2] * s;
	out[1] = tankpos[1] + rel[1];
	out[2] = tankpos[2] - rel[0] * s + rel[2] * c;
}

/** And back: a world point in the tank's axes. */
static void tankRideToRel(const f32 *tankpos, f32 yaw, const f32 *pos, f32 *rel)
{
	const f32 s = sinf(yaw);
	const f32 c = cosf(yaw);
	const f32 dx = pos[0] - tankpos[0];
	const f32 dz = pos[2] - tankpos[2];

	rel[0] = dx * c - dz * s;
	rel[1] = pos[1] - tankpos[1];
	rel[2] = dx * s + dz * c;
}

/**
 * The tank a rider stands on, its place and heading as this machine has it
 * now (pose: x y z yaw), and the rider's place in its axes. 0 for no rider.
 */
s32 geTankRideState(s32 playernum, f32 *pose, f32 *rel)
{
	struct prop *prop = geTankRidden(playernum);
	struct tankobj *tank;
	f32 pos[3];

	if (!prop) {
		return 0;
	}

	tank = (struct tankobj *)prop->obj;

	if (g_TankRide[playernum].prop == prop) {
		// the tank as its last carry left the rider on it: where the tank is
		// may have moved on since in this tick (its driver ticked after the
		// rider), and the host's and a client's orders differ
		pose[0] = g_TankRide[playernum].pos[0];
		pose[1] = g_TankRide[playernum].pos[1];
		pose[2] = g_TankRide[playernum].pos[2];
		pose[3] = g_TankRide[playernum].yaw;
	} else {
		pose[0] = prop->pos.x;
		pose[1] = prop->pos.y;
		pose[2] = prop->pos.z;
		pose[3] = tank->hullyaw;
	}

	if (rel) {
		pos[0] = g_Vars.players[playernum]->prop->pos.x;
		pos[1] = g_Vars.players[playernum]->prop->pos.y;
		pos[2] = g_Vars.players[playernum]->prop->pos.z;
		tankRideToRel(pose, pose[3], pos, rel);
	}

	return 1;
}

/**
 * bmoveTick, before the walk (and before a client's prediction notes what
 * moved its player since the last tick): the current player carried by the
 * tank it stands on, as far as the tank moved and turned since this player's
 * last tick. Online only, on ticks (not the frames drawn between them), and
 * never in a prediction replay, which repeats the carry it noted.
 */
void geTankRideCarry(void)
{
	const s32 pn = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	struct prop *prop;
	struct tankobj *tank;
	f32 pose[4];

	if (g_NetMode == NETMODE_NONE || g_NetReplaying || pn < 0 || pn >= MAX_PLAYERS) {
		return;
	}

	if (g_Vars.lvupdate240 <= 0) {
		return;
	}

	prop = geTankRidden(pn);

	if (!prop) {
		g_TankRide[pn].prop = NULL;
		return;
	}

	tank = (struct tankobj *)prop->obj;
	pose[0] = prop->pos.x;
	pose[1] = prop->pos.y;
	pose[2] = prop->pos.z;
	pose[3] = tank->hullyaw;

	if (g_TankRide[pn].prop == prop) {
		struct prop *playerprop = player->prop;
		f32 old[3];
		f32 rel[3];
		f32 now[3];
		f32 dx;
		f32 dz;

		old[0] = playerprop->pos.x;
		old[1] = playerprop->pos.y;
		old[2] = playerprop->pos.z;

		// where it stood on the tank as the tank was, put where the tank is
		tankRideToRel(g_TankRide[pn].pos, g_TankRide[pn].yaw, old, rel);
		geTankRideToWorld(pose, pose[3], rel, now);

		// height is the walk's (the hull's top over the ground under him)
		now[1] = old[1];
		dx = now[0] - old[0];
		dz = now[2] - old[2];

		if ((dx != 0.0f || dz != 0.0f) && dx * dx + dz * dz < 40.0f * 40.0f) {
			struct coord from = { old[0], old[1], old[2] };
			struct coord to = { now[0], old[1], now[2] };
			RoomNum torooms[8];

			// not through a wall: a hull swung against one scrapes him off
			if (cdExamLos08(&from, playerprop->rooms, &to, CDTYPE_BG | CDTYPE_DOORS, GEOFLAG_WALL) != CDRESULT_COLLISION) {
				func0f065e74(&from, playerprop->rooms, &to, torooms);

				if (torooms[0] != -1) {
					propDeregisterRooms(playerprop);
					playerprop->pos.x = to.x;
					playerprop->pos.z = to.z;
					roomsCopy(torooms, playerprop->rooms);
					propRegisterRooms(playerprop);
				}
			}
		}
	}

	g_TankRide[pn].prop = prop;
	g_TankRide[pn].pos[0] = pose[0];
	g_TankRide[pn].pos[1] = pose[1];
	g_TankRide[pn].pos[2] = pose[2];
	g_TankRide[pn].yaw = pose[3];
}

/**
 * A client driving the tank sees the other players as the host had them a
 * moment ago, and its own tank now: a rider drawn where the host had it
 * stood behind on the hull (or off it) by however far the tank went since.
 * The puppet's place is taken from the host's tank (hostpose: x y z yaw, the
 * tank's record at the same moment) to this machine's. 1 when it was a rider.
 */
s32 geTankRideMapPuppet(struct prop *tankprop, const f32 *hostpose, f32 *pos)
{
	struct tankobj *tank;
	struct modelrodata_bbox *hull;
	f32 rel[3];
	f32 now[4];
	f32 scale;

	if (!tankprop || tankprop->type != PROPTYPE_OBJ || !tankprop->obj || tankprop->obj->type != OBJTYPE_TANK) {
		return 0;
	}

	tank = (struct tankobj *)tankprop->obj;
	hull = tankPartBbox(tank, TANK_PART_HULLBOX);

	if (!hull) {
		return 0;
	}

	scale = tankScale(tank);
	tankRideToRel(hostpose, hostpose[3], pos, rel);

	// over the hull (a player beside it is outside: the hull is solid to him),
	// and over the tank's own middle, not under it
	if (rel[0] < hull->xmin * scale - TANK_TOUCH_SLACK || rel[0] > hull->xmax * scale + TANK_TOUCH_SLACK
			|| rel[2] < hull->zmin * scale - TANK_TOUCH_SLACK || rel[2] > hull->zmax * scale + TANK_TOUCH_SLACK) {
		return 0;
	}

	now[0] = tankprop->pos.x;
	now[1] = tankprop->pos.y;
	now[2] = tankprop->pos.z;
	now[3] = tank->hullyaw;
	geTankRideToWorld(now, now[3], rel, pos);

	return 1;
}

/**
 * --net-test-tankride T[,2] (netcooptest `tankride`, GoldenEye's
 * Runway in co-op): at host tick T the host's player is put in the tank and
 * player 1 (a client's) on the back of its hull, from where its script's
 * nudge has it climb on; from T+120 the host drives it on g_TankTestInput,
 * straight, left, right, straight, then stops at T+780 (short of the wall
 * past Runway's far end). Both machines log
 * the rider every 30 ticks from T to T+1200 ("net: tankride"). Mode 2: the
 * host's player on the back and player 1 beside it, whose own script climbs
 * in and drives (a client driving, the host riding).
 */
static void tankTestPlace(s32 pn, struct prop *tankprop, const f32 *rel)
{
	struct player *p = g_Vars.players[pn];
	struct tankobj *tank = (struct tankobj *)tankprop->obj;
	f32 halfwidth, halflength, height, bottom;
	f32 pose[4];
	f32 world[3];
	struct coord pos;
	RoomNum inrooms[21];
	RoomNum aboverooms[21];
	RoomNum bestroom;
	const f32 overground = p->prop->pos.y - p->vv_manground;

	tankSize(tank, &halfwidth, &halflength, &height, &bottom);
	pose[0] = tankprop->pos.x;
	pose[1] = tankprop->pos.y;
	pose[2] = tankprop->pos.z;
	pose[3] = tank->hullyaw;
	geTankRideToWorld(pose, pose[3], rel, world);

	pos.x = world[0];
	pos.y = tankprop->pos.y + bottom - 4.0f + overground;
	pos.z = world[2];

	bgFindRoomsByPos(&pos, inrooms, aboverooms, 20, &bestroom);

	if (inrooms[0] == -1) {
		sysLogPrintf(LOG_WARNING, "net: tankride: no room at %.0f %.0f %.0f for player %d", pos.x, pos.y, pos.z, pn);
		return;
	}

	propDeregisterRooms(p->prop);
	p->prop->pos = pos;
	inrooms[7] = -1;
	roomsCopy(inrooms, p->prop->rooms);
	propRegisterRooms(p->prop);

	// facing the tank's middle: the view looks along (-sin theta, cos theta)
	p->vv_theta = atan2f(-(tankprop->pos.x - pos.x), tankprop->pos.z - pos.z) * 360.0f / M_BADTAU;

	while (p->vv_theta < 0.0f) p->vv_theta += 360.0f;
	while (p->vv_theta >= 360.0f) p->vv_theta -= 360.0f;
}

void geTankNetTestTick(void)
{
	static s32 at = -2;
	static s32 mode = 1;
	static struct prop *tankprop = NULL;
	u32 t;

	if (at == -2) {
		const char *arg = sysArgGetString("--net-test-tankride");

		at = arg ? (s32)strtol(arg, NULL, 0) : -1;
		mode = arg && strchr(arg, ',') ? (s32)strtol(strchr(arg, ',') + 1, NULL, 0) : 1;
	}

	if (at < 0 || g_NetMode == NETMODE_NONE || g_NetTick < (u32)at || g_NetTick > (u32)at + 1200) {
		return;
	}

	t = g_NetTick - (u32)at;

	if (!tankprop || !tankprop->obj || tankprop->obj->type != OBJTYPE_TANK) {
		tankprop = g_NumTankObjs > 0 && g_TankObjs[0] ? g_TankObjs[0]->prop : NULL;

		if (!tankprop) {
			return;
		}
	}

	if (g_NetMode == NETMODE_SERVER && PLAYERCOUNT() >= 2 && g_Vars.players[0] && g_Vars.players[1]) {
		struct tankobj *tank = (struct tankobj *)tankprop->obj;
		struct modelrodata_bbox *hull = tankPartBbox(tank, TANK_PART_HULLBOX);

		if (t == 0 && hull) {
			const s32 prevplayernum = g_Vars.currentplayernum;
			const f32 back[3] = { 0.0f, 0.0f, hull->zmin * tankScale(tank) * 0.6f };
			const f32 mid[3] = { 0.0f, 0.0f, 0.0f };
			const f32 side[3] = { hull->xmax * tankScale(tank) + 70.0f, 0.0f, 0.0f };

			if (mode == 2) {
				// a client drives (its script's press and sticks), the host rides
				tankTestPlace(0, tankprop, back);
				tankTestPlace(1, tankprop, side);
				sysLogPrintf(LOG_NOTE, "net: tankride: player 1 beside the tank, the host on its back at %.0f %.0f",
						g_Vars.players[0]->prop->pos.x, g_Vars.players[0]->prop->pos.z);
			} else {
				tankTestPlace(1, tankprop, back);
				tankTestPlace(0, tankprop, mid);
				setCurrentPlayerNum(0);
				tankEnter(tankprop);
				setCurrentPlayerNum(prevplayernum);
				sysLogPrintf(LOG_NOTE, "net: tankride: the host in the tank, player 1 on its back at %.0f %.0f",
						g_Vars.players[1]->prop->pos.x, g_Vars.players[1]->prop->pos.z);
			}
		}

		if (mode != 2) {
			g_TankTestInput = t >= 120 && t < 1000;
			g_TankTestWalk = t < 780 ? 1.0f : 0.0f;
			g_TankTestTurn = t >= 280 && t < 480 ? -1.0f : t >= 480 && t < 700 ? 1.0f : 0.0f;
		}
	}

	if (t % 30 == 0) {
		// the rider: where it stands against the tank as this machine has
		// both now (a client's puppet too)
		const s32 pn = mode == 2 ? 0 : 1;
		struct player *rp = pn < PLAYERCOUNT() ? g_Vars.players[pn] : NULL;
		const s32 driver = geTankDriverOf(tankprop);
		f32 pose[4];
		f32 pos[3] = {0};
		f32 rel[3] = {0};

		pose[0] = tankprop->pos.x;
		pose[1] = tankprop->pos.y;
		pose[2] = tankprop->pos.z;
		pose[3] = ((struct tankobj *)tankprop->obj)->hullyaw;

		if (rp && rp->prop) {
			pos[0] = rp->prop->pos.x;
			pos[1] = rp->prop->pos.y;
			pos[2] = rp->prop->pos.z;
			tankRideToRel(pose, pose[3], pos, rel);
		}

		sysLogPrintf(LOG_NOTE, "net: tankride %s tick %u: rider %d %s, rel %.1f %.1f %.1f, tank %.1f %.1f yaw %.3f, driver %d; at %.0f %.0f %.0f, climbed %.0f%s",
				g_NetMode == NETMODE_SERVER ? "host" : "client", t, pn, geTankRidden(pn) ? "on" : "off", rel[0], rel[1], rel[2],
				pose[0], pose[2], pose[3], driver, pos[0], pos[1], pos[2],
				rp ? rp->bondonground : 0.f, rp && rp->tank == tankprop ? " (on its list)" : "");
	}
}

void geTankNetSave(s32 playernum, struct getanknet *out)
{
	struct tankobj *tank = NULL;
	struct player *player;

	memset(out, 0, sizeof(*out));

	if (playernum < 0 || playernum >= MAX_PLAYERS || g_Tank[playernum].state == TANK_OUT
			|| !(player = g_Vars.players[playernum])) {
		return;
	}

	if (player->unk1af0 && player->unk1af0->obj && player->unk1af0->obj->type == OBJTYPE_TANK) {
		tank = (struct tankobj *)player->unk1af0->obj;
	}

	if (!tank) {
		return;
	}

	out->state = (u8)g_Tank[playernum].state;
	out->penalty = (u8)(g_Tank[playernum].penalty < 0 ? 0 : g_Tank[playernum].penalty > 255 ? 255 : g_Tank[playernum].penalty);
	out->entert = g_Tank[playernum].entert;
	out->hullyaw = tank->hullyaw;
	out->speed = tank->speed;
	out->turnsum = g_Tank[playernum].turnsum;
	out->turretyaw = tank->turretyaw;
}

void geTankNetLoad(s32 playernum, const struct getanknet *in)
{
	struct tankobj *tank = NULL;
	struct player *player;

	if (playernum < 0 || playernum >= MAX_PLAYERS || g_Tank[playernum].state == TANK_OUT
			|| in->state == TANK_OUT || in->state > TANK_RUNNING
			|| !(player = g_Vars.players[playernum])) {
		return;
	}

	if (player->unk1af0 && player->unk1af0->obj && player->unk1af0->obj->type == OBJTYPE_TANK) {
		tank = (struct tankobj *)player->unk1af0->obj;
	}

	if (!tank || !isfinite(in->hullyaw) || !isfinite(in->speed) || !isfinite(in->turnsum)
			|| !isfinite(in->turretyaw) || !isfinite(in->entert)) {
		return;
	}

	g_Tank[playernum].state = in->state;
	g_Tank[playernum].penalty = in->penalty;
	g_Tank[playernum].entert = in->entert;
	g_Tank[playernum].turnsum = in->turnsum;
	g_Tank[playernum].speedtheta = in->turnsum * (1.0f - TANK_TURN_FILTER);
	tank->hullyaw = tankWrap(in->hullyaw);
	tank->speed = in->speed;
	tank->turretyaw = tankWrap(in->turretyaw);
}

/**
 * netplay: the host never let this machine's player in (another got there
 * first, or the press found no tank there): out again, as the host has it
 * (netpredict.c). Its place is the host's to give back.
 */
void geTankNetLeave(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;

	if (playernum < 0 || playernum >= MAX_PLAYERS || playernum >= PLAYERCOUNT()
			|| g_Tank[playernum].state == TANK_OUT || !g_Vars.players[playernum]) {
		return;
	}

	setCurrentPlayerNum(playernum);
	tankExit(1);
	setCurrentPlayerNum(prevplayernum);
}

s32 geTankDriverOf(struct prop *prop)
{
	for (s32 i = 0; i < PLAYERCOUNT() && i < MAX_PLAYERS; i++) {
		if (g_Tank[i].state != TANK_OUT && g_Vars.players[i] && g_Vars.players[i]->unk1af0 == prop) {
			return i;
		}
	}

	return -1;
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

/**
 * GoldenEye's tank model standing as a plain object - the GoldenEye Arenas
 * mod's Runway places it that way (OBJTYPE_BASIC, not OBJTYPE_TANK), so
 * nothing above ever touches it. Its switch 8 is a flash of the cannon's that
 * GoldenEye turns off every time it draws a tank; left at the model's default
 * of on, it hung lit on the barrel's end for the whole match, in both looks
 * (F3 20260928-165218, "a permanent effect of shooting"). Off, and the
 * muzzle's own flash (switch 7) stays as the model starts it: off.
 */
void geTankUpdateParkedModel(struct prop *prop)
{
	struct defaultobj *obj = prop->obj;
	struct model *model = obj->model;
	struct modelnode *flash;
	struct modelnode *off;
	const char *name;

	if (!model || !model->definition || model->definition->numparts <= TANK_PART_OFF) {
		return;
	}

	flash = modelGetPart(model->definition, TANK_PART_FLASH);
	off = modelGetPart(model->definition, TANK_PART_OFF);

	if (!flash || (flash->type & 0xff) != MODELNODETYPE_CHRGUNFIRE
			|| !off || (off->type & 0xff) != MODELNODETYPE_TOGGLE) {
		return;
	}

	if (obj->modelnum < 0 || obj->modelnum >= NUM_MODELS) {
		return;
	}

	name = romdataFileGetName(g_ModelStates[obj->modelnum].fileid);

	if (name && strcmp(name, "Pgx288Z") == 0) {
		((union modelrwdata *)modelGetNodeRwData(model, off))->toggle.visible = false;
	}
}

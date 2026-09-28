/**
 * Sitting in the Carrington Institute's chairs (Mod.SitInChairs).
 *
 * The use button on one of the Institute's office chairs (MODEL_DD_CHAIR, the
 * nineteen in setuptra.c) sits the player in it, and the use button again
 * stands him up where he was. Seated he cannot move and the chair stays put.
 *
 * Every chair is at a desk with a terminal on it, and both are in reach from
 * behind the chair: the nearer of the two is used (sitChairKeepsInteract()),
 * so he sits, and seated the terminal in front of him is the nearer - the use
 * button works it, and looking away from it and pressing use stands him up. It is the Institute's only for now.
 *
 * It is done the way GoldenEye's tank is (getank.c): he is still walking
 * (MOVEMODE_WALK) and the walk asks this file at each of its places -
 *
 *  - the input (sitChairApplyMoveData()): the sticks move nothing - he can
 *    look about, and that is all;
 *  - the move (sitChairTick(), sitChairHoldsMove()): he is carried from where
 *    he stood to the seat and back on an ease, and the walk's own move is
 *    held, so the desk the chair is pushed under cannot stop him short;
 *  - the eye (sitChairEyeHeight()): lowered to a seated man's;
 *  - and the body (sitChairAnimateBody()): stock has no animation of sitting
 *    down, only ANIM_STAND_UP_FROM_SITTING, which the guards at desks use, so
 *    sitting down is that played backwards; seated he holds
 *    ANIM_SITTING_DORMANT, the guards' idle, and turns to face where the chair
 *    faces whatever the camera does.
 *
 * The chair's perimeter is off while he is in it, and he stands up where he
 * sat down from, which was a place he could stand.
 */
#include <math.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/vars.h"
#include "lib/model.h"
#include "lib/anim.h"
#include "game/bondmove.h"
#include "game/modoptions.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "geroom.h"
#include "sitchair.h"

enum {
	SIT_OFF,
	SIT_DOWN,   // on the way into the chair
	SIT_SEATED,
	SIT_UP,     // on the way out of it
};

// ticks (60ths) to sit down or stand up: the stand up's 17 frames at half speed
#define SIT_TICKS 34.0f

// the stand up is played at this speed, and backwards at its negative
#define SIT_ANIM_SPEED 0.5f

// a chr standing this close to a chair's middle is sitting in it
#define SIT_TAKEN_DIST 50.0f

// a seated man's eye, as a share of a standing one's
#define SIT_EYE_SCALE 0.66f

// how far in front of the chair's origin he sits, along the way it faces
#define SIT_SEAT_FORWARD 0.0f

struct sitstate {
	s32 state;
	f32 t;              // 0 standing where he was, 1 in the seat
	struct prop *chair;
	struct coord standpos;
	f32 standtheta;
	s32 bodyphase;      // the state whose animation the body was last given
};

static struct sitstate g_Sit[MAX_PLAYERS];

static struct sitstate *sitCurrent(void)
{
	return &g_Sit[g_Vars.currentplayernum];
}

static f32 sitEase(f32 t)
{
	return (1.0f - cosf(t * M_BADPI)) * 0.5f;
}

static f32 sitWrap360(f32 angle)
{
	while (angle >= 360.0f) {
		angle -= 360.0f;
	}

	while (angle < 0.0f) {
		angle += 360.0f;
	}

	return angle;
}

/**
 * The player's theta (degrees, 0 looking along +z) that looks the way the
 * chair faces: its model's +z, turned by the object's rotation.
 */
static f32 sitChairTheta(struct prop *chair)
{
	struct defaultobj *obj = chair->obj;
	f32 dx = obj->realrot[2][0];
	f32 dz = obj->realrot[2][2];

	return sitWrap360(atan2f(-dx, dz) * 360.0f / M_BADTAU);
}

static void sitSeatPos(struct prop *chair, struct coord *seat)
{
	struct defaultobj *obj = chair->obj;

	seat->x = chair->pos.x + obj->realrot[2][0] * SIT_SEAT_FORWARD;
	seat->y = chair->pos.y;
	seat->z = chair->pos.z + obj->realrot[2][2] * SIT_SEAT_FORWARD;
}

static void sitMovePlayer(f32 x, f32 z)
{
	struct prop *playerprop = g_Vars.currentplayer->prop;
	struct coord pos;
	RoomNum rooms[8];

	pos.x = x;
	pos.y = playerprop->pos.y;
	pos.z = z;

	func0f065e74(&playerprop->pos, playerprop->rooms, &pos, rooms);

	playerprop->pos.x = pos.x;
	playerprop->pos.z = pos.z;

	propDeregisterRooms(playerprop);
	roomsCopy(rooms, playerprop->rooms);
	propRegisterRooms(playerprop);
}

static void sitRelease(struct sitstate *sit)
{
	if (sit->chair) {
		propSetPerimEnabled(sit->chair, true);
	}

	sit->state = SIT_OFF;
	sit->t = 0;
	sit->chair = NULL;
}

void sitChairReset(void)
{
	memset(g_Sit, 0, sizeof(g_Sit));
}

static s32 sitAllowed(void)
{
	return g_ModOptions.sitinchairs
		&& g_Vars.stagenum == STAGE_CITRAINING
		&& !geRoomActive();
}

/**
 * Whether someone is already in the chair: the Institute's staff sit at some
 * of the desks (a setup chr's `chair`), and another player may be in it.
 */
static s32 sitChairTaken(struct prop *chair)
{
	s16 propnums[MAX_ROOMPROPS];

	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Sit[i].state != SIT_OFF && g_Sit[i].chair == chair) {
			return 1;
		}
	}

	roomGetProps(chair->rooms, propnums, MAX_ROOMPROPS);

	for (s32 i = 0; propnums[i] >= 0; i++) {
		struct prop *prop = &g_Vars.props[propnums[i]];
		f32 dx;
		f32 dz;

		if (prop->type != PROPTYPE_CHR || prop->chr == NULL) {
			continue;
		}

		dx = prop->pos.x - chair->pos.x;
		dz = prop->pos.z - chair->pos.z;

		if (dx * dx + dz * dz < SIT_TAKEN_DIST * SIT_TAKEN_DIST) {
			return 1;
		}
	}

	return 0;
}

s32 sitChairIsSeat(struct defaultobj *obj)
{
	return sitAllowed()
		&& obj->modelnum == MODEL_DD_CHAIR
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& sitCurrent()->state == SIT_OFF
		&& obj->prop
		&& !sitChairTaken(obj->prop);
}

static s32 sitPropIsSeat(struct prop *prop)
{
	return prop->type == PROPTYPE_OBJ && prop->obj && prop->obj->modelnum == MODEL_DD_CHAIR && sitAllowed();
}

s32 sitChairKeepsInteract(struct prop *current, struct prop *candidate)
{
	struct coord *pos = &g_Vars.currentplayer->prop->pos;
	f32 dcur;
	f32 dnew;

	if (current == NULL || (!sitPropIsSeat(current) && !sitPropIsSeat(candidate))) {
		return 0;
	}

	dcur = (current->pos.x - pos->x) * (current->pos.x - pos->x) + (current->pos.z - pos->z) * (current->pos.z - pos->z);
	dnew = (candidate->pos.x - pos->x) * (candidate->pos.x - pos->x) + (candidate->pos.z - pos->z) * (candidate->pos.z - pos->z);

	return dcur <= dnew;
}

s32 sitChairInteract(struct prop *prop)
{
	struct sitstate *sit = sitCurrent();

	if (prop->type != PROPTYPE_OBJ || !sitChairIsSeat(prop->obj)) {
		return 0;
	}

	sit->state = SIT_DOWN;
	sit->t = 0;
	sit->chair = prop;
	sit->standpos = g_Vars.currentplayer->prop->pos;
	sit->standtheta = g_Vars.currentplayer->vv_theta;
	sit->bodyphase = SIT_OFF;

	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;
	g_Vars.currentplayer->speedmaxtime60 = 0;

	// he is in it: it is no obstacle to him
	propSetPerimEnabled(prop, false);

	return 1;
}

s32 sitChairActivate(void)
{
	struct sitstate *sit = sitCurrent();

	if (sit->state == SIT_OFF
			|| !(g_Vars.currentplayer->bondactivateorreload & JO_ACTION_ACTIVATE)) {
		return 0;
	}

	if (sit->state == SIT_SEATED) {
		// seated at a desk the use button is the terminal's: the level's
		// own interaction takes the press (lvRender(), after this tick).
		// Looking away from it and pressing use stands him up
		if (propFindForInteract(false)) {
			return 0;
		}

		sit->state = SIT_UP;
	}

	// spent on the chair, never a jump or a door
	g_Vars.currentplayer->bondactivateorreload = 0;

	return 1;
}

s32 sitChairApplyMoveData(struct movedata *data)
{
	if (sitCurrent()->state == SIT_OFF) {
		return 0;
	}

	// seated he cannot move, and the chair goes nowhere with him: only the
	// use button stands him up (sitChairActivate())
	g_Vars.currentplayer->speedforwards = 0;
	g_Vars.currentplayer->speedsideways = 0;
	g_Vars.currentplayer->speedmaxtime60 = 0;

	return 1;
}

void sitChairTick(void)
{
	struct sitstate *sit = sitCurrent();
	struct player *player = g_Vars.currentplayer;
	struct coord seat;
	f32 e;
	f32 turn;
	s32 turning;

	if (sit->state == SIT_OFF) {
		return;
	}

	// anything that is not walking - a cutscene, a death, the chair gone -
	// lets go of the chair where he is
	if (!sitAllowed()
			|| player->bondmovemode != MOVEMODE_WALK
			|| player->isdead
			|| sit->chair == NULL
			|| sit->chair->obj == NULL
			|| (sit->chair->obj->hidden2 & OBJH2FLAG_DESTROYED)) {
		sitRelease(sit);
		return;
	}

	// turned on the tick that finishes sitting down too, or a long frame
	// that sits him in one step leaves him facing the way he came
	turning = sit->state == SIT_DOWN;

	if (sit->state == SIT_DOWN) {
		sit->t += g_Vars.lvupdate60freal / SIT_TICKS;

		if (sit->t >= 1.0f) {
			sit->t = 1.0f;
			sit->state = SIT_SEATED;
		}
	} else if (sit->state == SIT_UP) {
		sit->t -= g_Vars.lvupdate60freal / SIT_TICKS;

		if (sit->t <= 0.0f) {
			sitMovePlayer(sit->standpos.x, sit->standpos.z);
			sitRelease(sit);
			return;
		}
	}

	// across to the seat, turning to face the way it does
	sitSeatPos(sit->chair, &seat);
	e = sitEase(sit->t);

	sitMovePlayer(sit->standpos.x + (seat.x - sit->standpos.x) * e,
			sit->standpos.z + (seat.z - sit->standpos.z) * e);

	if (turning) {
		turn = sitChairTheta(sit->chair) - sit->standtheta;

		if (turn > 180.0f) {
			turn -= 360.0f;
		} else if (turn < -180.0f) {
			turn += 360.0f;
		}

		player->vv_theta = sitWrap360(sit->standtheta + turn * e);
	}

	player->speedforwards = 0;
	player->speedsideways = 0;
#ifndef PLATFORM_N64
	player->rollspeed.f[0] = 0;
	player->rollspeed.f[2] = 0;
#endif
}

s32 sitChairHoldsMove(void)
{
	return sitCurrent()->state != SIT_OFF;
}

f32 sitChairEyeHeight(f32 eyeheight)
{
	struct sitstate *sit = sitCurrent();
	f32 target;

	if (sit->state == SIT_OFF) {
		return eyeheight;
	}

	target = g_Vars.currentplayer->vv_eyeheight * SIT_EYE_SCALE;

	return eyeheight + (target - eyeheight) * sitEase(sit->t);
}

s32 sitChairBodyFacing(struct player *player, f32 *facing)
{
	struct sitstate *sit = &g_Sit[playermgrGetPlayerNumByProp(player->prop)];

	if (sit->state == SIT_OFF || sit->chair == NULL) {
		return 0;
	}

	*facing = (360.0f - sitChairTheta(sit->chair)) * M_BADTAU / 360.0f;

	return 1;
}

s32 sitChairAnimateBody(struct chrdata *chr, f32 *angleoffset)
{
	struct sitstate *sit;

	if (chr->prop->type != PROPTYPE_PLAYER) {
		return 0;
	}

	sit = &g_Sit[playermgrGetPlayerNumByProp(chr->prop)];

	if (sit->state == SIT_OFF) {
		return 0;
	}

	*angleoffset = 0;
	chr->hidden2 |= CHRH2FLAG_AUTOANIM;

	if (sit->bodyphase == sit->state) {
		return 1;
	}

	if (sit->state == SIT_DOWN) {
		// the stand up, backwards from its last frame
		modelSetAnimation(chr->model, ANIM_STAND_UP_FROM_SITTING, false,
				animGetNumFrames(ANIM_STAND_UP_FROM_SITTING) - 1, -SIT_ANIM_SPEED, 16);
	} else if (sit->state == SIT_SEATED) {
		modelSetAnimation(chr->model, ANIM_SITTING_DORMANT, false, 0, 0.5f, 16);
		modelSetAnimLooping(chr->model, 0, 16);
	} else if (sit->state == SIT_UP) {
		modelSetAnimation(chr->model, ANIM_STAND_UP_FROM_SITTING, false, 0, SIT_ANIM_SPEED, 16);
	}

	sit->bodyphase = sit->state;

	return 1;
}

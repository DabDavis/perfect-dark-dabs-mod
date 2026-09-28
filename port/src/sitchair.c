/**
 * Sitting in the Carrington Institute's chairs (Mod.SitInChairs).
 *
 * The use button on one of the Institute's office chairs or sofas
 * (g_SeatModels) sits the player in it - the free seat nearest him on a sofa - and the use button again
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

// a chr standing this close to a seat is sitting in it
#define SIT_TAKEN_DIST 45.0f

// a seated man's eye, as a share of a standing one's
#define SIT_EYE_SCALE 0.66f

/**
 * What the Institute has to sit on (a census of its props: 19 office chairs
 * at desks, 16 two-seater sofas). Each seat is `forward` in front of the model's origin,
 * the way it faces (its +z, towards the desk or away from the sofa's back),
 * and a sofa's are `spacing` apart along its length (its x), world units.
 * `lift` raises the body over the floor: the seated animation was made for
 * the office chair, and the sofa's cushion is higher.
 *
 * The seated animation rests the left forearm on an armrest. A sofa's seats
 * are at its ends, against its armrests, and on seat 0 - the -x end, whose
 * armrest is on the sitter's right - the pose is played mirrored
 * (`mirrorfirst`) so the arm raised is the one over the armrest.
 */
struct seatmodel {
	s32 modelnum;
	f32 forward;
	s32 numseats;
	f32 spacing;
	f32 lift;
	s32 mirrorfirst;
};

static struct seatmodel g_SeatModels[] = {
	{ MODEL_DD_CHAIR, 0.0f,  1, 0.0f,   0.0f,  false },
	{ MODEL_CI_SOFA,  24.0f, 2, 160.0f, 12.0f, true },
};

struct sitstate {
	s32 state;
	f32 t;              // 0 standing where he was, 1 in the seat
	struct prop *chair;
	s32 seat;           // which of a sofa's seats
	struct coord standpos;
	f32 standtheta;
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

static const struct seatmodel *sitSeatModel(struct prop *prop)
{
	if (prop == NULL || prop->type != PROPTYPE_OBJ || prop->obj == NULL) {
		return NULL;
	}

	for (s32 i = 0; i < ARRAYCOUNT(g_SeatModels); i++) {
		if (g_SeatModels[i].modelnum == prop->obj->modelnum) {
			return &g_SeatModels[i];
		}
	}

	return NULL;
}

/**
 * Seat `index` of a chair or sofa. realrot carries the model's scale, so its
 * axes are made unit length first.
 */
static void sitSeatPos(struct prop *chair, s32 index, struct coord *seat)
{
	const struct seatmodel *def = sitSeatModel(chair);
	struct defaultobj *obj = chair->obj;
	f32 xx = obj->realrot[0][0];
	f32 xz = obj->realrot[0][2];
	f32 zx = obj->realrot[2][0];
	f32 zz = obj->realrot[2][2];
	f32 len;
	f32 along = 0;
	f32 forward = 0;

	len = sqrtf(xx * xx + xz * xz);

	if (len > 0) {
		xx /= len;
		xz /= len;
	}

	len = sqrtf(zx * zx + zz * zz);

	if (len > 0) {
		zx /= len;
		zz /= len;
	}

	if (def) {
		along = (index - (def->numseats - 1) * 0.5f) * def->spacing;
		forward = def->forward;
	}

	seat->x = chair->pos.x + xx * along + zx * forward;
	seat->y = chair->pos.y;
	seat->z = chair->pos.z + xz * along + zz * forward;
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
 * Whether someone is already in a seat: the Institute's staff sit at some of
 * the desks (a setup chr's `chair`), and another player may be in it.
 */
static s32 sitSeatTaken(struct prop *chair, s32 index)
{
	s16 propnums[MAX_ROOMPROPS];
	struct coord seat;

	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Sit[i].state != SIT_OFF && g_Sit[i].chair == chair && g_Sit[i].seat == index) {
			return 1;
		}
	}

	sitSeatPos(chair, index, &seat);
	roomGetProps(chair->rooms, propnums, MAX_ROOMPROPS);

	for (s32 i = 0; propnums[i] >= 0; i++) {
		struct prop *prop = &g_Vars.props[propnums[i]];
		f32 dx;
		f32 dz;

		if (prop->type != PROPTYPE_CHR || prop->chr == NULL) {
			continue;
		}

		dx = prop->pos.x - seat.x;
		dz = prop->pos.z - seat.z;

		if (dx * dx + dz * dz < SIT_TAKEN_DIST * SIT_TAKEN_DIST) {
			return 1;
		}
	}

	return 0;
}

/**
 * The free seat of a chair or sofa nearest the current player, or -1.
 */
static s32 sitSeatMirrored(struct sitstate *sit)
{
	const struct seatmodel *def = sit->chair ? sitSeatModel(sit->chair) : NULL;

	return def && def->mirrorfirst && sit->seat == 0;
}

static s32 sitFindSeat(struct prop *chair)
{
	const struct seatmodel *def = sitSeatModel(chair);
	struct coord *pos = &g_Vars.currentplayer->prop->pos;
	s32 best = -1;
	f32 bestdist = 0;

	if (def == NULL) {
		return -1;
	}

	for (s32 i = 0; i < def->numseats; i++) {
		struct coord seat;
		f32 dist;

		if (sitSeatTaken(chair, i)) {
			continue;
		}

		sitSeatPos(chair, i, &seat);
		dist = (seat.x - pos->x) * (seat.x - pos->x) + (seat.z - pos->z) * (seat.z - pos->z);

		if (best < 0 || dist < bestdist) {
			best = i;
			bestdist = dist;
		}
	}

	return best;
}

s32 sitChairIsSeat(struct defaultobj *obj)
{
	return sitAllowed()
		&& obj->prop
		&& sitSeatModel(obj->prop)
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& sitCurrent()->state == SIT_OFF
		&& sitFindSeat(obj->prop) >= 0;
}

static s32 sitPropIsSeat(struct prop *prop)
{
	return sitAllowed() && sitSeatModel(prop);
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

	// chosen before he is in it, or his own state takes the seat he wants
	sit->seat = sitFindSeat(prop);
	sit->state = SIT_DOWN;
	sit->t = 0;
	sit->chair = prop;
	sit->standpos = g_Vars.currentplayer->prop->pos;
	sit->standtheta = g_Vars.currentplayer->vv_theta;

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
	// that sits him in one step leaves him facing the way he came. Only in
	// first person: in third person the view is left where he was looking,
	// at the seat, so the camera stays out in front and he watches himself
	// sit down. Turned round with him, the camera went behind the seat - into
	// the wall a sofa stands against - came in onto the eye, and the body
	// faded out: "it auto transitions to first person"
	turning = sit->state == SIT_DOWN && !playerIsThirdPerson(player);

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
	sitSeatPos(sit->chair, sit->seat, &seat);
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

	if (sit->chair && sitSeatModel(sit->chair)) {
		target += sitSeatModel(sit->chair)->lift;
	}

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

	// asked every tick rather than once a phase: the body is only built
	// while it is drawn, so a player who sat down in first person and then
	// switched to third person had a body that missed the phase's start and
	// stood up in the middle of the sofa
	{
		const s32 animnum = modelGetAnimNum(chr->model);
		const f32 speed = modelGetAnimSpeed(chr->model);
		const f32 last = animGetNumFrames(ANIM_STAND_UP_FROM_SITTING) - 1;
		const s32 flip = sitSeatMirrored(sit);
		const s32 flipped = modelIsFlipped(chr->model) != 0;

		if (sit->state == SIT_DOWN) {
			// the stand up backwards, from as far into it as the move is
			if (animnum != ANIM_STAND_UP_FROM_SITTING || speed >= 0 || flipped != flip) {
				modelSetAnimation(chr->model, ANIM_STAND_UP_FROM_SITTING, flip,
						last * (1.0f - sit->t), -SIT_ANIM_SPEED, 16);
			}
		} else if (sit->state == SIT_SEATED) {
			if (animnum != ANIM_SITTING_DORMANT || flipped != flip) {
				modelSetAnimation(chr->model, ANIM_SITTING_DORMANT, flip, 0, 0.5f, 16);
				modelSetAnimLooping(chr->model, 0, 16);
			}
		} else if (sit->state == SIT_UP) {
			if (animnum != ANIM_STAND_UP_FROM_SITTING || speed <= 0 || flipped != flip) {
				modelSetAnimation(chr->model, ANIM_STAND_UP_FROM_SITTING, flip,
						last * (1.0f - sit->t), SIT_ANIM_SPEED, 16);
			}
		}
	}

	return 1;
}

f32 sitChairBodyLift(struct prop *playerprop)
{
	struct sitstate *sit = &g_Sit[playermgrGetPlayerNumByProp(playerprop)];
	const struct seatmodel *def;

	if (sit->state == SIT_OFF || sit->chair == NULL) {
		return 0;
	}

	def = sitSeatModel(sit->chair);

	return def ? def->lift * sitEase(sit->t) : 0;
}

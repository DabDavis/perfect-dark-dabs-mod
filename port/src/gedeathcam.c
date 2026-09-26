/**
 * GoldenEye's death replay.
 *
 * Perfect Dark kept all of GoldenEye's death but its last part. The first
 * person fall, the wash of blood (geblood.c), the red that follows it and the
 * fade to black are all still in player.c; what it dropped is what GoldenEye
 * does once the screen is black in a one player game (bondview2.c,
 * CAMERAMODE_DEATH_CAM_SP):
 *
 *  - Bond's body is put back where he died and his death animation - the same
 *    one the first person fall played, flipped the same way - starts again
 *    from its first frame on the body, at half speed (0.5 to the fall's 0.25);
 *  - the picture fades in from black over a second, seen from a camera placed
 *    at random round the body (pickDeathCameraAngles(), below) and turned each
 *    tick to a smoothed copy of where Bond's head is (field_3C4, an average
 *    that keeps nine tenths of itself every tick);
 *  - when the body's animation reaches its end the picture fades to black over
 *    a second (CAMERAMODE_DEATH_CAM_MP), and the replay starts again from
 *    another camera: three times in all (`camera_mode` 0, 1, 2), after which
 *    the mission is over;
 *  - A, B, Z or START cuts it short: the fade to black, and then the end.
 *
 * The first replay of a mission also cuts the death tune for the intro's
 * swoosh (musicTrack1Play(M_INTROSWOOSH), sequence 44, from the conversion's
 * music). GoldenEye halves the sound effects under it too, which is not done.
 *
 * Measured against the decomp's native port (2026-09-26: an attract demo on
 * Dam, bondviewKillCurrentPlayer() called from gdb and ramromFadeToTitle()
 * held off, every HUD frame traced and every third frame dumped): the blood,
 * the red, the fall's own 60 tick fade to black; then three replays, each a
 * 60 tick fade in, the body's 85 frames at 0.5 (170 ticks) counted from the
 * start of the fade, and a 60 tick fade out; the three cameras 227, 282 and
 * 182 units from what they looked at, the look climbing from the fallen eye
 * to the standing head and down with the body.
 *
 * Only on GE Plus's converted missions, played alone, as GoldenEye does it
 * (getPlayerCount() == 1). No Combat Simulator match has it - GE Plus's arenas
 * included, the user's call (2026-09-26); co-operative and counter-operative
 * keep Perfect Dark's. Skip Death Screen has no fall to replay (it only acts
 * in the Combat Simulator anyway), and the Randomizer and Mission Respawn
 * their own ends. The multiplayer half (Press START held back, a press as the
 * respawn) is still here, unreached, should the arenas ever be let in. A death in
 * GoldenEye's tank is not replayed (GoldenEye blows the tank up and watches
 * that instead).
 *
 * The camera search is pickDeathCameraAngles(): along each of sixteen
 * headings from a random start, how far the tile graph lets a line from where
 * Bond died go (up to 1500 units past his clearance, stopping at a wall or the
 * brink of a drop - gestan.c's geStanLineReach()), a distance between 200 and
 * that picked at random, nearer on each of four tries, room round the spot
 * for a body his width, and a height of his clearance plus a random part of
 * the rest of 185 over the floor there. A level with no tile graph has
 * Perfect Dark's collision asked instead. Three tests are added, all about
 * seeing the body: Perfect Dark's line of sight, the tile graph's floor staying
 * under the line (a converted level's ground is the graph's before it is
 * Perfect Dark's collision), and no prop's bounding box in the way - Jungle's
 * bushes have no collision at all, and the first replay there watched a wall
 * of leaves. GoldenEye tests none of them and does put the camera behind a
 * fern.
 */
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/collision.h"
#include "lib/joy.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "lib/rng.h"
#include "game/camera.h"
#include "game/chr.h"
#include "game/lv.h"
#include "game/modoptions.h"
#include "game/modrespawn.h"
#include "game/modrun.h"
#include "game/music.h"
#include "game/options.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/mplayer/mplayer.h"
#include "modloader.h"
#include "gemusic.h"
#include "getank.h"
#include "gestan.h"
#include "geroom.h"
#include "gedeathcam.h"
#include "system.h"

#ifndef PLATFORM_N64

#define DEATHCAM_REPLAYS      3       // camera_mode 0, 1 and 2
#define DEATHCAM_FADE60       60.0f   // in and out, both
#define DEATHCAM_SPEED        0.5f    // the body's; the fall was 0.25
#define DEATHCAM_NOBODY60     180.0f  // how long a replay with no body lasts
#define DEATHCAM_LONGEST60    900.0f  // a body whose animation never ends
#define DEATHCAM_DIST         200.0f  // the nearest the camera may be
#define DEATHCAM_REACH        1500.0f // and the furthest, past the clearance
#define DEATHCAM_HEIGHT       185.0f  // over the floor, clearance included
#define DEATHCAM_DROP         1000.0f // nor more than this above or below the head
#define DEATHCAM_SMOOTH       0.9f    // of the head's average kept each tick
#define DEATHCAM_SWOOSH       44      // M_INTROSWOOSH

#define DEATHCAM_SKIP_BUTTONS (A_BUTTON | B_BUTTON | Z_TRIG | START_BUTTON)

enum {
	DEATHCAM_NONE,     // dying, and the fall not yet faded out
	DEATHCAM_REPLAY,   // CAMERAMODE_DEATH_CAM_SP
	DEATHCAM_FADEOUT,  // CAMERAMODE_DEATH_CAM_MP
	DEATHCAM_DONE,     // over, or never going to run
};

struct deathcam {
	s32 state;
	s32 stagenum;          // the death this is: the stage, and
	s32 lifestarttime60;   // the mission time it happened at
	s32 startframe;
	s32 replays;           // camera_mode
	s32 posed;             // the body has been given its animation
	s32 skipped;           // a press cut it short
	s32 respawn;           // ... in multiplayer, and the respawn is owed
	s16 anim;
	s8 flip;
	f32 timer60;
	struct coord campos;
	RoomNum camrooms[8];
	struct coord look;     // field_3C4
	struct coord head;     // the body's head as its last tick posed it
	s32 headframe;         // the frame that was, 0 for none
};

static struct deathcam g_DeathCam[MAX_PLAYERS];

static struct deathcam *deathcamGet(void)
{
	struct deathcam *dc = &g_DeathCam[g_Vars.currentplayernum];
	struct player *pl = g_Vars.currentplayer;

	// A death of its own, not one left over from the last stage or life
	if (dc->stagenum != g_Vars.stagenum
			|| dc->lifestarttime60 != pl->lifestarttime60
			|| dc->startframe > g_Vars.lvframenum) {
		memset(dc, 0, sizeof(*dc));
		dc->state = DEATHCAM_NONE;
		dc->stagenum = g_Vars.stagenum;
		dc->lifestarttime60 = pl->lifestarttime60;
		dc->startframe = g_Vars.lvframenum;
	}

	return dc;
}

/**
 * One of GE Plus's converted GoldenEye missions, and nothing else: the user's
 * call (2026-09-26) - no Combat Simulator gets the replay, GE Plus's arenas
 * included, since GoldenEye's own multiplayer never had it.
 */
static s32 deathcamStageIsGoldenEye(void)
{
	return modloaderStageIsMission(g_Vars.stagenum) && !g_Vars.normmplayerisrunning;
}

/** Whether this death is one GoldenEye would replay. */
static s32 deathcamEligible(void)
{
	struct player *pl = g_Vars.currentplayer;

	if (!pl || !pl->prop || !pl->isdead || PLAYERCOUNT() != 1 || !deathcamStageIsGoldenEye()) {
		return 0;
	}

	if (modIsSkipDeathScreenOn()) {
		return 0;
	}

	if (!g_Vars.mplayerisrunning && (modRunIsOn() || modRespawnCanRespawn())) {
		return 0;
	}

	if (g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) {
		return 0;
	}

	return 1;
}

static f32 deathcamDistXZ(const struct coord *a, const struct coord *b)
{
	const f32 dx = a->x - b->x;
	const f32 dz = a->z - b->z;

	return sqrtf(dx * dx + dz * dz);
}

#define DEATHCAM_LOS_FLAGS (GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT | GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2 | GEOFLAG_LIFTFLOOR)

/**
 * How far the level lets a camera go from `from` towards `end` in plan: the
 * tile graph's walk where the level has one (GoldenEye's own test, which stops
 * at a wall and at the brink of a drop alike), else a line at the eye against
 * the walls. -1 when neither can say.
 */
static f32 deathcamReach(struct coord *from, RoomNum *fromrooms, struct coord *end, s32 stan)
{
	if (stan) {
		f32 hitx, hitz, ground;
		s32 room;
		const s32 r = geStanLineReach(from, end->x, end->z, &hitx, &hitz, &room, &ground);

		if (r >= 0) {
			const f32 dx = hitx - from->x, dz = hitz - from->z;

			return r ? deathcamDistXZ(end, from) : sqrtf(dx * dx + dz * dz);
		}
	}

	if (cdExamLos08(from, fromrooms, end, CDTYPE_BG | CDTYPE_CLOSEDDOORS, DEATHCAM_LOS_FLAGS) == CDRESULT_COLLISION) {
		struct coord hit;

		cdGetPos(&hit, __LINE__, "gedeathcam.c");

		return deathcamDistXZ(&hit, from);
	}

	return deathcamDistXZ(end, from);
}

/**
 * Whether the camera may stand at `cam` (x/z), reached in a straight line from
 * `from`: its floor and room into `ground` and `camrooms`. On the tile graph,
 * stanTestLineUnobstructed() to it and stanTestVolume() round it (the second
 * as eight short walks out to the clearance); elsewhere the walls, a volume
 * test and the ground under it.
 */
static s32 deathcamStand(struct coord *from, RoomNum *fromrooms, struct coord *cam, f32 clearance, s32 stan,
		f32 *ground, RoomNum *camrooms)
{
	RoomNum crossed[21];

	if (stan) {
		f32 hitx, hitz;
		s32 room;
		s32 r = geStanLineReach(from, cam->x, cam->z, &hitx, &hitz, &room, ground);

		if (r == 1) {
			struct coord at = { cam->x, *ground + 1.0f, cam->z };

			for (s32 k = 0; k < 8; k++) {
				const f32 a = k * (M_BADTAU / 8.0f);
				f32 g;
				s32 rr;

				if (geStanLineReach(&at, cam->x + sinf(a) * clearance, cam->z + cosf(a) * clearance,
							&hitx, &hitz, &rr, &g) != 1) {
					return 0;
				}
			}

			if (room <= 0 || room >= g_Vars.roomcount) {
				return 0;
			}

			camrooms[0] = room;
			camrooms[1] = -1;

			return 1;
		}

		if (r == 0) {
			return 0;
		}
	}

	if (cdExamLos08(from, fromrooms, cam, CDTYPE_BG | CDTYPE_CLOSEDDOORS, DEATHCAM_LOS_FLAGS) == CDRESULT_COLLISION) {
		return 0;
	}

	camrooms[0] = -1;
	func0f065dfc(from, fromrooms, cam, camrooms, crossed, 20);

	if (camrooms[0] < 0) {
		return 0;
	}

	if (!cdTestVolume(cam, clearance, camrooms, CDTYPE_BG | CDTYPE_CLOSEDDOORS | CDTYPE_OBJS, CHECKVERTICAL_NO, 0, 0)) {
		return 0;
	}

	*ground = cdFindGroundAtCyl(cam, clearance, camrooms, NULL, NULL);

	return *ground > -30000.0f;
}

/**
 * Whether a prop the camera would look through stands between it and the
 * body: Jungle's bushes and ferns are props with no collision, so no line of
 * sight test sees them, and a camera placed behind one watched a wall of
 * leaves. Each object in the rooms along the line is taken as a ball round
 * the middle of its bounding box, a little smaller than the box.
 */
static s32 deathcamPropsBlock(struct coord *cam, RoomNum *camrooms, struct coord *target)
{
	struct player *pl = g_Vars.currentplayer;
	RoomNum rooms[32];
	RoomNum dummy[8];
	RoomNum crossed[21];
	s16 propnums[MAX_ROOMPROPS];
	s32 n = 0;
	const f32 dx = target->x - cam->x, dy = target->y - cam->y, dz = target->z - cam->z;
	const f32 lensq = dx * dx + dy * dy + dz * dz;

	if (lensq < 1.0f) {
		return 0;
	}

	for (s32 i = 0; camrooms[i] >= 0 && i < 8 && n < 31; i++) {
		rooms[n++] = camrooms[i];
	}

	for (s32 i = 0; pl->prop->rooms[i] >= 0 && i < 8 && n < 31; i++) {
		rooms[n++] = pl->prop->rooms[i];
	}

	crossed[0] = -1;
	dummy[0] = -1;
	func0f065dfc(cam, camrooms, target, dummy, crossed, 20);

	for (s32 i = 0; crossed[i] >= 0 && i < 20 && n < 31; i++) {
		rooms[n++] = crossed[i];
	}

	rooms[n] = -1;
	roomGetProps(rooms, propnums, MAX_ROOMPROPS);

	for (s16 *ptr = propnums; *ptr >= 0; ptr++) {
		struct prop *prop = &g_Vars.props[*ptr];
		struct defaultobj *obj;
		struct modelrodata_bbox *bbox;
		struct coord mid;
		f32 scale, radius, t, ex, ey, ez;

		if (prop->type != PROPTYPE_OBJ || !prop->obj || !prop->obj->model) {
			continue;
		}

		obj = prop->obj;

		if (obj->hidden & OBJHFLAG_DELETING || !(prop->flags & PROPFLAG_ENABLED)) {
			continue;
		}

		bbox = objFindBboxRodata(obj);

		if (!bbox) {
			continue;
		}

		scale = obj->model->scale;
		radius = (bbox->xmax - bbox->xmin > bbox->zmax - bbox->zmin ? bbox->xmax - bbox->xmin : bbox->zmax - bbox->zmin)
			* 0.5f * 0.7f * scale;

		if (radius < 15.0f) {
			continue;
		}

		mid.x = prop->pos.x;
		mid.y = prop->pos.y + (bbox->ymin + bbox->ymax) * 0.5f * scale;
		mid.z = prop->pos.z;

		// the point of the line nearest the ball's middle
		t = ((mid.x - cam->x) * dx + (mid.y - cam->y) * dy + (mid.z - cam->z) * dz) / lensq;

		// what stands at the body is beside it, not in front of it
		if (t > 0.85f) {
			continue;
		}

		if (t < 0.0f) {
			t = 0.0f;
		}

		ex = cam->x + dx * t - mid.x;
		ey = cam->y + dy * t - mid.y;
		ez = cam->z + dz * t - mid.z;

		// its height the box's own, rather than the ball's
		if (ex * ex + ez * ez < radius * radius
				&& ey > (bbox->ymin - bbox->ymax) * 0.5f * scale
				&& ey < (bbox->ymax - bbox->ymin) * 0.5f * scale) {
			return 1;
		}
	}

	return 0;
}

/**
 * pickDeathCameraAngles(): somewhere round `centre` to watch from, reached
 * from `from` (where Bond died, at his eye). Headings are tried sixteen at a
 * time, a sixteenth of a turn apart from a random start, 129 times over.
 */
static s32 deathcamPick(struct deathcam *dc, const struct coord *centre, struct coord *from, RoomNum *fromrooms)
{
	struct player *pl = g_Vars.currentplayer;
	f32 clearance = pl->bond2.radius;
	const s32 stan = geRoomActive();
	s32 outer;

	if (clearance < 1.0f) {
		clearance = 30.0f;
	}

	for (outer = 0; outer <= 0x80; outer++) {
		f32 angle = RANDOMFRAC() * M_BADTAU;
		s32 tries;

		for (tries = 0; tries < 16; tries++) {
			struct coord dir;
			struct coord end;
			f32 reach;
			f32 frac;

			angle += 0.39269909f;

			if (angle >= M_BADTAU) {
				angle -= M_BADTAU;
			}

			dir.x = sinf(angle);
			dir.y = 0.0f;
			dir.z = cosf(angle);

			end.x = centre->x + dir.x * (DEATHCAM_REACH + clearance);
			end.y = from->y;
			end.z = centre->z + dir.z * (DEATHCAM_REACH + clearance);

			reach = deathcamReach(from, fromrooms, &end, stan) - clearance;

			if (reach < DEATHCAM_DIST) {
				continue;
			}

			for (frac = 1.0f; frac > 0.0f; frac -= 0.25f) {
				struct coord cam;
				RoomNum camrooms[8];
				RoomNum crossed[21];
				f32 dist = DEATHCAM_DIST + RANDOMFRAC() * (reach - DEATHCAM_DIST) * frac;
				f32 ground;

				cam.x = centre->x + dir.x * dist;
				cam.y = from->y;
				cam.z = centre->z + dir.z * dist;

				if (!deathcamStand(from, fromrooms, &cam, clearance, stan, &ground, camrooms)) {
					continue;
				}

				cam.y = ground + clearance + RANDOMFRAC() * (DEATHCAM_HEIGHT - clearance);

				if (cam.y - centre->y <= -DEATHCAM_DROP || cam.y - centre->y >= DEATHCAM_DROP) {
					continue;
				}

				// Perfect Dark's rooms for the height it came to, unless the
				// tile graph named one; and the body in view of it
				if (!stan) {
					camrooms[0] = -1;
					func0f065dfc(from, fromrooms, &cam, camrooms, crossed, 20);

					if (camrooms[0] < 0) {
						continue;
					}
				}

				if (cdExamLos08(&cam, camrooms, (struct coord *)centre, CDTYPE_BG | CDTYPE_CLOSEDDOORS | CDTYPE_OBJS,
							DEATHCAM_LOS_FLAGS) == CDRESULT_COLLISION) {
					continue;
				}

				// and over the ground between, the standing head and the fallen
				if (stan) {
					struct coord standing = *from;
					struct coord fallen = *centre;

					fallen.y += 5.0f;

					if (!geStanSightClear(&cam, &standing) || !geStanSightClear(&cam, &fallen)) {
						continue;
					}
				}

				if (deathcamPropsBlock(&cam, camrooms, from) || deathcamPropsBlock(&cam, camrooms, (struct coord *)centre)) {
					continue;
				}

				dc->campos = cam;
				memcpy(dc->camrooms, camrooms, sizeof(dc->camrooms));

				return 1;
			}
		}
	}

	return 0;
}

/**
 * Where the body's head is: GoldenEye's field_488.pos in the replay. Read in
 * the body's own tick, just after chrTick() has posed it for this frame's
 * camera. Its matrices are floats only until the frame is drawn, which turns
 * them into the RSP's fixed point where they lie (and by the next tick their
 * memory is the next frame's).
 */
static s32 deathcamHeadPos(struct coord *out)
{
	struct player *pl = g_Vars.currentplayer;
	struct model *model = pl->model00d4;
	struct modelnode *node;
	Mtxf *mtx;
	Mtxf world;

	if (!pl->haschrbody || !model || !model->matrices || !pl->prop->chr
			|| !(pl->prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
		return 0;
	}

	node = modelGetPart(model->definition, MODELPART_CHR_HEADSPOT);

	if (!node) {
		return 0;
	}

	mtx = modelFindNodeMtx(model, node, 0);

	if (!mtx) {
		return 0;
	}

	mtx00015be4(camGetProjectionMtxF(), mtx, &world);

	out->x = world.m[3][0];
	out->y = world.m[3][1];
	out->z = world.m[3][2];

	// a body built this tick has matrices nothing has written yet
	if (!isfinite(out->x) || !isfinite(out->y) || !isfinite(out->z)
			|| deathcamDistXZ(out, &pl->prop->pos) > 500.0f
			|| out->y - pl->prop->pos.y > 500.0f || out->y - pl->prop->pos.y < -500.0f) {
		return 0;
	}

	return 1;
}

/** bondviewSetCameraMode(CAMERAMODE_DEATH_CAM_SP): one replay begins, or false when there is nowhere to watch from. */
static s32 deathcamBegin(struct deathcam *dc)
{
	struct player *pl = g_Vars.currentplayer;
	struct coord from = pl->posdie;
	RoomNum fromrooms[8];

	fromrooms[0] = pl->prop->rooms[0];
	fromrooms[1] = -1;

	if (dc->replays == 0) {
		// the fall's own animation, which the body plays again
		dc->anim = modelGetAnimNum(&pl->model);
		dc->flip = pl->model.anim ? pl->model.anim->flip : 0;
		dc->look = pl->bond2.unk10;
	}

	if (fromrooms[0] < 0 || !deathcamPick(dc, &dc->look, &from, fromrooms)) {
		sysLogPrintf(LOG_NOTE, "gedeathcam: nowhere to watch replay %d from", dc->replays);
		return 0;
	}

	dc->state = DEATHCAM_REPLAY;
	dc->timer60 = 0;
	dc->posed = 0;

	playerSetFadeColour(0, 0, 0, 1);
	playerSetFadeFrac(DEATHCAM_FADE60, 0);

	if (dc->replays == 0 && !g_Vars.mplayerisrunning) {
		const s32 swoosh = geMusicSequence(DEATHCAM_SWOOSH);

		if (swoosh >= 0) {
			musicQueueStopEvent(TRACKTYPE_PRIMARY);
			musicQueueStartEvent(TRACKTYPE_PRIMARY, swoosh, 0, musicGetVolume());
		}
	}

	sysLogPrintf(LOG_NOTE, "gedeathcam: replay %d from (%.0f %.0f %.0f), %.0f from the head",
			dc->replays, dc->campos.x, dc->campos.y, dc->campos.z,
			sqrtf((dc->campos.x - dc->look.x) * (dc->campos.x - dc->look.x)
				+ (dc->campos.y - dc->look.y) * (dc->campos.y - dc->look.y)
				+ (dc->campos.z - dc->look.z) * (dc->campos.z - dc->look.z)));

	return 1;
}

/** The body, standing where he died, falls again (bondviewSetCameraMode()). */
static void deathcamPose(struct deathcam *dc)
{
	struct player *pl = g_Vars.currentplayer;
	struct chrdata *chr = pl->prop->chr;

	if (dc->posed || !pl->haschrbody || !pl->model00d4 || !chr || !pl->model00d4->anim) {
		return;
	}

	dc->posed = 1;

	chr->chrflags &= ~CHRCFLAG_HIDDEN;
	playerStartChrFade(0, 1);

	if (dc->anim > 0) {
		modelSetAnimation(pl->model00d4, dc->anim, dc->flip, 0, DEATHCAM_SPEED, 0);
		// playerChooseThirdPersonAnimation() keeps a death it was given
		chr->deathanim = dc->anim;
	}
}

static s32 deathcamPressed(void)
{
	const s8 contpad = optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex);

	if (lvIsPaused() || (g_Vars.mplayerisrunning && mpIsPaused())) {
		return 0;
	}

	return joyGetButtonsPressedThisFrame(contpad, DEATHCAM_SKIP_BUTTONS) != 0;
}

/** bondviewSetCameraMode(CAMERAMODE_DEATH_CAM_MP): the fade to black that ends a replay. */
static void deathcamFadeOut(struct deathcam *dc)
{
	dc->state = DEATHCAM_FADEOUT;
	playerSetFadeColour(0, 0, 0, 0);
	playerSetFadeFrac(DEATHCAM_FADE60, 1);
}

static void deathcamFinish(struct deathcam *dc)
{
	dc->state = DEATHCAM_DONE;

	if (dc->skipped && g_Vars.mplayerisrunning) {
		dc->respawn = 1;
	}

	// the black the last fade left, held for whatever comes next
	playerSetFadeColour(0, 0, 0, 1);
}

/** One tick of the replay, from the camera below. */
static void deathcamTick(struct deathcam *dc)
{
	struct player *pl = g_Vars.currentplayer;
	struct coord head;

	if (dc->state == DEATHCAM_NONE) {
		if (!pl->redbloodfinished || !pl->deathanimfinished || !playerIsFadeComplete()) {
			return;
		}

		if (!deathcamEligible() || geTankPlayerDriving(pl) || !deathcamBegin(dc)) {
			deathcamFinish(dc);
			return;
		}
	}

	if (dc->state == DEATHCAM_DONE) {
		return;
	}

	deathcamPose(dc);

	// field_3C4: the head's position, averaged
	if (dc->headframe > 0 && dc->headframe >= g_Vars.lvframenum - 2) {
		head = dc->head;

		for (s32 i = 0; i < g_Vars.lvupdate60; i++) {
			dc->look.x = dc->look.x * DEATHCAM_SMOOTH + head.x * (1.0f - DEATHCAM_SMOOTH);
			dc->look.y = dc->look.y * DEATHCAM_SMOOTH + head.y * (1.0f - DEATHCAM_SMOOTH);
			dc->look.z = dc->look.z * DEATHCAM_SMOOTH + head.z * (1.0f - DEATHCAM_SMOOTH);
		}
	}

	if (deathcamPressed()) {
		dc->skipped = 1;
	}

	if (dc->state == DEATHCAM_REPLAY) {
		struct model *model = pl->model00d4;
		s32 over;

		dc->timer60 += g_Vars.lvupdate60freal;

		if (model && dc->posed && dc->anim > 0) {
			over = modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)
				|| dc->timer60 >= DEATHCAM_LONGEST60;
		} else {
			over = dc->timer60 >= DEATHCAM_NOBODY60;
		}

		if (over || dc->skipped) {
			deathcamFadeOut(dc);
		}
	} else if (dc->state == DEATHCAM_FADEOUT) {
		if (playerIsFadeComplete()) {
			dc->replays++;

			if (dc->skipped || dc->replays >= DEATHCAM_REPLAYS || !deathcamBegin(dc)) {
				deathcamFinish(dc);
			}
		}
	}
}

void geDeathCamSeeHead(void)
{
	struct deathcam *dc;
	struct coord head;

	if (!deathcamEligible()) {
		return;
	}

	dc = deathcamGet();

	if ((dc->state == DEATHCAM_REPLAY || dc->state == DEATHCAM_FADEOUT) && dc->posed && deathcamHeadPos(&head)) {
		dc->head = head;
		dc->headframe = g_Vars.lvframenum;
	}
}

s32 geDeathCamIsGoldenEye(void)
{
	return g_Vars.currentplayer && g_Vars.currentplayer->isdead && deathcamStageIsGoldenEye();
}

s32 geDeathCamHolds(void)
{
	struct deathcam *dc;

	if (!deathcamEligible()) {
		return 0;
	}

	dc = deathcamGet();

	return dc->state != DEATHCAM_DONE;
}

s32 geDeathCamTakeRespawn(void)
{
	struct deathcam *dc;

	if (!g_Vars.currentplayer || !g_Vars.currentplayer->isdead) {
		return 0;
	}

	dc = deathcamGet();

	if (dc->respawn) {
		dc->respawn = 0;
		return 1;
	}

	return 0;
}

s32 geDeathCamWantsBody(struct player *player)
{
	s32 playernum;
	struct deathcam *dc;

	if (!player || !player->isdead || !player->prop) {
		return 0;
	}

	playernum = playermgrGetPlayerNumByProp(player->prop);

	if (playernum < 0 || playernum >= MAX_PLAYERS) {
		return 0;
	}

	dc = &g_DeathCam[playernum];

	return dc->stagenum == g_Vars.stagenum
		&& dc->lifestarttime60 == player->lifestarttime60
		&& (dc->state == DEATHCAM_REPLAY || dc->state == DEATHCAM_FADEOUT);
}

s32 geDeathCamCamera(struct coord *pos, struct coord *up, struct coord *look,
		struct coord *hintpos, RoomNum *hintrooms)
{
	struct deathcam *dc;
	f32 len;

	if (!deathcamEligible()) {
		return 0;
	}

	dc = deathcamGet();
	deathcamTick(dc);

	if (dc->state != DEATHCAM_REPLAY && dc->state != DEATHCAM_FADEOUT) {
		// black by now, and the game's own death camera is as good as any
		return 0;
	}

	look->x = dc->look.x - dc->campos.x;
	look->y = dc->look.y - dc->campos.y;
	look->z = dc->look.z - dc->campos.z;

	len = sqrtf(look->x * look->x + look->y * look->y + look->z * look->z);

	if (len < 1.0f) {
		look->x = 0;
		look->y = 0;
		look->z = 1;
	} else {
		look->x /= len;
		look->y /= len;
		look->z /= len;
	}

	*pos = dc->campos;
	up->x = 0;
	up->y = 1;
	up->z = 0;

	*hintpos = dc->campos;
	memcpy(hintrooms, dc->camrooms, sizeof(dc->camrooms));

	return 1;
}

#endif

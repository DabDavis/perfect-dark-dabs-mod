/**
 * GE Plus's Cinema: GoldenEye's own opening camera shots, watched on their
 * levels.
 *
 * Every GoldenEye mission carries one to six INTROTYPE_CAMERA records - where
 * the camera stands, the yaw and pitch it looks along, the pad whose room it is
 * in, and the one or two lines it shows. GoldenEye picks **one at random** each
 * time a mission starts and flies nothing: the camera sits still, the first
 * line appears two seconds in, the second at five, and at eight (or five with
 * one line, or on a button press) it swirls down into Bond's eyes
 * (bondview2.c). The Cinema page plays a mission's shots one after another
 * instead, so all of them can be seen, and then leaves.
 *
 * What runs the camera here is the game's own cutscene movement mode: the walk
 * is what would otherwise pull the player to the floor and collide them,
 * bcutsceneTick() is empty on purpose, and the placement is modSpectateTick()'s
 * - a move that resolves its rooms through func0f065e74() rather than testing
 * collision, since the camera is somewhere no player can stand.
 *
 * The conversion has already put the records in the level's own frame
 * (geconvert.c, gesolo.py's intro_camera()); nothing in Perfect Dark reads
 * them, so their fields are the port's to define.
 */
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/vars.h"
#include "lib/main.h"
#include "lib/joy.h"
#include "lib/mtx.h"
#include "game/bondcutscene.h"
#include "game/bondgun.h"
#include "game/bondmove.h"
#include "game/chr.h"
#include "game/hudmsg.h"
#include "game/propobj.h"
#include "game/lang.h"
#include "game/lv.h"
#include "game/options.h"
#include "game/player.h"
#include "game/prop.h"
#include "game/pad.h"
#include "game/setup.h"
#include "game/chrai.h"
#include "game/chraction.h"
#include "game/env.h"
#include "lib/model.h"
#include "lib/rng.h"
#include "system.h"
#include "gexplus.h"
#include "modloader.h"
#include "input.h"
#include "gexfront.h"
#include "gecinema.h"

#ifndef PLATFORM_N64

// GoldenEye's own, in 60ths of a second (bondview2.c): the first line at two
// seconds, the second at five, and the shot over at eight - or at five when it
// has only one line.
#define SHOT_LINE1   120.0f
#define SHOT_LINE2   300.0f
#define SHOT_END_1   300.0f
#define SHOT_END_2   480.0f

#define MAX_SHOTS 16

// The folder's own keys (gexfront.c), because a cinema is a page of the folder
// to whoever is watching it: what backs out of a page there leaves here, and
// what picks there goes on to the next shot. It used to read the N64's buttons
// and nothing else, so on a keyboard Escape and Enter did nothing at all, the
// right mouse button - R as well as Cancel - went on to the next shot instead
// of leaving, and the one key that left was E. "You cannot leave the videos."
#define LEAVE_BUTTONS (B_BUTTON | BUTTON_UI_CANCEL)
#define SKIP_BUTTONS  (A_BUTTON | Z_TRIG | START_BUTTON | R_TRIG | L_TRIG | BUTTON_UI_ACCEPT)

// The mission the folder picked, waiting for its stage to load
static s32 g_GeCinemaArmed = -1;
static s32 g_GeCinemaArmedWhat;
// which of the mission's two it is: GECINEMA_OPENING or GECINEMA_ENDING
static s32 g_GeCinemaWhat;
// the ending: 0 at the load, 1 once the screen is black, 2 once its list has been started
static s32 g_GeEndingKicked;
// and once it has: the mission being watched, or -1
static s32 g_GeCinemaMission = -1;
// set when the last shot is over, so the folder opens again on the Cinema page
static s32 g_GeCinemaWantFolder;

static const u8 *g_GeCinemaShots[MAX_SHOTS];
static s32 g_GeCinemaNumShots = -1;   // -1 until the stage's records are read
static s32 g_GeCinemaShot;
static f32 g_GeCinemaTime60;
static f32 g_GeCinemaTotal60;         // since the cinema began, not the shot
static s32 g_GeCinemaLine;            // how many of the shot's lines have shown
static s32 g_GeCinemaEntered;         // the one-off setup has run
static s32 g_GeCinemaLeft;            // the player backed out rather than watching to the end

// where the shot's camera stands, and the room its own pad names: the player's
// prop stays where the mission spawned it and only this moves
// (gecinemaCameraTick)
static struct coord g_GeCinemaCamPos;
static s32 g_GeCinemaCamRoom = -1;

/**
 * A mission's own opening, which is the same shots hooked up to the mission.
 *
 * GoldenEye opens every solo mission the same way (bondview2.c,
 * bondviewSetCameraMode() and bondviewFrozenCameraTick()):
 *
 *   CAMERAMODE_INTRO      one of the level's camera shots, picked at random,
 *                         faded in from black, with its one or two lines
 *   CAMERAMODE_FADESWIRL  a second's fade to black
 *   CAMERAMODE_SWIRL      Bond's body loaded where he spawned, playing the
 *                         animation the setup names, and the camera flown down
 *                         a spline of the setup's INTROTYPE_SWIRL points into
 *                         his eyes, the body fading out over the last half
 *                         second
 *   CAMERAMODE_FP         the gun comes up and the player has control
 *
 * and any of the six buttons cuts a stage short. The level runs underneath the
 * whole time, which is why its lists ask IFCameraIsInIntro.
 *
 * The still is the Cinema page's own shot. The swirl is Perfect Dark's
 * TICKMODE_WARP, which is what GoldenEye's frozen camera became there: it
 * builds the player's chr body and hands the camera to whoever wants it
 * (gecinemaSwirlTick(), from playerTick()).
 */
#define GEINTRO_NONE  0
#define GEINTRO_STILL 1
#define GEINTRO_FADE  2
#define GEINTRO_SWIRL 3
#define GEINTRO_HOLD  4   // the Cinema page's swirl is over and the stage is changing

#define MAX_SWIRL 32

// the setup's INTROTYPE_SWIRL record as the conversion writes it (geconvert.c):
// GoldenEye's own fields, the five fixed point ones already floats
struct geswirl {
	u32 flags;          // 1 the end of the path, 2 the offset turns with Bond, 4 look where he looks
	struct coord off;   // from Bond's eyes
	f32 scale;          // the spline's tangent scale
	f32 duration;       // of the leg that starts here, in 60ths
	s32 pad;            // whose room the camera is in, or -1 for Bond's
};

// stage_intro_anim_table[] (bondview.c): GoldenEye's animation id, the frame it
// starts on, the frame it ends on (or to its end) and its speed
static const struct { s16 geanim; f32 start; f32 end; f32 speed; } g_GeIntroAnimTable[9] = {
	{ 61,  95.0f, -1.0f, 0.02f },  // extending_left_hand
	{ 66,   7.0f, 40.0f, 0.5f  },  // fire_standing_draw_one_handed_weapon_fast
	{ 97,   0.0f, -1.0f, 0.5f  },  // draw_one_handed_weapon_and_look_around
	{ 98,   0.0f, -1.0f, 0.5f  },  // draw_one_handed_weapon_and_stand_up
	{ 99,   0.0f, -1.0f, 0.5f  },  // aim_one_handed_weapon_left_right
	{ 100,  0.0f, -1.0f, 0.5f  },  // cock_one_handed_weapon_and_turn_around
	{ 102,  0.0f, -1.0f, 0.5f  },  // cock_one_handed_weapon_turn_around_and_stand_up
	{ 103,  0.0f, -1.0f, 0.5f  },  // draw_one_handed_weapon_and_turn_around
	{ 176,  0.0f, -1.0f, 0.5f  },  // bond_eye_fire_alt
};

static s32 g_GeIntroPending;          // this stage is a mission that has not opened yet
static s32 g_GeIntroStage;
static const u8 *g_GeIntroShot;
static f32 g_GeIntroTimer;            // GoldenEye's camera_transition_timer
static s32 g_GeIntroLeg;              // and its intro_camera_index
static s32 g_GeIntroFadingOut;        // and its camera_fade_active
static s32 g_GeIntroPosed;            // the body has been given its animation
static s32 g_GeIntroAnimIndex;
static f32 g_GeIntroTheta;            // where Bond was looking before the still borrowed his angles
static f32 g_GeIntroVerta;
static struct geswirl g_GeSwirl[MAX_SWIRL + 4];
static s32 g_GeNumSwirl;

/**
 * The folder's Cinema page picked a mission. The stage starts the way a mission
 * does; gecinemaStageStart() picks this up when it has loaded.
 */
void gecinemaArm(s32 mission, s32 what)
{
	g_GeCinemaArmed = mission;
	g_GeCinemaArmedWhat = what;
}

/** Every stage load: this one is a cinema if the folder armed one. */
void gecinemaStageStart(void)
{
	g_GeCinemaMission = g_GeCinemaArmed;
	g_GeCinemaWhat = g_GeCinemaArmedWhat;
	g_GeCinemaArmed = -1;
	g_GeEndingKicked = 0;

	// for a probe that boots straight into a mission: play it as the Cinema
	// page would (build/gexrom/runall_cinema.sh)
	if (g_GeCinemaMission < 0 && modloaderStageIsMission(g_Vars.stagenum)) {
		if (sysArgCheck("--cinema-ending")) {
			g_GeCinemaMission = 0;
			g_GeCinemaWhat = GECINEMA_ENDING;
		} else if (sysArgCheck("--cinema-opening")) {
			g_GeCinemaMission = 0;
			g_GeCinemaWhat = GECINEMA_OPENING;
		}
	}
	g_GeCinemaNumShots = -1;
	g_GeCinemaShot = 0;
	g_GeCinemaTime60 = 0;
	g_GeCinemaTotal60 = 0;
	g_GeCinemaLine = 0;
	g_GeCinemaEntered = 0;
	g_GeCinemaLeft = 0;
	g_GeCinemaCamRoom = -1;

	// A mission that is not the Cinema page's opens on its own cinema. The
	// probes that boot straight into a level can ask for it not to.
	g_GeIntroStage = GEINTRO_NONE;
	g_GeIntroPending = g_GeCinemaMission < 0
		&& modloaderStageIsMission(g_Vars.stagenum)
		&& !sysArgCheck("--skip-mission-intro");
}

s32 gecinemaIsOn(void)
{
	return g_GeCinemaMission >= 0;
}

/** A mission's own opening is playing: the still, the fade or the swirl. */
s32 gecinemaIntroIsOn(void)
{
	return g_GeIntroStage != GEINTRO_NONE;
}

/** GoldenEye's IFCameraIsInIntro: the still and the fade out of it. */
s32 gecinemaIntroIsStill(void)
{
	return g_GeIntroPending || g_GeIntroStage == GEINTRO_STILL || g_GeIntroStage == GEINTRO_FADE;
}

/** And its IFCameraIsInBondSwirl. */
s32 gecinemaIntroIsSwirl(void)
{
	return g_GeIntroStage == GEINTRO_SWIRL;
}

s32 gecinemaWantsFolder(void)
{
	return g_GeCinemaWantFolder;
}

s32 gecinemaTakeFolderMission(void)
{
	const s32 mission = g_GeCinemaWantFolder ? g_GeCinemaMission : -1;

	g_GeCinemaWantFolder = 0;
	g_GeCinemaMission = -1;

	return mission;
}

/** The stage's camera records, in the order the setup lists them. */
static void gecinemaCollect(void)
{
	// the intro commands' lengths in bytes, by type (playerreset.c)
	static const u8 lens[9] = { 12, 16, 16, 32, 8, 8, 40, 12, 8 };
	const u8 *cmd = (const u8 *)g_StageSetup.intro;

	g_GeCinemaNumShots = 0;
	g_GeNumSwirl = 0;
	g_GeIntroAnimIndex = 0;

	while (cmd) {
		const u32 type = *(const u32 *)cmd;

		if (type >= sizeof(lens) || !lens[type]) {
			break;   // INTROCMD_END, or a record whose length is not known
		}

		if (type == 6 && g_GeCinemaNumShots < MAX_SHOTS) {
			g_GeCinemaShots[g_GeCinemaNumShots++] = cmd;
		}

		if (type == 3 && g_GeNumSwirl < MAX_SWIRL) {
			struct geswirl *sw = &g_GeSwirl[g_GeNumSwirl++];

			sw->flags = *(const u32 *)(cmd + 0x04);
			sw->off.x = *(const f32 *)(cmd + 0x08);
			sw->off.y = *(const f32 *)(cmd + 0x0c);
			sw->off.z = *(const f32 *)(cmd + 0x10);
			sw->scale = *(const f32 *)(cmd + 0x14);
			sw->duration = *(const f32 *)(cmd + 0x18);
			sw->pad = *(const s32 *)(cmd + 0x1c);
		}

		if (type == 4) {
			g_GeIntroAnimIndex = *(const s32 *)(cmd + 0x04);
		}

		cmd += lens[type];
	}

	// GoldenEye reads three records past the leg it is on looking for the end
	// of the path (bondviewFrozenCameraTick()); its own lists always end on a
	// flag 1 record, and these are what a list that did not would run into
	for (s32 i = 0; i < 4; i++) {
		struct geswirl *sw = &g_GeSwirl[g_GeNumSwirl + i];

		memset(sw, 0, sizeof(*sw));
		sw->flags = 1;
		sw->pad = -1;
	}
}

/** The player is the camera: no walk, no gun, no HUD and nothing to hit them. */
static void gecinemaEnter(void)
{
	g_GeCinemaEntered = 1;

	// a shot borrows the player's angles to aim with (gecinemaPlace), and the
	// swirl wants Bond facing the way the level spawned him
	g_GeIntroTheta = g_Vars.currentplayer->vv_theta;
	g_GeIntroVerta = g_Vars.currentplayer->vv_verta;

	bcutsceneInit();
	bgunSetSightVisible(GUNSIGHTREASON_NOCONTROL, false);
	bgunSetGunAmmoVisible(GUNAMMOREASON_NOCONTROL, false);
	hudmsgsSetOff(HUDMSGREASON_NOCONTROL);
	countdownTimerSetVisible(COUNTDOWNTIMERREASON_NOCONTROL, false);
	bgunEquipWeapon(WEAPON_NONE);

	g_PlayersWithControl[g_Vars.currentplayernum] = false;
	g_PlayerInvincible = true;
	g_Vars.bondvisible = false;
}

/**
 * The camera at one shot - and the player's prop left where it stands.
 *
 * GoldenEye moves the camera alone (`bondviewSetCameraMode(CAMERAMODE_POSEND)`)
 * and Bond stays at his spawn, where the level's guards go on answering
 * questions about him. Perfect Dark's camera *is* the eye by construction, so
 * this used to move the player's prop to the shot - and a guard's list, which
 * asks about the prop and not about the picture, found Bond standing at the
 * camera and came for it.
 *
 * `g_Vars.bondvisible` is already false for a shot and covers everything that
 * asks whether a guard can *see* him - `botIsTargetInvisible()`, and
 * `chrSetPadPresetToPadOnRouteToTarget()` tests it too - but a converted list
 * asks plenty that is not about seeing: `IFBondInRoomWithPad` is 264 commands
 * over the twenty missions and `IFMyDistanceToBondLessThan` another fifty, and
 * those read `prop->pos` and `prop->rooms` wherever they are.
 *
 * So the shot's position and room are kept here and the camera alone is put
 * there, after the normal tick has built its own from the eye
 * (gecinemaCameraTick). The angles still go through the player's own basis,
 * which is inert in a cutscene, because that is the game's own trigonometry.
 */
static void gecinemaPlace(const u8 *shot)
{
	struct player *pl = g_Vars.currentplayer;
	struct pad pad;

	g_GeCinemaCamPos.x = *(const f32 *)(shot + 0x04);
	g_GeCinemaCamPos.y = *(const f32 *)(shot + 0x08);
	g_GeCinemaCamPos.z = *(const f32 *)(shot + 0x0c);

	// The record names the pad the camera stands on and GoldenEye takes the
	// room from that pad's stan tile, which is the answer for a camera hanging
	// over a valley or inside a wall. A portal walk would be from the player's
	// prop, which is across the level at the spawn, and answers the room they
	// are in - which draws nothing.
	padUnpack(*(const u32 *)(shot + 0x18), PADFIELD_ROOM, &pad);

	if (pad.room > 0 && pad.room < g_Vars.roomcount) {
		g_GeCinemaCamRoom = pad.room;
	}

	// GoldenEye's look vector is (cos(pitch)sin(yaw), sin(pitch),
	// -cos(pitch)cos(yaw)) and Perfect Dark's horizontal one is
	// (-sin(theta), 0, cos(theta)), so theta is GoldenEye's yaw turned half
	// round. Both pitches are positive upwards.
	pl->vv_theta = 180.0f + *(const f32 *)(shot + 0x10) * (180.0f / 3.14159265f);
	pl->vv_verta = *(const f32 *)(shot + 0x14) * (180.0f / 3.14159265f);

	while (pl->vv_theta >= 360.0f) {
		pl->vv_theta -= 360.0f;
	}

	while (pl->vv_verta > 180.0f) {
		pl->vv_verta -= 360.0f;
	}

	bmoveUpdateVerta();
	bmove0f0cc654(0, 0, 0);
}

/**
 * The camera, put where the shot stands, after playerTick() has built its own.
 *
 * The normal tick places the camera at the eye and resolves its room by a
 * portal walk from the prop (`player0f0c1840()`), so it has to run first and be
 * replaced: `playerSetCamPropertiesWithRoom()` takes the room as it is given,
 * which is the record's own pad room. `cam_pos` and `cam_room` are what the
 * picture is drawn from - `g_CamRoom` seeds the room walk in bg.c and the
 * portal side tests are against `cam_pos` - so nothing else has to move.
 */
void gecinemaCameraTick(void)
{
	struct player *pl = g_Vars.currentplayer;

	// the Cinema page's shots, and the one a mission opens on
	if ((!gecinemaIsOn() && g_GeIntroStage != GEINTRO_STILL && g_GeIntroStage != GEINTRO_FADE)
			|| g_GeCinemaCamRoom < 0 || !pl || !pl->prop) {
		return;
	}

	playerSetCamPropertiesWithRoom(&g_GeCinemaCamPos, &pl->bond2.unk28,
			&pl->bond2.unk1c, g_GeCinemaCamRoom);
}

/** A shot's line of text, as GoldenEye shows it: the bottom of the screen. */
static void gecinemaShowLine(const u8 *shot, s32 line)
{
	const u32 textid = *(const u32 *)(shot + (line == 0 ? 0x1c : 0x20));

	if (textid) {
		char *text = langGet(textid);

		if (text && text[0]) {
			hudmsgCreate(text, HUDMSGTYPE_DEFAULT);
		}
	}
}

/**
 * The cinema is over: back to the folder, the way a match goes back
 * (menutick.c) - to the Institute, with its own arrival skipped and the Perfect
 * Menu put under the folder. It used to go to the title instead, and the title
 * is not a backdrop: it runs on under the folder, reads the same presses, and
 * left alone for twenty seconds loads its attract demo, which resets the model
 * pool the folder's own model is an instance in.
 *
 * The stage does not change until the end of the frame, so the flag is also
 * what keeps this from asking again on every frame until it does.
 */
static void gecinemaFinish(void)
{
	if (!g_GeCinemaWantFolder) {
		g_GeCinemaWantFolder = 1;
		sysLogPrintf(LOG_NOTE, "gecinema: over at frame %d, back to the folder", g_Vars.lvframenum);
		gexFrontGoBack();
	}
}

/** Backing out of a cinema: what backs out of a page of the folder. */
static s32 gecinemaLeavePressed(void)
{
	const s8 contpad = optionsGetContpadNum1(g_Vars.currentplayerstats
			? g_Vars.currentplayerstats->mpindex : 0);
	const u32 ui = contpad == 0 ? ~0u : ~(u32)(BUTTON_UI_CANCEL | BUTTON_UI_ACCEPT);

	return joyGetButtonsPressedThisFrame(contpad, LEAVE_BUTTONS & ui) != 0
		|| inputKeyJustPressed(VK_ESCAPE);
}

/**
 * A mission's ending, played for the Cinema page.
 *
 * Every GoldenEye mission ends on the same run of commands in one of its lists
 * - HideAllChrs, then TriggerFadeAndExitLevelOnButtonPress, then the camera
 * switch and the list Bond himself is handed, which is the show (all twenty,
 * build/gexrom/endsurvey.py) - and what comes before it is the mission's own
 * business: reach the exit, have the objectives, wait out a timer. So the
 * ending is found by that pair, which the conversion writes as
 * aiShowCutsceneChrs(0) and the port's own aiGeExitOnButtonPress, and a
 * background chr is started on it there. The level's own owner of the list
 * where it has one - its lists from 0x1000 are background chrs' - and the first
 * background chr otherwise: Statue Park's and the Cradle's endings are lists a
 * chr is handed, and nothing after the pair asks anything of the chr running
 * it.
 *
 * It ends the way the mission does - the button press and its fade, or the
 * list's own EndLevel - except that both come back to the folder
 * (gecinemaEndingOver()).
 */
static void gecinemaKickEnding(void)
{
	struct ailist *lists = g_StageSetup.ailists;

	g_GeEndingKicked = 2;

	for (s32 i = 0; lists && lists[i].list; i++) {
		u8 *cmd = lists[i].list;
		s32 steps = 0;

		while (steps++ < 100000) {
			const s32 type = (cmd[0] << 8) | cmd[1];
			const s32 len = chraiGetCommandLength(cmd, 0);

			if (type == AICMD_END) {
				break;
			}

			if (type == 0x01d5 && cmd[2] == 0 && ((cmd[len] << 8) | cmd[len + 1]) == 0x01e1) {
				struct chrdata *runner = NULL;

				for (s32 k = 0; k < g_NumBgChrs; k++) {
					if (g_BgChrs[k].ailist == lists[i].list) {
						runner = &g_BgChrs[k];
						break;
					}
				}

				if (!runner && g_NumBgChrs > 0) {
					runner = &g_BgChrs[0];
				}

				if (!runner) {
					break;
				}

				sysLogPrintf(LOG_NOTE, "gecinema: the ending is list %d at +%d, run by background chr %d",
						lists[i].id, (s32)(cmd - lists[i].list), runner->chrnum);

				runner->ailist = lists[i].list;
				runner->aioffset = cmd - lists[i].list;
				runner->aireturnlist = -1;
				runner->sleep = 0;
				return;
			}

			cmd += len;
		}
	}

	sysLogPrintf(LOG_WARNING, "gecinema: no ending found in this mission's lists");
	gecinemaFinish();
}

s32 gecinemaEndingOver(void)
{
	if (!gecinemaIsOn()) {
		return 0;
	}

	gecinemaFinish();
	return 1;
}

static void gecinemaEndingTick(void)
{
	if (g_GeEndingKicked == 0) {
		// the lists fade in from black themselves once their camera is up, and
		// what is on the screen until then is a level nobody is playing
		g_GeEndingKicked = 1;
		lvConfigureFade(0x000000ff, 1);
	}

	g_GeCinemaTotal60 += g_Vars.diffframe60f;

	// a level's chrs and lists settle over its first frames (its guards are
	// made, its background chrs number themselves)
	if (g_GeEndingKicked == 1 && g_Vars.lvframenum >= 20) {
		gecinemaKickEnding();
	}

	if (g_GeCinemaTotal60 > 30.0f && gecinemaLeavePressed()) {
		gecinemaFinish();
	}
}

/** Whether a button GoldenEye's cinemas end on went down this frame. */
static s32 gecinemaPressed(void)
{
	const s8 contpad = optionsGetContpadNum1(g_Vars.currentplayerstats
			? g_Vars.currentplayerstats->mpindex : 0);
	const u32 ui = contpad == 0 ? ~0u : ~(u32)(BUTTON_UI_CANCEL | BUTTON_UI_ACCEPT);

	return joyGetButtonsPressedThisFrame(contpad, (LEAVE_BUTTONS | SKIP_BUTTONS) & ui) != 0
		|| inputKeyJustPressed(VK_ESCAPE)
		|| inputKeyJustPressed(VK_MOUSE_LEFT);
}

/** The opening is over: GoldenEye's CAMERAMODE_FP. */
static void gecinemaIntroEnd(void)
{
	struct player *pl = g_Vars.currentplayer;

	if (gecinemaIsOn()) {
		// the Cinema page's opening ends where a mission's hands over: the
		// camera holds where it is for the frame the stage takes to change
		g_GeIntroStage = GEINTRO_HOLD;
		gecinemaFinish();
		return;
	}

	g_GeIntroStage = GEINTRO_NONE;
	g_GeCinemaCamRoom = -1;

	bgunSetSightVisible(GUNSIGHTREASON_NOCONTROL, true);
	bgunSetGunAmmoVisible(GUNAMMOREASON_NOCONTROL, true);
	hudmsgsSetOn(HUDMSGREASON_NOCONTROL);
	countdownTimerSetVisible(COUNTDOWNTIMERREASON_NOCONTROL, true);

	g_PlayersWithControl[g_Vars.currentplayernum] = true;
	g_PlayerInvincible = false;
	g_Vars.bondvisible = true;

	if (pl->prop->chr) {
		pl->prop->chr->actiontype = ACT_STAND;
	}

	// the walk back, the level's own fog and the guns the mission starts with:
	// the way Perfect Dark itself leaves a mission's fade in
	player0f0b9a20();

	if (g_GeIntroFadingOut) {
		playerSetFadeColour(0, 0, 0, 1);
		playerSetFadeFrac(60, 0);
	}
}

/** GoldenEye's CAMERAMODE_SWIRL, or straight on when the setup has no path. */
static void gecinemaIntroBeginSwirl(void)
{
	struct player *pl = g_Vars.currentplayer;

	// the still borrowed the player's angles to aim with (gecinemaPlace)
	pl->vv_theta = g_GeIntroTheta;
	pl->vv_verta = g_GeIntroVerta;
	bmoveUpdateVerta();
	bmove0f0cc654(0, 0, 0);

	g_GeCinemaCamRoom = -1;
	g_GeIntroFadingOut = 0;
	hudmsgRemoveAll();

	playerSetFadeColour(0, 0, 0, 1);
	playerSetFadeFrac(60, 0);

	if (g_GeNumSwirl < 2) {
		gecinemaIntroEnd();
		return;
	}

	g_GeIntroStage = GEINTRO_SWIRL;
	g_GeIntroTimer = 0;
	g_GeIntroLeg = 0;
	g_GeIntroPosed = 0;

	// Perfect Dark's frozen camera: the chr body is built and ticked, the walk
	// is not, and the camera is whoever's who asks (gecinemaSwirlTick)
	playerSetTickMode(TICKMODE_WARP);
}

/** A mission begins: GoldenEye's CAMERAMODE_INTRO. */
static void gecinemaIntroBegin(void)
{
	struct player *pl = g_Vars.currentplayer;

	g_GeIntroPending = 0;
	gecinemaCollect();

	gecinemaEnter();

	if (g_GeCinemaNumShots <= 0) {
		gecinemaIntroBeginSwirl();
		return;
	}

	g_GeIntroStage = GEINTRO_STILL;
	g_GeIntroShot = g_GeCinemaShots[rngRandom() % (u32)g_GeCinemaNumShots];
	g_GeIntroTimer = 0;
	g_GeCinemaLine = 0;

	playerSetFadeColour(0, 0, 0, 1);
	playerSetFadeFrac(60, 0);
}

/** The still and the fade out of it, every frame from lvTick(). */
static void gecinemaIntroTick(void)
{
	const u8 *shot = g_GeIntroShot;

	if (g_GeIntroStage == GEINTRO_SWIRL || g_GeIntroStage == GEINTRO_HOLD) {
		return;   // the swirl is ticked with the player (gecinemaSwirlTick)
	}

	gecinemaPlace(shot);

	if (g_GeIntroStage == GEINTRO_FADE) {
		if (playerIsFadeComplete()) {
			gecinemaIntroBeginSwirl();
		}

		return;
	}

	if (g_GeCinemaLine == 0 && g_GeIntroTimer >= SHOT_LINE1) {
		g_GeCinemaLine = 1;
		gecinemaShowLine(shot, 0);
	} else if (g_GeCinemaLine == 1 && g_GeIntroTimer >= SHOT_LINE2 && *(const u32 *)(shot + 0x20)) {
		g_GeCinemaLine = 2;
		gecinemaShowLine(shot, 1);
	}

	g_GeIntroTimer += g_Vars.diffframe60f;

	if (g_GeIntroTimer > (*(const u32 *)(shot + 0x20) ? SHOT_END_2 : SHOT_END_1)
			|| (g_GeIntroTimer > 10.0f && !lvIsPaused() && gecinemaPressed())) {
		g_GeIntroStage = GEINTRO_FADE;
		playerSetFadeColour(0, 0, 0, 0);
		playerSetFadeFrac(60, 1);
	}
}

/**
 * bondviewCalcIntroSwirlCamera(): the camera on leg `index` of the path, `time`
 * into it, and what it looks at.
 *
 * The four points the spline runs through are the leg's own, the one before
 * and the two after, never stepping past the record that ends the path. A
 * point flagged 2 is an offset in Bond's own frame - turned by the way he
 * faces - and the rest are in the level's. What the camera looks at is Bond's
 * eyes, pushed forty units along his own line of sight over the legs flagged
 * 4, which is what brings the picture round to what he is looking at as the
 * camera arrives.
 */
static void gecinemaSwirlCamera(s32 index, f32 time, struct coord *pos, struct coord *lookat)
{
	const struct player *pl = g_Vars.currentplayer;
	const struct geswirl *base = g_GeSwirl;
	const struct geswirl *leg = &base[index];
	struct coord pts[4];
	f32 frac = 0.0f;
	f32 blend;
	f32 t2, t3, a, b, c, d;

	if (leg->duration > 0.0f) {
		frac = time / leg->duration;
	}

	for (s32 i = -1; i < 3; i++) {
		const struct geswirl *entry = leg;
		struct coord *dst = &pts[i + 1];

		if (i < 0) {
			entry = index > 0 ? leg - 1 : base;
		} else {
			while (entry < leg + i && !(entry[1].flags & 1)) {
				entry++;
			}
		}

		if (entry->flags & 2) {
			dst->x = entry->off.z * pl->bond2.unk00.x + entry->off.x * pl->bond2.unk00.z;
			dst->y = entry->off.y;
			dst->z = entry->off.z * pl->bond2.unk00.z - entry->off.x * pl->bond2.unk00.x;
		} else {
			*dst = entry->off;
		}
	}

	// coord3dCubicSplineInterp()
	t2 = frac * frac;
	t3 = t2 * frac;
	a = (2.0f * t2 - (frac + t3)) * leg->scale;
	b = (2.0f - leg->scale) * t3 + t2 * (leg->scale - 3.0f) + 1.0f;
	c = (leg->scale - 2.0f) * t3 + t2 * (3.0f - 2.0f * leg->scale) + frac * leg->scale;
	d = (t3 - t2) * leg->scale;

	for (s32 k = 0; k < 3; k++) {
		pos->f[k] = a * pts[0].f[k] + b * pts[1].f[k] + c * pts[2].f[k] + d * pts[3].f[k]
			+ pl->bond2.unk10.f[k];
		lookat->f[k] = pl->bond2.unk10.f[k];
	}

	if (!(leg->flags & 4)) {
		blend = (leg[1].flags & 4) ? frac : 0.0f;
	} else {
		blend = (leg[1].flags & 4) ? 1.0f : 1.0f - frac;
	}

	for (s32 k = 0; k < 3; k++) {
		lookat->f[k] += pl->bond2.unk1c.f[k] * 40.0f * blend;
	}
}

/**
 * The swirl's camera, from playerTick()'s TICKMODE_WARP once the body has been
 * built and ticked. True while the swirl has the camera, so that the warp's own
 * does not take it.
 */
s32 gecinemaSwirlTick(void)
{
	struct player *pl = g_Vars.currentplayer;
	struct coord pos, lookat, look;
	struct coord up = {0, 1, 0};
	f32 left;

	if (g_GeIntroStage == GEINTRO_HOLD) {
		return 1;
	}

	if (g_GeIntroStage != GEINTRO_SWIRL || !pl || !pl->prop) {
		return 0;
	}

	if (gecinemaIsOn() && gecinemaLeavePressed()) {
		gecinemaIntroEnd();
		return 1;
	}

	if (!g_GeIntroPosed && pl->haschrbody && pl->model00d4 && pl->prop->chr) {
		// what Bond is doing when the camera finds him: the setup's own choice
		// out of stage_intro_anim_table[], played on his body the way
		// GoldenEye's bondviewSetCameraMode() does
		const s32 row = g_GeIntroAnimIndex >= 0 && g_GeIntroAnimIndex < ARRAYCOUNT(g_GeIntroAnimTable)
			? g_GeIntroAnimIndex : 0;
		const s32 animnum = gexPlusMissionAnim(g_GeIntroAnimTable[row].geanim);

		g_GeIntroPosed = 1;
		playerStartChrFade(0, 1);

		if (animnum > 0 && pl->model00d4->anim) {
			modelSetAnimation(pl->model00d4, animnum, 0, g_GeIntroAnimTable[row].start,
					g_GeIntroAnimTable[row].speed, 0);

			if (g_GeIntroAnimTable[row].end > 0.0f) {
				modelSetAnimEndFrame(pl->model00d4, g_GeIntroAnimTable[row].end);
			}

			pl->prop->chr->actiontype = ACT_BONDINTRO;
			pl->prop->chr->sleep = 0;
		}
	}

	g_GeIntroTimer += g_Vars.lvupdate60freal;

	while (g_GeSwirl[g_GeIntroLeg].duration <= g_GeIntroTimer) {
		if (!(g_GeSwirl[g_GeIntroLeg + 3].flags & 1)) {
			g_GeIntroTimer -= g_GeSwirl[g_GeIntroLeg].duration;
			g_GeIntroLeg++;
		} else {
			g_GeIntroTimer = g_GeSwirl[g_GeIntroLeg].duration;
			gecinemaIntroEnd();
			return g_GeIntroStage == GEINTRO_HOLD;
		}
	}

	// how long is left of the whole path
	left = g_GeSwirl[g_GeIntroLeg].duration - g_GeIntroTimer;

	for (s32 i = g_GeIntroLeg + 1; !(g_GeSwirl[i + 2].flags & 1); i++) {
		left += g_GeSwirl[i].duration;
	}

	// the body goes from solid to nothing just before the camera is inside it
	if (left < 30.0f && left + g_Vars.lvupdate60freal >= 30.0f) {
		playerStartChrFade(30, 0);
	}

	if (g_GeIntroFadingOut) {
		if (playerIsFadeComplete()) {
			gecinemaIntroEnd();
			return g_GeIntroStage == GEINTRO_HOLD;
		}
	} else if (left > 60.0f && !lvIsPaused() && gecinemaPressed()) {
		g_GeIntroFadingOut = 1;
		playerSetFadeColour(0, 0, 0, pl->colourscreenfrac);
		playerSetFadeFrac(playerIsFadeComplete() ? 60 : pl->colourfadetime60, 1);
	}

	gecinemaSwirlCamera(g_GeIntroLeg, g_GeIntroTimer, &pos, &lookat);

	look.x = lookat.x - pos.x;
	look.y = lookat.y - pos.y;
	look.z = lookat.z - pos.z;

	playerSetCameraMode(CAMERAMODE_THIRDPERSON);

	if (g_GeSwirl[g_GeIntroLeg].pad >= 0) {
		// a leg far enough from Bond to be in another part of the level names
		// the pad whose room it is in (Dam's starts over the reservoir)
		struct pad pad;

		padUnpack(g_GeSwirl[g_GeIntroLeg].pad, PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (pad.room > 0 && pad.room < g_Vars.roomcount) {
			player0f0c1ba4(&pos, &up, &look, &pad.pos, pad.room);
			return 1;
		}
	}

	player0f0c1840(&pos, &up, &look, &pl->prop->pos, pl->prop->rooms);

	return 1;
}

/** Every frame of a level, from lvTick(). */
void gecinemaTick(void)
{
	const u8 *shot;
	f32 end;
	s32 skip;

	if (g_GeIntroPending && g_Vars.currentplayer && g_Vars.currentplayer->prop
			&& !g_Vars.currentplayer->isdead) {
		gecinemaIntroBegin();
	}

	if (g_GeIntroStage != GEINTRO_NONE) {
		gecinemaIntroTick();
		return;
	}

	if (!gecinemaIsOn() || !g_Vars.currentplayer || !g_Vars.currentplayer->prop) {
		return;
	}

	if (g_GeCinemaNumShots < 0) {
		gecinemaCollect();
	}

	if (!g_GeCinemaEntered) {
		gecinemaEnter();
	}

	if (g_GeCinemaWhat == GECINEMA_ENDING) {
		gecinemaEndingTick();
		return;
	}

	if (g_GeCinemaShot >= g_GeCinemaNumShots && !g_GeCinemaLeft && g_GeCinemaNumShots > 0) {
		// Every shot has been seen, and what GoldenEye does after the one it
		// shows is fade to black and swirl down to Bond: the mission's own
		// opening from here on (gecinemaIntroTick), which ends the cinema where
		// a mission would hand over
		g_GeIntroShot = g_GeCinemaShots[g_GeCinemaNumShots - 1];
		g_GeIntroStage = GEINTRO_FADE;
		playerSetFadeColour(0, 0, 0, 0);
		playerSetFadeFrac(60, 1);
		return;
	}

	if (g_GeCinemaShot >= g_GeCinemaNumShots) {
		// nothing to watch, or the player backed out
		gecinemaFinish();

		return;
	}

	shot = g_GeCinemaShots[g_GeCinemaShot];
	gecinemaPlace(shot);

	end = *(const u32 *)(shot + 0x20) ? SHOT_END_2 : SHOT_END_1;

	if (g_GeCinemaLine == 0 && g_GeCinemaTime60 >= SHOT_LINE1) {
		g_GeCinemaLine = 1;
		gecinemaShowLine(shot, 0);
	} else if (g_GeCinemaLine == 1 && g_GeCinemaTime60 >= SHOT_LINE2 && *(const u32 *)(shot + 0x20)) {
		g_GeCinemaLine = 2;
		gecinemaShowLine(shot, 1);
	}

	// GoldenEye's own press ends the shot; here the rest of them are still to
	// come, so backing out leaves the whole thing - this is a gallery, not the
	// way into a mission - and a pick goes on to the next shot. Leaving is
	// asked first: the right mouse button is R as well as Cancel.
	//
	// The sixth of a second is so that the press that started the cinema is
	// not read as one in it, and it is counted from the start of the cinema
	// for leaving - counted from the start of the shot, a player leaning on
	// the button through a skip had their next press thrown away.
	{
		const s8 contpad = optionsGetContpadNum1(g_Vars.currentplayerstats
				? g_Vars.currentplayerstats->mpindex : 0);
		// the two UI buttons and the keyboard belong to the first pad, as in
		// the folder
		const u32 ui = contpad == 0 ? ~0u : ~(u32)(BUTTON_UI_CANCEL | BUTTON_UI_ACCEPT);

		if (g_GeCinemaTotal60 > 10.0f
				&& (joyGetButtonsPressedThisFrame(contpad, LEAVE_BUTTONS & ui)
					|| inputKeyJustPressed(VK_ESCAPE))) {
			g_GeCinemaShot = g_GeCinemaNumShots;
			g_GeCinemaLeft = 1;
			return;
		}

		skip = g_GeCinemaTime60 > 10.0f
			&& (joyGetButtonsPressedThisFrame(contpad, SKIP_BUTTONS & ui) != 0
				|| inputKeyJustPressed(VK_MOUSE_LEFT));
	}

	g_GeCinemaTime60 += g_Vars.diffframe60f;
	g_GeCinemaTotal60 += g_Vars.diffframe60f;

	if (skip || g_GeCinemaTime60 >= end) {
		g_GeCinemaShot++;
		g_GeCinemaTime60 = 0;
		g_GeCinemaLine = 0;
	}
}

#endif

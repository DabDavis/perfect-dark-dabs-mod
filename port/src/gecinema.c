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
#include "modloader.h"
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
#define SKIP_BUTTONS (A_BUTTON | B_BUTTON | Z_TRIG | START_BUTTON | R_TRIG | L_TRIG)

// The mission the folder picked, waiting for its stage to load
static s32 g_GeCinemaArmed = -1;
// and once it has: the mission being watched, or -1
static s32 g_GeCinemaMission = -1;
// set when the last shot is over, so the folder opens again on the Cinema page
static s32 g_GeCinemaWantFolder;

static const u8 *g_GeCinemaShots[MAX_SHOTS];
static s32 g_GeCinemaNumShots = -1;   // -1 until the stage's records are read
static s32 g_GeCinemaShot;
static f32 g_GeCinemaTime60;
static s32 g_GeCinemaLine;            // how many of the shot's lines have shown
static s32 g_GeCinemaEntered;         // the one-off setup has run

/**
 * The folder's Cinema page picked a mission. The stage starts the way a mission
 * does; gecinemaStageStart() picks this up when it has loaded.
 */
void gecinemaArm(s32 mission)
{
	g_GeCinemaArmed = mission;
}

/** Every stage load: this one is a cinema if the folder armed one. */
void gecinemaStageStart(void)
{
	g_GeCinemaMission = g_GeCinemaArmed;
	g_GeCinemaArmed = -1;
	g_GeCinemaNumShots = -1;
	g_GeCinemaShot = 0;
	g_GeCinemaTime60 = 0;
	g_GeCinemaLine = 0;
	g_GeCinemaEntered = 0;
}

s32 gecinemaIsOn(void)
{
	return g_GeCinemaMission >= 0;
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

	while (cmd) {
		const u32 type = *(const u32 *)cmd;

		if (type >= sizeof(lens) || !lens[type]) {
			break;   // INTROCMD_END, or a record whose length is not known
		}

		if (type == 6 && g_GeCinemaNumShots < MAX_SHOTS) {
			g_GeCinemaShots[g_GeCinemaNumShots++] = cmd;
		}

		cmd += lens[type];
	}
}

/** The player is the camera: no walk, no gun, no HUD and nothing to hit them. */
static void gecinemaEnter(void)
{
	g_GeCinemaEntered = 1;

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

/** The camera at one shot: modSpectateTick()'s move, with the angles given. */
static void gecinemaPlace(const u8 *shot)
{
	struct player *pl = g_Vars.currentplayer;
	struct prop *prop = pl->prop;
	struct coord dstpos;
	RoomNum dstrooms[8];

	dstpos.x = *(const f32 *)(shot + 0x04);
	dstpos.y = *(const f32 *)(shot + 0x08);
	dstpos.z = *(const f32 *)(shot + 0x0c);

	// The record names the pad the camera stands on and GoldenEye takes the
	// room from that pad's stan tile, which is the answer for a camera hanging
	// over a valley or inside a wall - a portal walk from where the player
	// spawned gives the room they were in and draws nothing. The walk is still
	// what fills the rest of the list, from the pad's room outwards.
	{
		struct pad pad;

		padUnpack(*(const u32 *)(shot + 0x18), PADFIELD_ROOM, &pad);

		if (pad.room > 0 && pad.room < g_Vars.roomcount) {
			prop->rooms[0] = pad.room;
			prop->rooms[1] = -1;
		}
	}

	func0f065e74(&prop->pos, prop->rooms, &dstpos, dstrooms);

	prop->pos.x = dstpos.x;
	prop->pos.y = dstpos.y;
	prop->pos.z = dstpos.z;

	propDeregisterRooms(prop);
	roomsCopy(dstrooms, prop->rooms);
	bmoveUpdateRooms(pl);

	// The prop is the eye and there is no walk to say where the floor is, so
	// the ground goes with it, as spectating does.
	pl->vv_ground = prop->pos.y - pl->vv_height;
	pl->vv_manground = pl->vv_ground;

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
	bmove0f0cc19c(&prop->pos);
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

/** Every frame of a level, from lvTick(). */
void gecinemaTick(void)
{
	const u8 *shot;
	f32 end;
	s32 skip;

	if (!gecinemaIsOn() || !g_Vars.currentplayer || !g_Vars.currentplayer->prop) {
		return;
	}

	if (g_GeCinemaNumShots < 0) {
		gecinemaCollect();
	}

	if (!g_GeCinemaEntered) {
		gecinemaEnter();
	}

	if (g_GeCinemaShot >= g_GeCinemaNumShots) {
		// Nothing left to watch: back to the folder. The stage does not change
		// until the end of the frame, so the flag is also what keeps this from
		// asking again on every frame until it does.
		if (!g_GeCinemaWantFolder) {
			g_GeCinemaWantFolder = 1;
			mainChangeToStage(STAGE_TITLE);
		}

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
	// come, so B leaves the whole thing - this is a gallery, not the way into
	// a mission. The tenth of a second is so that the press that started the
	// cinema is not read as one in it.
	{
		const s8 contpad = optionsGetContpadNum1(g_Vars.currentplayerstats
				? g_Vars.currentplayerstats->mpindex : 0);

		if (g_GeCinemaTime60 > 10.0f && joyGetButtonsPressedThisFrame(contpad, B_BUTTON)) {
			g_GeCinemaShot = g_GeCinemaNumShots;
			return;
		}

		skip = g_GeCinemaTime60 > 10.0f && joyGetButtonsPressedThisFrame(contpad, SKIP_BUTTONS) != 0;
	}

	g_GeCinemaTime60 += g_Vars.diffframe60f;

	if (skip || g_GeCinemaTime60 >= end) {
		g_GeCinemaShot++;
		g_GeCinemaTime60 = 0;
		g_GeCinemaLine = 0;
	}
}

#endif

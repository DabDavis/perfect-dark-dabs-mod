/**
 * GoldenEye's watch, as GE Plus's pause.
 *
 * GoldenEye pauses by raising Bond's left arm and looking at the watch on it:
 * the gun goes down, the view tilts to -40 degrees, the arm comes up playing
 * GoldenEye's own `bond_watch`, and the view zooms from 60 degrees to 5.9 so
 * that the watch face fills the screen. Its five screens - mission status,
 * inventory, control, options and the briefing - are drawn on the face, and
 * the way out runs the same four steps backwards. This is GoldenEye's own
 * order, its own numbers and its own state machine (the decomp's
 * `bondviewWatchAnimationTick()` and `src/game/options.c`).
 *
 * What is drawn comes out of the conversion of the player's ROM (geconvert.c):
 *
 * - the arm is GoldenEye's `Csuit_lf_handZ`, character 41, converted as
 *   `files/Cgx041Z`. Its three `positionheld` joints are the hour, minute and
 *   second hands, which GoldenEye turns by the mission clock rather than by
 *   the animation, and its six cuff toggles are Bond's outfits;
 * - the animation that raises it is GoldenEye's `bond_watch` (id 45), which
 *   the conversion always writes into `menu/geanims.bin`;
 * - the text is GoldenEye's `LoptionsE` with the folder screens' two fonts
 *   (gexFrontLoadText()), laid out on GoldenEye's in-game 320x240 frame.
 *
 * Three things are Perfect Dark's rather than GoldenEye's, because GoldenEye's
 * own way of doing them does not exist here:
 *
 * - the gun goes down by being swapped for unarmed (bgunEquipWeapon2()), which
 *   is Perfect Dark's own lowering; GoldenEye swaps the hand's *item* for the
 *   suit hand and has no other lowering either;
 * - the watch rises into the view rather than out of the player's own wrist.
 *   GoldenEye blends the model from where the watch sits on the body to a pose
 *   25 units in front of the eye; the view model here has no body to start
 *   from, so it takes the same pose and the animation does the swinging;
 * - the pages' options set Perfect Dark's own settings, which is what the
 *   game underneath actually reads.
 *
 * While the watch is up the player is `PAUSEMODE_PAUSED` with no menu open,
 * and the level is frozen from the moment the arm starts up to the moment it
 * comes down (GoldenEye's `pausing_flag`). The watch's own clock is the real
 * frame delta (`g_Vars.diffframe60freal`), as GoldenEye's is, so it keeps
 * moving while the level does not.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "input.h"
#include "mod.h"
#include "modloader.h"
#include "romdata.h"
#include "system.h"
#include "video.h"
#include "gewatch.h"
#include "xblamesh.h"
#include "gehud.h"
#include "gegadgets.h"
#include "gexfront.h"
#include "gexplus.h"
#include "geanimtable.h"
#include "game/bondmove.h"
#include "game/body.h"
#include "game/bondgun.h"
#include "game/dlights.h"
#include "game/file.h"
#include "game/game_006900.h"
#include "game/gfxmemory.h"
#include "game/inv.h"
#include "game/lang.h"
#include "game/lv.h"
#include "game/mainmenu.h"
#include "game/game_0b0fd0.h"
#include "game/menu.h"
#include "trace.h"
#include "game/modeldef.h"
#include "game/modelmgr.h"
#include "game/objectives.h"
#include "game/quaternion.h"
#include "game/options.h"
#include "game/player.h"
#include "game/camera.h"
#include "game/playermgr.h"
#include "game/zbuf.h"
#include "game/mplayer/mplayer.h"
#include "lib/anim.h"
#include "lib/joy.h"
#include "lib/main.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "lib/rng.h"
#include "lib/snd.h"
#include "gesfx.h"
#include "gemusic.h"
#include "game/music.h"
#include "lib/vi.h"

/* ---- GoldenEye's own numbers --------------------------------------------- */

// GoldenEye's in-game frame: 320x240, which its watch is laid out on
#define WATCH_FRAME_W 320.0f
#define WATCH_FRAME_H 240.0f

// options.h, the US column
#define XOFFSET_1              64
#define YOFFSET_1              80
#define YINC                   15
#define YOFFSET_WEAPTEXT       167
#define YOFFSET_ACTIONTEXT     149
#define YOFFSET_4              203
#define YOFFSET_5              185
#define YOFFSET_MISSIONSTATUS  0x41
#define YOFFSET_7              0x31
#define YOFFSET_8              0x25
#define YOFFSET_9              0x3b
/**
 * GoldenEye's zooms with the watch open, which are not a pulse as a screen
 * turns but where each screen stays: the mission status at 5.9 degrees, the
 * inventory and the briefing at 4.6, the control and options screens at 3.95
 * (options.c's watch_screen*_navigation(), each trigger_watch_zoom() to the
 * screen it goes to, over 15 sixtieths). They are vertical fields of view over
 * the player's viewport. On a window wider than 4:3 the view stands further out
 * by one factor for every screen (watchWiden()), so the arm and the hand are
 * beside the watch, where GoldenEye's 16:9 mode stretched the picture sideways.
 */
#define WATCHZOOM1             4.6f
#define WATCHZOOM2             5.9f
#define WATCHZOOM3             3.95f
#define WATCH_PAGE_ZOOMTIME    15.0f

// the inventory's line, LINEHEIGHT()
#define INV_LINE               12

// the five screens (WATCH_INDEX)
enum {
	PAGE_MISSION,
	PAGE_INVENTORY,
	PAGE_CONTROL,
	PAGE_OPTIONS,
	PAGE_BRIEFING,
	NUM_PAGES,
};

// the briefing's own five (WATCH_BRIEF_INDEX)
enum { BRIEF_BACKGROUND, BRIEF_M, BRIEF_Q, BRIEF_MONEYPENNY, BRIEF_OBJECTIVES, NUM_BRIEF_PAGES };

// LoptionsE (assets/obseg/text/LoptionE.h)
enum {
	// "1.1 honey" to "2.4 goodhead", the eight control styles
	STR_STYLE_FIRST = 0x09,
	STR_LOOKUPDOWN = 0x11, STR_AUTOAIM, STR_LOOKAHEAD, STR_AIMCONTROL, STR_SIGHTONSCREEN,
	STR_AMMOONSCREEN, STR_SCREEN, STR_RATIO, STR_ON, STR_OFF, STR_UPRIGHT, STR_REVERSE,
	STR_TOGGLE, STR_HOLD, STR_FULL, STR_WIDE, STR_CINEMA, STR_NORMAL, STR_169,
	STR_ABORT, STR_CONFIRM, STR_CANCEL, STR_MISSIONSTATUS, STR_COMPLETE, STR_INCOMPLETE,
	STR_LEFTHAND, STR_QWATCH, STR_DOWN, STR_UP, STR_SIDESTEP1, STR_SIDESTEP2, STR_FORWARD,
	STR_BACK, STR_CONTROLSTYLE, STR_CONTROLLER, STR_CONTROLLERS, STR_MUSIC, STR_FX,
	STR_FAILED, STR_2BACKGROUND, STR_3MBRIEFING, STR_4QBRANCH, STR_5MONEYPENNY,
	STR_1OBJECTIVES,
};

// the colours GoldenEye's own screens are drawn in, RGBA
#define COL_GREEN     0x00ff00b0
#define COL_HIGHLIGHT 0xa0ffa0f0
#define COL_DIM       0x00800080
#define COL_WHITE     0xffffffff

// the watch face: 30 vertices round a disc of radius 520, the green fill just
// inside the ring (sub_GAME_7F0A33F8(), draw_watch_background())
#define FACE_VERTICES 30
#define FACE_RADIUS   520.0f
#define FACE_RING     0.92f
#define FACE_FILL     0.9f

/**
 * What the face's own matrix is scaled to once the watch is up, which is what
 * GoldenEye's floating arm comes to (its 0.01) and so what its own numbers for
 * the screens are drawn at. The move that carries the watch to the eye carries
 * this scale with it, so a body of any size ends up wearing a watch of the
 * same size - and its arm shrinks or grows with the watch, which is what keeps
 * an arm looking like an arm beside it.
 */
#define WATCH_FACE_SCALE 0.01f

// the screen-select rectangles under it (options.h)
#define SELECT_RECTS   5
#define SELECT_WIDTH   100
#define SELECT_HEIGHT  20
#define SELECT_HSTEP   125
#define SELECT_LEFT    (-299)
#define SELECT_TOP     0x136

// the health and armour gauges either side of it (trigger_solo_watch_menu())
#define GAUGE_PAIRS    23
#define GAUGE_VERTICES (GAUGE_PAIRS * 2)

// GoldenEye's own suit hand, and the animation that raises it
#define HAND_CHR       41
#define ARM_FRAMES     20.0f
#define ARM_DURATION   40.0f

// its cuff toggles, in bondviewSelectCuff(model, header, 4)'s own order
#define CUFF_FIRST     4
#define CUFF_PART_BOILER   (CUFF_FIRST + 0)
#define CUFF_PART_TUXEDO   (CUFF_FIRST + 1)
#define CUFF_PART_CONNERY  (CUFF_FIRST + 2)
#define CUFF_PART_BLUE     (CUFF_FIRST + 3)
#define CUFF_PART_JUNGLE   (CUFF_FIRST + 4)
#define CUFF_PART_SNOW     (CUFF_FIRST + 5)

// and GoldenEye's own outfits (its CUFF_TYPES), which a mission names in its
// setup's intro stream. Perfect Dark reads the same command (INTROCMD_OUTFIT)
// into the same field it always had, `bondtype`, so a converted mission's own
// outfit is already there to be read.
#define GECUFF_BLUE    0
#define GECUFF_BROSNAN 1
#define GECUFF_JUNGLE  2
#define GECUFF_BOILER  3
#define GECUFF_SNOW    4
#define GECUFF_CONNERY 5
#define GECUFF_DALTON  6
#define GECUFF_MOORE   7
#define GECUFF_FOLDER  8

/**
 * How GoldenEye's own watch model sits on a hand it was not built on, which is
 * measured rather than fitted: GoldenEye's floating arm carries the same watch
 * as part of its own mesh, hung off the same hand bone of the same skeleton, so
 * where that one sits *is* the answer.
 *
 * The watch model's own axes (Igx056Z): the band is a loop round x, which is
 * the forearm; the dial looks out along +z; its three hands lie along +y, so
 * that is twelve o'clock, and the crown is on +x. The floating arm's hand bone:
 * the fingers are +x and the sleeve -x, the dial looks out along +y (the back
 * of the hand) and its hour hand lies along -z. So the watch goes on the bone
 * turned a quarter turn back about x - y to -z, z to +y - which is the inverse
 * of the rotX(+90) the pose squares the hand bone to, and is why the two
 * together show the dial upright with the fingers to its right.
 *
 * The floating arm's dial is at (-419.4, 152.2, 32.2) on its bone in units a
 * tenth of a body's, and the watch model's own dial is at (-10.5, 0, 196), both
 * models being the same size in their own units: that puts the middle of the
 * band a body's 18 units behind the wrist past its own half width, 4.4 under
 * the bone and 3.2 to the side, which is the middle of this body's wrist too.
 */
#define WATCH_DIAL_X       (-10.5f)  // the dial's middle, in the watch's own units
#define WATCH_DIAL_Y       0.0f
#define WATCH_DIAL_Z       196.0f
#define WATCH_HALF_ALONG   229.0f    // half the case, along the forearm
#define WATCH_WRIST_CLEAR  18.0f     // body units between the case and the wrist joint
#define WATCH_WRIST_Y      (-4.4f)
#define WATCH_WRIST_Z      3.2f

/**
 * How big the watch is on the wrist: life size, which is GoldenEye's own - a
 * body's units are ten of the watch's, and at that its band goes round a
 * body's wrist as it goes round the floating arm's. The face is drawn at one
 * size whatever this is (WATCH_FACE_SCALE), so what it really sets is how big
 * the *arm* is beside it, and anything over life size shrinks the whole body
 * towards the watch: at 0.35 the shoulders came into the picture from under the
 * eye, with the neck they end in.
 */
#define WATCH_WRIST_SCALE 0.1f

/**
 * The watch model's three hands are part of its one mesh, lying at twelve: flat
 * shapes a few units over the dial, each at a height of its own, and nothing
 * else is that close to the middle above the dial. They are found by that at
 * the load and turned in the mesh's own vertices (watchTurnMeshHands()).
 */
#define WATCH_HAND_VTX     16       // the most one hand is made of
#define WATCH_HAND_REACH   140.0f   // from the middle; the glass starts at 160
#define WATCH_HAND_OVER    2.0f     // over the dial

// the watch's pose in front of the eye (player.c's field_1D4, field_1D8 and
// pause_watch_position), and how big it is drawn there
#define WATCH_POSE_X   0.0f
#define WATCH_POSE_Y   0.0f
#define WATCH_POSE_Z   (-25.0f)

// the near plane of the projection the arm is drawn under
#define WATCH_NEAR     10.0f

// GoldenEye's own watch states (WATCH_ANIMATION_STATE_IDS)
enum {
	WS_CLOSED,
	WS_LOWER,     // 1: put the gun away
	WS_TILT,      // 2: the view goes down to the wrist
	WS_RAISE,     // 3: the arm comes up
	WS_ZOOMIN,    // 4: and the face is zoomed into
	WS_OPEN,      // 5: the watch is up and the game is paused
	WS_ZOOMOUT,   // 6
	WS_LOWERARM,  // 7
	WS_RESTORE,   // 8: the gun comes back
	WS_CLOSING = 0xc, // the page has been left and the zoom out is next
};

/* ---- what the watch is holding ------------------------------------------ */

struct gewatch {
	s32 loaded;
	s32 moddir;
	s32 stagenum;

	// the arm, and the animation that raises it: GoldenEye's own floating arm,
	// or the player's own body with GoldenEye's watch on its wrist (isbody)
	u8 *modelbuf;
	u32 modelbuflen;
	struct modeldef *modeldef;
	struct model *model;
	s32 animnum;
	f32 chrscale;
	s32 isbody;

	// and the head that goes on it: a copy of its own, since one head's
	// definition cannot sit on two bodies (modelAttachHead() re-parents it)
	u8 *headbuf;
	u32 headbuflen;

	// and a ball round it, in the head's own units, for knowing when the near
	// plane is about to cut it
	s32 hashead;
	struct coord headmid;
	f32 headradius;

	// GoldenEye's own watch, which the player's body has none of
	u8 *watchbuf;
	u32 watchbuflen;
	struct modeldef *watchdef;
	struct model *watchmodel;

	// its hour, minute and second hands, which are vertices of its one mesh:
	// which ones, and where each was drawn, so that they are always turned
	// from twelve rather than from wherever the last frame left them
	Vtx *handvtx;
	s32 numhandvtx[3];
	s16 handindex[3][WATCH_HAND_VTX];
	s16 handxy[3][WATCH_HAND_VTX][2];

	// GoldenEye's own text: LoptionsE, and the open mission's briefing
	u8 *options;
	u32 optionslen;
	u8 *mpmenu;
	u32 mpmenulen;
	u8 *brief;
	u8 *lang;
	u32 langlen;
	s32 mission;

	// the state machine (bondviewWatchAnimationTick())
	s32 state;
	s32 statetime;   // watch_pause_time, 1 on the first frame of a state
	f32 timer;       // timer_1C4

	// the view's pitch on the way down to the wrist and back (pause_state)
	s32 tiltstate;
	f32 tiltfrom;
	f32 tiltto;
	f32 tilttime;
	f32 tiltduration;
	f32 tiltstart;   // where the player was looking when the watch opened

	// the arm (step_in_view_watch_animation, pause_animation_counter)
	s32 armstep;
	f32 armframe;
	f32 armspeed;

	// the screens
	s32 page;
	s32 selected;    // watch_item_is_actively_selected
	s32 confirm;     // the abort's confirm/cancel
	s32 optionrow;   // game_options_index
	s32 controlrow;
	s32 briefpage;
	s32 invrow;
	s32 sticky;      // the stick's up/down latch

	// the interference (g_WatchBackgroundGreen, g_WatchStaticScanlineY)
	s32 bggreen;     // 0xe0 when the face is clear, 0x80 as static strikes
	s32 scany;       // the scanline's place up the face, -0x156 to 0x156
	f32 staticacc;   // 60ths owed to watchTickStatic()
	f32 gunangle;    // D_80040B14, the inventory's gun's turn
	f32 padspin;     // g_WatchControllerSpinAngle
	f32 padspeed;    // g_WatchControllerSpinSpeed
	s32 padidle;     // D_80040B2C, frames since the player last turned it
	s32 statichalf;

	// the controller's tip towards the player while they have hold of it
	// (g_WatchControllerPitch), and the two latches GoldenEye's stick moves
	// through: sideways (controlstick_lr_enabled) and up and down
	// (watch_stick_y_nav_ready, watch_stick_y_prev_active)
	f32 padpitch;
	s32 lrready;
	s32 yready;
	s32 yactive;

	// the pulses: INCOMPLETE on the mission status (D_80040AF4, a green that
	// fades and jumps back) and on the objectives (D_80040AFC)
	u32 statuspulse;
	s32 statuswait;
	u32 objpulse;
	s32 objwait;

	// the inventory's list (game_options_inventory_navigation()): a cursor
	// that moves in tenths while a direction is held, the item it is on, and
	// the list easing to it; and the name drawn outlined for ten frames after
	// an item is taken (D_800409C4)
	f32 invcursor;
	s32 invtexty;
	s32 invsettled;
	s32 invflash;

	// the control style's list, the same kind of list one line tall
	f32 stylecursor;
	s32 styleindex;
	s32 styletexty;
	s32 stylesettled;

	// what was in the hand when the watch came up, and its rounds, which
	// Perfect Dark's own lowering puts back in the reserve
	s32 ammoweapon;
	s32 ammomag;
	s32 ammoreserve;

	// what the level was doing before the watch took it over
	s32 paused;
	s32 music; // the watch theme is playing over the level's
	s32 weapons[2];
	s32 hadweapons;
};

static struct gewatch g_Watch;

// debugging: the arm can be left out of the frame to judge the face on its own
// ('gewatch.c'::g_WatchDrawArm from gdb), and the watch made bigger or smaller
// on the wrist to judge the framing ('gewatch.c'::g_WatchWristScale). Volatile
// because nothing in the game writes them: without it the compiler folds each
// into its one use and gdb has nothing to set.
static volatile s32 g_WatchDrawArm = 1;

// the player's own body wears the watch in place of GoldenEye's arm
// ('gewatch.c'::g_WatchOwnBody, before the first pause of a level)
static volatile s32 g_WatchOwnBody = 0;
static volatile f32 g_WatchWristScale = WATCH_WRIST_SCALE;

enum { MPPAGE_SCORES, MPPAGE_KILLS, MPPAGE_LOSSES, MPPAGE_PAUSE, MPPAGE_EXIT, NUM_MPPAGES };

// LmpmenuE (assets/obseg/text/LmpmenuE.h)
enum {
	MPSTR_RANK1 = 0x11, MPSTR_RANK2, MPSTR_RANK3, MPSTR_RANK4,
	MPSTR_PLAY, MPSTR_GAMEOVER, MPSTR_STARTTOEXIT, MPSTR_PAUSED, MPSTR_PAUSE,
	MPSTR_EXIT, MPSTR_SCORES, MPSTR_P, MPSTR_KILLS, MPSTR_LOSSES,
	MPSTR_WEAPONOFCHOICE, MPSTR_CANCEL, MPSTR_CONFIRM,
};

static struct {
	u8 on;
	u8 mode;
	u8 confirm;
	u8 sticky;
} g_MpWatch[MAX_PLAYERS];

// who paused, so that only they can let the match go again (who_paused)
static s32 g_MpWatchPauser = -1;

static void watchMpTick(void);
static Gfx *watchMpRender(Gfx *gdl);
static s32 watchIsMp(void);


static u32 watchBe32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u8 *watchLoad(const char *rel, u32 *len)
{
	char path[FS_MAXPATH + 1];
	const char *dir = fsGetModDirAt(g_Watch.moddir);

	if (!dir) {
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/menu/%s", dir, rel);

	return fsFileLoad(path, len);
}

/** A string of one of GoldenEye's banks: an offset table, then the strings. */
static const char *watchBankString(const u8 *bank, u32 len, s32 index)
{
	u32 at;

	if (!bank || index < 0 || (u32)(index + 1) * 4 > len) {
		return "";
	}

	at = watchBe32(bank + index * 4);

	return at && at < len ? (const char *)bank + at : "";
}

// LoptionsE, the watch's own screens
static const char *watchString(s32 index)
{
	return watchBankString(g_Watch.options, g_Watch.optionslen, index);
}

// and the open mission's bank, which its briefing file indexes
static const char *watchLangString(s32 id)
{
	return watchBankString(g_Watch.lang, g_Watch.langlen, id & 0x3ff);
}

/* ---- the arm ------------------------------------------------------------ */

/**
 * menu/gechrs.bin's row for a character: the scale makeonebody() would give
 * its model, which is what GoldenEye gives the watch
 * (c_item_entries[41].scale * 0.1).
 */
static f32 watchChrScale(s32 num)
{
	u32 len = 0;
	u8 *d = watchLoad("gechrs.bin", &len);
	f32 scale = 0.1f;

	if (d && len >= 8 && !memcmp(d, "GEC1", 4)) {
		const s32 rows = (s32)((d[4] << 8) | d[5]);

		for (s32 i = 0; i < rows && 8 + 12u * (i + 1) <= len; i++) {
			const u8 *row = d + 8 + 12 * i;

			if (((row[0] << 8) | row[1]) == num) {
				const u32 bits = watchBe32(row + 4);

				memcpy(&scale, &bits, sizeof(scale));
				scale *= 0.1f;
				break;
			}
		}
	}

	sysMemFree(d);

	return scale;
}

/**
 * GoldenEye's `bond_watch` (GEANIM_WATCH), appended after the game's own the
 * way a mission's animations are (gexPlusMissionAnimLoad()). Appending is
 * permanent, so it happens once a session.
 */
static s32 watchLoadAnim(void)
{
	static s32 animnum = -2;
	u32 len = 0;
	u8 *d;
	s32 numanims;

	if (animnum != -2) {
		return animnum;
	}

	animnum = -1;
	d = watchLoad("geanims.bin", &len);

	if (!d || len < 8 || memcmp(d, "GEA1", 4)) {
		sysMemFree(d);
		return -1;
	}

	numanims = (s32)((d[4] << 8) | d[5]);

	for (s32 r = 0; r < numanims && 8 + 20u * (r + 1) <= len; r++) {
		const u8 *row = d + 8 + 20 * r;
		const s32 id = (row[0] << 8) | row[1];
		const u32 at = watchBe32(row + 12);
		const u32 size = watchBe32(row + 16);
		struct animtableentry e;
		u8 *copy;

		if (id != GEANIM_WATCH || !size || at + size > len) {
			continue;
		}

		e.numframes = (row[2] << 8) | row[3];
		e.bytesperframe = (row[4] << 8) | row[5];
		e.headerlen = (row[6] << 8) | row[7];
		e.framelen = row[8];
		e.flags = row[9] ? ANIMFLAG_LOOP : 0;
		e.data = 0;

		// the bit reader runs off the end of the last frame
		copy = sysMemAlloc(size + 64);

		if (!copy) {
			break;
		}

		memcpy(copy, d + at, size);
		memset(copy + size, 0, 64);
		animnum = animAppendExternal(&e, copy);

		if (animnum < 0) {
			sysMemFree(copy);
		}

		break;
	}

	sysMemFree(d);

	if (animnum < 0) {
		sysLogPrintf(LOG_WARNING, "gewatch: the conversion has no `bond_watch`; the arm will not move");
	}

	return animnum;
}

/**
 * The arm's instance is the level's own - modelmgrInstantiateModel() hands out
 * a slot of the stage pool, which the next level reset takes back whole - so
 * it is dropped rather than freed. Freeing one after the stage it was made in
 * walks g_ModelRwdataBindings through memory the pool has reused, which is a
 * crash on the next stage load.
 */
static void watchGunUnload(void);

static void watchFreeModel(void)
{
	watchGunUnload();
	g_Watch.model = NULL;
	g_Watch.watchmodel = NULL;

	if (g_Watch.modelbuf) {
		videoFreeCachedTextures(g_Watch.modelbuf, g_Watch.modelbuf + g_Watch.modelbuflen);
		sysMemFree(g_Watch.modelbuf);
		g_Watch.modelbuf = NULL;
	}

	if (g_Watch.watchbuf) {
		videoFreeCachedTextures(g_Watch.watchbuf, g_Watch.watchbuf + g_Watch.watchbuflen);
		sysMemFree(g_Watch.watchbuf);
		g_Watch.watchbuf = NULL;
	}

	// The head is let go without being taken off the body's headspot first.
	// Once it is on, this only runs at the next stage's load, by when the body
	// it was on is gone with the stage pool (bodyreset.c has emptied the table)
	// and its headspot is memory somebody else has; inside a stage it runs only
	// for a body that never got as far as having the head put on.
	if (g_Watch.headbuf) {
		videoFreeCachedTextures(g_Watch.headbuf, g_Watch.headbuf + g_Watch.headbuflen);
		sysMemFree(g_Watch.headbuf);
		g_Watch.headbuf = NULL;
	}

	g_Watch.hashead = 0;
	g_Watch.handvtx = NULL;
	g_Watch.numhandvtx[0] = g_Watch.numhandvtx[1] = g_Watch.numhandvtx[2] = 0;

	// the player's body is the game's own modeldef and is not ours to drop
	if (!g_Watch.isbody) {
		g_Watch.modeldef = NULL;
	}

	g_Watch.modeldef = NULL;
	g_Watch.watchdef = NULL;
	g_Watch.isbody = 0;
}

// the next node of a definition's tree, depth first
static struct modelnode *watchNextNode(struct modelnode *node)
{
	if (node->child) {
		return node->child;
	}

	while (node && !node->next) {
		node = node->parent;
	}

	return node ? node->next : NULL;
}

/**
 * The render mode of a list, which a conversion older than 36 writes wrong.
 *
 * GoldenEye's plain list record (type 4) keeps it in **one byte** at 0x12 where
 * its list-with-collisions record (type 0x18) keeps a word at 0x18, and the
 * conversion read a word from both: a 3 came out as 0x0300. No case of
 * modelRenderNodeDl() answers to that, so the model is drawn in whatever render
 * mode the frame was left in and its second list is never drawn at all - which
 * is what made GoldenEye's arm a pale sleeve and an icy hand round a white
 * dial, and left its watch with no glass, crown or pusher.
 *
 * The conversion is right since version 36 and this does nothing to one. It is
 * kept for the player whose conversion is older and cannot be made again, the
 * ROM it came from having gone from data/ since: the two models the watch draws
 * are its own copies and are the watch's to put right.
 */
static void watchFixRenderModes(struct modeldef *def)
{
	for (struct modelnode *node = def->rootnode; node; node = watchNextNode(node)) {
		if ((node->type & 0xff) == MODELNODETYPE_DL && node->rodata) {
			const s16 mode = node->rodata->dl.mcount;

			if (mode > 4 && (mode & 0xff) == 0) {
				node->rodata->dl.mcount = mode >> 8;
			}
		}
	}
}

/** A ball round everything a head is made of, in the head's own units. */
static void watchMeasureHead(struct modeldef *headdef)
{
	struct coord min = { 0, 0, 0 };
	struct coord max = { 0, 0, 0 };
	s32 any = 0;

	for (struct modelnode *node = headdef->rootnode; node; node = watchNextNode(node)) {
		if ((node->type & 0xff) != MODELNODETYPE_DL || !node->rodata || !node->rodata->dl.vertices) {
			continue;
		}

		for (s32 i = 0; i < node->rodata->dl.numvertices; i++) {
			const Vtx *v = &node->rodata->dl.vertices[i];

			for (s32 k = 0; k < 3; k++) {
				if (!any || v->v[k] < min.f[k]) {
					min.f[k] = v->v[k];
				}

				if (!any || v->v[k] > max.f[k]) {
					max.f[k] = v->v[k];
				}
			}

			any = 1;
		}
	}

	g_Watch.hashead = any;
	g_Watch.headmid.x = (min.x + max.x) * 0.5f;
	g_Watch.headmid.y = (min.y + max.y) * 0.5f;
	g_Watch.headmid.z = (min.z + max.z) * 0.5f;
	g_Watch.headradius = 0.5f * sqrtf((max.x - min.x) * (max.x - min.x)
			+ (max.y - min.y) * (max.y - min.y) + (max.z - min.z) * (max.z - min.z));
}

/**
 * The watch model's hour, minute and second hands, found in its mesh.
 *
 * GoldenEye's floating arm has a joint for each hand and the clock turns the
 * joints. Its watch *item* is one list with the hands drawn into it at twelve,
 * so there is nothing to turn but the vertices. They are told from the rest by
 * where they are - over the dial and inside the glass's ring, which nothing
 * else is - and from each other by lying each at a height of its own: the
 * second hand is the thin one, and the hour hand the shorter of the other two.
 * A mesh that does not come apart this way keeps its hands at twelve.
 */
static void watchFindMeshHands(struct modeldef *def)
{
	struct modelnode *node = def->rootnode;
	struct modelrodata_dl *dl = NULL;
	s16 level[3];
	s16 index[3][WATCH_HAND_VTX];
	s32 count[3] = { 0, 0, 0 };
	f32 reach[3] = { 0, 0, 0 };
	f32 width[3] = { 0, 0, 0 };
	s32 numlevels = 0;
	s32 order[3];

	g_Watch.handvtx = NULL;
	g_Watch.numhandvtx[0] = g_Watch.numhandvtx[1] = g_Watch.numhandvtx[2] = 0;

	for (; node && !dl; node = watchNextNode(node)) {
		if ((node->type & 0xff) == MODELNODETYPE_DL && node->rodata && node->rodata->dl.vertices) {
			dl = &node->rodata->dl;
		}
	}

	if (!dl) {
		return;
	}

	for (s32 i = 0; i < dl->numvertices; i++) {
		const Vtx *v = &dl->vertices[i];
		const f32 dx = v->x - WATCH_DIAL_X;
		const f32 dy = v->y - WATCH_DIAL_Y;
		s32 l;

		if (v->z < WATCH_DIAL_Z + WATCH_HAND_OVER || dx * dx + dy * dy > WATCH_HAND_REACH * WATCH_HAND_REACH) {
			continue;
		}

		for (l = 0; l < numlevels && level[l] != v->z; l++);

		if (l == numlevels) {
			if (numlevels == 3) {
				return;
			}

			level[numlevels++] = v->z;
		}

		if (count[l] == WATCH_HAND_VTX) {
			return;
		}

		index[l][count[l]++] = i;

		if (dx * dx + dy * dy > reach[l] * reach[l]) {
			reach[l] = sqrtf(dx * dx + dy * dy);
		}

		if (fabsf(dx) > width[l]) {
			width[l] = fabsf(dx);
		}
	}

	if (numlevels != 3) {
		return;
	}

	// the second hand is the thinnest, and the hour hand the shorter of the rest
	order[2] = width[0] <= width[1] && width[0] <= width[2] ? 0 : (width[1] <= width[2] ? 1 : 2);
	order[0] = (order[2] + 1) % 3;
	order[1] = (order[2] + 2) % 3;

	if (reach[order[0]] > reach[order[1]]) {
		const s32 tmp = order[0];

		order[0] = order[1];
		order[1] = tmp;
	}

	for (s32 h = 0; h < 3; h++) {
		const s32 l = order[h];

		g_Watch.numhandvtx[h] = count[l];

		for (s32 i = 0; i < count[l]; i++) {
			g_Watch.handindex[h][i] = index[l][i];
			g_Watch.handxy[h][i][0] = dl->vertices[index[l][i]].x;
			g_Watch.handxy[h][i][1] = dl->vertices[index[l][i]].y;
		}
	}

	g_Watch.handvtx = dl->vertices;
}

/**
 * The three hands turned to the clock. The angles are the ones the floating
 * arm's joints take, which are negative the way the hands go round; in the
 * mesh the dial looks out along +z with twelve at +y and three at +x, so a
 * hand goes from +y towards +x. Each is turned from where it was drawn.
 */
static void watchTurnMeshHands(f32 hours, f32 minutes, f32 seconds)
{
	if (!g_Watch.handvtx) {
		return;
	}

	for (s32 h = 0; h < 3; h++) {
		const f32 turn = -(h == 0 ? hours : (h == 1 ? minutes : seconds));
		const f32 c = cosf(turn);
		const f32 sn = sinf(turn);

		for (s32 i = 0; i < g_Watch.numhandvtx[h]; i++) {
			const f32 dx = g_Watch.handxy[h][i][0] - WATCH_DIAL_X;
			const f32 dy = g_Watch.handxy[h][i][1] - WATCH_DIAL_Y;
			Vtx *v = &g_Watch.handvtx[g_Watch.handindex[h][i]];

			v->x = (s16)lroundf(WATCH_DIAL_X + dx * c + dy * sn);
			v->y = (s16)lroundf(WATCH_DIAL_Y - dx * sn + dy * c);
		}
	}
}

/**
 * GoldenEye's own watch, `GwatchidentifierZ`, converted out of its hand item
 * table (geconvert.c). The player's own body has no watch on it, so this is
 * what goes on its wrist.
 */
static s32 watchLoadWatchModel(void)
{
	const s32 fileid = romdataRegisterModFile("Igx056Z", g_Watch.moddir);
	s32 size;

	if (fileid <= 0) {
		return 0;
	}

	size = fileGetInflatedSize(fileid, LOADTYPE_MODEL);

	if (size <= 0) {
		return 0;
	}

	g_Watch.watchbuflen = ALIGN64(size) + 0x20000;
	g_Watch.watchbuf = sysMemZeroAlloc(g_Watch.watchbuflen);

	if (!g_Watch.watchbuf) {
		return 0;
	}

	g_Watch.watchdef = modeldefLoad(fileid, g_Watch.watchbuf, g_Watch.watchbuflen, NULL);

	if (!g_Watch.watchdef) {
		return 0;
	}

	watchFixRenderModes(g_Watch.watchdef);
	modelAllocateRwData(g_Watch.watchdef);
	g_Watch.watchmodel = modelmgrInstantiateModelWithoutAnim(g_Watch.watchdef);

	if (!g_Watch.watchmodel) {
		return 0;
	}

	modelSetScale(g_Watch.watchmodel, 1.0f);
	watchFindMeshHands(g_Watch.watchdef);

	return 1;
}

/**
 * The player's own body, posed by the same animation GoldenEye poses its
 * floating arm with - `bond_watch` is a character animation and the floating
 * arm is one of GoldenEye's own characters (41), so the two skeletons are the
 * same one and the animation reads on either.
 *
 * What is drawn is the body it would draw in third person, so the sleeve and
 * the hand are the player's own, and it is the *whole* of it, head and all:
 * whatever of it the move to the eye brings into the picture is a person
 * rather than a pair of shoulders ending in a neck. It is built by the game's
 * own body0f02ce8c(), as the player's third person body is.
 *
 * The head is a copy of its own, loaded into memory of the watch's own rather
 * than out of the stage pool. One head's definition cannot sit on two bodies -
 * modelAttachHead() re-parents it to whichever body took it last, and the
 * level's own people may be wearing the same head (chrs-and-memory.md) - so
 * this is what a match does for every player, offset for the body and all.
 */
static s32 watchLoadBody(void)
{
	struct modeldef *headdef = NULL;
	s32 bodynum = -1;
	s32 headnum = -1;
	s32 perfecthead = false;

	playerChooseBodyAndHead(&bodynum, &headnum, &perfecthead);

	if (bodynum < 0 || bodynum >= NUM_HEADSANDBODIES) {
		return 0;
	}

	bodyLoad(bodynum);

	if (!g_HeadsAndBodies[bodynum].modeldef) {
		return 0;
	}

	// a body with a head of its own takes none, and neither does a head that
	// is not a row of the table (a match's camera heads)
	if (g_HeadsAndBodies[bodynum].unk00_01 || perfecthead || headnum <= 0 || headnum >= NUM_HEADSANDBODIES) {
		headnum = 0;
	}

	if (headnum > 0) {
		const s32 fileid = g_HeadsAndBodies[headnum].filenum;
		const s32 size = fileGetInflatedSize(fileid, LOADTYPE_MODEL);

		if (size > 0) {
			g_Watch.headbuflen = ALIGN64(size) + 0x20000;
			g_Watch.headbuf = sysMemZeroAlloc(g_Watch.headbuflen);
		}

		if (g_Watch.headbuf) {
			headdef = modeldefLoad(fileid, g_Watch.headbuf, g_Watch.headbuflen, NULL);
		}

		if (headdef) {
			g_FileInfo[fileid].loadedsize = 0;
			bodyCalculateHeadOffset(headdef, headnum, bodynum);

			// measured while it is still a tree of its own: once it is on,
			// its roots' parent is the body's headspot and a walk of it
			// carries on out through the rest of the body
			watchMeasureHead(headdef);
		} else {
			headnum = 0;
		}
	}

	g_Watch.modeldef = g_HeadsAndBodies[bodynum].modeldef;
	g_Watch.model = body0f02ce8c(bodynum, headnum, g_Watch.modeldef, headdef, false, NULL, true, false);

	if (!g_Watch.model) {
		g_Watch.modeldef = NULL;
		g_Watch.hashead = 0;
		return 0;
	}

	g_Watch.isbody = 1;
	g_Watch.chrscale = g_Watch.model->scale;

	return 1;
}

static s32 watchLoadGeArm(void)
{
	char name[16];
	s32 fileid;
	s32 size;

	snprintf(name, sizeof(name), "Cgx%03dZ", HAND_CHR);
	fileid = romdataRegisterModFile(name, g_Watch.moddir);

	if (fileid <= 0) {
		return 0;
	}

	size = fileGetInflatedSize(fileid, LOADTYPE_MODEL);

	if (size <= 0) {
		return 0;
	}

	// the loader takes the file's textures' room from the same buffer
	g_Watch.modelbuflen = ALIGN64(size) + 0x20000;
	g_Watch.modelbuf = sysMemZeroAlloc(g_Watch.modelbuflen);

	if (!g_Watch.modelbuf) {
		return 0;
	}

	g_Watch.modeldef = modeldefLoad(fileid, g_Watch.modelbuf, g_Watch.modelbuflen, NULL);

	if (!g_Watch.modeldef) {
		watchFreeModel();
		return 0;
	}

	watchFixRenderModes(g_Watch.modeldef);
	modelAllocateRwData(g_Watch.modeldef);
	g_Watch.model = modelmgrInstantiateModelWithAnim(g_Watch.modeldef);

	if (!g_Watch.model) {
		watchFreeModel();
		return 0;
	}

	g_Watch.chrscale = watchChrScale(HAND_CHR);
	modelSetScale(g_Watch.model, g_Watch.chrscale);

	return 1;
}

static void watchUnload(void)
{
	watchFreeModel();
	sysMemFree(g_Watch.options);
	sysMemFree(g_Watch.mpmenu);
	sysMemFree(g_Watch.brief);
	sysMemFree(g_Watch.lang);
	g_Watch.options = NULL;
	g_Watch.mpmenu = NULL;
	g_Watch.mpmenulen = 0;
	g_Watch.brief = NULL;
	g_Watch.lang = NULL;
	g_Watch.optionslen = 0;
	g_Watch.langlen = 0;
	g_Watch.loaded = 0;
	g_Watch.state = WS_CLOSED;
}

/**
 * A stage has loaded. In a GE Plus level the watch takes the arm, the fonts
 * and GoldenEye's own strings; anywhere else it lets go of them.
 */
void geWatchStageStart(s32 stagenum)
{
	const char *brief = NULL;
	const char *lang = NULL;
	s32 nameid = 0;
	u32 len = 0;

	watchUnload();

	g_Watch.stagenum = stagenum;
	g_Watch.mission = -1;

	// a converted level is the remake's whether the player reached it through
	// GE Plus or any other way, and GoldenEye's own pause belongs to the level
	// rather than to the menu that started it
	if (!modloaderStageIsRemake(stagenum)) {
		return;
	}

	g_Watch.moddir = modloaderGetStageModDirIndex(stagenum);

	if (g_Watch.moddir < 0 || !gexFrontLoadText()) {
		return;
	}

	g_Watch.options = watchLoad("LoptionsE", &g_Watch.optionslen);
	g_Watch.mpmenu = watchLoad("LmpmenuE", &g_Watch.mpmenulen);

	if (!g_Watch.options) {
		sysLogPrintf(LOG_WARNING, "gewatch: the conversion has no LoptionsE; GE Plus pauses Perfect Dark's way");
		watchUnload();
		return;
	}

	g_Watch.animnum = watchLoadAnim();

	// a converted mission's briefing, for the screen that shows it. An arena
	// has none, and its briefing screen is its objectives alone.
	g_Watch.mission = modloaderStageMission(stagenum);

	if (g_Watch.mission >= 0 && gexFrontMissionFiles(g_Watch.mission, &brief, &lang, &nameid)) {
		g_Watch.brief = watchLoad(brief, &len);
		g_Watch.lang = watchLoad(lang, &g_Watch.langlen);
	}

	g_Watch.loaded = 1;
	g_Watch.state = WS_CLOSED;
	g_Watch.page = PAGE_MISSION;
	g_Watch.selected = 0;
	g_Watch.confirm = 0;
	g_Watch.optionrow = 0;
	g_Watch.controlrow = 0;
	g_Watch.briefpage = BRIEF_OBJECTIVES;
	g_Watch.invrow = 0;
	g_Watch.invcursor = 0.5f;
	g_Watch.invtexty = 0;
	g_Watch.statuspulse = 0xff00a0u;
	g_Watch.statuswait = 0xa;
	g_Watch.objpulse = 0xffu;
	g_Watch.objwait = 0xa;
}

/* ---- the state machine -------------------------------------------------- */

s32 geWatchIsOpen(void)
{
	return g_Watch.loaded && g_Watch.state != WS_CLOSED;
}

/**
 * The view model is out of the player's hands from the moment the arm starts
 * up to the moment it is down again: GoldenEye's hand is holding the watch
 * rather than a gun for exactly those states.
 */
s32 geWatchHidesGun(void)
{
	return geWatchIsOpen() && g_Watch.state >= WS_TILT && g_Watch.state <= WS_LOWERARM;
}

s32 geWatchIsSettled(void)
{
	return geWatchIsOpen() && g_Watch.state == WS_OPEN;
}

// GoldenEye's own clock for the watch: the real frame, which keeps running
// while the level is frozen (speedgraphframes)
static f32 watchDelta(void)
{
	f32 d = g_Vars.diffframe60freal;

	return d > 0.0f ? (d > 10.0f ? 10.0f : d) : 1.0f;
}

static void watchSetState(s32 state)
{
	g_Watch.state = state;
	g_Watch.statetime = 0;
	g_Watch.timer = 0.0f;
}

/**
 * The watch's sounds, out of GoldenEye's own bank (gesfx.c), which has four:
 * CAMERA_BEEP1 for every move and every press (options.c's
 * sub_GAME_7F0A5210() and mpmenu.c's mpwatchPlayBeep()), WATCH_STATIC for the
 * interference that follows one press in thirty-two on the solo watch, and
 * WATCH_ON and WATCH_OFF as the view zooms to the face and away from it
 * (bondview2.c). Perfect Dark's menu sounds are played only where the
 * conversion has no bank: one made before version 39 whose ROM has gone.
 */
static void watchSfx(s32 id, s32 menusound)
{
	if (!geSfxPlay(id, GESFX_VOLUME) && geSfxGet(id) <= 0) {
		menuPlaySound(menusound);
	}
}

#define STATIC_CLEAR 0xe0

/** sub_GAME_7F0A51D8(): interference - the face dims and the static plays. */
static void watchStrikeStatic(void)
{
	g_Watch.bggreen = 0x80;

	if (geSfxGet(GESFX_WATCH_STATIC) > 0) {
		geSfxPlay(GESFX_WATCH_STATIC, GESFX_VOLUME);
	}
}

static void watchBeep(void)
{
	watchSfx(GESFX_CAMERA_BEEP1, MENUSOUND_FOCUS);

	// D_80040B10 is 0xf800: static when a random word is over 0xf8000000,
	// and only on the solo watch, the multiplayer one having a beep alone
	if (g_Vars.mplayerisrunning == 0 && (rngRandom() >> 27) == 0x1f) {
		watchStrikeStatic();
	}
}

// a press is the same beep as a move in GoldenEye
static void watchPlaySelect(void)
{
	watchBeep();
}

/**
 * The solo watch's interference, the end of options.c's sub_GAME_7F0A6A80().
 * Every frame GoldenEye rolls a random word against D_80040B0C (0xffa0 of
 * 0x10000, so one frame in 683 - every twenty seconds or so) and static
 * strikes: the face's green drops from 0xe0 to 0x80 and climbs back by a
 * random 0 to 3 a frame, about two seconds, and while it is under 0xe0 the
 * face is drawn as snow - tested against noise, with the green as how much
 * of it survives - and a scanline climbs it four units a frame, wrapping at
 * the edge of the green.
 *
 * GoldenEye's frame on the watch is two sixtieths, and this is ticked in
 * sixtieths: the scanline moves each one, by half as far, and the roll and the
 * climb are every other one.
 */
static void watchTickStatic(void)
{
	g_Watch.staticacc += watchDelta();

	if (g_Watch.staticacc > 8.0f) {
		g_Watch.staticacc = 8.0f;
	}

	while (g_Watch.staticacc >= 1.0f) {
		g_Watch.staticacc -= 1.0f;

		// D_80040B14 by D_80040B1C: the inventory's gun, 2.5 degrees a sixtieth
		g_Watch.gunangle += 2.5f * M_BADTAU / 360.0f;

		if (g_Watch.gunangle >= M_BADTAU) {
			g_Watch.gunangle -= M_BADTAU;
		}

		g_Watch.scany -= 2;

		if (g_Watch.scany >= 0x157) {
			g_Watch.scany = -0x156;
		}

		if (g_Watch.scany < -0x156) {
			g_Watch.scany = 0x156;
		}

		g_Watch.statichalf ^= 1;

		if (g_Watch.statichalf) {
			continue;
		}

		// sub_GAME_7F0A9684(): the controller turns by the stick while the
		// player has hold of the control page's second row, and a hundred
		// frames after they let go of it eases back to rest
		{
			const s32 held = g_Watch.page == PAGE_CONTROL && g_Watch.selected && g_Watch.controlrow == 1;
			const s32 stickx = joyGetStickX(0);

			if (held && (stickx >= 10 || stickx < -9)) {
				g_Watch.padidle = 0;
			} else if (g_Watch.padidle < 100) {
				g_Watch.padidle++;
			}

			if (g_Watch.padidle >= 100) {
				g_Watch.padspeed += (-g_Watch.padspin / 10.0f - g_Watch.padspeed) / 4.0f;
			} else if (held) {
				g_Watch.padspeed += (-(f32)stickx * 0.2f * M_BADTAU / 360.0f - g_Watch.padspeed) / 4.0f;
			}

			g_Watch.padspin += g_Watch.padspeed * 2.0f * 0.5f;

			if (g_Watch.padspin > M_BADPI) {
				g_Watch.padspin -= M_BADTAU;
			} else if (g_Watch.padspin < -M_BADPI) {
				g_Watch.padspin += M_BADTAU;
			}
		}

		// the controller tips towards the player by the stick's up and down
		// while they have hold of it, and back when they let go
		{
			const s32 held = g_Watch.page == PAGE_CONTROL && g_Watch.selected && g_Watch.controlrow == 1;
			const f32 target = held ? (f32)joyGetStickY(0) * M_BADTAU / 360.0f : 0.0f;

			g_Watch.padpitch += (target - g_Watch.padpitch) / 4.0f;
		}

		// INCOMPLETE's pulse: sixteen frames bright, then down a sixteenth of
		// green a frame to 0x5f and back to the top (D_80040AF4 and
		// D_80040AF8); the objectives' own does the same in its red
		// (D_80040AFC, D_80040B00)
		if (g_Watch.statuswait < 0) {
			g_Watch.statuspulse -= 0x00100000;
		}

		g_Watch.statuswait--;

		if (g_Watch.statuspulse < 0x5f00a1u) {
			g_Watch.statuspulse = 0xff00a0u;
			g_Watch.statuswait = 0xf;
		}

		if (g_Watch.objwait < 0) {
			g_Watch.objpulse -= 0x10;
		}

		g_Watch.objwait--;

		if (g_Watch.objpulse < 0x60u) {
			g_Watch.objpulse = 0xffu;
			g_Watch.objwait = 0xf;
		}

		if (rngRandom() > (0xffa0u << 16)) {
			watchStrikeStatic();
		}

		if (g_Watch.bggreen < STATIC_CLEAR) {
			g_Watch.bggreen += rngRandom() >> 30;
		}

		if (g_Watch.bggreen > STATIC_CLEAR) {
			g_Watch.bggreen = STATIC_CLEAR;
		}
	}
}

/**
 * bondviewSetupPauseTransition() and bondviewStartPauseTransition(): the view
 * goes to -40 degrees on the way in and back to where the player was looking
 * on the way out, over a duration the size of the turn decides.
 */
static void watchStartTilt(s32 topause)
{
	f32 diff;

	if (topause) {
		g_Watch.tiltfrom = g_Vars.currentplayer->vv_verta;
		g_Watch.tiltto = -40.0f;
	} else {
		g_Watch.tiltfrom = g_Watch.tiltstart;
		g_Watch.tiltto = g_Vars.currentplayer->vv_verta;
	}

	diff = g_Watch.tiltfrom - g_Watch.tiltto;

	if (diff < 0.0f) {
		diff = -diff;
	}

	if (diff >= 60.0f) {
		g_Watch.tiltduration = (diff - 60.0f) * 0.5f + 60.0f;
	} else if (diff <= 0.0f) {
		g_Watch.tiltduration = 0.0f;
	} else {
		g_Watch.tiltduration = diff;
	}

	g_Watch.tilttime = 0.0f;
	g_Watch.tiltstate = topause ? 1 : 2;
}

static s32 watchTilting(void)
{
	return g_Watch.tiltstate != 0 && g_Watch.tiltstate != 3;
}

/** bondviewUpdatePauseTransition(): a cosine ease between the two angles. */
static void watchUpdateTilt(void)
{
	f32 frac, weight;

	if (g_Watch.tiltstate != 1 && g_Watch.tiltstate != 2) {
		return;
	}

	g_Watch.tilttime += watchDelta();

	if (g_Watch.tiltduration <= 0.0f || g_Watch.tilttime >= g_Watch.tiltduration) {
		g_Vars.currentplayer->vv_verta = g_Watch.tiltstate == 1 ? g_Watch.tiltto : g_Watch.tiltfrom;
		g_Watch.tiltstate = g_Watch.tiltstate == 1 ? 3 : 0;
		bmoveUpdateVerta();
		return;
	}

	frac = g_Watch.tilttime / g_Watch.tiltduration;
	weight = (1.0f - cosf(frac * M_PI)) * 0.5f;

	if (g_Watch.tiltstate == 1) {
		g_Vars.currentplayer->vv_verta = g_Watch.tiltfrom + (g_Watch.tiltto - g_Watch.tiltfrom) * weight;
	} else {
		g_Vars.currentplayer->vv_verta = g_Watch.tiltto + (g_Watch.tiltfrom - g_Watch.tiltto) * weight;
	}

	bmoveUpdateVerta();
}

/**
 * bondviewSetPauseWatchRelated() and bondviewStepWatchAnimation(): the arm's
 * twenty frames over a duration, up or down, and the model set to whatever
 * frame the counter has reached.
 */
static void watchStartArm(s32 up, f32 duration)
{
	if (duration <= 0.0f) {
		duration = 1.0f;
	}

	if (up) {
		g_Watch.armspeed = (ARM_FRAMES - g_Watch.armframe) / duration;
		g_Watch.armstep = 1;
	} else {
		g_Watch.armspeed = g_Watch.armframe / duration;
		g_Watch.armstep = 2;
	}
}

static void watchUpdateArm(void)
{
	if (g_Watch.armstep != 1 && g_Watch.armstep != 2) {
		return;
	}

	if (g_Watch.armstep == 1) {
		g_Watch.armframe += watchDelta() * g_Watch.armspeed;

		if (g_Watch.armframe >= ARM_FRAMES) {
			g_Watch.armframe = ARM_FRAMES;
			g_Watch.armstep = 3;
		}
	} else {
		g_Watch.armframe -= watchDelta() * g_Watch.armspeed;

		if (g_Watch.armframe <= 0.0f) {
			g_Watch.armframe = 0.0f;
			g_Watch.armstep = 0;
		}
	}

	if (g_Watch.model && g_Watch.model->anim) {
		modelSetAnimFrame2(g_Watch.model, g_Watch.armframe, 0.0f);
	}
}

/**
 * trigger_watch_zoom(): the view's own zoom, which the watch drives itself -
 * the level is frozen while it runs, so Perfect Dark's own playerUpdateZoom()
 * is not advancing it.
 */
static void watchZoomTo(f32 fovy, f32 duration)
{
	playerSetZoomFovY(fovy, duration > 0.0f ? duration : 1.0f);
}

static s32 watchZooming(void)
{
	return g_Vars.currentplayer->zoomintime < g_Vars.currentplayer->zoomintimemax;
}

static void watchUpdateZoom(void)
{
	struct player *player = g_Vars.currentplayer;

	if (player->zoomintime < player->zoomintimemax) {
		player->zoomintime += watchDelta();

		if (player->zoomintime > player->zoomintimemax) {
			player->zoomintime = player->zoomintimemax;
		}

		player->zoominfovy = player->zoominfovyold
			+ (player->zoomintime * (player->zoominfovynew - player->zoominfovyold)) / player->zoomintimemax;
	} else {
		player->zoomintime = player->zoomintimemax;
		player->zoominfovy = player->zoominfovynew;
	}

	playermgrSetFovY(player->zoominfovy);
	viSetFovY(player->zoominfovy);
}

/**
 * How much further out the view stands on a window wider than 4:3, so that the
 * arm and the hand are either side of the watch (the user, 2026-09-19 and again
 * 2026-09-26): 1 on 4:3, GoldenEye's own framing, and 11/5.9 by 16:9, which is
 * the whole watch with the cuff and the hand beside it. Every screen's zoom is
 * GoldenEye's times this, so the steps between the screens keep their ratios,
 * and the words and items shrink with the face (watchFaceShrink()).
 */
#define WATCHZOOM_WIDE         11.0f
#define WATCH_ASPECT_NARROW    (4.0f / 3.0f)
#define WATCH_ASPECT_WIDE      (16.0f / 9.0f)

static f32 watchWiden(void)
{
	const f32 t = (videoGetAspect() - WATCH_ASPECT_NARROW) / (WATCH_ASPECT_WIDE - WATCH_ASPECT_NARROW);

	// 4:3, give or take a pixel
	if (t <= 0.001f) {
		return 1.0f;
	}

	if (t >= 1.0f) {
		return WATCHZOOM_WIDE / WATCHZOOM2;
	}

	return (WATCHZOOM2 + (WATCHZOOM_WIDE - WATCHZOOM2) * t) / WATCHZOOM2;
}

/** Where each screen keeps the zoom (options.c's navigation functions). */
static f32 watchPageZoom(s32 page)
{
	f32 zoom = WATCHZOOM2;

	switch (page) {
	case PAGE_INVENTORY:
	case PAGE_BRIEFING:
		zoom = WATCHZOOM1;
		break;
	case PAGE_CONTROL:
	case PAGE_OPTIONS:
		zoom = WATCHZOOM3;
		break;
	}

	return watchWiden() == 1.0f ? zoom : zoom * watchWiden();
}

/**
 * The face's size on the screen against GoldenEye's at the same screen: 1 on
 * 4:3, and on a wider window the share of the view the face keeps under the
 * widened zoom. The words and items are laid out on GoldenEye's frame shrunk by
 * this about the middle of the view, which is where the face is.
 */
static f32 watchFaceShrink(void)
{
	const f32 w = watchWiden();
	const f32 fovy = g_Vars.currentplayer->zoominfovy;

	if (w == 1.0f || fovy <= 0.0f) {
		return 1.0f;
	}

	return tanf(fovy / w * (M_PI / 360.0f)) / tanf(fovy * (M_PI / 360.0f));
}

// bondviewZoomToWatchOnOpen() and bondviewZoomFromWatchOnExit(): the duration
// is the distance still to travel, at GoldenEye's own rate. The watch always
// opens on the mission status (sub_GAME_7F0A69A8()).
static void watchZoomIn(void)
{
	const f32 zoom = watchPageZoom(PAGE_MISSION);
	f32 f = ((zoom - g_Vars.currentplayer->zoominfovy) * 45.0f) / -54.1f;

	watchZoomTo(zoom, f < 0.0f ? -f : f);
}

static void watchZoomOut(void)
{
	f32 f = ((60.0f - g_Vars.currentplayer->zoominfovy) * 45.0f) / -54.1f;

	watchZoomTo(60.0f, f < 0.0f ? -f : f);
}

/**
 * check_watch_page_transistion_running(): a screen's zoom is still running,
 * and GoldenEye draws the face alone until it is done.
 */
static s32 watchPageTurning(void)
{
	return (g_Watch.state == WS_OPEN || g_Watch.state == WS_CLOSING) && watchZooming();
}

/** The level stops where GoldenEye's pausing_flag says it does. */
static void watchSetPaused(s32 paused)
{
	if (paused == g_Watch.paused) {
		return;
	}

	g_Watch.paused = paused;
	lvSetPaused(paused);

	// GoldenEye's watch theme over the level's, which comes back as the arm
	// goes down (its mission state 3, mp_music.c)
	if (paused && geMusicSequence(GEMUSIC_WATCH) >= 0) {
		musicStartTrackAsMenu(geMusicSequence(GEMUSIC_WATCH));
		g_Watch.music = 1;
	} else if (!paused && g_Watch.music) {
		musicEndMenu();
		g_Watch.music = 0;
	}
	g_Vars.currentplayer->pausemode = paused ? PAUSEMODE_PAUSED : PAUSEMODE_UNPAUSED;
}

/**
 * The gun goes away and comes back the way the player's own weapon switch
 * does it, which is Perfect Dark's own lowering and raising.
 */
static void watchPutGunAway(void)
{
	if (g_Watch.hadweapons) {
		return;
	}

	g_Watch.weapons[HAND_RIGHT] = bgunGetWeaponNum(HAND_RIGHT);
	g_Watch.weapons[HAND_LEFT] = bgunGetWeaponNum(HAND_LEFT);
	g_Watch.hadweapons = 1;

	// its rounds as the hand has them, before the lowering empties it
	g_Watch.ammoweapon = g_Watch.weapons[HAND_RIGHT];

	if (!geHudWatchAmmo(g_Watch.ammoweapon, &g_Watch.ammomag, &g_Watch.ammoreserve)) {
		g_Watch.ammoweapon = -1;
	}

	bgunEquipWeapon2(HAND_RIGHT, WEAPON_UNARMED);
	bgunEquipWeapon2(HAND_LEFT, WEAPON_NONE);
}

static void watchTakeGunBack(void)
{
	if (!g_Watch.hadweapons) {
		return;
	}

	g_Watch.hadweapons = 0;

	// the hands as they were, pair or single: bgunEquipWeapon2(HAND_LEFT,
	// WEAPON_NONE) only leaves the left hand undecided, and the switch then
	// doubles any gun the inventory holds two of (a single KF7 came back
	// as a pair when a second had been picked up)
	bgunEquipHands(g_Watch.weapons[HAND_RIGHT], g_Watch.weapons[HAND_LEFT]);
}

/**
 * Start in a level. GoldenEye's trigger_solo_watch_menu(): from closed it
 * starts the watch coming up, and from any of the four steps it turns that
 * step round and puts it away again.
 */
/**
 * The arm, loaded the first time the watch comes up rather than at the stage
 * load: modelmgrAllocateSlots() runs later in setupLoadFiles() than the
 * watch's own start, so there is no slot to instantiate into until the level
 * is up. The level is about to stop anyway.
 */
static s32 watchEnsureModel(void)
{
	if (g_Watch.model) {
		return 1;
	}

	// GoldenEye's own arm, which is GoldenEye's own way: its suit hand with the
	// watch built into it, a joint for each hand of the clock and a cuff for
	// each of the missions' outfits. It was passed over for a day in favour of
	// the player's own body because it drew as a pale sleeve and an icy hand
	// round a white dial - which was the conversion writing its render mode
	// wrong and not the arm (watchFixRenderModes()). The player's own body with
	// GoldenEye's watch on its wrist is still here, for when the conversion has
	// no arm and behind g_WatchOwnBody.
	if (!g_WatchOwnBody && watchLoadGeArm()) {
		return 1;
	}

	watchFreeModel();

	if (watchLoadWatchModel() && watchLoadBody()) {
		return 1;
	}

	watchFreeModel();

	if (g_WatchOwnBody && watchLoadGeArm()) {
		return 1;
	}

	watchFreeModel();
	sysLogPrintf(LOG_WARNING, "gewatch: the conversion has no %s; GE Plus pauses Perfect Dark's way", "Cgx041Z");

	return 0;
}

s32 geWatchPause(void)
{
	if (!g_Watch.loaded) {
		return 0;
	}

	// a match's pause is GoldenEye's own multiplayer overlay rather than the
	// arm, and every player works their own - but only in a match GE Plus
	// started: the Combat Simulator's own, on a GoldenEye arena or not, keeps
	// Perfect Dark's pause dialog (the user, after F3 20260926-052358)
	if (watchIsMp()) {
		const s32 num = g_Vars.currentplayernum;

		if (!gexFrontIsInside()) {
			return 0;
		}

		if (g_MpWatch[num].on) {
			g_MpWatch[num].on = 0;
			g_MpWatch[num].confirm = 0;

			if (mpIsPaused() && g_MpWatchPauser == num) {
				g_MpWatchPauser = -1;
				mpSetPaused(MPPAUSEMODE_UNPAUSED);
			}
		} else {
			g_MpWatch[num].on = 1;
			g_MpWatch[num].mode = MPPAGE_SCORES;
			g_MpWatch[num].confirm = 0;
		}

		watchBeep();

		return 1;
	}

	if (!watchEnsureModel()) {
		return 0;
	}

	switch (g_Watch.state) {
	case WS_CLOSED:
		g_Watch.tiltstart = g_Vars.currentplayer->vv_verta;

		// sub_GAME_7F0A69A8(): every time it comes up the watch is on the
		// mission status, with the briefing on its objectives and nothing held
		g_Watch.page = PAGE_MISSION;
		g_Watch.briefpage = BRIEF_OBJECTIVES;
		g_Watch.selected = 0;
		g_Watch.confirm = 0;
		g_Watch.lrready = 0;
		g_Watch.yready = 0;
		watchSetState(WS_LOWER);

		if (g_Watch.model && g_Watch.animnum >= 0) {
			modelSetAnimation(g_Watch.model, g_Watch.animnum, 0, 0.0f, 0.5f, 0.0f);
			modelSetAnimFrame2(g_Watch.model, 0.0f, 0.0f);
		}

		g_Watch.armframe = 0.0f;
		g_Watch.armstep = 0;
		break;
	case WS_LOWER:
	case WS_TILT:
		// the arm never started: take the gun back where it stands
		watchSetState(WS_RESTORE);
		break;
	case WS_RAISE:
		watchSetState(WS_LOWERARM);
		break;
	case WS_ZOOMIN:
		watchSetState(WS_ZOOMOUT);
		break;
	case WS_OPEN:
		watchSetState(WS_CLOSING);
		break;
	}

	return 1;
}

/* ---- the screens' own input --------------------------------------------- */

/**
 * GoldenEye's stick, as the watch reads it (options.c): a move sideways or up
 * and down counts once and then not again until the stick has been let back
 * to the middle (controlstick_lr_enabled, watch_stick_y_nav_ready), which
 * sub_GAME_7F0A6A80() re-arms every frame the stick is within ten of it.
 */
static void watchTickLatches(void)
{
	const s32 x = joyGetStickX(0);
	const s32 y = joyGetStickY(0);

	if (x >= -0xa && x < 0xb) {
		g_Watch.lrready = 1;
	}

	if (y >= -0xa && y < 0xb) {
		g_Watch.yready = 1;
	}
}

static s32 watchStickLeft(void)
{
	return joyGetStickX(0) < -0x2d && g_Watch.lrready;
}

static s32 watchStickRight(void)
{
	return joyGetStickX(0) >= 0x2e && g_Watch.lrready;
}

static s32 watchStickUp(void)
{
	return joyGetStickY(0) >= 0x2e && g_Watch.yready;
}

static s32 watchStickDown(void)
{
	return joyGetStickY(0) < -0x2d && g_Watch.yready;
}

static s32 watchPressedUp(void)
{
	return joyGetButtonsPressedThisFrame(0, U_JPAD | U_CBUTTONS) != 0 || watchStickUp();
}

static s32 watchPressedDown(void)
{
	return joyGetButtonsPressedThisFrame(0, D_JPAD | D_CBUTTONS) != 0 || watchStickDown();
}

static s32 watchPressedLeft(void)
{
	return joyGetButtonsPressedThisFrame(0, L_JPAD | L_CBUTTONS | L_TRIG) != 0 || watchStickLeft();
}

static s32 watchPressedRight(void)
{
	return joyGetButtonsPressedThisFrame(0, R_JPAD | R_CBUTTONS | R_TRIG) != 0 || watchStickRight();
}

static s32 watchPressedAccept(void)
{
	return joyGetButtonsPressedThisFrame(0, A_BUTTON | Z_TRIG | BUTTON_UI_ACCEPT) != 0
		|| inputKeyPressedThisFrame(VK_MOUSE_LEFT);
}

static s32 watchPressedBack(void)
{
	return joyGetButtonsPressedThisFrame(0, B_BUTTON | BUTTON_UI_CANCEL) != 0;
}

static s32 watchPressedStart(void)
{
	return joyGetButtonsPressedThisFrame(0, START_BUTTON) != 0 || inputKeyPressedThisFrame(VK_ESCAPE);
}

/**
 * sub_GAME_7F0A611C() and game_options_inventory_navigation(): a list with a
 * cursor that moves a whole row for a press and in tenths while a direction is
 * held or the stick is part way over, never past either end, and is drawn back
 * to the middle of its row once nothing is held. The text eases a third of the
 * way to the row a frame, and is settled when it is there.
 */
static void watchListNav(f32 *cursor, s32 *index, s32 count, s32 *texty, s32 top, s32 line, s32 *settled)
{
	const s32 sticky = joyGetStickY(0);
	s32 target;

	if (joyGetButtonsPressedThisFrame(0, U_JPAD | U_CBUTTONS) || sticky >= 0x47) {
		if ((s32)*cursor > 0) {
			*cursor -= 1.0f;
		}
	} else if (joyGetButtonsPressedThisFrame(0, D_JPAD | D_CBUTTONS) || sticky < -0x46) {
		if ((s32)*cursor < count - 1) {
			*cursor += 1.0f;
		}
	}

	if (joyGetButtons(0, U_JPAD | U_CBUTTONS)) {
		if ((s32)*cursor > 0) {
			*cursor -= 0.1f;
		}
	} else if (joyGetButtons(0, D_JPAD | D_CBUTTONS)) {
		if ((s32)*cursor < count - 1) {
			*cursor += 0.1f;
		}
	}

	if (sticky >= 0x1f && sticky < 0x46 && *index > 0) {
		*cursor -= (f32)sticky / 300.0f;
	} else if (sticky < -0x1e && sticky >= -0x45 && (s32)*cursor < count - 1) {
		*cursor -= (f32)sticky / 300.0f;
	}

	if (sticky >= 0x10 && !g_Watch.yactive && *index > 0) {
		*cursor -= 1.0f;
	} else if (sticky < -0xf && !g_Watch.yactive && *index < count - 1) {
		*cursor += 1.0f;
	}

	g_Watch.yactive = sticky >= 0x10 || sticky < -0xf;

	if (*cursor > (f32)count - 0.5f) {
		*cursor = (f32)count - 0.5f;
	}

	if (*cursor < -0.5f) {
		*cursor = -0.5f;
	}

	*index = (s32)*cursor;

	if (*index < 0) {
		*index = 0;
	}

	target = top - *index * line;

	if (target < *texty) {
		*texty = *texty - (*texty - target) / 3 - 1;
		*settled = 0;
	} else if (*texty < target) {
		*texty = *texty + (target - *texty) / 3 + 1;
		*settled = 0;
	} else {
		*settled = 1;
	}

	if (!joyGetButtons(0, 0xffff)) {
		if ((f32)*index + 0.55f < *cursor) {
			*cursor -= 0.1f;
		} else if (*cursor <= (f32)*index + 0.45f) {
			*cursor += 0.1f;
		}
	}
}

/**
 * The eight rows of the options screen under its two volume sliders
 * (game_options_entries): the label, its values, and Perfect Dark's own
 * setting behind it.
 */
struct watchoption {
	s32 label;
	s32 values[3];
	s32 numvalues;
};

static const struct watchoption g_Options[] = {
	{ STR_LOOKUPDOWN,     { STR_REVERSE, STR_UPRIGHT, 0 },       2 },
	{ STR_AUTOAIM,        { STR_OFF, STR_ON, 0 },                2 },
	{ STR_AIMCONTROL,     { STR_HOLD, STR_TOGGLE, 0 },           2 },
	{ STR_SIGHTONSCREEN,  { STR_OFF, STR_ON, 0 },                2 },
	{ STR_LOOKAHEAD,      { STR_OFF, STR_ON, 0 },                2 },
	{ STR_AMMOONSCREEN,   { STR_OFF, STR_ON, 0 },                2 },
	{ STR_SCREEN,         { STR_FULL, STR_WIDE, STR_CINEMA },    3 },
	{ STR_RATIO,          { STR_NORMAL, STR_169, 0 },            2 },
};

#define NUM_OPTIONS ((s32)(sizeof(g_Options) / sizeof(g_Options[0])))

static s32 watchOptionValue(s32 row)
{
	const s32 num = g_Vars.currentplayerstats ? g_Vars.currentplayerstats->mpindex : 0;

	switch (row) {
	case 0: return optionsGetForwardPitch(num) ? 0 : 1;
	case 1: return optionsGetAutoAim(num) ? 1 : 0;
	case 2: return optionsGetAimControl(num) ? 1 : 0;
	case 3: return optionsGetSightOnScreen(num) ? 1 : 0;
	case 4: return optionsGetLookAhead(num) ? 1 : 0;
	case 5: return optionsGetAmmoOnScreen(num) ? 1 : 0;
	case 6: return optionsGetScreenSize();
	case 7: return optionsGetScreenRatio();
	}

	return 0;
}

static void watchSetOptionValue(s32 row, s32 value)
{
	const s32 num = g_Vars.currentplayerstats ? g_Vars.currentplayerstats->mpindex : 0;

	switch (row) {
	case 0: optionsSetForwardPitch(num, value == 0); break;
	case 1: optionsSetAutoAim(num, value != 0); break;
	case 2: optionsSetAimControl(num, value); break;
	case 3: optionsSetSightOnScreen(num, value != 0); break;
	case 4: optionsSetLookAhead(num, value != 0); break;
	case 5: optionsSetAmmoOnScreen(num, value != 0); break;
	case 6: optionsSetScreenSize(value); break;
	case 7: optionsSetScreenRatio(value); break;
	}

	g_Vars.modifiedfiles |= MODFILE_GAME;
}

/**
 * The two sliders (watch_adjust_volume_slider()): while one is held, a
 * direction held moves it WATCH_VOL_ADJUST_STEP a frame and the stick moves it
 * by how far it is pushed. GoldenEye's scale is 0x7fff and Perfect Dark's
 * 0x5000, so its steps are taken at the same share of the whole.
 */
#define VOL_MAX   0x5000
#define VOL_SCALE ((f32)VOL_MAX / 32767.0f)

static void watchAdjustVolume(s32 row)
{
	s32 v = row == 0 ? (s32)optionsGetMusicVolume() : (s32)VOLUME(g_SfxVolume);
	const s32 old = v;
	s32 x = joyGetStickX(0);

	if (joyGetButtons(0, R_CBUTTONS | R_TRIG | R_JPAD)) {
		v += (s32)(1024 * VOL_SCALE);
	} else if (joyGetButtons(0, L_CBUTTONS | L_TRIG | L_JPAD)) {
		v -= (s32)(1024 * VOL_SCALE);
	}

	if (x >= 0x47) {
		x = 0x46;
	} else if (x < -0x46) {
		x = -0x46;
	}

	if (x >= 8) {
		v += (s32)((x * 0x800 - 0x3800) / 0x46 * VOL_SCALE);
	} else if (x < -7) {
		v += (s32)((x * 0x800 + 0x3800) / 0x46 * VOL_SCALE);
	}

	if (v < 0) {
		v = 0;
	} else if (v > VOL_MAX) {
		v = VOL_MAX;
	}

	if (v == old) {
		return;
	}

	if (row == 0) {
		optionsSetMusicVolume((u16)v);
	} else {
		sndSetSfxVolume((u16)v);
	}

	g_Vars.modifiedfiles |= MODFILE_GAME;
}

/**
 * The control styles the control screen lists: GoldenEye's four one-pad
 * styles, its four two-pad ones when a second pad is in (D_800409D8), and
 * last Perfect Dark's own mouse and keyboard, which GoldenEye has no name for.
 */
static s32 watchStyleCount(void)
{
	const s32 num = g_Vars.currentplayerstats ? g_Vars.currentplayerstats->mpindex : 0;
	const s32 mode = optionsGetControlMode(num);
	const s32 two = (joyGetConnectedControllers() & 2) || (mode >= CONTROLMODE_21 && mode <= CONTROLMODE_24);

	return (two ? 8 : 4) + 1;
}

static s32 watchStyleMode(s32 index)
{
	return index == watchStyleCount() - 1 ? CONTROLMODE_PC : index;
}

static s32 watchStyleIndex(s32 mode)
{
	return mode == CONTROLMODE_PC || mode >= watchStyleCount() - 1 ? watchStyleCount() - 1 : mode;
}

/** The mission status screen's abort, which is GoldenEye's own way out. */
static void watchAbort(void)
{
	watchSetPaused(0);
	watchTakeGunBack();
	watchSetState(WS_CLOSED);
	g_Vars.currentplayer->aborted = true;
	mainEndStage();
}

// a screen turned: GoldenEye's beep, and its zoom to that screen's own
static void watchTurnPage(s32 page)
{
	const f32 zoom = watchPageZoom(page);
	const s32 from = g_Watch.page;

	g_Watch.page = page;
	g_Watch.lrready = 0;

	// sub_GAME_7F0A5210() beeps every turn but the one from the control screen
	// to the options, which only disarms the stick
	if (page != PAGE_OPTIONS || from != PAGE_CONTROL) {
		watchBeep();
	}

	if (g_Vars.currentplayer->zoominfovynew != zoom) {
		watchZoomTo(zoom, WATCH_PAGE_ZOOMTIME);
	}
}

/** sub_GAME_7F0A8378(): the inventory's A, Z or START takes the item under the cursor. */
static void watchInventoryTake(s32 start)
{
	const s32 weaponnum = invGetWeaponNumByIndex(g_Watch.invrow);

	if (weaponnum <= 0) {
		return;
	}

	// START takes it only if it is not the one already in the hand
	if (start && weaponnum == g_Watch.weapons[HAND_RIGHT]) {
		return;
	}

	invSetCurrentIndex(g_Watch.invrow);
	g_Watch.weapons[HAND_RIGHT] = weaponnum;
	g_Watch.weapons[HAND_LEFT] = WEAPON_NONE;
	g_Watch.invflash = 10;
	watchSfx(GESFX_CAMERA_BEEP1, MENUSOUND_FOCUS);
}

/**
 * sub_GAME_7F0A6A80() and draw_watch_current_page()'s input, screen by screen.
 * START closes the watch; left and right (the pad's, the shoulder buttons, the
 * C buttons or the stick) turn the screens when nothing is held; A or Z holds
 * the row under the cursor and lets it go again, on every screen but the
 * inventory, where it takes the item. B is the port's own: it lets go of a
 * held row, or closes the watch as START does.
 */
static void watchTickInput(void)
{
	const s32 settled = !watchPageTurning();
	const s32 num = g_Vars.currentplayerstats ? g_Vars.currentplayerstats->mpindex : 0;

	watchTickLatches();

	if (watchPressedStart() || (watchPressedBack() && !g_Watch.selected)) {
		if (g_Watch.page == PAGE_INVENTORY && settled && g_Watch.invsettled && watchPressedStart()) {
			watchInventoryTake(1);
		}

		watchSetState(WS_CLOSING);
		return;
	}

	if (watchPressedBack()) {
		g_Watch.selected = 0;
		g_Watch.confirm = 0;
		watchBeep();
		return;
	}

	switch (g_Watch.page) {
	case PAGE_MISSION:
		if (g_Watch.selected) {
			// held, not pressed: the stick or a right button to CONFIRM, the
			// left ones back to CANCEL (draw_abort_cancel_confirm())
			if (!g_Watch.confirm && (joyGetStickX(0) >= 0x2e || joyGetButtons(0, R_JPAD | R_TRIG | R_CBUTTONS))) {
				g_Watch.confirm = 1;
			} else if (g_Watch.confirm && (joyGetStickX(0) < -0x2d || joyGetButtons(0, L_JPAD | L_TRIG | L_CBUTTONS))) {
				g_Watch.confirm = 0;
			}

			if (g_Watch.confirm && joyGetButtonsPressedThisFrame(0, Z_TRIG | A_BUTTON | BUTTON_UI_ACCEPT)) {
				watchAbort();
				return;
			}
		}
		break;
	case PAGE_INVENTORY:
		if (settled && invGetCount() > 0) {
			if (!g_Watch.selected) {
				watchListNav(&g_Watch.invcursor, &g_Watch.invrow, invGetCount(), &g_Watch.invtexty,
						2 * INV_LINE, INV_LINE, &g_Watch.invsettled);
			}

			if (g_Watch.invflash > 0) {
				g_Watch.invflash--;
			}

			if (g_Watch.invsettled && watchPressedAccept()) {
				watchInventoryTake(0);
			}
		}
		break;
	case PAGE_CONTROL:
		if (!g_Watch.selected) {
			// two rows, so up and down both go to the other one
			if (watchPressedUp() || watchPressedDown()) {
				g_Watch.controlrow ^= 1;
				g_Watch.yready = 0;
			}
		} else if (g_Watch.controlrow == 0) {
			const s32 before = g_Watch.styleindex;

			watchListNav(&g_Watch.stylecursor, &g_Watch.styleindex, watchStyleCount(), &g_Watch.styletexty,
					0, 10, &g_Watch.stylesettled);

			if (g_Watch.styleindex != before) {
				optionsSetControlMode(num, watchStyleMode(g_Watch.styleindex));
				g_Vars.modifiedfiles |= MODFILE_GAME;
			}
		}
		break;
	case PAGE_OPTIONS:
		if (g_Watch.optionrow < 2) {
			if (watchPressedUp() || watchPressedDown()) {
				g_Watch.optionrow = watchPressedUp() ? (g_Watch.optionrow == 0 ? 9 : 0) : (g_Watch.optionrow == 0 ? 1 : 2);
				g_Watch.yready = 0;
				g_Watch.selected = 0;
			} else if (g_Watch.selected) {
				watchAdjustVolume(g_Watch.optionrow);
			}
		} else {
			if (watchPressedUp() || watchPressedDown()) {
				g_Watch.optionrow += watchPressedUp() ? -1 : 1;
				g_Watch.yready = 0;
				g_Watch.selected = 0;

				if (g_Watch.optionrow >= 10) {
					g_Watch.optionrow = 0;
				}
			} else if (g_Watch.selected) {
				// game_option_toggle_input(): a value to either side, and no
				// further than the ends
				const s32 i = g_Watch.optionrow - 2;
				const s32 v = watchOptionValue(i);

				if (watchPressedLeft() && v > 0) {
					watchSetOptionValue(i, v - 1);
					g_Watch.lrready = 0;
					watchSfx(GESFX_OPTION_CHOOSE, MENUSOUND_FOCUS);
				} else if (watchPressedRight() && v < g_Options[i].numvalues - 1) {
					watchSetOptionValue(i, v + 1);
					g_Watch.lrready = 0;
					watchSfx(GESFX_OPTION_CHOOSE, MENUSOUND_FOCUS);
				}
			}
		}
		break;
	case PAGE_BRIEFING:
		if (watchPressedUp() || watchPressedDown()) {
			g_Watch.briefpage += watchPressedUp() ? -1 : 1;
			g_Watch.briefpage = (g_Watch.briefpage + NUM_BRIEF_PAGES) % NUM_BRIEF_PAGES;
			g_Watch.yready = 0;
			g_Watch.selected = 0;
		}
		break;
	}

	// left and right turn the screens when nothing is held; on the control and
	// options screens not while Z is down either
	if (!g_Watch.selected && (watchPressedLeft() || watchPressedRight())
			&& !((g_Watch.page == PAGE_CONTROL || g_Watch.page == PAGE_OPTIONS) && joyGetButtons(0, Z_TRIG))) {
		const s32 right = watchPressedRight();
		const s32 from = g_Watch.page;
		const s32 page = (from + (right ? 1 : NUM_PAGES - 1)) % NUM_PAGES;

		if (page == PAGE_MISSION) {
			g_Watch.confirm = 0;
		} else if (page == PAGE_OPTIONS) {
			g_Watch.optionrow = 0;
		} else if (page == PAGE_CONTROL && from == PAGE_OPTIONS) {
			g_Watch.controlrow = 0;
		}

		watchTurnPage(page);
		return;
	}

	// A or Z holds the row and lets it go (watch_play_beep_sound(), which
	// beeps only as it takes hold)
	if (g_Watch.page != PAGE_INVENTORY && settled && watchPressedAccept()) {
		g_Watch.selected = !g_Watch.selected;

		if (g_Watch.selected) {
			watchSfx(GESFX_CAMERA_BEEP1, MENUSOUND_FOCUS);

			if (g_Watch.page == PAGE_CONTROL && g_Watch.controlrow == 0) {
				g_Watch.styleindex = watchStyleIndex(optionsGetControlMode(num));
				g_Watch.stylecursor = g_Watch.styleindex + 0.5f;
				g_Watch.styletexty = -g_Watch.styleindex * 10;
			}
		} else {
			g_Watch.confirm = 0;
		}
	}
}

/**
 * GoldenEye's bondviewWatchAnimationTick(), state for state. The level runs
 * through the first two steps - the gun is being put away and the view is
 * still the player's - and is frozen from the arm starting up until it is down
 * again, which is where GoldenEye's pausing_flag is set.
 */
void geWatchTick(void)
{
	if (!g_Watch.loaded) {
		return;
	}

	if (watchIsMp()) {
		watchMpTick();
		return;
	}

	if (g_Watch.state == WS_CLOSED) {
		return;
	}

	g_Watch.statetime++;
	g_Watch.timer += watchDelta();

	switch (g_Watch.state) {
	case WS_LOWER:
		if (g_Watch.statetime == 1) {
			watchPutGunAway();
		}

		// GoldenEye waits 17 frames for the hand to hold the watch rather than
		// the gun; here it waits for Perfect Dark's own switch to finish, and
		// gives up on the same count in case it never does.
		//
		// In third person it waits for the camera as well. The watch is a
		// first person thing - the arm is in front of the eye, not on the
		// body - so opening it asks for the eye the way aiming does
		// (playerIsThirdPerson()), and the arm does not start up until the
		// camera has arrived there.
		if (((bgunGetWeaponNum(HAND_RIGHT) == WEAPON_UNARMED && !bgunIsAnimBusy(&g_Vars.currentplayer->hands[HAND_RIGHT]))
					|| g_Watch.timer >= 17.0f)
				&& g_Vars.currentplayer->thirdpersondist <= 0.0f) {
			watchSetState(WS_TILT);
		}
		break;
	case WS_TILT:
		if (g_Watch.statetime == 1) {
			watchStartTilt(1);
		}

		if (g_Watch.tiltduration - g_Watch.tilttime < 30.0f) {
			watchSetState(WS_RAISE);
		}
		break;
	case WS_RAISE:
		if (g_Watch.statetime == 1) {
			watchSetPaused(1);
			watchStartArm(1, ARM_DURATION);
		}

		if (g_Watch.armstep == 3 && !watchTilting()) {
			watchSetState(WS_ZOOMIN);
		}
		break;
	case WS_ZOOMIN:
		if (g_Watch.statetime == 1) {
			watchSetPaused(1);
			watchZoomIn();
			watchSfx(GESFX_WATCH_ON, MENUSOUND_SELECT);
			g_Watch.bggreen = STATIC_CLEAR;
		}

		if (!watchZooming()) {
			watchSetState(WS_OPEN);
		}
		break;
	case WS_OPEN:
		// F3's Report a Problem over the watch has the pad while it is up,
		// and the press that closes it is not one for the watch as well
		if (!traceReportHoldsInput()) {
			watchTickInput();
		}

		watchTickStatic();
		break;
	case WS_CLOSING:
		if (g_Watch.statetime >= 3) {
			watchSetState(WS_ZOOMOUT);
			watchSfx(GESFX_WATCH_OFF, MENUSOUND_FOCUS);
		}
		break;
	case WS_ZOOMOUT:
		if (g_Watch.statetime == 1) {
			watchZoomOut();
		}

		if (!watchZooming()) {
			watchSetState(WS_LOWERARM);
		}
		break;
	case WS_LOWERARM:
		if (g_Watch.statetime == 1) {
			watchStartArm(0, ARM_DURATION);
			watchStartTilt(0);
		}

		if (g_Watch.armstep == 0) {
			watchSetState(WS_RESTORE);
		}
		break;
	case WS_RESTORE:
		if (g_Watch.statetime == 1) {
			watchSetPaused(0);
			watchTakeGunBack();

			// the view goes back to where the player was looking even when the
			// arm never came up, which is the two steps that turn round early
			if (!watchTilting()) {
				watchStartTilt(0);
			}
		}

		if (!watchTilting()) {
			watchSetState(WS_CLOSED);
			g_Watch.armframe = 0.0f;
			g_Watch.armstep = 0;
		}
		break;
	}

	watchUpdateTilt();
	watchUpdateArm();

	if (g_Watch.paused) {
		watchUpdateZoom();
	}

	if (g_Watch.state == WS_CLOSED) {
		// whatever the zoom was left at goes back to the player's own view
		playerSetZoomFovY(PLAYER_DEFAULT_FOV, 1.0f);
		watchUpdateZoom();
	}
}

/* ---- the face ----------------------------------------------------------- */

/**
 * hudMakeDamageSegments(): a gauge of 23 pairs of vertices round the side of
 * the face, blue for armour (`side` 1) and red for health (-1), each pair lit
 * as far as the value goes. GoldenEye's own numbers.
 */
static void watchGaugeVertices(Vtx *v, Col *c, s32 side, f32 value)
{
	s32 n = 0;
	s32 deg = 0;

	value *= 8.0f;

	for (s32 i = 0; i < GAUGE_PAIRS; i++) {
		const f32 angle = ((142.5f - (f32)deg) * M_PI * 2.0f) / 360.0f;

		for (s32 pair = 0; pair < 2; pair++) {
			const f32 s = sinf(angle) * 4.0f * 130.0f * (f32)(6 - pair) / 5.0f * (f32)side;
			const f32 t = cosf(angle) * 4.0f * 130.0f * (f32)(6 - pair) / 5.0f;
			s32 alpha;

			v[n].x = (s16)s + 1;
			v[n].y = 0;
			v[n].z = (s16)-(s32)t;
			v[n].flags = 0;
			v[n].colour = n * 4;
			v[n].s = 0;
			v[n].t = 0;

			if (side > 0) {
				c[n].r = (u8)(96.0f - cosf(angle) * 96.0f);
				c[n].g = (u8)(127.0f - cosf(angle) * 127.0f);
				c[n].b = 0xff;
			} else {
				c[n].r = 0xff;
				c[n].g = (u8)(127.0f - cosf(angle) * 127.0f);
				c[n].b = (u8)(32.0f - cosf(angle) * 32.0f);
			}

			if (i < 10) {
				if (((s32)value * 2) - 1 >= i) {
					alpha = 0xff;
				} else if (i < (s32)(2.0f * value)) {
					alpha = (s32)((value - (f32)(s32)value) * 207.0f) + 0x30;
				} else {
					alpha = 0x30;
				}
			} else {
				if ((f32)i <= 9.0f + (value - 5.0f) * 4.0f) {
					alpha = 0xff;
				} else if ((s32)((value - 5.0f) * 4.0f + 0.5f) + 9 >= i && ((s32)(value - 5.0f) * 2) + 8 < i) {
					alpha = (s32)((value - (f32)(s32)value) * 207.0f) + 0x30;
				} else {
					alpha = 0x30;
				}
			}

			c[n].a = (u8)alpha;
			n++;
		}

		deg += 5;
	}
}

/** buildGaugeBarDL(): the pairs joined into a bar. */
static Gfx *watchDrawGauge(Gfx *gdl, Vtx *v, Col *c)
{
	gDma1p(gdl++, G_COL, c, GAUGE_VERTICES * 4, (GAUGE_VERTICES - 1) << 2);

	for (s32 i = 0; i <= GAUGE_VERTICES / 2 - 2; i++) {
		gSPVertex(gdl++, v + i * 2, 4, 0);

		if (i >= 9) {
			if ((i + 3) % 4) {
				gSP1Triangle(gdl++, 0, 1, 2, 0);
				gSP1Triangle(gdl++, 1, 2, 3, 0);
			}
		} else if ((i & 1) == 0) {
			gSP1Triangle(gdl++, 0, 1, 2, 0);
			gSP1Triangle(gdl++, 1, 2, 3, 0);
		}
	}

	return gdl;
}

// the same two for GoldenEye's HUD, which draws them either side of the view
// when the player is hit (gehud.c)
void geWatchGaugeVertices(Vtx *v, Col *c, s32 side, f32 value)
{
	watchGaugeVertices(v, c, side, value);
}

Gfx *geWatchDrawGauge(Gfx *gdl, Vtx *v, Col *c)
{
	return watchDrawGauge(gdl, v, c);
}

/**
 * sub_GAME_7F0A33F8(): the face's disc, a ring of vertices at `scale` of
 * GoldenEye's 520, shaded from dark at the top to green at the bottom. With
 * `centre` it writes the middle vertex first, which the fan is drawn round.
 */
static s32 watchFaceVertices(Vtx *v, Col *c, s32 numverts, f32 scale, s32 centre, s32 alpha)
{
	s32 n = 0;

	if (centre) {
		v[n].x = 1;
		v[n].y = 0;
		v[n].z = 0;
		v[n].flags = 0;
		v[n].colour = n * 4;
		v[n].s = 0;
		v[n].t = 0;
		c[n].r = 0;
		c[n].g = 0x2c;
		c[n].b = 0;
		c[n].a = (u8)alpha;
		n++;
	}

	for (s32 i = 7; i <= numverts - 7; i += 2) {
		const f32 angle = ((f32)i * M_PI) / (f32)numverts;
		const s16 sinval = (s16)(sinf(angle) * FACE_RADIUS * scale);
		const s16 cosval = (s16)(cosf(angle) * FACE_RADIUS * scale);
		const u8 green = (u8)(44.0f - cosf(angle) * 20.0f);

		for (s32 side = 0; side < 2; side++) {
			if (side && (i <= 0 || i >= numverts)) {
				continue;
			}

			v[n].x = 1 + (side ? -sinval : sinval);
			v[n].y = 0;
			v[n].z = -cosval;
			v[n].flags = 0;
			v[n].colour = n * 4;
			v[n].s = 0;
			v[n].t = 0;
			c[n].r = 0;
			c[n].g = green;
			c[n].b = 0;
			c[n].a = (u8)alpha;
			n++;
		}
	}

	return n;
}

/** draw_watch_background(): the ring as strips, or the fill as a fan. */
static Gfx *watchDrawFace(Gfx *gdl, Vtx *v, Col *c, s32 n, s32 fan)
{
	gDma1p(gdl++, G_COL, c, n * 4, (n - 1) << 2);

	if (fan) {
		Vtx *ring = v + 1;

		gSPVertex(gdl++, ring + 14, 4, 0);
		gSPVertex(gdl++, v, 1, 4);
		gSP1Triangle(gdl++, 2, 4, 3, 0);

		for (s32 i = 7; i >= 0; i--) {
			gSPVertex(gdl++, ring + 2 * i, 4, 0);
			gSPVertex(gdl++, v, 1, 4);
			gSP1Triangle(gdl++, 0, 4, 2, 0);
			gSP1Triangle(gdl++, 1, 3, 4, 0);
		}

		gSP1Triangle(gdl++, 0, 1, 4, 0);
	} else {
		for (s32 i = 0; i < 8; i++) {
			gSPVertex(gdl++, v + i * 2, 4, 0);
			gSP1Triangle(gdl++, 0, 1, 2, 0);
			gSP1Triangle(gdl++, 1, 2, 3, 0);
		}
	}

	return gdl;
}

/**
 * setup_watch_rectangles(): the five screen-select rectangles under the face,
 * the open one lit and the rest dim.
 */
static Gfx *watchDrawSelect(Gfx *gdl)
{
	Vtx *v = gfxAllocateVertices(SELECT_RECTS * 4);
	Col *c = gfxAllocate(SELECT_RECTS * 4 * sizeof(Col));
	s32 n = 0;

	for (s32 r = 0; r < SELECT_RECTS; r++) {
		const s32 x0 = SELECT_LEFT + r * SELECT_HSTEP;

		for (s32 i = 0; i < 2; i++) {
			for (s32 j = 0; j < 2; j++) {
				v[n].x = (s16)(x0 + i * SELECT_WIDTH);
				v[n].y = 0;
				v[n].z = (s16)(SELECT_TOP + j * SELECT_HEIGHT);
				v[n].flags = 0;
				v[n].colour = n * 4;
				v[n].s = 0;
				v[n].t = 0;

				if (r == g_Watch.page) {
					c[n].r = g_Watch.selected ? 0x30 : 0x50;
					c[n].g = g_Watch.selected ? 0xa0 : 0xf0;
					c[n].b = g_Watch.selected ? 0x30 : 0x50;
				} else {
					c[n].r = 0x20;
					c[n].g = 0x70;
					c[n].b = 0x20;
				}

				c[n].a = 0xf0;
				n++;
			}
		}
	}

	gDma1p(gdl++, G_COL, c, n * 4, (n - 1) << 2);

	for (s32 r = 0; r < SELECT_RECTS; r++) {
		gSPVertex(gdl++, v + r * 4, 4, 0);
		gSP1Triangle(gdl++, 0, 1, 2, 0);
		gSP1Triangle(gdl++, 1, 2, 3, 0);
	}

	return gdl;
}

/**
 * build_watch_static_scanline_vertices(): the thin green line that climbs the
 * face while there is static, as wide as the green is at its height (462 is
 * the radius GoldenEye fits it to) and four units deep.
 *
 * Its alpha is 0x380 less four times the green, which GoldenEye stores in a
 * vertex's byte: so it is 0x80 as static strikes, fades to nothing by a green
 * of 0xa0, and comes back at 0xfc to fade a second time.
 */
static Gfx *watchDrawScanline(Gfx *gdl, s32 green)
{
	Vtx *v = gfxAllocateVertices(4);
	Col *c = gfxAllocate(4 * sizeof(Col));
	const f32 y = (f32)g_Watch.scany;
	const f32 inside = 213444.0f - y * y;
	const s16 halfwidth = inside > 0.0f ? (s16)sqrtf(inside) : 0;
	const u8 alpha = (u8)(0x380 - green * 4);
	s32 n = 0;

	for (s32 zoffs = 0; zoffs != 8; zoffs += 4) {
		for (s32 side = -1; side != 3; side += 2) {
			v[n].x = halfwidth * side;
			v[n].y = 0;
			v[n].z = zoffs + g_Watch.scany;
			v[n].flags = 0;
			v[n].colour = n * 4;
			v[n].s = 0;
			v[n].t = 0;
			c[n].r = 0;
			c[n].g = 0xa0;
			c[n].b = 0;
			c[n].a = alpha;
			n++;
		}
	}

	gDPSetRenderMode(gdl++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gDma1p(gdl++, G_COL, c, 4 * 4, 3 << 2);
	gSPVertex(gdl++, v, 4, 0);
	gSP1Triangle(gdl++, 0, 1, 3, 0);
	gSP1Triangle(gdl++, 0, 3, 2, 0);

	return gdl;
}

/**
 * A volume slider (update_volume_slider_verts()): three quads across the face
 * from x -299 to 301, 20 deep from `top` - the empty part dark from the filled
 * edge to the right, the filled part from the left in a green that brightens
 * with the volume, and a band 30 wide (less as it fills) fading from one to
 * the other.
 */
static Gfx *watchDrawSlider(Gfx *gdl, s32 top, f32 fill)
{
	Vtx *v = gfxAllocateVertices(12);
	Col *c = gfxAllocate(12 * sizeof(Col));
	const s32 left = -299;
	const s32 right = 301;
	const s32 xdiff = right - left;
	const s32 tw = (s32)(30.0f * (1.2f - fill));
	s32 edge0, edge1;

	if (fill < 0.0f) {
		fill = 0.0f;
	} else if (fill > 1.0f) {
		fill = 1.0f;
	}

	edge0 = (s32)((f32)left + ((f32)xdiff + (f32)tw) * fill - (f32)tw);
	edge1 = (s32)((f32)left + ((f32)(xdiff + tw)) * fill + (f32)tw);

	if (edge0 < left) {
		edge0 = left;
	}

	if (edge1 > right) {
		edge1 = right;
	}

	for (s32 i = 0; i < 12; i++) {
		const s32 quad = i / 4;
		const s32 k = i % 4; // setup_watch_rectangles(): x then z
		s32 x;

		if (quad == 0) {
			x = k < 2 ? edge1 : right;
		} else if (quad == 1) {
			x = k < 2 ? left : edge0;
		} else {
			x = k < 2 ? edge0 : edge1;
		}

		v[i].x = x;
		v[i].y = 0;
		v[i].z = top + (k & 1) * 20;
		v[i].flags = 0;
		v[i].colour = i * 4;
		v[i].s = 0;
		v[i].t = 0;

		if (quad == 0) {
			c[i].r = 0x20; c[i].g = 0x40; c[i].b = 0x20; c[i].a = 0xe0;
		} else if (quad == 1 || k < 2) {
			c[i].r = (u8)(48.0f * fill) + 0x40;
			c[i].g = (u8)(96.0f * fill) + 0x80;
			c[i].b = c[i].r;
			c[i].a = 0xf0;
		} else {
			c[i].r = 0x20; c[i].g = 0x70; c[i].b = 0x20; c[i].a = 0xf0;
		}
	}

	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDma1p(gdl++, G_COL, c, 12 * 4, 11 << 2);

	for (s32 q = 0; q < 3; q++) {
		gSPVertex(gdl++, v + q * 4, 4, 0);
		gSP1Triangle(gdl++, 0, 1, 2, 0);
		gSP1Triangle(gdl++, 1, 2, 3, 0);
	}

	return gdl;
}

/**
 * draw_background_health_and_armor(): everything drawn on the face itself -
 * the two gauges, the green fill inside its ring and the screen-select
 * rectangles - under the watch's own matrix, a quarter of its size.
 *
 * `squish` is GoldenEye's zoom_squish: while the watch is coming up or going
 * down the face is flattened to a line, and it unfolds as the zoom runs.
 */
static Gfx *watchDrawPageBackground(Gfx *gdl, Mtx *facemtx, s32 squish)
{
	Vtx *ring = gfxAllocateVertices(FACE_VERTICES);
	Col *ringc = gfxAllocate(FACE_VERTICES * sizeof(Col));
	Vtx *fill = gfxAllocateVertices(FACE_VERTICES);
	Col *fillc = gfxAllocate(FACE_VERTICES * sizeof(Col));
	Vtx *health = gfxAllocateVertices(GAUGE_VERTICES);
	Col *healthc = gfxAllocate(GAUGE_VERTICES * sizeof(Col));
	Vtx *armour = gfxAllocateVertices(GAUGE_VERTICES);
	Col *armourc = gfxAllocate(GAUGE_VERTICES * sizeof(Col));
	Mtxf scalemtx;
	Mtx *quarter = gfxAllocateMatrix();
	Mtx *flat = gfxAllocateMatrix();
	f32 scale = 1.0f;
	s32 nring, nfill;
	// zoom_squish puts the green back: there is no static on a face that is
	// still opening
	const s32 green = squish ? STATIC_CLEAR : g_Watch.bggreen;
	const s32 clear = green >= STATIC_CLEAR;

	watchGaugeVertices(armour, armourc, 1, g_Vars.currentplayer->apparentarmour);
	watchGaugeVertices(health, healthc, -1, g_Vars.currentplayer->apparenthealth);

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gDPSetPrimColor(gdl++, 0, 0, 0xe6, 0xe6, 0xe6, 0x00);
	gSPMatrix(gdl++, facemtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

	if (squish) {
		// as the zoom runs the face opens out from a line to the whole disc
		scale = 0.05f;

		if (g_Watch.state == WS_ZOOMIN || g_Watch.state == WS_ZOOMOUT) {
			const f32 max = g_Vars.currentplayer->zoomintimemax;
			const f32 t = g_Vars.currentplayer->zoomintime;

			scale = g_Watch.state == WS_ZOOMIN ? ((45.0f - max) + t) / 45.0f : (max - t) / 45.0f;

			if (scale < 0.05f) {
				scale = 0.05f;
			} else if (scale > 1.0f) {
				scale = 1.0f;
			}
		}
	}

	// a screen turning folds the face flat and opens it again as its zoom runs
	// (draw_background_health_and_armor(): the share of the zoom's change so far,
	// in degrees, squared and held to one)
	if (!squish && watchPageTurning()) {
		const struct player *player = g_Vars.currentplayer;

		scale = player->zoomintimemax > 0.0f
			? player->zoomintime * (player->zoominfovynew - player->zoominfovyold) / player->zoomintimemax
			: 1.0f;

		if (scale < 0.0f) {
			scale = -scale;
		}

		if (scale > 1.0f) {
			scale = 1.0f;
		}

		scale *= scale;
	}

	mtx4LoadIdentity(&scalemtx);
	mtx00015f04(0.25f, &scalemtx);
	guMtxF2L(scalemtx.m, quarter);
	gSPMatrix(gdl++, quarter, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);

	gSPClearGeometryMode(gdl++, G_CULL_BOTH);

	if (!squish) {
		gdl = watchDrawGauge(gdl, armour, armourc);
		gdl = watchDrawGauge(gdl, health, healthc);
	}

	mtx4LoadIdentity(&scalemtx);
	scalemtx.m[2][2] = scale;
	guMtxF2L(scalemtx.m, flat);
	gSPMatrix(gdl++, flat, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);

	if (squish) {
		gdl = watchDrawGauge(gdl, armour, armourc);
		gdl = watchDrawGauge(gdl, health, healthc);
	}

	// the green is the alpha of both, and under static the fill is made
	// without its middle vertex and drawn as the ring is, in bands
	nring = watchFaceVertices(ring, ringc, FACE_VERTICES, FACE_RING, 0, green);
	nfill = watchFaceVertices(fill, fillc, FACE_VERTICES, FACE_FILL, clear, green);

	gDPPipeSync(gdl++);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetPrimColor(gdl++, 0, 0, 0x00, 0xff, 0x00, 0x00);
	gdl = watchDrawFace(gdl, ring, ringc, nring, 0);
	gDPPipeSync(gdl++);

	// G_RM_AA_PCL_SURF under static, which is the snow: the mode carries
	// G_AC_DITHER, and though those two bits are outside the ones a render
	// mode is meant to set, the microcode ors the whole word in - so the fill
	// is tested against noise and its alpha, the green, is how much of it
	// survives. The bits stay set, as they do on the console, which is why
	// the screen-select rectangles and the scanline after it are snowy too
	if (clear) {
		gDPSetRenderMode(gdl++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	} else {
		gDPSetRenderMode(gdl++, G_RM_AA_PCL_SURF, G_RM_AA_PCL_SURF2);
	}

	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gdl = watchDrawFace(gdl, fill, fillc, nfill, clear);

	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gdl = watchDrawSelect(gdl);

	// the options screen's two sliders are on the face too, under the same
	// matrix (draw_music_volume_slider(), draw_fx_volume_slider())
	if (!squish && !watchPageTurning() && g_Watch.page == PAGE_OPTIONS) {
		gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
		gdl = watchDrawSlider(gdl, -275, (f32)optionsGetMusicVolume() / VOL_MAX);
		gdl = watchDrawSlider(gdl, -205, (f32)VOLUME(g_SfxVolume) / VOL_MAX);
	}

	if (!clear) {
		gdl = watchDrawScanline(gdl, green);

		// and no further: the text sets its own, but the watch's hands and
		// markers are drawn by a model that sets none
		gDPSetAlphaCompare(gdl++, G_AC_NONE);
	}

	return gdl;
}

/* ---- the arm, and the watch on it --------------------------------------- */

/**
 * The hour, minute and second hands: the model's own parts 0, 1 and 2, which
 * are GoldenEye's `objheader->Switches[0..2]` and the skeleton's SKEL_HOUR,
 * SKEL_MINUTE and SKEL_SECOND. Each is a `positionheld` node carrying the
 * middle of the face and the matrix the hand is drawn under, and GoldenEye
 * turns all three by the clock rather than by the animation.
 *
 * The model has other `positionheld` nodes, so they are taken by part number
 * rather than by walking the tree for the first three - which found nodes at
 * the origin and left the watch a screen's width off the middle.
 */
static void watchFindHands(struct modeldef *def, struct modelrodata_positionheld **out, s32 *n)
{
	for (s32 i = 0; i < 3; i++) {
		struct modelnode *node = modelGetPart(def, i);

		if (node && (node->type & 0xff) == MODELNODETYPE_POSITIONHELD && node->rodata) {
			out[(*n)++] = &node->rodata->positionheld;
		}
	}
}

/**
 * The sleeve the player's own character wears, for the characters GoldenEye
 * made a sleeve for: its own bodies, by the number the conversion gives them
 * (gexPlusRomChrForRow()). -1 for anybody else, and the mission's outfit
 * answers instead.
 */
static s32 watchCuffForChr(s32 chr)
{
	switch (chr) {
	case 5:  // Brosnan's tuxedo
	case 23: // Formal Wear
		return CUFF_PART_TUXEDO;
	case 0:  // Jungle Commando
	case 24: // Jungle Fatigues
		return CUFF_PART_JUNGLE;
	case 19: // Mishkin, in the Siberian guards' winter coat
	case 21: // Siberian Special Forces
	case 25: // Parka
	case 37: // Siberian Guard
	case 38: // Arctic Commando
		return CUFF_PART_SNOW;
	case 22: // Special Operations Uniform, which is the boiler suit's black
		return CUFF_PART_BOILER;
	}

	return -1;
}

/**
 * bondviewSelectCuff(): the sleeve the arm wears.
 *
 * GoldenEye's own answer is the outfit the mission put Bond in - its
 * `INTROCMD_OUTFIT`, kept in `bondtype`, so the jungle levels get the
 * fatigues, the Surface ones the parka and Facility the boiler suit. Ahead of
 * that comes the player's own character, when they have picked one of
 * GoldenEye's in Customize Character and GoldenEye made a sleeve to match it:
 * somebody playing a mission as Xenia should not have Bond's white cuff on
 * their wrist.
 *
 * The hand inside the sleeve is GoldenEye's own, and there is only one of
 * those - the model is a single display list, so the skin cannot be dressed
 * separately from the sleeve.
 */
s32 geWatchCuff(void)
{
	const u32 outfit = g_Vars.currentplayer->bondtype;
	s32 body = -1;
	s32 head = -1;
	s32 wear;

	playerChooseBodyAndHead(&body, &head, NULL);
	wear = watchCuffForChr(gexPlusRomChrForRow(body));

	if (wear < 0) {
		switch (outfit) {
		case GECUFF_BOILER:  wear = CUFF_PART_BOILER; break;
		case GECUFF_CONNERY: wear = CUFF_PART_CONNERY; break;
		case GECUFF_BLUE:    wear = CUFF_PART_BLUE; break;
		case GECUFF_JUNGLE:  wear = CUFF_PART_JUNGLE; break;
		case GECUFF_SNOW:    wear = CUFF_PART_SNOW; break;
		default:             wear = CUFF_PART_TUXEDO; break;
		}
	}

	return wear - CUFF_FIRST;
}

static void watchSetCuff(void)
{
	const s32 wear = CUFF_FIRST + geWatchCuff();

	for (s32 i = CUFF_FIRST; i <= CUFF_PART_SNOW; i++) {
		struct modelnode *node = modelGetPart(g_Watch.modeldef, i);
		union modelrwdata *rwdata;

		if (!node || (node->type & 0xff) != MODELNODETYPE_TOGGLE) {
			continue;
		}

		rwdata = modelGetNodeRwData(g_Watch.model, node);

		if (rwdata) {
			rwdata->toggle.visible = i == wear;
		}
	}
}

/**
 * bondviewRenderWatch()'s `watchmtx`: where the watch sits on the player's own
 * wrist, in view space.
 *
 * GoldenEye builds it from the body's own position and its head/body offset,
 * turned by the player's heading and taken 12 units back along the look, then
 * multiplies it by the world-to-screen matrix. Perfect Dark keeps both of
 * those fields (`bond2.unk00` is GoldenEye's `theta_transform`, its look
 * vector, and `headbodyoffset` is the same field under the same name), so this
 * is GoldenEye's own three lines.
 */
static void watchWristMatrix(Mtxf *out)
{
	struct player *player = g_Vars.currentplayer;
	const struct coord *look = &player->bond2.unk00;
	const struct coord *hbo = &player->headbodyoffset;
	const struct coord *pos = &player->bond2.unk10;
	struct coord wrist;

	wrist.x = look->x * (hbo->z - 12.0f) + pos->x + hbo->x * -look->z;
	wrist.y = hbo->y + pos->y;
	wrist.z = look->z * (hbo->z - 12.0f) + pos->z + hbo->x * look->x;

	mtx4LoadYRotationWithTranslation(&wrist, (360.0f - player->vv_theta) * (M_PI / 180.0f), out);
	mtx4MultMtx4InPlace(camGetWorldToScreenMtxf(), out);
	mtx00015f04(g_Watch.chrscale, out);
}

/**
 * The inverse of a matrix that is a rotation, one uniform scale and a
 * translation, which is all any of these are. Row 3 is the translation and
 * rows 0-2 the basis scaled by s, so the inverse's basis is the transpose over
 * s squared and its translation is the old one through it, negated.
 */
static void watchInvert(const Mtxf *m, Mtxf *out)
{
	const f32 sq = m->m[0][0] * m->m[0][0] + m->m[0][1] * m->m[0][1] + m->m[0][2] * m->m[0][2];
	const f32 inv = sq > 0.0f ? 1.0f / sq : 0.0f;

	mtx4LoadIdentity(out);

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			out->m[i][j] = m->m[j][i] * inv;
		}
	}

	for (s32 j = 0; j < 3; j++) {
		out->m[3][j] = -(m->m[3][0] * out->m[0][j]
				+ m->m[3][1] * out->m[1][j]
				+ m->m[3][2] * out->m[2][j]);
	}
}

/**
 * The root moved from where the pose left it - the wrist - to the watch's own
 * pose in front of the eye, by how far the arm has come up: GoldenEye's own
 * slerp of the two rotations and lerp of the two positions, which lands on the
 * target as the arm finishes rising.
 *
 * GoldenEye moves the root alone and leaves the arm at the wrist, where its own
 * 4:3 screen at 5.9 degrees sees nothing but the face. This window is wider
 * than that, so the **whole** pose is carried over: every matrix goes through
 * the same move, the arm keeps its shape round the watch, and the width a 16:9
 * window has over a 4:3 one shows the hand and the cuff either side of a face
 * that is still round and still sized by the height.
 */
/**
 * Where the player's own body stands, in view space: at the player's own feet,
 * turned the way they are facing. Its arm then comes up out of its own
 * shoulder, which is where an arm comes from.
 */
static void watchBodyMatrix(Mtxf *out)
{
	struct player *player = g_Vars.currentplayer;
	struct coord pos;

	pos.x = player->bond2.unk10.x;
	pos.y = player->vv_manground;
	pos.z = player->bond2.unk10.z;

	mtx4LoadYRotationWithTranslation(&pos, (360.0f - player->vv_theta) * (M_PI / 180.0f), out);
	mtx4MultMtx4InPlace(camGetWorldToScreenMtxf(), out);
}

/**
 * The move that takes the watch's face from where the pose left it to the
 * watch's own pose in front of the eye, by how far the arm has come up:
 * GoldenEye's own slerp of the two rotations and lerp of the two positions,
 * which lands on the target as the arm finishes rising.
 *
 * GoldenEye moves the model's root alone and leaves the arm at the wrist,
 * since its own 4:3 screen at 5.9 degrees sees nothing but the face. This
 * window is wider and the arm is the player's own body, so what comes back is
 * the move itself and every matrix of both models goes through it: the arm
 * keeps its shape round the watch and the cuff and the hand are beside the
 * face.
 */
static void watchRelToPose(const Mtxf *face, const Mtxf *pose, Mtxf *rel)
{
	struct coord currot;
	struct coord targetrot;
	f32 q1[4];
	f32 q2[4];
	f32 q3[4];
	Mtxf blended;
	Mtxf inverse;
	f32 t = g_Watch.armframe / ARM_FRAMES;
	f32 scale;

	if (t > 1.0f) {
		t = 1.0f;
	} else if (t < 0.0f) {
		t = 0.0f;
	}

	// through the angles rather than through the matrices, as GoldenEye does:
	// both of these carry a scale in their columns and the angles do not care
	mtx4GetRotation((f32 (*)[4])face->m, &currot);
	mtx4GetRotation((f32 (*)[4])pose->m, &targetrot);
	quaternion0f096ca0(&currot, q1);
	quaternion0f096ca0(&targetrot, q2);
	quaternion0f0976c0(q1, q2);
	quaternionSlerp(q1, q2, t, q3);
	quaternionToMtx(q3, &blended);

	// and the scale goes with it: the watch ends at a size of its own however
	// big the body wearing it is, and the arm comes to that size with it
	scale = sqrtf(face->m[0][0] * face->m[0][0] + face->m[0][1] * face->m[0][1] + face->m[0][2] * face->m[0][2]);
	mtx00015f04(scale + (WATCH_FACE_SCALE - scale) * t, &blended);

	blended.m[3][0] = face->m[3][0] + (pose->m[3][0] - face->m[3][0]) * t;
	blended.m[3][1] = face->m[3][1] + (pose->m[3][1] - face->m[3][1]) * t;
	blended.m[3][2] = face->m[3][2] + (pose->m[3][2] - face->m[3][2]) * t;

	// rel = blended * inverse(face): applied after any matrix of the pose, it
	// carries that matrix the same way the face is carried
	watchInvert(face, &inverse);
	mtx4Copy(&inverse, rel);
	mtx4MultMtx4InPlace(&blended, rel);
}

/** Every matrix of a posed model through the same move. */
static void watchApplyRel(const Mtxf *rel, Mtxf *matrices, s32 nummatrices)
{
	for (s32 i = 0; i < nummatrices; i++) {
		mtx4MultMtx4InPlace((Mtxf *)rel, &matrices[i]);
	}
}

/**
 * Whether the near plane is about to cut the body's head, or the watch is up.
 *
 * The head's ball (watchMeasureHead()) goes through the matrix its headspot
 * hangs off, which by now has been through the move to the eye; the view looks
 * down -z and the watch's own projection starts WATCH_NEAR in front of it.
 */
static s32 watchHeadIsCut(struct model *model, struct modelnode *spot)
{
	const Mtxf *mtx = modelFindNodeMtx(model, spot, 0);
	f32 scale;
	f32 z;

	if (g_Watch.state != WS_RAISE && g_Watch.state != WS_LOWERARM) {
		return 1;
	}

	if (!mtx) {
		return 0;
	}

	scale = sqrtf(mtx->m[0][0] * mtx->m[0][0] + mtx->m[0][1] * mtx->m[0][1] + mtx->m[0][2] * mtx->m[0][2]);
	z = g_Watch.headmid.x * mtx->m[0][2] + g_Watch.headmid.y * mtx->m[1][2]
		+ g_Watch.headmid.z * mtx->m[2][2] + mtx->m[3][2];

	return z + g_Watch.headradius * scale > -WATCH_NEAR;
}

/**
 * bondviewRenderWatch(): the arm under its own projection, posed by the
 * animation, with the three hands turned to the mission's clock and the open
 * screen drawn on the face.
 *
 * GoldenEye blends the model between the watch on the player's own wrist and
 * a pose 25 units in front of the eye; the view here has no body to start
 * from, so it takes the pose and the animation does the swinging.
 */
static Gfx *watchDrawModel(Gfx *gdl)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	struct modelrodata_positionheld *hands[3] = { NULL, NULL, NULL };
	struct modeldef *def = g_Watch.modeldef;
	struct model *model = g_Watch.model;
	struct modeldef *wdef = g_Watch.isbody ? g_Watch.watchdef : NULL;
	struct model *wmodel = g_Watch.isbody ? g_Watch.watchmodel : NULL;
	Mtxf *matrices;
	Mtxf *wmatrices = NULL;
	Mtxf base;
	Mtxf pose;
	Mtxf face;
	Mtxf rel;
	Mtxf wrolled;
	const f32 target[3] = { WATCH_POSE_X, WATCH_POSE_Y, WATCH_POSE_Z };
	Mtx *facemtx;
	s32 numhands = 0;
	s32 time;
	f32 seconds, minutes, hours, frac;
	s32 total;

	if (!def || !model) {
		return gdl;
	}

	// the three clock hands, and the face they turn on: the watch's own when
	// the player is wearing their own arm, GoldenEye's floating arm's when
	// they are wearing that
	watchFindHands(wdef ? wdef : def, hands, &numhands);

	if (!g_Watch.isbody) {
		watchSetCuff();
	}

	matrices = gfxAllocate(def->nummatrices * sizeof(Mtxf));

	for (s32 i = 0; i < def->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	// where the arm is posed: the player's own body stands where the player
	// does, so that its arm comes up out of its own shoulder, and GoldenEye's
	// floating arm hangs off the wrist the way GoldenEye hangs it
	// (bondviewRenderWatch()'s watchmtx). What is drawn of the body is the
	// forearm and the hand: once the watch is carried to the eye the rest of
	// it is nearer than the near plane and is cut away.
	if (g_Watch.isbody) {
		watchBodyMatrix(&base);
	} else {
		watchWristMatrix(&base);
	}

	mtx4Copy(&base, matrices);
	model->matrices = matrices;

	renderdata.unk00 = &base;
	renderdata.unk10 = matrices;

	modelSetDistanceChecksDisabled(true);

	// the body's definition is shared with whoever else in the level wears
	// it, and what hangs off its headspot and its toggles is whatever the last
	// of them to be posed left there
	modelUpdateRelations(model);

	if (model->anim) {
		modelSetMatricesWithAnim(&renderdata, model);
	} else {
		modelSetMatrices(&renderdata, model);
	}

	// the watch itself goes on the hand the way anything a character holds
	// does: at the model's own left hand part, under that hand's matrix
	if (wdef && wmodel) {
		struct modelnode *hand = modelGetPart(def, MODELPART_CHR_LEFTHAND);
		Mtxf wbase;

		mtx4LoadIdentity(&wbase);

		// the hand's own matrix, which is already at the hand: a position
		// node's `pos` is the bone that *built* that matrix out of its
		// parent's, not an offset to put something at
		if (hand && (hand->type & 0xff) == MODELNODETYPE_POSITION
				&& hand->rodata->position.mtxindex0 >= 0
				&& hand->rodata->position.mtxindex0 < def->nummatrices) {
			mtx4Copy(&matrices[hand->rodata->position.mtxindex0], &wbase);
		} else {
			mtx4Copy(matrices, &wbase);
		}

		// How it sits on that hand: where GoldenEye's own floating arm wears the
		// same watch on the same bone (WATCH_DIAL_X and the rest) - round the
		// forearm behind the wrist, its dial out of the back of the hand and
		// its crown towards the fingers.
		//
		// Nothing done here can turn the watch on the *screen*. The move that
		// follows squares the dial to the camera whatever it is given, so any
		// turn put on the watch comes out as the opposite turn of the arm
		// under it: a roll tried here to stand the watch upright left the
		// watch where it was and stood the player on their side instead. What
		// this decides is how the arm lies beside a dial that is already
		// upright, and only the true fit leaves it lying as an arm does.
		{
			const f32 size = g_WatchWristScale;
			struct coord along = {
				-(WATCH_HALF_ALONG * size + WATCH_WRIST_CLEAR),
				WATCH_WRIST_Y,
				WATCH_WRIST_Z,
			};

			mtx4LoadXRotation(-M_PI / 2.0f, &wrolled);
			mtx00015f04(size, &wrolled);
			wrolled.m[3][0] = along.x;
			wrolled.m[3][1] = along.y;
			wrolled.m[3][2] = along.z;
			mtx4MultMtx4InPlace(&wbase, &wrolled);
		}

		wmatrices = gfxAllocate(wdef->nummatrices * sizeof(Mtxf));

		for (s32 i = 0; i < wdef->nummatrices; i++) {
			mtx4LoadIdentity(&wmatrices[i]);
		}

		mtx4Copy(&wrolled, wmatrices);
		wmodel->matrices = wmatrices;

		renderdata.unk00 = &wrolled;
		renderdata.unk10 = wmatrices;
		modelUpdateRelations(wmodel);
		modelSetMatrices(&renderdata, wmodel);
	}

	// where the face has ended up, and where GoldenEye's own pose wants it:
	// turned a quarter turn about x so that it looks back at the camera, 25
	// units in front of the eye, less the hour hand's own offset so that the
	// middle of the face is the middle of the screen.
	//
	// The face is always in the *hand bone's* axes - the dial looking out along
	// +y with twelve o'clock at -z - since that is what GoldenEye's rotX(+90)
	// squares up and what its screens are drawn in. The floating arm's hour
	// hand hangs straight off that bone, so its offset is the whole of it. The
	// watch model is a quarter turn off the bone (its dial looks out along its
	// own +z), so its dial's middle is turned back by the same quarter: left
	// in the watch's own axes, the pose stood the band on end with the dial
	// flat underneath it.
	{
		Mtxf *own = wmatrices ? wmatrices : matrices;
		const s32 numown = wmatrices ? wdef->nummatrices : def->nummatrices;
		struct coord pos = { 0, 0, 0 };
		f32 scale;

		if (wmatrices) {
			pos.x = WATCH_DIAL_X;
			pos.y = WATCH_DIAL_Y;
			pos.z = WATCH_DIAL_Z;

			mtx4LoadXRotation(M_PI / 2.0f, &face);
			face.m[3][0] = pos.x;
			face.m[3][1] = pos.y;
			face.m[3][2] = pos.z;
		} else {
			if (numhands > 0) {
				pos.x = hands[0]->pos.x;
				pos.y = hands[0]->pos.y;
				pos.z = hands[0]->pos.z;
			}

			mtx4LoadTranslation(&pos, &face);
		}

		mtx4MultMtx4InPlace(own, &face);

		scale = sqrtf(face.m[0][0] * face.m[0][0] + face.m[0][1] * face.m[0][1] + face.m[0][2] * face.m[0][2]);

		mtx4LoadXRotation(M_PI / 2.0f, &pose);
		mtx00015f04(scale, &pose);
		pose.m[3][0] = target[0];
		pose.m[3][1] = target[1];
		pose.m[3][2] = target[2];

		watchRelToPose(&face, &pose, &rel);
		watchApplyRel(&rel, matrices, def->nummatrices);

		if (wmatrices) {
			watchApplyRel(&rel, wmatrices, numown);
		}

		mtx4MultMtx4InPlace(&rel, &face);
	}

	// the three hands, turned by the mission's own clock. GoldenEye's own
	// watch time is the hour the mission's setup starts it at
	// (INTROCMD_WATCHTIME, which Perfect Dark reads into the field it has
	// always had) plus the time played.
	time = (s32)g_Vars.currentplayer->bondwatchtime60;
	total = time / 60;
	frac = (f32)(time % 60) / 60.0f;
	seconds = (-(((f32)(total % 60)) + frac) * M_PI * 2.0f) / 60.0f;
	minutes = ((-(f32)((total / 60) % 60) * M_PI * 2.0f) / 60.0f) + seconds / 60.0f;
	hours = ((-(f32)((total / 3600) % 12) * M_PI * 2.0f) / 12.0f) + minutes / 12.0f + seconds / 720.0f;

	// the watch model's hands are vertices of its mesh rather than joints
	if (wmodel) {
		watchTurnMeshHands(hours, minutes, seconds);
	}

	{
		Mtxf *own = wmatrices ? wmatrices : matrices;
		const s32 numown = wmatrices ? wdef->nummatrices : def->nummatrices;

		for (s32 i = 0; i < numhands; i++) {
			const s16 index = hands[i]->mtxindex;
			const f32 angle = i == 0 ? hours : (i == 1 ? minutes : seconds);
			struct coord pos;

			if (index < 0 || index >= numown) {
				continue;
			}

			pos.x = hands[i]->pos.x;
			pos.y = hands[i]->pos.y;
			pos.z = hands[i]->pos.z;

			mtx4LoadYRotationWithTranslation(&pos, angle, &own[index]);
			mtx4MultMtx4InPlace(own, &own[index]);
		}
	}

	// the screens are drawn at the middle of the face, in GoldenEye's own
	// units - the face being at the scale the move leaves it at
	{
		Mtxf tmp;

		mtx4Copy(&face, &tmp);
		facemtx = gfxAllocateMatrix();
		guMtxF2L(tmp.m, facemtx);
	}

	renderdata.flags = 3;
	renderdata.zbufferenabled = false;
	// PROP_TYPE_WEAPON, as GoldenEye draws the arm: its env word is the colour
	// the model is fogged towards, the room's shade on the way up and 0xcd of
	// black with the watch open, which is how dark GoldenEye's open dial is
	renderdata.unk30 = 4;
	//
	// On a window wider than 4:3 the arm and the hand are in the picture beside
	// the watch, and fogged that dark they read as a black mass: there the
	// room's shade stays on the whole model, as it is on the way up.
	renderdata.envcolour = (g_Watch.state == WS_OPEN || g_Watch.state == WS_CLOSING) && watchWiden() == 1.0f
		? 0x000000cd
		: (g_Vars.currentplayer->gunshadecol[0] << 24 | g_Vars.currentplayer->gunshadecol[1] << 16
			| g_Vars.currentplayer->gunshadecol[2] << 8 | g_Vars.currentplayer->gunshadecol[3]);
	renderdata.gdl = gdl;

	// GoldenEye draws its floating arm with no z buffer, in the order its own
	// lists come in, and that arm is drawn the same way here. A whole body and
	// a watch that is a model of its own have no such order: the watch's hands
	// come *before* its dial in its list and were painted over by it, which is
	// why they were never seen. So each of the two is drawn into a z buffer
	// of its own - the body sorts itself, then the buffer is emptied and the
	// watch sorts itself over whatever of the arm is under it, the dial being
	// lower on a body's wrist than the top of its forearm (the intro's gun
	// barrel takes a z buffer for the same reason, geintro.c).
	// The HD look's arm (char/suitlfhand, gebean.c) is Bean's mesh, which has
	// no order of its own either: its watch comes before the skin under it
	const s32 hd = xblaMeshModelDrawsBean(model);

	if (wmodel || hd) {
		renderdata.zbufferenabled = true;
	}

	if (g_WatchDrawArm) {
		struct modelnode *spot = g_Watch.hashead ? modelGetPart(def, MODELPART_CHR_HEADSPOT) : NULL;
		union modelrwdata *spotrw = spot ? modelGetNodeRwData(model, spot) : NULL;
		struct modeldef *headdef = spotrw ? spotrw->headspot.headmodeldef : NULL;

		// The head stays on the body while the arm comes up and goes as the
		// watch arrives: the move ends with the head right beside the eye,
		// where the near plane cuts it open and what is left is a wedge of
		// skin down one side of the screen. So it is left out from the moment
		// any of it would be cut - which is the last of the way up - and
		// while the watch is up. modelRender() links a headspot from the
		// instance's own record, so taking the head off that for one draw
		// hides it here and nowhere else.
		if (headdef && watchHeadIsCut(model, spot)) {
			spotrw->headspot.headmodeldef = NULL;
		} else {
			headdef = NULL;
		}

		// the arm is lit the way anything else in the level is, and drawn
		// under texture perspective: without the lights it takes whatever
		// state the frame was left in and draws as one flat pale mass, and
		// without the perspective bit its own textures are sampled flat (the
		// same fault the intro's gun barrel had, ge-bean.md)
		gDPPipeSync(renderdata.gdl++);
		gDPSetTexturePersp(renderdata.gdl++, G_TP_PERSP);
		gDPSetTextureLUT(renderdata.gdl++, G_TT_NONE);
		gDPSetAlphaCompare(renderdata.gdl++, G_AC_NONE);
		gDPSetTextureFilter(renderdata.gdl++, G_TF_BILERP);
		renderdata.gdl = lightsSetDefault(renderdata.gdl);

		if (wmodel || hd) {
			renderdata.gdl = zbufClear(renderdata.gdl);
			gSPSetGeometryMode(renderdata.gdl++, G_ZBUFFER);
		}

		modelRender(&renderdata, model);

		if (hd && !wmodel) {
			gSPClearGeometryMode(renderdata.gdl++, G_ZBUFFER);
		}

		if (headdef) {
			spotrw->headspot.headmodeldef = headdef;
		}
	}

	if (wmodel) {
		renderdata.gdl = zbufClear(renderdata.gdl);
		gSPSetGeometryMode(renderdata.gdl++, G_ZBUFFER);
		modelRender(&renderdata, wmodel);
		gSPClearGeometryMode(renderdata.gdl++, G_ZBUFFER);
	}

	gdl = renderdata.gdl;
	modelSetDistanceChecksDisabled(false);

	// the screen on the face, flattened while the watch is still moving
	gdl = watchDrawPageBackground(gdl, facemtx, g_Watch.state != WS_OPEN && g_Watch.state != WS_CLOSING);

	// the matrices the renderer reads are fixed point; the poses above are not
	for (s32 i = 0; i < def->nummatrices; i++) {
		Mtxf tmp;

		mtx4Copy((Mtxf *)((uintptr_t)model->matrices + i * sizeof(Mtxf)), &tmp);
		mtxF2L(&tmp, model->matrices + i);
	}

	if (wmodel && wmatrices) {
		for (s32 i = 0; i < wdef->nummatrices; i++) {
			Mtxf tmp;

			mtx4Copy((Mtxf *)((uintptr_t)wmodel->matrices + i * sizeof(Mtxf)), &tmp);
			mtxF2L(&tmp, wmodel->matrices + i);
		}
	}

	return gdl;
}


/* ---- the gun held up on the face ---------------------------------------- */


/**
 * GoldenEye shows the gun in the player's hand on the mission page, still, and
 * turns the item under the cursor on the inventory page
 * (draw_watch_mission_status_page() and draw_watch_inventory_page(), both
 * through gunfire.c's set_enviro_fog_for_items_in_solo_watch_menu()). The model
 * is the first person one, which the conversion writes as `Igx%03dZ` by
 * GoldenEye's own item number, and where it stands is its row of gitem_structs
 * (`menu/geitems.bin`, the table as the ROM has it): an eye `watch_pos_z` out
 * along x looking back at the origin on the mission page, and one circling at
 * `equip_watch_z` on the inventory's, the gun first turned by the row's two
 * angles.
 *
 * GoldenEye's camera for it is 45 degrees over its whole 320x240 screen, which
 * here is the frame the text is laid out on and not the window - so its
 * projection is squeezed onto that frame, which keeps the gun over the words
 * that name it at any zoom and on any window.
 *
 * One gun is kept loaded, in memory of the watch's own and as a model of the
 * watch's own rather than one out of the stage pool, as GoldenEye's is a
 * local: a pool instance could not be given back when the cursor moves on.
 */
#define GUN_ITEM_ROW   56
#define GUN_NUM_ITEMS  120
#define GUN_RWDATA_MAX 1024

// GoldenEye's ITEM_IDS the watch draws another item's model for
#define GEITEM_DD44                6
#define GEITEM_WATCHLASER          23
#define GEITEM_TRIGGER             30
#define GEITEM_WATCHMAGNETATTRACT  60

struct watchitem {
	u8 *items;
	u32 itemslen;
	s32 item;      // the item loaded, -1 for none
	s32 failed;    // the item that would not load, not to be tried every frame
	u8 *buf;
	u32 buflen;
	struct modeldef *def;
	struct model model;
	u32 rwdata[GUN_RWDATA_MAX];
};

// the gun, and the controller on the control page, which is a hand item too
static struct watchitem g_WatchGun = { .item = -1, .failed = -1 };
static struct watchitem g_WatchPad = { .item = -1, .failed = -1 };

/** GoldenEye's ITEM_IDS for one of the remake's guns, -1 for anything else. */
static s32 watchGunItem(s32 weaponnum)
{
	static const s8 items[NUM_GE_WEAPONS] = {
		[WEAPON_GE_PP7 - WEAPON_GE_FIRST] = 4,
		[WEAPON_GE_PP7SILENCED - WEAPON_GE_FIRST] = 5,
		[WEAPON_GE_DD44 - WEAPON_GE_FIRST] = 6,
		[WEAPON_GE_KLOBB - WEAPON_GE_FIRST] = 7,
		[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST] = 8,
		[WEAPON_GE_ZMG - WEAPON_GE_FIRST] = 9,
		[WEAPON_GE_D5K - WEAPON_GE_FIRST] = 10,
		[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST] = 11,
		[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST] = 12,
		[WEAPON_GE_AR33 - WEAPON_GE_FIRST] = 13,
		[WEAPON_GE_RCP90 - WEAPON_GE_FIRST] = 14,
		[WEAPON_GE_SHOTGUN - WEAPON_GE_FIRST] = 15,
		[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST] = 16,
		[WEAPON_GE_SNIPERRIFLE - WEAPON_GE_FIRST] = 17,
		[WEAPON_GE_COUGARMAGNUM - WEAPON_GE_FIRST] = 18,
		[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST] = 19,
		[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST] = 22,
		[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = 24,
		[WEAPON_GE_ROCKETLAUNCHER - WEAPON_GE_FIRST] = 25,
		[WEAPON_GE_HUNTINGKNIFE - WEAPON_GE_FIRST] = 2,
		[WEAPON_GE_THROWINGKNIFE - WEAPON_GE_FIRST] = 3,
		[WEAPON_GE_GRENADE - WEAPON_GE_FIRST] = 26,
		[WEAPON_GE_TIMEDMINE - WEAPON_GE_FIRST] = 27,
		[WEAPON_GE_PROXIMITYMINE - WEAPON_GE_FIRST] = 28,
		[WEAPON_GE_REMOTEMINE - WEAPON_GE_FIRST] = 29,
	};

	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= NUM_WEAPONS) {
		return -1;
	}

	// a gadget is whichever of GoldenEye's items the mission makes it
	// (gegadgets.c), the six with nothing in the hand included: the watch
	// shows those too
	if (gegadgetsIsGadget(weaponnum)) {
		return gegadgetsItem(weaponnum) > 0 ? gegadgetsItem(weaponnum) : -1;
	}

	return items[weaponnum - WEAPON_GE_FIRST];
}

static f32 watchGunFloat(s32 item, s32 offset)
{
	union { u32 u; f32 f; } v;

	v.u = watchBe32(g_WatchGun.items + item * GUN_ITEM_ROW + offset);

	return v.f;
}

/** Let an item go; the model is the watch's own and takes nothing with it. */
static void watchItemUnload(struct watchitem *it)
{
	if (it->buf) {
		videoFreeCachedTextures(it->buf, it->buf + it->buflen);
		sysMemFree(it->buf);
		it->buf = NULL;
	}

	it->def = NULL;
	it->item = -1;
}

static void watchGunUnload(void)
{
	watchItemUnload(&g_WatchGun);
	watchItemUnload(&g_WatchPad);
}

// a slot's key for one of Perfect Dark's own guns, past GoldenEye's items
#define GUN_PD_KEY 0x1000

/**
 * A model into a slot under `key`, which is what says it is already there:
 * GoldenEye's hand item number, its file the conversion's Igx%03dZ, or
 * GUN_PD_KEY and up for one of Perfect Dark's own guns, by the file the game
 * itself shows in its menus.
 */
static s32 watchItemLoad(struct watchitem *it, s32 key)
{
	s32 fileid;
	s32 size;

	if (key == it->item) {
		return 1;
	}

	if (key == it->failed) {
		return 0;
	}

	watchItemUnload(it);
	it->failed = key;

	if (key >= GUN_PD_KEY) {
		fileid = weaponGetFileNum(key - GUN_PD_KEY);
	} else if (key >= 0 && key < GUN_NUM_ITEMS) {
		char name[16];

		snprintf(name, sizeof(name), "Igx%03dZ", key);
		fileid = romdataRegisterModFile(name, g_Watch.moddir);
	} else {
		return 0;
	}

	size = fileid > 0 ? fileGetInflatedSize(fileid, LOADTYPE_MODEL) : 0;

	if (size <= 0) {
		return 0;
	}

	it->buflen = ALIGN64(size) + 0x20000;
	it->buf = sysMemZeroAlloc(it->buflen);

	if (!it->buf) {
		return 0;
	}

	it->def = modeldefLoad(fileid, it->buf, it->buflen, NULL);

	if (!it->def) {
		watchItemUnload(it);
		return 0;
	}

	watchFixRenderModes(it->def);
	modelAllocateRwData(it->def);

	if (it->def->rwdatalen > GUN_RWDATA_MAX) {
		watchItemUnload(it);
		return 0;
	}

	memset(it->rwdata, 0, sizeof(it->rwdata));
	modelInit(&it->model, it->def, it->rwdata, false);
	it->model.anim = NULL;
	modelSetScale(&it->model, 1.0f);

	it->item = key;
	it->failed = -1;

	return 1;
}

/** A gun, which also wants its row of gitem_structs to stand by. */
static s32 watchGunLoad(s32 item)
{
	if (!g_WatchGun.items) {
		g_WatchGun.items = watchLoad("geitems.bin", &g_WatchGun.itemslen);
	}

	if (!g_WatchGun.items || g_WatchGun.itemslen < GUN_ITEM_ROW * GUN_NUM_ITEMS) {
		return 0;
	}

	return watchItemLoad(&g_WatchGun, item);
}

static void watchGunSetPart(s32 part, s32 visible)
{
	struct modelnode *node = modelGetPart(g_WatchGun.def, part);

	// Only a toggle has a visible flag. Any other node's state starts where
	// the flag would be - a list's is its vertex pointer - so a part number
	// one of GoldenEye's items gives to something else is left alone
	// (bgunSetPartVisible() is held to the same)
	if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
		union modelrwdata *rwdata = modelGetNodeRwData(&g_WatchGun.model, node);

		if (rwdata) {
			rwdata->toggle.visible = visible;
		}
	}
}

/**
 * GoldenEye's own frame on ours. Its watch is laid out on its 320x240 in-game
 * frame, whose viewport is the 220 rows from 10 down, square pixels; every
 * screen's zoom is a vertical field of view over that viewport. Here the
 * viewport is the player's view, so a GoldenEye row is a 220th of the view's
 * height and a column is worth a row, centred on the view.
 */
#define GE_VIEW_TOP    10.0f
#define GE_VIEW_HEIGHT 220.0f

static f32 watchFrameScaleY(void)
{
	const f32 shrink = watchFaceShrink();

	return shrink == 1.0f ? (f32)viGetViewHeight() / GE_VIEW_HEIGHT : (f32)viGetViewHeight() / GE_VIEW_HEIGHT * shrink;
}

// the frame buffer's columns are not square on a window of another shape
static f32 watchFrameScaleX(void)
{
	return watchFrameScaleY() * (f32)viGetWidth() / ((f32)viGetHeight() * videoGetAspect());
}

// a point of GoldenEye's frame on the frame buffer
static f32 watchFrameX(f32 x)
{
	return viGetViewLeft() + viGetViewWidth() * 0.5f + (x - WATCH_FRAME_W * 0.5f) * watchFrameScaleX();
}

static f32 watchFrameY(f32 y)
{
	if (watchFaceShrink() != 1.0f) {
		// about the middle of the view, which is GoldenEye's row 120
		return viGetViewTop() + viGetViewHeight() * 0.5f + (y - WATCH_FRAME_H * 0.5f) * watchFrameScaleY();
	}

	return viGetViewTop() + (y - GE_VIEW_TOP) * watchFrameScaleY();
}

/**
 * GoldenEye's projection for an item on the watch, over its own viewport: 220
 * rows, the view's height here, and 320 columns, which on a wider view is less
 * than all of it. `aspect` is GoldenEye's own for that item, 1.333333 for all
 * three in the NTSC release (its LEFTOVERDEBUG build and its
 * WATCH_PERSPECTIVE_ASPECT), which is not its viewport's 320/220: items are
 * drawn a little narrow, as GoldenEye's are.
 */
static Gfx *watchItemProjection(Gfx *gdl, f32 fovy, f32 aspect, f32 near, f32 far)
{
	Mtx *projection = gfxAllocateMatrix();
	Mtxf persp;
	Mtxf squeeze;
	Mtxf tmp;
	u16 perspnorm;

	guPerspectiveF(persp.m, &perspnorm, fovy, aspect, near, far, 1.0f);
	mtx4LoadIdentity(&squeeze);
	squeeze.m[0][0] = WATCH_FRAME_W * watchFrameScaleX() / (f32)viGetViewWidth();
	squeeze.m[1][1] = watchFaceShrink();
	mtx4MultMtx4(&squeeze, &persp, &tmp);
	guMtxF2L(tmp.m, projection);

	gDPPipeSync(gdl++);
	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);

	return gdl;
}

/**
 * The gun slot's model drawn under `base`, as a weapon is
 * (PROP_TYPE_WEAPON, unk30 4), the env word being the fog colour it is faded
 * towards - which is the watch's green.
 */
static Gfx *watchRenderGun(Gfx *gdl, Mtxf *base, u32 envcolour)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	Mtxf *matrices = gfxAllocate(g_WatchGun.def->nummatrices * sizeof(Mtxf));
	Mtxf tmp;

	for (s32 i = 0; i < g_WatchGun.def->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	mtx4Copy(base, matrices);
	g_WatchGun.model.matrices = matrices;

	renderdata.unk00 = base;
	renderdata.unk10 = matrices;

	modelSetDistanceChecksDisabled(true);
	modelUpdateRelations(&g_WatchGun.model);
	modelSetMatrices(&renderdata, &g_WatchGun.model);

	renderdata.unk30 = 4;
	renderdata.envcolour = envcolour;
	renderdata.flags = 3;
	renderdata.zbufferenabled = true;

	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gdl = lightsSetDefault(gdl);
	gdl = zbufClear(gdl);
	gSPSetGeometryMode(gdl++, G_ZBUFFER);

	// every face, as a weapon in the hand is drawn (bondgun.c): the lists
	// cull their back faces, and the silenced D5K's silencer is wound the
	// other way round from the rest of it, so turned towards the camera it
	// showed only the inside of its far half (F3 20260926-202346)
	gSPSetExtraGeometryModeEXT(gdl++, G_NO_CULLING_EXT);

	renderdata.gdl = gdl;
	modelRender(&renderdata, &g_WatchGun.model);
	gdl = renderdata.gdl;

	gSPClearExtraGeometryModeEXT(gdl++, G_NO_CULLING_EXT);
	gSPClearGeometryMode(gdl++, G_ZBUFFER);
	modelSetDistanceChecksDisabled(false);

	for (s32 i = 0; i < g_WatchGun.def->nummatrices; i++) {
		mtx4Copy(&matrices[i], &tmp);
		mtxF2L(&tmp, &matrices[i]);
	}

	// the pages' text after it is GoldenEye's, and sets itself up
	return gexFrontTextSetup(gdl);
}

/**
 * One of Perfect Dark's own guns on the face, for a player carrying one
 * (Mod.GePlusPdGuns, a pickup a level left them): GoldenEye has no row for it,
 * so it is held up the way Perfect Dark's own inventory and firing range hold
 * it - the model its menus show (weaponGetFileNum()), its middle brought to
 * the origin, tipped and sized by the row of the inventory menu's table
 * (menuGetWeaponModelConfig()), and the pieces a weapon hides in a menu hidden
 * (its partvisibility list). Side on and still on the mission page, turning on
 * the inventory's, under the same camera and the same green as GoldenEye's.
 */
#define PDGUN_STILL -1.5707963f // side on and pointing left, as GoldenEye's stand
#define PDGUN_EYE   420.0f  // how far back the camera stands
#define PDGUN_ASIDE 75.0f   // and how far right of the list the inventory's gun turns
#define PDGUN_SIZE  1.4f  // over the table's scale, which is sized for the menu's own camera

static Gfx *watchDrawPdGun(Gfx *gdl, s32 weaponnum, s32 turning)
{
	struct weapon *weapon = weaponnum > WEAPON_UNARMED && weaponnum < WEAPON_GE_FIRST ? weaponFindById(weaponnum) : NULL;
	struct coord displace;
	f32 config[5];
	Mtxf base;
	Mtxf tmp;

	if (!weapon || !menuGetWeaponModelConfig(weaponnum, config) || !watchItemLoad(&g_WatchGun, GUN_PD_KEY + weaponnum)) {
		return gdl;
	}

	gdl = watchItemProjection(gdl, 45.0f, 1.3333334f, 10.0f, 10000.0f);

	// the menu's own order: out to its place, its size, its turn, and last the
	// displacement that brings the model's middle to where it turns about
	displace.x = config[0];
	displace.y = config[1];
	displace.z = config[2];

	mtx4LoadTranslation(&displace, &base);

	// tipped towards the eye as the menu tips it while it turns; standing side
	// on the same tip is a roll, and a pistol hangs crooked, so it is left out
	mtx4LoadXRotation(turning ? config[3] : 0.0f, &tmp);
	mtx4MultMtx4InPlace(&tmp, &base);

	mtx4LoadYRotation(turning ? g_Watch.gunangle : PDGUN_STILL, &tmp);
	mtx4MultMtx4InPlace(&tmp, &base);

	mtx4LoadIdentity(&tmp);
	mtx00015f04(config[4] * PDGUN_SIZE, &tmp);
	mtx4MultMtx4InPlace(&tmp, &base);

	mtx00016ae4(&tmp, 0.0f, 0.0f, PDGUN_EYE, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	mtx4MultMtx4InPlace(&tmp, &base);

	// on the inventory page it turns to the right of the list, where
	// GoldenEye's own rows (equip_watch_x) put its guns
	if (turning) {
		base.m[3][0] += PDGUN_ASIDE;
	}

	if (weapon->partvisibility) {
		for (struct modelpartvisibility *ptr = weapon->partvisibility; ptr->part != 255; ptr++) {
			struct modelnode *node = modelGetPart(g_WatchGun.def, ptr->part);
			union modelrwdata *rwdata = node && (node->type & 0xff) == MODELNODETYPE_TOGGLE
				? modelGetNodeRwData(&g_WatchGun.model, node) : NULL;

			if (rwdata) {
				rwdata->toggle.visible = ptr->visible ? true : false;
			}
		}
	}

	return watchRenderGun(gdl, &base, turning ? 0xa0ffa03c : 0x64dc6428);
}

/**
 * The gun of `weaponnum` on the face: still and side on (`turning` 0, the
 * mission page) or circled by the camera (the inventory's).
 */
static Gfx *watchDrawGun(Gfx *gdl, s32 weaponnum, s32 turning)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	s32 item = watchGunItem(weaponnum);
	Mtxf base;
	Mtxf tmp;
	f32 rotx, roty;

	if (item < 0) {
		return watchDrawPdGun(gdl, weaponnum, turning);
	}

	// set_enviro_fog_for_items_in_solo_watch_menu(): the detonator and the
	// watch laser are drawn as the watch magnet's model - GoldenEye's watch
	// on its own - and not as their first-person arm raising the watch
	if (item == GEITEM_TRIGGER || item == GEITEM_WATCHLASER) {
		item = GEITEM_WATCHMAGNETATTRACT;
	}

	if (!watchGunLoad(item)) {
		return gdl;
	}

	gdl = watchItemProjection(gdl, 45.0f, 1.3333334f, 10.0f, 10000.0f);

	// the gun turned by its row's two angles, then the camera
	rotx = watchGunFloat(item, 32);
	roty = watchGunFloat(item, 36);

	mtx4LoadYRotation(roty * M_BADTAU / 360.0f, &base);
	mtx4LoadZRotation(M_BADTAU - rotx * M_BADTAU / 360.0f, &tmp);
	mtx4MultMtx4InPlace(&tmp, &base);

	if (turning) {
		const f32 x = watchGunFloat(item, 44);
		const f32 y = watchGunFloat(item, 48);
		const f32 z = watchGunFloat(item, 52);

		mtx00016ae4(&tmp, cosf(g_Watch.gunangle) * z, y, sinf(g_Watch.gunangle) * z + x,
				0.0f, y, x, 0.0f, 1.0f, 0.0f);
	} else {
		const f32 x = watchGunFloat(item, 20);
		const f32 y = watchGunFloat(item, 24);
		const f32 z = watchGunFloat(item, 28);

		mtx00016ae4(&tmp, z, x, y, 0.0f, x, y, 0.0f, 1.0f, 0.0f);
	}

	mtx4MultMtx4InPlace(&tmp, &base);

	// no hands on it (sub_GAME_7F05E978(model, 0): parts 8 to 13, and 35) and
	// no flash at its muzzle (part 1), but 14 and 15 on (sub_GAME_7F05EA94(model,
	// 1)) - which are the whole of the throwing knife
	for (s32 part = 8; part <= 13; part++) {
		watchGunSetPart(part, 0);
	}

	// Except the DD44's part 11, which is not a hand but the bore at the end
	// of its slide. GoldenEye hides it here too and never shows the hole,
	// drawing the gun with no z buffer so the slide's front paints over it;
	// with one here the muzzle was cut off flat (F3 20260926-202154). The
	// user asked for the whole barrel. The only such part: every other gun
	// with a toggle among 8 to 13 (the PP7s, the Cougar, the Golden Gun, the
	// knife) has the hand there and nothing else.
	if (item == GEITEM_DD44) {
		watchGunSetPart(11, 1);
	}

	watchGunSetPart(35, 0);
	watchGunSetPart(14, 1);
	watchGunSetPart(15, 1);
	watchGunSetPart(1, 0);

	return watchRenderGun(gdl, &base, turning ? 0xa0ffa03c : 0x64dc6428);
}

/**
 * draw_watch_controller() and watchRenderController(): GoldenEye's own joypad,
 * hand item 0x55, 200 up and 200 back from the origin and tipped 45 degrees
 * towards a camera 2000 over it, under GoldenEye's 50.5 degrees. It turns
 * about its middle while the player has hold of the screen's second row, by
 * the stick, and eases back to rest a hundred frames after they let go
 * (`g_Watch.padspin`), and tips towards them by the stick's up and down
 * (`g_Watch.padpitch`). Its stick (part 2) leans with the player's own.
 *
 * Then the same model again with its body put away (toggle part 13), and each
 * of its buttons alone at the edge of the screen beside the words for it,
 * under a camera of its own that does not turn: GoldenEye's
 * g_1ContButtonPositions, each button tipped to face the camera, and pressed
 * in - or rocked, for the triggers and the pad - while the player holds it.
 */
#define PAD_ITEM 0x55
#define PAD_FOVY 50.5f
#define PAD_ASPECT 1.3333334f

enum {
	PADPART_START = 1, PADPART_STICK, PADPART_DPAD, PADPART_CUP, PADPART_CDOWN, PADPART_CLEFT,
	PADPART_CRIGHT, PADPART_A, PADPART_B, PADPART_L, PADPART_R, PADPART_Z, PADPART_BODY,
};

// g_1ContButtonPositions: where each button stands alone, by its part
static const f32 g_PadButtonPos[13][3] = {
	[PADPART_START]  = { -900.0f, 200.0f, -45.0f },
	[PADPART_STICK]  = { 715.0f, 200.0f, 393.0f },
	[PADPART_DPAD]   = { -875.0f, 200.0f, -210.0f },
	[PADPART_CUP]    = { 900.0f, 200.0f, -260.0f },
	[PADPART_CDOWN]  = { 900.0f, 200.0f, -160.0f },
	[PADPART_CLEFT]  = { 850.0f, 200.0f, -210.0f },
	[PADPART_CRIGHT] = { 950.0f, 200.0f, -210.0f },
	[PADPART_A]      = { 900.0f, 200.0f, 128.0f },
	[PADPART_B]      = { 900.0f, 200.0f, -45.0f },
	[PADPART_L]      = { -820.0f, 200.0f, -389.0f },
	[PADPART_R]      = { 820.0f, 200.0f, -389.0f },
	[PADPART_Z]      = { -830.0f, 200.0f, 78.0f },
};

// the stick's lean, 0.6 of a degree a unit
static void watchPadStickLean(Mtxf *out)
{
	Mtxf rx;

	mtx4LoadZRotation(-(f32)joyGetStickX(0) * M_BADTAU * 0.6f / 360.0f, out);
	mtx4LoadXRotation(-(f32)joyGetStickY(0) * M_BADTAU * 0.6f / 360.0f, &rx);
	mtx4MultMtx4InPlace(&rx, out);
}

static s32 watchPadPartMtx(s32 part, struct coord *pos)
{
	struct modelnode *node = modelGetPart(g_WatchPad.def, part);

	s32 index = -1;

	if (!node || !node->rodata) {
		return -1;
	}

	// GoldenEye's switch is a position either way: a held one for the stick,
	// a joint for the rest
	if ((node->type & 0xff) == MODELNODETYPE_POSITIONHELD) {
		*pos = node->rodata->positionheld.pos;
		index = node->rodata->positionheld.mtxindex;
	} else if ((node->type & 0xff) == MODELNODETYPE_POSITION) {
		*pos = node->rodata->position.pos;
		index = node->rodata->position.mtxindex0;
	}

	return index >= 0 && index < g_WatchPad.def->nummatrices ? index : -1;
}

// the renderer's matrices are fixed point once drawn
static void watchPadFix(Mtxf *matrices)
{
	for (s32 i = 0; i < g_WatchPad.def->nummatrices; i++) {
		Mtxf tmp;

		mtx4Copy(&matrices[i], &tmp);
		mtxF2L(&tmp, &matrices[i]);
	}
}

static Gfx *watchPadRender(Gfx *gdl, Mtxf *base, Mtxf *matrices)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };

	renderdata.unk00 = base;
	renderdata.unk10 = matrices;
	renderdata.unk30 = 1; // PROP_TYPE_OBJ: the controller in its own colours
	renderdata.flags = 3;
	renderdata.zbufferenabled = true;
	renderdata.gdl = gdl;

	modelRender(&renderdata, &g_WatchPad.model);

	return renderdata.gdl;
}

static Gfx *watchDrawController(Gfx *gdl)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	struct coord pos = { 0.0f, 200.0f, -200.0f };
	struct modelnode *body;
	union modelrwdata *bodyrw;
	Mtxf base;
	Mtxf tmp;
	Mtxf turn;
	Mtxf lookat;
	Mtxf *matrices;

	if (!watchItemLoad(&g_WatchPad, PAD_ITEM)) {
		return gdl;
	}

	gdl = watchItemProjection(gdl, PAD_FOVY, PAD_ASPECT, 1000.0f, 3000.0f);

	// the spin about z, then the tip towards the eye, then out to its place
	mtx4LoadZRotation(g_Watch.padspin, &turn);
	mtx4LoadXRotation(-g_Watch.padpitch - 0.78539819f, &tmp);
	mtx4MultMtx4(&turn, &tmp, &base);
	mtx4SetTranslation(&pos, &base);

	mtx00016ae4(&lookat, -5.0f, 2000.0f, -168.0f, -5.0f, 0.0f, -168.0f, 0.0f, 0.0f, -1.0f);
	mtx4MultMtx4InPlace(&lookat, &base);

	matrices = gfxAllocate(g_WatchPad.def->nummatrices * sizeof(Mtxf));

	for (s32 i = 0; i < g_WatchPad.def->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	mtx4Copy(&base, matrices);
	g_WatchPad.model.matrices = matrices;

	renderdata.unk00 = &base;
	renderdata.unk10 = matrices;

	modelSetDistanceChecksDisabled(true);

	body = modelGetPart(g_WatchPad.def, PADPART_BODY);
	bodyrw = body && (body->type & 0xff) == MODELNODETYPE_TOGGLE ? modelGetNodeRwData(&g_WatchPad.model, body) : NULL;

	if (bodyrw) {
		bodyrw->toggle.visible = true;
	}

	modelUpdateRelations(&g_WatchPad.model);
	modelSetMatrices(&renderdata, &g_WatchPad.model);

	// the whole pad: every part at its place on it, the stick leaning
	{
		struct coord at;
		const s32 index = watchPadPartMtx(PADPART_STICK, &at);

		if (index >= 0) {
			Mtxf lean;

			watchPadStickLean(&lean);
			mtx4SetTranslation(&at, &lean);
			mtx4MultMtx4(&base, &lean, &matrices[index]);
		}
	}

	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gdl = lightsSetDefault(gdl);
	gdl = zbufClear(gdl);
	gSPSetGeometryMode(gdl++, G_ZBUFFER);

	gdl = watchPadRender(gdl, &base, matrices);
	watchPadFix(matrices);

	// and its buttons alone, beside their words (the second half of
	// watchRenderController()), when the one-pad screen is up
	if (bodyrw) {
		Mtxf *loose = gfxAllocate(g_WatchPad.def->nummatrices * sizeof(Mtxf));

		for (s32 i = 0; i < g_WatchPad.def->nummatrices; i++) {
			mtx4Copy(&base, &loose[i]);
		}

		bodyrw->toggle.visible = false;
		g_WatchPad.model.matrices = loose;

		for (s32 part = PADPART_START; part <= PADPART_Z; part++) {
			struct coord at;
			const s32 index = watchPadPartMtx(part, &at);
			struct coord bpos = { g_PadButtonPos[part][0], g_PadButtonPos[part][1], g_PadButtonPos[part][2] };
			struct coord press = { 0.0f, 0.0f, 0.0f };
			Mtxf rock;
			Mtxf face;
			Mtxf m;
			u16 button = 0;

			if (index < 0) {
				continue;
			}

			mtx4LoadIdentity(&rock);
			mtx4LoadIdentity(&face);

			switch (part) {
			case PADPART_STICK:
				watchPadStickLean(&m);
				mtx4LoadTranslation(&bpos, &face);
				mtx4MultMtx4InPlace(&face, &m);
				mtx4MultMtx4(&lookat, &m, &loose[index]);
				continue;
			case PADPART_R:
			case PADPART_L:
				if (joyGetButtons(0, part == PADPART_R ? R_TRIG : L_TRIG)) {
					mtx4LoadYRotation(part == PADPART_R ? -0.17453294f : 0.17453294f, &rock);
				}

				mtx4LoadXRotation(1.0471976f, &face);
				break;
			case PADPART_DPAD: {
				Mtxf side;

				mtx4LoadIdentity(&side);

				if (joyGetButtons(0, U_JPAD)) {
					mtx4LoadXRotation(-0.17453294f, &rock);
				} else if (joyGetButtons(0, D_JPAD)) {
					mtx4LoadXRotation(0.17453294f, &rock);
				}

				if (joyGetButtons(0, L_JPAD)) {
					mtx4LoadZRotation(0.17453294f, &side);
				} else if (joyGetButtons(0, R_JPAD)) {
					mtx4LoadZRotation(-0.17453294f, &side);
				}

				mtx4MultMtx4InPlace(&side, &rock);
				mtx4LoadXRotation(-0.89759791f, &face);
				break;
			}
			case PADPART_Z:
				if (joyGetButtons(0, Z_TRIG)) {
					mtx4LoadXRotation(-0.17453294f, &rock);
				}

				mtx4LoadZRotation(M_BADPI, &face);
				break;
			default:
				switch (part) {
				case PADPART_START:  button = START_BUTTON; break;
				case PADPART_CUP:    button = U_CBUTTONS; break;
				case PADPART_CDOWN:  button = D_CBUTTONS; break;
				case PADPART_CLEFT:  button = L_CBUTTONS; break;
				case PADPART_CRIGHT: button = R_CBUTTONS; break;
				case PADPART_A:      button = A_BUTTON; break;
				case PADPART_B:      button = B_BUTTON; break;
				}

				if (button && joyGetButtons(0, button)) {
					press.y = -10.0f;
				}

				mtx4LoadXRotation(-1.0471976f, &face);
				break;
			}

			// sub_GAME_7F06351C(): the press, the tip to the camera, out to its
			// place and under the camera
			mtx4LoadTranslation(&press, &m);
			mtx4MultMtx4InPlace(&rock, &m);
			mtx4MultMtx4InPlace(&face, &m);
			mtx4LoadTranslation(&bpos, &face);
			mtx4MultMtx4InPlace(&face, &m);
			mtx4MultMtx4InPlace(&lookat, &m);
			mtx4Copy(&m, &loose[index]);
		}

		gdl = zbufClear(gdl);
		gSPSetGeometryMode(gdl++, G_ZBUFFER);
		gdl = watchPadRender(gdl, &base, loose);
		watchPadFix(loose);

		bodyrw->toggle.visible = true;
		g_WatchPad.model.matrices = matrices;
	}

	gSPClearGeometryMode(gdl++, G_ZBUFFER);
	modelSetDistanceChecksDisabled(false);

	return gexFrontTextSetup(gdl);
}

/* ---- the five screens' text --------------------------------------------- */

// the inventory's list (draw_watch_inventory_page(), the US column)
#define INV_X         0x4e   // the list's left
#define INV_BASE_Y    0x8c   // the top of its five-line window
#define INV_ROWS      5
#define INV_ROWCOLOUR 0x00aa00b0
#define INV_BARCOLOUR 0x00800050

// the rest of GoldenEye's colours for the screens
#define COL_OUTLINE   0x007000a0 // textRenderOutlined()'s, round white
#define COL_LIST      0x00aa00b0 // a list and the controller's words

/**
 * GoldenEye's 320x240 frame over the player's view (GE_VIEW_TOP): the screens'
 * text stands where GoldenEye's does on its own screen, whatever the zoom -
 * which, being GoldenEye's own for every screen, puts the face where
 * GoldenEye's is under it.
 */
static void watchTextFrame(void)
{
	const f32 sy = watchFrameScaleY();

	if (watchFaceShrink() != 1.0f) {
		gexFrontTextFrame(WATCH_FRAME_W, WATCH_FRAME_H,
				viGetViewLeft(), (s32)(viGetViewTop() + viGetViewHeight() * 0.5f - WATCH_FRAME_H * 0.5f * sy),
				viGetViewWidth(), (s32)(WATCH_FRAME_H * sy + 0.5f));
		return;
	}

	gexFrontTextFrame(WATCH_FRAME_W, WATCH_FRAME_H,
			viGetViewLeft(), (s32)(viGetViewTop() - GE_VIEW_TOP * sy),
			viGetViewWidth(), (s32)(WATCH_FRAME_H * sy + 0.5f));
}

// the watch's screens are all in GoldenEye's Bank Gothic
static Gfx *watchPrint(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
	return gexFrontTextPrint(gdl, 1, x, y, text, colour);
}

static void watchMeasure(const char *text, s32 *w, s32 *h)
{
	gexFrontTextMeasure(1, text, w, h);
}

/** textRenderOutlined(): white, the text eight times a unit out in the outline's green first. */
static Gfx *watchPrintOutlined(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
	for (s32 dx = -1; dx <= 1; dx++) {
		for (s32 dy = -1; dy <= 1; dy++) {
			if (dx || dy) {
				gdl = watchPrint(gdl, x + dx, y + dy, text, COL_OUTLINE);
			}
		}
	}

	return watchPrint(gdl, x, y, text, colour);
}

/**
 * Text GoldenEye renders with a line height of its own (textRender()'s last
 * argument): each line of it `line` further down.
 */
static Gfx *watchPrintLines(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour, s32 line)
{
	char buf[256];

	while (*text) {
		const char *end = strchr(text, '\n');
		size_t len = end ? (size_t)(end - text) : strlen(text);

		if (len >= sizeof(buf) - 1) {
			len = sizeof(buf) - 2;
		}

		memcpy(buf, text, len);
		buf[len] = '\n';
		buf[len + 1] = '\0';

		if (len > 0 && !(len == 1 && buf[0] == ' ')) {
			gdl = watchPrint(gdl, x, y, buf, colour);
		}

		y += line;
		text = end ? end + 1 : text + len;
	}

	return gdl;
}

/** draw_options_labels(): a word at x, left, right or centred, outlined white while it is pressed. */
static Gfx *watchLabel(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour, s32 outlined, s32 align)
{
	s32 w, h;

	watchMeasure(text, &w, &h);

	if (align == 1) {
		x -= w / 2;
	} else if (align == 2) {
		x -= w;
	}

	return outlined ? watchPrintOutlined(gdl, x, y, text, 0xffffffff) : watchPrint(gdl, x, y, text, colour);
}

/**
 * The name GoldenEye's gun table gives an item for the mission status, two
 * lines of LgunE (gitem_structs' upper_watch_text and lower_watch_text), or
 * NULL for one of Perfect Dark's own.
 */
static const char *watchGunText(s32 item, s32 offset)
{
	static u8 *bank;
	static u32 banklen;
	static s32 moddir = -2;
	u32 id;

	if (item < 0 || !g_WatchGun.items || g_WatchGun.itemslen < GUN_ITEM_ROW * GUN_NUM_ITEMS) {
		return NULL;
	}

	if (moddir != g_Watch.moddir) {
		sysMemFree(bank);
		bank = watchLoad("LgunE", &banklen);
		moddir = g_Watch.moddir;
	}

	if (!bank) {
		return NULL;
	}

	id = (g_WatchGun.items[item * GUN_ITEM_ROW + offset] << 8) | g_WatchGun.items[item * GUN_ITEM_ROW + offset + 1];

	return id ? watchBankString(bank, banklen, id & 0x3ff) : NULL;
}

/**
 * draw_watch_mission_status_page(): Q WATCH V2.01 BETA, the mission's status
 * with its INCOMPLETE pulsing, the abort's three words, and the gun in the hand
 * side on with its two lines of name and its rounds (draw_current_hand_item_and_ammo()).
 */
static Gfx *watchDrawMissionPage(Gfx *gdl)
{
	const s32 weaponnum = g_Watch.hadweapons ? g_Watch.weapons[HAND_RIGHT] : bgunGetWeaponNum(HAND_RIGHT);
	const char *status;
	s32 w, h;
	u32 colour;

	gdl = watchPrint(gdl, 0x65, YOFFSET_7, watchString(STR_QWATCH), COL_GREEN);

	watchMeasure(watchString(STR_MISSIONSTATUS), &w, &h);
	gdl = watchPrint(gdl, 0x51, YOFFSET_MISSIONSTATUS, watchString(STR_MISSIONSTATUS), COL_GREEN);

	if (objectiveIsAllComplete()) {
		status = watchString(STR_COMPLETE);
		colour = COL_GREEN;
	} else {
		status = watchString(STR_INCOMPLETE);
		colour = g_Watch.statuspulse;
	}

	// GoldenEye moves on by the width of the first string and back up by its
	// height, the newline at the end of it having taken the pen down a line;
	// this printer starts from the line it is given, so the two sit on the
	// same one
	gdl = watchPrint(gdl, 0x51 + w + 4, YOFFSET_MISSIONSTATUS, status, colour);

	// draw_abort_cancel_confirm(): all three dim until the row is held; then
	// ABORT: light, and the one of CANCEL and CONFIRM the player is on white
	// in its outline, the other the page's green
	if (g_Watch.selected) {
		gdl = watchPrint(gdl, 0x51, 0x4c, watchString(STR_ABORT), COL_HIGHLIGHT);

		if (g_Watch.confirm) {
			gdl = watchPrintOutlined(gdl, 0xbd, 0x4c, watchString(STR_CONFIRM), 0xffffffff);
			gdl = watchPrint(gdl, 0x88, 0x4c, watchString(STR_CANCEL), COL_GREEN);
		} else {
			gdl = watchPrint(gdl, 0xbd, 0x4c, watchString(STR_CONFIRM), COL_GREEN);
			gdl = watchPrintOutlined(gdl, 0x88, 0x4c, watchString(STR_CANCEL), 0xffffffff);
		}
	} else {
		gdl = watchPrint(gdl, 0x51, 0x4c, watchString(STR_ABORT), COL_DIM);
		gdl = watchPrint(gdl, 0xbd, 0x4c, watchString(STR_CONFIRM), COL_DIM);
		gdl = watchPrint(gdl, 0x88, 0x4c, watchString(STR_CANCEL), COL_DIM);
	}

	gdl = watchDrawGun(gdl, weaponnum, 0);

	// the rounds, then the name's two lines at 0x60 - 0xa0 and 0xaa down,
	// the NTSC release's own (LEFTOVERDEBUG), not the 0xbc and 0xc6 the
	// source has for the other builds
	if (weaponnum > 0) {
		s32 mag, reserve;

		if (weaponnum == g_Watch.ammoweapon) {
			mag = g_Watch.ammomag;
			reserve = g_Watch.ammoreserve;
		} else if (!geHudWatchAmmo(weaponnum, &mag, &reserve)) {
			mag = reserve = -1;
		}

		if (mag >= 0) {
			gdl = geHudRenderWatchAmmo(gdl, weaponnum, mag, reserve,
					watchFrameX(0.0f), watchFrameY(0.0f), watchFrameScaleX(), watchFrameScaleY());
			gdl = gexFrontTextSetup(gdl);
		}

		{
			const s32 item = watchGunItem(weaponnum);
			const char *upper = watchGunText(item, 16);
			const char *lower = watchGunText(item, 18);

			if (upper || lower) {
				if (upper) {
					gdl = watchPrint(gdl, 0x60, 0xa0, upper, COL_GREEN);
				}

				if (lower) {
					gdl = watchPrint(gdl, 0x60, 0xaa, lower, COL_GREEN);
				}
			} else if (bgunGetName(weaponnum)) {
				gdl = watchPrint(gdl, 0x60, 0xa0, bgunGetName(weaponnum), COL_GREEN);
			}
		}
	}

	return gdl;
}

/**
 * draw_watch_inventory_page(): the item under the cursor turning on the face,
 * and what the player is carrying in a window five lines tall at the bottom
 * left, scrolled so that the cursor's line is its third, with a bar behind
 * that line as wide as the widest name. The cursor's name is drawn light over
 * the bar once the list has settled, and outlined for ten frames after it is
 * taken. GoldenEye clips the list to its window glyph by glyph, so a line half
 * in it is not drawn at all.
 */
static Gfx *watchDrawInventoryPage(Gfx *gdl)
{
	const s32 count = invGetCount();
	s32 widest = 0;
	s32 w, h;

	if (count <= 0) {
		return gdl;
	}

	if (g_Watch.invrow >= count) {
		g_Watch.invrow = count - 1;
		g_Watch.invcursor = g_Watch.invrow + 0.5f;
	}

	gdl = watchDrawGun(gdl, invGetWeaponNumByIndex(g_Watch.invrow), 1);

	for (s32 i = 0; i < count; i++) {
		const char *name = invGetNameByIndex(i);

		if (name) {
			watchMeasure(name, &w, &h);

			if (w > widest) {
				widest = w;
			}
		}
	}

	for (s32 i = 0; i < count; i++) {
		const s32 line = INV_BASE_Y + g_Watch.invtexty + i * INV_LINE;
		const char *name = invGetNameByIndex(i);

		if (!name || line < INV_BASE_Y || line > INV_BASE_Y + (INV_ROWS - 1) * INV_LINE) {
			continue;
		}

		gdl = watchPrint(gdl, INV_X, line, name, INV_ROWCOLOUR);
	}

	{
		const s32 top = INV_BASE_Y + 2 * INV_LINE + 1;

		gdl = gexFrontFillRect(gdl, INV_X - 3, top, INV_X + widest + 4, top + INV_LINE - 2, INV_BARCOLOUR);
		gdl = gexFrontTextSetup(gdl);
	}

	if (g_Watch.invsettled && invGetNameByIndex(g_Watch.invrow)) {
		const char *name = invGetNameByIndex(g_Watch.invrow);

		if (g_Watch.invflash == 0) {
			gdl = watchPrint(gdl, INV_X, INV_BASE_Y + 2 * INV_LINE, name, COL_HIGHLIGHT);
		} else {
			gdl = watchPrintOutlined(gdl, INV_X, INV_BASE_Y + 2 * INV_LINE, name, 0xffffffff);
		}
	}

	return gdl;
}

// game_control_styles: the style's name and the word for each input, as
// LoptionsE numbers - A, B, Z, L, R, the C buttons, the pad, START, the stick
enum { CS_NAME, CS_A, CS_B, CS_Z, CS_L, CS_R, CS_C, CS_DPAD, CS_START, CS_STICK, NUM_CS };

#define STR_FIRE     0x00
#define STR_AIM      0x01
#define STR_ACTION   0x02
#define STR_WEAPON   0x03
#define STR_PAUSE    0x04
#define STR_MOVE     0x05
#define STR_LOOK     0x06
#define STR_QUESTION 0x07
#define STR_MOVESIGHT 0x08

static const u8 g_ControlStyles[8][NUM_CS] = {
	{ 0x09, STR_WEAPON, STR_ACTION, STR_FIRE, STR_AIM, STR_AIM, STR_LOOK, STR_LOOK, STR_PAUSE, STR_MOVE },
	{ 0x0a, STR_WEAPON, STR_ACTION, STR_FIRE, STR_AIM, STR_AIM, STR_MOVE, STR_MOVE, STR_PAUSE, STR_LOOK },
	{ 0x0b, STR_FIRE, STR_ACTION, STR_AIM, STR_WEAPON, STR_WEAPON, STR_LOOK, STR_LOOK, STR_PAUSE, STR_MOVE },
	{ 0x0c, STR_FIRE, STR_ACTION, STR_AIM, STR_WEAPON, STR_WEAPON, STR_MOVE, STR_MOVE, STR_PAUSE, STR_LOOK },
	{ 0x0d, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION },
	{ 0x0e, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION },
	{ 0x0f, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION },
	{ 0x10, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION, STR_QUESTION },
};

static const char *watchStyleName(s32 mode)
{
	return mode >= 0 && mode < 8 ? watchString(g_ControlStyles[mode][CS_NAME]) : "PC\n";
}

// OPTLABELS_*: the controller's words either side of it, the NTSC column
#define OPTLABELS_ROW1_Y    0x52
#define OPTLABELS_ROW2_Y    0x6b
#define OPTLABELS_ROW_PITCH 0x19
#define OPTLABELS_HINT_Y    0xc3

/**
 * sub_GAME_7F0A9AB8(): what each input does under the style, beside the
 * loose buttons - L, the pad, START and Z down the left, R, the C buttons, B
 * and A down the right, and the stick's word under them - each white in its
 * outline while it is held. While the player has hold of the CONTROLLER row
 * the pad and the C buttons name the direction held instead (FORWARD, BACK,
 * the two SIDESTEPs, and UP and DOWN by LOOK UP/DOWN), and an AIM held shows
 * the stick's word as MOVE SIGHT. Perfect Dark's own mouse and keyboard has
 * no words of GoldenEye's.
 */
static Gfx *watchDrawControlLabels(Gfx *gdl, s32 mode)
{
	const u8 *cs;
	const s32 holding = g_Watch.controlrow == 1 && g_Watch.selected;
	const s32 upright = optionsGetForwardPitch(g_Vars.currentplayerstats ? g_Vars.currentplayerstats->mpindex : 0) ? 0 : 1;
	const char *dir1 = watchString(upright ? STR_UP : STR_DOWN);
	const char *dir2 = watchString(upright ? STR_DOWN : STR_UP);
	s32 movesight = 0;
	s32 y;

	if (mode < 0 || mode >= 8) {
		return gdl;
	}

	cs = g_ControlStyles[mode];

	gdl = watchLabel(gdl, 0x32, OPTLABELS_ROW1_Y, watchString(cs[CS_L]), COL_LIST, joyGetButtons(0, L_TRIG) != 0, 0);
	movesight |= joyGetButtons(0, L_TRIG) && cs[CS_L] == STR_AIM;

	y = OPTLABELS_ROW2_Y;

	if (holding && joyGetButtons(0, U_JPAD | D_JPAD | L_JPAD | R_JPAD)) {
		const char *text;

		if (joyGetButtons(0, U_JPAD)) {
			text = cs[CS_DPAD] == STR_MOVE ? watchString(STR_FORWARD) : dir1;
		} else if (joyGetButtons(0, D_JPAD)) {
			text = cs[CS_DPAD] == STR_MOVE ? watchString(STR_BACK) : dir2;
		} else if (joyGetButtons(0, L_JPAD)) {
			text = watchString(STR_SIDESTEP2);
		} else {
			text = watchString(STR_SIDESTEP1);
		}

		gdl = watchLabel(gdl, 0x32, y, text, COL_LIST, 1, 0);
	} else {
		gdl = watchLabel(gdl, 0x32, y, watchString(cs[CS_DPAD]), COL_LIST, 0, 0);
	}

	y += OPTLABELS_ROW_PITCH;
	gdl = watchLabel(gdl, 0x32, y, watchString(cs[CS_START]), COL_LIST, 0, 0);
	y += OPTLABELS_ROW_PITCH;
	gdl = watchLabel(gdl, 0x32, y, watchString(cs[CS_Z]), COL_LIST, joyGetButtons(0, Z_TRIG) != 0, 0);
	movesight |= joyGetButtons(0, Z_TRIG) && cs[CS_Z] == STR_AIM;

	y = OPTLABELS_ROW1_Y;
	gdl = watchLabel(gdl, 0x10e, y, watchString(cs[CS_R]), COL_LIST, joyGetButtons(0, R_TRIG) != 0, 2);
	movesight |= joyGetButtons(0, R_TRIG) && cs[CS_R] == STR_AIM;
	y += OPTLABELS_ROW_PITCH;

	{
		const u32 c = joyGetButtons(0, U_CBUTTONS | D_CBUTTONS | L_CBUTTONS | R_CBUTTONS);

		if (holding && c && (c & (c - 1)) == 0) {
			const char *text;

			if (c & U_CBUTTONS) {
				text = cs[CS_C] == STR_MOVE ? watchString(STR_FORWARD) : dir1;
			} else if (c & D_CBUTTONS) {
				text = cs[CS_C] == STR_MOVE ? watchString(STR_BACK) : dir2;
			} else if (c & L_CBUTTONS) {
				text = watchString(STR_SIDESTEP2);
			} else {
				text = watchString(STR_SIDESTEP1);
			}

			gdl = watchLabel(gdl, 0x10e, y, text, COL_LIST, 1, 2);
		} else {
			gdl = watchLabel(gdl, 0x10e, y, watchString(cs[CS_C]), COL_LIST, 0, 2);
		}
	}

	y += OPTLABELS_ROW_PITCH;
	gdl = watchLabel(gdl, 0x10e, y, watchString(cs[CS_B]), COL_LIST, joyGetButtons(0, B_BUTTON) != 0, 2);
	y += OPTLABELS_ROW_PITCH;
	gdl = watchLabel(gdl, 0x10e, y, watchString(cs[CS_A]), COL_LIST, joyGetButtons(0, A_BUTTON) != 0, 2);

	if (movesight) {
		gdl = watchLabel(gdl, 0xfa, OPTLABELS_HINT_Y, watchString(STR_MOVESIGHT), COL_LIST, 1, 2);
	} else {
		gdl = watchLabel(gdl, 0xfa, OPTLABELS_HINT_Y, watchString(cs[CS_STICK]), COL_LIST, 0, 2);
	}

	return gdl;
}

// a row's name: the page's green, light under the cursor, white in its outline while held
static Gfx *watchRowName(Gfx *gdl, s32 x, s32 y, const char *text, s32 row, s32 cursor)
{
	if (row == cursor && g_Watch.selected) {
		return watchPrintOutlined(gdl, x, y, text, 0xffffffff);
	}

	return watchPrint(gdl, x, y, text, row == cursor ? COL_HIGHLIGHT : COL_GREEN);
}

/**
 * draw_watch_control_options_page(): the controller with its buttons beside
 * their words, CONTROL STYLE with the style's name beside it - a list one line
 * tall that the stick walks while the row is held - and CONTROLLER.
 */
static Gfx *watchDrawControlPage(Gfx *gdl)
{
	const s32 num = g_Vars.currentplayerstats ? g_Vars.currentplayerstats->mpindex : 0;
	const s32 mode = optionsGetControlMode(num);

	gdl = watchDrawController(gdl);

	gdl = watchRowName(gdl, XOFFSET_1, 0x1a, watchString(STR_CONTROLSTYLE), 0, g_Watch.controlrow);

	// draw_controller_style_text(): the list at 0xaa, the name under the
	// cursor light over it once it has settled
	if (g_Watch.selected && g_Watch.controlrow == 0) {
		for (s32 i = 0; i < watchStyleCount(); i++) {
			if (g_Watch.styletexty + i * 10 == 0) {
				gdl = watchPrint(gdl, 0xaa, 0x1a, watchStyleName(watchStyleMode(i)), COL_LIST);
			}
		}
	} else {
		gdl = watchPrint(gdl, 0xaa, 0x1a, watchStyleName(mode), COL_LIST);
	}

	gdl = watchRowName(gdl, XOFFSET_1, 0x2b, watchString(STR_CONTROLLER), 1, g_Watch.controlrow);

	return watchDrawControlLabels(gdl, mode);
}

/**
 * draw_toggle_option_values(): every value of the row, centred on its own x -
 * two at 0xc8 and 0xfa, three at 0xb4, 0xe1 and 0x10e - the one set in the
 * page's green, the others dim, and the one set light while the row is held.
 */
static Gfx *watchDrawOptionValues(Gfx *gdl, s32 y, s32 i, s32 held)
{
	static const s32 two[2] = { 0xc8, 0xfa };
	static const s32 three[3] = { 0xb4, 0xe1, 0x10e };
	const s32 value = watchOptionValue(i);
	const s32 *xs = g_Options[i].numvalues == 3 ? three : two;

	for (s32 v = 0; v < g_Options[i].numvalues; v++) {
		const u32 colour = v != value ? COL_DIM : (held ? COL_HIGHLIGHT : COL_GREEN);

		gdl = watchLabel(gdl, xs[v], y, watchString(g_Options[i].values[v]), colour, 0, 1);
	}

	return gdl;
}

/** draw_watch_game_options_page(): MUSIC and FX over their sliders, and the eight rows. */
static Gfx *watchDrawOptionsPage(Gfx *gdl)
{
	s32 y = YOFFSET_1;

	gdl = watchRowName(gdl, XOFFSET_1, YOFFSET_8, watchString(STR_MUSIC), 0, g_Watch.optionrow);
	gdl = watchRowName(gdl, XOFFSET_1, YOFFSET_9, watchString(STR_FX), 1, g_Watch.optionrow);

	for (s32 i = 0; i < NUM_OPTIONS; i++) {
		const s32 row = i + 2;

		gdl = watchRowName(gdl, XOFFSET_1, y, watchString(g_Options[i].label), row, g_Watch.optionrow);
		gdl = watchDrawOptionValues(gdl, y, i, row == g_Watch.optionrow && g_Watch.selected);

		y += YINC;
	}

	return gdl;
}

// the last line of some text that has anything on it (sub_GAME_7F0AC120())
static const char *watchLastLine(const char *text)
{
	const char *ret = text;

	while (*text) {
		if (*text == '\n') {
			if (*++text) {
				ret = text;
			}
		} else {
			text++;
		}
	}

	return ret;
}

static s32 watchCountLines(const char *text)
{
	s32 n = 0;

	for (; *text; text++) {
		n += *text == '\n';
	}

	return n;
}

/**
 * draw_watch_mission_briefing_page(): the mission's name centred at the top,
 * the page's own title under it, and the page - the briefing's paragraphs
 * wrapped to 0xd2, or the objectives lettered with how each stands, on lines
 * ten apart. GoldenEye stands each objective's status at 0xaf on the line of
 * the objective's last line when that line is short, and under it when not,
 * and leaves two lines between objectives.
 */
static Gfx *watchDrawBriefingPage(Gfx *gdl)
{
	static const s32 titles[NUM_BRIEF_PAGES] = {
		STR_2BACKGROUND, STR_3MBRIEFING, STR_4QBRANCH, STR_5MONEYPENNY, STR_1OBJECTIVES,
	};
	s32 w, h;
	s32 top = 0x1e;

	if (g_Watch.mission >= 0) {
		const char *brief = NULL;
		const char *lang = NULL;
		s32 nameid = 0;

		if (gexFrontMissionFiles(g_Watch.mission, &brief, &lang, &nameid)) {
			const char *title = gexFrontTitleString(nameid);

			s32 lw, lh;

			// GoldenEye's name ends in a newline, and the pen goes down a line
			watchMeasure(title, &w, &h);
			watchMeasure("H\n", &lw, &lh);
			gdl = watchPrint(gdl, (0xaa - w) / 2 + 0x4b, top, title, COL_HIGHLIGHT);
			top += h > lh ? h : lh;
		}
	}

	if (g_Watch.briefpage == BRIEF_OBJECTIVES) {
		const s32 count = objectiveGetCount();
		s32 lines = 1;
		s32 row = 0;

		for (s32 i = 0; i < count; i++) {
			char text[600];
			char status[200];
			const char *name = g_Briefing.objectivenames[i] ? langGet(g_Briefing.objectivenames[i]) : NULL;
			const char *state;
			char *letter;
			u32 colour;
			s32 lw, lh;

			// only this difficulty's, lettered among themselves
			// (get_difficulty_for_objective(i) <= lvlGetSelectedDifficulty());
			// the conversion gives an objective the bits from its lowest
			// difficulty up (geconvert.c's objectiveRecord())
			if (!name || !(objectiveGetDifficultyBits(i) & (1 << lvGetDifficulty()))) {
				continue;
			}

			text[0] = '\0';

			for (s32 j = 0; j < lines; j++) {
				strcat(text, " \n");
			}

			for (s32 j = 0; j < row; j++) {
				strcat(text, " \n\n");
			}

			letter = text + strlen(text);
			snprintf(letter, sizeof(text) - (letter - text), "%c: %s", 'a' + row, name);

			gdl = watchPrintLines(gdl, 0x3c, top + 5, text, COL_GREEN, 10);

			lines += watchCountLines(letter);
			watchMeasure(watchLastLine(letter), &lw, &lh);

			if (lw + 0x3c < (s32)(WATCH_FRAME_W / 2)) {
				lines--;
			}

			switch (objectiveCheck(i)) {
			case OBJECTIVE_COMPLETE:
				state = watchString(STR_COMPLETE);
				colour = COL_HIGHLIGHT;
				break;
			case OBJECTIVE_FAILED:
				state = watchString(STR_FAILED);
				colour = COL_GREEN;
				break;
			default:
				state = watchString(STR_INCOMPLETE);
				colour = (g_Watch.objpulse << 16) | 0x400040ff;
				break;
			}

			status[0] = '\0';

			for (s32 j = 0; j < lines; j++) {
				strcat(status, " \n");
			}

			for (s32 j = 0; j < row; j++) {
				strcat(status, " \n\n");
			}

			strncat(status, state, sizeof(status) - strlen(status) - 1);
			gdl = watchPrintLines(gdl, 0xaf, top + 5, status, colour, 10);

			row++;
		}
	}

	gdl = watchPrintLines(gdl, 0x41, top, watchString(titles[g_Watch.briefpage]), COL_HIGHLIGHT, 10);
	top += 10 + 5;

	if (g_Watch.briefpage != BRIEF_OBJECTIVES && g_Watch.brief && g_Watch.lang) {
		// the briefing file's four paragraphs, each a text id in the mission's
		// own bank (gexfront's frontBriefParagraph())
		const s32 id = (g_Watch.brief[g_Watch.briefpage * 2] << 8) | g_Watch.brief[g_Watch.briefpage * 2 + 1];
		char wrapped[3000];

		gexFrontTextWrap(1, watchLangString(id), wrapped, sizeof(wrapped), 0xd2);
		gdl = watchPrintLines(gdl, 0x3c, top, wrapped, COL_GREEN, 10);
	}

	return gdl;
}

/* ---- what the player sees ----------------------------------------------- */

/**
 * The watch over the player's view, from playerRenderHud(). GoldenEye draws it
 * under a projection of its own at whatever the zoom has reached - 60 degrees
 * as the arm comes up and the screen's own with the face open - and with the
 * depth buffer off, the watch being the last thing in front of the eye. A
 * screen's words and items are left off while its zoom runs.
 */
Gfx *geWatchRender(Gfx *gdl)
{
	if (!g_Watch.loaded) {
		return gdl;
	}

	if (watchIsMp()) {
		return watchMpRender(gdl);
	}

	if (g_Watch.state == WS_CLOSED || !g_Watch.model) {
		return gdl;
	}

	// the arm is not in the view until it starts coming up
	if (g_Watch.state < WS_RAISE) {
		return gdl;
	}

	gDPPipeSync(gdl++);
	gdl = viSetPerspectiveWithFov(gdl, g_Vars.currentplayer->zoominfovy, WATCH_NEAR, 300.0f);
	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	gdl = watchDrawModel(gdl);

	if ((g_Watch.state == WS_OPEN || g_Watch.state == WS_CLOSING) && !watchPageTurning()) {
		watchTextFrame();
		gdl = gexFrontTextSetup(gdl);

		switch (g_Watch.page) {
		case PAGE_MISSION:
			gdl = watchDrawMissionPage(gdl);
			break;
		case PAGE_INVENTORY:
			gdl = watchDrawInventoryPage(gdl);
			break;
		case PAGE_CONTROL:
			gdl = watchDrawControlPage(gdl);
			break;
		case PAGE_OPTIONS:
			gdl = watchDrawOptionsPage(gdl);
			break;
		case PAGE_BRIEFING:
			gdl = watchDrawBriefingPage(gdl);
			break;
		}

		gexFrontTextFrameDefault();
	}

	gDPPipeSync(gdl++);

	return gdl;
}

/* ---- the multiplayer watch ---------------------------------------------- */

/**
 * GoldenEye's multiplayer pause is not the solo watch at all: it is a flat
 * overlay in each player's own viewport (the decomp's src/game/mpmenu.c), with
 * its pages turned by left and right - the scores, the kills, the losses, the
 * pause and the way out - and each player working their own. This is that
 * overlay in GoldenEye's own font and strings, over Perfect Dark's own match.
 *
 * GoldenEye lays it out from the left of a split viewport (x 40, 80 and 112 of
 * its own 320); the viewports here are not GoldenEye's shape, so the rows are
 * centred in whatever viewport the player has.
 */
static const char *watchMpString(s32 index)
{
	return watchBankString(g_Watch.mpmenu, g_Watch.mpmenulen, index);
}

/** Whether this level's pause is the multiplayer overlay rather than the arm. */
static s32 watchIsMp(void)
{
	return g_Vars.mplayerisrunning;
}

static void watchMpTick(void)
{
	const s32 num = g_Vars.currentplayernum;
	const s32 pad = optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex);
	const s32 stickx = joyGetStickX(pad);
	const s32 left = joyGetButtonsPressedThisFrame(pad, L_JPAD | L_CBUTTONS | L_TRIG) != 0
		|| (stickx < -0x2d && !g_MpWatch[num].sticky);
	const s32 right = joyGetButtonsPressedThisFrame(pad, R_JPAD | R_CBUTTONS | R_TRIG) != 0
		|| (stickx > 0x2e && !g_MpWatch[num].sticky);
	const s32 accept = joyGetButtonsPressedThisFrame(pad, A_BUTTON | Z_TRIG | (num == 0 ? BUTTON_UI_ACCEPT : 0)) != 0;
	const s32 back = joyGetButtonsPressedThisFrame(pad, B_BUTTON | (num == 0 ? BUTTON_UI_CANCEL : 0)) != 0;

	// F3's Report a Problem, pushed over the overlay, has the pad (as on the
	// solo watch)
	if (traceReportHoldsInput()) {
		return;
	}

	if (!g_MpWatch[num].on) {
		g_MpWatch[num].sticky = stickx > 0x10 || stickx < -0x10;
		return;
	}

	if (g_MpWatch[num].mode == MPPAGE_EXIT && g_MpWatch[num].confirm) {
		// cancel or confirm, and confirm ends the match
		if (left) {
			g_MpWatch[num].confirm = 1;
			watchBeep();
		} else if (right) {
			g_MpWatch[num].confirm = 2;
			watchBeep();
		} else if (accept) {
			if (g_MpWatch[num].confirm == 2) {
				watchPlaySelect();
				mpSetPaused(MPPAUSEMODE_UNPAUSED);
				g_MpWatch[num].on = 0;
				mainEndStage();
				return;
			}

			g_MpWatch[num].confirm = 0;
			watchBeep();
		} else if (back) {
			g_MpWatch[num].confirm = 0;
			watchBeep();
		}

		g_MpWatch[num].sticky = stickx > 0x10 || stickx < -0x10;
		return;
	}

	if (left || right) {
		s32 mode = g_MpWatch[num].mode + (right ? 1 : -1);

		if (mode < 0) {
			mode = NUM_MPPAGES - 1;
		} else if (mode >= NUM_MPPAGES) {
			mode = 0;
		}

		g_MpWatch[num].mode = (u8)mode;
		watchBeep();
	} else if (accept) {
		switch (g_MpWatch[num].mode) {
		case MPPAGE_PAUSE:
			// the player who paused is the one who can let it go again
			if (!mpIsPaused()) {
				g_MpWatchPauser = num;
				mpSetPaused(MPPAUSEMODE_PAUSED);
				watchPlaySelect();
			} else if (g_MpWatchPauser == num) {
				g_MpWatchPauser = -1;
				mpSetPaused(MPPAUSEMODE_UNPAUSED);
				watchPlaySelect();
			}
			break;
		case MPPAGE_EXIT:
			g_MpWatch[num].confirm = 1;
			watchPlaySelect();
			break;
		default:
			g_MpWatch[num].on = 0;
			watchBeep();
			break;
		}
	} else if (back) {
		g_MpWatch[num].on = 0;

		if (mpIsPaused() && g_MpWatchPauser == num) {
			g_MpWatchPauser = -1;
			mpSetPaused(MPPAUSEMODE_UNPAUSED);
		}

		watchBeep();
	}

	g_MpWatch[num].sticky = stickx > 0x10 || stickx < -0x10;
}

// the overlay's rows above the first player's, in GoldenEye's units
#define MP_ROWS_TOP 48

/**
 * GoldenEye's in-game 320x240 over the player's own viewport. Not the solo
 * watch's frame: that one follows the face at the watch's zoom, and at a
 * match's field of view (no watch comes up) it put the whole overlay into a
 * smudge a few pixels high in the middle of the screen.
 */
static void watchMpTextFrame(void)
{
	gexFrontTextFrame(WATCH_FRAME_W, WATCH_FRAME_H,
			viGetViewLeft(), viGetViewTop(), viGetViewWidth(), viGetViewHeight());
}

/** A row of the overlay, centred in the player's own viewport. */
static Gfx *watchMpRow(Gfx *gdl, s32 y, const char *text, u32 colour)
{
	s32 w;
	s32 h;

	watchMeasure(text, &w, &h);

	return watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) - w / 2, y, text, colour);
}

static Gfx *watchMpRender(Gfx *gdl)
{
	const s32 num = g_Vars.currentplayernum;
	const s32 numchrs = mpGetNumChrs();
	const char *title;
	s32 rows = numchrs < MAX_MPCHRS ? numchrs : MAX_MPCHRS;
	s32 step = 16;
	s32 top;
	s32 y;

	if (!g_MpWatch[num].on) {
		return gdl;
	}

	// GoldenEye's rows (title, heading 31 below it, the players 17 below
	// that and 16 apart), but centred down the view rather than at its y 22:
	// alone in a view, the title sat under Perfect Dark's own health bar, and
	// a full match's twelve rows ran off the bottom
	if (MP_ROWS_TOP + rows * step > WATCH_FRAME_H - 8) {
		step = rows ? (WATCH_FRAME_H - 8 - MP_ROWS_TOP) / rows : step;
	}

	top = ((s32)WATCH_FRAME_H - (MP_ROWS_TOP + rows * step)) / 2;

	if (top < 4) {
		top = 4;
	}

	y = top;

	watchMpTextFrame();
	gdl = gexFrontTextSetup(gdl);

	switch (g_MpWatch[num].mode) {
	case MPPAGE_PAUSE:
		title = watchMpString(mpIsPaused() ? MPSTR_PAUSED : MPSTR_PAUSE);
		break;
	case MPPAGE_EXIT:
		title = watchMpString(MPSTR_EXIT);
		break;
	default:
		title = watchMpString(MPSTR_PLAY);
		break;
	}

	gdl = watchMpRow(gdl, y, title, mpIsPaused() && g_MpWatchPauser == num ? COL_HIGHLIGHT : COL_GREEN);

	// GoldenEye's own chevrons either side of the title, since every page but
	// the two ends has somewhere to go
	gdl = watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) - 44, y, "<\n", COL_GREEN);
	gdl = watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) + 40, y, ">\n", COL_GREEN);

	y = top + 31;

	switch (g_MpWatch[num].mode) {
	case MPPAGE_SCORES:
	case MPPAGE_KILLS:
	case MPPAGE_LOSSES:
		gdl = watchMpRow(gdl, y, watchMpString(g_MpWatch[num].mode == MPPAGE_SCORES ? MPSTR_SCORES
				: (g_MpWatch[num].mode == MPPAGE_KILLS ? MPSTR_KILLS : MPSTR_LOSSES)), COL_GREEN);
		y += 17;

		for (s32 i = 0; i < numchrs && i < MAX_MPCHRS; i++) {
			struct mpchrconfig *mpchr = mpGetChrConfigBySlotNum(i);
			char row[48];
			s32 value = 0;

			if (!mpchr) {
				continue;
			}

			if (g_MpWatch[num].mode == MPPAGE_LOSSES) {
				value = mpchr->numdeaths;
			} else if (g_MpWatch[num].mode == MPPAGE_KILLS) {
				for (s32 k = 0; k < MAX_MPCHRS; k++) {
					value += mpchr->killcounts[k];
				}
			} else {
				value = mpchr->numpoints;
			}

			// the name carries its own newline, so the number goes in a
			// column of its own rather than after it on the same string
			if (mpchr->name[0]) {
				snprintf(row, sizeof(row), "%.14s", mpchr->name);
			} else {
				snprintf(row, sizeof(row), "%s %d\n", watchMpString(MPSTR_P), i + 1);
			}

			{
				const u32 colour = i == g_Vars.currentplayerstats->mpindex ? COL_HIGHLIGHT : COL_GREEN;
				char number[16];

				gdl = watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) - 70, y, row, colour);
				snprintf(number, sizeof(number), "%d\n", value);
				gdl = watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) + 40, y, number, colour);
			}

			y += step;
		}
		break;
	case MPPAGE_EXIT:
		if (g_MpWatch[num].confirm) {
			gdl = watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) - 60, y, watchMpString(MPSTR_CANCEL),
					g_MpWatch[num].confirm == 2 ? COL_GREEN : COL_HIGHLIGHT);
			gdl = watchPrint(gdl, (s32)(WATCH_FRAME_W * 0.5f) + 12, y, watchMpString(MPSTR_CONFIRM),
					g_MpWatch[num].confirm == 2 ? COL_HIGHLIGHT : COL_GREEN);
		}
		break;
	}

	gexFrontTextFrameDefault();

	return gdl;
}

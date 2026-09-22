/**
 * GE Plus's menus: GoldenEye's own folder screens, drawn the way GoldenEye's
 * front end (the decomp's src/game/front.c) draws them.
 *
 * Everything drawn is GoldenEye's, from the conversion of the player's ROM
 * (geconvert.c): the menu folder is its prop model PROP_WALLETBOND (the models
 * block's slot 278), the cursor is its crosshair image, and the text is its
 * Zurich Bold and Bank Gothic fonts and the title screen's strings (LtitleE),
 * copied into menu/. GoldenEye's screens are laid out on a 440x330 frame, and
 * so are these: every position here is GoldenEye's, scaled to the screen.
 *
 * GoldenEye's screens on the way to a match, and so these:
 * - the mode select: 1. SELECT MISSION (off: the remake has no missions yet) and
 *   2. MULTIPLAYER, with the PREVIOUS tab back to the Perfect Menu;
 * - MULTIPLAYER OPTIONS, GoldenEye's rows with two of the remake's own after
 *   Players - Simulants and their difficulty - since GoldenEye has none, and the
 *   START tab to the match. Players, Game Length, Weapons and Aim change in
 *   place, as GoldenEye's do, and so do the two added rows;
 * - the Level page (constructor_menu12_mpstage): GoldenEye's film strip of
 *   stage pictures, twelve to a page - GoldenEye's own multiplayer twelve on
 *   the first, in its order - and GoldenEye's NEXT tab to turn to the rest,
 *   since the remake has 26 arenas where GoldenEye had eleven;
 * - the Scenario page, GoldenEye's eight with its three team games grey (the
 *   remake has no teams yet);
 * - the Health and Control Style pages, a panel a player, each player choosing
 *   on their own controller and the page closing when all have;
 * - the Characters page (constructor_menu0F_mpcharsel): a panel a player with
 *   a strip of GoldenEye's four-tile portraits scrolling under the player's
 *   choice. The characters are the remake's - GoldenEye X's, borrowed into
 *   the Combat Simulator's list (modborrow.c), or Perfect Dark's own without
 *   it - and a portrait is GoldenEye's for the character of that name, or its
 *   silhouette.
 *
 * The music is GoldenEye's too: its folders theme (sequence 23, M_FOLDERS) on
 * its own instrument bank, both copied out of the ROM into menu/ and appended
 * to the game's sequences once, the way GoldenEye X's borrowed tunes are.
 *
 * The cursor is GoldenEye's: the stick moves it, A or Z (or the keyboard's
 * accept) picks what it is over, B (or cancel) is PREVIOUS and START is the
 * START tab. The mouse moves it too, the left
 * button picks and Escape goes back.
 *
 * While open it owns the menu's tick and render (menuTick() and menuRender()
 * return after calling it); the Perfect Menu underneath is left open and comes
 * back as it was.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "input.h"
#include "mod.h"
#include "modborrow.h"
#include "modloader.h"
#include "romdata.h"
#include "system.h"
#include "video.h"
#include "gexplus.h"
#include "gecinema.h"
#include "gemonitor.h"
#include "game/zbuf.h"
#include "game/propobj.h"
#include "gefolder.h"
#include "gexfront.h"
#include "gemusic.h"
#include "gesfx.h"
#include "game/modghost.h"
#include "preprocess.h"
#include "game/challenge.h"
#include "game/title.h"
#include "game/pdmode.h"
#include "game/lv.h"
#include "lib/main.h"
#include "game/file.h"
#include "game/gfxmemory.h"
#include "game/lang.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/modeldef.h"
#include "game/modunlocks.h"
#include "game/modelmgr.h"
#include "game/music.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
#include "game/mpstats.h"
#include "game/objectives.h"
#include "game/inv.h"
#include "game/bondgun.h"
#include "game/player.h"
#include "game/options.h"
#include "game/tex.h"
#include "lib/joy.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "lib/snd.h"
#include "lib/vi.h"

extern s32 g_MpWeaponSetNum;

#define GEFRONT_W 440.0f
#define GEFRONT_H 330.0f

// frontSetupMenuBackground()'s camera: GoldenEye's 60 degree view with the eye
// 700 in front of the folder, which is drawn at a quarter of its own size.
// frontWidenBackdrop() measures the view with these as well.
#define FOLDER_FOVY 60.0f
#define FOLDER_EYEZ 700.0f
#define FOLDER_SCALE 0.25f
#define FOLDER_TANHALFFOVY 0.57735026f // tanf(FOLDER_FOVY / 2)

// the frame that stands behind the folder (frontWidenBackdrop())
#define GEFRONT_BACKDROP_VTX 8

// GoldenEye's model number of the menu folder, and its switches (bondconstants.h)
#define FOLDER_MODEL 278
#define SW_TABS         0
#define SW_PAPER        1
#define SW_EYESONLY     2
#define SW_OHMSS        3
#define SW_CONFIDENTIAL 4
#define SW_CONFIDENTIAL2 5
#define SW_CLASSIFIED   6
#define SW_PHOTOBOND    7
#define SW_BROSNAN      8
#define SW_PHOTOBRIEF   12
#define SW_BROSNANCOVER 15
#define SW_SLIDES       19
#define SW_PICS         20
// the mission select's grid of slides: the part bondconstants.h does not name,
// and the one the numbers after it are off by. A mission's own photo follows it
#define SW_SLIDEGRID    21
#define SW_BRIEFFIRST   22
#define SW_BLANK        42

// GoldenEye's crosshair image (IMAGE_CROSSHAIR1), 32x32 RGBA32
#define CURSOR_IMAGE 2236
// the film strip's holes (IMAGE_DOT), 16x16 I8
#define DOT_IMAGE 2631
#define DOT_RELEASE "attract/sprocket"
// a stage picture: 68x44 I8
#define STAGE_IMAGE_W 0x44
#define STAGE_IMAGE_H 0x2c
#define STAGE_IMAGE_RANDOM 2695

// front.h's tabs
#define TABS_LEFT_EDGE 390.0f
#define TABS_RIGHT_EDGE 411
#define STARTTAB_TEXT_TOP 51
#define STARTTAB_TEXT_BOTTOM 117
#define STARTTAB_TAB_BOTTOM 130.5f
#define NEXTTAB_TAB_TOP 130.5f
#define NEXTTAB_TEXT_TOP 144
#define NEXTTAB_TEXT_BOTTOM 210
#define PREVTAB_TAB_TOP 223.0f
#define PREVTAB_TEXT_TOP 236
#define PREVTAB_TEXT_BOTTOM 302

// LtitleE's strings GoldenEye's two screens use
#define TITLE_START        4
#define TITLE_NEXT         5
#define TITLE_PREVIOUS     6
#define TITLE_SELECTMISSION 29
#define TITLE_MULTIPLAYER  30
#define TITLE_LEN_UNLIMITED 45
#define TITLE_LEN_LASTALIVE 52
#define TITLE_SCEN_NORMAL  53
#define TITLE_AIM_FIRST    72
#define TITLE_MPOPTIONS    76
#define TITLE_PLAYERS      77
#define TITLE_SCENARIO     78
#define TITLE_LEVEL        79
#define TITLE_GAMELENGTH   80
#define TITLE_WEAPONS      81
#define TITLE_CHARACTERS   82
#define TITLE_HEALTH       83
#define TITLE_AIM          84
#define TITLE_SELECTCHARACTER 85
#define TITLE_SELECTHANDICAP 86
#define TITLE_SCENARIOHEAD 87
#define TITLE_HEALTH_FIRST 61
#define TITLE_CONTROL_FIRST 277
#define TITLE_SELECTCONTROLSTYLE 285
#define TITLE_CONTROLSTYLE 286

// and the strings its solo screens use
#define TITLE_DIFF_FIRST   19 // Agent, Secret Agent, 00 Agent, 007, as the briefing names them
#define TITLE_JB           32 // " - James Bond 007", after the difficulty
#define TITLE_MISSION2     33 // "Mission "
#define TITLE_PART         34 // "Part "
#define TITLE_DIFFICULTY   35
#define TITLE_DIFF2_FIRST  36 // and as the difficulty page lists them
#define TITLE_SPECOPS      40
#define TITLE_REACTION     41
#define TITLE_ENEMYHEALTH  42
#define TITLE_ENEMYDAMAGE  43
#define TITLE_ENEMYACCURACY 44
#define TITLE_OBJECTIVES   93
#define TITLE_BRIEF_FIRST  93 // Primary Objectives, Background, M Briefing, Q Branch, Moneypenny
#define TITLE_MISSION_FIRST 120 // the folder's names for the chapters and their missions
// and the two pages a mission ends on
#define TITLE_OBJ_COMPLETED 91
#define TITLE_OBJ_FAILED   92
#define TITLE_REPORT       98
#define TITLE_MISSIONSTATUS 99
#define TITLE_KIA          100
#define TITLE_ABORTED      101
#define TITLE_COMPLETED    102
#define TITLE_FAILED       103
#define TITLE_STATISTICS   104
#define TITLE_TIME         105
#define TITLE_ACCURACY     106
#define TITLE_WEAPONOFCHOICE 107
#define TITLE_SHOTTOTAL    108
#define TITLE_HEADHITS     109
#define TITLE_BODYHITS     110
#define TITLE_LIMBHITS     111
#define TITLE_OTHERS       112
#define TITLE_KILLTOTAL    113
#define TITLE_BESTTIME     273
#define TITLE_TARGET       274

// the text colours: black, and black greyed for a row that is off
#define COLOUR_ON  0x000000ff
#define COLOUR_OFF 0x00000070
// a highlight: black at 50, and a filled bar: black at 100
#define COLOUR_HIGHLIGHT 0x00000032
#define COLOUR_BAR 0x00000064
// the report's red: a mission or an objective that was not completed
#define COLOUR_FAILED 0x780000ff

enum { SCREEN_MODE, SCREEN_MPOPTIONS, SCREEN_LEVEL, SCREEN_SCENARIO, SCREEN_HEALTH, SCREEN_CONTROLSTYLE, SCREEN_CHARACTERS,
	SCREEN_MISSION, SCREEN_DIFFICULTY, SCREEN_007OPTIONS, SCREEN_BRIEFING, SCREEN_CINEMA, SCREEN_CINEMAPICK,
	SCREEN_EXTRA, SCREEN_MONITORS, SCREEN_MONITORVIEW, SCREEN_REPORT, SCREEN_STATS };

/**
 * GoldenEye's mission folder (front.c's mission_folder_setup_entries): its nine
 * chapter headings and, under each, its missions, in the order the folder gives
 * them. A mission is the grid's cell `mission`, and the missions run 0-19 across
 * five columns and down four rows.
 *
 * `brief` is its briefing file and `lang` the text bank every id in that file
 * indexes, both converted out of the ROM into menu/ (geconvert.c's g_MenuText).
 */
#define NUM_MISSIONS 20
#define MISSION_COLS 5
#define MISSION_ROWS 4

struct missionrow {
	const char *numeral;
	s32 name;          // LtitleE's name for it
	s32 icon;          // the shorter name the grid shows instead, 0 for none
	s32 mission;       // -1 for a chapter heading
	const char *brief;
	const char *lang;
};

static const struct missionrow g_Missions[] = {
	{ "1",   120,   0, -1, NULL,               NULL },
	{ "i",   121,   0,  0, "UbriefdamZ",       "LdamE" },
	{ "ii",  122,   0,  1, "UbriefarkZ",       "LarkE" },
	{ "iii", 123,   0,  2, "UbriefrunZ",       "LrunE" },
	{ "2",   124,   0, -1, NULL,               NULL },
	{ "i",   125,   0,  3, "UbriefsevxZ",      "LsevxE" },
	{ "ii",  126,   0,  4, "UbriefsevbunkerZ", "LsevE" },
	{ "3",   127,   0, -1, NULL,               NULL },
	{ "i",   128, 129,  5, "UbriefsiloZ",      "LsiloE" },
	{ "4",   130,   0, -1, NULL,               NULL },
	{ "i",   131,   0,  6, "UbriefdestZ",      "LdestE" },
	{ "5",   124,   0, -1, NULL,               NULL },
	{ "i",   125,   0,  7, "UbriefsevxbZ",     "LsevxbE" },
	{ "ii",  126,   0,  8, "UbriefsevbZ",      "LsevbE" },
	{ "6",   132,   0, -1, NULL,               NULL },
	{ "i",   133, 134,  9, "UbriefstatueZ",    "LstatE" },
	{ "ii",  135, 136, 10, "UbriefarchZ",      "LarchE" },
	{ "iii", 137,   0, 11, "UbriefpeteZ",      "LpeteE" },
	{ "iv",  138,   0, 12, "UbriefdepoZ",      "LdepoE" },
	{ "v",   139,   0, 13, "UbrieftraZ",       "LtraE" },
	{ "7",   140,   0, -1, NULL,               NULL },
	{ "i",   141,   0, 14, "UbriefjunZ",       "LjunE" },
	{ "ii",  142, 143, 15, "UbriefcontrolZ",   "LarecE" },
	{ "iii", 144, 145, 16, "UbriefcaveZ",      "LcaveE" },
	{ "iv",  146, 147, 17, "UbriefcradZ",      "LcradE" },
	{ "8",   148,   0, -1, NULL,               NULL },
	{ "i",   149, 150, 18, "UbriefaztZ",       "LaztE" },
	{ "9",   151,   0, -1, NULL,               NULL },
	{ "i",   152, 153, 19, "UbriefcrypZ",      "LcrypE" },
};

#define NUM_MISSION_ROWS (sizeof(g_Missions) / sizeof(g_Missions[0]))

// cursor_xpos_table_mission_select and cursor_ypos_table_mission_select
static const s32 g_MissionX[MISSION_COLS] = { 73, 142, 212, 282, 352 };
static const s32 g_MissionY[MISSION_ROWS] = { 62, 131, 201, 270 };

// a briefing file: four paragraphs, then ten objectives of {text id, difficulty}
#define BRIEF_PARAGRAPHS 4
#define BRIEF_OBJECTIVES 10
#define BRIEF_SIZE (BRIEF_PARAGRAPHS * 2 + BRIEF_OBJECTIVES * 4)

// the briefing's five pages
enum { BRIEF_TITLE, BRIEF_OVERVIEW, BRIEF_M, BRIEF_Q, BRIEF_MONEYPENNY, NUM_BRIEF_PAGES };

// the 007 options' four sliders, in the order the page lists them
enum { SLIDER_HEALTH, SLIDER_DAMAGE, SLIDER_ACCURACY, SLIDER_REACTION, NUM_SLIDERS };

// GoldenEye's four difficulties; the fourth is its 007 mode, the sliders' own
#define NUM_DIFFICULTIES 4
#define DIFFICULTY_007 3

// the Characters page: the Combat Simulator bodies it lists, and a portrait's spacing on the strip
#define MAX_CHARACTERS 128
#define PORTRAIT_SPACING 0x54

// the Level page: twelve to a page, and the most the remake converts
#define LEVELS_PER_PAGE 12
#define MAX_LEVELS 64

// the Scenario page's rows: GoldenEye's five, then its three team games
#define NUM_SCENARIO_ROWS 8
#define NUM_SOLO_SCENARIOS 5

// MP_handicap_table, and Perfect Dark's own Ext style after GoldenEye's eight
#define NUM_HANDICAPS 11
#define NUM_CONTROLSTYLES 9

// textures drawn this visit, by number: each a config texSelect() makes a pointer of
#define MAX_FRONT_TEXTURES 96   // the Monitor Programmes page can draw any of GoldenEye's fifty

/**
 * GoldenEye's multiplayer rows. GoldenEye's are 20 apart from 0x79; with the
 * remake's two added, 16 apart, which ends on GoldenEye's last row (0x119).
 */
enum {
	ROW_PLAYERS,
	ROW_SIMULANTS,
	ROW_SIMDIFF,
	ROW_SCENARIO,
	ROW_LEVEL,
	ROW_GAMELENGTH,
	ROW_WEAPONS,
	ROW_CHARACTERS,
	ROW_HEALTH,
	ROW_CONTROLSTYLE,
	ROW_AIM,
	NUM_ROWS
};

#define ROW_TOP 0x79
#define ROW_PITCH 16

struct gefontchar {
	s32 index;
	s32 baseline;
	s32 height;
	s32 width;
	s32 kerningindex;
	u8 *pixels;
};

struct gefont {
	u8 *data;
	s32 kerning[13 * 13];
	struct gefontchar chars[94];
};

static struct {
	s32 active;
	s32 loaded;
	s32 screen;
	s32 moddir;
	s32 inputdelay;

	struct gefont zurich;
	struct gefont gothic;
	u8 *title;
	u32 titlelen;

	u8 *modelbuf;
	u32 modelbuflen;
	struct modeldef *modeldef;
	struct model *model;
	struct textureconfig cursor;

	f32 cursorx;
	f32 cursory;
	s32 mousex;
	s32 mousey;
	s32 mouseseen;
	s32 highlight;      // the row (or mode) under the cursor, -1 for none
	s32 tabprev;
	s32 tabstart;

	s32 gamelength;     // GoldenEye's multi_game_lengths index
	s32 aim;            // GoldenEye's mp_sight_adjust_table index

	s32 levels[MAX_LEVELS]; // the Level page's stages, STAGE_MP_RANDOM first
	s32 numlevels;
	s32 levelpage;
	s32 tabnext;

	s32 handicap[MAX_PLAYERS];  // MP_handicap_table index a player
	s32 chosen[MAX_PLAYERS];    // a player has chosen on a per-player page
	s32 stickarmed[MAX_PLAYERS];

	s32 characters[MAX_CHARACTERS]; // mpbodynums the Characters page lists
	s32 numcharacters;
	s32 charcur[MAX_PLAYERS];     // the character a player is on
	s32 charprev[MAX_PLAYERS];    // the one the strip is centred on while it scrolls
	s32 charscroll[MAX_PLAYERS];  // how far past it
	s32 charsize[MAX_PLAYERS];    // how far a chosen portrait has grown, to 11
	s32 charpicked;               // player 1 chose on the Characters page this session

	s32 cinemawhat;     // the cinema last picked for a mission: its opening or its ending
	s32 monitor;        // the Monitor Programmes page: the programme showing
	s32 nummonitors;    // and how many the conversion has
	struct tvscreen monitorscreen;   // the screen the big view runs on
	s32 monitorpage;    // the page of TV sets showing
	struct tvscreen tvscreens[12];   // a screen a set, each on its own programme
	struct model *tvmodels[12];      // GoldenEye's TV set, an instance a cell
	struct modeldef *tvdef;
	u8 *tvbuf;
	s32 tvbuflen;
	s32 mission;        // the mission the grid is on, 0-19
	s32 difficulty;     // the difficulty chosen for it
	s32 briefpage;      // the briefing page open
	u8 *brief;          // its briefing file, and the text bank that file indexes
	u8 *lang;
	u32 langlen;
	f32 slider[NUM_SLIDERS];  // the 007 options, GoldenEye's own values
	s32 sliderheld;           // the slider the pick is dragging, -1 for none

	struct {
		s32 num;
		struct textureconfig config;
	} textures[MAX_FRONT_TEXTURES];
	s32 numtextures;

	// the backdrop's frame, and the vertices the ROM gave it
	struct modelrodata_dl *backdrop;
	Vtx backdropvtx[GEFRONT_BACKDROP_VTX];
} g_Front;

/* ---- files -------------------------------------------------------------- */

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u8 *frontLoad(const char *rel, u32 *len)
{
	char path[FS_MAXPATH + 1];
	const char *dir = fsGetModDirAt(g_Front.moddir);

	if (!dir) {
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/menu/%s", dir, rel);

	return fsFileLoad(path, len);
}

/**
 * A GoldenEye font as the ROM stores it: 169 kerning words, then 94 glyphs of
 * six words {index, top, height, width, kerning index, pixels offset}; each
 * glyph's pixels are 8-bit intensity, its width rounded up to 8.
 */
static s32 frontLoadFont(struct gefont *font, const char *rel)
{
	u32 len = 0;
	u8 *data = frontLoad(rel, &len);

	if (!data || len < 0x2a4 + 94 * 24) {
		sysMemFree(data);
		return 0;
	}

	font->data = data;

	for (s32 i = 0; i < 13 * 13; i++) {
		font->kerning[i] = (s32)be32(data + i * 4);
	}

	for (s32 i = 0; i < 94; i++) {
		const u8 *row = data + 0x2a4 + i * 24;
		struct gefontchar *c = &font->chars[i];
		const u32 at = be32(row + 20);

		c->index = (s32)be32(row);
		c->baseline = (s32)be32(row + 4);
		c->height = (s32)be32(row + 8);
		c->width = (s32)be32(row + 12);
		c->kerningindex = (s32)be32(row + 16) % 13;

		if (at + ((c->width + 7) & ~7) * c->height > len) {
			sysMemFree(data);
			font->data = NULL;
			return 0;
		}

		c->pixels = data + at;
	}

	return 1;
}

/** A string of LtitleE, as langGet() gives it: an offset table, then the strings. */
static const char *frontString(s32 index)
{
	u32 at;

	if (!g_Front.title || (u32)(index + 1) * 4 > g_Front.titlelen) {
		return "";
	}

	at = be32(g_Front.title + index * 4);

	return at && at < g_Front.titlelen ? (const char *)g_Front.title + at : "";
}

/**
 * A string of the mission's own text bank, which every id in its briefing file
 * indexes: a GoldenEye text id is its bank * 0x400 plus the slot.
 */
static const char *frontLangString(s32 id)
{
	const u32 index = (u32)id & 0x3ff;
	u32 at;

	if (!g_Front.lang || (index + 1) * 4 > g_Front.langlen) {
		return "";
	}

	at = be32(g_Front.lang + index * 4);

	return at && at < g_Front.langlen ? (const char *)g_Front.lang + at : "";
}

/** pull_and_display_text_for_folder_a0(): the folder's row for a mission. */
static s32 frontMissionRow(s32 mission)
{
	for (s32 i = 0; i < (s32)NUM_MISSION_ROWS; i++) {
		if (g_Missions[i].mission == mission) {
			return i;
		}
	}

	return -1;
}

/** get_chapter_briefing_entry(): the chapter heading a row falls under. */
static s32 frontMissionChapter(s32 row)
{
	for (s32 i = row; i >= 0; i--) {
		if (g_Missions[i].mission < 0) {
			return i;
		}
	}

	return -1;
}

/** A mission's name as the grid shows it: the shorter one where it has one, in capitals. */
static const char *frontMissionName(s32 mission, char *buf, size_t len)
{
	const s32 row = frontMissionRow(mission);
	const char *name;
	size_t n;

	if (row < 0) {
		return "";
	}

	name = frontString(g_Missions[row].icon ? g_Missions[row].icon : g_Missions[row].name);
	snprintf(buf, len, "%s\n", name);

	for (n = 0; buf[n]; n++) {
		if (buf[n] >= 'a' && buf[n] <= 'z') {
			buf[n] -= 0x20;
		}
	}

	return buf;
}

/**
 * The remake's own missions: GoldenEye's twenty, converted from the player's
 * ROM into the arenas' mod and registered as stages of their own
 * (tools/geconvert/gesolo.py, modloader.c's missions block). They need nothing
 * else installed.
 *
 * Where the conversion has not run - an old converted directory, or none - the
 * folder falls back to GoldenEye X's own missions, which the port plays when
 * GoldenEye X is the mod the game is *loaded* with and its mission list has
 * been imported over the port's (moddata.c's importSoloStages()). With neither
 * the mode select's SELECT MISSION stays grey.
 */
static s32 frontMissionsAreOwn(void)
{
	return modloaderNumMissions() >= NUM_MISSIONS;
}

static s32 frontMissionsAvailable(void)
{
	return frontMissionsAreOwn() || (modBorrowIsGoldenEyeLoaded() && NUM_MISSIONS <= NUM_SOLOSTAGES);
}

/** The stage a mission runs, the remake's own where there is one. */
static s32 frontMissionStage(s32 mission)
{
	if (mission < 0 || mission >= NUM_MISSIONS) {
		return 0;
	}

	if (frontMissionsAreOwn()) {
		return modloaderMissionStage(mission);
	}

	return g_SoloStages[mission].stagenum;
}

/** Whether GoldenEye's 007 mode is open, by the rule Perfect Dark opens its own PD Mode by. */
static s32 front007Unlocked(void)
{
	return g_GameFile.besttimes[SOLOSTAGEINDEX_SKEDARRUINS][DIFF_PA] != 0 || (g_ModUnlocks & MODUNLOCK_COMPLETION);
}

/**
 * get_highest_unlocked_difficulty_for_level(): the highest difficulty a mission
 * can be played at, or -1 when it cannot be played at all. GoldenEye's first
 * three are Perfect Dark's three, and its 007 is Perfect Dark's PD Mode - the
 * same sliders over the hardest difficulty.
 */
static s32 frontHighestDifficulty(s32 mission)
{
	if (!frontMissionsAvailable() || mission < 0 || mission >= NUM_MISSIONS) {
		return -1;
	}

	// The remake's own missions are stages of their own and have no place in
	// the save's solo stage table, so there is nothing to unlock them
	// against: they are all open, as GoldenEye X's are with its modconfig.
	if (frontMissionsAreOwn()) {
		return front007Unlocked() ? DIFFICULTY_007 : DIFF_PA;
	}

	for (s32 d = DIFF_PA; d >= 0; d--) {
		if (isStageDifficultyUnlocked(mission, d)) {
			return d == DIFF_PA && front007Unlocked() ? DIFFICULTY_007 : d;
		}
	}

	return -1;
}

/** The first of the remake's arenas, or -1 when the conversion has not run. */
static s32 frontFirstArena(void)
{
	for (s32 i = 0; i < mpGetNumStages(); i++) {
		if (modloaderStageIsRemake(g_MpArenas[i].stagenum)) {
			return i;
		}
	}

	return -1;
}

static void frontUnloadTvs(void);

static void frontUnloadModel(void)
{
	frontUnloadTvs();

	if (g_Front.model) {
		modelmgrFreeModel(g_Front.model);
		g_Front.model = NULL;
	}

	// The release's pictures are bound against this model's own textures, which
	// are inside the buffer about to be freed (gefolder.c)
	geFolderForget();

	if (g_Front.modelbuf) {
		videoFreeCachedTextures(g_Front.modelbuf, g_Front.modelbuf + g_Front.modelbuflen);
		sysMemFree(g_Front.modelbuf);
		g_Front.modelbuf = NULL;
	}

	g_Front.modeldef = NULL;
	g_Front.backdrop = NULL;
}

/**
 * The backdrop is the one display list of the folder model that no switch
 * covers: a frame of eight vertices - an outer rectangle and an inner one the
 * folder itself stands in - drawn behind everything else.
 *
 * It is found once, as the model loads - the first list of that many vertices
 * that no MODELNODETYPE_TOGGLE covers - and its vertices are kept so that
 * frontWidenBackdrop() can work from the ROM's own numbers however often the
 * window changes shape.
 */
static struct modelrodata_dl *frontFindBackdrop(struct modelnode *node)
{
	for (; node; node = node->next) {
		const s32 type = node->type & 0xff;

		if (type == MODELNODETYPE_TOGGLE) {
			continue;
		}

		if (type == MODELNODETYPE_DL) {
			if (node->rodata->dl.numvertices == GEFRONT_BACKDROP_VTX) {
				return &node->rodata->dl;
			}
		} else {
			struct modelrodata_dl *dl = frontFindBackdrop(node->child);

			if (dl) {
				return dl;
			}
		}
	}

	return NULL;
}

static void frontLoadBackdrop(void)
{
	g_Front.backdrop = frontFindBackdrop(g_Front.modeldef->rootnode);

	if (g_Front.backdrop) {
		for (s32 i = 0; i < GEFRONT_BACKDROP_VTX; i++) {
			g_Front.backdropvtx[i] = g_Front.backdrop->vertices[i];
		}
	}
}

static s32 frontLoadModel(void)
{
	const s32 fileid = romdataRegisterModFile("Pgx278Z", g_Front.moddir);
	s32 size;

	if (fileid <= 0) {
		return 0;
	}

	size = fileGetInflatedSize(fileid, LOADTYPE_MODEL);

	if (size <= 0) {
		return 0;
	}

	// the loader takes the file's textures' room from the same buffer
	g_Front.modelbuflen = ALIGN64(size) + 0x20000;
	g_Front.modelbuf = sysMemZeroAlloc(g_Front.modelbuflen);

	if (!g_Front.modelbuf) {
		return 0;
	}

	g_Front.modeldef = modeldefLoad(fileid, g_Front.modelbuf, g_Front.modelbuflen, NULL);

	if (!g_Front.modeldef) {
		frontUnloadModel();
		return 0;
	}

	modelAllocateRwData(g_Front.modeldef);
	g_Front.model = modelmgrInstantiateModelWithAnim(g_Front.modeldef);

	if (!g_Front.model) {
		frontUnloadModel();
		return 0;
	}

	{
		struct coord zero = {0, 0, 0};

		modelSetScale(g_Front.model, 1);
		modelSetRootPosition(g_Front.model, &zero);
	}

	frontLoadBackdrop();

	// The release's own art on GoldenEye's own folder, where there is a copy
	// of the release and its meshes are on (gefolder.c)
	geFolderRepaint(g_Front.modeldef);

	return 1;
}

/**
 * load_briefing_text_for_stage(): a mission's briefing file, and the text bank
 * it indexes. Both are held only while the briefing is open, as GoldenEye holds
 * them only while its briefing screen is.
 */
static void frontFreeBriefing(void)
{
	sysMemFree(g_Front.brief);
	sysMemFree(g_Front.lang);
	g_Front.brief = NULL;
	g_Front.lang = NULL;
	g_Front.langlen = 0;
}

static s32 frontLoadBriefing(s32 mission)
{
	const s32 row = frontMissionRow(mission);
	u32 brieflen = 0;

	frontFreeBriefing();

	if (row < 0 || !g_Missions[row].brief) {
		return 0;
	}

	g_Front.brief = frontLoad(g_Missions[row].brief, &brieflen);
	g_Front.lang = frontLoad(g_Missions[row].lang, &g_Front.langlen);

	if (!g_Front.brief || brieflen < BRIEF_SIZE || !g_Front.lang) {
		frontFreeBriefing();
		return 0;
	}

	return 1;
}

/** The briefing's four paragraphs, and its objectives' text and difficulty. */
static s32 frontBriefParagraph(s32 page)
{
	return g_Front.brief ? (s32)((g_Front.brief[page * 2] << 8) | g_Front.brief[page * 2 + 1]) : 0;
}

static s32 frontBriefObjective(s32 i, s32 *difficulty)
{
	const u8 *p;

	if (!g_Front.brief) {
		return 0;
	}

	p = g_Front.brief + BRIEF_PARAGRAPHS * 2 + i * 4;
	*difficulty = (s32)((p[2] << 8) | p[3]);

	return (s32)((p[0] << 8) | p[1]);
}

/**
 * How the last mission went, kept as it ends (gexFrontMissionReport()) for the
 * two pages GoldenEye closes a mission with: the level is gone by the time
 * they are drawn, as GoldenEye's is, and with it the player and the objectives
 * those pages are about.
 */
static struct {
	s32 valid;
	s32 kia;
	s32 aborted;
	s32 completed;
	s32 objstatus[BRIEF_OBJECTIVES];
	s32 time60;
	s32 kills;
	s32 shots[7];       // get_curplayer_shot_register(): Perfect Dark's SHOTREGION_*
	char weapon[64];    // the weapon of choice, by name, and whether it was a pair
	s32 weapondual;
	s32 difficulty;
	f32 slider[NUM_SLIDERS];
} g_FrontReport;

// solo_target_time_array: the time a cheat is won by, in seconds, a difficulty
static const s16 g_TargetTimes[NUM_MISSIONS][3] = {
	{ 0, 160, 0 }, { 0, 0, 125 }, { 300, 0, 0 }, { 0, 210, 0 }, { 0, 0, 240 },
	{ 180, 0, 0 }, { 0, 270, 0 }, { 0, 0, 255 }, { 90, 0, 0 },  { 0, 195, 0 },
	{ 0, 0, 80 },  { 105, 0, 0 }, { 0, 100, 0 }, { 0, 0, 325 }, { 225, 0, 0 },
	{ 0, 600, 0 }, { 0, 0, 570 }, { 135, 0, 0 }, { 0, 540, 0 }, { 0, 0, 360 },
};

/**
 * The best times, in seconds, a mission and a difficulty - GoldenEye's save
 * keeps the same twenty by four. They are the remake's own file: its missions
 * are stages of their own with no row in Perfect Dark's save, whose rows are
 * Perfect Dark's missions'.
 */
#define BESTTIMES_FILE "$S/geplus-times.txt"
#define BESTTIME_MAX 0x3ff

static u16 g_BestTimes[NUM_MISSIONS][NUM_DIFFICULTIES];
static s32 g_BestTimesLoaded;

static void frontLoadBestTimes(void)
{
	FILE *f;
	s32 mission, difficulty, secs;

	if (g_BestTimesLoaded) {
		return;
	}

	g_BestTimesLoaded = 1;

	if (fsFileSize(BESTTIMES_FILE) <= 0 || !(f = fsFileOpenRead(BESTTIMES_FILE))) {
		return;
	}

	while (fscanf(f, "%d %d %d", &mission, &difficulty, &secs) == 3) {
		if (mission >= 0 && mission < NUM_MISSIONS && difficulty >= 0 && difficulty < NUM_DIFFICULTIES
				&& secs > 0 && secs <= BESTTIME_MAX) {
			g_BestTimes[mission][difficulty] = secs;
		}
	}

	fsFileFree(f);
}

static void frontSaveBestTimes(void)
{
	FILE *f = fsFileOpenWrite(BESTTIMES_FILE);

	if (!f) {
		sysLogPrintf(LOG_WARNING, "gexfront: could not write %s", fsFullPath(BESTTIMES_FILE));
		return;
	}

	for (s32 mission = 0; mission < NUM_MISSIONS; mission++) {
		for (s32 difficulty = 0; difficulty < NUM_DIFFICULTIES; difficulty++) {
			if (g_BestTimes[mission][difficulty]) {
				fprintf(f, "%d %d %d\n", mission, difficulty, g_BestTimes[mission][difficulty]);
			}
		}
	}

	fsFileFree(f);
}

static void frontUnload(void)
{
	frontUnloadModel();
	frontFreeBriefing();
	sysMemFree(g_Front.zurich.data);
	sysMemFree(g_Front.gothic.data);
	sysMemFree(g_Front.title);
	g_Front.zurich.data = NULL;
	g_Front.gothic.data = NULL;
	g_Front.title = NULL;
	g_Front.loaded = 0;
}

/**
 * The fonts and the title screen's strings, without the folder model: what
 * GoldenEye's own text needs and nothing more. The watch (gewatch.c) asks for
 * this in a level, where the folder itself is closed and its model would be
 * half a megabyte of nothing.
 */
static s32 frontLoadText(void)
{
	const s32 first = frontFirstArena();

	if (g_Front.zurich.data && g_Front.gothic.data && g_Front.title) {
		return 1;
	}

	if (first < 0) {
		return 0;
	}

	g_Front.moddir = modloaderGetStageModDirIndex(g_MpArenas[first].stagenum);

	if (g_Front.moddir < 0
			|| !frontLoadFont(&g_Front.zurich, "fontzurichbold.bin")
			|| !frontLoadFont(&g_Front.gothic, "fontbankgothic.bin")
			|| !(g_Front.title = frontLoad("LtitleE", &g_Front.titlelen))) {
		return 0;
	}

	return 1;
}

static s32 frontLoadAll(void)
{
	if (!frontLoadText() || !frontLoadModel()) {
		sysLogPrintf(LOG_WARNING, "gexfront: the conversion's menu files are missing; GE Plus opens Perfect Dark's menu");
		frontUnload();
		return 0;
	}

	g_Front.loaded = 1;

	return 1;
}

/* ---- the music ---------------------------------------------------------- */

/**
 * The folders theme (GoldenEye's M_FOLDERS) as a sequence number of the
 * game's; -1 when the conversion has no music.
 */
static s32 frontMusic(void)
{
	static s32 warned;
	const s32 seqnum = geMusicSequence(GEMUSIC_FOLDERS);

	if (seqnum < 0 && !warned) {
		warned = 1;
		sysLogPrintf(LOG_WARNING, "gexfront: the conversion has no GoldenEye music; the folder plays the menu's own");
	}

	return seqnum;
}

s32 gexFrontMusic(void)
{
	return g_Front.active && g_Front.loaded ? frontMusic() : -1;
}

/* ---- the stages --------------------------------------------------------- */

/**
 * The remake's arenas by the name the converter gives each map (geconvert's
 * NAMES), with the stage picture GoldenEye has for its level, in the order the
 * Level page lists them: GoldenEye's multiplayer page in its own order first
 * (Library, Basement and Stack share a picture, as they do there), then the
 * rest in its mission order. An arena not named here goes last, with the
 * picture GoldenEye gives Random.
 */
static const struct { const char *name; s32 image; } g_FrontStages[] = {
	{ "Temple", 2686 }, { "Complex", 2688 }, { "Caves", 2689 },
	{ "Library", 2687 }, { "Basement", 2687 }, { "Stack", 2687 },
	{ "Facility", 2580 }, { "Bunker", 2592 }, { "Archives", 2578 },
	{ "Caverns", 2582 }, { "Egyptian", 2584 },
	{ "Dam", 2585 }, { "Runway", 2590 }, { "Surface", 2593 }, { "Bunker 1", 2591 },
	{ "Silo", 2595 }, { "Frigate", 2587 }, { "Surface 2", 2594 }, { "Statue Park", 2596 },
	{ "Streets", 2589 }, { "Depot", 2586 }, { "Train", 2597 }, { "Jungle", 2588 },
	{ "Control", 2579 }, { "Cradle", 2583 }, { "Aztec", 2581 },
};

static const char *frontStageName(s32 stagenum)
{
	for (s32 i = 0; i < mpGetNumStages(); i++) {
		if (g_MpArenas[i].stagenum == stagenum) {
			const char *name = modloaderGetStageMapName(stagenum);

			return name ? name : mpGetArenaName(i);
		}
	}

	return "";
}

static s32 frontStageImage(s32 stagenum)
{
	const char *name = stagenum == STAGE_MP_RANDOM ? NULL : modloaderGetStageMapName(stagenum);

	for (s32 i = 0; name && i < ARRAYCOUNT(g_FrontStages); i++) {
		if (strcmp(g_FrontStages[i].name, name) == 0) {
			return g_FrontStages[i].image;
		}
	}

	return STAGE_IMAGE_RANDOM;
}

/**
 * The release's picture of a stage picture (texture/level): GoldenEye's twenty
 * mission slides are its icons in the alphabetical order of their names, and
 * the multiplayer levels' four follow. Random has none.
 */
static const char *frontStageRelease(s32 image)
{
	static const char *const slides[] = {
		"arch", "arec", "ark", "azt", "cave", "crad", "cryp", "dam", "depo", "dest",
		"jun", "pete", "run", "sev", "sevb", "sevx", "sevxb", "silo", "stat", "tra",
	};
	static const char *const mp[] = { "smptemple", "smplib", "smpcomplex", "smpcave" };
	static char name[32];
	const char *stem = NULL;

	if (image >= 2578 && image < 2578 + (s32)ARRAYCOUNT(slides)) {
		stem = slides[image - 2578];
	} else if (image >= 2686 && image < 2686 + (s32)ARRAYCOUNT(mp)) {
		stem = mp[image - 2686];
	}

	if (!stem) {
		return NULL;
	}

	snprintf(name, sizeof(name), "level/%sicon", stem);

	return name;
}

/** The Level page's list: Random, then the remake's arenas in g_FrontStages' order. */
static void frontBuildLevels(void)
{
	const s32 num = mpGetNumStages();
	s32 used[MP_NUM_ARENAS_STATIC + MAX_MODSTAGES] = {0};

	g_Front.numlevels = 0;
	g_Front.levels[g_Front.numlevels++] = STAGE_MP_RANDOM;

	for (s32 k = 0; k <= ARRAYCOUNT(g_FrontStages); k++) {
		for (s32 i = 0; i < num && g_Front.numlevels < MAX_LEVELS; i++) {
			const char *name = modloaderGetStageMapName(g_MpArenas[i].stagenum);

			if (used[i] || !modloaderStageIsRemake(g_MpArenas[i].stagenum)) {
				continue;
			}

			// the named ones in the table's order, and on the last pass whatever is left
			if (k < ARRAYCOUNT(g_FrontStages) && (!name || strcmp(name, g_FrontStages[k].name) != 0)) {
				continue;
			}

			used[i] = 1;
			g_Front.levels[g_Front.numlevels++] = g_MpArenas[i].stagenum;
		}
	}
}

/* ---- the characters ----------------------------------------------------- */

/**
 * GoldenEye's portraits (s_mpcharselimages), four 65x65 tiles each - upper left,
 * upper right, lower left, lower right - by the name GoldenEye gives the
 * character, letters only and lower case, so GoldenEye X's "Natalya (Russia)"
 * and "May Day" find theirs. The Bonds by actor first. Mishkin's tiles are in
 * the ROM out of order.
 */
static const struct { const char *key; s32 tiles[4]; } g_FrontPortraits[] = {
	{ "connery", { 2606, 2607, 2608, 2609 } },
	{ "moore", { 2610, 2611, 2612, 2613 } },
	{ "dalton", { 2614, 2615, 2616, 2617 } },
	{ "bond", { 2602, 2603, 2604, 2605 } },
	{ "brosnan", { 2602, 2603, 2604, 2605 } },
	{ "boris", { 2632, 2633, 2634, 2635 } },
	{ "ourumov", { 2636, 2637, 2638, 2639 } },
	{ "trevelyan", { 2640, 2641, 2642, 2643 } },
	{ "valentin", { 2644, 2645, 2646, 2647 } },
	{ "xenia", { 2648, 2649, 2650, 2651 } },
	{ "natalya", { 2652, 2653, 2654, 2655 } },
	{ "baronsamedi", { 2656, 2657, 2658, 2659 } },
	{ "jaws", { 2660, 2661, 2662, 2663 } },
	{ "mayday", { 2664, 2665, 2666, 2667 } },
	{ "oddjob", { 2668, 2669, 2670, 2671 } },
	{ "mishkin", { 2694, 2693, 2691, 2692 } },
};

static const s32 g_FrontRandomPortrait[4] = { 2682, 2683, 2684, 2685 };

/**
 * The release's portraits (texture/characters), one picture each, by the same
 * keys - and the release draws the characters GoldenEye left with Random's
 * question mark as well. Connery, Moore and Dalton it has none of.
 */
static const struct { const char *key; const char *picture; } g_FrontReleasePortraits[] = {
	{ "connery", NULL }, { "moore", NULL }, { "dalton", NULL },
	{ "bond", "brosnan" }, { "brosnan", "brosnan" },
	{ "boris", "boris" }, { "ourumov", "ourumov" }, { "trevelyan", "trevelyan" },
	{ "valentin", "valentin" }, { "xenia", "xenia" }, { "natalya", "natalya" },
	{ "baronsamedi", "baron" }, { "jaws", "jaws" }, { "mayday", "mayday" },
	{ "oddjob", "oddjob" }, { "mishkin", "mishkin" },
	{ "arcticcommando", "arcticcommando" }, { "helicopterpilot", "helicopterpilot" },
	{ "janusmarine", "janusmarine" }, { "junglecommando", "junglecommando" },
	{ "moonrakerelite", "moonrakerelite" }, { "navalofficer", "navalguard" },
	{ "russianinfantry", "russianinfantry" }, { "russiansoldier", "russiansoldier" },
	{ "siberianspecialforces", "siberianspecialforces" }, { "siberianguard", "siberianguard1" },
	{ "stpetersburgguard", "stpetersburgguard" }, { "civilian", "civilian2" },
};

static const char *frontCharacterName(s32 mpbodynum)
{
	const char *name = modBorrowBodyName(g_MpBodies[mpbodynum].bodynum);

	return name ? name : mpGetBodyName(mpbodynum);
}

/** The character's name, the letters before any bracket, lower case. */
static void frontPortraitKey(s32 mpbodynum, char *key, s32 len)
{
	const char *name = frontCharacterName(mpbodynum);
	s32 n = 0;

	for (; name && *name && *name != '(' && n < len - 1; name++) {
		if (*name >= 'A' && *name <= 'Z') {
			key[n++] = *name + 32;
		} else if (*name >= 'a' && *name <= 'z') {
			key[n++] = *name;
		}
	}

	key[n] = '\0';
}

static const s32 *frontPortrait(s32 mpbodynum)
{
	char key[64];

	frontPortraitKey(mpbodynum, key, sizeof(key));

	for (s32 i = 0; i < ARRAYCOUNT(g_FrontPortraits); i++) {
		if (strstr(key, g_FrontPortraits[i].key)) {
			return g_FrontPortraits[i].tiles;
		}
	}

	return g_FrontRandomPortrait;
}

/** The release's portrait's name for a character, Random's where it has none of its own; NULL for the Bonds it lacks. */
static const char *frontReleasePortrait(s32 mpbodynum)
{
	static char name[48];
	char key[64];

	frontPortraitKey(mpbodynum, key, sizeof(key));

	for (s32 i = 0; i < ARRAYCOUNT(g_FrontReleasePortraits); i++) {
		if (strstr(key, g_FrontReleasePortraits[i].key)) {
			if (!g_FrontReleasePortraits[i].picture) {
				return NULL;
			}

			snprintf(name, sizeof(name), "characters/%s", g_FrontReleasePortraits[i].picture);

			return name;
		}
	}

	return "characters/who";
}

/**
 * The Characters page's list: GoldenEye X's characters where they are
 * borrowed, else every Combat Simulator body.
 */
static void frontBuildCharacters(void)
{
	g_Front.numcharacters = 0;

	for (s32 i = 0; i < g_MpListCounts.bodies && g_Front.numcharacters < MAX_CHARACTERS; i++) {
		if (modBorrowBodyName(g_MpBodies[i].bodynum)) {
			g_Front.characters[g_Front.numcharacters++] = i;
		}
	}

	if (g_Front.numcharacters == 0) {
		for (s32 i = 0; i < g_MpListCounts.bodies && g_Front.numcharacters < MAX_CHARACTERS; i++) {
			if (challengeIsFeatureUnlocked(g_MpBodies[i].requirefeature)) {
				g_Front.characters[g_Front.numcharacters++] = i;
			}
		}
	}
}

/* ---- the setup ---------------------------------------------------------- */

// GoldenEye's scenarios in its own order, as the remake's
static const s32 g_FrontScenarios[] = {
	GEXPLUS_NORMAL, GEXPLUS_YOLT, GEXPLUS_FLAGTAG, GEXPLUS_GOLDENGUN, GEXPLUS_LICENCETOKILL,
};

// and their names in LtitleE
static s32 frontScenarioString(s32 scenario)
{
	switch (scenario) {
	case GEXPLUS_YOLT: return 54;
	case GEXPLUS_FLAGTAG: return 55;
	case GEXPLUS_GOLDENGUN: return 56;
	case GEXPLUS_LICENCETOKILL: return 57;
	}

	return TITLE_SCEN_NORMAL;
}

/** GoldenEye's game lengths: {minutes, points}, 0 for none; the last is You Only Live Twice's. */
static const s32 g_FrontLengths[8][2] = {
	{ 0, 0 }, { 5, 0 }, { 10, 0 }, { 20, 0 }, { 0, 5 }, { 0, 10 }, { 0, 20 }, { 0, 0 },
};

static void frontApplyLength(void)
{
	const s32 mins = g_FrontLengths[g_Front.gamelength][0];
	const s32 points = g_FrontLengths[g_Front.gamelength][1];

	g_MpSetup.timelimit = mins ? mins - 1 : 60;
	g_MpSetup.scorelimit = points ? points - 1 : 100;
}

static s32 frontLengthFromSetup(void)
{
	for (s32 i = 1; i < 7; i++) {
		const s32 time = g_FrontLengths[i][0] ? g_FrontLengths[i][0] - 1 : 60;
		const s32 score = g_FrontLengths[i][1] ? g_FrontLengths[i][1] - 1 : 100;

		if (g_MpSetup.timelimit == time && g_MpSetup.scorelimit == score) {
			return i;
		}
	}

	return g_MpSetup.timelimit >= 60 && g_MpSetup.scorelimit >= 100 ? 0 : 2;
}

/**
 * reset_mp_options_for_scenario(): You Only Live Twice is won by the last one
 * alive and its length is fixed; The Living Daylights has no point limits.
 */
static void frontApplyScenarioRules(void)
{
	const s32 scenario = gexPlusGetScenario();

	if (scenario == GEXPLUS_YOLT) {
		g_Front.gamelength = 7;
	} else if (g_Front.gamelength == 7) {
		g_Front.gamelength = 2;
	}

	if (scenario == GEXPLUS_FLAGTAG && g_Front.gamelength > 3) {
		g_Front.gamelength = 2;
	}

	frontApplyLength();
}

static s32 frontNumControllers(void)
{
	const u32 mask = joyGetConnectedControllers();
	s32 count = 0;

	for (s32 i = 0; i < MAX_PLAYERS; i++) {
		if (mask & (1 << i)) {
			count++;
		}
	}

	return count > 0 ? count : 1;
}

static s32 frontNumPlayers(void)
{
	s32 count = 0;

	for (s32 i = 0; i < MAX_PLAYERS; i++) {
		if (g_MpSetup.chrslots & (1 << i)) {
			count++;
		}
	}

	return count > 0 ? count : 1;
}

static void frontSetPlayers(s32 count)
{
	g_MpSetup.chrslots &= ~0xfull;

	for (s32 i = 0; i < count; i++) {
		g_MpSetup.chrslots |= 1 << i;
	}
}

// the counts the Simulants row steps through: one at a time to 8, then in bigger steps to the cap
static s32 frontNextSimCount(s32 count)
{
	static const s32 steps[] = { 10, 12, 16, 20, 24, 32, 40, 48, 64 };
	const s32 cap = mpGetSimSlotCap();

	if (count < 8) {
		return count + 1 <= cap ? count + 1 : 0;
	}

	for (s32 i = 0; i < ARRAYCOUNT(steps); i++) {
		if (steps[i] > count) {
			return steps[i] <= cap ? steps[i] : (count < cap ? cap : 0);
		}
	}

	return count < cap ? cap : 0;
}

static s32 frontNextSimDifficulty(s32 difficulty)
{
	for (s32 i = 1; i <= NUM_BOTDIFFS; i++) {
		const s32 next = (difficulty + i) % NUM_BOTDIFFS;

		if (challengeIsFeatureUnlocked(g_BotProfiles[next].requirefeature)) {
			return next;
		}
	}

	return difficulty;
}

/**
 * MP_handicap_table's health as Perfect Dark's handicap: GoldenEye multiplies
 * the damage a player takes by its modifier, and Perfect Dark divides it by
 * mpHandicapToDamageScale(handicap), so the scale is the modifier's inverse.
 */
static const f32 g_FrontHandicapModifiers[NUM_HANDICAPS] = {
	10.0f, 2.8560996f, 2.1969998f, 1.6899998f, 1.3f, 1.0f, 0.76923078f, 0.59171599f, 0.45516616f, 0.35012782f, 0.1f,
};

static u8 frontHandicapValue(s32 index)
{
	s32 best = 127;
	f32 bestdiff = 1e9f;
	const f32 scale = 1.0f / g_FrontHandicapModifiers[index];

	for (s32 v = 0; v < 256; v++) {
		f32 diff = mpHandicapToDamageScale(v) - scale;

		diff = diff < 0 ? -diff : diff;

		if (diff < bestdiff) {
			bestdiff = diff;
			best = v;
		}
	}

	return best;
}

static s32 frontHandicapIndex(u8 value)
{
	s32 best = 5;
	f32 bestdiff = 1e9f;

	for (s32 i = 0; i < NUM_HANDICAPS; i++) {
		f32 diff = mpHandicapToDamageScale(value) * g_FrontHandicapModifiers[i] - 1.0f;

		diff = diff < 0 ? -diff : diff;

		if (diff < bestdiff) {
			bestdiff = diff;
			best = i;
		}
	}

	return best;
}

static void frontNextWeaponSet(void)
{
	s32 first = 0;
	const s32 num = gexPlusWeaponSets(&first);

	if (num <= 0) {
		// Perfect Dark's guns asked for as well (Mod.GePlusPdGuns): the whole list
		if (gexPlusGetPdGuns() && g_MpNumWeaponSets > 0) {
			g_MpWeaponSetNum = g_MpWeaponSetNum + 1 < g_MpNumWeaponSets ? g_MpWeaponSetNum + 1 : 0;
			mpApplyWeaponSet();
		}

		return;
	}

	g_MpWeaponSetNum = g_MpWeaponSetNum >= first && g_MpWeaponSetNum + 1 < first + num ? g_MpWeaponSetNum + 1 : first;
	mpApplyWeaponSet();
}

static void frontApplyAim(void)
{
	// mp_sight_adjust_table: {sight, auto aim}
	const s32 sight = g_Front.aim & 1;
	const s32 autoaim = (g_Front.aim >> 1) & 1;

	for (s32 i = 0; i < MAX_PLAYERS; i++) {
		g_PlayerConfigsArray[i].options &= ~(OPTION_SIGHTONSCREEN | OPTION_AUTOAIM);

		if (sight) {
			g_PlayerConfigsArray[i].options |= OPTION_SIGHTONSCREEN;
		}

		if (autoaim) {
			g_PlayerConfigsArray[i].options |= OPTION_AUTOAIM;
		}
	}
}

static s32 frontRowOn(s32 row)
{
	switch (row) {
	case ROW_SIMDIFF:
		return g_Vars.mpquickteamnumsims > 0;
	case ROW_GAMELENGTH:
		return gexPlusGetScenario() != GEXPLUS_YOLT;
	case ROW_WEAPONS:
		return gexPlusGetScenario() != GEXPLUS_GOLDENGUN;
	case ROW_HEALTH:
		// reset_mp_options_for_scenario(): License to Kill kills in one hit anyway
		return gexPlusGetScenario() != GEXPLUS_LICENCETOKILL;
	case ROW_CHARACTERS:
		return g_Front.numcharacters > 0 || (frontBuildCharacters(), g_Front.numcharacters > 0);
	}

	return 1;
}

static void frontSelectRow(s32 row)
{
	switch (row) {
	case ROW_PLAYERS:
		frontSetPlayers(frontNumPlayers() % frontNumControllers() + 1);
		break;
	case ROW_SIMULANTS:
		g_Vars.mpquickteamnumsims = frontNextSimCount(g_Vars.mpquickteamnumsims);
		break;
	case ROW_SIMDIFF:
		g_Vars.mpsimdifficulty = frontNextSimDifficulty(g_Vars.mpsimdifficulty);
		break;
	case ROW_SCENARIO:
		g_Front.screen = SCREEN_SCENARIO;
		break;
	case ROW_LEVEL:
		frontBuildLevels();
		g_Front.levelpage = 0;

		// the page the chosen arena is on
		for (s32 i = 0; i < g_Front.numlevels; i++) {
			if (g_Front.levels[i] == g_MpSetup.stagenum) {
				g_Front.levelpage = i / LEVELS_PER_PAGE;
			}
		}

		g_Front.screen = SCREEN_LEVEL;
		break;
	case ROW_CHARACTERS:
		frontBuildCharacters();

		// init_menu0f_mpcharsel(): each player on their own character
		for (s32 i = 0; i < MAX_PLAYERS; i++) {
			g_Front.charcur[i] = 0;

			for (s32 k = 0; k < g_Front.numcharacters; k++) {
				if (g_Front.characters[k] == g_PlayerConfigsArray[i].base.mpbodynum) {
					g_Front.charcur[i] = k;
				}
			}

			g_Front.charprev[i] = g_Front.charcur[i];
			g_Front.charscroll[i] = 0;
			g_Front.charsize[i] = 0;
			g_Front.chosen[i] = 0;
			g_Front.stickarmed[i] = 0;
		}

		g_Front.screen = SCREEN_CHARACTERS;
		break;
	case ROW_HEALTH:
	case ROW_CONTROLSTYLE:
		for (s32 i = 0; i < MAX_PLAYERS; i++) {
			g_Front.chosen[i] = 0;
			g_Front.stickarmed[i] = 0;
			g_Front.handicap[i] = frontHandicapIndex(g_PlayerConfigsArray[i].handicap);
		}

		g_Front.screen = row == ROW_HEALTH ? SCREEN_HEALTH : SCREEN_CONTROLSTYLE;
		break;
	case ROW_GAMELENGTH:
		// select_game_length(): The Living Daylights has the times only
		g_Front.gamelength = (g_Front.gamelength + 1) % (gexPlusGetScenario() == GEXPLUS_FLAGTAG ? 4 : 7);
		frontApplyLength();
		break;
	case ROW_WEAPONS:
		frontNextWeaponSet();
		break;
	case ROW_AIM:
		g_Front.aim = (g_Front.aim + 1) % 4;
		frontApplyAim();
		break;
	}
}

/**
 * The setup as GE Plus's Combat Simulator has it (mainmenu.c's GE Plus
 * row), with the players and simulants of the folder's rows.
 */
/**
 * Player 1 is who they chose on the Perfect Menu's Customize Character
 * (g_ModCiBody, modghost.h), as they are in the Institute and the missions,
 * until they choose on the folder's own Characters page - which then wins for
 * the session. Without it GE Plus kept the multiplayer setup's body, which
 * for most players is Joanna.
 */
static void frontApplyMenuCharacter(void)
{
	const s32 body = g_ModCiBody - 1;
	const s32 head = g_ModCiHead - 1;

	if (g_Front.charpicked || g_ModCiBody <= MODGHOST_BODY_DEFAULT || body >= g_MpListCounts.bodies) {
		return;
	}

	g_PlayerConfigsArray[0].base.mpbodynum = body;
	g_PlayerConfigsArray[0].base.mpheadnum = g_ModCiHead > MODGHOST_BODY_DEFAULT && head < mpGetNumHeads2()
		? head : modGhostBodyDefaultHead(body);
}

static void frontEnterSetup(void)
{
	const s32 first = frontFirstArena();

	frontApplyMenuCharacter();

	mpSetGexPlusMode(true);

	if (first >= 0 && !modloaderStageIsRemake(g_MpSetup.stagenum)) {
		g_MpSetup.stagenum = g_MpArenas[first].stagenum;
	}

	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = -1;
	g_Vars.antiplayernum = -1;
	challengeDetermineUnlockedFeatures();

	// A setup with no player's slot in it (a fresh one, or one only ever used
	// for simulants) reads as one player on the Players row but starts a match
	// with nobody in it: the human's stats then name mpindex 4, past
	// g_Menus[], so Start pushed its pause menu into memory that is no
	// player's, paused the match and showed nothing (a tester's report).
	if ((g_MpSetup.chrslots & 0xf) == 0) {
		frontSetPlayers(1);
	}

	if (frontNumPlayers() > frontNumControllers()) {
		frontSetPlayers(frontNumControllers());
	}

	if (g_Vars.mpquickteamnumsims < 0 || g_Vars.mpquickteamnumsims > mpGetSimSlotCap()) {
		g_Vars.mpquickteamnumsims = 0;
	}

	if (g_Vars.mpsimdifficulty < 0 || g_Vars.mpsimdifficulty >= NUM_BOTDIFFS) {
		g_Vars.mpsimdifficulty = BOTDIFF_NORMAL;
	}

	g_Front.gamelength = frontLengthFromSetup();
	g_Front.aim = ((g_PlayerConfigsArray[0].options & OPTION_SIGHTONSCREEN) ? 1 : 0)
		| ((g_PlayerConfigsArray[0].options & OPTION_AUTOAIM) ? 2 : 0);
	frontApplyScenarioRules();
}

static void frontStartMatch(void)
{
	if ((g_MpSetup.chrslots & 0xf) == 0) {
		frontSetPlayers(1);
	}

	g_Vars.mpsetupmenu = MPSETUPMENU_GENERAL;
	g_Vars.mpquickteam = g_Vars.mpquickteamnumsims > 0 ? MPQUICKTEAM_PLAYERSANDSIMS : MPQUICKTEAM_PLAYERSONLY;
	mpConfigureQuickTeamPlayers();
	frontApplyAim();

	g_Front.active = 0;
	frontUnload();

	// closes the Perfect Menu; menuTick() starts the match once it has gone
	func0f0f820c(NULL, -5);
}

/**
 * init_menu0B_runstage(): the mission the folder settled on, at the difficulty
 * it settled on. GoldenEye's first three difficulties are Perfect Dark's three;
 * its 007 is Perfect Dark's PD Mode, the sliders over the hardest difficulty,
 * and the fields are the same multipliers (pdmode.c) - except the reaction
 * speed, which GoldenEye alone acts on, so its slider only shows here.
 */
static void frontStartMission(void)
{
	union handlerdata data;

	g_MissionConfig.stageindex = g_Front.mission;
	g_MissionConfig.stagenum = frontMissionStage(g_Front.mission);
	g_MissionConfig.iscoop = false;
	g_MissionConfig.isanti = false;
	g_MissionConfig.pdmode = g_Front.difficulty == DIFFICULTY_007;
	g_MissionConfig.difficulty = g_MissionConfig.pdmode ? DIFF_PA : g_Front.difficulty;

	if (g_MissionConfig.pdmode) {
		g_MissionConfig.pdmodehealthf = g_Front.slider[SLIDER_HEALTH];
		g_MissionConfig.pdmodedamagef = g_Front.slider[SLIDER_DAMAGE];
		g_MissionConfig.pdmodeaccuracyf = g_Front.slider[SLIDER_ACCURACY];
		g_MissionConfig.pdmodereactionf = g_Front.slider[SLIDER_REACTION];
	}

	g_Front.active = 0;
	frontUnload();

	// as the Perfect Menu's own Accept Mission does it, from the same tick
	menuhandlerAcceptMission(MENUOP_SET, NULL, &data);
}

/**
 * Inside GE Plus: from the folder opening until the player backs out of its
 * mode select to the Perfect Menu. Starting a mission, a match or a cinema from
 * it does not end that - the folder is put away while the level runs, and
 * whatever the level ends in comes back to the folder, not to Perfect Dark's
 * menus. "While inside GE Plus, let the main menu be GE Plus's main menu."
 */
static s32 g_FrontInside;
// a mission started from the folder has ended: the Institute's menu comes up as
// the folder's own main menu (menutick.c)
static s32 g_FrontWantMain;

static void frontClose(void)
{
	g_Front.active = 0;
	g_FrontInside = 0;
	frontUnload();

	// the Perfect Menu's own tune again
	musicStartMenu();
}

/* ---- input -------------------------------------------------------------- */

static void frontMoveCursor(void)
{
	// frontUpdateControlStickPosition(): a 5 dead zone, 70 at most
	s32 stickx = joyGetStickX(0);
	s32 sticky = -joyGetStickY(0);
	const f32 frames = g_Vars.diffframe60freal;
	s32 mx;
	s32 my;

	stickx = stickx < -5 ? stickx + 5 : stickx > 5 ? stickx - 5 : 0;
	sticky = sticky < -5 ? sticky + 5 : sticky > 5 ? sticky - 5 : 0;
	stickx = stickx > 70 ? 70 : stickx < -70 ? -70 : stickx;
	sticky = sticky > 70 ? 70 : sticky < -70 ? -70 : sticky;

	if (stickx > 0) {
		g_Front.cursorx += (stickx * 0.075f + 0.5f) * frames;
	} else if (stickx < 0) {
		g_Front.cursorx += (stickx * 0.075f - 0.5f) * frames;
	}

	if (sticky > 0) {
		g_Front.cursory += (sticky * 0.075f + 0.5f) * frames;
	} else if (sticky < 0) {
		g_Front.cursory += (sticky * 0.075f - 0.5f) * frames;
	}

	// the mouse, where it is, in the 4:3 frame the folder is drawn in - once it
	// has moved from where it was when last looked at, or a pointer resting on
	// the window would pin the cursor wherever it rests
	if (inputMouseIsEnabled() && !inputMouseIsLocked()) {
		inputMouseGetPosition(&mx, &my);

		if (g_Front.mouseseen && (mx != g_Front.mousex || my != g_Front.mousey)) {
			// frontX() read backwards: the pointer is in this frame's 320x220
			// and G_ASPECT_CENTER_EXT holds what is drawn at SCREEN_ASPECT
			g_Front.cursorx = GEFRONT_W / 2
				+ ((f32)mx - SCREEN_WIDTH_LO / 2) * videoGetAspect() * GEFRONT_H / SCREEN_WIDTH_LO;
			g_Front.cursory = (f32)my * GEFRONT_H / SCREEN_HEIGHT_LO;
		}

		g_Front.mousex = mx;
		g_Front.mousey = my;
		g_Front.mouseseen = 1;
	}

	if (g_Front.cursorx > GEFRONT_W - 20) g_Front.cursorx = GEFRONT_W - 20;
	if (g_Front.cursorx < 20) g_Front.cursorx = 20;
	if (g_Front.cursory > GEFRONT_H - 20) g_Front.cursory = GEFRONT_H - 20;
	if (g_Front.cursory < 20) g_Front.cursory = 20;
}

static s32 frontOnPrevTab(void)
{
	return TABS_LEFT_EDGE < g_Front.cursorx && PREVTAB_TAB_TOP < g_Front.cursory;
}

static s32 frontOnStartTab(void)
{
	return TABS_LEFT_EDGE < g_Front.cursorx && g_Front.cursory <= STARTTAB_TAB_BOTTOM;
}

static s32 frontOnNextTab(void)
{
	return TABS_LEFT_EDGE < g_Front.cursorx && NEXTTAB_TAB_TOP < g_Front.cursory && g_Front.cursory <= PREVTAB_TAB_TOP;
}

/** interface_menu12_mpstage(): the picture under the cursor, the NEXT tab to turn the page. */
static void frontSetCursorForMode(s32 mode);

/**
 * The Cinema page is the mission select's own: the slides on the grid, a
 * mission's name on each, and the cursor finds them the same way
 * (frontTickMission()). It was the multiplayer Level page's film strip, two
 * pages of stage pictures, until the user asked for this one.
 *
 * Picking a mission opens a page in the difficulty page's shape with the two
 * things there are to watch: its opening - every one of its camera shots in
 * turn, then the fade and the swirl down to Bond - and its ending (gecinema.c).
 */
static void frontSetCursorForMission(s32 mission);
static s32 frontMissionUnderCursor(void);
static void frontOpenExtra(s32 row);

#define NUM_EXTRA_ROWS 2
#define EXTRA_CINEMA   0
#define EXTRA_MONITORS 1

#define NUM_CINEMA_ROWS 2

static void frontSetCursorForCinemaPick(s32 what)
{
	g_Front.cursorx = 106.0f;
	g_Front.cursory = what * 0x1e + 0xba;
}

static void frontOpenCinema(void)
{
	g_Front.screen = SCREEN_CINEMA;
	frontSetCursorForMission(g_Front.mission);
}

static void frontStartCinema(s32 mission, s32 what)
{
	union handlerdata data;

	g_MissionConfig.stageindex = mission;
	g_MissionConfig.stagenum = frontMissionStage(mission);
	g_MissionConfig.iscoop = false;
	g_MissionConfig.isanti = false;
	g_MissionConfig.pdmode = false;
	g_MissionConfig.difficulty = DIFF_A;

	// the stage that loads next is a cinema rather than a mission to play
	gecinemaArm(mission, what);

	g_Front.active = 0;
	frontUnload();

	menuhandlerAcceptMission(MENUOP_SET, NULL, &data);
}

/**
 * A sound of the folder's, out of GoldenEye's own bank (gesfx.c). GoldenEye's
 * front end has three: DOOR_METAL_CLOSE2 for every accept, back and tab,
 * DOOR_METAL_CLOSE for the mode select's choices and PAPER_TURN for a
 * difficulty picked - and none at all for a cursor or a value that moves, or
 * for a press on something that is not there. Perfect Dark's own is played
 * only where the conversion has no bank: one made before version 39 and kept
 * because the ROM has gone since.
 */
static void frontSfx(s32 id, s32 menusound)
{
	if (!geSfxPlay(id, GESFX_VOLUME) && geSfxGet(id) <= 0) {
		menuPlaySound(menusound);
	}
}

static void frontTickCinema(s32 pick, s32 back)
{
	if (!g_Front.tabprev) {
		g_Front.highlight = frontMissionUnderCursor();
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		frontOpenExtra(EXTRA_CINEMA);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		g_Front.mission = g_Front.highlight;
		g_Front.screen = SCREEN_CINEMAPICK;
		frontSetCursorForCinemaPick(GECINEMA_OPENING);
	}
}

/**
 * EXTRA, the mode select's third row: the remake's own page for what GoldenEye
 * never had a menu for. 1. Cinema, 2. Monitor Programmes - the user's names and
 * the user's order - in the difficulty page's rows.
 */
static void frontSetCursorForExtra(s32 row)
{
	g_Front.cursorx = 106.0f;
	g_Front.cursory = row * 0x1e + 0xba;
}

static void frontOpenExtra(s32 row)
{
	g_Front.screen = SCREEN_EXTRA;
	frontSetCursorForExtra(row);
}

/** Start programme n on the page's screen, from GoldenEye's own blank one. */
static void frontShowMonitor(s32 n)
{
	u32 *list;

	if (g_Front.nummonitors <= 0) {
		return;
	}

	g_Front.monitor = ((n % g_Front.nummonitors) + g_Front.nummonitors) % g_Front.nummonitors;
	list = geMonitorProgramAt(g_Front.monitor);

	if (list) {
		// the screen every monitor is made from (setup.c's var8009ce98, which
		// GoldenEye calls g_MonitorAnimController): white, unscrolled, 1:1
		g_Front.monitorscreen = var8009ce98;
		tvscreenSetCmdlist(&g_Front.monitorscreen, list);
	}
}

/**
 * Monitor Programmes, as the user asked for it: "each in their own monitor
 * prop, lined up similar to the mission reels". Twelve of GoldenEye's TV sets a
 * page (PROP_TV1, which the conversion writes as Pgx075Z), four across and
 * three down on the Level page's film strip pitch, each running its own
 * programme on its own screen exactly as a set in a level does -
 * tvscreenRender() onto the model's part 0 and then the model (objRender()).
 * NEXT turns the page; a press on a set shows its programme large.
 */
#define TVS_PER_PAGE 12
#define TV_FILE "Pgx075Z"

// where a set stands and how it is turned, against the folder's own camera:
// not constants so that they can be found from gdb with the page on the screen
// (volatile, or the compiler folds them in and gdb has nothing to set)
static volatile f32 g_TvScale = 0.2f;
static volatile f32 g_TvYaw = 0.0f;
static volatile f32 g_TvPitch = 0.0f;
static volatile f32 g_TvZ = 0.0f;
static volatile f32 g_TvLift = 0.0f;
// a menu pixel across the folder's plane, measured off the page: the frame the
// 2-D layer is held in is not the window's own shape (frontX())
static volatile f32 g_TvSpreadX = 0.9327f;   // 0.855 * 12 / 11: measured inside the 2-D frame on 4:3, drawn outside it
static volatile f32 g_TvSpreadY = 0.96f;

static void frontUnloadTvs(void)
{
	for (s32 i = 0; i < TVS_PER_PAGE; i++) {
		if (g_Front.tvmodels[i]) {
			modelmgrFreeModel(g_Front.tvmodels[i]);
			g_Front.tvmodels[i] = NULL;
		}
	}

	if (g_Front.tvbuf) {
		videoFreeCachedTextures(g_Front.tvbuf, g_Front.tvbuf + g_Front.tvbuflen);
		sysMemFree(g_Front.tvbuf);
		g_Front.tvbuf = NULL;
	}

	g_Front.tvdef = NULL;
	geMonitorClose();
}

static s32 frontLoadTvs(void)
{
	const s32 fileid = romdataRegisterModFile(TV_FILE, g_Front.moddir);
	s32 size;

	if (g_Front.tvdef) {
		return 1;
	}

	if (fileid <= 0 || (size = fileGetInflatedSize(fileid, LOADTYPE_MODEL)) <= 0) {
		return 0;
	}

	g_Front.tvbuflen = ALIGN64(size) + 0x20000;
	g_Front.tvbuf = sysMemZeroAlloc(g_Front.tvbuflen);

	if (!g_Front.tvbuf) {
		return 0;
	}

	{
		const s32 prevsrc = modSetTextureSourceMod(g_Front.moddir);

		g_Front.tvdef = modeldefLoad(fileid, g_Front.tvbuf, g_Front.tvbuflen, NULL);
		modSetTextureSourceMod(prevsrc);
	}

	if (!g_Front.tvdef) {
		frontUnloadTvs();
		return 0;
	}

	modelAllocateRwData(g_Front.tvdef);

	for (s32 i = 0; i < TVS_PER_PAGE; i++) {
		g_Front.tvmodels[i] = modelmgrInstantiateModelWithoutAnim(g_Front.tvdef);

		if (!g_Front.tvmodels[i]) {
			frontUnloadTvs();
			return 0;
		}

		modelSetScale(g_Front.tvmodels[i], 1);
	}

	return 1;
}

/** The programmes of a page, each started on its own set. */
static void frontSetMonitorPage(s32 page)
{
	const s32 pages = (g_Front.nummonitors + TVS_PER_PAGE - 1) / TVS_PER_PAGE;

	if (pages <= 0) {
		return;
	}

	g_Front.monitorpage = ((page % pages) + pages) % pages;

	for (s32 i = 0; i < TVS_PER_PAGE; i++) {
		u32 *list = geMonitorProgramAt(g_Front.monitorpage * TVS_PER_PAGE + i);

		g_Front.tvscreens[i] = var8009ce98;
		tvscreenSetCmdlist(&g_Front.tvscreens[i], list ? list : geMonitorProgramAt(0));
	}
}

static void frontOpenMonitors(void)
{
	g_Front.nummonitors = geMonitorOpen(g_Front.moddir, fsGetModDirAt(g_Front.moddir));
	frontLoadTvs();
	g_Front.screen = SCREEN_MONITORS;
	frontSetMonitorPage(g_Front.monitor / TVS_PER_PAGE);

	// the cursor on the set that was last looked at, as the Level page does
	g_Front.cursorx = 86.0f + 85.0f * ((g_Front.monitor % TVS_PER_PAGE) % 4) + 17.0f;
	g_Front.cursory = 134.0f + 70.0f * ((g_Front.monitor % TVS_PER_PAGE) / 4) + 11.0f;
}

static void frontTickExtra(s32 pick, s32 back)
{
	if (!g_Front.tabprev) {
		g_Front.highlight = g_Front.cursory >= 211 ? EXTRA_MONITORS : EXTRA_CINEMA;
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MODE;
		frontSetCursorForMode(2);
		return;
	}

	if (pick && g_Front.highlight == EXTRA_CINEMA) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		frontOpenCinema();
	} else if (pick && g_Front.highlight == EXTRA_MONITORS) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		frontOpenMonitors();
	}
}

/**
 * A screen counts in the level's clock, which is stopped while the folder is
 * up - so the page lends it the frame's own, or nothing scrolls, nothing tints
 * and no hold ever ends.
 */
static void frontMonitorClock(s32 lend, s32 *lvupdate60, f32 *lvupdate60f)
{
	if (lend) {
		*lvupdate60 = g_Vars.lvupdate60;
		*lvupdate60f = g_Vars.lvupdate60f;
		g_Vars.lvupdate60 = g_Vars.diffframe60;
		g_Vars.lvupdate60f = g_Vars.diffframe60f;
	} else {
		g_Vars.lvupdate60 = *lvupdate60;
		g_Vars.lvupdate60f = *lvupdate60f;
	}
}

/** The page of sets: the one under the cursor, the NEXT tab, a press to look closer. */
static void frontTickMonitors(s32 pick, s32 back)
{
	const s32 first = g_Front.monitorpage * TVS_PER_PAGE;
	const s32 onpage = g_Front.nummonitors - first < TVS_PER_PAGE ? g_Front.nummonitors - first : TVS_PER_PAGE;

	if (!g_Front.tabprev && !g_Front.tabnext) {
		const s32 y = (s32)g_Front.cursory;
		const s32 x = (s32)g_Front.cursorx;
		const s32 row = y >= 240 ? 2 : y >= 170 ? 1 : 0;
		const s32 col = x >= 292 ? 3 : x >= 207 ? 2 : x >= 122 ? 1 : 0;

		g_Front.highlight = row * 4 + col;

		if (g_Front.highlight >= onpage) {
			g_Front.highlight = onpage - 1;
		}
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		frontUnloadTvs();
		frontOpenExtra(EXTRA_MONITORS);
		return;
	}

	if (pick && g_Front.tabnext) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SWIPE);
		frontSetMonitorPage(g_Front.monitorpage + 1);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		g_Front.screen = SCREEN_MONITORVIEW;
		frontShowMonitor(first + g_Front.highlight);
	}
}

/**
 * One programme large: a press on the left of the picture goes back one and
 * anywhere else - or the NEXT tab - goes on one; backing out returns to the
 * page of sets that programme is on.
 */
static void frontTickMonitorView(s32 pick, s32 back)
{
	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		frontOpenMonitors();
		return;
	}

	if (pick) {
		const s32 step = !g_Front.tabnext && g_Front.cursorx < 220.0f ? -1 : 1;

		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SWIPE);
		frontShowMonitor(g_Front.monitor + step);
	}

}

/** The opening or the ending: the difficulty page's rows and its thresholds. */
static void frontTickCinemaPick(s32 pick, s32 back)
{
	if (!g_Front.tabprev) {
		g_Front.highlight = g_Front.cursory >= 211 ? GECINEMA_ENDING : GECINEMA_OPENING;
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		frontOpenCinema();
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_PAPER_TURN, MENUSOUND_SWIPE);
		g_Front.cinemawhat = g_Front.highlight;
		frontStartCinema(g_Front.mission, g_Front.highlight);
	}
}

static void frontTickLevel(s32 pick, s32 back)
{
	const s32 first = g_Front.levelpage * LEVELS_PER_PAGE;
	const s32 onpage = g_Front.numlevels - first < LEVELS_PER_PAGE ? g_Front.numlevels - first : LEVELS_PER_PAGE;

	if (!g_Front.tabprev && !g_Front.tabnext) {
		const s32 y = (s32)g_Front.cursory;
		const s32 x = (s32)g_Front.cursorx;
		const s32 row = y >= 240 ? 2 : y >= 170 ? 1 : 0;
		const s32 col = x >= 292 ? 3 : x >= 207 ? 2 : x >= 122 ? 1 : 0;

		g_Front.highlight = row * 4 + col;

		if (g_Front.highlight >= onpage) {
			g_Front.highlight = onpage - 1;
		}
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MPOPTIONS;
		return;
	}

	if (pick && g_Front.tabnext) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SWIPE);
		g_Front.levelpage = (g_Front.levelpage + 1) % ((g_Front.numlevels + LEVELS_PER_PAGE - 1) / LEVELS_PER_PAGE);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		g_MpSetup.stagenum = g_Front.levels[first + g_Front.highlight];
		g_Front.screen = SCREEN_MPOPTIONS;
	}
}

static s32 frontScenarioRowOn(s32 row)
{
	return row < NUM_SOLO_SCENARIOS;
}

/** interface_menu13_mpscenario(): the lowest row the cursor is at or below whose game can be played. */
static void frontTickScenario(s32 pick, s32 back)
{
	if (!g_Front.tabprev) {
		g_Front.highlight = 0;

		for (s32 i = NUM_SCENARIO_ROWS - 1; i > 0; i--) {
			if ((s32)g_Front.cursory >= 0x83 + i * 0x16 && frontScenarioRowOn(i)) {
				g_Front.highlight = i;
				break;
			}
		}
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MPOPTIONS;
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		gexPlusSetScenario(g_FrontScenarios[g_Front.highlight]);
		frontApplyScenarioRules();
		g_Front.screen = SCREEN_MPOPTIONS;
	}
}

static void frontSetControlStyle(s32 player, s32 style)
{
	optionsSetControlMode(player, style);
	g_PlayerExtCfg[player & 3].extcontrols = style == CONTROLMODE_PC;
	g_Vars.modifiedfiles |= MODFILE_GAME;
}

/**
 * interface_menu10_mphandicap() and interface_menu11_mpcontrols(): each player
 * steps their own value left and right on their own controller (the pad, the C
 * buttons, or a flick of the stick that has been back in the middle since),
 * A or Z chooses and B takes it back; the page closes when every player has
 * chosen. The keyboard's accept and cancel, and a click, are player 1's.
 */
static void frontTickPlayerPanels(void)
{
	const s32 numplayers = frontNumPlayers();
	s32 ready = 0;

	for (s32 i = 0; i < numplayers; i++) {
		const s32 stickx = joyGetStickX(i);
		const s32 left = joyGetButtonsPressedThisFrame(i, L_JPAD | L_CBUTTONS) || (stickx < -30 && g_Front.stickarmed[i]);
		const s32 right = joyGetButtonsPressedThisFrame(i, R_JPAD | R_CBUTTONS) || (stickx > 30 && g_Front.stickarmed[i]);
		const s32 pick = joyGetButtonsPressedThisFrame(i, A_BUTTON | Z_TRIG | START_BUTTON | (i == 0 ? BUTTON_UI_ACCEPT : 0))
			|| (i == 0 && inputKeyJustPressed(VK_MOUSE_LEFT));
		const s32 unpick = joyGetButtonsPressedThisFrame(i, B_BUTTON | (i == 0 ? BUTTON_UI_CANCEL : 0))
			|| (i == 0 && inputKeyJustPressed(VK_ESCAPE));

		if (g_Front.chosen[i]) {
			if (unpick) {
				g_Front.chosen[i] = 0;
				frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
			}
		} else if (left || right) {
			if (g_Front.screen == SCREEN_HEALTH) {
				const s32 next = g_Front.handicap[i] + (right ? 1 : -1);

				if (next >= 0 && next < NUM_HANDICAPS) {
					g_Front.handicap[i] = next;
					g_PlayerConfigsArray[i].handicap = frontHandicapValue(next);
				}
			} else {
				const s32 next = optionsGetControlMode(i) + (right ? 1 : -1);

				if (next >= 0 && next < NUM_CONTROLSTYLES) {
					frontSetControlStyle(i, next);
				}
			}
		} else if (pick) {
			g_Front.chosen[i] = 1;
			frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		}

		g_Front.stickarmed[i] = stickx >= -10 && stickx <= 10;

		if (g_Front.chosen[i]) {
			ready++;
		}
	}

	if (ready == numplayers) {
		g_Front.screen = SCREEN_MPOPTIONS;
	}
}

/** Whether another player has already chosen this character (get_players_who_have_selected_mp_char()). */
static s32 frontCharacterTaken(s32 player, s32 k)
{
	for (s32 i = 0; i < frontNumPlayers(); i++) {
		if (i != player && g_Front.chosen[i] && g_Front.charcur[i] == k) {
			return 1;
		}
	}

	return 0;
}

/**
 * interface_menu0F_mpcharsel(): each player steps along the strip on their own
 * controller and chooses with A, which another player's choice refuses; a
 * chosen portrait grows for eleven frames and B puts it back. The strip
 * scrolls twelve a frame to the player's character. The page closes when every
 * player has chosen and their portrait is grown.
 */
static void frontTickCharacters(void)
{
	const s32 numplayers = frontNumPlayers();
	s32 ready = 0;

	for (s32 i = 0; i < numplayers; i++) {
		const s32 stickx = joyGetStickX(i);
		const s32 left = joyGetButtonsPressedThisFrame(i, L_JPAD | L_CBUTTONS) || (stickx < -30 && g_Front.stickarmed[i]);
		const s32 right = joyGetButtonsPressedThisFrame(i, R_JPAD | R_CBUTTONS) || (stickx > 30 && g_Front.stickarmed[i]);
		const s32 pick = joyGetButtonsPressedThisFrame(i, A_BUTTON | Z_TRIG | START_BUTTON | (i == 0 ? BUTTON_UI_ACCEPT : 0))
			|| (i == 0 && inputKeyJustPressed(VK_MOUSE_LEFT));
		const s32 unpick = joyGetButtonsPressedThisFrame(i, B_BUTTON | (i == 0 ? BUTTON_UI_CANCEL : 0))
			|| (i == 0 && inputKeyJustPressed(VK_ESCAPE));

		if (g_Front.chosen[i]) {
			if (g_Front.charsize[i] < 11 && g_Front.charprev[i] == g_Front.charcur[i]) {
				g_Front.charsize[i]++;
			}

			if (unpick) {
				g_Front.chosen[i] = 0;
				frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
			}
		}

		if (!g_Front.chosen[i] && g_Front.charsize[i] > 0) {
			g_Front.charsize[i]--;
		} else if (!g_Front.chosen[i] && g_Front.charscroll[i] == 0) {
			if (left && g_Front.charcur[i] > 0) {
				g_Front.charcur[i]--;
			} else if (right && g_Front.charcur[i] < g_Front.numcharacters - 1) {
				g_Front.charcur[i]++;
			} else if (pick && !frontCharacterTaken(i, g_Front.charcur[i])) {
				const s32 mpbodynum = g_Front.characters[g_Front.charcur[i]];

				g_PlayerConfigsArray[i].base.mpbodynum = mpbodynum;
				g_PlayerConfigsArray[i].base.mpheadnum = mpGetMpheadnumByMpbodynum(mpbodynum);
				g_Front.chosen[i] = 1;

				if (i == 0) {
					g_Front.charpicked = 1;
				}

				g_Front.charsize[i] = 1;
				frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
			}
		}

		g_Front.stickarmed[i] = stickx >= -10 && stickx <= 10;

		// the strip to the player's character, twelve a frame
		{
			const s32 at = g_Front.charprev[i] * PORTRAIT_SPACING + g_Front.charscroll[i];
			const s32 want = g_Front.charcur[i] * PORTRAIT_SPACING;

			if (want < at) {
				g_Front.charscroll[i] -= 12;

				if (g_Front.charscroll[i] < 0) {
					g_Front.charscroll[i] += PORTRAIT_SPACING;
					g_Front.charprev[i]--;
				}
			} else if (want > at) {
				g_Front.charscroll[i] += 12;

				if (g_Front.charscroll[i] >= PORTRAIT_SPACING) {
					g_Front.charscroll[i] -= PORTRAIT_SPACING;
					g_Front.charprev[i]++;
				}
			}
		}

		if (g_Front.chosen[i] && g_Front.charsize[i] == 11) {
			ready++;
		}
	}

	if (ready == numplayers) {
		g_Front.screen = SCREEN_MPOPTIONS;
	}
}

/** set_cursor_to_stage_solo() and set_cursor_pos_difficulty(). */
static void frontSetCursorForMission(s32 mission)
{
	g_Front.cursorx = g_MissionX[mission % MISSION_COLS];
	g_Front.cursory = g_MissionY[mission / MISSION_COLS];
}

static void frontSetCursorForDifficulty(s32 difficulty)
{
	g_Front.cursorx = 106.0f;
	g_Front.cursory = difficulty * 0x1e + 0xba;
}

/**
 * interface_menu07_missionsel(): the cell the cursor is nearest, walked back to
 * a mission that can be played - up the rows first, then left along the row,
 * and right along it when nothing is to the left.
 */
static s32 frontMissionUnderCursor(void)
{
	s32 col = 0;
	s32 row = 0;

	while (col < MISSION_COLS - 1 && (g_MissionX[col] + g_MissionX[col + 1]) * 0.5f <= g_Front.cursorx) {
		col++;
	}

	while (row < MISSION_ROWS - 1 && (g_MissionY[row] + g_MissionY[row + 1]) * 0.5f <= g_Front.cursory) {
		row++;
	}

	for (; row > 0; row--) {
		s32 i;

		for (i = 0; i < MISSION_COLS; i++) {
			if (frontHighestDifficulty(row * MISSION_COLS + i) >= 0) {
				break;
			}
		}

		if (i < MISSION_COLS) {
			break;
		}
	}

	for (; col >= 0; col--) {
		if (frontHighestDifficulty(row * MISSION_COLS + col) >= 0) {
			break;
		}
	}

	if (col < 0) {
		for (col = 0; col < MISSION_COLS; col++) {
			if (frontHighestDifficulty(row * MISSION_COLS + col) >= 0) {
				break;
			}
		}
	}

	return col < MISSION_COLS ? row * MISSION_COLS + col : -1;
}

static void frontTickMission(s32 pick, s32 back)
{
	if (!g_Front.tabprev) {
		g_Front.highlight = frontMissionUnderCursor();
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MODE;
		frontSetCursorForMode(0);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		g_Front.mission = g_Front.highlight;
		g_Front.screen = SCREEN_DIFFICULTY;
		frontSetCursorForDifficulty(frontHighestDifficulty(g_Front.mission));
	}
}

/** interface_menu08_difficulty(): the rows the cursor falls in, up to the highest unlocked. */
static void frontTickDifficulty(s32 pick, s32 back)
{
	// 007, 00 Agent and Secret Agent's own thresholds, Agent below them all
	static const s32 tops[] = { 275, 243, 211 };
	const s32 highest = frontHighestDifficulty(g_Front.mission);

	if (!g_Front.tabprev) {
		g_Front.highlight = 0;

		for (s32 i = 0; i < 3; i++) {
			const s32 d = DIFFICULTY_007 - i;

			if (highest >= d && g_Front.cursory >= tops[i]) {
				g_Front.highlight = d;
				break;
			}
		}
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MISSION;
		frontSetCursorForMission(g_Front.mission);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		frontSfx(GESFX_PAPER_TURN, MENUSOUND_SWIPE);
		g_Front.difficulty = g_Front.highlight;

		if (g_Front.difficulty == DIFFICULTY_007) {
			g_Front.screen = SCREEN_007OPTIONS;
		} else {
			g_Front.screen = SCREEN_BRIEFING;
			g_Front.briefpage = BRIEF_TITLE;
			frontLoadBriefing(g_Front.mission);
		}

		g_Front.cursorx = TABS_LEFT_EDGE + 10;
		g_Front.cursory = (NEXTTAB_TAB_TOP + PREVTAB_TAB_TOP) * 0.5f;
	}
}

/**
 * interface_menu09_007options(): the four sliders, each dragged while the pick
 * is held - 300 wide from x 55, the enemy's health, damage and accuracy the
 * square of the position times ten and the reaction speed the position itself.
 */
static void frontTick007(s32 pick, s32 back, s32 held)
{
	if (!held) {
		g_Front.sliderheld = -1;

		if (!g_Front.tabprev && !g_Front.tabnext && !g_Front.tabstart) {
			const s32 y = (s32)g_Front.cursory;

			g_Front.highlight = y >= 0x107 ? SLIDER_REACTION : y >= 0xe6 ? SLIDER_ACCURACY
				: y >= 0xc5 ? SLIDER_DAMAGE : y >= 0xa4 ? SLIDER_HEALTH : -1;

			// above them all the NEXT tab is what a pick takes, as GoldenEye takes it
			if (g_Front.highlight < 0) {
				g_Front.tabnext = 1;
			}
		}
	}

	if (pick && g_Front.highlight >= 0 && !g_Front.tabprev && !g_Front.tabnext && !g_Front.tabstart) {
		g_Front.sliderheld = g_Front.highlight;
	}

	if (held && g_Front.sliderheld >= 0) {
		f32 x = (g_Front.cursorx - 55.0f) / 300.0f;

		x = x > 1.0f ? 1.0f : x < 0.0f ? 0.0f : x;
		g_Front.highlight = g_Front.sliderheld;
		g_Front.slider[g_Front.sliderheld] = g_Front.sliderheld == SLIDER_REACTION ? x : x * x * 10.0f;
		return;
	}

	if (joyGetButtonsPressedThisFrame(0, START_BUTTON) || (pick && g_Front.tabstart)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		frontStartMission();
		return;
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_DIFFICULTY;
		frontSetCursorForDifficulty(g_Front.difficulty);
		return;
	}

	if (pick && g_Front.tabnext) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SWIPE);
		g_Front.screen = SCREEN_BRIEFING;
		g_Front.briefpage = BRIEF_TITLE;
		frontLoadBriefing(g_Front.mission);
	}
}

/**
 * interface_menu0A_briefing(): the NEXT tab turns the page, the PREVIOUS tab
 * turns it back and leaves on the first, and START runs the mission from any
 * page. With the cursor on none of them the nearer of NEXT and START is taken,
 * as GoldenEye takes it.
 */
static void frontTickBriefing(s32 pick, s32 back)
{
	const s32 more = g_Front.briefpage < NUM_BRIEF_PAGES - 1;

	// with the cursor on no tab, NEXT is what a pick takes while there are pages
	// left and START once there are not
	if (!g_Front.tabprev && !g_Front.tabnext && !g_Front.tabstart) {
		if (more) {
			g_Front.tabnext = 1;
		} else {
			g_Front.tabstart = 1;
		}
	}

	if (joyGetButtonsPressedThisFrame(0, START_BUTTON) || (pick && g_Front.tabstart)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		frontStartMission();
		return;
	}

	if (pick && g_Front.tabnext && more) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SWIPE);
		g_Front.briefpage++;
		return;
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);

		if (g_Front.briefpage > BRIEF_TITLE) {
			g_Front.briefpage--;
			return;
		}

		frontFreeBriefing();
		g_Front.screen = g_Front.difficulty == DIFFICULTY_007 ? SCREEN_007OPTIONS : SCREEN_DIFFICULTY;

		if (g_Front.screen == SCREEN_DIFFICULTY) {
			frontSetCursorForDifficulty(g_Front.difficulty);
		}
	}
}

/**
 * interface_menu0C_missionfailed() and interface_menu0D_missioncomplete(): the
 * report turns to the statistics, and the statistics to a briefing - the next
 * mission's when this one was completed, at the same difficulty, and this one's
 * again when it was not. After Aztec and Egyptian, which nothing follows, it is
 * the mission select, and so it is from PREVIOUS on either page. With the
 * cursor on neither tab a pick is NEXT.
 */
static void frontTickReport(s32 pick, s32 back)
{
	if (!g_Front.tabprev) {
		g_Front.tabnext = 1;
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		frontFreeBriefing();
		g_Front.screen = SCREEN_MISSION;
		frontSetCursorForMission(g_Front.mission);
		return;
	}

	if (!pick) {
		return;
	}

	frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SWIPE);

	if (g_Front.screen == SCREEN_REPORT) {
		g_Front.screen = SCREEN_STATS;
		return;
	}

	if (g_FrontReport.completed) {
		// SP_LEVEL_AZTEC and on
		if (g_Front.mission >= NUM_MISSIONS - 2) {
			frontFreeBriefing();
			g_Front.screen = SCREEN_MISSION;
			frontSetCursorForMission(g_Front.mission);
			return;
		}

		g_Front.mission++;
	}

	g_Front.screen = SCREEN_BRIEFING;
	g_Front.briefpage = BRIEF_TITLE;

	if (!frontLoadBriefing(g_Front.mission)) {
		g_Front.screen = SCREEN_MISSION;
		frontSetCursorForMission(g_Front.mission);
	}
}

static void frontSetCursorForMode(s32 mode)
{
	// setCursorPOSforMode()
	g_Front.cursorx = 126.0f;
	g_Front.cursory = mode * 0x20 + 0xe2;
}

void gexFrontTick(void)
{
	s32 pick;
	s32 back;

	if (!g_Front.active) {
		return;
	}

	frontMoveCursor();

	// the press that opened the folder is not a press in it
	if (g_Front.inputdelay > 0) {
		g_Front.inputdelay--;
		return;
	}

	// the pad's A, Z and B, and the keyboard's own accept and cancel beside them
	pick = joyGetButtonsPressedThisFrame(0, A_BUTTON | Z_TRIG | BUTTON_UI_ACCEPT) != 0 || inputKeyJustPressed(VK_MOUSE_LEFT);
	back = joyGetButtonsPressedThisFrame(0, B_BUTTON | BUTTON_UI_CANCEL) != 0 || inputKeyJustPressed(VK_ESCAPE);

	g_Front.tabprev = frontOnPrevTab();
	g_Front.tabstart = (g_Front.screen == SCREEN_MPOPTIONS || g_Front.screen == SCREEN_007OPTIONS
			|| g_Front.screen == SCREEN_BRIEFING) && !g_Front.tabprev && frontOnStartTab();
	g_Front.tabnext = ((g_Front.screen == SCREEN_LEVEL && g_Front.numlevels > LEVELS_PER_PAGE)
			|| g_Front.screen == SCREEN_007OPTIONS
			|| g_Front.screen == SCREEN_MONITORS || g_Front.screen == SCREEN_MONITORVIEW
			|| g_Front.screen == SCREEN_REPORT || g_Front.screen == SCREEN_STATS
			|| (g_Front.screen == SCREEN_BRIEFING && g_Front.briefpage < NUM_BRIEF_PAGES - 1))
		&& !g_Front.tabprev && frontOnNextTab();
	g_Front.highlight = -1;

	switch (g_Front.screen) {
	case SCREEN_MISSION:
		frontTickMission(pick, back);
		return;
	case SCREEN_DIFFICULTY:
		frontTickDifficulty(pick, back);
		return;
	case SCREEN_007OPTIONS:
		frontTick007(pick, back, joyGetButtons(0, A_BUTTON | Z_TRIG | BUTTON_UI_ACCEPT) || inputKeyPressed(VK_MOUSE_LEFT));
		return;
	case SCREEN_BRIEFING:
		frontTickBriefing(pick, back);
		return;
	case SCREEN_REPORT:
	case SCREEN_STATS:
		frontTickReport(pick, back);
		return;
	case SCREEN_LEVEL:
		frontTickLevel(pick, back);
		return;
	case SCREEN_CINEMA:
		frontTickCinema(pick, back);
		return;
	case SCREEN_CINEMAPICK:
		frontTickCinemaPick(pick, back);
		return;
	case SCREEN_EXTRA:
		frontTickExtra(pick, back);
		return;
	case SCREEN_MONITORS:
		frontTickMonitors(pick, back);
		return;
	case SCREEN_MONITORVIEW:
		frontTickMonitorView(pick, back);
		return;
	case SCREEN_SCENARIO:
		frontTickScenario(pick, back);
		return;
	case SCREEN_HEALTH:
	case SCREEN_CONTROLSTYLE:
		frontTickPlayerPanels();
		return;
	case SCREEN_CHARACTERS:
		frontTickCharacters();
		return;
	}

	if (g_Front.screen == SCREEN_MODE) {
		// interface_menu06_modesel(): below 243 is SELECT MISSION, which opens
		// the mission folder when the remake has missions to put on it, and
		// below 275 is EXTRA, which is the remake's own third row
		if (!g_Front.tabprev) {
			g_Front.highlight = g_Front.cursory >= 275.0f ? 2 : g_Front.cursory >= 243.0f ? 1 : 0;
		}

		if (back || (pick && g_Front.tabprev)) {
			frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
			frontClose();
			return;
		}

		if (pick && g_Front.highlight == 1) {
			frontSfx(GESFX_DOOR_METAL_CLOSE, MENUSOUND_SELECT);
			frontEnterSetup();
			g_Front.screen = SCREEN_MPOPTIONS;
		} else if (pick && g_Front.highlight == 0 && frontMissionsAvailable()) {
			frontSfx(GESFX_DOOR_METAL_CLOSE, MENUSOUND_SELECT);
			g_Front.screen = SCREEN_MISSION;
			frontSetCursorForMission(g_Front.mission);
		} else if (pick && g_Front.highlight == 2 && frontMissionsAreOwn()) {
			frontSfx(GESFX_DOOR_METAL_CLOSE, MENUSOUND_SELECT);
			frontOpenExtra(EXTRA_CINEMA);
		}

		// and a row that is not there says nothing, as GoldenEye's does not
		return;
	}

	// interface_menu0E_mpoptions(): the row the cursor is at or below the top of
	if (!g_Front.tabprev && !g_Front.tabstart) {
		g_Front.highlight = ROW_PLAYERS;

		for (s32 row = NUM_ROWS - 1; row > 0; row--) {
			if (g_Front.cursory >= ROW_TOP + row * ROW_PITCH) {
				g_Front.highlight = row;
				break;
			}
		}
	}

	if (joyGetButtonsPressedThisFrame(0, START_BUTTON) || (pick && g_Front.tabstart)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_SELECT);
		frontStartMatch();
		return;
	}

	if (back || (pick && g_Front.tabprev)) {
		frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MODE;
		frontSetCursorForMode(1);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		if (frontRowOn(g_Front.highlight)) {
			frontSfx(GESFX_DOOR_METAL_CLOSE2, MENUSOUND_FOCUS);
			frontSelectRow(g_Front.highlight);
		}
	}
}

s32 gexFrontIsActive(void)
{
	return g_Front.active;
}

s32 gexFrontOpen(void)
{
	if (!g_Front.loaded && !frontLoadAll()) {
		return 0;
	}

	g_Front.cursor.texturenum = CURSOR_IMAGE;
	g_Front.cursor.width = 32;
	g_Front.cursor.height = 32;
	g_Front.cursor.level = 0;
	g_Front.cursor.format = G_IM_FMT_RGBA;
	g_Front.cursor.depth = G_IM_SIZ_32b;
	g_Front.cursor.s = G_TX_WRAP;
	g_Front.cursor.t = G_TX_WRAP;
	g_Front.cursor.unk0b = 0;

	// texSelect() turns a config's number into a pointer that lasts the stage
	g_Front.numtextures = 0;

	g_Front.active = 1;
	g_FrontInside = 1;
	g_Front.screen = SCREEN_MODE;
	g_Front.mouseseen = 0;

	if (frontMusic() >= 0) {
		musicStartTrackAsMenu(frontMusic());
	}

	g_Front.inputdelay = 2;
	g_Front.highlight = -1;
	g_Front.sliderheld = -1;
	g_Front.briefpage = BRIEF_TITLE;

	// initgamedata(): the 007 options start where GoldenEye starts them
	g_Front.slider[SLIDER_HEALTH] = 1.0f;
	g_Front.slider[SLIDER_DAMAGE] = 1.0f;
	g_Front.slider[SLIDER_ACCURACY] = 1.0f;
	g_Front.slider[SLIDER_REACTION] = 0.0f;

	if (g_Front.mission < 0 || g_Front.mission >= NUM_MISSIONS) {
		g_Front.mission = 0;
	}

	frontSetCursorForMode(1);

	return 1;
}

/**
 * Back from a match GE Plus started: straight to Multiplayer Options with the
 * setup the match was played with, as GoldenEye returns there. The caller has
 * put the Perfect Menu underneath, so leaving the folder lands where opening it
 * did.
 */
s32 gexFrontOpenAfterMatch(void)
{
	if (!gexFrontOpen()) {
		return 0;
	}

	frontEnterSetup();
	g_Front.screen = SCREEN_MPOPTIONS;
	g_Front.cursorx = 126.0f;
	g_Front.cursory = ROW_TOP + ROW_PLAYERS * ROW_PITCH + ROW_PITCH / 2;
	// the press that ended the match is not a press in the folder
	g_Front.inputdelay = 10;

	return 1;
}

s32 gexFrontIsInside(void)
{
	return g_FrontInside;
}

/**
 * Out of a level GE Plus started and back to the folder, **the way a match goes
 * back** (menutick.c's MENUROOT_MPENDSCREEN): straight to the Institute with the
 * title told to be skipped, and var80087260 set - which is what skips the
 * Institute's own arrival (aiIfCutsceneButtonPressed) and what menuTick() waits
 * on to put the Perfect Menu up with the folder over it.
 *
 * Not by way of the title, which is where a solo mission's endscreen goes on
 * its own. The title is not a backdrop: it runs on under a folder opened over
 * it, reads the same presses, and left alone for twenty seconds loads its
 * attract demo - a stage load that resets the model pool the folder's own model
 * is an instance in.
 */
void gexFrontGoBack(void)
{
	var80087260 = 3;
	titleSetNextStage(STAGE_CITRAINING);
	setNumPlayers(1);
	titleSetNextMode(TITLEMODE_SKIP);
	// as the title does on its way to the Institute
	lvSetDifficulty(DIFF_A);
	mainChangeToStage(STAGE_CITRAINING);
}

/**
 * A solo mission is ending, by any of its ways out: the map's own ending, a
 * death, or the watch's abort. GoldenEye puts nothing over the level then; it
 * leaves it for the folder, which opens on the mission's report. So for a
 * mission the folder started, this stands where Perfect Dark's endscreen does
 * (mainEndStage()): it keeps how the mission went, and goes back.
 *
 * What Perfect Dark's endscreen writes to the save is left unwritten, and has
 * to be: it files the time under the mission's number in Perfect Dark's own
 * table, where number 0 is dataDyne Central and not the Dam. The time is kept
 * in the remake's own file instead, under the conditions Perfect Dark lets a
 * time count by.
 */
s32 gexFrontMissionReport(void)
{
	struct player *player = g_Vars.currentplayer;
	s32 weapon1 = 0;
	s32 weapon2 = 0;
	const char *name;
	char *end;

	if (!g_FrontInside || !frontMissionsAreOwn() || !player
			|| g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0
			|| g_Vars.stagenum != frontMissionStage(g_Front.mission)) {
		return 0;
	}

	memset(&g_FrontReport, 0, sizeof(g_FrontReport));

	g_FrontReport.valid = 1;
	g_FrontReport.kia = player->isdead != 0;
	g_FrontReport.aborted = player->aborted != 0;
	g_FrontReport.completed = !g_FrontReport.kia && !g_FrontReport.aborted && objectiveIsAllComplete();
	g_FrontReport.time60 = playerGetMissionTime();
	g_FrontReport.kills = mpstatsGetPlayerKillCount();
	g_FrontReport.difficulty = g_Front.difficulty;

	for (s32 i = 0; i < BRIEF_OBJECTIVES; i++) {
		g_FrontReport.objstatus[i] = i < objectiveGetCount() ? objectiveCheck(i) : OBJECTIVE_INCOMPLETE;
	}

	for (s32 i = 0; i < 7; i++) {
		g_FrontReport.shots[i] = mpstatsGetPlayerShotCountByRegion(i);
	}

	// the name while its text is still loaded: a borrowed gun's is in its own bank
	invGetWeaponOfChoice(&weapon1, &weapon2);
	name = bgunGetName(weapon1);
	snprintf(g_FrontReport.weapon, sizeof(g_FrontReport.weapon), "%s", name ? name : "");

	if ((end = strchr(g_FrontReport.weapon, '\n'))) {
		*end = '\0';
	}

	g_FrontReport.weapondual = weapon1 > 0 && weapon1 == weapon2;

	for (s32 i = 0; i < NUM_SLIDERS; i++) {
		g_FrontReport.slider[i] = g_Front.slider[i];
	}

	if (g_FrontReport.completed && !g_CheatsActiveBank0 && !g_CheatsActiveBank1) {
		s32 secs = g_FrontReport.time60 / 60;
		u16 *best;

		frontLoadBestTimes();

		// zero is "not completed", and the save's ten bits are GoldenEye's
		secs = secs < 1 ? 1 : secs > BESTTIME_MAX ? BESTTIME_MAX : secs;
		best = &g_BestTimes[g_Front.mission][g_FrontReport.difficulty];

		if (*best == 0 || secs < *best) {
			*best = secs;
			frontSaveBestTimes();
		}

		// the run's ghost, which Perfect Dark's endscreen writes on the same terms
		modGhostSaveRun();
	}

	sysLogPrintf(LOG_NOTE, "gexfront: mission %d over at %d (%s), to the folder's report", g_Front.mission,
			g_FrontReport.time60, g_FrontReport.kia ? "killed" : g_FrontReport.aborted ? "aborted"
			: g_FrontReport.completed ? "completed" : "failed");

	g_FrontWantMain = 1;
	gexFrontGoBack();

	return 1;
}

/**
 * A solo mission's endscreen has closed for good - finished, failed or aborted,
 * they all end there. True when the mission was one the folder started, and
 * the ending is then taken: back to GE Plus's own main menu rather than Perfect
 * Dark's.
 */
s32 gexFrontMissionEnded(void)
{
	if (!g_FrontInside) {
		return 0;
	}

	g_FrontWantMain = 1;
	gexFrontGoBack();

	return 1;
}

s32 gexFrontWantsMain(void)
{
	return g_FrontWantMain;
}

/**
 * Back from a mission: GoldenEye's report on it, where there is one to give
 * (gexFrontMissionReport()). Otherwise - a mission that ended on Perfect Dark's
 * endscreen - GE Plus's main menu, the mode select, with SELECT MISSION under
 * the cursor since that is where the player came from.
 */
s32 gexFrontOpenAfterMission(void)
{
	g_FrontWantMain = 0;

	if (!gexFrontOpen()) {
		return 0;
	}

	frontSetCursorForMode(0);
	// the press that closed the endscreen is not a press in the folder
	g_Front.inputdelay = 10;

	// init_menu0C_missionfailed(): the report of the mission that has just
	// ended, with the cursor on NEXT - and the difficulty it was played at,
	// which opening the folder has put back to where GoldenEye starts it
	if (g_FrontReport.valid && frontLoadBriefing(g_Front.mission)) {
		g_Front.screen = SCREEN_REPORT;
		g_Front.difficulty = g_FrontReport.difficulty;
		g_Front.cursorx = 399.0f;
		g_Front.cursory = 144.0f;

		for (s32 i = 0; i < NUM_SLIDERS; i++) {
			g_Front.slider[i] = g_FrontReport.slider[i];
		}
	}

	g_FrontReport.valid = 0;

	return 1;
}

/**
 * Back from a cinema GE Plus played: straight to the page it was picked on,
 * its mission's opening and ending, with the one that was watched under the
 * cursor - the way a match goes back to Multiplayer Options.
 */
s32 gexFrontOpenAfterCinema(s32 mission)
{
	if (!gexFrontOpen()) {
		return 0;
	}

	if (mission >= 0 && mission < NUM_MISSIONS) {
		g_Front.mission = mission;
	}

	g_Front.screen = SCREEN_CINEMAPICK;
	frontSetCursorForCinemaPick(g_Front.cinemawhat);
	// the press that ended the cinema is not a press in the folder
	g_Front.inputdelay = 10;

	return 1;
}

/**
 * The menu files without the folder, for the intro (geintro.c): it runs before
 * the folder opens and draws GoldenEye's own text with the same fonts, and
 * loading them once here keeps one copy.
 */
/**
 * The release's meshes have been switched (F6): the folder's art follows them,
 * and the model may be loaded right now - the switch is live while the folder
 * is open. gefolder.c decides what to do with it.
 */
void gexFrontMeshesSwitched(void)
{
	geFolderSwitched(g_Front.model ? g_Front.modeldef : NULL);
}

s32 gexFrontLoadShared(void)
{
	return g_Front.loaded || frontLoadAll();
}

s32 gexFrontLoadText(void)
{
	return g_Front.loaded || frontLoadText();
}

s32 gexFrontModDir(void)
{
	return g_Front.moddir;
}

s32 gexFrontMissionFiles(s32 mission, const char **brief, const char **lang, s32 *nameid)
{
	const s32 row = frontMissionRow(mission);

	if (row < 0 || !g_Missions[row].brief) {
		return 0;
	}

	*brief = g_Missions[row].brief;
	*lang = g_Missions[row].lang;
	*nameid = g_Missions[row].name;

	return 1;
}

const char *gexFrontTitleString(s32 index)
{
	return frontString(index);
}

/* ---- drawing ------------------------------------------------------------ */

/**
 * GoldenEye lays out on a 440x330 frame, which is 4:3 with square pixels. This
 * one is 320x220, and G_ASPECT_CENTER_EXT holds it at *its own* aspect
 * (SCREEN_ASPECT, 320/220) in the middle of the window - so a column of it is
 * worth exactly what a row is, and GoldenEye's frame goes into it by its height
 * and centred, a twelfth narrower than the whole of it.
 *
 * Scaling x by viGetWidth()/440 instead is what put everything drawn here a
 * twelfth further from the middle than the folder it is drawn on: at the tabs,
 * the whole width of a tab, so PREVIOUS stood beside its tab rather than on it.
 * Near the middle it is a pixel or two and nothing looked wrong.
 *
 * The folder itself is drawn through videoGetAspect() with no aspect mode and
 * so is already where GoldenEye puts it; these are what had to come to it.
 */
/**
 * The frame in GoldenEye's own units and the box on the screen it is fitted
 * into: 440x330 over the whole window for the menus, and GoldenEye's in-game
 * 320x240 over the player's viewport for the watch (gewatch.c), which is the
 * frame its own screens are laid out on. Height fits, x is centred, and a
 * column is worth a row either way.
 */
static struct {
	f32 gew, geh;
	s32 left, top, width, height;
	s32 set;
} g_FrontFrame;

void gexFrontTextFrame(f32 gew, f32 geh, s32 left, s32 top, s32 width, s32 height)
{
	g_FrontFrame.gew = gew;
	g_FrontFrame.geh = geh;
	g_FrontFrame.left = left;
	g_FrontFrame.top = top;
	g_FrontFrame.width = width;
	g_FrontFrame.height = height;
	g_FrontFrame.set = 1;
}

void gexFrontTextFrameDefault(void)
{
	g_FrontFrame.set = 0;
}

static f32 frontScaleY(void)
{
	if (g_FrontFrame.set) {
		return g_FrontFrame.height / g_FrontFrame.geh;
	}

	return viGetHeight() / GEFRONT_H;
}

// the same as a row's, which is the whole of the point
static f32 frontScaleX(void)
{
	// a frame of somebody else's (the watch's, over the player's view) is
	// drawn with no aspect mode, so a column of it is the window's width over
	// the frame buffer's and a row the height over its height: not square,
	// and a third too wide on a 16:9 window, where the watch's text ran off
	// the face and onto the bezel
	if (g_FrontFrame.set) {
		return frontScaleY() * (f32)viGetWidth() / ((f32)viGetHeight() * videoGetAspect());
	}

	return frontScaleY();
}

// a GoldenEye x in this frame's own units
static f32 frontX(f32 x)
{
	if (g_FrontFrame.set) {
		return g_FrontFrame.left + g_FrontFrame.width * 0.5f + (x - g_FrontFrame.gew * 0.5f) * frontScaleX();
	}

	return viGetWidth() * 0.5f + (x - GEFRONT_W * 0.5f) * frontScaleX();
}

// and a GoldenEye y
static f32 frontY(f32 y)
{
	if (g_FrontFrame.set) {
		return g_FrontFrame.top + y * frontScaleY();
	}

	return y * frontScaleY();
}

static Gfx *frontFillRect(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, u32 colour)
{
	// microcode_constructor_related_to_menus()
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetPrimColor(gdl++, 0, 0, colour >> 24, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);
	gDPFillRectangle(gdl++, (s32)frontX(x1), (s32)frontY(y1), (s32)frontX(x2), (s32)frontY(y2));

	return gdl;
}

static Gfx *frontTextSetup(Gfx *gdl)
{
	// microcode_constructor()
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetColorDither(gdl++, G_CD_DISABLE);
	gDPSetRenderMode(gdl++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	gDPSetCombineLERP(gdl++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureConvert(gdl++, G_TC_FILT);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);

	return gdl;
}

static s32 frontLineHeight(const struct gefont *font)
{
	// chars['['], GoldenEye's own indexing: the '|' glyph
	return font->chars['['].height + font->chars['['].baseline;
}

/** textMeasure(): the width of the widest line, and the height. */
static void frontMeasure(const struct gefont *font, const char *text, s32 spacing, s32 *width, s32 *height)
{
	s32 prev = 'H';
	s32 w = 0;
	s32 longest = 0;

	*height = 0;

	for (; *text; text++) {
		const u8 c = *text;

		if (c == ' ') {
			if (text[1] != '\n') {
				w += 5;
			}
			prev = 'H';
		} else if (c == '\n') {
			longest = w > longest ? w : longest;
			w = 0;
			*height += frontLineHeight(font);
		} else if (c >= 0x21 && c < 0x7f) {
			const struct gefontchar *cur = &font->chars[c - 0x21];
			const struct gefontchar *p = &font->chars[prev - 0x21];

			w = cur->width + w - (font->kerning[p->kerningindex * 13 + cur->kerningindex] + spacing - 1);
			prev = c;
		}
	}

	*width = w > longest ? w : longest;
}

/**
 * textRender(): GoldenEye's glyphs, each an 8-bit intensity block used as the
 * alpha of the colour. rotated is ROT_90CW, the tabs' text: *x then runs down
 * the screen and *y is the column the letters stand on.
 */
static Gfx *frontText(Gfx *gdl, const struct gefont *font, s32 *x, s32 *y, const char *text, u32 colour, s32 spacing, s32 rotated)
{
	const f32 sx = frontScaleX();
	const f32 sy = frontScaleY();
	const s32 savedx = *x;
	s32 prev = 'H';

	gDPSetPrimColor(gdl++, 0, 0, colour >> 24, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);

	for (; *text; text++) {
		const u8 c = *text;
		const struct gefontchar *cur;
		const struct gefontchar *p;

		if (c == ' ') {
			*x += 5;
			prev = 'H';
			continue;
		}

		if (c == '\n') {
			*y += frontLineHeight(font);
			*x = savedx;
			prev = 'H';
			continue;
		}

		if (c < 0x21 || c >= 0x7f) {
			continue;
		}

		cur = &font->chars[c - 0x21];
		p = &font->chars[prev - 0x21];
		*x -= font->kerning[p->kerningindex * 13 + cur->kerningindex] + spacing - 1;

		gDPLoadTextureBlock(gdl++, cur->pixels, G_IM_FMT_I, G_IM_SIZ_8b, (cur->width + 7) & ~7, cur->height, 0,
				G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

		if (rotated) {
			// the glyph's rows run right to left across the screen, its columns down it
			gSPTextureRectangleFlip(gdl++,
					(s32)(frontX((*y - cur->baseline) - cur->height) * 4),
					(s32)(frontY(*x) * 4),
					(s32)(frontX(*y - cur->baseline) * 4),
					(s32)(frontY(*x + cur->width) * 4),
					G_TX_RENDERTILE, 0, (cur->height - 1) << 5,
					(s32)(1024 / sy), (s32)(-1024 / sx));
		} else {
			gSPTextureRectangle(gdl++,
					(s32)(frontX(*x) * 4),
					(s32)(frontY(*y + cur->baseline) * 4),
					(s32)(frontX(*x + cur->width) * 4),
					(s32)(frontY(*y + cur->baseline + cur->height) * 4),
					G_TX_RENDERTILE, 0, 0,
					(s32)(1024 / sx), (s32)(1024 / sy));
		}

		*x += cur->width;
		prev = c;
	}

	return gdl;
}

static Gfx *frontPrint(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
	return frontText(gdl, &g_Front.zurich, &x, &y, text, colour, 0, false);
}

static void frontWrap(const struct gefont *font, const char *text, char *out, size_t len, s32 width);

/* GoldenEye's text for the intro (geintro.c), which has no folder of its own */

Gfx *gexFrontTextSetup(Gfx *gdl)
{
	return frontTextSetup(gdl);
}

Gfx *gexFrontTextPrint(Gfx *gdl, s32 gothic, s32 x, s32 y, const char *text, u32 colour)
{
	return frontText(gdl, gothic ? &g_Front.gothic : &g_Front.zurich, &x, &y, text, colour, 0, false);
}

void gexFrontTextMeasure(s32 gothic, const char *text, s32 *width, s32 *height)
{
	frontMeasure(gothic ? &g_Front.gothic : &g_Front.zurich, text, 0, width, height);
}

Gfx *gexFrontFillRect(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, u32 colour)
{
	return frontFillRect(gdl, x1, y1, x2, y2, colour);
}

void gexFrontTextWrap(s32 gothic, const char *text, char *out, size_t len, s32 width)
{
	frontWrap(gothic ? &g_Front.gothic : &g_Front.zurich, text, out, len, width);
}

/** frontAddStartTabText() and frontAddPreviousTabText(): Bank Gothic, turned, a tab's middle. */
static Gfx *frontTab(Gfx *gdl, s32 textstr, s32 top, s32 bottom, s32 highlight)
{
	const char *text = frontString(textstr);
	s32 width;
	s32 height;
	s32 v;
	s32 h;

	// setTextSpacingInverted(TRUE): one closer
	frontMeasure(&g_Front.gothic, text, -1, &width, &height);
	h = TABS_RIGHT_EDGE - height / 2;

	if (highlight) {
		gdl = frontFillRect(gdl, h - height + 1, top, h, bottom, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	v = top + (bottom - top) / 2 - width / 2;

	return frontText(gdl, &g_Front.gothic, &v, &h, text, COLOUR_ON, -1, true);
}

static s32 frontReleasePicture(const char *name, struct textureconfig *tex);

static Gfx *frontDrawCursor(Gfx *gdl)
{
	const f32 sx = frontScaleX();
	const f32 sy = frontScaleY();
	const f32 x = (s32)(g_Front.cursorx + 0.5f);
	const f32 y = (s32)(g_Front.cursory + 0.5f);
	const s32 prevsrc = modSetTextureSourceMod(g_Front.moddir);
	struct textureconfig release;

	// the release's own crosshair (texture/sight), the same 32 texels square
	if (frontReleasePicture("sight", &release)) {
		texSelect(&gdl, &release, 4, 0, 2, 1, NULL);
		gDPSetTextureFilter(gdl++, G_TF_BILERP);
	} else {
		texSelect(&gdl, &g_Front.cursor, 4, 0, 2, 1, NULL);
	}

	modSetTextureSourceMod(prevsrc);

	// display_image_at_position(): white, 220 of 255, the image's middle on the cursor
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetEnvColor(gdl++, 255, 255, 255, 220);
	gDPSetCombineLERP(gdl++, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0);
	gSPTextureRectangle(gdl++,
			(s32)(frontX(x - 16) * 4), (s32)(frontY(y - 16) * 4),
			(s32)(frontX(x + 16) * 4), (s32)(frontY(y + 16) * 4),
			G_TX_RENDERTILE, 0, 0, (s32)(1024 / sx), (s32)(1024 / sy));

	return gdl;
}

/**
 * A texture of the conversion's by number, as a config texSelect() loads the
 * first time this visit draws it and keeps the loaded pointer in.
 */
static struct textureconfig *frontTexture(s32 num, s32 width, s32 height, s32 format, s32 depth, s32 wrap)
{
	struct textureconfig *tex;

	for (s32 i = 0; i < g_Front.numtextures; i++) {
		if (g_Front.textures[i].num == num) {
			return &g_Front.textures[i].config;
		}
	}

	if (g_Front.numtextures >= MAX_FRONT_TEXTURES) {
		return NULL;
	}

	g_Front.textures[g_Front.numtextures].num = num;
	tex = &g_Front.textures[g_Front.numtextures++].config;
	memset(tex, 0, sizeof(*tex));
	tex->texturenum = num;
	tex->width = width;
	tex->height = height;
	tex->format = format;
	tex->depth = depth;
	tex->s = wrap ? G_TX_WRAP : G_TX_CLAMP;
	tex->t = wrap ? G_TX_WRAP : G_TX_CLAMP;

	return tex;
}

/**
 * The release's own picture of one of the front end's images, where the
 * release is there and its look is on (gefolder.c): a config naming the
 * picture's stand-in. The renderer draws the whole picture over the config's
 * nominal FRONT_PICTURE_TEXELS square whatever its real size, so that is what
 * a rectangle's texel steps are counted in.
 */
#define FRONT_PICTURE_TEXELS 32

static s32 frontReleasePicture(const char *name, struct textureconfig *tex)
{
	s32 w = 0, h = 0;
	const void *tile = geFolderMenuPicture(name, &w, &h);

	if (!tile) {
		return 0;
	}

	memset(tex, 0, sizeof(*tex));
	tex->textureptr = (u8 *)tile;
	tex->width = FRONT_PICTURE_TEXELS;
	tex->height = FRONT_PICTURE_TEXELS;
	tex->format = G_IM_FMT_RGBA;
	tex->depth = G_IM_SIZ_32b;
	tex->s = G_TX_CLAMP;
	tex->t = G_TX_CLAMP;

	return 1;
}

/**
 * A selected texture over a rectangle (its middle and half size), twidth and
 * theight texels across it, tinted by the colour; opaque, as GoldenEye draws
 * its stage pictures, or translucent as it draws a portrait. A negative
 * theight runs the rows bottom to top. The release's pictures are smoothed,
 * GoldenEye's own point sampled.
 */
static Gfx *frontImageRect(Gfx *gdl, f32 cx, f32 cy, f32 hw, f32 hh, s32 twidth, s32 theight,
		u32 colour, s32 translucent, s32 smooth)
{
	const f32 sx = frontScaleX();
	const f32 sy = frontScaleY();

	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetEnvColor(gdl++, colour >> 24, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);

	if (translucent) {
		// a portrait: shaded by the colour, as see-through as its alpha and no more
		gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
		gDPSetTextureFilter(gdl++, G_TF_BILERP);
		gDPSetCombineLERP(gdl++, TEXEL0, 0, ENVIRONMENT, 0, 0, 0, 0, ENVIRONMENT, TEXEL0, 0, ENVIRONMENT, 0, 0, 0, 0, ENVIRONMENT);
	} else {
		gDPSetTextureFilter(gdl++, smooth ? G_TF_BILERP : G_TF_POINT);
		gDPSetCombineLERP(gdl++, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0);
	}

	gSPTextureRectangle(gdl++,
			(s32)(frontX(cx - hw) * 4), (s32)(frontY(cy - hh) * 4),
			(s32)(frontX(cx + hw) * 4), (s32)(frontY(cy + hh) * 4),
			G_TX_RENDERTILE, 0, theight < 0 ? ((-theight) << 5) - 1 : 0,
			(s32)(twidth / (2.0f * hw) * 1024.0f / sx), (s32)(theight / (2.0f * hh) * 1024.0f / sy));

	return gdl;
}

/**
 * display_image_at_position() and draw_textured_rectangle(): one of the
 * conversion's textures by number over a rectangle - see frontImageRect().
 */
static Gfx *frontImage(Gfx *gdl, s32 num, s32 width, s32 height, s32 format, s32 wrap,
		f32 cx, f32 cy, f32 hw, f32 hh, s32 twidth, s32 theight, u32 colour, s32 translucent)
{
	struct textureconfig *tex = frontTexture(num, width, height, format, G_IM_SIZ_8b, wrap);
	s32 prevsrc;

	if (!tex) {
		return gdl;
	}

	prevsrc = modSetTextureSourceMod(g_Front.moddir);
	texSelect(&gdl, tex, 1, 0, 2, 1, NULL);
	modSetTextureSourceMod(prevsrc);

	return frontImageRect(gdl, cx, cy, hw, hh, twidth, theight, colour, translucent, false);
}

/**
 * The release's picture by name over the same rectangle, the whole of it,
 * when the release is there and its look is on; else GoldenEye's own by
 * number, as frontImage() draws it.
 */
static Gfx *frontImageOrRelease(Gfx *gdl, const char *name, s32 num, s32 width, s32 height, s32 format, s32 wrap,
		f32 cx, f32 cy, f32 hw, f32 hh, s32 twidth, s32 theight, u32 colour, s32 translucent)
{
	struct textureconfig tex;

	if (name && frontReleasePicture(name, &tex)) {
		// the same share of the picture as of GoldenEye's, which a strip of
		// holes repeats across itself
		if (wrap) {
			tex.s = G_TX_WRAP;
			tex.t = G_TX_WRAP;
		}

		texSelect(&gdl, &tex, 1, 0, 2, 1, NULL);

		return frontImageRect(gdl, cx, cy, hw, hh, twidth * FRONT_PICTURE_TEXELS / width,
				theight * FRONT_PICTURE_TEXELS / height, colour, translucent, true);
	}

	return frontImage(gdl, num, width, height, format, wrap, cx, cy, hw, hh, twidth, theight, colour, translucent);
}

static void frontSetSwitch(s32 part, s32 visible)
{
	struct modelnode *node = modelGetPart(g_Front.modeldef, part);

	if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
		union modelrwdata *rwdata = modelGetNodeRwData(g_Front.model, node);

		if (rwdata) {
			rwdata->toggle.visible = visible;
		}
	}
}

/**
 * interface_menu07_missionsel(): the slides light by their own vertices - white
 * for the mission under the cursor, grey for one that can be played and
 * near-black for one that cannot.
 *
 * The grid is the part bondconstants.h does not name (21), a display list node
 * rather than a switch, four vertices a mission in mission order. The converter
 * gives every vertex a colour of its own in the same order (gemodelconv.py:
 * GoldenEye keeps a vertex's colour in the vertex, Perfect Dark in a table the
 * list loads with G_COL). The folder is loaded for the folder alone and freed
 * when it closes, so nothing else is drawing from those colours.
 *
 * The colours are **rwdata's**: `rodata->dl.colours` is what the load left
 * there, the model file's own base, which is the segment a list's G_COL
 * offsets are counted from (SPSEGMENT_MODEL_COL1) and not an array at all.
 * The array is where the vertices end, which is what modelAllocateRwData()
 * puts in rwdata. Writing 80 colours over the rodata pointer writes over the
 * head of the file - the node table - and the next frame dies in
 * modelUpdateRelations().
 */
static void frontColourSlides(void)
{
	struct modelnode *node = modelGetPart(g_Front.modeldef, SW_SLIDEGRID);
	union modelrwdata *rwdata;
	Col *colours;
	s32 count;

	if (!node || (node->type & 0xff) != MODELNODETYPE_DL) {
		return;
	}

	rwdata = modelGetNodeRwData(g_Front.model, node);
	count = node->rodata->dl.numcolours;

	if (!rwdata || !(colours = rwdata->dl.colours) || count > 4 * NUM_MISSIONS) {
		return;
	}

	for (s32 i = 0; i < count; i++) {
		const s32 mission = i / 4;

		if (frontHighestDifficulty(mission) < 0) {
			colours[i].r = colours[i].g = colours[i].b = 0x0f;
			colours[i].a = 0xff;
		} else if (mission == g_Front.highlight) {
			colours[i].r = colours[i].g = colours[i].b = 0xff;
			colours[i].a = 0xf5;
		} else {
			colours[i].r = colours[i].g = colours[i].b = 0x6e;
			colours[i].a = 0xff;
		}
	}
}

/**
 * The frame behind the folder covers GoldenEye's own 4:3 screen and no more, so
 * on a wider window its outer edge comes into view with black beyond it. The
 * folder is drawn through videoGetAspect() and so keeps its shape and its place
 * in the middle of the window whatever that window is; only the frame has to
 * reach further, and it reaches by its outer rectangle alone - the inner one is
 * where the folder sits, and moving it would open a gap between the two.
 *
 * How far it has to reach is asked of the camera rather than of the window's
 * aspect alone, because GoldenEye's own right hand edge is already a few units
 * inside its 4:3 screen (the N64's overscan covered it): the half width the
 * view spans at the frame's own depth, with a little over. An edge is only ever
 * moved outwards, so at 4:3 what is drawn is still the ROM's.
 *
 * The side bars grow by more than the window does (a third more window is two
 * and a half times the bar), so their texture is carried out at the density it
 * already had rather than stretched over the new width: s is extrapolated along
 * the same line that takes it from the inner edge to the outer one.
 */
static void frontWidenBackdrop(void)
{
	f32 halfwidth;
	s16 outer[2];
	s16 inner[2];
	s32 souter[2];
	s32 sinner[2];

	if (!g_Front.backdrop) {
		return;
	}

	// The release's look has a desk of its own behind the folder
	// (frontDrawReleaseBackdrop()), so the frame is folded to nothing - every
	// vertex on one line - and unfolded from the ROM's own numbers when the
	// look changes back.
	for (s32 i = 0; i < GEFRONT_BACKDROP_VTX; i++) {
		g_Front.backdrop->vertices[i].x = geFolderBackdrop() ? 0 : g_Front.backdropvtx[i].x;
		g_Front.backdrop->vertices[i].s = g_Front.backdropvtx[i].s;
	}

	if (geFolderBackdrop()) {
		return;
	}

	// the half width the view spans where the frame stands - it is flat, so any
	// of its vertices gives the depth - in the model's own units, and a percent
	// over so nothing sits exactly on the edge
	halfwidth = (FOLDER_EYEZ - g_Front.backdropvtx[0].z * FOLDER_SCALE)
		* FOLDER_TANHALFFOVY * videoGetAspect() / FOLDER_SCALE * 1.01f;

	// the outer rectangle's x either side, and the inner one's
	outer[0] = outer[1] = g_Front.backdropvtx[0].x;

	for (s32 i = 1; i < GEFRONT_BACKDROP_VTX; i++) {
		const s16 x = g_Front.backdropvtx[i].x;

		if (x < outer[0]) outer[0] = x;
		if (x > outer[1]) outer[1] = x;
	}

	inner[0] = outer[1];
	inner[1] = outer[0];

	for (s32 i = 0; i < GEFRONT_BACKDROP_VTX; i++) {
		const s16 x = g_Front.backdropvtx[i].x;

		if (x != outer[0] && x < inner[0]) inner[0] = x;
		if (x != outer[1] && x > inner[1]) inner[1] = x;
	}

	// not the frame this was written for: leave it alone rather than divide by
	// the width of an edge that is not there
	if (inner[0] <= outer[0] || inner[1] >= outer[1]) {
		return;
	}

	// and the s each of those four edges carries, so the texture can be
	// continued rather than stretched
	souter[0] = souter[1] = sinner[0] = sinner[1] = 0;

	for (s32 i = 0; i < GEFRONT_BACKDROP_VTX; i++) {
		const s16 x = g_Front.backdropvtx[i].x;
		const s16 v = g_Front.backdropvtx[i].s;

		if (x == outer[0]) souter[0] = v;
		if (x == outer[1]) souter[1] = v;
		if (x == inner[0]) sinner[0] = v;
		if (x == inner[1]) sinner[1] = v;
	}

	for (s32 i = 0; i < GEFRONT_BACKDROP_VTX; i++) {
		const s16 x = g_Front.backdropvtx[i].x;
		const s32 side = x == outer[0] ? 0 : (x == outer[1] ? 1 : -1);
		f32 wide;

		if (side < 0) {
			continue;
		}

		wide = side ? halfwidth : -halfwidth;

		if (side ? wide < x : wide > x) {
			wide = x;
		}

		g_Front.backdrop->vertices[i].x = (s16)wide;
		g_Front.backdrop->vertices[i].s = (s16)(sinner[side]
				+ (souter[side] - sinner[side]) * (wide - inner[side]) / (f32)(x - inner[side]));
	}
}

/**
 * The release's desk behind the folder (gefolder.c) over the whole frame,
 * whatever its shape, where its look is on.
 */
static Gfx *frontDrawReleaseBackdrop(Gfx *gdl)
{
	const void *tile = geFolderBackdrop();

	if (!tile) {
		return gdl;
	}

	// Loaded by hand rather than through texSelect(): that keeps a note of
	// the tiles it has set up, and the folder's lists, drawn next, trust the
	// note - a picture selected through it here drew the folder's photograph
	// and stamps at the wrong size.
	gDPPipeSync(gdl++);
	gDPLoadTextureBlock(gdl++, tile, G_IM_FMT_RGBA, G_IM_SIZ_32b, FRONT_PICTURE_TEXELS, FRONT_PICTURE_TEXELS, 0,
			G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetRenderMode(gdl++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
	gDPSetCombineMode(gdl++, G_CC_DECALRGB, G_CC_DECALRGB);
	gSPTextureRectangle(gdl++, 0, 0, viGetWidth() * 4, viGetHeight() * 4, G_TX_RENDERTILE, 0, 0,
			(s32)(FRONT_PICTURE_TEXELS * 1024.0f / viGetWidth()), (s32)(FRONT_PICTURE_TEXELS * 1024.0f / viGetHeight()));
	gDPPipeSync(gdl++);

	// and put back what the folder's lists take as given: they set neither
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);

	return gdl;
}

/**
 * frontSetupMenuBackground(): the folder at a quarter size, 4000 in front of a
 * camera 190 above its middle and 3300 nearer, with GoldenEye's 60 degree view
 * and no depth buffer - the folder draws in its own order.
 */
static Gfx *frontDrawFolder(Gfx *gdl)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	static Vp vp;
	Mtxf camera;
	Mtxf world;
	Mtxf persp;
	Mtx *projection = gfxAllocateMatrix();
	u16 perspnorm;
	Mtxf tmp;

	frontWidenBackdrop();

	for (s32 i = 0; i < g_Front.modeldef->numparts; i++) {
		frontSetSwitch(i, false);
	}

	// each screen's interface_menu*() switches
	frontSetSwitch(SW_TABS, true);

	switch (g_Front.screen) {
	case SCREEN_MODE:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_PHOTOBOND, true);
		frontSetSwitch(SW_EYESONLY, true);
		frontSetSwitch(SW_BROSNAN, true);
		frontSetSwitch(SW_BROSNANCOVER, true);
		break;
	case SCREEN_MPOPTIONS:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_CONFIDENTIAL2, true);
		break;
	case SCREEN_SCENARIO:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_CLASSIFIED, true);
		break;
	case SCREEN_LEVEL:
		frontSetSwitch(SW_BLANK, true);
		frontSetSwitch(SW_OHMSS, true);
		break;
	case SCREEN_MONITORS:
		// the Level page's film strip, with sets where its pictures are
		frontSetSwitch(SW_BLANK, true);
		frontSetSwitch(SW_OHMSS, true);
		break;
	case SCREEN_EXTRA:
	case SCREEN_MONITORVIEW:
	case SCREEN_CINEMAPICK:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_CONFIDENTIAL, true);
		break;
	case SCREEN_CINEMA:
	case SCREEN_MISSION:
		// the slides the missions are named on, and their grid
		frontSetSwitch(SW_SLIDES, true);
		frontSetSwitch(SW_PICS, true);
		frontColourSlides();
		break;
	case SCREEN_DIFFICULTY:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_CONFIDENTIAL, true);
		break;
	case SCREEN_007OPTIONS:
	case SCREEN_REPORT:
	case SCREEN_STATS:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_CLASSIFIED, true);
		break;
	case SCREEN_BRIEFING:
		frontSetSwitch(SW_PAPER, true);
		frontSetSwitch(SW_OHMSS, true);
		frontSetSwitch(SW_CLASSIFIED, true);
		// the mission's own photo, clipped to the folder, on the first page
		frontSetSwitch(SW_PHOTOBRIEF, g_Front.briefpage == BRIEF_TITLE);
		frontSetSwitch(SW_BRIEFFIRST + g_Front.mission, g_Front.briefpage == BRIEF_TITLE);
		break;
	default:
		frontSetSwitch(SW_BLANK, true);
		break;
	}

	vp.vp.vscale[0] = viGetWidth() * 2;
	vp.vp.vscale[1] = viGetHeight() * 2;
	vp.vp.vscale[2] = 511;
	vp.vp.vscale[3] = 0;
	vp.vp.vtrans[0] = viGetWidth() * 2;
	vp.vp.vtrans[1] = viGetHeight() * 2;
	vp.vp.vtrans[2] = 511;
	vp.vp.vtrans[3] = 0;

	guPerspectiveF(persp.m, &perspnorm, FOLDER_FOVY, videoGetAspect(), 100.0f, 10000.0f, 1.0f);
	guMtxF2L(persp.m, projection);

	gSPViewport(gdl++, &vp);
	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);
	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	// folderpositions[0] and D_8002AFC4..CC
	mtx00016ae4(&camera, -900.0f, 990.0f, FOLDER_EYEZ, -900.0f, 990.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	mtx4LoadIdentity(&world);
	world.m[3][0] = -900.0f;
	world.m[3][1] = 800.0f;
	mtx00015f04(FOLDER_SCALE, &world);
	mtx4MultMtx4InPlace(&camera, &world);

	renderdata.unk00 = &world;
	renderdata.unk10 = gfxAllocate(g_Front.modeldef->nummatrices * sizeof(Mtxf));
	mtx4Copy(&world, renderdata.unk10);
	g_Front.model->matrices = renderdata.unk10;

	modelUpdateRelations(g_Front.model);

	renderdata.flags = 3;
	renderdata.zbufferenabled = false;
	renderdata.gdl = gdl;

	// the release's own folder, node for node, under its look (gefolder.c)
	geFolderBeanSwap(g_Front.model, true);
	modelRender(&renderdata, g_Front.model);
	geFolderBeanSwap(g_Front.model, false);

	gdl = renderdata.gdl;

	for (s32 i = 0; i < g_Front.modeldef->nummatrices; i++) {
		mtx4Copy((Mtxf *)((uintptr_t)g_Front.model->matrices + i * sizeof(Mtxf)), &tmp);
		mtxF2L(&tmp, g_Front.model->matrices + i);
	}

	return gdl;
}

static Gfx *frontDrawMode(Gfx *gdl)
{
	// constructor_menu06_modesel()
	const char *text;
	s32 w;
	s32 h;

	const u32 missions = frontMissionsAvailable() ? COLOUR_ON : COLOUR_OFF;

	text = frontString(TITLE_SELECTMISSION);
	frontMeasure(&g_Front.zurich, text, 0, &w, &h);
	gdl = frontPrint(gdl, 0x96, 0xdc, "1.\n", missions);

	if (g_Front.highlight == 0) {
		gdl = frontFillRect(gdl, 0x94, 0xda, w + 0xaf, 0xea, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	gdl = frontPrint(gdl, 0xaa, 0xdc, text, missions);

	text = frontString(TITLE_MULTIPLAYER);
	frontMeasure(&g_Front.zurich, text, 0, &w, &h);
	gdl = frontPrint(gdl, 0x96, 0xfc, "2.\n", COLOUR_ON);

	if (g_Front.highlight == 1) {
		gdl = frontFillRect(gdl, 0x94, 0xfa, w + 0xaf, 0x10a, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	gdl = frontPrint(gdl, 0xaa, 0xfc, text, COLOUR_ON);

	// CINEMA is the remake's own row, on GoldenEye's own pitch below the two,
	// and it needs the remake's own missions: the shots are theirs.
	{
		const u32 cinema = frontMissionsAreOwn() ? COLOUR_ON : COLOUR_OFF;

		text = "EXTRA\n";
		frontMeasure(&g_Front.zurich, text, 0, &w, &h);
		gdl = frontPrint(gdl, 0x96, 0x11c, "3.\n", cinema);

		if (g_Front.highlight == 2) {
			gdl = frontFillRect(gdl, 0x94, 0x11a, w + 0xaf, 0x12a, COLOUR_HIGHLIGHT);
			gdl = frontTextSetup(gdl);
		}

		gdl = frontPrint(gdl, 0xaa, 0x11c, text, cinema);
	}

	return gdl;
}

static const char *frontRowLabel(s32 row)
{
	switch (row) {
	case ROW_PLAYERS: return frontString(TITLE_PLAYERS);
	case ROW_SIMULANTS: return "Simulants:\n";
	case ROW_SIMDIFF: return "Simulant Skill:\n";
	case ROW_SCENARIO: return frontString(TITLE_SCENARIO);
	case ROW_LEVEL: return frontString(TITLE_LEVEL);
	case ROW_GAMELENGTH: return frontString(TITLE_GAMELENGTH);
	case ROW_WEAPONS: return frontString(TITLE_WEAPONS);
	case ROW_CHARACTERS: return frontString(TITLE_CHARACTERS);
	case ROW_HEALTH: return frontString(TITLE_HEALTH);
	case ROW_CONTROLSTYLE: return frontString(TITLE_CONTROLSTYLE);
	case ROW_AIM: return frontString(TITLE_AIM);
	}

	return "";
}

static const char *frontRowValue(s32 row, char *buf, size_t len)
{
	switch (row) {
	case ROW_PLAYERS:
		snprintf(buf, len, "%d", frontNumPlayers());
		return buf;
	case ROW_SIMULANTS:
		snprintf(buf, len, "%d", g_Vars.mpquickteamnumsims);
		return buf;
	case ROW_SIMDIFF:
		snprintf(buf, len, "%s", langGet(L_MISC_082 + g_Vars.mpsimdifficulty));
		return buf;
	case ROW_SCENARIO:
		return frontString(frontScenarioString(gexPlusGetScenario()));
	case ROW_LEVEL:
		return g_MpSetup.stagenum == STAGE_MP_RANDOM ? frontString(154) : frontStageName(g_MpSetup.stagenum);
	case ROW_GAMELENGTH:
		return frontString(TITLE_LEN_UNLIMITED + g_Front.gamelength);
	case ROW_WEAPONS:
		return langGet(g_MpWeaponSets[g_MpWeaponSetNum].name);
	case ROW_AIM:
		return frontString(TITLE_AIM_FIRST + g_Front.aim);
	}

	return NULL;
}

static Gfx *frontDrawMpOptions(Gfx *gdl)
{
	// constructor_menu0E_mpoptions()
	gdl = frontPrint(gdl, 0x37, 0x5f, frontString(TITLE_MPOPTIONS), COLOUR_ON);

	for (s32 row = 0; row < NUM_ROWS; row++) {
		const s32 y = ROW_TOP + row * ROW_PITCH;
		const u32 colour = frontRowOn(row) ? COLOUR_ON : COLOUR_OFF;
		const char *label = frontRowLabel(row);
		const char *value;
		char buf[64];
		char line[80];
		s32 w;
		s32 h;

		frontMeasure(&g_Front.zurich, label, 0, &w, &h);

		if (g_Front.highlight == row) {
			gdl = frontFillRect(gdl, 0x37, y - 1, w + 0x3c, y + 14, COLOUR_HIGHLIGHT);
			gdl = frontTextSetup(gdl);
		}

		gdl = frontPrint(gdl, 0x39, y, label, colour);

		value = frontRowValue(row, buf, sizeof(buf));

		if (value) {
			// one line, as GoldenEye's own values are
			snprintf(line, sizeof(line), "%s", value);
			line[strcspn(line, "\n")] = '\0';
			gdl = frontPrint(gdl, 0xa0, y, line, colour);
		}
	}

	return gdl;
}

/** constructor_menu12_mpstage(): three strips of film, four pictures on each, captions over them. */
static Gfx *frontDrawLevel(Gfx *gdl)
{
	const s32 first = g_Front.levelpage * LEVELS_PER_PAGE;

	for (s32 i = 0; i < 3; i++) {
		gdl = frontFillRect(gdl, 0x25, 0x6c + i * 0x46, 0x185, 0xa0 + i * 0x46, 0x101010ff);
	}

	// the strips' holes, above and below each
	for (s32 i = 0; i < 3; i++) {
		gdl = frontImageOrRelease(gdl, DOT_RELEASE, DOT_IMAGE, 16, 16, G_IM_FMT_I, true, 213, 104 + 70 * i, 176, 4, 0x2f0, 0x12, 0x6b6753ff, false);
		gdl = frontImageOrRelease(gdl, DOT_RELEASE, DOT_IMAGE, 16, 16, G_IM_FMT_I, true, 213, 164 + 70 * i, 176, 4, 0x2f0, 0x12, 0x6b6753ff, false);
	}

	for (s32 n = 0; n < LEVELS_PER_PAGE && first + n < g_Front.numlevels; n++) {
		const s32 row = n / 4;
		const s32 col = n % 4;
		// the highlighted picture as GoldenEye brightens it, the rest dimmed
		const u32 colour = n == g_Front.highlight ? 0xffffffff : 0x6e6e6eff;

		const s32 image = frontStageImage(g_Front.levels[first + n]);

		gdl = frontImageOrRelease(gdl, frontStageRelease(image), image, STAGE_IMAGE_W, STAGE_IMAGE_H, G_IM_FMT_I, false,
				86 + 85 * col, 134 + 70 * row, 34, 22, STAGE_IMAGE_W, STAGE_IMAGE_H, colour, false);
	}

	gdl = frontTextSetup(gdl);

	for (s32 n = 0; n < LEVELS_PER_PAGE && first + n < g_Front.numlevels; n++) {
		const s32 stagenum = g_Front.levels[first + n];
		const char *name = stagenum == STAGE_MP_RANDOM ? frontString(155) : frontStageName(stagenum);
		const u32 colour = n == g_Front.highlight ? 0xffffff00 : 0x96969600;
		char caption[32];
		s32 w;
		s32 h;
		s32 x;
		s32 y;
		s32 i;

		// GoldenEye's captions are its names in capitals
		for (i = 0; name[i] && i < (s32)sizeof(caption) - 1; i++) {
			caption[i] = name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i];
		}

		caption[i] = '\0';

		frontMeasure(&g_Front.gothic, caption, 0, &w, &h);

		x = 0x56 + 0x55 * (n % 4) - 0x1f;
		y = 0x97 + 0x46 * (n / 4) - h;
		gdl = frontText(gdl, &g_Front.gothic, &x, &y, caption, colour | 0xff, 0, false);

		x = 0x56 + 0x55 * (n % 4) - 0x1f;
		y = 0x97 + 0x46 * (n / 4) - h;
		gdl = frontText(gdl, &g_Front.gothic, &x, &y, caption, colour | 0x64, 0, false);
	}

	if (g_Front.numlevels > LEVELS_PER_PAGE) {
		gdl = frontTab(gdl, TITLE_NEXT, NEXTTAB_TEXT_TOP, NEXTTAB_TEXT_BOTTOM, g_Front.tabnext);
		gdl = frontTextSetup(gdl);
	}

	return gdl;
}

/** constructor_menu13_mpscenario(): SCENARIO: and GoldenEye's eight, the team games grey. */
static Gfx *frontDrawScenario(Gfx *gdl)
{
	gdl = frontPrint(gdl, 0x37, 0x66, frontString(TITLE_SCENARIOHEAD), COLOUR_ON);

	for (s32 i = 0; i < NUM_SCENARIO_ROWS; i++) {
		const char *text = frontString(TITLE_SCEN_NORMAL + i);
		const s32 y = 0x83 + i * 0x16;
		s32 w;
		s32 h;

		frontMeasure(&g_Front.zurich, text, 0, &w, &h);

		if (i == g_Front.highlight) {
			gdl = frontFillRect(gdl, 0x37, y - 1, w + 0x3c, y + 0xe, COLOUR_HIGHLIGHT);
			gdl = frontTextSetup(gdl);
		}

		gdl = frontPrint(gdl, 0x39, y, text, frontScenarioRowOn(i) ? COLOUR_ON : COLOUR_OFF);
	}

	return gdl;
}

/** A player's panel on the per-player pages: GoldenEye's two across or four in a square, and one across the middle. */
static void frontPanel(s32 player, s32 numplayers, s32 *left, s32 *top, s32 *width)
{
	if (numplayers == 1) {
		*left = 0x26;
		*width = 0x15e;
		*top = 0x1e + 0x46;
	} else if (numplayers == 2) {
		*left = 0x26;
		*width = 0x15e;
		*top = (player > 0 ? 0x8c : 0) + 0x1e;
	} else {
		*width = 0xaf;
		*top = (player >= 2 ? 0x8c : 0) + 0x1e;
		*left = ((player & 1) ? 0xaf : 0) + 0x26;
	}
}

/**
 * constructor_menu10_mphandicap() and constructor_menu11_mpcontrol(): a panel a
 * player - two across the page one above the other, or four in a square - with
 * the prompt until the player has chosen and their value under it. One player,
 * which GoldenEye never had, gets one panel across the middle.
 */
static Gfx *frontDrawPlayerPanels(Gfx *gdl)
{
	const s32 numplayers = frontNumPlayers();

	if (numplayers >= 2) {
		gdl = frontFillRect(gdl, 0x26, 0xa9, 0x184, 0xab, 0x00000090);
	}

	if (numplayers >= 3) {
		gdl = frontFillRect(gdl, 0xd4, 0x1e, 0xd6, 0x136, 0x00000080);
	}

	gdl = frontTextSetup(gdl);

	for (s32 i = 0; i < numplayers; i++) {
		s32 left;
		s32 top;
		s32 width;
		const char *prompt;
		const char *value;
		char valuebuf[64];
		s32 w;
		s32 h;

		frontPanel(i, numplayers, &left, &top, &width);

		const s32 midx = (width >> 1) + left;
		const s32 midy = top + 0x46;

		if (g_Front.screen == SCREEN_HEALTH) {
			prompt = frontString(TITLE_SELECTHANDICAP);
			value = frontString(TITLE_HEALTH_FIRST + g_Front.handicap[i]);
		} else {
			const s32 style = optionsGetControlMode(i);

			prompt = frontString(TITLE_SELECTCONTROLSTYLE);

			if (style >= 0 && style < 8) {
				value = frontString(TITLE_CONTROL_FIRST + style);
			} else {
				// Perfect Dark's own, the keyboard and mouse
				snprintf(valuebuf, sizeof(valuebuf), "Ext\n");
				value = valuebuf;
			}
		}

		if (!g_Front.chosen[i]) {
			frontMeasure(&g_Front.zurich, prompt, 0, &w, &h);
			gdl = frontPrint(gdl, midx - (w >> 1), midy - (h >> 1) - 0xf, prompt, COLOUR_ON);
		}

		frontMeasure(&g_Front.zurich, value, 0, &w, &h);
		gdl = frontPrint(gdl, midx - (w >> 1), midy - (h >> 1) + 0xf, value, COLOUR_ON);
	}

	return gdl;
}

/**
 * frontRenderCharacterPortrait(): a portrait's four tiles about its middle,
 * 70 wide and 84 high and growing with size; faded out towards the panel's
 * sides (frontCalculateCharacterImageAlpha()), and greyed while another
 * player has it. GoldenEye's vertices run a tile's t up the screen.
 */
static Gfx *frontPortraitDraw(Gfx *gdl, s32 player, s32 k, s32 cx, s32 cy, s32 left, s32 right, s32 size)
{
	const s32 *tiles = frontPortrait(g_Front.characters[k]);
	const s32 hw = size + 0x23;
	const s32 hh = size + 0x2a;
	const s32 grey = size == 0 && frontCharacterTaken(player, k);
	const u32 shade = grey ? 0x6e : 0xff;
	s32 alpha = 0xff;

	if (cx - hw < left || cx + hw > right) {
		return gdl;
	}

	if (size == 0) {
		const s32 edge = cx - left < right - cx ? cx - left : right - cx;

		if (edge < hw + 0x28) {
			alpha = 0xff * (edge - hw) / 0x28;
		}
	}

	// the release's portrait is one picture, drawn over the four tiles' square
	{
		const char *release = frontReleasePortrait(g_Front.characters[k]);
		struct textureconfig tex;

		if (release && frontReleasePicture(release, &tex)) {
			texSelect(&gdl, &tex, 1, 0, 2, 1, NULL);

			return frontImageRect(gdl, cx, cy + size, hw, hh, FRONT_PICTURE_TEXELS, -FRONT_PICTURE_TEXELS,
					(shade << 24) | (shade << 16) | (shade << 8) | alpha, true, true);
		}
	}

	for (s32 t = 0; t < 4; t++) {
		const f32 qx = cx + (t & 1 ? hw / 2.0f : -hw / 2.0f);
		const f32 qy = cy + size + (t & 2 ? hh / 2.0f : -hh / 2.0f);

		gdl = frontImage(gdl, tiles[t], 0x41, 0x41, G_IM_FMT_I, false, qx, qy, hw / 2.0f, hh / 2.0f, 0x41, -0x41,
				(shade << 24) | (shade << 16) | (shade << 8) | alpha, true);
	}

	return gdl;
}

/** constructor_menu0F_mpcharsel(): the prompt, the character's name, and the strip. */
static Gfx *frontDrawCharacters(Gfx *gdl)
{
	const s32 numplayers = frontNumPlayers();

	if (numplayers >= 2) {
		gdl = frontFillRect(gdl, 0x26, 0xa9, 0x184, 0xab, 0x00000090);
	}

	if (numplayers >= 3) {
		gdl = frontFillRect(gdl, 0xd4, 0x1e, 0xd6, 0x136, 0x00000080);
	}

	for (s32 i = 0; i < numplayers; i++) {
		s32 left;
		s32 top;
		s32 width;
		s32 w;
		s32 h;
		char name[64];

		frontPanel(i, numplayers, &left, &top, &width);

		const s32 midx = (width >> 1) + left;
		const s32 stripleft = left + 0xd;
		const s32 stripright = left + width - 0xe;
		const s32 base = midx - g_Front.charscroll[i];

		gdl = frontPortraitDraw(gdl, i, g_Front.charprev[i], base, top + 0x46, stripleft, stripright, g_Front.charsize[i]);

		if (!g_Front.chosen[i] && g_Front.charsize[i] == 0) {
			for (s32 d = -3; d <= 3; d++) {
				const s32 k = g_Front.charprev[i] + d;

				if (d != 0 && k >= 0 && k < g_Front.numcharacters) {
					gdl = frontPortraitDraw(gdl, i, k, base + d * PORTRAIT_SPACING, top + 0x46, stripleft, stripright, 0);
				}
			}
		}

		gdl = frontTextSetup(gdl);

		if (!g_Front.chosen[i] && g_Front.charsize[i] == 0) {
			const char *prompt = frontString(TITLE_SELECTCHARACTER);

			frontMeasure(&g_Front.zurich, prompt, 0, &w, &h);
			gdl = frontPrint(gdl, midx - (w >> 1), top + 5, prompt, COLOUR_ON);
		}

		snprintf(name, sizeof(name), "%s", g_Front.numcharacters ? frontCharacterName(g_Front.characters[g_Front.charcur[i]]) : "");
		name[strcspn(name, "\n")] = '\0';
		frontMeasure(&g_Front.zurich, name, 0, &w, &h);
		gdl = frontPrint(gdl, midx - (w >> 1), top + 0x46 + 0x32, name, COLOUR_ON);
	}

	return gdl;
}

/* ---- the solo screens --------------------------------------------------- */

/** Appends a string with its line breaks left out, the labels being one line. */
static void frontAppend(char *buf, size_t len, const char *text)
{
	size_t n = strlen(buf);

	for (; *text && n + 1 < len; text++) {
		if (*text != '\n') {
			buf[n++] = *text;
		}
	}

	buf[n] = '\0';
}

/** textWrap(): the text broken into lines no wider than width. */
static void frontWrap(const struct gefont *font, const char *text, char *out, size_t len, s32 width)
{
	size_t n = 0;
	size_t line = 0;

	while (*text && n + 2 < len) {
		size_t wordat;
		s32 w;
		s32 h;

		while (*text == ' ') {
			text++;
		}

		if (!*text) {
			break;
		}

		wordat = n;

		if (n > line) {
			out[n++] = ' ';
		}

		while (*text && *text != ' ' && *text != '\n' && n + 2 < len) {
			out[n++] = *text++;
		}

		out[n] = '\0';
		frontMeasure(font, out + line, 0, &w, &h);

		// the word did not fit: the space before it becomes the break
		if (w > width && wordat > line) {
			out[wordat] = '\n';
			line = wordat + 1;
		}

		// the text's own break, taken after the word before it is measured:
		// taken first, the last word of every paragraph was never measured
		// and ran past the width it was wrapped to
		if (*text == '\n') {
			text++;
			out[n++] = '\n';
			line = n;
		}
	}

	out[n] = '\0';
}

static s32 frontCountLines(const char *text)
{
	s32 lines = 1;

	for (; *text; text++) {
		if (*text == '\n' && text[1]) {
			lines++;
		}
	}

	return lines;
}

/**
 * print_current_solo_briefing_stage_name(): the difficulty, the chapter the
 * mission belongs to and the mission itself, above every solo screen after the
 * mission select.
 */
static Gfx *frontMissionHeader(Gfx *gdl, s32 withdifficulty)
{
	const s32 row = frontMissionRow(g_Front.mission);
	const s32 chapter = frontMissionChapter(row);
	char buf[128];

	if (row < 0) {
		return gdl;
	}

	if (withdifficulty) {
		buf[0] = '\0';
		frontAppend(buf, sizeof(buf), frontString(TITLE_DIFF_FIRST + g_Front.difficulty));
		frontAppend(buf, sizeof(buf), frontString(TITLE_JB));
		gdl = frontPrint(gdl, 0x37, 0x57, buf, COLOUR_ON);
	}

	if (chapter >= 0) {
		buf[0] = '\0';
		frontAppend(buf, sizeof(buf), frontString(TITLE_MISSION2));
		frontAppend(buf, sizeof(buf), g_Missions[chapter].numeral);
		frontAppend(buf, sizeof(buf), ": ");
		frontAppend(buf, sizeof(buf), frontString(g_Missions[chapter].name));
		gdl = frontPrint(gdl, 0x37, 0x67, buf, COLOUR_ON);
	}

	buf[0] = '\0';
	frontAppend(buf, sizeof(buf), frontString(TITLE_PART));
	frontAppend(buf, sizeof(buf), g_Missions[row].numeral);
	frontAppend(buf, sizeof(buf), ": ");
	frontAppend(buf, sizeof(buf), frontString(g_Missions[row].name));

	return frontPrint(gdl, 0x37, 0x77, buf, COLOUR_ON);
}

/**
 * constructor_menu07_missionsel(): a mission's name on its slide, in Bank
 * Gothic capitals, white under the cursor and grey elsewhere - each drawn twice,
 * once solid and once at 100 of 255, as GoldenEye draws them.
 */
static Gfx *frontDrawMission(Gfx *gdl)
{
	for (s32 col = 0; col < MISSION_COLS; col++) {
		for (s32 row = 0; row < MISSION_ROWS; row++) {
			const s32 mission = row * MISSION_COLS + col;
			char name[64];
			u32 colour;
			s32 x;
			s32 y;
			s32 w;
			s32 h;

			if (frontHighestDifficulty(mission) < 0) {
				continue;
			}

			frontMissionName(mission, name, sizeof(name));
			frontMeasure(&g_Front.gothic, name, 0, &w, &h);

			colour = mission == g_Front.highlight ? 0xffffff00 : 0x96969600;
			x = g_MissionX[col] - 0x1f;
			y = g_MissionY[row] - h + 0x1d;

			{
				s32 tx = x;
				s32 ty = y;

				gdl = frontText(gdl, &g_Front.gothic, &tx, &ty, name, colour | 0xff, 0, false);
			}

			{
				s32 tx = x;
				s32 ty = y;

				gdl = frontText(gdl, &g_Front.gothic, &tx, &ty, name, colour | 0x64, 0, false);
			}
		}
	}

	return gdl;
}

/** EXTRA: the difficulty page's rows again, with no mission over them. */
static Gfx *frontDrawExtra(Gfx *gdl)
{
	static const char *rows[NUM_EXTRA_ROWS] = { "Cinema\n", "Monitor Programmes\n" };

	gdl = frontPrint(gdl, 0x37, 0x8f, "EXTRA:\n", COLOUR_ON);

	if (g_Front.highlight >= 0) {
		gdl = frontFillRect(gdl, 0x7e, g_Front.highlight * 0x1e + 0xb2, 0x140, g_Front.highlight * 0x1e + 0xc3, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	for (s32 i = 0; i < NUM_EXTRA_ROWS; i++) {
		char num[8];

		snprintf(num, sizeof(num), "%d.\n", i + 1);
		gdl = frontPrint(gdl, 0x82, i * 0x1e + 0xb4, num, COLOUR_ON);
		gdl = frontPrint(gdl, 0x96, i * 0x1e + 0xb4, rows[i], COLOUR_ON);
	}

	return gdl;
}

/**
 * What GoldenEye's fifty-two monitor programmes are, by the number a monitor's
 * record names one with (monitorSetImageByNum()): the decompilation's own
 * descriptions, tidied. From 21 on they are the pieces of one machine - pick a
 * picture at random, tint it, then scroll or zoom or flash it and come round
 * again - and each is an entry into it.
 */
static const char *g_MonitorNames[] = {
	"Bond logo", "Desktops and satellite", "Ten astrological screens", "Three wave patterns", "Wave pattern",
	"Green text, scrolling up", "Red text, scrolling down", "Dark green text, scrolling down",
	"Red bar graph", "Blue bar graph", "Green bar graph", "Radar", "Spinning cube",
	"Location, weapon armed, target", "Red target", "Satellite targeting", "Global map",
	"Karl yelling", "Skateboard", "Police guy", "Off",
	"One of seven at random", "Random screens, or dull ones", "Random screen and effect",
	"Random: shuttle 1", "Random: shuttle 2", "Random: full Earth 1", "Random: full Earth 2",
	"Random: blue stars", "Random: galaxy 1", "Random: galaxy 2", "Random: Earth text",
	"Random: target Earth", "Random: galaxy 3",
	"Tint: one of four", "Tint: red", "Tint: green", "Tint: blue", "Effect: one of five",
	"Effect: scroll right", "Effect: scroll up, fast", "Effect: scroll up", "Effect: scroll and zoom 1",
	"Effect: scroll and zoom 2", "Effect: wait and route", "Effect: flash",
	"Red, brightening", "Green, brightening", "Solid grey", "Solid red", "Solid green", "Solid black",
};

#define MONITOR_CX 220.0f
#define MONITOR_CY 226.0f
#define MONITOR_HW 84.0f
#define MONITOR_HH 56.0f

// a menu pixel across the plane the folder lies in: tan(30) over half of 330
#define FOLDER_PERPIXEL (FOLDER_EYEZ * 0.57735027f / 165.0f)

/**
 * The folder's own camera, for the sets and for one set's screen drawn large -
 * and drawn as the folder is, outside the frame the 2-D layer is held in
 * (G_ASPECT_CENTER_EXT, frontX()). Inside it a model is squeezed across by
 * SCREEN_ASPECT over the window's own shape: a twelfth too wide on 4:3, which
 * nobody saw, and a fifth too narrow and off its cell on 16:9. The caller turns
 * the mode back on when its models are drawn.
 */
static Gfx *frontTvCamera(Gfx *gdl, Mtxf *camera)
{
	static Vp vp;
	Mtxf persp;
	Mtx *projection = gfxAllocateMatrix();
	u16 perspnorm;

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	vp.vp.vscale[0] = viGetWidth() * 2;
	vp.vp.vscale[1] = viGetHeight() * 2;
	vp.vp.vscale[2] = 511;
	vp.vp.vscale[3] = 0;
	vp.vp.vtrans[0] = viGetWidth() * 2;
	vp.vp.vtrans[1] = viGetHeight() * 2;
	vp.vp.vtrans[2] = 511;
	vp.vp.vtrans[3] = 0;

	guPerspectiveF(persp.m, &perspnorm, FOLDER_FOVY, videoGetAspect(), 100.0f, 10000.0f, 1.0f);
	guMtxF2L(persp.m, projection);

	gSPViewport(gdl++, &vp);
	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);

	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);

	mtx00016ae4(camera, -900.0f, 990.0f, FOLDER_EYEZ, -900.0f, 990.0f, 0.0f, 0.0f, 1.0f, 0.0f);

	return gdl;
}

/**
 * One of the monitor programmes, large: its number and name, and its screen.
 *
 * The screen is a set's own - the four vertices of the TV's screen node, drawn
 * by tvscreenRender() as the page of sets and a level draw them, stood square
 * to the folder's camera and stretched over the tube. It was a texture
 * rectangle doing tvscreenRender()'s arithmetic over again, and what it did not
 * do over again is what the user saw: "some of our monitor programmes in the
 * menu dont render correctly when you click on them to enlarge them". It gave a
 * picture no mip levels, so every picture stored with them - all the colour
 * ones, and Karl - was read from the wrong place; it left the screen's alpha
 * out, so "Off" showed its wave; and a rectangle cannot turn, so the radar's
 * sweep stood still.
 */
static Gfx *frontDrawMonitorView(Gfx *gdl)
{
	struct model *model = g_Front.tvmodels[0];
	struct modelnode *node = g_Front.tvdef ? modelGetPart(g_Front.tvdef, MODELPART_0000) : NULL;
	char line[96];

	gdl = frontPrint(gdl, 0x37, 0x77, "MONITOR PROGRAMMES\n", COLOUR_ON);

	if (g_Front.nummonitors <= 0) {
		return frontPrint(gdl, 0x37, 0x8f, "None in this conversion.\n", COLOUR_ON);
	}

	snprintf(line, sizeof(line), "%d of %d: %s\n", g_Front.monitor + 1, g_Front.nummonitors,
			g_Front.monitor < ARRAYCOUNT(g_MonitorNames) ? g_MonitorNames[g_Front.monitor] : "");
	gdl = frontPrint(gdl, 0x37, 0x8f, line, COLOUR_ON);

	// the tube it is shown on
	gdl = frontFillRect(gdl, (s32)(MONITOR_CX - MONITOR_HW) - 3, (s32)(MONITOR_CY - MONITOR_HH) - 3,
			(s32)(MONITOR_CX + MONITOR_HW) + 3, (s32)(MONITOR_CY + MONITOR_HH) + 3, 0x000000ff);

	if (model && node && (node->type & 0xff) == MODELNODETYPE_DL && g_Front.monitorscreen.cmdlist) {
		const Vtx *v = node->rodata->dl.vertices;
		s32 min[3] = { v[0].x, v[0].y, v[0].z };
		s32 max[3] = { v[0].x, v[0].y, v[0].z };

		for (s32 i = 1; i < 4; i++) {
			for (s32 k = 0; k < 3; k++) {
				min[k] = v[i].v[k] < min[k] ? v[i].v[k] : min[k];
				max[k] = v[i].v[k] > max[k] ? v[i].v[k] : max[k];
			}
		}

		// the screen faces down the set's z, as the page of sets shows it
		if (max[0] > min[0] && max[1] > min[1]) {
			// a menu pixel is the same step across the folder's plane as down
			// it (the sets' two factors are no use here: they were fitted to
			// where a TV's model stands, not to a flat picture)
			const f32 stepx = FOLDER_PERPIXEL;
			const f32 stepy = FOLDER_PERPIXEL;
			const f32 sx = 2.0f * MONITOR_HW * stepx / (max[0] - min[0]);
			const f32 sy = 2.0f * MONITOR_HH * stepy / (max[1] - min[1]);
			Mtx *mtx = gfxAllocateMatrix();
			Mtxf camera;
			Mtxf world;
			s32 lvupdate60;
			f32 lvupdate60f;
			s32 prevsrc;

			gdl = frontTvCamera(gdl, &camera);

			mtx4LoadIdentity(&world);
			world.m[0][0] = sx;
			world.m[1][1] = sy;
			world.m[3][0] = -900.0f + (MONITOR_CX - 220.0f) * stepx - sx * (max[0] + min[0]) * 0.5f;
			world.m[3][1] = 990.0f - (MONITOR_CY - 165.0f) * stepy - sy * (max[1] + min[1]) * 0.5f;
			world.m[3][2] = -(max[2] + min[2]) * 0.5f;
			mtx4MultMtx4InPlace(&camera, &world);
			mtxF2L(&world, (Mtxf *)mtx);
			model->matrices = (Mtxf *)mtx;

			gSPClearGeometryMode(gdl++, G_ZBUFFER | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG | G_CULL_BOTH);
			gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);

			// its programme for the frame and the list its node would draw,
			// which is drawn here without the set
			frontMonitorClock(1, &lvupdate60, &lvupdate60f);
			prevsrc = modSetTextureSourceMod(g_Front.moddir);
			gdl = tvscreenRender(model, node, &g_Front.monitorscreen, gdl, 0, 1);
			modSetTextureSourceMod(prevsrc);
			frontMonitorClock(0, &lvupdate60, &lvupdate60f);

			gSPDisplayList(gdl++, ((union modelrwdata *)modelGetNodeRwData(model, node))->dl.gdl);
			gSPClearGeometryMode(gdl++, G_CULL_BOTH);
			gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
		}
	}

	gdl = frontTextSetup(gdl);
	gdl = frontTab(gdl, TITLE_NEXT, NEXTTAB_TEXT_TOP, NEXTTAB_TEXT_BOTTOM, g_Front.tabnext);
	gdl = frontTextSetup(gdl);

	return gdl;
}

/**
 * The page of TV sets. They are the one thing on the folder screens besides the
 * folder that is a model, so they are drawn the folder's own way - its camera,
 * its field of view, into a z buffer of their own - each stood where the
 * film strip's picture of that cell is: the camera looks square at the plane
 * the folder lies in, FOLDER_EYEZ off it under FOLDER_FOVY, so a menu pixel is
 * a fixed step across that plane and a cell's place follows from its pixel.
 */
static Gfx *frontDrawTvs(Gfx *gdl)
{
	const s32 first = g_Front.monitorpage * TVS_PER_PAGE;
	const f32 perpixel = FOLDER_PERPIXEL;
	Mtxf camera;
	s32 lvupdate60;
	f32 lvupdate60f;
	s32 prevsrc;

	if (!g_Front.tvdef) {
		return gdl;
	}

	gdl = frontTvCamera(gdl, &camera);
	gdl = zbufClear(gdl);
	gSPSetGeometryMode(gdl++, G_ZBUFFER);

	frontMonitorClock(1, &lvupdate60, &lvupdate60f);
	prevsrc = modSetTextureSourceMod(g_Front.moddir);

	for (s32 n = 0; n < TVS_PER_PAGE && first + n < g_Front.nummonitors; n++) {
		struct modelrenderdata renderdata = { NULL, true, 3 };
		struct model *model = g_Front.tvmodels[n];
		const f32 px = 86.0f + 85.0f * (n % 4) + 8.0f;
		const f32 py = 134.0f + 70.0f * (n / 4) + 4.0f;
		Mtxf world;
		Mtxf turn;
		Mtxf tmp;

		mtx4LoadYRotation(g_TvYaw, &world);
		mtx4LoadXRotation(g_TvPitch, &turn);
		mtx4MultMtx4InPlace(&turn, &world);
		mtx00015f04(g_TvScale * (n == g_Front.highlight ? 1.12f : 1.0f), &world);
		world.m[3][0] = -900.0f + (px - 220.0f) * perpixel * g_TvSpreadX;
		world.m[3][1] = 990.0f - (py - 165.0f) * perpixel * g_TvSpreadY + g_TvLift;
		world.m[3][2] = g_TvZ;
		mtx4MultMtx4InPlace(&camera, &world);

		renderdata.unk00 = &world;
		renderdata.unk10 = gfxAllocate(g_Front.tvdef->nummatrices * sizeof(Mtxf));
		mtx4Copy(&world, renderdata.unk10);
		model->matrices = renderdata.unk10;

		modelUpdateRelations(model);

		// the screen first, which writes the list its node draws, then the set
		gdl = tvscreenRender(model, modelGetPart(g_Front.tvdef, MODELPART_0000), &g_Front.tvscreens[n], gdl, 0, 1);

		renderdata.unk30 = 1;
		renderdata.flags = 3;
		renderdata.zbufferenabled = true;
		renderdata.gdl = gdl;
		modelRender(&renderdata, model);
		gdl = renderdata.gdl;

		for (s32 i = 0; i < g_Front.tvdef->nummatrices; i++) {
			mtx4Copy(&model->matrices[i], &tmp);
			mtxF2L(&tmp, &model->matrices[i]);
		}
	}

	modSetTextureSourceMod(prevsrc);
	frontMonitorClock(0, &lvupdate60, &lvupdate60f);

	gSPClearGeometryMode(gdl++, G_ZBUFFER);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	return frontTextSetup(gdl);
}

/** Monitor Programmes: the strip, the sets on it, and the name of the one under the cursor. */
static Gfx *frontDrawMonitors(Gfx *gdl)
{
	const s32 first = g_Front.monitorpage * TVS_PER_PAGE;
	char line[96];

	for (s32 i = 0; i < 3; i++) {
		gdl = frontFillRect(gdl, 0x25, 0x6c + i * 0x46, 0x185, 0xa0 + i * 0x46, 0x101010ff);
	}

	for (s32 i = 0; i < 3; i++) {
		gdl = frontImageOrRelease(gdl, DOT_RELEASE, DOT_IMAGE, 16, 16, G_IM_FMT_I, true, 213, 104 + 70 * i, 176, 4, 0x2f0, 0x12, 0x6b6753ff, false);
		gdl = frontImageOrRelease(gdl, DOT_RELEASE, DOT_IMAGE, 16, 16, G_IM_FMT_I, true, 213, 164 + 70 * i, 176, 4, 0x2f0, 0x12, 0x6b6753ff, false);
	}

	if (g_Front.nummonitors <= 0 || !g_Front.tvdef) {
		gdl = frontTextSetup(gdl);
		return frontPrint(gdl, 0x37, 0x57, "No monitor programmes in this conversion.\n", COLOUR_ON);
	}

	gdl = frontDrawTvs(gdl);

	for (s32 n = 0; n < TVS_PER_PAGE && first + n < g_Front.nummonitors; n++) {
		const u32 colour = n == g_Front.highlight ? 0xffffff00 : 0x96969600;
		char caption[8];
		s32 w;
		s32 h;
		s32 x;
		s32 y;

		snprintf(caption, sizeof(caption), "%d\n", first + n + 1);
		frontMeasure(&g_Front.gothic, caption, 0, &w, &h);

		// its number, to the right of the set
		x = 86 + 85 * (n % 4) + 34;
		y = 134 + 70 * (n / 4) + 14 - h;
		gdl = frontText(gdl, &g_Front.gothic, &x, &y, caption, colour | 0xff, 0, false);
	}

	if (g_Front.highlight >= 0 && first + g_Front.highlight < g_Front.nummonitors) {
		const s32 n = first + g_Front.highlight;

		snprintf(line, sizeof(line), "%d of %d: %s\n", n + 1, g_Front.nummonitors,
				n < ARRAYCOUNT(g_MonitorNames) ? g_MonitorNames[n] : "");
		gdl = frontPrint(gdl, 0x37, 0x57, line, COLOUR_ON);
	}

	gdl = frontTab(gdl, TITLE_NEXT, NEXTTAB_TEXT_TOP, NEXTTAB_TEXT_BOTTOM, g_Front.tabnext);
	gdl = frontTextSetup(gdl);

	return gdl;
}

/**
 * The Cinema's second page, laid out as the difficulty page is: the mission's
 * heading, and the two things of it there are to watch.
 */
static Gfx *frontDrawCinemaPick(Gfx *gdl)
{
	static const char *rows[NUM_CINEMA_ROWS] = { "Intro\n", "Outro\n" };   // the user's names, in the case GoldenEye sets its difficulties in

	gdl = frontMissionHeader(gdl, false);
	gdl = frontPrint(gdl, 0x37, 0x8f, "CINEMA:\n", COLOUR_ON);

	if (g_Front.highlight >= 0) {
		gdl = frontFillRect(gdl, 0x7e, g_Front.highlight * 0x1e + 0xb2, 0xf0, g_Front.highlight * 0x1e + 0xc3, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	for (s32 i = 0; i < NUM_CINEMA_ROWS; i++) {
		char num[8];

		snprintf(num, sizeof(num), "%d.\n", i + 1);
		gdl = frontPrint(gdl, 0x82, i * 0x1e + 0xb4, num, COLOUR_ON);
		gdl = frontPrint(gdl, 0x96, i * 0x1e + 0xb4, rows[i], COLOUR_ON);
	}

	return gdl;
}

/** constructor_menu08_difficulty(): the difficulties open to this mission, numbered. */
static Gfx *frontDrawDifficulty(Gfx *gdl)
{
	const s32 highest = frontHighestDifficulty(g_Front.mission);

	gdl = frontMissionHeader(gdl, false);
	gdl = frontPrint(gdl, 0x37, 0x8f, frontString(TITLE_DIFFICULTY), COLOUR_ON);

	if (g_Front.highlight >= 0) {
		gdl = frontFillRect(gdl, 0x7e, g_Front.highlight * 0x1e + 0xb2, 0xf0, g_Front.highlight * 0x1e + 0xc3, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	for (s32 i = 0; i < NUM_DIFFICULTIES; i++) {
		char num[8];

		if (i > 0 && highest < i) {
			continue;
		}

		snprintf(num, sizeof(num), "%d.\n", i + 1);
		gdl = frontPrint(gdl, 0x82, i * 0x1e + 0xb4, num, COLOUR_ON);
		gdl = frontPrint(gdl, 0x96, i * 0x1e + 0xb4, frontString(TITLE_DIFF2_FIRST + i), COLOUR_ON);
	}

	return gdl;
}

/**
 * constructor_menu09_007options(): four bars 300 wide and 33 apart, each with
 * the multiplier it stands for beside it. GoldenEye shows the enemy's accuracy
 * as a tenth of its own value, and its reaction speed is the one Perfect Dark
 * does not act on.
 */
static Gfx *frontDraw007(Gfx *gdl)
{
	static const s32 tops[NUM_SLIDERS] = { 164, 197, 230, 263 };
	static const s32 labels[NUM_SLIDERS] = { TITLE_ENEMYHEALTH, TITLE_ENEMYDAMAGE, TITLE_ENEMYACCURACY, TITLE_REACTION };

	gdl = frontMissionHeader(gdl, true);
	gdl = frontPrint(gdl, 55, 143, frontString(TITLE_SPECOPS), COLOUR_ON);

	for (s32 i = 0; i < NUM_SLIDERS; i++) {
		const f32 value = g_Front.slider[i];
		const s32 y = tops[i];
		const s32 filled = i == SLIDER_REACTION ? (s32)(value * 300.0f) : (s32)(sqrtf(value / 10.0f) * 300.0f);
		const s32 percent = i == SLIDER_ACCURACY ? (s32)(value * 10.0f) : (s32)(value * 100.0f);
		char text[16];
		s32 w;
		s32 h;
		s32 x;

		gdl = frontFillRect(gdl, 55, y + 17, 355, y + 28, COLOUR_HIGHLIGHT);
		gdl = frontFillRect(gdl, 55, y + 17, filled + 55, y + 28, COLOUR_BAR);

		if (g_Front.highlight == i) {
			gdl = frontFillRect(gdl, 55, y - 1, 199, y + 14, COLOUR_HIGHLIGHT);
		}

		gdl = frontTextSetup(gdl);
		gdl = frontPrint(gdl, 57, y, frontString(labels[i]), COLOUR_ON);

		snprintf(text, sizeof(text), "%d%%\n", percent);
		frontMeasure(&g_Front.zurich, text, 0, &w, &h);
		x = 285 - w;
		gdl = frontPrint(gdl, x, y, text, COLOUR_ON);
	}

	return gdl;
}

/**
 * constructor_menu0A_briefing(): the objectives this difficulty is given on the
 * first page, lettered, and one of the briefing's four paragraphs on each of
 * the rest. Both the objectives and the paragraphs are the mission's own text.
 */
static Gfx *frontDrawBriefing(Gfx *gdl)
{
	static char wrapped[2048];

	gdl = frontMissionHeader(gdl, true);
	gdl = frontPrint(gdl, 0x37, 0x8f, frontString(TITLE_BRIEF_FIRST + g_Front.briefpage), COLOUR_ON);

	if (g_Front.briefpage == BRIEF_TITLE) {
		s32 lines = 0;
		s32 shown = 0;

		for (s32 i = 0; i < BRIEF_OBJECTIVES; i++) {
			s32 difficulty = 0;
			const s32 textid = frontBriefObjective(i, &difficulty);
			char label[8];

			if (!textid || g_Front.difficulty < difficulty) {
				continue;
			}

			snprintf(label, sizeof(label), "%c.\n", 'a' + shown);
			gdl = frontPrint(gdl, 0x37, 0xa7 + lines * frontLineHeight(&g_Front.zurich), label, COLOUR_ON);

			frontWrap(&g_Front.zurich, frontLangString(textid), wrapped, sizeof(wrapped), 0x140);
			gdl = frontPrint(gdl, 0x4b, 0xa7 + lines * frontLineHeight(&g_Front.zurich), wrapped, COLOUR_ON);

			lines += frontCountLines(wrapped);
			shown++;
		}

		return gdl;
	}

	frontWrap(&g_Front.zurich, frontLangString(frontBriefParagraph(g_Front.briefpage - 1)), wrapped, sizeof(wrapped), 0x140);

	return frontPrint(gdl, 0x37, 0xa7, wrapped, COLOUR_ON);
}

/**
 * constructor_menu0C_missionfailed(): how the mission stands - killed in
 * action, aborted, completed or failed, and all but completed in red - over
 * its objectives, each with how it was left. print_objectives_and_status_to_menu()
 * gives an objective that was never completed as failed, and so does this.
 */
static Gfx *frontDrawReport(Gfx *gdl)
{
	static char wrapped[2048];
	const char *status;
	s32 width = 0;
	s32 height = 0;
	s32 lines = 0;
	s32 shown = 0;

	gdl = frontMissionHeader(gdl, true);
	gdl = frontPrint(gdl, 0x37, 0x8f, frontString(TITLE_REPORT), COLOUR_ON);

	frontMeasure(&g_Front.zurich, frontString(TITLE_MISSIONSTATUS), 0, &width, &height);
	gdl = frontPrint(gdl, 0x37, 0xa7, frontString(TITLE_MISSIONSTATUS), COLOUR_ON);

	status = frontString(g_FrontReport.kia ? TITLE_KIA : g_FrontReport.aborted ? TITLE_ABORTED
			: g_FrontReport.completed ? TITLE_COMPLETED : TITLE_FAILED);
	gdl = frontPrint(gdl, 0x37 + width, 0xa7, status, g_FrontReport.completed ? COLOUR_ON : COLOUR_FAILED);

	for (s32 i = 0; i < BRIEF_OBJECTIVES; i++) {
		s32 difficulty = 0;
		const s32 textid = frontBriefObjective(i, &difficulty);
		const s32 y = 0xbf + lines * frontLineHeight(&g_Front.zurich);
		const s32 done = g_FrontReport.objstatus[i] == OBJECTIVE_COMPLETE;
		char label[8];

		if (!textid || g_Front.difficulty < difficulty) {
			continue;
		}

		snprintf(label, sizeof(label), "%c.\n", 'a' + shown);
		gdl = frontPrint(gdl, 0x37, y, label, COLOUR_ON);

		// narrower than the briefing's, for the status beside it
		frontWrap(&g_Front.zurich, frontLangString(textid), wrapped, sizeof(wrapped), 0xdc);
		gdl = frontPrint(gdl, 0x4b, y, wrapped, COLOUR_ON);
		gdl = frontPrint(gdl, 0x136, y, frontString(done ? TITLE_OBJ_COMPLETED : TITLE_OBJ_FAILED),
				done ? COLOUR_ON : COLOUR_FAILED);

		lines += frontCountLines(wrapped);
		shown++;
	}

	return gdl;
}

/**
 * constructor_menu0D_missioncomplete(): the time, with the target a cheat is
 * won by and the best so far; the accuracy, the weapon of choice, and the shots
 * by where they landed. The hits are counted as GoldenEye counts them: the
 * accuracy is every hit over every shot, objects too, and a part's share is of
 * the hits on people - head, body, limb, gun and hat.
 */
static Gfx *frontDrawStats(Gfx *gdl)
{
	const s32 *shots = g_FrontReport.shots;
	const s32 others = shots[SHOTREGION_GUN] + shots[SHOTREGION_HAT];
	const s32 onpeople = shots[SHOTREGION_HEAD] + shots[SHOTREGION_BODY] + shots[SHOTREGION_LIMB] + others;
	const s32 allhits = onpeople > 0 ? onpeople : 1;
	const s32 secs = g_FrontReport.time60 / 60;
	const s32 difficulty = g_Front.difficulty >= DIFFICULTY_007 ? DIFF_PA : g_Front.difficulty;
	const s32 target = g_TargetTimes[g_Front.mission][difficulty];
	const s32 line = frontLineHeight(&g_Front.zurich);
	const s32 parts[4] = { shots[SHOTREGION_HEAD], shots[SHOTREGION_BODY], shots[SHOTREGION_LIMB], others };
	s32 best;
	char buf[128];

	frontLoadBestTimes();
	best = g_BestTimes[g_Front.mission][g_Front.difficulty];

	gdl = frontMissionHeader(gdl, true);
	gdl = frontPrint(gdl, 0x37, 0x8f, frontString(TITLE_STATISTICS), COLOUR_ON);

	gdl = frontPrint(gdl, 0x37, 0xa7, frontString(TITLE_TIME), COLOUR_ON);
	snprintf(buf, sizeof(buf), "%02d:%02d", secs / 60, secs % 60);
	gdl = frontPrint(gdl, 0x82, 0xa7, buf, COLOUR_ON);

	if (target > 0 && g_Front.difficulty != DIFFICULTY_007) {
		gdl = frontPrint(gdl, 0x37, 0xa9 + line, frontString(TITLE_TARGET), COLOUR_ON);

		if (best > 0) {
			snprintf(buf, sizeof(buf), "%02d:%02d     (%s  %02d:%02d)", target / 60, target % 60,
					frontString(TITLE_BESTTIME), best / 60, best % 60);
		} else {
			snprintf(buf, sizeof(buf), "%02d:%02d", target / 60, target % 60);
		}

		gdl = frontPrint(gdl, 0x82, 0xa9 + line, buf, COLOUR_ON);
	} else if (best > 0) {
		gdl = frontPrint(gdl, 0x37, 0xa9 + line, frontString(TITLE_BESTTIME), COLOUR_ON);
		snprintf(buf, sizeof(buf), "%02d:%02d", best / 60, best % 60);
		gdl = frontPrint(gdl, 0x82, 0xa9 + line, buf, COLOUR_ON);
	}

	gdl = frontPrint(gdl, 0x37, 0xcc, frontString(TITLE_ACCURACY), COLOUR_ON);
	snprintf(buf, sizeof(buf), "%.1f%%", shots[SHOTREGION_TOTAL] > 0
			? (onpeople + shots[SHOTREGION_OBJECT]) * 100.0f / shots[SHOTREGION_TOTAL] : 0.0f);
	gdl = frontPrint(gdl, 0x82, 0xcc, buf, COLOUR_ON);

	gdl = frontPrint(gdl, 0x37, 0xdc, frontString(TITLE_WEAPONOFCHOICE), COLOUR_ON);
	snprintf(buf, sizeof(buf), "%s%s", g_FrontReport.weapon, g_FrontReport.weapondual ? " x 2" : "");
	gdl = frontPrint(gdl, 0xbe, 0xdc, buf, COLOUR_ON);

	gdl = frontPrint(gdl, 0x37, 0xf4, frontString(TITLE_SHOTTOTAL), COLOUR_ON);
	snprintf(buf, sizeof(buf), "%d", shots[SHOTREGION_TOTAL]);
	gdl = frontPrint(gdl, 0x82, 0xf4, buf, COLOUR_ON);

	gdl = frontPrint(gdl, 0x37, 0xf4 + line, frontString(TITLE_KILLTOTAL), COLOUR_ON);
	snprintf(buf, sizeof(buf), "%d", g_FrontReport.kills);
	gdl = frontPrint(gdl, 0x82, 0xf4 + line, buf, COLOUR_ON);

	for (s32 i = 0; i < 4; i++) {
		gdl = frontPrint(gdl, 0xb4, 0xf4 + i * line, frontString(TITLE_HEADHITS + i), COLOUR_ON);
		snprintf(buf, sizeof(buf), "%d (%d%%)", parts[i], (s32)(parts[i] * 100.0f / allhits + 0.5f));
		gdl = frontPrint(gdl, 0x12c, 0xf4 + i * line, buf, COLOUR_ON);
	}

	return gdl;
}

Gfx *gexFrontRender(Gfx *gdl)
{
	if (!g_Front.active) {
		return gdl;
	}

	// viSetFillColor(0, 0, 0) and viFillScreen()
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_FILL);
	gDPSetScissor(gdl++, G_SC_NON_INTERLACE, 0, 0, viGetWidth(), viGetHeight());
	gDPSetFillColor(gdl++, GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1));
	gDPFillRectangle(gdl++, 0, 0, viGetWidth() - 1, viGetHeight() - 1);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);

	gdl = frontDrawReleaseBackdrop(gdl);
	gdl = frontDrawFolder(gdl);

	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = frontTextSetup(gdl);

	switch (g_Front.screen) {
	case SCREEN_MODE:
		gdl = frontDrawMode(gdl);
		break;
	case SCREEN_MPOPTIONS:
		gdl = frontDrawMpOptions(gdl);
		gdl = frontTab(gdl, TITLE_START, STARTTAB_TEXT_TOP, STARTTAB_TEXT_BOTTOM, g_Front.tabstart);
		gdl = frontTextSetup(gdl);
		break;
	case SCREEN_LEVEL:
		gdl = frontDrawLevel(gdl);
		break;
	case SCREEN_CINEMA:
		gdl = frontDrawMission(gdl);
		break;
	case SCREEN_CINEMAPICK:
		gdl = frontDrawCinemaPick(gdl);
		break;
	case SCREEN_EXTRA:
		gdl = frontDrawExtra(gdl);
		break;
	case SCREEN_MONITORS:
		gdl = frontDrawMonitors(gdl);
		break;
	case SCREEN_MONITORVIEW:
		gdl = frontDrawMonitorView(gdl);
		break;
	case SCREEN_SCENARIO:
		gdl = frontDrawScenario(gdl);
		break;
	case SCREEN_CHARACTERS:
		gdl = frontDrawCharacters(gdl);
		break;
	case SCREEN_MISSION:
		gdl = frontDrawMission(gdl);
		break;
	case SCREEN_DIFFICULTY:
		gdl = frontDrawDifficulty(gdl);
		break;
	case SCREEN_007OPTIONS:
		gdl = frontDraw007(gdl);
		gdl = frontTab(gdl, TITLE_START, STARTTAB_TEXT_TOP, STARTTAB_TEXT_BOTTOM, g_Front.tabstart);
		gdl = frontTextSetup(gdl);
		gdl = frontTab(gdl, TITLE_NEXT, NEXTTAB_TEXT_TOP, NEXTTAB_TEXT_BOTTOM, g_Front.tabnext);
		gdl = frontTextSetup(gdl);
		break;
	case SCREEN_BRIEFING:
		gdl = frontDrawBriefing(gdl);
		gdl = frontTab(gdl, TITLE_START, STARTTAB_TEXT_TOP, STARTTAB_TEXT_BOTTOM, g_Front.tabstart);
		gdl = frontTextSetup(gdl);

		if (g_Front.briefpage < NUM_BRIEF_PAGES - 1) {
			gdl = frontTab(gdl, TITLE_NEXT, NEXTTAB_TEXT_TOP, NEXTTAB_TEXT_BOTTOM, g_Front.tabnext);
			gdl = frontTextSetup(gdl);
		}
		break;
	case SCREEN_REPORT:
	case SCREEN_STATS:
		gdl = g_Front.screen == SCREEN_REPORT ? frontDrawReport(gdl) : frontDrawStats(gdl);
		gdl = frontTab(gdl, TITLE_NEXT, NEXTTAB_TEXT_TOP, NEXTTAB_TEXT_BOTTOM, g_Front.tabnext);
		gdl = frontTextSetup(gdl);
		break;
	default:
		gdl = frontDrawPlayerPanels(gdl);
		break;
	}

	// the per-player pages have no tabs and no cursor, as GoldenEye's have none
	if (g_Front.screen != SCREEN_HEALTH && g_Front.screen != SCREEN_CONTROLSTYLE && g_Front.screen != SCREEN_CHARACTERS) {
		gdl = frontTab(gdl, TITLE_PREVIOUS, PREVTAB_TEXT_TOP, PREVTAB_TEXT_BOTTOM, g_Front.tabprev);
		gdl = frontDrawCursor(gdl);
	}

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	return gdl;
}

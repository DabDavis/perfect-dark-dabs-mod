/**
 * GE-X Plus's menus: GoldenEye's own folder screens, drawn the way GoldenEye's
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
 * - Characters stays grey until its page is built.
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
#include "gexfront.h"
#include "preprocess.h"
#include "game/challenge.h"
#include "game/file.h"
#include "game/gfxmemory.h"
#include "game/lang.h"
#include "game/menu.h"
#include "game/modeldef.h"
#include "game/modelmgr.h"
#include "game/music.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
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
#define SW_BROSNANCOVER 15
#define SW_BLANK        42

// GoldenEye's crosshair image (IMAGE_CROSSHAIR1), 32x32 RGBA32
#define CURSOR_IMAGE 2236
// the film strip's holes (IMAGE_DOT), 16x16 I8
#define DOT_IMAGE 2631
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
#define TITLE_SELECTHANDICAP 86
#define TITLE_SCENARIOHEAD 87
#define TITLE_HEALTH_FIRST 61
#define TITLE_CONTROL_FIRST 277
#define TITLE_SELECTCONTROLSTYLE 285
#define TITLE_CONTROLSTYLE 286

// the text colours: black, and black greyed for a row that is off
#define COLOUR_ON  0x000000ff
#define COLOUR_OFF 0x00000070
// a highlight: black at 50
#define COLOUR_HIGHLIGHT 0x00000032

enum { SCREEN_MODE, SCREEN_MPOPTIONS, SCREEN_LEVEL, SCREEN_SCENARIO, SCREEN_HEALTH, SCREEN_CONTROLSTYLE };

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
#define MAX_FRONT_TEXTURES 32

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

	struct {
		s32 num;
		struct textureconfig config;
	} textures[MAX_FRONT_TEXTURES];
	s32 numtextures;
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

static void frontUnloadModel(void)
{
	if (g_Front.model) {
		modelmgrFreeModel(g_Front.model);
		g_Front.model = NULL;
	}

	if (g_Front.modelbuf) {
		videoFreeCachedTextures(g_Front.modelbuf, g_Front.modelbuf + g_Front.modelbuflen);
		sysMemFree(g_Front.modelbuf);
		g_Front.modelbuf = NULL;
	}

	g_Front.modeldef = NULL;
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

	return 1;
}

static void frontUnload(void)
{
	frontUnloadModel();
	sysMemFree(g_Front.zurich.data);
	sysMemFree(g_Front.gothic.data);
	sysMemFree(g_Front.title);
	g_Front.zurich.data = NULL;
	g_Front.gothic.data = NULL;
	g_Front.title = NULL;
	g_Front.loaded = 0;
}

static s32 frontLoadAll(void)
{
	const s32 first = frontFirstArena();

	if (first < 0) {
		return 0;
	}

	g_Front.moddir = modloaderGetStageModDirIndex(g_MpArenas[first].stagenum);

	if (g_Front.moddir < 0
			|| !frontLoadFont(&g_Front.zurich, "fontzurichbold.bin")
			|| !frontLoadFont(&g_Front.gothic, "fontbankgothic.bin")
			|| !(g_Front.title = frontLoad("LtitleE", &g_Front.titlelen))
			|| !frontLoadModel()) {
		sysLogPrintf(LOG_WARNING, "gexfront: the conversion's menu files are missing; GE-X Plus opens Perfect Dark's menu");
		frontUnload();
		return 0;
	}

	g_Front.loaded = 1;

	return 1;
}

/* ---- the music ---------------------------------------------------------- */

// GoldenEye's M_FOLDERS
#define FOLDERS_SEQUENCE 23

/**
 * The folders theme as a sequence number of the game's, appended once with
 * GoldenEye's instrument bank; -1 when the conversion has no music. What it
 * plays from stays loaded: an appended sequence is never taken back.
 */
static s32 frontMusic(void)
{
	static s32 seqnum = -2;
	u32 ctllen = 0;
	u32 len = 0;
	u32 seqlen = 0;
	u8 *raw;
	u8 *ctl;
	u8 *tbl;
	u8 *seqs;
	ALBank *bank;
	const u8 *e;

	if (seqnum != -2) {
		return seqnum;
	}

	seqnum = -1;

	raw = frontLoad("instrumentsctl", &len);
	ctl = raw ? preprocessALBankFile(raw, len, &ctllen) : NULL;
	sysMemFree(raw);
	tbl = frontLoad("instrumentstbl", &len);
	seqs = frontLoad("sequences", &seqlen);

	if (!ctl || !tbl || !seqs || seqlen < 4 + (FOLDERS_SEQUENCE + 1) * 8
			|| ((seqs[0] << 8) | seqs[1]) <= FOLDERS_SEQUENCE) {
		sysLogPrintf(LOG_WARNING, "gexfront: the conversion has no GoldenEye music; the folder plays the menu's own");
		sysMemFree(ctl);
		sysMemFree(tbl);
		sysMemFree(seqs);
		return -1;
	}

	alBnkfNew((ALBankFile *)ctl, tbl);
	bank = ((ALBankFile *)ctl)->bankArray[0];
	e = seqs + 4 + FOLDERS_SEQUENCE * 8;

	if (bank && be32(e) < seqlen && ((e[6] << 8) | e[7]) <= seqlen - be32(e)) {
		seqnum = seqAppend(seqs + be32(e), (e[4] << 8) | e[5], (e[6] << 8) | e[7], bank);
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
	const s32 num = modBorrowWeaponSets(&first);

	if (num <= 0) {
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
		// GoldenEye's page for this is not built yet
		return 0;
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
 * The setup as GE-X Plus's Combat Simulator has it (mainmenu.c's GE-X Plus
 * row), with the players and simulants of the folder's rows.
 */
static void frontEnterSetup(void)
{
	const s32 first = frontFirstArena();

	mpSetGexPlusMode(true);

	if (first >= 0 && !modloaderStageIsRemake(g_MpSetup.stagenum)) {
		g_MpSetup.stagenum = g_MpArenas[first].stagenum;
	}

	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = -1;
	g_Vars.antiplayernum = -1;
	challengeDetermineUnlockedFeatures();

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
	g_Vars.mpsetupmenu = MPSETUPMENU_GENERAL;
	g_Vars.mpquickteam = g_Vars.mpquickteamnumsims > 0 ? MPQUICKTEAM_PLAYERSANDSIMS : MPQUICKTEAM_PLAYERSONLY;
	mpConfigureQuickTeamPlayers();
	frontApplyAim();

	g_Front.active = 0;
	frontUnload();

	// closes the Perfect Menu; menuTick() starts the match once it has gone
	func0f0f820c(NULL, -5);
}

static void frontClose(void)
{
	g_Front.active = 0;
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
			const f32 cx = ((f32)mx - SCREEN_WIDTH_LO / 2) * (videoGetAspect() / SCREEN_ASPECT) + SCREEN_WIDTH_LO / 2;

			g_Front.cursorx = cx * GEFRONT_W / SCREEN_WIDTH_LO;
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
		menuPlaySound(MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MPOPTIONS;
		return;
	}

	if (pick && g_Front.tabnext) {
		menuPlaySound(MENUSOUND_SWIPE);
		g_Front.levelpage = (g_Front.levelpage + 1) % ((g_Front.numlevels + LEVELS_PER_PAGE - 1) / LEVELS_PER_PAGE);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		menuPlaySound(MENUSOUND_SELECT);
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
		menuPlaySound(MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MPOPTIONS;
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		menuPlaySound(MENUSOUND_SELECT);
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
				menuPlaySound(MENUSOUND_TOGGLEOFF);
			}
		} else if (left || right) {
			if (g_Front.screen == SCREEN_HEALTH) {
				const s32 next = g_Front.handicap[i] + (right ? 1 : -1);

				if (next >= 0 && next < NUM_HANDICAPS) {
					g_Front.handicap[i] = next;
					g_PlayerConfigsArray[i].handicap = frontHandicapValue(next);
					menuPlaySound(MENUSOUND_SUBFOCUS);
				}
			} else {
				const s32 next = optionsGetControlMode(i) + (right ? 1 : -1);

				if (next >= 0 && next < NUM_CONTROLSTYLES) {
					frontSetControlStyle(i, next);
					menuPlaySound(MENUSOUND_SUBFOCUS);
				}
			}
		} else if (pick) {
			g_Front.chosen[i] = 1;
			menuPlaySound(MENUSOUND_SELECT);
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
	g_Front.tabstart = g_Front.screen == SCREEN_MPOPTIONS && !g_Front.tabprev && frontOnStartTab();
	g_Front.tabnext = g_Front.screen == SCREEN_LEVEL && g_Front.numlevels > LEVELS_PER_PAGE && frontOnNextTab();
	g_Front.highlight = -1;

	switch (g_Front.screen) {
	case SCREEN_LEVEL:
		frontTickLevel(pick, back);
		return;
	case SCREEN_SCENARIO:
		frontTickScenario(pick, back);
		return;
	case SCREEN_HEALTH:
	case SCREEN_CONTROLSTYLE:
		frontTickPlayerPanels();
		return;
	}

	if (g_Front.screen == SCREEN_MODE) {
		// interface_menu06_modesel(): below 243 is SELECT MISSION, which has no missions to open
		if (!g_Front.tabprev) {
			g_Front.highlight = g_Front.cursory >= 243.0f ? 1 : 0;
		}

		if (back || (pick && g_Front.tabprev)) {
			menuPlaySound(MENUSOUND_TOGGLEOFF);
			frontClose();
			return;
		}

		if (pick && g_Front.highlight == 1) {
			menuPlaySound(MENUSOUND_SELECT);
			frontEnterSetup();
			g_Front.screen = SCREEN_MPOPTIONS;
		} else if (pick) {
			menuPlaySound(MENUSOUND_ERROR);
		}
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
		menuPlaySound(MENUSOUND_SELECT);
		frontStartMatch();
		return;
	}

	if (back || (pick && g_Front.tabprev)) {
		menuPlaySound(MENUSOUND_TOGGLEOFF);
		g_Front.screen = SCREEN_MODE;
		frontSetCursorForMode(1);
		return;
	}

	if (pick && g_Front.highlight >= 0) {
		if (frontRowOn(g_Front.highlight)) {
			menuPlaySound(MENUSOUND_FOCUS);
			frontSelectRow(g_Front.highlight);
		} else {
			menuPlaySound(MENUSOUND_ERROR);
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
	g_Front.screen = SCREEN_MODE;
	g_Front.mouseseen = 0;

	if (frontMusic() >= 0) {
		musicStartTrackAsMenu(frontMusic());
	}

	g_Front.inputdelay = 2;
	g_Front.highlight = -1;
	frontSetCursorForMode(1);

	return 1;
}

/* ---- drawing ------------------------------------------------------------ */

static f32 frontScaleX(void)
{
	return viGetWidth() / GEFRONT_W;
}

static f32 frontScaleY(void)
{
	return viGetHeight() / GEFRONT_H;
}

static Gfx *frontFillRect(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, u32 colour)
{
	// microcode_constructor_related_to_menus()
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetPrimColor(gdl++, 0, 0, colour >> 24, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);
	gDPFillRectangle(gdl++, (s32)(x1 * frontScaleX()), (s32)(y1 * frontScaleY()), (s32)(x2 * frontScaleX()), (s32)(y2 * frontScaleY()));

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
					(s32)(((*y - cur->baseline) - cur->height) * sx * 4),
					(s32)(*x * sy * 4),
					(s32)((*y - cur->baseline) * sx * 4),
					(s32)((*x + cur->width) * sy * 4),
					G_TX_RENDERTILE, 0, (cur->height - 1) << 5,
					(s32)(1024 / sy), (s32)(-1024 / sx));
		} else {
			gSPTextureRectangle(gdl++,
					(s32)(*x * sx * 4),
					(s32)((*y + cur->baseline) * sy * 4),
					(s32)((*x + cur->width) * sx * 4),
					(s32)((*y + cur->baseline + cur->height) * sy * 4),
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

static Gfx *frontDrawCursor(Gfx *gdl)
{
	const f32 sx = frontScaleX();
	const f32 sy = frontScaleY();
	const f32 x = (s32)(g_Front.cursorx + 0.5f);
	const f32 y = (s32)(g_Front.cursory + 0.5f);
	const s32 prevsrc = modSetTextureSourceMod(g_Front.moddir);

	texSelect(&gdl, &g_Front.cursor, 4, 0, 2, 1, NULL);
	modSetTextureSourceMod(prevsrc);

	// display_image_at_position(): white, 220 of 255, the image's middle on the cursor
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetEnvColor(gdl++, 255, 255, 255, 220);
	gDPSetCombineLERP(gdl++, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0);
	gSPTextureRectangle(gdl++,
			(s32)((x - 16) * sx * 4), (s32)((y - 16) * sy * 4),
			(s32)((x + 16) * sx * 4), (s32)((y + 16) * sy * 4),
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
 * display_image_at_position() and draw_textured_rectangle(): a texture over a
 * rectangle (its middle and half size), twidth and theight texels across it,
 * tinted by the colour; opaque, as GoldenEye draws its stage pictures.
 */
static Gfx *frontImage(Gfx *gdl, s32 num, s32 width, s32 height, s32 format, s32 wrap,
		f32 cx, f32 cy, f32 hw, f32 hh, s32 twidth, s32 theight, u32 colour)
{
	const f32 sx = frontScaleX();
	const f32 sy = frontScaleY();
	struct textureconfig *tex = frontTexture(num, width, height, format, G_IM_SIZ_8b, wrap);
	s32 prevsrc;

	if (!tex) {
		return gdl;
	}

	prevsrc = modSetTextureSourceMod(g_Front.moddir);
	texSelect(&gdl, tex, 1, 0, 2, 1, NULL);
	modSetTextureSourceMod(prevsrc);

	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetTextureFilter(gdl++, G_TF_POINT);
	gDPSetEnvColor(gdl++, colour >> 24, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);
	gDPSetCombineLERP(gdl++, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0);
	gSPTextureRectangle(gdl++,
			(s32)((cx - hw) * sx * 4), (s32)((cy - hh) * sy * 4),
			(s32)((cx + hw) * sx * 4), (s32)((cy + hh) * sy * 4),
			G_TX_RENDERTILE, 0, 0,
			(s32)(twidth / (2.0f * hw) * 1024.0f / sx), (s32)(theight / (2.0f * hh) * 1024.0f / sy));

	return gdl;
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

	guPerspectiveF(persp.m, &perspnorm, 60.0f, videoGetAspect(), 100.0f, 10000.0f, 1.0f);
	guMtxF2L(persp.m, projection);

	gSPViewport(gdl++, &vp);
	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);
	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	// folderpositions[0] and D_8002AFC4..CC
	mtx00016ae4(&camera, -900.0f, 990.0f, 700.0f, -900.0f, 990.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	mtx4LoadIdentity(&world);
	world.m[3][0] = -900.0f;
	world.m[3][1] = 800.0f;
	mtx00015f04(0.25f, &world);
	mtx4MultMtx4InPlace(&camera, &world);

	renderdata.unk00 = &world;
	renderdata.unk10 = gfxAllocate(g_Front.modeldef->nummatrices * sizeof(Mtxf));
	mtx4Copy(&world, renderdata.unk10);
	g_Front.model->matrices = renderdata.unk10;

	modelUpdateRelations(g_Front.model);

	renderdata.flags = 3;
	renderdata.zbufferenabled = false;
	renderdata.gdl = gdl;

	modelRender(&renderdata, g_Front.model);

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

	text = frontString(TITLE_SELECTMISSION);
	frontMeasure(&g_Front.zurich, text, 0, &w, &h);
	gdl = frontPrint(gdl, 0x96, 0xdc, "1.\n", COLOUR_OFF);

	if (g_Front.highlight == 0) {
		gdl = frontFillRect(gdl, 0x94, 0xda, w + 0xaf, 0xea, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	gdl = frontPrint(gdl, 0xaa, 0xdc, text, COLOUR_OFF);

	text = frontString(TITLE_MULTIPLAYER);
	frontMeasure(&g_Front.zurich, text, 0, &w, &h);
	gdl = frontPrint(gdl, 0x96, 0xfc, "2.\n", COLOUR_ON);

	if (g_Front.highlight == 1) {
		gdl = frontFillRect(gdl, 0x94, 0xfa, w + 0xaf, 0x10a, COLOUR_HIGHLIGHT);
		gdl = frontTextSetup(gdl);
	}

	gdl = frontPrint(gdl, 0xaa, 0xfc, text, COLOUR_ON);

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
		gdl = frontImage(gdl, DOT_IMAGE, 16, 16, G_IM_FMT_I, true, 213, 104 + 70 * i, 176, 4, 0x2f0, 0x12, 0x6b6753ff);
		gdl = frontImage(gdl, DOT_IMAGE, 16, 16, G_IM_FMT_I, true, 213, 164 + 70 * i, 176, 4, 0x2f0, 0x12, 0x6b6753ff);
	}

	for (s32 n = 0; n < LEVELS_PER_PAGE && first + n < g_Front.numlevels; n++) {
		const s32 row = n / 4;
		const s32 col = n % 4;
		// the highlighted picture as GoldenEye brightens it, the rest dimmed
		const u32 colour = n == g_Front.highlight ? 0xffffffff : 0x6e6e6eff;

		gdl = frontImage(gdl, frontStageImage(g_Front.levels[first + n]), STAGE_IMAGE_W, STAGE_IMAGE_H, G_IM_FMT_I, false,
				86 + 85 * col, 134 + 70 * row, 34, 22, STAGE_IMAGE_W, STAGE_IMAGE_H, colour);
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

		if (numplayers == 1) {
			left = 0x26;
			width = 0x15e;
			top = 0x1e + 0x46;
		} else if (numplayers == 2) {
			left = 0x26;
			width = 0x15e;
			top = (i > 0 ? 0x8c : 0) + 0x1e;
		} else {
			width = 0xaf;
			top = (i >= 2 ? 0x8c : 0) + 0x1e;
			left = ((i & 1) ? 0xaf : 0) + 0x26;
		}

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
	case SCREEN_SCENARIO:
		gdl = frontDrawScenario(gdl);
		break;
	default:
		gdl = frontDrawPlayerPanels(gdl);
		break;
	}

	// the per-player pages have no tabs and no cursor, as GoldenEye's have none
	if (g_Front.screen != SCREEN_HEALTH && g_Front.screen != SCREEN_CONTROLSTYLE) {
		gdl = frontTab(gdl, TITLE_PREVIOUS, PREVTAB_TEXT_TOP, PREVTAB_TEXT_BOTTOM, g_Front.tabprev);
		gdl = frontDrawCursor(gdl);
	}

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	return gdl;
}

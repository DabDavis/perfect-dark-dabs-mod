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
 * GoldenEye has two screens on the way to a match, and so does this:
 * - the mode select: 1. SELECT MISSION (off: the remake has no missions yet) and
 *   2. MULTIPLAYER, with the PREVIOUS tab back to the Perfect Menu;
 * - MULTIPLAYER OPTIONS, GoldenEye's rows with two of the remake's own after
 *   Players - Simulants and their difficulty - since GoldenEye has none, and the
 *   START tab to the match.
 * Only the rows GoldenEye changes in place are live; its Level, Scenario,
 * Characters, Health and Control Style pages are cycled in place or not yet
 * offered (grey), until those pages are built.
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
#define SW_PHOTOBOND    7
#define SW_BROSNAN      8
#define SW_BROSNANCOVER 16

// GoldenEye's crosshair image (IMAGE_CROSSHAIR1), 32x32 RGBA32
#define CURSOR_IMAGE 2236

// front.h's tabs
#define TABS_LEFT_EDGE 390.0f
#define TABS_RIGHT_EDGE 411
#define STARTTAB_TEXT_TOP 51
#define STARTTAB_TEXT_BOTTOM 117
#define STARTTAB_TAB_BOTTOM 130.5f
#define PREVTAB_TAB_TOP 223.0f
#define PREVTAB_TEXT_TOP 236
#define PREVTAB_TEXT_BOTTOM 302

// LtitleE's strings GoldenEye's two screens use
#define TITLE_START        4
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
#define TITLE_CONTROLSTYLE 286

// the text colours: black, and black greyed for a row that is off
#define COLOUR_ON  0x000000ff
#define COLOUR_OFF 0x00000070
// a highlight: black at 50
#define COLOUR_HIGHLIGHT 0x00000032

enum { SCREEN_MODE, SCREEN_MPOPTIONS };

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
	s32 highlight;      // the row (or mode) under the cursor, -1 for none
	s32 tabprev;
	s32 tabstart;

	s32 gamelength;     // GoldenEye's multi_game_lengths index
	s32 aim;            // GoldenEye's mp_sight_adjust_table index
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

/** The arena after the chosen one among the remake's, wrapping. */
static void frontNextArena(void)
{
	const s32 num = mpGetNumStages();
	s32 cur = -1;

	for (s32 i = 0; i < num; i++) {
		if (g_MpArenas[i].stagenum == g_MpSetup.stagenum) {
			cur = i;
		}
	}

	for (s32 n = 1; n <= num; n++) {
		const s32 i = (cur + n + num) % num;

		if (modloaderStageIsRemake(g_MpArenas[i].stagenum)) {
			g_MpSetup.stagenum = g_MpArenas[i].stagenum;
			return;
		}
	}
}

static const char *frontArenaName(void)
{
	for (s32 i = 0; i < mpGetNumStages(); i++) {
		if (g_MpArenas[i].stagenum == g_MpSetup.stagenum) {
			const char *name = modloaderGetStageMapName(g_MpArenas[i].stagenum);

			return name ? name : mpGetArenaName(i);
		}
	}

	return "";
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
	case ROW_CHARACTERS:
	case ROW_HEALTH:
	case ROW_CONTROLSTYLE:
		// GoldenEye's pages for these are not built yet
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
		{
			s32 cur = 0;

			for (s32 i = 0; i < ARRAYCOUNT(g_FrontScenarios); i++) {
				if (g_FrontScenarios[i] == gexPlusGetScenario()) {
					cur = i;
				}
			}

			gexPlusSetScenario(g_FrontScenarios[(cur + 1) % ARRAYCOUNT(g_FrontScenarios)]);
			frontApplyScenarioRules();
		}
		break;
	case ROW_LEVEL:
		frontNextArena();
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

	// the mouse, where it is, in the 4:3 frame the folder is drawn in
	if (inputMouseIsEnabled() && !inputMouseIsLocked() && inputMouseGetPosition(&mx, &my)) {
		const f32 cx = ((f32)mx - SCREEN_WIDTH_LO / 2) * (videoGetAspect() / SCREEN_ASPECT) + SCREEN_WIDTH_LO / 2;

		g_Front.cursorx = cx * GEFRONT_W / SCREEN_WIDTH_LO;
		g_Front.cursory = (f32)my * GEFRONT_H / SCREEN_HEIGHT_LO;
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
	g_Front.highlight = -1;

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

	g_Front.active = 1;
	g_Front.screen = SCREEN_MODE;

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

	frontSetSwitch(SW_TABS, true);
	frontSetSwitch(SW_PAPER, true);
	frontSetSwitch(SW_OHMSS, true);

	if (g_Front.screen == SCREEN_MODE) {
		frontSetSwitch(SW_PHOTOBOND, true);
		frontSetSwitch(SW_EYESONLY, true);
		frontSetSwitch(SW_BROSNAN, true);
		frontSetSwitch(SW_BROSNANCOVER, true);
	} else {
		frontSetSwitch(SW_CONFIDENTIAL2, true);
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
		return frontArenaName();
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

	if (g_Front.screen == SCREEN_MODE) {
		gdl = frontDrawMode(gdl);
	} else {
		gdl = frontDrawMpOptions(gdl);
		gdl = frontTab(gdl, TITLE_START, STARTTAB_TEXT_TOP, STARTTAB_TEXT_BOTTOM, g_Front.tabstart);
		gdl = frontTextSetup(gdl);
	}

	gdl = frontTab(gdl, TITLE_PREVIOUS, PREVTAB_TEXT_TOP, PREVTAB_TEXT_BOTTOM, g_Front.tabprev);
	gdl = frontDrawCursor(gdl);

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	return gdl;
}

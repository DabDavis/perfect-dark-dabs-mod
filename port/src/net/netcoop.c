#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "modloader.h"
#include "gexplusrom.h"
#include "gexfront.h"
#include "gexplus.h"
#include "geconvert.h"
#include "game/bg.h"
#include "game/chraction.h"
#include "game/coop.h"
#include "game/lv.h"
#include "game/bondmove.h"
#include "game/setuputils.h"
#include "game/lang.h"
#include "game/menu.h"
#include "game/objectives.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/pdmode.h"
#include "game/propobj.h"
#include "game/title.h"
#include "game/mplayer/mplayer.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "math.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "netint.h"

/**
 * Online co-op: the solo missions for up to twelve players
 * (PLANS/netplay/spec-coop.md).
 *
 * The host plays a mission as offline co-op does, bond (slot 0) and every
 * other seat a coop player (src/game/coop.c widens "the other player" to
 * many). A client's world is the host's as in a Combat Simulator match:
 * every chr is a puppet (the setup's spawned here too and posed by
 * SETUPCHR, netents.c), and what the mission itself keeps outside the
 * props comes in the SNAP scenario block (scenario 0xfe, the layout below,
 * netscen.c hands it here): the tick mode and the cutscene, the objectives'
 * statuses, the countdown timer, the alarm, each player's dead/aborted
 * bits. The mission ends on the host alone (MATCH_END carries the final
 * block); a client's Abort leaves.
 *
 * Nothing here touches offline progress: no coop completion bit, no best
 * time, no game file save (menutick.c, endscreen.c), every mission open.
 *
 * Block body (at netscen.c's OFF_BODY, little-endian):
 *     0  u8  tickmode             g_Vars.tickmode
 *     1  u8  flags                1 in_cutscene, 2 countdown running,
 *                                 4 objective checks disabled, 8 GoldenEye's
 *                                 end waits for a press, 16 and fades out
 *                                 (gexplus.c; protocol 16)
 *     2  s16 cutsceneanim         g_CutsceneAnimNum
 *     4  s32 cutsceneframe240     g_CutsceneCurAnimFrame240
 *     8  s16 tween60              g_CutsceneTweenDuration60
 *    10  s16 alarmtimer           g_AlarmTimer (clamped)
 *    12  f32 countdown60          g_CountdownTimerValue60
 *    16  u8  objectives[10]       objectiveCheck(i): 0 complete, 1 incomplete, 2 failed
 *    26  u8  players[12]          bit 0 isdead, 1 aborted, 2 coopcanrestart
 *    38  s16 warpcmd              the setup command a GoldenEye CameraSwitch
 *                                 (ai00df) put the camera at, -1 none
 *                                 (protocol 17)
 *    40  s16 warpdir              its direction word (g_WarpType2HasDirection)
 *    42  u8  fadeseq              counts the screen fades a converted
 *                                 mission's lists asked for (aiFadeScreen)
 *    44  u32 fadecolour           the last one's colour
 *    48  s16 fadeframes           and length
 */

#define MIS_TICKMODE  0
#define MIS_FLAGS     1
#define MIS_CUTANIM   2
#define MIS_CUTFRAME  4
#define MIS_TWEEN     8
#define MIS_ALARM     10
#define MIS_COUNTDOWN 12
#define MIS_OBJ       16
#define MIS_PLAYERS   (MIS_OBJ + MAX_OBJECTIVES)
#define MIS_WARPCMD   (MIS_PLAYERS + MAX_PLAYERS)
#define MIS_WARPDIR   (MIS_WARPCMD + 2)
#define MIS_FADESEQ   (MIS_WARPDIR + 2)
#define MIS_FADECOL   (MIS_FADESEQ + 2)
#define MIS_FADELEN   (MIS_FADECOL + 4)
#define MIS_SIZE      (MIS_FADELEN + 2)

#define MISF_INCUTSCENE 0x01
#define MISF_COUNTDOWN  0x02
#define MISF_NOCHECKS   0x04
#define MISF_GEEXIT     0x08
#define MISF_GEFADE     0x10

#define MISP_DEAD       0x01
#define MISP_ABORTED    0x02
#define MISP_CANRESTART 0x04

// player.c's (PC builds are NTSC: s32; no header declares them)
extern s32 g_CutsceneCurAnimFrame240;
extern s32 g_CutsceneTweenDuration60;
extern s16 g_WarpType1Pad;
extern struct warpparams *g_WarpType2Params;
extern s32 g_WarpType2HasDirection;

struct netcoopsetup g_NetCoopSetup = { 0, 0, DIFF_A, 1, 0 };

static s32 s_HostMatch = 0;       // host: the match H1 is starting (or running) is a co-op mission
static u32 s_JoinNear = 0;        // host: players a join in progress took, whose next spawn is beside a living one
static char s_HostGame[16];       // host: the mission's set ("" Perfect Dark's, else a conversion's tag)
static const char *s_HostVariantBefore; // host: g_GexPlusVariant before a conversion's mission, put back after

// a campaign room: the host plays its set's missions from its own menus
static struct {
	s32 on;
	char game[16];
	s32 radar;
	s32 friendlyfire;
} s_Campaign;

extern struct menudialogdef g_CiMenuViaPcMenuDialog;
static s32 s_ClientMatch = 0;     // client: RULES said a mission
static u8 s_ObjStatus[MAX_OBJECTIVES];
static s32 s_HaveObj = 0;
static u32 s_BlocksApplied = 0;
static u32 s_CutStarts = 0;
static u32 s_CutEnds = 0;
static s32 s_LoggedStart = 0;

// a converted mission's camera and fades, as the host's lists set them
static s32 s_HostWarpCmd = -1;                // host: the setup command the last CameraSwitch named
static struct warpparams *s_HostWarpParams;   // host: and its record, while the warp is still that one
static u8 s_HostFadeSeq = 0;                  // host: counts aiFadeScreen
static u32 s_HostFadeColour = 0;
static s16 s_HostFadeFrames = 0;
static s32 s_WarpCmd = -1;                    // client: the host's shot this machine is in, -1 none
static s32 s_HaveFadeSeq = 0;                 // client: a fade count seen (the first is only noted)
static u8 s_FadeSeq = 0;
static u32 s_WarpFollows = 0;

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static s16 gets16(const u8 *p) { return (s16)get16(p); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u32 get32(const u8 *p) { return get16(p) | ((u32)get16(p + 2) << 16); }
static void putf(u8 *p, f32 f) { u32 u; memcpy(&u, &f, 4); put32(p, u); }
static f32 getf(const u8 *p) { u32 u = get32(p); f32 f; memcpy(&f, &u, 4); return f; }

// a language string's text without its newline (the log lines)
static const char *netCoopClean(const char *in, char *out, s32 size)
{
	s32 i = 0;

	while (in && *in && *in != '\n' && i + 1 < size) {
		out[i++] = *in++;
	}

	out[i] = '\0';
	return out;
}

/*
 * The rules
 */

s32 netCoopHostMatch(void)
{
	return s_HostMatch;
}

const char *netCoopHostGame(void)
{
	return s_HostGame;
}

/*
 * The mission sets (protocol 14): Perfect Dark's own twenty, GoldenEye's
 * converted from the ROM, and each ROM hack's. A set is named by its
 * conversion's tag ("ge", "gf64", "tnd64"; "" Perfect Dark's), which both
 * machines resolve in their own conversions: the missions of a set are the
 * mod loader's for that conversion whatever Combat Simulator mode is chosen
 * (modloaderMissionStageOf).
 */

/**
 * The g_GexPlusVariant a set plays under (NULL for Perfect Dark's and
 * GoldenEye's own, the hack's converted name for its), in *variant; 0 when
 * the tag names a hack not converted here
 */
static s32 netCoopGameVariant(const char *game, const char **variant)
{
	*variant = NULL;

	if (!game || !game[0] || strcasecmp(game, geconvertGoldenEyeTag()) == 0) {
		return 1;
	}

	// the hack converted here, or its folder as the host served it (netcontent.c)
	*variant = netContentVariantName(game);

	return *variant != NULL;
}

static s32 netCoopIsPdGame(const char *game)
{
	return !game || !game[0];
}

/** The stage a set's mission runs here, 0 when the set or the mission is not here */
static s32 netCoopMissionStage(const char *game, s32 index)
{
	const char *variant;

	if (netCoopIsPdGame(game)) {
		return index >= 0 && index <= SOLOSTAGEINDEX_WAR ? g_SoloStages[index].stagenum : 0;
	}

	if (!netCoopGameVariant(game, &variant)) {
		return 0;
	}

	return modloaderMissionStageOf(variant, index);
}

const char *netCoopGameName(const char *game)
{
	const char *dir;

	if (netCoopIsPdGame(game)) {
		return "Perfect Dark";
	}

	if (strcasecmp(game, geconvertGoldenEyeTag()) == 0) {
		return "GoldenEye";
	}

	dir = gexPlusRomDirOfTag(game);

	return dir ? dir : game;
}

s32 netCoopGameMissions(const char *game)
{
	const char *variant;

	if (netCoopIsPdGame(game)) {
		return SOLOSTAGEINDEX_WAR + 1;
	}

	if (!netCoopGameVariant(game, &variant)) {
		return 0;
	}

	return modloaderNumMissionsOf(variant);
}

/**
 * The sets a room here can offer, in order: Perfect Dark's, then GoldenEye's
 * and each hack's that has a mission converted. The n-th into tag; 0 past
 * the end.
 */
s32 netCoopGameTag(s32 n, char *tag, s32 size)
{
	s32 k = 0;
	s32 i;

	if (n == 0) {
		tag[0] = '\0';
		return 1;
	}

	if (netCoopGameMissions(geconvertGoldenEyeTag()) > 0 && ++k == n) {
		snprintf(tag, size, "%s", geconvertGoldenEyeTag());
		return 1;
	}

	for (i = 0; geconvertVariantTagAt(i); i++) {
		if (netCoopGameMissions(geconvertVariantTagAt(i)) > 0 && ++k == n) {
			snprintf(tag, size, "%s", geconvertVariantTagAt(i));
			return 1;
		}
	}

	return 0;
}

const char *netCoopMissionNameOf(const char *game, s32 index)
{
	const s32 stage = netCoopMissionStage(game, index);
	const char *name;

	if (netCoopIsPdGame(game)) {
		return netCoopMissionName(index);
	}

	name = stage > 0 ? modloaderGetStageMapName(stage) : NULL;

	return name && name[0] ? name : "?";
}

s32 netCoopRulesOk(const struct netcooprules *r)
{
	if (r->difficulty > DIFF_PA) {
		return 0;
	}

	if (netCoopIsPdGame(r->game)) {
		return r->stageindex <= SOLOSTAGEINDEX_WAR;
	}

	return r->stageindex < MODLOADER_MAX_MISSIONS;
}

// H3, from netRulesApply on a client: the host's mission
void netCoopClientApplyRules(const struct netcooprules *r)
{
	const char *variant = NULL;

	// a conversion's mission plays under its set's mode (a hack's name, or
	// none for GoldenEye's own): the mission systems key off the stage,
	// the list it is looked up in off this (netRulesRestore puts it back)
	if (!netCoopIsPdGame(r->game)) {
		if (!netCoopGameVariant(r->game, &variant)) {
			sysLogPrintf(LOG_WARNING, "net: co-op: the host's mission set \"%s\" is not converted here", r->game);
		}

		g_GexPlusVariant = variant;
	}

	g_MissionConfig.iscoop = true;
	g_MissionConfig.isanti = false;
	g_MissionConfig.pdmode = false;
	g_MissionConfig.stageindex = r->stageindex;
	g_MissionConfig.stagenum = netCoopMissionStage(r->game, r->stageindex);
	g_MissionConfig.difficulty = r->difficulty;
	g_Vars.coopradaron = r->radar ? true : false;
	g_Vars.coopfriendlyfire = r->friendlyfire ? true : false;
	g_Vars.numaibuddies = 0;
	s_ClientMatch = 1;

	{
		char name[48];

		sysLogPrintf(LOG_NOTE, "net: co-op: the host's mission is %s (%s, index %d, stage 0x%02x) on difficulty %d, radar %d, friendly fire %d",
				netCoopClean(netCoopMissionNameOf(r->game, r->stageindex), name, sizeof(name)), netCoopGameName(r->game), r->stageindex,
				g_MissionConfig.stagenum, r->difficulty, r->radar, r->friendlyfire);
	}
}

/**
 * H3's tail on a client: the mission's players, as Accept Mission sets them
 * (the stage change and player count are the session's, as for a match)
 */
void netCoopClientStage(void)
{
	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = 1;
	g_Vars.antiplayernum = -1;
	g_Vars.numaibuddies = 0;
	g_Vars.perfectbuddynum = 0;
	lvSetDifficulty(g_MissionConfig.difficulty);
	viBlack(true);
	s_HaveObj = 0;
	s_BlocksApplied = 0;
	s_CutStarts = 0;
	s_CutEnds = 0;
	s_LoggedStart = 0;
	s_WarpCmd = -1;
	s_HaveFadeSeq = 0;
	s_WarpFollows = 0;
}

/*
 * Starting one (host)
 */

static const char *netCoopDifficultyName(s32 difficulty)
{
	switch (difficulty) {
	case DIFF_SA: return langGet(L_OPTIONS_252); // "Special Agent"
	case DIFF_PA: return langGet(L_OPTIONS_253); // "Perfect Agent"
	default:      return langGet(L_OPTIONS_251); // "Agent"
	}
}


const char *netCoopMissionName(s32 stageindex)
{
	if (stageindex < 0 || stageindex > SOLOSTAGEINDEX_WAR) {
		return "?";
	}

	return langGet(g_SoloStages[stageindex].name3);
}

// "Co-op Agent" and the like, for the lobby's MODE column (no newline)
void netCoopModeName(s32 difficulty, char *out, s32 size)
{
	char name[32];

	snprintf(out, size, "Co-op %s", netCoopClean(netCoopDifficultyName(difficulty), name, sizeof(name)));
}

/**
 * The host starts a co-op mission for the session (a lobby room's launch,
 * the Carrington Institute's Accept Mission in a --host session, a gate's
 * --net-test-coop): what menuhandlerAcceptMission does for a human buddy,
 * with the session's seats as the players (H1) and the mission in RULES.
 * Returns 0 and a notice when it cannot.
 */
s32 netCoopHostStart(const char *game, s32 stageindex, s32 difficulty, s32 radar, s32 friendlyfire)
{
	const char *variant = NULL;
	s32 numplayers;
	s32 stagenum;

	if (g_NetMode != NETMODE_SERVER) {
		return 0;
	}

	if (!game) {
		game = "";
	}

	stagenum = netCoopMissionStage(game, stageindex);

	if (stagenum <= 0 || difficulty < DIFF_A || difficulty > DIFF_PA || !netCoopGameVariant(game, &variant)) {
		sysLogPrintf(LOG_WARNING, "net: co-op: mission %d of %s / difficulty %d cannot be played online (stage 0x%02x)",
				stageindex, netCoopGameName(game), difficulty, stagenum);
		return 0;
	}

	snprintf(s_HostGame, sizeof(s_HostGame), "%s", game);

	// a conversion's mission plays under its set's mode (netCoopClientApplyRules)
	if (!netCoopIsPdGame(game)) {
		s_HostVariantBefore = g_GexPlusVariant;
		g_GexPlusVariant = variant;
	}

	g_MissionConfig.iscoop = true;
	g_MissionConfig.isanti = false;
	// a campaign's host keeps what its folder set (GoldenEye's 007 is
	// Perfect Dark's PD Mode with its sliders): the host runs the simulation
	if (!s_Campaign.on) {
		g_MissionConfig.pdmode = false;
	}
	g_MissionConfig.stageindex = stageindex;
	g_MissionConfig.stagenum = stagenum;
	g_MissionConfig.difficulty = difficulty;
	g_Vars.coopradaron = radar ? true : false;
	g_Vars.coopfriendlyfire = friendlyfire ? true : false;
	g_Vars.numaibuddies = 0;
	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = 1;
	g_Vars.antiplayernum = -1;
	g_Vars.perfectbuddynum = 0;

	if (g_Vars.stagenum == stagenum) {
		g_Vars.restartlevel = true;
	}

	menuStop();
	titleSetNextStage(stagenum);

	s_HostMatch = 1;
	s_JoinNear = 0;
	numplayers = netHostMatchStarting(stagenum, 2);

	if (numplayers < 1) {
		numplayers = 1;
	}

	setNumPlayers(numplayers);
	lvSetDifficulty(difficulty);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(stagenum);
	viBlack(true);

	{
		char name[48];
		char diff[32];

		sysLogPrintf(LOG_NOTE, "net: co-op: starting %s (%s, index %d, stage 0x%02x) on %s with %d players, radar %d, friendly fire %d",
				netCoopClean(netCoopMissionNameOf(game, stageindex), name, sizeof(name)), netCoopGameName(game), stageindex, stagenum,
				netCoopClean(netCoopDifficultyName(difficulty), diff, sizeof(diff)), numplayers, radar, friendlyfire);
	}

	return 1;
}

/**
 * mainmenu.c Accept Mission in a net session: 1 when handled here. A client
 * never starts one; the host starts co-op with a human buddy as the
 * session's mission, and is told why anything else cannot be played online
 * (counter-op, AI buddies, solo, GoldenEye's missions).
 */
s32 netCoopAcceptMission(void)
{
	s32 stageindex;

	if (g_NetMode == NETMODE_CLIENT) {
		netClientRefuseLocalStart();
		return 1;
	}

	if (g_NetMode != NETMODE_SERVER) {
		return 0;
	}

	stageindex = g_MissionConfig.stageindex;

	// a campaign room: whatever mission the host's menus start - Perfect
	// Dark's briefing (solo or co-op), GoldenEye's folder, a ROM hack's - is
	// the session's next co-op match, with the seats as the players
	if (s_Campaign.on) {
		const char *game = "";
		const char *dir = modloaderStageIsMission(g_MissionConfig.stagenum) ? modloaderGetStageModDir(g_MissionConfig.stagenum) : NULL;
		const char *tag = dir ? gexPlusRomDirTag(dir) : NULL;

		if (tag) {
			game = tag;
		} else if (dir || stageindex > SOLOSTAGEINDEX_WAR || g_SoloStages[stageindex].stagenum != g_MissionConfig.stagenum) {
			netSessionNoticeSet("This mission cannot be played online.");
			sysLogPrintf(LOG_NOTE, "net: co-op: campaign: refused to start stage 0x%02x (index %d) online", g_MissionConfig.stagenum, stageindex);
			return 1;
		}

		if (!netCoopHostStart(game, stageindex, g_MissionConfig.difficulty, s_Campaign.radar, s_Campaign.friendlyfire)) {
			netSessionNoticeSet("This mission cannot be played online.");
		}

		return 1;
	}

	if (!g_MissionConfig.iscoop || g_MissionConfig.isanti || g_Vars.numaibuddies > 0
			|| stageindex > SOLOSTAGEINDEX_WAR || g_SoloStages[stageindex].stagenum != g_MissionConfig.stagenum
			|| modloaderStageIsRemake(g_MissionConfig.stagenum)) {
		netSessionNoticeSet("Online, a mission is played Co-Operative with a human buddy.");
		sysLogPrintf(LOG_NOTE, "net: co-op: refused to start mission 0x%02x online (coop %d, anti %d, AI buddies %d)",
				g_MissionConfig.stagenum, g_MissionConfig.iscoop, g_MissionConfig.isanti, g_Vars.numaibuddies);
		return 1;
	}

	netCoopHostStart("", stageindex, g_MissionConfig.difficulty, g_Vars.coopradaron, g_Vars.coopfriendlyfire);
	return 1;
}

/*
 * A campaign room: the host plays its set's missions from its own menus
 * (the user, 2026-10-07: "start the whole mode ... host controls
 * everything, and game is seamless like offline"). The room's launch opens
 * the host's menus for the set rather than a match; every mission the host
 * starts there is the session's next co-op match (netCoopAcceptMission),
 * and after it the host is back in those menus while the guests are back
 * in the room, pulled into the next mission by its STAGE_LOAD.
 */
s32 netCoopCampaignOn(void)
{
	return s_Campaign.on;
}

void netCoopCampaignEnd(void)
{
	if (s_Campaign.on) {
		sysLogPrintf(LOG_NOTE, "net: co-op: the %s campaign is over", netCoopGameName(s_Campaign.game));
	}

	memset(&s_Campaign, 0, sizeof(s_Campaign));
}

// the host's menus for the set: the Perfect Menu, or the set's folder over it
static void netCoopCampaignMenus(s32 afterMission)
{
	const char *variant = NULL;

	menuPushRootDialog(&g_CiMenuViaPcMenuDialog, MENUROOT_MAINMENU);

	if (netCoopIsPdGame(s_Campaign.game)) {
		return;
	}

	netCoopGameVariant(s_Campaign.game, &variant);
	g_GexPlusVariant = variant;

	if (afterMission) {
		gexFrontOpenAfterMission();
	} else if (!gexFrontOpen()) {
		sysLogPrintf(LOG_WARNING, "net: co-op: campaign: the %s folder did not open", netCoopGameName(s_Campaign.game));
	}
}

/** The room's launch: the campaign begins in the host's menus (no match yet) */
s32 netCoopCampaignOpen(const char *game, s32 radar, s32 friendlyfire)
{
	const char *variant;

	if (g_NetMode != NETMODE_SERVER || !netCoopGameVariant(game ? game : "", &variant)) {
		return 0;
	}

	memset(&s_Campaign, 0, sizeof(s_Campaign));
	s_Campaign.on = 1;
	s_Campaign.radar = radar;
	s_Campaign.friendlyfire = friendlyfire;
	snprintf(s_Campaign.game, sizeof(s_Campaign.game), "%s", game ? game : "");
	sysLogPrintf(LOG_NOTE, "net: co-op: the %s campaign begins: the host picks the missions in its menus", netCoopGameName(s_Campaign.game));
	netCoopCampaignMenus(0);

	return 1;
}

/** The Game Lobby's row on a campaign host: back to the set's menus */
void netCoopCampaignMenusOpen(void)
{
	if (s_Campaign.on && g_NetMode == NETMODE_SERVER) {
		netCoopCampaignMenus(0);
	}
}

s32 netCoopCampaignAfterMatch(void)
{
	if (!s_Campaign.on || g_NetMode != NETMODE_SERVER) {
		return 0;
	}

	sysLogPrintf(LOG_NOTE, "net: co-op: campaign: back to the %s menus for the next mission", netCoopGameName(s_Campaign.game));
	netCoopCampaignMenus(1);
	playerPause(MENUROOT_MAINMENU);

	return 1;
}

// --net-test-coop INDEX (with --net-test-difficulty, --net-test-coop-ff,
// --net-test-coop-game TAG: a conversion's set, protocol 14): the gates'
// mission in place of a match; -1 when not asked for
s32 netCoopTestStart(void)
{
	const s32 index = sysArgGetInt("--net-test-coop", -1);
	const char *game = sysArgGetString("--net-test-coop-game");

	if (index < 0) {
		return 0;
	}

	return netCoopHostStart(game ? game : "", index, sysArgGetInt("--net-test-difficulty", DIFF_A), 1, sysArgCheck("--net-test-coop-ff"));
}

/*
 * The mission in the match (both sides)
 */

/**
 * mainmenu.c Abort Mission: a client leaves the session, as End Game does
 * (H11); the host's abort ends the mission for everyone. 1 when handled.
 */
s32 netCoopClientAbort(void)
{
	if (g_NetMode == NETMODE_CLIENT) {
		sysLogPrintf(LOG_NOTE, "net: co-op: this machine's player aborted: leaving the mission");
		netClientLeave();
		return 1;
	}

	return 0;
}

/**
 * pdmain.c mainEndStage, the co-op branch: the host tells the clients
 * (H9); a client takes the host's final block first, so its end screen
 * reads the objectives and deaths the host ended on (H10)
 */
void netCoopMatchEnded(void)
{
	if (g_NetMode == NETMODE_SERVER) {
		netHostMatchEnded();
	} else if (g_NetMode == NETMODE_CLIENT) {
		netClientApplyMatchEnd();
	}
}

/**
 * pdmain.c mainEndStage, the co-op branch, when no end screen went up here
 * (a GoldenEye mission's report is its folder's, gexFrontNetMissionReport()):
 * out of the mission at once, the way the end screen's close goes
 * (menutick.c, MENUROOT_MPENDSCREEN online), back to the room or the
 * campaign's menus (netMenuAfterMatch)
 */
void netCoopLeaveMission(void)
{
	sysLogPrintf(LOG_NOTE, "net: co-op: no end screen here: out of the mission to the menus");
	var80087260 = 3;
	mpSetPaused(MPPAUSEMODE_UNPAUSED);
	g_Vars.mplayerisrunning = false;
	g_Vars.normmplayerisrunning = false;
	g_Vars.lvmpbotlevel = 0;
	titleSetNextStage(STAGE_CITRAINING);
	setNumPlayers(1);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(STAGE_CITRAINING);
}

// H12: the match's flags off
void netCoopMatchStopped(void)
{
	if (g_NetMode == NETMODE_CLIENT && s_ClientMatch) {
		sysLogPrintf(LOG_NOTE, "net: co-op client: %u mission blocks applied, %u cutscene starts, %u ends, %u camera switches followed",
				s_BlocksApplied, s_CutStarts, s_CutEnds, s_WarpFollows);
	}

	// the host's own mode back after a conversion's mission (a client's
	// comes back with its rules, netRulesRestore)
	if (g_NetMode == NETMODE_SERVER && s_HostMatch && s_HostGame[0]) {
		g_GexPlusVariant = s_HostVariantBefore;
	}

	s_HostMatch = 0;
	s_JoinNear = 0;
	s_HostGame[0] = '\0';
	s_ClientMatch = 0;
	s_HaveObj = 0;
	s_HostWarpCmd = -1;
	s_HostWarpParams = NULL;
	s_HostFadeSeq = 0;
	s_HostFadeColour = 0;
	s_HostFadeFrames = 0;
	s_WarpCmd = -1;
	s_HaveFadeSeq = 0;
	s_WarpFollows = 0;
}

/**
 * playerreset.c, a co-op spawn online: past two players the spawn pad is
 * one spot, so the players stand in a ring round it (the ground search that
 * follows takes the rooms found here)
 */
void netCoopSpreadSpawn(struct coord *pos, s16 *rooms)
{
	const s32 count = PLAYERCOUNT();
	RoomNum inrooms[8];
	RoomNum aboverooms[8];
	f32 angle;
	f32 radius;

	if (count <= 2) {
		return;
	}

	angle = (f32)g_Vars.currentplayernum * (M_BADTAU / (f32)count);
	radius = count <= 6 ? 60.0f : 90.0f;
	pos->x += cosf(angle) * radius;
	pos->z += sinf(angle) * radius;

	bgFindRoomsByPos(pos, inrooms, aboverooms, 7, NULL);

	if (inrooms[0] != -1) {
		memcpy(rooms, inrooms, sizeof(inrooms));
	} else if (aboverooms[0] != -1) {
		memcpy(rooms, aboverooms, sizeof(aboverooms));
	}
}

/**
 * A join in progress (the user, 2026-10-07: a campaign "should allow a late
 * join in progress"): netHostLateGo brings the seat's player into play with a
 * new life, and a mission's own spawn is its start, which the players may
 * have left long ago. This one life starts beside a living player instead -
 * the one with the most health, as a co-op respawn picks its buddy - out of
 * PD's own co-op test for the second player's spawn (chrAdjustPosForSpawn:
 * 60 units round in eight directions, ground under it, nothing in the way).
 * A death later respawns the mission's way.
 */
void netCoopHostLateJoin(s32 playernum)
{
	if (s_HostMatch && playernum >= 0 && playernum < MAX_PLAYERS) {
		s_JoinNear |= 1u << playernum;
	}
}

s32 netCoopJoinSpawn(struct coord *pos, s16 *rooms, f32 *turnanglerad)
{
	const s32 pn = g_Vars.currentplayernum;
	struct player *buddy;
	struct coord at;
	RoomNum atrooms[8];
	s32 bn;
	s32 i;

	if (g_NetMode != NETMODE_SERVER || !s_HostMatch || pn < 0 || pn >= MAX_PLAYERS || !(s_JoinNear & (1u << pn))) {
		return 0;
	}

	s_JoinNear &= ~(1u << pn);
	bn = coopRespawnBuddy(pn);
	buddy = bn >= 0 && bn < PLAYERCOUNT() ? g_Vars.players[bn] : NULL;

	if (!buddy || bn == pn || !buddy->prop || !buddy->prop->chr || buddy->isdead || buddy->prop->rooms[0] < 0) {
		sysLogPrintf(LOG_NOTE, "net: co-op: player %d joined in progress; nobody living to spawn beside, the mission's spawn", pn);
		return 0;
	}

	// from just over the buddy's feet, so the test's height covers a body
	at.x = buddy->prop->pos.x;
	at.y = buddy->vv_manground + 30.0f;
	at.z = buddy->prop->pos.z;

	for (i = 0; i < ARRAYCOUNT(atrooms) - 1 && buddy->prop->rooms[i] != -1; i++) {
		atrooms[i] = buddy->prop->rooms[i];
	}

	atrooms[i] = -1;

	if (!chrAdjustPosForSpawn(30.0f, &at, atrooms, buddy->vv_theta * (M_BADTAU / 360.0f), true, false, true)) {
		sysLogPrintf(LOG_NOTE, "net: co-op: player %d joined in progress; no room beside player %d, the mission's spawn", pn, bn);
		return 0;
	}

	*pos = at;

	for (i = 0; i < 8; i++) {
		rooms[i] = atrooms[i];

		if (atrooms[i] == -1) {
			break;
		}
	}

	*turnanglerad = buddy->vv_theta * (M_BADTAU / 360.0f);
	sysLogPrintf(LOG_NOTE, "net: co-op: player %d joined in progress; spawns beside player %d at %.0f %.0f %.0f (room %d)",
			pn, bn, at.x, at.y, at.z, rooms[0]);

	return 1;
}

/*
 * The block: host
 */

void netCoopCapture(u8 *body)
{
	s32 i;
	u8 flags = 0;

	memset(body, 0, MIS_SIZE);
	body[MIS_TICKMODE] = (u8)g_Vars.tickmode;

	if (g_Vars.in_cutscene) {
		flags |= MISF_INCUTSCENE;
	}

	if (g_CountdownTimerRunning) {
		flags |= MISF_COUNTDOWN;
	}

	if (g_ObjectiveChecksDisabled) {
		flags |= MISF_NOCHECKS;
	}

	switch (gexPlusExitPending()) {
	case 1: flags |= MISF_GEEXIT; break;
	case 2: flags |= MISF_GEEXIT | MISF_GEFADE; break;
	}

	body[MIS_FLAGS] = flags;
	put16(body + MIS_CUTANIM, (u16)g_CutsceneAnimNum);
	put32(body + MIS_CUTFRAME, (u32)g_CutsceneCurAnimFrame240);
	put16(body + MIS_TWEEN, (u16)(s16)g_CutsceneTweenDuration60);
	put16(body + MIS_ALARM, (u16)(g_AlarmTimer < 0 ? 0 : g_AlarmTimer > 32000 ? 32000 : g_AlarmTimer));
	putf(body + MIS_COUNTDOWN, g_CountdownTimerValue60);

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		s32 st = i <= g_ObjectiveLastIndex ? objectiveCheck(i) : OBJECTIVE_COMPLETE;

		body[MIS_OBJ + i] = (u8)(st < 0 || st > OBJECTIVE_FAILED ? OBJECTIVE_INCOMPLETE : st);
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		u8 bits = 0;

		if (p) {
			bits |= p->isdead ? MISP_DEAD : 0;
			bits |= p->aborted ? MISP_ABORTED : 0;
			bits |= p->coopcanrestart ? MISP_CANRESTART : 0;
		}

		body[MIS_PLAYERS + i] = bits;
	}

	// a GoldenEye CameraSwitch's shot while the warp is still the one it
	// set (an opening's swirl and the credits are warps of their own)
	if (g_Vars.tickmode == TICKMODE_WARP && g_WarpType1Pad < 0 && g_WarpType2Params
			&& g_WarpType2Params == s_HostWarpParams && s_HostWarpCmd >= 0) {
		put16(body + MIS_WARPCMD, (u16)s_HostWarpCmd);
		put16(body + MIS_WARPDIR, (u16)g_WarpType2HasDirection);
	} else {
		put16(body + MIS_WARPCMD, 0xffff);
	}

	body[MIS_FADESEQ] = s_HostFadeSeq;
	put32(body + MIS_FADECOL, s_HostFadeColour);
	put16(body + MIS_FADELEN, (u16)s_HostFadeFrames);
}

/**
 * The host, ai00df on a converted mission: GoldenEye's CameraSwitch put the
 * camera at setup command `cmdindex` (an ending's shots, Dam's dive). The
 * clients take the same record from their own setup (netCoopApplyWarp())
 */
void netCoopHostCameraSwitch(s32 cmdindex, struct warpparams *params)
{
	if (g_NetMode != NETMODE_SERVER || !s_HostMatch) {
		return;
	}

	s_HostWarpCmd = cmdindex;
	s_HostWarpParams = params;
	sysLogPrintf(LOG_NOTE, "net: co-op: GoldenEye's camera switch to setup command %d (tick %u)", cmdindex, g_NetTick);
}

/** The host, aiFadeScreen on a converted mission: the clients' screens fade too */
void netCoopHostFade(u32 colour, s16 frames)
{
	if (g_NetMode != NETMODE_SERVER || !s_HostMatch || !modloaderStageIsMission(g_Vars.stagenum)) {
		return;
	}

	s_HostFadeSeq++;
	s_HostFadeColour = colour;
	s_HostFadeFrames = frames;
}

/*
 * The block: client
 */

s32 netCoopBlockOk(const u8 *body)
{
	s32 i;

	if (body[MIS_TICKMODE] > TICKMODE_AUTOWALK) {
		return 0;
	}

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		if (body[MIS_OBJ + i] > OBJECTIVE_FAILED) {
			return 0;
		}
	}

	return 1;
}

// the cutscene as the host has it: started, stepped or ended here
static void netCoopApplyCutscene(const u8 *body)
{
	const s32 tickmode = body[MIS_TICKMODE];
	const s32 anim = gets16(body + MIS_CUTANIM);
	const s32 frame240 = (s32)get32(body + MIS_CUTFRAME);
	const s32 tween = gets16(body + MIS_TWEEN);
	const s32 prev = g_Vars.currentplayernum;

	if (g_NetLocalSlot < 0 || g_NetLocalSlot >= PLAYERCOUNT() || !g_Vars.players[g_NetLocalSlot] || g_MainIsEndscreen) {
		return;
	}

	setCurrentPlayerNum(g_NetLocalSlot);

	if (tickmode == TICKMODE_CUTSCENE) {
		if (anim < 0 || anim >= g_NumAnimations) {
			setCurrentPlayerNum(prev);
			return;
		}

		if (g_Vars.tickmode != TICKMODE_CUTSCENE || g_CutsceneAnimNum != anim) {
			if (g_Vars.currentplayer->haschrbody) {
				playerStartCutscene((s16)anim);
				s_CutStarts++;
				sysLogPrintf(LOG_NOTE, "net: co-op client: cutscene anim %d starts at the host's frame %d (tick %u)", anim, frame240 >> 2, g_NetTick);
			}
		}

		if (g_Vars.tickmode == TICKMODE_CUTSCENE) {
			g_CutsceneCurAnimFrame240 = frame240;
			g_CutsceneCurAnimFrame60 = frame240 >> 2;
			// the tween eases the camera to bond's eye: this machine's own
			// player's only when it is bond (otherwise the cut is clean)
			g_CutsceneTweenDuration60 = g_NetLocalSlot == g_Vars.bondplayernum ? tween : -1;
		}
	} else if (g_Vars.tickmode == TICKMODE_CUTSCENE) {
		playerEndCutscene();
		netPredictForceSnap();
		s_CutEnds++;
		sysLogPrintf(LOG_NOTE, "net: co-op client: the cutscene ended (host tick mode %d, tick %u)", tickmode, g_NetTick);
	}

	setCurrentPlayerNum(prev);
}

/**
 * A client: the host's GoldenEye CameraSwitch shot (an ending's: Dam's dive),
 * from this machine's own copy of the setup record, the warp's camera on the
 * host's Bond (playerExecutePreparedWarp()); left when the host's is. The
 * player stands meanwhile, as everyone does on the host (MOVEMODE_CUTSCENE),
 * and prediction waits as in a cutscene (netCoopFollowingWarp())
 */
static void netCoopApplyWarp(const u8 *body)
{
	const s32 cmdindex = gets16(body + MIS_WARPCMD);
	const s32 prev = g_Vars.currentplayernum;

	if (g_NetLocalSlot < 0 || g_NetLocalSlot >= PLAYERCOUNT() || !g_Vars.players[g_NetLocalSlot] || g_MainIsEndscreen) {
		return;
	}

	if (body[MIS_TICKMODE] == TICKMODE_WARP && cmdindex >= 0) {
		struct warpparams *params;

		if (cmdindex == s_WarpCmd && g_Vars.tickmode == TICKMODE_WARP) {
			return;
		}

		params = (struct warpparams *) setupGetCmdByIndex(cmdindex);

		if (!params) {
			return;
		}

		setCurrentPlayerNum(g_NetLocalSlot);
		playerPrepareWarpType2(params, (s16)get16(body + MIS_WARPDIR), 0);
		setCurrentPlayerNum(prev);
		s_WarpCmd = cmdindex;
		s_WarpFollows++;
		sysLogPrintf(LOG_NOTE, "net: co-op client: the host's camera switch to setup command %d (tick %u)", cmdindex, g_NetTick);
	} else if (s_WarpCmd >= 0) {
		s_WarpCmd = -1;

		if (g_Vars.tickmode == TICKMODE_WARP) {
			playerSetTickMode(TICKMODE_NORMAL);
			bmoveSetModeForAllPlayers(MOVEMODE_WALK);
			netPredictForceSnap();
		}

		sysLogPrintf(LOG_NOTE, "net: co-op client: the host's camera is back (host tick mode %d, tick %u)", body[MIS_TICKMODE], g_NetTick);
	}
}

// a client: the host's lists' screen fades on a converted mission
static void netCoopApplyFade(const u8 *body)
{
	const u8 seq = body[MIS_FADESEQ];

	if (!s_HaveFadeSeq) {
		s_HaveFadeSeq = 1;
		s_FadeSeq = seq;
		return;
	}

	if (seq != s_FadeSeq && modloaderStageIsMission(g_Vars.stagenum) && !g_MainIsEndscreen) {
		const u32 colour = get32(body + MIS_FADECOL);
		const s16 frames = gets16(body + MIS_FADELEN);

		lvConfigureFade(colour, frames);
		sysLogPrintf(LOG_NOTE, "net: co-op client: the host's screen fade to %08x over %d frames (tick %u)", colour, frames, g_NetTick);
	}

	s_FadeSeq = seq;
}

/** netpredict.c: this machine is in the host's CameraSwitch shot */
s32 netCoopFollowingWarp(void)
{
	return s_WarpCmd >= 0 && g_Vars.tickmode == TICKMODE_WARP;
}

void netCoopApply(const u8 *body)
{
	s32 i;

	if (g_NetMode != NETMODE_CLIENT) {
		return;
	}

	s_BlocksApplied++;

	if (!s_LoggedStart) {
		s_LoggedStart = 1;
		sysLogPrintf(LOG_NOTE, "net: co-op client: the first mission block (host tick mode %d, %d objectives)", body[MIS_TICKMODE], g_ObjectiveLastIndex + 1);
	}

	netCoopApplyCutscene(body);
	netCoopApplyWarp(body);
	netCoopApplyFade(body);

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		s_ObjStatus[i] = body[MIS_OBJ + i];
	}

	s_HaveObj = 1;
	g_ObjectiveChecksDisabled = (body[MIS_FLAGS] & MISF_NOCHECKS) != 0;

	// GoldenEye's end of a mission: this machine's press goes to the host,
	// and its screen fades with the host's
	{
		const s32 ge = (body[MIS_FLAGS] & MISF_GEFADE) ? 2 : (body[MIS_FLAGS] & MISF_GEEXIT) ? 1 : 0;

		if (ge != gexPlusExitPending()) {
			sysLogPrintf(LOG_NOTE, "net: co-op client: the host's GoldenEye exit %s (tick %u)",
					ge == 2 ? "fades out" : ge == 1 ? "waits for a press" : "is off", g_NetTick);
			gexPlusExitFromHost(ge);
		}
	}
	g_CountdownTimerRunning = (body[MIS_FLAGS] & MISF_COUNTDOWN) != 0;
	g_CountdownTimerValue60 = getf(body + MIS_COUNTDOWN);

	if (g_CountdownTimerRunning) {
		g_CountdownTimerOff = false;
	}

	{
		const s32 alarm = get16(body + MIS_ALARM);

		if (alarm > 0 && g_AlarmTimer <= 0) {
			alarmActivate();
		} else if (alarm <= 0 && g_AlarmTimer > 0) {
			alarmDeactivate();
		}

		if (alarm > 0) {
			g_AlarmTimer = alarm;
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		const u8 bits = body[MIS_PLAYERS + i];

		if (!p) {
			continue;
		}

		p->aborted = (bits & MISP_ABORTED) != 0;
		p->coopcanrestart = (bits & MISP_CANRESTART) != 0;

		// another machine's player's death: the record's life mirrors it
		// each pose (netpuppets.c); the host's word settles the end screen
		if (i != g_NetLocalSlot && (bits & MISP_DEAD) && !p->isdead) {
			p->isdead = true;
			p->redbloodfinished = true;
			p->deathanimfinished = true;
		} else if (i != g_NetLocalSlot && !(bits & MISP_DEAD) && p->isdead) {
			p->isdead = false;
		}
	}
}

// MATCH_END's block: the end the host saw, for every machine's end screen
void netCoopApplyFinal(const u8 *body)
{
	s32 i;

	if (g_NetMode != NETMODE_CLIENT) {
		return;
	}

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		s_ObjStatus[i] = body[MIS_OBJ + i];
	}

	s_HaveObj = 1;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		const u8 bits = body[MIS_PLAYERS + i];

		if (!p) {
			continue;
		}

		p->aborted = (bits & MISP_ABORTED) != 0;
		p->isdead = (bits & MISP_DEAD) != 0;

		if (p->isdead) {
			p->redbloodfinished = true;
			p->deathanimfinished = true;
		}
	}

	sysLogPrintf(LOG_NOTE, "net: co-op client: the mission ended: %s, objectives %d/%d complete",
			coopAnyAborted() ? "aborted" : coopAllDead() ? "everyone dead" : objectiveIsAllComplete() ? "completed" : "failed",
			netCoopObjectivesComplete(), g_ObjectiveLastIndex + 1);
}

// objectives.c objectiveCheck on a client: the host's status (1 known)
s32 netCoopObjectiveStatus(s32 index, s32 *status)
{
	if (!s_HaveObj || index < 0 || index >= MAX_OBJECTIVES) {
		return 0;
	}

	*status = s_ObjStatus[index];
	return 1;
}

s32 netCoopObjectivesComplete(void)
{
	s32 n = 0;
	s32 i;

	for (i = 0; i <= g_ObjectiveLastIndex && i < MAX_OBJECTIVES; i++) {
		if (objectiveCheck(i) == OBJECTIVE_COMPLETE) {
			n++;
		}
	}

	return n;
}

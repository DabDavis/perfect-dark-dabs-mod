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
#include "modmode.h"
#include "gexplusrom.h"
#include "gexfront.h"
#include "gecinema.h"
#include "gecredits.h"
#include "gexplus.h"
#include "geconvert.h"
#include "game/bg.h"
#include "game/chraction.h"
#include "game/chr.h"
#include "game/modalarm.h"
#include "game/prop.h"
#include "game/coop.h"
#include "game/lv.h"
#include "game/bondmove.h"
#include "game/setuputils.h"
#include "game/lang.h"
#include "game/menu.h"
#include "game/modoptions.h"
#include "game/objectives.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/pdmode.h"
#include "game/propobj.h"
#include "game/title.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "math.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "net/netlobby.h"
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
 * Counter-op online (protocol 32, the user, 2026-10-12: "lets add
 * counter-ops to online play for PD, GE, etc."): a room's Game "Counter-Op
 * Mission", each player picking a side in the Game Lobby (the room's two
 * teams: 0 the agents, 1 the counter-ops). The agents play the mission as
 * online co-op does - bond the first agent's seat, Mission Respawn's team
 * lives, the dead watching - and every counter-op is Perfect Dark's own
 * (src/game/coop.c widens g_Vars.anti to a mask of players): a guard's body,
 * gun and health, a new guard at each death for as long as there are any
 * (A, B, Z or START), the agents' deaths its win. RULES carry the sides
 * (the mask by player number); the host takes each counter-op's guard and
 * the mission block takes the body to the clients.
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
 *    26  u8  players[12]          bit 0 isdead, 1 aborted, 2 coopcanrestart,
 *                                 3 the death that left nobody alive: the
 *                                 mission is lost, and on a converted
 *                                 mission GoldenEye's replay plays for it
 *                                 (protocol 22), 4 the death comes
 *                                 back (Mission Respawn's rules online:
 *                                 a new life where the player fell)
 *    38  s16 warpcmd              the setup command a GoldenEye CameraSwitch
 *                                 (ai00df) put the camera at, -1 none
 *                                 (protocol 17)
 *    40  s16 warpdir              its direction word (g_WarpType2HasDirection)
 *    42  u8  fadeseq              counts the screen fades a converted
 *                                 mission's lists asked for (aiFadeScreen)
 *    44  u32 fadecolour           the last one's colour
 *    48  s16 fadeframes           and length
 *    50  u8  cinema               (protocol 27) the Cinema page's cinema the
 *                                 match is (GECINEMA_NET_*), 0 a mission
 *    51  u8  cineshot             the opening's gallery: the shot showing
 *    52  u8  cineseq              counts the shots started (a guest takes
 *                                 the shot afresh when it changes)
 *    53  u8  cineflags            1 the host backed out of the gallery, 2 the
 *                                 credits' camera goes round, 4 their roll is
 *                                 on, 8 it is over
 *    54  s16 credpad              the credits' orbit (CameraOrbitPad): its pad,
 *    56  s16 creddistance         distance,
 *    58  s16 credheight           height
 *    60  s16 credlookheight       and the height it looks at
 *    62  f32 credspeed            radians a 60th
 *    66  f32 credangle            where it is now
 *    70  f32 credframe            how far the roll is
 *    74  u16 antimask             (protocol 32) the counter-ops, a bit per
 *                                 player number (0: a co-op mission)
 *    76  u8  antispawns[12]       each counter-op's guards taken (counts up)
 *    88  u8  antiradius[12]       the guard's radius it took (its walk's)
 *   100  s16 antibody[12]         the guard's body it wears, -1 none yet
 *   124  s16 antihead[12]         and its head
 *   148  u8  antitaken[32]        the setup chrs counter-ops have taken this
 *                                 mission, a bit per chrnum (0-255, the
 *                                 setup's own numbers): a client takes its
 *                                 copy out of the world
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
#define MIS_CINE      (MIS_FADELEN + 2)  // protocol 27: the Cinema page's cinema the match is
#define MIS_CINESHOT  (MIS_CINE + 1)
#define MIS_CINESEQ   (MIS_CINESHOT + 1)
#define MIS_CINEFLAGS (MIS_CINESEQ + 1)
#define MIS_CREDPAD   (MIS_CINEFLAGS + 1)
#define MIS_CREDDIST  (MIS_CREDPAD + 2)
#define MIS_CREDHIGH  (MIS_CREDDIST + 2)
#define MIS_CREDLOOK  (MIS_CREDHIGH + 2)
#define MIS_CREDSPEED (MIS_CREDLOOK + 2)
#define MIS_CREDANGLE (MIS_CREDSPEED + 4)
#define MIS_CREDFRAME (MIS_CREDANGLE + 4)
#define MIS_ANTIMASK  (MIS_CREDFRAME + 4) // protocol 32: online counter-op
#define MIS_ANTISPAWN (MIS_ANTIMASK + 2)
#define MIS_ANTIRAD   (MIS_ANTISPAWN + MAX_PLAYERS)
#define MIS_ANTIBODY  (MIS_ANTIRAD + MAX_PLAYERS)
#define MIS_ANTIHEAD  (MIS_ANTIBODY + MAX_PLAYERS * 2)
#define MIS_ANTITAKEN (MIS_ANTIHEAD + MAX_PLAYERS * 2)
#define MIS_TAKENCHRS 256
#define MIS_SIZE      (MIS_ANTITAKEN + MIS_TAKENCHRS / 8)

#define MISC_LEFT     0x01 // the host backed out of the gallery
#define MISC_ORBIT    0x02 // the credits' camera goes round its pad
#define MISC_ROLLING  0x04 // their roll is on
#define MISC_ROLLED   0x08 // ... and over

#define MISF_INCUTSCENE 0x01
#define MISF_COUNTDOWN  0x02
#define MISF_NOCHECKS   0x04
#define MISF_GEEXIT     0x08
#define MISF_GEFADE     0x10

#define MISP_DEAD       0x01
#define MISP_ABORTED    0x02
#define MISP_CANRESTART 0x04
#define MISP_LASTDEATH  0x08
#define MISP_RESPAWNDUE 0x10

// player.c's (PC builds are NTSC: s32; no header declares them)
extern s32 g_CutsceneCurAnimFrame240;
extern s32 g_CutsceneTweenDuration60;
extern s16 g_WarpType1Pad;
extern struct warpparams *g_WarpType2Params;
extern s32 g_WarpType2HasDirection;

struct netcoopsetup g_NetCoopSetup = { 0, 0, DIFF_A, 1, 0 };

// online counter-op (protocol 32)
static s32 s_HostAnti = 0;                 // host: the mission being started or run is counter-op
static s32 s_HostAntiNext = 0;             // host: ... the start under way is (netCoopHostStart's callers)
static s32 s_ClientAnti = 0;               // client: RULES said counter-op
static u16 s_ClientAntiMask = 0;           // client: ... and the sides, by player number
static u8 s_AntiSpawnSeen[MAX_PLAYERS];    // client: the host's spawn counts taken
static s16 s_AntiBuiltBody[MAX_PLAYERS];   // client: the body put on the player here
static s16 s_AntiBuiltHead[MAX_PLAYERS];
static u32 s_AntiRebuilds = 0;
static u8 s_HostAntiTaken[MIS_TAKENCHRS / 8];   // host: the setup chrs counter-ops took, by chrnum
static u8 s_ClientAntiTaken[MIS_TAKENCHRS / 8]; // client: ... and taken out here
static u32 s_AntiTakenOut = 0;

static void netCoopSetRoles(u32 antimask, s32 numplayers);
static s32 netCoopPopCount(u32 bits);

static s32 s_HostMatch = 0;       // host: the match H1 is starting (or running) is a co-op mission
static u32 s_JoinNear = 0;        // host: players a join in progress took, whose next spawn is on a living one's spot
static struct coord s_StackPos;    // the first player's spot this stage: everyone's first life (netCoopStackSpawn)
static RoomNum s_StackRooms[8];
static f32 s_StackAngle;
static s32 s_StackValid = 0;
static char s_HostGame[16];       // host: the mission's set ("" Perfect Dark's, else a conversion's tag)

// A Cinema-page cinema played as the room's match (protocol 27): what it is
// (GECINEMA_NET_*), its Loop row and Time row
struct netcoopcine {
	s32 kind;
	s32 loop;
	s32 minutes;
};

static struct netcoopcine s_HostCineNext;   // host: the start under way is the Cinema page's
static struct netcoopcine s_HostCine;       // host: the match running is that cinema
static struct netcoopcine s_ClientCine;     // client: RULES said the match is a cinema
static s32 s_ClientLoopBefore = -1;         // client: its own Loop and Time rows, back after
static s32 s_ClientMinutesBefore;
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
static s32 s_LostSubject = -1;                // the player whose death lost the mission (both sides), -1 none
static s32 s_LostFrame = 0;                   // the frame it was seen (a stage since is a new one)
static s32 s_CoopDeaths = 0;                  // host: this mission's deaths, every player's (Mission Lives is the team's)
static u8 s_RespawnDue[MAX_PLAYERS];          // host: that player's death comes back
static s32 s_LocalRespawnDue = 0;             // client: the host said this machine's player's death comes back
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

static const char *netCoopCinemaName(s32 kind)
{
	switch (kind) {
	case GECINEMA_NET_OPENING: return "opening";
	case GECINEMA_NET_ENDING:  return "ending";
	case GECINEMA_NET_CREDITS: return "credits";
	default:                   return "mission";
	}
}

// host: the match H1 is starting is the Cinema page's cinema (RULES, protocol 27)
void netCoopHostCinema(u8 *kind, u8 *loop, u8 *minutes)
{
	const s32 on = s_HostMatch && s_HostCine.kind;

	*kind = on ? (u8)s_HostCine.kind : 0;
	*loop = on ? (u8)s_HostCine.loop : 0;
	*minutes = on ? (u8)s_HostCine.minutes : 0;
}

/**
 * The host, an AI list's line on a co-op match (chraicommands.c): every
 * player's, on a converted GoldenEye mission (GoldenEye had one player, whose
 * line it was; online a guest saw none of them - Cuba's dialogue under the
 * room's credits, a mission's "Trevelyan has activated the antenna control
 * console!") or a Cinema-page cinema. Perfect Dark's own missions keep their
 * lines to the player they name.
 */
s32 netCoopListTextToAll(void)
{
	return g_NetMode == NETMODE_SERVER && s_HostMatch
		&& (s_HostCine.kind != GECINEMA_NET_NONE || modloaderStageIsRemake(g_Vars.stagenum));
}

/**
 * The cinema this machine's match is, GECINEMA_NET_* (0 a mission, or no
 * co-op match): the host's from its own start, a guest's from RULES
 */
s32 netCoopCinemaKind(void)
{
	if (g_NetMode == NETMODE_SERVER) {
		return s_HostMatch ? s_HostCine.kind : 0;
	}

	if (g_NetMode == NETMODE_CLIENT) {
		return s_ClientMatch ? s_ClientCine.kind : 0;
	}

	return 0;
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
		// a PD mod entered from the Perfect Menu plays its own missions
		// under Perfect Dark's set (protocol 25): "<Mod> Campaign"
		return modModeIsActive() ? modModeDisplayNameOf(modModeName()) : "Perfect Dark";
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

	// protocol 32: counter-op is a mission's, never a cinema's
	if (r->anti > 1 || (r->anti && r->cinema) || (!r->anti && r->antimask)) {
		return 0;
	}

	// protocol 27: the Cinema page's cinemas are a conversion's
	if (r->cinema > GECINEMA_NET_CREDITS || r->cinemaloop >= GECINEMA_NUM_LOOPS
			|| (r->cinema && netCoopIsPdGame(r->game))) {
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

	g_MissionConfig.iscoop = !r->anti;
	g_MissionConfig.isanti = r->anti ? true : false;
	g_MissionConfig.pdmode = false;
	g_MissionConfig.stageindex = r->stageindex;
	g_MissionConfig.stagenum = netCoopMissionStage(r->game, r->stageindex);
	g_MissionConfig.difficulty = r->difficulty;
	g_Vars.coopradaron = r->radar ? true : false;
	g_Vars.antiradaron = r->radar ? true : false;
	g_Vars.coopfriendlyfire = r->friendlyfire ? true : false;
	g_Vars.numaibuddies = 0;
	s_ClientMatch = 1;
	// protocol 32: counter-op, the sides by player number (netCoopClientStage)
	s_ClientAnti = r->anti != 0;
	s_ClientAntiMask = r->antimask;

	// protocol 27: the host's Cinema-page cinema, which this machine plays
	// too, following the host's mission block (the opening's and ending's
	// armed as the folder arms them; the credits are Cuba's stage itself)
	s_ClientCine.kind = r->cinema;
	s_ClientCine.loop = r->cinemaloop;
	s_ClientCine.minutes = r->cinemaminutes;

	if (r->cinema == GECINEMA_NET_OPENING || r->cinema == GECINEMA_NET_ENDING) {
		if (s_ClientLoopBefore < 0) {
			s_ClientLoopBefore = gecinemaGetLoop();
			s_ClientMinutesBefore = gecinemaGetMinutes();
		}

		gecinemaSetLoop(r->cinemaloop);
		gecinemaSetMinutes(r->cinemaminutes);
		gecinemaArm(r->stageindex, r->cinema == GECINEMA_NET_ENDING ? GECINEMA_ENDING : GECINEMA_OPENING);
	}

	if (r->cinema) {
		sysLogPrintf(LOG_NOTE, "net: co-op: the host's mission is the Cinema page's %s: watched here with the host",
				netCoopCinemaName(r->cinema));
	}

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
	s32 i;

	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = 1;
	g_Vars.antiplayernum = -1;
	g_CoopAntiMask = 0;

	if (s_ClientAnti) {
		netCoopSetRoles(s_ClientAntiMask, netCoopPopCount(mpHumanSlotMask()));
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		s_AntiSpawnSeen[i] = 0;
		s_AntiBuiltBody[i] = -1;
		s_AntiBuiltHead[i] = -1;
	}

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
 * Online counter-op: the players' parts by their sides (antimask, a bit per
 * player number). bond is the first agent, g_Vars.anti the first counter-op;
 * g_Vars.coop is a second agent where there is one, else the first other
 * seat (coop.c never takes it for "the other player" while counter-ops
 * play), so online co-op's rules stay on for the agents. A mask of every
 * player or none is co-op: a mission needs one of each.
 */
static void netCoopSetRoles(u32 antimask, s32 numplayers)
{
	const u32 all = numplayers >= 32 ? 0xffffffffu : (1u << numplayers) - 1;
	s32 i;

	antimask &= all;

	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = 1;
	g_Vars.antiplayernum = -1;
	g_CoopAntiMask = 0;

	if (antimask == 0 || antimask == all) {
		if (antimask) {
			sysLogPrintf(LOG_WARNING, "net: counter-op: every player is a counter-op (mask 0x%x): played as co-op", antimask);
		}

		return;
	}

	g_Vars.bondplayernum = -1;
	g_Vars.coopplayernum = -1;

	for (i = 0; i < numplayers && i < MAX_PLAYERS; i++) {
		if (antimask & (1u << i)) {
			if (g_Vars.antiplayernum < 0) {
				g_Vars.antiplayernum = i;
			}
		} else if (g_Vars.bondplayernum < 0) {
			g_Vars.bondplayernum = i;
		} else if (g_Vars.coopplayernum < 0) {
			g_Vars.coopplayernum = i;
		}
	}

	if (g_Vars.coopplayernum < 0) {
		// one agent: the coop slot is a counter-op's seat (coop.c's
		// questions leave it out by the mask)
		g_Vars.coopplayernum = g_Vars.bondplayernum == 0 ? 1 : 0;
	}

	g_CoopAntiMask = antimask;
	coopAntiResetBodies();
	memset(s_HostAntiTaken, 0, sizeof(s_HostAntiTaken));
	memset(s_ClientAntiTaken, 0, sizeof(s_ClientAntiTaken));

	sysLogPrintf(LOG_NOTE, "net: counter-op: %d players, counter-ops 0x%03x: bond is player %d, anti %d, coop %d",
			numplayers, antimask, g_Vars.bondplayernum, g_Vars.antiplayernum, g_Vars.coopplayernum);
}

static s32 netCoopPopCount(u32 bits)
{
	s32 n = 0;

	for (; bits; bits &= bits - 1) {
		n++;
	}

	return n;
}

/**
 * netsession.c H1 (netHostMatchStarting), the seats made and before RULES
 * go: a counter-op mission's sides. Player k is the k-th seat in play; it
 * is a counter-op when a member holds the seat (taken: occupied) and its
 * team is the room's second (the launch put each member's room team on its
 * slot, netlobby.c). An open seat is an agent's, out of play until a join.
 * --net-test-anti MASK: those seats' (by slot) in a --host session.
 */
void netCoopHostSides(u32 bits, u32 occupied)
{
	const s32 testmask = sysArgGetInt("--net-test-anti", -1);
	u32 antimask = 0;
	s32 k = 0;
	s32 slot;

	if (g_NetMode != NETMODE_SERVER || !s_HostMatch || !s_HostAnti) {
		return;
	}

	for (slot = 0; slot < MAX_PLAYERS; slot++) {
		if (!(bits & (1u << slot))) {
			continue;
		}

		if (testmask >= 0 ? ((u32)testmask & (1u << slot)) != 0
				: (occupied & (1u << slot)) && (g_PlayerConfigsArray[slot].base.team & 7) == 1) {
			antimask |= 1u << k;
		}

		k++;
	}

	netCoopSetRoles(antimask, k);

	if (g_Vars.antiplayernum < 0) {
		// nobody on the counter-ops' side: the room plays co-op
		s_HostAnti = 0;
		g_MissionConfig.iscoop = true;
		g_MissionConfig.isanti = false;
		sysLogPrintf(LOG_NOTE, "net: counter-op: no seat is a counter-op's; the mission is played co-op");
	}
}

s32 netCoopHostAnti(void)
{
	return s_HostMatch && s_HostAnti;
}

u16 netCoopHostAntiMask(void)
{
	return netCoopHostAnti() ? (u16)g_CoopAntiMask : 0;
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

	// online counter-op: the room's (or the test's); the sides at H1
	s_HostAnti = s_HostAntiNext;
	s_HostAntiNext = 0;

	// the Cinema page's start (netCoopAcceptMission) or a mission's
	s_HostCine = s_HostCineNext;
	memset(&s_HostCineNext, 0, sizeof(s_HostCineNext));

	// a conversion's mission plays under its set's mode (netCoopClientApplyRules)
	if (!netCoopIsPdGame(game)) {
		s_HostVariantBefore = g_GexPlusVariant;
		g_GexPlusVariant = variant;
	}

	g_MissionConfig.iscoop = !s_HostAnti;
	g_MissionConfig.isanti = s_HostAnti ? true : false;
	// a campaign's host keeps what its folder set (GoldenEye's 007 is
	// Perfect Dark's PD Mode with its sliders): the host runs the simulation
	if (!s_Campaign.on) {
		g_MissionConfig.pdmode = false;
	}
	g_MissionConfig.stageindex = stageindex;
	g_MissionConfig.stagenum = stagenum;
	g_MissionConfig.difficulty = difficulty;
	g_Vars.coopradaron = radar ? true : false;
	g_Vars.antiradaron = radar ? true : false;
	g_Vars.coopfriendlyfire = friendlyfire ? true : false;
	g_Vars.numaibuddies = 0;
	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = 1;
	g_Vars.antiplayernum = -1;
	g_Vars.perfectbuddynum = 0;
	g_CoopAntiMask = 0;

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

		sysLogPrintf(LOG_NOTE, "net: co-op: starting %s (%s, index %d, stage 0x%02x) on %s with %d players, radar %d, friendly fire %d%s",
				netCoopClean(netCoopMissionNameOf(game, stageindex), name, sizeof(name)), netCoopGameName(game), stageindex, stagenum,
				netCoopClean(netCoopDifficultyName(difficulty), diff, sizeof(diff)), numplayers, radar, friendlyfire,
				s_HostAnti ? ", counter-op" : "");
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

		// What the folder's Cinema page plays (a mission's opening or ending,
		// the credits) is watched, not played, and the room watches it with
		// the host (the user, 2026-10-10: "let's pull guests in also"): the
		// room's next match, RULES saying which cinema (protocol 27), every
		// guest playing it in step with the host's mission block and back in
		// the room at its end. (Before, the guests stood in the mission and
		// never saw it, and the cinema's way back to the folder left the
		// match's flags up: crash 20261009-233623.)
		const s32 cine = gexFrontStartingCinema();

		if (cine) {
			s_HostCineNext.kind = cine;
			s_HostCineNext.loop = cine == GECINEMA_NET_OPENING ? gecinemaGetLoop() : GECINEMA_LOOP_OFF;
			s_HostCineNext.minutes = gecinemaGetMinutes();
			sysLogPrintf(LOG_NOTE, "net: co-op: campaign: the Cinema page's %s of stage 0x%02x is the room's to watch (loop %d)",
					netCoopCinemaName(cine), g_MissionConfig.stagenum, s_HostCineNext.loop);
		}

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

		memset(&s_HostCineNext, 0, sizeof(s_HostCineNext));
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

/**
 * Host migration (netmigrate.c): a campaign room taken over mid-mission is
 * this host's campaign now; the mission starts again at once, and its menus
 * come after it as a campaign host's do
 */
void netCoopCampaignResume(const char *game, s32 radar, s32 friendlyfire)
{
	memset(&s_Campaign, 0, sizeof(s_Campaign));
	s_Campaign.on = 1;
	s_Campaign.radar = radar;
	s_Campaign.friendlyfire = friendlyfire;
	snprintf(s_Campaign.game, sizeof(s_Campaign.game), "%s", game ? game : "");
	sysLogPrintf(LOG_NOTE, "net: co-op: the %s campaign is this host's now", netCoopGameName(s_Campaign.game));
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
// a lobby room's (netsession.c) or a resumed one's (netmigrate.c) start is counter-op
void netCoopHostNextAnti(s32 anti)
{
	s_HostAntiNext = anti ? 1 : 0;
}

s32 netCoopTestStart(void)
{
	const s32 index = sysArgGetInt("--net-test-coop", -1);
	const char *game = sysArgGetString("--net-test-coop-game");

	if (index < 0) {
		return 0;
	}

	// --net-test-anti MASK: counter-op, those slots' players the counter-ops
	s_HostAntiNext = sysArgGetInt("--net-test-anti", -1) >= 0;

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

/**
 * gexfront.c gexFrontGoBack online: whatever goes back to the folder that way
 * leaves no match behind. The Institute it loads is the menus: a stage left
 * with a match's flags up (mplayerisrunning, lvmpbotlevel) skips mpReset() and
 * plays the menus as that match, its chr pointers the last stage's
 * (crash 20261009-233623: a campaign host's Cinema-page ending). A match the
 * host's session still runs there ends for its clients as a mission does.
 */
void netCoopFolderBack(void)
{
	if (g_NetMode == NETMODE_SERVER && netSessionMatchActive() && !netSessionHostEnded()) {
		sysLogPrintf(LOG_NOTE, "net: co-op: the match's stage goes back to the folder: the match ends");
		netHostMatchEnded();
	}

	if (g_Vars.mplayerisrunning || g_Vars.lvmpbotlevel) {
		sysLogPrintf(LOG_NOTE, "net: back to the folder: the match's flags off (mplayer %d, bots %d)",
				g_Vars.mplayerisrunning, g_Vars.lvmpbotlevel);
	}

	mpSetPaused(MPPAUSEMODE_UNPAUSED);
	g_Vars.mplayerisrunning = false;
	g_Vars.normmplayerisrunning = false;
	g_Vars.lvmpbotlevel = 0;
}

/**
 * pdmain.c mainEndStage's co-op branch, after MATCH_END (the host's) or its
 * final block (a client's): a Cinema-page cinema ends on no report. A guest
 * goes back to the room as from a mission's end, the host to its folder the
 * way the cinema itself goes there offline (the credits' end, Cuba's list's
 * own EndLevel, is the one that comes this way). 1 when it was a cinema.
 */
s32 netCoopCinemaEnded(void)
{
	if (g_NetMode == NETMODE_CLIENT && s_ClientMatch && s_ClientCine.kind) {
		sysLogPrintf(LOG_NOTE, "net: co-op: the host's %s is over: back to the room", netCoopCinemaName(s_ClientCine.kind));
		netCoopLeaveMission();
		return 1;
	}

	if (g_NetMode == NETMODE_SERVER && s_HostMatch && s_HostCine.kind) {
		sysLogPrintf(LOG_NOTE, "net: co-op: the room's %s is over: back to the folder", netCoopCinemaName(s_HostCine.kind));

		if (s_HostCine.kind == GECINEMA_NET_CREDITS) {
			gexFrontCreditsOver();
		} else if (!gecinemaEndingOver()) {
			netCoopLeaveMission();
		}

		return 1;
	}

	return 0;
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

	// a guest's cinema: nothing armed for the stage after it, and its own
	// Loop and Time rows back
	if (g_NetMode == NETMODE_CLIENT && s_ClientCine.kind) {
		gecinemaArm(-1, GECINEMA_OPENING);

		if (s_ClientLoopBefore >= 0) {
			gecinemaSetLoop(s_ClientLoopBefore);
			gecinemaSetMinutes(s_ClientMinutesBefore);
		}
	}

	s_ClientLoopBefore = -1;
	memset(&s_HostCine, 0, sizeof(s_HostCine));
	memset(&s_ClientCine, 0, sizeof(s_ClientCine));

	s_HostMatch = 0;
	s_HostAnti = 0;
	s_ClientAnti = 0;
	s_ClientAntiMask = 0;
	g_CoopAntiMask = 0;
	s_JoinNear = 0;
	s_StackValid = 0;
	s_HostGame[0] = '\0';
	s_ClientMatch = 0;
	s_HaveObj = 0;
	s_HostWarpCmd = -1;
	netCoopStageReset();
	s_HostWarpParams = NULL;
	s_HostFadeSeq = 0;
	s_HostFadeColour = 0;
	s_HostFadeFrames = 0;
	s_WarpCmd = -1;
	s_HaveFadeSeq = 0;
	s_WarpFollows = 0;
}

/**
 * playerreset.c, a co-op mission's first life online. Every player starts on
 * the one spot (the user, 2026-10-08, on GoldenEye's Cradle and Archives:
 * "the spawn point online is spawning players behind walls, outside levels
 * ... might be easier to stack all players on same tile since collision is
 * disabled for players only"): co-op players pass through each other
 * (g_NetPlayersPassThrough), so nobody needs a place of their own. Players
 * past two had stood in a ring 60-90 units round the pad, tested against
 * nothing, and the second was moved off the first by chrAdjustPosForSpawn,
 * whose line test a converted level's walls do not always stop. lvReset
 * resets the players in order, so the first picks the pad (its choice
 * blind to other players, as netCoopSpawnPick) and the rest take its spot.
 */
f32 netCoopSpawnPick(struct coord *pos, s16 *rooms)
{
	f32 angle;

	// the pad itself, not a step off it for a player standing there
	g_NetPlayersPassThrough = 1;
	angle = M_BADTAU - scenarioChooseSpawnLocation(30, pos, rooms, g_Vars.currentplayer->prop);
	g_NetPlayersPassThrough = 0;

	return angle;
}

f32 netCoopStackSpawn(struct coord *pos, s16 *rooms)
{
	s32 i;

	if (g_Vars.currentplayernum == 0 || !s_StackValid) {
		s_StackAngle = netCoopSpawnPick(pos, rooms);
		s_StackPos = *pos;

		for (i = 0; i < ARRAYCOUNT(s_StackRooms) - 1 && rooms[i] != -1; i++) {
			s_StackRooms[i] = rooms[i];
		}

		s_StackRooms[i] = -1;
		s_StackValid = 1;

		return s_StackAngle;
	}

	*pos = s_StackPos;

	for (i = 0; i < ARRAYCOUNT(s_StackRooms); i++) {
		rooms[i] = s_StackRooms[i];

		if (s_StackRooms[i] == -1) {
			break;
		}
	}

	return s_StackAngle;
}

// Player bn can have player pn stacked on it: living, on its feet, on the ground
static s32 netCoopStackable(s32 bn, s32 pn)
{
	const struct player *p = bn >= 0 && bn < MAX_PLAYERS ? g_Vars.players[bn] : NULL;

	return p && bn != pn && p->prop && p->prop->chr && !p->isdead && p->prop->rooms[0] >= 0
		&& p->bondmovemode == MOVEMODE_WALK && p->vv_ground > -100000 && p->vv_manground - p->vv_ground < 5.0f;
}

/**
 * A join in progress (the user, 2026-10-07: a campaign "should allow a late
 * join in progress"): netHostLateGo brings the seat's player into play with a
 * new life, and a mission's own spawn is its start, which the players may
 * have left long ago. This one life starts on a living player's own spot
 * instead - the one with the most health, as a co-op respawn picks its
 * buddy - stacked there as the players are at the start (netCoopStackSpawn):
 * a spot 60 units round it (chrAdjustPosForSpawn's eight directions) could
 * be through a converted level's wall. A buddy in a vehicle or mid-air is
 * passed over for one walking on the ground. A death later respawns the
 * mission's way.
 */
/**
 * Online counter-op: a join in progress plays the side its room team says
 * (the Game Lobby's; the seat's old player's is not its). --net-test-anti's
 * slot mask in a --host session. The mission block takes it to everyone.
 */
void netCoopHostLateSide(s32 playernum, const char *account)
{
	const s32 testmask = sysArgGetInt("--net-test-anti", -1);
	s32 anti;

	if (!netCoopHostAnti() || playernum < 0 || playernum >= MAX_PLAYERS) {
		return;
	}

	if (testmask >= 0) {
		anti = ((u32)testmask >> (g_Vars.playerstats[playernum].mpindex % MAX_PLAYERS)) & 1;
	} else {
		anti = netLobbyMemberTeam(account) == 1;
	}

	if (anti) {
		g_CoopAntiMask |= 1u << playernum;
	} else {
		g_CoopAntiMask &= ~(1u << playernum);
	}

	// never every player: the agents' side is the mission's
	if (g_CoopAntiMask == ((1u << PLAYERCOUNT()) - 1)) {
		g_CoopAntiMask &= ~(1u << playernum);
		anti = 0;
	}

	sysLogPrintf(LOG_NOTE, "net: counter-op: player %d (\"%s\") joins as %s (counter-ops 0x%03x)",
			playernum, account ? account : "", anti ? "a counter-op" : "an agent", g_CoopAntiMask);
}

/**
 * playerSpawnAnti, the host: a counter-op takes this guard, which goes from
 * the world next. A setup chr's copy on every client is that machine's own
 * (mapped by its setup command, posed from the host's records), and a record
 * that stops coming leaves it standing where it was: the mission block's
 * bitset of the setup chrs taken takes it out on each client, a join in
 * progress's too. A chr spawned at run time is a puppet of the client's
 * making, freed when its id leaves the snapshots.
 */
void netCoopAntiTookChr(struct prop *prop)
{
	s32 chrnum;

	if (g_NetMode != NETMODE_SERVER || !netCoopHostAnti() || !prop || !prop->chr) {
		return;
	}

	// a setup chr: the stage's load (the first guards are taken there,
	// before the entity table is made), or a setup command's chr since;
	// by its setup chrnum, the same on every machine
	chrnum = prop->chr->chrnum;

	if (g_Vars.lvframenum > 0 && netEntsSetupCmdOf(prop - g_Vars.props) < 0) {
		// one spawned at run time (Guards Alerted!, a mission's
		// reinforcements): the clients' copy goes with its record
		sysLogPrintf(LOG_NOTE, "net: counter-op: player %d takes chr %d (spawned%s) at tick %u",
				g_Vars.currentplayernum, chrnum, modAlarmIsGuard(prop->chr) ? ", Guards Alerted!" : "", g_NetTick);
		return;
	}

	if (chrnum < 0 || chrnum >= MIS_TAKENCHRS) {
		sysLogPrintf(LOG_WARNING, "net: counter-op: the guard taken is chr %d, past the %d the block names: clients keep their copy",
				chrnum, MIS_TAKENCHRS);
		return;
	}

	s_HostAntiTaken[chrnum >> 3] |= 1 << (chrnum & 7);
	sysLogPrintf(LOG_NOTE, "net: counter-op: player %d takes setup chr %d at tick %u (frame %d)",
			g_Vars.currentplayernum, chrnum, g_NetTick, g_Vars.lvframenum);
}

// the client: the setup chrs the host's counter-ops took, out of the world
// here as playerSpawnAnti takes the host's
static void netCoopApplyAntiTaken(const u8 *body)
{
	s32 i;

	for (i = 0; i < MIS_TAKENCHRS; i++) {
		struct chrdata *chr;
		struct prop *prop;

		if (!(body[MIS_ANTITAKEN + (i >> 3)] & (1 << (i & 7))) || (s_ClientAntiTaken[i >> 3] & (1 << (i & 7)))) {
			continue;
		}

		s_ClientAntiTaken[i >> 3] |= 1 << (i & 7);
		chr = chrFindByLiteralId(i);
		prop = chr ? chr->prop : NULL;

		if (!prop || prop->type != PROPTYPE_CHR || prop->chr != chr) {
			continue;
		}

		chrRemove(prop, true);
		propDeregisterRooms(prop);
		propDelist(prop);
		propDisable(prop);
		propFree(prop);
		s_AntiTakenOut++;
		sysLogPrintf(LOG_NOTE, "net: counter-op client: setup chr %d taken by a counter-op: out of the world here (tick %u)", i, g_NetTick);
	}
}

s32 netCoopAgentsGone(void)
{
	static s32 s_GoneFrame = -1;
	s32 i;

	if (g_NetMode != NETMODE_SERVER || !netCoopHostAnti() || g_MainIsEndscreen || g_Vars.lvframenum < TICKS(120)
			|| g_Vars.tickmode != TICKMODE_NORMAL) {
		return 0;
	}

	for (i = 0; i < PLAYERCOUNT() && i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] && !coopIsAntiNum(i) && !netPlayerOutOfPlay(i)) {
			return 0;
		}
	}

	if (s_GoneFrame == g_Vars.lvframenum) {
		return 0;
	}

	s_GoneFrame = g_Vars.lvframenum;
	sysLogPrintf(LOG_NOTE, "net: counter-op: no agent is left in the room: the mission is the counter-ops' (tick %u)", g_NetTick);

	return 1;
}

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
	s32 bn;
	s32 i;

	if (g_NetMode != NETMODE_SERVER || !s_HostMatch || pn < 0 || pn >= MAX_PLAYERS || !(s_JoinNear & (1u << pn))) {
		return 0;
	}

	s_JoinNear &= ~(1u << pn);
	bn = coopRespawnBuddy(pn);

	if (!netCoopStackable(bn, pn)) {
		// the healthiest is in a vehicle or off the ground: any other on its feet
		for (bn = 0; bn < MAX_PLAYERS && !netCoopStackable(bn, pn); bn++) {
		}
	}

	if (bn >= MAX_PLAYERS) {
		sysLogPrintf(LOG_NOTE, "net: co-op: player %d joined in progress; nobody living on foot to spawn on, the mission's spawn", pn);
		return 0;
	}

	buddy = g_Vars.players[bn];
	*pos = buddy->prop->pos;

	for (i = 0; i < 7 && buddy->prop->rooms[i] != -1; i++) {
		rooms[i] = buddy->prop->rooms[i];
	}

	rooms[i] = -1;

	*turnanglerad = buddy->vv_theta * (M_BADTAU / 360.0f);
	sysLogPrintf(LOG_NOTE, "net: co-op: player %d joined in progress; spawns on player %d at %.0f %.0f %.0f (room %d)",
			pn, bn, pos->x, pos->y, pos->z, rooms[0]);

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
			bits |= i == netCoopLostSubject() ? MISP_LASTDEATH : 0;
			bits |= netCoopRespawnDue(i) ? MISP_RESPAWNDUE : 0;
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

	// online counter-op (protocol 32): the sides, and each counter-op's guard
	put16(body + MIS_ANTIMASK, netCoopHostAntiMask());

	for (i = 0; i < MAX_PLAYERS; i++) {
		s32 antibody = -1;
		s32 antihead = -1;
		const f32 radius = coopAntiRadius(i);

		if (!coopIsAntiNum(i) || !coopAntiGetBody(i, &antibody, &antihead)) {
			antibody = antihead = -1;
		}

		body[MIS_ANTISPAWN + i] = (u8)coopAntiSpawnCount(i);
		body[MIS_ANTIRAD + i] = (u8)(radius < 0 ? 0 : radius > 255 ? 255 : radius);
		put16(body + MIS_ANTIBODY + i * 2, (u16)(s16)antibody);
		put16(body + MIS_ANTIHEAD + i * 2, (u16)(s16)antihead);
	}

	if (netCoopHostAnti()) {
		memcpy(body + MIS_ANTITAKEN, s_HostAntiTaken, sizeof(s_HostAntiTaken));
	}

	// the Cinema page's cinema the room watches (protocol 27)
	if (s_HostMatch && s_HostCine.kind) {
		u8 cflags = 0;

		body[MIS_CINE] = (u8)s_HostCine.kind;

		if (s_HostCine.kind == GECINEMA_NET_CREDITS) {
			struct gecreditsnet cn;

			gecreditsNetState(&cn);
			cflags |= cn.orbit ? MISC_ORBIT : 0;
			cflags |= cn.state == 1 ? MISC_ROLLING : cn.state == 2 ? MISC_ROLLED : 0;
			put16(body + MIS_CREDPAD, (u16)cn.padnum);
			put16(body + MIS_CREDDIST, (u16)(s16)cn.distance);
			put16(body + MIS_CREDHIGH, (u16)(s16)cn.height);
			put16(body + MIS_CREDLOOK, (u16)(s16)cn.lookheight);
			putf(body + MIS_CREDSPEED, cn.speed);
			putf(body + MIS_CREDANGLE, cn.angle);
			putf(body + MIS_CREDFRAME, cn.frame);
		} else {
			s32 shot;
			s32 seq;
			s32 gflags;

			gecinemaNetState(&shot, &seq, &gflags);
			body[MIS_CINESHOT] = (u8)shot;
			body[MIS_CINESEQ] = (u8)seq;
			cflags |= (gflags & GECINEMA_NETF_LEFT) ? MISC_LEFT : 0;
		}

		body[MIS_CINEFLAGS] = cflags;
	}
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

/**
 * modrespawn.c modRespawnReset, every stage: no deaths yet, nobody's due back,
 * the mission not lost
 */
void netCoopStageReset(void)
{
	s_CoopDeaths = 0;
	memset(s_RespawnDue, 0, sizeof(s_RespawnDue));
	s_LocalRespawnDue = 0;
	s_LostSubject = -1;
}

/**
 * player.c playerDieByShooter on the host, before the guns are dropped:
 * whether this death comes back. Online co-op plays by Mission Respawn's
 * rules (the user, 2026-10-08: "switch co-op respawn to Mission Respawn's
 * rules"), not Perfect Dark's co-op one (START, half a living player's
 * health): with the host's Mission Respawn on, a death is a new life where
 * the player fell, once the fade is black, with full health and the kit it
 * had - so nothing is dropped - while Mission Lives lasts, counted for the
 * team (with five, the fifth death of the mission is the one that does not
 * come back). A death that does not come back drops its guns as co-op's
 * does, and its player watches the others (netspec.c). A seat emptied by a
 * leaver is no death.
 */
s32 netCoopDeathRespawns(void)
{
	const s32 pn = g_Vars.currentplayernum;
	const s32 lives = modGetMissionLives();
	s32 due;

	if (g_NetMode != NETMODE_SERVER || !s_HostMatch || g_Vars.normmplayerisrunning || g_Vars.coopplayernum < 0
			|| pn < 0 || pn >= MAX_PLAYERS || netSessionVacating() || netPlayerOutOfPlay(pn)
			// a counter-op comes back by its own rule, a new guard at a press
			// (player.c), and spends none of the agents' lives
			|| coopIsAntiNum(pn)) {
		return 0;
	}

	s_CoopDeaths++;
	due = modIsMissionRespawnOn() && (lives == MODLIVES_UNLIMITED || s_CoopDeaths < lives);
	s_RespawnDue[pn] = due;

	if (due) {
		sysLogPrintf(LOG_NOTE, "net: co-op: player %d died (the mission's death %d, tick %u): a new life where it fell (Mission Respawn, %s)",
				pn, s_CoopDeaths, g_NetTick, lives == MODLIVES_UNLIMITED ? "lives unlimited" : "lives left after it");
	} else {
		sysLogPrintf(LOG_NOTE, "net: co-op: player %d died (the mission's death %d, tick %u): out of the mission (%s)",
				pn, s_CoopDeaths, g_NetTick, modIsMissionRespawnOn() ? "Mission Lives spent" : "Mission Respawn off");
	}

	return due;
}

/**
 * That player is dead and its death comes back: the host's word (the client
 * knows its own player's alone, from the mission block). 0 offline.
 */
s32 netCoopRespawnDue(s32 playernum)
{
	struct player *p;

	if (playernum < 0 || playernum >= MAX_PLAYERS || !(p = g_Vars.players[playernum]) || !p->isdead
			|| g_Vars.normmplayerisrunning || g_Vars.coopplayernum < 0) {
		return 0;
	}

	if (g_NetMode == NETMODE_SERVER) {
		return s_RespawnDue[playernum];
	}

	return g_NetMode == NETMODE_CLIENT && playernum == g_NetLocalSlot && s_LocalRespawnDue;
}

/** The host: a dead player in play will come back (the mission is not lost while one will) */
s32 netCoopAnyRespawnDue(void)
{
	s32 i;

	if (g_NetMode != NETMODE_SERVER) {
		return 0;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (netCoopRespawnDue(i) && !netPlayerOutOfPlay(i)) {
			return 1;
		}
	}

	return 0;
}

/**
 * netents.c, a client's new life from the host's: whether it is Mission
 * Respawn's (where the player fell, with its kit), taken once
 */
s32 netCoopTakeLocalRespawn(void)
{
	const s32 due = s_LocalRespawnDue;

	s_LocalRespawnDue = 0;

	return due;
}

/**
 * player.c, a player's death (the host): the one that leaves nobody in play
 * alive, and nobody due back, loses the mission. Its fall goes on to
 * GoldenEye's replay on a converted mission (gedeathcam.c, for that player
 * alone: the others are watching already, netspec.c), and the host ends the
 * mission when that is over (geDeathCamLostHolds()). A seat emptied by a
 * leaver (netSeatVacate) is never the one.
 */
void netCoopPlayerDied(s32 playernum)
{
	if (g_NetMode != NETMODE_SERVER || !s_HostMatch || g_Vars.normmplayerisrunning || g_Vars.coopplayernum < 0
			|| playernum < 0 || playernum >= MAX_PLAYERS || netSessionVacating() || netPlayerOutOfPlay(playernum)
			// online counter-op: the agents' deaths lose it (coopAllDead()
			// asks the agents alone), a counter-op's never
			|| coopIsAntiNum(playernum)
			|| !coopAllDead() || netCoopAnyRespawnDue()) {
		return;
	}

	s_LostSubject = playernum;
	s_LostFrame = g_Vars.lvframenum;
	sysLogPrintf(LOG_NOTE, "net: co-op: player %d's death leaves nobody alive: the mission is lost (tick %u)%s",
			playernum, g_NetTick, modloaderStageIsMission(g_Vars.stagenum) ? "; GoldenEye's replay plays for it" : "");
}

/** The player whose death lost the mission, -1 while somebody lives */
s32 netCoopLostSubject(void)
{
	if (g_NetMode == NETMODE_NONE || s_LostSubject < 0 || s_LostSubject >= MAX_PLAYERS
			|| g_Vars.lvframenum < s_LostFrame || g_Vars.normmplayerisrunning || g_Vars.coopplayernum < 0
			|| !g_Vars.players[s_LostSubject] || !g_Vars.players[s_LostSubject]->isdead) {
		return -1;
	}

	return s_LostSubject;
}

/**
 * gedeathcam.c: GoldenEye's replay plays for this player's death online -
 * the mission's last, on the host (which poses the body everyone sees) and
 * on that player's own machine (which watches it). 0 offline.
 */
s32 netCoopReplaysDeath(s32 playernum)
{
	return playernum >= 0 && playernum == netCoopLostSubject()
		&& (g_NetMode == NETMODE_SERVER || playernum == g_NetLocalSlot);
}

/*
 * The block: client
 */

/**
 * Online counter-op (protocol 32): the sides as the host has them (a join in
 * progress may have changed a seat's), and each counter-op in the guard the
 * host put it in - its body built here when it changes (this machine's own
 * player too, once its new life is here: the next local block is then taken
 * outright, the body's build having put the prop on the ground), its walk's
 * radius the guard's at each new guard
 */
static void netCoopApplyAnti(const u8 *body)
{
	const u16 mask = get16(body + MIS_ANTIMASK);
	s32 i;

	if (!s_ClientAnti || mask == 0) {
		return;
	}

	netCoopApplyAntiTaken(body);

	if (mask != g_CoopAntiMask) {
		sysLogPrintf(LOG_NOTE, "net: counter-op client: the counter-ops are 0x%03x (were 0x%03x, tick %u)", mask, g_CoopAntiMask, g_NetTick);
		g_CoopAntiMask = mask;
	}

	for (i = 0; i < PLAYERCOUNT() && i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		const s32 bodynum = gets16(body + MIS_ANTIBODY + i * 2);
		const s32 headnum = gets16(body + MIS_ANTIHEAD + i * 2);
		const u8 spawns = body[MIS_ANTISPAWN + i];

		if (!p || !p->prop || !p->prop->chr || !(mask & (1u << i)) || bodynum < 0 || headnum < 0) {
			continue;
		}

		coopAntiSetBody(i, bodynum, headnum);

		if (p->isdead) {
			continue;
		}

		if (bodynum != s_AntiBuiltBody[i] || headnum != s_AntiBuiltHead[i]) {
			s_AntiBuiltBody[i] = bodynum;
			s_AntiBuiltHead[i] = headnum;
			s_AntiRebuilds++;
			netPupRebuildPlayerBody(i);

			if (i == g_NetLocalSlot) {
				netPredictForceSnap();
			}

			sysLogPrintf(LOG_NOTE, "net: counter-op client: player %d in a guard's body %d head %d (spawn %d, tick %u)",
					i, bodynum, headnum, spawns, g_NetTick);
		}

		if (spawns != s_AntiSpawnSeen[i]) {
			const f32 radius = body[MIS_ANTIRAD + i];

			s_AntiSpawnSeen[i] = spawns;

			if (radius > 0 && p->prop->chr) {
				p->prop->chr->radius = radius;

				if (i == g_NetLocalSlot) {
					p->bond2.radius = radius;
				}
			}
		}
	}
}

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

	if (body[MIS_CINE] > GECINEMA_NET_CREDITS) {
		return 0;
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

/**
 * A client: the host's Cinema-page cinema as its block carries it (protocol
 * 27) - the opening's gallery shot (gecinema.c follows it), or Cuba's camera
 * and roll (gecredits.c) - while this machine plays the cinema RULES named
 */
static void netCoopApplyCinema(const u8 *body)
{
	const s32 kind = body[MIS_CINE];
	const u8 cflags = body[MIS_CINEFLAGS];

	if (!s_ClientCine.kind || kind != s_ClientCine.kind || g_MainIsEndscreen) {
		return;
	}

	if (kind == GECINEMA_NET_CREDITS) {
		struct gecreditsnet cn;
		const s32 prev = g_Vars.currentplayernum;

		cn.orbit = (cflags & MISC_ORBIT) != 0;
		cn.padnum = gets16(body + MIS_CREDPAD);
		cn.distance = gets16(body + MIS_CREDDIST);
		cn.height = gets16(body + MIS_CREDHIGH);
		cn.lookheight = gets16(body + MIS_CREDLOOK);
		cn.speed = getf(body + MIS_CREDSPEED);
		cn.angle = getf(body + MIS_CREDANGLE);
		cn.state = (cflags & MISC_ROLLED) ? 2 : (cflags & MISC_ROLLING) ? 1 : 0;
		cn.frame = getf(body + MIS_CREDFRAME);

		if (g_NetLocalSlot >= 0 && g_NetLocalSlot < PLAYERCOUNT() && g_Vars.players[g_NetLocalSlot]) {
			setCurrentPlayerNum(g_NetLocalSlot);
			gecreditsNetFollow(&cn);
			setCurrentPlayerNum(prev);
		}

		return;
	}

	gecinemaNetFollow(body[MIS_CINESHOT], body[MIS_CINESEQ], (cflags & MISC_LEFT) ? GECINEMA_NETF_LEFT : 0);
}

/** The client: the host's word on whose death lost the mission */
static void netCoopApplyLost(const u8 *body)
{
	s32 subject = -1;
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (body[MIS_PLAYERS + i] & MISP_LASTDEATH) {
			subject = i;
			break;
		}
	}

	if (subject != s_LostSubject) {
		if (subject >= 0) {
			sysLogPrintf(LOG_NOTE, "net: co-op client: player %d's death lost the mission (tick %u)%s", subject, g_NetTick,
					subject == g_NetLocalSlot ? ": this machine's own" : "");
		}

		s_LostSubject = subject;
		s_LostFrame = g_Vars.lvframenum;
	}
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
	netCoopApplyCinema(body);

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

	netCoopApplyLost(body);
	netCoopApplyAnti(body);

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		const u8 bits = body[MIS_PLAYERS + i];

		if (!p) {
			continue;
		}

		p->aborted = (bits & MISP_ABORTED) != 0;
		p->coopcanrestart = (bits & MISP_CANRESTART) != 0;

		// held until this machine's new life takes it: the block that says
		// the player lives again may come before the local block's respawn,
		// and a block from before it after (only while dead here)
		if (i == g_NetLocalSlot && (bits & MISP_RESPAWNDUE) && !s_LocalRespawnDue && p->isdead) {
			s_LocalRespawnDue = 1;
			sysLogPrintf(LOG_NOTE, "net: co-op client: this machine's player comes back where it fell (tick %u)", g_NetTick);
		}

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

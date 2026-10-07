#include <stdio.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "modloader.h"
#include "game/bg.h"
#include "game/coop.h"
#include "game/lv.h"
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
 *                                 4 objective checks disabled
 *     2  s16 cutsceneanim         g_CutsceneAnimNum
 *     4  s32 cutsceneframe240     g_CutsceneCurAnimFrame240
 *     8  s16 tween60              g_CutsceneTweenDuration60
 *    10  s16 alarmtimer           g_AlarmTimer (clamped)
 *    12  f32 countdown60          g_CountdownTimerValue60
 *    16  u8  objectives[10]       objectiveCheck(i): 0 complete, 1 incomplete, 2 failed
 *    26  u8  players[12]          bit 0 isdead, 1 aborted, 2 coopcanrestart
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
#define MIS_SIZE      (MIS_PLAYERS + MAX_PLAYERS)

#define MISF_INCUTSCENE 0x01
#define MISF_COUNTDOWN  0x02
#define MISF_NOCHECKS   0x04

#define MISP_DEAD       0x01
#define MISP_ABORTED    0x02
#define MISP_CANRESTART 0x04

// player.c's (PC builds are NTSC: s32; no header declares them)
extern s32 g_CutsceneCurAnimFrame240;
extern s32 g_CutsceneTweenDuration60;

struct netcoopsetup g_NetCoopSetup = { 0, 0, DIFF_A, 1, 0 };

static s32 s_HostMatch = 0;       // host: the match H1 is starting (or running) is a co-op mission
static s32 s_ClientMatch = 0;     // client: RULES said a mission
static u8 s_ObjStatus[MAX_OBJECTIVES];
static s32 s_HaveObj = 0;
static u32 s_BlocksApplied = 0;
static u32 s_CutStarts = 0;
static u32 s_CutEnds = 0;
static s32 s_LoggedStart = 0;

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

s32 netCoopRulesOk(const struct netcooprules *r)
{
	return r->stageindex <= SOLOSTAGEINDEX_WAR && r->difficulty <= DIFF_PA;
}

// H3, from netRulesApply on a client: the host's mission
void netCoopClientApplyRules(const struct netcooprules *r)
{
	g_MissionConfig.iscoop = true;
	g_MissionConfig.isanti = false;
	g_MissionConfig.pdmode = false;
	g_MissionConfig.stageindex = r->stageindex;
	g_MissionConfig.stagenum = g_SoloStages[r->stageindex].stagenum;
	g_MissionConfig.difficulty = r->difficulty;
	g_Vars.coopradaron = r->radar ? true : false;
	g_Vars.coopfriendlyfire = r->friendlyfire ? true : false;
	g_Vars.numaibuddies = 0;
	s_ClientMatch = 1;

	{
		char name[48];

		sysLogPrintf(LOG_NOTE, "net: co-op: the host's mission is %s (index %d, stage 0x%02x) on difficulty %d, radar %d, friendly fire %d",
				netCoopClean(langGet(g_SoloStages[r->stageindex].name3), name, sizeof(name)), r->stageindex, g_MissionConfig.stagenum,
				r->difficulty, r->radar, r->friendlyfire);
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
s32 netCoopHostStart(s32 stageindex, s32 difficulty, s32 radar, s32 friendlyfire)
{
	s32 numplayers;
	s32 stagenum;

	if (g_NetMode != NETMODE_SERVER) {
		return 0;
	}

	if (stageindex < 0 || stageindex > SOLOSTAGEINDEX_WAR || difficulty < DIFF_A || difficulty > DIFF_PA) {
		sysLogPrintf(LOG_WARNING, "net: co-op: mission index %d / difficulty %d cannot be played online", stageindex, difficulty);
		return 0;
	}

	stagenum = g_SoloStages[stageindex].stagenum;

	g_MissionConfig.iscoop = true;
	g_MissionConfig.isanti = false;
	g_MissionConfig.pdmode = false;
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

		sysLogPrintf(LOG_NOTE, "net: co-op: starting %s (index %d, stage 0x%02x) on %s with %d players, radar %d, friendly fire %d",
				netCoopClean(netCoopMissionName(stageindex), name, sizeof(name)), stageindex, stagenum,
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

	if (!g_MissionConfig.iscoop || g_MissionConfig.isanti || g_Vars.numaibuddies > 0
			|| stageindex > SOLOSTAGEINDEX_WAR || g_SoloStages[stageindex].stagenum != g_MissionConfig.stagenum
			|| modloaderStageIsRemake(g_MissionConfig.stagenum)) {
		netSessionNoticeSet("Online, a mission is played Co-Operative with a human buddy.");
		sysLogPrintf(LOG_NOTE, "net: co-op: refused to start mission 0x%02x online (coop %d, anti %d, AI buddies %d)",
				g_MissionConfig.stagenum, g_MissionConfig.iscoop, g_MissionConfig.isanti, g_Vars.numaibuddies);
		return 1;
	}

	netCoopHostStart(stageindex, g_MissionConfig.difficulty, g_Vars.coopradaron, g_Vars.coopfriendlyfire);
	return 1;
}

// --net-test-coop INDEX (with --net-test-difficulty, --net-test-coop-ff):
// the gates' mission in place of a match; -1 when not asked for
s32 netCoopTestStart(void)
{
	const s32 index = sysArgGetInt("--net-test-coop", -1);

	if (index < 0) {
		return 0;
	}

	return netCoopHostStart(index, sysArgGetInt("--net-test-difficulty", DIFF_A), 1, sysArgCheck("--net-test-coop-ff"));
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

// H12: the match's flags off
void netCoopMatchStopped(void)
{
	if (g_NetMode == NETMODE_CLIENT && s_ClientMatch) {
		sysLogPrintf(LOG_NOTE, "net: co-op client: %u mission blocks applied, %u cutscene starts, %u ends", s_BlocksApplied, s_CutStarts, s_CutEnds);
	}

	s_HostMatch = 0;
	s_ClientMatch = 0;
	s_HaveObj = 0;
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

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		s_ObjStatus[i] = body[MIS_OBJ + i];
	}

	s_HaveObj = 1;
	g_ObjectiveChecksDisabled = (body[MIS_FLAGS] & MISF_NOCHECKS) != 0;
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

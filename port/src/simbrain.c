/**
 * Mod.SimBrain: stock's decisions, the navmesh's legs (PLANS/AI-REWORK.md M3).
 * port/include/simbrain.h has the overview; this is the adapter between
 * stock's go-pos action (chraction.c) and the agents on the stage's mesh
 * (port/src/simnavagents.cpp).
 *
 * A simulant's life along a path:
 *
 *  - bot.c decides where to go and calls chrGoToRoomPos(), as it always has.
 *    Stock routes it over its waypoints as it always has (so stock can take
 *    over at any moment), and then simbrainAfterGoTo() asks the mesh for a
 *    path to the same end - queued, at most SIMBRAIN_QUERIES a frame, oldest
 *    first; a goal that has moved only a little (a chase re-plans every
 *    second) keeps its corridor and moves its end instead.
 *  - Each tick of ACT_GOPOS, simbrainTickGoPos() moves the corridor to the
 *    simulant's feet, finds the corners ahead and fills a simcontrol: the
 *    point to walk to (a corner, bent by local avoidance of the chrs near
 *    it), the speed, whether to duck. chrTickGoPos() hands the point to
 *    stock's chrNavTickMain(), which turns the simulant to it and walks round
 *    props the mesh does not know (crates, doors, other chrs) as it always
 *    has. Until the path comes back, stock's route follower has the tick.
 *  - A link at the head of the corridor is crossed by its own small state
 *    machine: a drop is walked off, a jump jumped (botTryJump()) from its
 *    edge, a ladder walked into (the chr code climbs it; down is walking off
 *    the head, as stock goes down one), and a lift waited for, boarded,
 *    ridden and left, the way chrGoPosUpdateLiftAction() takes one.
 *  - Anything that goes wrong - off the corridor, no headway for
 *    SIMBRAIN_NOPROGRESS, a link that takes too long - asks for a new path.
 *    The mesh learns from it for everybody: the way from the polygon a
 *    simulant was stuck on to the next is priced up (and at a second
 *    complaint as good as closed), a link that failed twice is taken out,
 *    and a lift link to a side of the shaft with no door is taken out as
 *    soon as a path shows one. A goal the mesh has no floor under or no way
 *    to, a simulant that keeps losing its path, or one stuck three times
 *    running, goes back to stock's route follower for SIMBRAIN_STOCKTIME.
 *    The new code never holds a simulant anywhere stock would not.
 *
 * All per-simulant state is in g_SimBrain.sims, by the simulant's bot
 * number (its config's index); nothing is added to chrdata or aibot. There is
 * no random number here at all, so a seeded match replays.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "system.h"
#include "game/bot.h"
#include "game/chraction.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/modoptions.h"
#include "simnav.h"
#include "simbrain.h"

#define SIMBRAIN_QUERIES      4           // path queries run a frame
#define SIMBRAIN_CORNERAGE    4           // ticks between looking for the corners ahead
#define SIMBRAIN_NOPROGRESS   TICKS(150)  // moved less than 60 in this long: stuck, and a new path
#define SIMBRAIN_MAXSTUCK     2           // new paths for want of headway before stock takes over
#define SIMBRAIN_STOCKTIME    TICKS(240)  // how long stock has a simulant it took over
#define SIMBRAIN_FAILTIME     TICKS(300)  // a goal the mesh could not reach is not asked again for this long
#define SIMBRAIN_SAMEGOAL     150.0f      // a goal moved less than this keeps its corridor
#define SIMBRAIN_AVOIDRANGE   300.0f      // chrs within this are avoided
#define SIMBRAIN_MESHSPEED    5.0f        // the speed the jump links were found at (simnavGetLinkParams())

enum {
	SB_IDLE,    // no path asked for
	SB_FOLLOW,  // along the corridor (or waiting for it)
	SB_LINK,    // crossing a link
};

enum {
	LIFT_TOSTART,
	LIFT_WAITCAR,
	LIFT_BOARD,
	LIFT_RIDE,
	LIFT_OFF,
};

struct simbrainsim {
	s32 state;
	s32 stockuntil60;   // stock's route follower has it until this frame
	f32 failgoal[3];    // the last goal the mesh could not reach...
	s32 failframe60;    // ...and when
	s32 cornerage;
	struct simnavsteer steer;
	s32 havesteer;
	f32 progresspos[3]; // where it was when last it made headway...
	s32 progress60;     // ...and when
	s32 stuck;
	s32 replanwindow60; // new paths asked for since this frame...
	s32 replans;        // ...this many
	s32 trace30;        // PD_SIMBRAIN_TRACE: the half second it was traced last
	// a link being crossed
	u8 linkarea;
	u8 phase;
	u8 jumped;
	u8 replan;          // a new goal came while crossing: ask for its path after
	f32 linkstart[3];
	f32 linkend[3];
	s32 link60;
	struct liftobj *lift;
};

s32 g_SimBrainModern = false;

static struct {
	struct simnavagents *agents;
	s32 pumpframe;
	s32 numstock;       // simulants handed back to stock this match, for the log
	s32 numlinks;
	s32 crossed[SIMNAV_NUMAREAS][2]; // links crossed, by area, down and up
	s32 failed[SIMNAV_NUMAREAS];     // and given up on
	s32 logframe;
	s32 trace;          // PD_SIMBRAIN_TRACE: each simulant's state every half second, on stdout
	struct simbrainsim sims[MAX_BOTS];
} g_SimBrain;

/*
 * The option
 */

static char g_SimBrainOpt[16] = "stock";

PD_CONSTRUCTOR static void simbrainConfigInit(void)
{
	configRegisterString("Mod.SimBrain", g_SimBrainOpt, sizeof(g_SimBrainOpt));
}

s32 simbrainGetOption(void)
{
	return strcasecmp(g_SimBrainOpt, "modern") == 0;
}

void simbrainSetOption(s32 modern)
{
	snprintf(g_SimBrainOpt, sizeof(g_SimBrainOpt), "%s", modern ? "modern" : "stock");
}

s32 simbrainWanted(void)
{
	const char *arg = sysArgGetString("--simbrain");

	if (arg) {
		return strcasecmp(arg, "modern") == 0;
	}

	return simbrainGetOption();
}

/*
 * The match
 */

void simbrainStageStart(const struct simnavmesh *mesh)
{
	s32 i;

	simbrainStageStop();

	if (!mesh || !g_Vars.normmplayerisrunning || !simbrainWanted()) {
		return;
	}

	g_SimBrain.agents = simnavAgentsCreate(mesh, MAX_BOTS);

	if (!g_SimBrain.agents) {
		sysLogPrintf(LOG_WARNING, "simbrain: no agents; stock simulants this match");
		return;
	}

	// Costs a unit of distance through each area, so a path weighs the time
	// a stretch takes: a duck walks at half speed, a crouch at 0.35
	// (botCalculateMaxSpeed()); a lift is waited for, ten seconds and more
	// with its doors (Grid); a ladder is slow
	simnavAgentsSetAreaCost(g_SimBrain.agents, SIMNAV_AREA_DUCK, 2.0f);
	simnavAgentsSetAreaCost(g_SimBrain.agents, SIMNAV_AREA_CROUCH, 3.0f);
	simnavAgentsSetAreaCost(g_SimBrain.agents, SIMNAV_AREA_WATER, 1.5f);
	simnavAgentsSetAreaCost(g_SimBrain.agents, SIMNAV_AREA_LADDER, 2.0f);
	simnavAgentsSetAreaCost(g_SimBrain.agents, SIMNAV_AREA_LIFT, 10.0f);
	simnavAgentsSetAreaCost(g_SimBrain.agents, SIMNAV_AREA_JUMP, 1.5f);

	g_SimBrain.pumpframe = -1;
	g_SimBrain.trace = getenv("PD_SIMBRAIN_TRACE") != NULL;

	for (i = 0; i < MAX_BOTS; i++) {
		g_SimBrain.sims[i].failframe60 = -TICKS(100000);
	}
	g_SimBrainModern = true;
	sysLogPrintf(LOG_NOTE, "simbrain: modern simulant movement this match");
}

/** How the match has gone so far, into the log */
static void simbrainLogCounts(const char *when)
{
	sysLogPrintf(LOG_NOTE, "simbrain: %s: %d path queries, %d links taken, %d hand-backs to stock; crossed "
			"(down/up, failed): ladder %d/%d %d, lift %d/%d %d, drop %d %d, jump %d/%d %d", when,
			simnavAgentsQueriesRun(g_SimBrain.agents), g_SimBrain.numlinks, g_SimBrain.numstock,
			g_SimBrain.crossed[SIMNAV_AREA_LADDER][0], g_SimBrain.crossed[SIMNAV_AREA_LADDER][1], g_SimBrain.failed[SIMNAV_AREA_LADDER],
			g_SimBrain.crossed[SIMNAV_AREA_LIFT][0], g_SimBrain.crossed[SIMNAV_AREA_LIFT][1], g_SimBrain.failed[SIMNAV_AREA_LIFT],
			g_SimBrain.crossed[SIMNAV_AREA_DROP][0], g_SimBrain.failed[SIMNAV_AREA_DROP],
			g_SimBrain.crossed[SIMNAV_AREA_JUMP][0], g_SimBrain.crossed[SIMNAV_AREA_JUMP][1], g_SimBrain.failed[SIMNAV_AREA_JUMP]);
}

void simbrainStageStop(void)
{
	if (g_SimBrain.agents) {
		simbrainLogCounts("match over");
	}

	simnavAgentsFree(g_SimBrain.agents);
	memset(&g_SimBrain, 0, sizeof(g_SimBrain));
	g_SimBrainModern = false;
}

/*
 * A simulant
 */

static s32 simbrainIndex(struct chrdata *chr)
{
	s32 idx;

	if (!chr->aibot || !chr->aibot->config) {
		return -1;
	}

	idx = chr->aibot->config - g_BotConfigsArray;

	return idx >= 0 && idx < MAX_BOTS ? idx : -1;
}

bool simbrainOwns(struct chrdata *chr)
{
	s32 idx;

	if (!g_SimBrainModern || !chr->aibot) {
		return false;
	}

	idx = simbrainIndex(chr);

	return idx >= 0 && g_Vars.lvframe60 >= g_SimBrain.sims[idx].stockuntil60;
}

/** This frame's path queries, run by whichever simulant asks first */
static void simbrainPump(void)
{
	if (g_SimBrain.pumpframe != g_Vars.lvframe60) {
		g_SimBrain.pumpframe = g_Vars.lvframe60;
		simnavAgentsUpdate(g_SimBrain.agents, SIMBRAIN_QUERIES);
	}
}

static void simbrainFeet(struct chrdata *chr, f32 *feet)
{
	feet[0] = chr->prop->pos.x;
	feet[1] = chr->manground;
	feet[2] = chr->prop->pos.z;
}

static f32 simbrainDist2D(const f32 *a, const f32 *b)
{
	f32 dx = a[0] - b[0];
	f32 dz = a[2] - b[2];

	return sqrtf(dx * dx + dz * dz);
}

/**
 * The simulant's running speed, units a tick: botCalculateMaxSpeed() without
 * the slowing for ducking or for the last stretch of a route, which say
 * nothing of how far it can jump.
 */
static f32 simbrainRunSpeed(struct chrdata *chr)
{
	f32 speed = botCalculateMaxSpeed(chr);
	s32 crouchpos = botGuessCrouchPos(chr);

	if (crouchpos == CROUCHPOS_SQUAT) {
		speed /= 0.35f;
	} else if (crouchpos == CROUCHPOS_DUCK) {
		speed /= 0.5f;
	} else if (chr->actiontype == ACT_GOPOS
			&& chr->act_gopos.waypoints[chr->act_gopos.curindex] == NULL
			&& chrGetLateralDistanceToCoord(chr, &chr->act_gopos.endpos) < 200) {
		speed /= 0.5f;
	}

	return speed;
}

/**
 * What of the mesh this simulant may use. Jump links are found at the
 * slowest simulant's speed, and only a simulant at least that fast makes
 * them (the stat slider takes one down to a tenth); and only when the match
 * lets simulants jump, as high as it lets them.
 */
static u16 simbrainIncludeFlags(struct chrdata *chr)
{
	u16 flags = SIMNAV_FLAGS_NOJUMP;
	s32 h;

	if (modCanChrJump() && simbrainRunSpeed(chr) >= SIMBRAIN_MESHSPEED * 0.95f) {
		for (h = 1; h <= modGetJumpHeight() && h <= SIMNAV_MAXJUMPHEIGHTS; h++) {
			flags |= SIMNAV_FLAG_JUMP(h);
		}
	}

	return flags;
}

static void simbrainRequest(struct chrdata *chr, s32 idx, const f32 *end)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];
	f32 feet[3];

	simbrainFeet(chr, feet);
	simnavAgentRequest(g_SimBrain.agents, idx, feet, end, simbrainIncludeFlags(chr));

	// headway is watched from here, but not afresh at each new path of one
	// already following (a chase asks for one every second or so)
	if (sim->state != SB_FOLLOW) {
		sim->progress60 = g_Vars.lvframe60;
		memcpy(sim->progresspos, feet, sizeof(sim->progresspos));
	}

	sim->state = SB_FOLLOW;
	sim->cornerage = 0;
	sim->havesteer = false;
}

static void simbrainClearWait(struct chrdata *chr)
{
	chr->act_gopos.flags &= ~GOPOSFLAG_WAITING;
}

/**
 * Stock's route follower has the simulant for a while: its route is planned
 * afresh from where the simulant is, as stock would on being stuck.
 */
static void simbrainToStock(struct chrdata *chr, s32 idx, const char *why)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];

	sim->stockuntil60 = g_Vars.lvframe60 + SIMBRAIN_STOCKTIME;
	sim->state = SB_IDLE;
	sim->stuck = 0;
	simnavAgentClear(g_SimBrain.agents, idx);
	g_SimBrain.numstock++;

	if (chr->actiontype == ACT_GOPOS) {
		simbrainClearWait(chr);
		chrGoToRoomPos(chr, &chr->act_gopos.endpos, chr->act_gopos.endrooms, chr->act_gopos.flags);
	}

	if (g_SimBrain.trace) {
		printf("SB f%d b%d to stock: %s at %.0f,%.0f,%.0f end %.0f,%.0f,%.0f\n", g_Vars.lvframe60, idx, why,
				chr->prop->pos.x, chr->manground, chr->prop->pos.z,
				chr->act_gopos.endpos.x, chr->act_gopos.endpos.y, chr->act_gopos.endpos.z);
	}
}

bool simbrainHoldsGoTo(struct chrdata *chr, struct coord *pos, RoomNum *rooms)
{
	struct simbrainsim *sim;
	s32 idx;

	if (!simbrainOwns(chr) || (idx = simbrainIndex(chr)) < 0) {
		return false;
	}

	sim = &g_SimBrain.sims[idx];

	if (sim->state != SB_LINK || chr->actiontype != ACT_GOPOS || chrIsDead(chr)) {
		return false;
	}

	{
		// not a link left behind by a death and a respawn elsewhere
		f32 feet[3];

		simbrainFeet(chr, feet);

		if (simbrainDist2D(feet, sim->linkstart) > 1500.0f || g_Vars.lvframe60 - sim->link60 > TICKS(1500)) {
			sim->state = SB_IDLE;
			return false;
		}
	}

	// crossing: the new end is taken up on the other side
	chr->act_gopos.endpos = *pos;
	roomsCopy(rooms, chr->act_gopos.endrooms);
	sim->replan = true;

	return true;
}

bool simbrainAfterGoTo(struct chrdata *chr, struct coord *pos, RoomNum *rooms, u32 goposflags, bool stockrouted)
{
	struct simbrainsim *sim;
	f32 end[3];
	f32 goal[3];
	s32 idx;
	s32 status;
	s32 complete;

	if (!simbrainOwns(chr) || (idx = simbrainIndex(chr)) < 0 || chrIsDead(chr)) {
		return stockrouted;
	}

	sim = &g_SimBrain.sims[idx];
	simbrainPump();

	end[0] = pos->x;
	end[1] = pos->y;
	end[2] = pos->z;

	// a goal the mesh could not reach a moment ago: stock's, for now
	if (g_Vars.lvframe60 - sim->failframe60 < SIMBRAIN_FAILTIME
			&& simbrainDist2D(end, sim->failgoal) < SIMBRAIN_SAMEGOAL
			&& fabsf(end[1] - sim->failgoal[1]) < 200.0f) {
		simnavAgentClear(g_SimBrain.agents, idx);
		sim->state = SB_IDLE;
		return stockrouted;
	}

	status = simnavAgentStatus(g_SimBrain.agents, idx, &complete, goal);

	if (!stockrouted) {
		// Stock found no route - its nearest waypoint to both ends is the
		// same one, or there is none - and does not move. Still on its way
		// to much the same end, the simulant carries on to the new one.
		if (chr->actiontype == ACT_GOPOS && sim->state == SB_FOLLOW && status == SIMNAV_AGENT_FOLLOWING
				&& simbrainDist2D(end, goal) < SIMBRAIN_SAMEGOAL && fabsf(end[1] - goal[1]) < 200.0f
				&& simnavAgentMoveGoal(g_SimBrain.agents, idx, end)) {
			chr->act_gopos.endpos = *pos;
			roomsCopy(rooms, chr->act_gopos.endrooms);
			return true;
		}

		// Otherwise it walks there if the mesh has a whole path now.
		simbrainRequest(chr, idx, end);

		if (simnavAgentRunNow(g_SimBrain.agents, idx) == SIMNAV_AGENT_FOLLOWING
				&& simnavAgentStatus(g_SimBrain.agents, idx, &complete, NULL) == SIMNAV_AGENT_FOLLOWING
				&& complete) {
			chrGoPosStartBare(chr, pos, rooms, goposflags);
			return true;
		}

		simnavAgentClear(g_SimBrain.agents, idx);
		sim->state = SB_IDLE;
		return false;
	}

	// Stock has set out on a route: the mesh's path to the same end
	if (sim->state == SB_FOLLOW && status == SIMNAV_AGENT_FOLLOWING
			&& simbrainDist2D(end, goal) < SIMBRAIN_SAMEGOAL && fabsf(end[1] - goal[1]) < 200.0f
			&& simnavAgentMoveGoal(g_SimBrain.agents, idx, end)) {
		return true;
	}

	simbrainRequest(chr, idx, end);

	return true;
}

/*
 * Following
 */

/** The lift a lift link crosses: the one whose car stands nearest both its ends */
static struct liftobj *simbrainFindLift(const f32 *a, const f32 *b)
{
	struct liftobj *best = NULL;
	f32 bestdist = 900.0f;
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_Lifts); i++) {
		struct prop *prop = g_Lifts[i];
		f32 pos[3];
		f32 dist;

		if (!prop || !prop->obj) {
			continue;
		}

		pos[0] = prop->pos.x;
		pos[1] = prop->pos.y;
		pos[2] = prop->pos.z;
		dist = simbrainDist2D(pos, a) + simbrainDist2D(pos, b);

		if (dist < bestdist) {
			bestdist = dist;
			best = (struct liftobj *)prop->obj;
		}
	}

	return best;
}

/**
 * Whether a lift link's end is a way on and off the car. The mesh's lift
 * links join every side of the shaft that has floor at a stop, and a car with
 * doors is got on and off only through them: Grid's lift had its sides
 * behind walls linked too, and a simulant waited at them for a car it could
 * never board.
 */
static bool simbrainLiftEndUsable(struct liftobj *lift, const f32 *end)
{
	struct doorobj *best = NULL;
	f32 bestdy = 150.0f;
	s32 i;

	for (i = 0; i < ARRAYCOUNT(lift->doors); i++) {
		struct doorobj *door = lift->doors[i];
		f32 dy;

		if (!door || !door->base.prop) {
			continue;
		}

		dy = fabsf(door->base.prop->pos.y - (end[1] + 100.0f));

		if (dy < bestdy) {
			bestdy = dy;
			best = door;
		}
	}

	if (!best) {
		// no door at this stop: any side will do
		return true;
	}

	{
		f32 doorpos[3] = { best->base.prop->pos.x, best->base.prop->pos.y, best->base.prop->pos.z };
		return simbrainDist2D(end, doorpos) < 180.0f;
	}
}

static bool simbrainLiftLinkUsable(const f32 *start, const f32 *end)
{
	struct liftobj *lift = simbrainFindLift(start, end);

	return lift && simbrainLiftEndUsable(lift, start) && simbrainLiftEndUsable(lift, end);
}

static void simbrainStartLink(struct chrdata *chr, s32 idx, u8 area, const f32 *start, const f32 *end)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];

	sim->state = SB_LINK;
	sim->linkarea = area;
	sim->phase = 0;
	sim->jumped = false;
	sim->replan = false;
	sim->link60 = g_Vars.lvframe60;
	memcpy(sim->linkstart, start, sizeof(sim->linkstart));
	memcpy(sim->linkend, end, sizeof(sim->linkend));
	sim->lift = area == SIMNAV_AREA_LIFT ? simbrainFindLift(start, end) : NULL;
	g_SimBrain.numlinks++;
}

/** Done with a link: along the corridor again, or a new path if the goal moved */
static void simbrainEndLink(struct chrdata *chr, s32 idx)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];

	g_SimBrain.crossed[sim->linkarea][sim->linkend[1] > sim->linkstart[1] + 30.0f]++;
	simbrainClearWait(chr);
	sim->state = SB_FOLLOW;
	sim->cornerage = 0;
	sim->havesteer = false;
	sim->stuck = 0;
	sim->progress60 = g_Vars.lvframe60;
	simbrainFeet(chr, sim->progresspos);

	if (sim->replan) {
		f32 end[3] = { chr->act_gopos.endpos.x, chr->act_gopos.endpos.y, chr->act_gopos.endpos.z };
		sim->replan = false;
		simbrainRequest(chr, idx, end);
	}
}

/**
 * Stuck while over an edge: caught on a ledge's lip on the way down, where
 * the edge rule (chr.c, chrHasFloorBelow()) holds every step. Stock's
 * progress watch lets such a simulant step for a second
 * (chrGoPosWatchProgress()), and so does this one's; on Villa one hung at
 * (-2189, -495, 1282) over a floor 300 below for the rest of the match.
 */
static void simbrainFreeFromLip(struct chrdata *chr)
{
	if (chr->fallspeed.y != 0.0f || chr->manground > chr->ground + 5.0f) {
		chr->aibot->navedgefree60 = g_Vars.lvframe60 + TICKS(60);
	}
}

/** Whether another chr stands within 80 of this one's feet */
static bool simbrainCrowded(struct chrdata *chr, const f32 *feet)
{
	s32 i;

	for (i = 0; i < g_MpNumChrs && i < MAX_MPCHRS; i++) {
		struct chrdata *other = g_MpAllChrPtrs[i];

		if (other && other != chr && other->prop && !chrIsDead(other)
				&& fabsf(other->prop->pos.x - feet[0]) < 80.0f && fabsf(other->prop->pos.z - feet[2]) < 80.0f
				&& fabsf(other->prop->pos.y - chr->prop->pos.y) < 150.0f) {
			return true;
		}
	}

	return false;
}

/** A new path from here to the same end, after something went wrong */
static void simbrainReplan(struct chrdata *chr, s32 idx)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];
	f32 end[3] = { chr->act_gopos.endpos.x, chr->act_gopos.endpos.y, chr->act_gopos.endpos.z };

	// one that keeps losing its path is somewhere the mesh does not
	// describe (a prop it stands on, a gap it was pushed through): stock's
	if (g_Vars.lvframe60 - sim->replanwindow60 > TICKS(120)) {
		sim->replanwindow60 = g_Vars.lvframe60;
		sim->replans = 0;
	}

	if (++sim->replans > 4) {
		simbrainToStock(chr, idx, "lost its path");
		return;
	}

	simbrainClearWait(chr);
	simnavAgentClear(g_SimBrain.agents, idx);
	simbrainRequest(chr, idx, end);
}

static void simbrainCopyAim(struct coord *aim, const f32 *p)
{
	aim->x = p[0];
	aim->y = p[1];
	aim->z = p[2];
}

/**
 * Crossing a link. Returns a SIMBRAIN_TICK_*, with the point to walk to in
 * aim; ctl->wait to stand.
 */
static s32 simbrainTickLink(struct chrdata *chr, s32 idx, struct simcontrol *ctl)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];
	s32 elapsed = g_Vars.lvframe60 - sim->link60;
	f32 feet[3];
	f32 toend;
	f32 tostart;
	bool grounded;
	s32 timeout;

	simbrainFeet(chr, feet);
	toend = simbrainDist2D(feet, sim->linkend);
	tostart = simbrainDist2D(feet, sim->linkstart);
	grounded = chr->fallspeed.y == 0.0f && !chr->onladder && chr->manground - chr->ground < 20.0f;

	memcpy(ctl->aim, sim->linkend, sizeof(ctl->aim));
	ctl->speed = 1.0f;

	switch (sim->linkarea) {
	case SIMNAV_AREA_LADDER: timeout = TICKS(600); break;
	case SIMNAV_AREA_LIFT:   timeout = TICKS(1500); break;
	default:                 timeout = TICKS(240); break;
	}

	// over: on the floor at the far end (off the car, for a lift)
	if (grounded && fabsf(chr->manground - sim->linkend[1]) < 60.0f && toend < 120.0f
			&& (sim->linkarea != SIMNAV_AREA_LIFT || (sim->phase == LIFT_OFF && !chr->inlift))
			&& (sim->linkarea != SIMNAV_AREA_JUMP || sim->jumped || fabsf(sim->linkend[1] - sim->linkstart[1]) > 60.0f)) {
		simbrainEndLink(chr, idx);
		return SIMBRAIN_TICK_WALK;
	}

	// taking too long, or landed on a floor that is neither end
	if (elapsed > timeout
			|| (grounded && elapsed > TICKS(30) && sim->linkarea != SIMNAV_AREA_LIFT
				&& fabsf(chr->manground - sim->linkend[1]) > 60.0f
				&& fabsf(chr->manground - sim->linkstart[1]) > 60.0f)) {
		sim->stuck++;
		simbrainFreeFromLip(chr);

		g_SimBrain.failed[sim->linkarea]++;

		if (simnavAgentLinkFailed(g_SimBrain.agents, idx) && g_SimBrain.trace) {
			printf("SB f%d b%d link %.0f,%.0f,%.0f -> %.0f,%.0f,%.0f (area %d) failed twice: taken out\n", g_Vars.lvframe60, idx,
					sim->linkstart[0], sim->linkstart[1], sim->linkstart[2], sim->linkend[0], sim->linkend[1], sim->linkend[2], sim->linkarea);
		}

		if (sim->stuck > SIMBRAIN_MAXSTUCK) {
			simbrainToStock(chr, idx, "link");
			return SIMBRAIN_TICK_STOCK;
		}

		simbrainReplan(chr, idx);
		return SIMBRAIN_TICK_STOCK;
	}

	// walked off an edge and hanging on its lip rather than falling
	if (sim->linkarea != SIMNAV_AREA_LIFT && elapsed > TICKS(45) && chr->fallspeed.y == 0.0f
			&& chr->manground > chr->ground + 20.0f && simbrainDist2D(feet, chr->prevpos.f) < 2.0f) {
		simbrainFreeFromLip(chr);
	}

	switch (sim->linkarea) {
	case SIMNAV_AREA_JUMP:
		// jump from the edge, still on the floor (botTryJump() refuses
		// in the air, and walking on off the edge is only a drop)
		// at a run: the link was found for a jump at walking speed. One
		// whose jump is not ready yet (botTryJump()'s cooldown) waits
		// short of the edge rather than walk off it.
		if (!sim->jumped) {
			if (botIsJumping(chr)) {
				sim->jumped = true;
			} else if (tostart < 30.0f || sim->phase) {
				sim->phase = 1;

				if (g_Vars.lvframe60 < chr->aibot->jumptimer60) {
					memcpy(ctl->aim, sim->linkstart, sizeof(ctl->aim));
					ctl->wait = true;
				} else {
					ctl->jump = true;
				}
			} else {
				memcpy(ctl->aim, sim->linkstart, sizeof(ctl->aim));
			}
		}
		break;
	case SIMNAV_AREA_LADDER:
		// Up: to the foot first, then straight at the head. Walking at the
		// head from wherever the link was taken caught the ladder at its
		// side, where the climb meets the floor above instead of the hole,
		// and on Warehouse a simulant hung half way up until the link
		// timed out
		if (sim->phase == 0 && sim->linkend[1] > sim->linkstart[1]) {
			if (tostart < 20.0f || (chr->onladder && chr->manground > sim->linkstart[1] + 40.0f)) {
				sim->phase = 1;
			} else {
				memcpy(ctl->aim, sim->linkstart, sizeof(ctl->aim));
			}
		}
		break;
	case SIMNAV_AREA_LIFT:
		if (sim->lift) {
			struct liftobj *lift = sim->lift;
			struct prop *liftprop = lift->base.prop;
			f32 lifty = liftGetY(lift);
			bool dooropen = !lift->doors[lift->levelcur] || lift->doors[lift->levelcur]->frac >= 0.5f;
			f32 car[3] = { liftprop->pos.x, lifty, liftprop->pos.z };
			bool atlevel = lifty <= chr->manground + 40.0f && lifty >= chr->manground - 150.0f;
			f32 waitat[3];
			f32 dx = sim->linkstart[0] - car[0];
			f32 dz = sim->linkstart[2] - car[2];
			f32 len = sqrtf(dx * dx + dz * dz);

			// The car is waited for a step back from the shaft: a lift's
			// door will not open on a chr standing in it (Grid's did not
			// for ten seconds)
			memcpy(waitat, sim->linkstart, sizeof(waitat));

			if (len > 1.0f) {
				waitat[0] += dx / len * 70.0f;
				waitat[2] += dz / len * 70.0f;
			}

			switch (sim->phase) {
			case LIFT_TOSTART:
				memcpy(ctl->aim, waitat, sizeof(ctl->aim));

				if (chr->inlift) {
					sim->phase = LIFT_RIDE;
				} else if (atlevel && dooropen && tostart < 100.0f) {
					sim->phase = LIFT_BOARD;
				} else if (simbrainDist2D(feet, waitat) < 30.0f) {
					sim->phase = LIFT_WAITCAR;
				}
				break;
			case LIFT_WAITCAR:
				memcpy(ctl->aim, car, sizeof(ctl->aim));
				ctl->aim[1] = chr->manground + 100.0f;
				ctl->wait = true;

				if ((elapsed % TICKS(30)) == 0) {
					struct coord callpos = { car[0], chr->manground + 50.0f, car[2] };
					chrOpenDoor(chr, &callpos);
				}

				if (atlevel && dooropen) {
					sim->phase = LIFT_BOARD;
				}
				break;
			case LIFT_BOARD:
				memcpy(ctl->aim, car, sizeof(ctl->aim));

				if (chr->inlift) {
					sim->phase = LIFT_RIDE;
				} else if (!atlevel) {
					sim->phase = LIFT_WAITCAR;
					ctl->wait = true;
				}
				break;
			case LIFT_RIDE:
				memcpy(ctl->aim, sim->linkend, sizeof(ctl->aim));
				ctl->wait = true;

				if (!chr->inlift && grounded) {
					// stepped or pushed off before its stop
					sim->phase = fabsf(chr->manground - sim->linkend[1]) < 60.0f ? LIFT_OFF : LIFT_TOSTART;
				} else if (fabsf(lifty - sim->linkend[1]) <= 30.0f && dooropen) {
					sim->phase = LIFT_OFF;
					ctl->wait = false;
				}
				break;
			case LIFT_OFF:
				break;
			}
		} else {
			sim->phase = LIFT_OFF;
		}
		break;
	default:
		// a drop walked off, a ladder walked into (and up, or off its head
		// and down): the chr code does the rest
		break;
	}

	// Not past the far end at a stride: it walks on through the air, and a
	// fast simulant dropping onto one of Pipes' pipes ran on off it into
	// the killing pit under them (14 falls in 12 matches at 500%, against
	// stock's 5)
	if (!ctl->wait && sim->linkarea != SIMNAV_AREA_LIFT) {
		f32 stride = simbrainRunSpeed(chr) * 4.0f * 1.2f;

		if (stride > 60.0f && toend < stride) {
			ctl->speed = toend / stride < 0.2f ? 0.2f : toend / stride;
		}
	}

	return SIMBRAIN_TICK_WALK;
}

/**
 * Local avoidance: the walk from feet to target bent round the chrs near it.
 * The speed in ctl is its share of the top speed.
 */
static void simbrainAvoid(struct chrdata *chr, s32 idx, const f32 *feet, const f32 *target, struct simcontrol *ctl)
{
	f32 neighbours[6 * 7];
	f32 nbdist[6];
	s32 numnb = 0;
	f32 speed = simbrainRunSpeed(chr) * 60.0f;
	f32 dvel[3];
	f32 vel[3];
	f32 nvel[3];
	f32 dx = target[0] - feet[0];
	f32 dz = target[2] - feet[2];
	f32 dist = sqrtf(dx * dx + dz * dz);
	f32 ticks = g_Vars.lvupdate60freal > 0.0f ? g_Vars.lvupdate60freal : 1.0f;
	f32 len;
	s32 i;
	s32 j;

	if (dist < 1.0f || speed <= 0.0f) {
		return;
	}

	for (i = 0; i < g_MpNumChrs && i < MAX_MPCHRS; i++) {
		struct chrdata *other = g_MpAllChrPtrs[i];
		f32 ox;
		f32 oz;
		f32 d;

		if (!other || other == chr || !other->prop || chrIsDead(other)) {
			continue;
		}

		ox = other->prop->pos.x - feet[0];
		oz = other->prop->pos.z - feet[2];

		if (fabsf(ox) > SIMBRAIN_AVOIDRANGE || fabsf(oz) > SIMBRAIN_AVOIDRANGE
				|| fabsf(other->prop->pos.y - chr->prop->pos.y) > 150.0f) {
			continue;
		}

		d = ox * ox + oz * oz;

		if (d > SIMBRAIN_AVOIDRANGE * SIMBRAIN_AVOIDRANGE) {
			continue;
		}

		// the nearest six, nearest first
		for (j = numnb; j > 0 && nbdist[j - 1] > d; j--) {
			if (j < 6) {
				nbdist[j] = nbdist[j - 1];
				memcpy(&neighbours[j * 7], &neighbours[(j - 1) * 7], sizeof(f32) * 7);
			}
		}

		if (j < 6) {
			f32 *nb = &neighbours[j * 7];

			nbdist[j] = d;
			nb[0] = other->prop->pos.x;
			nb[1] = feet[1];
			nb[2] = other->prop->pos.z;
			nb[3] = (other->prop->pos.x - other->prevpos.x) * 60.0f / ticks;
			nb[4] = 0.0f;
			nb[5] = (other->prop->pos.z - other->prevpos.z) * 60.0f / ticks;
			nb[6] = other->radius > 0.0f ? other->radius : 20.0f;

			if (numnb < 6) {
				numnb++;
			}
		}
	}

	if (numnb == 0) {
		return;
	}

	dvel[0] = dx / dist * speed;
	dvel[1] = 0.0f;
	dvel[2] = dz / dist * speed;
	vel[0] = (chr->prop->pos.x - chr->prevpos.x) * 60.0f / ticks;
	vel[1] = 0.0f;
	vel[2] = (chr->prop->pos.z - chr->prevpos.z) * 60.0f / ticks;

	if (!simnavAgentAvoid(g_SimBrain.agents, idx, feet, chr->radius > 0.0f ? chr->radius : 20.0f, speed,
				vel, dvel, neighbours, numnb, nvel)) {
		return;
	}

	len = sqrtf(nvel[0] * nvel[0] + nvel[2] * nvel[2]);

	if (len < speed * 0.1f) {
		// told to stand: keep going, slowly, rather than stall in a doorway
		if (ctl->speed > 0.25f) {
			ctl->speed = 0.25f;
		}

		return;
	}

	// the aim a short way along the new heading, at the target's height
	dist = dist < 60.0f ? 60.0f : dist > 150.0f ? 150.0f : dist;
	ctl->aim[0] = feet[0] + nvel[0] / len * dist;
	ctl->aim[2] = feet[2] + nvel[2] / len * dist;
	len = len / speed < 0.25f ? 0.25f : len / speed > 1.0f ? 1.0f : len / speed;

	if (len < ctl->speed) {
		ctl->speed = len;
	}
}

/** The tick along the corridor */
static s32 simbrainTickFollow(struct chrdata *chr, s32 idx, struct simcontrol *ctl)
{
	struct simbrainsim *sim = &g_SimBrain.sims[idx];
	struct simnavsteer *steer = &sim->steer;
	f32 feet[3];
	f32 target[3];
	s32 corners;
	s32 complete;
	s32 status;

	status = simnavAgentStatus(g_SimBrain.agents, idx, &complete, NULL);

	if (status == SIMNAV_AGENT_FAILED) {
		f32 end[3] = { chr->act_gopos.endpos.x, chr->act_gopos.endpos.y, chr->act_gopos.endpos.z };

		memcpy(sim->failgoal, end, sizeof(end));
		sim->failframe60 = g_Vars.lvframe60;
		simbrainToStock(chr, idx, simnavAgentFailReason(g_SimBrain.agents, idx) == 1 ? "no floor at start"
				: simnavAgentFailReason(g_SimBrain.agents, idx) == 2 ? "no floor at end" : "no path");
		return SIMBRAIN_TICK_STOCK;
	}

	if (status != SIMNAV_AGENT_FOLLOWING) {
		// waiting for its query: stock's route meanwhile
		return SIMBRAIN_TICK_STOCK;
	}

	if (!complete) {
		// The mesh stops short of the end - a pad on a crate, a player on
		// a prop. Stock's waypoints may know the way.
		f32 end[3] = { chr->act_gopos.endpos.x, chr->act_gopos.endpos.y, chr->act_gopos.endpos.z };

		memcpy(sim->failgoal, end, sizeof(end));
		sim->failframe60 = g_Vars.lvframe60;
		simbrainToStock(chr, idx, "short path");
		return SIMBRAIN_TICK_STOCK;
	}

	simbrainFeet(chr, feet);

	corners = !sim->havesteer || --sim->cornerage <= 0
		|| simbrainDist2D(feet, steer->corner) < 50.0f;

	if (!simnavAgentFollow(g_SimBrain.agents, idx, feet, corners, steer)) {
		// off its corridor: in the air it may yet land back on it
		if (chr->fallspeed.y == 0.0f && chr->manground - chr->ground < 20.0f
				&& !simnavAgentIsPending(g_SimBrain.agents, idx)) {
			simbrainReplan(chr, idx);
		}

		sim->havesteer = false;
		return SIMBRAIN_TICK_STOCK;
	}

	if (corners) {
		sim->havesteer = true;
		sim->cornerage = SIMBRAIN_CORNERAGE;
	}

	// ducking over the stretch ahead, as a pad flagged for it asks
	if (steer->aheadarea == SIMNAV_AREA_CROUCH) {
		ctl->crouch = true;
	} else if (steer->aheadarea == SIMNAV_AREA_DUCK) {
		ctl->duck = true;
	}

	// a lift link ahead that is no way onto the car: out of the mesh
	if (corners && steer->linkarea == SIMNAV_AREA_LIFT) {
		f32 start[3];
		f32 end[3];

		if (simnavAgentPeekLink(g_SimBrain.agents, idx, start, end) && !simbrainLiftLinkUsable(start, end)) {
			if (g_SimBrain.trace) {
				printf("SB f%d b%d lift link %.0f,%.0f,%.0f -> %.0f,%.0f,%.0f is no way onto the car: taken out\n",
						g_Vars.lvframe60, idx, start[0], start[1], start[2], end[0], end[1], end[2]);
			}

			simnavAgentDisableLink(g_SimBrain.agents, idx);
			simbrainReplan(chr, idx);
			return SIMBRAIN_TICK_STOCK;
		}
	}

	// at a link's start: cross it
	if (steer->linkarea != SIMNAV_AREA_NONE) {
		f32 tolink = simbrainDist2D(feet, steer->corner);
		f32 dy = fabsf(feet[1] - steer->corner[1]);
		bool reached = steer->linkarea == SIMNAV_AREA_LADDER
			? (tolink < 60.0f && dy < 200.0f) || (chr->onladder && tolink < 120.0f)
			: tolink < 40.0f && dy < 80.0f;

		if (reached) {
			f32 start[3];
			f32 end[3];
			u8 area;

			if (simnavAgentTakeLink(g_SimBrain.agents, idx, start, end, &area)) {
				simbrainStartLink(chr, idx, area, start, end);
				return simbrainTickLink(chr, idx, ctl);
			}
		}
	}

	// Headway: 60 from where it was 2.5 s ago. Not nearer its corner: at a
	// step the chr cannot climb (a riser flagged as a wall, which the mesh
	// merged into the floor above; G5 at (-570, -60)) the corner flipped
	// between the two floors' polygons every look, each nearer than the last.
	if (simbrainDist2D(feet, sim->progresspos) > 60.0f || fabsf(feet[1] - sim->progresspos[1]) > 60.0f) {
		memcpy(sim->progresspos, feet, sizeof(sim->progresspos));
		sim->progress60 = g_Vars.lvframe60;
		sim->stuck = 0;
	} else if (g_Vars.lvframe60 - sim->progress60 > SIMBRAIN_NOPROGRESS && !simnavAgentIsPending(g_SimBrain.agents, idx)) {
		sim->stuck++;
		simbrainFreeFromLip(chr);
		sim->progress60 = g_Vars.lvframe60;

		// the way from this polygon to the next is not one, whatever the
		// mesh says: dearer for every path after, and at a second
		// simulant's (or a second try's) word, as good as closed - at once
		// if the chr code refused its moves with no chr near it to blame
		if (simnavAgentBlockAhead(g_SimBrain.agents, idx, chr->invalidmove != 0 && !simbrainCrowded(chr, feet))
				&& g_SimBrain.trace) {
			printf("SB f%d b%d no headway at %.0f,%.0f,%.0f: the way ahead is blamed\n", g_Vars.lvframe60, idx, feet[0], feet[1], feet[2]);
		}

		if (sim->stuck > SIMBRAIN_MAXSTUCK) {
			simbrainToStock(chr, idx, "no headway");
			return SIMBRAIN_TICK_STOCK;
		}

		simbrainReplan(chr, idx);
		return SIMBRAIN_TICK_STOCK;
	}

	// the last stretch is stock's: straight to the end, arriving as stock does
	if (steer->final && complete) {
		return SIMBRAIN_TICK_FINAL;
	}

	// Walk to the corner, and within a stride of it to a point that slides
	// along to the next. A stride is what one tick can move it: an off-screen
	// simulant is ticked every fourth frame with four frames of movement, so
	// at the speed slider's top it covered 150 at a step and ran back and
	// forth past a corner it never got round (Complex, 500% DarkSims)
	memcpy(target, steer->corner, sizeof(target));

	if (steer->numcorners > 1 && steer->linkarea == SIMNAV_AREA_NONE) {
		f32 stride = simbrainRunSpeed(chr) * 4.0f * 1.2f;
		f32 tocorner = simbrainDist2D(feet, steer->corner);
		f32 seglen = simbrainDist2D(steer->corner, steer->after);

		if (stride < 25.0f) {
			stride = 25.0f;
		}

		if (tocorner < stride && seglen > 1.0f) {
			f32 t = (stride - tocorner) / seglen;

			if (t > 1.0f) {
				t = 1.0f;
			}

			target[0] = steer->corner[0] + (steer->after[0] - steer->corner[0]) * t;
			target[1] = steer->corner[1] + (steer->after[1] - steer->corner[1]) * t;
			target[2] = steer->corner[2] + (steer->after[2] - steer->corner[2]) * t;
		}
	}

	memcpy(ctl->aim, target, sizeof(ctl->aim));
	ctl->aim[1] += 100.0f; // where a chr's position stands over its feet
	ctl->speed = 1.0f;

	// ...and slower than a stride short of it, so the stride does not carry
	// it past: at 500% it still went back and forth over a zigzag of corners
	// 20 apart, 150 at a step, for the rest of the match
	{
		f32 stride = simbrainRunSpeed(chr) * 4.0f * 1.2f;
		f32 totarget = simbrainDist2D(feet, target);

		if (stride > 60.0f && totarget < stride) {
			ctl->speed = totarget / stride < 0.2f ? 0.2f : totarget / stride;
		}
	}

	if (steer->linkarea == SIMNAV_AREA_NONE) {
		simbrainAvoid(chr, idx, feet, target, ctl);
	}

	return SIMBRAIN_TICK_WALK;
}

s32 simbrainTickGoPos(struct chrdata *chr, struct coord *aim)
{
	struct simbrainsim *sim;
	struct simcontrol ctl;
	s32 idx;
	s32 result;
	bool tracenow;

	// the trace: each simulant's tick once in each half second it has one
	// (an off-screen simulant is ticked every fourth frame)
	tracenow = false;

	if (g_SimBrain.trace && (idx = simbrainIndex(chr)) >= 0 && g_Vars.lvframe60 / 30 != g_SimBrain.sims[idx].trace30) {
		g_SimBrain.sims[idx].trace30 = g_Vars.lvframe60 / 30;
		tracenow = true;
	}

	if (tracenow && (!simbrainOwns(chr) || g_SimBrain.sims[idx].state == SB_IDLE)) {
		printf("SB f%d b%d %s feet=%.0f,%.0f,%.0f\n", g_Vars.lvframe60, idx,
				simbrainOwns(chr) ? "idle" : "stock", chr->prop->pos.x, chr->manground, chr->prop->pos.z);
	}

	if (!simbrainOwns(chr) || (idx = simbrainIndex(chr)) < 0) {
		return SIMBRAIN_TICK_STOCK;
	}

	sim = &g_SimBrain.sims[idx];

	if (sim->state == SB_IDLE) {
		return SIMBRAIN_TICK_STOCK;
	}

	simbrainPump();
	memset(&ctl, 0, sizeof(ctl));

	// with the trace, the counts once a half minute
	if (g_SimBrain.trace && g_Vars.lvframe60 / TICKS(1800) != g_SimBrain.logframe) {
		g_SimBrain.logframe = g_Vars.lvframe60 / TICKS(1800);
		simbrainLogCounts("so far");
	}

	if (sim->state == SB_LINK) {
		result = simbrainTickLink(chr, idx, &ctl);
	} else {
		result = simbrainTickFollow(chr, idx, &ctl);
	}

	// A link is crossed looking level: an aim down a drop or up a ladder is
	// a line through the floor, and stock's navigation, which wants to see
	// its aim, wandered about the ledge instead of stepping off it
	if (sim->state == SB_LINK && result != SIMBRAIN_TICK_STOCK) {
		ctl.aim[1] = chr->manground + 100.0f;
	}

	if (tracenow) {
		f32 feet[3];
		simbrainFeet(chr, feet);
		printf("SB f%d b%d st%d res%d feet=%.0f,%.0f,%.0f aim=%.0f,%.0f,%.0f corner=%.0f,%.0f,%.0f n%d fin%d link%d area%d ph%d stuck%d onl%d inl%d gnd%.0f\n",
				g_Vars.lvframe60, idx, sim->state, result, feet[0], feet[1], feet[2], ctl.aim[0], ctl.aim[1], ctl.aim[2],
				sim->steer.corner[0], sim->steer.corner[1], sim->steer.corner[2], sim->steer.numcorners, sim->steer.final,
				sim->steer.linkarea, sim->linkarea, sim->phase, sim->stuck, chr->onladder, chr->inlift, chr->ground);

		if (sim->state == SB_LINK && sim->lift) {
			struct liftobj *lift = sim->lift;
			printf("SB   lift y=%.0f cur=%d aim=%d door=%.2f start=%.0f,%.0f,%.0f end=%.0f,%.0f,%.0f\n", liftGetY(lift),
					lift->levelcur, lift->levelaim, lift->doors[lift->levelcur] ? lift->doors[lift->levelcur]->frac : -1.0f,
					sim->linkstart[0], sim->linkstart[1], sim->linkstart[2], sim->linkend[0], sim->linkend[1], sim->linkend[2]);
		}
	}

	if (result == SIMBRAIN_TICK_STOCK) {
		return result;
	}

	// The controller, applied: stock's flags and speeds do the rest
	if (result == SIMBRAIN_TICK_FINAL) {
		// botCalculateMaxSpeed(), botCheckFetch() and the arrival know the
		// last stretch by the route having no waypoint left
		chr->act_gopos.curindex = 0;
		chr->act_gopos.waypoints[0] = NULL;
	}

	if (ctl.crouch) {
		chr->act_gopos.flags |= GOPOSFLAG_CROUCH;
	} else if (ctl.duck) {
		chr->act_gopos.flags |= GOPOSFLAG_DUCK;
	}

	if (ctl.wait) {
		chr->act_gopos.flags |= GOPOSFLAG_WAITING;
		chr->aibot->speedmultforwards = 0;
	} else {
		chr->act_gopos.flags &= ~GOPOSFLAG_WAITING;

		if (ctl.speed > 0.0f && ctl.speed < 1.0f && chr->aibot->speedmultforwards > ctl.speed) {
			chr->aibot->speedmultforwards = ctl.speed;
		}
	}

	if (ctl.jump) {
		botTryJump(chr);
	}

	simbrainCopyAim(aim, ctl.aim);

	return result;
}

s32 simbrainDebugPath(struct chrdata *chr, f32 *points, s32 maxpoints)
{
	s32 idx;

	if (!simbrainOwns(chr) || (idx = simbrainIndex(chr)) < 0) {
		return 0;
	}

	if (g_SimBrain.sims[idx].state == SB_LINK && maxpoints >= 2) {
		memcpy(&points[0], g_SimBrain.sims[idx].linkstart, sizeof(f32) * 3);
		memcpy(&points[3], g_SimBrain.sims[idx].linkend, sizeof(f32) * 3);
		return 2;
	}

	return simnavAgentCorners(g_SimBrain.agents, idx, points, maxpoints);
}

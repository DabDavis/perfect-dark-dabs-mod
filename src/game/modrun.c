#include <ultra64.h>
#include "constants.h"
#include "game/atan2f.h"
#include "game/game_0b0fd0.h"
#include "game/bondgun.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/hudmsg.h"
#include "game/inv.h"
#include "game/lang.h"
#include "game/lv.h"
#include "game/menu.h"
#include "game/modalarm.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "game/objectives.h"
#include "game/pad.h"
#include "game/pdmode.h"
#include "mod.h"
#include "game/player.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "game/stagetable.h"
#include "game/title.h"
#include "game/mplayer/setup.h"
#include "bss.h"
#include "lang.h"
#include "lib/collision.h"
#include "lib/main.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/vars.h"
#include "lib/vi.h"
#include "data.h"
#include "types.h"
#include "platform.h"
#include "math.h"
#include <string.h>
#include <stdio.h>

#ifndef PLATFORM_N64
extern void sysLogPrintf(s32 level, const char *fmt, ...);
#endif

/**
 * The Randomizer's run: one room at a time, across every map in the game.
 *
 * A run drops the player into a random room of a random map with something to
 * do there, and every door out of that room is a portal: walk through one and
 * the run lands somewhere else entirely. It keeps going until the player dies,
 * and what it scores is objectives finished, not rooms survived.
 *
 * **The room is sealed until its objective is done.** Every door out being a
 * portal makes leaving free, and leaving for nothing is a run that never does
 * anything: walk in, walk out, and what is scored is doors walked through. So
 * while the objective stands the doorway is a wall from the inside - a move
 * refused in bwalkCalculateNewPosition(), against an edge that is the portal's
 * own plane so the player slides along it rather than stopping dead - and the
 * door is a door again the moment the objective is met. A room whose objective
 * cannot be answered would be a run that cannot go on, so a room that has been
 * sealed too long is dealt the one thing that can always be answered: a clock.
 *
 * **It is sealed a room wider than that.** The landing room and every room its
 * doors open onto - modRunBuildZone() - because a room on its own is sometimes
 * a stairwell or a corridor two strides across, and a fight held in one of
 * those is fought against the walls. The rooms touching it are still one
 * helping of a level and there is somewhere to move. The seal and the door out
 * ask the same question, so what is sealed is also what a hop is measured
 * from: the portal is the first door out of those rooms rather than out of the
 * one landed in.
 *
 * **A portal is a stage load, because it has to be.** One bg file is resident
 * at a time and a portal's room numbers index that one file, so two rooms of
 * two maps cannot be joined by geometry - see modrandom.c, which learned the
 * same thing about dealing rooms from different stages. So the door out of the
 * room is not a door to the next room: it is the end of this level and the
 * beginning of another, dealt behind a fade the same way the co-operative
 * campaign moves from Deep Sea to the next mission. The load is what a hop
 * costs, and the room-sized helping of level either side of it is what makes
 * paying it every thirty seconds bearable.
 *
 * **The map is dealt again under it.** A landing is a Randomizer roll like any
 * other - the guns are moved, the crates re-filled, the guards shuffled - and
 * the run only takes two decisions out of the roll's hands: where the player
 * lands (a random room rather than a start that reaches most of the mission)
 * and what the objectives are (this room's, rather than the level's). Both are
 * things the roll has no reason to choose the run's way when it is dealing a
 * whole mission.
 *
 * **Where a landing may be.** The waypoint graph, which is how modalarm.c
 * decides where a guard may appear and for the same reason: a waypoint is by
 * construction a place something man-shaped can stand and walk away from, and
 * every stage that anything walks in has one. A random point in a random room
 * is a point inside a wall about as often as not. The room also has to have a
 * portal, or the landing is a sealed box with no door to leave by and the run
 * is over without a death.
 *
 * **What the objectives can be.** Not the stage's - those are a mission's, and
 * this is one room of it - and not the Randomizer's either, which name a thing
 * to fetch from the far end of the level or a room to reach. What is left is
 * what can be asked for and finished inside one room: put down what comes for
 * you, stay alive for a while, or pick up the gun lying there. The game's
 * objective machinery has no type for any of those, so each one is written as
 * a COMPFLAGS requirement on a stage flag this file owns and sets itself when
 * the condition is met. The objective is then the game's own - the briefing
 * draws it, objectiveCheck() answers for it - and the condition is ours.
 *
 * **What carries.** Everything: health, shield, the guns and the ammunition.
 * A run is one life across many maps, and the kit built up in the last four
 * rooms is what makes the fifth survivable. It is snapshotted at the portal
 * and put back on the frame after the landing, rather than through
 * playerStartNewLife(), because that call empties the inventory on its way
 * past and playerSpawn() zeroes the shield after it.
 *
 * **The seed.** A run has one, and each hop draws its stage, its landing and
 * its objective from the run's seed and the hop's number, so a seed written
 * down deals the same sequence of rooms. The Randomizer's own roll for a hop
 * is seeded from the same pair, which is what makes a hop reproducible all the
 * way down to which gun is lying where.
 */

#define MODRUN_OFF     0 // no run
#define MODRUN_LANDING 1 // the stage is loading or loaded, the player not yet placed
#define MODRUN_PLAYING 2 // in the room
#define MODRUN_HOPPING 3 // fading out, the next stage chosen
#define MODRUN_OVER    4 // dead, showing the score

// The stage flag a generated objective's COMPFLAGS requirement reads. The top
// bit: g_StageFlags is one word that the stage's own script owns, and scripts
// set the low bits - the highest anything in stock names is 0x00010000. It is
// cleared at every landing regardless, so the worst a stage that used it could
// do is finish one room's objective early.
#define MODRUN_STAGEFLAG 0x80000000

// How long the run's own messages stay up. The landing message has to survive
// the fade-in it is shown under, which is a second on its own.
#define MODRUN_MSGTICKS TICKS(180)

// How long a sealed room may stand unanswered before the run deals it
// something that cannot fail. Longer than the longest clock a room can ask
// for, so that holding a room for its full time is never mistaken for being
// stuck in it, and short enough that a player who has run out of ways to
// finish is not reading the same objective for ten minutes.
#define MODRUN_STUCK_SECS 150

// And how long a *kill* objective may stand with nothing in the sealed rooms
// to kill. The seal is a wall the player cannot walk out of, so a room the
// guards cannot walk into is a fight that never starts, and no amount of
// waiting changes it: if nothing hostile has been inside the zone for this
// long, nothing is coming and the room is dealt a clock instead. Two and a
// half minutes of standing in an empty room is the soft lock, not the cure.
#define MODRUN_STARVE_SECS 40

// How often the sealed room says so. It is said while the player walks into
// it, which is every frame they hold the stick forward.
#define MODRUN_SEALMSG_SECS 5

// Half the length of the wall the doorway becomes. The player slides along it
// and never reaches its end; it only has to be longer than a doorway.
#define MODRUN_SEALEDGE 10000.0f

// How many rooms the seal may shut the player into: the landing room and the
// rooms its doors open onto. A room with more doors than this keeps the rest
// of them shut, which is a smaller area rather than a broken one.
#define MODRUN_MAXZONE 32

// How near a guard may be dealt into a sealed room. The alarm's own eight
// metres is most of a zone, so it is relaxed here; four is still not on top of
// the player, and a spawn in view is refused whatever this says.
#define MODRUN_GUARDNEAR 400.0f

// Objective sizes. One block, the way modrandom.c lays its objectives out, so
// that re-dealing one at a landing cannot move another.
#define MODRUN_OBJBLOCK 64

// What a room may ask for. Kill counts are what the alarm sends rather than
// what the stage stood there, so they hold on an arena with no guards of its
// own; the seconds are a fight rather than a wait.
#define MODRUN_KILL_MIN     3
#define MODRUN_KILL_MAX     8
#define MODRUN_SURVIVE_MIN  20
#define MODRUN_SURVIVE_MAX  45

// Maps in a row that may be skipped for being unlandable before the run is
// given up on. Three is enough to get past one bad map in a pool and short
// enough that a pool of nothing but bad maps is not an endless load.
#define MODRUN_MAXSKIPS 3

// The guards a run brings with it, whatever the stage had. A run's rooms are
// entered cold and left in a minute, so the stage's own sleeping guards are
// mostly somewhere else: the ones that matter are the ones sent after the
// player, and these are the numbers the run sends them with.
//
// Halved from twelve and twelve, which was too much of a room to fight rather
// than a room to be in. Both halves matter and the cap alone would not have
// done it: with twelve still arriving every ten seconds against a cap of six,
// every kill is answered inside a second and the room feels the same however
// few are standing at once.
#define MODRUN_GUARDS 6 // up at once
#define MODRUN_SPEED  6 // per ten seconds

// The bodies a run's guards wear. The stage's own guards keep theirs; these
// are what the alarm brings, and they are drawn from the whole game so that a
// room in the Institute can be held by Skedar. One is chosen per landing
// rather than per guard, so a room reads as one enemy rather than a parade,
// and the head models stay one load each - see modbodies.c on what a second
// head costs.
static const u8 g_ModRunBodies[] = {
	BODY_DDSHOCK,
	BODY_DD_GUARD,
	BODY_A51TROOPER,
	BODY_G5_SWAT_GUARD,
	BODY_FBIGUY,
	BODY_ALASKAN_GUARD,
	BODY_PELAGIC_GUARD,
	BODY_SKEDAR,
	BODY_MINISKEDAR,
	BODY_MAIAN_SOLDIER,
	BODY_DARK_COMBAT,
	BODY_CHICROB,
	BODY_DD_SECGUARD,
	BODY_PRES_SECURITY,
	BODY_A51AIRMAN,
};

struct modrunobjective {
	s32 kind;      // MODRUN_OBJ_*
	s32 target;    // kills wanted, seconds wanted, or the weapon number
	s32 progress;  // kills at the start, or the tick the clock started
	bool done;
};

#ifndef PLATFORM_N64
bool g_ModRunAutoStart = false; // --random-run
s32 g_ModRunAutoHop = 0;        // --run-autohop N
#endif

static s32 g_ModRunState = MODRUN_OFF;
static u32 g_ModRunSeed;
static s32 g_ModRunHop;        // how many rooms the run has been dealt
static s32 g_ModRunScore;      // objectives finished
static s32 g_ModRunStage = -1; // the stage this hop asked for
static s32 g_ModRunBody = BODY_DDSHOCK;

// The landing: chosen by the roll, taken by playerStartNewLife().
static s32 g_ModRunLandPad = -1;
static s32 g_ModRunLandRoom = -1;
static s32 g_ModRunSpawnState; // 0 nothing to do, 1 waiting for control, 2 asked for
static bool g_ModRunPendingLoad; // a hop's stage has been asked for and has not arrived
static s32 g_ModRunSkips;        // maps in a row that could not be landed in
static bool g_ModRunKitDue;    // the carried kit goes back on the next tick
static s32 g_ModRunOverTicks;  // frames since the run ended

// This room's objective, and the buffer the game's objective machinery reads
// it out of. The buffer is a stage allocation; everything else here outlives
// the stage on purpose.
static struct modrunobjective g_ModRunObjective;
static u32 *g_ModRunObjCmds;
static char g_ModRunObjText[64];
static bool g_ModRunHasObjective;

// The seal: what keeps the player in the room until the objective is done.
static s32 g_ModRunObjDealt;    // the tick this room's objective was dealt at
static s32 g_ModRunObjFed;      // and the last tick it had something to work with
static s32 g_ModRunObjKills;    // the kills counted the last time it was asked
static s32 g_ModRunSealMsg;     // the tick the seal last said anything
static bool g_ModRunSealLogged; // whether this room's seal has been logged once

// What the seal shuts: the landing room and the rooms touching it. Built at
// the landing by modRunBuildZone(), and the thing every test that used to name
// the landing room asks about now.
static RoomNum g_ModRunZone[MODRUN_MAXZONE];
static s32 g_ModRunNumZone;

/**
 * What the player is carrying between rooms.
 *
 * Weapon numbers rather than inventory items: an item is a heap allocation
 * belonging to a level that is about to be torn down, and the number is the
 * whole of what a gun is once its ammunition is counted separately.
 */
struct modruncarry {
	f32 health;
	f32 shield;
	s32 ammo[33]; // ammoheldarr's length
	u8 weapons[64];
	s32 numweapons;
	s32 hands[2];
};

static struct modruncarry g_ModRunCarry;
static bool g_ModRunHasCarry;

/**
 * A run's own stream, the way modrandom.c has one per decision: a hop's stage,
 * its landing and its objective are each drawn from the seed, the hop number
 * and a stream id, so that changing how one of them is chosen does not move
 * the others in a seed somebody wrote down.
 */
#define MODRUN_STREAM_STAGE 1
#define MODRUN_STREAM_LAND  2
#define MODRUN_STREAM_OBJ   3
#define MODRUN_STREAM_BODY  4
#define MODRUN_STREAM_STUCK 5

static u32 modRunMix(u32 x)
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;

	return x;
}

static u32 g_ModRunStreamState;

static void modRunOpen(s32 stream, s32 index)
{
	g_ModRunStreamState = modRunMix(g_ModRunSeed
			+ modRunMix(stream * 0x9e3779b9u + index * 0x85ebca6bu));

	if (g_ModRunStreamState == 0) {
		g_ModRunStreamState = 1;
	}
}

static u32 modRunNext(void)
{
	u32 x = g_ModRunStreamState;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	g_ModRunStreamState = x;

	return x;
}

static s32 modRunBelow(s32 limit)
{
	if (limit <= 1) {
		return 0;
	}

	return (s32)(modRunNext() % (u32)limit);
}

/**
 * A run is under way. True from the moment the menu starts one until the
 * player dies or backs out, including while a hop's stage is loading, because
 * everything that has to behave differently during a run - the Randomizer
 * dealing the map, the alarm sending guards - is asked at stage load.
 */
bool modRunIsOn(void)
{
	return g_ModRunState != MODRUN_OFF;
}

/**
 * A run, in a level, with the player in it. What the tick and the objectives
 * ask; modRunIsOn() is what the stage load asks.
 */
bool modRunIsPlaying(void)
{
	return g_ModRunState == MODRUN_PLAYING || g_ModRunState == MODRUN_HOPPING;
}

bool modRunIsOver(void)
{
	return g_ModRunState == MODRUN_OVER;
}

s32 modRunGetScore(void)
{
	return g_ModRunScore;
}

s32 modRunGetRooms(void)
{
	return g_ModRunHop;
}

s32 modRunGetBestScore(void)
{
	return g_ModOptions.runbestscore;
}

s32 modRunGetBestRooms(void)
{
	return g_ModOptions.runbestrooms;
}

u32 modRunGetSeed(void)
{
	return g_ModRunSeed;
}

/**
 * The body this landing's guards wear, for modalarm.c to ask instead of its
 * own per-stage choice. A run is not the stage's fiction, so the guard is not
 * the stage's guard.
 */
s32 modRunChooseBody(void)
{
	return g_ModRunBody;
}

/**
 * The guards a run brings, for the alarm's settings to answer with while one
 * is under way. A run does not read Dab's Mod Options for these: the stream of
 * guards is what a room is, so it is the mode's number rather than a
 * preference the mode happens to run under.
 */
s32 modRunGetGuardCount(void)
{
	// None once the room is won. What is left of a room after its objective
	// is the walk to the way out, and reinforcements arriving through it are
	// not a rest - the point of the pause is that the player chooses when the
	// next room starts. The alarm needs nothing else told to it: its tick
	// stops at `alive >= maxalive`, which nothing can be under.
	if (g_ModRunHasObjective && g_ModRunObjective.done) {
		return 0;
	}

	return MODRUN_GUARDS;
}

s32 modRunGetGuardSpeed(void)
{
	return MODRUN_SPEED;
}

/**
 * A map's name, for the message that says where the run just landed.
 *
 * A stage number is not a name anywhere in the game: the missions are named in
 * g_SoloStages and the arenas in g_MpArenas, and a map a mod brought is named
 * only in the second because that is where the Stage Loader registers it. So
 * both lists are asked, and anything in neither is shown as its number, which
 * is at least the thing to put in a bug report.
 */
const char *modRunGetStageName(s32 stagenum)
{
	static char text[40];
	s32 i;

	for (i = 0; i < NUM_SOLOSTAGES; i++) {
		if ((s32)g_SoloStages[i].stagenum == stagenum) {
			const char *name = langGet(g_SoloStages[i].name3);

			if (name && name[0]) {
				return name;
			}
		}
	}

	for (i = 0; i < mpGetNumStages(); i++) {
		if (g_MpArenas[i].stagenum == stagenum) {
			char *name = mpGetArenaName(i);

			if (name && name[0] && name[0] != '\n') {
				s32 len;

				snprintf(text, sizeof(text), "%s", name);

				// The arena list's names carry the trailing newline
				// textMeasure() wants for a menu row; this is a sentence.
				len = strlen(text);

				while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' ')) {
					text[--len] = '\0';
				}

				return text;
			}
		}
	}

	snprintf(text, sizeof(text), "Stage %02x", stagenum);

	return text;
}

/**
 * Whether a stage may be landed in.
 *
 * The pool is built from the game's own two lists of maps - the missions in
 * g_SoloStages and the arenas in g_MpArenas - and never from the stage table,
 * which is what a first version of this did. The table holds everything the
 * build can load, and that includes the unfinished development maps:
 * STAGE_TEST_MP16 has no waypoints, no room with a portal, and the run that
 * landed in it took the game down with SIGFPE. A map nobody put in either list
 * is a map nobody plays.
 *
 * Which is also what makes the widest pool mean what it says: the Stage Loader
 * registers a mod's maps as arenas, so everything past the stock sixteen in
 * g_MpArenas is exactly "and the maps a mod brought".
 *
 * The Carrington Institute is out for the reason modrandom.c gives - it is the
 * backdrop the Perfect Menu is drawn over rather than a level anybody plays -
 * and so is the stage the run is standing in, since a portal that deals the
 * same map again looks like the portal did nothing.
 */
static bool modRunStageIsAllowed(s32 stagenum)
{
	if (stagenum <= 0 || !STAGE_IS_LEVEL(stagenum)) {
		return false;
	}

	if (stagenum == STAGE_MP_RANDOM) {
		// The arena list's last stock row is the "Random" button rather than
		// a map, and it carries a stage number that is not one.
		return false;
	}

	if (stagenum == g_ModRunStage) {
		return false;
	}

	if (stagenum == STAGE_CITRAINING || stagenum == modDataBgStage(STAGE_CITRAINING)) {
		return false;
	}

	return stageGetIndex(stagenum) >= 0;
}

/**
 * The next map, drawn from the run's stage stream so a seed keeps its
 * sequence.
 *
 * Reservoir sampling over the two lists rather than a list built first: the
 * arena list has room for 189 mod stages and most of it is empty, and a pass
 * that counts followed by a pass that picks would have to agree about which
 * rows were filled in on both.
 */
static s32 modRunChooseStage(void)
{
	const s32 pool = modGetRunPool();
	// mpGetNumStages(), not g_MpNumArenas: the rows past the stock sixteen are
	// maps only a mod provides the data for, and the count includes them
	// whether or not one is loaded. Without that rule a run landed in
	// STAGE_TEST_ARCH with no mod to supply it and took the game down.
	const s32 numarenas = pool >= MODRUN_POOL_ALL
		? mpGetNumStages()
		: (pool >= MODRUN_POOL_MP ? MP_NUM_STOCK_ARENAS : 0);
	s32 chosen = -1;
	s32 count = 0;
	s32 i;

	modRunOpen(MODRUN_STREAM_STAGE, g_ModRunHop);

	for (i = 0; i < NUM_SOLOSTAGES; i++) {
		const s32 stagenum = (s32)g_SoloStages[i].stagenum;

		if (!modRunStageIsAllowed(stagenum)) {
			continue;
		}

		count++;

		if (modRunBelow(count) == 0) {
			chosen = stagenum;
		}
	}

	for (i = 0; i < numarenas && i < (s32)ARRAYCOUNT(g_MpArenas); i++) {
		const s32 stagenum = g_MpArenas[i].stagenum;

		if (!modRunStageIsAllowed(stagenum)) {
			continue;
		}

		count++;

		if (modRunBelow(count) == 0) {
			chosen = stagenum;
		}
	}

	if (chosen < 0) {
		// Nothing but the map already loaded passed. Deal it again rather than
		// ending a run on a build whose pool is one map wide.
		chosen = g_ModRunStage > 0 ? g_ModRunStage : STAGE_CHICAGO;
	}

	return chosen;
}

/**
 * Load the map this hop landed on.
 *
 * The same five calls menuhandlerAcceptMission() makes, which is what starting
 * a mission is: the stage in g_MissionConfig, the difficulty, the title screen
 * told to skip itself, and the change asked for at the end of the frame.
 * TITLEMODE_SKIP is what keeps a hop from showing the Rare logo.
 */
static void modRunEnterStage(s32 stagenum)
{
	g_ModRunStage = stagenum;

	g_MissionConfig.iscoop = false;
	g_MissionConfig.isanti = false;
	g_MissionConfig.stagenum = stagenum;
	g_MissionConfig.difficulty = modGetRunDifficulty();

	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = -1;
	g_Vars.antiplayernum = -1;
	setNumPlayers(1);

	titleSetNextStage(stagenum);
	lvSetDifficulty(g_MissionConfig.difficulty);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(stagenum);
	viBlack(true);

	g_ModRunState = MODRUN_LANDING;
	g_ModRunPendingLoad = true;
	g_ModRunSpawnState = 0;
	g_ModRunLandPad = -1;
	g_ModRunLandRoom = -1;
	g_ModRunNumZone = 0;
	g_ModRunHasObjective = false;
	g_ModRunObjCmds = NULL;
}

/**
 * Start a run. From the menu, and from --random-run.
 *
 * The seed is the one in the options if a player kept one, and otherwise a
 * fresh number from the game's own generator - which is fine here in a way it
 * is not inside the roll, because it is read once, at the moment a person
 * pressed a button, rather than being what the run is dealt from frame to
 * frame.
 */
void modRunStart(void)
{
	g_ModRunSeed = g_ModOptions.randomseed ? (u32)g_ModOptions.randomseed : rngRandom();

	if (g_ModRunSeed == 0) {
		g_ModRunSeed = 1;
	}

	g_ModRunHop = 0;
	g_ModRunScore = 0;
	g_ModRunStage = -1;
	g_ModRunHasCarry = false;
	g_ModRunOverTicks = 0;
	g_ModRunSkips = 0;
	g_ModRunState = MODRUN_LANDING;

	menuStop();
	modRunEnterStage(modRunChooseStage());

#ifndef PLATFORM_N64
	sysLogPrintf(0, "run: begin seed %u pool %d difficulty %d, first stage 0x%02x",
			g_ModRunSeed, modGetRunPool(), modGetRunDifficulty(), g_ModRunStage);
#endif
}

/**
 * End a run without a death: backing out of the mode, or the game leaving the
 * level any way this file did not ask for.
 */
void modRunStop(void)
{
	g_ModRunState = MODRUN_OFF;
	g_ModRunPendingLoad = false;
	g_ModRunStage = -1;
	g_ModRunSpawnState = 0;
	g_ModRunKitDue = false;
	g_ModRunHasCarry = false;
	g_ModRunHasObjective = false;
	g_ModRunObjCmds = NULL;
	g_ModRunLandPad = -1;
	g_ModRunLandRoom = -1;
	g_ModRunNumZone = 0;
}

/**
 * The stage's waypoints, counted the way setupLoadWaypoints() counts them.
 */
static s32 modRunCountWaypoints(void)
{
	s32 count = 0;

	if (g_StageSetup.waypoints) {
		while (g_StageSetup.waypoints[count].padnum >= 0) {
			count++;
		}
	}

	return count;
}

static s32 modRunPadRoom(s32 padnum)
{
	struct pad pad;

	if (padnum < 0) {
		return -1;
	}

	padUnpack(padnum, PADFIELD_ROOM, &pad);

	return pad.room;
}

/**
 * Where this hop lands.
 *
 * A waypoint, because a waypoint is somewhere a chr can stand and walk away
 * from - see the header. The room it is in has to have a portal, or there is
 * no door to leave by and the hop is a dead end; and it may not be the room
 * the last hop landed in on this map, so that a run that deals the same map
 * twice is not the same room twice.
 *
 * **A waypoint a chr can walk through is not always somewhere a player can be
 * put down.** Between 2% and 8% of a stage's have no floor under them at all -
 * 31 of one stage's 371 - and a landing on one of those does not fail, it
 * lands the player four billion units below the level. That is the whole of
 * "sometimes it spawns you out of bounds and you die", and the question is
 * modRandomPadSpawnPos()'s: it is asked here, before the draw, so that a bad
 * pad is not in the pool rather than being drawn and then worked around.
 */
static void modRunChooseLanding(void)
{
	const s32 numwaypoints = modRunCountWaypoints();
	const bool checkpads = modRandomGetVersion() >= 3;
	s32 chosen = -1;
	s32 count = 0;
	s32 unusable = 0;
	s32 i;

	g_ModRunLandPad = -1;
	g_ModRunLandRoom = -1;
	g_ModRunNumZone = 0;

	modRunOpen(MODRUN_STREAM_LAND, g_ModRunHop);

	for (i = 0; i < numwaypoints; i++) {
		const s32 padnum = g_StageSetup.waypoints[i].padnum;
		const s32 room = modRunPadRoom(padnum);

		if (room <= 0 || room >= g_Vars.roomcount) {
			continue;
		}

		if (g_Rooms[room].numportals <= 0) {
			continue;
		}

		if (checkpads && !modRandomPadCanSpawn(padnum)) {
			unusable++;
			continue;
		}

		count++;

		if (modRunBelow(count) == 0) {
			chosen = padnum;
		}
	}

#ifndef PLATFORM_N64
	if (unusable) {
		sysLogPrintf(0, "run: stage 0x%02x - %d of %d waypoints in a room with a door are not standable",
				g_ModRunStage, unusable, count + unusable);
	}
#endif

	if (chosen < 0) {
		// No waypoint in a room with a door. Let the stage start the player
		// where it would have: a hop with no landing of its own is still a
		// room, and the portal out of it is the same test. The new life is
		// still asked for - see modRunTick() on why every landing wants one.
		return;
	}

	g_ModRunLandPad = chosen;
	g_ModRunLandRoom = modRunPadRoom(chosen);
}

/**
 * A gun lying loose in the landing room, for an objective to name.
 *
 * The setup stream, not the props: this runs before setupCreateProps() has
 * built anything, which is the only moment the whole level is describable and
 * nothing has been placed yet. A gun assigned to a guard or sitting inside
 * another object is not lying there to be picked up, and one the player is
 * already carrying would be an objective that completed itself.
 */
/**
 * Whether the kit coming through the portal already holds this gun.
 *
 * The inventory itself is no use at roll time: the level being dealt has not
 * built a player yet and the last one's inventory went with its stage. What
 * the run knows is what it snapshotted at the portal, which is the same list.
 */
static bool modRunIsCarrying(s32 weaponnum)
{
	s32 i;

	if (!g_ModRunHasCarry) {
		return false;
	}

	for (i = 0; i < g_ModRunCarry.numweapons; i++) {
		if (g_ModRunCarry.weapons[i] == weaponnum) {
			return true;
		}
	}

	return false;
}

static s32 modRunFindWeaponInRoom(s32 room)
{
	struct defaultobj *obj = (struct defaultobj *)g_StageSetup.props;
	s32 chosen = WEAPON_NONE;
	s32 count = 0;

	if (obj == NULL || room <= 0) {
		return WEAPON_NONE;
	}

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_WEAPON
				&& (obj->flags & (OBJFLAG_ASSIGNEDTOCHR | OBJFLAG_INSIDEANOTHEROBJ)) == 0
				&& modRunPadRoom(obj->pad) == room) {
			const s32 weaponnum = ((struct weaponobj *)obj)->weaponnum;

			if (weaponnum > WEAPON_NONE && !modRunIsCarrying(weaponnum)) {
				count++;

				if (modRunBelow(count) == 0) {
					chosen = weaponnum;
				}
			}
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}

	return chosen;
}

/**
 * What this room asks for.
 *
 * Three kinds, all of them answerable inside one room and all of them checked
 * by this file rather than by the game - see the header on why the objective
 * itself is a stage flag. Collecting is the best of the three when the room
 * has a loose gun in it, because it names a thing the player can see; the
 * other two are what is left when it does not.
 */
/**
 * The two objectives a room can always ask for, whatever is standing in it.
 * Split out because a landing that finds its collect objective already
 * satisfied falls back to one of these rather than handing out a free point.
 */
static void modRunDealFight(bool survive)
{
	if (survive) {
		const s32 seconds = MODRUN_SURVIVE_MIN
			+ modRunBelow(MODRUN_SURVIVE_MAX - MODRUN_SURVIVE_MIN + 1);

		g_ModRunObjective.kind = MODRUN_OBJ_SURVIVE;
		g_ModRunObjective.target = seconds;
		g_ModRunObjective.progress = 0;

		snprintf(g_ModRunObjText, sizeof(g_ModRunObjText), "Hold this room for %d seconds", seconds);
	} else {
		const s32 kills = MODRUN_KILL_MIN + modRunBelow(MODRUN_KILL_MAX - MODRUN_KILL_MIN + 1);

		g_ModRunObjective.kind = MODRUN_OBJ_KILL;
		g_ModRunObjective.target = kills;
		g_ModRunObjective.progress = 0;

		snprintf(g_ModRunObjText, sizeof(g_ModRunObjText), "Eliminate %d hostiles", kills);
	}
}

static void modRunDealObjective(void)
{
	s32 kind;

	g_ModRunHasObjective = false;
	g_ModRunObjText[0] = '\0';

	modRunOpen(MODRUN_STREAM_OBJ, g_ModRunHop);

	kind = modRunBelow(3);

	if (kind == MODRUN_OBJ_COLLECT) {
		const s32 weaponnum = modRunFindWeaponInRoom(g_ModRunLandRoom);

		if (weaponnum > WEAPON_NONE) {
			struct weapon *weapondef = weaponFindById(weaponnum);
			const char *name = weapondef ? langGet(weapondef->name) : NULL;

			g_ModRunObjective.kind = MODRUN_OBJ_COLLECT;
			g_ModRunObjective.target = weaponnum;
			g_ModRunObjective.progress = 0;

			snprintf(g_ModRunObjText, sizeof(g_ModRunObjText),
					"Recover the %s", name ? name : "hardware");

			g_ModRunHasObjective = true;
			return;
		}

		// Nothing loose in this room. Fall through to a fight rather than
		// dealing nothing, which would be a room worth no points through no
		// choice of the player's.
		kind = MODRUN_OBJ_KILL;
	}

	modRunDealFight(kind == MODRUN_OBJ_SURVIVE);

	g_ModRunHasObjective = true;
}

/**
 * Deal this hop's room: where the player lands, who holds it and what they
 * want. From modRandomRoll(), in the same gap the Randomizer's own roll
 * happens in - after the pads and the waypoints are readable and before
 * anything has been built from the stream.
 */
void modRunRoll(void)
{
	if (!modRunIsOn()) {
		return;
	}

	g_ModRunObjCmds = NULL;
	g_ModRunOverTicks = 0;
	g_ModRunObjective.done = false;

	// A landing's guards are one kind of thing, chosen with the room.
	modRunOpen(MODRUN_STREAM_BODY, g_ModRunHop);
	g_ModRunBody = g_ModRunBodies[modRunBelow(ARRAYCOUNT(g_ModRunBodies))];

	modRunChooseLanding();
	modRunDealObjective();

	// Every landing is a new life - it is what places the player, puts the kit
	// back and cures a map that started them dead - and the override below
	// always answers, even on a hop that found no pad. It has to: without an
	// answer playerStartNewLife() falls through to the game's own chooser, and
	// a map whose solo setup lists no spawn point at all (an arena, which was
	// never asked to hold a mission) divides by the number of them. SIGFPE,
	// and an hour's run gone.
	g_ModRunSpawnState = 1;
	g_ModRunKitDue = true;

#ifndef PLATFORM_N64
	sysLogPrintf(0, "run: hop %d stage 0x%02x seed %u - land pad %d room %d, body %d, objective %d \"%s\"",
			g_ModRunHop, g_ModRunStage, g_ModRunSeed,
			g_ModRunLandPad, g_ModRunLandRoom, g_ModRunBody,
			g_ModRunObjective.kind, g_ModRunObjText);
#endif
}

/**
 * The spot playerStartNewLife() should use, or nothing when the hop found no
 * pad and the stage's own start will do. What marks the landing done is the
 * tick watching dostartnewlife go back down, not this call: a landing with no
 * pad answers no and still has to know the new life happened.
 */
bool modRunTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle)
{
	struct pad pad;

	if (g_ModRunSpawnState != 2 || !modRunIsOn()) {
		return false;
	}

	if (g_ModRunLandPad < 0) {
		// No waypoint in a room with a door on this map. Where the stage put
		// the player will do - the point of answering at all is that the
		// game's own chooser is never reached.
		struct player *player = g_Vars.currentplayer;
		s32 i;

		if (player == NULL || player->prop == NULL) {
			return false;
		}

		pos->x = player->prop->pos.x;
		pos->y = player->prop->pos.y;
		pos->z = player->prop->pos.z;

		for (i = 0; i < (s32)ARRAYCOUNT(player->prop->rooms) - 1; i++) {
			rooms[i] = player->prop->rooms[i];

			if (rooms[i] == -1) {
				break;
			}
		}

		rooms[i] = -1;
		*angle = player->vv_theta * M_BADTAU / 360.0f;

		return true;
	}

	padUnpack(g_ModRunLandPad, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_LOOK, &pad);

	// The floor's own y rather than the pad's: playerStartNewLife() takes the
	// highest floor strictly below the position it is handed, and a pad level
	// with its own floor misses it. Every version gets this - it is not which
	// pad the seed dealt, only where on it the player stands.
	if (!modRandomPadSpawnPos(g_ModRunLandPad, pos, &rooms[0])) {
		pos->x = pad.pos.x;
		pos->y = pad.pos.y;
		pos->z = pad.pos.z;

		rooms[0] = pad.room;
	}

	rooms[1] = -1;

	*angle = atan2f(pad.look.x, pad.look.z);

	return true;
}

/**
 * Write this room's objective where the game's objective machinery can read
 * it, and put it in the list.
 *
 * A COMPFLAGS requirement on the flag this file owns: the objective is then
 * the game's - objectiveCheck() answers for it, the briefing and the pause
 * menu draw it - and what completes it is modRunTickObjective() setting the
 * flag. Everything else about the shape of this is modrandom.c's, which is
 * where the comment about writing a command stream by hand lives.
 */
void modRunInsertObjectives(void)
{
	struct objective *objective;
	u32 *cmd;
	s32 i;

	if (!modRunIsOn() || !g_ModRunHasObjective) {
		return;
	}

	g_ModRunObjCmds = mempAlloc(ALIGN16(MODRUN_OBJBLOCK), MEMPOOL_STAGE);

	if (g_ModRunObjCmds == NULL) {
		g_ModRunHasObjective = false;
		return;
	}

	// The flag starts clear: it is what says this room's objective is not done,
	// and a stage that set it for its own reasons would otherwise hand the
	// player a point for landing.
	g_StageFlags &= ~MODRUN_STAGEFLAG;

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		g_Objectives[i] = NULL;
		g_ObjectiveStatuses[i] = OBJECTIVE_INCOMPLETE;
	}

	for (i = 0; i < (s32)ARRAYCOUNT(g_Briefing.objectivenames); i++) {
		g_Briefing.objectivenames[i] = 0;
		g_Briefing.objectivedifficulties[i] = 0;
	}

	g_ObjectiveLastIndex = -1;

	cmd = g_ModRunObjCmds;
	objective = (struct objective *)cmd;

	objective->unk00 = PD_BE32(OBJTYPE_BEGINOBJECTIVE);
	objective->index = 0;
	objective->text = L_MISC_042; // never read: modRunGetObjectiveText() answers first
	objective->unk0c = 0;
	objective->flags = 0;
	objective->difficulties = DIFFBIT_A | DIFFBIT_SA | DIFFBIT_PA | DIFFBIT_PD;

	cmd += setupGetCmdLength(cmd);

	cmd[0] = PD_BE32(OBJECTIVETYPE_COMPFLAGS);
	cmd[1] = MODRUN_STAGEFLAG;

	cmd += setupGetCmdLength(cmd);

	cmd[0] = PD_BE32(OBJTYPE_ENDOBJECTIVE);

	objectiveInsert(objective);

	g_Briefing.objectivenames[0] = L_MISC_042;
	g_Briefing.objectivedifficulties[0] = objective->difficulties;
}

/**
 * The text of this room's objective, for everything that draws one.
 */
char *modRunGetObjectiveText(s32 index)
{
	if (modRunIsOn() && g_ModRunHasObjective && index == 0) {
		return g_ModRunObjText;
	}

	return NULL;
}

/**
 * Whether the landing's new life is the one being set up right now.
 *
 * What the three restore hooks below hang off. They are called from inside
 * playerStartNewLife() and playerSpawn(), which run for every new life the
 * game gives anybody; this says the one in progress is the run's landing.
 */
bool modRunIsLanding(void)
{
	return modRunIsOn() && g_ModRunSpawnState == 2 && g_ModRunKitDue && g_ModRunHasCarry;
}

/**
 * The guns and the ammunition, given back where the mission's own kit is
 * handed out - after the intro stream has had its say, so the two add up.
 *
 * This has to be here rather than a frame later from the tick, because a gun
 * put in a hand is a model to load and the load is a state machine that runs
 * from the spawn. Equipping from the tick instead left the left hand visible
 * with no modeldef behind it and the HUD render dereferenced NULL on the
 * frame after a landing - bondgun.c:8076, one hop into a run.
 */
void modRunRestoreInventory(void)
{
	struct player *player = g_Vars.currentplayer;
	s32 i;

	if (!modRunIsLanding() || player == NULL) {
		return;
	}

	for (i = 0; i < g_ModRunCarry.numweapons; i++) {
		invGiveSingleWeapon(g_ModRunCarry.weapons[i]);
	}

	for (i = 0; i < (s32)ARRAYCOUNT(g_ModRunCarry.ammo); i++) {
		if (g_ModRunCarry.ammo[i] > player->ammoheldarr[i]) {
			player->ammoheldarr[i] = g_ModRunCarry.ammo[i];
		}
	}
}

/**
 * What that hand was holding at the portal, for playerSpawnWeapons() to put
 * back - the same override Mission Respawn uses, and the only place a hand may
 * be filled from. A gun the landing's inventory does not have is not asked
 * for: a mod's table without it, or an all-guns cheat gone.
 */
s32 modRunGetHandWeapon(s32 handnum)
{
	const s32 weaponnum = g_ModRunCarry.hands[handnum];

	if (weaponnum > WEAPON_NONE && invHasSingleWeaponIncAllGuns(weaponnum)) {
		return weaponnum;
	}

	return handnum == HAND_RIGHT ? WEAPON_UNARMED : WEAPON_NONE;
}

/**
 * The damage taken so far, at the end of the landing: after playerSpawn(),
 * which sets the shield back to zero on its way past.
 */
void modRunRestoreHealth(void)
{
	struct player *player = g_Vars.currentplayer;

	if (!modRunIsLanding() || player == NULL) {
		return;
	}

	g_ModRunKitDue = false;

	player->bondhealth = g_ModRunCarry.health;
	player->oldhealth = 0;
	player->apparenthealth = 0;
	playerSetShieldFrac(g_ModRunCarry.shield);
	player->oldarmour = 0;
	player->apparentarmour = 0;
}

/**
 * What the player is carrying, at the moment they step through the portal.
 */
static void modRunSnapshotKit(void)
{
	struct player *player = g_Vars.currentplayer;
	s32 count = invGetCount();
	s32 i;

	g_ModRunCarry.numweapons = 0;
	g_ModRunCarry.health = player->bondhealth;
	g_ModRunCarry.shield = playerGetShieldFrac();
	g_ModRunCarry.hands[HAND_LEFT] = bgunGetWeaponNum(HAND_LEFT);
	g_ModRunCarry.hands[HAND_RIGHT] = bgunGetWeaponNum(HAND_RIGHT);

	for (i = 0; i < (s32)ARRAYCOUNT(g_ModRunCarry.ammo); i++) {
		g_ModRunCarry.ammo[i] = player->ammoheldarr[i];
	}

	for (i = 0; i < count && g_ModRunCarry.numweapons < (s32)ARRAYCOUNT(g_ModRunCarry.weapons); i++) {
		const s32 weaponnum = invGetWeaponNumByIndex(i);

		// Not the empty hand and not the pill: both are given by the level it
		// lands in, and the pill is a counter-operative's business.
		if (weaponnum > WEAPON_NONE
				&& weaponnum != WEAPON_UNARMED
				&& weaponnum != WEAPON_SUICIDEPILL
				&& weaponnum < 256) {
			g_ModRunCarry.weapons[g_ModRunCarry.numweapons++] = (u8)weaponnum;
		}
	}

	g_ModRunHasCarry = true;
}

static void modRunOpenExits(void);
static s32 modRunSweepZone(bool kill);

/**
 * Has this room's objective been met? Asked every tick while the run is
 * playing; sets the flag the objective reads once, and scores it.
 */
static void modRunTickObjective(void)
{
	struct player *player = g_Vars.currentplayer;
	bool done = false;

	if (!g_ModRunHasObjective || g_ModRunObjective.done) {
		return;
	}

	switch (g_ModRunObjective.kind) {
	case MODRUN_OBJ_KILL:
		if (g_Vars.currentplayerstats) {
			const s32 kills = g_Vars.currentplayerstats->killcount - g_ModRunObjective.progress;

			done = kills >= g_ModRunObjective.target;
		}
		break;
	case MODRUN_OBJ_SURVIVE:
		done = g_Vars.lvframe60 - g_ModRunObjective.progress >= TICKS(g_ModRunObjective.target * 60);
		break;
	case MODRUN_OBJ_COLLECT:
		done = invHasSingleWeaponIncAllGuns(g_ModRunObjective.target);
		break;
	}

	if (!done) {
		return;
	}

	g_ModRunObjective.done = true;
	g_ModRunScore++;

	// The flag is what the objective's COMPFLAGS requirement reads, so setting
	// it is what completes the objective as far as the rest of the game is
	// concerned.
	g_StageFlags |= MODRUN_STAGEFLAG;

	// And the way out has to be one: a room whose every door wants a key is
	// shut as firmly as the seal was.
	modRunOpenExits();

	// The room is won, so it stops being a fight: full health and shield, the
	// hostiles in it dead, and no more of them until the next landing (see
	// modRunGetGuardCount()). A run is one long life across every map it
	// deals, and the only thing it ever gave back was what the map's own
	// intro happened to hand out at the landing - so a room survived on a
	// tenth of a bar was a run over in the next one, whatever the player did.
	// The rest between rooms is where the health goes, and it is the player's
	// to take: nothing hurries them through the door once the guards are
	// down.
	modRunSweepZone(true);

	if (player) {
		player->bondhealth = 1;
		playerSetShieldFrac(1);

		// Zeroed so the HUD sweeps back up to full rather than cutting to it,
		// the same way modRunRestoreHealth() hands the carried kit back.
		player->oldhealth = 0;
		player->apparenthealth = 0;
		player->oldarmour = 0;
		player->apparentarmour = 0;
	}

#ifndef PLATFORM_N64
	sysLogPrintf(0, "run: objective %d done on stage 0x%02x at frame %d - score %d, health and shield back",
			g_ModRunObjective.kind, g_ModRunStage, g_Vars.lvframenum, g_ModRunScore);
#endif

	{
		static char text[96];

		sprintf(text, "Objective complete - %d in %d rooms\nThe way out is open - take a breath\n",
				g_ModRunScore, g_ModRunHop);
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
	}
}

/**
 * Whether a room list holds a given room. Both lists a move has are -1
 * terminated and eight long, and either end can come first.
 */
static bool modRunRoomsHave(const RoomNum *rooms, s32 room)
{
	s32 i;

	if (rooms == NULL || room < 0) {
		return false;
	}

	for (i = 0; i < 8; i++) {
		if (rooms[i] == -1) {
			break;
		}

		if (rooms[i] == room) {
			return true;
		}
	}

	return false;
}

/**
 * Is this room inside the seal?
 *
 * The zone, which is the landing room and the rooms its doors open onto. With
 * no zone built - a state this should not be in while a room is sealed - the
 * landing room alone, which is the question this used to ask everywhere.
 */
static bool modRunZoneHas(s32 room)
{
	s32 i;

	if (room < 0) {
		return false;
	}

	if (g_ModRunNumZone <= 0) {
		return room == g_ModRunLandRoom;
	}

	for (i = 0; i < g_ModRunNumZone; i++) {
		if (g_ModRunZone[i] == room) {
			return true;
		}
	}

	return false;
}

/**
 * Is any of this rooms[] list inside the seal? Standing in a doorway lists
 * both of its rooms, so "in the zone" is any of them, the same way the room
 * test it replaces was any of them.
 */
static bool modRunRoomsInZone(const RoomNum *rooms)
{
	s32 i;

	if (rooms == NULL) {
		return false;
	}

	for (i = 0; i < 8; i++) {
		if (rooms[i] == -1) {
			break;
		}

		if (modRunZoneHas(rooms[i])) {
			return true;
		}
	}

	return false;
}

/**
 * The far side of a portal that leaves the zone, or -1.
 *
 * A portal of any room in the zone whose other side is both out of the zone
 * and one of the rooms handed in. What it is for is the wall the doorway
 * becomes and the check that the zone has a door out of it at all.
 */
static s32 modRunZoneExitPortal(const RoomNum *torooms)
{
	s32 z;
	s32 i;

	if (g_Rooms == NULL || g_RoomPortals == NULL || g_BgPortals == NULL) {
		return -1;
	}

	for (z = 0; z < g_ModRunNumZone; z++) {
		const s32 zoneroom = g_ModRunZone[z];
		const struct room *room;

		if (zoneroom <= 0 || zoneroom >= g_Vars.roomcount) {
			continue;
		}

		room = &g_Rooms[zoneroom];

		for (i = 0; i < room->numportals; i++) {
			const s32 portalnum = g_RoomPortals[room->roomportallistoffset + i];
			const struct bgportal *portal = &g_BgPortals[portalnum];
			const s32 other = portal->roomnum1 == zoneroom
				? portal->roomnum2
				: portal->roomnum1;

			if (modRunZoneHas(other)) {
				continue;
			}

			if (torooms == NULL || modRunRoomsHave(torooms, other)) {
				return portalnum;
			}
		}
	}

	return -1;
}

/**
 * Whether a portal is a way out of the zone: one end inside it, one end not.
 *
 * The same question modRunZoneExitPortal() asks, put the other way round so
 * that a door can be handed its own portal number and answered about.
 */
static bool modRunPortalLeavesZone(s32 portalnum)
{
	const struct bgportal *portal;

	if (portalnum < 0 || g_BgPortals == NULL || g_ModRunNumZone <= 0) {
		return false;
	}

	portal = &g_BgPortals[portalnum];

	return modRunZoneHas(portal->roomnum1) != modRunZoneHas(portal->roomnum2);
}

/**
 * The hostiles standing in the sealed rooms: whether there is one, and killing
 * them where the caller asks for that.
 *
 * Not the alarm's list: the stage's own guards are as good a hostile as the
 * ones the run sends, so both are what a kill objective may be answered with
 * and both are what the room is cleared of when it is won. A chr on the
 * player's own team is not one, and neither is a scientist.
 *
 * The zone rather than the map, for the same reason the seal is the zone: the
 * rooms the player has been shut into are the fight, and a guard three rooms
 * away that the run never showed them is the map's own business.
 */
static s32 modRunSweepZone(bool kill)
{
	struct player *player = g_Vars.currentplayer;
	const s32 numchrs = chrsGetNumSlots();
	struct coord novector = {0, 0, 0};
	struct gset nogset = { WEAPON_COMBATKNIFE, 0, 0, FUNC_POISON };
	s32 playerteam = TEAM_ALLY;
	s32 found = 0;
	s32 i;

	if (player && player->prop && player->prop->chr) {
		playerteam = player->prop->chr->team;
	}

	for (i = 0; i < numchrs; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || chr->prop == NULL || chr->model == NULL) {
			continue;
		}

		if (chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DIE) {
			continue;
		}

		if (chr->team == playerteam || (chr->team & TEAM_NONCOMBAT)) {
			continue;
		}

		if (!modRunRoomsInZone(chr->prop->rooms)) {
			continue;
		}

		found++;

		if (!kill) {
			// The asker only wanted to know whether there is one.
			break;
		}

		// Killed rather than taken away: a guard that vanishes as the
		// objective lands reads as the room breaking, and the corpses are
		// what the room was. chrDamage() clamps the amount to what is left
		// of the chr's maxdamage, so any number past it is a death.
		//
		// The vector and the gset are the ones every damage with no shooter
		// behind it uses - the poison tick in chr.c and the AI list's own
		// damage command both pass exactly these. Neither may be NULL:
		// chrDamage() reads `vector` and `gset->weaponnum` on the way to the
		// alive branch and only tests `vector` for NULL much further down. A
		// knife with FUNC_POISON is not the knife special case either, which
		// wants FUNC_PRIMARY and a shot in the back.
		//
		// No attacker prop, so this is nobody's kill. The objective is
		// already done and the tick above has stopped counting, but a kill
		// objective that scored these would be scoring the reward for
		// finishing it.
		chrDamageByMisc(chr, 10000, &novector, &nogset, NULL);
	}

#ifndef PLATFORM_N64
	if (kill && found) {
		sysLogPrintf(0, "run: cleared %d hostile(s) out of the zone on stage 0x%02x",
				found, g_ModRunStage);
	}
#endif

	return found;
}

static bool modRunEnemyInZone(void)
{
	return modRunSweepZone(false) > 0;
}

/**
 * Take the keys off the doors the player has to leave by.
 *
 * The seal lifting says the room is done with, and on most maps that is the
 * whole of it: the doorway is a doorway again. A door with key flags on it is
 * not - it wants a card the roll had no reason to leave in this room, and a
 * zone whose every way out wants one is a room the player is shut into for
 * good with nothing left to do in it. So the keys come off when the seal does,
 * and only from the doors standing in a portal that leaves the zone: a locked
 * door deeper in the map is the map's own business.
 *
 * The setup stream's doorobj *is* the live door - setupCreateProps() fills in
 * its portalnum and its prop in place - which is why this can be walked at any
 * point in the level.
 */
static void modRunOpenExits(void)
{
	struct defaultobj *obj = (struct defaultobj *)g_StageSetup.props;
	s32 opened = 0;

	if (obj == NULL || g_ModRunNumZone <= 0) {
		return;
	}

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_DOOR) {
			struct doorobj *door = (struct doorobj *)obj;

			if (door->keyflags
					&& (door->base.flags & OBJFLAG_DOOR_HASPORTAL)
					&& modRunPortalLeavesZone(door->portalnum)) {
				door->keyflags = 0;
				opened++;
			}
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}

#ifndef PLATFORM_N64
	if (opened) {
		sysLogPrintf(0, "run: unlocked %d door(s) out of room %d on stage 0x%02x",
				opened, g_ModRunLandRoom, g_ModRunStage);
	}
#endif
}

/**
 * What the seal shuts the player into: the landing room and the rooms touching
 * it.
 *
 * One room on its own is what the mode first shipped with, and a room is
 * sometimes a stairwell, a lift lobby or a corridor two strides across: a
 * fight held in one of those is fought against the walls rather than against
 * the guards, and there is nowhere to break line of sight while a clock runs
 * down. The landing room and its neighbours is somewhere to move without being
 * a mission's worth of level.
 *
 * The far side of a *portal*, not of a walk: a room that merely overlaps the
 * landing room in a prop's rooms[] with no door between them is not in it. And
 * one door deep on purpose - two is most of a small map, and the helping of
 * level either side of a hop is the whole point of paying for the load.
 *
 * Built at the landing rather than at the roll, because the landing is the
 * only point that knows where the player actually stands: a stage with no
 * waypoint in a room with a door starts them its own way, and a pad's room is
 * not always the room they end up in. Not for want of a bg - lvReset() loads
 * this stage's rooms and portals before it reads its setup file at all, which
 * is how the roll's own walk over g_Rooms works.
 *
 * **The zone has to have a door out of it**, or a run ends without a death:
 * with the objective done the seal opens, and if every door of every room in
 * the zone leads back into the zone there is nothing left to hop through. On a
 * map small enough for that - an arena of three rooms all touching - the zone
 * is the landing room alone, which is the mode as it was.
 */
static void modRunBuildZone(void)
{
	s32 i;

	g_ModRunNumZone = 0;

	if (g_ModRunLandRoom < 0) {
		return;
	}

	g_ModRunZone[g_ModRunNumZone++] = g_ModRunLandRoom;

	if (g_Rooms == NULL || g_RoomPortals == NULL || g_BgPortals == NULL
			|| g_ModRunLandRoom == 0 || g_ModRunLandRoom >= g_Vars.roomcount) {
		return;
	}

	{
		const struct room *room = &g_Rooms[g_ModRunLandRoom];

		for (i = 0; i < room->numportals && g_ModRunNumZone < MODRUN_MAXZONE; i++) {
			const s32 portalnum = g_RoomPortals[room->roomportallistoffset + i];
			const struct bgportal *portal = &g_BgPortals[portalnum];
			const s32 other = portal->roomnum1 == g_ModRunLandRoom
				? portal->roomnum2
				: portal->roomnum1;

			// Room 0 is not a room the run lands in and not one it seals into.
			if (other <= 0 || other >= g_Vars.roomcount || modRunZoneHas(other)) {
				continue;
			}

			g_ModRunZone[g_ModRunNumZone++] = other;
		}
	}

	if (modRunZoneExitPortal(NULL) < 0) {
		g_ModRunNumZone = 1;
	}

#ifndef PLATFORM_N64
	{
		char rooms[128];
		s32 len = 0;

		rooms[0] = '\0';

		for (i = 0; i < g_ModRunNumZone && len < (s32)sizeof(rooms) - 8; i++) {
			len += sprintf(rooms + len, i == 0 ? "%d" : " %d", g_ModRunZone[i]);
		}

		sysLogPrintf(0, "run: sealing %d room(s) on stage 0x%02x - %s",
				g_ModRunNumZone, g_ModRunStage, rooms);
	}
#endif
}

/**
 * Is the way out shut?
 *
 * The room this hop dealt, with an objective on it that has not been met. Not
 * while the run is landing (there is nobody in the room yet) and not with no
 * objective at all - a landing whose objective could not be allocated leaves
 * the room open rather than sealing the player into one with nothing to do.
 *
 * Sealed Rooms turned off is the mode as it first shipped, and it is read here
 * rather than at the landing so that a run already under way answers to the
 * switch: nothing about a room is dealt differently for it.
 */
bool modRunIsSealed(void)
{
	return modIsRunSealOn()
		&& g_ModRunState == MODRUN_PLAYING
		&& g_ModRunHasObjective
		&& !g_ModRunObjective.done
		&& g_ModRunLandRoom >= 0;
}

/**
 * Whether a guard the alarm is about to place has to be placed where it can
 * reach the player, and whether a given room is such a place.
 *
 * The alarm puts its guards at a waypoint between eight and forty-five metres
 * from the nearest player and lets them walk in, which is right for a mission:
 * the player is somewhere in a level with doors they can open. A sealed room
 * is neither. Eight metres is often the whole of the zone, so every waypoint
 * inside it is refused as too near and every guard starts outside - and then
 * has to get in through whatever the map put between the two, which on a room
 * behind a locked door or at the end of a lift is nothing it can use. The
 * player then stands in an empty room with "Eliminate 5 hostiles" on the HUD,
 * which is exactly the soft lock this mode was reported for.
 *
 * So while a room is sealed the guards are dealt into it: the zone's own
 * rooms, no nearer than MODRUN_GUARDNEAR, and still only where the player
 * cannot see them appear (chrAdjustPosForSpawn() answers that, and it is the
 * reason a guard never pops into view). The alarm falls back to its own rule
 * when the zone has nowhere to put one.
 */
bool modRunGuardsWantZone(void)
{
	return modRunIsSealed() && g_ModRunNumZone > 0;
}

bool modRunGuardRoomOk(s32 room)
{
	return modRunZoneHas(room);
}

f32 modRunGuardMinDist(void)
{
	return MODRUN_GUARDNEAR;
}

/**
 * Say the way is shut, at a pace a person can read.
 *
 * Rate limited because it is said from the walk: the player holding forward
 * against the barrier asks the question sixty times a second.
 */
static void modRunSealSay(const char *why)
{
	static char text[128];

	if (g_Vars.lvframe60 - g_ModRunSealMsg < TICKS(MODRUN_SEALMSG_SECS * 60)) {
		return;
	}

	g_ModRunSealMsg = g_Vars.lvframe60;

	sprintf(text, "%s\n%s\n", why, g_ModRunObjText);
	hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
}

/**
 * The wall the doorway becomes, for the walk to slide the player along.
 *
 * A refused move is a collision like any other and what follows one asks what
 * it hit: bwalk0f0c494c() projects the move along the edge the collision left
 * behind, which is what makes a wall something a player slides down rather
 * than stops dead against. The barrier is a portal - the one leaving the
 * sealed rooms for wherever the move was headed - so the edge is that
 * portal's own plane: its normal turned a quarter turn in XZ, laid through
 * the point the move was going to.
 *
 * The obstacle is nothing. cdSetObstacleVtxProp() writes the edge and clears
 * the prop with it, which matters: the push that runs after a collision reads
 * cdGetObstacleProp(), and a stale door left there by the last real collision
 * would damage the player on a wall that is not there.
 */
static void modRunSealEdge(struct coord *frompos, struct coord *dstpos,
		const RoomNum *torooms, struct coord *vtx1, struct coord *vtx2)
{
	struct coord dir;
	f32 len;

	dir.x = 0;
	dir.y = 0;
	dir.z = 0;

	{
		const s32 portalnum = modRunZoneExitPortal(torooms);

		if (portalnum >= 0) {
			dir.x = g_PortalMetrics[portalnum].normal.z;
			dir.z = -g_PortalMetrics[portalnum].normal.x;
		}
	}

	len = sqrtf(dir.f[0] * dir.f[0] + dir.f[2] * dir.f[2]);

	if (len < 0.01f) {
		// Out of the room through something that is not one of its portals -
		// a room reached over a rooms[] overlap rather than through a door.
		// Across the move, then, which is a wall facing the player.
		dir.x = -(dstpos->z - frompos->z);
		dir.z = dstpos->x - frompos->x;
		len = sqrtf(dir.f[0] * dir.f[0] + dir.f[2] * dir.f[2]);
	}

	if (len < 0.01f) {
		dir.x = 1;
		dir.z = 0;
		len = 1;
	}

	dir.x *= MODRUN_SEALEDGE / len;
	dir.z *= MODRUN_SEALEDGE / len;

	vtx1->x = dstpos->x - dir.x;
	vtx1->y = dstpos->y;
	vtx1->z = dstpos->z - dir.z;

	vtx2->x = dstpos->x + dir.x;
	vtx2->y = dstpos->y;
	vtx2->z = dstpos->z + dir.z;
}

/**
 * Would this move leave the sealed room? From the movement code, before it
 * asks the collision system anything: a true answer is a wall and the move is
 * refused.
 *
 * "Out of the sealed rooms entirely" - the landing room and the rooms touching
 * it, modRunBuildZone() - and the same test the portal is taken by, so that the
 * barrier and the door agree by construction: there is no move that is refused
 * and still a hop, and none that hops without having passed the barrier.
 * Leaning through a doorway - a position that lists both rooms - is neither,
 * which is what lets the player see what is on the other side.
 *
 * A move that starts outside them is nobody's business here. Something
 * that is not the walk can put the player out of a sealed room - a lift, a
 * blast, a fall through a hole - and a barrier that only tested the
 * destination would freeze them where they landed instead of letting them
 * walk back in.
 */
bool modRunSealMove(RoomNum *fromrooms, struct coord *frompos, RoomNum *torooms, struct coord *dstpos)
{
	struct coord vtx1;
	struct coord vtx2;

	if (!modRunIsSealed()) {
		return false;
	}

	if (!modRunRoomsInZone(fromrooms) || modRunRoomsInZone(torooms)) {
		return false;
	}

	modRunSealEdge(frompos, dstpos, torooms, &vtx1, &vtx2);
	cdSetObstacleVtxProp(&vtx1, &vtx2, NULL);

	modRunSealSay("The way out is sealed");

#ifndef PLATFORM_N64
	if (!g_ModRunSealLogged) {
		g_ModRunSealLogged = true;
		sysLogPrintf(0, "run: sealed in room %d (+%d touching) on stage 0x%02x at frame %d - \"%s\"",
				g_ModRunLandRoom, g_ModRunNumZone - 1, g_ModRunStage,
				g_Vars.lvframenum, g_ModRunObjText);
	}
#endif

	return true;
}

/**
 * Keep a sealed room finishable.
 *
 * The seal makes the objective the only way on, and the three kinds are not
 * equally certain: the gun a collect objective names can be destroyed where it
 * lies, and guards sent after the player have to be able to reach the room to
 * be killed in it. Either one leaves a player shut in a room holding something
 * that will never be true.
 *
 * So a room that has stood sealed for too long is dealt a clock, which runs
 * out whatever else is or is not happening. It draws from its own stream for
 * the same reason every other decision does - a re-deal must not move what the
 * seed deals anybody else.
 */
static void modRunTickStuck(void)
{
	bool starved = false;

	// A kill objective is the one that can be made impossible by where the
	// room is rather than by what the player does: the seal holds them in and
	// a door the guards cannot path through holds the guards out. That does
	// not need the full stuck clock to be sure of - nothing hostile having
	// been in the sealed rooms at all for MODRUN_STARVE_SECS is the answer.
	if (g_ModRunObjective.kind == MODRUN_OBJ_KILL) {
		// Something standing in the rooms feeds it, and so does a kill: a
		// player quick enough to drop each guard as it comes through the door
		// leaves the zone empty between them, and that is the objective
		// working rather than the objective starving.
		const s32 kills = g_Vars.currentplayerstats
			? g_Vars.currentplayerstats->killcount - g_ModRunObjective.progress
			: 0;

		if (kills > g_ModRunObjKills || modRunEnemyInZone()) {
			g_ModRunObjKills = kills;
			g_ModRunObjFed = g_Vars.lvframe60;
		}

		starved = g_Vars.lvframe60 - g_ModRunObjFed >= TICKS(MODRUN_STARVE_SECS * 60);
	}

	if (!starved && g_Vars.lvframe60 - g_ModRunObjDealt < TICKS(MODRUN_STUCK_SECS * 60)) {
		return;
	}

	modRunOpen(MODRUN_STREAM_STUCK, g_ModRunHop);
	modRunDealFight(true);

	g_ModRunObjective.progress = g_Vars.lvframe60;
	g_ModRunObjDealt = g_Vars.lvframe60;
	g_ModRunObjFed = g_Vars.lvframe60;
	g_ModRunObjKills = 0;
	g_ModRunSealMsg = 0;

#ifndef PLATFORM_N64
	if (starved) {
		sysLogPrintf(0, "run: nothing has reached room %d on stage 0x%02x for %d seconds; dealing a clock - \"%s\"",
				g_ModRunLandRoom, g_ModRunStage, MODRUN_STARVE_SECS, g_ModRunObjText);
	} else {
		sysLogPrintf(0, "run: room %d on stage 0x%02x stood sealed for %d seconds; dealing a clock - \"%s\"",
				g_ModRunLandRoom, g_ModRunStage, MODRUN_STUCK_SECS, g_ModRunObjText);
	}
#endif

	{
		static char text[128];

		sprintf(text, "New objective\n%s\n", g_ModRunObjText);
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
	}
}

/**
 * The run is over. Keep the best, say what it was, and go back to the menu
 * once the death fade has had time to be read over.
 */
static void modRunEndRun(void)
{
	static char text[96];
	bool best = false;

	g_ModRunState = MODRUN_OVER;
	g_ModRunOverTicks = 0;

	if (g_ModRunScore > g_ModOptions.runbestscore
			|| (g_ModRunScore == g_ModOptions.runbestscore && g_ModRunHop > g_ModOptions.runbestrooms)) {
		g_ModOptions.runbestscore = g_ModRunScore;
		g_ModOptions.runbestrooms = g_ModRunHop;
		best = true;
	}

	if (best) {
		sprintf(text, "Run over: %d objectives, %d rooms - best yet\n", g_ModRunScore, g_ModRunHop);
	} else {
		sprintf(text, "Run over: %d objectives, %d rooms (best %d)\n",
				g_ModRunScore, g_ModRunHop, g_ModOptions.runbestscore);
	}

	{
		extern struct hudmsgtype g_HudmsgTypes[];
		hudmsgCreateWithDuration(text, HUDMSGTYPE_DEFAULT, &g_HudmsgTypes[HUDMSGTYPE_DEFAULT], MODRUN_MSGTICKS);
	}

#ifndef PLATFORM_N64
	sysLogPrintf(0, "run: over - seed %u, %d objectives, %d rooms, on stage 0x%02x at frame %d",
			g_ModRunSeed, g_ModRunScore, g_ModRunHop, g_Vars.stagenum, g_Vars.lvframenum);
#endif
}

/**
 * Step through the portal: keep the kit, fade, and deal the next room.
 */
static void modRunTakePortal(void)
{
	modRunSnapshotKit();

#ifndef PLATFORM_N64
	sysLogPrintf(0, "run: portal out of room %d on stage 0x%02x at frame %d, %d guns, health %.2f",
			g_ModRunLandRoom, g_ModRunStage, g_Vars.lvframenum,
			g_ModRunCarry.numweapons, g_ModRunCarry.health);
#endif

	g_ModRunState = MODRUN_HOPPING;

	playerSetFadeColour(0, 0, 0, 0);
	playerSetFadeFrac(TICKS(20), 1);
}

/**
 * The run, from playerTick().
 *
 * Four things in order, because each one can be true on the frame the last one
 * became true: place the player, put their kit back, watch the objective, and
 * watch for the door.
 */
void modRunTick(void)
{
	struct player *player = g_Vars.currentplayer;

#ifndef PLATFORM_N64
	// --random-run: begin a run as soon as there is a level to leave, so that
	// a headless run can be driven without pressing anything. A menu cannot
	// be pressed from a script and the mode is otherwise only reachable
	// through one.
	if (g_ModRunAutoStart && g_ModRunState == MODRUN_OFF && g_Vars.lvframenum > 60) {
		g_ModRunAutoStart = false;
		modRunStart();
		return;
	}
#endif

	if (!modRunIsOn() || player == NULL || player != g_Vars.bond) {
		return;
	}

	if (g_ModRunState == MODRUN_OVER) {
		// The death's own fade is running under this. Long enough to read the
		// score, then the Institute, which is where the Perfect Menu and the
		// run's own page are.
		if (++g_ModRunOverTicks > MODRUN_MSGTICKS) {
			modRunStop();
			titleSetNextStage(STAGE_CITRAINING);
			titleSetNextMode(TITLEMODE_SKIP);
			mainChangeToStage(STAGE_CITRAINING);
		}

		return;
	}

	if (g_ModRunState == MODRUN_HOPPING) {
		if (playerIsFadeComplete() && player->colourscreenfrac >= 1) {
			g_ModRunHop++;
			modRunEnterStage(modRunChooseStage());
		}

		return;
	}

	// The stage the hop asked for, and not the one it is leaving:
	// mainChangeToStage() takes effect at the end of the frame, so the level
	// that asked for a hop keeps ticking under this for the rest of it.
	if (g_Vars.stagenum != g_ModRunStage) {
		// And if the hop's stage has already arrived once, a different one
		// means the game left the run some way this file did not ask for -
		// Abort Mission, a script that changes stage, the title screen. The
		// run is over rather than quietly still on, which would deal the next
		// ordinary mission as one of its rooms.
		if (!g_ModRunPendingLoad) {
			modRunStop();
		}

		return;
	}

	g_ModRunPendingLoad = false;

	// A mission's opening is a mission's, and a run is landing in one room of
	// it: Crash Site's cutscene held the first landing off for forty-five
	// seconds. This is the same request pressing a button makes, after the
	// same thirty frames the game waits before it will take one.
	if (g_InCutscene || g_Vars.in_cutscene) {
		if (g_CutsceneCurTotalFrame60f > 30) {
			g_CutsceneSkipRequested = true;

			if (g_Vars.autocutplaying) {
				g_Vars.autocutgroupskip = true;
			}
		}

		return;
	}

	if (g_Vars.lvframenum < 2) {
		return;
	}

	// A death ends the run - but not one the run has not landed in yet. A map
	// loaded solo that was never asked to hold a mission can leave the player
	// dead on arrival: the arenas do, at level frame 2, and an hour-old run
	// ending because the next room was Temple would be the mode's worst
	// moment. The landing is a new life, and a new life is what cures it -
	// playerStartNewLife() clears isdead and hands back full health - so a
	// death before the landing is left to the landing.
	if (player->isdead && g_ModRunState != MODRUN_LANDING) {
		modRunEndRun();
		return;
	}

	if (g_ModRunState == MODRUN_LANDING) {
		// A map that left the player dead and gave the run nowhere to put a
		// landing is one this build cannot hold a room in - respawning into
		// wherever it left them would be a landing inside the geometry. Hop
		// on rather than ending the run on it, and give up after a few, since
		// a pool where every map does this would otherwise load forever.
		if (player->isdead && g_ModRunLandPad < 0) {
			if (++g_ModRunSkips > MODRUN_MAXSKIPS) {
#ifndef PLATFORM_N64
				sysLogPrintf(1, "run: %d maps in a row could not be landed in; ending the run",
						g_ModRunSkips);
#endif
				modRunEndRun();
			} else {
#ifndef PLATFORM_N64
				sysLogPrintf(1, "run: stage 0x%02x has no landing and starts the player dead; skipping it",
						g_ModRunStage);
#endif
				g_ModRunHop++;
				modRunEnterStage(modRunChooseStage());
			}

			return;
		}

		if (g_ModRunSpawnState == 1) {
			g_ModRunSpawnState = 2;
			player->dostartnewlife = true;

			return;
		}

		if (g_ModRunSpawnState == 2) {
			// Asked for. lvRender() calls playerStartNewLife() later in the
			// same frame and it clears the flag on its way past, which is
			// what says the landing has happened.
			if (player->dostartnewlife) {
				return;
			}

			g_ModRunSpawnState = 0;

			return;
		}

		g_ModRunSkips = 0;
		g_ModRunKitDue = false;

		// The map's own intro arms the player too, and the roll cannot see
		// what it will hand out. A collect objective the player can already
		// answer is a free point, so it becomes a fight instead.
		if (g_ModRunObjective.kind == MODRUN_OBJ_COLLECT
				&& invHasSingleWeaponIncAllGuns(g_ModRunObjective.target)) {
			modRunOpen(MODRUN_STREAM_OBJ, g_ModRunHop);
			modRunDealFight(modRunBelow(2) != 0);
		}

		// Where the player actually stands is the room the portal is measured
		// against, which is not always the landing pad's: a stage with no
		// waypoint in a room with a door starts the player its own way.
		g_ModRunLandRoom = player->prop->rooms[0];

		// What the seal shuts, which is that room and the rooms touching it.
		// Here rather than at the roll because this is the first point that
		// knows where the player actually stands - see modRunBuildZone().
		modRunBuildZone();

		g_ModRunObjective.progress = g_ModRunObjective.kind == MODRUN_OBJ_KILL
			? (g_Vars.currentplayerstats ? g_Vars.currentplayerstats->killcount : 0)
			: g_Vars.lvframe60;

		// The seal's own clocks, which start where the player does rather than
		// at the roll: the roll ran before there was a player standing in the
		// room to start them.
		g_ModRunObjDealt = g_Vars.lvframe60;
		g_ModRunObjFed = g_Vars.lvframe60;
		g_ModRunObjKills = 0;
		g_ModRunSealMsg = 0;
		g_ModRunSealLogged = false;

		g_ModRunState = MODRUN_PLAYING;

#ifndef PLATFORM_N64
		sysLogPrintf(0, "run: landed on stage 0x%02x in room %d at frame %d, health %.2f, standing at %.0f,%.0f,%.0f",
				g_ModRunStage, g_ModRunLandRoom, g_Vars.lvframenum, player->bondhealth,
				player->prop->pos.x, player->prop->pos.y, player->prop->pos.z);
#endif

		{
			static char text[128];

			sprintf(text, "%s - room %d\n%s\n",
					modRunGetStageName(g_ModRunStage), g_ModRunHop + 1, g_ModRunObjText);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		}

		return;
	}

	modRunTickObjective();

#ifndef PLATFORM_N64
	// --run-autohop N: take the portal after N frames in the room, without
	// walking to a door. A headless run cannot walk, and what a chain of hops
	// exercises - every map in the pool loading, the kit surviving the loads,
	// the stage pool coming back - is the half of this mode a person cannot
	// test by playing it once. It hops through the seal as well, which is the
	// only way a chain of maps can be walked in a minute: the seal is a wall
	// to the player, and this is not the player.
	if (g_ModRunAutoHop > 0 && g_Vars.lvframenum > g_ModRunAutoHop) {
		modRunTakePortal();
		return;
	}
#endif

	// Until the objective is done there is no door: the doorway is a wall from
	// the inside (modRunSealMove(), called by the movement code) and nothing
	// below this can happen. What can still put the player outside a sealed
	// room is everything that moves them without asking the walk - a lift, a
	// blast, a fall - and that is not a portal either: the objective is still
	// this room's, and the room is still where it has to be answered.
	if (modRunIsSealed()) {
		if (!modRunRoomsInZone(player->prop->rooms)) {
			modRunSealSay("Return to the room");
		}

		modRunTickStuck();

		return;
	}

	// The door. Any room outside the sealed ones is through one, which is what
	// makes every door out of them a portal without any of them having to be
	// marked: the run does not care which door, only that the player is no
	// longer in the rooms it put them in.
	//
	// Out of them entirely, not merely standing somewhere whose first room is
	// another one: a prop straddling a portal lists both rooms and which of
	// them is rooms[0] changes while the player stands in the doorway, so
	// testing that alone hops a run for leaning through a door.
	if (modRunRoomsInZone(player->prop->rooms)) {
		return;
	}

	modRunTakePortal();
}

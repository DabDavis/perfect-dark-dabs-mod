#include <ultra64.h>
#include "constants.h"
#include "game/bg.h"
#include "game/chraction.h"
#include "game/game_0b0fd0.h"
#include "game/hudmsg.h"
#include "game/lang.h"
#include "game/atan2f.h"
#include "game/lv.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "mod.h"
#include "game/objectives.h"
#include "game/pad.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "bss.h"
#include "lang.h"
#include "lib/collision.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/str.h"
#include "data.h"
#include "types.h"
#include "platform.h"

#ifndef PLATFORM_N64
extern void sysLogPrintf(s32 level, const char *fmt, ...);
#endif

/**
 * Randomizer: a mission dealt again from its own pieces.
 *
 * A stage's setup file is a command stream - one command per weapon, guard,
 * key, door, crate and objective - and the port loads it into a writable copy
 * of its own before anything is built from it (setupLoadFiles(), and
 * setupCreateProps() walks the copy afterwards). Everything here happens in
 * the gap between those two: the stream is rewritten, and the game then
 * builds the level the rewrite describes, with no idea it was not the one on
 * the disc.
 *
 * What it does not do is move geometry. One bg file is resident at a time and
 * a portal's room numbers index that one file, so rooms cannot be dealt from
 * different stages - see the note in CLAUDE-notes. The rooms are the stage's
 * own; what changes is everything standing in them, and which of them you can
 * get to.
 *
 * **Shuffled within a class, not scattered.** A weapon moves to a spot that
 * held a weapon, a guard to a pad that held a guard. The level's designer put
 * those spots where a thing can stand - on the floor, out of a wall, inside
 * the room's collision - and a random point in a room is none of those. So
 * placement is a permutation of the stage's own slots, and it is the *kind*
 * of thing in each slot that is rolled freely: any of the game's weapons can
 * turn up in any weapon spot, any ammo in any crate. That is where the
 * variety comes from, and it cannot put a gun inside a wall.
 *
 * **The portal graph decides whether a seed is playable.** Rooms are a graph
 * already - g_Rooms[i] lists its portals and each portal names the two rooms
 * it joins - and a locked door is an edge that needs a key. So after the keys
 * are moved the generator walks that graph from the spawn room, taking the
 * keys it finds and opening what they open, until nothing new is reachable.
 * Objectives are then only ever placed in rooms that walk reached. A seed
 * that cannot be finished is the one thing a randomizer must never hand a
 * player, and the graph is the only way to know before they have spent an
 * hour finding out.
 *
 * The walk is honest about what it does not model: lifts, hatches, ladders
 * and anything a mission script opens are not portals with doors on them, so
 * the walk can call a room unreachable that a player can reach. It is used
 * only to *reject* placements, never to prove one impossible, and the fallback
 * when it rejects everything is the stage's own key placement.
 *
 * **The seed is the run.** Mod.RandomizerSeed in pd.ini, or the menu: zero
 * means a fresh roll every mission, anything else is a run that can be
 * written down, replayed and handed to somebody else. A seed and a stage
 * always deal the same mission, on any machine, because the roll is this
 * file's own xorshift and never the game's rngRandom(), whose state depends
 * on how many frames the title screen was left running.
 *
 * **And it keeps dealing it after this file changes**, which is the point of
 * the streams: every decision draws from its own, so working on one part of
 * the roll cannot move the others. What a change to the *meaning* of a draw
 * does is covered by MODRANDOM_VERSION, and the fold printed with the seed is
 * how a change is checked - see modRandomOpen() and modRandomFold().
 */

/**
 * The generator's version, and what it is for.
 *
 * A seed is only worth writing down if it deals the same mission tomorrow.
 * Most of that is the streams below, which keep a change to one decision from
 * moving every other; this number covers the rest - a change to what the
 * draws *mean*, which no amount of stream separation can absorb.
 *
 * The rule for changing this file: if a change would deal a different mission
 * from the same seed, put it behind `g_ModRandomVersion >= N`, leave the older
 * behaviour where it is, and raise this to N. A run then keeps the generator
 * it was dealt by (Mod.RandomizerVersion in pd.ini, written beside the seed),
 * and a shared seed carries its version with it. Adding a new stream id, or
 * changing anything a draw does not feed, needs no bump.
 *
 * Version 1 is the first versioned generator. The build before it drew every
 * decision from one walked stream, so seeds from it are not reproducible here
 * and are not claimed to be.
 *
 * Version 2 widened where a "reach this room" objective may send the player,
 * from the deepest rooms the walk found to the deep half of them - Endless
 * Mode asks for a room over and over and four of them is not enough to ask
 * about. v1 runs keep the narrow rule.
 *
 * Version 3 asks whether a pad is somewhere a player can be stood up alive
 * before a start or a landing is put on it - modRandomPadSpawnPos() - which
 * moves the start on any stage whose pads include one that is not. v1 and v2
 * runs keep taking the pad on trust, out of bounds and all.
 */
#define MODRANDOM_VERSION MODRANDOM_VERSION_DEFAULT

// One stream per kind of decision. Ids are permanent: changing one is
// changing every seed. New kinds take the next number.
#define MODRANDOM_STREAM_WEAPON    1 // index: which weapon spot
#define MODRANDOM_STREAM_CRATE     2 // index: which crate
#define MODRANDOM_STREAM_CHRPADS   3 // the guards' shuffle
#define MODRANDOM_STREAM_INTROGUN  4 // index: which intro weapon command
#define MODRANDOM_STREAM_SPAWN     5 // index: which try
#define MODRANDOM_STREAM_KEYS      6 // index: which try
#define MODRANDOM_STREAM_OBJCOUNT  7 // how many objectives
#define MODRANDOM_STREAM_OBJPOOL   8 // the order the fetchable things are taken in
#define MODRANDOM_STREAM_OBJROOM   9 // index: which objective's room

struct modrandomstream {
	u32 state;
};

// The intro stream's command, as playerReset() reads it.
struct modrandomintrocmd {
	s32 type;
	s32 param1;
	s32 param2;
	s32 param3;
};

#define MODRANDOM_MAXOBJECTIVES 3  // generated per mission
#define MODRANDOM_OBJBLOCK      64 // bytes an objective's commands get, 16 aligned
#define MODRANDOM_MAXTRIES      16 // key placements tried before giving up on the roll
#define MODRANDOM_TEXTLEN       64

// What a pad has to be for a player to be put down on it. The lift is how far
// above the pad the ground is asked for, since the search takes the highest
// floor strictly *below* the position it is given and a pad standing exactly
// on its own floor would otherwise miss it; the drop is how far under the pad
// that floor may be before the pad is over a catwalk rather than on a floor;
// and -100000 is what the collision system answers when there is no floor at
// all, which is the game's own threshold for the same question
// (chrAdjustPosForSpawn()).
#define MODRANDOM_SPAWNLIFT      10.0f
#define MODRANDOM_SPAWNDROP      400.0f
#define MODRANDOM_NOGROUND       (-100000.0f)

static u32 g_ModRandomSeed;    // the run's, as the player sees it
static s32 g_ModRandomVersion; // the generator this run is dealt by

// The generated objectives: their command stream, which objectiveCheck()
// walks exactly as it walks the setup file's own, and their text, which the
// briefing and the HUD ask for by objective index.
// What the roll found in the stream, gathered once and used by every step.
struct modrandomlists {
	struct weaponobj **weapons;   // floor pickups only - see modRandomGather()
	struct defaultobj **crates;
	struct packedchr **chrs;
	struct doorobj **doors;
	struct keyobj **keys;
	struct tag **tags;
	struct defaultobj **tagobjs; // what each tag names, resolved at roll time
	u32 *portalkeys;             // what each portal wants before it can be walked through
	s32 numportals;
	s32 numweapons;
	s32 numcrates;
	s32 numchrs;
	s32 numdoors;
	s32 numkeys;
	s32 numtags;
};

// Endless Mode's run: what the level's roll found, kept for the whole level so
// the next objective can be dealt during play, and the score, which is how much
// of the level the player got through before dying.
static struct modrandomlists g_ModRandomLists;
static u8 *g_ModRandomReached;   // the walk, from the start the roll chose
static u8 *g_ModRandomVisited;   // rooms the player has stood in
static s32 g_ModRandomRooms;     // how many of them
static s32 g_ModRandomCleared;   // objectives finished this run
static s32 g_ModRandomDealt;     // objectives dealt, which is the draw's index
static bool g_ModRandomRunOver;

// Where the roll wants the mission to start, and whether the player has been
// put there yet: 0 nothing to move, 1 waiting for the mission to begin,
// 2 asked for.
static s32 g_ModRandomSpawnPad = -1;
static s32 g_ModRandomSpawnState;

static u32 *g_ModRandomObjCmds;
static struct weaponobj **g_ModRandomPool; // things left to be asked for, in the order they will be
static s32 g_ModRandomPoolSize;
static s32 g_ModRandomLastRoom = -1;       // so the next room objective is not the one just reached
static char g_ModRandomObjText[MODRANDOM_MAXOBJECTIVES][MODRANDOM_TEXTLEN];
static s32 g_ModRandomNumObjectives;


/**
 * Whether this stage is one to deal again.
 *
 * The Carrington Institute is the backdrop the Perfect Menu is drawn over, and
 * it is a level like any other as far as the code is concerned - it loads a
 * setup file with weapons, guards and pads in it, and the first version of
 * this rolled it. That put a random gun in the firing range, moved where the
 * player stands behind the menu and dealt an objective in a building with no
 * mission, all before the title screen had finished drawing.
 */
static bool modRandomStageIsMission(s32 stagenum)
{
	return STAGE_IS_LEVEL(stagenum) && stagenum != modDataBgStage(STAGE_CITRAINING);
}

/**
 * Whether a mission is being dealt again.
 *
 * Solo only. The Combat Simulator deals its own arena from mpsetup, and
 * co-operative and counter-operative have a second player whose start this
 * would move out from under them.
 */
/**
 * A mission armed as a random one from the Randomizer page, or by
 * --random-mission for a headless run that cannot press the page's button.
 *
 * The arming is a flag and not a setting, the way Ghost Trials arms a trial:
 * a setting is saved to pd.ini, and coming in through that door for one
 * mission should not leave every later mission dealt again. There was such a
 * setting once - Mod.Randomizer, the checkbox that the Randomizer page
 * replaced - and it outlived its checkbox in every pd.ini written while it
 * was ticked, dealing every Solo Mission again with nothing left in the menu
 * to switch it off. So nothing in pd.ini turns the roll on: the two doors
 * on the Randomizer page do, and the stock Solo Missions item disarms it on
 * the way past.
 */
static bool g_ModRandomArmed;

void modRandomArmMission(void)
{
	g_ModRandomArmed = true;
}

void modRandomDisarmMission(void)
{
	g_ModRandomArmed = false;
}

bool modRandomIsArmed(void)
{
	return g_ModRandomArmed;
}

bool modRandomIsOn(void)
{
	// A run deals every map it lands in, whatever the setting says: the roll
	// is what makes a landing worth making twice. See modrun.c.
	return (g_ModRandomArmed || modRunIsOn())
		&& !g_Vars.normmplayerisrunning
		&& !g_Vars.mplayerisrunning
		&& modRandomStageIsMission(g_Vars.stagenum);
}

/**
 * Endless Mode: the mission never ends, it deals another objective.
 *
 * A mission's objectives run out, which is the point of a mission and the
 * opposite of the point of a randomizer - the interesting part is the next
 * unknown thing, and stock runs out of those after three. So this deals one
 * objective at a time and deals another the moment it is finished, and the run
 * ends where a run should end, at the first death.
 *
 * The score is rooms: how much of the level the player got through before
 * dying, counted as rooms stood in and never counted twice. Kills would score
 * standing still in a doorway and objectives would score the same run twice,
 * but a room is ground covered, and covering ground is what the objectives are
 * for.
 */
bool modRandomIsEndless(void)
{
	// Not during a run: a run deals its own objective for the room it landed
	// in and scores that, and Endless Mode dealing a second one across the
	// whole map would be two modes asking for two different things at once.
	return modRandomIsOn() && !modRunIsOn() && g_ModOptions.randomendless != 0;
}

/**
 * The run so far: rooms covered and objectives finished. For the menu, and for
 * anything that wants to show a run's score while it is still being made.
 */
s32 modRandomGetRooms(void)
{
	return g_ModRandomRooms;
}

s32 modRandomGetCleared(void)
{
	return g_ModRandomCleared;
}

/**
 * The generator the current mission was dealt by, for the menu to show
 * beside the seed: the two together are the run.
 */
s32 modRandomGetVersion(void)
{
	return g_ModRandomVersion ? g_ModRandomVersion : MODRANDOM_VERSION;
}

/**
 * The seed the current mission was dealt from, for the menu to show.
 */
u32 modRandomGetSeed(void)
{
	return g_ModRandomSeed;
}

/**
 * xorshift32, and a mix on the way in so that seed 1 and seed 2 are different
 * missions rather than the same one a step apart.
 */
/**
 * The run's own number, mixed once: the seed, the stage and the difficulty.
 * Every stream below hangs off it, so one run's Villa and its Chicago are
 * different missions and Perfect Agent is its own deal rather than the Agent
 * one with more guards.
 */
static u32 g_ModRandomRun;

/**
 * A 32 bit finalizer - the mix from splitmix64's tail, in 32 bits. Every bit
 * of the output depends on every bit of the input, which is what makes seed 1
 * and seed 2 different missions rather than the same one a step apart.
 */
static u32 modRandomMix(u32 x)
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;

	return x;
}

/**
 * Open the stream for one decision.
 *
 * **This is what makes a seed survive a change to the generator.** A single
 * PRNG walked from the first decision to the last means every draw's value
 * depends on how many draws came before it: adding one retry to the weapon
 * roll moves the guards, the keys and the objectives, and a seed written down
 * last week deals a different mission today. That is exactly what happened
 * between the first two builds of this file.
 *
 * So there is no single stream. Each decision opens its own, seeded from the
 * run, a stream id naming what the decision is *about*, and an index naming
 * which one of them it is - the fourth weapon spot, the second objective. A
 * draw's value then depends on nothing but those three, so:
 *
 * - changing how the weapon roll works moves weapons, and nothing else;
 * - adding a whole new kind of thing to randomize takes a new stream id and
 *   disturbs no existing one, which is the common case and needs no version
 *   bump at all;
 * - the order the roll runs its steps in stops mattering.
 *
 * What it cannot save is a change to what a draw *means* - a weapon dropped
 * from the pool, a different rule for which pads can be a start. Those change
 * the mission a seed deals however the numbers are drawn, and those are what
 * MODRANDOM_VERSION and the guards on it are for.
 */
static void modRandomOpen(struct modrandomstream *rng, s32 streamid, s32 index)
{
	u32 state = modRandomMix(g_ModRandomRun + modRandomMix(streamid * 0x9e3779b9u + index));

	rng->state = state ? state : 0x9e3779b9;
}

static u32 modRandomNext(struct modrandomstream *rng)
{
	rng->state ^= rng->state << 13;
	rng->state ^= rng->state >> 17;
	rng->state ^= rng->state << 5;

	return rng->state;
}

static u32 modRandomBelow(struct modrandomstream *rng, u32 limit)
{
	return limit ? modRandomNext(rng) % limit : 0;
}

/**
 * Fisher-Yates over an array of pointers, out of one stream: a shuffle is one
 * decision however many swaps it takes.
 */
static void modRandomShuffle(struct modrandomstream *rng, void **array, s32 count)
{
	s32 i;

	for (i = count - 1; i > 0; i--) {
		s32 j = modRandomBelow(rng, i + 1);
		void *tmp = array[i];

		array[i] = array[j];
		array[j] = tmp;
	}
}

static void *modRandomAlloc(s32 count, s32 size)
{
	if (count <= 0) {
		return NULL;
	}

	return mempAlloc(ALIGN16(count * size), MEMPOOL_STAGE);
}

/**
 * A pad's room, or -1. Valid only once setupPreparePads() has filled in the
 * rooms the file left at -1, which is why the roll runs after it.
 */
static s32 modRandomPadRoom(s32 padnum)
{
	struct pad pad;

	if (padnum < 0 || g_PadsFile == NULL || padnum >= g_PadsFile->numpads) {
		return -1;
	}

	// Not chrGetPadRoom(): that one is for AI script parameters, where a value
	// under 10000 is already a room number and is handed straight back, so
	// asking it about a pad gets the pad number returned as a room.
	padUnpack(padnum, PADFIELD_ROOM, &pad);

	return pad.room;
}

/**
 * Where a player put down on this pad would actually stand, or nothing when
 * the answer is "nowhere they would survive".
 *
 * A spawn hands a position to playerStartNewLife(), which asks
 * cdFindGroundInfoAtCyl() for the floor under it and stands the player on what
 * comes back. That search takes the highest floor **strictly below** the y it
 * is given, and it has one answer for finding nothing: -4294967296. A pad with
 * no floor beneath it therefore does not fail, it succeeds with the player
 * four billion units under the level, falling, and that is the whole of
 * "sometimes it spawns you out of bounds and you die". It is not rare - 2% to
 * 8% of a stage's waypoints are like it, 31 of one stage's 371 - and every one
 * of them is a landing the run was willing to deal.
 *
 * The other way a spawn kills is a GEOFLAG_DIE tile, which bondwalk.c kills
 * whoever stands on at the first walk tick.
 *
 * So the question is asked here instead, from MODRANDOM_SPAWNLIFT above the
 * pad so that the floor the pad is standing on is inside the search rather
 * than exactly level with its edge, and what is handed back is a position on
 * **the floor that was found**, not the pad's own. The game's query then
 * repeats this one and comes to the same floor.
 *
 * This runs at the roll, inside setupCreateProps(), which is late enough:
 * lvReset() loads the bg and builds its tables before it reads the setup file
 * at all, and bodyAllocateChr() does its own cdTestVolume() from the same
 * walk.
 *
 * The position and the verdict are two calls because they are asked at
 * different moments and want different answers - see modRandomPadCanSpawn().
 */
static bool modRandomPadGround(s32 padnum, struct pad *pad, f32 *ground, u16 *floorflags)
{
	struct coord query;
	RoomNum rooms[2];
	RoomNum floorroom = -1;

	*floorflags = 0;

	if (padnum < 0 || g_PadsFile == NULL || padnum >= g_PadsFile->numpads) {
		return false;
	}

	padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, pad);

	if (pad->room <= 0 || pad->room >= g_Vars.roomcount) {
		return false;
	}

	rooms[0] = pad->room;
	rooms[1] = -1;

	query = pad->pos;
	query.y += MODRANDOM_SPAWNLIFT;

	*ground = cdFindGroundInfoAtCyl(&query, 30, rooms, NULL, NULL, floorflags, &floorroom, NULL, NULL);

	// Nothing under the pad at all, which is the whole of the fault: there is
	// no position to hand back that is not four billion units under the level.
	return floorroom >= 0 && *ground >= MODRANDOM_NOGROUND;
}

bool modRandomPadSpawnPos(s32 padnum, struct coord *pos, RoomNum *room)
{
	struct pad pad;
	u16 floorflags;
	f32 ground;

	if (!modRandomPadGround(padnum, &pad, &ground, &floorflags)) {
		return false;
	}

	if (pos) {
		pos->x = pad.pos.x;
		pos->y = ground + MODRANDOM_SPAWNLIFT;
		pos->z = pad.pos.z;
	}

	if (room) {
		*room = pad.room;
	}

	return true;
}

/**
 * And whether it is a pad worth putting them on at all.
 *
 * The verdict rather than the position: a floor under the pad, not one that
 * kills whoever stands on it, not so far below that the pad is over a drop
 * rather than on a floor, and room for someone to stand. This is what a
 * chooser asks before it deals a pad; modRandomPadSpawnPos() above is what
 * the spawn itself asks once one has been dealt, and answers for a pad this
 * would refuse, since a start already committed to is better placed on its
 * floor than on the pad's own y.
 *
 * Asked at the roll, where the level's own props do not exist yet: the volume
 * test therefore sees the bg alone, which is the same bg the ground came from.
 */
bool modRandomPadCanSpawn(s32 padnum)
{
	struct pad pad;
	RoomNum rooms[2];
	u16 floorflags;
	f32 ground;

	if (!modRandomPadGround(padnum, &pad, &ground, &floorflags)) {
		return false;
	}

	// A tile that kills whoever stands on it, on the first walk tick.
	if (floorflags & GEOFLAG_DIE) {
		return false;
	}

	// A pad over a drop rather than on a floor: the player would be put down
	// at the bottom of it, which is not where the pad is.
	if (pad.pos.y - ground > MODRANDOM_SPAWNDROP) {
		return false;
	}

	rooms[0] = pad.room;
	rooms[1] = -1;

	// And room to stand, the test bodyAllocateChr() puts every one of the
	// stage's own guards through.
	if (cdTestVolume(&pad.pos, 20, rooms, CDTYPE_ALL, CHECKVERTICAL_YES, 200, -200) == CDRESULT_COLLISION) {
		return false;
	}

	return true;
}

/**
 * Whether an object stands somewhere of its own, so that moving it means
 * moving a thing in the world.
 *
 * A weapon in a guard's hands or inside a crate has a chr id or a container
 * in the field a free-standing one keeps its pad in (setupPlaceWeapon() reads
 * it as a chr id), so those are left where they are. Their *type* is still
 * rolled - a guard carrying something unexpected is the point - but nothing
 * touches where they are.
 */
static bool modRandomObjIsFreeStanding(struct defaultobj *obj)
{
	return (obj->flags & (OBJFLAG_ASSIGNEDTOCHR | OBJFLAG_INSIDEANOTHEROBJ)) == 0;
}

/**
 * Walk the stream once to count and once to fill.
 */
static void modRandomGather(struct modrandomlists *lists)
{
	struct defaultobj *obj;
	s32 *tagcmdindexes = NULL;
	s32 pass;
	s32 i;

	for (pass = 0; pass < 2; pass++) {
		s32 cmdindex = 0;

		lists->numweapons = 0;
		lists->numcrates = 0;
		lists->numchrs = 0;
		lists->numdoors = 0;
		lists->numkeys = 0;
		lists->numtags = 0;

		obj = (struct defaultobj *)g_StageSetup.props;

		while (obj->type != OBJTYPE_END) {
			switch (obj->type) {
			case OBJTYPE_WEAPON:
				if (pass) {
					lists->weapons[lists->numweapons] = (struct weaponobj *)obj;
				}
				lists->numweapons++;
				break;
			case OBJTYPE_AMMOCRATE:
				if (pass) {
					lists->crates[lists->numcrates] = obj;
				}
				lists->numcrates++;
				break;
			case OBJTYPE_CHR:
				if (pass) {
					lists->chrs[lists->numchrs] = (struct packedchr *)obj;
				}
				lists->numchrs++;
				break;
			case OBJTYPE_DOOR:
				if (pass) {
					lists->doors[lists->numdoors] = (struct doorobj *)obj;
				}
				lists->numdoors++;
				break;
			case OBJTYPE_KEY:
				if (pass) {
					lists->keys[lists->numkeys] = (struct keyobj *)obj;
				}
				lists->numkeys++;
				break;
			case OBJTYPE_TAG:
				if (pass) {
					lists->tags[lists->numtags] = (struct tag *)obj;
					tagcmdindexes[lists->numtags] = cmdindex;
				}
				lists->numtags++;
				break;
			}

			obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
			cmdindex++;
		}

		if (pass == 0) {
			lists->weapons = modRandomAlloc(lists->numweapons, sizeof(void *));
			lists->crates = modRandomAlloc(lists->numcrates, sizeof(void *));
			lists->chrs = modRandomAlloc(lists->numchrs, sizeof(void *));
			lists->doors = modRandomAlloc(lists->numdoors, sizeof(void *));
			lists->keys = modRandomAlloc(lists->numkeys, sizeof(void *));
			lists->tags = modRandomAlloc(lists->numtags, sizeof(void *));
			lists->tagobjs = modRandomAlloc(lists->numtags, sizeof(void *));
			tagcmdindexes = modRandomAlloc(lists->numtags, sizeof(s32));

			if ((lists->numtags && (!lists->tagobjs || !tagcmdindexes))
					|| (lists->numweapons && !lists->weapons)
					|| (lists->numcrates && !lists->crates)
					|| (lists->numchrs && !lists->chrs)
					|| (lists->numdoors && !lists->doors)
					|| (lists->numkeys && !lists->keys)
					|| (lists->numtags && !lists->tags)) {
				// The stage pool is out. Nothing has been rewritten yet, so
				// the mission the disc describes is still intact.
				lists->numweapons = lists->numcrates = lists->numchrs = 0;
				lists->numdoors = lists->numkeys = lists->numtags = 0;
				return;
			}
		}
	}

	// A tag names the object a fixed number of commands along from itself, and
	// nothing has resolved that yet: the walk that sets OBJHFLAG_TAGGED and
	// fills tag->obj is what runs after the roll. objGetTagNum() is therefore
	// useless here and this is the same sum, done early.
	for (i = 0; i < lists->numtags; i++) {
		lists->tagobjs[i] = setupGetObjByCmdIndex(tagcmdindexes[i] + lists->tags[i]->cmdoffset);
	}
}

/**
 * The tag a generated objective can name this object by, or -1.
 */
static s32 modRandomTagNumOf(struct modrandomlists *lists, struct defaultobj *obj)
{
	s32 i;

	for (i = 0; i < lists->numtags; i++) {
		if (lists->tagobjs[i] == obj) {
			return lists->tags[i]->tagnum;
		}
	}

	return -1;
}

/**
 * Roll the kind of thing standing in each slot.
 *
 * g_MpWeapons is the game's own list of what can be a pickup - a weapon
 * number, the model that stands for it on the floor and the scale it is drawn
 * at, which have to agree or the pickup is the wrong shape - so the roll picks
 * a row of that table rather than a weapon number on its own. Index 0 is
 * "Nothing" and is skipped: an empty weapon spot is a spot the player walks
 * past, and a mission where half of them are empty reads as a bug.
 */
static void modRandomRollWeapons(struct modrandomlists *lists)
{
	s32 i;

	for (i = 0; i < lists->numweapons; i++) {
		struct weaponobj *weapon = lists->weapons[i];
		struct modrandomstream rng;
		// A weapon in a guard's hands has to be something a guard can fire:
		// the table holds the specs, the cloaking device and the boost pill
		// as well, and a guard issued a pair of X-Ray specs is a guard that
		// stands there. Ammo is what tells the two apart.
		bool forchr = (weapon->base.flags & OBJFLAG_ASSIGNEDTOCHR) != 0;
		s32 tries;

		modRandomOpen(&rng, MODRANDOM_STREAM_WEAPON, i);

		for (tries = 0; tries < 16; tries++) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, NUM_MPWEAPONS - 1)];

			if (mpweapon->weaponnum == WEAPON_NONE || mpweapon->model < 0) {
				continue;
			}

			if (forchr && mpweapon->priammotype <= 0) {
				continue;
			}

			weapon->weaponnum = mpweapon->weaponnum;
			weapon->base.modelnum = mpweapon->model;
			weapon->base.extrascale = mpweapon->extrascale;
			break;
		}
	}
}

/**
 * ... and what is in the crates. The ammo types a crate can hold are the ones
 * a weapon in the table above takes, so the roll draws from the same table
 * and asks it for its ammo rather than picking a number: an ammo type nothing
 * in the mission fires is a crate that does nothing.
 */
static void modRandomRollCrates(struct modrandomlists *lists)
{
	s32 i;

	for (i = 0; i < lists->numcrates; i++) {
		struct ammocrateobj *crate = (struct ammocrateobj *)lists->crates[i];
		struct modrandomstream rng;
		s32 tries;

		modRandomOpen(&rng, MODRANDOM_STREAM_CRATE, i);

		for (tries = 0; tries < 8; tries++) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, NUM_MPWEAPONS - 1)];

			if (mpweapon->priammotype > 0) {
				crate->ammotype = mpweapon->priammotype;
				break;
			}
		}
	}
}

/**
 * Move the guards.
 *
 * A permutation of the pads the stage's own guards stand on, so every guard
 * is somewhere a guard was meant to be. bodyAllocateChr() collision-tests the
 * pad before it spawns anyone, so a pad that has since been filled by
 * something else costs that guard rather than crashing.
 *
 * Their patrol lists come with them and will walk them somewhere else
 * entirely, which is fine - a guard patrolling a route that starts across the
 * level is a guard on the move, and a randomizer wants them on the move.
 */
static void modRandomRollChrs(struct modrandomlists *lists)
{
	struct modrandomstream rng;
	u16 *pads;
	s32 i;

	if (lists->numchrs < 2) {
		return;
	}

	pads = modRandomAlloc(lists->numchrs, sizeof(u16));

	if (pads == NULL) {
		return;
	}

	for (i = 0; i < lists->numchrs; i++) {
		pads[i] = lists->chrs[i]->padnum;
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_CHRPADS, 0);

	for (i = lists->numchrs - 1; i > 0; i--) {
		s32 j = modRandomBelow(&rng, i + 1);
		u16 tmp = pads[i];

		pads[i] = pads[j];
		pads[j] = tmp;
	}

	for (i = 0; i < lists->numchrs; i++) {
		lists->chrs[i]->padnum = pads[i];
	}
}

/**
 * The room graph, and how far a set of keys gets you into it.
 *
 * portalkeys[p] is what portal p wants before it can be walked through: 0 for
 * a doorway or an unlocked door, otherwise the key flags of the door standing
 * in it. Everything else about a door - that it is closed, that it opens when
 * shot, that a script opens it - is treated as open, because it is.
 */
static u32 *modRandomBuildPortalKeys(struct modrandomlists *lists, s32 *outcount)
{
	u32 *portalkeys;
	s32 numportals = 0;
	s32 i;
	s32 j;

	// The portal numbers this stage actually uses, as its rooms list them.
	for (i = 0; i < g_Vars.roomcount; i++) {
		for (j = 0; j < g_Rooms[i].numportals; j++) {
			s32 portalnum = g_RoomPortals[g_Rooms[i].roomportallistoffset + j];

			if (portalnum >= numportals) {
				numportals = portalnum + 1;
			}
		}
	}

	*outcount = numportals;

	if (numportals <= 0) {
		return NULL;
	}

	portalkeys = modRandomAlloc(numportals, sizeof(u32));

	if (portalkeys == NULL) {
		return NULL;
	}

	for (i = 0; i < numportals; i++) {
		portalkeys[i] = 0;
	}

	for (i = 0; i < lists->numdoors; i++) {
		struct doorobj *door = lists->doors[i];

		if (door->keyflags && modRandomObjIsFreeStanding(&door->base)) {
			s32 portalnum = setupGetPortalByDoorPad(door->base.pad);

			if (portalnum >= 0 && portalnum < numportals) {
				portalkeys[portalnum] |= door->keyflags;
			}
		}
	}

	return portalkeys;
}

/**
 * Walk the graph from the spawn room, taking every key that is standing in a
 * room the walk has already reached, until a pass adds nothing.
 *
 * Keys that are in a guard's hands or inside something are counted as held
 * from the start: where they are is not a room this can read, and a walk that
 * called them unobtainable would reject every placement on a stage that has
 * one. That is the permissive direction, so the check catches the failure it
 * exists for - the key shut behind the door it opens - and not every one.
 */
static void modRandomWalk(struct modrandomlists *lists, s32 spawnroom, u8 *reached)
{
	u32 *portalkeys = lists->portalkeys;
	s32 numportals = lists->numportals;
	u32 held = 0;
	s32 i;
	s32 j;
	bool grew;

	for (i = 0; i < g_Vars.roomcount; i++) {
		reached[i] = 0;
	}

	if (spawnroom < 0 || spawnroom >= g_Vars.roomcount || portalkeys == NULL) {
		return;
	}

	for (i = 0; i < lists->numkeys; i++) {
		struct keyobj *key = lists->keys[i];

		if (!modRandomObjIsFreeStanding(&key->base)) {
			held |= key->keyflags;
		}
	}

	// reached[r] is 0 for a room the walk never got to, and otherwise one more
	// than the number of rooms crossed to get there - so it is both the flag
	// and the distance, which is what puts an objective at the far end of a
	// level rather than in the room next door.
	reached[spawnroom] = 1;

	do {
		grew = false;

		for (i = 0; i < g_Vars.roomcount; i++) {
			if (!reached[i]) {
				continue;
			}

			for (j = 0; j < g_Rooms[i].numportals; j++) {
				s32 portalnum = g_RoomPortals[g_Rooms[i].roomportallistoffset + j];
				s32 neighbour;
				u32 wants;

				if (portalnum < 0 || portalnum >= numportals) {
					continue;
				}

				wants = portalkeys[portalnum];

				if (wants && (held & wants) != wants) {
					continue;
				}

				neighbour = g_BgPortals[portalnum].roomnum1 == i
					? g_BgPortals[portalnum].roomnum2
					: g_BgPortals[portalnum].roomnum1;

				if (neighbour > 0 && neighbour < g_Vars.roomcount && !reached[neighbour]) {
					reached[neighbour] = reached[i] < 254 ? reached[i] + 1 : 254;
					grew = true;
				}
			}
		}

		// Anything picked up in what the walk has reached opens more of it.
		for (i = 0; i < lists->numkeys; i++) {
			struct keyobj *key = lists->keys[i];
			s32 room;

			if (!modRandomObjIsFreeStanding(&key->base) || (held & key->keyflags) == key->keyflags) {
				continue;
			}

			room = modRandomPadRoom(key->base.pad);

			if (room > 0 && room < g_Vars.roomcount && reached[room]) {
				held |= key->keyflags;
				grew = true;
			}
		}
	} while (grew);
}

/**
 * How much of the stage a walk reached.
 */
static s32 modRandomCountReached(u8 *reached)
{
	s32 count = 0;
	s32 i;

	for (i = 0; i < g_Vars.roomcount; i++) {
		if (reached[i]) {
			count++;
		}
	}

	return count;
}

/**
 * Move the keys, and keep the first placement the walk says is playable.
 *
 * The keys move and the locks stay: permuting both would deal a different
 * mission that is the same shape, and it is a key being somewhere new that
 * makes a stage feel re-cut. Free-standing keys only, and only between spots
 * that already held one.
 *
 * A placement is kept when the walk reaches at least as much of the stage as
 * the stage's own does. Measuring against the stage rather than against "all
 * rooms" is deliberate: plenty of rooms are behind a lift or a script the
 * walk cannot model, and a stage whose own layout the walk scores at forty
 * rooms is telling us that forty is what a playable one looks like here.
 */
static void modRandomRollKeys(struct modrandomlists *lists, s32 spawnroom, u8 *reached)
{
	s16 *pads;
	s16 *best;
	s32 numfree = 0;
	s32 stockreach;
	s32 bestreach = -1;
	s32 tries;
	s32 slot;
	s32 i;

	for (i = 0; i < lists->numkeys; i++) {
		if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
			numfree++;
		}
	}

	// What the stage's own key placement is worth, from this start.
	modRandomWalk(lists, spawnroom, reached);
	stockreach = modRandomCountReached(reached);

	if (numfree < 2) {
		return;
	}

	pads = modRandomAlloc(numfree, sizeof(s16));
	best = modRandomAlloc(numfree, sizeof(s16));

	if (pads == NULL || best == NULL) {
		return;
	}

	for (i = 0, numfree = 0; i < lists->numkeys; i++) {
		if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
			pads[numfree] = lists->keys[i]->base.pad;
			best[numfree] = pads[numfree];
			numfree++;
		}
	}

	for (tries = 0; tries < MODRANDOM_MAXTRIES; tries++) {
		struct modrandomstream rng;
		s32 reach;

		modRandomOpen(&rng, MODRANDOM_STREAM_KEYS, tries);

		for (i = numfree - 1; i > 0; i--) {
			s32 j = modRandomBelow(&rng, i + 1);
			s16 tmp = pads[i];

			pads[i] = pads[j];
			pads[j] = tmp;
		}

		for (i = 0, slot = 0; i < lists->numkeys; i++) {
			if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
				lists->keys[i]->base.pad = pads[slot++];
			}
		}

		modRandomWalk(lists, spawnroom, reached);
		reach = modRandomCountReached(reached);

		if (reach > bestreach) {
			bestreach = reach;

			for (i = 0; i < numfree; i++) {
				best[i] = pads[i];
			}
		}

		if (reach >= stockreach) {
			return;
		}
	}

	// Nothing this seed tried opened as much of the stage as the mission's own
	// keys do, so the best of them goes back in and is walked once more -
	// everything after this reads that walk.
	for (i = 0, slot = 0; i < lists->numkeys; i++) {
		if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
			lists->keys[i]->base.pad = best[slot++];
		}
	}

	modRandomWalk(lists, spawnroom, reached);
}

/**
 * How long an intro command is, in bytes.
 *
 * The intro stream is not the setup stream and has no setupGetCmdLength() of
 * its own: the lengths live inside the switch statements that read it, in
 * playerReset() and playerStartNewLife(), where they are 12 for some commands
 * and 40 for one. Stepping by a flat 12 walks into the middle of a command and
 * reads a parameter as the next type, which is a rewrite of whatever happens
 * to be there. 0 means "not one this knows", and every walk here stops on it.
 */
static s32 modRandomIntroCmdLen(s32 type)
{
	switch (type) {
	case INTROCMD_SPAWN:        return 12;
	case INTROCMD_WEAPON:       return 16;
	case INTROCMD_AMMO:         return 16;
	case INTROCMD_3:            return 32;
	case INTROCMD_4:            return 8;
	case INTROCMD_OUTFIT:       return 8;
	case INTROCMD_6:            return 40;
	case INTROCMD_WATCHTIME:    return 12;
	case INTROCMD_CREDITOFFSET: return 8;
	case INTROCMD_CASE:         return 12;
	case INTROCMD_CASERESPAWN:  return 12;
	case INTROCMD_HILL:         return 8;
	}

	return 0;
}

static s32 modRandomWriteSpawn(s32 padnum)
{
	struct modrandomintrocmd *cmd = (struct modrandomintrocmd *)g_StageSetup.intro;
	s32 first = -1;

	while (cmd && cmd->type != INTROCMD_END) {
		s32 len = modRandomIntroCmdLen(cmd->type);

		if (len == 0) {
			break;
		}

		if (cmd->type == INTROCMD_SPAWN && cmd->param2 == 0) {
			if (first < 0) {
				first = cmd->param1;
			}

			if (padnum >= 0) {
				cmd->param1 = padnum;
			}
		}

		cmd = (struct modrandomintrocmd *)((uintptr_t)cmd + len);
	}

	return first;
}

/**
 * Where the mission starts.
 *
 * A solo mission's intro lists one spawn pad - the several a stage has are
 * co-operative's, and they are gated off - so picking between them is picking
 * between one thing, which is how the first version of this managed to roll
 * the same start on every seed. The pads worth starting on are the ones the
 * stage stands a *guard* on: a spot in a room, on the floor, that something
 * man-sized was meant to occupy, and there are dozens of them spread over the
 * level rather than one at the front door.
 *
 * A start is only taken if the walk from it reaches most of what the mission's
 * own start reaches. Otherwise it is a lift interior, a sealed vault or the
 * wrong side of a locked door - all of them somewhere a guard legitimately
 * stands and none of them a mission. The stage's own pad is the fallback and
 * is always tried first, so the worst case is the mission starting where it
 * always did.
 */
static s32 modRandomRollSpawn(struct modrandomlists *lists, u8 *reached)
{
	s32 stockpad = modRandomWriteSpawn(-1);
	s32 stockreach;
	s32 best = stockpad;
	s32 bestreach;
	s32 tries;

	modRandomWalk(lists, modRandomPadRoom(stockpad), reached);
	stockreach = modRandomCountReached(reached);
	bestreach = stockreach;

	for (tries = 0; tries < MODRANDOM_MAXTRIES && lists->numchrs > 0; tries++) {
		struct modrandomstream rng;
		s32 padnum;

		modRandomOpen(&rng, MODRANDOM_STREAM_SPAWN, tries);
		padnum = lists->chrs[modRandomBelow(&rng, lists->numchrs)]->padnum;
		s32 room = modRandomPadRoom(padnum);
		s32 reach;

		if (room <= 0 || room >= g_Vars.roomcount) {
			continue;
		}

		// And somewhere a player can be stood up alive. A guard's pad is a
		// place for a guard, which is not always a place for anybody: a ledge
		// with no floor under it starts the mission four billion units below
		// the level. See modRandomPadSpawnPos().
		if (g_ModRandomVersion >= 3 && !modRandomPadCanSpawn(padnum)) {
			continue;
		}

		modRandomWalk(lists, room, reached);
		reach = modRandomCountReached(reached);

		// Most of the mission, not all of it: a start deeper in the level is
		// past doors it will not have to come back through, and a start that
		// had to reach everything would only ever be the front door again.
		if (reach * 4 >= stockreach * 3) {
			best = padnum;
			bestreach = reach;
			break;
		}
	}

	modRandomWriteSpawn(best);
	modRandomWalk(lists, modRandomPadRoom(best), reached);

	return best;
}

/**
 * What the player starts holding.
 *
 * The intro's weapon commands, rolled from the same table the floor pickups
 * come from. A mission that starts you unarmed still does - the command is
 * rewritten, not added - so a stage that means you to walk in with nothing
 * keeps that, and one that arms you arms you with something else.
 */
static void modRandomRollIntroWeapons(void)
{
	struct modrandomintrocmd *cmd = (struct modrandomintrocmd *)g_StageSetup.intro;
	s32 index = 0;

	while (cmd && cmd->type != INTROCMD_END) {
		s32 len = modRandomIntroCmdLen(cmd->type);

		if (len == 0) {
			break;
		}

		if (cmd->type == INTROCMD_WEAPON) {
			struct modrandomstream rng;
			struct mpweapon *mpweapon;

			modRandomOpen(&rng, MODRANDOM_STREAM_INTROGUN, index++);
			mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, NUM_MPWEAPONS - 1)];

			if (mpweapon->weaponnum != WEAPON_NONE) {
				cmd->param1 = mpweapon->weaponnum;

				// The second hand is a weapon number too, and -1 for a mission
				// that arms one hand: rolling that into a gun would hand out a
				// pair where the mission meant one.
				if (cmd->param2 >= 0) {
					cmd->param2 = mpweapon->weaponnum;
				}
			}
		}

		cmd = (struct modrandomintrocmd *)((uintptr_t)cmd + len);
	}
}

/**
 * Write one generated objective into the command buffer.
 *
 * An objective is a run of commands - BEGINOBJECTIVE, its requirements, then
 * ENDOBJECTIVE - and objectiveCheck() walks that run from the pointer it was
 * given, so a generated one only has to look like the file's. The type is the
 * top byte of the first word wherever a command keeps it, which is what
 * PD_BE32() of a bare type value writes on either endianness.
 */
static struct objective *modRandomObjectiveAt(s32 slot);

static u32 *modRandomWriteObjective(u32 *cmd, s32 index, u32 type, u32 param)
{
	struct objective *objective = (struct objective *)cmd;

	objective->unk00 = PD_BE32(OBJTYPE_BEGINOBJECTIVE);
	objective->index = index;
	objective->text = L_MISC_042; // never read: modRandomGetObjectiveText() answers first
	objective->unk0c = 0;
	objective->flags = 0;
	objective->difficulties = DIFFBIT_A | DIFFBIT_SA | DIFFBIT_PA | DIFFBIT_PD;

	cmd += setupGetCmdLength(cmd);

	if (type == OBJECTIVETYPE_ENTERROOM) {
		struct criteria_roomentered *criteria = (struct criteria_roomentered *)cmd;

		// The field is named pad and is read by chrGetPadRoom(), which takes a
		// value under 10000 as a room number already. A room is what this
		// knows, so a room is what goes in.
		criteria->unk00 = PD_BE32(OBJECTIVETYPE_ENTERROOM);
		criteria->pad = param;
		criteria->status = OBJECTIVE_INCOMPLETE;
		criteria->next = NULL;

		objectiveAddRoomEnteredCriteria(criteria);
	} else {
		cmd[0] = PD_BE32(type);
		cmd[1] = param;
	}

	cmd += setupGetCmdLength(cmd);

	cmd[0] = PD_BE32(OBJTYPE_ENDOBJECTIVE);

	// Nothing to hand back: an objective lives in the block its slot names,
	// which is also what keeps a criteria struct's trailing pointer aligned -
	// three commands do not come to a multiple of eight, and packing them end
	// to end leaves every second one misaligned, which x86 forgives and the
	// arm64 build does not.
	return cmd;
}

/**
 * Deal the mission's objectives from what the roll actually placed.
 *
 * Every objective names something the generator has just seen standing in a
 * room the walk reached: a weapon it put down, a thing the stage already had
 * a tag on, a room a pad leads to. That is the whole discipline here - an
 * objective that names a tag the stage does not have, or a room the player
 * cannot get to, is a mission that cannot be finished, and the player has no
 * way to tell which of the two it is.
 *
 * Three at most, because the briefing draws them and six is where it stops,
 * and because a generated objective is a sentence with no story behind it -
 * three reads as a mission, seven reads as a chore list.
 */
static struct objective *modRandomObjectiveAt(s32 slot)
{
	if (g_ModRandomObjCmds == NULL || slot < 0 || slot >= MODRANDOM_MAXOBJECTIVES) {
		return NULL;
	}

	return (struct objective *)((uintptr_t)g_ModRandomObjCmds + slot * MODRANDOM_OBJBLOCK);
}

static bool modRandomDealObjective(struct modrandomlists *lists, u8 *reached, s32 slot, s32 ordinal)
{
	u32 *cmd = (u32 *)modRandomObjectiveAt(slot);

	if (cmd == NULL) {
		return false;
	}

	// A thing to fetch while there are things left to fetch, and somewhere to
	// be after that. Fetching is the better objective - it names something the
	// player can see and carry - but each one can only be asked for once: the
	// item stays in the inventory, so asking again would complete the moment
	// it was dealt.
	if (ordinal < g_ModRandomPoolSize) {
		struct weaponobj *weapon = g_ModRandomPool[ordinal];
		struct weapon *weapondef = weaponFindById(weapon->weaponnum);
		char *name = weapondef ? langGet(weapondef->name) : NULL;

		modRandomWriteObjective(cmd, slot, OBJECTIVETYPE_COLLECTOBJ,
				modRandomTagNumOf(lists, &weapon->base));

		if (name) {
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN, "Recover the %s", name);
		} else {
			strcpy(g_ModRandomObjText[slot], "Recover the stolen hardware");
		}

		return true;
	}

	// Somewhere to be: a room out at the far end of what the walk reached.
	// reached[] holds how many rooms were crossed to get there, so the deep
	// half of the level is everything at more than half the greatest depth -
	// which is a walk across the level rather than into the next room, and
	// still leaves enough rooms to choose between that an endless run does not
	// send the player to the same corner twice running.
	{
		struct modrandomstream rng;
		s32 deepest = 0;
		s32 chosen = -1;
		s32 count = 0;
		s32 i;

		for (i = 1; i < g_Vars.roomcount; i++) {
			if (reached[i] > deepest) {
				deepest = reached[i];
			}
		}

		if (deepest <= 1) {
			return false;
		}

		modRandomOpen(&rng, MODRANDOM_STREAM_OBJROOM, ordinal);

		for (i = 1; i < g_Vars.roomcount; i++) {
			// v1 took only the deepest rooms, which is the far end of the
			// level and about four rooms to choose from; from v2 the deep
			// half of it, so that an endless run asking again and again has
			// somewhere else to send the player each time. A change to what
			// the draw means, so the older runs keep the older rule.
			bool wanted = g_ModRandomVersion >= 2
				? (reached[i] * 2 >= deepest && i != g_ModRandomLastRoom)
				: (reached[i] == deepest);

			if (wanted) {
				count++;

				if (modRandomBelow(&rng, count) == 0) {
					chosen = i;
				}
			}
		}

		if (chosen < 0) {
			return false;
		}

		g_ModRandomLastRoom = chosen;

		modRandomWriteObjective(cmd, slot, OBJECTIVETYPE_ENTERROOM, chosen);
		strcpy(g_ModRandomObjText[slot], "Reach the extraction point");

		return true;
	}
}

/**
 * Deal the mission's objectives from what the roll actually placed.
 *
 * Every objective names something the generator has just seen standing in a
 * room the walk reached: a weapon it put down that the stage already had a tag
 * on, or a room a walk can get to. That is the whole discipline here - an
 * objective that names a tag the stage does not have, or a room the player
 * cannot reach, is a mission that cannot be finished, and the player has no
 * way to tell which of the two it is.
 *
 * Three at most, because the briefing draws them and six is where it stops,
 * and because a generated objective is a sentence with no story behind it -
 * three reads as a mission, seven reads as a chore list. Endless Mode deals
 * one at a time instead, and keeps dealing.
 */
static void modRandomGenerateObjectives(struct modrandomlists *lists, u8 *reached)
{
	struct modrandomstream rng;
	s32 numwanted;
	s32 i;

	g_ModRandomNumObjectives = 0;
	g_ModRandomPoolSize = 0;
	g_ModRandomLastRoom = -1;

	// The weapons the roll left in reachable rooms, which are the things it
	// can ask for by name. Shuffled once and taken in order, so an endless run
	// works through them without asking for the same one twice.
	g_ModRandomPool = modRandomAlloc(lists->numweapons + 1, sizeof(void *));

	if (g_ModRandomPool == NULL) {
		return;
	}

	for (i = 0; i < lists->numweapons; i++) {
		struct weaponobj *weapon = lists->weapons[i];
		s32 room;

		if (!modRandomObjIsFreeStanding(&weapon->base)) {
			continue;
		}

		if (modRandomTagNumOf(lists, &weapon->base) < 0) {
			continue;
		}

		room = modRandomPadRoom(weapon->base.pad);

		if (room > 0 && room < g_Vars.roomcount && reached[room]) {
			g_ModRandomPool[g_ModRandomPoolSize++] = weapon;
		}
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_OBJPOOL, 0);
	modRandomShuffle(&rng, (void **)g_ModRandomPool, g_ModRandomPoolSize);

	if (modRandomIsEndless()) {
		// One at a time: the run is the next objective, not a list of them.
		numwanted = 1;
	} else {
		modRandomOpen(&rng, MODRANDOM_STREAM_OBJCOUNT, 0);
		numwanted = 1 + modRandomBelow(&rng, MODRANDOM_MAXOBJECTIVES);
	}

	g_ModRandomObjCmds = modRandomAlloc(MODRANDOM_MAXOBJECTIVES, MODRANDOM_OBJBLOCK);

	if (g_ModRandomObjCmds == NULL) {
		return;
	}

	for (i = 0; i < numwanted; i++) {
		if (!modRandomDealObjective(lists, reached, i, g_ModRandomDealt)) {
			break;
		}

		g_ModRandomDealt++;
		g_ModRandomNumObjectives++;
	}
}

/**
 * The text a generated objective shows, or NULL when the objective at that
 * index is the stage's own.
 */
char *modRandomGetObjectiveText(s32 index)
{
	// A run's room asks for its own thing, and everything that draws an
	// objective's text comes through here.
	char *runtext = modRunGetObjectiveText(index);

	if (runtext) {
		return runtext;
	}

	if (modRandomIsOn()
			&& index >= 0
			&& index < g_ModRandomNumObjectives) {
		return g_ModRandomObjText[index];
	}

	return NULL;
}

/**
 * A number naming what the roll produced, for checking that a change to this
 * file left old seeds alone.
 *
 * The claim the streams make - that changing one decision moves nothing else -
 * is only worth as much as it can be checked, and reading a mission to see
 * whether it is the same mission is not checking. So each part of the roll
 * gets its own fold of what it decided, printed with the seed: change the
 * weapon roll and the weapon number moves while the others stand still. If one
 * of the others moves too, the change leaked, and every seed written down for
 * this build has quietly become a different mission.
 */
static u32 modRandomFold(u32 hash, u32 value)
{
	return modRandomMix(hash * 0x9e3779b9u + value);
}

/**
 * Deal the mission.
 *
 * Called from setupCreateProps(), after the pads have been prepared - the
 * walk needs their rooms - and before the stream is built from, which is what
 * makes every rewrite here free.
 */
void modRandomRoll(s32 stagenum)
{
	struct modrandomlists lists;
	u8 *reached;
	s32 spawnpad;
	s32 spawnroom;

	g_ModRandomNumObjectives = 0;
	g_ModRandomObjCmds = NULL;
	g_ModRandomSpawnPad = -1;
	g_ModRandomSpawnState = 0;
	g_ModRandomVisited = NULL;
	g_ModRandomReached = NULL;
	g_ModRandomRooms = 0;
	g_ModRandomCleared = 0;
	g_ModRandomDealt = 0;
	g_ModRandomRunOver = false;

	// modRandomIsOn() asks about g_Vars.stagenum, which is this stage by the
	// time the roll runs, but the stage being loaded is the one that matters.
	if (!modRandomIsOn() || !modRandomStageIsMission(stagenum)) {
		return;
	}

	if (g_StageSetup.props == NULL || g_StageSetup.padfiledata == NULL || g_PadsFile == NULL) {
		return;
	}

	if (g_Vars.roomcount <= 0 || g_Rooms == NULL || g_BgPortals == NULL || g_RoomPortals == NULL) {
		return;
	}

	// A run's hop is dealt from the run's seed rather than the menu's, so that
	// a run written down deals the same rooms with the same guns in them.
	g_ModRandomSeed = modRunIsOn()
		? modRunGetSeed()
		: (g_ModOptions.randomseed ? (u32)g_ModOptions.randomseed : rngRandom());

	// A kept seed keeps the generator it was dealt by; a fresh mission every
	// time has nothing to keep and takes the newest. Otherwise a config
	// written by an older build would pin every future run to that build's
	// generator, which is the opposite of what the setting is for.
	if (g_ModOptions.randomseed) {
		g_ModRandomVersion = g_ModOptions.randomversion;
	} else {
		g_ModRandomVersion = MODRANDOM_VERSION;
		g_ModOptions.randomversion = MODRANDOM_VERSION;
	}

	// A run dealt by a generator this build does not have is dealt by the
	// newest one it does, and says so: a mission that quietly differs from the
	// one the seed was written down for is worse than being told it will.
	if (g_ModRandomVersion < 1 || g_ModRandomVersion > MODRANDOM_VERSION) {
#ifndef PLATFORM_N64
		if (g_ModRandomVersion > MODRANDOM_VERSION) {
			sysLogPrintf(1, "randomizer: seed %u wants generator v%d and this build has v%d; "
					"dealing it with v%d", g_ModRandomSeed, g_ModRandomVersion,
					MODRANDOM_VERSION, MODRANDOM_VERSION);
		}
#endif
		g_ModRandomVersion = MODRANDOM_VERSION;
	}

	// The run's number: the seed, the stage so that one run's Villa and its
	// Chicago are different missions, and the difficulty so that Perfect Agent
	// is its own deal rather than the Agent one with more guards. Every stream
	// hangs off this and nothing else.
	g_ModRandomRun = modRandomMix(g_ModRandomSeed)
		+ modRandomMix(stagenum * 0x9e3779b9u + lvGetDifficulty());

	// And the hop's number on top of it during a run, so that a run landing on
	// the same map twice is not the same map twice.
	if (modRunIsOn()) {
		g_ModRandomRun += modRandomMix(modRunGetRooms() * 0x85ebca6bu);
	}

	modRandomGather(&lists);

	if (lists.numweapons == 0 && lists.numchrs == 0) {
		return;
	}

	reached = modRandomAlloc(g_Vars.roomcount, sizeof(u8));

	if (reached == NULL) {
		return;
	}

	// The locked edges are a property of the stage's doors, not of the roll,
	// so the graph is built once and every walk below borrows it.
	lists.portalkeys = modRandomBuildPortalKeys(&lists, &lists.numportals);

	if (lists.portalkeys == NULL) {
		// A stage with no portals, or the pool is out. There is then nothing
		// to say a room cannot be got to, and the walk exists only to say
		// that, so everything counts as reachable and the roll goes on.
		s32 i;

		for (i = 0; i < g_Vars.roomcount; i++) {
			reached[i] = 1;
		}
	}

	// What is standing about, and what is in the player's hands, first:
	// neither depends on where anything is.
	modRandomRollWeapons(&lists);
	modRandomRollCrates(&lists);
	modRandomRollIntroWeapons();

	// Then where the guards are, because the start is picked from their pads;
	// then the start, because the walk is measured from it; then the keys,
	// because what they open decides what the walk reaches; and only then the
	// objectives, which may name nothing the walk did not reach.
	modRandomRollChrs(&lists);

	if (lists.portalkeys) {
		spawnpad = modRandomRollSpawn(&lists, reached);
		spawnroom = modRandomPadRoom(spawnpad);

		modRandomRollKeys(&lists, spawnroom, reached);
	} else {
		spawnpad = modRandomWriteSpawn(-1);
		spawnroom = modRandomPadRoom(spawnpad);
	}

	// A run's objectives are its room's and its start is its landing, both
	// dealt by modrun.c: the roll above still moves the guns, the crates, the
	// guards and the keys, which is everything a landing wants from it.
	if (!modRunIsOn()) {
		modRandomGenerateObjectives(&lists, reached);
	}

	// Nothing to move to if the roll kept the mission's own start.
	g_ModRandomSpawnPad = spawnpad;
	g_ModRandomSpawnState = (spawnpad >= 0 && !modRunIsOn()) ? 1 : 0;

	// What an endless run needs for the rest of the level: the lists, the
	// walk, and a room per bit of score.
	g_ModRandomLists = lists;
	g_ModRandomReached = reached;
	g_ModRandomVisited = modRandomAlloc(g_Vars.roomcount, sizeof(u8));

	if (g_ModRandomVisited) {
		s32 i;

		for (i = 0; i < g_Vars.roomcount; i++) {
			g_ModRandomVisited[i] = 0;
		}
	}

#ifndef PLATFORM_N64
	{
		u32 wf = 0;
		u32 cf = 0;
		u32 kf = 0;
		u32 of = 0;
		s32 i;

		for (i = 0; i < lists.numweapons; i++) {
			wf = modRandomFold(wf, lists.weapons[i]->weaponnum);
		}

		for (i = 0; i < lists.numchrs; i++) {
			cf = modRandomFold(cf, lists.chrs[i]->padnum);
		}

		for (i = 0; i < lists.numkeys; i++) {
			kf = modRandomFold(kf, lists.keys[i]->base.pad);
		}

		{
			// What each objective asks for: the requirement's type and the tag
			// or room it names, which is the objective. Folding the objective
			// struct instead would fold the same four words every time.
			for (i = 0; i < g_ModRandomNumObjectives; i++) {
				u32 *cmd = (u32 *)modRandomObjectiveAt(i);

				if (cmd == NULL) {
					break;
				}

				cmd += setupGetCmdLength(cmd); // past the begin

				while ((u8)PD_BE32(cmd[0]) != OBJTYPE_ENDOBJECTIVE) {
					of = modRandomFold(of, PD_BE32(cmd[0]));
					of = modRandomFold(of, cmd[1]);
					cmd += setupGetCmdLength(cmd);
				}
			}
		}

		sysLogPrintf(0, "randomizer: fold weapons %08x guards %08x keys %08x spawn %08x objectives %08x",
				wf, cf, kf, (u32)spawnpad, of);
	}

	{
		s32 i;

		for (i = 0; i < g_ModRandomNumObjectives; i++) {
			sysLogPrintf(0, "randomizer: objective %d - %s", i, g_ModRandomObjText[i]);
		}
	}

	sysLogPrintf(0, "randomizer: stage 0x%02x seed %u v%d - %d weapons, %d guards, %d keys, "
			"spawn pad %d room %d, %d/%d rooms reachable, %d objectives",
			stagenum, g_ModRandomSeed, g_ModRandomVersion,
			lists.numweapons, lists.numchrs, lists.numkeys,
			spawnpad, spawnroom, modRandomCountReached(reached), g_Vars.roomcount,
			g_ModRandomNumObjectives);
#endif
}

/**
 * Put the player where the roll wants them, once the mission is actually
 * being played.
 *
 * Rewriting the intro's spawn pad is not enough on a mission that opens with
 * a cutscene, which is most of them: the pad is where a *new life* starts,
 * and the opening puts the player where its script says regardless. So the
 * start is moved the way a new life moves - the machinery co-operative and
 * Mission Respawn already use - on the first frame after the cutscene hands
 * control over.
 *
 * Once, and only for the mission's own player. Every death after this one is
 * the game's business.
 */
static void modRandomTickEndless(void);

void modRandomTick(void)
{
	modRandomTickEndless();

	if (g_ModRandomSpawnState != 1 || !modRandomIsOn()) {
		return;
	}

	if (g_InCutscene || g_Vars.lvframenum < 2 || g_Vars.currentplayer != g_Vars.bond) {
		return;
	}

	if (g_Vars.currentplayer->isdead || g_Vars.currentplayer->dostartnewlife) {
		return;
	}

	g_ModRandomSpawnState = 2;
	g_Vars.currentplayer->dostartnewlife = true;
}

/**
 * The run: count the ground covered, deal the next objective when the live one
 * is finished, and end at the first death.
 *
 * Everything the deal needs was kept from the level's roll - the lists point
 * into the setup stream and the walk into the stage pool, both of which live
 * as long as the level does - so dealing another objective mid-mission is the
 * same work the roll did, minus the rewriting.
 */
static void modRandomTickEndless(void)
{
	struct player *player = g_Vars.currentplayer;
	s32 room;

	if (!modRandomIsEndless() || g_ModRandomRunOver || g_ModRandomVisited == NULL) {
		return;
	}

	if (player != g_Vars.bond || g_InCutscene) {
		return;
	}

	if (player->isdead) {
		static char text[64];

		g_ModRandomRunOver = true;

		if (g_ModRandomRooms > g_ModOptions.endlessbest) {
			g_ModOptions.endlessbest = g_ModRandomRooms;
			sprintf(text, "%d rooms, %d objectives - best yet\n", g_ModRandomRooms, g_ModRandomCleared);
		} else {
			sprintf(text, "%d rooms, %d objectives (best %d)\n",
					g_ModRandomRooms, g_ModRandomCleared, g_ModOptions.endlessbest);
		}

		// Four seconds: the death fade is a second and a bit, and a score the
		// player cannot read is not a score.
		{
			extern struct hudmsgtype g_HudmsgTypes[];
			hudmsgCreateWithDuration(text, HUDMSGTYPE_DEFAULT, &g_HudmsgTypes[HUDMSGTYPE_DEFAULT], TICKS(240));
		}

		return;
	}

	// Ground covered. rooms[0] is where the player is standing; a room already
	// stood in is not covered again, so pacing back and forth scores nothing.
	room = player->prop->rooms[0];

	if (room > 0 && room < g_Vars.roomcount && !g_ModRandomVisited[room]) {
		g_ModRandomVisited[room] = 1;
		g_ModRandomRooms++;
	}

	// The live objective, and the next one. Dealt in the same tick it is
	// finished, so objectiveIsAllComplete() is never true for a whole frame -
	// which is what the stage's own exit trigger asks, and an endless run has
	// no business ending at the exit.
	if (g_ModRandomNumObjectives > 0 && objectiveCheck(0) == OBJECTIVE_COMPLETE) {
		static char text[96];

		g_ModRandomCleared++;

		if (modRandomDealObjective(&g_ModRandomLists, g_ModRandomReached, 0, g_ModRandomDealt)) {
			g_ModRandomDealt++;
			g_ObjectiveStatuses[0] = OBJECTIVE_INCOMPLETE;

			// The score rides along with the new objective: a run's number is
			// no use to the player on the death screen, where the fade and
			// the failure dialog are what they are looking at.
			sprintf(text, "%s - %d rooms\n", g_ModRandomObjText[0], g_ModRandomRooms);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		} else {
			// Nothing left to ask for on this stage. The run stands on what it
			// scored rather than looping an objective the player has done -
			// and says so, because a run that quietly stops dealing looks
			// like a bug from the inside.
			sprintf(text, "Nothing left to find - %d rooms, %d objectives\n",
					g_ModRandomRooms, g_ModRandomCleared);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);

			if (g_ModRandomRooms > g_ModOptions.endlessbest) {
				g_ModOptions.endlessbest = g_ModRandomRooms;
			}

			g_ModRandomRunOver = true;
		}
	}
}

/**
 * The spot playerStartNewLife() should use, if this is that first life.
 *
 * Answers once: the same call is what marks the move done, so a later death
 * spawns where the game would have spawned it.
 */
bool modRandomTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle)
{
	struct pad pad;

	if (g_ModRandomSpawnState != 2 || !modRandomIsOn() || g_ModRandomSpawnPad < 0) {
		return false;
	}

	g_ModRandomSpawnState = 0;

	padUnpack(g_ModRandomSpawnPad, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_LOOK, &pad);

	// The floor's own y, not the pad's: the game's ground query takes the
	// highest floor strictly below the position it is handed, and a pad level
	// with its own floor misses it. Every version gets this - it does not
	// change which pad the seed dealt, only where on it the player stands.
	if (!modRandomPadSpawnPos(g_ModRandomSpawnPad, pos, &rooms[0])) {
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
 * Replace the stage's objectives with the generated ones.
 *
 * Called at the end of setupCreateProps(), once the stream's own objectives
 * have been inserted: they are cleared here rather than skipped during the
 * walk so that everything else the walk does with them - the briefing text,
 * the tags they name - happens exactly as it always did, and only the list
 * the game checks is ours.
 */
void modRandomInsertObjectives(void)
{
	s32 i;

	if (!modRandomIsOn() || g_ModRandomNumObjectives == 0) {
		return;
	}

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		g_Objectives[i] = NULL;
		g_ObjectiveStatuses[i] = OBJECTIVE_INCOMPLETE;
	}

	for (i = 0; i < ARRAYCOUNT(g_Briefing.objectivenames); i++) {
		g_Briefing.objectivenames[i] = 0;
		g_Briefing.objectivedifficulties[i] = 0;
	}

	g_ObjectiveLastIndex = -1;

	// Re-insert from the buffer the roll wrote, which is where the objective
	// structs and their requirements live. One fixed block each, because
	// Endless Mode rewrites a block in place while the others stand: packing
	// them end to end would mean an objective's length deciding where the next
	// one lives, and a re-deal moving every objective after it.
	for (i = 0; i < g_ModRandomNumObjectives; i++) {
		struct objective *objective = modRandomObjectiveAt(i);

		if (objective == NULL) {
			break;
		}

		objectiveInsert(objective);

		g_Briefing.objectivenames[objective->index] = L_MISC_042;
		g_Briefing.objectivedifficulties[objective->index] = objective->difficulties;
	}
}

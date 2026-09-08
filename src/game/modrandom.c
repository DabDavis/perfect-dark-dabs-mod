#include <ultra64.h>
#include "constants.h"
#include "game/bg.h"
#include "game/chraction.h"
#include "game/game_0b0fd0.h"
#include "game/lang.h"
#include "game/atan2f.h"
#include "game/lv.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/objectives.h"
#include "game/pad.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "bss.h"
#include "lang.h"
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
 */

// The intro stream's command, as playerReset() reads it.
struct modrandomintrocmd {
	s32 type;
	s32 param1;
	s32 param2;
	s32 param3;
};

#define MODRANDOM_MAXOBJECTIVES 3  // generated per mission
#define MODRANDOM_MAXTRIES      16 // key placements tried before giving up on the roll
#define MODRANDOM_TEXTLEN       64

static u32 g_ModRandomSeed;    // the run's, as the player sees it
static u32 g_ModRandomState;   // the roll's own PRNG state

// The generated objectives: their command stream, which objectiveCheck()
// walks exactly as it walks the setup file's own, and their text, which the
// briefing and the HUD ask for by objective index.
// Where the roll wants the mission to start, and whether the player has been
// put there yet: 0 nothing to move, 1 waiting for the mission to begin,
// 2 asked for.
static s32 g_ModRandomSpawnPad = -1;
static s32 g_ModRandomSpawnState;

static u32 *g_ModRandomObjCmds;
static char g_ModRandomObjText[MODRANDOM_MAXOBJECTIVES][MODRANDOM_TEXTLEN];
static s32 g_ModRandomNumObjectives;

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

/**
 * Whether a mission is being dealt again.
 *
 * Solo only. The Combat Simulator deals its own arena from mpsetup and the
 * Institute is furniture around a menu; neither has objectives to generate or
 * a mission to make unplayable.
 */
bool modRandomIsOn(void)
{
	return g_ModOptions.randomizer != 0
		&& !g_Vars.normmplayerisrunning
		&& !g_Vars.mplayerisrunning;
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
static void modRandomSeed(u32 seed)
{
	seed ^= seed >> 16;
	seed *= 0x7feb352d;
	seed ^= seed >> 15;
	seed *= 0x846ca68b;
	seed ^= seed >> 16;

	g_ModRandomState = seed ? seed : 0x9e3779b9;
}

static u32 modRandomNext(void)
{
	g_ModRandomState ^= g_ModRandomState << 13;
	g_ModRandomState ^= g_ModRandomState >> 17;
	g_ModRandomState ^= g_ModRandomState << 5;

	return g_ModRandomState;
}

static u32 modRandomBelow(u32 limit)
{
	return limit ? modRandomNext() % limit : 0;
}

/**
 * Fisher-Yates over an array of pointers.
 */
static void modRandomShuffle(void **array, s32 count)
{
	s32 i;

	for (i = count - 1; i > 0; i--) {
		s32 j = modRandomBelow(i + 1);
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
		// A weapon in a guard's hands has to be something a guard can fire:
		// the table holds the specs, the cloaking device and the boost pill
		// as well, and a guard issued a pair of X-Ray specs is a guard that
		// stands there. Ammo is what tells the two apart.
		bool forchr = (weapon->base.flags & OBJFLAG_ASSIGNEDTOCHR) != 0;
		s32 tries;

		for (tries = 0; tries < 16; tries++) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(NUM_MPWEAPONS - 1)];

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
		s32 tries;

		for (tries = 0; tries < 8; tries++) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(NUM_MPWEAPONS - 1)];

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

	for (i = lists->numchrs - 1; i > 0; i--) {
		s32 j = modRandomBelow(i + 1);
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
		s32 reach;

		for (i = numfree - 1; i > 0; i--) {
			s32 j = modRandomBelow(i + 1);
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
		s32 padnum = lists->chrs[modRandomBelow(lists->numchrs)]->padnum;
		s32 room = modRandomPadRoom(padnum);
		s32 reach;

		if (room <= 0 || room >= g_Vars.roomcount) {
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

	while (cmd && cmd->type != INTROCMD_END) {
		s32 len = modRandomIntroCmdLen(cmd->type);

		if (len == 0) {
			break;
		}

		if (cmd->type == INTROCMD_WEAPON) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(NUM_MPWEAPONS - 1)];

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
	cmd += setupGetCmdLength(cmd);

	// The next objective starts on a 16 byte boundary. A criteria struct ends
	// in a pointer, and three commands do not come to a multiple of eight, so
	// packing them end to end leaves every second objective's pointer
	// misaligned - which x86 forgives and the arm64 build does not.
	return (u32 *)ALIGN16((uintptr_t)cmd);
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
static void modRandomGenerateObjectives(struct modrandomlists *lists, u8 *reached)
{
	struct weaponobj **pool;
	s32 numpool = 0;
	s32 numwanted;
	s32 count;
	s32 i;
	u32 *cmd;

	g_ModRandomNumObjectives = 0;

	// The weapons the roll left in reachable rooms, which are the things it
	// can ask for by name.
	pool = modRandomAlloc(lists->numweapons + 1, sizeof(void *));

	if (pool == NULL) {
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
			pool[numpool++] = weapon;
		}
	}

	modRandomShuffle((void **)pool, numpool);

	numwanted = 1 + modRandomBelow(MODRANDOM_MAXOBJECTIVES);

	// Enough room for the worst case: a begin, a requirement and an end each.
	g_ModRandomObjCmds = modRandomAlloc(numwanted,
			sizeof(struct objective) + sizeof(struct criteria_roomentered) + sizeof(u32) + 16);

	if (g_ModRandomObjCmds == NULL) {
		return;
	}

	cmd = g_ModRandomObjCmds;

	for (i = 0; i < numwanted; i++) {
		s32 index = g_ModRandomNumObjectives;

		if (i < numpool) {
			struct weapon *weapondef = weaponFindById(pool[i]->weaponnum);
			char *name = weapondef ? langGet(weapondef->name) : NULL;

			cmd = modRandomWriteObjective(cmd, index, OBJECTIVETYPE_COLLECTOBJ,
					modRandomTagNumOf(lists, &pool[i]->base));

			if (name) {
				snprintf(g_ModRandomObjText[index], MODRANDOM_TEXTLEN, "Recover the %s", name);
			} else {
				strcpy(g_ModRandomObjText[index], "Recover the stolen hardware");
			}

			g_ModRandomNumObjectives++;
		} else {
			// Nothing left to fetch, so somewhere to be instead: the far end
			// of the walk. reached[] holds how many rooms it crossed to get
			// there, so the deepest room it found is the one furthest from
			// the start through doors that open - which is a walk across the
			// level rather than into the next room.
			s32 chosen = -1;
			s32 deepest = 0;
			s32 j;

			for (j = 1; j < g_Vars.roomcount; j++) {
				if (reached[j] > deepest) {
					deepest = reached[j];
				}
			}

			for (j = 1, count = 0; j < g_Vars.roomcount; j++) {
				// Reservoir over everything at that depth, so two seeds that
				// agree about the far end of the level still send the player
				// to different corners of it.
				if (reached[j] == deepest) {
					count++;

					if (modRandomBelow(count) == 0) {
						chosen = j;
					}
				}
			}

			if (chosen < 0 || deepest <= 1) {
				break;
			}

			cmd = modRandomWriteObjective(cmd, index, OBJECTIVETYPE_ENTERROOM, chosen);
			strcpy(g_ModRandomObjText[index], "Reach the extraction point");
			g_ModRandomNumObjectives++;
		}
	}
}

/**
 * The text a generated objective shows, or NULL when the objective at that
 * index is the stage's own.
 */
char *modRandomGetObjectiveText(s32 index)
{
	if (modRandomIsOn()
			&& index >= 0
			&& index < g_ModRandomNumObjectives) {
		return g_ModRandomObjText[index];
	}

	return NULL;
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

	if (!modRandomIsOn() || !STAGE_IS_LEVEL(stagenum)) {
		return;
	}

	if (g_StageSetup.props == NULL || g_StageSetup.padfiledata == NULL || g_PadsFile == NULL) {
		return;
	}

	if (g_Vars.roomcount <= 0 || g_Rooms == NULL || g_BgPortals == NULL || g_RoomPortals == NULL) {
		return;
	}

	g_ModRandomSeed = g_ModOptions.randomseed ? (u32)g_ModOptions.randomseed : rngRandom();

	// The stage goes into the seed so that one run's Villa and its Chicago are
	// different missions, and the difficulty with it so that Perfect Agent is
	// its own deal rather than the Agent one with more guards.
	modRandomSeed(g_ModRandomSeed + stagenum * 0x9e3779b9u + (lvGetDifficulty() << 24));

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

	modRandomGenerateObjectives(&lists, reached);

	// Nothing to move to if the roll kept the mission's own start.
	g_ModRandomSpawnPad = spawnpad;
	g_ModRandomSpawnState = spawnpad >= 0 ? 1 : 0;

#ifndef PLATFORM_N64
	sysLogPrintf(0, "randomizer: stage 0x%02x seed %u - %d weapons, %d guards, %d keys, "
			"spawn pad %d room %d, %d/%d rooms reachable, %d objectives",
			stagenum, g_ModRandomSeed, lists.numweapons, lists.numchrs, lists.numkeys,
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
void modRandomTick(void)
{
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

	pos->x = pad.pos.x;
	pos->y = pad.pos.y;
	pos->z = pad.pos.z;

	rooms[0] = pad.room;
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
	// structs and their requirements live.
	{
		u32 *cmd = g_ModRandomObjCmds;

		for (i = 0; i < g_ModRandomNumObjectives && cmd; i++) {
			struct objective *objective = (struct objective *)cmd;

			objectiveInsert(objective);

			g_Briefing.objectivenames[objective->index] = L_MISC_042;
			g_Briefing.objectivedifficulties[objective->index] = objective->difficulties;

			while ((u8)PD_BE32(cmd[0]) != OBJTYPE_ENDOBJECTIVE) {
				cmd += setupGetCmdLength(cmd);
			}

			cmd += setupGetCmdLength(cmd);
			cmd = (u32 *)ALIGN16((uintptr_t)cmd); // as the writer left it
		}
	}
}

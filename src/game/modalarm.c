#include <ultra64.h>
#include "constants.h"
#include "math.h"
#include "game/body.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/game_0b0fd0.h"
#include "game/lv.h"
#include "game/modalarm.h"
#include "game/modeldef.h"
#include "game/modelmgr.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/bg.h"
#include "game/modrun.h"
#include "game/modrules.h"
#include "game/mplayer/mplayer.h"
#include "game/pad.h"
#include "game/prop.h"
#include "game/setuputils.h"
#include "bss.h"
#include "lib/ailist.h"
#include "lib/collision.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "system.h"
#include "gebean.h"
#include "gestan.h"
#include <stdlib.h>
#include <string.h>
#endif

/**
 * Guards Alerted!
 *
 * What the stock alarm does is in two places. Every guard runs the global
 * unalerted list, which checks if_alarm_active on each pass and sends the
 * guard to the wake-up list when it is: that is the part that makes a whole
 * stage come looking. Then a few mission scripts - the Villa is the model,
 * func100a_spawn_alarm_responders in setupear.c - spawn reinforcements at a
 * pad while the alarm is on, four of them, thirty seconds apart, once. So
 * "endless waves of guards" was never quite what stock did: the guards a
 * stage has come for you, and then it runs out.
 *
 * This keeps the first part as it is - alarmTick() simply never lets the
 * timer run out while the setting is on, and the stage's own guards do what
 * they always did - and does the second part itself, on every stage, for as
 * long as the level lasts. A guard goes into the world the way the Villa's
 * responders do: spawned at a pad, given a gun, put on the enemy team with
 * its alertness up, and handed the same unalerted list every guard starts on.
 * It hears the alarm on its first pass through that list and comes running,
 * which is the stock behaviour and the reason no AI list of our own is
 * needed.
 *
 * Where they appear is the one thing a mission script knows and this does
 * not. The Villa's responders come out of a doorway the designer chose; this
 * has to choose for itself on a stage it has never seen, so it uses the
 * waypoint graph. Every stage that has guards or simulants has one, since it
 * is how they walk, and a waypoint is by construction a place a chr can stand
 * and find a route from. A random waypoint far enough from every player to be
 * plausible, near enough to matter, and out of everyone's sight - which is
 * chrAdjustPosForSpawn()'s own test, and the reason a guard never pops into
 * view - is a doorway good enough.
 *
 * The chr slots are the hard limit. A mission allocates ten spares over what
 * its setup lists, and the stock reaper in chrSpawnAtCoord() fades a corpse
 * when they run low; a Combat Simulator match has the same ten. The level's
 * cap is reserved at stage load on top of that, through the same numchrs that
 * sizes the model, animation and prop pools, so a wave costs what a wave costs
 * and nothing else pays for it. Guards that die are kept on the list until
 * their chr is freed, so the count of what is standing is honest, and the
 * oldest corpse is retired when the slots are needed for the next one.
 *
 * Multiplayer heads are the other cost. body0f02ce8c() loads a fresh copy of
 * the head every time in a Combat Simulator match, and never frees it,
 * because each simulant's head is offset to sit on its own body - see
 * modbodies.c, which learned this the expensive way. The head numbers this
 * spawns are the stage's active guard heads, four of them in a match, so
 * each is loaded once, offset once for the body every guard here wears, and
 * shared from then on. Solo shares the stage's own copy the way every stock
 * guard does.
 */

#define MODALARM_MAXGUARDS MODALARM_GUARDS_MAX // the list is as long as the count can go
#define MODALARM_MAXHEADS  8    // one loaded head per active head number, in a match
#define MODALARM_MINDIST   800  // cm from the nearest player: not on top of them
#define MODALARM_MAXDIST   4500 // and not so far that they never arrive
#define MODALARM_TRIES     12   // waypoints tried per spawn attempt
#define MODALARM_MEMFLOOR  (512 * 1024) // stage pool to leave for the level itself

/**
 * Ticks between one guard arriving and the next, from the speed setting's
 * guards-per-ten-seconds. How many are up at once is the separate count,
 * since a slow trickle of many and a fast stream of few are different fights.
 */
static s32 modAlarmGetInterval60(void)
{
	return TICKS(600) / modGetGuardSpawnSpeed();
}

/**
 * How long to wait after an attempt that placed nobody, or after retiring a
 * corpse to make room: a second, or the interval when that is shorter, so a
 * swarm is not throttled by its misses.
 */
static s32 modAlarmGetRetry60(void)
{
	s32 interval60 = modAlarmGetInterval60();

	return interval60 < TICKS(60) ? interval60 : TICKS(60);
}

struct modalarmguard {
	struct chrdata *chr;
	s32 chrnum;   // the chr's number when it was spawned; a reused slot changes it
	s32 spawned60; // lvframe60 it arrived, so the oldest corpse can be found
};

struct modalarmhead {
	s32 headnum;
	struct modeldef *modeldef;
};

static struct modalarmguard g_ModAlarmGuards[MODALARM_MAXGUARDS];
static struct modalarmhead g_ModAlarmHeads[MODALARM_MAXHEADS];
static s32 g_ModAlarmNumHeads = 0;
static s32 g_ModAlarmCountdown60 = 0;
static s32 g_ModAlarmNumWaypoints = 0;
static s32 g_ModAlarmReserve = 0;
static bool g_ModAlarmPadGraph = false; // this stage's waypoints are modAlarmBuildPadWaypoints()'s

/**
 * The match's guard tally, by match slot: how many guards each player or
 * simulant has killed, and how many times a guard has killed them. Guards
 * are outside the match's own kill table - a guard kill is worth no score -
 * so this is the count the results screen shows beside it.
 */
static s32 g_ModAlarmGuardKills[MAX_MPCHRS];
static s32 g_ModAlarmGuardDeaths[MAX_MPCHRS];

extern s32 g_ChrSpawnTrace; // chraction.c, --chr-trace

// propobj.c has it, and no header does; it is chrGiveWeapon() with the chr
// model looked up from the weapon number, which is all a spawned guard needs.
struct prop *chrGiveWeaponWithAutoModel(struct chrdata *chr, s32 weaponnum, u32 flags);

/**
 * Guns a guard is given. The list is what a guard can be seen holding in a
 * mission plus the Combat Simulator's own, less the FarSight - a guard that
 * shoots through walls at a player it cannot see is not a wave, it is a
 * turret - and less mines, grenades and the knife, which the AI throws or
 * does not draw.
 */
static const u8 g_ModAlarmGuns[] = {
	WEAPON_FALCON2, WEAPON_FALCON2_SILENCER, WEAPON_FALCON2_SCOPE,
	WEAPON_MAGSEC4, WEAPON_MAULER, WEAPON_PHOENIX,
	WEAPON_DY357MAGNUM, WEAPON_DY357LX,
	WEAPON_CMP150, WEAPON_CYCLONE, WEAPON_CALLISTO, WEAPON_RCP120,
	WEAPON_LAPTOPGUN, WEAPON_DRAGON, WEAPON_K7AVENGER, WEAPON_AR34,
	WEAPON_SUPERDRAGON, WEAPON_SHOTGUN, WEAPON_REAPER, WEAPON_SNIPERRIFLE,
	WEAPON_CROSSBOW, WEAPON_TRANQUILIZER,
	WEAPON_DEVASTATOR, WEAPON_ROCKETLAUNCHER, WEAPON_SLAYER,
};

/**
 * What a guard carries when the stage has no say: the mission side arms.
 */
static const u8 g_ModAlarmDefaultGuns[] = {
	WEAPON_FALCON2, WEAPON_MAGSEC4, WEAPON_CMP150, WEAPON_CYCLONE,
	WEAPON_DRAGON, WEAPON_AR34, WEAPON_K7AVENGER, WEAPON_SHOTGUN,
};

static bool modAlarmIsGun(s32 weaponnum)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_ModAlarmGuns); i++) {
		if (g_ModAlarmGuns[i] == weaponnum) {
			return true;
		}
	}

	return false;
}

/**
 * Whether the weapon behind a number is one a guard can fire: its primary
 * function shoots (single, automatic or projectile). The lists above are
 * stock numbers, and a mod's table can put anything behind one - GE-X's
 * timed mine sits in the Crossbow's slot - which a guard would then be told
 * to shoot. chrTickShoot() reading that throw function as a launcher's was
 * the Guards Alerted! crash on GE-X's Runway.
 */
static bool modAlarmCanChrFire(s32 weaponnum)
{
	struct weapon *weapon = weaponFindById(weaponnum);
	struct weaponfunc *func = weapon ? weapon->functions[FUNC_PRIMARY] : NULL;

	return func != NULL && (func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT;
}

/**
 * Whether a match may hand a guard this gun: the same rule Start Armed uses
 * for a player, which is that the weapon is unlocked. A guard drops what it
 * carries, and a locked gun on the floor is a gun the player has not earned.
 */
static bool modAlarmCanMatchUseGun(s32 weaponnum)
{
	s32 i;

	for (i = 0; i < NUM_MPWEAPONS; i++) {
		if (g_MpWeapons[i].weaponnum == weaponnum) {
			return mpCanSpawnWithWeapon(&g_MpWeapons[i]);
		}
	}

	return false;
}

/**
 * A weapon for the next guard.
 *
 * Random rolls the whole table of guns a guard can hold, the way Start
 * Armed's Random rolls the whole weapon table for a player: the arena's six
 * slots are what everyone is fighting over and are gone within seconds of a
 * big match starting, and eighty guards all carrying the same six is a
 * dull armoury.
 *
 * Otherwise, in a Combat Simulator match it is one of the match's own six
 * slots, so the guards fight with what the arena is stocked with and drop
 * what a player can use. Anywhere else, or when the match holds nothing a
 * guard can carry, the mission side arms.
 */
static s32 modAlarmChooseGun(void)
{
	if (modGetGuardWeapons() == MODALARM_WEAPONS_RANDOM) {
		u8 guns[ARRAYCOUNT(g_ModAlarmGuns)];
		s32 numguns = 0;
		s32 i;

		for (i = 0; i < ARRAYCOUNT(g_ModAlarmGuns); i++) {
			if (modAlarmCanChrFire(g_ModAlarmGuns[i])
					&& (!g_Vars.normmplayerisrunning || modAlarmCanMatchUseGun(g_ModAlarmGuns[i]))) {
				guns[numguns++] = g_ModAlarmGuns[i];
			}
		}

		if (numguns > 0) {
			return guns[rngRandom() % numguns];
		}
	}

	if (g_Vars.normmplayerisrunning) {
		u8 guns[NUM_MPWEAPONSLOTS];
		s32 numguns = 0;
		s32 i;

		for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
			s32 slot = g_MpSetup.weapons[i];

			if (slot >= 0 && slot < NUM_MPWEAPONS && modAlarmIsGun(g_MpWeapons[slot].weaponnum)
					&& modAlarmCanChrFire(g_MpWeapons[slot].weaponnum)) {
				guns[numguns++] = g_MpWeapons[slot].weaponnum;
			}
		}

		if (numguns > 0) {
			return guns[rngRandom() % numguns];
		}
	}

	{
		u8 guns[ARRAYCOUNT(g_ModAlarmDefaultGuns)];
		s32 numguns = 0;
		s32 i;

		for (i = 0; i < ARRAYCOUNT(g_ModAlarmDefaultGuns); i++) {
			if (modAlarmCanChrFire(g_ModAlarmDefaultGuns[i])) {
				guns[numguns++] = g_ModAlarmDefaultGuns[i];
			}
		}

		if (numguns > 0) {
			return guns[rngRandom() % numguns];
		}
	}

	// A table with no gun behind any of these is not one a guard can use;
	// stock's answer is as good as any
	return g_ModAlarmDefaultGuns[rngRandom() % ARRAYCOUNT(g_ModAlarmDefaultGuns)];
}

/**
 * Which uniform the reinforcements wear. The stage's own where a stage has
 * an obvious one, dataDyne shock troopers everywhere else - the Institute,
 * the arenas, a mod's stages. A body is only a model file, and every one of
 * these is in the ROM, so a stage that never loaded it loads it now.
 *
 * Skedar stages get shock troopers too: a Skedar body has no head and a
 * different skeleton, and the head handling below is written for a human.
 */
static s32 modAlarmChooseBody(void)
{
#ifndef PLATFORM_N64
	// A Randomizer run is not the stage's fiction, so the guards holding a
	// room are not the stage's guards: the run picks one body per landing,
	// out of the whole game. See modrun.c.
	if (modRunIsOn()) {
		return modRunChooseBody();
	}
#endif

	switch (g_Vars.stagenum) {
	case STAGE_INFILTRATION:
	case STAGE_RESCUE:
	case STAGE_ESCAPE:
		return BODY_A51TROOPER;
	case STAGE_G5BUILDING:
	case STAGE_MP_G5BUILDING:
		return BODY_G5_SWAT_GUARD;
	case STAGE_CHICAGO:
		return BODY_FBIGUY;
	case STAGE_AIRBASE:
	case STAGE_AIRFORCEONE:
	case STAGE_CRASHSITE:
		return BODY_ALASKAN_GUARD;
	case STAGE_PELAGIC:
	case STAGE_DEEPSEA:
		return BODY_PELAGIC_GUARD;
	default:
		return BODY_DDSHOCK;
	}
}

/**
 * The stage's waypoints, counted the way setupLoadWaypoints() counts them.
 * A stage without any - there are none in stock, but a mod's could - spawns
 * nothing, since there is nowhere it knows a chr can stand.
 */
static s32 modAlarmCountWaypoints(void)
{
	s32 count = 0;

	if (g_StageSetup.waypoints) {
		while (g_StageSetup.waypoints[count].padnum >= 0) {
			count++;
		}
	}

	return count;
}

#define MODALARM_VOID        (-50000.0f) // under the level: cdFindGroundInfoAtCyl() answers -100000 for no floor
#define MODALARM_PADLINKS    10    // links sought per pad when building a graph
#define MODALARM_PADLINKDIST 2500.0f // and no longer than this
#define MODALARM_PADCANDS    32    // nearest candidates tested per pad
#define MODALARM_FLOORLINKDIST 1000.0f // a link's length at most among the floors' places
#define MODALARM_NEARROOMS   64    // rooms a pad may link into, -1 ended
#define MODALARM_MATCHHOPS   3     // portals crossed to them in a match

/**
 * The rooms up to MODALARM_MATCHHOPS portals from a room, not counting it,
 * ended by -1: how far a match's pad graph looks for a pad to link to.
 * GoldenEye cut its levels into many small rooms (Library's 92 hold 84 pads),
 * so pads a room or two apart down a clear corridor were never linked when
 * only the rooms beside a pad's own were looked in - Library came out as 18
 * pieces. The walk (modAlarmPadsWalkable()) still decides every link.
 */
static void modAlarmRoomsWithinHops(RoomNum room, RoomNum *dst)
{
	RoomNum ring[MODALARM_NEARROOMS];
	s32 count = 0;
	s32 start = 0;
	s32 hop;
	s32 i;
	s32 j;
	s32 k;

	dst[0] = -1;

	for (hop = 0; hop < MODALARM_MATCHHOPS; hop++) {
		const s32 end = count;

		for (i = hop == 0 ? -1 : start; i < end; i++) {
			RoomNum from = i < 0 ? room : dst[i];
			s32 num = bgRoomGetNeighbours(from, ring, MODALARM_NEARROOMS - 1);

			for (j = 0; j < num && count < MODALARM_NEARROOMS - 1; j++) {
				bool seen = ring[j] == room;

				for (k = 0; !seen && k < count; k++) {
					seen = dst[k] == ring[j];
				}

				if (!seen) {
					dst[count++] = ring[j];
				}
			}
		}

		start = end;
	}

	dst[count] = -1;
}

#define MODALARM_WALKSTEP    40.0f // a link is walked in steps this long
#define MODALARM_WALKSTAIR   40.0f // and the floor may rise or fall this much a step
#define MODALARM_WALKKNEE    20.0f // an edge lower than this is stepped over (chrGetBbox())
#define MODALARM_WALKHEAD    150.0f // and one higher than this is walked under
#define MODALARM_WALKWAIST   30.0f // the height the steps are taken at, clear of the floor

/**
 * The floor under a point, looked for from a little above it, or false when
 * there is none or it kills.
 */
static bool modAlarmWalkGround(struct coord *pos, f32 above, RoomNum *rooms, f32 *ground, RoomNum *floorroom)
{
	struct coord query = *pos;
	u16 floorflags = 0;

	query.y += above;
	*floorroom = -1;
	*ground = cdFindGroundInfoAtCyl(&query, 20, rooms, NULL, NULL, &floorflags, floorroom, NULL, NULL);

	return *floorroom >= 0 && *ground > -100000.0f && (floorflags & GEOFLAG_DIE) == 0;
}

/**
 * Whether a chr could walk from one pad to another along the straight line
 * between them, in both directions.
 *
 * The line is walked in short steps the way a chr's own move goes: the floor
 * under each step found from a stair's height above the last, no step up or
 * down more than a stair, and a cylinder from knee to head moved from each
 * step to the next through the bg. It has to end on the second pad's floor.
 *
 * The first version asked one straight line and one cylinder at the pads'
 * heights. That linked a catwalk to the floor under it - the line runs off
 * the edge and the cylinder, tested flat, meets no wall - and a guard routed
 * off Facility's gantry stood at its railing (F3 20260928-170555); and it
 * never linked a flight of stairs, whose risers are walls to a flat
 * cylinder, so the two levels of a room were two pieces of the graph and the
 * guards dealt on one never came to a player on the other.
 */
static bool modAlarmPadsWalkable(struct coord *from, RoomNum fromroom, struct coord *to, RoomNum toroom)
{
	struct coord cur;
	struct coord next;
	RoomNum currooms[2];
	RoomNum nextrooms[2];
	RoomNum near[12];
	RoomNum floorroom;
	f32 ground;
	f32 toground;
	f32 dx = to->x - from->x;
	f32 dz = to->z - from->z;
	s32 steps = (s32)(sqrtf(dx * dx + dz * dz) / MODALARM_WALKSTEP) + 1;
	s32 k;

	currooms[0] = toroom;
	currooms[1] = -1;

	if (!modAlarmWalkGround(to, 10.0f, currooms, &toground, &floorroom)) {
		return false;
	}

	currooms[0] = fromroom;
	currooms[1] = -1;

	if (!modAlarmWalkGround(from, 10.0f, currooms, &ground, &floorroom)) {
		return false;
	}

	currooms[0] = floorroom;

	// Each step is taken at waist height rather than on the floor, clear of
	// the floor's own edges
	cur.x = from->x;
	cur.y = ground + MODALARM_WALKWAIST;
	cur.z = from->z;

	for (k = 1; k <= steps; k++) {
		f32 frac = (f32)k / (f32)steps;
		f32 stepground;
		s32 i;

		next.x = from->x + dx * frac;
		next.z = from->z + dz * frac;
		next.y = ground;

		// The floor is looked for in the room the last step stood in and the
		// rooms beside it, and the room it is in is where the walk now is.
		// Rooms overlap in height - a stairwell over the floor below it - so
		// following portals at waist height leaves the walk in the lower
		// room and finds its floor, a drop, under the stairs.
		near[0] = currooms[0];

		for (i = 1; i < ARRAYCOUNT(near); i++) {
			near[i] = -1;
		}

		bgRoomGetNeighbours(currooms[0], &near[1], ARRAYCOUNT(near) - 2);

		if (!modAlarmWalkGround(&next, MODALARM_WALKSTAIR, near, &stepground, &floorroom)
				|| stepground < ground - MODALARM_WALKSTAIR) {
			return false;
		}

		next.y = stepground + MODALARM_WALKWAIST;
		nextrooms[0] = floorroom;
		nextrooms[1] = -1;

		if (cdExamCylMove05(&cur, currooms, &next, nextrooms, CDTYPE_BG, true,
					MODALARM_WALKHEAD - MODALARM_WALKWAIST, MODALARM_WALKKNEE - MODALARM_WALKWAIST) != CDRESULT_NOCOLLISION) {
			return false;
		}

		cur = next;
		currooms[0] = floorroom;
		ground = stepground;
	}

	// On the other pad's floor, not on one above or below it
	return ground - toground < MODALARM_WALKSTAIR && toground - ground < MODALARM_WALKSTAIR;
}

/**
 * A link of a match's graph among the floors' places. Perfect Dark's cylinder
 * walk (modAlarmPadsWalkable()) meets, on a converted GoldenEye level, walls
 * the conversion raised for a storey under or over the floor walked, and
 * alone it cut Cradle's platform from its walkways; so a link GoldenEye's own
 * walk along its tile graph makes (geStanLinks(), the graph that decides
 * where a body stands and which walls stop it) is taken as well. Either walk
 * is enough.
 */
static bool modAlarmLinkWalkable(struct coord *from, RoomNum fromroom, struct coord *to, RoomNum toroom)
{
	RoomNum rooms[2];
	RoomNum floorroom;
	f32 fromground;
	f32 toground;
	s32 stan;

	rooms[0] = fromroom;
	rooms[1] = -1;

	if (!modAlarmWalkGround(from, 10.0f, rooms, &fromground, &floorroom)) {
		return false;
	}

	rooms[0] = toroom;

	if (!modAlarmWalkGround(to, 10.0f, rooms, &toground, &floorroom)) {
		return false;
	}

	stan = geStanLinks(from, fromground, to, toground, false);

	return stan > 0 || modAlarmPadsWalkable(from, fromroom, to, toroom);
}

#define MODALARM_FLOORGRID   50.0f  // floors are looked for this far apart in each room
#define MODALARM_FLOORSPACE  200.0f // and a waypoint kept no nearer than this to another
#define MODALARM_FLOORLAYERS 6      // floors one over another in a room
#define MODALARM_FLOORMAX    1500   // waypoints added to a stage at most
#define MODALARM_ROOMMAX     200    // and to a room (struct room counts them in a u8)
#define MODALARM_FLOORCELLS  40000  // grid points a room is searched at, at most
#define MODALARM_FLOORLIFT   50.0f  // a place stands this far over its floor, as a settled pad does

/**
 * Whether a place on a floor is MODALARM_FLOORSPACE from every place already
 * taken in the same room, or on another level of it. `haverooms` is read
 * every `stride` entries.
 */
static bool modAlarmFloorSpotFree(const struct coord *spot, RoomNum room, const struct coord *have, const RoomNum *haverooms, s32 stride, s32 numhave, f32 space)
{
	s32 i;

	for (i = 0; i < numhave; i++) {
		const struct coord *h = &have[i];
		f32 dx = h->x - spot->x;
		f32 dz = h->z - spot->z;

		if (haverooms[i * stride] == room
				&& dx * dx + dz * dz < space * space
				&& fabsf(h->y - spot->y) <= 100.0f) {
			return false;
		}
	}

	return true;
}

/**
 * Places a chr could stand all over a stage's floors, for a match's waypoint
 * graph on a map whose only pads are its spawn points (every GoldenEye Arenas
 * map). A graph of the spawn points alone is too thin to walk: Complex's 49
 * pads made 24 pieces and 21 of its 44 rooms had no waypoint in or beside
 * them, so waypointFindClosestToPos() found nothing for a simulant or its
 * target there and the simulants stood on their spawn points all match (F3
 * 20261001-145625).
 *
 * Each room is searched on a grid for its floors, top down so the levels of a
 * room over one another are all found; a floor counts where it is the room's
 * own, does not kill, has room for a body over it, and - on a converted
 * GoldenEye level - is a floor of GoldenEye's tile graph, not the top of a
 * wall or a crate. Kept places are MODALARM_FLOORSPACE apart, and as far
 * from the pads already taken (`have`). Nothing random: the same stage gives
 * the same places. Returns how many were written to `out`/`outrooms`.
 */
static s32 modAlarmSampleFloors(const struct coord *have, const RoomNum (*haverooms)[2], s32 numhave, struct coord *out, RoomNum *outrooms, s32 max, f32 space)
{
	// Places clear of the walls by more than a body first, then, where a
	// corridor is too narrow for any, those with room for one
	static const f32 clearances[] = { 50.0f, 20.0f };
	s32 count = 0;
	s32 pass;
	s32 r;

	for (pass = 0; pass < ARRAYCOUNT(clearances); pass++) {
		// a pad's room is a signed ten bit field (padUnpack())
		for (r = 1; r < g_Vars.roomcount && r < 0x200 && count < max; r++) {
			struct room *room = &g_Rooms[r];
			f32 grid = MODALARM_FLOORGRID;
			f32 w = room->bbmax[0] - room->bbmin[0];
			f32 d = room->bbmax[2] - room->bbmin[2];
			s32 inroom = 0;
			s32 nx;
			s32 nz;
			s32 ix;
			s32 iz;

			if (w <= 0.0f || d <= 0.0f) {
				continue;
			}

			// The room's waypoints are counted in a u8 (struct room)
			for (ix = 0; ix < numhave; ix++) {
				inroom += haverooms[ix][0] == r;
			}

			for (ix = 0; ix < count; ix++) {
				inroom += outrooms[ix] == r;
			}

			while ((w / grid) * (d / grid) > MODALARM_FLOORCELLS) {
				grid *= 2.0f;
			}

			// An odd number of lines each way, so one runs down the middle
			// of the room
			nx = 2 * (s32)(w / (2.0f * grid)) + 1;
			nz = 2 * (s32)(d / (2.0f * grid)) + 1;

			for (ix = 0; ix < nx && count < max && inroom < MODALARM_ROOMMAX; ix++) {
				for (iz = 0; iz < nz && count < max && inroom < MODALARM_ROOMMAX; iz++) {
					struct coord query;
					s32 layer;

					query.x = room->bbmin[0] + w * (ix + 0.5f) / nx;
					query.y = room->bbmax[1] + 10.0f;
					query.z = room->bbmin[2] + d * (iz + 0.5f) / nz;

					for (layer = 0; layer < MODALARM_FLOORLAYERS; layer++) {
						RoomNum rooms[2];
						RoomNum floorroom = -1;
						u16 floorflags = 0;
						struct coord spot;
						f32 ground;
						f32 stan;

						rooms[0] = r;
						rooms[1] = -1;

						ground = cdFindGroundInfoAtCyl(&query, 20, rooms, NULL, NULL, &floorflags, &floorroom, NULL, NULL);

						if (floorroom < 0 || ground <= MODALARM_VOID || ground < room->bbmin[1] - 50.0f) {
							break;
						}

						// The next floor down is looked for from under this one
						query.y = ground - 30.0f;

						if (floorroom != r || (floorflags & GEOFLAG_DIE)) {
							continue;
						}

						spot.x = query.x;
						spot.y = ground + MODALARM_FLOORLIFT;
						spot.z = query.z;

						// On a converted GoldenEye level, a floor of its tile graph
						stan = geStanFloorAt(&spot);

						if (stan > -1e29f && fabsf(stan - ground) > 30.0f) {
							continue;
						}

						if (!modAlarmFloorSpotFree(&spot, r, have, &haverooms[0][0], 2, numhave, space)
								|| !modAlarmFloorSpotFree(&spot, r, out, outrooms, 1, count, space)) {
							continue;
						}

						// Room to stand, the walls a body would meet from its
						// knee to its head
						if (cdTestVolume(&spot, clearances[pass], rooms, CDTYPE_BG, CHECKVERTICAL_YES,
									185.0f - MODALARM_FLOORLIFT, 20.0f - MODALARM_FLOORLIFT) == CDRESULT_COLLISION) {
							continue;
						}

						out[count] = spot;
						outrooms[count] = r;
						count++;
						inroom++;
					}
				}
			}
		}
	}

	return count;
}

/**
 * Puts `count` more pads on the stage, at `pts` in `ptrooms`, after its own,
 * and returns the first one's number (or -1, the stage left as it was).
 *
 * The pads file is the stage's copy, with the pads looked up through
 * g_PadOffsets (u16 offsets from padfiledata): the file is copied as far as
 * its last pad into a buffer with room for the new ones after it, and the
 * offsets into one of their own. Whatever was already pointed at in the old
 * copy (waypoints, cover) stays where it is, in the stage pool.
 */
static s32 modAlarmAppendPads(const struct coord *pts, const RoomNum *ptrooms, s32 count)
{
	const s32 oldnum = g_PadsFile->numpads;
	const u32 headflags = PADFLAG_UPALIGNTOY | PADFLAG_LOOKALIGNTOZ;
	size_t end = (u8 *)&g_PadOffsets[oldnum] - (u8 *)g_StageSetup.padfiledata;
	size_t at;
	u8 *buf;
	u16 *offsets;
	s32 i;

	for (i = 0; i < oldnum; i++) {
		u32 header = *(u32 *)&g_StageSetup.padfiledata[g_PadOffsets[i]];
		u32 flags = header >> 14;
		size_t len = 4;

		len += (flags & PADFLAG_INTPOS) ? 8 : 12;
		len += (flags & (PADFLAG_UPALIGNTOX | PADFLAG_UPALIGNTOY | PADFLAG_UPALIGNTOZ)) ? 0 : 12;
		len += (flags & (PADFLAG_LOOKALIGNTOX | PADFLAG_LOOKALIGNTOY | PADFLAG_LOOKALIGNTOZ)) ? 0 : 12;
		len += (flags & PADFLAG_HASBBOXDATA) ? 24 : 0;

		if (g_PadOffsets[i] + len > end) {
			end = g_PadOffsets[i] + len;
		}
	}

	at = ALIGN16(end);

	if (count <= 0 || at + (size_t)count * 16 > 0xffff) {
		return -1;
	}

	buf = mempAlloc(ALIGN16(at + count * 16), MEMPOOL_STAGE);
	offsets = mempAlloc(ALIGN16((oldnum + count) * sizeof(u16)), MEMPOOL_STAGE);

	if (buf == NULL || offsets == NULL) {
		return -1;
	}

	memcpy(buf, g_StageSetup.padfiledata, end);
	memcpy(offsets, g_PadOffsets, oldnum * sizeof(u16));

	for (i = 0; i < count; i++) {
		u32 *header = (u32 *)&buf[at];
		f32 *fpos = (f32 *)&buf[at + 4];

		*header = (headflags << 14) | (((u32)ptrooms[i] & 0x3ff) << 4);
		fpos[0] = pts[i].x;
		fpos[1] = pts[i].y;
		fpos[2] = pts[i].z;
		offsets[oldnum + i] = (u16)at;
		at += 16;
	}

	g_StageSetup.padfiledata = (s8 *)buf;
	g_PadsFile = (struct padsfileheader *)buf;
	g_PadOffsets = offsets;
	g_PadsFile->numpads = oldnum + count;

	return oldnum;
}

/**
 * A waypoint graph for a stage that came without one.
 *
 * Every GoldenEye Arenas map is such a stage: GoldenEye's multiplayer setups
 * never carried a path table, since nothing in GoldenEye's multiplayer walked,
 * and the converter has nothing to copy. The alarm needs a graph twice over -
 * its guards spawn on waypoints, and "coming running" is chrGoToRoomPos(), a
 * route from one waypoint to another - so with none the alarm spawned nobody,
 * and a Randomizer run dealt "Eliminate 6 hostiles" into rooms nothing would
 * ever come to (tester, 2026-09-28: Statue Park, Facility, Bunker, Cradle,
 * Temple). The run's landing is a waypoint too, which is why every one of
 * those hops logged "land pad -1".
 *
 * So during a Randomizer run, and in a match with simulants (who stood still
 * on such a stage, having no route anywhere), a stage with no waypoints gets
 * them from its pads - and in a match, from its floors as well
 * (modAlarmSampleFloors(), modAlarmLinkWalkable()): a match's spawn points
 * alone were too few to walk between (F3 20261001-145625, Complex) -
 * after setupPreparePads() has put each pad in its room and before
 * setupLoadWaypoints() files the waypoints by room:
 *
 * - a waypoint on every pad a player could be put down on
 *   (modRandomPadCanSpawn(), the landing's own test);
 * - a link from each to its nearest few, where the two are in the same or
 *   neighbouring rooms (waypointFindClosestToPos() only ever looks that far;
 *   in a match, rooms up to MODALARM_MATCHHOPS portals away)
 *   and a chr could walk the line between them (modAlarmPadsWalkable());
 * - one waygroup per connected piece, with no links between groups, so a
 *   route between two pieces is refused rather than walked into a wall.
 *
 * Built from the stage's own data and nothing random, so a seed deals the
 * same landing on it every time.
 */
void modAlarmBuildPadWaypoints(void)
{
#ifndef PLATFORM_N64
	s32 numpads;
	s32 n = 0;
	s32 i;
	s32 j;
	s32 numlinks = 0;
	s32 numsettled = 0;
	s32 numfloor = 0;
	s32 numhigh = 0;
	f32 linkdist;
	s32 numgroups = 0;
	s32 largest = 0;
	s16 *spots;
	struct coord *pos;
	RoomNum (*rooms)[2];
	RoomNum (*near)[MODALARM_NEARROOMS];
	const bool wide = !modRunIsOn();
	u8 *adj;
	s32 *deg;
	s32 *group;
	s32 *queue;
	s32 *nblists;
	s32 *grlists;
	s32 *noneighbours;
	struct waypoint *waypoints;
	struct waygroup *groups;
	s32 nbpos;
	s32 grpos;
	u64 started;

	g_ModAlarmPadGraph = false;

	// For a Randomizer run's landings and guards, and for a match's
	// simulants, who route between waypoints and with none stood where they
	// spawned all match (F3 20260928-131305, GoldenEye Arenas' Library). A
	// mission on the same map plays exactly as it did before.
	if (!(modRunIsOn() || (g_Vars.normmplayerisrunning && mpHasSimulants()))
			|| g_PadsFile == NULL || g_StageSetup.padfiledata == NULL) {
		return;
	}

	if (g_StageSetup.waypoints != NULL && g_StageSetup.waypoints[0].padnum >= 0) {
		return;
	}

	numpads = g_PadsFile->numpads;
	started = sysGetMicroseconds();

	if (numpads <= 1) {
		return;
	}

	spots = malloc(numpads * sizeof(*spots));
	pos = malloc(numpads * sizeof(*pos));
	rooms = malloc(numpads * sizeof(*rooms));
	near = malloc(numpads * sizeof(*near));

	for (i = 0; i < numpads; i++) {
		struct pad pad;

		// A pad floating high over its floor (all of Complex's) is lowered
		// onto it first, or the drop rule refuses it
		if (modRandomGetVersion() >= 4 && modRandomPadSettle(i)) {
			numsettled++;
		}

		padUnpack(i, PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (pad.room > 0 && pad.room < g_Vars.roomcount && modRandomPadCanSpawn(i)) {
			// With the floors' own places, a pad left standing high over
			// its floor (one under the settling drop) is not a waypoint: a
			// simulant sent to it arrives nowhere under it and waits there
			// (Complex's pad 22, 350 over its walkway)
			if (wide) {
				RoomNum padrooms[2] = { pad.room, -1 };
				RoomNum floorroom;
				f32 ground;

				if (modAlarmWalkGround(&pad.pos, 10.0f, padrooms, &ground, &floorroom)
						&& pad.pos.y - ground > MODALARM_FLOORLIFT + 50.0f) {
					numhigh++;
					continue;
				}
			}

			spots[n] = i;
			pos[n] = pad.pos;
			rooms[n][0] = pad.room;
			rooms[n][1] = -1;
			if (wide) {
				modAlarmRoomsWithinHops(pad.room, near[n]);
			} else {
				bgRoomGetNeighbours(pad.room, near[n], 10);
			}
			n++;
		}
	}

	// A match's simulants walk all over the map, not only from one spawn
	// point to the next: the floors get waypoints of their own
	if (wide) {
		struct coord *extra = malloc(MODALARM_FLOORMAX * sizeof(*extra));
		RoomNum *extrarooms = malloc(MODALARM_FLOORMAX * sizeof(*extrarooms));
		f32 space = MODALARM_FLOORSPACE;
		s32 numextra = 0;
		s32 first = -1;
		s32 tries;

		// A map too big for the places at that spacing (Statue Park) has
		// them further apart rather than its last rooms going without
		for (tries = 0; extra && extrarooms && tries < 4; tries++, space *= 1.4f) {
			numextra = modAlarmSampleFloors(pos, rooms, n, extra, extrarooms, MODALARM_FLOORMAX, space);

			if (numextra < MODALARM_FLOORMAX) {
				break;
			}
		}

		if (numextra > 0) {
			first = modAlarmAppendPads(extra, extrarooms, numextra);
		}

		if (first >= 0) {
			spots = realloc(spots, (n + numextra) * sizeof(*spots));
			pos = realloc(pos, (n + numextra) * sizeof(*pos));
			rooms = realloc(rooms, (n + numextra) * sizeof(*rooms));
			near = realloc(near, (n + numextra) * sizeof(*near));

			for (i = 0; i < numextra; i++) {
				spots[n] = first + i;
				pos[n] = extra[i];
				rooms[n][0] = extrarooms[i];
				rooms[n][1] = -1;
				modAlarmRoomsWithinHops(extrarooms[i], near[n]);
				n++;
			}

			numfloor = numextra;
		}

		free(extra);
		free(extrarooms);
	}

	if (n < 2) {
		free(spots); free(pos); free(rooms); free(near);
		return;
	}

	// Among the floors' places the nearest few are never far, and a long
	// link is a long walk to test that a shorter chain already makes
	linkdist = numfloor > 0 ? MODALARM_FLOORLINKDIST : MODALARM_PADLINKDIST;

	adj = calloc((size_t)n * n, 1);
	deg = calloc(n, sizeof(*deg));
	group = malloc(n * sizeof(*group));
	queue = malloc(n * sizeof(*queue));

	for (i = 0; i < n; i++) {
		s32 cand[MODALARM_PADCANDS];
		f32 canddist[MODALARM_PADCANDS];
		s32 numcands = 0;
		s32 linked = 0;
		s32 k;

		// The nearest few in reach, nearest first
		for (j = 0; j < n; j++) {
			f32 dx = pos[j].x - pos[i].x;
			f32 dy = pos[j].y - pos[i].y;
			f32 dz = pos[j].z - pos[i].z;
			f32 dist = dx * dx + dy * dy + dz * dz;
			// Among the floors' places the walk alone decides: a converted
			// level's rooms are not always joined by portals where its
			// floors meet (Cradle's platforms)
			bool neighbour = numfloor > 0 || rooms[j][0] == rooms[i][0];

			if (j == i || dist > linkdist * linkdist) {
				continue;
			}

			for (k = 0; !neighbour && k < MODALARM_NEARROOMS && near[i][k] != -1; k++) {
				neighbour = near[i][k] == rooms[j][0];
			}

			if (!neighbour) {
				continue;
			}

			for (k = numcands; k > 0 && canddist[k - 1] > dist; k--) {
				if (k < MODALARM_PADCANDS) {
					cand[k] = cand[k - 1];
					canddist[k] = canddist[k - 1];
				}
			}

			if (k < MODALARM_PADCANDS) {
				cand[k] = j;
				canddist[k] = dist;

				if (numcands < MODALARM_PADCANDS) {
					numcands++;
				}
			}
		}

		for (k = 0; k < numcands; k++) {
			// Among the floors' places a pad's nearest few are all in its
			// own room, and with them counted the room's ways out were never
			// tried: there, a link into another room does not count
			bool counts;

			j = cand[k];
			counts = numfloor == 0 || rooms[j][0] == rooms[i][0];

			if (counts && linked >= MODALARM_PADLINKS) {
				continue;
			}

			if (adj[(size_t)i * n + j]) {
				linked += counts;
				continue;
			}

			if (!(numfloor > 0 ? modAlarmLinkWalkable(&pos[i], rooms[i][0], &pos[j], rooms[j][0])
						: modAlarmPadsWalkable(&pos[i], rooms[i][0], &pos[j], rooms[j][0]))) {
				continue;
			}

			adj[(size_t)i * n + j] = adj[(size_t)j * n + i] = 1;
			deg[i]++;
			deg[j]++;
			numlinks++;
			linked += counts;
		}
	}

	// The connected pieces, each a waygroup
	for (i = 0; i < n; i++) {
		group[i] = -1;
	}

	for (i = 0; i < n; i++) {
		s32 head = 0;
		s32 tail = 0;

		if (group[i] >= 0) {
			continue;
		}

		group[i] = numgroups;
		queue[tail++] = i;

		while (head < tail) {
			s32 cur = queue[head++];

			for (j = 0; j < n; j++) {
				if (adj[(size_t)cur * n + j] && group[j] < 0) {
					group[j] = numgroups;
					queue[tail++] = j;
				}
			}
		}

		if (tail > largest) {
			largest = tail;
		}

		numgroups++;
	}

	waypoints = mempAlloc(ALIGN16((n + 1) * sizeof(struct waypoint)), MEMPOOL_STAGE);
	nblists = mempAlloc(ALIGN16((numlinks * 2 + n) * sizeof(s32)), MEMPOOL_STAGE);
	groups = mempAlloc(ALIGN16((numgroups + 1) * sizeof(struct waygroup)), MEMPOOL_STAGE);
	grlists = mempAlloc(ALIGN16((n + numgroups) * sizeof(s32)), MEMPOOL_STAGE);
	noneighbours = mempAlloc(ALIGN16(sizeof(s32)), MEMPOOL_STAGE);

	if (waypoints && nblists && groups && grlists && noneighbours) {
		nbpos = 0;

		for (i = 0; i < n; i++) {
			waypoints[i].padnum = spots[i];
			waypoints[i].neighbours = &nblists[nbpos];
			waypoints[i].groupnum = group[i];
			waypoints[i].step = -1;

			for (j = 0; j < n; j++) {
				if (adj[(size_t)i * n + j]) {
					nblists[nbpos++] = j;
				}
			}

			nblists[nbpos++] = -1;
		}

		waypoints[n].padnum = -1;
		waypoints[n].neighbours = NULL;
		waypoints[n].groupnum = 0;
		waypoints[n].step = 0;

		*noneighbours = -1;
		grpos = 0;

		for (j = 0; j < numgroups; j++) {
			groups[j].neighbours = noneighbours;
			groups[j].waypoints = &grlists[grpos];
			groups[j].step = -1;

			for (i = 0; i < n; i++) {
				if (group[i] == j) {
					grlists[grpos++] = i;
				}
			}

			grlists[grpos++] = -1;
		}

		groups[numgroups].neighbours = NULL;
		groups[numgroups].waypoints = NULL;
		groups[numgroups].step = 0;

		g_StageSetup.waypoints = waypoints;
		g_StageSetup.waygroups = groups;
		g_ModAlarmPadGraph = true;


		sysLogPrintf(LOG_NOTE, "alarm: stage 0x%02x has no waypoints; built %d from its %d pads (%d lowered onto their floors, %d left out standing high) and %d places on its floors, %d links, %d groups (largest %d), in %d ms",
				g_Vars.stagenum, n, numpads, numsettled, numhigh, numfloor, numlinks, numgroups, largest,
				(s32)((sysGetMicroseconds() - started) / 1000));
	}

	free(spots);
	free(pos);
	free(rooms);
	free(near);
	free(adj);
	free(deg);
	free(group);
	free(queue);
#endif
}

/**
 * A stage's own waypoint links that climb where nothing carries a chr up.
 *
 * A Combat Simulator arena loaded solo - every Randomizer hop onto one - takes
 * its solo setup, which is a stub: no props, so none of the arena's lifts.
 * The arena's waypoints still link the bottom of each lift shaft to its top,
 * and the run's reach rule reads the waygroups, which say those levels join.
 * So Fortress's guards were dealt into the pits under a landing on the
 * battlements, and routed to the missing lift, stood at the bottom of the
 * shaft looking up at the player for the rest of the room (F3
 * 20260929-193910: pad 147, room 55, is linked 546 units straight up to pad
 * 148 in room 58).
 *
 * During a run on a stock arena, once the props are up, a link that rises more
 * than a stair and steeply - a shaft or the step off a lift's landing, not a
 * slope - is walked (modAlarmPadsWalkable(), the pad graph's own
 * test) from the lower end; one that cannot be walked, with no lift standing
 * near either end, is cut both ways. Only on the arenas: a mission's setup has
 * its lifts, and the walk test refuses some of the slopes a mission's own
 * waypoints climb (Crash Site's, Villa's steps), which its guards do walk. The waygroups are then rebuilt as the
 * graph's connected pieces, as a pad-built graph's are, so a piece the cut
 * left alone is one the reach rule knows is alone. A stage with nothing to cut
 * is left exactly as it was.
 */
#define MODALARM_CLIMBRISE 100.0f // floors further apart than this are a climb to test
#define MODALARM_CLIMBSLOPE 0.6f  // and rise more than this for every unit they run
#define MODALARM_LIFTNEAR  400.0f // a lift this close to either end carries the climb
#define MODALARM_MAXLIFTS  64

void modAlarmCutLiftlessClimbs(void)
{
#ifndef PLATFORM_N64
	struct waypoint *waypoints = g_StageSetup.waypoints;
	struct defaultobj *obj;
	struct coord lifts[MODALARM_MAXLIFTS];
	s32 numlifts = 0;
	s32 n = 0;
	s32 numcut = 0;
	s32 numlinks = 0;
	s32 numgroups = 0;
	s32 largest = 0;
	struct coord *pos;
	f32 *floory;
	RoomNum *rooms;
	s32 *group;
	s32 *queue;
	s32 *grlists;
	s32 *noneighbours;
	struct waygroup *groups;
	s32 grpos;
	s32 i;
	s32 j;
	u64 started;

	if (!modRunIsOn() || g_ModAlarmPadGraph || waypoints == NULL || waypoints[0].padnum < 0
			|| g_StageSetup.props == NULL || !modRunStageIsStockArena(g_Vars.stagenum)) {
		return;
	}

	started = sysGetMicroseconds();

	for (obj = (struct defaultobj *)g_StageSetup.props; obj->type != OBJTYPE_END;
			obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj))) {
		if (obj->type == OBJTYPE_LIFT && obj->prop && numlifts < MODALARM_MAXLIFTS) {
			lifts[numlifts++] = obj->prop->pos;
		}
	}

	while (waypoints[n].padnum >= 0) {
		n++;
	}

	pos = malloc(n * sizeof(*pos));
	floory = malloc(n * sizeof(*floory));
	rooms = malloc(n * sizeof(*rooms));

	if (pos == NULL || floory == NULL || rooms == NULL) {
		free(pos); free(floory); free(rooms);
		return;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;
		RoomNum padrooms[2];
		RoomNum floorroom;
		f32 ground;

		padUnpack(waypoints[i].padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);
		pos[i] = pad.pos;
		rooms[i] = pad.room;
		floory[i] = pad.pos.y;

		padrooms[0] = pad.room;
		padrooms[1] = -1;

		if (pad.room > 0 && modAlarmWalkGround(&pad.pos, 10.0f, padrooms, &ground, &floorroom)) {
			floory[i] = ground;
		}
	}

	for (i = 0; i < n; i++) {
		s32 *nb = waypoints[i].neighbours;
		s32 out = 0;

		if (nb == NULL) {
			continue;
		}

		for (j = 0; nb[j] >= 0; j++) {
			const s32 k = nb[j] & 0x3fffffff;
			bool cut = false;

			if (k < n && fabsf(floory[k] - floory[i]) > MODALARM_CLIMBRISE
					&& fabsf(floory[k] - floory[i]) * fabsf(floory[k] - floory[i])
						> MODALARM_CLIMBSLOPE * MODALARM_CLIMBSLOPE * ((pos[k].x - pos[i].x) * (pos[k].x - pos[i].x) + (pos[k].z - pos[i].z) * (pos[k].z - pos[i].z))) {
				const s32 lo = floory[k] < floory[i] ? k : i;
				const s32 hi = lo == i ? k : i;
				bool lifted = false;
				s32 l;

				for (l = 0; l < numlifts && !lifted; l++) {
					f32 ax = lifts[l].x - pos[lo].x;
					f32 az = lifts[l].z - pos[lo].z;
					f32 bx = lifts[l].x - pos[hi].x;
					f32 bz = lifts[l].z - pos[hi].z;

					lifted = ax * ax + az * az < MODALARM_LIFTNEAR * MODALARM_LIFTNEAR
						|| bx * bx + bz * bz < MODALARM_LIFTNEAR * MODALARM_LIFTNEAR;
				}

				cut = !lifted && rooms[lo] > 0 && rooms[hi] > 0
					&& !modAlarmPadsWalkable(&pos[lo], rooms[lo], &pos[hi], rooms[hi]);
			}

			if (cut) {
				if (g_ChrSpawnTrace) {
					sysLogPrintf(LOG_NOTE, "alarm: cut pad %d (room %d, floor %.0f) - pad %d (room %d, floor %.0f), %.0f apart",
							waypoints[i].padnum, rooms[i], floory[i], waypoints[k].padnum, rooms[k], floory[k],
							sqrtf((pos[k].x - pos[i].x) * (pos[k].x - pos[i].x) + (pos[k].z - pos[i].z) * (pos[k].z - pos[i].z)));
				}

				numcut++;
				continue;
			}

			nb[out++] = nb[j];
		}

		nb[out] = -1;
		numlinks += out;
	}

	// A link cut one way (its far end's list is walked from the other side)
	// is cut the other: the climb test is the same pair of pads either way,
	// so both lists lost it.

	if (numcut == 0) {
		free(pos); free(floory); free(rooms);
		return;
	}

	group = malloc(n * sizeof(*group));
	queue = malloc(n * sizeof(*queue));

	if (group == NULL || queue == NULL) {
		free(pos); free(floory); free(rooms); free(group); free(queue);
		return;
	}

	for (i = 0; i < n; i++) {
		group[i] = -1;
	}

	// Connected pieces, links taken either way
	for (i = 0; i < n; i++) {
		s32 head = 0;
		s32 tail = 0;

		if (group[i] >= 0) {
			continue;
		}

		group[i] = numgroups;
		queue[tail++] = i;

		while (head < tail) {
			const s32 cur = queue[head++];
			const s32 *nb = waypoints[cur].neighbours;
			s32 m;

			for (j = 0; nb && nb[j] >= 0; j++) {
				const s32 k = nb[j] & 0x3fffffff;

				if (k < n && group[k] < 0) {
					group[k] = numgroups;
					queue[tail++] = k;
				}
			}

			// And the links into it from waypoints that list it
			for (m = 0; m < n; m++) {
				const s32 *mb = waypoints[m].neighbours;

				if (group[m] >= 0 || mb == NULL) {
					continue;
				}

				for (j = 0; mb[j] >= 0; j++) {
					if ((mb[j] & 0x3fffffff) == cur) {
						group[m] = numgroups;
						queue[tail++] = m;
						break;
					}
				}
			}
		}

		if (tail > largest) {
			largest = tail;
		}

		numgroups++;
	}

	groups = mempAlloc(ALIGN16((numgroups + 1) * sizeof(struct waygroup)), MEMPOOL_STAGE);
	grlists = mempAlloc(ALIGN16((n + numgroups) * sizeof(s32)), MEMPOOL_STAGE);
	noneighbours = mempAlloc(ALIGN16(sizeof(s32)), MEMPOOL_STAGE);

	if (groups && grlists && noneighbours) {
		*noneighbours = -1;
		grpos = 0;

		for (j = 0; j < numgroups; j++) {
			groups[j].neighbours = noneighbours;
			groups[j].waypoints = &grlists[grpos];
			groups[j].step = -1;

			for (i = 0; i < n; i++) {
				if (group[i] == j) {
					grlists[grpos++] = i;
				}
			}

			grlists[grpos++] = -1;
		}

		groups[numgroups].neighbours = NULL;
		groups[numgroups].waypoints = NULL;
		groups[numgroups].step = 0;

		for (i = 0; i < n; i++) {
			waypoints[i].groupnum = group[i];
			waypoints[i].step = -1;
		}

		g_StageSetup.waygroups = groups;

		sysLogPrintf(LOG_NOTE, "alarm: stage 0x%02x: %d waypoint link(s) climb where no lift stands (%d lifts); cut, %d links left, %d groups (largest %d), in %d ms",
				g_Vars.stagenum, numcut, numlifts, numlinks, numgroups, largest,
				(s32)((sysGetMicroseconds() - started) / 1000));
	}

	free(pos);
	free(floory);
	free(rooms);
	free(group);
	free(queue);
#endif
}

void modAlarmReset(void)
{
	s32 i;

	for (i = 0; i < MODALARM_MAXGUARDS; i++) {
		g_ModAlarmGuards[i].chr = NULL;
		g_ModAlarmGuards[i].chrnum = -1;
		g_ModAlarmGuards[i].spawned60 = 0;
	}

	for (i = 0; i < MAX_MPCHRS; i++) {
		g_ModAlarmGuardKills[i] = 0;
		g_ModAlarmGuardDeaths[i] = 0;
	}

	g_ModAlarmNumHeads = 0;
	g_ModAlarmNumWaypoints = -1; // counted on first use: the setup is not loaded yet

	// A few seconds' grace after the stage starts, or after its intro ends -
	// the countdown does not run through a cutscene - so the first guard is
	// not already there when the player gets control.
	g_ModAlarmCountdown60 = TICKS(300);
}

/**
 * Chr slots to reserve for the level that is loading. Decided once, here, and
 * read back by modAlarmGetReserve() so the two counts in setupLoadFiles()
 * agree; the count changing mid-stage does not grow a pool that has already
 * been sized, which is why modAlarmTick() caps at the smaller of the two.
 */
s32 modAlarmSetReserve(void)
{
	g_ModAlarmReserve = modIsGuardsAlertedOn() ? modGetAlertedGuards() : 0;

	return g_ModAlarmReserve;
}

s32 modAlarmGetReserve(void)
{
	return g_ModAlarmReserve;
}

/**
 * Whether a list entry still names the chr it was made for. A chr slot is
 * reused with a new number once it is freed, and the prop and model go first.
 */
static bool modAlarmGuardIsValid(struct modalarmguard *guard)
{
	return guard->chr
		&& guard->chr->chrnum == guard->chrnum
		&& guard->chr->prop
		&& guard->chr->model;
}

static bool modAlarmGuardIsDead(struct modalarmguard *guard)
{
	return guard->chr->actiontype == ACT_DEAD;
}

/**
 * Send the oldest corpse on its way, so its slot comes back.
 *
 * The kept-bodies claim goes first, as modBodyRetire() does it: chrTickDead()
 * holds a claimed body at full opacity, and nothing fades while it stands.
 */
static bool modAlarmRetireOldest(void)
{
	struct modalarmguard *oldest = NULL;
	s32 i;

	for (i = 0; i < MODALARM_MAXGUARDS; i++) {
		struct modalarmguard *guard = &g_ModAlarmGuards[i];

		if (modAlarmGuardIsValid(guard) && modAlarmGuardIsDead(guard)
				&& !guard->chr->act_dead.fadenow
				&& (oldest == NULL || guard->spawned60 < oldest->spawned60)) {
			oldest = guard;
		}
	}

	if (oldest == NULL) {
		return false;
	}

	oldest->chr->keptbody60 = -1;
	chrFadeCorpse(oldest->chr);

	return true;
}

/**
 * The head a body wears whatever the stage's active heads are: a Maian
 * soldier's is the Maian's own (tester 2026-09-28: a Randomizer run's Maian
 * soldiers came in wearing the stage's human faces, since bodyChooseHead()
 * deals from the male list), and a GoldenEye body's is one of GoldenEye's.
 * -1 for a body that takes the stage's.
 */
static s32 modAlarmOwnHead(s32 bodynum)
{
	if (bodynum == BODY_MAIAN_SOLDIER) {
		return MOD_HEADNUM(HEAD_MAIAN_S);
	}

#ifndef PLATFORM_N64
	// A GoldenEye body - a Randomizer run's guards on a GoldenEye map - wears
	// one of GoldenEye's own faces of its sex, never a Perfect Dark one
	return gebeanRandomHeadForBody(bodynum);
#else
	return -1;
#endif
}

/**
 * A head for the next guard, and its modeldef where this has to hold one.
 *
 * Solo hands back NULL for the modeldef and lets body0f02ce8c() share the
 * stage's copy, which is what every stock guard with a random head does. A
 * match takes its own copies for the reason at the top of the file, one per
 * head number, and once the table is full the new guard wears one already
 * loaded rather than loading another.
 */
static s32 modAlarmChooseHead(s32 bodynum, struct modeldef **headmodeldef)
{
	s32 headnum = modAlarmOwnHead(bodynum);
	s32 i;

	if (headnum < 0) {
		headnum = bodyChooseHead(bodynum);
	}

	*headmodeldef = NULL;

	if (!g_Vars.normmplayerisrunning) {
		return headnum;
	}

	for (i = 0; i < g_ModAlarmNumHeads; i++) {
		if (g_ModAlarmHeads[i].headnum == headnum) {
			*headmodeldef = g_ModAlarmHeads[i].modeldef;
			return headnum;
		}
	}

	if (g_ModAlarmNumHeads >= MODALARM_MAXHEADS || mempGetStageFreeTotal() < MODALARM_MEMFLOOR) {
		if (g_ModAlarmNumHeads == 0) {
			return -1;
		}

		i = rngRandom() % g_ModAlarmNumHeads;
		*headmodeldef = g_ModAlarmHeads[i].modeldef;
		return g_ModAlarmHeads[i].headnum;
	}

	*headmodeldef = modeldefLoadToNew(g_HeadsAndBodies[headnum].filenum);

	if (*headmodeldef == NULL) {
		return -1;
	}

	bodyCalculateHeadOffset(*headmodeldef, headnum, bodynum);

	g_ModAlarmHeads[g_ModAlarmNumHeads].headnum = headnum;
	g_ModAlarmHeads[g_ModAlarmNumHeads].modeldef = *headmodeldef;
	g_ModAlarmNumHeads++;

	return headnum;
}

/**
 * The player nearest a point, and how far. Dead players do not count: a guard
 * sent after a corpse in a match stands over it until the respawn.
 */
static s32 modAlarmNearestPlayer(struct coord *pos, f32 *dist)
{
	s32 nearest = -1;
	f32 best = 0;
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *player = g_Vars.players[i];
		f32 xdiff;
		f32 ydiff;
		f32 zdiff;
		f32 sqdist;

		if (player == NULL || player->prop == NULL || player->isdead) {
			continue;
		}

		xdiff = player->prop->pos.x - pos->x;
		ydiff = player->prop->pos.y - pos->y;
		zdiff = player->prop->pos.z - pos->z;
		sqdist = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;

		if (nearest < 0 || sqdist < best) {
			nearest = i;
			best = sqdist;
		}
	}

	if (nearest >= 0) {
		*dist = sqrtf(best);
	}

	return nearest;
}

/**
 * Put one guard into the world at pos, or fail quietly.
 *
 * This is chrSpawnAtCoord() with the head modeldef passed through and without
 * the corpse reaper, which modAlarmTick() does for itself with better
 * knowledge of whose corpses they are.
 */
static struct chrdata *modAlarmSpawn(s32 bodynum, struct coord *pos, RoomNum *rooms, f32 angle)
{
	struct modeldef *headmodeldef;
	struct coord pos2;
	RoomNum rooms2[8];
	struct model *model;
	struct prop *prop;
	struct chrdata *chr;
	s32 headnum;

	pos2 = *pos;
	roomsCopy(rooms, rooms2);

	if (!chrAdjustPosForSpawn(20, &pos2, rooms2, angle, false, false, false)) {
		return NULL;
	}

	headnum = modAlarmChooseHead(bodynum, &headmodeldef);

	if (headnum < 0) {
		return NULL;
	}

	model = body0f02d338(bodynum, headnum, NULL, headmodeldef, false, true);

	if (model == NULL) {
		return NULL;
	}

	prop = chrAllocate(model, &pos2, rooms2, angle, ailistFindById(GAILIST_UNALERTED));

	if (prop == NULL) {
		modelmgrFreeModel(model);
		return NULL;
	}

	propActivateThisFrame(prop);
	propEnable(prop);

	chr = prop->chr;
	chr->headnum = headnum;
	chr->bodynum = bodynum;
	chr->race = bodyGetRace(bodynum);
	chr->flags = 0;
	chr->flags2 = 0;
	chr->hidden2 |= CHRH2FLAG_SPAWNED;

	// The voice the setup file's spawn gives a chr (bodyAllocateChr()):
	// one of the three men's, or the woman's for a female body. The man's is
	// taken from the chr number rather than the rng, so a seeded run or a
	// ghost deals exactly what it dealt before this line was here.
	// chrInit() leaves every chr on the first man's, so a woman dealt here
	// shouted his lines ("Why me?", "She got me") over her own pain sounds,
	// which read the body (F3 20260929-200003).
	chr->voicebox = (u32)chr->chrnum % 3;

	if (!g_HeadsAndBodies[bodynum].ismale) {
		chr->voicebox = VOICEBOX_FEMALE;
	}

	// What the body brings with it besides the model: a robot's fireslots and
	// the sizes the special bodies stand at. The setup file's spawn does this
	// and so must this one - the bodies here are the stage's own until a
	// Randomizer run picks one out of the whole game, and BODY_CHICROB without
	// its fireslots is a chr the beam render dereferences. See body.c.
	if (!bodyInitSpecialChr(chr, bodynum)) {
#ifndef PLATFORM_N64
		sysLogPrintf(LOG_WARNING, "alarm: no stage pool left for body %d's fireslots; it will not fire",
				bodynum);
#endif
	}

	return chr;
}

/**
 * The team the stage's own guards are on, for a responder in a mission.
 *
 * Not every stage keeps its guards on TEAM_ENEMY: Chicago's and the Villa's
 * are on TEAM_20, and their own responders (func041f_alarm_responder) join
 * TEAM_20 too. Teams are bits and two chrs are enemies when they share none,
 * so a responder put on TEAM_ENEMY there was every guard's enemy, and with
 * CHRFLAG0_AIVSAI it went for them (F3 20261001-021542). The answer is the
 * team most of the stage's hostile chrs are on - those sharing no bit with
 * the player and not TEAM_NONCOMBAT - or TEAM_ENEMY when there are none.
 */
static u8 modAlarmMissionTeam(void)
{
	u16 counts[256];
	u8 playerteam = 0;
	s32 best = TEAM_ENEMY;
	s32 i;

	memset(counts, 0, sizeof(counts));

	if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
		playerteam = g_Vars.bond->prop->chr->team;
	}

	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || chr->team == 0 || chr->team == TEAM_NONCOMBAT
				|| (chr->team & playerteam) != 0
				|| (chr->hidden2 & CHRH2FLAG_SPAWNED)
				|| (chr->prop && chr->prop->type == PROPTYPE_PLAYER)) {
			continue;
		}

		if (counts[chr->team] < 0xffff) {
			counts[chr->team]++;
		}
	}

	for (i = 1; i < 256; i++) {
		if (counts[i] > counts[best]) {
			best = i;
		}
	}

	return best;
}

/**
 * What the Villa's func0408_alarm_responder does to a responder, in C.
 *
 * The team is the stage's guards' own in a mission (modAlarmMissionTeam()). In a
 * match the players' teams are bits, one per Combat Simulator team, and
 * TEAM_ENEMY is team two's bit - a guard on it would be a teammate of that
 * team's players for friendly fire, and rebuildTeams() would list it with
 * them - so there the guard has no team bit at all and is nobody's friend.
 *
 * The target is the nearest player. Every guard's target defaults to the
 * player its p1p2 names, which chrInit() sets to the first; in a mission
 * that is the only one, and in a match it is the one who happened to be
 * player one, so the one who is closest is a fairer choice and the AI's own
 * chr_toggle_p1p2 moves it along from there.
 */
static s32 modAlarmArm(struct chrdata *chr, s32 playernum)
{
	s32 gun = modAlarmChooseGun();

	if (g_Vars.normmplayerisrunning) {
		chr->team = TEAM_00;
		chr->accuracyrating = 15;
	} else {
		chr->team = modAlarmMissionTeam();
		chr->accuracyrating = lvGetDifficulty() < DIFF_SA ? 20 : 10;
	}

	if (playernum >= 0) {
		chr->p1p2 = playernum;
	}

	chr->target = -1;
	chr->alertness = 90;

	// CHRFLAG0_AIVSAI is stock's "fight other AI": the guard lists check
	// it and run if_enemy_distance_lt_and_los, which is aiDetectEnemy()
	// scanning the team lists for the nearest chr on another team with a
	// line of sight. In a match the guard has no team bit and every
	// simulant has one, so every simulant is an enemy; in a mission the
	// guard is on the stage's guards' team and they are its friends,
	// while the Institute's staff and soldiers are not.
	chr->flags |= CHRFLAG0_CAN_HEAR_ALARMS | CHRFLAG0_SKIPSAFETYCHECKS | CHRFLAG0_AIVSAI;
	chr->flags2 |= CHRFLAG1_NOIDLEANIMS;
	chr->chrflags |= CHRCFLAG_CANCHANGEACTDURINGARGH;

	chrGiveWeaponWithAutoModel(chr, gun, 0);

	if (modIsAkimboForGuards() && modCanAkimbo(gun)) {
		s32 leftgun = gun;

		// Random: a second roll for the left hand, so the pair is two
		// different guns when the roll allows it, the way Start Armed's
		// Random does for a player. The AI fires each hand's own prop
		// (chrAttackStand() and its kin) and never asks that they match.
		if (modGetGuardWeapons() == MODALARM_WEAPONS_RANDOM) {
			s32 tries;

			for (tries = 0; tries < 8; tries++) {
				s32 roll = modAlarmChooseGun();

				if (roll != gun && modCanAkimbo(roll)) {
					leftgun = roll;
					break;
				}
			}
		}

		// A second one for the left hand, the way a mission gives a guard
		// two magnums: try_equip_weapon with the left-handed flag
		chrGiveWeaponWithAutoModel(chr, leftgun, OBJFLAG_WEAPON_LEFTHANDED);
	}

	rebuildTeams();

	return gun;
}

/**
 * A waypoint inside the rooms a Randomizer run has the player sealed into, far
 * enough from them to not be on top of them, or NULL - or one in the rooms
 * around them, or anywhere with a route to the player (where).
 *
 * The whole list is walked and one of the waypoints that qualify is taken at
 * random, rather than a dozen random draws being tried the way the spawn does
 * it: the zone is three or four rooms of a level's hundred and twelve draws
 * out of three hundred waypoints will usually not land in it once. Taking the
 * first match from a random start would do that much, but a zone with one
 * qualifying waypoint in it then deals every guard of the room onto that one
 * pad - twelve men filing out of the same corner, which is what the first
 * version of this did. See modrun.c on why a run's guards have to be dealt
 * into the room rather than walked to it.
 */
#define MODALARM_FIND_ZONE  0 // in the sealed rooms
#define MODALARM_FIND_RING  1 // in the rooms around them, on a group with a route in
#define MODALARM_FIND_REACH 2 // anywhere by the ordinary distances, on such a group

static struct waypoint *modAlarmFindZoneWaypoint(s32 where)
{
	struct waypoint *chosen = NULL;
	s32 count = 0;
	s32 pass;
	s32 i;

	// Inside the zone, a waypoint on the player's piece of the graph first:
	// one on another piece is a guard that stands where it appeared until
	// the player goes to it (Complex's pad graph is twenty-odd pieces)
	for (pass = where == MODALARM_FIND_ZONE ? 0 : 1; pass < 2 && chosen == NULL; pass++)
	for (i = 0; i < g_ModAlarmNumWaypoints; i++) {
		struct waypoint *waypoint = &g_StageSetup.waypoints[i];
		struct pad pad;
		f32 dist;

		padUnpack(waypoint->padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (where == MODALARM_FIND_RING ? !modRunGuardRingOk(pad.room, waypoint->groupnum)
				: where == MODALARM_FIND_ZONE ? !modRunGuardRoomOk(pad.room)
				: !modRunGuardGroupReaches(waypoint->groupnum)) {
			continue;
		}

		if (pass == 0 && !modRunGuardGroupReaches(waypoint->groupnum)) {
			continue;
		}

		if (modAlarmNearestPlayer(&pad.pos, &dist) < 0) {
			return NULL; // nobody alive to come for
		}

		if (where == MODALARM_FIND_REACH ? dist < MODALARM_MINDIST || dist > MODALARM_MAXDIST
				: dist < modRunGuardMinDist()) {
			continue;
		}

		count++;

		if (rngRandom() % count == 0) {
			chosen = waypoint;
		}
	}

	return chosen;
}

/**
 * Try to bring one guard in: a few random waypoints, the first that is the
 * right distance from everyone and passes the spawn test.
 *
 * While a run has the player sealed into a room the waypoints of that room
 * come first, since a guard placed anywhere else may have no way in at all.
 * The moment the zone has nowhere left to put one, this is the rule it always
 * was.
 */
static bool modAlarmSpawnOne(s32 bodynum)
{
	// 0 the rooms around a sealed room, to walk in from (Guards Walk In);
	// 1 the sealed rooms themselves; 2 anywhere, by the ordinary rule
	s32 phase = modRunGuardsWalkIn() ? 0 : modRunGuardsWantZone() ? 1 : 2;
	s32 attempt;
	s32 toonear = 0;
	s32 toofar = 0;
	s32 refused = 0;
	s32 unreachable = 0;

	for (attempt = 0; attempt < MODALARM_TRIES; attempt++) {
		struct waypoint *waypoint;
		struct pad pad;
		RoomNum rooms[2];
		f32 dist;
		f32 angle;
		s32 playernum;
		struct chrdata *chr;
		s32 gun;
		s32 i;

		if (phase < 2) {
			waypoint = modAlarmFindZoneWaypoint(phase == 0 ? MODALARM_FIND_RING : MODALARM_FIND_ZONE);

			if (waypoint == NULL) {
				// Nowhere around the room, or nowhere in it far enough from
				// the player. That will not change while they stand there, so
				// stop asking and spend the attempts left on the next way.
				phase = phase == 0 && modRunGuardsWantZone() ? 1 : 2;
				continue;
			}
		} else if (modRunIsOn()) {
			// Nor, during a run, anywhere with no route to where the player
			// landed: a guard there stands where it appeared (Bunker's pad
			// graph is fifteen pieces), so the waypoints are walked for one
			// that has one rather than drawn blind
			waypoint = modAlarmFindZoneWaypoint(MODALARM_FIND_REACH);

			if (waypoint == NULL) {
				unreachable++;
				break;
			}
		} else {
			waypoint = &g_StageSetup.waypoints[rngRandom() % g_ModAlarmNumWaypoints];
		}

		padUnpack(waypoint->padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

		playernum = modAlarmNearestPlayer(&pad.pos, &dist);

		if (playernum < 0) {
#ifndef PLATFORM_N64
			if (g_ChrSpawnTrace) {
				sysLogPrintf(LOG_NOTE, "alarm: nobody alive to come for (player 0: %s, dead %d, cutscene %d)",
						g_Vars.players[0] && g_Vars.players[0]->prop ? "has prop" : "no prop",
						g_Vars.players[0] ? g_Vars.players[0]->isdead : -1, g_Vars.in_cutscene);
			}
#endif
			return false; // nobody alive to come for
		}

		if (dist < (phase < 2 ? modRunGuardMinDist() : (f32)MODALARM_MINDIST)) {
			toonear++;
			continue;
		}

		// Inside the seal there is no such thing as too far: the zone is the
		// whole of where the fight can happen.
		if (phase == 2 && dist > MODALARM_MAXDIST) {
			toofar++;
			continue;
		}

		rooms[0] = pad.room;
		rooms[1] = -1;
		angle = (rngRandom() % 360) * M_BADTAU / 360.0f;

		chr = modAlarmSpawn(bodynum, &pad.pos, rooms, angle);

		if (chr == NULL) {
			refused++;

			// The rooms the seal holds are often all in view at once - a
			// GoldenEye arena's open ground, a Villa landing that looks down
			// its own corridor - and then every waypoint in them is refused
			// as a guard popping in in front of the player, attempt after
			// attempt, for as long as the player stands looking: nobody came
			// at all. Half the tries go to the rest of the map instead, by the
			// ordinary distance rule, and the guard walks in.
			if (phase == 0 && refused >= MODALARM_TRIES / 3) {
				phase = modRunGuardsWantZone() ? 1 : 2;
			} else if (phase == 1 && refused >= MODALARM_TRIES * 2 / 3) {
				phase = 2;
			}

			continue;
		}

		gun = modAlarmArm(chr, playernum);

		for (i = 0; i < MODALARM_MAXGUARDS; i++) {
			if (!modAlarmGuardIsValid(&g_ModAlarmGuards[i])) {
				g_ModAlarmGuards[i].chr = chr;
				g_ModAlarmGuards[i].chrnum = chr->chrnum;
				g_ModAlarmGuards[i].spawned60 = g_Vars.lvframe60;
				break;
			}
		}

#ifndef PLATFORM_N64
		if (g_ChrSpawnTrace) {
			sysLogPrintf(LOG_NOTE, "alarm: guard body %d head %d weapon %d left %d at pad %d in room %d%s, %.0fcm from player %d, %d free chr slots",
					bodynum, chr->headnum, gun,
					chr->weapons_held[HAND_LEFT] && chr->weapons_held[HAND_LEFT]->weapon ? chr->weapons_held[HAND_LEFT]->weapon->weaponnum : -1,
					waypoint->padnum, pad.room, phase == 0 ? " (walking in)" : phase == 1 ? " (sealed zone)" : "",
					dist, playernum, chrsGetNumFree());
		}
#endif

		return true;
	}

#ifndef PLATFORM_N64
	if (g_ChrSpawnTrace) {
		sysLogPrintf(LOG_NOTE, "alarm: no place for a guard this time: of %d waypoints tried, %d too near, %d too far, %d in view or blocked, %d with no route to the player",
				MODALARM_TRIES, toonear, toofar, refused, unreachable);
	}
#endif

	return false;
}

void modAlarmRecordGuardKill(s32 aplayernum)
{
	if (aplayernum >= 0 && aplayernum < MAX_MPCHRS) {
		g_ModAlarmGuardKills[aplayernum]++;
	}
}

void modAlarmRecordGuardDeath(s32 vplayernum)
{
	if (vplayernum >= 0 && vplayernum < MAX_MPCHRS) {
		g_ModAlarmGuardDeaths[vplayernum]++;
	}
}

s32 modAlarmGetGuardKills(s32 mpindex)
{
	if (mpindex < 0 || mpindex >= MAX_MPCHRS) {
		return 0;
	}

	return g_ModAlarmGuardKills[mpindex];
}

s32 modAlarmGetGuardDeaths(s32 mpindex)
{
	if (mpindex < 0 || mpindex >= MAX_MPCHRS) {
		return 0;
	}

	return g_ModAlarmGuardDeaths[mpindex];
}

/**
 * Whether the results have a guard tally to show: the setting was on for
 * the match, or something was counted before it was turned off.
 */
bool modAlarmHasMatchStats(void)
{
	s32 i;

	if (modIsGuardsAlertedOn()) {
		return true;
	}

	for (i = 0; i < MAX_MPCHRS; i++) {
		if (g_ModAlarmGuardKills[i] || g_ModAlarmGuardDeaths[i]) {
			return true;
		}
	}

	return false;
}

/**
 * Whether a chr is one of the reinforcements this spawned and still has.
 */
bool modAlarmIsGuard(struct chrdata *chr)
{
	s32 i;

	if (chr == NULL) {
		return false;
	}

	for (i = 0; i < MODALARM_MAXGUARDS; i++) {
		if (g_ModAlarmGuards[i].chr == chr && modAlarmGuardIsValid(&g_ModAlarmGuards[i])) {
			return true;
		}
	}

	return false;
}

/**
 * Whether this chr may open a door the stage marks as one the AI cannot use.
 *
 * A graph built from pads links through any door, since the doors are not
 * props yet when it is built - and a route through one the guards may not
 * open is a squad pressed against it for the rest of the room. GoldenEye
 * Arenas' Archives keeps its secret wall between rooms 17 and 62 shut to the
 * AI, and a run sealed in room 16 dealt every guard's route through it
 * (F3 20260928-170416). The player can open it, so the run's guards may too.
 *
 * A map's own waypoints are no better: Area 51 Escape links room 231 to 223
 * through a door shut to the AI, and a run sealed in room 229 had five of its
 * six walk-in guards dealt into 231 pressed against that door for the whole
 * room (F3 20260929-195347, -195516). The walk-in ring and the zone deal
 * take a guard's reach from the waygroups, which know nothing of doors, so
 * during a run a run's guards open any door, whoever laid the waypoints. A
 * mission's own Alerted Guards keep the stock rule outside a pad-built graph.
 */
bool modAlarmGuardOpensAnyDoor(struct chrdata *chr)
{
	return (g_ModAlarmPadGraph || modRunIsPlaying()) && modAlarmIsGuard(chr);
}

/**
 * The nearest living guard a simulant can see, within maxdist, or NULL.
 *
 * A simulant's own target picker only knows the match's participants - its
 * sight and distance tables are indexed by match slot - so it asks here for
 * the guards. The line of sight test is the expensive part, so only the
 * three nearest are tested; a guard further away than three others that
 * are all behind walls is not the one to shoot at anyway.
 */
struct chrdata *modAlarmFindGuardForBot(struct chrdata *botchr, f32 maxdist)
{
	struct chrdata *nearest[3] = { NULL, NULL, NULL };
	f32 nearestdist[3] = { 0, 0, 0 };
	s32 i;
	s32 j;

	for (i = 0; i < MODALARM_MAXGUARDS; i++) {
		struct modalarmguard *guard = &g_ModAlarmGuards[i];
		f32 dist;

		if (!modAlarmGuardIsValid(guard) || modAlarmGuardIsDead(guard)
				|| guard->chr->actiontype == ACT_DIE) {
			continue;
		}

		dist = chrGetDistanceToCoord(botchr, &guard->chr->prop->pos);

		if (dist > maxdist) {
			continue;
		}

		for (j = 0; j < ARRAYCOUNT(nearest); j++) {
			if (nearest[j] == NULL || dist < nearestdist[j]) {
				s32 k;

				for (k = ARRAYCOUNT(nearest) - 1; k > j; k--) {
					nearest[k] = nearest[k - 1];
					nearestdist[k] = nearestdist[k - 1];
				}

				nearest[j] = guard->chr;
				nearestdist[j] = dist;
				break;
			}
		}
	}

	for (j = 0; j < ARRAYCOUNT(nearest); j++) {
		RoomNum room = -1;

		if (nearest[j] && chrHasLosToChr(botchr, nearest[j], &room)) {
			return nearest[j];
		}
	}

	return NULL;
}

/**
 * Once a frame from alarmTick(), after the alarm itself has been kept on.
 */
void modAlarmTick(void)
{
	s32 interval60 = modAlarmGetInterval60();
	s32 maxalive = modGetAlertedGuards();
	s32 alive = 0;
	s32 i;

	// Off, or nothing should happen: the pause menu, and a cutscene - a
	// guard arriving mid-briefing has nobody to fight and spoils the shot.
	// The Institute counts as paused while a menu is up over it, which is
	// most of the time it is on screen and all of the time it is a backdrop.
	if (!modIsGuardsAlertedOn() || lvIsPaused() || g_Vars.in_cutscene) {
		return;
	}

	if (g_ModAlarmReserve > 0 && maxalive > g_ModAlarmReserve) {
		maxalive = g_ModAlarmReserve;
	}

	if (g_ModAlarmNumWaypoints < 0) {
		g_ModAlarmNumWaypoints = modAlarmCountWaypoints();
	}

	if (g_ModAlarmNumWaypoints == 0) {
		return;
	}

	// Count what is standing, and forget guards whose chr has gone
	for (i = 0; i < MODALARM_MAXGUARDS; i++) {
		struct modalarmguard *guard = &g_ModAlarmGuards[i];

		if (!modAlarmGuardIsValid(guard)) {
			guard->chr = NULL;
			guard->chrnum = -1;
		} else if (!modAlarmGuardIsDead(guard)) {
			// One that walked off the level stands at the collision system's
			// floor of last resort for good, holding its place in the count.
			// Stock deletes a guard that lands on a floor that kills; so does
			// this, and the next one comes.
			if (guard->chr->manground < MODALARM_VOID) {
				guard->chr->hidden |= CHRHFLAG_DELETING;
				continue;
			}

			alive++;
		}
	}

#ifndef PLATFORM_N64
	// --chr-trace: every ten seconds, where the guards are and what they
	// are doing, which is the only way a headless run can show that a wave
	// is coming for the player rather than standing where it appeared.
	if (g_ChrSpawnTrace && alive > 0) {
		static s32 tracecountdown60 = 0;

		tracecountdown60 -= g_Vars.lvupdate60;

		if (tracecountdown60 <= 0) {
			s32 near = 0;
			s32 moving = 0;
			s32 attacking = 0;
			f32 nearest = -1;

			tracecountdown60 = TICKS(600);

			for (i = 0; i < MODALARM_MAXGUARDS; i++) {
				struct modalarmguard *guard = &g_ModAlarmGuards[i];
				f32 dist;

				if (modAlarmGuardIsValid(guard) && !modAlarmGuardIsDead(guard)
						&& modAlarmNearestPlayer(&guard->chr->prop->pos, &dist) >= 0) {
					if (nearest < 0 || dist < nearest) {
						nearest = dist;
					}

					if (dist < 1000) {
						near++;
					}

					if (guard->chr->actiontype == ACT_GOPOS || guard->chr->actiontype == ACT_PATROL) {
						moving++;
					} else if (guard->chr->actiontype == ACT_ATTACK
							|| guard->chr->actiontype == ACT_ATTACKWALK
							|| guard->chr->actiontype == ACT_ATTACKROLL
							|| guard->chr->actiontype == ACT_ATTACKAMOUNT) {
						attacking++;
					}
				}
			}

			{
				s32 guardsonsims = 0;
				s32 simsonguards = 0;
				s32 numsims = 0;

				for (i = 0; i < MODALARM_MAXGUARDS; i++) {
					struct modalarmguard *guard = &g_ModAlarmGuards[i];

					if (modAlarmGuardIsValid(guard) && !modAlarmGuardIsDead(guard)
							&& guard->chr->target != -1
							&& g_Vars.props[guard->chr->target].type == PROPTYPE_CHR
							&& g_Vars.props[guard->chr->target].chr
							&& g_Vars.props[guard->chr->target].chr->aibot) {
						guardsonsims++;
					}
				}

				for (i = 0; i < chrsGetNumSlots(); i++) {
					struct chrdata *chr = &g_ChrSlots[i];

					if (chr->chrnum >= 0 && chr->aibot && chr->prop) {
						numsims++;

						if (chr->target != -1 && modAlarmIsGuard(g_Vars.props[chr->target].chr)) {
							simsonguards++;
						}
					}
				}

				{
					s32 kills = 0;
					s32 deaths = 0;

					for (i = 0; i < MAX_MPCHRS; i++) {
						kills += g_ModAlarmGuardKills[i];
						deaths += g_ModAlarmGuardDeaths[i];
					}

					sysLogPrintf(LOG_NOTE, "alarm: %d guards up: nearest %.0fcm, %d within 10m, %d moving, %d attacking; %d guards on sims, %d of %d sims on guards; %d guards killed, %d killed by guards",
							alive, nearest, near, moving, attacking, guardsonsims, simsonguards, numsims, kills, deaths);
				}
			}
		}
	}
#endif

	if (alive >= maxalive) {
		g_ModAlarmCountdown60 = interval60;
		return;
	}

	g_ModAlarmCountdown60 -= g_Vars.lvupdate60;

	if (g_ModAlarmCountdown60 > 0) {
		return;
	}

	// The next one is due. If the slots are short, retire a corpse first and
	// try again shortly rather than spending the attempt on a spawn that
	// cannot succeed.
	if (chrsGetNumFree() < 3) {
		modAlarmRetireOldest();
		g_ModAlarmCountdown60 = modAlarmGetRetry60();
		return;
	}

	if (modAlarmSpawnOne(modAlarmChooseBody())) {
		g_ModAlarmCountdown60 = interval60;
	} else {
		// No waypoint suited this frame; look again shortly
		g_ModAlarmCountdown60 = modAlarmGetRetry60();
	}
}

#include <ultra64.h>
#include "constants.h"
#include "math.h"
#include "game/body.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/lv.h"
#include "game/modalarm.h"
#include "game/modeldef.h"
#include "game/modelmgr.h"
#include "game/modoptions.h"
#include "game/pad.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/ailist.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "system.h"
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
 * A weapon for the next guard.
 *
 * In a Combat Simulator match it is one of the match's own six slots, so the
 * guards fight with what the arena is stocked with and drop what a player
 * can use. Anywhere else, or when the match holds nothing a guard can carry,
 * the mission side arms.
 */
static s32 modAlarmChooseGun(void)
{
	if (g_Vars.normmplayerisrunning) {
		u8 guns[NUM_MPWEAPONSLOTS];
		s32 numguns = 0;
		s32 i;

		for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
			s32 slot = g_MpSetup.weapons[i];

			if (slot >= 0 && slot < NUM_MPWEAPONS && modAlarmIsGun(g_MpWeapons[slot].weaponnum)) {
				guns[numguns++] = g_MpWeapons[slot].weaponnum;
			}
		}

		if (numguns > 0) {
			return guns[rngRandom() % numguns];
		}
	}

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

void modAlarmReset(void)
{
	s32 i;

	for (i = 0; i < MODALARM_MAXGUARDS; i++) {
		g_ModAlarmGuards[i].chr = NULL;
		g_ModAlarmGuards[i].chrnum = -1;
		g_ModAlarmGuards[i].spawned60 = 0;
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
	s32 headnum = bodyChooseHead(bodynum);
	s32 i;

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

	return chr;
}

/**
 * What the Villa's func0408_alarm_responder does to a responder, in C.
 *
 * The team is TEAM_ENEMY in a mission, which is what every guard is. In a
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
static void modAlarmArm(struct chrdata *chr, s32 playernum)
{
	s32 gun = modAlarmChooseGun();

	if (g_Vars.normmplayerisrunning) {
		chr->team = TEAM_00;
		chr->accuracyrating = 15;
	} else {
		chr->team = TEAM_ENEMY;
		chr->accuracyrating = lvGetDifficulty() < DIFF_SA ? 20 : 10;
	}

	if (playernum >= 0) {
		chr->p1p2 = playernum;
	}

	chr->target = -1;
	chr->alertness = 90;
	chr->flags |= CHRFLAG0_CAN_HEAR_ALARMS | CHRFLAG0_SKIPSAFETYCHECKS;
	chr->flags2 |= CHRFLAG1_NOIDLEANIMS;
	chr->chrflags |= CHRCFLAG_CANCHANGEACTDURINGARGH;

	chrGiveWeaponWithAutoModel(chr, gun, 0);
	rebuildTeams();
}

/**
 * Try to bring one guard in: a few random waypoints, the first that is the
 * right distance from everyone and passes the spawn test.
 */
static bool modAlarmSpawnOne(s32 bodynum)
{
	s32 attempt;
	s32 toonear = 0;
	s32 toofar = 0;
	s32 refused = 0;

	for (attempt = 0; attempt < MODALARM_TRIES; attempt++) {
		struct waypoint *waypoint = &g_StageSetup.waypoints[rngRandom() % g_ModAlarmNumWaypoints];
		struct pad pad;
		RoomNum rooms[2];
		f32 dist;
		f32 angle;
		s32 playernum;
		struct chrdata *chr;
		s32 i;

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

		if (dist < MODALARM_MINDIST) {
			toonear++;
			continue;
		}

		if (dist > MODALARM_MAXDIST) {
			toofar++;
			continue;
		}

		rooms[0] = pad.room;
		rooms[1] = -1;
		angle = (rngRandom() % 360) * M_BADTAU / 360.0f;

		chr = modAlarmSpawn(bodynum, &pad.pos, rooms, angle);

		if (chr == NULL) {
			refused++;
			continue;
		}

		modAlarmArm(chr, playernum);

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
			sysLogPrintf(LOG_NOTE, "alarm: guard body %d head %d at pad %d, %.0fcm from player %d, %d free chr slots",
					bodynum, chr->headnum, waypoint->padnum, dist, playernum, chrsGetNumFree());
		}
#endif

		return true;
	}

#ifndef PLATFORM_N64
	if (g_ChrSpawnTrace) {
		sysLogPrintf(LOG_NOTE, "alarm: no place for a guard this time: of %d waypoints tried, %d too near, %d too far, %d in view or blocked",
				MODALARM_TRIES, toonear, toofar, refused);
	}
#endif

	return false;
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

			sysLogPrintf(LOG_NOTE, "alarm: %d guards up: nearest %.0fcm, %d within 10m, %d moving, %d attacking",
					alive, nearest, near, moving, attacking);
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

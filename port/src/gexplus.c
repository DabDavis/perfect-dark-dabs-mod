/**
 * GE-X Plus's scenarios: GoldenEye's multiplayer modes over Perfect Dark's.
 *
 * GoldenEye's are chosen from the Combat Simulator's scenario list while in
 * GE-X Plus (scenarios.c) and are not a scenario number of their own - the
 * setup's scenario is saved, and its numbers are Perfect Dark's - but a choice
 * of one of Perfect Dark's scenarios and options:
 *
 * - Normal: Combat
 * - You Only Live Twice: Combat, and a chr that has died twice stays down
 *   (player.c and chraction.c ask gexPlusLivesSpent()); the match ends when one
 *   chr has a life left (lv.c asks gexPlusMatchOver())
 * - The Living Daylights (flag tag): Hold the Briefcase, which is the same game
 * - License to Kill: Combat with one-hit kills
 * - The Man with the Golden Gun: Combat with one Golden Gun in the arena
 *   (gexPlusTick(), from scenarioTick()). It is wherever it is - on the floor,
 *   or carried by a player or a simulant, who drops it on dying as any gun -
 *   and put down at a spawn pad when it is nowhere, as Hold the Briefcase does
 *   its briefcase. A second one, from a weapon set's slot, is taken away. The
 *   gun kills with one shot on GoldenEye's own numbers (gegunstats.h).
 */
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "gexplus.h"
#include <string.h>
#include <stdio.h>
#include "modborrow.h"
#include "gebean.h"
#include "game/mplayer/mplayer.h"
#include "game/game_0b0fd0.h"
#include "game/setuputils.h"
#include "game/botinv.h"
#include "game/chraction.h"
#include "game/inv.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/propobj.h"
#include "game/setup.h"
#include "lib/rng.h"

static s32 g_GexPlusScenario = GEXPLUS_NORMAL;

static const char *const g_GexPlusScenarioNames[GEXPLUS_NUMSCENARIOS] = {
	"Normal",
	"You Only Live Twice",
	"The Living Daylights",
	"License to Kill",
	"The Man with the Golden Gun",
};

s32 gexPlusGetScenario(void)
{
	return g_GexPlusScenario;
}

const char *gexPlusScenarioName(s32 scenario)
{
	return scenario >= 0 && scenario < GEXPLUS_NUMSCENARIOS ? g_GexPlusScenarioNames[scenario] : "";
}

void gexPlusSetScenario(s32 scenario)
{
	if (scenario < 0 || scenario >= GEXPLUS_NUMSCENARIOS) {
		scenario = GEXPLUS_NORMAL;
	}

	g_GexPlusScenario = scenario;
	g_MpSetup.scenario = scenario == GEXPLUS_FLAGTAG ? MPSCENARIO_HOLDTHEBRIEFCASE : MPSCENARIO_COMBAT;

	if (scenario == GEXPLUS_LICENCETOKILL) {
		g_MpSetup.options |= MPOPTION_ONEHITKILLS;
	} else {
		g_MpSetup.options &= ~MPOPTION_ONEHITKILLS;
	}
}

static s32 gexPlusYolt(void)
{
	return g_GexPlusMode && g_GexPlusScenario == GEXPLUS_YOLT && g_Vars.normmplayerisrunning;
}

s32 gexPlusLivesSpent(struct chrdata *chr)
{
	if (!gexPlusYolt() || !chr) {
		return 0;
	}

	for (s32 i = 0; i < g_MpNumChrs; i++) {
		if (g_MpAllChrPtrs[i] == chr) {
			return g_MpAllChrConfigPtrs[i]->numdeaths >= 2;
		}
	}

	return 0;
}

s32 gexPlusMatchOver(void)
{
	s32 alive = 0;

	if (!gexPlusYolt() || g_MpNumChrs < 2) {
		return 0;
	}

	for (s32 i = 0; i < g_MpNumChrs; i++) {
		if (g_MpAllChrConfigPtrs[i]->numdeaths < 2) {
			alive++;
		}
	}

	return alive <= 1;
}

static struct weaponobj g_GexPlusGoldenGunObj;

static void gexPlusPlaceGoldenGun(void)
{
	struct weaponobj template = {
		256,                    // extrascale
		0,                      // hidden2
		OBJTYPE_WEAPON,         // type
		0,                      // modelnum
		0,                      // pad
		OBJFLAG_FALL | OBJFLAG_INVINCIBLE | OBJFLAG_FORCENOBOUNCE,
		OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS,
		0,                      // flags3
		NULL,                   // prop
		NULL,                   // model
		1, 0, 0,                // realrot
		0, 1, 0,
		0, 0, 1,
		0,                      // hidden
		NULL,                   // geo
		NULL,                   // projectile
		0,                      // damage
		1000,                   // maxdamage
		0xff, 0xff, 0xff, 0x00, // shadecol
		0xff, 0xff, 0xff, 0x00, // nextcol
		0x0fff,                 // floorcol
		0,                      // tiles
		WEAPON_GE_GOLDENGUN,    // weaponnum
		0,                      // unk5d
		0,                      // unk5e
		FUNC_PRIMARY,           // gunfunc
		0,                      // fadeouttimer60
		-1,                     // dualweaponnum
		-1,                     // timer240
		NULL,                   // dualweapon
	};
	s32 i;

	if (g_NumSpawnPoints <= 0) {
		return;
	}

	// the pickup's model is its Combat Simulator row's (a borrowed gun's own)
	for (i = 0; i < NUM_MPWEAPONS; i++) {
		if (g_MpWeapons[i].weaponnum == WEAPON_GE_GOLDENGUN) {
			template.base.modelnum = g_MpWeapons[i].model;
			template.base.extrascale = g_MpWeapons[i].extrascale;
			break;
		}
	}

	if (i == NUM_MPWEAPONS || template.base.modelnum <= 0) {
		return;
	}

	g_GexPlusGoldenGunObj = template;
	g_GexPlusGoldenGunObj.base.pad = g_SpawnPoints[rngRandom() % g_NumSpawnPoints];

	setupPlaceWeapon(&g_GexPlusGoldenGunObj, 999);

	g_GexPlusGoldenGunObj.base.hidden2 &= ~OBJH2FLAG_CANREGEN;

	if (g_GexPlusGoldenGunObj.base.prop) {
		g_GexPlusGoldenGunObj.base.prop->forcetick = true;
	}
}

void gexPlusTick(void)
{
	struct prop *onfloor = NULL;
	bool going = false;
	s32 held = 0;
	s32 i;

	if (!g_GexPlusMode || g_GexPlusScenario != GEXPLUS_GOLDENGUN || !g_Vars.normmplayerisrunning) {
		return;
	}

	for (i = 0; i < g_MpNumChrs && !held; i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];

		// the dead have dropped theirs, though their inventory says otherwise
		if (!chr || !chr->prop || chrIsDead(chr)) {
			continue;
		}

		if (i < PLAYERCOUNT()) {
			const s32 prevplayernum = g_Vars.currentplayernum;

			setCurrentPlayerNum(i);
			held = invHasSingleWeaponIncAllGuns(WEAPON_GE_GOLDENGUN);
			setCurrentPlayerNum(prevplayernum);
		} else if (chr->aibot) {
			held = botinvGetItem(chr, WEAPON_GE_GOLDENGUN) != NULL;
		}
	}

	// one gun: any on the floor beside the one carried, or beside the first.
	// One still being taken away may be the object a new one would be placed
	// as, so none is placed until it has gone.
	for (struct prop *prop = g_Vars.activeprops; prop; prop = prop->next) {
		if (prop->type == PROPTYPE_WEAPON && prop->weapon->weaponnum == WEAPON_GE_GOLDENGUN) {
			if (prop->weapon->base.hidden & OBJHFLAG_DELETING) {
				going = true;
			} else if (held || onfloor) {
				prop->weapon->base.hidden |= OBJHFLAG_DELETING;
				prop->weapon->base.hidden2 &= ~OBJH2FLAG_CANREGEN;
			} else {
				onfloor = prop;
				prop->forcetick = true;
			}
		}
	}

	if (!held && !onfloor && !going) {
		gexPlusPlaceGoldenGun();
	}
}

/**
 * GE-X Plus's simulants wear GoldenEye's characters (Oddjob, Trevelyan, Jaws
 * ...) - the ones GoldenEye X lends the Combat Simulator, which are also the
 * folder's Characters page - rather than a simulant profile's Perfect Dark
 * body. Each sim takes a character nobody else in the match is wearing while
 * one is left, then any. Nothing changes when none are borrowed.
 *
 * From mpStartMatch(), after the quick team has made its simulants.
 */
void gexPlusThemeSimulants(void)
{
	static s32 bodies[256];
	s32 numbodies = 0;
	s32 i;

	if (!g_GexPlusMode) {
		return;
	}

	for (i = 0; i < g_MpListCounts.bodies && numbodies < ARRAYCOUNT(bodies); i++) {
		if (modBorrowBodyName(g_MpBodies[i].bodynum)) {
			bodies[numbodies++] = i;
		}
	}

	if (numbodies == 0) {
		return;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		const s32 offset = rngRandom() % numbodies;
		s32 mpbodynum = bodies[offset];
		s32 n;

		if (!mpIsSimSlotOn(i)) {
			continue;
		}

		for (n = 0; n < numbodies; n++) {
			const s32 candidate = bodies[(offset + n) % numbodies];
			s32 taken = false;
			s32 j;

			for (j = 0; j < MAX_MPCHRS && !taken; j++) {
				if (j != MAX_PLAYERS + i && mpIsChrSlotOn(j) && MPCHR(j)->mpbodynum == candidate) {
					taken = true;
				}
			}

			if (!taken) {
				mpbodynum = candidate;
				break;
			}
		}

		g_BotConfigsArray[i].base.mpbodynum = mpbodynum;
		g_BotConfigsArray[i].base.mpheadnum = mpGetMpheadnumByMpbodynum(mpbodynum);
	}
}

/* -------------------------------------------------------------------------
 * GoldenEye's own characters in the remake's missions
 * ------------------------------------------------------------------------- */

/**
 * GoldenEye's characters, in the order its own table holds them
 * (c_item_entries[], the decomp's assets/obseg/chr/chrModelFileRecords.inc.c),
 * which is the number a converted mission's chr record carries. Heads follow
 * the bodies from GEMISSION_FIRST_HEAD.
 *
 * The order is not the one chrobjdata.h declares the headers in - Bond in a
 * tuxedo is 5, not 40 - and three missions settle it: Jungle's forty camguards
 * and its Xenia, Cradle's five trevguards and its Trevelyan, and Facility's
 * fifteen scientists.
 *
 * `stock` is what Perfect Dark's own bodies make of the character when there is
 * nothing of GoldenEye's installed to wear.
 */
struct gemissionbody {
	const char *name;    // GoldenEye's own, which is Bean's asset name too
	const char *mpname;  // and the name its own select screen gives it, or NULL
	u8 stock;            // Perfect Dark's nearest body
};

#define GEMISSION_FIRST_HEAD 42

static const struct gemissionbody g_GeMissionBodies[] = {
	/* 00 */ { "camguard", "Jungle Commando", BODY_DD_GUARD },
	/* 01 */ { "greyguard", "St. Petersburg Guard", BODY_DD_GUARD },
	/* 02 */ { "oliveguard", "Russian Soldier", BODY_DD_GUARD },
	/* 03 */ { "rusguard", "Russian Infantry", BODY_DD_GUARD },
	/* 04 */ { "trevguard", "Janus Special Forces", BODY_DD_GUARD },
	/* 05 */ { "djbond", "Bond (Tuxedo)", BODY_DD_GUARD },
	/* 06 */ { "boris", "Boris", BODY_DD_GUARD },
	/* 07 */ { "orumov", "Ourumov", BODY_DD_GUARD },
	/* 08 */ { "trevelyan", "Trevelyan", BODY_DD_GUARD },
	/* 09 */ { "boilertrev", "Trevelyan (006)", BODY_DD_GUARD },
	/* 0a */ { "valentin", "Valentin", BODY_DD_GUARD },
	/* 0b */ { "xenia", "Xenia", BODY_CIFEMTECH },
	/* 0c */ { "baronsamedi", "Baron Samedi", BODY_DD_GUARD },
	/* 0d */ { "jaws", "Jaws", BODY_DD_GUARD },
	/* 0e */ { "mayday", "Mayday", BODY_CIFEMTECH },
	/* 0f */ { "oddjob", "Oddjob", BODY_DD_GUARD },
	/* 10 */ { "natalya", "Natalya", BODY_CIFEMTECH },
	/* 11 */ { "armourguard", "Janus Marine", BODY_DD_GUARD },
	/* 12 */ { "commguard", "Russian Commandant", BODY_DD_GUARD },
	/* 13 */ { "greatguard", "Siberian Guard", BODY_DD_GUARD },
	/* 14 */ { "navyguard", "Naval Officer", BODY_DD_GUARD },
	/* 15 */ { "snowguard", "Siberian Special Forces", BODY_DD_GUARD },
	/* 16 */ { "boilerbond", "Bond (Boiler Suit)", BODY_DD_GUARD },
	/* 17 */ { "suitbond", "Bond (Suit)", BODY_DD_GUARD },
	/* 18 */ { "timberbond", "Bond (Jungle)", BODY_DD_GUARD },
	/* 19 */ { "snowbond", "Bond (Parka)", BODY_DD_GUARD },
	/* 1a */ { "bluewoman", NULL, BODY_CIFEMTECH },
	/* 1b */ { "fattechwoman", NULL, BODY_CIFEMTECH },
	/* 1c */ { "techwoman", "Scientist", BODY_CIFEMTECH },
	/* 1d */ { "jeanwoman", "Civilian", BODY_CIFEMTECH },
	/* 1e */ { "greyman", NULL, BODY_DD_GUARD },
	/* 1f */ { "blueman", NULL, BODY_DD_GUARD },
	/* 20 */ { "redman", "Civilian", BODY_DD_GUARD },
	/* 21 */ { "cardiman", "Civilian", BODY_DD_GUARD },
	/* 22 */ { "checkman", "Civilian", BODY_DD_GUARD },
	/* 23 */ { "techman", "Scientist", BODY_DD_GUARD },
	/* 24 */ { "pilot", "Helicopter Pilot", BODY_DD_GUARD },
	/* 25 */ { "greatguard2", "Siberian Guard", BODY_DD_GUARD },
	/* 26 */ { "bluecamguard", "Arctic Commando", BODY_DD_GUARD },
	/* 27 */ { "moonguard", "Moonraker Elite", BODY_DD_GUARD },
	/* 28 */ { "moonfemale", "Moonraker Elite", BODY_CIFEMTECH },
	/* 29 */ { "suit_lf_hand", NULL, BODY_DD_GUARD },
};

/**
 * The body a GoldenEye character number becomes, by what the player has:
 * GoldenEye's own character out of the XBLA release (gebean.c's pool), else
 * GoldenEye X's if it is being borrowed from, else Perfect Dark's nearest.
 */
static s32 gexPlusBodyForGe(s32 gebody)
{
	char source[64];

	if (gebody < 0 || gebody >= (s32)ARRAYCOUNT(g_GeMissionBodies)) {
		return BODY_DD_GUARD;
	}

	const struct gemissionbody *row = &g_GeMissionBodies[gebody];

	snprintf(source, sizeof(source), "char/%s", row->name);

	const s32 bean = gebeanPoolNumBySource(source);

	if (bean >= 0) {
		return bean;
	}

	// GoldenEye X's own, which fill the same rows when it is installed. Its
	// names are the Combat Simulator's, not GoldenEye's file names, so a
	// character is found by the pool's name for it.
	if (row->mpname) {
		const s32 len = (s32)strlen(row->mpname);

		for (s32 i = 0; i < NUM_HEADSANDBODIES; i++) {
			const char *name = modBorrowBodyName(i);

			// a borrowed name carries its trailing newline
			if (name && !strncmp(name, row->mpname, len)
					&& (name[len] == '\0' || name[len] == '\n')) {
				return i;
			}
		}
	}

	return row->stock;
}

/**
 * A converted mission's props, once, before anything has read them.
 *
 * Two things in a mission's records are GoldenEye's own and cannot be mapped by
 * the conversion, because what the player has installed is not known until the
 * mission loads:
 *
 * - **a chr's body** is GoldenEye's own character number, which becomes the
 *   release's character, GoldenEye X's or Perfect Dark's nearest. Its head is
 *   left to Perfect Dark, since a Bean or GoldenEye X body carries its own.
 * A weapon's model is left alone: it is the pickup GoldenEye draws, converted
 * with the rest of the props, and a chr takes its held gun's model from the
 * weapon's own definition rather than the record. Pointing the record at the
 * GoldenEye weapon's model instead was tried and is wrong - that model aliases
 * the *first-person* gun, which has no bounding box, and every mission died
 * placing its first weapon in objGetLocalYMin().
 */
void gexPlusMissionSetup(u32 *props)
{
	struct defaultobj *obj = (struct defaultobj *)props;

	if (!obj) {
		return;
	}

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_CHR) {
			struct packedchr *chr = (struct packedchr *)obj;

			chr->bodynum = gexPlusBodyForGe(chr->bodynum);
			chr->headnum = -1;
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}
}



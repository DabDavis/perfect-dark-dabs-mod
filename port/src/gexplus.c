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

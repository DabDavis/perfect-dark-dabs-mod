/**
 * GE Plus's scenarios: GoldenEye's multiplayer modes over Perfect Dark's.
 *
 * GoldenEye's are chosen from the Combat Simulator's scenario list while in
 * GE Plus (scenarios.c) and are not a scenario number of their own - the
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
#include "gecinema.h"
#include "gexfront.h"
#include <string.h>
#include <stdio.h>
#include "modborrow.h"
#include "gebean.h"
#include "geconvert.h"
#include "geaitable.h"
#include "preprocess.h"
#include "romdata.h"
#include "fs.h"
#include "mod.h"
#include "config.h"
#include "platform.h"
#include "system.h"
#include "langpack.h"
#include <ctype.h>
#include "game/lang.h"
#include "lib/anim.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
#include "game/game_0b0fd0.h"
#include "game/setuputils.h"
#include "game/botinv.h"
#include "game/chraction.h"
#include "game/chrai.h"
#include "game/inv.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/propobj.h"
#include "game/setup.h"
#include "lib/rng.h"
#include "lib/joy.h"
#include "lib/main.h"
#include "game/lv.h"
#include "game/options.h"
#include "game/chr.h"
#include "lib/ailist.h"

static s32 g_GexPlusScenario = GEXPLUS_NORMAL;

// gexPlusExitOnButtonPress(): 0 not ending, 1 waiting for a press, 2 fading out
static s32 g_GeExitState;
static f32 g_GeExitFade60;

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
 * GE Plus's simulants wear GoldenEye's characters (Oddjob, Trevelyan, Jaws
 * ...) - the XBLA release's, else the ROM's conversion (gebean.c), which are
 * also the folder's Characters page, and never GoldenEye X's - rather than a
 * simulant profile's Perfect Dark body. Each sim takes a character nobody else
 * in the match is wearing while one is left, then any. Nothing changes when
 * there are none.
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
		if (gebeanIsGoldenEyeBody(g_MpBodies[i].bodynum)) {
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

	// every caller writes the answer into a byte
	if (bean >= 0 && bean < 256) {
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
				if (i < 256) {
					return i;
				}

				break;
			}
		}
	}

	return row->stock;
}

/* -------------------------------------------------------------------------
 * GoldenEye's own characters in a converted mission
 * ------------------------------------------------------------------------- */

/**
 * The conversion writes every one of GoldenEye's eighty characters as a Perfect
 * Dark model file of its own - files/Cgx%03dZ, the forty-two bodies and then
 * the heads (gechr.py) - and a table beside them, menu/gechrs.bin: each one's
 * two c_item_entries flags, its scale and its pov. Those last two are what
 * makeonebody() gives a chr's model, modelSetScale(scale * 0.1f) and
 * modelSetAnimTranslationScale(pov), which are Perfect Dark's own `scale` and
 * `animscale` in the same two places. Every character wears GoldenEye's guard
 * skeleton, which is g_SkelChr joint for joint, so the game's own animations
 * pose one and its gun sits in its hand.
 *
 * A mission dresses its chrs out of that table and nothing else. What the
 * player has installed decides how GE Plus's *arenas* look - the XBLA release's
 * characters, GoldenEye X's - but a converted mission is GoldenEye's own level,
 * and its guards are GoldenEye's own guards.
 *
 * The rows go in g_HeadsAndBodies past the stock table, beside the release's
 * pool (gebean.c) and GoldenEye X's borrowed characters, and are taken and
 * given back per mission: the twenty missions ask for six bodies at the most
 * (Train's six). **A body's row has to be addressable as a byte**, since a
 * setup's packedchr keeps bodynum in one and so does aiSpawnChrAtPad, so a body
 * takes one of the rows kept for it under the pool (gebean.h) - they used to
 * share the pool's, and with GoldenEye X installed there was never one left.
 * A head's row is never written into a record -
 * GoldenEye's own setups leave all but two heads at -1 and let bodyChooseHead()
 * pick, exactly as Perfect Dark's own do - so heads take rows past 255, where
 * there is always room.
 */
// GoldenEye's 80, and a ROM hack's more (Goldfinger 64's 126)
#define GEROM_NUM_CHRS    128
#define GEROM_FIRST_HEAD  42    // GoldenEye's HEAD_START
// and its last, Bond's tuxedo head. **79 is a body again** - Natalya in her
// jungle fatigues, the one body the enum lists after the heads, and who she is
// on Jungle and Control. Read as a head she took a head's row, which does not
// fit the byte a record keeps its body in: 256 became Perfect Dark's body 0,
// and once heads were taken from the far end 510 became 254, an empty row and
// a model with no definition under chrSetLookAngle().
#define GEROM_LAST_HEAD   78
#define GEROM_IS_HEAD(num) geRomIsHead(g_GeRomChrs, g_GeRomHeadFlagged, (num))
#define GEROM_ROWLEN      12
// GEROM_BODY_FIRST..GEROM_BODY_LAST are gebean.h's: rows kept for a mission's
// bodies, under the pool, where a packedchr's u8 bodynum reaches
#define GEROM_HEAD_FIRST  256
// the rows a mission holds, bodies and heads together: the bodies are capped
// by their own 24 (the row search), the heads by the free rows from the far
// end. Both shared one cap of 24 once, and Goldfinger 64's Ranch, with twelve
// bodies each wearing a head of its own, had none left for Bond when third
// person built his body: the player was Joanna (F3 20261003-071337)
#define GEROM_HEAD_ROWS   128
#define GEROM_MAX_ROWS    (GEROM_BODY_ROWS + GEROM_HEAD_ROWS)

_Static_assert(GEROM_BODY_LAST < 256, "a mission's body row has to fit a byte");

// c_item_entries' two flags, as the conversion writes them, and whether the
// model is a head (no skeleton), which a conversion since converter 98 marks
#define GEROM_MALE        0x1
#define GEROM_HASHEAD     0x2
#define GEROM_ISHEAD      0x4

// GoldenEye's own head pools: chr.c's random_male_heads and random_female_heads
// as c_item_entries numbers. Its Terrorist, Biker and Mishkin heads are in
// neither, and are worn only where a setup names them.
static const u8 g_GeRomMaleHeads[] = {
	57, 54, 55, 62, 59, 56, 58, 53, 52, 51, 42, 43, 44, 45, 46,
	47, 48, 49, 50, 63, 64, 65, 66, 67, 68,
};

static const u8 g_GeRomFemaleHeads[] = { 70, 71, 72, 73 };

// GoldenEye takes its four heads from one place in the male list for a whole
// level (initguards.c's current_random_male_head, and bodyChooseHead()'s
// `+ (random & 3)`), and one female head for all of it. A ROM hack may take
// more (gecast.bin's fourth byte: Tomorrow Never Dies 64's `andi 7` at
// 7F0235E4 is eight), up to the game's own eight (g_ActiveMaleHeads)
#define GEROM_MALE_HEADS_PER_LEVEL 4
#define GEROM_MAX_MALE_HEADS_PER_LEVEL 8

struct geromchr {
	f32 scale;
	f32 pov;
	u8 flags;
};

// a row this mission holds: GoldenEye's character in it and, for a body whose
// record named a head rather than taking one of the pool's, that head's row
struct geromrow {
	s16 row;
	s16 chr;
	s16 ownhead;
};

static struct geromchr g_GeRomChrs[GEROM_NUM_CHRS];
static s32 g_GeRomNumChrs;            // 0 until a conversion's table has been read
static s32 g_GeRomHeadFlagged;        // and whether it marks its heads (GEROM_ISHEAD)

/**
 * Whether character `num` of a table is a head. A ROM hack's list mixes heads
 * and bodies (Goldfinger's heads are 42-50, 63-67, 73-75 and on), so a
 * conversion marks each; an older one's is GoldenEye's own, whose bodies are
 * 0-41 and 79 and heads 42-78.
 */
static s32 geRomIsHead(const struct geromchr *table, s32 flagged, s32 num)
{
	if (num < 0 || num >= GEROM_NUM_CHRS) {
		return 0;
	}

	if (flagged) {
		return (table[num].flags & GEROM_ISHEAD) != 0;
	}

	return num >= GEROM_FIRST_HEAD && num <= GEROM_LAST_HEAD;
}

static s32 geRomTableFlagged(const struct geromchr *table, s32 numchrs)
{
	for (s32 i = 0; i < numchrs; i++) {
		if (table[i].flags & GEROM_ISHEAD) {
			return 1;
		}
	}

	return 0;
}

/**
 * menu/gecast.bin (converter 98): what the code rather than a setup says a
 * mission's people are - random_male_heads and random_female_heads, the pools
 * a guard with no head of its own draws from, and solo_char_load()'s Bond, a
 * body and a head an outfit. A ROM hack changes all three (Goldfinger's Bond
 * is one head on a body a mission). Without the file, GoldenEye's own.
 */
#define GEROM_MAX_POOL 64
#define GEROM_NUM_CUFFS 9

static u8 g_GeRomMen[GEROM_MAX_POOL];
static u8 g_GeRomWomen[GEROM_MAX_POOL];
static s32 g_GeRomNumMen;
static s32 g_GeRomNumWomen;
static u8 g_GeRomBond[GEROM_NUM_CUFFS][2];
static s32 g_GeRomHasBond;
static s32 g_GeRomMenPerLevel = GEROM_MALE_HEADS_PER_LEVEL;

/**
 * A ROM hack's further pools, after the file's Bond (converter 102): a guard
 * whose record or spawn names head -2 draws from the first of them rather than
 * the pool above - Goldfinger's six Korean henchmen, in nine of its missions.
 * Each takes up to four heads for a level from a start of its own, as the
 * hack's init does (one female head), and they are handed out in turn as
 * bodyChooseHead() hands out the game's own.
 */
#define GEROM_EXTRA_POOLS 3

struct geromextrapool {
	u8 heads[2][GEROM_MAX_POOL];     // men, women
	s32 num[2];
	s16 active[2][GEROM_MAX_MALE_HEADS_PER_LEVEL];
	s32 numactive[2];
	s32 next[2];
};

static struct geromextrapool g_GeRomExtraPools[GEROM_EXTRA_POOLS];
static s32 g_GeRomNumExtraPools;
static s32 g_GeRomTableModDir = -2;   // the mod its table was read from
static struct geromrow g_GeRomRows[GEROM_MAX_ROWS];
static s32 g_GeRomNumRows;

/**
 * A conversion's menu/gechrs.bin into `out`: how many characters it holds, or 0
 * where it has none. `out` is cleared first, so a row the file skips reads as
 * a character with no flags.
 */
static s32 geRomReadTable(const char *dir, struct geromchr *out, s32 warn)
{
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *d;
	s32 numchrs;

	memset(out, 0, sizeof(struct geromchr) * GEROM_NUM_CHRS);
	snprintf(path, sizeof(path), "%s/menu/gechrs.bin", dir);
	d = fsFileLoad(path, &len);

	if (!d || len < 8 || memcmp(d, "GEC1", 4)) {
		if (warn) {
			sysLogPrintf(LOG_WARNING, "gexplus: the conversion has no characters at %s", path);
		}

		sysMemFree(d);
		return 0;
	}

	numchrs = (d[4] << 8) | d[5];

	if (numchrs > GEROM_NUM_CHRS) {
		numchrs = GEROM_NUM_CHRS;
	}

	if (numchrs < 0 || len < 8 + (u32)GEROM_ROWLEN * numchrs) {
		sysMemFree(d);
		return 0;
	}

	for (s32 i = 0; i < numchrs; i++) {
		const u8 *row = d + 8 + GEROM_ROWLEN * i;
		u32 bits;

		if (((row[0] << 8) | row[1]) != i) {
			continue;
		}

		out[i].flags = row[3];
		bits = ((u32)row[4] << 24) | (row[5] << 16) | (row[6] << 8) | row[7];
		memcpy(&out[i].scale, &bits, sizeof(f32));
		bits = ((u32)row[8] << 24) | (row[9] << 16) | (row[10] << 8) | row[11];
		memcpy(&out[i].pov, &bits, sizeof(f32));
	}

	sysMemFree(d);

	return numchrs;
}

static void geRomReadCast(const char *dir)
{
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *d = NULL;

	g_GeRomNumMen = ARRAYCOUNT(g_GeRomMaleHeads);
	memcpy(g_GeRomMen, g_GeRomMaleHeads, sizeof(g_GeRomMaleHeads));
	g_GeRomNumWomen = ARRAYCOUNT(g_GeRomFemaleHeads);
	memcpy(g_GeRomWomen, g_GeRomFemaleHeads, sizeof(g_GeRomFemaleHeads));
	g_GeRomHasBond = 0;
	g_GeRomNumExtraPools = 0;
	g_GeRomMenPerLevel = GEROM_MALE_HEADS_PER_LEVEL;

	snprintf(path, sizeof(path), "%s/menu/gecast.bin", dir);

	if (fsFileSize(path) > 0) {
		d = fsFileLoad(path, &len);
	}

	if (d && len >= 8 && memcmp(d, "GEK1", 4) == 0) {
		const s32 men = d[4];
		const s32 women = d[5];
		const s32 cuffs = d[6];

		if (men <= GEROM_MAX_POOL && women <= GEROM_MAX_POOL && cuffs == GEROM_NUM_CUFFS
				&& len >= 8 + (u32)(men + women + 2 * cuffs)) {
			if (men > 0) {
				g_GeRomNumMen = men;
				memcpy(g_GeRomMen, d + 8, men);
			}

			if (women > 0) {
				g_GeRomNumWomen = women;
				memcpy(g_GeRomWomen, d + 8 + men, women);
			}

			memcpy(g_GeRomBond, d + 8 + men + women, sizeof(g_GeRomBond));
			g_GeRomHasBond = 1;

			// how many men a level draws from (converter 113; 0 GoldenEye's four)
			if (d[7] > 0 && d[7] <= GEROM_MAX_MALE_HEADS_PER_LEVEL) {
				g_GeRomMenPerLevel = d[7];
			}

			// and any further pools: u8 how many, then u8 men, u8 women and
			// the heads each
			u32 at = 8 + men + women + 2 * cuffs;

			if (at < len) {
				const s32 extra = d[at++];

				for (s32 k = 0; k < extra && k < GEROM_EXTRA_POOLS && at + 2 <= len; k++) {
					struct geromextrapool *pool = &g_GeRomExtraPools[k];
					const s32 nm = d[at];
					const s32 nw = d[at + 1];

					if (nm > GEROM_MAX_POOL || nw > GEROM_MAX_POOL || at + 2 + nm + nw > len) {
						break;
					}

					memset(pool, 0, sizeof(*pool));
					pool->num[0] = nm;
					pool->num[1] = nw;
					memcpy(pool->heads[0], d + at + 2, nm);
					memcpy(pool->heads[1], d + at + 2 + nm, nw);
					at += 2 + nm + nw;
					g_GeRomNumExtraPools = k + 1;
				}
			}
		}
	}

	sysMemFree(d);
}

/**
 * menu/gechrs.bin for the mod a stage belongs to, once.
 */
static s32 geRomLoadTable(s32 stagenum)
{
	const s32 moddir = modloaderGetStageModDirIndex(stagenum);
	const char *dir = modloaderGetStageModDir(stagenum);
	s32 numchrs;

	if (moddir < 0 || !dir) {
		return 0;
	}

	if (g_GeRomTableModDir == moddir) {
		return g_GeRomNumChrs > 0;
	}

	g_GeRomTableModDir = moddir;
	g_GeRomNumChrs = 0;

	numchrs = geRomReadTable(dir, g_GeRomChrs, 1);

	if (numchrs <= 0) {
		return 0;
	}

	g_GeRomNumChrs = numchrs;
	g_GeRomHeadFlagged = geRomTableFlagged(g_GeRomChrs, numchrs);
	geRomReadCast(dir);

	return 1;
}

/*
 * GoldenEye's headHat_array_8003E464 (menu/headhats.bin, converter 84): where a
 * hat sits on each of its 28 random heads, Karl (42, HEAD_START) to Mishkin
 * (69), a row per hat type - beret, side cap, peaked cap, helmet, fur hat, moon
 * - of an offset and a scale in the hat's own space. chrRender() moves the hat
 * by it as GoldenEye's does, every frame (chr.c). The first head was taken as
 * 45 until 2026-09-29, which is Martin: Karl, Alan and Pete (42-44, all in the
 * male pool) wore their hats as the model has them, every other head took the
 * row of the head three on - a helmet sat at another head's height (F3
 * 20260929-093133) - and three women's heads took Mishkin's and the two
 * before his, where GoldenEye fits no woman's (BODY_Female_Sally, 70, ends
 * its range).
 */
#define GEHAT_HEAD_FIRST 42
#define GEHAT_NUM_HEADS  28
#define GEHAT_NUM_TYPES  6
#define GEHAT_ROW        24

static u8 *g_GeHeadHats;
static u32 g_GeHeadHatsLen;
static s32 g_GeHeadHatsModDir = -2;
// the rows' first character and how many there are: GoldenEye's 42 and 28,
// a ROM hack's own where the file says ("GEH1", converter 98)
static s32 g_GeHeadHatsFirst = GEHAT_HEAD_FIRST;
static s32 g_GeHeadHatsCount = GEHAT_NUM_HEADS;
static u32 g_GeHeadHatsAt;

static f32 geHatFloat(const u8 *p)
{
	union { u32 u; f32 f; } v;

	v.u = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];

	return v.f;
}

/**
 * Where GoldenEye puts a hat of `hattype` (propobj.c's hatGetType()) on the
 * chr's head row `headnum`: an offset in its units (x 21.3 by the caller, as
 * GoldenEye's) and a scale per axis. 0 for a head that is not one of
 * GoldenEye's random ones (a woman's, Bond's, anything off a converted
 * mission), which wears the hat as the model has it.
 */
s32 gexPlusHeadHat(s32 headnum, s32 hattype, f32 *out)
{
	const s32 moddir = modloaderGetStageModDirIndex(g_Vars.stagenum);
	s32 gehead = -1;

	if (hattype < 0 || hattype >= GEHAT_NUM_TYPES || headnum <= 0 || moddir < 0
			|| !modloaderStageIsMission(g_Vars.stagenum)) {
		return 0;
	}

	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		if (g_GeRomRows[i].row == headnum) {
			gehead = g_GeRomRows[i].chr;
			break;
		}
	}

	if (g_GeHeadHatsModDir != moddir) {
		const char *dir = modloaderGetStageModDir(g_Vars.stagenum);
		char path[FS_MAXPATH + 1];

		sysMemFree(g_GeHeadHats);
		g_GeHeadHats = NULL;
		g_GeHeadHatsLen = 0;
		g_GeHeadHatsModDir = moddir;

		if (dir) {
			snprintf(path, sizeof(path), "%s/menu/headhats.bin", dir);

			if (fsFileSize(path) > 0) {
				g_GeHeadHats = fsFileLoad(path, &g_GeHeadHatsLen);
			}
		}

		g_GeHeadHatsFirst = GEHAT_HEAD_FIRST;
		g_GeHeadHatsCount = GEHAT_NUM_HEADS;
		g_GeHeadHatsAt = 0;

		if (g_GeHeadHats && g_GeHeadHatsLen >= 8 && memcmp(g_GeHeadHats, "GEH1", 4) == 0) {
			g_GeHeadHatsFirst = (g_GeHeadHats[4] << 8) | g_GeHeadHats[5];
			g_GeHeadHatsCount = (g_GeHeadHats[6] << 8) | g_GeHeadHats[7];
			g_GeHeadHatsAt = 8;
		}
	}

	if (gehead < g_GeHeadHatsFirst || gehead >= g_GeHeadHatsFirst + g_GeHeadHatsCount) {
		return 0;
	}

	{
		const u32 at = g_GeHeadHatsAt + ((u32)(gehead - g_GeHeadHatsFirst) * GEHAT_NUM_TYPES + (u32)hattype) * GEHAT_ROW;

		if (!g_GeHeadHats || at + GEHAT_ROW > g_GeHeadHatsLen) {
			return 0;
		}

		for (s32 k = 0; k < 6; k++) {
			out[k] = geHatFloat(g_GeHeadHats + at + 4 * k);
		}
	}

	return 1;
}

/**
 * menu/hattypes.bin (converter 120): get_hat_model()'s kinds of the twelve hat
 * props as the conversion's ROM deals them, "GHT1", the first prop, how many,
 * then a kind each. Loaded by the stage's mod as headhats.bin is.
 */
static u8 *g_GeHatTypes;
static u32 g_GeHatTypesLen;
static s32 g_GeHatTypesModDir = -2;

s32 gexPlusHatType(s32 propnum)
{
	const s32 moddir = modloaderStageIsRemake(g_Vars.stagenum) ? modloaderGetStageModDirIndex(g_Vars.stagenum) : -1;

	if (moddir < 0) {
		return -2;
	}

	if (g_GeHatTypesModDir != moddir) {
		const char *dir = modloaderGetStageModDir(g_Vars.stagenum);
		char path[FS_MAXPATH + 1];

		sysMemFree(g_GeHatTypes);
		g_GeHatTypes = NULL;
		g_GeHatTypesLen = 0;
		g_GeHatTypesModDir = moddir;

		if (dir) {
			snprintf(path, sizeof(path), "%s/menu/hattypes.bin", dir);

			if (fsFileSize(path) > 0) {
				g_GeHatTypes = fsFileLoad(path, &g_GeHatTypesLen);
			}
		}

		if (g_GeHatTypes && (g_GeHatTypesLen < 6 || memcmp(g_GeHatTypes, "GHT1", 4) != 0
					|| g_GeHatTypesLen < 6u + g_GeHatTypes[5])) {
			sysMemFree(g_GeHatTypes);
			g_GeHatTypes = NULL;
			g_GeHatTypesLen = 0;
		}
	}

	if (!g_GeHatTypes) {
		return -2;
	}

	if (propnum < g_GeHatTypes[4] || propnum >= g_GeHatTypes[4] + g_GeHatTypes[5]) {
		return -2;
	}

	return g_GeHatTypes[6 + propnum - g_GeHatTypes[4]] < GEHAT_NUM_TYPES ? g_GeHatTypes[6 + propnum - g_GeHatTypes[4]] : -2;
}

/**
 * Every row this held, given back: a mission's rows are its own, and the next
 * one asks for whichever characters it wants.
 */
static void geRomReleaseRows(void)
{
	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		const s32 row = g_GeRomRows[i].row;

		if (row >= 0 && row < NUM_HEADSANDBODIES) {
			memset(&g_HeadsAndBodies[row], 0, sizeof(struct headorbody));
		}
	}

	g_GeRomNumRows = 0;
}

/**
 * A row of g_HeadsAndBodies as GoldenEye's character `c`, whose model is file
 * `fileid`. It starts as one of Perfect Dark's own - the dataDyne guard for a
 * man, the Institute's female technician for a woman, a stock head for a head -
 * with the model file and the two scales replaced: the rest of a row is what
 * the game asks of any body (its race, its hands, whether its height varies)
 * and a converted character answers the same way.
 */
static void geRomFillRow(struct headorbody *hb, const struct geromchr *c, s32 ishead, s32 fileid)
{
	const struct headorbody *host = &g_HeadsAndBodies[ishead
			? ((c->flags & GEROM_MALE) ? HEAD_JAMIE : HEAD_ANKA)
			: ((c->flags & GEROM_MALE) ? BODY_DD_GUARD : BODY_CIFEMTECH)];

	*hb = *host;
	hb->filenum = (u16)fileid;
	hb->modeldef = NULL;
	hb->scale = c->scale;
	hb->animscale = c->pov;
	hb->ismale = (c->flags & GEROM_MALE) != 0;
	// GoldenEye's own hasHead, which its retrieve_header_for_body_and_head()
	// tests before it looks for a head at all
	hb->unk00_01 = (c->flags & GEROM_HASHEAD) != 0;
	// a guard is the height GoldenEye modelled it at: nothing in its own
	// makeonebody() varies one
	hb->canvaryheight = 0;
}

/**
 * The row GoldenEye's character `num` wears in this mission, taking one and
 * filling it out of the ROM's own table the first time it is asked for. -1
 * where there is no row left to take, or no such character.
 *
 * `ownhead` is the row of the head a body is to wear where a record named one,
 * and -1 where it takes the pool's - a body wanted both ways takes a row each
 * way, so that Facility's fifteen scientists still get GoldenEye's own faces
 * while Doctor Doak, who is the same body, keeps his. The row is filled by
 * geRomFillRow().
 */
static s32 geRomTake(s32 num, s32 ownhead)
{
	const s32 ishead = GEROM_IS_HEAD(num);
	char name[16];
	s32 row = -1;
	s32 fileid;

	if (num < 0 || num >= g_GeRomNumChrs) {
		return -1;
	}

	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		if (g_GeRomRows[i].chr == num && g_GeRomRows[i].ownhead == ownhead) {
			return g_GeRomRows[i].row;
		}
	}

	if (g_GeRomNumRows >= GEROM_MAX_ROWS) {
		return -1;
	}

	snprintf(name, sizeof(name), "Cgx%03dZ", num);
	fileid = romdataRegisterModFile(name, g_GeRomTableModDir);

	if (fileid <= 0) {
		return -1;
	}

	if (ishead) {
		// from the far end of the table, which the pool fills from the near
		// one: it may be refreshed while a mission holds these (the pause
		// menu's Customize Character), and is 106 rows with GoldenEye X's in it
		for (s32 i = NUM_HEADSANDBODIES - 2; i >= GEROM_HEAD_FIRST; i--) {
			if (!g_HeadsAndBodies[i].filenum) {
				row = i;
				break;
			}
		}
	} else {
		for (s32 i = GEROM_BODY_FIRST; i <= GEROM_BODY_LAST; i++) {
			if (!g_HeadsAndBodies[i].filenum) {
				row = i;
				break;
			}
		}
	}

	if (row < 0) {
		sysLogPrintf(LOG_WARNING, "gexplus: no row left for GoldenEye's character %d", num);
		return -1;
	}

	geRomFillRow(&g_HeadsAndBodies[row], &g_GeRomChrs[num], ishead, fileid);

	// in a crash report's log ring, which is where the question "what was that
	// chr wearing" gets asked
	sysLogPrintf(LOG_NOTE, "gexplus: GoldenEye's character %d (%s) takes row %d as a %s",
			num, name, row, ishead ? "head" : "body");

	g_GeRomRows[g_GeRomNumRows].row = (s16)row;
	g_GeRomRows[g_GeRomNumRows].chr = (s16)num;
	g_GeRomRows[g_GeRomNumRows].ownhead = (s16)ownhead;
	g_GeRomNumRows++;

	return row;
}

/**
 * GoldenEye's character `body` wearing `head` (its own numbers, `head` negative
 * for one of the pool's), as a row a record can name. -1 where the conversion
 * has no table or there is no row to be had, and the caller falls back on what
 * the player has installed.
 */
static s32 geRomBodyRow(s32 body, s32 head)
{
	s32 ownhead = -1;

	if (GEROM_IS_HEAD(head)) {
		ownhead = geRomTake(head, -1);
	} else if (head <= -2 && head >= -1 - g_GeRomNumExtraPools) {
		// a further pool's (gexPlusRomOwnHead()): a row of its own, as a body
		// wanted with a head of its own takes one
		ownhead = head;
	}

	return geRomTake(body, ownhead);
}

/**
 * Bond himself, as GoldenEye dresses him for a mission: solo_char_load()'s
 * choice (bondview2.c) by the outfit the setup's intro names - the same
 * INTROCMD_OUTFIT the watch's sleeve is chosen from. It is the body the opening
 * swirl circles and the one every ending is about; without it the player's
 * body on a converted mission was Joanna's.
 *
 * GoldenEye's numbers: the bodies are 5 Brosnan's tuxedo, 22 the special
 * operations uniform, 23 formal wear, 24 the jungle fatigues and 25 the parka,
 * and Brosnan's five heads run 74 boiler, 75 default, 76 jungle, 77 parka,
 * 78 tuxedo. The other three Bonds' cuffs wear Brosnan's tuxedo in the US ROM.
 *
 * False where the conversion has no characters, and the caller carries on to
 * Perfect Dark's own choice.
 */
s32 gexPlusMissionBond(s32 outfit, s32 *bodynum, s32 *headnum)
{
	s32 gebody = 23;
	s32 gehead = 75;
	s32 head, body;

	if (!modloaderStageIsMission(g_Vars.stagenum) || !geRomLoadTable(g_Vars.stagenum)) {
		return 0;
	}

	switch (outfit) {
	case 1: case 5: case 6: case 7: case 8: gebody = 5; gehead = 78; break;  // CUFF_BROSNAN..CUFF_FOLDER
	case 2: gebody = 24; gehead = 76; break;  // CUFF_JUNGLE
	case 3: gebody = 22; gehead = 74; break;  // CUFF_BOILER
	case 4: gebody = 25; gehead = 77; break;  // CUFF_SNOW
	}

	// the conversion's own table, which is a ROM hack's own Bond
	if (g_GeRomHasBond) {
		const s32 cuff = outfit >= 0 && outfit < GEROM_NUM_CUFFS ? outfit : 0;

		gebody = g_GeRomBond[cuff][0];
		gehead = g_GeRomBond[cuff][1];
	}

	head = geRomTake(gehead, -1);
	body = geRomTake(gebody, head);

	if (head < 0 || body < 0) {
		return 0;
	}

	*bodynum = body;
	*headnum = head;

	return 1;
}

/**
 * The head a converted mission's body wears where its record named one rather
 * than taking GoldenEye's pool: Facility's Doctor Doak and Statue Park's
 * Mishkin are the two in the twenty missions, and -1 is every other body.
 *
 * bodyChooseHead() asks, which is where both a setup's chr and aiSpawnChrAtPad
 * arrive once the head they carry is -1 - and it has to be -1, since neither
 * field is wide enough for a row past 127.
 *
 * A body whose record named a further pool (head -2 on) takes that pool's next
 * head for its sex; one with none for its sex takes the game's own pool, -1.
 */
s32 gexPlusRomOwnHead(s32 bodynum)
{
	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		if (g_GeRomRows[i].row == bodynum) {
			const s32 ownhead = g_GeRomRows[i].ownhead;

			if (ownhead <= -2 && -2 - ownhead < g_GeRomNumExtraPools) {
				struct geromextrapool *pool = &g_GeRomExtraPools[-2 - ownhead];
				const s32 sex = g_HeadsAndBodies[bodynum].ismale ? 0 : 1;

				if (pool->numactive[sex] <= 0) {
					return -1;
				}

				pool->next[sex] = (pool->next[sex] + 1) % pool->numactive[sex];

				return pool->active[sex][pool->next[sex]];
			}

			return ownhead;
		}
	}

	return -1;
}

/**
 * Whether a row is one this holds, which is what keeps headfit.c off a pair
 * GoldenEye made for itself: its heads sit on its own bodies' headspots as they
 * are, and a neck measured between two of them would move one that fits.
 */
s32 gexPlusRomChrForRow(s32 row)
{
	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		if (g_GeRomRows[i].row == row) {
			return g_GeRomRows[i].chr;
		}
	}

	return -1;
}

s32 gexPlusRomIsPoolRow(s32 num)
{
	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		if (g_GeRomRows[i].row == num) {
			return 1;
		}
	}

	return 0;
}

/**
 * GoldenEye's character number behind a row, wherever GE Plus took it from: a
 * mission's own rows, or the pool's (the release's, the ROM's standing in for
 * it and the extras - the arenas' and Customize Character's).
 */
static s32 gexPlusGeChrForRow(s32 row)
{
	const s32 ge = gexPlusRomChrForRow(row);

	return ge >= 0 ? ge : gebeanRowGeChr(row);
}

/**
 * Whether DK Mode leaves a chr at its own proportions: the Japanese
 * cartridge's chrCanUseDKModeScaling() (chr.c, BUGFIX_R1, on j_text_trigger),
 * which spares GoldenEye's named characters - by body Boris, Ourumov, both
 * Trevelyans, Valentin, Xenia, Baron Samedi, Jaws, May Day, Oddjob and both
 * Natalyas, by head Brosnan's five and Mishkin - while every other guard still
 * gets the big head and arms. Its callers are the three places DK Mode acts:
 * the build (chr_b.c's 0.8, cheat.c's 1.25 for guards), the joints (the head
 * at 4, the arms at 2.5) and the distance scale in the render.
 *
 * GE Plus with the region's rules on Japan only; the US cartridge, and
 * Perfect Dark, scale everyone.
 */
s32 gexPlusDkModeSpares(s32 bodynum, s32 headnum)
{
	s32 body, head;

	// GoldenEye's own numbers, which a ROM hack's characters are not
	if (!gexFrontIsJapanese() || !gexFrontIsInside() || (modloaderStageIsRemake(g_Vars.stagenum)
			&& !modloaderStageIsGexPlus(g_Vars.stagenum))) {
		return 0;
	}

	body = gexPlusGeChrForRow(bodynum);
	head = headnum >= 0 ? gexPlusGeChrForRow(headnum) : -1;

	// GoldenEye's BODY_Boris (6) to BODY_Natalya_Skirt (16), and
	// BODY_Natalya_Jungle_Fatigues (79), the one body after the heads
	if ((body >= 6 && body <= 16) || body == 79) {
		return 1;
	}

	// BODY_Male_Mishkin (69) and BODY_Male_Pierce_Bond_1 to _Tuxedo (74-78),
	// the enum's head numbers
	return head == 69 || (head >= 74 && head <= 78);
}

/* -------------------------------------------------------------------------
 * GoldenEye's own characters in the Combat Simulator's lists
 * ------------------------------------------------------------------------- */

/**
 * With no XBLA release to dress the Combat Simulator's GoldenEye characters
 * (gebean.c's pool), they are the conversion's own models - GoldenEye's N64
 * characters, the ones its missions wear. The table is read into a copy of its
 * own, so a mission's (g_GeRomChrs, keyed by the stage's mod) is never moved
 * under it, and the conversion is found as the gun models are
 * (gegunsFindConverted()): the mounted directory that holds the table.
 */
static struct geromchr g_GeRomMpChrs[GEROM_NUM_CHRS];
static s32 g_GeRomMpNumChrs;
static s32 g_GeRomMpDir = -1;

s32 gexPlusRomMpBegin(void)
{
	const s32 numdirs = fsGetNumModDirs();

	g_GeRomMpDir = -1;
	g_GeRomMpNumChrs = 0;

	for (s32 i = 0; i < numdirs; i++) {
		const char *at = fsGetModDirAt(i);
		char path[FS_MAXPATH + 1];
		s32 n;

		// GoldenEye's own conversion's, never a ROM hack's (Goldfinger 64's
		// numbers its characters its own way), wherever the two are mounted
		if (!at || (modloaderGexPlusDirIndex() >= 0 && !modloaderDirIndexIsGexPlus(i))) {
			continue;
		}

		// the first body is the conversion's marker, as the PP7 is the guns'
		snprintf(path, sizeof(path), "%s/files/Cgx000Z", at);

		if (fsFileSize(path) <= 0) {
			continue;
		}

		n = geRomReadTable(at, g_GeRomMpChrs, 0);

		if (n > 0) {
			g_GeRomMpDir = i;
			g_GeRomMpNumChrs = n;
			break;
		}
	}

	return g_GeRomMpNumChrs;
}

s32 gexPlusRomMpFill(s32 num, struct headorbody *hb)
{
	char name[16];
	char path[FS_MAXPATH + 1];
	s32 fileid;

	if (g_GeRomMpDir < 0 || num < 0 || num >= g_GeRomMpNumChrs || !fsGetModDirAt(g_GeRomMpDir)) {
		return 0;
	}

	snprintf(name, sizeof(name), "Cgx%03dZ", num);
	snprintf(path, sizeof(path), "%s/files/%s", fsGetModDirAt(g_GeRomMpDir), name);

	// a name the conversion did not write would take a slot all the same, and
	// fail only when a match loads it
	if (fsFileSize(path) <= 0) {
		return 0;
	}

	fileid = romdataRegisterModFile(name, g_GeRomMpDir);

	if (fileid <= 0) {
		return 0;
	}

	geRomFillRow(hb, &g_GeRomMpChrs[num], geRomIsHead(g_GeRomMpChrs, geRomTableFlagged(g_GeRomMpChrs, g_GeRomMpNumChrs), num), fileid);

	// GoldenEye seats any of its heads on any of its bodies as they are: one
	// type for them all, so the ROM's type table moves none of them
	// (bodyCalculateHeadOffset()), where a man's head on a woman's body would
	// otherwise take the host's male-on-female offset
	hb->type = HEADBODYTYPE_DEFAULT;

	return fileid;
}

/**
 * The heads a converted mission's guards wear, from the end of bodiesReset() -
 * which is after the mission's own rows were taken (setupLoadFiles) and before
 * its chrs are made (setupCreateProps), and is where Perfect Dark fills the
 * same two lists with its own.
 *
 * GoldenEye picks a place in its own male list when a level starts and takes
 * the four heads from there, and one female head for the whole level
 * (initguards.c, bodyChooseHead()); Perfect Dark's active lists are the same
 * idea, four or eight heads drawn for the level, so they are filled with
 * GoldenEye's choice and the game's own bodyChooseHead() answers out of them.
 */
void gexPlusMissionHeads(void)
{
	s32 start, female;

	// A stage that is not a converted mission gives the rows back: they are a
	// mission's own, and its heads must not end up in the game's own lists on
	// the next level - bodiesReset() runs on every one of them, and only a
	// mission's own load goes through gexPlusMissionSetup().
	if (!modloaderStageIsMission(g_Vars.stagenum)) {
		geRomReleaseRows();
		return;
	}

	if (g_GeRomNumRows <= 0) {
		return;
	}

	// the conversion's pools (geRomReadCast()): GoldenEye's, or a ROM hack's
	start = (s32)(rngRandom() % (u32)g_GeRomNumMen);
	female = geRomTake(g_GeRomWomen[rngRandom() % (u32)g_GeRomNumWomen], -1);

	g_NumActiveHeadsPerGender = g_GeRomMenPerLevel;

	for (s32 i = 0; i < g_GeRomMenPerLevel; i++) {
		const s32 row = geRomTake(g_GeRomMen[(start + i) % g_GeRomNumMen], -1);

		g_ActiveMaleHeads[i] = row >= 0 ? row : HEAD_JAMIE;
		g_ActiveFemaleHeads[i] = female >= 0 ? female : HEAD_ANKA;
	}

	g_ActiveMaleHeadsIndex = 0;
	g_ActiveFemaleHeadsIndex = 0;

	// a hack's further pools, the same way: four men (or the hack's number)
	// from a start of the level's, one woman
	for (s32 k = 0; k < g_GeRomNumExtraPools; k++) {
		struct geromextrapool *pool = &g_GeRomExtraPools[k];

		for (s32 sex = 0; sex < 2; sex++) {
			const s32 want = sex == 0 ? g_GeRomMenPerLevel : 1;

			pool->numactive[sex] = 0;
			pool->next[sex] = -1;

			if (pool->num[sex] <= 0) {
				continue;
			}

			start = (s32)(rngRandom() % (u32)pool->num[sex]);

			for (s32 i = 0; i < want && i < pool->num[sex]; i++) {
				const s32 row = geRomTake(pool->heads[sex][(start + i) % pool->num[sex]], -1);

				if (row >= 0) {
					pool->active[sex][pool->numactive[sex]++] = (s16)row;
				}
			}
		}
	}
}

/* -------------------------------------------------------------------------
 * A converted mission's own text
 * ------------------------------------------------------------------------- */

/**
 * GoldenEye's objective and radio text is the level's own text bank, which the
 * converter copies into the mod's menu/ beside the briefing files (geconvert.c's
 * g_MenuText). That file is already a Perfect Dark language file byte for byte -
 * an offset table, then the strings, each ending in a newline - because the two
 * games share the format, so it is served as a language bank of its own rather
 * than converted into anything.
 *
 * A bank is what it has to be. A GoldenEye text id is `bank * 0x400 + slot`,
 * always the mission's own bank, so only the slot carries; the conversion
 * writes `LANGBANK_GEMISSION << 9 | slot` in its place, in an objective's
 * record and in the AI lists' radio messages, and langGet() then answers those
 * ids with nothing standing in the way. No mission's bank holds more than 108
 * strings, well inside the 9 bits Perfect Dark gives a slot.
 *
 * The buffer is the port's own and outlives the stage: langClearBank() at the
 * end of a level only drops the pointer, and setupLoadBriefing() would
 * otherwise leave the bank pointing into a scratch buffer.
 */
static u8 *g_GeMissionLang;
static char g_GeMissionLangBank[16];

/**
 * The selected language's text for a slot of the mission's own bank, keyed
 * ge.<bank>.<slot> by the bank file's name (langpack.h). NULL for GoldenEye's.
 */
const char *gexPlusMissionLangTr(s32 slot)
{
	return g_GeMissionLangBank[0] ? langpackGe(g_GeMissionLangBank, slot) : NULL;
}

void gexPlusMissionLangLoad(s32 stagenum)
{
	const s32 mission = modloaderStageMission(stagenum);
	const char *name = mission >= 0 ? geconvertMissionLangFile(mission) : NULL;
	const char *dir = modloaderGetStageModDir(stagenum);
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *data;

	langClearBank(LANGBANK_GEMISSION);
	g_GeMissionLangBank[0] = '\0';

	if (!name || !dir) {
		return;
	}

	// "LdamE" is ge.dam.<slot> to a language pack (langpack.h) - GoldenEye's
	// own text, which a ROM hack's bank of the same name is not
	if (modloaderStageIsGexPlus(stagenum)
			&& name[0] == 'L' && strlen(name) > 2 && strlen(name) - 2 < sizeof(g_GeMissionLangBank)) {
		memcpy(g_GeMissionLangBank, name + 1, strlen(name) - 2);
		g_GeMissionLangBank[strlen(name) - 2] = '\0';

		for (char *p = g_GeMissionLangBank; *p; p++) {
			*p = tolower((u8)*p);
		}
	}

	snprintf(path, sizeof(path), "%s/menu/%s", dir, name);

	data = fsFileLoad(path, &len);

	if (!data || len < 4) {
		sysLogPrintf(LOG_WARNING, "gexplus: mission %d has no text bank at %s", mission, path);

		if (data) {
			sysMemFree(data);
		}

		return;
	}

	// The offsets in the file are the ROM's 32-bit big-endian ones and langGet()
	// reads a bank's table at the width of a pointer, so it goes through the
	// same conversion a language file of the game's does. That widens the
	// table, so the buffer is sized as the loader would size it.
	{
		const u32 want = romdataFileGetEstimatedSize(len, LOADTYPE_LANG);
		u8 *bank = sysMemZeroAlloc(want);
		u32 banklen = 0;

		if (!bank) {
			sysMemFree(data);
			return;
		}

		memcpy(bank, data, len);
		sysMemFree(data);
		preprocessLangFile(bank, len, &banklen);

		if (g_GeMissionLang) {
			sysMemFree(g_GeMissionLang);
		}

		g_GeMissionLang = bank;
		g_LangBanks[LANGBANK_GEMISSION] = (uintptr_t *)bank;
	}
}

/* ---- the missions' animations ------------------------------------------ */

/**
 * menu/geanims.bin: "GEA1", a row count, then a row an animation - GoldenEye's
 * own id, the fields Perfect Dark's animation table wants, and where its bytes
 * are. The conversion writes the ones its missions' PlayAnimation commands
 * name (geconvert.c, geanimtable.h).
 */
#define GEANIM_ROW 20
#define GEANIM_MAX 512

// GoldenEye's animation id -> ours, or -1 where the conversion has none. The
// table runs past its 183 because GoldenEye's three **vehicle** animations take
// an id space of their own from GEVEH_ANIM_FIRST (256): animation_table_ptrs2[]
// shares its numbering with the guards' table and only an AI list's owner tells
// the two apart, so the conversion separates them here.
// One set per conversion the session has played a stage of: GoldenEye's own,
// and each ROM hack's, whose animation under an id can be its own -
// Goldfinger 64's and Tomorrow Never Dies 64's `bond_watch` (45) is Bond's
// whole body raising his wrist where GoldenEye's is the arm alone, and
// Tomorrow Never Dies 64 redid 129, 170 and 171.
#define GEANIM_SETS 8

static struct {
	char dir[FS_MAXPATH + 1];
	s16 anims[GEANIM_MAX];   // ours, or -1
	u32 sizes[GEANIM_MAX];   // the bytes behind it, for telling two sets' apart
} g_GeAnimSets[GEANIM_SETS];

static s32 g_GeNumAnimSets;

// lib/anim.c's: the rows animAppendExternal() fills, and the bytes behind them
extern struct animtableentry *g_RomAnims;
extern u8 **g_AnimReplacements;
static s16 *g_GeMissionAnims;
static s32 g_GeMissionAnimsLoaded;

/**
 * An animation an earlier set appended that is this one byte for byte - the
 * same id's in another conversion that kept GoldenEye's - or -1.
 */
static s32 gexPlusAnimShared(s32 id, const struct animtableentry *e, const u8 *data, u32 size)
{
	for (s32 k = 0; k < g_GeNumAnimSets; k++) {
		const s32 num = g_GeAnimSets[k].anims[id];

		if (num >= 0 && g_GeAnimSets[k].sizes[id] == size
				&& g_RomAnims[num].numframes == e->numframes
				&& g_RomAnims[num].bytesperframe == e->bytesperframe
				&& g_RomAnims[num].headerlen == e->headerlen
				&& g_RomAnims[num].framelen == e->framelen
				&& g_RomAnims[num].flags == e->flags
				&& g_AnimReplacements[num]
				&& memcmp(g_AnimReplacements[num], data, size) == 0) {
			return num;
		}
	}

	return -1;
}

/**
 * The mod's mission animations, appended after the game's own.
 *
 * A converted PlayAnimation carries **GoldenEye's** animation id, since the
 * number an appended animation takes is not known until it is appended
 * (animAppendExternal(), which is what a borrowed mod's animations do), so
 * aiChrDoAnimation() asks gexPlusMissionAnim() for ours.
 *
 * Read once a session for each conversion: an appended animation is permanent
 * - it counts as one of the ROM's and animsReset() keeps it - so only the
 * animations a conversion has that no set before it had the same are
 * appended; the rest share the earlier one's number. Once a session for all
 * conversions together, as it was, Cartel's ending (Goldfinger 64) played
 * GoldenEye's arm-only `bond_watch` on Bond's whole body after a GoldenEye
 * mission had loaded first, and he sank to his waist in the floor (F3
 * 20261004-171154). A stage whose mod has none (an arena of a conversion
 * older than its missions) keeps the set already in use.
 */
void gexPlusMissionAnimLoad(s32 stagenum)
{
	const char *dir = modloaderGetStageModDir(stagenum);
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *d;
	s32 numanims, appended = 0, shared = 0, set;

	if (!dir) {
		return;
	}

	for (set = 0; set < g_GeNumAnimSets; set++) {
		if (strcmp(g_GeAnimSets[set].dir, dir) == 0) {
			g_GeMissionAnims = g_GeAnimSets[set].anims;
			g_GeMissionAnimsLoaded = 1;
			return;
		}
	}

	if (g_GeNumAnimSets >= GEANIM_SETS) {
		return;
	}

	snprintf(path, sizeof(path), "%s/menu/geanims.bin", dir);
	d = fsFileSize(path) > 0 ? fsFileLoad(path, &len) : NULL;

	if (!d || len < 8 || memcmp(d, "GEA1", 4)) {
		sysLogPrintf(LOG_WARNING, "gexplus: the conversion has no mission animations at %s", path);
		sysMemFree(d);
		return;
	}

	numanims = (s32)((d[4] << 8) | d[5]);

	if (numanims < 0 || len < 8 + (u32)GEANIM_ROW * numanims) {
		sysMemFree(d);
		return;
	}

	set = g_GeNumAnimSets;
	snprintf(g_GeAnimSets[set].dir, sizeof(g_GeAnimSets[set].dir), "%s", dir);

	for (s32 i = 0; i < GEANIM_MAX; i++) {
		g_GeAnimSets[set].anims[i] = -1;
		g_GeAnimSets[set].sizes[i] = 0;
	}

	for (s32 r = 0; r < numanims; r++) {
		const u8 *row = d + 8 + GEANIM_ROW * r;
		const s32 id = (row[0] << 8) | row[1];
		const u32 at = ((u32)row[12] << 24) | (row[13] << 16) | (row[14] << 8) | row[15];
		const u32 size = ((u32)row[16] << 24) | (row[17] << 16) | (row[18] << 8) | row[19];
		struct animtableentry e;
		u8 *copy;
		s32 ours;

		if (id < 0 || id >= GEANIM_MAX || g_GeAnimSets[set].anims[id] >= 0 || !size || at + size > len) {
			continue;
		}

		e.numframes = (row[2] << 8) | row[3];
		e.bytesperframe = (row[4] << 8) | row[5];
		e.headerlen = (row[6] << 8) | row[7];
		e.framelen = row[8];

		// GoldenEye's own loop bit is Perfect Dark's ANIMFLAG_LOOP, which is
		// what wraps a frame past the end round to the front instead of
		// holding the last one - the walks and the runs all carry it
		e.flags = row[9] ? ANIMFLAG_LOOP : 0;
		e.data = 0;

		ours = gexPlusAnimShared(id, &e, d + at, size);

		if (ours >= 0) {
			g_GeAnimSets[set].anims[id] = (s16)ours;
			g_GeAnimSets[set].sizes[id] = size;
			shared++;
			continue;
		}

		// the header and the frames are read into the slot buffers the ROM's
		// own sizes made, and the bit reader runs off the end of the last frame
		copy = sysMemAlloc(size + 64);

		if (!copy) {
			continue;
		}

		memcpy(copy, d + at, size);
		memset(copy + size, 0, 64);

		ours = animAppendExternal(&e, copy);

		if (ours < 0) {
			sysLogPrintf(LOG_WARNING, "gexplus: no room for GoldenEye's animation %d", id);
			sysMemFree(copy);
			continue;
		}

		g_GeAnimSets[set].anims[id] = (s16)ours;
		g_GeAnimSets[set].sizes[id] = size;
		appended++;
	}

	sysMemFree(d);

	g_GeNumAnimSets++;
	g_GeMissionAnims = g_GeAnimSets[set].anims;
	g_GeMissionAnimsLoaded = 1;

	sysLogPrintf(LOG_NOTE, "gexplus: %d of GoldenEye's own animations appended for %s, %d shared with a conversion before it",
			appended, dir, shared);
}

/**
 * Ours for a converted PlayAnimation's animation id.
 *
 * The conversion writes GoldenEye's own id with GEAI_ANIM_TAG set, and an id
 * without it belongs to Perfect Dark and is left alone - a mission's chrs do
 * fall back on the game's own ailists, whose aiChrDoAnimation carries Perfect
 * Dark's own numbers, and one of those would otherwise be read as GoldenEye's.
 *
 * A tagged id the conversion has no animation for plays animation 0 rather
 * than whatever number it happens to be, since that number means nothing here.
 */
s32 gexPlusMissionAnim(s32 geid)
{
	if (!(geid & GEAI_ANIM_TAG)) {
		return geid;
	}

	geid &= GEAI_ANIM_TAG - 1;

	if (!g_GeMissionAnimsLoaded || geid >= GEANIM_MAX || g_GeMissionAnims[geid] < 0) {
		return 0;
	}

	return g_GeMissionAnims[geid];
}

/**
 * A converted mission's props, once, before anything has read them.
 *
 * **A chr's body** is GoldenEye's own character number, which becomes the row
 * its converted model took (geRomBodyRow()), or - where the conversion is an old
 * one with no characters in it - the release's character, GoldenEye X's or
 * Perfect Dark's nearest. Its head is always -1 from here, because a row past
 * 127 does not fit the field: bodyChooseHead() answers for it, out of
 * GoldenEye's own pool or with the head this body's record named
 * (gexPlusRomOwnHead()).
 *
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

	// a mission that starts afresh is not ending
	g_GeExitState = 0;

	geRomLoadTable(g_Vars.stagenum);
	geRomReleaseRows();

	if (!obj) {
		return;
	}

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_CHR) {
			struct packedchr *chr = (struct packedchr *)obj;
			const s32 row = geRomBodyRow(chr->bodynum, (s8)chr->headnum);

			chr->bodynum = row >= 0 ? row : gexPlusBodyForGe(chr->bodynum);
			chr->headnum = -1;
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}
}

/**
 * And the bodies a converted mission's lists spawn, once its ailists have been
 * pointed at themselves (setupLoadFiles).
 *
 * aiSpawnChrAtPad and aiSpawnChrAtChr carry a body and a head of their own -
 * GoldenEye's TRYSpawningChrAtPad and TRYSpawningChrNextToChr, which its
 * missions use 162 times over the twenty: Statue Park's twenty Janus troops,
 * Facility's scientists and Doctor Doak, Control's sixteen commandos. Left as
 * GoldenEye's own numbers they spawned whatever Perfect Dark's body of that
 * number happens to be, so they are mapped here exactly as a record's chr is.
 */
void gexPlusMissionAilists(void)
{
	struct ailist *lists = g_StageSetup.ailists;

	if (!lists) {
		return;
	}

	for (s32 i = 0; lists[i].list; i++) {
		u8 *cmd = lists[i].list;
		// a list that lost its way out to the command map would otherwise walk
		// the heap; chrai.c bounds its own run of one for the same reason
		s32 steps = 0;

		while (steps++ < 100000) {
			const s32 type = (cmd[0] << 8) | cmd[1];

			if (type == AICMD_SPAWNCHRATPAD || type == AICMD_SPAWNCHRATCHR) {
				const s32 row = geRomBodyRow(cmd[2], (s8)cmd[3]);

				// Never left as it came: GoldenEye's number is a row of
				// Perfect Dark's table too, and not a body's necessarily -
				// its 12 is Baron Samedi and ours is Joanna's head, which has
				// no root matrix and ended the game the first frame it was on
				// screen (chrTestHit()). So the same fallback a record gets.
				cmd[2] = (u8)(row >= 0 ? row : gexPlusBodyForGe(cmd[2]));
				cmd[3] = 0xff;
			}

			if (type == AICMD_END) {
				break;
			}

			cmd += chraiGetCommandLength(cmd, 0);
		}
	}
}

/**
 * GoldenEye's own end of a mission: the list says the level is over, and the
 * next button press fades the screen out and leaves.
 *
 * GoldenEye holds this in `stop_time_flag` (bondview2.c): the command sets it,
 * a press takes it to 2 and starts a one-second fade to black, and when the
 * fade is done the level ends. Perfect Dark's own aiEndLevel goes at once,
 * with no wait and no fade, so the wait is kept here - GoldenEye leaves its
 * mission text on the screen until the player is ready, and a list goes on
 * running after the command (Dam swings the camera round onto Bond).
 */
#define GE_EXIT_BUTTONS (A_BUTTON | B_BUTTON | Z_TRIG | START_BUTTON | R_TRIG | L_TRIG)
#define GE_EXIT_FADE60 60

void gexPlusExitOnButtonPress(void)
{
	// GoldenEye only takes the first: "if (stop_time_flag == FALSE)"
	if (g_GeExitState == 0) {
		g_GeExitState = 1;
	}
}

void gexPlusMissionExitTick(void)
{
	if (g_GeExitState == 0) {
		return;
	}

	// nothing carries over out of the mission that asked
	if (!modloaderStageIsMission(g_Vars.stagenum)) {
		g_GeExitState = 0;
		return;
	}

	if (g_GeExitState == 1) {
		const s8 contpad = optionsGetContpadNum1(g_Vars.currentplayerstats
				? g_Vars.currentplayerstats->mpindex : 0);

		if (joyGetButtonsPressedThisFrame(contpad, GE_EXIT_BUTTONS)) {
			g_GeExitState = 2;
			g_GeExitFade60 = GE_EXIT_FADE60 + 2;   // the two frames lvConfigureFade waits
			lvConfigureFade(0x000000ff, GE_EXIT_FADE60);   // GoldenEye's own second, to black
		}

		return;
	}

	// The second is counted here rather than taken from lvIsFadeActive(),
	// because Perfect Dark advances a fade while it *draws* it (lvRenderFade)
	// and the list that asked has usually put the camera somewhere the HUD is
	// not drawn from - Dam swings it onto Bond - so the fade can sit at a
	// fraction of itself for ever and the level would never end. GoldenEye
	// advances its own on a tick.
	g_GeExitFade60 -= g_Vars.diffframe60f;

	if (g_GeExitFade60 <= 0) {
		g_GeExitState = 0;

		// an ending the Cinema page is playing goes back to the page
		if (!gecinemaEndingOver()) {
			func0000e990();
		}
	}
}



#ifndef PLATFORM_N64
/* ---- GoldenEye's guns, and only those ----------------------------------- */

/**
 * GE Plus is played with GoldenEye's guns. Its arenas list GoldenEye's own
 * fourteen weapon sets, which the conversion reads out of the player's ROM
 * (menu/gesets.bin, geconvert.c's writeWeaponSets()), and with no conversion
 * the sets borrowed from GoldenEye X; Perfect Dark's sets are Perfect Dark's
 * guns, and are listed beside them only when Mod.GePlusPdGuns asks for them
 * ("Include Perfect Dark Guns", off unless the player turns it on).
 *
 * A GoldenEye ROM hack's mode (g_GexPlusVariant: Goldfinger 64) lists the
 * hack's own sets, out of its own conversion: Goldfinger's are not
 * GoldenEye's ("Hats 'n' Clubs", "Gangster"), and four of them hand out its
 * own pistols. They take GoldenEye's rows of the list while its mode is
 * chosen rather than rows of their own after them, which Perfect Dark's
 * Combat Simulator, listing the whole list, would offer as well: a block of
 * the list is the mode's, GoldenEye's whenever no hack's is chosen.
 */
#define GESETS_NUM    14
#define GESETS_SLOTS  8
#define GESETS_NAME   32
#define GESETS_ROW    (GESETS_NAME + GESETS_SLOTS)
#define GESETS_GROUPS 4

extern s32 g_MpWeaponSetNum;

struct gesetsgroup {
	char dir[FS_MAXPATH + 1]; // the conversion's mod dir; "" a free group
	s32 num;
	struct mpweaponset sets[GESETS_NUM];
	char text[GESETS_NUM][GESETS_NAME + 2];
};

static s32 g_GePlusPdGuns = 0;
static struct gesetsgroup g_GeSetsGroups[GESETS_GROUPS];
static s32 g_GeSetsFirst = -1; // the block of the list
static s32 g_GeSetsNum = 0;
static s32 g_GeSetsIn = -1; // whose group the block holds

PD_CONSTRUCTOR static void gexPlusGunsConfigInit(void)
{
	configRegisterInt("Mod.GePlusPdGuns", &g_GePlusPdGuns, 0, 1);
}

s32 gexPlusGetPdGuns(void)
{
	return g_GePlusPdGuns;
}

void gexPlusSetPdGuns(s32 on)
{
	g_GePlusPdGuns = on ? 1 : 0;
}

/** Whether the block is still where it was put: a mod swap puts the list back. */
static s32 geSetsInList(void)
{
	const struct gesetsgroup *in = g_GeSetsIn >= 0 ? &g_GeSetsGroups[g_GeSetsIn] : NULL;

	return in && g_GeSetsFirst >= 0 && g_GeSetsNum == in->num
		&& g_GeSetsFirst + g_GeSetsNum <= g_MpNumWeaponSets
		&& g_MpWeaponSets[g_GeSetsFirst].name == in->sets[0].name
		&& g_MpWeaponSets[g_GeSetsFirst + g_GeSetsNum - 1].name == in->sets[g_GeSetsNum - 1].name;
}

/**
 * The mod dir whose sets are listed: the first arena's of GE Plus's list
 * (modloaderStageInGexPlusList(): GoldenEye's, or the chosen hack's), or of
 * GoldenEye's own for `own`; NULL when there is none.
 */
static const char *geSetsDir(s32 own)
{
	for (s32 i = 0; i < mpGetNumStages(); i++) {
		const s32 stagenum = g_MpArenas[i].stagenum;

		if (own ? modloaderStageIsGexPlus(stagenum) : modloaderStageInGexPlusList(stagenum)) {
			return modloaderGetStageModDir(stagenum);
		}
	}

	return NULL;
}

/** Mod dir `dir`'s sets, read the first time they are asked for; -1 where it has none. */
static s32 geSetsGroup(const char *dir)
{
	char path[FS_MAXPATH + 1];
	struct gesetsgroup *group = NULL;
	s32 index = -1;
	u32 len = 0;
	u8 *d;
	s32 num;

	if (!dir) {
		return -1;
	}

	for (s32 i = 0; i < GESETS_GROUPS; i++) {
		if (g_GeSetsGroups[i].dir[0] && !strcmp(g_GeSetsGroups[i].dir, dir)) {
			return i;
		}

		if (index < 0 && !g_GeSetsGroups[i].dir[0]) {
			index = i;
		}
	}

	if (index < 0) {
		return -1;
	}

	snprintf(path, sizeof(path), "%s/menu/gesets.bin", dir);
	d = fsFileLoad(path, &len);

	if (!d || len < 8 || memcmp(d, "GES1", 4)) {
		sysMemFree(d);
		return -1;
	}

	num = (s32)((d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]);

	if (num > GESETS_NUM) {
		num = GESETS_NUM;
	}

	if (num <= 0 || len < 8 + (u32)GESETS_ROW * num) {
		sysMemFree(d);
		return -1;
	}

	group = &g_GeSetsGroups[index];
	memset(group, 0, sizeof(*group));
	snprintf(group->dir, sizeof(group->dir), "%s", dir);

	for (s32 i = 0; i < num; i++) {
		const u8 *row = d + 8 + GESETS_ROW * i;
		struct mpweaponset *set = &group->sets[i];

		// GoldenEye's eight slots run from its lightest gun to its heaviest
		// with most of them said twice; Perfect Dark's six take them evenly
		for (s32 j = 0; j < NUM_MPWEAPONSLOTS; j++) {
			const u8 w = row[GESETS_NAME + (j * (GESETS_SLOTS - 1) + (NUM_MPWEAPONSLOTS - 1) / 2) / (NUM_MPWEAPONSLOTS - 1)];

			// Slappers Only hands out nothing at all
			set->slots[j] = w >= WEAPON_GE_FIRST ? w : WEAPON_DISABLED;
		}

		set->unk0c = set->slots[0];
		set->unk0d = set->slots[1];
		set->unk0e = set->slots[2];
		set->unk0f = set->slots[3];
		set->unk10 = set->slots[4];
		set->unk11 = set->slots[5];

		memcpy(group->text[i], row, GESETS_NAME);
		group->text[i][GESETS_NAME - 1] = '\0';
		strcat(group->text[i], "\n"); // as the game's own set names end
		set->name = langAddPortText(group->text[i]);
	}

	group->num = num;
	sysMemFree(d);

	return index;
}

/**
 * Group `index`'s sets into the block: over the one there when it is the
 * same size or the list's last, after the list otherwise. A set chosen out of
 * the block is chosen again as the row it was, whose guns are the group's.
 */
static void geSetsPut(s32 index)
{
	const struct gesetsgroup *group = &g_GeSetsGroups[index];
	const s32 inlist = geSetsInList();
	const s32 last = inlist && g_GeSetsFirst + g_GeSetsNum == g_MpNumWeaponSets;
	s32 first = g_MpNumWeaponSets;

	if (inlist && g_GeSetsIn == index) {
		return;
	}

	if (inlist && (group->num == g_GeSetsNum || last)) {
		first = g_GeSetsFirst;
	}

	if (first + group->num > MP_MAX_WEAPONSETS) {
		sysLogPrintf(LOG_WARNING, "gexplus: the weapon sets of %s do not fit the list (%d, %d in it)", group->dir, group->num, first);
		return;
	}

	memcpy(&g_MpWeaponSets[first], group->sets, sizeof(group->sets[0]) * group->num);

	if (first + group->num > g_MpNumWeaponSets || (last && first == g_GeSetsFirst)) {
		g_MpNumWeaponSets = first + group->num;
	}

	g_GeSetsFirst = first;
	g_GeSetsNum = group->num;
	g_GeSetsIn = index;

	if (inlist && g_MpWeaponSetNum >= first && g_MpWeaponSetNum < first + g_GeSetsNum) {
		mpApplyWeaponSet();
	}

	sysLogPrintf(LOG_NOTE, "gexplus: %d weapon sets of %s in the list at %d", g_GeSetsNum, group->dir, first);
}

/** The mode's sets in the block (GoldenEye's own for `own`): whether they are. */
static s32 geSetsUse(s32 own)
{
	const s32 index = geSetsGroup(geSetsDir(own));

	if (index >= 0) {
		geSetsPut(index);
	}

	return index >= 0 && g_GeSetsIn == index && geSetsInList();
}

/**
 * GoldenEye's own sets in the whole Combat Simulator's list too, not only in
 * GE Plus's: its guns are offered on any arena beside Perfect Dark's, and
 * until GE Plus had been opened once in a session the sets that hand them out
 * were nowhere. Those used to be GoldenEye X's, borrowed at boot; with it
 * dormant (modborrow.c) these are the ROM's. At boot, after a mod swap, which
 * puts the list back, and on leaving GE Plus's Combat Simulator, whose block
 * a hack's mode had; not under a mod with its own weapon list, which hides
 * GoldenEye's guns (gebeanGunsRefresh()).
 */
void gexPlusWeaponSetsAppend(void)
{
	if (!modDataMpWeaponsImported()) {
		geSetsUse(1);
	}
}

/**
 * The weapon sets GE Plus lists: how many, and the list index of the first.
 * GoldenEye's own out of the ROM, and never GoldenEye X's (which it fell back
 * to until 2026-09-26); 0 when there are none, when the player asked for
 * Perfect Dark's guns too, or under a mod with its own weapon list - the
 * whole list then.
 */
s32 gexPlusWeaponSets(s32 *first)
{
	// A mod that brings its own weapon list (GoldenEye X 6a) keeps it as it
	// made it: GE Plus stays out, and its menus offer the mod's list whole
	// (the user's call on board report 20261001-034632). They used to append
	// GoldenEye's fourteen sets under the mod's own here, whose GoldenEye guns
	// gebeanGunsRefresh() hides under such a mod.
	if (modDataMpWeaponsImported()) {
		return 0;
	}

	if (g_GePlusPdGuns) {
		// still put the mode's in the list the whole of which is shown
		geSetsUse(0);
		return 0;
	}

	if (geSetsUse(0)) {
		*first = g_GeSetsFirst;
		return g_GeSetsNum;
	}

	return 0;
}
#endif

/**
 * The explosion a converted GoldenEye prop makes when it is destroyed.
 *
 * Both games look it up by model: Perfect Dark in g_PropExplosionTypes[], which
 * stops at its own models, GoldenEye in object_explosion_details[] by its prop
 * number - and a converted prop's model is MODEL_REMAKE_FIRST plus that number,
 * past the end of Perfect Dark's table, so every crate, drum and vehicle on a
 * converted level was destroyed with EXPLOSIONTYPE_NONE: nothing drawn, nothing
 * hurt. The two games' explosion types are the same rows 0 to 20, so GoldenEye's
 * number is used as it stands. -1 is a model that is not one of GoldenEye's.
 */
/**
 * A GoldenEye ROM hack's own object_explosion_details (menu/geexplosions.bin,
 * converter 98) - its props are not GoldenEye's numbers (Goldfinger 64 renamed
 * 318 of the 340 and added 76) - for the stage's own mod, or NULL on
 * GoldenEye's, whose table is built in.
 */
static const u8 *gexPlusHackExplosions(u32 *numprops)
{
	static u8 *table;
	static u32 len;
	static s32 tablemoddir = -2;
	const s32 moddir = modloaderGetStageModDirIndex(g_Vars.stagenum);

	if (moddir < 0 || modloaderStageIsGexPlus(g_Vars.stagenum) || !modloaderStageIsRemake(g_Vars.stagenum)) {
		return NULL;
	}

	if (tablemoddir != moddir) {
		char path[FS_MAXPATH + 1];
		const char *dir = fsGetModDirAt(moddir);

		sysMemFree(table);
		table = NULL;
		len = 0;
		tablemoddir = moddir;

		if (dir) {
			snprintf(path, sizeof(path), "%s/menu/geexplosions.bin", dir);

			if (fsFileSize(path) > 0) {
				table = fsFileLoad(path, &len);
			}
		}
	}

	*numprops = len / 14;

	return table;
}

s32 gexPlusPropExplosionType(s32 modelnum)
{
	static const u8 types[] = {
#include "geexplosiontypes.h"
	};
	const s32 prop = modelnum - MODEL_REMAKE_FIRST;
	u32 numprops = 0;
	const u8 *hack = gexPlusHackExplosions(&numprops);

	if (hack) {
		return prop >= 0 && (u32)prop < numprops ? (s16)((hack[14 * prop] << 8) | hack[14 * prop + 1]) : -1;
	}

	if (prop < 0 || prop >= (s32)ARRAYCOUNT(types)) {
		return -1;
	}

	return types[prop];
}

/**
 * GoldenEye's seed for crumpling a converted prop (objDeform()), by its prop
 * number and the seed's index: the destroyed level, or the level plus three,
 * whichever a coin toss picks. GoldenEye has six a prop and reads a seventh
 * past the end at level 3 and up; that, a model that is not GoldenEye's, and
 * a prop without seeds are 0, which is "take a random one" in both games.
 */
u16 gexPlusPropDeformSeed(s32 modelnum, s32 index)
{
	static const u16 seeds[][6] = {
#include "geexplosionseeds.h"
	};
	const s32 prop = modelnum - MODEL_REMAKE_FIRST;
	u32 numprops = 0;
	const u8 *hack = gexPlusHackExplosions(&numprops);

	if (hack) {
		return prop >= 0 && (u32)prop < numprops && index >= 0 && index < 6
			? (u16)((hack[14 * prop + 2 + 2 * index] << 8) | hack[14 * prop + 3 + 2 * index]) : 0;
	}

	if (prop < 0 || prop >= (s32)ARRAYCOUNT(seeds) || index < 0 || index >= 6) {
		return 0;
	}

	return seeds[prop][index];
}

/**
 * Perfect Dark's psychosis gun on a converted GoldenEye mission's guard.
 *
 * The gun only sets CHRHFLAG_PSYCHOSISED (chraction.c). What turns the guard
 * is his AI list: Perfect Dark's global lists (unalerted, alerted, the shot
 * lists) look for the flag and hand over to GAILIST_INIT_PSYCHOSIS, which
 * puts him on the player's team and follows her. A converted mission's guards
 * run GoldenEye's own lists, converted, and nothing in GoldenEye knows the
 * flag, so the guard took the dart and carried on shooting at Bond (F3
 * 20260930-211331, Silo).
 *
 * So the hit hands him over itself, as the global lists would have. And the
 * teams: the converter leaves every guard on team 0, which is in none of
 * teamGetChrIds()' lists, so a turned guard's "nearest enemy" found nobody
 * to fight. The armed chrs that are not civilians go to TEAM_ENEMY, where a
 * Perfect Dark level's guards are; the unarmed (scientists, Natalya, Boris)
 * and the civilians stay out of it, as GoldenEye's guards never fought them.
 *
 * Returns whether it took the chr over.
 */
s32 gexPlusPsychosis(struct chrdata *chr)
{
	u8 *ailist;
	s32 i;

	if (!modloaderStageIsMission(g_Vars.stagenum) || g_Vars.normmplayerisrunning
			|| chr->aibot || chr->prop == NULL || chrIsDead(chr)
			|| (chr->hidden & CHRHFLAG_PSYCHOSISED)) {
		return false;
	}

	ailist = ailistFindById(GAILIST_INIT_PSYCHOSIS);

	if (ailist == NULL) {
		return false;
	}

	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *other = &g_ChrSlots[i];

		if (other->chrnum >= 0 && other->prop && other != chr && other->team == TEAM_00
				&& other->aibot == NULL
				&& (other->prop->type == PROPTYPE_CHR)
				&& (other->chrflags & CHRCFLAG_KILLCOUNTABLE) == 0
				&& (chrGetHeldProp(other, HAND_RIGHT) || chrGetHeldProp(other, HAND_LEFT))) {
			other->team = TEAM_ENEMY;
		}
	}

	chr->hidden |= CHRHFLAG_PSYCHOSISED;
	chr->ailist = ailist;
	chr->aioffset = 0;
	chr->aireturnlist = GAILIST_PSYCHOSISED;
	chr->sleep = 0;

	// the hit is about to trigger the shot list, which is GoldenEye's
	chr->aishotlist = GAILIST_INIT_PSYCHOSIS;

	return true;
}

/**
 * GoldenEye MP pairs a gun only off a linked prop: its pickup gives the pair
 * when the prop it touches has a partner, and the same gun again from another
 * pad is ammo (F3 20261004-023322: our GoldenEye arenas kept Perfect Dark's
 * rule that a second pad's copy makes a pair, players in inv.c and sims in
 * bot.c alike). A converted arena of GoldenEye's or a ROM hack's, in any
 * Combat Simulator; never Perfect Dark's own nor a PD mod's (GoldenEye X's).
 */
s32 gexPlusArenaPairsLinkedOnly(void)
{
	return g_Vars.normmplayerisrunning
		&& modloaderDirIndexIsConversion(modloaderGetStageModDirIndex(g_Vars.stagenum));
}

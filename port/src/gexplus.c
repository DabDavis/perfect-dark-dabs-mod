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
#include <string.h>
#include <stdio.h>
#include "modborrow.h"
#include "gebean.h"
#include "geconvert.h"
#include "geaitable.h"
#include "preprocess.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "game/lang.h"
#include "lib/anim.h"
#include "game/mplayer/mplayer.h"
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
 * takes a row under 256 and a mission that cannot get one falls back on
 * whatever is installed. A head's row is never written into a record -
 * GoldenEye's own setups leave all but two heads at -1 and let bodyChooseHead()
 * pick, exactly as Perfect Dark's own do - so heads take rows past 255, where
 * there is always room.
 */
#define GEROM_NUM_CHRS    80
#define GEROM_FIRST_HEAD  42
#define GEROM_ROWLEN      12
#define GEROM_BODY_FIRST  152   // the pool rows, straight after the stock table
#define GEROM_BODY_LAST   255   // and the last row a packedchr's u8 bodynum reaches
#define GEROM_HEAD_FIRST  256
#define GEROM_MAX_ROWS    24

// c_item_entries' two flags, as the conversion writes them
#define GEROM_MALE        0x1
#define GEROM_HASHEAD     0x2

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
// `+ (random & 3)`), and one female head for all of it
#define GEROM_MALE_HEADS_PER_LEVEL 4

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
static s32 g_GeRomTableModDir = -2;   // the mod its table was read from
static struct geromrow g_GeRomRows[GEROM_MAX_ROWS];
static s32 g_GeRomNumRows;

/**
 * menu/gechrs.bin for the mod a stage belongs to, once.
 */
static s32 geRomLoadTable(s32 stagenum)
{
	const s32 moddir = modloaderGetStageModDirIndex(stagenum);
	const char *dir = modloaderGetStageModDir(stagenum);
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *d;
	s32 numchrs;

	if (moddir < 0 || !dir) {
		return 0;
	}

	if (g_GeRomTableModDir == moddir) {
		return g_GeRomNumChrs > 0;
	}

	g_GeRomTableModDir = moddir;
	g_GeRomNumChrs = 0;

	snprintf(path, sizeof(path), "%s/menu/gechrs.bin", dir);
	d = fsFileLoad(path, &len);

	if (!d || len < 8 || memcmp(d, "GEC1", 4)) {
		sysLogPrintf(LOG_WARNING, "gexplus: the conversion has no characters at %s", path);
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

		g_GeRomChrs[i].flags = row[3];
		bits = ((u32)row[4] << 24) | (row[5] << 16) | (row[6] << 8) | row[7];
		memcpy(&g_GeRomChrs[i].scale, &bits, sizeof(f32));
		bits = ((u32)row[8] << 24) | (row[9] << 16) | (row[10] << 8) | row[11];
		memcpy(&g_GeRomChrs[i].pov, &bits, sizeof(f32));
	}

	sysMemFree(d);
	g_GeRomNumChrs = numchrs;

	return 1;
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
 * The row GoldenEye's character `num` wears in this mission, taking one and
 * filling it out of the ROM's own table the first time it is asked for. -1
 * where there is no row left to take, or no such character.
 *
 * `ownhead` is the row of the head a body is to wear where a record named one,
 * and -1 where it takes the pool's - a body wanted both ways takes a row each
 * way, so that Facility's fifteen scientists still get GoldenEye's own faces
 * while Doctor Doak, who is the same body, keeps his.
 *
 * A row starts as one of Perfect Dark's own - the dataDyne guard for a man, the
 * Institute's female technician for a woman, a stock head for a head - with the
 * model file and the two scales replaced: the rest of a row is what the game
 * asks of any body (its race, its hands, whether its height varies) and a
 * converted character answers the same way.
 */
static s32 geRomTake(s32 num, s32 ownhead)
{
	const s32 ishead = num >= GEROM_FIRST_HEAD;
	char name[16];
	struct headorbody *hb;
	const struct headorbody *host;
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

	for (s32 i = ishead ? GEROM_HEAD_FIRST : GEROM_BODY_FIRST;
			i <= (ishead ? NUM_HEADSANDBODIES - 1 : GEROM_BODY_LAST); i++) {
		if (!g_HeadsAndBodies[i].filenum) {
			row = i;
			break;
		}
	}

	if (row < 0) {
		sysLogPrintf(LOG_WARNING, "gexplus: no row left for GoldenEye's character %d", num);
		return -1;
	}

	host = &g_HeadsAndBodies[ishead
			? ((g_GeRomChrs[num].flags & GEROM_MALE) ? HEAD_JAMIE : HEAD_ANKA)
			: ((g_GeRomChrs[num].flags & GEROM_MALE) ? BODY_DD_GUARD : BODY_CIFEMTECH)];
	hb = &g_HeadsAndBodies[row];
	*hb = *host;
	hb->filenum = (u16)fileid;
	hb->modeldef = NULL;
	hb->scale = g_GeRomChrs[num].scale;
	hb->animscale = g_GeRomChrs[num].pov;
	hb->ismale = (g_GeRomChrs[num].flags & GEROM_MALE) != 0;
	// GoldenEye's own hasHead, which its retrieve_header_for_body_and_head()
	// tests before it looks for a head at all
	hb->unk00_01 = (g_GeRomChrs[num].flags & GEROM_HASHEAD) != 0;
	// a guard is the height GoldenEye modelled it at: nothing in its own
	// makeonebody() varies one
	hb->canvaryheight = 0;

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

	if (head >= GEROM_FIRST_HEAD) {
		ownhead = geRomTake(head, -1);
	}

	return geRomTake(body, ownhead);
}

/**
 * The head a converted mission's body wears where its record named one rather
 * than taking GoldenEye's pool: Facility's Doctor Doak and Statue Park's
 * Mishkin are the two in the twenty missions, and -1 is every other body.
 *
 * bodyChooseHead() asks, which is where both a setup's chr and aiSpawnChrAtPad
 * arrive once the head they carry is -1 - and it has to be -1, since neither
 * field is wide enough for a row past 127.
 */
s32 gexPlusRomOwnHead(s32 bodynum)
{
	for (s32 i = 0; i < g_GeRomNumRows; i++) {
		if (g_GeRomRows[i].row == bodynum) {
			return g_GeRomRows[i].ownhead;
		}
	}

	return -1;
}

/**
 * Whether a row is one this holds, which is what keeps headfit.c off a pair
 * GoldenEye made for itself: its heads sit on its own bodies' headspots as they
 * are, and a neck measured between two of them would move one that fits.
 */
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

	start = (s32)(rngRandom() % ARRAYCOUNT(g_GeRomMaleHeads));
	female = geRomTake(g_GeRomFemaleHeads[rngRandom() % ARRAYCOUNT(g_GeRomFemaleHeads)], -1);

	g_NumActiveHeadsPerGender = GEROM_MALE_HEADS_PER_LEVEL;

	for (s32 i = 0; i < GEROM_MALE_HEADS_PER_LEVEL; i++) {
		const s32 row = geRomTake(g_GeRomMaleHeads[(start + i) % ARRAYCOUNT(g_GeRomMaleHeads)], -1);

		g_ActiveMaleHeads[i] = row >= 0 ? row : HEAD_JAMIE;
		g_ActiveFemaleHeads[i] = female >= 0 ? female : HEAD_ANKA;
	}

	g_ActiveMaleHeadsIndex = 0;
	g_ActiveFemaleHeadsIndex = 0;
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

void gexPlusMissionLangLoad(s32 stagenum)
{
	const s32 mission = modloaderStageMission(stagenum);
	const char *name = mission >= 0 ? geconvertMissionLangFile(mission) : NULL;
	const char *dir = modloaderGetStageModDir(stagenum);
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *data;

	langClearBank(LANGBANK_GEMISSION);

	if (!name || !dir) {
		return;
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
#define GEANIM_MAX 256

// GoldenEye's animation id -> ours, or -1 where the conversion has none
static s16 g_GeMissionAnims[GEANIM_MAX];
static s32 g_GeMissionAnimsLoaded;

/**
 * The mod's mission animations, appended after the game's own.
 *
 * A converted PlayAnimation carries **GoldenEye's** animation id, since the
 * number an appended animation takes is not known until it is appended
 * (animAppendExternal(), which is what a borrowed mod's animations do), so
 * aiChrDoAnimation() asks gexPlusMissionAnim() for ours.
 *
 * Read once a session: an appended animation is permanent - it counts as one
 * of the ROM's and animsReset() keeps it - so loading the file again on the
 * next mission would only spend the thousand rows there are.
 */
void gexPlusMissionAnimLoad(s32 stagenum)
{
	const char *dir = modloaderGetStageModDir(stagenum);
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *d;
	s32 numanims, appended = 0;

	if (g_GeMissionAnimsLoaded || !dir) {
		return;
	}

	g_GeMissionAnimsLoaded = 1;

	for (s32 i = 0; i < GEANIM_MAX; i++) {
		g_GeMissionAnims[i] = -1;
	}

	snprintf(path, sizeof(path), "%s/menu/geanims.bin", dir);
	d = fsFileLoad(path, &len);

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

	for (s32 r = 0; r < numanims; r++) {
		const u8 *row = d + 8 + GEANIM_ROW * r;
		const s32 id = (row[0] << 8) | row[1];
		const u32 at = ((u32)row[12] << 24) | (row[13] << 16) | (row[14] << 8) | row[15];
		const u32 size = ((u32)row[16] << 24) | (row[17] << 16) | (row[18] << 8) | row[19];
		struct animtableentry e;
		u8 *copy;
		s32 ours;

		if (id < 0 || id >= GEANIM_MAX || g_GeMissionAnims[id] >= 0 || !size || at + size > len) {
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

		g_GeMissionAnims[id] = (s16)ours;
		appended++;
	}

	sysMemFree(d);
	sysLogPrintf(LOG_NOTE, "gexplus: %d of GoldenEye's own animations, appended", appended);
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

	if (!lists || g_GeRomNumChrs <= 0) {
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

				if (row >= 0) {
					cmd[2] = (u8)row;
					cmd[3] = 0xff;
				}
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
		func0000e990();
	}
}


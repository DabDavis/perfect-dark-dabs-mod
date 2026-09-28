#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "gexplus.h"
#include "geaitable.h"
#include "gechranims.h"
#include "gechranimtable.h"
#include "system.h"
#include "config.h"
#include "game/race.h"
#include "lib/anim.h"

/**
 * GoldenEye's own animations on a converted level's characters (gechranims.h).
 *
 * The numbers were matched two ways: GoldenEye's chr.c and Perfect Dark's
 * chraction.c hold the same attack, hit and death tables row for row, and
 * where a row's frame numbers, speeds and angles are the same the two
 * animations are the same one (only ANIM_0032 is GoldenEye's fire_standing
 * under another number, and ANIM_TWO_GUN_HOLD its idle); and the frame
 * counts of the two ROMs' animations, which agree for all but the rows below.
 *
 * Left alone are the rows Perfect Dark filled with something of its own and
 * reads for its own purposes - 22-31 are its hit-while-walking and -running
 * animations where GoldenEye's spin deaths were, 97-105 its dodges, 152-163
 * the idle fidgets its own AI lists play, 176-182 its extra flinches - since
 * those numbers still mean Perfect Dark's animation to the code naming them.
 * GoldenEye's own versions of them are played under their appended numbers:
 * its hit and death tables are taken whole (g_GeHitReactions).
 */

s32 g_GeChrAnims;

// our row, GoldenEye's id
static const u8 g_Moved[][2] = {
	{ 1, 0 },   // ANIM_TWO_GUN_HOLD: idle
	{ 50, 1 },  // ANIM_0032: fire_standing
};

static s32 geChrAnimIsOurs(s32 geid)
{
	if (geid >= 22 && geid <= 31 && geid != 23 && geid != 25 && geid != 26 && geid != 28) {
		return 0;
	}

	if (geid >= 97 && geid <= 105) {
		return 0;
	}

	if (geid >= 152 && geid <= 163 && geid != 153 && geid != 159 && geid != 161) {
		return 0;
	}

	return geid >= 2 && geid < 176;
}

s32 geChrAnim(s32 geid)
{
	s32 ours = gexPlusMissionAnim(GEAI_ANIM_TAG | geid);

	return ours > 0 ? ours : -1;
}

/* ---- GoldenEye's hit and death tables ----------------------------------- */

#define GE_MAX_ROWS 320

static struct animtablerow g_GeRows[GE_MAX_ROWS];
static struct animtable g_GeTables[ARRAYCOUNT(g_GeHitReactions) + 3];
static struct animtablerow g_GeStagger[2];
static struct animtable *g_PdHumanTables;

static s32 geChrAnimsRows(const struct gestruck *src, s32 count, s32 *used, struct animtablerow **out)
{
	if (*used + count + 1 > GE_MAX_ROWS) {
		return 0;
	}

	*out = &g_GeRows[*used];

	for (s32 i = 0; i < count; i++) {
		struct animtablerow *row = &g_GeRows[*used + i];
		s32 ours = geChrAnim(src[i].geid);

		if (ours < 0) {
			return 0;
		}

		row->animnum = ours;
		row->flip = src[i].flip;
		row->endframe = src[i].endframe;
		row->speed = src[i].speed;
		row->unk10 = src[i].knockback;
		row->thudframe1 = src[i].thud1;
		row->thudframe2 = src[i].thud2;
	}

	// and the empty row the game's own lists end on
	g_GeRows[*used + count].animnum = 0;
	*used += count + 1;

	return 1;
}

/**
 * GoldenEye's g_HitReactionTable as Perfect Dark's struct animtable, laid out
 * as g_AnimTablesHuman is: an empty row 0, the sixteen parts in HITPART order,
 * the hat and the end.
 */
static s32 geChrAnimsBuildTables(void)
{
	s32 used = 0;
	s32 n = 0;

	g_GeTables[n++] = (struct animtable){ 0, NULL, NULL, 0, 0 };

	for (s32 i = 0; i < ARRAYCOUNT(g_GeHitReactions); i++) {
		struct animtable *t = &g_GeTables[n++];

		t->hitpart = g_GeHitReactions[i].hitpart;

		if (!geChrAnimsRows(g_GeHitReactions[i].death, g_GeHitReactions[i].numdeath, &used, &t->deathanims)
				|| !geChrAnimsRows(g_GeHitReactions[i].flinch, g_GeHitReactions[i].numflinch, &used, &t->injuryanims)) {
			return 0;
		}

		t->deathanimcount = g_GeHitReactions[i].numdeath;
		t->injuryanimcount = g_GeHitReactions[i].numflinch;
	}

	g_GeTables[n++] = (struct animtable){ HITPART_HAT, NULL, NULL, 0, 0 };
	g_GeTables[n++] = (struct animtable){ -1, NULL, NULL, 0, 0 };

	for (s32 i = 0; i < 2; i++) {
		s32 ours = geChrAnim(g_Ge_death_stagger[i].geid);

		if (ours < 0) {
			return 0;
		}

		g_GeStagger[i].animnum = ours;
		g_GeStagger[i].flip = g_Ge_death_stagger[i].flip;
		g_GeStagger[i].endframe = g_Ge_death_stagger[i].endframe;
		g_GeStagger[i].speed = g_Ge_death_stagger[i].speed;
		g_GeStagger[i].unk10 = g_Ge_death_stagger[i].knockback;
		g_GeStagger[i].thudframe1 = g_Ge_death_stagger[i].thud1;
		g_GeStagger[i].thudframe2 = g_Ge_death_stagger[i].thud2;
	}

	for (s32 i = 0; i < ARRAYCOUNT(g_GeBlasts); i++) {
		if (geChrAnim(g_GeBlasts[i].geid) < 0) {
			return 0;
		}
	}

	return 1;
}

struct animtablerow *geChrAnimsStagger(s32 index)
{
	return &g_GeStagger[index & 1];
}

s32 geChrAnimsBlast(s32 side, u32 pick, s32 *animnum, s32 *flip, f32 *speed, f32 *startframe, f32 *thudframe, f32 *endframe)
{
	const struct geblast *row;

	if (!g_GeChrAnims || side < 0 || side >= 8) {
		return 0;
	}

	row = &g_GeBlasts[g_GeBlastSides[side].rows[pick % g_GeBlastSides[side].count]];

	*animnum = geChrAnim(row->geid);
	*flip = row->flip;
	*speed = row->speed;
	*startframe = row->startframe;
	*thudframe = row->thudframe;
	*endframe = row->endframe;

	return *animnum >= 0;
}

/* ---- Perfect Dark's deaths beside GoldenEye's --------------------------- */

/**
 * "GE Plus: Perfect Dark Death Animations" (Mod.GePlusPdDeathAnims, off by
 * default; F3 20260928-055343): a converted level's characters die by
 * GoldenEye's animations or by Perfect Dark's, picked from one pool per hit
 * part. Perfect Dark's frames sit under the numbers GoldenEye's took over for
 * the stage (animOverride()), so its rows play under animOriginal()'s numbers.
 * Its slump against a wall, its fall forward and its blast deaths join
 * GoldenEye's the same way (chraction.c, geChrAnimsPdDeaths()).
 */
static s32 g_GePdDeathsWanted = 0;
static s32 g_GeMixBuilt;
static struct animtablerow g_GeMixRows[GE_MAX_ROWS * 3];
static struct animtable g_GeMixTables[ARRAYCOUNT(g_GeTables)];

PD_CONSTRUCTOR static void geChrAnimsConfigInit(void)
{
	configRegisterInt("Mod.GePlusPdDeathAnims", &g_GePdDeathsWanted, 0, 1);
}

static const struct animtable *geChrAnimsPdTable(s32 hitpart)
{
	for (s32 i = 0; g_PdHumanTables && g_PdHumanTables[i].hitpart != -1; i++) {
		if (i > 0 && g_PdHumanTables[i].hitpart == hitpart) {
			return &g_PdHumanTables[i];
		}
	}

	return NULL;
}

static s32 geChrAnimsBuildMix(void)
{
	s32 used = 0;

	for (s32 i = 0; i < (s32)ARRAYCOUNT(g_GeTables); i++) {
		const struct animtable *ge = &g_GeTables[i];
		const struct animtable *pd;
		struct animtable *t = &g_GeMixTables[i];
		s32 count = 0;

		*t = *ge;

		if (ge->hitpart == -1) {
			break;
		}

		pd = ge->hitpart > 0 && ge->hitpart != HITPART_HAT ? geChrAnimsPdTable(ge->hitpart) : NULL;

		if (!pd || !pd->deathanims || pd->deathanimcount <= 0) {
			continue;
		}

		if (used + ge->deathanimcount + pd->deathanimcount + 1 > (s32)ARRAYCOUNT(g_GeMixRows)) {
			return 0;
		}

		t->deathanims = &g_GeMixRows[used];

		for (s32 j = 0; j < ge->deathanimcount; j++) {
			g_GeMixRows[used + count++] = ge->deathanims[j];
		}

		for (s32 j = 0; j < pd->deathanimcount; j++) {
			s32 num = animOriginal(pd->deathanims[j].animnum);

			if (num > 0) {
				g_GeMixRows[used + count] = pd->deathanims[j];
				g_GeMixRows[used + count].animnum = num;
				count++;
			}
		}

		g_GeMixRows[used + count].animnum = 0;
		t->deathanimcount = count;
		used += count + 1;
	}

	// the numbers chraction.c asks for at a death, made now and not mid-level
	for (s32 i = 0; g_AnimTableHumanSlumped[i].animnum; i++) {
		animOriginal(g_AnimTableHumanSlumped[i].animnum);
	}

	animOriginal(0x005b);
	animOriginal(0x0255);

	// the blast deaths (g_YeetAnimsHuman)
	for (s32 i = 0x82; i <= 0x8e; i++) {
		animOriginal(i);
	}

	return 1;
}

static void geChrAnimsApply(void)
{
	if (g_GeChrAnims) {
		g_AnimTablesByRace[RACE_HUMAN] = geChrAnimsPdDeaths() ? g_GeMixTables : g_GeTables;
	}
}

s32 geChrAnimsPdDeaths(void)
{
	return g_GeChrAnims && g_GePdDeathsWanted && g_GeMixBuilt;
}

s32 geChrAnimsGetPdDeaths(void)
{
	return g_GePdDeathsWanted;
}

void geChrAnimsSetPdDeaths(s32 on)
{
	g_GePdDeathsWanted = on ? 1 : 0;

	// the pool is taken at the next death; the numbers were made at the stage's start
	geChrAnimsApply();
}

s32 geChrAnimsPd(s32 animnum)
{
	s32 num = animOriginal(animnum);

	return num > 0 ? num : animnum;
}

/* ---- the switch --------------------------------------------------------- */

/**
 * What the walks and runs measure as their stride (race0f0005c0()), which
 * chraction.c times a route by and sets the unarmed walk's speed from - taken
 * again whenever the frames under those numbers change.
 */
static void geChrAnimsMeasure(void)
{
	for (s32 i = 0; var80067fdc[0][i].animnum >= 0; i++) {
		var80067fdc[0][i].value = race0f0005c0(var80067fdc[0][i].animnum);
	}
}

static void geChrAnimsSet(s32 on)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_Moved); i++) {
		animOverride(g_Moved[i][0], on ? geChrAnim(g_Moved[i][1]) : -1);
	}

	for (i = 2; i < 176; i++) {
		if (geChrAnimIsOurs(i) && i != 50) {
			s32 from = on ? geChrAnim(i) : -1;

			if (!on || from >= 0) {
				animOverride(i, from);
			}
		}
	}

	if (!g_PdHumanTables) {
		g_PdHumanTables = g_AnimTablesByRace[RACE_HUMAN];
	}

	g_AnimTablesByRace[RACE_HUMAN] = on ? g_GeTables : g_PdHumanTables;

	g_GeChrAnims = on;

	geChrAnimsMeasure();
}

void geChrAnimsStageStart(s32 stagenum)
{
	s32 on = 0;

	if (modloaderStageIsRemake(stagenum)) {
		// the conversion's animations, appended once a session
		gexPlusMissionAnimLoad(stagenum);

		// a conversion older than 65 carries only what its missions named, and
		// half of GoldenEye's tables would play nothing
		on = geChrAnim(0) >= 0 && geChrAnimsBuildTables();

		if (!on) {
			sysLogPrintf(LOG_WARNING, "gechranims: the conversion has too few of GoldenEye's animations; this level's characters play Perfect Dark's");
		}
	}

	if (on != g_GeChrAnims) {
		geChrAnimsSet(on);
		sysLogPrintf(LOG_NOTE, "gechranims: characters play %s animations", on ? "GoldenEye's" : "Perfect Dark's");
	}

	if (on) {
		// after the overrides, so the ROM's own entries are the saved ones
		g_GeMixBuilt = geChrAnimsBuildMix();
		geChrAnimsApply();

		sysLogPrintf(LOG_NOTE, "gechranims: Perfect Dark's death animations %s (pool %s)",
			geChrAnimsPdDeaths() ? "beside GoldenEye's" : "off", g_GeMixBuilt ? "built" : "not built");
	}
}

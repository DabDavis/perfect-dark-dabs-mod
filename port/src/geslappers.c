#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "game/file.h"
#include "game/lang.h"
#include "lib/rng.h"
#include "lib/snd.h"
#include "system.h"
#include "romdata.h"
#include "modloader.h"
#include "gesfx.h"
#include "geguns.h"
#include "geslappers.h"

#ifndef PLATFORM_N64

/**
 * GoldenEye's slappers.
 *
 * Unarmed, Bond slaps: GoldenEye's ITEM_FIST is his bare hand (GfistZ), held
 * below the bottom of the screen, that swings up from one side and chops down
 * across the view (gun.c's fistMeleeKeyframes1 and 2, one at random each time,
 * gunfire.c's GUN_ANIM_STATE_PUNCH1/2_STRIKE and _RECOVER). Perfect Dark's
 * unarmed is its own punch - the player's own fists, jab, cross and push, a
 * dizzying blow that knocks a guard out from behind - and a converted level
 * played that (F3 20260926-112255, Dam in the GE Arenas' Combat Simulator: "no
 * slappers yet, still using PD punches").
 *
 * There is no weapon number to give them: the port's numbers end at 0x7f,
 * all of them GoldenEye's guns and gadgets already (constants.h). And no
 * number is wanted, since unarmed is not something picked up - every piece of
 * the game that gives, takes, draws to or tests for "no gun" says
 * WEAPON_UNARMED. So on a converted level WEAPON_UNARMED's definition is this
 * one (geslappersStageLoad() swaps g_Weapons[WEAPON_UNARMED]), and on every
 * other stage Perfect Dark's own again. It is built the way geguns.c builds
 * GoldenEye's guns, field by field from three places:
 *
 * - **GoldenEye's own row** (gegunsfist.h, generated from the decomp's
 *   obseg/gun/fist/gunWeaponStat.inc.c like gegunstats.h): the damage (its
 *   Destruction, 2 - a guard standing about takes it twice in the chest and
 *   falls; Perfect Dark's punch is 0.5), the noise (a guard hears a slap a
 *   metre off; Perfect Dark's punch is silent), where the hand is held (PosX,
 *   PosY, PosZ: low enough to be out of sight at rest), the sway and the flags
 *   it is held by.
 * - **GoldenEye's code**, where the numbers are not in the row: the reach
 *   (chrprop.c's chraiFistAttackHandler(), 50 units, its level's own units,
 *   which a conversion keeps), the swing and when in it the slap lands
 *   (gunfire.c, 30 sixtieths in, WHEN_1E_FLD890 in the US build; the swing is
 *   done by 42), the whoosh of a slap that reaches nobody (PUNCHING_AIR_SFX)
 *   and the cut a guard who is busy doing something takes from it, by the side
 *   it comes from (chraction.c, geslappersDamageScale()). The punch's three
 *   thuds on a hit are Perfect Dark's 47, 48 and 49, which are GoldenEye's
 *   PUNCH1-3_SFX by number and on a converted level its own samples (gesfx.c).
 * - **Perfect Dark's engine** (invitem_unarmed): the aim settings, flags2 and
 *   flags3, and the rest of what bondgun.c asks of an unarmed hand.
 *
 * What it leaves out of Perfect Dark's punch, as GoldenEye has none of it: the
 * second function (disarm), the dizziness, the knock-out from behind, the
 * shove, the guard's dodge as the fist comes up, punching glass and the wall
 * thud. The model is GoldenEye's own, converted from the player's ROM (the
 * conversion's Igx001Z); without it - a conversion from before it was written
 * - unarmed stays Perfect Dark's everywhere, since the slap cannot be drawn on
 * Perfect Dark's hand.
 */

extern struct weapon invitem_unarmed;

// GoldenEye's own gun.c: the hand's reach, and when in the swing it lands
#define GESLAP_REACH  50.0f
#define GESLAP_STRIKE 30.0f

// PUNCHING_AIR_SFX, GoldenEye's SFX_ID
#define GESFX_PUNCHING_AIR 105

// GoldenEye's hand item number, the conversion's Igx%03dZ
#define ITEM_FIST 1

// The row's fields, in gegunstats.h's order; what the slappers take of them
struct gefiststat {
	f32 damage;
	struct noisesettings noise;
	f32 sway;
	f32 muzzle;
	u32 bitflags;
};

#define FISTSTAT(source, mag, autorate, singlerate, pen, dmg, spread, impact, loudmin, loudmax, pershot, lineartime, scaledtime, \
		speed0, speed1, speed2, speed3, back, up, bolt, sway, zoom, muzzle, ammotype, bitflags) \
	{ dmg, { loudmin, loudmax, pershot, lineartime, scaledtime }, sway, muzzle, bitflags }

static const struct gefiststat fiststat[] = {
#include "gegunsfist.h"
};

static const f32 fistpos[3] = FISTPOS;

#undef FISTSTAT

// GoldenEye's WEAPONSTATBITFLAG_* bits the flags are built from (as geguns.c)
#define GESTATFLAG_ONLY_1_HANDED          0x00000100
#define GESTATFLAG_HIDE_FIRST_PERSON_MENU 0x00004000
#define GESTATFLAG_USE_HOLD_TIME          0x00020000

/**
 * gun.c's fistMeleeKeyframes1 (the hand from the right of the screen, chopping
 * down and to the left) and fistMeleeKeyframes2 (from the left, down and to the
 * right). The two 20-sixtieth keyframes at the end are the spline's last
 * control points, not time the swing takes: gunSample1PTransform() ends the
 * track two keyframes before its last, at 42.
 */
static const struct geknifekey geSlap[2][10] = {
	{
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 10.0f },
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 10.0f },
		{ 0, {   6.0f, 23.0f, 0.0f }, {  5.91572f, 0.085832f, 0.219482f }, 0.5f, 10.0f },
		{ 0, {  18.0f, 35.0f, 9.5f }, { 4.998193f, 0.084203f, 0.268954f }, 0.5f, 10.0f },
		{ 0, { -20.0f, 25.5f, 4.0f }, { 0.126148f, 0.304284f, 0.548047f }, 0.5f, 10.0f },
		{ 0, { -28.0f, -4.0f, 2.0f }, { 0.506821f,  0.51473f, 0.484098f }, 0.5f,  1.0f },
		{ 0, { -28.0f, -4.0f, 2.0f }, { 0.506821f,  0.51473f, 0.484098f }, 0.5f,  1.0f },
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 20.0f },
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 20.0f },
		{ 1, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.0f,  0.0f },
	},
	{
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 10.0f },
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 10.0f },
		{ 0, {  -6.0f, 23.0f, 0.0f }, {  5.08683f, 6.131295f, 5.534376f }, 0.5f, 10.0f },
		{ 0, { -18.0f, 35.0f, 9.5f }, { 4.880698f, 0.070396f,  5.53615f }, 0.5f, 10.0f },
		{ 0, {   8.0f, 25.5f, 4.0f }, { 0.107213f, 6.062361f, 5.404225f }, 0.5f, 10.0f },
		{ 0, {  28.0f, -4.0f, 2.0f }, { 0.107213f, 6.062361f, 5.404225f }, 0.5f,  1.0f },
		{ 0, {  28.0f, -4.0f, 2.0f }, { 0.107213f, 6.062361f, 5.404225f }, 0.5f,  1.0f },
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 20.0f },
		{ 0, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.5f, 20.0f },
		{ 1, {   0.0f,  0.0f, 0.0f }, {      0.0f,      0.0f,      0.0f }, 0.0f,  0.0f },
	},
};

static struct noisesettings slapnoise;
static struct weaponfunc_melee slapfunc;
static struct weapon slappers;

// Whose unarmed the table had before this stage's swap
static struct weapon *pdunarmed;
static s32 active;

// Each hand's slap: which track, and how far into it in sixtieths (-1 none)
static s8 track[2] = { -1, -1 };
static f32 slaptime[2];

PD_CONSTRUCTOR static void geslappersInit(void)
{
	const struct gefiststat *stat = &fiststat[0];
	const struct weapon *engine = &invitem_unarmed;
	u32 flags;

	slapnoise = stat->noise;

	// GoldenEye's one function: no disarm, no dizziness, no knock-out from
	// behind (FUNCFLAG_BLUNTIMPACT, whose cut GoldenEye makes its own way:
	// geslappersDamageScale()), and a guard it hits reels as from a shot
	// (no FUNCFLAG_NOSTUN). FUNCFLAG_00400000 only says it is a punch.
	slapfunc.base.type = INVENTORYFUNCTYPE_MELEE;
	slapfunc.base.name = langAddPortText("Slap\n");
	slapfunc.base.ammoindex = -1;
	slapfunc.base.noisesettings = &slapnoise;
	slapfunc.base.fire_animation = NULL; // timed by the swing (bondgun.c)
	slapfunc.base.flags = FUNCFLAG_NOMUZZLEFLASH | FUNCFLAG_00400000;
	slapfunc.damage = stat->damage;
	slapfunc.range = GESLAP_REACH;

	memset(&slappers, 0, sizeof(slappers));

	// the model it is drawn on, GoldenEye's own hand (geslappersStageLoad())
	slappers.posx = fistpos[0];
	slappers.posy = fistpos[1];
	slappers.posz = fistpos[2];
	slappers.functions[0] = &slapfunc;
	slappers.functions[1] = NULL;
	slappers.ammos[0] = NULL;
	slappers.ammos[1] = NULL;
	slappers.aimsettings = engine->aimsettings;
	slappers.sway = stat->sway;
	slappers.muzzlez = stat->muzzle;

	// GoldenEye's "Unarmed" is Perfect Dark's word for it too; no maker or
	// description, as GoldenEye has none
	slappers.shortname = engine->shortname;
	slappers.name = engine->name;
	slappers.manufacturer = L_GUN_000;
	slappers.description = L_GUN_000;

	// how it is held from GoldenEye's bits, the rest the engine's; no hands
	// of Perfect Dark's, the hand being the model
	flags = engine->flags & ~(WEAPONFLAG_ONEHANDED | WEAPONFLAG_TRACKTIMEUSED | WEAPONFLAG_HIDEMENUMODEL | WEAPONFLAG_HASHANDS);

	if (stat->bitflags & GESTATFLAG_ONLY_1_HANDED) {
		flags |= WEAPONFLAG_ONEHANDED;
	}

	if (stat->bitflags & GESTATFLAG_USE_HOLD_TIME) {
		flags |= WEAPONFLAG_TRACKTIMEUSED;
	}

	if (stat->bitflags & GESTATFLAG_HIDE_FIRST_PERSON_MENU) {
		flags |= WEAPONFLAG_HIDEMENUMODEL;
	}

	slappers.flags = flags;
	slappers.flags2 = engine->flags2;
	slappers.flags3 = engine->flags3;
	slappers.unequippedreloadindex = engine->unequippedreloadindex;
	slappers.pickupsound = 0;
}

/**
 * The conversion's GoldenEye hand for this stage, or 0: registered in the
 * stage's own mod directory each time a stage loads, which also answers a
 * slot table emptied by a change of mods (geguns.c's gegunsFindConverted()
 * has the trap that makes a kept number dangerous).
 */
static u16 geslappersModel(s32 stagenum)
{
	char name[16];
	s32 fileid;

	if (!modloaderStageIsRemake(stagenum)) {
		return 0;
	}

	snprintf(name, sizeof(name), "Igx%03dZ", ITEM_FIST);
	fileid = romdataRegisterModFile(name, modloaderGetStageModDirIndex(stagenum));

	if (fileid <= 0 || fileGetInflatedSize(fileid, LOADTYPE_MODEL) == 0) {
		return 0;
	}

	return (u16)fileid;
}

void geslappersStageLoad(s32 stagenum)
{
	const u16 model = geslappersModel(stagenum);

	track[0] = track[1] = -1;

	if (g_Weapons[WEAPON_UNARMED] != &slappers) {
		pdunarmed = g_Weapons[WEAPON_UNARMED];
	}

	active = model != 0 && pdunarmed != NULL;

	if (active) {
		slappers.hi_model = model;
		slappers.lo_model = model;
		g_Weapons[WEAPON_UNARMED] = &slappers;
	} else if (g_Weapons[WEAPON_UNARMED] == &slappers) {
		g_Weapons[WEAPON_UNARMED] = pdunarmed;
	}

	if (active) {
		sysLogPrintf(LOG_NOTE, "geslappers: unarmed is GoldenEye's slappers on this stage (file %d)", model);
	} else if (modloaderStageIsRemake(stagenum)) {
		sysLogPrintf(LOG_NOTE, "geslappers: the conversion has no Igx%03dZ, unarmed is Perfect Dark's", ITEM_FIST);
	}
}

s32 geslappersActive(void)
{
	return active && g_Weapons[WEAPON_UNARMED] == &slappers;
}

s32 geslappersInHand(const struct hand *hand)
{
	return hand && hand->gset.weaponnum == WEAPON_UNARMED && geslappersActive();
}

void geslappersStart(struct hand *hand, s32 handnum)
{
	if (handnum < 0 || handnum > 1) {
		return;
	}

	if (!geslappersInHand(hand)) {
		track[handnum] = -1;
		return;
	}

	// gunfire.c: !(randomGetNext() & 1) is the first
	track[handnum] = (rngRandom() & 1) ? 1 : 0;
	slaptime[handnum] = 0.0f;
}

s32 geslappersStruck(s32 handnum)
{
	return handnum >= 0 && handnum <= 1 && (track[handnum] < 0 || slaptime[handnum] >= GESLAP_STRIKE);
}

s32 geslappersSwinging(s32 handnum)
{
	return handnum >= 0 && handnum <= 1 && track[handnum] >= 0;
}

void geslappersTick(struct hand *hand, s32 handnum, f32 lvupdate60)
{
	if (handnum < 0 || handnum > 1 || track[handnum] < 0) {
		return;
	}

	if (!geslappersInHand(hand) || hand->state == HANDSTATE_CHANGEGUN) {
		track[handnum] = -1;
		return;
	}

	slaptime[handnum] += lvupdate60;

	if (gegunsSampleTrack(geSlap[track[handnum]], slaptime[handnum], &hand->posrotmtx, handnum == HAND_LEFT)) {
		hand->useposrot = true;
	} else {
		track[handnum] = -1;
	}
}

void geslappersMissed(void)
{
	const s32 num = geSfxOurs(GESFX_PUNCHING_AIR, 0);

	if (num > 0) {
		sndStart(var80095200, num, NULL, -1, -1, -1, -1, -1);
	}
}

/**
 * chraction.c's ITEM_FIST: a guard standing, patrolling, walking somewhere,
 * surrendering or playing an animation takes the whole of a slap; any other -
 * running, shooting, a player or a simulant - an eighth of it from the front,
 * a quarter from the side and a half from behind. `angle` is Perfect Dark's
 * chrDamage() angle, which is GoldenEye's (0 is the face).
 */
f32 geslappersDamageScale(const struct chrdata *chr, f32 angle)
{
	const f32 sixty = 1.0471976f;

	if (!chr) {
		return 1.0f;
	}

	switch (chr->actiontype) {
	case ACT_STAND:
	case ACT_PATROL:
	case ACT_SURRENDER:
	case ACT_ANIM:
		return 1.0f;
	case ACT_GOPOS:
		if ((chr->act_gopos.flags & GOPOSMASK_SPEED) == GOPOSFLAG_WALK) {
			return 1.0f;
		}
		break;
	}

	if (angle < sixty || angle > 5 * sixty) {
		return 0.125f;
	}

	if (angle < 2 * sixty || angle > 4 * sixty) {
		return 0.25f;
	}

	return 0.5f;
}

#endif

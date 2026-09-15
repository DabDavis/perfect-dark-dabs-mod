#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "game/lang.h"
#include "game/playermgr.h"
#include "geguns.h"

#ifndef PLATFORM_N64

/**
 * GoldenEye's guns as weapons of Perfect Dark's own, numbered past the stock
 * table (WEAPON_GE_FIRST).
 *
 * Each is a copy of the Perfect Dark weapon GoldenEye's gun became in it - the
 * PP7 of the PP9i, the KF7 Soviet of the KF7 Special, the Cougar Magnum of the
 * DY357 - so it takes its host's animations, ammo, functions and sounds, and
 * every test of a weapon by number asks about the host (weaponHost()). Its
 * first-person model is the host's, or, for the guns gebean.c has checked
 * (fpReady), an alias of it with the release's gun drawn on it. What is its own: the number, so it is held, dropped and
 * picked up beside its host rather than as it; GoldenEye's name; a model state
 * (MODEL_GE_FIRST), which gebean.c points at an alias of the host's pickup
 * that the GoldenEye XBLA release's pickup is drawn on; and a Combat
 * Simulator row (MPWEAPON_GE_FIRST), which gebean.c shows when that release
 * is in xbla/ and Mod.XblaGoldenEye is on.
 *
 * The copies are made before anything reads g_Weapons, from the stock
 * definitions: a mod's imported table replaces the stock pointers later and
 * leaves these alone, and a mod's lists keep the rows hidden anyway.
 */

_Static_assert(NUM_MODELS - MODEL_GE_FIRST == NUM_GE_WEAPONS,
		"a model state per GoldenEye gun");
_Static_assert(NUM_MPWEAPONS - MPWEAPON_GE_FIRST == NUM_GE_WEAPONS,
		"a Combat Simulator row per GoldenEye gun");
_Static_assert(NUM_WEAPONS <= 0x80, "gunctrl.weaponnum is an s8");

static const char *const names[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7             - WEAPON_GE_FIRST] = "PP7\n",
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = "PP7 (silenced)\n",
	[WEAPON_GE_DD44            - WEAPON_GE_FIRST] = "DD44 Dostovei\n",
	[WEAPON_GE_KLOBB           - WEAPON_GE_FIRST] = "Klobb\n",
	[WEAPON_GE_KF7SOVIET       - WEAPON_GE_FIRST] = "KF7 Soviet\n",
	[WEAPON_GE_ZMG             - WEAPON_GE_FIRST] = "ZMG (9mm)\n",
	[WEAPON_GE_D5K             - WEAPON_GE_FIRST] = "D5K Deutsche\n",
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = "D5K (silenced)\n",
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = "Phantom\n",
	[WEAPON_GE_AR33            - WEAPON_GE_FIRST] = "AR33 Assault Rifle\n",
	[WEAPON_GE_RCP90           - WEAPON_GE_FIRST] = "RC-P90\n",
	[WEAPON_GE_SHOTGUN         - WEAPON_GE_FIRST] = "Shotgun\n",
	[WEAPON_GE_AUTOSHOTGUN     - WEAPON_GE_FIRST] = "Automatic Shotgun\n",
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = "Sniper Rifle\n",
	[WEAPON_GE_COUGARMAGNUM    - WEAPON_GE_FIRST] = "Cougar Magnum\n",
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = "Golden Gun\n",
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = "Moonraker Laser\n",
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = "Grenade Launcher\n",
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = "Rocket Launcher\n",
	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = "Hunting Knife\n",
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = "Throwing Knife\n",
	[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = "Hand Grenade\n",
	[WEAPON_GE_TIMEDMINE       - WEAPON_GE_FIRST] = "Timed Mine\n",
	[WEAPON_GE_PROXIMITYMINE   - WEAPON_GE_FIRST] = "Proximity Mine\n",
	[WEAPON_GE_REMOTEMINE      - WEAPON_GE_FIRST] = "Remote Mine\n",
};

/**
 * GoldenEye's own numbers for each of its guns (gegunstats.h, generated from
 * the decomp's obseg/gun/<source>/gunWeaponStat.inc.c).
 *
 * They are written as they stand. Perfect Dark's conversions of GoldenEye's
 * guns carry the same numbers in the same units - the DD44 is the tt33's 1
 * damage, 6 spread, 8 rounds and 16 frames of recovery, the Klobb the
 * skorpion's 0.6 and 15, the RC-P90 the fnp90's 1.8 and 80 - so GoldenEye's
 * Destruction is a damage, its Inaccuracy a spread, its MagSize a clip and
 * its SingleRate a recovery time, with nothing to scale between them.
 *
 * What is not taken: the recoil, zoom, sway and loudness, which are how a gun
 * handles rather than what it does, and a thrown weapon's damage, since every
 * one of Perfect Dark's carries 0 there and the explosion does the work.
 */
struct gegunstat {
	s16 magsize;
	u8 autorate;
	u8 singlerate;
	u8 penetration;
	f32 damage;
	f32 spread;
	f32 impactforce;
};

#define GUNSTAT(weapon, source, mag, autorate, singlerate, pen, dmg, spread, impact) \
	[weapon - WEAPON_GE_FIRST] = { mag, autorate, singlerate, pen, dmg, spread, impact }

static const struct gegunstat stats[NUM_GE_WEAPONS] = {
#include "gegunstats.h"
};

#undef GUNSTAT

/**
 * GoldenEye's automatic rate as the rounds per minute Perfect Dark counts in.
 *
 * Its conversions are the calibration, and between them they use two rates:
 * the Klobb, the KF7 Soviet and the D5K are GoldenEye's rate 3 and fire at
 * 450, the AR33 and the RC-P90 are rate 2 and fire at 550 and 600. No gun of
 * the twenty-five carries any other rate, and 0xff is not automatic at all.
 */
static f32 gegunsRpm(u8 rate)
{
	switch (rate) {
	case 2: return 600.0f;
	case 3: return 450.0f;
	}

	return 0.0f;
}

/** How much of a weapon function is its own, by type (moddata.c's cvFunc()). */
static u32 gegunsFuncSize(s32 type)
{
	switch (type) {
	case INVENTORYFUNCTYPE_SHOOT_SINGLE:     return sizeof(struct weaponfunc_shootsingle);
	case INVENTORYFUNCTYPE_SHOOT_AUTOMATIC:  return sizeof(struct weaponfunc_shootauto);
	case INVENTORYFUNCTYPE_SHOOT_PROJECTILE: return sizeof(struct weaponfunc_shootprojectile);
	case INVENTORYFUNCTYPE_THROW:            return sizeof(struct weaponfunc_throw);
	case INVENTORYFUNCTYPE_MELEE:            return sizeof(struct weaponfunc_melee);
	case INVENTORYFUNCTYPE_SPECIAL:          return sizeof(struct weaponfunc_special);
	case INVENTORYFUNCTYPE_DEVICE:           return sizeof(struct weaponfunc_device);
	}

	return sizeof(struct weaponfunc);
}

/**
 * GoldenEye's numbers onto one copy, on copies of its host's own structures.
 *
 * A weapon's damage, spread, penetration and rate live in the functions it
 * carries and its magazine in its ammo, both of them shared with the host
 * until here: writing through them would arm Perfect Dark's own gun with
 * GoldenEye's numbers, which is the whole thing weaponHost() exists to avoid.
 */
static void gegunsApplyStats(s32 i)
{
	const struct gegunstat *stat = &stats[i];
	struct weapon *def = &g_GeWeaponDefs[i];
	s32 shoots = 0;

	for (s32 f = 0; f < 2; f++) {
		const struct weaponfunc *host = def->functions[f];
		struct weaponfunc *copy;
		u32 size;

		if (!host) {
			continue;
		}

		size = gegunsFuncSize(host->type);
		copy = malloc(size);

		if (!copy) {
			continue;
		}

		memcpy(copy, host, size);
		def->functions[f] = copy;

		if ((copy->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
			struct weaponfunc_shoot *shoot = (struct weaponfunc_shoot *)copy;

			shoots = 1;
			shoot->damage = stat->damage;
			shoot->spread = stat->spread;
			shoot->penetration = stat->penetration;
			shoot->impactforce = stat->impactforce;

			// 0xff is GoldenEye's "no rate", not a time
			if (stat->singlerate != 0xff) {
				shoot->recoverytime60 = (s8)stat->singlerate;
			}

			if (copy->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
				const f32 rpm = gegunsRpm(stat->autorate);

				if (rpm > 0.0f) {
					((struct weaponfunc_shootauto *)copy)->initialrpm = rpm;
					((struct weaponfunc_shootauto *)copy)->maxrpm = rpm;
				}
			}
		} else if ((copy->type & 0xff) == INVENTORYFUNCTYPE_MELEE) {
			// GoldenEye's knife is a 3 against Perfect Dark's 2
			((struct weaponfunc_melee *)copy)->damage = stat->damage;
		}
	}

	// The magazine, for a gun that has one. A knife's or a mine's MagSize is
	// how many are carried rather than a clip, and the Moonraker has none at
	// all; the second ammo slot is a second kind of ammunition, not this one.
	if (shoots && stat->magsize > 0 && def->ammos[0]) {
		struct inventory_ammo *copy = malloc(sizeof(*copy));

		if (copy) {
			*copy = *def->ammos[0];
			copy->clipsize = stat->magsize;
			def->ammos[0] = copy;
		}
	}
}

/** The stock model state a GoldenEye gun's host is picked up as. */
s32 gegunsHostModel(s32 index)
{
	return playermgrGetModelOfWeapon(g_GeWeaponHosts[index]);
}

PD_CONSTRUCTOR static void gegunsInit(void)
{
	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		const struct weapon *host = g_Weapons[g_GeWeaponHosts[i]];
		const s32 hostmodel = gegunsHostModel(i);
		const u16 name = langAddPortText(names[i]);

		g_GeWeaponDefs[i] = *host;
		g_GeWeaponDefs[i].name = name;
		g_GeWeaponDefs[i].shortname = name;

		gegunsApplyStats(i);

		// Until gebean.c has the release's pickup to point it at, the host's
		if (hostmodel >= 0 && hostmodel < MODEL_GE_FIRST) {
			g_ModelStates[MODEL_GE_FIRST + i].fileid = g_ModelStates[hostmodel].fileid;
			g_ModelStates[MODEL_GE_FIRST + i].scale = g_ModelStates[hostmodel].scale;
		}
	}
}

#endif

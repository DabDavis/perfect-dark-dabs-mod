#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "game/lang.h"
#include "game/playermgr.h"
#include "fs.h"
#include "romdata.h"
#include "system.h"
#include "lib/model.h"
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
 * is in added-content/ (gebeanGetEnabled()).
 *
 * The copies are made before anything reads g_Weapons, from the stock
 * definitions: a mod's imported table replaces the stock pointers later and
 * leaves these alone, and a mod's lists keep the rows hidden anyway.
 */

_Static_assert(MODEL_GE_FIRST + NUM_GE_WEAPONS <= MODEL_REMAKE_FIRST,
		"a model state per GoldenEye gun, before the remake's models");
_Static_assert(NUM_MPWEAPONS - MPWEAPON_GE_FIRST == NUM_GE_GUNS,
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
	[WEAPON_GE_COVERTMODEM     - WEAPON_GE_FIRST] = "Covert Modem\n",
	[WEAPON_GE_PLASTIQUE       - WEAPON_GE_FIRST] = "Plastique\n",
	[WEAPON_GE_GOLDENEYEKEY    - WEAPON_GE_FIRST] = "GoldenEye Key\n",
	[WEAPON_GE_CAMERA          - WEAPON_GE_FIRST] = "Camera\n",
	[WEAPON_GE_WATCHMAGNET     - WEAPON_GE_FIRST] = "Watch Magnet Attract\n",
	[WEAPON_GE_GADGETA         - WEAPON_GE_FIRST] = "Gadget\n",
	[WEAPON_GE_GADGETB         - WEAPON_GE_FIRST] = "Gadget\n",
	[WEAPON_GE_TANKSHELLS      - WEAPON_GE_FIRST] = "Tank\n",
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
 * How it handles is taken too: the recoil, the sway, the aim zoom and how far
 * the flash reaches along the barrel (muzzlez stretches it in z). An earlier build left those as the host's,
 * as how a gun feels rather than what it does - but GoldenEye X, which took
 * the same rows, is the oracle here, and its numbers are these rows' to the
 * byte. What they carry is more than a feel: the recoil's last two bytes are
 * how soon a released trigger may fire again, which is all that makes
 * GoldenEye's automatic shotgun semi-automatic - on its host's 20, 28, 0, 0
 * every shot waited out the Perfect Dark shotgun's whole kick. Perfect Dark
 * runs them through the same code (bgun0f09aba4() is GoldenEye's recoil,
 * speed bytes, pull back, kick up and bolt slide in the same order).
 *
 * Not taken: a thrown weapon's damage, since every one of Perfect Dark's
 * carries 0 there and the explosion does the work; and the position on
 * screen, which is the fitted model's (gebean.c) and not GoldenEye's.
 *
 * Its loudness is taken, which it was not at first. The two games keep a
 * gun's noise in the same five numbers and run them through the same code
 * (gunfire.c's noise against bgunDecreaseNoiseRadius(), chr.c's hearing test
 * against chrsCheckForNoise()), and the hosts' numbers are GoldenEye X's,
 * which gives its three quiet guns - both silenced ones and the sniper rifle -
 * nothing a shot: their radius never left nought, and no guard on Dam, where
 * Bond starts with the silenced PP7, could hear a shot fired beside him.
 * GoldenEye's is 1 a shot up to 5, which is five metres.
 */
struct gegunstat {
	s16 magsize;
	u8 autorate;
	u8 singlerate;
	u8 penetration;
	f32 damage;
	f32 spread;
	f32 impactforce;
	struct noisesettings noise;
	s8 recoilspeed[4];
	f32 recoilback;
	f32 recoilup;
	f32 boltback;
	f32 sway;
	f32 zoom;
	f32 muzzle;
};

#define GUNSTAT(weapon, source, mag, autorate, singlerate, pen, dmg, spread, impact, loudmin, loudmax, pershot, lineartime, scaledtime, \
		speed0, speed1, speed2, speed3, back, up, bolt, sway, zoom, muzzle) \
	[weapon - WEAPON_GE_FIRST] = { mag, autorate, singlerate, pen, dmg, spread, impact, { loudmin, loudmax, pershot, lineartime, scaledtime }, \
		{ speed0, speed1, speed2, speed3 }, back, up, bolt, sway, zoom, muzzle }

/**
 * What each gun sounds like: the Sound field of the same gunWeaponStat rows,
 * GoldenEye's own SFX_ID. A gun here stands on a Perfect Dark host and fired
 * with its host's sound (or GoldenEye X's copy of GoldenEye's, borrowed). The
 * number is only good on a converted level, where a sound of GoldenEye's
 * number is GoldenEye's sample out of the ROM (gesfx.c) - Perfect Dark's bank
 * has something else in most of these slots, or nothing - so it is handed out
 * when a shot is fired there and never written to the definition. 0 is a
 * weapon GoldenEye fires in silence: what is thrown, and the rocket launcher,
 * whose sound is the rocket's.
 */
static const u8 shootsounds[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7 - WEAPON_GE_FIRST]             = 107, // GUN_B2_HEAVY
	[WEAPON_GE_PP7SILENCED - WEAPON_GE_FIRST]     = 46,  // GUN_SILPPK_A
	[WEAPON_GE_DD44 - WEAPON_GE_FIRST]            = 112, // GUN_B8_ANOTHER
	[WEAPON_GE_KLOBB - WEAPON_GE_FIRST]           = 106, // GUN_B1_MGUN3_3
	[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST]       = 109, // GUN_B4_BOLTACTION
	[WEAPON_GE_ZMG - WEAPON_GE_FIRST]             = 110, // GUN_B5_WINC44
	[WEAPON_GE_D5K - WEAPON_GE_FIRST]             = 117, // GUN_B13_M60AMMGUN
	[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST]     = 46,
	[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST]         = 109,
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST]            = 113, // GUN_B9_CANNON
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST]           = 253, // GUN_B9_CANNON_SHORT
	[WEAPON_GE_SHOTGUN - WEAPON_GE_FIRST]         = 121, // GUN_B17_RIFLE
	[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST]     = 116, // GUN_B12_FULLAMRIFLE
	[WEAPON_GE_SNIPERRIFLE - WEAPON_GE_FIRST]     = 46,
	[WEAPON_GE_COUGARMAGNUM - WEAPON_GE_FIRST]    = 111, // GUN_RIFLE7BIG_1
	[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST]       = 117,
	[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST]       = 228, // LASER_GUN
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = 12,  // GUN_TANK2BIGBIG_1
};

/**
 * And how often: the same rows' SoundTriggerRate (the US ROM's, its BUGFIX_R0
 * set; Europe's are a tick shorter). Where it is not 0, a held trigger starts
 * the sound again no sooner than this many sixtieths after the last, however
 * fast the gun fires, cutting the last one off as it does - so the Klobb's
 * rattle is one sound every eleven ticks and not one a bullet. With 0 every
 * shot sounds. Perfect Dark kept the code whole (bgun0f09a6f8() and
 * chrUpdateFireslot() are gunTickHandState() and sub_GAME_7F02BFE4() line for
 * line) under the name of a fire slot's duration, so this is that number.
 */
static const u8 shootsoundrates[NUM_GE_WEAPONS] = {
	[WEAPON_GE_KLOBB - WEAPON_GE_FIRST]       = 11,
	[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST]   = 4,
	[WEAPON_GE_ZMG - WEAPON_GE_FIRST]         = 4,
	[WEAPON_GE_D5K - WEAPON_GE_FIRST]         = 4,
	[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST] = 4,
	[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST]     = 4,
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST]        = 5,
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST]       = 2,
};

s32 gegunsShootSoundRate(s32 weaponnum)
{
	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= WEAPON_GE_FIRST + NUM_GE_WEAPONS) {
		return -1;
	}

	return shootsoundrates[weaponnum - WEAPON_GE_FIRST];
}

s32 gegunsShootSound(s32 weaponnum)
{
	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= WEAPON_GE_FIRST + NUM_GE_WEAPONS) {
		return 0;
	}

	return shootsounds[weaponnum - WEAPON_GE_FIRST];
}

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
	struct noisesettings *noise = NULL;
	s32 shoots = 0;

	// How loud it is, which a function carries. A row with no times is a
	// gadget's, which has no row, and the noise code divides by them.
	if (stat->noise.decbasespeed > 0.0f && stat->noise.decremspeed > 0.0f) {
		noise = malloc(sizeof(*noise));

		if (noise) {
			*noise = stat->noise;
		}
	}

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

		// GoldenEye's gun has the one noise whatever it is doing
		if (noise && (f == 0 || (copy->type & 0xff) == INVENTORYFUNCTYPE_SHOOT)) {
			copy->noisesettings = noise;
		}

		if ((copy->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
			struct weaponfunc_shoot *shoot = (struct weaponfunc_shoot *)copy;

			shoots = 1;
			shoot->damage = stat->damage;
			shoot->spread = stat->spread;
			shoot->penetration = stat->penetration;
			shoot->impactforce = stat->impactforce;

			// The Shotgun works its host's pump after every shot, and the
			// timing is the pump's: GoldenEye's early refire would cut it
			// short (GoldenEye X times its own pump too, 0 and 68)
			if (WEAPON_GE_FIRST + i != WEAPON_GE_SHOTGUN) {
				shoot->unk24 = stat->recoilspeed[0];
				shoot->unk25 = stat->recoilspeed[1];
				shoot->unk26 = stat->recoilspeed[2];
				shoot->unk27 = stat->recoilspeed[3];
			}

			shoot->recoildist = stat->recoilback;
			shoot->recoilangle = stat->recoilup;
			shoot->slidemax = stat->boltback;

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

	// How it sits in the hand. A gadget has no row (all nought) and keeps its
	// host's. The sniper rifle's zoom is the player's own, wound in and out
	// (currentPlayerGetGunZoomFov()), so only the others take GoldenEye's.
	if (shoots) {
		def->sway = stat->sway;
		def->muzzlez = stat->muzzle;

		if (def->aimsettings && g_GeWeaponHosts[i] != WEAPON_SNIPERRIFLE
				&& def->aimsettings->zoomfov != stat->zoom) {
			struct invaimsettings *aim = malloc(sizeof(*aim));

			if (aim) {
				*aim = *def->aimsettings;
				aim->zoomfov = stat->zoom;
				def->aimsettings = aim;
			}
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

/**
 * GoldenEye's guns reload the way their hosts do, and nothing is borrowed.
 *
 * Perfect Dark's own conversions of GoldenEye's eight classic guns - the PP9i,
 * the CC13, the KL01313, the KF7 Special, the ZZT, the DMC, the AR53 and the
 * RC-P45 - carry **no reload animation at all**, and bondgun.c reloads a
 * weapon without one by lowering the gun off the bottom of the screen and
 * raising it again (HANDSTATEMINOR_RELOAD_LOWER). Every GoldenEye gun hosted
 * on one of them inherits that, and that is what it is meant to do: "only
 * classic lower gun reload".
 *
 * An earlier build lent each of them an animation authored for another gun of
 * the same kind, on the grounds that every first-person gun model carries the
 * same hand skeleton. What that looks like on screen is somebody else's reload
 * played on this gun - "the pp7 is doing falcon 2 reload, etc" - a hand
 * reaching for a magazine the PP7 has not got and dropping one it never held.
 * A gun that lowers off the screen is honest about having no animation of its
 * own; a gun performing another gun's is not. So nothing is lent, and a copy's
 * ammunition is its host's apart from the magazine size the stats write.
 */

/**
 * What GoldenEye's guns do on the trigger, which is not what their hosts do.
 *
 * GoldenEye X is the oracle (build/gunoracle, both definitions dumped side by
 * side on one boot), and against it a host's copy brings along four things
 * GoldenEye never had:
 *
 * - **A second function.** GoldenEye's guns have one each. The Phantom took
 *   the CMP150's target locker, both shotguns the double blast, the Golden Gun
 *   the DY357-LX's pistol whip, the Moonraker the laser's stream, the grenade
 *   launcher the Devastator's wall hugger, the rocket launcher the homing
 *   rocket, the grenade its proximity pinball and the two mines a threat
 *   detector. GoldenEye X has none of them; what it keeps (the Cougar's whip,
 *   a knife's throw, the remote mine's detonator) is kept here too. It was
 *   more than a spare button: the choice of function is saved per *host*
 *   (bgunIsUsingSecondaryFunctionForHand()), so a player who had left the
 *   DY357-LX on its whip drew the Golden Gun whipping and never firing, and
 *   one who left the timed mine on its detector could not place GoldenEye's
 *   mines. With no second function the hand stays on the first, as it does
 *   for Perfect Dark's own classic guns.
 * - **The automatic shotgun's pump.** Its host's single shot is the Perfect
 *   Dark shotgun's, which works the pump after every shot - read as a reload
 *   each shot ("auto shotgun reloads every shot"). GoldenEye X's has no fire
 *   animation at all: the recoil does the kick. The plain Shotgun is a pump
 *   action in GoldenEye X and keeps its host's.
 * - **A muzzle flash** on the silenced guns and the two launchers, whose
 *   GoldenEye models have no flash to show.
 * - **Lock-on tracking** in the aim: the CMP150's follow lock and the rocket
 *   launcher's, both of which went with the secondary that used them.
 *
 * And, as GoldenEye X has them: the Cougar's pistol whip does not leave its
 * victim dizzy, a knife is thrown where it is aimed and not where auto-aim
 * would put it, and two words of text: "an Automatic Shotgun", "the Golden
 * Gun".
 */
static void gegunsOwnTrigger(s32 i)
{
	const s32 weaponnum = WEAPON_GE_FIRST + i;
	struct weapon *def = &g_GeWeaponDefs[i];

	switch (weaponnum) {
	case WEAPON_GE_PHANTOM:
	case WEAPON_GE_SHOTGUN:
	case WEAPON_GE_AUTOSHOTGUN:
	case WEAPON_GE_GOLDENGUN:
	case WEAPON_GE_MOONRAKER:
	case WEAPON_GE_GRENADELAUNCHER:
	case WEAPON_GE_ROCKETLAUNCHER:
	case WEAPON_GE_GRENADE:
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
		def->functions[1] = NULL;
		break;
	}

	for (s32 f = 0; f < 2; f++) {
		const struct weaponfunc *func = def->functions[f];
		struct weaponfunc *copy;
		u32 flags;
		struct guncmd *fire;

		if (!func) {
			continue;
		}

		flags = func->flags;
		fire = func->fire_animation;

		switch (weaponnum) {
		case WEAPON_GE_PP7SILENCED:
		case WEAPON_GE_D5KSILENCED:
		case WEAPON_GE_GRENADELAUNCHER:
		case WEAPON_GE_ROCKETLAUNCHER:
			if ((func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
				flags |= FUNCFLAG_NOMUZZLEFLASH;
			}
			break;
		case WEAPON_GE_AUTOSHOTGUN:
			fire = NULL;
			break;
		case WEAPON_GE_COUGARMAGNUM:
			if (func->type == INVENTORYFUNCTYPE_MELEE) {
				flags &= ~FUNCFLAG_MAKEDIZZY;
			}
			break;
		case WEAPON_GE_HUNTINGKNIFE:
		case WEAPON_GE_THROWINGKNIFE:
			if (func->type == INVENTORYFUNCTYPE_THROW) {
				flags |= FUNCFLAG_NOAUTOAIM;
			}
			break;
		}

		if (flags == func->flags && fire == func->fire_animation) {
			continue;
		}

		// gegunsApplyStats() gave a shooting function a copy of its own
		// already, but not the others; copy again rather than keep track
		copy = malloc(gegunsFuncSize(func->type));

		if (copy) {
			memcpy(copy, func, gegunsFuncSize(func->type));
			copy->flags = flags;
			copy->fire_animation = fire;
			def->functions[f] = copy;
		}
	}

	if ((weaponnum == WEAPON_GE_PHANTOM || weaponnum == WEAPON_GE_ROCKETLAUNCHER)
			&& def->aimsettings && def->aimsettings->tracktype != SIGHTTRACKTYPE_DEFAULT) {
		struct invaimsettings *aim = malloc(sizeof(*aim));

		if (aim) {
			*aim = *def->aimsettings;
			aim->tracktype = SIGHTTRACKTYPE_DEFAULT;
			def->aimsettings = aim;
		}
	}

	if (weaponnum == WEAPON_GE_AUTOSHOTGUN) {
		def->flags |= WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN;
	} else if (weaponnum == WEAPON_GE_GOLDENGUN) {
		def->flags |= WEAPONFLAG_DETERMINER_S_THE | WEAPONFLAG_DETERMINER_F_THE;
	}
}

/**
 * GoldenEye's guns as another installed mod made them (modborrow.c): GoldenEye
 * X's definition whole - its model, hands, positions, functions, fire and
 * reload scripts with their animations and sounds already moved to numbers of
 * the port's own - under GoldenEye's name here, and with the port's own fields
 * (flags2, flags3, the unequipped reload index, the pickup sound) the host's,
 * since the code keyed on them asks about the host (weaponHost()).
 *
 * The ammunition's type stays the host's: a type is a row of the game's ammo
 * table, the one the crates, the HUD and the Combat Simulator's lists count,
 * and GoldenEye X's rows are its own table's. Its magazine and its reload are
 * GoldenEye X's.
 */
static struct weapon stockDefs[NUM_GE_WEAPONS];
static struct weapon stockFalcon2;
static struct weapon stockKnife;
static struct weapon stockCmp150;
static struct weapon stockAr34;
static u8 borrowed[NUM_GE_WEAPONS];
static u16 borrowedPickupFile[NUM_GE_WEAPONS];
static u16 borrowedPickupScale[NUM_GE_WEAPONS];
static u32 borrowedHands[NUM_GE_WEAPONS];
static u16 borrowedModel[NUM_GE_WEAPONS];

/**
 * The port's name for function which, of this type, on gun index: its host's
 * function of the same type, else the Falcon 2's (a pistol whip) or the combat
 * knife's (a throw), else the host's first.
 */
static const struct weaponfunc *gegunsNameFor(s32 index, s32 which, s32 type)
{
	// GoldenEye's automatics are automatic, whatever their host fires: the
	// KF7 Special's and the AR53's "Burst Fire" named them wrong. A second
	// automatic function is the AR33's scope.
	if (type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
		return which == 0 ? stockCmp150.functions[0] : stockAr34.functions[1];
	}

	const struct weapon *const candidates[] = {
		&stockDefs[index],
		&stockFalcon2,
		&stockKnife,
	};

	for (s32 c = 0; c < ARRAYCOUNT(candidates); c++) {
		for (s32 f = 0; f < 2; f++) {
			const struct weaponfunc *func = candidates[c]->functions[f];

			if (func && func->type == type) {
				return func;
			}
		}
	}

	return stockDefs[index].functions[0];
}

void gegunsBorrow(s32 index, const struct weapon *def, u16 pickupfile, u16 pickupscale)
{
	struct weapon *out = &g_GeWeaponDefs[index];
	const struct weapon *stock = &stockDefs[index];

	if (!def) {
		if (borrowed[index]) {
			*out = *stock;
		}

		borrowed[index] = 0;
		borrowedPickupFile[index] = 0;
		return;
	}

	*out = *def;
	out->shortname = stock->shortname;
	out->name = stock->name;

	// Text ids are the mod's language files', which say something else here
	// (its KF7's function read "Burst Fire"): the port's own names stay
	out->manufacturer = stock->manufacturer;
	out->description = stock->description;

	for (s32 f = 0; f < 2; f++) {
		const struct weaponfunc *src = def->functions[f];
		const struct weaponfunc *ours = src ? gegunsNameFor(index, f, src->type) : NULL;

		if (src) {
			const u32 size = gegunsFuncSize(src->type);
			struct weaponfunc *copy = malloc(size);

			if (copy) {
				memcpy(copy, src, size);
				copy->name = ours ? ours->name : copy->name;
				out->functions[f] = copy;
			}
		}
	}
	out->flags2 = stock->flags2;
	out->flags3 = stock->flags3;
	out->unequippedreloadindex = stock->unequippedreloadindex;
	out->pickupsound = stock->pickupsound;

	for (s32 a = 0; a < 2; a++) {
		if (def->ammos[a] && stock->ammos[a]) {
			struct inventory_ammo *copy = malloc(sizeof(*copy));

			if (copy) {
				*copy = *def->ammos[a];
				copy->type = stock->ammos[a]->type;
				out->ammos[a] = copy;
			}
		}
	}

	// What the gun does is still GoldenEye's own, out of its ROM's rows: the
	// borrowed definition brings GoldenEye X's numbers with it, and those gave
	// the silenced PP7 no noise at all - a shot added nothing to its radius,
	// so nobody on Dam could hear Bond fire. gegunsApplyStats() copies the
	// functions it writes to, so the mod's own are left as they were.
	gegunsApplyStats(index);

	borrowed[index] = 1;
	borrowedHands[index] = def->flags & WEAPONFLAG_HASHANDS;
	borrowedModel[index] = def->hi_model;
	borrowedPickupFile[index] = pickupfile;
	borrowedPickupScale[index] = pickupscale;
}

s32 gegunsIsBorrowed(s32 index)
{
	return borrowed[index];
}

s32 gegunsBorrowedPickup(s32 index, u16 *fileid, u16 *scale)
{
	if (!borrowed[index] || !borrowedPickupFile[index]) {
		return 0;
	}

	*fileid = borrowedPickupFile[index];
	*scale = borrowedPickupScale[index];

	return 1;
}

/**
 * GoldenEye's own hand item number for gun `index`, which is what the
 * conversion names its first-person model after (files/Igx%03dZ) and what its
 * row of gitem_structs is at. The gadgets' is the mission's (gegadgets.c) and
 * is not here.
 */
s32 gegunsItemNumber(s32 index)
{
	static const s8 items[NUM_GE_WEAPONS] = {
		[WEAPON_GE_PP7 - WEAPON_GE_FIRST] = 4,
		[WEAPON_GE_PP7SILENCED - WEAPON_GE_FIRST] = 5,
		[WEAPON_GE_DD44 - WEAPON_GE_FIRST] = 6,
		[WEAPON_GE_KLOBB - WEAPON_GE_FIRST] = 7,
		[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST] = 8,
		[WEAPON_GE_ZMG - WEAPON_GE_FIRST] = 9,
		[WEAPON_GE_D5K - WEAPON_GE_FIRST] = 10,
		[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST] = 11,
		[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST] = 12,
		[WEAPON_GE_AR33 - WEAPON_GE_FIRST] = 13,
		[WEAPON_GE_RCP90 - WEAPON_GE_FIRST] = 14,
		[WEAPON_GE_SHOTGUN - WEAPON_GE_FIRST] = 15,
		[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST] = 16,
		[WEAPON_GE_SNIPERRIFLE - WEAPON_GE_FIRST] = 17,
		[WEAPON_GE_COUGARMAGNUM - WEAPON_GE_FIRST] = 18,
		[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST] = 19,
		[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST] = 22,
		[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = 24,
		[WEAPON_GE_ROCKETLAUNCHER - WEAPON_GE_FIRST] = 25,
		[WEAPON_GE_HUNTINGKNIFE - WEAPON_GE_FIRST] = 2,
		[WEAPON_GE_THROWINGKNIFE - WEAPON_GE_FIRST] = 3,
		[WEAPON_GE_GRENADE - WEAPON_GE_FIRST] = 26,
		[WEAPON_GE_TIMEDMINE - WEAPON_GE_FIRST] = 27,
		[WEAPON_GE_PROXIMITYMINE - WEAPON_GE_FIRST] = 28,
		[WEAPON_GE_REMOTEMINE - WEAPON_GE_FIRST] = 29,
	};

	return index >= 0 && index < NUM_GE_WEAPONS ? items[index] : 0;
}

/**
 * The gun's own model, converted from the player's ROM.
 *
 * GoldenEye's first-person guns convert whole (files/Igx%03dZ, converter 40),
 * and until now only the watch drew them - so in the N64 look a GoldenEye gun
 * with no GoldenEye X to borrow from had no model of its own and was not
 * offered at all. Found once, in whichever mod directory the conversion wrote
 * (there is one), and kept: registering a slot per directory per gun would
 * spend twenty-five of them on every mod installed.
 */
static u16 convertedModel[NUM_GE_WEAPONS];
static s32 convertedSearched = -1;

static void gegunsFindConverted(void)
{
	const s32 numdirs = fsGetNumModDirs();
	s32 found = 0;
	s32 dir = -1;

	if (convertedSearched == numdirs) {
		return;
	}

	convertedSearched = numdirs;

	// the PP7 is the conversion's marker: every GoldenEye gun is written with it
	for (s32 i = 0; i < numdirs && dir < 0; i++) {
		char path[FS_MAXPATH + 1];
		const char *at = fsGetModDirAt(i);

		if (!at) {
			continue;
		}

		snprintf(path, sizeof(path), "%s/files/Igx%03dZ", at, gegunsItemNumber(WEAPON_GE_PP7 - WEAPON_GE_FIRST));

		if (fsFileSize(path) > 0) {
			dir = i;
		}
	}

	if (dir < 0) {
		return;
	}

	for (s32 i = 0; i < NUM_GE_GUNS; i++) {
		const s32 item = gegunsItemNumber(i);
		char name[16];
		char path[FS_MAXPATH + 1];

		if (item <= 0 || convertedModel[i]) {
			continue;
		}

		snprintf(name, sizeof(name), "Igx%03dZ", item);
		snprintf(path, sizeof(path), "%s/files/%s", fsGetModDirAt(dir), name);

		if (fsFileSize(path) <= 0) {
			continue;
		}

		convertedModel[i] = (u16)romdataRegisterModFile(name, dir);

		if (convertedModel[i]) {
			found++;
		}
	}

	if (found) {
		sysLogPrintf(LOG_NOTE, "geguns: %d of GoldenEye's own first-person guns, converted from the ROM", found);
	}
}

/** Whether the conversion has GoldenEye's own first-person model for this gun. */
s32 gegunsHasOwnModel(s32 index)
{
	gegunsFindConverted();

	return index >= 0 && index < NUM_GE_GUNS && convertedModel[index] != 0;
}

/**
 * Whether GoldenEye draws nothing in the hand for this weapon, on its own
 * model: the grenade and the three mines carry
 * WEAPONSTATBITFLAG_HIDE_FIRST_PERSON_HAND, which in gunfire.c leaves
 * field_87F clear and the model undrawn at rest and through the throw alike -
 * what flies is the projectile, a model of its own. Drawn anyway, the
 * grenade's 715-unit model (a hand round it) filled the bottom of the view and
 * the mines sat below its edge. The native port shows only the ammunition icon.
 */
s32 gegunsOwnModelHidden(s32 weaponnum)
{
	if (!gegunsOwnModelInUse(weaponnum)) {
		return 0;
	}

	switch (weaponnum) {
	case WEAPON_GE_GRENADE:
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
	case WEAPON_GE_REMOTEMINE:
		return 1;
	}

	return 0;
}

/**
 * GoldenEye's own first-person model for this gun, converted from the ROM, or
 * 0. Only the N64 look wants it: in the other, the release's gun is skinned
 * onto the host's own first-person model and that is what has to be there
 * (gebeanBuildFirstPerson()).
 */
u16 gegunsOwnModel(s32 index)
{
	return gegunsHasOwnModel(index) ? convertedModel[index] : 0;
}

/**
 * Where GoldenEye holds each gun in front of the eye: gunWeaponStat's PosX,
 * PosY and PosZ, the offset in camera space gunfire.c builds the gun's matrix
 * at before sway and recoil. Perfect Dark's own posx/posy/posz are the same
 * three numbers in the same space (the Falcon 2's 9, -15.7, -23.8 beside the
 * PP7's 11, -20.8, -33.5), so GoldenEye's own model takes GoldenEye's.
 */
static const f32 ownpos[NUM_GE_GUNS][3] = {
	[WEAPON_GE_PP7 - WEAPON_GE_FIRST] = { 11.0f, -20.8f, -33.5f },
	[WEAPON_GE_PP7SILENCED - WEAPON_GE_FIRST] = { 11.0f, -20.8f, -33.5f },
	[WEAPON_GE_DD44 - WEAPON_GE_FIRST] = { 11.0f, -20.8f, -33.5f },
	[WEAPON_GE_KLOBB - WEAPON_GE_FIRST] = { 11.5f, -25.0f, -27.5f },
	[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST] = { 11.0f, -19.0f, -16.0f },
	[WEAPON_GE_ZMG - WEAPON_GE_FIRST] = { 11.0f, -24.5f, -37.0f },
	[WEAPON_GE_D5K - WEAPON_GE_FIRST] = { 11.0f, -26.4f, -35.0f },
	[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST] = { 11.0f, -26.4f, -35.0f },
	[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST] = { 11.0f, -21.9f, -35.0f },
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST] = { 11.0f, -19.2f, -21.5f },
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST] = { 12.5f, -25.3f, -32.5f },
	[WEAPON_GE_SHOTGUN - WEAPON_GE_FIRST] = { 11.0f, -20.6f, -19.5f },
	[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST] = { 12.0f, -24.1f, -19.0f },
	[WEAPON_GE_SNIPERRIFLE - WEAPON_GE_FIRST] = { 11.0f, -20.7f, -31.5f },
	[WEAPON_GE_COUGARMAGNUM - WEAPON_GE_FIRST] = { 12.0f, -20.8f, -33.5f },
	[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST] = { 11.0f, -20.8f, -33.5f },
	[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST] = { 11.0f, -19.5f, -28.0f },
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = { 9.5f, -18.0f, -18.5f },
	[WEAPON_GE_ROCKETLAUNCHER - WEAPON_GE_FIRST] = { 10.5f, -22.2f, -14.5f },
	[WEAPON_GE_HUNTINGKNIFE - WEAPON_GE_FIRST] = { 14.0f, -24.8f, -34.0f },
	[WEAPON_GE_THROWINGKNIFE - WEAPON_GE_FIRST] = { 14.0f, -24.8f, -34.0f },
	[WEAPON_GE_GRENADE - WEAPON_GE_FIRST] = { 11.0f, -41.8f, -33.0f },
	[WEAPON_GE_TIMEDMINE - WEAPON_GE_FIRST] = { 11.0f, -21.0f, -37.0f },
	[WEAPON_GE_PROXIMITYMINE - WEAPON_GE_FIRST] = { 11.0f, -21.0f, -37.0f },
	[WEAPON_GE_REMOTEMINE - WEAPON_GE_FIRST] = { 11.0f, -21.0f, -37.0f },
};

// The host's own placement and part commands, to go back to when the gun is
// drawn on the host's model again (the other look, F6)
static s32 ownInUse[NUM_GE_GUNS];
static s32 hostSaved[NUM_GE_GUNS];
static f32 hostPos[NUM_GE_GUNS][3];
static struct gunviscmd *hostVis[NUM_GE_GUNS];

// The host's part commands name the host's parts, which on GoldenEye's model
// are other things entirely
static struct gunviscmd noVisCmds[] = { { GUNVISCMD_END } };

/**
 * Draw gun `index` in first person on GoldenEye's own model (1) or on
 * whatever gebean.c put in hi_model otherwise (0): GoldenEye's placement and
 * no host part commands for the one, the host's for the other.
 */
void gegunsSetOwnModelInUse(s32 index, s32 inuse)
{
	struct weapon *def;

	if (index < 0 || index >= NUM_GE_GUNS) {
		return;
	}

	def = &g_GeWeaponDefs[index];

	if (!hostSaved[index]) {
		hostPos[index][0] = def->posx;
		hostPos[index][1] = def->posy;
		hostPos[index][2] = def->posz;
		hostVis[index] = def->gunviscmds;
		hostSaved[index] = 1;
	}

	ownInUse[index] = inuse;

	if (inuse) {
		def->posx = ownpos[index][0];
		def->posy = ownpos[index][1];
		def->posz = ownpos[index][2];
		def->gunviscmds = noVisCmds;
	} else {
		def->posx = hostPos[index][0];
		def->posy = hostPos[index][1];
		def->posz = hostPos[index][2];
		def->gunviscmds = hostVis[index];
	}
}

/** Whether this weapon is drawn in first person on GoldenEye's own model. */
s32 gegunsOwnModelInUse(s32 weaponnum)
{
	const s32 index = weaponnum - WEAPON_GE_FIRST;

	return index >= 0 && index < NUM_GE_GUNS && ownInUse[index];
}

static void gegunsSetPart(struct model *model, s32 part, s32 visible)
{
	struct modelnode *node = modelGetPart(model->definition, part);

	if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
		((union modelrwdata *)modelGetNodeRwData(model, node))->toggle.visible = visible;
	}
}

/**
 * GoldenEye's own switches on its own model, each frame the gun is drawn, as
 * gunfire.c sets them: Bond's hand and cuff are pieces of every gun (parts 8
 * to 13, and 35 where there are that many - sub_GAME_7F05E978(model, 1)), a
 * thrown item's own pieces 14 and 15 are on while it is in the hand, and part
 * 1 is the muzzle flash, on while the hand's flash is.
 */
void gegunsOwnModelParts(struct hand *hand, struct model *model)
{
	if (!gegunsOwnModelInUse(hand->gset.weaponnum)) {
		return;
	}

	for (s32 part = 8; part <= 13; part++) {
		gegunsSetPart(model, part, 1);
	}

	gegunsSetPart(model, 35, 1);
	gegunsSetPart(model, 14, 1);
	gegunsSetPart(model, 15, 1);
	gegunsSetPart(model, 1, hand->flashon ? 1 : 0);
}

/**
 * Where GoldenEye's own model's barrel ends: part 3, the position the muzzle
 * flash is drawn at (gunfire.c reads the flash's place from Switches[3]).
 *
 * Its own matrix is no use - it sits under the flash's switch (part 1), and a
 * matrix under a switch that is off is never computed that frame - so the
 * answer is the node whose matrix is always there, the one the switch hangs
 * from, and part 3's offset from it in that node's own space: the positions
 * between the two added up, which at rest (the gun is never animated joint by
 * joint, bgunSetGunMatrices()) is all there is. Asking for part 1 alone put
 * the flash and the tracers at the gun's root, which is its back.
 */
struct modelnode *gegunsOwnModelMuzzle(s32 weaponnum, struct modeldef *modeldef, f32 *offset)
{
	struct modelnode *flash;
	struct modelnode *node;
	s32 base;

	offset[0] = offset[1] = offset[2] = 0.0f;

	if (!gegunsOwnModelInUse(weaponnum) || !(flash = modelGetPart(modeldef, 1))) {
		return NULL;
	}

	base = modelFindNodeMtxIndex(flash, 0);
	node = modelGetPart(modeldef, 3);

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_POSITION) {
			if (modelFindNodeMtxIndex(node, 0) == base) {
				return node;
			}

			offset[0] += node->rodata->position.pos.x;
			offset[1] += node->rodata->position.pos.y;
			offset[2] += node->rodata->position.pos.z;
		}

		node = node->parent;
	}

	// no muzzle position under it: the switch's own node, with no offset
	offset[0] = offset[1] = offset[2] = 0.0f;

	return flash;
}

static f32 gegunsRandFrac(void)
{
	return (f32)rand() / (f32)RAND_MAX;
}

/** A flash matrix: a roll about z, `scale` all round and `ext` more along z, at `pos`, under `parent`. */
static void gegunsFlashMatrix(Mtxf *out, const Mtxf *parent, f32 roll, f32 scale, f32 ext, const f32 *pos, s32 billboard)
{
	const f32 c = cosf(roll);
	const f32 sn = sinf(roll);
	const f32 local[3][3] = {
		{ c * scale, sn * scale, 0.0f },
		{ -sn * scale, c * scale, 0.0f },
		{ 0.0f, 0.0f, scale * ext },
	};

	for (s32 r = 0; r < 3; r++) {
		for (s32 col = 0; col < 3; col++) {
			// a billboard's axes are the eye's, the gun's are the parent's
			out->m[r][col] = billboard ? local[r][col]
				: local[r][0] * parent->m[0][col] + local[r][1] * parent->m[1][col] + local[r][2] * parent->m[2][col];
		}

		out->m[r][3] = 0.0f;
	}

	for (s32 col = 0; col < 3; col++) {
		out->m[3][col] = billboard ? pos[col]
			: pos[0] * parent->m[0][col] + pos[1] * parent->m[1][col] + pos[2] * parent->m[2][col] + parent->m[3][col];
	}

	out->m[3][3] = 1.0f;
}

/**
 * GoldenEye's own muzzle flash, posed as gunfire.c poses it: GoldenEye never
 * lets the model pose it. The flash (part 3's matrix) is the gun's matrix
 * with a random roll, 1 to 1.25 times the size, and stretched along the barrel
 * by the gun's MuzzleFlashExtension; the star (part 2's, and part 4's on the
 * KF7) faces the eye at its place in the flash, a tenth the size. Posed as a
 * plain model both sat at their rest offsets unturned: a streak lying along
 * the top of the slide.
 */
void gegunsOwnModelFlash(struct hand *hand, struct model *model)
{
	struct modeldef *def = model->definition;
	struct modelnode *base;
	struct modelnode *flash;
	f32 off[3];
	f32 scale;
	f32 ext;
	f32 unit;
	Mtxf *parent;
	Mtxf *flashmtx;

	if (!hand->flashon || !gegunsOwnModelInUse(hand->gset.weaponnum) || !model->matrices) {
		return;
	}

	base = gegunsOwnModelMuzzle(hand->gset.weaponnum, def, off);
	flash = modelGetPart(def, 3);

	if (!base || !flash || modelFindNodeMtxIndex(flash, 0) == modelFindNodeMtxIndex(base, 0)) {
		return;
	}

	parent = &model->matrices[modelFindNodeMtxIndex(base, 0)];
	flashmtx = &model->matrices[modelFindNodeMtxIndex(flash, 0)];
	scale = gegunsRandFrac() * 0.25f + 1.0f;
	ext = g_Weapons[hand->gset.weaponnum]->muzzlez;
	unit = sqrtf(parent->m[0][0] * parent->m[0][0] + parent->m[0][1] * parent->m[0][1] + parent->m[0][2] * parent->m[0][2]);

	gegunsFlashMatrix(flashmtx, parent, gegunsRandFrac() * M_BADTAU, scale, ext, off, 0);

	for (s32 part = 2; part <= 4; part += 2) {
		struct modelnode *star = modelGetPart(def, part);
		f32 at[3];

		if (!star || (star->type & 0xff) != MODELNODETYPE_POSITION
				|| modelFindNodeMtxIndex(star, 0) == modelFindNodeMtxIndex(flash, 0)) {
			continue;
		}

		// its place in the flash's own space, into the eye's
		for (s32 col = 0; col < 3; col++) {
			at[col] = star->rodata->position.pos.x * flashmtx->m[0][col]
				+ star->rodata->position.pos.y * flashmtx->m[1][col]
				+ star->rodata->position.pos.z * flashmtx->m[2][col]
				+ flashmtx->m[3][col];
		}

		gegunsFlashMatrix(&model->matrices[modelFindNodeMtxIndex(star, 0)], NULL,
				gegunsRandFrac() * M_BADTAU, unit * scale, ext, at, 1);
	}
}

/** The first-person model file the gun's own definition names: the borrowed one's, or the host's. */
u16 gegunsModelFile(s32 index)
{
	return borrowed[index] ? borrowedModel[index] : stockDefs[index].hi_model;
}

/** Whether the gun's own definition has hands: the borrowed one's, or the host's. */
u32 gegunsHandsFlag(s32 index)
{
	return borrowed[index] ? borrowedHands[index] : g_Weapons[g_GeWeaponHosts[index]]->flags & WEAPONFLAG_HASHANDS;
}

/**
 * GoldenEye's knives throw a knife, not a poison one. The throw is the combat
 * knife's function, shared with Perfect Dark's own knife, so each copy gets a
 * function of its own under a name of the port's; a borrowed knife's throw
 * takes the name from here (gegunsNameFor()).
 */
static void gegunsNameThrow(s32 i)
{
	// what the HUD calls each one's function: a knife's throw is the combat
	// knife's "Throw Poison Knife" otherwise, and a gadget's its host's - the
	// ECM mine's "Jamming Device", the Data Uplink's "Uplink"
	static const struct { s32 weaponnum; s32 type; const char *text; } rows[] = {
		{ WEAPON_GE_HUNTINGKNIFE,  INVENTORYFUNCTYPE_THROW,   "Throw Knife\n" },
		{ WEAPON_GE_THROWINGKNIFE, INVENTORYFUNCTYPE_THROW,   "Throw Knife\n" },
		{ WEAPON_GE_COVERTMODEM,   INVENTORYFUNCTYPE_THROW,   "Attach\n" },
		{ WEAPON_GE_PLASTIQUE,     INVENTORYFUNCTYPE_THROW,   "Place\n" },
		{ WEAPON_GE_GOLDENEYEKEY,  INVENTORYFUNCTYPE_THROW,   "Put Down\n" },
		{ WEAPON_GE_CAMERA,        INVENTORYFUNCTYPE_SPECIAL, "Photograph\n" },
		{ WEAPON_GE_WATCHMAGNET,   INVENTORYFUNCTYPE_SPECIAL, "Attract\n" },
		{ WEAPON_GE_GADGETA,       INVENTORYFUNCTYPE_SPECIAL, "Use\n" },
		{ WEAPON_GE_GADGETB,       INVENTORYFUNCTYPE_SPECIAL, "Use\n" },
		{ WEAPON_GE_TANKSHELLS,    INVENTORYFUNCTYPE_SPECIAL, "Fire\n" },
	};
	struct weapon *def = &g_GeWeaponDefs[i];
	const s32 weaponnum = WEAPON_GE_FIRST + i;

	for (s32 r = 0; r < (s32)ARRAYCOUNT(rows); r++) {
		if (rows[r].weaponnum != weaponnum) {
			continue;
		}

		for (s32 f = 0; f < 2; f++) {
			const struct weaponfunc *func = def->functions[f];
			struct weaponfunc *copy;

			if (!func || func->type != rows[r].type) {
				continue;
			}

			copy = malloc(gegunsFuncSize(func->type));

			if (copy) {
				memcpy(copy, func, gegunsFuncSize(func->type));
				copy->name = langAddPortText(rows[r].text);
				def->functions[f] = copy;
			}
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
	stockFalcon2 = *g_Weapons[WEAPON_FALCON2];
	stockKnife = *g_Weapons[WEAPON_COMBATKNIFE];
	stockCmp150 = *g_Weapons[WEAPON_CMP150];
	stockAr34 = *g_Weapons[WEAPON_AR34];

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		const struct weapon *host = g_Weapons[g_GeWeaponHosts[i]];
		const s32 hostmodel = gegunsHostModel(i);
		const u16 name = langAddPortText(names[i]);

		g_GeWeaponDefs[i] = *host;
		g_GeWeaponDefs[i].name = name;
		g_GeWeaponDefs[i].shortname = name;

		gegunsApplyStats(i);
		gegunsOwnTrigger(i);

		gegunsNameThrow(i);

		if (WEAPON_GE_FIRST + i == WEAPON_GE_TANKSHELLS) {
			// the tank's shells are counted on the HUD as GoldenEye counts
			// them: ammunition type 0x1d is its AMMO_TANK, still in Perfect
			// Dark's list under no name. Held and in reserve are the one
			// number, as a thrown weapon's are, and getank.c spends them.
			static struct inventory_ammo shells = { AMMOTYPE_1D, CASING_NONE, 1, NULL, AMMOFLAG_EQUIPPEDISRESERVE };

			g_GeWeaponDefs[i].ammos[0] = &shells;
		}

		// what a borrow is undone to
		stockDefs[i] = g_GeWeaponDefs[i];

		// Until gebean.c has the release's pickup to point it at, the host's
		if (hostmodel >= 0 && hostmodel < MODEL_GE_FIRST) {
			g_ModelStates[MODEL_GE_FIRST + i].fileid = g_ModelStates[hostmodel].fileid;
			g_ModelStates[MODEL_GE_FIRST + i].scale = g_ModelStates[hostmodel].scale;
		}
	}
}

#endif

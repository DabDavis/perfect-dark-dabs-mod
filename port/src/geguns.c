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

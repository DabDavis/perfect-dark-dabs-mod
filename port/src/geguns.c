#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "game/lang.h"
#include "lang.h"
#include "game/playermgr.h"
#include "fs.h"
#include "romdata.h"
#include "system.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "lib/rng.h"
#include "geguns.h"
#include "gebean.h"
#include "geslappers.h"
#include "modloader.h"
#include "mod.h"
#include "langpack.h"

#ifndef PLATFORM_N64

/**
 * GoldenEye's guns as weapons of Perfect Dark's own, numbered past the stock
 * table (WEAPON_GE_FIRST).
 *
 * Each stands on the Perfect Dark weapon GoldenEye's gun became in it - the
 * PP7 on the PP9i, the KF7 Soviet on the KF7 Special, the Cougar Magnum on the
 * DY357 - its host, whose model it is drawn on and whose engine runs it: every
 * test of a weapon by number asks about the host (weaponHost()). The
 * definition itself is built field by field (gegunsBuild()), GoldenEye's
 * numbers, ammunition and flags from its own rows. Its first-person model is
 * the host's, or, for the guns gebean.c has checked (fpReady), an alias of it
 * with the release's gun drawn on it, or in the N64 look GoldenEye's own
 * (gegunsOwnModel()). What is its own besides: the number, so it is held, dropped and
 * picked up beside its host rather than as it; GoldenEye's name; a model state
 * (MODEL_GE_FIRST), which gebean.c points at an alias of the host's pickup
 * that the GoldenEye XBLA release's pickup is drawn on; and a Combat
 * Simulator row (MPWEAPON_GE_FIRST), which gebean.c shows when that release
 * is in added-content/ (gebeanGetEnabled()).
 *
 * The definitions are built before anything reads g_Weapons, from the stock
 * definitions: a mod's imported table replaces the stock pointers later and
 * leaves these alone, and a mod's lists keep the rows hidden anyway.
 */

_Static_assert(MODEL_GE_FIRST + NUM_GE_WEAPONS <= MODEL_REMAKE_FIRST,
		"a model state per GoldenEye gun, before the remake's models");
_Static_assert(MPWEAPON_GE_EXTRA1 - MPWEAPON_GE_FIRST == NUM_GE_GUNS
		&& NUM_MPWEAPONS - MPWEAPON_GE_EXTRA1 == NUM_GE_EXTRA,
		"a Combat Simulator row per GoldenEye gun, and per hack's own pistol");
_Static_assert(NUM_WEAPONS <= WEAPON_MPLOCATION00, "a weapon number below the pads' (gunctrl's are s16)");

static const char *const names[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7             - WEAPON_GE_FIRST] = LANG_N("PP7\n"),
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = LANG_N("PP7 (silenced)\n"),
	[WEAPON_GE_DD44            - WEAPON_GE_FIRST] = LANG_N("DD44 Dostovei\n"),
	[WEAPON_GE_KLOBB           - WEAPON_GE_FIRST] = LANG_N("Klobb\n"),
	[WEAPON_GE_KF7SOVIET       - WEAPON_GE_FIRST] = LANG_N("KF7 Soviet\n"),
	[WEAPON_GE_ZMG             - WEAPON_GE_FIRST] = LANG_N("ZMG (9mm)\n"),
	[WEAPON_GE_D5K             - WEAPON_GE_FIRST] = LANG_N("D5K Deutsche\n"),
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = LANG_N("D5K (silenced)\n"),
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = LANG_N("Phantom\n"),
	[WEAPON_GE_AR33            - WEAPON_GE_FIRST] = LANG_N("AR33 Assault Rifle\n"),
	[WEAPON_GE_RCP90           - WEAPON_GE_FIRST] = LANG_N("RC-P90\n"),
	[WEAPON_GE_SHOTGUN         - WEAPON_GE_FIRST] = LANG_N("Shotgun\n"),
	[WEAPON_GE_AUTOSHOTGUN     - WEAPON_GE_FIRST] = LANG_N("Automatic Shotgun\n"),
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = LANG_N("Sniper Rifle\n"),
	[WEAPON_GE_COUGARMAGNUM    - WEAPON_GE_FIRST] = LANG_N("Cougar Magnum\n"),
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = LANG_N("Golden Gun\n"),
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = LANG_N("Moonraker Laser\n"),
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = LANG_N("Grenade Launcher\n"),
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = LANG_N("Rocket Launcher\n"),
	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = LANG_N("Hunting Knife\n"),
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = LANG_N("Throwing Knife\n"),
	[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = LANG_N("Hand Grenade\n"),
	[WEAPON_GE_TIMEDMINE       - WEAPON_GE_FIRST] = LANG_N("Timed Mine\n"),
	[WEAPON_GE_PROXIMITYMINE   - WEAPON_GE_FIRST] = LANG_N("Proximity Mine\n"),
	[WEAPON_GE_REMOTEMINE      - WEAPON_GE_FIRST] = LANG_N("Remote Mine\n"),
	[WEAPON_GE_COVERTMODEM     - WEAPON_GE_FIRST] = LANG_N("Covert Modem\n"),
	[WEAPON_GE_PLASTIQUE       - WEAPON_GE_FIRST] = LANG_N("Plastique\n"),
	[WEAPON_GE_GOLDENEYEKEY    - WEAPON_GE_FIRST] = LANG_N("GoldenEye Key\n"),
	[WEAPON_GE_CAMERA          - WEAPON_GE_FIRST] = LANG_N("Camera\n"),
	[WEAPON_GE_WATCHMAGNET     - WEAPON_GE_FIRST] = LANG_N("Watch Magnet Attract\n"),
	[WEAPON_GE_GADGETA         - WEAPON_GE_FIRST] = LANG_N("Gadget\n"),
	[WEAPON_GE_GADGETB         - WEAPON_GE_FIRST] = LANG_N("Gadget\n"),
	[WEAPON_GE_TANKSHELLS      - WEAPON_GE_FIRST] = LANG_N("Tank\n"),
	[WEAPON_GE_DETONATOR       - WEAPON_GE_FIRST] = LANG_N("Detonator\n"),
	// a ROM hack's own pistols, named by its gun set (gegunsStageSet())
	[WEAPON_GE_EXTRA1          - WEAPON_GE_FIRST] = LANG_N("Pistol\n"),
	[WEAPON_GE_EXTRA2          - WEAPON_GE_FIRST] = LANG_N("Pistol\n"),
	[WEAPON_GE_EXTRA3          - WEAPON_GE_FIRST] = LANG_N("Pistol\n"),
	[WEAPON_GE_EXTRA4          - WEAPON_GE_FIRST] = LANG_N("Pistol\n"),
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
 * Not taken: a thrown weapon's damage, since Perfect Dark's grenade and mines
 * carry 0 there and the explosion does the work - but the throwing knife's
 * is its row's 3, a knife doing its damage itself; and the position on
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
	u8 ammotype;  // GoldenEye's AMMOTYPES index
	u32 bitflags; // GoldenEye's WEAPONSTATBITFLAG_* word; 0 is a gadget's, which has no row
};

#define GUNSTAT(weapon, source, mag, autorate, singlerate, pen, dmg, spread, impact, loudmin, loudmax, pershot, lineartime, scaledtime, \
		speed0, speed1, speed2, speed3, back, up, bolt, sway, zoom, muzzle, ammotype, bitflags) \
	[weapon - WEAPON_GE_FIRST] = { mag, autorate, singlerate, pen, dmg, spread, impact, { loudmin, loudmax, pershot, lineartime, scaledtime }, \
		{ speed0, speed1, speed2, speed3 }, back, up, bolt, sway, zoom, muzzle, ammotype, bitflags }

// GoldenEye's WEAPONSTATBITFLAG_* bits a definition is built from (bondconstants.h)
#define GESTATFLAG_ROLL_FLASH             0x00000001 // WEAPONSTATBITFLAG_00000001 (gunfire.c)
#define GESTATFLAG_HAS_AUTO_AIM           0x00000008
#define GESTATFLAG_CLICKY                 0x00000010
#define GESTATFLAG_HIDE_FIRST_PERSON_HAND 0x00002000
#define GESTATFLAG_ONLY_1_HANDED          0x00000100
#define GESTATFLAG_HIDE_FIRST_PERSON_MENU 0x00004000
#define GESTATFLAG_USE_HOLD_TIME          0x00020000
#define GESTATFLAG_CAN_DUAL_WIELD         0x00100000

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
// GoldenEye's melee reach (chrprop.c): 50 in front of the camera
#define GE_MELEE_REACH 50

static const u8 geShootSounds[NUM_GE_WEAPONS] = {
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

// the gun set's (gegunsStageSet()): GoldenEye's, or a ROM hack's own
static const u8 *shootsounds = geShootSounds;

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
static const u8 geShootSoundRates[NUM_GE_WEAPONS] = {
	[WEAPON_GE_KLOBB - WEAPON_GE_FIRST]       = 11,
	[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST]   = 4,
	[WEAPON_GE_ZMG - WEAPON_GE_FIRST]         = 4,
	[WEAPON_GE_D5K - WEAPON_GE_FIRST]         = 4,
	[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST] = 4,
	[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST]     = 4,
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST]        = 5,
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST]       = 2,
};

static const u8 *shootsoundrates = geShootSoundRates;

/**
 * How GoldenEye words each gun's name when it is picked up: "an AR33 Assault
 * Rifle", "the Golden Gun". A copy took its host's, which gave the covert modem,
 * the plastique and the GoldenEye key the ECM mine's "an".
 */
static const u32 geDeterminers[NUM_GE_WEAPONS] = {
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST]        = WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN,
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST]       = WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN,
	[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST] = WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN,
	[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST]   = WEAPONFLAG_DETERMINER_S_THE | WEAPONFLAG_DETERMINER_F_THE,
};

static const u32 *determiners = geDeterminers;

/**
 * What each makes a pickup sound like: GoldenEye's
 * set_sound_effect_for_weapontype_collection() (gunfire.c) - a knife's, a
 * mine's for the mines and the gadgets it throws like one, ammunition for the
 * grenade, the laser's for the Moonraker and a gun's for everything else, the
 * GoldenEye key and the camera included. The hosts' gave the gadgets on the
 * Data Uplink the keycard's and the key the mine's.
 */
static u16 gegunsPickupSound(s32 i)
{
	switch (WEAPON_GE_FIRST + i) {
	case WEAPON_GE_HUNTINGKNIFE:
	case WEAPON_GE_THROWINGKNIFE:
		return SFX_PICKUP_KNIFE;
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
	case WEAPON_GE_REMOTEMINE:
	case WEAPON_GE_COVERTMODEM:
	case WEAPON_GE_PLASTIQUE:
		return SFX_PICKUP_MINE;
	case WEAPON_GE_GRENADE:
		return SFX_PICKUP_AMMO;
	case WEAPON_GE_MOONRAKER:
		return SFX_PICKUP_LASER;
	}

	return SFX_PICKUP_GUN;
}

/**
 * GoldenEye's ammunition types (bondconstants.h, AMMOTYPES) as the port's.
 *
 * GoldenEye has one pool of 9mm for the PP7, the DD44, the Klobb, the ZMG, the
 * D5K, the Phantom and the RC-P90, and a copy took its host's type, which
 * split it in two: Perfect Dark's pistol rounds for the three pistols and its
 * submachine gun rounds for the rest, so a D5K's ammunition did not load a
 * PP7. They are all the submachine gun's now (800, as GoldenEye's 9mm). The
 * golden bullet has no row of Perfect Dark's to become and stood on the
 * magnum's, so the Cougar Magnum in one hand emptied the Golden Gun in the
 * other (F3 20260925-044735, akimbo: "192" under both); it is a pool of its
 * own now, AMMOTYPE_GOLDENGUN (constants.h), GoldenEye's 100 at most and 3 a
 * pickup. 0 - GoldenEye's AMMO_NONE, or a gadget with no row - keeps the host's.
 */
static const u8 geammotypes[] = {
	[1]  = AMMOTYPE_SMG,         // 9MM
	[2]  = AMMOTYPE_SMG,         // 9MM_2
	[3]  = AMMOTYPE_RIFLE,       // RIFLE
	[4]  = AMMOTYPE_SHOTGUN,     // SHOTGUN
	[5]  = AMMOTYPE_GRENADE,     // GRENADE
	[6]  = AMMOTYPE_ROCKET,      // ROCKETS
	[7]  = AMMOTYPE_REMOTE_MINE, // REMOTEMINE
	[8]  = AMMOTYPE_PROXY_MINE,  // PROXMINE
	[9]  = AMMOTYPE_TIMED_MINE,  // TIMEDMINE
	[10] = AMMOTYPE_KNIFE,       // KNIFE
	[11] = AMMOTYPE_DEVASTATOR,  // GRENADEROUND
	[12] = AMMOTYPE_MAGNUM,      // MAGNUM
	[13] = AMMOTYPE_GOLDENGUN,   // GGUN
};

s32 gegunsShootSoundRate(s32 weaponnum)
{
	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= WEAPON_GE_FIRST + NUM_GE_WEAPONS) {
		return -1;
	}

	return shootsoundrates[weaponnum - WEAPON_GE_FIRST];
}

static s32 gegunsWatchLaserInstalled(void);

// GoldenEye's watchlaser_fire_sounds (gun.c): RICO_LASER2_SFX and
// RICO_LASER3_SFX, one of the two at random with each shot (gunfire.c)
#define GESFX_RICO_LASER2 92

s32 gegunsShootSound(s32 weaponnum)
{
	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= WEAPON_GE_FIRST + NUM_GE_WEAPONS) {
		return 0;
	}

	if (weaponnum == WEAPON_GE_MOONRAKER && gegunsWatchLaserInstalled()) {
		return GESFX_RICO_LASER2 + (rngRandom() & 1);
	}

	return shootsounds[weaponnum - WEAPON_GE_FIRST];
}

static const struct gegunstat geStats[NUM_GE_WEAPONS] = {
#include "gegunstats.h"
};

static const struct gegunstat *stats = geStats;

#undef GUNSTAT

/**
 * GoldenEye's automatic rate as the rounds per minute Perfect Dark counts in.
 *
 * The rate is in GoldenEye's frames, not time: a held trigger fires on every
 * rate'th frame (gunfire.c, field_88C % AutomaticFiringRate), and a guard on
 * every rate'th of its ticks (chraction.c, firecount). So how fast a gun fires
 * is how fast the game draws. Rare's demos, recorded on the console, draw a
 * level in two to five sixtieths a frame and seldom in fewer than two (thirty
 * frames a second), and GoldenEye X arms the same guns at that ceiling. That
 * is the frame taken, two sixtieths: rate 3 (Klobb, KF7, D5K, Phantom) is 600
 * rpm and rate 2 (ZMG, AR33, RC-P90) 900.
 *
 * Perfect Dark's own classic guns fire at 450 and 550-600, a frame of about
 * 2.7 sixtieths - the console's average - and that was the rate here until a
 * tester found the guns slow beside GoldenEye X's. 0xff is not automatic and
 * leaves the host's.
 */
static f32 gegunsRpm(u8 rate)
{
	if (rate == 0 || rate == 0xff) {
		return 0.0f;
	}

	return 3600.0f / (2 * rate);
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
 * A GoldenEye gun's definition, built one field at a time.
 *
 * It was a copy of its host's with GoldenEye's numbers written over it, and
 * whatever nothing wrote over stayed Perfect Dark's without anyone deciding it
 * should: the one-handed flag held a guard's KF7 like a pistol, the inventory
 * described the Cougar Magnum as the DY357, the PP7 turned sideways at close
 * range as the PP9i does, and the pistols drew on other rounds than the
 * submachine guns. Now every field comes from one of three places, and says
 * which:
 *
 * - **GoldenEye's own row** (gegunstats.h): damage, spread, rates, recoil,
 *   noise, magazine, ammunition, zoom, sway, flash, how it is held, whether it
 *   counts towards the weapon of choice.
 * - **The model it is drawn on** (`model`): its file, its animations and part
 *   commands, where it sits, and the script pointers its functions and
 *   magazines carry - a fire or reload script names the model's own parts and
 *   animations. That is the host's, since the release's gun is skinned onto the
 *   host's model in the HD look, or GoldenEye X's when one is borrowed; the
 *   N64 look draws GoldenEye's own model over it (gegunsSetOwnModelInUse()).
 * - **Perfect Dark's engine** (`engine`, the host): what the port's code asks
 *   and GoldenEye has no number for - the kind of each function, a
 *   projectile's flight, a throw's fuse, flags2 and flags3 (less a knife's
 *   sticking and poison). The pickup sound is GoldenEye's.
 *
 * gegunsDump() writes all of it out, and a change to this is checked by the
 * difference between two dumps.
 */
static u16 geNameIds[NUM_GE_WEAPONS];
static u16 *nameids = geNameIds;

static struct noisesettings *gegunsNoise(s32 i)
{
	const struct gegunstat *stat = &stats[i];
	struct noisesettings *noise;

	// A row with no times is a gadget's, which has no row, and the noise code
	// divides by them
	if (stat->noise.decbasespeed <= 0.0f || stat->noise.decremspeed <= 0.0f) {
		return NULL;
	}

	noise = malloc(sizeof(*noise));

	if (noise) {
		*noise = stat->noise;
	}

	return noise;
}

/**
 * How long GoldenEye waits between two shots of a single-shot gun with the
 * trigger held, in sixtieths, and the recovery time that gives the same wait
 * here.
 *
 * gunTickHandState() (gunfire.c) runs once a frame. It fires in
 * GUN_ANIM_STATE_FIRE, goes to RECOIL1 the next frame (field_890 back to 0),
 * adds the frame's ticks (g_ClockTimer) to field_890 each frame after, goes
 * back to IDLE once field_890 reaches the two recoil speeds plus SingleRate,
 * and fires again from IDLE the frame after that. The Cougar and the grenade
 * launcher first wait 6 ticks in TRIGGER_PRESS. So the wait is counted in
 * frames and depends on how long a frame is: at GoldenEye's two sixtieths a
 * frame, the console's usual and the frame the automatic rates are counted
 * in here (gegunsRpm(), the KF7's 600 rpm), it is 2 * ceil(T / 2) + 4 ticks.
 * The native port run at two ticks a frame agrees to the tick for every gun
 * (PP7 32, shotguns 40, sniper rifle 20, Cougar 54, Golden Gun and Moonraker
 * 16, grenade launcher 54, rocket launcher 24, watch laser 4).
 *
 * Perfect Dark's bgun0f09aba4() counts ticks and lets the next shot go
 * `sum + recoverytime60` ticks after the last, plus one tick more for a gun
 * with a fire animation to start (GEGUNS_PD_SHOT_OVERHEAD, measured: the PP7,
 * the DD44 and both launchers against the Golden Gun, the Moonraker, the
 * sniper rifle and the automatic shotgun). A fire animation longer than the
 * wait holds the next shot back itself, which is why the Shotgun's pump and
 * the Cougar's kick are gone (gegunsOwnTrigger()). A recovery of SingleRate
 * itself fired every GoldenEye gun early - the PP7 at 29 ticks for 32, the
 * sniper rifle at 16 for 20 - and the watch laser at every tick for 4. Ticks,
 * not frames, so a faster frame rate does not change it.
 */
#ifndef GEGUNS_AR33_ADS_LIFT
#define GEGUNS_AR33_ADS_LIFT -1.0f
#endif
#define GEGUNS_GE_FRAME_TICKS    2
#define GEGUNS_PD_SHOT_OVERHEAD  1

/**
 * How long GoldenEye holds a shot back after the trigger is pressed, in
 * sixtieths: the Cougar and the grenade launcher sit in GUN_ANIM_STATE_TRIGGER_PRESS
 * until field_890 reaches 6 (gunfire.c) - the Cougar cocking its hammer - and
 * only then fire. Every other gun fires on the press. bgunTickIncAttackingShoot()
 * waits this long before the shot, and gegunsRecovery() takes it off the wait
 * after, so a held trigger still fires at GoldenEye's rate.
 */
s32 gegunsTriggerDelay60(s32 weaponnum)
{
	if (weaponnum == WEAPON_GE_COUGARMAGNUM || weaponnum == WEAPON_GE_GRENADELAUNCHER) {
		return 6;
	}

	return 0;
}

/**
 * How often one of GoldenEye's guns clicks held empty, in sixtieths: its
 * DRY_FIRE state runs 20 (gunfire.c's WHEN_D_FLD890) from the click, IDLE
 * takes a frame and TRIGGER_PRESS, after the Cougar's and the grenade
 * launcher's wait, clicks again. The cartridge: 22 on most, 30 on the Cougar.
 * 0 for a gun that is not GoldenEye's or has no magazine to run dry.
 */
s32 gegunsDryFireInterval60(s32 weaponnum)
{
	if (gegunsClicksEmpty(weaponnum) <= 0) {
		return 0;
	}

	return 20 + GEGUNS_GE_FRAME_TICKS + gegunsTriggerDelay60(weaponnum);
}

/**
 * Whether one of GoldenEye's guns clicks when its trigger is held empty: 1,
 * or 0 for one that reloads instead - the grenade, the mines, the throwing
 * knife, whose rows lack WEAPONSTATBITFLAG_CLICKY, go from an empty hand to
 * RELOAD_START with the trigger still held (gunfire.c), where Perfect Dark
 * dry-fires them until it lets go. -1 for a weapon that is not GoldenEye's.
 */
s32 gegunsClicksEmpty(s32 weaponnum)
{
	if (!GE_GUN_INDEX(weaponnum - WEAPON_GE_FIRST) || stats[weaponnum - WEAPON_GE_FIRST].bitflags == 0) {
		return -1;
	}

	return (stats[weaponnum - WEAPON_GE_FIRST].bitflags & GESTATFLAG_CLICKY) != 0;
}

/**
 * GoldenEye's grenade and mines: held and thrown by GoldenEye's rule rather
 * than their hosts' (bondgun.c's bgunTickIncAttackingGeThrow()).
 */
s32 gegunsThrowsAsGoldenEye(s32 weaponnum)
{
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
 * GoldenEye's three mines, which stick where they first land (propobj.c's
 * thrown weapon tick: embedded on contact, ATTACH_MINE_SFX).
 */
s32 gegunsMineAttaches(s32 weaponnum)
{
	return weaponnum == WEAPON_GE_TIMEDMINE || weaponnum == WEAPON_GE_PROXIMITYMINE
		|| weaponnum == WEAPON_GE_REMOTEMINE;
}

/**
 * Whether GoldenEye draws nothing of this in the hand
 * (WEAPONSTATBITFLAG_HIDE_FIRST_PERSON_HAND: the grenade, the mines), so that
 * its reload neither lowers nor raises anything: RELOAD_LOWER ends at once and
 * RELOAD_RAISE goes straight to idle (gunfire.c), 17 ticks of RELOAD_SWAP
 * between the throw's recovery and the next one ready.
 */
s32 gegunsHidesHand(s32 weaponnum)
{
	if (!GE_GUN_INDEX(weaponnum - WEAPON_GE_FIRST)) {
		return 0;
	}

	return (stats[weaponnum - WEAPON_GE_FIRST].bitflags & GESTATFLAG_HIDE_FIRST_PERSON_HAND) != 0;
}

static s32 gegunsGeSingleWait(s32 weaponnum, const struct gegunstat *stat)
{
	s32 speeds = stat->recoilspeed[0] + stat->recoilspeed[1];
	s32 t = speeds + (s8)stat->singlerate;
	s32 frames = (t + GEGUNS_GE_FRAME_TICKS - 1) / GEGUNS_GE_FRAME_TICKS + 2;

	if (t < 0) {
		frames = 2;
	}

	// the Cougar and the grenade launcher's TRIGGER_PRESS wait, field_890 >= 6
	if (weaponnum == WEAPON_GE_COUGARMAGNUM || weaponnum == WEAPON_GE_GRENADELAUNCHER) {
		frames += (6 + GEGUNS_GE_FRAME_TICKS - 1) / GEGUNS_GE_FRAME_TICKS;
	}

	return frames * GEGUNS_GE_FRAME_TICKS;
}

/**
 * How much more (or less) than the 3.5 every gun is raised by a GoldenEye gun
 * comes up to the eye under COD Style Aiming. The AR33's carry handle stands
 * over its bore as a solid block with no aperture through it, in GoldenEye's
 * own model and the release's alike, and raised as far as the rest the eye
 * sat behind its rear end: the handle filled the middle of the view under the
 * crosshair and hid the front sight and what it was on (F3 20260930-032244,
 * "ads doesn't actually look down its iron sights, they're obscured by the
 * top rail"). Held lower, the sight line passes over the handle.
 */
f32 gegunsCodAimLift(s32 weaponnum)
{
	if (weaponnum == WEAPON_GE_AR33) {
		return GEGUNS_AR33_ADS_LIFT;
	}

	return 0.0f;
}

/**
 * How long after a single shot GoldenEye's gun is ready again when the trigger
 * has been let go since, as bgun0f09aba4() counts it (ticks since the shot), or
 * -1 where Perfect Dark's own rule stands (not a GoldenEye gun, or an automatic).
 *
 * gunTickHandState() goes back to IDLE from RECOIL1 once field_890 reaches the
 * two recoil speeds plus SingleRate - unless the trigger was released since
 * the shot (field_888), when the two speeds are enough. So a tapped Cougar
 * fires every 34 ticks and a held one every 54: at GoldenEye's two sixtieths a
 * frame it is 2 * ceil(speeds / 2) + 4, and the Cougar and the grenade
 * launcher still wait their 6 in TRIGGER_PRESS, which the next shot here waits
 * itself (gegunsTriggerDelay60()). Perfect Dark lets a released trigger cut
 * the recovery short only where the fourth recoil speed is not negative, and
 * only once the trigger is pressed again: the Cougar's is -1, and a tapped
 * Cougar fired at its held rate (F3 20260929-212222, "time between shots on
 * magnum is too slow").
 */
s32 gegunsReleasedReady(s32 weaponnum, s32 speeds, s32 hasanim)
{
	s32 t;

	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= WEAPON_GE_FIRST + NUM_GE_WEAPONS) {
		return -1;
	}

	if (!stats[weaponnum - WEAPON_GE_FIRST].bitflags || stats[weaponnum - WEAPON_GE_FIRST].autorate != 0xff) {
		return -1;
	}

	if (speeds < 0) {
		speeds = 0;
	}

	t = ((speeds + GEGUNS_GE_FRAME_TICKS - 1) / GEGUNS_GE_FRAME_TICKS + 2) * GEGUNS_GE_FRAME_TICKS
		- (hasanim && !gegunsTriggerDelay60(weaponnum) ? GEGUNS_PD_SHOT_OVERHEAD : 0);

	return t < speeds ? speeds : t;
}

static s8 gegunsRecovery(s32 weaponnum, const struct gegunstat *stat, s32 hasanim)
{
	s32 speeds = stat->recoilspeed[0] + stat->recoilspeed[1];
	s32 rec;

	if (speeds < 1) {
		speeds = 0;
	}

	// a fire animation's tick to start runs inside the trigger's wait
	rec = gegunsGeSingleWait(weaponnum, stat) - speeds
		- (hasanim && !gegunsTriggerDelay60(weaponnum) ? GEGUNS_PD_SHOT_OVERHEAD : 0)
		- gegunsTriggerDelay60(weaponnum);

	return rec < 0 ? 0 : (rec > 127 ? 127 : rec);
}

/** Function f of gun i: its kind and scripts the model's, its numbers GoldenEye's. */
static struct weaponfunc *gegunsFunc(s32 i, s32 f, const struct weaponfunc *src, struct noisesettings *noise)
{
	const struct gegunstat *stat = &stats[i];
	const s32 hasrow = stat->bitflags != 0;
	u32 type = src->type;
	struct weaponfunc *fn;

	// A ROM hack's gun fires as its row says where its host's does not:
	// Goldfinger 64's Karabiner 98k and M1 Carbine are single shot on the
	// Phantom's and the RC-P90's automatic hosts. GoldenEye's own guns all
	// fire as their hosts do.
	if (hasrow && f == 0 && (type == INVENTORYFUNCTYPE_SHOOT_SINGLE || type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC)) {
		type = stat->autorate != 0xff ? INVENTORYFUNCTYPE_SHOOT_AUTOMATIC : INVENTORYFUNCTYPE_SHOOT_SINGLE;
	}

	fn = calloc(1, gegunsFuncSize(type) > gegunsFuncSize(src->type) ? gegunsFuncSize(type) : gegunsFuncSize(src->type));

	if (!fn) {
		return (struct weaponfunc *)src;
	}

	fn->type = type;
	fn->name = src->name;
	fn->ammoindex = src->ammoindex;
	fn->fire_animation = src->fire_animation; // the model's
	fn->flags = src->flags;

	// GoldenEye's gun has the one noise whatever it is doing
	fn->noisesettings = noise && (f == 0 || (src->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) ? noise : src->noisesettings;

	switch (src->type & 0xff) {
	case INVENTORYFUNCTYPE_SHOOT: {
		const struct weaponfunc_shoot *from = (const struct weaponfunc_shoot *)src;
		struct weaponfunc_shoot *shoot = (struct weaponfunc_shoot *)fn;

		// the hands' kick on the model: GoldenEye's pull back and kick up are
		// recoildist and recoilangle below, this is where the model moves
		shoot->recoilsettings = from->recoilsettings;
		shoot->duration60 = from->duration60; // gegunsShootSoundRate() on a converted level
		shoot->shootsound = from->shootsound; // gegunsShootSound() on a converted level

		shoot->damage = hasrow ? stat->damage : from->damage;
		shoot->spread = hasrow ? stat->spread : from->spread;
		shoot->penetration = hasrow ? stat->penetration : from->penetration;
		shoot->impactforce = hasrow ? stat->impactforce : from->impactforce;
		shoot->recoildist = hasrow ? stat->recoilback : from->recoildist;
		shoot->recoilangle = hasrow ? stat->recoilup : from->recoilangle;
		shoot->slidemax = hasrow ? stat->boltback : from->slidemax;

		// 0xff is GoldenEye's "no rate", not a time. A single-shot gun's is
		// GoldenEye's wait between two held shots (gegunsRecovery()); an
		// automatic's held rate is its rpm below and keeps SingleRate
		if (hasrow && stat->singlerate != 0xff && stat->autorate == 0xff) {
			shoot->recoverytime60 = gegunsRecovery(WEAPON_GE_FIRST + i, stat, fn->fire_animation != NULL);
		} else {
			shoot->recoverytime60 = hasrow && stat->singlerate != 0xff ? (s8)stat->singlerate : from->recoverytime60;
		}

		// GoldenEye's own recoil for every gun, the Shotgun's too: it kept
		// its host's pump timing while it worked the pump (75 ticks a shot
		// for GoldenEye's 40), which it no longer does (gegunsOwnTrigger())
		if (hasrow) {
			shoot->unk24 = stat->recoilspeed[0];
			shoot->unk25 = stat->recoilspeed[1];
			shoot->unk26 = stat->recoilspeed[2];
			shoot->unk27 = stat->recoilspeed[3];
		} else {
			shoot->unk24 = from->unk24;
			shoot->unk25 = from->unk25;
			shoot->unk26 = from->unk26;
			shoot->unk27 = from->unk27;
		}

		if (type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC && src->type != INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
			// an automatic on a single-shot host: no barrel of its own to spin
			struct weaponfunc_shootauto *autofn = (struct weaponfunc_shootauto *)fn;

			autofn->initialrpm = gegunsRpm(stat->autorate);
			autofn->maxrpm = autofn->initialrpm;
		} else if (type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
			const struct weaponfunc_shootauto *afrom = (const struct weaponfunc_shootauto *)src;
			struct weaponfunc_shootauto *autofn = (struct weaponfunc_shootauto *)fn;
			const f32 rpm = hasrow ? gegunsRpm(stat->autorate) : 0.0f;

			autofn->initialrpm = rpm > 0.0f ? rpm : afrom->initialrpm;
			autofn->maxrpm = rpm > 0.0f ? rpm : afrom->maxrpm;
			autofn->vibrationstart = afrom->vibrationstart; // the model's barrel
			autofn->vibrationmax = afrom->vibrationmax;
			autofn->turretaccel = afrom->turretaccel;
			autofn->turretdecel = afrom->turretdecel;
		} else if (src->type == INVENTORYFUNCTYPE_SHOOT_PROJECTILE) {
			// GoldenEye's grenade and rocket fly by its code, not by numbers in the row
			const struct weaponfunc_shootprojectile *pfrom = (const struct weaponfunc_shootprojectile *)src;
			struct weaponfunc_shootprojectile *proj = (struct weaponfunc_shootprojectile *)fn;

			proj->projectilemodelnum = pfrom->projectilemodelnum;
			proj->scale = pfrom->scale;
			proj->speed = pfrom->speed;
			proj->unk50 = pfrom->unk50;
			proj->traveldist = pfrom->traveldist;
			proj->timer60 = pfrom->timer60;
			proj->reflectangle = pfrom->reflectangle;
			proj->soundnum = pfrom->soundnum;
		}

		// A gun GoldenEye fires automatically on one Perfect Dark does not, or
		// the other way round, would fire at the wrong rate or not at all
		if (hasrow && f == 0 && (stat->autorate != 0xff) != (fn->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC)) {
			sysLogPrintf(LOG_WARNING, "geguns: weapon %02x is %s in GoldenEye and its function %04x is not",
					WEAPON_GE_FIRST + i, stat->autorate != 0xff ? "automatic" : "single shot", src->type);
		}
		break;
	}
	case INVENTORYFUNCTYPE_THROW: {
		// a thrown weapon's damage is 0 in every one of Perfect Dark's and the
		// explosion does the work; the fuse is the engine's
		const struct weaponfunc_throw *from = (const struct weaponfunc_throw *)src;
		struct weaponfunc_throw *thr = (struct weaponfunc_throw *)fn;

		thr->projectilemodelnum = from->projectilemodelnum;
		thr->activatetime60 = from->activatetime60;
		thr->recoverytime60 = from->recoverytime60;
		// GoldenEye's throwing knife hurts by its row's Destruction, 3, as
		// a bullet does (propobj.c's thrown knife goes through
		// handles_shot_actors); Perfect Dark's combat knife throw is 1
		thr->damage = hasrow && WEAPON_GE_FIRST + i == WEAPON_GE_THROWINGKNIFE ? stat->damage : from->damage;
		break;
	}
	case INVENTORYFUNCTYPE_MELEE: {
		const struct weaponfunc_melee *from = (const struct weaponfunc_melee *)src;
		struct weaponfunc_melee *melee = (struct weaponfunc_melee *)fn;

		// GoldenEye's knife is a 3 against Perfect Dark's 2, and reaches
		// 50 in front of the camera (chrprop.c's melee test, camera space z
		// as Perfect Dark's is) against the combat knife's 70
		melee->damage = hasrow ? stat->damage : from->damage;
		melee->range = hasrow ? GE_MELEE_REACH : from->range;
		break;
	}
	case INVENTORYFUNCTYPE_SPECIAL: {
		const struct weaponfunc_special *from = (const struct weaponfunc_special *)src;
		struct weaponfunc_special *special = (struct weaponfunc_special *)fn;

		special->specialfunc = from->specialfunc;
		special->recoverytime60 = from->recoverytime60;
		special->soundnum = from->soundnum;
		break;
	}
	case INVENTORYFUNCTYPE_DEVICE:
		((struct weaponfunc_device *)fn)->device = ((const struct weaponfunc_device *)src)->device;
		break;
	}

	return fn;
}

/**
 * Magazine a of gun i: GoldenEye's type and size, the model's casing and
 * reload script. A knife's or a mine's MagSize is how many are carried rather
 * than a clip and the Moonraker has none, so only a gun's is a clip; the
 * second slot is a second kind of ammunition, not this one.
 */
static struct inventory_ammo *gegunsAmmo(s32 i, s32 a, const struct inventory_ammo *src, const struct weapon *engine, s32 shoots)
{
	const struct gegunstat *stat = &stats[i];
	struct inventory_ammo *ammo;
	u32 type = 0;

	if (!src) {
		return NULL;
	}

	// GoldenEye's AMMO_NONE on a gun that has a row (the hunting knife; the
	// Moonraker's model has none anyway): nothing to carry or run out of. The
	// hunting knife kept the combat knife's knives, which it shared with the
	// throwing knife, so throwing the last of those took it out of the
	// inventory too (bondgun.c's pass for spent throwables)
	if (a == 0 && stat->bitflags && stat->ammotype == 0) {
		return NULL;
	}

	ammo = calloc(1, sizeof(*ammo));

	if (!ammo) {
		return (struct inventory_ammo *)src;
	}

	if (a == 0 && stat->ammotype < ARRAYCOUNT(geammotypes)) {
		type = geammotypes[stat->ammotype];
	}

	if (!type) {
		type = engine->ammos[a] ? engine->ammos[a]->type : src->type;
	}

	ammo->type = type;
	ammo->casingeject = src->casingeject;           // the model's
	ammo->reload_animation = src->reload_animation; // the model's
	ammo->flags = src->flags;
	ammo->clipsize = a == 0 && shoots && stat->magsize > 0 ? stat->magsize : src->clipsize;

	return ammo;
}

/**
 * How gun i aims: GoldenEye's zoom and auto-aim, and no lock-on (GoldenEye has none); how
 * far the model moves while aiming is the model's. The sniper rifle's zoom is
 * the player's own, wound in and out (currentPlayerGetGunZoomFov()), so it
 * keeps the model's.
 */
static struct invaimsettings *gegunsAim(s32 i, const struct invaimsettings *src, s32 shoots)
{
	const struct gegunstat *stat = &stats[i];
	struct invaimsettings *aim;

	if (!src) {
		return NULL;
	}

	aim = calloc(1, sizeof(*aim));

	if (!aim) {
		return (struct invaimsettings *)src;
	}

	aim->zoomfov = shoots && WEAPON_GE_FIRST + i != WEAPON_GE_SNIPERRIFLE ? stat->zoom : src->zoomfov;
	aim->guntransup = src->guntransup;
	aim->guntransdown = src->guntransdown;
	aim->guntransside = src->guntransside;
	aim->aimdamppal = src->aimdamppal;
	aim->aimdamp = src->aimdamp;
	aim->tracktype = SIGHTTRACKTYPE_DEFAULT;
	aim->flags = src->flags;

	// Auto-aim is GoldenEye's WEAPONSTATBITFLAG_HAS_AUTO_AIM, which the
	// launchers, the knives, the grenade and the mines lack
	if (stat->bitflags) {
		aim->flags &= ~INVAIMFLAG_AUTOAIM;

		if (stat->bitflags & GESTATFLAG_HAS_AUTO_AIM) {
			aim->flags |= INVAIMFLAG_AUTOAIM;
		}
	}

	return aim;
}

/**
 * Gun i's flags. GoldenEye's own bits decide how it is held
 * (WEAPONSTATBITFLAG_ONLY_1_HANDED, which its weaponIsOneHanded() reads to
 * choose a pistol's stand, run and fire or a rifle's), whether it counts
 * towards the weapon of choice and whether its model is kept out of the
 * inventory; its name decides "an" and "the". Two of Perfect Dark's features
 * GoldenEye never had are off: turning a pistol sideways at close range and
 * the red box round a target in aim mode. The rest say what the model is
 * (hands, a flipped left gun, the environment map, parts to switch) or what the
 * engine does with it (who can use it, whether it drops, throwing, gadgets),
 * and are the model's.
 *
 * Not GoldenEye's CAN_DUAL_WIELD: it is only read when the all-guns cheat is
 * on (bondinvItemAvailableForHand()), and marks the sniper rifle and both
 * launchers too, while WEAPONFLAG_DUALWIELD is whether picking up a second
 * one puts it in the other hand - which is the model's.
 */
static u32 gegunsFlags(s32 i, u32 modelflags)
{
	const u32 bits = stats[i].bitflags;
	u32 flags = modelflags;

	flags &= ~(WEAPONFLAG_GANGSTA | WEAPONFLAG_AIMTRACK);
	flags &= ~(WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN
			| WEAPONFLAG_DETERMINER_S_THE | WEAPONFLAG_DETERMINER_F_THE
			| WEAPONFLAG_DETERMINER_S_SOME | WEAPONFLAG_DETERMINER_F_SOME);
	flags |= determiners[i];

	// A gadget has no row and is held in one hand
	if (!bits) {
		return flags | WEAPONFLAG_ONEHANDED;
	}

	flags &= ~(WEAPONFLAG_ONEHANDED | WEAPONFLAG_TRACKTIMEUSED | WEAPONFLAG_HIDEMENUMODEL);

	if (bits & GESTATFLAG_ONLY_1_HANDED) {
		flags |= WEAPONFLAG_ONEHANDED;
	}

	if (bits & GESTATFLAG_USE_HOLD_TIME) {
		flags |= WEAPONFLAG_TRACKTIMEUSED;
	}

	if (bits & GESTATFLAG_HIDE_FIRST_PERSON_MENU) {
		flags |= WEAPONFLAG_HIDEMENUMODEL;
	}

	return flags;
}

/**
 * Builds g_GeWeaponDefs[i] (see above): drawn on `model`, run by `engine`.
 * Both are the host's, unless GoldenEye X's gun is borrowed as the model.
 */
static void gegunsBuild(s32 i, const struct weapon *model, const struct weapon *engine)
{
	const struct gegunstat *stat = &stats[i];
	struct weapon *def = &g_GeWeaponDefs[i];
	struct noisesettings *noise = gegunsNoise(i);
	s32 shoots = 0;

	memset(def, 0, sizeof(*def));

	// the model it is drawn on
	def->hi_model = model->hi_model;
	def->lo_model = model->lo_model;
	def->equip_animation = model->equip_animation;
	def->unequip_animation = model->unequip_animation;
	def->pritosec_animation = model->pritosec_animation;
	def->sectopri_animation = model->sectopri_animation;
	def->posx = model->posx;
	def->posy = model->posy;
	def->posz = model->posz;
	def->gunviscmds = model->gunviscmds;
	def->partvisibility = model->partvisibility;

	// GoldenEye's name, and nothing more: it has no maker or description, and
	// a copy showed the host's in the inventory
	def->shortname = nameids[i];
	def->name = nameids[i];
	def->manufacturer = L_GUN_000;
	def->description = L_GUN_000;

	for (s32 f = 0; f < 2; f++) {
		const struct weaponfunc *src = model->functions[f];

		def->functions[f] = src ? gegunsFunc(i, f, src, noise) : NULL;

		if (src && (src->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
			shoots = 1;
		}
	}

	for (s32 a = 0; a < 2; a++) {
		def->ammos[a] = gegunsAmmo(i, a, model->ammos[a], engine, shoots);
	}

	def->aimsettings = gegunsAim(i, model->aimsettings, shoots);

	// How it sits in the hand; a gadget has no row and keeps the model's
	def->sway = shoots ? stat->sway : model->sway;
	def->muzzlez = shoots ? stat->muzzle : model->muzzlez;

	def->flags = gegunsFlags(i, model->flags);

	// the engine's: flags2 and flags3 name behaviour of the port's own
	def->flags2 = engine->flags2;
	def->flags3 = engine->flags3;
	def->unequippedreloadindex = engine->unequippedreloadindex;
	def->pickupsound = gegunsPickupSound(i);

	// A thrown knife of GoldenEye's does its damage and falls: it neither
	// stays where it lands nor poisons, as the combat knife's does
	if (WEAPON_GE_FIRST + i == WEAPON_GE_HUNTINGKNIFE || WEAPON_GE_FIRST + i == WEAPON_GE_THROWINGKNIFE) {
		def->flags2 &= ~(WEAPONFLAG2_STICKSTOWALL | WEAPONFLAG2_POISONS);
	}

	// Thrown, the GoldenEye key lands and lies where it falls. It stands on
	// the ECM mine, which sticks to whatever it meets, but GoldenEye's list
	// of what embeds (propobj.c: remote, timed and proximity mines, the bomb
	// case, the bug, the micro camera and plastique) leaves the key out, and
	// thrown against Bunker's walls it hung there (F3 20260929-094251)
	if (WEAPON_GE_FIRST + i == WEAPON_GE_GOLDENEYEKEY) {
		def->flags2 &= ~WEAPONFLAG2_STICKSTOWALL;

		for (s32 f = 0; f < 2; f++) {
			struct weaponfunc *fn = def->functions[f];

			if (fn && fn != model->functions[f]) {
				fn->flags &= ~FUNCFLAG_STICKTOWALL;
			}
		}
	}

	// The gun's Combat Simulator row hands out its ammunition - Start Armed
	// fills it, a match's crate beside the gun carries it - and the rows
	// still named the hosts' types: the PP7s' and the DD44's pistol rounds,
	// where the gun loads GoldenEye's one 9mm pool (geammotypes, the
	// submachine gun's), so Start Armed gave them nothing to fire (F3
	// 20261003-003052). The row follows what the gun loads.
	{
		s32 row = -1;

		if (i < NUM_GE_GUNS) {
			row = MPWEAPON_GE_FIRST + i;
		} else if (i >= WEAPON_GE_EXTRA1 - WEAPON_GE_FIRST && i < NUM_GE_WEAPONS) {
			row = MPWEAPON_GE_EXTRA1 + i - (WEAPON_GE_EXTRA1 - WEAPON_GE_FIRST);
		}

		if (row >= 0 && g_MpWeapons[row].weaponnum == WEAPON_GE_FIRST + i
				&& g_MpWeapons[row].priammotype > 0 && def->ammos[0] && def->ammos[0]->type > 0) {
			g_MpWeapons[row].priammotype = def->ammos[0]->type;
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
 * own; a gun performing another gun's is not. So nothing is lent: a gun's
 * reload script is its model's (gegunsAmmo()).
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
 *   detector. GoldenEye X has none of them. It kept the Cougar's whip and a
 *   knife's throw, which GoldenEye has not: the Cougar has no second
 *   function, the sniper rifle not the host's crouch, the hunting knife only
 *   slashes and the throwing knife only throws (gunfire.c's ITEM_KNIFE and
 *   ITEM_THROWKNIFE), so its throw is its first. The remote mine's detonate
 *   goes too, to an item of its own (gegunsOwnThrown()). It was more than a
 *   spare button: the choice of function is saved per *host*
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
 * - **A muzzle flash** on the silenced guns and the rocket launcher, whose
 *   GoldenEye models have no flash to show. The grenade launcher keeps its
 *   flash: its model has one (the switch on part 1) and gunfire.c lights it
 *   on every shot, as it does the rocket launcher's, which has none (F3
 *   20260928-002238, "no muzzle flash when fired").
 *
 * - **The Golden Gun's magnum.** It stands on the DY357-LX, and the model's
 *   scripts it took were the magnum's: every shot played the revolver's kick,
 *   and with GoldenEye's clip of one every shot was followed by the revolver's
 *   reload - the cylinder swung out, six cases thrown from it (the revolver
 *   flag) and a speed loader - so it fired like a magnum (F3
 *   20260925-044735). GoldenEye's Golden Gun has no animation at all: its
 *   recoil numbers kick it (bgun0f09aba4(), from its row), and it reloads the
 *   way every GoldenEye gun does, lowered out of sight and raised again, which
 *   is what Perfect Dark does for a gun with no reload script
 *   (HANDSTATEMINOR_RELOAD_LOWER). Nor does GoldenEye top a holstered gun's
 *   clip up over time, which the magnums do (WEAPONFLAG2_UNEQUIPPEDRELOAD).
 *   The Cougar keeps its host's: it is the revolver GoldenEye's is.
 *
 * And, as GoldenEye X has them: the Cougar's pistol whip does not leave its
 * victim dizzy, and a knife is thrown where it is aimed and not where auto-aim
 * would put it. (Lock-on and "an"/"the" are gegunsBuild()'s, from
 * GoldenEye's own data.)
 */
static void gegunsFireRate(s32 i);

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
	case WEAPON_GE_COUGARMAGNUM:
	case WEAPON_GE_SNIPERRIFLE:
	case WEAPON_GE_HUNTINGKNIFE:
		def->functions[1] = NULL;
		def->flags &= ~WEAPONFLAG_THROWABLE;
		break;
	case WEAPON_GE_THROWINGKNIFE:
		// its only use is the throw (gunfire.c's ITEM_THROWKNIFE), which the
		// combat knife carries second
		def->functions[0] = def->functions[1];
		def->functions[1] = NULL;
		break;
	}

	// gegunsBuild() gave every function a copy of its own
	for (s32 f = 0; f < 2; f++) {
		struct weaponfunc *func = def->functions[f];

		if (!func) {
			continue;
		}

		switch (weaponnum) {
		case WEAPON_GE_PP7SILENCED:
		case WEAPON_GE_D5KSILENCED:
		case WEAPON_GE_ROCKETLAUNCHER:
			if ((func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
				func->flags |= FUNCFLAG_NOMUZZLEFLASH;
			}
			break;
		case WEAPON_GE_AUTOSHOTGUN:
			func->fire_animation = NULL;
			break;
		case WEAPON_GE_SHOTGUN:
			// GoldenEye's Shotgun does not pump: its recoil kicks it and it
			// fires again 40 ticks after (gunfire.c). The host's pump held
			// every shot back to 75
			if ((func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
				func->fire_animation = NULL;
			}
			break;
		case WEAPON_GE_COUGARMAGNUM:
			if (func->type == INVENTORYFUNCTYPE_MELEE) {
				func->flags &= ~FUNCFLAG_MAKEDIZZY;
			}

			// nor does the Cougar play a kick of its own: the DY357's held
			// every shot back to 64 ticks for GoldenEye's 54
			if ((func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
				func->fire_animation = NULL;
			}
			break;
		case WEAPON_GE_HUNTINGKNIFE:
		case WEAPON_GE_THROWINGKNIFE:
			if (func->type == INVENTORYFUNCTYPE_THROW) {
				func->flags |= FUNCFLAG_NOAUTOAIM;
			}

			// GoldenEye throws its knife straight, 25 a sixtieth along the
			// aim and 5 up (gun.c's generate_player_thrown_knife(),
			// gegunsThrowSpeed()), not on the combat knife's trajectory to the
			// aim point at 21.7. And the hand is free again 18 sixtieths after
			// the knife has gone (THROWKNIFE_RECOVER's 16 and the tick or two
			// idle before RELOAD_SWAP, measured on the native port) where the
			// combat knife's recovery is 60 from the start of the throw, the
			// knife leaving at 15: the next knife came up 40 later than
			// GoldenEye's
			if (weaponnum == WEAPON_GE_THROWINGKNIFE && func->type == INVENTORYFUNCTYPE_THROW) {
				struct weaponfunc_throw *throwfunc = (struct weaponfunc_throw *)func;

				func->flags &= ~FUNCFLAG_CALCULATETRAJECTORY;
				throwfunc->recoverytime60 = 15 + 18;
			}
			break;
		case WEAPON_GE_GOLDENGUN:
			func->fire_animation = NULL;
			break;
		}
	}

	if (weaponnum == WEAPON_GE_GOLDENGUN) {
		const struct weapon *host = g_Weapons[g_GeWeaponHosts[i]];

		// gegunsAmmo() gave it a magazine of its own, unless it ran out of memory
		if (def->ammos[0] && def->ammos[0] != host->ammos[0]) {
			def->ammos[0]->reload_animation = NULL;
		}

		def->flags2 &= ~WEAPONFLAG2_UNEQUIPPEDRELOAD;
		def->flags3 &= ~WEAPONFLAG3_REVOLVER;
	}

	// GoldenEye's automatic shotgun throws no shell: autoshot_stats has no
	// ejected cartridge (NULL where the Shotgun's row names cartshell), and
	// on the cartridge no casing lands after its shots. The host's shell
	// did, with CART_SPENT (FINDINGS row 11)
	if (weaponnum == WEAPON_GE_AUTOSHOTGUN) {
		const struct weapon *host = g_Weapons[g_GeWeaponHosts[i]];

		if (def->ammos[0] && def->ammos[0] != host->ammos[0]) {
			def->ammos[0]->casingeject = (u32)CASING_NONE;
		}
	}
}

/**
 * GoldenEye's grenade and mines, which are thrown whole, and the watch's
 * detonator that sets the remote mines off. A copy of a host's brought along
 * Perfect Dark's remote mine as it is, which GoldenEye's is not:
 *
 * - **The detonator in the left hand.** Perfect Dark holds a remote mine in
 *   the right and its detonator in the left (WEAPONFLAG2_DETONATORHAND), so
 *   the left hand was in use with the mine's own number: GoldenEye's HUD drew
 *   the mine's icon at both bottom corners and the mine was held "as though
 *   it was wielded akimbo" (F3 20260925-230009). GoldenEye holds one mine in
 *   one hand, and draws nothing of it (gegunsOwnModelHidden()).
 * - **The second function.** Perfect Dark detonates with the remote mine's
 *   second function, chosen as any second function is and saved per host, so
 *   a player who left Perfect Dark's remote mine on it drew GoldenEye's and
 *   could not throw one. GoldenEye's remote mine does one thing. Its mines
 *   go off two ways, both kept: A and B pressed together while the remote
 *   mine is in the hand (bondview2.c's moveData.detonating, which Perfect
 *   Dark kept whole in bondmove.c), and the watch's detonator, ITEM_TRIGGER -
 *   an item of its own, given with the remote mines (propobj.c), after them
 *   in the cycle, drawn to when the last one is thrown (gun.c's
 *   autoadvance_on_deplete_all_ammo()), shown as Bond's two hands at the
 *   watch, and pulling its trigger sets them off (chrprop.c's
 *   chraiCheckUseHeldItem()). That is WEAPON_GE_DETONATOR, on the Data
 *   Uplink, whose one function becomes Perfect Dark's own detonate.
 * - **Moving on when they run out.** GoldenEye goes to the next thing in
 *   the cycle once the last mine is thrown, the remote mine to its detonator
 *   (bgunAutoSwitchWeapon()); Perfect Dark's own mines stay in the hand empty.
 * - **Not a pair of GoldenEye's own.** GoldenEye pairs none of these (no
 *   CAN_DUAL_WIELD), and none of their hosts has WEAPONFLAG_DUALWIELD; it is
 *   cleared here all the same, so that a rule reading the definition's own
 *   flag never pairs them. Akimbo is the house rule that does (weaponHasFlag()
 *   answers yes for any weapon modCanAkimbo() lets into a hand): two grenades
 *   or two mines under it, as the user asked - but never two detonators
 *   (gegunsNeverPairs()).
 *
 * The same after a borrow (gegunsBorrow()), whose flags2 are the host's
 * again - Perfect Dark's remote mine's, detonator hand and all.
 */
static void gegunsOwnThrown(s32 i)
{
	const s32 weaponnum = WEAPON_GE_FIRST + i;
	struct weapon *def = &g_GeWeaponDefs[i];

	switch (weaponnum) {
	case WEAPON_GE_GRENADE:
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
	case WEAPON_GE_REMOTEMINE:
		// thrown whole, so the last one gone the hand moves on
		// (bgunAutoSwitchWeapon()); gegunsOwnTrigger() took it off them
		// with the hunting knife's
		def->flags |= WEAPONFLAG_THROWABLE;
		// fall through
	case WEAPON_GE_DETONATOR:
		def->flags &= ~WEAPONFLAG_DUALWIELD;
		def->flags2 &= ~WEAPONFLAG2_DETONATORHAND;
		break;
	default:
		return;
	}

	if (weaponnum == WEAPON_GE_REMOTEMINE) {
		def->functions[1] = NULL;
	}

	// Thrown straight along the aim, 16.7 a tick and 5 up (gun.c's
	// generate_player_thrown_grenade() and _object()), not on the host's arc
	// to the point under the crosshair at 21.7, which brought a mine down 12
	// ticks after the throw where the cartridge's takes 22 (FINDINGS row 10).
	// The 5 up is bondgun.c's (gegunsThrowsAsGoldenEye())
	if (weaponnum != WEAPON_GE_DETONATOR) {
		for (s32 f = 0; f < 2; f++) {
			struct weaponfunc *func = def->functions[f];

			if (func && (func->type & 0xff) == INVENTORYFUNCTYPE_THROW) {
				func->flags &= ~FUNCFLAG_CALCULATETRAJECTORY;
			}
		}
	}

	if (weaponnum == WEAPON_GE_DETONATOR) {
		// Perfect Dark's own detonate (HANDATTACKTYPE_DETONATE, which
		// playerActivateRemoteMineDetonator() answers), as the one function:
		// the host's Data Uplink would uplink whatever was in front of it
		const struct weaponfunc *detonate = g_Weapons[WEAPON_REMOTEMINE]->functions[1];
		struct weaponfunc *copy = detonate ? malloc(gegunsFuncSize(detonate->type)) : NULL;

		if (copy) {
			memcpy(copy, detonate, gegunsFuncSize(detonate->type));
			def->functions[0] = copy;
		}

		def->functions[1] = NULL;
		def->ammos[0] = NULL;
		def->ammos[1] = NULL;
		def->flags &= ~(WEAPONFLAG_FIRETOACTIVATE | WEAPONFLAG_THROWABLE);
		def->flags |= WEAPONFLAG_ONEHANDED | WEAPONFLAG_UNDROPPABLE;
	}
}

/**
 * Whether one of GoldenEye's weapons is never held as a pair, however it is
 * picked up and whatever Akimbo says: the watch's detonator, which is Bond's
 * two hands at the watch and one trigger for every mine (gegunsOwnThrown()).
 * Its definition has no WEAPONFLAG_DUALWIELD either; this is the answer for
 * a rule that pairs guns without asking that flag (modCanAkimbo()). The
 * grenade and the mines are not on it: Akimbo pairs them (user's call,
 * 2026-09-26), GoldenEye's own rule never does.
 */
static s32 gegunsWatchLaserInstalled(void);

s32 gegunsNeverPairs(s32 weaponnum)
{
	// nor the watch laser on the Moonraker's number (Train), which is the
	// same two hands at the watch: a pair drew the watch in one hand and the
	// Moonraker in the other (F3 20260929-220750, "with watch laser no akimbo
	// allowed, looks too funny")
	return weaponnum == WEAPON_GE_DETONATOR
		|| (weaponnum == WEAPON_GE_MOONRAKER && gegunsWatchLaserInstalled());
}

/**
 * Whether GoldenEye's All Guns cheat holds this gun as a pair too: its row's
 * CAN_DUAL_WIELD, which the cheat alone reads (bondinvItemAvailableForHand(),
 * bondinvChooseCycleForwardWeapon()); never the watch laser nor the detonator.
 */
s32 gegunsAllGunsPairs(s32 weaponnum)
{
	const s32 i = weaponnum - WEAPON_GE_FIRST;

	if (!GE_GUN_INDEX(i) || gegunsNeverPairs(weaponnum)) {
		return 0;
	}

	return (stats[i].bitflags & GESTATFLAG_CAN_DUAL_WIELD) != 0;
}

/**
 * GoldenEye's guns as another installed mod made them (modborrow.c): GoldenEye
 * X's definition as the model gegunsBuild() draws on - its model, hands,
 * positions, fire and reload scripts with their animations and sounds already
 * moved to numbers of the port's own - under GoldenEye's name here, with the
 * port's own fields (flags2, flags3, the unequipped reload index, the pickup
 * sound) the host's, since the code keyed on them asks about the host
 * (weaponHost()), and GoldenEye's numbers and ammunition type as the stock
 * copy has them: a type is a row of the game's ammo table, the one the
 * crates, the HUD and the Combat Simulator's lists count, and GoldenEye X's
 * rows are its own table's.
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

	// Drawn on GoldenEye X's model, with its scripts, and run by the host's
	// engine as the stock copy is. What the gun does is still GoldenEye's own,
	// out of the ROM's rows: GoldenEye X's numbers gave the silenced PP7 no
	// noise at all - a shot added nothing to its radius, so nobody on Dam
	// could hear Bond fire.
	gegunsBuild(index, def, g_Weapons[g_GeWeaponHosts[index]]);
	gegunsOwnThrown(index);
	gegunsFireRate(index);

	// Text ids are the mod's language files', which say something else here
	// (its KF7's function read "Burst Fire"): the port's own names stay
	for (s32 f = 0; f < 2; f++) {
		struct weaponfunc *fn = out->functions[f];
		const struct weaponfunc *ours = fn ? gegunsNameFor(index, f, fn->type) : NULL;

		if (fn && ours) {
			fn->name = ours->name;
		}
	}

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
static const u8 geItems[NUM_GE_WEAPONS] = {
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

static const u8 *items = geItems;

/*
 * gunRenderFirstPersonGunModels() (gunfire.c) draws six hand items under its
 * weapon envmap light and the camera's LookAt - the golden gun (19), the
 * magnum (18), the knife (2), the throwing knife (3) and the silver and gold
 * PP7s (20, 21) - and every other gun under what the level loaded last,
 * bgLevelRender()'s GlobalLight and the same LookAt. A bit an item; a hack's
 * own six come with its guns (geguns.bin, envmapitems in geconvert.c).
 */
#define GEGUNS_ENVMAP_ITEMS ((1u << 19) | (1u << 18) | (1u << 2) | (1u << 3) | (1u << 20) | (1u << 21))

static u32 envitems = GEGUNS_ENVMAP_ITEMS;

s32 gegunsItemNumber(s32 index)
{
	return index >= 0 && index < NUM_GE_WEAPONS ? items[index] : 0;
}

s32 gegunsItemWeapon(s32 item)
{
	if (item <= 0) {
		return WEAPON_NONE;
	}

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		if (gegunsItemNumber(i) == item) {
			return WEAPON_GE_FIRST + i;
		}
	}

	return WEAPON_NONE;
}

/**
 * The lights and LookAt one of GoldenEye's guns is drawn under in first
 * person, as gunRenderFirstPersonGunModels() gives them: the weapon envmap
 * light for an item on its list (envitems), the level's GlobalLight (bg.c,
 * what every other gun inherits from bgLevelRender()) otherwise, and the
 * camera's LookAt either way (camGetLookAt() is GoldenEye's own
 * guLookAtReflect()). Both in world terms against a modelview that ends at
 * the camera, as on the cartridge (gun.c builds a model's matrices on
 * camGetWorldToScreenMtxf()), so a sphere-mapped gun's highlight sweeps as
 * Bond turns. Perfect Dark set these only for its own WEAPONFLAG_00008000
 * guns; a GoldenEye gun took whatever the world drew last.
 */
Gfx *gegunsLightsAndLookAt(Gfx *gdl, s32 weaponnum)
{
	const s32 item = gegunsItemNumber(weaponnum - WEAPON_GE_FIRST);
	Lights1 *lights = gfxAllocate(sizeof(Lights1));

	// g_WeaponEnvmapLight (gun.c) or GlobalLight (bg.c): grey ambient and a
	// white light, the two alike but for the light's direction
	if (item > 0 && item < 32 && (envitems & (1u << item))) {
		*lights = (Lights1)gdSPDefLights1(0x96, 0x96, 0x96, 0xff, 0xff, 0xff, 0xb2, 0x4d, 0x2e);
	} else {
		*lights = (Lights1)gdSPDefLights1(150, 150, 150, 255, 255, 255, 77, 77, 46);
	}

	gSPSetLights1(gdl++, (*lights));
	gSPLookAt(gdl++, camGetLookAt());

	return gdl;
}

/** Weapon `index`'s own name (its text id), the gun set's. */
u16 gegunsNameId(s32 index)
{
	return index >= 0 && index < NUM_GE_WEAPONS ? nameids[index] : 0;
}

/** A gun's GoldenEye AmmoType, the gun set's (0 none), or -1 for a weapon with no row. */
s32 gegunsGeAmmoType(s32 weaponnum)
{
	const s32 i = weaponnum - WEAPON_GE_FIRST;

	return GE_GUN_INDEX(i) && stats[i].bitflags ? stats[i].ammotype : -1;
}

f32 gegunsItemDamage(s32 item)
{
	s32 weaponnum = gegunsItemWeapon(item);

	return weaponnum == WEAPON_NONE ? 0.0f : stats[weaponnum - WEAPON_GE_FIRST].damage;
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
 *
 * Kept only until the file slots are emptied. Choosing a mod, or turning a
 * mod's maps on or off, runs romdataResetFiles() and registers every mod file
 * again from the first free slot, so a number kept from before names whatever
 * came to sit in that slot after: a player who changed mods and then drew a
 * GoldenEye gun in the N64 look loaded GoldenEye Egyptian's room segment
 * (bg_gxcryp.seg, file 2164) as the gun's model, which was not a rare zip,
 * and the uninflated bytes crashed the model loader (crash 20260924-151337).
 */
static u16 convertedModel[NUM_GE_WEAPONS];
static s32 convertedSearched = -1;
static s32 convertedGeneration = -1;
static s32 convertedSet = -2;

// the mod dir whose gun set is in (gegunsStageSet()), -1 GoldenEye's own
static s32 g_GunSetDir = -1;

static void gegunsFindConverted(void)
{
	const s32 numdirs = fsGetNumModDirs();
	const s32 generation = romdataFilesGeneration();
	s32 found = 0;
	s32 dir = -1;

	if (convertedSearched == numdirs && convertedGeneration == generation && convertedSet == g_GunSetDir) {
		return;
	}

	convertedSearched = numdirs;
	convertedGeneration = generation;
	convertedSet = g_GunSetDir;

	// the mount's index may have moved too: searched again from nothing
	memset(convertedModel, 0, sizeof(convertedModel));

	// the PP7 is the conversion's marker: every GoldenEye gun is written with
	// it. GoldenEye's own conversion's, never a ROM hack's (Goldfinger 64's
	// guns are models of its own), wherever the two are mounted - but the
	// hack's own on a stage of its own, where its gun set is in
	for (s32 i = 0; i < numdirs && dir < 0; i++) {
		char path[FS_MAXPATH + 1];
		const char *at = fsGetModDirAt(i);

		if (!at || (g_GunSetDir >= 0 ? i != g_GunSetDir
				: (modloaderGexPlusDirIndex() >= 0 && !modloaderDirIndexIsGexPlus(i)))) {
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

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		const s32 item = GE_GUN_INDEX(i) ? gegunsItemNumber(i) : 0;
		char name[16];
		char path[FS_MAXPATH + 1];

		if (item <= 0) {
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
		sysLogPrintf(LOG_NOTE, "geguns: %d %s first-person guns, converted from the ROM", found,
				g_GunSetDir >= 0 ? "of a ROM hack's own" : "of GoldenEye's own");
	}
}

/** Whether the conversion has GoldenEye's own first-person model for this gun. */
s32 gegunsHasOwnModel(s32 index)
{
	gegunsFindConverted();

	return GE_GUN_INDEX(index) && convertedModel[index] != 0;
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
static const f32 geOwnPos[NUM_GE_WEAPONS][3] = {
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

static const f32 (*ownpos)[3] = geOwnPos;

/**
 * Whether gun `index` stands on one of Perfect Dark's classic pistols, the
 * PP9i and the CC13, which are GoldenEye's own PPK and TT33 models (FILE_GWPPK,
 * FILE_GTT33) in GoldenEye's own frame, with Perfect Dark's hands and
 * Perfect Dark's placement - and is not borrowed from GoldenEye X. On those
 * GoldenEye's placement (ownpos) is GoldenEye's gun where GoldenEye holds it,
 * whichever look draws it.
 *
 * Not the classic submachine guns and rifles: measured on screen against
 * GoldenEye's own models at the same placement, the KL01313 stood 12 units
 * lower and fell off the bottom of the view, and the DMC and RC-P45 went
 * further from GoldenEye's than their own placement is, so those models do
 * not stand in GoldenEye's frame and keep Perfect Dark's placement.
 */
static s32 gegunsHostIsOwnModel(s32 index)
{
	if (borrowed[index]) {
		return 0;
	}

	switch (g_GeWeaponHosts[index]) {
	case WEAPON_PP9I:
	case WEAPON_CC13:
		return 1;
	}

	return 0;
}

// The host's own placement and part commands, to go back to when the gun is
// drawn on the host's model again (the other look, F6)
static s32 ownInUse[NUM_GE_WEAPONS];
static s32 hostSaved[NUM_GE_WEAPONS];
static f32 hostPos[NUM_GE_WEAPONS][3];
static struct gunviscmd *hostVis[NUM_GE_WEAPONS];
static struct guncmd *hostEquip[NUM_GE_WEAPONS];
static struct guncmd *hostUnequip[NUM_GE_WEAPONS];
static s32 animsTaken[NUM_GE_WEAPONS];


/**
 * Draw gun `index` in first person on GoldenEye's own model (1) or on
 * whatever gebean.c put in hi_model otherwise (0): GoldenEye's placement and
 * no host part commands for the one, the host's for the other.
 */
void gegunsSetOwnModelInUse(s32 index, s32 inuse)
{
	struct weapon *def;

	if (!GE_GUN_INDEX(index)) {
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

	// A host's draw and put-away animate the host's joints, which on
	// GoldenEye's model are other parts or none: the Moonraker on the Laser's
	// stood still through the Laser's put-away and popped up through its
	// draw ("Moonraker Laser lacks a put away and pull out animation", F3
	// 20261002-174244). GoldenEye lowers and raises every gun whole
	// (gunfire.c's SWITCH_LOWER and SWITCH_RAISE), which is what Perfect
	// Dark does for a gun with no such animation. A classic gun's
	// equip_animation is its held pose (WEAPONFLAG_00004000), and a thrown
	// one is drawn at once (gegunsSwitchAtOnce()); both keep theirs.
	if (inuse && !(def->flags & (WEAPONFLAG_00004000 | WEAPONFLAG_THROWABLE))) {
		if (def->equip_animation || def->unequip_animation) {
			hostEquip[index] = def->equip_animation;
			hostUnequip[index] = def->unequip_animation;
			animsTaken[index] = 1;
		}

		def->equip_animation = NULL;
		def->unequip_animation = NULL;
	} else if (animsTaken[index]) {
		// the host's model again (the other look, or a rebuilt definition
		// left as it was built)
		if (!def->equip_animation && !def->unequip_animation) {
			def->equip_animation = hostEquip[index];
			def->unequip_animation = hostUnequip[index];
		}

		animsTaken[index] = 0;
	}

	if (inuse) {
		def->posx = ownpos[index][0];
		def->posy = ownpos[index][1];
		def->posz = ownpos[index][2];
		// The host's part commands name the host's parts, which on
		// GoldenEye's model are other things entirely, so it has none: NULL,
		// and not a list holding only its end - bgunExecuteGunVisCommands()
		// runs its first entry before it looks for the end, and the end read
		// as a command is "show part 0". Part 0 of GoldenEye's model is a
		// POSITIONHELD node, whose data read as a toggle's gave an rwdata
		// index past the hand's 32 words, and the command after the list was
		// whatever the linker put there: on Frigate, which starts Bond with
		// the silenced D5K, that wrote a 1 into the frame's display list and
		// the mission died at its first frame of play ("Unknown GBI opcode
		// 0x103")
		def->gunviscmds = NULL;
	} else if (gegunsHostIsOwnModel(index)) {
		// The host is GoldenEye's own pistol as Perfect Dark converted it
		// for its classic guns, and the release's gun is laid onto it, so it
		// is held where GoldenEye holds it. Perfect Dark held its classic PP9i
		// 6 units higher and 14.5 nearer the eye than GoldenEye holds the PP7
		// (10, -14.8, -19 against 11, -20.8, -33.5): the HD look's pistol sat
		// high and large beside the release's and our N64 look's (F3
		// 20260929-042658, "compare hand position to ge xbla release
		// position, some say too far up")
		def->posx = ownpos[index][0];
		def->posy = ownpos[index][1];
		def->posz = ownpos[index][2];
		def->gunviscmds = hostVis[index];
	} else {
		def->posx = hostPos[index][0];
		def->posy = hostPos[index][1];
		def->posz = hostPos[index][2];
		def->gunviscmds = hostVis[index];
	}
}

/**
 * Where GoldenEye holds gun `index` in front of the eye (ownpos) and where its
 * host's model is held when the release's gun is drawn on that instead, in
 * the camera's space; 0 when there is no such gun.
 */
s32 gegunsViewPlacement(s32 index, f32 *own, f32 *host)
{
	const struct weapon *def;

	if (!GE_GUN_INDEX(index)) {
		return 0;
	}

	def = &g_GeWeaponDefs[index];

	for (s32 a = 0; a < 3; a++) {
		own[a] = ownpos[index][a];
	}

	host[0] = hostSaved[index] ? hostPos[index][0] : def->posx;
	host[1] = hostSaved[index] ? hostPos[index][1] : def->posy;
	host[2] = hostSaved[index] ? hostPos[index][2] : def->posz;

	return 1;
}

/** Whether this weapon is drawn in first person on GoldenEye's own model. */
s32 gegunsOwnModelInUse(s32 weaponnum)
{
	const s32 index = weaponnum - WEAPON_GE_FIRST;

	// unarmed on a converted level is GoldenEye's own hand (geslappers.c)
	if (weaponnum == WEAPON_UNARMED) {
		return geslappersActive();
	}

	return GE_GUN_INDEX(index) && ownInUse[index];
}

/**
 * Each gun's model in a hand in GoldenEye, its PROP_CHR* prop: player.c's
 * getPropForHeldItem() by weapon, with the thrown ones' props besides. The
 * conversion's `models` block numbers them MODEL_REMAKE_FIRST + prop and its
 * setups give them to the guards and lay them on the floor.
 */
static const s16 geChrProps[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7             - WEAPON_GE_FIRST] = 191, // PROP_CHRWPPK
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = 204, // PROP_CHRWPPKSIL
	[WEAPON_GE_DD44            - WEAPON_GE_FIRST] = 205, // PROP_CHRTT33
	[WEAPON_GE_KLOBB           - WEAPON_GE_FIRST] = 193, // PROP_CHRSKORPION
	[WEAPON_GE_KF7SOVIET       - WEAPON_GE_FIRST] = 184, // PROP_CHRKALASH
	[WEAPON_GE_ZMG             - WEAPON_GE_FIRST] = 195, // PROP_CHRUZI
	[WEAPON_GE_D5K             - WEAPON_GE_FIRST] = 189, // PROP_CHRMP5K
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = 206, // PROP_CHRMP5KSIL
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = 194, // PROP_CHRSPECTRE
	[WEAPON_GE_AR33            - WEAPON_GE_FIRST] = 188, // PROP_CHRM16
	[WEAPON_GE_RCP90           - WEAPON_GE_FIRST] = 197, // PROP_CHRFNP90
	[WEAPON_GE_SHOTGUN         - WEAPON_GE_FIRST] = 192, // PROP_CHRSHOTGUN
	[WEAPON_GE_AUTOSHOTGUN     - WEAPON_GE_FIRST] = 207, // PROP_CHRAUTOSHOT
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = 210, // PROP_CHRSNIPERRIFLE
	[WEAPON_GE_COUGARMAGNUM    - WEAPON_GE_FIRST] = 190, // PROP_CHRRUGER
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = 208, // PROP_CHRGOLDEN
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = 187, // PROP_CHRLASER
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = 185, // PROP_CHRGRENADELAUNCH
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = 211, // PROP_CHRROCKETLAUNCH
	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = 186, // PROP_CHRKNIFE
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = 209, // PROP_CHRTHROWKNIFE
	[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = 196, // PROP_CHRGRENADE
	[WEAPON_GE_TIMEDMINE       - WEAPON_GE_FIRST] = 201, // PROP_CHRTIMEDMINE
	[WEAPON_GE_PROXIMITYMINE   - WEAPON_GE_FIRST] = 200, // PROP_CHRPROXIMITYMINE
	[WEAPON_GE_REMOTEMINE      - WEAPON_GE_FIRST] = 199, // PROP_CHRREMOTEMINE
};

static const s16 *chrProps = geChrProps;

/** Gun `index`'s PROP_CHR* number, or -1. */
s32 gegunsChrProp(s32 index)
{
	return GE_GUN_INDEX(index) && chrProps[index] > 0 ? chrProps[index] : -1;
}

/**
 * GoldenEye's own model of this gun in a hand - its PROP_CHR* prop, which the
 * conversion's `models` block numbers MODEL_REMAKE_FIRST + prop - wherever the
 * gun is drawn in GoldenEye's own look and the stage has the block loaded; -1
 * otherwise. On a stage of Perfect Dark's the prop is lent for the stage the
 * first time it is asked for, and read in when the first one is made.
 *
 * The model state a GoldenEye gun otherwise has (MODEL_GE_FIRST) is an alias
 * of its host's pickup that only the release's HD pickup is drawn over, so in
 * the N64 look every gun that shares a host drew the same host model: the
 * silenced PP7 Bond starts Dam with was the plain PP7 in his hand through the
 * opening swirl (the silenced D5K the same). The guards on the same level have
 * held GoldenEye's own props all along, from the converted setup.
 *
 * The thrown ones are left out: GoldenEye draws nothing of them in the hand,
 * and what they are thrown as is gegunsThrownModel()'s.
 */
s32 gegunsOwnPropModel(s32 weaponnum)
{
	const s32 index = weaponnum - WEAPON_GE_FIRST;
	s32 prop;

	if (!gegunsOwnModelInUse(weaponnum) || !GE_GUN_INDEX(index)) {
		return -1;
	}

	switch (weaponnum) {
	case WEAPON_GE_THROWINGKNIFE:
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
	case WEAPON_GE_REMOTEMINE:
		return -1;
	}

	prop = gegunsChrProp(index);

	// lent on a stage of Perfect Dark's (modloaderLendRemakeModel()), where
	// a sim or a guard held the host's gun and the floor had it
	return prop > 0 ? modloaderLendRemakeModel(prop) : -1;
}

/**
 * What one of GoldenEye's guns lies on the floor as, from a Combat Simulator
 * or random weapon row: its own held prop where that is drawn
 * (gegunsOwnPropModel()), as its setups lay it and as it is dropped, and the
 * row's `fallback` (the host's pickup, MODEL_GE_FIRST) otherwise.
 */
s32 gegunsFloorModel(s32 weaponnum, s32 fallback)
{
	const s32 model = gegunsOwnPropModel(weaponnum);

	return model >= 0 ? model : fallback;
}

/**
 * The rocket a GoldenEye launcher holds and fires, where its own model is in
 * the hand: GoldenEye's PROP_CHRROCKET (currentPlayerCreateRocket()), whose
 * origin is its tail at the barrel's end. The host's Perfect Dark rocket has
 * its origin further along its length, so it sat inside GoldenEye's tube with
 * nothing showing at the mouth. `fallback` otherwise.
 *
 * Only a converted level's `models` block has the rocket, so on a stage of
 * Perfect Dark's it is lent for the stage the first time it is asked for
 * (modloaderLendRemakeModel()): the Combat Simulator's launcher held and fired
 * Perfect Dark's rocket, which again showed nothing at the mouth (F3
 * 20260919-174510, Pipes).
 */
s32 gegunsOwnRocketModel(s32 weaponnum, s32 fallback)
{
	s32 model;

	if (weaponnum != WEAPON_GE_ROCKETLAUNCHER || !gegunsOwnModelInUse(weaponnum)) {
		return fallback;
	}

	model = modloaderLendRemakeModel(202); // PROP_CHRROCKET

	return model >= 0 ? model : fallback;
}

/**
 * What the player throws of GoldenEye's grenade and mines: GoldenEye's own
 * prop, as gun.c's throw makes it (PROP_CHRGRENADE, PROP_CHRREMOTEMINE,
 * PROP_CHRPROXIMITYMINE, PROP_CHRTIMEDMINE), where the stage has it - a
 * converted level's `models` block, in either look, as the thrown gadgets
 * (gegadgetsPropModel()) - or where GoldenEye's own model is in the hand and
 * it can be lent (a stage of Perfect Dark's); the host's throw function's
 * Perfect Dark projectile, `fallback`, otherwise. A remote mine thrown onto
 * one of Facility's tanks was Perfect Dark's blue mine (F3 20260926-171321).
 * The host's model still says whether it sticks (bgunCreateThrownProjectile2()).
 *
 * The throwing knife is thrown as the hunting knife's PROP_CHRKNIFE, as
 * gun.c's generate_player_thrown_knife() makes it. Its launch turn and spin
 * are the host's and GoldenEye's alike (a quarter turn about z and a half
 * about x on the hand's matrix, 360/(12.1..13.1) degrees a sixtieth about the
 * knife's y): measured on the native port, the knife leaves in the camera's
 * space with x (0, .52, .86), y (-1, 0, 0) where ours had (0, .47, .88) and
 * (-1, 0, 0), so GoldenEye's model under the same matrices flies as
 * GoldenEye's does.
 */
s32 gegunsThrownModel(s32 weaponnum, s32 fallback)
{
	s32 prop;
	s32 model;

	switch (weaponnum) {
	case WEAPON_GE_THROWINGKNIFE: prop = 186; break; // PROP_CHRKNIFE
	case WEAPON_GE_GRENADE:       prop = 196; break; // PROP_CHRGRENADE
	case WEAPON_GE_REMOTEMINE:    prop = 199; break; // PROP_CHRREMOTEMINE
	case WEAPON_GE_PROXIMITYMINE: prop = 200; break; // PROP_CHRPROXIMITYMINE
	case WEAPON_GE_TIMEDMINE:     prop = 201; break; // PROP_CHRTIMEDMINE
	default: return fallback;
	}

	if (g_ModelStates[MODEL_REMAKE_FIRST + prop].fileid) {
		return MODEL_REMAKE_FIRST + prop;
	}

	if (!gegunsOwnModelInUse(weaponnum)) {
		return fallback;
	}

	model = modloaderLendRemakeModel(prop);

	return model >= 0 ? model : fallback;
}

/**
 * GoldenEye's own weapons are drawn with every face. Their lists cull their
 * back faces, and not every part of every gun is wound the same way round:
 * the silenced D5K's silencer is wound backwards from the rest of it, so on
 * the watch, turned towards the camera, it was the inside of its far half
 * with its near half gone (F3 20260926-202346, "d5k silenced barrel is
 * getting culled ... maybe disable backface culling for weapons only").
 * Drawn with the depth buffer, a closed shape looks the same either way, and
 * a face wound backwards is there.
 */
s32 gegunsObjDrawsBothSides(struct defaultobj *obj)
{
	return obj && obj->type == OBJTYPE_WEAPON && obj->modelnum >= MODEL_REMAKE_FIRST && obj->modelnum < NUM_MODELS;
}

/**
 * The throwing knife's turn as it leaves the hand where the hand is not
 * GoldenEye's own (the HD look): the knife is turned from the hand's matrix,
 * which there is the host's combat knife under its own throw animation - the
 * release's knife left level, blade across the view, a sixth of a turn off
 * GoldenEye's. GoldenEye's release pose is always the end of its draw back
 * (throwKnifeDrawBackKeyframes), so its launch turn in the camera's space is
 * one matrix, measured on the native port (x 0, .52, .86; y -1, 0, 0), and
 * it is put in the world by the camera. The N64 look keeps the hand's own,
 * which is GoldenEye's pose on GoldenEye's model (within 3 degrees).
 */
void gegunsThrowKnifeLaunch(s32 weaponnum, Mtxf *mtx, Mtxf *camtoworld)
{
	static const f32 cam[3][3] = {
		{ 0.0f, 0.51757f, 0.85563f },
		{ -1.0f, 0.0f, 0.0f },
		{ 0.0f, -0.85563f, 0.51757f },
	};

	if (weaponnum != WEAPON_GE_THROWINGKNIFE || gegunsOwnModelInUse(weaponnum) || !camtoworld) {
		return;
	}

	for (s32 r = 0; r < 3; r++) {
		f32 len = 0.0f;

		for (s32 c = 0; c < 3; c++) {
			mtx->m[r][c] = cam[r][0] * camtoworld->m[0][c] + cam[r][1] * camtoworld->m[1][c]
				+ cam[r][2] * camtoworld->m[2][c];
			len += mtx->m[r][c] * mtx->m[r][c];
		}

		len = sqrtf(len);

		for (s32 c = 0; len > 0.0f && c < 3; c++) {
			mtx->m[r][c] /= len;
		}
	}
}

/**
 * How fast a throw leaves the hand along the aim, a sixtieth: GoldenEye's
 * throwing knife's 25 (gun.c's generate_player_thrown_knife()), `speed`
 * otherwise.
 */
f32 gegunsThrowSpeed(s32 weaponnum, f32 speed)
{
	return weaponnum == WEAPON_GE_THROWINGKNIFE ? 25.0f : speed;
}

/**
 * Whether a reload goes straight to taking the next one up, the hand already
 * out of view: GoldenEye's throwing knife, whose one-knife clip is only ever
 * reloaded after a throw, and GoldenEye goes from the follow-through straight
 * to RELOAD_SWAP (measured on the native port: 17 sixtieths with nothing in
 * the hand, then 24 raising the knife). Perfect Dark lowered the empty hand
 * for 15 first.
 */
s32 gegunsReloadSkipsLower(s32 weaponnum)
{
	return weaponnum == WEAPON_GE_THROWINGKNIFE;
}

/**
 * A guard's rocket and grenade round, where GoldenEye's own models are drawn:
 * its chraction.c fires PROP_CHRROCKET (202) and PROP_CHRGRENADEROUND (203),
 * where the host's function names Perfect Dark's.
 */
s32 gegunsChrProjectileModel(s32 weaponnum, s32 fallback)
{
	s32 model;

	if (!gegunsOwnModelInUse(weaponnum)) {
		return fallback;
	}

	// lent on a stage of Perfect Dark's, as the player's (gegunsOwnRocketModel())
	switch (weaponnum) {
	case WEAPON_GE_ROCKETLAUNCHER:  model = modloaderLendRemakeModel(202); break;
	case WEAPON_GE_GRENADELAUNCHER: model = modloaderLendRemakeModel(203); break;
	default: return fallback;
	}

	return model >= 0 ? model : fallback;
}

/**
 * The Enemy Rockets cheat on one of GoldenEye's weapons, as GoldenEye has it
 * (chrai.c's TRYGiveMeItem and prop.c's setup of a guard's gun): every gun,
 * both knives and the remote mine become its rocket launcher; the grenade
 * launcher, the grenade and the timed and proximity mines, and the gadgets,
 * stay. The hosts' list had it the other way round for most of them.
 */
s32 gegunsEnemyRocketsSwaps(s32 weaponnum)
{
	switch (weaponnum) {
	case WEAPON_GE_GRENADELAUNCHER:
	case WEAPON_GE_GRENADE:
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
		return 0;
	}

	return GE_GUN_INDEX(weaponnum - WEAPON_GE_FIRST);
}

s32 gegunsEnemyRocketModel(void)
{
	const s32 model = playermgrGetModelOfWeapon(WEAPON_GE_ROCKETLAUNCHER);

	return model >= 0 ? model : MODEL_CHRDYROCKET;
}

/**
 * Whether GoldenEye takes this weapon up and puts it away with no movement at
 * all: what it draws nothing of in the hand (gunfire.c's raise states go
 * straight to idle for WEAPONSTATBITFLAG_HIDE_FIRST_PERSON_HAND or no model).
 * The grenade and the mines on GoldenEye's own models (gegunsOwnModelHidden()),
 * and every gadget but the watch's detonator, which is drawn
 * (bondgun.c's bgunTickIncChangeGun()).
 */
s32 gegunsSwitchAtOnce(s32 weaponnum)
{
	if (weaponnum >= WEAPON_GE_COVERTMODEM && weaponnum <= WEAPON_GE_TANKSHELLS) {
		return 1;
	}

	return gegunsOwnModelHidden(weaponnum);
}

/**
 * GoldenEye draws its gadgets in silence (gunfire.c's equip sound leaves out
 * the covert modem, the plastique, the GoldenEye key, the camera, the watch
 * magnet, the tank's shells and the watch's detonator); their hosts, the ECM
 * mine and the Data Uplink, play the mine's.
 */
s32 gegunsEquipSilent(s32 weaponnum)
{
	// and the watch laser, which is no gun drawn (ITEM_WATCHLASER is in the
	// same silent case): it played the Moonraker's PICKUP_LASER_SFX, its
	// host's (F3 20260930-191858, "makes moonraker pullout sound")
	return (weaponnum >= WEAPON_GE_COVERTMODEM && weaponnum <= WEAPON_GE_DETONATOR)
		|| (weaponnum == WEAPON_GE_MOONRAKER && gegunsWatchLaserInstalled());
}

/**
 * GoldenEye's mines take five seconds from the throw to arm - or, the timed
 * one, to go off - and three in a game of more than one player (gun.c's
 * THROWN_ITEM_TIMER_SOLO and THROWN_ITEM_TIMER_MULTI, NTSC's 300 and 180 on a
 * clock of sixtieths). Perfect Dark's are four in either (activatetime60 240).
 */
s32 gegunsThrownFuse60(s32 weaponnum)
{
	switch (weaponnum) {
	case WEAPON_GE_TIMEDMINE:
	case WEAPON_GE_PROXIMITYMINE:
	case WEAPON_GE_REMOTEMINE:
		return PLAYERCOUNT() == 1 ? 300 : 180;
	}

	return 0;
}

/**
 * The fuse on a grenade a guard throws, in sixtieths, or 0 for Perfect Dark's.
 *
 * Perfect Dark's guards let go with timer240 at TICKS(240): one second, on the
 * clock of 240ths. GoldenEye's (chraction.c's chrlvTickThrowGrenade) set the
 * grenade's timer to CHRLV_DEFAULT_TIMER - NTSC's 180 sixtieths, PAL's 150
 * fiftieths, three seconds either way - every tick from frame 61 of the throw
 * until it leaves the hand at 119, and propobj.c counts it down on its
 * sixtieths. A converted mission's guards threw Perfect Dark's one second
 * (F3 20260930-211747, Control). GoldenEye's grenade from any guard, and any
 * grenade on a converted mission.
 */
s32 gegunsChrGrenadeFuse60(s32 weaponnum)
{
	if (weaponnum == WEAPON_GE_GRENADE
			|| (modloaderStageIsMission(g_Vars.stagenum) && !g_Vars.normmplayerisrunning)) {
		return 180;
	}

	return 0;
}

static s32 gegunsOwnThrowKnifeGone(const struct hand *hand);

static s32 gegunsIsShotgun(s32 weaponnum)
{
	return weaponnum == WEAPON_GE_SHOTGUN || weaponnum == WEAPON_GE_AUTOSHOTGUN;
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
/**
 * The release's flash on a gun GoldenEye shows none for (the silenced PP7 and
 * D5K, FUNCFLAG_NOMUZZLEFLASH): its file has flash cards like every other gun
 * (gebean.c's fpCardSet), and the release lights them for the shot - eight
 * shots of the silenced PP7 on Dam, each with its flash for a frame. The HD
 * look lights the cards alone for that tick, without hand->flashon and so
 * without the flash's light on the room or the firing flash of the player's
 * own body (bondgun.c sets it, bgunTickInc() clears it).
 */
static u8 gegunsCards[MAX_PLAYERS][2];

void gegunsCardsLit(s32 handnum, s32 on)
{
	const s32 p = g_Vars.currentplayernum;

	if (p >= 0 && p < MAX_PLAYERS && handnum >= 0 && handnum < 2) {
		gegunsCards[p][handnum] = on ? 1 : 0;
	}
}

/** Whether the hand's flash is drawn this tick: its own, or the release's cards on a silenced gun. */
static s32 gegunsFlashLit(struct hand *hand)
{
	const s32 p = g_Vars.currentplayernum;
	f32 flash[3];
	f32 star[3];

	if (hand->flashon) {
		return 1;
	}

	if (p < 0 || p >= MAX_PLAYERS || !g_Vars.currentplayer) {
		return 0;
	}

	for (s32 h = 0; h < 2; h++) {
		if (hand == &g_Vars.currentplayer->hands[h]) {
			return gegunsCards[p][h] && gebeanFirstPersonFlashCards(hand->gset.weaponnum, flash, star);
		}
	}

	return 0;
}

void gegunsOwnModelParts(struct hand *hand, struct model *model)
{
	if (!gegunsOwnModelInUse(hand->gset.weaponnum)) {
		return;
	}

	for (s32 part = 8; part <= 13; part++) {
		gegunsSetPart(model, part, 1);
	}

	gegunsSetPart(model, 35, 1);
	gegunsSetPart(model, 14, !gegunsOwnThrowKnifeGone(hand));
	gegunsSetPart(model, 15, !gegunsOwnThrowKnifeGone(hand));
	gegunsSetPart(model, 1, gegunsFlashLit(hand));

	// The shotguns' shells on the side of the gun, the top one going first:
	// shell i (parts 18 + i and 23 + i) while five - i or more are shown
	if (gegunsIsShotgun(hand->gset.weaponnum)) {
		for (s32 i = 0; i < 5; i++) {
			gegunsSetPart(model, 18 + i, hand->geshells >= 5 - i);
			gegunsSetPart(model, 23 + i, hand->geshells >= 5 - i);
		}
	}
}

/**
 * GoldenEye's shotgun and automatic shotgun carry five shells on the side of
 * the gun, and show one for each round in reserve, up to five - counted when
 * the gun is loaded, whether raised or reloaded (gunfire.c's
 * sub_GAME_7F0649D8() sets hand->numvisibleshells, and the draw hides
 * Switches[18..22] and [23..27] by it), not as each shot is fired or ammo is
 * picked up. With a shotgun in each hand the reserve is shared, and a hand
 * counts only what the other is not already showing
 * (get_ammo_in_hands_weapon()). Called as a clip is filled from the reserve
 * (bondgun.c's bgun0f098df8()).
 */
void gegunsShellsLoaded(struct hand *hand)
{
	struct player *player = g_Vars.currentplayer;
	s32 handnum;
	s32 shells;
	const struct hand *other;

	if (!player || !hand || !gegunsIsShotgun(hand->gset.weaponnum)) {
		return;
	}

	handnum = hand == &player->hands[HAND_LEFT] ? HAND_LEFT : HAND_RIGHT;

	if (hand != &player->hands[handnum] || hand->ammotypes[0] < 0) {
		return;
	}

	other = &player->hands[1 - handnum];
	shells = player->ammoheldarr[hand->ammotypes[0]];

	if (other->inuse && gegunsIsShotgun(other->gset.weaponnum)) {
		shells -= other->geshells;
	}

	hand->geshells = (s8)(shells >= 5 ? 5 : shells < 0 ? 0 : shells);
}

/**
 * The same switches for the gun held up bare in a menu (Perfect Dark's
 * inventory): no hand, no cuff, no flash, a thrown item's own pieces on - as
 * GoldenEye's watch shows its guns. Its model's toggles all start on, and
 * showed Bond's hand and a lit flash on every gun in the N64 look.
 */
void gegunsOwnModelMenuParts(s32 weaponnum, struct model *model)
{
	if (!gegunsOwnModelInUse(weaponnum)) {
		return;
	}

	for (s32 part = 8; part <= 13; part++) {
		gegunsSetPart(model, part, 0);
	}

	gegunsSetPart(model, 35, 0);
	gegunsSetPart(model, 14, 1);
	gegunsSetPart(model, 15, 1);
	gegunsSetPart(model, 1, 0);
}

/**
 * GoldenEye's knife slash on its own model (the N64 look).
 *
 * Perfect Dark's combat knife slashes with a skeletal animation of its own
 * model (ANIM_GUN_KNIFE_SLASH), which the hunting knife still runs for its
 * timing; on GoldenEye's model it is not applied (bgunSetGunMatrices()), since
 * that model has none of Perfect Dark's joints - so in the N64 look the knife
 * stood still in the hand while it cut (F3 20260926-063909, Silo). GoldenEye
 * moves the whole gun instead: gunfire.c picks one of two keyframe tracks at
 * random (GUN_ANIM_STATE_KNIFE_SLASH1/2, D_80034CA4 and D_80034E0C in gun.c),
 * and gunSample1PTransform() turns the time since the slash began into a
 * matrix (field_8EC) that the gun's placement takes before its sway - the
 * descendant of which in Perfect Dark is the hand's posrotmtx, taken in the
 * same place by the same arithmetic (bgun0f0a5550()).
 *
 * The keyframes are GoldenEye's: a position in the camera's space (+x right,
 * +y up, +z back), three angles in radians, the spline's tension and the
 * keyframe's length in sixtieths. The strike lands 24 sixtieths in, which is
 * where Perfect Dark's own slash script lands its hit (waittime 24), and the
 * whole swing is 52.
 */
// (struct geknifekey is geguns.h's: the slappers' swing is sampled the same way)
static const struct geknifekey geKnifeSlash[2][10] = {
	{ // D_80034CA4
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 8.0f },
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 8.0f },
		{ 0, { 6.0f, -1.5f, 0.0f }, { 5.6415639f, 0.23511f, 0.13564f }, 0.5f, 8.0f },
		{ 0, { 12.5f, -3.5f, 0.0f }, { 6.0422268f, 0.04475f, 0.555717f }, 0.5f, 8.0f },
		{ 0, { -10.0f, -11.0f, 0.0f }, { 1.241009f, 0.316988f, 1.086363f }, 0.5f, 8.0f },
		{ 0, { -14.0f, -15.0f, 0.0f }, { 1.830307f, 6.1436629f, 1.274134f }, 0.5f, 10.0f },
		{ 0, { -1.0f, -9.0f, 0.0f }, { 0.384244f, 0.360323f, 0.105151f }, 0.5f, 10.0f },
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 20.0f },
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 20.0f },
		{ 1, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f },
	},
	{ // D_80034E0C
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 8.0f },
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 8.0f },
		{ 0, { -8.5f, -6.0f, 0.0f }, { 5.4830351f, 5.8345609f, 6.0827341f }, 0.5f, 8.0f },
		{ 0, { -3.0f, -3.5f, 0.0f }, { 0.402412f, 5.7293859f, 5.6918988f }, 0.5f, 8.0f },
		{ 0, { -0.5f, -8.5f, 0.0f }, { 1.234298f, 5.7315431f, 5.608871f }, 0.5f, 8.0f },
		{ 0, { 7.0f, -28.5f, -1.5f }, { 1.306924f, 5.695158f, 5.6958299f }, 0.5f, 10.0f },
		{ 0, { -1.5f, -9.0f, 0.0f }, { 0.067808f, 6.2595758f, 0.203519f }, 0.5f, 10.0f },
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 20.0f },
		{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 20.0f },
		{ 1, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f },
	},
};

// Each hand's slash: which track, and how far into it in sixtieths (-1 none)
static s8 geKnifeTrack[2] = { -1, -1 };
static f32 geKnifeTime[2];

// quaternion.c's own, from GoldenEye's: w first
static void geQuatFromAngles(const f32 *angles, f32 *q)
{
	const f32 cx = cosf(angles[0] * 0.5f), sx = sinf(angles[0] * 0.5f);
	const f32 cy = cosf(angles[1] * 0.5f), sy = sinf(angles[1] * 0.5f);
	const f32 cz = cosf(angles[2] * 0.5f), sz = sinf(angles[2] * 0.5f);

	q[0] = cx * cy * cz + sx * sy * sz;
	q[1] = sx * cy * cz - cx * sy * sz;
	q[2] = cx * sy * cz + sx * cy * sz;
	q[3] = cx * cy * sz - sx * sy * cz;
}

static void geQuatShortest(const f32 *q1, f32 *q2)
{
	if (q1[0] * q2[0] + q1[1] * q2[1] + q1[2] * q2[2] + q1[3] * q2[3] < 0.0f) {
		for (s32 i = 0; i < 4; i++) {
			q2[i] = -q2[i];
		}
	}
}

static void geQuatMult(const f32 *a, const f32 *b, f32 *r)
{
	r[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
	r[1] = a[0] * b[1] + b[0] * a[1] + a[2] * b[3] - a[3] * b[2];
	r[2] = a[0] * b[2] + b[0] * a[2] + a[3] * b[1] - a[1] * b[3];
	r[3] = a[0] * b[3] + b[0] * a[3] + a[1] * b[2] - a[2] * b[1];
}

// quaternion_slerp()
static void geQuatSlerp(const f32 *q1, const f32 *q2, f32 t, f32 *r)
{
	const f32 dot = q1[0] * q2[0] + q1[1] * q2[1] + q1[2] * q2[2] + q1[3] * q2[3];

	if (dot < -1.0f + 0.00001001f) {
		for (s32 i = 0; i < 4; i++) {
			r[i] = (1.0f - t) * q1[i] - q2[i] * t;
		}
	} else if (dot <= 1.0f - 0.00001001f) {
		const f32 theta = acosf(dot);
		const f32 sine = sinf(theta);
		const f32 a = sinf((1.0f - t) * theta) / sine;
		const f32 b = sinf(t * theta) / sine;

		for (s32 i = 0; i < 4; i++) {
			r[i] = a * q1[i] + q2[i] * b;
		}
	} else {
		for (s32 i = 0; i < 4; i++) {
			r[i] = (1.0f - t) * q1[i] + q2[i] * t;
		}
	}
}

// quaternion_7F05BFD4() and quaternion_7F05C068(): log and exp
static void geQuatLog(const f32 *q, f32 *r)
{
	const f32 angle = acosf(q[0] > 1.0f ? 1.0f : q[0] < -1.0f ? -1.0f : q[0]);
	const f32 sine = sinf(angle);

	r[0] = 0.0f;

	for (s32 i = 1; i < 4; i++) {
		r[i] = sine == 0.0f ? 0.0f : q[i] * (angle / sine);
	}
}

static void geQuatExp(const f32 *q, f32 *r)
{
	const f32 angle = sqrtf(q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);

	if (angle == 0.0f) {
		r[0] = 1.0f;
		r[1] = r[2] = r[3] = 0.0f;
	} else {
		const f32 k = sinf(angle) / angle;

		r[0] = cosf(angle);
		r[1] = q[1] * k;
		r[2] = q[2] * k;
		r[3] = q[3] * k;
	}
}

// quaternion_7F05C138(): the inner control point of a squad at q1
static void geQuatInner(const f32 *q0, const f32 *q1, const f32 *q2, f32 *r)
{
	const f32 conj[4] = { q1[0], -q1[1], -q1[2], -q1[3] };
	f32 a[4], b[4], la[4], lb[4], e[4];

	geQuatMult(conj, q0, a);
	geQuatMult(conj, q2, b);
	geQuatLog(a, la);
	geQuatLog(b, lb);

	for (s32 i = 0; i < 4; i++) {
		la[i] = -(la[i] + lb[i]) * 0.25f;
	}

	geQuatExp(la, e);
	geQuatMult(q1, e, r);
}

// quaternion_7F05C2F0() by way of quaternion_7F05C250(): squad
static void geQuatSquad(f32 *q0, f32 *q1, f32 *q2, f32 *q3, f32 t, f32 *r)
{
	f32 s1[4], s2[4], a[4], b[4];

	geQuatInner(q0, q1, q2, s1);
	geQuatInner(q1, q2, q3, s2);

	geQuatShortest(q1, q2);
	geQuatSlerp(q1, q2, t, a);
	geQuatShortest(s1, s2);
	geQuatSlerp(s1, s2, t, b);
	geQuatShortest(a, b);
	geQuatSlerp(a, b, 2.0f * t * (1.0f - t), r);
}

/**
 * gunSample1PTransform(): the transform `time` sixtieths into a track, into
 * `mtx`. 0 once the track has ended (its last pose is written).
 */
s32 gegunsSampleTrack(const struct geknifekey *keys, f32 time, void *out, s32 left)
{
	Mtxf *mtx = out;
	const struct geknifekey *cur;
	f32 q[4][4];
	f32 rot[4];
	f32 pos[3];
	f32 frac, sq, cube, tension, a, b, c, d, n;
	s32 i = 1;

	while (time >= keys[i].duration) {
		time -= keys[i].duration;
		i++;

		if (keys[i + 2].last & 1) {
			break;
		}
	}

	cur = &keys[i];

	if (cur[2].last & 1) {
		// matrix_4x4_set_rotation_around_xyz() of the pose it ends on
		struct coord angles = { cur->rot[0], cur->rot[1], cur->rot[2] };

		mtx4LoadRotation(&angles, mtx);
		mtx->m[3][0] = cur->pos[0];
		mtx->m[3][1] = cur->pos[1];
		mtx->m[3][2] = cur->pos[2];
		return 0;
	}

	frac = time / cur->duration;
	tension = cur->tension;

	geQuatFromAngles(cur[-1].rot, q[0]);
	geQuatFromAngles(cur[0].rot, q[1]);
	geQuatFromAngles(cur[1].rot, q[2]);
	geQuatFromAngles(cur[2].rot, q[3]);

	geQuatShortest(q[1], q[2]);
	geQuatShortest(q[2], q[3]);
	geQuatShortest(q[1], q[0]);

	geQuatSquad(q[0], q[1], q[2], q[3], frac, rot);

	// coord3dCubicSplineInterp()
	sq = frac * frac;
	cube = sq * frac;
	a = (2.0f * sq - (frac + cube)) * tension;
	b = (2.0f - tension) * cube + sq * (tension - 3.0f) + 1.0f;
	c = (tension - 2.0f) * cube + sq * (3.0f - 2.0f * tension) + frac * tension;
	d = (cube - sq) * tension;

	for (s32 k = 0; k < 3; k++) {
		pos[k] = a * cur[-1].pos[k] + b * cur[0].pos[k] + c * cur[1].pos[k] + d * cur[2].pos[k];
	}

	if (left) {
		pos[0] = -pos[0];
		rot[0] = -rot[0];
		rot[1] = -rot[1];
	}

	// quaternion_to_matrix()
	n = 2.0f / (rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]);

	{
		const f32 x2 = rot[1] * n, y2 = rot[2] * n, z2 = rot[3] * n;
		const f32 wx = rot[0] * x2, wy = rot[0] * y2, wz = rot[0] * z2;
		const f32 xx = rot[1] * x2, xy = rot[1] * y2, xz = rot[1] * z2;
		const f32 yy = rot[2] * y2, yz = rot[2] * z2, zz = rot[3] * z2;

		mtx->m[0][0] = 1.0f - (yy + zz);
		mtx->m[0][1] = xy + wz;
		mtx->m[0][2] = xz - wy;
		mtx->m[0][3] = 0.0f;
		mtx->m[1][0] = xy - wz;
		mtx->m[1][1] = 1.0f - (xx + zz);
		mtx->m[1][2] = yz + wx;
		mtx->m[1][3] = 0.0f;
		mtx->m[2][0] = xz + wy;
		mtx->m[2][1] = yz - wx;
		mtx->m[2][2] = 1.0f - (xx + yy);
		mtx->m[2][3] = 0.0f;
		mtx->m[3][0] = pos[0];
		mtx->m[3][1] = pos[1];
		mtx->m[3][2] = pos[2];
		mtx->m[3][3] = 1.0f;
	}

	return 1;
}

/**
 * A slash has begun in this hand (bgunTickIncAttackingMelee()): GoldenEye's
 * swing, one of its two at random, where the hunting knife is drawn on
 * GoldenEye's own model. A slash begun during one starts it again, as
 * GoldenEye's does.
 */
void gegunsOwnMeleeStart(struct hand *hand, s32 handnum)
{
	if (handnum < 0 || handnum > 1) {
		return;
	}

	if (hand->gset.weaponnum != WEAPON_GE_HUNTINGKNIFE || !gegunsOwnModelInUse(hand->gset.weaponnum)) {
		geKnifeTrack[handnum] = -1;
		return;
	}

	geKnifeTrack[handnum] = (rngRandom() & 1) ? 1 : 0;
	geKnifeTime[handnum] = 0.0f;
}

/**
 * Each tick, after the hand's states (which clear posrotmtx): the swing
 * `lvupdate60` sixtieths further on, as the hand's posrotmtx. It stops at the
 * track's end, and at once if the knife is put away or the look changes.
 */
void gegunsOwnMeleeTick(struct hand *hand, s32 handnum, f32 lvupdate60)
{
	if (handnum < 0 || handnum > 1 || geKnifeTrack[handnum] < 0) {
		return;
	}

	if (hand->gset.weaponnum != WEAPON_GE_HUNTINGKNIFE || !gegunsOwnModelInUse(hand->gset.weaponnum)
			|| hand->state == HANDSTATE_CHANGEGUN) {
		geKnifeTrack[handnum] = -1;
		return;
	}

	geKnifeTime[handnum] += lvupdate60;

	if (gegunsSampleTrack(geKnifeSlash[geKnifeTrack[handnum]], geKnifeTime[handnum], &hand->posrotmtx, handnum == HAND_LEFT)) {
		hand->useposrot = true;
	} else {
		geKnifeTrack[handnum] = -1;
	}
}

/**
 * GoldenEye's knife throw on its own model (the N64 look): F3 20260926-110900,
 * "throwing knife no animation". The throw's timing is Perfect Dark's combat
 * knife's (invanim_combatknife_throw: held back at keyframe 12 while Z is held,
 * gone at 16), whose skeletal animation does not apply to GoldenEye's model -
 * so the knife stood still and a knife flew out of it. GoldenEye swings the
 * whole gun (gunfire.c's GUN_ANIM_STATE_THROWKNIFE_DRAW, _THROW and _RECOVER):
 * the draw back (throwKnifeDrawBackKeyframes, 16 sixtieths, then held drawn
 * back on the release track's first pose while Z is held), and once the knife
 * has gone the follow-through (throwKnifeReleaseKeyframes, 28 sixtieths) with
 * the knife's own pieces, parts 14 and 15, off (field_87E = 0) until it ends.
 */
static const struct geknifekey geKnifeDrawBack[6] = {
	{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, 0.0f, 4.5f }, { 5.576369f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, 0.0f, 20.5f }, { 5.26209f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, 3.0f, 5.5f }, { 0.031375f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 1, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f },
};

static const struct geknifekey geKnifeRelease[6] = {
	{ 0, { 0.0f, 0.0f, 4.5f }, { 5.576369f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, 0.0f, 20.5f }, { 5.26209f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, 3.0f, 5.5f }, { 0.031375f, 0.0f, 0.0f }, 0.5f, 8.0f },
	{ 0, { 0.0f, -20.0f, 18.0f }, { 0.785458f, 0.0f, 0.0f }, 0.5f, 20.0f },
	{ 0, { 0.0f, -20.0f, 18.0f }, { 0.785458f, 0.0f, 0.0f }, 0.5f, 20.0f },
	{ 1, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f },
};

enum { GETHROW_NONE, GETHROW_DRAW, GETHROW_RECOVER };

// Each hand's throw: which step, and how far into it in sixtieths
static s8 geThrowStep[2];
static f32 geThrowTime[2];

static s32 gegunsOwnThrowApplies(const struct hand *hand)
{
	return hand->gset.weaponnum == WEAPON_GE_THROWINGKNIFE && gegunsOwnModelInUse(hand->gset.weaponnum);
}

/**
 * A throw has begun in this hand (bgunTickIncAttackingThrow()): the draw back.
 */
void gegunsOwnThrowStart(struct hand *hand, s32 handnum)
{
	if (handnum < 0 || handnum > 1) {
		return;
	}

	geThrowStep[handnum] = gegunsOwnThrowApplies(hand) ? GETHROW_DRAW : GETHROW_NONE;
	geThrowTime[handnum] = 0.0f;
}

/**
 * Each tick, after the hand's states: the draw back until the knife leaves
 * the hand (the throw state past HANDSTATEMINOR_ATTACK_THROW_0), then the
 * follow-through to its end. Stops at once if the knife is put away or the
 * look changes.
 */
void gegunsOwnThrowTick(struct hand *hand, s32 handnum, f32 lvupdate60)
{
	if (handnum < 0 || handnum > 1 || geThrowStep[handnum] == GETHROW_NONE) {
		return;
	}

	if (!gegunsOwnThrowApplies(hand) || hand->state == HANDSTATE_CHANGEGUN) {
		geThrowStep[handnum] = GETHROW_NONE;
		return;
	}

	if (geThrowStep[handnum] == GETHROW_DRAW
			&& (hand->state != HANDSTATE_ATTACK || hand->stateminor != HANDSTATEMINOR_ATTACK_THROW_0)) {
		geThrowStep[handnum] = GETHROW_RECOVER;
		geThrowTime[handnum] = 0.0f;
	} else {
		geThrowTime[handnum] += lvupdate60;
	}

	if (geThrowStep[handnum] == GETHROW_DRAW) {
		if (!gegunsSampleTrack(geKnifeDrawBack, geThrowTime[handnum], &hand->posrotmtx, handnum == HAND_LEFT)) {
			// drawn back and held there
			gegunsSampleTrack(geKnifeRelease, 0.0f, &hand->posrotmtx, handnum == HAND_LEFT);
		}

		hand->useposrot = true;
		return;
	}

	// The follow-through ends below the view, and is held there until the
	// next knife is taken up: Perfect Dark waits out the throw's recovery and
	// then lowers and raises the hand to fill its one-knife clip, which would
	// otherwise have brought the empty hand back to rest in between (GoldenEye
	// goes from the follow-through straight to the knife coming up)
	if (!gegunsSampleTrack(geKnifeRelease, geThrowTime[handnum], &hand->posrotmtx, handnum == HAND_LEFT)
			&& (hand->loadedammo[0] > 0 || (hand->state != HANDSTATE_ATTACK && hand->state != HANDSTATE_RELOAD))) {
		geThrowStep[handnum] = GETHROW_NONE;
		return;
	}

	hand->useposrot = true;
}

/**
 * Whether the knife itself is out of the hand: thrown, and the hand still
 * following through (GoldenEye's field_87E) or waiting for the next.
 */
static s32 gegunsOwnThrowKnifeGone(const struct hand *hand)
{
	struct player *player = g_Vars.currentplayer;

	if (hand->gset.weaponnum != WEAPON_GE_THROWINGKNIFE) {
		return 0;
	}

	for (s32 i = 0; player && i < 2; i++) {
		if (hand == &player->hands[i]) {
			return geThrowStep[i] == GETHROW_RECOVER;
		}
	}

	return 0;
}

/**
 * Whether nothing of GoldenEye's own throwing knife is drawn in this hand:
 * from two sixtieths after the knife has gone until the next is taken up.
 * GoldenEye clears field_87F then (gunfire.c: an empty magazine on a
 * SINGLE_USE_RELOAD item), so its follow-through is never seen - on the
 * native port the hand is gone the second tick after the release and the
 * screen is empty until the next knife rises. The follow-through pose drew
 * the empty hand at the bottom of the screen through the recovery.
 */
s32 gegunsOwnThrowHidesHand(const struct hand *hand)
{
	struct player *player = g_Vars.currentplayer;

	if (!hand || hand->gset.weaponnum != WEAPON_GE_THROWINGKNIFE) {
		return 0;
	}

	for (s32 i = 0; player && i < 2; i++) {
		if (hand == &player->hands[i]) {
			return geThrowStep[i] == GETHROW_RECOVER && geThrowTime[i] >= 2.0f;
		}
	}

	return 0;
}

/**
 * Whether this model is the gun in one of the current player's hands and the
 * hand has no rocket in it: none made yet (reloading, out of ammo) or the one
 * it had just fired. The release's rocket launcher is made with its rocket in
 * the tube, and is drawn without it then (gebean.c's fpRound), as GoldenEye's
 * own gun is empty until bondgun.c's held rocket is hung in its mouth. Any
 * other model - one not in a hand - is drawn as it is made.
 */
s32 gegunsHandIsSpent(const struct model *model)
{
	struct player *player = g_Vars.currentplayer;

	if (!player || !model) {
		return 0;
	}

	for (s32 i = 0; i < 2; i++) {
		const struct hand *hand = &player->hands[i];

		if (model == &hand->gunmodel) {
			return hand->rocket == NULL || hand->firedrocket;
		}
	}

	return 0;
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

	if (!gegunsOwnModelInUse(weaponnum)) {
		return NULL;
	}

	// The rocket launcher has part 3 and no flash switch over it, so its own
	// matrix is always posed and is the barrel's end itself. GoldenEye reads
	// part 3 whether or not there is a switch (gunfire.c's flashdata), and
	// hangs the loaded rocket there; with no node the muzzle fell back to the
	// hand's world matrix and the rocket was posed off in the distance.
	if (!(flash = modelGetPart(modeldef, 1))) {
		node = modelGetPart(modeldef, 3);

		return node && (node->type & 0xff) == MODELNODETYPE_POSITION ? node : NULL;
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

/**
 * The slide on GoldenEye's own model: part 7 (gunfire.c's Switches[7]), which
 * it moves back along the gun's z by field_A88 after each shot - out by the
 * gun's BoltRecoilBack over four ticks, home over six, and left back while
 * the magazine is empty (gun.c's sub_GAME_7F05E83C()). That is Perfect Dark's
 * own slide to the letter (bgunUpdateSlide(), the shoot function's slidemax
 * being the same stat), which moves its host's MODELPART_GUN_SLIDE - a part
 * GoldenEye's model does not have, so the pistols fired with the slide stood
 * still (F3 20260929-092030, -092506).
 */
struct modelnode *gegunsOwnModelSlide(s32 weaponnum, struct modeldef *modeldef)
{
	struct modelnode *node;

	if (!modeldef || !gegunsOwnModelInUse(weaponnum)) {
		return NULL;
	}

	node = modelGetPart(modeldef, 7);

	return node && (node->type & 0xff) == MODELNODETYPE_POSITION ? node : NULL;
}

/**
 * Where GoldenEye's own model throws its spent cases from: part 0
 * (gunfire.c's Switches[0], the position sub_GAME_7F068508() starts a casing
 * at, in the gun's own frame). Without it the case left from the hand's
 * origin, which is the wrist (F3 20260929-092506: "the bullets eject from the
 * hand").
 */
struct modelnode *gegunsOwnModelCasingPort(s32 weaponnum, struct modeldef *modeldef)
{
	struct modelnode *node;

	if (!modeldef || !gegunsOwnModelInUse(weaponnum)) {
		return NULL;
	}

	// converted as Perfect Dark converts a host's ejection port, a held
	// position with a matrix of its own
	node = modelGetPart(modeldef, 0);

	return node && ((node->type & 0xff) == MODELNODETYPE_POSITIONHELD
			|| (node->type & 0xff) == MODELNODETYPE_POSITION) ? node : NULL;
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
 * A star under the flash that gunfire.c never poses: the ZMG carries a second
 * one in the flash as the KF7 does (part 4, 68 units out past its first), but
 * only the KF7's skeleton has it posed, and GoldenEye leaves its matrix the
 * identity - at the eye, where the near plane clips it away. Here it kept the
 * matrix the model's own pose gave it, and its quad covered the whole view
 * in grey for every frame the flash was lit (F3 20260927-210532 and
 * -210555, the ZMG akimbo on GoldenEye Arenas' Control). Nothing of it is
 * drawn: every vertex goes to the flash's own place.
 */
static void gegunsOwnModelFlashUnposed(struct model *model, struct modelnode *star, struct modelnode *flash, Mtxf *flashmtx)
{
	struct modelnode *node;
	Mtxf *mtx;

	if (!star || (star->type & 0xff) != MODELNODETYPE_POSITION
			|| modelFindNodeMtxIndex(star, 0) == modelFindNodeMtxIndex(flash, 0)) {
		return;
	}

	// under the flash, not the Cougar's cylinder
	for (node = star->parent; node && node != flash; node = node->parent);

	if (!node) {
		return;
	}

	mtx = &model->matrices[modelFindNodeMtxIndex(star, 0)];

	for (s32 r = 0; r < 4; r++) {
		for (s32 col = 0; col < 4; col++) {
			mtx->m[r][col] = r == 3 ? flashmtx->m[3][col] : 0.0f;
		}
	}
}

/**
 * GoldenEye's own muzzle flash, posed as gunfire.c poses it: GoldenEye never
 * lets the model pose it. The flash (part 3's matrix) is the gun's matrix
 * with a random roll (only on the guns whose row asks for one), 1 to 1.25
 * times the size, and stretched along the barrel
 * by the gun's MuzzleFlashExtension; the star (part 2's, and part 4's on the
 * KF7 alone) faces the eye at its place in the flash, a tenth the size. Posed as a
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
	f32 roll;
	Mtxf *parent;
	Mtxf *flashmtx;
	f32 cardflash[3];
	f32 cardstar[3];
	s32 cards;

	if (!gegunsFlashLit(hand) || !gegunsOwnModelInUse(hand->gset.weaponnum) || !model->matrices) {
		return;
	}

	base = gegunsOwnModelMuzzle(hand->gset.weaponnum, def, off);
	flash = modelGetPart(def, 3);

	if (!base || !flash || modelFindNodeMtxIndex(flash, 0) == modelFindNodeMtxIndex(base, 0)) {
		return;
	}

	// The HD look draws the release's own flash cards in these lists
	// (gebean.c's fpCardSet), turned about the release's muzzle and its
	// star's place rather than GoldenEye's
	cards = gebeanFirstPersonFlashCards(hand->gset.weaponnum, cardflash, cardstar);

	if (cards) {
		off[0] = cardflash[0];
		off[1] = cardflash[1];
		off[2] = cardflash[2];
	}

	parent = &model->matrices[modelFindNodeMtxIndex(base, 0)];
	flashmtx = &model->matrices[modelFindNodeMtxIndex(flash, 0)];
	scale = gegunsRandFrac() * 0.25f + 1.0f;
	ext = g_Weapons[hand->gset.weaponnum]->muzzlez;
	unit = sqrtf(parent->m[0][0] * parent->m[0][0] + parent->m[0][1] * parent->m[0][1] + parent->m[0][2] * parent->m[0][2]);

	// GoldenEye rolls the flash at random only on a gun whose row has bit
	// 0x1 (gunfire.c); the KF7, AR33, RC-P90 and sniper rifle lack it, and
	// their flash keeps the model's own turn, lined up with the vents
	// (F3 20261001-165454)
	roll = (stats[hand->gset.weaponnum - WEAPON_GE_FIRST].bitflags & GESTATFLAG_ROLL_FLASH)
		? gegunsRandFrac() * M_BADTAU : 0.0f;

	gegunsFlashMatrix(flashmtx, parent, roll, scale, ext, off, 0);

	for (s32 part = 2; part <= 4; part += 2) {
		struct modelnode *star = modelGetPart(def, part);
		f32 at[3];

		// part 4 is a second star only on the KF7 (gunfire.c tests for
		// skeleton_gun_kf7). On the Cougar's revolver skeleton it is the
		// cylinder, which this billboarded into the flash for the frame of
		// every shot: "shows a drum for a second when it's fired"
		if (part == 4 && hand->gset.weaponnum != WEAPON_GE_KF7SOVIET) {
			gegunsOwnModelFlashUnposed(model, star, flash, flashmtx);
			continue;
		}

		if (!star || (star->type & 0xff) != MODELNODETYPE_POSITION
				|| modelFindNodeMtxIndex(star, 0) == modelFindNodeMtxIndex(flash, 0)) {
			continue;
		}

		// its place in the flash's own space, into the eye's
		{
			const f32 sx = cards == 2 && part == 2 ? cardstar[0] : star->rodata->position.pos.x;
			const f32 sy = cards == 2 && part == 2 ? cardstar[1] : star->rodata->position.pos.y;
			const f32 sz = cards == 2 && part == 2 ? cardstar[2] : star->rodata->position.pos.z;

			for (s32 col = 0; col < 3; col++) {
				at[col] = sx * flashmtx->m[0][col] + sy * flashmtx->m[1][col] + sz * flashmtx->m[2][col]
					+ flashmtx->m[3][col];
			}
		}

		gegunsFlashMatrix(&model->matrices[modelFindNodeMtxIndex(star, 0)], NULL,
				gegunsRandFrac() * M_BADTAU, unit * scale, ext, at, 1);
	}
}

/**
 * The revolver skeleton's moving parts, posed as gunfire.c poses them on
 * GoldenEye's own model: part 4 is the cylinder, part 5 the hammer
 * (skeleton_gun_revolver: the Cougar has both, the grenade launcher only the
 * drum). Posed as a plain model both stood still, so the grenade launcher's
 * drum never turned and the Cougar's hammer never moved (F3 20260928-002238
 * and -002802).
 *
 * GoldenEye turns them while the trigger is held down before the shot
 * (GUN_ANIM_STATE_TRIGGER_PRESS, field_890 counting sixtieths up to 6), which
 * here is the attack's first minor state for the six ticks of
 * gegunsTriggerDelay60(). The grenade launcher's drum turns a sixth of a turn
 * over those six ticks; the Cougar's turns a chamber on to the one the shot
 * leaves (and rests at one chamber a spent round), and its hammer goes back
 * 30 degrees over the first three ticks and falls over the last three. Each is
 * a rotation in the part's own space, about z for the drum and x for the
 * hammer, as GoldenEye's rwmtx[3] and [4] are the gun's matrix times the
 * rotation at the part's place. It is applied as the model's matrices are
 * set (g_ModelJointPositionedFunc), so whatever hangs under the part follows.
 */
static s32 geRevCylMtx = -1;
static s32 geRevHammerMtx = -1;
static f32 geRevCylAngle;
static f32 geRevHammerAngle;
static void (*geRevPrevFunc)(s32 mtxindex, Mtxf *mtx);

static void gegunsRevolverRotate(Mtxf *mtx, s32 axis, f32 angle)
{
	const f32 c = cosf(angle);
	const f32 s = sinf(angle);
	Mtxf rot;
	Mtxf out;

	// matrixmath.c's matrix_4x4_set_rotation_around_x/z
	mtx4LoadIdentity(&rot);

	if (axis == 0) {
		rot.m[1][1] = c; rot.m[1][2] = s;
		rot.m[2][1] = -s; rot.m[2][2] = c;
	} else {
		rot.m[0][0] = c; rot.m[0][1] = s;
		rot.m[1][0] = -s; rot.m[1][1] = c;
	}

	mtx4MultMtx4(mtx, &rot, &out);
	mtx4Copy(&out, mtx);
}

static void gegunsRevolverJoint(s32 mtxindex, Mtxf *mtx)
{
	if (geRevPrevFunc) {
		geRevPrevFunc(mtxindex, mtx);
	}

	if (mtxindex == geRevCylMtx && geRevCylAngle != 0.0f) {
		gegunsRevolverRotate(mtx, 2, geRevCylAngle);
	}

	if (mtxindex == geRevHammerMtx && geRevHammerAngle != 0.0f) {
		gegunsRevolverRotate(mtx, 0, geRevHammerAngle);
	}
}

/** Whether the gun is drawn on GoldenEye's own revolver skeleton, whose parts move. */
s32 gegunsOwnModelRevolver(s32 weaponnum)
{
	return (weaponnum == WEAPON_GE_COUGARMAGNUM || weaponnum == WEAPON_GE_GRENADELAUNCHER)
		&& gegunsOwnModelInUse(weaponnum);
}

/** Before the own model's matrices are set: install the drum's and hammer's turn. */
void gegunsOwnModelRevolverBegin(struct hand *hand, struct model *model)
{
	const s32 weaponnum = hand->gset.weaponnum;
	struct modeldef *def = model->definition;
	struct modelnode *cyl;
	struct modelnode *hammer;
	s32 pressing;
	s32 t;
	s32 ammo;

	geRevCylMtx = geRevHammerMtx = -1;
	geRevPrevFunc = NULL;

	if (!gegunsOwnModelRevolver(weaponnum) || !def) {
		return;
	}

	cyl = modelGetPart(def, 4);
	hammer = modelGetPart(def, 5);

	if (cyl && (cyl->type & 0xff) == MODELNODETYPE_POSITION) {
		geRevCylMtx = modelFindNodeMtxIndex(cyl, 0);
	}

	if (hammer && (hammer->type & 0xff) == MODELNODETYPE_POSITION) {
		geRevHammerMtx = modelFindNodeMtxIndex(hammer, 0);
	}

	if (geRevCylMtx < 0 && geRevHammerMtx < 0) {
		return;
	}

	// field_890 in TRIGGER_PRESS: the ticks since the press, before the shot
	pressing = hand->state == HANDSTATE_ATTACK && hand->stateminor == HANDSTATEMINOR_ATTACK_SHOOT_0
		&& hand->stateframes < TICKS(gegunsTriggerDelay60(weaponnum));
	t = hand->stateframes * 60 / TICKS(60);
	ammo = hand->loadedammo[0];

	geRevCylAngle = 0.0f;
	geRevHammerAngle = 0.0f;

	if (weaponnum == WEAPON_GE_COUGARMAGNUM) {
		geRevCylAngle = pressing
			? ((t - ammo * 6) + 30) * M_BADTAU / 36.0f
			: (6 - ammo) * M_BADTAU / 6.0f;

		if (pressing) {
			geRevHammerAngle = (t < 3 ? -t : -(6 - t)) * 2.0f * (M_BADTAU / 12.0f) / 6.0f;
		}
	} else if (pressing && t < 6) {
		geRevCylAngle = t * M_BADTAU / 36.0f;
	}

	geRevPrevFunc = g_ModelJointPositionedFunc;
	g_ModelJointPositionedFunc = gegunsRevolverJoint;
}

/** After: put back whatever joint function was there. */
void gegunsOwnModelRevolverEnd(void)
{
	if (g_ModelJointPositionedFunc == gegunsRevolverJoint) {
		g_ModelJointPositionedFunc = geRevPrevFunc;
	}

	geRevCylMtx = geRevHammerMtx = -1;
	geRevPrevFunc = NULL;
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
		{ WEAPON_GE_HUNTINGKNIFE,  INVENTORYFUNCTYPE_THROW,   LANG_N("Throw Knife\n") },
		{ WEAPON_GE_THROWINGKNIFE, INVENTORYFUNCTYPE_THROW,   LANG_N("Throw Knife\n") },
		{ WEAPON_GE_COVERTMODEM,   INVENTORYFUNCTYPE_THROW,   LANG_N("Attach\n") },
		{ WEAPON_GE_PLASTIQUE,     INVENTORYFUNCTYPE_THROW,   LANG_N("Place\n") },
		{ WEAPON_GE_GOLDENEYEKEY,  INVENTORYFUNCTYPE_THROW,   LANG_N("Put Down\n") },
		{ WEAPON_GE_CAMERA,        INVENTORYFUNCTYPE_SPECIAL, LANG_N("Photograph\n") },
		{ WEAPON_GE_WATCHMAGNET,   INVENTORYFUNCTYPE_SPECIAL, LANG_N("Attract\n") },
		{ WEAPON_GE_GADGETA,       INVENTORYFUNCTYPE_SPECIAL, LANG_N("Use\n") },
		{ WEAPON_GE_GADGETB,       INVENTORYFUNCTYPE_SPECIAL, LANG_N("Use\n") },
		{ WEAPON_GE_TANKSHELLS,    INVENTORYFUNCTYPE_SPECIAL, LANG_N("Fire\n") },
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

/**
 * A simulant's view of GoldenEye gun i (g_GeAibotWeaponPreferences, read
 * through AIBOTPREF()): its host's row, less a second function the gun no
 * longer has - a simulant chose it, loaded nothing and held a gun it never
 * fired - with the throwing knife's throw as its first, and dual scores where
 * the host's row has none but the gun can be held in both hands (the KF7, the
 * D5Ks, the AR33 and the RC-P90 stand on the port's classic rows, whose dual
 * scores are 0), by the rows' own step (the ZZT's 116/128 to 136/152).
 */
static void gegunsBotPrefs(s32 i)
{
	const struct weapon *def = &g_GeWeaponDefs[i];
	struct aibotweaponpreference *pref = &g_GeAibotWeaponPreferences[i];

	*pref = g_AibotWeaponPreferences[g_GeWeaponHosts[i]];

	if (WEAPON_GE_FIRST + i == WEAPON_GE_THROWINGKNIFE) {
		pref->unk00 = pref->unk01;
		pref->unk02 = pref->unk03;
		pref->haspriammogoal = pref->hassecammogoal;
		pref->pridistconfig = pref->secdistconfig;
		pref->targetammopri = pref->targetammosec;
		pref->criticalammopri = pref->criticalammosec;
	}

	if (!def->functions[1]) {
		pref->unk01 = 0;
		pref->unk03 = 0;
		pref->hassecammogoal = 0;
		pref->targetammosec = 0;
		pref->criticalammosec = 0;
	}

	if ((def->flags & WEAPONFLAG_DUALWIELD) && pref->unk02 == 0 && pref->unk03 == 0) {
		pref->unk02 = pref->unk00 + 20;
		pref->unk03 = pref->unk01 ? pref->unk01 + 24 : 0;
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

		nameids[i] = langAddPortText(names[i]);

		gegunsBuild(i, host, host);
		gegunsOwnTrigger(i);
		gegunsFireRate(i);
		gegunsOwnThrown(i);
		gegunsBotPrefs(i);

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


/**
 * Gun i's single shot waits GoldenEye's own time between two held shots
 * (gegunsRecovery()), once what its first function plays on firing is
 * settled: gegunsOwnTrigger() takes some fire animations away, and a borrowed
 * model brings its own.
 */
static void gegunsFireRate(s32 i)
{
	const struct gegunstat *stat = &stats[i];
	struct weaponfunc_shoot *shoot = g_GeWeaponDefs[i].functions[0];

	if (!shoot || (shoot->base.type & 0xff) != INVENTORYFUNCTYPE_SHOOT
			|| !stat->bitflags || stat->singlerate == 0xff || stat->autorate != 0xff) {
		return;
	}

	shoot->recoverytime60 = gegunsRecovery(WEAPON_GE_FIRST + i, stat, shoot->base.fire_animation != NULL);
}

/**
 * GoldenEye's watch laser (ITEM_WATCHLASER), which the conversion stands on
 * the Moonraker's number (Train; gegadgets.c decides where). It is the same
 * beam but not the same gun: watchlaser_stats (obseg/gun/watchlaser) against
 * laser_stats -
 *
 * - its own ammunition, AMMO_WATCH_LASER, MagSize 1000 with no clip reloads
 *   and 1000 at most (gun.c's ammo_related[24]); Train starts Bond with 300.
 *   The Moonraker has none to run out of. The port's AMMOTYPE_WATCHLASER.
 * - SingleRate 0 (it fires as fast as the trigger is pulled) where the
 *   Moonraker's is 6, ObjectsShootThrough 1 and not 2, ForceOfImpact 0 and
 *   not 2, recoil speed bytes 0, 0, 0, 0xff and not 6, 0, 6, 6.
 * - quiet: loudness 1 to 4 with 0.2 a shot and a linear time of 1, where
 *   the Moonraker's is 2 to 16 with 2 a shot and 2.
 * - no auto-aim (no HAS_AUTO_AIM) and no hold time (no USE_HOLD_TIME).
 * - its own sound, watchlaser_fire_sounds (gegunsShootSound()).
 *
 * The same DestructionAmount, 2, and the same spread, 0, so the damage a hit
 * does is the Moonraker's: GoldenEye's chrDamage() takes 2 times the AI
 * health modifier times 2 on the chest, 4 on the head. Its beam is drawn at
 * most 300 units long (gunfx.c's beamCreate()).
 *
 * Swapped in and out whole, as the definition's own pointers, so the
 * Moonraker is itself again on every other stage.
 */
static const struct gegunstat watchlaserstat = {
	1000, 0xff, 0x00, 1, 2.0f, 0.0f, 0.0f,
	{ 1.0f, 4.0f, 0.2f, 1.0f, 4.0f },
	{ 0, 0, 0, -1 }, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 24, 0x00601091,
};

static struct {
	struct weaponfunc *func;       // the watch laser's shot, NULL until the first
	struct inventory_ammo ammo;
	struct invaimsettings aim;
	struct noisesettings noise;
	struct weaponfunc *hostfunc;   // the Moonraker's own, while the watch laser's is in
	struct inventory_ammo *hostammo;
	struct invaimsettings *hostaim;
	u32 hostflags;
	u32 hostflags3;
} g_WatchLaser;

static s32 gegunsWatchLaserInstalled(void)
{
	return g_WatchLaser.func && g_GeWeaponDefs[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST].functions[0] == g_WatchLaser.func;
}

void gegunsSetWatchLaser(s32 on)
{
	struct weapon *def = &g_GeWeaponDefs[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST];
	const struct gegunstat *stat = &watchlaserstat;
	struct weaponfunc_shoot *shoot;
	u32 size;

	if (!on) {
		if (gegunsWatchLaserInstalled()) {
			def->functions[0] = g_WatchLaser.hostfunc;
			def->ammos[0] = g_WatchLaser.hostammo;
			def->aimsettings = g_WatchLaser.hostaim;
			def->flags = g_WatchLaser.hostflags;
			def->flags3 = g_WatchLaser.hostflags3;
		}

		return;
	}

	if (gegunsWatchLaserInstalled() || !def->functions[0]
			|| (((struct weaponfunc *)def->functions[0])->type & 0xff) != INVENTORYFUNCTYPE_SHOOT) {
		return;
	}

	g_WatchLaser.hostfunc = def->functions[0];
	g_WatchLaser.hostammo = def->ammos[0];
	g_WatchLaser.hostaim = def->aimsettings;
	g_WatchLaser.hostflags = def->flags;
	g_WatchLaser.hostflags3 = def->flags3;

	size = gegunsFuncSize(((struct weaponfunc *)def->functions[0])->type);

	if (!g_WatchLaser.func) {
		g_WatchLaser.func = calloc(1, 0x80 > size ? 0x80 : size);

		if (!g_WatchLaser.func) {
			return;
		}
	}

	memcpy(g_WatchLaser.func, def->functions[0], size);
	shoot = (struct weaponfunc_shoot *)g_WatchLaser.func;
	shoot->damage = stat->damage;
	shoot->spread = stat->spread;
	shoot->penetration = stat->penetration;
	shoot->impactforce = stat->impactforce;
	shoot->recoildist = stat->recoilback;
	shoot->recoilangle = stat->recoilup;
	shoot->slidemax = stat->boltback;
	shoot->recoverytime60 = gegunsRecovery(WEAPON_GE_MOONRAKER, stat, g_WatchLaser.func->fire_animation != NULL);
	shoot->unk24 = stat->recoilspeed[0];
	shoot->unk25 = stat->recoilspeed[1];
	shoot->unk26 = stat->recoilspeed[2];
	shoot->unk27 = stat->recoilspeed[3];

	// the Moonraker's shot takes nothing from a magazine (-1); the watch
	// laser's takes its charge
	g_WatchLaser.func->ammoindex = 0;

	// nor the hands' shake while firing (recoilsettings, a random jitter of
	// the gun's position the Moonraker's host has): GoldenEye moves a gun
	// only by its recoil speeds and pull back, all nothing for the watch
	// laser, which fires from a still wrist (F3 20260930-191858, "has recoil")
	shoot->recoilsettings = NULL;

	g_WatchLaser.noise = stat->noise;
	g_WatchLaser.func->noisesettings = &g_WatchLaser.noise;

	memset(&g_WatchLaser.ammo, 0, sizeof(g_WatchLaser.ammo));
	g_WatchLaser.ammo.type = AMMOTYPE_WATCHLASER;
	// no casing: watchlaser_stats' ejected cartridge is NULL. A zeroed
	// casingeject is the pistol's cartridge (index 0), and the watch threw
	// one out of the hand every shot (F3 20260930-191858)
	g_WatchLaser.ammo.casingeject = (u32)-1;
	g_WatchLaser.ammo.clipsize = stat->magsize;
	def->ammos[0] = &g_WatchLaser.ammo;

	if (def->aimsettings) {
		g_WatchLaser.aim = *def->aimsettings;
		g_WatchLaser.aim.flags &= ~INVAIMFLAG_AUTOAIM;
		def->aimsettings = &g_WatchLaser.aim;
	}

	def->flags &= ~WEAPONFLAG_TRACKTIMEUSED;
	// GoldenEye never pairs it (no CAN_DUAL_WIELD), and Akimbo does not
	// either (gegunsNeverPairs())
	def->flags &= ~WEAPONFLAG_DUALWIELD;
	// the laser's shots are free (WEAPONFLAG3_FREESHOTS); each of the watch
	// laser's takes one of its charge
	def->flags3 &= ~WEAPONFLAG3_FREESHOTS;
	def->functions[0] = g_WatchLaser.func;
}

/**
 * A converted GoldenEye ROM hack's own guns: its gun set.
 *
 * GE Editor's hacks renumber the hand items and patch the code that tests an
 * item by number to follow, so each of a hack's guns is the GoldenEye gun whose
 * file it took - Goldfinger 64's AK47 is GoldenEye's KF7 at another item
 * number, its Armalite AR7 the sniper rifle, its golf club the hunting knife
 * - and stands on that gun's weapon here (geconvert.c's itemWeaponsBuild()),
 * with what is the hack's own: its gunWeaponStat row (damage, rates, recoil,
 * sound, clip, where it is held), its name, the prop a hand holds it as, and
 * its first-person model (Igx%03dZ under its own item number). Four of
 * Goldfinger's are on files GoldenEye has no gun of its own for (the taser,
 * the watch laser and the gold and silver PP7s), and take the four weapons
 * past the detonator (WEAPON_GE_EXTRA1).
 *
 * The conversion writes them as menu/geguns.bin (writeGuns()). On a stage of
 * the hack's own the guns' definitions are built from it once and swapped in
 * whole, and on any other stage GoldenEye's are swapped back, as they were -
 * as the watch laser is on Train.
 */
#define GEGUNS_SETS     4
#define GEGUNS_NAMELEN  40
#define GEGUNS_STATROW  0x70
#define GEGUNS_ROW      (4 + GEGUNS_NAMELEN + GEGUNS_STATROW)

struct gegunset {
	s32 moddir;
	struct gegunstat stats[NUM_GE_WEAPONS];
	u8 sounds[NUM_GE_WEAPONS];
	u8 soundrates[NUM_GE_WEAPONS];
	u8 items[NUM_GE_WEAPONS];
	s16 props[NUM_GE_WEAPONS];
	f32 pos[NUM_GE_WEAPONS][3];
	u32 determiners[NUM_GE_WEAPONS];
	u16 nameids[NUM_GE_WEAPONS];
	char names[NUM_GE_WEAPONS][GEGUNS_NAMELEN + 2];
	u32 envitems;
	s32 built;
	struct weapon defs[NUM_GE_WEAPONS];
	struct aibotweaponpreference prefs[NUM_GE_WEAPONS];
};

static struct gegunset *g_GunSets[GEGUNS_SETS];
static s32 g_GunSetNoFile; // mod dirs known to have none, a bit each
static s32 g_GunSetNumDirs = -1;

// GoldenEye's own definitions while a hack's are in
static struct weapon g_GunSetGeDefs[NUM_GE_WEAPONS];
static struct aibotweaponpreference g_GunSetGePrefs[NUM_GE_WEAPONS];

static u32 gegunsBe32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static f32 gegunsBeF32(const u8 *p)
{
	const u32 v = gegunsBe32(p);
	f32 f;

	memcpy(&f, &v, sizeof(f));

	return f;
}

/** A gunWeaponStat row (gun.h's WeaponStats, big-endian) as gegunstats.h has GoldenEye's. */
static void gegunsStatFromRow(struct gegunstat *st, const u8 *r)
{
	memset(st, 0, sizeof(*st));
	st->muzzle = gegunsBeF32(r + 0);
	st->ammotype = (u8)gegunsBe32(r + 28);
	st->magsize = (s16)(r[32] << 8 | r[33]);
	st->autorate = r[34];
	st->singlerate = r[35];
	st->penetration = r[36];
	st->damage = gegunsBeF32(r + 44);
	st->spread = gegunsBeF32(r + 48);
	st->zoom = gegunsBeF32(r + 52);
	st->sway = gegunsBeF32(r + 64);

	for (s32 k = 0; k < 4; k++) {
		st->recoilspeed[k] = (s8)r[68 + k];
	}

	st->recoilback = gegunsBeF32(r + 72);
	st->recoilup = gegunsBeF32(r + 76);
	st->boltback = gegunsBeF32(r + 80);
	st->noise.minradius = gegunsBeF32(r + 84);
	st->noise.maxradius = gegunsBeF32(r + 88);
	st->noise.incradius = gegunsBeF32(r + 92);
	st->noise.decbasespeed = gegunsBeF32(r + 96);
	st->noise.decremspeed = gegunsBeF32(r + 100);
	st->impactforce = gegunsBeF32(r + 104);
	st->bitflags = gegunsBe32(r + 108);
}

/**
 * "An" where a name is said with a vowel first: a vowel but U ("a UZI"), or
 * a letter said as itself before a number or another capital ("an M1
 * Garand", "an MP40", "an S&W Model 36").
 */
static u32 gegunsDeterminer(const char *name)
{
	const char c = name[0];

	if (strchr("AEIO", c)) {
		return WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN;
	}

	if (strchr("FHLMNRSX", c) && c && name[1] && ((name[1] >= '0' && name[1] <= '9')
			|| (name[1] >= 'A' && name[1] <= 'Z') || name[1] == '&')) {
		return WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN;
	}

	return 0;
}

/** Mod dir `moddir`'s gun set, read the first time it is asked for; NULL where it has none. */
static struct gegunset *gegunsSetAt(s32 moddir)
{
	char path[FS_MAXPATH + 1];
	struct gegunset *set;
	u32 len = 0;
	u8 *d;
	u32 count;
	u32 hdr;
	s32 slot = -1;

	if (moddir < 0 || !fsGetModDirAt(moddir)) {
		return NULL;
	}

	// a change of mods mounts them again, in whatever order
	if (g_GunSetNumDirs != fsGetNumModDirs()) {
		g_GunSetNumDirs = fsGetNumModDirs();
		g_GunSetNoFile = 0;

		for (s32 i = 0; i < GEGUNS_SETS; i++) {
			if (g_GunSets[i] && g_GunSetDir != g_GunSets[i]->moddir) {
				g_GunSets[i]->moddir = -1;
			}
		}
	}

	for (s32 i = 0; i < GEGUNS_SETS; i++) {
		if (g_GunSets[i] && g_GunSets[i]->moddir == moddir) {
			return g_GunSets[i];
		}

		// one the mounts left behind is free again: the defs in use are
		// copies, and the set they came from is the one in (g_GunSetDir)
		if (slot < 0 && (!g_GunSets[i] || g_GunSets[i]->moddir < 0)) {
			slot = i;
		}
	}

	if ((moddir < 32 && (g_GunSetNoFile & (1 << moddir))) || slot < 0) {
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/menu/geguns.bin", fsGetModDirAt(moddir));
	d = fsFileSize(path) > 0 ? fsFileLoad(path, &len) : NULL;

	// GGN2 carries the items drawn under the envmap light after the count
	hdr = d && len >= 12 && !memcmp(d, "GGN2", 4) ? 12 : 8;

	if (!d || len < 8 || (memcmp(d, "GGN1", 4) && hdr != 12) || len < hdr + (count = gegunsBe32(d + 4)) * GEGUNS_ROW) {
		if (d) {
			sysLogPrintf(LOG_WARNING, "geguns: %s is not a gun set", path);
		}

		sysMemFree(d);

		if (moddir < 32) {
			g_GunSetNoFile |= 1 << moddir;
		}

		return NULL;
	}

	set = g_GunSets[slot];

	if (!set) {
		set = g_GunSets[slot] = calloc(1, sizeof(*set));

		if (!set) {
			sysMemFree(d);
			return NULL;
		}
	} else {
		memset(set, 0, sizeof(*set));
	}

	set->moddir = moddir;
	set->envitems = hdr == 12 ? gegunsBe32(d + 8) : GEGUNS_ENVMAP_ITEMS;

	// GoldenEye's, where the hack has no gun on a weapon: its gadgets, and
	// whatever of GoldenEye's it has no row for
	memcpy(set->stats, geStats, sizeof(set->stats));
	memcpy(set->sounds, geShootSounds, sizeof(set->sounds));
	memcpy(set->soundrates, geShootSoundRates, sizeof(set->soundrates));
	memcpy(set->items, geItems, sizeof(set->items));
	memcpy(set->props, geChrProps, sizeof(set->props));
	memcpy(set->pos, geOwnPos, sizeof(set->pos));
	memcpy(set->determiners, geDeterminers, sizeof(set->determiners));
	memcpy(set->nameids, geNameIds, sizeof(set->nameids));

	for (u32 k = 0; k < count; k++) {
		const u8 *row = d + hdr + GEGUNS_ROW * k;
		const u8 *stat = row + 4 + GEGUNS_NAMELEN;
		const s32 i = row[0];

		if (i >= NUM_GE_WEAPONS) {
			continue;
		}

		// the detonator's item only: it is no gun and keeps GoldenEye's rows
		set->items[i] = row[1];

		if (!GE_GUN_INDEX(i)) {
			continue;
		}

		gegunsStatFromRow(&set->stats[i], stat);
		set->sounds[i] = (u8)(stat[38] << 8 | stat[39]);
		set->soundrates[i] = stat[37];
		set->props[i] = (s16)(row[2] << 8 | row[3]);
		set->pos[i][0] = gegunsBeF32(stat + 4);
		set->pos[i][1] = gegunsBeF32(stat + 8);
		set->pos[i][2] = gegunsBeF32(stat + 12);

		memcpy(set->names[i], row + 4, GEGUNS_NAMELEN);
		set->names[i][GEGUNS_NAMELEN] = '\0';

		if (set->names[i][0]) {
			set->determiners[i] = gegunsDeterminer(set->names[i]);
			strcat(set->names[i], "\n"); // as the game's own names end
			set->nameids[i] = langAddPortText(set->names[i]);
		}
	}

	sysMemFree(d);
	sysLogPrintf(LOG_NOTE, "geguns: %u guns of a ROM hack's own, from %s", count, path);

	return set;
}

/** Points geguns.c's tables at `set`'s, or GoldenEye's own for NULL. */
static void gegunsUseTables(struct gegunset *set)
{
	stats = set ? set->stats : geStats;
	shootsounds = set ? set->sounds : geShootSounds;
	shootsoundrates = set ? set->soundrates : geShootSoundRates;
	items = set ? set->items : geItems;
	chrProps = set ? set->props : geChrProps;
	ownpos = set ? (const f32 (*)[3])set->pos : geOwnPos;
	determiners = set ? set->determiners : geDeterminers;
	nameids = set ? set->nameids : geNameIds;
	envitems = set ? set->envitems : GEGUNS_ENVMAP_ITEMS;
}

/**
 * The guns of the stage about to load: a ROM hack's own on a stage of its
 * own (its menu/geguns.bin), GoldenEye's everywhere else. Before the setup's
 * props are made, which hold them.
 */
void gegunsStageSet(s32 stagenum)
{
	struct gegunset *set = NULL;
	s32 dir = -1;

	if (modloaderStageIsRemake(stagenum) && !modloaderStageIsGexPlus(stagenum)) {
		set = gegunsSetAt(modloaderGetStageModDirIndex(stagenum));
		dir = set ? set->moddir : -1;
	}

	if (dir == g_GunSetDir) {
		gegunsExtraRowsRefresh();
		return;
	}

	// the watch laser is put in again by its own stage (gegadgets.c)
	gegunsSetWatchLaser(0);

	if (g_GunSetDir < 0) {
		memcpy(g_GunSetGeDefs, g_GeWeaponDefs, sizeof(g_GunSetGeDefs));
		memcpy(g_GunSetGePrefs, g_GeAibotWeaponPreferences, sizeof(g_GunSetGePrefs));
	}

	gegunsUseTables(set);

	if (!set) {
		memcpy(g_GeWeaponDefs, g_GunSetGeDefs, sizeof(g_GunSetGeDefs));
		memcpy(g_GeAibotWeaponPreferences, g_GunSetGePrefs, sizeof(g_GunSetGePrefs));
	} else if (set->built) {
		memcpy(g_GeWeaponDefs, set->defs, sizeof(set->defs));
		memcpy(g_GeAibotWeaponPreferences, set->prefs, sizeof(set->prefs));
	} else {
		// GoldenEye's own first, so the gadgets and what the hack has no row
		// for are as they are everywhere else
		memcpy(g_GeWeaponDefs, g_GunSetGeDefs, sizeof(g_GunSetGeDefs));

		for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
			const struct weapon *host = g_Weapons[g_GeWeaponHosts[i]];

			if (!GE_GUN_INDEX(i)) {
				continue;
			}

			gegunsBuild(i, host, host);
			gegunsOwnTrigger(i);
			gegunsFireRate(i);
			gegunsOwnThrown(i);
			gegunsBotPrefs(i);
			gegunsNameThrow(i);
		}

		memcpy(set->defs, g_GeWeaponDefs, sizeof(set->defs));
		memcpy(set->prefs, g_GeAibotWeaponPreferences, sizeof(set->prefs));
		set->built = 1;
	}

	g_GunSetDir = dir;

	// and each one's model in the hand, the set's own (gegunsFindConverted()):
	// gebean.c's for GoldenEye's guns, and here for the hack's own pistols,
	// which the release has nothing of
	gebeanGunsStageRefresh();

	for (s32 w = WEAPON_GE_EXTRA1; w <= WEAPON_GE_EXTRA4; w++) {
		const s32 i = w - WEAPON_GE_FIRST;
		const u16 own = gegunsOwnModel(i);

		g_GeWeaponDefs[i].hi_model = own ? own : gegunsModelFile(i);
		gegunsSetOwnModelInUse(i, own != 0);
		g_GeWeaponDefs[i].flags &= ~WEAPONFLAG_HASHANDS;

		if (!own) {
			g_GeWeaponDefs[i].flags |= gegunsHandsFlag(i);
		}
	}

	gegunsExtraRowsRefresh();

	sysLogPrintf(LOG_NOTE, "geguns: %s guns", set ? "a ROM hack's own" : "GoldenEye's own");
}

/** Whether a ROM hack's own gun set is in (gegunsStageSet()): its stage is loaded. */
s32 gegunsHackSetIn(void)
{
	return g_GunSetDir >= 0;
}

/**
 * The gun set the menus name GoldenEye's guns by: a ROM hack's own while its
 * mode is chosen (g_GexPlusVariant: GE Plus's Combat Simulator and folder in
 * its mode), over whatever stage the menus are on - between its matches that
 * is Perfect Dark's, in GoldenEye's set (gegunsStageSet()); the stage's
 * otherwise. NULL for GoldenEye's own.
 */
static struct gegunset *gegunsMenuSet(void)
{
	if (g_GexPlusMode && g_GexPlusVariant) {
		return gegunsSetAt(modloaderGexPlusVariantDirIndex());
	}

	return g_GunSetDir >= 0 ? gegunsSetAt(g_GunSetDir) : NULL;
}

/**
 * A gun's name in the Combat Simulator's menus (mpGetWeaponLabel()): its
 * menus' gun set's (gegunsMenuSet()), so a ROM hack's weapon sets and slots
 * list its AK47 where GoldenEye's set has the KF7 Soviet; 0 where that is
 * GoldenEye's, whose names are the weapons' own.
 */
u16 gegunsMenuNameId(s32 weaponnum)
{
	const s32 i = weaponnum - WEAPON_GE_FIRST;
	const struct gegunset *set;

	if (!GE_GUN_INDEX(i) || !(set = gegunsMenuSet())) {
		return 0;
	}

	return set->nameids[i];
}

/**
 * The Combat Simulator's rows of a ROM hack's own pistols
 * (MPWEAPON_GE_EXTRA1): shown where a hack's gun set names them - its stage,
 * or its mode's menus (gegunsMenuSet()) - under the game's own weapon list,
 * hidden everywhere else, where each is a "Pistol" of nobody's. Goldfinger
 * 64's own weapon sets hand out its Luger, P38 and two Smith & Wessons.
 */
void gegunsExtraRowsRefresh(void)
{
	const struct gegunset *set = modDataMpWeaponsImported() ? NULL : gegunsMenuSet();

	for (s32 k = 0; k < NUM_GE_EXTRA; k++) {
		const s32 i = WEAPON_GE_EXTRA1 + k - WEAPON_GE_FIRST;

		g_MpWeapons[MPWEAPON_GE_EXTRA1 + k].unlockfeature = set && set->names[i][0] ? 0 : MPFEATURE_NEVER;
	}
}

#endif

/**
 * GoldenEye's Moonraker marks what it hits and sounds the surface as well as
 * its laser ricochet (chrprop.c's shot: an impact for every gun but the
 * watch laser, ITEM_WATCHLASER; gunfire.c's recall_joy2_hits_edit_flag():
 * RICO_LASER and the texture's own hit). Its host, the Laser, leaves no hole
 * (WEAPONFLAG2_NOWALLHIT) and sounds only its own hit (FINDINGS row 14). The
 * watch laser on the Moonraker's number (Train) stays as it is.
 */
s32 gegunsMoonrakerMarks(s32 weaponnum)
{
	return weaponnum == WEAPON_GE_MOONRAKER && !gegunsWatchLaserInstalled();
}

/**
 * Where GoldenEye's grenade and mines leave the hand, in camera space: where
 * it holds them, gunWeaponStat's PosX/Y/Z (ownpos), which gunfire.c turns
 * into the hand's world matrix every frame (throw_item_pos_related) and
 * gun.c throws from. Nothing of them is drawn, so the host's muzzle - the
 * mine's down at the hand, 80 units under the eye - was no place of
 * GoldenEye's: they came down a dozen ticks after the throw where the
 * cartridge's take 22, so a proximity mine lay within its own 250 of Bond
 * (FINDINGS row 10). 1 when it is one of those, with the position.
 */
s32 gegunsThrowOrigin(s32 weaponnum, struct coord *campos)
{
	if (!gegunsThrowsAsGoldenEye(weaponnum)) {
		return 0;
	}

	campos->x = ownpos[weaponnum - WEAPON_GE_FIRST][0];
	campos->y = ownpos[weaponnum - WEAPON_GE_FIRST][1];
	campos->z = ownpos[weaponnum - WEAPON_GE_FIRST][2];

	return 1;
}

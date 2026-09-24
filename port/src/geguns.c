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
#include "lang.h"
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
	u8 ammotype;  // GoldenEye's AMMOTYPES index
	u32 bitflags; // GoldenEye's WEAPONSTATBITFLAG_* word; 0 is a gadget's, which has no row
};

#define GUNSTAT(weapon, source, mag, autorate, singlerate, pen, dmg, spread, impact, loudmin, loudmax, pershot, lineartime, scaledtime, \
		speed0, speed1, speed2, speed3, back, up, bolt, sway, zoom, muzzle, ammotype, bitflags) \
	[weapon - WEAPON_GE_FIRST] = { mag, autorate, singlerate, pen, dmg, spread, impact, { loudmin, loudmax, pershot, lineartime, scaledtime }, \
		{ speed0, speed1, speed2, speed3 }, back, up, bolt, sway, zoom, muzzle, ammotype, bitflags }

// GoldenEye's WEAPONSTATBITFLAG_* bits a definition is built from (bondconstants.h)
#define GESTATFLAG_HAS_AUTO_AIM           0x00000008
#define GESTATFLAG_ONLY_1_HANDED          0x00000100
#define GESTATFLAG_HIDE_FIRST_PERSON_MENU 0x00004000
#define GESTATFLAG_USE_HOLD_TIME          0x00020000

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

/**
 * How GoldenEye words each gun's name when it is picked up: "an AR33 Assault
 * Rifle", "the Golden Gun". A copy took its host's, which gave the covert modem,
 * the plastique and the GoldenEye key the ECM mine's "an".
 */
static const u32 determiners[NUM_GE_WEAPONS] = {
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST]        = WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN,
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST]       = WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN,
	[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST] = WEAPONFLAG_DETERMINER_S_AN | WEAPONFLAG_DETERMINER_F_AN,
	[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST]   = WEAPONFLAG_DETERMINER_S_THE | WEAPONFLAG_DETERMINER_F_THE,
};

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
 * golden bullet has no row of Perfect Dark's to become and stays the magnum's,
 * and 0 - GoldenEye's AMMO_NONE, or a gadget with no row - keeps the host's.
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
	[13] = AMMOTYPE_MAGNUM,      // GGUN
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
static u16 nameids[NUM_GE_WEAPONS];

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

/** Function f of gun i: its kind and scripts the model's, its numbers GoldenEye's. */
static struct weaponfunc *gegunsFunc(s32 i, s32 f, const struct weaponfunc *src, struct noisesettings *noise)
{
	const struct gegunstat *stat = &stats[i];
	const s32 hasrow = stat->bitflags != 0;
	struct weaponfunc *fn = calloc(1, gegunsFuncSize(src->type));

	if (!fn) {
		return (struct weaponfunc *)src;
	}

	fn->type = src->type;
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

		// 0xff is GoldenEye's "no rate", not a time
		shoot->recoverytime60 = hasrow && stat->singlerate != 0xff ? (s8)stat->singlerate : from->recoverytime60;

		// The Shotgun works its model's pump after every shot, and the timing
		// is the pump's: GoldenEye's early refire would cut it short
		// (GoldenEye X times its own pump too, 0 and 68)
		if (hasrow && WEAPON_GE_FIRST + i != WEAPON_GE_SHOTGUN) {
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

		if (src->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
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
		if (hasrow && f == 0 && (stat->autorate != 0xff) != (src->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC)) {
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
		thr->damage = from->damage;
		break;
	}
	case INVENTORYFUNCTYPE_MELEE: {
		const struct weaponfunc_melee *from = (const struct weaponfunc_melee *)src;
		struct weaponfunc_melee *melee = (struct weaponfunc_melee *)fn;

		// GoldenEye's knife is a 3 against Perfect Dark's 2
		melee->damage = hasrow ? stat->damage : from->damage;
		melee->range = from->range;
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
 *   ITEM_THROWKNIFE), so its throw is its first. The remote mine's detonator
 *   stays - GoldenEye detonates with A and B, which the port's does too. It was
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
 *
 * And, as GoldenEye X has them: the Cougar's pistol whip does not leave its
 * victim dizzy, and a knife is thrown where it is aimed and not where auto-aim
 * would put it. (Lock-on and "an"/"the" are gegunsBuild()'s, from
 * GoldenEye's own data.)
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
		case WEAPON_GE_GRENADELAUNCHER:
		case WEAPON_GE_ROCKETLAUNCHER:
			if ((func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
				func->flags |= FUNCFLAG_NOMUZZLEFLASH;
			}
			break;
		case WEAPON_GE_AUTOSHOTGUN:
			func->fire_animation = NULL;
			break;
		case WEAPON_GE_COUGARMAGNUM:
			if (func->type == INVENTORYFUNCTYPE_MELEE) {
				func->flags &= ~FUNCFLAG_MAKEDIZZY;
			}
			break;
		case WEAPON_GE_HUNTINGKNIFE:
		case WEAPON_GE_THROWINGKNIFE:
			if (func->type == INVENTORYFUNCTYPE_THROW) {
				func->flags |= FUNCFLAG_NOAUTOAIM;
			}
			break;
		}
	}
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

static void gegunsFindConverted(void)
{
	const s32 numdirs = fsGetNumModDirs();
	const s32 generation = romdataFilesGeneration();
	s32 found = 0;
	s32 dir = -1;

	if (convertedSearched == numdirs && convertedGeneration == generation) {
		return;
	}

	convertedSearched = numdirs;
	convertedGeneration = generation;

	// the mount's index may have moved too: searched again from nothing
	memset(convertedModel, 0, sizeof(convertedModel));

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

/**
 * GoldenEye's own model of this gun in a hand - its PROP_CHR* prop, which the
 * conversion's `models` block numbers MODEL_REMAKE_FIRST + prop - wherever the
 * gun is drawn in GoldenEye's own look and the stage has the block loaded; -1
 * otherwise.
 *
 * The model state a GoldenEye gun otherwise has (MODEL_GE_FIRST) is an alias
 * of its host's pickup that only the release's HD pickup is drawn over, so in
 * the N64 look every gun that shares a host drew the same host model: the
 * silenced PP7 Bond starts Dam with was the plain PP7 in his hand through the
 * opening swirl (the silenced D5K the same). The guards on the same level have
 * held GoldenEye's own props all along, from the converted setup.
 *
 * The thrown ones are left out: what is thrown is built from this model too,
 * and theirs are Perfect Dark's projectiles.
 */
s32 gegunsOwnPropModel(s32 weaponnum)
{
	// player.c's getPropForHeldItem(), by weapon
	static const s16 props[NUM_GE_GUNS] = {
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
		[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = 196, // PROP_CHRGRENADE
	};
	const s32 index = weaponnum - WEAPON_GE_FIRST;
	s32 prop;

	if (!gegunsOwnModelInUse(weaponnum) || index >= NUM_GE_GUNS) {
		return -1;
	}

	prop = props[index];

	return prop > 0 && g_ModelStates[MODEL_REMAKE_FIRST + prop].fileid ? MODEL_REMAKE_FIRST + prop : -1;
}

/**
 * The rocket a GoldenEye launcher holds and fires, where its own model is in
 * the hand: GoldenEye's PROP_CHRROCKET (currentPlayerCreateRocket()), whose
 * origin is its tail at the barrel's end. The host's Perfect Dark rocket has
 * its origin further along its length, so it sat inside GoldenEye's tube with
 * nothing showing at the mouth. `fallback` otherwise.
 */
s32 gegunsOwnRocketModel(s32 weaponnum, s32 fallback)
{
	const s32 model = MODEL_REMAKE_FIRST + 202; // PROP_CHRROCKET

	if (weaponnum != WEAPON_GE_ROCKETLAUNCHER || !gegunsOwnModelInUse(weaponnum)
			|| !g_ModelStates[model].fileid) {
		return fallback;
	}

	return model;
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

	switch (weaponnum) {
	case WEAPON_GE_ROCKETLAUNCHER:  model = MODEL_REMAKE_FIRST + 202; break;
	case WEAPON_GE_GRENADELAUNCHER: model = MODEL_REMAKE_FIRST + 203; break;
	default: return fallback;
	}

	return g_ModelStates[model].fileid ? model : fallback;
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

	return weaponnum >= WEAPON_GE_FIRST && weaponnum < WEAPON_GE_FIRST + NUM_GE_GUNS;
}

s32 gegunsEnemyRocketModel(void)
{
	const s32 model = playermgrGetModelOfWeapon(WEAPON_GE_ROCKETLAUNCHER);

	return model >= 0 ? model : MODEL_CHRDYROCKET;
}

/**
 * GoldenEye draws its gadgets in silence (gunfire.c's equip sound leaves out
 * the covert modem, the plastique, the GoldenEye key, the camera, the watch
 * magnet and the tank's shells); their hosts, the ECM mine and the Data
 * Uplink, play the mine's.
 */
s32 gegunsEquipSilent(s32 weaponnum)
{
	return weaponnum >= WEAPON_GE_COVERTMODEM && weaponnum <= WEAPON_GE_TANKSHELLS;
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

#endif

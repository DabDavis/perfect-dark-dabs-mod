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
 * DY357 - so it takes its host's first-person model, animations, ammo,
 * functions and sounds, and every test of a weapon by number asks about the
 * host (weaponHost()). What is its own: the number, so it is held, dropped and
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

		// Until gebean.c has the release's pickup to point it at, the host's
		if (hostmodel >= 0 && hostmodel < MODEL_GE_FIRST) {
			g_ModelStates[MODEL_GE_FIRST + i].fileid = g_ModelStates[hostmodel].fileid;
			g_ModelStates[MODEL_GE_FIRST + i].scale = g_ModelStates[hostmodel].scale;
		}
	}
}

#endif

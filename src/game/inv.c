#include <ultra64.h>
#include "constants.h"
#include "game/modunlocks.h"
#include "game/modoptions.h"
#ifndef PLATFORM_N64
#include "gegadgets.h"
#include "gexfront.h"
#include "gexplus.h"
#include "geguns.h"
#endif
#include "game/cheats.h"
#include "game/bondgun.h"
#include "game/game_0b0fd0.h"
#include "game/inv.h"
#ifndef PLATFORM_N64
#include "getank.h"
#endif
#include "game/training.h"
#include "game/lang.h"
#include "bss.h"
#include "lib/main.h"
#include "data.h"
#include "types.h"

void invClear(void)
{
	s32 i;

	for (i = 0; i < g_Vars.currentplayer->equipmaxitems; i++) {
		g_Vars.currentplayer->equipment[i].type = -1;
	}

	g_Vars.currentplayer->weapons = NULL;
	g_Vars.currentplayer->equipcuritem = 0;
}

/**
 * Where a weapon stands in the inventory's order: its number, except that
 * GoldenEye's own inventory sorts by its item numbers (bondinv.c), which put
 * the hunting and throwing knives straight after the fist and the watch's
 * detonator and the tank's shells straight after the remote mine, where ours
 * number them after the guns and the gadgets (F3 20261002-225441: "the
 * throwing knife comes after the KF7"). Perfect Dark's own order is its
 * numbers, unchanged.
 */
static s32 invOrderKey(s32 weaponnum)
{
#ifndef PLATFORM_N64
	switch (weaponnum) {
	case WEAPON_GE_HUNTINGKNIFE:  return WEAPON_UNARMED * 4 + 1;
	case WEAPON_GE_THROWINGKNIFE: return WEAPON_UNARMED * 4 + 2;
	case WEAPON_GE_DETONATOR:     return WEAPON_GE_REMOTEMINE * 4 + 1;
	case WEAPON_GE_TANKSHELLS:    return WEAPON_GE_REMOTEMINE * 4 + 2;
	}
#endif

	return weaponnum * 4;
}

/**
 * Sorts subject into its correct position in the inventory list.
 *
 * Subject is expected to initially be at the head of the list. It works by
 * swapping the subject with the item to its right as many times as needed.
 */
void invSortItem(struct invitem *subject)
{
	struct invitem *candidate;
	s32 subjweapon1 = -1;
	s32 subjweapon2 = -1;
	s32 candweapon1;
	s32 candweapon2;

	// Prepare subject's properties for comparisons
	if (subject->type == INVITEMTYPE_WEAP) {
		subjweapon1 = invOrderKey(subject->type_weap.weapon1);
	} else if (subject->type == INVITEMTYPE_DUAL) {
		subjweapon1 = invOrderKey(subject->type_dual.weapon1);
		subjweapon2 = invOrderKey(subject->type_dual.weapon2);
	} else if (subject->type == INVITEMTYPE_PROP) {
		subjweapon1 = 2000 * 4;
	}

	candidate = subject->next;

	while (g_Vars.currentplayer->weapons != subject->next) {
		// Prepare candidate's properties for comparisons
		candweapon1 = -1;
		candweapon2 = -1;

		if (subject->next->type == INVITEMTYPE_WEAP) {
			candweapon1 = invOrderKey(subject->next->type_weap.weapon1);
		} else if (subject->next->type == INVITEMTYPE_DUAL) {
			candweapon1 = invOrderKey(subject->next->type_dual.weapon1);
			candweapon2 = invOrderKey(subject->next->type_dual.weapon2);
		} else if (subject->next->type == INVITEMTYPE_PROP) {
			candweapon1 = 1000 * 4;
		}

		// If the candidate should sort ahead of subject
		// then subject is in the desired position.
		if (candweapon1 >= subjweapon1 &&
				(subjweapon1 != candweapon1 || subjweapon2 <= candweapon2)) {
			return;
		}

		// If there's only two items in the list then there's no point swapping
		// them. Just set the list head to the candidate.
		if (candidate->next == subject) {
			g_Vars.currentplayer->weapons = candidate;
		} else {
			// Swap subject with candidate
			subject->next = candidate->next;
			candidate->prev = subject->prev;
			subject->prev = candidate;
			candidate->next = subject;
			subject->next->prev = subject;
			candidate->prev->next = candidate;

			// Set new list head if subject was the head
			if (subject == g_Vars.currentplayer->weapons) {
				g_Vars.currentplayer->weapons = candidate;
			}
		}

		candidate = subject->next;
	}
}

void invInsertItem(struct invitem *item)
{
	if (item->type == INVITEMTYPE_PROP) {
		struct prop *prop = item->type_prop.prop;

		if (prop && prop->obj) {
			struct textoverride *override = invGetTextOverrideForObj(prop->obj);
			bool setflag = true;

			if (override) {
				if (override->weapon >= WEAPON_UNARMED && override->weapon <= WEAPON_NECKLACE) {
					setflag = false;
				}
				if (weaponHost(override->weapon) == WEAPON_MPSHIELD) {
					setflag = false;
				}
				if (weaponHost(override->weapon) == WEAPON_SUICIDEPILL) {
					setflag = false;
				}
				if (weaponHost(override->weapon) == WEAPON_BRIEFCASE2) {
					setflag = false;
				}
			}

			if (setflag && prop->type == PROPTYPE_OBJ) {
				struct defaultobj *obj = prop->obj;
				obj->flags2 |= OBJFLAG2_INVHIDDEN;
			}
		}
	}

	// Place item at head of weapons list
	if (g_Vars.currentplayer->weapons) {
		item->next = g_Vars.currentplayer->weapons;
		item->prev = g_Vars.currentplayer->weapons->prev;
		item->next->prev = item;
		item->prev->next = item;
	} else {
		item->next = item;
		item->prev = item;
	}

	g_Vars.currentplayer->weapons = item;

	invSortItem(item);
	invCalculateCurrentIndex();
}

void invRemoveItem(struct invitem *item)
{
	struct invitem *next = item->next;
	struct invitem *prev = item->prev;

	if (g_Vars.currentplayer->weapons == item) {
		if (item == item->next) {
			g_Vars.currentplayer->weapons = NULL;
		} else {
			g_Vars.currentplayer->weapons = item->next;
		}
	}

	next->prev = prev;
	prev->next = next;
	item->type = -1;

	invCalculateCurrentIndex();
}

struct invitem *invFindUnusedSlot(void)
{
	s32 i;

	for (i = 0; i < g_Vars.currentplayer->equipmaxitems; i++) {
		if (g_Vars.currentplayer->equipment[i].type == -1) {
			return &g_Vars.currentplayer->equipment[i];
		}
	}

	return NULL;
}

void invSetAllGuns(bool enable)
{
	s32 weaponnum;

	g_Vars.currentplayer->equipallguns = enable;
	invCalculateCurrentIndex();
	weaponnum = invGetWeaponNumByIndex(g_Vars.currentplayer->equipcuritem);
	bgunEquipWeapon(weaponnum);
}

bool invHasAllGuns(void)
{
	return g_Vars.currentplayer->equipallguns;
}

struct invitem *invFindSingleWeapon(s32 weaponnum)
{
	struct invitem *first = g_Vars.currentplayer->weapons;
	struct invitem *item = first;

	while (item) {
		if (item->type == INVITEMTYPE_WEAP && item->type_weap.weapon1 == weaponnum) {
			return item;
		}

		item = item->next;

		if (item == first) {
			break;
		}
	}

	return NULL;
}

bool invHasSingleWeaponExcAllGuns(s32 weaponnum)
{
	return invFindSingleWeapon(weaponnum) != NULL;
}

struct invitem *invFindDoubleWeapon(s32 weapon1, s32 weapon2)
{
	struct invitem *first = g_Vars.currentplayer->weapons;
	struct invitem *item = first;

	while (item) {
		if (item->type == INVITEMTYPE_DUAL
				&& item->type_dual.weapon1 == weapon1
				&& item->type_dual.weapon2 == weapon2) {
			return item;
		}

		item = item->next;

		if (item == first) {
			break;
		}
	}

	return NULL;
}

bool invHasDoubleWeaponExcAllGuns(s32 weapon1, s32 weapon2)
{
	return invFindDoubleWeapon(weapon1, weapon2) != NULL;
}

bool invHasSingleWeaponOrProp(s32 weaponnum)
{
	struct invitem *item = g_Vars.currentplayer->weapons;

	while (item) {
		if (item->type == INVITEMTYPE_WEAP) {
			if (weaponnum == item->type_weap.weapon1) {
				return true;
			}
		} else if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;

			if (prop && prop->type == PROPTYPE_WEAPON) {
				struct defaultobj *obj = prop->obj;

				if (obj && obj->type == OBJTYPE_WEAPON) {
					struct weaponobj *weapon = (struct weaponobj *)prop->obj;

					if (weapon->weaponnum == weaponnum) {
						return true;
					}
				}
			}
		}

		item = item->next;

		if (item == g_Vars.currentplayer->weapons) {
			break;
		}
	}

	return false;
}

s32 invAddOneIfCantHaveSlayer(s32 index)
{
	if (g_ModUnlocks & MODUNLOCK_ALLGUNS) {
		return index;
	}

	if (mainGetStageNum());

	if (mainGetStageNum() != STAGE_ATTACKSHIP
			&& mainGetStageNum() != STAGE_SKEDARRUINS
			&& index >= WEAPON_SLAYER) {
		index++;
	}

#if (VERSION >= VERSION_JPN_FINAL) && defined(PLATFORM_N64)
	if (index >= 26) {
		index++;
	}
#endif

	return index;
}

s32 currentStageForbidsSlayer(void)
{
	bool value = VERSION >= VERSION_JPN_FINAL ? 1 : 0;

	if (g_ModUnlocks & MODUNLOCK_ALLGUNS) {
		return 0;
	}

	if (mainGetStageNum() != STAGE_ATTACKSHIP && mainGetStageNum() != STAGE_SKEDARRUINS) {
		value++;
	}

	return value;
}

bool invCanHaveAllGunsWeapon(s32 weaponnum)
{
	bool canhave = true;

	if (g_ModUnlocks & MODUNLOCK_ALLGUNS) {
		return true;
	}

#if (VERSION == VERSION_JPN_FINAL) && defined(PLATFORM_N64)
	if (weaponHost(weaponnum) == WEAPON_COMBATKNIFE) {
		canhave = false;
	}
#endif

	if (weaponHost(weaponnum) == WEAPON_SLAYER) {
		canhave = false;
	}

	// @bug: The stage conditions need an OR. This condition can never pass.
	if ((mainGetStageNum() == STAGE_ATTACKSHIP && mainGetStageNum() == STAGE_SKEDARRUINS)
			&& weaponHost(weaponnum) == WEAPON_SLAYER) {
		canhave = true;
	}

	return canhave;
}

/**
 * The All Guns cheat's list. Perfect Dark's is every weapon up to the
 * Psychosis Gun, by number. In a level GE Plus started it is GoldenEye's
 * instead (bondinv.c's equipallguns: ITEM_FIST up to ITEM_TANKSHELLS, in
 * GoldenEye's item order) - the guns, the knives, the explosives and the
 * detonator it gives with them, but not its tank's shells, which nothing can
 * fire outside the tank, nor the Silver and Gold PP7s, the watch laser and
 * the taser that the port has no weapon for; followed by Perfect Dark's own
 * list only when the player has asked for Perfect Dark's guns in GE Plus
 * (Mod.GePlusPdGuns). F3 20260929-025554.
 */
#ifndef PLATFORM_N64
static const u8 g_GeAllGuns[] = {
	WEAPON_UNARMED,
	WEAPON_GE_HUNTINGKNIFE,
	WEAPON_GE_THROWINGKNIFE,
	WEAPON_GE_PP7,
	WEAPON_GE_PP7SILENCED,
	WEAPON_GE_DD44,
	WEAPON_GE_KLOBB,
	WEAPON_GE_KF7SOVIET,
	WEAPON_GE_ZMG,
	WEAPON_GE_D5K,
	WEAPON_GE_D5KSILENCED,
	WEAPON_GE_PHANTOM,
	WEAPON_GE_AR33,
	WEAPON_GE_RCP90,
	WEAPON_GE_SHOTGUN,
	WEAPON_GE_AUTOSHOTGUN,
	WEAPON_GE_SNIPERRIFLE,
	WEAPON_GE_COUGARMAGNUM,
	WEAPON_GE_GOLDENGUN,
	WEAPON_GE_MOONRAKER,
	WEAPON_GE_GRENADELAUNCHER,
	WEAPON_GE_ROCKETLAUNCHER,
	WEAPON_GE_GRENADE,
	WEAPON_GE_TIMEDMINE,
	WEAPON_GE_PROXIMITYMINE,
	WEAPON_GE_REMOTEMINE,
	WEAPON_GE_DETONATOR,
};

#define NUM_GE_ALLGUNS ((s32)(sizeof(g_GeAllGuns) / sizeof(g_GeAllGuns[0])))

static bool invAllGunsAreGe(void)
{
	return gexFrontIsInside() != 0;
}

/**
 * Whether the All Guns cheat holds `weaponnum` as a pair as well as single.
 * In a level GE Plus started that is GoldenEye's own rule, its gun's
 * CAN_DUAL_WIELD (bondinv.c's bondinvItemAvailableForHand() and the cycles,
 * one player only): every gun but the grenade and the mines, the Moonraker
 * among them, which its Perfect Dark host never pairs (F3 20260930-190327,
 * "you can't dual wield the laser").
 */
static bool invAllGunsPairs(s32 weaponnum)
{
	if (invAllGunsAreGe() && WEAPON_IS_GE(weaponnum) && PLAYERCOUNT() == 1 && gegunsAllGunsPairs(weaponnum)) {
		return true;
	}

	return weaponHasFlag(weaponnum, WEAPONFLAG_DUALWIELD);
}

/**
 * GoldenEye's list as the region's cartridge has it. The Japanese one leaves
 * the hunting knife out of everything the cheat hands over (bondinv.c's
 * bondinvItemAvailable(), bondinvItemAvailableForHand(), the cycles and the
 * watch's list, all under j_text_trigger), so it is never cycled to, listed or
 * held as a pair. A knife actually picked up is still the player's: the
 * cartridge only tests it where the cheat is asked.
 */
static s32 invGeAllGunsCount(void)
{
	return gexFrontIsJapanese() ? NUM_GE_ALLGUNS - 1 : NUM_GE_ALLGUNS;
}

static s32 invGeAllGunsAt(s32 index)
{
	// the knife is the list's second, straight after Unarmed
	if (gexFrontIsJapanese() && index >= 1) {
		index++;
	}

	return g_GeAllGuns[index];
}

/** Perfect Dark's own list after GoldenEye's, with its Unarmed left out. */
static s32 invAllGunsPdCount(void)
{
	return gexPlusGetPdGuns() ? WEAPON_PSYCHOSISGUN - currentStageForbidsSlayer() - 1 : 0;
}
#endif

/** How many weapons the All Guns cheat lists. */
s32 invAllGunsCount(void)
{
#ifndef PLATFORM_N64
	if (invAllGunsAreGe()) {
		return invGeAllGunsCount() + invAllGunsPdCount();
	}
#endif

	return WEAPON_PSYCHOSISGUN - currentStageForbidsSlayer();
}

/** The weapon at `index` (from 0) of the All Guns cheat's list. */
s32 invAllGunsWeaponAt(s32 index)
{
#ifndef PLATFORM_N64
	if (invAllGunsAreGe()) {
		if (index < 0) {
			return WEAPON_NONE;
		}

		if (index < invGeAllGunsCount()) {
			return invGeAllGunsAt(index);
		}

		index -= invGeAllGunsCount();

		return index < invAllGunsPdCount() ? invAddOneIfCantHaveSlayer(index + 2) : WEAPON_NONE;
	}
#endif

	return invAddOneIfCantHaveSlayer(index + 1);
}

/** Where `weaponnum` stands in the All Guns cheat's list, or -1. */
static s32 invAllGunsIndexOf(s32 weaponnum)
{
	const s32 count = invAllGunsCount();

	for (s32 i = 0; i < count; i++) {
		if (invAllGunsWeaponAt(i) == weaponnum) {
			return i;
		}
	}

	return -1;
}

/** Whether the All Guns cheat gives `weaponnum`. */
bool invAllGunsGives(s32 weaponnum)
{
#ifndef PLATFORM_N64
	if (invAllGunsAreGe()) {
		if (weaponnum <= WEAPON_NONE) {
			return false;
		}

		if (WEAPON_IS_GE(weaponnum)) {
			return invAllGunsIndexOf(weaponnum) >= 0;
		}

		return weaponnum == WEAPON_UNARMED
			|| (invAllGunsPdCount() > 0 && weaponnum <= WEAPON_PSYCHOSISGUN && invCanHaveAllGunsWeapon(weaponnum));
	}
#endif

	return weaponnum && weaponnum <= WEAPON_PSYCHOSISGUN && invCanHaveAllGunsWeapon(weaponnum);
}

#ifndef PLATFORM_N64
/**
 * The next (`dir` 1) or previous (-1) weapon from `weaponnum` in the All Guns
 * cheat's list, in the list's own order and round its end, skipping the empty
 * ones when `needammo` asks; `weaponnum` itself when there is no other.
 */
static s32 invAllGunsStep(s32 weaponnum, s32 dir, bool needammo)
{
	const s32 listed = invAllGunsCount();
	// the tank's shells, which the list leaves out, while he drives: one
	// more stop after the list's end, as in GoldenEye's cycle (F3
	// 20260929-062621)
	const bool shells = invHasSingleWeaponExcAllGuns(WEAPON_GE_TANKSHELLS);
	const s32 count = listed + (shells ? 1 : 0);
	s32 index = weaponnum == WEAPON_GE_TANKSHELLS && shells ? listed : invAllGunsIndexOf(weaponnum);

	if (index < 0) {
		index = dir > 0 ? -1 : count;
	}

	for (s32 i = 0; i < count; i++) {
		s32 candidate;

		index = (index + dir + count) % count;
		candidate = index == listed ? WEAPON_GE_TANKSHELLS : invAllGunsWeaponAt(index);

		if (candidate == weaponnum) {
			break;
		}

		if (!needammo || bgun0f0a1a10(candidate)) {
			return candidate;
		}
	}

	return weaponnum;
}
#endif

/** Whether an inventory item of `weaponnum` is left out while the list shows it. */
static bool invAllGunsHides(s32 weaponnum)
{
#ifndef PLATFORM_N64
	if (invAllGunsAreGe()) {
		return invAllGunsGives(weaponnum);
	}
#endif

	return weaponnum <= WEAPON_PSYCHOSISGUN;
}

bool invHasSingleWeaponIncAllGuns(s32 weaponnum)
{
	if (g_Vars.currentplayer->equipallguns && invAllGunsGives(weaponnum)) {
		return true;
	}

	return invHasSingleWeaponExcAllGuns(weaponnum);
}

bool invHasDoubleWeaponIncAllGuns(s32 weapon1, s32 weapon2)
{
	if (weapon2 == WEAPON_NONE) {
		return true;
	}

	if (g_Vars.currentplayer->equipallguns &&
			weapon1 == weapon2 &&
#ifndef PLATFORM_N64
			invAllGunsPairs(weapon1) &&
#else
			weaponHasFlag(weapon1, WEAPONFLAG_DUALWIELD) &&
#endif
			invAllGunsGives(weapon1)) {
		return true;
	}

	return invHasDoubleWeaponExcAllGuns(weapon1, weapon2);
}

bool invGiveSingleWeapon(s32 weaponnum)
{
	frSetWeaponFound(weaponnum);

	if (invHasSingleWeaponExcAllGuns(weaponnum) == 0) {
		struct invitem *item;

		if (g_Vars.currentplayer->equipallguns && invAllGunsGives(weaponnum)) {
			return false;
		}

		item = invFindUnusedSlot();

		if (item) {
			item->type = INVITEMTYPE_WEAP;
			item->type_weap.weapon1 = weaponnum;
			item->type_weap.pickuppad = -1;
			invInsertItem(item);
		}

#ifndef PLATFORM_N64
		// GoldenEye gives the watch's detonator with the remote mines,
		// however they come (propobj.c's add_ammo_to_inventory() and
		// propPickupByPlayer(): ITEM_REMOTEMINE and ITEM_TRIGGER together)
		if (weaponnum == WEAPON_GE_REMOTEMINE) {
			invGiveSingleWeapon(WEAPON_GE_DETONATOR);
		}
#endif

		return true;
	}

	return false;
}

#ifndef PLATFORM_N64
static bool invGiveDoubleWeaponFlags(s32 weapon1, s32 weapon2, bool anyflags);

bool invGiveDoubleWeapon(s32 weapon1, s32 weapon2)
{
	return invGiveDoubleWeaponFlags(weapon1, weapon2, false);
}

/**
 * The second of a guard's two linked guns (setup's PROPFLAG_IS_DOUBLE, or a
 * guard handed a pair) makes a pair in the hands. GoldenEye pairs any linked
 * two (bondinv.c's bondinvAddDoublesInvItem() tests no flag; CAN_DUAL_WIELD
 * is read only by the all-guns cheat), so one of its guns does here too
 * whatever its Perfect Dark host allows: Aztec's guards with two Moonrakers
 * left the second one lying, the Laser being single-handed (F3
 * 20260928-214632). Two unlinked guns still pair only as they did.
 */
static bool invGiveDoubleWeaponLinked(s32 weapon1, s32 weapon2)
{
	return invGiveDoubleWeaponFlags(weapon1, weapon2, WEAPON_IS_GE(weapon1) && WEAPON_IS_GE(weapon2));
}

static bool invGiveDoubleWeaponFlags(s32 weapon1, s32 weapon2, bool anyflags)
{
	if (invHasDoubleWeaponExcAllGuns(weapon1, weapon2) == 0) {
		if (anyflags || weaponHasFlag(weapon1, WEAPONFLAG_DUALWIELD)) {
#else
bool invGiveDoubleWeapon(s32 weapon1, s32 weapon2)
{
	if (invHasDoubleWeaponExcAllGuns(weapon1, weapon2) == 0) {
		if (weaponHasFlag(weapon1, WEAPONFLAG_DUALWIELD)) {
#endif
			struct invitem *item = invFindUnusedSlot();

			if (item) {
				item->type = INVITEMTYPE_DUAL;
				item->type_dual.weapon1 = weapon1;
				item->type_dual.weapon2 = weapon2;
				invInsertItem(item);
			}

			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}

	return false;
}

void invRemoveItemByNum(s32 weaponnum)
{
	if (g_Vars.currentplayer->weapons) {
		// Begin iterating from the second item in the list. This is required
		// because the item might be removed from the list when iterating it,
		// and it needs to determine when the end of the list has been reached.
		struct invitem *item = g_Vars.currentplayer->weapons->next;

		while (true) {
			// Have to preload this because item->next shouldn't be trusted
			// after calling invRemoveItem()
			struct invitem *next = item->next;

			if (item->type == INVITEMTYPE_PROP) {
				struct prop *prop = item->type_prop.prop;
				struct textoverride *override = invGetTextOverrideForObj(prop->obj);

				if (override && override->weapon == weaponnum) {
					invRemoveItem(item);
				}
			} else if (item->type == INVITEMTYPE_WEAP) {
				if (item->type_weap.weapon1 == weaponnum) {
					invRemoveItem(item);
				}
			} else if (item->type == INVITEMTYPE_DUAL) {
				if (item->type_dual.weapon1 == weaponnum || item->type_dual.weapon2 == weaponnum) {
					invRemoveItem(item);
				}
			}

			if (item == g_Vars.currentplayer->weapons || !g_Vars.currentplayer->weapons) {
				break;
			}

			item = next;
		}
	}
}

bool invGiveProp(struct prop *prop)
{
	struct invitem *item;

	// Don't add duplicate night vision to inventory
	// (night vision is already there when using perfect darkness)
	// Note that this check doesn't work on Investigation because it uses the
	// IR specs model. See bug note in Investigation's setup file (setupear.c).
	if (cheatIsActive(CHEAT_PERFECTDARKNESS)
			&& prop->type == PROPTYPE_OBJ
			&& prop->obj
			&& prop->obj->modelnum == MODEL_CHRNIGHTSIGHT) {
		return true;
	}

	item = invFindUnusedSlot();

	if (item) {
		item->type = INVITEMTYPE_PROP;
		item->type_prop.prop = prop;
		invInsertItem(item);
	}

	return true;
}

void invRemoveProp(struct prop *prop)
{
	if (g_Vars.currentplayer->weapons) {
		struct invitem *item = g_Vars.currentplayer->weapons->next;

		while (true) {
			struct invitem *next = item->next;

			if (item->type == INVITEMTYPE_PROP && item->type_prop.prop == prop) {
				invRemoveItem(item);
			}

			if (item == g_Vars.currentplayer->weapons || !g_Vars.currentplayer->weapons) {
				break;
			}

			item = next;
		}
	}
}

#ifndef PLATFORM_N64
/**
 * Akimbo's house rule, solo: a second of a weapon the player holds one of
 * makes the pair, whatever the mission - Perfect Dark's own, where the
 * pickup's "second makes a pair" branch is multiplayer only, and GoldenEye's,
 * which pairs only what its setup pairs (link records, a guard's two guns).
 * Any weapon Akimbo lets into a hand, GoldenEye's grenade and mines included;
 * not GoldenEye's gadgets past the guns - the watch's detonator, the key, the
 * camera, the tank's shells - which are no weapon to hold two of. The
 * multiplayer branch below already pairs anything under Akimbo, a second
 * copy from another pad.
 */
bool invAkimboPairsPickup(s32 weaponnum)
{
	return !g_Vars.normmplayerisrunning
		&& modIsAkimboForPlayers()
		&& modCanAkimbo(weaponnum)
		&& !gegadgetsIsGadget(weaponnum)
		&& invHasSingleWeaponExcAllGuns(weaponnum)
		&& !invHasDoubleWeaponExcAllGuns(weaponnum, weaponnum);
}
#endif

s32 invGiveWeaponsByProp(struct prop *prop)
{
	s32 numgiven = 0;

	if (prop->type == PROPTYPE_WEAPON) {
		struct defaultobj *obj = prop->obj;
		struct weaponobj *weapon;
		struct weaponobj *otherweapon;
		s32 weaponnum;
		s32 otherweaponnum;
#ifndef PLATFORM_N64
		bool akimbopair;
#endif

		if (obj->type == OBJTYPE_WEAPON) {
			weapon = prop->weapon;
			weaponnum = weapon->weaponnum;
			otherweaponnum;

#ifndef PLATFORM_N64
			// always allow picking up a second gun if dual wield cheat is on
			if (!g_Vars.normmplayerisrunning && cheatIsActive(CHEAT_DUALWIELDALLGUNS) && !gegunsNeverPairs(weaponnum)) {
				if (invHasSingleWeaponExcAllGuns(weaponnum) && !invHasDoubleWeaponExcAllGuns(weaponnum, weaponnum)) {
					if (invGiveDoubleWeapon(weaponnum, weaponnum)) {
						return 2;
					}
				}
			}
#endif

#ifndef PLATFORM_N64
			// asked before the single below is given, which it would count
			akimbopair = invAkimboPairsPickup(weaponnum);
#endif

			if (cheatIsActive(CHEAT_PERFECTDARKNESS) && weaponHost(weaponnum) == WEAPON_NIGHTVISION) {
				return 1;
			}

			if (invGiveSingleWeapon(weaponnum)) {
				numgiven = 1;
			}

			if (g_Vars.normmplayerisrunning
					&& weaponHasFlag(weaponnum, WEAPONFLAG_DUALWIELD)
					&& !invHasDoubleWeaponExcAllGuns(weaponnum, weaponnum)) {
				struct invitem *invitem = invFindSingleWeapon(weaponnum);

				if (invitem) {
					if (invitem->type_weap.pickuppad < 0) {
						if (obj->pad >= 0) {
							invitem->type_weap.pickuppad = obj->pad;
						}
					} else if (obj->pad >= 0 && invitem->type_weap.pickuppad != obj->pad) {
						if (invGiveDoubleWeapon(weaponnum, weaponnum)) {
							numgiven = 2;
						} else {
							numgiven = 0;
						}
					}
				}
			}

			otherweapon = weapon->dualweapon;

			if (otherweapon) {
				if (weapon->base.flags & OBJFLAG_WEAPON_LEFTHANDED) {
					numgiven = invHasDoubleWeaponExcAllGuns(otherweapon->weaponnum, weaponnum) == 0;
				} else {
					numgiven = invHasDoubleWeaponExcAllGuns(weaponnum, otherweapon->weaponnum) == 0;
				}

				weapon->dualweapon->dualweaponnum = weaponnum;
				weapon->dualweapon->dualweapon = NULL;
				weapon->dualweapon = NULL;
			} else if (weapon->dualweaponnum != 0xff) {
#ifndef PLATFORM_N64
				if (weapon->base.flags & OBJFLAG_WEAPON_LEFTHANDED) {
					if (invGiveDoubleWeaponLinked(weapon->dualweaponnum, weaponnum)) {
						numgiven = 2;
					} else {
						numgiven = 0;
					}
				} else {
					if (invGiveDoubleWeaponLinked(weaponnum, weapon->dualweaponnum)) {
#else
				if (weapon->base.flags & OBJFLAG_WEAPON_LEFTHANDED) {
					if (invGiveDoubleWeapon(weapon->dualweaponnum, weaponnum)) {
						numgiven = 2;
					} else {
						numgiven = 0;
					}
				} else {
					if (invGiveDoubleWeapon(weaponnum, weapon->dualweaponnum)) {
#endif
						numgiven = 2;
					} else {
						numgiven = 0;
					}
				}
			}

#ifndef PLATFORM_N64
			// after the link's bookkeeping above, so that its other gun is
			// never left pointing at this one
			if (akimbopair && numgiven != 2 && invGiveDoubleWeapon(weaponnum, weaponnum)) {
				numgiven = 2;
			}
#endif
		}
	}

	return numgiven;
}

void invChooseCycleForwardWeapon(s32 *ptr1, s32 *ptr2, bool arg2)
{
	s32 weapon1 = *ptr1;
	s32 weapon2 = *ptr2;

	if (g_Vars.currentplayer->equipallguns) {
		s32 candidate = *ptr1;

#ifndef PLATFORM_N64
		if (invAllGunsPairs(*ptr1) && *ptr2 != *ptr1
#else
		if (weaponHasFlag(*ptr1, WEAPONFLAG_DUALWIELD) && *ptr2 != *ptr1
#endif
#ifndef PLATFORM_N64
				// the tank's driver has no left hand (bgunTickSwitch2()): the
				// pair is refused and the cycle would ask for it for ever
				&& !geTankIsDriving()
#endif
				) {
			// Switching to dual from single
			weapon1 = *ptr1;
			weapon2 = *ptr1;
#ifndef PLATFORM_N64
		} else if (invAllGunsAreGe()) {
			weapon1 = invAllGunsStep(weapon1, 1, arg2);
			weapon2 = WEAPON_NONE;
#endif
		} else {
			// Find next weapon
			do {
				candidate = (candidate + 1) % NUM_CYCLEABLE_WEAPONS;

				if (candidate == WEAPON_NONE) {
					candidate = (candidate + 1) % NUM_CYCLEABLE_WEAPONS;
				}

				if ((!arg2 || bgun0f0a1a10(candidate)) && invCanHaveAllGunsWeapon(candidate)) {
					weapon1 = candidate;
					weapon2 = WEAPON_NONE;
					break;
				}
			} while (candidate != weapon1);
		}
	} else {
		struct invitem *item = g_Vars.currentplayer->weapons;

		while (item) {
			if (item->type == INVITEMTYPE_WEAP) {
				if (INV_CYCLEABLE(item->type_weap.weapon1) && invOrderKey(item->type_weap.weapon1) > invOrderKey(weapon1)) {
					if (!arg2 || bgun0f0a1a10(item->type_weap.weapon1)) {
						weapon1 = item->type_weap.weapon1;
						weapon2 = WEAPON_NONE;
						break;
					}
				}
			} else if (item->type == INVITEMTYPE_DUAL) {
				if (invOrderKey(item->type_dual.weapon1) > invOrderKey(weapon1)
						|| (weapon1 == item->type_dual.weapon1 && invOrderKey(item->type_dual.weapon2) > invOrderKey(weapon2))) {
					if (!arg2 || bgun0f0a1a10(item->type_dual.weapon1) || bgun0f0a1a10(item->type_dual.weapon2)) {
						weapon1 = item->type_dual.weapon1;
						weapon2 = item->type_dual.weapon2;
						break;
					}
				}
			}

			item = item->next;

			if (item == g_Vars.currentplayer->weapons) {
				if (arg2) {
					break;
				}

				weapon1 = -1;
				weapon2 = -1;
			}
		}
	}

	*ptr1 = weapon1;
	*ptr2 = weapon2;
}

void invChooseCycleBackWeapon(s32 *ptr1, s32 *ptr2, bool arg2)
{
	s32 weapon1 = *ptr1;
	s32 weapon2 = *ptr2;

	if (g_Vars.currentplayer->equipallguns) {
		s32 candidate = *ptr1;

#ifndef PLATFORM_N64
		if (invAllGunsPairs(weapon1) && weapon1 == weapon2) {
#else
		if (weaponHasFlag(weapon1, WEAPONFLAG_DUALWIELD) && weapon1 == weapon2) {
#endif
			// Switching from dual to single
			weapon1 = candidate;
			weapon2 = WEAPON_NONE;
#ifndef PLATFORM_N64
		} else if (invAllGunsAreGe()) {
			candidate = invAllGunsStep(weapon1, -1, arg2);
			weapon1 = candidate;
			weapon2 = invAllGunsPairs(candidate) && !geTankIsDriving() ? candidate : WEAPON_NONE;
#endif
		} else {
			// Find prev weapon
			do {
				candidate = (candidate + NUM_CYCLEABLE_WEAPONS - 1) % NUM_CYCLEABLE_WEAPONS;

				if (candidate == WEAPON_NONE) {
					candidate = (candidate + NUM_CYCLEABLE_WEAPONS - 1) % NUM_CYCLEABLE_WEAPONS;
				}
			} while ((arg2 && !bgun0f0a1a10(candidate)) || !invCanHaveAllGunsWeapon(candidate));

			if (weaponHasFlag(candidate, WEAPONFLAG_DUALWIELD)) {
				weapon1 = candidate;
				weapon2 = candidate;
			} else {
				weapon1 = candidate;
				weapon2 = WEAPON_NONE;
			}
		}
	} else if (g_Vars.currentplayer->weapons != NULL) {
		struct invitem *item = g_Vars.currentplayer->weapons->prev;

		while (true) {
			if (item->type == INVITEMTYPE_WEAP) {
				if (INV_CYCLEABLE(item->type_weap.weapon1)
						&& (invOrderKey(item->type_weap.weapon1) < invOrderKey(weapon1) || (weapon1 == item->type_weap.weapon1 && weapon2 > 0))) {
					if (!arg2 || bgun0f0a1a10(item->type_weap.weapon1)) {
						weapon1 = item->type_weap.weapon1;
						weapon2 = WEAPON_NONE;
						break;
					}
				}
			} else if (item->type == INVITEMTYPE_DUAL) {
				if (invOrderKey(item->type_dual.weapon1) < invOrderKey(weapon1)
						|| (weapon1 == item->type_dual.weapon1 && invOrderKey(item->type_dual.weapon2) < invOrderKey(weapon2))) {
					if (!arg2 || bgun0f0a1a10(item->type_dual.weapon1) || bgun0f0a1a10(item->type_dual.weapon2)) {
						weapon1 = item->type_dual.weapon1;
						weapon2 = item->type_dual.weapon2;
						break;
					}
				}
			}

			if (item == g_Vars.currentplayer->weapons) {
				if (arg2) {
					break;
				}

				weapon1 = 1000;
				weapon2 = 1000;
			}

			item = item->prev;
		}
	}

	*ptr1 = weapon1;
	*ptr2 = weapon2;
}

bool invHasKeyFlags(u32 wantkeyflags)
{
	u32 heldkeyflags = 0;
	struct invitem *item = g_Vars.currentplayer->weapons;

	while (item) {
		if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;

			if (prop && prop->type == PROPTYPE_OBJ) {
				struct defaultobj *obj = prop->obj;

				if (obj && obj->type == OBJTYPE_KEY) {
					struct keyobj *key = (struct keyobj *)prop->obj;

					heldkeyflags |= key->keyflags;

					if ((wantkeyflags & heldkeyflags) == wantkeyflags) {
						return true;
					}
				}
			}
		}

		item = item->next;

		if (item == g_Vars.currentplayer->weapons) {
			break;
		}
	}

	return false;
}

bool func0f11283c(void)
{
	return false;
}

bool invHasBriefcase(void)
{
	if (g_Vars.currentplayer->isdead == false) {
		return invHasSingleWeaponExcAllGuns(WEAPON_BRIEFCASE2);
	}

	return false;
}

bool invHasDataUplink(void)
{
	if (g_Vars.currentplayer->isdead == false) {
		return invHasSingleWeaponExcAllGuns(WEAPON_DATAUPLINK);
	}

	return false;
}

bool func0f1128c4(void)
{
	return false;
}

bool invHasProp(struct prop *prop)
{
	struct invitem *item = g_Vars.currentplayer->weapons;
	struct prop *child;

	while (item) {
		if (item->type == INVITEMTYPE_PROP && item->type_prop.prop == prop) {
			return true;
		}

		item = item->next;

		if (item == g_Vars.currentplayer->weapons) {
			break;
		}
	}

	child = g_Vars.currentplayer->prop->child;

	while (child) {
		if (child == prop) {
			return true;
		}

		child = child->next;
	}

	return false;
}

s32 invGetCount(void)
{
	s32 numitems = 0;
	struct invitem *item;

	if (g_Vars.currentplayer->equipallguns) {
		numitems = invAllGunsCount();
	}

	item = g_Vars.currentplayer->weapons;

	while (item) {
		if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;

			if (prop) {
				struct defaultobj *obj = prop->obj;

				if (obj) {
					if (prop->type == PROPTYPE_WEAPON) {
						if (obj->hidden & OBJHFLAG_HASTEXTOVERRIDE) {
							numitems++;
						}
					} else if (prop->type == PROPTYPE_OBJ) {
						if ((obj->flags2 & OBJFLAG2_INVHIDDEN) == 0) {
							numitems++;
						}
					}
				}
			}
		} else if (item->type == INVITEMTYPE_WEAP) {
			if (g_Vars.currentplayer->equipallguns == false
					|| !invAllGunsHides(item->type_weap.weapon1)) {
				numitems++;
			}
		}

		item = item->next;

		if (item == g_Vars.currentplayer->weapons) {
			break;
		}
	}

	return numitems;
}

struct invitem *invGetItemByIndex(s32 index)
{
	struct invitem *item;

	if (g_Vars.currentplayer->equipallguns) {
		if (index < invAllGunsCount()) {
			return NULL;
		}

		index -= invAllGunsCount();
	}

	item = g_Vars.currentplayer->weapons;

	while (item) {
		if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;

			if (prop) {
				struct defaultobj *obj = prop->obj;

				if (obj) {
					if (prop->type == PROPTYPE_WEAPON) {
						if (obj->hidden & OBJHFLAG_HASTEXTOVERRIDE) {
							if (index == 0) {
								return item;
							}
							index--;
						}
					} else if (prop->type == PROPTYPE_OBJ) {
						if ((obj->flags2 & OBJFLAG2_INVHIDDEN) == 0) {
							if (index == 0) {
								return item;
							}
							index--;
						}
					}
				}
			}
		} else if (item->type == INVITEMTYPE_WEAP) {
			if (g_Vars.currentplayer->equipallguns == false
					|| !invAllGunsHides(item->type_weap.weapon1)) {
				if (index == 0) {
					return item;
				}
				index--;
			}
		}

		item = item->next;

		if (item == g_Vars.currentplayer->weapons) {
			break;
		}
	}

	return NULL;
}

struct textoverride *invGetTextOverrideForObj(struct defaultobj *obj)
{
	struct textoverride *override = g_Vars.textoverrides;

	while (override) {
		if (override->obj == obj) {
			return override;
		}

		override = override->next;
	}

	return NULL;
}

struct textoverride *invGetTextOverrideForWeapon(s32 weaponnum)
{
	struct textoverride *override = g_Vars.textoverrides;

	while (override) {
		if (override->objoffset == 0 && override->weapon == weaponnum) {
			return override;
		}

		override = override->next;
	}

	return NULL;
}

s32 invGetWeaponNumByIndex(s32 index)
{
	struct invitem *item = invGetItemByIndex(index);

	if (item) {
		if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;
			struct textoverride *override = invGetTextOverrideForObj(prop->obj);

			if (override) {
				return override->weapon;
			}
		} else if (item->type == INVITEMTYPE_WEAP) {
			return item->type_weap.weapon1;
		}
	} else if (g_Vars.currentplayer->equipallguns) {
		if (index < invAllGunsCount()) {
			return invAllGunsWeaponAt(index);
		}
	}

	return 0;
}

u16 invGetNameIdByIndex(s32 index)
{
	struct invitem *item = invGetItemByIndex(index);
	s32 weaponnum = 0;
	struct textoverride *override;

	if (item) {
		if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;
			override = invGetTextOverrideForObj(prop->obj);

			if (override) {
				if (override->inventorytext) {
					return override->inventorytext;
				}

				weaponnum = override->weapon;
			}
		} else if (item->type == INVITEMTYPE_WEAP) {
			weaponnum = item->type_weap.weapon1;
			override = invGetTextOverrideForWeapon(weaponnum);

			if (override && override->inventorytext) {
				return override->inventorytext;
			}
		}
	} else {
		if (g_Vars.currentplayer->equipallguns) {
			if (index < invAllGunsCount()) {
				return bgunGetNameId(invAllGunsWeaponAt(index));
			}
		}
	}

	return bgunGetNameId(weaponnum);
}

char *invGetNameByIndex(s32 index)
{
	return langGet(invGetNameIdByIndex(index));
}

char *invGetShortNameByIndex(s32 index)
{
	struct invitem *item = invGetItemByIndex(index);
	s32 weaponnum = 0;
	struct textoverride *override;

	if (item) {
		if (item->type == INVITEMTYPE_PROP) {
			struct prop *prop = item->type_prop.prop;
			override = invGetTextOverrideForObj(prop->obj);

			if (override) {
#if VERSION < VERSION_JPN_FINAL
				if (override->inventorytext) {
					return langGet(override->inventorytext);
				}
#endif

				weaponnum = override->weapon;
			}
		} else if (item->type == INVITEMTYPE_WEAP) {
			weaponnum = item->type_weap.weapon1;
#if VERSION < VERSION_JPN_FINAL
			override = invGetTextOverrideForWeapon(weaponnum);

			if (override && override->inventorytext) {
				return langGet(override->inventorytext);
			}
#endif
		}
	} else if (g_Vars.currentplayer->equipallguns) {
		if (index < invAllGunsCount()) {
			return bgunGetShortName(invAllGunsWeaponAt(index));
		}
	}

	return bgunGetShortName(weaponnum);
}

void invInsertTextOverride(struct textoverride *override)
{
	override->next = g_Vars.textoverrides;
	g_Vars.textoverrides = override;
}

u32 invGetCurrentIndex(void)
{
	return g_Vars.currentplayer->equipcuritem;
}

void invSetCurrentIndex(u32 item)
{
	g_Vars.currentplayer->equipcuritem = item;
}

void invCalculateCurrentIndex(void)
{
	s32 curweaponnum = bgunGetWeaponNum(HAND_RIGHT);
	s32 i;

	g_Vars.currentplayer->equipcuritem = 0;

	for (i = 0; i < invGetCount(); i++) {
		if (invGetWeaponNumByIndex(i) == curweaponnum) {
			g_Vars.currentplayer->equipcuritem = i;
			break;
		}
	}
}

char *invGetPickupTextByObj(struct defaultobj *obj)
{
	struct textoverride *override = invGetTextOverrideForObj(obj);

	if (override && override->pickuptext) {
		return langGet(override->pickuptext);
	}

	return NULL;
}

char *invGetPickupTextByWeaponNum(s32 weaponnum)
{
	struct textoverride *override = invGetTextOverrideForWeapon(weaponnum);

	if (override && override->pickuptext) {
		return langGet(override->pickuptext);
	}

	return NULL;
}

void invIncrementHeldTime(s32 weapon1, s32 weapon2)
{
	s32 leastusedtime;
	s32 leastusedindex;
	s32 i;

	if (!weaponHasFlag(weapon1, WEAPONFLAG_TRACKTIMEUSED)) {
		return;
	}

	leastusedtime = 0x7fffffff;
	leastusedindex = 0;

	if (!weaponHasFlag(weapon2, WEAPONFLAG_TRACKTIMEUSED)) {
		weapon2 = 0;
	}

	for (i = 0; i < ARRAYCOUNT(g_Vars.currentplayer->gunheldarr); i++) {
		s32 time = g_Vars.currentplayer->gunheldarr[i].totaltime240_60;

		if (time >= 0) {
			if (weapon1 == g_Vars.currentplayer->gunheldarr[i].weapon1 &&
					weapon2 == g_Vars.currentplayer->gunheldarr[i].weapon2) {
				g_Vars.currentplayer->gunheldarr[i].totaltime240_60 = time + g_Vars.lvupdate60;
				break;
			}

			if (time < leastusedtime) {
				leastusedtime = time;
				leastusedindex = i;
			}
		} else {
			leastusedindex = i;
			i = ARRAYCOUNT(g_Vars.currentplayer->gunheldarr);
			break;
		}
	}

	if (i == ARRAYCOUNT(g_Vars.currentplayer->gunheldarr)) {
		g_Vars.currentplayer->gunheldarr[leastusedindex].totaltime240_60 = g_Vars.lvupdate60;
		g_Vars.currentplayer->gunheldarr[leastusedindex].weapon1 = weapon1;
		g_Vars.currentplayer->gunheldarr[leastusedindex].weapon2 = weapon2;
	}
}

void invGetWeaponOfChoice(s32 *weapon1, s32 *weapon2)
{
	s32 mosttime = -1;
	s32 i;

	*weapon1 = 0;
	*weapon2 = 0;

	for (i = 0; i < ARRAYCOUNT(g_Vars.currentplayer->gunheldarr); i++) {
		if (g_Vars.currentplayer->gunheldarr[i].totaltime240_60 >= 0
				&& g_Vars.currentplayer->gunheldarr[i].totaltime240_60 > mosttime) {
			mosttime = g_Vars.currentplayer->gunheldarr[i].totaltime240_60;
			*weapon1 = g_Vars.currentplayer->gunheldarr[i].weapon1;
			*weapon2 = g_Vars.currentplayer->gunheldarr[i].weapon2;
		}
	}
}

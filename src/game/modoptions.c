#include <ultra64.h>
#include "constants.h"
#include "game/modoptions.h"
#include "types.h"

/**
 * The defaults are the fork's own behaviour, because a build of this fork with
 * nothing in its config is one that has never been to the options menu, and the
 * things it added should be there to find. Start Armed is the exception: it is
 * stock Perfect Dark's, and stock has it off.
 */
struct modoptions g_ModOptions = {
	1,                        // jumpheight, the base height
	MODWHO_EVERYONE,          // jumpwho
	MODROLL_EVERYONE,         // roll
	true,                     // melee
	true,                     // flinch
	SPAWNWEAPON_OFF,          // spawnweapon
	THIRDPERSON_CAMDIST,      // camdist
	THIRDPERSON_CAMCLEARANCE, // camclearance
	THIRDPERSON_CAMMINDIST,   // cammindist
};

/**
 * 0 for off, otherwise the height multiplier.
 */
s32 modGetJumpHeight(void)
{
	s32 mult = g_ModOptions.jumpheight;

	if (mult < 0) {
		mult = 0;
	} else if (mult > JUMPHEIGHT_MAX) {
		mult = JUMPHEIGHT_MAX;
	}

	return mult;
}

bool modIsJumpEnabled(void)
{
	return modGetJumpHeight() != 0;
}

/**
 * Whether a simulant may jump. Players Only leaves the move to the human, which
 * is the setting for anyone who wants the mobility without simulants using it.
 */
bool modCanChrJump(void)
{
	return modIsJumpEnabled() && g_ModOptions.jumpwho == MODWHO_EVERYONE;
}

/**
 * Upward velocity a jump starts with, scaled for the chosen height.
 *
 * The apex is v * v / (2 * gravity), so height goes with the square of the
 * impulse: a jump twice as high wants sqrt(2) times the velocity, not twice.
 * These are those roots, written out rather than computed because this runs on
 * every jump.
 */
f32 modGetJumpImpulse(void)
{
	static const f32 scale[JUMPHEIGHT_MAX] = { 1.0f, 1.4142135f, 1.7320508f, 2.0f, 2.2360680f };
	s32 mult = modGetJumpHeight();

	if (mult < JUMPHEIGHT_MIN) {
		mult = JUMPHEIGHT_MIN;
	}

	return JUMP_IMPULSE * scale[mult - JUMPHEIGHT_MIN];
}

/**
 * How high that impulse reaches. Bounds how far below an airborne simulant its
 * collision cylinder may extend, so it has to scale with the setting too.
 */
f32 modGetJumpApex(void)
{
	s32 mult = modGetJumpHeight();

	if (mult < JUMPHEIGHT_MIN) {
		mult = JUMPHEIGHT_MIN;
	}

	return JUMP_APEX * mult;
}

bool modCanPlayerRoll(void)
{
	return g_ModOptions.roll != MODROLL_OFF;
}

bool modCanChrRoll(void)
{
	return g_ModOptions.roll == MODROLL_EVERYONE;
}

bool modIsMeleeComboEnabled(void)
{
	return g_ModOptions.melee != 0;
}

bool modIsFlinchEnabled(void)
{
	return g_ModOptions.flinch != 0;
}

/**
 * SPAWNWEAPON_OFF, or which weapon everyone spawns an arena match holding.
 */
s32 modGetSpawnWeapon(void)
{
	if (g_ModOptions.spawnweapon < SPAWNWEAPON_OFF || g_ModOptions.spawnweapon > SPAWNWEAPON_RANDOM) {
		return SPAWNWEAPON_OFF;
	}

	return g_ModOptions.spawnweapon;
}

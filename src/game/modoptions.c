#include <ultra64.h>
#include "constants.h"
#include "game/modoptions.h"
#ifndef PLATFORM_N64
#include "game/modghost.h"
#endif
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
	MODWHO_EVERYONE,          // spawnweaponwho
	THIRDPERSON_CAMDIST,      // camdist
	THIRDPERSON_CAMCLEARANCE, // camclearance
	THIRDPERSON_CAMMINDIST,   // cammindist
	MODBODIES_DEFAULT,        // bodies
	MODBODYTIME_OFF,          // bodytime
	64,                       // bodiesdrawn
	MODALARM_OFF,             // guardsalerted: like Start Armed, a choice, not a default
	MODALARM_GUARDS_DEFAULT,  // alertedguards
	MODALARM_SPEED_DEFAULT,   // guardspawnspeed
	true,                     // alarmsound
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

/**
 * Whether anybody may jump.
 *
 * Off inside a Ghost Trial, whoever asked for it in the options. A time trial
 * is runs measured against each other, and a jump is worth seconds on a route:
 * a board where some runs had one and some did not is a board that measures
 * settings rather than driving. Simulants and guards lose it too, through
 * modCanChrJump below, because a guard that vaults a railing is a different
 * obstacle than one that does not.
 */
bool modIsJumpEnabled(void)
{
#ifndef PLATFORM_N64
	if (modGhostTrialRulesApply()) {
		return false;
	}
#endif

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
#ifndef PLATFORM_N64
	if (modGhostTrialRulesApply()) {
		return false;
	}
#endif

	return g_ModOptions.roll != MODROLL_OFF;
}

bool modCanChrRoll(void)
{
#ifndef PLATFORM_N64
	if (modGhostTrialRulesApply()) {
		return false;
	}
#endif

	return g_ModOptions.roll == MODROLL_EVERYONE;
}

/**
 * Whether the punch and kick combo is available.
 *
 * Off in a trial, with jump and the roll. It is a move this fork added and it
 * is worth time on a route: a guard taken down in one exchange rather than
 * three is seconds, and a board where some runs had it and some did not is a
 * board that ranks settings.
 */
bool modIsMeleeComboEnabled(void)
{
#ifndef PLATFORM_N64
	if (modGhostTrialRulesApply()) {
		return false;
	}
#endif

	return g_ModOptions.melee != 0;
}

/**
 * Whether a body twitches where a shot landed.
 *
 * Off in a trial too, and this one is less obvious than the moves: it is not
 * something the player does, it is something a guard does, and a guard that
 * staggers where it was hit is a guard that shoots back later. That is the
 * same mission played against different opposition, which is the thing a time
 * trial is trying not to be.
 */
bool modIsFlinchEnabled(void)
{
#ifndef PLATFORM_N64
	if (modGhostTrialRulesApply()) {
		return false;
	}
#endif

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

/**
 * Whether a simulant spawns holding the weapon too. Players Only leaves the
 * arena's pickups as the simulants' only source, which is the setting for
 * anyone who wants the head start without handing it to eighty opponents.
 */
bool modCanChrSpawnArmed(void)
{
	return modGetSpawnWeapon() != SPAWNWEAPON_OFF
		&& g_ModOptions.spawnweaponwho == MODWHO_EVERYONE;
}

/**
 * How many bodies are left lying around, or MODBODIES_OFF.
 */
s32 modGetBodiesKept(void)
{
	if (g_ModOptions.bodies < MODBODIES_OFF) {
		return MODBODIES_OFF;
	}

	if (g_ModOptions.bodies > MODBODIES_MAX) {
		return MODBODIES_MAX;
	}

	return g_ModOptions.bodies;
}

/**
 * Seconds a body lies there before it fades, or MODBODYTIME_OFF for as long as
 * the cap will hold it.
 */
s32 modGetBodyTime(void)
{
	if (g_ModOptions.bodytime < MODBODYTIME_OFF) {
		return MODBODYTIME_OFF;
	}

	if (g_ModOptions.bodytime > MODBODYTIME_MAX) {
		return MODBODYTIME_MAX;
	}

	return g_ModOptions.bodytime;
}

/**
 * How many kept bodies may be drawn in one frame, or MODBODIESDRAWN_ALL.
 */
s32 modGetBodiesDrawn(void)
{
	if (g_ModOptions.bodiesdrawn < MODBODIESDRAWN_ALL) {
		return MODBODIESDRAWN_ALL;
	}

	if (g_ModOptions.bodiesdrawn > MODBODIESDRAWN_MAX) {
		return MODBODIESDRAWN_MAX;
	}

	return g_ModOptions.bodiesdrawn;
}

bool modKeepsBodies(void)
{
	return modGetBodiesKept() != MODBODIES_OFF;
}

/**
 * Whether the alarm is a permanent condition.
 *
 * Off in a Ghost Trial, with the rest of the fork's moves: a run made against
 * an endless stream of guards is a different route from one made against the
 * stage's own, and the board should not be ranking which was chosen.
 */
bool modIsGuardsAlertedOn(void)
{
#ifndef PLATFORM_N64
	if (modGhostTrialRulesApply()) {
		return false;
	}
#endif

	return g_ModOptions.guardsalerted != MODALARM_OFF;
}

/**
 * How many alerted guards may be up at once. Meaningful only while the
 * setting is on; the pools are sized for it at stage load either way.
 */
s32 modGetAlertedGuards(void)
{
	if (g_ModOptions.alertedguards < MODALARM_GUARDS_MIN) {
		return MODALARM_GUARDS_MIN;
	}

	if (g_ModOptions.alertedguards > MODALARM_GUARDS_MAX) {
		return MODALARM_GUARDS_MAX;
	}

	return g_ModOptions.alertedguards;
}

/**
 * How fast the reinforcements come, in guards per ten seconds.
 */
s32 modGetGuardSpawnSpeed(void)
{
	if (g_ModOptions.guardspawnspeed < MODALARM_SPEED_MIN) {
		return MODALARM_SPEED_MIN;
	}

	if (g_ModOptions.guardspawnspeed > MODALARM_SPEED_MAX) {
		return MODALARM_SPEED_MAX;
	}

	return g_ModOptions.guardspawnspeed;
}

/**
 * Whether the siren plays. Only meaningful while the alarm is on, and the
 * stock alarm - the one a guard raises - keeps its sound whatever this says:
 * that is thirty seconds, and the mission is telling the player something.
 */
bool modIsAlarmSoundEnabled(void)
{
	return g_ModOptions.alarmsound != 0;
}

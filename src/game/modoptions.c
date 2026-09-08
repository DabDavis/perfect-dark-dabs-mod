#include <ultra64.h>
#include "constants.h"
#include "game/game_0b0fd0.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#ifndef PLATFORM_N64
#include "game/modghost.h"
#include "video.h"
#include "bss.h"
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
	THIRDPERSON_CAMSIDE,      // camside: centred, like the view the fork shipped
	MODBODIES_DEFAULT,        // bodies
	MODBODYTIME_OFF,          // bodytime
	64,                       // bodiesdrawn
	MODALARM_OFF,             // guardsalerted: like Start Armed, a choice, not a default
	MODALARM_GUARDS_DEFAULT,  // alertedguards
	MODALARM_SPEED_DEFAULT,   // guardspawnspeed
	MODALARM_WEAPONS_STAGE,   // guardweapons
	MODAKIMBO_OFF,            // akimbo
	false,                    // akimbotriggers
	false,                    // explosionshake: the Video page's slider scales it when on
	false,                    // codaiming
	true,                     // codaimlock: what COD Style Aiming means until it is turned off
	true,                     // alarmsound
	true,                     // cleantext
	MODTILT_NORMAL,           // cameratilt
	false,                    // tiltinvert: the lean the head makes going with the step, not against it
	false,                    // tiltforward: the tilt shipped as a roll and a bob, so this is a choice
	true,                     // gunsway
	false,                    // randomizer: a way of playing rather than a setting, so off
	0,                        // randomseed: a fresh mission every time until one is chosen
	MODRANDOM_VERSION_DEFAULT, // randomversion: whatever this build deals with
	false,                    // randomendless: a way of playing rather than a setting, so off
	0,                        // endlessbest: nothing survived yet
	MODSMOOTH_OFF,            // modelsmoothing: a look rather than a fix, so a choice
	true,                     // modellod: stock's distance models
	true,                     // smoothtext: a fix, so on
	MODENHANCE_2X,            // enhancetextures: the cheapest of the three
	MODVIVID_LIGHT,           // vividcolours: the washed-out look was the complaint
	MODBLACK_LIGHT,           // blacklevel: and the pedestal is most of it
	false,                    // missionrespawn: like Start Armed, a choice, not a default
	MODLIVES_UNLIMITED,       // missionlives
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

	// The Institute is the hub the menus sit over, and the stock game never
	// raises an alarm there. With it on, troopers spawned all over the
	// building and shot at the player behind the Perfect Menu: the impacts
	// shook the view and rattled while the menu itself looked fine.
	if (g_Vars.stagenum == STAGE_CITRAINING) {
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
 * MODALARM_WEAPONS_STAGE or MODALARM_WEAPONS_RANDOM.
 */
s32 modGetGuardWeapons(void)
{
	return g_ModOptions.guardweapons == MODALARM_WEAPONS_RANDOM ? MODALARM_WEAPONS_RANDOM : MODALARM_WEAPONS_STAGE;
}

/**
 * Whether Start Armed hands a player or simulant two of the gun.
 */
bool modIsAkimboForPlayers(void)
{
	return g_ModOptions.akimbo == MODAKIMBO_EVERYONE || g_ModOptions.akimbo == MODAKIMBO_PLAYERSANDSIMS;
}

/**
 * Whether a Guards Alerted! guard is given two of its gun.
 */
bool modIsAkimboForGuards(void)
{
	return g_ModOptions.akimbo == MODAKIMBO_EVERYONE || g_ModOptions.akimbo == MODAKIMBO_GUARDS;
}

bool modIsAkimboTriggersOn(void)
{
	return g_ModOptions.akimbotriggers != 0;
}

/**
 * Whether an explosion shakes the screen. Off by default: eighty guards
 * with grenades is a screen that never stops moving, and the Video page's
 * Explosion Shake slider still scales it for anyone who turns it on.
 */
bool modIsExplosionShakeOn(void)
{
	return g_ModOptions.explosionshake != 0;
}

/**
 * COD Style Aiming: aim mode as a modern shooter has it. The crosshair
 * stays in the centre and the gun comes up to it with a little zoom, and
 * the player keeps moving, slower. Two guns come in to either side.
 */
bool modIsCodAimingOn(void)
{
	return g_ModOptions.codaiming != 0;
}

/**
 * Aim Lock, under COD Style Aiming: the crosshair is held in the centre
 * and the aim stick turns the view, as the mouse always does there. Off,
 * the stick moves the crosshair about the screen as it does in the
 * game's own aim mode, and the mouse follows Mouse Aim Lock. Nothing
 * without COD Style Aiming itself.
 */
bool modIsCodAimLockOn(void)
{
	return g_ModOptions.codaiming != 0 && g_ModOptions.codaimlock != 0;
}

/**
 * Camera Tilt, as a multiplier on the angles player.c leans the view by
 * and the height it bobs the eye. 0 is off; Normal is 1, the two degree
 * roll, with Light half of it and Heavy twice.
 */
f32 modGetCameraTiltScale(void)
{
	switch (g_ModOptions.cameratilt) {
	case MODTILT_LIGHT:
		return 0.5f;
	case MODTILT_NORMAL:
		return 1.0f;
	case MODTILT_HEAVY:
		return 2.0f;
	}

	return 0.0f;
}

/**
 * Invert Camera Tilt: whether the leans go the other way about - the roll
 * away from the sidestep, the lean away from the look, and the run tilt
 * back rather than forward. The bob is a straight lift and is left alone.
 * Nothing without Camera Tilt itself.
 */
bool modIsCameraTiltInverted(void)
{
	return g_ModOptions.cameratilt != MODTILT_OFF && g_ModOptions.tiltinvert != 0;
}

/**
 * Forward And Back Tilt: whether running pitches the view down into the run
 * and backing away pitches it up, on top of the lean the look gives.
 * Nothing without Camera Tilt itself.
 */
bool modIsForwardTiltOn(void)
{
	return g_ModOptions.cameratilt != MODTILT_OFF && g_ModOptions.tiltforward != 0;
}

/**
 * Gun Sway With Tilt, as a multiplier on the gun's step motion: stock's 1
 * when off or when Camera Tilt is, else one more than the tilt's own scale
 * - 1.5 at Light, 2 at Normal, 3 at Heavy.
 */
f32 modGetGunSwayScale(void)
{
	if (g_ModOptions.gunsway == 0) {
		return 1.0f;
	}

	return 1.0f + modGetCameraTiltScale();
}

/**
 * Increase Poly Models, as the side of the grid a lit triangle is drawn as:
 * 0 for off, else 2, 3 or 4 - four, nine or sixteen triangles for one. The
 * renderer keeps its own copy (gfx_model_smoothing_level, with the amount
 * below); videoSetModelSmoothing() keeps them together.
 */
s32 modGetModelSmoothingLevel(void)
{
	switch (g_ModOptions.modelsmoothing) {
	case MODSMOOTH_LIGHT:
		return 2;
	case MODSMOOTH_NORMAL:
		return 3;
	case MODSMOOTH_HEAVY:
		return 4;
	}

	return 0;
}

/**
 * How far toward the curved patch the new vertices are pulled, 0 to 1.
 */
f32 modGetModelSmoothingAmount(void)
{
	switch (g_ModOptions.modelsmoothing) {
	case MODSMOOTH_LIGHT:
		return 0.5f;
	case MODSMOOTH_NORMAL:
		return 0.75f;
	case MODSMOOTH_HEAVY:
		return 1.0f;
	}

	return 0.0f;
}

/**
 * Whether Increase Poly Models is on at any level.
 */
bool modIsModelSmoothingOn(void)
{
	return modGetModelSmoothingLevel() != 0;
}

/**
 * Whether the game's distance models are in use: the Model LOD setting.
 * Increase Poly Models used to hold it off; now it pushes the switch out
 * (modGetModelLodDistanceScale).
 */
bool modIsModelLodOn(void)
{
	return g_ModOptions.modellod != 0;
}

#ifndef PLATFORM_N64
/**
 * What a distance node's distance is scaled by before it is compared with
 * the model's thresholds (modelUpdateDistanceRelations): 1 for stock.
 *
 * Under Increase Poly Models the far model must not replace a near one the
 * renderer is still bending, and the far model is a different mesh, so the
 * switch would be a pop. The thresholds were set for a 240-line screen: a
 * character switches at 900 units, about 44 pixels tall there, and at
 * 1080p the same distance leaves it 200 pixels tall with edges the
 * renderer gives two or three segments. So the switch is held off until
 * the figure is as small on this screen as it was on the console's, where
 * every edge is under the renderer's pixels-per-segment and it draws the
 * near model flat anyway: the distance is scaled by 240 over the height
 * drawn, and never up.
 */
f32 modGetModelLodDistanceScale(void)
{
	s32 height;

	if (!modIsModelSmoothingOn()) {
		return 1.0f;
	}

	height = videoGetHeight();

	if (height <= 240) {
		return 1.0f;
	}

	return 240.0f / height;
}
#endif

/**
 * Smooth Text, as the factor the font's glyphs are scaled up by: 1 for off.
 */
s32 modGetSmoothTextScale(void)
{
	return g_ModOptions.smoothtext ? 4 : 1;
}

/**
 * Enhance Textures, as the factor the game's textures are scaled up by: 1
 * for off, else 2, 4 or 8. The renderer keeps its own copy of this and of
 * Smooth Text (gfx_texture_enhance_scale, gfx_text_smooth_scale);
 * videoSetTextureEnhance() keeps them together.
 */
s32 modGetTextureEnhanceScale(void)
{
	switch (g_ModOptions.enhancetextures) {
	case MODENHANCE_2X:
		return 2;
	case MODENHANCE_4X:
		return 4;
	case MODENHANCE_8X:
		return 8;
	}

	return 1;
}

/**
 * Vivid Colours, as the frame's saturation and contrast: 1.0 for as drawn.
 * The renderer keeps its own copy (gfx_color_saturation, gfx_color_contrast);
 * videoSetVividColours() keeps them together.
 */
f32 modGetVividSaturation(void)
{
	switch (g_ModOptions.vividcolours) {
	case MODVIVID_LIGHT:
		return 1.2f;
	case MODVIVID_NORMAL:
		return 1.35f;
	case MODVIVID_HEAVY:
		return 1.5f;
	}

	return 1.0f;
}

f32 modGetVividContrast(void)
{
	switch (g_ModOptions.vividcolours) {
	case MODVIVID_LIGHT:
		return 1.08f;
	case MODVIVID_NORMAL:
		return 1.15f;
	case MODVIVID_HEAVY:
		return 1.25f;
	}

	return 1.0f;
}

/**
 * Black Level, as the fraction of full range taken off the bottom: 0 for
 * off. The renderer keeps its own copy (gfx_color_black_level);
 * videoSetBlackLevel() keeps them together.
 */
f32 modGetBlackLevelLift(void)
{
	switch (g_ModOptions.blacklevel) {
	case MODBLACK_LIGHT:
		return 0.02f;
	case MODBLACK_NORMAL:
		return 0.04f;
	case MODBLACK_HEAVY:
		return 0.07f;
	}

	return 0.0f;
}

/**
 * Whether a weapon number is something to fight with: not the empty hand,
 * and not the Combat Simulator's gadgets - the shield, the boost, the
 * cloak and the scanners - which the weapon table lists beside the guns
 * and which a player spawning "armed" with is unarmed.
 */
bool modIsWeaponAGun(s32 weaponnum)
{
	switch (weaponnum) {
	case WEAPON_NONE:
	case WEAPON_UNARMED:
	case WEAPON_DISABLED:
	case WEAPON_MPSHIELD:
	case WEAPON_COMBATBOOST:
	case WEAPON_CLOAKINGDEVICE:
	case WEAPON_XRAYSCANNER:
	case WEAPON_NIGHTVISION:
	case WEAPON_IRSCANNER:
	case WEAPON_BRIEFCASE2:
		return false;
	default:
		return weaponnum > WEAPON_NONE && weaponFindById(weaponnum) != NULL;
	}
}

/**
 * Whether a weapon may be held in each hand under Akimbo: anything that is
 * a weapon, two-handed or not. The weapon table's own dual-wield flag is
 * the pistols and the small automatics; the stock cheat "dual wield all
 * guns" already ignores it for the player, and so does this, for everyone.
 */
bool modCanAkimbo(s32 weaponnum)
{
	return modIsWeaponAGun(weaponnum);
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

/**
 * Clean Text Outlines: the border of outlined text drawn by the renderer as
 * a halo around the letter rather than the filled cell the font bakes in.
 * At the N64's resolution the cell reads as a bold outline; at a monitor's
 * it is a black square behind every glyph. The renderer holds its own copy
 * of this (gfx_clean_text_outlines); videoSetCleanTextOutlines() keeps them
 * together.
 */
bool modIsCleanTextOn(void)
{
	return g_ModOptions.cleantext != 0;
}

/**
 * Mission Respawn: whether a death in a mission is a new life.
 */
bool modIsMissionRespawnOn(void)
{
	return g_ModOptions.missionrespawn != 0;
}

/**
 * How many lives a mission has in all, MODLIVES_UNLIMITED for no limit.
 * Anything else is held to a multiple of MODLIVES_STEP up to MODLIVES_MAX,
 * which is what the menu offers.
 */
s32 modGetMissionLives(void)
{
	s32 lives = g_ModOptions.missionlives;

	if (lives <= MODLIVES_UNLIMITED) {
		return MODLIVES_UNLIMITED;
	}

	lives = (lives + MODLIVES_STEP / 2) / MODLIVES_STEP * MODLIVES_STEP;

	if (lives < MODLIVES_STEP) {
		lives = MODLIVES_STEP;
	}

	if (lives > MODLIVES_MAX) {
		lives = MODLIVES_MAX;
	}

	return lives;
}

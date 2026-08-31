#ifndef _IN_GAME_MODOPTIONS_H
#define _IN_GAME_MODOPTIONS_H

#include <ultra64.h>
#include "types.h"

/**
 * Dab's Mod Options - everything this fork added that a player should be able
 * to turn off.
 *
 * These are global settings kept in pd.ini, not arena rules kept in
 * mpsetup.options, which is what Jump and Start Armed used to be. An arena rule
 * only exists while a Combat Sim match does, and half of what this fork added -
 * the jump, the roll, the melee combos, the body that flinches when it is shot
 * - is worth having in a solo mission too. The bits they used to live in are
 * still reserved in constants.h so that a setup file saved by an older build is
 * not read back as something else.
 */

#define MODWHO_EVERYONE    0
#define MODWHO_PLAYERSONLY 1

#define MODROLL_OFF         0
#define MODROLL_EVERYONE    1
#define MODROLL_PLAYERSONLY 2

struct modoptions {
	s32 jumpheight;  // 0 for off, else the height multiplier, up to JUMPHEIGHT_MAX
	s32 jumpwho;     // MODWHO_*: whether simulants jump too
	s32 roll;        // MODROLL_*
	s32 melee;       // the punch and kick combo
	s32 flinch;      // the body twitching where a shot landed
	s32 spawnweapon; // SPAWNWEAPON_*: what everyone spawns holding in an arena
	f32 camdist;     // third person camera, units behind the eye
	f32 camclearance;// how far short of a wall it stops
	f32 cammindist;  // below which it is not worth leaving the eye at all
};

extern struct modoptions g_ModOptions;

s32 modGetJumpHeight(void);
bool modIsJumpEnabled(void);
bool modCanChrJump(void);
f32 modGetJumpImpulse(void);
f32 modGetJumpApex(void);
bool modCanPlayerRoll(void);
bool modCanChrRoll(void);
bool modIsMeleeComboEnabled(void);
bool modIsFlinchEnabled(void);
s32 modGetSpawnWeapon(void);

#endif

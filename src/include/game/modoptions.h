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

/**
 * Bodies: how many are left lying where they fell, and how long each one lies
 * there.
 *
 * The cost of the cap is paid at stage load, not when the bodies appear: a
 * simulant's body needs a chr of its own to stay behind in, so the cap is
 * reserved in chr, model, anim and prop slots by setupLoadFiles() whether the
 * match fills it or not - roughly two and a half kilobytes each.
 *
 * The cap is what is asked for rather than what is granted. modBodiesSetReserve()
 * takes what MEMPOOL_STAGE can carry at stage load and no more, and past that
 * point bodies are simply not kept and fade the way they always did. So a small
 * Game.MemorySize costs bodies, never a match. 64MB carries the full five
 * hundred alongside eighty simulants; a pd.ini written by an older build still
 * says 16 and wants raising by hand.
 *
 * The cap that costs nothing to raise still costs frames to fill. Five hundred
 * bodies on Temple runs at 16fps where sixty is normal - they are eighty
 * simulants' worth of models, drawn and ticked - so the default is the number
 * that leaves the game feeling like itself and the maximum is there for anyone
 * who wants to see what a massacre looks like.
 *
 * MODBODYTIME_OFF means a body lies there until the cap pushes it out, which on
 * a cap this size is most of a match.
 */
#define MODBODIES_OFF     0
#define MODBODIES_DEFAULT 128
#define MODBODIES_MAX     500
#define MODBODYTIME_OFF   0
#define MODBODYTIME_MAX   600

/**
 * How many kept bodies may be drawn in one frame, nearest first.
 *
 * Keeping a body and drawing it are separate costs, and only one of them is
 * large. Measured on Temple with two hundred props in view: the bodies cost
 * 49ms of a 68ms frame to draw, against 19ms for everything else including
 * their own ticking. Their models have no cheaper level of detail to fall back
 * on - scaling the LOD distance four times over moved 16fps to 18 - so the only
 * thing that buys frames is submitting fewer of them.
 *
 * The ones dropped are the furthest away, which are the smallest on screen, and
 * the choice is remade every frame from the draw order that already exists.
 * MODBODIESDRAWN_ALL draws all of them.
 */
#define MODBODIESDRAWN_ALL 0
#define MODBODIESDRAWN_MAX MODBODIES_MAX

struct modoptions {
	s32 jumpheight;  // 0 for off, else the height multiplier, up to JUMPHEIGHT_MAX
	s32 jumpwho;     // MODWHO_*: whether simulants jump too
	s32 roll;        // MODROLL_*
	s32 melee;       // the punch and kick combo
	s32 flinch;      // the body twitching where a shot landed
	s32 spawnweapon; // SPAWNWEAPON_*: what everyone spawns holding in an arena
	s32 spawnweaponwho; // MODWHO_*: whether simulants spawn armed too
	f32 camdist;     // third person camera, units behind the eye
	f32 camclearance;// how far short of a wall it stops
	f32 cammindist;  // below which it is not worth leaving the eye at all
	s32 bodies;      // how many bodies are left lying around, 0 for off
	s32 bodytime;    // seconds one lies there, 0 for until the cap takes it
	s32 bodiesdrawn; // how many may be drawn at once, 0 for all of them
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
bool modCanChrSpawnArmed(void);
s32 modGetBodiesKept(void);
s32 modGetBodyTime(void);
s32 modGetBodiesDrawn(void);
bool modKeepsBodies(void);

#endif

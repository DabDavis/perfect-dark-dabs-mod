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

/**
 * Guards Alerted!: the alarm never stops, and guards keep coming.
 *
 * In a stock mission the alarm is something a guard raises when the player is
 * seen or heard, and it has two effects: every guard who can hear it goes
 * alert and comes looking, and on a few stages the mission script spawns a
 * handful of reinforcements. Both stop after thirty seconds. This setting is
 * the alarm as a permanent condition, on every stage - solo, Combat Simulator,
 * the Institute - with reinforcements the port spawns itself, so a stage whose
 * script never spawned any gets them too. How many are on their feet at once
 * and how quickly the next one arrives are settings of their own; see
 * modalarm.c.
 *
 * The siren is separate. It is the sound of the thing, but thirty seconds of
 * it is one matter and a whole match of it is another, so it can be turned off
 * on its own while the guards keep coming.
 */
#define MODALARM_OFF 0
#define MODALARM_ON  1

/**
 * How many alerted guards may be on their feet at once. The ceiling is the
 * simulant count's, and for the same reason: each one is a chr, a model, an
 * animation and a prop reserved at stage load, and eighty is where a match
 * stops feeling like a match.
 */
#define MODALARM_GUARDS_MIN     1
#define MODALARM_GUARDS_DEFAULT 6
#define MODALARM_GUARDS_MAX     80

/**
 * How fast they come, in guards per ten seconds. One is a guard every ten
 * seconds, which keeps a player moving; the default is one every five, about
 * what the Villa's four-in-two-minutes feels like when it never stops; fifty
 * is one every fifth of a second, which puts the whole eighty on their feet
 * inside twenty seconds - a swarm, for anyone who asks for one.
 */
#define MODALARM_SPEED_MIN     1
#define MODALARM_SPEED_DEFAULT 2
#define MODALARM_SPEED_MAX     50

/**
 * What a reinforcement carries. Stage weapons is the match's own six slots,
 * or the mission side arms outside a match. Random rolls the whole table of
 * guns a guard can hold, the way Start Armed's Random does for a player.
 */
#define MODALARM_WEAPONS_STAGE  0
#define MODALARM_WEAPONS_RANDOM 1

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
	s32 guardsalerted; // MODALARM_ON: the alarm as a permanent condition
	s32 alertedguards; // how many reinforcements may be up at once
	s32 guardspawnspeed; // how fast they come, in guards per ten seconds
	s32 guardweapons; // MODALARM_WEAPONS_*: what they carry
	s32 alarmsound;  // whether the siren plays while the alarm is on
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
bool modIsGuardsAlertedOn(void);
s32 modGetAlertedGuards(void);
s32 modGetGuardSpawnSpeed(void);
s32 modGetGuardWeapons(void);
bool modIsAlarmSoundEnabled(void);

#endif

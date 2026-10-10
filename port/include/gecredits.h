#ifndef _IN_GECREDITS_H
#define _IN_GECREDITS_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GoldenEye's credits (port/src/gecredits.c): what GoldenEye plays when the
 * Cradle is finished - its level Cuba, Bond and Natalya in the jungle under a
 * camera that circles them, and the names rolling up the screen - converted
 * out of the player's ROM as a mission of its own that the folder never lists.
 */

// GoldenEye's own mission numbers, the folder's order (geconvert.c's
// g_Missions): the Cradle, and Cuba, which the conversion puts after the
// twenty the folder shows
#define GEMISSION_CRADLE 17
#define GEMISSION_CUBA   20

void gecreditsStageStart(s32 stagenum);

// Whether this stage is Cuba, the credits
s32 gecreditsIsOn(void);

// CameraOrbitPad's six arguments, as its record has them
void gecreditsOrbit(s32 distance, s32 height, s32 speed60, s32 padnum, s32 lookheight, s32 start);
// The orbit's camera, from playerTick()'s TICKMODE_WARP; false when there is none
s32 gecreditsCameraTick(void);

// CreditsRoll and IFCreditsHasCompleted
void gecreditsRoll(void);
s32 gecreditsHaveRolled(void);

void gecreditsTick(void);
Gfx *gecreditsRender(Gfx *gdl);

// Online (netcoop.c, protocol 27): Cuba's camera and roll as the host's list
// runs them, for a guest's screen, which runs no lists
struct gecreditsnet {
	s32 orbit;        // the camera goes round the pad
	s32 padnum;
	s32 distance;
	s32 height;
	s32 lookheight;
	f32 speed;        // radians a 60th
	f32 angle;        // radians
	s32 state;        // 0 not yet, 1 rolling, 2 over
	f32 frame;        // how far the roll is
};
void gecreditsNetState(struct gecreditsnet *out);
void gecreditsNetFollow(const struct gecreditsnet *in);

#endif

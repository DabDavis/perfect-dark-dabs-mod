#ifndef _IN_GESLAPPERS_H
#define _IN_GESLAPPERS_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GoldenEye's slappers: Perfect Dark's own unarmed (WEAPON_UNARMED) on a
 * converted GoldenEye level is GoldenEye's ITEM_FIST - its own hand model out
 * of the player's ROM (the conversion's Igx001Z), its slap, its reach, damage,
 * timing and sounds (geslappers.c). Everywhere else it is Perfect Dark's punch.
 */

struct hand;
struct chrdata;

// A stage is loading: whether unarmed is GoldenEye's there, and the weapon
// table's unarmed swapped to match (setup.c, before anything is given)
void geslappersStageLoad(s32 stagenum);

// Whether unarmed is GoldenEye's slappers on this stage
s32 geslappersActive(void);

// Whether this hand holds them
s32 geslappersInHand(const struct hand *hand);

// A slap has begun in this hand (bondgun.c's melee state): one of GoldenEye's
// two at random
void geslappersStart(struct hand *hand, s32 handnum);

// Whether this hand's slap has reached the moment it lands, and whether it is
// still being swung at all
s32 geslappersStruck(s32 handnum);
s32 geslappersSwinging(s32 handnum);

// Each tick, after the hand's states: the swing further on, as the hand's
// posrotmtx
void geslappersTick(struct hand *hand, s32 handnum, f32 lvupdate60);

// A slap that reached nobody: GoldenEye's whoosh (chrprop.c's PUNCHING_AIR_SFX)
void geslappersMissed(void);

// GoldenEye's own cut of a slap's damage by what the victim is doing and
// which side it is hit from (chraction.c's ITEM_FIST); 1 for anything else
f32 geslappersDamageScale(const struct chrdata *chr, f32 angle);

#ifdef __cplusplus
}
#endif

#endif

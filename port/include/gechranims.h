#ifndef _IN_GECHRANIMS_H
#define _IN_GECHRANIMS_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's own animations on a converted level's characters.
 * port/src/gechranims.c.
 *
 * Perfect Dark's first 183 animation numbers are GoldenEye's own table in
 * GoldenEye's order, and chraction.c is GoldenEye's chrlv.c grown, so most of
 * what a guard plays is already named by GoldenEye's number - Perfect Dark
 * re-made the frames behind it. On a remake stage those rows are served out of
 * the conversion's own animations (animOverride()), and where Perfect Dark
 * went its own way - the hit and death tables, the walks and runs by weapon,
 * the side step, the grenade, the blast deaths - chraction.c asks for
 * GoldenEye's choice while g_GeChrAnims is on.
 */

// on while the stage's characters play GoldenEye's own animations
extern s32 g_GeChrAnims;

/** Before a stage's characters are made: on for a remake stage, off otherwise. */
void geChrAnimsStageStart(s32 stagenum);

/** Our number for GoldenEye's own animation geid (its initanitable.c id), or -1. */
s32 geChrAnim(s32 geid);

/** GoldenEye's death_stagger: shot dead against a wall. rngRandom() & 1 picks. */
struct animtablerow *geChrAnimsStagger(s32 index);

/**
 * One of GoldenEye's blast deaths for a blast from side (0-7, as chraction.c's
 * angle index), picked with pick; false when there is none.
 */
s32 geChrAnimsBlast(s32 side, u32 pick, s32 *animnum, s32 *flip, f32 *speed, f32 *startframe, f32 *thudframe, f32 *endframe);

#endif

#ifndef _IN_GEHITPUFF_H
#define _IN_GEHITPUFF_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's bullet puffs on a converted GoldenEye level: glass2.c's bullet
 * sparks, the pale animated smoke a shot leaves where it hits a body, a wall
 * or an object, drawn with GoldenEye's own frames out of the level's
 * textures/ (and the release's texture/sfx/ pictures in its look).
 * port/src/gehitpuff.c.
 */

// On every stage load, after geImpactStageStart() (lv.c)
void geHitPuffStageStart(s32 stagenum);

// GoldenEye's puffs are what this stage's shots leave
s32 geHitPuffActive(void);

/**
 * chr.c's chrCreateHitPuffs(): a shot into a body at hitpos, travelling along
 * dir (unit), at the given hit part - the impact puff pulled 42 back towards
 * the shooter and, half the time, a bigger puff 42 beyond the hit.
 */
void geHitPuffChr(struct prop *chrprop, s32 hitpart, struct coord *hitpos, struct coord *dir);

// A shot's puff on a wall or an object: pulled `back` units back along dir
// (dir may be NULL for none)
void geHitPuffBg(struct coord *hitpos, struct coord *dir, f32 back);

void geHitPuffTick(void);
Gfx *geHitPuffRender(Gfx *gdl);

#endif

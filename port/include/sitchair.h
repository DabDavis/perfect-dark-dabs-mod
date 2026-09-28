#ifndef _IN_SITCHAIR_H
#define _IN_SITCHAIR_H

#include <ultra64.h>
#include "types.h"

/**
 * Sitting in the Carrington Institute's chairs (port/src/sitchair.c), behind
 * Mod.SitInChairs. The player sits as he drives GoldenEye's tank: still
 * walking (MOVEMODE_WALK), with these the places the walk asks the chair.
 */

// a stage begins: nobody is sitting
void sitChairReset(void);

// objTestForInteract(): whether this object is a chair the player may sit in
s32 sitChairIsSeat(struct defaultobj *obj);

// objTestForInteract(): with a chair in it, the nearer of the two is used -
// so from behind a chair he sits, and seated he uses the terminal before him
s32 sitChairKeepsInteract(struct prop *current, struct prop *candidate);

// propobjInteract(): the use button on a chair. 1 when it was the chair's
s32 sitChairInteract(struct prop *prop);

// bmoveHandleActivate(): the use button while seated stands him up, unless
// there is something before him to use (the terminal on the desk), which the
// level's own interaction then takes. 1 when it was the chair's
s32 sitChairActivate(void);

// bwalkApplyMoveData(): seated, the sticks move nothing. 1 when seated
s32 sitChairApplyMoveData(struct movedata *data);

// bwalkTick(), before the walk: the player carried to the seat and back
void sitChairTick(void);

// bwalk0f0c63bc(): the walk's own move is held while he is in the chair
s32 sitChairHoldsMove(void);

// bwalkUpdateVertical(): a seated man's eye
f32 sitChairEyeHeight(f32 eyeheight);

// playerTickThirdPerson(): the body faces the way the chair does
s32 sitChairBodyFacing(struct player *player, f32 *facing);

// playerChooseThirdPersonAnimation(): the body sits down, sits and stands. 1 when it was the chair's
s32 sitChairAnimateBody(struct chrdata *chr, f32 *angleoffset);

// chr0f01f378(): how far the seat raises the player's body over his floor
f32 sitChairBodyLift(struct prop *playerprop);

#endif

#ifndef _IN_GETANK_H
#define _IN_GETANK_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's tank on a converted GoldenEye mission (port/src/getank.c). The
 * player drives it as he walks: these are the places the walk asks it.
 */

// setupCreateProps(), once the object is made
void geTankCreate(struct defaultobj *obj);

// a stage begins: nobody is in anything
void geTankReset(void);

s32 geTankIsDriving(void);

// this player, whoever the current one is
s32 geTankPlayerDriving(struct player *player);

// a driver's own body is not drawn
s32 geTankHidesChr(struct chrdata *chr);

// GoldenEye shows the tank from behind and above while it is driven: the
// third person camera's four settings, replaced while the current player drives
void geTankCamera(f32 *dist, f32 *height, f32 *side, f32 *fwd);

// any player, for an AI list's IFBondInTank
s32 geTankAnyoneDriving(void);

// the activate button: in beside one, out of the one he is in. 1 when it was the tank's
s32 geTankActivate(void);

// the input as the tank's own; 1 when the walk should take none of it
s32 geTankApplyMoveData(struct movedata *data);

// the step the walk is about to take, which in a tank is the tank's
void geTankDrive(struct coord *delta);

// the trigger with the tank's shells held: the cannon
void geTankFireCannon(void);

// the player's collision radius, which in a tank is the hull's half width
f32 geTankRadius(struct prop *playerprop, f32 radius);

// the eye's height over the ground, which in a tank is the seat's
f32 geTankEyeHeight(f32 eyeheight);

// GoldenEye takes a quarter of the damage in a tank
f32 geTankDamageScale(void);

// after the walk: the tank under him, guards under it, the engine
void geTankTick(void);

// the turret and the barrel on their pivots, before it is drawn
void geTankUpdateModel(struct prop *prop);

#endif

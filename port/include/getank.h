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

// the cannon in hand: its sight held in the middle of the view (bondmove.c)
s32 geTankHoldsSight(void);

// this player, whoever the current one is
s32 geTankPlayerDriving(struct player *player);

// a driver's own body is not drawn
s32 geTankHidesChr(struct chrdata *chr);

// an ending's camera takes every driver out of the tank
void geTankLeaveForCutscene(void);

// GoldenEye shows the tank from behind and above while it is driven: the
// third person camera's four settings, replaced while the current player drives
void geTankCamera(f32 *dist, f32 *height, f32 *side, f32 *fwd);

// any player, for an AI list's IFBondInTank
s32 geTankAnyoneDriving(void);

// the activate button: in beside one, out of the one he is in. 1 when it was the tank's
s32 geTankActivate(void);

/**
 * GoldenEye's climb onto a tank Bond has walked into, at the top of the
 * player's collision step: the hull's or the turret's height into
 * `bondonground`, and the tank let go of when he walks off it. Answers whether
 * this move is to be held (he is still being lifted).
 */
s32 geTankBoard(void);

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

// The tank a projectile's owner is driving (its shells leave from inside it), or NULL
struct prop *geTankShellTank(struct prop *ownerprop);

// GoldenEye's tank model placed as a plain object: its cannon's flash off
void geTankUpdateParkedModel(struct prop *prop);

// The current player died, in the tank or not (playerDieByShooter()).
void geTankPlayerDied(void);

/**
 * Netplay (netpredict.c, netpuppets.c): what of a driver's tank the walk
 * carries from one tick to the next. A client keeps it per tick in its
 * prediction ring and puts it back before replaying commands, and the host
 * sends its own for the client's player in the local-player block, so the
 * replay starts from the host's tank as it does from the host's walk.
 * state 0 is out of any tank (nothing else is meaningful then).
 */
struct getanknet {
	u8 state;       // 0 out, 1 climbing in, 2 driving
	u8 penalty;     // ticks held to half speed after driving over something
	f32 entert;
	f32 hullyaw;
	f32 speed;
	f32 turnsum;    // the turn's one pole filter
	f32 turretyaw;
};

void geTankNetSave(s32 playernum, struct getanknet *out);
// only while both this machine and `in` have the player in a tank: entering
// and leaving are each machine's own press
void geTankNetLoad(s32 playernum, const struct getanknet *in);

// the player driving this tank prop, -1 when none
s32 geTankDriverOf(struct prop *prop);

#endif

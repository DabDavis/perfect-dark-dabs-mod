#ifndef _IN_GEDEATHCAM_H
#define _IN_GEDEATHCAM_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's death replay (port/src/gedeathcam.c): once the first person fall
 * has faded to black, Bond's body falls again three times, in slow motion, each
 * time seen from a camera placed at random round where he died - bondview2.c's
 * CAMERAMODE_DEATH_CAM_SP and pickDeathCameraAngles().
 */

// The current player is dying on a GE Plus mission: GoldenEye takes the gun
// out of his hands, and the sight and the ammo with it.
s32 geDeathCamIsGoldenEye(void);

// While the current player's death has a replay still to run, or running: the
// stage end (solo) and the respawn (multiplayer) wait for it.
s32 geDeathCamHolds(void);

// The replay was cut short by a press, which in multiplayer is the respawn.
s32 geDeathCamTakeRespawn(void);

// The replay wants the player's chr body built and ticked, as third person does.
s32 geDeathCamWantsBody(struct player *player);

// The body's head as its tick has just posed it, from playerTickThirdPerson().
void geDeathCamSeeHead(void);

// The replay's camera, from playerTick() while the player is dead. False when
// there is no replay and the death camera is the game's own. `hintpos` and
// `hintrooms` are what the camera's room is resolved from.
s32 geDeathCamCamera(struct coord *pos, struct coord *up, struct coord *look,
		struct coord *hintpos, RoomNum *hintrooms);

#endif

#ifndef _IN_GECINEMA_H
#define _IN_GECINEMA_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GE Plus's Cinema: GoldenEye's own opening camera shots, watched on their
 * levels (port/src/gecinema.c). The folder's Cinema page arms a mission, the
 * stage starts as a mission does, and every INTROTYPE_CAMERA record the setup
 * carries is shown in turn with its one or two lines of text.
 */

// The Cinema page picked a mission; the stage that loads next is its cinema.
void gecinemaArm(s32 mission);
// Every stage load, from setup.c: takes up what the folder armed.
void gecinemaStageStart(void);
s32 gecinemaIsOn(void);
// Every frame of a level, from lvTick().
void gecinemaTick(void);
// And after playerTick() has built its own camera, from lvTickPlayer(): the
// shot's camera, put where the record says without the player's prop going
// with it.
void gecinemaCameraTick(void);

// Whether the folder should open again on the Cinema page, and the mission it
// should be showing; taking it clears both.
s32 gecinemaWantsFolder(void);
s32 gecinemaTakeFolderMission(void);

#endif

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

// What the Cinema page plays of a mission: its opening - every camera shot in
// turn, then the fade and the swirl down to Bond - or its ending.
#define GECINEMA_OPENING 0
#define GECINEMA_ENDING  1

// The Cinema page's Loop row, for an opening: Off plays it once and swirls
// down to Bond; Level cycles its shots with no swirl until its music breaks
// cleanly near two minutes, then goes back; All does that for every mission
// in order, round again after the last, until the player backs out.
#define GECINEMA_LOOP_OFF   0
#define GECINEMA_LOOP_LEVEL 1
#define GECINEMA_LOOP_ALL   2
#define GECINEMA_NUM_LOOPS  3

void gecinemaSetLoop(s32 loop);
s32 gecinemaGetLoop(void);

// The Cinema page picked a mission; the stage that loads next is its cinema.
void gecinemaArm(s32 mission, s32 what);
// An ending the Cinema page is playing is over: the level's own exit and its
// aiEndLevel come here instead of ending a mission nobody played. True when it
// was a cinema's.
s32 gecinemaEndingOver(void);
// Every stage load, from setup.c: takes up what the folder armed.
void gecinemaStageStart(void);
s32 gecinemaIsOn(void);
// Every frame of a level, from lvTick().
void gecinemaTick(void);
// And after playerTick() has built its own camera, from lvTickPlayer(): the
// shot's camera, put where the record says without the player's prop going
// with it.
void gecinemaCameraTick(void);

// A mission's own opening - GoldenEye's still, fade and swirl down to Bond -
// which a converted mission plays before the player has control.
s32 gecinemaIntroIsOn(void);
s32 gecinemaIntroIsStill(void);
s32 gecinemaIntroIsSwirl(void);
// From playerTick()'s TICKMODE_WARP: the swirl's camera. True while it has it.
s32 gecinemaSwirlTick(void);

// Whether the folder should open again on the Cinema page, and the mission it
// should be showing; taking it clears both.
s32 gecinemaWantsFolder(void);
s32 gecinemaTakeFolderMission(void);

#endif

#ifndef _IN_ROOMSHEEN_H
#define _IN_ROOMSHEEN_H

#include <ultra64.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reflections that follow the player's movement as well as their turning.
 *
 * The N64 texgen reads only the normal against the camera's LookAt, so it
 * sees where the eye looks and never where it stands. G_TEXGEN_EYE_EXT bends
 * the lookup by the eye ray (gfx_pc.cpp), which is enough for anything that
 * stands still in the world; a gun held in front of the eye walks with it and
 * also needs the scroll roomSheenTexgenShift() writes.
 */

/**
 * Before a K7 sheen draw on a held or posed mesh (G_LIGHTING | G_TEXTURE_GEN
 * with G_TEXGEN_EYE_EXT): the shift the current player's own movement has
 * scrolled the streaks by.
 */
Gfx *roomSheenTexgenShift(Gfx *gdl);

/**
 * Level Reflections (Mod.LevelReflectFollow): the reflective surfaces the
 * levels mark themselves - their room lists turn on G_LIGHTING |
 * G_TEXTURE_GEN over an environment map, Defection's metal and most windows -
 * look up through the eye ray, and walking turns their LookAt the way turning
 * the camera does (G_TEXGEN_TURN_EXT: across the view yaws it, along it
 * pitches it, half a turn per 400 units). The eye ray alone moved them too
 * little to see: they stayed pinned to the screen while the wall slid past.
 * Begin and End go round a room pass and round each prop standing in the
 * level (its lifts, windows and doors carry texgen spans of their own); the
 * flags reach only the spans the lists put under texgen.
 */
s32 roomSheenGetStockFollow(void);
void roomSheenSetStockFollow(s32 on);
Gfx *roomSheenStockBegin(Gfx *gdl);
Gfx *roomSheenStockEnd(Gfx *gdl);

/**
 * After a K7 sheen pass inside Begin and End (an XBLA mesh on a prop): writes
 * the turn and the flags again, since the pass wrote its own scroll and
 * cleared the eye flag.
 */
Gfx *roomSheenStockResume(Gfx *gdl);

#ifdef __cplusplus
}
#endif

#endif

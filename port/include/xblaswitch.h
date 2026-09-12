#ifndef _IN_XBLASWITCH_H
#define _IN_XBLASWITCH_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's assets, all of them, on one switch.
 *
 * Every part of the release the port can draw has a checkbox of its own on the
 * page, because each one is something somebody wants on its own: the meshes
 * (xblamesh.h), the art on them and on the game's own textures (xblatex.h),
 * the rooms (xblastage.h), the font (xblafont.h) and the explosion
 * (xblaexpl.h). What a page of checkboxes cannot answer is what the release
 * looks like as a whole against the ROM as a whole, which is the one question
 * you have to be standing in a level to ask - so this is the five of them
 * together, from one key, live where the player is standing.
 *
 * Two states and no memory of a mixed one: with every part on, a press takes
 * every part off, and any other arrangement is taken to the whole release.
 * Somebody who wants the meshes without their art still has the page; a key
 * that has to be looked at to know what it will do is no use for flipping
 * between two pictures, which is all this is for.
 */

/** Whether every part of the release is switched on. */
s32 xblaSwitchGetEnabled(void);

/**
 * All five at once, as live as each of them is on its own - the models change
 * under the player on the next frame and the rooms with them, except on a
 * machine whose package is still inside its archive, where this is what takes
 * it apart and the level after this one has the meshes (xblaMeshSetEnabled()).
 */
void xblaSwitchSetEnabled(s32 enabled);

/**
 * Mod.XblaMeshKey (F6): the switch above from a key, polled by xblaSwitchTick()
 * each frame beside texpackTick(). Bound from Dab's Mod Options with the
 * texture pack keys.
 */
s32 xblaSwitchGetKey(void);
void xblaSwitchSetKey(s32 vk);
void xblaSwitchTick(void);

#ifdef __cplusplus
}
#endif

#endif

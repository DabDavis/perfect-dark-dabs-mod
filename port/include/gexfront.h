#ifndef _IN_GEXFRONT_H
#define _IN_GEXFRONT_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GE-X Plus's menus as GoldenEye's own folder screens (port/src/gexfront.c):
 * the folder model, GoldenEye's fonts and strings and its crosshair cursor, all
 * from the conversion of the player's ROM (menu/ and the models block of
 * mods/GoldenEye Arenas).
 *
 * While open it replaces the Perfect Menu's ticking and drawing: menuTick()
 * and menuRender() hand over to it and return.
 */

// Opens the folder at its mode select. False when the conversion's menu files
// are not there, and the caller shows Perfect Dark's GE-X Plus dialog instead.
s32 gexFrontOpen(void);
s32 gexFrontIsActive(void);
void gexFrontTick(void);
Gfx *gexFrontRender(Gfx *gdl);

// GoldenEye's folders theme while open (menuChooseMusic() asks), else -1
s32 gexFrontMusic(void);

#endif

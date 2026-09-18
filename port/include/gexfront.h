#ifndef _IN_GEXFRONT_H
#define _IN_GEXFRONT_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GE Plus's menus as GoldenEye's own folder screens (port/src/gexfront.c):
 * the folder model, GoldenEye's fonts and strings and its crosshair cursor, all
 * from the conversion of the player's ROM (menu/ and the models block of
 * mods/GoldenEye Arenas).
 *
 * While open it replaces the Perfect Menu's ticking and drawing: menuTick()
 * and menuRender() hand over to it and return.
 */

// Opens the folder at its mode select. False when the conversion's menu files
// are not there, and the caller shows Perfect Dark's GE Plus dialog instead.
s32 gexFrontOpen(void);
s32 gexFrontIsActive(void);
// Opens the folder at Multiplayer Options after a GE Plus match. False when
// it cannot be drawn, and the caller keeps Perfect Dark's Combat Simulator.
s32 gexFrontOpenAfterMatch(void);
void gexFrontTick(void);
Gfx *gexFrontRender(Gfx *gdl);

// GoldenEye's folders theme while open (menuChooseMusic() asks), else -1
s32 gexFrontMusic(void);

/**
 * The conversion's menu files (the two fonts, the title screen's strings and
 * the folder model) loaded without opening the folder, for the intro
 * (port/src/geintro.c), which runs before it and draws GoldenEye's own text
 * with the same fonts. False when they are not there. Everything loaded stays
 * loaded until the folder closes.
 */
s32 gexFrontLoadShared(void);

// A string of LtitleE by its index, "" when there is none
const char *gexFrontTitleString(s32 index);

// The state GoldenEye's text renderer draws under (microcode_constructor())
Gfx *gexFrontTextSetup(Gfx *gdl);

/**
 * GoldenEye's own text, in its Zurich Bold (gothic 0) or Bank Gothic (1), at a
 * position on the 440x330 frame both games lay their screens out on.
 * gexFrontTextMeasure() gives what it will take, unscaled.
 */
Gfx *gexFrontTextPrint(Gfx *gdl, s32 gothic, s32 x, s32 y, const char *text, u32 colour);
void gexFrontTextMeasure(s32 gothic, const char *text, s32 *width, s32 *height);

#endif

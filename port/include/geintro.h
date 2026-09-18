#ifndef _IN_GEINTRO_H
#define _IN_GEINTRO_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GoldenEye's own intro, played when the player enters GE Plus
 * (port/src/geintro.c): the gun barrel, the GoldenEye logo and the cast reel,
 * which is GoldenEye's chain from the Rare logo to the file select. The boot
 * screens before it are not here - the game the player is in has already
 * booted.
 *
 * Everything drawn is the conversion of the player's ROM: GoldenEye's own
 * characters and animations, its prop models, its backdrop, its blood and its
 * fonts and strings. While it plays it owns the menu's tick and render, as the
 * folder screens do, and when it ends it opens them.
 */

// Starts the intro. False when the conversion's files are not there, and the
// caller opens the folder screens (or Perfect Dark's dialog) straight away.
s32 geIntroOpen(void);
s32 geIntroIsActive(void);
void geIntroTick(void);
Gfx *geIntroRender(Gfx *gdl);

// GoldenEye's M_INTRO while it plays (menuChooseMusic() asks), else -1
s32 geIntroMusic(void);

#endif

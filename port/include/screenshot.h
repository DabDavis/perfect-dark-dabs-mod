#ifndef _IN_SCREENSHOT_H
#define _IN_SCREENSHOT_H

#include <PR/ultratypes.h>

/**
 * A key that writes the frame you are looking at to a PNG.
 *
 * Not a controller bind. The CK_/CONT_ binds this fork added are read out of
 * the pad in bondmove.c, so they only exist while a player has control of a
 * body; a screenshot is worth taking of a menu, a cutscene or the end of a
 * match just as much, so this one is a plain key read once a frame no matter
 * what the game is doing.
 */

void screenshotInit(void);

// Once per frame, after inputUpdate(). Reads the key.
void screenshotTick(void);

// Take one at the end of the frame being drawn now.
void screenshotRequest(void);

// VK_ value of the key, or 0 if unbound.
s32 screenshotGetKey(void);
void screenshotSetKey(s32 vk);

#endif

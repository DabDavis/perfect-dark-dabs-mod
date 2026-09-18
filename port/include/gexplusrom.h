#ifndef _IN_GEXPLUSROM_H
#define _IN_GEXPLUSROM_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEXPLUSROM_NONE   0 // no GoldenEye ROM in data/ and no arenas from before
#define GEXPLUSROM_READY  1 // the arenas are in mods/GoldenEye Arenas/
#define GEXPLUSROM_FAILED 2 // a ROM was found and the conversion failed (see the log)

/**
 * Converts the player's GoldenEye 007 (US) ROM in data/ into the GE Plus
 * arenas when they are not there already. Called once at startup, after the
 * window opens and before the mods are mounted; draws its own notice while
 * it works. --no-ge-convert skips it.
 */
void gexPlusRomConvert(void);

s32 gexPlusRomGetState(void);

/**
 * The startup notice the conversion draws - black, a title, a line under it and
 * a bar of done out of total - as one frame, for other work done once before
 * the game's own fonts are loaded (gebeanUnpackAtStartup()). Upper case,
 * digits and - . / only.
 */
void gexPlusRomNotice(const char *title, const char *line, s32 done, s32 total);

#ifdef __cplusplus
}
#endif

#endif

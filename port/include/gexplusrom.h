#ifndef _IN_GEXPLUSROM_H
#define _IN_GEXPLUSROM_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// The folder under mods/ the conversion writes. It is a mod only for its maps
// and GE Plus's files, never one to load: its textures/ are GoldenEye's art
// under Perfect Dark's numbers, which loaded as the mod repaints the Institute
// (modListIsMapsOnly()).
#define GEXPLUSROM_DIR "GoldenEye Arenas"

#define GEXPLUSROM_NONE   0 // no GoldenEye ROM in data/ and no arenas from before
#define GEXPLUSROM_READY  1 // the arenas are in mods/GoldenEye Arenas/
#define GEXPLUSROM_FAILED 2 // a ROM was found and the conversion failed (see the log)
// arenas from before, kept because there is no ROM to convert again from, and
// written by a converter older than this build: whatever the newer one adds
// (the intro's characters and animations, the folder's menu files) is not in
// them, and nothing can put it there until the ROM is back in data/
#define GEXPLUSROM_OLD    3

/**
 * Converts the player's GoldenEye 007 (US) ROM in data/ into the GE Plus
 * arenas when they are not there already. Called once at startup, after the
 * window opens and before the mods are mounted; draws its own notice while
 * it works. --no-ge-convert skips it.
 */
void gexPlusRomConvert(void);

s32 gexPlusRomGetState(void);

/**
 * The GoldenEye ROM hacks found in added-content/ (as the ROM or as its patch)
 * and converted to arenas beside GE Plus's, each in mods/<its name>/
 * (geconvertVariantName(): "Goldfinger 64"). Never part of GE Plus.
 */
s32 gexPlusRomGetNumVariants(void);
const char *gexPlusRomGetVariant(s32 index);

/**
 * Whether a folder under mods/ is a conversion - GE Plus's GoldenEye Arenas or
 * a ROM hack's - which is mounted for its maps and never loaded as the mod
 * (modListIsMapsOnly()).
 */
s32 gexPlusRomIsConversionDir(const char *name);

/**
 * Whether a patch the Mod list found applies to no Perfect Dark ROM (mod.c)
 * makes a GoldenEye ROM hack this converts; gexPlusRomAdoptFile() then moves
 * it - or the archive it came out of - to added-content/, where the next start
 * converts it. A new drop in mods/ is caught before the Mod list sees it, at
 * startup (gexPlusRomConvert()).
 */
s32 gexPlusRomPatchIsHack(const char *path);
s32 gexPlusRomAdoptFile(const char *path);

/**
 * GoldenEye's own file names for the models the conversion writes by number
 * (geconvertReadNames()), read from the ROM the startup scan found; 0 when
 * there is none. For the asset dump.
 */
s32 gexPlusRomReadNames(void (*fn)(void *arg, int kind, int num, const char *file), void *arg);

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

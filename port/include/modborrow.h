#ifndef _IN_MODBORROW_H
#define _IN_MODBORROW_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Another installed mod's assets, taken beside whatever mod is loaded, the way
 * the Stage Loader takes its maps: the mod is mounted for its files alone, its
 * definitions are read out of its data segment, and its animations and sounds
 * go in after the game's own under numbers of their own. See modborrow.c.
 */

// Mount the mod the GoldenEye guns are borrowed from, if there is one. After
// every mount of the mod list (boot, and a live swap, which empties the mounts).
void modBorrowMount(void);

// Read the borrowed guns: after animsInit() and sndInit(), since the mod's
// animations and sounds are appended to those tables, and again after a live
// swap has emptied the file slots.
void modBorrowCommit(void);

// Whether mount index moddir was made for borrowing alone: the Stage Loader
// leaves its maps out, since nobody asked for them.
s32 modBorrowIsGunsOnlyMount(s32 moddir);

// The installed mod GoldenEye's guns come from, or NULL.
const char *modBorrowGoldenEyeName(void);

#ifdef __cplusplus
}
#endif

#endif

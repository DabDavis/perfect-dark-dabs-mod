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

/**
 * GoldenEye X's own characters, appended to g_HeadsAndBodies from base and to
 * the Combat Simulator's lists from their current counts, up to maxindex (the
 * saves hold a list index in 7 bits). Returns how many rows it used; 0 when
 * nothing is borrowed, which leaves the lists as they were.
 */
s32 modBorrowCharacters(s32 base, s32 maxrows, s32 maxindex);
// A borrowed body's name, from the mod's own language file, or NULL.
const char *modBorrowBodyName(s32 bodynum);
// Whether a head row is GoldenEye X's: one borrowed, or GoldenEye X's own file with it loaded.
s32 modBorrowIsGoldenEyeHead(s32 headnum);

// The borrowed mod's music for a match on one of its own arenas, or -1.
s32 modBorrowStageTrack(s32 stagenum);

// The borrowed mod's arenas take its stage rows, skies and props (after modloaderInit()).
void modBorrowArenas(void);
// The borrowed GoldenEye weapon sets: how many, and the first one's list index.
s32 modBorrowWeaponSets(s32 *first);
// Before a stage's setup loads: the mod's model states for its arena, the game's otherwise.
void modBorrowStageModels(s32 stagenum);

// The installed mod GoldenEye's guns come from, or NULL.
const char *modBorrowGoldenEyeName(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef IN_GAME_MODUNLOCKS_H
#define IN_GAME_MODUNLOCKS_H
#include <ultra64.h>

/**
 * What a mod's code unlocks outright, as the port's own rule: the "everything
 * unlocked" patch that twenty-three mods of the archive share (and GE-X
 * carries) forces one test after another to true, and each family of tests
 * is a bit here (mod.c's unlocks block, from the importer reading those
 * sites). 0 is the game as it ships.
 */
#define MODUNLOCK_CHEATS        0x01 // every cheat, without its time
#define MODUNLOCK_DIFFICULTIES  0x02 // every difficulty of every mission
#define MODUNLOCK_MPOPTIONS     0x04 // slow motion and one-hit kills without the challenges that unlock them
#define MODUNLOCK_FIRINGRANGE   0x08 // every firing range weapon found, every score gold
#define MODUNLOCK_SPECIALSTAGES 0x10 // the special assignments, all of them
#define MODUNLOCK_COMPLETION    0x20 // the game counts as completed: the alternative title and Perfect Dark mode
#define MODUNLOCK_ALLGUNS       0x40 // the Slayer in every mission

extern u32 g_ModUnlocks;

#endif

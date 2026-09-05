#ifndef _IN_MODIMPORT_H
#define _IN_MODIMPORT_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Converts a console mod - an xdelta, BPS or IPS patch against the stock ROM -
 * into a mod directory in place: files/, segs/, textures/ and a modconfig.txt
 * are written under outDir, with what could not be used set aside in
 * files.incompatible/ and segs.unlocated/, and IMPORT.txt saying what happened.
 *
 * The in-game equivalent of tools/importmod, which it mirrors step for step.
 *
 * Both paths are taken as the file system has them (already expanded).
 * With basePatchPath, that patch is applied to the stock ROM first and the
 * patch under import on top of it - for a mod distributed as a patch against
 * another mod's ROM. The result is still diffed against the stock ROM.
 *
 * Returns 1 when something usable was written, 0 when the mod carried nothing
 * the port can use, MODIMPORT_NEEDS_BASE when the patch does not apply to the
 * stock ROM, and -1 when it could not be imported at all (a broken patch, no
 * stock ROM to apply it to). IMPORT.txt is written in every case so the
 * attempt is not repeated on the next start.
 */
s32 modImportPatch(const char *patchPath, const char *outDir, const char *basePatchPath);

// modImportPatch() returns this when the patch does not apply to the stock
// ROM: it may be one made on top of another mod, and a sibling patch that did
// apply is worth trying as basePatchPath.
#define MODIMPORT_NEEDS_BASE (-2)

// Written as the first line of IMPORT.txt. A directory whose report names an
// older importer, or none, is imported again on the next start: raise this
// when a fix changes what an import produces (2: sample tables placed by
// the stock samples a mod kept, not measured back from the next segment;
// 3: the stage table and the solo player's body and head; 4: every outfit's;
// 5: the solo guards' random head lists; 6: the animated texture numbers;
// 7: the stage environments; 8: the camera-pinned rooms;
// 9: the scene's own copy of them and the star field; 10: each stage's music
// and the Combat Simulator's track list; 11: ammo and explosion types, the
// auto-switch lists, simulant weapon preferences, HUD message styles and the
// global AI lists).
#define MODIMPORT_VERSION 11
#define MODIMPORT_VERSION_LINE "importer: 11 (the game's own)"

#ifdef __cplusplus
}
#endif

#endif

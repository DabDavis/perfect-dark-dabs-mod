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
 * Returns 1 when something usable was written, 0 when the mod carried nothing
 * the port can use, and -1 when it could not be imported at all (a patch for
 * another ROM, a broken patch, no stock ROM to apply it to). IMPORT.txt is
 * written in every case so the attempt is not repeated on the next start.
 */
s32 modImportPatch(const char *patchPath, const char *outDir);

#ifdef __cplusplus
}
#endif

#endif

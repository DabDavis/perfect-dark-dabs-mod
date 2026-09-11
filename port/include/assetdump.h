#ifndef _IN_ASSETDUMP_H
#define _IN_ASSETDUMP_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The asset dump: every texture and every model the game has, written out
 * as files somebody can edit, in one go.
 *
 * Four passes, each of which is the whole of its table rather than what the
 * game happened to draw:
 *
 *   texture-dumps/<romid>/          every texture in the ROM, as texpack's
 *                                   --dump-textures wrote them: a PNG the
 *                                   right way up, the raw texels, the palette
 *                                   and a manifest - the layout a texture
 *                                   pack reads back
 *   texture-dumps/<romid>/xbla/     every record of the XBLA release's
 *                                   Textures.raw, the folder a pack's xbla/
 *                                   folder is a copy of
 *   model-dumps/n64/<name>.obj      every model in the ROM - the characters,
 *                                   the props and the guns - as OBJ, a group
 *                                   per list node, with an MTL naming the
 *                                   textures above
 *   model-dumps/xbla/<name>.obj     every mesh in the XBLA release's
 *                                   package, the same way, named for the
 *                                   model it replaces
 *
 * The two XBLA passes run only with a package in xbla/ (and unpack the
 * archive if that has not happened yet). Both directories sit beside the
 * executable, or in the save directory where that cannot be written.
 *
 * From the menu it runs a few files a frame under a time budget so the game
 * stays alive, and says where it is up to; --dump-assets runs the whole
 * thing at startup and exits.
 */

/** Starts the dump if one is not running. */
void assetDumpStart(void);

/** Runs some of it: called once a frame. */
void assetDumpTick(void);

/** Stops one that is running, closing what it had open. */
void assetDumpCancel(void);

s32 assetDumpIsRunning(void);

/** One line for the menu: what is being written, or what was. */
const char *assetDumpGetStatus(void);

/** --dump-assets (and --dump-textures): the whole dump, then exit. */
void assetDumpFromCommandLine(void);

#ifdef __cplusplus
}
#endif

#endif

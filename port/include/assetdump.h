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
 * Four passes for Perfect Dark, each of which is the whole of its table
 * rather than what the game happened to draw:
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
 * and GoldenEye's, each in folders of their own, only when there is one:
 *
 *   texture-dumps/ge-n64/           the textures of the conversion of the
 *                                   player's GoldenEye ROM (mods/GoldenEye
 *                                   Arenas/textures), by the conversion's
 *                                   number, with index.csv giving GoldenEye's
 *   model-dumps/ge-n64/{props,chars,hand}/  its models (files/Pgx, Cgx,
 *                                   Igx), named by the ROM's own file names,
 *                                   index.csv saying which is which
 *   model-dumps/ge-xbla/<look>/<kind>/<name>.obj  the GoldenEye XBLA
 *                                   release's (Project Bean's) files/new/ and
 *                                   original/ char, head, gun, prop,
 *                                   background and skydome, by Rare's names
 *   texture-dumps/ge-xbla/<look>/<kind>/<name>/  each one's pictures, and
 *                                   files/texture/ and new/texture/ as a PNG
 *                                   a picture
 *
 * The two XBLA passes run only with a package in added-content/ (and unpack
 * the archive if that has not happened yet). Both directories sit beside the
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

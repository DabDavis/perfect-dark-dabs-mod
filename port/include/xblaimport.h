#ifndef _IN_XBLAIMPORT_H
#define _IN_XBLAIMPORT_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Building a texture pack out of the Xbox 360 XBLA release, on the player's
 * own machine.
 *
 * The console release is the N64 game with its art replaced, and it kept the
 * game's texture numbering: record N of its Textures.raw is texture N. That
 * makes the conversion a decode rather than a match - there is no checksum to
 * reproduce and nothing to guess, which is what separates this from an
 * emulator pack (see riceconvert.py).
 *
 * Nothing is bundled. Mod.XblaPackage says where the player's own copy is;
 * failing that a few usual places are tried. The same conversion exists
 * offline as tools/texpack/xblaconvert.py, and the two agree - the tool is how
 * this one was checked.
 */

#define XBLAIMPORT_IDLE       0
#define XBLAIMPORT_EXTRACTING 1 // unpacking the archive the package came in
#define XBLAIMPORT_READING    2 // pulling Textures.raw out of the package
#define XBLAIMPORT_CONVERTING 3 // decoding and writing PNGs
#define XBLAIMPORT_DONE       4
#define XBLAIMPORT_FAILED     5

/**
 * Texture numbers the console release's records line up with.
 *
 * Its Textures.raw has 5747 of them and only the first NUM_TEXTURES are
 * replacements; the rest are the release's own dashboard and achievement art,
 * which has no texture number to go to. The count is the NTSC one because that
 * is the data the release was built from.
 */
#define XBLAIMPORT_NUM_REPLACED 3503

/** Whether a package was found. Its path, for the page to show. */
s32 xblaImportIsAvailable(void);
const char *xblaImportGetPackagePath(void);
void xblaImportRedetect(void);

/**
 * A path to an STFS package rather than to whatever the player has.
 *
 * The texture conversion can unpack an archive on its way past; anything that
 * reads a file at a time out of the package - the mesh loader - cannot, since
 * a .7z is usually solid. So this hands back the detected path when that is
 * already a package, and otherwise whatever an earlier conversion left
 * unpacked. NULL when there is neither.
 */
const char *xblaImportGetStfsPath(void);

/** Starts a conversion. Returns 0 if it could not be started. */
s32 xblaImportStart(void);
void xblaImportCancel(void);

/**
 * Driven from the scheduler once a frame, not from the page that starts it, so
 * backing out of the menu does not leave a conversion half done. Everything it
 * does is files, so the work is all on the worker thread and this only watches
 * for the end of it.
 */
void xblaImportTick(void);

s32 xblaImportGetState(void);
s32 xblaImportGetPercent(void);
const char *xblaImportGetStatus(void);
const char *xblaImportGetPackName(void);

/** --xbla-import: convert a pack and exit, for scripted setup. */
void xblaImportFromCommandLine(void);

/** Whether to leave out textures the release redrew at the original size. */
s32 xblaImportGetUpscalesOnly(void);
void xblaImportSetUpscalesOnly(s32 enabled);

#ifdef __cplusplus
}
#endif

#endif

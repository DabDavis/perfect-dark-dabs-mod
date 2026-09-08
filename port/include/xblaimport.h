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
 * Nothing is bundled. The player drops their own copy - the release's .7z, or
 * the package out of it - into the xbla/ folder beside the executable, the way
 * a mod goes in mods/; Mod.XblaPackage names a file somewhere else instead.
 * The same conversion exists offline as tools/texpack/xblaconvert.py, and the
 * two agree - the tool is how this one was checked.
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

/**
 * Makes the xbla/ folder, so there is somewhere to drop a package before there
 * is a package, and logs what was found there. Called once at startup.
 */
void xblaImportInit(void);

/** Whether a package was found. Its path, for the page to show. */
s32 xblaImportIsAvailable(void);
const char *xblaImportGetPackagePath(void);

/** The xbla/ folder itself, for the page to tell the player where to look. */
const char *xblaImportGetDropDir(void);

void xblaImportRedetect(void);

/**
 * A path to an STFS package rather than to whatever the player has, unpacking
 * the archive they dropped in xbla/ if that has not been done yet.
 *
 * Nothing reads a .7z a file at a time: it is one LZMA stream, so a single
 * file out of it costs the whole archive. It comes apart once into a dot
 * directory inside xbla/ and a marker file says that finished - a few seconds
 * and 250MB of disk, once, since the release's archive is already-compressed
 * data and barely compresses again. NULL when there is no package to be had.
 *
 * This blocks for that unpack, so it belongs on the paths that are about to
 * read the package - the texture conversion's worker, the mesh and texture
 * loaders with Mod.XblaMeshes on - and not on one that only wants to know
 * whether a package exists. xblaImportIsAvailable() answers that without
 * touching the disk beyond a scan.
 */
const char *xblaImportGetStfsPath(void);

/**
 * The same, except that it will not unpack anything: the package when there is
 * one on disk already, and NULL while the player's copy is still inside its
 * archive.
 *
 * This is for work that is done speculatively, on the chance that somebody
 * wants it - every model load is matched against the release's copy so that
 * switching the meshes on is a live thing to do, and that must not be what
 * costs a player who only wanted the texture pack 250MB and a stall. Failing
 * is not remembered, so the caller that is willing to pay still can.
 */
const char *xblaImportGetReadyStfsPath(void);

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

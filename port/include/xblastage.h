#ifndef _IN_XBLASTAGE_H
#define _IN_XBLASTAGE_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's level geometry, served in place of the ROM's.
 *
 * 4J rewrote 31 of the game's `bgdata/bg_*.seg` files: the same room format
 * the N64 game reads, with two to four times the triangles - bevelled edges,
 * rounded pipes, panelled walls. Because the format is the game's own, none
 * of the renderer has to learn anything: the file is handed to the ordinary
 * bg loader through the file slot that would otherwise hold the ROM's copy
 * (romdataFileLoad()), and everything downstream reads it as it would any
 * other level.
 *
 * Three things about the release's files are not what the port expects, and
 * are put right on the way in - see xblaStageLoad() and the notes in
 * CLAUDE-notes/xbla.md, "The level files":
 *
 * - every `gSPVertex` has its byte length zeroed. The game's own code counts
 *   vertices from the (n-1) field and never noticed; gfx_pc counts them from
 *   the length and would load none. Filled in from the count;
 * - the per-room memory sizes in section 3 are mostly zero, so the room
 *   loader sizes its allocation from the data instead (bgLoadRoom());
 * - a few rooms in five levels bind textures that only exist in the release
 *   (records past NUM_TEXTURES). Those are drawn through the same stand-in
 *   tile the meshes use (xblatex.h), from texLoadFromGdl().
 *
 * `Mod.XblaStages` is the switch, on by default, and it counts only while
 * `Mod.XblaMeshes` is on: the stage geometry is the release's models
 * feature applied to the rooms. A level's file is chosen as the level loads
 * and kept for the life of the level, since a room table from one copy must
 * never be used to read rooms out of the other.
 */

/** Mod.XblaStages. Whether the release's level files are wanted. */
s32 xblaStageGetEnabled(void);
void xblaStageSetEnabled(s32 enabled);

/**
 * A stage is being reset (lvReset()): decide for the coming level and let go
 * of the file the last one used. The decision is frozen from here until the
 * next reset, whatever the switches do in between.
 */
void xblaStageLevelReset(void);

/**
 * Whether what the switches say now differs from what the running level was
 * loaded under - the menu's cue that the change shows from the next level.
 */
s32 xblaStagePending(void);

/**
 * Whether the file slot for `name` (a `bgdata/bg_*.seg`) should come from the
 * release rather than the ROM. Cheap: a flag and a name check.
 */
s32 xblaStageWants(s32 fileNum, const char *name);

/**
 * The release's copy of the file, read out of the package, checked and
 * corrected, in a buffer the file slot owns (sysMemAlloc). NULL means use the
 * ROM's: no package, no such file, or a copy that could not be made safe.
 */
u8 *xblaStageLoad(s32 fileNum, const char *name, u32 *outSize);

/** The slot has been freed by romdata; forget it. */
void xblaStageReleased(s32 fileNum);

/**
 * Whether the level being loaded is drawing the release's rooms - which is
 * what texLoadFromGdl() asks before it reads a texture number wider than the
 * ROM's twelve bits.
 */
s32 xblaStageIsRelease(void);

/**
 * A texture command in a release room names a record the ROM has no texture
 * for. Writes the tile state for it - the meshes' 32 texel stand-in, with the
 * texture scale set so the room's coordinates, which 4J measured in texels of
 * the full picture, land on it - and returns the list after it.
 */
Gfx *xblaStageWriteTexture(Gfx *gdl, const Gfx *cmd, u32 record);

/** --xbla-stage-verbose: log each file and what was corrected in it. */
void xblaStageSetVerbose(s32 verbose);

#ifdef __cplusplus
}
#endif

#endif

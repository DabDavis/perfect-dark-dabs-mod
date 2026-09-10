#ifndef _IN_XBLASTAGE_H
#define _IN_XBLASTAGE_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Drawing the XBLA release's level geometry.
 *
 * The release rewrote 31 of the game's `bgdata/bg_*.seg` files in the game's
 * own room format with two to four times the triangles, and stored them in
 * the package under the same file ids (xblamesh.h has the package). This
 * serves those rooms to bg.c in place of the ROM's, behind Mod.XblaStages,
 * which counts only while Mod.XblaMeshes is on: the geometry is the models
 * feature applied to the rooms. CLAUDE-notes/xbla.md, "The level files", has
 * the format and what had to be corrected in it.
 *
 * The file slot keeps serving the ROM's copy of the level; only the rooms
 * come from the release. bgLoadRoom() asks xblaStageRoomSize() for each room
 * as it loads it, and a room that has a copy in the release is read through
 * xblaStageRoomRead() from the release's file, at its own address in the
 * release's room table - which is the one thing the two copies disagree on.
 * Everything else the level is built from (the room table, the portals, the
 * lights, section 3's bounding boxes) is byte for byte the same in both, so
 * the ROM's serves either. That is what makes the switch live inside a level:
 * a room can be loaded from either copy under the same level, and a flip of
 * either switch drops the loaded rooms for the next frame to load again.
 */

/** Mod.XblaStages: whether to draw them. On unless the player says otherwise. */
s32 xblaStageGetEnabled(void);
void xblaStageSetEnabled(s32 enabled);

/**
 * Either switch has been flipped - this one or the meshes'. Drops the
 * rooms loaded under the old setting, if there is a level and the setting
 * that reaches the rooms has changed. Called by both setters.
 */
void xblaStageSwitched(void);

/**
 * A level is being reset. Called from lvReset(): the last level's file goes,
 * and the next level's is read when its first room asks.
 */
void xblaStageLevelReset(void);

/**
 * How many bytes the release's copy of a room has, or 0 when the room comes
 * from the ROM as usual - the switches are off, the level is a mod's or one
 * the release stores the ROM's way, or there is no package ready to read.
 * Asked by bgLoadRoom() for every room it loads, before it sizes the room's
 * allocation. The first ask in a level is the one that reads the file.
 */
u32 xblaStageRoomSize(s32 roomnum);

/**
 * Copies the room's bytes - the number xblaStageRoomSize() gave - and
 * returns the segment address they are relative to: the room's entry in the
 * release's table, which is what bgLoadRoom() must relocate the room's
 * pointers against instead of the ROM's entry.
 */
uintptr_t xblaStageRoomRead(s32 roomnum, u8 *dst, u32 len);

/** bgLoadRoom() has finished with the room, either way. */
void xblaStageRoomDone(void);

/**
 * Whether the room being converted right now is the release's. What
 * texLoadFromGdl() asks before it treats a texture number past the ROM's
 * table as one of the release's records.
 */
s32 xblaStageIsRelease(void);

/**
 * Writes the tile state for one of the release's own textures - a record
 * past NUM_TEXTURES that only the package has - in place of the texture
 * command that named it. Returns the next free gdl.
 */
Gfx *xblaStageWriteTexture(Gfx *gdl, const Gfx *cmd, u32 record);

/** --xbla-stage-verbose: log each file taken or turned down, each room read
 * from the release, and each Xbox-only record bound. */
void xblaStageSetVerbose(s32 verbose);

#ifdef __cplusplus
}
#endif

#endif

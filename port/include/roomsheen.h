#ifndef _IN_ROOMSHEEN_H
#define _IN_ROOMSHEEN_H

#include <ultra64.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct prop;
struct modelrenderdata;

/**
 * Level Sheen: the stock K7 Avenger's sheen on the level's own rooms and on
 * the props standing in it.
 *
 * Neither carries normals the sheen can use (the rooms and the ROM's models
 * have baked vertex colours), so each room's solid, non-decal, non-cutout
 * triangles - and each prop model part's opaque ones - are copied once, the
 * first time they are drawn with the sheen on, with normals worked out from
 * the triangles and smoothed across neighbours that meet at a shallow angle.
 * The copy is drawn over what was just drawn, multiplied into it
 * (G_MULADD_EXT) so a dark surface stays dark.
 *
 * Mod.LevelSheen is the strength (0 off, 1 subtle, 2 normal, 3 strong).
 * Mod.LevelSheenStyle picks how the streaks are looked up: 0 is the K7's own
 * texgen, per vertex off the camera's LookAt, which gives a flat wall one
 * colour; 1 is per pixel (G_ENVMAP_EXT), which slides across a wall as the
 * eye moves. Both are live.
 */

#define ROOMSHEEN_STYLE_K7    0
#define ROOMSHEEN_STYLE_PIXEL 1

s32 roomSheenGetLevel(void);
void roomSheenSetLevel(s32 level);
s32 roomSheenGetStyle(void);
void roomSheenSetStyle(s32 style);

/**
 * The sheen over one room, after bgRenderRoomOpaque() has drawn it: the
 * room's matrix is still loaded. Builds the room's copy on first use.
 */
Gfx *roomSheenRender(Gfx *gdl, s32 roomnum);

/** Drops a room's copy; bgUnloadRoom() calls it. */
void roomSheenFree(s32 roomnum);

/**
 * The prop objRenderProp() is drawing, around its modelRender() call, or
 * NULL. A model part takes the sheen only while this names a prop of the
 * level's own (an object, door or pickup with no parent), which is what
 * keeps characters, held guns and menu models out.
 */
void roomSheenSetProp(struct prop *prop);

/**
 * The sheen over one model part, straight after its opaque list: list is
 * that list and base the vertex array segment 4 names for it, which this
 * binds again before it returns. The part's matrix is the one its own list
 * loaded. cutout says the list is drawn with its texture's alpha cut out,
 * which the sheen cannot follow, so it is left alone.
 */
void roomSheenRenderNode(struct modelrenderdata *renderdata, const void *node, Gfx *list, Vtx *base, s32 cutout);

/** Drops every model part's copy; lvReset() calls it with the models. */
void roomSheenResetNodes(void);

#ifdef __cplusplus
}
#endif

#endif

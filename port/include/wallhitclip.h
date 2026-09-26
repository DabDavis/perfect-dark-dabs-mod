#ifndef _IN_WALLHITCLIP_H
#define _IN_WALLHITCLIP_H

#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Clip Decals at Edges (Mod.DecalEdgeClip, modIsDecalClipOn()): a wall hit -
 * a blood splat, a bullet hole, a scorch mark - is a flat quad laid on the
 * plane of the triangle it hit, as big as it is whatever the surface under it,
 * so near a ledge, a table's edge or a doorway part of it hangs in the air.
 * With the setting on, the quad is cut down to the room triangles lying in its
 * plane (wallhitclip.c), once, a tick or two after it is made; props' marks
 * are left as they are.
 */

// wallhitReset(), once g_Wallhits is allocated
void wallhitClipReset(void);

// A wall hit was just made: it is clipped when the setting next looks at it
void wallhitClipBegin(struct wallhit *wallhit);

// Every clip goes back to untried: the rooms were reloaded from another copy
// of the level (xblaStageSwitched()), so the triangles may not be the same
void wallhitClipForgetAll(void);

// From wallhitsTick(), for each wall hit in use: clip it if it is due
void wallhitClipTick(struct wallhit *wallhit);

// From wallhitsTick(): the size an expanding splat is drawn at this tick
void wallhitClipSetScale(struct wallhit *wallhit, f32 scale);

/**
 * In place of the quad's gSPVertex and gSPTri2, after its gSPColor: draws the
 * clipped wall hit and returns true, or returns false with nothing written
 * when it has no clip (setting off, not clipped yet, or nothing hangs over).
 */
bool wallhitClipRender(Gfx **gdlptr, struct wallhit *wallhit);

// For the trace: how many wall hits are clipped, whole, and waiting
void wallhitClipCounts(s32 *clipped, s32 *whole, s32 *waiting);

#ifdef __cplusplus
}
#endif

#endif

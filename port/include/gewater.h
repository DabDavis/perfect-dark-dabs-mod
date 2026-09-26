#ifndef _IN_GEWATER_H
#define _IN_GEWATER_H

#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"

/**
 * GoldenEye's moving water on a converted level (port/src/gewater.c).
 *
 * GoldenEye draws two of its pictures - 1511, Dam's reservoir (and the bottom
 * of Complex's pits), and 1508, Caverns' water - as two copies of the picture
 * offset from each other, both creeping across the surface, and cross-faded
 * by the primitive LOD fraction on a slow sine (tex.c's texnum tests,
 * unk_092E50.c's MipMap2C_Something*_Setup, advanced each frame by
 * sub_GAME_7F092E50()). Perfect Dark's texture loader has no such test, so the
 * converted reservoir stood still.
 */

// GoldenEye's own picture numbers, which the conversion keeps
#define GEWATER_TEX_CAVERNS 1508
#define GEWATER_TEX_DAM     1511

/** Once a frame from lvTick(), after the frame's lvupdate60freal is set. */
void geWaterTick(void);

/** Whether a room's texture command naming `texturenum` gets GoldenEye's water on this stage. */
s32 geWaterIsWaterTexture(s32 texturenum);

/**
 * After a room's texture command for one of the two pictures has been written
 * (tile 0 and tile 1 both the picture, at the same place in TMEM): GoldenEye's
 * water blend, called as a list that is rewritten every frame. `w` and `h`
 * are the tile's size in texels. The room's render mode and culling stay.
 */
Gfx *geWaterWrite(Gfx *gdl, s32 w, s32 h);

#endif

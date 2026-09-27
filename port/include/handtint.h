#ifndef _IN_HANDTINT_H
#define _IN_HANDTINT_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The first person hands painted from the player's own character (handtint.c):
 * a hand file keeps its shape and its grip, and its sleeve and its skin are
 * repainted in the colours the character's own forearm and hand are painted.
 */

// Mod.HandsMatchBody: 0 off, 1 GoldenEye's characters (the default), 2 every character
extern s32 g_HandTintMode;

// xblamesh.c, building a mesh: 0 nothing, 1 its textures by bone, 2 and its colours by bone
s32 handtintWantsMesh(s32 fileid, s32 ischr);

// bondgun.c, before the hands are drawn: brings the tints up to date
void handtintTick(void);

// The renderer: the colour a texture at addr is repainted in, or 0
s32 handtintLookup(const void *addr, u8 *rgb);

// The renderer: repaints rgba (w x h) in rgb, keeping its light and shade
void handtintApply(u8 *rgba, u32 width, u32 height, const u8 *rgb);

#ifdef __cplusplus
}
#endif

#endif

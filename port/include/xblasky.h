#ifndef _IN_XBLASKY_H
#define _IN_XBLASKY_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's skies, in place of the game's own.
 *
 * The N64 draws its sky as a colour, a tiled cloud plane scrolling overhead and
 * a water plane below (skyRender()). 4J replaced that with a cube: six
 * pictures around the camera, drawn first with no depth test, in records 0e57
 * to 0e92 - eight cubes, each the six faces in Direct3D's order (+X -X +Y -Y
 * +Z -Z) stored bottom row first, and each led by the cloud layer 4J scrolled
 * over it. Which level has which cube is in the release's code, which is
 * encrypted, so the table is part what recordings of the release show and
 * part chosen by the picture, each row saying which; see CLAUDE-notes/xbla.md,
 * "The skies".
 *
 * A level with no cube in the table keeps the game's sky, and so does X-ray
 * vision.
 */

/** Mod.XblaSkies, the menu's "Enable Skies". */
s32 xblaSkyGetEnabled(void);
void xblaSkySetEnabled(s32 enabled);

/**
 * The stage's sky in place of skyRender()'s, or NULL when there is none to
 * draw - the switch is off, there is no package, the level has no cube in the
 * table, or the player is in X-ray. Called at the top of skyRender().
 */
Gfx *xblaSkyRender(Gfx *gdl);

#ifdef __cplusplus
}
#endif

#endif

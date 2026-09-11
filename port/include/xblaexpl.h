#ifndef _IN_XBLAEXPL_H
#define _IN_XBLAEXPL_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's explosion, on the game's own explosion.
 *
 * 4J did not upscale the ROM's explosion: records 001e to 0039 are still the
 * 56x56 puff and the 14x14 colour ramp the N64 draws, because their renderer
 * did not use them. What it used is a 48 frame animation of its own at
 * records 0e1f to 0e4e - a fireball that blooms, rises, throws sparks and
 * fades, 256x256 with its own colour and alpha - and that is what this puts
 * in the ROM's place.
 *
 * Nothing about how an explosion is drawn changes - one picture does. The game
 * sets up two tiles and a two-cycle combiner whose colour is
 * TEXEL0 * TEXEL1 * shade; the release's frame goes in as tile 0 and the ROM's
 * colour ramp stays on tile 1, so the fireball is drawn at the game's own
 * size, tint and opacity (that ramp's alpha averages two fifths, which is what
 * keeps an explosion a part of the scene rather than a flash over it). The
 * tiles, the billboard, the sizes, the frames a part ages through and every
 * coordinate are the game's - which is the same bargain a texture pack makes,
 * and is why this needed no combiner and no geometry of its own.
 */

/** Mod.XblaExplosions, the menu's "Enable Explosions". */
s32 xblaExplGetEnabled(void);
void xblaExplSetEnabled(s32 enabled);

/** The switch, and a package to read frames out of. */
s32 xblaExplHaveFrames(void);

/**
 * The stand-in tile for one of the game's explosion frames, or NULL when there
 * is nothing to draw with - in which case the ROM's own texture is bound, as
 * it always was.
 *
 * frame is the game's, 1 to 14, and names one of the release's 48 by where it
 * falls in the run. Frame 0 is never asked for: the ROM's is blank and the
 * game uses it as a part's first, invisible step. Decodes the record the first
 * time it is asked for and keeps it, so an explosion costs fourteen decodes
 * once and nothing after.
 *
 * Called from the draw on the game thread, like everything else in
 * explosionRender().
 */
const void *xblaExplBindFrame(s32 frame);

#ifdef __cplusplus
}
#endif

#endif

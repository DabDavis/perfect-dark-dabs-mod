#ifndef _IN_GEBEANSKY_H
#define _IN_GEBEANSKY_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GoldenEye XBLA's skies over a level served in HD (gebeanstage.c): a dome
 * round the eye with a cloud cap over it, from the Community Edition's
 * files/new/skydome/. The release's own eleven are one placeholder - the same
 * mountain sunset under every name - and the Community Edition is what gave
 * the levels skies of their own, so without it a level keeps GoldenEye's sky;
 * but for Surface, whose sky the placeholder is (gebeansky.c,
 * releaseSkyNames[]).
 *
 * gebeanSkyRender() draws it in place of the sky plane and answers the next
 * free command, or NULL where this level has none and the game's sky is to be
 * drawn as ever. The sea is not part of it: the caller draws the level's water
 * after it.
 */
Gfx *gebeanSkyRender(Gfx *gdl);
s32 gebeanSkyIsDrawn(void);

/**
 * The colour the HD level's fog and the fill under the dome take in place of
 * the release's fog colour, where a Community Edition dome's horizon differs
 * from it (Surface 2's grey storm over GoldenEye's dark red): 1 and rgb set,
 * else 0 and the release's fog stands.
 */
s32 gebeanSkyFogColour(u8 *rgb);

/** A new level: the last one's dome goes. */
void gebeanSkyLevelReset(void);

#ifdef __cplusplus
}
#endif

#endif

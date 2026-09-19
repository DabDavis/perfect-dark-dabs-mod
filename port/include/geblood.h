#ifndef _IN_GEBLOOD_H
#define _IN_GEBLOOD_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GoldenEye's blood, the wash that runs down the screen when Bond dies and
 * down the lens when he fires in the gun barrel (port/src/geblood.c).
 *
 * One animation serves both screens, as it does in GoldenEye: 42 frames of an
 * 80x96 intensity picture, each decoded from where the last one ended, which
 * the conversion of the player's ROM writes as menu/introblood.bin.
 */

// the wash's own picture, one texel a byte until a step packs it to two a byte
#define GEBLOOD_W 80
#define GEBLOOD_H 96

/** One screen's place in the wash: the intro's, or a dying player's. */
struct geblood {
	u8 *frame;      // GEBLOOD_W * GEBLOOD_H, packed to 4 bits by a step
	const u8 *next; // where the next frame of the stream starts, NULL at the end
	s32 wait;       // ticks left until the next frame
};

// Whether the conversion's wash is there to be drawn at all.
s32 geBloodAvailable(void);

// The next frame of the wash into b->frame, ready to draw as a 4-bit intensity
// texture. `restart` begins it again. 1 when the wash has run out.
s32 geBloodStep(struct geblood *b, s32 restart);
void geBloodDrop(struct geblood *b);

// The player dying is on one of the remake's stages and the wash is there.
s32 geBloodDeathActive(void);
// GoldenEye starts it on the frame the player dies.
void geBloodDeathStart(void);
// A frame of the wash over the player's viewport, and redbloodfinished set
// when it has run out - which is where Perfect Dark's own red wash takes over.
Gfx *geBloodDeathRender(Gfx *gdl);

#endif

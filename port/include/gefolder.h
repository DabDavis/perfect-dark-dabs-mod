#ifndef _IN_GEFOLDER_H
#define _IN_GEFOLDER_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modeldef;

/**
 * GE Plus's folder screens in the release's own art - see gefolder.c.
 *
 * GoldenEye's folder model is the one drawn either way: what changes is the
 * picture on each of its textures, which the release redrew at eight to
 * sixteen times the size. Repaint when the folder's model has loaded and the
 * release's meshes are on; it answers how many pictures were swapped, and 0
 * when the release is not there, the look is the N64's, or the model is not
 * the one the table was written against.
 */
s32 geFolderRepaint(struct modeldef *modeldef);

/**
 * Whether the release's own folder is drawn in place of GoldenEye's (the
 * release is there, its look is on, and its folder stands on GoldenEye's).
 */
s32 geFolderBeanActive(void);

/**
 * Around the folder's modelRender(): on swaps each of GoldenEye's list nodes
 * for the release's triangles for it, off puts GoldenEye's back.
 */
struct model;
void geFolderBeanSwap(struct model *model, s32 on);

/** Puts the ROM's own pictures back, for the model going away or the look changing. */
void geFolderForget(void);

/**
 * The release's meshes have been switched (F6). Pass the folder's model when
 * it is loaded and NULL when it is not.
 */
void geFolderSwitched(struct modeldef *modeldef);

/**
 * One of the release's menu pictures that is a file of its own - "characters/
 * natalya", "level/damicon", "sight" under files/texture/ - bound as a
 * stand-in for a textureconfig to name, with its size. NULL when the release
 * is not there, the look is the N64's, or it has no such picture.
 */
const void *geFolderMenuPicture(const char *name, s32 *width, s32 *height);

/**
 * The release's dark, out-of-focus desk behind the folder, in place of
 * GoldenEye's frame of cover cloth: a stand-in to draw over the whole screen,
 * or NULL under the N64's look.
 */
const void *geFolderBackdrop(void);

/**
 * The release's own set of one of GoldenEye's two fonts - gothic 0 for Zurich
 * Bold (the release's alps3), 1 for Bank Gothic (doc0) - one stand-in picture
 * a printable ASCII character, each glyph's box with a clear texel round it.
 * Metrics are in the release's pixels: left and top place the picture's
 * corner (clear border and all) from the pen and up from the baseline. NULL
 * when the release is not there, the look is the N64's, or the font is not.
 */
struct gefolderglyph {
	const void *tile;
	s16 width, height;   // the picture's, border included
	s16 left, top;
	s16 advance;
};

struct gefolderfont {
	s32 lineheight;
	s32 ascent;
	s32 space;
	s32 capheight;       // 'H' from its baseline to its top
	struct gefolderglyph glyphs[0x7f - 0x21];
};

const struct gefolderfont *geFolderFont(s32 gothic);

#ifdef __cplusplus
}
#endif

#endif

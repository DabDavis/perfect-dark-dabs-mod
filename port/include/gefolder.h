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

/** Puts the ROM's own pictures back, for the model going away or the look changing. */
void geFolderForget(void);

/**
 * The release's meshes have been switched (F6). Pass the folder's model when
 * it is loaded and NULL when it is not.
 */
void geFolderSwitched(struct modeldef *modeldef);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _IN_GETEXSURFACE_H
#define _IN_GETEXSURFACE_H

#include <ultra64.h>

/**
 * On a converted GoldenEye level (modloaderStageIsRemake()), every texture
 * number the level's own textures/ serves takes GoldenEye's surface for its
 * image - the sound a shot makes and the mark it leaves - in place of the one
 * Perfect Dark's table gives that number; any other stage has Perfect Dark's
 * back. Called on every stage load, after texReset(). port/src/getexsurface.c.
 */
void geTexSurfaceReset(s32 stagenum);

#endif

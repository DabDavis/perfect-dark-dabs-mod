#ifndef _IN_PNGWRITE_H
#define _IN_PNGWRITE_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Writes pixels out as a PNG at path, which is a real filesystem path rather
 * than a VFS one - run it through fsFullPath() first.
 *
 * channels must be 3 (RGB) or 4 (RGBA), and pixels must be tightly packed with
 * no padding between rows. Set bottomRowFirst for a buffer in the renderer's
 * bottom-up order; PNG stores rows top down and the flip happens here.
 *
 * Returns 1 on success. Logs its own failures, so a caller that has nothing to
 * add can ignore the result.
 */
s32 pngWrite(const char *path, const u8 *pixels, s32 width, s32 height, s32 channels, s32 bottomRowFirst);

#ifdef __cplusplus
}
#endif

#endif

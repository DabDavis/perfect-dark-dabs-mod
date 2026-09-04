#ifndef _IN_JPEGREAD_H
#define _IN_JPEGREAD_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decodes a JPEG into a freshly malloc'd RGBA32 buffer, top row first, which the
 * caller frees. Returns NULL and logs why on anything it cannot read. The same
 * contract as pngRead(), so the two are interchangeable at the call site.
 *
 * Alpha comes out 255 everywhere: JPEG has none. A pack wanting transparency has
 * to ship that texture as a PNG.
 */
u8 *jpegRead(const char *path, s32 *outWidth, s32 *outHeight);

/**
 * A PNG that pngread.c would not take, through stb_image instead.
 *
 * That decoder refuses what it does not handle rather than guessing - 16 bit
 * channels, say - and this is the second opinion. Only worth calling after
 * pngRead() has said no.
 */
u8 *pngReadFallback(const char *path, s32 *outWidth, s32 *outHeight);

#ifdef __cplusplus
}
#endif

#endif

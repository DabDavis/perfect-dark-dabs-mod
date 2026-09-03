#ifndef _IN_PNGREAD_H
#define _IN_PNGREAD_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decodes a PNG into a freshly malloc'd RGBA32 buffer, top row first, which the
 * caller frees. Returns NULL and logs why on anything it cannot read.
 *
 * Handles 8 bit greyscale, greyscale+alpha, truecolour, truecolour+alpha and
 * palette (with tRNS), non-interlaced - which is what every image editor writes
 * by default and what the texture packs in the wild contain. 16 bit channels,
 * sub-byte bit depths and Adam7 interlacing are refused rather than guessed at.
 */
u8 *pngRead(const char *path, s32 *outWidth, s32 *outHeight);

#ifdef __cplusplus
}
#endif

#endif

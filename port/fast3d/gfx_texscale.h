#ifndef GFX_TEXSCALE_H
#define GFX_TEXSCALE_H

#include <stdint.h>
#include <stdbool.h>

// How the texels past an edge are found when the filter reaches over it.
enum TexScaleEdge {
    TEXSCALE_EDGE_CLAMP,
    TEXSCALE_EDGE_WRAP,
    TEXSCALE_EDGE_MIRROR,
};

// Scale a straight-alpha RGBA32 image up by a whole factor. The result is a
// buffer this module owns, good until the next call, or NULL if the image is
// too big to bother with, in which case the caller uploads what it had. A
// glyph is treated as a shape: its coverage is sharpened rather than its
// colour. See gfx_texscale.cpp.
const uint8_t* gfx_texscale(const uint8_t* rgba, uint32_t width, uint32_t height, int scale,
                            enum TexScaleEdge edge_s, enum TexScaleEdge edge_t, bool glyph);

#endif

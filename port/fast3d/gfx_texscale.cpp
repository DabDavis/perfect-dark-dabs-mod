/**
 * Enhance Textures and Smooth Text: the game's own texels, scaled up as they
 * are uploaded.
 *
 * The N64 drew from a few kilobytes of texture memory, so a wall is 32 texels
 * across and on a monitor each one is a soft-edged blob the width of a thumb.
 * The GPU's bilinear filter is what makes the blob: a tent across the two
 * nearest texels. A texture pack replaces the texels with more of them, and
 * the renderer already draws whatever size was uploaded against the N64's
 * tile geometry (import_texture in gfx_pc.cpp), so the same door is open to
 * making more texels here from the ones the game has - not detail it never
 * had, but a better guess at the surface between the texels than a tent:
 *
 * - A texture is resampled through a Catmull-Rom cubic, which keeps every
 *   original texel where it was and draws a smooth curve rather than a
 *   straight line between them. The curve is held within the two texels it
 *   runs between, so a hard edge gets no overshoot on either side. The work
 *   is done with the colour premultiplied by the alpha so a cut-out's
 *   transparent texels, which are usually black, do not bleed a dark rim
 *   into its edge. No sharpening: a first version added an unsharp mask at
 *   the texture's own resolution, and on a computer screen drawn as a field
 *   of one-texel dashes it turned each dash into a distinct symbol, so the
 *   text-at-a-distance the artist painted read as Wingdings.
 *
 * - Some textures are the blur. A halftone portrait is a grid of dots, a
 *   terminal screen a scatter of single texels, a window blind a stack of
 *   one-texel lines, and each was drawn to be seen through the bilinear
 *   filter, which is what turns the dots into a face. Any sharper curve
 *   between the texels undoes that, so a texture whose detail sits mostly at
 *   texel frequency - the second difference between neighbours large next
 *   to the first difference across them - is left as it came. Over the
 *   textures of dataDyne's offices the ratio is about 1.2 for a wall or a
 *   face and 2.4 and up for every dot pattern and text screen.
 *
 * - A font glyph is a shape, not a picture: its colour is flat and the alpha
 *   is the letter. It gets the same cubic and no sharpening, and its coverage
 *   then goes through a smoothstep twice, which steepens the ramp between a
 *   solid texel and an empty one into an edge a pixel or two wide while
 *   leaving the font's own anti-aliasing texels, which sit in the middle of
 *   the ramp, where they were. Read at 4x that is a letter with a clean
 *   outline instead of a staircase of blurred squares.
 *
 * Wrapping textures are sampled across their seam so the join stays clean;
 * a padded upload (the tile narrower than its row) is clamped instead, since
 * its far edge is padding.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx_texscale.h"

// A full TMEM of 4-bit texels at four by four. Anything larger is not a
// stock texture and goes up as it came.
#define TEXSCALE_MAX_TEXELS (8192 * 16)
#define TEXSCALE_MAX_SCALE 4

// Above this ratio of texel-frequency detail to the rest, a texture is a
// pattern the bilinear filter was meant to blur, and is left alone.
#define TEXSCALE_PATTERN_RATIO 2.0f

static float* src_buf;   // premultiplied, source size
static float* mid_buf;   // after the horizontal pass: scaled width, source height
static float* dst_buf;   // scaled both ways
static uint8_t* out_buf;
static size_t src_cap, mid_cap, dst_cap, out_cap;

static bool grow(void** buf, size_t* cap, size_t need) {
    if (need <= *cap) {
        return true;
    }
    void* p = realloc(*buf, need);
    if (!p) {
        return false;
    }
    *buf = p;
    *cap = need;
    return true;
}

static inline int edge_index(int i, int n, enum TexScaleEdge edge) {
    if (i >= 0 && i < n) {
        return i;
    }
    switch (edge) {
        case TEXSCALE_EDGE_WRAP:
            i %= n;
            return i < 0 ? i + n : i;
        case TEXSCALE_EDGE_MIRROR: {
            const int period = 2 * n;
            i %= period;
            if (i < 0) {
                i += period;
            }
            return i < n ? i : period - 1 - i;
        }
        default:
            return i < 0 ? 0 : n - 1;
    }
}

// Catmull-Rom: the cubic through the four nearest texels that passes through
// each of them, so the original texels survive the resample exactly.
static inline float catmull_rom(float x) {
    x = fabsf(x);
    if (x < 1.0f) {
        return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    }
    return 0.0f;
}

// The four taps for each of the scale's output phases: an output pixel at
// phase p sits at (p + 0.5) / scale - 0.5 source texels from the texel it
// falls in, and reads that texel, the one before it and the two after.
struct Phase {
    int base;        // offset of the first tap from the source texel
    float w[4];
};

static void make_phases(struct Phase* phases, int scale) {
    for (int p = 0; p < scale; p++) {
        const float centre = ((float)p + 0.5f) / (float)scale - 0.5f;
        const int first = (int)floorf(centre) - 1;
        float sum = 0.0f;
        phases[p].base = first;
        for (int k = 0; k < 4; k++) {
            phases[p].w[k] = catmull_rom(centre - (float)(first + k));
            sum += phases[p].w[k];
        }
        for (int k = 0; k < 4; k++) {
            phases[p].w[k] /= sum;
        }
    }
}

// How much of a texture's detail is one texel wide: the mean absolute second
// difference across each texel, over the mean absolute difference between its
// two neighbours, both ways, on the premultiplied luma. A smooth surface is
// near 1; a field of isolated dots or one-texel lines runs well above 2.
static float pattern_ratio(const float* img, int w, int h, enum TexScaleEdge edge_s, enum TexScaleEdge edge_t) {
    double d2 = 0.0, d1 = 0.0;

    for (int y = 0; y < h; y++) {
        const int yu = edge_index(y - 1, h, edge_t);
        const int yd = edge_index(y + 1, h, edge_t);
        for (int x = 0; x < w; x++) {
            const int xl = edge_index(x - 1, w, edge_s);
            const int xr = edge_index(x + 1, w, edge_s);
            const float* c = img + ((size_t)y * w + x) * 4;
            const float* l = img + ((size_t)y * w + xl) * 4;
            const float* r = img + ((size_t)y * w + xr) * 4;
            const float* u = img + ((size_t)yu * w + x) * 4;
            const float* d = img + ((size_t)yd * w + x) * 4;
            const float lc = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            const float ll = 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
            const float lr = 0.2126f * r[0] + 0.7152f * r[1] + 0.0722f * r[2];
            const float lu = 0.2126f * u[0] + 0.7152f * u[1] + 0.0722f * u[2];
            const float ld = 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
            d2 += fabsf(2.0f * lc - ll - lr) + fabsf(2.0f * lc - lu - ld);
            d1 += fabsf(ll - lr) + fabsf(lu - ld);
        }
    }

    // A flat texture has nothing at any frequency
    if (d1 < 1e-3 * (double)w * h) {
        return 0.0f;
    }
    return (float)(d2 / d1);
}

const uint8_t* gfx_texscale(const uint8_t* rgba, uint32_t width, uint32_t height, int scale,
                            enum TexScaleEdge edge_s, enum TexScaleEdge edge_t, bool glyph) {
    if (scale < 2 || scale > TEXSCALE_MAX_SCALE || width == 0 || height == 0) {
        return NULL;
    }

    const int w = (int)width;
    const int h = (int)height;
    const int ow = w * scale;
    const int oh = h * scale;

    if ((size_t)ow * oh > TEXSCALE_MAX_TEXELS) {
        return NULL;
    }

    if (!grow((void**)&src_buf, &src_cap, (size_t)w * h * 4 * sizeof(float)) ||
        !grow((void**)&mid_buf, &mid_cap, (size_t)ow * h * 4 * sizeof(float)) ||
        !grow((void**)&dst_buf, &dst_cap, (size_t)ow * oh * 4 * sizeof(float)) ||
        !grow((void**)&out_buf, &out_cap, (size_t)ow * oh * 4)) {
        return NULL;
    }

    // In, premultiplied
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float a = rgba[i * 4 + 3] / 255.0f;
        src_buf[i * 4 + 0] = rgba[i * 4 + 0] / 255.0f * a;
        src_buf[i * 4 + 1] = rgba[i * 4 + 1] / 255.0f * a;
        src_buf[i * 4 + 2] = rgba[i * 4 + 2] / 255.0f * a;
        src_buf[i * 4 + 3] = a;
    }

    if (!glyph && pattern_ratio(src_buf, w, h, edge_s, edge_t) > TEXSCALE_PATTERN_RATIO) {
        return NULL;
    }

    struct Phase phases[TEXSCALE_MAX_SCALE];
    make_phases(phases, scale);

    // Across
    for (int y = 0; y < h; y++) {
        const float* row = src_buf + (size_t)y * w * 4;
        float* orow = mid_buf + (size_t)y * ow * 4;
        for (int x = 0; x < w; x++) {
            for (int p = 0; p < scale; p++) {
                const struct Phase* ph = &phases[p];
                float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (int k = 0; k < 4; k++) {
                    const float* t = row + (size_t)edge_index(x + ph->base + k, w, edge_s) * 4;
                    acc[0] += t[0] * ph->w[k];
                    acc[1] += t[1] * ph->w[k];
                    acc[2] += t[2] * ph->w[k];
                    acc[3] += t[3] * ph->w[k];
                }
                // Held between the two texels it lies between: taps 1 and 2
                const float* n0 = row + (size_t)edge_index(x + ph->base + 1, w, edge_s) * 4;
                const float* n1 = row + (size_t)edge_index(x + ph->base + 2, w, edge_s) * 4;
                float* o = orow + ((size_t)x * scale + p) * 4;
                for (int c = 0; c < 4; c++) {
                    const float lo = n0[c] < n1[c] ? n0[c] : n1[c];
                    const float hi = n0[c] < n1[c] ? n1[c] : n0[c];
                    o[c] = acc[c] < lo ? lo : (acc[c] > hi ? hi : acc[c]);
                }
            }
        }
    }

    // Down
    for (int y = 0; y < h; y++) {
        for (int p = 0; p < scale; p++) {
            const struct Phase* ph = &phases[p];
            const float* rows[4];
            for (int k = 0; k < 4; k++) {
                rows[k] = mid_buf + (size_t)edge_index(y + ph->base + k, h, edge_t) * ow * 4;
            }
            float* orow = dst_buf + ((size_t)y * scale + p) * ow * 4;
            for (int x = 0; x < ow; x++) {
                for (int c = 0; c < 4; c++) {
                    float v = rows[0][x * 4 + c] * ph->w[0] + rows[1][x * 4 + c] * ph->w[1] +
                              rows[2][x * 4 + c] * ph->w[2] + rows[3][x * 4 + c] * ph->w[3];
                    const float a = rows[1][x * 4 + c];
                    const float b = rows[2][x * 4 + c];
                    const float lo = a < b ? a : b;
                    const float hi = a < b ? b : a;
                    orow[x * 4 + c] = v < lo ? lo : (v > hi ? hi : v);
                }
            }
        }
    }

    // Out, straight alpha again
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        float* px = dst_buf + i * 4;
        float a = px[3] < 0.0f ? 0.0f : (px[3] > 1.0f ? 1.0f : px[3]);
        float inv = a > 0.0f ? 1.0f / a : 0.0f;

        if (glyph) {
            a = a * a * (3.0f - 2.0f * a);
            a = a * a * (3.0f - 2.0f * a);
        }

        for (int c = 0; c < 3; c++) {
            float v = px[c] * inv;
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            out_buf[i * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
        }
        out_buf[i * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
    }

    return out_buf;
}

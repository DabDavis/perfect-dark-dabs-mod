#ifndef GFX_POST_H
#define GFX_POST_H

#include <stdint.h>
#include <string>

/*
 * The post-process chain's shaders, shared by both renderers: SMAA 1x (Jorge
 * Jimenez et al., port/fast3d/post/SMAA.hlsl) and AMD's FSR 1 (EASU and
 * RCAS, port/fast3d/post/ffx_*.h), both MIT, compiled from their own sources
 * at run time. Every pass is one full-screen triangle; the vertex shader is
 * the same for all of them and hands the fragment shader vUV.
 *
 * A pass reads up to three textures, TEX0-TEX2, and four floats, uParams:
 *
 *   SMAA_EDGES    TEX0 the frame                  -> edges (clear it first)
 *   SMAA_WEIGHTS  TEX0 edges, TEX1 area, TEX2 search -> blend weights
 *   SMAA_BLEND    TEX0 the frame, TEX1 weights    -> the frame, anti-aliased
 *   EASU          TEX0 the frame; uParams.xy the size drawn to
 *   RCAS          TEX0 EASU's output; uParams.x sharpness in stops
 *   COPY          TEX0 the frame, bilinear; going down, a box over the
 *                 window pixel's footprint (supersampling)
 *   TAA           TEX0 this frame (jittered), TEX1 the history, TEX2 the
 *                 depth; uTaa[0-2] take (u, v, depth, 1) to last frame's
 *                 (u, v) times w, uTaa[3] the rect in uv, uTaa[4].y 1 to
 *                 clip the history to the neighbourhood (0 shows the raw
 *                 reprojection, for checking it), .z the
 *                 weight of this frame, .w whether the history is any good
 *
 * OpenGL binds TEX0-2 to units 0-2 as uTex0-2 and uParams and uTaa[5] as
 * uniforms; Vulkan's push constants are four ints (the three texture slots
 * and a linear clamping sampler), uParams and uTaa, 112 bytes.
 *
 * Every image is in OpenGL's row order in both renderers, and SMAA and EASU
 * work in texture space alone, so neither needs to know which way up it is;
 * the area and search textures are uploaded as their headers store them.
 */

enum GfxPostPass {
    GFX_POST_SMAA_EDGES,
    GFX_POST_SMAA_WEIGHTS,
    GFX_POST_SMAA_BLEND,
    GFX_POST_EASU,
    GFX_POST_RCAS,
    GFX_POST_COPY,
    GFX_POST_TAA,
    GFX_POST_NUM_PASSES
};

struct GfxPostLang {
    bool vulkan;
    const char *version;      // what follows "#version "
    uint32_t texture_slots;   // Vulkan: the bindless tables' sizes
    uint32_t sampler_slots;
};

std::string gfx_post_vertex_shader(const GfxPostLang &lang);
std::string gfx_post_fragment_shader(const GfxPostLang &lang, GfxPostPass pass);

// SMAA's lookup textures expanded to RGBA8, in their headers' row order
#define GFX_POST_AREA_WIDTH 160
#define GFX_POST_AREA_HEIGHT 560
#define GFX_POST_SEARCH_WIDTH 64
#define GFX_POST_SEARCH_HEIGHT 16
#define GFX_POST_TAA_PARAMS 20
const uint8_t *gfx_post_area_rgba(void);
const uint8_t *gfx_post_search_rgba(void);

// Tells Vulkan's shader cache and the logs which pass failed
const char *gfx_post_pass_name(GfxPostPass pass);

#endif

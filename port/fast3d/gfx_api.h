#ifndef GFX_API_H
#define GFX_API_H

#ifndef __cplusplus
#include <stdint.h>
#include <stdbool.h>
#endif

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

struct XYWidthHeight {
    int16_t x, y;
    uint32_t width, height;
};

struct GfxDimensions {
    float internal_mul;
    uint32_t width, height;
    float aspect_ratio;
};

struct GfxInitSettings {
    struct GfxWindowManagerAPI *wapi;
    struct GfxRenderingAPI *rapi;
    struct GfxWindowInitSettings window_settings;
};

extern struct GfxDimensions gfx_current_window_dimensions; // The dimensions of the window
extern struct GfxDimensions
    gfx_current_dimensions; // The dimensions of the draw area the game draws to, before scaling (if applicable)
extern struct XYWidthHeight
    gfx_current_game_window_viewport; // The area of the window the game is drawn to, (0, 0) is top-left corner
extern uint32_t gfx_msaa_level;
extern struct XYWidthHeight gfx_current_native_viewport; // The internal/native video mode of the game
extern float gfx_current_native_aspect; // The aspect ratio of the above mode
extern bool gfx_framebuffers_enabled;
extern bool gfx_detail_textures_enabled;

// What ended a batch and forced a draw call. See g_GfxFlushReasons.
enum GfxFlushReason {
    GFX_FLUSH_TEXTURE,      // a different texture had to be bound
    GFX_FLUSH_SHADER,       // a different colour combiner
    GFX_FLUSH_BLEND,        // alpha blend / modulate changed
    GFX_FLUSH_SAMPLER,      // filter or clamp mode changed on a bound texture
    GFX_FLUSH_DEPTH,        // depth test/write/compare mode changed
    GFX_FLUSH_VIEWPORT,     // viewport or scissor moved
    GFX_FLUSH_BUFFERFULL,   // buf_vbo hit g_GfxMaxBufferedTris
    GFX_FLUSH_OTHER,        // framebuffer switches and other once-a-frame work
    GFX_FLUSH_COUNT
};

extern uint32_t g_GfxFlushReasons[GFX_FLUSH_COUNT];
extern uint32_t g_GfxNumDistinctTextures;
extern uint32_t g_GfxNumTexUploads;
extern uint32_t g_GfxNumTexEvictions;
extern uint32_t g_GfxTexCacheSize;

// Renderer cost of the last frame, and the batch size that shapes it.
// See the comments on these in gfx_pc.cpp, and --gfxstats / --gfxbatch.
extern uint32_t g_GfxMaxBufferedTris;
extern uint32_t g_GfxNumDrawCalls;
extern uint32_t g_GfxNumBufferFullFlushes;
extern uint32_t g_GfxNumTris;
extern uint32_t g_GfxNumVerts;
extern uint32_t g_GfxLogStats;

void gfx_init(const struct GfxInitSettings *settings);
void gfx_destroy(void);
struct GfxRenderingAPI* gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx* commands);
void gfx_end_frame(void);
void gfx_set_target_fps(int);
void gfx_set_texture_filter(enum FilteringMode mode);
void gfx_set_mipmap_filter(enum MipmapFilteringMode mode);
void gfx_texture_cache_clear(void);
void gfx_texture_cache_delete(const uint8_t *orig_addr);
void gfx_texture_cache_delete_range(const uint8_t *start, const uint8_t *end);
int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_set_framebuffer(int fb, float noise_scale) ;
void gfx_reset_framebuffer(void);
void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back);

// Called once per frame with the frame drawn and not yet presented, which is
// the only moment the finished image can be read back. NULL to unhook.
typedef void (*GfxPreSwapCallback)(void);
void gfx_set_pre_swap_callback(GfxPreSwapCallback cb);

// Reads a rect of the window's back buffer into rgb as tightly packed RGB
// triples, bottom row first. Only meaningful from a pre-swap callback.
//
// Synchronous: it waits for the GPU to finish the frame and the driver repacks
// every row, RGB being nobody's native layout. That is the right trade for one
// screenshot and the wrong one sixty times a second - see gfx_capture_start().
bool gfx_read_screen_pixels(int x, int y, int width, int height, void *rgb);

/**
 * Streaming capture of the back buffer, for the video recorder.
 *
 * Where gfx_read_screen_pixels() stalls the pipeline until the frame it asked
 * for has landed, this reads into a ring of pixel buffer objects and hands back
 * the frame from the call before: the GPU is given the whole frame to do the
 * copy in and nothing waits on it. The cost is that what comes out is one frame
 * behind, which is not a sync problem - a fixed rate stream timestamps by frame
 * index, and no index is skipped.
 *
 * The format is whatever the driver reads back without repacking, which is BGRA
 * everywhere that matters, and it is reported rather than converted: four byte
 * pixels are also what a GPU encoder wants uploaded.
 *
 * All of these are only valid on the thread holding the GL context.
 */
#define GFX_CAPTURE_NONE 0
#define GFX_CAPTURE_BGRA 1
#define GFX_CAPTURE_RGBA 2

// Returns the GFX_CAPTURE_ format frames will arrive in, or GFX_CAPTURE_NONE if
// the size is bad. Replaces any capture already running.
int gfx_capture_start(int width, int height);

// Issues this frame's read and copies out the previous one, into width*height*4
// bytes at dst, bottom row first. False when there is nothing ready yet, which
// is every call until the ring has filled.
bool gfx_capture_read(void *dst);

// The frames still in flight, newest last, so the recording does not lose its
// tail to the ring. False if there were none.
bool gfx_capture_drain(void *dst);

void gfx_capture_stop(void);

#endif

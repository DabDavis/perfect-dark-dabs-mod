#define NOMINMAX

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cstdarg>

#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <list>
#include <stack>
#include <string>
#include <iostream>
#include <memory>
#include <limits>
#include <algorithm>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "platform.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"

#include "texpack.h"
#include "xblatex.h"
#include "handtint.h"
#include "xblafont.h"
#include "menuimage.h"
#include "gfx_texscale.h"
#include "gfx_post.h"
#include "gfxmesh.h"

uintptr_t gfxFramebuffer;

#define ALIGN(x, a) (((x) + (a - 1)) & ~(a - 1))

#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_)*0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_)*0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

// SCREEN_WIDTH and SCREEN_HEIGHT are defined in the headerfile
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2.f)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2.f)

#define RATIO_X (gfx_current_dimensions.width / (float)SCREEN_WIDTH)
#define RATIO_Y (gfx_current_dimensions.height / (float)SCREEN_HEIGHT)

// The most triangles that can sit in buf_vbo waiting for a draw call.
//
// Left at what fast3d shipped with, because measuring said to. The guess was
// that a stage full of bodies would be flushing on this cap constantly; it
// never reached it once. Every draw call in a room of five hundred bodies was
// ended by a texture change instead - about 5000 of them a frame against 950
// distinct textures - and the cap contributed none. g_GfxMaxBufferedTris still
// makes it tunable from the command line for anyone who wants to check again.
#define MAX_BUFFERED 256
#define MAX_LIGHTS 4
#define MAX_VERTICES 128
#define MAX_VERTEX_COLORS 64

#define TEXTURE_CACHE_MAX_SIZE 1024

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

struct RGBA {
    uint8_t r, g, b, a;
};

/*
 * Four floats the compiler carries in one register (SSE on x86, NEON on
 * ARM): GCC and clang's vector extension, so no intrinsics and no
 * per-target code. Each lane rounds exactly as the scalar expression it
 * replaces, so a vertex transformed this way lands on the same bits.
 */
typedef float v4f __attribute__((vector_size(16)));

static inline v4f v4f_load(const float* p) {
    v4f v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void v4f_store(float* p, v4f v) {
    memcpy(p, &v, sizeof(v));
}

static inline v4f v4f_splat(float f) {
    return v4f{ f, f, f, f };
}

/*
 * c / 255.0f for every byte, computed once with the division so the
 * per-vertex colour reaches the shader on the same bits it always did,
 * without the divide.
 */
static const struct ByteToUnit {
    float f[256];
    ByteToUnit() {
        for (int i = 0; i < 256; i++) {
            f[i] = i / 255.0f;
        }
    }
} byte_unit;

struct NormalColor {
    union {
        struct { uint8_t r, g, b, a; };
        struct { int8_t x, y, z, w; };
    };
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t fog;
    uint8_t clip_rej;
    // The RSP's fog line at the time the vertex was loaded, for the fragment
    // shader to evaluate at its own depth: factor = z/w * mul + offset. A
    // per-vertex factor interpolated across a triangle that starts behind
    // the camera is wrong along most of it - the N64 clips first and
    // evaluates the line at the new vertices, so this goes one better.
    // Floats: G_SETFOGLINE_EXT sets a line past gSPFogFactor()'s s16s
    float fog_mul, fog_offset;
    // G_ENVMAP_EXT only: the vertex's normal (its colour, read as the signed
    // normal an RSP light would read) and its position, both put through the
    // modelview, so in view space with the eye at the origin. Written only
    // while the mode is on; see gfx_sp_load_vertex().
    float env[6];
};

static struct {
    TextureCacheMap map;
    std::list<TextureCacheMapIter> lru;
    std::vector<uint32_t> free_texture_ids;

    // Self-sizing; see g_GfxTexCacheSize
    uint32_t frame;           // counts gfx_start_frame()s, for last_frame
    uint32_t max_entries;     // what the backend can hold, less room for the rest
    uint64_t budget;          // bytes the cache may hold before it stops growing
    uint64_t bytes;           // what the entries hold now
    uint32_t peak, grows, evictions;
    bool limits_known;
} gfx_texture_cache = { {}, {}, {}, 0, 8192, 1024ull << 20, 0, 0, 0, 0, false };

// A texture drawn this recently is one the frame still wants: two seconds, so
// a working set drawn at 30 frames a second or split over two views counts.
#define GFX_TEXCACHE_HOT_FRAMES 120
// Past the starting size, a texture nothing has drawn for this long goes.
#define GFX_TEXCACHE_IDLE_FRAMES 1800
// At most this many of those a frame, and of the texture names nothing holds.
#define GFX_TEXCACHE_TRIM_PER_FRAME 32
// Names kept back for the next misses rather than handed to the backend.
#define GFX_TEXCACHE_SPARE_IDS 256
// The cache never sizes itself below this, whatever --gfxtexcache says.
#define GFX_TEXCACHE_MIN_SIZE 8

// The entry import_texture() is filling, for gfx_upload_texture() to charge.
static TextureCacheNode* gfx_texture_cache_filling;

struct ColorCombiner {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    struct ShaderProgram* prg[32]; // 16 clamp combinations, twice: the second half with SHADER_OPT_TEXT_OUTLINE
    uint8_t shader_input_mapping[2][7];
};

static std::map<ColorCombinerKey, struct ColorCombiner> color_combiner_pool;
static std::map<ColorCombinerKey, struct ColorCombiner>::iterator prev_combiner = color_combiner_pool.end();

static uint8_t* tex_upload_buffer = nullptr;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];


    Light_t lookat[2];
    bool lookat_enabled;

    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights;        // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    // G_MW_FOG's s16 pair, or G_SETFOGLINE_EXT's floats, and whether the
    // line is evaluated at the eye depth (linear fog) instead of at z/w
    float fog_mul, fog_offset;
    bool fog_linear;

    uint32_t extra_geometry_mode;

    // G_SETTEXGENSHIFT_EXT: added to the texgen's s and t under G_TEXGEN_EYE_EXT,
    // in the texgen's own units (a normal's whole range is one)
    float texgen_shift[2];

    // the shift as a turn (G_TEXGEN_TURN_EXT): cos and sin of the yaw, then of
    // the pitch
    float texgen_turn[4] = { 1.0f, 0.0f, 1.0f, 0.0f };

    uint32_t aspect_mode;
    float aspect_ofs;
    float aspect_scale;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];

    const struct NormalColor *vertex_colors; //[MAX_VERTEX_COLORS];
} rsp;

struct RawTexMetadata {
    uint16_t width, height;
    float h_byte_scale = 1, v_pixel_scale = 1;
};

struct LoadedTexture {
    const uint8_t* addr;
    uint32_t orig_size_bytes;
    uint32_t full_size_bytes; // full_image_line_size_bytes * height
    uint32_t size_bytes; // line_size_bytes * height
    uint32_t full_image_line_size_bytes;
    uint32_t line_size_bytes;
    uint32_t tex_flags;
    uint32_t glyph;      // gDPSetFontGlyphEXT, 0 when this is not a font glyph
    struct RawTexMetadata raw_tex_metadata;
};

static struct RDP {
    // Set by gDPSetFontGlyphEXT and taken by the next gDPSetTextureImage, which
    // is the one it describes. Cleared there either way, so it can never carry
    // over onto a texture that is not a glyph.
    uint32_t pending_glyph;
    uint16_t palette[256];
    const uint8_t* palette_addrs[2];
    uint32_t palette_fmt;
    struct {
        const uint8_t* addr;
        uint8_t siz;
        uint32_t width;
        uint32_t tex_flags;
        uint32_t glyph;
        struct RawTexMetadata raw_tex_metadata;
    } texture_to_load;
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint8_t shifts, shiftt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        float ofs_s, ofs_t;          // G_SETTILEOFFSET_EXT: texels past uls and ult, finer than a quarter
        uint16_t width, height;      // in texels
        uint16_t tmem;               // 0-511, in 64-bit word units
        uint32_t line_size_bytes;
        uint8_t palette;
    } texture_tile[8];
    LoadedTexture loaded_texture[512]; // for each tmem location
    bool textures_changed[2];

    uint8_t first_tile_index;
    uint8_t tex_min_lod;
    uint8_t tex_max_lod;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;
    bool tex_lod;
    bool tex_detail;

    uint8_t prim_lod_fraction;
    struct RGBA env_color, prim_color, fog_color, fill_color, grayscale_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void* z_buf_address;
    void* color_image_address;

    int16_t subpixel_ofs_x;
    int16_t subpixel_ofs_y;

    // G_SETRECTDEPTH_EXT: rectangles drawn at this normalised depth and tested
    // against the scene without writing, instead of in front of everything
    bool rect_depth_on;
    float rect_depth;

    // G_SETDEPTHBIAS_EXT: triangles pushed away from the eye by this many of
    // the depth buffer's smallest steps
    int16_t depth_bias;
} rdp;

static struct RenderingState {
    uint32_t depth_mode;
    bool alpha_blend;
    bool modulate;
    bool additive;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram* shader_program;
    TextureCacheNode* textures[SHADER_MAX_TEXTURES];
} rendering_state;

/**
 * Everything gfx_sp_tri1 works out that depends only on RDP/RSP state and not
 * on the triangle in hand.
 *
 * A batch is about sixteen triangles - 78k triangles against 5000 draws - so
 * all of this was being derived roughly sixteen times more often than it
 * changed: a std::map lookup for the colour combiner, two tile-geometry blocks
 * with integer divides in them, and a shader lookup, per triangle. The texture
 * coordinate scaling was worse still, redone per vertex per texture, three
 * times over for every triangle.
 *
 * The rule for what may live here: it must be derived from state that
 * gfx_mark_state_dirty() is called for. Comparisons against rendering_state
 * deliberately stay in gfx_sp_tri1, because rendering_state moves underneath
 * us for reasons this flag does not track; they are a pointer compare each and
 * cost nothing.
 */
static struct BatchState {
    struct ColorCombiner* comb;
    struct ShaderProgram* prg;
    struct GfxClipParameters clip_parameters;
    uint8_t num_inputs;
    bool used_textures[2]; // as the shader sees them, which is not comb->used_textures
    uint32_t tm;

    /**
     * A raw s/t off the vertex becomes a normalised texture coordinate with a
     * single multiply-add: out = raw * uv_scale + uv_ofs. Rectangles skip the
     * perspective halving and the linear-filter half-texel, so they get their
     * own pair. Index is [texture][0 for s, 1 for t].
     */
    float uv_scale[2][2], uv_ofs[2][2];
    float uv_scale_rect[2][2], uv_ofs_rect[2][2];
    float tex_clamp[2][2]; // (size2 - 0.5) / size, emitted when tm asks for it
    uint32_t tex_size[2][2]; // [texture][0 = width, 1 = height], kept for GFX_VERIFY_BATCH_STATE

    bool use_alpha, use_fog, use_grayscale, use_modulate, use_additive, use_envmap;
    bool fog_vertex; // SHADER_OPT_FOG_VERTEX: the fog slot carries per-vertex factors
} batch;

/**
 * Set whenever anything gfx_derive_batch_state() reads may have moved. Hooked
 * at whole-setter granularity rather than at each assignment: the setters own
 * their fields, so there is no way to change one and miss the flag.
 * Over-setting it only costs a recompute, under-setting it renders wrong, so
 * when in doubt it gets set.
 */
static bool batch_state_dirty = true;
static bool emit_plan_dirty = true; // the per-vertex layout below gfx_resolve_emit_inputs

static inline void gfx_mark_state_dirty(void) {
    batch_state_dirty = true;
}

struct GfxDimensions gfx_current_window_dimensions;
int32_t gfx_current_window_position_x;
int32_t gfx_current_window_position_y;
struct GfxDimensions gfx_current_dimensions;
static struct GfxDimensions gfx_prev_dimensions;
struct XYWidthHeight gfx_current_game_window_viewport;
struct XYWidthHeight gfx_current_native_viewport;
float gfx_current_native_aspect = 4.f / 3.f;
bool gfx_framebuffers_enabled = true;
bool gfx_detail_textures_enabled = true;
bool gfx_clean_text_outlines = true;
int gfx_clamped_edge_mode = CLAMPED_EDGE_STRETCH;
int gfx_texture_enhance_scale = 1;
int gfx_text_smooth_scale = 1;
float gfx_color_saturation = 1.0f;
float gfx_color_contrast = 1.0f;
float gfx_color_black_level = 0.0f;
bool gfx_post_smaa = false;
float gfx_render_scale = 1.0f;
#define GFX_MAX_RENDER_SIDE 8192u
float gfx_fsr_sharpness = 0.2f;
bool gfx_taa = false;

/*
 * TAA (gSPTaaEXT). Between a player's BEGIN and END every vertex is moved by
 * a sub-pixel jitter (Halton 2,3 over eight frames), and END resolves that
 * player's viewport against its last frame. The camera is all the motion
 * there is to go on: the game's world -> clip matrix for this frame and last
 * frame's (per player, so split screen keeps four histories) take a pixel
 * and its depth back to where it was drawn last frame.
 */
struct GfxTaaHistory {
    double mtx[16];
    uint32_t frame;
    uint32_t width, height;
    bool valid;
};
static bool taa_active;
static float taa_jx, taa_jy;
static int taa_slot;
static double taa_mtx[16];
static struct XYWidthHeight taa_viewport;
static float taa_aspect_k, taa_aspect_o;
static GfxTaaHistory taa_history[4];
static bool taa_failed; // the backend could not; no more jitter this run

static bool game_renders_to_framebuffer;
static bool game_post_processes; // through the backend's post_process() to the window
static int game_framebuffer;
static int game_framebuffer_msaa_resolved;

uint32_t gfx_msaa_level = 1;
uint32_t gfx_max_msaa_level = 1;

static bool dropped_frame;
static GfxPreSwapCallback gfx_pre_swap_callback;

static float buf_vbo[MAX_BUFFERED * (32 * 3)]; // 3 vertices in a triangle and 32 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

extern "C" {

/**
 * How many triangles may be batched into one draw call. Lower it from gdb to
 * compare against the value fast3d shipped with:
 *
 *     set g_GfxMaxBufferedTris = 256
 *
 * Clamped to MAX_BUFFERED, which is what buf_vbo is actually sized for.
 */
uint32_t g_GfxMaxBufferedTris = MAX_BUFFERED;

/**
 * What the last frame cost the renderer.
 *
 * These answer the question a stage full of bodies raises, which is whether
 * the frame is bound by transforming vertices or by issuing draw calls, and
 * those want opposite fixes. The one that tells them apart is
 * g_GfxNumBufferFullFlushes: a draw call fast3d made because buf_vbo filled
 * up, rather than because the render state changed. If most of the draw calls
 * are those, the batch size is the limit and raising g_GfxMaxBufferedTris is
 * the whole fix; if hardly any are, the frame is being cut into pieces by
 * texture and combiner changes and a bigger buffer will not help.
 *
 * Read them from gdb, or set g_GfxLogStats to a frame count to have a line
 * printed that often:
 *
 *     set g_GfxLogStats = 60
 */
uint32_t g_GfxNumDrawCalls = 0;
uint32_t g_GfxNumBufferFullFlushes = 0;

/**
 * Draw calls of the last frame, by the thing that ended the batch.
 *
 * A frame of bodies comes out at seven triangles a draw call, so what matters
 * is not how much geometry there is but what keeps cutting it up. Indexed by
 * enum GfxFlushReason; only flushes that actually drew something are counted,
 * since a flush with an empty buffer costs nothing.
 */
uint32_t g_GfxFlushReasons[GFX_FLUSH_COUNT] = {0};

/**
 * How many *distinct* textures were bound in the last frame, against the
 * number of binds.
 *
 * This is the number that decides what to do about a frame spent almost
 * entirely on texture binds. If the distinct count is small, the same handful
 * of textures are being rebound over and over because the draw order walks one
 * body at a time, and sorting the opaque pass by texture collapses it. If it
 * is close to the bind count, the textures really are all different and the
 * answer is an atlas or an array instead.
 */
uint32_t g_GfxNumDistinctTextures = 0;

/**
 * Textures decoded and uploaded to the GPU during the last frame, and cache
 * entries thrown out to make room for them.
 *
 * The texture cache holds g_GfxTexCacheSize entries and evicts least recently
 * used. That is fine while a frame's working set fits. Once it does not, the
 * cache is being asked for more distinct textures than it can hold and every
 * one of them evicts another that the same frame is about to want again - so a
 * frame stops binding textures it already has and starts decoding and
 * reuploading them from scratch, hundreds of times, every frame. That is a
 * cliff rather than a slope, and these two numbers are what it looks like:
 * uploads climbing to meet the bind count, and evictions alongside them.
 *
 * A frame in a room full of bodies was seen holding 879 distinct textures
 * against a cache of 1024, and still climbing.
 */
uint32_t g_GfxNumTexUploads = 0;
uint32_t g_GfxNumTexEvictions = 0;

/**
 * How many textures the cache may hold right now.
 *
 * It sizes itself. It starts at TEXTURE_CACHE_MAX_SIZE and, when making room
 * would throw out a texture drawn within the last GFX_TEXCACHE_HOT_FRAMES
 * frames - the working set no longer fits, which is the cliff described above
 * - it grows instead of evicting, up to what the backend can hold and a byte
 * budget taken from the card's video memory. A texture nothing has drawn for
 * GFX_TEXCACHE_IDLE_FRAMES is let go a few a frame, bringing the size back
 * down, and emptying the cache (a stage change) puts it back at the start.
 * A stage in the HD look with a texture pack on was seen pinned at 1024 of
 * 1024 (F3 20260929-045717), which this makes a size rather than a limit.
 *
 * --gfxtexcache N fixes it at N (g_GfxTexCacheFixed), for measuring.
 */
uint32_t g_GfxTexCacheSize = TEXTURE_CACHE_MAX_SIZE;
uint32_t g_GfxTexCacheFixed = 0;
uint32_t g_GfxNumTris = 0;

/*
 * GFX_VERIFY_BATCH_STATE tallies, read live over gdb. These deliberately do NOT
 * reset per frame like the stats counters above: they accumulate for the whole
 * session.
 *
 * The *Checks counters matter as much as the failure counters. A zero failure
 * count only means anything alongside a large check count - otherwise it is
 * indistinguishable from a verifier that never ran on real geometry, which is
 * exactly how a headless boot that never leaves the menus looks.
 */
uint32_t g_GfxVerifyBatchChecks = 0;
uint32_t g_GfxVerifyBatchStale = 0;
uint32_t g_GfxVerifyUvChecks = 0;
uint32_t g_GfxVerifyUvDrift = 0;
float g_GfxVerifyUvWorst = 0.f;   // largest absolute divergence seen, in texture-coordinate units
uint32_t g_GfxVerifyMaxTrisFrame = 0; // busiest frame the verifier actually saw
uint32_t g_GfxNumVerts = 0;
uint32_t g_GfxLogStats = 0;
// triangles of the last frame by fate: thrown out as wholly off screen, as
// facing away, or drawn
uint32_t g_GfxTrisClipped = 0, g_GfxTrisCulled = 0;

}

static struct GfxWindowManagerAPI* gfx_wapi;
static struct GfxRenderingAPI* gfx_rapi;

static uintptr_t segmentPointers[16];

struct FBInfo {
    uint32_t orig_width, orig_height;
    uint32_t applied_width, applied_height;
    bool upscale, autoresize;
};

static bool fbActive = 0;
static std::map<int, FBInfo>::iterator active_fb;
static std::map<int, FBInfo> framebuffers;

static constexpr float clampf(const float x, const float min, const float max) {
    return (x < min) ? min : (x > max) ? max : x;
}

// Texture ids bound this frame, for g_GfxNumDistinctTextures. Cleared each
// frame; a plain set because this only runs while stats are switched on.
static std::set<uint32_t> gfx_frame_textures;

static void gfx_note_texture_bound(uint32_t texture_id) {
    if (g_GfxLogStats) {
        gfx_frame_textures.insert(texture_id);
        g_GfxNumDistinctTextures = gfx_frame_textures.size();
    }
}

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        g_GfxNumDrawCalls++;
        g_GfxNumTris += buf_vbo_num_tris;
#ifdef GFX_VERIFY_BATCH_STATE
        if (g_GfxNumTris > g_GfxVerifyMaxTrisFrame) {
            g_GfxVerifyMaxTrisFrame = g_GfxNumTris; // g_GfxNumTris resets each frame, so this tracks the peak
        }
#endif
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
}

/**
 * Flush, recording what caused it. Only the sites that can fire per model are
 * tagged; the rest are once-a-frame things and fall under GFX_FLUSH_OTHER.
 */
static void gfx_flush_for(enum GfxFlushReason reason) {
    if (buf_vbo_len > 0) {
        g_GfxFlushReasons[reason]++;
    }
    gfx_flush();
}

static struct ShaderProgram* gfx_lookup_or_create_shader_program(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram* prg = gfx_rapi->lookup_shader(shader_id0, shader_id1);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id0, shader_id1);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static const char* ccmux_to_string(uint32_t ccmux) {
    static const char* const tbl[] = {
        "G_CCMUX_COMBINED",
        "G_CCMUX_TEXEL0",
        "G_CCMUX_TEXEL1",
        "G_CCMUX_PRIMITIVE",
        "G_CCMUX_SHADE",
        "G_CCMUX_ENVIRONMENT",
        "G_CCMUX_1",
        "G_CCMUX_COMBINED_ALPHA",
        "G_CCMUX_TEXEL0_ALPHA",
        "G_CCMUX_TEXEL1_ALPHA",
        "G_CCMUX_PRIMITIVE_ALPHA",
        "G_CCMUX_SHADE_ALPHA",
        "G_CCMUX_ENV_ALPHA",
        "G_CCMUX_LOD_FRACTION",
        "G_CCMUX_PRIM_LOD_FRAC",
        "G_CCMUX_K5",
    };
    if (ccmux > 15) {
        return "G_CCMUX_0";

    } else {
        return tbl[ccmux];
    }
}

static const char* acmux_to_string(uint32_t acmux) {
    static const char* const tbl[] = {
        "G_ACMUX_COMBINED or G_ACMUX_LOD_FRACTION",
        "G_ACMUX_TEXEL0",
        "G_ACMUX_TEXEL1",
        "G_ACMUX_PRIMITIVE",
        "G_ACMUX_SHADE",
        "G_ACMUX_ENVIRONMENT",
        "G_ACMUX_1 or G_ACMUX_PRIM_LOD_FRAC",
        "G_ACMUX_0",
    };
    return tbl[acmux];
}

static void gfx_generate_cc(struct ColorCombiner* comb, const ColorCombinerKey& key) {
    bool is_2cyc = (key.options & (uint64_t)SHADER_OPT_2CYC) != 0;

    uint8_t c[2][2][4] = { { { 0 } } };
    uint64_t shader_id0 = 0;
    uint32_t shader_id1 = key.options;
    uint8_t shader_input_mapping[2][7] = { { 0 } };
    bool used_textures[2] = { false, false };
    for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
        uint32_t rgb_a = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgb_b = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgb_c = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgb_d = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alpha_a = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alpha_b = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alpha_c = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alpha_d = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgb_a >= 8) {
            rgb_a = G_CCMUX_0;
        }
        if (rgb_b >= 8) {
            rgb_b = G_CCMUX_0;
        }
        if (rgb_c >= 16) {
            rgb_c = G_CCMUX_0;
        }
        if (rgb_d == 7) {
            rgb_d = G_CCMUX_0;
        }

        if (rgb_a == rgb_b || rgb_c == G_CCMUX_0) {
            // Normalize
            rgb_a = G_CCMUX_0;
            rgb_b = G_CCMUX_0;
            rgb_c = G_CCMUX_0;
        }
        if (alpha_a == alpha_b || alpha_c == G_ACMUX_0) {
            // Normalize
            alpha_a = G_ACMUX_0;
            alpha_b = G_ACMUX_0;
            alpha_c = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgb_a != G_CCMUX_COMBINED && rgb_b != G_CCMUX_COMBINED && rgb_c != G_CCMUX_COMBINED &&
                rgb_d != G_CCMUX_COMBINED) {
                // First cycle RGB not used, so clear it away
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgb_c != G_CCMUX_COMBINED_ALPHA && alpha_a != G_ACMUX_COMBINED && alpha_b != G_ACMUX_COMBINED &&
                alpha_d != G_ACMUX_COMBINED) {
                // First cycle ALPHA not used, so clear it away
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }

        c[i][0][0] = rgb_a;
        c[i][0][1] = rgb_b;
        c[i][0][2] = rgb_c;
        c[i][0][3] = rgb_d;
        c[i][1][0] = alpha_a;
        c[i][1][1] = alpha_b;
        c[i][1][2] = alpha_c;
        c[i][1][3] = alpha_d;
    }
    if (!is_2cyc) {
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < 4; k++) {
                c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
            }
        }
    }
    {
        uint8_t input_number[32] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0:
                        val = SHADER_0;
                        break;
                    case G_CCMUX_1:
                        val = SHADER_1;
                        break;
                    case G_CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_SHADE_ALPHA:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                        if (input_number[c[i][0][j]] == 0) {
                            shader_input_mapping[0][next_input_number - 1] = c[i][0][j];
                            input_number[c[i][0][j]] = next_input_number++;
                        }
                        val = input_number[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        sysLogPrintf(LOG_WARNING, "Unsupported ccmux: %d", c[i][0][j]);
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t input_number[16] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0:
                        val = SHADER_0;
                        break;
                    case G_ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_ACMUX_LOD_FRACTION:
                        // case G_ACMUX_COMBINED: same numerical value
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]]; // for G_ACMUX_LOD_FRACTION
                    case G_ACMUX_1:
                        // case G_ACMUX_PRIM_LOD_FRAC: same numerical value
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]]; // for G_ACMUX_PRIM_LOD_FRAC
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (input_number[c[i][1][j]] == 0) {
                            shader_input_mapping[1][next_input_number - 1] = c[i][1][j];
                            input_number[c[i][1][j]] = next_input_number++;
                        }
                        val = input_number[c[i][1][j]];
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shader_id0;
    comb->shader_id1 = shader_id1;
    comb->used_textures[0] = used_textures[0];
    comb->used_textures[1] = used_textures[1];
    // comb->prg = gfx_lookup_or_create_shader_program(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner* gfx_lookup_or_create_color_combiner(const ColorCombinerKey& key) {
    if (prev_combiner != color_combiner_pool.end() && prev_combiner->first == key) {
        return &prev_combiner->second;
    }

    prev_combiner = color_combiner_pool.find(key);
    if (prev_combiner != color_combiner_pool.end()) {
        return &prev_combiner->second;
    }
    gfx_flush_for(GFX_FLUSH_OTHER);
    prev_combiner = color_combiner_pool.insert(std::make_pair(key, ColorCombiner())).first;
    gfx_generate_cc(&prev_combiner->second, key);
    return &prev_combiner->second;
}

// Takes one entry out of the cache, its texture name kept for the next miss.
// rendering_state.textures points into the map, so a tile left naming the
// entry is let go too and imports afresh at its next draw.
static TextureCacheMap::iterator gfx_texture_cache_forget(TextureCacheMap::iterator it) {
    for (int t = 0; t < 2; t++) {
        if (rendering_state.textures[t] == &*it) {
            rendering_state.textures[t] = nullptr;
            rdp.textures_changed[t] = true;
        }
    }
    if (gfx_texture_cache_filling == &*it) {
        gfx_texture_cache_filling = nullptr;
    }
    gfx_texture_cache.bytes -= std::min<uint64_t>(gfx_texture_cache.bytes, it->second.bytes);
    gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
    gfx_texture_cache.lru.erase(it->second.lru_location);
    return gfx_texture_cache.map.erase(it);
}

// What the cache may grow to, asked of the backend once it is up.
static void gfx_texture_cache_limits(void) {
    uint32_t max_textures = 16384;
    uint64_t vram = 0;

    if (gfx_texture_cache.limits_known) {
        return;
    }
    gfx_texture_cache.limits_known = true;

    if (gfx_rapi->get_texture_limits) {
        gfx_rapi->get_texture_limits(&max_textures, &vram);
    }

    // Past a texture's own name, a re-upload holds a second image until the
    // GPU is done with the first, and the framebuffers take names too, so a
    // quarter is left over.
    gfx_texture_cache.max_entries = std::max<uint32_t>(TEXTURE_CACHE_MAX_SIZE, std::min<uint32_t>(16384, max_textures / 4 * 3));

    // A quarter of the card, and never less than the old fixed 1024 entries
    // were seen to use with a pack on, nor more than 3GB. The pack's decoded
    // images are held in main memory on their own budget (texpack.c).
    const uint64_t mb = 1ull << 20;
    gfx_texture_cache.budget = vram ? std::min<uint64_t>(3072 * mb, std::max<uint64_t>(512 * mb, vram / 4)) : 1024 * mb;

    sysLogPrintf(LOG_NOTE, "gfx: texture cache starts at %u, may grow to %u textures or %u MB (video memory %u MB)%s",
                 g_GfxTexCacheSize, gfx_texture_cache.max_entries, (uint32_t)(gfx_texture_cache.budget / mb),
                 (uint32_t)(vram / mb), g_GfxTexCacheFixed ? "; fixed by --gfxtexcache" : "");
}

// Once a frame, before anything is drawn: textures left behind go, a few at a
// time, and so do texture names nothing holds - never enough in one frame to
// be seen.
static void gfx_texture_cache_frame(void) {
    gfx_texture_cache.frame++;

    if (!g_GfxTexCacheFixed && g_GfxTexCacheSize > TEXTURE_CACHE_MAX_SIZE) {
        uint32_t trimmed = 0;

        while (trimmed < GFX_TEXCACHE_TRIM_PER_FRAME && gfx_texture_cache.map.size() > TEXTURE_CACHE_MAX_SIZE &&
               gfx_texture_cache.frame - gfx_texture_cache.lru.front().it->second.last_frame > GFX_TEXCACHE_IDLE_FRAMES) {
            gfx_texture_cache_forget(gfx_texture_cache.lru.front().it);
            trimmed++;
        }

        // Down to what is left, and to the starting size once that holds it
        if (trimmed || gfx_texture_cache.map.size() <= TEXTURE_CACHE_MAX_SIZE) {
            g_GfxTexCacheSize = std::max<uint32_t>(TEXTURE_CACHE_MAX_SIZE, (uint32_t)gfx_texture_cache.map.size());
        }
    }

    for (uint32_t n = 0; n < GFX_TEXCACHE_TRIM_PER_FRAME && gfx_texture_cache.free_texture_ids.size() > GFX_TEXCACHE_SPARE_IDS; n++) {
        gfx_rapi->delete_texture(gfx_texture_cache.free_texture_ids.back());
        gfx_texture_cache.free_texture_ids.pop_back();
    }
}

void gfx_texture_cache_clear() {
    gfx_mark_state_dirty();
    gfx_flush_for(GFX_FLUSH_OTHER);
    for (const auto& entry : gfx_texture_cache.map) {
        gfx_texture_cache.free_texture_ids.push_back(entry.second.texture_id);
    }
    gfx_texture_cache.map.clear();
    gfx_texture_cache.lru.clear();
    gfx_texture_cache.bytes = 0;
    gfx_texture_cache_filling = nullptr;
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    memset(rendering_state.textures, 0, sizeof(rendering_state.textures));

    // Empty, so back to the starting size: whatever the next stage draws
    // grows it again without a single texture being thrown out for it.
    if (!g_GfxTexCacheFixed) {
        g_GfxTexCacheSize = TEXTURE_CACHE_MAX_SIZE;
    }
}

/**
 * Drops the cache entries holding the original of a texture whose replacement
 * has just been decoded.
 *
 * The lookup at the top of import_texture() answers before the pack is ever
 * consulted, so an entry uploaded while the decode was still queued would keep
 * the original on screen forever. Erasing it makes the next draw a miss, which
 * is what asks the pack again - and by then texpackClaimDecoded() has the image
 * in hand. One texture number can be at more than one address, so this goes by
 * what the address resolves to rather than by the address itself.
 */
static void gfx_texture_cache_drop_texnum(int32_t texturenum) {
    bool dropped = false;

    // An XBLA mesh's texture is the one entry that has to be dropped while it
    // is already `replaced`: the release's own art is a replacement too, and it
    // is what the entry has been showing while the player's own picture for
    // that record decoded. There is only ever one stand-in address per record,
    // so the ping-pong the flag is there to stop cannot happen here anyway.
    const int32_t xbla_record = texpackXblaRecordFromId(texturenum);

    for (TextureCacheMap::iterator it = gfx_texture_cache.map.begin();
            it != gfx_texture_cache.map.end(); ) {
        // An entry already showing the replacement has nothing to drop. One
        // texture number at two addresses is what this is for: without it the
        // entry uploaded from the first decode went out with the other one's
        // original, and the two took turns re-queueing the decode.
        if (it->second.replaced && xbla_record < 0) {
            ++it;
            continue;
        }

        // A glyph has no texture number, so it is matched by what the display
        // list called it instead, and a record of the release's own textures by
        // the stand-in tile its list binds.
        const bool hit = xbla_record >= 0
                ? xblaTexRecordOf(it->first.texture_addr) == xbla_record
                : it->first.glyph
                ? texpackDecodedIsGlyph(texturenum, it->first.glyph) != 0
                : texpackGetTextureNum(it->first.texture_addr) == texturenum;

        if (hit) {
            it = gfx_texture_cache_forget(it);
            dropped = true;
        } else {
            ++it;
        }
    }

    if (dropped) {
        // rendering_state.textures holds pointers into the map, and the nodes
        // they name may be the ones just erased.
        gfx_mark_state_dirty();
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
        memset(rendering_state.textures, 0, sizeof(rendering_state.textures));
    }
}

static struct GfxTraceStats g_GfxLastFrame;

static inline void *seg_addr(uintptr_t w1);

extern "C" int gfx_trace_texture_entries(const void *addr, struct GfxTraceTexEntry *out, int max) {
    TextureCacheKey probe = { (const uint8_t *)addr, { 0 }, 0, 0 }; // the bucket is the address's alone
    int count = 0;

    if (gfx_texture_cache.map.bucket_count() == 0) {
        return 0;
    }

    const size_t bucket = gfx_texture_cache.map.bucket(probe);

    for (auto it = gfx_texture_cache.map.begin(bucket); it != gfx_texture_cache.map.end(bucket); ++it) {
        if (it->first.texture_addr != (const uint8_t *)addr) {
            continue;
        }

        if (count < max) {
            struct GfxTraceTexEntry *e = &out[count];
            e->palette = it->first.palette_addrs[0];
            e->palette1 = it->first.palette_addrs[1];
            e->glyph = it->first.glyph;
            e->fmt = it->first.fmt;
            e->siz = it->first.siz;
            e->palindex = it->first.palette_index;
            e->source = it->second.source;
            e->width = it->second.width;
            e->height = it->second.height;
            e->upload_frame = it->second.upload_frame;
            e->last_frame = it->second.last_frame;
        }

        count++;
    }

    return count;
}

extern "C" uint32_t gfx_trace_frame(void) {
    return gfx_texture_cache.frame;
}

extern "C" const void *gfx_trace_seg_addr(uintptr_t w1) {
    return seg_addr(w1);
}

extern "C" void gfx_trace_stats(struct GfxTraceStats *out) {
    *out = g_GfxLastFrame;
    out->cacheentries = (uint32_t)gfx_texture_cache.map.size();
    out->cachesize = g_GfxTexCacheSize;
    out->cachepeak = gfx_texture_cache.peak;
    out->cachegrows = gfx_texture_cache.grows;
    out->cacheevictions = gfx_texture_cache.evictions;
    out->cachemb = (uint32_t)((gfx_texture_cache.bytes + (1u << 19)) >> 20);
    out->cachebudgetmb = (uint32_t)(gfx_texture_cache.budget >> 20);
    out->cachemax = g_GfxTexCacheFixed ? g_GfxTexCacheSize : gfx_texture_cache.max_entries;
    out->cachefixed = g_GfxTexCacheFixed;
}

extern "C" void gfx_texpack_poll(void) {
    int32_t ready[32];
    const int32_t count = texpackPollDecoded(ready, (int32_t)(sizeof(ready) / sizeof(ready[0])));
    for (int32_t i = 0; i < count; i++) {
        gfx_texture_cache_drop_texnum(ready[i]);
    }
}

static bool gfx_texture_cache_lookup(int i, const TextureCacheKey& key) {
    TextureCacheMap::iterator it = gfx_texture_cache.map.find(key);
    TextureCacheNode** n = &rendering_state.textures[i];

    if (it != gfx_texture_cache.map.end()) {
        it->second.last_frame = gfx_texture_cache.frame;
        gfx_note_texture_bound(it->second.texture_id);
        gfx_rapi->select_texture(i, it->second.texture_id, it->second.linear_filter);
        *n = &*it;
        gfx_texture_cache.lru.splice(gfx_texture_cache.lru.end(), gfx_texture_cache.lru,
                                     it->second.lru_location); // move to back
        return true;
    }

    if (gfx_texture_cache.map.size() >= g_GfxTexCacheSize) {
        gfx_texture_cache_limits();

        // The least recently used goes - unless it was drawn so recently that
        // the frame will want it again, in which case the working set has
        // outgrown the cache and evicting would only trade one upload for
        // another, every frame. Then the cache grows instead, which costs
        // nothing now: the new entry takes a fresh texture name.
        const TextureCacheMap::iterator victim = gfx_texture_cache.lru.front().it;
        const bool hot = gfx_texture_cache.frame - victim->second.last_frame < GFX_TEXCACHE_HOT_FRAMES;

        if (hot && !g_GfxTexCacheFixed && g_GfxTexCacheSize < gfx_texture_cache.max_entries &&
            gfx_texture_cache.bytes < gfx_texture_cache.budget) {
            g_GfxTexCacheSize = std::min(gfx_texture_cache.max_entries, g_GfxTexCacheSize + std::max(256u, g_GfxTexCacheSize / 2));
            gfx_texture_cache.grows++;
        } else {
            g_GfxNumTexEvictions++;
            gfx_texture_cache.evictions++;
            gfx_texture_cache_forget(victim);
        }
    }

    uint32_t texture_id;
    if (!gfx_texture_cache.free_texture_ids.empty()) {
        texture_id = gfx_texture_cache.free_texture_ids.back();
        gfx_texture_cache.free_texture_ids.pop_back();
    } else {
        texture_id = gfx_rapi->new_texture();
    }

    it = gfx_texture_cache.map.insert(std::make_pair(key, TextureCacheValue())).first;
    TextureCacheNode* node = &*it;
    node->second.texture_id = texture_id;
    node->second.lru_location = gfx_texture_cache.lru.insert(gfx_texture_cache.lru.end(), { it });
    node->second.last_frame = gfx_texture_cache.frame;
    node->second.upload_frame = gfx_texture_cache.frame;
    gfx_texture_cache_filling = node;
    if (gfx_texture_cache.map.size() > gfx_texture_cache.peak) {
        gfx_texture_cache.peak = (uint32_t)gfx_texture_cache.map.size();
    }

    gfx_note_texture_bound(texture_id);
    gfx_rapi->select_texture(i, texture_id, false);
    gfx_rapi->set_sampler_parameters(i, false, 0, 0, rdp.tex_lod);
    *n = node;

    // Returning false is what makes the caller decode and upload it.
    g_GfxNumTexUploads++;

    return false;
}

void gfx_texture_cache_delete(const uint8_t* orig_addr) {
    gfx_mark_state_dirty();
    gfx_flush_for(GFX_FLUSH_OTHER);

    for (int i = 0; i < 2; ++i) {
        if (rendering_state.textures[i] && rendering_state.textures[i]->first.texture_addr == orig_addr) {
            rdp.textures_changed[i] = true;
            rendering_state.textures[i] = nullptr;
        }
    }

    while (gfx_texture_cache.map.bucket_count() > 0) {
        TextureCacheKey key = { orig_addr, { 0 }, 0, 0 }; // bucket index only depends on the address
        size_t bucket = gfx_texture_cache.map.bucket(key);
        bool again = false;
        for (auto it = gfx_texture_cache.map.begin(bucket); it != gfx_texture_cache.map.end(bucket); ++it) {
            if (it->first.texture_addr == orig_addr) {
                gfx_texture_cache_forget(gfx_texture_cache.map.find(it->first));
                again = true;
                break;
            }
        }
        if (!again) {
            break;
        }
    }
}

void gfx_texture_cache_delete_range(const uint8_t* start, const uint8_t* end) {
    gfx_mark_state_dirty();
    gfx_flush_for(GFX_FLUSH_OTHER);

    for (int i = 0; i < 2; ++i) {
        if (rendering_state.textures[i]
                && rendering_state.textures[i]->first.texture_addr >= start
                && rendering_state.textures[i]->first.texture_addr < end) {
            rdp.textures_changed[i] = true;
            rendering_state.textures[i] = nullptr;
        }
    }

    for (auto it = gfx_texture_cache.map.begin(); it != gfx_texture_cache.map.end(); ) {
        if (it->first.texture_addr >= start && it->first.texture_addr < end) {
            it = gfx_texture_cache_forget(it);
        } else {
            ++it;
        }
    }
}

// The dimensions each import_texture_* works out for itself, kept for the dump
// that happens back in import_texture() where the texture number is known.
// Width of zero means nothing was uploaded.
static uint32_t last_upload_width;
static uint32_t last_upload_height;

// Enhance Textures / Smooth Text, for the import_texture_* underneath
// import_texture(): the factor to scale the game's texels by on the way up,
// and how the edges wrap. A pack's replacement never comes through here, and
// the XBLA release's picture only where it is still the ROM's size.
static int import_enhance_scale;
static enum TexScaleEdge import_enhance_edge_s;
static enum TexScaleEdge import_enhance_edge_t;
static bool import_enhance_glyph;
static uint32_t import_enhance_tile_w; // the clamped tile, when narrower than the upload; else 0
static uint32_t import_enhance_tile_h;
static bool import_decode_only; // decode into tex_upload_buffer and stop short of the GPU

// The texture import_texture() is uploading, for handtint.c's repaint
static const uint8_t* import_tint_addr;
static std::vector<uint8_t> import_tint_buf;

// What an upload holds on the GPU, charged to the entry being filled
static void gfx_texture_cache_charge(uint32_t width, uint32_t height, bool gen_mipmaps) {
    TextureCacheNode* node = gfx_texture_cache_filling;
    if (!node) {
        return;
    }
    uint64_t bytes = (uint64_t)width * height * 4;
    if (gen_mipmaps) {
        bytes += bytes / 3;
    }
    gfx_texture_cache.bytes -= std::min<uint64_t>(gfx_texture_cache.bytes, node->second.bytes);
    node->second.bytes = (uint32_t)std::min<uint64_t>(bytes, UINT32_MAX);
    node->second.width = (uint16_t)std::min<uint32_t>(width, 0xffff);
    node->second.height = (uint16_t)std::min<uint32_t>(height, 0xffff);
    gfx_texture_cache.bytes += node->second.bytes;
}

static void gfx_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    // The dump and the dimensions the rest of the import works from stay the
    // game's; only what reaches the GPU is bigger.
    last_upload_width = width;
    last_upload_height = height;

    if (import_decode_only) {
        return;
    }

    // The first person hands, painted in the player's character's colours
    // (handtint.c): a copy, since the buffer may be the pack's or the
    // release's own, kept for the next upload of the same picture
    {
        uint8_t tint[3];

        if (import_tint_addr && handtintLookup(import_tint_addr, tint)) {
            import_tint_buf.assign(rgba32_buf, rgba32_buf + (size_t)width * height * 4);
            handtintApply(import_tint_buf.data(), width, height, tint);
            rgba32_buf = import_tint_buf.data();
        }
    }

    if (import_enhance_scale > 1) {
        const uint8_t* big = gfx_texscale(rgba32_buf, width, height, import_enhance_scale,
                                          import_enhance_edge_s, import_enhance_edge_t, import_enhance_glyph,
                                          import_enhance_tile_w, import_enhance_tile_h);
        if (big) {
            gfx_rapi->upload_texture(big, width * import_enhance_scale, height * import_enhance_scale, gen_mipmaps);
            gfx_texture_cache_charge(width * import_enhance_scale, height * import_enhance_scale, gen_mipmaps);
            return;
        }
    }

    gfx_rapi->upload_texture(rgba32_buf, width, height, gen_mipmaps);
    gfx_texture_cache_charge(width, height, gen_mipmaps);
}

static enum TexScaleEdge gfx_texscale_edge(uint8_t cm) {
    if (cm & G_TX_CLAMP) {
        return TEXSCALE_EDGE_CLAMP;
    }
    return (cm & G_TX_MIRROR) ? TEXSCALE_EDGE_MIRROR : TEXSCALE_EDGE_WRAP;
}

// Enhance Textures / Smooth Text for a tile's texels about to go up.
//
// A clamped tile smaller than what is uploaded for it - a padded row, or the
// mip levels stacked under a mipmapped texture - is resampled on its own with
// its edge repeated across the rest, so that the padding (near-white, as the
// decompressor leaves it) is not blended into the last texels the shader's
// clamp still samples. Measured the way the batch state measures the clamp
// (tex_width2 / tex_height2), so the crop and the shader agree on where the
// picture ends.
static void gfx_set_import_enhance(int tile, const LoadedTexture& loaded_texture, uint32_t tex_row_bytes, uint8_t siz) {
    const uint32_t padded_w = (tex_row_bytes * 2) >> siz;
    const bool padded = padded_w != rdp.texture_tile[tile].width;
    const uint8_t cms = rdp.texture_tile[tile].cms;
    const uint8_t cmt = rdp.texture_tile[tile].cmt;
    const uint32_t tile_w2 = rdp.texture_tile[tile].lrs >= rdp.texture_tile[tile].uls
        ? (rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls + 4) / 4 : 0;
    const uint32_t tile_h2 = rdp.texture_tile[tile].lrt >= rdp.texture_tile[tile].ult
        ? (rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult + 4) / 4 : 0;
    import_enhance_scale = loaded_texture.glyph ? gfx_text_smooth_scale : gfx_texture_enhance_scale;
    import_enhance_edge_s = padded ? TEXSCALE_EDGE_CLAMP : gfx_texscale_edge(cms);
    import_enhance_edge_t = gfx_texscale_edge(cmt);
    import_enhance_glyph = loaded_texture.glyph != 0;
    import_enhance_tile_w = (cms & G_TX_CLAMP) && !(cms & G_TX_MIRROR) ? tile_w2 : 0;
    import_enhance_tile_h = (cmt & G_TX_CLAMP) && !(cmt & G_TX_MIRROR) ? tile_h2 : 0;
}

static void import_texture_rgba16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    // SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);
    // TODO: this trips in some places with a garbage size in full_image_line_size_bytes
    // probably wherever framebuffer effects are used

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint16_t col16 = (addr[2 * i] << 8) | addr[2 * i + 1];
        const uint8_t a = col16 & 1;
        const uint8_t r = col16 >> 11;
        const uint8_t g = (col16 >> 6) & 0x1f;
        const uint8_t b = (col16 >> 1) & 0x1f;
        dest[0] = SCALE_5_8(r);
        dest[1] = SCALE_5_8(g);
        dest[2] = SCALE_5_8(b);
        dest[3] = a ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_rgba32(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint32_t *dest = (uint32_t *)tex_upload_buffer;
    const uint32_t *src = (const uint32_t *)addr;
    for (uint32_t i = 0; i < size_bytes; i += 4, ++dest, ++src) {
        *dest = PD_BE32(*src);
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = (size_bytes / 2) / rdp.texture_tile[tile].line_size_bytes;
	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_ia4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = part >> 1;
        const uint8_t alpha = part & 1;
        const uint8_t c = SCALE_3_8(intensity);
        dest[0] = c;
        dest[1] = c;
        dest[2] = c;
        dest[3] = alpha ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_ia8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = SCALE_4_8(addr[i] >> 4);
        const uint8_t alpha = SCALE_4_8(addr[i] & 0xf);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_ia16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint8_t intensity = addr[2 * i];
        const uint8_t alpha = addr[2 * i + 1];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_i4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = SCALE_4_8(part);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_i8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = addr[i];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static inline void palette_to_rgba32(const uint16_t palentry, uint8_t *rgba32_buf) {
    if (rdp.palette_fmt == G_TT_IA16) {
        // An IA16 entry is intensity in the high byte, alpha in the low one,
        // like an IA16 texel; the palette was byte-swapped to that on load.
        // Read the other way round, a dark opaque grey is a faint white,
        // which is how GE-X's KF7 magazine drew.
        const uint8_t intensity = palentry >> 8;
        const uint8_t alpha = (palentry & 0xff);
        rgba32_buf[0] = intensity;
        rgba32_buf[1] = intensity;
        rgba32_buf[2] = intensity;
        rgba32_buf[3] = alpha;
    } else {
        // assume G_TT_RGBA16
        const uint8_t a = palentry & 1;
        const uint8_t r = palentry >> 11;
        const uint8_t g = (palentry >> 6) & 0x1f;
        const uint8_t b = (palentry >> 1) & 0x1f;
        rgba32_buf[0] = SCALE_5_8(r);
        rgba32_buf[1] = SCALE_5_8(g);
        rgba32_buf[2] = SCALE_5_8(b);
        rgba32_buf[3] = a ? 255 : 0;
    }
}

static void import_texture_ci4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    const uint32_t pal_idx = rdp.texture_tile[tile].palette; // 0-15
    const uint16_t* palette = (const uint16_t *)(rdp.palette + pal_idx * 16); // 16 pixel entries, 16 bits each
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    for (uint32_t i = 0; i < size_bytes * 2; i++) {
        const uint8_t byte = addr[i / 2];
        const uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        palette_to_rgba32(palette[idx], tex_upload_buffer +4 * i);
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size * 2;
    const uint32_t height = size_bytes / result_line_size;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_ci8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;

    for (uint32_t i = 0, j = 0; i < size_bytes; j += full_image_line_size_bytes - line_size_bytes) {
        for (uint32_t k = 0; k < line_size_bytes; i++, k++, j++) {
            const uint8_t idx = addr[j];
            palette_to_rgba32(rdp.palette[idx], tex_upload_buffer + 4 * i);
        }
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size;
    const uint32_t height = size_bytes / result_line_size;

	gfx_upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

/**
 * Re-pads a replacement image to the shape the renderer maps.
 *
 * The N64 loads a texture as whole 8-byte lines, so a 33-texel-wide CI4 tile
 * is 48 texels of data, and the original goes up as those 48 with the tile in
 * the left 33 of them; every UV is normalised by the padded width. A pack
 * built for an emulator dumped the tile alone - 33 wide, scaled - and uploaded
 * over the 48 the tile's UVs show the left 33/48 of it: a stretch, on the few
 * hundred textures in a pack whose width is not a multiple of the line. Our
 * own dumps are the padded row and come back as they are.
 *
 * So an image that fits the padded shape is left alone, and anything else is
 * taken to be the tile: scaled onto a canvas of the padded shape at the same
 * scale, tile at the origin, with the edge repeated across the padding - the
 * clamp stops at the tile so it is never seen, and repeating the edge keeps
 * the filter from pulling black into the last texel.
 *
 * Returns the buffer to upload, which is either rep itself or a new one with
 * rep freed - both go back through texpackFreeReplacement().
 */
static uint8_t* gfx_pad_replacement(uint8_t* rep, int32_t* rep_width, int32_t* rep_height,
        uint32_t tile_w, uint32_t tile_h, uint32_t pad_w, uint32_t pad_h) {
    const uint32_t w = (uint32_t)*rep_width;
    const uint32_t h = (uint32_t)*rep_height;

    if ((tile_w == pad_w && tile_h == pad_h) || tile_w == 0 || tile_h == 0
            || tile_w > pad_w || tile_h > pad_h || w == 0 || h == 0) {
        return rep;
    }

    if ((uint64_t)w * pad_h == (uint64_t)h * pad_w) {
        return rep;
    }

    const uint32_t out_w = (uint32_t)lround((double)w * pad_w / tile_w);
    const uint32_t out_h = (uint32_t)lround((double)h * pad_h / tile_h);

    if (out_w < w || out_h < h || out_w > 16384 || out_h > 16384) {
        return rep;
    }

    uint8_t* out = (uint8_t*)malloc((size_t)out_w * out_h * 4);
    if (!out) {
        return rep;
    }

    for (uint32_t y = 0; y < out_h; y++) {
        const uint8_t* src = rep + (size_t)(y < h ? y : h - 1) * w * 4;
        uint8_t* dst = out + (size_t)y * out_w * 4;

        memcpy(dst, src, (size_t)w * 4);

        for (uint32_t x = w; x < out_w; x++) {
            memcpy(dst + (size_t)x * 4, src + (size_t)(w - 1) * 4, 4);
        }
    }

    texpackFreeReplacement(rep);
    *rep_width = (int32_t)out_w;
    *rep_height = (int32_t)out_h;

    return out;
}

/**
 * Decodes the game's own texels for a tile into tex_upload_buffer without
 * uploading them, leaving the size in last_upload_width/height (0 wide on a
 * format nothing decodes).
 */
static void gfx_decode_original(int tile, const LoadedTexture& loaded_texture, uint8_t fmt, uint8_t siz) {
    const int saved_scale = import_enhance_scale;

    import_decode_only = true;
    import_enhance_scale = 1;
    last_upload_width = 0;
    last_upload_height = 0;

    if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) {
        import_texture_rgba16(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_32b) {
        import_texture_rgba32(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_4b) {
        import_texture_ia4(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_8b) {
        import_texture_ia8(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_16b) {
        import_texture_ia16(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_CI && siz == G_IM_SIZ_4b) {
        import_texture_ci4(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_CI && siz == G_IM_SIZ_8b) {
        import_texture_ci8(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_4b) {
        import_texture_i4(tile, loaded_texture, false);
    } else if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_8b) {
        import_texture_i8(tile, loaded_texture, false);
    }

    import_decode_only = false;
    import_enhance_scale = saved_scale;
}

/**
 * A pack's opaque picture standing in for a texture the game draws with alpha.
 *
 * The XBLA release's art carries no alpha for most of the textures whose N64
 * original has some: its records for them are DXT1 or 8888 with 255 in every
 * pixel, and the console's renderer must have taken the shape from the game's
 * own texels, since the pictures draw right there. Uploaded as they are, a
 * light beam (an I8 gradient, whose alpha on the N64 *is* its intensity) is a
 * solid grey sheet, and every smoke puff, glare and cutout is a square. A Rice
 * pack missing the _a half of a split image has the same hole.
 *
 * So a replacement that is opaque in every pixel, for a texture whose own
 * texels are not, is given an alpha:
 *
 *   - an intensity texture's alpha is its intensity, so it comes from the
 *     picture's own luminance. That matches the picture where the release
 *     redrew it, which the original's alpha would not;
 *   - anything else takes the original's alpha, resampled onto the picture.
 *
 * Nothing changes for a picture that carries any alpha of its own, or for a
 * texture the game keeps at 255 throughout: an opaque wall stays one.
 */
static void gfx_replacement_alpha(uint8_t* rep, int32_t rep_width, int32_t rep_height,
        int tile, const LoadedTexture& loaded_texture, uint8_t fmt, uint8_t siz) {
    const size_t count = (size_t)rep_width * rep_height;

    if (!rep || rep_width <= 0 || rep_height <= 0) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (rep[i * 4 + 3] != 255) {
            return;
        }
    }

    gfx_decode_original(tile, loaded_texture, fmt, siz);

    const uint32_t ow = last_upload_width;
    const uint32_t oh = last_upload_height;

    if (ow == 0 || oh == 0) {
        return;
    }

    bool translucent = false;
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        if (tex_upload_buffer[i * 4 + 3] != 255) {
            translucent = true;
            break;
        }
    }

    if (!translucent) {
        return;
    }

    if (fmt == G_IM_FMT_I) {
        for (size_t i = 0; i < count; i++) {
            uint8_t* p = rep + i * 4;
            p[3] = (uint8_t)((p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8);
        }
        return;
    }

    // The original's alpha, bilinear, texel centres on the half - both images
    // cover the same tile, so the map is by ratio alone.
    const float sx = (float)ow / (float)rep_width;
    const float sy = (float)oh / (float)rep_height;

    for (int32_t y = 0; y < rep_height; y++) {
        float v = ((float)y + 0.5f) * sy - 0.5f;
        if (v < 0.0f) v = 0.0f;
        uint32_t y0 = (uint32_t)v;
        if (y0 >= oh - 1) { y0 = oh - 1; v = (float)y0; }
        const uint32_t y1 = y0 + 1 < oh ? y0 + 1 : y0;
        const float fy = v - (float)y0;
        const uint8_t* row0 = tex_upload_buffer + (size_t)y0 * ow * 4;
        const uint8_t* row1 = tex_upload_buffer + (size_t)y1 * ow * 4;
        uint8_t* out = rep + (size_t)y * rep_width * 4;

        for (int32_t x = 0; x < rep_width; x++) {
            float u = ((float)x + 0.5f) * sx - 0.5f;
            if (u < 0.0f) u = 0.0f;
            uint32_t x0 = (uint32_t)u;
            if (x0 >= ow - 1) { x0 = ow - 1; u = (float)x0; }
            const uint32_t x1 = x0 + 1 < ow ? x0 + 1 : x0;
            const float fx = u - (float)x0;
            const float a0 = row0[x0 * 4 + 3] * (1.0f - fx) + row0[x1 * 4 + 3] * fx;
            const float a1 = row1[x0 * 4 + 3] * (1.0f - fx) + row1[x1 * 4 + 3] * fx;
            out[x * 4 + 3] = (uint8_t)(a0 * (1.0f - fy) + a1 * fy + 0.5f);
        }
    }
}

static void import_texture(int i, int tile, bool importReplacement) {
    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    const uint8_t fmt = rdp.texture_tile[tile].fmt;
    const uint8_t siz = rdp.texture_tile[tile].siz;
    const uint32_t tex_flags = loaded_texture.tex_flags;
    const uint8_t palette_index = rdp.texture_tile[tile].palette;

    // Detail Textures on, and a texture that is its own detail - a room's
    // texture command with the detail flag and no second texture, such as the
    // Institute's rock 0281. Its detail tile reads the TMEM block the load put
    // down, which is every mip level stacked under the first (32x64 over 94
    // rows), while its lod tile is set up as level 0 alone just below. Both
    // are the same texels, so both land on one texture cache entry, and
    // whichever imported first was the picture the other drew: the detail
    // tile goes first, so the lod tile scaled its 64 rows over a 94 row
    // texture and wrapped through the mips - and, with a pack or the XBLA
    // release's picture, through padding that repeats the picture's last row,
    // a band of vertical streaks every period. Level 0 for both. A separate
    // detail texture (a second texture in the command) is loaded to its own
    // TMEM from its own image and is not this.
    const bool self_detail = rdp.tex_lod && rdp.tex_detail && tile == rdp.first_tile_index &&
                             loaded_texture.addr && loaded_texture.addr == rdp.texture_to_load.addr;

    if ((rdp.tex_lod && tile >= rdp.first_tile_index + rdp.tex_detail) || self_detail || !loaded_texture.addr) {
        // set up miplevel 0; also acts as a catch-all for when .addr is NULL because my texture loader sucks
        loaded_texture.addr = rdp.texture_to_load.addr;
        loaded_texture.glyph = rdp.texture_to_load.glyph;
        loaded_texture.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_image_line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_size_bytes = loaded_texture.full_image_line_size_bytes * rdp.texture_tile[tile].height;
        loaded_texture.size_bytes = loaded_texture.line_size_bytes * rdp.texture_tile[tile].height;
        if (siz == G_IM_SIZ_32b) {
            // HACK: fixup 32-bit LODed texture height
            loaded_texture.size_bytes <<= 1;
            loaded_texture.full_size_bytes <<= 1;
        }
        loaded_texture.orig_size_bytes = loaded_texture.size_bytes;
    }

    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* orig_addr = loaded_texture.addr;
    SUPPORT_CHECK(orig_addr);
    import_tint_addr = orig_addr;

    // A glyph adds its name to the key rather than replacing it. The palette
    // still matters for one that is not replaced: the fonts are CI4 through a
    // 16-entry bank of the TLUT that the tile's palette index picks, and the
    // same pixel data is meant to look different through a different bank -
    // keyed on the name alone, whichever palette drew a character first was
    // the one every later draw got, and the numeric font came out as solid
    // blocks. A replaced glyph gets one entry per palette it is drawn at, each
    // a copy out of texpack's store; nothing is decoded twice for it.
    //
    // The outline pass (textRender) is where the palette does the most: it
    // draws one glyph through both banks of its TLUT in a single two-cycle
    // pass, tile 0 through bank 0, whose alpha covers the body and the border,
    // and tile 1 through bank 1, whose alpha is the body alone - and the
    // combiner colours the body from the second and shapes the whole from the
    // first. A pack's outlines/ image is the first of those and its plain
    // image is the second, so the tile with palette 1 asks for the plain
    // glyph. Both tiles taking the outline image is what made every
    // highlighted menu item a bold glowing blob.
    uint32_t glyph = loaded_texture.glyph;
    if (glyph && TEXPACK_GLYPH_IS_OUTLINE(glyph) && palette_index == 1) {
        glyph &= ~0x7f000000u;
    }

    TextureCacheKey key;
    if (fmt == G_IM_FMT_CI) {
        key = { orig_addr, { rdp.palette_addrs[0], rdp.palette_addrs[1] }, fmt, siz, palette_index, glyph, 0 };
    } else {
        key = { orig_addr, {}, fmt, siz, palette_index, glyph, 0 };
    }

    // One of GoldenEye XBLA's animated pictures (xblaTexBindAnimation()): the
    // frame it shows now is its own entry, asked for by number below so the
    // entry holds the frame it is keyed on
    const int32_t anim_frame = xblaTexHaveAnimations() ? xblaTexAnimFrame(orig_addr) : -1;
    key.anim_frame = (uint16_t)(anim_frame + 1);

    if (gfx_texture_cache_lookup(i, key)) {
        return;
    }

    // A 32-bit texel is split across the two halves of TMEM, so the tile's line
    // counts half a row and the data is twice as long as it suggests - the same
    // doubling import_texture_rgba32() and loaded_texture.size_bytes apply. The
    // checksum and the raw dump both want the real pitch.
    const uint32_t tex_row_bytes =
        rdp.texture_tile[tile].line_size_bytes * (siz == G_IM_SIZ_32b ? 2 : 1);

    // A picture the menu draws that is not one of the game's textures - a
    // community pack's cover art. Like the meshes' textures below it, what the
    // list binds is a stand-in tile whose address is the picture's name, so
    // nothing keyed on a texture number could find it. First because it is the
    // shortest test of the three: a handful of addresses, and only while such
    // a page is open.
    if (menuImageHaveImages()) {
        int32_t rep_width;
        int32_t rep_height;
        uint8_t* rep = menuImageLoadReplacement(orig_addr, &rep_width, &rep_height);

        if (rep) {
            import_enhance_scale = 1; // a picture at the size it was drawn
            gfx_upload_texture(rep, rep_width, rep_height, rdp.tex_lod);
            menuImageFreeReplacement(rep);
            rendering_state.textures[i]->second.replaced = true;
            rendering_state.textures[i]->second.source = 'm';
            return;
        }
    }

    // The XBLA meshes' own textures. They are records in the release's
    // Textures.raw past the ones that carry a texture number, so nothing keyed
    // on a number can find them; what a mesh's display list binds is a stand-in
    // tile whose address is the name of a record. Ahead of the pack lookup
    // because a stand-in is not a texture the numbered index has an opinion
    // about, and behind the cache like everything else. A pack that ships an
    // xbla folder replaces one of these too - xblatex.c asks for it by record
    // and hands back the release's own art until it has decoded.
    if (xblaTexHaveTextures()) {
        int32_t rep_width;
        int32_t rep_height;
        uint8_t* rep = anim_frame >= 0 ? xblaTexLoadAnimFrame(orig_addr, anim_frame, &rep_width, &rep_height)
                                       : xblaTexLoadReplacement(orig_addr, &rep_width, &rep_height);

        if (rep) {
            import_enhance_scale = 1; // the release's own art, at the size it drew at
            gfx_upload_texture(rep, rep_width, rep_height, rdp.tex_lod);
            xblaTexFreeReplacement(rep);
            rendering_state.textures[i]->second.replaced = true;
            rendering_state.textures[i]->second.exact_uv = true;
            rendering_state.textures[i]->second.source = 'x';
            return;
        }
    }

    // A pack replaces the pixels and nothing else. The tile geometry the rest
    // of gfx_pc works from - and every texture coordinate derived from it - is
    // still the N64's, so a higher resolution image needs no other allowance:
    // UVs are normalised by the tile, not by what was uploaded.
    if (texpackHaveReplacements() || xblaTexHaveNumbered() || xblaFontHaveGlyphs()) {
        int32_t rep_width;
        int32_t rep_height;
        uint8_t* rep = nullptr;
        bool xbla_rep = false;
        bool xbla_font_rep = false;

        if (texpackHaveReplacements()) {
            rep = texpackLoadReplacement(orig_addr, &rep_width, &rep_height);

            // A font glyph has no texture number - it is uploaded straight out
            // of the font - so it is named by the display list instead, and a
            // pack keeps those under a folder per font.
            if (!rep && glyph) {
                rep = texpackLoadFontReplacement(glyph, &rep_width, &rep_height);
            }

            // A model's textures live inside the model file and never get a
            // texture number, so nothing above can find them. What is being
            // drawn is right here though, and a pack built for an emulator
            // named its files after a checksum of exactly these bytes.
            if (!rep && !loaded_texture.glyph && texpackHaveUnplacedFiles()) {
                rep = texpackLoadReplacementForTexels(orig_addr, loaded_texture.size_bytes,
                        rdp.texture_tile[tile].width, rdp.texture_tile[tile].height,
                        siz, tex_row_bytes, &rep_width, &rep_height);
            }
        }

        // The release's own glyph for this character, behind a pack's the way
        // its textures are behind a pack's textures. A glyph is named by the
        // display list, so this needs nothing of the texture registry - see
        // xblafont.h.
        // Asked of the pack as what it has rather than what it returned, since
        // a queued decode also answers NULL: reading that as "no file" would
        // paint the release's glyph over the pack's for a frame or two every
        // time the texture cache dropped one.
        if (!rep && glyph && xblaFontHaveGlyphs() && !texpackHaveFontReplacementFor(glyph)) {
            rep = xblaFontLoadGlyph(glyph, &rep_width, &rep_height);
            xbla_font_rep = rep != nullptr;
        }

        // The XBLA release's own picture for this texture, which is the pack
        // the conversion would have written, decoded out of the package
        // instead. Behind the pack and only for a number the pack has no file
        // for: a player who has painted over one texture keeps their picture
        // and the release's art fills in the rest. Asked for what the pack has
        // rather than what it returns, since a queued decode also answers
        // NULL. A glyph has no number and is never one of these.
        //
        // And never for texels that are not the ROM's: a mod's map brings its
        // own art at stock numbers, and the release has a picture for every
        // number, so the number is the only thing the two share
        // (texpackTextureArt()).
        if (!rep && !loaded_texture.glyph && xblaTexHaveNumbered()
                && texpackTextureArt(orig_addr) == TEXPACK_ART_ROM) {
            const int32_t texturenum = texpackGetTextureNum(orig_addr);

            if (texturenum >= 0 && !texpackHaveReplacementFor(texturenum)) {
                rep = xblaTexLoadNumbered(texturenum, &rep_width, &rep_height);
                xbla_rep = rep != nullptr;

                // F7 writes out what is drawn, and with the release's art on
                // what is drawn for most of the game's own textures is this -
                // which returns below before the dump at the end of the
                // function, so a dump taken with it on held next to nothing of
                // the level (F3 20260927-235833, the Institute's desk terminal
                // screen). Written under the texture's own number, as a pack
                // reads it back, and in the same row order as the ROM's texels.
                if (xbla_rep && texpackDumpEnabled()) {
                    struct texpackrawinfo raw;
                    raw.data = orig_addr;
                    raw.sizeBytes = loaded_texture.size_bytes;
                    raw.lineSizeBytes = tex_row_bytes;
                    raw.tileWidth = rdp.texture_tile[tile].width;
                    raw.tileHeight = rdp.texture_tile[tile].height;
                    raw.paletteIndex = palette_index;
                    raw.palette = fmt == G_IM_FMT_CI ? rdp.palette : NULL;
                    texpackDumpTexture(rep, (uint32_t)rep_width, (uint32_t)rep_height, fmt, siz, &raw);
                }
            }
        }

        if (rep) {
            // A glyph's image is the whole 16-wide block already - see the
            // font note in texture-packs.md - so only stage textures are
            // re-padded.
            import_enhance_scale = 1; // a pack's image is already what its author wanted

            if (!loaded_texture.glyph) {
                const uint32_t pad_w = (tex_row_bytes * 2) >> siz;
                const uint32_t pad_h = tex_row_bytes ? loaded_texture.size_bytes / tex_row_bytes : 0;
                rep = gfx_pad_replacement(rep, &rep_width, &rep_height,
                        rdp.texture_tile[tile].width, rdp.texture_tile[tile].height, pad_w, pad_h);
                gfx_replacement_alpha(rep, rep_width, rep_height, tile, loaded_texture, fmt, siz);

                // Except the release's picture where 4J never upscaled it -
                // 2033 of the numbered records are still the ROM's size (the
                // file select's "New Agent..." portrait, 063c, beside its 320x192
                // neighbours) - which is the game's own texels in all but name
                // and is enhanced as they would have been.
                if (xbla_rep && (uint32_t)rep_width <= pad_w && (uint32_t)rep_height <= pad_h) {
                    gfx_set_import_enhance(tile, loaded_texture, tex_row_bytes, siz);
                }
            }

            gfx_upload_texture(rep, rep_width, rep_height, rdp.tex_lod);

            if (xbla_rep) {
                xblaTexFreeReplacement(rep);
            } else if (xbla_font_rep) {
                xblaFontFreeGlyph(rep);
            } else {
                texpackFreeReplacement(rep);
            }

            rendering_state.textures[i]->second.replaced = true;
            rendering_state.textures[i]->second.source = xbla_rep ? 'X' : xbla_font_rep ? 'f' : 'p';
            return;
        }
    }

    last_upload_width = 0;

    // The game's own texels: scaled up as they are uploaded, if asked. A row
    // padded past the tile is clamped rather than wrapped, since what lies
    // over its far edge is padding and not the other side of the picture.
    gfx_set_import_enhance(tile, loaded_texture, tex_row_bytes, siz);

    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for RGBA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for IA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for CI texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for I texture in tile %d: %02x", tile, siz);
        }
    } else {
        sysFatalError("Bad texture format in tile %d: %02x %02x", tile, fmt, siz);
    }

    import_enhance_scale = 1;

    // Only ever reached on a cache miss, so a texture is written out once per
    // eviction at worst - and texpackDumpTexture() drops the repeats.
    if (last_upload_width && texpackDumpEnabled()) {
        struct texpackrawinfo raw;
        raw.data = orig_addr;
        raw.sizeBytes = loaded_texture.size_bytes;
        raw.lineSizeBytes = tex_row_bytes;
        raw.tileWidth = rdp.texture_tile[tile].width;
        raw.tileHeight = rdp.texture_tile[tile].height;
        raw.paletteIndex = palette_index;
        raw.palette = fmt == G_IM_FMT_CI ? rdp.palette : NULL;
        texpackDumpTexture(tex_upload_buffer, last_upload_width, last_upload_height, fmt, siz, &raw);
    }
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t* light, float coeffs[3]) {
    const float light_dir[3] = { light->dir[0] / 127.f, light->dir[1] / 127.f, light->dir[2] / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void calculate_normal_dir(const struct NormalColor *vcn, float coeffs[3]) {
    const float light_dir[3] = { vcn->x / 127.f, vcn->y / 127.f, vcn->z / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t* addr) {
    float matrix[4][4];

    if (parameters & G_MTX_FLOATS) {
        // The port's own flag: a matrix a port file built as floats and never
        // converted (xblamesh.c's divided draw matrix). Read as it is written,
        // for the precision s15.16 does not have for rows well under one.
        memcpy(matrix, addr, sizeof(matrix));
    } else {
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif
    }

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                   rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix,
                           rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                               rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x, float w = 1.f) {
    if (fbActive) {
        return x;
    } else {
        return (rsp.aspect_ofs * w + x) * rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
    }
}

static void gfx_adjust_width_height_for_scale(uint32_t& width, uint32_t& height) {
    width = std::round(width * RATIO_Y);
    height = std::round(height * RATIO_Y);
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
}

/**
 * G_TEXGEN_EYE_EXT: the normal the texgen looks up in place of `n` (model
 * space, 127 long). The texgen reads a normal as if the eye looked straight
 * down the middle of the screen, so a surface looks the same wherever the eye
 * stands and a flat wall takes one tint. This hands it the half-way vector
 * between the straight-on ray and the reflection of the ray the vertex is
 * really seen along: the same normal in the middle of the screen, and one that
 * turns as the vertex moves across the view. Worked in eye space (the
 * modelview alone, eye at the origin looking down -z) and handed back in the
 * model's, where the LookAt coefficients are.
 */
static inline void gfx_texgen_eye_normal(float px, float py, float pz, float n[3]) {
    const float (*mv)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    float ne[3], pe[3], h[3];

    for (int c = 0; c < 3; c++) {
        ne[c] = n[0] * mv[0][c] + n[1] * mv[1][c] + n[2] * mv[2][c];
        pe[c] = px * mv[0][c] + py * mv[1][c] + pz * mv[2][c] + mv[3][c];
    }

    const float nl = sqrtf(ne[0] * ne[0] + ne[1] * ne[1] + ne[2] * ne[2]);
    const float pl = sqrtf(pe[0] * pe[0] + pe[1] * pe[1] + pe[2] * pe[2]);

    if (nl < 1e-6f || pl < 1e-6f) {
        return;
    }

    const float d = (ne[0] * pe[0] + ne[1] * pe[1] + ne[2] * pe[2]) / (nl * pl);

    // the reflection e - 2(n.e)n, plus the straight-on ray reversed
    for (int c = 0; c < 3; c++) {
        h[c] = pe[c] / pl - 2.0f * d * ne[c] / nl;
    }

    h[2] += 1.0f;

    // back through the modelview's transpose, which undoes its rotation and
    // leaves only its scale, which the length below takes off
    float m[3];

    for (int k = 0; k < 3; k++) {
        m[k] = h[0] * mv[k][0] + h[1] * mv[k][1] + h[2] * mv[k][2];
    }

    const float ml = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);

    // a surface seen from behind reflects straight back down the middle
    if (ml < 1e-6f) {
        return;
    }

    for (int k = 0; k < 3; k++) {
        n[k] = m[k] * 127.0f / ml;
    }
}

/**
 * G_LIGHTING at one corner: its colour from the lights and, under
 * G_TEXTURE_GEN, its texture coordinates from the normal (nx, ny, nz, 127
 * long, in model space) against the LookAt. gfx_sp_load_vertex() hands it the
 * vertex's colour entry read as a normal; gfx_sp_tri_emit() hands it the
 * triangle's own under G_TEXGEN_FACE_EXT. Alpha is left to the caller.
 */
// The lights' and the LookAt's directions in model space, after a light or
// the modelview has changed
static inline void gfx_refresh_light_coeffs(void) {
    if (rsp.lights_changed) {
        for (int i = 0; i < rsp.current_num_lights - 1; i++) {
            calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
        }
        if (rsp.lookat_enabled) {
            calculate_normal_dir(&rsp.lookat[0], rsp.current_lookat_coeffs[0]);
            calculate_normal_dir(&rsp.lookat[1], rsp.current_lookat_coeffs[1]);
        }
        rsp.lights_changed = false;
    }
}

static inline __attribute__((always_inline)) void gfx_light_vertex(struct LoadedVertex* d, float px, float py, float pz,
                                                                   float nx, float ny, float nz, float* U, float* V) {
    gfx_refresh_light_coeffs();

    int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
    int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
    int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];

    for (int i = 0; i < rsp.current_num_lights - 1; i++) {
        float intensity = 0;
        intensity += nx * rsp.current_lights_coeffs[i][0];
        intensity += ny * rsp.current_lights_coeffs[i][1];
        intensity += nz * rsp.current_lights_coeffs[i][2];
        intensity /= 127.0f;
        if (intensity > 0.0f) {
            r += intensity * rsp.current_lights[i].col[0];
            g += intensity * rsp.current_lights[i].col[1];
            b += intensity * rsp.current_lights[i].col[2];
        }
    }

    d->color.r = r > 255 ? 255 : r;
    d->color.g = g > 255 ? 255 : g;
    d->color.b = b > 255 ? 255 : b;

    if (rsp.geometry_mode & G_TEXTURE_GEN) {
        const bool eye = (rsp.extra_geometry_mode & G_TEXGEN_EYE_EXT) != 0;
        float n[3] = { nx, ny, nz };
        float dotx = 0, doty = 0;

        if (eye) {
            gfx_texgen_eye_normal(px, py, pz, n);
        }

        if (rsp.lookat_enabled && eye && (rsp.extra_geometry_mode & G_TEXGEN_TURN_EXT)) {
            // G_TEXGEN_TURN_EXT: the LookAt yawed about its own y and
            // then pitched about the turned x, by the shift, so walking
            // sweeps the sphere map the way turning the camera does. A
            // turn stays on the map and wraps with no seam, where an
            // added shift would run off the round picture.
            const float* lx = rsp.current_lookat_coeffs[0];
            const float* ly = rsp.current_lookat_coeffs[1];
            const float lz[3] = { lx[1] * ly[2] - lx[2] * ly[1], lx[2] * ly[0] - lx[0] * ly[2],
                                  lx[0] * ly[1] - lx[1] * ly[0] };
            const float ca = rsp.texgen_turn[0], sa = rsp.texgen_turn[1];
            const float cb = rsp.texgen_turn[2], sb = rsp.texgen_turn[3];

            for (int c = 0; c < 3; c++) {
                const float rx = lx[c] * ca + lz[c] * sa;
                const float rz = lz[c] * ca - lx[c] * sa;
                const float ry = ly[c] * cb + rz * sb;

                dotx += n[c] * rx;
                doty += n[c] * ry;
            }

            dotx /= 127.0f;
            doty /= 127.0f;
        } else if (rsp.lookat_enabled) {
            dotx += n[0] * rsp.current_lookat_coeffs[0][0];
            dotx += n[1] * rsp.current_lookat_coeffs[0][1];
            dotx += n[2] * rsp.current_lookat_coeffs[0][2];
            doty += n[0] * rsp.current_lookat_coeffs[1][0];
            doty += n[1] * rsp.current_lookat_coeffs[1][1];
            doty += n[2] * rsp.current_lookat_coeffs[1][2];
            dotx /= 127.0f;
            doty /= 127.0f;
        } else {
            const float dir[3] = { n[0] / 127.f, n[1] / 127.f, n[2] / 127.f };
            float tvcn[3];
            gfx_transposed_matrix_mul(tvcn, dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
            gfx_normalize_vector(tvcn);
            dotx = tvcn[0];
            doty = tvcn[1];
        }

        dotx = clampf(dotx, -1.0f, 1.0f);
        doty = clampf(doty, -1.0f, 1.0f);

        if (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) {
            // Not sure exactly what formula we should use to get accurate values
            /*dotx = (2.906921f * dotx * dotx + 1.36114f) * dotx;
            doty = (2.906921f * doty * doty + 1.36114f) * doty;
            dotx = (dotx + 1.0f) / 4.0f;
            doty = (doty + 1.0f) / 4.0f;*/
            dotx = acosf(-dotx) /* M_PI */ / 4.0f;
            doty = acosf(-doty) /* M_PI */ / 4.0f;
        } else {
            dotx = (dotx + 1.0f) / 4.0f;
            doty = (doty + 1.0f) / 4.0f;
        }

        if (eye && !(rsp.extra_geometry_mode & G_TEXGEN_TURN_EXT)) {
            dotx += rsp.texgen_shift[0] / 2.0f;
            doty += rsp.texgen_shift[1] / 2.0f;
        }

        *U = (float)(int32_t)(dotx * rsp.texture_scaling_factor.s);
        *V = (float)(int32_t)(doty * rsp.texture_scaling_factor.t);
    }
}

/**
 * Transform, light and clip-test one vertex into `d`, from a model-space
 * position, its normal or colour entry, and texture coordinates already
 * scaled by the current G_TEXTURE factor. gfx_sp_vertex feeds it a G_VTX
 * command's vertices.
 */
// inlined into its two callers so gfx_sp_vertex's loop keeps the matrix rows
// and the mode tests out of the per-vertex work
static inline __attribute__((always_inline)) void gfx_sp_load_vertex(struct LoadedVertex* d, float px, float py, float pz, const struct NormalColor* vcn, float U, float V) {
    {
        // x, y, z and w are each px*M[0] + py*M[1] + pz*M[2] + M[3] over the
        // matrix's rows, which is one vector expression across the four
        // columns; the lanes add in the same order the scalars did.
        const v4f pos = v4f_splat(px) * v4f_load(rsp.MP_matrix[0]) + v4f_splat(py) * v4f_load(rsp.MP_matrix[1]) +
                        v4f_splat(pz) * v4f_load(rsp.MP_matrix[2]) + v4f_load(rsp.MP_matrix[3]);
        float x = pos[0];
        float y = pos[1];
        const float z = pos[2];
        const float w = pos[3];

        x = gfx_adjust_x_for_aspect_ratio(x, w);

        if (taa_active && !fbActive) {
            x += taa_jx * w;
            y += taa_jy * w;
        }

        if (rsp.geometry_mode & G_LIGHTING) {
            gfx_light_vertex(d, px, py, pz, vcn->x, vcn->y, vcn->z, &U, &V);
        } else {
            memcpy(&d->color, vcn, sizeof(d->color));
        }

        d->u = U;
        d->v = V;

        // G_TEXGEN_FACE_EXT: where the corner is in the model, for
        // gfx_sp_tri_emit() to build the triangle's normal from
        if (rsp.extra_geometry_mode & G_TEXGEN_FACE_EXT) {
            d->env[3] = px;
            d->env[4] = py;
            d->env[5] = pz;
        }

        // The per-pixel reflection's inputs (G_ENVMAP_EXT): what the fragment
        // shader reflects the view ray in, and where the ray starts. Through
        // the modelview alone - the projection and the aspect adjustment come
        // after the eye space the lookup is in. A normal is not renormalised
        // here, nor corrected for a matrix that scales one axis more than
        // another: the fragment shader normalises what it is handed, and the
        // CPU version this replaced did the same.
        if (rsp.extra_geometry_mode & G_ENVMAP_EXT) {
            const float (*mv)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
            const float nx = vcn->x, ny = vcn->y, nz = vcn->z;

            for (int c = 0; c < 3; c++) {
                d->env[c] = nx * mv[0][c] + ny * mv[1][c] + nz * mv[2][c];
                d->env[3 + c] = px * mv[0][c] + py * mv[1][c] + pz * mv[2][c] + mv[3][c];
            }
        }

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) {
            d->clip_rej |= 1; // CLIP_LEFT
        }
        if (x > w) {
            d->clip_rej |= 2; // CLIP_RIGHT
        }
        if (y < -w) {
            d->clip_rej |= 4; // CLIP_BOTTOM
        }
        if (y > w) {
            d->clip_rej |= 8; // CLIP_TOP
        }
        // if (z < -w) d->clip_rej |= 16; // CLIP_NEAR
        if (z > w) {
            d->clip_rej |= 32; // CLIP_FAR
        }

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (rsp.geometry_mode & G_FOG) {
            d->fog_mul = rsp.fog_mul;
            d->fog_offset = rsp.fog_offset;
        } else {

            // a constant factor: the fog colour's alpha
            d->fog_mul = 0;
            d->fog_offset = rdp.fog_color.a;
        }
        d->fog = 0;

        d->color.a = vcn->a; // can be required for SHADE_ALPHA even if fog is enabled

        // A lit corner's colour entry is its normal, and under a mode that
        // selects coverage for alpha without FORCE_BL the RDP blends only
        // partly covered edge pixels, by their coverage: the entry's alpha
        // byte never reaches a covered pixel. The levels' opaque lists draw
        // their reflective spans that way (G_RM_AA_ZB_OPA_TERR2, lit and
        // texgenned - Extraction's chrome door trims and its lobby's marble
        // counter) with a 4 in that byte, and blended by it they let the sky,
        // or whatever the frame held before, through a solid wall. Only the
        // vertex's byte: the texture's alpha still blends as before, since a
        // span of Defection's release rooms drew its environment map's dark
        // texels as black blocks in the sky when the whole draw was made
        // opaque. Not under CVG_X_ALPHA or an alpha compare, where the RDP
        // does read the combined alpha.
        if ((rsp.geometry_mode & G_LIGHTING) &&
            (rdp.other_mode_l & (ALPHA_CVG_SEL | FORCE_BL | CVG_X_ALPHA)) == ALPHA_CVG_SEL &&
            (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_NONE) {
            d->color.a = 0xff;
        }
    }
}

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx* vertices) {
    SUPPORT_CHECK(n_vertices <= MAX_VERTICES);

    g_GfxNumVerts += n_vertices;

    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx* v = &vertices[i];
        const short U = v->s * rsp.texture_scaling_factor.s >> 16;
        const short V = v->t * rsp.texture_scaling_factor.t >> 16;

        gfx_sp_load_vertex(&rsp.loaded_vertices[dest_index], v->v[0], v->v[1], v->v[2],
                           &rsp.vertex_colors[v->colour >> 2], U, V);
    }
}

static void gfx_sp_modify_vertex(uint16_t vtx_idx, uint8_t where, uint32_t val) {
    SUPPORT_CHECK(where == G_MWO_POINT_ST);

    int16_t s = (int16_t)(val >> 16);
    int16_t t = (int16_t)val;

    struct LoadedVertex* v = &rsp.loaded_vertices[vtx_idx];
    v->u = s;
    v->v = t;
}

static inline int gfx_lod_tile_offset(const int i) {
    if (gfx_detail_textures_enabled)
        return ((rdp.tex_lod && !rdp.tex_detail) ? 0 : i);
    return (rdp.tex_lod ? rdp.tex_detail : i);
}

/**
 * Work out everything about the current RDP/RSP state that gfx_sp_tri1 needs
 * but that no longer changes from one triangle to the next, and park it in
 * `batch`. Called only when gfx_mark_state_dirty() has fired or a texture is
 * pending, so the order of the flushes and the texture import inside it is the
 * same order gfx_sp_tri1 used to run them in.
 */
static void gfx_derive_batch_state(void) {
    uint64_t cc_options = 0;
    bool use_alpha =
        (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) && (rdp.other_mode_l & (3 << 16)) == (G_BL_1MA << 16);
    const bool use_fog = ((rdp.other_mode_l >> 30) == G_BL_CLR_FOG) || ((rdp.other_mode_l >> 26) == G_BL_A_FOG);
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    const bool use_noise = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    const bool use_2cyc = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    const bool alpha_threshold = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    bool invisible = (rdp.other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    const bool use_grayscale = rdp.grayscale;
    const bool use_blur = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BLUR_EXT;
    const bool use_envmap = (rsp.extra_geometry_mode & G_ENVMAP_EXT) != 0;

    if (texture_edge) {
        use_alpha = true;
    }

    // A faded body's depth pass (chrRender()): blended to nothing, so only the
    // depth it writes is left (gfx_emit_prepare() turns the write on). A
    // cutout's clear texels are still discarded before it, as they would be.
    if (rsp.extra_geometry_mode & G_DEPTH_PREPASS_EXT) {
        use_alpha = true;
        invisible = true;

        // With G_ALPHA_CORE_EXT as well, only the solid texels (three
        // quarters alpha and more) write their depth, whatever the render
        // mode: a room's translucent layer laid into the depth buffer after
        // it has been blended (bgRenderRoomXlu()), so a grating's metal hides
        // the props drawn after it and its holes do not.
        if (rsp.extra_geometry_mode & G_ALPHA_CORE_EXT) {
            texture_edge = true;
        }
    }

    if (use_alpha) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA;
    }
    if (use_fog) {
        cc_options |= (uint64_t)SHADER_OPT_FOG;
    }
    if (texture_edge) {
        cc_options |= (uint64_t)SHADER_OPT_TEXTURE_EDGE;

        if (rsp.extra_geometry_mode & G_ALPHA_CORE_EXT) {
            cc_options |= (uint64_t)SHADER_OPT_ALPHA_CORE;
        }
    }
    if (use_noise) {
        cc_options |= (uint64_t)SHADER_OPT_NOISE;
    }
    if (use_2cyc) {
        cc_options |= (uint64_t)SHADER_OPT_2CYC;
    }
    if (alpha_threshold) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA_THRESHOLD;
    }
    if (invisible) {
        cc_options |= (uint64_t)SHADER_OPT_INVISIBLE;
    }
    if (use_grayscale) {
        cc_options |= (uint64_t)SHADER_OPT_GRAYSCALE;
    }
    if (use_blur) {
        cc_options |= (uint64_t)SHADER_OPT_BLUR;
    }
    if (use_envmap) {
        cc_options |= (uint64_t)SHADER_OPT_ENVMAP;
    }
    if (use_fog && (rsp.extra_geometry_mode & G_ADDITIVE_EXT)) {
        cc_options |= (uint64_t)SHADER_OPT_FOG_FADE;
    }
    if (use_fog && rsp.fog_linear && (rsp.geometry_mode & G_FOG)) {
        cc_options |= (uint64_t)SHADER_OPT_FOG_LINEAR;
    }
    // G_FOG_VERTEX_EXT: the N64's fog, worked out per vertex (gfx_emit_vertex())
    const bool fog_vertex = use_fog && (rsp.extra_geometry_mode & G_FOG_VERTEX_EXT) != 0 &&
                            !(rsp.fog_linear && (rsp.geometry_mode & G_FOG));
    if (fog_vertex) {
        cc_options |= (uint64_t)SHADER_OPT_FOG_VERTEX;
    }

    // If we are not using alpha, clear the alpha components of the combiner as they have no effect
    if (!use_alpha) {
        cc_options &= ~((0xfff << 16) | ((uint64_t)0xfff << 44));
    }

    ColorCombinerKey key;
    key.combine_mode = rdp.combine_mode;
    key.options = cc_options;

    ColorCombiner* comb = gfx_lookup_or_create_color_combiner(key);

    uint32_t tm = 0;
    uint32_t tex_width[2] = { 1, 1 }, tex_height[2] = { 1, 1 };
    uint32_t tex_width2[2] = { 0, 0 }, tex_height2[2] = { 0, 0 };

    for (int i = 0; i < 2; i++) {
        // TODO: fix this; for now just ignore smaller mips
        const uint32_t tile = rdp.first_tile_index + gfx_lod_tile_offset(i);
        if (comb->used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush_for(GFX_FLUSH_TEXTURE);
                import_texture(i, tile, false);
                rdp.textures_changed[i] = false;
            }

            // textRender's outline pass: tile 0 is the glyph through the TLUT
            // bank that covers body and border, tile 1 through the bank that is
            // the body alone. The border the font bakes in is not a border but
            // the whole cell - every texel of an 'e' that is not red is opaque
            // black - which at 320x240 reads as a bold outline and at 1080p as
            // a black slab behind each letter. With this on, the shader shapes
            // the border itself as a one-texel halo around the body. A pack's
            // outlines/ image already is what its author wanted and is left be.
            if (i == 0 && gfx_clean_text_outlines && use_2cyc && comb->used_textures[1]) {
                const LoadedTexture& lt = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
                if (lt.glyph && TEXPACK_GLYPH_IS_OUTLINE(lt.glyph) && rendering_state.textures[0] &&
                    !rendering_state.textures[0]->second.replaced) {
                    tm |= 16;
                }
            }

            uint8_t cms = rdp.texture_tile[tile].cms;
            uint8_t cmt = rdp.texture_tile[tile].cmt;

            uint32_t tex_size_bytes = rdp.loaded_texture[rdp.texture_tile[tile].tmem].orig_size_bytes;
            uint32_t line_size = rdp.texture_tile[tile].line_size_bytes;

            if (line_size == 0) {
                line_size = 1;
            }

            tex_height[i] = tex_size_bytes / line_size;
            switch (rdp.texture_tile[tile].siz) {
                case G_IM_SIZ_4b:
                    line_size <<= 1;
                    break;
                case G_IM_SIZ_8b:
                    break;
                case G_IM_SIZ_16b:
                    line_size /= G_IM_SIZ_16b_LINE_BYTES;
                    break;
                case G_IM_SIZ_32b:
                    line_size /= G_IM_SIZ_32b_LINE_BYTES; // this is 2!
                    tex_height[i] /= 2;
                    break;
            }
            tex_width[i] = line_size;

            tex_width2[i] = (rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls + 4) / 4;
            tex_height2[i] = (rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult + 4) / 4;

            uint32_t tex_width1 = tex_width[i] << (cms & G_TX_MIRROR);
            uint32_t tex_height1 = tex_height[i] << (cmt & G_TX_MIRROR);

            if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
                tm |= 1 << 2 * i;
                cms &= ~G_TX_CLAMP;
            }
            if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != tex_height2[i])) {
                tm |= 1 << (2 * i + 1);
                cmt &= ~G_TX_CLAMP;
            }

            if (rendering_state.textures[i]) {
                bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
                if (linear_filter != rendering_state.textures[i]->second.linear_filter ||
                    cms != rendering_state.textures[i]->second.cms || cmt != rendering_state.textures[i]->second.cmt) {
                    gfx_flush_for(GFX_FLUSH_SAMPLER);
                    gfx_rapi->set_sampler_parameters(i, linear_filter, cms, cmt, rdp.tex_lod);
                    rendering_state.textures[i]->second.linear_filter = linear_filter;
                    rendering_state.textures[i]->second.cms = cms;
                    rendering_state.textures[i]->second.cmt = cmt;
                }
            }
        }
    }

    struct ShaderProgram* prg = comb->prg[tm];
    if (prg == NULL) {
        comb->prg[tm] = prg =
            gfx_lookup_or_create_shader_program(comb->shader_id0, comb->shader_id1 | ((tm & 15) * SHADER_OPT_TEXEL0_CLAMP_S) |
                                                                     ((tm & 16) ? SHADER_OPT_TEXT_OUTLINE : 0));
    }

    batch.comb = comb;
    batch.prg = prg;
    batch.tm = tm;
    batch.use_alpha = use_alpha;
    batch.use_fog = use_fog;
    batch.fog_vertex = fog_vertex;
    batch.use_grayscale = use_grayscale;
    batch.use_modulate = use_alpha && (rsp.extra_geometry_mode & G_MODULATE_EXT) != 0;
    batch.use_additive = use_alpha && !batch.use_modulate && (rsp.extra_geometry_mode & G_ADDITIVE_EXT) != 0;    batch.use_envmap = use_envmap;

    gfx_rapi->shader_get_info(prg, &batch.num_inputs, batch.used_textures);
    batch.clip_parameters = gfx_rapi->get_clip_parameters();

    /*
     * Fold the texture coordinate pipeline into one multiply-add per axis.
     * Per vertex it used to run: divide by 32, apply the tile shift, subtract
     * the tile origin, halve if the cycle is not perspective-corrected, add
     * half a texel under a linear filter, divide by the texture size. All of
     * those factors are tile and othermode state, so they collapse to a scale
     * and an offset here and the vertex loop keeps only the multiply-add.
     *
     * This drops a few intermediate roundings, so a coordinate can land a ULP
     * away from where it used to. Texture coordinates are continuous and the
     * result is sampled through a filter, so that is not observable.
     */
    const float persp = (rdp.other_mode_h & G_TP_PERSP) ? 1.0f : 0.5f;
    const float filt = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;

    for (int t = 0; t < 2; t++) {
        // Left at zero when the combiner does not use the texture, matching the
        // old code's habit of never touching these unless it had to.
        if (!comb->used_textures[t]) {
            batch.uv_scale[t][0] = batch.uv_scale[t][1] = 0.f;
            batch.uv_ofs[t][0] = batch.uv_ofs[t][1] = 0.f;
            batch.uv_scale_rect[t][0] = batch.uv_scale_rect[t][1] = 0.f;
            batch.uv_ofs_rect[t][0] = batch.uv_ofs_rect[t][1] = 0.f;
            batch.tex_clamp[t][0] = batch.tex_clamp[t][1] = 0.f;
            batch.tex_size[t][0] = batch.tex_size[t][1] = 1;
            continue;
        }

        const uint32_t tile = rdp.first_tile_index + gfx_lod_tile_offset(t);
        const int shift[2] = { rdp.texture_tile[tile].shifts, rdp.texture_tile[tile].shiftt };
        const float origin[2] = { rdp.texture_tile[tile].uls / 4.0f + rdp.texture_tile[tile].ofs_s,
                                  rdp.texture_tile[tile].ult / 4.0f + rdp.texture_tile[tile].ofs_t };
        const float inv_size[2] = { 1.0f / tex_width[t], 1.0f / tex_height[t] };

        // The half texel a linear filter adds is the N64's: its bilerp puts a
        // texel's centre on the integer, GL's on the half, and every texture
        // the game authored - and every pack image scaled from one - carries
        // that offset in the tile's own texels. A picture authored for a
        // half-centred sampler is not owed it. The XBLA meshes' art is the
        // case: their UVs are exact on the picture and the tile they are
        // measured against is a 32 texel stand-in, so half of one of those is
        // a sixty-fourth of the picture - eight pixels of a 512 wide face,
        // which is where a nose ends up beside its bridge.
        const float tfilt = (rendering_state.textures[t] && rendering_state.textures[t]->second.exact_uv) ? 0.0f : filt;

        for (int axis = 0; axis < 2; axis++) {
            float sf = 1.0f;
            if (shift[axis] != 0) {
                if (shift[axis] <= 10) {
                    sf = 1.0f / (float)(1 << shift[axis]);
                } else {
                    sf = (float)(1 << (16 - shift[axis]));
                }
            }

            // Triangles: scale, shift, origin, perspective, filter, normalise.
            batch.uv_scale[t][axis] = sf * persp * inv_size[axis] / 32.0f;
            batch.uv_ofs[t][axis] = (tfilt - origin[axis] * persp) * inv_size[axis];

            // Rectangles bypass the perspective and filter adjustments.
            batch.uv_scale_rect[t][axis] = sf * inv_size[axis] / 32.0f;
            batch.uv_ofs_rect[t][axis] = -origin[axis] * inv_size[axis];
        }

        batch.tex_clamp[t][0] = (tex_width2[t] - 0.5f) / tex_width[t];
        batch.tex_clamp[t][1] = (tex_height2[t] - 0.5f) / tex_height[t];
        batch.tex_size[t][0] = tex_width[t];
        batch.tex_size[t][1] = tex_height[t];
    }

    batch_state_dirty = false;
    emit_plan_dirty = true;
}

#ifdef GFX_VERIFY_BATCH_STATE
/**
 * Run a texture coordinate through the original per-vertex pipeline and check
 * the folded multiply-add landed in the same place. Folding drops a few
 * intermediate roundings, so this allows a small relative slack rather than
 * demanding equality.
 */
static void gfx_verify_uv(int t, bool is_rect, float raw_u, float raw_v, float got_u, float got_v) {
    const uint32_t tile = rdp.first_tile_index + gfx_lod_tile_offset(t);
    float u = raw_u / 32.0f;
    float v = raw_v / 32.0f;

    const int shifts = rdp.texture_tile[tile].shifts;
    const int shiftt = rdp.texture_tile[tile].shiftt;
    if (shifts != 0) {
        if (shifts <= 10) {
            u /= 1 << shifts;
        } else {
            u *= 1 << (16 - shifts);
        }
    }
    if (shiftt != 0) {
        if (shiftt <= 10) {
            v /= 1 << shiftt;
        } else {
            v *= 1 << (16 - shiftt);
        }
    }

    u -= rdp.texture_tile[tile].uls / 4.0f + rdp.texture_tile[tile].ofs_s;
    v -= rdp.texture_tile[tile].ult / 4.0f + rdp.texture_tile[tile].ofs_t;

    if (!is_rect) {
        if (!(rdp.other_mode_h & G_TP_PERSP)) {
            u *= 0.5f;
            v *= 0.5f;
        }
        if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT &&
                !(rendering_state.textures[t] && rendering_state.textures[t]->second.exact_uv)) {
            u += 0.5f;
            v += 0.5f;
        }
    }

    const float want_u = u / batch.tex_size[t][0];
    const float want_v = v / batch.tex_size[t][1];
    const float tol_u = 1e-4f * (fabsf(want_u) + 1.0f);
    const float tol_v = 1e-4f * (fabsf(want_v) + 1.0f);

    const float drift = fmaxf(fabsf(want_u - got_u), fabsf(want_v - got_v));
    if (drift > g_GfxVerifyUvWorst) {
        g_GfxVerifyUvWorst = drift;
    }
    g_GfxVerifyUvChecks++;

    if (fabsf(want_u - got_u) > tol_u || fabsf(want_v - got_v) > tol_v) {
        g_GfxVerifyUvDrift++;
        sysLogPrintf(LOG_ERROR, "F3D: uv fold drifted on tex%d: want %f,%f got %f,%f", t, want_u, want_v, got_u, got_v);
    }
}

/**
 * Derive the state again from scratch and complain if the cached copy had
 * drifted, which means some setter mutated an input without calling
 * gfx_mark_state_dirty(). Doing all the work the cache exists to avoid, so it
 * is a build-time opt-in: -DGFX_VERIFY_BATCH_STATE.
 *
 * Recomputing is side-effect free while the state really is clean - there is no
 * texture pending and no sampler change - so it does not perturb what it
 * measures.
 */
static void gfx_verify_batch_state(void) {
    const struct BatchState cached = batch;
    gfx_derive_batch_state();
    g_GfxVerifyBatchChecks++;
    if (memcmp(&cached, &batch, sizeof(batch)) != 0) {
        g_GfxVerifyBatchStale++;
        sysLogPrintf(LOG_ERROR, "F3D: batch state went stale; a setter is missing gfx_mark_state_dirty()");
    }
}
#endif

/*
 * What each shader input of the current combiner is fed from, per vertex.
 * Colour and alpha are resolved separately since the combiner maps them
 * separately; a CONST kind carries its value, already scaled to 0..1.
 */
enum EmitInputKind {
    EMIT_IN_CONST,
    EMIT_IN_SHADE,        // the vertex's colour (or alpha, for the alpha side)
    EMIT_IN_SHADE_ALPHA,  // the vertex's alpha, as a grey colour
    EMIT_IN_LOD_FRACTION, // from the vertex's depth
};

struct EmitInput {
    uint8_t rgb_kind, a_kind;
    float rgb[3];
    float a;
};

static struct {
    const ColorCombiner* comb;
    bool use_alpha;
    uint32_t tl_lod;
    struct RGBA prim, env;
    uint8_t prim_lod_fraction;
    struct EmitInput in[8];
} emit_inputs;

static inline bool operator!=(const struct RGBA& a, const struct RGBA& b) {
    return a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
}

static void gfx_resolve_emit_inputs(void) {
    emit_plan_dirty = true;
    emit_inputs.comb = batch.comb;
    emit_inputs.use_alpha = batch.use_alpha;
    emit_inputs.tl_lod = rdp.other_mode_h & G_TL_LOD;
    emit_inputs.prim = rdp.prim_color;
    emit_inputs.env = rdp.env_color;
    emit_inputs.prim_lod_fraction = rdp.prim_lod_fraction;

    for (int j = 0; j < batch.num_inputs && j < 8; j++) {
        struct EmitInput* in = &emit_inputs.in[j];
        // the colour side, as the per-vertex switch it replaces had it
        in->rgb_kind = EMIT_IN_CONST;
        in->rgb[0] = in->rgb[1] = in->rgb[2] = 0.0f;
        switch (batch.comb->shader_input_mapping[0][j]) {
            case G_CCMUX_PRIMITIVE:
                in->rgb[0] = rdp.prim_color.r / 255.0f;
                in->rgb[1] = rdp.prim_color.g / 255.0f;
                in->rgb[2] = rdp.prim_color.b / 255.0f;
                break;
            case G_CCMUX_SHADE:
                in->rgb_kind = EMIT_IN_SHADE;
                break;
            case G_CCMUX_SHADE_ALPHA:
                in->rgb_kind = EMIT_IN_SHADE_ALPHA;
                break;
            case G_CCMUX_ENVIRONMENT:
                in->rgb[0] = rdp.env_color.r / 255.0f;
                in->rgb[1] = rdp.env_color.g / 255.0f;
                in->rgb[2] = rdp.env_color.b / 255.0f;
                break;
            case G_CCMUX_PRIMITIVE_ALPHA:
                in->rgb[0] = in->rgb[1] = in->rgb[2] = rdp.prim_color.a / 255.0f;
                break;
            case G_CCMUX_ENV_ALPHA:
                in->rgb[0] = in->rgb[1] = in->rgb[2] = rdp.env_color.a / 255.0f;
                break;
            case G_CCMUX_PRIM_LOD_FRAC:
                in->rgb[0] = in->rgb[1] = in->rgb[2] = rdp.prim_lod_fraction / 255.0f;
                break;
            case G_CCMUX_LOD_FRACTION:
                if (rdp.other_mode_h & G_TL_LOD) {
                    in->rgb_kind = EMIT_IN_LOD_FRACTION;
                } else {
                    in->rgb[0] = in->rgb[1] = in->rgb[2] = 1.0f;
                }
                break;
            case G_ACMUX_PRIM_LOD_FRAC:
                // only the alpha was set: the colour stays zero
                break;
            default:
                break;
        }
        // the alpha side: only the cases that set tmp.a, or read a real
        // colour, gave anything but zero
        in->a_kind = EMIT_IN_CONST;
        in->a = 0.0f;
        if (batch.use_alpha) {
            switch (batch.comb->shader_input_mapping[1][j]) {
                case G_CCMUX_PRIMITIVE:
                    in->a = rdp.prim_color.a / 255.0f;
                    break;
                case G_CCMUX_SHADE:
                    in->a_kind = EMIT_IN_SHADE;
                    break;
                case G_CCMUX_ENVIRONMENT:
                    in->a = rdp.env_color.a / 255.0f;
                    break;
                case G_CCMUX_LOD_FRACTION:
                    if (rdp.other_mode_h & G_TL_LOD) {
                        in->a_kind = EMIT_IN_LOD_FRACTION;
                    } else {
                        in->a = 1.0f;
                    }
                    break;
                case G_ACMUX_PRIM_LOD_FRAC:
                    in->a = rdp.prim_lod_fraction / 255.0f;
                    break;
                default:
                    break;
            }
        }
    }
}

/*
 * The layout of one vertex in the buffer, as the batch's shader wants it,
 * worked out once per batch. Most of what goes into a vertex is the same
 * for every vertex of the batch - the texture clamps, the fog and grayscale
 * colours, the constant combiner inputs - so those sit in a template that is
 * copied whole, and the few floats that come from the vertex itself are
 * listed as slots with an offset each. gfx_sp_tri_emit then does one copy,
 * one position store and a short run of slot writes per vertex, instead of
 * rebuilding the layout with a switch per input per vertex. The values are
 * the same ones as before, from the same expressions.
 */
enum EmitSlotKind {
    EMIT_SLOT_UV0,         // texture 0's s,t: u * scale + offset, two floats
    EMIT_SLOT_UV1,         // texture 1's
    EMIT_SLOT_FOG_LINE,    // fog_mul, fog_offset
    EMIT_SLOT_SHADE_RGB,   // the vertex colour, three floats
    EMIT_SLOT_SHADE_A_RGB, // the vertex alpha as a grey colour
    EMIT_SLOT_LOD_RGB,     // the LOD fraction from the depth, three floats
    EMIT_SLOT_SHADE_A,     // the vertex alpha
    EMIT_SLOT_LOD_A,       // the LOD fraction, one float
    EMIT_SLOT_ENV,         // G_ENVMAP_EXT: the view-space normal and position, six floats
};

struct EmitSlot {
    uint8_t kind, off;
};

#define EMIT_MAX_FLOATS 32 // per vertex; buf_vbo is sized for it

static struct {
    struct RGBA fog, gray; // the colours the template was built with
    uint8_t stride;        // floats per vertex
    uint8_t nslots;
    struct EmitSlot slots[2 + 1 + 1 + 8 * 2];
    float tmpl[EMIT_MAX_FLOATS];
} emit_plan;

static void gfx_build_emit_plan(void) {
    emit_plan_dirty = false;
    emit_plan.fog = rdp.fog_color;
    emit_plan.gray = rdp.grayscale_color;
    memset(emit_plan.tmpl, 0, sizeof(emit_plan.tmpl));
    int off = 4; // the position
    int n = 0;
    float* tmpl = emit_plan.tmpl;

    for (int t = 0; t < 2; t++) {
        if (!batch.used_textures[t]) {
            continue;
        }
        emit_plan.slots[n++] = { (uint8_t)(EMIT_SLOT_UV0 + t), (uint8_t)off };
        off += 2;
        if (batch.tm & (1 << 2 * t)) {
            tmpl[off++] = batch.tex_clamp[t][0];
        }
        if (batch.tm & (1 << (2 * t + 1))) {
            tmpl[off++] = batch.tex_clamp[t][1];
        }
    }

    if (batch.use_fog) {
        tmpl[off + 0] = byte_unit.f[rdp.fog_color.r];
        tmpl[off + 1] = byte_unit.f[rdp.fog_color.g];
        tmpl[off + 2] = byte_unit.f[rdp.fog_color.b];
        emit_plan.slots[n++] = { EMIT_SLOT_FOG_LINE, (uint8_t)(off + 3) }; // the fog line, evaluated per fragment
        off += 5;
    }

    if (batch.use_grayscale) {
        tmpl[off + 0] = byte_unit.f[rdp.grayscale_color.r];
        tmpl[off + 1] = byte_unit.f[rdp.grayscale_color.g];
        tmpl[off + 2] = byte_unit.f[rdp.grayscale_color.b];
        tmpl[off + 3] = byte_unit.f[rdp.grayscale_color.a]; // lerp interpolation factor (not alpha)
        off += 4;
    }

    // after the grayscale colour and before the combiner inputs, the order the
    // shader declares its attributes in (gfx_opengl.cpp)
    if (batch.use_envmap) {
        emit_plan.slots[n++] = { EMIT_SLOT_ENV, (uint8_t)off };
        off += 6;
    }

    for (int j = 0; j < batch.num_inputs && j < 8; j++) {
        const struct EmitInput* in = &emit_inputs.in[j];
        switch (in->rgb_kind) {
            case EMIT_IN_SHADE:
                emit_plan.slots[n++] = { EMIT_SLOT_SHADE_RGB, (uint8_t)off };
                break;
            case EMIT_IN_SHADE_ALPHA:
                emit_plan.slots[n++] = { EMIT_SLOT_SHADE_A_RGB, (uint8_t)off };
                break;
            case EMIT_IN_LOD_FRACTION:
                emit_plan.slots[n++] = { EMIT_SLOT_LOD_RGB, (uint8_t)off };
                break;
            default:
                tmpl[off + 0] = in->rgb[0];
                tmpl[off + 1] = in->rgb[1];
                tmpl[off + 2] = in->rgb[2];
                break;
        }
        off += 3;
        if (batch.use_alpha) {
            switch (in->a_kind) {
                case EMIT_IN_SHADE:
                    emit_plan.slots[n++] = { EMIT_SLOT_SHADE_A, (uint8_t)off };
                    break;
                case EMIT_IN_LOD_FRACTION:
                    emit_plan.slots[n++] = { EMIT_SLOT_LOD_A, (uint8_t)off };
                    break;
                default:
                    tmpl[off] = in->a;
                    break;
            }
            off += 1;
        }
    }

    SUPPORT_CHECK(off <= EMIT_MAX_FLOATS);
    emit_plan.stride = off;
    emit_plan.nslots = n;
}

// the LOD fraction the combiner reads, from the vertex's depth
static inline float gfx_lod_fraction(float w) {
    const float distance_frac = std::max(0.f, std::min(w / 1024.f, 1.f));
    const uint8_t c = (uint8_t)((0.7f + distance_frac * 0.3f) * 255.f);
    return byte_unit.f[c];
}

/*
 * The state a triangle is drawn under - the depth mode, viewport and
 * scissor, shader and blending, the resolved combiner inputs and the vertex
 * layout: apply whatever moved, flushing the batch first, so the vertices
 * written next go out under it. Everything gfx_sp_tri_emit did before it
 * wrote a vertex.
 */
static inline __attribute__((always_inline)) void gfx_emit_prepare(void) {
    bool depth_test = ((rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER || (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM) &&
                      ((rdp.other_mode_h & G_CYC_1CYCLE) == G_CYC_1CYCLE || (rdp.other_mode_h & G_CYC_2CYCLE) == G_CYC_2CYCLE);
    bool depth_update = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    bool depth_compare = (rdp.other_mode_l & Z_CMP) == Z_CMP;
    bool depth_source_prim = (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM /* && gDP.primDepth.z == 1.0f */;
    uint16_t zmode = (rsp.extra_geometry_mode & G_DECAL_EXT) ? ZMODE_DEC : (rdp.other_mode_l & ZMODE_DEC);

    // A faded body in two passes (chrRender()): the first lays down the depth
    // of its nearest surface and draws nothing, the second compares
    // less-or-equal without writing, so exactly that surface is blended once
    // and the arm behind the chest, or the inside of the far shoulder, is not
    // seen through it.
    if (rsp.extra_geometry_mode & G_DEPTH_PREPASS_EXT) {
        depth_update = true;
        depth_compare = true;
        zmode = ZMODE_OPA;
    } else if (rsp.extra_geometry_mode & G_DEPTH_FRONT_EXT) {
        depth_update = false;
        depth_compare = true;
        zmode = ZMODE_INTER;
    }

    uint32_t depth_mode = (depth_test ? 1 : 0) | (depth_update ? 2 : 0) | (depth_compare ? 4 : 0) | (depth_source_prim ? 8 : 0) | (zmode >> 6) |
                          ((uint32_t)(uint16_t)rdp.depth_bias << 8);

    if (depth_mode != rendering_state.depth_mode) {
        gfx_flush_for(GFX_FLUSH_DEPTH);
        gfx_rapi->set_depth_mode(depth_test, depth_update, depth_compare, depth_source_prim, zmode, rdp.depth_bias);
        rendering_state.depth_mode = depth_mode;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush_for(GFX_FLUSH_VIEWPORT);
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush_for(GFX_FLUSH_VIEWPORT);
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    /*
     * batch.comb is stable while the state is clean, so last frame's answer for
     * which textures the combiner uses is the right one to test the pending
     * texture loads against. Testing rdp.textures_changed on its own would
     * force a recompute every triangle: the flag is set for both texture units
     * by every tile change but only ever cleared for units the combiner
     * actually samples, so the unused unit's flag stays raised for good.
     */
    if (batch_state_dirty || (rdp.textures_changed[0] && batch.comb->used_textures[0]) ||
        (rdp.textures_changed[1] && batch.comb->used_textures[1])) {
        gfx_derive_batch_state();
    }
#ifdef GFX_VERIFY_BATCH_STATE
    gfx_verify_batch_state();
#endif

    if (batch.prg != rendering_state.shader_program) {
        gfx_flush_for(GFX_FLUSH_SHADER);
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(batch.prg);
        rendering_state.shader_program = batch.prg;
    }
    if (batch.use_alpha != rendering_state.alpha_blend || batch.use_modulate != rendering_state.modulate ||
        batch.use_additive != rendering_state.additive) {
        gfx_flush_for(GFX_FLUSH_BLEND);
        gfx_rapi->set_use_alpha(batch.use_alpha, batch.use_modulate, batch.use_additive);
        rendering_state.alpha_blend = batch.use_alpha;
        rendering_state.modulate = batch.use_modulate;
        rendering_state.additive = batch.use_additive;
    }

    // The shader inputs. Most of them are a constant for the whole
    // triangle - the primitive and environment colours - so what each
    // one is, and its value where it is constant, is worked out once
    // when any of those change (below) rather than per vertex.
    if (emit_inputs.comb != batch.comb || emit_inputs.use_alpha != batch.use_alpha ||
        emit_inputs.prim != rdp.prim_color || emit_inputs.env != rdp.env_color ||
        emit_inputs.prim_lod_fraction != rdp.prim_lod_fraction ||
        emit_inputs.tl_lod != (rdp.other_mode_h & G_TL_LOD)) {
        gfx_resolve_emit_inputs();
    }
    // and the layout, which also carries the fog and grayscale colours
    if (emit_plan_dirty || emit_plan.fog != rdp.fog_color || emit_plan.gray != rdp.grayscale_color) {
        gfx_build_emit_plan();
    }
}

/*
 * Under G_FOG_VERTEX_EXT, the triangle being emitted carries its corners' own
 * fog factors (gfx_emit_tri3()).
 */
static bool emit_fog_tri;

/*
 * One vertex into buf_vbo in the batch's layout.
 */
static inline __attribute__((always_inline)) void gfx_emit_vertex(const struct LoadedVertex* v, bool is_rect) {
    /* Rectangles skip the perspective and filter terms, so they use their own pair. */
    const float (*uv_scale)[2] = is_rect ? batch.uv_scale_rect : batch.uv_scale;
    const float (*uv_ofs)[2] = is_rect ? batch.uv_ofs_rect : batch.uv_ofs;

    // y is flipped by a multiply, which is exact; z is halved into 0..1 only
    // for a backend that wants it, in the scalar form that always did it
    const v4f ysign = v4f{ 1.0f, batch.clip_parameters.invert_y ? -1.0f : 1.0f, 1.0f, 1.0f };

    // The vertex buffer is written through a pointer rather than an index
    // bumped per float, and a position goes in as one four-float store.
    float* out = buf_vbo + buf_vbo_len;
    const float w = v->w;

    // The template, all EMIT_MAX_FLOATS of it as straight-line stores
    // whatever the stride; the excess is overwritten by the next vertex,
    // and buf_vbo has room for a full-size vertex at every position
    for (int k = 0; k < EMIT_MAX_FLOATS; k += 4) {
        v4f_store(out + k, v4f_load(emit_plan.tmpl + k));
    }

    v4f pos = v4f_load(&v->x) * ysign;
    if (batch.clip_parameters.z_is_from_0_to_1) {
        pos[2] = (v->z + w) / 2.0f;
    }
    v4f_store(out, pos);

    for (int k = 0; k < emit_plan.nslots; k++) {
        float* o = out + emit_plan.slots[k].off;
        switch (emit_plan.slots[k].kind) {
            case EMIT_SLOT_UV0:
            case EMIT_SLOT_UV1: {
                const int t = emit_plan.slots[k].kind - EMIT_SLOT_UV0;
                o[0] = v->u * uv_scale[t][0] + uv_ofs[t][0];
                o[1] = v->v * uv_scale[t][1] + uv_ofs[t][1];
#ifdef GFX_VERIFY_BATCH_STATE
                gfx_verify_uv(t, is_rect, v->u, v->v, o[0], o[1]);
#endif
                break;
            }
            case EMIT_SLOT_FOG_LINE:
                if (batch.fog_vertex) {
                    // The offset times w, which the fragment divides by its
                    // own interpolated w: a value carried that way is linear
                    // on the screen, as the N64's shade alpha is. A triangle
                    // carries each corner's own factor, clamped there as the
                    // RSP clamps it (gfx_emit_tri3() has cut it where the RSP
                    // would first); a rectangle keeps the line.
                    if (emit_fog_tri) {
                        float f = (v->z / w) * (float)v->fog_mul + (float)v->fog_offset;
                        f = f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
                        o[0] = 0.0f;
                        o[1] = f * w;
                    } else {
                        o[0] = (float)v->fog_mul;
                        o[1] = (float)v->fog_offset * w;
                    }
                } else {
                    o[0] = (float)v->fog_mul;
                    o[1] = (float)v->fog_offset;
                }
                break;
            case EMIT_SLOT_SHADE_RGB:
                o[0] = byte_unit.f[v->color.r];
                o[1] = byte_unit.f[v->color.g];
                o[2] = byte_unit.f[v->color.b];
                break;
            case EMIT_SLOT_SHADE_A_RGB:
                o[0] = o[1] = o[2] = byte_unit.f[v->color.a];
                break;
            case EMIT_SLOT_LOD_RGB:
                o[0] = o[1] = o[2] = gfx_lod_fraction(w);
                break;
            case EMIT_SLOT_SHADE_A:
                o[0] = byte_unit.f[v->color.a];
                break;
            case EMIT_SLOT_LOD_A:
                o[0] = gfx_lod_fraction(w);
                break;
            case EMIT_SLOT_ENV:
                memcpy(o, v->env, sizeof(v->env));
                break;
        }
    }

    buf_vbo_len += emit_plan.stride;
}

// A triangle is in the batch: count it toward the batch's limit
static inline __attribute__((always_inline)) void gfx_emit_tri_done(void) {
    // >= rather than ==, because g_GfxMaxBufferedTris can be lowered from gdb
    // partway through a frame and must not be stepped straight over.
    if (++buf_vbo_num_tris >= g_GfxMaxBufferedTris) {
        g_GfxNumBufferFullFlushes++;
        gfx_flush_for(GFX_FLUSH_BUFFERFULL);
    }
}

/*
 * A point on the edge from p to q, t of the way along it in clip space, as a
 * clipper makes one: every attribute carried linearly.
 */
static void gfx_lerp_vertex(struct LoadedVertex* o, const struct LoadedVertex* p, const struct LoadedVertex* q, float t) {
    *o = *p;
#define LERP_F(f) o->f = p->f + (q->f - p->f) * t
    LERP_F(x);
    LERP_F(y);
    LERP_F(z);
    LERP_F(w);
    LERP_F(u);
    LERP_F(v);
    for (int e = 0; e < 6; e++) {
        LERP_F(env[e]);
    }
#undef LERP_F
    const uint8_t* pc = &p->color.r;
    const uint8_t* qc = &q->color.r;
    uint8_t* oc = &o->color.r;
    for (int e = 0; e < 4; e++) {
        const float f = pc[e] + (qc[e] - pc[e]) * t + 0.5f;
        oc[e] = f <= 0 ? 0 : f >= 255 ? 255 : (uint8_t)f;
    }
    o->clip_rej = 0;
}

/*
 * The clip volume of the N64's RSP under gSPClipRatio(FRUSTRATIO_2), which
 * both games set (lv.c): a guard band twice the screen's size, and the eye's
 * own plane - not the near plane: the cartridge draws what stands nearer than
 * it (Dam's pad 157, a rock face a step away), as the renderers here do with
 * depth clamping on. Distance inside plane k (0 the eye's, 1-4 left, right,
 * bottom, top).
 */
#define GFX_RSP_CLIP_W 0.01f

static inline float gfx_rsp_clip_dist(const struct LoadedVertex* v, int k) {
    switch (k) {
        case 0:
            return v->w - GFX_RSP_CLIP_W;
        case 1:
            return 2.0f * v->w + v->x;
        case 2:
            return 2.0f * v->w - v->x;
        case 3:
            return 2.0f * v->w + v->y;
        default:
            return 2.0f * v->w - v->y;
    }
}

/*
 * One triangle into the batch. Under G_FOG_VERTEX_EXT each corner carries its
 * own fog factor, worked out from its own depth (gfx_emit_vertex()), and the
 * triangle is cut first where the RSP would cut it - behind the eye and at
 * the guard band - so that its new corners work theirs out where they stand,
 * as the RSP's clipper does. A floor triangle reaching behind the camera then
 * has corners just under the screen with the fog of their own depth, not the
 * far corner's fog carried down to Bond's feet: measured on the cartridge at
 * Surface 2's pad 245 (fog 76, 97, 123, 142 up the middle of the screen;
 * this model 81, 101, 122, 142; without the guard band, 147-169).
 */
static void gfx_emit_tri3(const struct LoadedVertex* a, const struct LoadedVertex* b, const struct LoadedVertex* c,
                          bool is_rect) {
    emit_fog_tri = batch.fog_vertex && !is_rect;

    if (!emit_fog_tri) {
        gfx_emit_vertex(a, is_rect);
        gfx_emit_vertex(b, is_rect);
        gfx_emit_vertex(c, is_rect);
        gfx_emit_tri_done();
        return;
    }

    const struct LoadedVertex* in3[3] = { a, b, c };
    int outside = 0;

    for (int k = 0; k < 5; k++) {
        for (int i = 0; i < 3; i++) {
            if (gfx_rsp_clip_dist(in3[i], k) < 0.0f) {
                outside |= 1 << k;
            }
        }
    }

    if (outside == 0) {
        gfx_emit_vertex(a, is_rect);
        gfx_emit_vertex(b, is_rect);
        gfx_emit_vertex(c, is_rect);
        gfx_emit_tri_done();
        return;
    }

    // Sutherland-Hodgman, a plane at a time: three corners and five planes
    // make at most eight
    struct LoadedVertex buf[2][8];
    int n = 3;

    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;

    int cur = 0;

    for (int k = 0; k < 5 && n >= 3; k++) {
        if (!(outside & (1 << k))) {
            continue;
        }

        const struct LoadedVertex* poly = buf[cur];
        struct LoadedVertex* out = buf[cur ^ 1];
        int m = 0;

        for (int i = 0; i < n; i++) {
            const struct LoadedVertex* p = &poly[i];
            const struct LoadedVertex* q = &poly[(i + 1) % n];
            const float dp = gfx_rsp_clip_dist(p, k);
            const float dq = gfx_rsp_clip_dist(q, k);

            if (dp >= 0.0f && m < 8) {
                out[m++] = *p;
            }

            if ((dp >= 0.0f) != (dq >= 0.0f) && m < 8) {
                gfx_lerp_vertex(&out[m++], p, q, dp / (dp - dq));
            }
        }

        n = m;
        cur ^= 1;
    }

    for (int i = 1; i + 1 < n; i++) {
        gfx_emit_vertex(&buf[cur][0], is_rect);
        gfx_emit_vertex(&buf[cur][i], is_rect);
        gfx_emit_vertex(&buf[cur][i + 1], is_rect);
        gfx_emit_tri_done();
    }
}

/*
 * Whether the triangle faces away under the current culling mode.
 */
static bool gfx_tri_is_culled(const struct LoadedVertex* v1, const struct LoadedVertex* v2, const struct LoadedVertex* v3) {
    if ((rsp.geometry_mode & G_CULL_BOTH) == 0) {
        return false;
    }
    // G_NO_CULLING_EXT draws both sides only where the triangle writes depth,
    // since that is what sorts a closed shape's near faces over its far ones.
    // A GoldenEye gun's secondary lists are translucent and write none, and
    // the KF7 is most of its body in them: with both sides drawn, the far
    // faces landed over the near ones in list order (F3 20260927-105948,
    // "part of the texture seems invisible"). Those keep the lists' culling,
    // which is what GoldenEye draws them with.
    // A faded own body's two passes (chrRender()) count as writing depth
    // whatever the lists' render mode: the chr's fade puts every list in a
    // translucent mode, so its held GoldenEye gun lost its backward-wound
    // faces (the silenced D5K's silencer) while fading. The depth pass sorts
    // both sides, and the front pass draws only what the depth pass left
    // nearest, so drawing both sides is right in both.
    if ((rsp.extra_geometry_mode & G_NO_CULLING_EXT) &&
        ((rdp.other_mode_l & Z_UPD) || (rsp.extra_geometry_mode & (G_DEPTH_PREPASS_EXT | G_DEPTH_FRONT_EXT)))) {
        return false;
    }
    if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_BOTH) {
        return true;
    }
    // the two edges from v2 on screen: (x1/w1 - x2/w2, y1/w1 - y2/w2) and
    // the same for v3, with the four divisions of each side done as one
    const v4f p = v4f{ v1->x, v1->y, v3->x, v3->y } / v4f{ v1->w, v1->w, v3->w, v3->w };
    const v4f q = v4f{ v2->x, v2->y, v2->x, v2->y } / v4f_splat(v2->w);
    const v4f d = p - q;
    const float dx1 = d[0], dy1 = d[1], dx2 = d[2], dy2 = d[3];
    float cross = dx1 * dy2 - dy1 * dx2;
    if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
        cross = -cross;
    }
    // G_INVERT_CULLING_EXT: the model is drawn mirrored, so each face's
    // winding is backwards and a list's own G_CULL_BACK means its front
    if (rsp.extra_geometry_mode & G_INVERT_CULLING_EXT) {
        cross = -cross;
    }
    if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_FRONT) {
        return cross <= 0;
    }
    return cross >= 0;
}

/*
 * G_SEAL_SEAMS_EXT: a room's opaque faces grown on screen, so that faces which
 * do not quite meet still cover the pixels between them.
 *
 * Perfect Dark's levels are full of T-junctions - a corner of one face lying
 * on the edge of another rather than at its end - and on a wall square to no
 * axis the corner's whole-unit position is up to a unit off that edge. The
 * N64's 320x240 hid it; at 1080p it is a line of whatever lies behind: the
 * Institute's walls, whose feet stand a fraction of a unit off the top of the
 * reflection under the glass floor, drew a line of the sky's clear colour
 * along the floor (F3 20260928-012509, "blue sky lines appearing top and
 * bottom of the wall").
 *
 * Each edge is moved out by g_GfxSealSeams pixels (half of one: any crack
 * narrower than a pixel is covered from both sides) and each corner goes to
 * where its two moved edges meet, held to three times that at a sharp corner.
 * The new corner is put back on the face's own plane - the point of the
 * triangle, extended, that lands on that pixel - so its depth, texture
 * coordinates and colour are the face's own there, perspective-correct: no
 * texture shifts and no depth changes under a decal. Opaque faces only (a
 * translucent one would blend twice where it overlaps its neighbour), never a
 * decal, and only where every corner is in front of the eye. Only the faces
 * bgMarkRoomSeams() found at a T-junction (gfx_seal_this), and never an HD
 * level's rooms: growing every face cost a quarter of the game thread on
 * Surface in HD (post-process.md).
 */
float g_GfxSealSeams = 0.5f; // pixels; 0 turns it off (gdb)
// The triangle being drawn is one bgMarkRoomSeams() found a crack along (a
// corner on another face's edge, or a face with one on its own): its bit in
// the G_TRI4's pad byte or the G_TRI1's flag byte. Only those are grown - the
// rest of a room's faces meet their neighbours corner to corner.
static bool gfx_seal_this;

static bool gfx_seal_seams(const struct LoadedVertex* const in[3], struct LoadedVertex out[3]) {
    const float hw = rdp.viewport.width * 0.5f;
    const float hh = rdp.viewport.height * 0.5f;
    const float grow = g_GfxSealSeams;
    float px[3], py[3], iw[3], len[3];

    for (int i = 0; i < 3; i++) {
        if (!(in[i]->w > 1e-3f)) {
            return false;
        }
        iw[i] = 1.0f / in[i]->w;
        px[i] = in[i]->x * iw[i] * hw;
        py[i] = in[i]->y * iw[i] * hh;
    }

    const float area2 = fabsf((px[1] - px[0]) * (py[2] - py[0]) - (py[1] - py[0]) * (px[2] - px[0]));

    if (!(area2 > 1e-4f)) {
        return false;
    }

    // The length of the edge opposite each corner
    for (int i = 0; i < 3; i++) {
        const int j = (i + 1) % 3;
        const int k = (i + 2) % 3;
        const float dx = px[k] - px[j];
        const float dy = py[k] - py[j];
        len[i] = sqrtf(dx * dx + dy * dy);
    }

    // A point's barycentric coordinate for corner i is its distance from the
    // opposite edge over that corner's height, area2 / len[i]. The corner
    // moved to where both of its edges, moved out by grow, meet lies grow
    // outside each: -grow * len[i] / area2 for the two other corners.
    const float s = grow / area2;
    const float cap2 = grow * grow * 9.0f;

    for (int k = 0; k < 3; k++) {
        const int a = (k + 1) % 3;
        const int b = (k + 2) % 3;
        float da = -s * len[a];
        float db = -s * len[b];

        // How far that moves the corner on screen, held to three times grow at
        // a sharp corner (a sliver's would be many pixels)
        const float mx = da * (px[a] - px[k]) + db * (px[b] - px[k]);
        const float my = da * (py[a] - py[k]) + db * (py[b] - py[k]);
        const float m2 = mx * mx + my * my;

        if (m2 > cap2) {
            const float f = sqrtf(cap2 / m2);
            da *= f;
            db *= f;
        }

        // The weights of the corners in clip space that land there: b / w,
        // normalised
        float c[3];
        c[a] = da * iw[a];
        c[b] = db * iw[b];
        c[k] = (1.0f - da - db) * iw[k];

        const float sum = c[0] + c[1] + c[2];

        if (!(sum > 1e-12f)) {
            return false;
        }

        const float isum = 1.0f / sum;

        c[0] *= isum;
        c[1] *= isum;
        c[2] *= isum;

        struct LoadedVertex* o = &out[k];
        *o = *in[k];

#define SEAL_MIX(f) (c[0] * in[0]->f + c[1] * in[1]->f + c[2] * in[2]->f)
        o->x = SEAL_MIX(x);
        o->y = SEAL_MIX(y);
        o->z = SEAL_MIX(z);
        o->w = SEAL_MIX(w);
        o->u = SEAL_MIX(u);
        o->v = SEAL_MIX(v);

        // Only these read env (gfx_sp_load_vertex(), gfx_sp_tri_emit())
        if (rsp.extra_geometry_mode & (G_ENVMAP_EXT | G_TEXGEN_FACE_EXT)) {
            for (int e = 0; e < 6; e++) {
                o->env[e] = SEAL_MIX(env[e]);
            }
        }

        const float rgba[4] = { SEAL_MIX(color.r), SEAL_MIX(color.g), SEAL_MIX(color.b), SEAL_MIX(color.a) };
#undef SEAL_MIX
        uint8_t* dst[4] = { &o->color.r, &o->color.g, &o->color.b, &o->color.a };

        for (int e = 0; e < 4; e++) {
            const float f = rgba[e] + 0.5f;
            *dst[e] = f <= 0 ? 0 : f >= 255 ? 255 : (uint8_t)f;
        }
    }

    return true;
}

static void gfx_sp_tri_emit(struct LoadedVertex* v1, struct LoadedVertex* v2, struct LoadedVertex* v3, bool is_rect) {
    if ((rsp.extra_geometry_mode & G_NO_CLIPPING_EXT) == 0) {
        if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
            // The whole triangle lies outside the visible area
            g_GfxTrisClipped++;
            return;
        }
    }

    if (gfx_tri_is_culled(v1, v2, v3)) {
        g_GfxTrisCulled++;
        return;
    }

    struct LoadedVertex sealed[3];

    if (gfx_seal_this && (rsp.extra_geometry_mode & G_SEAL_SEAMS_EXT) && g_GfxSealSeams > 0 && !is_rect &&
        (rdp.other_mode_l & Z_UPD) && !(rdp.other_mode_l & FORCE_BL) &&
        (rdp.other_mode_l & ZMODE_DEC) != ZMODE_DEC && !(rsp.extra_geometry_mode & G_DECAL_EXT)) {
        const struct LoadedVertex* const in[3] = { v1, v2, v3 };

        if (gfx_seal_seams(in, sealed)) {
            v1 = &sealed[0];
            v2 = &sealed[1];
            v3 = &sealed[2];
        }
    }

    // G_TEXGEN_FACE_EXT: a flat surface with no normals of its own (a room's
    // vertex colours are colours) is lit and texgenned from the triangle's
    // normal, built from its corners' model positions and turned towards the
    // eye so either winding reflects. Copies, since corners are shared.
    if ((rsp.extra_geometry_mode & G_TEXGEN_FACE_EXT) && (rsp.geometry_mode & G_LIGHTING)) {
        struct LoadedVertex f[3] = { *v1, *v2, *v3 };
        const float e1[3] = { f[1].env[3] - f[0].env[3], f[1].env[4] - f[0].env[4], f[1].env[5] - f[0].env[5] };
        const float e2[3] = { f[2].env[3] - f[0].env[3], f[2].env[4] - f[0].env[4], f[2].env[5] - f[0].env[5] };
        const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

        if (len > 1e-6f) {
            const float (*mv)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
            float facing = 0;

            for (int c = 0; c < 3; c++) {
                const float ne = n[0] * mv[0][c] + n[1] * mv[1][c] + n[2] * mv[2][c];
                const float pe = f[0].env[3] * mv[0][c] + f[0].env[4] * mv[1][c] + f[0].env[5] * mv[2][c] + mv[3][c];
                facing += ne * pe;
            }

            const float s = (facing > 0 ? -127.0f : 127.0f) / len;

            for (int i = 0; i < 3; i++) {
                float U = f[i].u, V = f[i].v;
                gfx_light_vertex(&f[i], f[i].env[3], f[i].env[4], f[i].env[5], n[0] * s, n[1] * s, n[2] * s, &U, &V);
                f[i].u = U;
                f[i].v = V;
            }
        }

        gfx_emit_prepare();
        gfx_emit_tri3(&f[0], &f[1], &f[2], is_rect);
        return;
    }

    gfx_emit_prepare();
    gfx_emit_tri3(v1, v2, v3, is_rect);
}

static bool gfx_vertices_lost;   // the last vertex load was refused, and so are its triangles
// G_MESH_EXT, below: whether the triangle was the GPU's to draw
static const struct gfxmeshdraw* mesh_cur;
static bool mesh_room; // mesh_cur is a room's (gfxmesh.room): kept runs or the CPU, below
static bool gfx_mesh_tri(uint8_t a, uint8_t b, uint8_t c);

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    if (mesh_cur && gfx_mesh_tri(vtx1_idx, vtx2_idx, vtx3_idx)) {
        return;
    }

    struct LoadedVertex* v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &rsp.loaded_vertices[vtx3_idx];

    gfx_sp_tri_emit(v1, v2, v3, is_rect);
}

static inline void gfx_sp_tri4(Gfx *cmd) {
    // the game issues gSPTri2 for quads, which uses G_TRI4 with 2 empty triangles
    static const uint8_t xs[4] = { 0, 8, 16, 24 };

    for (int k = 0; k < 4; k++) {
        const uint8_t x = C1(xs[k], 4);
        const uint8_t y = C1(xs[k] + 4, 4);
        const uint8_t z = C0(k * 4, 4);

        if (x || y || z) {
            gfx_seal_this = C0(16 + k, 1);
            gfx_sp_tri1(x, y, z, false);
        }
    }

    gfx_seal_this = false;
}

/*
 * G_MESH_EXT: a mesh drawn from the GPU's copy of it (gfxmesh.h).
 *
 * Between a gSPMeshEXT() and the one of NULL that ends it, a G_VTX that loads
 * the mesh's own vertices does no work here: the slots it fills remember the
 * vertices' places in the mesh, and the triangles over them are gathered as
 * indices (gfx_mesh_tri()) until a command that is not part of the geometry
 * comes along. Then the run goes to the backend as one draw (gfx_mesh_flush()),
 * with what gfx_sp_load_vertex() and gfx_emit_vertex() would have worked out
 * for every vertex - the transform, the aspect and the jitter, the fog line,
 * the texture coordinates, the combiner's inputs - handed to the vertex shader
 * as the parameters of the draw (gfx_mesh_params()), and the shader poses the
 * bind pose by the draw's palette first. A list holds no state change inside
 * a run, so the state a run is drawn under is the state its vertices were
 * loaded under, as the CPU would have had it. Culling is the GPU's, by the
 * winding gfx_tri_is_culled() would have dropped (gfx_mesh_cull()).
 *
 * What the vertex shader does not do is loaded on the CPU as any list is,
 * posed here from the bind pose first (gfx_mesh_load_cpu()): lighting and
 * texgen (the sheens' passes), the reflection's per-pixel normals, a face's
 * own texgen, the sky's unclipped lists and the rooms' sealed seams.
 */
extern "C" {
// Video.GpuVertices
bool gfx_gpu_vertices = true;
// The last frame's meshes drawn by the GPU, their triangles, and the
// triangles a backend would not draw, which went to the CPU instead
uint32_t g_GfxMeshDraws = 0, g_GfxMeshTris = 0, g_GfxMeshRefused = 0;
// The same for rooms, and a room's triangles drawn on the CPU after all: those
// of runs the GPU cannot draw, and those sealed (G_SEAL_SEAMS_EXT)
uint32_t g_GfxRoomDraws = 0, g_GfxRoomTris = 0, g_GfxRoomRefused = 0, g_GfxRoomCpuTris = 0, g_GfxRoomSealedTris = 0;
}

/*
 * A run of a mesh's list - its loads and triangles between two commands that
 * are not geometry - read once (gfx_mesh_read_run()), its triangles kept with
 * the GPU's copy of the mesh, so a draw of it later skips the run whole. The
 * lists do not change once built, so a run is the same every frame; what can
 * change is what the segments name, which is checked each time.
 */
struct GfxMeshRun {
    const Gfx* end;               // the first command past it
    uintptr_t vtxseg;             // what its loads' segment held when it was read: the mesh's vertices
    uint8_t vtxsegno, colseg;     // the segments its G_VTXs and G_COLs name
    uint32_t lastcol;             // the last G_COL's offset into its colours, in entries
    int32_t lastbase, lastcount;  // the last load, which the slots hold after the run
    uint32_t numverts;            // vertices it loads, for the stats
    uint32_t first;               // where its triangles start among the mesh's kept indices; UINT32_MAX when not kept
    std::vector<uint32_t> indices;
    bool ok;                      // drawable this way at all
};

struct GfxRoomData;

struct GfxMeshEntry {
    std::unordered_map<const Gfx*, GfxMeshRun> runs;
    std::unique_ptr<GfxRoomData> room; // a room's (gfxmesh.room), made with the entry
    uint32_t backend;    // the backend's name for its copy, 0 when it could not make one
    const Vtx* vertices; // what the copy was made from: a mesh rebuilt in place is made again
    int32_t numvertices;
    const float* bindpos;
    const float* normals;
    uint32_t last_frame;
};

#define GFX_MESH_IDLE_FRAMES 1800

static std::unordered_map<uint32_t, GfxMeshEntry> gfx_meshes;
static uint32_t gfx_mesh_next_id = 1;
static uint32_t gfx_mesh_frame;
static GfxMeshEntry* mesh_entry;            // mesh_cur's copy on the GPU, NULL when it has none
static int32_t mesh_slot[MAX_VERTICES + 4]; // a loaded slot's vertex in the mesh, -1 when the CPU loaded it
static std::vector<uint32_t> mesh_run;      // the triangles gathered for one draw, three indices each
static const uint8_t* mesh_run_colours;     // the colour array they read, by vertex index
static uint32_t mesh_mixed;                 // triangles over slots of both kinds, dropped (never seen)
static const Gfx* mesh_skip_until;          // a run that cannot be drawn whole, interpreted until here

static bool gfx_mesh_backend(void) {
    return gfx_gpu_vertices && gfx_rapi->mesh_supported && gfx_rapi->mesh_create && gfx_rapi->mesh_draw &&
           gfx_rapi->mesh_supported();
}

extern "C" s32 gfxMeshGpuAvailable(void) {
    return gfx_rapi != NULL && gfx_mesh_backend();
}

extern "C" void gfxMeshForget(struct gfxmesh* mesh) {
    if (!mesh || !mesh->id) {
        return;
    }

    auto it = gfx_meshes.find(mesh->id);

    if (it != gfx_meshes.end()) {
        if (it->second.backend && gfx_rapi && gfx_rapi->mesh_delete) {
            gfx_rapi->mesh_delete(it->second.backend);
        }
        if (mesh_entry == &it->second) {
            mesh_entry = NULL;
        }
        gfx_meshes.erase(it);
    }

    mesh->id = 0;
}

/*
 * The GPU's copy of a mesh, made the first time it is drawn. A rigid mesh's
 * positions are its Vtx's; a skinned one's are the bind pose as it was read,
 * before it was rounded to the s16 a Vtx holds.
 */
static void gfx_room_init(GfxMeshEntry* e, const struct gfxmesh* mesh);
static void gfx_room_start(void);

static GfxMeshEntry* gfx_mesh_entry(struct gfxmesh* mesh) {
    if (mesh->id) {
        auto it = gfx_meshes.find(mesh->id);

        if (it != gfx_meshes.end()) {
            GfxMeshEntry* e = &it->second;

            if (e->vertices == mesh->vertices && e->numvertices == mesh->numvertices && e->bindpos == mesh->bindpos &&
                e->normals == mesh->normals) {
                e->last_frame = gfx_mesh_frame;
                return e->backend ? e : NULL;
            }

            gfxMeshForget(mesh);
        }
    }

    const bool skinned = mesh->bindpos && mesh->weights && mesh->bones;
    std::vector<GfxMeshVertex> verts(mesh->numvertices);

    for (int32_t i = 0; i < mesh->numvertices; i++) {
        GfxMeshVertex* o = &verts[i];
        const Vtx* v = &mesh->vertices[i];

        if (skinned) {
            const uint8_t* b = &mesh->bones[i * 4];

            for (int k = 0; k < 3; k++) {
                o->pos[k] = mesh->bindpos[i * 3 + k];
                o->weights[k] = mesh->weights[i * 3 + k];
                o->bones[k] = b[k] < mesh->nummatrices ? b[k] : 0;
            }
            o->bones[3] = b[3];
        } else {
            for (int k = 0; k < 3; k++) {
                o->pos[k] = v->v[k];
                o->weights[k] = k == 0 ? 1.0f : 0.0f;
                o->bones[k] = 0;
            }
            o->bones[3] = 1;
        }

        o->st[0] = v->s;
        o->st[1] = v->t;

        for (int k = 0; k < 3; k++) {
            o->normal[k] = mesh->normals ? mesh->normals[i * 3 + k] : 0.0f;
        }
    }

    if (gfx_mesh_next_id == 0) {
        gfx_mesh_next_id = 1;
    }

    const uint32_t id = gfx_mesh_next_id++;
    GfxMeshEntry e;

    e.backend = gfx_rapi->mesh_create(verts.data(), (uint32_t)verts.size());
    e.vertices = mesh->vertices;
    e.numvertices = mesh->numvertices;
    e.bindpos = mesh->bindpos;
    e.normals = mesh->normals;
    e.last_frame = gfx_mesh_frame;

    mesh->id = id;

    if (mesh->room) {
        gfx_room_init(&e, mesh);
    }

    GfxMeshEntry* ins = &gfx_meshes.emplace(id, std::move(e)).first->second;

    return ins->backend ? ins : NULL;
}

// A new frame: nothing carries over, and copies nothing has drawn for a while go
static void gfx_room_start_frame(void);

static void gfx_mesh_start_frame(void) {
    gfx_mesh_frame++;
    gfx_room_start_frame();
    mesh_cur = NULL;
    mesh_room = false;
    mesh_entry = NULL;
    mesh_run.clear();
    mesh_skip_until = NULL;

    if ((gfx_mesh_frame & 255) == 0) {
        for (auto it = gfx_meshes.begin(); it != gfx_meshes.end();) {
            if (gfx_mesh_frame - it->second.last_frame > GFX_MESH_IDLE_FRAMES) {
                if (it->second.backend && gfx_rapi->mesh_delete) {
                    gfx_rapi->mesh_delete(it->second.backend);
                }
                // the mesh still holds the id, and finds nothing under it
                // the next time it is drawn, so it is made again then
                it = gfx_meshes.erase(it);
            } else {
                ++it;
            }
        }
    }
}

static void gfx_mesh_begin(const struct gfxmeshdraw* draw) {
    mesh_cur = draw && draw->mesh && draw->mesh->vertices && draw->mesh->numvertices > 0 ? draw : NULL;
    mesh_entry = NULL;
    mesh_skip_until = NULL;
    // A room needs its colour table: without one its lists are drawn as any are
    mesh_room = mesh_cur && mesh_cur->mesh->room;

    if (mesh_room && (!draw->colours || draw->numcolours <= 0)) {
        mesh_cur = NULL;
        mesh_room = false;
    }

    if (mesh_cur && gfx_mesh_backend() && (!draw->palette || draw->mesh->nummatrices <= GFX_MESH_PALETTE_MAX)) {
        mesh_entry = gfx_mesh_entry(draw->mesh);

        if (mesh_entry && mesh_room != (mesh_entry->room != nullptr)) {
            mesh_entry = NULL;
        }
    }

    for (size_t i = 0; i < sizeof(mesh_slot) / sizeof(mesh_slot[0]); i++) {
        mesh_slot[i] = -1;
    }

    if (mesh_room && mesh_entry) {
        gfx_room_start();
    }
}

// Whether the vertex shader can stand in for gfx_sp_load_vertex() under the
// state the RSP is in
static inline bool gfx_mesh_gpu_state(void) {
    return !(rsp.extra_geometry_mode & (G_TEXGEN_FACE_EXT | G_NO_CLIPPING_EXT | G_SEAL_SEAMS_EXT));
}

// A skinned mesh lit or reflecting: its normal is its own, posed, not its
// colour's three bytes (which a mesh drawn this way leaves at nothing)
static inline bool gfx_mesh_posed_normals(void) {
    return mesh_cur->palette && mesh_cur->mesh->bindpos && mesh_cur->mesh->normals &&
           ((rsp.geometry_mode & G_LIGHTING) || (rsp.extra_geometry_mode & G_ENVMAP_EXT));
}

// Vertex i of the mesh in the pose: xblaMeshPose()'s sum, unrounded
static inline void gfx_mesh_skin(const struct gfxmesh* m, const float* pal, int32_t i, float out[3]) {
    const float* p = &m->bindpos[i * 3];
    const float* w = &m->weights[i * 3];
    const uint8_t* b = &m->bones[i * 4];
    const int num = b[3] < 1 ? 1 : b[3] < 3 ? b[3] : 3;
    float x = 0.0f, y = 0.0f, z = 0.0f;

    for (int j = 0; j < num; j++) {
        const float* r = &pal[(b[j] < m->nummatrices ? b[j] : 0) * GFXMESH_PALETTE_FLOATS];

        x += (r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3]) * w[j];
        y += (r[4] * p[0] + r[5] * p[1] + r[6] * p[2] + r[7]) * w[j];
        z += (r[8] * p[0] + r[9] * p[1] + r[10] * p[2] + r[11]) * w[j];
    }

    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// Vertex i's normal in the pose, 127 long and rounded as a colour's byte is
// (xblaMeshEnvironmentVertices())
static inline void gfx_mesh_skin_normal(const struct gfxmesh* m, const float* pal, int32_t i, struct NormalColor* out) {
    const float* n = &m->normals[i * 3];
    const float* w = &m->weights[i * 3];
    const uint8_t* b = &m->bones[i * 4];
    const int num = b[3] < 1 ? 1 : b[3] < 3 ? b[3] : 3;
    float o[3] = { 0.0f, 0.0f, 0.0f };

    for (int j = 0; j < num; j++) {
        const float* r = &pal[(b[j] < m->nummatrices ? b[j] : 0) * GFXMESH_PALETTE_FLOATS];

        o[0] += (r[0] * n[0] + r[1] * n[1] + r[2] * n[2]) * w[j];
        o[1] += (r[4] * n[0] + r[5] * n[1] + r[6] * n[2]) * w[j];
        o[2] += (r[8] * n[0] + r[9] * n[1] + r[10] * n[2]) * w[j];
    }

    const float len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    int8_t* dst[3] = { &out->x, &out->y, &out->z };

    for (int k = 0; k < 3; k++) {
        const float f = (len > 1e-6f ? o[k] / len : o[k]) * 127.0f;
        *dst[k] = (int8_t)(f < 0 ? f - 0.5f : f + 0.5f);
    }
}

// One of the mesh's vertices through gfx_sp_load_vertex(), posed first
static inline void gfx_mesh_load_one(struct LoadedVertex* d, int32_t i, const struct NormalColor* vcn) {
    const struct gfxmesh* m = mesh_cur->mesh;
    const Vtx* v = &m->vertices[i];
    const short U = v->s * rsp.texture_scaling_factor.s >> 16;
    const short V = v->t * rsp.texture_scaling_factor.t >> 16;
    struct NormalColor posed;
    float p[3];

    if (mesh_cur->palette && m->bindpos && m->weights && m->bones) {
        gfx_mesh_skin(m, mesh_cur->palette, i, p);

        if (gfx_mesh_posed_normals()) {
            gfx_mesh_skin_normal(m, mesh_cur->palette, i, &posed);
            posed.w = vcn->w;
            vcn = &posed;
        }
    } else {
        p[0] = v->v[0];
        p[1] = v->v[1];
        p[2] = v->v[2];
    }

    gfx_sp_load_vertex(d, p[0], p[1], p[2], vcn, U, V);
}

static void gfx_mesh_flush(void);

/*
 * A G_VTX under G_MESH_EXT: false when it names something other than the
 * mesh's vertices, which is loaded the way any list's are.
 */
static bool gfx_mesh_load(const Vtx* src, size_t count, size_t dest) {
    const struct gfxmesh* m = mesh_cur->mesh;

    // A room's run that was not kept is loaded as any list's is
    if (mesh_room) {
        return false;
    }

    if (src < m->vertices || src + count > m->vertices + m->numvertices || dest + count > MAX_VERTICES) {
        return false;
    }

    const int32_t base = (int32_t)(src - m->vertices);

    g_GfxNumVerts += count;

    // The colours by vertex index: xblaMeshWriteBatches()'s layout, checked
    // at both ends of the load
    if (mesh_entry && count > 0 && dest == 0 && rsp.vertex_colors && gfx_mesh_gpu_state() &&
        src[0].colour == 0 && src[count - 1].colour == (count - 1) * 4) {
        const uint8_t* colours = (const uint8_t*)(rsp.vertex_colors - base);

        if (!mesh_run.empty() && colours != mesh_run_colours) {
            gfx_mesh_flush();
        }

        mesh_run_colours = colours;

        for (size_t i = 0; i < count; i++) {
            mesh_slot[dest + i] = base + (int32_t)i;
        }

        return true;
    }

    for (size_t i = 0; i < count; i++) {
        gfx_mesh_load_one(&rsp.loaded_vertices[dest + i], base + (int32_t)i, &rsp.vertex_colors[src[i].colour >> 2]);
        mesh_slot[dest + i] = -1;
    }

    return true;
}

// Slots a plain G_VTX loaded under G_MESH_EXT
static void gfx_mesh_cpu_slots(size_t dest, size_t count) {
    for (size_t i = dest; i < dest + count && i < MAX_VERTICES; i++) {
        mesh_slot[i] = -1;
    }
}

static void gfx_room_materialise(uint8_t slot);

static bool gfx_mesh_tri(uint8_t a, uint8_t b, uint8_t c) {
    // A room's triangle on the CPU: a corner an earlier run left on the GPU
    // alone is loaded here first
    if (mesh_room) {
        const uint8_t corners[3] = { a, b, c };

        for (int i = 0; i < 3; i++) {
            if (corners[i] < MAX_VERTICES && mesh_slot[corners[i]] >= 0) {
                gfx_room_materialise(corners[i]);
            }
        }

        g_GfxRoomCpuTris++;
        return false;
    }

    const int32_t ia = mesh_slot[a];
    const int32_t ib = mesh_slot[b];
    const int32_t ic = mesh_slot[c];

    if ((ia | ib | ic) < 0) {
        if (ia >= 0 || ib >= 0 || ic >= 0) {
            // corners the GPU has and corners it has not: no list does this
            mesh_mixed++;
            return true;
        }
        return false;
    }

    mesh_run.push_back((uint32_t)ia);
    mesh_run.push_back((uint32_t)ib);
    mesh_run.push_back((uint32_t)ic);

    return true;
}

/*
 * Which winding the run's triangles are dropped for, as gfx_tri_is_culled()
 * decides it per triangle: 1 clockwise as emitted, -1 anticlockwise, 0
 * neither. The CPU's cross product is negative for a triangle wound
 * anticlockwise in clip space, so G_CULL_BACK drops the clockwise ones; the
 * emitted y is flipped when the target is (invert_y), which turns the winding.
 */
static int8_t gfx_mesh_cull(void) {
    if ((rsp.geometry_mode & G_CULL_BOTH) == 0) {
        return 0;
    }
    if ((rsp.extra_geometry_mode & G_NO_CULLING_EXT) &&
        ((rdp.other_mode_l & Z_UPD) || (rsp.extra_geometry_mode & (G_DEPTH_PREPASS_EXT | G_DEPTH_FRONT_EXT)))) {
        return 0;
    }

    int8_t cull = (rsp.geometry_mode & G_CULL_BOTH) == G_CULL_FRONT ? -1 : 1;

    if (rsp.extra_geometry_mode & G_INVERT_CULLING_EXT) {
        cull = -cull;
    }
    if (batch.clip_parameters.invert_y) {
        cull = -cull;
    }

    return cull;
}

static inline float gfx_mesh_kind(uint8_t kind) {
    switch (kind) {
        case EMIT_IN_SHADE:
            return GFX_MESH_IN_SHADE;
        case EMIT_IN_SHADE_ALPHA:
            return GFX_MESH_IN_SHADE_ALPHA;
        case EMIT_IN_LOD_FRACTION:
            return GFX_MESH_IN_LOD;
        default:
            return GFX_MESH_IN_CONST;
    }
}

/*
 * The draw's parameters (GFX_MESH_PARAMS in gfx_rendering_api.h): everything
 * gfx_sp_load_vertex() and gfx_emit_vertex() read that is the same for every
 * vertex of the run, from the same state they read it from.
 */
static void gfx_mesh_params(float* P, bool skinned) {
    memset(P, 0, sizeof(float) * 4 * GFX_MESH_PARAMS);
    memcpy(P, rsp.MP_matrix, sizeof(rsp.MP_matrix));

    // gfx_adjust_x_for_aspect_ratio()
    P[16] = rsp.aspect_ofs;
    P[17] = rsp.aspect_scale;
    P[18] = gfx_current_dimensions.aspect_ratio;
    P[19] = fbActive ? 0.0f : 1.0f;

    const bool jitter = taa_active && !fbActive;
    P[20] = jitter ? taa_jx : 0.0f;
    P[21] = jitter ? taa_jy : 0.0f;
    P[22] = batch.clip_parameters.invert_y ? -1.0f : 1.0f;
    P[23] = batch.clip_parameters.z_is_from_0_to_1 ? 1.0f : 0.0f;

    if (rsp.geometry_mode & G_FOG) {
        P[24] = rsp.fog_mul;
        P[25] = rsp.fog_offset;
    } else {
        // a constant factor: the fog colour's alpha
        P[24] = 0.0f;
        P[25] = rdp.fog_color.a;
    }
    P[26] = batch.fog_vertex ? 1.0f : 0.0f;

    P[28] = byte_unit.f[rdp.fog_color.r];
    P[29] = byte_unit.f[rdp.fog_color.g];
    P[30] = byte_unit.f[rdp.fog_color.b];

    for (int t = 0; t < 2; t++) {
        P[32 + t * 4 + 0] = batch.uv_scale[t][0];
        P[32 + t * 4 + 1] = batch.uv_scale[t][1];
        P[32 + t * 4 + 2] = batch.uv_ofs[t][0];
        P[32 + t * 4 + 3] = batch.uv_ofs[t][1];
        P[40 + t * 2 + 0] = batch.tex_clamp[t][0];
        P[40 + t * 2 + 1] = batch.tex_clamp[t][1];
    }

    P[44] = byte_unit.f[rdp.grayscale_color.r];
    P[45] = byte_unit.f[rdp.grayscale_color.g];
    P[46] = byte_unit.f[rdp.grayscale_color.b];
    P[47] = byte_unit.f[rdp.grayscale_color.a];

    P[48] = rsp.texture_scaling_factor.s;
    P[49] = rsp.texture_scaling_factor.t;
    P[50] = skinned ? 1.0f : 0.0f;

    for (int j = 0; j < batch.num_inputs && j < 8; j++) {
        const struct EmitInput* in = &emit_inputs.in[j];

        P[52 + j * 4 + 0] = in->rgb[0];
        P[52 + j * 4 + 1] = in->rgb[1];
        P[52 + j * 4 + 2] = in->rgb[2];
        P[52 + j * 4 + 3] = in->a;
        P[84 + j] = gfx_mesh_kind(in->rgb_kind);
        P[92 + j] = batch.use_alpha ? gfx_mesh_kind(in->a_kind) : GFX_MESH_IN_CONST;
    }

    // G_LIGHTING and G_TEXTURE_GEN: gfx_light_vertex()'s state
    const bool lit = (rsp.geometry_mode & G_LIGHTING) != 0;

    if (lit) {
        gfx_refresh_light_coeffs();
    }

    const int numlights = rsp.current_num_lights - 1 < 0 ? 0 : rsp.current_num_lights - 1 > MAX_LIGHTS ? MAX_LIGHTS : rsp.current_num_lights - 1;

    P[100] = (float)numlights;
    P[101] = lit ? 1.0f : 0.0f;
    P[102] = lit && (rsp.geometry_mode & G_TEXTURE_GEN) ? 1.0f : 0.0f;
    P[103] = (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) ? 1.0f : 0.0f;
    P[104] = (rsp.extra_geometry_mode & G_TEXGEN_EYE_EXT) ? 1.0f : 0.0f;
    P[105] = (rsp.extra_geometry_mode & G_TEXGEN_TURN_EXT) ? 1.0f : 0.0f;
    P[106] = rsp.lookat_enabled ? 1.0f : 0.0f;
    P[107] = (rsp.extra_geometry_mode & G_ENVMAP_EXT) ? 1.0f : 0.0f;
    // gfx_sp_load_vertex()'s coverage case: a lit corner's colour entry is its normal
    P[108] = lit && (rdp.other_mode_l & (ALPHA_CVG_SEL | FORCE_BL | CVG_X_ALPHA)) == ALPHA_CVG_SEL &&
                     (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_NONE
                 ? 1.0f
                 : 0.0f;
    P[109] = gfx_mesh_posed_normals() ? 1.0f : 0.0f;

    const Light_t* ambient = &rsp.current_lights[numlights];
    P[112] = ambient->col[0];
    P[113] = ambient->col[1];
    P[114] = ambient->col[2];

    for (int i = 0; i < numlights; i++) {
        P[116 + i * 4 + 0] = rsp.current_lights_coeffs[i][0];
        P[116 + i * 4 + 1] = rsp.current_lights_coeffs[i][1];
        P[116 + i * 4 + 2] = rsp.current_lights_coeffs[i][2];
        P[132 + i * 4 + 0] = rsp.current_lights[i].col[0];
        P[132 + i * 4 + 1] = rsp.current_lights[i].col[1];
        P[132 + i * 4 + 2] = rsp.current_lights[i].col[2];
    }

    for (int k = 0; k < 3; k++) {
        P[148 + k] = rsp.current_lookat_coeffs[0][k];
        P[152 + k] = rsp.current_lookat_coeffs[1][k];
    }

    P[156] = rsp.texgen_shift[0];
    P[157] = rsp.texgen_shift[1];
    P[160] = rsp.texgen_turn[0];
    P[161] = rsp.texgen_turn[1];
    P[162] = rsp.texgen_turn[2];
    P[163] = rsp.texgen_turn[3];

    memcpy(&P[164], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], sizeof(float) * 16);
}

/*
 * Triangles on the CPU after all, for a backend that would not draw them:
 * each corner loaded and posed on its own and drawn as any triangle is.
 */
static void gfx_mesh_draw_cpu(const uint32_t* indices, size_t numindices, const uint8_t* colourbytes) {
    const struct NormalColor* colours = (const struct NormalColor*)colourbytes;

    for (size_t t = 0; t + 2 < numindices; t += 3) {
        struct LoadedVertex v[3];

        for (int k = 0; k < 3; k++) {
            gfx_mesh_load_one(&v[k], (int32_t)indices[t + k], &colours[indices[t + k]]);
        }

        gfx_sp_tri_emit(&v[0], &v[1], &v[2], false);
    }
}

/*
 * One draw of the mesh's triangles under the state the RSP and RDP are in:
 * indices from the CPU, or the mesh's kept ones from first when indices is
 * NULL (cpuindices are the same, for the CPU to fall back on).
 */
static void gfx_mesh_draw(const uint32_t* indices, uint32_t numindices, uint32_t first, const uint32_t* cpuindices,
                          const uint8_t* colours) {
    const uint32_t numtris = numindices / 3;

    if (numtris == 0) {
        return;
    }

    if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_BOTH && gfx_mesh_cull() != 0) {
        g_GfxTrisCulled += numtris;
        return;
    }

    // What each triangle would have set up before it was written, and then the
    // CPU's triangles so far, which the lists drew first
    gfx_emit_prepare();
    gfx_flush();

    const bool skinned = mesh_cur->palette && mesh_cur->mesh->bindpos;
    float params[4 * GFX_MESH_PARAMS];
    gfx_mesh_params(params, skinned);

    struct GfxMeshDraw d;
    d.prg = rendering_state.shader_program;
    d.mesh = mesh_entry ? mesh_entry->backend : 0;
    d.colours = colours;
    d.numcolours = (uint32_t)mesh_cur->mesh->numvertices;
    d.indices = indices;
    d.numindices = numindices;
    d.first_index = first;
    d.params = params;
    d.palette = skinned ? mesh_cur->palette : NULL;
    d.numpalette = skinned ? (uint32_t)mesh_cur->mesh->nummatrices : 0;
    d.cull = gfx_mesh_cull();

    if (d.mesh && d.prg && gfx_rapi->mesh_draw(&d)) {
        g_GfxNumDrawCalls++;
        g_GfxNumTris += numtris;
        g_GfxMeshDraws++;
        g_GfxMeshTris += numtris;
    } else {
        g_GfxMeshRefused += numtris;
        gfx_mesh_draw_cpu(cpuindices, numindices, colours);
    }
}

static void gfx_mesh_flush(void) {
    if (mesh_run.empty()) {
        return;
    }

    gfx_mesh_draw(mesh_run.data(), (uint32_t)mesh_run.size(), 0, mesh_run.data(), mesh_run_colours);
    mesh_run.clear();
}

/*
 * Reads the run of geometry that starts at cmd, under the segments as they
 * stand: the loads must name the mesh's vertices through one segment, with
 * the colour layout gfx_mesh_load() checks, and the triangles must name only
 * what the run itself loaded.
 */
static GfxMeshRun gfx_mesh_read_run(const Gfx* cmd) {
    const struct gfxmesh* m = mesh_cur->mesh;
    GfxMeshRun r;
    int32_t slots[MAX_VERTICES];
    int32_t colofs = -1;

    r.end = cmd;
    r.vtxseg = 0;
    r.vtxsegno = 0;
    r.colseg = 0;
    r.lastcol = 0;
    r.lastbase = -1;
    r.lastcount = 0;
    r.numverts = 0;
    r.first = UINT32_MAX;
    r.ok = true;

    for (int i = 0; i < MAX_VERTICES; i++) {
        slots[i] = -1;
    }

    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES || slots[a] < 0 || slots[b] < 0 || slots[c] < 0) {
            r.ok = false;
            return;
        }
        r.indices.push_back((uint32_t)slots[a]);
        r.indices.push_back((uint32_t)slots[b]);
        r.indices.push_back((uint32_t)slots[c]);
    };

    for (const Gfx* c = cmd;; ++c) {
        const uint32_t op = c->words.w0 >> 24;
        const uintptr_t w1 = c->words.w1;

        if (op == G_NOOP) {
            continue;
        }

        if (op == G_COL) {
            const uint8_t seg = (w1 & 1) ? (uint8_t)((w1 >> 24) & 0x0f) : 0;

            if (!seg || (r.colseg && seg != r.colseg)) {
                r.ok = false;
            }

            r.colseg = seg;
            colofs = (int32_t)((w1 & 0x00fffffe) / sizeof(struct NormalColor));
            r.lastcol = (uint32_t)colofs;
            continue;
        }

        if (op == G_VTX) {
            const Vtx* src = (const Vtx*)seg_addr(w1);
            const size_t count = (c->words.w0 & 0xffff) / sizeof(Vtx);
            const size_t dest = (c->words.w0 >> 16) & 0xf;
            const uint8_t seg = (w1 & 1) ? (uint8_t)((w1 >> 24) & 0x0f) : 0;

            if (!seg || (r.vtxsegno && seg != r.vtxsegno) || dest != 0 || count == 0 || count > MAX_VERTICES ||
                src < m->vertices || src + count > m->vertices + m->numvertices) {
                r.ok = false;
                continue;
            }

            const int32_t base = (int32_t)(src - m->vertices);

            if (colofs != base || src[0].colour != 0 || src[count - 1].colour != (count - 1) * 4) {
                r.ok = false;
            }

            r.vtxsegno = seg;
            r.vtxseg = segmentPointers[seg];

            for (size_t i = 0; i < count; i++) {
                slots[i] = base + (int32_t)i;
            }

            r.lastbase = base;
            r.lastcount = (int32_t)count;
            r.numverts += (uint32_t)count;
            continue;
        }

        if (op == (uint8_t)G_TRI1) {
            tri(((w1 >> 16) & 0xff) / 10, ((w1 >> 8) & 0xff) / 10, (w1 & 0xff) / 10);
            continue;
        }

        if (op == (uint8_t)G_TRI4) {
            for (int k = 0; k < 4; k++) {
                const uint32_t x = (w1 >> (k * 8)) & 0xf;
                const uint32_t y = (w1 >> (k * 8 + 4)) & 0xf;
                const uint32_t z = (c->words.w0 >> (k * 4)) & 0xf;

                if (x || y || z) {
                    tri(x, y, z);
                }
            }
            continue;
        }

        r.end = c;
        break;
    }

    if (r.indices.empty() || !r.colseg || !r.vtxsegno || r.lastbase < 0) {
        r.ok = false;
    }

    return r;
}

/*
 * The run of a mesh's list that starts at cmd, drawn whole from the triangles
 * kept for it. Hands back the run's last command, for the interpreter to step
 * past, or NULL when the run cannot be drawn that way this time - the
 * segments name something else, or the state is one the GPU does not do -
 * and the interpreter reads it as any list until its end.
 */
static const Gfx* gfx_mesh_kept_run(const Gfx* cmd) {
    auto it = mesh_entry->runs.find(cmd);

    if (it == mesh_entry->runs.end()) {
        GfxMeshRun r = gfx_mesh_read_run(cmd);

        if (r.ok && gfx_rapi->mesh_add_indices) {
            r.first = gfx_rapi->mesh_add_indices(mesh_entry->backend, r.indices.data(), (uint32_t)r.indices.size());
        }

        it = mesh_entry->runs.emplace(cmd, std::move(r)).first;
    }

    const GfxMeshRun& r = it->second;

    if (!r.ok || segmentPointers[r.vtxsegno] != r.vtxseg || !segmentPointers[r.colseg] || !gfx_mesh_gpu_state()) {
        mesh_skip_until = r.end;
        return NULL;
    }

    // What the run leaves behind it, as reading it would have
    const uint8_t* colours = (const uint8_t*)segmentPointers[r.colseg];
    rsp.vertex_colors = (const struct NormalColor*)colours + r.lastcol;

    for (int32_t i = 0; i < r.lastcount; i++) {
        mesh_slot[i] = r.lastbase + i;
    }

    g_GfxNumVerts += r.numverts;
    gfx_vertices_lost = false;

    gfx_mesh_draw(r.first != UINT32_MAX ? NULL : r.indices.data(), (uint32_t)r.indices.size(),
                  r.first != UINT32_MAX ? r.first : 0, r.indices.data(), colours);

    return r.end - 1;
}

/*
 * G_MESH_EXT for a room (gfxmesh.room, roommesh.c): the level's rooms drawn
 * from the GPU's copy of their vertices.
 *
 * A room's lists are the level's, not built for this the way a mesh's are, so
 * none of the mesh's layout holds and its runs are read differently
 * (gfx_room_read_run()). A load may name any of the room's vertices into any
 * slot; a vertex's colour is the entry its colour byte names past the G_COL
 * before the load, anywhere in the room's colour table, which roomHighlight()
 * makes afresh every frame; and a triangle may name a slot an earlier run of
 * the list loaded - a fifth of the ROM's rooms' triangles do, across a change
 * of texture. What a run is read for:
 *
 *  - each vertex's place in the colour table, learnt the first time a run
 *    loads it (a vertex two runs load with two different colours keeps the
 *    second run on the CPU; no level seen does it). A table of up to
 *    GFXMESH_ROOM_PALETTE entries - every Perfect Dark room and every HD
 *    one - goes to the vertex shader whole with each draw, and the vertex
 *    carries its place in it (in its bone bytes, which a room does not use).
 *    A bigger one - a GoldenEye room converted from the ROM gives every
 *    vertex its own - is gathered into a colour per vertex once a frame, or
 *    named as it stands when each vertex's entry is its own index plus one
 *    offset (gfx_room_colours()).
 *  - its triangles, kept with the room's copy, as for a mesh: the list's
 *    order, and when some are marked for sealing (bgMarkRoomSeams()), those
 *    that are not apart.
 *  - the slots it leaves loaded, and the slots it reads that an earlier run
 *    loaded: those must hold the same vertices when the run is drawn, loaded
 *    under the same state (gfx_room_vstate() - the GPU works a vertex out
 *    under the state of the draw, the RSP under the state of its load).
 *
 * Drawn on the CPU as any list is: a run the GPU cannot draw at all or under
 * the state of the moment - per-vertex fog (whose RSP clipping the shader
 * does not do, on a converted GoldenEye level in the N64 look), a face's own
 * texgen, the sky's unclipped lists - and a run over a vertex dyntex moves
 * (gfx_room_start()). A triangle such a run draws over a slot a kept run left
 * on the GPU alone has that corner loaded first (gfx_room_materialise()), and
 * the kept run is drawn on the CPU from then on, so its loads happen where
 * the list has them. A run's sealed triangles are drawn on the CPU while
 * sealing is on (gfx_seal_seams() works on the whole triangle on the screen),
 * after the rest of the run.
 */
struct GfxRoomRun {
    const Gfx* end;               // the first command past it
    bool ok;                      // drawable on the GPU at all, from what its commands hold
    bool forced_cpu;              // a run after it needed its loads on the CPU
    uint8_t vtxsegno;             // the segment its loads name, 0 when it loads nothing
    uintptr_t vtxseg;             // and what that held: the room's block's vertices
    uint8_t colseg;               // the segment its G_COLs name, 0 when it has none
    intptr_t coldelta;            // where that segment stood in the colour table, bytes
    uint32_t outcolofs;           // its last G_COL's offset in the segment, which the RSP is left at
    bool incol;                   // it loads under the colours a G_COL before it named
    intptr_t incoldelta;          // and where those stood in the table, bytes
    uint32_t loaded;              // the slots its loads leave filled
    uint32_t incoming;            // the slots it reads that it does not load
    int32_t slotout[16];          // the vertex each slot it loads is left holding
    int32_t slotin[16];           // the vertex each incoming slot must hold
    uint32_t numverts;            // vertices it loads, for the stats
    uint32_t numsealed;           // triangles marked for sealing
    uint32_t first_all;           // where all its triangles are kept with the room's copy; UINT32_MAX when not
    uint32_t first_plain;         // and those not marked
    uint32_t animgen;             // the room's animgen it was last checked against
    float bmin[3], bmax[3];       // the box round its triangles' corners, in the room's space
    std::vector<uint32_t> all;    // its triangles in the list's order, three vertices each
    std::vector<uint32_t> plain;  // those not marked for sealing (only when some are)
    std::vector<uint32_t> sealed; // those that are
};

struct GfxRoomData {
    std::unordered_map<const Gfx*, GfxRoomRun> runs;
    std::vector<int32_t> colidx; // each vertex's entry in the colour table, -1 until a run loads it
    bool palette;                // the table goes to the vertex shader whole
    int64_t direct;              // every colidx known is its vertex's index plus this; INT64_MIN when not, or none known
    bool anyknown;
    std::vector<uint32_t> st;    // a dynamic room's s and t as last seen
    std::vector<uint8_t> animated;
    uint32_t animgen;            // bumped when a vertex is first seen to move
    uint32_t stframe;
    // This frame's colours as the draws read them (gfx_room_colours())
    uint32_t frame;
    const void* table;
    const uint8_t* colours;
    const float* pal;
    uint32_t numpal;
};

// What gfx_sp_load_vertex() reads of the state, unlit: a slot an earlier run
// loaded is good for a GPU draw only when its load was made under the same
struct GfxRoomVState {
    float mp[4][4];
    float aspect[3];
    float jitter[2];
    float fog[2];
    uint32_t flags;
    uint16_t tex[2];
};

static int32_t mesh_slot_sig[MAX_VERTICES + 4];        // a GPU slot's load state in room_sigs, -1 when unknown
static GfxRoomRun* mesh_slot_run[MAX_VERTICES + 4];    // the kept run that loaded it
static std::vector<GfxRoomVState> room_sigs;          // the room draw's load states so far
// Colours and palettes made for this frame's draws: a buffer each, reused
// next frame, so a draw's pointer names what it was made with for the frame
static std::vector<std::vector<uint8_t>> room_bufs;
static size_t room_bufs_used;

static void* gfx_room_alloc(size_t bytes) {
    if (room_bufs_used == room_bufs.size()) {
        room_bufs.emplace_back();
    }

    std::vector<uint8_t>& b = room_bufs[room_bufs_used++];
    b.resize(bytes);

    return b.data();
}

static void gfx_room_start_frame(void) {
    room_bufs_used = 0;
    room_sigs.clear();
}

static void gfx_room_init(GfxMeshEntry* e, const struct gfxmesh* mesh) {
    GfxRoomData* rd = new GfxRoomData();
    const int32_t n = mesh->numvertices;

    rd->colidx.assign(n, -1);
    rd->palette = mesh_cur && mesh_cur->numcolours <= GFXMESH_ROOM_PALETTE;
    rd->direct = INT64_MIN;
    rd->anyknown = false;
    rd->animgen = 0;
    rd->stframe = 0;
    rd->frame = 0;
    rd->table = NULL;
    rd->colours = NULL;
    rd->pal = NULL;
    rd->numpal = 0;
    rd->st.resize(n);
    rd->animated.assign(n, 0);

    for (int32_t k = 0; k < n; k++) {
        rd->st[k] = (uint16_t)mesh->vertices[k].s | ((uint32_t)(uint16_t)mesh->vertices[k].t << 16);
    }

    e->room.reset(rd);
}

static void gfx_room_vstate(GfxRoomVState* s) {
    const bool jitter = taa_active && !fbActive;

    memset(s, 0, sizeof(*s));
    memcpy(s->mp, rsp.MP_matrix, sizeof(s->mp));
    s->aspect[0] = rsp.aspect_ofs;
    s->aspect[1] = rsp.aspect_scale;
    s->aspect[2] = gfx_current_dimensions.aspect_ratio;
    s->jitter[0] = jitter ? taa_jx : 0.0f;
    s->jitter[1] = jitter ? taa_jy : 0.0f;
    s->flags = (rsp.geometry_mode & (G_LIGHTING | G_FOG)) | (fbActive ? 1u << 31 : 0) |
               ((rsp.extra_geometry_mode & (G_TEXGEN_FACE_EXT | G_ENVMAP_EXT)) ? 1u << 30 : 0);
    s->tex[0] = rsp.texture_scaling_factor.s;
    s->tex[1] = rsp.texture_scaling_factor.t;

    if (rsp.geometry_mode & G_FOG) {
        s->fog[0] = rsp.fog_mul;
        s->fog[1] = rsp.fog_offset;
    } else {
        s->fog[0] = rdp.fog_color.a;
    }
}

/*
 * A room's draw begins: the slots know nothing of its runs yet, and a room
 * dyntex animates has its s and t looked over once a frame - a vertex seen
 * to move is the CPU's from then on, and so is any run that loads or draws it.
 */
static void gfx_room_start(void) {
    GfxRoomData* rd = mesh_entry->room.get();
    const struct gfxmesh* m = mesh_cur->mesh;

    room_sigs.clear();

    for (size_t i = 0; i < sizeof(mesh_slot_sig) / sizeof(mesh_slot_sig[0]); i++) {
        mesh_slot_sig[i] = -1;
        mesh_slot_run[i] = NULL;
    }

    if (m->dynamic && rd->stframe != gfx_mesh_frame) {
        rd->stframe = gfx_mesh_frame;

        for (int32_t k = 0; k < m->numvertices; k++) {
            const uint32_t st = (uint16_t)m->vertices[k].s | ((uint32_t)(uint16_t)m->vertices[k].t << 16);

            if (st != rd->st[k]) {
                rd->st[k] = st;

                if (!rd->animated[k]) {
                    rd->animated[k] = 1;
                    rd->animgen++;
                }
            }
        }
    }
}

// One of the room's vertices through gfx_sp_load_vertex(), its colour the
// entry a run learnt for it
static inline void gfx_room_load_one(struct LoadedVertex* d, int32_t k) {
    const Vtx* v = &mesh_cur->mesh->vertices[k];
    const int32_t c = mesh_entry ? mesh_entry->room->colidx[k] : -1;
    static const struct NormalColor none = {};
    const struct NormalColor* vcn = c >= 0 && c < mesh_cur->numcolours ? &((const struct NormalColor*)mesh_cur->colours)[c] : &none;
    const short U = v->s * rsp.texture_scaling_factor.s >> 16;
    const short V = v->t * rsp.texture_scaling_factor.t >> 16;

    gfx_sp_load_vertex(d, v->v[0], v->v[1], v->v[2], vcn, U, V);
}

/*
 * A CPU triangle names a slot that a kept run filled on the GPU alone: the
 * vertex is loaded into it now, and the run that filled it is drawn on the
 * CPU from the next frame on, so the load happens where the list has it and
 * under the state it had there.
 */
static void gfx_room_materialise(uint8_t slot) {
    const int32_t k = mesh_slot[slot];

    if (mesh_slot_run[slot]) {
        mesh_slot_run[slot]->forced_cpu = true;
    }

    mesh_slot[slot] = -1;

    if (k >= 0 && k < mesh_cur->mesh->numvertices) {
        gfx_room_load_one(&rsp.loaded_vertices[slot], k);
    }
}

/*
 * Under G_FOG_VERTEX_EXT a triangle crossing the RSP's clip volume - behind
 * the eye, or past the guard band - is cut on the CPU, and its new corners
 * fogged where they stand (gfx_emit_tri3()). The shader fogs each corner as
 * it is and cuts nothing, which is the same thing for a triangle wholly
 * inside: so is every triangle of a run whose box has all eight corners
 * inside every plane of it, the volume being convex.
 */
static bool gfx_room_inside_rsp_clip(const struct GfxRoomRun& r) {
    const bool jitter = taa_active && !fbActive;

    for (int c = 0; c < 8; c++) {
        const float px = (c & 1) ? r.bmax[0] : r.bmin[0];
        const float py = (c & 2) ? r.bmax[1] : r.bmin[1];
        const float pz = (c & 4) ? r.bmax[2] : r.bmin[2];
        const v4f pos = v4f_splat(px) * v4f_load(rsp.MP_matrix[0]) + v4f_splat(py) * v4f_load(rsp.MP_matrix[1]) +
                        v4f_splat(pz) * v4f_load(rsp.MP_matrix[2]) + v4f_load(rsp.MP_matrix[3]);
        struct LoadedVertex v;

        v.w = pos[3];
        v.x = gfx_adjust_x_for_aspect_ratio(pos[0], v.w);
        v.y = pos[1];

        if (jitter) {
            v.x += taa_jx * v.w;
            v.y += taa_jy * v.w;
        }

        for (int k = 0; k < 5; k++) {
            // a hair inside, against the CPU's arithmetic and the shader's
            // disagreeing at the edge
            if (!(gfx_rsp_clip_dist(&v, k) > 1e-3f * fabsf(v.w) + 1e-4f)) {
                return false;
            }
        }
    }

    return true;
}

// The run's draw can be the GPU's under the state of the moment
static inline bool gfx_room_gpu_state(const struct GfxRoomRun& r) {
    if (rsp.extra_geometry_mode & (G_TEXGEN_FACE_EXT | G_NO_CLIPPING_EXT)) {
        return false;
    }

    return !(rsp.extra_geometry_mode & G_FOG_VERTEX_EXT) || gfx_room_inside_rsp_clip(r);
}

// gfx_sp_tri_emit() would grow a marked triangle
static inline bool gfx_room_sealing(void) {
    return (rsp.extra_geometry_mode & G_SEAL_SEAMS_EXT) && g_GfxSealSeams > 0 && (rdp.other_mode_l & Z_UPD) &&
           !(rdp.other_mode_l & FORCE_BL) && (rdp.other_mode_l & ZMODE_DEC) != ZMODE_DEC &&
           !(rsp.extra_geometry_mode & G_DECAL_EXT);
}

/*
 * The vertices a run has just taught their colours: the GPU's copy carries
 * each one's entry where the table goes to the shader whole. Only vertices no
 * draw has read yet - a vertex is only drawn by a kept run, and every run that
 * draws one has been read by then.
 */
static void gfx_room_teach(const std::vector<int32_t>& taught) {
    GfxRoomData* rd = mesh_entry->room.get();
    const struct gfxmesh* m = mesh_cur->mesh;

    if (taught.empty()) {
        return;
    }

    // the frame's colours are made again for the next draw
    rd->frame = 0;

    if (!rd->palette || !gfx_rapi->mesh_update) {
        return;
    }

    std::vector<int32_t> ks(taught);
    std::sort(ks.begin(), ks.end());

    std::vector<GfxMeshVertex> verts;

    for (size_t i = 0; i < ks.size();) {
        size_t j = i + 1;

        while (j < ks.size() && ks[j] == ks[j - 1] + 1) {
            j++;
        }

        verts.resize(j - i);

        for (size_t n = 0; n < j - i; n++) {
            const int32_t k = ks[i + n];
            const Vtx* v = &m->vertices[k];
            GfxMeshVertex* o = &verts[n];
            const uint32_t c = (uint32_t)rd->colidx[k];

            memset(o, 0, sizeof(*o));
            o->pos[0] = v->v[0];
            o->pos[1] = v->v[1];
            o->pos[2] = v->v[2];
            o->st[0] = v->s;
            o->st[1] = v->t;
            o->bones[0] = c & 0xff;
            o->bones[1] = (c >> 8) & 0xff;
            o->bones[2] = (c >> 16) & 0xff;
            o->bones[3] = 1;
            o->weights[0] = 1.0f;
        }

        gfx_rapi->mesh_update(mesh_entry->backend, (uint32_t)ks[i], verts.data(), (uint32_t)verts.size());
        i = j;
    }
}

/*
 * Reads the room's run that starts at cmd, under the segments and slots as
 * they stand.
 */
static GfxRoomRun gfx_room_read_run(const Gfx* cmd) {
    const struct gfxmesh* m = mesh_cur->mesh;
    GfxRoomData* rd = mesh_entry->room.get();
    const uintptr_t table = (uintptr_t)mesh_cur->colours;
    const int64_t numcolours = mesh_cur->numcolours;
    GfxRoomRun r;
    int32_t slots[16];
    bool own[16];
    int64_t colbase = 0; // entries from the table's start
    bool havecol = false;
    std::vector<int32_t> taught;

    r.end = cmd;
    r.ok = true;
    r.forced_cpu = false;
    r.vtxsegno = 0;
    r.vtxseg = 0;
    r.colseg = 0;
    r.coldelta = 0;
    r.outcolofs = 0;
    r.incol = false;
    r.incoldelta = 0;
    r.loaded = 0;
    r.incoming = 0;
    r.numverts = 0;
    r.numsealed = 0;
    r.first_all = UINT32_MAX;
    r.first_plain = UINT32_MAX;
    r.animgen = rd->animgen;

    for (int i = 0; i < 16; i++) {
        slots[i] = mesh_slot[i] >= 0 ? mesh_slot[i] : -1;
        own[i] = false;
        r.slotout[i] = -1;
        r.slotin[i] = -1;
    }

    auto tri = [&](uint32_t a, uint32_t b, uint32_t c, bool seal) {
        const uint32_t s[3] = { a, b, c };

        for (int i = 0; i < 3; i++) {
            if (s[i] >= 16 || slots[s[i]] < 0) {
                r.ok = false;
                return;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (!own[s[i]]) {
                r.incoming |= 1u << s[i];
                r.slotin[s[i]] = slots[s[i]];
            }
            r.all.push_back((uint32_t)slots[s[i]]);
        }

        std::vector<uint32_t>& to = seal ? r.sealed : r.plain;

        to.push_back((uint32_t)slots[a]);
        to.push_back((uint32_t)slots[b]);
        to.push_back((uint32_t)slots[c]);

        if (seal) {
            r.numsealed++;
        }
    };

    for (const Gfx* c = cmd;; ++c) {
        const uint32_t op = c->words.w0 >> 24;
        const uintptr_t w1 = c->words.w1;

        if (op == G_NOOP) {
            continue;
        }

        if (op == G_COL) {
            const uint8_t seg = (w1 & 1) ? (uint8_t)((w1 >> 24) & 0x0f) : 0;

            if (!seg || !segmentPointers[seg] || (r.colseg && seg != r.colseg)) {
                r.ok = false;
                continue;
            }

            r.colseg = seg;
            r.coldelta = (intptr_t)(segmentPointers[seg] - table);
            r.outcolofs = (uint32_t)(w1 & 0x00fffffe);

            const intptr_t bytes = r.coldelta + (intptr_t)r.outcolofs;

            if (bytes % (intptr_t)sizeof(struct NormalColor)) {
                r.ok = false;
            }

            colbase = bytes / (intptr_t)sizeof(struct NormalColor);
            havecol = true;
            continue;
        }

        if (op == G_VTX) {
            const Vtx* src = (const Vtx*)seg_addr(w1);
            const size_t count = (c->words.w0 & 0xffff) / sizeof(Vtx);
            const size_t dest = (c->words.w0 >> 16) & 0xf;
            const uint8_t seg = (w1 & 1) ? (uint8_t)((w1 >> 24) & 0x0f) : 0;

            if (!seg || (r.vtxsegno && seg != r.vtxsegno) || count == 0 || dest + count > 16 ||
                src < m->vertices || src + count > m->vertices + m->numvertices) {
                r.ok = false;

                for (size_t i = dest; i < dest + count && i < 16; i++) {
                    slots[i] = -1;
                    own[i] = true;
                }
                continue;
            }

            if (!havecol) {
                // the colours a G_COL before the run named
                const intptr_t bytes = (intptr_t)((uintptr_t)rsp.vertex_colors - table);

                if (!rsp.vertex_colors || bytes % (intptr_t)sizeof(struct NormalColor)) {
                    r.ok = false;
                }

                r.incol = true;
                r.incoldelta = bytes;
                colbase = bytes / (intptr_t)sizeof(struct NormalColor);
                havecol = true;
            }

            r.vtxsegno = seg;
            r.vtxseg = segmentPointers[seg];

            const int32_t base = (int32_t)(src - m->vertices);

            for (size_t i = 0; i < count; i++) {
                const int32_t k = base + (int32_t)i;
                const int64_t ci = colbase + (src[i].colour >> 2);

                if (ci < 0 || ci >= numcolours || (rd->palette && ci >= GFXMESH_ROOM_PALETTE)) {
                    r.ok = false;
                } else if (rd->colidx[k] < 0) {
                    rd->colidx[k] = (int32_t)ci;
                    taught.push_back(k);

                    const int64_t d = ci - k;

                    if (!rd->anyknown) {
                        rd->direct = d;
                        rd->anyknown = true;
                    } else if (rd->direct != d) {
                        rd->direct = INT64_MIN;
                    }
                } else if (rd->colidx[k] != ci) {
                    // loaded with another colour by another run
                    r.ok = false;
                }

                slots[dest + i] = k;
                own[dest + i] = true;
                r.loaded |= 1u << (dest + i);
            }

            r.numverts += (uint32_t)count;
            continue;
        }

        if (op == (uint8_t)G_TRI1) {
            tri(((w1 >> 16) & 0xff) / 10, ((w1 >> 8) & 0xff) / 10, (w1 & 0xff) / 10, (w1 >> 24) & 1);
            continue;
        }

        if (op == (uint8_t)G_TRI4) {
            for (int k = 0; k < 4; k++) {
                const uint32_t x = (w1 >> (k * 8)) & 0xf;
                const uint32_t y = (w1 >> (k * 8 + 4)) & 0xf;
                const uint32_t z = (c->words.w0 >> (k * 4)) & 0xf;

                if (x || y || z) {
                    tri(x, y, z, (c->words.w0 >> (16 + k)) & 1);
                }
            }
            continue;
        }

        r.end = c;
        break;
    }

    for (int i = 0; i < 16; i++) {
        if (r.loaded & (1u << i)) {
            r.slotout[i] = slots[i];
        }
    }

    if (r.numsealed == 0) {
        r.plain.clear();
    }

    for (int k = 0; k < 3; k++) {
        r.bmin[k] = 1e30f;
        r.bmax[k] = -1e30f;
    }
    for (uint32_t i : r.all) {
        const Vtx* v = &m->vertices[i];

        for (int k = 0; k < 3; k++) {
            r.bmin[k] = std::min(r.bmin[k], (float)v->v[k]);
            r.bmax[k] = std::max(r.bmax[k], (float)v->v[k]);
        }
    }

    if (r.loaded == 0 && r.all.empty()) {
        r.ok = false;
    }

    gfx_room_teach(taught);

    return r;
}

/*
 * This frame's colours for the room's draws: the table as floats for the
 * shader, or a colour per vertex - the table itself where it lines up with
 * the vertices, else gathered from it.
 */
static void gfx_room_colours(void) {
    GfxRoomData* rd = mesh_entry->room.get();
    const void* table = mesh_cur->colours;

    if (rd->frame == gfx_mesh_frame && rd->table == table) {
        return;
    }

    rd->frame = gfx_mesh_frame;
    rd->table = table;

    const struct NormalColor* t = (const struct NormalColor*)table;
    const int32_t numcolours = mesh_cur->numcolours;
    const int32_t n = mesh_cur->mesh->numvertices;

    if (rd->palette) {
        const int32_t num = numcolours < GFXMESH_ROOM_PALETTE ? numcolours : GFXMESH_ROOM_PALETTE;
        float* p = (float*)gfx_room_alloc(sizeof(float) * 4 * GFXMESH_ROOM_PALETTE);

        for (int32_t i = 0; i < num; i++) {
            p[i * 4 + 0] = t[i].r;
            p[i * 4 + 1] = t[i].g;
            p[i * 4 + 2] = t[i].b;
            p[i * 4 + 3] = t[i].a;
        }

        rd->pal = p;
        rd->numpal = (uint32_t)(num + 2) / 3;
        rd->colours = NULL;
        return;
    }

    if (rd->anyknown && rd->direct != INT64_MIN && rd->direct >= 0 && rd->direct + n <= numcolours) {
        rd->colours = (const uint8_t*)(t + rd->direct);
        return;
    }

    struct NormalColor* g = (struct NormalColor*)gfx_room_alloc(sizeof(struct NormalColor) * n);

    for (int32_t k = 0; k < n; k++) {
        const int32_t c = rd->colidx[k];

        if (c >= 0 && c < numcolours) {
            g[k] = t[c];
        } else {
            memset(&g[k], 0, sizeof(g[k]));
        }
    }

    rd->colours = (const uint8_t*)g;
}

// A run's triangles on the CPU, each corner loaded on its own
static void gfx_room_draw_cpu(const std::vector<uint32_t>& idx, bool seal) {
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        struct LoadedVertex v[3];

        for (int k = 0; k < 3; k++) {
            gfx_room_load_one(&v[k], (int32_t)idx[t + k]);
        }

        gfx_seal_this = seal;
        gfx_sp_tri_emit(&v[0], &v[1], &v[2], false);
    }

    gfx_seal_this = false;
}

// Triangles of the room's kept ones, from first, as one draw
static void gfx_room_draw(const std::vector<uint32_t>& idx, uint32_t first) {
    const uint32_t numtris = (uint32_t)idx.size() / 3;
    GfxRoomData* rd = mesh_entry->room.get();

    if (numtris == 0) {
        return;
    }

    if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_BOTH && gfx_mesh_cull() != 0) {
        g_GfxTrisCulled += numtris;
        return;
    }

    gfx_emit_prepare();
    gfx_flush();
    gfx_room_colours();

    float params[4 * GFX_MESH_PARAMS];
    gfx_mesh_params(params, false);
    params[51] = rd->palette ? 1.0f : 0.0f;

    struct GfxMeshDraw d;
    d.prg = rendering_state.shader_program;
    d.mesh = mesh_entry->backend;
    d.colours = rd->palette ? NULL : rd->colours;
    d.numcolours = (uint32_t)mesh_cur->mesh->numvertices;
    d.indices = first == UINT32_MAX ? idx.data() : NULL;
    d.numindices = (uint32_t)idx.size();
    d.first_index = first == UINT32_MAX ? 0 : first;
    d.params = params;
    d.palette = rd->palette ? rd->pal : NULL;
    d.numpalette = rd->palette ? rd->numpal : 0;
    d.cull = gfx_mesh_cull();

    if (d.mesh && d.prg && (d.colours || d.palette) && gfx_rapi->mesh_draw(&d)) {
        g_GfxNumDrawCalls++;
        g_GfxNumTris += numtris;
        g_GfxRoomDraws++;
        g_GfxRoomTris += numtris;
    } else {
        g_GfxRoomRefused += numtris;
        gfx_room_draw_cpu(idx, false);
    }
}

/*
 * The room's run that starts at cmd, drawn whole from what was kept of it, or
 * NULL when it is to be read as any list is this time (see above).
 */
static const Gfx* gfx_room_kept_run(const Gfx* cmd) {
    GfxRoomData* rd = mesh_entry->room.get();
    auto it = rd->runs.find(cmd);

    if (it == rd->runs.end()) {
        GfxRoomRun r = gfx_room_read_run(cmd);

        if (r.ok && gfx_rapi->mesh_add_indices) {
            if (!r.all.empty()) {
                r.first_all = gfx_rapi->mesh_add_indices(mesh_entry->backend, r.all.data(), (uint32_t)r.all.size());
            }
            if (!r.plain.empty()) {
                r.first_plain = gfx_rapi->mesh_add_indices(mesh_entry->backend, r.plain.data(), (uint32_t)r.plain.size());
            }
        }

        it = rd->runs.emplace(cmd, std::move(r)).first;
    }

    GfxRoomRun& r = it->second;
    const uintptr_t table = (uintptr_t)mesh_cur->colours;
    bool go = r.ok && !r.forced_cpu && gfx_room_gpu_state(r);

    if (go && r.vtxsegno && segmentPointers[r.vtxsegno] != r.vtxseg) {
        go = false;
    }
    if (go && r.colseg && (intptr_t)(segmentPointers[r.colseg] - table) != r.coldelta) {
        go = false;
    }
    if (go && r.incol && (intptr_t)((uintptr_t)rsp.vertex_colors - table) != r.incoldelta) {
        go = false;
    }

    // A dyntex vertex first seen to move since the run was looked at
    if (go && r.animgen != rd->animgen) {
        r.animgen = rd->animgen;

        for (uint32_t k : r.all) {
            if (rd->animated[k]) {
                r.ok = go = false;
                break;
            }
        }
        for (int i = 0; go && i < 16; i++) {
            if ((r.loaded & (1u << i)) && rd->animated[r.slotout[i]]) {
                r.ok = go = false;
            }
        }
    }

    int32_t sig = -1;

    if (go && (r.loaded || r.incoming)) {
        GfxRoomVState cur;
        gfx_room_vstate(&cur);

        if (!room_sigs.empty() && memcmp(&room_sigs.back(), &cur, sizeof(cur)) == 0) {
            sig = (int32_t)room_sigs.size() - 1;
        } else {
            room_sigs.push_back(cur);
            sig = (int32_t)room_sigs.size() - 1;
        }

        // Slots an earlier run loaded: the same vertices, loaded under this
        // state; a lit one's lights are not part of it
        if (r.incoming && (rsp.geometry_mode & G_LIGHTING)) {
            go = false;
        }

        for (int i = 0; go && i < 16; i++) {
            if (r.incoming & (1u << i)) {
                const int32_t s = mesh_slot_sig[i];

                if (mesh_slot[i] != r.slotin[i] || s < 0 ||
                    (s != sig && memcmp(&room_sigs[s], &cur, sizeof(cur)) != 0)) {
                    go = false;
                }
            }
        }
    }

    if (!go) {
        mesh_skip_until = r.end;
        return NULL;
    }

    // What the run leaves behind it, as reading it would have
    if (r.colseg) {
        rsp.vertex_colors = (const struct NormalColor*)(segmentPointers[r.colseg] + r.outcolofs);
    }

    for (int i = 0; i < 16; i++) {
        if (r.loaded & (1u << i)) {
            mesh_slot[i] = r.slotout[i];
            mesh_slot_sig[i] = sig;
            mesh_slot_run[i] = &r;
        }
    }

    g_GfxNumVerts += r.numverts;
    gfx_vertices_lost = false;

    if (r.numsealed && gfx_room_sealing()) {
        gfx_room_draw(r.plain, r.first_plain);
        g_GfxRoomSealedTris += r.numsealed;
        gfx_room_draw_cpu(r.sealed, true);
    } else {
        gfx_room_draw(r.all, r.first_all);
    }

    return r.end - 1;
}

/*
 * The mesh vertex shader's functions and main() (G_MESH_EXT), shared by both
 * backends, which put their own declarations in front: the inputs aPos, aST,
 * aBones, aWeights, aCol and aNormal (GfxMeshVertex and the colour stream),
 * the parameters uP[GFX_MESH_PARAMS] and the palette uPal[3 *
 * GFX_MESH_PALETTE_MAX], and the outputs the program's fragment shader
 * reads. The vertex is posed by the palette, put through the RSP's transform,
 * and every output is worked out from the draw's parameters in the order
 * gfx_sp_load_vertex(), gfx_light_vertex() and gfx_emit_vertex() work them out
 * on the CPU. depth_clamp_hack squeezes z as a renderer without depth clamping
 * does; vulkan_depth takes OpenGL's -1..1 depth into Vulkan's 0..1 last.
 */
static std::string gfx_mesh_strf(const char* fmt, ...) {
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    return buf;
}

std::string gfx_mesh_vs_main(const struct CCFeatures& cc, bool depth_clamp_hack, bool vulkan_depth) {
    std::string s;

    s += "vec3 skinBy(uint b, vec4 q) {\n";
    s += "    int i = int(b) * 3;\n";
    s += "    return vec3(dot(uPal[i], q), dot(uPal[i + 1], q), dot(uPal[i + 2], q));\n";
    s += "}\n";
    // a value put into a short, as gfx_sp_vertex()'s U and V are
    s += "float toShort(int x) { return float(((x + 32768) & 65535) - 32768); }\n";
    // gfx_texgen_eye_normal(): the half-way vector between the straight-on
    // ray and the reflection of the one the vertex is seen along
    s += "vec3 eyeNormal(vec3 p, vec3 n) {\n";
    s += "    vec3 ne = n.x * uP[41].xyz + n.y * uP[42].xyz + n.z * uP[43].xyz;\n";
    s += "    vec3 pe = p.x * uP[41].xyz + p.y * uP[42].xyz + p.z * uP[43].xyz + uP[44].xyz;\n";
    s += "    float nl = length(ne);\n";
    s += "    float pl = length(pe);\n";
    s += "    if (nl < 1e-6 || pl < 1e-6) return n;\n";
    s += "    float d = dot(ne, pe) / (nl * pl);\n";
    s += "    vec3 h = pe / pl - 2.0 * d * ne / nl;\n";
    s += "    h.z += 1.0;\n";
    s += "    vec3 m = vec3(dot(h, uP[41].xyz), dot(h, uP[42].xyz), dot(h, uP[43].xyz));\n";
    s += "    float ml = length(m);\n";
    s += "    if (ml < 1e-6) return n;\n";
    s += "    return m * 127.0 / ml;\n";
    s += "}\n";
    // gfx_light_vertex()'s texture coordinates from the normal
    s += "vec2 texgen(vec3 p, vec3 n) {\n";
    s += "    bool eye = uP[26].x != 0.0;\n";
    s += "    bool turn = uP[26].y != 0.0;\n";
    s += "    float dx = 0.0;\n";
    s += "    float dy = 0.0;\n";
    s += "    if (eye) n = eyeNormal(p, n);\n";
    s += "    if (uP[26].z != 0.0 && eye && turn) {\n";
    s += "        vec3 lx = uP[37].xyz;\n";
    s += "        vec3 ly = uP[38].xyz;\n";
    s += "        vec3 lz = vec3(lx.y * ly.z - lx.z * ly.y, lx.z * ly.x - lx.x * ly.z, lx.x * ly.y - lx.y * ly.x);\n";
    s += "        vec3 rx = lx * uP[40].x + lz * uP[40].y;\n";
    s += "        vec3 rz = lz * uP[40].x - lx * uP[40].y;\n";
    s += "        vec3 ry = ly * uP[40].z + rz * uP[40].w;\n";
    s += "        dx = (n.x * rx.x + n.y * rx.y + n.z * rx.z) / 127.0;\n";
    s += "        dy = (n.x * ry.x + n.y * ry.y + n.z * ry.z) / 127.0;\n";
    s += "    } else if (uP[26].z != 0.0) {\n";
    s += "        dx = (n.x * uP[37].x + n.y * uP[37].y + n.z * uP[37].z) / 127.0;\n";
    s += "        dy = (n.x * uP[38].x + n.y * uP[38].y + n.z * uP[38].z) / 127.0;\n";
    s += "    } else {\n";
    s += "        vec3 dir = n / 127.0;\n";
    s += "        vec3 t = vec3(dot(dir, uP[41].xyz), dot(dir, uP[42].xyz), dot(dir, uP[43].xyz));\n";
    s += "        t /= length(t);\n";
    s += "        dx = t.x;\n";
    s += "        dy = t.y;\n";
    s += "    }\n";
    s += "    dx = clamp(dx, -1.0, 1.0);\n";
    s += "    dy = clamp(dy, -1.0, 1.0);\n";
    s += "    if (uP[25].w != 0.0) {\n";
    s += "        dx = acos(-dx) / 4.0;\n";
    s += "        dy = acos(-dy) / 4.0;\n";
    s += "    } else {\n";
    s += "        dx = (dx + 1.0) / 4.0;\n";
    s += "        dy = (dy + 1.0) / 4.0;\n";
    s += "    }\n";
    s += "    if (eye && !turn) {\n";
    s += "        dx += uP[39].x / 2.0;\n";
    s += "        dy += uP[39].y / 2.0;\n";
    s += "    }\n";
    s += "    return vec2(float(int(dx * uP[12].x)), float(int(dy * uP[12].y)));\n";
    s += "}\n";

    s += "void main() {\n";
    s += "    vec3 p = aPos;\n";
    s += "    if (uP[12].z != 0.0) {\n";
    s += "        vec4 q = vec4(aPos, 1.0);\n";
    s += "        p = skinBy(aBones.x, q) * aWeights.x;\n";
    s += "        if (aBones.w > 1u) p += skinBy(aBones.y, q) * aWeights.y;\n";
    s += "        if (aBones.w > 2u) p += skinBy(aBones.z, q) * aWeights.z;\n";
    s += "    }\n";
    s += "    vec4 pos = p.x * uP[0] + p.y * uP[1] + p.z * uP[2] + uP[3];\n";
    s += "    if (uP[4].w != 0.0) pos.x = (uP[4].x * pos.w + pos.x) * uP[4].y / uP[4].z;\n";
    s += "    pos.xy += uP[5].xy * pos.w;\n";
    // Past the far plane at all three corners is thrown out on the CPU; with
    // depth clamping on, the GPU would draw it at the far plane instead. A
    // corner past it is far outside the clip distance's half-space and one
    // before it far inside, so only a triangle with all three past it loses
    // more than a sliver at the corner.
    s += "    gl_ClipDistance[0] = pos.z > pos.w ? -1.0 : 1.0e6;\n";
    // A room's colour is its entry in the table the draw hands over whole,
    // where it fits (GFXMESH_ROOM_PALETTE), and the vertex says which
    s += "    uvec4 col = aCol;\n";
    s += "    if (uP[12].w != 0.0) col = uvec4(uPal[int(aBones.x) | (int(aBones.y) << 8) | (int(aBones.z) << 16)]);\n";
    s += "    vec4 shade = vec4(col) / 255.0;\n";
    s += "    float lodf = floor((0.7 + clamp(pos.w / 1024.0, 0.0, 1.0) * 0.3) * 255.0) / 255.0;\n";
    s += "    vec4 outPos = vec4(pos.x, pos.y * uP[5].z, uP[5].w != 0.0 ? (pos.z + pos.w) / 2.0 : pos.z, pos.w);\n";
    s += "    vec2 uv = vec2(toShort((aST.x * int(uP[12].x)) >> 16), toShort((aST.y * int(uP[12].y)) >> 16));\n";
    // The normal an RSP light reads: the colour's bytes as signed, or a
    // skinned mesh's own normal posed, 127 long and rounded as a byte is
    s += "    vec3 nrm = vec3(col.rgb);\n";
    s += "    nrm = mix(nrm, nrm - 256.0, step(128.0, nrm));\n";
    s += "    if (uP[27].y != 0.0) {\n";
    s += "        vec3 n = vec3(dot(uPal[int(aBones.x) * 3].xyz, aNormal), dot(uPal[int(aBones.x) * 3 + 1].xyz, aNormal), dot(uPal[int(aBones.x) * 3 + 2].xyz, aNormal)) * aWeights.x;\n";
    s += "        if (aBones.w > 1u) n += vec3(dot(uPal[int(aBones.y) * 3].xyz, aNormal), dot(uPal[int(aBones.y) * 3 + 1].xyz, aNormal), dot(uPal[int(aBones.y) * 3 + 2].xyz, aNormal)) * aWeights.y;\n";
    s += "        if (aBones.w > 2u) n += vec3(dot(uPal[int(aBones.z) * 3].xyz, aNormal), dot(uPal[int(aBones.z) * 3 + 1].xyz, aNormal), dot(uPal[int(aBones.z) * 3 + 2].xyz, aNormal)) * aWeights.z;\n";
    s += "        float nl = length(n);\n";
    s += "        if (nl > 1e-6) n /= nl;\n";
    s += "        n *= 127.0;\n";
    s += "        nrm = trunc(n + sign(n) * 0.5);\n";
    s += "    }\n";
    // G_LIGHTING: gfx_light_vertex()
    s += "    if (uP[25].y != 0.0) {\n";
    s += "        vec3 c = uP[28].rgb;\n";
    s += "        for (int i = 0; i < 4; i++) {\n";
    s += "            if (float(i) >= uP[25].x) break;\n";
    s += "            float k = 0.0;\n";
    s += "            k += nrm.x * uP[29 + i].x;\n";
    s += "            k += nrm.y * uP[29 + i].y;\n";
    s += "            k += nrm.z * uP[29 + i].z;\n";
    s += "            k /= 127.0;\n";
    s += "            if (k > 0.0) c = floor(c + k * uP[33 + i].rgb);\n";
    s += "        }\n";
    s += "        shade.rgb = min(c, 255.0) / 255.0;\n";
    s += "        if (uP[27].x != 0.0) shade.a = 1.0;\n";
    s += "        if (uP[25].z != 0.0) uv = texgen(p, nrm);\n";
    s += "    }\n";

    for (int i = 0; i < 2; i++) {
        if (cc.used_textures[i]) {
            s += gfx_mesh_strf("    vTexCoord%d = uv * uP[%d].xy + uP[%d].zw;\n", i, 8 + i, 8 + i);
            for (int j = 0; j < 2; j++) {
                if (cc.clamp[i][j]) {
                    s += gfx_mesh_strf("    vTexClamp%s%d = uP[10].%c;\n", j == 0 ? "S" : "T", i, "xyzw"[i * 2 + j]);
                }
            }
        }
    }
    if (cc.opt_fog) {
        s += "    vFog = vec4(uP[7].rgb, uP[6].x);\n";
        s += "    vFogOffset = uP[6].y;\n";
        s += "    if (uP[6].z != 0.0) {\n";
        s += "        vFog.a = 0.0;\n";
        s += "        vFogOffset = clamp(pos.z / pos.w * uP[6].x + uP[6].y, 0.0, 255.0) * pos.w;\n";
        s += "    }\n";
        s += "    vFogZW = outPos.zw;\n";
    }
    if (cc.opt_grayscale) {
        s += "    vGrayscaleColor = uP[11];\n";
    }
    if (cc.opt_envmap) {
        // G_ENVMAP_EXT: the normal and the position through the modelview
        s += "    vEnvNormal = nrm.x * uP[41].xyz + nrm.y * uP[42].xyz + nrm.z * uP[43].xyz;\n";
        s += "    vEnvPos = p.x * uP[41].xyz + p.y * uP[42].xyz + p.z * uP[43].xyz + uP[44].xyz;\n";
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        const char comp = "xyzw"[i & 3];

        s += gfx_mesh_strf("    { float k = uP[%d].%c;\n", 21 + i / 4, comp);
        s += gfx_mesh_strf("      vec3 c = k == 1.0 ? shade.rgb : k == 2.0 ? vec3(shade.a) : k == 3.0 ? vec3(lodf) : uP[%d].rgb;\n", 13 + i);
        if (cc.opt_alpha) {
            s += gfx_mesh_strf("      float ka = uP[%d].%c;\n", 23 + i / 4, comp);
            s += gfx_mesh_strf("      float a = ka == 1.0 ? shade.a : ka == 3.0 ? lodf : uP[%d].a;\n", 13 + i);
            s += gfx_mesh_strf("      vInput%d = vec4(c, a); }\n", i + 1);
        } else {
            s += gfx_mesh_strf("      vInput%d = c; }\n", i + 1);
        }
    }

    s += "    gl_Position = outPos;\n";
    if (depth_clamp_hack) {
        s += "    gl_Position.z *= 0.3f;\n";
    }
    s += "}\n";
    if (vulkan_depth) {
        s.insert(s.rfind("}"), "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n");
    }

    return s;
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    gfx_mark_state_dirty();
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static inline void gfx_update_aspect_mode(void) {
    const uint32_t side = rsp.aspect_mode & G_ASPECT_CENTER_EXT;

    rsp.aspect_scale = rsp.aspect_mode ? gfx_current_native_aspect : gfx_current_window_dimensions.aspect_ratio;

    if (side == G_ASPECT_LEFT_EXT) {
        rsp.aspect_ofs = 1.f - gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect;
    } else if (side == G_ASPECT_RIGHT_EXT) {
        rsp.aspect_ofs = gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect - 1.f;
    } else {
        rsp.aspect_ofs = 0.f;
    }

    if (side && (rsp.aspect_mode & G_ASPECT_WIDE_EXT)) {
        constexpr float c = 16.f / 9.f;
        if (gfx_current_dimensions.aspect_ratio > c) {
            rsp.aspect_ofs *= c / gfx_current_dimensions.aspect_ratio;
        }
    }
}

static void gfx_sp_extra_geometry_mode(uint32_t clear, uint32_t set) {
    gfx_mark_state_dirty();
    rsp.extra_geometry_mode &= ~clear;
    rsp.extra_geometry_mode |= set;
    rsp.aspect_mode = (rsp.extra_geometry_mode & G_ASPECT_MODE_EXT);
    gfx_update_aspect_mode();
}

static void gfx_adjust_viewport_or_scissor(XYWidthHeight* area, bool preserve_aspect = false) {
    // HACK: assume all target framebuffers have the same aspect
    // Use floor/ceil to ensure scissor fully contains the logical region
    // and prevents sub-pixel gaps at viewport edges
    float x1 = area->x * RATIO_X;
    float y1 = (SCREEN_HEIGHT - area->y) * RATIO_Y;
    float x2 = (area->x + area->width) * RATIO_X;
    float y2 = (SCREEN_HEIGHT - area->y + area->height) * RATIO_Y;
    
    area->x = std::floor(x1);
    area->y = std::floor(y1);
    area->width = std::ceil(x2) - area->x;
    area->height = std::ceil(y2) - area->y;
    
    if (preserve_aspect) {
        // preserve native aspect ratio
        const float ratio = gfx_current_native_aspect / gfx_current_dimensions.aspect_ratio;
        const float midx = gfx_current_dimensions.width * 0.5f;
        area->x = midx + (area->x - midx) * ratio;
        area->x += rsp.aspect_ofs * gfx_current_dimensions.width * 0.5f;
        area->width *= ratio;
    }

    // The screen shake's offset (vi.c), in window pixels, and wherever the
    // game draws: straight to the window or into a framebuffer on its way
    const float to_draw = (float)gfx_current_dimensions.height / gfx_current_window_dimensions.height;
    area->x += gfx_current_game_window_viewport.x * to_draw;
    area->y += (gfx_current_window_dimensions.height -
                (gfx_current_game_window_viewport.y + gfx_current_game_window_viewport.height)) * to_draw;
}

static void gfx_calc_and_set_viewport(const Vp_t* viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.viewport);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t*)data);
            break;
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            // I think this is only really used for guLookAtReflect
            index = !((index - G_MV_LOOKATY) / 2);
            rsp.lookat[index] = ((const Light *)data)->l;
            rsp.lookat_enabled = (index == 0) || (rsp.lookat[1].dir[0] || rsp.lookat[1].dir[1]);
            rsp.lights_changed = true;
            break;
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uintptr_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            if (rsp.fog_linear) {
                rsp.fog_linear = false;
                gfx_mark_state_dirty();
            }
            break;
        case G_MW_SEGMENT:
            segmentPointers[(offset >> 2) & 0xff] = data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    gfx_mark_state_dirty();
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    rdp.tex_max_lod = level;
    if (rdp.first_tile_index != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rdp.first_tile_index = tile;
    }
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f;
    float y = lry / 4.0f;
    float width = (lrx - ulx) / 4.0f;
    float height = (lry - uly) / 4.0f;

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.scissor, rsp.aspect_mode != 0);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, uint32_t tex_flags, const void* addr) {
    gfx_mark_state_dirty();
    if ((uintptr_t)addr < 0x10000000u) {
        // The game replaces a display list's texture-number marker (0xabcdNNNN)
        // with a pointer to the loaded texture; one still here at draw time
        // is a texture that was never loaded, and it draws white. Say so,
        // once per texture.
        static uint32_t warned[64];
        static int nwarned;
        uint32_t w = (uint32_t)(uintptr_t)addr;
        int seen = 0;
        for (int k = 0; k < nwarned; k++) if (warned[k] == w) { seen = 1; break; }
        if (!seen && nwarned < 64) {
            warned[nwarned++] = w;
            sysLogPrintf(LOG_WARNING, "F3D: texture image address %08x was never loaded (texture %u?); it draws white", w, w & 0xffff);
        }
    }
    rdp.texture_to_load.addr = (const uint8_t*)addr;
    rdp.texture_to_load.glyph = rdp.pending_glyph;
    rdp.pending_glyph = 0;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width;
    rdp.texture_to_load.tex_flags = tex_flags;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                            uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                            uint32_t shifts) {
    gfx_mark_state_dirty();
    // OTRTODO:
    // SUPPORT_CHECK(tmem == 0 || tmem == 256);
    static uint32_t max_tmem = 0;
    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }

    if (fmt == G_IM_FMT_RGBA && siz < G_IM_SIZ_16b) {
        // HACK: sometimes the game will submit G_IM_FMT_RGBA, G_IM_SIZ_8b/4b, intending it to read as CI8/CI4 with RGBA16 palette
        fmt = G_IM_FMT_CI;
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_32b) {
        // HACK: ... and sometimes it submits this, apparently intending it to be I8
        fmt = G_IM_FMT_I;
        siz = G_IM_SIZ_8b;
    }

    rdp.texture_tile[tile].palette = palette; // palette should set upper 4 bits of color index in 4b mode
    rdp.texture_tile[tile].fmt = fmt;
    rdp.texture_tile[tile].siz = siz;
    rdp.texture_tile[tile].cms = cms;
    rdp.texture_tile[tile].cmt = cmt;
    rdp.texture_tile[tile].shifts = shifts;
    rdp.texture_tile[tile].shiftt = shiftt;
    rdp.texture_tile[tile].line_size_bytes = line * 8;
    rdp.texture_tile[tile].tmem = tmem;

    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    gfx_mark_state_dirty();
    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].ofs_s = 0.0f;
    rdp.texture_tile[tile].ofs_t = 0.0f;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = (lrs - uls + 4) / 4;
    rdp.texture_tile[tile].height = (lrt - ult + 4) / 4;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    gfx_mark_state_dirty();
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    SUPPORT_CHECK(rdp.texture_tile[tile].tmem >= 256);

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;

    const uint32_t width = (lrs - uls + 1);
    const uint32_t height = (lrt - ult + 1);
    const uint32_t pitch = rdp.texture_to_load.width + 1;
    const uint32_t count =  width * height;
    const uint16_t *base = (const uint16_t *)rdp.texture_to_load.addr + pitch * ult + uls;

    if (rdp.texture_tile[tile].tmem == 256) {
        rdp.palette_addrs[0] = (const uint8_t *)base;
        if (count >= 256) {
            rdp.palette_addrs[1] = (const uint8_t *)(base + 128);
        }
    } else {
        rdp.palette_addrs[1] = (const uint8_t *)base;
    }

    const uint32_t palofs = rdp.texture_tile[tile].tmem - 256;
    SUPPORT_CHECK(palofs + count <= 256);

    const uint16_t *src = base;
    uint16_t *dst = rdp.palette + palofs;
    for (uint32_t i = 0; i < count; ++i) {
        *dst++ = PD_BE16(*src++);
    }

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    gfx_mark_state_dirty();
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t orig_size_bytes = (lrs + 1) << rdp.texture_to_load.siz >> 1;
    uint32_t size_bytes = orig_size_bytes;
    if (rdp.texture_to_load.raw_tex_metadata.h_byte_scale != 1 ||
        rdp.texture_to_load.raw_tex_metadata.v_pixel_scale != 1) {
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = size_bytes;
    loaded_texture.line_size_bytes = size_bytes;
    loaded_texture.full_image_line_size_bytes = size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr;
    loaded_texture.glyph = rdp.texture_to_load.glyph;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    gfx_mark_state_dirty();
    SUPPORT_CHECK(tile == G_TX_LOADTILE);

    uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t full_image_width = rdp.texture_to_load.width + 1;

    uint32_t offset_x_in_bytes = offset_x << rdp.texture_to_load.siz >> 1;
    uint32_t tile_line_size_bytes = tile_width << rdp.texture_to_load.siz >> 1;
    uint32_t full_image_line_size_bytes = full_image_width << rdp.texture_to_load.siz >> 1;

    uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    uint32_t size_bytes = orig_size_bytes;
    uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    float h_byte_scale = rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
    float v_pixel_scale = rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;

    if (h_byte_scale != 1 || v_pixel_scale != 1) {
        start_offset_bytes = h_byte_scale * (v_pixel_scale * offset_y * full_image_line_size_bytes + offset_x_in_bytes);
        size_bytes *= h_byte_scale * v_pixel_scale;
        full_image_line_size_bytes *= h_byte_scale;
        tile_line_size_bytes *= h_byte_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = full_image_line_size_bytes * tile_height;
    loaded_texture.full_image_line_size_bytes = full_image_line_size_bytes;
    loaded_texture.line_size_bytes = tile_line_size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr + start_offset_bytes;
    loaded_texture.glyph = rdp.texture_to_load.glyph;

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    rdp.texture_tile[tile].height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2) {
    gfx_mark_state_dirty();
    rdp.combine_mode = rgb | (alpha << 16) | ((uint64_t)rgb_cyc2 << 28) | ((uint64_t)alpha_cyc2 << 44);
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}

static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

static void gfx_dp_set_grayscale_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.grayscale_color.r = r;
    rdp.grayscale_color.g = g;
    rdp.grayscale_color.b = b;
    rdp.grayscale_color.a = a;
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t m, uint8_t l, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_lod_fraction = l;
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
    rdp.fill_color.r = r;
    rdp.fill_color.g = g;
    rdp.fill_color.b = b;
    rdp.fill_color.a = a;
    rdp.tex_min_lod = m;

}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_dp_set_subpixel_offset(int16_t x, int16_t y) {
    rdp.subpixel_ofs_x = x;
    rdp.subpixel_ofs_y = y;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    ulx += rdp.subpixel_ofs_x;
    lrx += rdp.subpixel_ofs_x;
    uly += rdp.subpixel_ofs_y;
    lry += rdp.subpixel_ofs_y;

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;

    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];

    // In front of everything, unless G_SETRECTDEPTH_EXT gave the rectangle a
    // depth of its own to be tested at (a light's glare under Glare Clipping)
    const float rect_z = rdp.rect_depth_on ? rdp.rect_depth : -1.0f;

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = rect_z;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = rect_z;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = rect_z;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = rect_z;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = { 0, (int16_t)SCREEN_HEIGHT, (uint32_t)SCREEN_WIDTH, (uint32_t)SCREEN_HEIGHT };
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;

    gfx_adjust_viewport_or_scissor(&default_viewport);

    const uint32_t other_mode_l_saved = rdp.other_mode_l;

    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    if (rdp.rect_depth_on) {
        // Compared against what the scene wrote, never written itself, so a
        // nearer wall hides the part of the rectangle behind it
        rsp.geometry_mode = G_ZBUFFER;
        rdp.other_mode_l = (rdp.other_mode_l & ~(Z_UPD | ZMODE_DEC)) | Z_CMP | ZMODE_OPA;
    }

    gfx_mark_state_dirty();

    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3, true);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3, true);

    rdp.other_mode_l = other_mode_l_saved;
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
    gfx_mark_state_dirty();
}

/*
 * G_OCCLUSIONTEST_EXT: whether anything the frame has drawn so far is nearer
 * than z at pixel (x, y). This is the N64's own test for the light glares and
 * the sun, which read the z-buffer at a handful of pixels once the scene was
 * drawn (zbuf.c), done by the GPU instead: a rectangle one pixel of the
 * game's screen across goes out as a draw of its own inside an occlusion
 * query, depth tested but never written and blended away to nothing, so the
 * count that comes back is its samples that nothing in the depth buffer is in
 * front of - at whatever size, sample count and render scale the frame is
 * drawn at.
 */
static bool gfx_occlusion_issued[GFX_OCCLUSION_SLOTS];
static bool gfx_occlusion_failed;

static void gfx_occlusion_test(int slot, int32_t x, int32_t y, float z) {
    if (slot < 0 || slot >= GFX_OCCLUSION_SLOTS || !gfx_rapi->occlusion_begin || gfx_occlusion_failed) {
        return;
    }

    const uint32_t other_mode_l = rdp.other_mode_l;
    const uint32_t other_mode_h = rdp.other_mode_h;
    const uint64_t combine_mode = rdp.combine_mode;
    const uint32_t extra_geometry_mode = rsp.extra_geometry_mode;
    const bool rect_depth_on = rdp.rect_depth_on;
    const float rect_depth = rdp.rect_depth;
    const int16_t depth_bias = rdp.depth_bias;
    struct RGBA colors[4];

    for (int i = 0; i < 4; i++) {
        colors[i] = rsp.loaded_vertices[MAX_VERTICES + i].color;
    }

    gfx_flush();

    // The blender's "invisible" mode (0 x in + 1 x memory), which leaves
    // nothing that could discard a fragment before the depth test counts it
    rdp.other_mode_l = (rdp.other_mode_l & ~(0xffff0000U | CVG_X_ALPHA | G_ZS_PRIM | (3U << G_MDSFT_ALPHACOMPARE))) |
                       GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_MEM, G_BL_1MA) | GBL_c2(G_BL_CLR_IN, G_BL_0, G_BL_CLR_MEM, G_BL_1MA);
    rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_CYCLETYPE)) | G_CYC_1CYCLE;
    rsp.extra_geometry_mode &= ~(G_MODULATE_EXT | G_ADDITIVE_EXT | G_ENVMAP_EXT | G_DECAL_EXT);
    rdp.rect_depth_on = true;
    rdp.rect_depth = z;
    rdp.depth_bias = 0;
    gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        rsp.loaded_vertices[i].color = { 0xff, 0xff, 0xff, 0xff };
    }

    gfx_mark_state_dirty();

    if (gfx_rapi->occlusion_begin(slot)) {
        gfx_draw_rectangle(x << 2, y << 2, (x + 1) << 2, (y + 1) << 2);
        gfx_flush();
        gfx_rapi->occlusion_end(slot);
        gfx_occlusion_issued[slot] = true;
    } else {
        // Glares go back to the line of sight test (videoHasOcclusionQueries())
        gfx_occlusion_failed = true;
    }

    rdp.other_mode_l = other_mode_l;
    rdp.other_mode_h = other_mode_h;
    rdp.combine_mode = combine_mode;
    rsp.extra_geometry_mode = extra_geometry_mode;
    rdp.rect_depth_on = rect_depth_on;
    rdp.rect_depth = rect_depth;
    rdp.depth_bias = depth_bias;

    for (int i = 0; i < 4; i++) {
        rsp.loaded_vertices[MAX_VERTICES + i].color = colors[i];
    }

    gfx_mark_state_dirty();
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                     int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint64_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(0, 0, 0, G_ACMUX_TEXEL0), 0, 0);

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5

    const int16_t width = flip ? lry - uly : lrx - ulx;
    const int16_t height = flip ? lrx - ulx : lry - uly;
    const float lrs = ((uls << 7) + dsdx * width) >> 7;
    const float lrt = ((ult << 7) + dtdy * height) >> 7;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;
    rdp.combine_mode = saved_combine_mode;
    gfx_mark_state_dirty();
}

static void gfx_dp_image_rectangle(int32_t tile, int32_t w, int32_t h,
                                   int32_t ulx, int32_t uly, int16_t uls, int16_t ult,
                                   int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt) {
    uint64_t saved_combine_mode = rdp.combine_mode;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls * 32;
    ul->v = ult * 32;
    lr->u = lrs * 32;
    lr->v = lrt * 32;
    ll->u = uls * 32;
    ll->v = lrt * 32;
    ur->u = lrs * 32;
    ur->v = ult * 32;

    // ensure we have the correct texture size
    rdp.texture_tile[tile].line_size_bytes = w << rdp.texture_tile[tile].siz >> 1;
    rdp.texture_tile[tile].width = w;
    rdp.texture_tile[tile].height = h;
    rdp.texture_tile[tile].cms = 0;
    rdp.texture_tile[tile].cmt = 0;
    rdp.texture_tile[tile].shifts = 0;
    rdp.texture_tile[tile].shiftt = 0;
    auto& loadtex = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loadtex.full_image_line_size_bytes = loadtex.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
    loadtex.size_bytes = loadtex.orig_size_bytes = loadtex.full_size_bytes = loadtex.line_size_bytes * h;

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;

    rdp.combine_mode = saved_combine_mode;
    gfx_mark_state_dirty();
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    // OTRTODO: This is a bit of a hack for widescreen screen fades, but it'll work for now...
    if (ulx == 0 && uly == 0 && lrx == 319 * 4 && lry == 239 * 4) {
        ulx = -1024;
        uly = -1024;
        lrx = 2048;
        lry = 2048;
    }

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }

    uint64_t saved_combine_mode = rdp.combine_mode;

    if (mode == G_CYC_FILL) {
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);
    }

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
    gfx_mark_state_dirty();
}

static void gfx_dp_set_z_image(void* z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    gfx_mark_state_dirty();
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    rdp.palette_fmt = rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT);
    rdp.tex_lod = (rdp.other_mode_h & G_TL_LOD) != 0;
    rdp.tex_detail = (rdp.other_mode_h & (2U << G_MDSFT_TEXTDETAIL)) == G_TD_DETAIL;
}

static void gfx_sp_set_vertex_colors(uint32_t count, const struct NormalColor *vcn) {
    // common sense dictates that we should copy the colors as the command is supposed to do,
    // but it actually doesn't seem to matter
    // SUPPORT_CHECK(count <= sizeof(rsp.vertex_colors) / sizeof(rsp.vertex_colors[0]));
    // for (uint32_t i = 0; i < count; ++i) {
    //     rsp.vertex_colors[i] = vcn[i];
    // }
    rsp.vertex_colors = vcn;
}

static void gfx_dp_set_other_mode(uint32_t h, uint32_t l) {
    gfx_mark_state_dirty();
    rdp.other_mode_h = h;
    rdp.other_mode_l = l;
}

extern uint32_t num_dls;

static float gfx_taa_halton(int i, int base) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

// 4x4 row-vector products and inverse, in doubles: the world's coordinates
// run to tens of thousands and the two frames' matrices nearly cancel
static void gfx_taa_mul(const double *a, const double *b, double *out) {
    double r[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        }
    }
    memcpy(out, r, sizeof(r));
}

static bool gfx_taa_invert(const double *m, double *out) {
    double a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][4 + j] = i == j ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int best = c;
        for (int r = c + 1; r < 4; r++) {
            if (fabs(a[r][c]) > fabs(a[best][c])) {
                best = r;
            }
        }
        if (fabs(a[best][c]) < 1e-12) {
            return false;
        }
        for (int j = 0; j < 8; j++) {
            std::swap(a[c][j], a[best][j]);
        }
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) {
            a[c][j] *= inv;
        }
        for (int r = 0; r < 4; r++) {
            if (r != c && a[r][c] != 0.0) {
                const double k = a[r][c];
                for (int j = 0; j < 8; j++) {
                    a[r][j] -= k * a[c][j];
                }
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = a[i][4 + j];
        }
    }
    return true;
}

static void gfx_taa_resolve(void) {
    const uint32_t fw = gfx_current_dimensions.width, fh = gfx_current_dimensions.height;
    GfxTaaHistory &h = taa_history[taa_slot];

    int vx = std::max(0, (int)taa_viewport.x);
    int vy = std::max(0, (int)taa_viewport.y);
    int vw = std::min((int)fw - vx, (int)taa_viewport.width);
    int vh = std::min((int)fh - vy, (int)taa_viewport.height);
    if (vw <= 0 || vh <= 0) {
        return;
    }

    // What the vertices were put through after the game's matrix: the
    // aspect adjustment, x' = k * (x + o * w)
    const double k = taa_aspect_k, o = taa_aspect_o;
    const double aspect[16] = { k, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, k * o, 0, 0, 1 };

    const bool valid = h.valid && h.frame + 1 == num_dls && h.width == fw && h.height == fh;
    float params[GFX_POST_TAA_PARAMS] = {};

    double cur[16], prev[16], inv[16], r[16];
    gfx_taa_mul(taa_mtx, aspect, cur);
    gfx_taa_mul(h.mtx, aspect, prev);

    if (valid && gfx_taa_invert(cur, inv)) {
        gfx_taa_mul(inv, prev, r);

        // (u, v, depth, 1) -> this frame's NDC, in the viewport's pixels of
        // the texture (bottom row first, as every framebuffer here is)
        const double tvx = taa_viewport.x, tvy = taa_viewport.y;
        const double tvw = taa_viewport.width, tvh = taa_viewport.height;
        const double in[16] = {
            2.0 * fw / tvw, 0, 0, 0,
            0, 2.0 * fh / tvh, 0, 0,
            0, 0, 2, 0,
            -2.0 * tvx / tvw - 1.0, -2.0 * tvy / tvh - 1.0, -1, 1,
        };
        double t[16];
        gfx_taa_mul(in, r, t);

        // last frame's clip -> (u * w, v * w, w)
        for (int i = 0; i < 4; i++) {
            const double X = t[i * 4], Y = t[i * 4 + 1], W = t[i * 4 + 3];
            params[0 + i] = (float)((X * tvw * 0.5 + W * (tvx + tvw * 0.5)) / fw);
            params[4 + i] = (float)((Y * tvh * 0.5 + W * (tvy + tvh * 0.5)) / fh);
            params[8 + i] = (float)W;
        }
    }

    params[12] = (float)vx / fw;
    params[13] = (float)vy / fh;
    params[14] = (float)(vx + vw) / fw;
    params[15] = (float)(vy + vh) / fh;
    params[17] = 1.0f;
    params[18] = 0.1f;
    params[19] = valid ? 1.0f : 0.0f;

    if (gfx_rapi->taa_resolve(game_framebuffer, params, num_dls & 1, vx, vy, vw, vh)) {
        memcpy(h.mtx, taa_mtx, sizeof(h.mtx));
        h.frame = num_dls;
        h.width = fw;
        h.height = fh;
        h.valid = true;
    } else {
        h.valid = false;
        taa_failed = true;
        sysLogPrintf(LOG_WARNING, "F3D: TAA could not run on this renderer, off");
    }

    gfx_mark_state_dirty();
    rendering_state.viewport = {};
    rendering_state.scissor = {};
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_taa_marker(bool begin, int slot, const float *mtx) {
    const bool usable = gfx_taa && !taa_failed && game_renders_to_framebuffer && !fbActive && gfx_msaa_level <= 1 &&
                        gfx_rapi->taa_resolve;

    if (begin) {
        taa_active = false;
        if (!usable || !mtx) {
            return;
        }
        taa_slot = slot & 3;
        for (int i = 0; i < 16; i++) {
            taa_mtx[i] = mtx[i];
        }
        taa_viewport = rdp.viewport;
        if (taa_viewport.width <= 0 || taa_viewport.height <= 0) {
            taa_viewport = { 0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height };
        }
        taa_aspect_k = rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
        taa_aspect_o = rsp.aspect_ofs;

        // a pixel is 2 / size in NDC; the phases step through the pixel
        const int i = (int)(num_dls % 8) + 1;
        taa_jx = (gfx_taa_halton(i, 2) - 0.5f) * 2.0f / taa_viewport.width;
        taa_jy = (gfx_taa_halton(i, 3) - 0.5f) * 2.0f / taa_viewport.height;
        taa_active = true;
    } else if (taa_active) {
        gfx_flush_for(GFX_FLUSH_OTHER);
        taa_active = false;
        if (usable) {
            gfx_taa_resolve();
        }
    }
}

static inline void *seg_addr(uintptr_t w1) {
    // all segmented addresses have the least significant bit set
    if (w1 & 1) {
        // seg 0 is reserved and doesn't count here
        const uintptr_t seg = (w1 & 0x0f000000) >> 24;
        if (seg && segmentPointers[seg]) {
            const uintptr_t addr = (w1 & 0x00fffffe);
            return (void *)(segmentPointers[seg] + addr);
        }
    }
    return (void *)w1;
}

/**
 * A vertex load whose source is not there.
 *
 * Four Windows crash reports - 20260921-225327 (v3.8.0, a Randomizer hop on HD
 * Caverns), 20260923-111834 (GE Plus Facility after Dam), 20260923-192751 and
 * 20260924-202208 (GE Plus Runway, the watch) - died in gfx_sp_vertex()
 * reading an address that was not mapped, and in every one of them the call
 * stack is gfx_run() -> gfx_run_dl() with nothing between: the load was a
 * command of the frame's own list, not of a model's or a room's list that list
 * calls. Every vertex load the game writes into the frame's list itself names
 * the frame's vertex pool (gfxAllocateVertices()) or a static array, and the
 * addresses the reports read were neither - heap addresses, three of them not
 * even on a four byte boundary, which no Vtx array the game or the port makes
 * is. So the command was not one anything wrote there this frame. None of it
 * has reproduced here: not on Linux, not under ASan, not with every allocation
 * over 64KB given back to the system on free, not under wine with the same
 * Windows build, the same conversion and the same Runway, watch and folder.
 *
 * Until it does, a load from memory that cannot be read is refused rather than
 * followed: the load and the triangles after it are dropped until the next load
 * that can be read, the frame draws on, and the command is written down - what
 * it was, where in which list, what came before it and what the segments held -
 * in the log and, once a session, as a report the Crash Reports page offers to
 * send. That report is what will say which list this is.
 */
extern "C" const char *crashReportSave(const char *text);

#define GFX_READABLE_RUNS 16

static struct {
    uintptr_t lo, hi;
} gfx_readable_runs[GFX_READABLE_RUNS];
static int gfx_readable_count;
static int gfx_readable_next;
static int gfx_dl_depth;         // gfx_run_dl() nesting: 1 is the frame's own list
static uint32_t gfx_bad_vertex_loads;

// Memory is given back between frames, so what was readable last frame is asked again
static void gfx_readable_reset(void) {
    gfx_readable_count = 0;
    gfx_readable_next = 0;
    gfx_vertices_lost = false;
}

static bool gfx_readable(const void *ptr, size_t len) {
    uintptr_t at = (uintptr_t)ptr;
    const uintptr_t end = at + len;

    if (at < 0x10000 || end < at) {
        return false;
    }

    while (at < end) {
        uintptr_t lo;
        uintptr_t hi;
        int i;

        for (i = 0; i < gfx_readable_count; i++) {
            if (at >= gfx_readable_runs[i].lo && at < gfx_readable_runs[i].hi) {
                break;
            }
        }

        if (i < gfx_readable_count) {
            at = gfx_readable_runs[i].hi;
            continue;
        }

        if (!sysMemReadableRange((const void *)at, &lo, &hi)) {
            return false;
        }

        // Linux answers a page at a time: a run that ends where this one
        // starts takes it, so a mesh's vertices are one run and not fifty
        for (i = 0; i < gfx_readable_count; i++) {
            if (gfx_readable_runs[i].hi == lo) {
                gfx_readable_runs[i].hi = hi;
                break;
            }
        }

        if (i == gfx_readable_count) {
            const int slot = gfx_readable_count < GFX_READABLE_RUNS
                ? gfx_readable_count++
                : gfx_readable_next++ % GFX_READABLE_RUNS;

            gfx_readable_runs[slot].lo = lo;
            gfx_readable_runs[slot].hi = hi;
        }

        at = hi;
    }

    return true;
}

static void gfx_refuse_vertex_load(const Gfx *cmd, const Gfx *list, const void *src, size_t count) {
    char text[1536];
    int len;

    gfx_bad_vertex_loads++;

    // the first few of a session: after that it is the same list every frame
    if (gfx_bad_vertex_loads > 4) {
        return;
    }

    len = snprintf(text, sizeof(text),
            "F3D: a vertex load reads memory that is not there - %p, %u vertices - and was dropped "
            "(not a crash; the frame drew on). Command %p, %lld into list %p at depth %d%s; "
            "w0 %016llx w1 %016llx; segments 4 %p 5 %p 6 %p 14 %p 15 %p; before it:",
            src, (unsigned)count, (const void *)cmd, (long long)(cmd - list), (const void *)list, gfx_dl_depth,
            gfx_dl_depth == 1 ? " (the frame's own)" : "",
            (unsigned long long)cmd->words.w0, (unsigned long long)cmd->words.w1,
            (void *)segmentPointers[4], (void *)segmentPointers[5], (void *)segmentPointers[6],
            (void *)segmentPointers[14], (void *)segmentPointers[15]);

    for (int k = 6; k >= 1 && len > 0 && len < (int)sizeof(text); k--) {
        if (cmd - k >= list) {
            len += snprintf(text + len, sizeof(text) - len, " %016llx:%016llx",
                    (unsigned long long)cmd[-k].words.w0, (unsigned long long)cmd[-k].words.w1);
        }
    }

    sysLogPrintf(LOG_ERROR, "%s", text);

    if (gfx_bad_vertex_loads == 1) {
        crashReportSave(text);
    }
}

struct GfxDlDepth {
    GfxDlDepth() { gfx_dl_depth++; }
    ~GfxDlDepth() { gfx_dl_depth--; }
};

/**
 * The way down to the list being run: at each depth, the list (or the branch
 * target it went on in) and the G_DL that called the next one. Only for the
 * report below - "Unknown GBI opcode" used to name the command and nothing
 * else, and twice (20260929-170457, 20260930-124918: the same w0/w1, a float
 * 376 in w1, both on the Carrington Institute after a Community Packs
 * install) that was not enough to say whose list it was.
 */
#define GFX_DL_TRACK 8
static const Gfx *gfx_dl_lists[GFX_DL_TRACK];
static const Gfx *gfx_dl_calls[GFX_DL_TRACK];

static void gfx_unknown_opcode(const Gfx *cmd, const Gfx *list, uint32_t opcode) {
    char text[2048];
    int len;

    len = snprintf(text, sizeof(text),
            "Unknown GBI opcode 0x%02x at %p, %lld into list %p at depth %d%s.\n"
            "w0 %016llx w1 %016llx\nsegments 4 %p 5 %p 6 %p 14 %p 15 %p\nbefore it:",
            opcode, (const void *)cmd, (long long)(cmd - list), (const void *)list, gfx_dl_depth,
            gfx_dl_depth == 1 ? " (the frame's own)" : "",
            (unsigned long long)cmd->words.w0, (unsigned long long)cmd->words.w1,
            (void *)segmentPointers[4], (void *)segmentPointers[5], (void *)segmentPointers[6],
            (void *)segmentPointers[14], (void *)segmentPointers[15]);

    for (int k = 6; k >= 1 && len > 0 && len < (int)sizeof(text); k--) {
        if (cmd - k >= list) {
            len += snprintf(text + len, sizeof(text) - len, " %016llx:%016llx",
                    (unsigned long long)cmd[-k].words.w0, (unsigned long long)cmd[-k].words.w1);
        }
    }

    for (int d = 0; d < gfx_dl_depth - 1 && d < GFX_DL_TRACK && len > 0 && len < (int)sizeof(text); d++) {
        const Gfx *call = gfx_dl_calls[d];

        len += snprintf(text + len, sizeof(text) - len, "\ndepth %d: list %p called it at %p (+%lld)",
                d + 1, (const void *)gfx_dl_lists[d], (const void *)call,
                call && gfx_dl_lists[d] ? (long long)(call - gfx_dl_lists[d]) : -1LL);
    }

    sysFatalError("%s", text);
}

uintptr_t clearMtx;

static void gfx_run_dl(Gfx* cmd) {
    // puts("dl");
    GfxDlDepth depth;
    int dummy = 0;
    char dlName[128];
    const char* fileName;

    Gfx* dListStart = cmd;
    uint64_t ourHash = -1;

    if (gfx_dl_depth <= GFX_DL_TRACK) {
        gfx_dl_lists[gfx_dl_depth - 1] = cmd;
    }

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        // gfx_print_cmd(cmd);

        // A run of a mesh's triangles ends at the first command that is not
        // part of the geometry, which may change the state it is drawn under
        if (!mesh_run.empty() && opcode != G_VTX && opcode != G_COL && opcode != (uint8_t)G_TRI1 &&
            opcode != (uint8_t)G_TRI4 && opcode != G_NOOP && opcode != G_DL && opcode != (uint8_t)G_ENDDL) {
            gfx_mesh_flush();
        }

        // A mesh's run drawn whole from what was kept of it
        if (mesh_entry) {
            if (cmd == mesh_skip_until) {
                mesh_skip_until = NULL;
            }

            if (!mesh_skip_until && mesh_run.empty() &&
                (opcode == G_COL || opcode == G_VTX || opcode == (uint8_t)G_TRI1 || opcode == (uint8_t)G_TRI4)) {
                const Gfx* last = mesh_room ? gfx_room_kept_run(cmd) : gfx_mesh_kept_run(cmd);

                if (last) {
                    cmd = (Gfx*)last + 1;
                    continue;
                }
            }
        }

        switch (opcode) {
                // RSP commands:
            case G_NOOP:
                break;
            case G_MTX: {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
                break;
            }
            case (uint8_t)G_POPMTX:
                gfx_sp_pop_matrix(1);
                break;
            case G_MOVEMEM:
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
                break;
            case (uint8_t)G_MOVEWORD:
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
                break;
            case (uint8_t)G_TEXTURE:
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
                break;
            case G_VTX: {
                const Vtx* src = (const Vtx*)seg_addr(cmd->words.w1);
                const size_t count = C0(0, 16) / sizeof(Vtx);

                // a mesh's own vertices, which the mesh vouches for
                if (mesh_cur && gfx_mesh_load(src, count, C0(16, 4))) {
                    gfx_vertices_lost = false;
                    break;
                }

                if (count && !gfx_readable(src, count * sizeof(Vtx))) {
                    gfx_refuse_vertex_load(cmd, dListStart, src, count);
                    gfx_vertices_lost = true;
                    break;
                }

                gfx_vertices_lost = false;
                gfx_sp_vertex(count, C0(16, 4), src);

                if (mesh_cur) {
                    gfx_mesh_cpu_slots(C0(16, 4), count);
                }
                break;
            }
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    Gfx* subGFX = (Gfx*)seg_addr(cmd->words.w1);

                    if (subGFX != nullptr) {
                        if (gfx_dl_depth <= GFX_DL_TRACK) {
                            gfx_dl_calls[gfx_dl_depth - 1] = cmd;
                        }

                        gfx_run_dl(subGFX);
                    }
                } else {
                    cmd = (Gfx*)seg_addr(cmd->words.w1);

                    if (gfx_dl_depth <= GFX_DL_TRACK) {
                        gfx_dl_lists[gfx_dl_depth - 1] = cmd;
                    }

                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
            case G_EXTRAGEOMETRYMODE_EXT:
                gfx_sp_extra_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
            case (uint8_t)G_TRI1:
                if (!gfx_vertices_lost) {
                    gfx_seal_this = C1(24, 1);
                    gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10, false);
                    gfx_seal_this = false;
                }
                break;
            case (uint8_t)G_TRI4:
                if (!gfx_vertices_lost) {
                    gfx_sp_tri4(cmd);
                }
                break;
            case (uint8_t)G_SETOTHERMODE_L:
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
                break;
            case (uint8_t)G_SETOTHERMODE_H:
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t)cmd->words.w1 << 32);
                break;
            case G_COL:
                gfx_sp_set_vertex_colors(C0(0, 16) / 4, (NormalColor *)seg_addr(cmd->words.w1));
                break;

            // RDP Commands:
            case G_SETTIMG: {
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), 0, seg_addr(cmd->words.w1));
                break;
            }
            case G_SETTIMG_FB_EXT:
                gfx_flush_for(GFX_FLUSH_OTHER);
                gfx_rapi->select_texture_fb(cmd->words.w1);
                rdp.textures_changed[0] = false;
                rdp.textures_changed[1] = false;
                gfx_mark_state_dirty();
                break;
            case G_SETGRAYSCALE_EXT:
                rdp.grayscale = cmd->words.w1;
                gfx_mark_state_dirty();
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4),
                                C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C0(14, 10), C0(2, 10), C1(14, 10), C1(2, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C0(8, 8), C0(0, 8), C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETINTENSITY_EXT:
                gfx_dp_set_grayscale_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                                        alpha_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
                                        color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                                        alpha_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_SETTEXGENSHIFT_EXT:
                rsp.texgen_shift[0] = (int16_t)C0(0, 16) / 16384.0f;
                rsp.texgen_shift[1] = (int16_t)C1(0, 16) / 16384.0f;
                // the same shift read as a fraction of a turn, for G_TEXGEN_TURN_EXT
                rsp.texgen_turn[0] = cosf(rsp.texgen_shift[0] * 6.2831853f);
                rsp.texgen_turn[1] = sinf(rsp.texgen_shift[0] * 6.2831853f);
                rsp.texgen_turn[2] = cosf(rsp.texgen_shift[1] * 6.2831853f);
                rsp.texgen_turn[3] = sinf(rsp.texgen_shift[1] * 6.2831853f);
                break;
            case G_SETRECTDEPTH_EXT:
                rdp.rect_depth_on = C0(0, 1) != 0;
                rdp.rect_depth = (int32_t)(uint32_t)cmd->words.w1 / 1073741824.0f;
                break;
            case G_SETDEPTHBIAS_EXT:
                rdp.depth_bias = (int16_t)(int32_t)cmd->words.w1;
                break;
            case G_SETTILEOFFSET_EXT:
                // A fraction of a texel past the tile's corner, until its size
                // is set again: a picture moved by less than a quarter texel
                // at a time (gewater.c)
                gfx_mark_state_dirty();
                rdp.texture_tile[C0(0, 3)].ofs_s = (uint16_t)(cmd->words.w1 >> 16) / 65536.0f;
                rdp.texture_tile[C0(0, 3)].ofs_t = (uint16_t)(cmd->words.w1 & 0xffff) / 65536.0f;
                break;
            case G_OCCLUSIONTEST_EXT: {
                const int slot = C0(0, 16);
                const float z = (int32_t)(uint32_t)cmd->words.w1 / 1073741824.0f;
                ++cmd;
                gfx_occlusion_test(slot, (int32_t)cmd->words.w0, (int32_t)cmd->words.w1, z);
                break;
            }
            case G_SETFOGLINE_EXT: {
                union {
                    uint32_t u;
                    float f;
                } line;

                line.u = (uint32_t)cmd->words.w1;

                if (C0(0, 1)) {
                    rsp.fog_offset = line.f;
                } else {
                    rsp.fog_mul = line.f;
                }

                if (rsp.fog_linear != (C0(1, 1) != 0)) {
                    rsp.fog_linear = C0(1, 1) != 0;
                    gfx_mark_state_dirty();
                }
                break;
            }
            case G_SETSUBPIXELOFFSET_EXT: {
                gfx_dp_set_subpixel_offset(C0(0, 16), C1(0, 16));
                break;
            }
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
            case G_FILLRECT_WIDE_EXT: {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
            case G_TEXRECT_WIDE_EXT: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                bool flip;
                lrx = (int32_t)((C0(0, 24) << 8)) >> 8;
                lry = (int32_t)((C1(0, 24) << 8)) >> 8;
                tile = C1(24, 3);
                flip = C1(27, 1);
                ++cmd;
                ulx = (int32_t)((C0(0, 24) << 8)) >> 8;
                uly = (int32_t)((C1(0, 24) << 8)) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip);
                break;
            }
            case G_IMAGERECT_EXT: {
                int16_t tile, iw, ih;
                int16_t x0, y0, s0, t0;
                int16_t x1, y1, s1, t1;
                tile = C0(0, 3);
                iw = C1(16, 16);
                ih = C1(0, 16);
                ++cmd;
                x0 = C0(16, 16);
                y0 = C0(0, 16);
                s0 = C1(16, 16);
                t0 = C1(0, 16);
                ++cmd;
                x1 = C0(16, 16);
                y1 = C0(0, 16);
                s1 = C1(16, 16);
                t1 = C1(0, 16);
                gfx_dp_image_rectangle(tile, iw, ih, x0, y0, s0, t0, x1, y1, s1, t1);
                break;
            }
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
            case G_SETFB_EXT:
                gfx_flush_for(GFX_FLUSH_OTHER);
                if (cmd->words.w1) {
                    // don't care about noise here
                    gfx_set_framebuffer(cmd->words.w1, 1.f);
                    fbActive = true;
                } else {
                    gfx_reset_framebuffer();
                    fbActive = false;
                }
                break;
            case G_SETFONTGLYPH_EXT:
                // Kept whole rather than unpacked: it is only ever compared and
                // handed to texpack, which takes it apart. Bit 31 marks it set,
                // so a real glyph 0 of font 0 is still non-zero.
                rdp.pending_glyph = 0x80000000u | cmd->words.w1;
                break;
            case G_COPYFB_EXT:
                gfx_copy_framebuffer(C0(11, 11), C0(0, 11), (int16_t)C1(16, 16), (int16_t)C1(0, 16), C0(22, 1));
                break;
            case G_RDPSETOTHERMODE:
                gfx_dp_set_other_mode(C0(0, 24), cmd->words.w1);
                break;
            case G_INVALTEXCACHE_EXT:
                if (cmd->words.w1) {
                    gfx_texture_cache_delete((const uint8_t *)seg_addr(cmd->words.w1));
                } else {
                    gfx_texture_cache_clear();
                }
                break;
            case (uint8_t)G_RDPHALF_1:
            case (uint8_t)G_RDPHALF_2:
            case (uint8_t)G_RDPHALF_CONT:
                // on N64 skyRender uses these to render some types of skies and skybox water
                // by issuing low-level ucode commands G_TRI_FILL and G_TRI_SHADE_TXTR
                // the port renders the sky in a different manner
                break;
            case G_RDPFLUSH_EXT:
                gfx_flush_for(GFX_FLUSH_OTHER);
                break;
            case G_TAA_EXT:
                gfx_taa_marker(C0(8, 1) != 0, C0(0, 8),
                               cmd->words.w1 ? (const float *)seg_addr(cmd->words.w1) : NULL);
                break;
            case G_CLEAR_DEPTH_EXT:
                gfx_flush_for(GFX_FLUSH_OTHER);
                gfx_rapi->clear_framebuffer(false, true);
                break;
            case G_MESH_EXT:
                gfx_mesh_begin((const struct gfxmeshdraw*)cmd->words.w1);
                break;
            case G_RDPPIPESYNC:
            case G_RDPFULLSYNC:
            case G_RDPLOADSYNC:
            case G_RDPTILESYNC:
                break;
            default:
                gfx_unknown_opcode(cmd, gfx_dl_depth <= GFX_DL_TRACK ? gfx_dl_lists[gfx_dl_depth - 1] : dListStart, opcode);
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

extern "C" void gfx_get_dimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    gfx_wapi->get_dimensions(width, height, posX, posY);
}

extern "C" void gfx_init(const GfxInitSettings *settings) {
    gfx_wapi = settings->wapi;
    gfx_rapi = settings->rapi;
    gfx_wapi->init(&settings->window_settings);
    gfx_rapi->init();
    gfx_texture_cache_limits();
    gfx_rapi->update_framebuffer_parameters(0, settings->window_settings.width, settings->window_settings.height, 1, false, true, true, true);
    gfx_current_dimensions.internal_mul = 1;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width = settings->window_settings.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height = settings->window_settings.height;
    game_framebuffer = gfx_rapi->create_framebuffer();
    game_framebuffer_msaa_resolved = gfx_rapi->create_framebuffer();

    if (gfx_msaa_level > 1 && !gfx_framebuffers_enabled) {
        sysLogPrintf(LOG_WARNING, "F3D: MSAA set to %d, but framebuffers are not available; disabling", gfx_msaa_level);
        gfx_msaa_level = 1;
    }

    gfx_max_msaa_level = gfx_rapi->get_max_msaa_level ? (uint32_t)gfx_rapi->get_max_msaa_level() : 1;
    if (gfx_msaa_level > gfx_max_msaa_level) {
        sysLogPrintf(LOG_WARNING, "F3D: MSAA set to %d, but the GPU offers at most %d; using that",
                     gfx_msaa_level, gfx_max_msaa_level);
        gfx_msaa_level = gfx_max_msaa_level;
    }

    for (int i = 0; i < 16; i++) {
        segmentPointers[i] = 0;
    }

    if (tex_upload_buffer == nullptr) {
        // We cap texture max to 8k, because why would you need more?
        int max_tex_size = std::min(8192, gfx_rapi->get_max_texture_size());
        tex_upload_buffer = (uint8_t*)malloc(max_tex_size * max_tex_size * 4);
    }

    rsp.lookat[0].dir[0] = rsp.lookat[1].dir[1] = 0x7F;
    rsp.current_lookat_coeffs[0][0] = rsp.current_lookat_coeffs[1][1] = 1.f;
    rsp.lookat_enabled = true;
}

extern "C" void gfx_destroy(void) {
    // TODO: should also destroy rapi and wapi, and any other resources acquired in fast3d

    // Texture cache and loaded textures store references to Resources which need to be unreferenced.
    gfx_texture_cache_clear();
}

extern "C" struct GfxRenderingAPI* gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

extern "C" void gfx_start_frame(void) {
    // Replacements that finished decoding while the last frame was drawn. Done
    // here so a frame never both evicts and re-uploads the same texture.
    gfx_texpack_poll();
    gfx_texture_cache_frame();

    // Report and clear what the frame just finished cost, before anything is
    // added to the totals for the next one.
    if (g_GfxLogStats) {
        static uint32_t framessincelog = 0;

        if (++framessincelog >= g_GfxLogStats) {
            framessincelog = 0;
            sysLogPrintf(LOG_NOTE,
                    "gfx: %u draws, %u tris, %u verts, %.1f tris/draw",
                    g_GfxNumDrawCalls,
                    g_GfxNumTris,
                    g_GfxNumVerts,
                    g_GfxNumDrawCalls ? (float)g_GfxNumTris / g_GfxNumDrawCalls : 0.0f);
            sysLogPrintf(LOG_NOTE,
                    "gfx:   tex %u (%u distinct), shader %u, blend %u, sampler %u, depth %u, viewport %u, bufferfull %u, other %u",
                    g_GfxFlushReasons[GFX_FLUSH_TEXTURE],
                    g_GfxNumDistinctTextures,
                    g_GfxFlushReasons[GFX_FLUSH_SHADER],
                    g_GfxFlushReasons[GFX_FLUSH_BLEND],
                    g_GfxFlushReasons[GFX_FLUSH_SAMPLER],
                    g_GfxFlushReasons[GFX_FLUSH_DEPTH],
                    g_GfxFlushReasons[GFX_FLUSH_VIEWPORT],
                    g_GfxFlushReasons[GFX_FLUSH_BUFFERFULL],
                    g_GfxFlushReasons[GFX_FLUSH_OTHER]);
            sysLogPrintf(LOG_NOTE,
                    "gfx:   tris clipped %u, culled %u, drawn %u",
                    g_GfxTrisClipped, g_GfxTrisCulled, g_GfxNumTris);
            if (g_GfxRoomDraws || g_GfxRoomRefused || g_GfxRoomCpuTris || g_GfxRoomSealedTris) {
                sysLogPrintf(LOG_NOTE, "gfx:   gpu rooms: %u draws, %u tris (%u tris refused to the cpu); cpu %u tris, %u sealed",
                        g_GfxRoomDraws, g_GfxRoomTris, g_GfxRoomRefused, g_GfxRoomCpuTris, g_GfxRoomSealedTris);
            }
            if (g_GfxMeshDraws || g_GfxMeshRefused) {
                sysLogPrintf(LOG_NOTE, "gfx:   gpu meshes: %u draws, %u tris (%u tris refused to the cpu)",
                        g_GfxMeshDraws, g_GfxMeshTris, g_GfxMeshRefused);
            }
            sysLogPrintf(LOG_NOTE,
                    "gfx:   tex uploads %u, evictions %u, cache %u/%u (peak %u, grew %u times, %u evicted in all, %u MB)",
                    g_GfxNumTexUploads,
                    g_GfxNumTexEvictions,
                    (uint32_t)gfx_texture_cache.map.size(),
                    g_GfxTexCacheSize,
                    gfx_texture_cache.peak,
                    gfx_texture_cache.grows,
                    gfx_texture_cache.evictions,
                    (uint32_t)(gfx_texture_cache.bytes >> 20));
        }
    }

    if (g_GfxMaxBufferedTris < 1) {
        g_GfxMaxBufferedTris = 1;
    } else if (g_GfxMaxBufferedTris > MAX_BUFFERED) {
        g_GfxMaxBufferedTris = MAX_BUFFERED;
    }

    // Kept for the F3 trace dump, which asks mid-frame about the last one.
    g_GfxLastFrame.drawcalls = g_GfxNumDrawCalls;
    g_GfxLastFrame.tris = g_GfxNumTris;
    g_GfxLastFrame.verts = g_GfxNumVerts;
    g_GfxLastFrame.distincttextures = g_GfxNumDistinctTextures;
    g_GfxLastFrame.texuploads = g_GfxNumTexUploads;
    g_GfxLastFrame.texevictions = g_GfxNumTexEvictions;
    g_GfxLastFrame.bufferfullflushes = g_GfxNumBufferFullFlushes;

    g_GfxNumDrawCalls = 0;
    g_GfxNumBufferFullFlushes = 0;
    g_GfxNumTris = 0;
    g_GfxNumVerts = 0;
    g_GfxTrisClipped = g_GfxTrisCulled = 0;
    g_GfxMeshDraws = g_GfxMeshTris = g_GfxMeshRefused = 0;
    g_GfxRoomDraws = g_GfxRoomTris = g_GfxRoomRefused = g_GfxRoomCpuTris = g_GfxRoomSealedTris = 0;
    memset(g_GfxFlushReasons, 0, sizeof(g_GfxFlushReasons));
    gfx_frame_textures.clear();
    g_GfxNumDistinctTextures = 0;
    g_GfxNumTexUploads = 0;
    g_GfxNumTexEvictions = 0;

    gfx_wapi->handle_events();
    gfx_wapi->get_dimensions(&gfx_current_window_dimensions.width, &gfx_current_window_dimensions.height,
                             &gfx_current_window_position_x, &gfx_current_window_position_y);

    if (gfx_current_window_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_window_dimensions.height = 1;
    }

    gfx_current_window_dimensions.aspect_ratio = (float)gfx_current_window_dimensions.width / gfx_current_window_dimensions.height;

    gfx_current_dimensions = gfx_current_window_dimensions;

    // The render scale: FSR draws the game at a fraction of the window,
    // supersampling at a multiple of it, and the window keeps its aspect ratio
    // as the game's. A multiple stops at GFX_MAX_RENDER_SIDE on the longer
    // side - 2x of a 4K window fits, 2x of a 5K one would need a 10240-wide
    // target, and at 8x MSAA the colour and depth alone run to gigabytes.
    if (gfx_framebuffers_enabled && gfx_render_scale != 1.0f) {
        float scale = std::min(std::max(gfx_render_scale, 0.25f), 2.0f);
        const uint32_t side = std::max(gfx_current_window_dimensions.width, gfx_current_window_dimensions.height);
        if (scale > 1.0f && side * scale > GFX_MAX_RENDER_SIDE) {
            scale = std::max(1.0f, (float)GFX_MAX_RENDER_SIDE / side);
        }
        gfx_current_dimensions.width = std::max(1u, (uint32_t)(gfx_current_window_dimensions.width * scale + 0.5f));
        gfx_current_dimensions.height = std::max(1u, (uint32_t)(gfx_current_window_dimensions.height * scale + 0.5f));
    }

    gfx_current_game_window_viewport.width = gfx_current_window_dimensions.width;
    gfx_current_game_window_viewport.height = gfx_current_window_dimensions.height;

    if (gfx_current_dimensions.width != gfx_prev_dimensions.width ||
        gfx_current_dimensions.height != gfx_prev_dimensions.height) {
        for (auto& fb : framebuffers) {
            uint32_t width, height, msaa;
            if (fb.second.autoresize) {
                if (fb.second.upscale) {
                    width = fb.second.orig_width;
                    height = fb.second.orig_height;
                    gfx_adjust_width_height_for_scale(width, height);
                } else {
                    // assume this is a fullscreen fb
                    width = gfx_current_dimensions.width;
                    height = gfx_current_dimensions.height;
                }
                if (width != fb.second.applied_width || height != fb.second.applied_height) {
                    gfx_rapi->update_framebuffer_parameters(fb.first, width, height, 1, true, true, true, true);
                    fb.second.applied_width = width;
                    fb.second.applied_height = height;
                }
            }
        }
    }
    gfx_prev_dimensions = gfx_current_dimensions;

    // The menu sets gfx_msaa_level mid-run; the GPU's limit still applies.
    if (gfx_msaa_level > gfx_max_msaa_level) {
        sysLogPrintf(LOG_WARNING, "F3D: MSAA set to %d, but the GPU offers at most %d; using that",
                     gfx_msaa_level, gfx_max_msaa_level);
        gfx_msaa_level = gfx_max_msaa_level;
    }

    // SMAA or a render scale: the frame goes through the backend's post
    // chain on its way to the window, from a single-sampled framebuffer
    game_post_processes = gfx_framebuffers_enabled && gfx_rapi->post_process &&
                          (gfx_post_smaa || gfx_taa || gfx_current_dimensions.width != gfx_current_window_dimensions.width ||
                           gfx_current_dimensions.height != gfx_current_window_dimensions.height);
    if (gfx_framebuffers_enabled && (game_post_processes || gfx_msaa_level > 1)) {
        game_renders_to_framebuffer = true;
        // Laid out as the window is (invert_y off), so nothing between the
        // two is turned over
        gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_dimensions.width,
                                                gfx_current_dimensions.height, gfx_msaa_level, false, true, true,
                                                true);
        if (gfx_msaa_level > 1 && game_post_processes) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer_msaa_resolved, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, 1, false, true, false, false);
        }
    } else {
        game_renders_to_framebuffer = false;
    }

    fbActive = 0;

    // update aspect scale and offset
    gfx_update_aspect_mode();
}

uint32_t num_dls = 0;

extern "C" void gfx_run(Gfx* commands) {
    ++num_dls;
    gfx_sp_reset();

    // puts("New frame");

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;

    gfx_rapi->update_framebuffer_parameters(0, gfx_current_window_dimensions.width,
                                            gfx_current_window_dimensions.height, 1, false, true, true,
                                            !game_renders_to_framebuffer);
    gfx_rapi->start_frame();
    gfx_rapi->start_draw_to_framebuffer(game_renders_to_framebuffer ? game_framebuffer : 0,
                                        (float)gfx_current_dimensions.height / SCREEN_HEIGHT);
    gfx_rapi->clear_framebuffer(true, false);
    rdp.viewport_or_scissor_changed = true;
    rendering_state.viewport = {};
    rendering_state.scissor = {};
    gfx_mark_state_dirty();
    gfx_readable_reset();
    gfx_mesh_start_frame();
    gfx_run_dl(commands);
    gfx_mesh_flush();
    mesh_cur = NULL;
    gfx_flush_for(GFX_FLUSH_OTHER);
    gfxFramebuffer = 0;

    if (game_renders_to_framebuffer) {
        gfx_rapi->start_draw_to_framebuffer(0, 1);
        gfx_rapi->clear_framebuffer(true, true);

        if (game_post_processes) {
            int src = game_framebuffer;
            if (gfx_msaa_level > 1) {
                gfx_rapi->resolve_msaa_color_buffer(game_framebuffer_msaa_resolved, game_framebuffer);
                src = game_framebuffer_msaa_resolved;
            }
            if (!gfx_rapi->post_process(src, gfx_post_smaa, gfx_render_scale < 1.0f, gfx_fsr_sharpness)) {
                // a plain scaled copy rather than nothing
                gfx_rapi->resolve_msaa_color_buffer(0, src);
            }
            gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(src);
        } else if (gfx_msaa_level > 1) {
            gfx_rapi->resolve_msaa_color_buffer(0, game_framebuffer);
        } else {
            gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer);
        }
    }

    gfx_rapi->end_frame();

    // Last chance to read the frame back: swap_buffers_begin() presents it and
    // leaves the back buffer undefined.
    if (gfx_pre_swap_callback) {
        gfx_pre_swap_callback();
    }

    gfx_wapi->swap_buffers_begin();
}

extern "C" void gfx_set_pre_swap_callback(GfxPreSwapCallback cb) {
    gfx_pre_swap_callback = cb;
}

extern "C" bool gfx_read_screen_pixels(int x, int y, int width, int height, void *rgb) {
    if (!gfx_rapi || !gfx_rapi->read_screen_pixels) {
        return false;
    }
    return gfx_rapi->read_screen_pixels(x, y, width, height, rgb);
}

extern "C" int gfx_capture_start(int width, int height) {
    if (!gfx_rapi || !gfx_rapi->capture_start) {
        return GFX_CAPTURE_NONE;
    }
    return gfx_rapi->capture_start(width, height);
}

extern "C" bool gfx_capture_read(void *dst) {
    if (!gfx_rapi || !gfx_rapi->capture_read) {
        return false;
    }
    return gfx_rapi->capture_read(dst);
}

extern "C" bool gfx_capture_drain(void *dst) {
    if (!gfx_rapi || !gfx_rapi->capture_drain) {
        return false;
    }
    return gfx_rapi->capture_drain(dst);
}

extern "C" void gfx_capture_stop(void) {
    if (gfx_rapi && gfx_rapi->capture_stop) {
        gfx_rapi->capture_stop();
    }
}

extern "C" bool gfx_occlusion_supported(void) {
    return gfx_rapi && gfx_rapi->occlusion_begin && gfx_rapi->occlusion_end && gfx_rapi->occlusion_result &&
           !gfx_occlusion_failed;
}

extern "C" int gfx_occlusion_result(int slot) {
    if (!gfx_occlusion_supported() || slot < 0 || slot >= GFX_OCCLUSION_SLOTS || !gfx_occlusion_issued[slot]) {
        return -1;
    }
    gfx_occlusion_issued[slot] = false;
    return gfx_rapi->occlusion_result(slot);
}

extern "C" void gfx_end_frame(void) {
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
}

extern "C" void gfx_set_target_fps(int fps) {
    gfx_wapi->set_target_fps(fps);
}

extern "C" void reset_texture_state() {
    gfx_texture_cache_clear();
    // The pool is about to go, and batch.comb points into it.
    batch.comb = nullptr;
    gfx_mark_state_dirty();
    if (rendering_state.shader_program) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        rendering_state.shader_program = nullptr;
    }
    gfx_rapi->clear_shaders();
    color_combiner_pool.clear();
    prev_combiner = color_combiner_pool.end();
}

extern "C" void gfx_set_clamped_edge_mode(int mode) {
    if (mode < CLAMPED_EDGE_STRETCH || mode > CLAMPED_EDGE_REPEAT || mode == gfx_clamped_edge_mode) {
        return;
    }
    gfx_clamped_edge_mode = mode;
    // The sampler's wrap mode is cached per texture and the tile-smaller-than-
    // upload case is baked into the shader, so both have to go.
    reset_texture_state();
}

extern "C" void gfx_set_texture_enhance(int texture_scale, int text_scale) {
    texture_scale = texture_scale < 1 ? 1 : texture_scale;
    text_scale = text_scale < 1 ? 1 : text_scale;
    if (texture_scale == gfx_texture_enhance_scale && text_scale == gfx_text_smooth_scale) {
        return;
    }
    gfx_texture_enhance_scale = texture_scale;
    gfx_text_smooth_scale = text_scale;
    // Everything cached was uploaded at the old size
    gfx_texture_cache_clear();
}

extern "C" void gfx_set_texture_filter(enum FilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_texture_filter(mode);
}

extern "C" void gfx_set_mipmap_filter(enum MipmapFilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_mipmap_filter(mode);
}

extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize) {
    int fb = gfx_rapi->create_framebuffer();
    gfx_resize_framebuffer(fb, width, height, upscale, autoresize);
    return fb;
}

extern "C" void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize) {
    uint32_t orig_width, orig_height;

    if (width && height) {
        // user-specified size
        orig_width = width;
        orig_height = height;
        if (upscale) {
            gfx_adjust_width_height_for_scale(width, height);
        }
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    } else {
        // same size as main fb
        orig_width = width = gfx_current_dimensions.width;
        orig_height = height = gfx_current_dimensions.height;
        upscale = false;
        autoresize = true;
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    }

    framebuffers[fb] = { orig_width, orig_height, width, height, (bool)upscale, (bool)autoresize };
}

extern "C" void gfx_set_framebuffer(int fb, float noise_scale) {
    gfx_rapi->start_draw_to_framebuffer(fb, noise_scale);
    gfx_rapi->clear_framebuffer(true, true);
    active_fb = framebuffers.find(fb);
}

extern "C" void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back) {
    const bool is_main_fb = (fb_src == 0);

    if (is_main_fb) {
        if (left > 0 && top > 0) {
            // upscale the position
            left = left * gfx_current_dimensions.width / gfx_current_native_viewport.width;
            top = top * gfx_current_dimensions.height / gfx_current_native_viewport.height;
            // flip Y
            top = gfx_current_dimensions.height - top - 1;
        }
        if (use_back && game_renders_to_framebuffer) {
            // read from the framebuffer we've been rendering to
            fb_src = game_framebuffer;
        }
    }

    gfx_rapi->copy_framebuffer(fb_dst, fb_src, left, top, is_main_fb, (bool)use_back);
}

extern "C" void gfx_reset_framebuffer(void) {
    gfx_rapi->start_draw_to_framebuffer(0, (float)gfx_current_dimensions.height / SCREEN_HEIGHT);
    active_fb = framebuffers.end();
}

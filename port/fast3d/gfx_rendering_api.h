#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct ShaderProgram;

struct GfxClipParameters {
    bool z_is_from_0_to_1;
    bool invert_y;
};

/*
 * A mesh kept on the GPU (G_MESH_EXT, gfxmesh.h): one vertex of its static
 * copy. pos is the bind pose (or the vertex as the lists hold it, for a rigid
 * mesh), st the Vtx's raw s and t, bones three palette entries and how many of
 * them count, weights theirs, normal the bind pose's normal (unit length, or
 * zero for a mesh without them), posed with the vertex where a skinned mesh
 * is lit or reflects.
 */
struct GfxMeshVertex {
    float pos[3];
    int16_t st[2];
    uint8_t bones[4];
    float weights[3];
    float normal[3];
};

/*
 * The vec4s of a mesh draw's parameters (gfx_mesh_params() in gfx_pc.cpp
 * fills them; each backend's mesh vertex shader reads them):
 *  0-3   the RSP's modelview-projection, rows (clip = x*r0 + y*r1 + z*r2 + r3)
 *  4     aspect: offset, scale, window ratio, 1 to apply (x = (ofs*w + x) * scale / ratio)
 *  5     TAA jitter x and y (times w), y sign, 1 when z is to be taken to 0..1
 *  6     fog line multiplier and offset, 1 for per-vertex fog, 0
 *  7     fog colour
 *  8, 9  textures 0 and 1: s scale, t scale, s offset, t offset
 *  10    the clamps: texture 0 s and t, texture 1 s and t
 *  11    grayscale colour
 *  12    the G_TEXTURE scale s and t, 1 when skinned, 1 when a room's colours are the
 *        palette's entries (as 0..255) and each vertex's bones x, y and z its index there
 *  13-20 combiner inputs 1-8: constant colour and alpha
 *  21-22 their colour kinds, 23-24 their alpha kinds (GFX_MESH_IN_*)
 *  25    directional lights, 1 for G_LIGHTING, 1 for G_TEXTURE_GEN, 1 for G_TEXTURE_GEN_LINEAR
 *  26    1 for G_TEXGEN_EYE_EXT, G_TEXGEN_TURN_EXT, a LookAt, G_ENVMAP_EXT
 *  27    1 to force a lit vertex's alpha to 255, 1 when the normal is the mesh's own posed, 0, 0
 *  28    the ambient light's colour (0..255)
 *  29-32 the lights' directions in model space (gfx_light_vertex()'s coefficients)
 *  33-36 their colours (0..255)
 *  37-38 the LookAt's x and y coefficients
 *  39    G_SETTEXGENSHIFT_EXT's shift, 0, 0
 *  40    its turn: cos and sin of the yaw, of the pitch
 *  41-44 the modelview, rows
 */
#define GFX_MESH_PARAMS 45
#define GFX_MESH_PALETTE_MAX 64
enum { GFX_MESH_IN_CONST, GFX_MESH_IN_SHADE, GFX_MESH_IN_SHADE_ALPHA, GFX_MESH_IN_LOD };

struct GfxMeshDraw {
    struct ShaderProgram* prg;   // the program the renderer has bound; drawn under its mesh variant
    uint32_t mesh;               // from mesh_create()
    const uint8_t* colours;      // four bytes per vertex of the mesh; NULL for a room whose colours are the palette's
    uint32_t numcolours;
    const uint32_t* indices;     // into the mesh's vertices, three a triangle; NULL for the mesh's own (first_index)
    uint32_t numindices;
    uint32_t first_index;        // where they start among those mesh_add_indices() kept
    const float* params;         // GFX_MESH_PARAMS vec4s
    const float* palette;        // numpalette entries of 12 floats, or NULL
    uint32_t numpalette;
    int8_t cull;                 // drop triangles wound 1 clockwise, -1 anticlockwise, 0 neither, as emitted
};

enum FilteringMode { FILTER_NONE, FILTER_LINEAR, FILTER_THREE_POINT };
enum MipmapFilteringMode { MIPMAP_DISABLED, MIPMAP_NEAREST, MIPMAP_LINEAR };

struct GfxRenderingAPI {
    const char* (*get_name)(void);
    int (*get_max_texture_size)(void);
    struct GfxClipParameters (*get_clip_parameters)(void);
    void (*unload_shader)(struct ShaderProgram* old_prg);
    void (*load_shader)(struct ShaderProgram* new_prg);
    struct ShaderProgram* (*create_and_load_new_shader)(uint64_t shader_id0, uint32_t shader_id1);
    struct ShaderProgram* (*lookup_shader)(uint64_t shader_id0, uint32_t shader_id1);
    void (*shader_get_info)(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]);
    void (*clear_shaders)(void);
    uint32_t (*new_texture)(void);
    void (*select_texture)(int tile, uint32_t texture_id, bool linear_filter);
    void (*upload_texture)(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps);
    void (*set_sampler_parameters)(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps);
    void (*set_depth_mode)(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode, int16_t depth_bias);
    void (*set_depth_range)(float znear, float zfar);
    void (*set_viewport)(int x, int y, int width, int height);
    void (*set_scissor)(int x, int y, int width, int height);
    void (*set_use_alpha)(bool use_alpha, bool modulate, bool additive);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    void (*init)(void);
    void (*on_resize)(void);
    void (*start_frame)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
    int (*create_framebuffer)();
    void (*update_framebuffer_parameters)(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                          bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                          bool can_extract_depth);
    bool (*start_draw_to_framebuffer)(int fb_id, float noise_scale);
    void (*copy_framebuffer)(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back);
    void (*clear_framebuffer)(bool clear_color, bool clear_depth);
    void (*resolve_msaa_color_buffer)(int fb_id_target, int fb_id_source);
    void* (*get_framebuffer_texture_id)(int fb_id);
    void (*select_texture_fb)(int fb_id);
    void (*delete_texture)(uint32_t texID);
    void (*set_texture_filter)(enum FilteringMode mode);
    enum FilteringMode (*get_texture_filter)(void);
    void (*set_mipmap_filter)(enum MipmapFilteringMode mode);
	void (*set_anisotropy_level)(int);
	int (*get_max_anisotropy_level)(void);
    // Most samples a multisampled render target may have: GL_MAX_SAMPLES,
    // rounded down to a power of two, 1 when framebuffers are off.
    int (*get_max_msaa_level)(void);
    // Draws the finished frame in fb_src (single-sampled, any size) into the
    // window, fb 0: SMAA at fb_src's size when smaa, then up to the window's
    // size by FSR 1 when fsr and the sizes differ, else bilinear. False when
    // the backend could not, which leaves the window untouched.
    bool (*post_process)(int fb_src, bool smaa, bool fsr, float sharpness);
    // TAA's resolve (gfx_post.h, GFX_POST_TAA) over the rect of fb, a
    // single-sampled target mid-frame: fb's depth copied out, the pass drawn
    // into history image `out` from fb's colour, history 1 - out and that
    // depth, then the rect copied back into fb. params are uTaa's 20 floats.
    // False when the backend could not, which leaves fb untouched.
    bool (*taa_resolve)(int fb, const float *params, int out, int x, int y, int width, int height);
    // Reads a rect of the window's back buffer into rgb as tightly packed RGB
    // triples, bottom row first. Only valid before the frame is presented.
    bool (*read_screen_pixels)(int x, int y, int width, int height, void *rgb);
    // Streaming capture of the back buffer, one frame behind and without a
    // stall. See gfx_capture_start() in gfx_api.h. May be null.
    int (*capture_start)(int width, int height);
    bool (*capture_read)(void *dst);
    bool (*capture_drain)(void *dst);
    void (*capture_stop)(void);
    // Occlusion queries, for the light glares and the sun (gfx_pc.cpp,
    // G_OCCLUSIONTEST_EXT): what is drawn between occlusion_begin() and
    // occlusion_end() counts its samples that pass the depth test into query
    // slot (below GFX_OCCLUSION_SLOTS). occlusion_begin() is false when the
    // backend cannot, and then occlusion_end() is not called.
    // occlusion_result() gives the count of a slot ended in an earlier frame,
    // waiting for that frame if the GPU has not finished it, or -1 if the
    // slot has nothing to give. Any may be null.
    bool (*occlusion_begin)(int slot);
    void (*occlusion_end)(int slot);
    int (*occlusion_result)(int slot);
    // What the texture cache may grow to: the most textures the backend can
    // hold at once, and the card's video memory in bytes, 0 when it cannot
    // tell. May be null.
    void (*get_texture_limits)(uint32_t* max_textures, uint64_t* vram_bytes);
    // Meshes kept on the GPU (G_MESH_EXT). mesh_supported() says whether the
    // backend can draw them at all; mesh_create() copies a mesh's vertices
    // and gives a name for them (0 when it could not), mesh_delete() lets one
    // go, and mesh_draw() draws triangles of one under draw->prg's mesh
    // variant and the state the renderer last set - false when it could not,
    // and then it has drawn nothing. Any may be null.
    // mesh_add_indices() keeps triangles of a mesh with it for good, for a
    // draw to name by where they start (UINT32_MAX when it could not).
    bool (*mesh_supported)(void);
    uint32_t (*mesh_create)(const struct GfxMeshVertex* verts, uint32_t count);
    void (*mesh_delete)(uint32_t mesh);
    bool (*mesh_draw)(const struct GfxMeshDraw* draw);
    uint32_t (*mesh_add_indices)(uint32_t mesh, const uint32_t* indices, uint32_t count);
    // mesh_update() rewrites vertices of a mesh from first: only ever ones no
    // draw has read yet (a room's, as its runs teach their colours), so a
    // frame still in flight never sees the change.
    void (*mesh_update)(uint32_t mesh, uint32_t first, const struct GfxMeshVertex* verts, uint32_t count);
};

// Occlusion query slots a backend provides
#define GFX_OCCLUSION_SLOTS 512

#endif
